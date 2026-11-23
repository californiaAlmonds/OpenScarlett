#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace openscarlett {

namespace {

bool IsValidVertexMtbufFormat(Rdna2MtbufFormat fmt) noexcept {
    return GetRdna2MtbufFormatInfo(fmt).is_valid;
}

Status ValidateVertexInputTable(const std::vector<VertexFetchEntry>& fetches) {
    constexpr uint32_t kMaxVertexBufferSlots = 16u;
    constexpr uint32_t kMaxVertexFetchByteOffset = 65536u;
    std::array<bool, 256> dst_vgpr_claimed{};

    for (size_t idx = 0u; idx < fetches.size(); ++idx) {
        const auto& vf = fetches[idx];
        if (!IsValidVertexMtbufFormat(vf.format)) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "Malformed vertex input table entry #" + std::to_string(idx) +
                    ": invalid or unsupported MTBUF format code " +
                    std::to_string(static_cast<uint32_t>(vf.format)));
        }
        if (vf.num_components == 0u || vf.num_components > 4u) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "Malformed vertex input table entry #" + std::to_string(idx) +
                    ": num_components=" + std::to_string(static_cast<uint32_t>(vf.num_components)) +
                    " out of valid range [1, 4]");
        }
        if (vf.vb_slot >= kMaxVertexBufferSlots) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "Malformed vertex input table entry #" + std::to_string(idx) +
                    ": vb_slot=" + std::to_string(vf.vb_slot) +
                    " exceeds maximum vertex buffer slot limit (" +
                    std::to_string(kMaxVertexBufferSlots) + ")");
        }
        if (vf.byte_offset > kMaxVertexFetchByteOffset) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "Malformed vertex input table entry #" + std::to_string(idx) +
                    ": corrupt byte_offset=" + std::to_string(vf.byte_offset) +
                    " exceeds maximum fetch offset (" +
                    std::to_string(kMaxVertexFetchByteOffset) + ")");
        }
        if (static_cast<uint32_t>(vf.dst_vgpr) + static_cast<uint32_t>(vf.num_components) > 256u) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "Malformed vertex input table entry #" + std::to_string(idx) +
                    ": dst_vgpr range v[" + std::to_string(static_cast<uint32_t>(vf.dst_vgpr)) +
                    ":" + std::to_string(static_cast<uint32_t>(vf.dst_vgpr) + vf.num_components - 1u) +
                    "] exceeds 256-VGPR register file");
        }
        for (uint8_t c = 0u; c < vf.num_components; ++c) {
            const uint32_t reg = static_cast<uint32_t>(vf.dst_vgpr) + c;
            if (dst_vgpr_claimed[reg]) {
                return Status::Error(
                    StatusCode::InvalidArgument,
                    "Malformed vertex input table entry #" + std::to_string(idx) +
                        ": overlapping destination VGPR v" + std::to_string(reg));
            }
            dst_vgpr_claimed[reg] = true;
        }
    }
    return Status::Ok();
}

Status ValidateMimgUnsupportedModifiersIr(const std::vector<Rdna2Instruction>& instructions) {
    for (const auto& inst : instructions) {
        if (inst.encoding != Rdna2Encoding::MIMG || inst.word_count == 0u) continue;
        const uint32_t w0 = inst.raw_words[0];
        const bool has_r128 = ((w0 >> 15) & 1u) != 0u;
        const bool has_tfe = ((w0 >> 6) & 1u) != 0u;
        const bool has_lwe = ((w0 >> 17) & 1u) != 0u;
        if (!has_r128 && !has_tfe && !has_lwe) continue;

        std::string mods;
        auto add_mod = [&](const char* name) {
            if (!mods.empty()) mods += ", ";
            mods += name;
        };
        if (has_r128) add_mod("R128");
        if (has_tfe) add_mod("TFE");
        if (has_lwe) add_mod("LWE");
        const std::string op_name = !inst.mnemonic.empty()
                                        ? inst.mnemonic
                                        : std::string(Rdna2OpcodeName(inst.opcode));
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Unsupported MIMG modifier(s) [" + mods + "] on opcode " + op_name +
                " at word_offset=" + std::to_string(inst.word_offset) +
                " (R128 4-dword T#, TFE residency return, and LWE LOD warning are refused)");
    }
    return Status::Ok();
}

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// IEEE-754 binary16 <-> float32 conversion helpers (round-to-nearest-even)
float HalfBitsToFloat(uint16_t h) noexcept {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t fbits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            fbits = sign;
        } else {
            uint32_t m = mant;
            uint32_t e = 113u;  // 127 - 14
            while ((m & 0x0400u) == 0u) {
                m <<= 1;
                --e;
            }
            m &= 0x03FFu;
            fbits = sign | (e << 23) | (m << 13);
        }
    } else if (exp == 31u) {
        fbits = sign | 0x7F800000u | (mant << 13);
    } else {
        fbits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    return BitsToFloat(fbits);
}

uint16_t FloatToHalfBits(float f) noexcept {
    const uint32_t bits = FloatToBits(f);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t mant = bits & 0x007FFFFFu;
    if (exp == 255u) {
        if (mant != 0u) return static_cast<uint16_t>(sign | 0x7E00u | (mant >> 13));
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    const int32_t new_exp = static_cast<int32_t>(exp) - 112;
    if (new_exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (new_exp <= 0) {
        if (new_exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - new_exp);
        const uint32_t rounded = (m + 0x00001000u + ((m >> 13) & 1u) - 1u) >> 13;
        return static_cast<uint16_t>(sign | rounded);
    }
    const uint32_t half_mant = mant + 0x00000FFFu + ((mant >> 13) & 1u);
    if ((half_mant & 0x00800000u) != 0u) {
        if (new_exp + 1 >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp + 1) << 10));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp) << 10) | (half_mant >> 13));
}

uint16_t SelectHalf16(uint32_t packed, bool hi) noexcept {
    return static_cast<uint16_t>(hi ? ((packed >> 16) & 0xFFFFu) : (packed & 0xFFFFu));
}

float FlushDenormF32(float x) noexcept {
    const uint32_t bits = FloatToBits(x);
    if ((bits & 0x7F800000u) == 0u) {
        return BitsToFloat(bits & 0x80000000u);
    }
    return x;
}

bool IsNanF32Bits(uint32_t bits) noexcept {
    return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}

float QuietNanF32(float x) noexcept {
    const uint32_t bits = FloatToBits(x);
    if (IsNanF32Bits(bits)) {
        return BitsToFloat(bits | 0x00400000u);
    }
    return x;
}

float ExactFloorF32(float x) noexcept {
    const uint32_t bits = FloatToBits(x);
    if (IsNanF32Bits(bits)) {
        return BitsToFloat(bits | 0x00400000u);
    }
    const bool is_denorm = ((bits & 0x7F800000u) == 0u) && ((bits & 0x007FFFFFu) != 0u);
    if (is_denorm) {
        return ((bits & 0x80000000u) != 0u) ? -1.0f : 0.0f;
    }
    return std::floor(x);
}

float ExactCeilF32(float x) noexcept {
    const uint32_t bits = FloatToBits(x);
    if (IsNanF32Bits(bits)) {
        return BitsToFloat(bits | 0x00400000u);
    }
    const bool is_denorm = ((bits & 0x7F800000u) == 0u) && ((bits & 0x007FFFFFu) != 0u);
    if (is_denorm) {
        return ((bits & 0x80000000u) != 0u) ? BitsToFloat(0x80000000u) : 1.0f;
    }
    return std::ceil(x);
}

float ExactMinF32(float a, float b) noexcept {
    const uint32_t ba = FloatToBits(a);
    const uint32_t bb = FloatToBits(b);
    const bool na = IsNanF32Bits(ba);
    const bool nb = IsNanF32Bits(bb);
    if (na && nb) return BitsToFloat(ba | 0x00400000u);
    if (na) return b;
    if (nb) return a;
    return std::fmin(a, b);
}

float ExactMaxF32(float a, float b) noexcept {
    const uint32_t ba = FloatToBits(a);
    const uint32_t bb = FloatToBits(b);
    const bool na = IsNanF32Bits(ba);
    const bool nb = IsNanF32Bits(bb);
    if (na && nb) return BitsToFloat(ba | 0x00400000u);
    if (na) return b;
    if (nb) return a;
    return std::fmax(a, b);
}

uint32_t EvalAlignByteB32(uint32_t hi, uint32_t lo, uint32_t byte_sel) noexcept {
    // Strictly UB-free 32-bit shift implementation: all shifts strictly < 32
    const uint32_t rshift = (byte_sel & 3u) << 3u;
    const uint32_t lo_part = lo >> rshift;
    const uint32_t hi_shift = (32u - rshift) & 31u;
    const uint32_t hi_part = (rshift != 0u) ? (hi << hi_shift) : 0u;
    return lo_part | hi_part;
}

float RoundNearestEvenF32(float x) noexcept {
    const float fl = std::floor(x);
    const float diff = x - fl;
    if (diff < 0.5f) return fl;
    if (diff > 0.5f) return fl + 1.0f;
    const float half_fl = fl * 0.5f;
    return (std::floor(half_fl) == half_fl) ? fl : (fl + 1.0f);
}

uint16_t CvtSnorm16F32(float x) noexcept {
    if (std::isnan(x)) return 0u;
    const float c = (x >= 1.0f) ? 1.0f : ((x <= -1.0f) ? -1.0f : x);
    const int32_t v = static_cast<int32_t>(RoundNearestEvenF32(c * 32767.0f));
    return static_cast<uint16_t>(static_cast<int16_t>(std::clamp(v, -32767, 32767)));
}

uint16_t CvtUnorm16F32(float x) noexcept {
    if (std::isnan(x)) return 0u;
    const float c = (x >= 1.0f) ? 1.0f : ((x <= 0.0f) ? 0.0f : x);
    const int32_t v = static_cast<int32_t>(RoundNearestEvenF32(c * 65535.0f));
    return static_cast<uint16_t>(std::clamp(v, 0, 65535));
}

uint8_t EncodeF16CmpPred(Rdna2Opcode op) noexcept {
    switch (op) {
        case Rdna2Opcode::V_CMP_LT_F16:
        case Rdna2Opcode::V_CMPX_LT_F16:  return 1u;   // LT
        case Rdna2Opcode::V_CMP_EQ_F16:
        case Rdna2Opcode::V_CMPX_EQ_F16:  return 2u;   // EQ
        case Rdna2Opcode::V_CMP_LE_F16:
        case Rdna2Opcode::V_CMPX_LE_F16:  return 3u;   // LE
        case Rdna2Opcode::V_CMP_GT_F16:
        case Rdna2Opcode::V_CMPX_GT_F16:  return 4u;   // GT
        case Rdna2Opcode::V_CMP_LG_F16:
        case Rdna2Opcode::V_CMPX_LG_F16:  return 5u;   // LG (ordered !=)
        case Rdna2Opcode::V_CMP_GE_F16:
        case Rdna2Opcode::V_CMPX_GE_F16:  return 6u;   // GE
        case Rdna2Opcode::V_CMP_U_F16:    return 8u;   // Unordered (either NaN)
        case Rdna2Opcode::V_CMP_NGE_F16:  return 9u;   // !(a >= b) == (a < b || unordered)
        case Rdna2Opcode::V_CMP_NLG_F16:  return 10u;  // (a == b || unordered)
        case Rdna2Opcode::V_CMP_NGT_F16:  return 11u;  // (a <= b || unordered)
        case Rdna2Opcode::V_CMP_NLE_F16:  return 12u;  // (a > b || unordered)
        case Rdna2Opcode::V_CMP_NEQ_F16:
        case Rdna2Opcode::V_CMPX_NEQ_F16: return 13u;  // (a != b || unordered)
        case Rdna2Opcode::V_CMP_NLT_F16:
        case Rdna2Opcode::V_CMPX_NLT_F16: return 14u;  // (a >= b || unordered)
        case Rdna2Opcode::V_CMP_TRU_F16:  return 15u;  // Always true
        default:                          return 0u;
    }
}

bool EvalF16Compare(uint8_t pred, float a, float b) noexcept {
    const bool unord = std::isnan(a) || std::isnan(b);
    switch (pred & 15u) {
        case 0u:  return false;
        case 1u:  return !unord && (a < b);
        case 2u:  return !unord && (a == b);
        case 3u:  return !unord && (a <= b);
        case 4u:  return !unord && (a > b);
        case 5u:  return !unord && (a != b);
        case 6u:  return !unord && (a >= b);
        case 7u:  return !unord;
        case 8u:  return unord;
        case 9u:  return unord || (a < b);
        case 10u: return unord || (a == b);
        case 11u: return unord || (a <= b);
        case 12u: return unord || (a > b);
        case 13u: return unord || (a != b);
        case 14u: return unord || (a >= b);
        case 15u: return true;
        default:  return false;
    }
}

uint8_t EncodeF32CmpPred(Rdna2Opcode op) noexcept {
    switch (op) {
        case Rdna2Opcode::V_CMP_F_F32:   return 0u;
        case Rdna2Opcode::V_CMP_LT_F32:
        case Rdna2Opcode::V_CMPX_LT_F32: return 1u;
        case Rdna2Opcode::V_CMP_EQ_F32:
        case Rdna2Opcode::V_CMPX_EQ_F32: return 2u;
        case Rdna2Opcode::V_CMP_LE_F32:
        case Rdna2Opcode::V_CMPX_LE_F32: return 3u;
        case Rdna2Opcode::V_CMP_GT_F32:
        case Rdna2Opcode::V_CMPX_GT_F32: return 4u;
        case Rdna2Opcode::V_CMP_LG_F32:
        case Rdna2Opcode::V_CMPX_LG_F32: return 5u;
        case Rdna2Opcode::V_CMP_GE_F32:
        case Rdna2Opcode::V_CMPX_GE_F32: return 6u;
        case Rdna2Opcode::V_CMP_O_F32:   return 7u;
        case Rdna2Opcode::V_CMP_U_F32:   return 8u;
        case Rdna2Opcode::V_CMP_NGE_F32: return 9u;
        case Rdna2Opcode::V_CMP_NLG_F32: return 10u;
        case Rdna2Opcode::V_CMP_NGT_F32: return 11u;
        case Rdna2Opcode::V_CMP_NLE_F32: return 12u;
        case Rdna2Opcode::V_CMP_NEQ_F32: return 13u;
        case Rdna2Opcode::V_CMP_NLT_F32: return 14u;
        case Rdna2Opcode::V_CMP_TRU_F32: return 15u;
        default:                         return 0u;
    }
}

uint32_t BitReverse32Cpu(uint32_t x) noexcept {
    x = ((x & 0x55555555u) << 1) | ((x >> 1) & 0x55555555u);
    x = ((x & 0x33333333u) << 2) | ((x >> 2) & 0x33333333u);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x >> 4) & 0x0F0F0F0Fu);
    x = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
    return (x << 16) | (x >> 16);
}

uint32_t EvalAlignBitB32(uint32_t src0_hi, uint32_t src1_lo, uint32_t shift_src) noexcept {
    const uint32_t sh = shift_src & 31u;
    if (sh == 0u) return src1_lo;
    return (src1_lo >> sh) | (src0_hi << (32u - sh));
}

bool IsFormatIntegerKind(Rdna2MtbufFormat fmt) noexcept {
    return GetRdna2MtbufFormatInfo(fmt).is_integer;
}

uint32_t FormatLoadComponentCount(Rdna2Opcode op, bool& out_is_d16) noexcept {
    out_is_d16 = false;
    switch (op) {
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_X:        return 1u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY:       return 2u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ:      return 3u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW:     return 4u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X:    out_is_d16 = true; return 1u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY:   out_is_d16 = true; return 2u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ:  out_is_d16 = true; return 3u;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW: out_is_d16 = true; return 4u;
        default:                                        return 1u;
    }
}

float EvalMadF32(float a, float b, float c) noexcept {
    const float fa = FlushDenormF32(a);
    const float fb = FlushDenormF32(b);
    const float fc = FlushDenormF32(c);
    const float prod = FlushDenormF32(fa * fb);
    return FlushDenormF32(prod + fc);
}

bool IsBranchOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_BRANCH:
        case Rdna2Opcode::S_CBRANCH_SCC0:
        case Rdna2Opcode::S_CBRANCH_SCC1:
        case Rdna2Opcode::S_CBRANCH_VCCZ:
        case Rdna2Opcode::S_CBRANCH_VCCNZ:
        case Rdna2Opcode::S_CBRANCH_EXECZ:
        case Rdna2Opcode::S_CBRANCH_EXECNZ:
            return true;
        default:
            return false;
    }
}

bool IsConditionalBranchOpcode(Rdna2Opcode op) {
    return IsBranchOpcode(op) && op != Rdna2Opcode::S_BRANCH;
}

void AddUnique(std::vector<uint32_t>& vec, uint32_t val) {
    if (std::find(vec.begin(), vec.end(), val) == vec.end()) {
        vec.push_back(val);
    }
}

struct RegState {
    std::array<IrValueId, 256> vgpr{};
    std::array<IrValueId, 106> sgpr{};
    IrValueId scc = kInvalidIrValue;
    IrValueId vcc = kInvalidIrValue;
    IrValueId exec = kInvalidIrValue;
    IrValueId m0 = kInvalidIrValue;
};

constexpr uint32_t kIrKillExportMagic = 0xDEAD0001u;

bool IsExtendedSaveExecSop1(const Rdna2Instruction& inst) noexcept {
    if (inst.encoding != Rdna2Encoding::SOP1 || inst.opcode != Rdna2Opcode::Unknown) return false;
    const uint16_t r = inst.raw_opcode;
    return r == kSop1RawAndSaveExecB64 || r == kSop1RawOrSaveExecB64 ||
           r == kSop1RawXorSaveExecB64 || r == kSop1RawAndN2SaveExecB64 ||
           r == kSop1RawAndSaveExecB32 || r == kSop1RawOrSaveExecB32 ||
           r == kSop1RawXorSaveExecB32 || r == kSop1RawAndN2SaveExecB32;
}

bool IsExtendedMovB64Sop1(const Rdna2Instruction& inst) noexcept {
    return inst.encoding == Rdna2Encoding::SOP1 &&
           inst.opcode == Rdna2Opcode::Unknown &&
           inst.raw_opcode == kSop1RawMovB64;
}

bool IsExtendedExecSop2(const Rdna2Instruction& inst) noexcept {
    if (inst.encoding != Rdna2Encoding::SOP2 || inst.opcode != Rdna2Opcode::Unknown) return false;
    const uint16_t r = inst.raw_opcode;
    return r == kSop2RawAndB64 || r == kSop2RawOrB64 ||
           r == kSop2RawXorB64 || r == kSop2RawAndN2B64;
}

uint32_t RoundUpPow2U32(uint32_t v, uint32_t min_val, uint32_t max_val) noexcept {
    uint32_t r = min_val;
    while (r < v && r < max_val) {
        r <<= 1;
    }
    return std::min(r, max_val);
}

}  // namespace

void AnalyzeRdna2Cfg(const Rdna2IrModule& ir, Rdna2CfgAnalysis& out_analysis) {
    out_analysis = Rdna2CfgAnalysis{};
    const size_t n = ir.blocks.size();
    if (n == 0u) return;

    std::vector<std::vector<uint32_t>> succs(n);
    std::vector<std::vector<uint32_t>> preds(n);
    for (size_t b = 0u; b < n; ++b) {
        const auto& ins = ir.blocks[b].instructions;
        if (ins.empty()) continue;
        const auto& term = ins.back();
        if (term.op == IrOp::Branch && term.true_block < n) {
            succs[b].push_back(term.true_block);
        } else if (term.op == IrOp::CondBranch) {
            if (term.true_block < n) succs[b].push_back(term.true_block);
            if (term.false_block < n && term.false_block != term.true_block) {
                succs[b].push_back(term.false_block);
            }
        }
    }

    // 1. Reachability from entry block 0
    std::vector<uint8_t> reachable(n, 0u);
    std::vector<uint32_t> stack = {0u};
    reachable[0] = 1u;
    while (!stack.empty()) {
        const uint32_t u = stack.back();
        stack.pop_back();
        for (uint32_t v : succs[u]) {
            if (!reachable[v]) {
                reachable[v] = 1u;
                stack.push_back(v);
            }
        }
    }
    for (size_t b = 0u; b < n; ++b) {
        if (!reachable[b]) continue;
        ++out_analysis.reachable_block_count;
        for (uint32_t v : succs[b]) {
            if (reachable[v]) {
                preds[v].push_back(static_cast<uint32_t>(b));
            }
        }
    }

    // 2. Iterative dominators over reachable blocks
    std::vector<std::vector<uint8_t>> dom(n, std::vector<uint8_t>(n, 1u));
    dom[0].assign(n, 0u);
    dom[0][0] = 1u;
    bool changed = true;
    for (size_t iter = 0u; changed && iter < (n + 2u) * 8u; ++iter) {
        changed = false;
        for (size_t b = 1u; b < n; ++b) {
            if (!reachable[b] || preds[b].empty()) continue;
            std::vector<uint8_t> next_dom(n, 1u);
            for (uint32_t p : preds[b]) {
                for (size_t k = 0u; k < n; ++k) {
                    next_dom[k] = static_cast<uint8_t>(next_dom[k] & dom[p][k]);
                }
            }
            next_dom[b] = 1u;
            if (next_dom != dom[b]) {
                dom[b] = std::move(next_dom);
                changed = true;
            }
        }
    }

    // 3. Tarjan's Strongly Connected Components over reachable blocks
    std::vector<int32_t> dfn(n, -1);
    std::vector<int32_t> low(n, -1);
    std::vector<uint8_t> on_stack(n, 0u);
    std::vector<uint32_t> tarjan_stk;
    int32_t cur_time = 0;
    std::vector<std::vector<uint32_t>> sccs;

    auto strongconnect = [&](auto&& self, uint32_t u) -> void {
        dfn[u] = low[u] = cur_time++;
        tarjan_stk.push_back(u);
        on_stack[u] = 1u;
        for (uint32_t v : succs[u]) {
            if (!reachable[v]) continue;
            if (dfn[v] == -1) {
                self(self, v);
                low[u] = std::min(low[u], low[v]);
            } else if (on_stack[v]) {
                low[u] = std::min(low[u], dfn[v]);
            }
        }
        if (low[u] == dfn[u]) {
            std::vector<uint32_t> comp;
            for (;;) {
                const uint32_t w = tarjan_stk.back();
                tarjan_stk.pop_back();
                on_stack[w] = 0u;
                comp.push_back(w);
                if (w == u) break;
            }
            std::sort(comp.begin(), comp.end());
            sccs.push_back(std::move(comp));
        }
    };
    strongconnect(strongconnect, 0u);

    // 4. Identify natural loops (dominator back-edges) and irreducible multi-entry SCCs
    std::set<uint32_t> header_set;
    for (size_t u = 0u; u < n; ++u) {
        if (!reachable[u]) continue;
        for (uint32_t v : succs[u]) {
            if (!reachable[v]) continue;
            if (dom[u][v]) {
                out_analysis.has_cycles = true;
                out_analysis.back_edges.emplace_back(static_cast<uint32_t>(u), v);
                header_set.insert(v);
            }
        }
    }
    out_analysis.loop_headers.assign(header_set.begin(), header_set.end());
    out_analysis.natural_loop_count = static_cast<uint32_t>(out_analysis.loop_headers.size());

    for (const auto& comp : sccs) {
        bool is_cyclic = (comp.size() > 1u);
        if (!is_cyclic && comp.size() == 1u) {
            const uint32_t u = comp[0];
            for (uint32_t v : succs[u]) {
                if (v == u) {
                    is_cyclic = true;
                    break;
                }
            }
        }
        if (!is_cyclic) continue;
        out_analysis.has_cycles = true;

        std::unordered_set<uint32_t> in_comp(comp.begin(), comp.end());
        uint32_t entry_count = 0u;
        for (uint32_t u : comp) {
            bool is_entry = (u == 0u);
            if (!is_entry) {
                for (uint32_t p : preds[u]) {
                    if (in_comp.count(p) == 0u) {
                        is_entry = true;
                        break;
                    }
                }
            }
            if (is_entry) ++entry_count;
        }
        if (entry_count >= 2u) {
            out_analysis.is_irreducible = true;
            ++out_analysis.irreducible_scc_count;
            out_analysis.irreducible_sccs.push_back(comp);
        }
    }
}

