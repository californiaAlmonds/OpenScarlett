// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenScarlett Contributors
//
// Clean-room Xbox Series X|S DirectStorage Hardware MDU BCPack Bitstream Decoder
// (DSTORAGE_BCPACK_MODE 1..7: BC1, BC3, BC4, BC5, BC6H, BC7).
//
// Provenance & Permissive MIT Attribution:
//   - Entropy coder & 16-bit banded rANS probability law:
//       * Microsoft `silicon-rans` (`mdu_minirans.v`), MIT License
//       * Tim0219800 `bcpack-decoder` (`docs/SPEC.md`, `src/model.rs`, `src/lanes.rs`),
//         Copyright (c) 2026 Tim0219800, MIT License
//   - Interleaved 12/16-lane 8-byte/cycle scheduler, static codec tables, and
//     BC1/BC3/BC4/BC5/BC6H/BC7 block reconstruction:
//       * wxrrry `leonida-asset-pipeline` (`tools/bcpack.py`, `tools/bcpack_tables.py`,
//         `tools/bcpack_tables_bptc.py`), Copyright (c) 2026 wxrrry, MIT License
//   - Public Khronos / Microsoft D3D11 BPTC (BC6H/BC7) & S3TC/RGTC (BC1..BC5) specs,
//     and US Patent 11,234,023 B2.

