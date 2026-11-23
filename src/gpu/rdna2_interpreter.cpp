#include "openscarlett/gpu/rdna2_interpreter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace openscarlett {

namespace {

uint32_t FloatToBits(float f) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

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
            uint32_t e = 113u;
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

uint16_t PickHalf16(uint32_t packed, bool hi) noexcept {
    return static_cast<uint16_t>(hi ? ((packed >> 16) & 0xFFFFu) : (packed & 0xFFFFu));
}

uint32_t PopCount32(uint32_t v) noexcept {
    return static_cast<uint32_t>(std::popcount(v));
}

uint32_t PopCount64(uint64_t v) noexcept {
    return static_cast<uint32_t>(std::popcount(v));
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
    const uint32_t rshift = (byte_sel & 3u) << 3u;
    const uint32_t lo_part = lo >> rshift;
    const uint32_t hi_shift = (32u - rshift) & 31u;
    const uint32_t hi_part = (rshift != 0u) ? (hi << hi_shift) : 0u;
    return lo_part | hi_part;
}

uint32_t EvalAlignBitB32(uint32_t hi, uint32_t lo, uint32_t shift_src) noexcept {
    const uint32_t sh = shift_src & 31u;
    if (sh == 0u) return lo;
    return (lo >> sh) | (hi << (32u - sh));
}

uint32_t BitReverse32Cpu(uint32_t x) noexcept {
    x = ((x & 0x55555555u) << 1) | ((x >> 1) & 0x55555555u);
    x = ((x & 0x33333333u) << 2) | ((x >> 2) & 0x33333333u);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x >> 4) & 0x0F0F0F0Fu);
    x = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
    return (x << 16) | (x >> 16);
}

bool EvalF32CompareCpu(Rdna2Opcode op, float a, float b) noexcept {
    const bool unord = std::isnan(a) || std::isnan(b);
    switch (op) {
        case Rdna2Opcode::V_CMP_F_F32:    return false;
        case Rdna2Opcode::V_CMP_LT_F32:
        case Rdna2Opcode::V_CMPX_LT_F32:  return !unord && (a < b);
        case Rdna2Opcode::V_CMP_EQ_F32:
        case Rdna2Opcode::V_CMPX_EQ_F32:  return !unord && (a == b);
        case Rdna2Opcode::V_CMP_LE_F32:
        case Rdna2Opcode::V_CMPX_LE_F32:  return !unord && (a <= b);
        case Rdna2Opcode::V_CMP_GT_F32:
        case Rdna2Opcode::V_CMPX_GT_F32:  return !unord && (a > b);
        case Rdna2Opcode::V_CMP_LG_F32:
        case Rdna2Opcode::V_CMPX_LG_F32:  return !unord && (a != b);
        case Rdna2Opcode::V_CMP_GE_F32:
        case Rdna2Opcode::V_CMPX_GE_F32:  return !unord && (a >= b);
        case Rdna2Opcode::V_CMP_O_F32:    return !unord;
        case Rdna2Opcode::V_CMP_U_F32:    return unord;
        case Rdna2Opcode::V_CMP_NGE_F32:  return unord || (a < b);
        case Rdna2Opcode::V_CMP_NLG_F32:  return unord || (a == b);
        case Rdna2Opcode::V_CMP_NGT_F32:  return unord || (a <= b);
        case Rdna2Opcode::V_CMP_NLE_F32:  return unord || (a > b);
        case Rdna2Opcode::V_CMP_NEQ_F32:  return unord || (a != b);
        case Rdna2Opcode::V_CMP_NLT_F32:  return unord || (a >= b);
        case Rdna2Opcode::V_CMP_TRU_F32:  return true;
        default:                          return false;
    }
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

uint32_t EvalSBfeU32(uint32_t src0, uint32_t src1) noexcept {
    const uint32_t off = src1 & 31u;
    const uint32_t wid = (src1 >> 16) & 0x7Fu;
    if (wid == 0u || off >= 32u) return 0u;
    if (off + wid < 32u) return (src0 >> off) & ((1u << wid) - 1u);
    return src0 >> off;
}

uint32_t EvalSBfeI32(uint32_t src0, uint32_t src1) noexcept {
    const uint32_t off = src1 & 31u;
    const uint32_t wid = (src1 >> 16) & 0x7Fu;
    if (wid == 0u) return 0u;
    if (off >= 32u) return static_cast<uint32_t>(static_cast<int32_t>(src0) >> 31);
    if (off + wid < 32u) {
        return static_cast<uint32_t>(static_cast<int32_t>(src0 << (32u - off - wid)) >> (32u - wid));
    }
    return static_cast<uint32_t>(static_cast<int32_t>(src0) >> off);
}

bool EvalF16CompareCpu(Rdna2Opcode op, float a, float b) noexcept {
    const bool unord = std::isnan(a) || std::isnan(b);
    switch (op) {
        case Rdna2Opcode::V_CMP_LT_F16:
        case Rdna2Opcode::V_CMPX_LT_F16:  return !unord && (a < b);
        case Rdna2Opcode::V_CMP_EQ_F16:
        case Rdna2Opcode::V_CMPX_EQ_F16:  return !unord && (a == b);
        case Rdna2Opcode::V_CMP_LE_F16:
        case Rdna2Opcode::V_CMPX_LE_F16:  return !unord && (a <= b);
        case Rdna2Opcode::V_CMP_GT_F16:
        case Rdna2Opcode::V_CMPX_GT_F16:  return !unord && (a > b);
        case Rdna2Opcode::V_CMP_LG_F16:
        case Rdna2Opcode::V_CMPX_LG_F16:  return !unord && (a != b);
        case Rdna2Opcode::V_CMP_GE_F16:
        case Rdna2Opcode::V_CMPX_GE_F16:  return !unord && (a >= b);
        case Rdna2Opcode::V_CMP_U_F16:    return unord;
        case Rdna2Opcode::V_CMP_NGE_F16:  return unord || (a < b);
        case Rdna2Opcode::V_CMP_NLG_F16:  return unord || (a == b);
        case Rdna2Opcode::V_CMP_NGT_F16:  return unord || (a <= b);
        case Rdna2Opcode::V_CMP_NLE_F16:  return unord || (a > b);
        case Rdna2Opcode::V_CMP_NEQ_F16:
        case Rdna2Opcode::V_CMPX_NEQ_F16: return unord || (a != b);
        case Rdna2Opcode::V_CMP_NLT_F16:
        case Rdna2Opcode::V_CMPX_NLT_F16: return unord || (a >= b);
        case Rdna2Opcode::V_CMP_TRU_F16:  return true;
        default:                          return false;
    }
}

uint32_t ReadUnalignedU32Cpu(const std::vector<uint32_t>& buf, uint32_t byte_addr) noexcept {
    // Strictly UB-free sub-dword merge: all shifts guarded < 32
    const uint32_t aligned_addr = byte_addr & ~3u;
    const uint32_t bit_off = (byte_addr & 3u) << 3u;
    const size_t idx0 = static_cast<size_t>(aligned_addr >> 2);
    const uint32_t w0 = (idx0 < buf.size()) ? buf[idx0] : 0u;
    const uint32_t w1 = (idx0 + 1u < buf.size()) ? buf[idx0 + 1u] : 0u;
    const uint32_t hi_shift = (32u - bit_off) & 31u;
    return (w0 >> bit_off) | ((bit_off != 0u) ? (w1 << hi_shift) : 0u);
}

uint32_t DecodeFormatComponentCpu(
    const std::vector<uint32_t>& buf,
    uint32_t base_byte_addr,
    Rdna2MtbufFormat fmt,
    uint32_t comp_idx,
    bool as_d16_lane) noexcept {
    auto read_u8 = [&](uint32_t off) -> uint32_t {
        return ReadUnalignedU32Cpu(buf, base_byte_addr + off) & 0xFFu;
    };
    auto read_u16 = [&](uint32_t off) -> uint32_t {
        return ReadUnalignedU32Cpu(buf, base_byte_addr + off) & 0xFFFFu;
    };
    auto read_u32 = [&](uint32_t off) -> uint32_t {
        return ReadUnalignedU32Cpu(buf, base_byte_addr + off);
    };
    auto to_out_float = [&](float f) -> uint32_t {
        return as_d16_lane ? static_cast<uint32_t>(FloatToHalfBits(f)) : FloatToBits(f);
    };
    auto to_out_u16 = [&](uint32_t u) -> uint32_t {
        return as_d16_lane ? (u & 0xFFFFu) : u;
    };

    switch (fmt) {
        case Rdna2MtbufFormat::R8_UNORM:
        case Rdna2MtbufFormat::R8G8_UNORM:
        case Rdna2MtbufFormat::R8G8B8A8_UNORM:
            return to_out_float(static_cast<float>(read_u8(comp_idx)) / 255.0f);
        case Rdna2MtbufFormat::R8_SNORM:
        case Rdna2MtbufFormat::R8G8_SNORM:
        case Rdna2MtbufFormat::R8G8B8A8_SNORM: {
            const int8_t s = static_cast<int8_t>(read_u8(comp_idx));
            const float f = std::clamp(static_cast<float>(s) / 127.0f, -1.0f, 1.0f);
            return to_out_float(f);
        }
        case Rdna2MtbufFormat::R8_UINT:
        case Rdna2MtbufFormat::R8G8_UINT:
        case Rdna2MtbufFormat::R8G8B8A8_UINT:
            return to_out_u16(read_u8(comp_idx));
        case Rdna2MtbufFormat::R8_SINT:
        case Rdna2MtbufFormat::R8G8_SINT:
        case Rdna2MtbufFormat::R8G8B8A8_SINT: {
            const int32_t s = static_cast<int8_t>(read_u8(comp_idx));
            return to_out_u16(static_cast<uint32_t>(s));
        }
        case Rdna2MtbufFormat::R16_UNORM:
        case Rdna2MtbufFormat::R16G16_UNORM:
        case Rdna2MtbufFormat::R16G16B16A16_UNORM:
            return to_out_float(static_cast<float>(read_u16(comp_idx * 2u)) / 65535.0f);
        case Rdna2MtbufFormat::R16_SNORM:
        case Rdna2MtbufFormat::R16G16_SNORM:
        case Rdna2MtbufFormat::R16G16B16A16_SNORM: {
            const int16_t s = static_cast<int16_t>(read_u16(comp_idx * 2u));
            const float f = std::clamp(static_cast<float>(s) / 32767.0f, -1.0f, 1.0f);
            return to_out_float(f);
        }
        case Rdna2MtbufFormat::R16_UINT:
        case Rdna2MtbufFormat::R16G16_UINT:
        case Rdna2MtbufFormat::R16G16B16A16_UINT:
            return to_out_u16(read_u16(comp_idx * 2u));
        case Rdna2MtbufFormat::R16_SINT:
        case Rdna2MtbufFormat::R16G16_SINT:
        case Rdna2MtbufFormat::R16G16B16A16_SINT: {
            const int32_t s = static_cast<int16_t>(read_u16(comp_idx * 2u));
            return to_out_u16(static_cast<uint32_t>(s));
        }
        case Rdna2MtbufFormat::R16_FLOAT:
        case Rdna2MtbufFormat::R16G16_FLOAT:
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT: {
            const uint16_t h = static_cast<uint16_t>(read_u16(comp_idx * 2u));
            return as_d16_lane ? static_cast<uint32_t>(h) : FloatToBits(HalfBitsToFloat(h));
        }
        case Rdna2MtbufFormat::R32_UINT:
        case Rdna2MtbufFormat::R32_SINT:
            return to_out_u16(read_u32(comp_idx * 4u));
        case Rdna2MtbufFormat::R32_FLOAT:
        case Rdna2MtbufFormat::R32G32_FLOAT:
        case Rdna2MtbufFormat::R32G32B32_FLOAT:
        case Rdna2MtbufFormat::R32G32B32A32_FLOAT:
        default: {
            const uint32_t raw = read_u32(comp_idx * 4u);
            return as_d16_lane ? static_cast<uint32_t>(FloatToHalfBits(BitsToFloat(raw))) : raw;
        }
    }
}

float EvalMadF32(float a, float b, float c) noexcept {
    const float fa = FlushDenormF32(a);
    const float fb = FlushDenormF32(b);
    const float fc = FlushDenormF32(c);
    const float prod = FlushDenormF32(fa * fb);
    return FlushDenormF32(prod + fc);
}

// Bit-exact RDNA2 f32 v_rcp_f32 / v_rcp_iflag_f32 / v_rsq_f32 / v_sqrt_f32 helpers
// handling +/-0, +/-inf, negative inputs, and sNaN/qNaN per AMD GFX10.3 ISA.
float ExactRcpF32(float x) noexcept {
    const uint32_t b = FloatToBits(x);
    const uint32_t sign = b & 0x80000000u;
    const uint32_t mag = b & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return BitsToFloat(0x7FC00000u);  // NaN -> canonical qNaN
    if (mag == 0u) return BitsToFloat(sign | 0x7F800000u);   // +/-0 -> +/-Inf
    if (mag == 0x7F800000u) return BitsToFloat(sign);        // +/-Inf -> +/-0
    return 1.0f / x;
}

float ExactRsqF32(float x) noexcept {
    const uint32_t b = FloatToBits(x);
    const uint32_t sign = b & 0x80000000u;
    const uint32_t mag = b & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return BitsToFloat(0x7FC00000u);  // NaN -> canonical qNaN
    if (mag == 0u) return BitsToFloat(sign | 0x7F800000u);   // +/-0 -> +/-Inf
    if (sign != 0u) return BitsToFloat(0x7FC00000u);         // negative (incl. -Inf) -> qNaN
    if (mag == 0x7F800000u) return 0.0f;                     // +Inf -> +0.0
    return 1.0f / std::sqrt(x);
}

float ExactSqrtF32(float x) noexcept {
    const uint32_t b = FloatToBits(x);
    const uint32_t sign = b & 0x80000000u;
    const uint32_t mag = b & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return BitsToFloat(0x7FC00000u);  // NaN -> canonical qNaN
    if (mag == 0u) return BitsToFloat(sign);                 // +/-0 -> +/-0
    if (sign != 0u) return BitsToFloat(0x7FC00000u);         // negative (incl. -Inf) -> qNaN
    if (mag == 0x7F800000u) return BitsToFloat(0x7F800000u); // +Inf -> +Inf
    return std::sqrt(x);
}

bool IsBranchOp(Rdna2Opcode op) noexcept {
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

std::vector<size_t> ComputeInstructionIpdom(
    const std::vector<Rdna2Instruction>& instructions,
    const std::unordered_map<size_t, size_t>& branch_targets) {
    const size_t n = instructions.size();
    const size_t exit_node = n;
    std::vector<std::vector<size_t>> succs(n);
    for (size_t i = 0; i < n; ++i) {
        const auto op = instructions[i].opcode;
        if (op == Rdna2Opcode::S_ENDPGM) {
            succs[i].push_back(exit_node);
        } else if (op == Rdna2Opcode::S_BRANCH) {
            auto it = branch_targets.find(i);
            succs[i].push_back(it != branch_targets.end() ? it->second : exit_node);
        } else if (IsBranchOp(op)) {
            auto it = branch_targets.find(i);
            const size_t tgt = (it != branch_targets.end()) ? it->second : exit_node;
            succs[i].push_back(tgt);
            const size_t fall = (i + 1 < n) ? (i + 1) : exit_node;
            if (fall != tgt) {
                succs[i].push_back(fall);
            }
        } else {
            succs[i].push_back((i + 1 < n) ? (i + 1) : exit_node);
        }
    }

    std::vector<std::vector<uint8_t>> pdom(n + 1, std::vector<uint8_t>(n + 1, 1u));
    pdom[exit_node].assign(n + 1, 0u);
    pdom[exit_node][exit_node] = 1u;

    bool changed = true;
    for (size_t iter = 0; changed && iter < (n + 2) * 8; ++iter) {
        changed = false;
        for (size_t i = n; i-- > 0;) {
            std::vector<uint8_t> next_set(n + 1, 1u);
            for (size_t s : succs[i]) {
                for (size_t k = 0; k <= n; ++k) {
                    next_set[k] = static_cast<uint8_t>(next_set[k] & pdom[s][k]);
                }
            }
            next_set[i] = 1u;
            if (next_set != pdom[i]) {
                pdom[i] = std::move(next_set);
                changed = true;
            }
        }
    }

    std::vector<size_t> pdom_size(n + 1, 0);
    for (size_t i = 0; i <= n; ++i) {
        size_t c = 0;
        for (uint8_t b : pdom[i]) c += b;
        pdom_size[i] = c;
    }

    std::vector<size_t> ipdom(n, exit_node);
    for (size_t i = 0; i < n; ++i) {
        size_t best = exit_node;
        size_t best_size = 0;
        for (size_t d = 0; d <= n; ++d) {
            if (d == i || !pdom[i][d]) continue;
            if (pdom_size[d] > best_size) {
                best_size = pdom_size[d];
                best = d;
            }
        }
        ipdom[i] = best;
    }
    return ipdom;
}

uint32_t ReadBufferWord(const std::vector<uint32_t>& buf, uint32_t byte_addr) noexcept {
    const size_t idx = static_cast<size_t>(byte_addr >> 2);
    return (idx < buf.size()) ? buf[idx] : 0u;
}

void WriteBufferWord(std::vector<uint32_t>& buf, uint32_t byte_addr, uint32_t val) noexcept {
    const size_t idx = static_cast<size_t>(byte_addr >> 2);
    if (idx < buf.size()) {
        buf[idx] = val;
    }
}

struct BvhHitResult {
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t hit_id = 0xFFFFFFFFu;
};

BvhHitResult EvalBvhIntersectRayCpu(
    const std::vector<uint32_t>& uav0,
    uint32_t node_ptr,
    float tmax,
    float ox,
    float oy,
    float oz,
    float dx,
    float dy,
    float dz) noexcept {
    BvhHitResult miss{tmax, 0.0f, 0.0f, 0xFFFFFFFFu};
    const uint32_t hdr = ReadBufferWord(uav0, node_ptr);
    const uint32_t hit_id = hdr & 0x7FFFFFFFu;

    if ((hdr & 0x80000000u) == 0u) {
        // Triangle leaf node (Möller-Trumbore)
        const float v0x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 4u));
        const float v0y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 8u));
        const float v0z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 12u));
        const float v1x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 16u));
        const float v1y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 20u));
        const float v1z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 24u));
        const float v2x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 28u));
        const float v2y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 32u));
        const float v2z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 36u));

        const float e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
        const float e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;
        const float px = dy * e2z - dz * e2y;
        const float py = dz * e2x - dx * e2z;
        const float pz = dx * e2y - dy * e2x;
        const float det = e1x * px + e1y * py + e1z * pz;
        if (std::fabs(det) > 1e-7f) {
            const float inv_det = 1.0f / det;
            const float tvx = ox - v0x, tvy = oy - v0y, tvz = oz - v0z;
            const float u = (tvx * px + tvy * py + tvz * pz) * inv_det;
            const float qx = tvy * e1z - tvz * e1y;
            const float qy = tvz * e1x - tvx * e1z;
            const float qz = tvx * e1y - tvy * e1x;
            const float v = (dx * qx + dy * qy + dz * qz) * inv_det;
            const float t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
            if (u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f && t >= 0.0f && t <= tmax) {
                return BvhHitResult{t, u, v, hit_id};
            }
        }
        return miss;
    }

    // AABB Box node (Kay-Kajiya Slab)
    const float bmin_x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 4u));
    const float bmin_y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 8u));
    const float bmin_z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 12u));
    const float bmax_x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 16u));
    const float bmax_y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 20u));
    const float bmax_z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 24u));

    float t_enter = 0.0f;
    float t_exit = tmax;
    auto slab = [&](float o, float d, float bmin, float bmax) {
        if (std::fabs(d) > 1e-12f) {
            const float inv_d = 1.0f / d;
            const float t0 = (bmin - o) * inv_d;
            const float t1 = (bmax - o) * inv_d;
            t_enter = std::fmax(t_enter, std::fmin(t0, t1));
            t_exit = std::fmin(t_exit, std::fmax(t0, t1));
        } else if (o < bmin || o > bmax) {
            t_enter = 1e30f;
            t_exit = -1e30f;
        }
    };
    slab(ox, dx, bmin_x, bmax_x);
    slab(oy, dy, bmin_y, bmax_y);
    slab(oz, dz, bmin_z, bmax_z);
    if (t_enter <= t_exit) {
        return BvhHitResult{t_enter, t_exit, 0.0f, hit_id};
    }
    return miss;
}