void CanonicalizeRdna2Cfg(Rdna2IrModule& module) {
    if (module.blocks.empty()) return;

    auto rebuild_edges = [&]() {
        const size_t n = module.blocks.size();
        for (size_t b = 0u; b < n; ++b) {
            module.blocks[b].id = static_cast<uint32_t>(b);
            module.blocks[b].predecessors.clear();
            module.blocks[b].successors.clear();
        }
        for (size_t b = 0u; b < n; ++b) {
            const auto& ins = module.blocks[b].instructions;
            if (ins.empty()) continue;
            const auto& term = ins.back();
            if (term.op == IrOp::Branch && term.true_block < n) {
                module.blocks[b].successors.push_back(term.true_block);
            } else if (term.op == IrOp::CondBranch) {
                if (term.true_block < n) module.blocks[b].successors.push_back(term.true_block);
                if (term.false_block < n && term.false_block != term.true_block) {
                    module.blocks[b].successors.push_back(term.false_block);
                }
            }
        }
        for (size_t b = 0u; b < n; ++b) {
            for (uint32_t s : module.blocks[b].successors) {
                auto& preds = module.blocks[s].predecessors;
                if (std::find(preds.begin(), preds.end(), static_cast<uint32_t>(b)) == preds.end()) {
                    preds.push_back(static_cast<uint32_t>(b));
                }
            }
        }
    };

    auto redirect_edge = [&](uint32_t src_bb, uint32_t old_dst, uint32_t new_dst) {
        auto& ins = module.blocks[src_bb].instructions;
        if (ins.empty()) return;
        auto& term = ins.back();
        if (term.op == IrOp::Branch) {
            if (term.true_block == old_dst) term.true_block = new_dst;
        } else if (term.op == IrOp::CondBranch) {
            if (term.true_block == old_dst) term.true_block = new_dst;
            if (term.false_block == old_dst) term.false_block = new_dst;
        }
    };

    // Pass A: Canonicalize exit-less loops (infinite / self-looping cyclic SCCs with no exit edge
    // and no Return) by synthesizing a bounded loop-guard exit edge + clean Return exit block so
    // structured HLSL / SPIR-V emission never hangs or produces invalid control flow.
    for (uint32_t pass = 0u; pass < 16u; ++pass) {
        rebuild_edges();
        const size_t n = module.blocks.size();

        std::vector<uint8_t> reachable(n, 0u);
        std::vector<uint32_t> dfs_stk = {0u};
        reachable[0] = 1u;
        while (!dfs_stk.empty()) {
            const uint32_t u = dfs_stk.back();
            dfs_stk.pop_back();
            for (uint32_t v : module.blocks[u].successors) {
                if (!reachable[v]) {
                    reachable[v] = 1u;
                    dfs_stk.push_back(v);
                }
            }
        }

        std::vector<int32_t> dfn(n, -1);
        std::vector<int32_t> low(n, -1);
        std::vector<uint8_t> on_stack(n, 0u);
        std::vector<uint32_t> tarjan_stk;
        int32_t cur_time = 0;
        std::vector<std::vector<uint32_t>> sccs;

        auto strongconnect = [&](auto&& self, uint32_t u) -> void {
            dfn[u] = low[u] = cur_time++;
            tarjan_stk.push_back(u);
            on_stack[u] = 1u;
            for (uint32_t v : module.blocks[u].successors) {
                if (!reachable[v]) continue;
                if (dfn[v] == -1) {
                    self(self, v);
                    low[u] = std::min(low[u], low[v]);
                } else if (on_stack[v]) {
                    low[u] = std::min(low[u], dfn[v]);
                }
            }
            if (low[u] == dfn[u]) {
                std::vector<uint32_t> comp;
                for (;;) {
                    const uint32_t w = tarjan_stk.back();
                    tarjan_stk.pop_back();
                    on_stack[w] = 0u;
                    comp.push_back(w);
                    if (w == u) break;
                }
                std::sort(comp.begin(), comp.end());
                sccs.push_back(std::move(comp));
            }
        };
        strongconnect(strongconnect, 0u);

        bool fixed_one = false;
        for (const auto& comp : sccs) {
            bool is_cyclic = (comp.size() > 1u);
            if (!is_cyclic && comp.size() == 1u) {
                const uint32_t u = comp[0];
                for (uint32_t v : module.blocks[u].successors) {
                    if (v == u) {
                        is_cyclic = true;
                        break;
                    }
                }
            }
            if (!is_cyclic) continue;

            std::unordered_set<uint32_t> in_comp(comp.begin(), comp.end());
            bool has_exit = false;
            for (uint32_t u : comp) {
                const auto& ins = module.blocks[u].instructions;
                if (!ins.empty() && ins.back().op == IrOp::Return) {
                    has_exit = true;
                    break;
                }
                for (uint32_t v : module.blocks[u].successors) {
                    if (in_comp.count(v) == 0u) {
                        has_exit = true;
                        break;
                    }
                }
                if (has_exit) break;
            }
            if (has_exit) continue;

            // Found an exit-less cyclic SCC: choose header H, collect external & back-edge preds.
            uint32_t H = comp[0];
            if (in_comp.count(0u) > 0u) {
                H = 0u;
            } else {
                for (uint32_t u : comp) {
                    bool ext = false;
                    for (uint32_t p : module.blocks[u].predecessors) {
                        if (reachable[p] && in_comp.count(p) == 0u) {
                            ext = true;
                            break;
                        }
                    }
                    if (ext) {
                        H = u;
                        break;
                    }
                }
            }

            std::vector<uint32_t> ext_preds;
            std::vector<uint32_t> back_preds;
            for (uint32_t p : module.blocks[H].predecessors) {
                if (!reachable[p]) continue;
                if (in_comp.count(p) > 0u) back_preds.push_back(p);
                else ext_preds.push_back(p);
            }
            if (back_preds.empty()) {
                // Fallback: find any block in comp with an edge into comp
                for (uint32_t u : comp) {
                    if (!module.blocks[u].successors.empty()) {
                        H = module.blocks[u].successors[0];
                        for (uint32_t p : module.blocks[H].predecessors) {
                            if (in_comp.count(p) > 0u) back_preds.push_back(p);
                            else if (reachable[p]) ext_preds.push_back(p);
                        }
                        break;
                    }
                }
            }
            if (back_preds.empty()) continue;

            std::unordered_set<uint32_t> back_set(back_preds.begin(), back_preds.end());
            const uint32_t exit_bb_id = static_cast<uint32_t>(module.blocks.size());
            const uint32_t guard_bb_id = exit_bb_id + 1u;

            IrBasicBlock exit_bb{};
            exit_bb.id = exit_bb_id;
            IrInstruction ret_ins{};
            ret_ins.op = IrOp::Return;
            ret_ins.type = IrType::Void;
            exit_bb.instructions.push_back(std::move(ret_ins));

            IrBasicBlock guard_bb{};
            guard_bb.id = guard_bb_id;

            // Redirect every back-edge (L -> H) to (L -> guard_bb) and update H's Phis.
            for (uint32_t L : back_preds) {
                redirect_edge(L, H, guard_bb_id);
            }
            for (auto& phi : module.blocks[H].instructions) {
                if (phi.op != IrOp::Phi) break;
                std::vector<std::pair<uint32_t, IrValueId>> kept;
                std::vector<std::pair<uint32_t, IrValueId>> from_back;
                for (const auto& inc : phi.phi_incoming) {
                    if (back_set.count(inc.first) > 0u) from_back.push_back(inc);
                    else kept.push_back(inc);
                }
                if (!from_back.empty()) {
                    if (from_back.size() == 1u) {
                        kept.emplace_back(guard_bb_id, from_back[0].second);
                    } else {
                        IrInstruction merge_phi{};
                        merge_phi.id = module.AllocateValueId();
                        merge_phi.op = IrOp::Phi;
                        merge_phi.type = phi.type;
                        merge_phi.phi_incoming = std::move(from_back);
                        const IrValueId merged_id = merge_phi.id;
                        guard_bb.instructions.push_back(std::move(merge_phi));
                        kept.emplace_back(guard_bb_id, merged_id);
                    }
                    phi.phi_incoming = std::move(kept);
                }
            }

            // Synthesize a bounded loop trip-counter Phi in H and guard branch in guard_bb.
            IrValueId c_zero_id = kInvalidIrValue;
            if (!ext_preds.empty()) {
                IrInstruction c_zero{};
                c_zero.id = module.AllocateValueId();
                c_zero.op = IrOp::ConstU32;
                c_zero.type = IrType::U32;
                c_zero.imm_u32 = 0u;
                c_zero_id = c_zero.id;
                auto& b0_ins = module.blocks[0].instructions;
                size_t insert_pos = 0u;
                while (insert_pos < b0_ins.size() && b0_ins[insert_pos].op == IrOp::Phi) {
                    ++insert_pos;
                }
                b0_ins.insert(b0_ins.begin() + static_cast<std::ptrdiff_t>(insert_pos), std::move(c_zero));
            }

            const IrValueId cnt_phi_id = module.AllocateValueId();
            const IrValueId c_one_id = module.AllocateValueId();
            const IrValueId c_lim_id = module.AllocateValueId();
            const IrValueId cnt_next_id = module.AllocateValueId();
            const IrValueId keep_id = module.AllocateValueId();

            IrInstruction cnt_phi{};
            cnt_phi.id = cnt_phi_id;
            cnt_phi.op = IrOp::Phi;
            cnt_phi.type = IrType::U32;
            for (uint32_t ep : ext_preds) {
                cnt_phi.phi_incoming.emplace_back(ep, c_zero_id);
            }
            cnt_phi.phi_incoming.emplace_back(guard_bb_id, cnt_next_id);
            module.blocks[H].instructions.insert(module.blocks[H].instructions.begin(), std::move(cnt_phi));

            {
                IrInstruction c_one{};
                c_one.id = c_one_id;
                c_one.op = IrOp::ConstU32;
                c_one.type = IrType::U32;
                c_one.imm_u32 = 1u;
                guard_bb.instructions.push_back(std::move(c_one));

                IrInstruction c_lim{};
                c_lim.id = c_lim_id;
                c_lim.op = IrOp::ConstU32;
                c_lim.type = IrType::U32;
                c_lim.imm_u32 = 65536u;
                guard_bb.instructions.push_back(std::move(c_lim));

                IrInstruction cnt_next{};
                cnt_next.id = cnt_next_id;
                cnt_next.op = IrOp::IAdd;
                cnt_next.type = IrType::U32;
                cnt_next.operands = {cnt_phi_id, c_one_id};
                guard_bb.instructions.push_back(std::move(cnt_next));

                IrInstruction keep{};
                keep.id = keep_id;
                keep.op = IrOp::CmpLtU;
                keep.type = IrType::Bool;
                keep.operands = {cnt_next_id, c_lim_id};
                guard_bb.instructions.push_back(std::move(keep));

                IrInstruction cbr{};
                cbr.op = IrOp::CondBranch;
                cbr.type = IrType::Void;
                cbr.operands = {keep_id};
                cbr.true_block = H;
                cbr.false_block = exit_bb_id;
                guard_bb.instructions.push_back(std::move(cbr));
            }

            module.blocks.push_back(std::move(exit_bb));
            module.blocks.push_back(std::move(guard_bb));
            fixed_one = true;
            break;
        }
        if (!fixed_one) break;
    }

    // Pass B: Canonicalize non-merge loop latches on natural loops (multi-latch, self-loop, or
    // conditional back-edge latches) into a single dedicated unconditional latch block -> header.
    // Irreducible multi-entry SCCs are intentionally left intact so their SCC topology is preserved.
    rebuild_edges();
    Rdna2CfgAnalysis cfg{};
    AnalyzeRdna2Cfg(module, cfg);
    std::unordered_set<uint32_t> irred_blocks;
    for (const auto& comp : cfg.irreducible_sccs) {
        for (uint32_t b : comp) irred_blocks.insert(b);
    }

    std::map<uint32_t, std::vector<uint32_t>> header_latches;
    for (const auto& edge : cfg.back_edges) {
        const uint32_t L = edge.first;
        const uint32_t H = edge.second;
        if (irred_blocks.count(H) > 0u || irred_blocks.count(L) > 0u) continue;
        auto& vec = header_latches[H];
        if (std::find(vec.begin(), vec.end(), L) == vec.end()) {
            vec.push_back(L);
        }
    }

    bool modified_latches = false;
    for (const auto& kv : header_latches) {
        const uint32_t H = kv.first;
        const auto& latches = kv.second;
        if (latches.empty()) continue;

        const bool already_canonical =
            (latches.size() == 1u) &&
            (latches[0] != H) &&
            !module.blocks[latches[0]].instructions.empty() &&
            (module.blocks[latches[0]].instructions.back().op == IrOp::Branch) &&
            (module.blocks[latches[0]].instructions.back().true_block == H) &&
            (module.blocks[latches[0]].successors.size() == 1u);
        if (already_canonical) continue;

        const uint32_t latch_bb_id = static_cast<uint32_t>(module.blocks.size());
        IrBasicBlock latch_bb{};
        latch_bb.id = latch_bb_id;

        std::unordered_set<uint32_t> latch_set(latches.begin(), latches.end());
        for (uint32_t L : latches) {
            redirect_edge(L, H, latch_bb_id);
        }

        for (auto& phi : module.blocks[H].instructions) {
            if (phi.op != IrOp::Phi) break;
            std::vector<std::pair<uint32_t, IrValueId>> kept;
            std::vector<std::pair<uint32_t, IrValueId>> from_latches;
            for (const auto& inc : phi.phi_incoming) {
                if (latch_set.count(inc.first) > 0u) from_latches.push_back(inc);
                else kept.push_back(inc);
            }
            if (!from_latches.empty()) {
                if (from_latches.size() == 1u) {
                    kept.emplace_back(latch_bb_id, from_latches[0].second);
                } else {
                    IrInstruction merge_phi{};
                    merge_phi.id = module.AllocateValueId();
                    merge_phi.op = IrOp::Phi;
                    merge_phi.type = phi.type;
                    merge_phi.phi_incoming = std::move(from_latches);
                    const IrValueId merged_id = merge_phi.id;
                    latch_bb.instructions.push_back(std::move(merge_phi));
                    kept.emplace_back(latch_bb_id, merged_id);
                }
                phi.phi_incoming = std::move(kept);
            }
        }

        IrInstruction br{};
        br.op = IrOp::Branch;
        br.type = IrType::Void;
        br.true_block = H;
        latch_bb.instructions.push_back(std::move(br));

        module.blocks.push_back(std::move(latch_bb));
        modified_latches = true;
    }

    if (modified_latches) {
        rebuild_edges();
    } else {
        rebuild_edges();
    }
}

const IrInstruction* Rdna2IrModule::FindDef(IrValueId id) const noexcept {
    if (id == kInvalidIrValue) return nullptr;
    for (const auto& bb : blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.id == id) return &inst;
        }
    }
    return nullptr;
}

const char* IrOpName(IrOp op) noexcept {
    switch (op) {
        case IrOp::ConstU32:      return "const.u32";
        case IrOp::ConstF32:      return "const.f32";
        case IrOp::ThreadIdX:     return "thread_id.x";
        case IrOp::StageInput:    return "stage_input";
        case IrOp::LoadRootConst: return "load_root_const";
        case IrOp::LoadUav:       return "load_uav";
        case IrOp::StoreUav:      return "store_uav";
        case IrOp::ImageSample:   return "image_sample";
        case IrOp::ImageSampleDynamic: return "image_sample_dynamic";
        case IrOp::ImageSampleImplicit: return "image_sample_implicit";
        case IrOp::ImageLoad:     return "image_load";
        case IrOp::ImageLoadMip:  return "image_load_mip";
        case IrOp::ImageStore:    return "image_store";
        case IrOp::ImageAtomicCmpSwap: return "image_atomic_cmpswap";
        case IrOp::BvhIntersectRay:    return "bvh_intersect_ray";
        case IrOp::LdsLoad:       return "lds_load";
        case IrOp::LdsStore:      return "lds_store";
        case IrOp::LdsAtomicAdd:  return "lds_atomic_add";
        case IrOp::ScratchLoad:   return "scratch_load";
        case IrOp::ScratchStore:  return "scratch_store";
        case IrOp::WaveLaneId:            return "wave_lane_id";
        case IrOp::WaveReadFirstLane:     return "wave_read_first_lane";
        case IrOp::WaveReadLaneAt:        return "wave_read_lane_at";
        case IrOp::WaveBallotLo:          return "wave_ballot_lo";
        case IrOp::WaveBallotHi:          return "wave_ballot_hi";
        case IrOp::WaveActiveCountBits64: return "wave_active_count_bits64";
        case IrOp::WavePrefixCount64:     return "wave_prefix_count64";
        case IrOp::WavePermLane16:        return "wave_permlane16";
        case IrOp::MeshGlobalThreadId:    return "mesh_global_tid";
        case IrOp::MergedWaveInfo:        return "merged_wave_info";
        case IrOp::SetMeshOutputCounts:   return "set_mesh_output_counts";
        case IrOp::ExportMeshPrim:        return "export_mesh_prim";
        case IrOp::ExportMeshCull:        return "export_mesh_cull";
        case IrOp::DispatchMeshGroups:    return "dispatch_mesh_groups";
        case IrOp::FAdd:          return "fadd";
        case IrOp::FSub:          return "fsub";
        case IrOp::FMul:          return "fmul";
        case IrOp::FFma:          return "ffma";
        case IrOp::FMad:          return "fmad";
        case IrOp::FFma16:        return "ffma.f16";
        case IrOp::FMin:          return "fmin";
        case IrOp::FMax:          return "fmax";
        case IrOp::FRcp:          return "frcp";
        case IrOp::FRsq:          return "frsq";
        case IrOp::FSqrt:         return "fsqrt";
        case IrOp::FFloor:        return "ffloor";
        case IrOp::FCeil:         return "fceil";
        case IrOp::FFract:        return "ffract";
        case IrOp::FDot2:         return "fdot2";
        case IrOp::IAdd:          return "iadd";
        case IrOp::ISub:          return "isub";
        case IrOp::IMul:          return "imul";
        case IrOp::IMin:          return "imin";
        case IrOp::IMax:          return "imax";
        case IrOp::BitAnd:        return "and";
        case IrOp::BitOr:         return "or";
        case IrOp::BitXor:        return "xor";
        case IrOp::BitNot:        return "not";
        case IrOp::BitReverse32:  return "brev32";
        case IrOp::BitCount32:    return "bcnt32";
        case IrOp::AbsDiffI32:    return "absdiff.i32";
        case IrOp::Shl:           return "shl";
        case IrOp::Shr:           return "shr";
        case IrOp::AlignBit:      return "alignbit";
        case IrOp::AlignByte:     return "alignbyte";
        case IrOp::FmaMix:        return "fma_mix";
        case IrOp::BufferLoadFormat: return "buf_load_fmt";
        case IrOp::BfeU32:        return "bfe.u32";
        case IrOp::BfeI32:        return "bfe.i32";
        case IrOp::BfiB32:        return "bfi.b32";
        case IrOp::PkAddF16:      return "pk_add.f16";
        case IrOp::PkMulF16:      return "pk_mul.f16";
        case IrOp::PkFmaF16:      return "pk_fma.f16";
        case IrOp::PkAddU16:      return "pk_add.u16";
        case IrOp::PkShl16:       return "pk_shl.b16";
        case IrOp::PkShr16:       return "pk_shr.b16";
        case IrOp::I2F:           return "i2f";
        case IrOp::F2I:           return "f2i";
        case IrOp::U2F:           return "u2f";
        case IrOp::F2U:           return "f2u";
        case IrOp::CvtPkNormI16F32: return "cvt_pknorm.i16.f32";
        case IrOp::CvtPkNormU16F32: return "cvt_pknorm.u16.f32";
        case IrOp::CvtNormI16F16:   return "cvt_norm.i16.f16";
        case IrOp::CvtNormU16F16:   return "cvt_norm.u16.f16";
        case IrOp::CmpEq:         return "cmp.eq";
        case IrOp::CmpNe:         return "cmp.ne";
        case IrOp::CmpLt:         return "cmp.lt";
        case IrOp::CmpGt:         return "cmp.gt";
        case IrOp::CmpGe:         return "cmp.ge";
        case IrOp::CmpLtU:        return "cmp.ltu";
        case IrOp::CmpEqU:        return "cmp.equ";
        case IrOp::CmpGtU:        return "cmp.gtu";
        case IrOp::CmpGeU:        return "cmp.geu";
        case IrOp::CmpF32:        return "cmp.f32";
        case IrOp::CmpF16:        return "cmp.f16";
        case IrOp::LogicalAnd:    return "land";
        case IrOp::LogicalNot:    return "lnot";
        case IrOp::Select:        return "select";
        case IrOp::Phi:           return "phi";
        case IrOp::Barrier:       return "barrier";
        case IrOp::Branch:        return "br";
        case IrOp::CondBranch:    return "cbr";
        case IrOp::Export:        return "export";
        case IrOp::Return:        return "ret";
        default:                  return "unknown";
    }
}

const char* IrTypeName(IrType type) noexcept {
    switch (type) {
        case IrType::Void: return "void";
        case IrType::U32:  return "u32";
        case IrType::I32:  return "i32";
        case IrType::F32:  return "f32";
        case IrType::Bool: return "bool";
        default:           return "?";
    }
}

bool IrOpHasSideEffects(IrOp op) noexcept {
    switch (op) {
        case IrOp::StoreUav:
        case IrOp::ImageStore:
        case IrOp::ImageAtomicCmpSwap:
        case IrOp::LdsStore:
        case IrOp::LdsAtomicAdd:
        case IrOp::ScratchStore:
        case IrOp::SetMeshOutputCounts:
        case IrOp::ExportMeshPrim:
        case IrOp::ExportMeshCull:
        case IrOp::DispatchMeshGroups:
        case IrOp::Barrier:
        case IrOp::Branch:
        case IrOp::CondBranch:
        case IrOp::Export:
        case IrOp::Return:
            return true;
        default:
            return false;
    }
}

void ReconcileStageInterpolation(Rdna2IrModule& feeder_ir, Rdna2IrModule& ps_ir) noexcept {
    // Match interpolation modifiers between PS inputs and feeder stage (VS / GS / MS) outputs.
    const size_t max_slots = std::max(feeder_ir.output_interp_modes.size(), ps_ir.input_interp_modes.size());
    if (max_slots == 0u) return;
    feeder_ir.output_interp_modes.resize(max_slots, Rdna2InterpMode::Perspective);
    ps_ir.input_interp_modes.resize(max_slots, Rdna2InterpMode::Perspective);
    for (size_t i = 0; i < max_slots; ++i) {
        if (ps_ir.input_interp_modes[i] != Rdna2InterpMode::Perspective) {
            // PS FSSM / V_INTERP_MOV_F32 specifies non-default interpolation (Flat/NoPerspective/Centroid/Sample)
            feeder_ir.output_interp_modes[i] = ps_ir.input_interp_modes[i];
        } else if (feeder_ir.output_interp_modes[i] != Rdna2InterpMode::Perspective) {
            ps_ir.input_interp_modes[i] = feeder_ir.output_interp_modes[i];
        }
    }
}

