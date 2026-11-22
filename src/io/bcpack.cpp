#include "openscarlett/io/bcpack.h"
#include "openscarlett/kernel/vram_residency_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <queue>
#include <utility>

namespace openscarlett {

namespace {

constexpr size_t kHeaderFixedBytes = 36u;
constexpr size_t kPlaneDescBytes = 20u;
constexpr size_t kMipChainHeaderFixedBytes = 24u;
constexpr size_t kMipChainEntryBytes = 8u;
constexpr uint32_t kMaxHuffBits = 15u;

uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

void WriteLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void AppendLe16(std::vector<uint8_t>& dst, uint16_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

void AppendLe32(std::vector<uint8_t>& dst, uint32_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
}

const std::array<uint32_t, 256>& GetCrc32Table() noexcept {
    static const std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

bool IsValidPlaneTransform(BcPlaneTransform tr) noexcept {
    switch (tr) {
        case BcPlaneTransform::None:
        case BcPlaneTransform::Delta8:
        case BcPlaneTransform::Xor8:
        case BcPlaneTransform::Delta16:
        case BcPlaneTransform::Rgb565Delta:
        case BcPlaneTransform::TransposeDelta8:
            return true;
        default:
            return false;
    }
}

bool IsValidPlaneCodec(BcPlaneCodec codec) noexcept {
    switch (codec) {
        case BcPlaneCodec::Raw:
        case BcPlaneCodec::LzRle:
        case BcPlaneCodec::CanonicalHuffman:
        case BcPlaneCodec::LzRleHuffman:
        case BcPlaneCodec::EndpointRingWindow:
        case BcPlaneCodec::SelectorIndexTable:
            return true;
        default:
            return false;
    }
}

struct PlaneSpec {
    BcPlaneKind kind;
    uint8_t block_offset;
    uint8_t element_bytes;
    bool is_rgb565;
    bool is_selector;
};

std::vector<PlaneSpec> GetFormatPlanes(BcFormat format) {
    switch (format) {
        case BcFormat::BC1:
            return {
                {BcPlaneKind::Color0, 0, 2, true, false},
                {BcPlaneKind::Color1, 2, 2, true, false},
                {BcPlaneKind::ColorIndices, 4, 4, false, true},
            };
        case BcFormat::BC2:
            return {
                {BcPlaneKind::AlphaExplicit, 0, 8, false, true},
                {BcPlaneKind::Color0, 8, 2, true, false},
                {BcPlaneKind::Color1, 10, 2, true, false},
                {BcPlaneKind::ColorIndices, 12, 4, false, true},
            };
        case BcFormat::BC3:
            return {
                {BcPlaneKind::Alpha0, 0, 1, false, false},
                {BcPlaneKind::Alpha1, 1, 1, false, false},
                {BcPlaneKind::AlphaIndices, 2, 6, false, true},
                {BcPlaneKind::Color0, 8, 2, true, false},
                {BcPlaneKind::Color1, 10, 2, true, false},
                {BcPlaneKind::ColorIndices, 12, 4, false, true},
            };
        case BcFormat::BC4:
            return {
                {BcPlaneKind::Red0, 0, 1, false, false},
                {BcPlaneKind::Red1, 1, 1, false, false},
                {BcPlaneKind::RedIndices, 2, 6, false, true},
            };
        case BcFormat::BC5:
            return {
                {BcPlaneKind::Red0, 0, 1, false, false},
                {BcPlaneKind::Red1, 1, 1, false, false},
                {BcPlaneKind::RedIndices, 2, 6, false, true},
                {BcPlaneKind::Green0, 8, 1, false, false},
                {BcPlaneKind::Green1, 9, 1, false, false},
                {BcPlaneKind::GreenIndices, 10, 6, false, true},
            };
        case BcFormat::BC6H:
        case BcFormat::BC7:
            return {
                {BcPlaneKind::ModeHeader, 0, 2, false, true},
                {BcPlaneKind::Endpoints, 2, 6, false, false},
                {BcPlaneKind::SecondaryEndpoints, 8, 4, false, false},
                {BcPlaneKind::PrimaryIndices, 12, 4, false, true},
            };
        default:
            return {};
    }
}

bool TransformCompatibleWithPlane(BcPlaneTransform tr, uint8_t element_bytes, size_t raw_size) noexcept {
    switch (tr) {
        case BcPlaneTransform::None:
        case BcPlaneTransform::Delta8:
        case BcPlaneTransform::Xor8:
            return true;
        case BcPlaneTransform::Delta16:
        case BcPlaneTransform::Rgb565Delta:
            return (raw_size % 2u) == 0u;
        case BcPlaneTransform::TransposeDelta8:
            return element_bytes > 0u && (raw_size % element_bytes) == 0u;
        default:
            return false;
    }
}

void ApplyTransform(
    const std::vector<uint8_t>& in,
    uint8_t elem_bytes,
    BcPlaneTransform transform,
    std::vector<uint8_t>& out) {
    const size_t n = in.size();
    out.resize(n);
    if (n == 0 || transform == BcPlaneTransform::None) {
        out = in;
        return;
    }

    switch (transform) {
        case BcPlaneTransform::Delta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] - in[i - stride]);
            }
            break;
        }
        case BcPlaneTransform::Xor8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] ^ in[i - stride]);
            }
            break;
        }
        case BcPlaneTransform::Delta16: {
            if (n % 2u != 0) {
                out = in;
                return;
            }
            uint16_t prev = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t cur = ReadLe16(in.data() + i);
                const uint16_t delta = (i == 0) ? cur : static_cast<uint16_t>(cur - prev);
                WriteLe16(out.data() + i, delta);
                prev = cur;
            }
            break;
        }
        case BcPlaneTransform::Rgb565Delta: {
            if (n % 2u != 0) {
                out = in;
                return;
            }
            uint16_t pr = 0, pg = 0, pb = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t cur = ReadLe16(in.data() + i);
                const uint16_t r = (cur >> 11u) & 31u;
                const uint16_t g = (cur >> 5u) & 63u;
                const uint16_t b = cur & 31u;
                uint16_t packed = cur;
                if (i > 0) {
                    const uint16_t dr = static_cast<uint16_t>((r - pr) & 31u);
                    const uint16_t dg = static_cast<uint16_t>((g - pg) & 63u);
                    const uint16_t db = static_cast<uint16_t>((b - pb) & 31u);
                    packed = static_cast<uint16_t>((dr << 11u) | (dg << 5u) | db);
                }
                WriteLe16(out.data() + i, packed);
                pr = r;
                pg = g;
                pb = b;
            }
            break;
        }
        case BcPlaneTransform::TransposeDelta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            if (n % stride != 0) {
                out = in;
                return;
            }
            const size_t count = n / stride;
            std::vector<uint8_t> transposed(n);
            for (size_t b = 0; b < count; ++b) {
                for (size_t e = 0; e < stride; ++e) {
                    transposed[e * count + b] = in[b * stride + e];
                }
            }
            out[0] = transposed[0];
            for (size_t i = 1; i < n; ++i) {
                out[i] = static_cast<uint8_t>(transposed[i] - transposed[i - 1]);
            }
            break;
        }
        default:
            out = in;
            break;
    }
}

Status InverseTransform(
    const std::vector<uint8_t>& in,
    uint8_t elem_bytes,
    BcPlaneTransform transform,
    std::vector<uint8_t>& out) {
    const size_t n = in.size();
    out.resize(n);
    if (n == 0 || transform == BcPlaneTransform::None) {
        out = in;
        return Status::Ok();
    }

    switch (transform) {
        case BcPlaneTransform::Delta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] + out[i - stride]);
            }
            return Status::Ok();
        }
        case BcPlaneTransform::Xor8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] ^ out[i - stride]);
            }
            return Status::Ok();
        }
        case BcPlaneTransform::Delta16: {
            if (n % 2u != 0) {
                return Status::Error(StatusCode::ParseError, "Delta16 plane size is not even");
            }
            uint16_t acc = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t delta = ReadLe16(in.data() + i);
                acc = (i == 0) ? delta : static_cast<uint16_t>(acc + delta);
                WriteLe16(out.data() + i, acc);
            }
            return Status::Ok();
        }
        case BcPlaneTransform::Rgb565Delta: {
            if (n % 2u != 0) {
                return Status::Error(StatusCode::ParseError, "Rgb565Delta plane size is not even");
            }
            uint16_t ar = 0, ag = 0, ab = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t d = ReadLe16(in.data() + i);
                const uint16_t dr = (d >> 11u) & 31u;
                const uint16_t dg = (d >> 5u) & 63u;
                const uint16_t db = d & 31u;
                if (i == 0) {
                    ar = dr;
                    ag = dg;
                    ab = db;
                } else {
                    ar = static_cast<uint16_t>((ar + dr) & 31u);
                    ag = static_cast<uint16_t>((ag + dg) & 63u);
                    ab = static_cast<uint16_t>((ab + db) & 31u);
                }
                WriteLe16(out.data() + i, static_cast<uint16_t>((ar << 11u) | (ag << 5u) | ab));
            }
            return Status::Ok();
        }
        case BcPlaneTransform::TransposeDelta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            if (n % stride != 0) {
                return Status::Error(StatusCode::ParseError, "TransposeDelta8 plane size not divisible by stride");
            }
            const size_t count = n / stride;
            std::vector<uint8_t> transposed(n);
            transposed[0] = in[0];
            for (size_t i = 1; i < n; ++i) {
                transposed[i] = static_cast<uint8_t>(transposed[i - 1] + in[i]);
            }
            for (size_t b = 0; b < count; ++b) {
                for (size_t e = 0; e < stride; ++e) {
                    out[b * stride + e] = transposed[e * count + b];
                }
            }
            return Status::Ok();
        }
        default:
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown BCPack plane transform");
    }
}

// ---------------------------------------------------------------------------
// LZ77 + RLE Codec
// ---------------------------------------------------------------------------
// Tag byte:
//   0x00..0x7F : Literal run of (tag + 1) bytes (1..128)
//   0x80..0xBF : RLE run of ((tag - 0x80) + 3) bytes (3..66) of 1 literal byte
//   0xC0..0xFE : LZ77 match of ((tag - 0xC0) + 3) bytes (3..65), followed by uint16 LE distance (1..65535)
//   0xFF       : Extended LZ77 match of (66 + next_byte) bytes (66..321), followed by uint16 LE distance
void LzRleCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0 || n > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return;
    }

    constexpr size_t kHashBits = 13u;
    constexpr size_t kHashSize = 1u << kHashBits;
    constexpr size_t kMaxDist = 65535u;
    constexpr size_t kMaxChain = 12u;

    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(n, -1);

    auto hash3 = [&](size_t pos) -> size_t {
        const uint32_t v = (static_cast<uint32_t>(src[pos]) << 16u) |
                           (static_cast<uint32_t>(src[pos + 1]) << 8u) |
                           static_cast<uint32_t>(src[pos + 2]);
        return ((v * 2654435761u) >> (32u - kHashBits)) & (kHashSize - 1u);
    };

    auto insert_pos = [&](size_t pos) {
        if (pos + 2 < n) {
            const size_t h = hash3(pos);
            prev[pos] = head[h];
            head[h] = static_cast<int32_t>(pos);
        }
    };

    std::vector<uint8_t> pending_literals;
    pending_literals.reserve(128u);

    auto flush_literals = [&]() {
        size_t off = 0;
        while (off < pending_literals.size()) {
            const size_t chunk = std::min<size_t>(128u, pending_literals.size() - off);
            out.push_back(static_cast<uint8_t>(chunk - 1u));
            out.insert(out.end(), pending_literals.begin() + static_cast<ptrdiff_t>(off),
                       pending_literals.begin() + static_cast<ptrdiff_t>(off + chunk));
            off += chunk;
        }
        pending_literals.clear();
    };

    size_t pos = 0;
    while (pos < n) {
        size_t rle_len = 1;
        while (pos + rle_len < n && rle_len < 66u && src[pos + rle_len] == src[pos]) {
            ++rle_len;
        }

        size_t best_len = 0;
        size_t best_dist = 0;
        if (pos + 2 < n) {
            const size_t h = hash3(pos);
            int32_t cand = head[h];
            size_t chain = 0;
            const size_t max_match = std::min<size_t>(321u, n - pos);
            while (cand >= 0 && chain < kMaxChain) {
                const size_t cpos = static_cast<size_t>(cand);
                const size_t dist = pos - cpos;
                if (dist == 0 || dist > kMaxDist) {
                    break;
                }
                if (src[cpos] == src[pos] && src[cpos + best_len] == src[pos + best_len]) {
                    size_t mlen = 0;
                    while (mlen < max_match && src[cpos + mlen] == src[pos + mlen]) {
                        ++mlen;
                    }
                    if (mlen > best_len) {
                        best_len = mlen;
                        best_dist = dist;
                        if (best_len == max_match) {
                            break;
                        }
                    }
                }
                cand = prev[cpos];
                ++chain;
            }
        }

        if (rle_len >= 3u && (rle_len >= best_len || best_len < 4u)) {
            flush_literals();
            out.push_back(static_cast<uint8_t>(0x80u + (rle_len - 3u)));
            out.push_back(src[pos]);
            for (size_t k = 0; k < rle_len; ++k) {
                insert_pos(pos + k);
            }
            pos += rle_len;
        } else if (best_len >= 4u) {
            flush_literals();
            if (best_len <= 65u) {
                out.push_back(static_cast<uint8_t>(0xC0u + (best_len - 3u)));
            } else {
                out.push_back(0xFFu);
                out.push_back(static_cast<uint8_t>(best_len - 66u));
            }
            AppendLe16(out, static_cast<uint16_t>(best_dist));
            for (size_t k = 0; k < best_len; ++k) {
                insert_pos(pos + k);
            }
            pos += best_len;
        } else {
            insert_pos(pos);
            pending_literals.push_back(src[pos]);
            ++pos;
            if (pending_literals.size() == 128u) {
                flush_literals();
            }
        }
    }
    flush_literals();
}

