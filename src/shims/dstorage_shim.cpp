#define OPENSCARLETT_BUILDING_DSTORAGE 1
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/kernel/vram_residency_manager.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/io/zlib_rfc1950.h"
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

// Inline explicit D3D12 COM IIDs (portable across MSVC, Clang-CL, and MinGW-w64; zero __uuidof).
static constexpr GUID kOsDStorageIidId3d12Device = {
    0x189819F1u, 0x1DB6u, 0x4B57u, {0xBE, 0x54u, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
static constexpr GUID kOsDStorageIidId3d12Resource = {
    0x696442BEu, 0xA72Eu, 0x4059u, {0xBC, 0x79u, 0x5B, 0x5C, 0x98, 0x04, 0x0F, 0xAD}};
static constexpr GUID kOsDStorageIidId3d12CommandQueue = {
    0x0EC870A6u, 0x5D7Eu, 0x4C22u, {0x8C, 0xFCu, 0x5B, 0xAA, 0xE0, 0x76, 0x16, 0xED}};
static constexpr GUID kOsDStorageIidId3d12CommandAllocator = {
    0x6102DEE4u, 0xAF59u, 0x4B09u, {0xB9, 0x99u, 0xB4, 0x4D, 0x73, 0xF0, 0x9B, 0x24}};
static constexpr GUID kOsDStorageIidId3d12GraphicsCommandList = {
    0x5B160D0Fu, 0xAC1Bu, 0x4185u, {0x8B, 0xA8u, 0xB3, 0xAE, 0x42, 0xA5, 0xA4, 0x55}};
static constexpr GUID kOsDStorageIidId3d12Fence = {
    0x0A753DCFu, 0xC4D8u, 0x4B91u, {0xAD, 0xF6u, 0xBE, 0x5A, 0x60, 0xD9, 0x5A, 0x76}};
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
using PfnKxVirtualAllocFromApp = void*(__stdcall*)(void*, size_t, uint32_t, uint32_t);
using PfnXMemResolveCpuAliasVa = uint64_t(__stdcall*)(uint64_t);

constexpr uint32_t kKxMemGraphics = 0x10000000u;

PfnXMemResolveCpuAliasVa GetXMemResolveFn() noexcept {
    static std::atomic<PfnXMemResolveCpuAliasVa> s_fn{nullptr};
    static std::atomic<uint64_t> s_next_probe_ms{0u};
    PfnXMemResolveCpuAliasVa fn = s_fn.load(std::memory_order_acquire);
    if (fn) {
        return fn;
    }
    const uint64_t now_ms = ::GetTickCount64();
    const uint64_t next_ms = s_next_probe_ms.load(std::memory_order_relaxed);
    if (now_ms < next_ms) {
        return nullptr;
    }
    s_next_probe_ms.store(now_ms + 250ull, std::memory_order_relaxed);
    if (HMODULE hm = ::GetModuleHandleW(L"xmem.dll")) {
        fn = reinterpret_cast<PfnXMemResolveCpuAliasVa>(
            reinterpret_cast<void*>(::GetProcAddress(hm, "OpenScarlett_XMemResolveCpuAliasVa")));
        if (fn) {
            s_fn.store(fn, std::memory_order_release);
        }
    }
    return fn;
}

void* ResolveCpuAliasPointer(void* ptr) noexcept {
    if (!ptr) {
        return nullptr;
    }
    if (PfnXMemResolveCpuAliasVa fn = GetXMemResolveFn()) {
        const uint64_t orig = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
        const uint64_t resolved = fn(orig);
        if (resolved != 0ull) {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(resolved));
        }
    }
    return ptr;
}

const void* ResolveCpuAliasConstPointer(const void* ptr) noexcept {
    return ResolveCpuAliasPointer(const_cast<void*>(ptr));
}

struct CachedKernelxExports {
    PfnQueryTitlePhysicalPages query_phys = nullptr;
    PfnXMemSize xmem_size = nullptr;
    PfnMapTitleVirtualMemory map_title_vm = nullptr;
    PfnUnmapTitleVirtualMemory unmap_title_vm = nullptr;
    PfnKxVirtualAllocFromApp virtual_alloc_app = nullptr;
};

CachedKernelxExports GetKernelxExports() noexcept {
    static std::atomic<HMODULE> s_mod{nullptr};
    static std::atomic<PfnQueryTitlePhysicalPages> s_query{nullptr};
    static std::atomic<PfnXMemSize> s_xmem_size{nullptr};
    static std::atomic<PfnMapTitleVirtualMemory> s_map{nullptr};
    static std::atomic<PfnUnmapTitleVirtualMemory> s_unmap{nullptr};
    static std::atomic<PfnKxVirtualAllocFromApp> s_valloc{nullptr};
    static std::atomic<uint64_t> s_next_probe_ms{0u};

    CachedKernelxExports out{};
    out.query_phys = s_query.load(std::memory_order_acquire);
    out.xmem_size = s_xmem_size.load(std::memory_order_acquire);
    out.map_title_vm = s_map.load(std::memory_order_acquire);
    out.unmap_title_vm = s_unmap.load(std::memory_order_acquire);
    out.virtual_alloc_app = s_valloc.load(std::memory_order_acquire);
    if (s_mod.load(std::memory_order_acquire) != nullptr) {
        return out;
    }

    const uint64_t now_ms = ::GetTickCount64();
    const uint64_t next_ms = s_next_probe_ms.load(std::memory_order_relaxed);
    if (now_ms < next_ms) {
        return out;
    }
    s_next_probe_ms.store(now_ms + 250ull, std::memory_order_relaxed);

    if (HMODULE h_kx = ::GetModuleHandleW(L"kernelx.dll")) {
        out.query_phys = reinterpret_cast<PfnQueryTitlePhysicalPages>(
            reinterpret_cast<void*>(::GetProcAddress(h_kx, "QueryTitlePhysicalPages")));
        out.xmem_size = reinterpret_cast<PfnXMemSize>(
            reinterpret_cast<void*>(::GetProcAddress(h_kx, "XMemSize")));
        out.map_title_vm = reinterpret_cast<PfnMapTitleVirtualMemory>(
            reinterpret_cast<void*>(::GetProcAddress(h_kx, "MapTitleVirtualMemory")));
        out.unmap_title_vm = reinterpret_cast<PfnUnmapTitleVirtualMemory>(
            reinterpret_cast<void*>(::GetProcAddress(h_kx, "UnmapTitleVirtualMemory")));
        out.virtual_alloc_app = reinterpret_cast<PfnKxVirtualAllocFromApp>(
            reinterpret_cast<void*>(::GetProcAddress(h_kx, "VirtualAllocFromApp")));
        if (out.query_phys) s_query.store(out.query_phys, std::memory_order_release);
        if (out.xmem_size) s_xmem_size.store(out.xmem_size, std::memory_order_release);
        if (out.map_title_vm) s_map.store(out.map_title_vm, std::memory_order_release);
        if (out.unmap_title_vm) s_unmap.store(out.unmap_title_vm, std::memory_order_release);
        if (out.virtual_alloc_app) s_valloc.store(out.virtual_alloc_app, std::memory_order_release);
        s_mod.store(h_kx, std::memory_order_release);
    }
    return out;
}

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

// ===========================================================================
// Zero-Alloc Reusable Per-Worker Scratch Buffers (16 MiB steady-state cap)
// Eliminates per-request NtAllocateVirtualMemory/NtFreeVirtualMemory churn
// across multi-GB .rpf world streaming on IOCP & decompression worker threads.
// ===========================================================================
constexpr size_t kMaxRetainedScratchBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxRetainedPageSlots = 4096u;

void TrimByteScratchIfOversized(std::vector<uint8_t>& v) noexcept {
    v.clear();
    if (v.capacity() > kMaxRetainedScratchBytes) {
        std::vector<uint8_t>().swap(v);
    }
}

void ReserveZlibOutputHint(std::vector<uint8_t>& out, size_t src_size, size_t target_hint) {
    const size_t cap_hint = std::min<size_t>({
        target_hint,
        kMaxRetainedScratchBytes,
        (src_size <= (SIZE_MAX - 65536u) / 32u) ? (src_size * 32u + 65536u) : kMaxRetainedScratchBytes,
    });
    if (out.capacity() < cap_hint) {
        out.reserve(cap_hint);
    }
}

struct WorkerScratchPool {
    bool req_in_use = false;
    bool phys_in_use = false;
    std::vector<uint8_t> buf_a;
    std::vector<uint8_t> buf_b;
    std::vector<uint8_t> buf_c;
    std::vector<uint64_t> pfns;
    std::vector<uint8_t*> page_ptrs;
};

WorkerScratchPool& GetWorkerScratchPool() noexcept {
    thread_local WorkerScratchPool tl_pool;
    return tl_pool;
}

class ScopedRequestScratch {
public:
    ScopedRequestScratch() noexcept : pool_(GetWorkerScratchPool()) {
        if (!pool_.req_in_use) {
            pool_.req_in_use = true;
            owns_pool_ = true;
            pool_.buf_a.clear();
            pool_.buf_b.clear();
            pool_.buf_c.clear();
        }
    }

    ~ScopedRequestScratch() {
        if (owns_pool_) {
            TrimByteScratchIfOversized(pool_.buf_a);
            TrimByteScratchIfOversized(pool_.buf_b);
            TrimByteScratchIfOversized(pool_.buf_c);
            pool_.req_in_use = false;
        }
    }

    ScopedRequestScratch(const ScopedRequestScratch&) = delete;
    ScopedRequestScratch& operator=(const ScopedRequestScratch&) = delete;

    std::vector<uint8_t>& a() noexcept { return owns_pool_ ? pool_.buf_a : fallback_a_; }
    std::vector<uint8_t>& b() noexcept { return owns_pool_ ? pool_.buf_b : fallback_b_; }
    std::vector<uint8_t>& c() noexcept { return owns_pool_ ? pool_.buf_c : fallback_c_; }

private:
    WorkerScratchPool& pool_;
    bool owns_pool_ = false;
    std::vector<uint8_t> fallback_a_;
    std::vector<uint8_t> fallback_b_;
    std::vector<uint8_t> fallback_c_;
};

class ScopedPhysPageScratch {
public:
    ScopedPhysPageScratch() noexcept : pool_(GetWorkerScratchPool()) {
        if (!pool_.phys_in_use) {
            pool_.phys_in_use = true;
            owns_pool_ = true;
            pool_.pfns.clear();
            pool_.page_ptrs.clear();
        }
    }

    ~ScopedPhysPageScratch() {
        if (owns_pool_) {
            pool_.pfns.clear();
            if (pool_.pfns.capacity() > kMaxRetainedPageSlots) {
                std::vector<uint64_t>().swap(pool_.pfns);
            }
            pool_.page_ptrs.clear();
            if (pool_.page_ptrs.capacity() > kMaxRetainedPageSlots) {
                std::vector<uint8_t*>().swap(pool_.page_ptrs);
            }
            pool_.phys_in_use = false;
        }
    }

    ScopedPhysPageScratch(const ScopedPhysPageScratch&) = delete;
    ScopedPhysPageScratch& operator=(const ScopedPhysPageScratch&) = delete;

    std::vector<uint64_t>& pfns() noexcept { return owns_pool_ ? pool_.pfns : fallback_pfns_; }
    std::vector<uint8_t*>& page_ptrs() noexcept { return owns_pool_ ? pool_.page_ptrs : fallback_ptrs_; }

private:
    WorkerScratchPool& pool_;
    bool owns_pool_ = false;
    std::vector<uint64_t> fallback_pfns_;
    std::vector<uint8_t*> fallback_ptrs_;
};

struct DestinationClassification {
    bool is_physical_page = false;
    bool is_xmem = false;
    uint64_t physical_id = 0;
    uint64_t physical_mapping_size = 0;
};

int32_t ValidateAndClassifyDestination(
    void*& dst_ptr,
    size_t dst_cap,
    DestinationClassification& out_class) {
    out_class = DestinationClassification{};
    if (!dst_ptr || dst_cap == 0u) {
        return OS_DSTORAGE_E_INVALIDARG;
    }

#if defined(_WIN32)
    dst_ptr = ResolveCpuAliasPointer(dst_ptr);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(dst_ptr);
    if (cursor + dst_cap < cursor) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
    const CachedKernelxExports kx = GetKernelxExports();
    uintptr_t first_alloc_base = cursor;
    bool captured_first_base = false;
    size_t remaining = dst_cap;
    while (remaining > 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE) {
            // Retail Scarlett titles (e.g. RAGE9 post-FTUE world streaming) stream directly into
            // MEM_RESERVE VA windows inside their physical/unified pool. Commit on demand.
            const uintptr_t res_end =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (res_end > cursor) {
                const uintptr_t page_base = cursor & ~static_cast<uintptr_t>(4095u);
                const uintptr_t want_end = cursor + std::min(remaining, static_cast<size_t>(res_end - cursor));
                const size_t commit_len =
                    std::min<size_t>(((want_end - page_base) + 4095u) & ~size_t(4095u),
                                     static_cast<size_t>(res_end - page_base));
                void* committed = ::VirtualAlloc(
                    reinterpret_cast<LPVOID>(page_base), commit_len, MEM_COMMIT, PAGE_READWRITE);
                if (!committed && kx.virtual_alloc_app) {
                    const uintptr_t gran_base = cursor & ~static_cast<uintptr_t>(65535u);
                    if (gran_base >= reinterpret_cast<uintptr_t>(mbi.BaseAddress) && res_end > gran_base) {
                        const size_t gran_len =
                            std::min<size_t>(((want_end - gran_base) + 65535u) & ~size_t(65535u),
                                             static_cast<size_t>(res_end - gran_base));
                        committed = kx.virtual_alloc_app(
                            reinterpret_cast<void*>(gran_base), gran_len, MEM_COMMIT, PAGE_READWRITE);
                    }
                }
                if (committed) {
                    (void)VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi));
                }
            }
        }
        if (mbi.State != MEM_COMMIT || !IsWin32ProtectWritable(mbi.Protect)) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (!captured_first_base) {
            first_alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            captured_first_base = true;
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

    if (kx.query_phys) {
        uint64_t phys_id = 0u;
        uint64_t map_size = 0u;
        uint32_t flags = 0u;
        if (kx.query_phys(dst_ptr, &phys_id, &map_size, &flags) != 0 && map_size > 0u) {
            out_class.is_physical_page = true;
            out_class.physical_id = phys_id;
            const uintptr_t start_va = reinterpret_cast<uintptr_t>(dst_ptr);
            const uint64_t off_in_view =
                (start_va >= first_alloc_base && (start_va - first_alloc_base) < map_size)
                    ? static_cast<uint64_t>(start_va - first_alloc_base)
                    : 0ull;
            uint64_t covered = map_size - off_in_view;
            // Walk adjacent scatter-mapped physical pages (e.g. TileHeap / RPF scatter pages
            // mapped 64 KiB at a time via XMemMapPhysicalPagesScatter, including unaligned starts).
            while (covered > 0u && covered < dst_cap) {
                uint64_t next_phys = 0u;
                uint64_t next_size = 0u;
                uint32_t next_flags = 0u;
                void* next_ptr = static_cast<uint8_t*>(dst_ptr) + covered;
                if (kx.query_phys(next_ptr, &next_phys, &next_size, &next_flags) == 0 ||
                    next_size == 0u) {
                    break;
                }
                covered += next_size;
            }
            out_class.physical_mapping_size = covered;
            if (covered > 0u && dst_cap > covered) {
                return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
            }
        }
    }
    if (kx.xmem_size) {
        const size_t xsz = kx.xmem_size(dst_ptr, 0u);
        if (xsz > 0u) {
            out_class.is_xmem = true;
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

namespace zlib_rfc1950 = openscarlett::zlib_rfc1950;

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

    ScopedPhysPageScratch phys_scratch;
    std::vector<uint64_t>& pfns = phys_scratch.pfns();
    pfns.resize(static_cast<size_t>(page_count));
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
        std::vector<uint8_t*>& page_ptrs = phys_scratch.page_ptrs();
        page_ptrs.assign(pfns.size(), nullptr);
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
    const CachedKernelxExports kx = GetKernelxExports();
    if (kx.map_title_vm && kx.unmap_title_vm) {
        const uint32_t prot = is_write ? PAGE_READWRITE : PAGE_READONLY;
        constexpr uint64_t kMaxWindowPages = 256ull;  // 16 MiB VA window per chunk
        if (page_count <= kMaxWindowPages) {
            void* mapped = kx.map_title_vm(nullptr, page_count, kKxMemGraphics, prot, pfns.data());
            if (mapped) {
                auto* base = static_cast<uint8_t*>(mapped) + page_offset;
                if (is_write) {
                    std::memcpy(base, host_buf, byte_size);
                } else {
                    std::memcpy(host_buf, base, byte_size);
                }
                kx.unmap_title_vm(mapped, page_count);
                return OS_DSTORAGE_S_OK;
            }
        } else {
            // Multi-chunk scatter transfer for >16 MiB physical page arrays: pre-validate every
            // window before writing so invalid PFNs are strictly rejected with zero partial writes.
            if (is_write) {
                for (uint64_t base_pg = 0ull; base_pg < page_count; base_pg += kMaxWindowPages) {
                    const uint64_t win_pages = std::min<uint64_t>(kMaxWindowPages, page_count - base_pg);
                    void* probe = kx.map_title_vm(
                        nullptr, win_pages, kKxMemGraphics, PAGE_READONLY, pfns.data() + base_pg);
                    if (!probe) {
                        return OS_DSTORAGE_E_INVALIDARG;
                    }
                    kx.unmap_title_vm(probe, win_pages);
                }
            }
            size_t copied = 0u;
            size_t cur_off = static_cast<size_t>(page_offset);
            for (uint64_t base_pg = 0ull; base_pg < page_count && copied < byte_size;
                 base_pg += kMaxWindowPages) {
                const uint64_t win_pages = std::min<uint64_t>(kMaxWindowPages, page_count - base_pg);
                void* mapped = kx.map_title_vm(
                    nullptr, win_pages, kKxMemGraphics, prot, pfns.data() + base_pg);
                if (!mapped) {
                    return OS_DSTORAGE_E_INVALIDARG;
                }
                const size_t win_bytes = static_cast<size_t>(win_pages * kPage64KB) - cur_off;
                const size_t step = std::min<size_t>(byte_size - copied, win_bytes);
                auto* base = static_cast<uint8_t*>(mapped) + cur_off;
                if (is_write) {
                    std::memcpy(base, host_buf + copied, step);
                } else {
                    std::memcpy(host_buf + copied, base, step);
                }
                kx.unmap_title_vm(mapped, win_pages);
                copied += step;
                cur_off = 0u;
            }
            return OS_DSTORAGE_S_OK;
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
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
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

    bool AttachFile(HANDLE hfile, bool* out_skip_iocp_on_sync = nullptr) {
        if (out_skip_iocp_on_sync) {
            *out_skip_iocp_on_sync = false;
        }
        std::vector<std::thread> to_join;
        HANDLE to_close = nullptr;
        {
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
                if (active_files_ == 0u && iocp_) {
                    for (size_t i = 0u; i < threads_.size(); ++i) {
                        PostQueuedCompletionStatus(iocp_, 0u, kShutdownKey, nullptr);
                    }
                    to_join.swap(threads_);
                    to_close = iocp_;
                    iocp_ = nullptr;
                }
            } else {
                const BOOL skip_ok = SetFileCompletionNotificationModes(
                    hfile,
                    FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
                if (out_skip_iocp_on_sync) {
                    *out_skip_iocp_on_sync = (skip_ok != FALSE);
                }
                ++active_files_;
                return true;
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
        return false;
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
            AcquireSRWLockExclusive(&op->lock);
            op->bytes_transferred = bytes;
            op->error_code = err;
            op->completed = true;
            // Notify while holding op->lock so ReadAt() cannot observe completed==true and
            // destroy the stack-allocated IocpReadOp before WakeConditionVariable() returns!
            WakeConditionVariable(&op->cv);
            ReleaseSRWLockExclusive(&op->lock);
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
        bool iocp_attached = false,
        const wchar_t* path = nullptr,
        bool skip_iocp_on_sync = false
#else
        void* hfile,
        bool iocp_attached = false,
        const wchar_t* path = nullptr,
        bool skip_iocp_on_sync = false
#endif
        )
        : file_handle_(hfile),
          iocp_attached_(iocp_attached),
          skip_iocp_on_sync_(skip_iocp_on_sync),
          file_path_(path ? path : L"") {}

    ~DStorageFileImpl() {
        CloseInternal();
    }

    const std::wstring& FilePath() const noexcept { return file_path_; }

    struct Rds5ChunkEntry {
        uint64_t comp_off = 0ull;
        uint32_t comp_sz = 0u;
        uint32_t uncomp_sz = 0u;
        uint8_t flags = 0u;
    };

    bool TryCopyCachedRds5Chunk(
        uint64_t comp_off,
        uint32_t comp_sz,
        size_t slice_off,
        size_t dst_sz,
        std::vector<uint8_t>& out_slice) {
        std::lock_guard<std::mutex> lk(rds5_slice_mutex_);
        if (rds5_cached_comp_off_ == comp_off &&
            rds5_cached_comp_sz_ == comp_sz &&
            slice_off + dst_sz <= rds5_cached_uncomp_.size()) {
            out_slice.assign(
                rds5_cached_uncomp_.data() + slice_off,
                rds5_cached_uncomp_.data() + slice_off + dst_sz);
            return true;
        }
        return false;
    }

    void StoreCachedRds5Chunk(
        uint64_t comp_off,
        uint32_t comp_sz,
        const uint8_t* data,
        size_t data_sz) {
        if (!data || data_sz == 0u || data_sz > (4u * 1024u * 1024u)) {
            return;
        }
        std::lock_guard<std::mutex> lk(rds5_slice_mutex_);
        rds5_cached_comp_off_ = comp_off;
        rds5_cached_comp_sz_ = comp_sz;
        rds5_cached_uncomp_.assign(data, data + data_sz);
    }

    bool DecodeSingleRds5Chunk(
        const Rds5ChunkEntry& c,
        const std::atomic<bool>* cancel_flag,
        std::vector<uint8_t>& out_uncomp) {
        {
            std::lock_guard<std::mutex> lk(rds5_slice_mutex_);
            if (rds5_cached_comp_off_ == c.comp_off &&
                rds5_cached_comp_sz_ == c.comp_sz &&
                rds5_cached_uncomp_.size() == c.uncomp_sz) {
                out_uncomp = rds5_cached_uncomp_;
                return true;
            }
        }
        if (c.flags == 0u) {
            if (ReadAt(c.comp_off, c.uncomp_sz, out_uncomp, cancel_flag) < 0 ||
                out_uncomp.size() != c.uncomp_sz) {
                return false;
            }
            StoreCachedRds5Chunk(c.comp_off, c.comp_sz, out_uncomp.data(), out_uncomp.size());
            return true;
        }
        std::vector<uint8_t> raw;
        if (ReadAt(c.comp_off, c.comp_sz, raw, cancel_flag) < 0 || raw.size() != c.comp_sz) {
            return false;
        }
        if (c.flags == 8u) {
            if (!zlib_rfc1950::DecompressXboxStream(
                    raw.data(), raw.size(), c.uncomp_sz, out_uncomp, false) ||
                out_uncomp.size() != c.uncomp_sz) {
                return false;
            }
            StoreCachedRds5Chunk(c.comp_off, c.comp_sz, out_uncomp.data(), out_uncomp.size());
            return true;
        }
        if (c.flags >= 1u && c.flags <= 7u) {
            const auto fmt = static_cast<openscarlett::BcFormat>(c.flags);
            out_uncomp.resize(c.uncomp_sz);
            size_t written = 0u;
            if (!openscarlett::BcPackHwDecodeBitstream(
                    fmt, raw.data(), raw.size(), out_uncomp.data(), out_uncomp.size(), &written)
                     .ok() ||
                written != c.uncomp_sz) {
                return false;
            }
            StoreCachedRds5Chunk(c.comp_off, c.comp_sz, out_uncomp.data(), out_uncomp.size());
            return true;
        }
        return false;
    }

    bool AssembleCrossChunkRds5Span(
        size_t start_idx,
        size_t first_slice_off,
        size_t dst_sz,
        const std::atomic<bool>* cancel_flag,
        std::vector<uint8_t>& out_span) {
        out_span.clear();
        out_span.reserve(dst_sz);
        size_t idx = start_idx;
        size_t in_off = first_slice_off;
        std::vector<uint8_t> chunk_buf;
        while (out_span.size() < dst_sz && idx < rds5_chunks_.size()) {
            const Rds5ChunkEntry& c = rds5_chunks_[idx];
            if (in_off >= c.uncomp_sz) {
                in_off -= c.uncomp_sz;
                ++idx;
                continue;
            }
            if (!DecodeSingleRds5Chunk(c, cancel_flag, chunk_buf)) {
                return false;
            }
            const size_t avail = c.uncomp_sz - in_off;
            const size_t take = std::min<size_t>(dst_sz - out_span.size(), avail);
            out_span.insert(
                out_span.end(),
                chunk_buf.data() + in_off,
                chunk_buf.data() + in_off + take);
            in_off = 0u;
            ++idx;
        }
        if (out_span.size() < dst_sz) {
            out_span.resize(dst_sz, 0u);
        }
        return true;
    }

    bool ResolveRds5Request(
        uint64_t req_off,
        uint32_t req_src_sz,
        size_t dst_sz,
        bool use_zlib,
        uint64_t bcpack_mode,
        uint64_t& out_true_off,
        size_t& out_full_uncomp_sz,
        size_t& out_slice_off,
        size_t* out_cross_chunk_idx = nullptr) {
        if (out_cross_chunk_idx) {
            *out_cross_chunk_idx = SIZE_MAX;
        }
        EnsureRds5IndexLoaded();
        if (!rds5_valid_ || rds5_chunks_.empty() || req_src_sz == 0u || dst_sz == 0u) {
            return false;
        }
        const uint8_t want_flags =
            use_zlib ? 8u : static_cast<uint8_t>(bcpack_mode & 0x7u);

        auto cmp_off = [](const Rds5ChunkEntry& e, uint64_t off) {
            return e.comp_off < off;
        };

        // 1. Exact comp_off match (normal unshifted request or prefix sub-chunk).
        auto it = std::lower_bound(
            rds5_chunks_.begin(), rds5_chunks_.end(), req_off, cmp_off);
        if (it != rds5_chunks_.end() &&
            it->comp_off == req_off &&
            it->comp_sz == req_src_sz &&
            it->flags == want_flags &&
            it->uncomp_sz >= dst_sz) {
            out_true_off = req_off;
            out_full_uncomp_sz = it->uncomp_sz;
            out_slice_off = 0u;
            return true;
        }

        // 2. Cached constant-shift fast path (e.g. lighting.ttpf / ttdb_maps / ttdb_props +0xE0000).
        const uint64_t cached_shift = last_rds5_shift_.load(std::memory_order_relaxed);
        if (cached_shift > 0ull && req_off >= cached_shift) {
            const uint64_t cand_off = req_off - cached_shift;
            auto sit = std::lower_bound(
                rds5_chunks_.begin(), rds5_chunks_.end(), cand_off, cmp_off);
            if (sit != rds5_chunks_.end() &&
                sit->comp_off == cand_off &&
                sit->comp_sz == req_src_sz &&
                sit->flags == want_flags &&
                sit->uncomp_sz >= dst_sz) {
                out_true_off = cand_off;
                out_full_uncomp_sz = sit->uncomp_sz;
                out_slice_off = 0u;
                return true;
            }
        }

        // 3. Backward scan within 16 MiB for intra-chunk sub-slice, cross-chunk span, or constant-shifted chunk.
        constexpr uint64_t kMaxRds5ShiftWindow = 16ull * 1024ull * 1024ull;
        auto cur = std::upper_bound(
            rds5_chunks_.begin(),
            rds5_chunks_.end(),
            req_off,
            [](uint64_t off, const Rds5ChunkEntry& e) { return off < e.comp_off; });

        const Rds5ChunkEntry* fallback_match = nullptr;
        size_t fallback_idx = SIZE_MAX;
        uint64_t fallback_shift = 0ull;
        while (cur != rds5_chunks_.begin()) {
            --cur;
            if (cur->comp_off >= req_off) continue;
            const uint64_t shift = req_off - cur->comp_off;
            if (shift > kMaxRds5ShiftWindow) {
                break;
            }
            if (cur->comp_sz != req_src_sz || cur->flags != want_flags || cur->uncomp_sz < dst_sz) {
                continue;
            }
            if (cur->uncomp_sz > dst_sz && shift + dst_sz <= cur->uncomp_sz) {
                out_true_off = cur->comp_off;
                out_full_uncomp_sz = cur->uncomp_sz;
                out_slice_off = static_cast<size_t>(shift);
                return true;
            }
            if (cur->uncomp_sz == dst_sz) {
                last_rds5_shift_.store(shift, std::memory_order_relaxed);
                out_true_off = cur->comp_off;
                out_full_uncomp_sz = dst_sz;
                out_slice_off = 0u;
                return true;
            }
            if (!fallback_match) {
                fallback_match = &(*cur);
                fallback_idx = static_cast<size_t>(cur - rds5_chunks_.begin());
                fallback_shift = shift;
            }
        }
        if (fallback_match) {
            out_true_off = fallback_match->comp_off;
            out_full_uncomp_sz = fallback_match->uncomp_sz;
            if (fallback_shift < fallback_match->uncomp_sz) {
                // Request starts inside fallback_match at offset fallback_shift and crosses into next chunk(s).
                out_slice_off = static_cast<size_t>(fallback_shift);
                if (out_cross_chunk_idx) {
                    *out_cross_chunk_idx = fallback_idx;
                }
            } else {
                last_rds5_shift_.store(fallback_shift, std::memory_order_relaxed);
                out_slice_off = 0u;
            }
            return true;
        }
        return false;
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
        if (closing_.load(std::memory_order_acquire)) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
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
        if (size == 0u || offset + static_cast<uint64_t>(size) < offset) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (closing_.load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        // Shared lock allows N parallel positioned OVERLAPPED reads on the SAME file handle!
        std::shared_lock<std::shared_mutex> lk(handle_rw_mutex_);
#if defined(_WIN32)
        if (closing_.load(std::memory_order_acquire) ||
            file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }
        LARGE_INTEGER file_sz{};
        if (!GetFileSizeEx(file_handle_, &file_sz) ||
            file_sz.QuadPart < 0 ||
            offset + static_cast<uint64_t>(size) > static_cast<uint64_t>(file_sz.QuadPart)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        out.resize(size);
        constexpr uint32_t kReadChunkBytes = 4u * 1024u * 1024u;  // 4 MiB async overlapped chunk
        constexpr uint64_t k64GiB = 64ull * 1024ull * 1024ull * 1024ull;

        auto& rt = GetRuntime();
        rt.RecordIocpReadStart();
        struct ReadGuard {
            GlobalDStorageRuntime* r;
            ~ReadGuard() { r->RecordIocpReadEnd(); }
        } guard{&rt};

        uint32_t total_read = 0u;
        while (total_read < size) {
            if (closing_.load(std::memory_order_acquire)) {
                out.clear();
                return OS_DSTORAGE_E_IO_ERROR;
            }
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
            if (ok && skip_iocp_on_sync_) {
                // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS guarantees no IOCP packet was queued
                // when ReadFile completed synchronously from the OS file cache.
                if (imm_bytes == 0u) {
                    out.clear();
                    return OS_DSTORAGE_E_IO_ERROR;
                }
                total_read += imm_bytes;
            } else if (iocp_attached_) {
                AcquireSRWLockExclusive(&op.lock);
                while (!op.completed) {
                    if (closing_.load(std::memory_order_acquire) ||
                        (cancel_flag && cancel_flag->load(std::memory_order_acquire))) {
                        CancelIoEx(file_handle_, &op.ov);
                    }
                    SleepConditionVariableSRW(&op.cv, &op.lock, 5u, 0u);
                }
                ReleaseSRWLockExclusive(&op.lock);
                if (op.error_code == ERROR_OPERATION_ABORTED) {
                    out.clear();
                    return closing_.load(std::memory_order_acquire)
                               ? OS_DSTORAGE_E_IO_ERROR
                               : OS_DSTORAGE_E_CANCELLED;
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
        closing_.store(true, std::memory_order_release);
#if defined(_WIN32)
        // Cancel any blocked overlapped reads before waiting for exclusive handle lock.
        {
            std::shared_lock<std::shared_mutex> slk(handle_rw_mutex_);
            if (file_handle_ != INVALID_HANDLE_VALUE && file_handle_ != nullptr) {
                CancelIoEx(file_handle_, nullptr);
            }
        }
#endif
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

    void EnsureRds5IndexLoaded() {
        if (rds5_checked_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lk(rds5_mutex_);
        if (rds5_checked_.load(std::memory_order_relaxed)) {
            return;
        }
        DSTORAGE_FILE_INFO finfo{};
        std::vector<uint8_t> hdr;
        if (GetFileInformation(&finfo) >= 0 && finfo.FileSize >= 32u &&
            ReadAt(0u, 16u, hdr) >= 0 && hdr.size() == 16u &&
            std::memcmp(hdr.data(), "RDS5", 4u) == 0) {
            uint32_t cnt = 0u;
            uint64_t q1 = 0ull;
            std::memcpy(&cnt, hdr.data() + 4u, sizeof(cnt));
            std::memcpy(&q1, hdr.data() + 8u, sizeof(q1));
            const uint64_t tbl_bytes = static_cast<uint64_t>(cnt) * 8ull;
            if (cnt >= 2u && cnt <= 262144u && 16ull + tbl_bytes <= finfo.FileSize) {
                std::vector<uint8_t> tbl;
                if (ReadAt(16u, static_cast<uint32_t>(tbl_bytes), tbl) >= 0 &&
                    tbl.size() == static_cast<size_t>(tbl_bytes)) {
                    uint64_t qlast = 0ull;
                    std::memcpy(&qlast, tbl.data() + static_cast<size_t>(cnt - 1u) * 8u, sizeof(qlast));
                    const uint64_t last_comp = (qlast & 0x00FFFFFFull) << 8u;
                    const uint64_t last_uncomp = ((qlast >> 30u) & 0x3FFFFFull) << 12u;
                    const uint64_t hdr_uncomp = q1 & 0xFFFFFFFFFFull;
                    if (last_comp == finfo.FileSize && last_uncomp == hdr_uncomp) {
                        rds5_chunks_.resize(cnt - 1u);
                        bool valid_tbl = true;
                        for (uint32_t i = 0u; i + 1u < cnt; ++i) {
                            uint64_t q = 0ull;
                            uint64_t qn = 0ull;
                            std::memcpy(&q, tbl.data() + static_cast<size_t>(i) * 8u, sizeof(q));
                            std::memcpy(&qn, tbl.data() + static_cast<size_t>(i + 1u) * 8u, sizeof(qn));
                            const uint64_t c0 = (q & 0x00FFFFFFull) << 8u;
                            const uint64_t c1 = (qn & 0x00FFFFFFull) << 8u;
                            const uint64_t pad = (q >> 52u) & 0xFFFull;
                            const uint64_t u0 = ((q >> 30u) & 0x3FFFFFull) << 12u;
                            const uint64_t u1 = ((qn >> 30u) & 0x3FFFFFull) << 12u;
                            if (c1 < c0 + pad || u1 < u0) {
                                valid_tbl = false;
                                break;
                            }
                            rds5_chunks_[i].comp_off = c0;
                            rds5_chunks_[i].comp_sz = static_cast<uint32_t>((c1 - c0) - pad);
                            rds5_chunks_[i].uncomp_sz = static_cast<uint32_t>(u1 - u0);
                            rds5_chunks_[i].flags = static_cast<uint8_t>((q >> 26u) & 0xFu);
                        }
                        if (valid_tbl) {
                            rds5_valid_ = true;
                        } else {
                            rds5_chunks_.clear();
                        }
                    }
                }
            }
        }
        rds5_checked_.store(true, std::memory_order_release);
    }

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<bool> closing_{false};
    mutable std::shared_mutex handle_rw_mutex_;
#if defined(_WIN32)
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
#else
    void* file_handle_ = nullptr;
#endif
    bool iocp_attached_ = false;
    bool skip_iocp_on_sync_ = false;
    std::wstring file_path_;
    std::mutex rds5_mutex_;
    std::atomic<bool> rds5_checked_{false};
    bool rds5_valid_ = false;
    std::vector<Rds5ChunkEntry> rds5_chunks_;
    mutable std::atomic<uint64_t> last_rds5_shift_{0ull};
    std::mutex rds5_slice_mutex_;
    uint64_t rds5_cached_comp_off_ = UINT64_MAX;
    uint32_t rds5_cached_comp_sz_ = 0u;
    std::vector<uint8_t> rds5_cached_uncomp_;
};

// ===========================================================================
// IDStorageStatusArrayX Implementation
// ===========================================================================
class DStorageStatusArrayImpl final : public IDStorageStatusArrayX {
public:
    explicit DStorageStatusArrayImpl(uint32_t capacity)
        : capacity_(capacity),
          complete_(capacity > 0u ? std::make_unique<std::atomic<uint8_t>[]>(capacity) : nullptr),
          results_(capacity > 0u ? std::make_unique<std::atomic<int32_t>[]>(capacity) : nullptr) {
        for (uint32_t i = 0u; i < capacity_; ++i) {
            complete_[i].store(1u, std::memory_order_relaxed);
            results_[i].store(OS_DSTORAGE_S_OK, std::memory_order_relaxed);
        }
    }

    uint32_t Capacity() const noexcept {
        return capacity_;
    }

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
        if (index >= capacity_) {
            RecordOk("IDStorageStatusArrayX::IsComplete", 3);
            return false;
        }
        const bool done = (complete_[index].load(std::memory_order_acquire) != 0u);
        thread_local uint32_t tl_spin_polls = 0u;
        if (done) {
            tl_spin_polls = 0u;
            RecordOk("IDStorageStatusArrayX::IsComplete", 3);
            return true;
        }
        const uint32_t spins = ++tl_spin_polls;
        if ((spins & 4095u) == 1u) {
            RecordOk("IDStorageStatusArrayX::IsComplete", 3);
        }
#if defined(_WIN32)
        if ((spins & 255u) == 0u) {
            ::SwitchToThread();
        } else {
            YieldProcessor();
        }
#endif
        return false;
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetHResult(uint32_t index) override {
        if (index >= capacity_) {
            return RecordHr("IDStorageStatusArrayX::GetHResult", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        const bool done = (complete_[index].load(std::memory_order_acquire) != 0u);
        const int32_t hr = done ? results_[index].load(std::memory_order_acquire) : OS_DSTORAGE_E_PENDING;
        return RecordHr("IDStorageStatusArrayX::GetHResult", hr, 4);
    }

    void MarkPending(uint32_t index) {
        if (index < capacity_) {
            results_[index].store(OS_DSTORAGE_E_PENDING, std::memory_order_relaxed);
            complete_[index].store(0u, std::memory_order_release);
        }
    }

    void MarkComplete(uint32_t index, int32_t hr) {
        if (index < capacity_) {
            results_[index].store(hr, std::memory_order_release);
            complete_[index].store(1u, std::memory_order_release);
        }
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    uint32_t capacity_ = 0u;
    std::unique_ptr<std::atomic<uint8_t>[]> complete_;
    std::unique_ptr<std::atomic<int32_t>[]> results_;
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
                nullptr, kOsDStorageIidId3d12Resource, reinterpret_cast<void**>(&slot.buffer))) ||
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
        if (FAILED(dev->CreateCommandQueue(&qd, kOsDStorageIidId3d12CommandQueue,
                                           reinterpret_cast<void**>(&cmd_queue))) ||
            !cmd_queue) {
            Reset();
            return false;
        }
        if (FAILED(dev->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                kOsDStorageIidId3d12CommandAllocator,
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
                kOsDStorageIidId3d12GraphicsCommandList,
                reinterpret_cast<void**>(&cmd_list))) ||
            !cmd_list) {
            Reset();
            return false;
        }
        cmd_list->Close();

        if (FAILED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, kOsDStorageIidId3d12Fence,
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
class DStorageQueueCore;

// ===========================================================================
// Multi-Threaded Cross-Queue Priority Scheduler (REALTIME > HIGH > NORMAL > LOW)
// Dispatches concurrent IOCP file reads & parallel decompression across worker
// threads while strictly preserving per-queue Status/Event/Signal barriers and
// priority preemption. Holds shared_ptr<DStorageQueueCore> and uses monotonic
// generation-scoped worker teardown to prevent all Close/Release/restart races.
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

    void RegisterQueue(const std::shared_ptr<DStorageQueueCore>& q, DSTORAGE_PRIORITY priority);
    void UnregisterQueue(const DStorageQueueCore* q);
    void NotifyWorkAvailable();

private:
    struct Slot {
        std::shared_ptr<DStorageQueueCore> queue;
        size_t bucket = 2u;
        uint32_t active_workers = 0u;
        bool removing = false;
    };

    void RunLoop(uint64_t my_generation);

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::vector<Slot> slots_;
    std::array<size_t, 4> rr_cursor_{0u, 0u, 0u, 0u};
    uint64_t generation_ = 0ull;
    bool worker_running_ = false;
    std::vector<std::thread> workers_;
};

// ===========================================================================
// Ref-Counted Queue Core (shared between IDStorageQueueX2 wrapper & workers;
// eliminates all Close() / Release() / worker teardown races, xcl #81)
// ===========================================================================
class DStorageQueueCore final : public std::enable_shared_from_this<DStorageQueueCore> {
public:
    struct InFlightRequestState {
        uint64_t tag = 0ull;
        std::atomic<bool> canceled{false};
    };

    static std::shared_ptr<DStorageQueueCore> Create(
        const DSTORAGE_QUEUE_DESC& desc,
        std::shared_ptr<std::atomic<uint32_t>> staging_size_ref = nullptr,
        uint32_t queue_options = DSTORAGE_QUEUE_OPTIONS_NONE) {
        auto core = std::shared_ptr<DStorageQueueCore>(
            new (std::nothrow) DStorageQueueCore(desc, std::move(staging_size_ref), queue_options));
        if (core) {
            DStoragePriorityScheduler::Global().RegisterQueue(core, core->desc_.Priority);
        }
        return core;
    }

    ~DStorageQueueCore() {
        CloseInternal();
#if defined(_WIN32)
        if (error_event_) {
            CloseHandle(error_event_);
            error_event_ = nullptr;
        }
#endif
    }

    // Title entry point: `request` is the Xbox dstorage_xs.h DSTORAGE_REQUEST (0x48 bytes).
    void EnqueueRequest(const DSTORAGE_REQUEST* request) {
        RecordOk("IDStorageQueueX::EnqueueRequest", 3);
        if (!request) {
            RecordCommandFailure(OS_DSTORAGE_E_POINTER, DSTORAGE_COMMAND_TYPE_REQUEST);
            return;
        }
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
    // DestinationIsPhysicalPages, SourceIsPhysicalPages) and strictly validating reserved bits,
    // pointers, sizes, and 64-bit file offset overflow.
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
            void* src_buf = x.Source;
#if defined(_WIN32)
            if (!src_phys && src_buf) {
                src_buf = ResolveCpuAliasPointer(src_buf);
            }
#endif
            out.Source.Memory.Source = src_buf;
            out.Source.Memory.Size = x.SourceSize;
        } else {
            out.Source.File.Source = x.File;
            out.Source.File.Offset = x.FileOffset;
            out.Source.File.Size = x.SourceSize;
        }
        void* dst_buf = x.Destination;
#if defined(_WIN32)
        if (!dest_phys && dst_buf) {
            dst_buf = ResolveCpuAliasPointer(dst_buf);
        }
#endif
        out.Destination.Memory.Buffer = dst_buf;
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
        // Strict pointer, zero-size, and offset-overflow validation.
        if (!x.Destination) {
            return OS_DSTORAGE_E_POINTER;
        }
        if (x.DestinationSize == 0u || x.SourceSize == 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (from_memory) {
            if (!x.Source) {
                return OS_DSTORAGE_E_POINTER;
            }
        } else {
            if (!x.File) {
                return OS_DSTORAGE_E_POINTER;
            }
            if (x.FileOffset + static_cast<uint64_t>(x.SourceSize) < x.FileOffset) {
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
    explicit DStorageQueueCore(
        const DSTORAGE_QUEUE_DESC& desc,
        std::shared_ptr<std::atomic<uint32_t>> staging_size_ref,
        uint32_t queue_options)
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
        ResetErrorRecordLocked();
#if defined(_WIN32)
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
    }

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
    void EnqueueStatus(IDStorageStatusArrayX* statusArray, uint32_t index) {
        RecordOk("IDStorageQueueX::EnqueueStatus", 4);
        if (!statusArray) {
            RecordCommandFailure(OS_DSTORAGE_E_POINTER, DSTORAGE_COMMAND_TYPE_STATUS);
            return;
        }
        auto* impl = static_cast<DStorageStatusArrayImpl*>(statusArray);
        if (index >= impl->Capacity()) {
            RecordCommandFailure(OS_DSTORAGE_E_INVALIDARG, DSTORAGE_COMMAND_TYPE_STATUS, statusArray, index);
            return;
        }
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

    void EnqueueSetEvent(void* handle) {
        RecordOk("IDStorageQueueX::EnqueueSetEvent", 5);
        if (!handle) {
            RecordCommandFailure(OS_DSTORAGE_E_POINTER, DSTORAGE_COMMAND_TYPE_EVENT);
            return;
        }
#if defined(_WIN32)
        if (handle == INVALID_HANDLE_VALUE) {
            RecordCommandFailure(OS_DSTORAGE_E_INVALIDARG, DSTORAGE_COMMAND_TYPE_EVENT);
            return;
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(),
                static_cast<HANDLE>(handle),
                GetCurrentProcess(),
                &dup,
                0u,
                FALSE,
                DUPLICATE_SAME_ACCESS) ||
            !dup) {
            RecordCommandFailure(OS_DSTORAGE_E_INVALIDARG, DSTORAGE_COMMAND_TYPE_EVENT);
            return;
        }
        QueueItem item{};
        item.kind = QueueItem::Kind::Event;
        item.event_handle = dup;
        bool close_dup = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
            } else {
                close_dup = true;
            }
        }
        if (close_dup) {
            CloseHandle(dup);
        }
#else
        QueueItem item{};
        item.kind = QueueItem::Kind::Event;
        item.event_handle = handle;
        std::unique_lock<std::mutex> lk(mutex_);
        WaitForSlotLocked(lk);
        if (!closed_) {
            staged_.push_back(item);
        }
#endif
    }

    void Submit() {
        RecordOk("IDStorageQueueX::Submit", 6);
        static const bool trace_ds = []() {
            char v[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DSTORAGE", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        static std::atomic<uint32_t> s_ds_submits{0u};
        bool flushed = false;
        size_t staged_n = 0u;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closed_ || staged_.empty()) return;
            staged_n = staged_.size();
            flushed = FlushStagedLocked();
        }
        if (trace_ds && flushed) {
            const uint32_t n = s_ds_submits.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 5u || (n % 200u) == 0u) {
                std::fprintf(stderr, "[OpenScarlett][DSTORAGE] Submit #%u (staged=%zu)\n", n, staged_n);
                std::fflush(stderr);
            }
        }
        if (flushed) {
            DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        }
    }

    void CancelRequestsWithTag(uint64_t mask, uint64_t value) {
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

    void Close() {
        RecordOk("IDStorageQueueX::Close", 8);
        CloseInternal();
    }

    void* GetErrorEvent() {
        RecordOk("IDStorageQueueX::GetErrorEvent", 9);
#if defined(_WIN32)
        std::lock_guard<std::mutex> lk(error_mutex_);
        if (!error_event_) {
            return nullptr;
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(),
                error_event_,
                GetCurrentProcess(),
                &dup,
                0u,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            return nullptr;
        }
        return dup;
#else
        return nullptr;
#endif
    }

    void RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) {
        RecordOk("IDStorageQueueX::RetrieveErrorRecord", 10);
        if (!record) return;
        std::lock_guard<std::mutex> lk(error_mutex_);
        *record = error_record_;
        ResetErrorRecordLocked();
#if defined(_WIN32)
        if (error_event_) {
            ResetEvent(error_event_);
        }
#endif
    }

    void Query(DSTORAGE_QUEUE_INFO* info) {
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
    void EnqueueSignal(void* fence, uint64_t value) {
        RecordOk("IDStorageQueueX1::EnqueueSignal", 12);
        if (!fence) {
            RecordCommandFailure(
                OS_DSTORAGE_E_POINTER, DSTORAGE_COMMAND_TYPE_SIGNAL, nullptr, 0u, nullptr, value);
            return;
        }
#if defined(_WIN32)
        if (!ValidateReadableRange(fence, sizeof(void*))) {
            RecordCommandFailure(
                OS_DSTORAGE_E_INVALIDARG, DSTORAGE_COMMAND_TYPE_SIGNAL, nullptr, 0u, fence, value);
            return;
        }
        static_cast<ID3D12Fence*>(fence)->AddRef();
#endif
        QueueItem item{};
        item.kind = QueueItem::Kind::Signal;
        item.signal_fence = fence;
        item.signal_value = value;
        bool release_fence = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
            } else {
                release_fence = true;
            }
        }
#if defined(_WIN32)
        if (release_fence) {
            static_cast<ID3D12Fence*>(fence)->Release();
        }
#else
        (void)release_fence;
#endif
    }

    // IDStorageQueueX2 (slots 13..15)
    void Query1(DSTORAGE_QUEUE_INFO1* info) {
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

    int32_t SetAutoSubmitDisabled(bool disabled) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto_submit_disabled_ = disabled;
        if (disabled) {
            queue_options_ |= DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT;
        } else {
            queue_options_ &= ~static_cast<uint32_t>(DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT);
        }
        return RecordHr("IDStorageQueueX2::SetAutoSubmitDisabled", OS_DSTORAGE_S_OK, 14);
    }

    bool GetAutoSubmitDisabled() {
        RecordOk("IDStorageQueueX2::GetAutoSubmitDisabled", 15);
        std::lock_guard<std::mutex> lk(mutex_);
        return auto_submit_disabled_;
    }

    // Called by DStoragePriorityScheduler under scheduler mutex to test whether an item
    // can be dispatched immediately (requests run concurrently; barriers serialize strictly
    // against both in-flight requests AND any preceding in-flight barrier so e.g. EnqueueStatus
    // always completes before a subsequent EnqueueSetEvent fires).
    bool CanDispatchNow() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (active_.empty() || in_flight_barriers_ > 0u) return false;
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
            if (active_.empty() || in_flight_barriers_ > 0u) {
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
                    space_cv_.notify_all();
                }
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
                    HANDLE h = static_cast<HANDLE>(item.event_handle);
                    SetEvent(h);
                    CloseHandle(h);
                }
#endif
                break;
            }

            case QueueItem::Kind::Signal: {
#if defined(_WIN32)
                if (item.signal_fence) {
                    auto* f = static_cast<ID3D12Fence*>(item.signal_fence);
                    f->Signal(item.signal_value);
                    f->Release();
                }
#endif
                break;
            }
        }

        bool wake_after_barrier = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (in_flight_barriers_ > 0u) --in_flight_barriers_;
            if (in_flight_barriers_ == 0u && !active_.empty()) {
                wake_after_barrier = true;
            }
            if (closed_ && active_.empty() && in_flight_requests_ == 0u && in_flight_barriers_ == 0u) {
                drain_cv_.notify_all();
            }
            space_cv_.notify_all();
        }
        if (wake_after_barrier) {
            DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        }
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
            if (!req.Source.File.Source) {
                return OS_DSTORAGE_E_POINTER;
            }
            if (req.Source.File.Size == 0u ||
                req.Source.File.Offset + static_cast<uint64_t>(req.Source.File.Size) < req.Source.File.Offset) {
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
            if (!req.Source.Memory.Source) {
                return OS_DSTORAGE_E_POINTER;
            }
            if (req.Source.Memory.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            const void* resolved_src = req.Source.Memory.Source;
#if defined(_WIN32)
            resolved_src = ResolveCpuAliasConstPointer(resolved_src);
            if (!ValidateReadableRange(resolved_src, req.Source.Memory.Size)) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
#endif
            if (check_staging_for_src &&
                req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.Memory.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            out_ptr = static_cast<const uint8_t*>(resolved_src);
            out_size = req.Source.Memory.Size;
            return OS_DSTORAGE_S_OK;
        }
        return OS_DSTORAGE_E_INVALIDARG;
    }

    static bool TryResolveShiftedZlibSubChunk(
        DStorageFileImpl* file_impl,
        uint64_t shifted_off,
        uint32_t src_sz,
        size_t dst_sz,
        size_t staging_limit,
        const std::atomic<bool>* cancel_flag,
        std::vector<uint8_t>& out_slice) {
        if (!file_impl || src_sz < 6u || dst_sz == 0u) return false;

        struct CachedChunk {
            const DStorageFileImpl* file = nullptr;
            uint64_t base_off = 0ull;
            uint32_t src_sz = 0u;
            uint64_t tick = 0ull;
            std::vector<uint8_t> decompressed;
        };
        static std::mutex s_cache_mu;
        static std::array<CachedChunk, 4> s_cache{};
        static uint64_t s_cache_tick = 0ull;

        {
            std::lock_guard<std::mutex> lk(s_cache_mu);
            for (auto& e : s_cache) {
                if (e.file == file_impl && e.src_sz == src_sz && shifted_off >= e.base_off) {
                    const uint64_t sub_off = shifted_off - e.base_off;
                    if (sub_off <= e.decompressed.size() &&
                        dst_sz <= e.decompressed.size() - static_cast<size_t>(sub_off)) {
                        e.tick = ++s_cache_tick;
                        out_slice.assign(
                            e.decompressed.begin() + static_cast<std::ptrdiff_t>(sub_off),
                            e.decompressed.begin() + static_cast<std::ptrdiff_t>(sub_off + dst_sz));
                        return true;
                    }
                }
            }
        }

        DSTORAGE_FILE_INFO finfo{};
        if (file_impl->GetFileInformation(&finfo) < 0 || finfo.FileSize < src_sz) {
            return false;
        }
        constexpr uint64_t kMaxSubChunkShift = 8ull * 1024ull * 1024ull;
        const uint64_t min_sub =
            (shifted_off + src_sz > finfo.FileSize)
                ? (((shifted_off + src_sz - finfo.FileSize) + 0xFFull) & ~0xFFull)
                : 0x100ull;
        const uint64_t max_sub = std::min<uint64_t>(shifted_off & ~0xFFull, kMaxSubChunkShift);
        if (min_sub == 0ull || min_sub > max_sub) {
            return false;
        }

        const uint64_t win_start = shifted_off - max_sub;
        const uint64_t win_end = std::min<uint64_t>(finfo.FileSize, (shifted_off - min_sub) + src_sz);
        if (win_end <= win_start || (win_end - win_start) > (kMaxSubChunkShift + src_sz)) {
            return false;
        }
        std::vector<uint8_t> win_buf;
        if (file_impl->ReadAt(win_start, static_cast<uint32_t>(win_end - win_start), win_buf, cancel_flag) < 0 ||
            win_buf.size() < src_sz) {
            return false;
        }

        const size_t max_chunk_cap =
            std::min<size_t>(staging_limit, std::max<size_t>(static_cast<size_t>(max_sub) + dst_sz, 16ull * 1024ull * 1024ull));
        std::vector<uint8_t> full_dec;
        for (uint64_t sub_off = min_sub; sub_off <= max_sub; sub_off += 0x100ull) {
            if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) return false;
            const uint64_t cand_off = shifted_off - sub_off;
            if (cand_off < win_start || (cand_off - win_start) + src_sz > win_buf.size()) {
                continue;
            }
            const uint8_t* p = win_buf.data() + static_cast<size_t>(cand_off - win_start);
            const uint8_t cmf = p[0];
            const uint8_t flg = p[1];
            if ((cmf & 0x0Fu) != 8u || ((cmf >> 4u) & 0x0Fu) > 7u || ((flg >> 5u) & 1u) != 0u ||
                (((static_cast<uint32_t>(cmf) << 8u) | static_cast<uint32_t>(flg)) % 31u) != 0u) {
                continue;
            }
            if (zlib_rfc1950::DecompressDeflateAttempt(
                    p, src_sz, /*expect_rfc1950_wrapper=*/true, max_chunk_cap,
                    /*allow_prefix_stop=*/false, full_dec) &&
                full_dec.size() >= static_cast<size_t>(sub_off) + dst_sz) {
                out_slice.assign(
                    full_dec.begin() + static_cast<std::ptrdiff_t>(sub_off),
                    full_dec.begin() + static_cast<std::ptrdiff_t>(sub_off + dst_sz));
                std::lock_guard<std::mutex> lk(s_cache_mu);
                size_t victim = 0u;
                for (size_t i = 1u; i < s_cache.size(); ++i) {
                    if (s_cache[i].tick < s_cache[victim].tick) victim = i;
                }
                s_cache[victim].file = file_impl;
                s_cache[victim].base_off = cand_off;
                s_cache[victim].src_sz = src_sz;
                s_cache[victim].tick = ++s_cache_tick;
                s_cache[victim].decompressed = std::move(full_dec);
                return true;
            }
        }
        return false;
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
        void* resolved_dst = req.Destination.Memory.Buffer;
        DestinationClassification dst_class{};
        if (!dest_phys) {
            const int32_t dhr = ValidateAndClassifyDestination(
                resolved_dst, dst_size, dst_class);
            if (dhr < 0) return dhr;
        }

        ScopedRequestScratch scratch;
        std::vector<uint8_t>& buf_a = scratch.a();
        std::vector<uint8_t>& buf_b = scratch.b();

        // 2. Acquire source bytes (IOCP file read, virtual memory, or physical page array).
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0u;
        bool has_rds5_remap = false;
        bool rds5_served_from_cache = false;
        uint64_t rds5_resolved_off = 0ull;
        uint32_t rds5_resolved_src_sz = 0u;
        size_t rds5_full_uncomp_sz = 0u;
        size_t rds5_slice_off = 0u;
        if (src_phys) {
            if (req.Options.SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY ||
                !req.Source.Memory.Source || req.Source.Memory.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            buf_a.resize(req.Source.Memory.Size);
            const int32_t phr = TransferPhysicalPageArray(
                title_va_mirror_.load(std::memory_order_acquire),
                static_cast<const uintptr_t*>(req.Source.Memory.Source),
                item.xbox_src_page_offset,
                req.Source.Memory.Size,
                false,
                buf_a.data());
            if (phr < 0) return phr;
            src_ptr = buf_a.data();
            src_size = buf_a.size();
        } else {
            if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                req.Source.File.Source != nullptr && req.Source.File.Size > 0u) {
                auto* file_impl = static_cast<DStorageFileImpl*>(req.Source.File.Source);
                uint64_t rds5_true_off = 0ull;
                size_t rds5_cross_idx = SIZE_MAX;
                if (file_impl->ResolveRds5Request(
                        req.Source.File.Offset,
                        req.Source.File.Size,
                        dst_size,
                        use_zlib,
                        bcpack_mode,
                        rds5_true_off,
                        rds5_full_uncomp_sz,
                        rds5_slice_off,
                        &rds5_cross_idx)) {
                    if (rds5_true_off != req.Source.File.Offset) {
                        static std::atomic<uint32_t> s_rds5_log{0u};
                        const uint32_t rn = s_rds5_log.fetch_add(1u, std::memory_order_relaxed);
                        if (rn < 8u) {
                            std::fprintf(
                                stderr,
                                "[OpenScarlett][dstorage] RDS5-REMAP #%u reqOff=0x%llX -> trueOff=0x%llX srcSz=0x%X dstSz=0x%zX fullSz=0x%zX slice=0x%zX cross=%d z=%d bc=%llu\n",
                                rn,
                                static_cast<unsigned long long>(req.Source.File.Offset),
                                static_cast<unsigned long long>(rds5_true_off),
                                req.Source.File.Size,
                                dst_size,
                                rds5_full_uncomp_sz,
                                rds5_slice_off,
                                (rds5_cross_idx != SIZE_MAX) ? 1 : 0,
                                use_zlib ? 1 : 0,
                                static_cast<unsigned long long>(bcpack_mode));
                            std::fflush(stderr);
                        }
                    }
                    rds5_resolved_off = rds5_true_off;
                    rds5_resolved_src_sz = req.Source.File.Size;
                    if (rds5_cross_idx != SIZE_MAX &&
                        file_impl->AssembleCrossChunkRds5Span(
                            rds5_cross_idx, rds5_slice_off, dst_size, cancel_flag, buf_b)) {
                        src_ptr = buf_b.data();
                        src_size = buf_b.size();
                        has_rds5_remap = true;
                        rds5_served_from_cache = true;
                    } else if (rds5_full_uncomp_sz > dst_size &&
                               file_impl->TryCopyCachedRds5Chunk(
                                   rds5_true_off, req.Source.File.Size, rds5_slice_off, dst_size, buf_b)) {
                        src_ptr = buf_b.data();
                        src_size = buf_b.size();
                        has_rds5_remap = true;
                        rds5_served_from_cache = true;
                    } else {
                        const int32_t rds_hr = file_impl->ReadAt(
                            rds5_true_off, req.Source.File.Size, buf_a, cancel_flag);
                        if (rds_hr < 0) return rds_hr;
                        src_ptr = buf_a.data();
                        src_size = buf_a.size();
                        has_rds5_remap = true;
                    }
                }
            }
            if (!has_rds5_remap && dest_phys && req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                req.Source.File.Source != nullptr) {
                auto* file_impl = static_cast<DStorageFileImpl*>(req.Source.File.Source);
                DSTORAGE_FILE_INFO finfo{};
                if (file_impl->GetFileInformation(&finfo) >= 0 && finfo.FileSize > 0u &&
                    (req.Source.File.Offset >= finfo.FileSize ||
                     req.Source.File.Size > finfo.FileSize ||
                     req.Source.File.Offset + static_cast<uint64_t>(req.Source.File.Size) > finfo.FileSize)) {
                    if (use_zlib && bcpack_mode == 0u && swizzle_mode == 0u &&
                        TryResolveShiftedZlibSubChunk(
                            file_impl, req.Source.File.Offset, req.Source.File.Size,
                            dst_size, staging_limit, cancel_flag, buf_b)) {
                        const int32_t sh_hr = TransferPhysicalPageArray(
                            title_va_mirror_.load(std::memory_order_acquire),
                            static_cast<const uintptr_t*>(resolved_dst),
                            item.xbox_dest_page_offset,
                            static_cast<uint32_t>(dst_size),
                            true,
                            buf_b.data());
                        if (sh_hr >= 0) {
                            last_intermediate_size_.store(
                                static_cast<uint32_t>(std::min<size_t>(dst_size, UINT32_MAX)),
                                std::memory_order_release);
                            GetRuntime().total_zlib_decompressions.fetch_add(1u, std::memory_order_relaxed);
                            RecordOk("DirectStorage::XboxZlibDecompress");
                            return OS_DSTORAGE_S_OK;
                        }
                        return sh_hr;
                    }
                    static std::atomic<uint32_t> s_oob_cnt{0u};
                    const uint32_t on = s_oob_cnt.fetch_add(1u, std::memory_order_relaxed);
                    if (on < 8u) {
                        std::fprintf(stderr,
                                     "[OpenScarlett][dstorage] OOB-ZEROFILL #%u off=0x%llX srcSz=0x%X fileSz=0x%llX dstSz=0x%zx\n",
                                     on, static_cast<unsigned long long>(req.Source.File.Offset),
                                     req.Source.File.Size,
                                     static_cast<unsigned long long>(finfo.FileSize), dst_size);
                        std::fflush(stderr);
                    }
                    buf_a.assign(dst_size, 0u);
                    const int32_t zhr = TransferPhysicalPageArray(
                        title_va_mirror_.load(std::memory_order_acquire),
                        static_cast<const uintptr_t*>(resolved_dst),
                        item.xbox_dest_page_offset,
                        static_cast<uint32_t>(dst_size),
                        true,
                        buf_a.data());
                    return (zhr < 0) ? zhr : OS_DSTORAGE_S_OK;
                }
            }
            if (!has_rds5_remap) {
                const int32_t shr = AcquireSourceBytes(
                    req, staging_limit, buf_a, src_ptr, src_size, cancel_flag, false);
                if (shr < 0) return shr;
            }
        }
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        // 3. Stage 1 Decompression: RFC 1950/1951 Zlib if Options.ZlibDecompress is set.
        const uint8_t* cur_ptr = src_ptr;
        size_t cur_size = src_size;
        const size_t stage_dst_size =
            (has_rds5_remap && !rds5_served_from_cache && rds5_full_uncomp_sz > dst_size)
                ? rds5_full_uncomp_sz
                : dst_size;

        if (use_zlib && !rds5_served_from_cache) {
            const size_t zlib_max =
                (bcpack_mode != 0u && item.xbox_intermediate_size > 0u)
                    ? static_cast<size_t>(item.xbox_intermediate_size)
                    : std::max(stage_dst_size, static_cast<size_t>(item.xbox_intermediate_size));
            if (zlib_max == 0u) return OS_DSTORAGE_E_INVALIDARG;
            if (zlib_max > staging_limit) return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            ReserveZlibOutputHint(buf_b, cur_size, (bcpack_mode == 0u) ? stage_dst_size : zlib_max);
            const bool allow_prefix =
                (bcpack_mode == 0u && req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE);
            bool z_ok = zlib_rfc1950::DecompressXboxStream(
                cur_ptr, cur_size, zlib_max, buf_b, allow_prefix);
            if (!z_ok && allow_prefix) {
                // Retail RPF8/9 packfile readers occasionally request a sub-chunk prefix
                // (e.g. DestinationSize = 16 KiB from a 64 KiB Zlib-compressed RPF chunk).
                const size_t chunk_cap =
                    std::min<size_t>(staging_limit, std::max<size_t>(stage_dst_size * 16u, 262144u));
                if (chunk_cap > zlib_max) {
                    z_ok = zlib_rfc1950::DecompressXboxStream(
                        cur_ptr, cur_size, chunk_cap, buf_b, true);
                    if (z_ok && buf_b.size() > stage_dst_size) {
                        buf_b.resize(stage_dst_size);
                    }
                }
                // Retail RDS5 (.ttpf) packfile readers sub-slice multi-page Zlib chunks (e.g. 512 KiB
                // chunk read in 128 KiB pages) where EnqueueChunksForRead added sub_offset to File.Offset.
                if (!z_ok && !has_rds5_remap && req.Source.File.Source != nullptr) {
                    z_ok = TryResolveShiftedZlibSubChunk(
                        static_cast<DStorageFileImpl*>(req.Source.File.Source),
                        req.Source.File.Offset,
                        req.Source.File.Size,
                        dst_size,
                        staging_limit,
                        cancel_flag,
                        buf_b);
                }
            }
            if (!z_ok) {
                static std::atomic<uint32_t> s_zf{0u};
                const uint32_t zfn = s_zf.fetch_add(1u, std::memory_order_relaxed);
                if (zfn < 8u) {
                    char hex[64]{};
                    int hp = 0;
                    const size_t hn = std::min<size_t>(cur_size, 16u);
                    for (size_t hi = 0u; hi < hn && hp + 4 < static_cast<int>(sizeof(hex)); ++hi) {
                        hp += std::snprintf(hex + hp, sizeof(hex) - static_cast<size_t>(hp), "%02X ", cur_ptr[hi]);
                    }
                    std::fprintf(stderr,
                                 "[OpenScarlett][dstorage] ZLIB-FAIL #%u off=0x%llX srcSz=0x%zX dstSz=0x%zX bc=%u hdr=[%s]\n",
                                 zfn,
                                 req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE
                                     ? static_cast<unsigned long long>(req.Source.File.Offset)
                                     : 0ull,
                                 cur_size, dst_size, static_cast<unsigned>(bcpack_mode), hex);
                    std::fflush(stderr);
                }
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            if (bcpack_mode == 0u && buf_b.size() > stage_dst_size &&
                req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
                buf_b.resize(stage_dst_size);
            }
            if (bcpack_mode == 0u && buf_b.size() != stage_dst_size) {
                static const bool trace_ds_z = []() {
#if defined(_WIN32)
                    char buf[16]{};
                    return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DSTORAGE", buf, sizeof(buf)) > 0 && buf[0] != '0';
#else
                    return false;
#endif
                }();
                if (trace_ds_z) {
                    std::fprintf(stderr,
                                 "[OpenScarlett][dstorage] ZLIB-SIZE-MISMATCH stage=%zu dst=%zu src=%zu off=0x%llX\n",
                                 buf_b.size(), stage_dst_size, cur_size,
                                 req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE
                                     ? static_cast<unsigned long long>(req.Source.File.Offset)
                                     : 0ull);
                    std::fflush(stderr);
                }
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            cur_ptr = buf_b.data();
            cur_size = buf_b.size();
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(cur_size, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().total_zlib_decompressions.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::XboxZlibDecompress");
        }
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        // 4. Stage 2 Decompression: Hardware BCPack BC1-BC7 if Options.BcpackMode != NONE.
        if (bcpack_mode != 0u && !rds5_served_from_cache) {
            if (stage_dst_size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            const auto fmt = static_cast<openscarlett::BcFormat>(bcpack_mode);
            const uint32_t blk_bytes = openscarlett::BcFormatBlockBytes(fmt);
            if (blk_bytes == 0u || (stage_dst_size % blk_bytes) != 0u) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            std::vector<uint8_t>& bcpack_out =
                (!buf_a.empty() && cur_ptr == buf_a.data()) ? buf_b : buf_a;
            bcpack_out.resize(stage_dst_size);
            size_t written = 0u;
            auto st = openscarlett::BcPackHwDecodeBitstream(
                fmt, cur_ptr, cur_size, bcpack_out.data(), bcpack_out.size(), &written);
            if (!st.ok() && stage_dst_size < 65536u) {
                static constexpr size_t kCandidateChunkBytes[] = {
                    4096u, 8192u, 12288u, 16384u, 32768u, 65536u};
                std::vector<uint8_t> full_chunk;
                for (size_t cand_bytes : kCandidateChunkBytes) {
                    if (stage_dst_size >= cand_bytes) continue;
                    full_chunk.resize(cand_bytes);
                    size_t cand_written = 0u;
                    if (openscarlett::BcPackHwDecodeBitstream(
                            fmt, cur_ptr, cur_size, full_chunk.data(), full_chunk.size(), &cand_written).ok() &&
                        cand_written == cand_bytes) {
                        std::memcpy(bcpack_out.data(), full_chunk.data(), stage_dst_size);
                        written = stage_dst_size;
                        st = openscarlett::Status::Ok();
                        break;
                    }
                }
            }
            if (!st.ok()) {
                // Also accept a matching-format BCPK container if provided with exact size.
                openscarlett::BcPackHeader hdr{};
                if (openscarlett::BcPackParseHeader(cur_ptr, cur_size, hdr).ok() &&
                    hdr.format == fmt &&
                    hdr.original_size == stage_dst_size &&
                    openscarlett::BcPackDecompressInto(
                        cur_ptr, cur_size, bcpack_out.data(), bcpack_out.size(), &written).ok()) {
                    st = openscarlett::Status::Ok();
                }
            }
            if (!st.ok() || written != stage_dst_size) {
                static std::atomic<uint32_t> s_bcp_log_cnt{0u};
                const uint32_t bcn = s_bcp_log_cnt.fetch_add(1u, std::memory_order_relaxed);
                if (bcn < 8u) {
                    char hex[96]{};
                    size_t hp = 0u;
                    for (size_t k = 0u; k < std::min<size_t>(cur_size, 24u) && hp + 4u < sizeof(hex); ++k) {
                        hp += static_cast<size_t>(std::snprintf(hex + hp, sizeof(hex) - hp, "%02X ", cur_ptr[k]));
                    }
                    std::fprintf(stderr,
                                 "[OpenScarlett][dstorage] BCPACK-FAIL #%u mode=%llu src=%zu dst=%zu int=%u off=0x%llX hdr=[%s]\n",
                                 bcn,
                                 static_cast<unsigned long long>(bcpack_mode),
                                 cur_size, stage_dst_size, item.xbox_intermediate_size,
                                 req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE
                                     ? static_cast<unsigned long long>(req.Source.File.Offset)
                                     : 0ull,
                                 hex);
                    std::fflush(stderr);
                }
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

        if (has_rds5_remap && !rds5_served_from_cache &&
            rds5_full_uncomp_sz > dst_size && cur_size >= rds5_slice_off + dst_size) {
            if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                req.Source.File.Source != nullptr) {
                static_cast<DStorageFileImpl*>(req.Source.File.Source)
                    ->StoreCachedRds5Chunk(rds5_resolved_off, rds5_resolved_src_sz, cur_ptr, cur_size);
            }
            std::vector<uint8_t>& active_vec =
                (!buf_a.empty() && cur_ptr == buf_a.data()) ? buf_a : buf_b;
            if (rds5_slice_off > 0u) {
                std::memmove(active_vec.data(), active_vec.data() + rds5_slice_off, dst_size);
            }
            active_vec.resize(dst_size);
            cur_ptr = active_vec.data();
            cur_size = dst_size;
        } else if (rds5_served_from_cache) {
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(cur_size, UINT32_MAX)),
                std::memory_order_release);
            if (use_zlib) {
                GetRuntime().total_zlib_decompressions.fetch_add(1u, std::memory_order_relaxed);
                RecordOk("DirectStorage::XboxZlibDecompress");
            }
        }

        if (cur_size != dst_size) {
            return (use_zlib || bcpack_mode != 0u)
                       ? OS_DSTORAGE_E_DECOMPRESSION_ERROR
                       : OS_DSTORAGE_E_INVALIDARG;
        }

        // 5. Stage 3 Hardware Swizzle (Options.SwizzleMode).
        if (swizzle_mode != 0u) {
            std::vector<uint8_t>& swizzled_out =
                (!buf_a.empty() && cur_ptr == buf_a.data()) ? buf_b : buf_a;
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
                static_cast<const uintptr_t*>(resolved_dst),
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

        auto* dst_ptr = static_cast<uint8_t*>(resolved_dst);
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
                ReserveZlibOutputHint(out_data, src_size, max_out);
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
        std::vector<uint8_t>& unpacked,
        const std::atomic<bool>* cancel_flag) {
        auto* dst_res = static_cast<ID3D12Resource*>(req.Destination.Memory.Buffer);
        if (!dst_res) return OS_DSTORAGE_E_INVALIDARG;

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
            if (FAILED(dst_res->GetDevice(kOsDStorageIidId3d12Device,
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
        std::vector<uint8_t>& unpacked,
        std::vector<uint8_t>& linear_buf,
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
            if (FAILED(dst_tex->GetDevice(kOsDStorageIidId3d12Device,
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
        linear_buf.clear();
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

        ScopedRequestScratch scratch;
        std::vector<uint8_t>& file_buf = scratch.a();
        std::vector<uint8_t>& work_buf = scratch.b();

        // Handle GPU texture & buffer destinations (automatic BCPack/GDeflate -> XgGpuUntiler upload).
        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TILES ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
#if defined(_WIN32)
            const uint8_t* src_ptr = nullptr;
            size_t src_size = 0u;
            const int32_t src_hr = AcquireSourceBytes(req, staging_limit, file_buf, src_ptr, src_size, cancel_flag);
            if (src_hr < 0) return src_hr;
            if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
                return ExecuteGpuBufferRequest(req, src_ptr, src_size, staging_limit, work_buf, cancel_flag);
            }
            return ExecuteGpuTextureRequest(
                req, src_ptr, src_size, staging_limit, work_buf, scratch.c(), cancel_flag);
#else
            return OS_DSTORAGE_E_INVALIDARG;
#endif
        }

        if (req.Options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (!req.Destination.Memory.Buffer) {
            return OS_DSTORAGE_E_POINTER;
        }
        if (req.Destination.Memory.Size == 0) {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        void* raw_dst_ptr = req.Destination.Memory.Buffer;
        const size_t dst_cap = req.Destination.Memory.Size;
        DestinationClassification dst_class{};
        const int32_t dst_hr = ValidateAndClassifyDestination(raw_dst_ptr, dst_cap, dst_class);
        if (dst_hr < 0) {
            return dst_hr;
        }
        auto* dst_ptr = static_cast<uint8_t*>(raw_dst_ptr);

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

                work_buf.resize(header_uncompressed);
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, work_buf.data(), work_buf.size(), &out_size);
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
                    std::memcpy(dst_ptr, work_buf.data(), out_size);
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
                ReserveZlibOutputHint(work_buf, src_size, max_out);
                if (zlib_rfc1950::Decompress(src_ptr, src_size, max_out, work_buf)) {
                    if (req.UncompressedSize != 0u && work_buf.size() != req.UncompressedSize) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                    last_intermediate_size_.store(
                        static_cast<uint32_t>(std::min<size_t>(work_buf.size(), UINT32_MAX)),
                        std::memory_order_release);
                    if (!work_buf.empty()) {
                        std::memcpy(dst_ptr, work_buf.data(), work_buf.size());
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
                if (!openscarlett::BcPackDecompress(src_ptr, src_size, work_buf).ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (work_buf.size() > dst_cap) return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                if (!work_buf.empty()) {
                    std::memcpy(dst_ptr, work_buf.data(), work_buf.size());
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

                const auto st = openscarlett::BcPackDecompress(src_ptr, src_size, work_buf);
                if (!st.ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (work_buf.size() > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && work_buf.size() != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (!work_buf.empty()) {
                    std::memcpy(dst_ptr, work_buf.data(), work_buf.size());
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            default:
                return OS_DSTORAGE_E_INVALIDARG;
        }
    }

    void ResetErrorRecordLocked() noexcept {
        std::memset(&error_record_, 0, sizeof(error_record_));
        error_record_.FailureCount = 0u;
        error_record_.FirstFailure.HResult = OS_DSTORAGE_S_OK;
        error_record_.FirstFailure.CommandType = DSTORAGE_COMMAND_TYPE_NONE;
    }

    void RecordCommandFailure(
        int32_t hr,
        DSTORAGE_COMMAND_TYPE cmd_type,
        IDStorageStatusArrayX* status_arr = nullptr,
        uint32_t status_index = 0u,
        void* signal_fence = nullptr,
        uint64_t signal_value = 0ull) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (batch_hr_ >= 0) {
                batch_hr_ = hr;
            }
        }
        completed_failed_count_.fetch_add(1u, std::memory_order_relaxed);
        std::lock_guard<std::mutex> elk(error_mutex_);
        if (error_record_.FailureCount == 0u) {
            ResetErrorRecordLocked();
            auto& ff = error_record_.FirstFailure;
            ff.HResult = hr;
            ff.CommandType = cmd_type;
            if (cmd_type == DSTORAGE_COMMAND_TYPE_STATUS) {
                ff.Status.StatusArray = status_arr;
                ff.Status.Index = status_index;
            } else if (cmd_type == DSTORAGE_COMMAND_TYPE_SIGNAL) {
                ff.Signal.Fence = signal_fence;
                ff.Signal.Value = signal_value;
            }
        }
        ++error_record_.FailureCount;
#if defined(_WIN32)
        if (error_event_) {
            SetEvent(error_event_);
        }
#endif
    }

    void RecordFailure(int32_t hr, const QueueItem& item) {
        static const bool trace_ds = []() {
#if defined(_WIN32)
            char buf[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DSTORAGE", buf, sizeof(buf)) > 0 && buf[0] != '0';
#else
            return false;
#endif
        }();
        if (trace_ds) {
            static std::atomic<uint32_t> s_fail_log{0u};
            if (s_fail_log.fetch_add(1u, std::memory_order_relaxed) < 24u) {
                const wchar_t* fpath = L"<mem>";
                if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                    item.request.Source.File.Source) {
                    const auto* fimpl = static_cast<const DStorageFileImpl*>(item.request.Source.File.Source);
                    fpath = fimpl->FilePath().c_str();
                }
                std::fprintf(stderr,
                             "[OpenScarlett][dstorage] FAIL hr=0x%08lX opts=0x%llX dst=%p dstSz=0x%X srcSz=0x%X off=0x%llX file=%ls\n",
                             static_cast<unsigned long>(hr),
                             static_cast<unsigned long long>(item.xbox_options),
                             item.request.Destination.Memory.Buffer,
                             item.request.Destination.Memory.Size,
                             item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE
                                 ? item.request.Source.File.Size
                                 : item.request.Source.Memory.Size,
                             item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE
                                 ? static_cast<unsigned long long>(item.request.Source.File.Offset)
                                 : 0ull,
                             fpath);
                std::fflush(stderr);
            }
        }
        const OpenScarlettDStorageRequestDesc& req = item.request;
        std::lock_guard<std::mutex> lk(error_mutex_);
        if (error_record_.FailureCount == 0) {
            ResetErrorRecordLocked();
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
                if (req.Source.File.Source) {
                    const auto* fimpl = static_cast<const DStorageFileImpl*>(req.Source.File.Source);
                    const std::wstring& fp = fimpl->FilePath();
                    size_t i = 0u;
                    for (; i + 1u < DSTORAGE_ERROR_RECORD_MAX_PATH && i < fp.size(); ++i) {
                        r.Filename[i] = static_cast<char16_t>(fp[i]);
                    }
                    r.Filename[i] = u'\0';
                }
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

public:
    void CloseInternal() {
        // Keep DStorageQueueCore alive across CloseInternal() if invoked while still owned by
        // shared_ptr (weak_from_this() safely yields nullptr if invoked from ~DStorageQueueCore()).
        const auto self_keepalive = weak_from_this().lock();
        std::deque<QueueItem> leftover_staged;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (closed_) {
                // Wait for the concurrent Close() call to finish draining & unregistering
                // so Release() / ~DStorageQueueImpl() never races active teardown!
                close_cv_.wait(lk, [&]() { return close_complete_; });
                return;
            }
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
            } else if (item.kind == QueueItem::Kind::Event && item.event_handle) {
#if defined(_WIN32)
                CloseHandle(static_cast<HANDLE>(item.event_handle));
#endif
            } else if (item.kind == QueueItem::Kind::Signal && item.signal_fence) {
#if defined(_WIN32)
                static_cast<ID3D12Fence*>(item.signal_fence)->Release();
#endif
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
        {
            std::lock_guard<std::mutex> lk(mutex_);
            close_complete_ = true;
            // Notify while holding mutex_ so concurrent Release()/Close() cannot destroy
            // DStorageQueueCore / close_cv_ before notify_all() returns!
            close_cv_.notify_all();
        }
    }

private:
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
    std::condition_variable close_cv_;
    bool closed_ = false;
    bool close_complete_ = false;
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
// IDStorageQueueX / X1 / X2 COM Wrapper (owns shared_ptr<DStorageQueueCore>)
// ===========================================================================
class DStorageQueueImpl final : public IDStorageQueueX2 {
public:
    explicit DStorageQueueImpl(std::shared_ptr<DStorageQueueCore> core)
        : core_(std::move(core)) {}

    ~DStorageQueueImpl() {
        if (core_) {
            core_->CloseInternal();
        }
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
        if (GuidEquals(riid, IID_IDStorageQueueX2) ||
            GuidEquals(riid, IID_IDStorageQueueX2_Xbox)) {
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

    void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        core_->EnqueueRequest(request);
    }

    void EnqueueRequestDesc(const OpenScarlettDStorageRequestDesc& request) {
        core_->EnqueueRequestDesc(request);
    }

    void SetDevice(void* device) { core_->SetDevice(device); }
    void SetTitleVaMirror(void* mirror) { core_->SetTitleVaMirror(mirror); }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) override {
        core_->EnqueueStatus(statusArray, index);
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) override {
        core_->EnqueueSetEvent(handle);
    }

    void OPENSCARLETT_DSTORAGE_CALL Submit() override {
        core_->Submit();
    }

    void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) override {
        core_->CancelRequestsWithTag(mask, value);
    }

    void GetQueueStats(DSTORAGE_QUEUE_STATS& out) {
        core_->GetQueueStats(out);
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        core_->Close();
    }

    void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() override {
        return core_->GetErrorEvent();
    }

    void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        core_->RetrieveErrorRecord(record);
    }

    void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) override {
        core_->Query(info);
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSignal(void* fence, uint64_t value) override {
        core_->EnqueueSignal(fence, value);
    }

    void OPENSCARLETT_DSTORAGE_CALL Query1(DSTORAGE_QUEUE_INFO1* info) override {
        core_->Query1(info);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetAutoSubmitDisabled(bool disabled) override {
        return core_->SetAutoSubmitDisabled(disabled);
    }

    bool OPENSCARLETT_DSTORAGE_CALL GetAutoSubmitDisabled() override {
        return core_->GetAutoSubmitDisabled();
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::shared_ptr<DStorageQueueCore> core_;
};

// ===========================================================================
// Retail Xbox GDK IDStorageQueueX / IDStorageQueueX2 Vtable Wrapper
// Matches Microsoft dstorage_xs.h binary vtable order for IID_IDStorageQueueX_Xbox
// ({A3F6649A-...}) and IID_IDStorageQueueX2_Xbox ({3C02CB0C-...}):
//   Slot  3: EnqueueRequest(const DSTORAGE_REQUEST*)
//   Slot  4: EnqueueStatus(IDStorageStatusArrayX*, uint32_t)
//   Slot  5: EnqueueSignal(ID3D12Fence*, uint64_t)
//   Slot  6: Submit()
//   Slot  7: CancelRequestsWithTag(uint64_t, uint64_t)
//   Slot  8: Close()
//   Slot  9: GetErrorEvent()
//   Slot 10: RetrieveErrorRecord(DSTORAGE_ERROR_RECORD*)
//   Slot 11: Query(DSTORAGE_QUEUE_INFO*)
//   Slot 12: EnqueueSetEvent(HANDLE)
//   Slot 13: Query1(RetailXboxQueueInfo1*)
// ===========================================================================
struct RetailXboxQueueDesc1 {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;  // +0x00 (UINT64)
    uint16_t Capacity;                        // +0x08
    DSTORAGE_PRIORITY Priority;               // +0x0A (INT8)
    const char* Name;                         // +0x10
    uint64_t Options;                         // +0x18
};
static_assert(sizeof(RetailXboxQueueDesc1) == 0x20, "RetailXboxQueueDesc1 must be 0x20 bytes");

struct RetailXboxQueueInfo1 {
    RetailXboxQueueDesc1 Desc;                // +0x00..+0x1F
    uint16_t EmptySlotCount;                  // +0x20
    uint16_t RequestCountUntilAutoSubmit;     // +0x22
    uint32_t Padding;                         // +0x24
};
static_assert(sizeof(RetailXboxQueueInfo1) == 0x28, "RetailXboxQueueInfo1 must be 0x28 bytes");

struct IRetailXboxDStorageQueueVtbl {
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL Release() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueSignal(void* fence, uint64_t value) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Submit() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Close() = 0;
    virtual void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(
        DSTORAGE_ERROR_RECORD* record) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Query1(RetailXboxQueueInfo1* info) = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL SetAutoSubmitDisabled(bool disabled) = 0;
    virtual bool OPENSCARLETT_DSTORAGE_CALL GetAutoSubmitDisabled() = 0;
};

class DStorageRetailXboxQueueImpl final : public IRetailXboxDStorageQueueVtbl {
public:
    explicit DStorageRetailXboxQueueImpl(std::shared_ptr<DStorageQueueCore> core)
        : core_(std::move(core)) {}

    virtual ~DStorageRetailXboxQueueImpl() = default;

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) {
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        }
        *ppvObject = nullptr;
        if (!riid) {
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        }
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageQueueX_Xbox) ||
            GuidEquals(riid, IID_IDStorageQueueX2_Xbox)) {
            AddRef();
            *ppvObject = static_cast<IRetailXboxDStorageQueueVtbl*>(this);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        if (GuidEquals(riid, IID_IDStorageQueueX) ||
            GuidEquals(riid, IID_IDStorageQueueX1) ||
            GuidEquals(riid, IID_IDStorageQueueX2)) {
            auto* compat = new (std::nothrow) DStorageQueueImpl(core_);
            if (!compat) {
                return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_OUTOFMEMORY, 0);
            }
            *ppvObject = static_cast<IDStorageQueueX2*>(compat);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageQueueX::AddRef", 1);
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageQueueX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        core_->EnqueueRequest(request);
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) override {
        core_->EnqueueStatus(statusArray, index);
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSignal(void* fence, uint64_t value) override {
        core_->EnqueueSignal(fence, value);
    }

    void OPENSCARLETT_DSTORAGE_CALL Submit() override {
        core_->Submit();
    }

    void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) override {
        core_->CancelRequestsWithTag(mask, value);
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        core_->Close();
    }

    void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() override {
        return core_->GetErrorEvent();
    }

    void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        core_->RetrieveErrorRecord(record);
    }

    void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) override {
        core_->Query(info);
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) override {
        core_->EnqueueSetEvent(handle);
    }

    void OPENSCARLETT_DSTORAGE_CALL Query1(RetailXboxQueueInfo1* info) override {
        if (!info) return;
        DSTORAGE_QUEUE_INFO1 internal_info{};
        core_->Query1(&internal_info);
        info->Desc.SourceType = internal_info.Desc.SourceType;
        info->Desc.Capacity = internal_info.Desc.Capacity;
        info->Desc.Priority = internal_info.Desc.Priority;
        info->Desc.Name = internal_info.Desc.Name;
        info->Desc.Options = internal_info.Desc.Options;
        info->EmptySlotCount = internal_info.EmptySlotCount;
        info->RequestCountUntilAutoSubmit = internal_info.RequestCountUntilAutoSubmit;
        info->Padding = 0u;
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetAutoSubmitDisabled(bool disabled) override {
        return core_->SetAutoSubmitDisabled(disabled);
    }

    bool OPENSCARLETT_DSTORAGE_CALL GetAutoSubmitDisabled() override {
        return core_->GetAutoSubmitDisabled();
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::shared_ptr<DStorageQueueCore> core_;
};

// ===========================================================================
// Multi-Threaded DStoragePriorityScheduler Method Definitions
// ===========================================================================
void DStoragePriorityScheduler::RegisterQueue(
    const std::shared_ptr<DStorageQueueCore>& q,
    DSTORAGE_PRIORITY priority) {
    std::lock_guard<std::mutex> lk(mutex_);
    slots_.push_back(Slot{q, PriorityToBucket(priority), 0u, false});
    if (!worker_running_) {
        worker_running_ = true;
        const uint64_t my_gen = ++generation_;
        const unsigned hw = std::thread::hardware_concurrency();
        const size_t num_workers = std::clamp<size_t>(hw ? hw : 4u, 2u, 4u);
        workers_.reserve(num_workers);
        for (size_t i = 0u; i < num_workers; ++i) {
            workers_.emplace_back(&DStoragePriorityScheduler::RunLoop, this, my_gen);
        }
    }
    work_cv_.notify_all();
}

void DStoragePriorityScheduler::UnregisterQueue(const DStorageQueueCore* q) {
    std::vector<std::thread> to_join;
    {
        std::unique_lock<std::mutex> lk(mutex_);
        for (auto& s : slots_) {
            if (s.queue.get() == q) {
                s.removing = true;
                break;
            }
        }
        idle_cv_.wait(lk, [&]() {
            for (const auto& s : slots_) {
                if (s.queue.get() == q) return s.active_workers == 0u;
            }
            return true;
        });
        slots_.erase(
            std::remove_if(slots_.begin(), slots_.end(),
                           [q](const Slot& s) { return s.queue.get() == q; }),
            slots_.end());

        if (slots_.empty() && worker_running_) {
            ++generation_;
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

void DStoragePriorityScheduler::RunLoop(uint64_t my_generation) {
    for (;;) {
        std::shared_ptr<DStorageQueueCore> chosen;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            auto pick_next = [&]() -> std::shared_ptr<DStorageQueueCore> {
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
                        if (s.bucket != b || s.removing || !s.queue) continue;
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
                if (generation_ != my_generation) return true;
                chosen = pick_next();
                return chosen != nullptr;
            });

            if (generation_ != my_generation && !chosen) {
                return;
            }
        }

        if (chosen) {
            chosen->ProcessOneActiveItem();
            bool notify_next = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (auto& s : slots_) {
                    if (s.queue.get() == chosen.get()) {
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
            chosen.reset();
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
        if (GuidEquals(riid, IID_IDStorageFactoryX2) ||
            GuidEquals(riid, IID_IDStorageFactoryX2_Xbox)) {
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
            !GuidEquals(riid, IID_IDStorageQueueX2_Xbox) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_NOINTERFACE, 3);
        }
        if (desc->SourceType != DSTORAGE_REQUEST_SOURCE_FILE &&
            desc->SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_INVALIDARG, 3);
        }
        auto core = DStorageQueueCore::Create(*desc, staging_size_, DSTORAGE_QUEUE_OPTIONS_NONE);
        if (!core) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_OUTOFMEMORY, 3);
        }
        if (riid && (GuidEquals(riid, IID_IDStorageQueueX_Xbox) ||
                     GuidEquals(riid, IID_IDStorageQueueX2_Xbox))) {
            auto* rq = new (std::nothrow) DStorageRetailXboxQueueImpl(std::move(core));
            if (!rq) {
                return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_OUTOFMEMORY, 3);
            }
            *ppv = static_cast<IRetailXboxDStorageQueueVtbl*>(rq);
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_S_OK, 3);
        }
        auto* q = new (std::nothrow) DStorageQueueImpl(std::move(core));
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
        bool skip_sync = false;
        const bool attached = DStorageIocpEngine::Global().AttachFile(h, &skip_sync);
        auto* f = new (std::nothrow) DStorageFileImpl(h, attached, path, skip_sync);
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
#if defined(_WIN32)
        // Retail Scarlett GRDK 2504/2510 IDStorageFactoryX2 ({62959D11-...}) places a UINT32
        // buffer/staging configuration method at slot 8 (+0x40) rather than GetStats(DSTORAGE_STATS*).
        // Detect non-pointer integer arguments (e.g. 0x02000000 = 32 MiB) safely via VirtualQuery.
        MEMORY_BASIC_INFORMATION mbi{};
        const uintptr_t raw = reinterpret_cast<uintptr_t>(stats);
        if (VirtualQuery(stats, &mbi, sizeof(mbi)) == 0u ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) == 0u) {
            const uint32_t sz = static_cast<uint32_t>(raw & 0xFFFFFFFFull);
            if (sz >= DSTORAGE_MIN_STAGING_BUFFER_SIZE && sz <= DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
                staging_size_->store(sz, std::memory_order_release);
            }
            return RecordHr("IDStorageFactoryX2::Slot8_SetStagingBufferSize", OS_DSTORAGE_S_OK, 8);
        }
#endif
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
            !GuidEquals(riid, IID_IDStorageQueueX2_Xbox) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_NOINTERFACE, 10);
        }
        const bool is_retail_xbox_iid =
            riid && (GuidEquals(riid, IID_IDStorageQueueX_Xbox) ||
                     GuidEquals(riid, IID_IDStorageQueueX2_Xbox));
        if (desc->SourceType != DSTORAGE_REQUEST_SOURCE_FILE &&
            desc->SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_INVALIDARG, 10);
        }
        DSTORAGE_QUEUE_DESC base_desc{};
        uint32_t q_opts = DSTORAGE_QUEUE_OPTIONS_NONE;
        if (is_retail_xbox_iid) {
            const auto* rdesc = reinterpret_cast<const RetailXboxQueueDesc1*>(desc);
            base_desc.SourceType = rdesc->SourceType;
            base_desc.Capacity = rdesc->Capacity;
            base_desc.Priority = rdesc->Priority;
            base_desc.Name = rdesc->Name;
            q_opts = static_cast<uint32_t>(rdesc->Options & DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT);
        } else {
            if ((desc->Options & ~static_cast<uint32_t>(DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT)) != 0u) {
                return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_INVALIDARG, 10);
            }
            base_desc.SourceType = desc->SourceType;
            base_desc.Capacity = desc->Capacity;
            base_desc.Priority = desc->Priority;
            base_desc.Name = desc->Name;
            q_opts = desc->Options;
        }
        auto core = DStorageQueueCore::Create(base_desc, staging_size_, q_opts);
        if (!core) {
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_OUTOFMEMORY, 10);
        }
        if (is_retail_xbox_iid) {
            auto* rq = new (std::nothrow) DStorageRetailXboxQueueImpl(std::move(core));
            if (!rq) {
                return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_E_OUTOFMEMORY, 10);
            }
            *ppv = static_cast<IRetailXboxDStorageQueueVtbl*>(rq);
            return RecordHr("IDStorageFactoryX2::CreateQueue1", OS_DSTORAGE_S_OK, 10);
        }
        auto* q = new (std::nothrow) DStorageQueueImpl(std::move(core));
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
        !GuidEquals(riid, IID_IDStorageFactoryX2_Xbox) &&
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

#if defined(_WIN32)
    src = ResolveCpuAliasConstPointer(src);
#endif
    DestinationClassification dst_class{};
    const int32_t dst_hr = ValidateAndClassifyDestination(dst, dst_size, dst_class);
    if (dst_hr < 0) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", dst_hr);
    }

    const auto* src_bytes = static_cast<const uint8_t*>(src);
    auto* dst_bytes = static_cast<uint8_t*>(dst);

    ScopedRequestScratch scratch;
    std::vector<uint8_t>& staging = scratch.a();

    // Check custom decompressor registry first.
    CustomDecompressorEntry custom{};
    if (GetRuntime().LookupCustom(static_cast<uint8_t>(format), custom)) {
        staging.resize(intermediate_size);
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
                staging.resize(intermediate_size);
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
            if (!openscarlett::BcPackDecompress(src_bytes, src_size, staging).ok()) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (staging.size() > dst_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            if (!staging.empty()) {
                std::memcpy(dst_bytes, staging.data(), staging.size());
            }
            if (out_decompressed_size) {
                *out_decompressed_size = staging.size();
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
