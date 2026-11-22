#include "rdna2_isa_mem_img.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace openscarlett::rdna2_mem {

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

uint64_t DoubleToBits(double d) noexcept {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

double BitsToDouble(uint64_t bits) noexcept {
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
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
    if (new_exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
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

int32_t SignExtend(uint32_t val, unsigned bits) noexcept {
    const uint32_t shift = 32u - bits;
    return static_cast<int32_t>(val << shift) >> shift;
}

uint8_t ReadByte(const uint8_t* buf, size_t size, uint32_t addr) noexcept {
    if (!buf || static_cast<size_t>(addr) >= size) return 0u;
    return buf[addr];
}

void WriteByte(uint8_t* buf, size_t size, uint32_t addr, uint8_t val) noexcept {
    if (!buf || static_cast<size_t>(addr) >= size) return;
    buf[addr] = val;
}

uint16_t ReadU16(const uint8_t* buf, size_t size, uint32_t addr) noexcept {
    const uint16_t b0 = ReadByte(buf, size, addr);
    const uint16_t b1 = ReadByte(buf, size, addr + 1u);
    return static_cast<uint16_t>(b0 | (b1 << 8));
}

void WriteU16(uint8_t* buf, size_t size, uint32_t addr, uint16_t val) noexcept {
    WriteByte(buf, size, addr, static_cast<uint8_t>(val & 0xFFu));
    WriteByte(buf, size, addr + 1u, static_cast<uint8_t>((val >> 8) & 0xFFu));
}

uint32_t ReadU32(const uint8_t* buf, size_t size, uint32_t addr) noexcept {
    const uint32_t b0 = ReadByte(buf, size, addr);
    const uint32_t b1 = ReadByte(buf, size, addr + 1u);
    const uint32_t b2 = ReadByte(buf, size, addr + 2u);
    const uint32_t b3 = ReadByte(buf, size, addr + 3u);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void WriteU32(uint8_t* buf, size_t size, uint32_t addr, uint32_t val) noexcept {
    WriteByte(buf, size, addr + 0u, static_cast<uint8_t>(val & 0xFFu));
    WriteByte(buf, size, addr + 1u, static_cast<uint8_t>((val >> 8) & 0xFFu));
    WriteByte(buf, size, addr + 2u, static_cast<uint8_t>((val >> 16) & 0xFFu));
    WriteByte(buf, size, addr + 3u, static_cast<uint8_t>((val >> 24) & 0xFFu));
}

uint64_t ReadU64(const uint8_t* buf, size_t size, uint32_t addr) noexcept {
    const uint64_t lo = ReadU32(buf, size, addr);
    const uint64_t hi = ReadU32(buf, size, addr + 4u);
    return lo | (hi << 32);
}

void WriteU64(uint8_t* buf, size_t size, uint32_t addr, uint64_t val) noexcept {
    WriteU32(buf, size, addr, static_cast<uint32_t>(val & 0xFFFFFFFFull));
    WriteU32(buf, size, addr + 4u, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFFull));
}

uint64_t HashBytesFnv1a(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Complete GFX10.3 Memory / Image / LDS / FLAT / EXP Opcode Table Builder
// ---------------------------------------------------------------------------
MemImgOpcodeEntry MakeEntry(
    Rdna2Encoding enc,
    uint16_t raw_op,
    uint8_t flat_seg,
    const char* name,
    uint8_t data_dwords,
    bool is_load,
    bool is_store,
    bool is_atomic,
    bool has_rtn,
    bool is_d16,
    bool is_sample,
    bool is_gather,
    bool is_bvh,
    MemImgSemanticClass sem) {
    return MemImgOpcodeEntry{
        enc,
        raw_op,
        flat_seg,
        name,
        data_dwords,
        is_load,
        is_store,
        is_atomic,
        has_rtn,
        is_d16,
        is_sample,
        is_gather,
        is_bvh,
        static_cast<uint16_t>(sem),
        true,
        true,
        true,
        true,
    };
}

std::vector<MemImgOpcodeEntry> BuildOpcodeTable() {
    std::vector<MemImgOpcodeEntry> t;
    t.reserve(460);
    using E = Rdna2Encoding;
    using S = MemImgSemanticClass;

    // -----------------------------------------------------------------------
    // 1. MUBUF (encoding 0x38) — 77 standard GFX10.3 mnemonics
    // -----------------------------------------------------------------------
    // Format loads & stores
    t.push_back(MakeEntry(E::MUBUF, 0x00, 0, "buffer_load_format_x", 1, true, false, false, true, false, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x01, 0, "buffer_load_format_xy", 2, true, false, false, true, false, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x02, 0, "buffer_load_format_xyz", 3, true, false, false, true, false, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x03, 0, "buffer_load_format_xyzw", 4, true, false, false, true, false, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x04, 0, "buffer_store_format_x", 1, false, true, false, false, false, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x05, 0, "buffer_store_format_xy", 2, false, true, false, false, false, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x06, 0, "buffer_store_format_xyz", 3, false, true, false, false, false, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x07, 0, "buffer_store_format_xyzw", 4, false, true, false, false, false, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x08, 0, "buffer_load_format_d16_x", 1, true, false, false, true, true, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x09, 0, "buffer_load_format_d16_xy", 1, true, false, false, true, true, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x0A, 0, "buffer_load_format_d16_xyz", 2, true, false, false, true, true, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x0B, 0, "buffer_load_format_d16_xyzw", 2, true, false, false, true, true, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x0C, 0, "buffer_store_format_d16_x", 1, false, true, false, false, true, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x0D, 0, "buffer_store_format_d16_xy", 1, false, true, false, false, true, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x0E, 0, "buffer_store_format_d16_xyz", 2, false, true, false, false, true, false, false, false, S::BufferStoreFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x0F, 0, "buffer_store_format_d16_xyzw", 2, false, true, false, false, true, false, false, false, S::BufferStoreFormat));
    // Raw loads & stores
    t.push_back(MakeEntry(E::MUBUF, 0x10, 0, "buffer_load_ubyte", 1, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x11, 0, "buffer_load_sbyte", 1, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x12, 0, "buffer_load_ushort", 1, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x13, 0, "buffer_load_sshort", 1, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x14, 0, "buffer_load_dword", 1, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x15, 0, "buffer_load_dwordx2", 2, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x16, 0, "buffer_load_dwordx3", 3, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x17, 0, "buffer_load_dwordx4", 4, true, false, false, true, false, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x18, 0, "buffer_store_byte", 1, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x19, 0, "buffer_store_byte_d16_hi", 1, false, true, false, false, true, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x1A, 0, "buffer_store_short", 1, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x1B, 0, "buffer_store_short_d16_hi", 1, false, true, false, false, true, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x1C, 0, "buffer_store_dword", 1, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x1D, 0, "buffer_store_dwordx2", 2, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x1E, 0, "buffer_store_dwordx3", 3, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x1F, 0, "buffer_store_dwordx4", 4, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    // Sub-dword D16 loads & D16_HI format ops
    t.push_back(MakeEntry(E::MUBUF, 0x20, 0, "buffer_load_ubyte_d16", 1, true, false, false, true, true, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x21, 0, "buffer_load_ubyte_d16_hi", 1, true, false, false, true, true, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x22, 0, "buffer_load_sbyte_d16", 1, true, false, false, true, true, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x23, 0, "buffer_load_sbyte_d16_hi", 1, true, false, false, true, true, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x24, 0, "buffer_load_short_d16", 1, true, false, false, true, true, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x25, 0, "buffer_load_short_d16_hi", 1, true, false, false, true, true, false, false, false, S::BufferLoadRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x26, 0, "buffer_load_format_d16_hi_x", 1, true, false, false, true, true, false, false, false, S::BufferLoadFormat));
    t.push_back(MakeEntry(E::MUBUF, 0x27, 0, "buffer_store_format_d16_hi_x", 1, false, true, false, false, true, false, false, false, S::BufferStoreFormat));
    // 32-bit Buffer Atomics
    t.push_back(MakeEntry(E::MUBUF, 0x30, 0, "buffer_atomic_swap", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x31, 0, "buffer_atomic_cmpswap", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x32, 0, "buffer_atomic_add", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x33, 0, "buffer_atomic_sub", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x34, 0, "buffer_atomic_csub", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x35, 0, "buffer_atomic_smin", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x36, 0, "buffer_atomic_umin", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x37, 0, "buffer_atomic_smax", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x38, 0, "buffer_atomic_umax", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x39, 0, "buffer_atomic_and", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x3A, 0, "buffer_atomic_or", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x3B, 0, "buffer_atomic_xor", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x3C, 0, "buffer_atomic_inc", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x3D, 0, "buffer_atomic_dec", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x3E, 0, "buffer_atomic_fcmpswap", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x3F, 0, "buffer_atomic_fmin", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x40, 0, "buffer_atomic_fmax", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x4D, 0, "buffer_atomic_add_f32", 1, false, false, true, true, false, false, false, false, S::BufferAtomic));
    // 64-bit (_x2) Buffer Atomics
    t.push_back(MakeEntry(E::MUBUF, 0x50, 0, "buffer_atomic_swap_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x51, 0, "buffer_atomic_cmpswap_x2", 4, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x52, 0, "buffer_atomic_add_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x53, 0, "buffer_atomic_sub_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x55, 0, "buffer_atomic_smin_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x56, 0, "buffer_atomic_umin_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x57, 0, "buffer_atomic_smax_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x58, 0, "buffer_atomic_umax_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x59, 0, "buffer_atomic_and_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x5A, 0, "buffer_atomic_or_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x5B, 0, "buffer_atomic_xor_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x5C, 0, "buffer_atomic_inc_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x5D, 0, "buffer_atomic_dec_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x5E, 0, "buffer_atomic_fcmpswap_x2", 4, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x5F, 0, "buffer_atomic_fmin_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    t.push_back(MakeEntry(E::MUBUF, 0x60, 0, "buffer_atomic_fmax_x2", 2, false, false, true, true, false, false, false, false, S::BufferAtomic));
    // LDS store & cache invalidates
    t.push_back(MakeEntry(E::MUBUF, 0x70, 0, "buffer_store_lds_dword", 1, false, true, false, false, false, false, false, false, S::BufferStoreRaw));
    t.push_back(MakeEntry(E::MUBUF, 0x71, 0, "buffer_gl0_inv", 0, false, false, false, false, false, false, false, false, S::BufferCache));
    t.push_back(MakeEntry(E::MUBUF, 0x72, 0, "buffer_gl1_inv", 0, false, false, false, false, false, false, false, false, S::BufferCache));

    // -----------------------------------------------------------------------
    // 2. MTBUF (encoding 0x3A) — 18 standard GFX10.3 mnemonics
    // -----------------------------------------------------------------------
    t.push_back(MakeEntry(E::MTBUF, 0x00, 0, "tbuffer_load_format_x", 1, true, false, false, true, false, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x01, 0, "tbuffer_load_format_xy", 2, true, false, false, true, false, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x02, 0, "tbuffer_load_format_xyz", 3, true, false, false, true, false, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x03, 0, "tbuffer_load_format_xyzw", 4, true, false, false, true, false, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x04, 0, "tbuffer_store_format_x", 1, false, true, false, false, false, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x05, 0, "tbuffer_store_format_xy", 2, false, true, false, false, false, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x06, 0, "tbuffer_store_format_xyz", 3, false, true, false, false, false, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x07, 0, "tbuffer_store_format_xyzw", 4, false, true, false, false, false, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x08, 0, "tbuffer_load_format_d16_x", 1, true, false, false, true, true, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x09, 0, "tbuffer_load_format_d16_xy", 1, true, false, false, true, true, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x0A, 0, "tbuffer_load_format_d16_xyz", 2, true, false, false, true, true, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x0B, 0, "tbuffer_load_format_d16_xyzw", 2, true, false, false, true, true, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x0C, 0, "tbuffer_store_format_d16_x", 1, false, true, false, false, true, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x0D, 0, "tbuffer_store_format_d16_xy", 1, false, true, false, false, true, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x0E, 0, "tbuffer_store_format_d16_xyz", 2, false, true, false, false, true, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x0F, 0, "tbuffer_store_format_d16_xyzw", 2, false, true, false, false, true, false, false, false, S::TbufferStoreFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x10, 0, "tbuffer_load_format_d16_hi_x", 1, true, false, false, true, true, false, false, false, S::TbufferLoadFormat));
    t.push_back(MakeEntry(E::MTBUF, 0x11, 0, "tbuffer_store_format_d16_hi_x", 1, false, true, false, false, true, false, false, false, S::TbufferStoreFormat));

    // -----------------------------------------------------------------------
    // 3. MIMG (encoding 0x3C) — 95 standard GFX10.3 mnemonics
    // -----------------------------------------------------------------------
    // Loads, stores, resinfo, lod
    t.push_back(MakeEntry(E::MIMG, 0x00, 0, "image_load", 4, true, false, false, true, false, false, false, false, S::ImageLoad));
    t.push_back(MakeEntry(E::MIMG, 0x01, 0, "image_load_mip", 4, true, false, false, true, false, false, false, false, S::ImageLoad));
    t.push_back(MakeEntry(E::MIMG, 0x02, 0, "image_load_pck", 4, true, false, false, true, false, false, false, false, S::ImageLoad));
    t.push_back(MakeEntry(E::MIMG, 0x03, 0, "image_load_pck_sgn", 4, true, false, false, true, false, false, false, false, S::ImageLoad));
    t.push_back(MakeEntry(E::MIMG, 0x04, 0, "image_load_mip_pck", 4, true, false, false, true, false, false, false, false, S::ImageLoad));
    t.push_back(MakeEntry(E::MIMG, 0x05, 0, "image_load_mip_pck_sgn", 4, true, false, false, true, false, false, false, false, S::ImageLoad));
    t.push_back(MakeEntry(E::MIMG, 0x08, 0, "image_store", 4, false, true, false, false, false, false, false, false, S::ImageStore));
    t.push_back(MakeEntry(E::MIMG, 0x09, 0, "image_store_mip", 4, false, true, false, false, false, false, false, false, S::ImageStore));
    t.push_back(MakeEntry(E::MIMG, 0x0A, 0, "image_store_pck", 4, false, true, false, false, false, false, false, false, S::ImageStore));
    t.push_back(MakeEntry(E::MIMG, 0x0B, 0, "image_store_mip_pck", 4, false, true, false, false, false, false, false, false, S::ImageStore));
    t.push_back(MakeEntry(E::MIMG, 0x0E, 0, "image_get_resinfo", 4, true, false, false, true, false, false, false, false, S::ImageQuery));
    t.push_back(MakeEntry(E::MIMG, 0x60, 0, "image_get_lod", 2, true, false, false, true, false, false, false, false, S::ImageQuery));
    // Image atomics (16)
    t.push_back(MakeEntry(E::MIMG, 0x10, 0, "image_atomic_swap", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x11, 0, "image_atomic_cmpswap", 2, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x12, 0, "image_atomic_add", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x13, 0, "image_atomic_sub", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x14, 0, "image_atomic_smin", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x15, 0, "image_atomic_umin", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x16, 0, "image_atomic_smax", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x17, 0, "image_atomic_umax", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x18, 0, "image_atomic_and", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x19, 0, "image_atomic_or", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x1A, 0, "image_atomic_xor", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x1B, 0, "image_atomic_inc", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x1C, 0, "image_atomic_dec", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x1D, 0, "image_atomic_fcmpswap", 2, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x1E, 0, "image_atomic_fmin", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    t.push_back(MakeEntry(E::MIMG, 0x1F, 0, "image_atomic_fmax", 1, false, false, true, true, false, false, false, false, S::ImageAtomic));
    // Image sample matrix (0x20..0x3F = 32 variants)
    static constexpr const char* kSampleNames[32] = {
        "image_sample", "image_sample_cl", "image_sample_d", "image_sample_d_cl",
        "image_sample_l", "image_sample_b", "image_sample_b_cl", "image_sample_lz",
        "image_sample_c", "image_sample_c_cl", "image_sample_c_d", "image_sample_c_d_cl",
        "image_sample_c_l", "image_sample_c_b", "image_sample_c_b_cl", "image_sample_c_lz",
        "image_sample_o", "image_sample_cl_o", "image_sample_d_o", "image_sample_d_cl_o",
        "image_sample_l_o", "image_sample_b_o", "image_sample_b_cl_o", "image_sample_lz_o",
        "image_sample_c_o", "image_sample_c_cl_o", "image_sample_c_d_o", "image_sample_c_d_cl_o",
        "image_sample_c_l_o", "image_sample_c_b_o", "image_sample_c_b_cl_o", "image_sample_c_lz_o",
    };
    for (uint16_t i = 0; i < 32u; ++i) {
        t.push_back(MakeEntry(E::MIMG, static_cast<uint16_t>(0x20u + i), 0, kSampleNames[i], 4,
                              true, false, false, true, false, true, false, false, S::ImageSample));
    }
    // Image sample G16 gradient variants (8)
    t.push_back(MakeEntry(E::MIMG, 0xA2, 0, "image_sample_d_g16", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xA3, 0, "image_sample_d_cl_g16", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xAA, 0, "image_sample_c_d_g16", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xAB, 0, "image_sample_c_d_cl_g16", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xB2, 0, "image_sample_d_g16_o", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xB3, 0, "image_sample_d_cl_g16_o", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xBA, 0, "image_sample_c_d_g16_o", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    t.push_back(MakeEntry(E::MIMG, 0xBB, 0, "image_sample_c_d_cl_g16_o", 4, true, false, false, true, false, true, false, false, S::ImageSample));
    // Image gather4 matrix (24 variants)
    struct GatherSpec { uint16_t op; const char* name; };
    static constexpr GatherSpec kGatherSpecs[24] = {
        {0x40, "image_gather4"},        {0x41, "image_gather4_cl"},
        {0x44, "image_gather4_l"},      {0x45, "image_gather4_b"},
        {0x46, "image_gather4_b_cl"},   {0x47, "image_gather4_lz"},
        {0x48, "image_gather4_c"},      {0x49, "image_gather4_c_cl"},
        {0x4C, "image_gather4_c_l"},    {0x4D, "image_gather4_c_b"},
        {0x4E, "image_gather4_c_b_cl"}, {0x4F, "image_gather4_c_lz"},
        {0x50, "image_gather4_o"},      {0x51, "image_gather4_cl_o"},
        {0x54, "image_gather4_l_o"},    {0x55, "image_gather4_b_o"},
        {0x56, "image_gather4_b_cl_o"}, {0x57, "image_gather4_lz_o"},
        {0x58, "image_gather4_c_o"},    {0x59, "image_gather4_c_cl_o"},
        {0x5C, "image_gather4_c_l_o"},  {0x5D, "image_gather4_c_b_o"},
        {0x5E, "image_gather4_c_b_cl_o"}, {0x5F, "image_gather4_c_lz_o"},
    };
    for (const auto& gs : kGatherSpecs) {
        t.push_back(MakeEntry(E::MIMG, gs.op, 0, gs.name, 4,
                              true, false, false, true, false, false, true, false, S::ImageGather4));
    }
    // Hardware Ray Tracing BVH & MSAA Load
    t.push_back(MakeEntry(E::MIMG, 0xE0, 0, "image_msaa_load", 4, true, false, false, true, false, false, false, false, S::ImageMsaaLoad));
    t.push_back(MakeEntry(E::MIMG, 0xE6, 0, "image_bvh_intersect_ray", 4, true, false, false, true, false, false, false, true, S::ImageBvhIntersect));
    t.push_back(MakeEntry(E::MIMG, 0xE7, 0, "image_bvh64_intersect_ray", 4, true, false, false, true, false, false, false, true, S::ImageBvhIntersect));

    // -----------------------------------------------------------------------
    // 4. DS (LDS / Shared Memory, encoding 0x36) — 124 standard GFX10.3 mnemonics
    // -----------------------------------------------------------------------
    // 32-bit non-_rtn atomics & stores (0x00..0x1F)
    t.push_back(MakeEntry(E::DS, 0x00, 0, "ds_add_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x01, 0, "ds_sub_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x02, 0, "ds_rsub_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x03, 0, "ds_inc_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x04, 0, "ds_dec_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x05, 0, "ds_min_i32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x06, 0, "ds_max_i32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x07, 0, "ds_min_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x08, 0, "ds_max_u32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x09, 0, "ds_and_b32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x0A, 0, "ds_or_b32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x0B, 0, "ds_xor_b32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x0C, 0, "ds_mskor_b32", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x0D, 0, "ds_write_b32", 1, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x0E, 0, "ds_write2_b32", 2, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x0F, 0, "ds_write2st64_b32", 2, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x10, 0, "ds_cmpst_b32", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x11, 0, "ds_cmpst_f32", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x12, 0, "ds_min_f32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x13, 0, "ds_max_f32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x14, 0, "ds_nop", 0, false, false, false, false, false, false, false, false, S::DsNop));
    t.push_back(MakeEntry(E::DS, 0x15, 0, "ds_add_f32", 1, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x18, 0, "ds_gws_sema_release_all", 0, false, false, false, false, false, false, false, false, S::DsGws));
    t.push_back(MakeEntry(E::DS, 0x19, 0, "ds_gws_init", 0, false, false, false, false, false, false, false, false, S::DsGws));
    t.push_back(MakeEntry(E::DS, 0x1A, 0, "ds_gws_sema_v", 0, false, false, false, false, false, false, false, false, S::DsGws));
    t.push_back(MakeEntry(E::DS, 0x1B, 0, "ds_gws_sema_br", 0, false, false, false, false, false, false, false, false, S::DsGws));
    t.push_back(MakeEntry(E::DS, 0x1C, 0, "ds_gws_sema_p", 0, false, false, false, false, false, false, false, false, S::DsGws));
    t.push_back(MakeEntry(E::DS, 0x1D, 0, "ds_gws_barrier", 0, false, false, false, false, false, false, false, false, S::DsGws));
    t.push_back(MakeEntry(E::DS, 0x1E, 0, "ds_write_b8", 1, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x1F, 0, "ds_write_b16", 1, false, true, false, false, false, false, false, false, S::DsStore));
    // 32-bit _rtn atomics, reads, swizzle, append/consume (0x20..0x3F)
    t.push_back(MakeEntry(E::DS, 0x20, 0, "ds_add_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x21, 0, "ds_sub_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x22, 0, "ds_rsub_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x23, 0, "ds_inc_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x24, 0, "ds_dec_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x25, 0, "ds_min_rtn_i32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x26, 0, "ds_max_rtn_i32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x27, 0, "ds_min_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x28, 0, "ds_max_rtn_u32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x29, 0, "ds_and_rtn_b32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x2A, 0, "ds_or_rtn_b32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x2B, 0, "ds_xor_rtn_b32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x2C, 0, "ds_mskor_rtn_b32", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x2D, 0, "ds_wrxchg_rtn_b32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x2E, 0, "ds_wrxchg2_rtn_b32", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x2F, 0, "ds_wrxchg2st64_rtn_b32", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x30, 0, "ds_cmpst_rtn_b32", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x31, 0, "ds_cmpst_rtn_f32", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x32, 0, "ds_min_rtn_f32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x33, 0, "ds_max_rtn_f32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x35, 0, "ds_swizzle_b32", 1, false, false, false, true, false, false, false, false, S::DsCrossLane));
    t.push_back(MakeEntry(E::DS, 0x36, 0, "ds_read_b32", 1, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x37, 0, "ds_read2_b32", 2, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x38, 0, "ds_read2st64_b32", 2, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x39, 0, "ds_read_i8", 1, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x3A, 0, "ds_read_u8", 1, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x3B, 0, "ds_read_i16", 1, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x3C, 0, "ds_read_u16", 1, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x3D, 0, "ds_consume", 1, false, false, true, true, false, false, false, false, S::DsOrdered));
    t.push_back(MakeEntry(E::DS, 0x3E, 0, "ds_append", 1, false, false, true, true, false, false, false, false, S::DsOrdered));
    t.push_back(MakeEntry(E::DS, 0x3F, 0, "ds_ordered_count", 1, false, false, true, true, false, false, false, false, S::DsOrdered));
    // 64-bit non-_rtn atomics & stores (0x40..0x55)
    t.push_back(MakeEntry(E::DS, 0x40, 0, "ds_add_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x41, 0, "ds_sub_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x42, 0, "ds_rsub_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x43, 0, "ds_inc_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x44, 0, "ds_dec_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x45, 0, "ds_min_i64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x46, 0, "ds_max_i64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x47, 0, "ds_min_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x48, 0, "ds_max_u64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x49, 0, "ds_and_b64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x4A, 0, "ds_or_b64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x4B, 0, "ds_xor_b64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x4C, 0, "ds_mskor_b64", 4, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x4D, 0, "ds_write_b64", 2, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x4E, 0, "ds_write2_b64", 4, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x4F, 0, "ds_write2st64_b64", 4, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0x50, 0, "ds_cmpst_b64", 4, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x51, 0, "ds_cmpst_f64", 4, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x52, 0, "ds_min_f64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x53, 0, "ds_max_f64", 2, false, false, true, false, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x55, 0, "ds_add_rtn_f32", 1, false, false, true, true, false, false, false, false, S::DsAtomic));
    // 64-bit _rtn atomics & reads (0x60..0x7E)
    t.push_back(MakeEntry(E::DS, 0x60, 0, "ds_add_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x61, 0, "ds_sub_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x62, 0, "ds_rsub_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x63, 0, "ds_inc_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x64, 0, "ds_dec_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x65, 0, "ds_min_rtn_i64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x66, 0, "ds_max_rtn_i64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x67, 0, "ds_min_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x68, 0, "ds_max_rtn_u64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x69, 0, "ds_and_rtn_b64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x6A, 0, "ds_or_rtn_b64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x6B, 0, "ds_xor_rtn_b64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x6C, 0, "ds_mskor_rtn_b64", 4, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x6D, 0, "ds_wrxchg_rtn_b64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x6E, 0, "ds_wrxchg2_rtn_b64", 4, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x6F, 0, "ds_wrxchg2st64_rtn_b64", 4, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x70, 0, "ds_cmpst_rtn_b64", 4, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x71, 0, "ds_cmpst_rtn_f64", 4, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x72, 0, "ds_min_rtn_f64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x73, 0, "ds_max_rtn_f64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0x76, 0, "ds_read_b64", 2, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x77, 0, "ds_read2_b64", 4, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x78, 0, "ds_read2st64_b64", 4, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0x7E, 0, "ds_condxchg32_rtn_b64", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    // Extended DS: src2, d16, gs_reg, permute, bpermute, b96, b128
    t.push_back(MakeEntry(E::DS, 0x8D, 0, "ds_write_src2_b32", 1, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0xA0, 0, "ds_write_b8_d16_hi", 1, false, true, false, false, true, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0xA1, 0, "ds_write_b16_d16_hi", 1, false, true, false, false, true, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0xA2, 0, "ds_read_u8_d16", 1, true, false, false, true, true, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xA3, 0, "ds_read_u8_d16_hi", 1, true, false, false, true, true, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xA4, 0, "ds_read_i8_d16", 1, true, false, false, true, true, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xA5, 0, "ds_read_i8_d16_hi", 1, true, false, false, true, true, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xA6, 0, "ds_read_u16_d16", 1, true, false, false, true, true, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xA7, 0, "ds_read_u16_d16_hi", 1, true, false, false, true, true, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xB0, 0, "ds_add_gs_reg_rtn", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0xB1, 0, "ds_sub_gs_reg_rtn", 2, false, false, true, true, false, false, false, false, S::DsAtomic));
    t.push_back(MakeEntry(E::DS, 0xB2, 0, "ds_permute_b32", 1, false, false, false, true, false, false, false, false, S::DsCrossLane));
    t.push_back(MakeEntry(E::DS, 0xB3, 0, "ds_bpermute_b32", 1, false, false, false, true, false, false, false, false, S::DsCrossLane));
    t.push_back(MakeEntry(E::DS, 0xCD, 0, "ds_write_src2_b64", 2, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0xDE, 0, "ds_write_b96", 3, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0xDF, 0, "ds_write_b128", 4, false, true, false, false, false, false, false, false, S::DsStore));
    t.push_back(MakeEntry(E::DS, 0xFE, 0, "ds_read_b96", 3, true, false, false, true, false, false, false, false, S::DsLoad));
    t.push_back(MakeEntry(E::DS, 0xFF, 0, "ds_read_b128", 4, true, false, false, true, false, false, false, false, S::DsLoad));

    // -----------------------------------------------------------------------
    // 5. FLAT / GLOBAL / SCRATCH (encoding 0x37) — 134 standard GFX10.3 mnemonics
    // -----------------------------------------------------------------------
    struct FlatMemSpec {
        uint16_t op;
        const char* flat_name;
        const char* scratch_name;
        const char* global_name;
        uint8_t dwords;
        bool is_load;
        bool is_store;
        bool is_d16;
    };
    static constexpr FlatMemSpec kFlatMemSpecs[22] = {
        {0x08, "flat_load_ubyte", "scratch_load_ubyte", "global_load_ubyte", 1, true, false, false},
        {0x09, "flat_load_sbyte", "scratch_load_sbyte", "global_load_sbyte", 1, true, false, false},
        {0x0A, "flat_load_ushort", "scratch_load_ushort", "global_load_ushort", 1, true, false, false},
        {0x0B, "flat_load_sshort", "scratch_load_sshort", "global_load_sshort", 1, true, false, false},
        {0x0C, "flat_load_dword", "scratch_load_dword", "global_load_dword", 1, true, false, false},
        {0x0D, "flat_load_dwordx2", "scratch_load_dwordx2", "global_load_dwordx2", 2, true, false, false},
        {0x0E, "flat_load_dwordx4", "scratch_load_dwordx4", "global_load_dwordx4", 4, true, false, false},
        {0x0F, "flat_load_dwordx3", "scratch_load_dwordx3", "global_load_dwordx3", 3, true, false, false},
        {0x18, "flat_store_byte", "scratch_store_byte", "global_store_byte", 1, false, true, false},
        {0x19, "flat_store_byte_d16_hi", "scratch_store_byte_d16_hi", "global_store_byte_d16_hi", 1, false, true, true},
        {0x1A, "flat_store_short", "scratch_store_short", "global_store_short", 1, false, true, false},
        {0x1B, "flat_store_short_d16_hi", "scratch_store_short_d16_hi", "global_store_short_d16_hi", 1, false, true, true},
        {0x1C, "flat_store_dword", "scratch_store_dword", "global_store_dword", 1, false, true, false},
        {0x1D, "flat_store_dwordx2", "scratch_store_dwordx2", "global_store_dwordx2", 2, false, true, false},
        {0x1E, "flat_store_dwordx4", "scratch_store_dwordx4", "global_store_dwordx4", 4, false, true, false},
        {0x1F, "flat_store_dwordx3", "scratch_store_dwordx3", "global_store_dwordx3", 3, false, true, false},
        {0x20, "flat_load_ubyte_d16", "scratch_load_ubyte_d16", "global_load_ubyte_d16", 1, true, false, true},
        {0x21, "flat_load_ubyte_d16_hi", "scratch_load_ubyte_d16_hi", "global_load_ubyte_d16_hi", 1, true, false, true},
        {0x22, "flat_load_sbyte_d16", "scratch_load_sbyte_d16", "global_load_sbyte_d16", 1, true, false, true},
        {0x23, "flat_load_sbyte_d16_hi", "scratch_load_sbyte_d16_hi", "global_load_sbyte_d16_hi", 1, true, false, true},
        {0x24, "flat_load_short_d16", "scratch_load_short_d16", "global_load_short_d16", 1, true, false, true},
        {0x25, "flat_load_short_d16_hi", "scratch_load_short_d16_hi", "global_load_short_d16_hi", 1, true, false, true},
    };
    for (const auto& s : kFlatMemSpecs) {
        t.push_back(MakeEntry(E::FLAT, s.op, 0, s.flat_name, s.dwords, s.is_load, s.is_store, false, s.is_load, s.is_d16, false, false, false,
                              s.is_load ? S::FlatLoad : S::FlatStore));
        t.push_back(MakeEntry(E::FLAT, s.op, 1, s.scratch_name, s.dwords, s.is_load, s.is_store, false, s.is_load, s.is_d16, false, false, false,
                              s.is_load ? S::ScratchLoad : S::ScratchStore));
        t.push_back(MakeEntry(E::FLAT, s.op, 2, s.global_name, s.dwords, s.is_load, s.is_store, false, s.is_load, s.is_d16, false, false, false,
                              s.is_load ? S::GlobalLoad : S::GlobalStore));
    }

    struct FlatAtomicSpec {
        uint16_t op;
        const char* flat_name;
        const char* global_name;
        uint8_t dwords;
    };
    static constexpr FlatAtomicSpec kFlatAtomicSpecs[34] = {
        {0x30, "flat_atomic_swap", "global_atomic_swap", 1},
        {0x31, "flat_atomic_cmpswap", "global_atomic_cmpswap", 2},
        {0x32, "flat_atomic_add", "global_atomic_add", 1},
        {0x33, "flat_atomic_sub", "global_atomic_sub", 1},
        {0x34, "flat_atomic_csub", "global_atomic_csub", 1},
        {0x35, "flat_atomic_smin", "global_atomic_smin", 1},
        {0x36, "flat_atomic_umin", "global_atomic_umin", 1},
        {0x37, "flat_atomic_smax", "global_atomic_smax", 1},
        {0x38, "flat_atomic_umax", "global_atomic_umax", 1},
        {0x39, "flat_atomic_and", "global_atomic_and", 1},
        {0x3A, "flat_atomic_or", "global_atomic_or", 1},
        {0x3B, "flat_atomic_xor", "global_atomic_xor", 1},
        {0x3C, "flat_atomic_inc", "global_atomic_inc", 1},
        {0x3D, "flat_atomic_dec", "global_atomic_dec", 1},
        {0x3E, "flat_atomic_fcmpswap", "global_atomic_fcmpswap", 2},
        {0x3F, "flat_atomic_fmin", "global_atomic_fmin", 1},
        {0x40, "flat_atomic_fmax", "global_atomic_fmax", 1},
        {0x4D, "flat_atomic_add_f32", "global_atomic_add_f32", 1},
        {0x50, "flat_atomic_swap_x2", "global_atomic_swap_x2", 2},
        {0x51, "flat_atomic_cmpswap_x2", "global_atomic_cmpswap_x2", 4},
        {0x52, "flat_atomic_add_x2", "global_atomic_add_x2", 2},
        {0x53, "flat_atomic_sub_x2", "global_atomic_sub_x2", 2},
        {0x55, "flat_atomic_smin_x2", "global_atomic_smin_x2", 2},
        {0x56, "flat_atomic_umin_x2", "global_atomic_umin_x2", 2},
        {0x57, "flat_atomic_smax_x2", "global_atomic_smax_x2", 2},
        {0x58, "flat_atomic_umax_x2", "global_atomic_umax_x2", 2},
        {0x59, "flat_atomic_and_x2", "global_atomic_and_x2", 2},
        {0x5A, "flat_atomic_or_x2", "global_atomic_or_x2", 2},
        {0x5B, "flat_atomic_xor_x2", "global_atomic_xor_x2", 2},
        {0x5C, "flat_atomic_inc_x2", "global_atomic_inc_x2", 2},
        {0x5D, "flat_atomic_dec_x2", "global_atomic_dec_x2", 2},
        {0x5E, "flat_atomic_fcmpswap_x2", "global_atomic_fcmpswap_x2", 4},
        {0x5F, "flat_atomic_fmin_x2", "global_atomic_fmin_x2", 2},
        {0x60, "flat_atomic_fmax_x2", "global_atomic_fmax_x2", 2},
    };
    for (const auto& a : kFlatAtomicSpecs) {
        t.push_back(MakeEntry(E::FLAT, a.op, 0, a.flat_name, a.dwords, false, false, true, true, false, false, false, false, S::FlatAtomic));
        t.push_back(MakeEntry(E::FLAT, a.op, 2, a.global_name, a.dwords, false, false, true, true, false, false, false, false, S::GlobalAtomic));
    }

    // -----------------------------------------------------------------------
    // 6. EXP (encoding 0x3E) — 1 mnemonic supporting all targets & flags
    // -----------------------------------------------------------------------
    t.push_back(MakeEntry(E::EXP, 0x00, 0, "exp", 4, false, true, false, false, false, false, false, false, S::Export));

    return t;
}

uint32_t MakeOpcodeKey(Rdna2Encoding enc, uint16_t raw_op, uint8_t flat_seg) noexcept {
    return (static_cast<uint32_t>(enc) << 18) | (static_cast<uint32_t>(flat_seg & 0x3u) << 16) |
           static_cast<uint32_t>(raw_op);
}

struct OpcodeIndex {
    std::vector<MemImgOpcodeEntry> table;
    std::unordered_map<uint32_t, const MemImgOpcodeEntry*> by_key;
    std::unordered_map<std::string, const MemImgOpcodeEntry*> by_name;
    size_t unique_count = 0;

    OpcodeIndex() : table(BuildOpcodeTable()) {
        std::unordered_set<std::string> names;
        for (const auto& entry : table) {
            by_key[MakeOpcodeKey(entry.encoding, entry.raw_opcode, entry.flat_seg)] = &entry;
            by_name[entry.mnemonic] = &entry;
            names.insert(entry.mnemonic);
        }
        unique_count = names.size();
    }
};

const OpcodeIndex& GetIndex() noexcept {
    static const OpcodeIndex kIndex{};
    return kIndex;
}

std::string FormatExpTargetName(uint8_t target) {
    if (target <= 7u) return "mrt" + std::to_string(target);
    if (target == 8u) return "mrtz";
    if (target == 9u) return "null";
    if (target >= 12u && target <= 15u) return "pos" + std::to_string(target - 12u);
    if (target == 20u) return "prim";
    if (target >= 32u && target <= 63u) return "param" + std::to_string(target - 32u);
    return "tgt" + std::to_string(target);
}

}  // namespace

// ---------------------------------------------------------------------------
// MTBUF Unified 7-bit Format Pack / Unpack
// ---------------------------------------------------------------------------
uint32_t MtbufFormatComponentBytes(Rdna2MtbufFormat fmt) noexcept {
    switch (fmt) {
        case Rdna2MtbufFormat::R8_UNORM:
        case Rdna2MtbufFormat::R8_SNORM:
        case Rdna2MtbufFormat::R8_UINT:
        case Rdna2MtbufFormat::R8_SINT:
        case Rdna2MtbufFormat::R8G8_UNORM:
        case Rdna2MtbufFormat::R8G8_SNORM:
        case Rdna2MtbufFormat::R8G8_UINT:
        case Rdna2MtbufFormat::R8G8_SINT:
        case Rdna2MtbufFormat::R8G8B8A8_UNORM:
        case Rdna2MtbufFormat::R8G8B8A8_SNORM:
        case Rdna2MtbufFormat::R8G8B8A8_UINT:
        case Rdna2MtbufFormat::R8G8B8A8_SINT:
            return 1u;
        case Rdna2MtbufFormat::R16_UNORM:
        case Rdna2MtbufFormat::R16_SNORM:
        case Rdna2MtbufFormat::R16_UINT:
        case Rdna2MtbufFormat::R16_SINT:
        case Rdna2MtbufFormat::R16_FLOAT:
        case Rdna2MtbufFormat::R16G16_UNORM:
        case Rdna2MtbufFormat::R16G16_SNORM:
        case Rdna2MtbufFormat::R16G16_UINT:
        case Rdna2MtbufFormat::R16G16_SINT:
        case Rdna2MtbufFormat::R16G16_FLOAT:
        case Rdna2MtbufFormat::R16G16B16A16_UNORM:
        case Rdna2MtbufFormat::R16G16B16A16_SNORM:
        case Rdna2MtbufFormat::R16G16B16A16_UINT:
        case Rdna2MtbufFormat::R16G16B16A16_SINT:
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT:
            return 2u;
        default:
            return 4u;
    }
}

uint32_t MtbufFormatByteStride(Rdna2MtbufFormat fmt) noexcept {
    switch (fmt) {
        case Rdna2MtbufFormat::R8_UNORM:
        case Rdna2MtbufFormat::R8_SNORM:
        case Rdna2MtbufFormat::R8_UINT:
        case Rdna2MtbufFormat::R8_SINT:
            return 1u;
        case Rdna2MtbufFormat::R16_UNORM:
        case Rdna2MtbufFormat::R16_SNORM:
        case Rdna2MtbufFormat::R16_UINT:
        case Rdna2MtbufFormat::R16_SINT:
        case Rdna2MtbufFormat::R16_FLOAT:
        case Rdna2MtbufFormat::R8G8_UNORM:
        case Rdna2MtbufFormat::R8G8_SNORM:
        case Rdna2MtbufFormat::R8G8_UINT:
        case Rdna2MtbufFormat::R8G8_SINT:
            return 2u;
        case Rdna2MtbufFormat::R32_UINT:
        case Rdna2MtbufFormat::R32_SINT:
        case Rdna2MtbufFormat::R32_FLOAT:
        case Rdna2MtbufFormat::R16G16_UNORM:
        case Rdna2MtbufFormat::R16G16_SNORM:
        case Rdna2MtbufFormat::R16G16_UINT:
        case Rdna2MtbufFormat::R16G16_SINT:
        case Rdna2MtbufFormat::R16G16_FLOAT:
        case Rdna2MtbufFormat::R8G8B8A8_UNORM:
        case Rdna2MtbufFormat::R8G8B8A8_SNORM:
        case Rdna2MtbufFormat::R8G8B8A8_UINT:
        case Rdna2MtbufFormat::R8G8B8A8_SINT:
            return 4u;
        case Rdna2MtbufFormat::R32G32_FLOAT:
        case Rdna2MtbufFormat::R16G16B16A16_UNORM:
        case Rdna2MtbufFormat::R16G16B16A16_SNORM:
        case Rdna2MtbufFormat::R16G16B16A16_UINT:
        case Rdna2MtbufFormat::R16G16B16A16_SINT:
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT:
            return 8u;
        case Rdna2MtbufFormat::R32G32B32_FLOAT:
            return 12u;
        case Rdna2MtbufFormat::R32G32B32A32_FLOAT:
        default:
            return 16u;
    }
}

uint32_t UnpackMtbufFormatComponent(
    const uint8_t* bytes,
    size_t buf_size,
    uint32_t base_byte_addr,
    Rdna2MtbufFormat fmt,
    uint32_t comp_idx,
    bool as_d16) noexcept {
    auto out_f = [&](float f) -> uint32_t {
        return as_d16 ? static_cast<uint32_t>(FloatToHalfBits(f)) : FloatToBits(f);
    };
    auto out_u = [&](uint32_t u) -> uint32_t {
        return as_d16 ? (u & 0xFFFFu) : u;
    };
    switch (fmt) {
        case Rdna2MtbufFormat::R8_UNORM:
        case Rdna2MtbufFormat::R8G8_UNORM:
        case Rdna2MtbufFormat::R8G8B8A8_UNORM:
            return out_f(static_cast<float>(ReadByte(bytes, buf_size, base_byte_addr + comp_idx)) / 255.0f);
        case Rdna2MtbufFormat::R8_SNORM:
        case Rdna2MtbufFormat::R8G8_SNORM:
        case Rdna2MtbufFormat::R8G8B8A8_SNORM: {
            const int8_t s = static_cast<int8_t>(ReadByte(bytes, buf_size, base_byte_addr + comp_idx));
            return out_f(std::clamp(static_cast<float>(s) / 127.0f, -1.0f, 1.0f));
        }
        case Rdna2MtbufFormat::R8_UINT:
        case Rdna2MtbufFormat::R8G8_UINT:
        case Rdna2MtbufFormat::R8G8B8A8_UINT:
            return out_u(ReadByte(bytes, buf_size, base_byte_addr + comp_idx));
        case Rdna2MtbufFormat::R8_SINT:
        case Rdna2MtbufFormat::R8G8_SINT:
        case Rdna2MtbufFormat::R8G8B8A8_SINT: {
            const int32_t s = static_cast<int8_t>(ReadByte(bytes, buf_size, base_byte_addr + comp_idx));
            return out_u(static_cast<uint32_t>(s));
        }
        case Rdna2MtbufFormat::R16_UNORM:
        case Rdna2MtbufFormat::R16G16_UNORM:
        case Rdna2MtbufFormat::R16G16B16A16_UNORM:
            return out_f(static_cast<float>(ReadU16(bytes, buf_size, base_byte_addr + comp_idx * 2u)) / 65535.0f);
        case Rdna2MtbufFormat::R16_SNORM:
        case Rdna2MtbufFormat::R16G16_SNORM:
        case Rdna2MtbufFormat::R16G16B16A16_SNORM: {
            const int16_t s = static_cast<int16_t>(ReadU16(bytes, buf_size, base_byte_addr + comp_idx * 2u));
            return out_f(std::clamp(static_cast<float>(s) / 32767.0f, -1.0f, 1.0f));
        }
        case Rdna2MtbufFormat::R16_UINT:
        case Rdna2MtbufFormat::R16G16_UINT:
        case Rdna2MtbufFormat::R16G16B16A16_UINT:
            return out_u(ReadU16(bytes, buf_size, base_byte_addr + comp_idx * 2u));
        case Rdna2MtbufFormat::R16_SINT:
        case Rdna2MtbufFormat::R16G16_SINT:
        case Rdna2MtbufFormat::R16G16B16A16_SINT: {
            const int32_t s = static_cast<int16_t>(ReadU16(bytes, buf_size, base_byte_addr + comp_idx * 2u));
            return out_u(static_cast<uint32_t>(s));
        }
        case Rdna2MtbufFormat::R16_FLOAT:
        case Rdna2MtbufFormat::R16G16_FLOAT:
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT: {
            const uint16_t h = ReadU16(bytes, buf_size, base_byte_addr + comp_idx * 2u);
            return as_d16 ? static_cast<uint32_t>(h) : FloatToBits(HalfBitsToFloat(h));
        }
        case Rdna2MtbufFormat::R32_UINT:
        case Rdna2MtbufFormat::R32_SINT:
            return out_u(ReadU32(bytes, buf_size, base_byte_addr + comp_idx * 4u));
        case Rdna2MtbufFormat::R32_FLOAT:
        case Rdna2MtbufFormat::R32G32_FLOAT:
        case Rdna2MtbufFormat::R32G32B32_FLOAT:
        case Rdna2MtbufFormat::R32G32B32A32_FLOAT:
        default: {
            const uint32_t raw = ReadU32(bytes, buf_size, base_byte_addr + comp_idx * 4u);
            return as_d16 ? static_cast<uint32_t>(FloatToHalfBits(BitsToFloat(raw))) : raw;
        }
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

void PackMtbufFormatComponent(
    uint8_t* bytes,
    size_t buf_size,
    uint32_t base_byte_addr,
    Rdna2MtbufFormat fmt,
    uint32_t comp_idx,
    uint32_t value_bits,
    bool from_d16) noexcept {
    const float fval = from_d16 ? HalfBitsToFloat(static_cast<uint16_t>(value_bits & 0xFFFFu))
                                : BitsToFloat(value_bits);
    const uint32_t uval = from_d16 ? (value_bits & 0xFFFFu) : value_bits;
    switch (fmt) {
        case Rdna2MtbufFormat::R8_UNORM:
        case Rdna2MtbufFormat::R8G8_UNORM:
        case Rdna2MtbufFormat::R8G8B8A8_UNORM: {
            const float c = std::clamp(std::isnan(fval) ? 0.0f : fval, 0.0f, 1.0f);
            const int32_t q = std::clamp(static_cast<int32_t>(RoundNearestEvenF32(c * 255.0f)), 0, 255);
            WriteByte(bytes, buf_size, base_byte_addr + comp_idx, static_cast<uint8_t>(q));
            break;
        }
        case Rdna2MtbufFormat::R8_SNORM:
        case Rdna2MtbufFormat::R8G8_SNORM:
        case Rdna2MtbufFormat::R8G8B8A8_SNORM: {
            const float c = std::clamp(std::isnan(fval) ? 0.0f : fval, -1.0f, 1.0f);
            const int32_t q = std::clamp(static_cast<int32_t>(RoundNearestEvenF32(c * 127.0f)), -127, 127);
            WriteByte(bytes, buf_size, base_byte_addr + comp_idx,
                      static_cast<uint8_t>(static_cast<int8_t>(q)));
            break;
        }
        case Rdna2MtbufFormat::R8_UINT:
        case Rdna2MtbufFormat::R8G8_UINT:
        case Rdna2MtbufFormat::R8G8B8A8_UINT:
        case Rdna2MtbufFormat::R8_SINT:
        case Rdna2MtbufFormat::R8G8_SINT:
        case Rdna2MtbufFormat::R8G8B8A8_SINT:
            WriteByte(bytes, buf_size, base_byte_addr + comp_idx, static_cast<uint8_t>(uval & 0xFFu));
            break;
        case Rdna2MtbufFormat::R16_UNORM:
        case Rdna2MtbufFormat::R16G16_UNORM:
        case Rdna2MtbufFormat::R16G16B16A16_UNORM: {
            const float c = std::clamp(std::isnan(fval) ? 0.0f : fval, 0.0f, 1.0f);
            const int32_t q = std::clamp(static_cast<int32_t>(RoundNearestEvenF32(c * 65535.0f)), 0, 65535);
            WriteU16(bytes, buf_size, base_byte_addr + comp_idx * 2u, static_cast<uint16_t>(q));
            break;
        }
        case Rdna2MtbufFormat::R16_SNORM:
        case Rdna2MtbufFormat::R16G16_SNORM:
        case Rdna2MtbufFormat::R16G16B16A16_SNORM: {
            const float c = std::clamp(std::isnan(fval) ? 0.0f : fval, -1.0f, 1.0f);
            const int32_t q = std::clamp(static_cast<int32_t>(RoundNearestEvenF32(c * 32767.0f)), -32767, 32767);
            WriteU16(bytes, buf_size, base_byte_addr + comp_idx * 2u,
                     static_cast<uint16_t>(static_cast<int16_t>(q)));
            break;
        }
        case Rdna2MtbufFormat::R16_UINT:
        case Rdna2MtbufFormat::R16G16_UINT:
        case Rdna2MtbufFormat::R16G16B16A16_UINT:
        case Rdna2MtbufFormat::R16_SINT:
        case Rdna2MtbufFormat::R16G16_SINT:
        case Rdna2MtbufFormat::R16G16B16A16_SINT:
            WriteU16(bytes, buf_size, base_byte_addr + comp_idx * 2u, static_cast<uint16_t>(uval & 0xFFFFu));
            break;
        case Rdna2MtbufFormat::R16_FLOAT:
        case Rdna2MtbufFormat::R16G16_FLOAT:
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT:
            WriteU16(bytes, buf_size, base_byte_addr + comp_idx * 2u,
                     from_d16 ? static_cast<uint16_t>(value_bits & 0xFFFFu) : FloatToHalfBits(fval));
            break;
        case Rdna2MtbufFormat::R32_UINT:
        case Rdna2MtbufFormat::R32_SINT:
            WriteU32(bytes, buf_size, base_byte_addr + comp_idx * 4u, uval);
            break;
        case Rdna2MtbufFormat::R32_FLOAT:
        case Rdna2MtbufFormat::R32G32_FLOAT:
        case Rdna2MtbufFormat::R32G32B32_FLOAT:
        case Rdna2MtbufFormat::R32G32B32A32_FLOAT:
        default:
            WriteU32(bytes, buf_size, base_byte_addr + comp_idx * 4u, FloatToBits(fval));
            break;
    }
}

// ---------------------------------------------------------------------------
// MemImgTexture Implementation (1D/2D/3D/Cube/Arrays/MSAA)
// ---------------------------------------------------------------------------
void MemImgTexture::AllocateRgba32f(
    uint8_t dimension,
    uint32_t width,
    uint32_t height,
    uint32_t depth_or_layers,
    uint32_t mip_levels,
    uint32_t samples) {
    dim = dimension & 7u;
    const uint32_t num_mips = std::max(1u, mip_levels);
    const uint32_t num_samples = std::max(1u, samples);
    mips.clear();
    mips.resize(num_mips);
    uint32_t w = std::max(1u, width);
    uint32_t h = (dim == kMimgDim1D || dim == kMimgDim1DArray) ? 1u : std::max(1u, height);
    uint32_t d = (dim == kMimgDimCube) ? std::max(6u, depth_or_layers) : std::max(1u, depth_or_layers);
    for (uint32_t m = 0; m < num_mips; ++m) {
        mips[m].width = w;
        mips[m].height = h;
        mips[m].depth_or_layers = d;
        mips[m].samples = num_samples;
        const size_t texels = static_cast<size_t>(w) * h * d * num_samples;
        mips[m].rgba32f.assign(texels * 4u, 0.0f);
        w = std::max(1u, w >> 1);
        if (dim != kMimgDim1D && dim != kMimgDim1DArray) h = std::max(1u, h >> 1);
        if (dim == kMimgDim3D) d = std::max(1u, d >> 1);
    }
}

void MemImgTexture::AllocateR32u(
    uint8_t dimension,
    uint32_t width,
    uint32_t height,
    uint32_t depth_or_layers,
    uint32_t mip_levels,
    uint32_t samples) {
    dim = dimension & 7u;
    const uint32_t num_mips = std::max(1u, mip_levels);
    const uint32_t num_samples = std::max(1u, samples);
    mips.clear();
    mips.resize(num_mips);
    uint32_t w = std::max(1u, width);
    uint32_t h = (dim == kMimgDim1D || dim == kMimgDim1DArray) ? 1u : std::max(1u, height);
    uint32_t d = (dim == kMimgDimCube) ? std::max(6u, depth_or_layers) : std::max(1u, depth_or_layers);
    for (uint32_t m = 0; m < num_mips; ++m) {
        mips[m].width = w;
        mips[m].height = h;
        mips[m].depth_or_layers = d;
        mips[m].samples = num_samples;
        const size_t texels = static_cast<size_t>(w) * h * d * num_samples;
        mips[m].r32u.assign(texels, 0u);
        w = std::max(1u, w >> 1);
        if (dim != kMimgDim1D && dim != kMimgDim1DArray) h = std::max(1u, h >> 1);
        if (dim == kMimgDim3D) d = std::max(1u, d >> 1);
    }
}

std::array<float, 4> MemImgTexture::LoadRgba32f(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip, uint32_t sample) const noexcept {
    if (mip >= mips.size()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const auto& m = mips[mip];
    if (m.rgba32f.empty() || x >= m.width || y >= m.height || z >= m.depth_or_layers || sample >= m.samples) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const size_t idx = ((((static_cast<size_t>(z) * m.height + y) * m.width + x) * m.samples) + sample) * 4u;
    return {m.rgba32f[idx], m.rgba32f[idx + 1u], m.rgba32f[idx + 2u], m.rgba32f[idx + 3u]};
}

void MemImgTexture::StoreRgba32f(
    uint32_t x, uint32_t y, uint32_t z, const std::array<float, 4>& rgba,
    uint32_t mip, uint32_t sample) noexcept {
    if (mip >= mips.size()) return;
    auto& m = mips[mip];
    if (m.rgba32f.empty() || x >= m.width || y >= m.height || z >= m.depth_or_layers || sample >= m.samples) return;
    const size_t idx = ((((static_cast<size_t>(z) * m.height + y) * m.width + x) * m.samples) + sample) * 4u;
    m.rgba32f[idx + 0u] = rgba[0];
    m.rgba32f[idx + 1u] = rgba[1];
    m.rgba32f[idx + 2u] = rgba[2];
    m.rgba32f[idx + 3u] = rgba[3];
}

uint32_t MemImgTexture::LoadR32u(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip, uint32_t sample) const noexcept {
    if (mip >= mips.size()) return 0u;
    const auto& m = mips[mip];
    if (m.r32u.empty() || x >= m.width || y >= m.height || z >= m.depth_or_layers || sample >= m.samples) return 0u;
    const size_t idx = (((static_cast<size_t>(z) * m.height + y) * m.width + x) * m.samples) + sample;
    return m.r32u[idx];
}

void MemImgTexture::StoreR32u(
    uint32_t x, uint32_t y, uint32_t z, uint32_t val, uint32_t mip, uint32_t sample) noexcept {
    if (uint32_t* p = MutableR32uPtr(x, y, z, mip, sample)) {
        *p = val;
    }
}

uint32_t* MemImgTexture::MutableR32uPtr(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip, uint32_t sample) noexcept {
    if (mip >= mips.size()) return nullptr;
    auto& m = mips[mip];
    if (m.r32u.empty() || x >= m.width || y >= m.height || z >= m.depth_or_layers || sample >= m.samples) {
        return nullptr;
    }
    const size_t idx = (((static_cast<size_t>(z) * m.height + y) * m.width + x) * m.samples) + sample;
    return &m.r32u[idx];
}

std::array<float, 4> MemImgTexture::Sample(
    float u, float v, float w, float lod, bool linear, bool unrm) const noexcept {
    if (mips.empty()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const uint32_t mip = static_cast<uint32_t>(
        std::clamp(static_cast<int32_t>(std::floor(lod + 0.5f)), 0, static_cast<int32_t>(mips.size() - 1u)));
    const auto& m = mips[mip];
    float fx = unrm ? u : (u * static_cast<float>(m.width));
    float fy = unrm ? v : (v * static_cast<float>(m.height));
    uint32_t iz = 0;
    if (dim == kMimgDim3D) {
        const float fz = unrm ? w : (w * static_cast<float>(m.depth_or_layers));
        iz = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor(fz)), 0,
                                              static_cast<int32_t>(m.depth_or_layers) - 1));
    } else if (dim == kMimgDimCube) {
        // Standard cubemap major-axis projection when given 3D direction (u, v, w)
        const float ax = std::fabs(u);
        const float ay = std::fabs(v);
        const float az = std::fabs(w);
        float sc = 0.0f, tc = 0.0f, ma = 1.0f;
        if (ax >= ay && ax >= az) {
            ma = std::max(ax, 1e-6f);
            iz = (u >= 0.0f) ? 0u : 1u;
            sc = (u >= 0.0f) ? -w : w;
            tc = -v;
        } else if (ay >= ax && ay >= az) {
            ma = std::max(ay, 1e-6f);
            iz = (v >= 0.0f) ? 2u : 3u;
            sc = u;
            tc = (v >= 0.0f) ? w : -w;
        } else {
            ma = std::max(az, 1e-6f);
            iz = (w >= 0.0f) ? 4u : 5u;
            sc = (w >= 0.0f) ? u : -u;
            tc = -v;
        }
        const float cu = 0.5f * (sc / ma + 1.0f);
        const float cv = 0.5f * (tc / ma + 1.0f);
        fx = cu * static_cast<float>(m.width);
        fy = cv * static_cast<float>(m.height);
    } else if (dim == kMimgDim1DArray || dim == kMimgDim2DArray || dim == kMimgDim2DArrayMsaa) {
        iz = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor(w + 0.5f)), 0,
                                              static_cast<int32_t>(m.depth_or_layers) - 1));
    }

    if (!linear) {
        const uint32_t ix = static_cast<uint32_t>(
            std::clamp(static_cast<int32_t>(std::floor(fx)), 0, static_cast<int32_t>(m.width) - 1));
        const uint32_t iy = static_cast<uint32_t>(
            std::clamp(static_cast<int32_t>(std::floor(fy)), 0, static_cast<int32_t>(m.height) - 1));
        return LoadRgba32f(ix, iy, iz, mip, 0u);
    }
    const float sx = fx - 0.5f;
    const float sy = fy - 0.5f;
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(sx)), 0, static_cast<int32_t>(m.width) - 1);
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(sy)), 0, static_cast<int32_t>(m.height) - 1);
    const int32_t x1 = std::clamp(x0 + 1, 0, static_cast<int32_t>(m.width) - 1);
    const int32_t y1 = std::clamp(y0 + 1, 0, static_cast<int32_t>(m.height) - 1);
    const float tx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
    const float ty = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
    const auto c00 = LoadRgba32f(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), iz, mip, 0u);
    const auto c10 = LoadRgba32f(static_cast<uint32_t>(x1), static_cast<uint32_t>(y0), iz, mip, 0u);
    const auto c01 = LoadRgba32f(static_cast<uint32_t>(x0), static_cast<uint32_t>(y1), iz, mip, 0u);
    const auto c11 = LoadRgba32f(static_cast<uint32_t>(x1), static_cast<uint32_t>(y1), iz, mip, 0u);
    std::array<float, 4> out{};
    for (int ch = 0; ch < 4; ++ch) {
        const float top = c00[ch] * (1.0f - tx) + c10[ch] * tx;
        const float bot = c01[ch] * (1.0f - tx) + c11[ch] * tx;
        out[ch] = top * (1.0f - ty) + bot * ty;
    }
    return out;
}

std::array<float, 4> MemImgTexture::Gather4(
    float u, float v, float w, uint32_t channel, bool unrm) const noexcept {
    if (mips.empty()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const auto& m = mips[0];
    const uint32_t ch = channel & 3u;
    const float sx = (unrm ? u : (u * static_cast<float>(m.width))) - 0.5f;
    const float sy = (unrm ? v : (v * static_cast<float>(m.height))) - 0.5f;
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(sx)), 0, static_cast<int32_t>(m.width) - 1);
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(sy)), 0, static_cast<int32_t>(m.height) - 1);
    const int32_t x1 = std::clamp(x0 + 1, 0, static_cast<int32_t>(m.width) - 1);
    const int32_t y1 = std::clamp(y0 + 1, 0, static_cast<int32_t>(m.height) - 1);
    const uint32_t iz = (dim == kMimgDim2DArray || dim == kMimgDimCube)
                            ? static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor(w + 0.5f)), 0,
                                                               static_cast<int32_t>(m.depth_or_layers) - 1))
                            : 0u;
    // DirectX / Vulkan gather4 order: (0,1), (1,1), (1,0), (0,0)
    const float t01 = LoadRgba32f(static_cast<uint32_t>(x0), static_cast<uint32_t>(y1), iz, 0u, 0u)[ch];
    const float t11 = LoadRgba32f(static_cast<uint32_t>(x1), static_cast<uint32_t>(y1), iz, 0u, 0u)[ch];
    const float t10 = LoadRgba32f(static_cast<uint32_t>(x1), static_cast<uint32_t>(y0), iz, 0u, 0u)[ch];
    const float t00 = LoadRgba32f(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), iz, 0u, 0u)[ch];
    return {t01, t11, t10, t00};
}

// ---------------------------------------------------------------------------
// Table & Lookup API
// ---------------------------------------------------------------------------
const std::vector<MemImgOpcodeEntry>& GetMemImgOpcodeTable() noexcept {
    return GetIndex().table;
}

const MemImgOpcodeEntry* LookupMemImgOpcode(
    Rdna2Encoding enc, uint16_t raw_opcode, uint8_t flat_seg) noexcept {
    const uint8_t seg = (enc == Rdna2Encoding::FLAT) ? (flat_seg & 0x3u) : 0u;
    const auto& by_key = GetIndex().by_key;
    const auto it = by_key.find(MakeOpcodeKey(enc, raw_opcode, seg));
    return (it != by_key.end()) ? it->second : nullptr;
}

const MemImgOpcodeEntry* LookupMemImgByName(const char* mnemonic) noexcept {
    if (!mnemonic) return nullptr;
    const auto& by_name = GetIndex().by_name;
    const auto it = by_name.find(mnemonic);
    return (it != by_name.end()) ? it->second : nullptr;
}

size_t GetUniqueMemImgMnemonicCount() noexcept {
    return GetIndex().unique_count;
}

// ---------------------------------------------------------------------------
// Decoder (MUBUF / MTBUF / MIMG / DS / FLAT / GLOBAL / SCRATCH / EXP)
// ---------------------------------------------------------------------------
Status DecodeMemImgAt(
    const uint32_t* words,
    size_t word_count,
    size_t pc,
    MemImgDecodedInst& out_inst) {
    out_inst = MemImgDecodedInst{};
    if (!words || pc >= word_count) {
        return Status::Error(StatusCode::ParseError, "Instruction PC out of bounds");
    }

    const uint32_t w0 = words[pc];
    Rdna2Instruction& inst = out_inst.base;
    inst.word_offset = static_cast<uint32_t>(pc);
    inst.raw_words[0] = w0;

    // Graceful SOPP support for s_nop / s_endpgm / s_barrier / s_waitcnt inside shader streams
    if ((w0 >> 23) == 0x17Fu) {
        inst.word_count = 1;
        inst.encoding = Rdna2Encoding::SOPP;
        inst.raw_opcode = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
        inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        if (inst.raw_opcode == 0x00u) {
            inst.opcode = Rdna2Opcode::S_NOP;
            inst.mnemonic = "s_nop";
        } else if (inst.raw_opcode == 0x01u) {
            inst.opcode = Rdna2Opcode::S_ENDPGM;
            inst.mnemonic = "s_endpgm";
        } else if (inst.raw_opcode == 0x0Au) {
            inst.opcode = Rdna2Opcode::S_BARRIER;
            inst.mnemonic = "s_barrier";
        } else if (inst.raw_opcode == 0x0Cu) {
            inst.opcode = Rdna2Opcode::S_WAITCNT;
            inst.mnemonic = "s_waitcnt";
        } else {
            return Status::Error(StatusCode::UnsupportedFormat, "Unsupported SOPP opcode in MemImg stream");
        }
        inst.disasm = inst.mnemonic;
        return Status::Ok();
    }

    if (pc + 1u >= word_count) {
        return Status::Error(StatusCode::ParseError, "Truncated 64-bit MemImg instruction");
    }
    const uint32_t w1 = words[pc + 1u];
    inst.word_count = 2;
    inst.raw_words[1] = w1;

    const uint32_t enc_bits = w0 >> 26;
    if (enc_bits == 0x38u) {
        // MUBUF (64-bit)
        inst.encoding = Rdna2Encoding::MUBUF;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
        out_inst.lds = ((w0 >> 16) & 1u) != 0u;
        inst.dlc = ((w0 >> 15) & 1u) != 0u;
        inst.glc = ((w0 >> 14) & 1u) != 0u;
        inst.idxen = ((w0 >> 13) & 1u) != 0u;
        inst.offen = ((w0 >> 12) & 1u) != 0u;
        inst.mem_offset = static_cast<int32_t>(w0 & 0xFFFu);
        inst.soffset = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
        out_inst.tfe = ((w1 >> 23) & 1u) != 0u;
        inst.slc = ((w1 >> 22) & 1u) != 0u;
        inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
        const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
        const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupMemImgOpcode(Rdna2Encoding::MUBUF, raw_op, 0u);
        if (!out_inst.entry) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown MUBUF raw opcode");
        }
        out_inst.d16 = out_inst.entry->is_d16;
        if (out_inst.entry->is_load || out_inst.entry->is_atomic) {
            inst.dst = Rdna2Operand::Vgpr(vdata);
        }
        inst.src[0] = Rdna2Operand::Vgpr(vdata);
        inst.src[1] = Rdna2Operand::Vgpr(vaddr);
        inst.src_count = 2;
    } else if (enc_bits == 0x3Au) {
        // MTBUF (64-bit)
        inst.encoding = Rdna2Encoding::MTBUF;
        inst.mtbuf_format = static_cast<uint8_t>((w0 >> 19) & 0x7Fu);
        const uint16_t raw_op = static_cast<uint16_t>(
            ((w0 >> 16) & 0x7u) | (((w1 >> 23) & 1u) << 3) | (((w1 >> 21) & 1u) << 4));
        inst.dlc = ((w0 >> 15) & 1u) != 0u;
        inst.glc = ((w0 >> 14) & 1u) != 0u;
        inst.idxen = ((w0 >> 13) & 1u) != 0u;
        inst.offen = ((w0 >> 12) & 1u) != 0u;
        inst.mem_offset = static_cast<int32_t>(w0 & 0xFFFu);
        inst.soffset = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
        inst.slc = ((w1 >> 22) & 1u) != 0u;
        inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
        const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
        const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupMemImgOpcode(Rdna2Encoding::MTBUF, raw_op, 0u);
        if (!out_inst.entry) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown MTBUF raw opcode");
        }
        out_inst.d16 = out_inst.entry->is_d16;
        if (out_inst.entry->is_load) {
            inst.dst = Rdna2Operand::Vgpr(vdata);
        }
        inst.src[0] = Rdna2Operand::Vgpr(vdata);
        inst.src[1] = Rdna2Operand::Vgpr(vaddr);
        inst.src_count = 2;
    } else if (enc_bits == 0x3Cu) {
        // MIMG (64-bit)
        inst.encoding = Rdna2Encoding::MIMG;
        inst.slc = ((w0 >> 25) & 1u) != 0u;
        const uint16_t raw_op = static_cast<uint16_t>(((w0 >> 18) & 0x7Fu) | ((w0 & 1u) << 7));
        out_inst.lwe = ((w0 >> 17) & 1u) != 0u;
        out_inst.a16 = ((w0 >> 16) & 1u) != 0u;
        out_inst.r128 = ((w0 >> 15) & 1u) != 0u;
        inst.glc = ((w0 >> 13) & 1u) != 0u;
        inst.mimg_unrm = ((w0 >> 12) & 1u) != 0u;
        inst.mimg_dmask = static_cast<uint8_t>((w0 >> 8) & 0xFu);
        inst.dlc = ((w0 >> 7) & 1u) != 0u;
        out_inst.tfe = ((w0 >> 6) & 1u) != 0u;
        inst.mimg_dim = static_cast<uint8_t>((w0 >> 3) & 0x7u);
        out_inst.d16 = ((w1 >> 31) & 1u) != 0u;
        inst.ssamp = static_cast<uint8_t>((w1 >> 21) & 0x1Fu);
        inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
        const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
        const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupMemImgOpcode(Rdna2Encoding::MIMG, raw_op, 0u);
        if (!out_inst.entry) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown MIMG raw opcode");
        }
        if (out_inst.entry->is_store) {
            inst.src[0] = Rdna2Operand::Vgpr(vdata);
            inst.src[1] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 2;
        } else if (out_inst.entry->is_atomic) {
            inst.dst = Rdna2Operand::Vgpr(vdata);
            inst.src[0] = Rdna2Operand::Vgpr(vdata);
            inst.src[1] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 2;
        } else {
            inst.dst = Rdna2Operand::Vgpr(vdata);
            inst.src[0] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 1;
        }
    } else if (enc_bits == 0x36u) {
        // DS (64-bit)
        inst.encoding = Rdna2Encoding::DS;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
        inst.ds_gds = ((w0 >> 17) & 1u) != 0u;
        inst.ds_offset1 = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
        inst.ds_offset0 = static_cast<uint8_t>(w0 & 0xFFu);
        inst.mem_offset = static_cast<int32_t>(
            (static_cast<uint32_t>(inst.ds_offset1) << 8) | inst.ds_offset0);
        const uint8_t vdst = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
        const uint8_t data1 = static_cast<uint8_t>((w1 >> 16) & 0xFFu);
        const uint8_t data0 = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
        const uint8_t addr = static_cast<uint8_t>(w1 & 0xFFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupMemImgOpcode(Rdna2Encoding::DS, raw_op, 0u);
        if (!out_inst.entry) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown DS raw opcode");
        }
        out_inst.d16 = out_inst.entry->is_d16;
        if (out_inst.entry->has_rtn) {
            inst.dst = Rdna2Operand::Vgpr(vdst);
        }
        inst.src[0] = Rdna2Operand::Vgpr(addr);
        inst.src[1] = Rdna2Operand::Vgpr(data0);
        inst.src[2] = Rdna2Operand::Vgpr(data1);
        inst.src_count = 3;
    } else if (enc_bits == 0x37u) {
        // FLAT / GLOBAL / SCRATCH (64-bit)
        inst.encoding = Rdna2Encoding::FLAT;
        const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
        inst.slc = ((w0 >> 17) & 1u) != 0u;
        inst.glc = ((w0 >> 16) & 1u) != 0u;
        inst.flat_seg = static_cast<uint8_t>((w0 >> 14) & 0x3u);
        inst.dlc = ((w0 >> 12) & 1u) != 0u;
        inst.mem_offset = (inst.flat_seg == 0u)
                              ? static_cast<int32_t>(w0 & 0xFFFu)
                              : SignExtend(w0 & 0xFFFu, 12u);
        const uint8_t vdst = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
        inst.saddr = static_cast<uint8_t>((w1 >> 16) & 0x7Fu);
        const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
        const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
        inst.raw_opcode = raw_op;
        out_inst.entry = LookupMemImgOpcode(Rdna2Encoding::FLAT, raw_op, inst.flat_seg);
        if (!out_inst.entry) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown FLAT/GLOBAL/SCRATCH raw opcode");
        }
        out_inst.d16 = out_inst.entry->is_d16;
        if (out_inst.entry->is_load) {
            inst.dst = Rdna2Operand::Vgpr(vdst);
            inst.src[0] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 1;
        } else if (out_inst.entry->is_atomic) {
            inst.dst = Rdna2Operand::Vgpr(vdst);
            inst.src[0] = Rdna2Operand::Vgpr(vaddr);
            inst.src[1] = Rdna2Operand::Vgpr(vdata);
            inst.src_count = 2;
        } else {
            inst.src[0] = Rdna2Operand::Vgpr(vaddr);
            inst.src[1] = Rdna2Operand::Vgpr(vdata);
            inst.src_count = 2;
        }
    } else if (enc_bits == 0x3Eu) {
        // EXP (64-bit)
        inst.encoding = Rdna2Encoding::EXP;
        inst.opcode = Rdna2Opcode::EXP;
        inst.raw_opcode = 0;
        inst.exp_vm = ((w0 >> 12) & 1u) != 0u;
        inst.exp_done = ((w0 >> 11) & 1u) != 0u;
        inst.exp_compr = ((w0 >> 10) & 1u) != 0u;
        inst.exp_target = static_cast<uint8_t>((w0 >> 4) & 0x3Fu);
        inst.exp_en = static_cast<uint8_t>(w0 & 0xFu);
        inst.src[0] = Rdna2Operand::Vgpr(static_cast<uint8_t>(w1 & 0xFFu));
        inst.src[1] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 8) & 0xFFu));
        inst.src[2] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 16) & 0xFFu));
        inst.src[3] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 24) & 0xFFu));
        inst.src_count = 4;
        out_inst.entry = LookupMemImgOpcode(Rdna2Encoding::EXP, 0u, 0u);
    } else {
        return Status::Error(StatusCode::UnsupportedFormat, "Instruction encoding is not MemImg/DS/FLAT/EXP");
    }

    inst.mnemonic = out_inst.entry->mnemonic;
    std::ostringstream ds;
    ds << inst.mnemonic;
    if (inst.encoding == Rdna2Encoding::EXP) {
        ds << " " << FormatExpTargetName(inst.exp_target);
        if (inst.exp_done) ds << " done";
        if (inst.exp_compr) ds << " compr";
        if (inst.exp_vm) ds << " vm";
    }
    inst.disasm = ds.str();
    return Status::Ok();
}

