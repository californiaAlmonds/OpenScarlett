#define OPENSCARLETT_BUILDING_KERNELX 1

#if defined(_WIN32)
// Temporarily rename Win32 UMS prototypes before including <windows.h> so we can
// cleanly export our user-space cooperative UMS shims from kernelx.dll without
// dllimport/dllexport conflicts across MSVC, Clang-CL, and MinGW-w64 (Win11 removed
// kernel UMS support).
#  define CreateUmsCompletionList _os_win_CreateUmsCompletionList
#  define DeleteUmsCompletionList _os_win_DeleteUmsCompletionList
#  define DequeueUmsCompletionListItems _os_win_DequeueUmsCompletionListItems
#  define GetUmsCompletionListEvent _os_win_GetUmsCompletionListEvent
#  define CreateUmsThreadContext _os_win_CreateUmsThreadContext
#  define DeleteUmsThreadContext _os_win_DeleteUmsThreadContext
#  define EnterUmsSchedulingMode _os_win_EnterUmsSchedulingMode
#  define ExecuteUmsThread _os_win_ExecuteUmsThread
#  define UmsThreadYield _os_win_UmsThreadYield
#  define QueryUmsThreadInformation _os_win_QueryUmsThreadInformation
#  define SetUmsThreadInformation _os_win_SetUmsThreadInformation
#  define GetCurrentUmsThread _os_win_GetCurrentUmsThread
#  define GetNextUmsListItem _os_win_GetNextUmsListItem
#  include <malloc.h>
#  include <windows.h>
#  undef CreateUmsCompletionList
#  undef DeleteUmsCompletionList
#  undef DequeueUmsCompletionListItems
#  undef GetUmsCompletionListEvent
#  undef CreateUmsThreadContext
#  undef DeleteUmsThreadContext
#  undef EnterUmsSchedulingMode
#  undef ExecuteUmsThread
#  undef UmsThreadYield
#  undef QueryUmsThreadInformation
#  undef SetUmsThreadInformation
#  undef GetCurrentUmsThread
#  undef GetNextUmsListItem
#endif

#include "openscarlett/kernel/kernelx_abi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/kernel/gpu_tile_pool.h"
#include "openscarlett/kernel/plm_notifications.h"
#include "openscarlett/kernel/xmemory.h"

#if defined(_MSC_VER)
// Export standard unprefixed Xbox/Win32 kernelx.dll symbol names alongside the
// header-safe KernelX-prefixed entry points so guest PE imports resolve cleanly
// without clashing with <windows.h> in host C++ translation units.
#pragma comment(linker, "/export:VirtualAllocFromApp=KernelXVirtualAllocFromApp")
#pragma comment(linker, "/export:VirtualProtectFromApp=KernelXVirtualProtectFromApp")
#pragma comment(linker, "/export:CreateWaitableTimerExW=KernelXCreateWaitableTimerExW")
#pragma comment(linker, "/export:QueryUnbiasedInterruptTime=KernelXQueryUnbiasedInterruptTime")
#pragma comment(linker, "/export:SetThreadAffinityMask=KernelXSetThreadAffinityMask")
#pragma comment(linker, "/export:SetThreadIdealProcessor=KernelXSetThreadIdealProcessor")
#pragma comment(linker, "/export:SetThreadIdealProcessorEx=KernelXSetThreadIdealProcessorEx")
#pragma comment(linker, "/export:SetThreadGroupAffinity=KernelXSetThreadGroupAffinity")
#pragma comment(linker, "/export:SetProcessAffinityMask=KernelXSetProcessAffinityMask")
#pragma comment(linker, "/export:GetProcessAffinityMask=KernelXGetProcessAffinityMask")
#pragma comment(linker, "/export:GetSystemInfo=KernelXGetSystemInfo")
#pragma comment(linker, "/export:GetCurrentProcessorNumber=KernelXGetCurrentProcessorNumber")
#endif

namespace {

using openscarlett::CallOutcome;
using openscarlett::CallTelemetry;
using openscarlett::GpuTileCoord;
using openscarlett::GpuTilePool;
using openscarlett::MemoryPageSize;
using openscarlett::MemoryProtection;
using openscarlett::PhysicalAllocation;
using openscarlett::Status;
using openscarlett::UnifiedMemoryAlias;
using openscarlett::UnifiedMemoryManager;

constexpr const char* kModuleName = "kernelx.dll";

// Xbox Series X (Anaconda): 16 GiB GDDR6 total -> 13.5 GiB title + 2.5 GiB tooling/OS.
constexpr uint64_t kAnacondaTitleBudgetBytes = 27ull * 512ull * 1024ull * 1024ull;    // 13.5 GiB
constexpr uint64_t kAnacondaToolingBudgetBytes = 5ull * 512ull * 1024ull * 1024ull;   // 2.5 GiB

// Xbox Series S (Lockhart): 10 GiB GDDR6 total -> 8.0 GiB title + 2.0 GiB tooling/OS.
constexpr uint64_t kLockhartTitleBudgetBytes = 8ull * 1024ull * 1024ull * 1024ull;    // 8.0 GiB
constexpr uint64_t kLockhartToolingBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2.0 GiB

// Xbox Series X|S title-available Zen 2 logical processors (7 title cores x 2 SMT threads).
constexpr uint32_t kScarlettTitleProcessorCount = 14u;

constexpr size_t kPage4KB = 4096u;
constexpr size_t kPage64KB = 65536u;
constexpr size_t kPage4MB = 4ull * 1024ull * 1024ull;
constexpr size_t kDefaultAuxTitleMemoryBytes = 4ull * 1024ull * 1024ull;  // 4 MiB default chunk

constexpr int32_t kHrOk = 0;
constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrInsufficientBuffer = static_cast<int32_t>(0x8007007Au);
constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);

constexpr uint64_t kScarlettAllCoresMask = (1ull << kScarlettTitleProcessorCount) - 1ull;

// Deterministic Xbox Series X|S (7 cores / 14 logical processors, bits 0..13) to
// host PC CPU affinity remapping helpers. Prevents ERROR_INVALID_PARAMETER (0) or
// thread starvation on 2/4/6/8-core PCs while preserving round-robin thread spread.
uint64_t RemapXboxAffinityMaskToHost(uint64_t guest_mask, uint64_t host_process_mask) noexcept {
    if (guest_mask == 0ull || host_process_mask == 0ull) {
        return 0ull;
    }
    uint32_t host_bits[64]{};
    uint32_t host_count = 0u;
    for (uint32_t b = 0u; b < 64u; ++b) {
        if ((host_process_mask & (1ull << b)) != 0ull) {
            host_bits[host_count++] = b;
        }
    }
    if (host_count == 0u) {
        return 0ull;
    }
    uint64_t out_mask = 0ull;
    for (uint32_t b = 0u; b < 64u; ++b) {
        if ((guest_mask & (1ull << b)) != 0ull) {
            const uint32_t mapped_bit = host_bits[b % host_count];
            out_mask |= (1ull << mapped_bit);
        }
    }
    return out_mask != 0ull ? out_mask : (1ull << host_bits[0]);
}

uint32_t RemapXboxIdealProcessorToHost(uint32_t guest_proc, uint32_t host_cpu_count) noexcept {
    if (host_cpu_count == 0u) {
        return 0u;
    }
    return guest_proc % host_cpu_count;
}

uint32_t QueryHostLogicalProcessorCount() noexcept {
#if defined(_WIN32)
    DWORD_PTR proc_mask = 0;
    DWORD_PTR sys_mask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask != 0u) {
        uint32_t count = 0u;
        const uint64_t m = static_cast<uint64_t>(proc_mask);
        for (uint32_t b = 0u; b < 64u; ++b) {
            if ((m & (1ull << b)) != 0ull) {
                ++count;
            }
        }
        if (count > 0u) {
            return count;
        }
    }
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0u ? static_cast<uint32_t>(si.dwNumberOfProcessors) : 1u;
#else
    return kScarlettTitleProcessorCount;
#endif
}

bool IsPowerOfTwo(size_t v) noexcept {
    return v != 0u && (v & (v - 1u)) == 0u;
}

bool ValidateXAllocAttributes(uint64_t attrs) noexcept {
    const uint32_t mem_type = static_cast<uint32_t>((attrs >> 28) & 0xFu);
    if (mem_type > XALLOC_MEMTYPE_PHYSICAL) {
        return false;
    }
    return true;
}

int32_t ClampThreadPriority(int32_t priority) noexcept {
    if (priority <= -15) return -15;  // THREAD_PRIORITY_IDLE
    if (priority >= 15) return 15;    // THREAD_PRIORITY_TIME_CRITICAL
    if (priority < -2) return -2;     // THREAD_PRIORITY_LOWEST
    if (priority > 2) return 2;       // THREAD_PRIORITY_HIGHEST
    return priority;
}

inline void RecordTelemetry(const char* symbol, CallOutcome outcome, int32_t hr = 0) {
#if defined(_WIN32)
    const DWORD saved_err = GetLastError();
#endif
    CallTelemetry::Global().Record(kModuleName, symbol, outcome, hr);
#if defined(_WIN32)
    SetLastError(saved_err);
#endif
}

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void SetHostLastError(uint32_t err) {
#if defined(_WIN32)
    SetLastError(static_cast<DWORD>(err));
#else
    (void)err;
#endif
}

bool ValidateProtect(uint32_t flProtect) {
    const uint32_t base = flProtect & 0xFFu;
    switch (base) {
        case PAGE_NOACCESS:
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
            return true;
        default:
            return false;
    }
}

MemoryProtection ToMemoryProtection(uint32_t flProtect) {
    const uint32_t base = flProtect & 0xFFu;
    if (base == PAGE_NOACCESS) {
        return MemoryProtection::NoAccess;
    }
    if (base == PAGE_READONLY || base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ) {
        return MemoryProtection::ReadOnly;
    }
    if ((flProtect & PAGE_WRITECOMBINE) != 0u) {
        return MemoryProtection::WriteCombined;
    }
    return MemoryProtection::ReadWrite;
}

#if defined(_WIN32)
DWORD ToWin32PageProtect(uint32_t flProtect) {
    const uint32_t base = flProtect & 0xFFu;
    DWORD result = PAGE_READWRITE;
    switch (base) {
        case PAGE_NOACCESS: result = PAGE_NOACCESS; break;
        case PAGE_READONLY: result = PAGE_READONLY; break;
        case PAGE_READWRITE: result = PAGE_READWRITE; break;
        case PAGE_WRITECOPY: result = PAGE_WRITECOPY; break;
        case PAGE_EXECUTE: result = PAGE_EXECUTE; break;
        case PAGE_EXECUTE_READ: result = PAGE_EXECUTE_READ; break;
        case PAGE_EXECUTE_READWRITE: result = PAGE_EXECUTE_READWRITE; break;
        default: result = PAGE_READWRITE; break;
    }
    if (flProtect & PAGE_GUARD) {
        result |= PAGE_GUARD;
    }
    return result;
}
#endif

uint32_t ToUnifiedFlags(uint32_t flAllocationType, uint32_t flProtect) {
    uint32_t flags = openscarlett::kMemFlagNone;
    if (flAllocationType & MEM_GRAPHICS) {
        flags |= openscarlett::kMemFlagGraphics;
    }
    if (flAllocationType & (MEM_LARGE_PAGES | MEM_4MB_PAGES)) {
        flags |= openscarlett::kMemFlagLargePages;
    }
    if (flProtect & PAGE_WRITECOMBINE) {
        flags |= openscarlett::kMemFlagWriteCombined;
    }
    return flags;
}

size_t DecodeXAllocAlignment(uint64_t attrs) {
    const uint32_t align_code = static_cast<uint32_t>((attrs >> 24) & 0xFu);
    size_t alignment = 16u;
    switch (align_code) {
        case XALLOC_ALIGNMENT_DEFAULT: alignment = 16u; break;
        case XALLOC_ALIGNMENT_4: alignment = 4u; break;
        case XALLOC_ALIGNMENT_8: alignment = 8u; break;
        case XALLOC_ALIGNMENT_16: alignment = 16u; break;
        case XALLOC_ALIGNMENT_32: alignment = 32u; break;
        case XALLOC_ALIGNMENT_64: alignment = 64u; break;
        case XALLOC_ALIGNMENT_128: alignment = 128u; break;
        case XALLOC_ALIGNMENT_256: alignment = 256u; break;
        case XALLOC_ALIGNMENT_512: alignment = 512u; break;
        case XALLOC_ALIGNMENT_1K: alignment = 1024u; break;
        case XALLOC_ALIGNMENT_2K: alignment = 2048u; break;
        case XALLOC_ALIGNMENT_4K: alignment = 4096u; break;
        case XALLOC_ALIGNMENT_8K: alignment = 8192u; break;
        case XALLOC_ALIGNMENT_16K: alignment = 16384u; break;
        case XALLOC_ALIGNMENT_32K: alignment = 32768u; break;
        case XALLOC_ALIGNMENT_64K: alignment = 65536u; break;
        default: alignment = 16u; break;
    }

    const uint32_t page_pref = static_cast<uint32_t>((attrs >> 32) & 0x3u);
    if (page_pref == XALLOC_PAGESIZE_4KB && alignment < kPage4KB) {
        alignment = kPage4KB;
    } else if (page_pref == XALLOC_PAGESIZE_64KB && alignment < kPage64KB) {
        alignment = kPage64KB;
    } else if (page_pref == XALLOC_PAGESIZE_4MB && alignment < kPage4MB) {
        alignment = kPage4MB;
    }
    return alignment;
}

uint32_t ResolveConsoleType() {
#if defined(_WIN32)
    char env_buf[64] = {};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_TYPE", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len > 0 && len < sizeof(env_buf)) {
        std::string v(env_buf, len);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "lockhart" || v == "3" || v == "series_s") {
            return CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART;
        }
        if (v == "anaconda" || v == "4" || v == "series_x") {
            return CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
        }
        if (v == "devkit" || v == "5") {
            return CONSOLE_TYPE_XBOX_SCARLETT_DEVKIT;
        }
    }
#endif
    return CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
}

uint64_t GetTitleBudgetBytes() {
    if (ResolveConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART) {
        return kLockhartTitleBudgetBytes;
    }
    return kAnacondaTitleBudgetBytes;
}

uint64_t GetToolingBudgetBytes() {
    if (ResolveConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART) {
        return kLockhartToolingBudgetBytes;
    }
    return kAnacondaToolingBudgetBytes;
}

#if defined(_WIN32)
using FnSetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
using FnGetThreadDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);

struct Win32ThreadDescApi {
    FnSetThreadDescription set_desc = nullptr;
    FnGetThreadDescription get_desc = nullptr;

    Win32ThreadDescApi() {
        HMODULE mod = GetModuleHandleW(L"kernelbase.dll");
        if (!mod) {
            mod = GetModuleHandleW(L"kernel32.dll");
        }
        if (mod) {
            set_desc = reinterpret_cast<FnSetThreadDescription>(GetProcAddress(mod, "SetThreadDescription"));
            get_desc = reinterpret_cast<FnGetThreadDescription>(GetProcAddress(mod, "GetThreadDescription"));
        }
        if (!set_desc || !get_desc) {
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            if (k32) {
                if (!set_desc) {
                    set_desc = reinterpret_cast<FnSetThreadDescription>(GetProcAddress(k32, "SetThreadDescription"));
                }
                if (!get_desc) {
                    get_desc = reinterpret_cast<FnGetThreadDescription>(GetProcAddress(k32, "GetThreadDescription"));
                }
            }
        }
    }
};

