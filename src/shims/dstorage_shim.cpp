#define OPENSCARLETT_BUILDING_DSTORAGE 1
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/kernel/vram_residency_manager.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/kernel/title_va_mirror.h"
#include "openscarlett/loader/vfs_mounts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <d3d12.h>
#  include <windows.h>

#  if defined(_MSC_VER)
#    define OS_UUIDOF(T) __uuidof(T)
#  elif defined(__CRT_UUID_DECL)
#    define OS_UUIDOF(T) __mingw_uuidof<T>()
#  else
#    define OS_UUIDOF(T) IID_##T
#  endif
#endif

namespace {

constexpr const char* kModName = "dstorage_xs.dll";

#if defined(_WIN32)
using PfnQueryTitlePhysicalPages = int32_t(__stdcall*)(
    const void*, uint64_t*, uint64_t*, uint32_t*);
using PfnXMemSize = size_t(__stdcall*)(void*, uint64_t);
using PfnMapTitleVirtualMemory = void*(__stdcall*)(
    void*, uint64_t, uint32_t, uint32_t, const uint64_t*);
using PfnUnmapTitleVirtualMemory = int32_t(__stdcall*)(void*, uint64_t);

constexpr uint32_t kKxMemGraphics = 0x10000000u;

bool IsWin32ProtectReadable(DWORD prot) {
    if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD base = prot & 0xFFu;
    return base == PAGE_READONLY ||
           base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

bool ValidateReadableRange(const void* ptr, size_t size) {
    if (!ptr || size == 0u) return false;
    uintptr_t cursor = reinterpret_cast<uintptr_t>(ptr);
    if (cursor + size < cursor) return false;
    size_t remaining = size;
    while (remaining > 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || !IsWin32ProtectReadable(mbi.Protect)) {
            return false;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor) return false;
        const size_t step = std::min(remaining, static_cast<size_t>(region_end - cursor));
        cursor += step;
        remaining -= step;
    }
    return true;
}

bool IsWin32ProtectWritable(DWORD prot) {
    if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD base = prot & 0xFFu;
    return base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}
#endif

struct DestinationClassification {
    bool is_physical_page = false;
    bool is_xmem = false;
    uint64_t physical_id = 0;
    uint64_t physical_mapping_size = 0;
};

int32_t ValidateAndClassifyDestination(
    void* dst_ptr,
    size_t dst_cap,
    DestinationClassification& out_class) {
    out_class = DestinationClassification{};
    if (!dst_ptr || dst_cap == 0u) {
        return OS_DSTORAGE_E_INVALIDARG;
    }

#if defined(_WIN32)
    uintptr_t cursor = reinterpret_cast<uintptr_t>(dst_ptr);
    if (cursor + dst_cap < cursor) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
    size_t remaining = dst_cap;
    while (remaining > 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (mbi.State != MEM_COMMIT || !IsWin32ProtectWritable(mbi.Protect)) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const size_t avail = static_cast<size_t>(region_end - cursor);
        const size_t step = std::min(remaining, avail);
        cursor += step;
        remaining -= step;
    }

    HMODULE h_kx = GetModuleHandleW(L"kernelx.dll");
    if (h_kx) {
        auto fn_query = reinterpret_cast<PfnQueryTitlePhysicalPages>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "QueryTitlePhysicalPages")));
        if (fn_query) {
            uint64_t phys_id = 0u;
            uint64_t map_size = 0u;
            uint32_t flags = 0u;
            if (fn_query(dst_ptr, &phys_id, &map_size, &flags) != 0) {
                out_class.is_physical_page = true;
                out_class.physical_id = phys_id;
                out_class.physical_mapping_size = map_size;
                if (map_size > 0u && dst_cap > map_size) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
            }
        }
        auto fn_xmem_size = reinterpret_cast<PfnXMemSize>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "XMemSize")));
        if (fn_xmem_size) {
            const size_t xsz = fn_xmem_size(dst_ptr, 0u);
            if (xsz > 0u) {
                out_class.is_xmem = true;
            }
        }
    }
#else
    (void)dst_ptr;
    (void)dst_cap;
#endif
    return OS_DSTORAGE_S_OK;
}

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const DStorageGuid* a, const DStorageGuid& b) {
    return a && std::memcmp(a, &b, sizeof(DStorageGuid)) == 0;
}

// ===========================================================================
// Global Custom Decompression Callback Registry & Priority Telemetry
// ===========================================================================
struct CustomDecompressorEntry {
    PfnOpenScarlettDStorageCustomDecompress callback = nullptr;
    void* user_context = nullptr;
};

struct GlobalDStorageRuntime {
    std::mutex custom_mutex;
    std::unordered_map<uint8_t, CustomDecompressorEntry> custom_decompressors;

    std::atomic<uint64_t> realtime_dispatched{0u};
    std::atomic<uint64_t> high_dispatched{0u};
    std::atomic<uint64_t> normal_dispatched{0u};
    std::atomic<uint64_t> low_dispatched{0u};
    std::atomic<uint64_t> custom_decompressed{0u};
    std::atomic<uint64_t> gpu_texture_uploads{0u};
    std::atomic<uint64_t> gpu_untiled_uploads{0u};

    // Sprint 14 X2 / IOCP / Hardware Decompression Stats
    std::atomic<uint64_t> total_requests_completed{0u};
    std::atomic<uint64_t> total_bytes_read{0u};
    std::atomic<uint64_t> total_iocp_async_reads{0u};
    std::atomic<uint64_t> total_over_64gib_reads{0u};
    std::atomic<uint64_t> active_iocp_reads{0u};
    std::atomic<uint64_t> max_concurrent_iocp_reads{0u};
    std::atomic<uint64_t> total_zlib_decompressions{0u};
    std::atomic<uint64_t> total_bcpack_decompressions{0u};
    std::atomic<uint64_t> total_swizzle_transforms{0u};
    std::atomic<uint64_t> total_physical_page_transfers{0u};

    void RecordIocpReadStart() {
        const uint64_t cur = active_iocp_reads.fetch_add(1u, std::memory_order_acq_rel) + 1u;
        uint64_t prev = max_concurrent_iocp_reads.load(std::memory_order_relaxed);
        while (cur > prev &&
               !max_concurrent_iocp_reads.compare_exchange_weak(
                   prev, cur, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        }
    }

    void RecordIocpReadEnd() {
        active_iocp_reads.fetch_sub(1u, std::memory_order_acq_rel);
    }

    bool LookupCustom(uint8_t format, CustomDecompressorEntry& out) {
        std::lock_guard<std::mutex> lk(custom_mutex);
        auto it = custom_decompressors.find(format);
        if (it == custom_decompressors.end() || !it->second.callback) {
            return false;
        }
        out = it->second;
        return true;
    }
};

GlobalDStorageRuntime& GetRuntime() {
    static GlobalDStorageRuntime rt;
    return rt;
}

// ===========================================================================
// Self-Contained MIT RFC 1950 (Zlib) + RFC 1951 (DEFLATE) Decoder
// Supports stored (BTYPE=0), fixed Huffman (BTYPE=1), and dynamic Huffman
// (BTYPE=2) blocks with strict bounds checking and Adler-32 verification.
// ===========================================================================
namespace zlib_rfc1950 {

constexpr uint32_t kMaxBits = 15u;
constexpr uint32_t kMaxLitLenSyms = 288u;
constexpr uint32_t kMaxDistSyms = 32u;

uint32_t ComputeAdler32(const uint8_t* data, size_t len) noexcept {
    constexpr uint32_t kModAdler = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    size_t i = 0u;
    while (i < len) {
        const size_t chunk = std::min<size_t>(len - i, 5552u);
        for (size_t k = 0u; k < chunk; ++k) {
            s1 += data[i + k];
            s2 += s1;
        }
        s1 %= kModAdler;
        s2 %= kModAdler;
        i += chunk;
    }
    return (s2 << 16u) | s1;
}

struct BitStream {
    const uint8_t* data = nullptr;
    size_t size = 0u;
    size_t byte_pos = 0u;
    uint64_t bit_buf = 0ull;
    uint32_t bit_cnt = 0u;

    bool EnsureBits(uint32_t n) noexcept {
        while (bit_cnt < n) {
            if (byte_pos >= size) return false;
            bit_buf |= (static_cast<uint64_t>(data[byte_pos++]) << bit_cnt);
            bit_cnt += 8u;
        }
        return true;
    }

    bool ReadBits(uint32_t n, uint32_t& out) noexcept {
        if (n == 0u) {
            out = 0u;
            return true;
        }
        if (n > 32u || !EnsureBits(n)) return false;
        out = static_cast<uint32_t>(bit_buf & ((1ull << n) - 1ull));
        bit_buf >>= n;
        bit_cnt -= n;
        return true;
    }

    void AlignToByte() noexcept {
        const uint32_t drop = bit_cnt & 7u;
        bit_buf >>= drop;
        bit_cnt -= drop;
    }

    // Rewinds buffered full bytes back into byte_pos so raw byte reads stay exact.
    void FlushBufferedFullBytes() noexcept {
        AlignToByte();
        while (bit_cnt >= 8u && byte_pos > 0u) {
            --byte_pos;
            bit_cnt -= 8u;
        }
        bit_buf = 0ull;
        bit_cnt = 0u;
    }
};

struct HuffTable {
    std::array<uint16_t, kMaxBits + 1u> count{};
    std::array<uint16_t, kMaxLitLenSyms> symbols{};
    uint32_t num_syms = 0u;

    bool Build(const uint8_t* lengths, uint32_t n, bool allow_empty) noexcept {
        count.fill(0u);
        num_syms = 0u;
        for (uint32_t i = 0u; i < n; ++i) {
            if (lengths[i] > kMaxBits) return false;
            if (lengths[i] > 0u) {
                ++count[lengths[i]];
                ++num_syms;
            }
        }
        if (num_syms == 0u) {
            return allow_empty;
        }
        // Check Kraft inequality (oversubscribed tree is invalid; single-symbol tree is allowed).
        int32_t left = 1;
        for (uint32_t bits = 1u; bits <= kMaxBits; ++bits) {
            left <<= 1;
            left -= static_cast<int32_t>(count[bits]);
            if (left < 0) return false;
        }
        if (left > 0 && num_syms > 1u) {
            // Incomplete tree with >1 symbol is invalid per RFC 1951.
            return false;
        }
        std::array<uint16_t, kMaxBits + 1u> offsets{};
        uint16_t off = 0u;
        for (uint32_t bits = 1u; bits <= kMaxBits; ++bits) {
            offsets[bits] = off;
            off = static_cast<uint16_t>(off + count[bits]);
        }
        for (uint32_t sym = 0u; sym < n; ++sym) {
            if (lengths[sym] > 0u) {
                symbols[offsets[lengths[sym]]++] = static_cast<uint16_t>(sym);
            }
        }
        return true;
    }