struct SimtFrame {
    size_t target_pc = 0;
    uint64_t exec_mask = 0;
    size_t reconv_pc = SIZE_MAX;
};

struct CpuWaveState {
    uint32_t lane_count = 32;
    uint32_t base_local_idx = 0;
    std::array<uint32_t, 106> sgpr{};
    std::vector<std::array<uint32_t, 256>> vgpr;
    std::vector<std::vector<uint32_t>> scratch;
    std::vector<uint32_t> global_thread_ids;

    uint64_t exec = 0;
    uint64_t vcc = 0;
    bool scc = false;
    uint32_t m0 = 0;
    bool m0_written = false;

    size_t pc = 0;
    size_t cur_reconv_pc = SIZE_MAX;
    std::vector<SimtFrame> simt_stack;
    bool halted = false;
    bool at_barrier = false;
    uint32_t steps = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Rdna2CpuTexture Helpers
// ---------------------------------------------------------------------------
void Rdna2CpuTexture::AllocateRgba32f(
    uint8_t dimension,
    uint32_t width,
    uint32_t height,
    uint32_t depth_or_faces,
    uint32_t mip_levels) {
    dim = dimension;
    const uint32_t levels = mip_levels ? mip_levels : 1u;
    mips.clear();
    mips.resize(levels);
    uint32_t w = width ? width : 1u;
    uint32_t h = height ? height : 1u;
    uint32_t d = (dimension == 3u) ? 6u : (depth_or_faces ? depth_or_faces : 1u);
    for (uint32_t m = 0; m < levels; ++m) {
        mips[m].width = w;
        mips[m].height = h;
        mips[m].depth = d;
        mips[m].rgba32f.assign(static_cast<size_t>(w) * h * d * 4u, 0.0f);
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        if (dimension == 2u) {
            d = std::max(1u, d >> 1);
        }
    }
}

void Rdna2CpuTexture::AllocateR32u(
    uint8_t dimension,
    uint32_t width,
    uint32_t height,
    uint32_t depth_or_faces,
    uint32_t mip_levels) {
    dim = dimension;
    const uint32_t levels = mip_levels ? mip_levels : 1u;
    mips.clear();
    mips.resize(levels);
    uint32_t w = width ? width : 1u;
    uint32_t h = height ? height : 1u;
    uint32_t d = (dimension == 3u) ? 6u : (depth_or_faces ? depth_or_faces : 1u);
    for (uint32_t m = 0; m < levels; ++m) {
        mips[m].width = w;
        mips[m].height = h;
        mips[m].depth = d;
        mips[m].r32u.assign(static_cast<size_t>(w) * h * d, 0u);
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        if (dimension == 2u) {
            d = std::max(1u, d >> 1);
        }
    }
}

std::array<float, 4> Rdna2CpuTexture::LoadRgba32f(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept {
    if (mip >= mips.size()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const auto& lv = mips[mip];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return {0.0f, 0.0f, 0.0f, 0.0f};
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx * 4u + 3u >= lv.rgba32f.size()) return {0.0f, 0.0f, 0.0f, 0.0f};
    return {lv.rgba32f[idx * 4u + 0u], lv.rgba32f[idx * 4u + 1u],
            lv.rgba32f[idx * 4u + 2u], lv.rgba32f[idx * 4u + 3u]};
}

std::array<uint32_t, 4> Rdna2CpuTexture::LoadR32uRgba(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept {
    // Null / unbound descriptor returns all zeros; for a valid bound R32_UINT texture,
    // D3D12 / AMD RDNA2 T# DST_SEL missing-component substitution fills (G=0, B=0, A=1)
    // even when present channel R is clamped to 0 on out-of-bounds coordinates or mip.
    if (mips.empty()) return {0u, 0u, 0u, 0u};
    if (mip >= mips.size()) return {0u, 0u, 0u, 1u};
    const auto& lv = mips[mip];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return {0u, 0u, 0u, 1u};
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx >= lv.r32u.size()) return {0u, 0u, 0u, 1u};
    return {lv.r32u[idx], 0u, 0u, 1u};
}

uint32_t Rdna2CpuTexture::LoadR32u(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept {
    return LoadR32uRgba(x, y, z, mip)[0];
}

std::array<float, 4> Rdna2CpuTexture::SampleNearest(float u, float v, float lod) const noexcept {
    if (mips.empty()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const uint32_t mip = static_cast<uint32_t>(
        std::clamp(static_cast<int32_t>(std::floor(lod + 0.5f)),
                   0, static_cast<int32_t>(mips.size()) - 1));
    const auto& lv = mips[mip];
    if (lv.width == 0u || lv.height == 0u) return {0.0f, 0.0f, 0.0f, 0.0f};
    const float cu = std::clamp(u, 0.0f, 1.0f);
    const float cv = std::clamp(v, 0.0f, 1.0f);
    const int32_t ix = std::clamp(static_cast<int32_t>(std::floor(cu * static_cast<float>(lv.width))),
                                  0, static_cast<int32_t>(lv.width) - 1);
    const int32_t iy = std::clamp(static_cast<int32_t>(std::floor(cv * static_cast<float>(lv.height))),
                                  0, static_cast<int32_t>(lv.height) - 1);
    return LoadRgba32f(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), 0u, mip);
}

void Rdna2CpuTexture::StoreRgba32f(
    uint32_t x, uint32_t y, uint32_t z, const std::array<float, 4>& rgba) noexcept {
    if (mips.empty()) return;
    auto& lv = mips[0];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return;
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx * 4u + 3u >= lv.rgba32f.size()) return;
    lv.rgba32f[idx * 4u + 0u] = rgba[0];
    lv.rgba32f[idx * 4u + 1u] = rgba[1];
    lv.rgba32f[idx * 4u + 2u] = rgba[2];
    lv.rgba32f[idx * 4u + 3u] = rgba[3];
}

uint32_t Rdna2CpuTexture::AtomicCmpSwapR32u(
    uint32_t x, uint32_t y, uint32_t z, uint32_t cmp_val, uint32_t new_val) noexcept {
    if (mips.empty()) return 0u;
    auto& lv = mips[0];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return 0u;
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx >= lv.r32u.size()) return 0u;
    const uint32_t old = lv.r32u[idx];
    if (old == cmp_val) {
        lv.r32u[idx] = new_val;
    }
    return old;
}

// ---------------------------------------------------------------------------
// CPU RDNA2 Wave32 / Wave64 Interpreter
// ---------------------------------------------------------------------------
Status ExecuteRdna2Cpu(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2CpuExecState& state) {
    if (instructions.empty()) {
        return Status::Error(StatusCode::ParseError, "Cannot execute empty RDNA2 instruction stream");
    }

    const uint32_t wave_size = (container.wave_size == 64u) ? 64u : 32u;
    const uint32_t tg_x = container.threadgroup_x ? container.threadgroup_x : 1u;
    const uint32_t tg_y = container.threadgroup_y ? container.threadgroup_y : 1u;
    const uint32_t tg_z = container.threadgroup_z ? container.threadgroup_z : 1u;
    const uint32_t tg_threads = tg_x * tg_y * tg_z;
    const uint32_t disp_x = state.dispatch_x ? state.dispatch_x : 1u;
    const uint32_t disp_y = state.dispatch_y ? state.dispatch_y : 1u;
    const uint32_t disp_z = state.dispatch_z ? state.dispatch_z : 1u;
    const uint32_t total_groups = disp_x * disp_y * disp_z;

    // Map word_offset -> instruction index and resolve branch targets
    std::unordered_map<uint32_t, size_t> word_to_idx;
    word_to_idx.reserve(instructions.size());
    for (size_t i = 0; i < instructions.size(); ++i) {
        word_to_idx[instructions[i].word_offset] = i;
    }

    std::unordered_map<size_t, size_t> branch_targets;
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (IsBranchOp(instructions[i].opcode)) {
            const int64_t tgt_word = static_cast<int64_t>(instructions[i].word_offset) + 1 +
                                     static_cast<int64_t>(instructions[i].simm16);
            if (tgt_word < 0) {
                return Status::Error(StatusCode::ParseError, "Interpreter branch target < 0");
            }
            auto it = word_to_idx.find(static_cast<uint32_t>(tgt_word));
            if (it == word_to_idx.end()) {
                return Status::Error(StatusCode::ParseError, "Interpreter branch target misaligned");
            }
            branch_targets[i] = it->second;
        }
    }
    const std::vector<size_t> ipdom = ComputeInstructionIpdom(instructions, branch_targets);