Status DecodeMemImgStream(
    const std::vector<uint32_t>& words,
    std::vector<MemImgDecodedInst>& out_insts) {
    out_insts.clear();
    if (words.empty()) {
        return Status::Error(StatusCode::ParseError, "Empty MemImg instruction stream");
    }
    size_t pc = 0;
    while (pc < words.size()) {
        MemImgDecodedInst decoded{};
        if (auto st = DecodeMemImgAt(words.data(), words.size(), pc, decoded); !st.ok()) {
            return st;
        }
        pc += decoded.base.word_count;
        out_insts.push_back(std::move(decoded));
    }
    return Status::Ok();
}

bool EncodeMemImgInst(
    const char* mnemonic,
    const MemImgEncodeParams& params,
    std::vector<uint32_t>& out_words) {
    const MemImgOpcodeEntry* entry = LookupMemImgByName(mnemonic);
    if (!entry) return false;

    uint32_t w0 = 0;
    uint32_t w1 = 0;
    switch (entry->encoding) {
        case Rdna2Encoding::MUBUF: {
            const uint32_t srsrc = static_cast<uint32_t>(params.srsrc_sgpr >> 2) & 0x1Fu;
            w0 = (0x38u << 26) | (static_cast<uint32_t>(entry->raw_opcode & 0x7Fu) << 18) |
                 (params.lds ? (1u << 16) : 0u) | (params.dlc ? (1u << 15) : 0u) |
                 (params.glc ? (1u << 14) : 0u) | (params.idxen ? (1u << 13) : 0u) |
                 (params.offen ? (1u << 12) : 0u) | (static_cast<uint32_t>(params.offset) & 0xFFFu);
            w1 = (static_cast<uint32_t>(params.soffset) << 24) |
                 (params.tfe ? (1u << 23) : 0u) | (params.slc ? (1u << 22) : 0u) |
                 (srsrc << 16) | (static_cast<uint32_t>(params.vdata) << 8) |
                 static_cast<uint32_t>(params.vaddr);
            break;
        }
        case Rdna2Encoding::MTBUF: {
            const uint32_t srsrc = static_cast<uint32_t>(params.srsrc_sgpr >> 2) & 0x1Fu;
            w0 = (0x3Au << 26) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(params.mtbuf_format) & 0x7Fu) << 19) |
                 (static_cast<uint32_t>(entry->raw_opcode & 0x7u) << 16) |
                 (params.dlc ? (1u << 15) : 0u) | (params.glc ? (1u << 14) : 0u) |
                 (params.idxen ? (1u << 13) : 0u) | (params.offen ? (1u << 12) : 0u) |
                 (static_cast<uint32_t>(params.offset) & 0xFFFu);
            w1 = (static_cast<uint32_t>(params.soffset) << 24) |
                 (static_cast<uint32_t>((entry->raw_opcode >> 3) & 1u) << 23) |
                 (params.slc ? (1u << 22) : 0u) |
                 (static_cast<uint32_t>((entry->raw_opcode >> 4) & 1u) << 21) |
                 (srsrc << 16) | (static_cast<uint32_t>(params.vdata) << 8) |
                 static_cast<uint32_t>(params.vaddr);
            break;
        }
        case Rdna2Encoding::MIMG: {
            const uint32_t srsrc = static_cast<uint32_t>(params.srsrc_sgpr >> 2) & 0x1Fu;
            const uint32_t ssamp = static_cast<uint32_t>(params.ssamp_sgpr >> 2) & 0x1Fu;
            w0 = (0x3Cu << 26) | (params.slc ? (1u << 25) : 0u) |
                 (static_cast<uint32_t>(entry->raw_opcode & 0x7Fu) << 18) |
                 (params.lwe ? (1u << 17) : 0u) | (params.a16 ? (1u << 16) : 0u) |
                 (params.r128 ? (1u << 15) : 0u) | (params.glc ? (1u << 13) : 0u) |
                 (params.unrm ? (1u << 12) : 0u) | (static_cast<uint32_t>(params.dmask & 0xFu) << 8) |
                 (params.dlc ? (1u << 7) : 0u) | (params.tfe ? (1u << 6) : 0u) |
                 (static_cast<uint32_t>(params.dim & 0x7u) << 3) |
                 static_cast<uint32_t>((entry->raw_opcode >> 7) & 1u);
            w1 = (params.d16 ? (1u << 31) : 0u) | (ssamp << 21) | (srsrc << 16) |
                 (static_cast<uint32_t>(params.vdata) << 8) | static_cast<uint32_t>(params.vaddr);
            break;
        }
        case Rdna2Encoding::DS: {
            const uint16_t ds_off = (params.offset != 0)
                                        ? static_cast<uint16_t>(params.offset & 0xFFFFu)
                                        : static_cast<uint16_t>(
                                              (static_cast<uint16_t>(params.offset1) << 8) | params.offset0);
            w0 = (0x36u << 26) | (static_cast<uint32_t>(entry->raw_opcode & 0xFFu) << 18) |
                 (params.gds ? (1u << 17) : 0u) | static_cast<uint32_t>(ds_off);
            w1 = (static_cast<uint32_t>(params.vdst) << 24) |
                 (static_cast<uint32_t>(params.data1) << 16) |
                 (static_cast<uint32_t>(params.data0) << 8) |
                 static_cast<uint32_t>(params.vaddr);
            break;
        }
        case Rdna2Encoding::FLAT: {
            w0 = (0x37u << 26) | (static_cast<uint32_t>(entry->raw_opcode & 0x7Fu) << 18) |
                 (params.slc ? (1u << 17) : 0u) | (params.glc ? (1u << 16) : 0u) |
                 (static_cast<uint32_t>(entry->flat_seg & 0x3u) << 14) |
                 (params.dlc ? (1u << 12) : 0u) | (static_cast<uint32_t>(params.offset) & 0xFFFu);
            w1 = (static_cast<uint32_t>(params.vdst) << 24) |
                 (static_cast<uint32_t>(params.saddr & 0x7Fu) << 16) |
                 (static_cast<uint32_t>(params.vdata) << 8) |
                 static_cast<uint32_t>(params.vaddr);
            break;
        }
        case Rdna2Encoding::EXP: {
            w0 = (0x3Eu << 26) | (params.exp_vm ? (1u << 12) : 0u) |
                 (params.exp_done ? (1u << 11) : 0u) | (params.exp_compr ? (1u << 10) : 0u) |
                 (static_cast<uint32_t>(params.exp_target & 0x3Fu) << 4) |
                 static_cast<uint32_t>(params.exp_en & 0xFu);
            w1 = static_cast<uint32_t>(params.exp_vsrc[0]) |
                 (static_cast<uint32_t>(params.exp_vsrc[1]) << 8) |
                 (static_cast<uint32_t>(params.exp_vsrc[2]) << 16) |
                 (static_cast<uint32_t>(params.exp_vsrc[3]) << 24);
            break;
        }
        default:
            return false;
    }

    out_words.push_back(w0);
    out_words.push_back(w1);
    return true;
}