    bool Decode(BitStream& bs, uint32_t& out_sym) const noexcept {
        if (num_syms == 0u) return false;
        int32_t code = 0;
        int32_t first = 0;
        int32_t index = 0;
        for (uint32_t len = 1u; len <= kMaxBits; ++len) {
            uint32_t bit = 0u;
            if (!bs.ReadBits(1u, bit)) return false;
            code |= static_cast<int32_t>(bit);
            const int32_t cnt = static_cast<int32_t>(count[len]);
            if (code - cnt < first) {
                out_sym = symbols[static_cast<size_t>(index + (code - first))];
                return true;
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

bool BuildFixedTrees(HuffTable& litlen, HuffTable& dist) noexcept {
    std::array<uint8_t, kMaxLitLenSyms> ll{};
    for (uint32_t i = 0u; i <= 143u; ++i) ll[i] = 8u;
    for (uint32_t i = 144u; i <= 255u; ++i) ll[i] = 9u;
    for (uint32_t i = 256u; i <= 279u; ++i) ll[i] = 7u;
    for (uint32_t i = 280u; i <= 287u; ++i) ll[i] = 8u;
    std::array<uint8_t, kMaxDistSyms> dl{};
    dl.fill(5u);
    return litlen.Build(ll.data(), 288u, false) && dist.Build(dl.data(), 32u, false);
}

bool DecodeDynamicTrees(BitStream& bs, HuffTable& litlen, HuffTable& dist) noexcept {
    uint32_t hlit_raw = 0u, hdist_raw = 0u, hclen_raw = 0u;
    if (!bs.ReadBits(5u, hlit_raw) || !bs.ReadBits(5u, hdist_raw) || !bs.ReadBits(4u, hclen_raw)) {
        return false;
    }
    const uint32_t hlit = hlit_raw + 257u;
    const uint32_t hdist = hdist_raw + 1u;
    const uint32_t hclen = hclen_raw + 4u;
    if (hlit > 286u || hdist > 30u || hclen > 19u) {
        return false;
    }

    static constexpr std::array<uint8_t, 19> kOrder = {
        16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u};
    std::array<uint8_t, 19> cl_lengths{};
    for (uint32_t i = 0u; i < hclen; ++i) {
        uint32_t v = 0u;
        if (!bs.ReadBits(3u, v)) return false;
        cl_lengths[kOrder[i]] = static_cast<uint8_t>(v);
    }

    HuffTable cl_tree{};
    if (!cl_tree.Build(cl_lengths.data(), 19u, false)) {
        return false;
    }

    const uint32_t total = hlit + hdist;
    std::array<uint8_t, kMaxLitLenSyms + kMaxDistSyms> lengths{};
    uint32_t idx = 0u;
    while (idx < total) {
        uint32_t sym = 0u;
        if (!cl_tree.Decode(bs, sym)) return false;
        if (sym < 16u) {
            lengths[idx++] = static_cast<uint8_t>(sym);
        } else if (sym == 16u) {
            if (idx == 0u) return false;
            uint32_t extra = 0u;
            if (!bs.ReadBits(2u, extra)) return false;
            const uint32_t rep = 3u + extra;
            if (idx + rep > total) return false;
            const uint8_t prev = lengths[idx - 1u];
            for (uint32_t r = 0u; r < rep; ++r) lengths[idx++] = prev;
        } else if (sym == 17u) {
            uint32_t extra = 0u;
            if (!bs.ReadBits(3u, extra)) return false;
            const uint32_t rep = 3u + extra;
            if (idx + rep > total) return false;
            for (uint32_t r = 0u; r < rep; ++r) lengths[idx++] = 0u;
        } else if (sym == 18u) {
            uint32_t extra = 0u;
            if (!bs.ReadBits(7u, extra)) return false;
            const uint32_t rep = 11u + extra;
            if (idx + rep > total) return false;
            for (uint32_t r = 0u; r < rep; ++r) lengths[idx++] = 0u;
        } else {
            return false;
        }
    }

    if (lengths[256u] == 0u) {
        return false;  // End-of-block symbol must be present
    }
    return litlen.Build(lengths.data(), hlit, false) &&
           dist.Build(lengths.data() + hlit, hdist, true);
}

bool DecodeHuffmanBlock(
    BitStream& bs,
    const HuffTable& litlen,
    const HuffTable& dist,
    size_t max_out_size,
    std::vector<uint8_t>& out) {
    static constexpr std::array<uint16_t, 29> kLenBase = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<uint8_t, 29> kLenExtra = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::array<uint16_t, 30> kDistBase = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<uint8_t, 30> kDistExtra = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        uint32_t sym = 0u;
        if (!litlen.Decode(bs, sym)) return false;
        if (sym < 256u) {
            if (out.size() >= max_out_size) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256u) {
            return true;
        } else if (sym <= 285u) {
            const size_t lidx = sym - 257u;
            uint32_t lextra = 0u;
            if (!bs.ReadBits(kLenExtra[lidx], lextra)) return false;
            const size_t match_len = static_cast<size_t>(kLenBase[lidx]) + lextra;

            uint32_t dsym = 0u;
            if (!dist.Decode(bs, dsym) || dsym >= 30u) return false;
            uint32_t dextra = 0u;
            if (!bs.ReadBits(kDistExtra[dsym], dextra)) return false;
            const size_t distance = static_cast<size_t>(kDistBase[dsym]) + dextra;

            if (distance == 0u || distance > out.size() || match_len > max_out_size - out.size()) {
                return false;
            }
            const size_t copy_start = out.size() - distance;
            for (size_t i = 0u; i < match_len; ++i) {
                out.push_back(out[copy_start + i]);
            }
        } else {
            return false;
        }
    }
}

bool Decompress(
    const uint8_t* src,
    size_t src_size,
    size_t max_out_size,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!src || src_size < 6u) return false;

    // RFC 1950 header validation
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    const uint32_t cm = cmf & 0x0Fu;
    const uint32_t cinfo = (cmf >> 4u) & 0x0Fu;
    const uint32_t fdict = (flg >> 5u) & 1u;
    const uint32_t fcheck = (static_cast<uint32_t>(cmf) << 8u) | static_cast<uint32_t>(flg);
    if (cm != 8u || cinfo > 7u || fdict != 0u || (fcheck % 31u) != 0u) {
        return false;
    }

    BitStream bs{};
    bs.data = src + 2u;
    bs.size = src_size - 6u;  // reserve final 4 bytes for big-endian Adler-32 trailer
    if (max_out_size > 0u && max_out_size <= 64u * 1024u * 1024u) {
        out.reserve(std::min<size_t>(max_out_size, 1024u * 1024u));
    }

    uint32_t bfinal = 0u;
    do {
        uint32_t btype = 0u;
        if (!bs.ReadBits(1u, bfinal) || !bs.ReadBits(2u, btype)) {
            out.clear();
            return false;
        }
        if (btype == 0u) {
            bs.FlushBufferedFullBytes();
            if (4u > bs.size - bs.byte_pos) {
                out.clear();
                return false;
            }
            const uint16_t len = static_cast<uint16_t>(bs.data[bs.byte_pos]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(bs.data[bs.byte_pos + 1u]) << 8u);
            const uint16_t nlen = static_cast<uint16_t>(bs.data[bs.byte_pos + 2u]) |
                                  static_cast<uint16_t>(static_cast<uint16_t>(bs.data[bs.byte_pos + 3u]) << 8u);
            bs.byte_pos += 4u;
            if ((len ^ 0xFFFFu) != nlen) {
                out.clear();
                return false;
            }
            if (len > bs.size - bs.byte_pos || len > max_out_size - out.size()) {
                out.clear();
                return false;
            }
            out.insert(out.end(), bs.data + bs.byte_pos, bs.data + bs.byte_pos + len);
            bs.byte_pos += len;
        } else if (btype == 1u) {
            HuffTable ll{}, dt{};
            if (!BuildFixedTrees(ll, dt) || !DecodeHuffmanBlock(bs, ll, dt, max_out_size, out)) {
                out.clear();
                return false;
            }
        } else if (btype == 2u) {
            HuffTable ll{}, dt{};
            if (!DecodeDynamicTrees(bs, ll, dt) || !DecodeHuffmanBlock(bs, ll, dt, max_out_size, out)) {
                out.clear();
                return false;
            }
        } else {
            out.clear();
            return false;
        }
    } while (bfinal == 0u);

    bs.FlushBufferedFullBytes();
    if (bs.byte_pos != bs.size) {
        // Unexpected trailing bytes before Adler-32 trailer.
        out.clear();
        return false;
    }

    const uint8_t* tr = src + (src_size - 4u);
    const uint32_t expected_adler =
        (static_cast<uint32_t>(tr[0]) << 24u) |
        (static_cast<uint32_t>(tr[1]) << 16u) |
        (static_cast<uint32_t>(tr[2]) << 8u) |
        static_cast<uint32_t>(tr[3]);
    const uint32_t actual_adler = ComputeAdler32(out.data(), out.size());
    if (actual_adler != expected_adler) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace zlib_rfc1950

// ===========================================================================
// Xbox Hardware 64 KiB DirectStorage Swizzle Helper (DSTORAGE_SWIZZLE_MODE)
// Bijective 16 x 4 KiB page permutation per 64 KiB tile.
// ===========================================================================
bool IsValidXboxSwizzleMode(uint64_t mode) noexcept {
    return mode <= static_cast<uint64_t>(DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K);
}

bool ApplyXboxSwizzle64KB(
    uint64_t swizzle_mode,
    const uint8_t* src,
    size_t size,
    std::vector<uint8_t>& out) {
    constexpr size_t kTileBytes = 65536u;
    constexpr size_t kPageBytes = 4096u;
    if (!src || size == 0u || (size % kTileBytes) != 0u) {
        return false;
    }
    static constexpr std::array<std::array<uint8_t, 16>, 4> kPerm = {{
        // 1: DSTORAGE_SWIZZLE_MODE_4K_4K_8K
        {0u, 1u, 4u, 5u, 2u, 3u, 6u, 7u, 8u, 9u, 12u, 13u, 10u, 11u, 14u, 15u},
        // 2: DSTORAGE_SWIZZLE_MODE_4K_4K_24K
        {0u, 8u, 1u, 9u, 2u, 3u, 4u, 5u, 6u, 7u, 10u, 11u, 12u, 13u, 14u, 15u},
        // 3: DSTORAGE_SWIZZLE_MODE_4K_12K_4K_4K_8K
        {0u, 4u, 5u, 6u, 1u, 2u, 8u, 9u, 3u, 10u, 11u, 12u, 7u, 13u, 14u, 15u},
        // 4: DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K
        {0u, 2u, 3u, 4u, 1u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 9u, 13u, 14u, 15u},
    }};
    if (swizzle_mode == 0u || swizzle_mode > 4u) {
        return false;
    }
    const auto& perm = kPerm[static_cast<size_t>(swizzle_mode - 1u)];
    out.resize(size);
    const size_t num_tiles = size / kTileBytes;
    for (size_t t = 0u; t < num_tiles; ++t) {
        const uint8_t* src_tile = src + t * kTileBytes;
        uint8_t* dst_tile = out.data() + t * kTileBytes;
        for (size_t p = 0u; p < 16u; ++p) {
            std::memcpy(dst_tile + static_cast<size_t>(perm[p]) * kPageBytes,
                        src_tile + p * kPageBytes,
                        kPageBytes);
        }
    }
    return true;
}

// ===========================================================================
// Physical Page Array Transfer (Options.DestinationIsPhysicalPages / Source)
// Routes cleanly through attached TitleVAMirror OR kernelx.dll physical pages.
// Strictly rejects invalid/unmapped page numbers with OS_DSTORAGE_E_INVALIDARG.
// ===========================================================================
int32_t TransferPhysicalPageArray(
    void* title_va_mirror_ptr,
    const uintptr_t* page_array,
    uint16_t page_offset,
    uint32_t byte_size,
    bool is_write,
    uint8_t* host_buf) {
    if (!page_array || !host_buf || byte_size == 0u) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
    constexpr uint64_t kPage64KB = 65536ull;
    const uint64_t total_span = static_cast<uint64_t>(page_offset) + static_cast<uint64_t>(byte_size);
    const uint64_t page_count = (total_span + kPage64KB - 1ull) / kPage64KB;
    if (page_count == 0ull || page_count > 65536ull) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
#if defined(_WIN32)
    if (!ValidateReadableRange(page_array, static_cast<size_t>(page_count) * sizeof(uintptr_t))) {
        return OS_DSTORAGE_E_INVALIDARG;
    }

    std::vector<uint64_t> pfns(static_cast<size_t>(page_count), 0ull);
    for (size_t i = 0u; i < pfns.size(); ++i) {
        pfns[i] = static_cast<uint64_t>(page_array[i]);
        if (pfns[i] == 0ull) {
            // Page 0 is never a valid Xbox physical page number.
            return OS_DSTORAGE_E_INVALIDARG;
        }
    }

    // Path 1: Resolve through attached openscarlett::TitleVAMirror if registered.
    if (title_va_mirror_ptr) {
        auto* mirror = static_cast<openscarlett::TitleVAMirror*>(title_va_mirror_ptr);
        std::vector<uint8_t*> page_ptrs(pfns.size(), nullptr);
        bool all_resolved = true;
        for (size_t i = 0u; i < pfns.size(); ++i) {
            void* cpu_ptr = nullptr;
            uint64_t rem = 0ull;
            // Support either 64 KiB page frame index (pfn << 16) or direct 64 KiB-aligned Title VA.
            const uint64_t va_from_pfn = pfns[i] << 16u;
            bool ok = mirror->ResolveTitleVaToCpu(va_from_pfn, cpu_ptr, rem) && rem >= kPage64KB;
            if (!ok && (pfns[i] & (kPage64KB - 1ull)) == 0ull) {
                ok = mirror->ResolveTitleVaToCpu(pfns[i], cpu_ptr, rem) && rem >= kPage64KB;
            }
            if (!ok || !cpu_ptr) {
                all_resolved = false;
                break;
            }
            page_ptrs[i] = static_cast<uint8_t*>(cpu_ptr);
        }
        if (all_resolved) {
            size_t copied = 0u;
            size_t cur_off = static_cast<size_t>(page_offset);
            for (size_t i = 0u; i < page_ptrs.size() && copied < byte_size; ++i) {
                const size_t avail = static_cast<size_t>(kPage64KB) - cur_off;
                const size_t step = std::min<size_t>(byte_size - copied, avail);
                if (is_write) {
                    std::memcpy(page_ptrs[i] + cur_off, host_buf + copied, step);
                } else {
                    std::memcpy(host_buf + copied, page_ptrs[i] + cur_off, step);
                }
                copied += step;
                cur_off = 0u;
            }
            return OS_DSTORAGE_S_OK;
        }
    }

    // Path 2: Resolve through kernelx.dll MapTitleVirtualMemory / UnmapTitleVirtualMemory.
    HMODULE h_kx = GetModuleHandleW(L"kernelx.dll");
    if (h_kx) {
        auto fn_map = reinterpret_cast<PfnMapTitleVirtualMemory>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "MapTitleVirtualMemory")));
        auto fn_unmap = reinterpret_cast<PfnUnmapTitleVirtualMemory>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "UnmapTitleVirtualMemory")));
        if (fn_map && fn_unmap) {
            const uint32_t prot = is_write ? PAGE_READWRITE : PAGE_READONLY;
            void* mapped = fn_map(nullptr, page_count, kKxMemGraphics, prot, pfns.data());
            if (mapped) {
                auto* base = static_cast<uint8_t*>(mapped) + page_offset;
                if (is_write) {
                    std::memcpy(base, host_buf, byte_size);
                } else {
                    std::memcpy(host_buf, base, byte_size);
                }
                fn_unmap(mapped, page_count);
                return OS_DSTORAGE_S_OK;
            }
        }
    }
    return OS_DSTORAGE_E_INVALIDARG;
#else
    (void)title_va_mirror_ptr;
    return OS_DSTORAGE_E_INVALIDARG;
#endif
}

size_t PriorityToBucket(DSTORAGE_PRIORITY p) noexcept {
    if (p >= DSTORAGE_PRIORITY_REALTIME) return 0u;
    if (p == DSTORAGE_PRIORITY_HIGH) return 1u;
    if (p == DSTORAGE_PRIORITY_NORMAL) return 2u;
    return 3u;  // DSTORAGE_PRIORITY_LOW
}

#if defined(_WIN32)
// ===========================================================================
// Win32 I/O Completion Port (IOCP) Async Overlapped File I/O Engine
// Ref-counted lifecycle: created on first open file, joined on last close
// (never hangs in DLL_PROCESS_DETACH).
// ===========================================================================
struct IocpReadOp {
    OVERLAPPED ov{};
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    DWORD bytes_transferred = 0u;
    DWORD error_code = ERROR_SUCCESS;
};

class DStorageIocpEngine {
public:
    static DStorageIocpEngine& Global() {
        static DStorageIocpEngine eng;
        return eng;
    }

    ~DStorageIocpEngine() {
        for (auto& t : threads_) {
            if (t.joinable()) t.detach();
        }
        if (iocp_) {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }
    }

    bool AttachFile(HANDLE hfile) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (active_files_ == 0u) {
            iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0u, 0u);
            if (!iocp_) return false;
            constexpr size_t kIocpThreads = 2u;
            threads_.reserve(kIocpThreads);
            for (size_t i = 0u; i < kIocpThreads; ++i) {
                threads_.emplace_back(&DStorageIocpEngine::CompletionLoop, this, iocp_);
            }
        }
        if (!CreateIoCompletionPort(hfile, iocp_, kFileIoKey, 0u)) {
            if (active_files_ == 0u) {
                ShutdownLocked();
            }
            return false;
        }
        ++active_files_;
        return true;
    }

    void DetachFile() {
        std::vector<std::thread> to_join;
        HANDLE to_close = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (active_files_ > 0u) {
                --active_files_;
            }
            if (active_files_ == 0u && iocp_) {
                for (size_t i = 0u; i < threads_.size(); ++i) {
                    PostQueuedCompletionStatus(iocp_, 0u, kShutdownKey, nullptr);
                }
                to_join.swap(threads_);
                to_close = iocp_;
                iocp_ = nullptr;
            }
        }
        for (auto& t : to_join) {
            if (t.joinable()) {
                if (t.get_id() != std::this_thread::get_id()) t.join();
                else t.detach();
            }
        }
        if (to_close) {
            CloseHandle(to_close);
        }
    }

private:
    static constexpr ULONG_PTR kFileIoKey = 0x5CA0F11Eu;
    static constexpr ULONG_PTR kShutdownKey = 0x5CA0DEADu;

    void ShutdownLocked() {
        if (!iocp_) return;
        for (size_t i = 0u; i < threads_.size(); ++i) {
            PostQueuedCompletionStatus(iocp_, 0u, kShutdownKey, nullptr);
        }
        for (auto& t : threads_) {
            if (t.joinable()) t.detach();
        }
        threads_.clear();
        CloseHandle(iocp_);
        iocp_ = nullptr;
    }

    void CompletionLoop(HANDLE port) {
        for (;;) {
            DWORD bytes = 0u;
            ULONG_PTR key = 0u;
            LPOVERLAPPED ov = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &ov, INFINITE);
            if (key == kShutdownKey && !ov) {
                return;
            }
            if (!ov) {
                if (!ok && GetLastError() == ERROR_ABANDONED_WAIT_0) {
                    return;
                }
                continue;
            }
            auto* op = reinterpret_cast<IocpReadOp*>(ov);
            const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            {
                std::lock_guard<std::mutex> lk(op->mutex);
                op->bytes_transferred = bytes;
                op->error_code = err;
                op->completed = true;
            }
            op->cv.notify_one();
        }
    }

    std::mutex mutex_;
    HANDLE iocp_ = nullptr;
    size_t active_files_ = 0u;
    std::vector<std::thread> threads_;
};
#endif

// ===========================================================================
// IDStorageFileX Implementation (Concurrent Positioned OVERLAPPED + IOCP I/O,
// 64-bit Offset/OffsetHigh > 64 GiB safe, lock-free multi-thread readers)
// ===========================================================================
class DStorageFileImpl final : public IDStorageFileX {
public:
    explicit DStorageFileImpl(
#if defined(_WIN32)
        HANDLE hfile,
        bool iocp_attached = false
#else
        void* hfile,
        bool iocp_attached = false
#endif
        )
        : file_handle_(hfile), iocp_attached_(iocp_attached) {}