const Win32ThreadDescApi& ThreadDescApi() {
    static const Win32ThreadDescApi api;
    return api;
}
#endif

struct PhysicalPoolRecord {
    uint64_t physical_id = 0;
    size_t page_size_bytes = kPage64KB;
    uint32_t alloc_flags = 0;
    uint64_t live_pages = 0;
};

struct PageFrameRecord {
    uint64_t pfn = 0;
    uint64_t physical_id = 0;
    uint64_t page_index = 0;
    size_t page_size_bytes = kPage64KB;
    uint32_t alloc_flags = 0;
    uint32_t map_refcount = 0;
};

struct ActiveTitleMapping {
    uintptr_t base_va = 0;
    size_t size_bytes = 0;
    size_t page_size_bytes = kPage64KB;
    uint64_t primary_physical_id = 0;
    uint32_t alloc_flags = 0;
    uint32_t win32_protect = PAGE_READWRITE;
    bool owns_placeholder = false;
    std::vector<uint64_t> pfns;
    std::vector<void*> alias_ptrs;
};

struct AppVirtualAllocRecord {
    uintptr_t base = 0;
    size_t size = 0;
    size_t committed_bytes = 0;
    uint32_t alloc_flags = 0;
    uint32_t protection = PAGE_NOACCESS;
    bool is_placeholder = false;
};

struct XMemAllocRecord {
    size_t requested_size = 0;
    size_t allocated_size = 0;
    size_t alignment = 16u;
    uint64_t attributes = 0;
};

class KernelXState {
public:
    static KernelXState& Instance() {
        static KernelXState instance;
        return instance;
    }

    UnifiedMemoryManager& Memory() { return memory_; }
    GpuTilePool& TilePool() { return tile_pool_; }

    uint64_t CurrentTitleUsedBytes() const {
        const uint64_t unified = static_cast<uint64_t>(memory_.TotalAllocatedBytes());
        const uint64_t app = app_committed_bytes_.load(std::memory_order_acquire);
        const uint64_t xmem = xmem_allocated_bytes_.load(std::memory_order_acquire);
        const uint64_t aux = aux_allocated_bytes_.load(std::memory_order_acquire);
        return unified + app + xmem + aux;
    }

    uint64_t UpdateAndGetPeakTitleUsed() {
        const uint64_t current = CurrentTitleUsedBytes();
        uint64_t prev = peak_title_used_.load(std::memory_order_relaxed);
        while (current > prev &&
               !peak_title_used_.compare_exchange_weak(prev, current, std::memory_order_acq_rel)) {
        }
        return std::max(prev, current);
    }

    void* XMemAllocImpl(size_t dwSize, uint64_t dwAllocAttributes, size_t explicitAlignment = 0u) {
        if (dwSize == 0u || !ValidateXAllocAttributes(dwAllocAttributes)) {
            SetHostLastError(87u);
            return nullptr;
        }
        if (explicitAlignment != 0u && (!IsPowerOfTwo(explicitAlignment) || explicitAlignment > kPage4MB)) {
            SetHostLastError(87u);
            return nullptr;
        }
        const uint64_t budget = GetTitleBudgetBytes();
        if (static_cast<uint64_t>(dwSize) > budget) {
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return nullptr;
        }
        const size_t attr_align = DecodeXAllocAlignment(dwAllocAttributes);
        const size_t alignment = std::max(attr_align, explicitAlignment);
        const size_t pad_granule = std::min<size_t>(alignment, kPage64KB);
        if (dwSize > (SIZE_MAX - pad_granule)) {
            SetHostLastError(8u);
            return nullptr;
        }
        const size_t alloc_size = AlignUp(dwSize, pad_granule);
        if (CurrentTitleUsedBytes() + alloc_size > budget) {
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return nullptr;
        }

#if defined(_WIN32)
        void* ptr = _aligned_malloc(alloc_size, alignment);
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, std::max<size_t>(alignment, sizeof(void*)), alloc_size) != 0) {
            ptr = nullptr;
        }
#endif
        if (!ptr) {
            SetHostLastError(8u);
            return nullptr;
        }

        const bool zero_init = ((dwAllocAttributes & XALLOC_FLAG_ZERO_INIT) != 0ull) ||
                               (((dwAllocAttributes >> 28) & 0xFu) != XALLOC_MEMTYPE_HEAP);
        if (zero_init) {
            std::memset(ptr, 0, alloc_size);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            XMemAllocRecord rec{};
            rec.requested_size = dwSize;
            rec.allocated_size = alloc_size;
            rec.alignment = alignment;
            rec.attributes = dwAllocAttributes;
            xmem_allocs_.emplace(reinterpret_cast<uintptr_t>(ptr), rec);
        }
        xmem_allocated_bytes_.fetch_add(alloc_size, std::memory_order_acq_rel);
        UpdateAndGetPeakTitleUsed();
        return ptr;
    }

    bool XMemFreeImpl(void* pAddress) {
        if (!pAddress) {
            SetHostLastError(0u);
            return true;
        }
        size_t freed_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = xmem_allocs_.find(reinterpret_cast<uintptr_t>(pAddress));
            if (it == xmem_allocs_.end()) {
                SetHostLastError(87u);
                return false;
            }
            freed_bytes = it->second.allocated_size;
            xmem_allocs_.erase(it);
        }
#if defined(_WIN32)
        _aligned_free(pAddress);
#else
        std::free(pAddress);
#endif
        xmem_allocated_bytes_.fetch_sub(freed_bytes, std::memory_order_acq_rel);
        SetHostLastError(0u);
        return true;
    }

    size_t XMemSizeImpl(void* pAddress) const {
        if (!pAddress) {
            return 0u;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = xmem_allocs_.find(reinterpret_cast<uintptr_t>(pAddress));
        if (it == xmem_allocs_.end()) {
            return 0u;
        }
        return it->second.requested_size;
    }

    size_t XMemVirtualQueryImpl(const void* address, void* buffer, size_t length) const {
        if (!buffer || length < sizeof(XMEM_MEMORY_BASIC_INFORMATION)) {
            SetHostLastError(87u);
            return 0u;
        }
#if defined(_WIN32)
        static_assert(sizeof(XMEM_MEMORY_BASIC_INFORMATION) == sizeof(MEMORY_BASIC_INFORMATION),
                      "XMEM_MEMORY_BASIC_INFORMATION must match Win32 MEMORY_BASIC_INFORMATION");
#endif
        auto* out = static_cast<XMEM_MEMORY_BASIC_INFORMATION*>(buffer);
        std::memset(out, 0, sizeof(XMEM_MEMORY_BASIC_INFORMATION));

        if (!address) {
            SetHostLastError(87u);
            return 0u;
        }

        const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
        constexpr uint32_t kMemPrivate = 0x00020000u;
        constexpr uint32_t kMemMapped = 0x00040000u;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 1. Tracked XMemAlloc / XMemAllocAligned / XMemAllocDefault allocations
            for (const auto& [base, rec] : xmem_allocs_) {
                if (addr >= base && addr < base + rec.allocated_size) {
                    openscarlett::EraAllocationAttributes era{};
                    openscarlett::DecodeEraAllocationAttributes(rec.attributes, era);
                    const uintptr_t page_mask = static_cast<uintptr_t>(kPage4KB - 1u);
                    const uintptr_t aligned_addr = addr & ~page_mask;
                    const uintptr_t region_base = (aligned_addr >= base) ? aligned_addr : base;
                    const uintptr_t region_end = base + rec.allocated_size;
                    out->BaseAddress = reinterpret_cast<void*>(region_base);
                    out->AllocationBase = reinterpret_cast<void*>(base);
                    out->AllocationProtect = era.protect_flags;
                    out->RegionSize = static_cast<size_t>(region_end - region_base);
                    out->State = MEM_COMMIT;
                    out->Protect = era.protect_flags;
                    out->Type = kMemPrivate | era.alloc_flags;
                    return sizeof(XMEM_MEMORY_BASIC_INFORMATION);
                }
            }

            // 2. Tracked MapTitlePhysicalPages / MapTitleVirtualMemory / XMemMapPhysicalPages mappings
            for (const auto& [base, m] : mappings_) {
                if (addr >= base && addr < base + m.size_bytes) {
                    const uintptr_t gran_mask = static_cast<uintptr_t>(kPage4KB - 1u);
                    const uintptr_t region_base = std::max(base, addr & ~gran_mask);
                    const uintptr_t region_end = base + m.size_bytes;
                    out->BaseAddress = reinterpret_cast<void*>(region_base);
                    out->AllocationBase = reinterpret_cast<void*>(base);
                    out->AllocationProtect = m.win32_protect;
                    out->RegionSize = static_cast<size_t>(region_end - region_base);
                    out->State = MEM_COMMIT;
                    out->Protect = m.win32_protect;
                    out->Type = kMemMapped | (m.alloc_flags & (MEM_GRAPHICS | MEM_TITLE |
                                                               MEM_WRITE_COMBINE | MEM_LARGE_PAGES |
                                                               MEM_4MB_PAGES));
                    return sizeof(XMEM_MEMORY_BASIC_INFORMATION);
                }
            }

            // 3. Tracked VirtualAllocFromApp allocations & placeholders
            for (const auto& [base, rec] : app_allocs_) {
                if (addr >= base && addr < base + rec.size) {
                    const uintptr_t region_base = std::max(base, addr & ~static_cast<uintptr_t>(kPage4KB - 1u));
                    const uintptr_t region_end = base + rec.size;
                    out->BaseAddress = reinterpret_cast<void*>(region_base);
                    out->AllocationBase = reinterpret_cast<void*>(base);
                    out->AllocationProtect = rec.protection;
                    out->RegionSize = static_cast<size_t>(region_end - region_base);
                    out->State = (rec.committed_bytes > 0u && !rec.is_placeholder) ? MEM_COMMIT : MEM_RESERVE;
                    out->Protect = rec.is_placeholder ? PAGE_NOACCESS : rec.protection;
                    out->Type = kMemPrivate | (rec.alloc_flags & (MEM_GRAPHICS | MEM_TITLE |
                                                                  MEM_WRITE_COMBINE | MEM_LARGE_PAGES |
                                                                  MEM_4MB_PAGES));
                    return sizeof(XMEM_MEMORY_BASIC_INFORMATION);
                }
            }

            // 4. Tracked Auxiliary Title Memory allocations
            for (const auto& [base, sz] : aux_allocs_) {
                if (addr >= base && addr < base + sz) {
                    const uintptr_t region_base = std::max(base, addr & ~static_cast<uintptr_t>(kPage4KB - 1u));
                    const uintptr_t region_end = base + sz;
                    out->BaseAddress = reinterpret_cast<void*>(region_base);
                    out->AllocationBase = reinterpret_cast<void*>(base);
                    out->AllocationProtect = PAGE_READWRITE;
                    out->RegionSize = static_cast<size_t>(region_end - region_base);
                    out->State = MEM_COMMIT;
                    out->Protect = PAGE_READWRITE;
                    out->Type = kMemPrivate | MEM_TITLE;
                    return sizeof(XMEM_MEMORY_BASIC_INFORMATION);
                }
            }
        }

        // 5. Tracked UnifiedMemoryManager blocks or aliases
        openscarlett::UnifiedMemoryBlock block{};
        if (memory_.LookupBlockByHostPtr(address, block)) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(block.host_ptr);
            const uintptr_t region_base = std::max(base, addr & ~static_cast<uintptr_t>(kPage4KB - 1u));
            uint32_t era_flags = MEM_TITLE;
            if (block.gpu_visible) era_flags |= MEM_GRAPHICS;
            if (block.write_combined) era_flags |= MEM_WRITE_COMBINE;
            if (block.page_size == MemoryPageSize::Page2MB) era_flags |= MEM_LARGE_PAGES;
            if (block.page_size == MemoryPageSize::Page4MB) era_flags |= MEM_4MB_PAGES | MEM_LARGE_PAGES;
            const uint32_t prot = block.write_combined ? (PAGE_READWRITE | PAGE_WRITECOMBINE) : PAGE_READWRITE;
            out->BaseAddress = reinterpret_cast<void*>(region_base);
            out->AllocationBase = block.host_ptr;
            out->AllocationProtect = prot;
            out->RegionSize = static_cast<size_t>((base + block.size) - region_base);
            out->State = MEM_COMMIT;
            out->Protect = prot;
            out->Type = kMemPrivate | era_flags;
            return sizeof(XMEM_MEMORY_BASIC_INFORMATION);
        }

        UnifiedMemoryAlias alias{};
        if (memory_.LookupAliasByHostPtr(address, alias)) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(alias.host_ptr);
            const uintptr_t region_base = std::max(base, addr & ~static_cast<uintptr_t>(kPage4KB - 1u));
            uint32_t era_flags = MEM_TITLE;
            if (alias.flags & openscarlett::kMemFlagGraphics) era_flags |= MEM_GRAPHICS;
            if (alias.flags & openscarlett::kMemFlagLargePages) era_flags |= MEM_LARGE_PAGES;
            if (alias.flags & openscarlett::kMemFlagWriteCombined) era_flags |= MEM_WRITE_COMBINE;
            const uint32_t prot = (alias.protection == MemoryProtection::ReadOnly)
                                      ? PAGE_READONLY
                                      : ((alias.protection == MemoryProtection::NoAccess)
                                             ? PAGE_NOACCESS
                                             : ((alias.protection == MemoryProtection::WriteCombined)
                                                    ? (PAGE_READWRITE | PAGE_WRITECOMBINE)
                                                    : PAGE_READWRITE));
            out->BaseAddress = reinterpret_cast<void*>(region_base);
            out->AllocationBase = alias.host_ptr;
            out->AllocationProtect = prot;
            out->RegionSize = static_cast<size_t>((base + alias.size) - region_base);
            out->State = MEM_COMMIT;
            out->Protect = prot;
            out->Type = kMemMapped | era_flags;
            return sizeof(XMEM_MEMORY_BASIC_INFORMATION);
        }

        // 6. Host Win32 VirtualQuery fallback for standard process memory ranges
#if defined(_WIN32)
        MEMORY_BASIC_INFORMATION host_mbi{};
        const SIZE_T got = ::VirtualQuery(address, &host_mbi, sizeof(host_mbi));
        if (got == 0u) {
            return 0u;
        }
        std::memcpy(out, &host_mbi, sizeof(host_mbi));
        return static_cast<size_t>(got);
#else
        SetHostLastError(87u);
        return 0u;