void SeedDefaultComputeVgprs(MemImgExecState& state) noexcept {
    const uint32_t lanes = std::min(state.wave_size, 64u);
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        state.vgpr[lane].fill(0u);
        state.vgpr[lane][0] = lane;
        state.vgpr[lane][1] = lane * 4u;
        state.vgpr[lane][2] = lane * 16u;
        state.vgpr[lane][3] = lane * 8u;
    }
}

// ---------------------------------------------------------------------------
// CPU Reference Execution Engine (ExecuteMemImgCpu)
// ---------------------------------------------------------------------------
namespace {

int32_t ResolveSoffset(const MemImgExecState& state, uint8_t code) noexcept {
    if (code < 106u) return static_cast<int32_t>(state.sgpr[code]);
    if (code == 124u) return static_cast<int32_t>(state.m0);
    if (code >= 128u && code <= 192u) return static_cast<int32_t>(code) - 128;
    if (code >= 193u && code <= 208u) return -(static_cast<int32_t>(code) - 192);
    return 0;
}

uint32_t ComputeBufferByteAddr(
    const MemImgExecState& state,
    const Rdna2Instruction& inst,
    uint32_t lane,
    uint8_t vaddr) noexcept {
    int64_t addr = static_cast<int64_t>(inst.mem_offset) +
                   static_cast<int64_t>(ResolveSoffset(state, inst.soffset));
    if (inst.idxen && inst.offen) {
        addr += static_cast<int64_t>(state.vgpr[lane][vaddr & 63u]) *
                static_cast<int64_t>(state.buffer_stride_bytes);
        addr += static_cast<int64_t>(state.vgpr[lane][(vaddr + 1u) & 63u]);
    } else if (inst.idxen) {
        addr += static_cast<int64_t>(state.vgpr[lane][vaddr & 63u]) *
                static_cast<int64_t>(state.buffer_stride_bytes);
    } else if (inst.offen) {
        addr += static_cast<int64_t>(state.vgpr[lane][vaddr & 63u]);
    }
    return (addr < 0) ? 0u : static_cast<uint32_t>(addr);
}

uint32_t ExecAtomic32(uint32_t& mem, uint16_t raw_op, uint32_t src0, uint32_t src1) noexcept {
    const uint32_t old = mem;
    switch (raw_op) {
        case 0x30: mem = src0; break;
        case 0x31: if (old == src1) mem = src0; break;
        case 0x32: mem = old + src0; break;
        case 0x33: mem = old - src0; break;
        case 0x34: mem = (old >= src0) ? (old - src0) : 0u; break;
        case 0x35: mem = static_cast<uint32_t>(std::min(static_cast<int32_t>(old), static_cast<int32_t>(src0))); break;
        case 0x36: mem = std::min(old, src0); break;
        case 0x37: mem = static_cast<uint32_t>(std::max(static_cast<int32_t>(old), static_cast<int32_t>(src0))); break;
        case 0x38: mem = std::max(old, src0); break;
        case 0x39: mem = old & src0; break;
        case 0x3A: mem = old | src0; break;
        case 0x3B: mem = old ^ src0; break;
        case 0x3C: mem = (old >= src0) ? 0u : (old + 1u); break;
        case 0x3D: mem = (old == 0u || old > src0) ? src0 : (old - 1u); break;
        case 0x3E: if (BitsToFloat(old) == BitsToFloat(src1)) mem = src0; break;
        case 0x3F: mem = FloatToBits(std::fmin(BitsToFloat(old), BitsToFloat(src0))); break;
        case 0x40: mem = FloatToBits(std::fmax(BitsToFloat(old), BitsToFloat(src0))); break;
        case 0x4D: mem = FloatToBits(BitsToFloat(old) + BitsToFloat(src0)); break;
        default: break;
    }
    return old;
}

uint64_t ExecAtomic64(uint64_t& mem, uint16_t raw_op, uint64_t src0, uint64_t src1) noexcept {
    const uint64_t old = mem;
    switch (raw_op) {
        case 0x50: mem = src0; break;
        case 0x51: if (old == src1) mem = src0; break;
        case 0x52: mem = old + src0; break;
        case 0x53: mem = old - src0; break;
        case 0x55: mem = static_cast<uint64_t>(std::min(static_cast<int64_t>(old), static_cast<int64_t>(src0))); break;
        case 0x56: mem = std::min(old, src0); break;
        case 0x57: mem = static_cast<uint64_t>(std::max(static_cast<int64_t>(old), static_cast<int64_t>(src0))); break;
        case 0x58: mem = std::max(old, src0); break;
        case 0x59: mem = old & src0; break;
        case 0x5A: mem = old | src0; break;
        case 0x5B: mem = old ^ src0; break;
        case 0x5C: mem = (old >= src0) ? 0ull : (old + 1ull); break;
        case 0x5D: mem = (old == 0ull || old > src0) ? src0 : (old - 1ull); break;
        case 0x5E: if (BitsToDouble(old) == BitsToDouble(src1)) mem = src0; break;
        case 0x5F: mem = DoubleToBits(std::fmin(BitsToDouble(old), BitsToDouble(src0))); break;
        case 0x60: mem = DoubleToBits(std::fmax(BitsToDouble(old), BitsToDouble(src0))); break;
        default: break;
    }
    return old;
}

bool EvalCompare(uint32_t func, float ref_val, float tex_val) noexcept {
    switch (func) {
        case 1:  return ref_val < tex_val;
        case 2:  return ref_val > tex_val;
        case 3:  return ref_val >= tex_val;
        case 0:
        default: return ref_val <= tex_val;
    }
}

void ExecMubufOrMtbufCpu(const MemImgDecodedInst& di, MemImgExecState& state) {
    const auto& inst = di.base;
    const auto* entry = di.entry;
    if (!entry) return;

    if (inst.encoding == Rdna2Encoding::MUBUF && entry->raw_opcode == 0x71u) {
        ++state.gl0_inv_count;
        return;
    }
    if (inst.encoding == Rdna2Encoding::MUBUF && entry->raw_opcode == 0x72u) {
        ++state.gl1_inv_count;
        return;
    }

    std::vector<uint8_t>& buf = (inst.srsrc == 0u) ? state.uav0 : state.uav1;
    const uint8_t vdata = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
    const uint8_t vaddr = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
    const Rdna2MtbufFormat fmt = (inst.encoding == Rdna2Encoding::MTBUF)
                                     ? static_cast<Rdna2MtbufFormat>(inst.mtbuf_format)
                                     : state.default_mubuf_format;

    const uint32_t lanes = std::min(state.wave_size, 64u);
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (((state.exec_mask >> lane) & 1ull) == 0ull) continue;
        const uint32_t addr = ComputeBufferByteAddr(state, inst, lane, vaddr);
        auto& v = state.vgpr[lane];

        if (inst.encoding == Rdna2Encoding::MTBUF ||
            entry->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferLoadFormat) ||
            entry->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferStoreFormat)) {
            const uint16_t op = entry->raw_opcode;
            if (op <= 0x03u) {
                const uint32_t comps = static_cast<uint32_t>(op) + 1u;
                for (uint32_t c = 0; c < comps; ++c) {
                    v[(vdata + c) & 63u] = UnpackMtbufFormatComponent(buf.data(), buf.size(), addr, fmt, c, false);
                }
            } else if (op >= 0x04u && op <= 0x07u) {
                const uint32_t comps = static_cast<uint32_t>(op - 0x04u) + 1u;
                for (uint32_t c = 0; c < comps; ++c) {
                    PackMtbufFormatComponent(buf.data(), buf.size(), addr, fmt, c, v[(vdata + c) & 63u], false);
                }
            } else if (op >= 0x08u && op <= 0x0Bu) {
                const uint32_t comps = static_cast<uint32_t>(op - 0x08u) + 1u;
                uint32_t hs[4] = {0u, 0u, 0u, 0u};
                for (uint32_t c = 0; c < comps; ++c) {
                    hs[c] = UnpackMtbufFormatComponent(buf.data(), buf.size(), addr, fmt, c, true) & 0xFFFFu;
                }
                if (comps == 1u) {
                    // GFX10.3 D16_HI=0 hi-preserve: keep upper 16 bits of v[vdata] untouched
                    v[vdata & 63u] = (v[vdata & 63u] & 0xFFFF0000u) | hs[0];
                } else {
                    v[vdata & 63u] = hs[0] | (hs[1] << 16);
                }
                if (comps == 3u) {
                    // GFX10.3 D16_HI=0 hi-preserve: keep upper 16 bits of v[vdata+1] untouched
                    v[(vdata + 1u) & 63u] = (v[(vdata + 1u) & 63u] & 0xFFFF0000u) | hs[2];
                } else if (comps == 4u) {
                    v[(vdata + 1u) & 63u] = hs[2] | (hs[3] << 16);
                }
            } else if (op >= 0x0Cu && op <= 0x0Fu) {
                const uint32_t comps = static_cast<uint32_t>(op - 0x0Cu) + 1u;
                for (uint32_t c = 0; c < comps; ++c) {
                    const uint32_t word = v[(vdata + (c >> 1)) & 63u];
                    const uint32_t h = ((c & 1u) == 0u) ? (word & 0xFFFFu) : ((word >> 16) & 0xFFFFu);
                    PackMtbufFormatComponent(buf.data(), buf.size(), addr, fmt, c, h, true);
                }
            } else if (op == 0x26u || op == 0x10u) {
                // *_load_format_d16_hi_x
                const uint32_t h = UnpackMtbufFormatComponent(buf.data(), buf.size(), addr, fmt, 0u, true) & 0xFFFFu;
                v[vdata & 63u] = (v[vdata & 63u] & 0xFFFFu) | (h << 16);
            } else if (op == 0x27u || op == 0x11u) {
                // *_store_format_d16_hi_x
                const uint32_t h = (v[vdata & 63u] >> 16) & 0xFFFFu;
                PackMtbufFormatComponent(buf.data(), buf.size(), addr, fmt, 0u, h, true);
            }
            continue;
        }

        // MUBUF Raw / Atomic / LDS
        const uint16_t op = entry->raw_opcode;
        switch (op) {
            case 0x10: v[vdata] = ReadByte(buf.data(), buf.size(), addr); break;
            case 0x11: v[vdata] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(buf.data(), buf.size(), addr)))); break;
            case 0x12: v[vdata] = ReadU16(buf.data(), buf.size(), addr); break;
            case 0x13: v[vdata] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadU16(buf.data(), buf.size(), addr)))); break;
            case 0x14: v[vdata] = ReadU32(buf.data(), buf.size(), addr); break;
            case 0x15:
                v[vdata] = ReadU32(buf.data(), buf.size(), addr);
                v[(vdata + 1u) & 63u] = ReadU32(buf.data(), buf.size(), addr + 4u);
                break;
            case 0x16:
                for (uint32_t k = 0; k < 3u; ++k) v[(vdata + k) & 63u] = ReadU32(buf.data(), buf.size(), addr + k * 4u);
                break;
            case 0x17:
                for (uint32_t k = 0; k < 4u; ++k) v[(vdata + k) & 63u] = ReadU32(buf.data(), buf.size(), addr + k * 4u);
                break;
            case 0x18: WriteByte(buf.data(), buf.size(), addr, static_cast<uint8_t>(v[vdata] & 0xFFu)); break;
            case 0x19: WriteByte(buf.data(), buf.size(), addr, static_cast<uint8_t>((v[vdata] >> 16) & 0xFFu)); break;
            case 0x1A: WriteU16(buf.data(), buf.size(), addr, static_cast<uint16_t>(v[vdata] & 0xFFFFu)); break;
            case 0x1B: WriteU16(buf.data(), buf.size(), addr, static_cast<uint16_t>((v[vdata] >> 16) & 0xFFFFu)); break;
            case 0x1C: WriteU32(buf.data(), buf.size(), addr, v[vdata]); break;
            case 0x1D:
                WriteU32(buf.data(), buf.size(), addr, v[vdata]);
                WriteU32(buf.data(), buf.size(), addr + 4u, v[(vdata + 1u) & 63u]);
                break;
            case 0x1E:
                for (uint32_t k = 0; k < 3u; ++k) WriteU32(buf.data(), buf.size(), addr + k * 4u, v[(vdata + k) & 63u]);
                break;
            case 0x1F:
                for (uint32_t k = 0; k < 4u; ++k) WriteU32(buf.data(), buf.size(), addr + k * 4u, v[(vdata + k) & 63u]);
                break;
            case 0x20: v[vdata] = (v[vdata] & 0xFFFF0000u) | ReadByte(buf.data(), buf.size(), addr); break;
            case 0x21: v[vdata] = (v[vdata] & 0x0000FFFFu) | (static_cast<uint32_t>(ReadByte(buf.data(), buf.size(), addr)) << 16); break;
            case 0x22: {
                const uint16_t s = static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(ReadByte(buf.data(), buf.size(), addr))));
                v[vdata] = (v[vdata] & 0xFFFF0000u) | s;
                break;
            }
            case 0x23: {
                const uint16_t s = static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(ReadByte(buf.data(), buf.size(), addr))));
                v[vdata] = (v[vdata] & 0x0000FFFFu) | (static_cast<uint32_t>(s) << 16);
                break;
            }
            case 0x24: v[vdata] = (v[vdata] & 0xFFFF0000u) | ReadU16(buf.data(), buf.size(), addr); break;
            case 0x25: v[vdata] = (v[vdata] & 0x0000FFFFu) | (static_cast<uint32_t>(ReadU16(buf.data(), buf.size(), addr)) << 16); break;
            case 0x70: {
                // buffer_store_lds_dword: read dword from LDS at m0 + lane*4, store to buffer
                const uint32_t lds_addr = (state.m0 + lane * 4u) & 0xFFFFu;
                WriteU32(buf.data(), buf.size(), addr, ReadU32(state.lds.data(), state.lds.size(), lds_addr));
                break;
            }
            default: {
                if (op >= 0x30u && op <= 0x4Du) {
                    uint32_t cur = ReadU32(buf.data(), buf.size(), addr);
                    const uint32_t prev = ExecAtomic32(cur, op, v[vdata], v[(vdata + 1u) & 63u]);
                    WriteU32(buf.data(), buf.size(), addr, cur);
                    if (inst.glc) v[vdata] = prev;
                } else if (op >= 0x50u && op <= 0x60u) {
                    uint64_t cur = ReadU64(buf.data(), buf.size(), addr);
                    const uint64_t src0 = static_cast<uint64_t>(v[vdata]) |
                                          (static_cast<uint64_t>(v[(vdata + 1u) & 63u]) << 32);
                    const uint64_t src1 = static_cast<uint64_t>(v[(vdata + 2u) & 63u]) |
                                          (static_cast<uint64_t>(v[(vdata + 3u) & 63u]) << 32);
                    const uint64_t prev = ExecAtomic64(cur, op, src0, src1);
                    WriteU64(buf.data(), buf.size(), addr, cur);
                    if (inst.glc) {
                        v[vdata] = static_cast<uint32_t>(prev & 0xFFFFFFFFull);
                        v[(vdata + 1u) & 63u] = static_cast<uint32_t>((prev >> 32) & 0xFFFFFFFFull);
                    }
                }
                break;
            }
        }
    }
}

void ExecMimgCpu(const MemImgDecodedInst& di, MemImgExecState& state) {
    const auto& inst = di.base;
    const auto* entry = di.entry;
    if (!entry) return;

    const uint8_t dim = inst.mimg_dim & 7u;
    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
    const uint8_t vdata = static_cast<uint8_t>((entry->is_store ? inst.src[0].reg_index : inst.dst.reg_index) & 63u);
    const uint8_t vaddr = static_cast<uint8_t>(((entry->is_store || entry->is_atomic) ? inst.src[1].reg_index : inst.src[0].reg_index) & 63u);
    const uint16_t op = entry->raw_opcode;
    const uint32_t lanes = std::min(state.wave_size, 64u);

    auto write_dmask = [&](std::array<uint32_t, 64>& v, const std::array<uint32_t, 4>& vals) {
        uint32_t dst_off = 0;
        for (uint32_t ch = 0; ch < 4u; ++ch) {
            if ((dmask & (1u << ch)) != 0u) {
                v[(vdata + dst_off) & 63u] = vals[ch];
                ++dst_off;
            }
        }
    };

    const MemImgTexture& src_tex = state.textures[dim];

    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (((state.exec_mask >> lane) & 1ull) == 0ull) continue;
        auto& v = state.vgpr[lane];

        if (entry->is_bvh) {
            // image_bvh_intersect_ray (0xE6) or image_bvh64_intersect_ray (0xE7)
            const uint32_t off_shift = (op == 0xE7u) ? 1u : 0u;
            const uint32_t node_ptr = v[vaddr & 63u];
            const float tmax = BitsToFloat(v[(vaddr + 1u + off_shift) & 63u]);
            const float ox = BitsToFloat(v[(vaddr + 2u + off_shift) & 63u]);
            const float oy = BitsToFloat(v[(vaddr + 3u + off_shift) & 63u]);
            const float oz = BitsToFloat(v[(vaddr + 4u + off_shift) & 63u]);
            const float dx = BitsToFloat(v[(vaddr + 5u + off_shift) & 63u]);
            const float dy = BitsToFloat(v[(vaddr + 6u + off_shift) & 63u]);
            const float dz = BitsToFloat(v[(vaddr + 7u + off_shift) & 63u]);

            auto intersect_tri = [&](const std::array<float, 3>& p0,
                                     const std::array<float, 3>& p1,
                                     const std::array<float, 3>& p2,
                                     uint32_t prim_id) {
                const float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
                const float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
                const float px = dy * e2z - dz * e2y;
                const float py = dz * e2x - dx * e2z;
                const float pz = dx * e2y - dy * e2x;
                const float det = e1x * px + e1y * py + e1z * pz;
                if (std::fabs(det) > 1e-8f) {
                    const float inv_det = 1.0f / det;
                    const float tx = ox - p0[0], ty = oy - p0[1], tz = oz - p0[2];
                    const float bu = (tx * px + ty * py + tz * pz) * inv_det;
                    const float qx = ty * e1z - tz * e1y;
                    const float qy = tz * e1x - tx * e1z;
                    const float qz = tx * e1y - ty * e1x;
                    const float bv = (dx * qx + dy * qy + dz * qz) * inv_det;
                    const float t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
                    if (bu >= 0.0f && bv >= 0.0f && (bu + bv) <= 1.0f && t >= 0.0f && t <= tmax) {
                        v[vdata & 63u] = FloatToBits(t);
                        v[(vdata + 1u) & 63u] = FloatToBits(bu);
                        v[(vdata + 2u) & 63u] = FloatToBits(bv);
                        v[(vdata + 3u) & 63u] = prim_id | 0x80000000u;
                        return;
                    }
                }
                v[vdata & 63u] = FloatToBits(tmax);
                v[(vdata + 1u) & 63u] = 0u;
                v[(vdata + 2u) & 63u] = 0u;
                v[(vdata + 3u) & 63u] = 0u;
            };

            if (node_ptr < state.bvh_nodes.size()) {
                const auto& node = state.bvh_nodes[node_ptr];
                if (node.node_type == 0u) {
                    intersect_tri(node.v0, node.v1, node.v2, node.primitive_id);
                } else {
                    // Box4 internal node: test 4 child AABBs and return hit child pointers
                    std::array<std::pair<float, uint32_t>, 4> hits{};
                    uint32_t hit_count = 0;
                    const float invx = (std::fabs(dx) > 1e-8f) ? (1.0f / dx) : 1e30f;
                    const float invy = (std::fabs(dy) > 1e-8f) ? (1.0f / dy) : 1e30f;
                    const float invz = (std::fabs(dz) > 1e-8f) ? (1.0f / dz) : 1e30f;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        if (node.child_ptrs[c] == 0xFFFFFFFFu) continue;
                        const auto& b = node.child_aabb[c];
                        const float tx0 = (b[0] - ox) * invx, tx1 = (b[3] - ox) * invx;
                        const float ty0 = (b[1] - oy) * invy, ty1 = (b[4] - oy) * invy;
                        const float tz0 = (b[2] - oz) * invz, tz1 = (b[5] - oz) * invz;
                        const float tmin = std::max({0.0f, std::min(tx0, tx1), std::min(ty0, ty1), std::min(tz0, tz1)});
                        const float tmx = std::min({tmax, std::max(tx0, tx1), std::max(ty0, ty1), std::max(tz0, tz1)});
                        if (tmin <= tmx) {
                            hits[hit_count++] = {tmin, node.child_ptrs[c]};
                        }
                    }
                    std::sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(hit_count),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
                    for (uint32_t c = 0; c < 4u; ++c) {
                        v[(vdata + c) & 63u] = (c < hit_count) ? hits[c].second : 0xFFFFFFFFu;
                    }
                }
            } else if (state.uav0.size() >= (static_cast<size_t>(node_ptr) + 1u) * 48u) {
                // Fallback flat triangle layout in uav0 (12 dwords = 48 bytes per triangle)
                const uint32_t base_off = node_ptr * 48u;
                std::array<float, 3> p0 = {
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 0u)),
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 4u)),
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 8u)),
                };
                std::array<float, 3> p1 = {
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 12u)),
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 16u)),
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 20u)),
                };
                std::array<float, 3> p2 = {
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 24u)),
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 28u)),
                    BitsToFloat(ReadU32(state.uav0.data(), state.uav0.size(), base_off + 32u)),
                };
                const uint32_t prim_id = ReadU32(state.uav0.data(), state.uav0.size(), base_off + 36u);
                intersect_tri(p0, p1, p2, prim_id);
            }
            continue;
        }

        if (op == 0x0Eu) {
            // image_get_resinfo
            const uint32_t mip = v[vaddr & 63u];
            uint32_t w = 0, h = 0, d = 0;
            const uint32_t num_mips = static_cast<uint32_t>(src_tex.mips.size());
            if (mip < src_tex.mips.size()) {
                w = src_tex.mips[mip].width;
                h = src_tex.mips[mip].height;
                d = src_tex.mips[mip].depth_or_layers;
            }
            write_dmask(v, {w, h, d, num_mips});
            continue;
        }
        if (op == 0x60u) {
            // image_get_lod
            const float lod = std::clamp(state.sampler.lod_bias, state.sampler.min_lod, state.sampler.max_lod);
            write_dmask(v, {FloatToBits(lod), FloatToBits(lod), 0u, 0u});
            continue;
        }

        if (entry->is_load && !entry->is_sample && !entry->is_gather) {
            // image_load*, image_msaa_load
            const bool has_mip = (op == 0x01u || op == 0x04u || op == 0x05u);
            const bool is_msaa = (op == 0xE0u || dim == kMimgDim2DMsaa || dim == kMimgDim2DArrayMsaa);
            uint32_t x = v[vaddr & 63u];
            uint32_t y = (dim == kMimgDim1D || dim == kMimgDim1DArray) ? 0u : v[(vaddr + 1u) & 63u];
            uint32_t z = 0u;
            uint32_t extra_idx = (dim == kMimgDim1D) ? 1u : (dim == kMimgDim2D ? 2u : 3u);
            if (dim == kMimgDim1DArray) {
                z = v[(vaddr + 1u) & 63u];
                extra_idx = 2u;
            } else if (dim == kMimgDim3D || dim == kMimgDimCube || dim == kMimgDim2DArray || dim == kMimgDim2DArrayMsaa) {
                z = v[(vaddr + 2u) & 63u];
                extra_idx = 3u;
            } else if (dim == kMimgDim2DMsaa) {
                extra_idx = 2u;
            }
            const uint32_t mip = has_mip ? v[(vaddr + extra_idx) & 63u] : 0u;
            const uint32_t samp = is_msaa ? v[(vaddr + extra_idx) & 63u] : 0u;
            const auto rgba = src_tex.LoadRgba32f(x, y, z, mip, samp);
            write_dmask(v, {FloatToBits(rgba[0]), FloatToBits(rgba[1]), FloatToBits(rgba[2]), FloatToBits(rgba[3])});
            continue;
        }

        if (entry->is_store) {
            // image_store*
            const bool has_mip = (op == 0x09u || op == 0x0Bu);
            const uint32_t x = v[vaddr & 63u];
            const uint32_t y = (dim == kMimgDim1D || dim == kMimgDim1DArray) ? 0u : v[(vaddr + 1u) & 63u];
            uint32_t z = 0u;
            uint32_t mip_idx = 2u;
            if (dim == kMimgDim1D) mip_idx = 1u;
            else if (dim == kMimgDim1DArray) { z = v[(vaddr + 1u) & 63u]; mip_idx = 2u; }
            else if (dim == kMimgDim3D || dim == kMimgDimCube || dim == kMimgDim2DArray) { z = v[(vaddr + 2u) & 63u]; mip_idx = 3u; }
            const uint32_t mip = has_mip ? v[(vaddr + mip_idx) & 63u] : 0u;
            std::array<float, 4> rgba = {0.0f, 0.0f, 0.0f, 1.0f};
            uint32_t src_off = 0;
            for (uint32_t ch = 0; ch < 4u; ++ch) {
                if ((dmask & (1u << ch)) != 0u) {
                    rgba[ch] = BitsToFloat(v[(vdata + src_off) & 63u]);
                    ++src_off;
                }
            }
            state.rw_texture.StoreRgba32f(x, y, z, rgba, mip, 0u);
            continue;
        }

        if (entry->is_atomic) {
            const uint32_t x = v[vaddr & 63u];
            const uint32_t y = (dim == kMimgDim1D || dim == kMimgDim1DArray) ? 0u : v[(vaddr + 1u) & 63u];
            const uint32_t z = (dim == kMimgDim3D || dim == kMimgDimCube || dim == kMimgDim2DArray)
                                   ? v[(vaddr + 2u) & 63u]
                                   : ((dim == kMimgDim1DArray) ? v[(vaddr + 1u) & 63u] : 0u);
            uint32_t* ptr = state.rw_texture.MutableR32uPtr(x, y, z, 0u, 0u);
            if (ptr) {
                const uint32_t old = *ptr;
                const uint32_t src0 = v[vdata & 63u];
                const uint32_t src1 = v[(vdata + 1u) & 63u];
                switch (op) {
                    case 0x10: *ptr = src0; break;
                    case 0x11: if (old == src1) *ptr = src0; break;
                    case 0x12: *ptr = old + src0; break;
                    case 0x13: *ptr = old - src0; break;
                    case 0x14: *ptr = static_cast<uint32_t>(std::min(static_cast<int32_t>(old), static_cast<int32_t>(src0))); break;
                    case 0x15: *ptr = std::min(old, src0); break;
                    case 0x16: *ptr = static_cast<uint32_t>(std::max(static_cast<int32_t>(old), static_cast<int32_t>(src0))); break;
                    case 0x17: *ptr = std::max(old, src0); break;
                    case 0x18: *ptr = old & src0; break;
                    case 0x19: *ptr = old | src0; break;
                    case 0x1A: *ptr = old ^ src0; break;
                    case 0x1B: *ptr = (old >= src0) ? 0u : (old + 1u); break;
                    case 0x1C: *ptr = (old == 0u || old > src0) ? src0 : (old - 1u); break;
                    case 0x1D: if (BitsToFloat(old) == BitsToFloat(src1)) *ptr = src0; break;
                    case 0x1E: *ptr = FloatToBits(std::fmin(BitsToFloat(old), BitsToFloat(src0))); break;
                    case 0x1F: *ptr = FloatToBits(std::fmax(BitsToFloat(old), BitsToFloat(src0))); break;
                    default: break;
                }
                if (inst.glc) v[vdata & 63u] = old;
            }
            continue;
        }

        if (entry->is_sample || entry->is_gather) {
            const uint8_t low_nib = static_cast<uint8_t>(op & 0x0Fu);
            const bool is_cmp = (low_nib >= 0x08u);
            const bool has_off = ((op & 0x10u) != 0u);
            const uint8_t mode = low_nib & 0x07u;
            const bool has_bias = (mode == 5u || mode == 6u);
            const bool has_lod = (mode == 4u);
            const bool has_grad = (mode == 2u || mode == 3u);

            uint32_t aidx = vaddr;
            int32_t off_u = 0, off_v = 0;
            if (has_off) {
                const uint32_t packed_off = v[aidx & 63u];
                off_u = SignExtend(packed_off & 0x3Fu, 6u);
                off_v = SignExtend((packed_off >> 8) & 0x3Fu, 6u);
                ++aidx;
            }
            float bias = 0.0f;
            if (has_bias) {
                bias = BitsToFloat(v[aidx & 63u]);
                ++aidx;
            }
            float dref = 0.0f;
            if (is_cmp) {
                dref = BitsToFloat(v[aidx & 63u]);
                ++aidx;
            }
            if (has_grad) {
                aidx += (op >= 0xA0u) ? 2u : 4u;  // skip gradients
            }
            float u = BitsToFloat(v[aidx & 63u]);
            float vv = (dim == kMimgDim1D || dim == kMimgDim1DArray) ? 0.0f : BitsToFloat(v[(aidx + 1u) & 63u]);
            float ww = 0.0f;
            uint32_t coord_cnt = (dim == kMimgDim1D) ? 1u : (dim == kMimgDim2D ? 2u : 3u);
            if (dim == kMimgDim1DArray) { ww = BitsToFloat(v[(aidx + 1u) & 63u]); coord_cnt = 2u; }
            else if (dim == kMimgDim3D || dim == kMimgDimCube || dim == kMimgDim2DArray) {
                ww = BitsToFloat(v[(aidx + 2u) & 63u]);
                coord_cnt = 3u;
            }
            if (has_off && !src_tex.mips.empty()) {
                u += static_cast<float>(off_u) / static_cast<float>(std::max(1u, src_tex.mips[0].width));
                vv += static_cast<float>(off_v) / static_cast<float>(std::max(1u, src_tex.mips[0].height));
            }
            float lod = state.sampler.lod_bias + bias;
            if (has_lod) {
                lod = BitsToFloat(v[(aidx + coord_cnt) & 63u]);
            } else if (mode == 7u) {
                lod = 0.0f;  // _lz
            }

            if (entry->is_gather) {
                uint32_t ch = 0u;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if ((dmask & (1u << c)) != 0u) { ch = c; break; }
                }
                auto g = src_tex.Gather4(u, vv, ww, ch, inst.mimg_unrm);
                if (is_cmp) {
                    for (uint32_t k = 0; k < 4u; ++k) {
                        g[k] = EvalCompare(state.sampler.compare_func, dref, g[k]) ? 1.0f : 0.0f;
                    }
                }
                v[vdata & 63u] = FloatToBits(g[0]);
                v[(vdata + 1u) & 63u] = FloatToBits(g[1]);
                v[(vdata + 2u) & 63u] = FloatToBits(g[2]);
                v[(vdata + 3u) & 63u] = FloatToBits(g[3]);
            } else {
                auto rgba = src_tex.Sample(u, vv, ww, lod, state.sampler.linear_filter, inst.mimg_unrm);
                if (is_cmp) {
                    const float cmp_r = EvalCompare(state.sampler.compare_func, dref, rgba[0]) ? 1.0f : 0.0f;
                    rgba = {cmp_r, cmp_r, cmp_r, cmp_r};
                }
                write_dmask(v, {FloatToBits(rgba[0]), FloatToBits(rgba[1]), FloatToBits(rgba[2]), FloatToBits(rgba[3])});
            }
        }
    }
}