    ~DStorageFileImpl() {
        CloseInternal();
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageFileX) ||
            GuidEquals(riid, IID_IDStorageFileX_Xbox)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFileX*>(this);
            return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageFileX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageFileX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        RecordOk("IDStorageFileX::Close", 3);
        CloseInternal();
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetFileInformation(DSTORAGE_FILE_INFO* info) override {
        if (!info) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        std::shared_lock<std::shared_mutex> lk(handle_rw_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
        }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file_handle_, &sz)) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
        }
        info->FileSize = static_cast<uint64_t>(sz.QuadPart);
        return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_S_OK, 4);
#else
        info->FileSize = 0;
        return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
#endif
    }

    int32_t ReadAt(
        uint64_t offset,
        uint32_t size,
        std::vector<uint8_t>& out,
        const std::atomic<bool>* cancel_flag = nullptr) {
        // Shared lock allows N parallel positioned OVERLAPPED reads on the SAME file handle!
        std::shared_lock<std::shared_mutex> lk(handle_rw_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }
        if (offset + static_cast<uint64_t>(size) < offset) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        LARGE_INTEGER file_sz{};
        if (!GetFileSizeEx(file_handle_, &file_sz) ||
            file_sz.QuadPart < 0 ||
            offset + static_cast<uint64_t>(size) > static_cast<uint64_t>(file_sz.QuadPart)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        out.resize(size);
        constexpr uint32_t kReadChunkBytes = 256u * 1024u;  // 256 KiB async overlapped chunk
        constexpr uint64_t k64GiB = 64ull * 1024ull * 1024ull * 1024ull;

        auto& rt = GetRuntime();
        rt.RecordIocpReadStart();
        struct ReadGuard {
            GlobalDStorageRuntime* r;
            ~ReadGuard() { r->RecordIocpReadEnd(); }
        } guard{&rt};

        uint32_t total_read = 0u;
        while (total_read < size) {
            if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
                out.clear();
                return OS_DSTORAGE_E_CANCELLED;
            }
            const uint64_t chunk_off = offset + static_cast<uint64_t>(total_read);
            const DWORD to_read = static_cast<DWORD>(std::min<uint32_t>(size - total_read, kReadChunkBytes));

            IocpReadOp op{};
            op.ov.Offset = static_cast<DWORD>(chunk_off & 0xFFFFFFFFull);
            op.ov.OffsetHigh = static_cast<DWORD>((chunk_off >> 32u) & 0xFFFFFFFFull);

            DWORD imm_bytes = 0u;
            const BOOL ok = ReadFile(file_handle_, out.data() + total_read, to_read, &imm_bytes, &op.ov);
            if (!ok) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    out.clear();
                    return OS_DSTORAGE_E_IO_ERROR;
                }
            }
            // Note: On a handle associated with an IOCP (without FILE_SKIP_COMPLETION_PORT_ON_SUCCESS),
            // Windows ALWAYS queues a completion packet even when ReadFile returns TRUE synchronously.
            if (iocp_attached_) {
                std::unique_lock<std::mutex> olk(op.mutex);
                while (!op.completed) {
                    if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
                        CancelIoEx(file_handle_, &op.ov);
                    }
                    op.cv.wait_for(olk, std::chrono::milliseconds(5), [&]() { return op.completed; });
                }
                if (op.error_code == ERROR_OPERATION_ABORTED) {
                    out.clear();
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (op.error_code != ERROR_SUCCESS || op.bytes_transferred == 0u) {
                    out.clear();
                    return OS_DSTORAGE_E_IO_ERROR;
                }
                total_read += op.bytes_transferred;
            } else {
                DWORD got = imm_bytes;
                if (!ok) {
                    if (!GetOverlappedResult(file_handle_, &op.ov, &got, TRUE) || got == 0u) {
                        out.clear();
                        return OS_DSTORAGE_E_IO_ERROR;
                    }
                } else if (got == 0u) {
                    out.clear();
                    return OS_DSTORAGE_E_IO_ERROR;
                }
                total_read += got;
            }
        }

        rt.total_iocp_async_reads.fetch_add(1u, std::memory_order_relaxed);
        rt.total_bytes_read.fetch_add(total_read, std::memory_order_relaxed);
        if (offset >= k64GiB) {
            rt.total_over_64gib_reads.fetch_add(1u, std::memory_order_relaxed);
        }
        return OS_DSTORAGE_S_OK;
#else
        (void)offset;
        (void)size;
        (void)out;
        (void)cancel_flag;
        return OS_DSTORAGE_E_IO_ERROR;
#endif
    }

private:
    void CloseInternal() {
        bool detach_iocp = false;
        {
            std::unique_lock<std::shared_mutex> lk(handle_rw_mutex_);
#if defined(_WIN32)
            if (file_handle_ != INVALID_HANDLE_VALUE && file_handle_ != nullptr) {
                CloseHandle(file_handle_);
                file_handle_ = INVALID_HANDLE_VALUE;
                detach_iocp = iocp_attached_;
                iocp_attached_ = false;
            }
#endif
        }
#if defined(_WIN32)
        if (detach_iocp) {
            DStorageIocpEngine::Global().DetachFile();
        }
#else
        (void)detach_iocp;
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    mutable std::shared_mutex handle_rw_mutex_;
#if defined(_WIN32)
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
#else
    void* file_handle_ = nullptr;
#endif
    bool iocp_attached_ = false;
};

// ===========================================================================
// IDStorageStatusArrayX Implementation
// ===========================================================================
class DStorageStatusArrayImpl final : public IDStorageStatusArrayX {
public:
    explicit DStorageStatusArrayImpl(uint32_t capacity)
        : complete_(capacity, true), results_(capacity, OS_DSTORAGE_S_OK) {}

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) {
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        }
        *ppvObject = nullptr;
        if (!riid) {
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        }
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageStatusArrayX) ||
            GuidEquals(riid, IID_IDStorageStatusArrayX_Xbox)) {
            AddRef();
            *ppvObject = static_cast<IDStorageStatusArrayX*>(this);
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageStatusArrayX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageStatusArrayX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    bool OPENSCARLETT_DSTORAGE_CALL IsComplete(uint32_t index) override {
        RecordOk("IDStorageStatusArrayX::IsComplete", 3);
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= complete_.size()) return false;
        return complete_[index];
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetHResult(uint32_t index) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= results_.size()) {
            return RecordHr("IDStorageStatusArrayX::GetHResult", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        const int32_t hr = complete_[index] ? results_[index] : OS_DSTORAGE_E_PENDING;
        return RecordHr("IDStorageStatusArrayX::GetHResult", hr, 4);
    }

    void MarkPending(uint32_t index) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            complete_[index] = false;
            results_[index] = OS_DSTORAGE_E_PENDING;
        }
    }

    void MarkComplete(uint32_t index, int32_t hr) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            results_[index] = hr;
            complete_[index] = true;
        }
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    std::vector<bool> complete_;
    std::vector<int32_t> results_;
};

#if defined(_WIN32)
// ===========================================================================
// Lightweight Per-Queue D3D12 Async Upload Ring + XgGpuUntiler Context
// ===========================================================================
struct DStorageGpuUploadContext {
    static constexpr size_t kUploadRingSlots = 4u;

    struct RingSlot {
        ID3D12Resource* buffer = nullptr;
        uint64_t capacity = 0ull;
    };

    std::mutex gpu_mutex;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* cmd_queue = nullptr;
    ID3D12CommandAllocator* cmd_alloc = nullptr;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0ull;
    std::array<RingSlot, kUploadRingSlots> upload_ring_{};
    size_t ring_cursor_ = 0u;
    openscarlett::XgGpuUntiler untiler;
    bool untiler_attempted = false;

    ~DStorageGpuUploadContext() {
        Reset();
    }

    void Reset() {
        untiler.Shutdown();
        untiler_attempted = false;
        for (auto& slot : upload_ring_) {
            if (slot.buffer) {
                slot.buffer->Release();
                slot.buffer = nullptr;
            }
            slot.capacity = 0ull;
        }
        ring_cursor_ = 0u;
        if (cmd_list) { cmd_list->Release(); cmd_list = nullptr; }
        if (cmd_alloc) { cmd_alloc->Release(); cmd_alloc = nullptr; }
        if (cmd_queue) { cmd_queue->Release(); cmd_queue = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        fence_value = 0ull;
    }

    ID3D12Resource* AcquireUploadRingBuffer(uint64_t required_bytes) {
        if (!device) return nullptr;
        const uint64_t alloc_bytes = std::max<uint64_t>(required_bytes, 65536ull);
        RingSlot& slot = upload_ring_[ring_cursor_];
        ring_cursor_ = (ring_cursor_ + 1u) % kUploadRingSlots;
        if (slot.buffer && slot.capacity >= alloc_bytes) {
            return slot.buffer;
        }
        if (slot.buffer) {
            slot.buffer->Release();
            slot.buffer = nullptr;
            slot.capacity = 0ull;
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CreationNodeMask = 1u;
        hp.VisibleNodeMask = 1u;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = alloc_bytes;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, OS_UUIDOF(ID3D12Resource), reinterpret_cast<void**>(&slot.buffer))) ||
            !slot.buffer) {
            slot.buffer = nullptr;
            return nullptr;
        }
        slot.capacity = alloc_bytes;
        return slot.buffer;
    }

    bool EnsureInitialized(ID3D12Device* dev) {
        if (!dev) return false;
        if (device == dev && cmd_queue && cmd_alloc && cmd_list && fence && fence_event) {
            return true;
        }
        Reset();
        dev->AddRef();
        device = dev;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, OS_UUIDOF(ID3D12CommandQueue),
                                           reinterpret_cast<void**>(&cmd_queue))) ||
            !cmd_queue) {
            Reset();
            return false;
        }
        if (FAILED(dev->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                OS_UUIDOF(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&cmd_alloc))) ||
            !cmd_alloc) {
            Reset();
            return false;
        }
        if (FAILED(dev->CreateCommandList(
                0u,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                cmd_alloc,
                nullptr,
                OS_UUIDOF(ID3D12GraphicsCommandList),
                reinterpret_cast<void**>(&cmd_list))) ||
            !cmd_list) {
            Reset();
            return false;
        }
        cmd_list->Close();

        if (FAILED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence),
                                    reinterpret_cast<void**>(&fence))) ||
            !fence) {
            Reset();
            return false;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            Reset();
            return false;
        }
        return true;
    }

    bool EnsureUntilerReady() {
        if (!device) return false;
        if (untiler.IsD3D12Ready()) return true;
        if (untiler_attempted) return false;
        untiler_attempted = true;
        return untiler.InitializeD3D12WithDevice(device).ok();
    }

    bool ExecuteAndWait() {
        if (FAILED(cmd_list->Close())) return false;
        ID3D12CommandList* lists[] = {cmd_list};
        cmd_queue->ExecuteCommandLists(1u, lists);
        const uint64_t wait_val = ++fence_value;
        if (FAILED(cmd_queue->Signal(fence, wait_val))) return false;
        if (fence->GetCompletedValue() < wait_val) {
            if (FAILED(fence->SetEventOnCompletion(wait_val, fence_event))) return false;
            if (WaitForSingleObject(fence_event, 30000u) != WAIT_OBJECT_0) return false;
        }
        return true;
    }
};
#endif

// Forward declaration for priority scheduler.
class DStorageQueueImpl;

// ===========================================================================
// Multi-Threaded Cross-Queue Priority Scheduler (REALTIME > HIGH > NORMAL > LOW)
// Dispatches concurrent IOCP file reads & parallel decompression across worker
// threads while strictly preserving per-queue Status/Event/Signal barriers and
// priority preemption. Ref-counted worker lifecycle guarantees zero hangs in
// DLL_PROCESS_DETACH.
// ===========================================================================
class DStoragePriorityScheduler {
public:
    static DStoragePriorityScheduler& Global() {
        static DStoragePriorityScheduler sched;
        return sched;
    }

    ~DStoragePriorityScheduler() {
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.detach();
            }
        }
    }

    void RegisterQueue(DStorageQueueImpl* q, DSTORAGE_PRIORITY priority);
    void UnregisterQueue(DStorageQueueImpl* q);
    void NotifyWorkAvailable();

private:
    struct Slot {
        DStorageQueueImpl* queue = nullptr;
        size_t bucket = 2u;
        uint32_t active_workers = 0u;
        bool removing = false;
    };

    void RunLoop();

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::vector<Slot> slots_;
    std::array<size_t, 4> rr_cursor_{0u, 0u, 0u, 0u};
    bool stop_ = false;
    bool worker_running_ = false;
    std::vector<std::thread> workers_;
};

// ===========================================================================
// IDStorageQueueX / X1 / X2 Implementation (Multi-Threaded IOCP + Real Xbox
// Zlib/BCPack/Swizzle/PhysicalPages + GDeflate/Custom + XgGpuUntiler Ring)
// ===========================================================================
class DStorageQueueImpl final : public IDStorageQueueX2 {
public:
    struct InFlightRequestState {
        uint64_t tag = 0ull;
        std::atomic<bool> canceled{false};
    };

    explicit DStorageQueueImpl(
        const DSTORAGE_QUEUE_DESC& desc,
        std::shared_ptr<std::atomic<uint32_t>> staging_size_ref = nullptr,
        uint32_t queue_options = DSTORAGE_QUEUE_OPTIONS_NONE)
        : desc_(desc),
          queue_options_(queue_options),
          auto_submit_disabled_((queue_options & DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT) != 0u),
          staging_size_ref_(staging_size_ref
                                ? std::move(staging_size_ref)
                                : std::make_shared<std::atomic<uint32_t>>(
                                      DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE)) {
        if (desc_.Capacity < DSTORAGE_MIN_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        } else if (desc_.Capacity > DSTORAGE_MAX_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        }
        auto_submit_threshold_ =
            std::max<uint16_t>(1u, static_cast<uint16_t>(desc_.Capacity / 2u));
        if (desc_.Name) {
            name_storage_ = desc_.Name;
            desc_.Name = name_storage_.c_str();
        }
#if defined(_WIN32)
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
        DStoragePriorityScheduler::Global().RegisterQueue(this, desc_.Priority);
    }