#endif
    }

    int32_t XMemGetAllocationAttributesImpl(const void* address,
                                            XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded) const {
        if (!address || !outDecoded) {
            SetHostLastError(87u);
            return 0;
        }
        std::memset(outDecoded, 0, sizeof(*outDecoded));
        const uintptr_t addr = reinterpret_cast<uintptr_t>(address);

        auto copy_from_era = [&](const openscarlett::EraAllocationAttributes& era) {
            outDecoded->objectType = era.object_type;
            outDecoded->heapTracksAttributes = era.heap_tracks_attributes;
            outDecoded->mustSucceed = era.must_succeed;
            outDecoded->fixedSize = era.fixed_size;
            outDecoded->allocatorId = era.allocator_id;
            outDecoded->alignmentCode = era.alignment_code;
            outDecoded->memoryType = era.memory_type;
            outDecoded->pageSizeCode = era.page_size_code;
            outDecoded->zeroInitialize = era.zero_initialize;
            outDecoded->allocFlags = era.alloc_flags;
            outDecoded->protectFlags = era.protect_flags;
            outDecoded->alignmentBytes = era.alignment_bytes;
            outDecoded->pageSizeBytes = era.page_size_bytes;
            outDecoded->rawAttributes = era.raw_attributes;
        };

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [base, rec] : xmem_allocs_) {
                if (addr >= base && addr < base + rec.allocated_size) {
                    openscarlett::EraAllocationAttributes era{};
                    if (!openscarlett::DecodeEraAllocationAttributes(rec.attributes, era)) {
                        return 0;
                    }
                    era.alignment_bytes = std::max<uint64_t>(era.alignment_bytes, rec.alignment);
                    copy_from_era(era);
                    return 1;
                }
            }
            for (const auto& [base, m] : mappings_) {
                if (addr >= base && addr < base + m.size_bytes) {
                    const uint32_t page_code =
                        (m.page_size_bytes >= kPage4MB)
                            ? XALLOC_PAGESIZE_4MB
                            : ((m.page_size_bytes >= kPage64KB) ? XALLOC_PAGESIZE_64KB : XALLOC_PAGESIZE_4KB);
                    const bool wc = (m.win32_protect & PAGE_WRITECOMBINE) != 0u ||
                                    (m.alloc_flags & MEM_WRITE_COMBINE) != 0u;
                    const uint32_t mem_type = wc ? XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE
                                                 : XALLOC_MEMTYPE_PHYSICAL;
                    const uint64_t raw = MAKE_XALLOC_ATTRIBUTES(
                        0u, 1u, 0u, 0u, 0u, XALLOC_ALIGNMENT_64K, mem_type, page_code, 0u);
                    openscarlett::EraAllocationAttributes era{};
                    openscarlett::DecodeEraAllocationAttributes(raw, era);
                    era.page_size_bytes = m.page_size_bytes;
                    era.alloc_flags |= (m.alloc_flags & (MEM_GRAPHICS | MEM_TITLE | MEM_WRITE_COMBINE |
                                                         MEM_LARGE_PAGES | MEM_4MB_PAGES));
                    era.protect_flags = m.win32_protect;
                    copy_from_era(era);
                    return 1;
                }
            }
            for (const auto& [base, rec] : app_allocs_) {
                if (addr >= base && addr < base + rec.size) {
                    const bool is_4mb = (rec.alloc_flags & MEM_4MB_PAGES) != 0u;
                    const bool is_large = (rec.alloc_flags & MEM_LARGE_PAGES) != 0u;
                    const bool is_gfx = (rec.alloc_flags & MEM_GRAPHICS) != 0u;
                    const bool is_wc = (rec.protection & PAGE_WRITECOMBINE) != 0u ||
                                       (rec.alloc_flags & MEM_WRITE_COMBINE) != 0u;
                    const uint32_t page_code =
                        is_4mb ? XALLOC_PAGESIZE_4MB
                               : (is_gfx ? XALLOC_PAGESIZE_64KB : XALLOC_PAGESIZE_4KB);
                    const uint32_t mem_type =
                        is_gfx ? (is_wc ? XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE
                                        : XALLOC_MEMTYPE_GRAPHICS_CACHEABLE)
                               : XALLOC_MEMTYPE_HEAP;
                    uint64_t raw = MAKE_XALLOC_ATTRIBUTES(
                        0u, 1u, 0u, 0u, 0u, XALLOC_ALIGNMENT_DEFAULT, mem_type, page_code, 0u);
                    if (is_large && !is_4mb) {
                        raw |= XALLOC_FLAG_2MB_PAGES;
                    }
                    openscarlett::EraAllocationAttributes era{};
                    openscarlett::DecodeEraAllocationAttributes(raw, era);
                    era.alloc_flags |= (rec.alloc_flags & (MEM_GRAPHICS | MEM_TITLE | MEM_WRITE_COMBINE |
                                                           MEM_LARGE_PAGES | MEM_4MB_PAGES));
                    era.protect_flags = rec.protection;
                    copy_from_era(era);
                    return 1;
                }
            }
            for (const auto& [base, sz] : aux_allocs_) {
                if (addr >= base && addr < base + sz) {
                    const uint64_t raw = MAKE_XALLOC_ATTRIBUTES(
                        0u, 1u, 0u, 0u, 0u, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_HEAP,
                        XALLOC_PAGESIZE_64KB, 1u);
                    openscarlett::EraAllocationAttributes era{};
                    openscarlett::DecodeEraAllocationAttributes(raw, era);
                    copy_from_era(era);
                    return 1;
                }
            }
        }

        // Host Win32 fallback: synthesize baseline 4KB page attributes for valid committed ranges
#if defined(_WIN32)
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(address, &mbi, sizeof(mbi)) != 0u && mbi.State != MEM_FREE) {
            outDecoded->memoryType = XALLOC_MEMTYPE_HEAP;
            outDecoded->pageSizeCode = XALLOC_PAGESIZE_4KB;
            outDecoded->alignmentBytes = kPage4KB;
            outDecoded->pageSizeBytes = kPage4KB;
            outDecoded->protectFlags = static_cast<uint32_t>(mbi.Protect);
            outDecoded->allocFlags = 0u;
            return 1;
        }