Status LzRleDecompress(const uint8_t* src, size_t src_size, size_t expected_size, std::vector<uint8_t>& out) {
    out.clear();
    if (expected_size == 0) {
        return src_size == 0
                   ? Status::Ok()
                   : Status::Error(StatusCode::ParseError, "LZ-RLE trailing bytes on empty payload");
    }
    out.reserve(expected_size);

    size_t pos = 0;
    while (pos < src_size) {
        const uint8_t tag = src[pos++];
        if (tag < 0x80u) {
            const size_t lit_len = static_cast<size_t>(tag) + 1u;
            if (lit_len > src_size - pos || lit_len > expected_size - out.size()) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE literal run out of bounds");
            }
            out.insert(out.end(), src + pos, src + pos + lit_len);
            pos += lit_len;
        } else if (tag < 0xC0u) {
            const size_t rle_len = static_cast<size_t>(tag - 0x80u) + 3u;
            if (pos >= src_size || rle_len > expected_size - out.size()) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE repeat run out of bounds");
            }
            const uint8_t val = src[pos++];
            out.insert(out.end(), rle_len, val);
        } else {
            size_t match_len = 0;
            if (tag < 0xFFu) {
                match_len = static_cast<size_t>(tag - 0xC0u) + 3u;
            } else {
                if (pos >= src_size) {
                    return Status::Error(StatusCode::ParseError, "LZ-RLE extended match truncated");
                }
                match_len = 66u + static_cast<size_t>(src[pos++]);
            }
            if (2u > src_size - pos) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE distance truncated");
            }
            const size_t dist = ReadLe16(src + pos);
            pos += 2u;
            if (dist == 0 || dist > out.size() || match_len > expected_size - out.size()) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE match distance or length invalid");
            }
            const size_t copy_start = out.size() - dist;
            for (size_t i = 0; i < match_len; ++i) {
                out.push_back(out[copy_start + i]);
            }
        }
    }

    if (out.size() != expected_size) {
        return Status::Error(StatusCode::ParseError, "LZ-RLE decompressed size mismatch");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Canonical Huffman Codec (length-limited <= 15 bits)
// ---------------------------------------------------------------------------
struct HuffNode {
    uint64_t freq = 0;
    int32_t sym = -1;
    int32_t left = -1;
    int32_t right = -1;
};

void ComputeDepths(const std::vector<HuffNode>& nodes, int32_t idx, uint32_t depth, std::array<uint8_t, 256>& lengths) {
    if (idx < 0) return;
    const auto& node = nodes[static_cast<size_t>(idx)];
    if (node.sym >= 0) {
        lengths[static_cast<size_t>(node.sym)] = static_cast<uint8_t>(std::max<uint32_t>(1u, depth));
        return;
    }
    ComputeDepths(nodes, node.left, depth + 1u, lengths);
    ComputeDepths(nodes, node.right, depth + 1u, lengths);
}

void BuildLengthLimitedHuffman(const std::array<uint64_t, 256>& freq, std::array<uint8_t, 256>& lengths) {
    lengths.fill(0);

    std::vector<int32_t> active;
    active.reserve(256u);
    for (int32_t s = 0; s < 256; ++s) {
        if (freq[static_cast<size_t>(s)] > 0) {
            active.push_back(s);
        }
    }
    if (active.empty()) {
        return;
    }
    if (active.size() == 1u) {
        lengths[static_cast<size_t>(active[0])] = 1u;
        return;
    }

    std::vector<HuffNode> nodes;
    nodes.reserve(active.size() * 2u);

    struct HeapEntry {
        uint64_t freq;
        int32_t idx;
        bool operator>(const HeapEntry& o) const noexcept {
            return freq != o.freq ? (freq > o.freq) : (idx > o.idx);
        }
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> pq;

    for (int32_t s : active) {
        const int32_t idx = static_cast<int32_t>(nodes.size());
        nodes.push_back({freq[static_cast<size_t>(s)], s, -1, -1});
        pq.push({freq[static_cast<size_t>(s)], idx});
    }

    while (pq.size() > 1u) {
        const auto a = pq.top(); pq.pop();
        const auto b = pq.top(); pq.pop();
        const int32_t idx = static_cast<int32_t>(nodes.size());
        nodes.push_back({a.freq + b.freq, -1, a.idx, b.idx});
        pq.push({a.freq + b.freq, idx});
    }

    ComputeDepths(nodes, pq.top().idx, 0u, lengths);

    bool clamped = false;
    for (int32_t s : active) {
        if (lengths[static_cast<size_t>(s)] > kMaxHuffBits) {
            lengths[static_cast<size_t>(s)] = static_cast<uint8_t>(kMaxHuffBits);
            clamped = true;
        }
    }

    if (clamped) {
        constexpr uint32_t kKraftTarget = 1u << kMaxHuffBits;
        auto compute_kraft = [&]() -> uint32_t {
            uint32_t sum = 0;
            for (int32_t s : active) {
                sum += 1u << (kMaxHuffBits - lengths[static_cast<size_t>(s)]);
            }
            return sum;
        };

        uint32_t kraft = compute_kraft();
        while (kraft > kKraftTarget) {
            int32_t pick = -1;
            for (int32_t s : active) {
                const uint8_t l = lengths[static_cast<size_t>(s)];
                if (l < kMaxHuffBits) {
                    if (pick < 0 || lengths[static_cast<size_t>(s)] > lengths[static_cast<size_t>(pick)] ||
                        (lengths[static_cast<size_t>(s)] == lengths[static_cast<size_t>(pick)] &&
                         freq[static_cast<size_t>(s)] < freq[static_cast<size_t>(pick)])) {
                        pick = s;
                    }
                }
            }
            if (pick < 0) break;
            const uint8_t l = lengths[static_cast<size_t>(pick)];
            lengths[static_cast<size_t>(pick)] = static_cast<uint8_t>(l + 1u);
            kraft -= 1u << (kMaxHuffBits - l - 1u);
        }
        while (kraft < kKraftTarget) {
            int32_t pick = -1;
            for (int32_t s : active) {
                const uint8_t l = lengths[static_cast<size_t>(s)];
                if (l > 1u) {
                    const uint32_t delta = 1u << (kMaxHuffBits - l);
                    if (kraft + delta <= kKraftTarget) {
                        if (pick < 0 || freq[static_cast<size_t>(s)] > freq[static_cast<size_t>(pick)]) {
                            pick = s;
                        }
                    }
                }
            }
            if (pick < 0) break;
            const uint8_t l = lengths[static_cast<size_t>(pick)];
            lengths[static_cast<size_t>(pick)] = static_cast<uint8_t>(l - 1u);
            kraft += 1u << (kMaxHuffBits - l);
        }
    }
}

void AssignCanonicalCodes(
    const std::array<uint8_t, 256>& lengths,
    std::array<uint16_t, 256>& codes) {
    codes.fill(0);
    std::array<uint32_t, kMaxHuffBits + 1u> count{};
    for (uint8_t l : lengths) {
        if (l > 0 && l <= kMaxHuffBits) {
            ++count[l];
        }
    }
    std::array<uint32_t, kMaxHuffBits + 1u> next_code{};
    uint32_t code = 0;
    for (uint32_t bits = 1u; bits <= kMaxHuffBits; ++bits) {
        code = (code + count[bits - 1u]) << 1u;
        next_code[bits] = code;
    }
    for (size_t s = 0; s < 256u; ++s) {
        const uint8_t l = lengths[s];
        if (l > 0 && l <= kMaxHuffBits) {
            codes[s] = static_cast<uint16_t>(next_code[l]++);
        }
    }
}

void HuffmanCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0) {
        return;
    }

    std::array<uint64_t, 256> freq{};
    for (size_t i = 0; i < n; ++i) {
        ++freq[src[i]];
    }

    std::array<uint8_t, 256> lengths{};
    BuildLengthLimitedHuffman(freq, lengths);

    std::array<uint16_t, 256> codes{};
    AssignCanonicalCodes(lengths, codes);

    std::vector<uint16_t> active_syms;
    active_syms.reserve(256u);
    for (uint16_t s = 0; s < 256u; ++s) {
        if (lengths[s] > 0) {
            active_syms.push_back(s);
        }
    }

    AppendLe32(out, static_cast<uint32_t>(n));
    if (active_syms.size() <= 60u) {
        out.push_back(1u);
        out.push_back(static_cast<uint8_t>(active_syms.size()));
        for (uint16_t s : active_syms) {
            out.push_back(static_cast<uint8_t>(s));
            out.push_back(lengths[s]);
        }
    } else {
        out.push_back(0u);
        for (size_t i = 0; i < 256u; i += 2u) {
            const uint8_t packed = static_cast<uint8_t>((lengths[i] & 0x0Fu) |
                                                        ((lengths[i + 1u] & 0x0Fu) << 4u));
            out.push_back(packed);
        }
    }

    uint64_t bit_buf = 0;
    uint32_t bit_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t s = src[i];
        const uint32_t len = lengths[s];
        const uint32_t code = codes[s];
        bit_buf = (bit_buf << len) | code;
        bit_count += len;
        while (bit_count >= 8u) {
            bit_count -= 8u;
            out.push_back(static_cast<uint8_t>((bit_buf >> bit_count) & 0xFFu));
        }
    }
    if (bit_count > 0) {
        out.push_back(static_cast<uint8_t>((bit_buf << (8u - bit_count)) & 0xFFu));
    }
}