    ~DStorageQueueImpl() {
        CloseInternal();
#if defined(_WIN32)
        if (error_event_) {
            CloseHandle(error_event_);
            error_event_ = nullptr;
        }
#endif
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageQueueX) ||
            GuidEquals(riid, IID_IDStorageQueueX_Xbox)) {
            AddRef();
            *ppvObject = static_cast<IDStorageQueueX*>(this);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        if (GuidEquals(riid, IID_IDStorageQueueX1)) {
            AddRef();
            *ppvObject = static_cast<IDStorageQueueX1*>(this);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        if (GuidEquals(riid, IID_IDStorageQueueX2)) {
            AddRef();
            *ppvObject = static_cast<IDStorageQueueX2*>(this);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageQueueX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageQueueX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    // Title entry point: `request` is the Xbox dstorage_xs.h DSTORAGE_REQUEST (0x48 bytes).
    void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        RecordOk("IDStorageQueueX::EnqueueRequest", 3);
        if (!request) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Request;
        item.is_xbox_request = true;
        item.xbox_options = request->Options.AsUINT64;
        item.xbox_dest_page_offset = request->DestinationPageOffset;
        item.xbox_src_page_offset = request->SourcePageOffset;
        item.xbox_intermediate_size = request->IntermediateSize;
        item.decode_hr = DecodeXboxRequest(*request, item.request);
        EnqueueRequestItem(item);
    }

    // Host-extension entry point (OpenScarlettDStorageEnqueueRequestDesc).
    void EnqueueRequestDesc(const OpenScarlettDStorageRequestDesc& request) {
        QueueItem item{};
        item.kind = QueueItem::Kind::Request;
        item.is_xbox_request = false;
        item.request = request;
        EnqueueRequestItem(item);
    }

    void SetDevice(void* device) { device_ = device; }
    void SetTitleVaMirror(void* mirror) { title_va_mirror_.store(mirror, std::memory_order_release); }

    // Decodes a title's 0x48-byte Xbox DSTORAGE_REQUEST into the internal descriptor while
    // preserving exact Xbox option bits (ZlibDecompress, BcpackMode, SwizzleMode,
    // DestinationIsPhysicalPages, SourceIsPhysicalPages) and strictly validating reserved bits.
    static int32_t DecodeXboxRequest(const DSTORAGE_REQUEST& x, OpenScarlettDStorageRequestDesc& out) {
        out = OpenScarlettDStorageRequestDesc{};
        const uint64_t opts = x.Options.AsUINT64;
        const bool from_memory = (opts & DSTORAGE_REQUEST_OPTION_SOURCE_TYPE_MASK) != 0u;
        const bool dest_phys = (opts & DSTORAGE_REQUEST_OPTION_DEST_PHYSICAL_MASK) != 0u;
        const bool src_phys = (opts & DSTORAGE_REQUEST_OPTION_SOURCE_PHYSICAL_MASK) != 0u;
        const bool use_zlib = (opts & DSTORAGE_REQUEST_OPTION_ZLIB_MASK) != 0u;
        const uint64_t bcpack_mode =
            (opts & DSTORAGE_REQUEST_OPTION_BCPACK_MASK) >> DSTORAGE_REQUEST_OPTION_BCPACK_SHIFT;
        const uint64_t swizzle_mode =
            (opts & DSTORAGE_REQUEST_OPTION_SWIZZLE_MASK) >> DSTORAGE_REQUEST_OPTION_SWIZZLE_SHIFT;

        out.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        out.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        out.Options.SourceType = from_memory ? DSTORAGE_REQUEST_SOURCE_MEMORY : DSTORAGE_REQUEST_SOURCE_FILE;
        if (from_memory) {
            out.Source.Memory.Source = x.Source;
            out.Source.Memory.Size = x.SourceSize;
        } else {
            out.Source.File.Source = x.File;
            out.Source.File.Offset = x.FileOffset;
            out.Source.File.Size = x.SourceSize;
        }
        out.Destination.Memory.Buffer = x.Destination;
        out.Destination.Memory.Size = x.DestinationSize;
        out.UncompressedSize = x.DestinationSize;
        out.CancellationTag = x.CancellationTag;
        out.Name = x.Name;

        // Reserved option bits [24..63] MUST be zero.
        if ((opts & DSTORAGE_REQUEST_OPTION_RESERVED_MASK) != 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        // SourceIsPhysicalPages is only valid for memory-sourced requests.
        if (src_phys && !from_memory) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        // Validate BcpackMode enum range (0..7).
        if (bcpack_mode > static_cast<uint64_t>(DSTORAGE_BCPACK_MODE_BC7)) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        // Validate SwizzleMode enum range and Xbox 64 KiB tile granularity.
        if (swizzle_mode != 0u) {
            if (!IsValidXboxSwizzleMode(swizzle_mode)) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (x.DestinationSize == 0u || (x.DestinationSize & 0xFFFFu) != 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
        }
        // Uncompressed requests require SourceSize == DestinationSize.
        if (!use_zlib && bcpack_mode == 0u && x.SourceSize != x.DestinationSize) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        (void)dest_phys;
        return OS_DSTORAGE_S_OK;
    }

private:
    struct QueueItem;
    void EnqueueRequestItem(QueueItem& item) {
        if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
            item.request.Source.File.Source) {
            item.request.Source.File.Source->AddRef();
        }
        bool release_file = false;
        bool flushed = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
                ++staged_request_count_;
                if (!auto_submit_disabled_ && staged_request_count_ >= auto_submit_threshold_) {
                    flushed = FlushStagedLocked();
                }
            } else if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                       item.request.Source.File.Source) {
                release_file = true;
            }
        }
        if (flushed) {
            DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        }
        if (release_file) {
            item.request.Source.File.Source->Release();
        }
    }

public:
    void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) override {
        RecordOk("IDStorageQueueX::EnqueueStatus", 4);
        if (!statusArray) return;
        auto* impl = static_cast<DStorageStatusArrayImpl*>(statusArray);
        impl->MarkPending(index);
        impl->AddRef();

        QueueItem item{};
        item.kind = QueueItem::Kind::Status;
        item.status_array = impl;
        item.status_index = index;
        bool fail_closed = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
            } else {
                fail_closed = true;
            }
        }
        if (fail_closed) {
            impl->MarkComplete(index, OS_DSTORAGE_E_FAIL);
            impl->Release();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) override {
        RecordOk("IDStorageQueueX::EnqueueSetEvent", 5);
        if (!handle) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Event;
        item.event_handle = handle;
        std::unique_lock<std::mutex> lk(mutex_);
        WaitForSlotLocked(lk);
        if (!closed_) {
            staged_.push_back(item);
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL Submit() override {
        RecordOk("IDStorageQueueX::Submit", 6);
        bool flushed = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closed_ || staged_.empty()) return;
            flushed = FlushStagedLocked();
        }
        if (flushed) {
            DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) override {
        RecordOk("IDStorageQueueX::CancelRequestsWithTag", 7);
        std::lock_guard<std::mutex> lk(mutex_);
        auto cancel_in = [&](std::deque<QueueItem>& q, uint64_t& counter) {
            for (auto& item : q) {
                if (item.kind == QueueItem::Kind::Request &&
                    !item.canceled &&
                    (item.request.CancellationTag & mask) == value) {
                    item.canceled = true;
                    ++counter;
                }
            }
        };
        cancel_in(staged_, staged_canceled_count_);
        cancel_in(active_, submitted_canceled_count_);
        for (auto& st : in_flight_states_) {
            if (st && (st->tag & mask) == value) {
                if (!st->canceled.exchange(true, std::memory_order_acq_rel)) {
                    ++inflight_canceled_count_;
                }
            }
        }
    }

    void GetQueueStats(DSTORAGE_QUEUE_STATS& out) {
        std::lock_guard<std::mutex> lk(mutex_);
        out.StagedCanceled = staged_canceled_count_;
        out.SubmittedCanceled = submitted_canceled_count_;
        out.InFlightCanceled = inflight_canceled_count_;
        out.CompletedOk = completed_ok_count_.load(std::memory_order_acquire);
        out.CompletedFailed = completed_failed_count_.load(std::memory_order_acquire);
        out.CompletedCanceled = completed_canceled_count_.load(std::memory_order_acquire);
        out.PhysicalPageWrites = physical_page_writes_.load(std::memory_order_acquire);
        out.XMemDestinationWrites = xmem_destination_writes_.load(std::memory_order_acquire);
        out.StagingBufferSize = staging_size_ref_->load(std::memory_order_acquire);
        out.LastIntermediateSize = last_intermediate_size_.load(std::memory_order_acquire);
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        RecordOk("IDStorageQueueX::Close", 8);
        CloseInternal();
    }

    void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() override {
        RecordOk("IDStorageQueueX::GetErrorEvent", 9);
#if defined(_WIN32)
        return error_event_;
#else
        return nullptr;
#endif
    }

    void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        RecordOk("IDStorageQueueX::RetrieveErrorRecord", 10);
        if (!record) return;
        std::lock_guard<std::mutex> lk(error_mutex_);
        *record = error_record_;
        std::memset(&error_record_, 0, sizeof(error_record_));
#if defined(_WIN32)
        if (error_event_) {
            ResetEvent(error_event_);
        }
#endif
    }

    void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) override {
        RecordOk("IDStorageQueueX::Query", 11);
        if (!info) return;
        std::lock_guard<std::mutex> lk(mutex_);
        info->Desc = desc_;
        const size_t used = staged_.size() + active_.size() + in_flight_requests_;
        info->EmptySlotCount = (used < desc_.Capacity)
                                   ? static_cast<uint16_t>(desc_.Capacity - used)
                                   : 0u;
        info->RequestCountUntilAutoSubmit =
            auto_submit_disabled_
                ? 0u
                : ((staged_request_count_ < auto_submit_threshold_)
                       ? static_cast<uint16_t>(auto_submit_threshold_ - staged_request_count_)
                       : 0u);
    }

    // IDStorageQueueX1 (slot 12)
    void OPENSCARLETT_DSTORAGE_CALL EnqueueSignal(void* fence, uint64_t value) override {
        RecordOk("IDStorageQueueX1::EnqueueSignal", 12);
        if (!fence) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Signal;
        item.signal_fence = fence;
        item.signal_value = value;
        std::unique_lock<std::mutex> lk(mutex_);
        WaitForSlotLocked(lk);
        if (!closed_) {
            staged_.push_back(item);
        }
    }

    // IDStorageQueueX2 (slots 13..15)
    void OPENSCARLETT_DSTORAGE_CALL Query1(DSTORAGE_QUEUE_INFO1* info) override {
        RecordOk("IDStorageQueueX2::Query1", 13);
        if (!info) return;
        std::lock_guard<std::mutex> lk(mutex_);
        info->Desc.SourceType = desc_.SourceType;
        info->Desc.Capacity = desc_.Capacity;
        info->Desc.Priority = desc_.Priority;
        info->Desc.Options = queue_options_;
        info->Desc.Name = desc_.Name;
        const size_t used = staged_.size() + active_.size() + in_flight_requests_;
        info->EmptySlotCount = (used < desc_.Capacity)
                                   ? static_cast<uint16_t>(desc_.Capacity - used)
                                   : 0u;
        info->RequestCountUntilAutoSubmit =
            auto_submit_disabled_
                ? 0u
                : ((staged_request_count_ < auto_submit_threshold_)
                       ? static_cast<uint16_t>(auto_submit_threshold_ - staged_request_count_)
                       : 0u);
        info->AutoSubmitDisabled = auto_submit_disabled_ ? 1u : 0u;
        info->InFlightRequestCount = static_cast<uint32_t>(in_flight_requests_);
        info->CompletedRequestCount =
            completed_ok_count_.load(std::memory_order_acquire) +
            completed_failed_count_.load(std::memory_order_acquire) +
            completed_canceled_count_.load(std::memory_order_acquire);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetAutoSubmitDisabled(bool disabled) override {
        std::lock_guard<std::mutex> lk(mutex_);
        auto_submit_disabled_ = disabled;
        if (disabled) {
            queue_options_ |= DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT;
        } else {
            queue_options_ &= ~static_cast<uint32_t>(DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT);
        }
        return RecordHr("IDStorageQueueX2::SetAutoSubmitDisabled", OS_DSTORAGE_S_OK, 14);
    }

    bool OPENSCARLETT_DSTORAGE_CALL GetAutoSubmitDisabled() override {
        RecordOk("IDStorageQueueX2::GetAutoSubmitDisabled", 15);
        std::lock_guard<std::mutex> lk(mutex_);
        return auto_submit_disabled_;
    }

    // Called by DStoragePriorityScheduler under scheduler mutex to test whether an item
    // can be dispatched immediately (requests run concurrently; barriers wait for in-flight==0).
    bool CanDispatchNow() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (active_.empty()) return false;
        if (active_.front().kind == QueueItem::Kind::Request) {
            return true;
        }
        return in_flight_requests_ == 0u;
    }

    // Returns true if this queue has queued active items or in-flight work (used to prevent
    // lower-priority buckets from preempting a higher-priority queue waiting on its own barrier).
    bool HasPendingActiveOrInFlight() {
        std::lock_guard<std::mutex> lk(mutex_);
        return !active_.empty() || in_flight_requests_ > 0u || in_flight_barriers_ > 0u;
    }

    // Claims and executes 1 active item (concurrently with sibling Requests when possible).
    bool ProcessOneActiveItem() {
        QueueItem item{};
        std::shared_ptr<InFlightRequestState> inflight;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (active_.empty()) {
                return false;
            }
            if (active_.front().kind != QueueItem::Kind::Request && in_flight_requests_ > 0u) {
                return false;
            }
            item = active_.front();
            active_.pop_front();
            active_count_.fetch_sub(1u, std::memory_order_release);
            if (item.kind == QueueItem::Kind::Request) {
                inflight = std::make_shared<InFlightRequestState>();
                inflight->tag = item.request.CancellationTag;
                inflight->canceled.store(item.canceled, std::memory_order_release);
                in_flight_states_.push_back(inflight);
                ++in_flight_requests_;
            } else {
                ++in_flight_barriers_;
            }
        }
        space_cv_.notify_all();

        switch (item.kind) {
            case QueueItem::Kind::Request: {
                auto& rt = GetRuntime();
                switch (PriorityToBucket(desc_.Priority)) {
                    case 0u: rt.realtime_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                    case 1u: rt.high_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                    case 2u: rt.normal_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                    default: rt.low_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                }

                int32_t hr = OS_DSTORAGE_S_OK;
                if (!item.canceled && !inflight->canceled.load(std::memory_order_acquire)) {
                    if (item.decode_hr < 0) {
                        hr = item.decode_hr;
                    } else if (item.is_xbox_request) {
                        hr = ExecuteXboxRequest(item, &inflight->canceled);
                    } else {
                        hr = ExecuteRequest(item.request, &inflight->canceled);
                    }
                }
                const bool was_canceled =
                    item.canceled ||
                    (hr == OS_DSTORAGE_E_CANCELLED) ||
                    inflight->canceled.load(std::memory_order_acquire);

                if (was_canceled) {
                    // Cancelled requests MUST NEVER poison RetrieveErrorRecord, error_event_, or batch_hr!
                    completed_canceled_count_.fetch_add(1u, std::memory_order_relaxed);
                } else if (hr < 0) {
                    completed_failed_count_.fetch_add(1u, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        if (batch_hr_ >= 0) batch_hr_ = hr;
                    }
                    RecordFailure(hr, item);
                } else {
                    completed_ok_count_.fetch_add(1u, std::memory_order_relaxed);
                    rt.total_requests_completed.fetch_add(1u, std::memory_order_relaxed);
                }

                if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                    item.request.Source.File.Source) {
                    item.request.Source.File.Source->Release();
                }

                bool wake_barriers = false;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    in_flight_states_.erase(
                        std::remove(in_flight_states_.begin(), in_flight_states_.end(), inflight),
                        in_flight_states_.end());
                    if (in_flight_requests_ > 0u) --in_flight_requests_;
                    if (in_flight_requests_ == 0u && !active_.empty()) {
                        wake_barriers = true;
                    }
                    if (closed_ && active_.empty() && in_flight_requests_ == 0u && in_flight_barriers_ == 0u) {
                        drain_cv_.notify_all();
                    }
                }
                space_cv_.notify_all();
                if (wake_barriers) {
                    DStoragePriorityScheduler::Global().NotifyWorkAvailable();
                }
                return true;
            }

            case QueueItem::Kind::Status: {
                int32_t hr_to_report = OS_DSTORAGE_S_OK;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    hr_to_report = batch_hr_;
                    batch_hr_ = OS_DSTORAGE_S_OK;
                }
                if (item.status_array) {
                    item.status_array->MarkComplete(item.status_index, hr_to_report);
                    item.status_array->Release();
                }
                break;
            }

            case QueueItem::Kind::Event: {
#if defined(_WIN32)
                if (item.event_handle) {
                    SetEvent(static_cast<HANDLE>(item.event_handle));
                }
#endif
                break;
            }

            case QueueItem::Kind::Signal: {
#if defined(_WIN32)
                if (item.signal_fence) {
                    static_cast<ID3D12Fence*>(item.signal_fence)->Signal(item.signal_value);
                }
#endif
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (in_flight_barriers_ > 0u) --in_flight_barriers_;
            if (closed_ && active_.empty() && in_flight_requests_ == 0u && in_flight_barriers_ == 0u) {
                drain_cv_.notify_all();
            }
        }
        space_cv_.notify_all();
        return true;
    }