void ExecDsCpu(const MemImgDecodedInst& di, MemImgExecState& state) {
    const auto& inst = di.base;
    const auto* entry = di.entry;
    if (!entry) return;

    const uint16_t op = entry->raw_opcode;
    if (op == 0x14u) return;  // ds_nop
    if (op >= 0x18u && op <= 0x1Du) {
        ++state.gws_barrier_count;
        return;
    }

    const uint8_t vdst = static_cast<uint8_t>(inst.dst.reg_index & 63u);
    const uint8_t vaddr = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
    const uint8_t vdata0 = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
    const uint8_t vdata1 = static_cast<uint8_t>(inst.src[2].reg_index & 63u);
    const uint32_t off16 = static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu;
    const uint32_t lanes = std::min(state.wave_size, 64u);

    // Cross-lane instructions (wave-wide snapshot first)
    if (op == 0x35u) {
        // ds_swizzle_b32
        std::array<uint32_t, 64> snap{};
        for (uint32_t l = 0; l < lanes; ++l) snap[l] = state.vgpr[l][vaddr];
        for (uint32_t l = 0; l < lanes; ++l) {
            if (((state.exec_mask >> l) & 1ull) == 0ull) continue;
            uint32_t src_lane = l;
            if ((off16 & 0x8000u) != 0u) {
                const uint32_t quad_base = l & ~3u;
                const uint32_t sel = (off16 >> ((l & 3u) * 2u)) & 3u;
                src_lane = quad_base | sel;
            } else {
                const uint32_t and_m = off16 & 0x1Fu;
                const uint32_t or_m = (off16 >> 5) & 0x1Fu;
                const uint32_t xor_m = (off16 >> 10) & 0x1Fu;
                src_lane = (l & ~31u) | ((((l & 31u) & and_m) | or_m) ^ xor_m);
            }
            state.vgpr[l][vdst] = (src_lane < lanes) ? snap[src_lane] : 0u;
        }
        return;
    }
    if (op == 0xB2u) {
        // ds_permute_b32 (forward scatter: dst_lane = addr >> 2)
        std::array<uint32_t, 64> out_vals{};
        for (uint32_t l = 0; l < lanes; ++l) {
            if (((state.exec_mask >> l) & 1ull) == 0ull) continue;
            const uint32_t target_lane = (state.vgpr[l][vaddr] >> 2) & (lanes - 1u);
            out_vals[target_lane] = state.vgpr[l][vdata0];
        }
        for (uint32_t l = 0; l < lanes; ++l) {
            if (((state.exec_mask >> l) & 1ull) == 0ull) continue;
            state.vgpr[l][vdst] = out_vals[l];
        }
        return;
    }
    if (op == 0xB3u) {
        // ds_bpermute_b32 (backward gather: src_lane = addr >> 2)
        std::array<uint32_t, 64> snap{};
        for (uint32_t l = 0; l < lanes; ++l) snap[l] = state.vgpr[l][vdata0];
        for (uint32_t l = 0; l < lanes; ++l) {
            if (((state.exec_mask >> l) & 1ull) == 0ull) continue;
            const uint32_t src_lane = (state.vgpr[l][vaddr] >> 2) & (lanes - 1u);
            state.vgpr[l][vdst] = snap[src_lane];
        }
        return;
    }

    auto lds_addr = [&](uint32_t base, uint32_t add_bytes) -> uint32_t {
        return (base + add_bytes) % static_cast<uint32_t>(std::max<size_t>(state.lds.size(), 1u));
    };

    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (((state.exec_mask >> lane) & 1ull) == 0ull) continue;
        auto& v = state.vgpr[lane];
        const uint32_t base_a = v[vaddr];
        const uint32_t a = lds_addr(base_a, off16);
        uint8_t* mem = state.lds.data();
        const size_t sz = state.lds.size();

        switch (op) {
            // Ordered / append / consume
            case 0x3D: v[vdst] = (state.append_consume_counter > 0u) ? (--state.append_consume_counter) : 0u; break;
            case 0x3E: v[vdst] = state.append_consume_counter++; break;
            case 0x3F: v[vdst] = state.append_consume_counter++; break;
            // Loads
            case 0x36: v[vdst] = ReadU32(mem, sz, a); break;
            case 0x37:
                v[vdst] = ReadU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 4u));
                v[(vdst + 1u) & 63u] = ReadU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 4u));
                break;
            case 0x38:
                v[vdst] = ReadU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 256u));
                v[(vdst + 1u) & 63u] = ReadU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 256u));
                break;
            case 0x39: v[vdst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(mem, sz, a)))); break;
            case 0x3A: v[vdst] = ReadByte(mem, sz, a); break;
            case 0x3B: v[vdst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadU16(mem, sz, a)))); break;
            case 0x3C: v[vdst] = ReadU16(mem, sz, a); break;
            case 0x76:
                v[vdst] = ReadU32(mem, sz, a);
                v[(vdst + 1u) & 63u] = ReadU32(mem, sz, lds_addr(a, 4u));
                break;
            case 0x77: {
                const uint32_t a0 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 8u);
                const uint32_t a1 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 8u);
                v[vdst] = ReadU32(mem, sz, a0);
                v[(vdst + 1u) & 63u] = ReadU32(mem, sz, lds_addr(a0, 4u));
                v[(vdst + 2u) & 63u] = ReadU32(mem, sz, a1);
                v[(vdst + 3u) & 63u] = ReadU32(mem, sz, lds_addr(a1, 4u));
                break;
            }
            case 0x78: {
                const uint32_t a0 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 512u);
                const uint32_t a1 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 512u);
                v[vdst] = ReadU32(mem, sz, a0);
                v[(vdst + 1u) & 63u] = ReadU32(mem, sz, lds_addr(a0, 4u));
                v[(vdst + 2u) & 63u] = ReadU32(mem, sz, a1);
                v[(vdst + 3u) & 63u] = ReadU32(mem, sz, lds_addr(a1, 4u));
                break;
            }
            case 0xFE:
                for (uint32_t k = 0; k < 3u; ++k) v[(vdst + k) & 63u] = ReadU32(mem, sz, lds_addr(a, k * 4u));
                break;
            case 0xFF:
                for (uint32_t k = 0; k < 4u; ++k) v[(vdst + k) & 63u] = ReadU32(mem, sz, lds_addr(a, k * 4u));
                break;
            case 0xA2: v[vdst] = (v[vdst] & 0xFFFF0000u) | ReadByte(mem, sz, a); break;
            case 0xA3: v[vdst] = (v[vdst] & 0x0000FFFFu) | (static_cast<uint32_t>(ReadByte(mem, sz, a)) << 16); break;
            case 0xA4: v[vdst] = (v[vdst] & 0xFFFF0000u) | static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(ReadByte(mem, sz, a)))); break;
            case 0xA5: v[vdst] = (v[vdst] & 0x0000FFFFu) | (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(ReadByte(mem, sz, a))))) << 16); break;
            case 0xA6: v[vdst] = (v[vdst] & 0xFFFF0000u) | ReadU16(mem, sz, a); break;
            case 0xA7: v[vdst] = (v[vdst] & 0x0000FFFFu) | (static_cast<uint32_t>(ReadU16(mem, sz, a)) << 16); break;
            // Stores
            case 0x0D: WriteU32(mem, sz, a, v[vdata0]); break;
            case 0x0E:
                WriteU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 4u), v[vdata0]);
                WriteU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 4u), v[vdata1]);
                break;
            case 0x0F:
                WriteU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 256u), v[vdata0]);
                WriteU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 256u), v[vdata1]);
                break;
            case 0x1E: WriteByte(mem, sz, a, static_cast<uint8_t>(v[vdata0] & 0xFFu)); break;
            case 0x1F: WriteU16(mem, sz, a, static_cast<uint16_t>(v[vdata0] & 0xFFFFu)); break;
            case 0xA0: WriteByte(mem, sz, a, static_cast<uint8_t>((v[vdata0] >> 16) & 0xFFu)); break;
            case 0xA1: WriteU16(mem, sz, a, static_cast<uint16_t>((v[vdata0] >> 16) & 0xFFFFu)); break;
            case 0x4D:
                WriteU32(mem, sz, a, v[vdata0]);
                WriteU32(mem, sz, lds_addr(a, 4u), v[(vdata0 + 1u) & 63u]);
                break;
            case 0x4E: {
                const uint32_t a0 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 8u);
                const uint32_t a1 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 8u);
                WriteU32(mem, sz, a0, v[vdata0]);
                WriteU32(mem, sz, lds_addr(a0, 4u), v[(vdata0 + 1u) & 63u]);
                WriteU32(mem, sz, a1, v[vdata1]);
                WriteU32(mem, sz, lds_addr(a1, 4u), v[(vdata1 + 1u) & 63u]);
                break;
            }
            case 0x4F: {
                const uint32_t a0 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 512u);
                const uint32_t a1 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 512u);
                WriteU32(mem, sz, a0, v[vdata0]);
                WriteU32(mem, sz, lds_addr(a0, 4u), v[(vdata0 + 1u) & 63u]);
                WriteU32(mem, sz, a1, v[vdata1]);
                WriteU32(mem, sz, lds_addr(a1, 4u), v[(vdata1 + 1u) & 63u]);
                break;
            }
            case 0x8D: WriteU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 4u),
                                ReadU32(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 4u))); break;
            case 0xCD: WriteU64(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * 8u),
                                ReadU64(mem, sz, lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * 8u))); break;
            case 0xDE:
                for (uint32_t k = 0; k < 3u; ++k) WriteU32(mem, sz, lds_addr(a, k * 4u), v[(vdata0 + k) & 63u]);
                break;
            case 0xDF:
                for (uint32_t k = 0; k < 4u; ++k) WriteU32(mem, sz, lds_addr(a, k * 4u), v[(vdata0 + k) & 63u]);
                break;
            // 32-bit atomics (non-_rtn and _rtn)
            default: {
                const bool rtn = entry->has_rtn;
                if ((op <= 0x15u) || (op >= 0x20u && op <= 0x33u) || op == 0x55u) {
                    const uint16_t base_op = (op == 0x55u) ? 0x15u : ((op >= 0x20u) ? static_cast<uint16_t>(op - 0x20u) : op);
                    if (op == 0x2Eu || op == 0x2Fu) {
                        const uint32_t scale = (op == 0x2Fu) ? 256u : 4u;
                        const uint32_t a0 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * scale);
                        const uint32_t a1 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * scale);
                        const uint32_t o0 = ReadU32(mem, sz, a0);
                        const uint32_t o1 = ReadU32(mem, sz, a1);
                        WriteU32(mem, sz, a0, v[vdata0]);
                        WriteU32(mem, sz, a1, v[vdata1]);
                        v[vdst] = o0;
                        v[(vdst + 1u) & 63u] = o1;
                        break;
                    }
                    uint32_t cur = ReadU32(mem, sz, a);
                    const uint32_t old = cur;
                    const uint32_t d0 = v[vdata0];
                    const uint32_t d1 = v[vdata1];
                    switch (base_op) {
                        case 0x00: cur = old + d0; break;
                        case 0x01: cur = old - d0; break;
                        case 0x02: cur = d0 - old; break;
                        case 0x03: cur = (old >= d0) ? 0u : (old + 1u); break;
                        case 0x04: cur = (old == 0u || old > d0) ? d0 : (old - 1u); break;
                        case 0x05: cur = static_cast<uint32_t>(std::min(static_cast<int32_t>(old), static_cast<int32_t>(d0))); break;
                        case 0x06: cur = static_cast<uint32_t>(std::max(static_cast<int32_t>(old), static_cast<int32_t>(d0))); break;
                        case 0x07: cur = std::min(old, d0); break;
                        case 0x08: cur = std::max(old, d0); break;
                        case 0x09: cur = old & d0; break;
                        case 0x0A: cur = old | d0; break;
                        case 0x0B: cur = old ^ d0; break;
                        case 0x0C: cur = (old & ~d0) | d1; break;
                        case 0x0D: cur = d0; break;  // ds_wrxchg_rtn_b32
                        case 0x10: if (old == d0) cur = d1; break;
                        case 0x11: if (BitsToFloat(old) == BitsToFloat(d0)) cur = d1; break;
                        case 0x12: cur = FloatToBits(std::fmin(BitsToFloat(old), BitsToFloat(d0))); break;
                        case 0x13: cur = FloatToBits(std::fmax(BitsToFloat(old), BitsToFloat(d0))); break;
                        case 0x15: cur = FloatToBits(BitsToFloat(old) + BitsToFloat(d0)); break;
                        default: break;
                    }
                    WriteU32(mem, sz, a, cur);
                    if (rtn) v[vdst] = old;
                } else if ((op >= 0x40u && op <= 0x53u) || (op >= 0x60u && op <= 0x73u) ||
                           op == 0x7Eu || op == 0xB0u || op == 0xB1u) {
                    if (op == 0x6Eu || op == 0x6Fu) {
                        const uint32_t scale = (op == 0x6Fu) ? 512u : 8u;
                        const uint32_t a0 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset0) * scale);
                        const uint32_t a1 = lds_addr(base_a, static_cast<uint32_t>(inst.ds_offset1) * scale);
                        const uint64_t o0 = ReadU64(mem, sz, a0);
                        const uint64_t o1 = ReadU64(mem, sz, a1);
                        const uint64_t n0 = static_cast<uint64_t>(v[vdata0]) | (static_cast<uint64_t>(v[(vdata0 + 1u) & 63u]) << 32);
                        const uint64_t n1 = static_cast<uint64_t>(v[vdata1]) | (static_cast<uint64_t>(v[(vdata1 + 1u) & 63u]) << 32);
                        WriteU64(mem, sz, a0, n0);
                        WriteU64(mem, sz, a1, n1);
                        v[vdst] = static_cast<uint32_t>(o0 & 0xFFFFFFFFull);
                        v[(vdst + 1u) & 63u] = static_cast<uint32_t>(o0 >> 32);
                        v[(vdst + 2u) & 63u] = static_cast<uint32_t>(o1 & 0xFFFFFFFFull);
                        v[(vdst + 3u) & 63u] = static_cast<uint32_t>(o1 >> 32);
                        break;
                    }
                    uint64_t cur = ReadU64(mem, sz, a);
                    const uint64_t old = cur;
                    const uint64_t d0 = static_cast<uint64_t>(v[vdata0]) | (static_cast<uint64_t>(v[(vdata0 + 1u) & 63u]) << 32);
                    const uint64_t d1 = static_cast<uint64_t>(v[vdata1]) | (static_cast<uint64_t>(v[(vdata1 + 1u) & 63u]) << 32);
                    if (op == 0x7Eu) {
                        // ds_condxchg32_rtn_b64: compare low 32 bits against 0xFFFFFFFF (or M0) and exchange
                        const uint32_t lo = static_cast<uint32_t>(old & 0xFFFFFFFFull);
                        const uint32_t hi = static_cast<uint32_t>(old >> 32);
                        if (hi == static_cast<uint32_t>(d0 >> 32)) cur = d0;
                        (void)lo;
                    } else if (op == 0xB0u) {
                        cur = old + d0;
                    } else if (op == 0xB1u) {
                        cur = old - d0;
                    } else {
                        const uint16_t base_op = (op >= 0x60u) ? static_cast<uint16_t>(op - 0x60u) : static_cast<uint16_t>(op - 0x40u);
                        switch (base_op) {
                            case 0x00: cur = old + d0; break;
                            case 0x01: cur = old - d0; break;
                            case 0x02: cur = d0 - old; break;
                            case 0x03: cur = (old >= d0) ? 0ull : (old + 1ull); break;
                            case 0x04: cur = (old == 0ull || old > d0) ? d0 : (old - 1ull); break;
                            case 0x05: cur = static_cast<uint64_t>(std::min(static_cast<int64_t>(old), static_cast<int64_t>(d0))); break;
                            case 0x06: cur = static_cast<uint64_t>(std::max(static_cast<int64_t>(old), static_cast<int64_t>(d0))); break;
                            case 0x07: cur = std::min(old, d0); break;
                            case 0x08: cur = std::max(old, d0); break;
                            case 0x09: cur = old & d0; break;
                            case 0x0A: cur = old | d0; break;
                            case 0x0B: cur = old ^ d0; break;
                            case 0x0C: cur = (old & ~d0) | d1; break;
                            case 0x0D: cur = d0; break;
                            case 0x10: if (old == d0) cur = d1; break;
                            case 0x11: if (BitsToDouble(old) == BitsToDouble(d0)) cur = d1; break;
                            case 0x12: cur = DoubleToBits(std::fmin(BitsToDouble(old), BitsToDouble(d0))); break;
                            case 0x13: cur = DoubleToBits(std::fmax(BitsToDouble(old), BitsToDouble(d0))); break;
                            default: break;
                        }
                    }
                    WriteU64(mem, sz, a, cur);
                    if (rtn) {
                        v[vdst] = static_cast<uint32_t>(old & 0xFFFFFFFFull);
                        v[(vdst + 1u) & 63u] = static_cast<uint32_t>(old >> 32);
                    }
                }
                break;
            }
        }
    }
}

void ExecFlatCpu(const MemImgDecodedInst& di, MemImgExecState& state) {
    const auto& inst = di.base;
    const auto* entry = di.entry;
    if (!entry) return;

    const uint16_t op = entry->raw_opcode;
    const uint8_t seg = inst.flat_seg;
    const uint8_t vdst = static_cast<uint8_t>(inst.dst.reg_index & 63u);
    const uint8_t vaddr = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
    const uint8_t vdata = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
    const uint32_t lanes = std::min(state.wave_size, 64u);

    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (((state.exec_mask >> lane) & 1ull) == 0ull) continue;
        auto& v = state.vgpr[lane];
        int64_t raw_addr = static_cast<int64_t>(v[vaddr]) + static_cast<int64_t>(inst.mem_offset);
        if (inst.saddr < 106u) {
            raw_addr += static_cast<int64_t>(state.sgpr[inst.saddr]);
        }
        const uint32_t addr = (raw_addr < 0) ? 0u : static_cast<uint32_t>(raw_addr);

        std::vector<uint8_t>& buf = (seg == 1u)
                                        ? state.scratch[lane]
                                        : ((inst.saddr != 0x7Fu && inst.saddr != 0u) ? state.uav1 : state.uav0);
        uint8_t* mem = buf.data();
        const size_t sz = buf.size();

        switch (op) {
            case 0x08: v[vdst] = ReadByte(mem, sz, addr); break;
            case 0x09: v[vdst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(mem, sz, addr)))); break;
            case 0x0A: v[vdst] = ReadU16(mem, sz, addr); break;
            case 0x0B: v[vdst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadU16(mem, sz, addr)))); break;
            case 0x0C: v[vdst] = ReadU32(mem, sz, addr); break;
            case 0x0D:
                v[vdst] = ReadU32(mem, sz, addr);
                v[(vdst + 1u) & 63u] = ReadU32(mem, sz, addr + 4u);
                break;
            case 0x0E:
                for (uint32_t k = 0; k < 4u; ++k) v[(vdst + k) & 63u] = ReadU32(mem, sz, addr + k * 4u);
                break;
            case 0x0F:
                for (uint32_t k = 0; k < 3u; ++k) v[(vdst + k) & 63u] = ReadU32(mem, sz, addr + k * 4u);
                break;
            case 0x18: WriteByte(mem, sz, addr, static_cast<uint8_t>(v[vdata] & 0xFFu)); break;
            case 0x19: WriteByte(mem, sz, addr, static_cast<uint8_t>((v[vdata] >> 16) & 0xFFu)); break;
            case 0x1A: WriteU16(mem, sz, addr, static_cast<uint16_t>(v[vdata] & 0xFFFFu)); break;
            case 0x1B: WriteU16(mem, sz, addr, static_cast<uint16_t>((v[vdata] >> 16) & 0xFFFFu)); break;
            case 0x1C: WriteU32(mem, sz, addr, v[vdata]); break;
            case 0x1D:
                WriteU32(mem, sz, addr, v[vdata]);
                WriteU32(mem, sz, addr + 4u, v[(vdata + 1u) & 63u]);
                break;
            case 0x1E:
                for (uint32_t k = 0; k < 4u; ++k) WriteU32(mem, sz, addr + k * 4u, v[(vdata + k) & 63u]);
                break;
            case 0x1F:
                for (uint32_t k = 0; k < 3u; ++k) WriteU32(mem, sz, addr + k * 4u, v[(vdata + k) & 63u]);
                break;
            case 0x20: v[vdst] = (v[vdst] & 0xFFFF0000u) | ReadByte(mem, sz, addr); break;
            case 0x21: v[vdst] = (v[vdst] & 0x0000FFFFu) | (static_cast<uint32_t>(ReadByte(mem, sz, addr)) << 16); break;
            case 0x22: v[vdst] = (v[vdst] & 0xFFFF0000u) | static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(ReadByte(mem, sz, addr)))); break;
            case 0x23: v[vdst] = (v[vdst] & 0x0000FFFFu) | (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(ReadByte(mem, sz, addr))))) << 16); break;
            case 0x24: v[vdst] = (v[vdst] & 0xFFFF0000u) | ReadU16(mem, sz, addr); break;
            case 0x25: v[vdst] = (v[vdst] & 0x0000FFFFu) | (static_cast<uint32_t>(ReadU16(mem, sz, addr)) << 16); break;
            default: {
                if (op >= 0x30u && op <= 0x4Du) {
                    uint32_t cur = ReadU32(mem, sz, addr);
                    const uint32_t prev = ExecAtomic32(cur, op, v[vdata], v[(vdata + 1u) & 63u]);
                    WriteU32(mem, sz, addr, cur);
                    if (inst.glc) v[vdst] = prev;
                } else if (op >= 0x50u && op <= 0x60u) {
                    uint64_t cur = ReadU64(mem, sz, addr);
                    const uint64_t src0 = static_cast<uint64_t>(v[vdata]) |
                                          (static_cast<uint64_t>(v[(vdata + 1u) & 63u]) << 32);
                    const uint64_t src1 = static_cast<uint64_t>(v[(vdata + 2u) & 63u]) |
                                          (static_cast<uint64_t>(v[(vdata + 3u) & 63u]) << 32);
                    const uint64_t prev = ExecAtomic64(cur, op, src0, src1);
                    WriteU64(mem, sz, addr, cur);
                    if (inst.glc) {
                        v[vdst] = static_cast<uint32_t>(prev & 0xFFFFFFFFull);
                        v[(vdst + 1u) & 63u] = static_cast<uint32_t>(prev >> 32);
                    }
                }
                break;
            }
        }
    }
}

void ExecExpCpu(const MemImgDecodedInst& di, MemImgExecState& state) {
    const auto& inst = di.base;
    const uint32_t lanes = std::min(state.wave_size, 64u);
    const uint8_t target = inst.exp_target;
    const uint8_t en = inst.exp_en;
    const bool compr = inst.exp_compr;

    for (uint32_t lane = 0; lane < lanes; ++lane) {
        if (((state.exec_mask >> lane) & 1ull) == 0ull) continue;
        const auto& v = state.vgpr[lane];
        auto& ex = state.exports[lane];
        ex.last_compr = compr;
        ex.last_vm = inst.exp_vm;
        if (inst.exp_done) ex.done_seen = true;

        std::array<float, 4> fvals = {0.0f, 0.0f, 0.0f, 0.0f};
        std::array<uint32_t, 4> uvals = {0u, 0u, 0u, 0u};
        if (compr) {
            const uint32_t w0 = v[inst.src[0].reg_index & 63u];
            const uint32_t w1 = v[inst.src[1].reg_index & 63u];
            if ((en & 1u) != 0u) fvals[0] = HalfBitsToFloat(static_cast<uint16_t>(w0 & 0xFFFFu));
            if ((en & 2u) != 0u) fvals[1] = HalfBitsToFloat(static_cast<uint16_t>((w0 >> 16) & 0xFFFFu));
            if ((en & 4u) != 0u) fvals[2] = HalfBitsToFloat(static_cast<uint16_t>(w1 & 0xFFFFu));
            if ((en & 8u) != 0u) fvals[3] = HalfBitsToFloat(static_cast<uint16_t>((w1 >> 16) & 0xFFFFu));
            for (uint32_t c = 0; c < 4u; ++c) uvals[c] = FloatToBits(fvals[c]);
        } else {
            for (uint32_t c = 0; c < 4u; ++c) {
                if ((en & (1u << c)) != 0u) {
                    uvals[c] = v[inst.src[c].reg_index & 63u];
                    fvals[c] = BitsToFloat(uvals[c]);
                }
            }
        }

        if (target <= 7u) {
            ex.mrt[target] = fvals;
            ex.mrt_written_mask |= static_cast<uint8_t>(1u << target);
        } else if (target == 8u) {
            ex.mrtz = fvals[0];
            ex.mrtz_written = true;
        } else if (target == 9u) {
            ex.null_exported = true;
        } else if (target >= 12u && target <= 15u) {
            const uint8_t idx = static_cast<uint8_t>(target - 12u);
            ex.pos[idx] = fvals;
            ex.pos_written_mask |= static_cast<uint8_t>(1u << idx);
        } else if (target == 20u) {
            ex.prim = uvals;
            ex.prim_written = true;
        } else if (target >= 32u && target <= 63u) {
            const uint8_t idx = static_cast<uint8_t>(target - 32u);
            ex.param[idx] = fvals;
            ex.param_written_mask |= (1u << idx);
        }
    }
}

Status ValidateMimgUnsupportedModifiers(const std::vector<MemImgDecodedInst>& insts) {
    for (const auto& di : insts) {
        if (di.base.encoding != Rdna2Encoding::MIMG) continue;
        const uint32_t w0 = (di.base.word_count > 0u) ? di.base.raw_words[0] : 0u;
        const bool has_r128 = di.r128 || (((w0 >> 15) & 1u) != 0u);
        const bool has_tfe = di.tfe || (((w0 >> 6) & 1u) != 0u);
        const bool has_lwe = di.lwe || (((w0 >> 17) & 1u) != 0u);
        if (!has_r128 && !has_tfe && !has_lwe) continue;
        std::string mods;
        auto add_mod = [&](const char* name) {
            if (!mods.empty()) mods += ", ";
            mods += name;
        };
        if (has_r128) add_mod("R128");
        if (has_tfe) add_mod("TFE");
        if (has_lwe) add_mod("LWE");
        const char* mnem = di.entry ? di.entry->mnemonic : "image_unknown";
        return Status::Error(
            StatusCode::UnsupportedFormat,
            std::string("Unsupported MIMG modifier(s) [") + mods + "] on opcode " + mnem +
                " at word_offset=" + std::to_string(di.base.word_offset) +
                " (R128 4-dword T#, TFE residency return, and LWE LOD warning are refused)");
    }
    return Status::Ok();
}

}  // namespace