Status HuffmanDecompress(const uint8_t* src, size_t src_size, size_t expected_size, std::vector<uint8_t>& out) {
    out.clear();
    if (expected_size == 0) {
        return src_size == 0
                   ? Status::Ok()
                   : Status::Error(StatusCode::ParseError, "Huffman trailing bytes on empty payload");
    }
    if (src_size < 5u) {
        return Status::Error(StatusCode::ParseError, "Huffman stream header truncated");
    }

    const uint32_t stored_len = ReadLe32(src);
    if (stored_len != expected_size) {
        return Status::Error(StatusCode::ParseError, "Huffman uncompressed length mismatch");
    }

    const uint8_t table_mode = src[4];
    size_t pos = 5u;
    std::array<uint8_t, 256> lengths{};

    if (table_mode == 1u) {
        if (pos >= src_size) {
            return Status::Error(StatusCode::ParseError, "Huffman sparse count truncated");
        }
        const uint8_t count = src[pos++];
        if (count == 0 || static_cast<size_t>(count) * 2u > src_size - pos) {
            return Status::Error(StatusCode::ParseError, "Huffman sparse table invalid");
        }
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t sym = src[pos++];
            const uint8_t len = src[pos++];
            if (len == 0 || len > kMaxHuffBits) {
                return Status::Error(StatusCode::ParseError, "Huffman invalid symbol bit length");
            }
            if (lengths[sym] != 0) {
                return Status::Error(StatusCode::ParseError, "Huffman duplicate symbol in sparse table");
            }
            lengths[sym] = len;
        }
    } else if (table_mode == 0u) {
        if (128u > src_size - pos) {
            return Status::Error(StatusCode::ParseError, "Huffman dense table truncated");
        }
        for (size_t i = 0; i < 256u; i += 2u) {
            const uint8_t packed = src[pos++];
            lengths[i] = packed & 0x0Fu;
            lengths[i + 1u] = (packed >> 4u) & 0x0Fu;
        }
    } else {
        return Status::Error(StatusCode::ParseError, "Unknown Huffman table mode");
    }

    std::array<uint32_t, kMaxHuffBits + 1u> count{};
    uint32_t kraft = 0;
    uint32_t total_syms = 0;
    for (uint8_t l : lengths) {
        if (l > 0) {
            if (l > kMaxHuffBits) {
                return Status::Error(StatusCode::ParseError, "Huffman length > 15");
            }
            ++count[l];
            ++total_syms;
            kraft += 1u << (kMaxHuffBits - l);
        }
    }
    if (total_syms == 0 || kraft > (1u << kMaxHuffBits)) {
        return Status::Error(StatusCode::ParseError, "Huffman Kraft sum invalid");
    }

    std::array<uint32_t, kMaxHuffBits + 1u> first_code{};
    std::array<uint32_t, kMaxHuffBits + 1u> first_index{};
    uint32_t code = 0;
    uint32_t idx = 0;
    for (uint32_t bits = 1u; bits <= kMaxHuffBits; ++bits) {
        code = (code + count[bits - 1u]) << 1u;
        first_code[bits] = code;
        first_index[bits] = idx;
        idx += count[bits];
    }

    std::array<uint8_t, 256> sym_table{};
    std::array<uint32_t, kMaxHuffBits + 1u> offsets = first_index;
    for (uint32_t s = 0; s < 256u; ++s) {
        const uint8_t l = lengths[s];
        if (l > 0) {
            sym_table[offsets[l]++] = static_cast<uint8_t>(s);
        }
    }

    out.resize(expected_size);
    uint32_t cur_byte = 0;
    uint32_t bits_left = 0;

    for (size_t i = 0; i < expected_size; ++i) {
        uint32_t cur_code = 0;
        bool decoded = false;
        for (uint32_t len = 1u; len <= kMaxHuffBits; ++len) {
            if (bits_left == 0) {
                if (pos >= src_size) {
                    return Status::Error(StatusCode::ParseError, "Huffman bitstream truncated");
                }
                cur_byte = src[pos++];
                bits_left = 8u;
            }
            --bits_left;
            cur_code = (cur_code << 1u) | ((cur_byte >> bits_left) & 1u);
            if (count[len] > 0 && cur_code >= first_code[len] &&
                cur_code - first_code[len] < count[len]) {
                out[i] = sym_table[first_index[len] + (cur_code - first_code[len])];
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            return Status::Error(StatusCode::ParseError, "Huffman invalid canonical code");
        }
    }

    // Strict check: no unconsumed trailing bytes and padding bits in final byte must be zero.
    if (pos != src_size) {
        return Status::Error(StatusCode::ParseError, "Huffman trailing unconsumed bytes");
    }
    if (bits_left > 0) {
        const uint32_t pad_mask = (1u << bits_left) - 1u;
        if ((cur_byte & pad_mask) != 0u) {
            return Status::Error(StatusCode::ParseError, "Huffman non-zero trailing padding bits");
        }
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Endpoint Ring-Buffer Sliding-Window Codec (BC1..BC7 Endpoints)
// ---------------------------------------------------------------------------
// Fixed 64-entry history ring buffer of element_bytes (1..8):
//   0x00..0x3F : Single history back-ref at age_idx = tag (0 = most recent, < populated)
//   0x40..0x7F : Repeat most recent element ((tag - 0x40) + 1) times (1..64); does not pollute ring
//   0x80..0x9F : Narrow delta from ring[age_idx = tag & 0x1F] + 1-byte delta:
//                - element_bytes == 2: packed signed RGB565 delta (dr:3 [-4..3], dg:3 [-4..3], db:2 [-2..1])
//                - otherwise: elem[0] += delta_byte, remaining bytes copied from ring[age_idx]
//   0xA0..0xBF : Literal run of ((tag - 0xA0) + 1) elements (1..32), pushing each to ring
//   0xC0..0xFF : Indexed history run of ((tag - 0xC0) + 2) elements (2..65), followed by 1-byte age indices
struct EndpointRing {
    std::array<std::array<uint8_t, 8>, kBcPackEndpointRingCapacity> entries{};
    size_t write_pos = 0;
    size_t populated = 0;
    uint8_t elem_bytes = 1;

    explicit EndpointRing(uint8_t eb) noexcept : elem_bytes(eb) {}

    const uint8_t* Get(size_t age_idx) const noexcept {
        const size_t slot = (write_pos + kBcPackEndpointRingCapacity - 1u - age_idx) %
                            kBcPackEndpointRingCapacity;
        return entries[slot].data();
    }

    void Push(const uint8_t* elem) noexcept {
        std::memcpy(entries[write_pos].data(), elem, elem_bytes);
        write_pos = (write_pos + 1u) % kBcPackEndpointRingCapacity;
        if (populated < kBcPackEndpointRingCapacity) {
            ++populated;
        }
    }
};

int32_t SignedWrapMod(int32_t diff, int32_t modulus) noexcept {
    int32_t d = diff % modulus;
    if (d < 0) d += modulus;
    if (d >= modulus / 2) d -= modulus;
    return d;
}

bool TryEncodeRgb565Delta(uint16_t base, uint16_t target, uint8_t& out_packed) noexcept {
    const int32_t br = static_cast<int32_t>((base >> 11u) & 31u);
    const int32_t bg = static_cast<int32_t>((base >> 5u) & 63u);
    const int32_t bb = static_cast<int32_t>(base & 31u);
    const int32_t tr = static_cast<int32_t>((target >> 11u) & 31u);
    const int32_t tg = static_cast<int32_t>((target >> 5u) & 63u);
    const int32_t tb = static_cast<int32_t>(target & 31u);

    const int32_t dr = SignedWrapMod(tr - br, 32);
    const int32_t dg = SignedWrapMod(tg - bg, 64);
    const int32_t db = SignedWrapMod(tb - bb, 32);

    if (dr < -4 || dr > 3 || dg < -4 || dg > 3 || db < -2 || db > 1) {
        return false;
    }
    const uint8_t ur = static_cast<uint8_t>(dr & 0x07);
    const uint8_t ug = static_cast<uint8_t>(dg & 0x07);
    const uint8_t ub = static_cast<uint8_t>(db & 0x03);
    out_packed = static_cast<uint8_t>((ur << 5u) | (ug << 2u) | ub);
    return true;
}

uint16_t ApplyRgb565Delta(uint16_t base, uint8_t packed) noexcept {
    int32_t dr = static_cast<int32_t>((packed >> 5u) & 0x07u);
    if (dr >= 4) dr -= 8;
    int32_t dg = static_cast<int32_t>((packed >> 2u) & 0x07u);
    if (dg >= 4) dg -= 8;
    int32_t db = static_cast<int32_t>(packed & 0x03u);
    if (db >= 2) db -= 4;

    const uint16_t r = static_cast<uint16_t>((static_cast<int32_t>((base >> 11u) & 31u) + dr) & 31);
    const uint16_t g = static_cast<uint16_t>((static_cast<int32_t>((base >> 5u) & 63u) + dg) & 63);
    const uint16_t b = static_cast<uint16_t>((static_cast<int32_t>(base & 31u) + db) & 31);
    return static_cast<uint16_t>((r << 11u) | (g << 5u) | b);
}

void EndpointRingWindowCompress(
    const uint8_t* src,
    size_t n,
    uint8_t elem_bytes,
    std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0 || elem_bytes == 0 || elem_bytes > 8u || (n % elem_bytes) != 0u) {
        return;
    }
    const size_t elem_count = n / elem_bytes;
    EndpointRing ring(elem_bytes);

    std::vector<uint8_t> pending_literals;
    pending_literals.reserve(32u * elem_bytes);
    size_t pending_lit_count = 0;

    auto flush_literals = [&]() {
        if (pending_lit_count == 0) return;
        size_t off = 0;
        while (pending_lit_count > 0) {
            const size_t chunk = std::min<size_t>(32u, pending_lit_count);
            out.push_back(static_cast<uint8_t>(0xA0u + (chunk - 1u)));
            out.insert(out.end(),
                       pending_literals.begin() + static_cast<ptrdiff_t>(off * elem_bytes),
                       pending_literals.begin() + static_cast<ptrdiff_t>((off + chunk) * elem_bytes));
            off += chunk;
            pending_lit_count -= chunk;
        }
        pending_literals.clear();
    };

    size_t idx = 0;
    while (idx < elem_count) {
        const uint8_t* cur = src + idx * elem_bytes;

        // 1. Check repeat-most-recent run
        if (ring.populated > 0 && std::memcmp(cur, ring.Get(0), elem_bytes) == 0) {
            size_t run = 1u;
            while (idx + run < elem_count && run < 64u &&
                   std::memcmp(src + (idx + run) * elem_bytes, cur, elem_bytes) == 0) {
                ++run;
            }
            if (run >= 2u || elem_bytes >= 2u) {
                flush_literals();
                out.push_back(static_cast<uint8_t>(0x40u + (run - 1u)));
                idx += run;
                continue;
            }
        }

        // 2. Check exact ring hit
        int32_t exact_age = -1;
        for (size_t a = 0; a < ring.populated; ++a) {
            if (std::memcmp(cur, ring.Get(a), elem_bytes) == 0) {
                exact_age = static_cast<int32_t>(a);
                break;
            }
        }
        if (exact_age >= 0) {
            flush_literals();
            out.push_back(static_cast<uint8_t>(exact_age));
            ring.Push(cur);
            ++idx;
            continue;
        }

        // 3. Check narrow RGB565 delta from ring[0..31] when elem_bytes == 2
        if (elem_bytes == 2u && ring.populated > 0) {
            const uint16_t cur16 = ReadLe16(cur);
            const size_t max_search = std::min<size_t>(32u, ring.populated);
            int32_t delta_age = -1;
            uint8_t delta_byte = 0;
            for (size_t a = 0; a < max_search; ++a) {
                const uint16_t base16 = ReadLe16(ring.Get(a));
                if (TryEncodeRgb565Delta(base16, cur16, delta_byte)) {
                    delta_age = static_cast<int32_t>(a);
                    break;
                }
            }
            if (delta_age >= 0) {
                flush_literals();
                out.push_back(static_cast<uint8_t>(0x80u | static_cast<uint8_t>(delta_age)));
                out.push_back(delta_byte);
                ring.Push(cur);
                ++idx;
                continue;
            }
        }

        // 4. Literal fallback
        pending_literals.insert(pending_literals.end(), cur, cur + elem_bytes);
        ++pending_lit_count;
        ring.Push(cur);
        ++idx;
        if (pending_lit_count == 32u) {
            flush_literals();
        }
    }
    flush_literals();
}

Status EndpointRingWindowDecompress(
    const uint8_t* src,
    size_t src_size,
    size_t expected_size,
    uint8_t elem_bytes,
    std::vector<uint8_t>& out) {
    out.clear();
    if (elem_bytes == 0 || elem_bytes > 8u || (expected_size % elem_bytes) != 0u) {
        return Status::Error(StatusCode::ParseError, "EndpointRingWindow element alignment invalid");
    }
    const size_t expected_elems = expected_size / elem_bytes;
    if (expected_elems == 0) {
        return src_size == 0
                   ? Status::Ok()
                   : Status::Error(StatusCode::ParseError, "EndpointRingWindow trailing bytes on empty payload");
    }

    out.reserve(expected_size);
    EndpointRing ring(elem_bytes);
    size_t emitted = 0;
    size_t pos = 0;

    while (pos < src_size) {
        const uint8_t tag = src[pos++];
        if (tag < 0x40u) {
            // Single back-reference
            const size_t age_idx = tag;
            if (age_idx >= ring.populated || emitted >= expected_elems) {
                return Status::Error(StatusCode::ParseError,
                                     "EndpointRingWindow back-reference out of bounds");
            }
            std::array<uint8_t, 8> elem{};
            std::memcpy(elem.data(), ring.Get(age_idx), elem_bytes);
            out.insert(out.end(), elem.data(), elem.data() + elem_bytes);
            ring.Push(elem.data());
            ++emitted;
        } else if (tag < 0x80u) {
            // Repeat most recent element
            const size_t run_len = static_cast<size_t>(tag - 0x40u) + 1u;
            if (ring.populated == 0 || run_len > expected_elems - emitted) {
                return Status::Error(StatusCode::ParseError,
                                     "EndpointRingWindow repeat run out of bounds");
            }
            const uint8_t* recent = ring.Get(0);
            for (size_t k = 0; k < run_len; ++k) {
                out.insert(out.end(), recent, recent + elem_bytes);
            }
            emitted += run_len;
        } else if (tag < 0xA0u) {
            // Narrow delta from ring[age_idx]
            const size_t age_idx = tag & 0x1Fu;
            if (age_idx >= ring.populated || pos >= src_size || emitted >= expected_elems) {
                return Status::Error(StatusCode::ParseError,
                                     "EndpointRingWindow delta reference out of bounds");
            }
            const uint8_t delta_byte = src[pos++];
            std::array<uint8_t, 8> elem{};
            std::memcpy(elem.data(), ring.Get(age_idx), elem_bytes);
            if (elem_bytes == 2u) {
                const uint16_t base16 = ReadLe16(elem.data());
                WriteLe16(elem.data(), ApplyRgb565Delta(base16, delta_byte));
            } else {
                elem[0] = static_cast<uint8_t>(elem[0] + delta_byte);
            }
            out.insert(out.end(), elem.data(), elem.data() + elem_bytes);
            ring.Push(elem.data());
            ++emitted;
        } else if (tag < 0xC0u) {
            // Literal run
            const size_t lit_len = static_cast<size_t>(tag - 0xA0u) + 1u;
            if (lit_len > expected_elems - emitted || lit_len * elem_bytes > src_size - pos) {
                return Status::Error(StatusCode::ParseError,
                                     "EndpointRingWindow literal run out of bounds");
            }
            for (size_t k = 0; k < lit_len; ++k) {
                const uint8_t* elem = src + pos;
                pos += elem_bytes;
                out.insert(out.end(), elem, elem + elem_bytes);
                ring.Push(elem);
            }
            emitted += lit_len;
        } else {
            // Indexed history run
            const size_t run_len = static_cast<size_t>(tag - 0xC0u) + 2u;
            if (run_len > expected_elems - emitted || run_len > src_size - pos) {
                return Status::Error(StatusCode::ParseError,
                                     "EndpointRingWindow indexed run out of bounds");
            }
            for (size_t k = 0; k < run_len; ++k) {
                const size_t age_idx = src[pos++];
                if (age_idx >= kBcPackEndpointRingCapacity || age_idx >= ring.populated) {
                    return Status::Error(StatusCode::ParseError,
                                         "EndpointRingWindow indexed age out of bounds");
                }
                std::array<uint8_t, 8> elem{};
                std::memcpy(elem.data(), ring.Get(age_idx), elem_bytes);
                out.insert(out.end(), elem.data(), elem.data() + elem_bytes);
                ring.Push(elem.data());
            }
            emitted += run_len;
        }
    }

    if (emitted != expected_elems || out.size() != expected_size) {
        return Status::Error(StatusCode::ParseError, "EndpointRingWindow decompressed size mismatch");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Selector Index-Table & Sliding-Window Codec (BC1..BC7 Selectors)
// ---------------------------------------------------------------------------
// Header:
//   uint16 LE table_size (0..256)
//   table_size * elem_bytes palette entries
// Commands:
//   0x00..0x3F : Literal selector run of (tag + 1) elements (1..64)
//   0x40..0x7F : Indexed selector run of ((tag - 0x40) + 1) elements (1..64), 1-byte indices (< table_size)
//   0x80..0xBF : Repeated indexed selector of ((tag - 0x80) + 2) elements (2..65), 1-byte index (< table_size)
//   0xC0..0xFF : Element-aligned sliding-window back-ref of ((tag - 0xC0) + 1) elements (1..64), uint16 LE dist
uint64_t PackElementKey(const uint8_t* elem, uint8_t elem_bytes) noexcept {
    uint64_t key = 0;
    for (uint8_t i = 0; i < elem_bytes; ++i) {
        key |= static_cast<uint64_t>(elem[i]) << (static_cast<uint64_t>(i) * 8u);
    }
    return key;
}

void SelectorIndexTableCompress(
    const uint8_t* src,
    size_t n,
    uint8_t elem_bytes,
    std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0 || elem_bytes == 0 || elem_bytes > 8u || (n % elem_bytes) != 0u) {
        return;
    }
    const size_t elem_count = n / elem_bytes;

    // 1. Build frequency histogram for palette selection (up to 256 frequent selector entries)
    std::map<uint64_t, uint32_t> freq;
    for (size_t i = 0; i < elem_count; ++i) {
        ++freq[PackElementKey(src + i * elem_bytes, elem_bytes)];
    }

    std::vector<std::pair<uint32_t, uint64_t>> ranked;
    ranked.reserve(freq.size());
    for (const auto& kv : freq) {
        if (kv.second >= 2u) {
            ranked.push_back({kv.second, kv.first});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? (a.first > b.first) : (a.second < b.second);
    });
    if (ranked.size() > kBcPackMaxSelectorTableEntries) {
        ranked.resize(kBcPackMaxSelectorTableEntries);
    }

    std::map<uint64_t, uint8_t> key_to_idx;
    const uint16_t table_size = static_cast<uint16_t>(ranked.size());
    AppendLe16(out, table_size);
    for (uint16_t i = 0; i < table_size; ++i) {
        const uint64_t key = ranked[i].second;
        key_to_idx[key] = static_cast<uint8_t>(i);
        for (uint8_t b = 0; b < elem_bytes; ++b) {
            out.push_back(static_cast<uint8_t>((key >> (static_cast<uint64_t>(b) * 8u)) & 0xFFu));
        }
    }

    std::vector<uint8_t> pending_lit;
    size_t pending_lit_elems = 0;
    std::vector<uint8_t> pending_idx;

    auto flush_lit = [&]() {
        if (pending_lit_elems == 0) return;
        size_t off = 0;
        while (pending_lit_elems > 0) {
            const size_t chunk = std::min<size_t>(64u, pending_lit_elems);
            out.push_back(static_cast<uint8_t>(chunk - 1u));
            out.insert(out.end(),
                       pending_lit.begin() + static_cast<ptrdiff_t>(off * elem_bytes),
                       pending_lit.begin() + static_cast<ptrdiff_t>((off + chunk) * elem_bytes));
            off += chunk;
            pending_lit_elems -= chunk;
        }
        pending_lit.clear();
    };

    auto flush_idx = [&]() {
        if (pending_idx.empty()) return;
        size_t off = 0;
        while (off < pending_idx.size()) {
            const size_t chunk = std::min<size_t>(64u, pending_idx.size() - off);
            out.push_back(static_cast<uint8_t>(0x40u + (chunk - 1u)));
            out.insert(out.end(),
                       pending_idx.begin() + static_cast<ptrdiff_t>(off),
                       pending_idx.begin() + static_cast<ptrdiff_t>(off + chunk));
            off += chunk;
        }
        pending_idx.clear();
    };

    size_t i = 0;
    while (i < elem_count) {
        const uint8_t* cur = src + i * elem_bytes;
        const uint64_t cur_key = PackElementKey(cur, elem_bytes);
        const auto pal_it = key_to_idx.find(cur_key);

        // Check repeated identical element run (either palette-repeat or window-repeat)
        size_t rep_run = 1u;
        while (i + rep_run < elem_count && rep_run < 65u &&
               std::memcmp(src + (i + rep_run) * elem_bytes, cur, elem_bytes) == 0) {
            ++rep_run;
        }
        if (pal_it != key_to_idx.end() && rep_run >= 3u) {
            flush_lit();
            flush_idx();
            out.push_back(static_cast<uint8_t>(0x80u + (rep_run - 2u)));
            out.push_back(pal_it->second);
            i += rep_run;
            continue;
        }

        // Check short sliding-window back-reference (searches last 64 elements for >= 3-elem match)
        size_t best_len = 0;
        size_t best_dist = 0;
        if (i > 0 && elem_bytes >= 2u) {
            const size_t max_back = std::min<size_t>(i, 128u);
            const size_t max_len = std::min<size_t>(64u, elem_count - i);
            for (size_t dist = 1u; dist <= max_back; ++dist) {
                size_t m = 0;
                while (m < max_len &&
                       std::memcmp(src + (i - dist + m) * elem_bytes,
                                   src + (i + m) * elem_bytes,
                                   elem_bytes) == 0) {
                    ++m;
                }
                if (m > best_len) {
                    best_len = m;
                    best_dist = dist;
                    if (best_len == max_len) break;
                }
            }
        }
        if (best_len >= 4u) {
            flush_lit();
            flush_idx();
            out.push_back(static_cast<uint8_t>(0xC0u + (best_len - 1u)));
            AppendLe16(out, static_cast<uint16_t>(best_dist));
            i += best_len;
            continue;
        }

        if (pal_it != key_to_idx.end()) {
            flush_lit();
            pending_idx.push_back(pal_it->second);
            if (pending_idx.size() == 64u) {
                flush_idx();
            }
        } else {
            flush_idx();
            pending_lit.insert(pending_lit.end(), cur, cur + elem_bytes);
            ++pending_lit_elems;
            if (pending_lit_elems == 64u) {
                flush_lit();
            }
        }
        ++i;
    }

    flush_lit();
    flush_idx();
}

Status SelectorIndexTableDecompress(
    const uint8_t* src,
    size_t src_size,
    size_t expected_size,
    uint8_t elem_bytes,
    std::vector<uint8_t>& out) {
    out.clear();
    if (elem_bytes == 0 || elem_bytes > 8u || (expected_size % elem_bytes) != 0u) {
        return Status::Error(StatusCode::ParseError, "SelectorIndexTable element alignment invalid");
    }
    const size_t expected_elems = expected_size / elem_bytes;
    if (expected_elems == 0) {
        return src_size == 0
                   ? Status::Ok()
                   : Status::Error(StatusCode::ParseError, "SelectorIndexTable trailing bytes on empty payload");
    }
    if (src_size < 2u) {
        return Status::Error(StatusCode::ParseError, "SelectorIndexTable header truncated");
    }

    const uint16_t table_size = ReadLe16(src);
    if (table_size > kBcPackMaxSelectorTableEntries) {
        return Status::Error(StatusCode::ParseError, "SelectorIndexTable table_size exceeds 256");
    }
    const size_t table_bytes = static_cast<size_t>(table_size) * elem_bytes;
    if (table_bytes > src_size - 2u) {
        return Status::Error(StatusCode::ParseError, "SelectorIndexTable palette truncated");
    }

    const uint8_t* table_data = src + 2u;
    size_t pos = 2u + table_bytes;
    size_t emitted = 0;
    out.reserve(expected_size);

    while (pos < src_size) {
        const uint8_t tag = src[pos++];
        if (tag < 0x40u) {
            // Literal selector run
            const size_t count = static_cast<size_t>(tag) + 1u;
            if (count > expected_elems - emitted || count * elem_bytes > src_size - pos) {
                return Status::Error(StatusCode::ParseError, "SelectorIndexTable literal run out of bounds");
            }
            out.insert(out.end(), src + pos, src + pos + count * elem_bytes);
            pos += count * elem_bytes;
            emitted += count;
        } else if (tag < 0x80u) {
            // Indexed selector run
            const size_t count = static_cast<size_t>(tag - 0x40u) + 1u;
            if (count > expected_elems - emitted || count > src_size - pos) {
                return Status::Error(StatusCode::ParseError, "SelectorIndexTable indexed run out of bounds");
            }
            for (size_t k = 0; k < count; ++k) {
                const uint8_t idx = src[pos++];
                if (idx >= table_size) {
                    return Status::Error(StatusCode::ParseError,
                                         "SelectorIndexTable palette index out of bounds");
                }
                const uint8_t* entry = table_data + static_cast<size_t>(idx) * elem_bytes;
                out.insert(out.end(), entry, entry + elem_bytes);
            }
            emitted += count;
        } else if (tag < 0xC0u) {
            // Repeated indexed selector
            const size_t count = static_cast<size_t>(tag - 0x80u) + 2u;
            if (count > expected_elems - emitted || pos >= src_size) {
                return Status::Error(StatusCode::ParseError,
                                     "SelectorIndexTable repeated index run out of bounds");
            }
            const uint8_t idx = src[pos++];
            if (idx >= table_size) {
                return Status::Error(StatusCode::ParseError,
                                     "SelectorIndexTable repeated palette index out of bounds");
            }
            const uint8_t* entry = table_data + static_cast<size_t>(idx) * elem_bytes;
            for (size_t k = 0; k < count; ++k) {
                out.insert(out.end(), entry, entry + elem_bytes);
            }
            emitted += count;
        } else {
            // Element-aligned sliding-window back-reference
            const size_t count = static_cast<size_t>(tag - 0xC0u) + 1u;
            if (count > expected_elems - emitted || 2u > src_size - pos) {
                return Status::Error(StatusCode::ParseError,
                                     "SelectorIndexTable back-reference truncated or out of bounds");
            }
            const size_t dist = ReadLe16(src + pos);
            pos += 2u;
            if (dist == 0 || dist > emitted) {
                return Status::Error(StatusCode::ParseError,
                                     "SelectorIndexTable sliding-window distance invalid");
            }
            for (size_t k = 0; k < count; ++k) {
                const size_t src_elem = emitted - dist;
                const size_t src_byte = src_elem * elem_bytes;
                for (uint8_t b = 0; b < elem_bytes; ++b) {
                    out.push_back(out[src_byte + b]);
                }
                ++emitted;
            }
        }
    }

    if (emitted != expected_elems || out.size() != expected_size) {
        return Status::Error(StatusCode::ParseError, "SelectorIndexTable decompressed size mismatch");
    }
    return Status::Ok();
}

}  // namespace

uint32_t BcFormatBlockBytes(BcFormat format) noexcept {
    switch (format) {
        case BcFormat::BC1:
        case BcFormat::BC4:
            return 8u;
        case BcFormat::BC2:
        case BcFormat::BC3:
        case BcFormat::BC5:
        case BcFormat::BC6H:
        case BcFormat::BC7:
            return 16u;
        default:
            return 0u;
    }
}

uint64_t BcCalculateSurfaceSize(BcFormat format, uint32_t width, uint32_t height) noexcept {
    const uint32_t block_bytes = BcFormatBlockBytes(format);
    if (block_bytes == 0 || width == 0 || height == 0 ||
        width > kBcPackMaxDimension || height > kBcPackMaxDimension) {
        return 0;
    }
    const uint64_t bw = (static_cast<uint64_t>(width) + 3ull) / 4ull;
    const uint64_t bh = (static_cast<uint64_t>(height) + 3ull) / 4ull;
    return bw * bh * static_cast<uint64_t>(block_bytes);
}

uint32_t BcCalculateMaxMipLevels(uint32_t base_width, uint32_t base_height) noexcept {
    if (base_width == 0 || base_height == 0 ||
        base_width > kBcPackMaxDimension || base_height > kBcPackMaxDimension) {
        return 0u;
    }
    uint32_t w = base_width;
    uint32_t h = base_height;
    uint32_t levels = 1u;
    while (w > 1u || h > 1u) {
        w = std::max<uint32_t>(1u, w >> 1u);
        h = std::max<uint32_t>(1u, h >> 1u);
        ++levels;
    }
    return levels;
}

bool BcCalculateMipLevelInfo(
    BcFormat format,
    uint32_t base_width,
    uint32_t base_height,
    uint32_t mip_level,
    BcPackMipLevelInfo& out_info) noexcept {
    out_info = BcPackMipLevelInfo{};
    const uint32_t block_bytes = BcFormatBlockBytes(format);
    const uint32_t max_mips = BcCalculateMaxMipLevels(base_width, base_height);
    if (block_bytes == 0 || max_mips == 0 || mip_level >= max_mips) {
        return false;
    }

    uint32_t w = base_width;
    uint32_t h = base_height;
    for (uint32_t m = 0; m < mip_level; ++m) {
        w = std::max<uint32_t>(1u, w >> 1u);
        h = std::max<uint32_t>(1u, h >> 1u);
    }

    const uint32_t bw = static_cast<uint32_t>((static_cast<uint64_t>(w) + 3ull) / 4ull);
    const uint32_t bh = static_cast<uint32_t>((static_cast<uint64_t>(h) + 3ull) / 4ull);
    const uint64_t blocks = static_cast<uint64_t>(bw) * static_cast<uint64_t>(bh);
    const uint64_t bytes = blocks * static_cast<uint64_t>(block_bytes);
    if (blocks == 0 || blocks > std::numeric_limits<uint32_t>::max() ||
        bytes > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    out_info.mip_level = mip_level;
    out_info.width = w;
    out_info.height = h;
    out_info.block_width = bw;
    out_info.block_height = bh;
    out_info.block_count = static_cast<uint32_t>(blocks);
    out_info.byte_size = bytes;
    return true;
}

uint32_t BcPackCrc32(const uint8_t* data, size_t size) noexcept {
    if (!data || size == 0) {
        return 0u;
    }
    const auto& table = GetCrc32Table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return crc ^ 0xFFFFFFFFu;
}

Status BcPackParseHeader(const uint8_t* src, size_t src_size, BcPackHeader& out_header) {
    out_header = BcPackHeader{};
    if (!src || src_size < kHeaderFixedBytes) {
        return Status::Error(StatusCode::ParseError, "BCPack stream too small for header");
    }

    out_header.magic = ReadLe32(src + 0);
    if (out_header.magic != kBcPackMagic) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack magic (expected 'BCPK')");
    }

    out_header.version = ReadLe16(src + 4);
    if (out_header.version != kBcPackVersion) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BCPack version");
    }

    out_header.flags = ReadLe16(src + 6);
    if (out_header.flags != 0u) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported non-zero BCPack header flags");
    }

    out_header.format = static_cast<BcFormat>(ReadLe32(src + 8));
    out_header.width = ReadLe32(src + 12);
    out_header.height = ReadLe32(src + 16);
    out_header.block_count = ReadLe32(src + 20);
    out_header.original_size = ReadLe32(src + 24);
    out_header.plane_count = ReadLe32(src + 28);
    out_header.uncompressed_crc32 = ReadLe32(src + 32);

    const uint32_t block_bytes = BcFormatBlockBytes(out_header.format);
    if (block_bytes == 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BC format in BCPack header");
    }
    if (out_header.width == 0 || out_header.height == 0 ||
        out_header.width > kBcPackMaxDimension || out_header.height > kBcPackMaxDimension) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack texture dimensions");
    }

    const uint64_t bw = (static_cast<uint64_t>(out_header.width) + 3ull) / 4ull;
    const uint64_t bh = (static_cast<uint64_t>(out_header.height) + 3ull) / 4ull;
    const uint64_t expected_blocks = bw * bh;
    const uint64_t expected_bytes = expected_blocks * static_cast<uint64_t>(block_bytes);

    if (expected_blocks == 0 || expected_blocks > std::numeric_limits<uint32_t>::max() ||
        expected_bytes > std::numeric_limits<uint32_t>::max() ||
        expected_blocks != out_header.block_count || expected_bytes != out_header.original_size) {
        return Status::Error(StatusCode::ParseError, "BCPack block count / original size mismatch");
    }

    const auto expected_specs = GetFormatPlanes(out_header.format);
    if (out_header.plane_count == 0 || out_header.plane_count > kBcPackMaxPlanes ||
        out_header.plane_count != expected_specs.size()) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack plane count");
    }

    const size_t table_end = kHeaderFixedBytes + static_cast<size_t>(out_header.plane_count) * kPlaneDescBytes;
    if (table_end > src_size) {
        return Status::Error(StatusCode::ParseError, "BCPack plane descriptor table truncated");
    }

    out_header.planes.resize(out_header.plane_count);
    uint64_t sum_raw_sizes = 0;
    for (uint32_t i = 0; i < out_header.plane_count; ++i) {
        const uint8_t* p = src + kHeaderFixedBytes + static_cast<size_t>(i) * kPlaneDescBytes;
        BcPackPlaneDescriptor& d = out_header.planes[i];
        d.kind = static_cast<BcPlaneKind>(p[0]);
        d.transform = static_cast<BcPlaneTransform>(p[1]);
        d.codec = static_cast<BcPlaneCodec>(p[2]);
        d.element_bytes = p[3];
        d.raw_size = ReadLe32(p + 4);
        d.compressed_size = ReadLe32(p + 8);
        d.data_offset = ReadLe32(p + 12);
        d.plane_crc32 = ReadLe32(p + 16);

        if (d.kind != expected_specs[i].kind) {
            return Status::Error(StatusCode::ParseError, "BCPack plane kind mismatch");
        }
        if (!IsValidPlaneTransform(d.transform)) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown BCPack plane transform in header");
        }
        if (!IsValidPlaneCodec(d.codec)) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown BCPack plane codec in header");
        }
        if (d.element_bytes == 0 || d.element_bytes != expected_specs[i].element_bytes ||
            d.raw_size == 0 ||
            d.raw_size != static_cast<uint64_t>(out_header.block_count) * d.element_bytes) {
            return Status::Error(StatusCode::ParseError, "BCPack plane descriptor raw_size mismatch");
        }
        if (!TransformCompatibleWithPlane(d.transform, d.element_bytes, d.raw_size)) {
            return Status::Error(StatusCode::ParseError, "BCPack plane transform incompatible with plane geometry");
        }
        if (d.compressed_size == 0) {
            return Status::Error(StatusCode::ParseError, "BCPack plane compressed_size is zero");
        }
        // Strict codec-specific minimum payload & element geometry checks before decompression.
        switch (d.codec) {
            case BcPlaneCodec::Raw:
                if (d.compressed_size != d.raw_size) {
                    return Status::Error(StatusCode::ParseError, "BCPack raw plane compressed_size != raw_size");
                }
                break;
            case BcPlaneCodec::CanonicalHuffman:
            case BcPlaneCodec::LzRleHuffman:
                if (d.compressed_size < 5u) {
                    return Status::Error(StatusCode::ParseError, "BCPack Huffman plane payload truncated");
                }
                break;
            case BcPlaneCodec::EndpointRingWindow:
                if (d.element_bytes == 0u || d.element_bytes > 8u || (d.raw_size % d.element_bytes) != 0u) {
                    return Status::Error(StatusCode::ParseError, "BCPack EndpointRingWindow geometry invalid");
                }
                break;
            case BcPlaneCodec::SelectorIndexTable:
                if (d.element_bytes == 0u || d.element_bytes > 8u ||
                    (d.raw_size % d.element_bytes) != 0u || d.compressed_size < 2u) {
                    return Status::Error(StatusCode::ParseError, "BCPack SelectorIndexTable geometry/header invalid");
                }
                break;
            case BcPlaneCodec::LzRle:
            default:
                break;
        }
        if (d.data_offset < table_end ||
            static_cast<uint64_t>(d.data_offset) + static_cast<uint64_t>(d.compressed_size) > src_size) {
            return Status::Error(StatusCode::ParseError, "BCPack plane payload out of bounds");
        }
        sum_raw_sizes += d.raw_size;
    }

    if (sum_raw_sizes != out_header.original_size) {
        return Status::Error(StatusCode::ParseError, "BCPack total plane raw_size mismatch");
    }

    // Reject overlapping plane payload ranges (guards against aliasing/zip-bomb streams).
    for (uint32_t i = 0; i < out_header.plane_count; ++i) {
        const uint64_t a0 = out_header.planes[i].data_offset;
        const uint64_t a1 = a0 + out_header.planes[i].compressed_size;
        for (uint32_t j = i + 1u; j < out_header.plane_count; ++j) {
            const uint64_t b0 = out_header.planes[j].data_offset;
            const uint64_t b1 = b0 + out_header.planes[j].compressed_size;
            if (a0 < b1 && b0 < a1) {
                return Status::Error(StatusCode::ParseError, "BCPack plane payload ranges overlap");
            }
        }
    }

    return Status::Ok();
}