private:
    struct QueueItem {
        enum class Kind { Request, Status, Event, Signal };
        Kind kind = Kind::Request;
        bool canceled = false;
        bool is_xbox_request = false;
        OpenScarlettDStorageRequestDesc request{};
        int32_t decode_hr = OS_DSTORAGE_S_OK;
        uint64_t xbox_options = 0u;
        uint16_t xbox_dest_page_offset = 0u;
        uint16_t xbox_src_page_offset = 0u;
        uint32_t xbox_intermediate_size = 0u;
        DStorageStatusArrayImpl* status_array = nullptr;
        uint32_t status_index = 0;
        void* event_handle = nullptr;
        void* signal_fence = nullptr;
        uint64_t signal_value = 0ull;
    };

    bool FlushStagedLocked() {
        if (staged_.empty()) {
            staged_request_count_ = 0u;
            return false;
        }
        const size_t moved = staged_.size();
        for (auto& it : staged_) {
            active_.push_back(it);
        }
        staged_.clear();
        staged_request_count_ = 0u;
        active_count_.fetch_add(moved, std::memory_order_release);
        return true;
    }

    void WaitForSlotLocked(std::unique_lock<std::mutex>& lk) {
        while (!closed_ && (staged_.size() + active_.size() + in_flight_requests_) >= desc_.Capacity) {
            bool flushed = false;
            if (!staged_.empty()) {
                flushed = FlushStagedLocked();
            }
            if (flushed) {
                lk.unlock();
                DStoragePriorityScheduler::Global().NotifyWorkAvailable();
                lk.lock();
                if (closed_ || (staged_.size() + active_.size() + in_flight_requests_) < desc_.Capacity) {
                    break;
                }
            }
            space_cv_.wait(lk, [&]() {
                return closed_ || (staged_.size() + active_.size() + in_flight_requests_) < desc_.Capacity;
            });
        }
    }

    int32_t AcquireSourceBytes(
        const OpenScarlettDStorageRequestDesc& req,
        size_t staging_limit,
        std::vector<uint8_t>& file_buf,
        const uint8_t*& out_ptr,
        size_t& out_size,
        const std::atomic<bool>* cancel_flag,
        bool check_staging_for_src = true) {
        out_ptr = nullptr;
        out_size = 0u;
        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!req.Source.File.Source || req.Source.File.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (check_staging_for_src &&
                req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.File.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            auto* file_impl = static_cast<DStorageFileImpl*>(req.Source.File.Source);
            const int32_t rd_hr = file_impl->ReadAt(
                req.Source.File.Offset, req.Source.File.Size, file_buf, cancel_flag);
            if (rd_hr < 0) return rd_hr;
            out_ptr = file_buf.data();
            out_size = file_buf.size();
            return OS_DSTORAGE_S_OK;
        }
        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
            if (!req.Source.Memory.Source || req.Source.Memory.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (check_staging_for_src &&
                req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.Memory.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            out_ptr = static_cast<const uint8_t*>(req.Source.Memory.Source);
            out_size = req.Source.Memory.Size;
            return OS_DSTORAGE_S_OK;
        }
        return OS_DSTORAGE_E_INVALIDARG;
    }

    // Executes a real Xbox 0x48-byte DSTORAGE_REQUEST honoring:
    //   - Options.ZlibDecompress (RFC 1950/1951 Zlib/Deflate)
    //   - Options.BcpackMode (BC1..BC7 hardware bitstream / BCPK)
    //   - Options.SwizzleMode (4K/4K/8K, 4K/4K/24K, 4K/12K/4K/4K/8K, 4K/12K/4K/12K)
    //   - Options.DestinationIsPhysicalPages & Options.SourceIsPhysicalPages
    int32_t ExecuteXboxRequest(const QueueItem& item, const std::atomic<bool>* cancel_flag) {
        auto is_canceled = [&]() -> bool {
            return cancel_flag && cancel_flag->load(std::memory_order_acquire);
        };
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        const OpenScarlettDStorageRequestDesc& req = item.request;
        const uint64_t opts = item.xbox_options;
        const bool use_zlib = (opts & DSTORAGE_REQUEST_OPTION_ZLIB_MASK) != 0u;
        const uint64_t bcpack_mode =
            (opts & DSTORAGE_REQUEST_OPTION_BCPACK_MASK) >> DSTORAGE_REQUEST_OPTION_BCPACK_SHIFT;
        const uint64_t swizzle_mode =
            (opts & DSTORAGE_REQUEST_OPTION_SWIZZLE_MASK) >> DSTORAGE_REQUEST_OPTION_SWIZZLE_SHIFT;
        const bool dest_phys = (opts & DSTORAGE_REQUEST_OPTION_DEST_PHYSICAL_MASK) != 0u;
        const bool src_phys = (opts & DSTORAGE_REQUEST_OPTION_SOURCE_PHYSICAL_MASK) != 0u;

        if (!req.Destination.Memory.Buffer || req.Destination.Memory.Size == 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const size_t dst_size = req.Destination.Memory.Size;
        const size_t staging_limit =
            static_cast<size_t>(staging_size_ref_->load(std::memory_order_acquire));

        // 1. Validate virtual destination before reading/decompressing (physical page array validated below).
        DestinationClassification dst_class{};
        if (!dest_phys) {
            const int32_t dhr = ValidateAndClassifyDestination(
                req.Destination.Memory.Buffer, dst_size, dst_class);
            if (dhr < 0) return dhr;
        }

        // 2. Acquire source bytes (IOCP file read, virtual memory, or physical page array).
        std::vector<uint8_t> src_storage;
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0u;
        if (src_phys) {
            if (req.Options.SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY ||
                !req.Source.Memory.Source || req.Source.Memory.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            src_storage.resize(req.Source.Memory.Size);
            const int32_t phr = TransferPhysicalPageArray(
                title_va_mirror_.load(std::memory_order_acquire),
                static_cast<const uintptr_t*>(req.Source.Memory.Source),
                item.xbox_src_page_offset,
                req.Source.Memory.Size,
                false,
                src_storage.data());
            if (phr < 0) return phr;
            src_ptr = src_storage.data();
            src_size = src_storage.size();
        } else {
            const int32_t shr = AcquireSourceBytes(
                req, staging_limit, src_storage, src_ptr, src_size, cancel_flag, false);
            if (shr < 0) return shr;
        }
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        // 3. Stage 1 Decompression: RFC 1950/1951 Zlib if Options.ZlibDecompress is set.
        std::vector<uint8_t> stage_buf;
        const uint8_t* cur_ptr = src_ptr;
        size_t cur_size = src_size;

        if (use_zlib) {
            const size_t zlib_max =
                (bcpack_mode != 0u && item.xbox_intermediate_size > 0u)
                    ? static_cast<size_t>(item.xbox_intermediate_size)
                    : std::max(dst_size, static_cast<size_t>(item.xbox_intermediate_size));
            if (zlib_max == 0u) return OS_DSTORAGE_E_INVALIDARG;
            if (zlib_max > staging_limit) return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            if (!zlib_rfc1950::Decompress(cur_ptr, cur_size, zlib_max, stage_buf)) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            if (bcpack_mode == 0u && stage_buf.size() != dst_size) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            cur_ptr = stage_buf.data();
            cur_size = stage_buf.size();
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(cur_size, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().total_zlib_decompressions.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::XboxZlibDecompress");
        }
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        // 4. Stage 2 Decompression: Hardware BCPack BC1-BC7 if Options.BcpackMode != NONE.
        std::vector<uint8_t> bcpack_out;
        if (bcpack_mode != 0u) {
            if (dst_size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            const auto fmt = static_cast<openscarlett::BcFormat>(bcpack_mode);
            const uint32_t blk_bytes = openscarlett::BcFormatBlockBytes(fmt);
            if (blk_bytes == 0u || (dst_size % blk_bytes) != 0u) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            bcpack_out.resize(dst_size);
            size_t written = 0u;
            auto st = openscarlett::BcPackHwDecodeBitstream(
                fmt, cur_ptr, cur_size, bcpack_out.data(), bcpack_out.size(), &written);
            if (!st.ok()) {
                // Also accept a matching-format BCPK container if provided with exact size.
                openscarlett::BcPackHeader hdr{};
                if (openscarlett::BcPackParseHeader(cur_ptr, cur_size, hdr).ok() &&
                    hdr.format == fmt &&
                    hdr.original_size == dst_size &&
                    openscarlett::BcPackDecompressInto(
                        cur_ptr, cur_size, bcpack_out.data(), bcpack_out.size(), &written).ok()) {
                    st = openscarlett::Status::Ok();
                }
            }
            if (!st.ok() || written != dst_size) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            cur_ptr = bcpack_out.data();
            cur_size = bcpack_out.size();
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(cur_size, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().total_bcpack_decompressions.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::XboxBcpackDecompress");
        }
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        if (cur_size != dst_size) {
            return (use_zlib || bcpack_mode != 0u)
                       ? OS_DSTORAGE_E_DECOMPRESSION_ERROR
                       : OS_DSTORAGE_E_INVALIDARG;
        }

        // 5. Stage 3 Hardware Swizzle (Options.SwizzleMode).
        std::vector<uint8_t> swizzled_out;
        if (swizzle_mode != 0u) {
            if (!ApplyXboxSwizzle64KB(swizzle_mode, cur_ptr, cur_size, swizzled_out)) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            cur_ptr = swizzled_out.data();
            cur_size = swizzled_out.size();
            GetRuntime().total_swizzle_transforms.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::XboxSwizzle");
        }
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        // 6. Commit output to destination (physical page array OR virtual memory).
        if (dest_phys) {
            const int32_t phr = TransferPhysicalPageArray(
                title_va_mirror_.load(std::memory_order_acquire),
                static_cast<const uintptr_t*>(req.Destination.Memory.Buffer),
                item.xbox_dest_page_offset,
                static_cast<uint32_t>(cur_size),
                true,
                const_cast<uint8_t*>(cur_ptr));
            if (phr < 0) return phr;
            physical_page_writes_.fetch_add(1u, std::memory_order_relaxed);
            GetRuntime().total_physical_page_transfers.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::XboxPhysicalPageWrite");
            return OS_DSTORAGE_S_OK;
        }

        auto* dst_ptr = static_cast<uint8_t*>(req.Destination.Memory.Buffer);
        constexpr size_t kCopyChunk = 65536u;
        size_t copied = 0u;
        while (copied < cur_size) {
            if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
            const size_t step = std::min(cur_size - copied, kCopyChunk);
            std::memcpy(dst_ptr + copied, cur_ptr + copied, step);
            copied += step;
        }
        if (dst_class.is_physical_page) {
            physical_page_writes_.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::DecompressToPhysicalPages");
        }
        if (dst_class.is_xmem) {
            xmem_destination_writes_.fetch_add(1u, std::memory_order_relaxed);
        }
        return OS_DSTORAGE_S_OK;
    }

    int32_t DecompressToVector(
        const OpenScarlettDStorageRequestDesc& req,
        const uint8_t* src_ptr,
        size_t src_size,
        size_t staging_limit,
        std::vector<uint8_t>& out_data,
        const std::atomic<bool>* cancel_flag) {
        out_data.clear();
        auto is_canceled = [&]() -> bool {
            return cancel_flag && cancel_flag->load(std::memory_order_acquire);
        };
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        const uint8_t fmt_u8 = static_cast<uint8_t>(req.Options.CompressionFormat);
        CustomDecompressorEntry custom{};
        if (GetRuntime().LookupCustom(fmt_u8, custom)) {
            const size_t expected_sz = (req.UncompressedSize != 0u) ? req.UncompressedSize : src_size;
            if (expected_sz == 0u) return OS_DSTORAGE_E_INVALIDARG;
            if (expected_sz > staging_limit) return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            out_data.resize(expected_sz);
            size_t actual_sz = 0u;
            const int32_t chr = custom.callback(
                fmt_u8, src_ptr, src_size, out_data.data(), out_data.size(), &actual_sz, custom.user_context);
            if (chr < 0) {
                out_data.clear();
                return chr;
            }
            if (actual_sz > out_data.size()) {
                out_data.clear();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            if (req.UncompressedSize != 0u && actual_sz != req.UncompressedSize) {
                out_data.clear();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            out_data.resize(actual_sz);
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(actual_sz, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().custom_decompressed.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::CustomDecompress");
            return OS_DSTORAGE_S_OK;
        }

        switch (req.Options.CompressionFormat) {
            case DSTORAGE_COMPRESSION_FORMAT_NONE: {
                if (req.UncompressedSize != 0u && req.UncompressedSize != src_size) {
                    return OS_DSTORAGE_E_INVALIDARG;
                }
                out_data.assign(src_ptr, src_ptr + src_size);
                return OS_DSTORAGE_S_OK;
            }
            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
                size_t header_uncompressed = 0u;
                if (!openscarlett::GDeflateIsValidStream(src_ptr, src_size) ||
                    !openscarlett::GDeflateGetUncompressedSize(src_ptr, src_size, &header_uncompressed) ||
                    header_uncompressed == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(
                    static_cast<uint32_t>(std::min<size_t>(header_uncompressed, UINT32_MAX)),
                    std::memory_order_release);
                if (header_uncompressed > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (req.UncompressedSize != 0u && header_uncompressed != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                out_data.resize(header_uncompressed);
                size_t out_sz = 0u;
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, out_data.data(), out_data.size(), &out_sz);
                if (res != openscarlett::GDeflateResult::Ok ||
                    (req.UncompressedSize != 0u && out_sz != req.UncompressedSize)) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                out_data.resize(out_sz);
                return OS_DSTORAGE_S_OK;
            }
            case DSTORAGE_COMPRESSION_FORMAT_ZLIB: {
                const size_t max_out = (req.UncompressedSize != 0u)
                                           ? std::min<size_t>(req.UncompressedSize, staging_limit)
                                           : staging_limit;
                if (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (zlib_rfc1950::Decompress(src_ptr, src_size, max_out, out_data)) {
                    if (req.UncompressedSize != 0u && out_data.size() != req.UncompressedSize) {
                        out_data.clear();
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    last_intermediate_size_.store(
                        static_cast<uint32_t>(std::min<size_t>(out_data.size(), UINT32_MAX)),
                        std::memory_order_release);
                    GetRuntime().total_zlib_decompressions.fetch_add(1u, std::memory_order_relaxed);
                    return OS_DSTORAGE_S_OK;
                }
                // Legacy host-extension fallback only when explicit BCPK container is passed.
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                if (!openscarlett::BcPackDecompress(src_ptr, src_size, out_data).ok()) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                return OS_DSTORAGE_S_OK;
            }
            case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                if (!openscarlett::BcPackDecompress(src_ptr, src_size, out_data).ok()) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (req.UncompressedSize != 0u && out_data.size() != req.UncompressedSize) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                return OS_DSTORAGE_S_OK;
            }
            default:
                return OS_DSTORAGE_E_INVALIDARG;
        }
    }

#if defined(_WIN32)
    int32_t ExecuteGpuBufferRequest(
        const OpenScarlettDStorageRequestDesc& req,
        const uint8_t* src_ptr,
        size_t src_size,
        size_t staging_limit,
        const std::atomic<bool>* cancel_flag) {
        auto* dst_res = static_cast<ID3D12Resource*>(req.Destination.Memory.Buffer);
        if (!dst_res) return OS_DSTORAGE_E_INVALIDARG;

        std::vector<uint8_t> unpacked;
        const int32_t dhr = DecompressToVector(req, src_ptr, src_size, staging_limit, unpacked, cancel_flag);
        if (dhr < 0) return dhr;
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        const D3D12_RESOURCE_DESC rdesc = dst_res->GetDesc();
        if (rdesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || unpacked.size() > rdesc.Width) {
            return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
        }
        if (req.Destination.Memory.Size != 0u && unpacked.size() > req.Destination.Memory.Size) {
            return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
        }

        ID3D12Device* dev = static_cast<ID3D12Device*>(device_);
        ID3D12Device* queried_dev = nullptr;
        if (!dev) {
            if (FAILED(dst_res->GetDevice(OS_UUIDOF(ID3D12Device),
                                          reinterpret_cast<void**>(&queried_dev))) ||
                !queried_dev) {
                return OS_DSTORAGE_E_FAIL;
            }
            dev = queried_dev;
        }

        int32_t result_hr = OS_DSTORAGE_S_OK;
        {
            std::lock_guard<std::mutex> glk(gpu_ctx_.gpu_mutex);
            if (!gpu_ctx_.EnsureInitialized(dev)) {
                result_hr = OS_DSTORAGE_E_FAIL;
            } else {
                ID3D12Resource* up_buf = gpu_ctx_.AcquireUploadRingBuffer(unpacked.size());
                if (!up_buf) {
                    result_hr = OS_DSTORAGE_E_OUTOFMEMORY;
                } else {
                    void* mapped = nullptr;
                    const D3D12_RANGE empty_r{0, 0};
                    if (FAILED(up_buf->Map(0u, &empty_r, &mapped)) || !mapped) {
                        result_hr = OS_DSTORAGE_E_FAIL;
                    } else {
                        std::memcpy(mapped, unpacked.data(), unpacked.size());
                        up_buf->Unmap(0u, nullptr);

                        if (FAILED(gpu_ctx_.cmd_alloc->Reset()) ||
                            FAILED(gpu_ctx_.cmd_list->Reset(gpu_ctx_.cmd_alloc, nullptr))) {
                            result_hr = OS_DSTORAGE_E_FAIL;
                        } else {
                            D3D12_RESOURCE_BARRIER b0{};
                            b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b0.Transition = {dst_res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                             D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
                            gpu_ctx_.cmd_list->ResourceBarrier(1u, &b0);
                            gpu_ctx_.cmd_list->CopyBufferRegion(dst_res, 0ull, up_buf, 0ull, unpacked.size());
                            D3D12_RESOURCE_BARRIER b1{};
                            b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b1.Transition = {dst_res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
                            gpu_ctx_.cmd_list->ResourceBarrier(1u, &b1);

                            if (!gpu_ctx_.ExecuteAndWait()) {
                                result_hr = OS_DSTORAGE_E_FAIL;
                            }
                        }
                    }
                }
            }
        }
        if (queried_dev) queried_dev->Release();
        return result_hr;
    }

    int32_t ExecuteGpuTextureRequest(
        const OpenScarlettDStorageRequestDesc& req,
        const uint8_t* src_ptr,
        size_t src_size,
        size_t staging_limit,
        const std::atomic<bool>* cancel_flag) {
        if (!req.Destination.Memory.Buffer) return OS_DSTORAGE_E_INVALIDARG;

        OpenScarlettDStorageTextureDestDesc local_desc{};
        OpenScarlettDStorageTextureDestDesc* caller_desc = nullptr;

        if (req.Destination.Memory.Size == sizeof(OpenScarlettDStorageTextureDestDesc)) {
            auto* candidate =
                static_cast<OpenScarlettDStorageTextureDestDesc*>(req.Destination.Memory.Buffer);
            if (candidate->struct_magic == OS_DSTORAGE_TEXTURE_DEST_MAGIC &&
                candidate->struct_size == sizeof(OpenScarlettDStorageTextureDestDesc)) {
                local_desc = *candidate;
                caller_desc = candidate;
            }
        }
        if (!caller_desc) {
            local_desc.d3d12_resource = req.Destination.Memory.Buffer;
            local_desc.d3d12_device = device_;
        }

        auto* dst_tex = static_cast<ID3D12Resource*>(local_desc.d3d12_resource);
        if (!dst_tex) return OS_DSTORAGE_E_INVALIDARG;

        // 1. Decompress source stream in parallel on worker thread BEFORE locking GPU upload ring!
        std::vector<uint8_t> unpacked;
        const int32_t dhr = DecompressToVector(req, src_ptr, src_size, staging_limit, unpacked, cancel_flag);
        if (dhr < 0) return dhr;
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        // 2. Resolve D3D12 device and target texture geometry.
        ID3D12Device* dev = static_cast<ID3D12Device*>(
            local_desc.d3d12_device ? local_desc.d3d12_device : device_);
        ID3D12Device* queried_dev = nullptr;
        if (!dev) {
            if (FAILED(dst_tex->GetDevice(OS_UUIDOF(ID3D12Device),
                                          reinterpret_cast<void**>(&queried_dev))) ||
                !queried_dev) {
                return OS_DSTORAGE_E_FAIL;
            }
            dev = queried_dev;
        }

        const D3D12_RESOURCE_DESC rdesc = dst_tex->GetDesc();
        const uint32_t tex_w = local_desc.width ? local_desc.width : static_cast<uint32_t>(rdesc.Width);
        const uint32_t tex_h = local_desc.height ? local_desc.height : rdesc.Height;
        const uint32_t dxgi_fmt = local_desc.dxgi_format
                                      ? local_desc.dxgi_format
                                      : static_cast<uint32_t>(rdesc.Format);

        std::lock_guard<std::mutex> glk(gpu_ctx_.gpu_mutex);
        if (!gpu_ctx_.EnsureInitialized(dev)) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_FAIL;
        }

        // 3. If source layout is GFX10 tiled, route automatically through XgGpuUntiler!
        std::vector<uint8_t> linear_buf;
        const uint8_t* upload_src = unpacked.data();
        size_t upload_src_size = unpacked.size();
        uint32_t src_row_pitch = local_desc.src_row_pitch;
        bool used_gpu_untiler = false;

        if (static_cast<XG_TILE_MODE>(local_desc.src_tile_mode) != XG_TILE_MODE_LINEAR) {
            openscarlett::XgGpuSurface2DRequest ureq{};
            ureq.desc.Width = tex_w;
            ureq.desc.Height = tex_h;
            ureq.desc.MipLevels = std::max<uint32_t>(rdesc.MipLevels, local_desc.mip_level + 1u);
            ureq.desc.ArraySize = std::max<uint32_t>(rdesc.DepthOrArraySize, local_desc.array_slice + 1u);
            ureq.desc.Format = dxgi_fmt;
            ureq.desc.SampleCount = 1u;
            ureq.desc.BindFlags = XG_BIND_SHADER_RESOURCE;
            ureq.desc.TileMode = static_cast<XG_TILE_MODE>(local_desc.src_tile_mode);
            ureq.plane = local_desc.plane_slice;
            ureq.mip_level = local_desc.mip_level;
            ureq.slice_index = local_desc.array_slice;
            ureq.direction = openscarlett::XgGpuDirection::Untile;
            ureq.linear_row_pitch_bytes = local_desc.src_row_pitch;
            ureq.metadata_mode = static_cast<openscarlett::XgGpuMetadataMode>(local_desc.metadata_mode);
            ureq.console_type = static_cast<XG_CONSOLE_TYPE>(local_desc.console_type);
            for (size_t i = 0u; i < 4u; ++i) {
                ureq.clear_reg_value[i] = local_desc.clear_reg_value[i];
            }

            openscarlett::XgGpuUntileParams uparams{};
            uint64_t linear_bytes = 0ull;
            uint64_t tiled_bytes = 0ull;
            if (!openscarlett::BuildXgGpuUntileParams2D(ureq, uparams, &linear_bytes, &tiled_bytes).ok()) {
                if (queried_dev) queried_dev->Release();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            if (unpacked.size() < tiled_bytes) {
                if (queried_dev) queried_dev->Release();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }

            linear_buf.assign(static_cast<size_t>(linear_bytes), 0u);
            std::vector<uint8_t> meta_copy;
            const std::vector<uint8_t>* meta_ptr = nullptr;
            if (local_desc.metadata_buffer && local_desc.metadata_size_bytes > 0u) {
                const auto* mb = static_cast<const uint8_t*>(local_desc.metadata_buffer);
                meta_copy.assign(mb, mb + local_desc.metadata_size_bytes);
                meta_ptr = &meta_copy;
            }

            bool untile_ok = false;
            if (local_desc.prefer_gpu_untiler != 0u && gpu_ctx_.EnsureUntilerReady()) {
                if (gpu_ctx_.untiler.ExecuteD3D12(uparams, unpacked, linear_buf, meta_ptr).ok()) {
                    untile_ok = true;
                    used_gpu_untiler = true;
                }
            }
            if (!untile_ok && local_desc.allow_cpu_untile_fallback != 0u) {
                if (openscarlett::xg::UntileSurface2DPlane(
                        ureq.desc,
                        ureq.plane,
                        ureq.mip_level,
                        ureq.slice_index,
                        unpacked.data(),
                        unpacked.size(),
                        linear_buf.data(),
                        uparams.linear_row_pitch)) {
                    untile_ok = true;
                }
            }
            if (!untile_ok) {
                if (queried_dev) queried_dev->Release();
                return OS_DSTORAGE_E_FAIL;
            }
            upload_src = linear_buf.data();
            upload_src_size = linear_buf.size();
            src_row_pitch = uparams.linear_row_pitch;
        }

        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_CANCELLED;
        }

        // 4. Upload linear texels/BC blocks into target ID3D12Resource texture via upload ring.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0u;
        UINT64 row_size_bytes = 0ull;
        UINT64 total_upload_bytes = 0ull;
        dev->GetCopyableFootprints(
            &rdesc,
            local_desc.subresource_index,
            1u,
            0ull,
            &footprint,
            &num_rows,
            &row_size_bytes,
            &total_upload_bytes);

        if (num_rows == 0u || row_size_bytes == 0ull || total_upload_bytes == 0ull) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const uint64_t effective_src_pitch =
            (src_row_pitch != 0u) ? static_cast<uint64_t>(src_row_pitch) : row_size_bytes;
        if (effective_src_pitch < row_size_bytes ||
            effective_src_pitch * static_cast<uint64_t>(num_rows) > upload_src_size) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
        }

        ID3D12Resource* up_buf = gpu_ctx_.AcquireUploadRingBuffer(total_upload_bytes);
        if (!up_buf) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_OUTOFMEMORY;
        }

        int32_t result_hr = OS_DSTORAGE_S_OK;
        void* mapped = nullptr;
        const D3D12_RANGE empty_r{0, 0};
        if (FAILED(up_buf->Map(0u, &empty_r, &mapped)) || !mapped) {
            result_hr = OS_DSTORAGE_E_FAIL;
        } else {
            auto* dst_base = static_cast<uint8_t*>(mapped) + footprint.Offset;
            for (UINT row = 0u; row < num_rows; ++row) {
                std::memcpy(
                    dst_base + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    upload_src + static_cast<size_t>(row) * static_cast<size_t>(effective_src_pitch),
                    static_cast<size_t>(row_size_bytes));
            }
            up_buf->Unmap(0u, nullptr);

            if (FAILED(gpu_ctx_.cmd_alloc->Reset()) ||
                FAILED(gpu_ctx_.cmd_list->Reset(gpu_ctx_.cmd_alloc, nullptr))) {
                result_hr = OS_DSTORAGE_E_FAIL;
            } else {
                const auto init_state = static_cast<D3D12_RESOURCE_STATES>(local_desc.initial_resource_state);
                const auto final_state = static_cast<D3D12_RESOURCE_STATES>(local_desc.final_resource_state);
                if (init_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    D3D12_RESOURCE_BARRIER b0{};
                    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b0.Transition = {dst_tex, local_desc.subresource_index,
                                     init_state, D3D12_RESOURCE_STATE_COPY_DEST};
                    gpu_ctx_.cmd_list->ResourceBarrier(1u, &b0);
                }

                D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                dst_loc.pResource = dst_tex;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = local_desc.subresource_index;

                D3D12_TEXTURE_COPY_LOCATION src_loc{};
                src_loc.pResource = up_buf;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = footprint;

                gpu_ctx_.cmd_list->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);

                if (final_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    D3D12_RESOURCE_BARRIER b1{};
                    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b1.Transition = {dst_tex, local_desc.subresource_index,
                                     D3D12_RESOURCE_STATE_COPY_DEST, final_state};
                    gpu_ctx_.cmd_list->ResourceBarrier(1u, &b1);
                }

                if (!gpu_ctx_.ExecuteAndWait()) {
                    result_hr = OS_DSTORAGE_E_FAIL;
                }
            }
        }

        if (queried_dev) queried_dev->Release();

        if (result_hr == OS_DSTORAGE_S_OK) {
            GetRuntime().gpu_texture_uploads.fetch_add(1u, std::memory_order_relaxed);
            if (used_gpu_untiler) {
                GetRuntime().gpu_untiled_uploads.fetch_add(1u, std::memory_order_relaxed);
                RecordOk("DirectStorage::BCPackToXgGpuUntilerUpload");
            } else {
                RecordOk("DirectStorage::TextureUpload");
            }
            if (caller_desc) {
                caller_desc->out_used_gpu_untiler = used_gpu_untiler ? 1u : 0u;
                caller_desc->out_decompressed_bytes = static_cast<uint32_t>(unpacked.size());
                caller_desc->out_untiled_bytes = static_cast<uint32_t>(upload_src_size);
                caller_desc->out_uploaded_bytes = static_cast<uint32_t>(row_size_bytes * num_rows);
            }
        }
        return result_hr;
    }
#endif

    int32_t ExecuteRequest(
        const OpenScarlettDStorageRequestDesc& req,
        const std::atomic<bool>* cancel_flag) {
        auto is_canceled = [&]() -> bool {
            return cancel_flag && cancel_flag->load(std::memory_order_acquire);
        };
        if (is_canceled()) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        const size_t staging_limit =
            static_cast<size_t>(staging_size_ref_->load(std::memory_order_acquire));

        // Handle GPU texture & buffer destinations (automatic BCPack/GDeflate -> XgGpuUntiler upload).
        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TILES ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
#if defined(_WIN32)
            std::vector<uint8_t> file_buf;
            const uint8_t* src_ptr = nullptr;
            size_t src_size = 0u;
            const int32_t src_hr = AcquireSourceBytes(req, staging_limit, file_buf, src_ptr, src_size, cancel_flag);
            if (src_hr < 0) return src_hr;
            if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
                return ExecuteGpuBufferRequest(req, src_ptr, src_size, staging_limit, cancel_flag);
            }
            return ExecuteGpuTextureRequest(req, src_ptr, src_size, staging_limit, cancel_flag);
#else
            return OS_DSTORAGE_E_INVALIDARG;
#endif
        }

        if (req.Options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY ||
            !req.Destination.Memory.Buffer || req.Destination.Memory.Size == 0) {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        auto* dst_ptr = static_cast<uint8_t*>(req.Destination.Memory.Buffer);
        const size_t dst_cap = req.Destination.Memory.Size;
        DestinationClassification dst_class{};
        const int32_t dst_hr = ValidateAndClassifyDestination(dst_ptr, dst_cap, dst_class);
        if (dst_hr < 0) {
            return dst_hr;
        }

        std::vector<uint8_t> file_buf;
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0;
        const int32_t src_hr = AcquireSourceBytes(req, staging_limit, file_buf, src_ptr, src_size, cancel_flag);
        if (src_hr < 0) {
            return src_hr;
        }

        if (is_canceled()) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        auto record_destination_write = [&]() {
            if (dst_class.is_physical_page) {
                physical_page_writes_.fetch_add(1u, std::memory_order_relaxed);
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            if (dst_class.is_xmem) {
                xmem_destination_writes_.fetch_add(1u, std::memory_order_relaxed);
            }
        };

        // Check registered custom decompressor callbacks first.
        const uint8_t fmt_u8 = static_cast<uint8_t>(req.Options.CompressionFormat);
        CustomDecompressorEntry custom{};
        if (GetRuntime().LookupCustom(fmt_u8, custom)) {
            if (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            if (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap) {
                return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
            }
            size_t out_sz = 0u;
            const int32_t chr = custom.callback(
                fmt_u8, src_ptr, src_size, dst_ptr, dst_cap, &out_sz, custom.user_context);
            if (chr < 0) return chr;
            if (out_sz > dst_cap) return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
            if (req.UncompressedSize != 0u && out_sz != req.UncompressedSize) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(out_sz, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().custom_decompressed.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::CustomDecompress");
            record_destination_write();
            return OS_DSTORAGE_S_OK;
        }

        switch (req.Options.CompressionFormat) {
            case DSTORAGE_COMPRESSION_FORMAT_NONE: {
                if (src_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && req.UncompressedSize != src_size) {
                    return OS_DSTORAGE_E_INVALIDARG;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                constexpr size_t kCopyChunk = 65536u;
                size_t copied = 0u;
                while (copied < src_size) {
                    if (is_canceled()) {
                        return OS_DSTORAGE_E_CANCELLED;
                    }
                    const size_t step = std::min(src_size - copied, kCopyChunk);
                    std::memcpy(dst_ptr + copied, src_ptr + copied, step);
                    copied += step;
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
                size_t header_uncompressed = 0u;
                if (!openscarlett::GDeflateIsValidStream(src_ptr, src_size) ||
                    !openscarlett::GDeflateGetUncompressedSize(src_ptr, src_size, &header_uncompressed) ||
                    header_uncompressed == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(
                    static_cast<uint32_t>(std::min<size_t>(header_uncompressed, UINT32_MAX)),
                    std::memory_order_release);
                if (header_uncompressed > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (header_uncompressed > dst_cap ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap)) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && header_uncompressed != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }

                size_t out_size = 0u;
                if (dst_class.is_physical_page) {
                    const auto res = openscarlett::GDeflateDecompress(
                        src_ptr, src_size, dst_ptr, dst_cap, &out_size);
                    if (res != openscarlett::GDeflateResult::Ok) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    if (req.UncompressedSize != 0u && out_size != req.UncompressedSize) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    record_destination_write();
                    return OS_DSTORAGE_S_OK;
                }

                std::vector<uint8_t> staging(header_uncompressed);
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, staging.data(), staging.size(), &out_size);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (out_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && out_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (out_size > 0u) {
                    std::memcpy(dst_ptr, staging.data(), out_size);
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_ZLIB: {
                if (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                const size_t max_out = std::min(dst_cap, staging_limit);
                std::vector<uint8_t> unpacked;
                if (zlib_rfc1950::Decompress(src_ptr, src_size, max_out, unpacked)) {
                    if (req.UncompressedSize != 0u && unpacked.size() != req.UncompressedSize) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                    last_intermediate_size_.store(
                        static_cast<uint32_t>(std::min<size_t>(unpacked.size(), UINT32_MAX)),
                        std::memory_order_release);
                    if (!unpacked.empty()) {
                        std::memcpy(dst_ptr, unpacked.data(), unpacked.size());
                    }
                    GetRuntime().total_zlib_decompressions.fetch_add(1u, std::memory_order_relaxed);
                    record_destination_write();
                    return OS_DSTORAGE_S_OK;
                }
                // Legacy host-extension fallback only when explicit BCPK container is passed.
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (static_cast<size_t>(hdr.original_size) > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                if (!openscarlett::BcPackDecompress(src_ptr, src_size, unpacked).ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (unpacked.size() > dst_cap) return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                if (!unpacked.empty()) {
                    std::memcpy(dst_ptr, unpacked.data(), unpacked.size());
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }
            case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (static_cast<size_t>(hdr.original_size) > dst_cap ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap)) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }

                std::vector<uint8_t> unpacked;
                const auto st = openscarlett::BcPackDecompress(src_ptr, src_size, unpacked);
                if (!st.ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (unpacked.size() > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && unpacked.size() != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (!unpacked.empty()) {
                    std::memcpy(dst_ptr, unpacked.data(), unpacked.size());
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            default:
                return OS_DSTORAGE_E_INVALIDARG;
        }
    }

    void RecordFailure(int32_t hr, const QueueItem& item) {
        const OpenScarlettDStorageRequestDesc& req = item.request;
        std::lock_guard<std::mutex> lk(error_mutex_);
        if (error_record_.FailureCount == 0) {
            auto& ff = error_record_.FirstFailure;
            ff.HResult = hr;
            ff.CommandType = DSTORAGE_COMMAND_TYPE_REQUEST;
            auto& r = ff.Request;
            r.Options.AsUINT64 = item.xbox_options;
            if (item.xbox_options == 0u &&
                req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
                r.Options.SourceType = 1u;
            }
            r.Destination = req.Destination.Memory.Buffer;
            r.DestinationSize = req.Destination.Memory.Size;
            if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
                r.FileOffset = req.Source.File.Offset;
                r.SourceSize = req.Source.File.Size;
            } else {
                r.Source = const_cast<void*>(req.Source.Memory.Source);
                r.SourceSize = req.Source.Memory.Size;
            }
            r.CancellationTag = req.CancellationTag;
            if (req.Name) {
                size_t i = 0;
                for (; i + 1u < DSTORAGE_REQUEST_MAX_NAME && req.Name[i] != '\0'; ++i) {
                    r.Name[i] = req.Name[i];
                }
                r.Name[i] = '\0';
            }
        }
        ++error_record_.FailureCount;
#if defined(_WIN32)
        if (error_event_) {
            SetEvent(error_event_);
        }
#endif
    }

    void CloseInternal() {
        std::deque<QueueItem> leftover_staged;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (closed_) return;
            closed_ = true;
            leftover_staged.swap(staged_);
            staged_request_count_ = 0u;
            space_cv_.notify_all();
        }
        for (auto& item : leftover_staged) {
            if (item.kind == QueueItem::Kind::Request &&
                item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                item.request.Source.File.Source) {
                item.request.Source.File.Source->Release();
            } else if (item.kind == QueueItem::Kind::Status && item.status_array) {
                item.status_array->MarkComplete(item.status_index, OS_DSTORAGE_E_FAIL);
                item.status_array->Release();
            }
        }

        // Wait until all already-submitted active items & in-flight workers drain cleanly before unregistering.
        DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        {
            std::unique_lock<std::mutex> lk(mutex_);
            drain_cv_.wait(lk, [&]() {
                return active_.empty() && in_flight_requests_ == 0u && in_flight_barriers_ == 0u;
            });
        }
        DStoragePriorityScheduler::Global().UnregisterQueue(this);

#if defined(_WIN32)
        {
            std::lock_guard<std::mutex> glk(gpu_ctx_.gpu_mutex);
            gpu_ctx_.Reset();
        }
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    DSTORAGE_QUEUE_DESC desc_{};
    uint32_t queue_options_ = DSTORAGE_QUEUE_OPTIONS_NONE;
    bool auto_submit_disabled_ = false;
    void* device_ = nullptr;  // host ID3D12Device (OpenScarlettDStorageSetQueueDevice)
    std::atomic<void*> title_va_mirror_{nullptr};
    std::shared_ptr<std::atomic<uint32_t>> staging_size_ref_;
    uint16_t auto_submit_threshold_ = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;
    std::string name_storage_;

    std::mutex mutex_;
    std::condition_variable space_cv_;
    std::condition_variable drain_cv_;
    bool closed_ = false;
    uint32_t staged_request_count_ = 0u;
    std::atomic<size_t> active_count_{0u};
    size_t in_flight_requests_ = 0u;
    size_t in_flight_barriers_ = 0u;
    int32_t batch_hr_ = OS_DSTORAGE_S_OK;
    std::vector<std::shared_ptr<InFlightRequestState>> in_flight_states_;
    uint64_t staged_canceled_count_ = 0u;
    uint64_t submitted_canceled_count_ = 0u;
    uint64_t inflight_canceled_count_ = 0u;
    std::atomic<uint64_t> completed_ok_count_{0u};
    std::atomic<uint64_t> completed_failed_count_{0u};
    std::atomic<uint64_t> completed_canceled_count_{0u};
    std::atomic<uint64_t> physical_page_writes_{0u};
    std::atomic<uint64_t> xmem_destination_writes_{0u};
    std::atomic<uint32_t> last_intermediate_size_{0u};
    std::deque<QueueItem> staged_;
    std::deque<QueueItem> active_;

#if defined(_WIN32)
    DStorageGpuUploadContext gpu_ctx_;
#endif

    std::mutex error_mutex_;
    DSTORAGE_ERROR_RECORD error_record_{};
#if defined(_WIN32)
    HANDLE error_event_ = nullptr;
#endif
};

// ===========================================================================
// Multi-Threaded DStoragePriorityScheduler Method Definitions
// ===========================================================================
void DStoragePriorityScheduler::RegisterQueue(DStorageQueueImpl* q, DSTORAGE_PRIORITY priority) {
    std::lock_guard<std::mutex> lk(mutex_);
    slots_.push_back(Slot{q, PriorityToBucket(priority), 0u, false});
    if (!worker_running_) {
        stop_ = false;
        worker_running_ = true;
        const unsigned hw = std::thread::hardware_concurrency();
        const size_t num_workers = std::clamp<size_t>(hw ? hw : 4u, 2u, 4u);
        workers_.reserve(num_workers);
        for (size_t i = 0u; i < num_workers; ++i) {
            workers_.emplace_back(&DStoragePriorityScheduler::RunLoop, this);
        }
    }
    work_cv_.notify_all();
}

void DStoragePriorityScheduler::UnregisterQueue(DStorageQueueImpl* q) {
    std::vector<std::thread> to_join;
    {
        std::unique_lock<std::mutex> lk(mutex_);
        for (auto& s : slots_) {
            if (s.queue == q) {
                s.removing = true;
                break;
            }
        }
        idle_cv_.wait(lk, [&]() {
            for (const auto& s : slots_) {
                if (s.queue == q) return s.active_workers == 0u;
            }
            return true;
        });
        slots_.erase(
            std::remove_if(slots_.begin(), slots_.end(),
                           [q](const Slot& s) { return s.queue == q; }),
            slots_.end());

        if (slots_.empty() && worker_running_) {
            stop_ = true;
            worker_running_ = false;
            work_cv_.notify_all();
            to_join.swap(workers_);
        }
    }
    for (auto& w : to_join) {
        if (w.joinable()) {
            if (w.get_id() != std::this_thread::get_id()) {
                w.join();
            } else {
                w.detach();
            }
        }
    }
}

void DStoragePriorityScheduler::NotifyWorkAvailable() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
    }
    work_cv_.notify_all();
}

void DStoragePriorityScheduler::RunLoop() {
    for (;;) {
        DStorageQueueImpl* chosen = nullptr;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            auto pick_next = [&]() -> DStorageQueueImpl* {
                // Strictly scan REALTIME(0) > HIGH(1) > NORMAL(2) > LOW(3).
                // Higher-priority queues get full multi-worker concurrency; if any higher-priority
                // bucket has pending work (even waiting on its own final barrier), lower buckets
                // never steal workers ahead of it.
                for (size_t b = 0u; b < 4u; ++b) {
                    const size_t n = slots_.size();
                    if (n == 0u) continue;
                    bool bucket_busy = false;
                    const size_t start = rr_cursor_[b] % n;
                    for (size_t off = 0u; off < n; ++off) {
                        const size_t idx = (start + off) % n;
                        Slot& s = slots_[idx];
                        if (s.bucket != b || s.removing) continue;
                        if (s.queue->HasPendingActiveOrInFlight()) {
                            bucket_busy = true;
                        }
                        // REALTIME/HIGH (0..1) allow full multi-worker intra-queue parallelism;
                        // NORMAL/LOW (2..3) cap at 1 concurrent worker per queue so they cannot
                        // starve or outrun higher-priority preemption checks.
                        const uint32_t max_w = (b <= 1u) ? 4u : 1u;
                        if (s.active_workers < max_w && s.queue->CanDispatchNow()) {
                            ++s.active_workers;
                            rr_cursor_[b] = (idx + 1u) % n;
                            return s.queue;
                        }
                    }
                    if (bucket_busy) {
                        // Do not drop into a lower-priority bucket while this higher bucket is active.
                        return nullptr;
                    }
                }
                return nullptr;
            };

            work_cv_.wait(lk, [&]() {
                if (stop_) return true;
                chosen = pick_next();
                return chosen != nullptr;
            });

            if (stop_ && !chosen) {
                return;
            }
        }

        if (chosen) {
            chosen->ProcessOneActiveItem();
            bool notify_next = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (auto& s : slots_) {
                    if (s.queue == chosen) {
                        if (s.active_workers > 0u) --s.active_workers;
                        if (s.active_workers == 0u) {
                            notify_next = true;
                            if (s.removing) {
                                idle_cv_.notify_all();
                            }
                        }
                        break;
                    }
                }
            }
            if (notify_next) {
                work_cv_.notify_all();
            }
        }
    }
}

// ===========================================================================
// IDStorageFactoryX / X1 / X2 Implementation
// ===========================================================================
class DStorageFactoryImpl final : public IDStorageFactoryX2 {
public:
    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageFactoryX) ||
            GuidEquals(riid, IID_IDStorageFactoryX_Xbox)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFactoryX*>(this);
            return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        if (GuidEquals(riid, IID_IDStorageFactoryX1)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFactoryX1*>(this);
            return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        if (GuidEquals(riid, IID_IDStorageFactoryX2)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFactoryX2*>(this);
            return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageFactoryX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageFactoryX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc, const DStorageGuid* riid, void** ppv) override {
        if (!desc || !ppv) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_INVALIDARG, 3);
        }
        *ppv = nullptr;
        if (riid &&
            !GuidEquals(riid, IID_IDStorageQueueX) &&
            !GuidEquals(riid, IID_IDStorageQueueX_Xbox) &&
            !GuidEquals(riid, IID_IDStorageQueueX1) &&
            !GuidEquals(riid, IID_IDStorageQueueX2) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_NOINTERFACE, 3);
        }
        if (desc->SourceType != DSTORAGE_REQUEST_SOURCE_FILE &&
            desc->SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_INVALIDARG, 3);
        }
        auto* q = new (std::nothrow) DStorageQueueImpl(*desc, staging_size_, DSTORAGE_QUEUE_OPTIONS_NONE);
        if (!q) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_OUTOFMEMORY, 3);
        }
        *ppv = static_cast<IDStorageQueueX*>(q);
        return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_S_OK, 3);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL OpenFile(
        const wchar_t* path, const DStorageGuid* riid, void** ppv) override {
        if (!path || !*path || !ppv) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageFileX) &&
            !GuidEquals(riid, IID_IDStorageFileX_Xbox) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_NOINTERFACE, 4);
        }
#if defined(_WIN32)
        std::wstring open_path(path);
        if (openscarlett::VfsMounts::DetectVirtualDriveW(open_path) != openscarlett::VfsDrive::None) {
            auto read_env = [](const wchar_t* name) -> std::wstring {
                DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
                if (n == 0) return {};
                std::wstring v(n, L'\0');
                DWORD w = GetEnvironmentVariableW(name, v.data(), n);
                v.resize(w);
                return v;
            };
            openscarlett::VfsMounts vfs;
            openscarlett::VfsMountConfig cfg{};
            cfg.g_root = read_env(L"OPENSCARLETT_VFS_G");
            cfg.g_overlay_root = read_env(L"OPENSCARLETT_VFS_G_OVERLAY");
            cfg.t_root = read_env(L"OPENSCARLETT_VFS_T");
            cfg.s_root = read_env(L"OPENSCARLETT_VFS_S");
            vfs.Configure(cfg);
            std::wstring translated;
            if (vfs.TranslateForAccessW(open_path, openscarlett::VfsAccessMode::ReadOnly,
                                        translated, nullptr, false) ==
                openscarlett::VfsTranslateStatus::Translated) {
                open_path = std::move(translated);
            }
        }
        HANDLE h = CreateFileW(
            open_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_FILE_NOT_FOUND, 4);
        }
        const bool attached = DStorageIocpEngine::Global().AttachFile(h);
        auto* f = new (std::nothrow) DStorageFileImpl(h, attached);
        if (!f) {
            CloseHandle(h);
            if (attached) DStorageIocpEngine::Global().DetachFile();
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_OUTOFMEMORY, 4);
        }
        *ppv = static_cast<IDStorageFileX*>(f);
        return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_S_OK, 4);
#else
        return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_FAIL, 4);
#endif
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL CreateStatusArray(
        uint32_t capacity, const char* /*name*/, const DStorageGuid* riid, void** ppv) override {
        if (capacity == 0 || !ppv) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_INVALIDARG, 5);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageStatusArrayX) &&
            !GuidEquals(riid, IID_IDStorageStatusArrayX_Xbox) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_NOINTERFACE, 5);
        }
        auto* arr = new (std::nothrow) DStorageStatusArrayImpl(capacity);
        if (!arr) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_OUTOFMEMORY, 5);
        }
        *ppv = static_cast<IDStorageStatusArrayX*>(arr);
        return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_S_OK, 5);
    }

    void OPENSCARLETT_DSTORAGE_CALL SetDebugFlags(uint32_t flags) override {
        RecordOk("IDStorageFactoryX::SetDebugFlags", 6);
        debug_flags_.store(flags, std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetStagingBufferSize(uint32_t size) override {
        if (size < DSTORAGE_MIN_STAGING_BUFFER_SIZE || size > DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
            return RecordHr("IDStorageFactoryX::SetStagingBufferSize",
                            OS_DSTORAGE_E_STAGING_BUFFER_INVALID, 7);
        }
        staging_size_->store(size, std::memory_order_release);
        return RecordHr("IDStorageFactoryX::SetStagingBufferSize", OS_DSTORAGE_S_OK, 7);
    }

    // IDStorageFactoryX1 (slots 8..9)
    int32_t OPENSCARLETT_DSTORAGE_CALL GetStats(DSTORAGE_STATS* stats) override {
        if (!stats) {
            return RecordHr("IDStorageFactoryX1::GetStats", OS_DSTORAGE_E_INVALIDARG, 8);
        }
        auto& rt = GetRuntime();
        stats->TotalRequestsCompleted = rt.total_requests_completed.load(std::memory_order_acquire);
        stats->TotalBytesRead = rt.total_bytes_read.load(std::memory_order_acquire);
        stats->TotalIocpAsyncReads = rt.total_iocp_async_reads.load(std::memory_order_acquire);
        stats->TotalOver64GiBReads = rt.total_over_64gib_reads.load(std::memory_order_acquire);
        stats->MaxConcurrentIocpReads = rt.max_concurrent_iocp_reads.load(std::memory_order_acquire);
        stats->TotalZlibDecompressions = rt.total_zlib_decompressions.load(std::memory_order_acquire);
        stats->TotalBcpackDecompressions = rt.total_bcpack_decompressions.load(std::memory_order_acquire);
        stats->TotalSwizzleTransforms = rt.total_swizzle_transforms.load(std::memory_order_acquire);
        stats->TotalPhysicalPageTransfers = rt.total_physical_page_transfers.load(std::memory_order_acquire);
        return RecordHr("IDStorageFactoryX1::GetStats", OS_DSTORAGE_S_OK, 8);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetCpuAffinity(uint64_t affinity) override {
        if (affinity == 0ull) {
            return RecordHr("IDStorageFactoryX1::SetCpuAffinity", OS_DSTORAGE_E_INVALIDARG, 9);
        }
        cpu_affinity_.store(affinity, std::memory_order_relaxed);
        return RecordHr("IDStorageFactoryX1::SetCpuAffinity", OS_DSTORAGE_S_OK, 9);
    }

    // IDStorageFactoryX2 (slot 10)
    int32_t OPENSCARLETT_DSTORAGE_CALL CreateQueue1(
        const DSTORAGE_QUEUE_DESC1* desc, const DStorageGuid* riid, void** ppv) override {
        if (!desc || !ppv) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_INVALIDARG, 10);
        }
        *ppv = nullptr;
        if (riid &&
            !GuidEquals(riid, IID_IDStorageQueueX) &&
            !GuidEquals(riid, IID_IDStorageQueueX_Xbox) &&
            !GuidEquals(riid, IID_IDStorageQueueX1) &&
            !GuidEquals(riid, IID_IDStorageQueueX2) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_NOINTERFACE, 10);
        }
        if (desc->SourceType != DSTORAGE_REQUEST_SOURCE_FILE &&
            desc->SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_INVALIDARG, 10);
        }
        if ((desc->Options & ~static_cast<uint32_t>(DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT)) != 0u) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_INVALIDARG, 10);
        }
        DSTORAGE_QUEUE_DESC base_desc{};
        base_desc.SourceType = desc->SourceType;
        base_desc.Capacity = desc->Capacity;
        base_desc.Priority = desc->Priority;
        base_desc.Name = desc->Name;
        auto* q = new (std::nothrow) DStorageQueueImpl(base_desc, staging_size_, desc->Options);
        if (!q) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_OUTOFMEMORY, 10);
        }
        *ppv = static_cast<IDStorageQueueX2*>(q);
        return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_S_OK, 10);
    }

    uint32_t GetStagingBufferSizeValue() const {
        return staging_size_->load(std::memory_order_acquire);
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> debug_flags_{0};
    std::atomic<uint64_t> cpu_affinity_{0xFFFFull};
    std::shared_ptr<std::atomic<uint32_t>> staging_size_{
        std::make_shared<std::atomic<uint32_t>>(DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE)};
};