Status ExecuteMemImgCpu(
    const std::vector<MemImgDecodedInst>& insts,
    MemImgExecState& state) {
    if (auto st = ValidateMimgUnsupportedModifiers(insts); !st.ok()) {
        return st;
    }
    if (state.wave_size == 0u || state.wave_size > 64u) {
        return Status::Error(StatusCode::InvalidArgument, "MemImgExecState wave_size must be in 1..64");
    }
    if (state.lds.empty()) {
        state.lds.assign(65536u, 0u);
    }
    for (const auto& di : insts) {
        if (di.base.encoding == Rdna2Encoding::SOPP) {
            if (di.base.opcode == Rdna2Opcode::S_ENDPGM) break;
            continue;
        }
        if (!di.entry) {
            return Status::Error(StatusCode::UnsupportedFormat, "Decoded MemImg instruction missing opcode entry");
        }
        switch (di.base.encoding) {
            case Rdna2Encoding::MUBUF:
            case Rdna2Encoding::MTBUF:
                ExecMubufOrMtbufCpu(di, state);
                break;
            case Rdna2Encoding::MIMG:
                ExecMimgCpu(di, state);
                break;
            case Rdna2Encoding::DS:
                ExecDsCpu(di, state);
                break;
            case Rdna2Encoding::FLAT:
                ExecFlatCpu(di, state);
                break;
            case Rdna2Encoding::EXP:
                ExecExpCpu(di, state);
                break;
            default:
                return Status::Error(StatusCode::UnsupportedFormat, "Unsupported encoding in ExecuteMemImgCpu");
        }
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// SM 6.5 HLSL Emitter for MemImg / DS / FLAT / EXP Shaders
// ---------------------------------------------------------------------------
namespace {

struct MemImgResourceUsage {
    bool uses_uav0 = false;
    bool uses_uav1 = false;
    bool uses_lds = false;
    bool uses_wave_tmp = false;
    bool uses_ds_counter = false;
    bool uses_scratch = false;
    bool uses_tex_dim[8] = {};
    bool uses_samp = false;
    bool uses_samp_cmp = false;
    bool uses_rwtex_f4 = false;
    bool uses_rwtex_u32 = false;
    bool uses_bvh = false;
    bool uses_fmt = false;
    bool uses_subdword = false;
    bool uses_atomic_helpers = false;
    bool uses_exp_pos0 = false;
    bool uses_exp_mrtz = false;
    uint8_t exp_mrt_mask = 0;
    uint32_t exp_param_mask = 0;
    bool uses_exp_prim = false;
};

MemImgResourceUsage AnalyzeResourceUsage(
    const Xs2ShaderContainer& container,
    const std::vector<MemImgDecodedInst>& insts) {
    MemImgResourceUsage u{};
    for (const auto& di : insts) {
        if (!di.entry) continue;
        const auto& inst = di.base;
        const auto* e = di.entry;
        switch (inst.encoding) {
            case Rdna2Encoding::MUBUF:
            case Rdna2Encoding::MTBUF: {
                if (inst.srsrc == 0u) u.uses_uav0 = true;
                else u.uses_uav1 = true;
                if (inst.encoding == Rdna2Encoding::MTBUF ||
                    e->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferLoadFormat) ||
                    e->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferStoreFormat)) {
                    u.uses_fmt = true;
                    u.uses_subdword = true;
                }
                if (e->raw_opcode == 0x10u || e->raw_opcode == 0x11u || e->raw_opcode == 0x12u ||
                    e->raw_opcode == 0x13u || e->raw_opcode == 0x18u || e->raw_opcode == 0x19u ||
                    e->raw_opcode == 0x1Au || e->raw_opcode == 0x1Bu ||
                    (e->raw_opcode >= 0x20u && e->raw_opcode <= 0x25u)) {
                    u.uses_subdword = true;
                }
                if (e->raw_opcode == 0x70u) u.uses_lds = true;
                if (e->is_atomic) u.uses_atomic_helpers = true;
                break;
            }
            case Rdna2Encoding::MIMG: {
                if (e->is_bvh) {
                    u.uses_bvh = true;
                    u.uses_uav0 = true;
                } else if (e->is_store) {
                    u.uses_rwtex_f4 = true;
                } else if (e->is_atomic) {
                    u.uses_rwtex_u32 = true;
                } else {
                    const uint8_t d = inst.mimg_dim & 7u;
                    u.uses_tex_dim[d] = true;
                    if (d == kMimgDimCube) u.uses_samp = true;
                    if (e->is_sample || e->is_gather || e->raw_opcode == 0x60u) {
                        const uint8_t low = static_cast<uint8_t>(e->raw_opcode & 0x0Fu);
                        if ((e->is_sample || e->is_gather) && low >= 0x08u) u.uses_samp_cmp = true;
                        else u.uses_samp = true;
                    }
                }
                break;
            }
            case Rdna2Encoding::DS: {
                if (e->raw_opcode == 0x35u || e->raw_opcode == 0xB2u || e->raw_opcode == 0xB3u) {
                    u.uses_wave_tmp = true;
                } else if (e->raw_opcode == 0x3Du || e->raw_opcode == 0x3Eu || e->raw_opcode == 0x3Fu) {
                    u.uses_ds_counter = true;
                } else if (e->raw_opcode != 0x14u && !(e->raw_opcode >= 0x18u && e->raw_opcode <= 0x1Du)) {
                    u.uses_lds = true;
                }
                break;
            }
            case Rdna2Encoding::FLAT: {
                if (inst.flat_seg == 1u) {
                    u.uses_scratch = true;
                } else {
                    if (inst.saddr != 0x7Fu && inst.saddr != 0u) u.uses_uav1 = true;
                    else u.uses_uav0 = true;
                    if (e->is_atomic) u.uses_atomic_helpers = true;
                }
                if (e->raw_opcode == 0x08u || e->raw_opcode == 0x09u || e->raw_opcode == 0x0Au ||
                    e->raw_opcode == 0x0Bu || e->raw_opcode == 0x18u || e->raw_opcode == 0x19u ||
                    e->raw_opcode == 0x1Au || e->raw_opcode == 0x1Bu ||
                    (e->raw_opcode >= 0x20u && e->raw_opcode <= 0x25u)) {
                    u.uses_subdword = true;
                }
                break;
            }
            case Rdna2Encoding::EXP: {
                const uint8_t tgt = inst.exp_target;
                if (tgt <= 7u) u.exp_mrt_mask |= static_cast<uint8_t>(1u << tgt);
                else if (tgt == 8u) u.uses_exp_mrtz = true;
                else if (tgt >= 12u && tgt <= 15u) u.uses_exp_pos0 = true;
                else if (tgt == 20u) u.uses_exp_prim = true;
                else if (tgt >= 32u && tgt <= 63u) u.exp_param_mask |= (1u << (tgt - 32u));
                break;
            }
            default:
                break;
        }
    }
    if (container.stage == ShaderStage::Compute && !u.uses_uav0 && !u.uses_uav1 && !u.uses_rwtex_f4 && !u.uses_rwtex_u32) {
        u.uses_uav0 = true;
    }
    return u;
}

const char* TexRegSlotName(uint8_t dim) noexcept {
    switch (dim & 7u) {
        case kMimgDim2D:         return "_tex2d";
        case kMimgDim3D:         return "_tex3d";
        case kMimgDimCube:       return "_texcube";
        case kMimgDim2DArray:    return "_tex2darr";
        case kMimgDim1D:         return "_tex1d";
        case kMimgDim1DArray:    return "_tex1darr";
        case kMimgDim2DMsaa:     return "_tex2dms";
        case kMimgDim2DArrayMsaa:return "_tex2darrms";
        default:                 return "_tex2d";
    }
}

uint32_t TexRegSlotIndex(uint8_t dim) noexcept {
    switch (dim & 7u) {
        case kMimgDim2D:          return 0u;
        case kMimgDim3D:          return 1u;
        case kMimgDimCube:        return 2u;
        case kMimgDim2DArray:     return 3u;
        case kMimgDim1D:          return 4u;
        case kMimgDim1DArray:     return 5u;
        case kMimgDim2DMsaa:      return 6u;
        case kMimgDim2DArrayMsaa: return 7u;
        default:                  return 0u;
    }
}

std::string HlslBufAddrExpr(const Rdna2Instruction& inst, uint8_t vaddr) {
    std::ostringstream oss;
    oss << "(" << inst.mem_offset << "u";
    if (inst.soffset >= 128u && inst.soffset <= 192u) {
        const uint32_t imm = static_cast<uint32_t>(inst.soffset - 128u);
        if (imm != 0u) oss << " + " << imm << "u";
    }
    if (inst.idxen && inst.offen) {
        oss << " + v[" << static_cast<uint32_t>(vaddr) << "] * 16u + v["
            << static_cast<uint32_t>((vaddr + 1u) & 63u) << "]";
    } else if (inst.idxen) {
        oss << " + v[" << static_cast<uint32_t>(vaddr) << "] * 16u";
    } else if (inst.offen) {
        oss << " + v[" << static_cast<uint32_t>(vaddr) << "]";
    }
    oss << ")";
    return oss.str();
}

void EmitHlslAtomicOp(
    std::ostringstream& os,
    const char* buf_name,
    const std::string& addr_expr,
    uint16_t op,
    uint8_t vdata,
    uint8_t vdst,
    bool write_rtn,
    bool is_lds) {
    os << "    {\n";
    if (is_lds) {
        os << "        uint _idx = ((" << addr_expr << ") >> 2u) & 4095u;\n";
    } else {
        os << "        uint _addr = (" << addr_expr << ") & ~3u;\n";
    }
    os << "        uint _prev = 0u;\n";
    auto target_lhs = [&]() -> std::string {
        return is_lds ? "_lds[_idx]" : std::string(buf_name);
    };
    auto emit_cas_loop = [&](const std::string& next_expr, const std::string& extra_check = "") {
        os << "        uint _cur = " << (is_lds ? "_lds[_idx]" : (std::string(buf_name) + ".Load(_addr)")) << ";\n";
        os << "        [allow_uav_condition] for (uint _it = 0u; _it < 64u; ++_it) {\n";
        if (!extra_check.empty()) {
            os << "            if (!(" << extra_check << ")) { _prev = _cur; break; }\n";
        }
        os << "            uint _next = " << next_expr << ";\n";
        os << "            uint _orig = 0u;\n";
        if (is_lds) {
            os << "            InterlockedCompareExchange(_lds[_idx], _cur, _next, _orig);\n";
        } else {
            os << "            " << buf_name << ".InterlockedCompareExchange(_addr, _cur, _next, _orig);\n";
        }
        os << "            if (_orig == _cur) { _prev = _orig; break; }\n";
        os << "            _cur = _orig;\n";
        os << "        }\n";
    };

    const uint32_t d0 = vdata & 63u;
    const uint32_t d1 = (vdata + 1u) & 63u;
    switch (op) {
        case 0x30:
            if (is_lds) os << "        InterlockedExchange(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedExchange(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x31:
            if (is_lds) os << "        InterlockedCompareExchange(" << target_lhs() << ", v[" << d1 << "], v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedCompareExchange(_addr, v[" << d1 << "], v[" << d0 << "], _prev);\n";
            break;
        case 0x32:
            if (is_lds) os << "        InterlockedAdd(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedAdd(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x33:
            // LDS: a CAS loop, not InterlockedAdd of the negated value. NVIDIA's D3D12 driver (621.24, RTX 5070)
            // fails CreateComputePipelineState with E_OUTOFMEMORY on a groupshared atomic add whose operand is a
            // runtime negation (DXC folds every spelling of -x to `sub 0, x`); UAV atomics are not affected.
            if (is_lds) emit_cas_loop("_cur - v[" + std::to_string(d0) + "]");
            else os << "        " << buf_name << ".InterlockedAdd(_addr, (0u - v[" << d0 << "]), _prev);\n";
            break;
        case 0x34:
            emit_cas_loop("(_cur >= v[" + std::to_string(d0) + "]) ? (_cur - v[" + std::to_string(d0) + "]) : 0u");
            break;
        case 0x35:
            emit_cas_loop("asuint(min(asint(_cur), asint(v[" + std::to_string(d0) + "])))");
            break;
        case 0x36:
            if (is_lds) os << "        InterlockedMin(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedMin(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x37:
            emit_cas_loop("asuint(max(asint(_cur), asint(v[" + std::to_string(d0) + "])))");
            break;
        case 0x38:
            if (is_lds) os << "        InterlockedMax(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedMax(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x39:
            if (is_lds) os << "        InterlockedAnd(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedAnd(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x3A:
            if (is_lds) os << "        InterlockedOr(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedOr(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x3B:
            if (is_lds) os << "        InterlockedXor(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedXor(_addr, v[" << d0 << "], _prev);\n";
            break;
        case 0x3C:
            emit_cas_loop("(_cur >= v[" + std::to_string(d0) + "]) ? 0u : (_cur + 1u)");
            break;
        case 0x3D:
            emit_cas_loop("(_cur == 0u || _cur > v[" + std::to_string(d0) + "]) ? v[" + std::to_string(d0) + "] : (_cur - 1u)");
            break;
        case 0x3E:
            emit_cas_loop("v[" + std::to_string(d0) + "]", "asfloat(_cur) == asfloat(v[" + std::to_string(d1) + "])");
            break;
        case 0x3F:
            emit_cas_loop("asuint(min(asfloat(_cur), asfloat(v[" + std::to_string(d0) + "])))");
            break;
        case 0x40:
            emit_cas_loop("asuint(max(asfloat(_cur), asfloat(v[" + std::to_string(d0) + "])))");
            break;
        case 0x4D:
            emit_cas_loop("asuint(asfloat(_cur) + asfloat(v[" + std::to_string(d0) + "]))");
            break;
        case 0x4E:
            // ds_rsub_u32: mem = data0 - old_mem
            emit_cas_loop("(v[" + std::to_string(d0) + "] - _cur)");
            break;
        case 0x52: {
            // 64-bit carry-propagating atomic add (buffer_atomic_add_x2 / global_atomic_add_x2 / ds_add_u64)
            os << "        uint _prev_hi = 0u;\n";
            if (is_lds) {
                os << "        uint _idx_hi = (_idx + 1u) & 4095u;\n";
                os << "        InterlockedAdd(_lds[_idx], v[" << d0 << "], _prev);\n";
                os << "        uint _carry = ((_prev + v[" << d0 << "]) < _prev) ? 1u : 0u;\n";
                os << "        InterlockedAdd(_lds[_idx_hi], v[" << d1 << "] + _carry, _prev_hi);\n";
            } else {
                os << "        " << buf_name << ".InterlockedAdd(_addr, v[" << d0 << "], _prev);\n";
                os << "        uint _carry = ((_prev + v[" << d0 << "]) < _prev) ? 1u : 0u;\n";
                os << "        " << buf_name << ".InterlockedAdd(_addr + 4u, v[" << d1 << "] + _carry, _prev_hi);\n";
            }
            if (write_rtn) {
                os << "        v[" << static_cast<uint32_t>(vdst & 63u) << "] = _prev;\n";
                os << "        v[" << static_cast<uint32_t>((vdst + 1u) & 63u) << "] = _prev_hi;\n";
            }
            os << "    }\n";
            return;
        }
        default:
            // 64-bit _x2 emulation on low/high dwords
            if (is_lds) os << "        InterlockedAdd(" << target_lhs() << ", v[" << d0 << "], _prev);\n";
            else os << "        " << buf_name << ".InterlockedAdd(_addr, v[" << d0 << "], _prev);\n";
            break;
    }
    if (write_rtn) {
        os << "        v[" << static_cast<uint32_t>(vdst & 63u) << "] = _prev;\n";
    }
    os << "    }\n";
}

Status EmitMemImgHlsl(
    const Xs2ShaderContainer& container,
    const std::vector<MemImgDecodedInst>& insts,
    const MemImgResourceUsage& u,
    std::string& out_hlsl,
    std::string& out_target,
    bool use_sm65) {
    const char* sm = use_sm65 ? "6_5" : "5_0";
    switch (container.stage) {
        case ShaderStage::Vertex:        out_target = std::string("vs_") + sm; break;
        case ShaderStage::Pixel:         out_target = std::string("ps_") + sm; break;
        case ShaderStage::Compute:
        default:                         out_target = std::string("cs_") + sm; break;
    }

    std::ostringstream os;
    os << "// Generated by OpenScarlett GFX10.3 Mem/Img/LDS/FLAT/EXP Lifter\n";
    if (u.uses_uav0) os << "RWByteAddressBuffer _uav0 : register(u0);\n";
    if (u.uses_uav1) os << "RWByteAddressBuffer _uav1 : register(u1);\n";
    if (u.uses_rwtex_f4) os << "RWTexture2D<float4> _rwtex : register(u2);\n";
    if (u.uses_rwtex_u32) os << "RWTexture2D<uint> _rwtex_u32 : register(u3);\n";

    if (u.uses_tex_dim[kMimgDim2D]) os << "Texture2D<float4> _tex2d : register(t0);\n";
    if (u.uses_tex_dim[kMimgDim3D]) os << "Texture3D<float4> _tex3d : register(t1);\n";
    if (u.uses_tex_dim[kMimgDimCube]) os << "TextureCube<float4> _texcube : register(t2);\n";
    if (u.uses_tex_dim[kMimgDim2DArray]) os << "Texture2DArray<float4> _tex2darr : register(t3);\n";
    if (u.uses_tex_dim[kMimgDim1D]) os << "Texture1D<float4> _tex1d : register(t4);\n";
    if (u.uses_tex_dim[kMimgDim1DArray]) os << "Texture1DArray<float4> _tex1darr : register(t5);\n";
    if (u.uses_tex_dim[kMimgDim2DMsaa]) os << "Texture2DMS<float4> _tex2dms : register(t6);\n";
    if (u.uses_tex_dim[kMimgDim2DArrayMsaa]) os << "Texture2DMSArray<float4> _tex2darrms : register(t7);\n";
    if (u.uses_samp) os << "SamplerState _samp0 : register(s0);\n";
    if (u.uses_samp_cmp) os << "SamplerComparisonState _samp_cmp : register(s1);\n";

    const bool is_compute = (container.stage == ShaderStage::Compute);
    if (is_compute && u.uses_lds) os << "groupshared uint _lds[4096];\n";
    if (is_compute && u.uses_wave_tmp) os << "groupshared uint _wave_tmp[64];\n";
    if (is_compute && u.uses_ds_counter) os << "groupshared uint _ds_counter;\n";

    // Sub-dword byte/short helpers on RWByteAddressBuffer
    if (u.uses_subdword || u.uses_fmt) {
        auto emit_buf_helpers = [&](const char* name, const char* tag) {
            os << "uint _load_u8_" << tag << "(uint a) { uint w = " << name << ".Load(a & ~3u); return (w >> ((a & 3u) * 8u)) & 0xFFu; }\n";
            os << "uint _load_u16_" << tag << "(uint a) {\n"
                  "    uint b0 = _load_u8_" << tag << "(a); uint b1 = _load_u8_" << tag << "(a + 1u);\n"
                  "    return b0 | (b1 << 8u);\n"
                  "}\n";
            os << "void _store_u8_" << tag << "(uint a, uint val) {\n"
                  "    uint sh = (a & 3u) * 8u; uint m = ~(0xFFu << sh);\n"
                  "    uint old = 0u; " << name << ".InterlockedAnd(a & ~3u, m, old);\n"
                  "    " << name << ".InterlockedOr(a & ~3u, (val & 0xFFu) << sh, old);\n"
                  "}\n";
            os << "void _store_u16_" << tag << "(uint a, uint val) {\n"
                  "    _store_u8_" << tag << "(a, val & 0xFFu);\n"
                  "    _store_u8_" << tag << "(a + 1u, (val >> 8u) & 0xFFu);\n"
                  "}\n";
        };
        if (u.uses_uav0) emit_buf_helpers("_uav0", "u0");
        if (u.uses_uav1) emit_buf_helpers("_uav1", "u1");
    }

    if (u.uses_fmt) {
        os << "uint _os_unpack_fmt(uint raw, uint fmt, bool d16) {\n"
              "    if (fmt == 1u || fmt == 10u || fmt == 28u) { float f = (float)(raw & 0xFFu) / 255.0; return d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f); }\n"
              "    if (fmt == 2u || fmt == 11u || fmt == 29u) { int s = ((int)((raw & 0xFFu) << 24u)) >> 24; float f = clamp((float)s / 127.0, -1.0, 1.0); return d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f); }\n"
              "    if (fmt == 3u || fmt == 12u || fmt == 30u) return raw & 0xFFu;\n"
              "    if (fmt == 4u || fmt == 13u || fmt == 31u) { uint s = asuint(((int)((raw & 0xFFu) << 24u)) >> 24); return d16 ? (s & 0xFFFFu) : s; }\n"
              "    if (fmt == 5u || fmt == 17u || fmt == 47u) { float f = (float)(raw & 0xFFFFu) / 65535.0; return d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f); }\n"
              "    if (fmt == 6u || fmt == 18u || fmt == 48u) { int s = ((int)((raw & 0xFFFFu) << 16u)) >> 16; float f = clamp((float)s / 32767.0, -1.0, 1.0); return d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f); }\n"
              "    if (fmt == 7u || fmt == 19u || fmt == 49u) return raw & 0xFFFFu;\n"
              "    if (fmt == 8u || fmt == 20u || fmt == 50u) { uint s = asuint(((int)((raw & 0xFFFFu) << 16u)) >> 16); return d16 ? (s & 0xFFFFu) : s; }\n"
              "    if (fmt == 9u || fmt == 21u || fmt == 51u) return d16 ? (raw & 0xFFFFu) : asuint(f16tof32(raw & 0xFFFFu));\n"
              "    if (fmt == 14u || fmt == 15u) return d16 ? (raw & 0xFFFFu) : raw;\n"
              "    return d16 ? (f32tof16(asfloat(raw)) & 0xFFFFu) : raw;\n"
              "}\n"
              "float _os_rne_f32(float x) {\n"
              "    float fl = floor(x); float diff = x - fl;\n"
              "    if (diff < 0.5) return fl;\n"
              "    if (diff > 0.5) return fl + 1.0;\n"
              "    float h = fl * 0.5;\n"
              "    return (floor(h) == h) ? fl : (fl + 1.0);\n"
              "}\n"
              "uint _os_pack_fmt(uint val, uint fmt, bool from_d16) {\n"
              "    float f = from_d16 ? f16tof32(val & 0xFFFFu) : asfloat(val);\n"
              "    uint u = from_d16 ? (val & 0xFFFFu) : val;\n"
              "    if (fmt == 1u || fmt == 10u || fmt == 28u) { float c = isnan(f) ? 0.0 : clamp(f, 0.0, 1.0); return (uint)clamp((int)_os_rne_f32(c * 255.0), 0, 255); }\n"
              "    if (fmt == 2u || fmt == 11u || fmt == 29u) { float c = isnan(f) ? 0.0 : clamp(f, -1.0, 1.0); return asuint(clamp((int)_os_rne_f32(c * 127.0), -127, 127)) & 0xFFu; }\n"
              "    if (fmt == 3u || fmt == 12u || fmt == 30u || fmt == 4u || fmt == 13u || fmt == 31u) return u & 0xFFu;\n"
              "    if (fmt == 5u || fmt == 17u || fmt == 47u) { float c = isnan(f) ? 0.0 : clamp(f, 0.0, 1.0); return (uint)clamp((int)_os_rne_f32(c * 65535.0), 0, 65535); }\n"
              "    if (fmt == 6u || fmt == 18u || fmt == 48u) { float c = isnan(f) ? 0.0 : clamp(f, -1.0, 1.0); return asuint(clamp((int)_os_rne_f32(c * 32767.0), -32767, 32767)) & 0xFFFFu; }\n"
              "    if (fmt == 7u || fmt == 19u || fmt == 49u || fmt == 8u || fmt == 20u || fmt == 50u) return u & 0xFFFFu;\n"
              "    if (fmt == 9u || fmt == 21u || fmt == 51u) return from_d16 ? (val & 0xFFFFu) : (f32tof16(f) & 0xFFFFu);\n"
              "    if (fmt == 14u || fmt == 15u) return u;\n"
              "    return asuint(f);\n"
              "}\n";
    }

    if (u.uses_bvh) {
        os << "uint4 _os_bvh_intersect(uint node_ptr, float tmax, float3 orig, float3 dir) {\n"
              "    uint base = node_ptr * 48u;\n"
              "    float3 p0 = float3(asfloat(_uav0.Load(base+0u)), asfloat(_uav0.Load(base+4u)), asfloat(_uav0.Load(base+8u)));\n"
              "    float3 p1 = float3(asfloat(_uav0.Load(base+12u)), asfloat(_uav0.Load(base+16u)), asfloat(_uav0.Load(base+20u)));\n"
              "    float3 p2 = float3(asfloat(_uav0.Load(base+24u)), asfloat(_uav0.Load(base+28u)), asfloat(_uav0.Load(base+32u)));\n"
              "    uint prim_id = _uav0.Load(base+36u);\n"
              "    float3 e1 = p1 - p0; float3 e2 = p2 - p0;\n"
              "    float3 p = cross(dir, e2);\n"
              "    float det = dot(e1, p);\n"
              "    if (abs(det) > 1e-8) {\n"
              "        float inv_det = 1.0 / det;\n"
              "        float3 tv = orig - p0;\n"
              "        float bu = dot(tv, p) * inv_det;\n"
              "        float3 q = cross(tv, e1);\n"
              "        float bv = dot(dir, q) * inv_det;\n"
              "        float t = dot(e2, q) * inv_det;\n"
              "        if (bu >= 0.0 && bv >= 0.0 && (bu + bv) <= 1.0 && t >= 0.0 && t <= tmax) {\n"
              "            return uint4(asuint(t), asuint(bu), asuint(bv), prim_id | 0x80000000u);\n"
              "        }\n"
              "    }\n"
              "    return uint4(asuint(tmax), 0u, 0u, 0u);\n"
              "}\n";
    }

    if (container.stage == ShaderStage::Vertex) {
        os << "struct VSOut {\n";
        os << "    float4 pos : SV_Position;\n";
        for (uint32_t p = 0; p < 8u; ++p) {
            if ((u.exp_param_mask & (1u << p)) != 0u) {
                os << "    float4 param" << p << " : TEXCOORD" << p << ";\n";
            }
        }
        os << "};\n";
        os << "VSOut main(uint vid : SV_VertexID) {\n";
        os << "    VSOut _out = (VSOut)0;\n";
        os << "    _out.pos = float4(0.0, 0.0, 0.0, 1.0);\n";
        os << "    uint tid = vid;\n";
    } else if (container.stage == ShaderStage::Pixel) {
        os << "struct PSOut {\n";
        uint8_t mask = u.exp_mrt_mask ? u.exp_mrt_mask : 1u;
        for (uint32_t m = 0; m < 8u; ++m) {
            if ((mask & (1u << m)) != 0u) {
                os << "    float4 mrt" << m << " : SV_Target" << m << ";\n";
            }
        }
        if (u.uses_exp_mrtz) os << "    float depth : SV_Depth;\n";
        os << "};\n";
        os << "PSOut main(float4 sv_pos : SV_Position) {\n";
        os << "    PSOut _out = (PSOut)0;\n";
        os << "    uint tid = (uint)sv_pos.x;\n";
    } else {
        const uint32_t tx = std::max(1u, container.threadgroup_x);
        const uint32_t ty = std::max(1u, container.threadgroup_y);
        const uint32_t tz = std::max(1u, container.threadgroup_z);
        os << "[numthreads(" << tx << ", " << ty << ", " << tz << ")]\n";
        os << "void main(uint3 dtid : SV_DispatchThreadID, uint gidx : SV_GroupIndex) {\n";
        os << "    uint tid = dtid.x;\n";
    }

    os << "    uint v[64];\n";
    os << "    [unroll] for (uint _i = 0u; _i < 64u; ++_i) v[_i] = 0u;\n";
    os << "    v[0] = tid;\n";
    os << "    v[1] = tid * 4u;\n";
    os << "    v[2] = tid * 16u;\n";
    os << "    v[3] = tid * 8u;\n";
    if (u.uses_scratch) {
        os << "    uint _scratch[64];\n";
        os << "    [unroll] for (uint _s = 0u; _s < 64u; ++_s) _scratch[_s] = 0u;\n";
    }
    if (is_compute && (u.uses_lds || u.uses_ds_counter)) {
        if (u.uses_lds) {
            const uint32_t tg_total = std::max(1u, container.threadgroup_x) *
                                      std::max(1u, container.threadgroup_y) *
                                      std::max(1u, container.threadgroup_z);
            os << "    for (uint _li = gidx; _li < 4096u; _li += " << tg_total << "u) _lds[_li] = 0u;\n";
        }
        if (u.uses_ds_counter) {
            os << "    if (gidx == 0u) _ds_counter = 0u;\n";
        }
        os << "    GroupMemoryBarrierWithGroupSync();\n";
    }

    for (const auto& di : insts) {
        const auto& inst = di.base;
        if (inst.encoding == Rdna2Encoding::SOPP) {
            if (inst.opcode == Rdna2Opcode::S_BARRIER && is_compute) {
                os << "    GroupMemoryBarrierWithGroupSync();\n";
            } else if (inst.opcode == Rdna2Opcode::S_ENDPGM) {
                break;
            }
            continue;
        }
        if (!di.entry) continue;
        const auto* e = di.entry;
        const uint16_t op = e->raw_opcode;

        if (inst.encoding == Rdna2Encoding::MUBUF || inst.encoding == Rdna2Encoding::MTBUF) {
            if (inst.encoding == Rdna2Encoding::MUBUF && (op == 0x71u || op == 0x72u)) {
                os << "    DeviceMemoryBarrier();\n";
                continue;
            }
            const char* bname = (inst.srsrc == 0u) ? "_uav0" : "_uav1";
            const char* btag = (inst.srsrc == 0u) ? "u0" : "u1";
            const uint8_t vd = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t va = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const std::string aexpr = HlslBufAddrExpr(inst, va);

            if (inst.encoding == Rdna2Encoding::MTBUF ||
                e->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferLoadFormat) ||
                e->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferStoreFormat)) {
                const uint32_t fmt = (inst.encoding == Rdna2Encoding::MTBUF)
                                         ? static_cast<uint32_t>(inst.mtbuf_format)
                                         : static_cast<uint32_t>(Rdna2MtbufFormat::R32G32B32A32_FLOAT);
                const uint32_t cbytes = MtbufFormatComponentBytes(static_cast<Rdna2MtbufFormat>(fmt));
                const char* load_fn = (cbytes == 1u) ? "_load_u8_" : ((cbytes == 2u) ? "_load_u16_" : "");
                if (op <= 0x03u) {
                    const uint32_t comps = static_cast<uint32_t>(op) + 1u;
                    os << "    { uint _ba = " << aexpr << ";\n";
                    for (uint32_t c = 0; c < comps; ++c) {
                        std::string raw_expr = (cbytes == 4u)
                                                   ? (std::string(bname) + ".Load((_ba + " + std::to_string(c * 4u) + "u) & ~3u)")
                                                   : (std::string(load_fn) + btag + "(_ba + " + std::to_string(c * cbytes) + "u)");
                        os << "      v[" << ((vd + c) & 63u) << "] = _os_unpack_fmt(" << raw_expr << ", " << fmt << "u, false);\n";
                    }
                    os << "    }\n";
                } else if (op >= 0x08u && op <= 0x0Bu) {
                    const uint32_t comps = static_cast<uint32_t>(op - 0x08u) + 1u;
                    os << "    { uint _ba = " << aexpr << ";\n";
                    for (uint32_t c = 0; c < comps; ++c) {
                        std::string raw_expr = (cbytes == 4u)
                                                   ? (std::string(bname) + ".Load((_ba + " + std::to_string(c * 4u) + "u) & ~3u)")
                                                   : (std::string(load_fn) + btag + "(_ba + " + std::to_string(c * cbytes) + "u)");
                        os << "      uint _h" << c << " = _os_unpack_fmt(" << raw_expr << ", " << fmt << "u, true) & 0xFFFFu;\n";
                    }
                    if (comps == 1u) {
                        os << "      v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFF0000u) | _h0;\n";
                    } else {
                        os << "      v[" << static_cast<uint32_t>(vd) << "] = _h0 | (_h1 << 16u);\n";
                    }
                    if (comps == 3u) {
                        os << "      v[" << ((vd + 1u) & 63u) << "] = (v[" << ((vd + 1u) & 63u) << "] & 0xFFFF0000u) | _h2;\n";
                    } else if (comps == 4u) {
                        os << "      v[" << ((vd + 1u) & 63u) << "] = _h2 | (_h3 << 16u);\n";
                    }
                    os << "    }\n";
                } else if (op == 0x26u || op == 0x10u) {
                    std::string raw_expr = (cbytes == 4u)
                                               ? (std::string(bname) + ".Load((" + aexpr + ") & ~3u)")
                                               : (std::string(load_fn) + btag + "(" + aexpr + ")");
                    os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd)
                       << "] & 0xFFFFu) | ((_os_unpack_fmt(" << raw_expr << ", " << fmt << "u, true) & 0xFFFFu) << 16u);\n";
                } else {
                    // Format stores (standard 0x04..0x07, d16 0x0C..0x0F, d16_hi 0x11/0x27) with exact sub-dword partial writes
                    const bool is_d16_hi = (op == 0x11u || op == 0x27u);
                    const bool is_d16 = is_d16_hi || (op >= 0x0Cu && op <= 0x0Fu);
                    const uint32_t comps = (op >= 0x04u && op <= 0x07u)
                                               ? (static_cast<uint32_t>(op - 0x04u) + 1u)
                                               : ((op >= 0x0Cu && op <= 0x0Fu) ? (static_cast<uint32_t>(op - 0x0Cu) + 1u) : 1u);
                    os << "    { uint _ba = " << aexpr << ";\n";
                    for (uint32_t c = 0; c < comps; ++c) {
                        std::string src_expr;
                        if (is_d16_hi) {
                            src_expr = "(v[" + std::to_string(vd) + "] >> 16u)";
                        } else if (is_d16) {
                            const uint32_t r = (vd + (c >> 1u)) & 63u;
                            src_expr = ((c & 1u) == 0u) ? ("(v[" + std::to_string(r) + "] & 0xFFFFu)")
                                                        : ("(v[" + std::to_string(r) + "] >> 16u)");
                        } else {
                            src_expr = "v[" + std::to_string((vd + c) & 63u) + "]";
                        }
                        os << "      uint _pk" << c << " = _os_pack_fmt(" << src_expr << ", " << fmt << "u, "
                           << (is_d16 ? "true" : "false") << ");\n";
                        if (cbytes == 1u) {
                            os << "      _store_u8_" << btag << "(_ba + " << c << "u, _pk" << c << ");\n";
                        } else if (cbytes == 2u) {
                            os << "      _store_u16_" << btag << "(_ba + " << (c * 2u) << "u, _pk" << c << ");\n";
                        } else {
                            os << "      " << bname << ".Store((_ba + " << (c * 4u) << "u) & ~3u, _pk" << c << ");\n";
                        }
                    }
                    os << "    }\n";
                }
            } else if (e->is_atomic) {
                EmitHlslAtomicOp(os, bname, aexpr, op, vd, vd, inst.glc, false);
            } else {
                // Raw MUBUF loads & stores
                switch (op) {
                    case 0x10: os << "    v[" << static_cast<uint32_t>(vd) << "] = _load_u8_" << btag << "(" << aexpr << ");\n"; break;
                    case 0x11: os << "    v[" << static_cast<uint32_t>(vd) << "] = asuint(((int)(_load_u8_" << btag << "(" << aexpr << ") << 24u)) >> 24);\n"; break;
                    case 0x12: os << "    v[" << static_cast<uint32_t>(vd) << "] = _load_u16_" << btag << "(" << aexpr << ");\n"; break;
                    case 0x13: os << "    v[" << static_cast<uint32_t>(vd) << "] = asuint(((int)(_load_u16_" << btag << "(" << aexpr << ") << 16u)) >> 16);\n"; break;
                    case 0x14: os << "    v[" << static_cast<uint32_t>(vd) << "] = " << bname << ".Load((" << aexpr << ") & ~3u);\n"; break;
                    case 0x15:
                    case 0x16:
                    case 0x17: {
                        const uint32_t n = e->data_dwords;
                        os << "    { uint _ba = (" << aexpr << ") & ~3u;\n";
                        for (uint32_t k = 0; k < n; ++k) {
                            os << "      v[" << ((vd + k) & 63u) << "] = " << bname << ".Load(_ba + " << (k * 4u) << "u);\n";
                        }
                        os << "    }\n";
                        break;
                    }
                    case 0x18: os << "    _store_u8_" << btag << "(" << aexpr << ", v[" << static_cast<uint32_t>(vd) << "]);\n"; break;
                    case 0x19: os << "    _store_u8_" << btag << "(" << aexpr << ", v[" << static_cast<uint32_t>(vd) << "] >> 16u);\n"; break;
                    case 0x1A: os << "    _store_u16_" << btag << "(" << aexpr << ", v[" << static_cast<uint32_t>(vd) << "]);\n"; break;
                    case 0x1B: os << "    _store_u16_" << btag << "(" << aexpr << ", v[" << static_cast<uint32_t>(vd) << "] >> 16u);\n"; break;
                    case 0x1C: os << "    " << bname << ".Store((" << aexpr << ") & ~3u, v[" << static_cast<uint32_t>(vd) << "]);\n"; break;
                    case 0x1D:
                    case 0x1E:
                    case 0x1F: {
                        const uint32_t n = e->data_dwords;
                        os << "    { uint _ba = (" << aexpr << ") & ~3u;\n";
                        for (uint32_t k = 0; k < n; ++k) {
                            os << "      " << bname << ".Store(_ba + " << (k * 4u) << "u, v[" << ((vd + k) & 63u) << "]);\n";
                        }
                        os << "    }\n";
                        break;
                    }
                    case 0x20: os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFF0000u) | _load_u8_" << btag << "(" << aexpr << ");\n"; break;
                    case 0x21: os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFFu) | (_load_u8_" << btag << "(" << aexpr << ") << 16u);\n"; break;
                    case 0x22: os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFF0000u) | (asuint(((int)(_load_u8_" << btag << "(" << aexpr << ") << 24u)) >> 24) & 0xFFFFu);\n"; break;
                    case 0x23: os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFFu) | ((asuint(((int)(_load_u8_" << btag << "(" << aexpr << ") << 24u)) >> 24) & 0xFFFFu) << 16u);\n"; break;
                    case 0x24: os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFF0000u) | _load_u16_" << btag << "(" << aexpr << ");\n"; break;
                    case 0x25: os << "    v[" << static_cast<uint32_t>(vd) << "] = (v[" << static_cast<uint32_t>(vd) << "] & 0xFFFFu) | (_load_u16_" << btag << "(" << aexpr << ") << 16u);\n"; break;
                    case 0x70:
                        if (is_compute && u.uses_lds) {
                            os << "    " << bname << ".Store((" << aexpr << ") & ~3u, _lds[gidx & 4095u]);\n";
                        }
                        break;
                    default: break;
                }
            }
        } else if (inst.encoding == Rdna2Encoding::MIMG) {
            const uint8_t dim = inst.mimg_dim & 7u;
            const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
            const uint8_t vd = static_cast<uint8_t>((e->is_store ? inst.src[0].reg_index : inst.dst.reg_index) & 63u);
            const uint8_t va = static_cast<uint8_t>(((e->is_store || e->is_atomic) ? inst.src[1].reg_index : inst.src[0].reg_index) & 63u);
            const char* tname = TexRegSlotName(dim);

            if (e->is_bvh) {
                const uint32_t sh = (op == 0xE7u) ? 1u : 0u;
                os << "    { uint4 _bh = _os_bvh_intersect(v[" << static_cast<uint32_t>(va)
                   << "], asfloat(v[" << ((va + 1u + sh) & 63u) << "]), "
                   << "float3(asfloat(v[" << ((va + 2u + sh) & 63u) << "]), asfloat(v[" << ((va + 3u + sh) & 63u)
                   << "]), asfloat(v[" << ((va + 4u + sh) & 63u) << "])), "
                   << "float3(asfloat(v[" << ((va + 5u + sh) & 63u) << "]), asfloat(v[" << ((va + 6u + sh) & 63u)
                   << "]), asfloat(v[" << ((va + 7u + sh) & 63u) << "])));\n"
                   << "      v[" << static_cast<uint32_t>(vd) << "] = _bh.x; v[" << ((vd + 1u) & 63u)
                   << "] = _bh.y; v[" << ((vd + 2u) & 63u) << "] = _bh.z; v[" << ((vd + 3u) & 63u) << "] = _bh.w; }\n";
            } else if (op == 0x0Eu) {
                os << "    { uint _w=0u, _h=1u, _d=1u, _nm=1u;\n";
                if (dim == kMimgDim1D) os << "      " << tname << ".GetDimensions(v[" << static_cast<uint32_t>(va) << "], _w, _nm);\n";
                else if (dim == kMimgDim2D || dim == kMimgDimCube) os << "      " << tname << ".GetDimensions(v[" << static_cast<uint32_t>(va) << "], _w, _h, _nm);\n";
                else if (dim == kMimgDim2DMsaa) os << "      " << tname << ".GetDimensions(_w, _h, _nm);\n";
                else if (dim == kMimgDim2DArrayMsaa) os << "      " << tname << ".GetDimensions(_w, _h, _d, _nm);\n";
                else if (dim == kMimgDim1DArray) os << "      " << tname << ".GetDimensions(v[" << static_cast<uint32_t>(va) << "], _w, _d, _nm);\n";
                else os << "      " << tname << ".GetDimensions(v[" << static_cast<uint32_t>(va) << "], _w, _h, _d, _nm);\n";
                const char* rvals[4] = {"_w", "_h", "_d", "_nm"};
                uint32_t dst_off = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if ((dmask & (1u << c)) != 0u) {
                        os << "      v[" << ((vd + dst_off) & 63u) << "] = " << rvals[c] << ";\n";
                        ++dst_off;
                    }
                }
                os << "    }\n";
            } else if (op == 0x60u) {
                if (container.stage == ShaderStage::Pixel && dim == kMimgDim2D) {
                    os << "    { float _lod = " << tname << ".CalculateLevelOfDetail(_samp0, float2(asfloat(v["
                       << static_cast<uint32_t>(va) << "]), asfloat(v[" << ((va + 1u) & 63u) << "])));\n"
                       << "      v[" << static_cast<uint32_t>(vd) << "] = asuint(_lod); v[" << ((vd + 1u) & 63u)
                       << "] = asuint(_lod); }\n";
                } else {
                    uint32_t dst_off = 0u;
                    for (uint32_t c = 0u; c < 4u; ++c) {
                        if ((dmask & (1u << c)) != 0u) {
                            os << "    v[" << ((vd + dst_off) & 63u) << "] = 0u;\n";
                            ++dst_off;
                        }
                    }
                }
            } else if (e->is_store) {
                os << "    _rwtex[uint2(v[" << static_cast<uint32_t>(va) << "], v[" << ((va + 1u) & 63u)
                   << "])] = float4(asfloat(v[" << static_cast<uint32_t>(vd) << "]), asfloat(v[" << ((vd + 1u) & 63u)
                   << "]), asfloat(v[" << ((vd + 2u) & 63u) << "]), asfloat(v[" << ((vd + 3u) & 63u) << "]));\n";
            } else if (e->is_atomic) {
                os << "    { uint2 _ic = uint2(v[" << static_cast<uint32_t>(va) << "], v[" << ((va + 1u) & 63u) << "]); uint _ip = 0u;\n";
                auto emit_tex_cas = [&](const std::string& next_expr) {
                    os << "      uint _cur = _rwtex_u32[_ic];\n"
                       << "      [allow_uav_condition] for (uint _it = 0u; _it < 64u; ++_it) {\n"
                       << "          uint _next = " << next_expr << "; uint _orig = 0u;\n"
                       << "          InterlockedCompareExchange(_rwtex_u32[_ic], _cur, _next, _orig);\n"
                       << "          if (_orig == _cur) { _ip = _orig; break; }\n"
                       << "          _cur = _orig;\n"
                       << "      }\n";
                };
                switch (op) {
                    case 0x10: os << "      InterlockedExchange(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x11: os << "      InterlockedCompareExchange(_rwtex_u32[_ic], v[" << ((vd + 1u) & 63u) << "], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x12: os << "      InterlockedAdd(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x13: os << "      InterlockedAdd(_rwtex_u32[_ic], (0u - v[" << static_cast<uint32_t>(vd) << "]), _ip);\n"; break;
                    case 0x14: emit_tex_cas("asuint(min(asint(_cur), asint(v[" + std::to_string(vd) + "])))"); break;
                    case 0x15: os << "      InterlockedMin(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x16: emit_tex_cas("asuint(max(asint(_cur), asint(v[" + std::to_string(vd) + "])))"); break;
                    case 0x17: os << "      InterlockedMax(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x18: os << "      InterlockedAnd(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x19: os << "      InterlockedOr(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x1A: os << "      InterlockedXor(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                    case 0x1B: emit_tex_cas("(_cur >= v[" + std::to_string(vd) + "]) ? 0u : (_cur + 1u)"); break;
                    case 0x1C: emit_tex_cas("(_cur == 0u || _cur > v[" + std::to_string(vd) + "]) ? v[" + std::to_string(vd) + "] : (_cur - 1u)"); break;
                    default:   os << "      InterlockedAdd(_rwtex_u32[_ic], v[" << static_cast<uint32_t>(vd) << "], _ip);\n"; break;
                }
                if (inst.glc) os << "      v[" << static_cast<uint32_t>(vd) << "] = _ip;\n";
                os << "    }\n";
            } else if (e->is_load && !e->is_sample && !e->is_gather) {
                const bool has_mip = (op == 0x01u || op == 0x04u || op == 0x05u);
                os << "    { float4 _tx = ";
                if (dim == kMimgDim1D) {
                    os << tname << ".Load(int2((int)v[" << static_cast<uint32_t>(va) << "], "
                       << (has_mip ? ("(int)v[" + std::to_string((va + 1u) & 63u) + "]") : "0") << "));\n";
                } else if (dim == kMimgDim3D || dim == kMimgDim2DArray) {
                    os << tname << ".Load(int4((int)v[" << static_cast<uint32_t>(va) << "], (int)v["
                       << ((va + 1u) & 63u) << "], (int)v[" << ((va + 2u) & 63u) << "], "
                       << (has_mip ? ("(int)v[" + std::to_string((va + 3u) & 63u) + "]") : "0") << "));\n";
                } else if (dim == kMimgDimCube) {
                    os << tname << ".SampleLevel(_samp0, float3(asfloat(v[" << static_cast<uint32_t>(va)
                       << "]), asfloat(v[" << ((va + 1u) & 63u) << "]), asfloat(v[" << ((va + 2u) & 63u) << "])), 0.0);\n";
                } else if (dim == kMimgDim2DMsaa) {
                    os << tname << ".Load(int2((int)v[" << static_cast<uint32_t>(va) << "], (int)v["
                       << ((va + 1u) & 63u) << "]), (int)v[" << ((va + 2u) & 63u) << "]);\n";
                } else if (dim == kMimgDim2DArrayMsaa) {
                    os << tname << ".Load(int3((int)v[" << static_cast<uint32_t>(va) << "], (int)v["
                       << ((va + 1u) & 63u) << "], (int)v[" << ((va + 2u) & 63u) << "]), (int)v["
                       << ((va + 3u) & 63u) << "]);\n";
                } else if (dim == kMimgDim1DArray) {
                    os << tname << ".Load(int3((int)v[" << static_cast<uint32_t>(va) << "], (int)v["
                       << ((va + 1u) & 63u) << "], 0));\n";
                } else {
                    os << tname << ".Load(int3((int)v[" << static_cast<uint32_t>(va) << "], (int)v["
                       << ((va + 1u) & 63u) << "], "
                       << (has_mip ? ("(int)v[" + std::to_string((va + 2u) & 63u) + "]") : "0") << "));\n";
                }
                const char* comps[4] = {"_tx.x", "_tx.y", "_tx.z", "_tx.w"};
                uint32_t dst_off = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if ((dmask & (1u << c)) != 0u) {
                        os << "      v[" << ((vd + dst_off) & 63u) << "] = asuint(" << comps[c] << ");\n";
                        ++dst_off;
                    }
                }
                os << "    }\n";
            } else {
                // Sample / Gather4
                const uint8_t low = static_cast<uint8_t>(op & 0x0Fu);
                const bool is_cmp = (low >= 0x08u);
                const bool has_off = ((op & 0x10u) != 0u);
                const uint8_t mode = low & 0x07u;
                const bool has_bias = (mode == 5u || mode == 6u);
                const bool has_lod = (mode == 4u);
                const bool has_grad = (mode == 2u || mode == 3u);
                uint32_t ai = va;
                if (has_off) ++ai;
                if (has_bias) ++ai;
                const uint32_t dref_r = ai;
                if (is_cmp) ++ai;
                if (has_grad) ai += (op >= 0xA0u) ? 2u : 4u;

                std::string coord_expr;
                uint32_t ccnt = 2u;
                if (dim == kMimgDim1D) {
                    coord_expr = "asfloat(v[" + std::to_string(ai & 63u) + "])";
                    ccnt = 1u;
                } else if (dim == kMimgDim3D || dim == kMimgDimCube || dim == kMimgDim2DArray) {
                    coord_expr = "float3(asfloat(v[" + std::to_string(ai & 63u) + "]), asfloat(v[" +
                                 std::to_string((ai + 1u) & 63u) + "]), asfloat(v[" +
                                 std::to_string((ai + 2u) & 63u) + "]))";
                    ccnt = 3u;
                } else {
                    coord_expr = "float2(asfloat(v[" + std::to_string(ai & 63u) + "]), asfloat(v[" +
                                 std::to_string((ai + 1u) & 63u) + "]))";
                    ccnt = 2u;
                }
                const std::string lod_expr = has_lod ? ("asfloat(v[" + std::to_string((ai + ccnt) & 63u) + "])") : "0.0";

                if (e->is_gather && (dim == kMimgDim2D || dim == kMimgDimCube || dim == kMimgDim2DArray)) {
                    if (is_cmp) {
                        os << "    { float4 _g = " << tname << ".GatherCmp(_samp_cmp, " << coord_expr
                           << ", asfloat(v[" << (dref_r & 63u) << "]));\n";
                    } else {
                        const char* gfn = ((dmask & 2u) && !(dmask & 1u)) ? "GatherGreen"
                                        : ((dmask & 4u) && !(dmask & 3u)) ? "GatherBlue"
                                        : ((dmask & 8u) && !(dmask & 7u)) ? "GatherAlpha" : "GatherRed";
                        os << "    { float4 _g = " << tname << "." << gfn << "(_samp0, " << coord_expr << ");\n";
                    }
                    os << "      v[" << static_cast<uint32_t>(vd) << "] = asuint(_g.x); v[" << ((vd + 1u) & 63u)
                       << "] = asuint(_g.y); v[" << ((vd + 2u) & 63u) << "] = asuint(_g.z); v[" << ((vd + 3u) & 63u)
                       << "] = asuint(_g.w); }\n";
                } else {
                    if (is_cmp) {
                        os << "    { float _c = " << tname << ".SampleCmpLevelZero(_samp_cmp, " << coord_expr
                           << ", asfloat(v[" << (dref_r & 63u) << "]));\n"
                           << "      float4 _tx = float4(_c, _c, _c, _c);\n";
                    } else {
                        os << "    { float4 _tx = " << tname << ".SampleLevel(_samp0, " << coord_expr << ", " << lod_expr << ");\n";
                    }
                    const char* comps[4] = {"_tx.x", "_tx.y", "_tx.z", "_tx.w"};
                    uint32_t dst_off = 0;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        if ((dmask & (1u << c)) != 0u) {
                            os << "      v[" << ((vd + dst_off) & 63u) << "] = asuint(" << comps[c] << ");\n";
                            ++dst_off;
                        }
                    }
                    os << "    }\n";
                }
            }
        } else if (inst.encoding == Rdna2Encoding::DS && is_compute) {
            const uint8_t vdst = static_cast<uint8_t>(inst.dst.reg_index & 63u);
            const uint8_t vaddr = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t d0 = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const uint8_t d1 = static_cast<uint8_t>(inst.src[2].reg_index & 63u);
            const uint32_t off16 = static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu;
            const std::string base_a = "(v[" + std::to_string(vaddr) + "] + " + std::to_string(off16) + "u)";

            if (op == 0x14u) {
                // ds_nop
            } else if (op >= 0x18u && op <= 0x1Du) {
                os << "    GroupMemoryBarrierWithGroupSync();\n";
            } else if (op == 0x35u) {
                const uint32_t wmask = std::max(1u, container.threadgroup_x) - 1u;
                os << "    { _wave_tmp[gidx & 63u] = v[" << static_cast<uint32_t>(vaddr) << "];\n"
                   << "      GroupMemoryBarrierWithGroupSync();\n";
                if ((off16 & 0x8000u) != 0u) {
                    os << "      uint _sl = ((gidx & ~3u) | ((" << off16 << "u >> ((gidx & 3u) * 2u)) & 3u)) & " << wmask << "u;\n";
                } else {
                    const uint32_t and_m = off16 & 0x1Fu;
                    const uint32_t or_m = (off16 >> 5) & 0x1Fu;
                    const uint32_t xor_m = (off16 >> 10) & 0x1Fu;
                    os << "      uint _sl = ((gidx & ~31u) | ((((gidx & 31u) & " << and_m << "u) | " << or_m << "u) ^ " << xor_m << "u)) & " << wmask << "u;\n";
                }
                os << "      v[" << static_cast<uint32_t>(vdst) << "] = _wave_tmp[_sl];\n"
                   << "      GroupMemoryBarrierWithGroupSync(); }\n";
            } else if (op == 0xB2u) {
                const uint32_t wmask = std::max(1u, container.threadgroup_x) - 1u;
                os << "    { _wave_tmp[gidx & 63u] = 0u;\n"
                   << "      GroupMemoryBarrierWithGroupSync();\n"
                   << "      _wave_tmp[(v[" << static_cast<uint32_t>(vaddr) << "] >> 2u) & " << wmask << "u] = v[" << static_cast<uint32_t>(d0) << "];\n"
                   << "      GroupMemoryBarrierWithGroupSync();\n"
                   << "      v[" << static_cast<uint32_t>(vdst) << "] = _wave_tmp[gidx & 63u];\n"
                   << "      GroupMemoryBarrierWithGroupSync(); }\n";
            } else if (op == 0xB3u) {
                const uint32_t wmask = std::max(1u, container.threadgroup_x) - 1u;
                os << "    { _wave_tmp[gidx & 63u] = v[" << static_cast<uint32_t>(d0) << "];\n"
                   << "      GroupMemoryBarrierWithGroupSync();\n"
                   << "      v[" << static_cast<uint32_t>(vdst) << "] = _wave_tmp[(v[" << static_cast<uint32_t>(vaddr) << "] >> 2u) & " << wmask << "u];\n"
                   << "      GroupMemoryBarrierWithGroupSync(); }\n";
            } else if (op == 0x3Eu || op == 0x3Fu) {
                os << "    InterlockedAdd(_ds_counter, 1u, v[" << static_cast<uint32_t>(vdst) << "]);\n";
            } else if (op == 0x3Du) {
                os << "    { uint _prev_c = 0u; InterlockedAdd(_ds_counter, 0xFFFFFFFFu, _prev_c); v["
                   << static_cast<uint32_t>(vdst) << "] = (_prev_c > 0u) ? (_prev_c - 1u) : 0u; }\n";
            } else if (op == 0x0Du) {
                os << "    _lds[(" << base_a << " >> 2u) & 4095u] = v[" << static_cast<uint32_t>(d0) << "];\n";
            } else if (op == 0x0Eu || op == 0x0Fu) {
                const uint32_t sc = (op == 0x0Fu) ? 256u : 4u;
                os << "    _lds[((v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset0) * sc) << "u) >> 2u) & 4095u] = v[" << static_cast<uint32_t>(d0) << "];\n";
                os << "    _lds[((v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset1) * sc) << "u) >> 2u) & 4095u] = v[" << static_cast<uint32_t>(d1) << "];\n";
            } else if (op == 0x1Eu || op == 0xA0u) {
                const std::string vexpr = (op == 0xA0u) ? ("(v[" + std::to_string(d0) + "] >> 16u)") : ("v[" + std::to_string(d0) + "]");
                os << "    { uint _a = " << base_a << "; uint _sh = (_a & 3u) * 8u; uint _o = 0u;\n"
                   << "      InterlockedAnd(_lds[(_a >> 2u) & 4095u], ~(0xFFu << _sh), _o);\n"
                   << "      InterlockedOr(_lds[(_a >> 2u) & 4095u], (" << vexpr << " & 0xFFu) << _sh, _o); }\n";
            } else if (op == 0x1Fu || op == 0xA1u) {
                const std::string vexpr = (op == 0xA1u) ? ("(v[" + std::to_string(d0) + "] >> 16u)") : ("v[" + std::to_string(d0) + "]");
                os << "    { uint _a = " << base_a << "; uint _sh = (_a & 2u) * 8u; uint _o = 0u;\n"
                   << "      InterlockedAnd(_lds[(_a >> 2u) & 4095u], ~(0xFFFFu << _sh), _o);\n"
                   << "      InterlockedOr(_lds[(_a >> 2u) & 4095u], (" << vexpr << " & 0xFFFFu) << _sh, _o); }\n";
            } else if (op == 0x4Du || op == 0xDEu || op == 0xDFu) {
                const uint32_t n = e->data_dwords;
                for (uint32_t k = 0; k < n; ++k) {
                    os << "    _lds[((" << base_a << " + " << (k * 4u) << "u) >> 2u) & 4095u] = v[" << ((d0 + k) & 63u) << "];\n";
                }
            } else if (op == 0x4Eu || op == 0x4Fu) {
                const uint32_t sc = (op == 0x4Fu) ? 512u : 8u;
                os << "    { uint _a0 = v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset0) * sc) << "u;\n"
                   << "      uint _a1 = v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset1) * sc) << "u;\n"
                   << "      _lds[(_a0 >> 2u) & 4095u] = v[" << static_cast<uint32_t>(d0) << "];\n"
                   << "      _lds[((_a0 + 4u) >> 2u) & 4095u] = v[" << ((d0 + 1u) & 63u) << "];\n"
                   << "      _lds[(_a1 >> 2u) & 4095u] = v[" << static_cast<uint32_t>(d1) << "];\n"
                   << "      _lds[((_a1 + 4u) >> 2u) & 4095u] = v[" << ((d1 + 1u) & 63u) << "]; }\n";
            } else if (op == 0x36u) {
                os << "    v[" << static_cast<uint32_t>(vdst) << "] = _lds[(" << base_a << " >> 2u) & 4095u];\n";
            } else if (op == 0x37u || op == 0x38u) {
                const uint32_t sc = (op == 0x38u) ? 256u : 4u;
                os << "    v[" << static_cast<uint32_t>(vdst) << "] = _lds[((v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset0) * sc) << "u) >> 2u) & 4095u];\n";
                os << "    v[" << ((vdst + 1u) & 63u) << "] = _lds[((v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset1) * sc) << "u) >> 2u) & 4095u];\n";
            } else if (op == 0x39u || op == 0x3Au) {
                os << "    { uint _a = " << base_a << "; uint _b = (_lds[(_a >> 2u) & 4095u] >> ((_a & 3u) * 8u)) & 0xFFu;\n"
                   << "      v[" << static_cast<uint32_t>(vdst) << "] = "
                   << ((op == 0x39u) ? "asuint(((int)(_b << 24u)) >> 24)" : "_b") << "; }\n";
            } else if (op == 0x3Bu || op == 0x3Cu) {
                os << "    { uint _a = " << base_a << "; uint _h = (_lds[(_a >> 2u) & 4095u] >> ((_a & 2u) * 8u)) & 0xFFFFu;\n"
                   << "      v[" << static_cast<uint32_t>(vdst) << "] = "
                   << ((op == 0x3Bu) ? "asuint(((int)(_h << 16u)) >> 16)" : "_h") << "; }\n";
            } else if (op == 0x76u || op == 0xFEu || op == 0xFFu) {
                const uint32_t n = e->data_dwords;
                for (uint32_t k = 0; k < n; ++k) {
                    os << "    v[" << ((vdst + k) & 63u) << "] = _lds[((" << base_a << " + " << (k * 4u) << "u) >> 2u) & 4095u];\n";
                }
            } else if (op == 0x77u || op == 0x78u) {
                const uint32_t sc = (op == 0x78u) ? 512u : 8u;
                os << "    { uint _a0 = v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset0) * sc) << "u;\n"
                   << "      uint _a1 = v[" << static_cast<uint32_t>(vaddr) << "] + " << (static_cast<uint32_t>(inst.ds_offset1) * sc) << "u;\n"
                   << "      v[" << static_cast<uint32_t>(vdst) << "] = _lds[(_a0 >> 2u) & 4095u];\n"
                   << "      v[" << ((vdst + 1u) & 63u) << "] = _lds[((_a0 + 4u) >> 2u) & 4095u];\n"
                   << "      v[" << ((vdst + 2u) & 63u) << "] = _lds[(_a1 >> 2u) & 4095u];\n"
                   << "      v[" << ((vdst + 3u) & 63u) << "] = _lds[((_a1 + 4u) >> 2u) & 4095u]; }\n";
            } else if (e->is_atomic) {
                if (op == 0x40u || op == 0x60u) {
                    EmitHlslAtomicOp(os, "_lds", base_a, 0x52u, d0, vdst, e->has_rtn, true);
                } else {
                    const uint16_t base_op = (op == 0x55u) ? 0x15u
                                           : ((op >= 0x20u && op <= 0x33u) ? static_cast<uint16_t>(op - 0x20u)
                                           : ((op >= 0x60u && op <= 0x73u) ? static_cast<uint16_t>(op - 0x60u)
                                           : ((op >= 0x40u && op <= 0x53u) ? static_cast<uint16_t>(op - 0x40u) : op)));
                    uint16_t mapped_atomic = 0x32u;
                    switch (base_op) {
                        case 0x00: mapped_atomic = 0x32u; break;
                        case 0x01: mapped_atomic = 0x33u; break;
                        case 0x02: mapped_atomic = 0x4Eu; break;
                        case 0x03: mapped_atomic = 0x3Cu; break;
                        case 0x04: mapped_atomic = 0x3Du; break;
                        case 0x05: mapped_atomic = 0x35u; break;
                        case 0x06: mapped_atomic = 0x37u; break;
                        case 0x07: mapped_atomic = 0x36u; break;
                        case 0x08: mapped_atomic = 0x38u; break;
                        case 0x09: mapped_atomic = 0x39u; break;
                        case 0x0A: mapped_atomic = 0x3Au; break;
                        case 0x0B: mapped_atomic = 0x3Bu; break;
                        case 0x0D: mapped_atomic = 0x30u; break;
                        case 0x12: mapped_atomic = 0x3Fu; break;
                        case 0x13: mapped_atomic = 0x40u; break;
                        case 0x15: mapped_atomic = 0x4Du; break;
                        default:   mapped_atomic = 0x32u; break;
                    }
                    if (base_op == 0x10u || base_op == 0x11u) {
                        // ds_cmpst: data0 = cmp, data1 = new
                        os << "    { uint _idx = ((" << base_a << ") >> 2u) & 4095u; uint _prev = 0u;\n"
                           << "      InterlockedCompareExchange(_lds[_idx], v[" << static_cast<uint32_t>(d0)
                           << "], v[" << static_cast<uint32_t>(d1) << "], _prev);\n";
                        if (e->has_rtn) os << "      v[" << static_cast<uint32_t>(vdst) << "] = _prev;\n";
                        os << "    }\n";
                    } else {
                        EmitHlslAtomicOp(os, "_lds", base_a, mapped_atomic, d0, vdst, e->has_rtn, true);
                    }
                }
            }
        } else if (inst.encoding == Rdna2Encoding::FLAT) {
            const uint8_t vdst = static_cast<uint8_t>(inst.dst.reg_index & 63u);
            const uint8_t va = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t vd = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const std::string aexpr = "(uint)((int)v[" + std::to_string(va) + "] + " + std::to_string(inst.mem_offset) + ")";
            if (inst.flat_seg == 1u) {
                // SCRATCH (byte/short/d16/dword/x2/x3/x4 on private _scratch[64])
                if (e->is_load) {
                    switch (op) {
                        case 0x08: os << "    { uint _sa = " << aexpr << "; v[" << static_cast<uint32_t>(vdst) << "] = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 3u) * 8u)) & 0xFFu; }\n"; break;
                        case 0x09: os << "    { uint _sa = " << aexpr << "; uint _b = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 3u) * 8u)) & 0xFFu; v[" << static_cast<uint32_t>(vdst) << "] = asuint(((int)(_b << 24u)) >> 24); }\n"; break;
                        case 0x0A: os << "    { uint _sa = " << aexpr << "; v[" << static_cast<uint32_t>(vdst) << "] = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 2u) * 8u)) & 0xFFFFu; }\n"; break;
                        case 0x0B: os << "    { uint _sa = " << aexpr << "; uint _h = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 2u) * 8u)) & 0xFFFFu; v[" << static_cast<uint32_t>(vdst) << "] = asuint(((int)(_h << 16u)) >> 16); }\n"; break;
                        case 0x20: os << "    { uint _sa = " << aexpr << "; uint _b = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 3u) * 8u)) & 0xFFu; v[" << static_cast<uint32_t>(vdst) << "] = (v[" << static_cast<uint32_t>(vdst) << "] & 0xFFFF0000u) | _b; }\n"; break;
                        case 0x21: os << "    { uint _sa = " << aexpr << "; uint _b = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 3u) * 8u)) & 0xFFu; v[" << static_cast<uint32_t>(vdst) << "] = (v[" << static_cast<uint32_t>(vdst) << "] & 0xFFFFu) | (_b << 16u); }\n"; break;
                        case 0x22: os << "    { uint _sa = " << aexpr << "; uint _b = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 3u) * 8u)) & 0xFFu; uint _s = asuint(((int)(_b << 24u)) >> 24) & 0xFFFFu; v[" << static_cast<uint32_t>(vdst) << "] = (v[" << static_cast<uint32_t>(vdst) << "] & 0xFFFF0000u) | _s; }\n"; break;
                        case 0x24: os << "    { uint _sa = " << aexpr << "; uint _h = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 2u) * 8u)) & 0xFFFFu; v[" << static_cast<uint32_t>(vdst) << "] = (v[" << static_cast<uint32_t>(vdst) << "] & 0xFFFF0000u) | _h; }\n"; break;
                        case 0x25: os << "    { uint _sa = " << aexpr << "; uint _h = (_scratch[(_sa >> 2u) & 63u] >> ((_sa & 2u) * 8u)) & 0xFFFFu; v[" << static_cast<uint32_t>(vdst) << "] = (v[" << static_cast<uint32_t>(vdst) << "] & 0xFFFFu) | (_h << 16u); }\n"; break;
                        default: {
                            const uint32_t n = e->data_dwords;
                            for (uint32_t k = 0; k < n; ++k) {
                                os << "    v[" << ((vdst + k) & 63u) << "] = _scratch[(((" << aexpr << ") + " << (k * 4u) << "u) >> 2u) & 63u];\n";
                            }
                            break;
                        }
                    }
                } else {
                    switch (op) {
                        case 0x18: os << "    { uint _sa = " << aexpr << "; uint _sh = (_sa & 3u) * 8u; uint _si = (_sa >> 2u) & 63u; _scratch[_si] = (_scratch[_si] & ~(0xFFu << _sh)) | ((v[" << static_cast<uint32_t>(vd) << "] & 0xFFu) << _sh); }\n"; break;
                        case 0x1A: os << "    { uint _sa = " << aexpr << "; uint _sh = (_sa & 2u) * 8u; uint _si = (_sa >> 2u) & 63u; _scratch[_si] = (_scratch[_si] & ~(0xFFFFu << _sh)) | ((v[" << static_cast<uint32_t>(vd) << "] & 0xFFFFu) << _sh); }\n"; break;
                        default: {
                            const uint32_t n = e->data_dwords;
                            for (uint32_t k = 0; k < n; ++k) {
                                os << "    _scratch[(((" << aexpr << ") + " << (k * 4u) << "u) >> 2u) & 63u] = v[" << ((vd + k) & 63u) << "];\n";
                            }
                            break;
                        }
                    }
                }
            } else {
                const bool use_u1 = (inst.saddr != 0x7Fu && inst.saddr != 0u);
                const char* bname = use_u1 ? "_uav1" : "_uav0";
                const char* btag = use_u1 ? "u1" : "u0";
                if (e->is_atomic) {
                    EmitHlslAtomicOp(os, bname, aexpr, op, vd, vdst, inst.glc, false);
                } else if (e->is_load) {
                    switch (op) {
                        case 0x08: os << "    v[" << static_cast<uint32_t>(vdst) << "] = _load_u8_" << btag << "(" << aexpr << ");\n"; break;
                        case 0x09: os << "    v[" << static_cast<uint32_t>(vdst) << "] = asuint(((int)(_load_u8_" << btag << "(" << aexpr << ") << 24u)) >> 24);\n"; break;
                        case 0x0A: os << "    v[" << static_cast<uint32_t>(vdst) << "] = _load_u16_" << btag << "(" << aexpr << ");\n"; break;
                        case 0x0B: os << "    v[" << static_cast<uint32_t>(vdst) << "] = asuint(((int)(_load_u16_" << btag << "(" << aexpr << ") << 16u)) >> 16);\n"; break;
                        default: {
                            const uint32_t n = e->data_dwords;
                            for (uint32_t k = 0; k < n; ++k) {
                                os << "    v[" << ((vdst + k) & 63u) << "] = " << bname << ".Load(((" << aexpr << ") + " << (k * 4u) << "u) & ~3u);\n";
                            }
                            break;
                        }
                    }
                } else {
                    switch (op) {
                        case 0x18: os << "    _store_u8_" << btag << "(" << aexpr << ", v[" << static_cast<uint32_t>(vd) << "]);\n"; break;
                        case 0x1A: os << "    _store_u16_" << btag << "(" << aexpr << ", v[" << static_cast<uint32_t>(vd) << "]);\n"; break;
                        default: {
                            const uint32_t n = e->data_dwords;
                            for (uint32_t k = 0; k < n; ++k) {
                                os << "    " << bname << ".Store(((" << aexpr << ") + " << (k * 4u) << "u) & ~3u, v[" << ((vd + k) & 63u) << "]);\n";
                            }
                            break;
                        }
                    }
                }
            }
        } else if (inst.encoding == Rdna2Encoding::EXP) {
            const uint8_t tgt = inst.exp_target;
            const uint8_t s0 = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t s1 = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const uint8_t s2 = static_cast<uint8_t>(inst.src[2].reg_index & 63u);
            const uint8_t s3 = static_cast<uint8_t>(inst.src[3].reg_index & 63u);
            std::string vec_expr;
            if (inst.exp_compr) {
                vec_expr = "float4(f16tof32(v[" + std::to_string(s0) + "] & 0xFFFFu), f16tof32(v[" +
                           std::to_string(s0) + "] >> 16u), f16tof32(v[" + std::to_string(s1) +
                           "] & 0xFFFFu), f16tof32(v[" + std::to_string(s1) + "] >> 16u))";
            } else {
                vec_expr = "float4(asfloat(v[" + std::to_string(s0) + "]), asfloat(v[" + std::to_string(s1) +
                           "]), asfloat(v[" + std::to_string(s2) + "]), asfloat(v[" + std::to_string(s3) + "]))";
            }
            if (container.stage == ShaderStage::Vertex) {
                if (tgt >= 12u && tgt <= 15u) os << "    _out.pos = " << vec_expr << ";\n";
                else if (tgt >= 32u && tgt < 40u) os << "    _out.param" << (tgt - 32u) << " = " << vec_expr << ";\n";
            } else if (container.stage == ShaderStage::Pixel) {
                if (tgt <= 7u) os << "    _out.mrt" << static_cast<uint32_t>(tgt) << " = " << vec_expr << ";\n";
                else if (tgt == 8u) os << "    _out.depth = (" << vec_expr << ").x;\n";
            }
        }
    }

    if (container.stage == ShaderStage::Vertex || container.stage == ShaderStage::Pixel) {
        os << "    return _out;\n";
    }
    os << "}\n";
    out_hlsl = os.str();
    return Status::Ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Binary Emitter (spirv-val & Vulkan 1.2/1.4 Verified)
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint32_t kSpvVer13 = 0x00010300u;
constexpr uint32_t kSpvGenId = 0x4F533200u;