Status BcPackCompressWithOptions(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src_blocks,
    size_t src_size,
    const BcPackCompressOptions& options,
    std::vector<uint8_t>& out_compressed,
    BcPackStats* out_stats) {
    out_compressed.clear();

    const uint32_t block_bytes = BcFormatBlockBytes(format);
    if (block_bytes == 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BC format");
    }
    if (width == 0 || height == 0 || width > kBcPackMaxDimension || height > kBcPackMaxDimension) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BC texture dimensions");
    }
    const uint64_t expected_size = BcCalculateSurfaceSize(format, width, height);
    if (!src_blocks || src_size != expected_size || expected_size > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::InvalidArgument, "BC surface buffer pointer or size invalid");
    }
    if (options.force_transform && !IsValidPlaneTransform(options.forced_transform)) {
        return Status::Error(StatusCode::InvalidArgument, "Forced BCPack transform invalid");
    }
    if (options.force_codec && !IsValidPlaneCodec(options.forced_codec)) {
        return Status::Error(StatusCode::InvalidArgument, "Forced BCPack codec invalid");
    }

    const uint32_t block_count = static_cast<uint32_t>(expected_size / block_bytes);
    const auto specs = GetFormatPlanes(format);
    const uint32_t plane_count = static_cast<uint32_t>(specs.size());

    std::vector<BcPackPlaneDescriptor> descs(plane_count);
    std::vector<std::vector<uint8_t>> payloads(plane_count);

    for (uint32_t p = 0; p < plane_count; ++p) {
        const PlaneSpec& spec = specs[p];
        const size_t raw_size = static_cast<size_t>(block_count) * spec.element_bytes;
        std::vector<uint8_t> raw_plane(raw_size);

        for (uint32_t b = 0; b < block_count; ++b) {
            const uint8_t* blk = src_blocks + static_cast<size_t>(b) * block_bytes + spec.block_offset;
            std::memcpy(raw_plane.data() + static_cast<size_t>(b) * spec.element_bytes,
                        blk, spec.element_bytes);
        }

        std::vector<BcPlaneTransform> candidate_transforms;
        if (options.force_transform) {
            if (!TransformCompatibleWithPlane(options.forced_transform, spec.element_bytes, raw_size)) {
                candidate_transforms.push_back(BcPlaneTransform::None);
            } else {
                candidate_transforms.push_back(options.forced_transform);
            }
        } else {
            candidate_transforms.push_back(BcPlaneTransform::None);
            candidate_transforms.push_back(BcPlaneTransform::Delta8);
            candidate_transforms.push_back(BcPlaneTransform::Xor8);
            if (spec.is_rgb565) {
                candidate_transforms.push_back(BcPlaneTransform::Rgb565Delta);
                candidate_transforms.push_back(BcPlaneTransform::Delta16);
            }
            if (spec.element_bytes > 1u) {
                candidate_transforms.push_back(BcPlaneTransform::TransposeDelta8);
            }
        }

        BcPlaneTransform best_transform = BcPlaneTransform::None;
        BcPlaneCodec best_codec = BcPlaneCodec::Raw;
        std::vector<uint8_t> best_payload;
        bool has_best = false;

        auto consider = [&](BcPlaneTransform tr, BcPlaneCodec codec, const std::vector<uint8_t>& buf) {
            if (buf.empty()) return;
            if (!has_best || buf.size() < best_payload.size()) {
                best_transform = tr;
                best_codec = codec;
                best_payload = buf;
                has_best = true;
            }
        };

        std::vector<uint8_t> transformed;
        std::vector<uint8_t> lz_buf;
        std::vector<uint8_t> huff_buf;
        std::vector<uint8_t> lz_huff_buf;
        std::vector<uint8_t> ring_buf;
        std::vector<uint8_t> sel_buf;

        if (!options.force_codec) {
            consider(BcPlaneTransform::None, BcPlaneCodec::Raw, raw_plane);
        }

        for (BcPlaneTransform tr : candidate_transforms) {
            ApplyTransform(raw_plane, spec.element_bytes, tr, transformed);

            if (options.force_codec) {
                switch (options.forced_codec) {
                    case BcPlaneCodec::Raw:
                        consider(tr, BcPlaneCodec::Raw, transformed);
                        break;
                    case BcPlaneCodec::LzRle:
                        LzRleCompress(transformed.data(), transformed.size(), lz_buf);
                        consider(tr, BcPlaneCodec::LzRle, lz_buf);
                        break;
                    case BcPlaneCodec::CanonicalHuffman:
                        HuffmanCompress(transformed.data(), transformed.size(), huff_buf);
                        consider(tr, BcPlaneCodec::CanonicalHuffman, huff_buf);
                        break;
                    case BcPlaneCodec::LzRleHuffman:
                        LzRleCompress(transformed.data(), transformed.size(), lz_buf);
                        if (!lz_buf.empty()) {
                            HuffmanCompress(lz_buf.data(), lz_buf.size(), lz_huff_buf);
                            consider(tr, BcPlaneCodec::LzRleHuffman, lz_huff_buf);
                        }
                        break;
                    case BcPlaneCodec::EndpointRingWindow:
                        EndpointRingWindowCompress(transformed.data(), transformed.size(), spec.element_bytes, ring_buf);
                        consider(tr, BcPlaneCodec::EndpointRingWindow, ring_buf);
                        break;
                    case BcPlaneCodec::SelectorIndexTable:
                        SelectorIndexTableCompress(transformed.data(), transformed.size(), spec.element_bytes, sel_buf);
                        consider(tr, BcPlaneCodec::SelectorIndexTable, sel_buf);
                        break;
                }
                continue;
            }

            // Option A: LZ-RLE
            LzRleCompress(transformed.data(), transformed.size(), lz_buf);
            consider(tr, BcPlaneCodec::LzRle, lz_buf);

            // Option B: Canonical Huffman directly on transformed plane
            HuffmanCompress(transformed.data(), transformed.size(), huff_buf);
            consider(tr, BcPlaneCodec::CanonicalHuffman, huff_buf);

            // Option C: LZ-RLE followed by Canonical Huffman
            if (!lz_buf.empty()) {
                HuffmanCompress(lz_buf.data(), lz_buf.size(), lz_huff_buf);
                consider(tr, BcPlaneCodec::LzRleHuffman, lz_huff_buf);
            }

            // Option D/E: Specialized BC1..BC7 Endpoint Ring-Window & Selector Index-Table codecs
            if (options.enable_specialized_codecs && tr == BcPlaneTransform::None) {
                if (spec.is_rgb565 || spec.element_bytes == 1u) {
                    EndpointRingWindowCompress(raw_plane.data(), raw_plane.size(), spec.element_bytes, ring_buf);
                    consider(BcPlaneTransform::None, BcPlaneCodec::EndpointRingWindow, ring_buf);
                }
                if (spec.is_selector) {
                    SelectorIndexTableCompress(raw_plane.data(), raw_plane.size(), spec.element_bytes, sel_buf);
                    consider(BcPlaneTransform::None, BcPlaneCodec::SelectorIndexTable, sel_buf);
                }
            }
        }

        if (!has_best) {
            best_transform = BcPlaneTransform::None;
            best_codec = BcPlaneCodec::Raw;
            best_payload = raw_plane;
        }

        descs[p].kind = spec.kind;
        descs[p].transform = best_transform;
        descs[p].codec = best_codec;
        descs[p].element_bytes = spec.element_bytes;
        descs[p].raw_size = static_cast<uint32_t>(raw_size);
        descs[p].compressed_size = static_cast<uint32_t>(best_payload.size());
        descs[p].plane_crc32 = BcPackCrc32(best_payload.data(), best_payload.size());
        payloads[p] = std::move(best_payload);
    }

    const size_t header_total = kHeaderFixedBytes + static_cast<size_t>(plane_count) * kPlaneDescBytes;
    uint64_t total_out = header_total;
    for (uint32_t p = 0; p < plane_count; ++p) {
        if (total_out > std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::OutOfMemory, "BCPack compressed stream exceeds 4 GiB");
        }
        descs[p].data_offset = static_cast<uint32_t>(total_out);
        total_out += payloads[p].size();
    }
    if (total_out > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::OutOfMemory, "BCPack compressed stream exceeds 4 GiB");
    }

    out_compressed.reserve(static_cast<size_t>(total_out));
    AppendLe32(out_compressed, kBcPackMagic);
    AppendLe16(out_compressed, kBcPackVersion);
    AppendLe16(out_compressed, 0u);
    AppendLe32(out_compressed, static_cast<uint32_t>(format));
    AppendLe32(out_compressed, width);
    AppendLe32(out_compressed, height);
    AppendLe32(out_compressed, block_count);
    AppendLe32(out_compressed, static_cast<uint32_t>(expected_size));
    AppendLe32(out_compressed, plane_count);
    AppendLe32(out_compressed, BcPackCrc32(src_blocks, src_size));

    for (uint32_t p = 0; p < plane_count; ++p) {
        const auto& d = descs[p];
        out_compressed.push_back(static_cast<uint8_t>(d.kind));
        out_compressed.push_back(static_cast<uint8_t>(d.transform));
        out_compressed.push_back(static_cast<uint8_t>(d.codec));
        out_compressed.push_back(d.element_bytes);
        AppendLe32(out_compressed, d.raw_size);
        AppendLe32(out_compressed, d.compressed_size);
        AppendLe32(out_compressed, d.data_offset);
        AppendLe32(out_compressed, d.plane_crc32);
    }

    for (uint32_t p = 0; p < plane_count; ++p) {
        out_compressed.insert(out_compressed.end(), payloads[p].begin(), payloads[p].end());
    }

    if (out_stats) {
        out_stats->original_size = static_cast<uint32_t>(expected_size);
        out_stats->compressed_size = static_cast<uint32_t>(out_compressed.size());
        out_stats->block_count = block_count;
        out_stats->plane_count = plane_count;
        out_stats->compression_ratio =
            static_cast<double>(out_compressed.size()) / static_cast<double>(expected_size);
    }

    return Status::Ok();
}