#endif
        SetHostLastError(87u);
        return 0;
    }

    void* GetAuxTitleMemory(size_t requestedSize, size_t* outActualSize) {
        const size_t raw_size = (requestedSize == 0) ? kDefaultAuxTitleMemoryBytes : requestedSize;
        const size_t aligned_size = AlignUp(raw_size, kPage64KB);
        if (CurrentTitleUsedBytes() + aligned_size > GetTitleBudgetBytes()) {
            if (outActualSize) {
                *outActualSize = 0;
            }
            SetHostLastError(8u);
            return nullptr;
        }

#if defined(_WIN32)
        void* ptr = VirtualAlloc(nullptr, aligned_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        void* ptr = std::calloc(1, aligned_size);
#endif
        if (!ptr) {
            if (outActualSize) {
                *outActualSize = 0;
            }
            SetHostLastError(8u);
            return nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            aux_allocs_.emplace(reinterpret_cast<uintptr_t>(ptr), aligned_size);
        }
        aux_allocated_bytes_.fetch_add(aligned_size, std::memory_order_acq_rel);
        UpdateAndGetPeakTitleUsed();
        if (outActualSize) {
            *outActualSize = aligned_size;
        }
        return ptr;
    }

    int32_t ReleaseAuxTitleMemory(void* baseAddress) {
        if (!baseAddress) {
            SetHostLastError(87u);
            return 0;
        }
        size_t freed_size = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = aux_allocs_.find(reinterpret_cast<uintptr_t>(baseAddress));
            if (it == aux_allocs_.end()) {
                SetHostLastError(87u);
                return 0;
            }
            freed_size = it->second;
            aux_allocs_.erase(it);
        }
#if defined(_WIN32)
        if (!VirtualFree(baseAddress, 0, MEM_RELEASE)) {
            return 0;
        }
#else
        std::free(baseAddress);
#endif
        aux_allocated_bytes_.fetch_sub(freed_size, std::memory_order_acq_rel);
        return 1;
    }

    int32_t AllocatePhysicalPages(uint32_t flAllocationType,
                                  uint64_t* pNumberOfPages,
                                  uint64_t* pPageArray) {
        if (!pNumberOfPages || !pPageArray || *pNumberOfPages == 0) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0;
        }

        const bool large_pages = (flAllocationType & (MEM_LARGE_PAGES | MEM_4MB_PAGES)) != 0u;
        const size_t page_bytes = large_pages ? kPage4MB : kPage64KB;
        const MemoryPageSize page_enum = large_pages ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
        const uint64_t req_pages = *pNumberOfPages;

        if (req_pages > (GetTitleBudgetBytes() / page_bytes)) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return 0;
        }

        const uint64_t total_bytes_64 = req_pages * static_cast<uint64_t>(page_bytes);
        const uint64_t budget = GetTitleBudgetBytes();
        if (CurrentTitleUsedBytes() + total_bytes_64 > budget) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return 0;
        }

        const uint32_t mem_flags = ToUnifiedFlags(flAllocationType, 0u);
        PhysicalAllocation phys{};
        Status st = memory_.AllocatePhysicalPages(static_cast<size_t>(total_bytes_64),
                                                  page_enum,
                                                  mem_flags,
                                                  phys);
        if (!st.ok()) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            PhysicalPoolRecord pool{};
            pool.physical_id = phys.physical_id;
            pool.page_size_bytes = page_bytes;
            pool.alloc_flags = flAllocationType;
            pool.live_pages = req_pages;
            pools_.emplace(phys.physical_id, pool);

            for (uint64_t i = 0; i < req_pages; ++i) {
                const uint64_t pfn = next_pfn_++;
                PageFrameRecord rec{};
                rec.pfn = pfn;
                rec.physical_id = phys.physical_id;
                rec.page_index = i;
                rec.page_size_bytes = page_bytes;
                rec.alloc_flags = flAllocationType;
                rec.map_refcount = 0;
                pfn_table_.emplace(pfn, rec);
                pPageArray[i] = pfn;
            }
        }

        UpdateAndGetPeakTitleUsed();
        return 1;
    }

    int32_t FreePhysicalPages(uint64_t numberOfPages, const uint64_t* pPageArray) {
        if (numberOfPages == 0 || !pPageArray) {
            SetHostLastError(87u);
            return 0;
        }

        std::vector<uint64_t> pools_to_free;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (numberOfPages > pfn_table_.size()) {
                SetHostLastError(87u);
                return 0;
            }
            std::unordered_map<uint64_t, uint64_t> seen;
            seen.reserve(static_cast<size_t>(numberOfPages));
            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uint64_t pfn = pPageArray[i];
                if (seen.find(pfn) != seen.end()) {
                    SetHostLastError(87u);
                    return 0;
                }
                auto it = pfn_table_.find(pfn);
                if (it == pfn_table_.end() || it->second.map_refcount > 0) {
                    SetHostLastError(87u);
                    return 0;
                }
                seen.emplace(pfn, it->second.physical_id);
            }

            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uint64_t pfn = pPageArray[i];
                auto it = pfn_table_.find(pfn);
                const uint64_t phys_id = it->second.physical_id;
                pfn_table_.erase(it);

                auto pit = pools_.find(phys_id);
                if (pit != pools_.end()) {
                    if (pit->second.live_pages > 0) {
                        --pit->second.live_pages;
                    }
                    if (pit->second.live_pages == 0) {
                        pools_to_free.push_back(phys_id);
                        pools_.erase(pit);
                    }
                }
            }
        }

        for (uint64_t phys_id : pools_to_free) {
            Status st = memory_.FreePhysicalPages(phys_id);
            if (!st.ok()) {
                SetHostLastError(87u);
                return 0;
            }
        }
        return 1;
    }

    void* MapPhysicalPages(void* virtualAddress,
                           uint64_t numberOfPages,
                           uint32_t flAllocationType,
                           uint32_t flProtect,
                           const uint64_t* pPageArray) {
        if (numberOfPages == 0 || !pPageArray || !ValidateProtect(flProtect)) {
            SetHostLastError(87u);
            return nullptr;
        }
        if (numberOfPages > (GetTitleBudgetBytes() / kPage64KB)) {
            SetHostLastError(87u);
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PageFrameRecord> frames;
        frames.reserve(static_cast<size_t>(numberOfPages));
        size_t page_bytes = 0;
        bool is_contiguous_single_pool = true;

        for (uint64_t i = 0; i < numberOfPages; ++i) {
            auto it = pfn_table_.find(pPageArray[i]);
            if (it == pfn_table_.end()) {
                SetHostLastError(87u);
                return nullptr;
            }
            if (i == 0) {
                page_bytes = it->second.page_size_bytes;
            } else {
                if (it->second.page_size_bytes != page_bytes) {
                    SetHostLastError(87u);
                    return nullptr;
                }
                if (it->second.physical_id != frames[0].physical_id ||
                    it->second.page_index != frames[0].page_index + i) {
                    is_contiguous_single_pool = false;
                }
            }
            frames.push_back(it->second);
        }

        if (numberOfPages > (static_cast<uint64_t>(SIZE_MAX) / static_cast<uint64_t>(page_bytes))) {
            SetHostLastError(87u);
            return nullptr;
        }
        const size_t total_bytes = static_cast<size_t>(numberOfPages) * page_bytes;
        if (virtualAddress != nullptr) {
            const uintptr_t req_check = reinterpret_cast<uintptr_t>(virtualAddress);
            if (req_check + total_bytes < req_check) {
                SetHostLastError(487u);
                return nullptr;
            }
        }
        const MemoryPageSize page_granule =
            (page_bytes == kPage4MB) ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
        const MemoryProtection prot = ToMemoryProtection(flProtect);
        const uint32_t mem_flags = ToUnifiedFlags(flAllocationType | frames[0].alloc_flags, flProtect);

        void* base_va = nullptr;
        bool owns_placeholder = false;
        std::vector<void*> mapped_ptrs;

        if (virtualAddress != nullptr) {
            const uintptr_t req = reinterpret_cast<uintptr_t>(virtualAddress);
            bool valid_placeholder = false;
            for (const auto& [b, rec] : app_allocs_) {
                if (rec.is_placeholder && req >= b && req + total_bytes <= b + rec.size) {
                    valid_placeholder = true;
                    break;
                }
            }
            if (!valid_placeholder || !UnifiedMemoryManager::SupportsOsPlaceholders()) {
                SetHostLastError(487u);  // ERROR_INVALID_ADDRESS
                return nullptr;
            }

            if (is_contiguous_single_pool) {
                UnifiedMemoryAlias alias{};
                const size_t phys_off = static_cast<size_t>(frames[0].page_index) * page_bytes;
                Status st = memory_.MapPhysicalAlias(frames[0].physical_id,
                                                     phys_off,
                                                     total_bytes,
                                                     prot,
                                                     mem_flags,
                                                     virtualAddress,
                                                     alias);
                if (!st.ok()) {
                    SetHostLastError(487u);
                    return nullptr;
                }
                base_va = alias.host_ptr;
                mapped_ptrs.push_back(alias.host_ptr);
            } else {
                base_va = virtualAddress;
                for (uint64_t i = 0; i < numberOfPages; ++i) {
                    void* slot = static_cast<uint8_t*>(virtualAddress) + static_cast<size_t>(i) * page_bytes;
                    UnifiedMemoryAlias alias{};
                    Status st = memory_.MapPhysicalAlias(frames[i].physical_id,
                                                         static_cast<size_t>(frames[i].page_index) * page_bytes,
                                                         page_bytes,
                                                         prot,
                                                         mem_flags,
                                                         slot,
                                                         alias);
                    if (!st.ok()) {
                        for (void* p : mapped_ptrs) {
                            (void)memory_.UnmapPhysicalAlias(p);
                        }
                        SetHostLastError(487u);
                        return nullptr;
                    }
                    mapped_ptrs.push_back(alias.host_ptr);
                }
            }
        } else if (UnifiedMemoryManager::SupportsOsPlaceholders()) {
            // Reserve a properly aligned OS virtual placeholder (64 KiB or 4 MiB granule)
            // and map physical pages via MapViewOfFile3(MEM_REPLACE_PLACEHOLDER).
            Status ph_st = memory_.ReserveVirtualPlaceholder(total_bytes, base_va, page_granule);
            if (!ph_st.ok()) {
                SetHostLastError(8u);
                return nullptr;
            }
            owns_placeholder = true;

            if (is_contiguous_single_pool) {
                UnifiedMemoryAlias alias{};
                const size_t phys_off = static_cast<size_t>(frames[0].page_index) * page_bytes;
                Status st = memory_.MapPhysicalAlias(frames[0].physical_id,
                                                     phys_off,
                                                     total_bytes,
                                                     prot,
                                                     mem_flags,
                                                     base_va,
                                                     alias);
                if (!st.ok()) {
                    (void)memory_.ReleaseVirtualPlaceholder(base_va);
                    SetHostLastError(8u);
                    return nullptr;
                }
                mapped_ptrs.push_back(alias.host_ptr);
            } else {
                for (uint64_t i = 0; i < numberOfPages; ++i) {
                    void* slot = static_cast<uint8_t*>(base_va) + static_cast<size_t>(i) * page_bytes;
                    UnifiedMemoryAlias alias{};
                    Status st = memory_.MapPhysicalAlias(frames[i].physical_id,
                                                         static_cast<size_t>(frames[i].page_index) * page_bytes,
                                                         page_bytes,
                                                         prot,
                                                         mem_flags,
                                                         slot,
                                                         alias);
                    if (!st.ok()) {
                        for (void* p : mapped_ptrs) {
                            (void)memory_.UnmapPhysicalAlias(p);
                        }
                        (void)memory_.ReleaseVirtualPlaceholder(base_va);
                        SetHostLastError(8u);
                        return nullptr;
                    }
                    mapped_ptrs.push_back(alias.host_ptr);
                }
            }
        } else if (is_contiguous_single_pool) {
            UnifiedMemoryAlias alias{};
            const size_t phys_off = static_cast<size_t>(frames[0].page_index) * page_bytes;
            Status st = memory_.MapPhysicalAlias(frames[0].physical_id,
                                                 phys_off,
                                                 total_bytes,
                                                 prot,
                                                 mem_flags,
                                                 nullptr,
                                                 alias);
            if (!st.ok()) {
                SetHostLastError(8u);
                return nullptr;
            }
            base_va = alias.host_ptr;
            mapped_ptrs.push_back(alias.host_ptr);
        } else {
            SetHostLastError(50u);  // ERROR_NOT_SUPPORTED
            return nullptr;
        }

        ActiveTitleMapping mapping{};
        mapping.base_va = reinterpret_cast<uintptr_t>(base_va);
        mapping.size_bytes = total_bytes;
        mapping.page_size_bytes = page_bytes;
        mapping.primary_physical_id = frames[0].physical_id;
        mapping.alloc_flags = flAllocationType | frames[0].alloc_flags;
        mapping.win32_protect = flProtect;
        mapping.owns_placeholder = owns_placeholder;
        mapping.pfns.assign(pPageArray, pPageArray + numberOfPages);
        mapping.alias_ptrs = std::move(mapped_ptrs);

        for (uint64_t pfn : mapping.pfns) {
            ++pfn_table_[pfn].map_refcount;
        }
        mappings_.emplace(mapping.base_va, std::move(mapping));
        return base_va;
    }

    int32_t UnmapPhysicalPages(void* virtualAddress, uint64_t numberOfPages) {
        if (!virtualAddress) {
            SetHostLastError(87u);
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const uintptr_t key = reinterpret_cast<uintptr_t>(virtualAddress);
        auto it = mappings_.find(key);
        if (it == mappings_.end()) {
            SetHostLastError(87u);
            return 0;
        }
        if (numberOfPages != 0 && numberOfPages != it->second.pfns.size()) {
            SetHostLastError(87u);
            return 0;
        }

        for (void* alias_ptr : it->second.alias_ptrs) {
            Status st = memory_.UnmapPhysicalAlias(alias_ptr);
            if (!st.ok()) {
                SetHostLastError(87u);
                return 0;
            }
        }
        if (it->second.owns_placeholder) {
            (void)memory_.ReleaseVirtualPlaceholder(virtualAddress);
        }

        for (uint64_t pfn : it->second.pfns) {
            auto pit = pfn_table_.find(pfn);
            if (pit != pfn_table_.end() && pit->second.map_refcount > 0) {
                --pit->second.map_refcount;
            }
        }

        mappings_.erase(it);
        return 1;
    }

    int32_t QueryPhysicalPages(const void* virtualAddress,
                               uint64_t* outPhysicalId,
                               uint64_t* outSizeBytes,
                               uint32_t* outFlags) const {
        if (!virtualAddress) {
            SetHostLastError(87u);
            return 0;
        }

        const uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [base, m] : mappings_) {
                if (addr >= base && addr < base + m.size_bytes) {
                    uint64_t phys_id = m.primary_physical_id;
                    if (m.page_size_bytes > 0 && !m.pfns.empty()) {
                        const size_t page_idx = static_cast<size_t>((addr - base) / m.page_size_bytes);
                        if (page_idx < m.pfns.size()) {
                            auto pit = pfn_table_.find(m.pfns[page_idx]);
                            if (pit != pfn_table_.end()) {
                                phys_id = pit->second.physical_id;
                            }
                        }
                    }
                    if (outPhysicalId) *outPhysicalId = phys_id;
                    if (outSizeBytes) *outSizeBytes = m.size_bytes;
                    if (outFlags) *outFlags = m.alloc_flags;
                    return 1;
                }
            }
        }

        UnifiedMemoryAlias alias{};
        if (memory_.LookupAliasByHostPtr(virtualAddress, alias)) {
            if (outPhysicalId) *outPhysicalId = alias.physical_id;
            if (outSizeBytes) *outSizeBytes = alias.size;
            if (outFlags) {
                uint32_t f = 0;
                if (alias.flags & openscarlett::kMemFlagGraphics) f |= MEM_GRAPHICS;
                if (alias.flags & openscarlett::kMemFlagLargePages) f |= MEM_LARGE_PAGES;
                *outFlags = f;
            }
            return 1;
        }

        SetHostLastError(87u);
        return 0;
    }

    void* VirtualAllocApp(void* baseAddress,
                          size_t size,
                          uint32_t allocationType,
                          uint32_t protection) {
        if (size == 0 || allocationType == 0 || !ValidateProtect(protection)) {
            SetHostLastError(87u);
            return nullptr;
        }

        const bool reserve_only =
            ((allocationType & MEM_RESERVE) != 0u) && ((allocationType & MEM_COMMIT) == 0u);
        const bool is_graphics = (allocationType & MEM_GRAPHICS) != 0u;
        const bool is_large = (allocationType & (MEM_LARGE_PAGES | MEM_4MB_PAGES)) != 0u;

        // MEM_RESERVE | MEM_GRAPHICS creates a real OS virtual placeholder so
        // MapTitleVirtualMemory / MapTitlePhysicalPages(va, ...) can bind physical pages directly.
        if (reserve_only && is_graphics && baseAddress == nullptr &&
            UnifiedMemoryManager::SupportsOsPlaceholders()) {
            const MemoryPageSize granule = is_large ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
            const size_t gran_bytes = is_large ? kPage4MB : kPage64KB;
            const size_t aligned = AlignUp(size, gran_bytes);
            void* base = nullptr;
            Status st = memory_.ReserveVirtualPlaceholder(aligned, base, granule);
            if (!st.ok()) {
                SetHostLastError(8u);
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            AppVirtualAllocRecord rec{};
            rec.base = reinterpret_cast<uintptr_t>(base);
            rec.size = aligned;
            rec.committed_bytes = 0;
            rec.alloc_flags = allocationType;
            rec.protection = protection;
            rec.is_placeholder = true;
            app_allocs_.emplace(rec.base, rec);
            return base;
        }

        const size_t align = is_large ? kPage4MB : (is_graphics ? kPage64KB : kPage4KB);
        const size_t aligned_size = AlignUp(size, align);

        if ((allocationType & MEM_COMMIT) != 0u &&
            CurrentTitleUsedBytes() + aligned_size > GetTitleBudgetBytes()) {
            SetHostLastError(8u);
            return nullptr;
        }

#if defined(_WIN32)
        DWORD win_alloc = 0;
        if (allocationType & MEM_COMMIT) win_alloc |= MEM_COMMIT;
        if (allocationType & MEM_RESERVE) win_alloc |= MEM_RESERVE;
        if (allocationType & MEM_RESET) win_alloc |= MEM_RESET;
        if (allocationType & MEM_TOP_DOWN) win_alloc |= MEM_TOP_DOWN;
        if (win_alloc == 0) {
            SetHostLastError(87u);
            return nullptr;
        }
        const DWORD win_prot = ToWin32PageProtect(protection);
        void* ptr = VirtualAlloc(baseAddress, aligned_size, win_alloc, win_prot);
        if (!ptr) {
            return nullptr;
        }
#else
        (void)baseAddress;
        void* ptr = std::calloc(1, aligned_size);
        if (!ptr) {
            return nullptr;
        }
#endif

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
            auto it = app_allocs_.find(key);
            if (it != app_allocs_.end()) {
                if ((allocationType & MEM_COMMIT) != 0u) {
                    const size_t new_committed = std::max(it->second.committed_bytes, aligned_size);
                    const size_t delta = new_committed - it->second.committed_bytes;
                    it->second.committed_bytes = new_committed;
                    app_committed_bytes_.fetch_add(delta, std::memory_order_acq_rel);
                }
                it->second.protection = protection;
            } else {
                AppVirtualAllocRecord rec{};
                rec.base = key;
                rec.size = aligned_size;
                rec.committed_bytes = (allocationType & MEM_COMMIT) ? aligned_size : 0;
                rec.alloc_flags = allocationType;
                rec.protection = protection;
                rec.is_placeholder = false;
                app_allocs_.emplace(key, rec);
                if (rec.committed_bytes > 0) {
                    app_committed_bytes_.fetch_add(rec.committed_bytes, std::memory_order_acq_rel);
                }
            }
        }

        UpdateAndGetPeakTitleUsed();
        return ptr;
    }

    int32_t VirtualFreeApp(void* address, size_t size, uint32_t freeType) {
        if (!address || freeType == 0) {
            SetHostLastError(87u);
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const uintptr_t key = reinterpret_cast<uintptr_t>(address);
        auto it = app_allocs_.find(key);
        if (it != app_allocs_.end()) {
            if (it->second.is_placeholder) {
                Status st = memory_.ReleaseVirtualPlaceholder(address);
                if (!st.ok()) {
                    SetHostLastError(87u);
                    return 0;
                }
                app_allocs_.erase(it);
                return 1;
            }

            const bool is_release = (freeType & MEM_RELEASE) != 0u;
#if defined(_WIN32)
            if (!VirtualFree(address, is_release ? 0 : size, static_cast<DWORD>(freeType))) {
                return 0;
            }
#else
            (void)size;
            if (is_release) {
                std::free(address);
            }
#endif
            if (is_release) {
                if (it->second.committed_bytes > 0) {
                    app_committed_bytes_.fetch_sub(it->second.committed_bytes, std::memory_order_acq_rel);
                }
                app_allocs_.erase(it);
            } else if (freeType & MEM_DECOMMIT) {
                const size_t decommit = (size == 0 || size >= it->second.committed_bytes)
                                            ? it->second.committed_bytes
                                            : size;
                it->second.committed_bytes -= decommit;
                app_committed_bytes_.fetch_sub(decommit, std::memory_order_acq_rel);
            }
            return 1;
        }

#if defined(_WIN32)
        return VirtualFree(address, size, static_cast<DWORD>(freeType)) ? 1 : 0;
#else
        return 0;
#endif
    }

    int32_t VirtualProtectApp(void* address,
                              size_t size,
                              uint32_t newProtection,
                              uint32_t* oldProtection) {
        if (!address || size == 0 || !oldProtection || !ValidateProtect(newProtection)) {
            SetHostLastError(87u);
            return 0;
        }

        const uintptr_t req_start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t req_end = req_start + size;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [base, m] : mappings_) {
                if (req_start >= base && req_end <= base + m.size_bytes) {
                    const uint32_t prev = m.win32_protect;
                    const MemoryProtection mem_prot = ToMemoryProtection(newProtection);

                    if (m.alias_ptrs.size() == 1 && req_start == base && size == m.size_bytes) {
                        Status st = memory_.ProtectAlias(m.alias_ptrs[0], size, mem_prot);
                        if (!st.ok()) {
                            SetHostLastError(87u);
                            return 0;
                        }
                    } else {
                        for (void* alias_ptr : m.alias_ptrs) {
                            const uintptr_t a_start = reinterpret_cast<uintptr_t>(alias_ptr);
                            const size_t a_size =
                                (m.alias_ptrs.size() == 1) ? m.size_bytes : m.page_size_bytes;
                            const uintptr_t a_end = a_start + a_size;
                            if (a_end <= req_start || a_start >= req_end) {
                                continue;
                            }
                            if (req_start <= a_start && req_end >= a_end) {
                                Status st = memory_.ProtectAlias(alias_ptr, a_size, mem_prot);
                                if (!st.ok()) {
                                    SetHostLastError(87u);
                                    return 0;
                                }
                            } else {
#if defined(_WIN32)
                                const uintptr_t sub_start = std::max(req_start, a_start);
                                const uintptr_t sub_end = std::min(req_end, a_end);
                                DWORD host_old = 0;
                                if (!VirtualProtect(reinterpret_cast<void*>(sub_start),
                                                    sub_end - sub_start,
                                                    ToWin32PageProtect(newProtection),
                                                    &host_old)) {
                                    return 0;
                                }
#endif
                            }
                        }
                    }

                    *oldProtection = prev;
                    m.win32_protect = newProtection;
                    return 1;
                }
            }

            for (auto& [base, rec] : app_allocs_) {
                if (req_start >= base && req_end <= base + rec.size) {
                    if (rec.is_placeholder) {
                        SetHostLastError(487u);
                        return 0;
                    }
#if defined(_WIN32)
                    DWORD host_old = 0;
                    if (!VirtualProtect(address, size, ToWin32PageProtect(newProtection), &host_old)) {
                        return 0;
                    }
#endif
                    *oldProtection = rec.protection;
                    rec.protection = newProtection;
                    return 1;
                }
            }
        }

#if defined(_WIN32)
        DWORD host_old = 0;
        if (!VirtualProtect(address, size, ToWin32PageProtect(newProtection), &host_old)) {
            return 0;
        }
        *oldProtection = static_cast<uint32_t>(host_old);
        return 1;
#else
        *oldProtection = PAGE_READWRITE;
        return 1;
#endif
    }

    int32_t SetThreadNameImpl(void* hThread, const wchar_t* lpThreadName) {
        if (!lpThreadName) {
            return kHrInvalidArg;
        }
#if defined(_WIN32)
        if (hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);
            return kHrHandle;
        }
        HANDLE target = hThread ? static_cast<HANDLE>(hThread) : GetCurrentThread();
        const DWORD tid = GetThreadId(target);
        if (tid == 0u) {
            SetHostLastError(6u);
            return kHrHandle;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            thread_names_[tid] = lpThreadName;
        }
        if (ThreadDescApi().set_desc != nullptr) {
            const HRESULT hr = ThreadDescApi().set_desc(target, lpThreadName);
            if (FAILED(hr)) {
                return static_cast<int32_t>(hr);
            }
        }
        return kHrOk;
#else
        (void)hThread;
        std::lock_guard<std::mutex> lock(mutex_);
        thread_names_[0] = lpThreadName;
        return kHrOk;