enum SpvOp : uint16_t {
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpImageTexelPointer = 60,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpSampledImage = 86,
    OpImageSampleExplicitLod = 88,
    OpImageSampleDrefExplicitLod = 90,
    OpImageFetch = 95,
    OpImageGather = 96,
    OpImageDrefGather = 97,
    OpImageWrite = 99,
    OpImageQuerySizeLod = 103,
    OpImageQuerySize = 104,
    OpImageQueryLevels = 106,
    OpConvertFToU = 109,
    OpConvertFToS = 110,
    OpConvertSToF = 111,
    OpConvertUToF = 112,
    OpBitcast = 124,
    OpIAdd = 128,
    OpFAdd = 129,
    OpISub = 130,
    OpFSub = 131,
    OpIMul = 132,
    OpFMul = 133,
    OpFDiv = 136,
    OpDot = 148,
    OpIsNan = 156,
    OpLogicalAnd = 167,
    OpSelect = 169,
    OpIEqual = 170,
    OpUGreaterThanEqual = 174,
    OpULessThan = 176,
    OpFOrdGreaterThan = 186,
    OpFOrdLessThanEqual = 188,
    OpFOrdGreaterThanEqual = 190,
    OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195,
    OpShiftLeftLogical = 196,
    OpBitwiseOr = 197,
    OpBitwiseXor = 198,
    OpBitwiseAnd = 199,
    OpNot = 200,
    OpControlBarrier = 224,
    OpMemoryBarrier = 225,
    OpAtomicExchange = 229,
    OpAtomicCompareExchange = 230,
    OpAtomicIAdd = 234,
    OpAtomicISub = 235,
    OpAtomicSMin = 236,
    OpAtomicUMin = 237,
    OpAtomicSMax = 238,
    OpAtomicUMax = 239,
    OpAtomicAnd = 240,
    OpAtomicOr = 241,
    OpAtomicXor = 242,
    OpPhi = 245,
    OpLoopMerge = 246,
    OpLabel = 248,
    OpBranch = 249,
    OpBranchConditional = 250,
    OpReturn = 253,
};

class SpirvMemImgEmitter {
public:
    SpirvMemImgEmitter(const Xs2ShaderContainer& c, const std::vector<MemImgDecodedInst>& insts,
                       const MemImgResourceUsage& u)
        : container_(c), insts_(insts), usage_(u) {}

    Status Emit(std::vector<uint32_t>& out) {
        out.clear();
        const bool is_compute = (container_.stage == ShaderStage::Compute);
        const bool is_vs = (container_.stage == ShaderStage::Vertex);
        const bool is_ps = (container_.stage == ShaderStage::Pixel);

        // Scan for extra capabilities
        bool need_img_query = false;
        bool need_sampled_1d = usage_.uses_tex_dim[kMimgDim1D] || usage_.uses_tex_dim[kMimgDim1DArray];
        for (const auto& di : insts_) {
            if (di.entry && di.base.encoding == Rdna2Encoding::MIMG &&
                (di.entry->raw_opcode == 0x0Eu || di.entry->raw_opcode == 0x60u)) {
                need_img_query = true;
            }
        }

        EmitInst(sec_caps_, OpCapability, {1u});  // Shader
        if (need_img_query) EmitInst(sec_caps_, OpCapability, {50u});  // ImageQuery
        if (need_sampled_1d) EmitInst(sec_caps_, OpCapability, {43u});  // Sampled1D

        glsl_ext_ = AllocId();
        {
            std::vector<uint32_t> ops = {glsl_ext_};
            AppendString(ops, "GLSL.std.450");
            EmitInst(sec_ext_, OpExtInstImport, ops);
        }
        EmitInst(sec_mem_, OpMemoryModel, {0u, 1u});  // Logical GLSL450

        // Core types
        t_void_ = AllocId(); EmitInst(sec_types_, OpTypeVoid, {t_void_});
        t_bool_ = AllocId(); EmitInst(sec_types_, OpTypeBool, {t_bool_});
        t_u32_ = AllocId(); EmitInst(sec_types_, OpTypeInt, {t_u32_, 32u, 0u});
        t_i32_ = AllocId(); EmitInst(sec_types_, OpTypeInt, {t_i32_, 32u, 1u});
        t_f32_ = AllocId(); EmitInst(sec_types_, OpTypeFloat, {t_f32_, 32u});
        t_uvec2_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_uvec2_, t_u32_, 2u});
        t_uvec3_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_uvec3_, t_u32_, 3u});
        t_uvec4_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_uvec4_, t_u32_, 4u});
        t_ivec2_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_ivec2_, t_i32_, 2u});
        t_ivec3_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_ivec3_, t_i32_, 3u});
        t_vec2_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_vec2_, t_f32_, 2u});
        t_vec3_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_vec3_, t_f32_, 3u});
        t_vec4_ = AllocId(); EmitInst(sec_types_, OpTypeVector, {t_vec4_, t_f32_, 4u});
        t_fn_ = AllocId(); EmitInst(sec_types_, OpTypeFunction, {t_fn_, t_void_});

        c_u0_ = ConstU32(0u);
        c_u1_ = ConstU32(1u);
        c_u2_ = ConstU32(2u);
        c_u3_ = ConstU32(3u);
        c_u4_ = ConstU32(4u);
        c_u64_ = ConstU32(64u);
        c_u4096_ = ConstU32(4096u);
        c_f0_ = ConstF32(0.0f);
        c_f1_ = ConstF32(1.0f);

        t_arr64_u32_ = AllocId(); EmitInst(sec_types_, OpTypeArray, {t_arr64_u32_, t_u32_, c_u64_});
        t_ptr_fn_u32_ = AllocId(); EmitInst(sec_types_, OpTypePointer, {t_ptr_fn_u32_, 7u, t_u32_});
        t_ptr_fn_arr64_ = AllocId(); EmitInst(sec_types_, OpTypePointer, {t_ptr_fn_arr64_, 7u, t_arr64_u32_});

        // Storage buffers
        if (usage_.uses_uav0 || usage_.uses_uav1) {
            t_rtarr_u32_ = AllocId(); EmitInst(sec_types_, OpTypeRuntimeArray, {t_rtarr_u32_, t_u32_});
            EmitInst(sec_decor_, OpDecorate, {t_rtarr_u32_, 6u, 4u});  // ArrayStride 4
            t_ssbo_ = AllocId(); EmitInst(sec_types_, OpTypeStruct, {t_ssbo_, t_rtarr_u32_});
            EmitInst(sec_decor_, OpDecorate, {t_ssbo_, 2u});  // Block
            EmitInst(sec_decor_, OpMemberDecorate, {t_ssbo_, 0u, 35u, 0u});  // Offset 0
            t_ptr_ssbo_ = AllocId(); EmitInst(sec_types_, OpTypePointer, {t_ptr_ssbo_, 12u, t_ssbo_});
            t_ptr_ssbo_u32_ = AllocId(); EmitInst(sec_types_, OpTypePointer, {t_ptr_ssbo_u32_, 12u, t_u32_});

            if (usage_.uses_uav0) {
                var_uav0_ = AllocId();
                EmitInst(sec_globals_, OpVariable, {t_ptr_ssbo_, var_uav0_, 12u});
                EmitInst(sec_decor_, OpDecorate, {var_uav0_, 34u, 0u});
                EmitInst(sec_decor_, OpDecorate, {var_uav0_, 33u, 0u});
            }
            if (usage_.uses_uav1) {
                var_uav1_ = AllocId();
                EmitInst(sec_globals_, OpVariable, {t_ptr_ssbo_, var_uav1_, 12u});
                EmitInst(sec_decor_, OpDecorate, {var_uav1_, 34u, 0u});
                EmitInst(sec_decor_, OpDecorate, {var_uav1_, 33u, 1u});
            }
        }

        // Workgroup shared memory
        if (is_compute && (usage_.uses_lds || usage_.uses_wave_tmp || usage_.uses_ds_counter)) {
            t_ptr_wg_u32_ = AllocId(); EmitInst(sec_types_, OpTypePointer, {t_ptr_wg_u32_, 4u, t_u32_});
            if (usage_.uses_lds) {
                t_arr4096_u32_ = AllocId(); EmitInst(sec_types_, OpTypeArray, {t_arr4096_u32_, t_u32_, c_u4096_});
                uint32_t ptr_lds = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_lds, 4u, t_arr4096_u32_});
                var_lds_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_lds, var_lds_, 4u});
            }
            if (usage_.uses_wave_tmp) {
                uint32_t ptr_wt = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_wt, 4u, t_arr64_u32_});
                var_wave_tmp_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_wt, var_wave_tmp_, 4u});
            }
            if (usage_.uses_ds_counter) {
                var_ds_counter_ = AllocId(); EmitInst(sec_globals_, OpVariable, {t_ptr_wg_u32_, var_ds_counter_, 4u});
            }
        }

        // Textures & samplers
        for (uint8_t d = 0; d < 8u; ++d) {
            if (!usage_.uses_tex_dim[d]) continue;
            uint32_t spv_dim = 1u;
            uint32_t arrayed = 0u;
            uint32_t ms = 0u;
            switch (d) {
                case kMimgDim1D:          spv_dim = 0u; break;
                case kMimgDim2D:          spv_dim = 1u; break;
                case kMimgDim3D:          spv_dim = 2u; break;
                case kMimgDimCube:        spv_dim = 3u; break;
                case kMimgDim1DArray:     spv_dim = 0u; arrayed = 1u; break;
                case kMimgDim2DArray:     spv_dim = 1u; arrayed = 1u; break;
                case kMimgDim2DMsaa:      spv_dim = 1u; ms = 1u; break;
                case kMimgDim2DArrayMsaa: spv_dim = 1u; arrayed = 1u; ms = 1u; break;
                default: break;
            }
            t_tex_[d] = AllocId();
            EmitInst(sec_types_, OpTypeImage, {t_tex_[d], t_f32_, spv_dim, 0u, arrayed, ms, 1u, 0u});
            t_sampled_tex_[d] = AllocId();
            EmitInst(sec_types_, OpTypeSampledImage, {t_sampled_tex_[d], t_tex_[d]});
            uint32_t ptr_img = AllocId();
            EmitInst(sec_types_, OpTypePointer, {ptr_img, 0u, t_tex_[d]});
            var_tex_[d] = AllocId();
            EmitInst(sec_globals_, OpVariable, {ptr_img, var_tex_[d], 0u});
            EmitInst(sec_decor_, OpDecorate, {var_tex_[d], 34u, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_tex_[d], 33u, 2u + static_cast<uint32_t>(d)});
        }
        if (usage_.uses_samp || usage_.uses_samp_cmp) {
            t_samp_ = AllocId(); EmitInst(sec_types_, OpTypeSampler, {t_samp_});
            uint32_t ptr_s = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_s, 0u, t_samp_});
            var_samp_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_s, var_samp_, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_samp_, 34u, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_samp_, 33u, 10u});
        }
        if (usage_.uses_rwtex_f4) {
            t_rwtex_f4_ = AllocId(); EmitInst(sec_types_, OpTypeImage, {t_rwtex_f4_, t_f32_, 1u, 0u, 0u, 0u, 2u, 1u});
            uint32_t ptr_rw = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_rw, 0u, t_rwtex_f4_});
            var_rwtex_f4_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_rw, var_rwtex_f4_, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_rwtex_f4_, 34u, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_rwtex_f4_, 33u, 11u});
        }
        if (usage_.uses_rwtex_u32) {
            t_rwtex_u32_ = AllocId(); EmitInst(sec_types_, OpTypeImage, {t_rwtex_u32_, t_u32_, 1u, 0u, 0u, 0u, 2u, 33u});
            uint32_t ptr_rw = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_rw, 0u, t_rwtex_u32_});
            var_rwtex_u32_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_rw, var_rwtex_u32_, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_rwtex_u32_, 34u, 0u});
            EmitInst(sec_decor_, OpDecorate, {var_rwtex_u32_, 33u, 12u});
            t_ptr_img_u32_ = AllocId(); EmitInst(sec_types_, OpTypePointer, {t_ptr_img_u32_, 11u, t_u32_});
        }

        // Stage inputs / outputs (interface variables)
        std::vector<uint32_t> iface;
        if (is_compute) {
            uint32_t ptr_in_uv3 = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_in_uv3, 1u, t_uvec3_});
            uint32_t ptr_in_u32 = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_in_u32, 1u, t_u32_});
            in_gid_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_in_uv3, in_gid_, 1u});
            EmitInst(sec_decor_, OpDecorate, {in_gid_, 11u, 28u});  // GlobalInvocationId
            in_gidx_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_in_u32, in_gidx_, 1u});
            EmitInst(sec_decor_, OpDecorate, {in_gidx_, 11u, 29u});  // LocalInvocationIndex
            iface.push_back(in_gid_);
            iface.push_back(in_gidx_);
        } else if (is_vs) {
            uint32_t ptr_in_i32 = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_in_i32, 1u, t_i32_});
            uint32_t ptr_out_v4 = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_out_v4, 3u, t_vec4_});
            in_vid_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_in_i32, in_vid_, 1u});
            EmitInst(sec_decor_, OpDecorate, {in_vid_, 11u, 42u});  // VertexIndex
            iface.push_back(in_vid_);
            out_pos_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_out_v4, out_pos_, 3u});
            EmitInst(sec_decor_, OpDecorate, {out_pos_, 11u, 0u});  // BuiltIn Position
            iface.push_back(out_pos_);
            for (uint32_t p = 0; p < 8u; ++p) {
                if ((usage_.exp_param_mask & (1u << p)) != 0u) {
                    out_param_[p] = AllocId();
                    EmitInst(sec_globals_, OpVariable, {ptr_out_v4, out_param_[p], 3u});
                    EmitInst(sec_decor_, OpDecorate, {out_param_[p], 30u, p});  // Location p
                    iface.push_back(out_param_[p]);
                }
            }
        } else if (is_ps) {
            uint32_t ptr_out_v4 = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_out_v4, 3u, t_vec4_});
            const uint8_t mrt_mask = usage_.exp_mrt_mask ? usage_.exp_mrt_mask : 1u;
            for (uint32_t m = 0; m < 8u; ++m) {
                if ((mrt_mask & (1u << m)) != 0u) {
                    out_mrt_[m] = AllocId();
                    EmitInst(sec_globals_, OpVariable, {ptr_out_v4, out_mrt_[m], 3u});
                    EmitInst(sec_decor_, OpDecorate, {out_mrt_[m], 30u, m});  // Location m
                    iface.push_back(out_mrt_[m]);
                }
            }
            if (usage_.uses_exp_mrtz) {
                uint32_t ptr_out_f32 = AllocId(); EmitInst(sec_types_, OpTypePointer, {ptr_out_f32, 3u, t_f32_});
                out_depth_ = AllocId(); EmitInst(sec_globals_, OpVariable, {ptr_out_f32, out_depth_, 3u});
                EmitInst(sec_decor_, OpDecorate, {out_depth_, 11u, 22u});  // BuiltIn FragDepth
                iface.push_back(out_depth_);
            }
        }

        // EntryPoint & ExecutionMode
        const uint32_t main_id = AllocId();
        {
            const uint32_t exec_model = is_vs ? 0u : (is_ps ? 4u : 5u);
            std::vector<uint32_t> ops = {exec_model, main_id};
            AppendString(ops, "main");
            ops.insert(ops.end(), iface.begin(), iface.end());
            EmitInst(sec_entry_, OpEntryPoint, ops);
        }
        if (is_compute) {
            EmitInst(sec_exec_, OpExecutionMode,
                     {main_id, 17u, std::max(1u, container_.threadgroup_x),
                      std::max(1u, container_.threadgroup_y), std::max(1u, container_.threadgroup_z)});
        } else if (is_ps) {
            EmitInst(sec_exec_, OpExecutionMode, {main_id, 7u});  // OriginUpperLeft
            if (usage_.uses_exp_mrtz) {
                EmitInst(sec_exec_, OpExecutionMode, {main_id, 12u});  // DepthReplacing
            }
        }

        // Function body
        EmitInst(sec_fn_, OpFunction, {t_void_, main_id, 0u, t_fn_});
        const uint32_t lbl_entry = AllocId();
        EmitInst(sec_fn_, OpLabel, {lbl_entry});
        cur_label_ = lbl_entry;

        // All Function-scope OpVariables must come first!
        var_vgpr_ = AllocId();
        EmitInst(sec_fn_, OpVariable, {t_ptr_fn_arr64_, var_vgpr_, 7u});
        if (usage_.uses_scratch) {
            var_scratch_ = AllocId();
            EmitInst(sec_fn_, OpVariable, {t_ptr_fn_arr64_, var_scratch_, 7u});
        }

        // Initialize VGPRs (v[0]=tid, v[1]=tid*4, v[2]=tid*16, v[3]=tid*8, v[4..63]=0)
        uint32_t tid_id = c_u0_;
        gidx_id_ = c_u0_;
        if (is_compute) {
            uint32_t gid = AllocId(); EmitInst(sec_fn_, OpLoad, {t_uvec3_, gid, in_gid_});
            tid_id = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, tid_id, gid, 0u});
            gidx_id_ = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, gidx_id_, in_gidx_});
        } else if (is_vs) {
            uint32_t vi = AllocId(); EmitInst(sec_fn_, OpLoad, {t_i32_, vi, in_vid_});
            tid_id = AllocId(); EmitInst(sec_fn_, OpBitcast, {t_u32_, tid_id, vi});
        }

        StoreVgpr(0u, tid_id);
        StoreVgpr(1u, EmitBinary(OpIMul, t_u32_, tid_id, c_u4_));
        StoreVgpr(2u, EmitBinary(OpIMul, t_u32_, tid_id, ConstU32(16u)));
        StoreVgpr(3u, EmitBinary(OpIMul, t_u32_, tid_id, ConstU32(8u)));
        for (uint32_t r = 4u; r < 64u; ++r) {
            StoreVgpr(r, c_u0_);
        }
        if (usage_.uses_scratch) {
            for (uint32_t s = 0u; s < 16u; ++s) {
                uint32_t p = AllocId();
                EmitInst(sec_fn_, OpAccessChain, {t_ptr_fn_u32_, p, var_scratch_, ConstU32(s)});
                EmitInst(sec_fn_, OpStore, {p, c_u0_});
            }
        }
        if (is_compute && usage_.uses_ds_counter) {
            EmitInst(sec_fn_, OpStore, {var_ds_counter_, c_u0_});
            EmitWorkgroupBarrier();
        }

        // Emit body for each instruction
        for (const auto& di : insts_) {
            const auto& inst = di.base;
            if (inst.encoding == Rdna2Encoding::SOPP) {
                if (inst.opcode == Rdna2Opcode::S_BARRIER && is_compute) EmitWorkgroupBarrier();
                else if (inst.opcode == Rdna2Opcode::S_ENDPGM) break;
                continue;
            }
            if (!di.entry) continue;
            EmitInstruction(di, is_compute, is_vs, is_ps);
        }

        EmitInst(sec_fn_, OpReturn, {});
        EmitInst(sec_fn_, OpFunctionEnd, {});

        // Assemble final SPIR-V 1.3 binary
        out.push_back(kSpvMagic);
        out.push_back(kSpvVer13);
        out.push_back(kSpvGenId);
        out.push_back(next_id_);
        out.push_back(0u);
        auto append_sec = [&](const std::vector<uint32_t>& s) {
            out.insert(out.end(), s.begin(), s.end());
        };
        append_sec(sec_caps_);
        append_sec(sec_ext_);
        append_sec(sec_mem_);
        append_sec(sec_entry_);
        append_sec(sec_exec_);
        append_sec(sec_decor_);
        append_sec(sec_types_);
        append_sec(sec_globals_);
        append_sec(sec_fn_);
        return Status::Ok();
    }