Status BcPackCompress(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src_blocks,
    size_t src_size,
    std::vector<uint8_t>& out_compressed,
    BcPackStats* out_stats) {
    BcPackCompressOptions default_opts{};
    return BcPackCompressWithOptions(format, width, height, src_blocks, src_size,
                                     default_opts, out_compressed, out_stats);
}

Status BcPackDecompressInto(
    const uint8_t* src,
    size_t src_size,
    uint8_t* dst_blocks,
    size_t dst_capacity,
    size_t* out_written,
    BcPackHeader* out_header) {
    if (out_written) {
        *out_written = 0;
    }

    BcPackHeader hdr{};
    Status st = BcPackParseHeader(src, src_size, hdr);
    if (!st.ok()) {
        return st;
    }

    if (!dst_blocks) {
        return Status::Error(StatusCode::InvalidArgument, "BCPack destination buffer is null");
    }
    if (dst_capacity < hdr.original_size) {
        return Status::Error(StatusCode::InvalidArgument, "BCPack destination buffer too small");
    }

    const uint32_t block_bytes = BcFormatBlockBytes(hdr.format);
    const auto specs = GetFormatPlanes(hdr.format);

    std::vector<uint8_t> staged_blocks(hdr.original_size, 0u);
    std::vector<uint8_t> decoded_stage1;
    std::vector<uint8_t> decoded_transformed;
    std::vector<uint8_t> raw_plane;

    for (uint32_t p = 0; p < hdr.plane_count; ++p) {
        const auto& d = hdr.planes[p];
        const uint8_t* payload = src + d.data_offset;

        const uint32_t actual_plane_crc = BcPackCrc32(payload, d.compressed_size);
        if (actual_plane_crc != d.plane_crc32) {
            return Status::Error(StatusCode::ParseError, "BCPack plane CRC32 mismatch");
        }

        switch (d.codec) {
            case BcPlaneCodec::Raw: {
                if (d.compressed_size != d.raw_size) {
                    return Status::Error(StatusCode::ParseError, "BCPack raw plane size mismatch");
                }
                decoded_transformed.assign(payload, payload + d.compressed_size);
                break;
            }
            case BcPlaneCodec::LzRle: {
                st = LzRleDecompress(payload, d.compressed_size, d.raw_size, decoded_transformed);
                if (!st.ok()) {
                    return st;
                }
                break;
            }
            case BcPlaneCodec::CanonicalHuffman: {
                st = HuffmanDecompress(payload, d.compressed_size, d.raw_size, decoded_transformed);
                if (!st.ok()) {
                    return st;
                }
                break;
            }
            case BcPlaneCodec::LzRleHuffman: {
                if (d.compressed_size < 4u) {
                    return Status::Error(StatusCode::ParseError, "BCPack LZ-Huffman header truncated");
                }
                const uint32_t lz_len = ReadLe32(payload);
                const uint64_t max_lz = static_cast<uint64_t>(d.raw_size) +
                                        static_cast<uint64_t>(d.raw_size / 64u) + 1024ull;
                if ((lz_len == 0 && d.raw_size > 0) || static_cast<uint64_t>(lz_len) > max_lz) {
                    return Status::Error(StatusCode::ParseError, "BCPack LZ-Huffman intermediate size invalid");
                }
                st = HuffmanDecompress(payload, d.compressed_size, lz_len, decoded_stage1);
                if (!st.ok()) {
                    return st;
                }
                st = LzRleDecompress(decoded_stage1.data(), decoded_stage1.size(), d.raw_size, decoded_transformed);
                if (!st.ok()) {
                    return st;
                }
                break;
            }
            case BcPlaneCodec::EndpointRingWindow: {
                st = EndpointRingWindowDecompress(payload, d.compressed_size, d.raw_size,
                                                  d.element_bytes, decoded_transformed);
                if (!st.ok()) {
                    return st;
                }
                break;
            }
            case BcPlaneCodec::SelectorIndexTable: {
                st = SelectorIndexTableDecompress(payload, d.compressed_size, d.raw_size,
                                                  d.element_bytes, decoded_transformed);
                if (!st.ok()) {
                    return st;
                }
                break;
            }
            default:
                return Status::Error(StatusCode::UnsupportedFormat, "Unknown BCPack plane codec");
        }

        if (decoded_transformed.size() != d.raw_size) {
            return Status::Error(StatusCode::ParseError, "BCPack decoded plane size mismatch");
        }

        st = InverseTransform(decoded_transformed, d.element_bytes, d.transform, raw_plane);
        if (!st.ok()) {
            return st;
        }
        if (raw_plane.size() != d.raw_size) {
            return Status::Error(StatusCode::ParseError, "BCPack inverse-transformed plane size mismatch");
        }

        const PlaneSpec& spec = specs[p];
        for (uint32_t b = 0; b < hdr.block_count; ++b) {
            uint8_t* dst_blk = staged_blocks.data() + static_cast<size_t>(b) * block_bytes + spec.block_offset;
            std::memcpy(dst_blk,
                        raw_plane.data() + static_cast<size_t>(b) * spec.element_bytes,
                        spec.element_bytes);
        }
    }

    const uint32_t actual_crc = BcPackCrc32(staged_blocks.data(), staged_blocks.size());
    if (actual_crc != hdr.uncompressed_crc32) {
        return Status::Error(StatusCode::ParseError, "BCPack uncompressed surface CRC32 mismatch");
    }

    std::memcpy(dst_blocks, staged_blocks.data(), staged_blocks.size());
    if (out_written) {
        *out_written = staged_blocks.size();
    }
    if (out_header) {
        *out_header = std::move(hdr);
    }
    return Status::Ok();
}