#endif
    }

    int32_t GetThreadNameImpl(void* hThread,
                              wchar_t* lpThreadName,
                              size_t nBufferLength,
                              size_t* pnRequiredLength) {
        std::wstring resolved;
#if defined(_WIN32)
        if (hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);
            return kHrHandle;
        }
        HANDLE target = hThread ? static_cast<HANDLE>(hThread) : GetCurrentThread();
        const DWORD tid = GetThreadId(target);
        if (tid == 0u) {
            SetHostLastError(6u);
            return kHrHandle;
        }
        bool got_from_os = false;
        if (ThreadDescApi().get_desc != nullptr) {
            PWSTR desc = nullptr;
            if (SUCCEEDED(ThreadDescApi().get_desc(target, &desc)) && desc != nullptr) {
                resolved.assign(desc);
                LocalFree(desc);
                got_from_os = true;
            }
        }
        if (!got_from_os || resolved.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = thread_names_.find(tid);
            if (it != thread_names_.end()) {
                resolved = it->second;
            }
        }
#else
        (void)hThread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = thread_names_.find(0);
            if (it != thread_names_.end()) {
                resolved = it->second;
            }
        }
#endif
        const size_t required = resolved.size() + 1u;
        if (pnRequiredLength) {
            *pnRequiredLength = required;
        }
        if (!lpThreadName || nBufferLength < required) {
            return kHrInsufficientBuffer;
        }
        std::memcpy(lpThreadName, resolved.c_str(), required * sizeof(wchar_t));
        return kHrOk;
    }

    uint64_t SetThreadAffinityMaskImpl(void* hThread, uint64_t dwThreadAffinityMask) {
#if defined(_WIN32)
        if (!hThread || hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return 0ull;
        }
        HANDLE target = static_cast<HANDLE>(hThread);
        const DWORD tid = GetThreadId(target);
        if (tid == 0u) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return 0ull;
        }
        if (dwThreadAffinityMask == 0ull) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0ull;
        }

        DWORD_PTR proc_mask = 0;
        DWORD_PTR sys_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) || proc_mask == 0) {
            proc_mask = 1u;
        }
        const uint64_t allowed_guest_bits = kScarlettAllCoresMask | static_cast<uint64_t>(proc_mask);
        const uint64_t valid_guest_mask = dwThreadAffinityMask & allowed_guest_bits;
        if (valid_guest_mask == 0ull) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0ull;
        }

        const uint64_t remapped = RemapXboxAffinityMaskToHost(
            valid_guest_mask, static_cast<uint64_t>(proc_mask));
        DWORD_PTR host_mask = static_cast<DWORD_PTR>(remapped);
        if (host_mask == 0u) {
            host_mask = proc_mask;
        }

        const DWORD_PTR os_prev = ::SetThreadAffinityMask(target, host_mask);
        if (os_prev == 0u) {
            return 0ull;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = thread_affinities_.find(tid);
        const uint64_t prev_logical = (it != thread_affinities_.end())
                                          ? it->second
                                          : (kScarlettAllCoresMask & allowed_guest_bits);
        thread_affinities_[tid] = valid_guest_mask;
        return prev_logical;
#else
        if (!hThread || dwThreadAffinityMask == 0ull) {
            return 0ull;
        }
        return kScarlettAllCoresMask;
#endif
    }

    uint32_t SetThreadIdealProcessorImpl(void* hThread, uint32_t dwIdealProcessor) {
#if defined(_WIN32)
        if (!hThread || hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);
            return static_cast<uint32_t>(-1);
        }
        HANDLE target = static_cast<HANDLE>(hThread);
        const DWORD tid = GetThreadId(target);
        if (tid == 0u) {
            SetHostLastError(6u);
            return static_cast<uint32_t>(-1);
        }
        if (dwIdealProcessor == MAXIMUM_PROCESSORS) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = thread_ideal_procs_.find(tid);
            if (it != thread_ideal_procs_.end()) {
                return it->second;
            }
            return ::GetCurrentProcessorNumber() % kScarlettTitleProcessorCount;
        }
        const uint32_t host_cpus = QueryHostLogicalProcessorCount();
        const uint32_t mapped_host_proc = RemapXboxIdealProcessorToHost(dwIdealProcessor, host_cpus);
        const DWORD os_prev = ::SetThreadIdealProcessor(target, static_cast<DWORD>(mapped_host_proc));
        if (os_prev == static_cast<DWORD>(-1)) {
            return static_cast<uint32_t>(-1);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = thread_ideal_procs_.find(tid);
        const uint32_t prev_logical = (it != thread_ideal_procs_.end())
                                          ? it->second
                                          : (static_cast<uint32_t>(os_prev) % kScarlettTitleProcessorCount);
        thread_ideal_procs_[tid] = dwIdealProcessor % kScarlettTitleProcessorCount;
        return prev_logical;
#else
        if (!hThread) {
            return static_cast<uint32_t>(-1);
        }
        return dwIdealProcessor % kScarlettTitleProcessorCount;
#endif
    }

    int32_t SetThreadIdealProcessorExImpl(void* hThread,
                                          const void* lpIdealProcessor,
                                          void* lpPreviousIdealProcessor) {
#if defined(_WIN32)
        if (!hThread || hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);
            return 0;
        }
        if (!lpIdealProcessor) {
            SetHostLastError(87u);
            return 0;
        }
        HANDLE target = static_cast<HANDLE>(hThread);
        const DWORD tid = GetThreadId(target);
        if (tid == 0u) {
            SetHostLastError(6u);
            return 0;
        }
        const auto* req = static_cast<const PROCESSOR_NUMBER*>(lpIdealProcessor);
        const uint32_t host_cpus = QueryHostLogicalProcessorCount();
        PROCESSOR_NUMBER host_req{};
        host_req.Group = 0;
        host_req.Number = static_cast<BYTE>(RemapXboxIdealProcessorToHost(req->Number, host_cpus));
        host_req.Reserved = 0;

        PROCESSOR_NUMBER host_prev{};
        if (!::SetThreadIdealProcessorEx(target, &host_req, &host_prev)) {
            return 0;
        }
        uint32_t prev_logical = static_cast<uint32_t>(host_prev.Number) % kScarlettTitleProcessorCount;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = thread_ideal_procs_.find(tid);
            if (it != thread_ideal_procs_.end()) {
                prev_logical = it->second;
            }
            thread_ideal_procs_[tid] = static_cast<uint32_t>(req->Number) % kScarlettTitleProcessorCount;
        }
        if (lpPreviousIdealProcessor) {
            auto* prev_out = static_cast<PROCESSOR_NUMBER*>(lpPreviousIdealProcessor);
            prev_out->Group = 0;
            prev_out->Number = static_cast<BYTE>(prev_logical);
            prev_out->Reserved = 0;
        }
        return 1;
#else
        (void)lpPreviousIdealProcessor;
        return (hThread && lpIdealProcessor) ? 1 : 0;
#endif
    }

    int32_t SetThreadGroupAffinityImpl(void* hThread,
                                       const void* groupAffinity,
                                       void* previousGroupAffinity) {
#if defined(_WIN32)
        if (!hThread || hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);
            return 0;
        }
        if (!groupAffinity) {
            SetHostLastError(87u);
            return 0;
        }
        const auto* req = static_cast<const GROUP_AFFINITY*>(groupAffinity);
        const uint64_t prev = SetThreadAffinityMaskImpl(hThread, static_cast<uint64_t>(req->Mask));
        if (prev == 0ull) {
            return 0;
        }
        if (previousGroupAffinity) {
            auto* out = static_cast<GROUP_AFFINITY*>(previousGroupAffinity);
            std::memset(out, 0, sizeof(GROUP_AFFINITY));
            out->Mask = static_cast<KAFFINITY>(prev);
            out->Group = 0;
        }
        return 1;
#else
        (void)previousGroupAffinity;
        return (hThread && groupAffinity) ? 1 : 0;
#endif
    }

    int32_t SetProcessAffinityMaskImpl(void* hProcess, uint64_t dwProcessAffinityMask) {
#if defined(_WIN32)
        if (!hProcess) {
            SetHostLastError(6u);
            return 0;
        }
        HANDLE target = static_cast<HANDLE>(hProcess);
        if (target != ::GetCurrentProcess() && ::GetProcessId(target) == 0u) {
            SetHostLastError(6u);
            return 0;
        }
        if (dwProcessAffinityMask == 0ull) {
            SetHostLastError(87u);
            return 0;
        }
        DWORD_PTR proc_mask = 0;
        DWORD_PTR sys_mask = 0;
        if (!::GetProcessAffinityMask(::GetCurrentProcess(), &proc_mask, &sys_mask) || sys_mask == 0u) {
            sys_mask = 1u;
        }
        const uint64_t allowed = kScarlettAllCoresMask | static_cast<uint64_t>(sys_mask);
        const uint64_t valid_guest = dwProcessAffinityMask & allowed;
        if (valid_guest == 0ull) {
            SetHostLastError(87u);
            return 0;
        }
        const uint64_t remapped = RemapXboxAffinityMaskToHost(valid_guest, static_cast<uint64_t>(sys_mask));
        if (remapped == 0ull) {
            SetHostLastError(87u);
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        guest_process_affinity_mask_ = valid_guest;
        return 1;
#else
        return (hProcess && dwProcessAffinityMask != 0ull) ? 1 : 0;
#endif
    }

    int32_t GetProcessAffinityMaskImpl(void* hProcess,
                                       uint64_t* lpProcessAffinityMask,
                                       uint64_t* lpSystemAffinityMask) {
        if (!hProcess) {
            SetHostLastError(6u);
            return 0;
        }
        if (!lpProcessAffinityMask || !lpSystemAffinityMask) {
            SetHostLastError(87u);
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        *lpProcessAffinityMask = guest_process_affinity_mask_;
        *lpSystemAffinityMask = kScarlettAllCoresMask;
        return 1;
    }

    void GetSystemInfoImpl(void* lpSystemInfo) const {
        if (!lpSystemInfo) {
            return;
        }
#if defined(_WIN32)
        auto* si = static_cast<SYSTEM_INFO*>(lpSystemInfo);
        ::GetSystemInfo(si);
        si->dwNumberOfProcessors = kScarlettTitleProcessorCount;
        si->dwActiveProcessorMask = static_cast<DWORD_PTR>(kScarlettAllCoresMask);
#else
        (void)lpSystemInfo;
#endif
    }

    uint32_t GetCurrentProcessorNumberImpl() const {
#if defined(_WIN32)
        return static_cast<uint32_t>(::GetCurrentProcessorNumber()) % kScarlettTitleProcessorCount;
#else
        return 0u;
#endif
    }

    int32_t SetThreadPriorityImpl(void* hThread, int32_t nPriority) {
#if defined(_WIN32)
        if (!hThread || hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return 0;
        }
        HANDLE target = static_cast<HANDLE>(hThread);
        if (GetThreadId(target) == 0u) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return 0;
        }
        const int32_t clamped = ClampThreadPriority(nPriority);
        if (!::SetThreadPriority(target, clamped)) {
            return 0;
        }
        return 1;
#else
        (void)nPriority;
        return hThread ? 1 : 0;
#endif
    }

    int32_t GetThreadPriorityImpl(void* hThread) {
#if defined(_WIN32)
        if (!hThread || hThread == INVALID_HANDLE_VALUE) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return KERNELX_THREAD_PRIORITY_ERROR_RETURN;
        }
        HANDLE target = static_cast<HANDLE>(hThread);
        if (GetThreadId(target) == 0u) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return KERNELX_THREAD_PRIORITY_ERROR_RETURN;
        }
        return ::GetThreadPriority(target);
#else
        return hThread ? 0 : KERNELX_THREAD_PRIORITY_ERROR_RETURN;
#endif
    }

    ~KernelXState() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [base, m] : mappings_) {
            for (void* p : m.alias_ptrs) {
                (void)memory_.UnmapPhysicalAlias(p);
            }
            if (m.owns_placeholder) {
                (void)memory_.ReleaseVirtualPlaceholder(reinterpret_cast<void*>(base));
            }
        }
        mappings_.clear();

        for (auto& [base, rec] : app_allocs_) {
            if (rec.is_placeholder) {
                (void)memory_.ReleaseVirtualPlaceholder(reinterpret_cast<void*>(base));
            } else {
#if defined(_WIN32)
                VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
#else
                std::free(reinterpret_cast<void*>(base));
#endif
            }
        }
        app_allocs_.clear();

        for (auto& [ptr, rec] : xmem_allocs_) {
            (void)rec;
#if defined(_WIN32)
            _aligned_free(reinterpret_cast<void*>(ptr));
#else
            std::free(reinterpret_cast<void*>(ptr));
#endif
        }
        xmem_allocs_.clear();

        for (auto& [ptr, sz] : aux_allocs_) {
            (void)sz;
#if defined(_WIN32)
            VirtualFree(reinterpret_cast<void*>(ptr), 0, MEM_RELEASE);
#else
            std::free(reinterpret_cast<void*>(ptr));
#endif
        }
        aux_allocs_.clear();

        pfn_table_.clear();
        pools_.clear();
    }

private:
    KernelXState() : memory_(), tile_pool_(memory_) {}

    UnifiedMemoryManager memory_;
    GpuTilePool tile_pool_;
    mutable std::mutex mutex_;
    uint64_t next_pfn_ = 0x10000ull;
    std::atomic<uint64_t> app_committed_bytes_{0};
    std::atomic<uint64_t> xmem_allocated_bytes_{0};
    std::atomic<uint64_t> aux_allocated_bytes_{0};
    std::atomic<uint64_t> peak_title_used_{0};
    std::unordered_map<uint64_t, PhysicalPoolRecord> pools_;
    std::unordered_map<uint64_t, PageFrameRecord> pfn_table_;
    std::unordered_map<uintptr_t, ActiveTitleMapping> mappings_;
    std::unordered_map<uintptr_t, AppVirtualAllocRecord> app_allocs_;
    std::unordered_map<uintptr_t, XMemAllocRecord> xmem_allocs_;
    std::unordered_map<uintptr_t, size_t> aux_allocs_;
    std::unordered_map<uint32_t, std::wstring> thread_names_;
    std::unordered_map<uint32_t, uint64_t> thread_affinities_;
    std::unordered_map<uint32_t, uint32_t> thread_ideal_procs_;
    uint64_t guest_process_affinity_mask_ = kScarlettAllCoresMask;
};

}  // namespace