struct GlobalDStorageConfig {
    std::mutex mutex;
    DSTORAGE_CONFIGURATION1 config{};
};

GlobalDStorageConfig& GetConfig() {
    static GlobalDStorageConfig cfg;
    return cfg;
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageGetFactory(const DStorageGuid* riid, void** ppv) {
    if (!ppv) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_POINTER);
    }
    *ppv = nullptr;
    if (riid &&
        !GuidEquals(riid, IID_IDStorageFactoryX) &&
        !GuidEquals(riid, IID_IDStorageFactoryX_Xbox) &&
        !GuidEquals(riid, IID_IDStorageFactoryX1) &&
        !GuidEquals(riid, IID_IDStorageFactoryX2) &&
        !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_NOINTERFACE);
    }
    auto* f = new (std::nothrow) DStorageFactoryImpl();
    if (!f) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_OUTOFMEMORY);
    }
    *ppv = static_cast<IDStorageFactoryX*>(f);
    return RecordHr("DStorageGetFactory", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration(
    const DSTORAGE_CONFIGURATION* configuration) {
    if (!configuration) {
        return RecordHr("DStorageSetConfiguration", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& cfg = GetConfig();
    std::lock_guard<std::mutex> lk(cfg.mutex);
    cfg.config.NumSubmitThreads = configuration->NumSubmitThreads;
    cfg.config.NumBuiltInCpuDecompressionThreads = configuration->NumBuiltInCpuDecompressionThreads;
    cfg.config.ForceMappingLayer = configuration->ForceMappingLayer;
    cfg.config.DisableBypassIO = configuration->DisableBypassIO;
    cfg.config.DisableTelemetry = configuration->DisableTelemetry;
    cfg.config.DisableGpuDecompressionMetacommand = configuration->DisableGpuDecompressionMetacommand;
    cfg.config.DisableGpuDecompression = configuration->DisableGpuDecompression;
    return RecordHr("DStorageSetConfiguration", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration1(
    const DSTORAGE_CONFIGURATION1* configuration) {
    if (!configuration) {
        return RecordHr("DStorageSetConfiguration1", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& cfg = GetConfig();
    std::lock_guard<std::mutex> lk(cfg.mutex);
    cfg.config = *configuration;
    return RecordHr("DStorageSetConfiguration1", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageEnqueueRequestDesc(
    IDStorageQueueX* queue, const OpenScarlettDStorageRequestDesc* request) {
    if (!queue || !request) {
        return RecordHr("OpenScarlettDStorageEnqueueRequestDesc", OS_DSTORAGE_E_INVALIDARG);
    }
    static_cast<DStorageQueueImpl*>(queue)->EnqueueRequestDesc(*request);
    return RecordHr("OpenScarlettDStorageEnqueueRequestDesc", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageSetQueueDevice(IDStorageQueueX* queue, void* d3d12_device) {
    if (!queue) {
        return RecordHr("OpenScarlettDStorageSetQueueDevice", OS_DSTORAGE_E_INVALIDARG);
    }
    static_cast<DStorageQueueImpl*>(queue)->SetDevice(d3d12_device);
    return RecordHr("OpenScarlettDStorageSetQueueDevice", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageSetQueueTitleVaMirror(
    IDStorageQueueX* queue, void* title_va_mirror) {
    if (!queue) {
        return RecordHr("OpenScarlettDStorageSetQueueTitleVaMirror", OS_DSTORAGE_E_INVALIDARG);
    }
    static_cast<DStorageQueueImpl*>(queue)->SetTitleVaMirror(title_va_mirror);
    return RecordHr("OpenScarlettDStorageSetQueueTitleVaMirror", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetQueueStats(
    IDStorageQueueX* queue, DSTORAGE_QUEUE_STATS* outStats) {
    if (!queue || !outStats) {
        return RecordHr("OpenScarlettDStorageGetQueueStats", OS_DSTORAGE_E_INVALIDARG);
    }
    std::memset(outStats, 0, sizeof(*outStats));
    auto* impl = static_cast<DStorageQueueImpl*>(queue);
    impl->GetQueueStats(*outStats);
    return RecordHr("OpenScarlettDStorageGetQueueStats", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetStagingBufferSize(
    IDStorageFactoryX* factory, uint32_t* outSize) {
    if (!factory || !outSize) {
        return RecordHr("OpenScarlettDStorageGetStagingBufferSize", OS_DSTORAGE_E_INVALIDARG);
    }
    auto* impl = static_cast<DStorageFactoryImpl*>(factory);
    *outSize = impl->GetStagingBufferSizeValue();
    return RecordHr("OpenScarlettDStorageGetStagingBufferSize", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageRegisterCustomDecompressor(
    uint8_t format,
    PfnOpenScarlettDStorageCustomDecompress callback,
    void* user_context) {
    if (!callback) {
        return RecordHr("OpenScarlettDStorageRegisterCustomDecompressor", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.custom_mutex);
    rt.custom_decompressors[format] = CustomDecompressorEntry{callback, user_context};
    return RecordHr("OpenScarlettDStorageRegisterCustomDecompressor", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageUnregisterCustomDecompressor(uint8_t format) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.custom_mutex);
    rt.custom_decompressors.erase(format);
    return RecordHr("OpenScarlettDStorageUnregisterCustomDecompressor", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetPriorityStats(
    OpenScarlettDStoragePriorityStats* out_stats) {
    if (!out_stats) {
        return RecordHr("OpenScarlettDStorageGetPriorityStats", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    out_stats->realtime_dispatched = rt.realtime_dispatched.load(std::memory_order_acquire);
    out_stats->high_dispatched = rt.high_dispatched.load(std::memory_order_acquire);
    out_stats->normal_dispatched = rt.normal_dispatched.load(std::memory_order_acquire);
    out_stats->low_dispatched = rt.low_dispatched.load(std::memory_order_acquire);
    out_stats->custom_decompressed = rt.custom_decompressed.load(std::memory_order_acquire);
    out_stats->gpu_texture_uploads = rt.gpu_texture_uploads.load(std::memory_order_acquire);
    out_stats->gpu_untiled_uploads = rt.gpu_untiled_uploads.load(std::memory_order_acquire);
    return RecordHr("OpenScarlettDStorageGetPriorityStats", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageDecompressWithIntermediate(
    DSTORAGE_COMPRESSION_FORMAT format,
    const void* src,
    size_t src_size,
    void* dst,
    size_t dst_size,
    size_t intermediate_size,
    size_t* out_decompressed_size) {
    if (out_decompressed_size) {
        *out_decompressed_size = 0u;
    }
    if (!src || !dst || src_size == 0u || dst_size == 0u) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_E_INVALIDARG);
    }
    if (intermediate_size == 0u || intermediate_size > DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                        OS_DSTORAGE_E_STAGING_BUFFER_INVALID);
    }

    DestinationClassification dst_class{};
    const int32_t dst_hr = ValidateAndClassifyDestination(dst, dst_size, dst_class);
    if (dst_hr < 0) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", dst_hr);
    }

    const auto* src_bytes = static_cast<const uint8_t*>(src);
    auto* dst_bytes = static_cast<uint8_t*>(dst);

    // Check custom decompressor registry first.
    CustomDecompressorEntry custom{};
    if (GetRuntime().LookupCustom(static_cast<uint8_t>(format), custom)) {
        std::vector<uint8_t> staging(intermediate_size);
        size_t out_sz = 0u;
        const int32_t chr = custom.callback(
            static_cast<uint8_t>(format),
            src_bytes, src_size,
            staging.data(), staging.size(),
            &out_sz,
            custom.user_context);
        if (chr < 0) {
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", chr);
        }
        if (out_sz > dst_size) {
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                            OS_DSTORAGE_E_REQUEST_TOO_LARGE);
        }
        if (out_sz > 0u) {
            std::memcpy(dst_bytes, staging.data(), out_sz);
        }
        if (out_decompressed_size) *out_decompressed_size = out_sz;
        GetRuntime().custom_decompressed.fetch_add(1u, std::memory_order_relaxed);
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
    }

    switch (format) {
        case DSTORAGE_COMPRESSION_FORMAT_NONE: {
            if (intermediate_size < src_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < src_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            std::memcpy(dst_bytes, src_bytes, src_size);
            if (out_decompressed_size) {
                *out_decompressed_size = src_size;
            }
            if (dst_class.is_physical_page) {
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
            size_t required = 0u;
            if (!openscarlett::GDeflateIsValidStream(src_bytes, src_size) ||
                !openscarlett::GDeflateGetUncompressedSize(src_bytes, src_size, &required) ||
                required == 0u) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (intermediate_size < required) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < required) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            size_t out_sz = 0u;
            if (dst_class.is_physical_page) {
                const auto res = openscarlett::GDeflateDecompress(
                    src_bytes, src_size, dst_bytes, dst_size, &out_sz);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_DECOMPRESSION_ERROR);
                }
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            } else {
                std::vector<uint8_t> staging(intermediate_size);
                const auto res = openscarlett::GDeflateDecompress(
                    src_bytes, src_size, staging.data(), staging.size(), &out_sz);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_DECOMPRESSION_ERROR);
                }
                if (out_sz > dst_size) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_REQUEST_TOO_LARGE);
                }
                std::memcpy(dst_bytes, staging.data(), out_sz);
            }
            if (out_decompressed_size) {
                *out_decompressed_size = out_sz;
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
        case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
            openscarlett::BcPackHeader hdr{};
            if (!openscarlett::BcPackParseHeader(src_bytes, src_size, hdr).ok() ||
                hdr.original_size == 0u) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (intermediate_size < static_cast<size_t>(hdr.original_size)) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < static_cast<size_t>(hdr.original_size)) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            std::vector<uint8_t> unpacked;
            if (!openscarlett::BcPackDecompress(src_bytes, src_size, unpacked).ok()) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (unpacked.size() > dst_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            if (!unpacked.empty()) {
                std::memcpy(dst_bytes, unpacked.data(), unpacked.size());
            }
            if (out_decompressed_size) {
                *out_decompressed_size = unpacked.size();
            }
            if (dst_class.is_physical_page) {
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        default:
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                            OS_DSTORAGE_E_INVALIDARG);
    }
}

}  // extern "C"