Status BuildRdna2Ir(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2IrModule& out_module,
    const StageContractTable* stage_contract) {
    out_module = Rdna2IrModule{};
    out_module.stage = container.stage;
    out_module.threadgroup_x = container.threadgroup_x ? container.threadgroup_x : 1u;
    out_module.threadgroup_y = container.threadgroup_y ? container.threadgroup_y : 1u;
    out_module.threadgroup_z = container.threadgroup_z ? container.threadgroup_z : 1u;
    out_module.sgpr_count = container.sgpr_count;
    out_module.vgpr_count = container.vgpr_count;
    out_module.wave_size = (container.wave_size == 64u) ? 64u : 32u;
    out_module.uses_wave64_emulation = (out_module.wave_size == 64u);
    // Register mesh NumThreads BEFORE any v5 workgroup seeding (group * NumThreads + lane)
    out_module.mesh_num_threads = std::max(
        1u, out_module.threadgroup_x * out_module.threadgroup_y * out_module.threadgroup_z);
    if (stage_contract != nullptr) {
        if (auto st = ValidateVertexInputTable(stage_contract->vertex_fetches); !st.ok()) {
            return st;
        }
        out_module.stage_contract = *stage_contract;
        out_module.texture_srv_format = stage_contract->texture_srv_format;
        for (const auto& entry : stage_contract->pixel_inputs) {
            if (entry.semantic_location >= out_module.input_interp_modes.size()) {
                out_module.input_interp_modes.resize(entry.semantic_location + 1u, Rdna2InterpMode::Perspective);
            }
            out_module.input_interp_modes[entry.semantic_location] = entry.interp_mode;
        }
    }

    if (instructions.empty()) {
        return Status::Error(StatusCode::ParseError, "Cannot build IR from empty instruction stream");
    }
    if (auto st = ValidateMimgUnsupportedModifiersIr(instructions); !st.ok()) {
        return st;
    }

    // 1. Map word offsets -> instruction indices & identify basic block leaders.
    std::unordered_map<uint32_t, size_t> word_to_idx;
    word_to_idx.reserve(instructions.size());
    for (size_t i = 0; i < instructions.size(); ++i) {
        word_to_idx[instructions[i].word_offset] = i;
    }

    std::set<size_t> leader_set;
    leader_set.insert(0);
    std::unordered_map<size_t, size_t> branch_target_idx;
    std::unordered_map<size_t, size_t> call_target_idx;
    std::unordered_map<size_t, size_t> call_return_idx;
    std::unordered_map<size_t, size_t> setpc_target_idx;
    std::vector<std::pair<uint32_t, size_t>> call_return_sites;  // (ret_pc_bytes, return_inst_idx)

    // Pre-pass: track statically resolved 64-bit PC byte addresses (including s_getpc_b64 +
    // s_add_u32 + s_addc_u32 + s_setpc_b64, xbox-emulator #185) in SGPR pairs with SCC carry propagation.
    {
        std::array<bool, 106> sgpr_has_val{};
        std::array<uint32_t, 106> sgpr_val_u32{};
        bool scc_known = false;
        uint32_t scc_val = 0u;
        const bool has_vf_contract = !out_module.stage_contract.vertex_fetches.empty();

        auto try_const_u32 = [&](const Rdna2Operand& op, uint32_t& out_v) -> bool {
            if (op.kind == Rdna2OperandKind::InlineInt) {
                out_v = static_cast<uint32_t>(op.int_value);
                return true;
            }
            if (op.kind == Rdna2OperandKind::Literal32) {
                out_v = op.literal_bits;
                return true;
            }
            if (op.kind == Rdna2OperandKind::Sgpr && op.reg_index < 106u && sgpr_has_val[op.reg_index]) {
                out_v = sgpr_val_u32[op.reg_index];
                return true;
            }
            return false;
        };

        auto resolve_pc_pair = [&](uint16_t reg, uint64_t& out_byte_addr) -> bool {
            if (reg >= 106u || !sgpr_has_val[reg]) return false;
            const uint32_t lo = sgpr_val_u32[reg];
            const uint32_t hi = (reg + 1u < 106u && sgpr_has_val[reg + 1u]) ? sgpr_val_u32[reg + 1u] : 0u;
            out_byte_addr = (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
            return true;
        };

        for (size_t i = 0; i < instructions.size(); ++i) {
            const auto& inst = instructions[i];
            if (IsBranchOpcode(inst.opcode)) {
                const int64_t target_word =
                    static_cast<int64_t>(inst.word_offset) + 1 + static_cast<int64_t>(inst.simm16);
                if (target_word < 0) {
                    return Status::Error(StatusCode::ParseError, "RDNA2 branch target out of range (< 0)");
                }
                auto it = word_to_idx.find(static_cast<uint32_t>(target_word));
                if (it == word_to_idx.end()) {
                    return Status::Error(StatusCode::ParseError, "RDNA2 branch target does not land on instruction boundary");
                }
                branch_target_idx[i] = it->second;
                leader_set.insert(it->second);
                if (i + 1 < instructions.size()) {
                    leader_set.insert(i + 1);
                }
            } else if (inst.opcode == Rdna2Opcode::S_GETPC_B64 &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                const uint64_t next_pc = static_cast<uint64_t>(inst.word_offset + 1u) * 4ull;
                sgpr_has_val[inst.dst.reg_index] = true;
                sgpr_val_u32[inst.dst.reg_index] = static_cast<uint32_t>(next_pc & 0xFFFFFFFFull);
                if (inst.dst.reg_index + 1u < 106u) {
                    sgpr_has_val[inst.dst.reg_index + 1u] = true;
                    sgpr_val_u32[inst.dst.reg_index + 1u] = static_cast<uint32_t>(next_pc >> 32);
                }
            } else if (inst.opcode == Rdna2Opcode::S_MOV_B32 &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                uint32_t v = 0u;
                if (try_const_u32(inst.src[0], v)) {
                    sgpr_has_val[inst.dst.reg_index] = true;
                    sgpr_val_u32[inst.dst.reg_index] = v;
                } else {
                    sgpr_has_val[inst.dst.reg_index] = false;
                }
            } else if (inst.opcode == Rdna2Opcode::S_MOVK_I32 &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                sgpr_has_val[inst.dst.reg_index] = true;
                sgpr_val_u32[inst.dst.reg_index] = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
            } else if ((inst.opcode == Rdna2Opcode::S_ADD_U32 || inst.opcode == Rdna2Opcode::S_ADD_I32) &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                uint32_t a = 0u, b = 0u;
                if (try_const_u32(inst.src[0], a) && try_const_u32(inst.src[1], b)) {
                    const uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
                    sgpr_has_val[inst.dst.reg_index] = true;
                    sgpr_val_u32[inst.dst.reg_index] = static_cast<uint32_t>(sum & 0xFFFFFFFFull);
                    scc_known = true;
                    scc_val = (sum > 0xFFFFFFFFull) ? 1u : 0u;
                } else {
                    sgpr_has_val[inst.dst.reg_index] = false;
                    scc_known = false;
                }
            } else if (inst.opcode == Rdna2Opcode::S_ADDC_U32 &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                uint32_t a = 0u, b = 0u;
                if (scc_known && try_const_u32(inst.src[0], a) && try_const_u32(inst.src[1], b)) {
                    const uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + static_cast<uint64_t>(scc_val);
                    sgpr_has_val[inst.dst.reg_index] = true;
                    sgpr_val_u32[inst.dst.reg_index] = static_cast<uint32_t>(sum & 0xFFFFFFFFull);
                    scc_known = true;
                    scc_val = (sum > 0xFFFFFFFFull) ? 1u : 0u;
                } else {
                    sgpr_has_val[inst.dst.reg_index] = false;
                    scc_known = false;
                }
            } else if ((inst.opcode == Rdna2Opcode::S_SUB_U32 || inst.opcode == Rdna2Opcode::S_SUB_I32) &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                uint32_t a = 0u, b = 0u;
                if (try_const_u32(inst.src[0], a) && try_const_u32(inst.src[1], b)) {
                    sgpr_has_val[inst.dst.reg_index] = true;
                    sgpr_val_u32[inst.dst.reg_index] = a - b;
                    scc_known = true;
                    scc_val = (a < b) ? 1u : 0u;
                } else {
                    sgpr_has_val[inst.dst.reg_index] = false;
                    scc_known = false;
                }
            } else if (inst.opcode == Rdna2Opcode::S_SUBB_U32 &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                uint32_t a = 0u, b = 0u;
                if (scc_known && try_const_u32(inst.src[0], a) && try_const_u32(inst.src[1], b)) {
                    const uint64_t sub = static_cast<uint64_t>(b) + static_cast<uint64_t>(scc_val);
                    sgpr_has_val[inst.dst.reg_index] = true;
                    sgpr_val_u32[inst.dst.reg_index] = static_cast<uint32_t>(static_cast<uint64_t>(a) - sub);
                    scc_known = true;
                    scc_val = (static_cast<uint64_t>(a) < sub) ? 1u : 0u;
                } else {
                    sgpr_has_val[inst.dst.reg_index] = false;
                    scc_known = false;
                }
            } else if (inst.opcode == Rdna2Opcode::S_ADDK_I32 &&
                       inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u &&
                       sgpr_has_val[inst.dst.reg_index]) {
                const uint64_t sum = static_cast<uint64_t>(sgpr_val_u32[inst.dst.reg_index]) +
                                     static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                sgpr_val_u32[inst.dst.reg_index] = static_cast<uint32_t>(sum & 0xFFFFFFFFull);
            } else if (!has_vf_contract && inst.opcode == Rdna2Opcode::S_SWAPPC_B64) {
                uint64_t byte_tgt = 0ull;
                if (inst.src[0].kind == Rdna2OperandKind::Sgpr && resolve_pc_pair(inst.src[0].reg_index, byte_tgt)) {
                    if ((byte_tgt & 3ull) == 0ull && (byte_tgt >> 32) == 0ull) {
                        auto it = word_to_idx.find(static_cast<uint32_t>(byte_tgt / 4ull));
                        if (it != word_to_idx.end() && i + 1 < instructions.size()) {
                            call_target_idx[i] = it->second;
                            call_return_idx[i] = i + 1;
                            call_return_sites.emplace_back((inst.word_offset + 1u) * 4u, i + 1);
                            leader_set.insert(it->second);
                            leader_set.insert(i + 1);
                        }
                    }
                }
                if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                    sgpr_has_val[inst.dst.reg_index] = false;
                    if (inst.dst.reg_index + 1u < 106u) sgpr_has_val[inst.dst.reg_index + 1u] = false;
                }
            } else if (!has_vf_contract && inst.opcode == Rdna2Opcode::S_SETPC_B64) {
                uint64_t byte_tgt = 0ull;
                if (inst.src[0].kind == Rdna2OperandKind::Sgpr && resolve_pc_pair(inst.src[0].reg_index, byte_tgt)) {
                    if ((byte_tgt & 3ull) == 0ull && (byte_tgt >> 32) == 0ull) {
                        auto it = word_to_idx.find(static_cast<uint32_t>(byte_tgt / 4ull));
                        if (it != word_to_idx.end()) {
                            setpc_target_idx[i] = it->second;
                            leader_set.insert(it->second);
                        }
                    }
                }
                if (i + 1 < instructions.size()) {
                    leader_set.insert(i + 1);
                }
            } else if (inst.opcode == Rdna2Opcode::S_ENDPGM) {
                if (i + 1 < instructions.size()) {
                    bool has_non_pad = false;
                    for (size_t k = i + 1; k < instructions.size(); ++k) {
                        if (instructions[k].opcode != Rdna2Opcode::S_CODE_END &&
                            instructions[k].opcode != Rdna2Opcode::S_NOP) {
                            has_non_pad = true;
                            break;
                        }
                    }
                    if (has_non_pad) {
                        leader_set.insert(i + 1);
                    }
                }
            } else {
                // Any other scalar write invalidates tracked constant SGPR state
                if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
                    sgpr_has_val[inst.dst.reg_index] = false;
                }
            }
        }
    }

    std::vector<size_t> leaders(leader_set.begin(), leader_set.end());
    std::unordered_map<size_t, uint32_t> inst_idx_to_bb;
    out_module.blocks.resize(leaders.size());
    for (size_t b = 0; b < leaders.size(); ++b) {
        out_module.blocks[b].id = static_cast<uint32_t>(b);
        out_module.blocks[b].start_word_offset = instructions[leaders[b]].word_offset;
        inst_idx_to_bb[leaders[b]] = static_cast<uint32_t>(b);
    }

    // Build CFG edges between basic blocks.
    for (size_t b = 0; b < leaders.size(); ++b) {
        const size_t end_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        const size_t last_i = end_i - 1;
        const auto& last = instructions[last_i];

        if (last.opcode == Rdna2Opcode::S_ENDPGM) {
            // Terminal block
        } else if (last.opcode == Rdna2Opcode::S_BRANCH) {
            const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
        } else if (call_target_idx.count(last_i) > 0u) {
            const uint32_t tgt_bb = inst_idx_to_bb[call_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
        } else if (last.opcode == Rdna2Opcode::S_SETPC_B64 &&
                   (setpc_target_idx.count(last_i) > 0u || !call_return_sites.empty())) {
            if (setpc_target_idx.count(last_i) > 0u) {
                const uint32_t tgt_bb = inst_idx_to_bb[setpc_target_idx[last_i]];
                AddUnique(out_module.blocks[b].successors, tgt_bb);
                AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
            } else {
                for (const auto& rs : call_return_sites) {
                    const uint32_t ret_bb = inst_idx_to_bb[rs.second];
                    AddUnique(out_module.blocks[b].successors, ret_bb);
                    AddUnique(out_module.blocks[ret_bb].predecessors, static_cast<uint32_t>(b));
                }
            }
        } else if (IsConditionalBranchOpcode(last.opcode)) {
            const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
            if (b + 1 < leaders.size()) {
                const uint32_t fall_bb = static_cast<uint32_t>(b + 1);
                AddUnique(out_module.blocks[b].successors, fall_bb);
                AddUnique(out_module.blocks[fall_bb].predecessors, static_cast<uint32_t>(b));
            }
        } else if (b + 1 < leaders.size()) {
            const uint32_t fall_bb = static_cast<uint32_t>(b + 1);
            AddUnique(out_module.blocks[b].successors, fall_bb);
            AddUnique(out_module.blocks[fall_bb].predecessors, static_cast<uint32_t>(b));
        }
    }

    // 2. Entry-block hoisted input/constant definitions so they dominate all blocks.
    std::vector<IrInstruction> entry_preamble;
    std::unordered_map<IrValueId, IrType> value_types;

    auto register_preamble = [&](IrOp op, IrType type, uint32_t imm_u = 0, float imm_f = 0.0f) -> IrValueId {
        IrInstruction inst{};
        inst.id = out_module.AllocateValueId();
        inst.op = op;
        inst.type = type;
        inst.imm_u32 = imm_u;
        inst.imm_f32 = imm_f;
        value_types[inst.id] = type;
        entry_preamble.push_back(std::move(inst));
        return entry_preamble.back().id;
    };

    IrValueId tid_x_val = kInvalidIrValue;
    IrValueId mesh_gtid_val = kInvalidIrValue;
    IrValueId merged_wave_info_val = kInvalidIrValue;
    IrValueId zero_u32_val = kInvalidIrValue;
    IrValueId zero_f32_val = kInvalidIrValue;
    IrValueId true_bool_val = kInvalidIrValue;
    IrValueId false_bool_val = kInvalidIrValue;
    std::array<IrValueId, 16> ps_inputs{};
    std::unordered_map<uint32_t, IrValueId> root_const_vals;

    auto get_zero_u32 = [&]() -> IrValueId {
        if (zero_u32_val == kInvalidIrValue) {
            zero_u32_val = register_preamble(IrOp::ConstU32, IrType::U32, 0u, 0.0f);
        }
        return zero_u32_val;
    };

    auto get_zero_f32 = [&]() -> IrValueId {
        if (zero_f32_val == kInvalidIrValue) {
            zero_f32_val = register_preamble(IrOp::ConstF32, IrType::F32, 0u, 0.0f);
        }
        return zero_f32_val;
    };

    auto get_true_bool = [&]() -> IrValueId {
        if (true_bool_val == kInvalidIrValue) {
            true_bool_val = register_preamble(IrOp::ConstU32, IrType::Bool, 1u, 0.0f);
        }
        return true_bool_val;
    };

    auto get_false_bool = [&]() -> IrValueId {
        if (false_bool_val == kInvalidIrValue) {
            false_bool_val = register_preamble(IrOp::ConstU32, IrType::Bool, 0u, 0.0f);
        }
        return false_bool_val;
    };

    auto get_tid_x = [&]() -> IrValueId {
        if (tid_x_val == kInvalidIrValue) {
            tid_x_val = register_preamble(IrOp::ThreadIdX, IrType::U32, 0u, 0.0f);
        }
        return tid_x_val;
    };

    auto get_mesh_global_tid = [&]() -> IrValueId {
        if (mesh_gtid_val == kInvalidIrValue) {
            out_module.uses_mesh_global_tid = true;
            // imm_u32 carries registered NumThreads so v5 = group * NumThreads + lane
            mesh_gtid_val = register_preamble(IrOp::MeshGlobalThreadId, IrType::U32, out_module.mesh_num_threads, 0.0f);
        }
        return mesh_gtid_val;
    };

    auto get_merged_wave_info = [&]() -> IrValueId {
        if (merged_wave_info_val == kInvalidIrValue) {
            out_module.uses_merged_wave_info = true;
            merged_wave_info_val = register_preamble(IrOp::MergedWaveInfo, IrType::U32, 0u, 0.0f);
        }
        return merged_wave_info_val;
    };

    auto get_ps_input = [&](uint32_t ch, uint32_t param_slot = 0u) -> IrValueId {
        const uint32_t slot = (param_slot & 3u) * 4u + (ch & 3u);
        if (slot < 16u && ps_inputs[slot] == kInvalidIrValue) {
            const uint32_t imm = (ch & 3u) | ((param_slot & 3u) << 2);
            ps_inputs[slot] = register_preamble(IrOp::StageInput, IrType::F32, imm, 0.0f);
        }
        return slot < 16u ? ps_inputs[slot] : get_zero_f32();
    };

    auto get_root_const = [&](uint32_t idx) -> IrValueId {
        auto it = root_const_vals.find(idx);
        if (it != root_const_vals.end()) return it->second;
        out_module.uses_root_constants = true;
        if (idx > out_module.max_root_const_index) {
            out_module.max_root_const_index = idx;
        }
        IrValueId v = register_preamble(IrOp::LoadRootConst, IrType::U32, idx, 0.0f);
        root_const_vals[idx] = v;
        return v;
    };

    auto get_type = [&](IrValueId id) -> IrType {
        auto it = value_types.find(id);
        return it != value_types.end() ? it->second : IrType::U32;
    };

    // 3. Lower each basic block into SSA IR (with back-edge / irreducible loop-carried Phi patching).
    std::vector<RegState> exit_states(leaders.size());
    std::vector<bool> block_processed(leaders.size(), false);
    uint32_t max_scratch_byte_end = 0u;

    // Track which registers are written in each block so loop headers pre-allocate loop-carried Phis.
    struct BlockWrites {
        std::array<uint8_t, 256> vgpr{};
        std::array<uint8_t, 106> sgpr{};
        bool scc = false;
        bool vcc = false;
        bool exec = false;
        bool m0 = false;
    };
    std::vector<BlockWrites> block_writes(leaders.size());
    for (size_t b = 0; b < leaders.size(); ++b) {
        const size_t s_i = leaders[b];
        const size_t e_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        auto& bw = block_writes[b];
        for (size_t i = s_i; i < e_i; ++i) {
            const auto& ins = instructions[i];
            auto mark_dst = [&](const Rdna2Operand& d) {
                if (d.kind == Rdna2OperandKind::Vgpr && d.reg_index < 256u) bw.vgpr[d.reg_index] = 1u;
                else if (d.kind == Rdna2OperandKind::Sgpr && d.reg_index < 106u) bw.sgpr[d.reg_index] = 1u;
                else if (d.kind == Rdna2OperandKind::VccLo || d.kind == Rdna2OperandKind::VccHi) bw.vcc = true;
                else if (d.kind == Rdna2OperandKind::ExecLo || d.kind == Rdna2OperandKind::ExecHi) bw.exec = true;
                else if (d.kind == Rdna2OperandKind::M0) bw.m0 = true;
                else if (d.kind == Rdna2OperandKind::Scc) bw.scc = true;
            };
            mark_dst(ins.dst);
            if (IsExtendedSaveExecSop1(ins)) {
                bw.exec = true;
                bw.scc = true;
            } else if (IsExtendedExecSop2(ins)) {
                bw.scc = true;
            } else if (IsVcmpxOpcode(ins.opcode)) {
                bw.exec = true;
            } else if (ins.encoding == Rdna2Encoding::SOPC ||
                       ins.opcode == Rdna2Opcode::S_AND_B32 || ins.opcode == Rdna2Opcode::S_OR_B32 ||
                       ins.opcode == Rdna2Opcode::S_XOR_B32 || ins.opcode == Rdna2Opcode::S_ANDN2_B32) {
                bw.scc = true;
            }
            if ((ins.opcode == Rdna2Opcode::IMAGE_LOAD || ins.opcode == Rdna2Opcode::IMAGE_LOAD_MIP ||
                 ins.opcode == Rdna2Opcode::IMAGE_SAMPLE || ins.opcode == Rdna2Opcode::IMAGE_SAMPLE_LZ ||
                 ins.opcode == Rdna2Opcode::IMAGE_SAMPLE_C_LZ ||
                 ins.opcode == Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY) &&
                ins.dst.kind == Rdna2OperandKind::Vgpr) {
                for (uint32_t k = 0u; k < 4u && ins.dst.reg_index + k < 256u; ++k) {
                    bw.vgpr[ins.dst.reg_index + k] = 1u;
                }
            }
        }
    }

    enum class PendingRegKind : uint8_t { Vgpr, Sgpr, Scc, Vcc, Exec, M0 };
    struct PendingBackEdgePhi {
        uint32_t bb_id = 0;
        size_t inst_index = 0;
        PendingRegKind kind = PendingRegKind::Vgpr;
        uint16_t reg_index = 0;
    };
    std::vector<PendingBackEdgePhi> pending_loop_phis;

    for (size_t b = 0; b < leaders.size(); ++b) {
        auto& bb = out_module.blocks[b];
        RegState state{};

        auto emit_in_bb = [&](IrOp op, IrType type, std::vector<IrValueId> ops = {},
                              uint32_t imm_u = 0, float imm_f = 0.0f) -> IrValueId {
            IrInstruction inst{};
            inst.op = op;
            inst.type = type;
            inst.operands = std::move(ops);
            inst.imm_u32 = imm_u;
            inst.imm_f32 = imm_f;
            if (type != IrType::Void) {
                inst.id = out_module.AllocateValueId();
                value_types[inst.id] = type;
            }
            bb.instructions.push_back(std::move(inst));
            return bb.instructions.back().id;
        };

        // Merge register state from predecessors (emitting Phi nodes at join & loop-header blocks).
        std::vector<uint32_t> ready_preds;
        bool has_unprocessed_pred = false;
        for (uint32_t p : bb.predecessors) {
            if (block_processed[p]) ready_preds.push_back(p);
            else has_unprocessed_pred = true;
        }

        if (!has_unprocessed_pred) {
            if (ready_preds.size() == 1) {
                state = exit_states[ready_preds[0]];
            } else if (ready_preds.size() > 1) {
                auto merge_reg = [&](auto getter) -> IrValueId {
                    IrValueId first = kInvalidIrValue;
                    bool differs = false;
                    bool any_set = false;
                    for (uint32_t p : ready_preds) {
                        IrValueId v = getter(exit_states[p]);
                        if (v != kInvalidIrValue) any_set = true;
                        if (first == kInvalidIrValue) {
                            first = v;
                        } else if (v != first) {
                            differs = true;
                        }
                    }
                    if (!any_set) return kInvalidIrValue;
                    if (!differs) return first;

                    IrType phi_type = IrType::U32;
                    for (uint32_t p : ready_preds) {
                        IrValueId v = getter(exit_states[p]);
                        if (v != kInvalidIrValue) {
                            phi_type = get_type(v);
                            break;
                        }
                    }

                    IrInstruction phi{};
                    phi.id = out_module.AllocateValueId();
                    phi.op = IrOp::Phi;
                    phi.type = phi_type;
                    for (uint32_t p : ready_preds) {
                        IrValueId v = getter(exit_states[p]);
                        if (v == kInvalidIrValue) {
                            v = (phi_type == IrType::F32)
                                    ? get_zero_f32()
                                    : (phi_type == IrType::Bool ? get_true_bool() : get_zero_u32());
                        }
                        phi.phi_incoming.emplace_back(p, v);
                    }
                    value_types[phi.id] = phi_type;
                    bb.instructions.push_back(std::move(phi));
                    return bb.instructions.back().id;
                };

                for (size_t r = 0; r < 256; ++r) {
                    state.vgpr[r] = merge_reg([r](const RegState& s) { return s.vgpr[r]; });
                }
                for (size_t r = 0; r < 106; ++r) {
                    state.sgpr[r] = merge_reg([r](const RegState& s) { return s.sgpr[r]; });
                }
                state.scc = merge_reg([](const RegState& s) { return s.scc; });
                state.vcc = merge_reg([](const RegState& s) { return s.vcc; });
                state.exec = merge_reg([](const RegState& s) { return s.exec; });
                state.m0 = merge_reg([](const RegState& s) { return s.m0; });
            }
        } else {
            // Loop header or irreducible multi-entry header: compute registers written in blocks >= b
            BlockWrites loop_mod{};
            for (size_t k = b; k < leaders.size(); ++k) {
                for (size_t r = 0; r < 256; ++r) loop_mod.vgpr[r] |= block_writes[k].vgpr[r];
                for (size_t r = 0; r < 106; ++r) loop_mod.sgpr[r] |= block_writes[k].sgpr[r];
                loop_mod.scc = loop_mod.scc || block_writes[k].scc;
                loop_mod.vcc = loop_mod.vcc || block_writes[k].vcc;
                loop_mod.exec = loop_mod.exec || block_writes[k].exec;
                loop_mod.m0 = loop_mod.m0 || block_writes[k].m0;
            }

            auto make_loop_phi = [&](PendingRegKind kind, uint16_t reg_idx, bool written_in_loop,
                                     auto getter, IrType default_type) -> IrValueId {
                IrValueId first = kInvalidIrValue;
                bool differs = false;
                bool any_set = false;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v != kInvalidIrValue) any_set = true;
                    if (first == kInvalidIrValue) first = v;
                    else if (v != first) differs = true;
                }
                if (!written_in_loop && !differs) {
                    return first;
                }
                if (!any_set && !written_in_loop) {
                    return kInvalidIrValue;
                }
                IrType phi_type = default_type;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v != kInvalidIrValue) {
                        phi_type = get_type(v);
                        break;
                    }
                }
                IrInstruction phi{};
                phi.id = out_module.AllocateValueId();
                phi.op = IrOp::Phi;
                phi.type = phi_type;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v == kInvalidIrValue) {
                        if (kind == PendingRegKind::Exec) {
                            v = get_true_bool();
                        } else if (kind == PendingRegKind::Vgpr && reg_idx == 0u &&
                                   (out_module.stage == ShaderStage::Compute || out_module.stage == ShaderStage::Vertex ||
                                    out_module.stage == ShaderStage::Geometry || out_module.stage == ShaderStage::Mesh ||
                                    out_module.stage == ShaderStage::Amplification)) {
                            v = get_tid_x();
                        } else if (kind == PendingRegKind::Vgpr && reg_idx < 4u &&
                                   out_module.stage == ShaderStage::Pixel) {
                            v = get_ps_input(reg_idx);
                        } else if (phi_type == IrType::F32) {
                            v = get_zero_f32();
                        } else if (phi_type == IrType::Bool) {
                            v = get_false_bool();
                        } else {
                            v = get_zero_u32();
                        }
                    }
                    phi.phi_incoming.emplace_back(p, v);
                }
                value_types[phi.id] = phi_type;
                const size_t idx = bb.instructions.size();
                bb.instructions.push_back(std::move(phi));
                pending_loop_phis.push_back({static_cast<uint32_t>(b), idx, kind, reg_idx});
                return bb.instructions[idx].id;
            };

            for (size_t r = 0; r < 256; ++r) {
                state.vgpr[r] = make_loop_phi(PendingRegKind::Vgpr, static_cast<uint16_t>(r),
                                              loop_mod.vgpr[r] != 0u,
                                              [r](const RegState& s) { return s.vgpr[r]; }, IrType::U32);
            }
            for (size_t r = 0; r < 106; ++r) {
                state.sgpr[r] = make_loop_phi(PendingRegKind::Sgpr, static_cast<uint16_t>(r),
                                              loop_mod.sgpr[r] != 0u,
                                              [r](const RegState& s) { return s.sgpr[r]; }, IrType::U32);
            }
            state.scc = make_loop_phi(PendingRegKind::Scc, 0u, loop_mod.scc,
                                      [](const RegState& s) { return s.scc; }, IrType::Bool);
            state.vcc = make_loop_phi(PendingRegKind::Vcc, 0u, loop_mod.vcc,
                                      [](const RegState& s) { return s.vcc; }, IrType::Bool);
            state.exec = make_loop_phi(PendingRegKind::Exec, 0u, loop_mod.exec,
                                       [](const RegState& s) { return s.exec; }, IrType::Bool);
            state.m0 = make_loop_phi(PendingRegKind::M0, 0u, loop_mod.m0,
                                     [](const RegState& s) { return s.m0; }, IrType::U32);
        }

        auto read_vgpr = [&](uint8_t idx, bool expect_float) -> IrValueId {
            if (state.vgpr[idx] != kInvalidIrValue) return state.vgpr[idx];
            if (out_module.stage == ShaderStage::Pixel && idx < 4u) {
                state.vgpr[idx] = get_ps_input(idx);
                return state.vgpr[idx];
            }
            if (out_module.stage == ShaderStage::Mesh && idx == 5u) {
                // Mesh v5 workgroup seeding: group * NumThreads + lane (using pre-registered NumThreads)
                state.vgpr[idx] = get_mesh_global_tid();
                return state.vgpr[idx];
            }
            if ((out_module.stage == ShaderStage::Compute || out_module.stage == ShaderStage::Vertex ||
                 out_module.stage == ShaderStage::Geometry ||
                 out_module.stage == ShaderStage::Mesh || out_module.stage == ShaderStage::Amplification) &&
                idx == 0u) {
                state.vgpr[idx] = get_tid_x();
                return state.vgpr[idx];
            }
            return expect_float ? get_zero_f32() : get_zero_u32();
        };

        auto read_sgpr = [&](uint8_t idx) -> IrValueId {
            if (state.sgpr[idx] != kInvalidIrValue) return state.sgpr[idx];
            if ((out_module.stage == ShaderStage::Mesh || out_module.stage == ShaderStage::Geometry) &&
                idx == 3u) {
                // GFX10.3 merged_wave_info in s3
                state.sgpr[idx] = get_merged_wave_info();
                return state.sgpr[idx];
            }
            if (idx < 16u) {
                state.sgpr[idx] = get_root_const(idx);
                return state.sgpr[idx];
            }
            return get_zero_u32();
        };

        // Per-block cache for 64-lane ballot masks derived from a predicate value.
        std::unordered_map<IrValueId, std::pair<IrValueId, IrValueId>> ballot_cache;
        auto get_ballot_pair = [&](IrValueId pred_bool) -> std::pair<IrValueId, IrValueId> {
            auto it = ballot_cache.find(pred_bool);
            if (it != ballot_cache.end()) return it->second;
            out_module.uses_wave64_emulation = true;
            IrValueId lo = emit_in_bb(IrOp::WaveBallotLo, IrType::U32, {pred_bool});
            IrValueId hi = emit_in_bb(IrOp::WaveBallotHi, IrType::U32, {pred_bool});
            ballot_cache[pred_bool] = {lo, hi};
            return {lo, hi};
        };

        auto eval_operand = [&](const Rdna2Operand& op, bool expect_float) -> IrValueId {
            IrValueId val = kInvalidIrValue;
            switch (op.kind) {
                case Rdna2OperandKind::Vgpr:
                    val = read_vgpr(static_cast<uint8_t>(op.reg_index), expect_float);
                    break;
                case Rdna2OperandKind::Sgpr:
                    val = read_sgpr(static_cast<uint8_t>(op.reg_index));
                    break;
                case Rdna2OperandKind::InlineInt:
                    if (expect_float && op.int_value == 0) {
                        val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(op.int_value), 0.0f);
                    }
                    break;
                case Rdna2OperandKind::InlineFloat:
                    val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, op.literal_bits, op.float_value);
                    break;
                case Rdna2OperandKind::Literal32:
                    if (expect_float) {
                        val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, op.literal_bits, BitsToFloat(op.literal_bits));
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, op.literal_bits, 0.0f);
                    }
                    break;
                case Rdna2OperandKind::VccLo:
                case Rdna2OperandKind::VccHi:
                    val = (state.vcc != kInvalidIrValue) ? state.vcc : get_zero_u32();
                    break;
                case Rdna2OperandKind::ExecLo:
                    if (state.exec != kInvalidIrValue) {
                        val = (get_type(state.exec) == IrType::Bool)
                                  ? get_ballot_pair(state.exec).first
                                  : state.exec;
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    }
                    break;
                case Rdna2OperandKind::ExecHi:
                    if (state.exec != kInvalidIrValue && get_type(state.exec) == IrType::Bool) {
                        val = (out_module.wave_size == 64u)
                                  ? get_ballot_pair(state.exec).second
                                  : get_zero_u32();
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                         (out_module.wave_size == 64u) ? 0xFFFFFFFFu : 0u, 0.0f);
                    }
                    break;
                case Rdna2OperandKind::M0:
                    val = (state.m0 != kInvalidIrValue) ? state.m0 : get_zero_u32();
                    break;
                case Rdna2OperandKind::Scc:
                    val = (state.scc != kInvalidIrValue) ? state.scc : get_zero_u32();
                    break;
                default:
                    val = expect_float ? get_zero_f32() : get_zero_u32();
                    break;
            }
            if (expect_float) {
                if (op.abs) {
                    IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    IrValueId neg_v = emit_in_bb(IrOp::FSub, IrType::F32, {z, val});
                    val = emit_in_bb(IrOp::FMax, IrType::F32, {val, neg_v});
                }
                if (op.neg) {
                    IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    val = emit_in_bb(IrOp::FSub, IrType::F32, {z, val});
                }
            }
            return val;
        };

        auto apply_clamp = [&](IrValueId val, bool clamp) -> IrValueId {
            if (!clamp) return val;
            IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
            IrValueId o = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
            IrValueId lo = emit_in_bb(IrOp::FMax, IrType::F32, {val, z});
            return emit_in_bb(IrOp::FMin, IrType::F32, {lo, o});
        };

        auto ensure_bool = [&](IrValueId v) -> IrValueId {
            if (v == kInvalidIrValue) {
                IrValueId z = get_zero_u32();
                return emit_in_bb(IrOp::CmpNe, IrType::Bool, {z, z});
            }
            if (get_type(v) == IrType::Bool) return v;
            IrValueId z = get_zero_u32();
            return emit_in_bb(IrOp::CmpNe, IrType::Bool, {v, z});
        };

        auto write_dst = [&](const Rdna2Operand& dst, IrValueId val) {
            if (dst.kind == Rdna2OperandKind::Vgpr) {
                if (state.exec != kInvalidIrValue && get_type(state.exec) == IrType::Bool) {
                    const IrType vt = get_type(val);
                    IrValueId prev = (state.vgpr[dst.reg_index] != kInvalidIrValue)
                                         ? state.vgpr[dst.reg_index]
                                         : (vt == IrType::F32 ? get_zero_f32() : get_zero_u32());
                    state.vgpr[dst.reg_index] = emit_in_bb(IrOp::Select, vt, {state.exec, val, prev});
                } else {
                    state.vgpr[dst.reg_index] = val;
                }
            } else if (dst.kind == Rdna2OperandKind::Sgpr) {
                state.sgpr[dst.reg_index] = val;
            } else if (dst.kind == Rdna2OperandKind::VccLo) {
                state.vcc = val;
            } else if (dst.kind == Rdna2OperandKind::ExecLo) {
                state.exec = val;
            } else if (dst.kind == Rdna2OperandKind::M0) {
                state.m0 = val;
            } else if (dst.kind == Rdna2OperandKind::Scc) {
                state.scc = val;
            }
        };

        auto attach_exec_pred = [&]() {
            if (state.exec != kInvalidIrValue && get_type(state.exec) == IrType::Bool && !bb.instructions.empty()) {
                bb.instructions.back().exec_pred = state.exec;
            }
        };

        auto is_lane_pred_operand = [&](const Rdna2Operand& op) -> bool {
            if (op.kind == Rdna2OperandKind::ExecLo || op.kind == Rdna2OperandKind::ExecHi ||
                op.kind == Rdna2OperandKind::VccLo || op.kind == Rdna2OperandKind::VccHi) {
                return true;
            }
            if (op.kind == Rdna2OperandKind::Sgpr && op.reg_index < 106u &&
                state.sgpr[op.reg_index] != kInvalidIrValue &&
                get_type(state.sgpr[op.reg_index]) == IrType::Bool) {
                return true;
            }
            return false;
        };

        auto eval_lane_pred = [&](const Rdna2Operand& op) -> IrValueId {
            if (op.kind == Rdna2OperandKind::ExecLo || op.kind == Rdna2OperandKind::ExecHi) {
                if (state.exec == kInvalidIrValue) return get_true_bool();
                return ensure_bool(state.exec);
            }
            if (op.kind == Rdna2OperandKind::VccLo || op.kind == Rdna2OperandKind::VccHi) {
                return ensure_bool(state.vcc);
            }
            if (op.kind == Rdna2OperandKind::Sgpr && op.reg_index < 106u) {
                if (state.sgpr[op.reg_index] != kInvalidIrValue) {
                    return ensure_bool(state.sgpr[op.reg_index]);
                }
                return get_false_bool();
            }
            if (op.kind == Rdna2OperandKind::InlineInt) {
                return (op.int_value != 0) ? get_true_bool() : get_false_bool();
            }
            if (op.kind == Rdna2OperandKind::Literal32) {
                return (op.literal_bits != 0u) ? get_true_bool() : get_false_bool();
            }
            return ensure_bool(eval_operand(op, false));
        };

        auto emit_bool_or = [&](IrValueId a, IrValueId b_val) -> IrValueId {
            IrValueId na = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {a});
            IrValueId nb = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {b_val});
            IrValueId nand = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {na, nb});
            return emit_in_bb(IrOp::LogicalNot, IrType::Bool, {nand});
        };

        auto emit_bool_xor = [&](IrValueId a, IrValueId b_val) -> IrValueId {
            IrValueId na = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {a});
            IrValueId nb = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {b_val});
            IrValueId a_nb = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {a, nb});
            IrValueId na_b = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {na, b_val});
            return emit_bool_or(a_nb, na_b);
        };

        auto emit_pixel_kill = [&](IrValueId kill_cond_or_invalid) {
            if (out_module.stage != ShaderStage::Pixel) return;
            IrInstruction kill_inst{};
            kill_inst.op = IrOp::Export;
            kill_inst.type = IrType::Void;
            kill_inst.export_target = static_cast<uint8_t>(Rdna2ExportTarget::Null);
            kill_inst.export_mask = 0u;
            kill_inst.imm_u32 = kIrKillExportMagic;
            if (kill_cond_or_invalid != kInvalidIrValue) {
                kill_inst.operands.push_back(kill_cond_or_invalid);
            }
            bb.instructions.push_back(std::move(kill_inst));
        };

        const size_t start_i = leaders[b];
        const size_t end_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        bool terminated = false;

        for (size_t i = start_i; i < end_i; ++i) {
            const auto& inst = instructions[i];

            // Extended GFX10.3 64-bit / 32-bit EXEC-mask manipulation instructions
            if (IsExtendedSaveExecSop1(inst)) {
                const IrValueId old_exec = eval_lane_pred(Rdna2Operand::ExecLo());
                const IrValueId rhs = eval_lane_pred(inst.src[0]);
                write_dst(inst.dst, old_exec);
                IrValueId new_exec = old_exec;
                if (inst.raw_opcode == kSop1RawAndSaveExecB64 || inst.raw_opcode == kSop1RawAndSaveExecB32) {
                    new_exec = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {old_exec, rhs});
                } else if (inst.raw_opcode == kSop1RawOrSaveExecB64 || inst.raw_opcode == kSop1RawOrSaveExecB32) {
                    new_exec = emit_bool_or(old_exec, rhs);
                } else if (inst.raw_opcode == kSop1RawXorSaveExecB64 || inst.raw_opcode == kSop1RawXorSaveExecB32) {
                    new_exec = emit_bool_xor(old_exec, rhs);
                } else {
                    IrValueId nr = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {rhs});
                    new_exec = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {old_exec, nr});
                }
                state.exec = new_exec;
                state.scc = new_exec;
                continue;
            }
            if (IsExtendedMovB64Sop1(inst)) {
                if (inst.dst.kind == Rdna2OperandKind::ExecLo || inst.dst.kind == Rdna2OperandKind::ExecHi) {
                    if ((inst.src[0].kind == Rdna2OperandKind::InlineInt && inst.src[0].int_value == -1) ||
                        (inst.src[0].kind == Rdna2OperandKind::Literal32 && inst.src[0].literal_bits == 0xFFFFFFFFu)) {
                        state.exec = get_true_bool();
                    } else if ((inst.src[0].kind == Rdna2OperandKind::InlineInt && inst.src[0].int_value == 0) ||
                               (inst.src[0].kind == Rdna2OperandKind::Literal32 && inst.src[0].literal_bits == 0u)) {
                        state.exec = get_false_bool();
                        // Folded EXECZ guard (Leonida #4574): do NOT kill pixel if followed by S_CBRANCH_EXECZ to join
                        const bool followed_by_execz =
                            (i + 1u < end_i && instructions[i + 1u].opcode == Rdna2Opcode::S_CBRANCH_EXECZ);
                        if (!followed_by_execz) {
                            emit_pixel_kill(kInvalidIrValue);
                        }
                    } else {
                        state.exec = eval_lane_pred(inst.src[0]);
                    }
                } else if (is_lane_pred_operand(inst.src[0])) {
                    write_dst(inst.dst, eval_lane_pred(inst.src[0]));
                } else {
                    write_dst(inst.dst, eval_operand(inst.src[0], false));
                }
                continue;
            }
            if (IsExtendedExecSop2(inst)) {
                const IrValueId a = eval_lane_pred(inst.src[0]);
                const IrValueId bval = eval_lane_pred(inst.src[1]);
                IrValueId res = a;
                if (inst.raw_opcode == kSop2RawAndB64) {
                    res = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {a, bval});
                } else if (inst.raw_opcode == kSop2RawOrB64) {
                    res = emit_bool_or(a, bval);
                } else if (inst.raw_opcode == kSop2RawXorB64) {
                    res = emit_bool_xor(a, bval);
                } else {
                    IrValueId nb = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {bval});
                    res = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {a, nb});
                }
                write_dst(inst.dst, res);
                state.scc = res;
                continue;
            }

            switch (inst.opcode) {
                // ----- SOP2 -----
                case Rdna2Opcode::S_ADD_U32: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId sum = emit_in_bb(IrOp::IAdd, IrType::U32, {a, bval});
                    write_dst(inst.dst, sum);
                    state.scc = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {sum, a});
                    break;
                }
                case Rdna2Opcode::S_SUB_U32: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId diff = emit_in_bb(IrOp::ISub, IrType::U32, {a, bval});
                    write_dst(inst.dst, diff);
                    state.scc = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {a, bval});
                    break;
                }
                case Rdna2Opcode::S_ADDC_U32: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId c_one = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                    IrValueId carry_u = emit_in_bb(IrOp::Select, IrType::U32, {ensure_bool(state.scc), c_one, get_zero_u32()});
                    IrValueId s1 = emit_in_bb(IrOp::IAdd, IrType::U32, {a, bval});
                    IrValueId s2 = emit_in_bb(IrOp::IAdd, IrType::U32, {s1, carry_u});
                    write_dst(inst.dst, s2);
                    IrValueId c1 = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {s1, a});
                    IrValueId c2 = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {s2, s1});
                    state.scc = emit_bool_or(c1, c2);
                    break;
                }
                case Rdna2Opcode::S_SUBB_U32: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId c_one = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                    IrValueId borrow_u = emit_in_bb(IrOp::Select, IrType::U32, {ensure_bool(state.scc), c_one, get_zero_u32()});
                    IrValueId d1 = emit_in_bb(IrOp::ISub, IrType::U32, {a, bval});
                    IrValueId d2 = emit_in_bb(IrOp::ISub, IrType::U32, {d1, borrow_u});
                    write_dst(inst.dst, d2);
                    IrValueId b1 = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {a, bval});
                    IrValueId b2 = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {d1, borrow_u});
                    state.scc = emit_bool_or(b1, b2);
                    break;
                }
                case Rdna2Opcode::S_ABSDIFF_I32: {
                    IrValueId r = emit_in_bb(IrOp::AbsDiffI32, IrType::U32,
                                             {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    write_dst(inst.dst, r);
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {r, get_zero_u32()});
                    break;
                }
                case Rdna2Opcode::S_ADD_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_SUB_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MIN_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MIN_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MAX_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MAX_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_CSELECT_B32: {
                    IrValueId c = ensure_bool(state.scc);
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Select, get_type(a), {c, a, bval}));
                    break;
                }
                case Rdna2Opcode::S_AND_B32:
                    if (inst.dst.kind == Rdna2OperandKind::ExecLo || inst.dst.kind == Rdna2OperandKind::ExecHi ||
                        is_lane_pred_operand(inst.src[0]) || is_lane_pred_operand(inst.src[1])) {
                        IrValueId r = emit_in_bb(IrOp::LogicalAnd, IrType::Bool,
                                                 {eval_lane_pred(inst.src[0]), eval_lane_pred(inst.src[1])});
                        write_dst(inst.dst, r);
                        state.scc = r;
                    } else {
                        write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    }
                    break;
                case Rdna2Opcode::S_OR_B32:
                    if (inst.dst.kind == Rdna2OperandKind::ExecLo || inst.dst.kind == Rdna2OperandKind::ExecHi ||
                        is_lane_pred_operand(inst.src[0]) || is_lane_pred_operand(inst.src[1])) {
                        IrValueId r = emit_bool_or(eval_lane_pred(inst.src[0]), eval_lane_pred(inst.src[1]));
                        write_dst(inst.dst, r);
                        state.scc = r;
                    } else {
                        write_dst(inst.dst, emit_in_bb(IrOp::BitOr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    }
                    break;
                case Rdna2Opcode::S_XOR_B32:
                    if (inst.dst.kind == Rdna2OperandKind::ExecLo || inst.dst.kind == Rdna2OperandKind::ExecHi ||
                        is_lane_pred_operand(inst.src[0]) || is_lane_pred_operand(inst.src[1])) {
                        IrValueId r = emit_bool_xor(eval_lane_pred(inst.src[0]), eval_lane_pred(inst.src[1]));
                        write_dst(inst.dst, r);
                        state.scc = r;
                    } else {
                        write_dst(inst.dst, emit_in_bb(IrOp::BitXor, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    }
                    break;
                case Rdna2Opcode::S_ANDN2_B32: {
                    if (inst.dst.kind == Rdna2OperandKind::ExecLo || inst.dst.kind == Rdna2OperandKind::ExecHi ||
                        is_lane_pred_operand(inst.src[0]) || is_lane_pred_operand(inst.src[1])) {
                        IrValueId a = eval_lane_pred(inst.src[0]);
                        IrValueId bval = eval_lane_pred(inst.src[1]);
                        IrValueId nb = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {bval});
                        IrValueId r = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {a, nb});
                        write_dst(inst.dst, r);
                        state.scc = r;
                    } else {
                        IrValueId a = eval_operand(inst.src[0], false);
                        IrValueId bval = eval_operand(inst.src[1], false);
                        IrValueId nb = emit_in_bb(IrOp::BitNot, IrType::U32, {bval});
                        write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {a, nb}));
                    }
                    break;
                }
                case Rdna2Opcode::S_LSHL_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shl, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_LSHR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_ASHR_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_BFE_U32:
                case Rdna2Opcode::S_BFE_I32: {
                    IrValueId s0 = eval_operand(inst.src[0], false);
                    IrValueId s1 = eval_operand(inst.src[1], false);
                    IrValueId c_m31 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 31u, 0.0f);
                    IrValueId c_sh16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 16u, 0.0f);
                    IrValueId c_m7f = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0x7Fu, 0.0f);
                    IrValueId off = emit_in_bb(IrOp::BitAnd, IrType::U32, {s1, c_m31});
                    IrValueId s1_hi = emit_in_bb(IrOp::Shr, IrType::U32, {s1, c_sh16});
                    IrValueId wid = emit_in_bb(IrOp::BitAnd, IrType::U32, {s1_hi, c_m7f});
                    const bool is_signed = (inst.opcode == Rdna2Opcode::S_BFE_I32);
                    IrValueId r = emit_in_bb(is_signed ? IrOp::BfeI32 : IrOp::BfeU32,
                                             is_signed ? IrType::I32 : IrType::U32,
                                             {s0, off, wid}, 1u);
                    write_dst(inst.dst, r);
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {r, get_zero_u32()});
                    break;
                }
                case Rdna2Opcode::S_MUL_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;

                // ----- SOP1 -----
                case Rdna2Opcode::S_MOV_B32: {
                    if (inst.dst.kind == Rdna2OperandKind::ExecLo || inst.dst.kind == Rdna2OperandKind::ExecHi) {
                        if ((inst.src[0].kind == Rdna2OperandKind::InlineInt && inst.src[0].int_value == -1) ||
                            (inst.src[0].kind == Rdna2OperandKind::Literal32 && inst.src[0].literal_bits == 0xFFFFFFFFu)) {
                            state.exec = get_true_bool();
                        } else if ((inst.src[0].kind == Rdna2OperandKind::InlineInt && inst.src[0].int_value == 0) ||
                                   (inst.src[0].kind == Rdna2OperandKind::Literal32 && inst.src[0].literal_bits == 0u)) {
                            state.exec = get_false_bool();
                            const bool followed_by_execz =
                                (i + 1u < end_i && instructions[i + 1u].opcode == Rdna2Opcode::S_CBRANCH_EXECZ);
                            if (!followed_by_execz) {
                                emit_pixel_kill(kInvalidIrValue);
                            }
                        } else {
                            state.exec = eval_lane_pred(inst.src[0]);
                        }
                    } else if (is_lane_pred_operand(inst.src[0])) {
                        write_dst(inst.dst, eval_lane_pred(inst.src[0]));
                    } else {
                        const bool is_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                        write_dst(inst.dst, eval_operand(inst.src[0], is_flt));
                    }
                    break;
                }
                case Rdna2Opcode::S_NOT_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitNot, IrType::U32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::S_BREV_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitReverse32, IrType::U32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::S_BCNT1_I32_B32: {
                    IrValueId word = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccLo) {
                        word = get_ballot_pair(ensure_bool(state.vcc)).first;
                    } else if (inst.src[0].kind == Rdna2OperandKind::VccHi) {
                        word = get_ballot_pair(ensure_bool(state.vcc)).second;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecLo) {
                        word = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecHi) {
                        word = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                          (out_module.wave_size == 64u) ? 0xFFFFFFFFu : 0u, 0.0f);
                    } else if (inst.src[0].kind == Rdna2OperandKind::Sgpr &&
                               inst.src[0].reg_index < 106u &&
                               state.sgpr[inst.src[0].reg_index] != kInvalidIrValue &&
                               get_type(state.sgpr[inst.src[0].reg_index]) == IrType::Bool) {
                        word = get_ballot_pair(state.sgpr[inst.src[0].reg_index]).first;
                    } else {
                        word = eval_operand(inst.src[0], false);
                    }
                    IrValueId cnt = emit_in_bb(IrOp::BitCount32, IrType::U32, {word});
                    write_dst(inst.dst, cnt);
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {cnt, get_zero_u32()});
                    break;
                }
                case Rdna2Opcode::S_BCNT1_I32_B64: {
                    IrValueId lo = kInvalidIrValue;
                    IrValueId hi = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccLo || inst.src[0].kind == Rdna2OperandKind::VccHi) {
                        auto pair = get_ballot_pair(ensure_bool(state.vcc));
                        lo = pair.first;
                        hi = pair.second;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecLo || inst.src[0].kind == Rdna2OperandKind::ExecHi) {
                        lo = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                        hi = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                        (out_module.wave_size == 64u) ? 0xFFFFFFFFu : 0u, 0.0f);
                    } else if (inst.src[0].kind == Rdna2OperandKind::Sgpr) {
                        const uint8_t idx = static_cast<uint8_t>(inst.src[0].reg_index);
                        if (state.sgpr[idx] != kInvalidIrValue && get_type(state.sgpr[idx]) == IrType::Bool) {
                            auto pair = get_ballot_pair(state.sgpr[idx]);
                            lo = pair.first;
                            hi = (out_module.wave_size == 64u) ? pair.second : get_zero_u32();
                        } else {
                            lo = read_sgpr(idx);
                            hi = (idx + 1u < 106u) ? read_sgpr(static_cast<uint8_t>(idx + 1u)) : get_zero_u32();
                        }
                    } else if (inst.src[0].kind == Rdna2OperandKind::InlineInt && inst.src[0].int_value < 0) {
                        lo = eval_operand(inst.src[0], false);
                        hi = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else {
                        lo = eval_operand(inst.src[0], false);
                        hi = get_zero_u32();
                    }
                    IrValueId cnt = emit_in_bb(IrOp::WaveActiveCountBits64, IrType::U32, {lo, hi});
                    write_dst(inst.dst, cnt);
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {cnt, get_zero_u32()});
                    break;
                }
                case Rdna2Opcode::S_SETPC_B64:
                case Rdna2Opcode::S_SWAPPC_B64: {
                    // Vertex-fetch subroutine table dispatch (VS prolog mapped via stage_contract.vertex_fetches)
                    if (inst.opcode == Rdna2Opcode::S_SWAPPC_B64 &&
                        inst.dst.kind == Rdna2OperandKind::Sgpr) {
                        IrValueId ret_pc = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, (inst.word_offset + 1u) * 4u, 0.0f);
                        write_dst(inst.dst, ret_pc);
                        if (inst.dst.reg_index + 1u < 106u) {
                            state.sgpr[inst.dst.reg_index + 1u] = get_zero_u32();
                        }
                    }
                    if (!out_module.stage_contract.vertex_fetches.empty()) {
                        out_module.uses_uav = true;
                        out_module.uses_uav0 = true;
                        IrValueId vid = get_tid_x();
                        for (const auto& vf : out_module.stage_contract.vertex_fetches) {
                            const uint32_t stride_bytes = std::max(4u, static_cast<uint32_t>(vf.num_components) * 4u);
                            IrValueId c_stride = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, stride_bytes, 0.0f);
                            IrValueId base_addr = emit_in_bb(IrOp::IMul, IrType::U32, {vid, c_stride});
                            if (vf.byte_offset != 0u) {
                                IrValueId c_off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, vf.byte_offset, 0.0f);
                                base_addr = emit_in_bb(IrOp::IAdd, IrType::U32, {base_addr, c_off});
                            }
                            const uint8_t comps = std::min<uint8_t>(4u, std::max<uint8_t>(1u, vf.num_components));
                            const IrType res_t = IsFormatIntegerKind(vf.format) ? IrType::U32 : IrType::F32;
                            for (uint8_t c = 0; c < comps; ++c) {
                                const uint32_t imm = (vf.vb_slot & 0xFu) |
                                                     (static_cast<uint32_t>(static_cast<uint8_t>(vf.format) & 0x7Fu) << 4) |
                                                     (static_cast<uint32_t>(c & 0xFu) << 12);
                                state.vgpr[static_cast<uint8_t>(vf.dst_vgpr + c)] =
                                    emit_in_bb(IrOp::BufferLoadFormat, res_t, {base_addr}, imm);
                            }
                        }
                    } else if (inst.opcode == Rdna2Opcode::S_SWAPPC_B64 && call_target_idx.count(i) > 0u) {
                        IrInstruction br{};
                        br.op = IrOp::Branch;
                        br.type = IrType::Void;
                        br.true_block = inst_idx_to_bb[call_target_idx[i]];
                        bb.instructions.push_back(std::move(br));
                        terminated = true;
                    } else if (inst.opcode == Rdna2Opcode::S_SETPC_B64) {
                        if (setpc_target_idx.count(i) > 0u) {
                            IrInstruction br{};
                            br.op = IrOp::Branch;
                            br.type = IrType::Void;
                            br.true_block = inst_idx_to_bb[setpc_target_idx[i]];
                            bb.instructions.push_back(std::move(br));
                            terminated = true;
                        } else if (call_return_sites.size() == 1u) {
                            IrInstruction br{};
                            br.op = IrOp::Branch;
                            br.type = IrType::Void;
                            br.true_block = inst_idx_to_bb[call_return_sites[0].second];
                            bb.instructions.push_back(std::move(br));
                            terminated = true;
                        } else if (call_return_sites.size() > 1u) {
                            // Indirect return dispatch: compare saved return PC against known call return sites
                            IrValueId ret_addr = eval_operand(inst.src[0], false);
                            IrValueId c0 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, call_return_sites[0].first, 0.0f);
                            IrValueId eq0 = emit_in_bb(IrOp::CmpEqU, IrType::Bool, {ret_addr, c0});
                            IrInstruction cbr{};
                            cbr.op = IrOp::CondBranch;
                            cbr.type = IrType::Void;
                            cbr.operands.push_back(eq0);
                            cbr.true_block = inst_idx_to_bb[call_return_sites[0].second];
                            cbr.false_block = inst_idx_to_bb[call_return_sites[1].second];
                            bb.instructions.push_back(std::move(cbr));
                            terminated = true;
                        }
                    }
                    break;
                }
                case Rdna2Opcode::S_GETPC_B64: {
                    IrValueId pc_lo = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, (inst.word_offset + 1u) * 4u, 0.0f);
                    write_dst(inst.dst, pc_lo);
                    if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index + 1u < 106u) {
                        state.sgpr[inst.dst.reg_index + 1u] = get_zero_u32();
                    }
                    break;
                }

                // ----- SOPC -----
                case Rdna2Opcode::S_CMP_EQ_I32:
                    state.scc = emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LG_I32:
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GT_I32:
                    state.scc = emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GE_I32:
                    state.scc = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LT_I32:
                    state.scc = emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LE_I32:
                    state.scc = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[1], false), eval_operand(inst.src[0], false)});
                    break;
                case Rdna2Opcode::S_CMP_EQ_U32:
                    state.scc = emit_in_bb(IrOp::CmpEqU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LG_U32:
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GT_U32:
                    state.scc = emit_in_bb(IrOp::CmpGtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GE_U32:
                    state.scc = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LT_U32:
                    state.scc = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LE_U32:
                    state.scc = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[1], false), eval_operand(inst.src[0], false)});
                    break;

                // ----- SOPK -----
                case Rdna2Opcode::S_MOVK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    write_dst(inst.dst, emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f));
                    break;
                }
                case Rdna2Opcode::S_VERSION:
                case Rdna2Opcode::S_WAITCNT_VSCNT:
                case Rdna2Opcode::S_WAITCNT_VMCNT:
                case Rdna2Opcode::S_WAITCNT_EXPCNT:
                case Rdna2Opcode::S_WAITCNT_LGKMCNT:
                    break;
                case Rdna2Opcode::S_ADDK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    IrValueId cur = read_sgpr(static_cast<uint8_t>(inst.dst.reg_index));
                    IrValueId k = emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f);
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::I32, {cur, k}));
                    break;
                }
                case Rdna2Opcode::S_MULK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    IrValueId cur = read_sgpr(static_cast<uint8_t>(inst.dst.reg_index));
                    IrValueId k = emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f);
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::I32, {cur, k}));
                    break;
                }

                // ----- SOPP -----
                case Rdna2Opcode::S_NOP:
                case Rdna2Opcode::S_WAITCNT:
                case Rdna2Opcode::S_ICACHE_INV:
                case Rdna2Opcode::S_TTRACEDATA:
                case Rdna2Opcode::S_TTRACEDATA_IMM:
                case Rdna2Opcode::S_CODE_END:
                    break;
                case Rdna2Opcode::S_BARRIER:
                    emit_in_bb(IrOp::Barrier, IrType::Void);
                    break;
                case Rdna2Opcode::S_SENDMSG: {
                    const uint16_t msg_id = static_cast<uint16_t>(static_cast<uint16_t>(inst.simm16) & 0xFu);
                    if (out_module.stage == ShaderStage::Mesh || msg_id == kRdna2SendMsgGsAllocReq) {
                        IrValueId vcnt = kInvalidIrValue;
                        IrValueId pcnt = kInvalidIrValue;
                        if (state.m0 != kInvalidIrValue) {
                            // ISA-exact GFX10.3 GS_ALLOC_REQ field widths from M0:
                            //   M0[10:0]  (11 bits) = vertex count
                            //   M0[22:12] (11 bits) = primitive count
                            IrValueId c_mask11 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, kGsAllocReqVertMask, 0.0f);
                            IrValueId c_sh12 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, kGsAllocReqPrimShift, 0.0f);
                            vcnt = emit_in_bb(IrOp::BitAnd, IrType::U32, {state.m0, c_mask11});
                            IrValueId shifted = emit_in_bb(IrOp::Shr, IrType::U32, {state.m0, c_sh12});
                            pcnt = emit_in_bb(IrOp::BitAnd, IrType::U32, {shifted, c_mask11});
                        } else {
                            const uint32_t imm_v = (static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16)) >> 4) & 0x3Fu;
                            const uint32_t imm_p = (static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16)) >> 10) & 0x3Fu;
                            vcnt = (state.sgpr[0] != kInvalidIrValue)
                                       ? state.sgpr[0]
                                       : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, imm_v ? imm_v : 3u, 0.0f);
                            pcnt = (state.sgpr[1] != kInvalidIrValue)
                                       ? state.sgpr[1]
                                       : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, imm_p ? imm_p : 1u, 0.0f);
                        }
                        emit_in_bb(IrOp::SetMeshOutputCounts, IrType::Void, {vcnt, pcnt});
                    } else if (out_module.stage == ShaderStage::Amplification || msg_id == kRdna2SendMsgDispatchMesh) {
                        IrValueId gx = (state.sgpr[0] != kInvalidIrValue)
                                           ? state.sgpr[0]
                                           : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        IrValueId gy = (state.sgpr[1] != kInvalidIrValue)
                                           ? state.sgpr[1]
                                           : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        IrValueId gz = (state.sgpr[2] != kInvalidIrValue)
                                           ? state.sgpr[2]
                                           : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        emit_in_bb(IrOp::DispatchMeshGroups, IrType::Void, {gx, gy, gz});
                    }
                    break;
                }
                case Rdna2Opcode::S_ENDPGM:
                    emit_in_bb(IrOp::Return, IrType::Void);
                    terminated = true;
                    break;
                case Rdna2Opcode::S_BRANCH: {
                    IrInstruction br{};
                    br.op = IrOp::Branch;
                    br.type = IrType::Void;
                    br.true_block = inst_idx_to_bb[branch_target_idx[i]];
                    bb.instructions.push_back(std::move(br));
                    terminated = true;
                    break;
                }
                case Rdna2Opcode::S_CBRANCH_SCC0:
                case Rdna2Opcode::S_CBRANCH_SCC1:
                case Rdna2Opcode::S_CBRANCH_VCCZ:
                case Rdna2Opcode::S_CBRANCH_VCCNZ:
                case Rdna2Opcode::S_CBRANCH_EXECZ:
                case Rdna2Opcode::S_CBRANCH_EXECNZ: {
                    const bool uses_scc = (inst.opcode == Rdna2Opcode::S_CBRANCH_SCC0 || inst.opcode == Rdna2Opcode::S_CBRANCH_SCC1);
                    const bool uses_exec = (inst.opcode == Rdna2Opcode::S_CBRANCH_EXECZ || inst.opcode == Rdna2Opcode::S_CBRANCH_EXECNZ);
                    const bool branch_when_false = (inst.opcode == Rdna2Opcode::S_CBRANCH_SCC0 ||
                                                    inst.opcode == Rdna2Opcode::S_CBRANCH_VCCZ ||
                                                    inst.opcode == Rdna2Opcode::S_CBRANCH_EXECZ);
                    IrValueId raw_cond = uses_scc ? state.scc
                                                  : ((uses_exec && state.exec != kInvalidIrValue) ? state.exec : state.vcc);
                    IrValueId cond = ensure_bool(raw_cond);
                    const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[i]];
                    const uint32_t fall_bb = (b + 1 < leaders.size()) ? static_cast<uint32_t>(b + 1) : tgt_bb;
                    IrInstruction cbr{};
                    cbr.op = IrOp::CondBranch;
                    cbr.type = IrType::Void;
                    cbr.operands.push_back(cond);
                    if (branch_when_false) {
                        cbr.true_block = fall_bb;
                        cbr.false_block = tgt_bb;
                    } else {
                        cbr.true_block = tgt_bb;
                        cbr.false_block = fall_bb;
                    }
                    bb.instructions.push_back(std::move(cbr));
                    terminated = true;
                    break;
                }

                // ----- VOP2 / promoted VOP3 -----
                case Rdna2Opcode::V_ADD_F32: {
                    IrValueId r = emit_in_bb(IrOp::FAdd, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_SUB_F32: {
                    IrValueId r = emit_in_bb(IrOp::FSub, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_SUBREV_F32: {
                    IrValueId r = emit_in_bb(IrOp::FSub, IrType::F32, {eval_operand(inst.src[1], true), eval_operand(inst.src[0], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MUL_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMul, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MIN_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMin, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MAX_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMax, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MIN_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAX_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MIN_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAX_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAC_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), true);
                    IrValueId r = emit_in_bb(IrOp::FMad, IrType::F32, {a, bval, acc});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MADMK_F32:
                case Rdna2Opcode::V_MADAK_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId cval = eval_operand(inst.src[2], true);
                    IrValueId r = emit_in_bb(IrOp::FMad, IrType::F32, {a, bval, cval});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMAC_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), true);
                    IrValueId r = emit_in_bb(IrOp::FFma, IrType::F32, {a, bval, acc});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMAC_F16: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), false);
                    const uint32_t flags = inst.clamp ? (1u << 16) : 0u;
                    write_dst(inst.dst, emit_in_bb(IrOp::FFma16, IrType::U32, {a, bval, acc}, flags));
                    break;
                }
                case Rdna2Opcode::V_FMAMK_F16:
                case Rdna2Opcode::V_FMAAK_F16: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId cval = eval_operand(inst.src[2], false);
                    const uint32_t flags = inst.clamp ? (1u << 16) : 0u;
                    write_dst(inst.dst, emit_in_bb(IrOp::FFma16, IrType::U32, {a, bval, cval}, flags));
                    break;
                }
                case Rdna2Opcode::V_AND_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_OR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitOr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_XOR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitXor, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_LSHLREV_B32: {
                    // AMD ISA: vdst = vsrc1 << (src0 & 31)
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shl, IrType::U32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_LSHRREV_B32: {
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::U32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_ASHRREV_I32: {
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::I32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_ADD_NC_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_SUB_NC_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CNDMASK_B32: {
                    // AMD ISA: D = cond ? src1 : src0
                    IrValueId cond_raw = (inst.src_count >= 3)
                                             ? eval_operand(inst.src[2], false)
                                             : state.vcc;
                    IrValueId cond = ensure_bool(cond_raw);
                    const bool s0_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    const bool s1_flt = (inst.src[1].kind == Rdna2OperandKind::InlineFloat);
                    IrValueId v0 = eval_operand(inst.src[0], s0_flt);
                    IrValueId v1 = eval_operand(inst.src[1], s1_flt);
                    IrType sel_type = (get_type(v1) == IrType::F32 || get_type(v0) == IrType::F32)
                                          ? IrType::F32
                                          : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::Select, sel_type, {cond, v1, v0}));
                    break;
                }

                // ----- VOP1 -----
                case Rdna2Opcode::V_NOP:
                    break;
                case Rdna2Opcode::V_MOV_B32: {
                    const bool is_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    write_dst(inst.dst, eval_operand(inst.src[0], is_flt));
                    break;
                }
                case Rdna2Opcode::V_READFIRSTLANE_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    const IrType t = (get_type(val) == IrType::F32) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::WaveReadFirstLane, t, {val}));
                    break;
                }
                case Rdna2Opcode::V_CVT_F32_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::I2F, IrType::F32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::V_CVT_I32_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::F2I, IrType::I32, {eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_CVT_F32_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::U2F, IrType::F32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::V_CVT_U32_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::F2U, IrType::U32, {eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_RCP_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FRcp, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_RSQ_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FRsq, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_SQRT_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FSqrt, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_FLOOR_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FFloor, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_CEIL_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FCeil, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_FRACT_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FFract, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_CVT_NORM_I16_F16:
                case Rdna2Opcode::V_CVT_NORM_U16_F16: {
                    const uint32_t imm = static_cast<uint32_t>(inst.vop3_op_sel & 1u) |
                                         (inst.src[0].neg ? (1u << 8) : 0u) |
                                         (inst.src[0].abs ? (1u << 12) : 0u);
                    const IrOp iop = (inst.opcode == Rdna2Opcode::V_CVT_NORM_I16_F16)
                                         ? IrOp::CvtNormI16F16
                                         : IrOp::CvtNormU16F16;
                    write_dst(inst.dst, emit_in_bb(iop, IrType::U32, {eval_operand(inst.src[0], false)}, imm));
                    break;
                }
                case Rdna2Opcode::V_SWAP_B32: {
                    // V_SWAP_B32 vdst, vsrc0: atomically swaps two VGPRs within active lanes
                    const IrValueId old_dst = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), false);
                    const IrValueId old_src = eval_operand(inst.src[0], false);
                    write_dst(inst.dst, old_src);
                    if (inst.src[0].kind == Rdna2OperandKind::Vgpr) {
                        write_dst(inst.src[0], old_dst);
                    }
                    break;
                }

                // ----- VOPC & GFX10 V_CMPX & split-range F16 compares -----
                case Rdna2Opcode::V_CMP_F_F32:
                case Rdna2Opcode::V_CMP_LT_F32:
                case Rdna2Opcode::V_CMP_EQ_F32:
                case Rdna2Opcode::V_CMP_LE_F32:
                case Rdna2Opcode::V_CMP_GT_F32:
                case Rdna2Opcode::V_CMP_LG_F32:
                case Rdna2Opcode::V_CMP_GE_F32:
                case Rdna2Opcode::V_CMP_O_F32:
                case Rdna2Opcode::V_CMP_U_F32:
                case Rdna2Opcode::V_CMP_NGE_F32:
                case Rdna2Opcode::V_CMP_NLG_F32:
                case Rdna2Opcode::V_CMP_NGT_F32:
                case Rdna2Opcode::V_CMP_NLE_F32:
                case Rdna2Opcode::V_CMP_NEQ_F32:
                case Rdna2Opcode::V_CMP_NLT_F32:
                case Rdna2Opcode::V_CMP_TRU_F32:
                case Rdna2Opcode::V_CMPX_LT_F32:
                case Rdna2Opcode::V_CMPX_EQ_F32:
                case Rdna2Opcode::V_CMPX_LE_F32:
                case Rdna2Opcode::V_CMPX_GT_F32:
                case Rdna2Opcode::V_CMPX_LG_F32:
                case Rdna2Opcode::V_CMPX_GE_F32:
                case Rdna2Opcode::V_CMP_LT_I32:
                case Rdna2Opcode::V_CMP_EQ_I32:
                case Rdna2Opcode::V_CMP_GT_I32:
                case Rdna2Opcode::V_CMP_GE_I32:
                case Rdna2Opcode::V_CMPX_LT_I32:
                case Rdna2Opcode::V_CMPX_EQ_I32:
                case Rdna2Opcode::V_CMPX_GT_I32:
                case Rdna2Opcode::V_CMPX_GE_I32:
                case Rdna2Opcode::V_CMP_LT_U32:
                case Rdna2Opcode::V_CMP_EQ_U32:
                case Rdna2Opcode::V_CMP_GT_U32:
                case Rdna2Opcode::V_CMP_GE_U32:
                case Rdna2Opcode::V_CMPX_LT_U32:
                case Rdna2Opcode::V_CMPX_EQ_U32:
                case Rdna2Opcode::V_CMPX_GT_U32:
                case Rdna2Opcode::V_CMPX_GE_U32:
                case Rdna2Opcode::V_CMP_LT_F16:
                case Rdna2Opcode::V_CMP_EQ_F16:
                case Rdna2Opcode::V_CMP_LE_F16:
                case Rdna2Opcode::V_CMP_GT_F16:
                case Rdna2Opcode::V_CMP_LG_F16:
                case Rdna2Opcode::V_CMP_GE_F16:
                case Rdna2Opcode::V_CMP_U_F16:
                case Rdna2Opcode::V_CMP_NGE_F16:
                case Rdna2Opcode::V_CMP_NLG_F16:
                case Rdna2Opcode::V_CMP_NGT_F16:
                case Rdna2Opcode::V_CMP_NLE_F16:
                case Rdna2Opcode::V_CMP_NEQ_F16:
                case Rdna2Opcode::V_CMP_NLT_F16:
                case Rdna2Opcode::V_CMP_TRU_F16:
                case Rdna2Opcode::V_CMPX_LT_F16:
                case Rdna2Opcode::V_CMPX_EQ_F16:
                case Rdna2Opcode::V_CMPX_LE_F16:
                case Rdna2Opcode::V_CMPX_GT_F16:
                case Rdna2Opcode::V_CMPX_LG_F16:
                case Rdna2Opcode::V_CMPX_GE_F16:
                case Rdna2Opcode::V_CMPX_NEQ_F16:
                case Rdna2Opcode::V_CMPX_NLT_F16: {
                    IrValueId cmp_val = kInvalidIrValue;
                    if (IsF16CompareOpcode(inst.opcode)) {
                        const uint32_t pred = EncodeF16CmpPred(inst.opcode);
                        const uint32_t neg_m = (inst.src[0].neg ? 1u : 0u) | (inst.src[1].neg ? 2u : 0u);
                        const uint32_t abs_m = (inst.src[0].abs ? 1u : 0u) | (inst.src[1].abs ? 2u : 0u);
                        const uint32_t imm = (pred & 15u) |
                                             (static_cast<uint32_t>(inst.vop3_op_sel & 3u) << 8) |
                                             (neg_m << 12) | (abs_m << 16);
                        cmp_val = emit_in_bb(IrOp::CmpF16, IrType::Bool,
                                             {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}, imm);
                    } else {
                        switch (inst.opcode) {
                            case Rdna2Opcode::V_CMP_LT_F32:
                            case Rdna2Opcode::V_CMPX_LT_F32:
                                cmp_val = emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                                break;
                            case Rdna2Opcode::V_CMP_EQ_F32:
                            case Rdna2Opcode::V_CMPX_EQ_F32:
                                cmp_val = emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                                break;
                            case Rdna2Opcode::V_CMP_LE_F32:
                            case Rdna2Opcode::V_CMPX_LE_F32:
                                cmp_val = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[1], true), eval_operand(inst.src[0], true)});
                                break;
                            case Rdna2Opcode::V_CMP_GT_F32:
                            case Rdna2Opcode::V_CMPX_GT_F32:
                                cmp_val = emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                                break;
                            case Rdna2Opcode::V_CMP_LG_F32:
                            case Rdna2Opcode::V_CMPX_LG_F32:
                            case Rdna2Opcode::V_CMP_F_F32:
                            case Rdna2Opcode::V_CMP_O_F32:
                            case Rdna2Opcode::V_CMP_U_F32:
                            case Rdna2Opcode::V_CMP_NGE_F32:
                            case Rdna2Opcode::V_CMP_NLG_F32:
                            case Rdna2Opcode::V_CMP_NGT_F32:
                            case Rdna2Opcode::V_CMP_NLE_F32:
                            case Rdna2Opcode::V_CMP_NEQ_F32:
                            case Rdna2Opcode::V_CMP_NLT_F32:
                            case Rdna2Opcode::V_CMP_TRU_F32:
                                cmp_val = emit_in_bb(IrOp::CmpF32, IrType::Bool,
                                                     {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)},
                                                     static_cast<uint32_t>(EncodeF32CmpPred(inst.opcode)));
                                break;
                            case Rdna2Opcode::V_CMP_GE_F32:
                            case Rdna2Opcode::V_CMPX_GE_F32:
                                cmp_val = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                                break;
                            case Rdna2Opcode::V_CMP_LT_I32:
                            case Rdna2Opcode::V_CMPX_LT_I32:
                                cmp_val = emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_EQ_I32:
                            case Rdna2Opcode::V_CMPX_EQ_I32:
                                cmp_val = emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_GT_I32:
                            case Rdna2Opcode::V_CMPX_GT_I32:
                                cmp_val = emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_GE_I32:
                            case Rdna2Opcode::V_CMPX_GE_I32:
                                cmp_val = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_LT_U32:
                            case Rdna2Opcode::V_CMPX_LT_U32:
                                cmp_val = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_EQ_U32:
                            case Rdna2Opcode::V_CMPX_EQ_U32:
                                cmp_val = emit_in_bb(IrOp::CmpEqU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_GT_U32:
                            case Rdna2Opcode::V_CMPX_GT_U32:
                                cmp_val = emit_in_bb(IrOp::CmpGtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            case Rdna2Opcode::V_CMP_GE_U32:
                            case Rdna2Opcode::V_CMPX_GE_U32:
                                cmp_val = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                                break;
                            default:
                                break;
                        }
                    }
                    if (IsVcmpxOpcode(inst.opcode)) {
                        // GFX10 V_CMPX updates ONLY EXEC (EXEC = EXEC & cmp_val), preserving VCC.
                        if (state.exec != kInvalidIrValue && get_type(state.exec) == IrType::Bool) {
                            state.exec = emit_in_bb(IrOp::LogicalAnd, IrType::Bool, {state.exec, cmp_val});
                        } else {
                            state.exec = cmp_val;
                        }
                    } else {
                        write_dst(inst.dst, cmp_val);
                    }
                    break;
                }

                // ----- VOP3 (native 3-op & 16-bit VOP3) -----
                case Rdna2Opcode::V_FMA_F32: {
                    IrValueId r = emit_in_bb(IrOp::FFma, IrType::F32, {
                        eval_operand(inst.src[0], true),
                        eval_operand(inst.src[1], true),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MAD_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMad, IrType::F32, {
                        eval_operand(inst.src[0], true),
                        eval_operand(inst.src[1], true),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMA_F16: {
                    const uint32_t neg_mask = (inst.src[0].neg ? 1u : 0u) | (inst.src[1].neg ? 2u : 0u) |
                                              (inst.src[2].neg ? 4u : 0u);
                    const uint32_t abs_mask = (inst.src[0].abs ? 1u : 0u) | (inst.src[1].abs ? 2u : 0u) |
                                              (inst.src[2].abs ? 4u : 0u);
                    const uint32_t flags = static_cast<uint32_t>(inst.vop3_op_sel & 7u) |
                                           (neg_mask << 8) | (abs_mask << 12) |
                                           (inst.clamp ? (1u << 16) : 0u);
                    write_dst(inst.dst, emit_in_bb(IrOp::FFma16, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }, flags));
                    break;
                }
                case Rdna2Opcode::V_ADD_NC_U16:
                case Rdna2Opcode::V_SUB_NC_U16:
                case Rdna2Opcode::V_MIN_U16:
                case Rdna2Opcode::V_MAX_U16:
                case Rdna2Opcode::V_MIN_I16:
                case Rdna2Opcode::V_MAX_I16:
                case Rdna2Opcode::V_MUL_LO_U16:
                case Rdna2Opcode::V_LSHLREV_B16:
                case Rdna2Opcode::V_LSHRREV_B16:
                case Rdna2Opcode::V_MAD_U16: {
                    // Structural 16-bit integer VOP3 lowering (classified by encoding/metadata, not _e64 string)
                    IrValueId c_mask16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFu, 0.0f);
                    IrValueId c_sh16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 16u, 0.0f);
                    auto read_u16 = [&](uint8_t src_idx) -> IrValueId {
                        IrValueId raw = eval_operand(inst.src[src_idx], false);
                        if (((inst.vop3_op_sel >> src_idx) & 1u) != 0u) {
                            raw = emit_in_bb(IrOp::Shr, IrType::U32, {raw, c_sh16});
                        }
                        return emit_in_bb(IrOp::BitAnd, IrType::U32, {raw, c_mask16});
                    };
                    IrValueId a16 = read_u16(0u);
                    IrValueId b16 = read_u16(1u);
                    IrValueId r16 = kInvalidIrValue;
                    if (inst.opcode == Rdna2Opcode::V_ADD_NC_U16) {
                        r16 = emit_in_bb(IrOp::IAdd, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_SUB_NC_U16) {
                        r16 = emit_in_bb(IrOp::ISub, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MUL_LO_U16) {
                        r16 = emit_in_bb(IrOp::IMul, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MAD_U16) {
                        IrValueId c16 = read_u16(2u);
                        IrValueId prod = emit_in_bb(IrOp::IMul, IrType::U32, {a16, b16});
                        r16 = emit_in_bb(IrOp::IAdd, IrType::U32, {prod, c16});
                    } else if (inst.opcode == Rdna2Opcode::V_MIN_U16) {
                        r16 = emit_in_bb(IrOp::IMin, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MAX_U16) {
                        r16 = emit_in_bb(IrOp::IMax, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MIN_I16 || inst.opcode == Rdna2Opcode::V_MAX_I16) {
                        IrValueId c_zero = get_zero_u32();
                        IrValueId sa = emit_in_bb(IrOp::BfeI32, IrType::I32, {a16, c_zero, c_sh16});
                        IrValueId sb = emit_in_bb(IrOp::BfeI32, IrType::I32, {b16, c_zero, c_sh16});
                        r16 = emit_in_bb((inst.opcode == Rdna2Opcode::V_MIN_I16) ? IrOp::IMin : IrOp::IMax,
                                         IrType::I32, {sa, sb});
                    } else {
                        IrValueId c_mask4 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 15u, 0.0f);
                        IrValueId sh = emit_in_bb(IrOp::BitAnd, IrType::U32, {a16, c_mask4});
                        r16 = emit_in_bb((inst.opcode == Rdna2Opcode::V_LSHLREV_B16) ? IrOp::Shl : IrOp::Shr,
                                         IrType::U32, {b16, sh});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {r16, c_mask16}));
                    break;
                }
                case Rdna2Opcode::V_MUL_LO_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                    }));
                    break;
                case Rdna2Opcode::V_MBCNT_LO_U32_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId mask = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccLo) {
                        mask = get_ballot_pair(ensure_bool(state.vcc)).first;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecLo) {
                        mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else {
                        mask = eval_operand(inst.src[0], false);
                    }
                    IrValueId init = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::WavePrefixCount64, IrType::U32, {mask, init}, 0u));
                    break;
                }
                case Rdna2Opcode::V_MBCNT_HI_U32_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId mask = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccHi || inst.src[0].kind == Rdna2OperandKind::VccLo) {
                        mask = get_ballot_pair(ensure_bool(state.vcc)).second;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecHi || inst.src[0].kind == Rdna2OperandKind::ExecLo) {
                        mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else {
                        mask = eval_operand(inst.src[0], false);
                    }
                    IrValueId init = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::WavePrefixCount64, IrType::U32, {mask, init}, 1u));
                    break;
                }
                case Rdna2Opcode::V_READLANE_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    IrValueId lane = eval_operand(inst.src[1], false);
                    const IrType t = (get_type(val) == IrType::F32) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::WaveReadLaneAt, t, {val, lane}));
                    break;
                }
                case Rdna2Opcode::V_CVT_PKNORM_I16_F32:
                case Rdna2Opcode::V_CVT_PKNORM_U16_F32: {
                    const IrOp iop = (inst.opcode == Rdna2Opcode::V_CVT_PKNORM_I16_F32)
                                         ? IrOp::CvtPkNormI16F32
                                         : IrOp::CvtPkNormU16F32;
                    write_dst(inst.dst, emit_in_bb(iop, IrType::U32, {
                        eval_operand(inst.src[0], true),
                        eval_operand(inst.src[1], true),
                    }));
                    break;
                }
                case Rdna2Opcode::V_ADD3_U32: {
                    IrValueId ab = emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {ab, eval_operand(inst.src[2], false)}));
                    break;
                }
                case Rdna2Opcode::V_DOT2_F32_F16: {
                    IrValueId r = emit_in_bb(IrOp::FDot2, IrType::F32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_BFE_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BfeU32, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_BFE_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BfeI32, IrType::I32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_BFI_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BfiB32, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_ALIGNBYTE_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::AlignByte, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_ALIGNBIT_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::AlignBit, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_DIV_FMAS_F32: {
                    // RDNA2 ISA: if (VCC[lane]) scale fma(s0,s1,s2) by 2^+64 (if exp(s2)>127) else 2^-64
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId c = eval_operand(inst.src[2], true);
                    IrValueId fma_r = emit_in_bb(IrOp::FFma, IrType::F32, {a, bval, c});
                    IrValueId c_p64 = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0x5F800000u, BitsToFloat(0x5F800000u));
                    IrValueId c_m64 = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0x1F800000u, BitsToFloat(0x1F800000u));
                    IrValueId c_two = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0x40000000u, 2.0f);
                    IrValueId c_neg2 = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0xC0000000u, -2.0f);
                    IrValueId c_ge2 = emit_in_bb(IrOp::CmpGe, IrType::Bool, {c, c_two});
                    IrValueId c_le_n2 = emit_in_bb(IrOp::CmpGe, IrType::Bool, {c_neg2, c});
                    IrValueId exp_gt_127 = emit_bool_or(c_ge2, c_le_n2);
                    IrValueId scale_k = emit_in_bb(IrOp::Select, IrType::F32, {exp_gt_127, c_p64, c_m64});
                    IrValueId scaled = emit_in_bb(IrOp::FMul, IrType::F32, {fma_r, scale_k});
                    IrValueId vcc_b = (state.vcc != kInvalidIrValue) ? ensure_bool(state.vcc) : get_false_bool();
                    IrValueId r = emit_in_bb(IrOp::Select, IrType::F32, {vcc_b, scaled, fma_r});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMA_MIX_F32:
                case Rdna2Opcode::V_FMA_MIXLO_F16:
                case Rdna2Opcode::V_FMA_MIXHI_F16: {
                    const uint32_t mode = (inst.opcode == Rdna2Opcode::V_FMA_MIX_F32) ? 0u
                                          : ((inst.opcode == Rdna2Opcode::V_FMA_MIXLO_F16) ? 1u : 2u);
                    const uint32_t pk_flags = PackVop3pIrFlags(
                        inst.vop3p_op_sel, inst.vop3p_op_sel_hi,
                        inst.vop3p_neg_lo, inst.vop3p_neg_hi, inst.clamp);
                    const uint32_t imm = (mode & 3u) | (pk_flags << 4);
                    IrValueId s0 = eval_operand(inst.src[0], false);
                    IrValueId s1 = eval_operand(inst.src[1], false);
                    IrValueId s2 = eval_operand(inst.src[2], false);
                    IrValueId old_d = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), false);
                    const IrType out_t = (mode == 0u) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::FmaMix, out_t, {s0, s1, s2, old_d}, imm));
                    break;
                }
                case Rdna2Opcode::V_PERMLANE16_B32:
                case Rdna2Opcode::V_PERMLANEX16_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    IrValueId s1 = eval_operand(inst.src[1], false);
                    IrValueId s2 = eval_operand(inst.src[2], false);
                    const uint32_t cross = (inst.opcode == Rdna2Opcode::V_PERMLANEX16_B32) ? 1u : 0u;
                    const IrType t = (get_type(val) == IrType::F32) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::WavePermLane16, t, {val, s1, s2}, cross));
                    break;
                }

                // ----- VOP3P (packed 2x16 f16 / u16 / b16) -----
                case Rdna2Opcode::V_PK_ADD_F16:
                case Rdna2Opcode::V_PK_MUL_F16:
                case Rdna2Opcode::V_PK_FMA_F16:
                case Rdna2Opcode::V_PK_ADD_U16:
                case Rdna2Opcode::V_PK_LSHLREV_B16:
                case Rdna2Opcode::V_PK_LSHRREV_B16: {
                    const uint32_t pk_flags = PackVop3pIrFlags(
                        inst.vop3p_op_sel, inst.vop3p_op_sel_hi,
                        inst.vop3p_neg_lo, inst.vop3p_neg_hi, inst.clamp);
                    IrValueId s0 = eval_operand(inst.src[0], false);
                    IrValueId s1 = eval_operand(inst.src[1], false);
                    if (inst.opcode == Rdna2Opcode::V_PK_FMA_F16) {
                        IrValueId s2 = eval_operand(inst.src[2], false);
                        write_dst(inst.dst, emit_in_bb(IrOp::PkFmaF16, IrType::U32, {s0, s1, s2}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_ADD_F16) {
                        write_dst(inst.dst, emit_in_bb(IrOp::PkAddF16, IrType::U32, {s0, s1}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_MUL_F16) {
                        write_dst(inst.dst, emit_in_bb(IrOp::PkMulF16, IrType::U32, {s0, s1}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_ADD_U16) {
                        write_dst(inst.dst, emit_in_bb(IrOp::PkAddU16, IrType::U32, {s0, s1}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_LSHLREV_B16) {
                        // REV: src1 is value, src0 is shift amount
                        write_dst(inst.dst, emit_in_bb(IrOp::PkShl16, IrType::U32, {s1, s0}, pk_flags));
                    } else {
                        // REV: src1 is value, src0 is shift amount
                        write_dst(inst.dst, emit_in_bb(IrOp::PkShr16, IrType::U32, {s1, s0}, pk_flags));
                    }
                    break;
                }

                // ----- SMEM -----
                case Rdna2Opcode::S_LOAD_DWORD:
                case Rdna2Opcode::S_BUFFER_LOAD_DWORD: {
                    // Signed 21-bit SMEM immediate offset (xbox-emulator #206) + OOB clamp flag (Leonida #4573)
                    const int32_t signed_off = inst.mem_offset;
                    const uint32_t raw_off = static_cast<uint32_t>(signed_off);
                    const bool static_zero_soff =
                        (inst.soffset == 0x7Du || inst.soffset == 0x7Cu || inst.soffset == 128u);
                    if (inst.sbase == 0u && static_zero_soff && signed_off >= 0 && (static_cast<uint32_t>(signed_off) / 4u) < 16u) {
                        write_dst(inst.dst, get_root_const(static_cast<uint32_t>(signed_off) / 4u));
                    } else if (inst.sbase != 0u) {
                        // Mixed-heap safety: CB loads with non-zero base SGPR offset route to
                        // dedicated CB space1 / DescriptorSet 1 so they never alias V# slot 0.
                        const uint32_t cb_slot = static_cast<uint32_t>(inst.sbase & 0xFFu);
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, raw_off, 0.0f);
                        if (inst.soffset < 106u) {
                            off = emit_in_bb(IrOp::IAdd, IrType::U32, {off, read_sgpr(inst.soffset)});
                        } else if (inst.soffset >= 129u && inst.soffset <= 192u) {
                            IrValueId imm_s = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                                         static_cast<uint32_t>(inst.soffset - 128u), 0.0f);
                            off = emit_in_bb(IrOp::IAdd, IrType::U32, {off, imm_s});
                        }
                        const uint32_t load_flags = kIrLoadCbHeapFlag | kIrLoadSmemBoundsCheckFlag | cb_slot;
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {off}, load_flags));
                    } else {
                        out_module.uses_uav = true;
                        out_module.uses_uav0 = true;
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, raw_off, 0.0f);
                        if (inst.soffset < 106u) {
                            off = emit_in_bb(IrOp::IAdd, IrType::U32, {off, read_sgpr(inst.soffset)});
                        } else if (inst.soffset >= 129u && inst.soffset <= 192u) {
                            IrValueId imm_s = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                                         static_cast<uint32_t>(inst.soffset - 128u), 0.0f);
                            off = emit_in_bb(IrOp::IAdd, IrType::U32, {off, imm_s});
                        }
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {off},
                                                       kIrLoadSmemBoundsCheckFlag));
                    }
                    break;
                }

                // ----- MUBUF & MTBUF -----
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ:
                case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_X:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ:
                case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW: {
                    const uint32_t uav_slot = (inst.srsrc == 0u) ? 0u : 1u;
                    out_module.uses_uav = true;
                    if (uav_slot == 0u) out_module.uses_uav0 = true;
                    else out_module.uses_uav1 = true;

                    IrValueId addr = kInvalidIrValue;
                    if (inst.offen) {
                        addr = eval_operand(inst.src[1], false);
                    } else if (inst.idxen) {
                        IrValueId idx = eval_operand(inst.src[1], false);
                        IrValueId sh = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 2u, 0.0f);
                        addr = emit_in_bb(IrOp::Shl, IrType::U32, {idx, sh});
                    } else {
                        addr = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0u, 0.0f);
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    if (inst.soffset != 128u && inst.soffset != 125u) {
                        Rdna2Operand sop = DecodeRdna2SourceOperand(inst.soffset);
                        IrValueId soff_v = eval_operand(sop, false);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, soff_v});
                    }

                    bool is_d16 = false;
                    const uint32_t comps = FormatLoadComponentCount(inst.opcode, is_d16);
                    const uint8_t fmt_code = (inst.encoding == Rdna2Encoding::MTBUF && inst.mtbuf_format != 0u)
                                                 ? inst.mtbuf_format
                                                 : static_cast<uint8_t>(Rdna2MtbufFormat::R32G32B32A32_FLOAT);
                    const auto fmt = static_cast<Rdna2MtbufFormat>(fmt_code);
                    const uint8_t dst_base = static_cast<uint8_t>(inst.dst.reg_index);

                    if (is_d16) {
                        // D16 packs 2 converted 16-bit components per VGPR
                        const uint32_t reg_count = (comps + 1u) / 2u;
                        for (uint32_t r = 0; r < reg_count; ++r) {
                            const uint32_t imm = (uav_slot & 0xFu) |
                                                 (static_cast<uint32_t>(fmt_code & 0x7Fu) << 4) |
                                                 (static_cast<uint32_t>((r * 2u) & 0xFu) << 12) |
                                                 (1u << 16) |
                                                 (((r * 2u + 1u < comps) ? 1u : 0u) << 17);
                            Rdna2Operand dst_op = Rdna2Operand::Vgpr(static_cast<uint8_t>(dst_base + r));
                            write_dst(dst_op, emit_in_bb(IrOp::BufferLoadFormat, IrType::U32, {addr}, imm));
                        }
                    } else {
                        const IrType res_t = IsFormatIntegerKind(fmt) ? IrType::U32 : IrType::F32;
                        for (uint32_t c = 0; c < comps; ++c) {
                            const uint32_t imm = (uav_slot & 0xFu) |
                                                 (static_cast<uint32_t>(fmt_code & 0x7Fu) << 4) |
                                                 (static_cast<uint32_t>(c & 0xFu) << 12);
                            Rdna2Operand dst_op = Rdna2Operand::Vgpr(static_cast<uint8_t>(dst_base + c));
                            write_dst(dst_op, emit_in_bb(IrOp::BufferLoadFormat, res_t, {addr}, imm));
                        }
                    }
                    break;
                }
                case Rdna2Opcode::BUFFER_LOAD_DWORD:
                case Rdna2Opcode::BUFFER_STORE_DWORD: {
                    const uint32_t uav_slot = (inst.srsrc == 0u) ? 0u : 1u;
                    out_module.uses_uav = true;
                    if (uav_slot == 0u) {
                        out_module.uses_uav0 = true;
                    } else {
                        out_module.uses_uav1 = true;
                    }
                    IrValueId addr = kInvalidIrValue;
                    if (inst.offen) {
                        addr = eval_operand(inst.src[1], false);
                    } else if (inst.idxen) {
                        IrValueId idx = eval_operand(inst.src[1], false);
                        IrValueId sh = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 2u, 0.0f);
                        addr = emit_in_bb(IrOp::Shl, IrType::U32, {idx, sh});
                    } else {
                        addr = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0u, 0.0f);
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    if (inst.soffset != 128u && inst.soffset != 125u) {
                        Rdna2Operand sop = DecodeRdna2SourceOperand(inst.soffset);
                        IrValueId soff_v = eval_operand(sop, false);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, soff_v});
                    }
                    if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {addr}, uav_slot));
                    } else {
                        IrValueId data = eval_operand(inst.src[0], false);
                        emit_in_bb(IrOp::StoreUav, IrType::Void, {addr, data}, uav_slot);
                        attach_exec_pred();
                    }
                    break;
                }

                // ----- MIMG -----
                case Rdna2Opcode::IMAGE_LOAD: {
                    out_module.uses_texture_srv = true;
                    out_module.uses_texture_load = true;
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    const IrType load_ty = (out_module.texture_srv_format == Rdna2TextureFormat::Uint4)
                                               ? IrType::U32
                                               : IrType::F32;
                    uint8_t out_reg = vdata_base;
                    for (uint32_t ch = 0; ch < 4u; ++ch) {
                        if ((dmask & (1u << ch)) != 0u) {
                            state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageLoad, load_ty, {x, y}, ch);
                        }
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_LOAD_MIP: {
                    const uint8_t dim = inst.mimg_dim;
                    if (dim == 2u) {
                        out_module.uses_texture3d_srv = true;
                    } else if (dim == 3u) {
                        out_module.uses_texture_cube_srv = true;
                    } else {
                        out_module.uses_texture_srv = true;
                        out_module.uses_texture2d_mip_srv = true;
                    }
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    IrValueId z = get_zero_u32();
                    IrValueId mip = get_zero_u32();
                    if (dim == 2u || dim == 3u) {
                        z = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), false);
                        mip = read_vgpr(static_cast<uint8_t>(vaddr_base + 3u), false);
                    } else {
                        mip = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), false);
                    }
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    const IrType load_ty = (dim != 2u && dim != 3u &&
                                            out_module.texture_srv_format == Rdna2TextureFormat::Uint4)
                                               ? IrType::U32
                                               : IrType::F32;
                    uint8_t out_reg = vdata_base;
                    for (uint32_t ch = 0; ch < 4u; ++ch) {
                        if ((dmask & (1u << ch)) != 0u) {
                            const uint32_t imm = (ch & 3u) | (static_cast<uint32_t>(dim & 7u) << 4);
                            state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageLoadMip, load_ty, {x, y, z, mip}, imm);
                        }
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP: {
                    const uint8_t dim = (inst.mimg_dim == 2u || inst.mimg_dim == 3u) ? inst.mimg_dim : 1u;
                    out_module.uses_texture_u32_uav = true;
                    out_module.texture_u32_uav_dim = dim;
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[1].reg_index);
                    // RDNA2 ISA: vdata[0] = src (new value), vdata[1] = cmp (compare value)
                    IrValueId src_val = read_vgpr(vdata_base, false);
                    IrValueId cmp_val = read_vgpr(static_cast<uint8_t>(vdata_base + 1u), false);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    IrValueId z = (dim == 2u || dim == 3u)
                                      ? read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), false)
                                      : get_zero_u32();
                    IrValueId prev = emit_in_bb(IrOp::ImageAtomicCmpSwap, IrType::U32,
                                                {x, y, z, cmp_val, src_val}, dim);
                    if (inst.glc) {
                        state.vgpr[vdata_base] = prev;
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY: {
                    out_module.uses_uav = true;
                    out_module.uses_uav0 = true;
                    out_module.uses_bvh_ray = true;
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId node_ptr = read_vgpr(vaddr_base, false);
                    IrValueId tmax = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), true);
                    IrValueId ox = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), true);
                    IrValueId oy = read_vgpr(static_cast<uint8_t>(vaddr_base + 3u), true);
                    IrValueId oz = read_vgpr(static_cast<uint8_t>(vaddr_base + 4u), true);
                    IrValueId dx = read_vgpr(static_cast<uint8_t>(vaddr_base + 5u), true);
                    IrValueId dy = read_vgpr(static_cast<uint8_t>(vaddr_base + 6u), true);
                    IrValueId dz = read_vgpr(static_cast<uint8_t>(vaddr_base + 7u), true);
                    std::vector<IrValueId> ops = {node_ptr, tmax, ox, oy, oz, dx, dy, dz};
                    state.vgpr[vdata_base + 0u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::F32, ops, 0u);
                    state.vgpr[vdata_base + 1u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::F32, ops, 1u);
                    state.vgpr[vdata_base + 2u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::F32, ops, 2u);
                    state.vgpr[vdata_base + 3u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::U32, ops, 3u);
                    break;
                }
                case Rdna2Opcode::IMAGE_SAMPLE:
                case Rdna2Opcode::IMAGE_SAMPLE_LZ:
                case Rdna2Opcode::IMAGE_SAMPLE_C_LZ: {
                    out_module.uses_texture_srv = true;
                    out_module.uses_texture_sample = true;
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    uint8_t out_reg = vdata_base;
                    if (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE_C_LZ) {
                        // RDNA2 ISA: image_sample_c_lz takes vaddr[0]=dref, vaddr[1]=u, vaddr[2]=v
                        out_module.uses_comparison_sample = true;
                        IrValueId dref = read_vgpr(vaddr_base, true);
                        IrValueId u = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), true);
                        IrValueId v = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), true);
                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                            if ((dmask & (1u << ch)) != 0u) {
                                state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageSample, IrType::F32, {u, v, dref}, ch | 0x80u);
                            }
                        }
                    } else {
                        IrValueId u = read_vgpr(vaddr_base, true);
                        IrValueId v = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), true);
                        if (out_module.stage == ShaderStage::Pixel && inst.opcode == Rdna2Opcode::IMAGE_SAMPLE) {
                            // Pixel shader implicit-LOD sample: use helper-lane-aware affine quad derivatives
                            out_module.uses_pixel_quad_derivatives = true;
                            for (uint32_t ch = 0; ch < 4u; ++ch) {
                                if ((dmask & (1u << ch)) != 0u) {
                                    state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageSampleImplicit, IrType::F32, {u, v}, ch);
                                }
                            }
                        } else {
                            IrValueId lod = get_zero_f32();
                            if (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE &&
                                vaddr_base + 2u < 256u && state.vgpr[vaddr_base + 2u] != kInvalidIrValue) {
                                lod = state.vgpr[vaddr_base + 2u];
                            }
                            for (uint32_t ch = 0; ch < 4u; ++ch) {
                                if ((dmask & (1u << ch)) != 0u) {
                                    state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageSample, IrType::F32, {u, v, lod}, ch);
                                }
                            }
                        }
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_STORE: {
                    out_module.uses_texture_uav = true;
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[1].reg_index);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    IrValueId rgba[4]{};
                    uint8_t in_reg = vdata_base;
                    for (uint32_t ch = 0; ch < 4u; ++ch) {
                        if ((dmask & (1u << ch)) != 0u) {
                            rgba[ch] = read_vgpr(in_reg++, true);
                        } else if (ch == 3u) {
                            rgba[ch] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
                        } else {
                            rgba[ch] = get_zero_f32();
                        }
                    }
                    emit_in_bb(IrOp::ImageStore, IrType::Void, {x, y, rgba[0], rgba[1], rgba[2], rgba[3]});
                    break;
                }

                // ----- DS (LDS Shared Memory & Swizzle) -----
                case Rdna2Opcode::DS_WRITE_B32: {
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::LdsStore, IrType::Void, {addr, data});
                    attach_exec_pred();
                    break;
                }
                case Rdna2Opcode::DS_READ_B32: {
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::LdsLoad, IrType::U32, {addr}));
                    break;
                }
                case Rdna2Opcode::DS_READ_U8_D16:
                case Rdna2Opcode::DS_READ_U8_D16_HI:
                case Rdna2Opcode::DS_READ_I8_D16:
                case Rdna2Opcode::DS_READ_I8_D16_HI:
                case Rdna2Opcode::DS_READ_U16_D16:
                case Rdna2Opcode::DS_READ_U16_D16_HI: {
                    // LDS D16 loads (xbox-emulator #205): load aligned dword, extract/sign-extend, preserve opposite 16-bit half
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId c_inv3 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, ~3u, 0.0f);
                    IrValueId aligned = emit_in_bb(IrOp::BitAnd, IrType::U32, {addr, c_inv3});
                    IrValueId word = emit_in_bb(IrOp::LdsLoad, IrType::U32, {aligned});

                    const bool is_hi = (inst.opcode == Rdna2Opcode::DS_READ_U8_D16_HI ||
                                        inst.opcode == Rdna2Opcode::DS_READ_I8_D16_HI ||
                                        inst.opcode == Rdna2Opcode::DS_READ_U16_D16_HI);
                    const bool is_u16 = (inst.opcode == Rdna2Opcode::DS_READ_U16_D16 ||
                                         inst.opcode == Rdna2Opcode::DS_READ_U16_D16_HI);
                    const bool is_i8 = (inst.opcode == Rdna2Opcode::DS_READ_I8_D16 ||
                                        inst.opcode == Rdna2Opcode::DS_READ_I8_D16_HI);

                    IrValueId c_byte_mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, is_u16 ? 2u : 3u, 0.0f);
                    IrValueId c_3 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 3u, 0.0f);
                    IrValueId byte_off = emit_in_bb(IrOp::BitAnd, IrType::U32, {addr, c_byte_mask});
                    IrValueId bit_shift = emit_in_bb(IrOp::Shl, IrType::U32, {byte_off, c_3});
                    IrValueId c_m16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFu, 0.0f);

                    IrValueId val16 = kInvalidIrValue;
                    if (is_i8) {
                        IrValueId c_8 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 8u, 0.0f);
                        IrValueId sext = emit_in_bb(IrOp::BfeI32, IrType::I32, {word, bit_shift, c_8});
                        val16 = emit_in_bb(IrOp::BitAnd, IrType::U32, {sext, c_m16});
                    } else {
                        IrValueId shifted = emit_in_bb(IrOp::Shr, IrType::U32, {word, bit_shift});
                        IrValueId mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, is_u16 ? 0xFFFFu : 0xFFu, 0.0f);
                        val16 = emit_in_bb(IrOp::BitAnd, IrType::U32, {shifted, mask});
                    }

                    IrValueId old_dst = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), false);
                    IrValueId merged = kInvalidIrValue;
                    if (is_hi) {
                        IrValueId c_16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 16u, 0.0f);
                        IrValueId kept_lo = emit_in_bb(IrOp::BitAnd, IrType::U32, {old_dst, c_m16});
                        IrValueId hi_part = emit_in_bb(IrOp::Shl, IrType::U32, {val16, c_16});
                        merged = emit_in_bb(IrOp::BitOr, IrType::U32, {kept_lo, hi_part});
                    } else {
                        IrValueId c_hi_mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFF0000u, 0.0f);
                        IrValueId kept_hi = emit_in_bb(IrOp::BitAnd, IrType::U32, {old_dst, c_hi_mask});
                        merged = emit_in_bb(IrOp::BitOr, IrType::U32, {kept_hi, val16});
                    }
                    write_dst(inst.dst, merged);
                    break;
                }
                case Rdna2Opcode::DS_ADD_U32:
                case Rdna2Opcode::DS_ADD_RTN_U32: {
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    IrValueId prev = emit_in_bb(IrOp::LdsAtomicAdd, IrType::U32, {addr, data});
                    attach_exec_pred();
                    if (inst.opcode == Rdna2Opcode::DS_ADD_RTN_U32) {
                        write_dst(inst.dst, prev);
                    }
                    break;
                }
                case Rdna2Opcode::DS_SWIZZLE_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    IrValueId lane = emit_in_bb(IrOp::WaveLaneId, IrType::U32);
                    const uint16_t swiz = static_cast<uint16_t>(static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu);
                    const Rdna2DsSwizzleInfo sw = DecodeRdna2DsSwizzleOffset(swiz);
                    IrValueId target_lane = lane;
                    if (sw.mode == Rdna2DsSwizzleMode::BitMode) {
                        // Preserve bit 5 (0x20) so Wave64 upper half [32..63] swizzles within its own 32-lane half
                        const uint32_t and_m = static_cast<uint32_t>(sw.and_mask) | 0x20u;
                        IrValueId c_and = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, and_m, 0.0f);
                        IrValueId c_or = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, sw.or_mask, 0.0f);
                        IrValueId c_xor = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, sw.xor_mask, 0.0f);
                        IrValueId t1 = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_and});
                        IrValueId t2 = emit_in_bb(IrOp::BitOr, IrType::U32, {t1, c_or});
                        target_lane = emit_in_bb(IrOp::BitXor, IrType::U32, {t2, c_xor});
                    } else if (sw.mode == Rdna2DsSwizzleMode::QuadPermute) {
                        IrValueId c_3 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 3u, 0.0f);
                        IrValueId c_inv3 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, ~3u, 0.0f);
                        IrValueId c_1 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        IrValueId c_swiz = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(swiz), 0.0f);
                        IrValueId quad_base = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_inv3});
                        IrValueId q_lane = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_3});
                        IrValueId shift = emit_in_bb(IrOp::Shl, IrType::U32, {q_lane, c_1});
                        IrValueId shifted = emit_in_bb(IrOp::Shr, IrType::U32, {c_swiz, shift});
                        IrValueId sel = emit_in_bb(IrOp::BitAnd, IrType::U32, {shifted, c_3});
                        target_lane = emit_in_bb(IrOp::BitOr, IrType::U32, {quad_base, sel});
                    } else if (sw.mode == Rdna2DsSwizzleMode::Rotate) {
                        // Rotate-right within cluster size: (lane & ~cmask) | ((lane - shift) & cmask)
                        const uint32_t cmask = static_cast<uint32_t>(sw.rotate_cluster_mask);
                        IrValueId c_inv = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, ~cmask, 0.0f);
                        IrValueId c_cmask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, cmask, 0.0f);
                        IrValueId c_sh = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, sw.rotate_shift, 0.0f);
                        IrValueId base = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_inv});
                        IrValueId diff = emit_in_bb(IrOp::ISub, IrType::U32, {lane, c_sh});
                        IrValueId rot = emit_in_bb(IrOp::BitAnd, IrType::U32, {diff, c_cmask});
                        target_lane = emit_in_bb(IrOp::BitOr, IrType::U32, {base, rot});
                    } else if (sw.mode == Rdna2DsSwizzleMode::Fft) {
                        // Butterfly pair swap within 32-lane half: lane ^ fft_mask
                        IrValueId c_fft = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, sw.fft_mask, 0.0f);
                        target_lane = emit_in_bb(IrOp::BitXor, IrType::U32, {lane, c_fft});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::WaveReadLaneAt, IrType::U32, {val, target_lane},
                                                   kIrWaveSwizzleFlag | static_cast<uint32_t>(swiz)));
                    break;
                }

                // ----- GLOBAL / FLAT -----
                case Rdna2Opcode::GLOBAL_LOAD_DWORD:
                case Rdna2Opcode::FLAT_LOAD_DWORD: {
                    out_module.uses_uav = true;
                    out_module.uses_uav0 = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {addr}, 0u));
                    break;
                }
                case Rdna2Opcode::GLOBAL_STORE_DWORD:
                case Rdna2Opcode::FLAT_STORE_DWORD: {
                    out_module.uses_uav = true;
                    out_module.uses_uav0 = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::StoreUav, IrType::Void, {addr, data}, 0u);
                    break;
                }
                case Rdna2Opcode::SCRATCH_LOAD_DWORD: {
                    out_module.uses_scratch = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.saddr != 0x7Fu && inst.saddr < 106u) {
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, read_sgpr(inst.saddr)});
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                        if (inst.mem_offset > 0) {
                            max_scratch_byte_end = std::max(max_scratch_byte_end, static_cast<uint32_t>(inst.mem_offset) + 4u);
                        }
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::ScratchLoad, IrType::U32, {addr}));
                    break;
                }
                case Rdna2Opcode::SCRATCH_STORE_DWORD: {
                    out_module.uses_scratch = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.saddr != 0x7Fu && inst.saddr < 106u) {
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, read_sgpr(inst.saddr)});
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                        if (inst.mem_offset > 0) {
                            max_scratch_byte_end = std::max(max_scratch_byte_end, static_cast<uint32_t>(inst.mem_offset) + 4u);
                        }
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::ScratchStore, IrType::Void, {addr, data});
                    attach_exec_pred();
                    break;
                }

                // ----- VINTRP (FSSM Pixel Shader parameter interpolation) -----
                case Rdna2Opcode::V_INTERP_P1_F32:
                case Rdna2Opcode::V_INTERP_P2_F32:
                case Rdna2Opcode::V_INTERP_MOV_F32: {
                    uint32_t sem_loc = inst.vintrp_attr;
                    Rdna2InterpMode mode = (inst.opcode == Rdna2Opcode::V_INTERP_MOV_F32)
                                               ? Rdna2InterpMode::Flat
                                               : Rdna2InterpMode::Perspective;
                    for (const auto& fssm : out_module.stage_contract.pixel_inputs) {
                        if (fssm.param_slot == inst.vintrp_attr) {
                            sem_loc = fssm.semantic_location;
                            if (inst.opcode != Rdna2Opcode::V_INTERP_MOV_F32) {
                                mode = fssm.interp_mode;
                            } else {
                                mode = Rdna2InterpMode::Flat;
                            }
                            break;
                        }
                    }
                    if (sem_loc >= out_module.input_interp_modes.size()) {
                        out_module.input_interp_modes.resize(sem_loc + 1u, Rdna2InterpMode::Perspective);
                    }
                    if (mode != Rdna2InterpMode::Perspective ||
                        out_module.input_interp_modes[sem_loc] == Rdna2InterpMode::Perspective) {
                        out_module.input_interp_modes[sem_loc] = mode;
                    }
                    const uint32_t pslot = std::min<uint32_t>(3u, sem_loc);
                    if (inst.opcode == Rdna2Opcode::V_INTERP_P1_F32) {
                        // P1 sets up intermediate barycentric step; P2 completes final interpolated value
                        write_dst(inst.dst, get_ps_input(inst.vintrp_chan & 3u, pslot));
                    } else {
                        // P2 or MOV produces the interpolated/flat attribute channel
                        write_dst(inst.dst, get_ps_input(inst.vintrp_chan & 3u, pslot));
                    }
                    break;
                }

                // ----- EXP -----
                case Rdna2Opcode::EXP: {
                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Null)) {
                        // Pixel shader null-export kill (exp null, off, off, off, off done vm)
                        if (out_module.stage == ShaderStage::Pixel) {
                            IrValueId cond = (state.exec != kInvalidIrValue && get_type(state.exec) == IrType::Bool)
                                                 ? state.exec
                                                 : kInvalidIrValue;
                            emit_pixel_kill(cond);
                        }
                        break;
                    }
                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Prim)) {
                        out_module.exports_prim = true;
                        if (inst.exp_en == 0x1u) {
                            IrValueId packed = eval_operand(inst.src[0], false);
                            emit_in_bb(IrOp::ExportMeshPrim, IrType::Void, {packed}, 1u);
                            if (inst.exp_compr) {
                                // Packed primitive cull bit in bit 31 (emits CullPrimitiveEXT / SV_CullPrimitive)
                                out_module.uses_mesh_cull = true;
                                IrValueId c_sh31 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 31u, 0.0f);
                                IrValueId cull_bit = emit_in_bb(IrOp::Shr, IrType::U32, {packed, c_sh31});
                                IrValueId cull_bool = emit_in_bb(IrOp::CmpNe, IrType::Bool, {cull_bit, get_zero_u32()});
                                emit_in_bb(IrOp::ExportMeshCull, IrType::Void, {cull_bool});
                            }
                        } else {
                            IrValueId i0 = eval_operand(inst.src[0], false);
                            IrValueId i1 = eval_operand(inst.src[1], false);
                            IrValueId i2 = eval_operand(inst.src[2], false);
                            emit_in_bb(IrOp::ExportMeshPrim, IrType::Void, {i0, i1, i2}, 0u);
                        }
                        break;
                    }
                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) &&
                        out_module.stage == ShaderStage::Pixel &&
                        state.exec != kInvalidIrValue && get_type(state.exec) == IrType::Bool) {
                        // Any lane deactivated in EXEC before final MRT0 export is discarded
                        IrValueId kill_cond = emit_in_bb(IrOp::LogicalNot, IrType::Bool, {state.exec});
                        emit_pixel_kill(kill_cond);
                    }
                    std::vector<IrValueId> ch(4);
                    for (uint8_t c = 0; c < 4; ++c) {
                        if ((inst.exp_en & (1u << c)) != 0u) {
                            ch[c] = eval_operand(inst.src[c], true);
                        } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && c == 3u) {
                            ch[c] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
                        } else {
                            ch[c] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                        }
                    }
                    IrInstruction ex{};
                    ex.op = IrOp::Export;
                    ex.type = IrType::Void;
                    ex.operands = std::move(ch);
                    ex.export_target = inst.exp_target;
                    ex.export_mask = inst.exp_en;
                    bb.instructions.push_back(std::move(ex));

                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                        out_module.exports_pos0 = true;
                    } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0)) {
                        out_module.exports_mrt0 = true;
                    } else if (inst.exp_target >= static_cast<uint8_t>(Rdna2ExportTarget::Param0) &&
                               inst.exp_target <= static_cast<uint8_t>(Rdna2ExportTarget::Param3)) {
                        out_module.exports_param0 = true;
                        const size_t pidx = static_cast<size_t>(
                            inst.exp_target - static_cast<uint8_t>(Rdna2ExportTarget::Param0));
                        if (out_module.output_interp_modes.size() <= pidx) {
                            out_module.output_interp_modes.resize(pidx + 1u, Rdna2InterpMode::Perspective);
                        }
                    }
                    break;
                }

                default:
                    return Status::Error(
                        StatusCode::NotImplemented,
                        std::string("Unhandled RDNA2 opcode in IR builder: ") + Rdna2OpcodeName(inst.opcode));
            }
            if (terminated) {
                break;
            }
        }

        if (!terminated) {
            if (b + 1 < leaders.size()) {
                IrInstruction br{};
                br.op = IrOp::Branch;
                br.type = IrType::Void;
                br.true_block = static_cast<uint32_t>(b + 1);
                bb.instructions.push_back(std::move(br));
            } else {
                emit_in_bb(IrOp::Return, IrType::Void);
            }
        }

        exit_states[b] = state;
        block_processed[b] = true;
    }

    // Patch back-edge / irreducible incoming values into pre-allocated loop header Phis.
    for (const auto& pend : pending_loop_phis) {
        auto& bb = out_module.blocks[pend.bb_id];
        if (pend.inst_index >= bb.instructions.size()) continue;
        auto& phi = bb.instructions[pend.inst_index];
        if (phi.op != IrOp::Phi) continue;

        for (uint32_t p : bb.predecessors) {
            bool already = false;
            for (const auto& inc : phi.phi_incoming) {
                if (inc.first == p) {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            IrValueId val = kInvalidIrValue;
            const auto& es = exit_states[p];
            switch (pend.kind) {
                case PendingRegKind::Vgpr: val = es.vgpr[pend.reg_index]; break;
                case PendingRegKind::Sgpr: val = es.sgpr[pend.reg_index]; break;
                case PendingRegKind::Scc:  val = es.scc; break;
                case PendingRegKind::Vcc:  val = es.vcc; break;
                case PendingRegKind::Exec: val = es.exec; break;
                case PendingRegKind::M0:   val = es.m0; break;
            }
            if (val == kInvalidIrValue) {
                if (pend.kind == PendingRegKind::Exec) {
                    val = get_true_bool();
                } else if (phi.type == IrType::F32) {
                    val = get_zero_f32();
                } else if (phi.type == IrType::Bool) {
                    val = get_false_bool();
                } else {
                    val = get_zero_u32();
                }
            }
            phi.phi_incoming.emplace_back(p, val);
        }
    }

    // Grow per-thread scratch frame size to fit heavy VGPR spilling offsets (power-of-2, min 64 dwords).
    if (out_module.uses_scratch && max_scratch_byte_end > 0u) {
        const uint32_t needed_dwords = (max_scratch_byte_end + 3u) / 4u;
        out_module.scratch_size_dwords = RoundUpPow2U32(needed_dwords, 64u, 2048u);
    }

    // Prepend hoisted entry preamble into block 0 (strictly after any Phi nodes).
    if (!entry_preamble.empty()) {
        auto& bb0 = out_module.blocks[0].instructions;
        size_t insert_pos = 0u;
        while (insert_pos < bb0.size() && bb0[insert_pos].op == IrOp::Phi) {
            ++insert_pos;
        }
        bb0.insert(bb0.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                   std::make_move_iterator(entry_preamble.begin()),
                   std::make_move_iterator(entry_preamble.end()));
    }

    // Canonicalize exit-less loops and non-merge natural loop latches for structured lowering.
    CanonicalizeRdna2Cfg(out_module);

    uint32_t total_ops = 0;
    for (const auto& bb : out_module.blocks) {
        total_ops += static_cast<uint32_t>(bb.instructions.size());
    }
    out_module.opt_stats.initial_ops = total_ops;
    out_module.opt_stats.final_ops = total_ops;

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Constant Folding + Dead-Code Elimination Passes
// ---------------------------------------------------------------------------
Status OptimizeRdna2Ir(Rdna2IrModule& module, Rdna2IrOptStats* out_stats) {
    CanonicalizeRdna2Cfg(module);
    Rdna2IrOptStats stats{};
    for (const auto& bb : module.blocks) {
        stats.initial_ops += static_cast<uint32_t>(bb.instructions.size());
    }

    // Build value definition lookup table.
    std::unordered_map<IrValueId, IrInstruction*> defs;
    auto rebuild_defs = [&]() {
        defs.clear();
        for (auto& bb : module.blocks) {
            for (auto& inst : bb.instructions) {
                if (inst.id != kInvalidIrValue) {
                    defs[inst.id] = &inst;
                }
            }
        }
    };

    auto get_const_u32 = [&](IrValueId id, uint32_t& out_v) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        if (d->op == IrOp::ConstU32) {
            out_v = d->imm_u32;
            return true;
        }
        if (d->op == IrOp::ConstF32) {
            out_v = FloatToBits(d->imm_f32);
            return true;
        }
        return false;
    };

    auto get_const_f32 = [&](IrValueId id, float& out_v) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        if (d->op == IrOp::ConstF32) {
            out_v = d->imm_f32;
            return true;
        }
        if (d->op == IrOp::ConstU32) {
            out_v = BitsToFloat(d->imm_u32);
            return true;
        }
        return false;
    };

    // Pass 1 + 1.5: Iterative Constant Folding + Active-Mask Bitmask/Xor/Ballot Implication & Disjoint Pruning
    auto replace_all_uses = [&](IrValueId old_id, IrValueId new_id) {
        if (old_id == kInvalidIrValue || new_id == kInvalidIrValue || old_id == new_id) return;
        for (auto& bb_u : module.blocks) {
            for (auto& ins_u : bb_u.instructions) {
                for (auto& op_id : ins_u.operands) {
                    if (op_id == old_id) op_id = new_id;
                }
                for (auto& inc : ins_u.phi_incoming) {
                    if (inc.second == old_id) inc.second = new_id;
                }
                if (ins_u.exec_pred == old_id) {
                    ins_u.exec_pred = new_id;
                }
            }
        }
    };

    auto is_const_zero = [&](IrValueId id) -> bool {
        uint32_t v = 0;
        return get_const_u32(id, v) && v == 0u;
    };

    auto is_const_all_ones = [&](IrValueId id) -> bool {
        auto it = defs.find(id);
        if (it == defs.end() || it->second->op != IrOp::ConstU32) return false;
        if (it->second->type == IrType::Bool) return it->second->imm_u32 != 0u;
        return it->second->imm_u32 == 0xFFFFFFFFu;
    };

    auto is_not_of = [&](IrValueId id, IrValueId target) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        return (d->op == IrOp::BitNot || d->op == IrOp::LogicalNot) &&
               !d->operands.empty() && d->operands[0] == target;
    };

    auto implies_subset = [&](auto&& self, IrValueId a, IrValueId b, int depth) -> bool {
        if (depth > 6) return false;
        if (a == b) return true;
        if (is_const_zero(a) || is_const_all_ones(b)) return true;
        auto ita = defs.find(a);
        auto itb = defs.find(b);
        if (ita != defs.end()) {
            const auto* da = ita->second;
            if ((da->op == IrOp::BitAnd || da->op == IrOp::LogicalAnd) && da->operands.size() >= 2u) {
                if (self(self, da->operands[0], b, depth + 1) ||
                    self(self, da->operands[1], b, depth + 1)) {
                    return true;
                }
            }
            if (da->op == IrOp::BitXor && da->operands.size() >= 2u) {
                if (self(self, da->operands[0], b, depth + 1) &&
                    self(self, da->operands[1], b, depth + 1)) {
                    return true;
                }
            }
            if ((da->op == IrOp::WaveBallotLo || da->op == IrOp::WaveBallotHi) &&
                itb != defs.end() && itb->second->op == da->op &&
                !da->operands.empty() && !itb->second->operands.empty()) {
                if (self(self, da->operands[0], itb->second->operands[0], depth + 1)) {
                    return true;
                }
            }
        }
        if (itb != defs.end()) {
            const auto* db = itb->second;
            if (db->op == IrOp::BitOr && db->operands.size() >= 2u) {
                if (self(self, a, db->operands[0], depth + 1) ||
                    self(self, a, db->operands[1], depth + 1)) {
                    return true;
                }
            }
        }
        return false;
    };

    auto are_disjoint = [&](auto&& self, IrValueId a, IrValueId b, int depth) -> bool {
        if (depth > 6) return false;
        if (is_const_zero(a) || is_const_zero(b)) return true;
        if (is_not_of(a, b) || is_not_of(b, a)) return true;
        auto ita = defs.find(a);
        auto itb = defs.find(b);
        if (ita != defs.end()) {
            const auto* da = ita->second;
            if ((da->op == IrOp::BitAnd || da->op == IrOp::LogicalAnd) && da->operands.size() >= 2u) {
                if (self(self, da->operands[0], b, depth + 1) ||
                    self(self, da->operands[1], b, depth + 1)) {
                    return true;
                }
            }
            if (da->op == IrOp::BitXor && da->operands.size() >= 2u) {
                // If s <= m, then (m ^ s) == (m & ~s), which is disjoint from any subset of s
                if (implies_subset(implies_subset, da->operands[1], da->operands[0], 0) &&
                    implies_subset(implies_subset, b, da->operands[1], 0)) {
                    return true;
                }
                if (implies_subset(implies_subset, da->operands[0], da->operands[1], 0) &&
                    implies_subset(implies_subset, b, da->operands[0], 0)) {
                    return true;
                }
            }
            if ((da->op == IrOp::WaveBallotLo || da->op == IrOp::WaveBallotHi) &&
                itb != defs.end() && itb->second->op == da->op &&
                !da->operands.empty() && !itb->second->operands.empty()) {
                if (self(self, da->operands[0], itb->second->operands[0], depth + 1)) {
                    return true;
                }
            }
        }
        if (itb != defs.end()) {
            const auto* db = itb->second;
            if ((db->op == IrOp::BitAnd || db->op == IrOp::LogicalAnd) && db->operands.size() >= 2u) {
                if (self(self, a, db->operands[0], depth + 1) ||
                    self(self, a, db->operands[1], depth + 1)) {
                    return true;
                }
            }
            if (db->op == IrOp::BitXor && db->operands.size() >= 2u) {
                if (implies_subset(implies_subset, db->operands[1], db->operands[0], 0) &&
                    implies_subset(implies_subset, a, db->operands[1], 0)) {
                    return true;
                }
                if (implies_subset(implies_subset, db->operands[0], db->operands[1], 0) &&
                    implies_subset(implies_subset, a, db->operands[0], 0)) {
                    return true;
                }
            }
        }
        return false;
    };

    for (int iter = 0; iter < 16; ++iter) {
        rebuild_defs();
        bool changed = false;

        for (auto& bb : module.blocks) {
            for (auto& inst : bb.instructions) {
                if (inst.id == kInvalidIrValue || IrOpHasSideEffects(inst.op)) continue;
                if (inst.op == IrOp::ConstU32 || inst.op == IrOp::ConstF32 ||
                    inst.op == IrOp::ThreadIdX || inst.op == IrOp::MeshGlobalThreadId ||
                    inst.op == IrOp::MergedWaveInfo || inst.op == IrOp::StageInput ||
                    inst.op == IrOp::LoadRootConst || inst.op == IrOp::LoadUav ||
                    inst.op == IrOp::BufferLoadFormat ||
                    inst.op == IrOp::ImageSample || inst.op == IrOp::ImageSampleDynamic ||
                    inst.op == IrOp::ImageSampleImplicit ||
                    inst.op == IrOp::ImageLoad ||
                    inst.op == IrOp::ImageLoadMip || inst.op == IrOp::ImageAtomicCmpSwap ||
                    inst.op == IrOp::BvhIntersectRay ||
                    inst.op == IrOp::LdsLoad || inst.op == IrOp::ScratchLoad ||
                    inst.op == IrOp::WaveLaneId ||
                    inst.op == IrOp::WaveReadFirstLane || inst.op == IrOp::WaveReadLaneAt ||
                    inst.op == IrOp::WaveBallotLo || inst.op == IrOp::WaveBallotHi ||
                    inst.op == IrOp::WaveActiveCountBits64 || inst.op == IrOp::WavePrefixCount64 ||
                    inst.op == IrOp::WavePermLane16) {
                    continue;
                }

                auto fold_to_f32 = [&](float v) {
                    inst.op = IrOp::ConstF32;
                    inst.type = IrType::F32;
                    inst.operands.clear();
                    inst.phi_incoming.clear();
                    inst.imm_f32 = v;
                    inst.imm_u32 = FloatToBits(v);
                    ++stats.folded_constants;
                    changed = true;
                };

                auto fold_to_u32 = [&](uint32_t v, IrType t) {
                    inst.op = IrOp::ConstU32;
                    inst.type = t;
                    inst.operands.clear();
                    inst.phi_incoming.clear();
                    inst.imm_u32 = v;
                    inst.imm_f32 = BitsToFloat(v);
                    ++stats.folded_constants;
                    changed = true;
                };

                float fa = 0.0f, fb = 0.0f, fc = 0.0f;
                uint32_t ua = 0, ub = 0;

                switch (inst.op) {
                    case IrOp::FAdd:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(QuietNanF32(fa + fb));
                        break;
                    case IrOp::FSub:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(QuietNanF32(fa - fb));
                        break;
                    case IrOp::FMul:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(QuietNanF32(fa * fb));
                        break;
                    case IrOp::FFma:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb) &&
                            get_const_f32(inst.operands[2], fc)) {
                            fold_to_f32(QuietNanF32(std::fmaf(fa, fb, fc)));
                        }
                        break;
                    case IrOp::FMad:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb) &&
                            get_const_f32(inst.operands[2], fc)) {
                            fold_to_f32(QuietNanF32(EvalMadF32(fa, fb, fc)));
                        }
                        break;
                    case IrOp::FFma16: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            const uint32_t f = inst.imm_u32;
                            auto read_f16 = [&](uint32_t u, int idx) -> float {
                                const bool hi = ((f >> idx) & 1u) != 0u;
                                float v = HalfBitsToFloat(SelectHalf16(u, hi));
                                if (((f >> (12 + idx)) & 1u) != 0u) v = std::fabs(v);
                                if (((f >> (8 + idx)) & 1u) != 0u) v = -v;
                                return v;
                            };
                            float r = std::fmaf(read_f16(ua, 0), read_f16(ub, 1), read_f16(uc, 2));
                            if (((f >> 16) & 1u) != 0u) r = std::fmin(std::fmax(r, 0.0f), 1.0f);
                            fold_to_u32(static_cast<uint32_t>(FloatToHalfBits(r)), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::FMin:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(ExactMinF32(fa, fb));
                        break;
                    case IrOp::FMax:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(ExactMaxF32(fa, fb));
                        break;
                    case IrOp::FRcp:
                        if (get_const_f32(inst.operands[0], fa) && fa != 0.0f) fold_to_f32(QuietNanF32(1.0f / fa));
                        break;
                    case IrOp::FSqrt:
                        if (get_const_f32(inst.operands[0], fa) && fa >= 0.0f) fold_to_f32(QuietNanF32(std::sqrt(fa)));
                        break;
                    case IrOp::FRsq:
                        if (get_const_f32(inst.operands[0], fa) && fa > 0.0f) fold_to_f32(QuietNanF32(1.0f / std::sqrt(fa)));
                        break;
                    case IrOp::FFloor:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_f32(ExactFloorF32(fa));
                        break;
                    case IrOp::FCeil:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_f32(ExactCeilF32(fa));
                        break;
                    case IrOp::FFract:
                        if (get_const_f32(inst.operands[0], fa)) {
                            fold_to_f32(std::isfinite(fa) ? QuietNanF32(fa - ExactFloorF32(fa)) : BitsToFloat(0x7FC00000u));
                        }
                        break;
                    case IrOp::IAdd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua + ub, inst.type);
                        break;
                    case IrOp::ISub:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua - ub, inst.type);
                        break;
                    case IrOp::IMul:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua * ub, inst.type);
                        break;
                    case IrOp::IMin:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(std::min(static_cast<int32_t>(ua), static_cast<int32_t>(ub)))
                                                   : std::min(ua, ub);
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::IMax:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(std::max(static_cast<int32_t>(ua), static_cast<int32_t>(ub)))
                                                   : std::max(ua, ub);
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::BitAnd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            fold_to_u32(ua & ub, IrType::U32);
                        } else if (are_disjoint(are_disjoint, inst.operands[0], inst.operands[1], 0)) {
                            ++stats.pruned_mask_ops;
                            fold_to_u32(0u, IrType::U32);
                        } else if (implies_subset(implies_subset, inst.operands[0], inst.operands[1], 0)) {
                            replace_all_uses(inst.id, inst.operands[0]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        } else if (implies_subset(implies_subset, inst.operands[1], inst.operands[0], 0)) {
                            replace_all_uses(inst.id, inst.operands[1]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        }
                        break;
                    case IrOp::BitOr:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            fold_to_u32(ua | ub, IrType::U32);
                        } else if (implies_subset(implies_subset, inst.operands[0], inst.operands[1], 0)) {
                            replace_all_uses(inst.id, inst.operands[1]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        } else if (implies_subset(implies_subset, inst.operands[1], inst.operands[0], 0)) {
                            replace_all_uses(inst.id, inst.operands[0]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        }
                        break;
                    case IrOp::BitXor:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            fold_to_u32(ua ^ ub, IrType::U32);
                        } else if (inst.operands[0] == inst.operands[1]) {
                            ++stats.pruned_mask_ops;
                            fold_to_u32(0u, IrType::U32);
                        } else if (is_const_zero(inst.operands[1])) {
                            replace_all_uses(inst.id, inst.operands[0]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        } else if (is_const_zero(inst.operands[0])) {
                            replace_all_uses(inst.id, inst.operands[1]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        } else {
                            auto it0 = defs.find(inst.operands[0]);
                            auto it1 = defs.find(inst.operands[1]);
                            if (it0 != defs.end() && it0->second->op == IrOp::BitXor && it0->second->operands.size() >= 2u) {
                                if (it0->second->operands[1] == inst.operands[1]) {
                                    replace_all_uses(inst.id, it0->second->operands[0]);
                                    ++stats.pruned_mask_ops;
                                    changed = true;
                                } else if (it0->second->operands[0] == inst.operands[1]) {
                                    replace_all_uses(inst.id, it0->second->operands[1]);
                                    ++stats.pruned_mask_ops;
                                    changed = true;
                                }
                            } else if (it1 != defs.end() && it1->second->op == IrOp::BitXor && it1->second->operands.size() >= 2u) {
                                if (it1->second->operands[1] == inst.operands[0]) {
                                    replace_all_uses(inst.id, it1->second->operands[0]);
                                    ++stats.pruned_mask_ops;
                                    changed = true;
                                } else if (it1->second->operands[0] == inst.operands[0]) {
                                    replace_all_uses(inst.id, it1->second->operands[1]);
                                    ++stats.pruned_mask_ops;
                                    changed = true;
                                }
                            }
                        }
                        break;
                    case IrOp::BitNot:
                        if (get_const_u32(inst.operands[0], ua)) {
                            fold_to_u32(~ua, IrType::U32);
                        } else {
                            auto it0 = defs.find(inst.operands[0]);
                            if (it0 != defs.end() && it0->second->op == IrOp::BitNot && !it0->second->operands.empty()) {
                                replace_all_uses(inst.id, it0->second->operands[0]);
                                ++stats.pruned_mask_ops;
                                changed = true;
                            }
                        }
                        break;
                    case IrOp::BitReverse32:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_u32(BitReverse32Cpu(ua), IrType::U32);
                        break;
                    case IrOp::BitCount32:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_u32(static_cast<uint32_t>(std::popcount(ua)), IrType::U32);
                        break;
                    case IrOp::AbsDiffI32:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const int64_t diff = static_cast<int64_t>(static_cast<int32_t>(ua)) -
                                                 static_cast<int64_t>(static_cast<int32_t>(ub));
                            fold_to_u32(static_cast<uint32_t>(diff < 0 ? -diff : diff), IrType::U32);
                        }
                        break;
                    case IrOp::Shl:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua << (ub & 31u), IrType::U32);
                        break;
                    case IrOp::Shr:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(static_cast<int32_t>(ua) >> (ub & 31u))
                                                   : (ua >> (ub & 31u));
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::AlignBit: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            fold_to_u32(EvalAlignBitB32(ua, ub, uc), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::AlignByte: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            fold_to_u32(EvalAlignByteB32(ua, ub, uc), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::BfeU32: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            const uint32_t off = ub & 31u;
                            const uint32_t wid = (inst.imm_u32 != 0u) ? (uc & 0x7Fu) : (uc & 31u);
                            uint32_t res = 0u;
                            if (wid == 0u) {
                                res = 0u;
                            } else if (off + wid < 32u) {
                                res = (ua >> off) & ((1u << wid) - 1u);
                            } else {
                                res = ua >> off;
                            }
                            fold_to_u32(res, IrType::U32);
                        }
                        break;
                    }
                    case IrOp::BfeI32: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            const uint32_t off = ub & 31u;
                            const uint32_t wid = (inst.imm_u32 != 0u) ? (uc & 0x7Fu) : (uc & 31u);
                            int32_t res = 0;
                            if (wid == 0u) {
                                res = 0;
                            } else if (off + wid < 32u) {
                                res = static_cast<int32_t>(ua << (32u - off - wid)) >> (32u - wid);
                            } else {
                                res = static_cast<int32_t>(ua) >> off;
                            }
                            fold_to_u32(static_cast<uint32_t>(res), IrType::I32);
                        }
                        break;
                    }
                    case IrOp::BfiB32: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            fold_to_u32((ua & ub) | (~ua & uc), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::PkAddU16:
                    case IrOp::PkShl16:
                    case IrOp::PkShr16: {
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t f = inst.imm_u32;
                            const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                            const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                            // Note: for PkShl16/PkShr16 operands[0] is val (ISA src1), operands[1] is shift (ISA src0)
                            const bool is_shift = (inst.op == IrOp::PkShl16 || inst.op == IrOp::PkShr16);
                            const uint16_t a_lo = SelectHalf16(ua, ((op_sel >> (is_shift ? 1 : 0)) & 1u) != 0u);
                            const uint16_t a_hi = SelectHalf16(ua, ((op_sel_hi >> (is_shift ? 1 : 0)) & 1u) != 0u);
                            const uint16_t b_lo = SelectHalf16(ub, ((op_sel >> (is_shift ? 0 : 1)) & 1u) != 0u);
                            const uint16_t b_hi = SelectHalf16(ub, ((op_sel_hi >> (is_shift ? 0 : 1)) & 1u) != 0u);
                            uint16_t r_lo = 0, r_hi = 0;
                            if (inst.op == IrOp::PkAddU16) {
                                r_lo = static_cast<uint16_t>(a_lo + b_lo);
                                r_hi = static_cast<uint16_t>(a_hi + b_hi);
                            } else if (inst.op == IrOp::PkShl16) {
                                r_lo = static_cast<uint16_t>(a_lo << (b_lo & 15u));
                                r_hi = static_cast<uint16_t>(a_hi << (b_hi & 15u));
                            } else {
                                r_lo = static_cast<uint16_t>(a_lo >> (b_lo & 15u));
                                r_hi = static_cast<uint16_t>(a_hi >> (b_hi & 15u));
                            }
                            fold_to_u32(static_cast<uint32_t>(r_lo) | (static_cast<uint32_t>(r_hi) << 16), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::PkAddF16:
                    case IrOp::PkMulF16:
                    case IrOp::PkFmaF16: {
                        uint32_t uc = 0;
                        const bool has_c = (inst.op == IrOp::PkFmaF16);
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            (!has_c || get_const_u32(inst.operands[2], uc))) {
                            const uint32_t f = inst.imm_u32;
                            const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                            const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                            const uint8_t neg_lo = static_cast<uint8_t>((f >> 8) & 7u);
                            const uint8_t neg_hi = static_cast<uint8_t>((f >> 12) & 7u);
                            const bool clamp = ((f >> 16) & 1u) != 0u;
                            auto lane_f = [&](uint32_t src, int idx, bool hi_out) -> float {
                                const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> idx) & 1u) != 0u;
                                const bool do_neg = (((hi_out ? neg_hi : neg_lo) >> idx) & 1u) != 0u;
                                float v = HalfBitsToFloat(SelectHalf16(src, pick_hi));
                                return do_neg ? -v : v;
                            };
                            auto eval_lane = [&](bool hi_out) -> uint16_t {
                                const float x = lane_f(ua, 0, hi_out);
                                const float y = lane_f(ub, 1, hi_out);
                                float r = 0.0f;
                                if (inst.op == IrOp::PkAddF16) r = x + y;
                                else if (inst.op == IrOp::PkMulF16) r = x * y;
                                else r = std::fma(x, y, lane_f(uc, 2, hi_out));
                                if (clamp) r = std::fmin(std::fmax(r, 0.0f), 1.0f);
                                return FloatToHalfBits(r);
                            };
                            const uint16_t r_lo = eval_lane(false);
                            const uint16_t r_hi = eval_lane(true);
                            fold_to_u32(static_cast<uint32_t>(r_lo) | (static_cast<uint32_t>(r_hi) << 16), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::I2F:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_f32(static_cast<float>(static_cast<int32_t>(ua)));
                        break;
                    case IrOp::U2F:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_f32(static_cast<float>(ua));
                        break;
                    case IrOp::F2I:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_u32(static_cast<uint32_t>(static_cast<int32_t>(fa)), IrType::I32);
                        break;
                    case IrOp::F2U:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_u32(static_cast<uint32_t>(fa), IrType::U32);
                        break;
                    case IrOp::CvtPkNormI16F32:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) {
                            const uint32_t lo = CvtSnorm16F32(fa);
                            const uint32_t hi = CvtSnorm16F32(fb);
                            fold_to_u32(lo | (hi << 16), IrType::U32);
                        }
                        break;
                    case IrOp::CvtPkNormU16F32:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) {
                            const uint32_t lo = CvtUnorm16F32(fa);
                            const uint32_t hi = CvtUnorm16F32(fb);
                            fold_to_u32(lo | (hi << 16), IrType::U32);
                        }
                        break;
                    case IrOp::CvtNormI16F16:
                    case IrOp::CvtNormU16F16:
                        if (get_const_u32(inst.operands[0], ua)) {
                            const bool hi = (inst.imm_u32 & 1u) != 0u;
                            float f = HalfBitsToFloat(SelectHalf16(ua, hi));
                            if (((inst.imm_u32 >> 12) & 1u) != 0u) f = std::fabs(f);
                            if (((inst.imm_u32 >> 8) & 1u) != 0u) f = -f;
                            const uint16_t out16 = (inst.op == IrOp::CvtNormI16F16)
                                                       ? CvtSnorm16F32(f)
                                                       : CvtUnorm16F32(f);
                            fold_to_u32(static_cast<uint32_t>(out16), IrType::U32);
                        }
                        break;
                    case IrOp::CmpEq:
                    case IrOp::CmpNe:
                    case IrOp::CmpLt:
                    case IrOp::CmpGt:
                    case IrOp::CmpGe: {
                        const auto* d0 = defs.count(inst.operands[0]) ? defs[inst.operands[0]] : nullptr;
                        const auto* d1 = defs.count(inst.operands[1]) ? defs[inst.operands[1]] : nullptr;
                        if (d0 && d1 && (d0->type == IrType::F32 || d1->type == IrType::F32)) {
                            if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) {
                                bool r = false;
                                if (inst.op == IrOp::CmpEq) r = (fa == fb);
                                else if (inst.op == IrOp::CmpNe) r = (!std::isnan(fa) && !std::isnan(fb) && fa != fb);
                                else if (inst.op == IrOp::CmpLt) r = (fa < fb);
                                else if (inst.op == IrOp::CmpGt) r = (fa > fb);
                                else if (inst.op == IrOp::CmpGe) r = (fa >= fb);
                                fold_to_u32(r ? 1u : 0u, IrType::Bool);
                            }
                        } else if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const int32_t sa = static_cast<int32_t>(ua);
                            const int32_t sb = static_cast<int32_t>(ub);
                            bool r = false;
                            if (inst.op == IrOp::CmpEq) r = (sa == sb);
                            else if (inst.op == IrOp::CmpNe) r = (sa != sb);
                            else if (inst.op == IrOp::CmpLt) r = (sa < sb);
                            else if (inst.op == IrOp::CmpGt) r = (sa > sb);
                            else if (inst.op == IrOp::CmpGe) r = (sa >= sb);
                            fold_to_u32(r ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    }
                    case IrOp::CmpLtU:
                    case IrOp::CmpEqU:
                    case IrOp::CmpGtU:
                    case IrOp::CmpGeU:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            bool r = false;
                            if (inst.op == IrOp::CmpLtU) r = (ua < ub);
                            else if (inst.op == IrOp::CmpEqU) r = (ua == ub);
                            else if (inst.op == IrOp::CmpGtU) r = (ua > ub);
                            else if (inst.op == IrOp::CmpGeU) r = (ua >= ub);
                            fold_to_u32(r ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    case IrOp::CmpF32:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) {
                            fold_to_u32(EvalF16Compare(static_cast<uint8_t>(inst.imm_u32 & 15u), fa, fb) ? 1u : 0u,
                                        IrType::Bool);
                        }
                        break;
                    case IrOp::CmpF16:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t imm = inst.imm_u32;
                            const uint8_t pred = static_cast<uint8_t>(imm & 15u);
                            const uint8_t op_sel = static_cast<uint8_t>((imm >> 8) & 3u);
                            const uint8_t neg_m = static_cast<uint8_t>((imm >> 12) & 3u);
                            const uint8_t abs_m = static_cast<uint8_t>((imm >> 16) & 3u);
                            float a = HalfBitsToFloat(SelectHalf16(ua, (op_sel & 1u) != 0u));
                            float b = HalfBitsToFloat(SelectHalf16(ub, (op_sel & 2u) != 0u));
                            if ((abs_m & 1u) != 0u) a = std::fabs(a);
                            if ((abs_m & 2u) != 0u) b = std::fabs(b);
                            if ((neg_m & 1u) != 0u) a = -a;
                            if ((neg_m & 2u) != 0u) b = -b;
                            fold_to_u32(EvalF16Compare(pred, a, b) ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    case IrOp::LogicalAnd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            fold_to_u32((ua != 0u && ub != 0u) ? 1u : 0u, IrType::Bool);
                        } else if (are_disjoint(are_disjoint, inst.operands[0], inst.operands[1], 0)) {
                            ++stats.pruned_mask_ops;
                            fold_to_u32(0u, IrType::Bool);
                        } else if (implies_subset(implies_subset, inst.operands[0], inst.operands[1], 0)) {
                            replace_all_uses(inst.id, inst.operands[0]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        } else if (implies_subset(implies_subset, inst.operands[1], inst.operands[0], 0)) {
                            replace_all_uses(inst.id, inst.operands[1]);
                            ++stats.pruned_mask_ops;
                            changed = true;
                        }
                        break;
                    case IrOp::LogicalNot:
                        if (get_const_u32(inst.operands[0], ua)) {
                            fold_to_u32((ua == 0u) ? 1u : 0u, IrType::Bool);
                        } else {
                            auto it0 = defs.find(inst.operands[0]);
                            if (it0 != defs.end() && it0->second->op == IrOp::LogicalNot && !it0->second->operands.empty()) {
                                replace_all_uses(inst.id, it0->second->operands[0]);
                                ++stats.pruned_mask_ops;
                                changed = true;
                            }
                        }
                        break;
                    case IrOp::Select:
                        if (get_const_u32(inst.operands[0], ua)) {
                            const IrValueId chosen = (ua != 0u) ? inst.operands[1] : inst.operands[2];
                            if (inst.type == IrType::F32 && get_const_f32(chosen, fa)) {
                                fold_to_f32(fa);
                            } else if (inst.type != IrType::F32 && get_const_u32(chosen, ub)) {
                                fold_to_u32(ub, inst.type);
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        if (!changed) break;
    }

    // Pass 1.75: Folded EXECZ / Constant Conditional Branch-to-Join CFG Fix (Leonida #4574)
    // When EXEC or SCC folds to a constant boolean before S_CBRANCH_EXECZ/EXECNZ/SCC*, rewrite
    // CondBranch -> direct Branch to the taken target (join block), prune dead unreachable region
    // blocks, and remove dead incoming edges from join-block Phi nodes so post-join active code
    // executes cleanly without being skipped or polluted by dead-region values.
    for (int cfg_iter = 0; cfg_iter < 8; ++cfg_iter) {
        rebuild_defs();
        bool cfg_changed = false;
        for (auto& bb : module.blocks) {
            if (bb.instructions.empty()) continue;
            auto& term = bb.instructions.back();
            if (term.op != IrOp::CondBranch) continue;
            uint32_t taken_bb = 0u;
            bool fold_branch = false;
            if (term.true_block == term.false_block) {
                taken_bb = term.true_block;
                fold_branch = true;
            } else if (!term.operands.empty()) {
                uint32_t cond_u = 0u;
                if (get_const_u32(term.operands[0], cond_u)) {
                    taken_bb = (cond_u != 0u) ? term.true_block : term.false_block;
                    fold_branch = true;
                }
            }
            if (fold_branch) {
                term.op = IrOp::Branch;
                term.operands.clear();
                term.true_block = taken_bb;
                term.false_block = 0u;
                ++stats.folded_execz_branches;
                cfg_changed = true;
            }
        }
        if (!cfg_changed) break;

        // Recompute CFG successors & BFS reachability from block 0
        const size_t n_blocks = module.blocks.size();
        for (auto& bb : module.blocks) {
            bb.successors.clear();
            bb.predecessors.clear();
            if (bb.instructions.empty()) continue;
            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Branch && term.true_block < n_blocks) {
                AddUnique(bb.successors, term.true_block);
            } else if (term.op == IrOp::CondBranch) {
                if (term.true_block < n_blocks) AddUnique(bb.successors, term.true_block);
                if (term.false_block < n_blocks) AddUnique(bb.successors, term.false_block);
            }
        }

        std::vector<bool> reachable(n_blocks, false);
        std::vector<uint32_t> bfs;
        if (n_blocks > 0u) {
            reachable[0] = true;
            bfs.push_back(0u);
        }
        for (size_t head = 0u; head < bfs.size(); ++head) {
            const uint32_t u = bfs[head];
            for (uint32_t s : module.blocks[u].successors) {
                if (s < n_blocks && !reachable[s]) {
                    reachable[s] = true;
                    bfs.push_back(s);
                }
            }
        }

        // Clear dead unreachable blocks (preserving block IDs) and rebuild reachable predecessors
        for (size_t b = 0u; b < n_blocks; ++b) {
            if (!reachable[b]) {
                module.blocks[b].successors.clear();
                module.blocks[b].instructions.clear();
                IrInstruction ret{};
                ret.op = IrOp::Return;
                ret.type = IrType::Void;
                module.blocks[b].instructions.push_back(std::move(ret));
            } else {
                for (uint32_t s : module.blocks[b].successors) {
                    if (s < n_blocks && reachable[s]) {
                        AddUnique(module.blocks[s].predecessors, static_cast<uint32_t>(b));
                    }
                }
            }
        }

        // Prune unreachable incoming edges from Phi nodes & simplify trivial single-edge Phis
        for (size_t b = 0u; b < n_blocks; ++b) {
            if (!reachable[b]) continue;
            auto& bb = module.blocks[b];
            std::vector<IrInstruction> updated;
            updated.reserve(bb.instructions.size());
            for (auto& inst : bb.instructions) {
                if (inst.op == IrOp::Phi) {
                    std::vector<std::pair<uint32_t, IrValueId>> valid_inc;
                    for (const auto& inc : inst.phi_incoming) {
                        if (std::find(bb.predecessors.begin(), bb.predecessors.end(), inc.first) != bb.predecessors.end()) {
                            valid_inc.push_back(inc);
                        }
                    }
                    inst.phi_incoming = std::move(valid_inc);
                    if (inst.phi_incoming.size() == 1u) {
                        replace_all_uses(inst.id, inst.phi_incoming[0].second);
                        ++stats.eliminated_ops;
                        continue;
                    }
                    if (inst.phi_incoming.size() > 1u) {
                        bool all_same = true;
                        const IrValueId v0 = inst.phi_incoming[0].second;
                        for (size_t k = 1u; k < inst.phi_incoming.size(); ++k) {
                            if (inst.phi_incoming[k].second != v0) {
                                all_same = false;
                                break;
                            }
                        }
                        if (all_same) {
                            replace_all_uses(inst.id, v0);
                            ++stats.eliminated_ops;
                            continue;
                        }
                    }
                }
                updated.push_back(std::move(inst));
            }
            bb.instructions = std::move(updated);
        }
    }

    // Pass 2: Dead-Code Elimination (mark-and-sweep from side-effecting roots)
    rebuild_defs();
    std::unordered_set<IrValueId> live;
    std::vector<IrValueId> worklist;

    auto mark_use = [&](IrValueId id) {
        if (id != kInvalidIrValue && live.insert(id).second) {
            worklist.push_back(id);
        }
    };

    for (const auto& bb : module.blocks) {
        for (const auto& inst : bb.instructions) {
            if (IrOpHasSideEffects(inst.op)) {
                if (inst.id != kInvalidIrValue) {
                    live.insert(inst.id);
                }
                for (IrValueId op_id : inst.operands) {
                    mark_use(op_id);
                }
                if (inst.exec_pred != kInvalidIrValue) {
                    mark_use(inst.exec_pred);
                }
            }
        }
    }

    while (!worklist.empty()) {
        const IrValueId cur = worklist.back();
        worklist.pop_back();
        auto it = defs.find(cur);
        if (it == defs.end()) continue;
        const auto* d = it->second;
        for (IrValueId op_id : d->operands) {
            mark_use(op_id);
        }
        for (const auto& incoming : d->phi_incoming) {
            mark_use(incoming.second);
        }
    }

    for (auto& bb : module.blocks) {
        std::vector<IrInstruction> kept;
        kept.reserve(bb.instructions.size());
        for (auto& inst : bb.instructions) {
            if (IrOpHasSideEffects(inst.op) || (inst.id != kInvalidIrValue && live.count(inst.id) > 0)) {
                kept.push_back(std::move(inst));
            } else {
                ++stats.eliminated_ops;
            }
        }
        bb.instructions = std::move(kept);
    }

    // Pass 3: Single-wave lockstep LDS barrier insertion for 1-wave (<= 64 threads) shaders
    InsertSingleWaveLockstepLdsBarriers(module);
    stats.inserted_lockstep_lds_barriers = module.inserted_lockstep_lds_barriers;

    for (const auto& bb : module.blocks) {
        stats.final_ops += static_cast<uint32_t>(bb.instructions.size());
    }

    module.opt_stats = stats;
    if (out_stats) {
        *out_stats = stats;
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Single-Wave Lockstep LDS Barrier Insertion Pass
// ---------------------------------------------------------------------------
uint32_t InsertSingleWaveLockstepLdsBarriers(Rdna2IrModule& module) {
    if (!module.uses_lds) return 0u;
    if (module.stage != ShaderStage::Compute &&
        module.stage != ShaderStage::Mesh &&
        module.stage != ShaderStage::Amplification) {
        return 0u;
    }

    const uint64_t total_threads =
        static_cast<uint64_t>(module.threadgroup_x ? module.threadgroup_x : 1u) *
        static_cast<uint64_t>(module.threadgroup_y ? module.threadgroup_y : 1u) *
        static_cast<uint64_t>(module.threadgroup_z ? module.threadgroup_z : 1u);
    // Console single-wave groups (<= 64 threads) execute in lockstep on AMD Wave64 hardware
    // without requiring s_barrier, whereas 32-lane NVIDIA / 16-lane Intel / WARP GPUs split
    // them into independent waves that race on LDS unless explicit group barriers are inserted.
    if (total_threads <= 1u || total_threads > 64u) {
        return 0u;
    }

    auto make_barrier = []() -> IrInstruction {
        IrInstruction bar{};
        bar.op = IrOp::Barrier;
        bar.type = IrType::Void;
        return bar;
    };

    uint32_t inserted = 0u;

    // 1. Intra-block LDS hazard detection & barrier insertion
    for (auto& bb : module.blocks) {
        bool written = false;
        bool read = false;
        bool atomic_written = false;
        std::vector<IrInstruction> rewritten;
        rewritten.reserve(bb.instructions.size() + 4u);

        for (auto& inst : bb.instructions) {
            if (inst.op == IrOp::Barrier) {
                written = false;
                read = false;
                atomic_written = false;
            } else if (inst.op == IrOp::LdsLoad) {
                if (written || atomic_written) {
                    rewritten.push_back(make_barrier());
                    ++inserted;
                    written = false;
                    read = false;
                    atomic_written = false;
                }
                read = true;
            } else if (inst.op == IrOp::LdsStore) {
                if (read || atomic_written) {
                    rewritten.push_back(make_barrier());
                    ++inserted;
                    written = false;
                    read = false;
                    atomic_written = false;
                }
                written = true;
            } else if (inst.op == IrOp::LdsAtomicAdd) {
                if (written || read) {
                    rewritten.push_back(make_barrier());
                    ++inserted;
                    written = false;
                    read = false;
                    atomic_written = false;
                }
                atomic_written = true;
            }
            rewritten.push_back(std::move(inst));
        }
        bb.instructions = std::move(rewritten);
    }

    // 2. Cross-block LDS hazard propagation across CFG edges (e.g. write in bb0 -> read in bb1)
    const size_t n = module.blocks.size();
    if (n > 1u) {
        struct BlockLdsSummary {
            bool has_barrier = false;
            uint8_t first_op = 0;  // 0=none, 1=load, 2=store, 3=atomic
            size_t first_op_idx = 0;
            bool out_written = false;
            bool out_read = false;
            bool out_atomic = false;
        };
        std::vector<BlockLdsSummary> sum(n);
        for (size_t b = 0; b < n; ++b) {
            const auto& ins = module.blocks[b].instructions;
            bool w = false, r = false, a = false;
            for (size_t i = 0; i < ins.size(); ++i) {
                const IrOp op = ins[i].op;
                if (op == IrOp::Barrier) {
                    sum[b].has_barrier = true;
                    w = r = a = false;
                } else if (op == IrOp::LdsLoad) {
                    if (!sum[b].has_barrier && sum[b].first_op == 0u) {
                        sum[b].first_op = 1u;
                        sum[b].first_op_idx = i;
                    }
                    r = true;
                } else if (op == IrOp::LdsStore) {
                    if (!sum[b].has_barrier && sum[b].first_op == 0u) {
                        sum[b].first_op = 2u;
                        sum[b].first_op_idx = i;
                    }
                    w = true;
                } else if (op == IrOp::LdsAtomicAdd) {
                    if (!sum[b].has_barrier && sum[b].first_op == 0u) {
                        sum[b].first_op = 3u;
                        sum[b].first_op_idx = i;
                    }
                    a = true;
                }
            }
            sum[b].out_written = w;
            sum[b].out_read = r;
            sum[b].out_atomic = a;
        }

        std::vector<uint8_t> in_w(n, 0), in_r(n, 0), in_a(n, 0);
        std::vector<uint8_t> out_w(n, 0), out_r(n, 0), out_a(n, 0);
        for (size_t b = 0; b < n; ++b) {
            out_w[b] = sum[b].out_written ? 1u : 0u;
            out_r[b] = sum[b].out_read ? 1u : 0u;
            out_a[b] = sum[b].out_atomic ? 1u : 0u;
        }
        bool changed = true;
        for (size_t iter = 0; changed && iter < n * 4u; ++iter) {
            changed = false;
            for (size_t b = 0; b < n; ++b) {
                uint8_t nw = 0, nr = 0, na = 0;
                for (uint32_t p : module.blocks[b].predecessors) {
                    if (p < n) {
                        nw |= out_w[p];
                        nr |= out_r[p];
                        na |= out_a[p];
                    }
                }
                if (nw != in_w[b] || nr != in_r[b] || na != in_a[b]) {
                    in_w[b] = nw;
                    in_r[b] = nr;
                    in_a[b] = na;
                    changed = true;
                }
                const uint8_t ew = sum[b].has_barrier ? (sum[b].out_written ? 1u : 0u) : (nw | (sum[b].out_written ? 1u : 0u));
                const uint8_t er = sum[b].has_barrier ? (sum[b].out_read ? 1u : 0u) : (nr | (sum[b].out_read ? 1u : 0u));
                const uint8_t ea = sum[b].has_barrier ? (sum[b].out_atomic ? 1u : 0u) : (na | (sum[b].out_atomic ? 1u : 0u));
                if (ew != out_w[b] || er != out_r[b] || ea != out_a[b]) {
                    out_w[b] = ew;
                    out_r[b] = er;
                    out_a[b] = ea;
                    changed = true;
                }
            }
        }

        std::unordered_set<uint32_t> barrier_before_term_blocks;
        for (size_t b = 0; b < n; ++b) {
            const bool hazard =
                (sum[b].first_op == 1u && (in_w[b] || in_a[b])) ||
                (sum[b].first_op == 2u && (in_r[b] || in_a[b])) ||
                (sum[b].first_op == 3u && (in_w[b] || in_r[b]));
            if (!hazard) continue;

            // If entered solely from a conditional branch, place barrier uniformly before pred's CondBranch
            if (module.blocks[b].predecessors.size() == 1u) {
                const uint32_t p = module.blocks[b].predecessors[0];
                if (p < n && !module.blocks[p].instructions.empty() &&
                    module.blocks[p].instructions.back().op == IrOp::CondBranch) {
                    if (barrier_before_term_blocks.insert(p).second) {
                        auto& pins = module.blocks[p].instructions;
                        pins.insert(pins.end() - 1, make_barrier());
                        ++inserted;
                    }
                    continue;
                }
            }
            auto& ins = module.blocks[b].instructions;
            ins.insert(ins.begin() + static_cast<std::ptrdiff_t>(sum[b].first_op_idx), make_barrier());
            ++inserted;
        }
    }

    module.inserted_lockstep_lds_barriers += inserted;
    module.opt_stats.inserted_lockstep_lds_barriers = module.inserted_lockstep_lds_barriers;
    return inserted;
}

std::string DumpRdna2Ir(const Rdna2IrModule& module) {
    std::ostringstream os;
    os << "; OpenScarlett RDNA2 SSA IR (blocks=" << module.blocks.size()
       << ", tg=" << module.threadgroup_x << "x" << module.threadgroup_y << "x" << module.threadgroup_z << ")\n";
    for (const auto& bb : module.blocks) {
        os << "bb" << bb.id << ":";
        if (!bb.predecessors.empty()) {
            os << "  ; preds=";
            for (size_t i = 0; i < bb.predecessors.size(); ++i) {
                if (i) os << ",";
                os << "bb" << bb.predecessors[i];
            }
        }
        os << "\n";
        for (const auto& inst : bb.instructions) {
            os << "  ";
            if (inst.id != kInvalidIrValue) {
                os << "%" << inst.id << " : " << IrTypeName(inst.type) << " = ";
            }
            os << IrOpName(inst.op);
            if (inst.op == IrOp::ConstU32) {
                os << " " << inst.imm_u32;
            } else if (inst.op == IrOp::ConstF32) {
                os << " " << inst.imm_f32;
            } else if (inst.op == IrOp::LoadRootConst || inst.op == IrOp::StageInput) {
                os << " " << inst.imm_u32;
            } else if (inst.op == IrOp::Phi) {
                for (size_t i = 0; i < inst.phi_incoming.size(); ++i) {
                    os << (i ? ", " : " ") << "[bb" << inst.phi_incoming[i].first << ": %" << inst.phi_incoming[i].second << "]";
                }
            } else if (inst.op == IrOp::Branch) {
                os << " bb" << inst.true_block;
            } else if (inst.op == IrOp::CondBranch) {
                os << " %" << inst.operands[0] << ", bb" << inst.true_block << ", bb" << inst.false_block;
            } else {
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    os << (i ? ", " : " ") << "%" << inst.operands[i];
                }
                if (inst.op == IrOp::Export) {
                    os << " (target=" << static_cast<uint32_t>(inst.export_target) << ")";
                }
            }
            os << "\n";
        }
    }
    return os.str();
}

}  // namespace openscarlett