    const uint32_t lds_dwords = state.lds_size_dwords ? state.lds_size_dwords : 16384u;
    const uint32_t lds_mask = lds_dwords - 1u;
    const uint32_t scr_dwords = state.scratch_size_dwords ? state.scratch_size_dwords : 256u;
    const uint32_t scr_mask = scr_dwords - 1u;

    state.exports.assign(static_cast<size_t>(total_groups) * tg_threads, Rdna2CpuThreadExport{});
    state.total_instructions_executed = 0;
    state.barriers_synchronized = 0;
    state.waves_executed = 0;

    for (uint32_t gz = 0; gz < disp_z; ++gz) {
        for (uint32_t gy = 0; gy < disp_y; ++gy) {
            for (uint32_t gx = 0; gx < disp_x; ++gx) {
                const uint32_t group_linear = (gz * disp_y + gy) * disp_x + gx;
                std::vector<uint32_t> lds(lds_dwords, 0u);

                const uint32_t num_waves = (tg_threads + wave_size - 1u) / wave_size;
                std::vector<CpuWaveState> waves(num_waves);
                for (uint32_t w = 0; w < num_waves; ++w) {
                    auto& wv = waves[w];
                    wv.base_local_idx = w * wave_size;
                    wv.lane_count = std::min(wave_size, tg_threads - wv.base_local_idx);
                    wv.exec = (wv.lane_count == 64u)
                                  ? ~0ull
                                  : ((1ull << wv.lane_count) - 1ull);
                    wv.vgpr.assign(wv.lane_count, std::array<uint32_t, 256>{});
                    wv.scratch.assign(wv.lane_count, std::vector<uint32_t>(scr_dwords, 0u));
                    wv.global_thread_ids.resize(wv.lane_count, 0u);

                    for (size_t rc = 0; rc < std::min<size_t>(16u, state.root_constants.size()); ++rc) {
                        wv.sgpr[rc] = state.root_constants[rc];
                    }
                    // Seed full GFX10.3 merged_wave_info in s3 for Mesh/Geometry shaders (when not overridden by root_constants)
                    if ((container.stage == ShaderStage::Mesh || container.stage == ShaderStage::Geometry) &&
                        state.root_constants.size() <= 3u) {
                        Rdna2MergedWaveInfo mwi{};
                        mwi.es_thread_count = static_cast<uint8_t>(std::min(255u, wv.lane_count));
                        mwi.gs_thread_count = static_cast<uint8_t>(std::min(255u, tg_threads));
                        mwi.gs_wave_id = static_cast<uint8_t>(w & 0xFFu);
                        mwi.wave_in_group_idx = static_cast<uint8_t>(w & 0xFu);
                        mwi.waves_in_group = static_cast<uint8_t>(std::min(15u, num_waves));
                        wv.sgpr[3] = PackMergedWaveInfo(mwi);
                    }

                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                        const uint32_t local_idx = wv.base_local_idx + l;
                        const uint32_t lx = local_idx % tg_x;
                        const uint32_t global_linear = group_linear * tg_threads + local_idx;
                        wv.global_thread_ids[l] = global_linear;

                        if (container.stage == ShaderStage::Pixel) {
                            if (global_linear < state.stage_inputs.size()) {
                                const auto& in_p = state.stage_inputs[global_linear];
                                wv.vgpr[l][0] = FloatToBits(in_p[0]);
                                wv.vgpr[l][1] = FloatToBits(in_p[1]);
                                wv.vgpr[l][2] = FloatToBits(in_p[2]);
                                wv.vgpr[l][3] = FloatToBits(in_p[3]);
                            }
                        } else if (container.stage == ShaderStage::Mesh ||
                                   container.stage == ShaderStage::Amplification) {
                            wv.vgpr[l][0] = local_idx;
                            // Register mesh NumThreads (tg_threads) before v5 workgroup seeding: group * NumThreads + lane
                            if (container.stage == ShaderStage::Mesh) {
                                wv.vgpr[l][5] = group_linear * tg_threads + local_idx;
                            }
                        } else {
                            // 1D / Compute / Vertex: SV_DispatchThreadID.x = gx * tg_x + lx
                            wv.vgpr[l][0] = gx * tg_x + lx;
                        }
                    }
                    ++state.waves_executed;
                }

                // Step all waves in the workgroup in barrier-synchronized lockstep
                for (;;) {
                    bool any_running = false;
                    for (auto& wv : waves) {
                        while (!wv.halted && !wv.at_barrier) {
                            any_running = true;

                            // Reconverge SIMT stack if we reached cur_reconv_pc
                            while (wv.pc == wv.cur_reconv_pc && !wv.simt_stack.empty()) {
                                const SimtFrame fr = wv.simt_stack.back();
                                wv.simt_stack.pop_back();
                                wv.pc = fr.target_pc;
                                wv.exec = fr.exec_mask;
                                wv.cur_reconv_pc = fr.reconv_pc;
                            }

                            if (wv.pc >= instructions.size()) {
                                if (!wv.simt_stack.empty()) {
                                    const SimtFrame fr = wv.simt_stack.back();
                                    wv.simt_stack.pop_back();
                                    wv.pc = fr.target_pc;
                                    wv.exec = fr.exec_mask;
                                    wv.cur_reconv_pc = fr.reconv_pc;
                                    continue;
                                }
                                wv.halted = true;
                                break;
                            }

                            if (++wv.steps > state.max_steps_per_wave) {
                                return Status::Error(StatusCode::ShaderError,
                                                     "RDNA2 CPU interpreter exceeded max_steps_per_wave");
                            }
                            ++state.total_instructions_executed;

                            const auto& inst = instructions[wv.pc];
                            const size_t cur_pc = wv.pc;
                            size_t next_pc = cur_pc + 1;

                            auto read_op_u32 = [&](const Rdna2Operand& op, uint32_t lane) -> uint32_t {
                                switch (op.kind) {
                                    case Rdna2OperandKind::Sgpr:
                                        return (op.reg_index < 106u) ? wv.sgpr[op.reg_index] : 0u;
                                    case Rdna2OperandKind::Vgpr:
                                        return (op.reg_index < 256u) ? wv.vgpr[lane][op.reg_index] : 0u;
                                    case Rdna2OperandKind::InlineInt:
                                        return static_cast<uint32_t>(op.int_value);
                                    case Rdna2OperandKind::InlineFloat:
                                    case Rdna2OperandKind::Literal32:
                                        return op.literal_bits;
                                    case Rdna2OperandKind::VccLo:
                                        return static_cast<uint32_t>(wv.vcc & 0xFFFFFFFFull);
                                    case Rdna2OperandKind::VccHi:
                                        return static_cast<uint32_t>(wv.vcc >> 32);
                                    case Rdna2OperandKind::ExecLo:
                                        return static_cast<uint32_t>(wv.exec & 0xFFFFFFFFull);
                                    case Rdna2OperandKind::ExecHi:
                                        return static_cast<uint32_t>(wv.exec >> 32);
                                    case Rdna2OperandKind::Scc:
                                        return wv.scc ? 1u : 0u;
                                    case Rdna2OperandKind::M0:
                                        return wv.m0;
                                    default:
                                        return 0u;
                                }
                            };

                            auto read_op_f32 = [&](const Rdna2Operand& op, uint32_t lane) -> float {
                                float v = BitsToFloat(read_op_u32(op, lane));
                                if (op.abs) v = std::fabs(v);
                                if (op.neg) v = -v;
                                return v;
                            };

                            auto clamp_f32 = [&](float v) -> float {
                                const float q = QuietNanF32(v);
                                if (!inst.clamp) return q;
                                if (IsNanF32Bits(FloatToBits(q))) return 0.0f;
                                return std::fmin(std::fmax(q, 0.0f), 1.0f);
                            };

                            auto write_sgpr = [&](const Rdna2Operand& dst, uint32_t val) {
                                if (dst.kind == Rdna2OperandKind::Sgpr && dst.reg_index < 106u) {
                                    wv.sgpr[dst.reg_index] = val;
                                } else if (dst.kind == Rdna2OperandKind::VccLo) {
                                    wv.vcc = (wv.vcc & 0xFFFFFFFF00000000ull) | val;
                                } else if (dst.kind == Rdna2OperandKind::VccHi) {
                                    wv.vcc = (wv.vcc & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32);
                                } else if (dst.kind == Rdna2OperandKind::ExecLo) {
                                    wv.exec = (wv.exec & 0xFFFFFFFF00000000ull) | val;
                                } else if (dst.kind == Rdna2OperandKind::ExecHi) {
                                    wv.exec = (wv.exec & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32);
                                } else if (dst.kind == Rdna2OperandKind::M0) {
                                    wv.m0 = val;
                                    wv.m0_written = true;
                                }
                            };

                            auto write_mask64 = [&](const Rdna2Operand& dst, uint64_t mask) {
                                if (dst.kind == Rdna2OperandKind::VccLo) {
                                    wv.vcc = mask;
                                } else if (dst.kind == Rdna2OperandKind::Sgpr && dst.reg_index < 106u) {
                                    wv.sgpr[dst.reg_index] = static_cast<uint32_t>(mask & 0xFFFFFFFFull);
                                    if (wave_size == 64u && dst.reg_index + 1u < 106u) {
                                        wv.sgpr[dst.reg_index + 1u] = static_cast<uint32_t>(mask >> 32);
                                    }
                                }
                            };

                            auto read_mask64 = [&](const Rdna2Operand& op) -> uint64_t {
                                if (op.kind == Rdna2OperandKind::VccLo || op.kind == Rdna2OperandKind::VccHi) {
                                    return wv.vcc;
                                }
                                if (op.kind == Rdna2OperandKind::ExecLo || op.kind == Rdna2OperandKind::ExecHi) {
                                    return wv.exec;
                                }
                                if (op.kind == Rdna2OperandKind::Sgpr && op.reg_index < 106u) {
                                    uint64_t lo = wv.sgpr[op.reg_index];
                                    uint64_t hi = (op.reg_index + 1u < 106u) ? wv.sgpr[op.reg_index + 1u] : 0u;
                                    return lo | (hi << 32);
                                }
                                if (op.kind == Rdna2OperandKind::InlineInt) {
                                    return static_cast<uint64_t>(static_cast<int64_t>(op.int_value));
                                }
                                return read_op_u32(op, 0u);
                            };

                            auto lane_active = [&](uint32_t l) -> bool {
                                return ((wv.exec >> l) & 1ull) != 0ull;
                            };

                            auto do_divergent_branch = [&](uint64_t taken_mask, size_t tgt_pc) {
                                const uint64_t fall_mask = wv.exec & ~taken_mask;
                                if (taken_mask == 0ull) {
                                    next_pc = cur_pc + 1;
                                } else if (fall_mask == 0ull) {
                                    next_pc = tgt_pc;
                                } else {
                                    const size_t reconv = ipdom[cur_pc];
                                    wv.simt_stack.push_back({reconv, wv.exec, wv.cur_reconv_pc});
                                    if (tgt_pc != reconv) {
                                        wv.simt_stack.push_back({tgt_pc, taken_mask, reconv});
                                    }
                                    wv.exec = fall_mask;
                                    wv.cur_reconv_pc = reconv;
                                    next_pc = cur_pc + 1;
                                }
                            };

                            switch (inst.opcode) {
                                // ----- SOP2 -----
                                case Rdna2Opcode::S_ADD_U32: {
                                    const uint64_t sum = static_cast<uint64_t>(read_op_u32(inst.src[0], 0)) +
                                                         static_cast<uint64_t>(read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, static_cast<uint32_t>(sum));
                                    wv.scc = (sum >> 32) != 0ull;
                                    break;
                                }
                                case Rdna2Opcode::S_SUB_U32: {
                                    const uint32_t a = read_op_u32(inst.src[0], 0);
                                    const uint32_t b = read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, a - b);
                                    wv.scc = (b > a);
                                    break;
                                }
                                case Rdna2Opcode::S_ADD_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    const int64_t r = static_cast<int64_t>(a) + static_cast<int64_t>(b);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(static_cast<int32_t>(r)));
                                    wv.scc = (r != static_cast<int64_t>(static_cast<int32_t>(r)));
                                    break;
                                }
                                case Rdna2Opcode::S_SUB_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    const int64_t r = static_cast<int64_t>(a) - static_cast<int64_t>(b);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(static_cast<int32_t>(r)));
                                    wv.scc = (r != static_cast<int64_t>(static_cast<int32_t>(r)));
                                    break;
                                }
                                case Rdna2Opcode::S_ADDC_U32: {
                                    const uint64_t sum = static_cast<uint64_t>(read_op_u32(inst.src[0], 0)) +
                                                         static_cast<uint64_t>(read_op_u32(inst.src[1], 0)) +
                                                         (wv.scc ? 1ull : 0ull);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(sum));
                                    wv.scc = (sum >> 32) != 0ull;
                                    break;
                                }
                                case Rdna2Opcode::S_SUBB_U32: {
                                    const uint64_t a = static_cast<uint64_t>(read_op_u32(inst.src[0], 0));
                                    const uint64_t sub = static_cast<uint64_t>(read_op_u32(inst.src[1], 0)) +
                                                         (wv.scc ? 1ull : 0ull);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(a - sub));
                                    wv.scc = (a < sub);
                                    break;
                                }
                                case Rdna2Opcode::S_ABSDIFF_I32: {
                                    const int64_t a = static_cast<int64_t>(static_cast<int32_t>(read_op_u32(inst.src[0], 0)));
                                    const int64_t b = static_cast<int64_t>(static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    const int64_t diff = (a >= b) ? (a - b) : (b - a);
                                    const uint32_t res = static_cast<uint32_t>(diff);
                                    write_sgpr(inst.dst, res);
                                    wv.scc = (res != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_MIN_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, static_cast<uint32_t>(std::min(a, b)));
                                    wv.scc = (a < b);
                                    break;
                                }
                                case Rdna2Opcode::S_MIN_U32: {
                                    const uint32_t a = read_op_u32(inst.src[0], 0);
                                    const uint32_t b = read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, std::min(a, b));
                                    wv.scc = (a < b);
                                    break;
                                }
                                case Rdna2Opcode::S_MAX_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, static_cast<uint32_t>(std::max(a, b)));
                                    wv.scc = (a > b);
                                    break;
                                }
                                case Rdna2Opcode::S_MAX_U32: {
                                    const uint32_t a = read_op_u32(inst.src[0], 0);
                                    const uint32_t b = read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, std::max(a, b));
                                    wv.scc = (a > b);
                                    break;
                                }
                                case Rdna2Opcode::S_CSELECT_B32:
                                    write_sgpr(inst.dst, wv.scc ? read_op_u32(inst.src[0], 0) : read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_AND_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) & read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_OR_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) | read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_XOR_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) ^ read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_ANDN2_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) & ~read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_LSHL_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) << (read_op_u32(inst.src[1], 0) & 31u);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_LSHR_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) >> (read_op_u32(inst.src[1], 0) & 31u);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_ASHR_I32: {
                                    const int32_t r = static_cast<int32_t>(read_op_u32(inst.src[0], 0)) >>
                                                      (read_op_u32(inst.src[1], 0) & 31u);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(r));
                                    wv.scc = (r != 0);
                                    break;
                                }
                                case Rdna2Opcode::S_BFE_U32: {
                                    const uint32_t r = EvalSBfeU32(read_op_u32(inst.src[0], 0), read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_BFE_I32: {
                                    const uint32_t r = EvalSBfeI32(read_op_u32(inst.src[0], 0), read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_MUL_I32:
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], 0) * read_op_u32(inst.src[1], 0));
                                    break;

                                // ----- SOP1 -----
                                case Rdna2Opcode::S_MOV_B32:
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], 0));
                                    break;
                                case Rdna2Opcode::S_NOT_B32: {
                                    const uint32_t r = ~read_op_u32(inst.src[0], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_BREV_B32: {
                                    const uint32_t r = BitReverse32Cpu(read_op_u32(inst.src[0], 0));
                                    write_sgpr(inst.dst, r);
                                    break;
                                }
                                case Rdna2Opcode::S_BCNT1_I32_B32: {
                                    const uint32_t cnt = PopCount32(read_op_u32(inst.src[0], 0));
                                    write_sgpr(inst.dst, cnt);
                                    wv.scc = (cnt != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_BCNT1_I32_B64: {
                                    const uint32_t cnt = PopCount64(read_mask64(inst.src[0]));
                                    write_sgpr(inst.dst, cnt);
                                    wv.scc = (cnt != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_SETPC_B64:
                                case Rdna2Opcode::S_SWAPPC_B64: {
                                    uint64_t tgt_bytes = 0ull;
                                    if (inst.src[0].kind == Rdna2OperandKind::Sgpr && inst.src[0].reg_index < 106u) {
                                        const uint32_t lo = wv.sgpr[inst.src[0].reg_index];
                                        const uint32_t hi = (inst.src[0].reg_index + 1u < 106u)
                                                                ? wv.sgpr[inst.src[0].reg_index + 1u]
                                                                : 0u;
                                        tgt_bytes = (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
                                    }
                                    if (inst.opcode == Rdna2Opcode::S_SWAPPC_B64) {
                                        write_sgpr(inst.dst, (inst.word_offset + 1u) * 4u);
                                        if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index + 1u < 106u) {
                                            wv.sgpr[inst.dst.reg_index + 1u] = 0u;
                                        }
                                    }
                                    if (tgt_bytes >= 4ull && (tgt_bytes & 3ull) == 0ull && (tgt_bytes >> 32) == 0ull) {
                                        auto it_pc = word_to_idx.find(static_cast<uint32_t>(tgt_bytes / 4ull));
                                        if (it_pc != word_to_idx.end()) {
                                            next_pc = it_pc->second;
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::S_GETPC_B64: {
                                    write_sgpr(inst.dst, (inst.word_offset + 1u) * 4u);
                                    if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index + 1u < 106u) {
                                        wv.sgpr[inst.dst.reg_index + 1u] = 0u;
                                    }
                                    break;
                                }

                                // ----- SOPC -----
                                case Rdna2Opcode::S_CMP_EQ_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) == static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_LG_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) != static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_GT_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) > static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_GE_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) >= static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_LT_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) < static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_LE_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) <= static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_EQ_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) == read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_LG_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) != read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_GT_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) > read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_GE_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) >= read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_LT_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) < read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_LE_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) <= read_op_u32(inst.src[1], 0));
                                    break;

                                // ----- SOPK -----
                                case Rdna2Opcode::S_MOVK_I32:
                                    write_sgpr(inst.dst, static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                                    break;
                                case Rdna2Opcode::S_VERSION:
                                case Rdna2Opcode::S_WAITCNT_VSCNT:
                                case Rdna2Opcode::S_WAITCNT_VMCNT:
                                case Rdna2Opcode::S_WAITCNT_EXPCNT:
                                case Rdna2Opcode::S_WAITCNT_LGKMCNT:
                                    break;
                                case Rdna2Opcode::S_ADDK_I32: {
                                    const uint32_t cur = read_op_u32(inst.dst, 0);
                                    write_sgpr(inst.dst, cur + static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                                    break;
                                }
                                case Rdna2Opcode::S_MULK_I32: {
                                    const uint32_t cur = read_op_u32(inst.dst, 0);
                                    write_sgpr(inst.dst, cur * static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
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
                                    wv.pc = next_pc;
                                    wv.at_barrier = true;
                                    continue;
                                case Rdna2Opcode::S_SENDMSG: {
                                    const uint16_t msg_id = static_cast<uint16_t>(static_cast<uint16_t>(inst.simm16) & 0xFu);
                                    if (container.stage == ShaderStage::Mesh || msg_id == kRdna2SendMsgGsAllocReq) {
                                        if (wv.m0_written) {
                                            // ISA-exact GFX10.3 GS_ALLOC_REQ 11-bit fields: M0[10:0], M0[22:12]
                                            state.mesh_vertex_count = wv.m0 & kGsAllocReqVertMask;
                                            state.mesh_primitive_count = (wv.m0 >> kGsAllocReqPrimShift) & kGsAllocReqPrimMask;
                                        } else {
                                            state.mesh_vertex_count = wv.sgpr[0];
                                            state.mesh_primitive_count = wv.sgpr[1];
                                        }
                                    } else if (container.stage == ShaderStage::Amplification || msg_id == kRdna2SendMsgDispatchMesh) {
                                        state.dispatch_mesh_groups = {wv.sgpr[0], wv.sgpr[1], wv.sgpr[2]};
                                    }
                                    break;
                                }
                                case Rdna2Opcode::S_ENDPGM: {
                                    if (!wv.simt_stack.empty()) {
                                        // Mask out finished lanes from pending reconvergence frames
                                        for (auto& fr : wv.simt_stack) {
                                            fr.exec_mask &= ~wv.exec;
                                        }
                                        const SimtFrame fr = wv.simt_stack.back();
                                        wv.simt_stack.pop_back();
                                        wv.pc = fr.target_pc;
                                        wv.exec = fr.exec_mask;
                                        wv.cur_reconv_pc = fr.reconv_pc;
                                        continue;
                                    }
                                    wv.halted = true;
                                    continue;
                                }
                                case Rdna2Opcode::S_BRANCH:
                                    next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_SCC0:
                                    if (!wv.scc) next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_SCC1:
                                    if (wv.scc) next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_VCCZ:
                                    do_divergent_branch(wv.exec & ~wv.vcc, branch_targets[cur_pc]);
                                    break;
                                case Rdna2Opcode::S_CBRANCH_VCCNZ:
                                    do_divergent_branch(wv.exec & wv.vcc, branch_targets[cur_pc]);
                                    break;
                                case Rdna2Opcode::S_CBRANCH_EXECZ:
                                    if (wv.exec == 0ull) next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_EXECNZ:
                                    if (wv.exec != 0ull) next_pc = branch_targets[cur_pc];
                                    break;

                                // ----- VOP2 / VOP1 / VOP3 per-lane vector ops -----
                                case Rdna2Opcode::V_NOP:
                                    break;
                                case Rdna2Opcode::V_MOV_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_CNDMASK_B32: {
                                    const uint64_t mask = (inst.src_count >= 3) ? read_mask64(inst.src[2]) : wv.vcc;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const bool sel1 = ((mask >> l) & 1ull) != 0ull;
                                        wv.vgpr[l][inst.dst.reg_index] = sel1 ? read_op_u32(inst.src[1], l)
                                                                              : read_op_u32(inst.src[0], l);
                                    }
                                    break;
                                }
                                case Rdna2Opcode::V_ADD_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[0], l) + read_op_f32(inst.src[1], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_SUB_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[0], l) - read_op_f32(inst.src[1], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_SUBREV_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[1], l) - read_op_f32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_MUL_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[0], l) * read_op_f32(inst.src[1], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_MIN_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(ExactMinF32(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAX_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(ExactMaxF32(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_MIN_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], l));
                                        const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], l));
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(std::min(a, b));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAX_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], l));
                                        const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], l));
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(std::max(a, b));
                                    }
                                    break;
                                case Rdna2Opcode::V_MIN_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            std::min(read_op_u32(inst.src[0], l), read_op_u32(inst.src[1], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAX_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            std::max(read_op_u32(inst.src[0], l), read_op_u32(inst.src[1], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAC_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float acc = BitsToFloat(wv.vgpr[l][inst.dst.reg_index]);
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(EvalMadF32(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), acc)));
                                    }
                                    break;
                                case Rdna2Opcode::V_MADMK_F32:
                                case Rdna2Opcode::V_MADAK_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(
                                            EvalMadF32(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), read_op_f32(inst.src[2], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FMAC_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float acc = BitsToFloat(wv.vgpr[l][inst.dst.reg_index]);
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(std::fmaf(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), acc)));
                                    }
                                    break;
                                case Rdna2Opcode::V_FMAC_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float a = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[0], l), false));
                                        const float b = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[1], l), false));
                                        const float acc = HalfBitsToFloat(PickHalf16(wv.vgpr[l][inst.dst.reg_index], false));
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(FloatToHalfBits(clamp_f32(std::fmaf(a, b, acc))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FMAMK_F16:
                                case Rdna2Opcode::V_FMAAK_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float a = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[0], l), false));
                                        const float b = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[1], l), false));
                                        const float c = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[2], l), false));
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(FloatToHalfBits(clamp_f32(std::fmaf(a, b, c))));
                                    }
                                    break;
                                case Rdna2Opcode::V_AND_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) & read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_OR_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) | read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_XOR_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) ^ read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_LSHLREV_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            read_op_u32(inst.src[1], l) << (read_op_u32(inst.src[0], l) & 31u);
                                    }
                                    break;
                                case Rdna2Opcode::V_LSHRREV_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            read_op_u32(inst.src[1], l) >> (read_op_u32(inst.src[0], l) & 31u);
                                    }
                                    break;
                                case Rdna2Opcode::V_ASHRREV_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const int32_t val = static_cast<int32_t>(read_op_u32(inst.src[1], l));
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(val >> (read_op_u32(inst.src[0], l) & 31u));
                                    }
                                    break;
                                case Rdna2Opcode::V_ADD_NC_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) + read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_SUB_NC_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) - read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_READFIRSTLANE_B32: {
                                    uint32_t first = 0u;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (lane_active(l)) {
                                            first = l;
                                            break;
                                        }
                                    }
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], first));
                                    break;
                                }
                                case Rdna2Opcode::V_CVT_F32_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            FloatToBits(static_cast<float>(static_cast<int32_t>(read_op_u32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_I32_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(static_cast<int32_t>(read_op_f32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_F32_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            FloatToBits(static_cast<float>(read_op_u32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_U32_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(read_op_f32(inst.src[0], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_RCP_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(ExactRcpF32(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_RSQ_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(ExactRsqF32(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_SQRT_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(ExactSqrtF32(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FLOOR_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(ExactFloorF32(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_CEIL_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(ExactCeilF32(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FRACT_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float x = read_op_f32(inst.src[0], l);
                                        wv.vgpr[l][inst.dst.reg_index] = std::isfinite(x)
                                            ? FloatToBits(clamp_f32(x - ExactFloorF32(x)))
                                            : (inst.clamp ? 0u : 0x7FC00000u);
                                    }
                                    break;
                                case Rdna2Opcode::V_SWAP_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t old_dst = wv.vgpr[l][inst.dst.reg_index];
                                        const uint32_t old_src = read_op_u32(inst.src[0], l);
                                        wv.vgpr[l][inst.dst.reg_index] = old_src;
                                        if (inst.src[0].kind == Rdna2OperandKind::Vgpr) {
                                            wv.vgpr[l][inst.src[0].reg_index] = old_dst;
                                        }
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_NORM_I16_F16:
                                case Rdna2Opcode::V_CVT_NORM_U16_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint16_t h = PickHalf16(read_op_u32(inst.src[0], l), (inst.vop3_op_sel & 1u) != 0u);
                                        float f = HalfBitsToFloat(h);
                                        if (inst.src[0].abs) f = std::fabs(f);
                                        if (inst.src[0].neg) f = -f;
                                        const uint16_t out16 = (inst.opcode == Rdna2Opcode::V_CVT_NORM_I16_F16)
                                                                   ? CvtSnorm16F32(f)
                                                                   : CvtUnorm16F32(f);
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(out16);
                                    }
                                    break;

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
                                    uint64_t mask = 0ull;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        bool r = false;
                                        if (IsF16CompareOpcode(inst.opcode)) {
                                            float a = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[0], l), (inst.vop3_op_sel & 1u) != 0u));
                                            float b = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[1], l), ((inst.vop3_op_sel >> 1) & 1u) != 0u));
                                            if (inst.src[0].abs) a = std::fabs(a);
                                            if (inst.src[1].abs) b = std::fabs(b);
                                            if (inst.src[0].neg) a = -a;
                                            if (inst.src[1].neg) b = -b;
                                            r = EvalF16CompareCpu(inst.opcode, a, b);
                                        } else {
                                            switch (inst.opcode) {
                                                case Rdna2Opcode::V_CMP_F_F32:
                                                case Rdna2Opcode::V_CMP_LT_F32:
                                                case Rdna2Opcode::V_CMPX_LT_F32:
                                                case Rdna2Opcode::V_CMP_EQ_F32:
                                                case Rdna2Opcode::V_CMPX_EQ_F32:
                                                case Rdna2Opcode::V_CMP_LE_F32:
                                                case Rdna2Opcode::V_CMPX_LE_F32:
                                                case Rdna2Opcode::V_CMP_GT_F32:
                                                case Rdna2Opcode::V_CMPX_GT_F32:
                                                case Rdna2Opcode::V_CMP_LG_F32:
                                                case Rdna2Opcode::V_CMPX_LG_F32:
                                                case Rdna2Opcode::V_CMP_GE_F32:
                                                case Rdna2Opcode::V_CMPX_GE_F32:
                                                case Rdna2Opcode::V_CMP_O_F32:
                                                case Rdna2Opcode::V_CMP_U_F32:
                                                case Rdna2Opcode::V_CMP_NGE_F32:
                                                case Rdna2Opcode::V_CMP_NLG_F32:
                                                case Rdna2Opcode::V_CMP_NGT_F32:
                                                case Rdna2Opcode::V_CMP_NLE_F32:
                                                case Rdna2Opcode::V_CMP_NEQ_F32:
                                                case Rdna2Opcode::V_CMP_NLT_F32:
                                                case Rdna2Opcode::V_CMP_TRU_F32:
                                                    r = EvalF32CompareCpu(inst.opcode, read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l));
                                                    break;
                                                case Rdna2Opcode::V_CMP_LT_I32:
                                                case Rdna2Opcode::V_CMPX_LT_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) < static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                                case Rdna2Opcode::V_CMP_EQ_I32:
                                                case Rdna2Opcode::V_CMPX_EQ_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) == static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                                case Rdna2Opcode::V_CMP_GT_I32:
                                                case Rdna2Opcode::V_CMPX_GT_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) > static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                                case Rdna2Opcode::V_CMP_GE_I32:
                                                case Rdna2Opcode::V_CMPX_GE_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) >= static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                                case Rdna2Opcode::V_CMP_LT_U32:
                                                case Rdna2Opcode::V_CMPX_LT_U32: r = read_op_u32(inst.src[0], l) < read_op_u32(inst.src[1], l); break;
                                                case Rdna2Opcode::V_CMP_EQ_U32:
                                                case Rdna2Opcode::V_CMPX_EQ_U32: r = read_op_u32(inst.src[0], l) == read_op_u32(inst.src[1], l); break;
                                                case Rdna2Opcode::V_CMP_GT_U32:
                                                case Rdna2Opcode::V_CMPX_GT_U32: r = read_op_u32(inst.src[0], l) > read_op_u32(inst.src[1], l); break;
                                                case Rdna2Opcode::V_CMP_GE_U32:
                                                case Rdna2Opcode::V_CMPX_GE_U32: r = read_op_u32(inst.src[0], l) >= read_op_u32(inst.src[1], l); break;
                                                default: break;
                                            }
                                        }
                                        if (r) mask |= (1ull << l);
                                    }
                                    if (IsVcmpxOpcode(inst.opcode)) {
                                        // GFX10 V_CMPX writes ONLY EXEC (EXEC = EXEC & cmp_mask), never clobbers VCC
                                        wv.exec &= mask;
                                    } else {
                                        write_mask64(inst.dst, mask);
                                    }
                                    break;
                                }

                                // ----- VOP3 (3-op / wave / bitfield / 16-bit) -----
                                case Rdna2Opcode::V_FMA_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(
                                            std::fmaf(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), read_op_f32(inst.src[2], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAD_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(
                                            EvalMadF32(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), read_op_f32(inst.src[2], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FMA_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        auto read_f16 = [&](uint8_t idx) -> float {
                                            const bool hi = ((inst.vop3_op_sel >> idx) & 1u) != 0u;
                                            float v = HalfBitsToFloat(PickHalf16(read_op_u32(inst.src[idx], l), hi));
                                            if (inst.src[idx].abs) v = std::fabs(v);
                                            if (inst.src[idx].neg) v = -v;
                                            return v;
                                        };
                                        const float r = clamp_f32(std::fmaf(read_f16(0u), read_f16(1u), read_f16(2u)));
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(FloatToHalfBits(r));
                                    }
                                    break;
                                case Rdna2Opcode::V_ADD_NC_U16:
                                case Rdna2Opcode::V_SUB_NC_U16:
                                case Rdna2Opcode::V_MIN_U16:
                                case Rdna2Opcode::V_MAX_U16:
                                case Rdna2Opcode::V_MIN_I16:
                                case Rdna2Opcode::V_MAX_I16:
                                case Rdna2Opcode::V_MUL_LO_U16:
                                case Rdna2Opcode::V_LSHLREV_B16:
                                case Rdna2Opcode::V_LSHRREV_B16:
                                case Rdna2Opcode::V_MAD_U16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint16_t a16 = PickHalf16(read_op_u32(inst.src[0], l), (inst.vop3_op_sel & 1u) != 0u);
                                        const uint16_t b16 = PickHalf16(read_op_u32(inst.src[1], l), ((inst.vop3_op_sel >> 1) & 1u) != 0u);
                                        uint16_t r16 = 0u;
                                        if (inst.opcode == Rdna2Opcode::V_ADD_NC_U16) {
                                            r16 = static_cast<uint16_t>(a16 + b16);
                                        } else if (inst.opcode == Rdna2Opcode::V_SUB_NC_U16) {
                                            r16 = static_cast<uint16_t>(a16 - b16);
                                        } else if (inst.opcode == Rdna2Opcode::V_MIN_U16) {
                                            r16 = std::min(a16, b16);
                                        } else if (inst.opcode == Rdna2Opcode::V_MAX_U16) {
                                            r16 = std::max(a16, b16);
                                        } else if (inst.opcode == Rdna2Opcode::V_MIN_I16) {
                                            r16 = static_cast<uint16_t>(std::min(static_cast<int16_t>(a16), static_cast<int16_t>(b16)));
                                        } else if (inst.opcode == Rdna2Opcode::V_MAX_I16) {
                                            r16 = static_cast<uint16_t>(std::max(static_cast<int16_t>(a16), static_cast<int16_t>(b16)));
                                        } else if (inst.opcode == Rdna2Opcode::V_MUL_LO_U16) {
                                            r16 = static_cast<uint16_t>(a16 * b16);
                                        } else if (inst.opcode == Rdna2Opcode::V_LSHLREV_B16) {
                                            r16 = static_cast<uint16_t>(b16 << (a16 & 15u));
                                        } else if (inst.opcode == Rdna2Opcode::V_LSHRREV_B16) {
                                            r16 = static_cast<uint16_t>(b16 >> (a16 & 15u));
                                        } else {
                                            const uint16_t c16 = PickHalf16(read_op_u32(inst.src[2], l), ((inst.vop3_op_sel >> 2) & 1u) != 0u);
                                            r16 = static_cast<uint16_t>(static_cast<uint32_t>(a16) * static_cast<uint32_t>(b16) + c16);
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(r16);
                                    }
                                    break;
                                case Rdna2Opcode::V_BFE_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t s0 = read_op_u32(inst.src[0], l);
                                        const uint32_t off = read_op_u32(inst.src[1], l) & 31u;
                                        const uint32_t wid = read_op_u32(inst.src[2], l) & 31u;
                                        const uint32_t mask = (wid == 0u) ? 0u : ((1u << wid) - 1u);
                                        wv.vgpr[l][inst.dst.reg_index] = (s0 >> off) & mask;
                                    }
                                    break;
                                case Rdna2Opcode::V_BFE_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t s0 = read_op_u32(inst.src[0], l);
                                        const uint32_t off = read_op_u32(inst.src[1], l) & 31u;
                                        const uint32_t wid = read_op_u32(inst.src[2], l) & 31u;
                                        int32_t r = 0;
                                        if (wid == 0u) {
                                            r = 0;
                                        } else if (off + wid < 32u) {
                                            r = static_cast<int32_t>(s0 << (32u - off - wid)) >> (32u - wid);
                                        } else {
                                            r = static_cast<int32_t>(s0) >> off;
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(r);
                                    }
                                    break;
                                case Rdna2Opcode::V_BFI_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t m = read_op_u32(inst.src[0], l);
                                        const uint32_t ins = read_op_u32(inst.src[1], l);
                                        const uint32_t base = read_op_u32(inst.src[2], l);
                                        wv.vgpr[l][inst.dst.reg_index] = (m & ins) | (~m & base);
                                    }
                                    break;
                                case Rdna2Opcode::V_ALIGNBYTE_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = EvalAlignByteB32(
                                            read_op_u32(inst.src[0], l),
                                            read_op_u32(inst.src[1], l),
                                            read_op_u32(inst.src[2], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_ALIGNBIT_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = EvalAlignBitB32(
                                            read_op_u32(inst.src[0], l),
                                            read_op_u32(inst.src[1], l),
                                            read_op_u32(inst.src[2], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_DIV_FMAS_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float f0 = read_op_f32(inst.src[0], l);
                                        const float f1 = read_op_f32(inst.src[1], l);
                                        const float f2 = read_op_f32(inst.src[2], l);
                                        float r = std::fmaf(f0, f1, f2);
                                        if (((wv.vcc >> l) & 1ull) != 0ull) {
                                            const uint32_t exp_c = (FloatToBits(f2) >> 23) & 0xFFu;
                                            const float sc = (exp_c > 127u) ? BitsToFloat(0x5F800000u) : BitsToFloat(0x1F800000u);
                                            r *= sc;
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(r));
                                    }
                                    break;
                                case Rdna2Opcode::V_FMA_MIX_F32:
                                case Rdna2Opcode::V_FMA_MIXLO_F16:
                                case Rdna2Opcode::V_FMA_MIXHI_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        auto read_mix = [&](uint8_t idx) -> float {
                                            const uint32_t u = read_op_u32(inst.src[idx], l);
                                            const bool is_f16 = ((inst.vop3p_op_sel_hi >> idx) & 1u) != 0u;
                                            const bool hi_half = ((inst.vop3p_op_sel >> idx) & 1u) != 0u;
                                            float v = is_f16 ? HalfBitsToFloat(PickHalf16(u, hi_half)) : BitsToFloat(u);
                                            if (((inst.vop3p_neg_hi >> idx) & 1u) != 0u) v = std::fabs(v);
                                            if (((inst.vop3p_neg_lo >> idx) & 1u) != 0u) v = -v;
                                            return v;
                                        };
                                        const float r = clamp_f32(std::fmaf(read_mix(0u), read_mix(1u), read_mix(2u)));
                                        if (inst.opcode == Rdna2Opcode::V_FMA_MIX_F32) {
                                            wv.vgpr[l][inst.dst.reg_index] = FloatToBits(r);
                                        } else if (inst.opcode == Rdna2Opcode::V_FMA_MIXLO_F16) {
                                            wv.vgpr[l][inst.dst.reg_index] =
                                                (wv.vgpr[l][inst.dst.reg_index] & 0xFFFF0000u) |
                                                static_cast<uint32_t>(FloatToHalfBits(r));
                                        } else {
                                            wv.vgpr[l][inst.dst.reg_index] =
                                                (wv.vgpr[l][inst.dst.reg_index] & 0x0000FFFFu) |
                                                (static_cast<uint32_t>(FloatToHalfBits(r)) << 16);
                                        }
                                    }
                                    break;
                                case Rdna2Opcode::V_MUL_LO_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) * read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_MBCNT_LO_U32_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t m = read_op_u32(inst.src[0], l);
                                        const uint32_t init = read_op_u32(inst.src[1], l);
                                        const uint32_t lane_m = (l < 32u) ? ((1u << l) - 1u) : 0xFFFFFFFFu;
                                        wv.vgpr[l][inst.dst.reg_index] = PopCount32(m & lane_m) + init;
                                    }
                                    break;
                                case Rdna2Opcode::V_MBCNT_HI_U32_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t m = read_op_u32(inst.src[0], l);
                                        const uint32_t init = read_op_u32(inst.src[1], l);
                                        const uint32_t lane_m = (l >= 32u) ? ((1u << (l - 32u)) - 1u) : 0u;
                                        wv.vgpr[l][inst.dst.reg_index] = PopCount32(m & lane_m) + init;
                                    }
                                    break;
                                case Rdna2Opcode::V_READLANE_B32: {
                                    const uint32_t src_lane = read_op_u32(inst.src[1], 0) % wv.lane_count;
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], src_lane));
                                    break;
                                }
                                case Rdna2Opcode::V_CVT_PKNORM_I16_F32:
                                case Rdna2Opcode::V_CVT_PKNORM_U16_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float f0 = read_op_f32(inst.src[0], l);
                                        const float f1 = read_op_f32(inst.src[1], l);
                                        const uint16_t lo = (inst.opcode == Rdna2Opcode::V_CVT_PKNORM_I16_F32)
                                                                ? CvtSnorm16F32(f0)
                                                                : CvtUnorm16F32(f0);
                                        const uint16_t hi = (inst.opcode == Rdna2Opcode::V_CVT_PKNORM_I16_F32)
                                                                ? CvtSnorm16F32(f1)
                                                                : CvtUnorm16F32(f1);
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
                                    }
                                    break;
                                case Rdna2Opcode::V_ADD3_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            read_op_u32(inst.src[0], l) + read_op_u32(inst.src[1], l) + read_op_u32(inst.src[2], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_DOT2_F32_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t a = read_op_u32(inst.src[0], l);
                                        const uint32_t b = read_op_u32(inst.src[1], l);
                                        const float c = read_op_f32(inst.src[2], l);
                                        const float r = HalfBitsToFloat(PickHalf16(a, false)) * HalfBitsToFloat(PickHalf16(b, false)) +
                                                        HalfBitsToFloat(PickHalf16(a, true)) * HalfBitsToFloat(PickHalf16(b, true)) + c;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(r));
                                    }
                                    break;
                                case Rdna2Opcode::V_PERMLANE16_B32:
                                case Rdna2Opcode::V_PERMLANEX16_B32: {
                                    std::array<uint32_t, 64> snap{};
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        snap[l] = read_op_u32(inst.src[0], l);
                                    }
                                    const uint32_t x16 = (inst.opcode == Rdna2Opcode::V_PERMLANEX16_B32) ? 16u : 0u;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t row_lane = l & 15u;
                                        const uint32_t row_base = (l & ~15u) ^ x16;
                                        const uint32_t s1 = read_op_u32(inst.src[1], l);
                                        const uint32_t s2 = read_op_u32(inst.src[2], l);
                                        const uint32_t word = (row_lane < 8u) ? s1 : s2;
                                        const uint32_t sel = (word >> ((row_lane & 7u) * 4u)) & 15u;
                                        const uint32_t tgt = (row_base | sel) % wv.lane_count;
                                        wv.vgpr[l][inst.dst.reg_index] = snap[tgt];
                                    }
                                    break;
                                }

                                // ----- VOP3P (packed 2x16 f16 / u16 / b16) -----
                                case Rdna2Opcode::V_PK_ADD_F16:
                                case Rdna2Opcode::V_PK_MUL_F16:
                                case Rdna2Opcode::V_PK_FMA_F16: {
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t u0 = read_op_u32(inst.src[0], l);
                                        const uint32_t u1 = read_op_u32(inst.src[1], l);
                                        const uint32_t u2 = (inst.src_count >= 3) ? read_op_u32(inst.src[2], l) : 0u;
                                        auto lane_f = [&](uint32_t src, int idx, bool hi_out) -> float {
                                            const bool pick_hi = (((hi_out ? inst.vop3p_op_sel_hi : inst.vop3p_op_sel) >> idx) & 1u) != 0u;
                                            const bool do_neg = (((hi_out ? inst.vop3p_neg_hi : inst.vop3p_neg_lo) >> idx) & 1u) != 0u;
                                            float v = HalfBitsToFloat(PickHalf16(src, pick_hi));
                                            return do_neg ? -v : v;
                                        };
                                        auto eval_h = [&](bool hi_out) -> uint16_t {
                                            const float a = lane_f(u0, 0, hi_out);
                                            const float b = lane_f(u1, 1, hi_out);
                                            float r = 0.0f;
                                            if (inst.opcode == Rdna2Opcode::V_PK_ADD_F16) r = a + b;
                                            else if (inst.opcode == Rdna2Opcode::V_PK_MUL_F16) r = a * b;
                                            else r = std::fma(a, b, lane_f(u2, 2, hi_out));
                                            return FloatToHalfBits(clamp_f32(r));
                                        };
                                        const uint16_t lo = eval_h(false);
                                        const uint16_t hi = eval_h(true);
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
                                    }
                                    break;
                                }
                                case Rdna2Opcode::V_PK_ADD_U16:
                                case Rdna2Opcode::V_PK_LSHLREV_B16:
                                case Rdna2Opcode::V_PK_LSHRREV_B16: {
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t u0 = read_op_u32(inst.src[0], l);
                                        const uint32_t u1 = read_op_u32(inst.src[1], l);
                                        const uint16_t s0_lo = PickHalf16(u0, (inst.vop3p_op_sel & 1u) != 0u);
                                        const uint16_t s0_hi = PickHalf16(u0, (inst.vop3p_op_sel_hi & 1u) != 0u);
                                        const uint16_t s1_lo = PickHalf16(u1, ((inst.vop3p_op_sel >> 1) & 1u) != 0u);
                                        const uint16_t s1_hi = PickHalf16(u1, ((inst.vop3p_op_sel_hi >> 1) & 1u) != 0u);
                                        uint16_t r_lo = 0, r_hi = 0;
                                        if (inst.opcode == Rdna2Opcode::V_PK_ADD_U16) {
                                            r_lo = static_cast<uint16_t>(s0_lo + s1_lo);
                                            r_hi = static_cast<uint16_t>(s0_hi + s1_hi);
                                        } else if (inst.opcode == Rdna2Opcode::V_PK_LSHLREV_B16) {
                                            r_lo = static_cast<uint16_t>(s1_lo << (s0_lo & 15u));
                                            r_hi = static_cast<uint16_t>(s1_hi << (s0_hi & 15u));
                                        } else {
                                            r_lo = static_cast<uint16_t>(s1_lo >> (s0_lo & 15u));
                                            r_hi = static_cast<uint16_t>(s1_hi >> (s0_hi & 15u));
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(r_lo) | (static_cast<uint32_t>(r_hi) << 16);
                                    }
                                    break;
                                }

                                // ----- SMEM -----
                                case Rdna2Opcode::S_LOAD_DWORD:
                                case Rdna2Opcode::S_BUFFER_LOAD_DWORD: {
                                    // Signed 21-bit SMEM immediate offset (xbox-emulator #206) + strict OOB zero-fill (Leonida #4573)
                                    const bool static_zero_soff =
                                        (inst.soffset == 0x7Du || inst.soffset == 0x7Cu || inst.soffset == 128u);
                                    int64_t eff_off = static_cast<int64_t>(inst.mem_offset);
                                    if (inst.soffset < 106u) {
                                        eff_off += static_cast<int64_t>(wv.sgpr[inst.soffset]);
                                    } else if (inst.soffset >= 129u && inst.soffset <= 192u) {
                                        eff_off += static_cast<int64_t>(inst.soffset - 128u);
                                    }
                                    if (eff_off < 0 || eff_off > 0xFFFFFFFFll) {
                                        write_sgpr(inst.dst, 0u);
                                    } else {
                                        const uint32_t addr = static_cast<uint32_t>(eff_off);
                                        const uint32_t slot = addr / 4u;
                                        if (inst.sbase == 0u && static_zero_soff && slot < 16u) {
                                            const uint32_t val = (slot < state.root_constants.size()) ? state.root_constants[slot] : 0u;
                                            write_sgpr(inst.dst, val);
                                        } else {
                                            write_sgpr(inst.dst, ReadBufferWord(state.uav0, addr));
                                        }
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
                                    auto& buf = (inst.srsrc == 0u) ? state.uav0 : state.uav1;
                                    bool is_d16 = false;
                                    uint32_t comps = 1u;
                                    switch (inst.opcode) {
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_X: comps = 1u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY: comps = 2u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ: comps = 3u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW: comps = 4u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X: is_d16 = true; comps = 1u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY: is_d16 = true; comps = 2u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ: is_d16 = true; comps = 3u; break;
                                        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW:
                                        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW: is_d16 = true; comps = 4u; break;
                                        default: break;
                                    }
                                    const auto fmt = (inst.encoding == Rdna2Encoding::MTBUF && inst.mtbuf_format != 0u)
                                                         ? static_cast<Rdna2MtbufFormat>(inst.mtbuf_format)
                                                         : Rdna2MtbufFormat::R32G32B32A32_FLOAT;
                                    const uint8_t dst_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = 0u;
                                        if (inst.offen) addr = read_op_u32(inst.src[1], l);
                                        else if (inst.idxen) addr = read_op_u32(inst.src[1], l) << 2;
                                        addr += static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.soffset != 128u && inst.soffset != 125u) {
                                            addr += read_op_u32(DecodeRdna2SourceOperand(inst.soffset), l);
                                        }
                                        if (is_d16) {
                                            const uint32_t nregs = (comps + 1u) / 2u;
                                            for (uint32_t r = 0; r < nregs; ++r) {
                                                const uint8_t dst_reg = static_cast<uint8_t>(dst_b + r);
                                                const uint32_t lo = DecodeFormatComponentCpu(buf, addr, fmt, r * 2u, true) & 0xFFFFu;
                                                const uint32_t hi = (r * 2u + 1u < comps)
                                                                        ? (DecodeFormatComponentCpu(buf, addr, fmt, r * 2u + 1u, true) & 0xFFFFu)
                                                                        : ((wv.vgpr[l][dst_reg] >> 16u) & 0xFFFFu);
                                                wv.vgpr[l][dst_reg] = lo | (hi << 16);
                                            }
                                        } else {
                                            for (uint32_t c = 0; c < comps; ++c) {
                                                wv.vgpr[l][static_cast<uint8_t>(dst_b + c)] =
                                                    DecodeFormatComponentCpu(buf, addr, fmt, c, false);
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::BUFFER_LOAD_DWORD:
                                case Rdna2Opcode::BUFFER_STORE_DWORD: {
                                    auto& buf = (inst.srsrc == 0u) ? state.uav0 : state.uav1;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = 0u;
                                        if (inst.offen) {
                                            addr = read_op_u32(inst.src[1], l);
                                        } else if (inst.idxen) {
                                            addr = read_op_u32(inst.src[1], l) << 2;
                                        }
                                        addr += static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.soffset != 128u && inst.soffset != 125u) {
                                            addr += read_op_u32(DecodeRdna2SourceOperand(inst.soffset), l);
                                        }
                                        if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                                            wv.vgpr[l][inst.dst.reg_index] = ReadBufferWord(buf, addr);
                                        } else {
                                            WriteBufferWord(buf, addr, read_op_u32(inst.src[0], l));
                                        }
                                    }
                                    break;
                                }

                                // ----- MIMG -----
                                case Rdna2Opcode::IMAGE_LOAD: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    const bool is_u32 = !state.tex2d_srv.mips.empty() && !state.tex2d_srv.mips[0].r32u.empty();
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        if (is_u32) {
                                            const auto px = state.tex2d_srv.LoadR32uRgba(
                                                wv.vgpr[l][vaddr_b], wv.vgpr[l][vaddr_b + 1u], 0u, 0u);
                                            uint8_t out_r = vdata_b;
                                            for (uint32_t ch = 0; ch < 4u; ++ch) {
                                                if ((dmask & (1u << ch)) != 0u) {
                                                    wv.vgpr[l][out_r++] = px[ch];
                                                }
                                            }
                                        } else {
                                            const auto px = state.tex2d_srv.LoadRgba32f(
                                                wv.vgpr[l][vaddr_b], wv.vgpr[l][vaddr_b + 1u], 0u, 0u);
                                            uint8_t out_r = vdata_b;
                                            for (uint32_t ch = 0; ch < 4u; ++ch) {
                                                if ((dmask & (1u << ch)) != 0u) {
                                                    wv.vgpr[l][out_r++] = FloatToBits(px[ch]);
                                                }
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_LOAD_MIP: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    const uint8_t dim = inst.mimg_dim;
                                    const auto& tex = (dim == 2u) ? state.tex3d_srv
                                                                  : ((dim == 3u) ? state.texcube_srv : state.tex2d_srv);
                                    const bool is_u32 = !tex.mips.empty() && !tex.mips[0].r32u.empty();
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t x = wv.vgpr[l][vaddr_b];
                                        const uint32_t y = wv.vgpr[l][vaddr_b + 1u];
                                        const uint32_t z = (dim == 2u || dim == 3u) ? wv.vgpr[l][vaddr_b + 2u] : 0u;
                                        const uint32_t mip = (dim == 2u || dim == 3u) ? wv.vgpr[l][vaddr_b + 3u] : wv.vgpr[l][vaddr_b + 2u];
                                        if (is_u32) {
                                            const auto px = tex.LoadR32uRgba(x, y, z, mip);
                                            uint8_t out_r = vdata_b;
                                            for (uint32_t ch = 0; ch < 4u; ++ch) {
                                                if ((dmask & (1u << ch)) != 0u) {
                                                    wv.vgpr[l][out_r++] = px[ch];
                                                }
                                            }
                                        } else {
                                            const auto px = tex.LoadRgba32f(x, y, z, mip);
                                            uint8_t out_r = vdata_b;
                                            for (uint32_t ch = 0; ch < 4u; ++ch) {
                                                if ((dmask & (1u << ch)) != 0u) {
                                                    wv.vgpr[l][out_r++] = FloatToBits(px[ch]);
                                                }
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_SAMPLE:
                                case Rdna2Opcode::IMAGE_SAMPLE_LZ: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    const float tex_w = (!state.tex2d_srv.mips.empty())
                                                            ? static_cast<float>(state.tex2d_srv.mips[0].width)
                                                            : 1.0f;
                                    const float tex_h = (!state.tex2d_srv.mips.empty())
                                                            ? static_cast<float>(state.tex2d_srv.mips[0].height)
                                                            : 1.0f;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float u = BitsToFloat(wv.vgpr[l][vaddr_b]);
                                        const float v = BitsToFloat(wv.vgpr[l][vaddr_b + 1u]);
                                        float lod = 0.0f;
                                        if (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE) {
                                            if (container.stage == ShaderStage::Pixel && wv.lane_count >= 4u) {
                                                // 2x2 quad helper-lane finite difference mip derivative estimation
                                                const uint32_t lx = l ^ 1u;
                                                const uint32_t ly = l ^ 2u;
                                                const float dudx = (lx < wv.lane_count) ? (BitsToFloat(wv.vgpr[lx][vaddr_b]) - u) * tex_w : 0.0f;
                                                const float dvdx = (lx < wv.lane_count) ? (BitsToFloat(wv.vgpr[lx][vaddr_b + 1u]) - v) * tex_h : 0.0f;
                                                const float dudy = (ly < wv.lane_count) ? (BitsToFloat(wv.vgpr[ly][vaddr_b]) - u) * tex_w : 0.0f;
                                                const float dvdy = (ly < wv.lane_count) ? (BitsToFloat(wv.vgpr[ly][vaddr_b + 1u]) - v) * tex_h : 0.0f;
                                                const float rho2 = std::fmax(dudx * dudx + dvdx * dvdx, dudy * dudy + dvdy * dvdy);
                                                lod = (rho2 > 1e-12f) ? (0.5f * std::log2(rho2)) : 0.0f;
                                            } else {
                                                lod = BitsToFloat(wv.vgpr[l][vaddr_b + 2u]);
                                            }
                                        }
                                        const auto px = state.tex2d_srv.SampleNearest(u, v, lod);
                                        uint8_t out_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                wv.vgpr[l][out_r++] = FloatToBits(px[ch]);
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_SAMPLE_C_LZ: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0x1u;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float dref = BitsToFloat(wv.vgpr[l][vaddr_b]);
                                        const float u = BitsToFloat(wv.vgpr[l][vaddr_b + 1u]);
                                        const float v = BitsToFloat(wv.vgpr[l][vaddr_b + 2u]);
                                        const auto px = state.tex2d_srv.SampleNearest(u, v, 0.0f);
                                        const float cmp_r = (dref <= px[0]) ? 1.0f : 0.0f;
                                        uint8_t out_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                wv.vgpr[l][out_r++] = FloatToBits(cmp_r);
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_STORE: {
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[1].reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        std::array<float, 4> rgba = {0.0f, 0.0f, 0.0f, 1.0f};
                                        uint8_t in_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                rgba[ch] = BitsToFloat(wv.vgpr[l][in_r++]);
                                            }
                                        }
                                        state.tex2d_uav.StoreRgba32f(
                                            wv.vgpr[l][vaddr_b], wv.vgpr[l][vaddr_b + 1u], 0u, rgba);
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP: {
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[1].reg_index);
                                    const uint8_t dim = inst.mimg_dim;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t src_val = wv.vgpr[l][vdata_b];
                                        const uint32_t cmp_val = wv.vgpr[l][vdata_b + 1u];
                                        const uint32_t x = wv.vgpr[l][vaddr_b];
                                        const uint32_t y = wv.vgpr[l][vaddr_b + 1u];
                                        const uint32_t z = (dim == 2u || dim == 3u) ? wv.vgpr[l][vaddr_b + 2u] : 0u;
                                        const uint32_t old = state.tex_u32_uav.AtomicCmpSwapR32u(x, y, z, cmp_val, src_val);
                                        if (inst.glc) {
                                            wv.vgpr[l][vdata_b] = old;
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const BvhHitResult hit = EvalBvhIntersectRayCpu(
                                            state.uav0,
                                            wv.vgpr[l][vaddr_b + 0u],
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 1u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 2u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 3u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 4u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 5u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 6u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 7u]));
                                        wv.vgpr[l][vdata_b + 0u] = FloatToBits(hit.t);
                                        wv.vgpr[l][vdata_b + 1u] = FloatToBits(hit.u);
                                        wv.vgpr[l][vdata_b + 2u] = FloatToBits(hit.v);
                                        wv.vgpr[l][vdata_b + 3u] = hit.hit_id;
                                    }
                                    break;
                                }

                                // ----- DS (LDS / Swizzle) -----
                                case Rdna2Opcode::DS_WRITE_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        lds[(addr >> 2) & lds_mask] = read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::DS_READ_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        wv.vgpr[l][inst.dst.reg_index] = lds[(addr >> 2) & lds_mask];
                                    }
                                    break;
                                case Rdna2Opcode::DS_READ_U8_D16:
                                case Rdna2Opcode::DS_READ_U8_D16_HI:
                                case Rdna2Opcode::DS_READ_I8_D16:
                                case Rdna2Opcode::DS_READ_I8_D16_HI:
                                case Rdna2Opcode::DS_READ_U16_D16:
                                case Rdna2Opcode::DS_READ_U16_D16_HI:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        const uint32_t word = lds[(addr >> 2) & lds_mask];
                                        const bool is_hi = (inst.opcode == Rdna2Opcode::DS_READ_U8_D16_HI ||
                                                            inst.opcode == Rdna2Opcode::DS_READ_I8_D16_HI ||
                                                            inst.opcode == Rdna2Opcode::DS_READ_U16_D16_HI);
                                        const bool is_u16 = (inst.opcode == Rdna2Opcode::DS_READ_U16_D16 ||
                                                             inst.opcode == Rdna2Opcode::DS_READ_U16_D16_HI);
                                        const bool is_i8 = (inst.opcode == Rdna2Opcode::DS_READ_I8_D16 ||
                                                            inst.opcode == Rdna2Opcode::DS_READ_I8_D16_HI);
                                        uint32_t val16 = 0u;
                                        if (is_u16) {
                                            val16 = (word >> ((addr & 2u) * 8u)) & 0xFFFFu;
                                        } else {
                                            const uint32_t b8 = (word >> ((addr & 3u) * 8u)) & 0xFFu;
                                            val16 = is_i8 ? (static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(b8))) & 0xFFFFu)
                                                          : b8;
                                        }
                                        const uint32_t old_v = wv.vgpr[l][inst.dst.reg_index];
                                        wv.vgpr[l][inst.dst.reg_index] = is_hi
                                            ? ((old_v & 0x0000FFFFu) | (val16 << 16))
                                            : ((old_v & 0xFFFF0000u) | val16);
                                    }
                                    break;
                                case Rdna2Opcode::DS_ADD_U32:
                                case Rdna2Opcode::DS_ADD_RTN_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        const uint32_t idx = (addr >> 2) & lds_mask;
                                        const uint32_t prev = lds[idx];
                                        lds[idx] = prev + read_op_u32(inst.src[1], l);
                                        if (inst.opcode == Rdna2Opcode::DS_ADD_RTN_U32) {
                                            wv.vgpr[l][inst.dst.reg_index] = prev;
                                        }
                                    }
                                    break;
                                case Rdna2Opcode::DS_SWIZZLE_B32: {
                                    std::array<uint32_t, 64> snap{};
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        snap[l] = read_op_u32(inst.src[0], l);
                                    }
                                    const uint16_t swiz = static_cast<uint16_t>(static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu);
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t tgt = ComputeRdna2DsSwizzleSrcLane(l, swiz, wv.lane_count);
                                        wv.vgpr[l][inst.dst.reg_index] = snap[tgt % wv.lane_count];
                                    }
                                    break;
                                }

                                // ----- FLAT / GLOBAL / SCRATCH -----
                                case Rdna2Opcode::GLOBAL_LOAD_DWORD:
                                case Rdna2Opcode::FLAT_LOAD_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        wv.vgpr[l][inst.dst.reg_index] = ReadBufferWord(state.uav0, addr);
                                    }
                                    break;
                                case Rdna2Opcode::GLOBAL_STORE_DWORD:
                                case Rdna2Opcode::FLAT_STORE_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        WriteBufferWord(state.uav0, addr, read_op_u32(inst.src[1], l));
                                    }
                                    break;
                                case Rdna2Opcode::SCRATCH_LOAD_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.saddr != 0x7Fu && inst.saddr < 106u) addr += wv.sgpr[inst.saddr];
                                        wv.vgpr[l][inst.dst.reg_index] = wv.scratch[l][(addr >> 2) & scr_mask];
                                    }
                                    break;
                                case Rdna2Opcode::SCRATCH_STORE_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.saddr != 0x7Fu && inst.saddr < 106u) addr += wv.sgpr[inst.saddr];
                                        wv.scratch[l][(addr >> 2) & scr_mask] = read_op_u32(inst.src[1], l);
                                    }
                                    break;

                                // ----- VINTRP (FSSM Pixel Interpolation) -----
                                case Rdna2Opcode::V_INTERP_P1_F32:
                                case Rdna2Opcode::V_INTERP_P2_F32:
                                case Rdna2Opcode::V_INTERP_MOV_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t gid_l = wv.global_thread_ids[l];
                                        const uint32_t ch = inst.vintrp_chan & 3u;
                                        // Unmapped PS input attributes default to (0, 0, 0, 1) (xbox-emulator #181).
                                        float val = (ch == 3u) ? 1.0f : 0.0f;
                                        if (inst.vintrp_attr == 0u && gid_l < state.stage_inputs.size()) {
                                            val = state.stage_inputs[gid_l][ch];
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(val);
                                    }
                                    break;

                                // ----- EXP -----
                                case Rdna2Opcode::EXP:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        auto& ex = state.exports[wv.global_thread_ids[l]];
                                        if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Prim)) {
                                            ex.wrote_prim = true;
                                            if (inst.exp_en == 0x1u) {
                                                const uint32_t p = read_op_u32(inst.src[0], l);
                                                ex.prim = {p & 0x3FFu, (p >> 10) & 0x3FFu, (p >> 20) & 0x3FFu};
                                                if (inst.exp_compr) {
                                                    ex.culled_prim = ((p >> 31) != 0u);
                                                }
                                            } else {
                                                ex.prim = {read_op_u32(inst.src[0], l),
                                                           read_op_u32(inst.src[1], l),
                                                           read_op_u32(inst.src[2], l)};
                                            }
                                        } else {
                                            std::array<float, 4> vec = {0.0f, 0.0f, 0.0f, 0.0f};
                                            for (uint8_t c = 0; c < 4u; ++c) {
                                                if ((inst.exp_en & (1u << c)) != 0u) {
                                                    vec[c] = read_op_f32(inst.src[c], l);
                                                } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && c == 3u) {
                                                    vec[c] = 1.0f;
                                                }
                                            }
                                            if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                                                ex.pos0 = vec;
                                                ex.wrote_pos0 = true;
                                            } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0)) {
                                                ex.param0 = vec;
                                                ex.wrote_param0 = true;
                                            } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0)) {
                                                ex.mrt0 = vec;
                                                ex.wrote_mrt0 = true;
                                            }
                                        }
                                    }
                                    break;

                                default:
                                    return Status::Error(
                                        StatusCode::NotImplemented,
                                        std::string("Unhandled RDNA2 opcode in CPU interpreter: ") + Rdna2OpcodeName(inst.opcode));
                            }

                            wv.pc = next_pc;
                        }
                    }

                    // Check if any non-halted wave is waiting at a barrier
                    bool any_alive = false;
                    bool all_alive_at_barrier = true;
                    for (const auto& wv : waves) {
                        if (!wv.halted) {
                            any_alive = true;
                            if (!wv.at_barrier) all_alive_at_barrier = false;
                        }
                    }
                    if (!any_alive) break;
                    if (all_alive_at_barrier) {
                        ++state.barriers_synchronized;
                        for (auto& wv : waves) {
                            if (!wv.halted) wv.at_barrier = false;
                        }
                    } else if (!any_running) {
                        break;
                    }
                }
            }
        }
    }

    return Status::Ok();
}

Status ExecuteRdna2BlobCpu(const std::vector<uint8_t>& blob, Rdna2CpuExecState& state) {
    XboxDxbcContainer container{};
    if (auto st = ParseXboxShaderContainer(blob, container); !st.ok()) {
        return st;
    }
    if (!container.has_xs2) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Blob does not contain an Xbox XS2 RDNA2 shader payload");
    }
    std::vector<Rdna2Instruction> insns;
    if (auto st = DisassembleRdna2(container.xs2.code_words, insns); !st.ok()) {
        return st;
    }
    return ExecuteRdna2Cpu(container.xs2, insns, state);
}

}  // namespace openscarlett