Status BcPackDecompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_blocks,
    BcPackHeader* out_header) {
    out_blocks.clear();
    BcPackHeader hdr{};
    Status st = BcPackParseHeader(src, src_size, hdr);
    if (!st.ok()) {
        return st;
    }

    out_blocks.resize(hdr.original_size);
    size_t written = 0;
    st = BcPackDecompressInto(src, src_size, out_blocks.data(), out_blocks.size(), &written, out_header);
    if (!st.ok()) {
        out_blocks.clear();
        return st;
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Multi-Mip BCPack Chain Container
// ---------------------------------------------------------------------------
Status BcPackCompressMipChain(
    BcFormat format,
    uint32_t base_width,
    uint32_t base_height,
    const std::vector<std::vector<uint8_t>>& mip_surfaces,
    std::vector<uint8_t>& out_compressed) {
    out_compressed.clear();

    const uint32_t max_mips = BcCalculateMaxMipLevels(base_width, base_height);
    if (BcFormatBlockBytes(format) == 0 || max_mips == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BC format or base dimensions");
    }
    if (mip_surfaces.empty() || mip_surfaces.size() > max_mips || mip_surfaces.size() > kBcPackMaxMipLevels) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid mip count for surface dimensions");
    }

    const uint32_t mip_count = static_cast<uint32_t>(mip_surfaces.size());
    std::vector<std::vector<uint8_t>> packed_mips(mip_count);
    for (uint32_t m = 0; m < mip_count; ++m) {
        BcPackMipLevelInfo info{};
        if (!BcCalculateMipLevelInfo(format, base_width, base_height, m, info)) {
            return Status::Error(StatusCode::InvalidArgument, "Failed calculating mip level geometry");
        }
        if (mip_surfaces[m].size() != info.byte_size) {
            return Status::Error(StatusCode::InvalidArgument, "Mip surface byte size mismatch");
        }
        Status st = BcPackCompress(format, info.width, info.height,
                                   mip_surfaces[m].data(), mip_surfaces[m].size(), packed_mips[m]);
        if (!st.ok()) {
            return st;
        }
    }

    const size_t table_end = kMipChainHeaderFixedBytes + static_cast<size_t>(mip_count) * kMipChainEntryBytes;
    uint64_t cur_offset = table_end;
    std::vector<uint32_t> offsets(mip_count);
    std::vector<uint32_t> sizes(mip_count);
    for (uint32_t m = 0; m < mip_count; ++m) {
        if (cur_offset > std::numeric_limits<uint32_t>::max() ||
            packed_mips[m].size() > std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::OutOfMemory, "Mip chain exceeds 4 GiB");
        }
        offsets[m] = static_cast<uint32_t>(cur_offset);
        sizes[m] = static_cast<uint32_t>(packed_mips[m].size());
        cur_offset += packed_mips[m].size();
    }
    if (cur_offset > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::OutOfMemory, "Mip chain total size exceeds 4 GiB");
    }

    out_compressed.reserve(static_cast<size_t>(cur_offset));
    AppendLe32(out_compressed, kBcPackMipChainMagic);
    AppendLe16(out_compressed, kBcPackVersion);
    AppendLe16(out_compressed, 0u);
    AppendLe32(out_compressed, static_cast<uint32_t>(format));
    AppendLe32(out_compressed, base_width);
    AppendLe32(out_compressed, base_height);
    AppendLe32(out_compressed, mip_count);

    for (uint32_t m = 0; m < mip_count; ++m) {
        AppendLe32(out_compressed, offsets[m]);
        AppendLe32(out_compressed, sizes[m]);
    }
    for (uint32_t m = 0; m < mip_count; ++m) {
        out_compressed.insert(out_compressed.end(), packed_mips[m].begin(), packed_mips[m].end());
    }
    return Status::Ok();
}