extern "C" {

int32_t OPENSCARLETT_KERNELX_CALL TitleMemoryStatus(TITLEMEMORYSTATUS* lpBuffer) {
    if (!lpBuffer) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    if (lpBuffer->dwLength != 0 && lpBuffer->dwLength < sizeof(TITLEMEMORYSTATUS)) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }

    auto& state = KernelXState::Instance();
    const uint64_t total = GetTitleBudgetBytes();
    const uint64_t used = state.CurrentTitleUsedBytes();
    const uint64_t peak = state.UpdateAndGetPeakTitleUsed();
    const uint64_t avail = (total > used) ? (total - used) : 0ull;

    lpBuffer->dwLength = static_cast<uint32_t>(sizeof(TITLEMEMORYSTATUS));
    lpBuffer->dwReserved = 0;
    lpBuffer->ullTotalMem = total;
    lpBuffer->ullAvailMem = avail;
    lpBuffer->ullLegacyUsed = used;
    lpBuffer->ullLegacyPeak = peak;
    lpBuffer->ullLegacyAvail = avail;
    lpBuffer->ullTitleUsed = used;
    lpBuffer->ullTitleAvail = avail;

    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

int32_t OPENSCARLETT_KERNELX_CALL ToolingMemoryStatus(TOOLINGMEMORYSTATUS* lpBuffer) {
    if (!lpBuffer) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    if (lpBuffer->dwLength != 0 && lpBuffer->dwLength < sizeof(TOOLINGMEMORYSTATUS)) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }

    const uint64_t total = GetToolingBudgetBytes();
    const uint64_t used = 0ull;
    lpBuffer->dwLength = static_cast<uint32_t>(sizeof(TOOLINGMEMORYSTATUS));
    lpBuffer->dwReserved = 0;
    lpBuffer->ullTotalMem = total;
    lpBuffer->ullAvailMem = total - used;
    lpBuffer->ullUsed = used;
    lpBuffer->ullPeak = used;

    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

void* OPENSCARLETT_KERNELX_CALL XMemAlloc(size_t dwSize, uint64_t dwAllocAttributes) {
    void* ptr = KernelXState::Instance().XMemAllocImpl(dwSize, dwAllocAttributes);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

void* OPENSCARLETT_KERNELX_CALL XMemAllocDefault(size_t dwSize, uint64_t dwAllocAttributes) {
    void* ptr = KernelXState::Instance().XMemAllocImpl(dwSize, dwAllocAttributes);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

void* OPENSCARLETT_KERNELX_CALL XMemAllocAligned(
    size_t dwSize, size_t alignment, uint64_t dwAllocAttributes) {
    void* ptr = KernelXState::Instance().XMemAllocImpl(dwSize, dwAllocAttributes, alignment);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrInvalidArg);
    return ptr;
}

void OPENSCARLETT_KERNELX_CALL XMemFree(void* pAddress, uint64_t dwAllocAttributes) {
    (void)dwAllocAttributes;
    const bool ok = KernelXState::Instance().XMemFreeImpl(pAddress);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
}

void OPENSCARLETT_KERNELX_CALL XMemFreeDefault(void* pAddress, uint64_t dwAllocAttributes) {
    (void)dwAllocAttributes;
    const bool ok = KernelXState::Instance().XMemFreeImpl(pAddress);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
}

size_t OPENSCARLETT_KERNELX_CALL XMemSize(void* pAddress, uint64_t dwAllocAttributes) {
    (void)dwAllocAttributes;
    const size_t sz = KernelXState::Instance().XMemSizeImpl(pAddress);
    RecordTelemetry(__func__, sz > 0 ? CallOutcome::Ok : CallOutcome::Error, sz > 0 ? kHrOk : kHrInvalidArg);
    return sz;
}

size_t OPENSCARLETT_KERNELX_CALL XMemSizeDefault(void* pAddress, uint64_t dwAllocAttributes) {
    (void)dwAllocAttributes;
    const size_t sz = KernelXState::Instance().XMemSizeImpl(pAddress);
    RecordTelemetry(__func__, sz > 0 ? CallOutcome::Ok : CallOutcome::Error, sz > 0 ? kHrOk : kHrInvalidArg);
    return sz;
}

void* OPENSCARLETT_KERNELX_CALL XMemGetAuxiliaryTitleMemory(size_t requestedSize,
                                                            size_t* outActualSize) {
    void* ptr = KernelXState::Instance().GetAuxTitleMemory(requestedSize, outActualSize);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemReleaseAuxiliaryTitleMemory(void* baseAddress) {
    const int32_t ok = KernelXState::Instance().ReleaseAuxTitleMemory(baseAddress);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

size_t OPENSCARLETT_KERNELX_CALL XMemVirtualQuery(const void* lpAddress,
                                                  void* lpBuffer,
                                                  size_t dwLength) {
    const size_t bytes = KernelXState::Instance().XMemVirtualQueryImpl(lpAddress, lpBuffer, dwLength);
    RecordTelemetry(__func__, bytes > 0u ? CallOutcome::Ok : CallOutcome::Error,
                    bytes > 0u ? kHrOk : kHrInvalidArg);
    return bytes;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemDecodeAllocationAttributes(
    uint64_t dwAllocAttributes,
    XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded) {
    if (!outDecoded) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    openscarlett::EraAllocationAttributes era{};
    if (!openscarlett::DecodeEraAllocationAttributes(dwAllocAttributes, era)) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    outDecoded->objectType = era.object_type;
    outDecoded->heapTracksAttributes = era.heap_tracks_attributes;
    outDecoded->mustSucceed = era.must_succeed;
    outDecoded->fixedSize = era.fixed_size;
    outDecoded->allocatorId = era.allocator_id;
    outDecoded->alignmentCode = era.alignment_code;
    outDecoded->memoryType = era.memory_type;
    outDecoded->pageSizeCode = era.page_size_code;
    outDecoded->zeroInitialize = era.zero_initialize;
    outDecoded->allocFlags = era.alloc_flags;
    outDecoded->protectFlags = era.protect_flags;
    outDecoded->alignmentBytes = era.alignment_bytes;
    outDecoded->pageSizeBytes = era.page_size_bytes;
    outDecoded->rawAttributes = era.raw_attributes;
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemGetAllocationAttributes(
    const void* lpAddress,
    XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded) {
    const int32_t ok = KernelXState::Instance().XMemGetAllocationAttributesImpl(lpAddress, outDecoded);
    RecordTelemetry(__func__, ok != 0 ? CallOutcome::Ok : CallOutcome::Error,
                    ok != 0 ? kHrOk : kHrInvalidArg);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL AllocateTitlePhysicalPages(void* hProcess,
                                                             uint32_t flAllocationType,
                                                             uint64_t* pNumberOfPages,
                                                             uint64_t* pPageArray) {
    (void)hProcess;
    const int32_t ok = KernelXState::Instance().AllocatePhysicalPages(
        flAllocationType, pNumberOfPages, pPageArray);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrFail);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL FreeTitlePhysicalPages(void* hProcess,
                                                         uint64_t numberOfPages,
                                                         const uint64_t* pPageArray) {
    (void)hProcess;
    const int32_t ok = KernelXState::Instance().FreePhysicalPages(numberOfPages, pPageArray);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

void* OPENSCARLETT_KERNELX_CALL MapTitleVirtualMemory(void* virtualAddress,
                                                      uint64_t numberOfPages,
                                                      uint32_t flAllocationType,
                                                      uint32_t flProtect,
                                                      const uint64_t* pPageArray) {
    void* ptr = KernelXState::Instance().MapPhysicalPages(
        virtualAddress, numberOfPages, flAllocationType, flProtect, pPageArray);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL UnmapTitleVirtualMemory(void* virtualAddress,
                                                          uint64_t numberOfPages) {
    const int32_t ok = KernelXState::Instance().UnmapPhysicalPages(virtualAddress, numberOfPages);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

void* OPENSCARLETT_KERNELX_CALL MapTitlePhysicalPages(void* virtualAddress,
                                                      uint64_t numberOfPages,
                                                      uint32_t flAllocationType,
                                                      uint32_t flProtect,
                                                      const uint64_t* pPageArray) {
    void* ptr = KernelXState::Instance().MapPhysicalPages(
        virtualAddress, numberOfPages, flAllocationType, flProtect, pPageArray);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL UnmapTitlePhysicalPages(void* virtualAddress,
                                                          uint64_t numberOfPages) {
    const int32_t ok = KernelXState::Instance().UnmapPhysicalPages(virtualAddress, numberOfPages);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

void* OPENSCARLETT_KERNELX_CALL XMemMapPhysicalPages(void* virtualAddress,
                                                     uint64_t numberOfPages,
                                                     uint32_t flAllocationType,
                                                     uint32_t flProtect,
                                                     const uint64_t* pPageArray) {
    void* ptr = KernelXState::Instance().MapPhysicalPages(
        virtualAddress, numberOfPages, flAllocationType, flProtect, pPageArray);
    RecordTelemetry(__func__, ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemUnmapPhysicalPages(void* virtualAddress,
                                                         uint64_t numberOfPages) {
    const int32_t ok = KernelXState::Instance().UnmapPhysicalPages(virtualAddress, numberOfPages);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL QueryTitlePhysicalPages(const void* virtualAddress,
                                                          uint64_t* outPhysicalId,
                                                          uint64_t* outSizeBytes,
                                                          uint32_t* outFlags) {
    const int32_t ok = KernelXState::Instance().QueryPhysicalPages(
        virtualAddress, outPhysicalId, outSizeBytes, outFlags);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

void* OPENSCARLETT_KERNELX_CALL KernelXVirtualAllocFromApp(void* baseAddress,
                                                           size_t size,
                                                           uint32_t allocationType,
                                                           uint32_t protection) {
    void* ptr = KernelXState::Instance().VirtualAllocApp(baseAddress, size, allocationType, protection);
    RecordTelemetry("VirtualAllocFromApp", ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXVirtualProtectFromApp(void* address,
                                                               size_t size,
                                                               uint32_t newProtection,
                                                               uint32_t* oldProtection) {
    const int32_t ok = KernelXState::Instance().VirtualProtectApp(
        address, size, newProtection, oldProtection);
    RecordTelemetry("VirtualProtectFromApp", ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXVirtualFree(void* address,
                                                     size_t size,
                                                     uint32_t freeType) {
    const int32_t ok = KernelXState::Instance().VirtualFreeApp(address, size, freeType);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
    return ok;
}

uint32_t OPENSCARLETT_KERNELX_CALL GetConsoleType(void) {
    const uint32_t ctype = ResolveConsoleType();
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return ctype;
}

int32_t OPENSCARLETT_KERNELX_CALL GetSystemOSVersion(KERNELX_OS_VERSION* outVersion) {
    if (!outVersion) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    outVersion->MajorVersion = 10u;
    outVersion->MinorVersion = 0u;
    outVersion->BuildNumber = 26100u;
    outVersion->Revision = 2499u;
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

uint32_t OPENSCARLETT_KERNELX_CALL XGetProcessorCount(void) {
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return kScarlettTitleProcessorCount;
}

int32_t OPENSCARLETT_KERNELX_CALL SetThreadName(void* hThread, const wchar_t* lpThreadName) {
    const int32_t hr = KernelXState::Instance().SetThreadNameImpl(hThread, lpThreadName);
    RecordTelemetry(__func__, hr >= 0 ? CallOutcome::Ok : CallOutcome::Error, hr);
    return hr;
}

int32_t OPENSCARLETT_KERNELX_CALL GetThreadName(void* hThread,
                                                wchar_t* lpThreadName,
                                                size_t nBufferLength,
                                                size_t* pnRequiredLength) {
    const int32_t hr = KernelXState::Instance().GetThreadNameImpl(
        hThread, lpThreadName, nBufferLength, pnRequiredLength);
    RecordTelemetry(__func__, hr >= 0 ? CallOutcome::Ok : CallOutcome::Error, hr);
    return hr;
}

uint64_t OPENSCARLETT_KERNELX_CALL XThreadSetAffinityMask(void* hThread,
                                                          uint64_t dwThreadAffinityMask) {
    const uint64_t prev =
        KernelXState::Instance().SetThreadAffinityMaskImpl(hThread, dwThreadAffinityMask);
    RecordTelemetry(__func__, prev != 0ull ? CallOutcome::Ok : CallOutcome::Error,
                    prev != 0ull ? kHrOk : kHrInvalidArg);
    return prev;
}

OPENSCARLETT_KERNELX_API uint64_t OPENSCARLETT_KERNELX_CALL OpenScarlettRemapXboxAffinityMask(
    uint64_t guestMask,
    uint64_t hostProcessMask) {
    const uint64_t mapped = RemapXboxAffinityMaskToHost(guestMask, hostProcessMask);
    RecordTelemetry(__func__, mapped != 0ull ? CallOutcome::Ok : CallOutcome::Error,
                    mapped != 0ull ? kHrOk : kHrInvalidArg);
    return mapped;
}

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL OpenScarlettRemapXboxIdealProcessor(
    uint32_t guestProc,
    uint32_t hostCpuCount) {
    const uint32_t mapped = RemapXboxIdealProcessorToHost(guestProc, hostCpuCount);
    RecordTelemetry(__func__, hostCpuCount != 0u ? CallOutcome::Ok : CallOutcome::Error,
                    hostCpuCount != 0u ? kHrOk : kHrInvalidArg);
    return mapped;
}

OPENSCARLETT_KERNELX_API uint64_t OPENSCARLETT_KERNELX_CALL KernelXSetThreadAffinityMask(
    void* hThread,
    uint64_t dwThreadAffinityMask) {
    const uint64_t prev =
        KernelXState::Instance().SetThreadAffinityMaskImpl(hThread, dwThreadAffinityMask);
    RecordTelemetry("SetThreadAffinityMask", prev != 0ull ? CallOutcome::Ok : CallOutcome::Error,
                    prev != 0ull ? kHrOk : kHrInvalidArg);
    return prev;
}

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL KernelXSetThreadIdealProcessor(
    void* hThread,
    uint32_t dwIdealProcessor) {
    const uint32_t prev =
        KernelXState::Instance().SetThreadIdealProcessorImpl(hThread, dwIdealProcessor);
    const bool ok = (prev != static_cast<uint32_t>(-1));
    RecordTelemetry("SetThreadIdealProcessor", ok ? CallOutcome::Ok : CallOutcome::Error,
                    ok ? kHrOk : kHrInvalidArg);
    return prev;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXSetThreadIdealProcessorEx(
    void* hThread,
    const void* lpIdealProcessor,
    void* lpPreviousIdealProcessor) {
    const int32_t ok = KernelXState::Instance().SetThreadIdealProcessorExImpl(
        hThread, lpIdealProcessor, lpPreviousIdealProcessor);
    RecordTelemetry("SetThreadIdealProcessorEx", ok != 0 ? CallOutcome::Ok : CallOutcome::Error,
                    ok != 0 ? kHrOk : kHrInvalidArg);
    return ok;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXSetThreadGroupAffinity(
    void* hThread,
    const void* groupAffinity,
    void* previousGroupAffinity) {
    const int32_t ok = KernelXState::Instance().SetThreadGroupAffinityImpl(
        hThread, groupAffinity, previousGroupAffinity);
    RecordTelemetry("SetThreadGroupAffinity", ok != 0 ? CallOutcome::Ok : CallOutcome::Error,
                    ok != 0 ? kHrOk : kHrInvalidArg);
    return ok;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXSetProcessAffinityMask(
    void* hProcess,
    uint64_t dwProcessAffinityMask) {
    const int32_t ok =
        KernelXState::Instance().SetProcessAffinityMaskImpl(hProcess, dwProcessAffinityMask);
    RecordTelemetry("SetProcessAffinityMask", ok != 0 ? CallOutcome::Ok : CallOutcome::Error,
                    ok != 0 ? kHrOk : kHrInvalidArg);
    return ok;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXGetProcessAffinityMask(
    void* hProcess,
    uint64_t* lpProcessAffinityMask,
    uint64_t* lpSystemAffinityMask) {
    const int32_t ok = KernelXState::Instance().GetProcessAffinityMaskImpl(
        hProcess, lpProcessAffinityMask, lpSystemAffinityMask);
    RecordTelemetry("GetProcessAffinityMask", ok != 0 ? CallOutcome::Ok : CallOutcome::Error,
                    ok != 0 ? kHrOk : kHrInvalidArg);
    return ok;
}

OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL KernelXGetSystemInfo(void* lpSystemInfo) {
    KernelXState::Instance().GetSystemInfoImpl(lpSystemInfo);
    RecordTelemetry("GetSystemInfo", lpSystemInfo ? CallOutcome::Ok : CallOutcome::Error,
                    lpSystemInfo ? kHrOk : kHrInvalidArg);
}

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL KernelXGetCurrentProcessorNumber(void) {
    const uint32_t proc = KernelXState::Instance().GetCurrentProcessorNumberImpl();
    RecordTelemetry("GetCurrentProcessorNumber", CallOutcome::Ok, kHrOk);
    return proc;
}

int32_t OPENSCARLETT_KERNELX_CALL XThreadSetPriority(void* hThread, int32_t nPriority) {
    const int32_t ok = KernelXState::Instance().SetThreadPriorityImpl(hThread, nPriority);
    RecordTelemetry(__func__, ok != 0 ? CallOutcome::Ok : CallOutcome::Error,
                    ok != 0 ? kHrOk : kHrHandle);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL XThreadGetPriority(void* hThread) {
    const int32_t prio = KernelXState::Instance().GetThreadPriorityImpl(hThread);
    const bool ok = (prio != KERNELX_THREAD_PRIORITY_ERROR_RETURN);
    RecordTelemetry(__func__, ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrHandle);
    return prio;
}

void* OPENSCARLETT_KERNELX_CALL KernelXCreateWaitableTimerExW(void* lpTimerAttributes,
                                                              const wchar_t* lpTimerName,
                                                              uint32_t dwFlags,
                                                              uint32_t dwDesiredAccess) {
#if defined(_WIN32)
    HANDLE h = ::CreateWaitableTimerExW(
        static_cast<LPSECURITY_ATTRIBUTES>(lpTimerAttributes),
        lpTimerName,
        static_cast<DWORD>(dwFlags),
        static_cast<DWORD>(dwDesiredAccess));
    RecordTelemetry("CreateWaitableTimerExW", h ? CallOutcome::Ok : CallOutcome::Error, h ? kHrOk : kHrFail);
    return static_cast<void*>(h);
#else
    (void)lpTimerAttributes;
    (void)lpTimerName;
    (void)dwFlags;
    (void)dwDesiredAccess;
    RecordTelemetry("CreateWaitableTimerExW", CallOutcome::NotImplemented, kHrFail);
    return nullptr;
#endif
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXQueryUnbiasedInterruptTime(uint64_t* unbiasedTime) {
    if (!unbiasedTime) {
        SetHostLastError(87u);
        RecordTelemetry("QueryUnbiasedInterruptTime", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
#if defined(_WIN32)
    ULONGLONG t = 0;
    const BOOL ok = ::QueryUnbiasedInterruptTime(&t);
    *unbiasedTime = static_cast<uint64_t>(t);
    RecordTelemetry("QueryUnbiasedInterruptTime", ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrFail);
    return ok ? 1 : 0;
#else
    *unbiasedTime = 0;
    RecordTelemetry("QueryUnbiasedInterruptTime", CallOutcome::NotImplemented, kHrFail);
    return 0;
#endif
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXCreateTilePool(uint32_t tileCount,
                                                        uint32_t flags,
                                                        uint64_t* outPoolId) {
    if (!outPoolId) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    auto& state = KernelXState::Instance();
    uint64_t pool_id = 0;
    Status st = state.TilePool().CreatePool(tileCount, flags, pool_id);
    if (!st.ok()) {
        *outPoolId = 0;
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrFail);
        return 0;
    }
    *outPoolId = pool_id;
    state.UpdateAndGetPeakTitleUsed();
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXDestroyTilePool(uint64_t poolId) {
    Status st = KernelXState::Instance().TilePool().DestroyPool(poolId);
    if (!st.ok()) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXReserveTiledResource(uint32_t widthTiles,
                                                              uint32_t heightTiles,
                                                              uint64_t* outResourceId,
                                                              void** outVirtualBase) {
    if (!outResourceId) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    auto& tp = KernelXState::Instance().TilePool();
    uint64_t res_id = 0;
    Status st = tp.ReserveTiledResource(widthTiles, heightTiles, res_id);
    if (!st.ok()) {
        *outResourceId = 0;
        if (outVirtualBase) *outVirtualBase = nullptr;
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrFail);
        return 0;
    }
    *outResourceId = res_id;
    if (outVirtualBase) {
        uint32_t w = 0;
        uint32_t h = 0;
        void* base = nullptr;
        tp.GetResourceDimensions(res_id, w, h, base);
        *outVirtualBase = base;
    }
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXReleaseTiledResource(uint64_t resourceId) {
    Status st = KernelXState::Instance().TilePool().ReleaseTiledResource(resourceId);
    if (!st.ok()) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXMapTileRange(uint64_t resourceId,
                                                      uint32_t tileX,
                                                      uint32_t tileY,
                                                      uint64_t poolId,
                                                      uint32_t poolTileOffset,
                                                      uint32_t tileCount,
                                                      uint32_t flags) {
    GpuTileCoord coord{};
    coord.x = tileX;
    coord.y = tileY;
    coord.subresource = 0;
    Status st = KernelXState::Instance().TilePool().UpdateTileMappings(
        resourceId, coord, poolId, poolTileOffset, tileCount, flags);
    if (!st.ok()) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrFail);
        return 0;
    }
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
}

// ---------------------------------------------------------------------------
// Xbox Game Core PLM Notifications (appnotify.h)
// ---------------------------------------------------------------------------
}  // extern "C"

namespace {

openscarlett::PlmNotificationRegistry& GetKernelXPlmRegistry() {
    static openscarlett::PlmNotificationRegistry reg;
    return reg;
}

}  // namespace

extern "C" {

uint32_t OPENSCARLETT_KERNELX_CALL RegisterAppStateChangeNotification(
    PAPPSTATE_CHANGE_ROUTINE Routine,
    void* Context,
    PAPPSTATE_REGISTRATION* Registration) {
    const uint32_t err = GetKernelXPlmRegistry().RegisterStateChange(Routine, Context, Registration);
    RecordTelemetry("RegisterAppStateChangeNotification",
                    err == 0u ? CallOutcome::Ok : CallOutcome::Error,
                    err == 0u ? kHrOk : kHrInvalidArg);
    return err;
}

void OPENSCARLETT_KERNELX_CALL UnregisterAppStateChangeNotification(
    PAPPSTATE_REGISTRATION Registration) {
    (void)GetKernelXPlmRegistry().UnregisterStateChange(Registration);
    RecordTelemetry("UnregisterAppStateChangeNotification", CallOutcome::Ok, kHrOk);
}

uint32_t OPENSCARLETT_KERNELX_CALL RegisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_CHANGE_ROUTINE Routine,
    void* Context,
    PAPPCONSTRAIN_REGISTRATION* Registration) {
    const uint32_t err =
        GetKernelXPlmRegistry().RegisterConstrainedChange(Routine, Context, Registration);
    RecordTelemetry("RegisterAppConstrainedChangeNotification",
                    err == 0u ? CallOutcome::Ok : CallOutcome::Error,
                    err == 0u ? kHrOk : kHrInvalidArg);
    return err;
}

void OPENSCARLETT_KERNELX_CALL UnregisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_REGISTRATION Registration) {
    (void)GetKernelXPlmRegistry().UnregisterConstrainedChange(Registration);
    RecordTelemetry("UnregisterAppConstrainedChangeNotification", CallOutcome::Ok, kHrOk);
}

uint32_t OPENSCARLETT_KERNELX_CALL KernelXDispatchAppStateChange(uint8_t Quiesced) {
    return GetKernelXPlmRegistry().DispatchStateChange(Quiesced);
}

uint32_t OPENSCARLETT_KERNELX_CALL KernelXDispatchAppConstrainedChange(uint8_t Constrained) {
    return GetKernelXPlmRegistry().DispatchConstrainedChange(Constrained);
}

void OPENSCARLETT_KERNELX_CALL KernelXGetAppNotificationCounts(
    uint32_t* outStateCount,
    uint32_t* outConstrainedCount) {
    GetKernelXPlmRegistry().GetCounts(outStateCount, outConstrainedCount);
}

void OPENSCARLETT_KERNELX_CALL KernelXResetTelemetry(void) {
    CallTelemetry::Global().Reset();
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXGetTelemetryCallCount(const char* symbolName,
                                                               uint64_t* outTotalCalls,
                                                               uint64_t* outOkCalls,
                                                               uint64_t* outErrorCalls) {
    if (!symbolName) {
        return 0;
    }
    if (outTotalCalls) *outTotalCalls = 0;
    if (outOkCalls) *outOkCalls = 0;
    if (outErrorCalls) *outErrorCalls = 0;

    const auto snap = CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kModuleName && entry.symbol_name == symbolName) {
            if (outTotalCalls) *outTotalCalls = entry.total_calls;
            if (outOkCalls) *outOkCalls = entry.ok_calls;
            if (outErrorCalls) *outErrorCalls = entry.error_calls;
            return 1;
        }
    }
    return 0;
}

int32_t OPENSCARLETT_KERNELX_CALL KernelXWriteTelemetryJson(const char* filePath) {
    if (!filePath || filePath[0] == '\0') {
        return 0;
    }
    return CallTelemetry::Global().WriteJsonReport(filePath).ok() ? 1 : 0;
}

}  // extern "C"

// ===========================================================================
// Clean-Room User-Mode Scheduling (UMS) Cooperative / Fiber-Backed Shims
// (Compatible with Win10 & Win11 where Microsoft removed kernel UMS support.)
// ===========================================================================
namespace {

constexpr uint32_t kOsUmsVersion = 0x0100u;
constexpr uint32_t kOsUmsSchedulerStartup = 0u;
constexpr uint32_t kOsUmsSchedulerThreadYield = 2u;

constexpr uint32_t kOsUmsInfoUserContext = 1u;
constexpr uint32_t kOsUmsInfoPriority = 2u;
constexpr uint32_t kOsUmsInfoAffinity = 3u;
constexpr uint32_t kOsUmsInfoTeb = 4u;
constexpr uint32_t kOsUmsInfoIsSuspended = 5u;
constexpr uint32_t kOsUmsInfoIsTerminated = 6u;

using PfnOsUmsSchedulerEntry = void(OPENSCARLETT_KERNELX_CALL*)(
    uint32_t Reason,
    uintptr_t ActivationPayload,
    void* SchedulerParam);

using PfnOsUmsWorkerEntry = void(OPENSCARLETT_KERNELX_CALL*)(void* Param);

struct OsUmsSchedulerStartupInfo {
    uint32_t UmsVersion;
    void* CompletionList;
    PfnOsUmsSchedulerEntry SchedulerProc;
    void* SchedulerParam;
};

struct UmsThreadContextObj;

struct UmsCompletionListObj {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<UmsThreadContextObj*> items;
#if defined(_WIN32)
    HANDLE event = nullptr;
#endif
};

struct UmsThreadContextObj {
    void* user_context = nullptr;
    int32_t priority = 0;
    uintptr_t affinity = 1u;
    uint64_t synthetic_teb[16]{};
    void* teb = nullptr;
    bool is_suspended = false;
    bool is_terminated = false;
    bool running = false;
    bool has_yielded = false;
    void* yield_param = nullptr;
    UmsThreadContextObj* next_list_item = nullptr;
    PfnOsUmsWorkerEntry worker_entry = nullptr;
    void* worker_param = nullptr;
#if defined(_WIN32)
    void* worker_fiber = nullptr;
    void* scheduler_fiber = nullptr;
#endif
};

struct UmsSchedulerTls {
    bool in_scheduler = false;
    UmsCompletionListObj* completion_list = nullptr;
    PfnOsUmsSchedulerEntry scheduler_proc = nullptr;
    void* scheduler_param = nullptr;
    void* scheduler_fiber = nullptr;
    UmsThreadContextObj* current_worker = nullptr;
};

thread_local UmsSchedulerTls g_ums_tls;

struct UmsRuntime {
    std::mutex mutex;
    std::unordered_map<void*, std::unique_ptr<UmsCompletionListObj>> lists;
    std::unordered_map<void*, std::unique_ptr<UmsThreadContextObj>> contexts;

    bool IsValidList(void* p) {
        std::lock_guard<std::mutex> lk(mutex);
        return lists.find(p) != lists.end();
    }

    bool IsValidContext(void* p) {
        std::lock_guard<std::mutex> lk(mutex);
        return contexts.find(p) != contexts.end();
    }
};

UmsRuntime& GetUmsRuntime() {
    static UmsRuntime* rt = new UmsRuntime();
    return *rt;
}

void EnqueueUmsListItem(UmsCompletionListObj* list, UmsThreadContextObj* ctx) {
    if (!list || !ctx) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(list->mutex);
        list->items.push_back(ctx);
#if defined(_WIN32)
        if (list->event) {
            ::SetEvent(list->event);
        }
#endif
    }
    list->cv.notify_all();
}

#if defined(_WIN32)
void WINAPI UmsWorkerFiberTrampoline(PVOID param) {
    auto* ctx = static_cast<UmsThreadContextObj*>(param);
    if (ctx && ctx->worker_entry) {
        ctx->worker_entry(ctx->worker_param);
    }
    if (ctx) {
        ctx->is_terminated = true;
        ctx->has_yielded = false;
        if (ctx->scheduler_fiber) {
            ::SwitchToFiber(ctx->scheduler_fiber);
        }
    }
}
#endif

}  // namespace

extern "C" {

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL CreateUmsCompletionList(
    void** UmsCompletionList) {
    if (!UmsCompletionList) {
        SetHostLastError(87u);
        RecordTelemetry("CreateUmsCompletionList", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    *UmsCompletionList = nullptr;
    auto list = std::make_unique<UmsCompletionListObj>();
#if defined(_WIN32)
    list->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!list->event) {
        SetHostLastError(8u);
        RecordTelemetry("CreateUmsCompletionList", CallOutcome::Error, kHrFail);
        return 0;
    }
#endif
    void* raw = list.get();
    {
        auto& rt = GetUmsRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        rt.lists.emplace(raw, std::move(list));
    }
    *UmsCompletionList = raw;
    SetHostLastError(0u);
    RecordTelemetry("CreateUmsCompletionList", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL DeleteUmsCompletionList(
    void* UmsCompletionList) {
    if (!UmsCompletionList) {
        SetHostLastError(87u);
        RecordTelemetry("DeleteUmsCompletionList", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    std::unique_ptr<UmsCompletionListObj> owned;
    {
        auto& rt = GetUmsRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        auto it = rt.lists.find(UmsCompletionList);
        if (it == rt.lists.end()) {
            SetHostLastError(87u);
            RecordTelemetry("DeleteUmsCompletionList", CallOutcome::Error, kHrInvalidArg);
            return 0;
        }
        {
            std::lock_guard<std::mutex> llk(it->second->mutex);
            if (!it->second->items.empty()) {
                SetHostLastError(87u);
                RecordTelemetry("DeleteUmsCompletionList", CallOutcome::Error, kHrInvalidArg);
                return 0;
            }
        }
        owned = std::move(it->second);
        rt.lists.erase(it);
    }
#if defined(_WIN32)
    if (owned && owned->event) {
        ::CloseHandle(owned->event);
        owned->event = nullptr;
    }
#endif
    SetHostLastError(0u);
    RecordTelemetry("DeleteUmsCompletionList", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL GetUmsCompletionListEvent(
    void* UmsCompletionList,
    void** UmsCompletionEvent) {
    if (!UmsCompletionList || !UmsCompletionEvent) {
        SetHostLastError(87u);
        RecordTelemetry("GetUmsCompletionListEvent", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    *UmsCompletionEvent = nullptr;
    auto& rt = GetUmsRuntime();
    std::lock_guard<std::mutex> lk(rt.mutex);
    auto it = rt.lists.find(UmsCompletionList);
    if (it == rt.lists.end()) {
        SetHostLastError(87u);
        RecordTelemetry("GetUmsCompletionListEvent", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
#if defined(_WIN32)
    *UmsCompletionEvent = static_cast<void*>(it->second->event);
#else
    *UmsCompletionEvent = it->second.get();
#endif
    SetHostLastError(0u);
    RecordTelemetry("GetUmsCompletionListEvent", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL DequeueUmsCompletionListItems(
    void* UmsCompletionList,
    uint32_t WaitTimeOut,
    void** UmsThreadList) {
    if (!UmsCompletionList || !UmsThreadList) {
        SetHostLastError(87u);
        RecordTelemetry("DequeueUmsCompletionListItems", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    *UmsThreadList = nullptr;
    UmsCompletionListObj* list = nullptr;
    {
        auto& rt = GetUmsRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        auto it = rt.lists.find(UmsCompletionList);
        if (it == rt.lists.end()) {
            SetHostLastError(87u);
            RecordTelemetry("DequeueUmsCompletionListItems", CallOutcome::Error, kHrInvalidArg);
            return 0;
        }
        list = it->second.get();
    }

    std::vector<UmsThreadContextObj*> drained;
    {
        std::unique_lock<std::mutex> llk(list->mutex);
        if (list->items.empty() && WaitTimeOut != 0u) {
            if (WaitTimeOut == 0xFFFFFFFFu) {
                list->cv.wait(llk, [&]() { return !list->items.empty(); });
            } else {
                (void)list->cv.wait_for(
                    llk, std::chrono::milliseconds(WaitTimeOut),
                    [&]() { return !list->items.empty(); });
            }
        }
        if (list->items.empty()) {
            if (WaitTimeOut == 0u) {
                SetHostLastError(0u);
                RecordTelemetry("DequeueUmsCompletionListItems", CallOutcome::Ok, kHrOk);
                return 1;
            }
            SetHostLastError(1460u);  // ERROR_TIMEOUT
            RecordTelemetry("DequeueUmsCompletionListItems", CallOutcome::Error, kHrFail);
            return 0;
        }
        while (!list->items.empty()) {
            drained.push_back(list->items.front());
            list->items.pop_front();
        }
#if defined(_WIN32)
        if (list->event) {
            ::ResetEvent(list->event);
        }
#endif
    }

    for (size_t i = 0; i < drained.size(); ++i) {
        drained[i]->next_list_item = (i + 1u < drained.size()) ? drained[i + 1u] : nullptr;
    }
    *UmsThreadList = drained.front();
    SetHostLastError(0u);
    RecordTelemetry("DequeueUmsCompletionListItems", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL GetNextUmsListItem(void* UmsContext) {
    if (!UmsContext || !GetUmsRuntime().IsValidContext(UmsContext)) {
        SetHostLastError(87u);
        RecordTelemetry("GetNextUmsListItem", CallOutcome::Error, kHrInvalidArg);
        return nullptr;
    }
    auto* ctx = static_cast<UmsThreadContextObj*>(UmsContext);
    SetHostLastError(0u);
    RecordTelemetry("GetNextUmsListItem", CallOutcome::Ok, kHrOk);
    return ctx->next_list_item;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL CreateUmsThreadContext(
    void** lpUmsThread) {
    if (!lpUmsThread) {
        SetHostLastError(87u);
        RecordTelemetry("CreateUmsThreadContext", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    *lpUmsThread = nullptr;
    auto ctx = std::make_unique<UmsThreadContextObj>();
    ctx->teb = ctx->synthetic_teb;
    void* raw = ctx.get();
    {
        auto& rt = GetUmsRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        rt.contexts.emplace(raw, std::move(ctx));
    }
    *lpUmsThread = raw;
    SetHostLastError(0u);
    RecordTelemetry("CreateUmsThreadContext", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL DeleteUmsThreadContext(
    void* UmsThread) {
    if (!UmsThread) {
        SetHostLastError(87u);
        RecordTelemetry("DeleteUmsThreadContext", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    std::unique_ptr<UmsThreadContextObj> owned;
    {
        auto& rt = GetUmsRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        auto it = rt.contexts.find(UmsThread);
        if (it == rt.contexts.end() || it->second->running) {
            SetHostLastError(87u);
            RecordTelemetry("DeleteUmsThreadContext", CallOutcome::Error, kHrInvalidArg);
            return 0;
        }
        owned = std::move(it->second);
        rt.contexts.erase(it);
    }
#if defined(_WIN32)
    if (owned && owned->worker_fiber) {
        ::DeleteFiber(owned->worker_fiber);
        owned->worker_fiber = nullptr;
    }
#endif
    SetHostLastError(0u);
    RecordTelemetry("DeleteUmsThreadContext", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL QueryUmsThreadInformation(
    void* UmsThread,
    uint32_t UmsThreadInfoClass,
    void* UmsThreadInformation,
    uint32_t UmsThreadInformationLength,
    uint32_t* ReturnLength) {
    if (ReturnLength) {
        *ReturnLength = 0u;
    }
    if (!UmsThread || !GetUmsRuntime().IsValidContext(UmsThread)) {
        SetHostLastError(87u);
        RecordTelemetry("QueryUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    auto* ctx = static_cast<UmsThreadContextObj*>(UmsThread);

    auto write_val = [&](const void* src, uint32_t needed) -> int32_t {
        if (ReturnLength) {
            *ReturnLength = needed;
        }
        if (!UmsThreadInformation || UmsThreadInformationLength < needed) {
            SetHostLastError(122u);  // ERROR_INSUFFICIENT_BUFFER
            RecordTelemetry("QueryUmsThreadInformation", CallOutcome::Error, kHrInsufficientBuffer);
            return 0;
        }
        std::memcpy(UmsThreadInformation, src, needed);
        SetHostLastError(0u);
        RecordTelemetry("QueryUmsThreadInformation", CallOutcome::Ok, kHrOk);
        return 1;
    };

    switch (UmsThreadInfoClass) {
        case kOsUmsInfoUserContext:
            return write_val(&ctx->user_context, static_cast<uint32_t>(sizeof(void*)));
        case kOsUmsInfoPriority:
            return write_val(&ctx->priority, static_cast<uint32_t>(sizeof(int32_t)));
        case kOsUmsInfoAffinity:
            return write_val(&ctx->affinity, static_cast<uint32_t>(sizeof(uintptr_t)));
        case kOsUmsInfoTeb:
            return write_val(&ctx->teb, static_cast<uint32_t>(sizeof(void*)));
        case kOsUmsInfoIsSuspended: {
            if (UmsThreadInformationLength == 1u && UmsThreadInformation) {
                const uint8_t b = ctx->is_suspended ? 1u : 0u;
                return write_val(&b, 1u);
            }
            const uint32_t v = ctx->is_suspended ? 1u : 0u;
            return write_val(&v, static_cast<uint32_t>(sizeof(uint32_t)));
        }
        case kOsUmsInfoIsTerminated: {
            if (UmsThreadInformationLength == 1u && UmsThreadInformation) {
                const uint8_t b = ctx->is_terminated ? 1u : 0u;
                return write_val(&b, 1u);
            }
            const uint32_t v = ctx->is_terminated ? 1u : 0u;
            return write_val(&v, static_cast<uint32_t>(sizeof(uint32_t)));
        }
        default:
            SetHostLastError(87u);
            RecordTelemetry("QueryUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
            return 0;
    }
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL SetUmsThreadInformation(
    void* UmsThread,
    uint32_t UmsThreadInfoClass,
    void* UmsThreadInformation,
    uint32_t UmsThreadInformationLength) {
    if (!UmsThread || !UmsThreadInformation || !GetUmsRuntime().IsValidContext(UmsThread)) {
        SetHostLastError(87u);
        RecordTelemetry("SetUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    auto* ctx = static_cast<UmsThreadContextObj*>(UmsThread);
    switch (UmsThreadInfoClass) {
        case kOsUmsInfoUserContext:
            if (UmsThreadInformationLength != sizeof(void*)) {
                SetHostLastError(87u);
                RecordTelemetry("SetUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
                return 0;
            }
            std::memcpy(&ctx->user_context, UmsThreadInformation, sizeof(void*));
            break;
        case kOsUmsInfoPriority:
            if (UmsThreadInformationLength != sizeof(int32_t)) {
                SetHostLastError(87u);
                RecordTelemetry("SetUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
                return 0;
            }
            std::memcpy(&ctx->priority, UmsThreadInformation, sizeof(int32_t));
            break;
        case kOsUmsInfoAffinity:
            if (UmsThreadInformationLength != sizeof(uintptr_t)) {
                SetHostLastError(87u);
                RecordTelemetry("SetUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
                return 0;
            }
            std::memcpy(&ctx->affinity, UmsThreadInformation, sizeof(uintptr_t));
            break;
        default:
            SetHostLastError(87u);
            RecordTelemetry("SetUmsThreadInformation", CallOutcome::Error, kHrInvalidArg);
            return 0;
    }
    SetHostLastError(0u);
    RecordTelemetry("SetUmsThreadInformation", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL GetCurrentUmsThread(void) {
    RecordTelemetry("GetCurrentUmsThread", CallOutcome::Ok, kHrOk);
    return g_ums_tls.current_worker;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL EnterUmsSchedulingMode(
    const void* SchedulerStartupInfo) {
    if (!SchedulerStartupInfo || g_ums_tls.in_scheduler) {
        SetHostLastError(87u);
        RecordTelemetry("EnterUmsSchedulingMode", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    const auto* info = static_cast<const OsUmsSchedulerStartupInfo*>(SchedulerStartupInfo);
    if (info->UmsVersion != kOsUmsVersion ||
        !info->CompletionList ||
        !info->SchedulerProc ||
        !GetUmsRuntime().IsValidList(info->CompletionList)) {
        SetHostLastError(87u);
        RecordTelemetry("EnterUmsSchedulingMode", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }

#if defined(_WIN32)
    const bool was_fiber = (::IsThreadAFiber() != FALSE);
    void* sched_fiber = was_fiber ? ::GetCurrentFiber() : ::ConvertThreadToFiber(nullptr);
    if (!sched_fiber) {
        SetHostLastError(8u);
        RecordTelemetry("EnterUmsSchedulingMode", CallOutcome::Error, kHrFail);
        return 0;
    }
#else
    void* sched_fiber = nullptr;
#endif

    g_ums_tls.in_scheduler = true;
    g_ums_tls.completion_list = static_cast<UmsCompletionListObj*>(info->CompletionList);
    g_ums_tls.scheduler_proc = info->SchedulerProc;
    g_ums_tls.scheduler_param = info->SchedulerParam;
    g_ums_tls.scheduler_fiber = sched_fiber;
    g_ums_tls.current_worker = nullptr;

    RecordTelemetry("EnterUmsSchedulingMode", CallOutcome::Ok, kHrOk);
    info->SchedulerProc(kOsUmsSchedulerStartup, 0u, info->SchedulerParam);

    g_ums_tls.in_scheduler = false;
    g_ums_tls.completion_list = nullptr;
    g_ums_tls.scheduler_proc = nullptr;
    g_ums_tls.scheduler_param = nullptr;
    g_ums_tls.scheduler_fiber = nullptr;
    g_ums_tls.current_worker = nullptr;

#if defined(_WIN32)
    if (!was_fiber) {
        (void)::ConvertFiberToThread();
    }
#endif
    SetHostLastError(0u);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL ExecuteUmsThread(void* UmsThread) {
    if (!UmsThread || !g_ums_tls.in_scheduler || !GetUmsRuntime().IsValidContext(UmsThread)) {
        SetHostLastError(87u);
        RecordTelemetry("ExecuteUmsThread", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    auto* ctx = static_cast<UmsThreadContextObj*>(UmsThread);
    if (ctx->is_terminated || ctx->running) {
        SetHostLastError(87u);
        RecordTelemetry("ExecuteUmsThread", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }

    RecordTelemetry("ExecuteUmsThread", CallOutcome::Ok, kHrOk);

    if (!ctx->worker_entry) {
        ctx->is_terminated = true;
        EnqueueUmsListItem(g_ums_tls.completion_list, ctx);
        SetHostLastError(0u);
        return 1;
    }

#if defined(_WIN32)
    if (!ctx->worker_fiber) {
        ctx->worker_fiber = ::CreateFiber(0u, &UmsWorkerFiberTrampoline, ctx);
        if (!ctx->worker_fiber) {
            SetHostLastError(8u);
            return 0;
        }
    }
    ctx->scheduler_fiber = g_ums_tls.scheduler_fiber;
    ctx->running = true;
    ctx->has_yielded = false;
    ctx->yield_param = nullptr;
    UmsThreadContextObj* prev_worker = g_ums_tls.current_worker;
    g_ums_tls.current_worker = ctx;

    ::SwitchToFiber(ctx->worker_fiber);

    g_ums_tls.current_worker = prev_worker;
    ctx->running = false;

    if (ctx->has_yielded) {
        EnqueueUmsListItem(g_ums_tls.completion_list, ctx);
        if (g_ums_tls.scheduler_proc) {
            g_ums_tls.scheduler_proc(
                kOsUmsSchedulerThreadYield,
                reinterpret_cast<uintptr_t>(ctx),
                ctx->yield_param);
        }
    } else if (ctx->is_terminated) {
        if (ctx->worker_fiber) {
            ::DeleteFiber(ctx->worker_fiber);
            ctx->worker_fiber = nullptr;
        }
        EnqueueUmsListItem(g_ums_tls.completion_list, ctx);
    }
#else
    ctx->running = true;
    UmsThreadContextObj* prev_worker = g_ums_tls.current_worker;
    g_ums_tls.current_worker = ctx;
    ctx->worker_entry(ctx->worker_param);
    g_ums_tls.current_worker = prev_worker;
    ctx->running = false;
    ctx->is_terminated = true;
    EnqueueUmsListItem(g_ums_tls.completion_list, ctx);
#endif
    SetHostLastError(0u);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL UmsThreadYield(void* SchedulerParam) {
    UmsThreadContextObj* ctx = g_ums_tls.current_worker;
    if (!ctx) {
        SetHostLastError(1u);  // ERROR_INVALID_FUNCTION
        RecordTelemetry("UmsThreadYield", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    RecordTelemetry("UmsThreadYield", CallOutcome::Ok, kHrOk);
#if defined(_WIN32)
    if (!ctx->scheduler_fiber) {
        SetHostLastError(1u);
        return 0;
    }
    ctx->has_yielded = true;
    ctx->yield_param = SchedulerParam;
    ::SwitchToFiber(ctx->scheduler_fiber);
    SetHostLastError(0u);
    return 1;
#else
    (void)SchedulerParam;
    SetHostLastError(0u);
    return 1;
#endif
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL OpenScarlettUmsBindWorkerProc(
    void* UmsThread,
    void* EntryProc,
    void* EntryParam) {
    if (!UmsThread || !EntryProc || !GetUmsRuntime().IsValidContext(UmsThread)) {
        SetHostLastError(87u);
        return 0;
    }
    auto* ctx = static_cast<UmsThreadContextObj*>(UmsThread);
    if (ctx->running || ctx->is_terminated) {
        SetHostLastError(87u);
        return 0;
    }
    ctx->worker_entry = reinterpret_cast<PfnOsUmsWorkerEntry>(EntryProc);
    ctx->worker_param = EntryParam;
    SetHostLastError(0u);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL OpenScarlettUmsEnqueueCompletionItem(
    void* UmsCompletionList,
    void* UmsThread) {
    if (!UmsCompletionList || !UmsThread ||
        !GetUmsRuntime().IsValidList(UmsCompletionList) ||
        !GetUmsRuntime().IsValidContext(UmsThread)) {
        SetHostLastError(87u);
        return 0;
    }
    EnqueueUmsListItem(
        static_cast<UmsCompletionListObj*>(UmsCompletionList),
        static_cast<UmsThreadContextObj*>(UmsThread));
    SetHostLastError(0u);
    return 1;
}

}  // extern "C"