private:
    uint32_t AllocId() { return next_id_++; }

    static void EmitInst(std::vector<uint32_t>& buf, uint16_t op, const std::vector<uint32_t>& ops) {
        buf.push_back((static_cast<uint32_t>(ops.size() + 1u) << 16) | static_cast<uint32_t>(op));
        buf.insert(buf.end(), ops.begin(), ops.end());
    }

    static void AppendString(std::vector<uint32_t>& ops, const char* s) {
        const size_t len = std::strlen(s) + 1u;
        const size_t words = (len + 3u) / 4u;
        for (size_t w = 0; w < words; ++w) {
            uint32_t v = 0;
            for (size_t b = 0; b < 4u; ++b) {
                const size_t idx = w * 4u + b;
                if (idx < len) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[idx])) << (b * 8u);
            }
            ops.push_back(v);
        }
    }

    uint32_t ConstU32(uint32_t val) {
        auto it = const_u32_.find(val);
        if (it != const_u32_.end()) return it->second;
        const uint32_t id = AllocId();
        EmitInst(sec_types_, OpConstant, {t_u32_, id, val});
        const_u32_[val] = id;
        return id;
    }

    uint32_t ConstI32(int32_t val) {
        const uint32_t bits = static_cast<uint32_t>(val);
        auto it = const_i32_.find(bits);
        if (it != const_i32_.end()) return it->second;
        const uint32_t id = AllocId();
        EmitInst(sec_types_, OpConstant, {t_i32_, id, bits});
        const_i32_[bits] = id;
        return id;
    }

    uint32_t ConstF32(float val) {
        const uint32_t bits = FloatToBits(val);
        auto it = const_f32_.find(bits);
        if (it != const_f32_.end()) return it->second;
        const uint32_t id = AllocId();
        EmitInst(sec_types_, OpConstant, {t_f32_, id, bits});
        const_f32_[bits] = id;
        return id;
    }

    uint32_t LoadVgpr(uint32_t r) {
        const uint32_t ptr = AllocId();
        EmitInst(sec_fn_, OpAccessChain, {t_ptr_fn_u32_, ptr, var_vgpr_, ConstU32(r & 63u)});
        const uint32_t val = AllocId();
        EmitInst(sec_fn_, OpLoad, {t_u32_, val, ptr});
        return val;
    }

    void StoreVgpr(uint32_t r, uint32_t val) {
        const uint32_t ptr = AllocId();
        EmitInst(sec_fn_, OpAccessChain, {t_ptr_fn_u32_, ptr, var_vgpr_, ConstU32(r & 63u)});
        EmitInst(sec_fn_, OpStore, {ptr, val});
    }

    uint32_t EmitBinary(uint16_t op, uint32_t ty, uint32_t a, uint32_t b) {
        const uint32_t id = AllocId();
        EmitInst(sec_fn_, op, {ty, id, a, b});
        return id;
    }

    uint32_t BitcastF32(uint32_t u) {
        const uint32_t id = AllocId();
        EmitInst(sec_fn_, OpBitcast, {t_f32_, id, u});
        return id;
    }

    uint32_t BitcastU32(uint32_t f) {
        const uint32_t id = AllocId();
        EmitInst(sec_fn_, OpBitcast, {t_u32_, id, f});
        return id;
    }

    uint32_t BitcastI32(uint32_t u) {
        const uint32_t id = AllocId();
        EmitInst(sec_fn_, OpBitcast, {t_i32_, id, u});
        return id;
    }

    void EmitWorkgroupBarrier() {
        EmitInst(sec_fn_, OpControlBarrier, {c_u2_, c_u2_, ConstU32(0x108u)});
    }

    uint32_t SsboPtrAtDword(uint32_t ssbo_var, uint32_t dword_idx) {
        const uint32_t ptr = AllocId();
        EmitInst(sec_fn_, OpAccessChain, {t_ptr_ssbo_u32_, ptr, ssbo_var, c_u0_, dword_idx});
        return ptr;
    }

    uint32_t LoadSsboByte(uint32_t ssbo_var, uint32_t byte_addr) {
        const uint32_t dw = EmitBinary(OpShiftRightLogical, t_u32_, byte_addr, c_u2_);
        const uint32_t sh = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, byte_addr, c_u3_), c_u3_);
        const uint32_t w = AllocId();
        EmitInst(sec_fn_, OpLoad, {t_u32_, w, SsboPtrAtDword(ssbo_var, dw)});
        return EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, w, sh), ConstU32(0xFFu));
    }

    uint32_t LoadSsboU16(uint32_t ssbo_var, uint32_t byte_addr) {
        const uint32_t b0 = LoadSsboByte(ssbo_var, byte_addr);
        const uint32_t b1 = LoadSsboByte(ssbo_var, EmitBinary(OpIAdd, t_u32_, byte_addr, c_u1_));
        return EmitBinary(OpBitwiseOr, t_u32_, b0, EmitBinary(OpShiftLeftLogical, t_u32_, b1, ConstU32(8u)));
    }

    void StoreSsboByte(uint32_t ssbo_var, uint32_t byte_addr, uint32_t val_u32) {
        const uint32_t dw = EmitBinary(OpShiftRightLogical, t_u32_, byte_addr, c_u2_);
        const uint32_t sh = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, byte_addr, c_u3_), c_u3_);
        const uint32_t ptr = SsboPtrAtDword(ssbo_var, dw);
        const uint32_t mask = AllocId();
        EmitInst(sec_fn_, OpNot, {t_u32_, mask, EmitBinary(OpShiftLeftLogical, t_u32_, ConstU32(0xFFu), sh)});
        const uint32_t tmp1 = AllocId();
        EmitInst(sec_fn_, OpAtomicAnd, {t_u32_, tmp1, ptr, c_u1_, c_u0_, mask});
        const uint32_t ins = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, val_u32, ConstU32(0xFFu)), sh);
        const uint32_t tmp2 = AllocId();
        EmitInst(sec_fn_, OpAtomicOr, {t_u32_, tmp2, ptr, c_u1_, c_u0_, ins});
    }

    void StoreSsboU16(uint32_t ssbo_var, uint32_t byte_addr, uint32_t val_u32) {
        StoreSsboByte(ssbo_var, byte_addr, val_u32);
        StoreSsboByte(ssbo_var, EmitBinary(OpIAdd, t_u32_, byte_addr, c_u1_),
                      EmitBinary(OpShiftRightLogical, t_u32_, val_u32, ConstU32(8u)));
    }

    uint32_t ComputeSpvBufAddr(const Rdna2Instruction& inst, uint8_t vaddr) {
        uint32_t addr = ConstU32(static_cast<uint32_t>(inst.mem_offset));
        if (inst.soffset >= 128u && inst.soffset <= 192u && inst.soffset != 128u) {
            addr = EmitBinary(OpIAdd, t_u32_, addr, ConstU32(static_cast<uint32_t>(inst.soffset - 128u)));
        }
        if (inst.idxen && inst.offen) {
            const uint32_t idx_off = EmitBinary(OpIMul, t_u32_, LoadVgpr(vaddr), ConstU32(16u));
            addr = EmitBinary(OpIAdd, t_u32_, addr, EmitBinary(OpIAdd, t_u32_, idx_off, LoadVgpr(vaddr + 1u)));
        } else if (inst.idxen) {
            addr = EmitBinary(OpIAdd, t_u32_, addr, EmitBinary(OpIMul, t_u32_, LoadVgpr(vaddr), ConstU32(16u)));
        } else if (inst.offen) {
            addr = EmitBinary(OpIAdd, t_u32_, addr, LoadVgpr(vaddr));
        }
        return addr;
    }

    uint32_t EmitSpvAtomicCas(
        uint32_t ptr,
        uint32_t scope,
        const std::function<uint32_t(uint32_t)>& compute_next) {
        const uint32_t init_val = AllocId();
        EmitInst(sec_fn_, OpLoad, {t_u32_, init_val, ptr});
        const uint32_t pred_lbl = cur_label_;
        const uint32_t lbl_hdr = AllocId();
        const uint32_t lbl_body = AllocId();
        const uint32_t lbl_cont = AllocId();
        const uint32_t lbl_merge = AllocId();
        EmitInst(sec_fn_, OpBranch, {lbl_hdr});

        EmitInst(sec_fn_, OpLabel, {lbl_hdr});
        const uint32_t phi_cur = AllocId();
        const uint32_t phi_it = AllocId();
        const uint32_t orig_val = AllocId();
        const uint32_t next_it = AllocId();
        EmitInst(sec_fn_, OpPhi, {t_u32_, phi_cur, init_val, pred_lbl, orig_val, lbl_cont});
        EmitInst(sec_fn_, OpPhi, {t_u32_, phi_it, c_u0_, pred_lbl, next_it, lbl_cont});
        const uint32_t it_ok = EmitBinary(OpULessThan, t_bool_, phi_it, ConstU32(64u));
        EmitInst(sec_fn_, OpLoopMerge, {lbl_merge, lbl_cont, 0u});
        EmitInst(sec_fn_, OpBranchConditional, {it_ok, lbl_body, lbl_merge});

        EmitInst(sec_fn_, OpLabel, {lbl_body});
        const uint32_t next_val = compute_next(phi_cur);
        EmitInst(sec_fn_, OpAtomicCompareExchange, {t_u32_, orig_val, ptr, scope, c_u0_, c_u0_, next_val, phi_cur});
        const uint32_t eq = EmitBinary(OpIEqual, t_bool_, orig_val, phi_cur);
        EmitInst(sec_fn_, OpBranchConditional, {eq, lbl_merge, lbl_cont});

        EmitInst(sec_fn_, OpLabel, {lbl_cont});
        EmitInst(sec_fn_, OpIAdd, {t_u32_, next_it, phi_it, c_u1_});
        EmitInst(sec_fn_, OpBranch, {lbl_hdr});

        EmitInst(sec_fn_, OpLabel, {lbl_merge});
        const uint32_t out_prev = AllocId();
        EmitInst(sec_fn_, OpPhi, {t_u32_, out_prev, phi_cur, lbl_hdr, orig_val, lbl_body});
        cur_label_ = lbl_merge;
        return out_prev;
    }

    std::pair<uint32_t, uint32_t> EmitSpvAtomicAdd64(
        uint32_t ptr_lo,
        uint32_t ptr_hi,
        uint32_t scope,
        uint32_t add_lo,
        uint32_t add_hi) {
        const uint32_t prev_lo = AllocId();
        EmitInst(sec_fn_, OpAtomicIAdd, {t_u32_, prev_lo, ptr_lo, scope, c_u0_, add_lo});
        const uint32_t sum_lo = EmitBinary(OpIAdd, t_u32_, prev_lo, add_lo);
        const uint32_t wrapped = EmitBinary(OpULessThan, t_bool_, sum_lo, prev_lo);
        const uint32_t carry = AllocId();
        EmitInst(sec_fn_, OpSelect, {t_u32_, carry, wrapped, c_u1_, c_u0_});
        const uint32_t hi_inc = EmitBinary(OpIAdd, t_u32_, add_hi, carry);
        const uint32_t prev_hi = AllocId();
        EmitInst(sec_fn_, OpAtomicIAdd, {t_u32_, prev_hi, ptr_hi, scope, c_u0_, hi_inc});
        return {prev_lo, prev_hi};
    }

    uint32_t EmitSpvAtomic(uint32_t ptr, uint32_t scope, uint16_t op, uint32_t d0, uint32_t d1) {
        if (op == 0x4Eu) {
            return EmitSpvAtomicCas(ptr, scope, [&](uint32_t cur) {
                return EmitBinary(OpISub, t_u32_, d0, cur);
            });
        }
        if (op == 0x3Fu || op == 0x40u) {
            const uint32_t glsl_op = (op == 0x3Fu) ? 37u /*FMin*/ : 40u /*FMax*/;
            const uint32_t f_d0 = BitcastF32(d0);
            return EmitSpvAtomicCas(ptr, scope, [&](uint32_t cur) {
                const uint32_t r = AllocId();
                EmitInst(sec_fn_, OpExtInst, {t_f32_, r, glsl_ext_, glsl_op, BitcastF32(cur), f_d0});
                return BitcastU32(r);
            });
        }
        const uint32_t prev = AllocId();
        switch (op) {
            case 0x30: EmitInst(sec_fn_, OpAtomicExchange, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x31: EmitInst(sec_fn_, OpAtomicCompareExchange, {t_u32_, prev, ptr, scope, c_u0_, c_u0_, d0, d1}); break;
            case 0x32: EmitInst(sec_fn_, OpAtomicIAdd, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x33: EmitInst(sec_fn_, OpAtomicISub, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x35: EmitInst(sec_fn_, OpAtomicSMin, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x36: EmitInst(sec_fn_, OpAtomicUMin, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x37: EmitInst(sec_fn_, OpAtomicSMax, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x38: EmitInst(sec_fn_, OpAtomicUMax, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x39: EmitInst(sec_fn_, OpAtomicAnd, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x3A: EmitInst(sec_fn_, OpAtomicOr, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x3B: EmitInst(sec_fn_, OpAtomicXor, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
            case 0x3C: EmitInst(sec_fn_, OpAtomicIAdd, {t_u32_, prev, ptr, scope, c_u0_, c_u1_}); break;
            case 0x3D: EmitInst(sec_fn_, OpAtomicISub, {t_u32_, prev, ptr, scope, c_u0_, c_u1_}); break;
            default:   EmitInst(sec_fn_, OpAtomicIAdd, {t_u32_, prev, ptr, scope, c_u0_, d0}); break;
        }
        return prev;
    }

    void EmitInstruction(const MemImgDecodedInst& di, bool is_compute, bool is_vs, bool is_ps) {
        const auto& inst = di.base;
        const auto* e = di.entry;
        const uint16_t op = e->raw_opcode;

        if (inst.encoding == Rdna2Encoding::MUBUF || inst.encoding == Rdna2Encoding::MTBUF) {
            if (inst.encoding == Rdna2Encoding::MUBUF && (op == 0x71u || op == 0x72u)) {
                EmitInst(sec_fn_, OpMemoryBarrier, {c_u1_, ConstU32(0x40u)});
                return;
            }
            const uint32_t ssbo = (inst.srsrc == 0u) ? var_uav0_ : var_uav1_;
            const uint8_t vd = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t va = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const uint32_t baddr = ComputeSpvBufAddr(inst, va);
            const uint32_t dw0 = EmitBinary(OpShiftRightLogical, t_u32_, baddr, c_u2_);

            if (inst.encoding == Rdna2Encoding::MTBUF ||
                e->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferLoadFormat) ||
                e->semantic_class == static_cast<uint16_t>(MemImgSemanticClass::BufferStoreFormat)) {
                const Rdna2MtbufFormat fmt = (inst.encoding == Rdna2Encoding::MTBUF)
                                                 ? static_cast<Rdna2MtbufFormat>(inst.mtbuf_format)
                                                 : Rdna2MtbufFormat::R32G32B32A32_FLOAT;
                const uint32_t cb = MtbufFormatComponentBytes(fmt);
                auto pack_f16_lo = [&](uint32_t nf) -> uint32_t {
                    const uint32_t v2 = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_vec2_, v2, nf, c_f0_});
                    const uint32_t pk = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_u32_, pk, glsl_ext_, 58u, v2});
                    return EmitBinary(OpBitwiseAnd, t_u32_, pk, ConstU32(0xFFFFu));
                };
                auto unpack_f16_lo = [&](uint32_t h) -> uint32_t {
                    const uint32_t up = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_vec2_, up, glsl_ext_, 62u, h});
                    const uint32_t f0 = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, f0, up, 0u});
                    return f0;
                };
                auto load_fmt_comp = [&](uint32_t c, bool d16) -> uint32_t {
                    const uint32_t ca = EmitBinary(OpIAdd, t_u32_, baddr, ConstU32(c * cb));
                    if (cb == 1u) {
                        const uint32_t raw = LoadSsboByte(ssbo, ca);
                        if (fmt == Rdna2MtbufFormat::R8_UNORM || fmt == Rdna2MtbufFormat::R8G8_UNORM ||
                            fmt == Rdna2MtbufFormat::R8G8B8A8_UNORM) {
                            const uint32_t uf = AllocId(); EmitInst(sec_fn_, OpConvertUToF, {t_f32_, uf, raw});
                            const uint32_t nf = EmitBinary(OpFDiv, t_f32_, uf, ConstF32(255.0f));
                            return d16 ? pack_f16_lo(nf) : BitcastU32(nf);
                        }
                        if (fmt == Rdna2MtbufFormat::R8_SNORM || fmt == Rdna2MtbufFormat::R8G8_SNORM ||
                            fmt == Rdna2MtbufFormat::R8G8B8A8_SNORM) {
                            const uint32_t si = EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, raw, ConstU32(24u))), ConstI32(24));
                            const uint32_t sf = AllocId(); EmitInst(sec_fn_, OpConvertSToF, {t_f32_, sf, si});
                            const uint32_t dv = EmitBinary(OpFDiv, t_f32_, sf, ConstF32(127.0f));
                            const uint32_t cl = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_f32_, cl, glsl_ext_, 43u /*FClamp*/, dv, ConstF32(-1.0f), c_f1_});
                            return d16 ? pack_f16_lo(cl) : BitcastU32(cl);
                        }
                        if (fmt == Rdna2MtbufFormat::R8_SINT || fmt == Rdna2MtbufFormat::R8G8_SINT ||
                            fmt == Rdna2MtbufFormat::R8G8B8A8_SINT) {
                            const uint32_t si = EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, raw, ConstU32(24u))), ConstI32(24));
                            const uint32_t su = BitcastU32(si);
                            return d16 ? EmitBinary(OpBitwiseAnd, t_u32_, su, ConstU32(0xFFFFu)) : su;
                        }
                        return raw;
                    }
                    if (cb == 2u) {
                        const uint32_t raw = LoadSsboU16(ssbo, ca);
                        if (fmt == Rdna2MtbufFormat::R16_UNORM || fmt == Rdna2MtbufFormat::R16G16_UNORM ||
                            fmt == Rdna2MtbufFormat::R16G16B16A16_UNORM) {
                            const uint32_t uf = AllocId(); EmitInst(sec_fn_, OpConvertUToF, {t_f32_, uf, raw});
                            const uint32_t nf = EmitBinary(OpFDiv, t_f32_, uf, ConstF32(65535.0f));
                            return d16 ? pack_f16_lo(nf) : BitcastU32(nf);
                        }
                        if (fmt == Rdna2MtbufFormat::R16_SNORM || fmt == Rdna2MtbufFormat::R16G16_SNORM ||
                            fmt == Rdna2MtbufFormat::R16G16B16A16_SNORM) {
                            const uint32_t si = EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, raw, ConstU32(16u))), ConstI32(16));
                            const uint32_t sf = AllocId(); EmitInst(sec_fn_, OpConvertSToF, {t_f32_, sf, si});
                            const uint32_t dv = EmitBinary(OpFDiv, t_f32_, sf, ConstF32(32767.0f));
                            const uint32_t cl = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_f32_, cl, glsl_ext_, 43u /*FClamp*/, dv, ConstF32(-1.0f), c_f1_});
                            return d16 ? pack_f16_lo(cl) : BitcastU32(cl);
                        }
                        if (fmt == Rdna2MtbufFormat::R16_SINT || fmt == Rdna2MtbufFormat::R16G16_SINT ||
                            fmt == Rdna2MtbufFormat::R16G16B16A16_SINT) {
                            const uint32_t si = EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, raw, ConstU32(16u))), ConstI32(16));
                            const uint32_t su = BitcastU32(si);
                            return d16 ? EmitBinary(OpBitwiseAnd, t_u32_, su, ConstU32(0xFFFFu)) : su;
                        }
                        if (fmt == Rdna2MtbufFormat::R16_FLOAT || fmt == Rdna2MtbufFormat::R16G16_FLOAT ||
                            fmt == Rdna2MtbufFormat::R16G16B16A16_FLOAT) {
                            return d16 ? raw : BitcastU32(unpack_f16_lo(raw));
                        }
                        return raw;
                    }
                    const uint32_t w = AllocId();
                    EmitInst(sec_fn_, OpLoad, {t_u32_, w, SsboPtrAtDword(ssbo, EmitBinary(OpShiftRightLogical, t_u32_, ca, c_u2_))});
                    if (d16 && fmt != Rdna2MtbufFormat::R32_UINT && fmt != Rdna2MtbufFormat::R32_SINT) {
                        return pack_f16_lo(BitcastF32(w));
                    }
                    return d16 ? EmitBinary(OpBitwiseAnd, t_u32_, w, ConstU32(0xFFFFu)) : w;
                };
                auto pack_fmt_comp = [&](uint32_t val_u32, bool from_d16) -> uint32_t {
                    const uint32_t fval = from_d16 ? unpack_f16_lo(EmitBinary(OpBitwiseAnd, t_u32_, val_u32, ConstU32(0xFFFFu)))
                                                   : BitcastF32(val_u32);
                    const uint32_t uval = from_d16 ? EmitBinary(OpBitwiseAnd, t_u32_, val_u32, ConstU32(0xFFFFu)) : val_u32;
                    auto pack_norm = [&](float lo_f, float hi_f, float scale_f, bool is_signed, uint32_t out_mask) -> uint32_t {
                        const uint32_t is_n = AllocId(); EmitInst(sec_fn_, OpIsNan, {t_bool_, is_n, fval});
                        const uint32_t clean = AllocId(); EmitInst(sec_fn_, OpSelect, {t_f32_, clean, is_n, c_f0_, fval});
                        const uint32_t cl = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_f32_, cl, glsl_ext_, 43u /*FClamp*/, clean, ConstF32(lo_f), ConstF32(hi_f)});
                        const uint32_t sc = EmitBinary(OpFMul, t_f32_, cl, ConstF32(scale_f));
                        const uint32_t rne = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_f32_, rne, glsl_ext_, 2u /*RoundEven*/, sc});
                        if (is_signed) {
                            const uint32_t si = AllocId(); EmitInst(sec_fn_, OpConvertFToS, {t_i32_, si, rne});
                            return EmitBinary(OpBitwiseAnd, t_u32_, BitcastU32(si), ConstU32(out_mask));
                        }
                        const uint32_t ui = AllocId(); EmitInst(sec_fn_, OpConvertFToU, {t_u32_, ui, rne});
                        return EmitBinary(OpBitwiseAnd, t_u32_, ui, ConstU32(out_mask));
                    };
                    if (fmt == Rdna2MtbufFormat::R8_UNORM || fmt == Rdna2MtbufFormat::R8G8_UNORM || fmt == Rdna2MtbufFormat::R8G8B8A8_UNORM) {
                        return pack_norm(0.0f, 1.0f, 255.0f, false, 0xFFu);
                    }
                    if (fmt == Rdna2MtbufFormat::R8_SNORM || fmt == Rdna2MtbufFormat::R8G8_SNORM || fmt == Rdna2MtbufFormat::R8G8B8A8_SNORM) {
                        return pack_norm(-1.0f, 1.0f, 127.0f, true, 0xFFu);
                    }
                    if (fmt == Rdna2MtbufFormat::R8_UINT || fmt == Rdna2MtbufFormat::R8G8_UINT || fmt == Rdna2MtbufFormat::R8G8B8A8_UINT ||
                        fmt == Rdna2MtbufFormat::R8_SINT || fmt == Rdna2MtbufFormat::R8G8_SINT || fmt == Rdna2MtbufFormat::R8G8B8A8_SINT) {
                        return EmitBinary(OpBitwiseAnd, t_u32_, uval, ConstU32(0xFFu));
                    }
                    if (fmt == Rdna2MtbufFormat::R16_UNORM || fmt == Rdna2MtbufFormat::R16G16_UNORM || fmt == Rdna2MtbufFormat::R16G16B16A16_UNORM) {
                        return pack_norm(0.0f, 1.0f, 65535.0f, false, 0xFFFFu);
                    }
                    if (fmt == Rdna2MtbufFormat::R16_SNORM || fmt == Rdna2MtbufFormat::R16G16_SNORM || fmt == Rdna2MtbufFormat::R16G16B16A16_SNORM) {
                        return pack_norm(-1.0f, 1.0f, 32767.0f, true, 0xFFFFu);
                    }
                    if (fmt == Rdna2MtbufFormat::R16_UINT || fmt == Rdna2MtbufFormat::R16G16_UINT || fmt == Rdna2MtbufFormat::R16G16B16A16_UINT ||
                        fmt == Rdna2MtbufFormat::R16_SINT || fmt == Rdna2MtbufFormat::R16G16_SINT || fmt == Rdna2MtbufFormat::R16G16B16A16_SINT) {
                        return EmitBinary(OpBitwiseAnd, t_u32_, uval, ConstU32(0xFFFFu));
                    }
                    if (fmt == Rdna2MtbufFormat::R16_FLOAT || fmt == Rdna2MtbufFormat::R16G16_FLOAT || fmt == Rdna2MtbufFormat::R16G16B16A16_FLOAT) {
                        return from_d16 ? EmitBinary(OpBitwiseAnd, t_u32_, val_u32, ConstU32(0xFFFFu)) : pack_f16_lo(fval);
                    }
                    if (fmt == Rdna2MtbufFormat::R32_UINT || fmt == Rdna2MtbufFormat::R32_SINT) {
                        return uval;
                    }
                    return BitcastU32(fval);
                };
                if (op <= 0x03u) {
                    const uint32_t comps = static_cast<uint32_t>(op) + 1u;
                    for (uint32_t c = 0; c < comps; ++c) StoreVgpr(vd + c, load_fmt_comp(c, false));
                } else if (op >= 0x08u && op <= 0x0Bu) {
                    const uint32_t comps = static_cast<uint32_t>(op - 0x08u) + 1u;
                    uint32_t hs[4] = {c_u0_, c_u0_, c_u0_, c_u0_};
                    for (uint32_t c = 0; c < comps; ++c) hs[c] = load_fmt_comp(c, true);
                    if (comps == 1u) {
                        const uint32_t keep_hi = EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), ConstU32(0xFFFF0000u));
                        StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, keep_hi, hs[0]));
                    } else {
                        StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, hs[0], EmitBinary(OpShiftLeftLogical, t_u32_, hs[1], ConstU32(16u))));
                    }
                    if (comps == 3u) {
                        const uint32_t keep_hi = EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd + 1u), ConstU32(0xFFFF0000u));
                        StoreVgpr(vd + 1u, EmitBinary(OpBitwiseOr, t_u32_, keep_hi, hs[2]));
                    } else if (comps == 4u) {
                        StoreVgpr(vd + 1u, EmitBinary(OpBitwiseOr, t_u32_, hs[2], EmitBinary(OpShiftLeftLogical, t_u32_, hs[3], ConstU32(16u))));
                    }
                } else if (op == 0x26u || op == 0x10u) {
                    const uint32_t lo = EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), ConstU32(0xFFFFu));
                    const uint32_t hi = EmitBinary(OpShiftLeftLogical, t_u32_, load_fmt_comp(0u, true), ConstU32(16u));
                    StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, lo, hi));
                } else {
                    const bool is_d16_hi = (op == 0x11u || op == 0x27u);
                    const bool is_d16 = is_d16_hi || (op >= 0x0Cu && op <= 0x0Fu);
                    const uint32_t comps = (op >= 0x04u && op <= 0x07u)
                                               ? (static_cast<uint32_t>(op - 0x04u) + 1u)
                                               : ((op >= 0x0Cu && op <= 0x0Fu) ? (static_cast<uint32_t>(op - 0x0Cu) + 1u) : 1u);
                    for (uint32_t c = 0; c < comps; ++c) {
                        uint32_t src_u = c_u0_;
                        if (is_d16_hi) {
                            src_u = EmitBinary(OpShiftRightLogical, t_u32_, LoadVgpr(vd), ConstU32(16u));
                        } else if (is_d16) {
                            const uint32_t w = LoadVgpr(vd + (c >> 1u));
                            src_u = ((c & 1u) == 0u) ? EmitBinary(OpBitwiseAnd, t_u32_, w, ConstU32(0xFFFFu))
                                                     : EmitBinary(OpShiftRightLogical, t_u32_, w, ConstU32(16u));
                        } else {
                            src_u = LoadVgpr(vd + c);
                        }
                        const uint32_t pk = pack_fmt_comp(src_u, is_d16);
                        const uint32_t ca = EmitBinary(OpIAdd, t_u32_, baddr, ConstU32(c * cb));
                        if (cb == 1u) {
                            StoreSsboByte(ssbo, ca, pk);
                        } else if (cb == 2u) {
                            StoreSsboU16(ssbo, ca, pk);
                        } else {
                            EmitInst(sec_fn_, OpStore, {SsboPtrAtDword(ssbo, EmitBinary(OpShiftRightLogical, t_u32_, ca, c_u2_)), pk});
                        }
                    }
                }
            } else if (e->is_atomic) {
                if (op == 0x52u) {
                    const uint32_t ptr_lo = SsboPtrAtDword(ssbo, dw0);
                    const uint32_t ptr_hi = SsboPtrAtDword(ssbo, EmitBinary(OpIAdd, t_u32_, dw0, c_u1_));
                    const auto prev64 = EmitSpvAtomicAdd64(ptr_lo, ptr_hi, c_u1_, LoadVgpr(vd), LoadVgpr(vd + 1u));
                    if (inst.glc) {
                        StoreVgpr(vd, prev64.first);
                        StoreVgpr(vd + 1u, prev64.second);
                    }
                } else {
                    const uint32_t ptr = SsboPtrAtDword(ssbo, dw0);
                    const uint32_t prev = EmitSpvAtomic(ptr, c_u1_, op, LoadVgpr(vd), LoadVgpr(vd + 1u));
                    if (inst.glc) StoreVgpr(vd, prev);
                }
            } else {
                switch (op) {
                    case 0x10: StoreVgpr(vd, LoadSsboByte(ssbo, baddr)); break;
                    case 0x11: {
                        const uint32_t b = LoadSsboByte(ssbo, baddr);
                        const uint32_t s = EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, b, ConstU32(24u))), ConstI32(24));
                        StoreVgpr(vd, BitcastU32(s));
                        break;
                    }
                    case 0x12: StoreVgpr(vd, LoadSsboU16(ssbo, baddr)); break;
                    case 0x13: {
                        const uint32_t h = LoadSsboU16(ssbo, baddr);
                        const uint32_t s = EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, h, ConstU32(16u))), ConstI32(16));
                        StoreVgpr(vd, BitcastU32(s));
                        break;
                    }
                    case 0x14:
                    case 0x15:
                    case 0x16:
                    case 0x17:
                        for (uint32_t k = 0; k < e->data_dwords; ++k) {
                            const uint32_t w = AllocId();
                            EmitInst(sec_fn_, OpLoad, {t_u32_, w, SsboPtrAtDword(ssbo, EmitBinary(OpIAdd, t_u32_, dw0, ConstU32(k)))});
                            StoreVgpr(vd + k, w);
                        }
                        break;
                    case 0x18: StoreSsboByte(ssbo, baddr, LoadVgpr(vd)); break;
                    case 0x19: StoreSsboByte(ssbo, baddr, EmitBinary(OpShiftRightLogical, t_u32_, LoadVgpr(vd), ConstU32(16u))); break;
                    case 0x1A: StoreSsboU16(ssbo, baddr, LoadVgpr(vd)); break;
                    case 0x1B: StoreSsboU16(ssbo, baddr, EmitBinary(OpShiftRightLogical, t_u32_, LoadVgpr(vd), ConstU32(16u))); break;
                    case 0x1C:
                    case 0x1D:
                    case 0x1E:
                    case 0x1F:
                        for (uint32_t k = 0; k < e->data_dwords; ++k) {
                            EmitInst(sec_fn_, OpStore, {SsboPtrAtDword(ssbo, EmitBinary(OpIAdd, t_u32_, dw0, ConstU32(k))), LoadVgpr(vd + k)});
                        }
                        break;
                    case 0x20: StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), ConstU32(0xFFFF0000u)), LoadSsboByte(ssbo, baddr))); break;
                    case 0x21: StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), ConstU32(0xFFFFu)), EmitBinary(OpShiftLeftLogical, t_u32_, LoadSsboByte(ssbo, baddr), ConstU32(16u)))); break;
                    case 0x24: StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), ConstU32(0xFFFF0000u)), LoadSsboU16(ssbo, baddr))); break;
                    case 0x25: StoreVgpr(vd, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), ConstU32(0xFFFFu)), EmitBinary(OpShiftLeftLogical, t_u32_, LoadSsboU16(ssbo, baddr), ConstU32(16u)))); break;
                    default: break;
                }
            }
        } else if (inst.encoding == Rdna2Encoding::MIMG) {
            const uint8_t dim = inst.mimg_dim & 7u;
            const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
            const uint8_t vd = static_cast<uint8_t>((e->is_store ? inst.src[0].reg_index : inst.dst.reg_index) & 63u);
            const uint8_t va = static_cast<uint8_t>(((e->is_store || e->is_atomic) ? inst.src[1].reg_index : inst.src[0].reg_index) & 63u);

            if (e->is_bvh) {
                // Branchless Möller-Trumbore triangle intersection reading 12 dwords from uav0[node_ptr * 12]
                const uint32_t sh = (op == 0xE7u) ? 1u : 0u;
                const uint32_t base_dw = EmitBinary(OpIMul, t_u32_, LoadVgpr(va), ConstU32(12u));
                auto load_bvh_f = [&](uint32_t k) -> uint32_t {
                    const uint32_t w = AllocId();
                    EmitInst(sec_fn_, OpLoad, {t_u32_, w, SsboPtrAtDword(var_uav0_, EmitBinary(OpIAdd, t_u32_, base_dw, ConstU32(k)))});
                    return BitcastF32(w);
                };
                auto make_v3 = [&](uint32_t a, uint32_t b, uint32_t c) -> uint32_t {
                    const uint32_t id = AllocId();
                    EmitInst(sec_fn_, OpCompositeConstruct, {t_vec3_, id, a, b, c});
                    return id;
                };
                const uint32_t p0 = make_v3(load_bvh_f(0u), load_bvh_f(1u), load_bvh_f(2u));
                const uint32_t p1 = make_v3(load_bvh_f(3u), load_bvh_f(4u), load_bvh_f(5u));
                const uint32_t p2 = make_v3(load_bvh_f(6u), load_bvh_f(7u), load_bvh_f(8u));
                const uint32_t prim = AllocId();
                EmitInst(sec_fn_, OpLoad, {t_u32_, prim, SsboPtrAtDword(var_uav0_, EmitBinary(OpIAdd, t_u32_, base_dw, ConstU32(9u)))});
                const uint32_t tmax = BitcastF32(LoadVgpr(va + 1u + sh));
                const uint32_t orig = make_v3(BitcastF32(LoadVgpr(va + 2u + sh)), BitcastF32(LoadVgpr(va + 3u + sh)), BitcastF32(LoadVgpr(va + 4u + sh)));
                const uint32_t dir = make_v3(BitcastF32(LoadVgpr(va + 5u + sh)), BitcastF32(LoadVgpr(va + 6u + sh)), BitcastF32(LoadVgpr(va + 7u + sh)));
                const uint32_t e1 = EmitBinary(OpFSub, t_vec3_, p1, p0);
                const uint32_t e2 = EmitBinary(OpFSub, t_vec3_, p2, p0);
                const uint32_t pv = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_vec3_, pv, glsl_ext_, 68u, dir, e2});
                const uint32_t det = EmitBinary(OpDot, t_f32_, e1, pv);
                const uint32_t adet = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_f32_, adet, glsl_ext_, 4u, det});
                const uint32_t det_ok = EmitBinary(OpFOrdGreaterThan, t_bool_, adet, ConstF32(1e-8f));
                const uint32_t safe_det = AllocId(); EmitInst(sec_fn_, OpSelect, {t_f32_, safe_det, det_ok, det, c_f1_});
                const uint32_t inv_det = EmitBinary(OpFDiv, t_f32_, c_f1_, safe_det);
                const uint32_t tv = EmitBinary(OpFSub, t_vec3_, orig, p0);
                const uint32_t bu = EmitBinary(OpFMul, t_f32_, EmitBinary(OpDot, t_f32_, tv, pv), inv_det);
                const uint32_t qv = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_vec3_, qv, glsl_ext_, 68u, tv, e1});
                const uint32_t bv = EmitBinary(OpFMul, t_f32_, EmitBinary(OpDot, t_f32_, dir, qv), inv_det);
                const uint32_t tval = EmitBinary(OpFMul, t_f32_, EmitBinary(OpDot, t_f32_, e2, qv), inv_det);
                const uint32_t c0 = EmitBinary(OpFOrdGreaterThanEqual, t_bool_, bu, c_f0_);
                const uint32_t c1 = EmitBinary(OpFOrdGreaterThanEqual, t_bool_, bv, c_f0_);
                const uint32_t c2 = EmitBinary(OpFOrdLessThanEqual, t_bool_, EmitBinary(OpFAdd, t_f32_, bu, bv), c_f1_);
                const uint32_t c3 = EmitBinary(OpFOrdGreaterThanEqual, t_bool_, tval, c_f0_);
                const uint32_t c4 = EmitBinary(OpFOrdLessThanEqual, t_bool_, tval, tmax);
                const uint32_t hit = EmitBinary(OpLogicalAnd, t_bool_, det_ok,
                                     EmitBinary(OpLogicalAnd, t_bool_, EmitBinary(OpLogicalAnd, t_bool_, c0, c1),
                                                EmitBinary(OpLogicalAnd, t_bool_, c2, EmitBinary(OpLogicalAnd, t_bool_, c3, c4))));
                const uint32_t out_t = AllocId(); EmitInst(sec_fn_, OpSelect, {t_f32_, out_t, hit, tval, tmax});
                const uint32_t out_u = AllocId(); EmitInst(sec_fn_, OpSelect, {t_f32_, out_u, hit, bu, c_f0_});
                const uint32_t out_v = AllocId(); EmitInst(sec_fn_, OpSelect, {t_f32_, out_v, hit, bv, c_f0_});
                const uint32_t hit_id = EmitBinary(OpBitwiseOr, t_u32_, prim, ConstU32(0x80000000u));
                const uint32_t out_id = AllocId(); EmitInst(sec_fn_, OpSelect, {t_u32_, out_id, hit, hit_id, c_u0_});
                StoreVgpr(vd + 0u, BitcastU32(out_t));
                StoreVgpr(vd + 1u, BitcastU32(out_u));
                StoreVgpr(vd + 2u, BitcastU32(out_v));
                StoreVgpr(vd + 3u, out_id);
            } else if (op == 0x0Eu) {
                const uint32_t img = AllocId(); EmitInst(sec_fn_, OpLoad, {t_tex_[dim], img, var_tex_[dim]});
                const uint32_t lod_i = BitcastI32(LoadVgpr(va));
                uint32_t rw = c_u1_, rh = c_u1_, rd = c_u1_, rm = c_u1_;
                if (dim == kMimgDim2DMsaa || dim == kMimgDim2DArrayMsaa) {
                    const uint32_t sz_ty = (dim == kMimgDim2DMsaa) ? t_uvec2_ : t_uvec3_;
                    const uint32_t sz = AllocId(); EmitInst(sec_fn_, OpImageQuerySize, {sz_ty, sz, img});
                    rw = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rw, sz, 0u});
                    rh = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rh, sz, 1u});
                    if (dim == kMimgDim2DArrayMsaa) { rd = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rd, sz, 2u}); }
                } else {
                    const uint32_t sz_ty = (dim == kMimgDim1D) ? t_u32_
                                         : ((dim == kMimgDim2D || dim == kMimgDimCube || dim == kMimgDim1DArray) ? t_uvec2_ : t_uvec3_);
                    const uint32_t sz = AllocId(); EmitInst(sec_fn_, OpImageQuerySizeLod, {sz_ty, sz, img, lod_i});
                    if (dim == kMimgDim1D) {
                        rw = sz;
                    } else {
                        rw = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rw, sz, 0u});
                        if (dim == kMimgDim1DArray) { rd = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rd, sz, 1u}); }
                        else {
                            rh = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rh, sz, 1u});
                            if (dim == kMimgDim3D || dim == kMimgDim2DArray) { rd = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_u32_, rd, sz, 2u}); }
                        }
                    }
                    rm = AllocId(); EmitInst(sec_fn_, OpImageQueryLevels, {t_u32_, rm, img});
                }
                const uint32_t vals[4] = {rw, rh, rd, rm};
                uint32_t dst_off = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if ((dmask & (1u << c)) != 0u) StoreVgpr(vd + dst_off++, vals[c]);
                }
            } else if (op == 0x60u) {
                StoreVgpr(vd, c_u0_);
                StoreVgpr(vd + 1u, c_u0_);
            } else if (e->is_store) {
                const uint32_t img = AllocId(); EmitInst(sec_fn_, OpLoad, {t_rwtex_f4_, img, var_rwtex_f4_});
                const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_ivec2_, coord, BitcastI32(LoadVgpr(va)), BitcastI32(LoadVgpr(va + 1u))});
                const uint32_t texel = AllocId();
                EmitInst(sec_fn_, OpCompositeConstruct, {t_vec4_, texel, BitcastF32(LoadVgpr(vd)), BitcastF32(LoadVgpr(vd + 1u)), BitcastF32(LoadVgpr(vd + 2u)), BitcastF32(LoadVgpr(vd + 3u))});
                EmitInst(sec_fn_, OpImageWrite, {img, coord, texel});
            } else if (e->is_atomic) {
                const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_ivec2_, coord, BitcastI32(LoadVgpr(va)), BitcastI32(LoadVgpr(va + 1u))});
                const uint32_t tptr = AllocId(); EmitInst(sec_fn_, OpImageTexelPointer, {t_ptr_img_u32_, tptr, var_rwtex_u32_, coord, c_u0_});
                const uint16_t mapped = static_cast<uint16_t>(op + 0x20u);  // 0x10..0x1F -> 0x30..0x3F
                const uint32_t prev = EmitSpvAtomic(tptr, c_u1_, mapped, LoadVgpr(vd), LoadVgpr(vd + 1u));
                if (inst.glc) StoreVgpr(vd, prev);
            } else if (e->is_load && !e->is_sample && !e->is_gather) {
                const uint32_t img = AllocId(); EmitInst(sec_fn_, OpLoad, {t_tex_[dim], img, var_tex_[dim]});
                const uint32_t texel = AllocId();
                if (dim == kMimgDimCube) {
                    const uint32_t samp = AllocId(); EmitInst(sec_fn_, OpLoad, {t_samp_, samp, var_samp_});
                    const uint32_t si = AllocId(); EmitInst(sec_fn_, OpSampledImage, {t_sampled_tex_[dim], si, img, samp});
                    const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_vec3_, coord, BitcastF32(LoadVgpr(va)), BitcastF32(LoadVgpr(va + 1u)), BitcastF32(LoadVgpr(va + 2u))});
                    EmitInst(sec_fn_, OpImageSampleExplicitLod, {t_vec4_, texel, si, coord, 2u, c_f0_});
                } else if (dim == kMimgDim1D) {
                    EmitInst(sec_fn_, OpImageFetch, {t_vec4_, texel, img, BitcastI32(LoadVgpr(va)), 2u, ConstI32(0)});
                } else if (dim == kMimgDim3D || dim == kMimgDim2DArray) {
                    const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_ivec3_, coord, BitcastI32(LoadVgpr(va)), BitcastI32(LoadVgpr(va + 1u)), BitcastI32(LoadVgpr(va + 2u))});
                    const bool has_mip = (op == 0x01u || op == 0x04u || op == 0x05u);
                    EmitInst(sec_fn_, OpImageFetch, {t_vec4_, texel, img, coord, 2u, has_mip ? BitcastI32(LoadVgpr(va + 3u)) : ConstI32(0)});
                } else if (dim == kMimgDim2DMsaa) {
                    const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_ivec2_, coord, BitcastI32(LoadVgpr(va)), BitcastI32(LoadVgpr(va + 1u))});
                    EmitInst(sec_fn_, OpImageFetch, {t_vec4_, texel, img, coord, 0x40u, BitcastI32(LoadVgpr(va + 2u))});
                } else if (dim == kMimgDim2DArrayMsaa) {
                    const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_ivec3_, coord, BitcastI32(LoadVgpr(va)), BitcastI32(LoadVgpr(va + 1u)), BitcastI32(LoadVgpr(va + 2u))});
                    EmitInst(sec_fn_, OpImageFetch, {t_vec4_, texel, img, coord, 0x40u, BitcastI32(LoadVgpr(va + 3u))});
                } else {
                    const uint32_t coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_ivec2_, coord, BitcastI32(LoadVgpr(va)), BitcastI32(LoadVgpr(va + 1u))});
                    const bool has_mip = (op == 0x01u || op == 0x04u || op == 0x05u);
                    EmitInst(sec_fn_, OpImageFetch, {t_vec4_, texel, img, coord, 2u, has_mip ? BitcastI32(LoadVgpr(va + 2u)) : ConstI32(0)});
                }
                uint32_t dst_off = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if ((dmask & (1u << c)) != 0u) {
                        const uint32_t ch = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, ch, texel, c});
                        StoreVgpr(vd + dst_off++, BitcastU32(ch));
                    }
                }
            } else {
                // Sample / Gather4
                const uint8_t low = static_cast<uint8_t>(op & 0x0Fu);
                const bool is_cmp = (low >= 0x08u);
                const bool has_off = ((op & 0x10u) != 0u);
                const uint8_t mode = low & 0x07u;
                const bool has_bias = (mode == 5u || mode == 6u);
                const bool has_lod = (mode == 4u);
                const bool has_grad = (mode == 2u || mode == 3u);
                uint32_t ai = va;
                if (has_off) ++ai;
                if (has_bias) ++ai;
                const uint32_t dref_r = ai;
                if (is_cmp) ++ai;
                if (has_grad) ai += (op >= 0xA0u) ? 2u : 4u;

                const uint32_t img = AllocId(); EmitInst(sec_fn_, OpLoad, {t_tex_[dim], img, var_tex_[dim]});
                const uint32_t samp = AllocId(); EmitInst(sec_fn_, OpLoad, {t_samp_, samp, var_samp_});
                const uint32_t si = AllocId(); EmitInst(sec_fn_, OpSampledImage, {t_sampled_tex_[dim], si, img, samp});
                uint32_t coord = c_f0_;
                uint32_t ccnt = 2u;
                if (dim == kMimgDim1D) {
                    coord = BitcastF32(LoadVgpr(ai));
                    ccnt = 1u;
                } else if (dim == kMimgDim3D || dim == kMimgDimCube || dim == kMimgDim2DArray) {
                    coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_vec3_, coord, BitcastF32(LoadVgpr(ai)), BitcastF32(LoadVgpr(ai + 1u)), BitcastF32(LoadVgpr(ai + 2u))});
                    ccnt = 3u;
                } else {
                    coord = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_vec2_, coord, BitcastF32(LoadVgpr(ai)), BitcastF32(LoadVgpr(ai + 1u))});
                    ccnt = 2u;
                }
                const uint32_t lod_f = has_lod ? BitcastF32(LoadVgpr(ai + ccnt)) : c_f0_;

                if (e->is_gather && (dim == kMimgDim2D || dim == kMimgDimCube || dim == kMimgDim2DArray)) {
                    const uint32_t g = AllocId();
                    if (is_cmp) {
                        EmitInst(sec_fn_, OpImageDrefGather, {t_vec4_, g, si, coord, BitcastF32(LoadVgpr(dref_r))});
                    } else {
                        uint32_t ch = 0u;
                        for (uint32_t c = 0; c < 4u; ++c) { if ((dmask & (1u << c)) != 0u) { ch = c; break; } }
                        EmitInst(sec_fn_, OpImageGather, {t_vec4_, g, si, coord, ConstU32(ch)});
                    }
                    for (uint32_t c = 0; c < 4u; ++c) {
                        const uint32_t el = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, el, g, c});
                        StoreVgpr(vd + c, BitcastU32(el));
                    }
                } else if (is_cmp) {
                    const uint32_t cf = AllocId();
                    EmitInst(sec_fn_, OpImageSampleDrefExplicitLod, {t_f32_, cf, si, coord, BitcastF32(LoadVgpr(dref_r)), 2u, lod_f});
                    uint32_t dst_off = 0;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        if ((dmask & (1u << c)) != 0u) StoreVgpr(vd + dst_off++, BitcastU32(cf));
                    }
                } else {
                    const uint32_t tx = AllocId();
                    EmitInst(sec_fn_, OpImageSampleExplicitLod, {t_vec4_, tx, si, coord, 2u, lod_f});
                    uint32_t dst_off = 0;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        if ((dmask & (1u << c)) != 0u) {
                            const uint32_t el = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, el, tx, c});
                            StoreVgpr(vd + dst_off++, BitcastU32(el));
                        }
                    }
                }
            }
        } else if (inst.encoding == Rdna2Encoding::DS && is_compute) {
            const uint8_t vdst = static_cast<uint8_t>(inst.dst.reg_index & 63u);
            const uint8_t vaddr = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t d0 = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const uint8_t d1 = static_cast<uint8_t>(inst.src[2].reg_index & 63u);
            const uint32_t off16 = static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu;
            const uint32_t base_a = EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(off16));
            auto lds_ptr = [&](uint32_t b_addr) -> uint32_t {
                const uint32_t idx = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, b_addr, c_u2_), ConstU32(4095u));
                const uint32_t p = AllocId();
                EmitInst(sec_fn_, OpAccessChain, {t_ptr_wg_u32_, p, var_lds_, idx});
                return p;
            };
            auto wt_ptr = [&](uint32_t lane_idx) -> uint32_t {
                const uint32_t idx = EmitBinary(OpBitwiseAnd, t_u32_, lane_idx, ConstU32(63u));
                const uint32_t p = AllocId();
                EmitInst(sec_fn_, OpAccessChain, {t_ptr_wg_u32_, p, var_wave_tmp_, idx});
                return p;
            };

            if (op == 0x14u) {
                // ds_nop
            } else if (op >= 0x18u && op <= 0x1Du) {
                EmitWorkgroupBarrier();
            } else if (op == 0x35u) {
                const uint32_t wmask = ConstU32(std::max(1u, container_.threadgroup_x) - 1u);
                EmitInst(sec_fn_, OpStore, {wt_ptr(gidx_id_), LoadVgpr(vaddr)});
                EmitWorkgroupBarrier();
                uint32_t sl = gidx_id_;
                if ((off16 & 0x8000u) != 0u) {
                    const uint32_t qb = EmitBinary(OpBitwiseAnd, t_u32_, gidx_id_, ConstU32(~3u));
                    const uint32_t sh = EmitBinary(OpIMul, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, gidx_id_, c_u3_), c_u2_);
                    const uint32_t sel = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, ConstU32(off16), sh), c_u3_);
                    sl = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpBitwiseOr, t_u32_, qb, sel), wmask);
                } else {
                    const uint32_t lo = EmitBinary(OpBitwiseAnd, t_u32_, gidx_id_, ConstU32(31u));
                    const uint32_t hi = EmitBinary(OpBitwiseAnd, t_u32_, gidx_id_, ConstU32(~31u));
                    const uint32_t m1 = EmitBinary(OpBitwiseAnd, t_u32_, lo, ConstU32(off16 & 0x1Fu));
                    const uint32_t m2 = EmitBinary(OpBitwiseOr, t_u32_, m1, ConstU32((off16 >> 5) & 0x1Fu));
                    const uint32_t m3 = EmitBinary(OpBitwiseXor, t_u32_, m2, ConstU32((off16 >> 10) & 0x1Fu));
                    sl = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpBitwiseOr, t_u32_, hi, m3), wmask);
                }
                const uint32_t r = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r, wt_ptr(sl)});
                StoreVgpr(vdst, r);
                EmitWorkgroupBarrier();
            } else if (op == 0xB2u) {
                const uint32_t wmask = ConstU32(std::max(1u, container_.threadgroup_x) - 1u);
                EmitInst(sec_fn_, OpStore, {wt_ptr(gidx_id_), c_u0_});
                EmitWorkgroupBarrier();
                const uint32_t tgt = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, LoadVgpr(vaddr), c_u2_), wmask);
                EmitInst(sec_fn_, OpStore, {wt_ptr(tgt), LoadVgpr(d0)});
                EmitWorkgroupBarrier();
                const uint32_t r = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r, wt_ptr(gidx_id_)});
                StoreVgpr(vdst, r);
                EmitWorkgroupBarrier();
            } else if (op == 0xB3u) {
                const uint32_t wmask = ConstU32(std::max(1u, container_.threadgroup_x) - 1u);
                EmitInst(sec_fn_, OpStore, {wt_ptr(gidx_id_), LoadVgpr(d0)});
                EmitWorkgroupBarrier();
                const uint32_t src = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, LoadVgpr(vaddr), c_u2_), wmask);
                const uint32_t r = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r, wt_ptr(src)});
                StoreVgpr(vdst, r);
                EmitWorkgroupBarrier();
            } else if (op == 0x3Eu || op == 0x3Fu) {
                const uint32_t prev = AllocId();
                EmitInst(sec_fn_, OpAtomicIAdd, {t_u32_, prev, var_ds_counter_, c_u2_, c_u0_, c_u1_});
                StoreVgpr(vdst, prev);
            } else if (op == 0x3Du) {
                const uint32_t prev = AllocId();
                EmitInst(sec_fn_, OpAtomicISub, {t_u32_, prev, var_ds_counter_, c_u2_, c_u0_, c_u1_});
                const uint32_t nz = EmitBinary(OpUGreaterThanEqual, t_bool_, prev, c_u1_);
                const uint32_t dec = EmitBinary(OpISub, t_u32_, prev, c_u1_);
                const uint32_t out_v = AllocId(); EmitInst(sec_fn_, OpSelect, {t_u32_, out_v, nz, dec, c_u0_});
                StoreVgpr(vdst, out_v);
            } else if (op == 0x0Du) {
                EmitInst(sec_fn_, OpStore, {lds_ptr(base_a), LoadVgpr(d0)});
            } else if (op == 0x0Eu || op == 0x0Fu) {
                const uint32_t sc = (op == 0x0Fu) ? 256u : 4u;
                EmitInst(sec_fn_, OpStore, {lds_ptr(EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset0) * sc))), LoadVgpr(d0)});
                EmitInst(sec_fn_, OpStore, {lds_ptr(EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset1) * sc))), LoadVgpr(d1)});
            } else if (op == 0x4Du || op == 0xDEu || op == 0xDFu) {
                for (uint32_t k = 0; k < e->data_dwords; ++k) {
                    EmitInst(sec_fn_, OpStore, {lds_ptr(EmitBinary(OpIAdd, t_u32_, base_a, ConstU32(k * 4u))), LoadVgpr(d0 + k)});
                }
            } else if (op == 0x4Eu || op == 0x4Fu) {
                const uint32_t sc = (op == 0x4Fu) ? 512u : 8u;
                const uint32_t a0 = EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset0) * sc));
                const uint32_t a1 = EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset1) * sc));
                EmitInst(sec_fn_, OpStore, {lds_ptr(a0), LoadVgpr(d0)});
                EmitInst(sec_fn_, OpStore, {lds_ptr(EmitBinary(OpIAdd, t_u32_, a0, c_u4_)), LoadVgpr(d0 + 1u)});
                EmitInst(sec_fn_, OpStore, {lds_ptr(a1), LoadVgpr(d1)});
                EmitInst(sec_fn_, OpStore, {lds_ptr(EmitBinary(OpIAdd, t_u32_, a1, c_u4_)), LoadVgpr(d1 + 1u)});
            } else if (op == 0x36u) {
                const uint32_t r = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r, lds_ptr(base_a)});
                StoreVgpr(vdst, r);
            } else if (op == 0x37u || op == 0x38u) {
                const uint32_t sc = (op == 0x38u) ? 256u : 4u;
                const uint32_t r0 = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r0, lds_ptr(EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset0) * sc)))});
                const uint32_t r1 = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r1, lds_ptr(EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset1) * sc)))});
                StoreVgpr(vdst, r0); StoreVgpr(vdst + 1u, r1);
            } else if (op == 0x76u || op == 0xFEu || op == 0xFFu) {
                for (uint32_t k = 0; k < e->data_dwords; ++k) {
                    const uint32_t r = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r, lds_ptr(EmitBinary(OpIAdd, t_u32_, base_a, ConstU32(k * 4u)))});
                    StoreVgpr(vdst + k, r);
                }
            } else if (op == 0x77u || op == 0x78u) {
                const uint32_t sc = (op == 0x78u) ? 512u : 8u;
                const uint32_t a0 = EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset0) * sc));
                const uint32_t a1 = EmitBinary(OpIAdd, t_u32_, LoadVgpr(vaddr), ConstU32(static_cast<uint32_t>(inst.ds_offset1) * sc));
                const uint32_t r0 = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r0, lds_ptr(a0)});
                const uint32_t r1 = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r1, lds_ptr(EmitBinary(OpIAdd, t_u32_, a0, c_u4_))});
                const uint32_t r2 = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r2, lds_ptr(a1)});
                const uint32_t r3 = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r3, lds_ptr(EmitBinary(OpIAdd, t_u32_, a1, c_u4_))});
                StoreVgpr(vdst, r0); StoreVgpr(vdst + 1u, r1); StoreVgpr(vdst + 2u, r2); StoreVgpr(vdst + 3u, r3);
            } else if (op == 0x1Eu || op == 0x1Fu || op == 0xA0u || op == 0xA1u) {
                const bool is16 = (op == 0x1Fu || op == 0xA1u);
                const uint32_t val = (op == 0xA0u || op == 0xA1u) ? EmitBinary(OpShiftRightLogical, t_u32_, LoadVgpr(d0), ConstU32(16u)) : LoadVgpr(d0);
                const uint32_t sh = EmitBinary(OpIMul, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, base_a, is16 ? c_u2_ : c_u3_), ConstU32(8u));
                const uint32_t fmask = ConstU32(is16 ? 0xFFFFu : 0xFFu);
                const uint32_t clrm = AllocId(); EmitInst(sec_fn_, OpNot, {t_u32_, clrm, EmitBinary(OpShiftLeftLogical, t_u32_, fmask, sh)});
                const uint32_t p = lds_ptr(base_a);
                const uint32_t t1 = AllocId(); EmitInst(sec_fn_, OpAtomicAnd, {t_u32_, t1, p, c_u2_, c_u0_, clrm});
                const uint32_t t2 = AllocId(); EmitInst(sec_fn_, OpAtomicOr, {t_u32_, t2, p, c_u2_, c_u0_, EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, val, fmask), sh)});
            } else if (op == 0x39u || op == 0x3Au || op == 0x3Bu || op == 0x3Cu) {
                const bool is16 = (op == 0x3Bu || op == 0x3Cu);
                const bool sgn = (op == 0x39u || op == 0x3Bu);
                const uint32_t w = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, w, lds_ptr(base_a)});
                const uint32_t sh = EmitBinary(OpIMul, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, base_a, is16 ? c_u2_ : c_u3_), ConstU32(8u));
                uint32_t v = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, w, sh), ConstU32(is16 ? 0xFFFFu : 0xFFu));
                if (sgn) {
                    const uint32_t sb = is16 ? 16u : 24u;
                    v = BitcastU32(EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, v, ConstU32(sb))), ConstI32(static_cast<int32_t>(sb))));
                }
                StoreVgpr(vdst, v);
            } else if (e->is_atomic) {
                if (op == 0x40u || op == 0x60u) {
                    const uint32_t p_lo = lds_ptr(base_a);
                    const uint32_t p_hi = lds_ptr(EmitBinary(OpIAdd, t_u32_, base_a, c_u4_));
                    const auto prev64 = EmitSpvAtomicAdd64(p_lo, p_hi, c_u2_, LoadVgpr(d0), LoadVgpr(d0 + 1u));
                    if (e->has_rtn) {
                        StoreVgpr(vdst, prev64.first);
                        StoreVgpr(vdst + 1u, prev64.second);
                    }
                } else {
                    const uint32_t p = lds_ptr(base_a);
                    const uint16_t base_op = (op == 0x55u) ? 0x15u
                                           : ((op >= 0x20u && op <= 0x33u) ? static_cast<uint16_t>(op - 0x20u)
                                           : ((op >= 0x60u && op <= 0x73u) ? static_cast<uint16_t>(op - 0x60u)
                                           : ((op >= 0x40u && op <= 0x53u) ? static_cast<uint16_t>(op - 0x40u) : op)));
                    uint32_t prev = c_u0_;
                    if (base_op == 0x10u || base_op == 0x11u) {
                        prev = AllocId();
                        EmitInst(sec_fn_, OpAtomicCompareExchange, {t_u32_, prev, p, c_u2_, c_u0_, c_u0_, LoadVgpr(d1), LoadVgpr(d0)});
                    } else {
                        uint16_t mapped = 0x32u;
                        switch (base_op) {
                            case 0x00: mapped = 0x32u; break;
                            case 0x01: mapped = 0x33u; break;
                            case 0x02: mapped = 0x4Eu; break;
                            case 0x03: mapped = 0x3Cu; break;
                            case 0x04: mapped = 0x3Du; break;
                            case 0x05: mapped = 0x35u; break;
                            case 0x06: mapped = 0x37u; break;
                            case 0x07: mapped = 0x36u; break;
                            case 0x08: mapped = 0x38u; break;
                            case 0x09: mapped = 0x39u; break;
                            case 0x0A: mapped = 0x3Au; break;
                            case 0x0B: mapped = 0x3Bu; break;
                            case 0x0D: mapped = 0x30u; break;
                            case 0x12: mapped = 0x3Fu; break;
                            case 0x13: mapped = 0x40u; break;
                            default:   mapped = 0x32u; break;
                        }
                        prev = EmitSpvAtomic(p, c_u2_, mapped, LoadVgpr(d0), LoadVgpr(d1));
                    }
                    if (e->has_rtn) StoreVgpr(vdst, prev);
                }
            }
        } else if (inst.encoding == Rdna2Encoding::FLAT) {
            const uint8_t vdst = static_cast<uint8_t>(inst.dst.reg_index & 63u);
            const uint8_t va = static_cast<uint8_t>(inst.src[0].reg_index & 63u);
            const uint8_t vd = static_cast<uint8_t>(inst.src[1].reg_index & 63u);
            const uint32_t baddr = EmitBinary(OpIAdd, t_u32_, LoadVgpr(va), ConstU32(static_cast<uint32_t>(inst.mem_offset)));
            const uint32_t dw0 = EmitBinary(OpShiftRightLogical, t_u32_, baddr, c_u2_);
            if (inst.flat_seg == 1u) {
                auto scratch_ptr = [&](uint32_t dw_i) -> uint32_t {
                    const uint32_t idx = EmitBinary(OpBitwiseAnd, t_u32_, dw_i, ConstU32(63u));
                    const uint32_t sp = AllocId();
                    EmitInst(sec_fn_, OpAccessChain, {t_ptr_fn_u32_, sp, var_scratch_, idx});
                    return sp;
                };
                if (e->is_load) {
                    if (op == 0x08u || op == 0x09u || op == 0x20u || op == 0x21u || op == 0x22u) {
                        const uint32_t w = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, w, scratch_ptr(dw0)});
                        const uint32_t sh = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, baddr, c_u3_), c_u3_);
                        uint32_t b = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, w, sh), ConstU32(0xFFu));
                        if (op == 0x09u || op == 0x22u) {
                            b = BitcastU32(EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, b, ConstU32(24u))), ConstI32(24)));
                        }
                        if (op == 0x20u || op == 0x22u) {
                            const uint32_t lo = EmitBinary(OpBitwiseAnd, t_u32_, b, ConstU32(0xFFFFu));
                            StoreVgpr(vdst, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vdst), ConstU32(0xFFFF0000u)), lo));
                        } else if (op == 0x21u) {
                            StoreVgpr(vdst, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vdst), ConstU32(0xFFFFu)), EmitBinary(OpShiftLeftLogical, t_u32_, b, ConstU32(16u))));
                        } else {
                            StoreVgpr(vdst, b);
                        }
                    } else if (op == 0x0Au || op == 0x0Bu || op == 0x24u || op == 0x25u) {
                        const uint32_t w = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, w, scratch_ptr(dw0)});
                        const uint32_t sh = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, baddr, c_u2_), c_u3_);
                        uint32_t h = EmitBinary(OpBitwiseAnd, t_u32_, EmitBinary(OpShiftRightLogical, t_u32_, w, sh), ConstU32(0xFFFFu));
                        if (op == 0x0Bu) {
                            h = BitcastU32(EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, h, ConstU32(16u))), ConstI32(16)));
                        }
                        if (op == 0x24u) {
                            StoreVgpr(vdst, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vdst), ConstU32(0xFFFF0000u)), h));
                        } else if (op == 0x25u) {
                            StoreVgpr(vdst, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vdst), ConstU32(0xFFFFu)), EmitBinary(OpShiftLeftLogical, t_u32_, h, ConstU32(16u))));
                        } else {
                            StoreVgpr(vdst, h);
                        }
                    } else {
                        for (uint32_t k = 0; k < e->data_dwords; ++k) {
                            const uint32_t r = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, r, scratch_ptr(EmitBinary(OpIAdd, t_u32_, dw0, ConstU32(k)))});
                            StoreVgpr(vdst + k, r);
                        }
                    }
                } else {
                    if (op == 0x18u || op == 0x1Au) {
                        const bool is16 = (op == 0x1Au);
                        const uint32_t sp = scratch_ptr(dw0);
                        const uint32_t cur = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, cur, sp});
                        const uint32_t sh = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, baddr, is16 ? c_u2_ : c_u3_), c_u3_);
                        const uint32_t fm = ConstU32(is16 ? 0xFFFFu : 0xFFu);
                        const uint32_t clrm = AllocId(); EmitInst(sec_fn_, OpNot, {t_u32_, clrm, EmitBinary(OpShiftLeftLogical, t_u32_, fm, sh)});
                        const uint32_t val = EmitBinary(OpShiftLeftLogical, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, LoadVgpr(vd), fm), sh);
                        EmitInst(sec_fn_, OpStore, {sp, EmitBinary(OpBitwiseOr, t_u32_, EmitBinary(OpBitwiseAnd, t_u32_, cur, clrm), val)});
                    } else {
                        for (uint32_t k = 0; k < e->data_dwords; ++k) {
                            EmitInst(sec_fn_, OpStore, {scratch_ptr(EmitBinary(OpIAdd, t_u32_, dw0, ConstU32(k))), LoadVgpr(vd + k)});
                        }
                    }
                }
            } else {
                const uint32_t ssbo = (inst.saddr != 0x7Fu && inst.saddr != 0u) ? var_uav1_ : var_uav0_;
                if (e->is_atomic) {
                    if (op == 0x52u) {
                        const uint32_t ptr_lo = SsboPtrAtDword(ssbo, dw0);
                        const uint32_t ptr_hi = SsboPtrAtDword(ssbo, EmitBinary(OpIAdd, t_u32_, dw0, c_u1_));
                        const auto prev64 = EmitSpvAtomicAdd64(ptr_lo, ptr_hi, c_u1_, LoadVgpr(vd), LoadVgpr(vd + 1u));
                        if (inst.glc) {
                            StoreVgpr(vdst, prev64.first);
                            StoreVgpr(vdst + 1u, prev64.second);
                        }
                    } else {
                        const uint32_t prev = EmitSpvAtomic(SsboPtrAtDword(ssbo, dw0), c_u1_, op, LoadVgpr(vd), LoadVgpr(vd + 1u));
                        if (inst.glc) StoreVgpr(vdst, prev);
                    }
                } else if (e->is_load) {
                    if (op == 0x08u) {
                        StoreVgpr(vdst, LoadSsboByte(ssbo, baddr));
                    } else if (op == 0x09u) {
                        const uint32_t b = LoadSsboByte(ssbo, baddr);
                        StoreVgpr(vdst, BitcastU32(EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, b, ConstU32(24u))), ConstI32(24))));
                    } else if (op == 0x0Au) {
                        StoreVgpr(vdst, LoadSsboU16(ssbo, baddr));
                    } else if (op == 0x0Bu) {
                        const uint32_t h = LoadSsboU16(ssbo, baddr);
                        StoreVgpr(vdst, BitcastU32(EmitBinary(OpShiftRightArithmetic, t_i32_, BitcastI32(EmitBinary(OpShiftLeftLogical, t_u32_, h, ConstU32(16u))), ConstI32(16))));
                    } else {
                        for (uint32_t k = 0; k < e->data_dwords; ++k) {
                            const uint32_t w = AllocId(); EmitInst(sec_fn_, OpLoad, {t_u32_, w, SsboPtrAtDword(ssbo, EmitBinary(OpIAdd, t_u32_, dw0, ConstU32(k)))});
                            StoreVgpr(vdst + k, w);
                        }
                    }
                } else {
                    if (op == 0x18u) StoreSsboByte(ssbo, baddr, LoadVgpr(vd));
                    else if (op == 0x1Au) StoreSsboU16(ssbo, baddr, LoadVgpr(vd));
                    else {
                        for (uint32_t k = 0; k < e->data_dwords; ++k) {
                            EmitInst(sec_fn_, OpStore, {SsboPtrAtDword(ssbo, EmitBinary(OpIAdd, t_u32_, dw0, ConstU32(k))), LoadVgpr(vd + k)});
                        }
                    }
                }
            }
        } else if (inst.encoding == Rdna2Encoding::EXP) {
            const uint8_t tgt = inst.exp_target;
            uint32_t vec4_val = 0;
            if (inst.exp_compr) {
                const uint32_t u01 = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_vec2_, u01, glsl_ext_, 62u, LoadVgpr(inst.src[0].reg_index)});
                const uint32_t u23 = AllocId(); EmitInst(sec_fn_, OpExtInst, {t_vec2_, u23, glsl_ext_, 62u, LoadVgpr(inst.src[1].reg_index)});
                const uint32_t f0 = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, f0, u01, 0u});
                const uint32_t f1 = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, f1, u01, 1u});
                const uint32_t f2 = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, f2, u23, 0u});
                const uint32_t f3 = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, f3, u23, 1u});
                vec4_val = AllocId(); EmitInst(sec_fn_, OpCompositeConstruct, {t_vec4_, vec4_val, f0, f1, f2, f3});
            } else {
                vec4_val = AllocId();
                EmitInst(sec_fn_, OpCompositeConstruct, {t_vec4_, vec4_val,
                         BitcastF32(LoadVgpr(inst.src[0].reg_index)), BitcastF32(LoadVgpr(inst.src[1].reg_index)),
                         BitcastF32(LoadVgpr(inst.src[2].reg_index)), BitcastF32(LoadVgpr(inst.src[3].reg_index))});
            }
            if (is_vs) {
                if (tgt >= 12u && tgt <= 15u && out_pos_) EmitInst(sec_fn_, OpStore, {out_pos_, vec4_val});
                else if (tgt >= 32u && tgt < 40u && out_param_[tgt - 32u]) EmitInst(sec_fn_, OpStore, {out_param_[tgt - 32u], vec4_val});
            } else if (is_ps) {
                if (tgt <= 7u && out_mrt_[tgt]) EmitInst(sec_fn_, OpStore, {out_mrt_[tgt], vec4_val});
                else if (tgt == 8u && out_depth_) {
                    const uint32_t d = AllocId(); EmitInst(sec_fn_, OpCompositeExtract, {t_f32_, d, vec4_val, 0u});
                    EmitInst(sec_fn_, OpStore, {out_depth_, d});
                }
            }
        }
    }

    const Xs2ShaderContainer& container_;
    const std::vector<MemImgDecodedInst>& insts_;
    const MemImgResourceUsage& usage_;

    uint32_t next_id_ = 1u;
    uint32_t cur_label_ = 0u;
    uint32_t glsl_ext_ = 0u;
    uint32_t t_void_ = 0, t_bool_ = 0, t_u32_ = 0, t_i32_ = 0, t_f32_ = 0;
    uint32_t t_uvec2_ = 0, t_uvec3_ = 0, t_uvec4_ = 0, t_ivec2_ = 0, t_ivec3_ = 0;
    uint32_t t_vec2_ = 0, t_vec3_ = 0, t_vec4_ = 0, t_fn_ = 0;
    uint32_t t_arr64_u32_ = 0, t_arr4096_u32_ = 0, t_rtarr_u32_ = 0, t_ssbo_ = 0;
    uint32_t t_ptr_fn_u32_ = 0, t_ptr_fn_arr64_ = 0, t_ptr_ssbo_ = 0, t_ptr_ssbo_u32_ = 0;
    uint32_t t_ptr_wg_u32_ = 0, t_ptr_img_u32_ = 0;
    uint32_t t_tex_[8] = {}, t_sampled_tex_[8] = {}, var_tex_[8] = {};
    uint32_t t_samp_ = 0, var_samp_ = 0;
    uint32_t t_rwtex_f4_ = 0, var_rwtex_f4_ = 0;
    uint32_t t_rwtex_u32_ = 0, var_rwtex_u32_ = 0;
    uint32_t var_uav0_ = 0, var_uav1_ = 0, var_lds_ = 0, var_wave_tmp_ = 0, var_ds_counter_ = 0;
    uint32_t var_vgpr_ = 0, var_scratch_ = 0;
    uint32_t in_gid_ = 0, in_gidx_ = 0, in_vid_ = 0, gidx_id_ = 0;
    uint32_t out_pos_ = 0, out_depth_ = 0, out_mrt_[8] = {}, out_param_[8] = {};
    uint32_t c_u0_ = 0, c_u1_ = 0, c_u2_ = 0, c_u3_ = 0, c_u4_ = 0, c_u64_ = 0, c_u4096_ = 0;
    uint32_t c_f0_ = 0, c_f1_ = 0;

    std::unordered_map<uint32_t, uint32_t> const_u32_;
    std::unordered_map<uint32_t, uint32_t> const_i32_;
    std::unordered_map<uint32_t, uint32_t> const_f32_;

    std::vector<uint32_t> sec_caps_, sec_ext_, sec_mem_, sec_entry_, sec_exec_, sec_decor_, sec_types_, sec_globals_, sec_fn_;
};

}  // namespace