#include "bcpack_xbox_mdu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace openscarlett {
namespace {

constexpr uint64_t kRansL = 1ull << 24;          // 0x01000000
constexpr uint64_t kRansMaxState = 1ull << 32;   // 0x100000000
constexpr uint32_t kMaxSymbolWidth = 8u;
constexpr uint32_t kMaxModelQ = 15u;
constexpr uint32_t kMaxLanes = 16u;
constexpr uint32_t kPrefetchBudget = 8u;
constexpr uint32_t kMaxVirtualZeros = 8u;
constexpr size_t kCodingUnitBytes = 8u;

struct CodecLaneSpec {
    uint32_t block_bytes;
    uint32_t num_lanes;
    uint8_t widths[kMaxLanes];
};

bool GetCodecLaneSpec(BcFormat fmt, CodecLaneSpec& out) noexcept {
    switch (fmt) {
        case BcFormat::BC1:
            out = {8u, 12u, {8, 8, 5, 6, 5, 5, 6, 5, 8, 8, 8, 8, 0, 0, 0, 0}};
            return true;
        case BcFormat::BC3:
            out = {16u, 12u, {4, 4, 8, 6, 6, 6, 6, 5, 6, 5, 8, 8, 0, 0, 0, 0}};
            return true;
        case BcFormat::BC4:
            out = {8u, 12u, {8, 8, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 0}};
            return true;
        case BcFormat::BC5:
            out = {16u, 12u, {4, 4, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 0}};
            return true;
        case BcFormat::BC6H:
            out = {16u, 16u, {4, 4, 8, 8, 8, 5, 5, 5, 6, 6, 6, 8, 8, 8, 8, 5}};
            return true;
        case BcFormat::BC7:
            out = {16u, 16u, {4, 4, 8, 8, 8, 8, 8, 7, 7, 5, 6, 8, 8, 8, 8, 5}};
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// 16-Bit Banded rANS Static Probability Models (w = 1..8, q = 0..15)
// Exact integer band frequencies matching Microsoft MIT `silicon-rans`
// (`mdu_minirans.v`), `Tim0219800/bcpack-decoder`, and `wxrrry/leonida-asset-pipeline`.
// Band 0 holds (FIN=1 + symbol 0); band p >= 1 holds per-symbol frequency for [2^(p-1), 2^p).
// ---------------------------------------------------------------------------
constexpr uint32_t kProbBands[kMaxSymbolWidth][kMaxModelQ + 1u][kMaxSymbolWidth + 1u] = {
    // w = 1
    {
        {32768, 32768}, {38761, 26775}, {44484, 21052}, {49818, 15718},
        {54623, 10913}, {58721, 6815},  {61911, 3625},  {64036, 1500},
        {65121, 415},   {65477, 59},    {65534, 2},     {65535, 1},
        {65535, 1},     {65535, 1},     {65535, 1},     {65535, 1},
    },
    // w = 2
    {
        {16384, 16384, 16384}, {21168, 17592, 13388}, {26354, 18130, 10526}, {31900, 17918, 7859},
        {37750, 16874, 5456},  {43800, 14920, 3408},  {49850, 12062, 1812},  {55537, 8499, 750},
        {60308, 4812, 208},    {63575, 1901, 30},     {65132, 402, 1},       {65511, 23, 1},
        {65533, 1, 1},         {65533, 1, 1},         {65533, 1, 1},         {65533, 1, 1},
    },
    // w = 3
    {
        {8192, 8192, 8192, 8192},    {11073, 10095, 8796, 6694},  {14406, 11948, 9065, 5263},  {18234, 13666, 8958, 3930},
        {22624, 15126, 8437, 2728},  {27657, 16143, 7460, 1704},  {33414, 16436, 6031, 906},   {39920, 15616, 4250, 375},
        {47024, 13284, 2406, 104},   {54201, 9375, 950, 15},      {60388, 4744, 200, 1},       {64250, 1262, 10, 1},
        {65456, 74, 1, 1},           {65529, 1, 1, 1},            {65529, 1, 1, 1},            {65529, 1, 1, 1},
    },
    // w = 4
    {
        {4096, 4096, 4096, 4096, 4096},     {5665, 5407, 5048, 4398, 3347},     {7539, 6867, 5973, 4532, 2632},     {9773, 8461, 6833, 4479, 1965},
        {12447, 10177, 7564, 4218, 1364},   {15679, 11977, 8072, 3730, 852},    {19640, 13774, 8217, 3016, 453},    {24559, 15361, 7808, 2124, 188},
        {30704, 16320, 6642, 1203, 52},     {38280, 15920, 4688, 476, 7},       {47169, 13219, 2372, 99, 1},        {56354, 7896, 631, 4, 1},
        {63248, 2208, 34, 1, 1},            {65465, 57, 1, 1, 1},               {65521, 1, 1, 1, 1},                {65521, 1, 1, 1, 1},
    },
    // w = 5
    {
        {2048, 2048, 2048, 2048, 2048, 2048},       {2865, 2799, 2704, 2524, 2200, 1673},       {3858, 3682, 3434, 2986, 2266, 1316},       {5063, 4709, 4230, 3418, 2240, 982},
        {6536, 5912, 5088, 3782, 2109, 682},        {8366, 7314, 5988, 4036, 1865, 426},        {10688, 8952, 6888, 4108, 1507, 227},       {13713, 10847, 7680, 3904, 1062, 94},
        {17758, 12946, 8160, 3320, 602, 26},        {23272, 15008, 7960, 2344, 237, 4},         {30841, 16327, 6610, 1187, 48, 1},          {41006, 15348, 3947, 316, 1, 1},
        {53291, 9957, 1104, 14, 1, 1},              {63380, 2084, 22, 1, 1, 1},                 {65505, 1, 1, 1, 1, 1},                     {65505, 1, 1, 1, 1, 1},
    },
    // w = 6
    {
        {1024, 1024, 1024, 1024, 1024, 1024, 1024},         {1441, 1423, 1400, 1352, 1262, 1099, 837},          {1951, 1907, 1841, 1717, 1493, 1133, 658},          {2577, 2487, 2354, 2115, 1709, 1120, 491},
        {3350, 3186, 2956, 2544, 1890, 1055, 341},          {4324, 4042, 3657, 2994, 2019, 932, 213},           {5581, 5107, 4476, 3444, 2055, 754, 113},           {7258, 6454, 5424, 3840, 1952, 531, 47},
        {9579, 8179, 6473, 4080, 1660, 301, 13},            {12907, 10365, 7504, 3980, 1171, 119, 2},           {17852, 12988, 8164, 3304, 594, 23, 1},             {25441, 15565, 7673, 1974, 155, 1, 1},
        {37208, 16084, 4978, 552, 4, 1, 1},                 {53649, 9731, 1042, 4, 1, 1, 1},                    {65147, 327, 1, 1, 1, 1, 1},                        {65473, 1, 1, 1, 1, 1, 1},
    },
    // w = 7
    {
        {512, 512, 512, 512, 512, 512, 512, 512},           {722, 718, 712, 700, 676, 632, 550, 418},           {981, 971, 952, 920, 858, 748, 566, 329},           {1300, 1276, 1244, 1178, 1058, 854, 559, 246},
        {1696, 1654, 1593, 1478, 1272, 946, 526, 171},      {2198, 2126, 2022, 1828, 1498, 1009, 467, 106},     {2852, 2728, 2554, 2238, 1721, 1028, 376, 57},      {3736, 3522, 3227, 2712, 1920, 977, 266, 23},
        {4979, 4601, 4090, 3236, 2040, 831, 151, 6},        {6807, 6101, 5182, 3752, 1991, 586, 59, 1},         {9634, 8218, 6494, 4082, 1652, 296, 11, 1},         {14275, 11165, 7784, 3836, 988, 74, 1, 1},
        {22449, 14759, 8042, 2489, 272, 1, 1, 1},           {37625, 16023, 4866, 509, 1, 1, 1, 1},              {60485, 4663, 132, 1, 1, 1, 1, 1},                  {65409, 1, 1, 1, 1, 1, 1, 1},
    },
    // w = 8
    {
        {256, 256, 256, 256, 256, 256, 256, 256, 256},      {362, 360, 359, 356, 350, 338, 316, 275, 209},      {492, 488, 486, 476, 460, 429, 374, 284, 164},      {653, 647, 638, 622, 589, 529, 426, 280, 123},
        {853, 843, 828, 796, 739, 636, 473, 264, 85},       {1108, 1090, 1063, 1011, 914, 748, 504, 234, 53},   {1442, 1410, 1364, 1277, 1120, 860, 514, 189, 28},  {1895, 1841, 1760, 1614, 1356, 961, 488, 132, 12},
        {2538, 2442, 2300, 2045, 1618, 1021, 414, 76, 3},   {3497, 3311, 3050, 2591, 1875, 996, 292, 29, 1},    {5008, 4626, 4109, 3247, 2041, 826, 149, 4, 1},     {7575, 6701, 5582, 3892, 1918, 494, 34, 1, 1},
        {12397, 10051, 7380, 4020, 1245, 129, 1, 1, 1},     {22767, 14857, 8012, 2432, 240, 1, 1, 1, 1},        {47343, 13141, 2332, 35, 1, 1, 1, 1, 1},            {65281, 1, 1, 1, 1, 1, 1, 1, 1},
    },
};

struct RansBandModel {
    uint32_t w = 0u;
    uint32_t freq[kMaxSymbolWidth + 1u]{};
    uint32_t base[kMaxSymbolWidth + 1u]{};

    // Looks up symbol for `cf` in 1..65535 (caller checks `cf != 0` FIN slot).
    uint32_t Lookup(uint32_t cf) const noexcept {
        uint32_t p = w;
        while (p > 0u && cf < base[p]) {
            --p;
        }
        return (p == 0u) ? 0u : ((1u << (p - 1u)) + (cf - base[p]) / freq[p]);
    }

    uint64_t Advance(uint32_t s, uint64_t x) const noexcept {
        const uint32_t p = static_cast<uint32_t>(std::bit_width(s));
        uint32_t f = freq[p];
        uint32_t c = base[p] + f * (s - ((p == 0u) ? 0u : (1u << (p - 1u))));
        if (c == 0u) {
            f -= 1u;
            c = 1u;
        }
        return static_cast<uint64_t>(f) * (x >> 16u) + (x & 0xFFFFull) - static_cast<uint64_t>(c);
    }
};

struct AllRansModels {
    RansBandModel zero_width_q0{};
    RansBandModel table[kMaxSymbolWidth][kMaxModelQ + 1u]{};
};

const AllRansModels& GetAllRansModels() noexcept {
    static const AllRansModels kModels = []() {
        AllRansModels m{};
        m.zero_width_q0.w = 0u;
        m.zero_width_q0.freq[0] = 65536u;
        m.zero_width_q0.base[0] = 0u;

        for (uint32_t w = 1u; w <= kMaxSymbolWidth; ++w) {
            for (uint32_t q = 0u; q <= kMaxModelQ; ++q) {
                RansBandModel& dst = m.table[w - 1u][q];
                dst.w = w;
                uint32_t acc = 0u;
                for (uint32_t p = 0u; p <= w; ++p) {
                    const uint32_t f = kProbBands[w - 1u][q][p];
                    dst.freq[p] = f;
                    dst.base[p] = acc;
                    acc += f * ((p == 0u) ? 1u : (1u << (p - 1u)));
                }
            }
        }
        return m;
    }();
    return kModels;
}

const RansBandModel* ResolveRansModel(int32_t width, uint32_t q) noexcept {
    if (q > kMaxModelQ) {
        return nullptr;
    }
    const AllRansModels& all = GetAllRansModels();
    if (width == 0 && q == 0u) {
        return &all.zero_width_q0;
    }
    if (width < 1 || width > static_cast<int32_t>(kMaxSymbolWidth)) {
        return nullptr;
    }
    return &all.table[static_cast<size_t>(width - 1)][q];
}

// ---------------------------------------------------------------------------
// 8-Byte/Cycle Interleaved K-Lane Hardware Scheduler (`mdu_minirans` pipeline)
// ---------------------------------------------------------------------------
Status DecodeMduLanes(
    const uint8_t* raw,
    size_t raw_size,
    const CodecLaneSpec& spec,
    size_t nsym,
    std::array<std::vector<uint8_t>, kMaxLanes>& out_lanes) {
    const uint32_t nl = spec.num_lanes;
    for (uint32_t i = 0u; i < nl; ++i) {
        out_lanes[i].clear();
        out_lanes[i].reserve(nsym);
    }
    if (nsym == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Zero coding units in MDU BCPack decode");
    }

    enum class LaneCtrl : uint8_t { Hdr0 = 0, Hdr1 = 1, Hdr2 = 2, Proc = 3 };

    std::array<LaneCtrl, kMaxLanes> ctrl{};
    ctrl.fill(LaneCtrl::Hdr0);
    std::array<size_t, kMaxLanes> out_rem{};
    out_rem.fill(nsym);
    std::array<int32_t, kMaxLanes> w{};
    std::array<uint32_t, kMaxLanes> q{};
    std::array<uint8_t, kMaxLanes> h3{};
    std::array<uint8_t, kMaxLanes> flush{};
    std::array<bool, kMaxLanes> done{};
    done.fill(false);
    std::array<bool, kMaxLanes> full{};
    full.fill(false);
    std::array<bool, kMaxLanes> held{};
    held.fill(false);
    std::array<uint32_t, kMaxLanes> vz{};
    std::array<uint8_t, kMaxLanes> inbuf{};
    std::array<uint32_t, kMaxLanes> sym{};
    std::array<uint32_t, kMaxLanes> in_rem{};
    std::array<uint64_t, kMaxLanes> p0{};
    std::array<uint64_t, kMaxLanes> p1{};
    std::array<const RansBandModel*, kMaxLanes> model{};
    model.fill(nullptr);
    std::array<bool, kMaxLanes> slotfull{};
    slotfull.fill(false);
    std::array<uint8_t, kMaxLanes> slotval{};
    std::array<bool, kMaxLanes> stall{};
    stall.fill(false);

    size_t pos = 0u;
    const size_t max_cycles = std::max<size_t>(1024u, nsym * 64u + raw_size * 8u + 256u);

    for (size_t cycle = 0u; cycle < max_cycles; ++cycle) {
        bool all_finished = true;
        for (uint32_t i = 0u; i < nl; ++i) {
            if (!done[i] || slotfull[i]) {
                all_finished = false;
                break;
            }
        }
        if (all_finished) {
            for (uint32_t i = 0u; i < nl; ++i) {
                if (out_lanes[i].size() != nsym) {
                    return Status::Error(StatusCode::ParseError, "BCPack lane symbol count mismatch");
                }
            }
            for (size_t rem = pos; rem < raw_size; ++rem) {
                if (raw[rem] != 0u) {
                    return Status::Error(
                        StatusCode::ParseError,
                        "Non-zero trailing bytes after final BCPack lane completed");
                }
            }
            return Status::Ok();
        }

        // Phase 1: rANS state advance, 8-byte/cycle prefetch arbitration, and held-symbol release.
        uint32_t left = kPrefetchBudget;
        for (uint32_t i = 0u; i < nl; ++i) {
            if (done[i] || (held[i] && slotfull[i])) {
                stall[i] = true;
                continue;
            }
            stall[i] = false;
            if (ctrl[i] == LaneCtrl::Proc) {
                p0[i] = held[i] ? model[i]->Advance(sym[i], p1[i]) : p1[i];
                if (p0[i] >= kRansMaxState) {
                    return Status::Error(StatusCode::ParseError, "BCPack rANS state overflow");
                }
            }
            if (!full[i] && (ctrl[i] != LaneCtrl::Proc || in_rem[i] > 0u)) {
                if (left == 0u) {
                    stall[i] = true;
                    continue;
                }
                if (pos >= raw_size) {
                    return Status::Error(StatusCode::ParseError, "BCPack bitstream truncated");
                }
                inbuf[i] = raw[pos++];
                full[i] = true;
                --left;
            }
            if (held[i]) {
                slotfull[i] = true;
                slotval[i] = static_cast<uint8_t>(sym[i]);
                held[i] = false;
            }
        }

        // Phase 2: Header parsing, byte merge, virtual zero restoration on flush, and symbol lookup.
        for (uint32_t i = 0u; i < nl; ++i) {
            if (done[i] || stall[i]) {
                continue;
            }
            switch (ctrl[i]) {
                case LaneCtrl::Hdr0: {
                    if (!full[i]) {
                        continue;
                    }
                    const uint8_t h = inbuf[i];
                    q[i] = static_cast<uint32_t>(h & 0x0Fu);
                    flush[i] = static_cast<uint8_t>((h >> 4u) & 1u);
                    h3[i] = static_cast<uint8_t>((h >> 5u) & 1u);
                    w[i] = static_cast<int32_t>(spec.widths[i]) - static_cast<int32_t>(h >> 6u);
                    full[i] = false;
                    ctrl[i] = LaneCtrl::Hdr1;
                    vz[i] = 0u;
                    break;
                }
                case LaneCtrl::Hdr1: {
                    if (!full[i]) {
                        continue;
                    }
                    full[i] = false;
                    const RansBandModel* m = ResolveRansModel(w[i], q[i]);
                    if (!m) {
                        return Status::Error(
                            StatusCode::ParseError, "BCPack lane symbol width or q model invalid");
                    }
                    model[i] = m;
                    if (h3[i] != 0u) {
                        in_rem[i] = static_cast<uint32_t>(inbuf[i]);
                        ctrl[i] = LaneCtrl::Hdr2;
                    } else {
                        in_rem[i] = static_cast<uint32_t>(inbuf[i]) + 1u;
                        ctrl[i] = LaneCtrl::Proc;
                    }
                    break;
                }
                case LaneCtrl::Hdr2: {
                    if (!full[i]) {
                        continue;
                    }
                    in_rem[i] = ((static_cast<uint32_t>(inbuf[i]) << 8u) | (in_rem[i] & 0xFFu)) + 257u;
                    ctrl[i] = LaneCtrl::Proc;
                    full[i] = false;
                    break;
                }
                case LaneCtrl::Proc: {
                    uint64_t st = p0[i];
                    if (st < kRansL) {
                        if (in_rem[i] > 0u) {
                            if (full[i]) {
                                st = (st << 8u) | static_cast<uint64_t>(inbuf[i]);
                                --in_rem[i];
                                full[i] = false;
                                if (in_rem[i] == 0u && flush[i] != 0u) {
                                    if (st == 0ull) {
                                        return Status::Error(
                                            StatusCode::ParseError,
                                            "BCPack rANS zero state on flushed fragment");
                                    }
                                    while (st < kRansL && vz[i] < kMaxVirtualZeros) {
                                        st <<= 8u;
                                        ++vz[i];
                                    }
                                }
                            }
                        } else if (flush[i] != 0u) {
                            if (st == 0ull || vz[i] >= kMaxVirtualZeros) {
                                return Status::Error(
                                    StatusCode::ParseError,
                                    "BCPack fragment failed to reach rANS home state");
                            }
                            while (st < kRansL && vz[i] < kMaxVirtualZeros) {
                                st <<= 8u;
                                ++vz[i];
                            }
                        } else {
                            p1[i] = st;
                            if (out_rem[i] > 0u) {
                                ctrl[i] = LaneCtrl::Hdr0;
                                continue;
                            }
                            return Status::Error(
                                StatusCode::ParseError,
                                "BCPack final lane fragment must have flush bit set");
                        }
                    }

                    const bool nxt = (st > kRansL) && (out_rem[i] > 0u);
                    if (nxt) {
                        const uint32_t cf = static_cast<uint32_t>(st & 0xFFFFull);
                        if (cf == 0u) {
                            return Status::Error(
                                StatusCode::ParseError,
                                "BCPack rANS hit FIN slot 0 before fragment end");
                        }
                        const uint32_t s = model[i]->Lookup(cf);
                        if ((s >> static_cast<uint32_t>(w[i])) != 0u) {
                            return Status::Error(
                                StatusCode::ParseError, "BCPack decoded symbol exceeds lane width");
                        }
                        sym[i] = s;
                        --out_rem[i];
                    }
                    held[i] = nxt;
                    p1[i] = st;

                    if (in_rem[i] == 0u) {
                        if (flush[i] != 0u) {
                            if (st == kRansL) {
                                if (out_rem[i] > 0u) {
                                    ctrl[i] = LaneCtrl::Hdr0;
                                    p0[i] = 0ull;
                                    p1[i] = 0ull;
                                } else {
                                    done[i] = true;
                                }
                            } else if (st > kRansL && !nxt) {
                                return Status::Error(
                                    StatusCode::ParseError,
                                    "BCPack flushed fragment did not end at home state L");
                            }
                        } else if (out_rem[i] == 0u && !nxt) {
                            return Status::Error(
                                StatusCode::ParseError,
                                "BCPack non-flushed fragment cannot terminate lane");
                        }
                    } else if (st == kRansL || (out_rem[i] == 0u && !nxt && st > kRansL)) {
                        return Status::Error(
                            StatusCode::ParseError,
                            "BCPack premature home state or trailing bytes in fragment");
                    }
                    break;
                }
            }
        }

        // Phase 3: Synchronized unit barrier across all active lanes.
        bool ready = true;
        bool anyfull = false;
        for (uint32_t i = 0u; i < nl; ++i) {
            if (slotfull[i]) {
                anyfull = true;
            } else if (!(done[i] && !held[i])) {
                ready = false;
                break;
            }
        }
        if (ready && anyfull) {
            for (uint32_t i = 0u; i < nl; ++i) {
                if (slotfull[i]) {
                    out_lanes[i].push_back(slotval[i]);
                    slotfull[i] = false;
                }
            }
        }
        for (uint32_t i = 0u; i < nl; ++i) {
            if (done[i] && held[i] && !slotfull[i]) {
                slotfull[i] = true;
                slotval[i] = static_cast<uint8_t>(sym[i]);
                held[i] = false;
            }
        }
    }

    return Status::Error(StatusCode::ParseError, "BCPack scheduler exceeded maximum cycle bound");
}

// ---------------------------------------------------------------------------
// Bit Manipulation, ZigZag, Interleave, and 128-Bit Block Packing Helpers
// ---------------------------------------------------------------------------
constexpr int32_t Unzig(uint32_t v) noexcept {
    return (v & 1u) != 0u
               ? -static_cast<int32_t>((v + 1u) >> 1u)
               : static_cast<int32_t>(v >> 1u);
}

constexpr uint32_t WrapModPow2(int32_t v, uint32_t mask) noexcept {
    return static_cast<uint32_t>(v) & mask;
}

constexpr uint32_t InterleaveBits(uint32_t a, uint32_t b, uint32_t n) noexcept {
    uint32_t d = 0u;
    for (uint32_t k = 0u; k < n; ++k) {
        d |= (((a >> k) & 1u) << (2u * k)) | (((b >> k) & 1u) << (2u * k + 1u));
    }
    return d;
}

constexpr uint32_t Inter8(uint32_t a, uint32_t b) noexcept {
    return InterleaveBits(a, b, 4u);
}

struct DeintTables {
    std::array<uint8_t, 256> even{};
    std::array<uint8_t, 256> odd{};
};

constexpr DeintTables kDeint = []() {
    DeintTables t{};
    for (uint32_t s = 0u; s < 256u; ++s) {
        t.even[s] = static_cast<uint8_t>(
            ((s >> 0u) & 1u) | (((s >> 2u) & 1u) << 1u) |
            (((s >> 4u) & 1u) << 2u) | (((s >> 6u) & 1u) << 3u));
        t.odd[s] = static_cast<uint8_t>(
            ((s >> 1u) & 1u) | (((s >> 3u) & 1u) << 1u) |
            (((s >> 5u) & 1u) << 2u) | (((s >> 7u) & 1u) << 3u));
    }
    return t;
}();

struct BitAccum128 {
    uint64_t lo = 0ull;
    uint64_t hi = 0ull;

    constexpr void OrBit(uint32_t pos, uint32_t bit) noexcept {
        if ((bit & 1u) == 0u) {
            return;
        }
        if (pos < 64u) {
            lo |= (1ull << pos);
        } else if (pos < 128u) {
            hi |= (1ull << (pos - 64u));
        }
    }

    constexpr void OrBits(uint32_t pos, uint32_t val, uint32_t width) noexcept {
        if (width == 0u) {
            return;
        }
        const uint64_t mask = (width >= 64u) ? ~0ull : ((1ull << width) - 1ull);
        const uint64_t v = static_cast<uint64_t>(val) & mask;
        if (pos < 64u) {
            lo |= (v << pos);
            if (pos > 0u && pos + width > 64u) {
                hi |= (v >> (64u - pos));
            }
        } else if (pos < 128u) {
            hi |= (v << (pos - 64u));
        }
    }

    void StoreLe(uint8_t out[16]) const noexcept {
        for (uint32_t i = 0u; i < 8u; ++i) {
            out[i] = static_cast<uint8_t>((lo >> (i * 8u)) & 0xFFull);
            out[8u + i] = static_cast<uint8_t>((hi >> (i * 8u)) & 0xFFull);
        }
    }
};

// ---------------------------------------------------------------------------
// Static BC1 / BC3 / BC4 / BC5 Codec Rank & Built-In History Tables
// (MIT `wxrrry/leonida-asset-pipeline/tools/bcpack_tables.py`)
// ---------------------------------------------------------------------------
constexpr uint8_t kBc1Quadrant[256] = {
    0xaa, 0xff, 0x00, 0x55, 0xfa, 0xaf, 0xfb, 0xbf, 0xfe, 0xef, 0xbb, 0xee, 0xdf, 0xba, 0x5a, 0xae,
    0x7f, 0xea, 0xab, 0xf7, 0xfd, 0xa5, 0x5f, 0xf5, 0xdd, 0x77, 0x66, 0x99, 0x2a, 0x8a, 0xa2, 0x50,
    0x05, 0xa8, 0x11, 0x9a, 0x44, 0x3f, 0xa6, 0xa9, 0xcf, 0xbe, 0xfc, 0xf3, 0x6a, 0xeb, 0x7e, 0xe7,
    0xdb, 0xbd, 0x7a, 0xa7, 0xda, 0x0f, 0xad, 0x57, 0xd5, 0x0a, 0x5d, 0x75, 0x2f, 0x6e, 0xf0, 0x8f,
    0xf8, 0xe6, 0x3d, 0xf2, 0xb9, 0x25, 0x7c, 0x9b, 0x3b, 0x85, 0xce, 0xec, 0x58, 0xb3, 0xd3, 0xa0,
    0x52, 0xde, 0x7b, 0x2b, 0xb7, 0x5e, 0xc7, 0xed, 0x5b, 0xb2, 0x8e, 0xcc, 0x6f, 0x19, 0xf6, 0x9f,
    0x64, 0xe8, 0xe5, 0x29, 0xf9, 0x33, 0x46, 0x22, 0xb5, 0xd7, 0x2d, 0x78, 0x56, 0x86, 0x88, 0x68,
    0x91, 0x95, 0x92, 0xd2, 0x87, 0x7d, 0x76, 0x67, 0x9d, 0x59, 0x65, 0xd9, 0x15, 0x37, 0x93, 0x5c,
    0xc6, 0x35, 0x6c, 0xcd, 0x39, 0x45, 0xdc, 0x54, 0x73, 0x51, 0x53, 0xc5, 0x2e, 0x8b, 0x8d, 0x27,
    0xe2, 0x1f, 0xf4, 0x1d, 0xd8, 0xf1, 0x72, 0xa3, 0x4f, 0x74, 0xb8, 0xca, 0x3a, 0xac, 0xd1, 0x89,
    0x47, 0x26, 0x62, 0x98, 0x70, 0x0d, 0xa1, 0x1a, 0x4a, 0x6b, 0x1b, 0xd0, 0xa4, 0xe9, 0x07, 0xb1,
    0x9e, 0xb6, 0xe4, 0x4e, 0xbc, 0xe3, 0x3e, 0xcb, 0x82, 0x31, 0x02, 0x28, 0x4c, 0x13, 0xc4, 0x20,
    0x0e, 0x0b, 0x08, 0x96, 0x80, 0xe0, 0x09, 0xb0, 0x69, 0x60, 0x06, 0x90, 0x23, 0x97, 0x8c, 0xd6,
    0x21, 0x6d, 0x79, 0x32, 0xc8, 0x48, 0x84, 0x17, 0x12, 0xd4, 0x4d, 0x71, 0x01, 0x36, 0x63, 0x9c,
    0xc9, 0x40, 0x04, 0x10, 0xe1, 0x16, 0x94, 0x49, 0x4b, 0x1e, 0xb4, 0x61, 0x03, 0x0c, 0xc0, 0x42,
    0x81, 0x18, 0x30, 0x24, 0xc3, 0x3c, 0xc2, 0x41, 0x14, 0x83, 0x2c, 0x38, 0x43, 0x34, 0x1c, 0xc1,
};

constexpr uint8_t kBc3AlphaPair[64][2] = {
    {0, 0}, {1, 1}, {6, 6}, {4, 4}, {3, 3}, {5, 5}, {7, 7}, {1, 6},
    {6, 1}, {0, 1}, {1, 0}, {7, 1}, {1, 7}, {0, 3}, {3, 0}, {0, 2},
    {2, 0}, {2, 2}, {5, 4}, {3, 4}, {4, 3}, {4, 5}, {1, 5}, {5, 1},
    {0, 4}, {4, 0}, {2, 3}, {3, 2}, {5, 3}, {3, 5}, {6, 5}, {5, 6},
    {6, 7}, {7, 6}, {5, 7}, {4, 6}, {6, 4}, {7, 5}, {6, 0}, {0, 6},
    {4, 2}, {2, 4}, {4, 1}, {1, 4}, {5, 0}, {0, 5}, {6, 3}, {3, 6},
    {3, 1}, {5, 2}, {1, 3}, {2, 5}, {7, 4}, {4, 7}, {7, 3}, {3, 7},
    {2, 6}, {6, 2}, {2, 1}, {1, 2}, {0, 7}, {7, 0}, {2, 7}, {7, 2},
};

constexpr uint8_t kBc3ColourQuadrant[256] = {
    0x55, 0xaa, 0xff, 0x00, 0x50, 0x05, 0xaf, 0xfa, 0xae, 0xea, 0xab, 0xba, 0xee, 0xbb, 0xfe, 0xbf,
    0xef, 0xfb, 0xa5, 0x5a, 0xf0, 0x11, 0x0f, 0x44, 0xf5, 0x5f, 0xa0, 0x0a, 0x8a, 0xa2, 0xa8, 0x2a,
    0x7f, 0xf7, 0x77, 0xdf, 0xfd, 0xdd, 0x66, 0x99, 0x88, 0xbe, 0xeb, 0x22, 0xcc, 0x33, 0x8f, 0xf8,
    0x57, 0x75, 0xd5, 0x5d, 0x2f, 0xf2, 0x7a, 0x85, 0xa7, 0xda, 0x58, 0x25, 0xad, 0x52, 0x3f, 0xf3,
    0xcf, 0xe8, 0xfc, 0x8e, 0x2b, 0x7e, 0x6a, 0xa6, 0x9a, 0x70, 0x07, 0x0d, 0xa9, 0x78, 0x80, 0x08,
    0x02, 0xe7, 0xb2, 0x20, 0xdb, 0xd0, 0x87, 0x5e, 0xbd, 0x7c, 0x2d, 0xe5, 0xc7, 0xec, 0xce, 0x5b,
    0xb3, 0xd2, 0x2e, 0xb5, 0x3b, 0xe0, 0xe6, 0x6e, 0x68, 0x8b, 0x9b, 0xe2, 0xb9, 0x0e, 0xb8, 0x0b,
    0x3d, 0xd3, 0x51, 0x86, 0x27, 0x45, 0x15, 0xde, 0x54, 0x8d, 0xb7, 0xed, 0x7b, 0xb0, 0xd8, 0x29,
    0x72, 0x92, 0x7d, 0xd7, 0x35, 0xa3, 0xc5, 0x82, 0x37, 0x53, 0x28, 0x5c, 0x56, 0x65, 0x76, 0x95,
    0x59, 0xcd, 0x9f, 0x3a, 0x73, 0x89, 0xca, 0xac, 0x67, 0x9d, 0xf6, 0xd9, 0xf9, 0x91, 0x19, 0xdc,
    0x98, 0x6c, 0x46, 0x64, 0x6f, 0x26, 0x39, 0x62, 0xc6, 0x93, 0x09, 0x23, 0x06, 0x60, 0x8c, 0xc8,
    0x90, 0x32, 0x03, 0x3e, 0xc0, 0x13, 0x31, 0xe3, 0x4c, 0x9e, 0x0c, 0xc4, 0xcb, 0xbc, 0xb6, 0xe9,
    0x30, 0x6b, 0x1f, 0xf1, 0x1d, 0xd1, 0x47, 0x74, 0x4f, 0x10, 0x40, 0xf4, 0x01, 0x04, 0xa1, 0x1a,
    0xa4, 0x1b, 0x4a, 0xb1, 0xe4, 0x4e, 0x36, 0xc9, 0x9c, 0x63, 0x97, 0x71, 0x79, 0x17, 0x48, 0x69,
    0x21, 0x12, 0x3c, 0x84, 0x83, 0x6d, 0xd6, 0x96, 0xc3, 0x38, 0x1e, 0x4d, 0xe1, 0xd4, 0xc2, 0x2c,
    0x18, 0x81, 0xb4, 0x4b, 0x16, 0x61, 0xc1, 0x24, 0x1c, 0x49, 0x14, 0x42, 0x94, 0x41, 0x34, 0x43,
};

constexpr uint8_t kBc4Pair[64][2] = {
    {1, 1}, {0, 0}, {3, 3}, {6, 6}, {4, 4}, {5, 5}, {3, 0}, {0, 3},
    {6, 1}, {1, 6}, {7, 7}, {2, 2}, {6, 3}, {3, 6}, {1, 0}, {0, 1},
    {4, 1}, {1, 4}, {3, 1}, {1, 3}, {6, 0}, {0, 6}, {4, 2}, {7, 5},
    {5, 7}, {2, 4}, {0, 5}, {5, 0}, {6, 4}, {4, 6}, {5, 3}, {3, 5},
    {4, 0}, {0, 4}, {1, 5}, {5, 1}, {3, 4}, {4, 3}, {2, 6}, {6, 2},
    {7, 3}, {3, 7}, {1, 2}, {2, 1}, {0, 7}, {7, 0}, {2, 5}, {5, 2},
    {7, 4}, {4, 7}, {4, 5}, {5, 4}, {3, 2}, {2, 3}, {0, 2}, {2, 0},
    {1, 7}, {7, 1}, {6, 5}, {5, 6}, {6, 7}, {7, 6}, {2, 7}, {7, 2},
};

constexpr uint8_t kBc5Pair[64][2] = {
    {0, 0}, {1, 1}, {4, 4}, {3, 3}, {6, 6}, {7, 7}, {4, 0}, {0, 4},
    {0, 1}, {2, 2}, {1, 0}, {3, 0}, {0, 3}, {5, 5}, {2, 0}, {0, 2},
    {1, 6}, {6, 1}, {4, 3}, {1, 7}, {3, 4}, {1, 4}, {7, 1}, {6, 0},
    {0, 6}, {6, 3}, {6, 4}, {3, 6}, {3, 2}, {4, 6}, {1, 5}, {1, 3},
    {2, 3}, {5, 4}, {5, 3}, {5, 0}, {4, 1}, {7, 6}, {4, 5}, {4, 2},
    {3, 1}, {3, 5}, {5, 1}, {6, 7}, {2, 4}, {7, 5}, {7, 4}, {0, 5},
    {6, 5}, {4, 7}, {5, 7}, {7, 0}, {5, 6}, {0, 7}, {7, 3}, {3, 7},
    {5, 2}, {2, 5}, {1, 2}, {6, 2}, {2, 1}, {2, 6}, {7, 2}, {2, 7},
};

struct Bc1HistInit { uint8_t idx; uint8_t v[6]; };
constexpr Bc1HistInit kBc1HistKnown[] = {
    // Confirmed (248 entries) + Provisional (7 entries) from MIT `bcpack_tables.py`.
    {1, {7, 29, 6, 8, 30, 7}}, {2, {15, 29, 15, 16, 36, 17}}, {3, {20, 38, 14, 3, 5, 2}},
    {4, {12, 27, 13, 6, 12, 6}}, {5, {10, 22, 11, 6, 14, 7}}, {6, {12, 24, 11, 7, 14, 6}},
    {7, {12, 26, 12, 7, 14, 7}}, {8, {22, 36, 15, 20, 32, 14}}, {9, {14, 23, 8, 1, 2, 1}},
    {10, {23, 5, 30, 9, 5, 30}}, {11, {11, 22, 11, 7, 15, 7}}, {12, {17, 22, 7, 10, 11, 3}},
    {13, {12, 25, 12, 7, 16, 7}}, {14, {13, 28, 8, 5, 11, 3}}, {15, {11, 20, 8, 16, 31, 14}},
    {16, {23, 32, 30, 17, 12, 29}}, {17, {8, 44, 30, 6, 26, 30}}, {18, {10, 24, 30, 6, 45, 30}},
    {19, {10, 12, 4, 8, 8, 2}}, {20, {16, 23, 30, 14, 6, 26}}, {21, {12, 25, 13, 10, 22, 12}},
    {22, {19, 27, 10, 12, 15, 5}}, {23, {11, 24, 12, 4, 11, 6}}, {24, {29, 60, 29, 6, 13, 6}},
    {25, {17, 31, 11, 1, 2, 1}}, {26, {26, 54, 24, 12, 24, 10}}, {27, {23, 49, 30, 20, 36, 30}},
    {28, {16, 23, 10, 10, 14, 6}}, {29, {11, 24, 10, 6, 13, 5}}, {30, {18, 52, 30, 14, 22, 30}},
    {31, {14, 51, 30, 9, 34, 30}}, {32, {2, 3, 1, 19, 37, 16}}, {33, {14, 15, 30, 9, 31, 30}},
    {34, {29, 57, 27, 27, 52, 25}}, {35, {13, 29, 15, 8, 20, 11}}, {36, {13, 17, 5, 9, 10, 3}},
    {37, {18, 50, 29, 15, 10, 29}}, {38, {11, 25, 13, 8, 19, 10}}, {39, {17, 32, 12, 6, 11, 4}},
    {40, {22, 18, 30, 19, 41, 30}}, {41, {10, 22, 11, 8, 18, 9}}, {42, {18, 30, 31, 10, 44, 30}},
    {43, {25, 50, 24, 2, 4, 1}}, {44, {11, 30, 31, 7, 15, 29}}, {45, {12, 19, 8, 11, 16, 7}},
    {46, {19, 29, 30, 14, 55, 28}}, {47, {9, 20, 10, 4, 10, 5}}, {48, {12, 24, 10, 7, 13, 5}},
    {49, {26, 53, 25, 6, 13, 6}}, {50, {19, 27, 31, 11, 19, 30}}, {51, {21, 31, 30, 22, 31, 30}},
    {52, {10, 19, 5, 11, 21, 6}}, {53, {30, 50, 3, 29, 46, 2}}, {54, {15, 9, 30, 7, 9, 30}},
    {55, {14, 18, 6, 11, 14, 4}}, {56, {28, 56, 26, 4, 7, 3}}, {57, {9, 20, 10, 5, 13, 7}},
    {58, {28, 58, 26, 15, 32, 14}}, {59, {11, 19, 8, 6, 9, 4}}, {60, {16, 22, 9, 13, 17, 7}},
    {61, {1, 2, 1, 14, 28, 12}}, {62, {22, 45, 30, 9, 15, 30}}, {63, {19, 22, 30, 16, 23, 31}},
    {64, {25, 50, 24, 21, 43, 20}}, {65, {6, 11, 4, 16, 31, 13}}, {66, {13, 25, 8, 3, 6, 1}},
    {67, {5, 9, 3, 12, 24, 10}}, {68, {16, 29, 13, 7, 13, 5}}, {69, {20, 38, 16, 13, 25, 10}},
    {70, {18, 35, 15, 5, 9, 3}}, {71, {13, 25, 11, 6, 11, 4}}, {72, {11, 21, 7, 7, 13, 4}},
    {73, {20, 39, 16, 11, 20, 8}}, {74, {16, 31, 12, 4, 8, 2}}, {75, {22, 41, 30, 18, 23, 30}},
    {76, {24, 50, 24, 9, 19, 8}}, {77, {3, 12, 7, 4, 16, 8}}, {78, {18, 27, 12, 16, 22, 10}},
    {79, {24, 49, 23, 13, 28, 12}}, {80, {24, 50, 24, 18, 37, 18}}, {81, {17, 33, 12, 14, 27, 9}},
    {82, {20, 10, 31, 13, 10, 31}}, {83, {7, 8, 2, 5, 5, 1}}, {84, {1, 2, 1, 10, 21, 9}},
    {85, {10, 18, 6, 1, 1, 0}}, {86, {22, 46, 21, 1, 3, 1}}, {87, {11, 20, 8, 8, 14, 5}},
    {88, {17, 35, 31, 14, 14, 29}}, {89, {16, 22, 31, 12, 40, 31}}, {90, {12, 22, 9, 8, 15, 6}},
    {91, {18, 15, 30, 15, 33, 31}}, {92, {19, 32, 30, 15, 43, 31}}, {93, {18, 35, 15, 8, 15, 5}},
    {94, {21, 43, 20, 18, 37, 17}}, {95, {10, 17, 6, 9, 15, 6}}, {96, {14, 27, 12, 7, 13, 5}},
    {97, {8, 16, 7, 12, 25, 11}}, {98, {18, 37, 17, 1, 1, 0}}, {99, {17, 45, 30, 15, 33, 31}},
    {100, {13, 25, 11, 8, 16, 7}}, {101, {23, 47, 22, 16, 32, 15}}, {102, {18, 33, 13, 11, 21, 8}},
    {103, {4, 10, 4, 8, 17, 7}}, {104, {1, 2, 1, 3, 7, 3}}, {105, {11, 22, 9, 6, 11, 4}},
    {106, {16, 29, 11, 13, 23, 9}}, {107, {12, 24, 12, 6, 12, 6}}, {108, {23, 47, 22, 7, 14, 6}},
    {109, {11, 32, 30, 11, 31, 30}}, {110, {14, 26, 11, 10, 18, 7}}, {111, {20, 41, 19, 1, 3, 1}},
    {112, {14, 28, 12, 8, 16, 6}}, {113, {11, 21, 9, 8, 15, 6}}, {114, {1, 2, 1, 7, 13, 6}},
    {115, {19, 35, 14, 16, 29, 12}}, {116, {13, 27, 13, 7, 15, 7}}, {117, {15, 33, 16, 8, 18, 9}},
    {118, {15, 42, 31, 11, 22, 30}}, {119, {23, 48, 23, 4, 9, 4}}, {120, {22, 45, 21, 11, 22, 10}},
    {121, {31, 62, 30, 31, 61, 29}}, {122, {13, 26, 11, 5, 8, 3}}, {123, {19, 39, 19, 9, 20, 10}},
    {124, {7, 14, 2, 6, 12, 2}}, {125, {21, 42, 20, 3, 7, 3}}, {126, {10, 17, 7, 8, 13, 5}},
    {127, {11, 20, 8, 9, 17, 7}}, {128, {19, 33, 31, 12, 31, 31}}, {129, {21, 43, 20, 5, 11, 5}},
    {130, {10, 20, 9, 7, 13, 6}}, {131, {10, 21, 10, 6, 13, 6}}, {132, {15, 29, 12, 11, 20, 8}},
    {133, {21, 43, 20, 9, 18, 8}}, {134, {3, 8, 3, 5, 11, 5}}, {135, {16, 31, 13, 9, 17, 7}},
    {136, {11, 22, 11, 6, 12, 6}}, {137, {10, 21, 10, 7, 15, 7}}, {138, {27, 3, 1, 26, 1, 0}},
    {139, {18, 37, 18, 16, 32, 16}}, {140, {12, 23, 10, 9, 17, 7}}, {141, {12, 25, 12, 8, 17, 8}},
    {142, {11, 21, 9, 2, 4, 1}}, {143, {7, 15, 7, 8, 18, 9}}, {144, {17, 33, 15, 13, 24, 11}},
    {145, {9, 19, 9, 7, 15, 7}}, {146, {21, 43, 20, 13, 27, 12}}, {147, {17, 36, 17, 2, 4, 2}},
    {148, {10, 20, 9, 6, 12, 5}}, {149, {20, 41, 19, 7, 14, 7}}, {150, {3, 9, 6, 2, 8, 5}},
    {151, {13, 24, 10, 11, 20, 8}}, {152, {18, 38, 18, 4, 7, 3}}, {153, {19, 38, 18, 8, 17, 8}},
    {154, {16, 35, 17, 4, 10, 4}}, {155, {11, 23, 11, 10, 21, 10}}, {156, {17, 36, 17, 7, 14, 7}},
    {157, {13, 27, 13, 6, 12, 6}}, {158, {9, 15, 4, 8, 13, 4}}, {159, {10, 19, 7, 5, 9, 3}},
    {160, {19, 40, 19, 11, 24, 11}}, {161, {11, 23, 11, 8, 17, 8}}, {162, {14, 29, 14, 7, 15, 7}},
    {163, {12, 22, 9, 5, 8, 3}}, {164, {10, 20, 9, 8, 17, 8}}, {165, {21, 42, 19, 15, 31, 14}},
    {166, {14, 29, 13, 1, 1, 0}}, {167, {8, 16, 8, 7, 14, 7}}, {168, {9, 18, 8, 8, 16, 7}},
    {169, {10, 17, 7, 3, 4, 1}}, {170, {14, 28, 13, 2, 4, 1}}, {171, {14, 30, 14, 3, 6, 3}},
    {172, {19, 38, 18, 5, 11, 5}}, {173, {12, 26, 12, 1, 2, 1}}, {174, {9, 16, 6, 8, 13, 5}},
    {175, {8, 13, 4, 7, 11, 4}}, {176, {16, 34, 16, 3, 7, 3}}, {177, {13, 28, 13, 4, 9, 4}},
    {178, {15, 31, 15, 6, 13, 6}}, {179, {12, 24, 11, 9, 19, 9}}, {180, {9, 19, 8, 7, 14, 6}},
    {181, {14, 28, 13, 5, 10, 4}}, {182, {10, 20, 7, 4, 6, 2}}, {183, {16, 34, 17, 11, 25, 12}},
    {184, {15, 30, 15, 8, 16, 8}}, {185, {13, 27, 13, 10, 20, 10}}, {186, {9, 20, 9, 0, 1, 0}},
    {187, {16, 32, 15, 2, 4, 2}}, {188, {13, 27, 12, 3, 6, 3}}, {189, {13, 26, 12, 12, 23, 11}},
    {190, {12, 25, 12, 5, 10, 5}}, {191, {11, 23, 11, 5, 11, 5}}, {192, {16, 34, 16, 6, 12, 5}},
    {193, {16, 35, 31, 15, 25, 31}}, {194, {16, 33, 16, 7, 15, 7}}, {195, {13, 27, 13, 8, 18, 8}},
    {196, {18, 36, 17, 11, 22, 10}}, {197, {18, 37, 18, 13, 27, 13}}, {198, {12, 25, 12, 4, 8, 4}},
    {199, {9, 18, 7, 6, 11, 4}}, {200, {15, 30, 14, 9, 19, 9}}, {201, {17, 35, 16, 9, 18, 9}},
    {202, {11, 24, 11, 2, 4, 2}}, {203, {16, 33, 16, 0, 1, 0}}, {204, {15, 31, 15, 4, 9, 4}},
    {205, {16, 31, 15, 14, 27, 13}}, {206, {14, 29, 14, 13, 25, 12}}, {207, {11, 23, 11, 3, 6, 3}},
    {208, {16, 32, 16, 10, 21, 10}}, {209, {11, 22, 11, 4, 9, 4}}, {210, {11, 22, 10, 1, 2, 0}},
    {211, {8, 14, 5, 2, 4, 1}}, {212, {9, 16, 6, 6, 10, 4}}, {213, {10, 21, 10, 9, 19, 9}},
    {214, {14, 30, 14, 11, 22, 11}}, {215, {9, 18, 8, 6, 13, 6}}, {216, {6, 13, 6, 5, 11, 5}},
    {217, {7, 14, 6, 0, 1, 0}}, {218, {4, 7, 2, 3, 4, 1}}, {219, {10, 20, 9, 4, 8, 4}},
    {220, {9, 17, 7, 5, 9, 3}}, {221, {7, 14, 7, 1, 2, 1}}, {222, {10, 20, 10, 5, 10, 5}},
    {223, {8, 15, 5, 4, 6, 2}}, {224, {8, 17, 8, 1, 2, 1}}, {225, {10, 20, 10, 3, 6, 3}},
    {226, {7, 10, 3, 6, 8, 3}}, {227, {8, 15, 7, 6, 12, 6}}, {228, {9, 19, 9, 2, 4, 2}},
    {229, {6, 11, 4, 3, 4, 1}}, {230, {6, 11, 4, 0, 1, 0}}, {231, {8, 16, 7, 2, 4, 2}},
    {232, {16, 34, 16, 15, 29, 15}}, {233, {7, 16, 7, 5, 10, 5}}, {234, {8, 14, 5, 5, 9, 3}},
    {235, {9, 18, 8, 4, 8, 4}}, {236, {5, 9, 3, 4, 7, 2}}, {237, {9, 18, 8, 5, 10, 5}},
    {238, {6, 12, 6, 2, 4, 2}}, {239, {7, 12, 5, 4, 7, 3}}, {240, {5, 11, 5, 1, 2, 1}},
    {241, {8, 17, 8, 3, 6, 3}}, {242, {16, 31, 31, 16, 32, 31}}, {243, {7, 14, 7, 3, 6, 3}},
    {244, {5, 11, 6, 4, 9, 4}}, {245, {7, 15, 7, 4, 8, 4}}, {246, {2, 4, 2, 0, 0, 1}},
    {247, {5, 11, 5, 3, 6, 3}}, {248, {4, 8, 4, 1, 1, 1}}, {249, {3, 6, 3, 1, 2, 1}},
    {250, {2, 4, 1, 1, 2, 1}}, {251, {4, 8, 4, 3, 6, 3}}, {252, {3, 6, 3, 2, 4, 2}},
    {253, {4, 9, 4, 2, 4, 2}}, {254, {17, 27, 31, 15, 34, 31}}, {255, {0, 0, 1, 0, 0, 0}},
};

struct Bc3HistInit { uint8_t idx; uint8_t v[8]; };
constexpr Bc3HistInit kBc3HistKnown[] = {
    {2, {177, 134, 27, 55, 24, 14, 54, 24}}, {26, {255, 255, 21, 34, 8, 15, 22, 3}},
    {29, {220, 109, 17, 35, 17, 15, 30, 15}}, {30, {219, 96, 2, 53, 14, 2, 59, 16}},
    {35, {95, 65, 16, 31, 16, 15, 31, 15}}, {37, {136, 13, 16, 33, 16, 15, 32, 15}},
    {47, {146, 108, 18, 37, 18, 13, 26, 13}}, {52, {255, 255, 20, 38, 16, 3, 5, 2}},
    {71, {200, 51, 17, 34, 17, 15, 31, 15}}, {76, {170, 150, 2, 60, 20, 1, 61, 20}},
    {78, {248, 10, 3, 4, 2, 0, 1, 0}}, {95, {237, 229, 26, 51, 25, 1, 3, 1}},
    {98, {255, 0, 19, 38, 18, 14, 29, 13}}, {104, {255, 6, 13, 27, 13, 8, 17, 8}},
    {109, {254, 4, 15, 27, 11, 3, 4, 2}}, {112, {117, 39, 21, 41, 21, 1, 3, 1}},
    {126, {170, 119, 16, 31, 16, 15, 30, 15}}, {138, {122, 121, 14, 29, 14, 13, 27, 13}},
    {139, {247, 1, 15, 28, 13, 9, 16, 7}}, {153, {105, 126, 2, 7, 4, 1, 3, 2}},
    {154, {93, 100, 20, 40, 20, 15, 30, 15}}, {166, {91, 99, 17, 35, 17, 15, 30, 15}},
    {167, {155, 99, 16, 33, 16, 15, 31, 15}}, {177, {255, 255, 13, 24, 12, 11, 23, 10}},
    {185, {254, 255, 10, 17, 7, 2, 3, 1}}, {197, {253, 255, 11, 20, 9, 6, 11, 4}},
    {210, {254, 255, 5, 10, 5, 1, 2, 1}}, {220, {255, 255, 11, 20, 9, 9, 18, 7}},
    {225, {106, 113, 24, 39, 23, 0, 13, 0}}, {226, {1, 255, 8, 15, 7, 5, 10, 4}},
    {230, {1, 2, 0, 1, 1, 0, 0, 0}}, {232, {140, 131, 16, 31, 16, 15, 34, 16}},
    {237, {255, 254, 5, 11, 5, 4, 9, 3}}, {244, {255, 255, 1, 1, 1, 0, 0, 0}},
    {246, {253, 254, 3, 5, 2, 1, 2, 1}},
};

struct Bc4HistInit { uint8_t idx; uint8_t v[2]; };
constexpr Bc4HistInit kBc4HistKnown[] = {
    {25, {81, 50}}, {33, {58, 31}}, {77, {164, 124}}, {84, {204, 140}},
    {93, {208, 61}}, {101, {171, 137}}, {102, {212, 125}}, {116, {77, 34}},
    {119, {193, 127}}, {121, {178, 118}}, {126, {138, 108}}, {127, {132, 115}},
    {131, {165, 52}}, {142, {142, 80}}, {145, {74, 16}}, {148, {150, 124}},
    {154, {195, 103}}, {157, {71, 51}}, {163, {55, 33}}, {172, {139, 119}},
    {173, {42, 15}}, {178, {91, 47}}, {179, {48, 8}}, {184, {72, 34}},
    {185, {158, 136}}, {187, {105, 12}}, {188, {142, 38}}, {190, {43, 25}},
    {197, {133, 133}}, {200, {55, 17}}, {202, {82, 43}}, {207, {127, 129}},
    {208, {39, 9}}, {209, {134, 27}}, {211, {39, 23}}, {213, {19, 41}},
    {214, {39, 17}}, {217, {115, 21}}, {218, {118, 60}}, {221, {47, 17}},
    {223, {64, 25}}, {224, {119, 33}}, {227, {33, 16}}, {232, {78, 5}},
    {234, {33, 60}}, {235, {57, 24}}, {236, {69, 2}}, {239, {105, 24}},
    {240, {96, 34}}, {241, {111, 44}}, {243, {47, 25}}, {244, {84, 23}},
    {247, {22, 17}}, {252, {27, 8}}, {254, {12, 6}},
};

struct Bc5HistInit { uint8_t idx; uint8_t v[4]; };
constexpr Bc5HistInit kBc5HistKnown[] = {
    {3, {130, 218, 45, 192}}, {4, {172, 109, 158, 94}}, {5, {183, 136, 113, 69}},
    {6, {125, 129, 86, 34}}, {7, {171, 196, 33, 82}}, {9, {142, 149, 102, 118}},
    {14, {82, 62, 26, 23}}, {15, {100, 94, 32, 32}}, {16, {97, 123, 61, 97}},
    {19, {133, 140, 118, 123}}, {20, {206, 194, 128, 123}}, {22, {147, 145, 64, 66}},
    {24, {76, 189, 55, 171}}, {27, {94, 115, 86, 102}}, {28, {163, 38, 152, 29}},
    {29, {143, 133, 89, 80}}, {30, {197, 67, 172, 50}}, {31, {164, 218, 130, 127}},
    {32, {141, 145, 111, 132}}, {33, {177, 125, 131, 32}}, {34, {49, 113, 26, 94}},
    {36, {173, 169, 40, 28}}, {38, {117, 125, 43, 50}}, {39, {139, 111, 99, 72}},
    {40, {123, 93, 83, 49}}, {41, {189, 166, 135, 116}}, {42, {198, 97, 178, 73}},
    {43, {205, 201, 76, 86}}, {44, {172, 174, 73, 72}}, {46, {157, 210, 136, 188}},
    {47, {126, 149, 115, 129}}, {48, {158, 67, 137, 41}}, {49, {214, 120, 131, 80}},
    {51, {147, 150, 119, 119}}, {52, {169, 107, 135, 65}}, {54, {231, 168, 223, 154}},
    {55, {115, 144, 99, 120}}, {56, {74, 104, 50, 83}}, {57, {220, 132, 32, 123}},
    {58, {150, 136, 110, 96}}, {60, {139, 147, 124, 108}}, {61, {121, 124, 78, 94}},
    {63, {169, 151, 124, 103}}, {64, {120, 106, 103, 88}}, {65, {134, 147, 110, 109}},
    {67, {133, 125, 103, 107}}, {68, {160, 133, 118, 123}}, {69, {175, 78, 163, 68}},
    {70, {229, 133, 166, 121}}, {71, {233, 147, 224, 134}}, {72, {127, 142, 104, 121}},
    {73, {134, 150, 99, 121}}, {74, {110, 149, 83, 129}}, {75, {175, 187, 129, 141}},
    {77, {115, 124, 96, 104}}, {78, {164, 128, 141, 109}}, {79, {129, 85, 114, 22}},
    {80, {246, 131, 241, 125}}, {82, {168, 186, 145, 165}}, {83, {192, 133, 158, 100}},
    {84, {107, 143, 105, 133}}, {85, {201, 212, 172, 181}}, {86, {124, 138, 116, 121}},
    {87, {92, 60, 71, 40}}, {88, {203, 134, 149, 126}}, {90, {61, 123, 25, 114}},
    {91, {123, 160, 113, 145}}, {92, {150, 169, 135, 150}}, {93, {134, 139, 101, 109}},
    {94, {253, 94, 255, 63}}, {95, {123, 87, 106, 67}}, {96, {238, 133, 121, 121}},
    {98, {24, 18, 15, 10}}, {99, {117, 41, 97, 25}}, {100, {123, 121, 70, 67}},
    {101, {64, 59, 51, 49}}, {102, {11, 103, 12, 88}}, {103, {144, 133, 111, 123}},
    {104, {189, 200, 65, 52}}, {105, {219, 153, 195, 135}}, {106, {150, 110, 138, 98}},
    {107, {118, 142, 60, 117}}, {108, {139, 145, 114, 118}}, {109, {216, 225, 191, 200}},
    {110, {46, 46, 38, 33}}, {111, {91, 132, 85, 126}}, {112, {154, 178, 115, 132}},
    {114, {131, 127, 125, 17}}, {115, {136, 134, 15, 123}}, {116, {131, 129, 105, 123}},
    {119, {132, 215, 122, 149}}, {120, {129, 138, 112, 111}}, {123, {136, 145, 119, 131}},
    {124, {179, 196, 165, 183}}, {125, {126, 125, 111, 92}}, {126, {136, 150, 122, 123}},
    {127, {168, 158, 136, 126}}, {128, {146, 153, 129, 134}}, {129, {127, 130, 85, 124}},
    {130, {132, 241, 120, 126}}, {131, {155, 124, 125, 85}}, {132, {121, 115, 115, 108}},
    {133, {106, 72, 89, 51}}, {134, {136, 155, 118, 137}}, {135, {121, 145, 106, 133}},
    {136, {119, 220, 102, 209}}, {137, {179, 136, 150, 122}}, {138, {120, 133, 102, 113}},
    {139, {145, 142, 111, 112}}, {140, {130, 108, 123, 62}}, {141, {121, 132, 37, 122}},
    {142, {157, 142, 135, 119}}, {143, {80, 201, 59, 191}}, {144, {130, 133, 122, 104}},
    {145, {138, 136, 112, 120}}, {146, {131, 107, 123, 37}}, {147, {122, 120, 107, 119}},
    {148, {126, 181, 121, 146}}, {149, {143, 141, 137, 135}}, {150, {134, 178, 121, 161}},
    {151, {103, 135, 55, 126}}, {152, {193, 132, 128, 123}}, {153, {141, 129, 111, 114}},
    {154, {79, 77, 53, 51}}, {155, {133, 72, 127, 64}}, {157, {102, 101, 83, 80}},
    {159, {92, 137, 39, 125}}, {160, {108, 136, 87, 122}}, {161, {102, 97, 65, 57}},
    {162, {163, 149, 151, 134}}, {163, {133, 17, 121, 15}}, {165, {179, 162, 153, 133}},
    {166, {119, 132, 98, 125}}, {167, {175, 132, 130, 124}}, {169, {132, 92, 124, 81}},
    {170, {40, 31, 27, 19}}, {171, {188, 211, 180, 202}}, {172, {139, 121, 117, 101}},
    {173, {150, 125, 130, 106}}, {174, {129, 184, 123, 131}}, {175, {132, 158, 126, 148}},
    {176, {130, 155, 126, 129}}, {177, {131, 167, 124, 127}}, {178, {155, 131, 132, 125}},
    {179, {198, 139, 170, 124}}, {180, {200, 196, 190, 186}}, {182, {133, 126, 124, 55}},
    {183, {128, 126, 116, 112}}, {184, {173, 157, 163, 147}}, {185, {197, 175, 158, 137}},
    {186, {102, 129, 98, 126}}, {187, {131, 216, 121, 198}}, {188, {127, 135, 115, 130}},
    {189, {139, 196, 94, 111}}, {190, {132, 106, 123, 94}}, {191, {219, 130, 193, 116}},
    {192, {212, 133, 127, 124}}, {193, {196, 176, 173, 151}}, {194, {56, 192, 15, 27}},
    {195, {129, 198, 124, 131}}, {196, {131, 124, 125, 99}}, {197, {176, 130, 169, 126}},
    {198, {137, 127, 121, 111}}, {199, {144, 137, 131, 123}}, {200, {132, 130, 114, 120}},
    {201, {182, 165, 169, 153}}, {202, {138, 136, 113, 109}}, {203, {185, 172, 179, 164}},
    {204, {131, 127, 123, 81}}, {205, {124, 141, 121, 137}}, {206, {173, 131, 136, 92}},
    {208, {120, 133, 108, 123}}, {209, {147, 134, 123, 110}}, {210, {130, 142, 125, 129}},
    {211, {136, 137, 122, 131}}, {212, {130, 119, 127, 114}}, {214, {117, 136, 111, 131}},
    {215, {199, 181, 187, 167}}, {216, {132, 215, 124, 125}}, {217, {135, 138, 120, 114}},
    {218, {131, 85, 123, 45}}, {219, {197, 131, 193, 126}}, {221, {69, 206, 57, 195}},
    {222, {131, 128, 123, 37}}, {223, {161, 156, 96, 92}}, {225, {130, 136, 123, 121}},
    {226, {126, 60, 118, 46}}, {227, {139, 137, 120, 124}}, {228, {140, 130, 124, 123}},
    {229, {122, 127, 118, 123}}, {231, {130, 141, 127, 138}}, {232, {54, 205, 50, 185}},
    {233, {75, 74, 69, 68}}, {234, {223, 130, 217, 125}}, {235, {111, 129, 108, 127}},
    {236, {160, 129, 157, 126}}, {237, {129, 217, 126, 217}}, {238, {148, 128, 146, 126}},
    {239, {130, 129, 127, 118}}, {241, {60, 200, 50, 200}}, {242, {131, 130, 120, 124}},
    {243, {123, 132, 116, 127}}, {244, {132, 135, 130, 132}}, {245, {129, 41, 126, 37}},
    {246, {122, 133, 120, 133}}, {247, {118, 128, 116, 126}}, {248, {136, 128, 133, 126}},
    {249, {128, 133, 123, 131}}, {250, {82, 81, 78, 77}}, {251, {124, 130, 122, 128}},
    {252, {130, 130, 125, 124}}, {253, {128, 128, 127, 127}}, {254, {127, 126, 126, 125}},
    {255, {129, 127, 128, 126}},
};

struct ExpandedS3tcRgtcHistory {
    std::array<std::array<uint8_t, 6>, 256> bc1{};
    std::array<std::array<uint8_t, 8>, 256> bc3{};
    std::array<std::array<uint8_t, 2>, 256> bc4{};
    std::array<std::array<uint8_t, 4>, 256> bc5{};
};

const ExpandedS3tcRgtcHistory& GetS3tcRgtcHistory() noexcept {
    static const ExpandedS3tcRgtcHistory kHist = []() {
        ExpandedS3tcRgtcHistory h{};
        for (auto& e : h.bc3) {
            e = {128u, 128u, 0u, 0u, 0u, 0u, 0u, 0u};
        }
        for (auto& e : h.bc4) {
            e = {128u, 128u};
        }
        for (auto& e : h.bc5) {
            e = {128u, 128u, 128u, 128u};
        }
        for (const auto& it : kBc1HistKnown) {
            for (size_t c = 0u; c < 6u; ++c) h.bc1[it.idx][c] = it.v[c];
        }
        for (const auto& it : kBc3HistKnown) {
            for (size_t c = 0u; c < 8u; ++c) h.bc3[it.idx][c] = it.v[c];
        }
        for (const auto& it : kBc4HistKnown) {
            for (size_t c = 0u; c < 2u; ++c) h.bc4[it.idx][c] = it.v[c];
        }
        for (const auto& it : kBc5HistKnown) {
            for (size_t c = 0u; c < 4u; ++c) h.bc5[it.idx][c] = it.v[c];
        }
        return h;
    }();
    return kHist;
}

inline void StoreLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

inline void StoreLe48(uint8_t* p, uint64_t v) noexcept {
    for (uint32_t i = 0u; i < 6u; ++i) {
        p[i] = static_cast<uint8_t>((v >> (i * 8u)) & 0xFFull);
    }
}

Status RebuildBc1Blocks(
    const std::array<std::vector<uint8_t>, kMaxLanes>& lanes,
    size_t nblocks,
    uint8_t* out) {
    for (uint8_t z : lanes[1]) {
        if (z != 0u) {
            return Status::Error(StatusCode::ParseError, "BC1 reserved lane 1 is non-zero");
        }
    }
    const auto& pre = GetS3tcRgtcHistory().bc1;
    std::vector<std::array<uint8_t, 6>> hist(nblocks);
    constexpr uint32_t kMasks[6] = {31u, 63u, 31u, 31u, 63u, 31u};

    for (size_t i = 0u; i < nblocks; ++i) {
        const uint32_t v = lanes[0][i];
        const uint32_t d = WrapModPow2(Unzig(v), 255u);
        std::array<uint8_t, 6> base{};
        if (d != 0u) {
            if (i >= d) {
                base = hist[i - d];
            } else {
                base = pre[(256u + i - d) & 0xFFu];
            }
        }
        std::array<uint8_t, 6> e{};
        for (size_t k = 0u; k < 6u; ++k) {
            e[k] = static_cast<uint8_t>(
                WrapModPow2(static_cast<int32_t>(base[k]) + Unzig(lanes[2u + k][i]), kMasks[k]));
        }
        hist[i] = e;

        const uint16_t c0 = static_cast<uint16_t>(
            (static_cast<uint16_t>(e[0]) << 11u) |
            (static_cast<uint16_t>(e[1]) << 5u) |
            static_cast<uint16_t>(e[2]));
        const uint16_t c1 = static_cast<uint16_t>(
            (static_cast<uint16_t>(e[3]) << 11u) |
            (static_cast<uint16_t>(e[4]) << 5u) |
            static_cast<uint16_t>(e[5]));

        const uint8_t qa = kBc1Quadrant[lanes[8][i]];
        const uint8_t qb = kBc1Quadrant[lanes[9][i]];
        const uint8_t qc = kBc1Quadrant[lanes[10][i]];
        const uint8_t qd = kBc1Quadrant[lanes[11][i]];

        uint8_t* blk = out + i * 8u;
        StoreLe16(blk, c0);
        StoreLe16(blk + 2u, c1);
        blk[4] = static_cast<uint8_t>((qa & 0x0Fu) | ((qb & 0x0Fu) << 4u));
        blk[5] = static_cast<uint8_t>((qa >> 4u) | ((qb >> 4u) << 4u));
        blk[6] = static_cast<uint8_t>((qc & 0x0Fu) | ((qd & 0x0Fu) << 4u));
        blk[7] = static_cast<uint8_t>((qc >> 4u) | ((qd >> 4u) << 4u));
    }
    return Status::Ok();
}

Status RebuildBc3Blocks(
    const std::array<std::vector<uint8_t>, kMaxLanes>& lanes,
    size_t nblocks,
    uint8_t* out) {
    for (uint8_t z : lanes[1]) {
        if (z != 0u) {
            return Status::Error(StatusCode::ParseError, "BC3 reserved lane 1 is non-zero");
        }
    }
    const auto& pre = GetS3tcRgtcHistory().bc3;
    std::vector<std::array<uint8_t, 8>> hist(nblocks);
    constexpr uint32_t kMasks[8] = {255u, 255u, 31u, 63u, 31u, 31u, 63u, 31u};
    constexpr std::array<uint8_t, 8> kNone = {128u, 128u, 0u, 0u, 0u, 0u, 0u, 0u};

    for (size_t i = 0u; i < nblocks; ++i) {
        const size_t a = 2u * i;
        const size_t b = 2u * i + 1u;
        const uint32_t d = WrapModPow2(Unzig(Inter8(lanes[0][a], lanes[0][b])), 255u);
        std::array<uint8_t, 8> base = kNone;
        if (d != 0u) {
            base = (i >= d) ? hist[i - d] : pre[(256u + i - d) & 0xFFu];
        }

        const uint8_t dz[8] = {
            lanes[2][a], lanes[2][b],
            lanes[7][a], lanes[8][a], lanes[9][a],
            lanes[7][b], lanes[8][b], lanes[9][b],
        };
        std::array<uint8_t, 8> e{};
        for (size_t k = 0u; k < 8u; ++k) {
            e[k] = static_cast<uint8_t>(
                WrapModPow2(static_cast<int32_t>(base[k]) + Unzig(dz[k]), kMasks[k]));
        }
        hist[i] = e;

        uint64_t abits = 0ull;
        for (uint32_t r = 0u; r < 4u; ++r) {
            const uint8_t* lp = kBc3AlphaPair[lanes[3u + r][a] & 63u];
            const uint8_t* rp = kBc3AlphaPair[lanes[3u + r][b] & 63u];
            const uint64_t row12 =
                static_cast<uint64_t>(lp[0]) |
                (static_cast<uint64_t>(lp[1]) << 3u) |
                (static_cast<uint64_t>(rp[0]) << 6u) |
                (static_cast<uint64_t>(rp[1]) << 9u);
            abits |= (row12 << (12u * r));
        }

        const uint16_t c0 = static_cast<uint16_t>(
            (static_cast<uint16_t>(e[2]) << 11u) |
            (static_cast<uint16_t>(e[3]) << 5u) |
            static_cast<uint16_t>(e[4]));
        const uint16_t c1 = static_cast<uint16_t>(
            (static_cast<uint16_t>(e[5]) << 11u) |
            (static_cast<uint16_t>(e[6]) << 5u) |
            static_cast<uint16_t>(e[7]));

        const uint8_t qa = kBc3ColourQuadrant[lanes[10][a]];
        const uint8_t qb = kBc3ColourQuadrant[lanes[10][b]];
        const uint8_t qc = kBc3ColourQuadrant[lanes[11][a]];
        const uint8_t qd = kBc3ColourQuadrant[lanes[11][b]];

        uint8_t* blk = out + i * 16u;
        blk[0] = e[0];
        blk[1] = e[1];
        StoreLe48(blk + 2u, abits);
        StoreLe16(blk + 8u, c0);
        StoreLe16(blk + 10u, c1);
        blk[12] = static_cast<uint8_t>((qa & 0x0Fu) | ((qb & 0x0Fu) << 4u));
        blk[13] = static_cast<uint8_t>((qa >> 4u) | ((qb >> 4u) << 4u));
        blk[14] = static_cast<uint8_t>((qc & 0x0Fu) | ((qd & 0x0Fu) << 4u));
        blk[15] = static_cast<uint8_t>((qc >> 4u) | ((qd >> 4u) << 4u));
    }
    return Status::Ok();
}

Status RebuildBc4Blocks(
    const std::array<std::vector<uint8_t>, kMaxLanes>& lanes,
    size_t nblocks,
    uint8_t* out) {
    for (uint8_t z : lanes[1]) {
        if (z != 0u) {
            return Status::Error(StatusCode::ParseError, "BC4 reserved lane 1 is non-zero");
        }
    }
    const auto& pre = GetS3tcRgtcHistory().bc4;
    std::vector<std::array<uint8_t, 2>> hist(nblocks);

    for (size_t i = 0u; i < nblocks; ++i) {
        const uint32_t d = WrapModPow2(Unzig(lanes[0][i]), 255u);
        std::array<uint8_t, 2> base = {128u, 128u};
        if (d != 0u) {
            base = (i >= d) ? hist[i - d] : pre[(256u + i - d) & 0xFFu];
        }
        const uint8_t a0 = static_cast<uint8_t>(
            WrapModPow2(static_cast<int32_t>(base[0]) + Unzig(lanes[2][i]), 255u));
        const uint8_t a1 = static_cast<uint8_t>(
            WrapModPow2(static_cast<int32_t>(base[1]) + Unzig(lanes[3][i]), 255u));
        hist[i] = {a0, a1};

        uint64_t bits = 0ull;
        for (uint32_t r = 0u; r < 4u; ++r) {
            const uint8_t* lp = kBc4Pair[lanes[4u + 2u * r][i] & 63u];
            const uint8_t* rp = kBc4Pair[lanes[4u + 2u * r + 1u][i] & 63u];
            const uint64_t row12 =
                static_cast<uint64_t>(lp[0]) |
                (static_cast<uint64_t>(lp[1]) << 3u) |
                (static_cast<uint64_t>(rp[0]) << 6u) |
                (static_cast<uint64_t>(rp[1]) << 9u);
            bits |= (row12 << (12u * r));
        }

        uint8_t* blk = out + i * 8u;
        blk[0] = a0;
        blk[1] = a1;
        StoreLe48(blk + 2u, bits);
    }
    return Status::Ok();
}

Status RebuildBc5Blocks(
    const std::array<std::vector<uint8_t>, kMaxLanes>& lanes,
    size_t nblocks,
    uint8_t* out) {
    for (uint8_t z : lanes[1]) {
        if (z != 0u) {
            return Status::Error(StatusCode::ParseError, "BC5 reserved lane 1 is non-zero");
        }
    }
    const auto& pre = GetS3tcRgtcHistory().bc5;
    std::vector<std::array<uint8_t, 4>> hist(nblocks);

    for (size_t i = 0u; i < nblocks; ++i) {
        const size_t a = 2u * i;
        const size_t b = 2u * i + 1u;
        const uint32_t d = WrapModPow2(Unzig(Inter8(lanes[0][a], lanes[0][b])), 255u);
        std::array<uint8_t, 4> base = {128u, 128u, 128u, 128u};
        if (d != 0u) {
            base = (i >= d) ? hist[i - d] : pre[(256u + i - d) & 0xFFu];
        }
        const std::array<uint8_t, 4> e = {
            static_cast<uint8_t>(WrapModPow2(static_cast<int32_t>(base[0]) + Unzig(lanes[2][a]), 255u)),
            static_cast<uint8_t>(WrapModPow2(static_cast<int32_t>(base[1]) + Unzig(lanes[2][b]), 255u)),
            static_cast<uint8_t>(WrapModPow2(static_cast<int32_t>(base[2]) + Unzig(lanes[3][a]), 255u)),
            static_cast<uint8_t>(WrapModPow2(static_cast<int32_t>(base[3]) + Unzig(lanes[3][b]), 255u)),
        };
        hist[i] = e;

        uint8_t* blk = out + i * 16u;
        for (uint32_t ch = 0u; ch < 2u; ++ch) {
            uint64_t bits = 0ull;
            for (uint32_t r = 0u; r < 4u; ++r) {
                const uint8_t* lp = kBc5Pair[lanes[4u + 4u * ch + r][a] & 63u];
                const uint8_t* rp = kBc5Pair[lanes[4u + 4u * ch + r][b] & 63u];
                const uint64_t row12 =
                    static_cast<uint64_t>(lp[0]) |
                    (static_cast<uint64_t>(lp[1]) << 3u) |
                    (static_cast<uint64_t>(rp[0]) << 6u) |
                    (static_cast<uint64_t>(rp[1]) << 9u);
                bits |= (row12 << (12u * r));
            }
            blk[8u * ch] = e[2u * ch];
            blk[8u * ch + 1u] = e[2u * ch + 1u];
            StoreLe48(blk + 8u * ch + 2u, bits);
        }
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Public BPTC (BC6H & BC7) Tables & Block Rebuilders
// (MIT `wxrrry/leonida-asset-pipeline/tools/{bcpack.py, bcpack_tables_bptc.py}`)
// ---------------------------------------------------------------------------
constexpr uint16_t kBptcPart2Packed[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80,
    0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
    0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce,
    0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
    0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a,
    0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c,
    0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
};

constexpr uint32_t kBptcPart3Packed[64] = {
    0xaa685050u, 0x6a5a5040u, 0x5a5a4200u, 0x5450a0a8u, 0xa5a50000u, 0xa0a05050u,
    0x5555a0a0u, 0x5a5a5050u, 0xaa550000u, 0xaa555500u, 0xaaaa5500u, 0x90909090u,
    0x94949494u, 0xa4a4a4a4u, 0xa9a59450u, 0x2a0a4250u, 0xa5945040u, 0x0a425054u,
    0xa5a5a500u, 0x55a0a0a0u, 0xa8a85454u, 0x6a6a4040u, 0xa4a45000u, 0x1a1a0500u,
    0x0050a4a4u, 0xaaa59090u, 0x14696914u, 0x69691400u, 0xa08585a0u, 0xaa821414u,
    0x50a4a450u, 0x6a5a0200u, 0xa9a58000u, 0x5090a0a8u, 0xa8a09050u, 0x24242424u,
    0x00aa5500u, 0x24924924u, 0x24499224u, 0x50a50a50u, 0x500aa550u, 0xaaaa4444u,
    0x66660000u, 0xa5a0a5a0u, 0x50a050a0u, 0x69286928u, 0x44aaaa44u, 0x66666600u,
    0xaa444444u, 0x54a854a8u, 0x95809580u, 0x96969600u, 0xa85454a8u, 0x80959580u,
    0xaa141414u, 0x96960000u, 0xaaaa1414u, 0xa05050a0u, 0xa0a5a5a0u, 0x96000000u,
    0x40804080u, 0xa9a8a9a8u, 0xaaaaaa44u, 0x2a4a5254u,
};

constexpr uint8_t kBptcAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15,  2,  8,  2,  2,  8,  8, 15,  2,  8,  2,  2,  8,  8,  2,  2,
    15, 15,  6,  8,  2,  8, 15, 15,  2,  8,  2,  2,  2, 15, 15,  6,
     6,  2,  6,  8, 15, 15,  2,  2, 15, 15, 15, 15, 15,  2,  2, 15,
};

constexpr uint8_t kBptcAnchor3A[64] = {
     3,  3, 15, 15,  8,  3, 15, 15,  8,  8,  6,  6,  6,  5,  3,  3,
     3,  3,  8, 15,  3,  3,  6, 10,  5,  8,  8,  6,  8,  5, 15, 15,
     8, 15,  3,  5,  6, 10,  8, 15, 15,  3, 15,  5, 15, 15, 15, 15,
     3, 15,  5,  5,  5,  8,  5, 10,  5, 10,  8, 13, 15, 12,  3,  3,
};

constexpr uint8_t kBptcAnchor3B[64] = {
    15,  8,  8,  3, 15, 15,  3,  8, 15, 15, 15, 15, 15, 15, 15,  8,
    15,  8, 15,  3, 15,  8, 15,  8,  3, 15,  6, 10, 15, 15, 10,  8,
    15,  3, 15, 10, 10,  8,  9, 10,  6, 15,  8, 15,  3,  6,  6,  8,
    15,  3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  3, 15, 15,  8,
};

// --- BC6H Plan & Mode Descriptors ---
struct Bc6hModeDesc {
    uint8_t code;
    uint8_t transformed;
    uint8_t base_bits;
    uint8_t deltas[3];
    uint8_t regions;
    const char* layout_tokens;
};

constexpr Bc6hModeDesc kBc6hModes[14] = {
    {0x00u, 1u, 10u, {5u, 5u, 5u}, 2u, "gy4 by4 bz4 rw10 gw10 bw10 rx5 gz4 gy4_ gx5 bz0 gz4_ bx5 bz1 by4_ ry5 bz2 rz5 bz3 d"},
    {0x01u, 1u,  7u, {6u, 6u, 6u}, 2u, "gy5 gz4 gz5 rw7 bz0 bz1 by4 gw7 by5 bz2 gy4 bw7 bz3 bz5 bz4 rx6 gy4_ gx6 gz4_ bx6 by4_ ry6 rz6 d"},
    {0x02u, 1u, 11u, {5u, 4u, 4u}, 2u, "rw10 gw10 bw10 rx5 rw:10 gy4_ gx4 gw:10 bz0 gz4_ bx4 bw:10 bz1 by4_ ry5 bz2 rz5 bz3 d"},
    {0x06u, 1u, 11u, {4u, 5u, 4u}, 2u, "rw10 gw10 bw10 rx4 rw:10 gz4 gy4_ gx5 gw:10 gz4_ bx4 bw:10 bz1 by4_ ry4 bz0 bz2 rz4 gy4 bz3 d"},
    {0x0Au, 1u, 11u, {4u, 4u, 5u}, 2u, "rw10 gw10 bw10 rx4 rw:10 by4 gy4_ gx4 gw:10 bz0 gz4_ bx5 bw:10 by4_ ry4 bz1 bz2 rz4 bz4 bz3 d"},
    {0x0Eu, 1u,  9u, {5u, 5u, 5u}, 2u, "rw9 by4 gw9 gy4 bw9 bz4 rx5 gz4 gy4_ gx5 bz0 gz4_ bx5 bz1 by4_ ry5 bz2 rz5 bz3 d"},
    {0x12u, 1u,  8u, {6u, 5u, 5u}, 2u, "rw8 gz4 by4 gw8 bz2 gy4 bw8 bz3 bz4 rx6 gy4_ gx5 bz0 gz4_ bx5 bz1 by4_ ry6 rz6 d"},
    {0x16u, 1u,  8u, {5u, 6u, 5u}, 2u, "rw8 bz0 by4 gw8 gy5 gy4 bw8 gz5 bz4 rx5 gz4 gy4_ gx6 gz4_ bx5 bz1 by4_ ry5 bz2 rz5 bz3 d"},
    {0x1Au, 1u,  8u, {5u, 5u, 6u}, 2u, "rw8 bz1 by4 gw8 by5 gy4 bw8 bz5 bz4 rx5 gz4 gy4_ gx5 bz0 gz4_ bx6 by4_ ry5 bz2 rz5 bz3 d"},
    {0x1Eu, 0u,  6u, {6u, 6u, 6u}, 2u, "rw6 gz4 bz0 bz1 by4 gw6 gy5 by5 bz2 gy4 bw6 gz5 bz3 bz5 bz4 rx6 gy4_ gx6 gz4_ bx6 by4_ ry6 rz6 d"},
    {0x03u, 0u, 10u, {10u, 10u, 10u}, 1u, "rw10 gw10 bw10 rx10 gx10 bx10"},
    {0x07u, 1u, 11u, {9u, 9u, 9u}, 1u, "rw10 gw10 bw10 rx9 rw:10 gx9 gw:10 bx9 bw:10"},
    {0x0Bu, 1u, 12u, {8u, 8u, 8u}, 1u, "rw10 gw10 bw10 rx8 rw:11-10 gx8 gw:11-10 bx8 bw:11-10"},
    {0x0Fu, 1u, 16u, {4u, 4u, 4u}, 1u, "rw10 gw10 bw10 rx4 rw:15-10 gx4 gw:15-10 bx4 bw:15-10"},
};

struct Bc6hPlanItem {
    int8_t channel;   // -1 for partition `d`, else 0..2 (R, G, B)
    int8_t slot;      // 0..3 (w, x, y, z)
    uint8_t bit_idx;
    uint8_t stream_pos;
};

struct Bc6hCompiledPlan {
    uint32_t count = 0u;
    uint32_t header_bits = 0u;
    std::array<Bc6hPlanItem, 84> items{};
};

const std::array<Bc6hCompiledPlan, 14>& GetBc6hPlans() noexcept {
    static const std::array<Bc6hCompiledPlan, 14> kPlans = []() {
        std::array<Bc6hCompiledPlan, 14> all{};
        auto parse_uint = [](const char* s, size_t len) -> uint32_t {
            uint32_t v = 0u;
            for (size_t i = 0u; i < len; ++i) {
                v = v * 10u + static_cast<uint32_t>(s[i] - '0');
            }
            return v;
        };
        auto map_field = [](char c0, char c1, int8_t& ch, int8_t& sl) {
            ch = (c0 == 'r') ? 0 : ((c0 == 'g') ? 1 : 2);
            sl = (c1 == 'w') ? 0 : ((c1 == 'x') ? 1 : ((c1 == 'y') ? 2 : 3));
        };

        for (size_t m = 0u; m < 14u; ++m) {
            Bc6hCompiledPlan& plan = all[m];
            uint32_t pos = (kBc6hModes[m].code < 2u) ? 2u : 5u;
            const char* p = kBc6hModes[m].layout_tokens;
            while (*p != '\0') {
                while (*p == ' ') ++p;
                if (*p == '\0') break;
                const char* start = p;
                while (*p != '\0' && *p != ' ') ++p;
                const size_t tlen = static_cast<size_t>(p - start);

                if (tlen == 1u && start[0] == 'd') {
                    for (uint32_t b = 0u; b < 5u; ++b) {
                        plan.items[plan.count++] = {-1, -1, static_cast<uint8_t>(b), static_cast<uint8_t>(pos + b)};
                    }
                    pos += 5u;
                } else {
                    const char* colon = nullptr;
                    for (size_t k = 0u; k < tlen; ++k) {
                        if (start[k] == ':') { colon = start + k; break; }
                    }
                    if (colon != nullptr) {
                        int8_t ch = 0, sl = 0;
                        map_field(start[0], start[1], ch, sl);
                        const char* span = colon + 1;
                        const size_t slen = static_cast<size_t>((start + tlen) - span);
                        const char* dash = nullptr;
                        for (size_t k = 0u; k < slen; ++k) {
                            if (span[k] == '-') { dash = span + k; break; }
                        }
                        uint32_t hi = 0u, lo = 0u;
                        if (dash != nullptr) {
                            hi = parse_uint(span, static_cast<size_t>(dash - span));
                            lo = parse_uint(dash + 1, static_cast<size_t>((span + slen) - (dash + 1)));
                        } else {
                            hi = parse_uint(span, slen);
                            lo = hi;
                        }
                        for (int32_t b = static_cast<int32_t>(hi); b >= static_cast<int32_t>(lo); --b) {
                            plan.items[plan.count++] = {
                                ch, sl, static_cast<uint8_t>(b), static_cast<uint8_t>(pos++)};
                        }
                    } else if (start[tlen - 1u] == '_') {
                        int8_t ch = 0, sl = 0;
                        map_field(start[0], start[1], ch, sl);
                        const uint32_t n = parse_uint(start + 2, tlen - 3u);
                        for (uint32_t b = 0u; b < n; ++b) {
                            plan.items[plan.count++] = {
                                ch, sl, static_cast<uint8_t>(b), static_cast<uint8_t>(pos + b)};
                        }
                        pos += n;
                    } else {
                        int8_t ch = 0, sl = 0;
                        map_field(start[0], start[1], ch, sl);
                        const uint32_t n = parse_uint(start + 2, tlen - 2u);
                        const bool is_run = (start[1] == 'w' || start[1] == 'x' ||
                                             (start[0] == 'r' && (start[1] == 'y' || start[1] == 'z')));
                        if (is_run) {
                            for (uint32_t b = 0u; b < n; ++b) {
                                plan.items[plan.count++] = {
                                    ch, sl, static_cast<uint8_t>(b), static_cast<uint8_t>(pos + b)};
                            }
                            pos += n;
                        } else {
                            plan.items[plan.count++] = {
                                ch, sl, static_cast<uint8_t>(n), static_cast<uint8_t>(pos++)};
                        }
                    }
                }
            }
            plan.header_bits = pos;
        }
        return all;
    }();
    return kPlans;
}

inline uint32_t Bc6hStreamW(uint32_t mode1Based, uint32_t w) noexcept {
    if (mode1Based == 13u) {
        return (w & 0x3FFu) | (((w >> 10u) & 1u) << 11u) | (((w >> 11u) & 1u) << 10u);
    }
    if (mode1Based == 14u) {
        uint32_t out = w & 0x3FFu;
        for (uint32_t b = 10u; b < 16u; ++b) {
            out |= ((w >> b) & 1u) << (25u - b);
        }
        return out;
    }
    return w;
}

Status RebuildBc6hBlocks(
    const std::array<std::vector<uint8_t>, kMaxLanes>& lanes,
    size_t nblocks,
    uint8_t* out) {
    using EpState = std::array<std::array<uint32_t, 4>, 3>;
    using IxState = std::array<uint8_t, 16>;

    constexpr EpState kNoneE = {{
        {{0x8000u, 0x200u, 0x20u, 0x20u}},
        {{0x8000u, 0x200u, 0x20u, 0x20u}},
        {{0x8000u, 0x200u, 0x20u, 0x20u}},
    }};
    constexpr EpState kZeroFallbackE{};
    constexpr IxState kNoneI = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    constexpr IxState kZeroFallbackI{};

    const auto& plans = GetBc6hPlans();
    std::vector<EpState> st_e(nblocks);
    std::vector<IxState> st_i(nblocks);
    std::vector<bool> i_from_fallback(nblocks, false);

    for (size_t i = 0u; i < nblocks; ++i) {
        const size_t a = 2u * i;
        const size_t b = 2u * i + 1u;
        const uint8_t m_sym = lanes[15][a];
        if (m_sym > 13u) {
            return Status::Error(StatusCode::ParseError, "BC6H mode symbol outside 0..13");
        }
        const Bc6hModeDesc& mdesc = kBc6hModes[m_sym];
        const uint32_t mode1 = static_cast<uint32_t>(m_sym) + 1u;
        const uint32_t part = std::min<uint32_t>(lanes[15][b], 31u);

        const uint32_t di = Inter8(lanes[1][a], lanes[1][b]);
        const uint32_t de = WrapModPow2(
            static_cast<int32_t>(di) + Unzig(Inter8(lanes[0][a], lanes[0][b])), 255u);

        EpState eref = kNoneE;
        if (de != 0u) {
            eref = (i >= de) ? st_e[i - de] : kZeroFallbackE;
        }

        IxState iref = kNoneI;
        bool idx_fallback = false;
        if (di != 0u) {
            if (i >= di) {
                iref = st_i[i - di];
                idx_fallback = i_from_fallback[i - di];
            } else {
                iref = kZeroFallbackI;
                idx_fallback = true;
            }
        }
        i_from_fallback[i] = idx_fallback;

        const uint32_t base = mdesc.base_bits;
        const uint32_t mw = (1u << base) - 1u;
        const uint32_t w_shift = 16u - base;
        std::array<std::array<uint32_t, 4>, 3> f_block{};
        EpState st_block{};

        for (uint32_t c = 0u; c < 3u; ++c) {
            const uint32_t dc = mdesc.deltas[c];
            const uint32_t mD = (1u << dc) - 1u;
            const uint32_t sh = (dc < 6u) ? (6u - dc) : 0u;
            const int32_t dz0 = Unzig(InterleaveBits(lanes[2u + c][a], lanes[2u + c][b], 8u));
            const int32_t dz1 = Unzig(InterleaveBits(lanes[5u + c][a], lanes[5u + c][b], 5u));
            const int32_t dz2 = Unzig(lanes[8u + c][a]);
            const int32_t dz3 = Unzig(lanes[8u + c][b]);

            const uint32_t w = WrapModPow2(static_cast<int32_t>(eref[c][0] >> w_shift) + dz0, mw);
            const uint32_t x = WrapModPow2(static_cast<int32_t>(eref[c][1] >> (10u - dc)) + dz1, mD);
            const uint32_t y = WrapModPow2(static_cast<int32_t>(eref[c][2] >> sh) + dz2, mD);
            const uint32_t z = WrapModPow2(static_cast<int32_t>(eref[c][3] >> sh) + dz3, mD);
            const uint32_t x10 = x << (10u - dc);

            if (mdesc.regions == 2u) {
                st_block[c] = {{w << w_shift, x10, y << sh, z << sh}};
                f_block[c] = {{Bc6hStreamW(mode1, w), x, y, z}};
            } else {
                const uint32_t v = (w & mD) ^ ((mdesc.transformed != 0u) ? (1u << (dc - 1u)) : 0u);
                const uint32_t low_y = (dc >= 6u) ? (v >> (dc - 6u)) : (v << sh);
                st_block[c] = {{w << w_shift, x10, low_y, x10 >> 4u}};
                f_block[c] = {{Bc6hStreamW(mode1, w), x, 0u, 0u}};
            }
        }
        st_e[i] = st_block;

        const uint32_t ibits = (mdesc.regions == 2u) ? 3u : 4u;
        const uint32_t imask = (1u << ibits) - 1u;
        const uint32_t ishift = 4u - ibits;
        uint8_t ix_block[16]{};
        IxState si_block{};

        for (uint32_t yy = 0u; yy < 4u; ++yy) {
            const uint8_t sa = lanes[11u + yy][a];
            const uint8_t sb = lanes[11u + yy][b];
            const uint8_t d0 = (yy < 3u) ? kDeint.odd[sa] : kDeint.even[sa];
            const uint8_t d1 = (yy < 3u) ? kDeint.even[sa] : kDeint.odd[sa];
            const uint8_t d2 = (yy < 3u) ? kDeint.odd[sb] : kDeint.even[sb];
            const uint8_t d3 = (yy < 3u) ? kDeint.even[sb] : kDeint.odd[sb];
            const uint8_t row_d[4] = {d0, d1, d2, d3};
            for (uint32_t xx = 0u; xx < 4u; ++xx) {
                const uint32_t p = yy * 4u + xx;
                const uint32_t ix = WrapModPow2(
                    static_cast<int32_t>(iref[p] >> ishift) + Unzig(row_d[xx]), imask);
                ix_block[p] = static_cast<uint8_t>(ix);
                si_block[p] = static_cast<uint8_t>(ix << ishift);
            }
        }
        st_i[i] = si_block;

        const int32_t second = (mdesc.regions == 2u) ? static_cast<int32_t>(kBptcAnchor2[part]) : -1;
        if (!idx_fallback) {
            const uint8_t anchor_msb = (mdesc.regions == 2u) ? 4u : 8u;
            if ((ix_block[0] & anchor_msb) != 0u ||
                (second >= 0 && (ix_block[static_cast<size_t>(second)] & 4u) != 0u)) {
                return Status::Error(
                    StatusCode::ParseError, "BC6H anchor pixel index MSB violation");
            }
        }

        const Bc6hCompiledPlan& plan = plans[m_sym];
        BitAccum128 acc{};
        acc.lo = mdesc.code;
        for (uint32_t k = 0u; k < plan.count; ++k) {
            const Bc6hPlanItem& it = plan.items[k];
            if (it.channel < 0) {
                acc.OrBit(it.stream_pos, (part >> it.bit_idx) & 1u);
            } else {
                const uint32_t fv =
                    f_block[static_cast<size_t>(it.channel)][static_cast<size_t>(it.slot)];
                acc.OrBit(it.stream_pos, (fv >> it.bit_idx) & 1u);
            }
        }

        uint32_t at = plan.header_bits;
        for (uint32_t p = 0u; p < 16u; ++p) {
            const bool is_anchor = (p == 0u) || (second >= 0 && p == static_cast<uint32_t>(second));
            const uint32_t nb = is_anchor ? (ibits - 1u) : ibits;
            acc.OrBits(at, ix_block[p] & ((1u << nb) - 1u), nb);
            at += nb;
        }
        acc.StoreLe(out + i * 16u);
    }
    return Status::Ok();
}

// --- BC7 Signature Table & Rebuilder ---
struct Bc7SignatureConfig {
    uint8_t mode;
    uint8_t part;
    uint8_t rot;
    uint8_t isel;
};

struct Bc7StaticTables {
    std::array<Bc7SignatureConfig, 286> config{};
    std::array<int16_t, 512> rank_of{};
    std::array<std::array<uint8_t, 16>, 64> part2{};
    std::array<std::array<uint8_t, 16>, 64> part3{};
};

const Bc7StaticTables& GetBc7StaticTables() noexcept {
    static const Bc7StaticTables kTbl = []() {
        Bc7StaticTables t{};
        t.rank_of.fill(-1);

        constexpr uint8_t kMode2Head[22] = {
            63, 59, 58, 57, 54, 53, 52, 47, 43, 42, 40,
            38, 37, 35, 34, 32, 23, 21, 18, 17, 16, 11,
        };
        std::array<bool, 64> in_head{};
        for (uint8_t p : kMode2Head) {
            in_head[p] = true;
        }

        size_t idx = 0u;
        for (uint32_t r = 0u; r < 64u; ++r) {
            t.config[idx++] = {7u, static_cast<uint8_t>(63u - r), 0u, 0u};
        }
        t.config[idx++] = {6u, 0u, 0u, 0u};
        for (uint32_t r = 0u; r < 4u; ++r) {
            t.config[idx++] = {5u, 0u, static_cast<uint8_t>(3u - r), 0u};
        }
        for (uint32_t r = 0u; r < 8u; ++r) {
            const uint32_t v = 7u - r;
            t.config[idx++] = {4u, 0u, static_cast<uint8_t>(v >> 1u), static_cast<uint8_t>(v & 1u)};
        }
        for (uint32_t r = 0u; r < 64u; ++r) {
            t.config[idx++] = {3u, static_cast<uint8_t>(63u - r), 0u, 0u};
        }
        for (uint8_t p : kMode2Head) {
            t.config[idx++] = {2u, p, 0u, 0u};
        }
        for (uint32_t r = 0u; r < 64u; ++r) {
            t.config[idx++] = {1u, static_cast<uint8_t>(63u - r), 0u, 0u};
        }
        for (int32_t p = 15; p >= 8; --p) {
            t.config[idx++] = {0u, static_cast<uint8_t>(p), 0u, 0u};
        }
        for (int32_t p = 5; p >= 0; --p) {
            t.config[idx++] = {0u, static_cast<uint8_t>(p), 0u, 0u};
        }
        for (int32_t p = 63; p >= 0; --p) {
            if (!in_head[static_cast<size_t>(p)]) {
                t.config[idx++] = {2u, static_cast<uint8_t>(p), 0u, 0u};
            }
        }
        t.config[idx++] = {0u, 7u, 0u, 0u};
        t.config[idx++] = {0u, 6u, 0u, 0u};
        t.config[idx++] = {255u, 255u, 0u, 0u};

        struct Cell {
            uint16_t prod;
            uint8_t a;
            uint8_t b;
        };
        std::array<Cell, 512> cells{};
        size_t ci = 0u;
        for (uint32_t a = 0u; a < 32u; ++a) {
            for (uint32_t b = 0u; b < 16u; ++b) {
                cells[ci++] = {
                    static_cast<uint16_t>((a + 1u) * (b + 1u)),
                    static_cast<uint8_t>(a),
                    static_cast<uint8_t>(b)};
            }
        }
        std::sort(cells.begin(), cells.end(), [](const Cell& x, const Cell& y) {
            return (x.prod != y.prod) ? (x.prod < y.prod) : (x.a < y.a);
        });
        for (size_t rank = 0u; rank < 285u; ++rank) {
            const Cell& c = cells[rank];
            t.rank_of[static_cast<size_t>(c.a) * 16u + c.b] = static_cast<int16_t>(rank);
        }

        for (size_t part = 0u; part < 64u; ++part) {
            for (uint32_t p = 0u; p < 16u; ++p) {
                t.part2[part][p] = static_cast<uint8_t>((kBptcPart2Packed[part] >> p) & 1u);
                t.part3[part][p] = static_cast<uint8_t>((kBptcPart3Packed[part] >> (2u * p)) & 3u);
            }
        }
        return t;
    }();
    return kTbl;
}

Status RebuildBc7Blocks(
    const std::array<std::vector<uint8_t>, kMaxLanes>& lanes,
    size_t nblocks,
    uint8_t* out) {
    constexpr uint8_t kB7Ns[8] = {3, 2, 3, 2, 1, 1, 1, 2};
    constexpr uint8_t kB7Cb[8] = {4, 6, 5, 7, 5, 7, 7, 5};
    constexpr uint8_t kB7Ab[8] = {0, 0, 0, 0, 6, 8, 7, 5};
    constexpr uint8_t kB7Pk[8] = {1, 2, 0, 1, 0, 0, 1, 1};
    constexpr uint8_t kB7Ib[8] = {3, 3, 2, 2, 3, 2, 4, 2};
    constexpr uint8_t kB7Pb[8] = {4, 6, 6, 6, 0, 0, 0, 6};
    constexpr int8_t kB7Lane[3][4] = {{2, 3, 4, 5}, {6, 7, 8, 9}, {10, 5, 9, -1}};
    constexpr uint32_t kSecondaryRowLanes[4] = {6u, 7u, 8u, 10u};

    using EpRow = std::array<uint8_t, 24>;
    using IxRow = std::array<uint8_t, 32>;
    constexpr EpRow kZeroE{};
    constexpr IxRow kZeroI{};

    const Bc7StaticTables& tbl = GetBc7StaticTables();
    std::vector<EpRow> eps(nblocks);
    std::vector<IxRow> ixs(nblocks);
    std::vector<bool> ep_from_fallback(nblocks, false);
    std::vector<bool> ix_from_fallback(nblocks, false);

    for (size_t i = 0u; i < nblocks; ++i) {
        const size_t a = 2u * i;
        const size_t b = 2u * i + 1u;
        const uint8_t s15a = lanes[15][a];
        const uint8_t s15b = lanes[15][b];
        const int16_t rank = (s15a < 32u && s15b < 16u)
                                 ? tbl.rank_of[static_cast<size_t>(s15a) * 16u + s15b]
                                 : static_cast<int16_t>(-1);
        if (rank < 0 || rank >= 285) {
            return Status::Error(StatusCode::ParseError, "BC7 signature pair outside valid table");
        }
        const Bc7SignatureConfig cfg = tbl.config[static_cast<size_t>(rank)];
        const uint32_t mode = cfg.mode;
        const uint32_t part = cfg.part;
        const uint32_t rot = cfg.rot;
        const uint32_t isel = cfg.isel;

        const uint32_t di = Inter8(lanes[1][a], lanes[1][b]);
        const uint32_t de = WrapModPow2(
            static_cast<int32_t>(di) + Unzig(Inter8(lanes[0][a], lanes[0][b])), 255u);

        EpRow rep = kZeroE;
        bool ep_fallback = false;
        if (de != 0u) {
            if (i >= de) {
                rep = eps[i - de];
                ep_fallback = ep_from_fallback[i - de];
            } else {
                rep = kZeroE;
                ep_fallback = true;
            }
        }
        ep_from_fallback[i] = ep_fallback;

        IxRow rix = kZeroI;
        bool ix_fallback = false;
        if (di != 0u) {
            if (i >= di) {
                rix = ixs[i - di];
                ix_fallback = ix_from_fallback[i - di];
            } else {
                rix = kZeroI;
                ix_fallback = true;
            }
        }
        ix_from_fallback[i] = ix_fallback;

        const uint32_t ns = kB7Ns[mode];
        const uint32_t cb = kB7Cb[mode];
        const uint32_t ab = kB7Ab[mode];
        const uint32_t pk = kB7Pk[mode];
        uint32_t cw[4] = {cb, cb, cb, ab};
        if (rot != 0u) {
            cw[rot - 1u] = ab;
            cw[3] = cb;
        }

        uint32_t val[3][2][4]{};
        EpRow sep{};
        uint32_t pb[3][2]{};

        for (uint32_t sub = 0u; sub < ns; ++sub) {
            const uint32_t base_sub = sub * 8u;
            for (uint32_t c = 0u; c < 4u; ++c) {
                const int8_t lane_idx = kB7Lane[sub][c];
                const uint32_t wc = cw[c];
                if (wc == 0u || lane_idx < 0) {
                    continue;
                }
                const uint32_t bits = wc + ((pk != 0u && c == 0u) ? 1u : 0u);
                const uint32_t shift = 8u - bits;
                const uint32_t mask = (1u << bits) - 1u;
                const auto& ln = lanes[static_cast<size_t>(lane_idx)];
                const uint32_t v0 = WrapModPow2(
                    static_cast<int32_t>(rep[base_sub + c] >> shift) + Unzig(ln[a]), mask);
                const uint32_t v1 = WrapModPow2(
                    static_cast<int32_t>(rep[base_sub + 4u + c] >> shift) + Unzig(ln[b]), mask);
                val[sub][0][c] = v0;
                val[sub][1][c] = v1;
                sep[base_sub + c] = static_cast<uint8_t>(v0 << shift);
                sep[base_sub + 4u + c] = static_cast<uint8_t>(v1 << shift);
            }
            if (pk == 1u) {
                pb[sub][0] = val[sub][0][0] & 1u;
                pb[sub][1] = val[sub][1][0] & 1u;
            } else if (pk == 2u) {
                const uint32_t p0_bit = val[sub][0][0] & 1u;
                pb[sub][0] = p0_bit;
                pb[sub][1] = p0_bit;
                if (!ep_fallback && ((val[sub][1][0] & 1u) != p0_bit)) {
                    return Status::Error(
                        StatusCode::ParseError, "BC7 Mode 1 shared p-bit consistency mismatch");
                }
            }
        }
        for (uint32_t sub = ns; sub < 3u; ++sub) {
            for (uint32_t k = 0u; k < 8u; ++k) {
                sep[sub * 8u + k] = sep[k];
            }
        }
        eps[i] = sep;

        const uint32_t b1 = kB7Ib[mode];
        const uint32_t b1_shift = 4u - b1;
        const uint32_t b1_mask = (1u << b1) - 1u;
        const bool two_idx = (mode == 4u || mode == 5u);
        uint8_t s1[16]{};
        uint8_t s2[16]{};
        IxRow six{};

        for (uint32_t y = 0u; y < 4u; ++y) {
            const uint8_t sa = lanes[11u + y][a];
            const uint8_t sb = lanes[11u + y][b];
            const uint8_t d0 = (y < 3u) ? kDeint.odd[sa] : kDeint.even[sa];
            const uint8_t d1 = (y < 3u) ? kDeint.even[sa] : kDeint.odd[sa];
            const uint8_t d2 = (y < 3u) ? kDeint.odd[sb] : kDeint.even[sb];
            const uint8_t d3 = (y < 3u) ? kDeint.even[sb] : kDeint.odd[sb];
            const uint8_t row_d[4] = {d0, d1, d2, d3};
            for (uint32_t x = 0u; x < 4u; ++x) {
                const uint32_t p = y * 4u + x;
                const uint32_t iv = WrapModPow2(
                    static_cast<int32_t>(rix[p] >> b1_shift) + Unzig(row_d[x]), b1_mask);
                s1[p] = static_cast<uint8_t>(iv);
                six[p] = static_cast<uint8_t>(iv << b1_shift);
            }
            if (two_idx) {
                const uint32_t l2 = kSecondaryRowLanes[y];
                const uint8_t s2a = lanes[l2][a];
                const uint8_t s2b = lanes[l2][b];
                const uint8_t e0 = (y < 3u) ? kDeint.odd[s2a] : kDeint.even[s2a];
                const uint8_t e1 = (y < 3u) ? kDeint.even[s2a] : kDeint.odd[s2a];
                const uint8_t e2 = (y < 3u) ? kDeint.odd[s2b] : kDeint.even[s2b];
                const uint8_t e3 = (y < 3u) ? kDeint.even[s2b] : kDeint.odd[s2b];
                const uint8_t row_e[4] = {e0, e1, e2, e3};
                for (uint32_t x = 0u; x < 4u; ++x) {
                    const uint32_t p = y * 4u + x;
                    const uint32_t iv2 = WrapModPow2(
                        static_cast<int32_t>(rix[16u + p] >> 2u) + Unzig(row_e[x]), 3u);
                    s2[p] = static_cast<uint8_t>(iv2);
                    six[16u + p] = static_cast<uint8_t>(iv2 << 2u);
                }
            }
        }
        ixs[i] = six;

        // Reserved all-zero BC7 block (encoded under Mode 0, part 0 with all-zero endpoints/indices).
        if (mode == 0u && part == 0u) {
            bool any_nz = false;
            for (uint8_t v : sep) { if (v != 0u) { any_nz = true; break; } }
            if (!any_nz) {
                for (uint8_t v : s1) { if (v != 0u) { any_nz = true; break; } }
            }
            if (!any_nz) {
                std::memset(out + i * 16u, 0, 16u);
                continue;
            }
        }

        const uint8_t* pri = s1;
        uint32_t ib = b1;
        const uint8_t* sec = nullptr;
        uint32_t ib2 = 0u;
        if (mode == 4u) {
            pri = s2;
            ib = 2u;
            sec = s1;
            ib2 = 3u;
        } else if (mode == 5u) {
            pri = s1;
            ib = 2u;
            sec = s2;
            ib2 = 2u;
        }

        BitAccum128 acc{};
        acc.OrBit(mode, 1u);
        uint32_t pos = mode + 1u;

        const uint32_t pbits = kB7Pb[mode];
        if (pbits > 0u) {
            acc.OrBits(pos, part & ((1u << pbits) - 1u), pbits);
            pos += pbits;
        }
        if (mode == 4u || mode == 5u) {
            acc.OrBits(pos, rot & 3u, 2u);
            pos += 2u;
        }
        if (mode == 4u) {
            acc.OrBits(pos, isel & 1u, 1u);
            pos += 1u;
        }

        const uint32_t cb_mask = (1u << cb) - 1u;
        for (uint32_t c = 0u; c < 3u; ++c) {
            const uint32_t tc = (rot != 0u && c == rot - 1u) ? 3u : c;
            const bool drop = (pk != 0u && tc == 0u);
            for (uint32_t sub = 0u; sub < ns; ++sub) {
                for (uint32_t e = 0u; e < 2u; ++e) {
                    const uint32_t v = val[sub][e][tc];
                    acc.OrBits(pos, (drop ? (v >> 1u) : v) & cb_mask, cb);
                    pos += cb;
                }
            }
        }
        if (ab > 0u) {
            const uint32_t ab_mask = (1u << ab) - 1u;
            const uint32_t tc = (rot != 0u) ? (rot - 1u) : 3u;
            const bool drop = (pk != 0u && tc == 0u);
            for (uint32_t sub = 0u; sub < ns; ++sub) {
                for (uint32_t e = 0u; e < 2u; ++e) {
                    const uint32_t v = val[sub][e][tc];
                    acc.OrBits(pos, (drop ? (v >> 1u) : v) & ab_mask, ab);
                    pos += ab;
                }
            }
        }
        if (pk == 1u) {
            for (uint32_t sub = 0u; sub < ns; ++sub) {
                acc.OrBit(pos++, pb[sub][0]);
                acc.OrBit(pos++, pb[sub][1]);
            }
        } else if (pk == 2u) {
            for (uint32_t sub = 0u; sub < ns; ++sub) {
                acc.OrBit(pos++, pb[sub][0]);
            }
        }

        uint8_t anchors[3] = {0u, 0u, 0u};
        if (ns == 2u) {
            anchors[1] = kBptcAnchor2[part];
        } else if (ns == 3u) {
            anchors[1] = kBptcAnchor3A[part];
            anchors[2] = kBptcAnchor3B[part];
        }

        const uint32_t ib_mask = (1u << ib) - 1u;
        const uint32_t ib_amask = (1u << (ib - 1u)) - 1u;
        for (uint32_t p = 0u; p < 16u; ++p) {
            uint32_t sub_idx = 0u;
            if (ns == 2u) {
                sub_idx = tbl.part2[part][p];
            } else if (ns == 3u) {
                sub_idx = tbl.part3[part][p];
            }
            const bool is_anchor = (p == anchors[sub_idx]);
            const uint32_t pv = pri[p];
            if (is_anchor) {
                if (!ix_fallback && (pv >> (ib - 1u)) != 0u) {
                    return Status::Error(
                        StatusCode::ParseError, "BC7 primary anchor index MSB violation");
                }
                acc.OrBits(pos, pv & ib_amask, ib - 1u);
                pos += (ib - 1u);
            } else {
                acc.OrBits(pos, pv & ib_mask, ib);
                pos += ib;
            }
        }
        if (sec != nullptr) {
            const uint32_t ib2_mask = (1u << ib2) - 1u;
            const uint32_t ib2_amask = (1u << (ib2 - 1u)) - 1u;
            for (uint32_t p = 0u; p < 16u; ++p) {
                const uint32_t sv = sec[p];
                if (p == 0u) {
                    if (!ix_fallback && (sv >> (ib2 - 1u)) != 0u) {
                        return Status::Error(
                            StatusCode::ParseError, "BC7 secondary anchor index MSB violation");
                    }
                    acc.OrBits(pos, sv & ib2_amask, ib2 - 1u);
                    pos += (ib2 - 1u);
                } else {
                    acc.OrBits(pos, sv & ib2_mask, ib2);
                    pos += ib2;
                }
            }
        }
        if (pos != 128u) {
            return Status::Error(StatusCode::ParseError, "BC7 packed block bit length != 128");
        }
        acc.StoreLe(out + i * 16u);
    }
    return Status::Ok();
}

}  // namespace

Status BcPackDecodeXboxMduBitstream(
    BcFormat format,
    const uint8_t* src_bitstream,
    size_t src_size,
    uint8_t* dst_blocks,
    size_t dst_size,
    size_t* out_written) {
    if (out_written) {
        *out_written = 0u;
    }

    CodecLaneSpec spec{};
    if (!GetCodecLaneSpec(format, spec)) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Unsupported BC format for Xbox hardware MDU BCPack bitstream");
    }

    if (!dst_blocks || dst_size == 0u || (dst_size % spec.block_bytes) != 0u) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "Invalid destination buffer or size not aligned to BCn block size");
    }

