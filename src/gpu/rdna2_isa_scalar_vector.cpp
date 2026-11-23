#include "rdna2_isa_scalar_vector.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace openscarlett::rdna2_sv {

namespace {

// ---------------------------------------------------------------------------
// Portable Bit, Float, Double, FP16, SDWA & DPP Helpers (C++20, Zero Intrinsics)
// ---------------------------------------------------------------------------
inline uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

inline float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

inline uint64_t DoubleToBits(double d) noexcept {
    uint64_t b = 0u;
    std::memcpy(&b, &d, sizeof(b));
    return b;
}

inline double BitsToDouble(uint64_t b) noexcept {
    double d = 0.0;
    std::memcpy(&d, &b, sizeof(d));
    return d;
}

inline int32_t SignExtend32(uint32_t v, unsigned bits) noexcept {
    const unsigned shift = 32u - bits;
    return static_cast<int32_t>(v << shift) >> shift;
}

inline uint32_t BitReverse32(uint32_t v) noexcept {
    v = ((v >> 1u) & 0x55555555u) | ((v & 0x55555555u) << 1u);
    v = ((v >> 2u) & 0x33333333u) | ((v & 0x33333333u) << 2u);
    v = ((v >> 4u) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4u);
    v = ((v >> 8u) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8u);
    return (v >> 16u) | (v << 16u);
}

inline uint64_t BitReverse64(uint64_t v) noexcept {
    const uint64_t lo = static_cast<uint64_t>(BitReverse32(static_cast<uint32_t>(v & 0xFFFFFFFFull)));
    const uint64_t hi = static_cast<uint64_t>(BitReverse32(static_cast<uint32_t>(v >> 32u)));
    return (lo << 32u) | hi;
}

inline int32_t FirstBitLow32(uint32_t v) noexcept {
    if (v == 0u) return -1;
    return static_cast<int32_t>(std::countr_zero(v));
}

inline int32_t FirstBitLow64(uint64_t v) noexcept {
    if (v == 0ull) return -1;
    return static_cast<int32_t>(std::countr_zero(v));
}

inline int32_t FirstBitHighU32(uint32_t v) noexcept {
    if (v == 0u) return -1;
    return static_cast<int32_t>(std::countl_zero(v));
}

inline int32_t FirstBitHighU64(uint64_t v) noexcept {
    if (v == 0ull) return -1;
    return static_cast<int32_t>(std::countl_zero(v));
}

inline int32_t FirstBitHighI32(int32_t s) noexcept {
    if (s == 0 || s == -1) return -1;
    const uint32_t u = (s < 0) ? ~static_cast<uint32_t>(s) : static_cast<uint32_t>(s);
    return static_cast<int32_t>(std::countl_zero(u));
}

inline int32_t FirstBitHighI64(int64_t s) noexcept {
    if (s == 0 || s == -1) return -1;
    const uint64_t u = (s < 0) ? ~static_cast<uint64_t>(s) : static_cast<uint64_t>(s);
    return static_cast<int32_t>(std::countl_zero(u));
}

inline uint64_t BitReplicate32To64(uint32_t v) noexcept {
    uint64_t out = 0ull;
    for (uint32_t i = 0u; i < 32u; ++i) {
        if ((v & (1u << i)) != 0u) {
            out |= (3ull << (i * 2u));
        }
    }
    return out;
}

inline float F16ToF32(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16u;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10u) & 0x1Fu;
    const uint32_t frac = static_cast<uint32_t>(h) & 0x03FFu;
    if (exp == 0u) {
        if (frac == 0u) return BitsToFloat(sign);
        uint32_t m = frac;
        int32_t e = -14;
        while ((m & 0x0400u) == 0u) {
            m <<= 1u;
            --e;
        }
        m &= 0x03FFu;
        const uint32_t f32_exp = static_cast<uint32_t>(e + 127) << 23u;
        return BitsToFloat(sign | f32_exp | (m << 13u));
    }
    if (exp == 31u) {
        const uint32_t f32_frac = (frac != 0u) ? ((frac << 13u) | 0x00400000u) : 0u;
        return BitsToFloat(sign | 0x7F800000u | f32_frac);
    }
    const uint32_t f32_exp = (exp + 112u) << 23u;
    return BitsToFloat(sign | f32_exp | (frac << 13u));
}

inline uint16_t F32ToF16(float f, bool rtz = false) noexcept {
    const uint32_t bits = FloatToBits(f);
    const uint16_t sign = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
    const uint32_t exp = (bits >> 23u) & 0xFFu;
    const uint32_t frac = bits & 0x007FFFFFu;
    if (exp == 255u) {
        if (frac == 0u) return static_cast<uint16_t>(sign | 0x7C00u);
        return static_cast<uint16_t>(sign | 0x7E00u | ((frac >> 13u) & 0x01FFu));
    }
    const int32_t new_exp = static_cast<int32_t>(exp) - 127 + 15;
    if (new_exp >= 31) {
        return rtz ? static_cast<uint16_t>(sign | 0x7BFFu) : static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (new_exp <= 0) {
        if (new_exp < -10) return sign;
        const uint32_t mant = frac | 0x00800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - new_exp);
        uint32_t hfrac = mant >> shift;
        if (!rtz) {
            const uint32_t rem = mant & ((1u << shift) - 1u);
            const uint32_t half = 1u << (shift - 1u);
            if (rem > half || (rem == half && (hfrac & 1u) != 0u)) {
                ++hfrac;
            }
        }
        return static_cast<uint16_t>(sign | hfrac);
    }
    uint32_t hfrac = frac >> 13u;
    uint32_t hexp = static_cast<uint32_t>(new_exp);
    if (!rtz) {
        const uint32_t rem = frac & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (hfrac & 1u) != 0u)) {
            ++hfrac;
            if (hfrac >= 0x0400u) {
                hfrac = 0u;
                ++hexp;
                if (hexp >= 31u) return static_cast<uint16_t>(sign | 0x7C00u);
            }
        }
    }
    return static_cast<uint16_t>(sign | (hexp << 10u) | (hfrac & 0x03FFu));
}

inline uint32_t ApplySdwaSrc(
    uint32_t raw,
    uint8_t sel,
    bool sext,
    bool is_float,
    bool neg,
    bool abs_v) noexcept {
    uint32_t v = raw;
    switch (sel & 7u) {
        case kSdwaSelByte0: v = raw & 0xFFu; if (sext) v = static_cast<uint32_t>(SignExtend32(v, 8u)); break;
        case kSdwaSelByte1: v = (raw >> 8u) & 0xFFu; if (sext) v = static_cast<uint32_t>(SignExtend32(v, 8u)); break;
        case kSdwaSelByte2: v = (raw >> 16u) & 0xFFu; if (sext) v = static_cast<uint32_t>(SignExtend32(v, 8u)); break;
        case kSdwaSelByte3: v = (raw >> 24u) & 0xFFu; if (sext) v = static_cast<uint32_t>(SignExtend32(v, 8u)); break;
        case kSdwaSelWord0: v = raw & 0xFFFFu; if (sext) v = static_cast<uint32_t>(SignExtend32(v, 16u)); break;
        case kSdwaSelWord1: v = (raw >> 16u) & 0xFFFFu; if (sext) v = static_cast<uint32_t>(SignExtend32(v, 16u)); break;
        default: break;
    }
    if (is_float) {
        if (abs_v) v &= 0x7FFFFFFFu;
        if (neg) v ^= 0x80000000u;
    }
    return v;
}

inline uint32_t ApplySdwaDst(
    uint32_t old_dst,
    uint32_t result,
    uint8_t dst_sel,
    uint8_t dst_unused,
    bool is_float,
    bool clamp,
    uint8_t omod) noexcept {
    uint32_t r = result;
    if (is_float) {
        float f = BitsToFloat(r);
        if (omod == 1u) f *= 2.0f;
        else if (omod == 2u) f *= 4.0f;
        else if (omod == 3u) f *= 0.5f;
        if (clamp) {
            if (std::isnan(f) || f < 0.0f) f = 0.0f;
            else if (f > 1.0f) f = 1.0f;
        }
        r = FloatToBits(f);
    }
    const uint8_t sel = dst_sel & 7u;
    if (sel == kSdwaSelDword || sel > kSdwaSelDword) {
        return r;
    }
    uint32_t mask = 0xFFFFFFFFu;
    uint32_t shift = 0u;
    uint32_t sign_bit = 0u;
    switch (sel) {
        case kSdwaSelByte0: mask = 0x000000FFu; shift = 0u; sign_bit = (r >> 7u) & 1u; break;
        case kSdwaSelByte1: mask = 0x0000FF00u; shift = 8u; sign_bit = (r >> 7u) & 1u; break;
        case kSdwaSelByte2: mask = 0x00FF0000u; shift = 16u; sign_bit = (r >> 7u) & 1u; break;
        case kSdwaSelByte3: mask = 0xFF000000u; shift = 24u; sign_bit = (r >> 7u) & 1u; break;
        case kSdwaSelWord0: mask = 0x0000FFFFu; shift = 0u; sign_bit = (r >> 15u) & 1u; break;
        case kSdwaSelWord1: mask = 0xFFFF0000u; shift = 16u; sign_bit = (r >> 15u) & 1u; break;
        default: return r;
    }
    const uint32_t placed = (r << shift) & mask;
    if (dst_unused == kSdwaUnusedPreserve) {
        return (old_dst & ~mask) | placed;
    }
    if (dst_unused == kSdwaUnusedSext && sign_bit != 0u) {
        const uint32_t upper_mask = ~((mask | (mask - 1u)));
        return placed | upper_mask;
    }
    return placed;
}

inline bool Dpp16LaneEnabled(uint32_t lane, uint8_t row_mask, uint8_t bank_mask) noexcept {
    const uint32_t row = (lane >> 4u) & 3u;
    const uint32_t bank = (lane >> 2u) & 3u;
    return (((row_mask >> row) & 1u) != 0u) && (((bank_mask >> bank) & 1u) != 0u);
}

inline int32_t ComputeDpp16SrcLane(uint32_t lane, uint16_t ctrl, uint32_t wave_size) noexcept {
    const uint32_t row_base = lane & ~15u;
    const uint32_t in_row = lane & 15u;
    if (ctrl <= 0xFFu) {
        const uint32_t quad_base = lane & ~3u;
        const uint32_t in_quad = lane & 3u;
        const uint32_t sel = (ctrl >> (in_quad * 2u)) & 3u;
        return static_cast<int32_t>(quad_base | sel);
    }
    if (ctrl >= 0x101u && ctrl <= 0x10Fu) {
        const uint32_t sh = ctrl - 0x100u;
        const uint32_t src_in_row = in_row + sh;
        return (src_in_row < 16u) ? static_cast<int32_t>(row_base + src_in_row) : -1;
    }
    if (ctrl >= 0x111u && ctrl <= 0x11Fu) {
        const uint32_t sh = ctrl - 0x110u;
        return (in_row >= sh) ? static_cast<int32_t>(row_base + (in_row - sh)) : -1;
    }
    if (ctrl >= 0x121u && ctrl <= 0x12Fu) {
        const uint32_t sh = ctrl - 0x120u;
        return static_cast<int32_t>(row_base + ((in_row + 16u - sh) & 15u));
    }
    if (ctrl == kDppWaveShl1) {
        return (lane + 1u < wave_size) ? static_cast<int32_t>(lane + 1u) : -1;
    }
    if (ctrl == kDppWaveRol1) {
        return static_cast<int32_t>((lane + 1u) % wave_size);
    }
    if (ctrl == kDppWaveShr1) {
        return (lane > 0u) ? static_cast<int32_t>(lane - 1u) : -1;
    }
    if (ctrl == kDppWaveRor1) {
        return static_cast<int32_t>((lane + wave_size - 1u) % wave_size);
    }
    if (ctrl == kDppRowMirror) {
        return static_cast<int32_t>(row_base + (15u - in_row));
    }
    if (ctrl == kDppRowHalfMirror) {
        return static_cast<int32_t>((lane & ~7u) + (7u - (lane & 7u)));
    }
    if (ctrl == kDppRowBcast15) {
        return (lane >= 16u) ? static_cast<int32_t>((lane & ~15u) - 1u) : -1;
    }
    if (ctrl == kDppRowBcast31) {
        return (lane >= 32u) ? static_cast<int32_t>((lane & ~31u) - 1u) : -1;
    }
    if (ctrl >= 0x150u && ctrl <= 0x15Fu) {
        return static_cast<int32_t>(row_base + (ctrl & 15u));
    }
    if (ctrl >= 0x160u && ctrl <= 0x16Fu) {
        return static_cast<int32_t>(row_base + (in_row ^ (ctrl & 15u)));
    }
    return static_cast<int32_t>(lane);
}

inline uint32_t ComputeDpp8SrcLane(uint32_t lane, const std::array<uint8_t, 8>& sel) noexcept {
    const uint32_t grp = lane & ~7u;
    return grp + static_cast<uint32_t>(sel[lane & 7u] & 7u);
}

// ---------------------------------------------------------------------------
// Complete GFX10.3 Scalar + Vector Opcode Table
// ---------------------------------------------------------------------------
using Enc = Rdna2Encoding;
using DK = Rdna2DataKind;
using SC = SvSemanticClass;

ScalarVectorOpcodeEntry MakeEntry(
    Enc enc,
    uint16_t raw,
    const char* mnem,
    uint8_t srcs,
    bool has_dst,
    bool dst_sgpr,
    bool dst_64,
    bool wscc,
    bool wvcc,
    bool wexec,
    bool mlit,
    uint8_t bw,
    DK dk,
    SC sc,
    bool spv = true,
    bool dxil = true,
    bool cpu = true,
    bool gpu = true) {
    return ScalarVectorOpcodeEntry{
        enc, raw, mnem, srcs, has_dst, dst_sgpr, dst_64,
        wscc, wvcc, wexec, mlit, bw, dk, static_cast<uint16_t>(sc),
        spv, dxil, cpu, gpu};
}

std::vector<ScalarVectorOpcodeEntry> BuildScalarVectorOpcodeTable() {
    std::vector<ScalarVectorOpcodeEntry> t;
    t.reserve(768u);

    auto add = [&](Enc enc, uint16_t raw, const char* mnem, uint8_t srcs,
                   bool has_dst, bool dst_sgpr, bool dst_64,
                   bool wscc, bool wvcc, bool wexec, bool mlit,
                   uint8_t bw, DK dk, SC sc,
                   bool spv = true, bool dxil = true, bool cpu = true, bool gpu = true) {
        t.push_back(MakeEntry(enc, raw, mnem, srcs, has_dst, dst_sgpr, dst_64,
                              wscc, wvcc, wexec, mlit, bw, dk, sc, spv, dxil, cpu, gpu));
    };

    // ==================== 1. SOP2 (51 unique mnemonics) ====================
    add(Enc::SOP2, 0x00, "s_add_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x01, "s_sub_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x02, "s_add_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x03, "s_sub_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x04, "s_addc_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x05, "s_subb_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x06, "s_min_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x07, "s_min_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x08, "s_max_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x09, "s_max_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x0A, "s_cselect_b32", 2, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x0B, "s_cselect_b64", 2, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x0E, "s_and_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x0F, "s_and_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x10, "s_or_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x11, "s_or_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x12, "s_xor_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x13, "s_xor_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x14, "s_andn2_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x15, "s_andn2_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x16, "s_orn2_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x17, "s_orn2_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x18, "s_nand_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x19, "s_nand_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x1A, "s_nor_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x1B, "s_nor_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x1C, "s_xnor_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x1D, "s_xnor_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP2, 0x1E, "s_lshl_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x1F, "s_lshl_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x20, "s_lshr_b32", 2, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x21, "s_lshr_b64", 2, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x22, "s_ashr_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x23, "s_ashr_i64", 2, true, true, true, true, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x24, "s_bfm_b32", 2, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x25, "s_bfm_b64", 2, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x26, "s_absdiff_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x27, "s_bfe_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x28, "s_bfe_i32", 2, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x29, "s_bfe_u64", 2, true, true, true, true, false, false, false, 64, DK::UnsignedInt, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x2A, "s_bfe_i64", 2, true, true, true, true, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x2B, "s_cbranch_g_fork", 2, false, false, false, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOP2, 0x2C, "s_mul_i32", 2, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x2D, "s_rfe_restore_b64", 2, false, false, false, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOP2, 0x2E, "s_lshl1_add_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x2F, "s_lshl2_add_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x30, "s_lshl3_add_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x31, "s_lshl4_add_u32", 2, true, true, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x32, "s_pack_ll_b32_b16", 2, true, true, false, false, false, false, false, 32, DK::Packed16, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x33, "s_pack_lh_b32_b16", 2, true, true, false, false, false, false, false, 32, DK::Packed16, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x34, "s_pack_hh_b32_b16", 2, true, true, false, false, false, false, false, 32, DK::Packed16, SC::ScalarShiftBit);
    add(Enc::SOP2, 0x35, "s_mul_hi_u32", 2, true, true, false, false, false, false, false, 32, DK::UnsignedInt, SC::ScalarArith);
    add(Enc::SOP2, 0x36, "s_mul_hi_i32", 2, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarArith);

    // ==================== 2. SOP1 (58 unique mnemonics) ====================
    add(Enc::SOP1, 0x03, "s_mov_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x04, "s_mov_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x05, "s_cmov_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x06, "s_cmov_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x07, "s_not_b32", 1, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x08, "s_bcnt1_i32_b32", 1, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x09, "s_bcnt1_i32_b64", 1, true, true, false, true, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x0A, "s_not_b64", 1, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x0B, "s_wqm_b32", 1, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x0C, "s_wqm_b64", 1, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x0D, "s_brev_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x0E, "s_brev_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x0F, "s_bcnt0_i32_b32", 1, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x10, "s_bcnt0_i32_b64", 1, true, true, false, true, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x11, "s_ff0_i32_b32", 1, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x12, "s_ff0_i32_b64", 1, true, true, false, false, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x13, "s_ff1_i32_b32", 1, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x14, "s_ff1_i32_b64", 1, true, true, false, false, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x15, "s_flbit_i32_b32", 1, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x16, "s_flbit_i32_b64", 1, true, true, false, false, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x17, "s_flbit_i32", 1, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x18, "s_flbit_i32_i64", 1, true, true, false, false, false, false, false, 64, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x19, "s_sext_i32_i8", 1, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x1A, "s_sext_i32_i16", 1, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x1B, "s_bitset0_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x1C, "s_bitset0_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x1D, "s_setpc_b64", 1, false, false, false, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOP1, 0x1E, "s_swappc_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOP1, 0x1F, "s_getpc_b64", 0, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOP1, 0x20, "s_rfe_b64", 1, false, false, false, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOP1, 0x21, "s_bitset1_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x22, "s_bitset1_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x24, "s_and_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x25, "s_or_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x26, "s_xor_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x27, "s_andn2_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x28, "s_orn2_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x29, "s_nand_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x2A, "s_nor_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x2B, "s_xnor_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x2C, "s_quadmask_b32", 1, true, true, false, true, false, false, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x2D, "s_quadmask_b64", 1, true, true, true, true, false, false, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x2E, "s_movrels_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x2F, "s_movrels_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x30, "s_movreld_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x31, "s_movreld_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x34, "s_abs_i32", 1, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOP1, 0x35, "s_mov_fed_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x37, "s_andn1_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x38, "s_orn1_saveexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x39, "s_andn1_wrexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x3A, "s_andn2_wrexec_b64", 1, true, true, true, true, false, true, false, 64, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x3B, "s_bitreplicate_b64_b32", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarShiftBit);
    add(Enc::SOP1, 0x3C, "s_and_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x3D, "s_or_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x3E, "s_xor_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x3F, "s_andn2_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x40, "s_orn2_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x41, "s_nand_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x42, "s_nor_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x43, "s_xnor_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x44, "s_andn1_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x45, "s_orn1_saveexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x46, "s_andn1_wrexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x47, "s_andn2_wrexec_b32", 1, true, true, false, true, false, true, false, 32, DK::Untyped, SC::ScalarExecMask);
    add(Enc::SOP1, 0x49, "s_movrelsd_2_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarLogic);
    add(Enc::SOP1, 0x4A, "s_movrelsd_2_b64", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarLogic);

    // ==================== 3. SOPK (28 unique mnemonics) ====================
    add(Enc::SOPK, 0x00, "s_movk_i32", 0, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOPK, 0x01, "s_version", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x02, "s_cmovk_i32", 0, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOPK, 0x03, "s_cmpk_eq_i32", 0, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x04, "s_cmpk_lg_i32", 0, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x05, "s_cmpk_gt_i32", 0, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x06, "s_cmpk_ge_i32", 0, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x07, "s_cmpk_lt_i32", 0, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x08, "s_cmpk_le_i32", 0, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x09, "s_cmpk_eq_u32", 0, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x0A, "s_cmpk_lg_u32", 0, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x0B, "s_cmpk_gt_u32", 0, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x0C, "s_cmpk_ge_u32", 0, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x0D, "s_cmpk_lt_u32", 0, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x0E, "s_cmpk_le_u32", 0, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPK, 0x0F, "s_addk_i32", 0, true, true, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOPK, 0x10, "s_mulk_i32", 0, true, true, false, false, false, false, false, 32, DK::SignedInt, SC::ScalarArith);
    add(Enc::SOPK, 0x11, "s_cbranch_i_fork", 0, false, false, false, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPK, 0x12, "s_getreg_b32", 0, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x13, "s_setreg_b32", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x15, "s_setreg_imm32_b32", 0, false, false, false, false, false, false, true, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x16, "s_call_b64", 0, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPK, 0x17, "s_waitcnt_vscnt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x18, "s_waitcnt_vmcnt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x19, "s_waitcnt_expcnt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x1A, "s_waitcnt_lgkmcnt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPK, 0x1B, "s_subvector_loop_begin", 0, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPK, 0x1C, "s_subvector_loop_end", 0, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);

    // ==================== 4. SOPC (18 unique mnemonics) ====================
    add(Enc::SOPC, 0x00, "s_cmp_eq_i32", 2, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x01, "s_cmp_lg_i32", 2, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x02, "s_cmp_gt_i32", 2, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x03, "s_cmp_ge_i32", 2, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x04, "s_cmp_lt_i32", 2, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x05, "s_cmp_le_i32", 2, false, false, false, true, false, false, false, 32, DK::SignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x06, "s_cmp_eq_u32", 2, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x07, "s_cmp_lg_u32", 2, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x08, "s_cmp_gt_u32", 2, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x09, "s_cmp_ge_u32", 2, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x0A, "s_cmp_lt_u32", 2, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x0B, "s_cmp_le_u32", 2, false, false, false, true, false, false, false, 32, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x0C, "s_bitcmp0_b32", 2, false, false, false, true, false, false, false, 32, DK::Untyped, SC::ScalarCompare);
    add(Enc::SOPC, 0x0D, "s_bitcmp1_b32", 2, false, false, false, true, false, false, false, 32, DK::Untyped, SC::ScalarCompare);
    add(Enc::SOPC, 0x0E, "s_bitcmp0_b64", 2, false, false, false, true, false, false, false, 64, DK::Untyped, SC::ScalarCompare);
    add(Enc::SOPC, 0x0F, "s_bitcmp1_b64", 2, false, false, false, true, false, false, false, 64, DK::Untyped, SC::ScalarCompare);
    add(Enc::SOPC, 0x12, "s_cmp_eq_u64", 2, false, false, false, true, false, false, false, 64, DK::UnsignedInt, SC::ScalarCompare);
    add(Enc::SOPC, 0x13, "s_cmp_lg_u64", 2, false, false, false, true, false, false, false, 64, DK::UnsignedInt, SC::ScalarCompare);

    // ==================== 5. SOPP (39 unique mnemonics) ====================
    add(Enc::SOPP, 0x00, "s_nop", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x01, "s_endpgm", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x02, "s_branch", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x03, "s_wakeup", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x04, "s_cbranch_scc0", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x05, "s_cbranch_scc1", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x06, "s_cbranch_vccz", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x07, "s_cbranch_vccnz", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x08, "s_cbranch_execz", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x09, "s_cbranch_execnz", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x0A, "s_barrier", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x0B, "s_setkill", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x0C, "s_waitcnt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x0D, "s_sethalt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x0E, "s_sleep", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x0F, "s_setprio", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x10, "s_sendmsg", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x11, "s_sendmsghalt", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x12, "s_trap", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x13, "s_icache_inv", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x14, "s_incperflevel", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x15, "s_decperflevel", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x16, "s_ttracedata", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x17, "s_cbranch_cdbgsys", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x18, "s_cbranch_cdbguser", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x19, "s_cbranch_cdbgsys_or_user", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x1A, "s_cbranch_cdbgsys_and_user", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x1B, "s_endpgm_saved", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x1C, "s_set_gpr_idx_off", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x1D, "s_set_gpr_idx_mode", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x1E, "s_endpgm_ordered_ps_done", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x1F, "s_code_end", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarControl);
    add(Enc::SOPP, 0x20, "s_inst_prefetch", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x21, "s_clause", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x22, "s_wait_idle", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x23, "s_waitcnt_depctr", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x24, "s_round_mode", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x25, "s_denorm_mode", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SOPP, 0x28, "s_ttracedata_imm", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);

    // ==================== 6. SMEM (68 unique mnemonics) ====================
    add(Enc::SMEM, 0x00, "s_load_dword", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x01, "s_load_dwordx2", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x02, "s_load_dwordx4", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x03, "s_load_dwordx8", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x04, "s_load_dwordx16", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x08, "s_buffer_load_dword", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x09, "s_buffer_load_dwordx2", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x0A, "s_buffer_load_dwordx4", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x0B, "s_buffer_load_dwordx8", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x0C, "s_buffer_load_dwordx16", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x10, "s_scratch_load_dword", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x11, "s_scratch_load_dwordx2", 1, true, true, true, false, false, false, false, 64, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x12, "s_scratch_load_dwordx4", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::ScalarMemory);
    add(Enc::SMEM, 0x1F, "s_gl1_inv", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SMEM, 0x20, "s_dcache_inv", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SMEM, 0x24, "s_memtime", 0, true, true, true, false, false, false, false, 64, DK::UnsignedInt, SC::ScalarSystem);
    add(Enc::SMEM, 0x25, "s_memrealtime", 0, true, true, true, false, false, false, false, 64, DK::UnsignedInt, SC::ScalarSystem);
    add(Enc::SMEM, 0x26, "s_atc_probe", 1, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SMEM, 0x27, "s_atc_probe_buffer", 1, false, false, false, false, false, false, false, 32, DK::Untyped, SC::ScalarSystem);
    add(Enc::SMEM, 0x2A, "s_get_waveid_in_workgroup", 0, true, true, false, false, false, false, false, 32, DK::UnsignedInt, SC::ScalarSystem);
    {
        static constexpr const char* kAtomOps[13] = {
            "swap", "cmpswap", "add", "sub", "smin", "umin", "smax",
            "umax", "and", "or", "xor", "inc", "dec"};
        static constexpr const char* kSAtom32[13] = {
            "s_atomic_swap", "s_atomic_cmpswap", "s_atomic_add", "s_atomic_sub",
            "s_atomic_smin", "s_atomic_umin", "s_atomic_smax", "s_atomic_umax",
            "s_atomic_and", "s_atomic_or", "s_atomic_xor", "s_atomic_inc", "s_atomic_dec"};
        static constexpr const char* kSAtom64[13] = {
            "s_atomic_swap_x2", "s_atomic_cmpswap_x2", "s_atomic_add_x2", "s_atomic_sub_x2",
            "s_atomic_smin_x2", "s_atomic_umin_x2", "s_atomic_smax_x2", "s_atomic_umax_x2",
            "s_atomic_and_x2", "s_atomic_or_x2", "s_atomic_xor_x2", "s_atomic_inc_x2", "s_atomic_dec_x2"};
        static constexpr const char* kSBufAtom32[13] = {
            "s_buffer_atomic_swap", "s_buffer_atomic_cmpswap", "s_buffer_atomic_add", "s_buffer_atomic_sub",
            "s_buffer_atomic_smin", "s_buffer_atomic_umin", "s_buffer_atomic_smax", "s_buffer_atomic_umax",
            "s_buffer_atomic_and", "s_buffer_atomic_or", "s_buffer_atomic_xor", "s_buffer_atomic_inc", "s_buffer_atomic_dec"};
        static constexpr const char* kSBufAtom64[13] = {
            "s_buffer_atomic_swap_x2", "s_buffer_atomic_cmpswap_x2", "s_buffer_atomic_add_x2", "s_buffer_atomic_sub_x2",
            "s_buffer_atomic_smin_x2", "s_buffer_atomic_umin_x2", "s_buffer_atomic_smax_x2", "s_buffer_atomic_umax_x2",
            "s_buffer_atomic_and_x2", "s_buffer_atomic_or_x2", "s_buffer_atomic_xor_x2", "s_buffer_atomic_inc_x2", "s_buffer_atomic_dec_x2"};
        (void)kAtomOps;
        for (uint16_t i = 0u; i < 13u; ++i) {
            add(Enc::SMEM, static_cast<uint16_t>(0x40u + i), kSAtom32[i], 1, true, true, false, false, false, false, false, 32, DK::UnsignedInt, SC::ScalarAtomic);
            add(Enc::SMEM, static_cast<uint16_t>(0x60u + i), kSAtom64[i], 1, true, true, true, false, false, false, false, 64, DK::UnsignedInt, SC::ScalarAtomic);
            add(Enc::SMEM, static_cast<uint16_t>(0x80u + i), kSBufAtom32[i], 1, true, true, false, false, false, false, false, 32, DK::UnsignedInt, SC::ScalarAtomic);
            add(Enc::SMEM, static_cast<uint16_t>(0xA0u + i), kSBufAtom64[i], 1, true, true, true, false, false, false, false, 64, DK::UnsignedInt, SC::ScalarAtomic);
        }
    }

    // ==================== 7. VOP1 (75 unique mnemonics) ====================
    add(Enc::VOP1, 0x00, "v_nop", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x01, "v_mov_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x02, "v_readfirstlane_b32", 1, true, true, false, false, false, false, false, 32, DK::Untyped, SC::VectorCrossLane);
    add(Enc::VOP1, 0x03, "v_cvt_i32_f64", 1, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x04, "v_cvt_f64_i32", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x05, "v_cvt_f32_i32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x06, "v_cvt_f32_u32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x07, "v_cvt_u32_f32", 1, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x08, "v_cvt_i32_f32", 1, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x0A, "v_cvt_f16_f32", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x0B, "v_cvt_f32_f16", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x0C, "v_cvt_rpi_i32_f32", 1, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x0D, "v_cvt_flr_i32_f32", 1, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x0E, "v_cvt_off_f32_i4", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x0F, "v_cvt_f32_f64", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x10, "v_cvt_f64_f32", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x11, "v_cvt_f32_ubyte0", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x12, "v_cvt_f32_ubyte1", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x13, "v_cvt_f32_ubyte2", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x14, "v_cvt_f32_ubyte3", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x15, "v_cvt_u32_f64", 1, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x16, "v_cvt_f64_u32", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x17, "v_trunc_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x18, "v_ceil_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x19, "v_rndne_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x1A, "v_floor_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x1B, "v_pipeflush", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x20, "v_fract_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x21, "v_trunc_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x22, "v_ceil_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x23, "v_rndne_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x24, "v_floor_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x25, "v_exp_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x27, "v_log_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x2A, "v_rcp_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x2B, "v_rcp_iflag_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x2E, "v_rsq_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x2F, "v_rcp_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x31, "v_rsq_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x33, "v_sqrt_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x34, "v_sqrt_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x35, "v_sin_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x36, "v_cos_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x37, "v_not_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP1, 0x38, "v_bfrev_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP1, 0x39, "v_ffbh_u32", 1, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP1, 0x3A, "v_ffbl_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP1, 0x3B, "v_ffbh_i32", 1, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP1, 0x3C, "v_frexp_exp_i32_f64", 1, true, false, false, false, false, false, false, 64, DK::SignedInt, SC::VectorTranscendental);
    add(Enc::VOP1, 0x3D, "v_frexp_mant_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x3E, "v_fract_f64", 1, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x3F, "v_frexp_exp_i32_f32", 1, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorTranscendental);
    add(Enc::VOP1, 0x40, "v_frexp_mant_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x41, "v_clrexcp", 0, false, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x42, "v_movreld_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x43, "v_movrels_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x44, "v_movrelsd_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x48, "v_movrelsd_2_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x50, "v_cvt_f16_u16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x51, "v_cvt_f16_i16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorConvert);
    add(Enc::VOP1, 0x52, "v_cvt_u16_f16", 1, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x53, "v_cvt_i16_f16", 1, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x54, "v_rcp_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x55, "v_sqrt_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x56, "v_rsq_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x57, "v_log_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x58, "v_exp_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x59, "v_frexp_mant_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x5A, "v_frexp_exp_i16_f16", 1, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorTranscendental);
    add(Enc::VOP1, 0x5B, "v_floor_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x5C, "v_ceil_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x5D, "v_trunc_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x5E, "v_rndne_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x5F, "v_fract_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x60, "v_sin_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x61, "v_cos_f16", 1, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP1, 0x62, "v_sat_pk_u8_i16", 1, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP1, 0x63, "v_cvt_norm_i16_f16", 1, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x64, "v_cvt_norm_u16_f16", 1, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorConvert);
    add(Enc::VOP1, 0x65, "v_swap_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);
    add(Enc::VOP1, 0x68, "v_swaprel_b32", 1, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorMove);

    // ==================== 8. VOP2 (47 unique mnemonics) ====================
    add(Enc::VOP2, 0x01, "v_cndmask_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x02, "v_dot2c_f32_f16", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorDotProduct);
    add(Enc::VOP2, 0x03, "v_add_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x04, "v_sub_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x05, "v_subrev_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x06, "v_fmac_legacy_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x07, "v_mul_dx9_zero_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x08, "v_mul_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x09, "v_mul_i32_i24", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x0A, "v_mul_hi_i32_i24", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x0B, "v_mul_u32_u24", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x0C, "v_mul_hi_u32_u24", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x0D, "v_dot4c_i32_i8", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorDotProduct);
    add(Enc::VOP2, 0x0E, "v_dot8c_i32_i4", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorDotProduct);
    add(Enc::VOP2, 0x0F, "v_min_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x10, "v_max_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x11, "v_min_i32", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x12, "v_max_i32", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x13, "v_min_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x14, "v_max_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x16, "v_lshrrev_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x17, "v_ashrrev_i32", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x18, "v_lshlrev_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x1B, "v_and_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x1C, "v_or_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x1D, "v_xor_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x1E, "v_xnor_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP2, 0x1F, "v_mac_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x21, "v_madmk_f32", 3, true, false, false, false, false, false, true, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x22, "v_madak_f32", 3, true, false, false, false, false, false, true, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x25, "v_add_nc_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x26, "v_sub_nc_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x27, "v_subrev_nc_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x28, "v_add_co_ci_u32", 2, true, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x29, "v_sub_co_ci_u32", 2, true, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x2A, "v_subrev_co_ci_u32", 2, true, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP2, 0x2B, "v_fmac_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x2C, "v_fmamk_f32", 3, true, false, false, false, false, false, true, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x2D, "v_fmaak_f32", 3, true, false, false, false, false, false, true, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x2F, "v_cvt_pkrtz_f16_f32", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP2, 0x32, "v_add_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x33, "v_sub_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x34, "v_subrev_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x35, "v_mul_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x36, "v_fmac_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x37, "v_fmamk_f16", 3, true, false, false, false, false, false, true, 16, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x38, "v_fmaak_f16", 3, true, false, false, false, false, false, true, 16, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP2, 0x39, "v_max_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x3A, "v_min_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP2, 0x3B, "v_ldexp_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP2, 0x3C, "v_pk_fmac_f16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);

    // ==================== 9. VOPC (198 unique mnemonics — 100% GFX10.3!) ====================
    // Class compares
    add(Enc::VOPC, 0x88, "v_cmp_class_f32", 2, false, false, false, false, true, false, false, 32, DK::Float, SC::VectorCompare);
    add(Enc::VOPC, 0x98, "v_cmpx_class_f32", 2, false, false, false, false, false, true, false, 32, DK::Float, SC::VectorCompare);
    add(Enc::VOPC, 0xA8, "v_cmp_class_f64", 2, false, false, false, false, true, false, false, 64, DK::Float, SC::VectorCompare);
    add(Enc::VOPC, 0xB8, "v_cmpx_class_f64", 2, false, false, false, false, false, true, false, 64, DK::Float, SC::VectorCompare);
    add(Enc::VOPC, 0x8F, "v_cmp_class_f16", 2, false, false, false, false, true, false, false, 16, DK::Float, SC::VectorCompare);
    add(Enc::VOPC, 0x9F, "v_cmpx_class_f16", 2, false, false, false, false, false, true, false, 16, DK::Float, SC::VectorCompare);

    // F32 (0x10..0x1F / 0x30..0x3F)
    {
        static constexpr const char* kCmpF32[16] = {
            "v_cmp_f_f32", "v_cmp_lt_f32", "v_cmp_eq_f32", "v_cmp_le_f32",
            "v_cmp_gt_f32", "v_cmp_lg_f32", "v_cmp_ge_f32", "v_cmp_o_f32",
            "v_cmp_u_f32", "v_cmp_nge_f32", "v_cmp_nlg_f32", "v_cmp_ngt_f32",
            "v_cmp_nle_f32", "v_cmp_neq_f32", "v_cmp_nlt_f32", "v_cmp_tru_f32"};
        static constexpr const char* kCmpxF32[16] = {
            "v_cmpx_f_f32", "v_cmpx_lt_f32", "v_cmpx_eq_f32", "v_cmpx_le_f32",
            "v_cmpx_gt_f32", "v_cmpx_lg_f32", "v_cmpx_ge_f32", "v_cmpx_o_f32",
            "v_cmpx_u_f32", "v_cmpx_nge_f32", "v_cmpx_nlg_f32", "v_cmpx_ngt_f32",
            "v_cmpx_nle_f32", "v_cmpx_neq_f32", "v_cmpx_nlt_f32", "v_cmpx_tru_f32"};
        static constexpr const char* kCmpF64[16] = {
            "v_cmp_f_f64", "v_cmp_lt_f64", "v_cmp_eq_f64", "v_cmp_le_f64",
            "v_cmp_gt_f64", "v_cmp_lg_f64", "v_cmp_ge_f64", "v_cmp_o_f64",
            "v_cmp_u_f64", "v_cmp_nge_f64", "v_cmp_nlg_f64", "v_cmp_ngt_f64",
            "v_cmp_nle_f64", "v_cmp_neq_f64", "v_cmp_nlt_f64", "v_cmp_tru_f64"};
        static constexpr const char* kCmpxF64[16] = {
            "v_cmpx_f_f64", "v_cmpx_lt_f64", "v_cmpx_eq_f64", "v_cmpx_le_f64",
            "v_cmpx_gt_f64", "v_cmpx_lg_f64", "v_cmpx_ge_f64", "v_cmpx_o_f64",
            "v_cmpx_u_f64", "v_cmpx_nge_f64", "v_cmpx_nlg_f64", "v_cmpx_ngt_f64",
            "v_cmpx_nle_f64", "v_cmpx_neq_f64", "v_cmpx_nlt_f64", "v_cmpx_tru_f64"};
        static constexpr const char* kCmpF16[16] = {
            "v_cmp_f_f16", "v_cmp_lt_f16", "v_cmp_eq_f16", "v_cmp_le_f16",
            "v_cmp_gt_f16", "v_cmp_lg_f16", "v_cmp_ge_f16", "v_cmp_o_f16",
            "v_cmp_u_f16", "v_cmp_nge_f16", "v_cmp_nlg_f16", "v_cmp_ngt_f16",
            "v_cmp_nle_f16", "v_cmp_neq_f16", "v_cmp_nlt_f16", "v_cmp_tru_f16"};
        static constexpr const char* kCmpxF16[16] = {
            "v_cmpx_f_f16", "v_cmpx_lt_f16", "v_cmpx_eq_f16", "v_cmpx_le_f16",
            "v_cmpx_gt_f16", "v_cmpx_lg_f16", "v_cmpx_ge_f16", "v_cmpx_o_f16",
            "v_cmpx_u_f16", "v_cmpx_nge_f16", "v_cmpx_nlg_f16", "v_cmpx_ngt_f16",
            "v_cmpx_nle_f16", "v_cmpx_neq_f16", "v_cmpx_nlt_f16", "v_cmpx_tru_f16"};
        for (uint16_t i = 0u; i < 16u; ++i) {
            add(Enc::VOPC, static_cast<uint16_t>(0x10u + i), kCmpF32[i], 2, false, false, false, false, true, false, false, 32, DK::Float, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x30u + i), kCmpxF32[i], 2, false, false, false, false, false, true, false, 32, DK::Float, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x20u + i), kCmpF64[i], 2, false, false, false, false, true, false, false, 64, DK::Float, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x40u + i), kCmpxF64[i], 2, false, false, false, false, false, true, false, 64, DK::Float, SC::VectorCompare);
            // GFX10 split F16 compare ranges: [0xC8..0xCF]/[0xD8..0xDF] and [0xE8..0xEF]/[0xF8..0xFF]
            const uint16_t f16_cmp_op = (i < 8u) ? static_cast<uint16_t>(0xC8u + i) : static_cast<uint16_t>(0xD8u + (i - 8u));
            const uint16_t f16_cmpx_op = (i < 8u) ? static_cast<uint16_t>(0xE8u + i) : static_cast<uint16_t>(0xF8u + (i - 8u));
            add(Enc::VOPC, f16_cmp_op, kCmpF16[i], 2, false, false, false, false, true, false, false, 16, DK::Float, SC::VectorCompare);
            add(Enc::VOPC, f16_cmpx_op, kCmpxF16[i], 2, false, false, false, false, false, true, false, 16, DK::Float, SC::VectorCompare);
        }

        // Integer compares (i16, u16, i32, u32, i64, u64)
        static constexpr const char* kCmpI16[8] = {"v_cmp_f_i16", "v_cmp_lt_i16", "v_cmp_eq_i16", "v_cmp_le_i16", "v_cmp_gt_i16", "v_cmp_ne_i16", "v_cmp_ge_i16", "v_cmp_t_i16"};
        static constexpr const char* kCmpxI16[8] = {"v_cmpx_f_i16", "v_cmpx_lt_i16", "v_cmpx_eq_i16", "v_cmpx_le_i16", "v_cmpx_gt_i16", "v_cmpx_ne_i16", "v_cmpx_ge_i16", "v_cmpx_t_i16"};
        static constexpr const char* kCmpU16[8] = {"v_cmp_f_u16", "v_cmp_lt_u16", "v_cmp_eq_u16", "v_cmp_le_u16", "v_cmp_gt_u16", "v_cmp_ne_u16", "v_cmp_ge_u16", "v_cmp_t_u16"};
        static constexpr const char* kCmpxU16[8] = {"v_cmpx_f_u16", "v_cmpx_lt_u16", "v_cmpx_eq_u16", "v_cmpx_le_u16", "v_cmpx_gt_u16", "v_cmpx_ne_u16", "v_cmpx_ge_u16", "v_cmpx_t_u16"};
        static constexpr const char* kCmpI32[8] = {"v_cmp_f_i32", "v_cmp_lt_i32", "v_cmp_eq_i32", "v_cmp_le_i32", "v_cmp_gt_i32", "v_cmp_ne_i32", "v_cmp_ge_i32", "v_cmp_t_i32"};
        static constexpr const char* kCmpxI32[8] = {"v_cmpx_f_i32", "v_cmpx_lt_i32", "v_cmpx_eq_i32", "v_cmpx_le_i32", "v_cmpx_gt_i32", "v_cmpx_ne_i32", "v_cmpx_ge_i32", "v_cmpx_t_i32"};
        static constexpr const char* kCmpU32[8] = {"v_cmp_f_u32", "v_cmp_lt_u32", "v_cmp_eq_u32", "v_cmp_le_u32", "v_cmp_gt_u32", "v_cmp_ne_u32", "v_cmp_ge_u32", "v_cmp_t_u32"};
        static constexpr const char* kCmpxU32[8] = {"v_cmpx_f_u32", "v_cmpx_lt_u32", "v_cmpx_eq_u32", "v_cmpx_le_u32", "v_cmpx_gt_u32", "v_cmpx_ne_u32", "v_cmpx_ge_u32", "v_cmpx_t_u32"};
        static constexpr const char* kCmpI64[8] = {"v_cmp_f_i64", "v_cmp_lt_i64", "v_cmp_eq_i64", "v_cmp_le_i64", "v_cmp_gt_i64", "v_cmp_ne_i64", "v_cmp_ge_i64", "v_cmp_t_i64"};
        static constexpr const char* kCmpxI64[8] = {"v_cmpx_f_i64", "v_cmpx_lt_i64", "v_cmpx_eq_i64", "v_cmpx_le_i64", "v_cmpx_gt_i64", "v_cmpx_ne_i64", "v_cmpx_ge_i64", "v_cmpx_t_i64"};
        static constexpr const char* kCmpU64[8] = {"v_cmp_f_u64", "v_cmp_lt_u64", "v_cmp_eq_u64", "v_cmp_le_u64", "v_cmp_gt_u64", "v_cmp_ne_u64", "v_cmp_ge_u64", "v_cmp_t_u64"};
        static constexpr const char* kCmpxU64[8] = {"v_cmpx_f_u64", "v_cmpx_lt_u64", "v_cmpx_eq_u64", "v_cmpx_le_u64", "v_cmpx_gt_u64", "v_cmpx_ne_u64", "v_cmpx_ge_u64", "v_cmpx_t_u64"};

        for (uint16_t i = 0u; i < 8u; ++i) {
            add(Enc::VOPC, static_cast<uint16_t>(0x50u + i), kCmpI16[i], 2, false, false, false, false, true, false, false, 16, DK::SignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x70u + i), kCmpxI16[i], 2, false, false, false, false, false, true, false, 16, DK::SignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x60u + i), kCmpU16[i], 2, false, false, false, false, true, false, false, 16, DK::UnsignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x78u + i), kCmpxU16[i], 2, false, false, false, false, false, true, false, 16, DK::UnsignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x80u + i), kCmpI32[i], 2, false, false, false, false, true, false, false, 32, DK::SignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0x90u + i), kCmpxI32[i], 2, false, false, false, false, false, true, false, 32, DK::SignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0xA0u + i), kCmpI64[i], 2, false, false, false, false, true, false, false, 64, DK::SignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0xB0u + i), kCmpxI64[i], 2, false, false, false, false, false, true, false, 64, DK::SignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0xC0u + i), kCmpU32[i], 2, false, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0xD0u + i), kCmpxU32[i], 2, false, false, false, false, false, true, false, 32, DK::UnsignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0xE0u + i), kCmpU64[i], 2, false, false, false, false, true, false, false, 64, DK::UnsignedInt, SC::VectorCompare);
            add(Enc::VOPC, static_cast<uint16_t>(0xF0u + i), kCmpxU64[i], 2, false, false, false, false, false, true, false, 64, DK::UnsignedInt, SC::VectorCompare);
        }
    }

    // ==================== 10. VOP3 (97 unique native mnemonics) ====================
    add(Enc::VOP3, 0x140, "v_mad_legacy_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3, 0x141, "v_mad_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3, 0x142, "v_mad_i32_i24", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorFmaMad);
    add(Enc::VOP3, 0x143, "v_mad_u32_u24", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorFmaMad);
    add(Enc::VOP3, 0x144, "v_cubeid_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorSadCube);
    add(Enc::VOP3, 0x145, "v_cubesc_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorSadCube);
    add(Enc::VOP3, 0x146, "v_cubetc_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorSadCube);
    add(Enc::VOP3, 0x147, "v_cubema_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorSadCube);
    add(Enc::VOP3, 0x148, "v_bfe_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x149, "v_bfe_i32", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x14A, "v_bfi_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x14B, "v_fma_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3, 0x14C, "v_alignbyte_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x14D, "v_fma_f64", 3, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3, 0x14E, "v_lerp_u8", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x14F, "v_alignbit_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x150, "v_mullit_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x151, "v_min3_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x152, "v_min3_i32", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x153, "v_min3_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x154, "v_max3_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x155, "v_max3_i32", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x156, "v_max3_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x157, "v_med3_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x158, "v_med3_i32", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x159, "v_med3_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x15A, "v_sad_u8", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x15B, "v_sad_hi_u8", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x15C, "v_sad_u16", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x15D, "v_sad_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x15E, "v_cvt_pk_u8_f32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorConvert);
    add(Enc::VOP3, 0x15F, "v_div_fixup_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x160, "v_div_fixup_f64", 3, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x161, "v_ldexp_f32", 2, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x162, "v_div_fmas_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x163, "v_div_fmas_f64", 3, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x164, "v_msad_u8", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x165, "v_qsad_pk_u16_u8", 3, true, false, true, false, false, false, false, 64, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x166, "v_mqsad_pk_u16_u8", 3, true, false, true, false, false, false, false, 64, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x167, "v_trig_preop_f64", 2, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x168, "v_mqsad_u32_u8", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorSadCube);
    add(Enc::VOP3, 0x169, "v_mul_lo_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x16A, "v_mul_hi_u32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x16B, "v_mul_hi_i32", 2, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x16C, "v_mad_u64_u32", 3, true, false, true, false, true, false, false, 64, DK::UnsignedInt, SC::VectorFmaMad);
    add(Enc::VOP3, 0x16D, "v_mad_i64_i32", 3, true, false, true, false, true, false, false, 64, DK::SignedInt, SC::VectorFmaMad);
    add(Enc::VOP3, 0x16E, "v_div_scale_f32", 3, true, false, false, false, true, false, false, 32, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x16F, "v_div_scale_f64", 3, true, false, true, false, true, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x170, "v_add_f64", 2, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x171, "v_mul_f64", 2, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x172, "v_min_f64", 2, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x173, "v_max_f64", 2, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x174, "v_ldexp_f64", 2, true, false, true, false, false, false, false, 64, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x175, "v_lshlrev_b64", 2, true, false, true, false, false, false, false, 64, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x176, "v_lshrrev_b64", 2, true, false, true, false, false, false, false, 64, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x177, "v_ashrrev_i64", 2, true, false, true, false, false, false, false, 64, DK::SignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x178, "v_bcnt_u32_b32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x179, "v_fma_legacy_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    // VOP3-encoded f16 VINTRP ops (GFX10.3)
    add(Enc::VOP3, 0x274, "v_interp_p1ll_f16", 2, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorInterp);
    add(Enc::VOP3, 0x275, "v_interp_p1lv_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorInterp);
    add(Enc::VOP3, 0x276, "v_interp_p2_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorInterp);
    // VOP3 GFX10.3 16-bit / wave / 3-src ops (0x300..0x37F)
    add(Enc::VOP3, 0x303, "v_add_nc_u16", 2, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x304, "v_sub_nc_u16", 2, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x305, "v_mbcnt_lo_u32_b32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorCrossLane);
    add(Enc::VOP3, 0x306, "v_mbcnt_hi_u32_b32", 2, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorCrossLane);
    add(Enc::VOP3, 0x307, "v_add_nc_i16", 2, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x308, "v_sub_nc_i16", 2, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x309, "v_min_u16", 2, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x30A, "v_max_u16", 2, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x30B, "v_min_i16", 2, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x30C, "v_max_i16", 2, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x30D, "v_add_co_u32", 2, true, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x30E, "v_sub_co_u32", 2, true, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x30F, "v_subrev_co_u32", 2, true, false, false, false, true, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x311, "v_pack_b32_f16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3, 0x312, "v_mul_lo_u16", 2, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x313, "v_ashrrev_i16", 2, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x314, "v_lshlrev_b16", 2, true, false, false, false, false, false, false, 16, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x315, "v_lshrrev_b16", 2, true, false, false, false, false, false, false, 16, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x340, "v_mad_u16", 3, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorFmaMad);
    add(Enc::VOP3, 0x341, "v_mad_i16", 3, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorFmaMad);
    add(Enc::VOP3, 0x344, "v_perm_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x34B, "v_fma_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3, 0x34C, "v_div_fixup_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorTranscendental);
    add(Enc::VOP3, 0x34D, "v_min3_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x34E, "v_min3_i16", 3, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x34F, "v_min3_u16", 3, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x350, "v_max3_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x351, "v_max3_i16", 3, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x352, "v_max3_u16", 3, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x353, "v_med3_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFloatArith);
    add(Enc::VOP3, 0x354, "v_med3_i16", 3, true, false, false, false, false, false, false, 16, DK::SignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x355, "v_med3_u16", 3, true, false, false, false, false, false, false, 16, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x360, "v_readlane_b32", 2, true, true, false, false, false, false, false, 32, DK::Untyped, SC::VectorCrossLane);
    add(Enc::VOP3, 0x361, "v_writelane_b32", 2, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorCrossLane);
    add(Enc::VOP3, 0x368, "v_cvt_pknorm_i16_f32", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP3, 0x369, "v_cvt_pknorm_u16_f32", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP3, 0x36A, "v_cvt_pk_u16_u32", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP3, 0x36B, "v_cvt_pk_i16_i32", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP3, 0x36C, "v_cvt_pk_f16_f32", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorConvert);
    add(Enc::VOP3, 0x36D, "v_add3_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x36E, "v_lshl_or_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x36F, "v_and_or_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x370, "v_or3_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorLogicShiftBit);
    add(Enc::VOP3, 0x371, "v_xad_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x372, "v_lshl_add_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x373, "v_dot2_f32_f16", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorDotProduct);
    add(Enc::VOP3, 0x374, "v_add_lshl_u32", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorIntArith);
    add(Enc::VOP3, 0x377, "v_permlane16_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorCrossLane);
    add(Enc::VOP3, 0x378, "v_permlanex16_b32", 3, true, false, false, false, false, false, false, 32, DK::Untyped, SC::VectorCrossLane);

    // ==================== 11. VOP3P (28 unique mnemonics) ====================
    add(Enc::VOP3P, 0x00, "v_pk_mad_i16", 3, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x01, "v_pk_mul_lo_u16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x02, "v_pk_add_i16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x03, "v_pk_sub_i16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x04, "v_pk_lshlrev_b16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x05, "v_pk_lshrrev_b16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x06, "v_pk_ashrrev_i16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x07, "v_pk_max_i16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x08, "v_pk_min_i16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x09, "v_pk_mad_u16", 3, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x0A, "v_pk_add_u16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x0B, "v_pk_sub_u16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x0C, "v_pk_max_u16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x0D, "v_pk_min_u16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x0E, "v_pk_fma_f16", 3, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x0F, "v_pk_add_f16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x10, "v_pk_mul_f16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x11, "v_pk_min_f16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x12, "v_pk_max_f16", 2, true, false, false, false, false, false, false, 16, DK::Packed16, SC::VectorPacked16);
    add(Enc::VOP3P, 0x13, "v_dot2_i32_i16", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorDotProduct);
    add(Enc::VOP3P, 0x14, "v_dot2_u32_u16", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorDotProduct);
    add(Enc::VOP3P, 0x15, "v_dot4_i32_i8", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorDotProduct);
    add(Enc::VOP3P, 0x16, "v_dot4_u32_u8", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorDotProduct);
    add(Enc::VOP3P, 0x17, "v_dot8_i32_i4", 3, true, false, false, false, false, false, false, 32, DK::SignedInt, SC::VectorDotProduct);
    add(Enc::VOP3P, 0x18, "v_dot8_u32_u4", 3, true, false, false, false, false, false, false, 32, DK::UnsignedInt, SC::VectorDotProduct);
    add(Enc::VOP3P, 0x20, "v_fma_mix_f32", 3, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3P, 0x21, "v_fma_mixlo_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFmaMad);
    add(Enc::VOP3P, 0x22, "v_fma_mixhi_f16", 3, true, false, false, false, false, false, false, 16, DK::Float, SC::VectorFmaMad);

    // ==================== 12. VINTRP (3 unique 32-bit mnemonics; f16 forms in VOP3) ====================
    add(Enc::VINTRP, 0x00, "v_interp_p1_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorInterp);
    add(Enc::VINTRP, 0x01, "v_interp_p2_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorInterp);
    add(Enc::VINTRP, 0x02, "v_interp_mov_f32", 1, true, false, false, false, false, false, false, 32, DK::Float, SC::VectorInterp);

    return t;
}

struct SvOpcodeIndex {
    std::vector<ScalarVectorOpcodeEntry> table;
    std::unordered_map<uint32_t, const ScalarVectorOpcodeEntry*> by_enc_op;
    std::unordered_map<std::string, const ScalarVectorOpcodeEntry*> by_name;
    size_t unique_mnemonic_count = 0u;

    SvOpcodeIndex() {
        table = BuildScalarVectorOpcodeTable();
        std::unordered_set<std::string> uniq;
        uniq.reserve(table.size());
        for (const auto& e : table) {
            const uint32_t key = (static_cast<uint32_t>(e.encoding) << 16u) | static_cast<uint32_t>(e.raw_opcode);
            by_enc_op[key] = &e;
            by_name[e.mnemonic] = &e;
            uniq.insert(e.mnemonic);
        }
        // Preserve legacy GCN alias for GFX10.3 v_mul_dx9_zero_f32 (VOP2 0x07)
        if (auto it = by_name.find("v_mul_dx9_zero_f32"); it != by_name.end()) {
            by_name["v_mul_legacy_f32"] = it->second;
        }
        unique_mnemonic_count = uniq.size();
    }
};

const SvOpcodeIndex& GetIndex() noexcept {
    static const SvOpcodeIndex kIndex;
    return kIndex;
}

Rdna2Operand DecodeScalarDst(uint8_t sdst) noexcept {
    if (sdst == 106u) return Rdna2Operand::VccLo();
    if (sdst == 107u) return Rdna2Operand::VccHi();
    if (sdst == 124u) return Rdna2Operand::M0();
    if (sdst == 125u) return Rdna2Operand::Null();
    if (sdst == 126u) return Rdna2Operand::ExecLo();
    if (sdst == 127u) return Rdna2Operand::ExecHi();
    return Rdna2Operand::Sgpr(static_cast<uint8_t>(sdst & 0x7Fu));
}

bool EncodeSrcOperandSafe(
    const Rdna2Operand& op,
    uint16_t& out_code,
    uint32_t& out_lit,
    bool& out_needs_lit) noexcept {
    if (op.kind == Rdna2OperandKind::None) {
        out_code = 128u;  // inline 0
        out_lit = 0u;
        out_needs_lit = false;
        return true;
    }
    return EncodeRdna2SourceOperand(op, out_code, out_lit, out_needs_lit);
}

void DecodeSdwaWord(uint32_t w1, DecodedSvModifier& mod) noexcept {
    mod.kind = SvModifierKind::Sdwa;
    mod.raw_word = w1;
    mod.vsrc0 = static_cast<uint8_t>(w1 & 0xFFu);
    mod.dst_sel = static_cast<uint8_t>((w1 >> 8u) & 0x7u);
    mod.dst_unused = static_cast<uint8_t>((w1 >> 11u) & 0x3u);
    mod.sdwa_clamp = ((w1 >> 13u) & 1u) != 0u;
    mod.sdwa_omod = static_cast<uint8_t>((w1 >> 14u) & 0x3u);
    mod.src0_sel = static_cast<uint8_t>((w1 >> 16u) & 0x7u);
    mod.src0_sext = ((w1 >> 19u) & 1u) != 0u;
    mod.src0_neg = ((w1 >> 20u) & 1u) != 0u;
    mod.src0_abs = ((w1 >> 21u) & 1u) != 0u;
    mod.src0_is_sgpr = ((w1 >> 23u) & 1u) != 0u;
    mod.src1_sel = static_cast<uint8_t>((w1 >> 24u) & 0x7u);
    mod.src1_sext = ((w1 >> 27u) & 1u) != 0u;
    mod.src1_neg = ((w1 >> 28u) & 1u) != 0u;
    mod.src1_abs = ((w1 >> 29u) & 1u) != 0u;
    mod.src1_is_sgpr = ((w1 >> 31u) & 1u) != 0u;
}

uint32_t EncodeSdwaWord(const DecodedSvModifier& mod) noexcept {
    return static_cast<uint32_t>(mod.vsrc0) |
           (static_cast<uint32_t>(mod.dst_sel & 0x7u) << 8u) |
           (static_cast<uint32_t>(mod.dst_unused & 0x3u) << 11u) |
           (mod.sdwa_clamp ? (1u << 13u) : 0u) |
           (static_cast<uint32_t>(mod.sdwa_omod & 0x3u) << 14u) |
           (static_cast<uint32_t>(mod.src0_sel & 0x7u) << 16u) |
           (mod.src0_sext ? (1u << 19u) : 0u) |
           (mod.src0_neg ? (1u << 20u) : 0u) |
           (mod.src0_abs ? (1u << 21u) : 0u) |
           (mod.src0_is_sgpr ? (1u << 23u) : 0u) |
           (static_cast<uint32_t>(mod.src1_sel & 0x7u) << 24u) |
           (mod.src1_sext ? (1u << 27u) : 0u) |
           (mod.src1_neg ? (1u << 28u) : 0u) |
           (mod.src1_abs ? (1u << 29u) : 0u) |
           (mod.src1_is_sgpr ? (1u << 31u) : 0u);
}

void DecodeDpp16Word(uint32_t w1, DecodedSvModifier& mod) noexcept {
    mod.kind = SvModifierKind::Dpp16;
    mod.raw_word = w1;
    mod.vsrc0 = static_cast<uint8_t>(w1 & 0xFFu);
    mod.dpp_ctrl = static_cast<uint16_t>((w1 >> 8u) & 0x1FFu);
    mod.dpp_fi = ((w1 >> 18u) & 1u) != 0u;
    mod.bound_ctrl = ((w1 >> 19u) & 1u) != 0u;
    mod.src0_neg = ((w1 >> 20u) & 1u) != 0u;
    mod.src0_abs = ((w1 >> 21u) & 1u) != 0u;
    mod.src1_neg = ((w1 >> 22u) & 1u) != 0u;
    mod.src1_abs = ((w1 >> 23u) & 1u) != 0u;
    mod.bank_mask = static_cast<uint8_t>((w1 >> 24u) & 0xFu);
    mod.row_mask = static_cast<uint8_t>((w1 >> 28u) & 0xFu);
}

uint32_t EncodeDpp16Word(const DecodedSvModifier& mod) noexcept {
    return static_cast<uint32_t>(mod.vsrc0) |
           (static_cast<uint32_t>(mod.dpp_ctrl & 0x1FFu) << 8u) |
           (mod.dpp_fi ? (1u << 18u) : 0u) |
           (mod.bound_ctrl ? (1u << 19u) : 0u) |
           (mod.src0_neg ? (1u << 20u) : 0u) |
           (mod.src0_abs ? (1u << 21u) : 0u) |
           (mod.src1_neg ? (1u << 22u) : 0u) |
           (mod.src1_abs ? (1u << 23u) : 0u) |
           (static_cast<uint32_t>(mod.bank_mask & 0xFu) << 24u) |
           (static_cast<uint32_t>(mod.row_mask & 0xFu) << 28u);
}

void DecodeDpp8Word(uint32_t w1, bool fi, DecodedSvModifier& mod) noexcept {
    mod.kind = fi ? SvModifierKind::Dpp8Fi : SvModifierKind::Dpp8;
    mod.raw_word = w1;
    mod.vsrc0 = static_cast<uint8_t>(w1 & 0xFFu);
    mod.dpp_fi = fi;
    for (uint32_t i = 0u; i < 8u; ++i) {
        mod.dpp8_sel[i] = static_cast<uint8_t>((w1 >> (8u + i * 3u)) & 0x7u);
    }
}

uint32_t EncodeDpp8Word(const DecodedSvModifier& mod) noexcept {
    uint32_t w1 = static_cast<uint32_t>(mod.vsrc0);
    for (uint32_t i = 0u; i < 8u; ++i) {
        w1 |= (static_cast<uint32_t>(mod.dpp8_sel[i] & 0x7u) << (8u + i * 3u));
    }
    return w1;
}

bool IsModifierSrcCode(uint16_t src0) noexcept {
    return src0 == 249u || src0 == 250u || src0 == 233u || src0 == 234u;
}

uint16_t ModifierSrcCode(SvModifierKind k) noexcept {
    switch (k) {
        case SvModifierKind::Sdwa:   return 249u;
        case SvModifierKind::Dpp16:  return 250u;
        case SvModifierKind::Dpp8:   return 233u;
        case SvModifierKind::Dpp8Fi: return 234u;
        default:                     return 0u;
    }
}

bool IsVop3CarryOutOpcode(uint16_t raw_op) noexcept {
    return raw_op == 0x30Du || raw_op == 0x30Eu || raw_op == 0x30Fu ||
           raw_op == 0x16Cu || raw_op == 0x16Du ||
           raw_op == 0x16Eu || raw_op == 0x16Fu ||
           raw_op == 0x128u || raw_op == 0x129u || raw_op == 0x12Au;
}

}  // namespace

// ---------------------------------------------------------------------------
// Table & Lookup Public API
// ---------------------------------------------------------------------------
const std::vector<ScalarVectorOpcodeEntry>& GetScalarVectorOpcodeTable() noexcept {
    return GetIndex().table;
}

const ScalarVectorOpcodeEntry* LookupScalarVectorOpcode(
    Rdna2Encoding enc,
    uint16_t raw_opcode) noexcept {
    const auto& idx = GetIndex();
    const uint32_t key = (static_cast<uint32_t>(enc) << 16u) | static_cast<uint32_t>(raw_opcode);
    auto it = idx.by_enc_op.find(key);
    if (it != idx.by_enc_op.end()) {
        return it->second;
    }
    if (enc == Rdna2Encoding::VOP3) {
        if (raw_opcode < 0x100u) {
            return LookupScalarVectorOpcode(Rdna2Encoding::VOPC, raw_opcode);
        }
        if (raw_opcode >= 0x100u && raw_opcode < 0x140u) {
            return LookupScalarVectorOpcode(Rdna2Encoding::VOP2, static_cast<uint16_t>(raw_opcode - 0x100u));
        }
        if (raw_opcode >= 0x180u && raw_opcode < 0x200u) {
            return LookupScalarVectorOpcode(Rdna2Encoding::VOP1, static_cast<uint16_t>(raw_opcode - 0x180u));
        }
    }
    return nullptr;
}

const ScalarVectorOpcodeEntry* LookupScalarVectorByName(const char* mnemonic) noexcept {
    if (!mnemonic || mnemonic[0] == '\0') return nullptr;
    const auto& idx = GetIndex();
    auto it = idx.by_name.find(mnemonic);
    return (it != idx.by_name.end()) ? it->second : nullptr;
}

size_t GetUniqueScalarVectorMnemonicCount() noexcept {
    return GetIndex().unique_mnemonic_count;
}

// ---------------------------------------------------------------------------
// Binary Decoder
// ---------------------------------------------------------------------------
Status DecodeScalarVectorAt(
    const uint32_t* words,
    size_t word_count,
    size_t pc,
    SvDecodedInst& out_inst) {
    out_inst = SvDecodedInst{};
    if (!words || pc >= word_count) {
        return Status::Error(StatusCode::InvalidArgument, "DecodeScalarVectorAt: out-of-range pc");
    }

    auto& inst = out_inst.base;
    const uint32_t w0 = words[pc];
    inst.word_offset = static_cast<uint32_t>(pc);
    inst.raw_words[0] = w0;
    inst.word_count = 1u;

    auto consume = [&](uint32_t& out_w) -> Status {
        if (pc + inst.word_count >= word_count) {
            return Status::Error(StatusCode::ParseError, "Truncated multi-word RDNA2 instruction");
        }
        out_w = words[pc + inst.word_count];
        if (inst.word_count < 3u) {
            inst.raw_words[inst.word_count] = out_w;
        }
        ++inst.word_count;
        return Status::Ok();
    };

    auto decode_mod_or_lit = [&](uint16_t src0, uint32_t& out_lit) -> Status {
        out_lit = 0u;
        if (src0 == 255u) {
            return consume(out_lit);
        }
        if (src0 == 249u) {
            uint32_t mw = 0u;
            if (auto st = consume(mw); !st.ok()) return st;
            DecodeSdwaWord(mw, out_inst.modifier);
        } else if (src0 == 250u) {
            uint32_t mw = 0u;
            if (auto st = consume(mw); !st.ok()) return st;
            DecodeDpp16Word(mw, out_inst.modifier);
        } else if (src0 == 233u || src0 == 234u) {
            uint32_t mw = 0u;
            if (auto st = consume(mw); !st.ok()) return st;
            DecodeDpp8Word(mw, src0 == 234u, out_inst.modifier);
        }
        return Status::Ok();
    };

    if ((w0 >> 23u) == 0x17Du) {
        // SOP1
        inst.encoding = Rdna2Encoding::SOP1;
        const uint8_t sdst = static_cast<uint8_t>((w0 >> 16u) & 0x7Fu);
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 8u) & 0xFFu);
        const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
        uint32_t lit = 0u;
        if (ssrc0 == 255u) {
            if (auto st = consume(lit); !st.ok()) return st;
        }
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::SOP1, raw_op);
        if (!out_inst.entry || out_inst.entry->has_dst) {
            inst.dst = DecodeScalarDst(sdst);
        }
        if (!out_inst.entry || out_inst.entry->src_count > 0u) {
            inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
            inst.src_count = 1u;
        }
    } else if ((w0 >> 23u) == 0x17Eu) {
        // SOPC
        inst.encoding = Rdna2Encoding::SOPC;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16u) & 0x7Fu);
        const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8u) & 0xFFu);
        const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
        uint32_t lit = 0u;
        if (ssrc0 == 255u || ssrc1 == 255u) {
            if (auto st = consume(lit); !st.ok()) return st;
        }
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::SOPC, raw_op);
        inst.dst = Rdna2Operand::Scc();
        inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
        inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
        inst.src_count = 2u;
    } else if ((w0 >> 23u) == 0x17Fu) {
        // SOPP
        inst.encoding = Rdna2Encoding::SOPP;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16u) & 0x7Fu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::SOPP, raw_op);
        inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
    } else if ((w0 >> 28u) == 0xBu) {
        // SOPK
        inst.encoding = Rdna2Encoding::SOPK;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23u) & 0x1Fu);
        const uint8_t sdst = static_cast<uint8_t>((w0 >> 16u) & 0x7Fu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::SOPK, raw_op);
        inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        if (!out_inst.entry || out_inst.entry->has_dst ||
            (raw_op >= 0x03u && raw_op <= 0x0Eu) || raw_op == 0x13u || raw_op == 0x11u) {
            inst.dst = DecodeScalarDst(sdst);
        }
        if (out_inst.entry && out_inst.entry->has_mandatory_literal) {
            uint32_t lit = 0u;
            if (auto st = consume(lit); !st.ok()) return st;
            inst.src[0] = Rdna2Operand::LiteralU32(lit);
            inst.src_count = 1u;
        }
    } else if ((w0 >> 30u) == 0x2u) {
        // SOP2
        inst.encoding = Rdna2Encoding::SOP2;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23u) & 0x7Fu);
        const uint8_t sdst = static_cast<uint8_t>((w0 >> 16u) & 0x7Fu);
        const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8u) & 0xFFu);
        const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
        uint32_t lit = 0u;
        if (ssrc0 == 255u || ssrc1 == 255u) {
            if (auto st = consume(lit); !st.ok()) return st;
        }
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::SOP2, raw_op);
        if (!out_inst.entry || out_inst.entry->has_dst) {
            inst.dst = DecodeScalarDst(sdst);
        }
        inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
        inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
        inst.src_count = 2u;
    } else if ((w0 >> 25u) == 0x3Fu) {
        // VOP1
        inst.encoding = Rdna2Encoding::VOP1;
        const uint8_t vdst = static_cast<uint8_t>((w0 >> 17u) & 0xFFu);
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 9u) & 0xFFu);
        const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
        uint32_t lit = 0u;
        if (auto st = decode_mod_or_lit(src0, lit); !st.ok()) return st;
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::VOP1, raw_op);
        if (!out_inst.entry || out_inst.entry->has_dst) {
            if (out_inst.entry && out_inst.entry->dst_is_sgpr) {
                inst.dst = DecodeScalarDst(static_cast<uint8_t>(vdst & 0x7Fu));
            } else {
                inst.dst = Rdna2Operand::Vgpr(vdst);
            }
        }
        if (!out_inst.entry || out_inst.entry->src_count > 0u) {
            if (out_inst.modifier.kind != SvModifierKind::None) {
                inst.src[0] = (out_inst.modifier.kind == SvModifierKind::Sdwa && out_inst.modifier.src0_is_sgpr)
                                  ? DecodeRdna2SourceOperand(out_inst.modifier.vsrc0, 0u)
                                  : Rdna2Operand::Vgpr(out_inst.modifier.vsrc0);
            } else {
                inst.src[0] = DecodeRdna2SourceOperand(src0, lit);
            }
            inst.src_count = 1u;
        }
    } else if ((w0 >> 25u) == 0x3Eu) {
        // VOPC
        inst.encoding = Rdna2Encoding::VOPC;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 17u) & 0xFFu);
        const uint8_t vsrc1 = static_cast<uint8_t>((w0 >> 9u) & 0xFFu);
        const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
        uint32_t lit = 0u;
        if (auto st = decode_mod_or_lit(src0, lit); !st.ok()) return st;
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::VOPC, raw_op);
        const bool is_cmpx = out_inst.entry ? out_inst.entry->writes_exec : false;
        inst.dst = is_cmpx ? Rdna2Operand::ExecLo() : Rdna2Operand::VccLo();
        if (out_inst.modifier.kind != SvModifierKind::None) {
            inst.src[0] = (out_inst.modifier.kind == SvModifierKind::Sdwa && out_inst.modifier.src0_is_sgpr)
                              ? DecodeRdna2SourceOperand(out_inst.modifier.vsrc0, 0u)
                              : Rdna2Operand::Vgpr(out_inst.modifier.vsrc0);
            inst.src[1] = (out_inst.modifier.kind == SvModifierKind::Sdwa && out_inst.modifier.src1_is_sgpr)
                              ? DecodeRdna2SourceOperand(vsrc1, 0u)
                              : Rdna2Operand::Vgpr(vsrc1);
        } else {
            inst.src[0] = DecodeRdna2SourceOperand(src0, lit);
            inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
        }
        inst.src_count = 2u;
    } else if ((w0 >> 31u) == 0x0u) {
        // VOP2
        inst.encoding = Rdna2Encoding::VOP2;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 25u) & 0x3Fu);
        const uint8_t vdst = static_cast<uint8_t>((w0 >> 17u) & 0xFFu);
        const uint8_t vsrc1 = static_cast<uint8_t>((w0 >> 9u) & 0xFFu);
        const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::VOP2, raw_op);
        uint32_t lit = 0u;
        const bool mlit = out_inst.entry && out_inst.entry->has_mandatory_literal;
        if (mlit) {
            if (auto st = consume(lit); !st.ok()) return st;
        } else {
            if (auto st = decode_mod_or_lit(src0, lit); !st.ok()) return st;
        }
        inst.dst = Rdna2Operand::Vgpr(vdst);
        if (raw_op == 0x21u || raw_op == 0x2Cu || raw_op == 0x37u) {
            // *MK: D = S0 * K + VSRC1
            inst.src[0] = DecodeRdna2SourceOperand(src0, 0u);
            inst.src[1] = Rdna2Operand::LiteralU32(lit);
            inst.src[2] = Rdna2Operand::Vgpr(vsrc1);
            inst.src_count = 3u;
        } else if (raw_op == 0x22u || raw_op == 0x2Du || raw_op == 0x38u) {
            // *AK: D = S0 * VSRC1 + K
            inst.src[0] = DecodeRdna2SourceOperand(src0, 0u);
            inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
            inst.src[2] = Rdna2Operand::LiteralU32(lit);
            inst.src_count = 3u;
        } else {
            if (out_inst.modifier.kind != SvModifierKind::None) {
                inst.src[0] = (out_inst.modifier.kind == SvModifierKind::Sdwa && out_inst.modifier.src0_is_sgpr)
                                  ? DecodeRdna2SourceOperand(out_inst.modifier.vsrc0, 0u)
                                  : Rdna2Operand::Vgpr(out_inst.modifier.vsrc0);
                inst.src[1] = (out_inst.modifier.kind == SvModifierKind::Sdwa && out_inst.modifier.src1_is_sgpr)
                                  ? DecodeRdna2SourceOperand(vsrc1, 0u)
                                  : Rdna2Operand::Vgpr(vsrc1);
            } else {
                inst.src[0] = DecodeRdna2SourceOperand(src0, lit);
                inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
            }
            inst.src_count = 2u;
            if (raw_op == 0x01u) {
                inst.src[2] = Rdna2Operand::VccLo();
                inst.src_count = 3u;
            }
        }
    } else if ((w0 >> 26u) == 0x32u) {
        // VINTRP
        inst.encoding = Rdna2Encoding::VINTRP;
        const uint8_t vdst = static_cast<uint8_t>((w0 >> 18u) & 0xFFu);
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16u) & 0x3u);
        inst.vintrp_attr = static_cast<uint8_t>((w0 >> 10u) & 0x3Fu);
        inst.vintrp_chan = static_cast<uint8_t>((w0 >> 8u) & 0x3u);
        const uint8_t vsrc = static_cast<uint8_t>(w0 & 0xFFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::VINTRP, raw_op);
        inst.dst = Rdna2Operand::Vgpr(vdst);
        inst.src[0] = Rdna2Operand::Vgpr(vsrc);
        inst.src_count = 1u;
    } else if ((w0 >> 26u) == 0x33u) {
        // VOP3P
        inst.encoding = Rdna2Encoding::VOP3P;
        uint32_t w1 = 0u;
        if (auto st = consume(w1); !st.ok()) return st;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16u) & 0x7Fu);
        inst.clamp = ((w0 >> 15u) & 1u) != 0u;
        const uint8_t op_sel_hi_2 = static_cast<uint8_t>((w0 >> 14u) & 1u);
        inst.vop3p_op_sel = static_cast<uint8_t>((w0 >> 11u) & 0x7u);
        inst.vop3p_neg_hi = static_cast<uint8_t>((w0 >> 8u) & 0x7u);
        const uint8_t vdst = static_cast<uint8_t>(w0 & 0xFFu);
        inst.vop3p_neg_lo = static_cast<uint8_t>((w1 >> 29u) & 0x7u);
        const uint8_t op_sel_hi_10 = static_cast<uint8_t>((w1 >> 27u) & 0x3u);
        inst.vop3p_op_sel_hi = static_cast<uint8_t>(op_sel_hi_10 | (op_sel_hi_2 << 2u));
        const uint16_t src2 = static_cast<uint16_t>((w1 >> 18u) & 0x1FFu);
        const uint16_t src1 = static_cast<uint16_t>((w1 >> 9u) & 0x1FFu);
        const uint16_t src0 = static_cast<uint16_t>(w1 & 0x1FFu);
        uint32_t lit = 0u;
        if (src0 == 255u || src1 == 255u || src2 == 255u) {
            if (auto st = consume(lit); !st.ok()) return st;
        }
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::VOP3P, raw_op);
        const uint8_t sc = out_inst.entry ? out_inst.entry->src_count : 2u;
        inst.src_count = sc;
        inst.dst = Rdna2Operand::Vgpr(vdst);
        const uint16_t raw_srcs[3] = {src0, src1, src2};
        for (uint8_t i = 0u; i < sc && i < 3u; ++i) {
            inst.src[i] = DecodeRdna2SourceOperand(raw_srcs[i], lit);
        }
    } else if (IsVop3EncodingWord(w0)) {
        // VOP3 (64-bit, or 96-bit with literal)
        inst.encoding = Rdna2Encoding::VOP3;
        uint32_t w1 = 0u;
        if (auto st = consume(w1); !st.ok()) return st;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16u) & 0x3FFu);
        const bool is_vop3b = IsVop3CarryOutOpcode(raw_op);
        inst.clamp = ((w0 >> 15u) & 1u) != 0u;
        inst.vop3_op_sel = is_vop3b ? 0u : static_cast<uint8_t>((w0 >> 11u) & 0xFu);
        const uint8_t abs_bits = is_vop3b ? 0u : static_cast<uint8_t>((w0 >> 8u) & 0x7u);
        const uint8_t vop3b_sdst = is_vop3b ? static_cast<uint8_t>((w0 >> 8u) & 0x7Fu) : 106u;
        const uint8_t vdst = static_cast<uint8_t>(w0 & 0xFFu);
        const uint8_t neg_bits = static_cast<uint8_t>((w1 >> 29u) & 0x7u);
        inst.omod = static_cast<uint8_t>((w1 >> 27u) & 0x3u);
        const uint16_t src2 = static_cast<uint16_t>((w1 >> 18u) & 0x1FFu);
        const uint16_t src1 = static_cast<uint16_t>((w1 >> 9u) & 0x1FFu);
        const uint16_t src0 = static_cast<uint16_t>(w1 & 0x1FFu);
        uint32_t lit = 0u;
        if (src0 == 255u || src1 == 255u || src2 == 255u) {
            if (auto st = consume(lit); !st.ok()) return st;
        }
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::VOP3, raw_op);
        uint8_t sc = out_inst.entry ? out_inst.entry->src_count : 3u;
        if (raw_op == 0x101u) sc = 3u;  // v_cndmask_b32 in VOP3 has 3 explicit sources
        inst.src_count = sc;
        if (out_inst.entry && out_inst.entry->writes_exec) {
            inst.dst = Rdna2Operand::ExecLo();
        } else if (raw_op < 0x100u || (out_inst.entry && out_inst.entry->dst_is_sgpr)) {
            inst.dst = DecodeScalarDst(static_cast<uint8_t>(vdst & 0x7Fu));
        } else {
            inst.dst = Rdna2Operand::Vgpr(vdst);
        }
        if (is_vop3b) {
            inst.soffset = vop3b_sdst;  // store VOP3b carry-out sdst in soffset
        }
        const uint16_t raw_srcs[3] = {src0, src1, src2};
        for (uint8_t i = 0u; i < sc && i < 3u; ++i) {
            inst.src[i] = DecodeRdna2SourceOperand(raw_srcs[i], lit);
            inst.src[i].abs = ((abs_bits >> i) & 1u) != 0u;
            inst.src[i].neg = ((neg_bits >> i) & 1u) != 0u;
        }
    } else if ((w0 >> 26u) == 0x3Du) {
        // SMEM (64-bit)
        inst.encoding = Rdna2Encoding::SMEM;
        uint32_t w1 = 0u;
        if (auto st = consume(w1); !st.ok()) return st;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18u) & 0xFFu);
        inst.dlc = ((w0 >> 17u) & 1u) != 0u;
        inst.glc = ((w0 >> 16u) & 1u) != 0u;
        const uint8_t sdata = static_cast<uint8_t>((w0 >> 6u) & 0x7Fu);
        inst.sbase = static_cast<uint8_t>(w0 & 0x3Fu);
        inst.soffset = static_cast<uint8_t>((w1 >> 25u) & 0x7Fu);
        inst.mem_offset = SignExtend32(w1 & 0x1FFFFFu, 21u);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupScalarVectorOpcode(Rdna2Encoding::SMEM, raw_op);
        if (!out_inst.entry || out_inst.entry->has_dst) {
            inst.dst = DecodeScalarDst(sdata);
        }
        if (!out_inst.entry || out_inst.entry->src_count > 0u) {
            inst.src[0] = Rdna2Operand::Sgpr(static_cast<uint8_t>(inst.sbase * 2u));
            inst.src_count = 1u;
        }
    } else {
        // Non-SV shader instructions (MUBUF / MTBUF / MIMG / DS / FLAT / EXP) are all 64-bit (2 words).
        const size_t avail = std::min<size_t>(word_count - pc, 2u);
        std::vector<uint32_t> slice(words + pc, words + pc + avail);
        std::vector<Rdna2Instruction> tmp;
        if (auto st = DisassembleRdna2(slice, tmp); st.ok() && !tmp.empty()) {
            inst = tmp[0];
            inst.word_offset = static_cast<uint32_t>(pc);
            out_inst.entry = nullptr;
            return Status::Ok();
        }
        return Status::Error(StatusCode::ParseError, "Unknown or unsupported RDNA2 instruction word");
    }

    if (out_inst.entry) {
        inst.mnemonic = out_inst.entry->mnemonic;
        inst.data_kind = out_inst.entry->data_kind;
        if (out_inst.entry->bit_width == 16u) inst.operand_width = Rdna2OperandWidth::Bits16;
        else if (out_inst.entry->bit_width == 64u) inst.operand_width = Rdna2OperandWidth::Bits64;
        else inst.operand_width = Rdna2OperandWidth::Bits32;
        inst.disasm = inst.mnemonic;
    } else {
        inst.mnemonic = "unknown";
        inst.disasm = "unknown";
    }
    return Status::Ok();
}

Status DecodeScalarVectorStream(
    const std::vector<uint32_t>& words,
    std::vector<SvDecodedInst>& out_insts) {
    out_insts.clear();
    if (words.empty()) {
        return Status::Error(StatusCode::ParseError, "Empty RDNA2 instruction stream");
    }
    size_t pc = 0u;
    while (pc < words.size()) {
        SvDecodedInst decoded{};
        if (auto st = DecodeScalarVectorAt(words.data(), words.size(), pc, decoded); !st.ok()) {
            return st;
        }
        const uint32_t step = decoded.base.word_count ? decoded.base.word_count : 1u;
        out_insts.push_back(std::move(decoded));
        pc += step;
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Binary Encoder
// ---------------------------------------------------------------------------
bool EncodeScalarVectorInst(
    const char* mnemonic,
    uint8_t dst,
    const std::vector<Rdna2Operand>& srcs,
    int32_t imm_or_offset,
    std::vector<uint32_t>& out_words,
    const DecodedSvModifier* mod) {
    const ScalarVectorOpcodeEntry* e = LookupScalarVectorByName(mnemonic);
    if (!e) return false;

    auto get_src = [&](size_t idx) -> Rdna2Operand {
        return (idx < srcs.size()) ? srcs[idx] : Rdna2Operand::InlineInt(0);
    };

    const bool has_mod = (mod != nullptr && mod->kind != SvModifierKind::None);

    switch (e->encoding) {
        case Rdna2Encoding::SOP2: {
            uint16_t c0 = 0u, c1 = 0u;
            uint32_t lit0 = 0u, lit1 = 0u;
            bool n0 = false, n1 = false;
            if (!EncodeSrcOperandSafe(get_src(0u), c0, lit0, n0) ||
                !EncodeSrcOperandSafe(get_src(1u), c1, lit1, n1)) {
                return false;
            }
            if (c0 > 255u || c1 > 255u) return false;
            const uint32_t w0 = (0x2u << 30u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x7Fu) << 23u) |
                                (static_cast<uint32_t>(dst & 0x7Fu) << 16u) |
                                (static_cast<uint32_t>(c1 & 0xFFu) << 8u) |
                                static_cast<uint32_t>(c0 & 0xFFu);
            out_words.push_back(w0);
            if (n0 || n1) out_words.push_back(n0 ? lit0 : lit1);
            return true;
        }
        case Rdna2Encoding::SOP1: {
            uint16_t c0 = 0u;
            uint32_t lit0 = 0u;
            bool n0 = false;
            if (e->src_count > 0u) {
                if (!EncodeSrcOperandSafe(get_src(0u), c0, lit0, n0) || c0 > 255u) return false;
            }
            const uint32_t w0 = (0x17Du << 23u) |
                                (static_cast<uint32_t>(dst & 0x7Fu) << 16u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0xFFu) << 8u) |
                                static_cast<uint32_t>(c0 & 0xFFu);
            out_words.push_back(w0);
            if (n0) out_words.push_back(lit0);
            return true;
        }
        case Rdna2Encoding::SOPC: {
            uint16_t c0 = 0u, c1 = 0u;
            uint32_t lit0 = 0u, lit1 = 0u;
            bool n0 = false, n1 = false;
            if (!EncodeSrcOperandSafe(get_src(0u), c0, lit0, n0) ||
                !EncodeSrcOperandSafe(get_src(1u), c1, lit1, n1)) {
                return false;
            }
            if (c0 > 255u || c1 > 255u) return false;
            const uint32_t w0 = (0x17Eu << 23u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x7Fu) << 16u) |
                                (static_cast<uint32_t>(c1 & 0xFFu) << 8u) |
                                static_cast<uint32_t>(c0 & 0xFFu);
            out_words.push_back(w0);
            if (n0 || n1) out_words.push_back(n0 ? lit0 : lit1);
            return true;
        }
        case Rdna2Encoding::SOPK: {
            const uint32_t w0 = (0xBu << 28u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x1Fu) << 23u) |
                                (static_cast<uint32_t>(dst & 0x7Fu) << 16u) |
                                static_cast<uint32_t>(static_cast<uint16_t>(imm_or_offset));
            out_words.push_back(w0);
            if (e->has_mandatory_literal) {
                const uint32_t lit = !srcs.empty() ? srcs[0].literal_bits : static_cast<uint32_t>(imm_or_offset);
                out_words.push_back(lit);
            }
            return true;
        }
        case Rdna2Encoding::SOPP: {
            const uint32_t w0 = (0x17Fu << 23u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x7Fu) << 16u) |
                                static_cast<uint32_t>(static_cast<uint16_t>(imm_or_offset));
            out_words.push_back(w0);
            return true;
        }
        case Rdna2Encoding::SMEM: {
            const uint8_t sbase = (!srcs.empty() && srcs[0].kind == Rdna2OperandKind::Sgpr)
                                      ? static_cast<uint8_t>((srcs[0].reg_index / 2u) & 0x3Fu)
                                      : 0u;
            const uint8_t soffset = (srcs.size() >= 2u && srcs[1].kind == Rdna2OperandKind::Sgpr)
                                        ? static_cast<uint8_t>(srcs[1].reg_index & 0x7Fu)
                                        : 0x7Du;
            const uint32_t off21 = static_cast<uint32_t>(imm_or_offset) & 0x1FFFFFu;
            const uint32_t w0 = (0x3Du << 26u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0xFFu) << 18u) |
                                (static_cast<uint32_t>(dst & 0x7Fu) << 6u) |
                                static_cast<uint32_t>(sbase & 0x3Fu);
            const uint32_t w1 = (static_cast<uint32_t>(soffset & 0x7Fu) << 25u) | off21;
            out_words.push_back(w0);
            out_words.push_back(w1);
            return true;
        }
        case Rdna2Encoding::VOP1: {
            uint16_t c0 = 0u;
            uint32_t lit0 = 0u;
            bool n0 = false;
            if (has_mod) {
                c0 = ModifierSrcCode(mod->kind);
            } else if (e->src_count > 0u) {
                if (!EncodeSrcOperandSafe(get_src(0u), c0, lit0, n0)) return false;
            }
            const uint32_t w0 = (0x3Fu << 25u) |
                                (static_cast<uint32_t>(dst) << 17u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0xFFu) << 9u) |
                                static_cast<uint32_t>(c0 & 0x1FFu);
            out_words.push_back(w0);
            if (has_mod) {
                if (mod->kind == SvModifierKind::Sdwa) out_words.push_back(EncodeSdwaWord(*mod));
                else if (mod->kind == SvModifierKind::Dpp16) out_words.push_back(EncodeDpp16Word(*mod));
                else out_words.push_back(EncodeDpp8Word(*mod));
            } else if (n0) {
                out_words.push_back(lit0);
            }
            return true;
        }
        case Rdna2Encoding::VOPC: {
            uint16_t c0 = 0u;
            uint32_t lit0 = 0u;
            bool n0 = false;
            if (has_mod) {
                c0 = ModifierSrcCode(mod->kind);
            } else {
                if (!EncodeSrcOperandSafe(get_src(0u), c0, lit0, n0)) return false;
            }
            const Rdna2Operand s1 = get_src(1u);
            const uint8_t vsrc1 = static_cast<uint8_t>(s1.reg_index & 0xFFu);
            const uint32_t w0 = (0x3Eu << 25u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0xFFu) << 17u) |
                                (static_cast<uint32_t>(vsrc1) << 9u) |
                                static_cast<uint32_t>(c0 & 0x1FFu);
            out_words.push_back(w0);
            if (has_mod) {
                if (mod->kind == SvModifierKind::Sdwa) out_words.push_back(EncodeSdwaWord(*mod));
                else if (mod->kind == SvModifierKind::Dpp16) out_words.push_back(EncodeDpp16Word(*mod));
                else out_words.push_back(EncodeDpp8Word(*mod));
            } else if (n0) {
                out_words.push_back(lit0);
            }
            return true;
        }
        case Rdna2Encoding::VOP2: {
            uint16_t c0 = 0u;
            uint32_t lit0 = 0u;
            bool n0 = false;
            if (has_mod && !e->has_mandatory_literal) {
                c0 = ModifierSrcCode(mod->kind);
            } else {
                if (!EncodeSrcOperandSafe(get_src(0u), c0, lit0, n0)) return false;
            }
            uint8_t vsrc1 = 0u;
            uint32_t mlit_word = static_cast<uint32_t>(imm_or_offset);
            if (e->has_mandatory_literal) {
                if (e->raw_opcode == 0x21u || e->raw_opcode == 0x2Cu || e->raw_opcode == 0x37u) {
                    // *MK: src0, K, vsrc1
                    mlit_word = (srcs.size() >= 2u) ? srcs[1].literal_bits : static_cast<uint32_t>(imm_or_offset);
                    vsrc1 = (srcs.size() >= 3u) ? static_cast<uint8_t>(srcs[2].reg_index & 0xFFu) : 0u;
                } else {
                    // *AK: src0, vsrc1, K
                    vsrc1 = (srcs.size() >= 2u) ? static_cast<uint8_t>(srcs[1].reg_index & 0xFFu) : 0u;
                    mlit_word = (srcs.size() >= 3u) ? srcs[2].literal_bits : static_cast<uint32_t>(imm_or_offset);
                }
            } else {
                vsrc1 = static_cast<uint8_t>(get_src(1u).reg_index & 0xFFu);
            }
            const uint32_t w0 = (static_cast<uint32_t>(e->raw_opcode & 0x3Fu) << 25u) |
                                (static_cast<uint32_t>(dst) << 17u) |
                                (static_cast<uint32_t>(vsrc1) << 9u) |
                                static_cast<uint32_t>(c0 & 0x1FFu);
            out_words.push_back(w0);
            if (e->has_mandatory_literal) {
                out_words.push_back(mlit_word);
            } else if (has_mod) {
                if (mod->kind == SvModifierKind::Sdwa) out_words.push_back(EncodeSdwaWord(*mod));
                else if (mod->kind == SvModifierKind::Dpp16) out_words.push_back(EncodeDpp16Word(*mod));
                else out_words.push_back(EncodeDpp8Word(*mod));
            } else if (n0) {
                out_words.push_back(lit0);
            }
            return true;
        }
        case Rdna2Encoding::VOP3: {
            uint16_t c0 = 128u, c1 = 128u, c2 = 128u;
            uint32_t l0 = 0u, l1 = 0u, l2 = 0u;
            bool n0 = false, n1 = false, n2 = false;
            const Rdna2Operand s0 = get_src(0u);
            const Rdna2Operand s1 = get_src(1u);
            const Rdna2Operand s2 = get_src(2u);
            if (!EncodeSrcOperandSafe(s0, c0, l0, n0) ||
                !EncodeSrcOperandSafe(s1, c1, l1, n1) ||
                !EncodeSrcOperandSafe(s2, c2, l2, n2)) {
                return false;
            }
            const bool is_vop3b = IsVop3CarryOutOpcode(e->raw_opcode);
            const uint8_t abs_bits = (s0.abs ? 1u : 0u) | (s1.abs ? 2u : 0u) | (s2.abs ? 4u : 0u);
            const uint8_t neg_bits = (s0.neg ? 1u : 0u) | (s1.neg ? 2u : 0u) | (s2.neg ? 4u : 0u);
            const uint8_t op_sel = static_cast<uint8_t>((imm_or_offset >> 4) & 0xFu);
            const bool clamp = (imm_or_offset & 1) != 0;
            const uint8_t vop3b_sdst = is_vop3b ? static_cast<uint8_t>(imm_or_offset ? (imm_or_offset & 0x7F) : 106) : 0u;
            const uint32_t w0 = (0x35u << 26u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x3FFu) << 16u) |
                                (clamp ? (1u << 15u) : 0u) |
                                (is_vop3b ? (static_cast<uint32_t>(vop3b_sdst & 0x7Fu) << 8u)
                                          : ((static_cast<uint32_t>(op_sel & 0xFu) << 11u) |
                                             (static_cast<uint32_t>(abs_bits & 0x7u) << 8u))) |
                                static_cast<uint32_t>(dst);
            const uint32_t w1 = (static_cast<uint32_t>(neg_bits & 0x7u) << 29u) |
                                (static_cast<uint32_t>(c2 & 0x1FFu) << 18u) |
                                (static_cast<uint32_t>(c1 & 0x1FFu) << 9u) |
                                static_cast<uint32_t>(c0 & 0x1FFu);
            out_words.push_back(w0);
            out_words.push_back(w1);
            if (n0 || n1 || n2) {
                out_words.push_back(n0 ? l0 : (n1 ? l1 : l2));
            }
            return true;
        }
        case Rdna2Encoding::VOP3P: {
            uint16_t c0 = 128u, c1 = 128u, c2 = 128u;
            uint32_t l0 = 0u, l1 = 0u, l2 = 0u;
            bool n0 = false, n1 = false, n2 = false;
            if (!EncodeSrcOperandSafe(get_src(0u), c0, l0, n0) ||
                !EncodeSrcOperandSafe(get_src(1u), c1, l1, n1) ||
                !EncodeSrcOperandSafe(get_src(2u), c2, l2, n2)) {
                return false;
            }
            const uint8_t op_sel = static_cast<uint8_t>(imm_or_offset & 0x7);
            const uint8_t op_sel_hi = (imm_or_offset == 0) ? 7u : static_cast<uint8_t>((imm_or_offset >> 4) & 0x7);
            const uint8_t neg_lo = static_cast<uint8_t>((imm_or_offset >> 8) & 0x7);
            const uint8_t neg_hi = static_cast<uint8_t>((imm_or_offset >> 12) & 0x7);
            const bool clamp = ((imm_or_offset >> 16) & 1) != 0;
            const uint32_t w0 = (0x33u << 26u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x7Fu) << 16u) |
                                (clamp ? (1u << 15u) : 0u) |
                                (static_cast<uint32_t>((op_sel_hi >> 2u) & 1u) << 14u) |
                                (static_cast<uint32_t>(op_sel & 0x7u) << 11u) |
                                (static_cast<uint32_t>(neg_hi & 0x7u) << 8u) |
                                static_cast<uint32_t>(dst);
            const uint32_t w1 = (static_cast<uint32_t>(neg_lo & 0x7u) << 29u) |
                                (static_cast<uint32_t>(op_sel_hi & 0x3u) << 27u) |
                                (static_cast<uint32_t>(c2 & 0x1FFu) << 18u) |
                                (static_cast<uint32_t>(c1 & 0x1FFu) << 9u) |
                                static_cast<uint32_t>(c0 & 0x1FFu);
            out_words.push_back(w0);
            out_words.push_back(w1);
            if (n0 || n1 || n2) {
                out_words.push_back(n0 ? l0 : (n1 ? l1 : l2));
            }
            return true;
        }
        case Rdna2Encoding::VINTRP: {
            const uint8_t vsrc = static_cast<uint8_t>(get_src(0u).reg_index & 0xFFu);
            const uint8_t attr = static_cast<uint8_t>((imm_or_offset >> 2) & 0x3F);
            const uint8_t chan = static_cast<uint8_t>(imm_or_offset & 3);
            const uint32_t w0 = (0x32u << 26u) |
                                (static_cast<uint32_t>(dst) << 18u) |
                                (static_cast<uint32_t>(e->raw_opcode & 0x3u) << 16u) |
                                (static_cast<uint32_t>(attr) << 10u) |
                                (static_cast<uint32_t>(chan) << 8u) |
                                static_cast<uint32_t>(vsrc);
            out_words.push_back(w0);
            return true;
        }
        default:
            return false;
    }
}

namespace {

// ---------------------------------------------------------------------------
// CPU Reference Executor Helpers
// ---------------------------------------------------------------------------
inline constexpr float kTwoPiF32 = 6.2831853071795864769f;

uint32_t ReadScalarOp32(const SvWaveState& w, const Rdna2Operand& op) noexcept {
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:        return w.ReadSgpr(static_cast<uint8_t>(op.reg_index));
        case Rdna2OperandKind::InlineInt:   return static_cast<uint32_t>(op.int_value);
        case Rdna2OperandKind::InlineFloat: return FloatToBits(op.float_value);
        case Rdna2OperandKind::Literal32:   return op.literal_bits;
        case Rdna2OperandKind::VccLo:       return static_cast<uint32_t>(w.vcc & 0xFFFFFFFFull);
        case Rdna2OperandKind::VccHi:       return static_cast<uint32_t>(w.vcc >> 32u);
        case Rdna2OperandKind::ExecLo:      return static_cast<uint32_t>(w.exec & 0xFFFFFFFFull);
        case Rdna2OperandKind::ExecHi:      return static_cast<uint32_t>(w.exec >> 32u);
        case Rdna2OperandKind::Scc:         return w.scc ? 1u : 0u;
        case Rdna2OperandKind::M0:          return w.m0;
        default:                            return 0u;
    }
}

uint64_t ReadScalarOp64(const SvWaveState& w, const Rdna2Operand& op) noexcept {
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:        return w.ReadSgpr64(static_cast<uint8_t>(op.reg_index));
        case Rdna2OperandKind::InlineInt:   return static_cast<uint64_t>(static_cast<int64_t>(op.int_value));
        case Rdna2OperandKind::InlineFloat: return DoubleToBits(static_cast<double>(op.float_value));
        case Rdna2OperandKind::Literal32:   return static_cast<uint64_t>(op.literal_bits);
        case Rdna2OperandKind::VccLo:       return w.vcc;
        case Rdna2OperandKind::ExecLo:      return w.exec;
        case Rdna2OperandKind::Scc:         return w.scc ? 1ull : 0ull;
        case Rdna2OperandKind::M0:          return static_cast<uint64_t>(w.m0);
        default:                            return 0ull;
    }
}

void WriteScalarDst32(SvWaveState& w, const Rdna2Operand& dst, uint32_t val) noexcept {
    switch (dst.kind) {
        case Rdna2OperandKind::Sgpr:   w.WriteSgpr(static_cast<uint8_t>(dst.reg_index), val); break;
        case Rdna2OperandKind::VccLo:  w.WriteSgpr(106u, val); break;
        case Rdna2OperandKind::VccHi:  w.WriteSgpr(107u, val); break;
        case Rdna2OperandKind::M0:     w.m0 = val; break;
        case Rdna2OperandKind::ExecLo: w.WriteSgpr(126u, val); break;
        case Rdna2OperandKind::ExecHi: w.WriteSgpr(127u, val); break;
        default: break;
    }
}

void WriteScalarDst64(SvWaveState& w, const Rdna2Operand& dst, uint64_t val) noexcept {
    switch (dst.kind) {
        case Rdna2OperandKind::Sgpr:   w.WriteSgpr64(static_cast<uint8_t>(dst.reg_index), val); break;
        case Rdna2OperandKind::VccLo:  w.vcc = val; break;
        case Rdna2OperandKind::ExecLo: w.exec = val; break;
        default: break;
    }
}

uint32_t ReadVecOp32(const SvWaveState& w, uint32_t lane, const Rdna2Operand& op, bool apply_fmod = false) noexcept {
    uint32_t v = 0u;
    if (op.kind == Rdna2OperandKind::Vgpr) {
        v = w.ReadVgpr(lane, static_cast<uint8_t>(op.reg_index));
    } else {
        v = ReadScalarOp32(w, op);
    }
    if (apply_fmod) {
        if (op.abs) v &= 0x7FFFFFFFu;
        if (op.neg) v ^= 0x80000000u;
    }
    return v;
}

uint64_t ReadVecOp64(const SvWaveState& w, uint32_t lane, const Rdna2Operand& op, bool apply_fmod = false) noexcept {
    uint64_t v = 0ull;
    if (op.kind == Rdna2OperandKind::Vgpr) {
        v = w.ReadVgpr64(lane, static_cast<uint8_t>(op.reg_index));
    } else {
        v = ReadScalarOp64(w, op);
    }
    if (apply_fmod) {
        if (op.abs) v &= 0x7FFFFFFFFFFFFFFFull;
        if (op.neg) v ^= 0x8000000000000000ull;
    }
    return v;
}

float ApplyFloatOutMod(float f, bool clamp, uint8_t omod) noexcept {
    if (omod == 1u) f *= 2.0f;
    else if (omod == 2u) f *= 4.0f;
    else if (omod == 3u) f *= 0.5f;
    if (clamp) {
        if (std::isnan(f) || f < 0.0f) f = 0.0f;
        else if (f > 1.0f) f = 1.0f;
    }
    return f;
}

// GFX10.3 V_SIN_F32 / V_COS_F32 / V_SIN_F16 / V_COS_F16 turns/cycles semantics:
// Input is in turns (1.0 = 2*pi radians), valid hardware range [-256, 256].
// Fractional turns are reduced BEFORE multiplying by 2*pi; NaN/Inf -> QNaN; |x| >= 256 -> 0 (sin) / 1 (cos).
float EvalSinTurnsF32(float x) noexcept {
    if (std::isnan(x) || std::isinf(x)) return std::numeric_limits<float>::quiet_NaN();
    if (std::fabs(x) >= 256.0f) return 0.0f;
    const float frac = x - std::floor(x);
    if (frac == 0.0f || frac == 0.5f) return 0.0f;
    if (frac == 0.25f) return 1.0f;
    if (frac == 0.75f) return -1.0f;
    return std::sin(frac * kTwoPiF32);
}

float EvalCosTurnsF32(float x) noexcept {
    if (std::isnan(x) || std::isinf(x)) return std::numeric_limits<float>::quiet_NaN();
    if (std::fabs(x) >= 256.0f) return 1.0f;
    const float frac = x - std::floor(x);
    if (frac == 0.0f) return 1.0f;
    if (frac == 0.25f || frac == 0.75f) return 0.0f;
    if (frac == 0.5f) return -1.0f;
    return std::cos(frac * kTwoPiF32);
}

// AMD RDNA2 S_BFE_U32 / S_BFE_I32 hardware rule:
// offset = src1[4:0] (or direct offset with >= 32 guard), width = src1[22:16] (full 7-bit 0..127, NOT masked to 5 bits!).
// width == 0 -> 0; offset >= 32 -> 0 (u32) or sign-extension (i32); offset + width >= 32 -> src0 >> offset (logical / arithmetic).
uint32_t EvalSBfeU32(uint32_t src0, uint32_t offset, uint32_t width) noexcept {
    const uint32_t w = width & 0x7Fu;
    if (w == 0u || offset >= 32u) return 0u;
    if (offset + w >= 32u) return src0 >> offset;
    return (src0 >> offset) & ((1u << w) - 1u);
}

int32_t EvalSBfeI32(uint32_t src0, uint32_t offset, uint32_t width) noexcept {
    const uint32_t w = width & 0x7Fu;
    if (w == 0u) return 0;
    if (offset >= 32u) return static_cast<int32_t>(src0) >> 31;
    if (offset + w >= 32u) return static_cast<int32_t>(src0) >> offset;
    const uint32_t lsh = 32u - offset - w;
    return static_cast<int32_t>(src0 << lsh) >> (32u - w);
}

uint64_t EvalSBfeU64(uint64_t src0, uint32_t offset, uint32_t width) noexcept {
    const uint32_t w = width & 0x7Fu;
    if (w == 0u || offset >= 64u) return 0ull;
    if (offset + w >= 64u) return src0 >> offset;
    return (src0 >> offset) & ((1ull << w) - 1ull);
}

int64_t EvalSBfeI64(uint64_t src0, uint32_t offset, uint32_t width) noexcept {
    const uint32_t w = width & 0x7Fu;
    if (w == 0u) return 0;
    if (offset >= 64u) return static_cast<int64_t>(src0) >> 63;
    if (offset + w >= 64u) return static_cast<int64_t>(src0) >> offset;
    const uint32_t lsh = 64u - offset - w;
    return static_cast<int64_t>(src0 << lsh) >> (64u - w);
}

// V_BFE_U32 / V_BFE_I32 (VOP3): 5-bit offset src1[4:0], 5-bit width src2[4:0]
uint32_t EvalBfeU32(uint32_t src0, uint32_t offset, uint32_t width) noexcept {
    return EvalSBfeU32(src0, offset & 31u, width & 31u);
}

int32_t EvalBfeI32(uint32_t src0, uint32_t offset, uint32_t width) noexcept {
    return EvalSBfeI32(src0, offset & 31u, width & 31u);
}

uint32_t EvalPermB32(uint32_t src0, uint32_t src1, uint32_t sel_word) noexcept {
    uint32_t out = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t s = (sel_word >> (i * 8u)) & 0xFFu;
        uint32_t byte_val = 0u;
        if (s < 4u) {
            byte_val = (src1 >> (s * 8u)) & 0xFFu;
        } else if (s < 8u) {
            byte_val = (src0 >> ((s - 4u) * 8u)) & 0xFFu;
        } else if (s == 8u) {
            byte_val = ((src1 >> 15u) & 1u) ? 0xFFu : 0x00u;
        } else if (s == 9u) {
            byte_val = ((src1 >> 31u) & 1u) ? 0xFFu : 0x00u;
        } else if (s == 10u) {
            byte_val = ((src0 >> 15u) & 1u) ? 0xFFu : 0x00u;
        } else if (s == 11u) {
            byte_val = ((src0 >> 31u) & 1u) ? 0xFFu : 0x00u;
        } else if (s == 12u) {
            byte_val = 0x00u;
        } else {
            byte_val = 0xFFu;
        }
        out |= (byte_val << (i * 8u));
    }
    return out;
}

uint32_t EvalLerpU8(uint32_t a, uint32_t b, uint32_t c) noexcept {
    uint32_t out = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t ba = (a >> (i * 8u)) & 0xFFu;
        const uint32_t bb = (b >> (i * 8u)) & 0xFFu;
        const uint32_t bc = (c >> (i * 8u)) & 1u;
        out |= (((ba + bb + bc) >> 1u) & 0xFFu) << (i * 8u);
    }
    return out;
}

uint32_t EvalSadU8(uint32_t a, uint32_t b, uint32_t c) noexcept {
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t ba = (a >> (i * 8u)) & 0xFFu;
        const uint32_t bb = (b >> (i * 8u)) & 0xFFu;
        sum += (ba >= bb) ? (ba - bb) : (bb - ba);
    }
    return c + sum;
}

uint32_t EvalMsadU8(uint32_t a, uint32_t b, uint32_t c) noexcept {
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t ba = (a >> (i * 8u)) & 0xFFu;
        const uint32_t bb = (b >> (i * 8u)) & 0xFFu;
        if (bb != 0u) {
            sum += (ba >= bb) ? (ba - bb) : (bb - ba);
        }
    }
    return c + sum;
}

float EvalDivFixupF32(float s0, float s1, float s2) noexcept {
    // s0 = quotient, s1 = denominator, s2 = numerator
    const bool sign_out = std::signbit(s1) != std::signbit(s2);
    auto signed_val = [&](float mag) noexcept { return sign_out ? -std::fabs(mag) : std::fabs(mag); };
    if (std::isnan(s1) || std::isnan(s2)) return std::numeric_limits<float>::quiet_NaN();
    if (std::isinf(s2) && std::isinf(s1)) return std::numeric_limits<float>::quiet_NaN();
    if (s2 == 0.0f && s1 == 0.0f) return std::numeric_limits<float>::quiet_NaN();
    if (std::isinf(s2) || s1 == 0.0f) return signed_val(std::numeric_limits<float>::infinity());
    if (s2 == 0.0f || std::isinf(s1)) return signed_val(0.0f);
    if (std::isnan(s0)) return signed_val(std::numeric_limits<float>::infinity());
    return signed_val(std::fabs(s0));
}

double EvalDivFixupF64(double s0, double s1, double s2) noexcept {
    // True IEEE-754 double-precision v_div_fixup_f64 (accurate past f32/i64 ranges)
    const bool sign_out = std::signbit(s1) != std::signbit(s2);
    auto signed_val = [&](double mag) noexcept { return sign_out ? -std::fabs(mag) : std::fabs(mag); };
    if (std::isnan(s1) || std::isnan(s2)) return std::numeric_limits<double>::quiet_NaN();
    if (std::isinf(s2) && std::isinf(s1)) return std::numeric_limits<double>::quiet_NaN();
    if (s2 == 0.0 && s1 == 0.0) return std::numeric_limits<double>::quiet_NaN();
    if (std::isinf(s2) || s1 == 0.0) return signed_val(std::numeric_limits<double>::infinity());
    if (s2 == 0.0 || std::isinf(s1)) return signed_val(0.0);
    if (std::isnan(s0)) return signed_val(std::numeric_limits<double>::infinity());
    return signed_val(std::fabs(s0));
}

bool EvalFloatClassBits32(uint32_t bits, uint32_t mask) noexcept {
    const bool neg = (bits >> 31u) != 0u;
    const uint32_t exp = (bits >> 23u) & 0xFFu;
    const uint32_t frac = bits & 0x7FFFFFu;
    uint32_t bit = 0u;
    if (exp == 0xFFu) {
        if (frac == 0u) bit = neg ? 2u : 9u;
        else bit = ((frac & 0x400000u) != 0u) ? 1u : 0u;
    } else if (exp == 0u) {
        if (frac == 0u) bit = neg ? 5u : 6u;
        else bit = neg ? 4u : 7u;
    } else {
        bit = neg ? 3u : 8u;
    }
    return ((mask >> bit) & 1u) != 0u;
}

bool EvalFloatClassBits16(uint16_t bits, uint32_t mask) noexcept {
    const bool neg = (bits >> 15u) != 0u;
    const uint32_t exp = (bits >> 10u) & 0x1Fu;
    const uint32_t frac = bits & 0x03FFu;
    uint32_t bit = 0u;
    if (exp == 0x1Fu) {
        if (frac == 0u) bit = neg ? 2u : 9u;
        else bit = ((frac & 0x0200u) != 0u) ? 1u : 0u;
    } else if (exp == 0u) {
        if (frac == 0u) bit = neg ? 5u : 6u;
        else bit = neg ? 4u : 7u;
    } else {
        bit = neg ? 3u : 8u;
    }
    return ((mask >> bit) & 1u) != 0u;
}

bool EvalFloatClassBits64(uint64_t bits, uint32_t mask) noexcept {
    const bool neg = (bits >> 63u) != 0ull;
    const uint32_t exp = static_cast<uint32_t>((bits >> 52u) & 0x7FFull);
    const uint64_t frac = bits & 0x000FFFFFFFFFFFFFull;
    uint32_t bit = 0u;
    if (exp == 0x7FFu) {
        if (frac == 0ull) bit = neg ? 2u : 9u;
        else bit = ((frac & 0x0008000000000000ull) != 0ull) ? 1u : 0u;
    } else if (exp == 0u) {
        if (frac == 0ull) bit = neg ? 5u : 6u;
        else bit = neg ? 4u : 7u;
    } else {
        bit = neg ? 3u : 8u;
    }
    return ((mask >> bit) & 1u) != 0u;
}

uint32_t ExactRcpF32Bits(uint32_t b) noexcept {
    const uint32_t sign = b & 0x80000000u;
    const uint32_t mag = b & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return 0x7FC00000u;
    if (mag == 0u) return sign | 0x7F800000u;
    if (mag == 0x7F800000u) return sign;
    return FloatToBits(1.0f / BitsToFloat(b));
}

uint32_t ExactRsqF32Bits(uint32_t b) noexcept {
    const uint32_t sign = b & 0x80000000u;
    const uint32_t mag = b & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return 0x7FC00000u;
    if (mag == 0u) return sign | 0x7F800000u;
    if (sign != 0u) return 0x7FC00000u;
    if (mag == 0x7F800000u) return 0u;
    return FloatToBits(1.0f / std::sqrt(BitsToFloat(b)));
}

uint32_t ExactSqrtF32Bits(uint32_t b) noexcept {
    const uint32_t sign = b & 0x80000000u;
    const uint32_t mag = b & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return 0x7FC00000u;
    if (mag == 0u) return sign;
    if (sign != 0u) return 0x7FC00000u;
    if (mag == 0x7F800000u) return 0x7F800000u;
    return FloatToBits(std::sqrt(BitsToFloat(b)));
}

bool EvalFloatCmp(double a, double b, uint32_t cc) noexcept {
    const bool ord = !std::isnan(a) && !std::isnan(b);
    switch (cc & 15u) {
        case 0u:  return false;
        case 1u:  return ord && (a < b);
        case 2u:  return ord && (a == b);
        case 3u:  return ord && (a <= b);
        case 4u:  return ord && (a > b);
        case 5u:  return ord && (a != b);
        case 6u:  return ord && (a >= b);
        case 7u:  return ord;
        case 8u:  return !ord;
        case 9u:  return !ord || (a < b);
        case 10u: return !ord || (a == b);
        case 11u: return !ord || (a <= b);
        case 12u: return !ord || (a > b);
        case 13u: return !ord || (a != b);
        case 14u: return !ord || (a >= b);
        case 15u: return true;
        default:  return false;
    }
}

bool EvalIntCmpSigned(int64_t a, int64_t b, uint32_t cc) noexcept {
    switch (cc & 7u) {
        case 0u: return false;
        case 1u: return a < b;
        case 2u: return a == b;
        case 3u: return a <= b;
        case 4u: return a > b;
        case 5u: return a != b;
        case 6u: return a >= b;
        case 7u: return true;
        default: return false;
    }
}

bool EvalIntCmpUnsigned(uint64_t a, uint64_t b, uint32_t cc) noexcept {
    switch (cc & 7u) {
        case 0u: return false;
        case 1u: return a < b;
        case 2u: return a == b;
        case 3u: return a <= b;
        case 4u: return a > b;
        case 5u: return a != b;
        case 6u: return a >= b;
        case 7u: return true;
        default: return false;
    }
}

uint32_t EvalWqm32(uint32_t mask) noexcept {
    uint32_t out = 0u;
    for (uint32_t q = 0u; q < 8u; ++q) {
        const uint32_t m = 0xFu << (q * 4u);
        if ((mask & m) != 0u) out |= m;
    }
    return out;
}

uint64_t EvalWqm64(uint64_t mask) noexcept {
    uint64_t out = 0ull;
    for (uint32_t q = 0u; q < 16u; ++q) {
        const uint64_t m = 0xFull << (q * 4u);
        if ((mask & m) != 0ull) out |= m;
    }
    return out;
}

uint32_t EvalQuadMask32(uint32_t mask) noexcept {
    uint32_t out = 0u;
    for (uint32_t q = 0u; q < 8u; ++q) {
        if ((mask & (0xFu << (q * 4u))) != 0u) out |= (1u << q);
    }
    return out;
}

uint64_t EvalQuadMask64(uint64_t mask) noexcept {
    uint64_t out = 0ull;
    for (uint32_t q = 0u; q < 16u; ++q) {
        if ((mask & (0xFull << (q * 4u))) != 0ull) out |= (1ull << q);
    }
    return out;
}

}  // namespace

Status ExecuteScalarVectorCpu(
    const std::vector<SvDecodedInst>& insts,
    SvWaveState& wave) {
    if (insts.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty instruction stream for ExecuteScalarVectorCpu");
    }

    std::unordered_map<uint32_t, size_t> pc_to_idx;
    pc_to_idx.reserve(insts.size());
    for (size_t i = 0u; i < insts.size(); ++i) {
        pc_to_idx[insts[i].base.word_offset] = i;
    }

    const uint32_t wave_n = (wave.wave_size == 64u) ? 64u : 32u;
    size_t idx = 0u;
    uint32_t steps = 0u;

    while (idx < insts.size() && !wave.ended && !wave.halted) {
        if (++steps > wave.max_steps) {
            return Status::Error(StatusCode::ShaderError, "ExecuteScalarVectorCpu exceeded max_steps");
        }
        const SvDecodedInst& sv = insts[idx];
        const Rdna2Instruction& inst = sv.base;
        const ScalarVectorOpcodeEntry* e = sv.entry;
        const DecodedSvModifier& mod = sv.modifier;
        wave.pc = inst.word_offset;
        ++wave.instructions_executed;

        size_t next_idx = idx + 1u;
        const uint32_t next_word_offset = inst.word_offset + inst.word_count;

        auto jump_rel = [&](int16_t simm) {
            const int32_t target = static_cast<int32_t>(next_word_offset) + static_cast<int32_t>(simm);
            if (target >= 0) {
                auto it = pc_to_idx.find(static_cast<uint32_t>(target));
                if (it != pc_to_idx.end()) {
                    next_idx = it->second;
                    return;
                }
            }
            next_idx = insts.size();
        };

        // Handle non-SV memory ops (MUBUF / FLAT / GLOBAL / EXP)
        if (!e) {
            if (inst.encoding == Rdna2Encoding::MUBUF) {
                auto& buf = (inst.srsrc == 0u) ? wave.uav0 : wave.uav1;
                const uint32_t soff = ReadScalarOp32(wave, DecodeRdna2SourceOperand(inst.soffset, 0u));
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    if (!wave.IsLaneActive(l)) continue;
                    const uint32_t vaddr = inst.offen ? wave.ReadVgpr(l, static_cast<uint8_t>(inst.src[1].reg_index)) : 0u;
                    const uint32_t byte_addr = vaddr + soff + static_cast<uint32_t>(inst.mem_offset);
                    const uint32_t dw = byte_addr >> 2u;
                    if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                        const uint32_t val = (dw < buf.size()) ? buf[dw] : 0u;
                        wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), val);
                    } else if (inst.opcode == Rdna2Opcode::BUFFER_STORE_DWORD) {
                        if (dw >= buf.size()) buf.resize(static_cast<size_t>(dw) + 1u, 0u);
                        buf[dw] = wave.ReadVgpr(l, static_cast<uint8_t>(inst.src[0].reg_index));
                    }
                }
            } else if (inst.encoding == Rdna2Encoding::FLAT) {
                auto& buf = (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD || inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD)
                                ? (wave.uav1.empty() ? wave.uav0 : wave.uav1)
                                : wave.uav0;
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    if (!wave.IsLaneActive(l)) continue;
                    const uint32_t vaddr = wave.ReadVgpr(l, static_cast<uint8_t>(inst.src[0].reg_index));
                    const uint32_t byte_addr = static_cast<uint32_t>(static_cast<int32_t>(vaddr) + inst.mem_offset);
                    const uint32_t dw = byte_addr >> 2u;
                    if (inst.opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD || inst.opcode == Rdna2Opcode::FLAT_LOAD_DWORD) {
                        const uint32_t val = (dw < buf.size()) ? buf[dw] : 0u;
                        wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), val);
                    } else if (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD || inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD) {
                        if (dw >= buf.size()) buf.resize(static_cast<size_t>(dw) + 1u, 0u);
                        buf[dw] = wave.ReadVgpr(l, static_cast<uint8_t>(inst.src[1].reg_index));
                    }
                }
            }
            idx = next_idx;
            continue;
        }

        const uint16_t raw = inst.raw_opcode;
        const Rdna2Encoding enc = inst.encoding;

        // ==================== SCALAR ENCODINGS ====================
        if (enc == Rdna2Encoding::SOP2) {
            const uint32_t s0 = ReadScalarOp32(wave, inst.src[0]);
            const uint32_t s1 = ReadScalarOp32(wave, inst.src[1]);
            const uint64_t s0_64 = ReadScalarOp64(wave, inst.src[0]);
            const uint64_t s1_64 = ReadScalarOp64(wave, inst.src[1]);
            switch (raw) {
                case 0x00: { uint64_t r = static_cast<uint64_t>(s0) + s1; WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r >> 32u) != 0ull; break; }
                case 0x01: { WriteScalarDst32(wave, inst.dst, s0 - s1); wave.scc = (s1 > s0); break; }
                case 0x02: {
                    int64_t r = static_cast<int64_t>(static_cast<int32_t>(s0)) + static_cast<int32_t>(s1);
                    WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r));
                    wave.scc = (r < INT32_MIN || r > INT32_MAX);
                    break;
                }
                case 0x03: {
                    int64_t r = static_cast<int64_t>(static_cast<int32_t>(s0)) - static_cast<int32_t>(s1);
                    WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r));
                    wave.scc = (r < INT32_MIN || r > INT32_MAX);
                    break;
                }
                case 0x04: { uint64_t r = static_cast<uint64_t>(s0) + s1 + (wave.scc ? 1ull : 0ull); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r >> 32u) != 0ull; break; }
                case 0x05: { uint64_t c = wave.scc ? 1ull : 0ull; WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(s0 - s1 - c)); wave.scc = (static_cast<uint64_t>(s1) + c > s0); break; }
                case 0x06: { int32_t a = static_cast<int32_t>(s0), b = static_cast<int32_t>(s1); wave.scc = (a < b); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(a < b ? a : b)); break; }
                case 0x07: { wave.scc = (s0 < s1); WriteScalarDst32(wave, inst.dst, s0 < s1 ? s0 : s1); break; }
                case 0x08: { int32_t a = static_cast<int32_t>(s0), b = static_cast<int32_t>(s1); wave.scc = (a > b); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(a > b ? a : b)); break; }
                case 0x09: { wave.scc = (s0 > s1); WriteScalarDst32(wave, inst.dst, s0 > s1 ? s0 : s1); break; }
                case 0x0A: WriteScalarDst32(wave, inst.dst, wave.scc ? s0 : s1); break;
                case 0x0B: WriteScalarDst64(wave, inst.dst, wave.scc ? s0_64 : s1_64); break;
                case 0x0E: { uint32_t r = s0 & s1; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x0F: { uint64_t r = s0_64 & s1_64; WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x10: { uint32_t r = s0 | s1; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x11: { uint64_t r = s0_64 | s1_64; WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x12: { uint32_t r = s0 ^ s1; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x13: { uint64_t r = s0_64 ^ s1_64; WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x14: { uint32_t r = s0 & ~s1; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x15: { uint64_t r = s0_64 & ~s1_64; WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x16: { uint32_t r = s0 | ~s1; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x17: { uint64_t r = s0_64 | ~s1_64; WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x18: { uint32_t r = ~(s0 & s1); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x19: { uint64_t r = ~(s0_64 & s1_64); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x1A: { uint32_t r = ~(s0 | s1); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x1B: { uint64_t r = ~(s0_64 | s1_64); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x1C: { uint32_t r = ~(s0 ^ s1); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x1D: { uint64_t r = ~(s0_64 ^ s1_64); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x1E: { uint32_t r = s0 << (s1 & 31u); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x1F: { uint64_t r = s0_64 << (s1 & 63u); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x20: { uint32_t r = s0 >> (s1 & 31u); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x21: { uint64_t r = s0_64 >> (s1 & 63u); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x22: { int32_t r = static_cast<int32_t>(s0) >> (s1 & 31u); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r != 0); break; }
                case 0x23: { int64_t r = static_cast<int64_t>(s0_64) >> (s1 & 63u); WriteScalarDst64(wave, inst.dst, static_cast<uint64_t>(r)); wave.scc = (r != 0); break; }
                case 0x24: { uint32_t r = static_cast<uint32_t>(((1ull << (s0 & 31u)) - 1ull) << (s1 & 31u)); WriteScalarDst32(wave, inst.dst, r); break; }
                case 0x25: { uint64_t w = s0 & 63u; uint64_t m = (w == 64u) ? ~0ull : ((1ull << w) - 1ull); WriteScalarDst64(wave, inst.dst, m << (s1 & 63u)); break; }
                case 0x26: { int64_t d = static_cast<int64_t>(static_cast<int32_t>(s0)) - static_cast<int32_t>(s1); uint32_t r = static_cast<uint32_t>(d < 0 ? -d : d); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x27: { uint32_t r = EvalSBfeU32(s0, s1 & 31u, (s1 >> 16u) & 0x7Fu); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x28: { int32_t r = EvalSBfeI32(s0, s1 & 31u, (s1 >> 16u) & 0x7Fu); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r != 0); break; }
                case 0x29: {
                    const uint64_t r = EvalSBfeU64(s0_64, s1 & 63u, (s1 >> 16u) & 0x7Fu);
                    WriteScalarDst64(wave, inst.dst, r);
                    wave.scc = (r != 0ull);
                    break;
                }
                case 0x2A: {
                    const int64_t r = EvalSBfeI64(s0_64, s1 & 63u, (s1 >> 16u) & 0x7Fu);
                    WriteScalarDst64(wave, inst.dst, static_cast<uint64_t>(r));
                    wave.scc = (r != 0);
                    break;
                }
                case 0x2C: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(static_cast<int32_t>(s0) * static_cast<int32_t>(s1))); break;
                case 0x2E: { uint64_t r = (static_cast<uint64_t>(s0) << 1u) + s1; WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r >> 32u) != 0ull; break; }
                case 0x2F: { uint64_t r = (static_cast<uint64_t>(s0) << 2u) + s1; WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r >> 32u) != 0ull; break; }
                case 0x30: { uint64_t r = (static_cast<uint64_t>(s0) << 3u) + s1; WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r >> 32u) != 0ull; break; }
                case 0x31: { uint64_t r = (static_cast<uint64_t>(s0) << 4u) + s1; WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r)); wave.scc = (r >> 32u) != 0ull; break; }
                case 0x32: WriteScalarDst32(wave, inst.dst, (s0 & 0xFFFFu) | ((s1 & 0xFFFFu) << 16u)); break;
                case 0x33: WriteScalarDst32(wave, inst.dst, (s0 & 0xFFFFu) | (s1 & 0xFFFF0000u)); break;
                case 0x34: WriteScalarDst32(wave, inst.dst, (s0 >> 16u) | (s1 & 0xFFFF0000u)); break;
                case 0x35: { uint64_t p = static_cast<uint64_t>(s0) * static_cast<uint64_t>(s1); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(p >> 32u)); break; }
                case 0x36: { int64_t p = static_cast<int64_t>(static_cast<int32_t>(s0)) * static_cast<int64_t>(static_cast<int32_t>(s1)); WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32u)); break; }
                default: break;
            }
        } else if (enc == Rdna2Encoding::SOP1) {
            const uint32_t s0 = (inst.src_count > 0u) ? ReadScalarOp32(wave, inst.src[0]) : 0u;
            const uint64_t s0_64 = (inst.src_count > 0u) ? ReadScalarOp64(wave, inst.src[0]) : 0ull;
            switch (raw) {
                case 0x03: WriteScalarDst32(wave, inst.dst, s0); break;
                case 0x04: WriteScalarDst64(wave, inst.dst, s0_64); break;
                case 0x05: if (wave.scc) WriteScalarDst32(wave, inst.dst, s0); break;
                case 0x06: if (wave.scc) WriteScalarDst64(wave, inst.dst, s0_64); break;
                case 0x07: { uint32_t r = ~s0; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x08: { uint32_t r = static_cast<uint32_t>(std::popcount(s0)); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x09: { uint32_t r = static_cast<uint32_t>(std::popcount(s0_64)); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x0A: { uint64_t r = ~s0_64; WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x0B: { uint32_t r = EvalWqm32(s0); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x0C: { uint64_t r = EvalWqm64(s0_64); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x0D: WriteScalarDst32(wave, inst.dst, BitReverse32(s0)); break;
                case 0x0E: WriteScalarDst64(wave, inst.dst, BitReverse64(s0_64)); break;
                case 0x0F: { uint32_t r = static_cast<uint32_t>(std::popcount(~s0)); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x10: { uint32_t r = static_cast<uint32_t>(std::popcount(~s0_64)); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x11: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitLow32(~s0))); break;
                case 0x12: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitLow64(~s0_64))); break;
                case 0x13: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitLow32(s0))); break;
                case 0x14: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitLow64(s0_64))); break;
                case 0x15: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitHighU32(s0))); break;
                case 0x16: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitHighU64(s0_64))); break;
                case 0x17: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitHighI32(static_cast<int32_t>(s0)))); break;
                case 0x18: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(FirstBitHighI64(static_cast<int64_t>(s0_64)))); break;
                case 0x19: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(SignExtend32(s0 & 0xFFu, 8u))); break;
                case 0x1A: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(SignExtend32(s0 & 0xFFFFu, 16u))); break;
                case 0x1B: WriteScalarDst32(wave, inst.dst, ReadScalarOp32(wave, inst.dst) & ~(1u << (s0 & 31u))); break;
                case 0x1C: WriteScalarDst64(wave, inst.dst, ReadScalarOp64(wave, inst.dst) & ~(1ull << (s0 & 63u))); break;
                case 0x1D: { auto it = pc_to_idx.find(static_cast<uint32_t>(s0_64 >> 2u)); next_idx = (it != pc_to_idx.end()) ? it->second : insts.size(); break; }
                case 0x1E: { WriteScalarDst64(wave, inst.dst, static_cast<uint64_t>(next_word_offset) << 2u); auto it = pc_to_idx.find(static_cast<uint32_t>(s0_64 >> 2u)); next_idx = (it != pc_to_idx.end()) ? it->second : insts.size(); break; }
                case 0x1F: WriteScalarDst64(wave, inst.dst, static_cast<uint64_t>(next_word_offset) << 2u); break;
                case 0x21: WriteScalarDst32(wave, inst.dst, ReadScalarOp32(wave, inst.dst) | (1u << (s0 & 31u))); break;
                case 0x22: WriteScalarDst64(wave, inst.dst, ReadScalarOp64(wave, inst.dst) | (1ull << (s0 & 63u))); break;
                case 0x24: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = s0_64 & wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x25: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = s0_64 | wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x26: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = s0_64 ^ wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x27: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = s0_64 & ~wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x28: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = s0_64 | ~wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x29: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = ~(s0_64 & wave.exec); wave.scc = (wave.exec != 0ull); break; }
                case 0x2A: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = ~(s0_64 | wave.exec); wave.scc = (wave.exec != 0ull); break; }
                case 0x2B: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = ~(s0_64 ^ wave.exec); wave.scc = (wave.exec != 0ull); break; }
                case 0x2C: { uint32_t r = EvalQuadMask32(s0); WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x2D: { uint64_t r = EvalQuadMask64(s0_64); WriteScalarDst64(wave, inst.dst, r); wave.scc = (r != 0ull); break; }
                case 0x2E: WriteScalarDst32(wave, inst.dst, wave.ReadSgpr(static_cast<uint8_t>((inst.src[0].reg_index + wave.m0) & 0x7Fu))); break;
                case 0x2F: WriteScalarDst64(wave, inst.dst, wave.ReadSgpr64(static_cast<uint8_t>((inst.src[0].reg_index + wave.m0) & 0x7Fu))); break;
                case 0x30: wave.WriteSgpr(static_cast<uint8_t>((inst.dst.reg_index + wave.m0) & 0x7Fu), s0); break;
                case 0x31: wave.WriteSgpr64(static_cast<uint8_t>((inst.dst.reg_index + wave.m0) & 0x7Fu), s0_64); break;
                case 0x34: { int32_t a = static_cast<int32_t>(s0); uint32_t r = (a < 0) ? (0u - s0) : s0; WriteScalarDst32(wave, inst.dst, r); wave.scc = (r != 0u); break; }
                case 0x35: WriteScalarDst32(wave, inst.dst, s0); break;
                case 0x37: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = ~s0_64 & wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x38: { WriteScalarDst64(wave, inst.dst, wave.exec); wave.exec = ~s0_64 | wave.exec; wave.scc = (wave.exec != 0ull); break; }
                case 0x39: { uint64_t r = ~s0_64 & wave.exec; WriteScalarDst64(wave, inst.dst, r); wave.exec = r; wave.scc = (r != 0ull); break; }
                case 0x3A: { uint64_t r = s0_64 & ~wave.exec; WriteScalarDst64(wave, inst.dst, r); wave.exec = r; wave.scc = (r != 0ull); break; }
                case 0x3B: WriteScalarDst64(wave, inst.dst, BitReplicate32To64(s0)); break;
                case 0x3C: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = s0 & ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x3D: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = s0 | ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x3E: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = s0 ^ ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x3F: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = s0 & ~ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x40: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = s0 | ~ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x41: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = ~(s0 & ex); wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x42: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = ~(s0 | ex); wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x43: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = ~(s0 ^ ex); wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x44: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = ~s0 & ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x45: { uint32_t ex = static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, ex); ex = ~s0 | ex; wave.WriteSgpr(126u, ex); wave.scc = (ex != 0u); break; }
                case 0x46: { uint32_t r = ~s0 & static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, r); wave.WriteSgpr(126u, r); wave.scc = (r != 0u); break; }
                case 0x47: { uint32_t r = s0 & ~static_cast<uint32_t>(wave.exec); WriteScalarDst32(wave, inst.dst, r); wave.WriteSgpr(126u, r); wave.scc = (r != 0u); break; }
                case 0x49: wave.WriteSgpr(static_cast<uint8_t>((inst.dst.reg_index + ((wave.m0 >> 16u) & 0xFFFFu)) & 0x7Fu), wave.ReadSgpr(static_cast<uint8_t>((inst.src[0].reg_index + (wave.m0 & 0xFFFFu)) & 0x7Fu))); break;
                case 0x4A: wave.WriteSgpr64(static_cast<uint8_t>((inst.dst.reg_index + ((wave.m0 >> 16u) & 0xFFFFu)) & 0x7Fu), wave.ReadSgpr64(static_cast<uint8_t>((inst.src[0].reg_index + (wave.m0 & 0xFFFFu)) & 0x7Fu))); break;
                default: break;
            }
        } else if (enc == Rdna2Encoding::SOPK) {
            const int32_t simm = static_cast<int32_t>(inst.simm16);
            const uint32_t uimm = static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16));
            const uint32_t cur = ReadScalarOp32(wave, inst.dst);
            switch (raw) {
                case 0x00: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(simm)); break;
                case 0x02: if (wave.scc) WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(simm)); break;
                case 0x03: wave.scc = (static_cast<int32_t>(cur) == simm); break;
                case 0x04: wave.scc = (static_cast<int32_t>(cur) != simm); break;
                case 0x05: wave.scc = (static_cast<int32_t>(cur) > simm); break;
                case 0x06: wave.scc = (static_cast<int32_t>(cur) >= simm); break;
                case 0x07: wave.scc = (static_cast<int32_t>(cur) < simm); break;
                case 0x08: wave.scc = (static_cast<int32_t>(cur) <= simm); break;
                case 0x09: wave.scc = (cur == uimm); break;
                case 0x0A: wave.scc = (cur != uimm); break;
                case 0x0B: wave.scc = (cur > uimm); break;
                case 0x0C: wave.scc = (cur >= uimm); break;
                case 0x0D: wave.scc = (cur < uimm); break;
                case 0x0E: wave.scc = (cur <= uimm); break;
                case 0x0F: {
                    int64_t r = static_cast<int64_t>(static_cast<int32_t>(cur)) + simm;
                    WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(r));
                    wave.scc = (r < INT32_MIN || r > INT32_MAX);
                    break;
                }
                case 0x10: WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(static_cast<int32_t>(cur) * simm)); break;
                case 0x12: {
                    const uint32_t hw_id = uimm & 0x3Fu;
                    const uint32_t off = (uimm >> 6u) & 0x1Fu;
                    const uint32_t sz = ((uimm >> 11u) & 0x1Fu) + 1u;
                    const uint32_t hv = (hw_id < wave.hwreg.size()) ? wave.hwreg[hw_id] : 0u;
                    const uint32_t mask = (sz >= 32u) ? 0xFFFFFFFFu : ((1u << sz) - 1u);
                    WriteScalarDst32(wave, inst.dst, (hv >> off) & mask);
                    break;
                }
                case 0x13:
                case 0x15: {
                    const uint32_t hw_id = uimm & 0x3Fu;
                    const uint32_t off = (uimm >> 6u) & 0x1Fu;
                    const uint32_t sz = ((uimm >> 11u) & 0x1Fu) + 1u;
                    const uint32_t src_v = (raw == 0x15u && inst.src_count > 0u) ? inst.src[0].literal_bits : cur;
                    if (hw_id < wave.hwreg.size()) {
                        const uint32_t mask = (sz >= 32u) ? 0xFFFFFFFFu : (((1u << sz) - 1u) << off);
                        wave.hwreg[hw_id] = (wave.hwreg[hw_id] & ~mask) | ((src_v << off) & mask);
                    }
                    break;
                }
                case 0x16: {
                    WriteScalarDst64(wave, inst.dst, static_cast<uint64_t>(next_word_offset) << 2u);
                    jump_rel(inst.simm16);
                    break;
                }
                default: break;
            }
        } else if (enc == Rdna2Encoding::SOPC) {
            const uint32_t s0 = ReadScalarOp32(wave, inst.src[0]);
            const uint32_t s1 = ReadScalarOp32(wave, inst.src[1]);
            const uint64_t s0_64 = ReadScalarOp64(wave, inst.src[0]);
            const uint64_t s1_64 = ReadScalarOp64(wave, inst.src[1]);
            switch (raw) {
                case 0x00: wave.scc = (static_cast<int32_t>(s0) == static_cast<int32_t>(s1)); break;
                case 0x01: wave.scc = (static_cast<int32_t>(s0) != static_cast<int32_t>(s1)); break;
                case 0x02: wave.scc = (static_cast<int32_t>(s0) > static_cast<int32_t>(s1)); break;
                case 0x03: wave.scc = (static_cast<int32_t>(s0) >= static_cast<int32_t>(s1)); break;
                case 0x04: wave.scc = (static_cast<int32_t>(s0) < static_cast<int32_t>(s1)); break;
                case 0x05: wave.scc = (static_cast<int32_t>(s0) <= static_cast<int32_t>(s1)); break;
                case 0x06: wave.scc = (s0 == s1); break;
                case 0x07: wave.scc = (s0 != s1); break;
                case 0x08: wave.scc = (s0 > s1); break;
                case 0x09: wave.scc = (s0 >= s1); break;
                case 0x0A: wave.scc = (s0 < s1); break;
                case 0x0B: wave.scc = (s0 <= s1); break;
                case 0x0C: wave.scc = ((s0 & (1u << (s1 & 31u))) == 0u); break;
                case 0x0D: wave.scc = ((s0 & (1u << (s1 & 31u))) != 0u); break;
                case 0x0E: wave.scc = ((s0_64 & (1ull << (s1 & 63u))) == 0ull); break;
                case 0x0F: wave.scc = ((s0_64 & (1ull << (s1 & 63u))) != 0ull); break;
                case 0x12: wave.scc = (s0_64 == s1_64); break;
                case 0x13: wave.scc = (s0_64 != s1_64); break;
                default: break;
            }
        } else if (enc == Rdna2Encoding::SOPP) {
            switch (raw) {
                case 0x01: case 0x1B: case 0x1E: case 0x1F: wave.ended = true; break;
                case 0x02: jump_rel(inst.simm16); break;
                case 0x04: if (!wave.scc) jump_rel(inst.simm16); break;
                case 0x05: if (wave.scc) jump_rel(inst.simm16); break;
                case 0x06: if ((wave.vcc & wave.WaveMask()) == 0ull) jump_rel(inst.simm16); break;
                case 0x07: if ((wave.vcc & wave.WaveMask()) != 0ull) jump_rel(inst.simm16); break;
                case 0x08: if (wave.ActiveMask() == 0ull) jump_rel(inst.simm16); break;
                case 0x09: if (wave.ActiveMask() != 0ull) jump_rel(inst.simm16); break;
                case 0x0D: case 0x11: wave.halted = true; break;
                default: break;
            }
        } else if (enc == Rdna2Encoding::SMEM) {
            const uint32_t soff = (inst.soffset < 106u) ? wave.ReadSgpr(inst.soffset) : 0u;
            const int64_t byte_addr = static_cast<int64_t>(soff) + static_cast<int64_t>(inst.mem_offset);
            const bool valid_addr = (byte_addr >= 0 && byte_addr <= 0xFFFFFFFFll);
            const uint32_t dw = valid_addr ? static_cast<uint32_t>(byte_addr >> 2) : 0u;
            auto& src_buf = (inst.sbase == 0u && !wave.root_constants.empty()) ? wave.root_constants : wave.uav0;
            auto load_n = [&](uint32_t count) {
                const uint8_t dst_reg = static_cast<uint8_t>(inst.dst.reg_index);
                for (uint32_t k = 0u; k < count; ++k) {
                    const uint32_t v = (valid_addr && (static_cast<size_t>(dw) + k) < src_buf.size())
                                           ? src_buf[static_cast<size_t>(dw) + k]
                                           : 0u;
                    wave.WriteSgpr(static_cast<uint8_t>(dst_reg + k), v);
                }
            };
            if (raw <= 0x04u) load_n(1u << raw);
            else if (raw >= 0x08u && raw <= 0x0Cu) load_n(1u << (raw - 0x08u));
            else if (raw >= 0x10u && raw <= 0x12u) load_n(1u << (raw - 0x10u));
            else if (raw == 0x2Au) WriteScalarDst32(wave, inst.dst, 0u);
            else if (raw == 0x24u || raw == 0x25u) WriteScalarDst64(wave, inst.dst, wave.instructions_executed);
            else if (raw >= 0x40u && raw <= 0xACu) {
                const uint32_t op_idx = raw & 0x1Fu;
                const bool is_x2 = (raw >= 0x60u && raw < 0x80u) || (raw >= 0xA0u);
                if (!is_x2 && op_idx < 13u) {
                    if (dw >= wave.uav0.size()) wave.uav0.resize(static_cast<size_t>(dw) + 1u, 0u);
                    const uint32_t old_v = wave.uav0[dw];
                    const uint32_t data = ReadScalarOp32(wave, inst.dst);
                    uint32_t nv = old_v;
                    switch (op_idx) {
                        case 0: nv = data; break;
                        case 1: { uint32_t cmp = wave.ReadSgpr(static_cast<uint8_t>(inst.dst.reg_index + 1u)); if (old_v == cmp) nv = data; break; }
                        case 2: nv = old_v + data; break;
                        case 3: nv = old_v - data; break;
                        case 4: nv = static_cast<uint32_t>(std::min(static_cast<int32_t>(old_v), static_cast<int32_t>(data))); break;
                        case 5: nv = std::min(old_v, data); break;
                        case 6: nv = static_cast<uint32_t>(std::max(static_cast<int32_t>(old_v), static_cast<int32_t>(data))); break;
                        case 7: nv = std::max(old_v, data); break;
                        case 8: nv = old_v & data; break;
                        case 9: nv = old_v | data; break;
                        case 10: nv = old_v ^ data; break;
                        case 11: nv = (old_v >= data) ? 0u : (old_v + 1u); break;
                        case 12: nv = (old_v == 0u || old_v > data) ? data : (old_v - 1u); break;
                        default: break;
                    }
                    wave.uav0[dw] = nv;
                    if (inst.glc) WriteScalarDst32(wave, inst.dst, old_v);
                }
            }
        }
        // ==================== VECTOR ENCODINGS ====================
        else if (enc == Rdna2Encoding::VOP1 || (enc == Rdna2Encoding::VOP3 && raw >= 0x180u && raw < 0x200u)) {
            const uint16_t v1op = (enc == Rdna2Encoding::VOP3) ? static_cast<uint16_t>(raw - 0x180u) : raw;
            const bool is_float = (e->data_kind == DK::Float);
            if (v1op == 0x02u) {
                // v_readfirstlane_b32
                uint32_t first_val = 0u;
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    if (wave.IsLaneActive(l)) {
                        first_val = ReadVecOp32(wave, l, inst.src[0], false);
                        break;
                    }
                }
                WriteScalarDst32(wave, inst.dst, first_val);
            } else if (v1op != 0x00u && v1op != 0x1Bu && v1op != 0x41u) {
                std::array<uint32_t, 64> src0_snap{};
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    src0_snap[l] = (mod.kind != SvModifierKind::None)
                                       ? ((mod.kind == SvModifierKind::Sdwa && mod.src0_is_sgpr)
                                              ? wave.ReadSgpr(mod.vsrc0)
                                              : wave.ReadVgpr(l, mod.vsrc0))
                                       : ReadVecOp32(wave, l, inst.src[0], is_float);
                }
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    if (!wave.IsLaneActive(l)) continue;
                    const uint32_t old_dst = wave.ReadVgpr(l, static_cast<uint8_t>(inst.dst.reg_index));
                    uint32_t s0 = src0_snap[l];
                    if (mod.kind == SvModifierKind::Sdwa) {
                        s0 = ApplySdwaSrc(s0, mod.src0_sel, mod.src0_sext, is_float, mod.src0_neg, mod.src0_abs);
                    } else if (mod.kind == SvModifierKind::Dpp16) {
                        if (!Dpp16LaneEnabled(l, mod.row_mask, mod.bank_mask)) continue;
                        const int32_t sl = ComputeDpp16SrcLane(l, mod.dpp_ctrl, wave_n);
                        if (sl < 0 || (!mod.dpp_fi && !wave.IsLaneActive(static_cast<uint32_t>(sl)))) {
                            if (!mod.bound_ctrl) continue;
                            s0 = 0u;
                        } else {
                            s0 = src0_snap[static_cast<uint32_t>(sl)];
                        }
                        if (is_float) {
                            if (mod.src0_abs) s0 &= 0x7FFFFFFFu;
                            if (mod.src0_neg) s0 ^= 0x80000000u;
                        }
                    } else if (mod.kind == SvModifierKind::Dpp8 || mod.kind == SvModifierKind::Dpp8Fi) {
                        const uint32_t sl = ComputeDpp8SrcLane(l, mod.dpp8_sel);
                        s0 = (sl < wave_n && (mod.dpp_fi || wave.IsLaneActive(sl))) ? src0_snap[sl] : 0u;
                    }

                    uint32_t res = 0u;
                    uint64_t res64 = 0ull;
                    bool write64 = false;
                    const float f0 = BitsToFloat(s0);
                    const bool src0_is_f64 = (e->bit_width == 64u || v1op == 0x03u || v1op == 0x0Fu || v1op == 0x15u || v1op == 0x3Cu);
                    const double d0 = src0_is_f64 ? BitsToDouble(ReadVecOp64(wave, l, inst.src[0], true)) : 0.0;
                    const float h0 = F16ToF32(static_cast<uint16_t>(s0 & 0xFFFFu));

                    switch (v1op) {
                        case 0x01: res = s0; break;
                        case 0x03: res = std::isnan(d0) ? 0u : ((d0 >= 2147483647.0) ? 0x7FFFFFFFu : ((d0 <= -2147483648.0) ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(d0)))); break;
                        case 0x04: res64 = DoubleToBits(static_cast<double>(static_cast<int32_t>(s0))); write64 = true; break;
                        case 0x05: res = FloatToBits(static_cast<float>(static_cast<int32_t>(s0))); break;
                        case 0x06: res = FloatToBits(static_cast<float>(s0)); break;
                        case 0x07: res = (std::isnan(f0) || f0 <= 0.0f) ? 0u : ((f0 >= 4294967296.0f) ? 0xFFFFFFFFu : static_cast<uint32_t>(f0)); break;
                        case 0x08: res = std::isnan(f0) ? 0u : ((f0 >= 2147483648.0f) ? 0x7FFFFFFFu : ((f0 <= -2147483648.0f) ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(f0)))); break;
                        case 0x0A: res = static_cast<uint32_t>(F32ToF16(f0, false)); break;
                        case 0x0B: res = FloatToBits(h0); break;
                        case 0x0C: res = static_cast<uint32_t>(static_cast<int32_t>(std::floor(f0 + 0.5f))); break;
                        case 0x0D: res = static_cast<uint32_t>(static_cast<int32_t>(std::floor(f0))); break;
                        case 0x0E: res = FloatToBits(static_cast<float>(SignExtend32(s0 & 0xFu, 4u)) * 0.0625f); break;
                        case 0x0F: res = FloatToBits(static_cast<float>(d0)); break;
                        case 0x10: res64 = DoubleToBits(static_cast<double>(f0)); write64 = true; break;
                        case 0x11: res = FloatToBits(static_cast<float>(s0 & 0xFFu)); break;
                        case 0x12: res = FloatToBits(static_cast<float>((s0 >> 8u) & 0xFFu)); break;
                        case 0x13: res = FloatToBits(static_cast<float>((s0 >> 16u) & 0xFFu)); break;
                        case 0x14: res = FloatToBits(static_cast<float>((s0 >> 24u) & 0xFFu)); break;
                        case 0x15: res = (std::isnan(d0) || d0 <= 0.0) ? 0u : ((d0 >= 4294967295.0) ? 0xFFFFFFFFu : static_cast<uint32_t>(d0)); break;
                        case 0x16: res64 = DoubleToBits(static_cast<double>(s0)); write64 = true; break;
                        case 0x17: res64 = DoubleToBits(std::trunc(d0)); write64 = true; break;
                        case 0x18: res64 = DoubleToBits(std::ceil(d0)); write64 = true; break;
                        case 0x19: res64 = DoubleToBits(std::nearbyint(d0)); write64 = true; break;
                        case 0x1A: res64 = DoubleToBits(std::floor(d0)); write64 = true; break;
                        case 0x20: res = std::isfinite(f0) ? FloatToBits(f0 - std::floor(f0)) : 0x7FC00000u; break;
                        case 0x21: res = FloatToBits(std::trunc(f0)); break;
                        case 0x22: res = FloatToBits(std::ceil(f0)); break;
                        case 0x23: res = FloatToBits(std::nearbyint(f0)); break;
                        case 0x24: res = FloatToBits(std::floor(f0)); break;
                        case 0x25: res = FloatToBits(std::exp2(f0)); break;
                        case 0x27: res = FloatToBits(std::log2(f0)); break;
                        case 0x2A: case 0x2B: res = ExactRcpF32Bits(s0); break;
                        case 0x2E: res = ExactRsqF32Bits(s0); break;
                        case 0x2F: res64 = DoubleToBits(1.0 / d0); write64 = true; break;
                        case 0x31: res64 = DoubleToBits(1.0 / std::sqrt(d0)); write64 = true; break;
                        case 0x33: res = ExactSqrtF32Bits(s0); break;
                        case 0x34: res64 = DoubleToBits(std::sqrt(d0)); write64 = true; break;
                        case 0x35: res = FloatToBits(EvalSinTurnsF32(f0)); break;
                        case 0x36: res = FloatToBits(EvalCosTurnsF32(f0)); break;
                        case 0x37: res = ~s0; break;
                        case 0x38: res = BitReverse32(s0); break;
                        case 0x39: res = static_cast<uint32_t>(FirstBitHighU32(s0)); break;
                        case 0x3A: res = static_cast<uint32_t>(FirstBitLow32(s0)); break;
                        case 0x3B: res = static_cast<uint32_t>(FirstBitHighI32(static_cast<int32_t>(s0))); break;
                        case 0x3C: { int ex = 0; (void)std::frexp(d0, &ex); res = static_cast<uint32_t>(ex); break; }
                        case 0x3D: { int ex = 0; res64 = DoubleToBits(std::frexp(d0, &ex)); write64 = true; break; }
                        case 0x3E: res64 = std::isfinite(d0) ? DoubleToBits(d0 - std::floor(d0)) : 0x7FF8000000000000ull; write64 = true; break;
                        case 0x3F: { int ex = 0; (void)std::frexp(f0, &ex); res = static_cast<uint32_t>(ex); break; }
                        case 0x40: { int ex = 0; res = FloatToBits(std::frexp(f0, &ex)); break; }
                        case 0x42: wave.WriteVgpr(l, static_cast<uint8_t>((inst.dst.reg_index + wave.m0) & 63u), s0); continue;
                        case 0x43: res = wave.ReadVgpr(l, static_cast<uint8_t>((inst.src[0].reg_index + wave.m0) & 63u)); break;
                        case 0x44: case 0x48: wave.WriteVgpr(l, static_cast<uint8_t>((inst.dst.reg_index + wave.m0) & 63u), wave.ReadVgpr(l, static_cast<uint8_t>((inst.src[0].reg_index + wave.m0) & 63u))); continue;
                        case 0x50: res = static_cast<uint32_t>(F32ToF16(static_cast<float>(s0 & 0xFFFFu))); break;
                        case 0x51: res = static_cast<uint32_t>(F32ToF16(static_cast<float>(SignExtend32(s0 & 0xFFFFu, 16u)))); break;
                        case 0x52: res = (std::isnan(h0) || h0 <= 0.0f) ? 0u : static_cast<uint32_t>(std::min(h0, 65535.0f)); break;
                        case 0x53: res = static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(h0, -32768.0f, 32767.0f)))); break;
                        case 0x54: res = F32ToF16(1.0f / h0); break;
                        case 0x55: res = F32ToF16(std::sqrt(h0)); break;
                        case 0x56: res = F32ToF16(1.0f / std::sqrt(h0)); break;
                        case 0x57: res = F32ToF16(std::log2(h0)); break;
                        case 0x58: res = F32ToF16(std::exp2(h0)); break;
                        case 0x59: { int ex = 0; res = F32ToF16(std::frexp(h0, &ex)); break; }
                        case 0x5A: { int ex = 0; (void)std::frexp(h0, &ex); res = static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(ex))); break; }
                        case 0x5B: res = F32ToF16(std::floor(h0)); break;
                        case 0x5C: res = F32ToF16(std::ceil(h0)); break;
                        case 0x5D: res = F32ToF16(std::trunc(h0)); break;
                        case 0x5E: res = F32ToF16(std::nearbyint(h0)); break;
                        case 0x5F: res = std::isfinite(h0) ? F32ToF16(h0 - std::floor(h0)) : 0x7E00u; break;
                        case 0x60: res = F32ToF16(EvalSinTurnsF32(h0)); break;
                        case 0x61: res = F32ToF16(EvalCosTurnsF32(h0)); break;
                        case 0x62: {
                            int32_t lo = std::clamp(SignExtend32(s0 & 0xFFFFu, 16u), 0, 255);
                            int32_t hi = std::clamp(SignExtend32(s0 >> 16u, 16u), 0, 255);
                            res = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 8u);
                            break;
                        }
                        case 0x63: {
                            float cl = std::isnan(h0) ? 0.0f : std::clamp(h0, -1.0f, 1.0f);
                            res = static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(std::lrint(cl * 32767.0f))));
                            break;
                        }
                        case 0x64: {
                            float cl = std::isnan(h0) ? 0.0f : std::clamp(h0, 0.0f, 1.0f);
                            res = static_cast<uint32_t>(static_cast<uint16_t>(std::lrint(cl * 65535.0f)));
                            break;
                        }
                        case 0x65: {
                            wave.WriteVgpr(l, static_cast<uint8_t>(inst.src[0].reg_index), old_dst);
                            res = s0;
                            break;
                        }
                        default: res = s0; break;
                    }
                    if (write64) {
                        wave.WriteVgpr64(l, static_cast<uint8_t>(inst.dst.reg_index), res64);
                    } else {
                        if (is_float && (inst.clamp || inst.omod != 0u)) {
                            res = FloatToBits(ApplyFloatOutMod(BitsToFloat(res), inst.clamp, inst.omod));
                        }
                        if (mod.kind == SvModifierKind::Sdwa) {
                            res = ApplySdwaDst(old_dst, res, mod.dst_sel, mod.dst_unused, is_float, mod.sdwa_clamp, mod.sdwa_omod);
                        }
                        wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), res);
                    }
                }
            }
        } else if (enc == Rdna2Encoding::VOP2 || (enc == Rdna2Encoding::VOP3 && raw >= 0x100u && raw < 0x140u)) {
            const uint16_t v2op = (enc == Rdna2Encoding::VOP3) ? static_cast<uint16_t>(raw - 0x100u) : raw;
            const bool is_float = (e->data_kind == DK::Float);
            std::array<uint32_t, 64> src0_snap{};
            for (uint32_t l = 0u; l < wave_n; ++l) {
                src0_snap[l] = (mod.kind != SvModifierKind::None)
                                   ? ((mod.kind == SvModifierKind::Sdwa && mod.src0_is_sgpr)
                                          ? wave.ReadSgpr(mod.vsrc0)
                                          : wave.ReadVgpr(l, mod.vsrc0))
                                   : ReadVecOp32(wave, l, inst.src[0], is_float);
            }
            uint64_t new_vcc = 0ull;
            const uint64_t old_vcc = wave.vcc;
            for (uint32_t l = 0u; l < wave_n; ++l) {
                if (!wave.IsLaneActive(l)) continue;
                const uint32_t old_dst = wave.ReadVgpr(l, static_cast<uint8_t>(inst.dst.reg_index));
                uint32_t s0 = src0_snap[l];
                uint32_t s1 = ReadVecOp32(wave, l, inst.src[1], is_float);
                if (mod.kind == SvModifierKind::Sdwa) {
                    s0 = ApplySdwaSrc(s0, mod.src0_sel, mod.src0_sext, is_float, mod.src0_neg, mod.src0_abs);
                    s1 = ApplySdwaSrc(s1, mod.src1_sel, mod.src1_sext, is_float, mod.src1_neg, mod.src1_abs);
                } else if (mod.kind == SvModifierKind::Dpp16) {
                    if (!Dpp16LaneEnabled(l, mod.row_mask, mod.bank_mask)) continue;
                    const int32_t sl = ComputeDpp16SrcLane(l, mod.dpp_ctrl, wave_n);
                    if (sl < 0 || (!mod.dpp_fi && !wave.IsLaneActive(static_cast<uint32_t>(sl)))) {
                        if (!mod.bound_ctrl) continue;
                        s0 = 0u;
                    } else {
                        s0 = src0_snap[static_cast<uint32_t>(sl)];
                    }
                    if (is_float) {
                        if (mod.src0_abs) s0 &= 0x7FFFFFFFu;
                        if (mod.src0_neg) s0 ^= 0x80000000u;
                        if (mod.src1_abs) s1 &= 0x7FFFFFFFu;
                        if (mod.src1_neg) s1 ^= 0x80000000u;
                    }
                } else if (mod.kind == SvModifierKind::Dpp8 || mod.kind == SvModifierKind::Dpp8Fi) {
                    const uint32_t sl = ComputeDpp8SrcLane(l, mod.dpp8_sel);
                    s0 = (sl < wave_n && (mod.dpp_fi || wave.IsLaneActive(sl))) ? src0_snap[sl] : 0u;
                }
                const float f0 = BitsToFloat(s0);
                const float f1 = BitsToFloat(s1);
                const float fd = BitsToFloat(old_dst);
                uint32_t res = 0u;
                switch (v2op) {
                    case 0x01: {
                        uint64_t mask = (enc == Rdna2Encoding::VOP3 && inst.src_count >= 3u)
                                            ? ReadScalarOp64(wave, inst.src[2])
                                            : old_vcc;
                        res = (((mask >> l) & 1ull) != 0ull) ? s1 : s0;
                        break;
                    }
                    case 0x02: {
                        float r = F16ToF32(static_cast<uint16_t>(s0)) * F16ToF32(static_cast<uint16_t>(s1)) +
                                  F16ToF32(static_cast<uint16_t>(s0 >> 16u)) * F16ToF32(static_cast<uint16_t>(s1 >> 16u)) + fd;
                        res = FloatToBits(r);
                        break;
                    }
                    case 0x03: res = FloatToBits(f0 + f1); break;
                    case 0x04: res = FloatToBits(f0 - f1); break;
                    case 0x05: res = FloatToBits(f1 - f0); break;
                    case 0x06: res = FloatToBits(((f0 == 0.0f || f1 == 0.0f) ? 0.0f : (f0 * f1)) + fd); break;
                    case 0x07: res = FloatToBits((f0 == 0.0f || f1 == 0.0f) ? 0.0f : (f0 * f1)); break;
                    case 0x08: res = FloatToBits(f0 * f1); break;
                    case 0x09: res = static_cast<uint32_t>(SignExtend32(s0 & 0xFFFFFFu, 24u) * SignExtend32(s1 & 0xFFFFFFu, 24u)); break;
                    case 0x0A: { int64_t p = static_cast<int64_t>(SignExtend32(s0 & 0xFFFFFFu, 24u)) * SignExtend32(s1 & 0xFFFFFFu, 24u); res = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32u); break; }
                    case 0x0B: res = (s0 & 0xFFFFFFu) * (s1 & 0xFFFFFFu); break;
                    case 0x0C: { uint64_t p = static_cast<uint64_t>(s0 & 0xFFFFFFu) * (s1 & 0xFFFFFFu); res = static_cast<uint32_t>(p >> 32u); break; }
                    case 0x0D: {
                        int32_t acc = static_cast<int32_t>(old_dst);
                        for (uint32_t k = 0u; k < 4u; ++k) {
                            acc += SignExtend32((s0 >> (k * 8u)) & 0xFFu, 8u) * SignExtend32((s1 >> (k * 8u)) & 0xFFu, 8u);
                        }
                        res = static_cast<uint32_t>(acc);
                        break;
                    }
                    case 0x0E: {
                        int32_t acc = static_cast<int32_t>(old_dst);
                        for (uint32_t k = 0u; k < 8u; ++k) {
                            acc += SignExtend32((s0 >> (k * 4u)) & 0xFu, 4u) * SignExtend32((s1 >> (k * 4u)) & 0xFu, 4u);
                        }
                        res = static_cast<uint32_t>(acc);
                        break;
                    }
                    case 0x0F: res = FloatToBits(std::isnan(f0) ? f1 : (std::isnan(f1) ? f0 : std::min(f0, f1))); break;
                    case 0x10: res = FloatToBits(std::isnan(f0) ? f1 : (std::isnan(f1) ? f0 : std::max(f0, f1))); break;
                    case 0x11: res = static_cast<uint32_t>(std::min(static_cast<int32_t>(s0), static_cast<int32_t>(s1))); break;
                    case 0x12: res = static_cast<uint32_t>(std::max(static_cast<int32_t>(s0), static_cast<int32_t>(s1))); break;
                    case 0x13: res = std::min(s0, s1); break;
                    case 0x14: res = std::max(s0, s1); break;
                    case 0x16: res = s1 >> (s0 & 31u); break;
                    case 0x17: res = static_cast<uint32_t>(static_cast<int32_t>(s1) >> (s0 & 31u)); break;
                    case 0x18: res = s1 << (s0 & 31u); break;
                    case 0x1B: res = s0 & s1; break;
                    case 0x1C: res = s0 | s1; break;
                    case 0x1D: res = s0 ^ s1; break;
                    case 0x1E: res = ~(s0 ^ s1); break;
                    case 0x1F: res = FloatToBits(f0 * f1 + fd); break;
                    case 0x21: { float k = BitsToFloat(inst.src[1].literal_bits); float v1 = BitsToFloat(ReadVecOp32(wave, l, inst.src[2], false)); res = FloatToBits(f0 * k + v1); break; }
                    case 0x22: { float k = BitsToFloat(inst.src[2].literal_bits); res = FloatToBits(f0 * f1 + k); break; }
                    case 0x25: res = s0 + s1; break;
                    case 0x26: res = s0 - s1; break;
                    case 0x27: res = s1 - s0; break;
                    case 0x28: { uint64_t ci = (old_vcc >> l) & 1ull; uint64_t sum = static_cast<uint64_t>(s0) + s1 + ci; res = static_cast<uint32_t>(sum); if (sum >> 32u) new_vcc |= (1ull << l); break; }
                    case 0x29: { uint64_t bi = (old_vcc >> l) & 1ull; res = static_cast<uint32_t>(s0 - s1 - bi); if (static_cast<uint64_t>(s1) + bi > s0) new_vcc |= (1ull << l); break; }
                    case 0x2A: { uint64_t bi = (old_vcc >> l) & 1ull; res = static_cast<uint32_t>(s1 - s0 - bi); if (static_cast<uint64_t>(s0) + bi > s1) new_vcc |= (1ull << l); break; }
                    case 0x2B: res = FloatToBits(std::fmaf(f0, f1, fd)); break;
                    case 0x2C: { float k = BitsToFloat(inst.src[1].literal_bits); float v1 = BitsToFloat(ReadVecOp32(wave, l, inst.src[2], false)); res = FloatToBits(std::fmaf(f0, k, v1)); break; }
                    case 0x2D: { float k = BitsToFloat(inst.src[2].literal_bits); res = FloatToBits(std::fmaf(f0, f1, k)); break; }
                    case 0x2F: res = static_cast<uint32_t>(F32ToF16(f0, true)) | (static_cast<uint32_t>(F32ToF16(f1, true)) << 16u); break;
                    case 0x32: res = F32ToF16(F16ToF32(static_cast<uint16_t>(s0)) + F16ToF32(static_cast<uint16_t>(s1))); break;
                    case 0x33: res = F32ToF16(F16ToF32(static_cast<uint16_t>(s0)) - F16ToF32(static_cast<uint16_t>(s1))); break;
                    case 0x34: res = F32ToF16(F16ToF32(static_cast<uint16_t>(s1)) - F16ToF32(static_cast<uint16_t>(s0))); break;
                    case 0x35: res = F32ToF16(F16ToF32(static_cast<uint16_t>(s0)) * F16ToF32(static_cast<uint16_t>(s1))); break;
                    case 0x36: res = F32ToF16(std::fmaf(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), F16ToF32(static_cast<uint16_t>(old_dst)))); break;
                    case 0x37: { float k = F16ToF32(static_cast<uint16_t>(inst.src[1].literal_bits)); float v1 = F16ToF32(static_cast<uint16_t>(ReadVecOp32(wave, l, inst.src[2], false))); res = F32ToF16(std::fmaf(F16ToF32(static_cast<uint16_t>(s0)), k, v1)); break; }
                    case 0x38: { float k = F16ToF32(static_cast<uint16_t>(inst.src[2].literal_bits)); res = F32ToF16(std::fmaf(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), k)); break; }
                    case 0x39: res = F32ToF16(std::max(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)))); break;
                    case 0x3A: res = F32ToF16(std::min(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)))); break;
                    case 0x3B: res = F32ToF16(std::ldexp(F16ToF32(static_cast<uint16_t>(s0)), SignExtend32(s1 & 0xFFFFu, 16u))); break;
                    case 0x3C: {
                        uint16_t lo = F32ToF16(std::fmaf(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), F16ToF32(static_cast<uint16_t>(old_dst))));
                        uint16_t hi = F32ToF16(std::fmaf(F16ToF32(static_cast<uint16_t>(s0 >> 16u)), F16ToF32(static_cast<uint16_t>(s1 >> 16u)), F16ToF32(static_cast<uint16_t>(old_dst >> 16u))));
                        res = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16u);
                        break;
                    }
                    default: break;
                }
                if (is_float && e->bit_width == 32u && (inst.clamp || inst.omod != 0u)) {
                    res = FloatToBits(ApplyFloatOutMod(BitsToFloat(res), inst.clamp, inst.omod));
                }
                if (mod.kind == SvModifierKind::Sdwa) {
                    res = ApplySdwaDst(old_dst, res, mod.dst_sel, mod.dst_unused, is_float, mod.sdwa_clamp, mod.sdwa_omod);
                }
                wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), res);
            }
            if (v2op == 0x28u || v2op == 0x29u || v2op == 0x2Au) {
                wave.vcc = new_vcc;
            }
        } else if (enc == Rdna2Encoding::VOPC || (enc == Rdna2Encoding::VOP3 && raw < 0x100u)) {
            const uint16_t cop = raw;
            uint64_t mask = 0ull;
            for (uint32_t l = 0u; l < wave_n; ++l) {
                if (!wave.IsLaneActive(l)) continue;
                auto read_f16_bits = [&](const Rdna2Operand& op) noexcept -> uint16_t {
                    uint16_t h = static_cast<uint16_t>(ReadVecOp32(wave, l, op, false) & 0xFFFFu);
                    if (op.abs) h = static_cast<uint16_t>(h & 0x7FFFu);
                    if (op.neg) h = static_cast<uint16_t>(h ^ 0x8000u);
                    return h;
                };
                bool r = false;
                if (cop == 0x88u || cop == 0x98u) {
                    r = EvalFloatClassBits32(ReadVecOp32(wave, l, inst.src[0], true), ReadVecOp32(wave, l, inst.src[1], false));
                } else if (cop == 0xA8u || cop == 0xB8u) {
                    r = EvalFloatClassBits64(ReadVecOp64(wave, l, inst.src[0], true), ReadVecOp32(wave, l, inst.src[1], false));
                } else if (cop == 0x8Fu || cop == 0x9Fu) {
                    r = EvalFloatClassBits16(read_f16_bits(inst.src[0]), ReadVecOp32(wave, l, inst.src[1], false));
                } else if ((cop >= 0x10u && cop <= 0x1Fu) || (cop >= 0x30u && cop <= 0x3Fu)) {
                    r = EvalFloatCmp(BitsToFloat(ReadVecOp32(wave, l, inst.src[0], true)), BitsToFloat(ReadVecOp32(wave, l, inst.src[1], true)), cop & 15u);
                } else if ((cop >= 0x20u && cop <= 0x2Fu) || (cop >= 0x40u && cop <= 0x4Fu)) {
                    r = EvalFloatCmp(BitsToDouble(ReadVecOp64(wave, l, inst.src[0], true)), BitsToDouble(ReadVecOp64(wave, l, inst.src[1], true)), cop & 15u);
                } else if ((cop >= 0xC8u && cop <= 0xCFu) || (cop >= 0xD8u && cop <= 0xDFu) ||
                           (cop >= 0xE8u && cop <= 0xEFu) || (cop >= 0xF8u && cop <= 0xFFu)) {
                    const uint32_t cc = ((cop & 0x10u) ? 8u : 0u) | (cop & 7u);
                    r = EvalFloatCmp(F16ToF32(read_f16_bits(inst.src[0])),
                                     F16ToF32(read_f16_bits(inst.src[1])), cc);
                } else if ((cop >= 0x50u && cop <= 0x57u) || (cop >= 0x70u && cop <= 0x77u)) {
                    r = EvalIntCmpSigned(SignExtend32(ReadVecOp32(wave, l, inst.src[0]) & 0xFFFFu, 16u),
                                         SignExtend32(ReadVecOp32(wave, l, inst.src[1]) & 0xFFFFu, 16u), cop & 7u);
                } else if ((cop >= 0x60u && cop <= 0x67u) || (cop >= 0x78u && cop <= 0x7Fu)) {
                    r = EvalIntCmpUnsigned(ReadVecOp32(wave, l, inst.src[0]) & 0xFFFFu,
                                           ReadVecOp32(wave, l, inst.src[1]) & 0xFFFFu, cop & 7u);
                } else if ((cop >= 0x80u && cop <= 0x87u) || (cop >= 0x90u && cop <= 0x97u)) {
                    r = EvalIntCmpSigned(static_cast<int32_t>(ReadVecOp32(wave, l, inst.src[0])),
                                         static_cast<int32_t>(ReadVecOp32(wave, l, inst.src[1])), cop & 7u);
                } else if ((cop >= 0xA0u && cop <= 0xA7u) || (cop >= 0xB0u && cop <= 0xB7u)) {
                    r = EvalIntCmpSigned(static_cast<int64_t>(ReadVecOp64(wave, l, inst.src[0])),
                                         static_cast<int64_t>(ReadVecOp64(wave, l, inst.src[1])), cop & 7u);
                } else if ((cop >= 0xC0u && cop <= 0xC7u) || (cop >= 0xD0u && cop <= 0xD7u)) {
                    r = EvalIntCmpUnsigned(ReadVecOp32(wave, l, inst.src[0]), ReadVecOp32(wave, l, inst.src[1]), cop & 7u);
                } else if ((cop >= 0xE0u && cop <= 0xE7u) || (cop >= 0xF0u && cop <= 0xF7u)) {
                    r = EvalIntCmpUnsigned(ReadVecOp64(wave, l, inst.src[0]), ReadVecOp64(wave, l, inst.src[1]), cop & 7u);
                }
                if (r) mask |= (1ull << l);
            }
            if (e->writes_exec) {
                wave.exec = mask;
            } else if (enc == Rdna2Encoding::VOP3) {
                if (wave.wave_size == 64u) WriteScalarDst64(wave, inst.dst, mask);
                else WriteScalarDst32(wave, inst.dst, static_cast<uint32_t>(mask));
            } else {
                wave.vcc = mask;
            }
        } else if (enc == Rdna2Encoding::VOP3) {
            if (raw == 0x360u) {
                // v_readlane_b32 sdst, vsrc0, ssrc1
                const uint32_t lane = ReadScalarOp32(wave, inst.src[1]) & (wave_n - 1u);
                WriteScalarDst32(wave, inst.dst, ReadVecOp32(wave, lane, inst.src[0], false));
            } else if (raw == 0x361u) {
                // v_writelane_b32 vdst, ssrc0, ssrc1
                const uint32_t val = ReadScalarOp32(wave, inst.src[0]);
                const uint32_t lane = ReadScalarOp32(wave, inst.src[1]) & (wave_n - 1u);
                wave.WriteVgpr(lane, static_cast<uint8_t>(inst.dst.reg_index), val);
            } else if (raw == 0x377u || raw == 0x378u) {
                // v_permlane16_b32 / v_permlanex16_b32
                const bool cross = (raw == 0x378u);
                const uint32_t s1 = ReadScalarOp32(wave, inst.src[1]);
                const uint32_t s2 = ReadScalarOp32(wave, inst.src[2]);
                std::array<uint32_t, 64> snap{};
                for (uint32_t l = 0u; l < wave_n; ++l) snap[l] = ReadVecOp32(wave, l, inst.src[0], false);
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    if (!wave.IsLaneActive(l)) continue;
                    const uint32_t row_lane = l & 15u;
                    const uint32_t row_base = cross ? ((l & ~15u) ^ 16u) : (l & ~15u);
                    const uint32_t w_sel = (row_lane < 8u) ? s1 : s2;
                    const uint32_t sel = (w_sel >> ((row_lane & 7u) * 4u)) & 15u;
                    const uint32_t src_l = (row_base | sel) & (wave_n - 1u);
                    wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), snap[src_l]);
                }
            } else {
                uint64_t carry_out = 0ull;
                const bool is_float = (e->data_kind == DK::Float);
                for (uint32_t l = 0u; l < wave_n; ++l) {
                    if (!wave.IsLaneActive(l)) continue;
                    const uint32_t s0 = ReadVecOp32(wave, l, inst.src[0], is_float);
                    const uint32_t s1 = (inst.src_count >= 2u) ? ReadVecOp32(wave, l, inst.src[1], is_float) : 0u;
                    const uint32_t s2 = (inst.src_count >= 3u) ? ReadVecOp32(wave, l, inst.src[2], is_float) : 0u;
                    const float f0 = BitsToFloat(s0), f1 = BitsToFloat(s1), f2 = BitsToFloat(s2);
                    const uint64_t u0_64 = (e->bit_width == 64u) ? ReadVecOp64(wave, l, inst.src[0], is_float) : 0ull;
                    const uint64_t u1_64 = (e->bit_width == 64u && inst.src_count >= 2u) ? ReadVecOp64(wave, l, inst.src[1], is_float) : 0ull;
                    const uint64_t u2_64 = (e->bit_width == 64u && inst.src_count >= 3u) ? ReadVecOp64(wave, l, inst.src[2], is_float) : 0ull;
                    const double d0 = BitsToDouble(u0_64), d1 = BitsToDouble(u1_64), d2 = BitsToDouble(u2_64);
                    uint32_t res = 0u;
                    uint64_t res64 = 0ull;
                    bool write64 = false;

                    switch (raw) {
                        case 0x140: res = FloatToBits(((f0 == 0.0f || f1 == 0.0f) ? 0.0f : (f0 * f1)) + f2); break;
                        case 0x141: res = FloatToBits(f0 * f1 + f2); break;
                        case 0x142: res = static_cast<uint32_t>(SignExtend32(s0 & 0xFFFFFFu, 24u) * SignExtend32(s1 & 0xFFFFFFu, 24u) + static_cast<int32_t>(s2)); break;
                        case 0x143: res = (s0 & 0xFFFFFFu) * (s1 & 0xFFFFFFu) + s2; break;
                        case 0x144: {
                            float ax = std::fabs(f0), ay = std::fabs(f1), az = std::fabs(f2);
                            float fid = (az >= ax && az >= ay) ? (f2 < 0.0f ? 5.0f : 4.0f)
                                        : ((ay >= ax) ? (f1 < 0.0f ? 3.0f : 2.0f) : (f0 < 0.0f ? 1.0f : 0.0f));
                            res = FloatToBits(fid);
                            break;
                        }
                        case 0x145: {
                            float ax = std::fabs(f0), ay = std::fabs(f1), az = std::fabs(f2);
                            float sc = (az >= ax && az >= ay) ? (f2 < 0.0f ? -f0 : f0)
                                       : ((ay >= ax) ? f0 : (f0 < 0.0f ? f2 : -f2));
                            res = FloatToBits(sc);
                            break;
                        }
                        case 0x146: {
                            float ax = std::fabs(f0), ay = std::fabs(f1), az = std::fabs(f2);
                            float tc = (az >= ax && az >= ay) ? -f1 : ((ay >= ax) ? (f1 < 0.0f ? -f2 : f2) : -f1);
                            res = FloatToBits(tc);
                            break;
                        }
                        case 0x147: res = FloatToBits(2.0f * std::max({std::fabs(f0), std::fabs(f1), std::fabs(f2)})); break;
                        case 0x148: res = EvalBfeU32(s0, s1, s2); break;
                        case 0x149: res = static_cast<uint32_t>(EvalBfeI32(s0, s1, s2)); break;
                        case 0x14A: res = (s0 & s1) | (~s0 & s2); break;
                        case 0x14B: res = FloatToBits(std::fma(f0, f1, f2)); break;
                        case 0x14C: { uint32_t sh = (s2 & 3u) * 8u; res = (sh == 0u) ? s1 : ((s1 >> sh) | (s0 << (32u - sh))); break; }
                        case 0x14D: res64 = DoubleToBits(std::fma(d0, d1, d2)); write64 = true; break;
                        case 0x14E: res = EvalLerpU8(s0, s1, s2); break;
                        case 0x14F: { uint32_t sh = s2 & 31u; res = (sh == 0u) ? s1 : ((s1 >> sh) | (s0 << (32u - sh))); break; }
                        case 0x150: res = FloatToBits((f0 > 0.0f) ? (f0 * f1 + f2) : f2); break;
                        case 0x151: res = FloatToBits(std::min({f0, f1, f2})); break;
                        case 0x152: res = static_cast<uint32_t>(std::min({static_cast<int32_t>(s0), static_cast<int32_t>(s1), static_cast<int32_t>(s2)})); break;
                        case 0x153: res = std::min({s0, s1, s2}); break;
                        case 0x154: res = FloatToBits(std::max({f0, f1, f2})); break;
                        case 0x155: res = static_cast<uint32_t>(std::max({static_cast<int32_t>(s0), static_cast<int32_t>(s1), static_cast<int32_t>(s2)})); break;
                        case 0x156: res = std::max({s0, s1, s2}); break;
                        case 0x157: res = FloatToBits(std::max(std::min(f0, f1), std::min(std::max(f0, f1), f2))); break;
                        case 0x158: { int32_t a = static_cast<int32_t>(s0), b = static_cast<int32_t>(s1), c = static_cast<int32_t>(s2); res = static_cast<uint32_t>(std::max(std::min(a, b), std::min(std::max(a, b), c))); break; }
                        case 0x159: res = std::max(std::min(s0, s1), std::min(std::max(s0, s1), s2)); break;
                        case 0x15A: res = EvalSadU8(s0, s1, s2); break;
                        case 0x15B: res = s2 + (EvalSadU8(s0, s1, 0u) << 16u); break;
                        case 0x15C: {
                            uint32_t a0 = s0 & 0xFFFFu, a1 = s0 >> 16u, b0 = s1 & 0xFFFFu, b1 = s1 >> 16u;
                            res = s2 + (a0 >= b0 ? a0 - b0 : b0 - a0) + (a1 >= b1 ? a1 - b1 : b1 - a1);
                            break;
                        }
                        case 0x15D: res = s2 + (s0 >= s1 ? s0 - s1 : s1 - s0); break;
                        case 0x15E: {
                            uint32_t byte_v = static_cast<uint32_t>(std::clamp(f0, 0.0f, 255.0f));
                            uint32_t sh = (s1 & 3u) * 8u;
                            res = (s2 & ~(0xFFu << sh)) | ((byte_v & 0xFFu) << sh);
                            break;
                        }
                        case 0x15F: res = FloatToBits(EvalDivFixupF32(f0, f1, f2)); break;
                        case 0x160: res64 = DoubleToBits(EvalDivFixupF64(d0, d1, d2)); write64 = true; break;
                        case 0x161: res = FloatToBits(std::ldexp(f0, static_cast<int32_t>(s1))); break;
                        case 0x162: {
                            float r = std::fma(f0, f1, f2);
                            if (((wave.vcc >> l) & 1ull) != 0ull) {
                                const uint32_t exp_c = (FloatToBits(f2) >> 23u) & 0xFFu;
                                r = std::ldexp(r, (exp_c > 127u) ? 64 : -64);
                            }
                            res = FloatToBits(r);
                            break;
                        }
                        case 0x163: res64 = DoubleToBits(std::fma(d0, d1, d2)); write64 = true; break;
                        case 0x164: res = EvalMsadU8(s0, s1, s2); break;
                        case 0x165: case 0x166: res64 = u2_64 + static_cast<uint64_t>(EvalSadU8(static_cast<uint32_t>(u0_64), s1, 0u)); write64 = true; break;
                        case 0x167: res64 = DoubleToBits(d0); write64 = true; break;
                        case 0x168: res = EvalMsadU8(s0, s1, s2); break;
                        case 0x169: res = s0 * s1; break;
                        case 0x16A: res = static_cast<uint32_t>((static_cast<uint64_t>(s0) * s1) >> 32u); break;
                        case 0x16B: res = static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s0)) * static_cast<int32_t>(s1)) >> 32u); break;
                        case 0x16C: { uint64_t p = static_cast<uint64_t>(s0) * s1; res64 = p + u2_64; if (res64 < p) carry_out |= (1ull << l); write64 = true; break; }
                        case 0x16D: { int64_t p = static_cast<int64_t>(static_cast<int32_t>(s0)) * static_cast<int32_t>(s1); res64 = static_cast<uint64_t>(p + static_cast<int64_t>(u2_64)); write64 = true; break; }
                        case 0x16E: res = FloatToBits(f0); break;
                        case 0x16F: res64 = DoubleToBits(d0); write64 = true; break;
                        case 0x170: res64 = DoubleToBits(d0 + d1); write64 = true; break;
                        case 0x171: res64 = DoubleToBits(d0 * d1); write64 = true; break;
                        case 0x172: res64 = DoubleToBits(std::min(d0, d1)); write64 = true; break;
                        case 0x173: res64 = DoubleToBits(std::max(d0, d1)); write64 = true; break;
                        case 0x174: res64 = DoubleToBits(std::ldexp(d0, static_cast<int32_t>(s1))); write64 = true; break;
                        case 0x175: res64 = u1_64 << (s0 & 63u); write64 = true; break;
                        case 0x176: res64 = u1_64 >> (s0 & 63u); write64 = true; break;
                        case 0x177: res64 = static_cast<uint64_t>(static_cast<int64_t>(u1_64) >> (s0 & 63u)); write64 = true; break;
                        case 0x178: res = static_cast<uint32_t>(std::popcount(s0)) + s1; break;
                        case 0x179: res = FloatToBits((f0 == 0.0f || f1 == 0.0f) ? f2 : std::fma(f0, f1, f2)); break;
                        case 0x274: case 0x275: case 0x276: res = s0; break;
                        case 0x303: res = (s0 + s1) & 0xFFFFu; break;
                        case 0x304: res = (s0 - s1) & 0xFFFFu; break;
                        case 0x305: {
                            uint32_t lm = (l < 32u) ? ((1u << l) - 1u) : 0xFFFFFFFFu;
                            res = static_cast<uint32_t>(std::popcount(s0 & lm)) + s1;
                            break;
                        }
                        case 0x306: {
                            uint32_t hm = (l >= 32u) ? ((1u << (l - 32u)) - 1u) : 0u;
                            res = static_cast<uint32_t>(std::popcount(s0 & hm)) + s1;
                            break;
                        }
                        case 0x307: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(s0 & 0xFFFFu, 16u) + SignExtend32(s1 & 0xFFFFu, 16u))); break;
                        case 0x308: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(s0 & 0xFFFFu, 16u) - SignExtend32(s1 & 0xFFFFu, 16u))); break;
                        case 0x309: res = std::min(s0 & 0xFFFFu, s1 & 0xFFFFu); break;
                        case 0x30A: res = std::max(s0 & 0xFFFFu, s1 & 0xFFFFu); break;
                        case 0x30B: res = static_cast<uint32_t>(static_cast<uint16_t>(std::min(SignExtend32(s0 & 0xFFFFu, 16u), SignExtend32(s1 & 0xFFFFu, 16u)))); break;
                        case 0x30C: res = static_cast<uint32_t>(static_cast<uint16_t>(std::max(SignExtend32(s0 & 0xFFFFu, 16u), SignExtend32(s1 & 0xFFFFu, 16u)))); break;
                        case 0x30D: { uint64_t sum = static_cast<uint64_t>(s0) + s1; res = static_cast<uint32_t>(sum); if (sum >> 32u) carry_out |= (1ull << l); break; }
                        case 0x30E: { res = s0 - s1; if (s1 > s0) carry_out |= (1ull << l); break; }
                        case 0x30F: { res = s1 - s0; if (s0 > s1) carry_out |= (1ull << l); break; }
                        case 0x311: res = (s0 & 0xFFFFu) | ((s1 & 0xFFFFu) << 16u); break;
                        case 0x312: res = ((s0 & 0xFFFFu) * (s1 & 0xFFFFu)) & 0xFFFFu; break;
                        case 0x313: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(s1 & 0xFFFFu, 16u) >> (s0 & 15u))); break;
                        case 0x314: res = ((s1 & 0xFFFFu) << (s0 & 15u)) & 0xFFFFu; break;
                        case 0x315: res = (s1 & 0xFFFFu) >> (s0 & 15u); break;
                        case 0x340: res = ((s0 & 0xFFFFu) * (s1 & 0xFFFFu) + (s2 & 0xFFFFu)) & 0xFFFFu; break;
                        case 0x341: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(s0 & 0xFFFFu, 16u) * SignExtend32(s1 & 0xFFFFu, 16u) + SignExtend32(s2 & 0xFFFFu, 16u))); break;
                        case 0x344: res = EvalPermB32(s0, s1, s2); break;
                        case 0x34B: res = F32ToF16(std::fma(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), F16ToF32(static_cast<uint16_t>(s2)))); break;
                        case 0x34C: res = F32ToF16(EvalDivFixupF32(F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), F16ToF32(static_cast<uint16_t>(s2)))); break;
                        case 0x34D: res = F32ToF16(std::min({F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), F16ToF32(static_cast<uint16_t>(s2))})); break;
                        case 0x34E: res = static_cast<uint32_t>(static_cast<uint16_t>(std::min({SignExtend32(s0 & 0xFFFFu, 16u), SignExtend32(s1 & 0xFFFFu, 16u), SignExtend32(s2 & 0xFFFFu, 16u)}))); break;
                        case 0x34F: res = std::min({s0 & 0xFFFFu, s1 & 0xFFFFu, s2 & 0xFFFFu}); break;
                        case 0x350: res = F32ToF16(std::max({F16ToF32(static_cast<uint16_t>(s0)), F16ToF32(static_cast<uint16_t>(s1)), F16ToF32(static_cast<uint16_t>(s2))})); break;
                        case 0x351: res = static_cast<uint32_t>(static_cast<uint16_t>(std::max({SignExtend32(s0 & 0xFFFFu, 16u), SignExtend32(s1 & 0xFFFFu, 16u), SignExtend32(s2 & 0xFFFFu, 16u)}))); break;
                        case 0x352: res = std::max({s0 & 0xFFFFu, s1 & 0xFFFFu, s2 & 0xFFFFu}); break;
                        case 0x353: { float a = F16ToF32(static_cast<uint16_t>(s0)), b = F16ToF32(static_cast<uint16_t>(s1)), c = F16ToF32(static_cast<uint16_t>(s2)); res = F32ToF16(std::max(std::min(a, b), std::min(std::max(a, b), c))); break; }
                        case 0x354: { int32_t a = SignExtend32(s0 & 0xFFFFu, 16u), b = SignExtend32(s1 & 0xFFFFu, 16u), c = SignExtend32(s2 & 0xFFFFu, 16u); res = static_cast<uint32_t>(static_cast<uint16_t>(std::max(std::min(a, b), std::min(std::max(a, b), c)))); break; }
                        case 0x355: { uint32_t a = s0 & 0xFFFFu, b = s1 & 0xFFFFu, c = s2 & 0xFFFFu; res = std::max(std::min(a, b), std::min(std::max(a, b), c)); break; }
                        case 0x368: {
                            int16_t lo = static_cast<int16_t>(std::lrint(std::clamp(f0, -1.0f, 1.0f) * 32767.0f));
                            int16_t hi = static_cast<int16_t>(std::lrint(std::clamp(f1, -1.0f, 1.0f) * 32767.0f));
                            res = static_cast<uint32_t>(static_cast<uint16_t>(lo)) | (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16u);
                            break;
                        }
                        case 0x369: {
                            uint16_t lo = static_cast<uint16_t>(std::lrint(std::clamp(f0, 0.0f, 1.0f) * 65535.0f));
                            uint16_t hi = static_cast<uint16_t>(std::lrint(std::clamp(f1, 0.0f, 1.0f) * 65535.0f));
                            res = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16u);
                            break;
                        }
                        case 0x36A: res = std::min(s0, 0xFFFFu) | (std::min(s1, 0xFFFFu) << 16u); break;
                        case 0x36B: {
                            int16_t lo = static_cast<int16_t>(std::clamp(static_cast<int32_t>(s0), -32768, 32767));
                            int16_t hi = static_cast<int16_t>(std::clamp(static_cast<int32_t>(s1), -32768, 32767));
                            res = static_cast<uint32_t>(static_cast<uint16_t>(lo)) | (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16u);
                            break;
                        }
                        case 0x36C: res = static_cast<uint32_t>(F32ToF16(f0)) | (static_cast<uint32_t>(F32ToF16(f1)) << 16u); break;
                        case 0x36D: res = s0 + s1 + s2; break;
                        case 0x36E: res = (s0 << (s1 & 31u)) | s2; break;
                        case 0x36F: res = (s0 & s1) | s2; break;
                        case 0x370: res = s0 | s1 | s2; break;
                        case 0x371: res = (s0 ^ s1) + s2; break;
                        case 0x372: res = (s0 << (s1 & 31u)) + s2; break;
                        case 0x373: {
                            float r = F16ToF32(static_cast<uint16_t>(s0)) * F16ToF32(static_cast<uint16_t>(s1)) +
                                      F16ToF32(static_cast<uint16_t>(s0 >> 16u)) * F16ToF32(static_cast<uint16_t>(s1 >> 16u)) + f2;
                            res = FloatToBits(r);
                            break;
                        }
                        case 0x374: res = (s0 + s1) << (s2 & 31u); break;
                        default: break;
                    }
                    if (write64) {
                        wave.WriteVgpr64(l, static_cast<uint8_t>(inst.dst.reg_index), res64);
                    } else {
                        if (is_float && e->bit_width == 32u && (inst.clamp || inst.omod != 0u)) {
                            res = FloatToBits(ApplyFloatOutMod(BitsToFloat(res), inst.clamp, inst.omod));
                        }
                        wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), res);
                    }
                }
                if (IsVop3CarryOutOpcode(raw)) {
                    const uint8_t sdst = inst.soffset ? inst.soffset : 106u;
                    if (wave.wave_size == 64u) wave.WriteSgpr64(sdst, carry_out);
                    else wave.WriteSgpr(sdst, static_cast<uint32_t>(carry_out));
                }
            }
        } else if (enc == Rdna2Encoding::VOP3P) {
            auto pick_half = [](uint32_t v, bool hi) noexcept -> uint16_t {
                return static_cast<uint16_t>(hi ? (v >> 16u) : (v & 0xFFFFu));
            };
            for (uint32_t l = 0u; l < wave_n; ++l) {
                if (!wave.IsLaneActive(l)) continue;
                const uint32_t s0 = ReadVecOp32(wave, l, inst.src[0], false);
                const uint32_t s1 = (inst.src_count >= 2u) ? ReadVecOp32(wave, l, inst.src[1], false) : 0u;
                const uint32_t s2 = (inst.src_count >= 3u) ? ReadVecOp32(wave, l, inst.src[2], false) : 0u;
                const uint16_t a0 = pick_half(s0, (inst.vop3p_op_sel & 1u) != 0u);
                const uint16_t a1 = pick_half(s0, (inst.vop3p_op_sel_hi & 1u) != 0u);
                const uint16_t b0 = pick_half(s1, (inst.vop3p_op_sel & 2u) != 0u);
                const uint16_t b1 = pick_half(s1, (inst.vop3p_op_sel_hi & 2u) != 0u);
                const uint16_t c0 = pick_half(s2, (inst.vop3p_op_sel & 4u) != 0u);
                const uint16_t c1 = pick_half(s2, (inst.vop3p_op_sel_hi & 4u) != 0u);
                auto f16_arg = [&](uint16_t h, uint8_t neg_mask, uint8_t bit) noexcept -> float {
                    float f = F16ToF32(h);
                    return ((neg_mask >> bit) & 1u) ? -f : f;
                };
                uint32_t res = 0u;
                switch (raw) {
                    case 0x00: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(a0, 16u) * SignExtend32(b0, 16u) + SignExtend32(c0, 16u))) | (static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(a1, 16u) * SignExtend32(b1, 16u) + SignExtend32(c1, 16u))) << 16u); break;
                    case 0x01: res = ((static_cast<uint32_t>(a0) * b0) & 0xFFFFu) | (((static_cast<uint32_t>(a1) * b1) & 0xFFFFu) << 16u); break;
                    case 0x02: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(a0, 16u) + SignExtend32(b0, 16u))) | (static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(a1, 16u) + SignExtend32(b1, 16u))) << 16u); break;
                    case 0x03: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(a0, 16u) - SignExtend32(b0, 16u))) | (static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(a1, 16u) - SignExtend32(b1, 16u))) << 16u); break;
                    case 0x04: res = ((static_cast<uint32_t>(b0) << (a0 & 15u)) & 0xFFFFu) | (((static_cast<uint32_t>(b1) << (a1 & 15u)) & 0xFFFFu) << 16u); break;
                    case 0x05: res = (static_cast<uint32_t>(b0) >> (a0 & 15u)) | ((static_cast<uint32_t>(b1) >> (a1 & 15u)) << 16u); break;
                    case 0x06: res = static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(b0, 16u) >> (a0 & 15u))) | (static_cast<uint32_t>(static_cast<uint16_t>(SignExtend32(b1, 16u) >> (a1 & 15u))) << 16u); break;
                    case 0x07: res = static_cast<uint32_t>(static_cast<uint16_t>(std::max(SignExtend32(a0, 16u), SignExtend32(b0, 16u)))) | (static_cast<uint32_t>(static_cast<uint16_t>(std::max(SignExtend32(a1, 16u), SignExtend32(b1, 16u)))) << 16u); break;
                    case 0x08: res = static_cast<uint32_t>(static_cast<uint16_t>(std::min(SignExtend32(a0, 16u), SignExtend32(b0, 16u)))) | (static_cast<uint32_t>(static_cast<uint16_t>(std::min(SignExtend32(a1, 16u), SignExtend32(b1, 16u)))) << 16u); break;
                    case 0x09: res = ((static_cast<uint32_t>(a0) * b0 + c0) & 0xFFFFu) | (((static_cast<uint32_t>(a1) * b1 + c1) & 0xFFFFu) << 16u); break;
                    case 0x0A: res = ((static_cast<uint32_t>(a0) + b0) & 0xFFFFu) | (((static_cast<uint32_t>(a1) + b1) & 0xFFFFu) << 16u); break;
                    case 0x0B: res = ((static_cast<uint32_t>(a0) - b0) & 0xFFFFu) | (((static_cast<uint32_t>(a1) - b1) & 0xFFFFu) << 16u); break;
                    case 0x0C: res = std::max<uint32_t>(a0, b0) | (std::max<uint32_t>(a1, b1) << 16u); break;
                    case 0x0D: res = std::min<uint32_t>(a0, b0) | (std::min<uint32_t>(a1, b1) << 16u); break;
                    case 0x0E: res = static_cast<uint32_t>(F32ToF16(std::fma(f16_arg(a0, inst.vop3p_neg_lo, 0), f16_arg(b0, inst.vop3p_neg_lo, 1), f16_arg(c0, inst.vop3p_neg_lo, 2)))) | (static_cast<uint32_t>(F32ToF16(std::fma(f16_arg(a1, inst.vop3p_neg_hi, 0), f16_arg(b1, inst.vop3p_neg_hi, 1), f16_arg(c1, inst.vop3p_neg_hi, 2)))) << 16u); break;
                    case 0x0F: res = static_cast<uint32_t>(F32ToF16(f16_arg(a0, inst.vop3p_neg_lo, 0) + f16_arg(b0, inst.vop3p_neg_lo, 1))) | (static_cast<uint32_t>(F32ToF16(f16_arg(a1, inst.vop3p_neg_hi, 0) + f16_arg(b1, inst.vop3p_neg_hi, 1))) << 16u); break;
                    case 0x10: res = static_cast<uint32_t>(F32ToF16(f16_arg(a0, inst.vop3p_neg_lo, 0) * f16_arg(b0, inst.vop3p_neg_lo, 1))) | (static_cast<uint32_t>(F32ToF16(f16_arg(a1, inst.vop3p_neg_hi, 0) * f16_arg(b1, inst.vop3p_neg_hi, 1))) << 16u); break;
                    case 0x11: res = static_cast<uint32_t>(F32ToF16(std::min(f16_arg(a0, inst.vop3p_neg_lo, 0), f16_arg(b0, inst.vop3p_neg_lo, 1)))) | (static_cast<uint32_t>(F32ToF16(std::min(f16_arg(a1, inst.vop3p_neg_hi, 0), f16_arg(b1, inst.vop3p_neg_hi, 1)))) << 16u); break;
                    case 0x12: res = static_cast<uint32_t>(F32ToF16(std::max(f16_arg(a0, inst.vop3p_neg_lo, 0), f16_arg(b0, inst.vop3p_neg_lo, 1)))) | (static_cast<uint32_t>(F32ToF16(std::max(f16_arg(a1, inst.vop3p_neg_hi, 0), f16_arg(b1, inst.vop3p_neg_hi, 1)))) << 16u); break;
                    case 0x13: res = static_cast<uint32_t>(SignExtend32(s0 & 0xFFFFu, 16u) * SignExtend32(s1 & 0xFFFFu, 16u) + SignExtend32(s0 >> 16u, 16u) * SignExtend32(s1 >> 16u, 16u) + static_cast<int32_t>(s2)); break;
                    case 0x14: res = (s0 & 0xFFFFu) * (s1 & 0xFFFFu) + (s0 >> 16u) * (s1 >> 16u) + s2; break;
                    case 0x15: {
                        int32_t acc = static_cast<int32_t>(s2);
                        for (uint32_t k = 0u; k < 4u; ++k) acc += SignExtend32((s0 >> (k * 8u)) & 0xFFu, 8u) * SignExtend32((s1 >> (k * 8u)) & 0xFFu, 8u);
                        res = static_cast<uint32_t>(acc);
                        break;
                    }
                    case 0x16: {
                        uint32_t acc = s2;
                        for (uint32_t k = 0u; k < 4u; ++k) acc += ((s0 >> (k * 8u)) & 0xFFu) * ((s1 >> (k * 8u)) & 0xFFu);
                        res = acc;
                        break;
                    }
                    case 0x17: {
                        int32_t acc = static_cast<int32_t>(s2);
                        for (uint32_t k = 0u; k < 8u; ++k) acc += SignExtend32((s0 >> (k * 4u)) & 0xFu, 4u) * SignExtend32((s1 >> (k * 4u)) & 0xFu, 4u);
                        res = static_cast<uint32_t>(acc);
                        break;
                    }
                    case 0x18: {
                        uint32_t acc = s2;
                        for (uint32_t k = 0u; k < 8u; ++k) acc += ((s0 >> (k * 4u)) & 0xFu) * ((s1 >> (k * 4u)) & 0xFu);
                        res = acc;
                        break;
                    }
                    case 0x20: case 0x21: case 0x22: {
                        auto mix_f = [&](uint32_t v, uint8_t bit) noexcept -> float {
                            const bool is_f16 = ((inst.vop3p_op_sel_hi >> bit) & 1u) != 0u;
                            const bool hi = ((inst.vop3p_op_sel >> bit) & 1u) != 0u;
                            float f = is_f16 ? F16ToF32(pick_half(v, hi)) : BitsToFloat(v);
                            if ((inst.vop3p_neg_lo >> bit) & 1u) f = std::fabs(f);
                            if ((inst.vop3p_neg_hi >> bit) & 1u) f = -f;
                            return f;
                        };
                        const float r = std::fmaf(mix_f(s0, 0u), mix_f(s1, 1u), mix_f(s2, 2u));
                        if (raw == 0x20u) res = FloatToBits(r);
                        else if (raw == 0x21u) res = (wave.ReadVgpr(l, static_cast<uint8_t>(inst.dst.reg_index)) & 0xFFFF0000u) | F32ToF16(r);
                        else res = (wave.ReadVgpr(l, static_cast<uint8_t>(inst.dst.reg_index)) & 0x0000FFFFu) | (static_cast<uint32_t>(F32ToF16(r)) << 16u);
                        break;
                    }
                    default: break;
                }
                wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), res);
            }
        } else if (enc == Rdna2Encoding::VINTRP) {
            for (uint32_t l = 0u; l < wave_n; ++l) {
                if (!wave.IsLaneActive(l)) continue;
                const uint32_t s0 = ReadVecOp32(wave, l, inst.src[0], false);
                wave.WriteVgpr(l, static_cast<uint8_t>(inst.dst.reg_index), s0);
            }
        }

        idx = next_idx;
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Dual-Target Lifter: SM 6.5 HLSL + SPIR-V 1.3
// ---------------------------------------------------------------------------
namespace {

uint64_t HashFnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0u; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

struct SvShaderUsage {
    uint32_t max_sgpr = 8u;
    uint32_t max_vgpr = 8u;
    bool uses_uav0 = false;
    bool uses_uav1 = false;
    bool uses_root_const = false;
    bool uses_cross_lane = false;
    bool uses_branches = false;
};

SvShaderUsage AnalyzeSvUsage(const std::vector<SvDecodedInst>& insts) {
    SvShaderUsage u{};
    auto track_op = [&](const Rdna2Operand& op) {
        if (op.kind == Rdna2OperandKind::Sgpr) {
            u.max_sgpr = std::max<uint32_t>(u.max_sgpr, static_cast<uint32_t>(op.reg_index) + 2u);
        } else if (op.kind == Rdna2OperandKind::Vgpr) {
            u.max_vgpr = std::max<uint32_t>(u.max_vgpr, static_cast<uint32_t>(op.reg_index) + 2u);
        }
    };
    for (const auto& sv : insts) {
        const auto& inst = sv.base;
        track_op(inst.dst);
        for (uint8_t i = 0u; i < inst.src_count && i < 4u; ++i) track_op(inst.src[i]);
        if (sv.modifier.kind != SvModifierKind::None) {
            if (sv.modifier.kind == SvModifierKind::Dpp16 ||
                sv.modifier.kind == SvModifierKind::Dpp8 ||
                sv.modifier.kind == SvModifierKind::Dpp8Fi) {
                u.uses_cross_lane = true;
            }
            if (sv.modifier.kind == SvModifierKind::Sdwa && sv.modifier.src0_is_sgpr) {
                u.max_sgpr = std::max<uint32_t>(u.max_sgpr, static_cast<uint32_t>(sv.modifier.vsrc0) + 2u);
            } else {
                u.max_vgpr = std::max<uint32_t>(u.max_vgpr, static_cast<uint32_t>(sv.modifier.vsrc0) + 2u);
            }
        }
        if (!sv.entry) {
            if (inst.encoding == Rdna2Encoding::MUBUF) {
                if (inst.srsrc == 0u) u.uses_uav0 = true;
                else u.uses_uav1 = true;
            } else if (inst.encoding == Rdna2Encoding::FLAT) {
                if (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD || inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD) u.uses_uav1 = true;
                else u.uses_uav0 = true;
            }
            continue;
        }
        if (inst.encoding == Rdna2Encoding::SMEM) {
            u.uses_root_const = true;
            u.max_sgpr = std::max<uint32_t>(u.max_sgpr, static_cast<uint32_t>(inst.dst.reg_index) + 16u);
        } else if (inst.encoding == Rdna2Encoding::SOPP &&
                   (inst.raw_opcode == 0x02u || (inst.raw_opcode >= 0x04u && inst.raw_opcode <= 0x09u))) {
            u.uses_branches = true;
        } else if ((inst.encoding == Rdna2Encoding::VOP1 && inst.raw_opcode == 0x02u) ||
                   (inst.encoding == Rdna2Encoding::VOP3 &&
                    (inst.raw_opcode == 0x182u || inst.raw_opcode == 0x360u ||
                     inst.raw_opcode == 0x377u || inst.raw_opcode == 0x378u))) {
            u.uses_cross_lane = true;
        }
    }
    u.max_sgpr = std::min<uint32_t>(u.max_sgpr, 64u);
    u.max_vgpr = std::min<uint32_t>(u.max_vgpr, 64u);
    return u;
}

std::string HexLitU32(uint32_t v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return buf;
}

// ---------------------------------------------------------------------------
// SM 6.5 HLSL Emitter
// ---------------------------------------------------------------------------
Status EmitSvHlsl(
    const Xs2ShaderContainer& container,
    const std::vector<SvDecodedInst>& insts,
    const SvShaderUsage& usage,
    std::string& out_hlsl,
    std::string& out_target,
    bool use_sm65) {
    const uint32_t tg_x = container.threadgroup_x ? container.threadgroup_x : 1u;
    const uint32_t tg_y = container.threadgroup_y ? container.threadgroup_y : 1u;
    const uint32_t tg_z = container.threadgroup_z ? container.threadgroup_z : 1u;
    const uint32_t wave_n = (container.wave_size == 64u) ? 64u : 32u;
    const uint32_t lane_mask = wave_n - 1u;
    const uint32_t tg_total = std::max(wave_n, tg_x * tg_y * tg_z);

    std::ostringstream os;
    os << "// Auto-lifted by OpenScarlett RDNA2 Scalar+Vector Lifter (GFX10.3)\n";
    if (usage.uses_uav0) os << "RWByteAddressBuffer _uav0 : register(u0);\n";
    if (usage.uses_uav1) os << "RWByteAddressBuffer _uav1 : register(u1);\n";
    if (usage.uses_root_const) {
        os << "cbuffer _cb0 : register(b0) { uint4 _rc4[4]; };\n";
        os << "uint _sv_rc(uint idx) { if (idx >= 16u) return 0u; uint4 v = _rc4[idx >> 2u]; uint c = idx & 3u; return (c==0u)?v.x:((c==1u)?v.y:((c==2u)?v.z:v.w)); }\n";
    }
    if (usage.uses_cross_lane && (container.stage == ShaderStage::Compute || container.stage == ShaderStage::Mesh || container.stage == ShaderStage::Amplification)) {
        os << "groupshared uint _lane_xchg[" << tg_total << "];\n";
    }

    // Compact helper library (all SM 6.5 / DXC compliant, zero warnings)
    os << "static const float SV_TWO_PI = 6.2831853071795864769;\n"
       << "bool _sv_isnan_u32(uint b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }\n"
       << "bool _sv_isinf_u32(uint b) { return (b & 0x7FFFFFFFu) == 0x7F800000u; }\n"
       << "bool _sv_isnan_u16(uint h) { return (h & 0x7FFFu) > 0x7C00u; }\n"
       << "bool _sv_isnan_u64(uint lo, uint hi) { uint m = hi & 0x7FFFFFFFu; return (m > 0x7FF00000u) || (m == 0x7FF00000u && lo != 0u); }\n"
       << "uint _sv_rcp_f32(uint b) {\n"
       << "    uint mag = b & 0x7FFFFFFFu; uint sgn = b & 0x80000000u;\n"
       << "    if (mag > 0x7F800000u) return 0x7FC00000u;\n"
       << "    if (mag == 0u) return sgn | 0x7F800000u;\n"
       << "    if (mag == 0x7F800000u) return sgn;\n"
       << "    return asuint(1.0 / asfloat(b));\n"
       << "}\n"
       << "uint _sv_rsq_f32(uint b) {\n"
       << "    uint mag = b & 0x7FFFFFFFu;\n"
       << "    if (mag > 0x7F800000u) return 0x7FC00000u;\n"
       << "    if (mag == 0u) return (b & 0x80000000u) | 0x7F800000u;\n"
       << "    if ((b & 0x80000000u) != 0u) return 0x7FC00000u;\n"
       << "    if (mag == 0x7F800000u) return 0u;\n"
       << "    return asuint(rsqrt(asfloat(b)));\n"
       << "}\n"
       << "uint _sv_sqrt_f32(uint b) {\n"
       << "    uint mag = b & 0x7FFFFFFFu;\n"
       << "    if (mag > 0x7F800000u) return 0x7FC00000u;\n"
       << "    if (mag == 0u) return b;\n"
       << "    if ((b & 0x80000000u) != 0u) return 0x7FC00000u;\n"
       << "    if (mag == 0x7F800000u) return 0x7F800000u;\n"
       << "    return asuint(sqrt(asfloat(b)));\n"
       << "}\n"
       << "float _sv_f16_to_f32(uint h) { return f16tof32(h & 0xFFFFu); }\n"
       << "uint _sv_f32_to_f16(float f) { return f32tof16(f) & 0xFFFFu; }\n"
       << "float _sv_sin_turns_f32(float x) {\n"
       << "    uint bx = asuint(x);\n"
       << "    if (_sv_isnan_u32(bx) || _sv_isinf_u32(bx)) return asfloat(0x7FC00000u);\n"
       << "    if (abs(x) >= 256.0) return 0.0;\n"
       << "    float t = x - floor(x);\n"
       << "    if (t == 0.0 || t == 0.5) return 0.0;\n"
       << "    if (t == 0.25) return 1.0;\n"
       << "    if (t == 0.75) return -1.0;\n"
       << "    return sin(t * SV_TWO_PI);\n"
       << "}\n"
       << "float _sv_cos_turns_f32(float x) {\n"
       << "    uint bx = asuint(x);\n"
       << "    if (_sv_isnan_u32(bx) || _sv_isinf_u32(bx)) return asfloat(0x7FC00000u);\n"
       << "    if (abs(x) >= 256.0) return 1.0;\n"
       << "    float t = x - floor(x);\n"
       << "    if (t == 0.0) return 1.0;\n"
       << "    if (t == 0.25 || t == 0.75) return 0.0;\n"
       << "    if (t == 0.5) return -1.0;\n"
       << "    return cos(t * SV_TWO_PI);\n"
       << "}\n"
       << "float _sv_fma_f32(float a, float b, float c) {\n"
       << "    uint ba = asuint(a); uint bb = asuint(b);\n"
       << "    uint ea = (ba >> 23u) & 0xFFu; uint eb = (bb >> 23u) & 0xFFu;\n"
       << "    if (ea >= 30u && ea <= 220u && eb >= 30u && eb <= 220u) {\n"
       << "        precise float ca = 4097.0 * a; precise float ahi = ca - (ca - a); precise float alo = a - ahi;\n"
       << "        precise float cb = 4097.0 * b; precise float bhi = cb - (cb - b); precise float blo = b - bhi;\n"
       << "        precise float phi = a * b;\n"
       << "        precise float plo = (((ahi * bhi - phi) + ahi * blo) + alo * bhi) + alo * blo;\n"
       << "        precise float s = phi + c; precise float v = s - phi;\n"
       << "        precise float e = (phi - (s - v)) + (c - v);\n"
       << "        precise float r = s + (e + plo);\n"
       << "        return r;\n"
       << "    }\n"
       << "    precise float r = mad(a, b, c);\n"
       << "    return r;\n"
       << "}\n"
       << "uint _sv_sbfe_u32(uint src0, uint src1) {\n"
       << "    uint o = src1 & 31u; uint ww = (src1 >> 16u) & 0x7Fu;\n"
       << "    if (ww == 0u || o >= 32u) return 0u;\n"
       << "    if (o + ww >= 32u) return src0 >> o;\n"
       << "    return (src0 >> o) & ((1u << (ww & 31u)) - 1u);\n"
       << "}\n"
       << "uint _sv_sbfe_i32(uint src0, uint src1) {\n"
       << "    uint o = src1 & 31u; uint ww = (src1 >> 16u) & 0x7Fu;\n"
       << "    if (ww == 0u) return 0u;\n"
       << "    if (o >= 32u) return asuint(asint(src0) >> 31);\n"
       << "    if (o + ww >= 32u) return asuint(asint(src0) >> o);\n"
       << "    return asuint(asint(src0 << ((32u - o - ww) & 31u)) >> ((32u - ww) & 31u));\n"
       << "}\n"
       << "uint2 _sv_sbfe_u64(uint lo, uint hi, uint src1) {\n"
       << "    uint o = src1 & 63u; uint w = (src1 >> 16u) & 0x7Fu;\n"
       << "    if (w == 0u || o >= 64u) return uint2(0u, 0u);\n"
       << "    uint rlo = (o == 0u) ? lo : ((o < 32u) ? ((lo >> o) | (hi << (32u - o))) : (hi >> (o - 32u)));\n"
       << "    uint rhi = (o < 32u) ? (hi >> o) : 0u;\n"
       << "    if (o + w >= 64u) return uint2(rlo, rhi);\n"
       << "    if (w < 32u) return uint2(rlo & ((1u << w) - 1u), 0u);\n"
       << "    if (w == 32u) return uint2(rlo, 0u);\n"
       << "    return uint2(rlo, rhi & ((1u << (w - 32u)) - 1u));\n"
       << "}\n"
       << "uint2 _sv_sbfe_i64(uint lo, uint hi, uint src1) {\n"
       << "    uint o = src1 & 63u; uint w = (src1 >> 16u) & 0x7Fu;\n"
       << "    if (w == 0u) return uint2(0u, 0u);\n"
       << "    uint sfill = asuint(asint(hi) >> 31);\n"
       << "    if (o >= 64u) return uint2(sfill, sfill);\n"
       << "    uint rlo = (o == 0u) ? lo : ((o < 32u) ? ((lo >> o) | (hi << (32u - o))) : asuint(asint(hi) >> (o - 32u)));\n"
       << "    uint rhi = (o < 32u) ? asuint(asint(hi) >> o) : sfill;\n"
       << "    if (o + w >= 64u) return uint2(rlo, rhi);\n"
       << "    if (w < 32u) {\n"
       << "        uint slo = asuint(asint(rlo << (32u - w)) >> (32u - w));\n"
       << "        return uint2(slo, asuint(asint(slo) >> 31));\n"
       << "    }\n"
       << "    if (w == 32u) return uint2(rlo, asuint(asint(rlo) >> 31));\n"
       << "    uint sw = w - 32u;\n"
       << "    return uint2(rlo, asuint(asint(rhi << (32u - sw)) >> (32u - sw)));\n"
       << "}\n"
       << "uint _sv_wqm32(uint m) {\n"
       << "    uint r = 0u; [unroll] for (uint q = 0u; q < 8u; ++q) { uint qm = 0xFu << (q * 4u); if ((m & qm) != 0u) r |= qm; } return r;\n"
       << "}\n"
       << "uint _sv_quadmask32(uint m) {\n"
       << "    uint r = 0u; [unroll] for (uint q = 0u; q < 8u; ++q) { if ((m & (0xFu << (q * 4u))) != 0u) r |= (1u << q); } return r;\n"
       << "}\n"
       << "bool _sv_fclass_f32_bits(uint b, uint mask) {\n"
       << "    bool neg = (b >> 31u) != 0u; uint ex = (b >> 23u) & 0xFFu; uint fr = b & 0x7FFFFFu;\n"
       << "    uint bit_i = 0u;\n"
       << "    if (ex == 0xFFu) bit_i = (fr == 0u) ? (neg ? 2u : 9u) : (((fr & 0x400000u) != 0u) ? 1u : 0u);\n"
       << "    else if (ex == 0u) bit_i = (fr == 0u) ? (neg ? 5u : 6u) : (neg ? 4u : 7u);\n"
       << "    else bit_i = neg ? 3u : 8u;\n"
       << "    return ((mask >> bit_i) & 1u) != 0u;\n"
       << "}\n"
       << "bool _sv_fclass_f16_bits(uint h, uint mask) {\n"
       << "    uint hb = h & 0xFFFFu; bool neg = (hb >> 15u) != 0u; uint ex = (hb >> 10u) & 0x1Fu; uint fr = hb & 0x3FFu;\n"
       << "    uint bit_i = 0u;\n"
       << "    if (ex == 0x1Fu) bit_i = (fr == 0u) ? (neg ? 2u : 9u) : (((fr & 0x200u) != 0u) ? 1u : 0u);\n"
       << "    else if (ex == 0u) bit_i = (fr == 0u) ? (neg ? 5u : 6u) : (neg ? 4u : 7u);\n"
       << "    else bit_i = neg ? 3u : 8u;\n"
       << "    return ((mask >> bit_i) & 1u) != 0u;\n"
       << "}\n"
       << "bool _sv_fclass_f64_bits(uint lo, uint hi, uint mask) {\n"
       << "    bool neg = (hi >> 31u) != 0u; uint ex = (hi >> 20u) & 0x7FFu; uint fh = hi & 0xFFFFFu; bool fz = (fh == 0u && lo == 0u);\n"
       << "    uint bit_i = 0u;\n"
       << "    if (ex == 0x7FFu) bit_i = fz ? (neg ? 2u : 9u) : (((fh & 0x80000u) != 0u) ? 1u : 0u);\n"
       << "    else if (ex == 0u) bit_i = fz ? (neg ? 5u : 6u) : (neg ? 4u : 7u);\n"
       << "    else bit_i = neg ? 3u : 8u;\n"
       << "    return ((mask >> bit_i) & 1u) != 0u;\n"
       << "}\n"
       << "bool _sv_cmp_f32_bits(uint a, uint b, uint cc) {\n"
       << "    bool nan = _sv_isnan_u32(a) || _sv_isnan_u32(b);\n"
       << "    bool unord = (cc >= 8u); uint op = cc & 7u;\n"
       << "    if (op == 0u) return unord ? nan : false;\n"
       << "    if (op == 7u) return unord ? true : !nan;\n"
       << "    if (nan) return unord;\n"
       << "    uint am = a & 0x7FFFFFFFu; uint bm = b & 0x7FFFFFFFu;\n"
       << "    uint ao = (am == 0u) ? 0x80000000u : (((a & 0x80000000u) != 0u) ? ~a : (a | 0x80000000u));\n"
       << "    uint bo = (bm == 0u) ? 0x80000000u : (((b & 0x80000000u) != 0u) ? ~b : (b | 0x80000000u));\n"
       << "    if (op == 1u) return ao < bo; if (op == 2u) return ao == bo; if (op == 3u) return ao <= bo;\n"
       << "    if (op == 4u) return ao > bo; if (op == 5u) return ao != bo; return ao >= bo;\n"
       << "}\n"
       << "bool _sv_cmp_f16_bits(uint a, uint b, uint cc) {\n"
       << "    uint ah = a & 0xFFFFu; uint bh = b & 0xFFFFu;\n"
       << "    bool nan = _sv_isnan_u16(ah) || _sv_isnan_u16(bh);\n"
       << "    bool unord = (cc >= 8u); uint op = cc & 7u;\n"
       << "    if (op == 0u) return unord ? nan : false;\n"
       << "    if (op == 7u) return unord ? true : !nan;\n"
       << "    if (nan) return unord;\n"
       << "    uint am = ah & 0x7FFFu; uint bm = bh & 0x7FFFu;\n"
       << "    uint ao = (am == 0u) ? 0x8000u : (((ah & 0x8000u) != 0u) ? ((~ah) & 0xFFFFu) : (ah | 0x8000u));\n"
       << "    uint bo = (bm == 0u) ? 0x8000u : (((bh & 0x8000u) != 0u) ? ((~bh) & 0xFFFFu) : (bh | 0x8000u));\n"
       << "    if (op == 1u) return ao < bo; if (op == 2u) return ao == bo; if (op == 3u) return ao <= bo;\n"
       << "    if (op == 4u) return ao > bo; if (op == 5u) return ao != bo; return ao >= bo;\n"
       << "}\n"
       << "bool _sv_cmp_f64_bits(uint alo, uint ahi, uint blo, uint bhi, uint cc) {\n"
       << "    bool nan = _sv_isnan_u64(alo, ahi) || _sv_isnan_u64(blo, bhi);\n"
       << "    bool unord = (cc >= 8u); uint op = cc & 7u;\n"
       << "    if (op == 0u) return unord ? nan : false;\n"
       << "    if (op == 7u) return unord ? true : !nan;\n"
       << "    if (nan) return unord;\n"
       << "    bool az = ((ahi & 0x7FFFFFFFu) == 0u) && (alo == 0u);\n"
       << "    bool bz = ((bhi & 0x7FFFFFFFu) == 0u) && (blo == 0u);\n"
       << "    uint aoh = az ? 0x80000000u : (((ahi & 0x80000000u) != 0u) ? ~ahi : (ahi | 0x80000000u));\n"
       << "    uint aol = az ? 0u : (((ahi & 0x80000000u) != 0u) ? ~alo : alo);\n"
       << "    uint boh = bz ? 0x80000000u : (((bhi & 0x80000000u) != 0u) ? ~bhi : (bhi | 0x80000000u));\n"
       << "    uint bol = bz ? 0u : (((bhi & 0x80000000u) != 0u) ? ~blo : blo);\n"
       << "    bool eq = (aoh == boh) && (aol == bol);\n"
       << "    bool lt = (aoh < boh) || ((aoh == boh) && (aol < bol));\n"
       << "    if (op == 1u) return lt; if (op == 2u) return eq; if (op == 3u) return lt || eq;\n"
       << "    if (op == 4u) return !lt && !eq; if (op == 5u) return !eq; return !lt;\n"
       << "}\n"
       << "bool _sv_cmp_u64(uint alo, uint ahi, uint blo, uint bhi, uint cc) {\n"
       << "    bool eq = (ahi == bhi) && (alo == blo);\n"
       << "    bool lt = (ahi < bhi) || ((ahi == bhi) && (alo < blo));\n"
       << "    if (cc == 0u) return false; if (cc == 1u) return lt; if (cc == 2u) return eq; if (cc == 3u) return lt || eq;\n"
       << "    if (cc == 4u) return !lt && !eq; if (cc == 5u) return !eq; if (cc == 6u) return !lt; return true;\n"
       << "}\n"
       << "bool _sv_cmp_i64(uint alo, uint ahi, uint blo, uint bhi, uint cc) {\n"
       << "    return _sv_cmp_u64(alo, ahi ^ 0x80000000u, blo, bhi ^ 0x80000000u, cc);\n"
       << "}\n"
       << "uint _sv_bfe_u32(uint src0, uint off, uint w) {\n"
       << "    uint o = off & 31u; uint ww = w & 31u;\n"
       << "    if (ww == 0u) return 0u;\n"
       << "    if (o + ww >= 32u) return src0 >> o;\n"
       << "    return (src0 >> o) & ((1u << ww) - 1u);\n"
       << "}\n"
       << "uint _sv_bfe_i32(uint src0, uint off, uint w) {\n"
       << "    uint o = off & 31u; uint ww = w & 31u;\n"
       << "    if (ww == 0u) return 0u;\n"
       << "    if (o + ww >= 32u) return asuint(asint(src0) >> o);\n"
       << "    return asuint(asint(src0 << (32u - o - ww)) >> (32u - ww));\n"
       << "}\n"
       << "uint _sv_bfm_b32(uint w, uint off) { uint ww = w & 31u; uint oo = off & 31u; return ((1u << ww) - 1u) << oo; }\n"
       << "uint _sv_ff1_32(uint x) { return (x == 0u) ? 0xFFFFFFFFu : firstbitlow(x); }\n"
       << "uint _sv_ff0_32(uint x) { return _sv_ff1_32(~x); }\n"
       << "uint _sv_flbit_u32(uint x) { return (x == 0u) ? 0xFFFFFFFFu : (31u - firstbithigh(x)); }\n"
       << "uint _sv_flbit_i32(uint x) { int s = asint(x); if (s == 0 || s == -1) return 0xFFFFFFFFu; uint u = (s < 0) ? ~x : x; return 31u - firstbithigh(u); }\n"
       << "uint _sv_bitrep_lo(uint x) { uint r = 0u; [unroll] for (uint i = 0u; i < 16u; ++i) { if ((x & (1u << i)) != 0u) r |= (3u << (i * 2u)); } return r; }\n"
       << "uint _sv_bitrep_hi(uint x) { uint r = 0u; [unroll] for (uint i = 0u; i < 16u; ++i) { if ((x & (1u << (i + 16u))) != 0u) r |= (3u << (i * 2u)); } return r; }\n"
       << "uint _sv_mul_hi_u32(uint a, uint b) {\n"
       << "    uint a0 = a & 0xFFFFu, a1 = a >> 16u, b0 = b & 0xFFFFu, b1 = b >> 16u;\n"
       << "    uint p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;\n"
       << "    uint mid = (p0 >> 16u) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);\n"
       << "    return p3 + (p1 >> 16u) + (p2 >> 16u) + (mid >> 16u);\n"
       << "}\n"
       << "uint _sv_mul_hi_i32(uint au, uint bu) {\n"
       << "    uint hi = _sv_mul_hi_u32(au, bu);\n"
       << "    if (asint(au) < 0) hi -= bu;\n"
       << "    if (asint(bu) < 0) hi -= au;\n"
       << "    return hi;\n"
       << "}\n"
       << "uint _sv_alignbit(uint s0, uint s1, uint s2) { uint sh = s2 & 31u; return (sh == 0u) ? s1 : ((s1 >> sh) | (s0 << (32u - sh))); }\n"
       << "uint _sv_alignbyte(uint s0, uint s1, uint s2) { uint sh = (s2 & 3u) * 8u; return (sh == 0u) ? s1 : ((s1 >> sh) | (s0 << (32u - sh))); }\n"
       << "uint _sv_perm_b32(uint s0, uint s1, uint s2) {\n"
       << "    uint r = 0u;\n"
       << "    [unroll] for (uint i = 0u; i < 4u; ++i) {\n"
       << "        uint sel = (s2 >> (i * 8u)) & 0xFFu;\n"
       << "        uint bv = 0xFFu;\n"
       << "        if (sel < 4u) bv = (s1 >> (sel * 8u)) & 0xFFu;\n"
       << "        else if (sel < 8u) bv = (s0 >> ((sel - 4u) * 8u)) & 0xFFu;\n"
       << "        else if (sel == 8u) bv = (((s1 >> 15u) & 1u) != 0u) ? 0xFFu : 0u;\n"
       << "        else if (sel == 9u) bv = (((s1 >> 31u) & 1u) != 0u) ? 0xFFu : 0u;\n"
       << "        else if (sel == 10u) bv = (((s0 >> 15u) & 1u) != 0u) ? 0xFFu : 0u;\n"
       << "        else if (sel == 11u) bv = (((s0 >> 31u) & 1u) != 0u) ? 0xFFu : 0u;\n"
       << "        else if (sel == 12u) bv = 0u;\n"
       << "        r |= (bv << (i * 8u));\n"
       << "    }\n"
       << "    return r;\n"
       << "}\n"
       << "uint _sv_lerp_u8(uint a, uint b, uint c) {\n"
       << "    uint r = 0u;\n"
       << "    [unroll] for (uint i = 0u; i < 4u; ++i) {\n"
       << "        uint ba = (a >> (i * 8u)) & 0xFFu, bb = (b >> (i * 8u)) & 0xFFu, bc = (c >> (i * 8u)) & 1u;\n"
       << "        r |= (((ba + bb + bc) >> 1u) & 0xFFu) << (i * 8u);\n"
       << "    }\n"
       << "    return r;\n"
       << "}\n"
       << "uint _sv_sad_u8(uint a, uint b, uint c) {\n"
       << "    uint sum = 0u;\n"
       << "    [unroll] for (uint i = 0u; i < 4u; ++i) {\n"
       << "        uint ba = (a >> (i * 8u)) & 0xFFu, bb = (b >> (i * 8u)) & 0xFFu;\n"
       << "        sum += (ba >= bb) ? (ba - bb) : (bb - ba);\n"
       << "    }\n"
       << "    return c + sum;\n"
       << "}\n"
       << "uint _sv_sad_u16(uint a, uint b, uint c) {\n"
       << "    uint a0 = a & 0xFFFFu, a1 = a >> 16u, b0 = b & 0xFFFFu, b1 = b >> 16u;\n"
       << "    return c + ((a0 >= b0) ? (a0 - b0) : (b0 - a0)) + ((a1 >= b1) ? (a1 - b1) : (b1 - a1));\n"
       << "}\n"
       << "uint _sv_msad_u8(uint a, uint b, uint c) {\n"
       << "    uint sum = 0u;\n"
       << "    [unroll] for (uint i = 0u; i < 4u; ++i) {\n"
       << "        uint ba = (a >> (i * 8u)) & 0xFFu, bb = (b >> (i * 8u)) & 0xFFu;\n"
       << "        if (bb != 0u) sum += (ba >= bb) ? (ba - bb) : (bb - ba);\n"
       << "    }\n"
       << "    return c + sum;\n"
       << "}\n"
       << "uint _sv_sext8(uint x) { return asuint(asint((x & 0xFFu) << 24u) >> 24); }\n"
       << "uint _sv_sext16(uint x) { return asuint(asint((x & 0xFFFFu) << 16u) >> 16); }\n"
       << "uint _sv_sext4(uint x) { return asuint(asint((x & 0xFu) << 28u) >> 28); }\n"
       << "uint _sv_dot2_i16(uint a, uint b, uint c) {\n"
       << "    int s = asint(c) + asint(_sv_sext16(a)) * asint(_sv_sext16(b)) + asint(_sv_sext16(a >> 16u)) * asint(_sv_sext16(b >> 16u));\n"
       << "    return asuint(s);\n"
       << "}\n"
       << "uint _sv_dot2_u16(uint a, uint b, uint c) { return c + (a & 0xFFFFu) * (b & 0xFFFFu) + (a >> 16u) * (b >> 16u); }\n"
       << "uint _sv_dot4_i8(uint a, uint b, uint c) {\n"
       << "    int s = asint(c);\n"
       << "    [unroll] for (uint i = 0u; i < 4u; ++i) s += asint(_sv_sext8(a >> (i * 8u))) * asint(_sv_sext8(b >> (i * 8u)));\n"
       << "    return asuint(s);\n"
       << "}\n"
       << "uint _sv_dot4_u8(uint a, uint b, uint c) {\n"
       << "    uint s = c;\n"
       << "    [unroll] for (uint i = 0u; i < 4u; ++i) s += ((a >> (i * 8u)) & 0xFFu) * ((b >> (i * 8u)) & 0xFFu);\n"
       << "    return s;\n"
       << "}\n"
       << "uint _sv_dot8_i4(uint a, uint b, uint c) {\n"
       << "    int s = asint(c);\n"
       << "    [unroll] for (uint i = 0u; i < 8u; ++i) s += asint(_sv_sext4(a >> (i * 4u))) * asint(_sv_sext4(b >> (i * 4u)));\n"
       << "    return asuint(s);\n"
       << "}\n"
       << "uint _sv_dot8_u4(uint a, uint b, uint c) {\n"
       << "    uint s = c;\n"
       << "    [unroll] for (uint i = 0u; i < 8u; ++i) s += ((a >> (i * 4u)) & 0xFu) * ((b >> (i * 4u)) & 0xFu);\n"
       << "    return s;\n"
       << "}\n"
       << "float _sv_div_fixup_f32(float s0, float s1, float s2) {\n"
       << "    uint b1 = asuint(s1); uint b2 = asuint(s2);\n"
       << "    bool sn = ((b1 ^ b2) & 0x80000000u) != 0u;\n"
       << "    if (_sv_isnan_u32(b1) || _sv_isnan_u32(b2)) return asfloat(0x7FC00000u);\n"
       << "    if ((b1 & 0x7FFFFFFFu) == 0u) return sn ? asfloat(0xFF800000u) : asfloat(0x7F800000u);\n"
       << "    if ((b2 & 0x7FFFFFFFu) == 0u) return sn ? asfloat(0x80000000u) : 0.0;\n"
       << "    return sn ? -abs(s0) : abs(s0);\n"
       << "}\n"
       << "uint _sv_frexp_exp_f32(float x) {\n"
       << "    if (x == 0.0) return 0u;\n"
       << "    float ex = 0.0; (void)frexp(x, ex); return asuint(int(ex));\n"
       << "}\n"
       << "float _sv_frexp_mant_f32(float x) {\n"
       << "    if (x == 0.0) return x;\n"
       << "    float ex = 0.0; return frexp(x, ex);\n"
       << "}\n"
       << "uint _sv_sdwa_src(uint raw, uint sel, bool sext, bool isf, bool neg_v, bool abs_v) {\n"
       << "    uint v = raw;\n"
       << "    if (sel == 0u) v = sext ? _sv_sext8(raw) : (raw & 0xFFu);\n"
       << "    else if (sel == 1u) v = sext ? _sv_sext8(raw >> 8u) : ((raw >> 8u) & 0xFFu);\n"
       << "    else if (sel == 2u) v = sext ? _sv_sext8(raw >> 16u) : ((raw >> 16u) & 0xFFu);\n"
       << "    else if (sel == 3u) v = sext ? _sv_sext8(raw >> 24u) : ((raw >> 24u) & 0xFFu);\n"
       << "    else if (sel == 4u) v = sext ? _sv_sext16(raw) : (raw & 0xFFFFu);\n"
       << "    else if (sel == 5u) v = sext ? _sv_sext16(raw >> 16u) : ((raw >> 16u) & 0xFFFFu);\n"
       << "    if (isf) { if (abs_v) v &= 0x7FFFFFFFu; if (neg_v) v ^= 0x80000000u; }\n"
       << "    return v;\n"
       << "}\n"
       << "uint _sv_sdwa_dst(uint old_d, uint res, uint sel, uint unused_m) {\n"
       << "    if (sel >= 6u) return res;\n"
       << "    uint mask = 0xFFu; uint sh = 0u; uint sb = (res >> 7u) & 1u;\n"
       << "    if (sel == 1u) { mask = 0xFF00u; sh = 8u; }\n"
       << "    else if (sel == 2u) { mask = 0xFF0000u; sh = 16u; }\n"
       << "    else if (sel == 3u) { mask = 0xFF000000u; sh = 24u; }\n"
       << "    else if (sel == 4u) { mask = 0xFFFFu; sh = 0u; sb = (res >> 15u) & 1u; }\n"
       << "    else if (sel == 5u) { mask = 0xFFFF0000u; sh = 16u; sb = (res >> 15u) & 1u; }\n"
       << "    uint placed = (res << sh) & mask;\n"
       << "    if (unused_m == 2u) return (old_d & ~mask) | placed;\n"
       << "    if (unused_m == 1u && sb != 0u) return placed | ~(mask | (mask - 1u));\n"
       << "    return placed;\n"
       << "}\n"
       << "int _sv_dpp16_src_lane(uint lane, uint ctrl, uint wsize) {\n"
       << "    uint rb = lane & ~15u; uint ir = lane & 15u;\n"
       << "    if (ctrl <= 0xFFu) return int((lane & ~3u) | ((ctrl >> ((lane & 3u) * 2u)) & 3u));\n"
       << "    if (ctrl >= 0x101u && ctrl <= 0x10Fu) { uint s = ir + (ctrl - 0x100u); return (s < 16u) ? int(rb + s) : -1; }\n"
       << "    if (ctrl >= 0x111u && ctrl <= 0x11Fu) { uint sh = ctrl - 0x110u; return (ir >= sh) ? int(rb + (ir - sh)) : -1; }\n"
       << "    if (ctrl >= 0x121u && ctrl <= 0x12Fu) { uint sh = ctrl - 0x120u; return int(rb + ((ir + 16u - sh) & 15u)); }\n"
       << "    if (ctrl == 0x130u) return (lane + 1u < wsize) ? int(lane + 1u) : -1;\n"
       << "    if (ctrl == 0x134u) return int((lane + 1u) % wsize);\n"
       << "    if (ctrl == 0x138u) return (lane > 0u) ? int(lane - 1u) : -1;\n"
       << "    if (ctrl == 0x13Cu) return int((lane + wsize - 1u) % wsize);\n"
       << "    if (ctrl == 0x140u) return int(rb + (15u - ir));\n"
       << "    if (ctrl == 0x141u) return int((lane & ~7u) + (7u - (lane & 7u)));\n"
       << "    if (ctrl == 0x142u) return (lane >= 16u) ? int((lane & ~15u) - 1u) : -1;\n"
       << "    if (ctrl == 0x143u) return (lane >= 32u) ? int((lane & ~31u) - 1u) : -1;\n"
       << "    if (ctrl >= 0x150u && ctrl <= 0x15Fu) return int(rb + (ctrl & 15u));\n"
       << "    if (ctrl >= 0x160u && ctrl <= 0x16Fu) return int(rb + (ir ^ (ctrl & 15u)));\n"
       << "    return int(lane);\n"
       << "}\n";

    switch (container.stage) {
        case ShaderStage::Compute:
            out_target = use_sm65 ? "cs_6_5" : "cs_5_0";
            os << "[numthreads(" << tg_x << ", " << tg_y << ", " << tg_z << ")]\n";
            os << "void main(uint3 _tid : SV_DispatchThreadID, uint _gtid : SV_GroupIndex) {\n";
            os << "    uint _lane = _gtid & " << lane_mask << "u;\n";
            os << "    uint _gw_base = _gtid & ~" << lane_mask << "u;\n";
            break;
        case ShaderStage::Vertex:
            out_target = use_sm65 ? "vs_6_5" : "vs_5_0";
            os << "struct VSOut { float4 pos : SV_POSITION; };\n";
            os << "VSOut main(uint _vid : SV_VertexID) {\n";
            os << "    VSOut _out; _out.pos = float4(0.0, 0.0, 0.0, 1.0);\n";
            os << "    uint _lane = _vid & " << lane_mask << "u; uint _gw_base = 0u;\n";
            break;
        case ShaderStage::Pixel:
            out_target = use_sm65 ? "ps_6_5" : "ps_5_0";
            os << "float4 main(float4 _pos : SV_POSITION) : SV_TARGET0 {\n";
            os << "    float4 _mrt0 = float4(0.0, 0.0, 0.0, 1.0);\n";
            os << "    uint _lane = 0u; uint _gw_base = 0u;\n";
            break;
        case ShaderStage::Mesh:
            out_target = "ms_6_5";
            os << "struct MeshVert { float4 pos : SV_POSITION; };\n";
            os << "[outputtopology(\"triangle\")]\n";
            os << "[numthreads(" << tg_x << ", " << tg_y << ", " << tg_z << ")]\n";
            os << "void main(uint _gtid : SV_GroupIndex, out vertices MeshVert _verts[64], out indices uint3 _prims[64]) {\n";
            os << "    SetMeshOutputCounts(3u, 1u);\n";
            os << "    uint _lane = _gtid & " << lane_mask << "u; uint _gw_base = _gtid & ~" << lane_mask << "u;\n";
            break;
        case ShaderStage::Amplification:
            out_target = "as_6_5";
            os << "struct ASPayload { uint d[4]; };\n";
            os << "groupshared ASPayload _payload;\n";
            os << "[numthreads(" << tg_x << ", " << tg_y << ", " << tg_z << ")]\n";
            os << "void main(uint _gtid : SV_GroupIndex) {\n";
            os << "    _payload.d[0] = 0u; _payload.d[1] = 0u; _payload.d[2] = 0u; _payload.d[3] = 0u;\n";
            os << "    uint _lane = _gtid & " << lane_mask << "u; uint _gw_base = _gtid & ~" << lane_mask << "u;\n";
            break;
        default:
            return Status::Error(StatusCode::UnsupportedFormat, "Unsupported shader stage");
    }

    os << "    uint s[" << usage.max_sgpr << "];\n";
    os << "    uint v[" << usage.max_vgpr << "];\n";
    os << "    [unroll] for (uint _i = 0u; _i < " << usage.max_sgpr << "u; ++_i) s[_i] = 0u;\n";
    os << "    [unroll] for (uint _i = 0u; _i < " << usage.max_vgpr << "u; ++_i) v[_i] = 0u;\n";
    if (container.stage == ShaderStage::Compute) os << "    v[0] = _tid.x;\n";
    else if (container.stage == ShaderStage::Vertex) os << "    v[0] = _vid;\n";
    else if (container.stage == ShaderStage::Pixel) os << "    v[0] = asuint(_pos.x);\n";
    else os << "    v[0] = _gtid;\n";
    os << "    bool scc = false;\n";
    os << "    uint vcc_lo = 0u; uint vcc_hi = 0u;\n";
    os << "    uint exec_lo = 0xFFFFFFFFu; uint exec_hi = " << (wave_n == 64u ? "0xFFFFFFFFu" : "0u") << ";\n";
    os << "    uint m0 = 0u;\n";
    os << "    bool _vcc_bit = false;\n";
    os << "    (void)_lane; (void)_gw_base; (void)scc; (void)vcc_lo; (void)vcc_hi; (void)exec_lo; (void)exec_hi; (void)m0; (void)_vcc_bit;\n";

    auto hlsl_op_u32 = [&](const Rdna2Operand& op, bool apply_fmod = false) -> std::string {
        std::string expr = "0u";
        switch (op.kind) {
            case Rdna2OperandKind::Sgpr:        expr = "s[" + std::to_string(op.reg_index) + "]"; break;
            case Rdna2OperandKind::Vgpr:        expr = "v[" + std::to_string(op.reg_index) + "]"; break;
            case Rdna2OperandKind::InlineInt:   expr = HexLitU32(static_cast<uint32_t>(op.int_value)); break;
            case Rdna2OperandKind::InlineFloat: expr = HexLitU32(FloatToBits(op.float_value)); break;
            case Rdna2OperandKind::Literal32:   expr = HexLitU32(op.literal_bits); break;
            case Rdna2OperandKind::VccLo:       expr = "vcc_lo"; break;
            case Rdna2OperandKind::VccHi:       expr = "vcc_hi"; break;
            case Rdna2OperandKind::ExecLo:      expr = "exec_lo"; break;
            case Rdna2OperandKind::ExecHi:      expr = "exec_hi"; break;
            case Rdna2OperandKind::Scc:         expr = "(scc ? 1u : 0u)"; break;
            case Rdna2OperandKind::M0:          expr = "m0"; break;
            default:                            expr = "0u"; break;
        }
        if (apply_fmod) {
            if (op.abs) expr = "(" + expr + " & 0x7FFFFFFFu)";
            if (op.neg) expr = "(" + expr + " ^ 0x80000000u)";
        }
        return expr;
    };

    auto hlsl_dst_lval = [&](const Rdna2Operand& dst) -> std::string {
        switch (dst.kind) {
            case Rdna2OperandKind::Sgpr:   return "s[" + std::to_string(dst.reg_index) + "]";
            case Rdna2OperandKind::Vgpr:   return "v[" + std::to_string(dst.reg_index) + "]";
            case Rdna2OperandKind::VccLo:  return "vcc_lo";
            case Rdna2OperandKind::VccHi:  return "vcc_hi";
            case Rdna2OperandKind::ExecLo: return "exec_lo";
            case Rdna2OperandKind::ExecHi: return "exec_hi";
            case Rdna2OperandKind::M0:     return "m0";
            default:                       return "s[0]";
        }
    };

    for (size_t idx = 0u; idx < insts.size(); ++idx) {
        const auto& sv = insts[idx];
        const auto& inst = sv.base;
        const auto* e = sv.entry;
        const auto& mod = sv.modifier;

        if (!e) {
            if (inst.encoding == Rdna2Encoding::MUBUF) {
                const char* buf = (inst.srsrc == 0u) ? "_uav0" : "_uav1";
                const std::string addr = (inst.offen ? ("v[" + std::to_string(inst.src[1].reg_index) + "]") : "0u") +
                                         " + " + HexLitU32(static_cast<uint32_t>(inst.mem_offset));
                if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                    os << "    v[" << inst.dst.reg_index << "] = " << buf << ".Load(" << addr << ");\n";
                } else if (inst.opcode == Rdna2Opcode::BUFFER_STORE_DWORD) {
                    os << "    " << buf << ".Store(" << addr << ", v[" << inst.src[0].reg_index << "]);\n";
                }
            } else if (inst.encoding == Rdna2Encoding::FLAT) {
                const std::string addr = "asuint(asint(v[" + std::to_string(inst.src[0].reg_index) + "]) + " + std::to_string(inst.mem_offset) + ")";
                if (inst.opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD || inst.opcode == Rdna2Opcode::FLAT_LOAD_DWORD) {
                    os << "    v[" << inst.dst.reg_index << "] = _uav0.Load(" << addr << ");\n";
                } else if (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD || inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD) {
                    os << "    " << (usage.uses_uav1 ? "_uav1" : "_uav0") << ".Store(" << addr << ", v[" << inst.src[1].reg_index << "]);\n";
                }
            }
            continue;
        }

        const uint16_t raw = inst.raw_opcode;
        const Rdna2Encoding enc = inst.encoding;
        os << "    { // " << e->mnemonic << "\n";

        if (enc == Rdna2Encoding::SOP2) {
            const std::string s0 = hlsl_op_u32(inst.src[0]);
            const std::string s1 = hlsl_op_u32(inst.src[1]);
            const std::string dst = hlsl_dst_lval(inst.dst);
            auto hlsl_op_hi32 = [&](const Rdna2Operand& op) -> std::string {
                if (op.kind == Rdna2OperandKind::Sgpr && static_cast<uint32_t>(op.reg_index) + 1u < usage.max_sgpr) {
                    return "s[" + std::to_string(op.reg_index + 1u) + "]";
                }
                if (op.kind == Rdna2OperandKind::VccLo) return "vcc_hi";
                if (op.kind == Rdna2OperandKind::ExecLo) return "exec_hi";
                if (op.kind == Rdna2OperandKind::InlineInt && op.int_value < 0) return "0xFFFFFFFFu";
                return "0u";
            };
            auto hlsl_dst_hi = [&]() -> std::string {
                if (inst.dst.kind == Rdna2OperandKind::Sgpr && static_cast<uint32_t>(inst.dst.reg_index) + 1u < usage.max_sgpr) {
                    return "s[" + std::to_string(inst.dst.reg_index + 1u) + "]";
                }
                if (inst.dst.kind == Rdna2OperandKind::VccLo) return "vcc_hi";
                if (inst.dst.kind == Rdna2OperandKind::ExecLo) return "exec_hi";
                return "";
            };
            const std::string dst_hi = hlsl_dst_hi();
            os << "        uint _a = " << s0 << "; uint _b = " << s1 << ";\n";
            switch (raw) {
                case 0x00: os << "        uint _r = _a + _b; scc = (_r < _a); " << dst << " = _r;\n"; break;
                case 0x01: os << "        scc = (_b > _a); " << dst << " = _a - _b;\n"; break;
                case 0x02: os << "        " << dst << " = asuint(asint(_a) + asint(_b));\n"; break;
                case 0x03: os << "        " << dst << " = asuint(asint(_a) - asint(_b));\n"; break;
                case 0x04: os << "        uint _c = scc ? 1u : 0u; uint _r = _a + _b + _c; scc = (_r < _a) || (_c != 0u && _r == _a); " << dst << " = _r;\n"; break;
                case 0x05: os << "        uint _c = scc ? 1u : 0u; scc = (_b > _a) || (_c != 0u && _b == _a); " << dst << " = _a - _b - _c;\n"; break;
                case 0x06: os << "        scc = (asint(_a) < asint(_b)); " << dst << " = scc ? _a : _b;\n"; break;
                case 0x07: os << "        scc = (_a < _b); " << dst << " = scc ? _a : _b;\n"; break;
                case 0x08: os << "        scc = (asint(_a) > asint(_b)); " << dst << " = scc ? _a : _b;\n"; break;
                case 0x09: os << "        scc = (_a > _b); " << dst << " = scc ? _a : _b;\n"; break;
                case 0x0A: os << "        " << dst << " = scc ? _a : _b;\n"; break;
                case 0x0B: os << "        " << dst << " = scc ? _a : _b;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = scc ? " << hlsl_op_hi32(inst.src[0]) << " : " << hlsl_op_hi32(inst.src[1]) << ";\n"; break;
                case 0x0E: os << "        uint _r = _a & _b; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x0F: os << "        uint _rl = _a & _b; uint _rh = " << hlsl_op_hi32(inst.src[0]) << " & " << hlsl_op_hi32(inst.src[1]) << "; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x10: os << "        uint _r = _a | _b; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x11: os << "        uint _rl = _a | _b; uint _rh = " << hlsl_op_hi32(inst.src[0]) << " | " << hlsl_op_hi32(inst.src[1]) << "; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x12: os << "        uint _r = _a ^ _b; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x13: os << "        uint _rl = _a ^ _b; uint _rh = " << hlsl_op_hi32(inst.src[0]) << " ^ " << hlsl_op_hi32(inst.src[1]) << "; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x14: os << "        uint _r = _a & ~_b; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x15: os << "        uint _rl = _a & ~_b; uint _rh = " << hlsl_op_hi32(inst.src[0]) << " & ~" << hlsl_op_hi32(inst.src[1]) << "; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x16: os << "        uint _r = _a | ~_b; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x17: os << "        uint _rl = _a | ~_b; uint _rh = " << hlsl_op_hi32(inst.src[0]) << " | ~" << hlsl_op_hi32(inst.src[1]) << "; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x18: os << "        uint _r = ~(_a & _b); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x19: os << "        uint _rl = ~(_a & _b); uint _rh = ~(" << hlsl_op_hi32(inst.src[0]) << " & " << hlsl_op_hi32(inst.src[1]) << "); scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x1A: os << "        uint _r = ~(_a | _b); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x1B: os << "        uint _rl = ~(_a | _b); uint _rh = ~(" << hlsl_op_hi32(inst.src[0]) << " | " << hlsl_op_hi32(inst.src[1]) << "); scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x1C: os << "        uint _r = ~(_a ^ _b); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x1D: os << "        uint _rl = ~(_a ^ _b); uint _rh = ~(" << hlsl_op_hi32(inst.src[0]) << " ^ " << hlsl_op_hi32(inst.src[1]) << "); scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x1E: os << "        uint _r = _a << (_b & 31u); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x1F: os << "        uint _ah = " << hlsl_op_hi32(inst.src[0]) << "; uint _sh = _b & 63u; uint _rl = (_sh < 32u) ? (_a << _sh) : 0u; uint _rh = (_sh == 0u) ? _ah : ((_sh < 32u) ? ((_ah << _sh) | (_a >> (32u - _sh))) : (_a << (_sh - 32u))); scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x20: os << "        uint _r = _a >> (_b & 31u); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x21: os << "        uint _ah = " << hlsl_op_hi32(inst.src[0]) << "; uint _sh = _b & 63u; uint _rl = (_sh == 0u) ? _a : ((_sh < 32u) ? ((_a >> _sh) | (_ah << (32u - _sh))) : (_ah >> (_sh - 32u))); uint _rh = (_sh < 32u) ? (_ah >> _sh) : 0u; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x22: os << "        uint _r = asuint(asint(_a) >> (_b & 31u)); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x23: os << "        uint _ah = " << hlsl_op_hi32(inst.src[0]) << "; uint _sh = _b & 63u; uint _rl = (_sh == 0u) ? _a : ((_sh < 32u) ? ((_a >> _sh) | (_ah << (32u - _sh))) : asuint(asint(_ah) >> (_sh - 32u))); uint _rh = (_sh < 32u) ? asuint(asint(_ah) >> _sh) : asuint(asint(_ah) >> 31); scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x24: os << "        " << dst << " = _sv_bfm_b32(_a, _b);\n"; break;
                case 0x26: os << "        uint _r = (asint(_a) >= asint(_b)) ? (_a - _b) : (_b - _a); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x27: os << "        uint _r = _sv_sbfe_u32(_a, _b); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x28: os << "        uint _r = _sv_sbfe_i32(_a, _b); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x29: os << "        uint2 _r2 = _sv_sbfe_u64(_a, " << hlsl_op_hi32(inst.src[0]) << ", _b); scc = ((_r2.x | _r2.y) != 0u); " << dst << " = _r2.x;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _r2.y;\n"; break;
                case 0x2A: os << "        uint2 _r2 = _sv_sbfe_i64(_a, " << hlsl_op_hi32(inst.src[0]) << ", _b); scc = ((_r2.x | _r2.y) != 0u); " << dst << " = _r2.x;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _r2.y;\n"; break;
                case 0x2C: os << "        " << dst << " = asuint(asint(_a) * asint(_b));\n"; break;
                case 0x2E: os << "        " << dst << " = (_a << 1u) + _b;\n"; break;
                case 0x2F: os << "        " << dst << " = (_a << 2u) + _b;\n"; break;
                case 0x30: os << "        " << dst << " = (_a << 3u) + _b;\n"; break;
                case 0x31: os << "        " << dst << " = (_a << 4u) + _b;\n"; break;
                case 0x32: os << "        " << dst << " = (_a & 0xFFFFu) | ((_b & 0xFFFFu) << 16u);\n"; break;
                case 0x33: os << "        " << dst << " = (_a & 0xFFFFu) | (_b & 0xFFFF0000u);\n"; break;
                case 0x34: os << "        " << dst << " = (_a >> 16u) | (_b & 0xFFFF0000u);\n"; break;
                case 0x35: os << "        " << dst << " = _sv_mul_hi_u32(_a, _b);\n"; break;
                case 0x36: os << "        " << dst << " = _sv_mul_hi_i32(_a, _b);\n"; break;
                default: os << "        (void)_a; (void)_b;\n"; break;
            }
        } else if (enc == Rdna2Encoding::SOP1) {
            const std::string s0 = (inst.src_count > 0u) ? hlsl_op_u32(inst.src[0]) : "0u";
            const std::string dst = hlsl_dst_lval(inst.dst);
            auto s0_hi_expr = [&]() -> std::string {
                if (inst.src_count > 0u && inst.src[0].kind == Rdna2OperandKind::Sgpr &&
                    static_cast<uint32_t>(inst.src[0].reg_index) + 1u < usage.max_sgpr) {
                    return "s[" + std::to_string(inst.src[0].reg_index + 1u) + "]";
                }
                if (inst.src_count > 0u && inst.src[0].kind == Rdna2OperandKind::VccLo) return "vcc_hi";
                if (inst.src_count > 0u && inst.src[0].kind == Rdna2OperandKind::ExecLo) return "exec_hi";
                return "0u";
            };
            const std::string dst_hi = (inst.dst.kind == Rdna2OperandKind::Sgpr && static_cast<uint32_t>(inst.dst.reg_index) + 1u < usage.max_sgpr)
                                           ? ("s[" + std::to_string(inst.dst.reg_index + 1u) + "]")
                                           : "";
            os << "        uint _a = " << s0 << ";\n";
            switch (raw) {
                case 0x03: os << "        " << dst << " = _a;\n"; break;
                case 0x04: os << "        " << dst << " = _a;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = " << s0_hi_expr() << ";\n"; break;
                case 0x05: os << "        if (scc) " << dst << " = _a;\n"; break;
                case 0x06: os << "        if (scc) { " << dst << " = _a;"; if (!dst_hi.empty()) os << " " << dst_hi << " = " << s0_hi_expr() << ";"; os << " }\n"; break;
                case 0x07: os << "        uint _r = ~_a; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x08: os << "        uint _r = countbits(_a); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x09: os << "        uint _r = countbits(_a) + countbits(" << s0_hi_expr() << "); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x0A: os << "        uint _rl = ~_a; uint _rh = ~" << s0_hi_expr() << "; scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x0B: os << "        uint _r = _sv_wqm32(_a); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x0C: os << "        uint _rl = _sv_wqm32(_a); uint _rh = _sv_wqm32(" << s0_hi_expr() << "); scc = ((_rl | _rh) != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x0D: os << "        " << dst << " = reversebits(_a);\n"; break;
                case 0x0E: os << "        uint _rl = reversebits(" << s0_hi_expr() << "); uint _rh = reversebits(_a); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = _rh;\n"; break;
                case 0x0F: os << "        uint _r = countbits(~_a); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x10: os << "        uint _r = countbits(~_a) + countbits(~" << s0_hi_expr() << "); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x11: os << "        " << dst << " = _sv_ff0_32(_a);\n"; break;
                case 0x13: os << "        " << dst << " = _sv_ff1_32(_a);\n"; break;
                case 0x15: os << "        " << dst << " = _sv_flbit_u32(_a);\n"; break;
                case 0x17: os << "        " << dst << " = _sv_flbit_i32(_a);\n"; break;
                case 0x19: os << "        " << dst << " = _sv_sext8(_a);\n"; break;
                case 0x1A: os << "        " << dst << " = _sv_sext16(_a);\n"; break;
                case 0x1B: os << "        " << dst << " = " << dst << " & ~(1u << (_a & 31u));\n"; break;
                case 0x1C: os << "        uint _bi = _a & 63u; if (_bi < 32u) " << dst << " &= ~(1u << _bi);"; if (!dst_hi.empty()) os << " else " << dst_hi << " &= ~(1u << (_bi - 32u));"; os << "\n"; break;
                case 0x21: os << "        " << dst << " = " << dst << " | (1u << (_a & 31u));\n"; break;
                case 0x22: os << "        uint _bi = _a & 63u; if (_bi < 32u) " << dst << " |= (1u << _bi);"; if (!dst_hi.empty()) os << " else " << dst_hi << " |= (1u << (_bi - 32u));"; os << "\n"; break;
                case 0x2C: os << "        uint _r = _sv_quadmask32(_a); scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x2D: os << "        uint _rl = _sv_quadmask32(_a) | (_sv_quadmask32(" << s0_hi_expr() << ") << 8u); scc = (_rl != 0u); " << dst << " = _rl;\n"; if (!dst_hi.empty()) os << "        " << dst_hi << " = 0u;\n"; break;
                case 0x34: os << "        int _si = asint(_a); uint _r = (_si < 0) ? (0u - _a) : _a; scc = (_r != 0u); " << dst << " = _r;\n"; break;
                case 0x3B: {
                    const uint32_t dreg = inst.dst.reg_index;
                    os << "        s[" << dreg << "] = _sv_bitrep_lo(_a);\n";
                    if (dreg + 1u < usage.max_sgpr) os << "        s[" << (dreg + 1u) << "] = _sv_bitrep_hi(_a);\n";
                    break;
                }
                default: os << "        (void)_a;\n"; break;
            }
        } else if (enc == Rdna2Encoding::SOPK) {
            const std::string dst = hlsl_dst_lval(inst.dst);
            const int32_t simm = static_cast<int32_t>(inst.simm16);
            const uint32_t uimm = static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16));
            switch (raw) {
                case 0x00: os << "        " << dst << " = asuint(int(" << simm << "));\n"; break;
                case 0x02: os << "        if (scc) " << dst << " = asuint(int(" << simm << "));\n"; break;
                case 0x03: os << "        scc = (asint(" << dst << ") == " << simm << ");\n"; break;
                case 0x04: os << "        scc = (asint(" << dst << ") != " << simm << ");\n"; break;
                case 0x05: os << "        scc = (asint(" << dst << ") > " << simm << ");\n"; break;
                case 0x06: os << "        scc = (asint(" << dst << ") >= " << simm << ");\n"; break;
                case 0x07: os << "        scc = (asint(" << dst << ") < " << simm << ");\n"; break;
                case 0x08: os << "        scc = (asint(" << dst << ") <= " << simm << ");\n"; break;
                case 0x09: os << "        scc = (" << dst << " == " << uimm << "u);\n"; break;
                case 0x0A: os << "        scc = (" << dst << " != " << uimm << "u);\n"; break;
                case 0x0B: os << "        scc = (" << dst << " > " << uimm << "u);\n"; break;
                case 0x0C: os << "        scc = (" << dst << " >= " << uimm << "u);\n"; break;
                case 0x0D: os << "        scc = (" << dst << " < " << uimm << "u);\n"; break;
                case 0x0E: os << "        scc = (" << dst << " <= " << uimm << "u);\n"; break;
                case 0x0F: os << "        " << dst << " = asuint(asint(" << dst << ") + " << simm << ");\n"; break;
                case 0x10: os << "        " << dst << " = asuint(asint(" << dst << ") * " << simm << ");\n"; break;
                default: break;
            }
        } else if (enc == Rdna2Encoding::SOPC) {
            const std::string s0 = hlsl_op_u32(inst.src[0]);
            const std::string s1 = hlsl_op_u32(inst.src[1]);
            auto sopc_hi = [&](const Rdna2Operand& op) -> std::string {
                if (op.kind == Rdna2OperandKind::Sgpr && static_cast<uint32_t>(op.reg_index) + 1u < usage.max_sgpr) {
                    return "s[" + std::to_string(op.reg_index + 1u) + "]";
                }
                return "0u";
            };
            os << "        uint _a = " << s0 << "; uint _b = " << s1 << ";\n";
            switch (raw) {
                case 0x00: os << "        scc = (asint(_a) == asint(_b));\n"; break;
                case 0x01: os << "        scc = (asint(_a) != asint(_b));\n"; break;
                case 0x02: os << "        scc = (asint(_a) > asint(_b));\n"; break;
                case 0x03: os << "        scc = (asint(_a) >= asint(_b));\n"; break;
                case 0x04: os << "        scc = (asint(_a) < asint(_b));\n"; break;
                case 0x05: os << "        scc = (asint(_a) <= asint(_b));\n"; break;
                case 0x06: os << "        scc = (_a == _b);\n"; break;
                case 0x07: os << "        scc = (_a != _b);\n"; break;
                case 0x08: os << "        scc = (_a > _b);\n"; break;
                case 0x09: os << "        scc = (_a >= _b);\n"; break;
                case 0x0A: os << "        scc = (_a < _b);\n"; break;
                case 0x0B: os << "        scc = (_a <= _b);\n"; break;
                case 0x0C: os << "        scc = ((_a & (1u << (_b & 31u))) == 0u);\n"; break;
                case 0x0D: os << "        scc = ((_a & (1u << (_b & 31u))) != 0u);\n"; break;
                case 0x0E: os << "        { uint _bi = _b & 63u; uint _w = (_bi < 32u) ? _a : " << sopc_hi(inst.src[0]) << "; scc = ((_w & (1u << (_bi & 31u))) == 0u); }\n"; break;
                case 0x0F: os << "        { uint _bi = _b & 63u; uint _w = (_bi < 32u) ? _a : " << sopc_hi(inst.src[0]) << "; scc = ((_w & (1u << (_bi & 31u))) != 0u); }\n"; break;
                case 0x10: case 0x12: os << "        scc = (_a == _b) && (" << sopc_hi(inst.src[0]) << " == " << sopc_hi(inst.src[1]) << ");\n"; break;
                case 0x11: case 0x13: os << "        scc = (_a != _b) || (" << sopc_hi(inst.src[0]) << " != " << sopc_hi(inst.src[1]) << ");\n"; break;
                default: os << "        (void)_a; (void)_b;\n"; break;
            }
        } else if (enc == Rdna2Encoding::SMEM) {
            if (raw == 0x2Au) {
                if (inst.dst.reg_index < usage.max_sgpr) {
                    os << "        s[" << inst.dst.reg_index << "] = (_gw_base / " << wave_n << "u);\n";
                }
            } else {
                const std::string soff_expr = (inst.soffset < usage.max_sgpr) ? ("s[" + std::to_string(inst.soffset) + "]") : "0u";
                const uint32_t cnt = (raw <= 0x04u) ? (1u << raw)
                                   : ((raw >= 0x08u && raw <= 0x0Cu) ? (1u << (raw - 0x08u))
                                   : ((raw >= 0x10u && raw <= 0x12u) ? (1u << (raw - 0x10u)) : 0u));
                os << "        { int _sm_b = int(" << soff_expr << ") + (" << inst.mem_offset << ");\n";
                for (uint32_t k = 0u; k < cnt && (inst.dst.reg_index + k) < usage.max_sgpr; ++k) {
                    os << "          s[" << (inst.dst.reg_index + k) << "] = (_sm_b < 0) ? 0u : _sv_rc((uint(_sm_b) >> 2u) + " << k << "u);\n";
                }
                os << "        }\n";
            }
        } else if (enc == Rdna2Encoding::VOP1 || (enc == Rdna2Encoding::VOP3 && raw >= 0x180u && raw < 0x200u)) {
            const uint16_t v1op = (enc == Rdna2Encoding::VOP3) ? static_cast<uint16_t>(raw - 0x180u) : raw;
            const bool is_float = (e->data_kind == DK::Float);
            if (v1op == 0x02u) {
                if (container.stage == ShaderStage::Compute || container.stage == ShaderStage::Mesh || container.stage == ShaderStage::Amplification) {
                    os << "        _lane_xchg[_gw_base + _lane] = " << hlsl_op_u32(inst.src[0]) << ";\n";
                    os << "        GroupMemoryBarrierWithGroupSync();\n";
                    os << "        " << hlsl_dst_lval(inst.dst) << " = _lane_xchg[_gw_base];\n";
                    os << "        GroupMemoryBarrierWithGroupSync();\n";
                } else {
                    os << "        " << hlsl_dst_lval(inst.dst) << " = " << hlsl_op_u32(inst.src[0]) << ";\n";
                }
            } else if (v1op != 0x00u && v1op != 0x1Bu && v1op != 0x41u) {
                const std::string dst = hlsl_dst_lval(inst.dst);
                os << "        bool _do_write = true;\n";
                if (mod.kind == SvModifierKind::Dpp16) {
                    os << "        _lane_xchg[_gw_base + _lane] = v[" << static_cast<uint32_t>(mod.vsrc0) << "];\n";
                    os << "        GroupMemoryBarrierWithGroupSync();\n";
                    os << "        uint _a = 0u;\n";
                    os << "        if ((((" << static_cast<uint32_t>(mod.row_mask) << "u >> ((_lane >> 4u) & 3u)) & 1u) != 0u) && (((" << static_cast<uint32_t>(mod.bank_mask) << "u >> ((_lane >> 2u) & 3u)) & 1u) != 0u)) {\n";
                    os << "            int _sl = _sv_dpp16_src_lane(_lane, " << static_cast<uint32_t>(mod.dpp_ctrl) << "u, " << wave_n << "u);\n";
                    os << "            if (_sl < 0) { if (" << (mod.bound_ctrl ? "true" : "false") << ") _a = 0u; else _do_write = false; }\n";
                    os << "            else _a = _lane_xchg[(_gw_base + uint(_sl)) & " << (tg_total - 1u) << "u];\n";
                    os << "        } else { _do_write = false; }\n";
                    os << "        GroupMemoryBarrierWithGroupSync();\n";
                } else if (mod.kind == SvModifierKind::Dpp8 || mod.kind == SvModifierKind::Dpp8Fi) {
                    uint32_t packed_sel = 0u;
                    for (uint32_t k = 0u; k < 8u; ++k) {
                        packed_sel |= (static_cast<uint32_t>(mod.dpp8_sel[k] & 7u) << (k * 3u));
                    }
                    os << "        _lane_xchg[_gw_base + _lane] = v[" << static_cast<uint32_t>(mod.vsrc0) << "];\n";
                    os << "        GroupMemoryBarrierWithGroupSync();\n";
                    os << "        uint _d8_sel = (" << HexLitU32(packed_sel) << " >> ((_lane & 7u) * 3u)) & 7u;\n";
                    os << "        uint _a = _lane_xchg[(_gw_base + ((_lane & ~7u) | _d8_sel)) & " << (tg_total - 1u) << "u];\n";
                    os << "        GroupMemoryBarrierWithGroupSync();\n";
                } else if (mod.kind == SvModifierKind::Sdwa) {
                    const std::string raw_s0 = mod.src0_is_sgpr ? ("s[" + std::to_string(mod.vsrc0) + "]") : ("v[" + std::to_string(mod.vsrc0) + "]");
                    os << "        uint _a = _sv_sdwa_src(" << raw_s0 << ", " << static_cast<uint32_t>(mod.src0_sel) << "u, "
                       << (mod.src0_sext ? "true" : "false") << ", " << (is_float ? "true" : "false") << ", "
                       << (mod.src0_neg ? "true" : "false") << ", " << (mod.src0_abs ? "true" : "false") << ");\n";
                } else {
                    os << "        uint _a = " << hlsl_op_u32(inst.src[0], is_float) << ";\n";
                }
                os << "        uint _r = _a;\n";
                switch (v1op) {
                    case 0x01: os << "        _r = _a;\n"; break;
                    case 0x05: os << "        _r = asuint(float(asint(_a)));\n"; break;
                    case 0x06: os << "        _r = asuint(float(_a));\n"; break;
                    case 0x07: os << "        _r = uint(max(0.0, asfloat(_a)));\n"; break;
                    case 0x08: os << "        _r = asuint(int(asfloat(_a)));\n"; break;
                    case 0x0A: os << "        _r = _sv_f32_to_f16(asfloat(_a));\n"; break;
                    case 0x0B: os << "        _r = asuint(_sv_f16_to_f32(_a));\n"; break;
                    case 0x0C: os << "        _r = asuint(int(floor(asfloat(_a) + 0.5)));\n"; break;
                    case 0x0D: os << "        _r = asuint(int(floor(asfloat(_a))));\n"; break;
                    case 0x11: os << "        _r = asuint(float(_a & 0xFFu));\n"; break;
                    case 0x12: os << "        _r = asuint(float((_a >> 8u) & 0xFFu));\n"; break;
                    case 0x13: os << "        _r = asuint(float((_a >> 16u) & 0xFFu));\n"; break;
                    case 0x14: os << "        _r = asuint(float((_a >> 24u) & 0xFFu));\n"; break;
                    case 0x20: os << "        _r = (_sv_isnan_u32(_a) || _sv_isinf_u32(_a)) ? 0x7FC00000u : asuint(asfloat(_a) - floor(asfloat(_a)));\n"; break;
                    case 0x21: os << "        _r = asuint(trunc(asfloat(_a)));\n"; break;
                    case 0x22: os << "        _r = asuint(ceil(asfloat(_a)));\n"; break;
                    case 0x23: os << "        _r = asuint(round(asfloat(_a)));\n"; break;
                    case 0x24: os << "        _r = asuint(floor(asfloat(_a)));\n"; break;
                    case 0x25: os << "        _r = asuint(exp2(asfloat(_a)));\n"; break;
                    case 0x27: os << "        _r = asuint(log2(asfloat(_a)));\n"; break;
                    case 0x2A: case 0x2B: os << "        _r = _sv_rcp_f32(_a);\n"; break;
                    case 0x2E: os << "        _r = _sv_rsq_f32(_a);\n"; break;
                    case 0x33: os << "        _r = _sv_sqrt_f32(_a);\n"; break;
                    case 0x35: os << "        _r = asuint(_sv_sin_turns_f32(asfloat(_a)));\n"; break;
                    case 0x36: os << "        _r = asuint(_sv_cos_turns_f32(asfloat(_a)));\n"; break;
                    case 0x37: os << "        _r = ~_a;\n"; break;
                    case 0x38: os << "        _r = reversebits(_a);\n"; break;
                    case 0x39: os << "        _r = _sv_flbit_u32(_a);\n"; break;
                    case 0x3A: os << "        _r = _sv_ff1_32(_a);\n"; break;
                    case 0x3B: os << "        _r = _sv_flbit_i32(_a);\n"; break;
                    case 0x3F: os << "        _r = _sv_frexp_exp_f32(asfloat(_a));\n"; break;
                    case 0x40: os << "        _r = asuint(_sv_frexp_mant_f32(asfloat(_a)));\n"; break;
                    case 0x50: os << "        _r = _sv_f32_to_f16(float(_a & 0xFFFFu));\n"; break;
                    case 0x51: os << "        _r = _sv_f32_to_f16(float(asint(_sv_sext16(_a))));\n"; break;
                    case 0x52: os << "        { float _hf = _sv_f16_to_f32(_a); _r = (_sv_isnan_u16(_a) || _hf <= 0.0) ? 0u : uint(min(_hf, 65535.0)); }\n"; break;
                    case 0x53: os << "        { float _hf = _sv_f16_to_f32(_a); _r = asuint(int(clamp(_hf, -32768.0, 32767.0))) & 0xFFFFu; }\n"; break;
                    case 0x54: os << "        _r = _sv_f32_to_f16(1.0 / _sv_f16_to_f32(_a));\n"; break;
                    case 0x55: os << "        _r = _sv_f32_to_f16(sqrt(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x56: os << "        _r = _sv_f32_to_f16(rsqrt(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x57: os << "        _r = _sv_f32_to_f16(log2(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x58: os << "        _r = _sv_f32_to_f16(exp2(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x59: os << "        _r = _sv_f32_to_f16(_sv_frexp_mant_f32(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x5A: os << "        _r = _sv_frexp_exp_f32(_sv_f16_to_f32(_a)) & 0xFFFFu;\n"; break;
                    case 0x5B: os << "        _r = _sv_f32_to_f16(floor(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x5C: os << "        _r = _sv_f32_to_f16(ceil(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x5D: os << "        _r = _sv_f32_to_f16(trunc(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x5E: os << "        _r = _sv_f32_to_f16(round(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x5F: os << "        { uint _ha = _a & 0x7FFFu; float _hf = _sv_f16_to_f32(_a); _r = (_ha >= 0x7C00u) ? 0x7E00u : _sv_f32_to_f16(_hf - floor(_hf)); }\n"; break;
                    case 0x60: os << "        _r = _sv_f32_to_f16(_sv_sin_turns_f32(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x61: os << "        _r = _sv_f32_to_f16(_sv_cos_turns_f32(_sv_f16_to_f32(_a)));\n"; break;
                    case 0x62: os << "        _r = uint(clamp(asint(_sv_sext16(_a)), 0, 255)) | (uint(clamp(asint(_sv_sext16(_a >> 16u)), 0, 255)) << 8u);\n"; break;
                    case 0x63: os << "        { float _hf = _sv_f16_to_f32(_a); float _cl = _sv_isnan_u16(_a) ? 0.0 : clamp(_hf, -1.0, 1.0); _r = asuint(int(round(_cl * 32767.0))) & 0xFFFFu; }\n"; break;
                    case 0x64: os << "        { float _hf = _sv_f16_to_f32(_a); float _cl = _sv_isnan_u16(_a) ? 0.0 : clamp(_hf, 0.0, 1.0); _r = uint(round(_cl * 65535.0)) & 0xFFFFu; }\n"; break;
                    case 0x65:
                        if (inst.src[0].kind == Rdna2OperandKind::Vgpr && inst.src[0].reg_index < usage.max_vgpr) {
                            os << "        { uint _swp_old = " << dst << "; v[" << inst.src[0].reg_index << "] = _swp_old; _r = _a; }\n";
                        } else {
                            os << "        _r = _a;\n";
                        }
                        break;
                    default: break;
                }
                if (mod.kind == SvModifierKind::Sdwa) {
                    os << "        if (_do_write) " << dst << " = _sv_sdwa_dst(" << dst << ", _r, " << static_cast<uint32_t>(mod.dst_sel) << "u, " << static_cast<uint32_t>(mod.dst_unused) << "u);\n";
                } else {
                    os << "        if (_do_write) " << dst << " = _r;\n";
                }
            }
        } else if (enc == Rdna2Encoding::VOP2 || (enc == Rdna2Encoding::VOP3 && raw >= 0x100u && raw < 0x140u)) {
            const uint16_t v2op = (enc == Rdna2Encoding::VOP3) ? static_cast<uint16_t>(raw - 0x100u) : raw;
            const bool is_float = (e->data_kind == DK::Float);
            const std::string dst = hlsl_dst_lval(inst.dst);
            os << "        bool _do_write = true;\n";
            if (mod.kind == SvModifierKind::Dpp16) {
                os << "        _lane_xchg[_gw_base + _lane] = v[" << static_cast<uint32_t>(mod.vsrc0) << "];\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
                os << "        uint _a = 0u;\n";
                os << "        if ((((" << static_cast<uint32_t>(mod.row_mask) << "u >> ((_lane >> 4u) & 3u)) & 1u) != 0u) && (((" << static_cast<uint32_t>(mod.bank_mask) << "u >> ((_lane >> 2u) & 3u)) & 1u) != 0u)) {\n";
                os << "            int _sl = _sv_dpp16_src_lane(_lane, " << static_cast<uint32_t>(mod.dpp_ctrl) << "u, " << wave_n << "u);\n";
                os << "            if (_sl < 0) { if (" << (mod.bound_ctrl ? "true" : "false") << ") _a = 0u; else _do_write = false; }\n";
                os << "            else _a = _lane_xchg[(_gw_base + uint(_sl)) & " << (tg_total - 1u) << "u];\n";
                os << "        } else { _do_write = false; }\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
                os << "        uint _b = " << hlsl_op_u32(inst.src[1], is_float) << ";\n";
            } else if (mod.kind == SvModifierKind::Dpp8 || mod.kind == SvModifierKind::Dpp8Fi) {
                uint32_t packed_sel = 0u;
                for (uint32_t k = 0u; k < 8u; ++k) {
                    packed_sel |= (static_cast<uint32_t>(mod.dpp8_sel[k] & 7u) << (k * 3u));
                }
                os << "        _lane_xchg[_gw_base + _lane] = v[" << static_cast<uint32_t>(mod.vsrc0) << "];\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
                os << "        uint _d8_sel = (" << HexLitU32(packed_sel) << " >> ((_lane & 7u) * 3u)) & 7u;\n";
                os << "        uint _a = _lane_xchg[(_gw_base + ((_lane & ~7u) | _d8_sel)) & " << (tg_total - 1u) << "u];\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
                os << "        uint _b = " << hlsl_op_u32(inst.src[1], is_float) << ";\n";
            } else if (mod.kind == SvModifierKind::Sdwa) {
                const std::string raw_s0 = mod.src0_is_sgpr ? ("s[" + std::to_string(mod.vsrc0) + "]") : ("v[" + std::to_string(mod.vsrc0) + "]");
                const std::string raw_s1 = hlsl_op_u32(inst.src[1], false);
                os << "        uint _a = _sv_sdwa_src(" << raw_s0 << ", " << static_cast<uint32_t>(mod.src0_sel) << "u, "
                   << (mod.src0_sext ? "true" : "false") << ", " << (is_float ? "true" : "false") << ", "
                   << (mod.src0_neg ? "true" : "false") << ", " << (mod.src0_abs ? "true" : "false") << ");\n";
                os << "        uint _b = _sv_sdwa_src(" << raw_s1 << ", " << static_cast<uint32_t>(mod.src1_sel) << "u, "
                   << (mod.src1_sext ? "true" : "false") << ", " << (is_float ? "true" : "false") << ", "
                   << (mod.src1_neg ? "true" : "false") << ", " << (mod.src1_abs ? "true" : "false") << ");\n";
            } else {
                os << "        uint _a = " << hlsl_op_u32(inst.src[0], is_float) << ";\n";
                os << "        uint _b = " << hlsl_op_u32(inst.src[1], is_float) << ";\n";
            }
            os << "        uint _r = 0u;\n";
            switch (v2op) {
                case 0x01: os << "        { uint _bw_t = _b; uint _bw_f = _a; _r = _vcc_bit ? _bw_t : _bw_f; }\n"; break;
                case 0x02: os << "        _r = asuint(_sv_f16_to_f32(_a)*_sv_f16_to_f32(_b) + _sv_f16_to_f32(_a>>16u)*_sv_f16_to_f32(_b>>16u) + asfloat(" << dst << "));\n"; break;
                case 0x03: os << "        _r = asuint(asfloat(_a) + asfloat(_b));\n"; break;
                case 0x04: os << "        _r = asuint(asfloat(_a) - asfloat(_b));\n"; break;
                case 0x05: os << "        _r = asuint(asfloat(_b) - asfloat(_a));\n"; break;
                case 0x06: os << "        _r = asuint(((asfloat(_a) == 0.0 || asfloat(_b) == 0.0) ? 0.0 : (asfloat(_a) * asfloat(_b))) + asfloat(" << dst << "));\n"; break;
                case 0x07: os << "        _r = asuint((asfloat(_a) == 0.0 || asfloat(_b) == 0.0) ? 0.0 : (asfloat(_a) * asfloat(_b)));\n"; break;
                case 0x08: os << "        _r = asuint(asfloat(_a) * asfloat(_b));\n"; break;
                case 0x09: os << "        _r = asuint((asint((_a & 0xFFFFFFu) << 8u) >> 8) * (asint((_b & 0xFFFFFFu) << 8u) >> 8));\n"; break;
                case 0x0A: os << "        _r = _sv_mul_hi_i32(asuint(asint((_a & 0xFFFFFFu) << 8u) >> 8), asuint(asint((_b & 0xFFFFFFu) << 8u) >> 8));\n"; break;
                case 0x0B: os << "        _r = (_a & 0xFFFFFFu) * (_b & 0xFFFFFFu);\n"; break;
                case 0x0C: os << "        _r = _sv_mul_hi_u32(_a & 0xFFFFFFu, _b & 0xFFFFFFu);\n"; break;
                case 0x0D: os << "        _r = _sv_dot4_i8(_a, _b, " << dst << ");\n"; break;
                case 0x0E: os << "        _r = _sv_dot8_i4(_a, _b, " << dst << ");\n"; break;
                case 0x0F: os << "        _r = asuint(min(asfloat(_a), asfloat(_b)));\n"; break;
                case 0x10: os << "        _r = asuint(max(asfloat(_a), asfloat(_b)));\n"; break;
                case 0x11: os << "        _r = asuint(min(asint(_a), asint(_b)));\n"; break;
                case 0x12: os << "        _r = asuint(max(asint(_a), asint(_b)));\n"; break;
                case 0x13: os << "        _r = min(_a, _b);\n"; break;
                case 0x14: os << "        _r = max(_a, _b);\n"; break;
                case 0x16: os << "        _r = _b >> (_a & 31u);\n"; break;
                case 0x17: os << "        _r = asuint(asint(_b) >> (_a & 31u));\n"; break;
                case 0x18: os << "        _r = _b << (_a & 31u);\n"; break;
                case 0x1B: os << "        _r = _a & _b;\n"; break;
                case 0x1C: os << "        _r = _a | _b;\n"; break;
                case 0x1D: os << "        _r = _a ^ _b;\n"; break;
                case 0x1E: os << "        _r = ~(_a ^ _b);\n"; break;
                case 0x1F: os << "        _r = asuint(asfloat(_a) * asfloat(_b) + asfloat(" << dst << "));\n"; break;
                case 0x21: os << "        _r = asuint(asfloat(_a) * asfloat(" << HexLitU32(inst.src[1].literal_bits) << ") + asfloat(" << hlsl_op_u32(inst.src[2]) << "));\n"; break;
                case 0x22: os << "        _r = asuint(asfloat(_a) * asfloat(_b) + asfloat(" << HexLitU32(inst.src[2].literal_bits) << "));\n"; break;
                case 0x25: os << "        _r = _a + _b;\n"; break;
                case 0x26: os << "        _r = _a - _b;\n"; break;
                case 0x27: os << "        _r = _b - _a;\n"; break;
                case 0x28: os << "        { uint _ci = _vcc_bit ? 1u : 0u; _r = _a + _b + _ci; _vcc_bit = (_r < _a) || (_ci != 0u && _r == _a); }\n"; break;
                case 0x29: os << "        { uint _bi = _vcc_bit ? 1u : 0u; _r = _a - _b - _bi; _vcc_bit = (_b > _a) || (_bi != 0u && _b == _a); }\n"; break;
                case 0x2A: os << "        { uint _bi = _vcc_bit ? 1u : 0u; _r = _b - _a - _bi; _vcc_bit = (_a > _b) || (_bi != 0u && _a == _b); }\n"; break;
                case 0x2B: os << "        _r = asuint(_sv_fma_f32(asfloat(_a), asfloat(_b), asfloat(" << dst << ")));\n"; break;
                case 0x2C: os << "        _r = asuint(_sv_fma_f32(asfloat(_a), asfloat(" << HexLitU32(inst.src[1].literal_bits) << "), asfloat(" << hlsl_op_u32(inst.src[2]) << ")));\n"; break;
                case 0x2D: os << "        _r = asuint(_sv_fma_f32(asfloat(_a), asfloat(_b), asfloat(" << HexLitU32(inst.src[2].literal_bits) << ")));\n"; break;
                case 0x2F: os << "        _r = _sv_f32_to_f16(asfloat(_a)) | (_sv_f32_to_f16(asfloat(_b)) << 16u);\n"; break;
                case 0x32: os << "        _r = _sv_f32_to_f16(_sv_f16_to_f32(_a) + _sv_f16_to_f32(_b));\n"; break;
                case 0x33: os << "        _r = _sv_f32_to_f16(_sv_f16_to_f32(_a) - _sv_f16_to_f32(_b));\n"; break;
                case 0x34: os << "        _r = _sv_f32_to_f16(_sv_f16_to_f32(_b) - _sv_f16_to_f32(_a));\n"; break;
                case 0x35: os << "        _r = _sv_f32_to_f16(_sv_f16_to_f32(_a) * _sv_f16_to_f32(_b));\n"; break;
                case 0x36: os << "        _r = _sv_f32_to_f16(_sv_fma_f32(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b), _sv_f16_to_f32(" << dst << ")));\n"; break;
                case 0x37: os << "        _r = _sv_f32_to_f16(_sv_fma_f32(_sv_f16_to_f32(_a), _sv_f16_to_f32(" << HexLitU32(inst.src[1].literal_bits) << "), _sv_f16_to_f32(" << hlsl_op_u32(inst.src[2]) << ")));\n"; break;
                case 0x38: os << "        _r = _sv_f32_to_f16(_sv_fma_f32(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b), _sv_f16_to_f32(" << HexLitU32(inst.src[2].literal_bits) << ")));\n"; break;
                case 0x39: os << "        _r = _sv_f32_to_f16(max(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b)));\n"; break;
                case 0x3A: os << "        _r = _sv_f32_to_f16(min(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b)));\n"; break;
                case 0x3B: os << "        _r = _sv_f32_to_f16(ldexp(_sv_f16_to_f32(_a), float(asint(_sv_sext16(_b)))));\n"; break;
                case 0x3C: os << "        _r = _sv_f32_to_f16(_sv_fma_f32(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b), _sv_f16_to_f32(" << dst << "))) | (_sv_f32_to_f16(_sv_fma_f32(_sv_f16_to_f32(_a>>16u), _sv_f16_to_f32(_b>>16u), _sv_f16_to_f32(" << dst << ">>16u))) << 16u);\n"; break;
                default: os << "        _r = _a + _b;\n"; break;
            }
            if (mod.kind == SvModifierKind::Sdwa) {
                os << "        if (_do_write) " << dst << " = _sv_sdwa_dst(" << dst << ", _r, " << static_cast<uint32_t>(mod.dst_sel) << "u, " << static_cast<uint32_t>(mod.dst_unused) << "u);\n";
            } else {
                os << "        if (_do_write) " << dst << " = _r;\n";
            }
        } else if (enc == Rdna2Encoding::VOPC || (enc == Rdna2Encoding::VOP3 && raw < 0x100u)) {
            const uint16_t cop = raw;
            const bool is_f16_cmp = (cop == 0x8Fu || cop == 0x9Fu ||
                                     (cop >= 0xC8u && cop <= 0xCFu) || (cop >= 0xD8u && cop <= 0xDFu) ||
                                     (cop >= 0xE8u && cop <= 0xEFu) || (cop >= 0xF8u && cop <= 0xFFu));
            const bool is_f64_cmp = (cop == 0xA8u || cop == 0xB8u ||
                                     (cop >= 0x20u && cop <= 0x2Fu) || (cop >= 0x40u && cop <= 0x4Fu));
            const bool is_i64_cmp = ((cop >= 0xA0u && cop <= 0xA7u) || (cop >= 0xB0u && cop <= 0xB7u));
            const bool is_u64_cmp = ((cop >= 0xE0u && cop <= 0xE7u) || (cop >= 0xF0u && cop <= 0xF7u));
            auto hlsl_op_hi = [&](const Rdna2Operand& op, bool apply_f64_mod) -> std::string {
                std::string expr = "0u";
                if (op.kind == Rdna2OperandKind::Vgpr && static_cast<uint32_t>(op.reg_index) + 1u < usage.max_vgpr) {
                    expr = "v[" + std::to_string(op.reg_index + 1u) + "]";
                } else if (op.kind == Rdna2OperandKind::Sgpr && static_cast<uint32_t>(op.reg_index) + 1u < usage.max_sgpr) {
                    expr = "s[" + std::to_string(op.reg_index + 1u) + "]";
                }
                if (apply_f64_mod) {
                    if (op.abs) expr = "(" + expr + " & 0x7FFFFFFFu)";
                    if (op.neg) expr = "(" + expr + " ^ 0x80000000u)";
                }
                return expr;
            };
            auto hlsl_op_f16 = [&](const Rdna2Operand& op) -> std::string {
                std::string expr = "(" + hlsl_op_u32(op, false) + " & 0xFFFFu)";
                if (op.abs) expr = "(" + expr + " & 0x7FFFu)";
                if (op.neg) expr = "(" + expr + " ^ 0x8000u)";
                return expr;
            };
            if (is_f16_cmp) {
                os << "        uint _a = " << hlsl_op_f16(inst.src[0]) << ";\n";
                os << "        uint _b = " << ((cop == 0x8Fu || cop == 0x9Fu) ? hlsl_op_u32(inst.src[1], false) : hlsl_op_f16(inst.src[1])) << ";\n";
            } else if (is_f64_cmp || is_i64_cmp || is_u64_cmp) {
                os << "        uint _a = " << hlsl_op_u32(inst.src[0], false) << "; uint _ah = " << hlsl_op_hi(inst.src[0], is_f64_cmp) << ";\n";
                os << "        uint _b = " << hlsl_op_u32(inst.src[1], false) << "; uint _bh = " << hlsl_op_hi(inst.src[1], is_f64_cmp && cop != 0xA8u && cop != 0xB8u) << ";\n";
            } else {
                os << "        uint _a = " << hlsl_op_u32(inst.src[0], e->data_kind == DK::Float) << ";\n";
                os << "        uint _b = " << hlsl_op_u32(inst.src[1], (e->data_kind == DK::Float) && cop != 0x88u && cop != 0x98u) << ";\n";
            }
            if (cop == 0x88u || cop == 0x98u) {
                os << "        _vcc_bit = _sv_fclass_f32_bits(_a, _b);\n";
            } else if (cop == 0xA8u || cop == 0xB8u) {
                os << "        _vcc_bit = _sv_fclass_f64_bits(_a, _ah, _b);\n";
            } else if (cop == 0x8Fu || cop == 0x9Fu) {
                os << "        _vcc_bit = _sv_fclass_f16_bits(_a, _b);\n";
            } else if ((cop >= 0x10u && cop <= 0x1Fu) || (cop >= 0x30u && cop <= 0x3Fu)) {
                os << "        _vcc_bit = _sv_cmp_f32_bits(_a, _b, " << (cop & 15u) << "u);\n";
            } else if ((cop >= 0x20u && cop <= 0x2Fu) || (cop >= 0x40u && cop <= 0x4Fu)) {
                os << "        _vcc_bit = _sv_cmp_f64_bits(_a, _ah, _b, _bh, " << (cop & 15u) << "u);\n";
            } else if (is_f16_cmp) {
                const uint32_t cc = ((cop & 0x10u) ? 8u : 0u) | (cop & 7u);
                os << "        _vcc_bit = _sv_cmp_f16_bits(_a, _b, " << cc << "u);\n";
            } else if (is_i64_cmp) {
                os << "        _vcc_bit = _sv_cmp_i64(_a, _ah, _b, _bh, " << (cop & 7u) << "u);\n";
            } else if (is_u64_cmp) {
                os << "        _vcc_bit = _sv_cmp_u64(_a, _ah, _b, _bh, " << (cop & 7u) << "u);\n";
            } else if ((cop >= 0x50u && cop <= 0x57u) || (cop >= 0x70u && cop <= 0x77u)) {
                static constexpr const char* kI16Op[8] = {"false", "(asint(_sv_sext16(_a))<asint(_sv_sext16(_b)))", "(asint(_sv_sext16(_a))==asint(_sv_sext16(_b)))", "(asint(_sv_sext16(_a))<=asint(_sv_sext16(_b)))",
                                                          "(asint(_sv_sext16(_a))>asint(_sv_sext16(_b)))", "(asint(_sv_sext16(_a))!=asint(_sv_sext16(_b)))", "(asint(_sv_sext16(_a))>=asint(_sv_sext16(_b)))", "true"};
                os << "        _vcc_bit = " << kI16Op[cop & 7u] << ";\n";
            } else if ((cop >= 0x60u && cop <= 0x67u) || (cop >= 0x78u && cop <= 0x7Fu)) {
                static constexpr const char* kU16Op[8] = {"false", "((_a&0xFFFFu)<(_b&0xFFFFu))", "((_a&0xFFFFu)==(_b&0xFFFFu))", "((_a&0xFFFFu)<=(_b&0xFFFFu))",
                                                          "((_a&0xFFFFu)>(_b&0xFFFFu))", "((_a&0xFFFFu)!=(_b&0xFFFFu))", "((_a&0xFFFFu)>=(_b&0xFFFFu))", "true"};
                os << "        _vcc_bit = " << kU16Op[cop & 7u] << ";\n";
            } else if ((cop >= 0x80u && cop <= 0x87u) || (cop >= 0x90u && cop <= 0x97u)) {
                static constexpr const char* kIOp[8] = {"false", "(asint(_a)<asint(_b))", "(asint(_a)==asint(_b))", "(asint(_a)<=asint(_b))",
                                                        "(asint(_a)>asint(_b))", "(asint(_a)!=asint(_b))", "(asint(_a)>=asint(_b))", "true"};
                os << "        _vcc_bit = " << kIOp[cop & 7u] << ";\n";
            } else {
                static constexpr const char* kUOp[8] = {"false", "(_a<_b)", "(_a==_b)", "(_a<=_b)", "(_a>_b)", "(_a!=_b)", "(_a>=_b)", "true"};
                os << "        _vcc_bit = " << kUOp[cop & 7u] << ";\n";
            }
        } else if (enc == Rdna2Encoding::VOP3) {
            const bool is_float = (e->data_kind == DK::Float);
            const std::string dst = hlsl_dst_lval(inst.dst);
            if (raw == 0x360u) {
                os << "        _lane_xchg[_gw_base + _lane] = " << hlsl_op_u32(inst.src[0]) << ";\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
                os << "        " << dst << " = _lane_xchg[_gw_base + (" << hlsl_op_u32(inst.src[1]) << " & " << lane_mask << "u)];\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
            } else if (raw == 0x361u) {
                os << "        if (_lane == (" << hlsl_op_u32(inst.src[1]) << " & " << lane_mask << "u)) " << dst << " = " << hlsl_op_u32(inst.src[0]) << ";\n";
            } else if (raw == 0x377u || raw == 0x378u) {
                const bool cross = (raw == 0x378u);
                os << "        _lane_xchg[_gw_base + _lane] = " << hlsl_op_u32(inst.src[0]) << ";\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
                os << "        uint _s1 = " << hlsl_op_u32(inst.src[1]) << "; uint _s2 = " << hlsl_op_u32(inst.src[2]) << ";\n";
                os << "        uint _rl = _lane & 15u;\n";
                os << "        uint _rb = " << (cross ? "((_lane & ~15u) ^ 16u)" : "(_lane & ~15u)") << ";\n";
                os << "        uint _sel = (((_rl < 8u) ? _s1 : _s2) >> ((_rl & 7u) * 4u)) & 15u;\n";
                os << "        " << dst << " = _lane_xchg[_gw_base + ((_rb | _sel) & " << lane_mask << "u)];\n";
                os << "        GroupMemoryBarrierWithGroupSync();\n";
            } else {
                os << "        uint _a = " << hlsl_op_u32(inst.src[0], is_float) << ";\n";
                os << "        uint _b = " << (inst.src_count >= 2u ? hlsl_op_u32(inst.src[1], is_float) : "0u") << ";\n";
                os << "        uint _c = " << (inst.src_count >= 3u ? hlsl_op_u32(inst.src[2], is_float) : "0u") << ";\n";
                os << "        uint _r = 0u;\n";
                switch (raw) {
                    case 0x140: os << "        _r = asuint(((asfloat(_a) == 0.0 || asfloat(_b) == 0.0) ? 0.0 : (asfloat(_a) * asfloat(_b))) + asfloat(_c));\n"; break;
                    case 0x141: os << "        _r = asuint(asfloat(_a) * asfloat(_b) + asfloat(_c));\n"; break;
                    case 0x142: os << "        _r = asuint((asint((_a & 0xFFFFFFu) << 8u) >> 8) * (asint((_b & 0xFFFFFFu) << 8u) >> 8) + asint(_c));\n"; break;
                    case 0x143: os << "        _r = (_a & 0xFFFFFFu) * (_b & 0xFFFFFFu) + _c;\n"; break;
                    case 0x144: os << "        { float _ax = abs(asfloat(_a)), _ay = abs(asfloat(_b)), _az = abs(asfloat(_c)); _r = asuint((_az >= _ax && _az >= _ay) ? (asfloat(_c) < 0.0 ? 5.0 : 4.0) : ((_ay >= _ax) ? (asfloat(_b) < 0.0 ? 3.0 : 2.0) : (asfloat(_a) < 0.0 ? 1.0 : 0.0))); }\n"; break;
                    case 0x145: os << "        { float _ax = abs(asfloat(_a)), _ay = abs(asfloat(_b)), _az = abs(asfloat(_c)); _r = asuint((_az >= _ax && _az >= _ay) ? (asfloat(_c) < 0.0 ? -asfloat(_a) : asfloat(_a)) : ((_ay >= _ax) ? asfloat(_a) : (asfloat(_a) < 0.0 ? asfloat(_c) : -asfloat(_c)))); }\n"; break;
                    case 0x146: os << "        { float _ax = abs(asfloat(_a)), _ay = abs(asfloat(_b)), _az = abs(asfloat(_c)); _r = asuint((_az >= _ax && _az >= _ay) ? -asfloat(_b) : ((_ay >= _ax) ? (asfloat(_b) < 0.0 ? -asfloat(_c) : asfloat(_c)) : -asfloat(_b))); }\n"; break;
                    case 0x147: os << "        _r = asuint(2.0 * max(max(abs(asfloat(_a)), abs(asfloat(_b))), abs(asfloat(_c))));\n"; break;
                    case 0x148: os << "        _r = _sv_bfe_u32(_a, _b, _c);\n"; break;
                    case 0x149: os << "        _r = _sv_bfe_i32(_a, _b, _c);\n"; break;
                    case 0x14A: os << "        _r = (_a & _b) | (~_a & _c);\n"; break;
                    case 0x14B: os << "        _r = asuint(_sv_fma_f32(asfloat(_a), asfloat(_b), asfloat(_c)));\n"; break;
                    case 0x162: os << "        { float _fm = _sv_fma_f32(asfloat(_a), asfloat(_b), asfloat(_c)); if (_vcc_bit) { uint _ec = (_c >> 23u) & 0xFFu; _fm = ldexp(_fm, (_ec > 127u) ? 64.0 : -64.0); } _r = asuint(_fm); }\n"; break;
                    case 0x14C: os << "        _r = _sv_alignbyte(_a, _b, _c);\n"; break;
                    case 0x14E: os << "        _r = _sv_lerp_u8(_a, _b, _c);\n"; break;
                    case 0x14F: os << "        _r = _sv_alignbit(_a, _b, _c);\n"; break;
                    case 0x150: os << "        _r = asuint((asfloat(_a) > 0.0) ? (asfloat(_a) * asfloat(_b) + asfloat(_c)) : asfloat(_c));\n"; break;
                    case 0x151: os << "        _r = asuint(min(min(asfloat(_a), asfloat(_b)), asfloat(_c)));\n"; break;
                    case 0x152: os << "        _r = asuint(min(min(asint(_a), asint(_b)), asint(_c)));\n"; break;
                    case 0x153: os << "        _r = min(min(_a, _b), _c);\n"; break;
                    case 0x154: os << "        _r = asuint(max(max(asfloat(_a), asfloat(_b)), asfloat(_c)));\n"; break;
                    case 0x155: os << "        _r = asuint(max(max(asint(_a), asint(_b)), asint(_c)));\n"; break;
                    case 0x156: os << "        _r = max(max(_a, _b), _c);\n"; break;
                    case 0x157: os << "        _r = asuint(max(min(asfloat(_a), asfloat(_b)), min(max(asfloat(_a), asfloat(_b)), asfloat(_c))));\n"; break;
                    case 0x158: os << "        _r = asuint(max(min(asint(_a), asint(_b)), min(max(asint(_a), asint(_b)), asint(_c))));\n"; break;
                    case 0x159: os << "        _r = max(min(_a, _b), min(max(_a, _b), _c));\n"; break;
                    case 0x15A: os << "        _r = _sv_sad_u8(_a, _b, _c);\n"; break;
                    case 0x15B: os << "        _r = _c + (_sv_sad_u8(_a, _b, 0u) << 16u);\n"; break;
                    case 0x15C: os << "        _r = _sv_sad_u16(_a, _b, _c);\n"; break;
                    case 0x15D: os << "        _r = _c + ((_a >= _b) ? (_a - _b) : (_b - _a));\n"; break;
                    case 0x15E: os << "        { uint _bv = uint(clamp(asfloat(_a), 0.0, 255.0)) & 0xFFu; uint _sh = (_b & 3u) * 8u; _r = (_c & ~(0xFFu << _sh)) | (_bv << _sh); }\n"; break;
                    case 0x15F: os << "        _r = asuint(_sv_div_fixup_f32(asfloat(_a), asfloat(_b), asfloat(_c)));\n"; break;
                    case 0x161: os << "        _r = asuint(ldexp(asfloat(_a), float(asint(_b))));\n"; break;
                    case 0x164: case 0x168: os << "        _r = _sv_msad_u8(_a, _b, _c);\n"; break;
                    case 0x169: os << "        _r = _a * _b;\n"; break;
                    case 0x16A: os << "        _r = _sv_mul_hi_u32(_a, _b);\n"; break;
                    case 0x16B: os << "        _r = _sv_mul_hi_i32(_a, _b);\n"; break;
                    case 0x178: os << "        _r = countbits(_a) + _b;\n"; break;
                    case 0x179: os << "        _r = asuint((asfloat(_a) == 0.0 || asfloat(_b) == 0.0) ? asfloat(_c) : _sv_fma_f32(asfloat(_a), asfloat(_b), asfloat(_c)));\n"; break;
                    case 0x303: os << "        _r = (_a + _b) & 0xFFFFu;\n"; break;
                    case 0x304: os << "        _r = (_a - _b) & 0xFFFFu;\n"; break;
                    case 0x305: os << "        _r = countbits(_a & ((_lane < 32u) ? ((1u << _lane) - 1u) : 0xFFFFFFFFu)) + _b;\n"; break;
                    case 0x306: os << "        _r = countbits(_a & ((_lane >= 32u) ? ((1u << (_lane - 32u)) - 1u) : 0u)) + _b;\n"; break;
                    case 0x307: os << "        _r = (asuint(asint(_sv_sext16(_a)) + asint(_sv_sext16(_b)))) & 0xFFFFu;\n"; break;
                    case 0x308: os << "        _r = (asuint(asint(_sv_sext16(_a)) - asint(_sv_sext16(_b)))) & 0xFFFFu;\n"; break;
                    case 0x309: os << "        _r = min(_a & 0xFFFFu, _b & 0xFFFFu);\n"; break;
                    case 0x30A: os << "        _r = max(_a & 0xFFFFu, _b & 0xFFFFu);\n"; break;
                    case 0x30B: os << "        _r = asuint(min(asint(_sv_sext16(_a)), asint(_sv_sext16(_b)))) & 0xFFFFu;\n"; break;
                    case 0x30C: os << "        _r = asuint(max(asint(_sv_sext16(_a)), asint(_sv_sext16(_b)))) & 0xFFFFu;\n"; break;
                    case 0x30D: os << "        _r = _a + _b; _vcc_bit = (_r < _a);\n"; break;
                    case 0x30E: os << "        _r = _a - _b; _vcc_bit = (_b > _a);\n"; break;
                    case 0x30F: os << "        _r = _b - _a; _vcc_bit = (_a > _b);\n"; break;
                    case 0x311: os << "        _r = (_a & 0xFFFFu) | ((_b & 0xFFFFu) << 16u);\n"; break;
                    // Avoid ((_a & 0xFFFFu) * (_b & 0xFFFFu)) & 0xFFFFu: AMD RDNA2 drivers (RX 6750 XT, 32.0.21045.16010)
                    // elide the outer & 0xFFFFu mask on that pattern in both D3D12 and Vulkan (#2).
                    // Mod 2^16, (_a * _b) & 0xFFFFu is algebraically identical for all 32-bit inputs.
                    case 0x312: os << "        _r = (_a * _b) & 0xFFFFu;\n"; break;
                    case 0x313: os << "        _r = asuint(asint(_sv_sext16(_b)) >> (_a & 15u)) & 0xFFFFu;\n"; break;
                    case 0x314: os << "        _r = ((_b & 0xFFFFu) << (_a & 15u)) & 0xFFFFu;\n"; break;
                    case 0x315: os << "        _r = (_b & 0xFFFFu) >> (_a & 15u);\n"; break;
                    case 0x340: os << "        _r = ((_a & 0xFFFFu) * (_b & 0xFFFFu) + (_c & 0xFFFFu)) & 0xFFFFu;\n"; break;
                    case 0x341: os << "        _r = asuint(asint(_sv_sext16(_a)) * asint(_sv_sext16(_b)) + asint(_sv_sext16(_c))) & 0xFFFFu;\n"; break;
                    case 0x344: os << "        _r = _sv_perm_b32(_a, _b, _c);\n"; break;
                    case 0x34B: os << "        _r = _sv_f32_to_f16(_sv_fma_f32(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b), _sv_f16_to_f32(_c)));\n"; break;
                    case 0x34C: os << "        _r = _sv_f32_to_f16(_sv_div_fixup_f32(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b), _sv_f16_to_f32(_c)));\n"; break;
                    case 0x34D: os << "        _r = _sv_f32_to_f16(min(min(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b)), _sv_f16_to_f32(_c)));\n"; break;
                    case 0x34E: os << "        _r = asuint(min(min(asint(_sv_sext16(_a)), asint(_sv_sext16(_b))), asint(_sv_sext16(_c)))) & 0xFFFFu;\n"; break;
                    case 0x34F: os << "        _r = min(min(_a & 0xFFFFu, _b & 0xFFFFu), _c & 0xFFFFu);\n"; break;
                    case 0x350: os << "        _r = _sv_f32_to_f16(max(max(_sv_f16_to_f32(_a), _sv_f16_to_f32(_b)), _sv_f16_to_f32(_c)));\n"; break;
                    case 0x351: os << "        _r = asuint(max(max(asint(_sv_sext16(_a)), asint(_sv_sext16(_b))), asint(_sv_sext16(_c)))) & 0xFFFFu;\n"; break;
                    case 0x352: os << "        _r = max(max(_a & 0xFFFFu, _b & 0xFFFFu), _c & 0xFFFFu);\n"; break;
                    case 0x353: os << "        { float _ha = _sv_f16_to_f32(_a), _hb = _sv_f16_to_f32(_b), _hc = _sv_f16_to_f32(_c); _r = _sv_f32_to_f16(max(min(_ha, _hb), min(max(_ha, _hb), _hc))); }\n"; break;
                    case 0x354: os << "        { int _ia = asint(_sv_sext16(_a)), _ib = asint(_sv_sext16(_b)), _ic = asint(_sv_sext16(_c)); _r = asuint(max(min(_ia, _ib), min(max(_ia, _ib), _ic))) & 0xFFFFu; }\n"; break;
                    case 0x355: os << "        { uint _ua = _a & 0xFFFFu, _ub = _b & 0xFFFFu, _uc = _c & 0xFFFFu; _r = max(min(_ua, _ub), min(max(_ua, _ub), _uc)); }\n"; break;
                    case 0x368: os << "        _r = (asuint(int(round(clamp(asfloat(_a), -1.0, 1.0) * 32767.0))) & 0xFFFFu) | ((asuint(int(round(clamp(asfloat(_b), -1.0, 1.0) * 32767.0))) & 0xFFFFu) << 16u);\n"; break;
                    case 0x369: os << "        _r = (uint(round(clamp(asfloat(_a), 0.0, 1.0) * 65535.0)) & 0xFFFFu) | ((uint(round(clamp(asfloat(_b), 0.0, 1.0) * 65535.0)) & 0xFFFFu) << 16u);\n"; break;
                    case 0x36A: os << "        _r = min(_a, 0xFFFFu) | (min(_b, 0xFFFFu) << 16u);\n"; break;
                    case 0x36B: os << "        _r = (asuint(clamp(asint(_a), -32768, 32767)) & 0xFFFFu) | ((asuint(clamp(asint(_b), -32768, 32767)) & 0xFFFFu) << 16u);\n"; break;
                    case 0x36C: os << "        _r = _sv_f32_to_f16(asfloat(_a)) | (_sv_f32_to_f16(asfloat(_b)) << 16u);\n"; break;
                    case 0x36D: os << "        _r = _a + _b + _c;\n"; break;
                    case 0x36E: os << "        _r = (_a << (_b & 31u)) | _c;\n"; break;
                    case 0x36F: os << "        _r = (_a & _b) | _c;\n"; break;
                    case 0x370: os << "        _r = _a | _b | _c;\n"; break;
                    case 0x371: os << "        _r = (_a ^ _b) + _c;\n"; break;
                    case 0x372: os << "        _r = (_a << (_b & 31u)) + _c;\n"; break;
                    case 0x373: os << "        _r = asuint(_sv_f16_to_f32(_a)*_sv_f16_to_f32(_b) + _sv_f16_to_f32(_a>>16u)*_sv_f16_to_f32(_b>>16u) + asfloat(_c));\n"; break;
                    case 0x374: os << "        _r = (_a + _b) << (_c & 31u);\n"; break;
                    default: os << "        _r = _a;\n"; break;
                }
                os << "        " << dst << " = _r;\n";
            }
        } else if (enc == Rdna2Encoding::VOP3P) {
            const std::string dst = hlsl_dst_lval(inst.dst);
            os << "        uint _a = " << hlsl_op_u32(inst.src[0]) << ";\n";
            os << "        uint _b = " << (inst.src_count >= 2u ? hlsl_op_u32(inst.src[1]) : "0u") << ";\n";
            os << "        uint _c = " << (inst.src_count >= 3u ? hlsl_op_u32(inst.src[2]) : "0u") << ";\n";
            os << "        uint _a0 = " << ((inst.vop3p_op_sel & 1u) ? "(_a >> 16u)" : "(_a & 0xFFFFu)") << ";\n";
            os << "        uint _a1 = " << ((inst.vop3p_op_sel_hi & 1u) ? "(_a >> 16u)" : "(_a & 0xFFFFu)") << ";\n";
            os << "        uint _b0 = " << ((inst.vop3p_op_sel & 2u) ? "(_b >> 16u)" : "(_b & 0xFFFFu)") << ";\n";
            os << "        uint _b1 = " << ((inst.vop3p_op_sel_hi & 2u) ? "(_b >> 16u)" : "(_b & 0xFFFFu)") << ";\n";
            os << "        uint _c0 = " << ((inst.vop3p_op_sel & 4u) ? "(_c >> 16u)" : "(_c & 0xFFFFu)") << ";\n";
            os << "        uint _c1 = " << ((inst.vop3p_op_sel_hi & 4u) ? "(_c >> 16u)" : "(_c & 0xFFFFu)") << ";\n";
            auto f16_hlsl = [&](const char* var, uint8_t nmask, uint8_t bit) -> std::string {
                std::string s = std::string("_sv_f16_to_f32(") + var + ")";
                return ((nmask >> bit) & 1u) ? ("(-" + s + ")") : s;
            };
            os << "        uint _r = 0u;\n";
            switch (raw) {
                case 0x00: os << "        _r = (asuint(asint(_sv_sext16(_a0))*asint(_sv_sext16(_b0))+asint(_sv_sext16(_c0))) & 0xFFFFu) | ((asuint(asint(_sv_sext16(_a1))*asint(_sv_sext16(_b1))+asint(_sv_sext16(_c1))) & 0xFFFFu) << 16u);\n"; break;
                case 0x01: os << "        _r = ((_a0 * _b0) & 0xFFFFu) | (((_a1 * _b1) & 0xFFFFu) << 16u);\n"; break;
                case 0x02: os << "        _r = ((_a0 + _b0) & 0xFFFFu) | (((_a1 + _b1) & 0xFFFFu) << 16u);\n"; break;
                case 0x03: os << "        _r = ((_a0 - _b0) & 0xFFFFu) | (((_a1 - _b1) & 0xFFFFu) << 16u);\n"; break;
                case 0x04: os << "        _r = ((_b0 << (_a0 & 15u)) & 0xFFFFu) | (((_b1 << (_a1 & 15u)) & 0xFFFFu) << 16u);\n"; break;
                case 0x05: os << "        _r = (_b0 >> (_a0 & 15u)) | ((_b1 >> (_a1 & 15u)) << 16u);\n"; break;
                case 0x06: os << "        _r = (asuint(asint(_sv_sext16(_b0)) >> (_a0 & 15u)) & 0xFFFFu) | ((asuint(asint(_sv_sext16(_b1)) >> (_a1 & 15u)) & 0xFFFFu) << 16u);\n"; break;
                case 0x07: os << "        _r = (asuint(max(asint(_sv_sext16(_a0)), asint(_sv_sext16(_b0)))) & 0xFFFFu) | ((asuint(max(asint(_sv_sext16(_a1)), asint(_sv_sext16(_b1)))) & 0xFFFFu) << 16u);\n"; break;
                case 0x08: os << "        _r = (asuint(min(asint(_sv_sext16(_a0)), asint(_sv_sext16(_b0)))) & 0xFFFFu) | ((asuint(min(asint(_sv_sext16(_a1)), asint(_sv_sext16(_b1)))) & 0xFFFFu) << 16u);\n"; break;
                case 0x09: os << "        _r = ((_a0 * _b0 + _c0) & 0xFFFFu) | (((_a1 * _b1 + _c1) & 0xFFFFu) << 16u);\n"; break;
                case 0x0A: os << "        _r = ((_a0 + _b0) & 0xFFFFu) | (((_a1 + _b1) & 0xFFFFu) << 16u);\n"; break;
                case 0x0B: os << "        _r = ((_a0 - _b0) & 0xFFFFu) | (((_a1 - _b1) & 0xFFFFu) << 16u);\n"; break;
                case 0x0C: os << "        _r = max(_a0, _b0) | (max(_a1, _b1) << 16u);\n"; break;
                case 0x0D: os << "        _r = min(_a0, _b0) | (min(_a1, _b1) << 16u);\n"; break;
                case 0x0E: os << "        _r = _sv_f32_to_f16(_sv_fma_f32(" << f16_hlsl("_a0", inst.vop3p_neg_lo, 0) << ", " << f16_hlsl("_b0", inst.vop3p_neg_lo, 1) << ", " << f16_hlsl("_c0", inst.vop3p_neg_lo, 2) << ")) | (_sv_f32_to_f16(_sv_fma_f32(" << f16_hlsl("_a1", inst.vop3p_neg_hi, 0) << ", " << f16_hlsl("_b1", inst.vop3p_neg_hi, 1) << ", " << f16_hlsl("_c1", inst.vop3p_neg_hi, 2) << ")) << 16u);\n"; break;
                case 0x0F: os << "        _r = _sv_f32_to_f16(" << f16_hlsl("_a0", inst.vop3p_neg_lo, 0) << " + " << f16_hlsl("_b0", inst.vop3p_neg_lo, 1) << ") | (_sv_f32_to_f16(" << f16_hlsl("_a1", inst.vop3p_neg_hi, 0) << " + " << f16_hlsl("_b1", inst.vop3p_neg_hi, 1) << ") << 16u);\n"; break;
                case 0x10: os << "        _r = _sv_f32_to_f16(" << f16_hlsl("_a0", inst.vop3p_neg_lo, 0) << " * " << f16_hlsl("_b0", inst.vop3p_neg_lo, 1) << ") | (_sv_f32_to_f16(" << f16_hlsl("_a1", inst.vop3p_neg_hi, 0) << " * " << f16_hlsl("_b1", inst.vop3p_neg_hi, 1) << ") << 16u);\n"; break;
                case 0x11: os << "        _r = _sv_f32_to_f16(min(" << f16_hlsl("_a0", inst.vop3p_neg_lo, 0) << ", " << f16_hlsl("_b0", inst.vop3p_neg_lo, 1) << ")) | (_sv_f32_to_f16(min(" << f16_hlsl("_a1", inst.vop3p_neg_hi, 0) << ", " << f16_hlsl("_b1", inst.vop3p_neg_hi, 1) << ")) << 16u);\n"; break;
                case 0x12: os << "        _r = _sv_f32_to_f16(max(" << f16_hlsl("_a0", inst.vop3p_neg_lo, 0) << ", " << f16_hlsl("_b0", inst.vop3p_neg_lo, 1) << ")) | (_sv_f32_to_f16(max(" << f16_hlsl("_a1", inst.vop3p_neg_hi, 0) << ", " << f16_hlsl("_b1", inst.vop3p_neg_hi, 1) << ")) << 16u);\n"; break;
                case 0x13: os << "        _r = _sv_dot2_i16(_a, _b, _c);\n"; break;
                case 0x14: os << "        _r = _sv_dot2_u16(_a, _b, _c);\n"; break;
                case 0x15: os << "        _r = _sv_dot4_i8(_a, _b, _c);\n"; break;
                case 0x16: os << "        _r = _sv_dot4_u8(_a, _b, _c);\n"; break;
                case 0x17: os << "        _r = _sv_dot8_i4(_a, _b, _c);\n"; break;
                case 0x18: os << "        _r = _sv_dot8_u4(_a, _b, _c);\n"; break;
                case 0x20: case 0x21: case 0x22: {
                    auto mix_hlsl = [&](const char* var, uint8_t bit) -> std::string {
                        const bool is_f16 = ((inst.vop3p_op_sel_hi >> bit) & 1u) != 0u;
                        const bool hi = ((inst.vop3p_op_sel >> bit) & 1u) != 0u;
                        std::string f = is_f16 ? (std::string("_sv_f16_to_f32(") + var + (hi ? " >> 16u)" : ")"))
                                               : (std::string("asfloat(") + var + ")");
                        if ((inst.vop3p_neg_lo >> bit) & 1u) f = "abs(" + f + ")";
                        if ((inst.vop3p_neg_hi >> bit) & 1u) f = "(-" + f + ")";
                        return f;
                    };
                    os << "        float _mf = _sv_fma_f32(" << mix_hlsl("_a", 0u) << ", " << mix_hlsl("_b", 1u) << ", " << mix_hlsl("_c", 2u) << ");\n";
                    if (raw == 0x20u) os << "        _r = asuint(_mf);\n";
                    else if (raw == 0x21u) os << "        _r = (" << dst << " & 0xFFFF0000u) | _sv_f32_to_f16(_mf);\n";
                    else os << "        _r = (" << dst << " & 0x0000FFFFu) | (_sv_f32_to_f16(_mf) << 16u);\n";
                    break;
                }
                default: os << "        _r = _a;\n"; break;
            }
            os << "        " << dst << " = _r;\n";
        } else if (enc == Rdna2Encoding::VINTRP) {
            os << "        " << hlsl_dst_lval(inst.dst) << " = " << hlsl_op_u32(inst.src[0]) << ";\n";
        }
        os << "    }\n";
    }

    if (container.stage == ShaderStage::Vertex) {
        os << "    _out.pos = float4(asfloat(v[0]), asfloat(v[1]), asfloat(v[2]), 1.0);\n";
        os << "    return _out;\n";
    } else if (container.stage == ShaderStage::Pixel) {
        os << "    _mrt0 = float4(asfloat(v[0]), asfloat(v[1]), asfloat(v[2]), 1.0);\n";
        os << "    return _mrt0;\n";
    } else if (container.stage == ShaderStage::Mesh) {
        os << "    if (_gtid < 3u) _verts[_gtid].pos = float4(asfloat(v[0]), asfloat(v[1]), 0.0, 1.0);\n";
        os << "    if (_gtid == 0u) _prims[0] = uint3(0u, 1u, 2u);\n";
    } else if (container.stage == ShaderStage::Amplification) {
        os << "    _payload.d[0] = v[0];\n";
        os << "    DispatchMesh(1u, 1u, 1u, _payload);\n";
    }
    os << "}\n";
    out_hlsl = os.str();
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Direct Emitter (Khronos SPIR-V 1.3 + GLSL.std.450)
// ---------------------------------------------------------------------------
class SvSpirvBuilder {
public:
    static constexpr uint32_t kMagic = 0x07230203u;
    static constexpr uint32_t kVer13 = 0x00010300u;
    static constexpr uint32_t kGen = 0x4F533200u;

    static void Emit(std::vector<uint32_t>& buf, uint16_t op, const std::vector<uint32_t>& ops) {
        buf.push_back((static_cast<uint32_t>(ops.size() + 1u) << 16u) | static_cast<uint32_t>(op));
        buf.insert(buf.end(), ops.begin(), ops.end());
    }

    static void AppendStr(std::vector<uint32_t>& ops, const char* s) {
        const size_t len = std::strlen(s) + 1u;
        const size_t words = (len + 3u) / 4u;
        for (size_t w = 0u; w < words; ++w) {
            uint32_t word = 0u;
            for (size_t b = 0u; b < 4u; ++b) {
                const size_t idx = w * 4u + b;
                if (idx < len) word |= static_cast<uint32_t>(static_cast<uint8_t>(s[idx])) << (b * 8u);
            }
            ops.push_back(word);
        }
    }

    uint32_t AllocId() noexcept { return next_id_++; }

    uint32_t ConstU32(uint32_t v) {
        auto it = const_u32_.find(v);
        if (it != const_u32_.end()) return it->second;
        const uint32_t id = AllocId();
        Emit(sec_types_, 43u /*OpConstant*/, {t_u32_, id, v});
        const_u32_[v] = id;
        return id;
    }

    uint32_t ConstI32(int32_t v) {
        const uint32_t bits = static_cast<uint32_t>(v);
        auto it = const_i32_.find(bits);
        if (it != const_i32_.end()) return it->second;
        const uint32_t id = AllocId();
        Emit(sec_types_, 43u /*OpConstant*/, {t_i32_, id, bits});
        const_i32_[bits] = id;
        return id;
    }

    uint32_t ConstF32(float f) {
        const uint32_t bits = FloatToBits(f);
        auto it = const_f32_.find(bits);
        if (it != const_f32_.end()) return it->second;
        const uint32_t id = AllocId();
        Emit(sec_types_, 43u /*OpConstant*/, {t_f32_, id, bits});
        const_f32_[bits] = id;
        return id;
    }

    uint32_t BinU32(uint16_t op, uint32_t a, uint32_t b) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, op, {t_u32_, r, a, b});
        return r;
    }

    uint32_t BinI32(uint16_t op, uint32_t a, uint32_t b) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, op, {t_i32_, r, a, b});
        return r;
    }

    uint32_t BinF32(uint16_t op, uint32_t a, uint32_t b) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, op, {t_f32_, r, a, b});
        return r;
    }

    uint32_t BinBool(uint16_t op, uint32_t a, uint32_t b) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, op, {t_bool_, r, a, b});
        return r;
    }

    uint32_t Bitcast(uint32_t dst_t, uint32_t v) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, 124u /*OpBitcast*/, {dst_t, r, v});
        return r;
    }

    uint32_t SelU32(uint32_t cond, uint32_t t_val, uint32_t f_val) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, 169u /*OpSelect*/, {t_u32_, r, cond, t_val, f_val});
        return r;
    }

    uint32_t SelF32(uint32_t cond, uint32_t t_val, uint32_t f_val) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, 169u /*OpSelect*/, {t_f32_, r, cond, t_val, f_val});
        return r;
    }

    uint32_t EmitTrigTurnsF32(uint32_t fa, bool is_cos) {
        const uint32_t is_nan = AllocId(); Emit(sec_fn_, 156u /*OpIsNan*/, {t_bool_, is_nan, fa});
        const uint32_t is_inf = AllocId(); Emit(sec_fn_, 157u /*OpIsInf*/, {t_bool_, is_inf, fa});
        const uint32_t bad = BinBool(166u /*LogicalOr*/, is_nan, is_inf);
        const uint32_t abs_x = Glsl1(t_f32_, 4u /*FAbs*/, fa);
        const uint32_t oob = BinBool(190u /*FOrdGe*/, abs_x, ConstF32(256.0f));
        const uint32_t skip = BinBool(166u, bad, oob);
        const uint32_t safe = SelF32(skip, ConstF32(0.0f), fa);
        const uint32_t t = BinF32(131u /*FSub*/, safe, Glsl1(t_f32_, 8u /*Floor*/, safe));
        const uint32_t rad = BinF32(133u /*FMul*/, t, ConstF32(kTwoPiF32));
        uint32_t r = Glsl1(t_f32_, is_cos ? 14u /*Cos*/ : 13u /*Sin*/, rad);
        const uint32_t eq0 = BinBool(180u /*FOrdEq*/, t, ConstF32(0.0f));
        const uint32_t eq25 = BinBool(180u, t, ConstF32(0.25f));
        const uint32_t eq50 = BinBool(180u, t, ConstF32(0.5f));
        const uint32_t eq75 = BinBool(180u, t, ConstF32(0.75f));
        if (is_cos) {
            r = SelF32(eq50, ConstF32(-1.0f), r);
            r = SelF32(BinBool(166u, eq25, eq75), ConstF32(0.0f), r);
            r = SelF32(eq0, ConstF32(1.0f), r);
        } else {
            r = SelF32(eq75, ConstF32(-1.0f), r);
            r = SelF32(eq25, ConstF32(1.0f), r);
            r = SelF32(BinBool(166u, eq0, eq50), ConstF32(0.0f), r);
        }
        r = SelF32(oob, ConstF32(is_cos ? 1.0f : 0.0f), r);
        return SelF32(bad, Bitcast(t_f32_, ConstU32(0x7FC00000u)), r);
    }

    uint32_t EmitSBfeU32(uint32_t src0, uint32_t src1) {
        const uint32_t off = BinU32(199u, src1, ConstU32(31u));
        const uint32_t w = BinU32(199u, BinU32(194u, src1, ConstU32(16u)), ConstU32(0x7Fu));
        const uint32_t w_zero = BinBool(170u /*IEqual*/, w, ConstU32(0u));
        const uint32_t sum = BinU32(128u /*IAdd*/, off, w);
        const uint32_t full = BinBool(174u /*UGe*/, sum, ConstU32(32u));
        const uint32_t shr = BinU32(194u /*Shr*/, src0, off);
        const uint32_t w_safe = BinU32(199u, w, ConstU32(31u));
        const uint32_t mask = BinU32(130u /*ISub*/, BinU32(196u /*Shl*/, ConstU32(1u), w_safe), ConstU32(1u));
        const uint32_t masked = BinU32(199u /*And*/, shr, mask);
        return SelU32(w_zero, ConstU32(0u), SelU32(full, shr, masked));
    }

    uint32_t EmitSBfeI32(uint32_t src0, uint32_t src1) {
        const uint32_t off = BinU32(199u, src1, ConstU32(31u));
        const uint32_t w = BinU32(199u, BinU32(194u, src1, ConstU32(16u)), ConstU32(0x7Fu));
        const uint32_t w_zero = BinBool(170u, w, ConstU32(0u));
        const uint32_t sum = BinU32(128u, off, w);
        const uint32_t full = BinBool(174u, sum, ConstU32(32u));
        const uint32_t full_r = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, src0), off));
        const uint32_t lsh_amt = BinU32(199u, BinU32(130u, ConstU32(32u), sum), ConstU32(31u));
        const uint32_t rsh_amt = BinU32(199u, BinU32(130u, ConstU32(32u), w), ConstU32(31u));
        const uint32_t lsh = BinU32(196u, src0, lsh_amt);
        const uint32_t mid_r = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, lsh), rsh_amt));
        return SelU32(w_zero, ConstU32(0u), SelU32(full, full_r, mid_r));
    }

    void EmitSBfe64(uint32_t lo, uint32_t hi, uint32_t src1, bool is_signed, uint32_t& out_lo, uint32_t& out_hi) {
        const uint32_t o = BinU32(199u, src1, ConstU32(63u));
        const uint32_t w = BinU32(199u, BinU32(194u, src1, ConstU32(16u)), ConstU32(0x7Fu));
        const uint32_t w0 = BinBool(170u, w, ConstU32(0u));
        const uint32_t o0 = BinBool(170u, o, ConstU32(0u));
        const uint32_t olt32 = BinBool(176u /*ULt*/, o, ConstU32(32u));
        const uint32_t o_lo = BinU32(199u, o, ConstU32(31u));
        const uint32_t inv_o = BinU32(199u, BinU32(130u, ConstU32(32u), o_lo), ConstU32(31u));
        const uint32_t sfill = is_signed ? Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, hi), ConstU32(31u))) : ConstU32(0u);
        const uint32_t hi_sh = is_signed ? Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, hi), o_lo)) : BinU32(194u, hi, o_lo);
        const uint32_t rlo_lt = BinU32(197u, BinU32(194u, lo, o_lo), BinU32(196u, hi, inv_o));
        const uint32_t rlo = SelU32(o0, lo, SelU32(olt32, rlo_lt, hi_sh));
        const uint32_t rhi = SelU32(olt32, hi_sh, sfill);
        const uint32_t full = BinBool(174u, BinU32(128u, o, w), ConstU32(64u));
        const uint32_t wlt32 = BinBool(176u, w, ConstU32(32u));
        const uint32_t weq32 = BinBool(170u, w, ConstU32(32u));
        const uint32_t w_m31 = BinU32(199u, w, ConstU32(31u));
        const uint32_t sw_m31 = BinU32(199u, BinU32(130u, w, ConstU32(32u)), ConstU32(31u));
        uint32_t mlo = rlo, mhi = rhi;
        if (!is_signed) {
            const uint32_t mask_lo = BinU32(130u, BinU32(196u, ConstU32(1u), w_m31), ConstU32(1u));
            const uint32_t mask_hi = BinU32(130u, BinU32(196u, ConstU32(1u), sw_m31), ConstU32(1u));
            mlo = SelU32(wlt32, BinU32(199u, rlo, mask_lo), rlo);
            mhi = SelU32(BinBool(166u, wlt32, weq32), ConstU32(0u), BinU32(199u, rhi, mask_hi));
        } else {
            const uint32_t sh_lo = BinU32(199u, BinU32(130u, ConstU32(32u), w_m31), ConstU32(31u));
            const uint32_t slo = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, BinU32(196u, rlo, sh_lo)), sh_lo));
            const uint32_t slo_s = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, slo), ConstU32(31u)));
            const uint32_t rlo_s = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, rlo), ConstU32(31u)));
            const uint32_t sh_hi = BinU32(199u, BinU32(130u, ConstU32(32u), sw_m31), ConstU32(31u));
            const uint32_t shi = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, BinU32(196u, rhi, sh_hi)), sh_hi));
            mlo = SelU32(wlt32, slo, rlo);
            mhi = SelU32(wlt32, slo_s, SelU32(weq32, rlo_s, shi));
        }
        out_lo = SelU32(w0, ConstU32(0u), SelU32(full, rlo, mlo));
        out_hi = SelU32(w0, ConstU32(0u), SelU32(full, rhi, mhi));
    }

    uint32_t EmitWqm32(uint32_t m) {
        uint32_t r = ConstU32(0u);
        for (uint32_t q = 0u; q < 8u; ++q) {
            const uint32_t qm = ConstU32(0xFu << (q * 4u));
            const uint32_t nz = BinBool(171u, BinU32(199u, m, qm), ConstU32(0u));
            r = BinU32(197u, r, SelU32(nz, qm, ConstU32(0u)));
        }
        return r;
    }

    uint32_t EmitQuadMask32(uint32_t m) {
        uint32_t r = ConstU32(0u);
        for (uint32_t q = 0u; q < 8u; ++q) {
            const uint32_t nz = BinBool(171u, BinU32(199u, m, ConstU32(0xFu << (q * 4u))), ConstU32(0u));
            r = BinU32(197u, r, SelU32(nz, ConstU32(1u << q), ConstU32(0u)));
        }
        return r;
    }

    uint32_t EmitFClassBits(uint32_t b, uint32_t mask) {
        const uint32_t neg = BinBool(171u, BinU32(194u, b, ConstU32(31u)), ConstU32(0u));
        const uint32_t ex = BinU32(199u, BinU32(194u, b, ConstU32(23u)), ConstU32(0xFFu));
        const uint32_t fr = BinU32(199u, b, ConstU32(0x7FFFFFu));
        const uint32_t fr_z = BinBool(170u, fr, ConstU32(0u));
        const uint32_t is_q = BinBool(171u, BinU32(199u, fr, ConstU32(0x400000u)), ConstU32(0u));
        const uint32_t ex_ff = BinBool(170u, ex, ConstU32(0xFFu));
        const uint32_t ex_z = BinBool(170u, ex, ConstU32(0u));
        const uint32_t bit_ff = SelU32(fr_z, SelU32(neg, ConstU32(2u), ConstU32(9u)), SelU32(is_q, ConstU32(1u), ConstU32(0u)));
        const uint32_t bit_z = SelU32(fr_z, SelU32(neg, ConstU32(5u), ConstU32(6u)), SelU32(neg, ConstU32(4u), ConstU32(7u)));
        const uint32_t bit_n = SelU32(neg, ConstU32(3u), ConstU32(8u));
        const uint32_t bit_i = SelU32(ex_ff, bit_ff, SelU32(ex_z, bit_z, bit_n));
        return BinBool(171u, BinU32(199u, BinU32(194u, mask, bit_i), ConstU32(1u)), ConstU32(0u));
    }

    uint32_t EmitFClassF16Bits(uint32_t h, uint32_t mask) {
        const uint32_t hb = BinU32(199u, h, ConstU32(0xFFFFu));
        const uint32_t neg = BinBool(171u, BinU32(194u, hb, ConstU32(15u)), ConstU32(0u));
        const uint32_t ex = BinU32(199u, BinU32(194u, hb, ConstU32(10u)), ConstU32(0x1Fu));
        const uint32_t fr = BinU32(199u, hb, ConstU32(0x3FFu));
        const uint32_t fr_z = BinBool(170u, fr, ConstU32(0u));
        const uint32_t is_q = BinBool(171u, BinU32(199u, fr, ConstU32(0x200u)), ConstU32(0u));
        const uint32_t ex_ff = BinBool(170u, ex, ConstU32(0x1Fu));
        const uint32_t ex_z = BinBool(170u, ex, ConstU32(0u));
        const uint32_t bit_ff = SelU32(fr_z, SelU32(neg, ConstU32(2u), ConstU32(9u)), SelU32(is_q, ConstU32(1u), ConstU32(0u)));
        const uint32_t bit_z = SelU32(fr_z, SelU32(neg, ConstU32(5u), ConstU32(6u)), SelU32(neg, ConstU32(4u), ConstU32(7u)));
        const uint32_t bit_n = SelU32(neg, ConstU32(3u), ConstU32(8u));
        const uint32_t bit_i = SelU32(ex_ff, bit_ff, SelU32(ex_z, bit_z, bit_n));
        return BinBool(171u, BinU32(199u, BinU32(194u, mask, bit_i), ConstU32(1u)), ConstU32(0u));
    }

    uint32_t EmitCmpF32Bits(uint32_t a, uint32_t b, uint32_t cc) {
        const uint32_t am = BinU32(199u, a, ConstU32(0x7FFFFFFFu));
        const uint32_t bm = BinU32(199u, b, ConstU32(0x7FFFFFFFu));
        const uint32_t nan = BinBool(166u, BinBool(172u, am, ConstU32(0x7F800000u)), BinBool(172u, bm, ConstU32(0x7F800000u)));
        const bool unord = (cc >= 8u);
        const uint32_t op = cc & 7u;
        if (op == 0u) return unord ? nan : c_false_;
        if (op == 7u) {
            if (unord) return c_true_;
            const uint32_t r = AllocId();
            Emit(sec_fn_, 168u /*LogicalNot*/, {t_bool_, r, nan});
            return r;
        }
        const uint32_t na = AllocId(); Emit(sec_fn_, 200u, {t_u32_, na, a});
        const uint32_t nb = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nb, b});
        const uint32_t a_neg = BinBool(171u, BinU32(199u, a, ConstU32(0x80000000u)), ConstU32(0u));
        const uint32_t b_neg = BinBool(171u, BinU32(199u, b, ConstU32(0x80000000u)), ConstU32(0u));
        const uint32_t ao = SelU32(BinBool(170u, am, ConstU32(0u)), ConstU32(0x80000000u), SelU32(a_neg, na, BinU32(197u, a, ConstU32(0x80000000u))));
        const uint32_t bo = SelU32(BinBool(170u, bm, ConstU32(0u)), ConstU32(0x80000000u), SelU32(b_neg, nb, BinU32(197u, b, ConstU32(0x80000000u))));
        uint32_t cmp = c_false_;
        switch (op) {
            case 1u: cmp = BinBool(176u /*ULt*/, ao, bo); break;
            case 2u: cmp = BinBool(170u /*IEqual*/, ao, bo); break;
            case 3u: cmp = BinBool(178u /*ULe*/, ao, bo); break;
            case 4u: cmp = BinBool(172u /*UGt*/, ao, bo); break;
            case 5u: cmp = BinBool(171u /*INotEqual*/, ao, bo); break;
            default: cmp = BinBool(174u /*UGe*/, ao, bo); break;
        }
        if (unord) return BinBool(166u /*LOr*/, nan, cmp);
        const uint32_t not_nan = AllocId();
        Emit(sec_fn_, 168u, {t_bool_, not_nan, nan});
        return BinBool(167u /*LAnd*/, not_nan, cmp);
    }

    uint32_t EmitCmpF16Bits(uint32_t a, uint32_t b, uint32_t cc) {
        const uint32_t ah = BinU32(199u, a, ConstU32(0xFFFFu));
        const uint32_t bh = BinU32(199u, b, ConstU32(0xFFFFu));
        const uint32_t am = BinU32(199u, ah, ConstU32(0x7FFFu));
        const uint32_t bm = BinU32(199u, bh, ConstU32(0x7FFFu));
        const uint32_t nan = BinBool(166u, BinBool(172u, am, ConstU32(0x7C00u)), BinBool(172u, bm, ConstU32(0x7C00u)));
        const bool unord = (cc >= 8u);
        const uint32_t op = cc & 7u;
        if (op == 0u) return unord ? nan : c_false_;
        if (op == 7u) {
            if (unord) return c_true_;
            const uint32_t r = AllocId();
            Emit(sec_fn_, 168u, {t_bool_, r, nan});
            return r;
        }
        const uint32_t na = BinU32(199u, BinU32(198u, ah, ConstU32(0xFFFFu)), ConstU32(0xFFFFu));
        const uint32_t nb = BinU32(199u, BinU32(198u, bh, ConstU32(0xFFFFu)), ConstU32(0xFFFFu));
        const uint32_t a_neg = BinBool(171u, BinU32(199u, ah, ConstU32(0x8000u)), ConstU32(0u));
        const uint32_t b_neg = BinBool(171u, BinU32(199u, bh, ConstU32(0x8000u)), ConstU32(0u));
        const uint32_t ao = SelU32(BinBool(170u, am, ConstU32(0u)), ConstU32(0x8000u), SelU32(a_neg, na, BinU32(197u, ah, ConstU32(0x8000u))));
        const uint32_t bo = SelU32(BinBool(170u, bm, ConstU32(0u)), ConstU32(0x8000u), SelU32(b_neg, nb, BinU32(197u, bh, ConstU32(0x8000u))));
        uint32_t cmp = c_false_;
        switch (op) {
            case 1u: cmp = BinBool(176u, ao, bo); break;
            case 2u: cmp = BinBool(170u, ao, bo); break;
            case 3u: cmp = BinBool(178u, ao, bo); break;
            case 4u: cmp = BinBool(172u, ao, bo); break;
            case 5u: cmp = BinBool(171u, ao, bo); break;
            default: cmp = BinBool(174u, ao, bo); break;
        }
        if (unord) return BinBool(166u, nan, cmp);
        const uint32_t not_nan = AllocId();
        Emit(sec_fn_, 168u, {t_bool_, not_nan, nan});
        return BinBool(167u, not_nan, cmp);
    }

    uint32_t EmitExactRcpF32Bits(uint32_t b) {
        const uint32_t mag = BinU32(199u, b, ConstU32(0x7FFFFFFFu));
        const uint32_t sgn = BinU32(199u, b, ConstU32(0x80000000u));
        const uint32_t is_nan = BinBool(172u, mag, ConstU32(0x7F800000u));
        const uint32_t is_z = BinBool(170u, mag, ConstU32(0u));
        const uint32_t is_inf = BinBool(170u, mag, ConstU32(0x7F800000u));
        const uint32_t normal = Bitcast(t_u32_, BinF32(136u /*FDiv*/, ConstF32(1.0f), Bitcast(t_f32_, b)));
        return SelU32(is_nan, ConstU32(0x7FC00000u),
               SelU32(is_z, BinU32(197u, sgn, ConstU32(0x7F800000u)),
               SelU32(is_inf, sgn, normal)));
    }

    uint32_t EmitExactRsqF32Bits(uint32_t b) {
        const uint32_t mag = BinU32(199u, b, ConstU32(0x7FFFFFFFu));
        const uint32_t sgn = BinU32(199u, b, ConstU32(0x80000000u));
        const uint32_t is_nan = BinBool(172u, mag, ConstU32(0x7F800000u));
        const uint32_t is_z = BinBool(170u, mag, ConstU32(0u));
        const uint32_t is_neg = BinBool(171u, sgn, ConstU32(0u));
        const uint32_t is_inf = BinBool(170u, mag, ConstU32(0x7F800000u));
        const uint32_t normal = Bitcast(t_u32_, Glsl1(t_f32_, 32u /*InverseSqrt*/, Bitcast(t_f32_, b)));
        return SelU32(is_nan, ConstU32(0x7FC00000u),
               SelU32(is_z, BinU32(197u, sgn, ConstU32(0x7F800000u)),
               SelU32(is_neg, ConstU32(0x7FC00000u),
               SelU32(is_inf, ConstU32(0u), normal))));
    }

    uint32_t EmitExactSqrtF32Bits(uint32_t b) {
        const uint32_t mag = BinU32(199u, b, ConstU32(0x7FFFFFFFu));
        const uint32_t sgn = BinU32(199u, b, ConstU32(0x80000000u));
        const uint32_t is_nan = BinBool(172u, mag, ConstU32(0x7F800000u));
        const uint32_t is_z = BinBool(170u, mag, ConstU32(0u));
        const uint32_t is_neg = BinBool(171u, sgn, ConstU32(0u));
        const uint32_t is_inf = BinBool(170u, mag, ConstU32(0x7F800000u));
        const uint32_t normal = Bitcast(t_u32_, Glsl1(t_f32_, 31u /*Sqrt*/, Bitcast(t_f32_, b)));
        return SelU32(is_nan, ConstU32(0x7FC00000u),
               SelU32(is_z, b,
               SelU32(is_neg, ConstU32(0x7FC00000u),
               SelU32(is_inf, ConstU32(0x7F800000u), normal))));
    }

    uint32_t Glsl1(uint32_t res_t, uint32_t ext_op, uint32_t a) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, 12u /*OpExtInst*/, {res_t, r, ext_glsl_, ext_op, a});
        return r;
    }

    uint32_t Glsl2(uint32_t res_t, uint32_t ext_op, uint32_t a, uint32_t b) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, 12u /*OpExtInst*/, {res_t, r, ext_glsl_, ext_op, a, b});
        return r;
    }

    uint32_t Glsl3(uint32_t res_t, uint32_t ext_op, uint32_t a, uint32_t b, uint32_t c) {
        const uint32_t r = AllocId();
        Emit(sec_fn_, 12u /*OpExtInst*/, {res_t, r, ext_glsl_, ext_op, a, b, c});
        return r;
    }

    uint32_t UnpackF16Lo(uint32_t u) {
        const uint32_t v2 = AllocId();
        Emit(sec_fn_, 12u, {t_v2f32_, v2, ext_glsl_, 62u /*UnpackHalf2x16*/, u});
        const uint32_t f = AllocId();
        Emit(sec_fn_, 81u /*OpCompositeExtract*/, {t_f32_, f, v2, 0u});
        return f;
    }

    uint32_t UnpackF16Hi(uint32_t u) {
        const uint32_t v2 = AllocId();
        Emit(sec_fn_, 12u, {t_v2f32_, v2, ext_glsl_, 62u /*UnpackHalf2x16*/, u});
        const uint32_t f = AllocId();
        Emit(sec_fn_, 81u /*OpCompositeExtract*/, {t_f32_, f, v2, 1u});
        return f;
    }

    uint32_t PackF16Pair(uint32_t f_lo, uint32_t f_hi) {
        const uint32_t v2 = AllocId();
        Emit(sec_fn_, 80u /*OpCompositeConstruct*/, {t_v2f32_, v2, f_lo, f_hi});
        return Glsl1(t_u32_, 58u /*PackHalf2x16*/, v2);
    }

    uint32_t PackF16Lo(uint32_t f) {
        return BinU32(199u /*And*/, PackF16Pair(f, ConstF32(0.0f)), ConstU32(0xFFFFu));
    }

    uint32_t SextN(uint32_t u, uint32_t bits) {
        const uint32_t sh = ConstU32(32u - bits);
        const uint32_t ls = BinU32(196u /*Shl*/, u, sh);
        const uint32_t rs = BinI32(195u /*Ashr*/, Bitcast(t_i32_, ls), sh);
        return Bitcast(t_u32_, rs);
    }

    uint32_t EmitBfeU32(uint32_t src0, uint32_t off_raw, uint32_t w_raw) {
        const uint32_t off = BinU32(199u, off_raw, ConstU32(31u));
        const uint32_t w = BinU32(199u, w_raw, ConstU32(31u));
        const uint32_t w_zero = BinBool(170u /*IEqual*/, w, ConstU32(0u));
        const uint32_t sum = BinU32(128u /*IAdd*/, off, w);
        const uint32_t full = BinBool(174u /*UGe*/, sum, ConstU32(32u));
        const uint32_t shr = BinU32(194u /*Shr*/, src0, off);
        const uint32_t p2 = BinU32(196u /*Shl*/, ConstU32(1u), w);
        const uint32_t mask = BinU32(130u /*ISub*/, p2, ConstU32(1u));
        const uint32_t masked = BinU32(199u /*And*/, shr, mask);
        return SelU32(w_zero, ConstU32(0u), SelU32(full, shr, masked));
    }

    uint32_t EmitBfeI32(uint32_t src0, uint32_t off_raw, uint32_t w_raw) {
        const uint32_t off = BinU32(199u, off_raw, ConstU32(31u));
        const uint32_t w = BinU32(199u, w_raw, ConstU32(31u));
        const uint32_t w_zero = BinBool(170u, w, ConstU32(0u));
        const uint32_t sum = BinU32(128u, off, w);
        const uint32_t full = BinBool(174u, sum, ConstU32(32u));
        const uint32_t full_r = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, src0), off));
        const uint32_t lsh_amt = BinU32(199u, BinU32(130u, ConstU32(32u), sum), ConstU32(31u));
        const uint32_t rsh_amt = BinU32(199u, BinU32(130u, ConstU32(32u), w), ConstU32(31u));
        const uint32_t lsh = BinU32(196u, src0, lsh_amt);
        const uint32_t mid_r = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, lsh), rsh_amt));
        return SelU32(w_zero, ConstU32(0u), SelU32(full, full_r, mid_r));
    }

    uint32_t EmitBfmB32(uint32_t w_raw, uint32_t off_raw) {
        const uint32_t w = BinU32(199u, w_raw, ConstU32(31u));
        const uint32_t off = BinU32(199u, off_raw, ConstU32(31u));
        const uint32_t mask = BinU32(130u, BinU32(196u, ConstU32(1u), w), ConstU32(1u));
        return BinU32(196u, mask, off);
    }

    uint32_t EmitMulHiU32(uint32_t a, uint32_t b) {
        const uint32_t prod = AllocId();
        Emit(sec_fn_, 151u /*OpUMulExtended*/, {t_u32_pair_, prod, a, b});
        const uint32_t hi = AllocId();
        Emit(sec_fn_, 81u /*OpCompositeExtract*/, {t_u32_, hi, prod, 1u});
        return hi;
    }

    uint32_t EmitMulHiI32(uint32_t a, uint32_t b) {
        const uint32_t prod = AllocId();
        Emit(sec_fn_, 152u /*OpSMulExtended*/, {t_i32_pair_, prod, Bitcast(t_i32_, a), Bitcast(t_i32_, b)});
        const uint32_t hi_i = AllocId();
        Emit(sec_fn_, 81u /*OpCompositeExtract*/, {t_i32_, hi_i, prod, 1u});
        return Bitcast(t_u32_, hi_i);
    }

    uint32_t EmitSdwaSrc(uint32_t raw, uint8_t sel, bool sext, bool isf, bool neg_v, bool abs_v) {
        uint32_t v = raw;
        switch (sel & 7u) {
            case 0u: v = sext ? SextN(raw, 8u) : BinU32(199u, raw, ConstU32(0xFFu)); break;
            case 1u: { uint32_t s = BinU32(194u, raw, ConstU32(8u)); v = sext ? SextN(s, 8u) : BinU32(199u, s, ConstU32(0xFFu)); break; }
            case 2u: { uint32_t s = BinU32(194u, raw, ConstU32(16u)); v = sext ? SextN(s, 8u) : BinU32(199u, s, ConstU32(0xFFu)); break; }
            case 3u: { uint32_t s = BinU32(194u, raw, ConstU32(24u)); v = sext ? SextN(s, 8u) : BinU32(199u, s, ConstU32(0xFFu)); break; }
            case 4u: v = sext ? SextN(raw, 16u) : BinU32(199u, raw, ConstU32(0xFFFFu)); break;
            case 5u: { uint32_t s = BinU32(194u, raw, ConstU32(16u)); v = sext ? SextN(s, 16u) : BinU32(199u, s, ConstU32(0xFFFFu)); break; }
            default: break;
        }
        if (isf) {
            if (abs_v) v = BinU32(199u, v, ConstU32(0x7FFFFFFFu));
            if (neg_v) v = BinU32(198u, v, ConstU32(0x80000000u));
        }
        return v;
    }

    uint32_t EmitSdwaDst(uint32_t old_d, uint32_t res, uint8_t sel, uint8_t unused_m) {
        if ((sel & 7u) >= 6u) return res;
        uint32_t mask_v = 0xFFu, sh_v = 0u, sb_sh = 7u;
        if (sel == 1u) { mask_v = 0xFF00u; sh_v = 8u; }
        else if (sel == 2u) { mask_v = 0xFF0000u; sh_v = 16u; }
        else if (sel == 3u) { mask_v = 0xFF000000u; sh_v = 24u; }
        else if (sel == 4u) { mask_v = 0xFFFFu; sh_v = 0u; sb_sh = 15u; }
        else if (sel == 5u) { mask_v = 0xFFFF0000u; sh_v = 16u; sb_sh = 15u; }
        const uint32_t placed = BinU32(199u, BinU32(196u, res, ConstU32(sh_v)), ConstU32(mask_v));
        if (unused_m == 2u) {
            return BinU32(197u, BinU32(199u, old_d, ConstU32(~mask_v)), placed);
        }
        if (unused_m == 1u) {
            const uint32_t sb = BinU32(199u, BinU32(194u, res, ConstU32(sb_sh)), ConstU32(1u));
            const uint32_t is_neg = BinBool(171u /*INotEqual*/, sb, ConstU32(0u));
            const uint32_t upper = ~(mask_v | (mask_v - 1u));
            return SelU32(is_neg, BinU32(197u, placed, ConstU32(upper)), placed);
        }
        return placed;
    }

    void EmitBarrier() {
        Emit(sec_fn_, 224u /*OpControlBarrier*/, {ConstU32(2u), ConstU32(2u), ConstU32(0x108u)});
    }

    uint32_t EmitLaneXchg(uint32_t val_u32, uint32_t src_lane_in_wave) {
        const uint32_t my_idx = BinU32(128u, gw_base_id_, lane_id_);
        const uint32_t ptr_me = AllocId();
        Emit(sec_fn_, 65u /*OpAccessChain*/, {t_ptr_wg_u32_, ptr_me, var_lanes_, my_idx});
        Emit(sec_fn_, 62u /*OpStore*/, {ptr_me, val_u32});
        EmitBarrier();
        const uint32_t tgt_idx = BinU32(128u, gw_base_id_, src_lane_in_wave);
        const uint32_t ptr_tgt = AllocId();
        Emit(sec_fn_, 65u, {t_ptr_wg_u32_, ptr_tgt, var_lanes_, tgt_idx});
        const uint32_t loaded = AllocId();
        Emit(sec_fn_, 61u /*OpLoad*/, {t_u32_, loaded, ptr_tgt});
        EmitBarrier();
        return loaded;
    }

    Status Build(
        const Xs2ShaderContainer& container,
        const std::vector<SvDecodedInst>& insts,
        const SvShaderUsage& usage,
        std::vector<uint32_t>& out_spv) {
        out_spv.clear();
        const uint32_t tg_x = container.threadgroup_x ? container.threadgroup_x : 1u;
        const uint32_t tg_y = container.threadgroup_y ? container.threadgroup_y : 1u;
        const uint32_t tg_z = container.threadgroup_z ? container.threadgroup_z : 1u;
        const uint32_t wave_n = (container.wave_size == 64u) ? 64u : 32u;
        const uint32_t lane_mask = wave_n - 1u;
        const uint32_t tg_total = std::max(wave_n, tg_x * tg_y * tg_z);
        const bool is_compute = (container.stage == ShaderStage::Compute ||
                                 container.stage == ShaderStage::Mesh ||
                                 container.stage == ShaderStage::Amplification);

        // Capability, ExtInst, MemoryModel
        Emit(sec_caps_, 17u /*OpCapability*/, {1u /*Shader*/});
        ext_glsl_ = AllocId();
        {
            std::vector<uint32_t> ops = {ext_glsl_};
            AppendStr(ops, "GLSL.std.450");
            Emit(sec_ext_, 11u /*OpExtInstImport*/, ops);
        }
        Emit(sec_mem_, 14u /*OpMemoryModel*/, {0u /*Logical*/, 1u /*GLSL450*/});

        // Core types
        t_void_ = AllocId(); Emit(sec_types_, 19u, {t_void_});
        t_bool_ = AllocId(); Emit(sec_types_, 20u, {t_bool_});
        t_u32_ = AllocId();  Emit(sec_types_, 21u, {t_u32_, 32u, 0u});
        t_i32_ = AllocId();  Emit(sec_types_, 21u, {t_i32_, 32u, 1u});
        t_f32_ = AllocId();  Emit(sec_types_, 22u, {t_f32_, 32u});
        t_v2f32_ = AllocId(); Emit(sec_types_, 23u, {t_v2f32_, t_f32_, 2u});
        t_v3u32_ = AllocId(); Emit(sec_types_, 23u, {t_v3u32_, t_u32_, 3u});
        t_v4f32_ = AllocId(); Emit(sec_types_, 23u, {t_v4f32_, t_f32_, 4u});
        t_u32_pair_ = AllocId(); Emit(sec_types_, 30u, {t_u32_pair_, t_u32_, t_u32_});
        t_i32_pair_ = AllocId(); Emit(sec_types_, 30u, {t_i32_pair_, t_i32_, t_i32_});
        t_fn_void_ = AllocId(); Emit(sec_types_, 33u, {t_fn_void_, t_void_});
        t_ptr_fn_u32_ = AllocId(); Emit(sec_types_, 32u, {t_ptr_fn_u32_, 7u /*Function*/, t_u32_});
        t_ptr_fn_bool_ = AllocId(); Emit(sec_types_, 32u, {t_ptr_fn_bool_, 7u /*Function*/, t_bool_});
        c_false_ = AllocId(); Emit(sec_types_, 42u /*OpConstantFalse*/, {t_bool_, c_false_});
        c_true_ = AllocId();  Emit(sec_types_, 41u /*OpConstantTrue*/, {t_bool_, c_true_});

        std::vector<uint32_t> iface_vars;

        // Storage buffers uav0 (binding 0), uav1 (binding 1)
        uint32_t var_uav0 = 0u, var_uav1 = 0u, t_ptr_sb_u32 = 0u;
        if (usage.uses_uav0 || usage.uses_uav1) {
            const uint32_t t_rt_u32 = AllocId();
            const uint32_t t_sb_blk = AllocId();
            const uint32_t t_ptr_sb_blk = AllocId();
            t_ptr_sb_u32 = AllocId();
            Emit(sec_types_, 29u /*OpTypeRuntimeArray*/, {t_rt_u32, t_u32_});
            Emit(sec_decor_, 71u, {t_rt_u32, 6u /*ArrayStride*/, 4u});
            Emit(sec_types_, 30u /*OpTypeStruct*/, {t_sb_blk, t_rt_u32});
            Emit(sec_decor_, 71u, {t_sb_blk, 2u /*Block*/});
            Emit(sec_decor_, 72u, {t_sb_blk, 0u, 35u /*Offset*/, 0u});
            Emit(sec_types_, 32u, {t_ptr_sb_blk, 12u /*StorageBuffer*/, t_sb_blk});
            Emit(sec_types_, 32u, {t_ptr_sb_u32, 12u /*StorageBuffer*/, t_u32_});
            if (usage.uses_uav0) {
                var_uav0 = AllocId();
                Emit(sec_globals_, 59u, {t_ptr_sb_blk, var_uav0, 12u});
                Emit(sec_decor_, 71u, {var_uav0, 34u /*DescriptorSet*/, 0u});
                Emit(sec_decor_, 71u, {var_uav0, 33u /*Binding*/, 0u});
            }
            if (usage.uses_uav1) {
                var_uav1 = AllocId();
                Emit(sec_globals_, 59u, {t_ptr_sb_blk, var_uav1, 12u});
                Emit(sec_decor_, 71u, {var_uav1, 34u /*DescriptorSet*/, 0u});
                Emit(sec_decor_, 71u, {var_uav1, 33u /*Binding*/, 1u});
            }
        }

        // Uniform root constants (binding 2)
        uint32_t var_rc = 0u, t_ptr_unif_u32 = 0u;
        if (usage.uses_root_const) {
            const uint32_t t_rc_blk = AllocId();
            std::vector<uint32_t> members = {t_rc_blk};
            for (uint32_t i = 0u; i < 16u; ++i) {
                members.push_back(t_u32_);
                Emit(sec_decor_, 72u, {t_rc_blk, i, 35u /*Offset*/, i * 4u});
            }
            Emit(sec_types_, 30u, members);
            Emit(sec_decor_, 71u, {t_rc_blk, 2u /*Block*/});
            const uint32_t t_ptr_rc_blk = AllocId();
            t_ptr_unif_u32 = AllocId();
            Emit(sec_types_, 32u, {t_ptr_rc_blk, 2u /*Uniform*/, t_rc_blk});
            Emit(sec_types_, 32u, {t_ptr_unif_u32, 2u /*Uniform*/, t_u32_});
            var_rc = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_rc_blk, var_rc, 2u});
            Emit(sec_decor_, 71u, {var_rc, 34u, 0u});
            Emit(sec_decor_, 71u, {var_rc, 33u, 2u});
        }

        // Workgroup shared lane exchange buffer
        if (usage.uses_cross_lane && is_compute) {
            const uint32_t c_len = ConstU32(tg_total);
            const uint32_t t_arr = AllocId();
            const uint32_t t_ptr_arr = AllocId();
            t_ptr_wg_u32_ = AllocId();
            Emit(sec_types_, 28u /*OpTypeArray*/, {t_arr, t_u32_, c_len});
            Emit(sec_types_, 32u, {t_ptr_arr, 4u /*Workgroup*/, t_arr});
            Emit(sec_types_, 32u, {t_ptr_wg_u32_, 4u /*Workgroup*/, t_u32_});
            var_lanes_ = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_arr, var_lanes_, 4u});
        }

        // Stage inputs/outputs
        uint32_t var_gid = 0u, var_lid = 0u, t_ptr_in_u32 = 0u;
        uint32_t var_vid = 0u, var_pv = 0u, var_mrt0 = 0u, t_ptr_out_v4f32 = 0u;
        if (is_compute) {
            const uint32_t t_ptr_in_v3u32 = AllocId();
            t_ptr_in_u32 = AllocId();
            Emit(sec_types_, 32u, {t_ptr_in_v3u32, 1u /*Input*/, t_v3u32_});
            Emit(sec_types_, 32u, {t_ptr_in_u32, 1u /*Input*/, t_u32_});
            var_gid = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_in_v3u32, var_gid, 1u});
            Emit(sec_decor_, 71u, {var_gid, 11u /*BuiltIn*/, 28u /*GlobalInvocationId*/});
            iface_vars.push_back(var_gid);
            var_lid = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_in_u32, var_lid, 1u});
            Emit(sec_decor_, 71u, {var_lid, 11u /*BuiltIn*/, 29u /*LocalInvocationIndex*/});
            iface_vars.push_back(var_lid);
        } else if (container.stage == ShaderStage::Vertex) {
            const uint32_t t_ptr_in_i32 = AllocId();
            Emit(sec_types_, 32u, {t_ptr_in_i32, 1u, t_i32_});
            var_vid = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_in_i32, var_vid, 1u});
            Emit(sec_decor_, 71u, {var_vid, 11u, 42u /*VertexIndex*/});
            iface_vars.push_back(var_vid);
            const uint32_t t_pv = AllocId();
            const uint32_t t_ptr_out_pv = AllocId();
            t_ptr_out_v4f32 = AllocId();
            Emit(sec_types_, 30u, {t_pv, t_v4f32_});
            Emit(sec_decor_, 71u, {t_pv, 2u /*Block*/});
            Emit(sec_decor_, 72u, {t_pv, 0u, 11u, 0u /*Position*/});
            Emit(sec_types_, 32u, {t_ptr_out_pv, 3u /*Output*/, t_pv});
            Emit(sec_types_, 32u, {t_ptr_out_v4f32, 3u /*Output*/, t_v4f32_});
            var_pv = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_out_pv, var_pv, 3u});
            iface_vars.push_back(var_pv);
        } else if (container.stage == ShaderStage::Pixel) {
            t_ptr_out_v4f32 = AllocId();
            Emit(sec_types_, 32u, {t_ptr_out_v4f32, 3u, t_v4f32_});
            var_mrt0 = AllocId();
            Emit(sec_globals_, 59u, {t_ptr_out_v4f32, var_mrt0, 3u});
            Emit(sec_decor_, 71u, {var_mrt0, 30u /*Location*/, 0u});
            iface_vars.push_back(var_mrt0);
        }

        // EntryPoint & ExecutionMode
        const uint32_t main_id = AllocId();
        {
            uint32_t em = 5u /*GLCompute*/;
            if (container.stage == ShaderStage::Vertex) em = 0u;
            else if (container.stage == ShaderStage::Pixel) em = 4u;
            std::vector<uint32_t> ops = {em, main_id};
            AppendStr(ops, "main");
            ops.insert(ops.end(), iface_vars.begin(), iface_vars.end());
            Emit(sec_entry_, 15u /*OpEntryPoint*/, ops);
            if (is_compute) Emit(sec_exec_, 16u /*OpExecutionMode*/, {main_id, 17u /*LocalSize*/, tg_x, tg_y, tg_z});
            else if (container.stage == ShaderStage::Pixel) Emit(sec_exec_, 16u, {main_id, 7u /*OriginUpperLeft*/});
        }

        // Function body
        Emit(sec_fn_, 54u /*OpFunction*/, {t_void_, main_id, 0u, t_fn_void_});
        const uint32_t entry_lbl = AllocId();
        Emit(sec_fn_, 248u /*OpLabel*/, {entry_lbl});

        // Function-local variables MUST come first in the entry block!
        std::vector<uint32_t> s_vars(usage.max_sgpr, 0u);
        std::vector<uint32_t> v_vars(usage.max_vgpr, 0u);
        for (uint32_t i = 0u; i < usage.max_sgpr; ++i) {
            s_vars[i] = AllocId();
            Emit(sec_fn_, 59u, {t_ptr_fn_u32_, s_vars[i], 7u});
        }
        for (uint32_t i = 0u; i < usage.max_vgpr; ++i) {
            v_vars[i] = AllocId();
            Emit(sec_fn_, 59u, {t_ptr_fn_u32_, v_vars[i], 7u});
        }
        const uint32_t var_scc = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_bool_, var_scc, 7u});
        const uint32_t var_vcc_bit = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_bool_, var_vcc_bit, 7u});
        const uint32_t var_vcc_lo = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_vcc_lo, 7u});
        const uint32_t var_vcc_hi = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_vcc_hi, 7u});
        const uint32_t var_exec_lo = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_exec_lo, 7u});
        const uint32_t var_exec_hi = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_exec_hi, 7u});
        const uint32_t var_m0 = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_m0, 7u});
        const uint32_t var_bw_t = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_bw_t, 7u});
        const uint32_t var_bw_f = AllocId(); Emit(sec_fn_, 59u, {t_ptr_fn_u32_, var_bw_f, 7u});

        // Zero-init registers
        const uint32_t c0 = ConstU32(0u);
        for (uint32_t i = 0u; i < usage.max_sgpr; ++i) Emit(sec_fn_, 62u, {s_vars[i], c0});
        for (uint32_t i = 0u; i < usage.max_vgpr; ++i) Emit(sec_fn_, 62u, {v_vars[i], c0});
        Emit(sec_fn_, 62u, {var_scc, c_false_});
        Emit(sec_fn_, 62u, {var_vcc_bit, c_false_});
        Emit(sec_fn_, 62u, {var_vcc_lo, c0});
        Emit(sec_fn_, 62u, {var_vcc_hi, c0});
        Emit(sec_fn_, 62u, {var_exec_lo, ConstU32(0xFFFFFFFFu)});
        Emit(sec_fn_, 62u, {var_exec_hi, wave_n == 64u ? ConstU32(0xFFFFFFFFu) : c0});
        Emit(sec_fn_, 62u, {var_m0, c0});

        // Seed v0 & lane/gw_base
        if (is_compute) {
            const uint32_t ptr_gx = AllocId();
            Emit(sec_fn_, 65u, {t_ptr_in_u32, ptr_gx, var_gid, ConstU32(0u)});
            const uint32_t tid_x = AllocId();
            Emit(sec_fn_, 61u, {t_u32_, tid_x, ptr_gx});
            Emit(sec_fn_, 62u, {v_vars[0], tid_x});
            const uint32_t gtid = AllocId();
            Emit(sec_fn_, 61u, {t_u32_, gtid, var_lid});
            lane_id_ = BinU32(199u, gtid, ConstU32(lane_mask));
            gw_base_id_ = BinU32(199u, gtid, ConstU32(~lane_mask));
        } else if (container.stage == ShaderStage::Vertex) {
            const uint32_t vid_i = AllocId();
            Emit(sec_fn_, 61u, {t_i32_, vid_i, var_vid});
            Emit(sec_fn_, 62u, {v_vars[0], Bitcast(t_u32_, vid_i)});
            lane_id_ = c0;
            gw_base_id_ = c0;
        } else {
            lane_id_ = c0;
            gw_base_id_ = c0;
        }

        auto load_var = [&](uint32_t var) -> uint32_t {
            const uint32_t r = AllocId();
            Emit(sec_fn_, 61u, {t_u32_, r, var});
            return r;
        };
        auto load_bool = [&](uint32_t var) -> uint32_t {
            const uint32_t r = AllocId();
            Emit(sec_fn_, 61u, {t_bool_, r, var});
            return r;
        };
        auto read_op = [&](const Rdna2Operand& op, bool apply_fmod = false) -> uint32_t {
            uint32_t v = c0;
            switch (op.kind) {
                case Rdna2OperandKind::Sgpr:        v = (op.reg_index < s_vars.size()) ? load_var(s_vars[op.reg_index]) : c0; break;
                case Rdna2OperandKind::Vgpr:        v = (op.reg_index < v_vars.size()) ? load_var(v_vars[op.reg_index]) : c0; break;
                case Rdna2OperandKind::InlineInt:   v = ConstU32(static_cast<uint32_t>(op.int_value)); break;
                case Rdna2OperandKind::InlineFloat: v = ConstU32(FloatToBits(op.float_value)); break;
                case Rdna2OperandKind::Literal32:   v = ConstU32(op.literal_bits); break;
                case Rdna2OperandKind::VccLo:       v = load_var(var_vcc_lo); break;
                case Rdna2OperandKind::ExecLo:      v = load_var(var_exec_lo); break;
                case Rdna2OperandKind::Scc:         v = SelU32(load_bool(var_scc), ConstU32(1u), c0); break;
                case Rdna2OperandKind::M0:          v = load_var(var_m0); break;
                default:                            v = c0; break;
            }
            if (apply_fmod) {
                if (op.abs) v = BinU32(199u, v, ConstU32(0x7FFFFFFFu));
                if (op.neg) v = BinU32(198u, v, ConstU32(0x80000000u));
            }
            return v;
        };
        auto dst_var = [&](const Rdna2Operand& d) -> uint32_t {
            if (d.kind == Rdna2OperandKind::Sgpr && d.reg_index < s_vars.size()) return s_vars[d.reg_index];
            if (d.kind == Rdna2OperandKind::Vgpr && d.reg_index < v_vars.size()) return v_vars[d.reg_index];
            if (d.kind == Rdna2OperandKind::VccLo) return var_vcc_lo;
            if (d.kind == Rdna2OperandKind::ExecLo) return var_exec_lo;
            if (d.kind == Rdna2OperandKind::M0) return var_m0;
            return s_vars[0];
        };

        // Emit instructions
        for (const auto& sv : insts) {
            const auto& inst = sv.base;
            const auto* e = sv.entry;
            const auto& mod = sv.modifier;

            if (!e) {
                if (inst.encoding == Rdna2Encoding::MUBUF) {
                    const uint32_t buf = (inst.srsrc == 0u) ? var_uav0 : var_uav1;
                    if (buf == 0u) continue;
                    const uint32_t vaddr = inst.offen ? read_op(inst.src[1]) : c0;
                    const uint32_t byte_a = BinU32(128u, vaddr, ConstU32(static_cast<uint32_t>(inst.mem_offset)));
                    const uint32_t dw = BinU32(194u, byte_a, ConstU32(2u));
                    const uint32_t ptr = AllocId();
                    Emit(sec_fn_, 65u, {t_ptr_sb_u32, ptr, buf, ConstI32(0), dw});
                    if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                        const uint32_t lv = AllocId();
                        Emit(sec_fn_, 61u, {t_u32_, lv, ptr});
                        Emit(sec_fn_, 62u, {dst_var(inst.dst), lv});
                    } else if (inst.opcode == Rdna2Opcode::BUFFER_STORE_DWORD) {
                        Emit(sec_fn_, 62u, {ptr, read_op(inst.src[0])});
                    }
                } else if (inst.encoding == Rdna2Encoding::FLAT) {
                    const bool is_st = (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD || inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD);
                    const uint32_t buf = is_st ? (var_uav1 ? var_uav1 : var_uav0) : var_uav0;
                    if (buf == 0u) continue;
                    const uint32_t byte_a = BinU32(128u, read_op(inst.src[0]), ConstU32(static_cast<uint32_t>(inst.mem_offset)));
                    const uint32_t dw = BinU32(194u, byte_a, ConstU32(2u));
                    const uint32_t ptr = AllocId();
                    Emit(sec_fn_, 65u, {t_ptr_sb_u32, ptr, buf, ConstI32(0), dw});
                    if (is_st) Emit(sec_fn_, 62u, {ptr, read_op(inst.src[1])});
                    else { const uint32_t lv = AllocId(); Emit(sec_fn_, 61u, {t_u32_, lv, ptr}); Emit(sec_fn_, 62u, {dst_var(inst.dst), lv}); }
                }
                continue;
            }

            const uint16_t raw = inst.raw_opcode;
            const Rdna2Encoding enc = inst.encoding;

            auto resolve_mod_src0 = [&](bool isf, uint32_t& out_a, uint32_t& out_do_write) {
                out_do_write = c_true_;
                if (mod.kind == SvModifierKind::None) {
                    out_a = read_op(inst.src[0], isf);
                    return;
                }
                if (mod.kind == SvModifierKind::Sdwa) {
                    const uint32_t raw_s0 = mod.src0_is_sgpr
                                                ? ((mod.vsrc0 < s_vars.size()) ? load_var(s_vars[mod.vsrc0]) : c0)
                                                : ((mod.vsrc0 < v_vars.size()) ? load_var(v_vars[mod.vsrc0]) : c0);
                    out_a = EmitSdwaSrc(raw_s0, mod.src0_sel, mod.src0_sext, isf, mod.src0_neg, mod.src0_abs);
                    return;
                }
                if (mod.kind == SvModifierKind::Dpp16 && is_compute && var_lanes_ != 0u) {
                    const uint32_t raw_v = (mod.vsrc0 < v_vars.size()) ? load_var(v_vars[mod.vsrc0]) : c0;
                    const uint32_t rb = BinU32(199u, lane_id_, ConstU32(~15u));
                    const uint32_t ir = BinU32(199u, lane_id_, ConstU32(15u));
                    uint32_t sl_u = lane_id_;
                    uint32_t in_bounds = c_true_;
                    if (mod.dpp_ctrl <= 0xFFu) {
                        const uint32_t qb = BinU32(199u, lane_id_, ConstU32(~3u));
                        const uint32_t iq = BinU32(199u, lane_id_, ConstU32(3u));
                        const uint32_t sh = BinU32(196u, iq, ConstU32(1u));
                        const uint32_t sel = BinU32(199u, BinU32(194u, ConstU32(mod.dpp_ctrl), sh), ConstU32(3u));
                        sl_u = BinU32(197u, qb, sel);
                    } else if (mod.dpp_ctrl >= 0x101u && mod.dpp_ctrl <= 0x10Fu) {
                        const uint32_t s = BinU32(128u, ir, ConstU32(mod.dpp_ctrl - 0x100u));
                        in_bounds = BinBool(176u /*ULt*/, s, ConstU32(16u));
                        sl_u = SelU32(in_bounds, BinU32(128u, rb, s), lane_id_);
                    } else if (mod.dpp_ctrl >= 0x111u && mod.dpp_ctrl <= 0x11Fu) {
                        const uint32_t sh = ConstU32(mod.dpp_ctrl - 0x110u);
                        in_bounds = BinBool(174u /*UGe*/, ir, sh);
                        sl_u = SelU32(in_bounds, BinU32(128u, rb, BinU32(130u, ir, sh)), lane_id_);
                    } else if (mod.dpp_ctrl >= 0x121u && mod.dpp_ctrl <= 0x12Fu) {
                        const uint32_t sh = ConstU32(16u - (mod.dpp_ctrl - 0x120u));
                        sl_u = BinU32(128u, rb, BinU32(199u, BinU32(128u, ir, sh), ConstU32(15u)));
                    } else if (mod.dpp_ctrl == kDppRowMirror) {
                        sl_u = BinU32(128u, rb, BinU32(130u, ConstU32(15u), ir));
                    } else if (mod.dpp_ctrl == kDppRowBcast15) {
                        in_bounds = BinBool(174u /*UGe*/, lane_id_, ConstU32(16u));
                        sl_u = SelU32(in_bounds, BinU32(130u, rb, ConstU32(1u)), lane_id_);
                    } else if (mod.dpp_ctrl >= 0x150u && mod.dpp_ctrl <= 0x15Fu) {
                        sl_u = BinU32(128u, rb, ConstU32(mod.dpp_ctrl & 15u));
                    } else if (mod.dpp_ctrl >= 0x160u && mod.dpp_ctrl <= 0x16Fu) {
                        sl_u = BinU32(128u, rb, BinU32(198u, ir, ConstU32(mod.dpp_ctrl & 15u)));
                    }
                    const uint32_t fetched = EmitLaneXchg(raw_v, BinU32(199u, sl_u, ConstU32(lane_mask)));
                    const uint32_t row_bit = BinU32(199u, BinU32(194u, ConstU32(mod.row_mask), BinU32(199u, BinU32(194u, lane_id_, ConstU32(4u)), ConstU32(3u))), ConstU32(1u));
                    const uint32_t bnk_bit = BinU32(199u, BinU32(194u, ConstU32(mod.bank_mask), BinU32(199u, BinU32(194u, lane_id_, ConstU32(2u)), ConstU32(3u))), ConstU32(1u));
                    const uint32_t mask_ok = BinBool(167u /*LAnd*/, BinBool(171u, row_bit, c0), BinBool(171u, bnk_bit, c0));
                    if (mod.bound_ctrl) {
                        out_a = SelU32(in_bounds, fetched, c0);
                        out_do_write = mask_ok;
                    } else {
                        out_a = fetched;
                        out_do_write = BinBool(167u, mask_ok, in_bounds);
                    }
                    if (isf) {
                        if (mod.src0_abs) out_a = BinU32(199u, out_a, ConstU32(0x7FFFFFFFu));
                        if (mod.src0_neg) out_a = BinU32(198u, out_a, ConstU32(0x80000000u));
                    }
                    return;
                }
                if ((mod.kind == SvModifierKind::Dpp8 || mod.kind == SvModifierKind::Dpp8Fi) && is_compute && var_lanes_ != 0u) {
                    const uint32_t raw_v = (mod.vsrc0 < v_vars.size()) ? load_var(v_vars[mod.vsrc0]) : c0;
                    uint32_t packed_sel = 0u;
                    for (uint32_t k = 0u; k < 8u; ++k) packed_sel |= (static_cast<uint32_t>(mod.dpp8_sel[k] & 7u) << (k * 3u));
                    const uint32_t grp = BinU32(199u, lane_id_, ConstU32(~7u));
                    const uint32_t in_grp = BinU32(199u, lane_id_, ConstU32(7u));
                    const uint32_t sh = BinU32(128u, BinU32(196u, in_grp, ConstU32(1u)), in_grp);  // in_grp * 3
                    const uint32_t sel = BinU32(199u, BinU32(194u, ConstU32(packed_sel), sh), ConstU32(7u));
                    out_a = EmitLaneXchg(raw_v, BinU32(197u, grp, sel));
                    return;
                }
                out_a = read_op(inst.src[0], isf);
            };

            if (enc == Rdna2Encoding::SOP2) {
                const uint32_t a = read_op(inst.src[0]);
                const uint32_t b = read_op(inst.src[1]);
                auto read_op_hi = [&](const Rdna2Operand& op) -> uint32_t {
                    if (op.kind == Rdna2OperandKind::Sgpr && static_cast<size_t>(op.reg_index) + 1u < s_vars.size()) {
                        return load_var(s_vars[op.reg_index + 1u]);
                    }
                    if (op.kind == Rdna2OperandKind::VccLo) return load_var(var_vcc_hi);
                    if (op.kind == Rdna2OperandKind::ExecLo) return load_var(var_exec_hi);
                    if (op.kind == Rdna2OperandKind::InlineInt && op.int_value < 0) return ConstU32(0xFFFFFFFFu);
                    return c0;
                };
                auto dst_var_hi = [&](const Rdna2Operand& d) -> uint32_t {
                    if (d.kind == Rdna2OperandKind::Sgpr && static_cast<size_t>(d.reg_index) + 1u < s_vars.size()) return s_vars[d.reg_index + 1u];
                    if (d.kind == Rdna2OperandKind::VccLo) return var_vcc_hi;
                    if (d.kind == Rdna2OperandKind::ExecLo) return var_exec_hi;
                    return 0u;
                };
                const uint32_t dv = dst_var(inst.dst);
                const uint32_t dvh = dst_var_hi(inst.dst);
                uint32_t r = c0;
                uint32_t rh = c0;
                bool write64 = false;
                bool set_scc_nz = false;
                switch (raw) {
                    case 0x00: r = BinU32(128u, a, b); Emit(sec_fn_, 62u, {var_scc, BinBool(176u, r, a)}); break;
                    case 0x01: r = BinU32(130u, a, b); Emit(sec_fn_, 62u, {var_scc, BinBool(172u, b, a)}); break;
                    case 0x02: r = BinU32(128u, a, b); break;
                    case 0x03: r = BinU32(130u, a, b); break;
                    case 0x04: { uint32_t ci = SelU32(load_bool(var_scc), ConstU32(1u), c0); uint32_t t0 = BinU32(128u, a, b); r = BinU32(128u, t0, ci); Emit(sec_fn_, 62u, {var_scc, BinBool(166u, BinBool(176u, t0, a), BinBool(176u, r, t0))}); break; }
                    case 0x05: { uint32_t bi = SelU32(load_bool(var_scc), ConstU32(1u), c0); r = BinU32(130u, BinU32(130u, a, b), bi); Emit(sec_fn_, 62u, {var_scc, BinBool(166u, BinBool(172u, b, a), BinBool(167u, BinBool(170u, b, a), load_bool(var_scc)))}); break; }
                    case 0x06: { uint32_t c = BinBool(177u /*SLt*/, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); Emit(sec_fn_, 62u, {var_scc, c}); r = SelU32(c, a, b); break; }
                    case 0x07: { uint32_t c = BinBool(176u /*ULt*/, a, b); Emit(sec_fn_, 62u, {var_scc, c}); r = SelU32(c, a, b); break; }
                    case 0x08: { uint32_t c = BinBool(173u /*SGt*/, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); Emit(sec_fn_, 62u, {var_scc, c}); r = SelU32(c, a, b); break; }
                    case 0x09: { uint32_t c = BinBool(172u /*UGt*/, a, b); Emit(sec_fn_, 62u, {var_scc, c}); r = SelU32(c, a, b); break; }
                    case 0x0A: r = SelU32(load_bool(var_scc), a, b); break;
                    case 0x0B: r = SelU32(load_bool(var_scc), a, b); rh = SelU32(load_bool(var_scc), read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); write64 = true; break;
                    case 0x0E: r = BinU32(199u, a, b); set_scc_nz = true; break;
                    case 0x0F: r = BinU32(199u, a, b); rh = BinU32(199u, read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); write64 = true; set_scc_nz = true; break;
                    case 0x10: r = BinU32(197u, a, b); set_scc_nz = true; break;
                    case 0x11: r = BinU32(197u, a, b); rh = BinU32(197u, read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); write64 = true; set_scc_nz = true; break;
                    case 0x12: r = BinU32(198u, a, b); set_scc_nz = true; break;
                    case 0x13: r = BinU32(198u, a, b); rh = BinU32(198u, read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); write64 = true; set_scc_nz = true; break;
                    case 0x14: { uint32_t nb = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nb, b}); r = BinU32(199u, a, nb); set_scc_nz = true; break; }
                    case 0x15: { uint32_t nb = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nb, b}); uint32_t nbh = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nbh, read_op_hi(inst.src[1])}); r = BinU32(199u, a, nb); rh = BinU32(199u, read_op_hi(inst.src[0]), nbh); write64 = true; set_scc_nz = true; break; }
                    case 0x16: { uint32_t nb = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nb, b}); r = BinU32(197u, a, nb); set_scc_nz = true; break; }
                    case 0x17: { uint32_t nb = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nb, b}); uint32_t nbh = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nbh, read_op_hi(inst.src[1])}); r = BinU32(197u, a, nb); rh = BinU32(197u, read_op_hi(inst.src[0]), nbh); write64 = true; set_scc_nz = true; break; }
                    case 0x18: { uint32_t t = BinU32(199u, a, b); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, t}); set_scc_nz = true; break; }
                    case 0x19: { uint32_t t = BinU32(199u, a, b); uint32_t th = BinU32(199u, read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, t}); rh = AllocId(); Emit(sec_fn_, 200u, {t_u32_, rh, th}); write64 = true; set_scc_nz = true; break; }
                    case 0x1A: { uint32_t t = BinU32(197u, a, b); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, t}); set_scc_nz = true; break; }
                    case 0x1B: { uint32_t t = BinU32(197u, a, b); uint32_t th = BinU32(197u, read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, t}); rh = AllocId(); Emit(sec_fn_, 200u, {t_u32_, rh, th}); write64 = true; set_scc_nz = true; break; }
                    case 0x1C: { uint32_t t = BinU32(198u, a, b); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, t}); set_scc_nz = true; break; }
                    case 0x1D: { uint32_t t = BinU32(198u, a, b); uint32_t th = BinU32(198u, read_op_hi(inst.src[0]), read_op_hi(inst.src[1])); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, t}); rh = AllocId(); Emit(sec_fn_, 200u, {t_u32_, rh, th}); write64 = true; set_scc_nz = true; break; }
                    case 0x1E: r = BinU32(196u, a, BinU32(199u, b, ConstU32(31u))); set_scc_nz = true; break;
                    case 0x1F: {
                        const uint32_t ah = read_op_hi(inst.src[0]);
                        const uint32_t sh = BinU32(199u, b, ConstU32(63u));
                        const uint32_t sh0 = BinBool(170u, sh, c0);
                        const uint32_t lt32 = BinBool(176u, sh, ConstU32(32u));
                        const uint32_t sh_lo = BinU32(199u, sh, ConstU32(31u));
                        const uint32_t inv = BinU32(199u, BinU32(130u, ConstU32(32u), sh_lo), ConstU32(31u));
                        r = SelU32(lt32, BinU32(196u, a, sh_lo), c0);
                        rh = SelU32(sh0, ah, SelU32(lt32, BinU32(197u, BinU32(196u, ah, sh_lo), BinU32(194u, a, inv)), BinU32(196u, a, sh_lo)));
                        write64 = true; set_scc_nz = true; break;
                    }
                    case 0x20: r = BinU32(194u, a, BinU32(199u, b, ConstU32(31u))); set_scc_nz = true; break;
                    case 0x21: {
                        const uint32_t ah = read_op_hi(inst.src[0]);
                        const uint32_t sh = BinU32(199u, b, ConstU32(63u));
                        const uint32_t sh0 = BinBool(170u, sh, c0);
                        const uint32_t lt32 = BinBool(176u, sh, ConstU32(32u));
                        const uint32_t sh_lo = BinU32(199u, sh, ConstU32(31u));
                        const uint32_t inv = BinU32(199u, BinU32(130u, ConstU32(32u), sh_lo), ConstU32(31u));
                        r = SelU32(sh0, a, SelU32(lt32, BinU32(197u, BinU32(194u, a, sh_lo), BinU32(196u, ah, inv)), BinU32(194u, ah, sh_lo)));
                        rh = SelU32(lt32, BinU32(194u, ah, sh_lo), c0);
                        write64 = true; set_scc_nz = true; break;
                    }
                    case 0x22: r = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, a), BinU32(199u, b, ConstU32(31u)))); set_scc_nz = true; break;
                    case 0x23: {
                        const uint32_t ah = read_op_hi(inst.src[0]);
                        const uint32_t sh = BinU32(199u, b, ConstU32(63u));
                        const uint32_t sh0 = BinBool(170u, sh, c0);
                        const uint32_t lt32 = BinBool(176u, sh, ConstU32(32u));
                        const uint32_t sh_lo = BinU32(199u, sh, ConstU32(31u));
                        const uint32_t inv = BinU32(199u, BinU32(130u, ConstU32(32u), sh_lo), ConstU32(31u));
                        const uint32_t ah_asr = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, ah), sh_lo));
                        const uint32_t ah_s = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, ah), ConstU32(31u)));
                        r = SelU32(sh0, a, SelU32(lt32, BinU32(197u, BinU32(194u, a, sh_lo), BinU32(196u, ah, inv)), ah_asr));
                        rh = SelU32(lt32, ah_asr, ah_s);
                        write64 = true; set_scc_nz = true; break;
                    }
                    case 0x24: r = EmitBfmB32(a, b); break;
                    case 0x26: { uint32_t ge = BinBool(175u /*SGe*/, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); r = SelU32(ge, BinU32(130u, a, b), BinU32(130u, b, a)); set_scc_nz = true; break; }
                    case 0x27: r = EmitSBfeU32(a, b); set_scc_nz = true; break;
                    case 0x28: r = EmitSBfeI32(a, b); set_scc_nz = true; break;
                    case 0x29: EmitSBfe64(a, read_op_hi(inst.src[0]), b, false, r, rh); write64 = true; set_scc_nz = true; break;
                    case 0x2A: EmitSBfe64(a, read_op_hi(inst.src[0]), b, true, r, rh); write64 = true; set_scc_nz = true; break;
                    case 0x2C: r = BinU32(132u, a, b); break;
                    case 0x2E: r = BinU32(128u, BinU32(196u, a, ConstU32(1u)), b); break;
                    case 0x2F: r = BinU32(128u, BinU32(196u, a, ConstU32(2u)), b); break;
                    case 0x30: r = BinU32(128u, BinU32(196u, a, ConstU32(3u)), b); break;
                    case 0x31: r = BinU32(128u, BinU32(196u, a, ConstU32(4u)), b); break;
                    case 0x32: r = BinU32(197u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(196u, BinU32(199u, b, ConstU32(0xFFFFu)), ConstU32(16u))); break;
                    case 0x33: r = BinU32(197u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFF0000u))); break;
                    case 0x34: r = BinU32(197u, BinU32(194u, a, ConstU32(16u)), BinU32(199u, b, ConstU32(0xFFFF0000u))); break;
                    case 0x35: r = EmitMulHiU32(a, b); break;
                    case 0x36: r = EmitMulHiI32(a, b); break;
                    default: r = a; break;
                }
                if (e->has_dst) {
                    Emit(sec_fn_, 62u, {dv, r});
                    if (write64 && dvh != 0u) Emit(sec_fn_, 62u, {dvh, rh});
                }
                if (set_scc_nz) {
                    const uint32_t nz = write64 ? BinBool(171u, BinU32(197u, r, rh), c0) : BinBool(171u, r, c0);
                    Emit(sec_fn_, 62u, {var_scc, nz});
                }
            } else if (enc == Rdna2Encoding::SOP1) {
                const uint32_t a = (inst.src_count > 0u) ? read_op(inst.src[0]) : c0;
                const uint32_t ah = (inst.src_count > 0u && inst.src[0].kind == Rdna2OperandKind::Sgpr && static_cast<size_t>(inst.src[0].reg_index) + 1u < s_vars.size())
                                        ? load_var(s_vars[inst.src[0].reg_index + 1u]) : c0;
                const uint32_t dv = dst_var(inst.dst);
                const uint32_t dvh = (inst.dst.kind == Rdna2OperandKind::Sgpr && static_cast<size_t>(inst.dst.reg_index) + 1u < s_vars.size())
                                         ? s_vars[inst.dst.reg_index + 1u] : 0u;
                uint32_t r = a;
                uint32_t rh = c0;
                bool write64 = false;
                bool do_store = e->has_dst;
                bool set_scc_nz = false;
                switch (raw) {
                    case 0x03: r = a; break;
                    case 0x04: r = a; rh = ah; write64 = true; break;
                    case 0x05: r = SelU32(load_bool(var_scc), a, load_var(dv)); break;
                    case 0x06: r = SelU32(load_bool(var_scc), a, load_var(dv)); rh = (dvh != 0u) ? SelU32(load_bool(var_scc), ah, load_var(dvh)) : c0; write64 = true; break;
                    case 0x07: r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, a}); set_scc_nz = true; break;
                    case 0x08: r = AllocId(); Emit(sec_fn_, 205u /*OpBitCount*/, {t_u32_, r, a}); set_scc_nz = true; break;
                    case 0x09: { uint32_t p0 = AllocId(); Emit(sec_fn_, 205u, {t_u32_, p0, a}); uint32_t p1 = AllocId(); Emit(sec_fn_, 205u, {t_u32_, p1, ah}); r = BinU32(128u, p0, p1); set_scc_nz = true; break; }
                    case 0x0A: r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, a}); rh = AllocId(); Emit(sec_fn_, 200u, {t_u32_, rh, ah}); write64 = true; set_scc_nz = true; break;
                    case 0x0B: r = EmitWqm32(a); set_scc_nz = true; break;
                    case 0x0C: r = EmitWqm32(a); rh = EmitWqm32(ah); write64 = true; set_scc_nz = true; break;
                    case 0x0D: r = AllocId(); Emit(sec_fn_, 204u /*OpBitReverse*/, {t_u32_, r, a}); break;
                    case 0x0E: r = AllocId(); Emit(sec_fn_, 204u, {t_u32_, r, ah}); rh = AllocId(); Emit(sec_fn_, 204u, {t_u32_, rh, a}); write64 = true; break;
                    case 0x0F: { uint32_t na = AllocId(); Emit(sec_fn_, 200u, {t_u32_, na, a}); r = AllocId(); Emit(sec_fn_, 205u, {t_u32_, r, na}); set_scc_nz = true; break; }
                    case 0x10: { uint32_t na = AllocId(); Emit(sec_fn_, 200u, {t_u32_, na, a}); uint32_t nah = AllocId(); Emit(sec_fn_, 200u, {t_u32_, nah, ah}); uint32_t p0 = AllocId(); Emit(sec_fn_, 205u, {t_u32_, p0, na}); uint32_t p1 = AllocId(); Emit(sec_fn_, 205u, {t_u32_, p1, nah}); r = BinU32(128u, p0, p1); set_scc_nz = true; break; }
                    case 0x11: { uint32_t na = AllocId(); Emit(sec_fn_, 200u, {t_u32_, na, a}); r = Glsl1(t_u32_, 73u /*FindILsb*/, na); break; }
                    case 0x13: r = Glsl1(t_u32_, 73u /*FindILsb*/, a); break;
                    case 0x15: { uint32_t msb = Glsl1(t_u32_, 75u /*FindUMsb*/, a); r = SelU32(BinBool(170u, a, c0), ConstU32(0xFFFFFFFFu), BinU32(130u, ConstU32(31u), msb)); break; }
                    case 0x17: { uint32_t msb = Bitcast(t_u32_, Glsl1(t_i32_, 74u /*FindSMsb*/, Bitcast(t_i32_, a))); uint32_t triv = BinBool(166u, BinBool(170u, a, c0), BinBool(170u, a, ConstU32(0xFFFFFFFFu))); r = SelU32(triv, ConstU32(0xFFFFFFFFu), BinU32(130u, ConstU32(31u), msb)); break; }
                    case 0x19: r = SextN(a, 8u); break;
                    case 0x1A: r = SextN(a, 16u); break;
                    case 0x1B: { uint32_t m = AllocId(); Emit(sec_fn_, 200u, {t_u32_, m, BinU32(196u, ConstU32(1u), BinU32(199u, a, ConstU32(31u)))}); r = BinU32(199u, load_var(dv), m); break; }
                    case 0x1C: {
                        const uint32_t bi = BinU32(199u, a, ConstU32(63u));
                        const uint32_t lt32 = BinBool(176u, bi, ConstU32(32u));
                        const uint32_t m = AllocId(); Emit(sec_fn_, 200u, {t_u32_, m, BinU32(196u, ConstU32(1u), BinU32(199u, bi, ConstU32(31u)))});
                        const uint32_t cur_lo = load_var(dv), cur_hi = (dvh != 0u) ? load_var(dvh) : c0;
                        r = SelU32(lt32, BinU32(199u, cur_lo, m), cur_lo);
                        rh = SelU32(lt32, cur_hi, BinU32(199u, cur_hi, m));
                        write64 = true; break;
                    }
                    case 0x21: r = BinU32(197u, load_var(dv), BinU32(196u, ConstU32(1u), BinU32(199u, a, ConstU32(31u)))); break;
                    case 0x22: {
                        const uint32_t bi = BinU32(199u, a, ConstU32(63u));
                        const uint32_t lt32 = BinBool(176u, bi, ConstU32(32u));
                        const uint32_t bmask = BinU32(196u, ConstU32(1u), BinU32(199u, bi, ConstU32(31u)));
                        const uint32_t cur_lo = load_var(dv), cur_hi = (dvh != 0u) ? load_var(dvh) : c0;
                        r = SelU32(lt32, BinU32(197u, cur_lo, bmask), cur_lo);
                        rh = SelU32(lt32, cur_hi, BinU32(197u, cur_hi, bmask));
                        write64 = true; break;
                    }
                    case 0x2C: r = EmitQuadMask32(a); set_scc_nz = true; break;
                    case 0x2D: r = BinU32(197u, EmitQuadMask32(a), BinU32(196u, EmitQuadMask32(ah), ConstU32(8u))); rh = c0; write64 = true; set_scc_nz = true; break;
                    case 0x34: r = Bitcast(t_u32_, Glsl1(t_i32_, 5u /*SAbs*/, Bitcast(t_i32_, a))); set_scc_nz = true; break;
                    case 0x3B: {
                        // s_bitreplicate_b64_b32
                        uint32_t lo = c0, hi = c0;
                        for (uint32_t bit = 0u; bit < 16u; ++bit) {
                            uint32_t b_lo = BinBool(171u, BinU32(199u, a, ConstU32(1u << bit)), c0);
                            lo = BinU32(197u, lo, SelU32(b_lo, ConstU32(3u << (bit * 2u)), c0));
                            uint32_t b_hi = BinBool(171u, BinU32(199u, a, ConstU32(1u << (bit + 16u))), c0);
                            hi = BinU32(197u, hi, SelU32(b_hi, ConstU32(3u << (bit * 2u)), c0));
                        }
                        Emit(sec_fn_, 62u, {dv, lo});
                        if (dvh != 0u) Emit(sec_fn_, 62u, {dvh, hi});
                        do_store = false;
                        break;
                    }
                    default: break;
                }
                if (do_store) {
                    Emit(sec_fn_, 62u, {dv, r});
                    if (write64 && dvh != 0u) Emit(sec_fn_, 62u, {dvh, rh});
                }
                if (set_scc_nz) {
                    const uint32_t nz = write64 ? BinBool(171u, BinU32(197u, r, rh), c0) : BinBool(171u, r, c0);
                    Emit(sec_fn_, 62u, {var_scc, nz});
                }
            } else if (enc == Rdna2Encoding::SOPK) {
                const uint32_t dv = dst_var(inst.dst);
                const uint32_t cur = load_var(dv);
                const uint32_t simm_u = ConstU32(static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                const uint32_t uimm_u = ConstU32(static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16)));
                switch (raw) {
                    case 0x00: Emit(sec_fn_, 62u, {dv, simm_u}); break;
                    case 0x02: Emit(sec_fn_, 62u, {dv, SelU32(load_bool(var_scc), simm_u, cur)}); break;
                    case 0x03: Emit(sec_fn_, 62u, {var_scc, BinBool(170u, cur, simm_u)}); break;
                    case 0x04: Emit(sec_fn_, 62u, {var_scc, BinBool(171u, cur, simm_u)}); break;
                    case 0x05: Emit(sec_fn_, 62u, {var_scc, BinBool(173u, Bitcast(t_i32_, cur), Bitcast(t_i32_, simm_u))}); break;
                    case 0x06: Emit(sec_fn_, 62u, {var_scc, BinBool(175u, Bitcast(t_i32_, cur), Bitcast(t_i32_, simm_u))}); break;
                    case 0x07: Emit(sec_fn_, 62u, {var_scc, BinBool(177u, Bitcast(t_i32_, cur), Bitcast(t_i32_, simm_u))}); break;
                    case 0x08: Emit(sec_fn_, 62u, {var_scc, BinBool(179u /*SLe*/, Bitcast(t_i32_, cur), Bitcast(t_i32_, simm_u))}); break;
                    case 0x09: Emit(sec_fn_, 62u, {var_scc, BinBool(170u, cur, uimm_u)}); break;
                    case 0x0A: Emit(sec_fn_, 62u, {var_scc, BinBool(171u, cur, uimm_u)}); break;
                    case 0x0B: Emit(sec_fn_, 62u, {var_scc, BinBool(172u, cur, uimm_u)}); break;
                    case 0x0C: Emit(sec_fn_, 62u, {var_scc, BinBool(174u, cur, uimm_u)}); break;
                    case 0x0D: Emit(sec_fn_, 62u, {var_scc, BinBool(176u, cur, uimm_u)}); break;
                    case 0x0E: Emit(sec_fn_, 62u, {var_scc, BinBool(178u /*ULe*/, cur, uimm_u)}); break;
                    case 0x0F: Emit(sec_fn_, 62u, {dv, BinU32(128u, cur, simm_u)}); break;
                    case 0x10: Emit(sec_fn_, 62u, {dv, BinU32(132u, cur, simm_u)}); break;
                    default: break;
                }
            } else if (enc == Rdna2Encoding::SOPC) {
                const uint32_t a = read_op(inst.src[0]);
                const uint32_t b = read_op(inst.src[1]);
                const uint32_t ah = (inst.src[0].kind == Rdna2OperandKind::Sgpr && static_cast<size_t>(inst.src[0].reg_index) + 1u < s_vars.size())
                                        ? load_var(s_vars[inst.src[0].reg_index + 1u]) : c0;
                const uint32_t bh = (inst.src[1].kind == Rdna2OperandKind::Sgpr && static_cast<size_t>(inst.src[1].reg_index) + 1u < s_vars.size())
                                        ? load_var(s_vars[inst.src[1].reg_index + 1u]) : c0;
                uint32_t c = c_false_;
                switch (raw) {
                    case 0x00: c = BinBool(170u, a, b); break;
                    case 0x01: c = BinBool(171u, a, b); break;
                    case 0x02: c = BinBool(173u, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); break;
                    case 0x03: c = BinBool(175u, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); break;
                    case 0x04: c = BinBool(177u, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); break;
                    case 0x05: c = BinBool(179u, Bitcast(t_i32_, a), Bitcast(t_i32_, b)); break;
                    case 0x06: c = BinBool(170u, a, b); break;
                    case 0x07: c = BinBool(171u, a, b); break;
                    case 0x08: c = BinBool(172u, a, b); break;
                    case 0x09: c = BinBool(174u, a, b); break;
                    case 0x0A: c = BinBool(176u, a, b); break;
                    case 0x0B: c = BinBool(178u, a, b); break;
                    case 0x0C: c = BinBool(170u, BinU32(199u, a, BinU32(196u, ConstU32(1u), BinU32(199u, b, ConstU32(31u)))), c0); break;
                    case 0x0D: c = BinBool(171u, BinU32(199u, a, BinU32(196u, ConstU32(1u), BinU32(199u, b, ConstU32(31u)))), c0); break;
                    case 0x0E: case 0x0F: {
                        const uint32_t bi = BinU32(199u, b, ConstU32(63u));
                        const uint32_t w = SelU32(BinBool(176u, bi, ConstU32(32u)), a, ah);
                        const uint32_t bitv = BinU32(199u, w, BinU32(196u, ConstU32(1u), BinU32(199u, bi, ConstU32(31u))));
                        c = (raw == 0x0Eu) ? BinBool(170u, bitv, c0) : BinBool(171u, bitv, c0);
                        break;
                    }
                    case 0x10: case 0x12: c = BinBool(167u, BinBool(170u, a, b), BinBool(170u, ah, bh)); break;
                    case 0x11: case 0x13: c = BinBool(166u, BinBool(171u, a, b), BinBool(171u, ah, bh)); break;
                    default: break;
                }
                Emit(sec_fn_, 62u, {var_scc, c});
            } else if (enc == Rdna2Encoding::SMEM) {
                if (raw == 0x2Au) {
                    if (inst.dst.reg_index < s_vars.size()) {
                        Emit(sec_fn_, 62u, {s_vars[inst.dst.reg_index], BinU32(194u, gw_base_id_, ConstU32(wave_n == 64u ? 6u : 5u))});
                    }
                } else if (var_rc != 0u) {
                    const uint32_t cnt = (raw <= 0x04u) ? (1u << raw)
                                       : ((raw >= 0x08u && raw <= 0x0Cu) ? (1u << (raw - 0x08u))
                                       : ((raw >= 0x10u && raw <= 0x12u) ? (1u << (raw - 0x10u)) : 0u));
                    if (inst.soffset < s_vars.size()) {
                        // Signed 21-bit offset + dynamic soffset with safe OOB clamping to 0
                        const uint32_t soff_v = load_var(s_vars[inst.soffset]);
                        const uint32_t off_i = ConstI32(inst.mem_offset);
                        const uint32_t byte_i = Bitcast(t_i32_, BinU32(128u /*IAdd*/, soff_v, Bitcast(t_u32_, off_i)));
                        const uint32_t is_neg = BinBool(177u /*SLessThan*/, byte_i, ConstI32(0));
                        const uint32_t base_dw = BinU32(194u /*Shr*/, Bitcast(t_u32_, byte_i), ConstU32(2u));
                        uint32_t rc_vals[16]{};
                        for (uint32_t ri = 0u; ri < 16u; ++ri) {
                            const uint32_t ptr = AllocId();
                            Emit(sec_fn_, 65u, {t_ptr_unif_u32, ptr, var_rc, ConstI32(static_cast<int32_t>(ri))});
                            rc_vals[ri] = AllocId();
                            Emit(sec_fn_, 61u, {t_u32_, rc_vals[ri], ptr});
                        }
                        for (uint32_t k = 0u; k < cnt && (inst.dst.reg_index + k) < s_vars.size(); ++k) {
                            const uint32_t dw_k = BinU32(128u, base_dw, ConstU32(k));
                            uint32_t acc = c0;
                            for (uint32_t ri = 0u; ri < 16u; ++ri) {
                                acc = SelU32(BinBool(170u /*IEqual*/, dw_k, ConstU32(ri)), rc_vals[ri], acc);
                            }
                            Emit(sec_fn_, 62u, {s_vars[inst.dst.reg_index + k], SelU32(is_neg, c0, acc)});
                        }
                    } else {
                        for (uint32_t k = 0u; k < cnt && (inst.dst.reg_index + k) < s_vars.size(); ++k) {
                            const int64_t eff = static_cast<int64_t>(inst.mem_offset) + static_cast<int64_t>(k) * 4ll;
                            if (eff < 0 || (eff >> 2) >= 16ll) {
                                Emit(sec_fn_, 62u, {s_vars[inst.dst.reg_index + k], c0});
                            } else {
                                const uint32_t ptr = AllocId();
                                Emit(sec_fn_, 65u, {t_ptr_unif_u32, ptr, var_rc, ConstI32(static_cast<int32_t>(eff >> 2))});
                                const uint32_t lv = AllocId();
                                Emit(sec_fn_, 61u, {t_u32_, lv, ptr});
                                Emit(sec_fn_, 62u, {s_vars[inst.dst.reg_index + k], lv});
                            }
                        }
                    }
                }
            } else if (enc == Rdna2Encoding::VOP1 || (enc == Rdna2Encoding::VOP3 && raw >= 0x180u && raw < 0x200u)) {
                const uint16_t v1op = (enc == Rdna2Encoding::VOP3) ? static_cast<uint16_t>(raw - 0x180u) : raw;
                const bool isf = (e->data_kind == DK::Float);
                if (v1op == 0x02u) {
                    const uint32_t val = read_op(inst.src[0]);
                    const uint32_t res = (is_compute && var_lanes_ != 0u) ? EmitLaneXchg(val, c0) : val;
                    Emit(sec_fn_, 62u, {dst_var(inst.dst), res});
                } else if (v1op != 0x00u && v1op != 0x1Bu && v1op != 0x41u) {
                    uint32_t a = c0, do_w = c_true_;
                    resolve_mod_src0(isf, a, do_w);
                    const uint32_t dv = dst_var(inst.dst);
                    const uint32_t old_d = load_var(dv);
                    const uint32_t fa = Bitcast(t_f32_, a);
                    uint32_t r = a;
                    switch (v1op) {
                        case 0x01: r = a; break;
                        case 0x05: { uint32_t f = AllocId(); Emit(sec_fn_, 111u /*SToF*/, {t_f32_, f, Bitcast(t_i32_, a)}); r = Bitcast(t_u32_, f); break; }
                        case 0x06: { uint32_t f = AllocId(); Emit(sec_fn_, 112u /*UToF*/, {t_f32_, f, a}); r = Bitcast(t_u32_, f); break; }
                        case 0x07: { uint32_t u = AllocId(); Emit(sec_fn_, 109u /*FToU*/, {t_u32_, u, Glsl2(t_f32_, 40u /*FMax*/, fa, ConstF32(0.0f))}); r = u; break; }
                        case 0x08: { uint32_t i = AllocId(); Emit(sec_fn_, 110u /*FToS*/, {t_i32_, i, fa}); r = Bitcast(t_u32_, i); break; }
                        case 0x0A: r = PackF16Lo(fa); break;
                        case 0x0B: r = Bitcast(t_u32_, UnpackF16Lo(a)); break;
                        case 0x0C: { uint32_t i = AllocId(); Emit(sec_fn_, 110u, {t_i32_, i, Glsl1(t_f32_, 8u /*Floor*/, BinF32(129u, fa, ConstF32(0.5f)))}); r = Bitcast(t_u32_, i); break; }
                        case 0x0D: { uint32_t i = AllocId(); Emit(sec_fn_, 110u, {t_i32_, i, Glsl1(t_f32_, 8u /*Floor*/, fa)}); r = Bitcast(t_u32_, i); break; }
                        case 0x11: { uint32_t f = AllocId(); Emit(sec_fn_, 112u, {t_f32_, f, BinU32(199u, a, ConstU32(0xFFu))}); r = Bitcast(t_u32_, f); break; }
                        case 0x12: { uint32_t f = AllocId(); Emit(sec_fn_, 112u, {t_f32_, f, BinU32(199u, BinU32(194u, a, ConstU32(8u)), ConstU32(0xFFu))}); r = Bitcast(t_u32_, f); break; }
                        case 0x13: { uint32_t f = AllocId(); Emit(sec_fn_, 112u, {t_f32_, f, BinU32(199u, BinU32(194u, a, ConstU32(16u)), ConstU32(0xFFu))}); r = Bitcast(t_u32_, f); break; }
                        case 0x14: { uint32_t f = AllocId(); Emit(sec_fn_, 112u, {t_f32_, f, BinU32(199u, BinU32(194u, a, ConstU32(24u)), ConstU32(0xFFu))}); r = Bitcast(t_u32_, f); break; }
                        case 0x20: {
                            const uint32_t nonfin = BinBool(174u /*UGe*/, BinU32(199u, a, ConstU32(0x7FFFFFFFu)), ConstU32(0x7F800000u));
                            r = SelU32(nonfin, ConstU32(0x7FC00000u), Bitcast(t_u32_, Glsl1(t_f32_, 10u /*Fract*/, fa)));
                            break;
                        }
                        case 0x21: r = Bitcast(t_u32_, Glsl1(t_f32_, 3u /*Trunc*/, fa)); break;
                        case 0x22: r = Bitcast(t_u32_, Glsl1(t_f32_, 9u /*Ceil*/, fa)); break;
                        case 0x23: r = Bitcast(t_u32_, Glsl1(t_f32_, 2u /*RoundEven*/, fa)); break;
                        case 0x24: r = Bitcast(t_u32_, Glsl1(t_f32_, 8u /*Floor*/, fa)); break;
                        case 0x25: r = Bitcast(t_u32_, Glsl1(t_f32_, 29u /*Exp2*/, fa)); break;
                        case 0x27: r = Bitcast(t_u32_, Glsl1(t_f32_, 30u /*Log2*/, fa)); break;
                        case 0x2A: case 0x2B: r = EmitExactRcpF32Bits(a); break;
                        case 0x2E: r = EmitExactRsqF32Bits(a); break;
                        case 0x33: r = EmitExactSqrtF32Bits(a); break;
                        case 0x35: r = Bitcast(t_u32_, EmitTrigTurnsF32(fa, false)); break;
                        case 0x36: r = Bitcast(t_u32_, EmitTrigTurnsF32(fa, true)); break;
                        case 0x37: r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, a}); break;
                        case 0x38: r = AllocId(); Emit(sec_fn_, 204u, {t_u32_, r, a}); break;
                        case 0x39: { uint32_t msb = Glsl1(t_u32_, 75u, a); r = SelU32(BinBool(170u, a, c0), ConstU32(0xFFFFFFFFu), BinU32(130u, ConstU32(31u), msb)); break; }
                        case 0x3A: r = Glsl1(t_u32_, 73u, a); break;
                        case 0x3B: { uint32_t msb = Bitcast(t_u32_, Glsl1(t_i32_, 74u, Bitcast(t_i32_, a))); uint32_t triv = BinBool(166u, BinBool(170u, a, c0), BinBool(170u, a, ConstU32(0xFFFFFFFFu))); r = SelU32(triv, ConstU32(0xFFFFFFFFu), BinU32(130u, ConstU32(31u), msb)); break; }
                        case 0x3F: {
                            // frexp_exp_f32 via IEEE exponent extraction for normal non-zero floats
                            const uint32_t exp_raw = BinU32(199u, BinU32(194u, a, ConstU32(23u)), ConstU32(0xFFu));
                            const uint32_t is_z = BinBool(170u, BinU32(199u, a, ConstU32(0x7FFFFFFFu)), c0);
                            r = SelU32(is_z, c0, BinU32(130u, exp_raw, ConstU32(126u)));
                            break;
                        }
                        case 0x40: {
                            // frexp_mant_f32: keep sign + frac, set exponent = 126 (0x3F000000 -> [0.5, 1.0))
                            const uint32_t is_z = BinBool(170u, BinU32(199u, a, ConstU32(0x7FFFFFFFu)), c0);
                            const uint32_t mant = BinU32(197u, BinU32(199u, a, ConstU32(0x807FFFFFu)), ConstU32(0x3F000000u));
                            r = SelU32(is_z, a, mant);
                            break;
                        }
                        case 0x50: { uint32_t f = AllocId(); Emit(sec_fn_, 112u, {t_f32_, f, BinU32(199u, a, ConstU32(0xFFFFu))}); r = PackF16Lo(f); break; }
                        case 0x51: { uint32_t f = AllocId(); Emit(sec_fn_, 111u, {t_f32_, f, Bitcast(t_i32_, SextN(a, 16u))}); r = PackF16Lo(f); break; }
                        case 0x52: { uint32_t hf = UnpackF16Lo(a); uint32_t u = AllocId(); Emit(sec_fn_, 109u, {t_u32_, u, Glsl3(t_f32_, 43u /*FClamp*/, hf, ConstF32(0.0f), ConstF32(65535.0f))}); r = u; break; }
                        case 0x53: { uint32_t hf = UnpackF16Lo(a); uint32_t i = AllocId(); Emit(sec_fn_, 110u, {t_i32_, i, Glsl3(t_f32_, 43u, hf, ConstF32(-32768.0f), ConstF32(32767.0f))}); r = BinU32(199u, Bitcast(t_u32_, i), ConstU32(0xFFFFu)); break; }
                        case 0x54: r = PackF16Lo(BinF32(136u, ConstF32(1.0f), UnpackF16Lo(a))); break;
                        case 0x55: r = PackF16Lo(Glsl1(t_f32_, 31u, UnpackF16Lo(a))); break;
                        case 0x56: r = PackF16Lo(Glsl1(t_f32_, 32u, UnpackF16Lo(a))); break;
                        case 0x57: r = PackF16Lo(Glsl1(t_f32_, 30u, UnpackF16Lo(a))); break;
                        case 0x58: r = PackF16Lo(Glsl1(t_f32_, 29u, UnpackF16Lo(a))); break;
                        case 0x59: { uint32_t hb = Bitcast(t_u32_, UnpackF16Lo(a)); uint32_t iz = BinBool(170u, BinU32(199u, hb, ConstU32(0x7FFFFFFFu)), c0); uint32_t m = BinU32(197u, BinU32(199u, hb, ConstU32(0x807FFFFFu)), ConstU32(0x3F000000u)); r = PackF16Lo(Bitcast(t_f32_, SelU32(iz, hb, m))); break; }
                        case 0x5A: { uint32_t hb = Bitcast(t_u32_, UnpackF16Lo(a)); uint32_t iz = BinBool(170u, BinU32(199u, hb, ConstU32(0x7FFFFFFFu)), c0); uint32_t er = BinU32(199u, BinU32(194u, hb, ConstU32(23u)), ConstU32(0xFFu)); r = BinU32(199u, SelU32(iz, c0, BinU32(130u, er, ConstU32(126u))), ConstU32(0xFFFFu)); break; }
                        case 0x5B: r = PackF16Lo(Glsl1(t_f32_, 8u, UnpackF16Lo(a))); break;
                        case 0x5C: r = PackF16Lo(Glsl1(t_f32_, 9u, UnpackF16Lo(a))); break;
                        case 0x5D: r = PackF16Lo(Glsl1(t_f32_, 3u, UnpackF16Lo(a))); break;
                        case 0x5E: r = PackF16Lo(Glsl1(t_f32_, 2u, UnpackF16Lo(a))); break;
                        case 0x5F: {
                            const uint32_t nonfin = BinBool(174u /*UGe*/, BinU32(199u, a, ConstU32(0x7FFFu)), ConstU32(0x7C00u));
                            r = SelU32(nonfin, ConstU32(0x7E00u), PackF16Lo(Glsl1(t_f32_, 10u, UnpackF16Lo(a))));
                            break;
                        }
                        case 0x60: r = PackF16Lo(EmitTrigTurnsF32(UnpackF16Lo(a), false)); break;
                        case 0x61: r = PackF16Lo(EmitTrigTurnsF32(UnpackF16Lo(a), true)); break;
                        case 0x62: {
                            const uint32_t lo = Bitcast(t_u32_, Glsl3(t_i32_, 45u /*SClamp*/, Bitcast(t_i32_, SextN(a, 16u)), ConstI32(0), ConstI32(255)));
                            const uint32_t hi = Bitcast(t_u32_, Glsl3(t_i32_, 45u, Bitcast(t_i32_, SextN(BinU32(194u, a, ConstU32(16u)), 16u)), ConstI32(0), ConstI32(255)));
                            r = BinU32(197u, lo, BinU32(196u, hi, ConstU32(8u)));
                            break;
                        }
                        case 0x63: {
                            const uint32_t hf = UnpackF16Lo(a);
                            const uint32_t is_n = AllocId(); Emit(sec_fn_, 156u, {t_bool_, is_n, hf});
                            const uint32_t cl = Glsl3(t_f32_, 43u, SelF32(is_n, ConstF32(0.0f), hf), ConstF32(-1.0f), ConstF32(1.0f));
                            const uint32_t q = AllocId(); Emit(sec_fn_, 110u, {t_i32_, q, Glsl1(t_f32_, 2u, BinF32(133u, cl, ConstF32(32767.0f)))});
                            r = BinU32(199u, Bitcast(t_u32_, q), ConstU32(0xFFFFu));
                            break;
                        }
                        case 0x64: {
                            const uint32_t hf = UnpackF16Lo(a);
                            const uint32_t is_n = AllocId(); Emit(sec_fn_, 156u, {t_bool_, is_n, hf});
                            const uint32_t cl = Glsl3(t_f32_, 43u, SelF32(is_n, ConstF32(0.0f), hf), ConstF32(0.0f), ConstF32(1.0f));
                            const uint32_t q = AllocId(); Emit(sec_fn_, 109u, {t_u32_, q, Glsl1(t_f32_, 2u, BinF32(133u, cl, ConstF32(65535.0f)))});
                            r = BinU32(199u, q, ConstU32(0xFFFFu));
                            break;
                        }
                        case 0x65: {
                            if (inst.src[0].kind == Rdna2OperandKind::Vgpr && inst.src[0].reg_index < v_vars.size()) {
                                Emit(sec_fn_, 62u, {v_vars[inst.src[0].reg_index], old_d});
                            }
                            r = a;
                            break;
                        }
                        default: break;
                    }
                    if (mod.kind == SvModifierKind::Sdwa) {
                        r = EmitSdwaDst(old_d, r, mod.dst_sel, mod.dst_unused);
                    }
                    Emit(sec_fn_, 62u, {dv, SelU32(do_w, r, old_d)});
                }
            } else if (enc == Rdna2Encoding::VOP2 || (enc == Rdna2Encoding::VOP3 && raw >= 0x100u && raw < 0x140u)) {
                const uint16_t v2op = (enc == Rdna2Encoding::VOP3) ? static_cast<uint16_t>(raw - 0x100u) : raw;
                const bool isf = (e->data_kind == DK::Float);
                uint32_t a = c0, do_w = c_true_;
                resolve_mod_src0(isf, a, do_w);
                uint32_t b = read_op(inst.src[1], isf);
                if (mod.kind == SvModifierKind::Sdwa) {
                    b = EmitSdwaSrc(read_op(inst.src[1], false), mod.src1_sel, mod.src1_sext, isf, mod.src1_neg, mod.src1_abs);
                }
                const uint32_t dv = dst_var(inst.dst);
                const uint32_t old_d = load_var(dv);
                const uint32_t fa = Bitcast(t_f32_, a), fb = Bitcast(t_f32_, b), fd = Bitcast(t_f32_, old_d);
                uint32_t r = c0;
                switch (v2op) {
                    case 0x01: {
                        // Blackwell select-after-shuffle guard: spill through distinct Function temps
                        Emit(sec_fn_, 62u, {var_bw_t, b});
                        Emit(sec_fn_, 62u, {var_bw_f, a});
                        r = SelU32(load_bool(var_vcc_bit), load_var(var_bw_t), load_var(var_bw_f));
                        break;
                    }
                    case 0x02: r = Bitcast(t_u32_, BinF32(129u, BinF32(129u, BinF32(133u, UnpackF16Lo(a), UnpackF16Lo(b)), BinF32(133u, UnpackF16Hi(a), UnpackF16Hi(b))), fd)); break;
                    case 0x03: r = Bitcast(t_u32_, BinF32(129u, fa, fb)); break;
                    case 0x04: r = Bitcast(t_u32_, BinF32(131u, fa, fb)); break;
                    case 0x05: r = Bitcast(t_u32_, BinF32(131u, fb, fa)); break;
                    case 0x06: case 0x07: {
                        const uint32_t az = BinBool(180u, fa, ConstF32(0.0f));
                        const uint32_t bz = BinBool(180u, fb, ConstF32(0.0f));
                        const uint32_t p = SelF32(BinBool(166u, az, bz), ConstF32(0.0f), BinF32(133u, fa, fb));
                        r = Bitcast(t_u32_, (v2op == 0x06u) ? BinF32(129u, p, fd) : p);
                        break;
                    }
                    case 0x08: r = Bitcast(t_u32_, BinF32(133u, fa, fb)); break;
                    case 0x09: r = BinU32(132u, SextN(a, 24u), SextN(b, 24u)); break;
                    case 0x0A: r = EmitMulHiI32(SextN(a, 24u), SextN(b, 24u)); break;
                    case 0x0B: r = BinU32(132u, BinU32(199u, a, ConstU32(0xFFFFFFu)), BinU32(199u, b, ConstU32(0xFFFFFFu))); break;
                    case 0x0C: r = EmitMulHiU32(BinU32(199u, a, ConstU32(0xFFFFFFu)), BinU32(199u, b, ConstU32(0xFFFFFFu))); break;
                    case 0x0D: {
                        uint32_t acc = old_d;
                        for (uint32_t k = 0u; k < 4u; ++k) {
                            acc = BinU32(128u, acc, BinU32(132u, SextN(BinU32(194u, a, ConstU32(k * 8u)), 8u), SextN(BinU32(194u, b, ConstU32(k * 8u)), 8u)));
                        }
                        r = acc; break;
                    }
                    case 0x0E: {
                        uint32_t acc = old_d;
                        for (uint32_t k = 0u; k < 8u; ++k) {
                            acc = BinU32(128u, acc, BinU32(132u, SextN(BinU32(194u, a, ConstU32(k * 4u)), 4u), SextN(BinU32(194u, b, ConstU32(k * 4u)), 4u)));
                        }
                        r = acc; break;
                    }
                    case 0x0F: r = Bitcast(t_u32_, Glsl2(t_f32_, 37u /*FMin*/, fa, fb)); break;
                    case 0x10: r = Bitcast(t_u32_, Glsl2(t_f32_, 40u /*FMax*/, fa, fb)); break;
                    case 0x11: r = Bitcast(t_u32_, Glsl2(t_i32_, 39u /*SMin*/, Bitcast(t_i32_, a), Bitcast(t_i32_, b))); break;
                    case 0x12: r = Bitcast(t_u32_, Glsl2(t_i32_, 42u /*SMax*/, Bitcast(t_i32_, a), Bitcast(t_i32_, b))); break;
                    case 0x13: r = Glsl2(t_u32_, 38u /*UMin*/, a, b); break;
                    case 0x14: r = Glsl2(t_u32_, 41u /*UMax*/, a, b); break;
                    case 0x16: r = BinU32(194u, b, BinU32(199u, a, ConstU32(31u))); break;
                    case 0x17: r = Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, b), BinU32(199u, a, ConstU32(31u)))); break;
                    case 0x18: r = BinU32(196u, b, BinU32(199u, a, ConstU32(31u))); break;
                    case 0x1B: r = BinU32(199u, a, b); break;
                    case 0x1C: r = BinU32(197u, a, b); break;
                    case 0x1D: r = BinU32(198u, a, b); break;
                    case 0x1E: { uint32_t x = BinU32(198u, a, b); r = AllocId(); Emit(sec_fn_, 200u, {t_u32_, r, x}); break; }
                    case 0x1F: r = Bitcast(t_u32_, BinF32(129u, BinF32(133u, fa, fb), fd)); break;
                    case 0x21: r = Bitcast(t_u32_, BinF32(129u, BinF32(133u, fa, ConstF32(BitsToFloat(inst.src[1].literal_bits))), Bitcast(t_f32_, read_op(inst.src[2])))); break;
                    case 0x22: r = Bitcast(t_u32_, BinF32(129u, BinF32(133u, fa, fb), ConstF32(BitsToFloat(inst.src[2].literal_bits)))); break;
                    case 0x25: r = BinU32(128u, a, b); break;
                    case 0x26: r = BinU32(130u, a, b); break;
                    case 0x27: r = BinU32(130u, b, a); break;
                    case 0x28: { uint32_t ci = SelU32(load_bool(var_vcc_bit), ConstU32(1u), c0); uint32_t t0 = BinU32(128u, a, b); r = BinU32(128u, t0, ci); Emit(sec_fn_, 62u, {var_vcc_bit, BinBool(166u, BinBool(176u, t0, a), BinBool(176u, r, t0))}); break; }
                    case 0x29: { uint32_t bi = SelU32(load_bool(var_vcc_bit), ConstU32(1u), c0); r = BinU32(130u, BinU32(130u, a, b), bi); Emit(sec_fn_, 62u, {var_vcc_bit, BinBool(166u, BinBool(172u, b, a), BinBool(167u, BinBool(170u, b, a), load_bool(var_vcc_bit)))}); break; }
                    case 0x2A: { uint32_t bi = SelU32(load_bool(var_vcc_bit), ConstU32(1u), c0); r = BinU32(130u, BinU32(130u, b, a), bi); Emit(sec_fn_, 62u, {var_vcc_bit, BinBool(166u, BinBool(172u, a, b), BinBool(167u, BinBool(170u, a, b), load_bool(var_vcc_bit)))}); break; }
                    case 0x2B: r = Bitcast(t_u32_, Glsl3(t_f32_, 50u /*Fma*/, fa, fb, fd)); break;
                    case 0x2C: r = Bitcast(t_u32_, Glsl3(t_f32_, 50u, fa, ConstF32(BitsToFloat(inst.src[1].literal_bits)), Bitcast(t_f32_, read_op(inst.src[2])))); break;
                    case 0x2D: r = Bitcast(t_u32_, Glsl3(t_f32_, 50u, fa, fb, ConstF32(BitsToFloat(inst.src[2].literal_bits)))); break;
                    case 0x2F: r = PackF16Pair(fa, fb); break;
                    case 0x32: r = PackF16Lo(BinF32(129u, UnpackF16Lo(a), UnpackF16Lo(b))); break;
                    case 0x33: r = PackF16Lo(BinF32(131u, UnpackF16Lo(a), UnpackF16Lo(b))); break;
                    case 0x34: r = PackF16Lo(BinF32(131u, UnpackF16Lo(b), UnpackF16Lo(a))); break;
                    case 0x35: r = PackF16Lo(BinF32(133u, UnpackF16Lo(a), UnpackF16Lo(b))); break;
                    case 0x36: r = PackF16Lo(Glsl3(t_f32_, 50u, UnpackF16Lo(a), UnpackF16Lo(b), UnpackF16Lo(old_d))); break;
                    case 0x37: r = PackF16Lo(Glsl3(t_f32_, 50u, UnpackF16Lo(a), UnpackF16Lo(ConstU32(inst.src[1].literal_bits)), UnpackF16Lo(read_op(inst.src[2])))); break;
                    case 0x38: r = PackF16Lo(Glsl3(t_f32_, 50u, UnpackF16Lo(a), UnpackF16Lo(b), UnpackF16Lo(ConstU32(inst.src[2].literal_bits)))); break;
                    case 0x39: r = PackF16Lo(Glsl2(t_f32_, 40u /*FMax*/, UnpackF16Lo(a), UnpackF16Lo(b))); break;
                    case 0x3A: r = PackF16Lo(Glsl2(t_f32_, 37u /*FMin*/, UnpackF16Lo(a), UnpackF16Lo(b))); break;
                    case 0x3B: r = PackF16Lo(Glsl2(t_f32_, 53u /*Ldexp*/, UnpackF16Lo(a), Bitcast(t_i32_, SextN(b, 16u)))); break;
                    case 0x3C: r = PackF16Pair(Glsl3(t_f32_, 50u, UnpackF16Lo(a), UnpackF16Lo(b), UnpackF16Lo(old_d)), Glsl3(t_f32_, 50u, UnpackF16Hi(a), UnpackF16Hi(b), UnpackF16Hi(old_d))); break;
                    default: r = BinU32(128u, a, b); break;
                }
                if (mod.kind == SvModifierKind::Sdwa) {
                    r = EmitSdwaDst(old_d, r, mod.dst_sel, mod.dst_unused);
                }
                Emit(sec_fn_, 62u, {dv, SelU32(do_w, r, old_d)});
            } else if (enc == Rdna2Encoding::VOPC || (enc == Rdna2Encoding::VOP3 && raw < 0x100u)) {
                const bool is_f16_cmp = (raw == 0x8Fu || raw == 0x9Fu ||
                                         (raw >= 0xC8u && raw <= 0xCFu) || (raw >= 0xD8u && raw <= 0xDFu) ||
                                         (raw >= 0xE8u && raw <= 0xEFu) || (raw >= 0xF8u && raw <= 0xFFu));
                auto read_f16_op = [&](const Rdna2Operand& op) -> uint32_t {
                    uint32_t h = BinU32(199u, read_op(op, false), ConstU32(0xFFFFu));
                    if (op.abs) h = BinU32(199u, h, ConstU32(0x7FFFu));
                    if (op.neg) h = BinU32(198u, h, ConstU32(0x8000u));
                    return h;
                };
                const uint32_t a = is_f16_cmp ? read_f16_op(inst.src[0]) : read_op(inst.src[0], e->data_kind == DK::Float);
                const uint32_t b = is_f16_cmp
                                       ? ((raw == 0x8Fu || raw == 0x9Fu) ? read_op(inst.src[1], false) : read_f16_op(inst.src[1]))
                                       : read_op(inst.src[1], (e->data_kind == DK::Float) && raw != 0x88u && raw != 0x98u);
                uint32_t c = c_false_;
                if (raw == 0x88u || raw == 0x98u) {
                    c = EmitFClassBits(a, b);
                } else if (raw == 0x8Fu || raw == 0x9Fu) {
                    c = EmitFClassF16Bits(a, b);
                } else if ((raw >= 0x10u && raw <= 0x1Fu) || (raw >= 0x30u && raw <= 0x3Fu)) {
                    c = EmitCmpF32Bits(a, b, raw & 15u);
                } else if (is_f16_cmp) {
                    const uint32_t cc = ((raw & 0x10u) ? 8u : 0u) | (raw & 7u);
                    c = EmitCmpF16Bits(a, b, cc);
                } else if ((raw >= 0x50u && raw <= 0x57u) || (raw >= 0x70u && raw <= 0x77u)) {
                    const uint32_t ia = Bitcast(t_i32_, SextN(a, 16u)), ib = Bitcast(t_i32_, SextN(b, 16u));
                    switch (raw & 7u) {
                        case 1: c = BinBool(177u, ia, ib); break;
                        case 2: c = BinBool(170u, ia, ib); break;
                        case 3: c = BinBool(179u, ia, ib); break;
                        case 4: c = BinBool(173u, ia, ib); break;
                        case 5: c = BinBool(171u, ia, ib); break;
                        case 6: c = BinBool(175u, ia, ib); break;
                        case 7: c = c_true_; break;
                        default: c = c_false_; break;
                    }
                } else if ((raw >= 0x60u && raw <= 0x67u) || (raw >= 0x78u && raw <= 0x7Fu)) {
                    const uint32_t ua = BinU32(199u, a, ConstU32(0xFFFFu)), ub = BinU32(199u, b, ConstU32(0xFFFFu));
                    switch (raw & 7u) {
                        case 1: c = BinBool(176u, ua, ub); break;
                        case 2: c = BinBool(170u, ua, ub); break;
                        case 3: c = BinBool(178u, ua, ub); break;
                        case 4: c = BinBool(172u, ua, ub); break;
                        case 5: c = BinBool(171u, ua, ub); break;
                        case 6: c = BinBool(174u, ua, ub); break;
                        case 7: c = c_true_; break;
                        default: c = c_false_; break;
                    }
                } else if ((raw >= 0x80u && raw <= 0x87u) || (raw >= 0x90u && raw <= 0x97u)) {
                    const uint32_t ia = Bitcast(t_i32_, a), ib = Bitcast(t_i32_, b);
                    switch (raw & 7u) {
                        case 1: c = BinBool(177u, ia, ib); break;
                        case 2: c = BinBool(170u, ia, ib); break;
                        case 3: c = BinBool(179u, ia, ib); break;
                        case 4: c = BinBool(173u, ia, ib); break;
                        case 5: c = BinBool(171u, ia, ib); break;
                        case 6: c = BinBool(175u, ia, ib); break;
                        case 7: c = c_true_; break;
                        default: c = c_false_; break;
                    }
                } else {
                    switch (raw & 7u) {
                        case 1: c = BinBool(176u, a, b); break;
                        case 2: c = BinBool(170u, a, b); break;
                        case 3: c = BinBool(178u, a, b); break;
                        case 4: c = BinBool(172u, a, b); break;
                        case 5: c = BinBool(171u, a, b); break;
                        case 6: c = BinBool(174u, a, b); break;
                        case 7: c = c_true_; break;
                        default: c = c_false_; break;
                    }
                }
                Emit(sec_fn_, 62u, {var_vcc_bit, c});
            } else if (enc == Rdna2Encoding::VOP3) {
                const bool isf = (e->data_kind == DK::Float);
                const uint32_t dv = dst_var(inst.dst);
                if (raw == 0x360u) {
                    const uint32_t val = read_op(inst.src[0]);
                    const uint32_t tgt = BinU32(199u, read_op(inst.src[1]), ConstU32(lane_mask));
                    const uint32_t res = (is_compute && var_lanes_ != 0u) ? EmitLaneXchg(val, tgt) : val;
                    Emit(sec_fn_, 62u, {dv, res});
                } else if (raw == 0x361u) {
                    const uint32_t val = read_op(inst.src[0]);
                    const uint32_t tgt = BinU32(199u, read_op(inst.src[1]), ConstU32(lane_mask));
                    const uint32_t match = BinBool(170u, lane_id_, tgt);
                    Emit(sec_fn_, 62u, {dv, SelU32(match, val, load_var(dv))});
                } else if (raw == 0x377u || raw == 0x378u) {
                    const uint32_t val = read_op(inst.src[0]);
                    const uint32_t s1 = read_op(inst.src[1]);
                    const uint32_t s2 = read_op(inst.src[2]);
                    const uint32_t rl = BinU32(199u, lane_id_, ConstU32(15u));
                    uint32_t rb = BinU32(199u, lane_id_, ConstU32(~15u));
                    if (raw == 0x378u) rb = BinU32(198u, rb, ConstU32(16u));
                    const uint32_t w = SelU32(BinBool(176u, rl, ConstU32(8u)), s1, s2);
                    const uint32_t sh = BinU32(196u, BinU32(199u, rl, ConstU32(7u)), ConstU32(2u));
                    const uint32_t sel = BinU32(199u, BinU32(194u, w, sh), ConstU32(15u));
                    const uint32_t tgt = BinU32(199u, BinU32(197u, rb, sel), ConstU32(lane_mask));
                    const uint32_t res = (is_compute && var_lanes_ != 0u) ? EmitLaneXchg(val, tgt) : val;
                    Emit(sec_fn_, 62u, {dv, res});
                } else {
                    const uint32_t a = read_op(inst.src[0], isf);
                    const uint32_t b = (inst.src_count >= 2u) ? read_op(inst.src[1], isf) : c0;
                    const uint32_t c = (inst.src_count >= 3u) ? read_op(inst.src[2], isf) : c0;
                    const uint32_t fa = Bitcast(t_f32_, a), fb = Bitcast(t_f32_, b), fc = Bitcast(t_f32_, c);
                    const uint32_t ia = Bitcast(t_i32_, a), ib = Bitcast(t_i32_, b), ic = Bitcast(t_i32_, c);
                    uint32_t r = a;
                    switch (raw) {
                        case 0x140: { uint32_t p = SelF32(BinBool(166u, BinBool(180u, fa, ConstF32(0.0f)), BinBool(180u, fb, ConstF32(0.0f))), ConstF32(0.0f), BinF32(133u, fa, fb)); r = Bitcast(t_u32_, BinF32(129u, p, fc)); break; }
                        case 0x141: r = Bitcast(t_u32_, BinF32(129u, BinF32(133u, fa, fb), fc)); break;
                        case 0x142: r = BinU32(128u, BinU32(132u, SextN(a, 24u), SextN(b, 24u)), c); break;
                        case 0x143: r = BinU32(128u, BinU32(132u, BinU32(199u, a, ConstU32(0xFFFFFFu)), BinU32(199u, b, ConstU32(0xFFFFFFu))), c); break;
                        case 0x144: case 0x145: case 0x146: {
                            const uint32_t ax = Glsl1(t_f32_, 4u, fa), ay = Glsl1(t_f32_, 4u, fb), az = Glsl1(t_f32_, 4u, fc);
                            const uint32_t z_dom = BinBool(167u, BinBool(190u, az, ax), BinBool(190u, az, ay));
                            const uint32_t y_dom = BinBool(190u, ay, ax);
                            const uint32_t neg_a = BinBool(184u, fa, ConstF32(0.0f));
                            const uint32_t neg_b = BinBool(184u, fb, ConstF32(0.0f));
                            const uint32_t neg_c = BinBool(184u, fc, ConstF32(0.0f));
                            const uint32_t nfa = AllocId(); Emit(sec_fn_, 127u /*OpFNegate*/, {t_f32_, nfa, fa});
                            const uint32_t nfb = AllocId(); Emit(sec_fn_, 127u, {t_f32_, nfb, fb});
                            const uint32_t nfc = AllocId(); Emit(sec_fn_, 127u, {t_f32_, nfc, fc});
                            uint32_t res_f = fa;
                            if (raw == 0x144u) {
                                res_f = SelF32(z_dom, SelF32(neg_c, ConstF32(5.0f), ConstF32(4.0f)),
                                        SelF32(y_dom, SelF32(neg_b, ConstF32(3.0f), ConstF32(2.0f)),
                                                      SelF32(neg_a, ConstF32(1.0f), ConstF32(0.0f))));
                            } else if (raw == 0x145u) {
                                res_f = SelF32(z_dom, SelF32(neg_c, nfa, fa), SelF32(y_dom, fa, SelF32(neg_a, fc, nfc)));
                            } else {
                                res_f = SelF32(z_dom, nfb, SelF32(y_dom, SelF32(neg_b, nfc, fc), nfb));
                            }
                            r = Bitcast(t_u32_, res_f);
                            break;
                        }
                        case 0x147: r = Bitcast(t_u32_, BinF32(133u, ConstF32(2.0f), Glsl2(t_f32_, 40u, Glsl2(t_f32_, 40u, Glsl1(t_f32_, 4u, fa), Glsl1(t_f32_, 4u, fb)), Glsl1(t_f32_, 4u, fc)))); break;
                        case 0x148: r = EmitBfeU32(a, b, c); break;
                        case 0x149: r = EmitBfeI32(a, b, c); break;
                        case 0x14A: { uint32_t na = AllocId(); Emit(sec_fn_, 200u, {t_u32_, na, a}); r = BinU32(197u, BinU32(199u, a, b), BinU32(199u, na, c)); break; }
                        case 0x14B: r = Bitcast(t_u32_, Glsl3(t_f32_, 50u, fa, fb, fc)); break;
                        case 0x162: {
                            const uint32_t fma_v = Glsl3(t_f32_, 50u, fa, fb, fc);
                            const uint32_t exp_c = BinU32(199u, BinU32(194u, c, ConstU32(23u)), ConstU32(0xFFu));
                            const uint32_t hi_exp = BinBool(172u /*UGreaterThan*/, exp_c, ConstU32(127u));
                            const uint32_t scale = SelF32(hi_exp, ConstF32(18446744073709551616.0f), ConstF32(5.421010862427522e-20f));
                            const uint32_t scaled = BinF32(133u /*FMul*/, fma_v, scale);
                            r = Bitcast(t_u32_, SelF32(load_bool(var_vcc_bit), scaled, fma_v));
                            break;
                        }
                        case 0x14C: { uint32_t sh = BinU32(196u, BinU32(199u, c, ConstU32(3u)), ConstU32(3u)); uint32_t z = BinBool(170u, sh, c0); r = SelU32(z, b, BinU32(197u, BinU32(194u, b, sh), BinU32(196u, a, BinU32(199u, BinU32(130u, ConstU32(32u), sh), ConstU32(31u))))); break; }
                        case 0x14E: {
                            uint32_t acc = c0;
                            for (uint32_t bi = 0u; bi < 4u; ++bi) {
                                uint32_t sh = ConstU32(bi * 8u);
                                uint32_t ba = BinU32(199u, BinU32(194u, a, sh), ConstU32(0xFFu));
                                uint32_t bb = BinU32(199u, BinU32(194u, b, sh), ConstU32(0xFFu));
                                uint32_t bc = BinU32(199u, BinU32(194u, c, sh), ConstU32(1u));
                                uint32_t avg = BinU32(199u, BinU32(194u, BinU32(128u, BinU32(128u, ba, bb), bc), ConstU32(1u)), ConstU32(0xFFu));
                                acc = BinU32(197u, acc, BinU32(196u, avg, sh));
                            }
                            r = acc;
                            break;
                        }
                        case 0x14F: { uint32_t sh = BinU32(199u, c, ConstU32(31u)); uint32_t z = BinBool(170u, sh, c0); r = SelU32(z, b, BinU32(197u, BinU32(194u, b, sh), BinU32(196u, a, BinU32(199u, BinU32(130u, ConstU32(32u), sh), ConstU32(31u))))); break; }
                        case 0x150: r = Bitcast(t_u32_, SelF32(BinBool(186u, fa, ConstF32(0.0f)), BinF32(129u, BinF32(133u, fa, fb), fc), fc)); break;
                        case 0x151: r = Bitcast(t_u32_, Glsl2(t_f32_, 37u, Glsl2(t_f32_, 37u, fa, fb), fc)); break;
                        case 0x152: r = Bitcast(t_u32_, Glsl2(t_i32_, 39u, Glsl2(t_i32_, 39u, ia, ib), ic)); break;
                        case 0x153: r = Glsl2(t_u32_, 38u, Glsl2(t_u32_, 38u, a, b), c); break;
                        case 0x154: r = Bitcast(t_u32_, Glsl2(t_f32_, 40u, Glsl2(t_f32_, 40u, fa, fb), fc)); break;
                        case 0x155: r = Bitcast(t_u32_, Glsl2(t_i32_, 42u, Glsl2(t_i32_, 42u, ia, ib), ic)); break;
                        case 0x156: r = Glsl2(t_u32_, 41u, Glsl2(t_u32_, 41u, a, b), c); break;
                        case 0x157: r = Bitcast(t_u32_, Glsl2(t_f32_, 40u, Glsl2(t_f32_, 37u, fa, fb), Glsl2(t_f32_, 37u, Glsl2(t_f32_, 40u, fa, fb), fc))); break;
                        case 0x158: r = Bitcast(t_u32_, Glsl2(t_i32_, 42u, Glsl2(t_i32_, 39u, ia, ib), Glsl2(t_i32_, 39u, Glsl2(t_i32_, 42u, ia, ib), ic))); break;
                        case 0x159: r = Glsl2(t_u32_, 41u, Glsl2(t_u32_, 38u, a, b), Glsl2(t_u32_, 38u, Glsl2(t_u32_, 41u, a, b), c)); break;
                        case 0x15A: case 0x15B: {
                            uint32_t sum = c0;
                            for (uint32_t bi = 0u; bi < 4u; ++bi) {
                                uint32_t sh = ConstU32(bi * 8u);
                                uint32_t ba = BinU32(199u, BinU32(194u, a, sh), ConstU32(0xFFu));
                                uint32_t bb = BinU32(199u, BinU32(194u, b, sh), ConstU32(0xFFu));
                                uint32_t df = SelU32(BinBool(174u, ba, bb), BinU32(130u, ba, bb), BinU32(130u, bb, ba));
                                sum = BinU32(128u, sum, df);
                            }
                            r = BinU32(128u, c, (raw == 0x15Bu) ? BinU32(196u, sum, ConstU32(16u)) : sum);
                            break;
                        }
                        case 0x15C: {
                            uint32_t a0 = BinU32(199u, a, ConstU32(0xFFFFu)), b0 = BinU32(199u, b, ConstU32(0xFFFFu));
                            uint32_t a1 = BinU32(194u, a, ConstU32(16u)), b1 = BinU32(194u, b, ConstU32(16u));
                            uint32_t d0 = SelU32(BinBool(174u, a0, b0), BinU32(130u, a0, b0), BinU32(130u, b0, a0));
                            uint32_t d1 = SelU32(BinBool(174u, a1, b1), BinU32(130u, a1, b1), BinU32(130u, b1, a1));
                            r = BinU32(128u, c, BinU32(128u, d0, d1));
                            break;
                        }
                        case 0x15D: r = BinU32(128u, c, SelU32(BinBool(174u, a, b), BinU32(130u, a, b), BinU32(130u, b, a))); break;
                        case 0x15E: {
                            const uint32_t bv = AllocId(); Emit(sec_fn_, 109u, {t_u32_, bv, Glsl3(t_f32_, 43u, fa, ConstF32(0.0f), ConstF32(255.0f))});
                            const uint32_t sh = BinU32(196u, BinU32(199u, b, ConstU32(3u)), ConstU32(3u));
                            const uint32_t clrm = AllocId(); Emit(sec_fn_, 200u, {t_u32_, clrm, BinU32(196u, ConstU32(0xFFu), sh)});
                            r = BinU32(197u, BinU32(199u, c, clrm), BinU32(196u, BinU32(199u, bv, ConstU32(0xFFu)), sh));
                            break;
                        }
                        case 0x15F: {
                            const uint32_t sn = BinU32(199u, BinU32(198u, b, c), ConstU32(0x80000000u));
                            const uint32_t mag = BinU32(199u, a, ConstU32(0x7FFFFFFFu));
                            r = BinU32(197u, sn, mag);
                            break;
                        }
                        case 0x161: r = Bitcast(t_u32_, Glsl2(t_f32_, 53u /*Ldexp*/, fa, ib)); break;
                        case 0x164: case 0x168: {
                            uint32_t sum = c0;
                            for (uint32_t bi = 0u; bi < 4u; ++bi) {
                                uint32_t sh = ConstU32(bi * 8u);
                                uint32_t ba = BinU32(199u, BinU32(194u, a, sh), ConstU32(0xFFu));
                                uint32_t bb = BinU32(199u, BinU32(194u, b, sh), ConstU32(0xFFu));
                                uint32_t df = SelU32(BinBool(174u, ba, bb), BinU32(130u, ba, bb), BinU32(130u, bb, ba));
                                sum = BinU32(128u, sum, SelU32(BinBool(171u, bb, c0), df, c0));
                            }
                            r = BinU32(128u, c, sum);
                            break;
                        }
                        case 0x169: r = BinU32(132u, a, b); break;
                        case 0x16A: r = EmitMulHiU32(a, b); break;
                        case 0x16B: r = EmitMulHiI32(a, b); break;
                        case 0x178: { uint32_t pc = AllocId(); Emit(sec_fn_, 205u, {t_u32_, pc, a}); r = BinU32(128u, pc, b); break; }
                        case 0x179: r = Bitcast(t_u32_, SelF32(BinBool(166u, BinBool(180u, fa, ConstF32(0.0f)), BinBool(180u, fb, ConstF32(0.0f))), fc, Glsl3(t_f32_, 50u, fa, fb, fc))); break;
                        case 0x303: r = BinU32(199u, BinU32(128u, a, b), ConstU32(0xFFFFu)); break;
                        case 0x304: r = BinU32(199u, BinU32(130u, a, b), ConstU32(0xFFFFu)); break;
                        case 0x305: {
                            const uint32_t lt32 = BinBool(176u, lane_id_, ConstU32(32u));
                            const uint32_t sh = SelU32(lt32, lane_id_, c0);
                            const uint32_t m = SelU32(lt32, BinU32(130u, BinU32(196u, ConstU32(1u), sh), ConstU32(1u)), ConstU32(0xFFFFFFFFu));
                            const uint32_t pc = AllocId();
                            Emit(sec_fn_, 205u, {t_u32_, pc, BinU32(199u, a, m)});
                            r = BinU32(128u, pc, b);
                            break;
                        }
                        case 0x306: {
                            const uint32_t ge32 = BinBool(174u, lane_id_, ConstU32(32u));
                            const uint32_t sh = SelU32(ge32, BinU32(130u, lane_id_, ConstU32(32u)), c0);
                            const uint32_t m = SelU32(ge32, BinU32(130u, BinU32(196u, ConstU32(1u), sh), ConstU32(1u)), c0);
                            const uint32_t pc = AllocId();
                            Emit(sec_fn_, 205u, {t_u32_, pc, BinU32(199u, a, m)});
                            r = BinU32(128u, pc, b);
                            break;
                        }
                        case 0x307: r = BinU32(199u, BinU32(128u, SextN(a, 16u), SextN(b, 16u)), ConstU32(0xFFFFu)); break;
                        case 0x308: r = BinU32(199u, BinU32(130u, SextN(a, 16u), SextN(b, 16u)), ConstU32(0xFFFFu)); break;
                        case 0x309: r = Glsl2(t_u32_, 38u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFFu))); break;
                        case 0x30A: r = Glsl2(t_u32_, 41u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFFu))); break;
                        case 0x30B: r = BinU32(199u, Bitcast(t_u32_, Glsl2(t_i32_, 39u, Bitcast(t_i32_, SextN(a, 16u)), Bitcast(t_i32_, SextN(b, 16u)))), ConstU32(0xFFFFu)); break;
                        case 0x30C: r = BinU32(199u, Bitcast(t_u32_, Glsl2(t_i32_, 42u, Bitcast(t_i32_, SextN(a, 16u)), Bitcast(t_i32_, SextN(b, 16u)))), ConstU32(0xFFFFu)); break;
                        case 0x30D: r = BinU32(128u, a, b); Emit(sec_fn_, 62u, {var_vcc_bit, BinBool(176u, r, a)}); break;
                        case 0x30E: r = BinU32(130u, a, b); Emit(sec_fn_, 62u, {var_vcc_bit, BinBool(172u, b, a)}); break;
                        case 0x30F: r = BinU32(130u, b, a); Emit(sec_fn_, 62u, {var_vcc_bit, BinBool(172u, a, b)}); break;
                        case 0x311: r = BinU32(197u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(196u, BinU32(199u, b, ConstU32(0xFFFFu)), ConstU32(16u))); break;
                        // Algebraically identical mod 2^16 without redundant input masks that trigger
                        // AMD RDNA2 (RX 6750 XT) outer-mask elision in both D3D12 and Vulkan (#2).
                        case 0x312: r = BinU32(199u, BinU32(132u, a, b), ConstU32(0xFFFFu)); break;
                        case 0x313: r = BinU32(199u, Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, SextN(b, 16u)), BinU32(199u, a, ConstU32(15u)))), ConstU32(0xFFFFu)); break;
                        case 0x314: r = BinU32(199u, BinU32(196u, BinU32(199u, b, ConstU32(0xFFFFu)), BinU32(199u, a, ConstU32(15u))), ConstU32(0xFFFFu)); break;
                        case 0x315: r = BinU32(194u, BinU32(199u, b, ConstU32(0xFFFFu)), BinU32(199u, a, ConstU32(15u))); break;
                        case 0x340: r = BinU32(199u, BinU32(128u, BinU32(132u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFFu))), BinU32(199u, c, ConstU32(0xFFFFu))), ConstU32(0xFFFFu)); break;
                        case 0x341: r = BinU32(199u, BinU32(128u, BinU32(132u, SextN(a, 16u), SextN(b, 16u)), SextN(c, 16u)), ConstU32(0xFFFFu)); break;
                        case 0x344: {
                            // v_perm_b32
                            uint32_t acc = c0;
                            for (uint32_t bi = 0u; bi < 4u; ++bi) {
                                const uint32_t sel = BinU32(199u, BinU32(194u, c, ConstU32(bi * 8u)), ConstU32(0xFFu));
                                const uint32_t lo_s1 = BinU32(199u, BinU32(194u, b, BinU32(196u, BinU32(199u, sel, ConstU32(3u)), ConstU32(3u))), ConstU32(0xFFu));
                                const uint32_t lo_s0 = BinU32(199u, BinU32(194u, a, BinU32(196u, BinU32(199u, sel, ConstU32(3u)), ConstU32(3u))), ConstU32(0xFFu));
                                uint32_t bv = SelU32(BinBool(176u, sel, ConstU32(4u)), lo_s1,
                                              SelU32(BinBool(176u, sel, ConstU32(8u)), lo_s0,
                                              SelU32(BinBool(170u, sel, ConstU32(12u)), c0, ConstU32(0xFFu))));
                                acc = BinU32(197u, acc, BinU32(196u, bv, ConstU32(bi * 8u)));
                            }
                            r = acc;
                            break;
                        }
                        case 0x34B: r = PackF16Lo(Glsl3(t_f32_, 50u, UnpackF16Lo(a), UnpackF16Lo(b), UnpackF16Lo(c))); break;
                        case 0x34C: { const uint32_t ha = Bitcast(t_u32_, UnpackF16Lo(a)), hb = Bitcast(t_u32_, UnpackF16Lo(b)), hc = Bitcast(t_u32_, UnpackF16Lo(c)); r = PackF16Lo(Bitcast(t_f32_, BinU32(197u, BinU32(199u, BinU32(198u, hb, hc), ConstU32(0x80000000u)), BinU32(199u, ha, ConstU32(0x7FFFFFFFu))))); break; }
                        case 0x34D: r = PackF16Lo(Glsl2(t_f32_, 37u, Glsl2(t_f32_, 37u, UnpackF16Lo(a), UnpackF16Lo(b)), UnpackF16Lo(c))); break;
                        case 0x34E: r = BinU32(199u, Bitcast(t_u32_, Glsl2(t_i32_, 39u, Glsl2(t_i32_, 39u, Bitcast(t_i32_, SextN(a, 16u)), Bitcast(t_i32_, SextN(b, 16u))), Bitcast(t_i32_, SextN(c, 16u)))), ConstU32(0xFFFFu)); break;
                        case 0x34F: r = Glsl2(t_u32_, 38u, Glsl2(t_u32_, 38u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFFu))), BinU32(199u, c, ConstU32(0xFFFFu))); break;
                        case 0x350: r = PackF16Lo(Glsl2(t_f32_, 40u, Glsl2(t_f32_, 40u, UnpackF16Lo(a), UnpackF16Lo(b)), UnpackF16Lo(c))); break;
                        case 0x351: r = BinU32(199u, Bitcast(t_u32_, Glsl2(t_i32_, 42u, Glsl2(t_i32_, 42u, Bitcast(t_i32_, SextN(a, 16u)), Bitcast(t_i32_, SextN(b, 16u))), Bitcast(t_i32_, SextN(c, 16u)))), ConstU32(0xFFFFu)); break;
                        case 0x352: r = Glsl2(t_u32_, 41u, Glsl2(t_u32_, 41u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFFu))), BinU32(199u, c, ConstU32(0xFFFFu))); break;
                        case 0x353: { uint32_t ha = UnpackF16Lo(a), hb = UnpackF16Lo(b), hc = UnpackF16Lo(c); r = PackF16Lo(Glsl2(t_f32_, 40u, Glsl2(t_f32_, 37u, ha, hb), Glsl2(t_f32_, 37u, Glsl2(t_f32_, 40u, ha, hb), hc))); break; }
                        case 0x354: { uint32_t s_a = Bitcast(t_i32_, SextN(a, 16u)), s_b = Bitcast(t_i32_, SextN(b, 16u)), s_c = Bitcast(t_i32_, SextN(c, 16u)); r = BinU32(199u, Bitcast(t_u32_, Glsl2(t_i32_, 42u, Glsl2(t_i32_, 39u, s_a, s_b), Glsl2(t_i32_, 39u, Glsl2(t_i32_, 42u, s_a, s_b), s_c))), ConstU32(0xFFFFu)); break; }
                        case 0x355: { uint32_t ua = BinU32(199u, a, ConstU32(0xFFFFu)), ub = BinU32(199u, b, ConstU32(0xFFFFu)), uc = BinU32(199u, c, ConstU32(0xFFFFu)); r = Glsl2(t_u32_, 41u, Glsl2(t_u32_, 38u, ua, ub), Glsl2(t_u32_, 38u, Glsl2(t_u32_, 41u, ua, ub), uc)); break; }
                        case 0x368: {
                            const uint32_t q0 = AllocId(); Emit(sec_fn_, 110u, {t_i32_, q0, Glsl1(t_f32_, 2u, BinF32(133u, Glsl3(t_f32_, 43u, fa, ConstF32(-1.0f), ConstF32(1.0f)), ConstF32(32767.0f)))});
                            const uint32_t q1 = AllocId(); Emit(sec_fn_, 110u, {t_i32_, q1, Glsl1(t_f32_, 2u, BinF32(133u, Glsl3(t_f32_, 43u, fb, ConstF32(-1.0f), ConstF32(1.0f)), ConstF32(32767.0f)))});
                            r = BinU32(197u, BinU32(199u, Bitcast(t_u32_, q0), ConstU32(0xFFFFu)), BinU32(196u, BinU32(199u, Bitcast(t_u32_, q1), ConstU32(0xFFFFu)), ConstU32(16u)));
                            break;
                        }
                        case 0x369: {
                            const uint32_t q0 = AllocId(); Emit(sec_fn_, 109u, {t_u32_, q0, Glsl1(t_f32_, 2u, BinF32(133u, Glsl3(t_f32_, 43u, fa, ConstF32(0.0f), ConstF32(1.0f)), ConstF32(65535.0f)))});
                            const uint32_t q1 = AllocId(); Emit(sec_fn_, 109u, {t_u32_, q1, Glsl1(t_f32_, 2u, BinF32(133u, Glsl3(t_f32_, 43u, fb, ConstF32(0.0f), ConstF32(1.0f)), ConstF32(65535.0f)))});
                            r = BinU32(197u, BinU32(199u, q0, ConstU32(0xFFFFu)), BinU32(196u, BinU32(199u, q1, ConstU32(0xFFFFu)), ConstU32(16u)));
                            break;
                        }
                        case 0x36A: r = BinU32(197u, Glsl2(t_u32_, 38u, a, ConstU32(0xFFFFu)), BinU32(196u, Glsl2(t_u32_, 38u, b, ConstU32(0xFFFFu)), ConstU32(16u))); break;
                        case 0x36B: {
                            const uint32_t q0 = Bitcast(t_u32_, Glsl3(t_i32_, 45u, ia, ConstI32(-32768), ConstI32(32767)));
                            const uint32_t q1 = Bitcast(t_u32_, Glsl3(t_i32_, 45u, ib, ConstI32(-32768), ConstI32(32767)));
                            r = BinU32(197u, BinU32(199u, q0, ConstU32(0xFFFFu)), BinU32(196u, BinU32(199u, q1, ConstU32(0xFFFFu)), ConstU32(16u)));
                            break;
                        }
                        case 0x36C: r = PackF16Pair(fa, fb); break;
                        case 0x36D: r = BinU32(128u, BinU32(128u, a, b), c); break;
                        case 0x36E: r = BinU32(197u, BinU32(196u, a, BinU32(199u, b, ConstU32(31u))), c); break;
                        case 0x36F: r = BinU32(197u, BinU32(199u, a, b), c); break;
                        case 0x370: r = BinU32(197u, BinU32(197u, a, b), c); break;
                        case 0x371: r = BinU32(128u, BinU32(198u, a, b), c); break;
                        case 0x372: r = BinU32(128u, BinU32(196u, a, BinU32(199u, b, ConstU32(31u))), c); break;
                        case 0x373: r = Bitcast(t_u32_, BinF32(129u, BinF32(129u, BinF32(133u, UnpackF16Lo(a), UnpackF16Lo(b)), BinF32(133u, UnpackF16Hi(a), UnpackF16Hi(b))), fc)); break;
                        case 0x374: r = BinU32(196u, BinU32(128u, a, b), BinU32(199u, c, ConstU32(31u))); break;
                        default: break;
                    }
                    Emit(sec_fn_, 62u, {dv, r});
                }
            } else if (enc == Rdna2Encoding::VOP3P) {
                const uint32_t dv = dst_var(inst.dst);
                const uint32_t old_d = load_var(dv);
                const uint32_t a = read_op(inst.src[0]);
                const uint32_t b = (inst.src_count >= 2u) ? read_op(inst.src[1]) : c0;
                const uint32_t c = (inst.src_count >= 3u) ? read_op(inst.src[2]) : c0;
                auto half_u = [&](uint32_t v, bool hi) -> uint32_t {
                    return hi ? BinU32(194u, v, ConstU32(16u)) : BinU32(199u, v, ConstU32(0xFFFFu));
                };
                const uint32_t a0 = half_u(a, (inst.vop3p_op_sel & 1u) != 0u);
                const uint32_t a1 = half_u(a, (inst.vop3p_op_sel_hi & 1u) != 0u);
                const uint32_t b0 = half_u(b, (inst.vop3p_op_sel & 2u) != 0u);
                const uint32_t b1 = half_u(b, (inst.vop3p_op_sel_hi & 2u) != 0u);
                const uint32_t c0h = half_u(c, (inst.vop3p_op_sel & 4u) != 0u);
                const uint32_t c1h = half_u(c, (inst.vop3p_op_sel_hi & 4u) != 0u);
                auto f16_spv = [&](uint32_t h, uint8_t nmask, uint8_t bit) -> uint32_t {
                    uint32_t f = UnpackF16Lo(h);
                    if ((nmask >> bit) & 1u) {
                        uint32_t nf = AllocId(); Emit(sec_fn_, 127u /*OpFNegate*/, {t_f32_, nf, f});
                        f = nf;
                    }
                    return f;
                };
                auto pack2 = [&](uint32_t lo, uint32_t hi) -> uint32_t {
                    return BinU32(197u, BinU32(199u, lo, ConstU32(0xFFFFu)), BinU32(196u, BinU32(199u, hi, ConstU32(0xFFFFu)), ConstU32(16u)));
                };
                uint32_t r = c0;
                switch (raw) {
                    case 0x00: r = pack2(BinU32(128u, BinU32(132u, SextN(a0, 16u), SextN(b0, 16u)), SextN(c0h, 16u)), BinU32(128u, BinU32(132u, SextN(a1, 16u), SextN(b1, 16u)), SextN(c1h, 16u))); break;
                    case 0x01: r = pack2(BinU32(132u, a0, b0), BinU32(132u, a1, b1)); break;
                    case 0x02: case 0x0A: r = pack2(BinU32(128u, a0, b0), BinU32(128u, a1, b1)); break;
                    case 0x03: case 0x0B: r = pack2(BinU32(130u, a0, b0), BinU32(130u, a1, b1)); break;
                    case 0x04: r = pack2(BinU32(196u, b0, BinU32(199u, a0, ConstU32(15u))), BinU32(196u, b1, BinU32(199u, a1, ConstU32(15u)))); break;
                    case 0x05: r = pack2(BinU32(194u, b0, BinU32(199u, a0, ConstU32(15u))), BinU32(194u, b1, BinU32(199u, a1, ConstU32(15u)))); break;
                    case 0x06: r = pack2(Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, SextN(b0, 16u)), BinU32(199u, a0, ConstU32(15u)))), Bitcast(t_u32_, BinI32(195u, Bitcast(t_i32_, SextN(b1, 16u)), BinU32(199u, a1, ConstU32(15u))))); break;
                    case 0x07: r = pack2(Bitcast(t_u32_, Glsl2(t_i32_, 42u, Bitcast(t_i32_, SextN(a0, 16u)), Bitcast(t_i32_, SextN(b0, 16u)))), Bitcast(t_u32_, Glsl2(t_i32_, 42u, Bitcast(t_i32_, SextN(a1, 16u)), Bitcast(t_i32_, SextN(b1, 16u))))); break;
                    case 0x08: r = pack2(Bitcast(t_u32_, Glsl2(t_i32_, 39u, Bitcast(t_i32_, SextN(a0, 16u)), Bitcast(t_i32_, SextN(b0, 16u)))), Bitcast(t_u32_, Glsl2(t_i32_, 39u, Bitcast(t_i32_, SextN(a1, 16u)), Bitcast(t_i32_, SextN(b1, 16u))))); break;
                    case 0x09: r = pack2(BinU32(128u, BinU32(132u, a0, b0), c0h), BinU32(128u, BinU32(132u, a1, b1), c1h)); break;
                    case 0x0C: r = pack2(Glsl2(t_u32_, 41u, a0, b0), Glsl2(t_u32_, 41u, a1, b1)); break;
                    case 0x0D: r = pack2(Glsl2(t_u32_, 38u, a0, b0), Glsl2(t_u32_, 38u, a1, b1)); break;
                    case 0x0E: r = PackF16Pair(Glsl3(t_f32_, 50u, f16_spv(a0, inst.vop3p_neg_lo, 0), f16_spv(b0, inst.vop3p_neg_lo, 1), f16_spv(c0h, inst.vop3p_neg_lo, 2)), Glsl3(t_f32_, 50u, f16_spv(a1, inst.vop3p_neg_hi, 0), f16_spv(b1, inst.vop3p_neg_hi, 1), f16_spv(c1h, inst.vop3p_neg_hi, 2))); break;
                    case 0x0F: r = PackF16Pair(BinF32(129u, f16_spv(a0, inst.vop3p_neg_lo, 0), f16_spv(b0, inst.vop3p_neg_lo, 1)), BinF32(129u, f16_spv(a1, inst.vop3p_neg_hi, 0), f16_spv(b1, inst.vop3p_neg_hi, 1))); break;
                    case 0x10: r = PackF16Pair(BinF32(133u, f16_spv(a0, inst.vop3p_neg_lo, 0), f16_spv(b0, inst.vop3p_neg_lo, 1)), BinF32(133u, f16_spv(a1, inst.vop3p_neg_hi, 0), f16_spv(b1, inst.vop3p_neg_hi, 1))); break;
                    case 0x11: r = PackF16Pair(Glsl2(t_f32_, 37u, f16_spv(a0, inst.vop3p_neg_lo, 0), f16_spv(b0, inst.vop3p_neg_lo, 1)), Glsl2(t_f32_, 37u, f16_spv(a1, inst.vop3p_neg_hi, 0), f16_spv(b1, inst.vop3p_neg_hi, 1))); break;
                    case 0x12: r = PackF16Pair(Glsl2(t_f32_, 40u, f16_spv(a0, inst.vop3p_neg_lo, 0), f16_spv(b0, inst.vop3p_neg_lo, 1)), Glsl2(t_f32_, 40u, f16_spv(a1, inst.vop3p_neg_hi, 0), f16_spv(b1, inst.vop3p_neg_hi, 1))); break;
                    case 0x13: r = BinU32(128u, c, BinU32(128u, BinU32(132u, SextN(a, 16u), SextN(b, 16u)), BinU32(132u, SextN(BinU32(194u, a, ConstU32(16u)), 16u), SextN(BinU32(194u, b, ConstU32(16u)), 16u)))); break;
                    case 0x14: r = BinU32(128u, c, BinU32(128u, BinU32(132u, BinU32(199u, a, ConstU32(0xFFFFu)), BinU32(199u, b, ConstU32(0xFFFFu))), BinU32(132u, BinU32(194u, a, ConstU32(16u)), BinU32(194u, b, ConstU32(16u))))); break;
                    case 0x15: case 0x16: {
                        uint32_t acc = c;
                        for (uint32_t k = 0u; k < 4u; ++k) {
                            uint32_t sa = BinU32(194u, a, ConstU32(k * 8u));
                            uint32_t sb = BinU32(194u, b, ConstU32(k * 8u));
                            uint32_t ka = (raw == 0x15u) ? SextN(sa, 8u) : BinU32(199u, sa, ConstU32(0xFFu));
                            uint32_t kb = (raw == 0x15u) ? SextN(sb, 8u) : BinU32(199u, sb, ConstU32(0xFFu));
                            acc = BinU32(128u, acc, BinU32(132u, ka, kb));
                        }
                        r = acc;
                        break;
                    }
                    case 0x17: case 0x18: {
                        uint32_t acc = c;
                        for (uint32_t k = 0u; k < 8u; ++k) {
                            uint32_t sa = BinU32(194u, a, ConstU32(k * 4u));
                            uint32_t sb = BinU32(194u, b, ConstU32(k * 4u));
                            uint32_t ka = (raw == 0x17u) ? SextN(sa, 4u) : BinU32(199u, sa, ConstU32(0xFu));
                            uint32_t kb = (raw == 0x17u) ? SextN(sb, 4u) : BinU32(199u, sb, ConstU32(0xFu));
                            acc = BinU32(128u, acc, BinU32(132u, ka, kb));
                        }
                        r = acc;
                        break;
                    }
                    case 0x20: case 0x21: case 0x22: {
                        auto mix_spv = [&](uint32_t v, uint8_t bit) -> uint32_t {
                            const bool is_f16 = ((inst.vop3p_op_sel_hi >> bit) & 1u) != 0u;
                            const bool hi = ((inst.vop3p_op_sel >> bit) & 1u) != 0u;
                            uint32_t f = is_f16 ? (hi ? UnpackF16Hi(v) : UnpackF16Lo(v)) : Bitcast(t_f32_, v);
                            if ((inst.vop3p_neg_lo >> bit) & 1u) f = Glsl1(t_f32_, 4u /*FAbs*/, f);
                            if ((inst.vop3p_neg_hi >> bit) & 1u) { uint32_t nf = AllocId(); Emit(sec_fn_, 127u, {t_f32_, nf, f}); f = nf; }
                            return f;
                        };
                        const uint32_t mf = Glsl3(t_f32_, 50u /*Fma*/, mix_spv(a, 0u), mix_spv(b, 1u), mix_spv(c, 2u));
                        if (raw == 0x20u) {
                            r = Bitcast(t_u32_, mf);
                        } else if (raw == 0x21u) {
                            r = BinU32(197u, BinU32(199u, old_d, ConstU32(0xFFFF0000u)), PackF16Lo(mf));
                        } else {
                            r = BinU32(197u, BinU32(199u, old_d, ConstU32(0x0000FFFFu)), BinU32(196u, PackF16Lo(mf), ConstU32(16u)));
                        }
                        break;
                    }
                    default: r = a; break;
                }
                Emit(sec_fn_, 62u, {dv, r});
            } else if (enc == Rdna2Encoding::VINTRP) {
                Emit(sec_fn_, 62u, {dst_var(inst.dst), read_op(inst.src[0])});
            }
        }

        // Stage output writes for VS/PS
        if (container.stage == ShaderStage::Vertex && var_pv != 0u) {
            const uint32_t fx = Bitcast(t_f32_, load_var(v_vars[0]));
            const uint32_t fy = Bitcast(t_f32_, load_var(v_vars[1]));
            const uint32_t fz = Bitcast(t_f32_, load_var(v_vars[2]));
            const uint32_t v4 = AllocId();
            Emit(sec_fn_, 80u, {t_v4f32_, v4, fx, fy, fz, ConstF32(1.0f)});
            const uint32_t ptr = AllocId();
            Emit(sec_fn_, 65u, {t_ptr_out_v4f32, ptr, var_pv, ConstI32(0)});
            Emit(sec_fn_, 62u, {ptr, v4});
        } else if (container.stage == ShaderStage::Pixel && var_mrt0 != 0u) {
            const uint32_t fx = Bitcast(t_f32_, load_var(v_vars[0]));
            const uint32_t fy = Bitcast(t_f32_, load_var(v_vars[1]));
            const uint32_t fz = Bitcast(t_f32_, load_var(v_vars[2]));
            const uint32_t v4 = AllocId();
            Emit(sec_fn_, 80u, {t_v4f32_, v4, fx, fy, fz, ConstF32(1.0f)});
            Emit(sec_fn_, 62u, {var_mrt0, v4});
        }

        Emit(sec_fn_, 253u /*OpReturn*/, {});
        Emit(sec_fn_, 56u /*OpFunctionEnd*/, {});

        // Assemble final binary
        out_spv.push_back(kMagic);
        out_spv.push_back(kVer13);
        out_spv.push_back(kGen);
        out_spv.push_back(next_id_);
        out_spv.push_back(0u);
        auto append = [&](const std::vector<uint32_t>& sec) {
            out_spv.insert(out_spv.end(), sec.begin(), sec.end());
        };
        append(sec_caps_);
        append(sec_ext_);
        append(sec_mem_);
        append(sec_entry_);
        append(sec_exec_);
        append(sec_decor_);
        append(sec_types_);
        append(sec_globals_);
        append(sec_fn_);
        return Status::Ok();
    }

private:
    uint32_t next_id_ = 1u;
    uint32_t ext_glsl_ = 0u;
    uint32_t t_void_ = 0u, t_bool_ = 0u, t_u32_ = 0u, t_i32_ = 0u, t_f32_ = 0u;
    uint32_t t_v2f32_ = 0u, t_v3u32_ = 0u, t_v4f32_ = 0u, t_u32_pair_ = 0u, t_i32_pair_ = 0u;
    uint32_t t_fn_void_ = 0u, t_ptr_fn_u32_ = 0u, t_ptr_fn_bool_ = 0u, t_ptr_wg_u32_ = 0u;
    uint32_t c_false_ = 0u, c_true_ = 0u;
    uint32_t var_lanes_ = 0u, lane_id_ = 0u, gw_base_id_ = 0u;

    std::unordered_map<uint32_t, uint32_t> const_u32_;
    std::unordered_map<uint32_t, uint32_t> const_i32_;
    std::unordered_map<uint32_t, uint32_t> const_f32_;

    std::vector<uint32_t> sec_caps_, sec_ext_, sec_mem_, sec_entry_, sec_exec_, sec_decor_, sec_types_, sec_globals_, sec_fn_;
};

}  // namespace

Status LiftScalarVectorShader(
    const Xs2ShaderContainer& container,
    const std::vector<SvDecodedInst>& insts,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options) {
    out_result = Rdna2LiftResult{};
    out_result.container = container;
    if (insts.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "LiftScalarVectorShader: empty instruction stream");
    }

    out_result.instructions.reserve(insts.size());
    for (const auto& sv : insts) {
        out_result.instructions.push_back(sv.base);
    }

    const SvShaderUsage usage = AnalyzeSvUsage(insts);

    if (options.emit_spirv) {
        SvSpirvBuilder spv_builder;
        if (auto st = spv_builder.Build(container, insts, usage, out_result.spirv_words); !st.ok()) {
            return st;
        }
        if (auto st = ValidateSpirvBinary(out_result.spirv_words); !st.ok()) {
            return st;
        }
        if (options.validate_spirv_external) {
            if (auto st = ValidateSpirvExternal(out_result.spirv_words, out_result.spirv_val_diagnostics,
                                                options.spirv_val_path, options.spirv_target_env);
                !st.ok()) {
                return st;
            }
        }
    }

    if (options.emit_hlsl) {
        if (auto st = EmitSvHlsl(container, insts, usage, out_result.hlsl_source, out_result.hlsl_target, options.use_sm65);
            !st.ok()) {
            return st;
        }
        if (!options.target_profile_override.empty()) {
            out_result.hlsl_target = options.target_profile_override;
        }
        if (options.compile_dxil_with_dxc) {
            if (auto st = CompileHlslWithDxc(out_result.hlsl_source, out_result.hlsl_target, "main",
                                             out_result.pc_dxil, out_result.dxc_diagnostics, options.dxc_path);
                !st.ok()) {
                return st;
            }
            if (options.resign_dxil && !VerifyDxbcContainerHash(out_result.pc_dxil.data(), out_result.pc_dxil.size())) {
                if (auto rs = ResignDxilContainer(out_result.pc_dxil); !rs.ok()) {
                    return rs;
                }
            }
            out_result.pc_dxil_hash = HashFnv1a64(out_result.pc_dxil.data(), out_result.pc_dxil.size());
        }
    }

    return Status::Ok();
}

}  // namespace openscarlett::rdna2_sv