Status LiftMemImgShader(
    const Xs2ShaderContainer& container,
    const std::vector<MemImgDecodedInst>& insts,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options) {
    out_result = Rdna2LiftResult{};
    if (auto st = ValidateMimgUnsupportedModifiers(insts); !st.ok()) {
        out_result.dxc_diagnostics = st.message;
        return st;
    }
    out_result.container = container;
    out_result.instructions.reserve(insts.size());
    for (const auto& di : insts) {
        out_result.instructions.push_back(di.base);
    }

    const MemImgResourceUsage usage = AnalyzeResourceUsage(container, insts);

    if (options.emit_spirv) {
        SpirvMemImgEmitter spv_emitter(container, insts, usage);
        if (auto st = spv_emitter.Emit(out_result.spirv_words); !st.ok()) {
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
        if (auto st = EmitMemImgHlsl(container, insts, usage, out_result.hlsl_source, out_result.hlsl_target,
                                     options.use_sm65);
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
            if (options.resign_dxil) {
                if (auto rs = ResignDxilContainer(out_result.pc_dxil); !rs.ok()) {
                    return rs;
                }
            }
            out_result.pc_dxil_hash = HashBytesFnv1a(out_result.pc_dxil.data(), out_result.pc_dxil.size());
        }
    }

    return Status::Ok();
}

}  // namespace openscarlett::rdna2_mem