    if (!src_bitstream || src_size == 0u) {
        return Status::Error(StatusCode::ParseError, "Empty or null Xbox MDU BCPack bitstream");
    }

    const size_t nsym = dst_size / kCodingUnitBytes;
    const size_t nblocks = dst_size / spec.block_bytes;

    std::array<std::vector<uint8_t>, kMaxLanes> lanes{};
    Status st = DecodeMduLanes(src_bitstream, src_size, spec, nsym, lanes);
    if (!st.ok()) {
        return st;
    }

    // Stage reconstructed blocks so `dst_blocks` is never partially mutated if
    // block-level validation (e.g. reserved lane 1, invalid BC6H/BC7 mode/anchor) fails.
    std::vector<uint8_t> staged(dst_size, 0u);
    switch (format) {
        case BcFormat::BC1:
            st = RebuildBc1Blocks(lanes, nblocks, staged.data());
            break;
        case BcFormat::BC3:
            st = RebuildBc3Blocks(lanes, nblocks, staged.data());
            break;
        case BcFormat::BC4:
            st = RebuildBc4Blocks(lanes, nblocks, staged.data());
            break;
        case BcFormat::BC5:
            st = RebuildBc5Blocks(lanes, nblocks, staged.data());
            break;
        case BcFormat::BC6H:
            st = RebuildBc6hBlocks(lanes, nblocks, staged.data());
            break;
        case BcFormat::BC7:
            st = RebuildBc7Blocks(lanes, nblocks, staged.data());
            break;
        default:
            return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BC format");
    }
    if (!st.ok()) {
        return st;
    }

    std::memcpy(dst_blocks, staged.data(), dst_size);
    if (out_written) {
        *out_written = dst_size;
    }
    return Status::Ok();
}

}  // namespace openscarlett