Status BcPackParseMipChainHeader(
    const uint8_t* src,
    size_t src_size,
    BcPackMipChainHeader& out_header) {
    out_header = BcPackMipChainHeader{};
    if (!src || src_size < kMipChainHeaderFixedBytes) {
        return Status::Error(StatusCode::ParseError, "BCPack mip-chain stream too small");
    }
    out_header.magic = ReadLe32(src + 0);
    if (out_header.magic != kBcPackMipChainMagic) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack mip-chain magic");
    }
    out_header.version = ReadLe16(src + 4);
    if (out_header.version != kBcPackVersion) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BCPack mip-chain version");
    }
    out_header.flags = ReadLe16(src + 6);
    if (out_header.flags != 0u) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BCPack mip-chain flags");
    }
    out_header.format = static_cast<BcFormat>(ReadLe32(src + 8));
    out_header.base_width = ReadLe32(src + 12);
    out_header.base_height = ReadLe32(src + 16);
    out_header.mip_count = ReadLe32(src + 20);

    const uint32_t max_mips = BcCalculateMaxMipLevels(out_header.base_width, out_header.base_height);
    if (BcFormatBlockBytes(out_header.format) == 0 || max_mips == 0) {
        return Status::Error(StatusCode::ParseError, "Invalid format or base dimensions in mip chain");
    }
    if (out_header.mip_count == 0 || out_header.mip_count > max_mips ||
        out_header.mip_count > kBcPackMaxMipLevels) {
        return Status::Error(StatusCode::ParseError, "Invalid mip count in mip-chain header");
    }

    const size_t table_end = kMipChainHeaderFixedBytes +
                             static_cast<size_t>(out_header.mip_count) * kMipChainEntryBytes;
    if (table_end > src_size) {
        return Status::Error(StatusCode::ParseError, "Mip-chain table truncated");
    }

    out_header.mip_offsets.resize(out_header.mip_count);
    out_header.mip_compressed_sizes.resize(out_header.mip_count);
    for (uint32_t m = 0; m < out_header.mip_count; ++m) {
        const uint8_t* e = src + kMipChainHeaderFixedBytes + static_cast<size_t>(m) * kMipChainEntryBytes;
        const uint32_t off = ReadLe32(e + 0);
        const uint32_t sz = ReadLe32(e + 4);
        if (sz == 0 || off < table_end ||
            static_cast<uint64_t>(off) + static_cast<uint64_t>(sz) > src_size) {
            return Status::Error(StatusCode::ParseError, "Mip-chain payload entry out of bounds");
        }
        out_header.mip_offsets[m] = off;
        out_header.mip_compressed_sizes[m] = sz;
    }

    for (uint32_t i = 0; i < out_header.mip_count; ++i) {
        const uint64_t a0 = out_header.mip_offsets[i];
        const uint64_t a1 = a0 + out_header.mip_compressed_sizes[i];
        for (uint32_t j = i + 1u; j < out_header.mip_count; ++j) {
            const uint64_t b0 = out_header.mip_offsets[j];
            const uint64_t b1 = b0 + out_header.mip_compressed_sizes[j];
            if (a0 < b1 && b0 < a1) {
                return Status::Error(StatusCode::ParseError, "Mip-chain payload entries overlap");
            }
        }
    }

    return Status::Ok();
}

Status BcPackDecompressMipChain(
    const uint8_t* src,
    size_t src_size,
    std::vector<std::vector<uint8_t>>& out_mip_surfaces,
    BcPackMipChainHeader* out_header) {
    out_mip_surfaces.clear();
    BcPackMipChainHeader hdr{};
    Status st = BcPackParseMipChainHeader(src, src_size, hdr);
    if (!st.ok()) {
        return st;
    }

    out_mip_surfaces.resize(hdr.mip_count);
    for (uint32_t m = 0; m < hdr.mip_count; ++m) {
        BcPackMipLevelInfo expected{};
        if (!BcCalculateMipLevelInfo(hdr.format, hdr.base_width, hdr.base_height, m, expected)) {
            out_mip_surfaces.clear();
            return Status::Error(StatusCode::ParseError, "Invalid mip geometry during decompression");
        }

        BcPackHeader mip_hdr{};
        st = BcPackDecompress(src + hdr.mip_offsets[m], hdr.mip_compressed_sizes[m],
                              out_mip_surfaces[m], &mip_hdr);
        if (!st.ok()) {
            out_mip_surfaces.clear();
            return st;
        }
        if (mip_hdr.format != hdr.format ||
            mip_hdr.width != expected.width ||
            mip_hdr.height != expected.height) {
            out_mip_surfaces.clear();
            return Status::Error(StatusCode::ParseError, "Mip sub-stream dimensions/format mismatch");
        }
    }

    if (out_header) {
        *out_header = std::move(hdr);
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Texture-Array + Multi-Mip BCPack Container ('BCPA') & Stream Validation
// ---------------------------------------------------------------------------
namespace {
constexpr size_t kTextureArrayHeaderFixedBytes = 28u;
constexpr size_t kTextureArrayEntryBytes = 8u;
}  // namespace

Status BcPackValidateStream(const uint8_t* src, size_t src_size) noexcept {
    if (!src || src_size < 4u) {
        return Status::Error(StatusCode::ParseError, "BCPack stream null or smaller than magic");
    }
    const uint32_t magic = ReadLe32(src);
    if (magic == kBcPackMagic) {
        BcPackHeader hdr{};
        return BcPackParseHeader(src, src_size, hdr);
    }
    if (magic == kBcPackMipChainMagic) {
        BcPackMipChainHeader mhdr{};
        return BcPackParseMipChainHeader(src, src_size, mhdr);
    }
    if (magic == kBcPackTextureArrayMagic) {
        BcPackTextureArrayHeader ahdr{};
        return BcPackParseTextureArrayHeader(src, src_size, ahdr);
    }
    return Status::Error(StatusCode::ParseError, "Unknown BCPack container magic");
}

Status BcPackCompressTextureArray(
    BcFormat format,
    uint32_t base_width,
    uint32_t base_height,
    uint32_t mip_count,
    uint32_t array_size,
    const std::vector<std::vector<uint8_t>>& subresources,
    std::vector<uint8_t>& out_compressed) {
    out_compressed.clear();

    const uint32_t max_mips = BcCalculateMaxMipLevels(base_width, base_height);
    if (BcFormatBlockBytes(format) == 0u || max_mips == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BC format or base dimensions for texture array");
    }
    if (mip_count == 0u || mip_count > max_mips || mip_count > kBcPackMaxMipLevels) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid mip_count for texture array");
    }
    if (array_size == 0u || array_size > kBcPackMaxArraySlices) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid array_size for texture array");
    }
    const uint64_t total_subresources = static_cast<uint64_t>(array_size) * static_cast<uint64_t>(mip_count);
    if (total_subresources == 0ull || subresources.size() != total_subresources) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Texture array subresource vector size must equal array_size * mip_count");
    }

    std::vector<std::vector<uint8_t>> packed(static_cast<size_t>(total_subresources));
    for (uint32_t slice = 0u; slice < array_size; ++slice) {
        for (uint32_t mip = 0u; mip < mip_count; ++mip) {
            BcPackMipLevelInfo info{};
            if (!BcCalculateMipLevelInfo(format, base_width, base_height, mip, info)) {
                return Status::Error(StatusCode::InvalidArgument, "Failed calculating texture array mip geometry");
            }
            const size_t sub_idx = static_cast<size_t>(slice) * mip_count + mip;
            if (subresources[sub_idx].size() != info.byte_size) {
                return Status::Error(StatusCode::InvalidArgument, "Texture array subresource byte size mismatch");
            }
            Status st = BcPackCompress(
                format,
                info.width,
                info.height,
                subresources[sub_idx].data(),
                subresources[sub_idx].size(),
                packed[sub_idx]);
            if (!st.ok()) {
                return st;
            }
        }
    }

    const uint64_t table_end =
        static_cast<uint64_t>(kTextureArrayHeaderFixedBytes) +
        total_subresources * static_cast<uint64_t>(kTextureArrayEntryBytes);
    uint64_t cur_offset = table_end;
    std::vector<uint32_t> offsets(static_cast<size_t>(total_subresources));
    std::vector<uint32_t> sizes(static_cast<size_t>(total_subresources));
    for (size_t i = 0u; i < packed.size(); ++i) {
        if (cur_offset > std::numeric_limits<uint32_t>::max() ||
            packed[i].size() > std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::OutOfMemory, "BCPack texture array exceeds 4 GiB");
        }
        offsets[i] = static_cast<uint32_t>(cur_offset);
        sizes[i] = static_cast<uint32_t>(packed[i].size());
        cur_offset += packed[i].size();
    }
    if (cur_offset > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::OutOfMemory, "BCPack texture array total size exceeds 4 GiB");
    }

    out_compressed.reserve(static_cast<size_t>(cur_offset));
    AppendLe32(out_compressed, kBcPackTextureArrayMagic);
    AppendLe16(out_compressed, kBcPackVersion);
    AppendLe16(out_compressed, 0u);
    AppendLe32(out_compressed, static_cast<uint32_t>(format));
    AppendLe32(out_compressed, base_width);
    AppendLe32(out_compressed, base_height);
    AppendLe32(out_compressed, mip_count);
    AppendLe32(out_compressed, array_size);

    for (size_t i = 0u; i < packed.size(); ++i) {
        AppendLe32(out_compressed, offsets[i]);
        AppendLe32(out_compressed, sizes[i]);
    }
    for (size_t i = 0u; i < packed.size(); ++i) {
        out_compressed.insert(out_compressed.end(), packed[i].begin(), packed[i].end());
    }
    return Status::Ok();
}

Status BcPackParseTextureArrayHeader(
    const uint8_t* src,
    size_t src_size,
    BcPackTextureArrayHeader& out_header) {
    out_header = BcPackTextureArrayHeader{};
    if (!src || src_size < kTextureArrayHeaderFixedBytes) {
        return Status::Error(StatusCode::ParseError, "BCPack texture-array stream too small");
    }
    out_header.magic = ReadLe32(src + 0u);
    if (out_header.magic != kBcPackTextureArrayMagic) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack texture-array magic");
    }
    out_header.version = ReadLe16(src + 4u);
    if (out_header.version != kBcPackVersion) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BCPack texture-array version");
    }
    out_header.flags = ReadLe16(src + 6u);
    if (out_header.flags != 0u) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported non-zero texture-array flags");
    }
    out_header.format = static_cast<BcFormat>(ReadLe32(src + 8u));
    out_header.base_width = ReadLe32(src + 12u);
    out_header.base_height = ReadLe32(src + 16u);
    out_header.mip_count = ReadLe32(src + 20u);
    out_header.array_size = ReadLe32(src + 24u);

    const uint32_t max_mips = BcCalculateMaxMipLevels(out_header.base_width, out_header.base_height);
    if (BcFormatBlockBytes(out_header.format) == 0u || max_mips == 0u) {
        return Status::Error(StatusCode::ParseError, "Invalid format or base dimensions in texture array");
    }
    if (out_header.mip_count == 0u || out_header.mip_count > max_mips ||
        out_header.mip_count > kBcPackMaxMipLevels) {
        return Status::Error(StatusCode::ParseError, "Invalid mip_count in texture-array header");
    }
    if (out_header.array_size == 0u || out_header.array_size > kBcPackMaxArraySlices) {
        return Status::Error(StatusCode::ParseError, "Invalid array_size in texture-array header");
    }

    const uint64_t total_subresources =
        static_cast<uint64_t>(out_header.array_size) * static_cast<uint64_t>(out_header.mip_count);
    const uint64_t table_end =
        static_cast<uint64_t>(kTextureArrayHeaderFixedBytes) +
        total_subresources * static_cast<uint64_t>(kTextureArrayEntryBytes);
    if (table_end > static_cast<uint64_t>(src_size)) {
        return Status::Error(StatusCode::ParseError, "Texture-array offset table truncated");
    }

    const size_t count = static_cast<size_t>(total_subresources);
    out_header.subresource_offsets.resize(count);
    out_header.subresource_compressed_sizes.resize(count);
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    intervals.reserve(count);

    for (size_t i = 0u; i < count; ++i) {
        const uint8_t* e = src + kTextureArrayHeaderFixedBytes + i * kTextureArrayEntryBytes;
        const uint32_t off = ReadLe32(e + 0u);
        const uint32_t sz = ReadLe32(e + 4u);
        if (sz == 0u || static_cast<uint64_t>(off) < table_end ||
            static_cast<uint64_t>(off) + static_cast<uint64_t>(sz) > static_cast<uint64_t>(src_size)) {
            return Status::Error(StatusCode::ParseError, "Texture-array subresource range out of bounds");
        }
        out_header.subresource_offsets[i] = off;
        out_header.subresource_compressed_sizes[i] = sz;
        intervals.push_back({static_cast<uint64_t>(off), static_cast<uint64_t>(off) + static_cast<uint64_t>(sz)});
    }

    // O(N log N) overlap check to prevent aliasing/zip-bomb streams even for large arrays.
    std::sort(intervals.begin(), intervals.end());
    for (size_t i = 1u; i < intervals.size(); ++i) {
        if (intervals[i].first < intervals[i - 1u].second) {
            return Status::Error(StatusCode::ParseError, "Texture-array subresource ranges overlap");
        }
    }

    return Status::Ok();
}

Status BcPackDecompressTextureArray(
    const uint8_t* src,
    size_t src_size,
    std::vector<std::vector<uint8_t>>& out_subresources,
    BcPackTextureArrayHeader* out_header) {
    out_subresources.clear();
    BcPackTextureArrayHeader hdr{};
    Status st = BcPackParseTextureArrayHeader(src, src_size, hdr);
    if (!st.ok()) {
        return st;
    }

    const size_t count = static_cast<size_t>(hdr.array_size) * static_cast<size_t>(hdr.mip_count);
    out_subresources.resize(count);
    for (uint32_t slice = 0u; slice < hdr.array_size; ++slice) {
        for (uint32_t mip = 0u; mip < hdr.mip_count; ++mip) {
            BcPackMipLevelInfo expected{};
            if (!BcCalculateMipLevelInfo(hdr.format, hdr.base_width, hdr.base_height, mip, expected)) {
                out_subresources.clear();
                return Status::Error(StatusCode::ParseError, "Invalid mip geometry during texture-array decode");
            }
            const size_t idx = static_cast<size_t>(slice) * hdr.mip_count + mip;
            BcPackHeader sub_hdr{};
            st = BcPackDecompress(
                src + hdr.subresource_offsets[idx],
                hdr.subresource_compressed_sizes[idx],
                out_subresources[idx],
                &sub_hdr);
            if (!st.ok()) {
                out_subresources.clear();
                return st;
            }
            if (sub_hdr.format != hdr.format ||
                sub_hdr.width != expected.width ||
                sub_hdr.height != expected.height) {
                out_subresources.clear();
                return Status::Error(StatusCode::ParseError, "Texture-array subresource format/dimensions mismatch");
            }
        }
    }

    if (out_header) {
        *out_header = std::move(hdr);
    }
    return Status::Ok();
}

Status BcPackDecompressTextureArraySubresource(
    const uint8_t* src,
    size_t src_size,
    uint32_t array_slice,
    uint32_t mip_level,
    std::vector<uint8_t>& out_blocks) {
    out_blocks.clear();
    BcPackTextureArrayHeader hdr{};
    Status st = BcPackParseTextureArrayHeader(src, src_size, hdr);
    if (!st.ok()) {
        return st;
    }
    if (array_slice >= hdr.array_size || mip_level >= hdr.mip_count) {
        return Status::Error(StatusCode::InvalidArgument, "Texture-array slice or mip out of bounds");
    }
    BcPackMipLevelInfo expected{};
    if (!BcCalculateMipLevelInfo(hdr.format, hdr.base_width, hdr.base_height, mip_level, expected)) {
        return Status::Error(StatusCode::ParseError, "Invalid mip geometry for subresource decode");
    }
    const size_t idx = static_cast<size_t>(array_slice) * hdr.mip_count + mip_level;
    BcPackHeader sub_hdr{};
    st = BcPackDecompress(
        src + hdr.subresource_offsets[idx],
        hdr.subresource_compressed_sizes[idx],
        out_blocks,
        &sub_hdr);
    if (!st.ok()) {
        out_blocks.clear();
        return st;
    }
    if (sub_hdr.format != hdr.format ||
        sub_hdr.width != expected.width ||
        sub_hdr.height != expected.height) {
        out_blocks.clear();
        return Status::Error(StatusCode::ParseError, "Texture-array subresource format/dimensions mismatch");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Synthetic BC1..BC7 Surface Generator (for Round-Trip Verification)
// ---------------------------------------------------------------------------
Status BcGenerateSyntheticSurface(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t seed,
    std::vector<uint8_t>& out_blocks) {
    out_blocks.clear();
    const uint32_t block_bytes = BcFormatBlockBytes(format);
    const uint64_t total_bytes = BcCalculateSurfaceSize(format, width, height);
    if (block_bytes == 0 || total_bytes == 0 || total_bytes > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BC format or dimensions for test surface");
    }

    const uint32_t bw = static_cast<uint32_t>((static_cast<uint64_t>(width) + 3ull) / 4ull);
    const uint32_t bh = static_cast<uint32_t>((static_cast<uint64_t>(height) + 3ull) / 4ull);
    out_blocks.assign(static_cast<size_t>(total_bytes), 0u);

    auto make_rgb565 = [](uint32_t r, uint32_t g, uint32_t b) noexcept -> uint16_t {
        return static_cast<uint16_t>(((r & 31u) << 11u) | ((g & 63u) << 5u) | (b & 31u));
    };

    static constexpr std::array<uint32_t, 8> kSelectorPalette = {
        0x00000000u, 0x55555555u, 0xAAAAAAAAu, 0xFFFFFFFFu,
        0x0F0F0F0Fu, 0x33333333u, 0x1B1B1B1Bu, 0xE4E4E4E4u,
    };

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint32_t b = by * bw + bx;
            uint8_t* blk = out_blocks.data() + static_cast<size_t>(b) * block_bytes;

            const uint16_t c0 = make_rgb565((bx + seed) & 31u, (by * 2u + seed) & 63u, ((bx + by) + seed) & 31u);
            const uint16_t c1 = make_rgb565((bx + seed + 3u) & 31u, (by * 2u + seed + 5u) & 63u, ((bx + by) + seed + 1u) & 31u);
            const uint32_t sel = (b % 5u == 0u)
                                     ? (0x12345678u ^ (b * 2654435761u) ^ seed)
                                     : kSelectorPalette[(bx / 2u + by + seed) & 7u];

            switch (format) {
                case BcFormat::BC1: {
                    WriteLe16(blk + 0, c0);
                    WriteLe16(blk + 2, c1);
                    blk[4] = static_cast<uint8_t>(sel & 0xFFu);
                    blk[5] = static_cast<uint8_t>((sel >> 8u) & 0xFFu);
                    blk[6] = static_cast<uint8_t>((sel >> 16u) & 0xFFu);
                    blk[7] = static_cast<uint8_t>((sel >> 24u) & 0xFFu);
                    break;
                }
                case BcFormat::BC2: {
                    for (uint32_t i = 0; i < 8u; ++i) {
                        blk[i] = static_cast<uint8_t>(((by * 17u + i * 3u + seed) & 0x0Fu) |
                                                      (((bx * 11u + i * 5u + seed) & 0x0Fu) << 4u));
                    }
                    WriteLe16(blk + 8, c0);
                    WriteLe16(blk + 10, c1);
                    blk[12] = static_cast<uint8_t>(sel & 0xFFu);
                    blk[13] = static_cast<uint8_t>((sel >> 8u) & 0xFFu);
                    blk[14] = static_cast<uint8_t>((sel >> 16u) & 0xFFu);
                    blk[15] = static_cast<uint8_t>((sel >> 24u) & 0xFFu);
                    break;
                }
                case BcFormat::BC3: {
                    blk[0] = static_cast<uint8_t>(200u + ((bx + seed) & 31u));
                    blk[1] = static_cast<uint8_t>(40u + ((by + seed) & 31u));
                    for (uint32_t i = 0; i < 6u; ++i) {
                        blk[2u + i] = static_cast<uint8_t>((b * 3u + i * 19u + seed) & 0xFFu);
                    }
                    WriteLe16(blk + 8, c0);
                    WriteLe16(blk + 10, c1);
                    blk[12] = static_cast<uint8_t>(sel & 0xFFu);
                    blk[13] = static_cast<uint8_t>((sel >> 8u) & 0xFFu);
                    blk[14] = static_cast<uint8_t>((sel >> 16u) & 0xFFu);
                    blk[15] = static_cast<uint8_t>((sel >> 24u) & 0xFFu);
                    break;
                }
                case BcFormat::BC4: {
                    blk[0] = static_cast<uint8_t>((bx * 7u + seed) & 0xFFu);
                    blk[1] = static_cast<uint8_t>((by * 9u + 64u + seed) & 0xFFu);
                    for (uint32_t i = 0; i < 6u; ++i) {
                        blk[2u + i] = static_cast<uint8_t>(((bx + by) * 5u + i * 29u + seed) & 0xFFu);
                    }
                    break;
                }
                case BcFormat::BC5: {
                    blk[0] = static_cast<uint8_t>((bx * 7u + seed) & 0xFFu);
                    blk[1] = static_cast<uint8_t>((by * 9u + 64u + seed) & 0xFFu);
                    for (uint32_t i = 0; i < 6u; ++i) {
                        blk[2u + i] = static_cast<uint8_t>(((bx + by) * 5u + i * 29u + seed) & 0xFFu);
                    }
                    blk[8] = static_cast<uint8_t>((by * 11u + 32u + seed) & 0xFFu);
                    blk[9] = static_cast<uint8_t>((bx * 13u + 128u + seed) & 0xFFu);
                    for (uint32_t i = 0; i < 6u; ++i) {
                        blk[10u + i] = static_cast<uint8_t>(((bx * 3u + by * 7u) + i * 31u + seed) & 0xFFu);
                    }
                    break;
                }
                case BcFormat::BC6H: {
                    // Valid BC6H mode bits (e.g. mode 0x03 or 0x00) + smooth HDR endpoints + selectors
                    blk[0] = ((b & 1u) == 0u) ? 0x03u : 0x00u;
                    blk[1] = static_cast<uint8_t>((bx * 4u + seed) & 0xFFu);
                    for (uint32_t i = 2u; i < 12u; ++i) {
                        blk[i] = static_cast<uint8_t>((bx * 3u + by * 5u + i * 7u + seed) & 0xFFu);
                    }
                    blk[12] = static_cast<uint8_t>(sel & 0xFFu);
                    blk[13] = static_cast<uint8_t>((sel >> 8u) & 0xFFu);
                    blk[14] = static_cast<uint8_t>((sel >> 16u) & 0xFFu);
                    blk[15] = static_cast<uint8_t>((sel >> 24u) & 0xFFu);
                    break;
                }
                case BcFormat::BC7: {
                    // Valid BC7 mode 6 (0x40) or mode 5 (0x20) + coherent endpoints + selectors
                    blk[0] = ((by & 1u) == 0u) ? 0x40u : 0x20u;
                    blk[1] = static_cast<uint8_t>((bx * 6u + seed) & 0xFFu);
                    for (uint32_t i = 2u; i < 12u; ++i) {
                        blk[i] = static_cast<uint8_t>((bx * 2u + by * 4u + i * 11u + seed) & 0xFFu);
                    }
                    blk[12] = static_cast<uint8_t>(sel & 0xFFu);
                    blk[13] = static_cast<uint8_t>((sel >> 8u) & 0xFFu);
                    blk[14] = static_cast<uint8_t>((sel >> 16u) & 0xFFu);
                    blk[15] = static_cast<uint8_t>((sel >> 24u) & 0xFFu);
                    break;
                }
                default:
                    return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BC format");
            }
        }
    }

    return Status::Ok();
}

}  // namespace openscarlett
