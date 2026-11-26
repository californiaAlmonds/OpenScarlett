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
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
#pragma comment(linker, "/export:VirtualFree=KernelXVirtualFree")
#pragma comment(linker, "/export:VirtualFreeEx=KernelXVirtualFreeEx")
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
#pragma comment(linker, "/export:GlobalMemoryStatusEx=KernelXGlobalMemoryStatusEx")
#pragma comment(linker, "/export:QueryInformationJobObject=KernelXQueryInformationJobObject")
#pragma comment(linker, "/export:JobMemoryStatus=KernelXJobMemoryStatus")
#pragma comment(linker, "/export:GetProcessInformation=KernelXGetProcessInformation")
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

// Tracks active 64 KiB physical view-swap windows around UnmapPhysicalAlias -> MapPhysicalAlias
// (UnmapViewOfFile2 -> MapViewOfFile3) so concurrent worker threads hitting a mid-remap page
// fault can spin/synchronize in a first-chance VEH and retry cleanly once MEM_COMMIT is restored.
// Adapted with MIT attribution from R0Y00/Xbox2Windows (src/xmem_mapped_pages.cpp:433-495).
struct RemapWindowRange {
    uintptr_t base = 0u;
    size_t size = 0u;
};

struct RemapWindowTracker {
#if defined(_WIN32)
    SRWLOCK lock = SRWLOCK_INIT;
#else
    mutable std::mutex lock;
#endif
    std::vector<RemapWindowRange> ranges;
    std::atomic<uint64_t> generation{0ull};  // Odd while a view-swap window is active, even when finished.
};

RemapWindowTracker& GetRemapWindowTracker() noexcept {
    static RemapWindowTracker tracker;
    return tracker;
}

thread_local bool g_in_remap_window_thread = false;

class ScopedRemapWindow {
public:
    explicit ScopedRemapWindow(std::vector<RemapWindowRange>&& new_ranges) noexcept {
        if (new_ranges.empty()) {
            return;
        }
        active_ = true;
        g_in_remap_window_thread = true;
        auto& t = GetRemapWindowTracker();
#if defined(_WIN32)
        ::AcquireSRWLockExclusive(&t.lock);
        t.ranges = std::move(new_ranges);
        const uint64_t prev = t.generation.load(std::memory_order_relaxed);
        t.generation.store((prev & ~1ull) + 1ull, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&t.lock);
#else
        std::lock_guard<std::mutex> lk(t.lock);
        t.ranges = std::move(new_ranges);
        const uint64_t prev = t.generation.load(std::memory_order_relaxed);
        t.generation.store((prev & ~1ull) + 1ull, std::memory_order_release);
#endif
    }

    ~ScopedRemapWindow() noexcept {
        if (!active_) {
            return;
        }
        auto& t = GetRemapWindowTracker();
#if defined(_WIN32)
        ::AcquireSRWLockExclusive(&t.lock);
        const uint64_t cur = t.generation.load(std::memory_order_relaxed);
        t.generation.store(cur + 1ull, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&t.lock);
#else
        {
            std::lock_guard<std::mutex> lk(t.lock);
            const uint64_t cur = t.generation.load(std::memory_order_relaxed);
            t.generation.store(cur + 1ull, std::memory_order_release);
        }
#endif
        g_in_remap_window_thread = false;
    }

    ScopedRemapWindow(const ScopedRemapWindow&) = delete;
    ScopedRemapWindow& operator=(const ScopedRemapWindow&) = delete;

private:
    bool active_ = false;
};

void AppendCoalescedRemapRange(std::vector<RemapWindowRange>& ranges,
                               uintptr_t base,
                               size_t size) {
    if (size == 0u) {
        return;
    }
    if (!ranges.empty() && ranges.back().base + ranges.back().size == base) {
        ranges.back().size += size;
    } else {
        ranges.push_back(RemapWindowRange{base, size});
    }
}

void NotifyActiveD3D12VirtualRemapRanges(const std::vector<RemapWindowRange>& ranges) noexcept {
    if (ranges.empty()) {
        return;
    }
#if defined(_WIN32)
    using PfnInvalidateActiveRange = void(__stdcall*)(uint64_t, uint64_t);
    static std::atomic<PfnInvalidateActiveRange> s_fn{nullptr};
    PfnInvalidateActiveRange fn = s_fn.load(std::memory_order_acquire);
    if (!fn) {
        if (HMODULE hd3d = ::GetModuleHandleW(L"d3d12_xs.dll")) {
            fn = reinterpret_cast<PfnInvalidateActiveRange>(
                ::GetProcAddress(hd3d, "OpenScarlettD3D12InvalidateActiveVirtualRange"));
            if (fn) {
                s_fn.store(fn, std::memory_order_release);
            }
        }
    }
    if (fn) {
        for (const auto& r : ranges) {
            if (r.base != 0u && r.size != 0u) {
                fn(static_cast<uint64_t>(r.base), static_cast<uint64_t>(r.size));
            }
        }
    }
#endif
}

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
    if (mem_type <= XALLOC_MEMTYPE_PHYSICAL) {
        return true;
    }
    // Retail Xbox GDK system/middleware allocators (dwAllocatorId >= 0x80, e.g.
    // 0xF0814000 for Chad/XAudio2 physical memory) pack memory-protect + zero-init +
    // physical flags into bits 28..31.
    const uint32_t alloc_id = static_cast<uint32_t>((attrs >> 16) & 0xFFu);
    return alloc_id >= 0x80u;
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

#if defined(_WIN32)
struct ScopedLastErrorPreserve {
    DWORD saved = ::GetLastError();
    ~ScopedLastErrorPreserve() { ::SetLastError(saved); }
};

std::string ReadEnvLowerTrimmed(const char* name) {
    char env_buf[128] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len == 0u || len >= sizeof(env_buf)) {
        return {};
    }
    size_t b = 0u;
    size_t e = static_cast<size_t>(len);
    while (b < e && std::isspace(static_cast<unsigned char>(env_buf[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(env_buf[e - 1u]))) --e;
    std::string v(env_buf + b, e - b);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

bool TryParseConsoleTypeString(const std::string& v, uint32_t& out_type) noexcept {
    if (v.empty()) {
        return false;
    }
    if (v == "lockhart" || v == "series_s" || v == "series-s" || v == "seriess" ||
        v == "3" || v == "s" || v == "8gb" || v == "8gib") {
        out_type = CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART;
        return true;
    }
    if (v == "anaconda" || v == "series_x" || v == "series-x" || v == "seriesx" ||
        v == "4" || v == "x" || v == "13.5gb" || v == "16gb") {
        out_type = CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
        return true;
    }
    if (v == "devkit" || v == "5") {
        out_type = CONSOLE_TYPE_XBOX_SCARLETT_DEVKIT;
        return true;
    }
    if (v == "auto") {
        constexpr uint64_t kLockhartHostRamThresholdBytes = 18ull * 1024ull * 1024ull * 1024ull;
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (::GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0ull &&
            ms.ullTotalPhys <= kLockhartHostRamThresholdBytes) {
            out_type = CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART;
        } else {
            out_type = CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
        }
        return true;
    }
    return false;
}
#endif

uint32_t ResolveConsoleType() {
#if defined(_WIN32)
    ScopedLastErrorPreserve preserve_err;
    uint32_t parsed = CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
    if (TryParseConsoleTypeString(ReadEnvLowerTrimmed("OPENSCARLETT_CONSOLE_TYPE"), parsed)) {
        return parsed;
    }
    if (TryParseConsoleTypeString(ReadEnvLowerTrimmed("OPENSCARLETT_CONSOLE_PROFILE"), parsed)) {
        return parsed;
    }
#endif
    return CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
}

uint64_t GetTitleBudgetBytes() {
#if defined(_WIN32)
    ScopedLastErrorPreserve preserve_err;
    const std::string mb_str = ReadEnvLowerTrimmed("OPENSCARLETT_TITLE_MEM_MB");
    if (!mb_str.empty()) {
        char* end_ptr = nullptr;
        const unsigned long long mb = std::strtoull(mb_str.c_str(), &end_ptr, 10);
        if (end_ptr != mb_str.c_str() && mb >= 64ull && mb <= 262144ull) {
            return static_cast<uint64_t>(mb) * 1024ull * 1024ull;
        }
    }
#endif
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
bool IsPathOnCDriveKx(const std::wstring& path) noexcept {
    if (path.size() >= 2u && path[1] == L':') {
        return path[0] == L'C' || path[0] == L'c';
    }
    if (path.size() >= 6u &&
        (path.compare(0u, 4u, L"\\\\?\\") == 0 || path.compare(0u, 4u, L"\\??\\") == 0) &&
        path[5] == L':') {
        return path[4] == L'C' || path[4] == L'c';
    }
    return false;
}

std::wstring ResolveKernelXSparseTempDir() {
    wchar_t env_buf[MAX_PATH * 2]{};
    const DWORD env_len = ::GetEnvironmentVariableW(
        L"OPENSCARLETT_TMP_DIR", env_buf, static_cast<DWORD>(sizeof(env_buf) / sizeof(wchar_t)));
    if (env_len > 0u && env_len < (sizeof(env_buf) / sizeof(wchar_t))) {
        std::wstring custom(env_buf, env_len);
        if (!IsPathOnCDriveKx(custom)) {
            ::CreateDirectoryW(custom.c_str(), nullptr);
            return custom;
        }
    }

    const UINT e_type = ::GetDriveTypeW(L"E:\\");
    if (e_type != DRIVE_NO_ROOT_DIR && e_type != DRIVE_UNKNOWN) {
        ::CreateDirectoryW(L"E:\\OpenScarlett", nullptr);
        const std::wstring e_tmp = L"E:\\OpenScarlett\\.tmp";
        if (::CreateDirectoryW(e_tmp.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS) {
            return e_tmp;
        }
    }

    HMODULE self_mod = nullptr;
    wchar_t dir_buf[MAX_PATH * 2]{};
    if (::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ResolveConsoleType),
            &self_mod) &&
        self_mod) {
        const DWORD n = ::GetModuleFileNameW(
            self_mod, dir_buf, static_cast<DWORD>(sizeof(dir_buf) / sizeof(wchar_t)));
        if (n > 0u && n < (sizeof(dir_buf) / sizeof(wchar_t))) {
            std::wstring p(dir_buf, n);
            const size_t sl = p.find_last_of(L"\\/");
            if (sl != std::wstring::npos) {
                std::wstring candidate = p.substr(0u, sl) + L"\\.tmp";
                if (!IsPathOnCDriveKx(candidate)) {
                    ::CreateDirectoryW(candidate.c_str(), nullptr);
                    return candidate;
                }
            }
        }
    }
    return {};
}

bool ShouldPreferSparseCommitKx(size_t byte_size, bool prefer_write_watch_first) noexcept {
    char force_buf[16]{};
    const DWORD flen = ::GetEnvironmentVariableA(
        "OPENSCARLETT_FORCE_SPARSE_COMMIT", force_buf, static_cast<DWORD>(sizeof(force_buf)));
    if (flen > 0u && force_buf[0] != '0') {
        if (force_buf[0] == 'l' || force_buf[0] == 'L') {
            if (byte_size >= (4ull * 1024ull * 1024ull)) {
                return true;
            }
        } else {
            return true;
        }
    }
    const size_t min_size = prefer_write_watch_first ? (128ull * 1024ull * 1024ull)
                                                     : (32ull * 1024ull * 1024ull);
    if (byte_size >= min_size) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        const uint64_t headroom = prefer_write_watch_first
                                      ? (1536ull * 1024ull * 1024ull)
                                      : (2048ull * 1024ull * 1024ull);
        if (::GlobalMemoryStatusEx(&ms) &&
            ms.ullAvailPageFile < static_cast<DWORDLONG>(byte_size) + headroom) {
            return true;
        }
    }
    return false;
}

HANDLE CreateSparseFileSectionKx(size_t byte_size, const wchar_t* prefix) noexcept {
    const std::wstring backing_dir = ResolveKernelXSparseTempDir();
    if (backing_dir.empty() || byte_size == 0u) {
        return nullptr;
    }
    static std::atomic<uint64_t> s_sparse_seq{1};
    const uint64_t seq = s_sparse_seq.fetch_add(1u, std::memory_order_relaxed);
    wchar_t file_path[MAX_PATH * 2]{};
    std::swprintf(file_path,
                  sizeof(file_path) / sizeof(wchar_t),
                  L"%ls\\%ls_%lu_%llu.bin",
                  backing_dir.c_str(),
                  prefix ? prefix : L"xmem",
                  static_cast<unsigned long>(::GetCurrentProcessId()),
                  static_cast<unsigned long long>(seq));
    HANDLE hf = ::CreateFileW(
        file_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    constexpr DWORD kFsctlSetSparse = 0x000900C4u;
    DWORD bytes_ret = 0;
    (void)::DeviceIoControl(hf, kFsctlSetSparse, nullptr, 0, nullptr, 0, &bytes_ret, nullptr);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(byte_size);
    HANDLE hmap = nullptr;
    if (::SetFilePointerEx(hf, li, nullptr, FILE_BEGIN) && ::SetEndOfFile(hf)) {
        const DWORD hi = static_cast<DWORD>((static_cast<uint64_t>(byte_size) >> 32) & 0xFFFFFFFFull);
        const DWORD lo = static_cast<DWORD>(static_cast<uint64_t>(byte_size) & 0xFFFFFFFFull);
        hmap = ::CreateFileMappingW(hf, nullptr, PAGE_READWRITE, hi, lo, nullptr);
    }
    ::CloseHandle(hf);
    return hmap;
}

struct KxMemAddressRequirements {
    void* lowest_starting_address;
    void* highest_ending_address;
    SIZE_T alignment;
};

struct alignas(8) KxMemExtendedParameter {
    uint64_t type_and_reserved;  // Low 8 bits: 1 == MemExtendedParameterAddressRequirements
    union {
        uint64_t ulong64;
        void* pointer;
        SIZE_T size;
        HANDLE handle;
        DWORD ulong;
    } value;
};

using PfnKxVirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, PVOID, ULONG);
using PfnKxMapViewOfFile3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, PVOID, ULONG);

PfnKxVirtualAlloc2 GetKxVirtualAlloc2() noexcept {
    static PfnKxVirtualAlloc2 s_fn = []() -> PfnKxVirtualAlloc2 {
        HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
        return kb ? reinterpret_cast<PfnKxVirtualAlloc2>(::GetProcAddress(kb, "VirtualAlloc2")) : nullptr;
    }();
    return s_fn;
}

PfnKxMapViewOfFile3 GetKxMapViewOfFile3() noexcept {
    static PfnKxMapViewOfFile3 s_fn = []() -> PfnKxMapViewOfFile3 {
        HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
        return kb ? reinterpret_cast<PfnKxMapViewOfFile3>(::GetProcAddress(kb, "MapViewOfFile3")) : nullptr;
    }();
    return s_fn;
}

// Allocates VirtualAlloc / VirtualAlloc2 memory, trying < 2^40 (1 TiB) placement first so
// retail Scarlett titles using 40-bit tagged pointers (bzhi ..., 40) or 40-bit GPU/BVH VAs
// never truncate high ASLR bits, and trying MEM_WRITE_WATCH whenever MEM_RESERVE is requested.
void* AllocVirtualWriteWatched(void* base_addr,
                               size_t byte_size,
                               size_t alignment,
                               DWORD win_alloc,
                               DWORD win_prot,
                               bool require_below_40bit = false) noexcept {
    constexpr uintptr_t kScarlettLowLimit40 = 1ull << 40;
    const bool try_write_watch = ((win_alloc & MEM_RESERVE) != 0u) &&
                                 ((win_alloc & MEM_RESET) == 0u);
    void* ptr = nullptr;
    const PfnKxVirtualAlloc2 va2 = GetKxVirtualAlloc2();
    if (base_addr == nullptr && va2 != nullptr) {
        if (byte_size <= kScarlettLowLimit40) {
            KxMemAddressRequirements addr_req{
                nullptr,
                reinterpret_cast<void*>(kScarlettLowLimit40 - 1ull),
                (alignment > kPage64KB) ? alignment : 0u};
            KxMemExtendedParameter ext{};
            ext.type_and_reserved = 1ull;
            ext.value.pointer = &addr_req;
            if (try_write_watch) {
                ptr = va2(::GetCurrentProcess(),
                          nullptr,
                          byte_size,
                          win_alloc | MEM_WRITE_WATCH,
                          win_prot,
                          &ext,
                          1u);
            }
            if (!ptr) {
                ptr = va2(::GetCurrentProcess(), nullptr, byte_size, win_alloc, win_prot, &ext, 1u);
            }
        }
        if (!ptr && !require_below_40bit && alignment > kPage64KB) {
            KxMemAddressRequirements addr_req{nullptr, nullptr, alignment};
            KxMemExtendedParameter ext{};
            ext.type_and_reserved = 1ull;
            ext.value.pointer = &addr_req;
            if (try_write_watch) {
                ptr = va2(::GetCurrentProcess(),
                          nullptr,
                          byte_size,
                          win_alloc | MEM_WRITE_WATCH,
                          win_prot,
                          &ext,
                          1u);
            }
            if (!ptr) {
                ptr = va2(::GetCurrentProcess(), nullptr, byte_size, win_alloc, win_prot, &ext, 1u);
            }
        }
    }
    if (!ptr && !require_below_40bit) {
        if (try_write_watch) {
            ptr = ::VirtualAlloc(base_addr, byte_size, win_alloc | MEM_WRITE_WATCH, win_prot);
        }
        if (!ptr) {
            ptr = ::VirtualAlloc(base_addr, byte_size, win_alloc, win_prot);
        }
    }
    if (ptr && require_below_40bit) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
        const bool aligned_ok = (alignment == 0u) || ((base % alignment) == 0u);
        const bool below_ok = (byte_size <= kScarlettLowLimit40) &&
                              (base <= (kScarlettLowLimit40 - byte_size));
        if (!aligned_ok || !below_ok) {
            (void)::VirtualFree(ptr, 0u, MEM_RELEASE);
            ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    }
    if (ptr && try_write_watch && (win_alloc & MEM_COMMIT) != 0u) {
        (void)::ResetWriteWatch(ptr, byte_size);
    }
    return ptr;
}

void* MapSparseViewAlignedKx(size_t byte_size,
                             size_t alignment,
                             DWORD win_prot,
                             const wchar_t* prefix,
                             bool require_below_40bit = false) noexcept {
    constexpr uintptr_t kScarlettLowLimit40 = 1ull << 40;
    constexpr ULONG kMemReservePlaceholder = 0x00040000u;
    constexpr ULONG kMemReplacePlaceholder = 0x00004000u;
    const size_t eff_gran = (alignment > kPage64KB) ? alignment : kPage64KB;
    const size_t map_size = AlignUp(byte_size, eff_gran);
    if (require_below_40bit && map_size > kScarlettLowLimit40) {
        return nullptr;
    }
    HANDLE hmap = CreateSparseFileSectionKx(map_size, prefix);
    if (!hmap) {
        return nullptr;
    }
    void* mapped = nullptr;
    const PfnKxVirtualAlloc2 va2 = GetKxVirtualAlloc2();
    const PfnKxMapViewOfFile3 map3 = GetKxMapViewOfFile3();
    if (va2 != nullptr && map3 != nullptr) {
        const DWORD map_prot = (win_prot == PAGE_READONLY) ? PAGE_READONLY : PAGE_READWRITE;
        auto try_placeholder_map = [&](void* highest_end, size_t req_align) noexcept -> void* {
            KxMemAddressRequirements addr_req{nullptr, highest_end, req_align};
            KxMemExtendedParameter ext{};
            ext.type_and_reserved = 1ull;
            ext.value.pointer = &addr_req;
            void* ph = va2(::GetCurrentProcess(),
                           nullptr,
                           map_size,
                           MEM_RESERVE | kMemReservePlaceholder,
                           PAGE_NOACCESS,
                           &ext,
                           1u);
            if (!ph) {
                return nullptr;
            }
            void* view = map3(
                hmap, ::GetCurrentProcess(), ph, 0ull, map_size,
                kMemReplacePlaceholder, map_prot, nullptr, 0u);
            if (!view) {
                (void)::VirtualFree(ph, 0u, MEM_RELEASE);
            }
            return view;
        };
        if (map_size <= kScarlettLowLimit40) {
            mapped = try_placeholder_map(
                reinterpret_cast<void*>(kScarlettLowLimit40 - 1ull),
                (alignment > kPage64KB) ? alignment : 0u);
        }
        if (!mapped && !require_below_40bit && alignment > kPage64KB) {
            mapped = try_placeholder_map(nullptr, alignment);
        }
    }
    if (!mapped && !require_below_40bit) {
        const DWORD access =
            (win_prot == PAGE_READONLY) ? FILE_MAP_READ : (FILE_MAP_READ | FILE_MAP_WRITE);
        mapped = ::MapViewOfFile(hmap, access, 0u, 0u, map_size);
    }
    ::CloseHandle(hmap);
    if (mapped && require_below_40bit) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mapped);
        const bool aligned_ok = (alignment == 0u) || ((base % alignment) == 0u);
        const bool below_ok = base <= (kScarlettLowLimit40 - map_size);
        if (!aligned_ok || !below_ok) {
            (void)::UnmapViewOfFile(mapped);
            return nullptr;
        }
    }
    if (mapped && win_prot == PAGE_NOACCESS) {
        DWORD old_p = 0;
        (void)::VirtualProtect(mapped, map_size, PAGE_NOACCESS, &old_p);
    }
    return mapped;
}
#endif

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
    std::vector<uintptr_t> gpu_tile_vas;
};

struct ActiveTitleMapping {
    uintptr_t base_va = 0;
    size_t size_bytes = 0;
    size_t page_size_bytes = kPage64KB;
    uint64_t primary_physical_id = 0;
    uint32_t alloc_flags = 0;
    uint32_t win32_protect = PAGE_READWRITE;
    bool owns_placeholder = false;
    bool is_gpu_tile_view = false;
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
    bool is_file_mapping = false;
};

enum class XMemBackingKind : uint8_t {
    AlignedHeap = 0,
    VirtualAllocMem = 1,
    FileMappingView = 2,
};

struct XMemAllocRecord {
    size_t requested_size = 0;
    size_t allocated_size = 0;
    size_t alignment = 16u;
    uint64_t attributes = 0;
    XMemBackingKind backing = XMemBackingKind::AlignedHeap;
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
        if (pad_granule > 1u && dwSize > (SIZE_MAX - (pad_granule - 1u))) {
            SetHostLastError(8u);
            return nullptr;
        }
        size_t alloc_size = AlignUp(dwSize, pad_granule);
        if (alloc_size > budget) {
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return nullptr;
        }

        const uint32_t mem_type = static_cast<uint32_t>((dwAllocAttributes >> 28) & 0xFu);
        const bool is_graphics_or_phys = (mem_type != XALLOC_MEMTYPE_HEAP);
        const bool zero_init = ((dwAllocAttributes & XALLOC_FLAG_ZERO_INIT) != 0ull) || is_graphics_or_phys;
        XMemBackingKind backing = XMemBackingKind::AlignedHeap;
        void* ptr = nullptr;
        bool already_zeroed = false;

#if defined(_WIN32)
        const bool prefer_virtual =
            is_graphics_or_phys || alloc_size >= kPage64KB || alignment >= kPage4KB;
        if (prefer_virtual) {
            alloc_size = AlignUp(alloc_size, kPage4KB);
            if (!ShouldPreferSparseCommitKx(alloc_size, /*prefer_write_watch_first=*/true)) {
                ptr = AllocVirtualWriteWatched(
                    nullptr, alloc_size, alignment, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (ptr) {
                    backing = XMemBackingKind::VirtualAllocMem;
                    already_zeroed = true;
                }
            }
            if (!ptr) {
                alloc_size = AlignUp(alloc_size, kPage64KB);
                ptr = MapSparseViewAlignedKx(alloc_size, alignment, PAGE_READWRITE, L"xmemdef");
                if (ptr) {
                    backing = XMemBackingKind::FileMappingView;
                    already_zeroed = true;
                } else {
                    ptr = AllocVirtualWriteWatched(
                        nullptr, alloc_size, alignment, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                    if (ptr) {
                        backing = XMemBackingKind::VirtualAllocMem;
                        already_zeroed = true;
                    }
                }
            }
        } else {
            ptr = _aligned_malloc(alloc_size, alignment);
            if (!ptr) {
                alloc_size = AlignUp(alloc_size, kPage4KB);
                ptr = AllocVirtualWriteWatched(
                    nullptr, alloc_size, alignment, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (ptr) {
                    backing = XMemBackingKind::VirtualAllocMem;
                    already_zeroed = true;
                } else {
                    alloc_size = AlignUp(alloc_size, kPage64KB);
                    ptr = MapSparseViewAlignedKx(alloc_size, alignment, PAGE_READWRITE, L"xmemdef");
                    if (ptr) {
                        backing = XMemBackingKind::FileMappingView;
                        already_zeroed = true;
                    }
                }
            }
        }
#else
        if (posix_memalign(&ptr, std::max<size_t>(alignment, sizeof(void*)), alloc_size) != 0) {
            ptr = nullptr;
        }
#endif
        if (!ptr) {
            SetHostLastError(8u);
            return nullptr;
        }

        if (zero_init && !already_zeroed) {
            std::memset(ptr, 0, alloc_size);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            XMemAllocRecord rec{};
            rec.requested_size = dwSize;
            rec.allocated_size = alloc_size;
            rec.alignment = alignment;
            rec.attributes = dwAllocAttributes;
            rec.backing = backing;
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
        XMemBackingKind backing = XMemBackingKind::AlignedHeap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = xmem_allocs_.find(reinterpret_cast<uintptr_t>(pAddress));
            if (it == xmem_allocs_.end()) {
                SetHostLastError(87u);
                return false;
            }
            freed_bytes = it->second.allocated_size;
            backing = it->second.backing;
            xmem_allocs_.erase(it);
        }
#if defined(_WIN32)
        if (backing == XMemBackingKind::VirtualAllocMem) {
            ::VirtualFree(pAddress, 0u, MEM_RELEASE);
        } else if (backing == XMemBackingKind::FileMappingView) {
            ::UnmapViewOfFile(pAddress);
        } else {
            _aligned_free(pAddress);
        }
#else
        (void)backing;
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
        if (raw_size > (SIZE_MAX - (kPage64KB - 1u))) {
            if (outActualSize) {
                *outActualSize = 0;
            }
            SetHostLastError(8u);
            return nullptr;
        }
        const size_t aligned_size = AlignUp(raw_size, kPage64KB);
        const uint64_t budget = GetTitleBudgetBytes();
        if (aligned_size > budget) {
            if (outActualSize) {
                *outActualSize = 0;
            }
            SetHostLastError(8u);
            return nullptr;
        }

        bool is_file_mapped = false;
#if defined(_WIN32)
        void* ptr = nullptr;
        if (!ShouldPreferSparseCommitKx(aligned_size, /*prefer_write_watch_first=*/true)) {
            ptr = AllocVirtualWriteWatched(
                nullptr, aligned_size, kPage64KB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        if (!ptr) {
            ptr = MapSparseViewAlignedKx(aligned_size, kPage64KB, PAGE_READWRITE, L"xaux");
            if (ptr) {
                is_file_mapped = true;
            } else {
                ptr = AllocVirtualWriteWatched(
                    nullptr, aligned_size, kPage64KB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            }
        }
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
            if (is_file_mapped) {
                aux_file_mapped_.insert(reinterpret_cast<uintptr_t>(ptr));
            }
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
        bool was_file_mapped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uintptr_t key = reinterpret_cast<uintptr_t>(baseAddress);
            auto it = aux_allocs_.find(key);
            if (it == aux_allocs_.end()) {
                SetHostLastError(87u);
                return 0;
            }
            freed_size = it->second;
            aux_allocs_.erase(it);
            auto fit = aux_file_mapped_.find(key);
            if (fit != aux_file_mapped_.end()) {
                was_file_mapped = true;
                aux_file_mapped_.erase(fit);
            }
        }
#if defined(_WIN32)
        if (was_file_mapped) {
            if (!::UnmapViewOfFile(baseAddress)) {
                return 0;
            }
        } else if (!VirtualFree(baseAddress, 0, MEM_RELEASE)) {
            return 0;
        }
#else
        (void)was_file_mapped;
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

        // On Xbox Series X, MEM_PHYSICAL (0x00400000) | MEM_LARGE_PAGES (0x20000000) uses 64 KiB
        // physical pages unless MEM_4MB_PAGES (0x80000000) is explicitly requested.
        constexpr uint32_t kMemPhysical = 0x00400000u;
        const bool is_physical = (flAllocationType & kMemPhysical) != 0u;
        const bool large_pages =
            ((flAllocationType & MEM_4MB_PAGES) != 0u) ||
            (!is_physical && (flAllocationType & MEM_LARGE_PAGES) != 0u);
        const size_t page_bytes = large_pages ? kPage4MB : kPage64KB;
        const MemoryPageSize page_enum = large_pages ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
        const uint64_t req_pages = *pNumberOfPages;
        const uint64_t budget = GetTitleBudgetBytes();

        if (req_pages > (budget / page_bytes)) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return 0;
        }

        const uint64_t total_bytes_64 = req_pages * static_cast<uint64_t>(page_bytes);
        if (total_bytes_64 > budget) {
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

        bool pfn_exhausted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            PhysicalPoolRecord pool{};
            pool.physical_id = phys.physical_id;
            pool.page_size_bytes = page_bytes;
            pool.alloc_flags = flAllocationType;
            pool.live_pages = req_pages;
            pools_.emplace(phys.physical_id, pool);

            for (uint64_t i = 0; i < req_pages; ++i) {
                const uint64_t pfn = AllocateNextPfnLocked();
                if (pfn == 0ull) {
                    for (uint64_t j = 0; j < i; ++j) {
                        pfn_table_.erase(pPageArray[j]);
                        free_pfns_.insert(pPageArray[j]);
                        pPageArray[j] = 0ull;
                    }
                    pools_.erase(phys.physical_id);
                    pfn_exhausted = true;
                    break;
                }
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
        if (pfn_exhausted) {
            (void)memory_.FreePhysicalPages(phys.physical_id);
            *pNumberOfPages = 0;
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return 0;
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
        std::vector<RemapWindowRange> retired_tile_ranges;
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

            // Unmap any lingering GPU-only Slot31 tile views referencing these CPU-unmapped PFNs
            // so physical section handles release cleanly without leaking PFNs or host commit.
            std::vector<uintptr_t> stale_tile_vas;
            for (uint64_t i = 0; i < numberOfPages; ++i) {
                auto it = pfn_table_.find(pPageArray[i]);
                if (it != pfn_table_.end() && !it->second.gpu_tile_vas.empty()) {
                    stale_tile_vas.insert(
                        stale_tile_vas.end(),
                        it->second.gpu_tile_vas.begin(),
                        it->second.gpu_tile_vas.end());
                    it->second.gpu_tile_vas.clear();
                }
            }
            if (!stale_tile_vas.empty()) {
                std::sort(stale_tile_vas.begin(), stale_tile_vas.end());
                stale_tile_vas.erase(
                    std::unique(stale_tile_vas.begin(), stale_tile_vas.end()),
                    stale_tile_vas.end());
                for (uintptr_t tva : stale_tile_vas) {
                    AppendCoalescedRemapRange(retired_tile_ranges, tva, kPage64KB);
                }
                auto tile_ranges_copy = retired_tile_ranges;
                ScopedRemapWindow tile_unmap_guard(std::move(tile_ranges_copy));
                for (uintptr_t tva : stale_tile_vas) {
                    auto mit = mappings_.find(tva);
                    if (mit != mappings_.end() && mit->second.is_gpu_tile_view &&
                        !mit->second.owns_placeholder) {
                        for (void* ap : mit->second.alias_ptrs) {
                            (void)memory_.UnmapPhysicalAlias(ap);
                        }
                        mappings_.erase(mit);
                    }
                }
            }

            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uint64_t pfn = pPageArray[i];
                auto it = pfn_table_.find(pfn);
                const uint64_t phys_id = it->second.physical_id;
                pfn_table_.erase(it);
                free_pfns_.insert(pfn);

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

        if (!retired_tile_ranges.empty()) {
            NotifyActiveD3D12VirtualRemapRanges(retired_tile_ranges);
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

    void detach_mapping_from_pfn_locked(const ActiveTitleMapping& m, uintptr_t slot_va) {
        for (uint64_t pfn : m.pfns) {
            auto pit = pfn_table_.find(pfn);
            if (pit == pfn_table_.end()) {
                continue;
            }
            if (m.is_gpu_tile_view) {
                auto& gvas = pit->second.gpu_tile_vas;
                gvas.erase(std::remove(gvas.begin(), gvas.end(), slot_va), gvas.end());
            } else if (pit->second.map_refcount > 0) {
                --pit->second.map_refcount;
            }
        }
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

        std::unique_lock<std::mutex> lock(mutex_);
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
            if (!valid_placeholder) {
                void* ph_base = nullptr;
                size_t ph_sz = 0u, ph_gr = 0u;
                bool ph_mapped = false;
                if (memory_.LookupPlaceholderByHostPtr(virtualAddress, ph_base, ph_sz, ph_gr, ph_mapped) &&
                    ph_base != nullptr) {
                    valid_placeholder = true;
                }
            }
#if defined(_WIN32)
            if (!valid_placeholder) {
                uintptr_t cur = req;
                const uintptr_t req_end = req + total_bytes;
                bool all_valid_target = true;
                while (cur < req_end) {
                    UnifiedMemoryAlias ex_alias{};
                    void* ph_b = nullptr;
                    size_t ph_s = 0u, ph_g = 0u;
                    bool ph_m = false;
                    if (mappings_.find(cur) != mappings_.end() ||
                        memory_.LookupAliasByHostPtr(reinterpret_cast<const void*>(cur), ex_alias) ||
                        memory_.LookupPlaceholderByHostPtr(reinterpret_cast<const void*>(cur), ph_b, ph_s, ph_g, ph_m)) {
                        cur += page_bytes;
                        continue;
                    }
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0u ||
                        mbi.State != MEM_RESERVE || mbi.Type != MEM_PRIVATE || mbi.RegionSize == 0u) {
                        all_valid_target = false;
                        break;
                    }
                    const uintptr_t seg_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    if (seg_end <= cur) {
                        all_valid_target = false;
                        break;
                    }
                    cur = seg_end;
                }
                if (all_valid_target) {
                    valid_placeholder = true;
                }
            }
#endif
            if (!valid_placeholder || !UnifiedMemoryManager::SupportsOsPlaceholders()) {
                SetHostLastError(487u);  // ERROR_INVALID_ADDRESS
                return nullptr;
            }

            {
                void* ph_base = nullptr;
                size_t ph_sz = 0u, ph_gr = 0u;
                bool ph_mapped = false;
                if (!memory_.LookupPlaceholderByHostPtr(virtualAddress, ph_base, ph_sz, ph_gr, ph_mapped)) {
                    (void)memory_.PromoteReservedRangeToPlaceholder(virtualAddress, total_bytes);
                }
            }

            auto unmap_single_slot_locked = [&](uintptr_t slot_va) {
                auto ex_it = mappings_.find(slot_va);
                if (ex_it != mappings_.end() && ex_it->second.size_bytes == page_bytes &&
                    !ex_it->second.owns_placeholder) {
                    for (void* ap : ex_it->second.alias_ptrs) {
                        (void)memory_.UnmapPhysicalAlias(ap);
                    }
                    detach_mapping_from_pfn_locked(ex_it->second, slot_va);
                    mappings_.erase(ex_it);
                }
            };

            auto map_slot_with_retry = [&](uint64_t phys_id,
                                           size_t phys_off,
                                           size_t bytes,
                                           void* slot,
                                           UnifiedMemoryAlias& out_alias) -> Status {
                Status st = memory_.MapPhysicalAlias(
                    phys_id, phys_off, bytes, prot, mem_flags, slot, out_alias);
#if defined(_WIN32)
                if (!st.ok()) {
                    (void)memory_.PromoteReservedRangeToPlaceholder(slot, bytes);
                    st = memory_.MapPhysicalAlias(
                        phys_id, phys_off, bytes, prot, mem_flags, slot, out_alias);
                }
                if (!st.ok()) {
                    void* sp_base = nullptr;
                    size_t sp_size = 0u;
                    size_t sp_granule = 0u;
                    bool sp_mapped = false;
                    if (memory_.LookupPlaceholderByHostPtr(slot, sp_base, sp_size, sp_granule, sp_mapped) &&
                        sp_base != nullptr && !sp_mapped && sp_size > 0u) {
                        (void)sp_granule;
                        constexpr DWORD kMemCoalescePlaceholders = 0x00000001u;
                        (void)VirtualFree(sp_base, sp_size, MEM_RELEASE | kMemCoalescePlaceholders);
                        st = memory_.MapPhysicalAlias(
                            phys_id, phys_off, bytes, prot, mem_flags, slot, out_alias);
                    }
                }
#endif
                return st;
            };

            // Map placeholder targets at individual page granularity (64 KiB / 4 MiB) so subsequent
            // XMemMapPhysicalPagesScatter or partial XMemUnmapPhysicalPages calls can atomically
            // remap or unmap any subset of pages without disturbing adjacent mapped pages.
            std::vector<RemapWindowRange> remap_ranges;
            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uintptr_t slot_va = req + static_cast<size_t>(i) * page_bytes;
                if (mappings_.find(slot_va) != mappings_.end()) {
                    AppendCoalescedRemapRange(remap_ranges, slot_va, page_bytes);
                }
            }
            std::vector<RemapWindowRange> notify_ranges = remap_ranges;
            ScopedRemapWindow remap_guard(std::move(remap_ranges));

            base_va = virtualAddress;
            std::vector<uintptr_t> newly_mapped_slots;
            newly_mapped_slots.reserve(static_cast<size_t>(numberOfPages));
            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uintptr_t slot_va = req + static_cast<size_t>(i) * page_bytes;
                void* slot = reinterpret_cast<void*>(slot_va);
                unmap_single_slot_locked(slot_va);

                UnifiedMemoryAlias alias{};
                Status st = map_slot_with_retry(
                    frames[i].physical_id,
                    static_cast<size_t>(frames[i].page_index) * page_bytes,
                    page_bytes,
                    slot,
                    alias);
                if (!st.ok()) {
                    for (uintptr_t prev_va : newly_mapped_slots) {
                        unmap_single_slot_locked(prev_va);
                    }
                    SetHostLastError(487u);
                    return nullptr;
                }

                ActiveTitleMapping page_map{};
                page_map.base_va = slot_va;
                page_map.size_bytes = page_bytes;
                page_map.page_size_bytes = page_bytes;
                page_map.primary_physical_id = frames[i].physical_id;
                page_map.alloc_flags = flAllocationType | frames[i].alloc_flags;
                page_map.win32_protect = flProtect;
                page_map.owns_placeholder = false;
                page_map.pfns.push_back(pPageArray[i]);
                page_map.alias_ptrs.push_back(alias.host_ptr);
                ++pfn_table_[pPageArray[i]].map_refcount;
                mappings_[slot_va] = std::move(page_map);
                newly_mapped_slots.push_back(slot_va);
            }
            lock.unlock();
            NotifyActiveD3D12VirtualRemapRanges(notify_ranges);
            return base_va;
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

        std::vector<RemapWindowRange> notify_ranges;
        std::unique_lock<std::mutex> lock(mutex_);
        const uintptr_t key = reinterpret_cast<uintptr_t>(virtualAddress);
        auto it = mappings_.find(key);
        if (it != mappings_.end() &&
            (numberOfPages == 0u || numberOfPages == it->second.pfns.size())) {
            const size_t unmapped_bytes = it->second.size_bytes;
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
            detach_mapping_from_pfn_locked(it->second, key);
            mappings_.erase(it);
            AppendCoalescedRemapRange(notify_ranges, key, unmapped_bytes);
            lock.unlock();
            NotifyActiveD3D12VirtualRemapRanges(notify_ranges);
            return 1;
        }

        if (numberOfPages > 0u) {
            size_t page_bytes = 0u;
            if (it != mappings_.end() && it->second.page_size_bytes > 0u) {
                page_bytes = it->second.page_size_bytes;
            } else {
                void* ph_base = nullptr;
                size_t ph_sz = 0u, ph_gr = 0u;
                bool ph_mapped = false;
                if (memory_.LookupPlaceholderByHostPtr(virtualAddress, ph_base, ph_sz, ph_gr, ph_mapped) &&
                    ph_gr >= kPage64KB) {
                    page_bytes = ph_gr;
                } else {
                    page_bytes = kPage64KB;
                }
            }
            bool unmapped_any = false;
            for (uint64_t p = 0u; p < numberOfPages; ++p) {
                const uintptr_t slot_va = key + static_cast<size_t>(p) * page_bytes;
                auto sit = mappings_.find(slot_va);
                if (sit == mappings_.end() || sit->second.owns_placeholder) continue;
                for (void* alias_ptr : sit->second.alias_ptrs) {
                    (void)memory_.UnmapPhysicalAlias(alias_ptr);
                }
                detach_mapping_from_pfn_locked(sit->second, slot_va);
                mappings_.erase(sit);
                AppendCoalescedRemapRange(notify_ranges, slot_va, page_bytes);
                unmapped_any = true;
            }
            if (unmapped_any) {
                lock.unlock();
                NotifyActiveD3D12VirtualRemapRanges(notify_ranges);
                return 1;
            }
            void* ph_base = nullptr;
            size_t ph_sz = 0u, ph_gr = 0u;
            bool ph_mapped = false;
            if (memory_.LookupPlaceholderByHostPtr(virtualAddress, ph_base, ph_sz, ph_gr, ph_mapped) &&
                ph_base != nullptr) {
                return 1;
            }
            for (const auto& [b, rec] : app_allocs_) {
                if (rec.is_placeholder && key >= b && key < b + rec.size) {
                    return 1;
                }
            }
#if defined(_WIN32)
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(virtualAddress, &mbi, sizeof(mbi)) != 0u &&
                mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE) {
                return 1;
            }
#endif
        }

        SetHostLastError(87u);
        return 0;
    }

    int32_t MapPhysicalPagesScatter(void* const* addresses,
                                    uint64_t count,
                                    const uint64_t* tokens) {
        if (!addresses || count == 0u || !UnifiedMemoryManager::SupportsOsPlaceholders()) {
            SetHostLastError(87u);
            return 0;
        }
        if (count > (GetTitleBudgetBytes() / kPage64KB) * 4ull) {
            SetHostLastError(87u);
            return 0;
        }

        constexpr uint64_t kUnmapSlotToken = UINT64_MAX;

        // Deduplicate by 64 KiB page address (last write in batch wins, matching Scarlett &
        // MIT-licensed R0Y00/Xbox2Windows local_apply_locked semantics).
        // UINT64_MAX (or tokens == nullptr) explicitly unmaps the slot to a MEM_RESERVE placeholder.
        std::map<uintptr_t, uint64_t> desired;
        for (uint64_t i = 0u; i < count; ++i) {
            const uintptr_t va = reinterpret_cast<uintptr_t>(addresses[i]);
            if (va < 0x10000ull || (va & (kPage64KB - 1u)) != 0u) {
                SetHostLastError(87u);
                return 0;
            }
            const uint64_t tok = (tokens != nullptr) ? tokens[i] : kUnmapSlotToken;
            desired[va] = tok;
        }

        std::vector<RemapWindowRange> notify_ranges;
        std::unique_lock<std::mutex> lock(mutex_);

        struct ValidatedScatterOp {
            uintptr_t slot_va = 0u;
            bool is_unmap = false;
            uint64_t pfn = 0ull;
            PageFrameRecord frame{};
        };
        std::vector<ValidatedScatterOp> ops;
        ops.reserve(desired.size());
        std::vector<RemapWindowRange> remap_ranges;

        // Validate all non-unmap tokens up front BEFORE unmapping any existing view so an
        // invalid token never partially mutates mappings or leaves an unmapped hole.
        // Note: Valid PFNs start at 1 (AllocateNextPfnLocked); both 0 and UINT64_MAX are
        // explicit Scarlett unmap sentinels used by RAGE9 scatter unmap batches (+0xF2C4D1).
        for (const auto& [slot_va, tok] : desired) {
            auto ex_it = mappings_.find(slot_va);
            if (ex_it != mappings_.end() && ex_it->second.size_bytes != kPage64KB) {
                SetHostLastError(87u);
                return 0;
            }
            if (tok == 0ull || tok == kUnmapSlotToken) {
                if (ex_it != mappings_.end()) {
                    AppendCoalescedRemapRange(notify_ranges, slot_va, kPage64KB);
                }
                ops.push_back(ValidatedScatterOp{slot_va, true, 0ull, {}});
                continue;
            }
            auto pit = pfn_table_.find(tok);
            if (pit == pfn_table_.end() || pit->second.page_size_bytes != kPage64KB) {
                SetHostLastError(87u);
                return 0;
            }
            if (ex_it != mappings_.end() &&
                !ex_it->second.is_gpu_tile_view &&
                ex_it->second.pfns.size() == 1u &&
                ex_it->second.pfns[0] == tok) {
                continue;  // Already mapped to the exact requested PFN.
            }
            if (ex_it != mappings_.end()) {
                AppendCoalescedRemapRange(remap_ranges, slot_va, kPage64KB);
                AppendCoalescedRemapRange(notify_ranges, slot_va, kPage64KB);
            }
            ops.push_back(ValidatedScatterOp{slot_va, false, tok, pit->second});
        }

        auto unmap_slot_locked = [&](uintptr_t slot_va, bool* out_prev_owns_ph = nullptr) -> bool {
            if (out_prev_owns_ph) {
                *out_prev_owns_ph = false;
            }
            auto ex_it = mappings_.find(slot_va);
            if (ex_it == mappings_.end() || ex_it->second.size_bytes != kPage64KB) {
                return false;
            }
            if (out_prev_owns_ph) {
                *out_prev_owns_ph = ex_it->second.owns_placeholder;
            }
            for (void* ap : ex_it->second.alias_ptrs) {
                (void)memory_.UnmapPhysicalAlias(ap);
            }
            detach_mapping_from_pfn_locked(ex_it->second, slot_va);
            mappings_.erase(ex_it);
            return true;
        };

        auto map_slot_locked = [&](uintptr_t slot_va,
                                   const PageFrameRecord& frame,
                                   uint64_t pfn,
                                   bool owns_placeholder) -> bool {
            void* slot = reinterpret_cast<void*>(slot_va);
            const MemoryProtection prot = MemoryProtection::ReadWrite;
            const uint32_t mem_flags = ToUnifiedFlags(MEM_GRAPHICS | frame.alloc_flags, PAGE_READWRITE);
            const size_t phys_off = static_cast<size_t>(frame.page_index) * kPage64KB;
            UnifiedMemoryAlias out_alias{};
            Status st = memory_.MapPhysicalAlias(
                frame.physical_id, phys_off, kPage64KB, prot, mem_flags, slot, out_alias);
#if defined(_WIN32)
            if (!st.ok()) {
                (void)memory_.PromoteReservedRangeToPlaceholder(slot, kPage64KB);
                st = memory_.MapPhysicalAlias(
                    frame.physical_id, phys_off, kPage64KB, prot, mem_flags, slot, out_alias);
            }
            if (!st.ok()) {
                void* sp_base = nullptr;
                size_t sp_size = 0u;
                size_t sp_granule = 0u;
                bool sp_mapped = false;
                if (memory_.LookupPlaceholderByHostPtr(slot, sp_base, sp_size, sp_granule, sp_mapped) &&
                    sp_base != nullptr && !sp_mapped && sp_size > 0u) {
                    (void)sp_granule;
                    constexpr DWORD kMemCoalescePlaceholders = 0x00000001u;
                    (void)VirtualFree(sp_base, sp_size, MEM_RELEASE | kMemCoalescePlaceholders);
                    st = memory_.MapPhysicalAlias(
                        frame.physical_id, phys_off, kPage64KB, prot, mem_flags, slot, out_alias);
                }
            }
#endif
            if (!st.ok()) {
                return false;
            }
            ActiveTitleMapping page_map{};
            page_map.base_va = slot_va;
            page_map.size_bytes = kPage64KB;
            page_map.page_size_bytes = kPage64KB;
            page_map.primary_physical_id = frame.physical_id;
            page_map.alloc_flags = MEM_GRAPHICS | frame.alloc_flags;
            page_map.win32_protect = PAGE_READWRITE;
            page_map.owns_placeholder = owns_placeholder;
            page_map.pfns.push_back(pfn);
            page_map.alias_ptrs.push_back(out_alias.host_ptr);
            ++pfn_table_[pfn].map_refcount;
            mappings_[slot_va] = std::move(page_map);
            return true;
        };

        ScopedRemapWindow remap_guard(std::move(remap_ranges));

        for (const auto& op : ops) {
            if (op.is_unmap) {
                (void)unmap_slot_locked(op.slot_va);
#if defined(_WIN32)
                (void)memory_.PromoteReservedRangeToPlaceholder(
                    reinterpret_cast<void*>(op.slot_va), kPage64KB);
#endif
                continue;
            }
            bool has_prev = false;
            uint64_t prev_pfn = 0ull;
            PageFrameRecord prev_frame{};
            bool prev_owns_ph = false;
            auto ex_it = mappings_.find(op.slot_va);
            if (ex_it != mappings_.end() && !ex_it->second.pfns.empty()) {
                prev_pfn = ex_it->second.pfns[0];
                auto old_pit = pfn_table_.find(prev_pfn);
                if (old_pit != pfn_table_.end()) {
                    has_prev = true;
                    prev_frame = old_pit->second;
                }
            }
            (void)unmap_slot_locked(op.slot_va, &prev_owns_ph);
            if (!map_slot_locked(op.slot_va, op.frame, op.pfn, prev_owns_ph)) {
                if (has_prev) {
                    (void)map_slot_locked(op.slot_va, prev_frame, prev_pfn, prev_owns_ph);
                }
                SetHostLastError(487u);
                return 0;
            }
        }
        lock.unlock();
        NotifyActiveD3D12VirtualRemapRanges(notify_ranges);
        SetHostLastError(0u);
        return 1;
    }

    // Applies Scarlett D3D12X CommandQueue::Slot31 GPU tile page-mapping batches onto the GPU
    // placeholder window (adapted with attribution from MIT-licensed R0Y00/Xbox2Windows X2WMapPageBatch).
    int32_t MapTilePageBatch(uint32_t batchCount,
                             const OPENSCARLETT_TILE_PAGE_BATCH* batches,
                             uint64_t poolBaseVa,
                             uint32_t poolPages,
                             uint32_t flags) {
        constexpr int32_t kHrBatchOk = 0;
        constexpr int32_t kHrBatchInvalidArg = static_cast<int32_t>(0x80070057u);
        constexpr int32_t kHrBatchPointer = static_cast<int32_t>(0x80004003u);
        if (flags != 0u || (batchCount > 0u && !batches) ||
            (poolPages > 0u && poolBaseVa == 0ull) ||
            (poolBaseVa & (kPage64KB - 1u)) != 0ull) {
            return kHrBatchInvalidArg;
        }
        if (batchCount == 0u || !UnifiedMemoryManager::SupportsOsPlaceholders()) {
            return kHrBatchOk;
        }
#if defined(_WIN32)
        auto is_readable = [](const void* p, size_t len) -> bool {
            if (!p || len == 0u) return false;
            uintptr_t cur = reinterpret_cast<uintptr_t>(p);
            if (cur + len < cur) return false;
            size_t rem = len;
            while (rem > 0u) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0u ||
                    mbi.State != MEM_COMMIT ||
                    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0u) {
                    return false;
                }
                const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (end <= cur) return false;
                const size_t step = std::min(rem, static_cast<size_t>(end - cur));
                cur += step;
                rem -= step;
            }
            return true;
        };
        if (!is_readable(batches, static_cast<size_t>(batchCount) * sizeof(OPENSCARLETT_TILE_PAGE_BATCH))) {
            return kHrBatchPointer;
        }
#endif
        struct CapturedTileRange {
            uintptr_t base_va = 0u;
            OPENSCARLETT_TILE_PAGE_RANGE range{};
        };
        std::vector<CapturedTileRange> captured;
        captured.reserve(static_cast<size_t>(batchCount) * 2u);
        constexpr uint32_t kMaxTotalTilePages = 524288u;
        uint32_t total_pages = 0u;
        for (uint32_t b = 0u; b < batchCount; ++b) {
            const OPENSCARLETT_TILE_PAGE_BATCH batch = batches[b];
            if (batch.destination < 0x10000ull ||
                (batch.destination & (kPage64KB - 1u)) != 0ull ||
                batch.count == 0u || !batch.ranges ||
                batch.count > kMaxTotalTilePages - total_pages) {
                return kHrBatchInvalidArg;
            }
#if defined(_WIN32)
            if (!is_readable(batch.ranges, static_cast<size_t>(batch.count) * sizeof(OPENSCARLETT_TILE_PAGE_RANGE))) {
                return kHrBatchPointer;
            }
#endif
            uintptr_t cur_va = static_cast<uintptr_t>(batch.destination);
            for (uint32_t r = 0u; r < batch.count; ++r) {
                const OPENSCARLETT_TILE_PAGE_RANGE rng = batch.ranges[r];
                if (rng.count == 0u || rng.count > kMaxTotalTilePages - total_pages || rng.kind > 3u) {
                    return kHrBatchInvalidArg;
                }
                if (rng.kind <= 1u && poolPages > 0u) {
                    if (rng.start >= poolPages || (rng.kind == 0u && rng.count > poolPages - rng.start)) {
                        return kHrBatchInvalidArg;
                    }
                }
                const size_t bytes = static_cast<size_t>(rng.count) * kPage64KB;
                if (cur_va + bytes < cur_va) {
                    return kHrBatchInvalidArg;
                }
                captured.push_back({cur_va, rng});
                total_pages += rng.count;
                cur_va += bytes;
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        const uintptr_t pool_base = static_cast<uintptr_t>(poolBaseVa);
        const auto pool_root_it = mappings_.find(pool_base);

        auto lookup_pool_pfn_locked = [&](uint32_t page_idx) -> uint64_t {
            if (pool_root_it != mappings_.end() &&
                page_idx < pool_root_it->second.pfns.size()) {
                return pool_root_it->second.pfns[page_idx];
            }
            const uintptr_t pva = pool_base + static_cast<size_t>(page_idx) * kPage64KB;
            const auto pit = mappings_.find(pva);
            if (pit != mappings_.end() && !pit->second.pfns.empty()) {
                return pit->second.pfns[0];
            }
            return 0ull;
        };

        constexpr uint64_t kKeepTileSentinel = ~0ull;
        std::map<uintptr_t, uint64_t> edits;
        for (const auto& item : captured) {
            for (uint32_t p = 0u; p < item.range.count; ++p) {
                const uintptr_t dst_va = item.base_va + static_cast<size_t>(p) * kPage64KB;
                if (item.range.kind == 3u) {
                    edits.try_emplace(dst_va, kKeepTileSentinel);
                } else if (item.range.kind == 2u) {
                    edits[dst_va] = 0ull;
                } else {
                    const uint32_t pool_idx = item.range.start + ((item.range.kind == 0u) ? p : 0u);
                    const uint64_t pfn = lookup_pool_pfn_locked(pool_idx);
                    if (pfn != 0ull) {
                        edits[dst_va] = pfn;
                    }
                }
            }
        }

        std::vector<RemapWindowRange> tile_remap_ranges;
        std::vector<RemapWindowRange> notify_ranges;
        for (const auto& [slot_va, want_pfn] : edits) {
            if (want_pfn == kKeepTileSentinel) {
                continue;
            }
            AppendCoalescedRemapRange(notify_ranges, slot_va, kPage64KB);
            if (want_pfn == 0ull) {
                continue;
            }
            auto ex_it = mappings_.find(slot_va);
            if (ex_it != mappings_.end() &&
                ex_it->second.size_bytes == kPage64KB &&
                !(ex_it->second.pfns.size() == 1u && ex_it->second.pfns[0] == want_pfn)) {
                AppendCoalescedRemapRange(tile_remap_ranges, slot_va, kPage64KB);
            }
        }
        ScopedRemapWindow tile_remap_guard(std::move(tile_remap_ranges));

        for (const auto& [slot_va, want_pfn] : edits) {
            if (want_pfn == kKeepTileSentinel) {
                continue;
            }
            auto ex_it = mappings_.find(slot_va);
            if (want_pfn == 0ull) {
                if (ex_it != mappings_.end() && !ex_it->second.owns_placeholder &&
                    ex_it->second.size_bytes == kPage64KB) {
                    for (void* ap : ex_it->second.alias_ptrs) {
                        (void)memory_.UnmapPhysicalAlias(ap);
                    }
                    detach_mapping_from_pfn_locked(ex_it->second, slot_va);
                    mappings_.erase(ex_it);
                }
                continue;
            }
            if (ex_it != mappings_.end() &&
                ex_it->second.size_bytes == kPage64KB &&
                ex_it->second.pfns.size() == 1u &&
                ex_it->second.pfns[0] == want_pfn) {
                continue;
            }
            auto pit = pfn_table_.find(want_pfn);
            if (pit == pfn_table_.end() || pit->second.page_size_bytes != kPage64KB) {
                continue;
            }
            const PageFrameRecord frame = pit->second;
            if (ex_it != mappings_.end() && !ex_it->second.owns_placeholder &&
                ex_it->second.size_bytes == kPage64KB) {
                for (void* ap : ex_it->second.alias_ptrs) {
                    (void)memory_.UnmapPhysicalAlias(ap);
                }
                detach_mapping_from_pfn_locked(ex_it->second, slot_va);
                mappings_.erase(ex_it);
            }
            void* slot = reinterpret_cast<void*>(slot_va);
            const MemoryProtection prot = MemoryProtection::ReadWrite;
            const uint32_t mem_flags = ToUnifiedFlags(MEM_GRAPHICS | frame.alloc_flags, PAGE_READWRITE);
            const size_t phys_off = static_cast<size_t>(frame.page_index) * kPage64KB;
            UnifiedMemoryAlias out_alias{};
            Status st = memory_.MapPhysicalAlias(
                frame.physical_id, phys_off, kPage64KB, prot, mem_flags, slot, out_alias);
#if defined(_WIN32)
            if (!st.ok()) {
                (void)memory_.PromoteReservedRangeToPlaceholder(slot, kPage64KB);
                st = memory_.MapPhysicalAlias(
                    frame.physical_id, phys_off, kPage64KB, prot, mem_flags, slot, out_alias);
            }
            if (!st.ok()) {
                void* sp_base = nullptr;
                size_t sp_size = 0u;
                size_t sp_granule = 0u;
                bool sp_mapped = false;
                if (memory_.LookupPlaceholderByHostPtr(slot, sp_base, sp_size, sp_granule, sp_mapped) &&
                    sp_base != nullptr && !sp_mapped && sp_size > 0u) {
                    (void)sp_granule;
                    constexpr DWORD kMemCoalescePlaceholders = 0x00000001u;
                    (void)VirtualFree(sp_base, sp_size, MEM_RELEASE | kMemCoalescePlaceholders);
                    st = memory_.MapPhysicalAlias(
                        frame.physical_id, phys_off, kPage64KB, prot, mem_flags, slot, out_alias);
                }
            }
#endif
            if (st.ok()) {
                ActiveTitleMapping page_map{};
                page_map.base_va = slot_va;
                page_map.size_bytes = kPage64KB;
                page_map.page_size_bytes = kPage64KB;
                page_map.primary_physical_id = frame.physical_id;
                page_map.alloc_flags = MEM_GRAPHICS | frame.alloc_flags;
                page_map.win32_protect = PAGE_READWRITE;
                page_map.owns_placeholder = false;
                page_map.is_gpu_tile_view = true;
                page_map.pfns.push_back(want_pfn);
                page_map.alias_ptrs.push_back(out_alias.host_ptr);
                auto& gvas = pfn_table_[want_pfn].gpu_tile_vas;
                if (std::find(gvas.begin(), gvas.end(), slot_va) == gvas.end()) {
                    gvas.push_back(slot_va);
                }
                mappings_[slot_va] = std::move(page_map);
            }
        }
        lock.unlock();
        NotifyActiveD3D12VirtualRemapRanges(notify_ranges);
        return kHrBatchOk;
    }

    int32_t RetryRemappedPageFault(uintptr_t faultVirtualAddress) const {
        if (g_in_remap_window_thread || faultVirtualAddress < 0x10000u) {
            return 0;
        }
        const uintptr_t page = faultVirtualAddress & ~(uintptr_t{kPage64KB} - 1u);
        auto& tracker = GetRemapWindowTracker();
        bool inside = false;
        uint64_t seen = 0ull;
#if defined(_WIN32)
        ::AcquireSRWLockShared(&tracker.lock);
        for (const auto& r : tracker.ranges) {
            if (faultVirtualAddress >= r.base && (faultVirtualAddress - r.base) < r.size) {
                inside = true;
                break;
            }
        }
        seen = tracker.generation.load(std::memory_order_acquire);
        ::ReleaseSRWLockShared(&tracker.lock);
#else
        {
            std::lock_guard<std::mutex> lk(tracker.lock);
            for (const auto& r : tracker.ranges) {
                if (faultVirtualAddress >= r.base && (faultVirtualAddress - r.base) < r.size) {
                    inside = true;
                    break;
                }
            }
            seen = tracker.generation.load(std::memory_order_acquire);
        }
#endif
        if (!inside || seen == 0ull) {
            return 0;
        }

        const uint64_t break_id = (seen & 1ull) ? seen : (seen - 1ull);
        thread_local uintptr_t tl_retried_page = 0u;
        thread_local uint64_t tl_retried_break = 0ull;
        if (tl_retried_page == page && tl_retried_break == break_id) {
            return 0;
        }

        for (uint32_t spin = 0u;
             tracker.generation.load(std::memory_order_acquire) == break_id && spin < 4096u;
             ++spin) {
#if defined(_WIN32)
            if (spin < 64u) {
                YieldProcessor();
            } else {
                ::SwitchToThread();
            }
#endif
        }

        std::lock_guard<std::mutex> lock(mutex_);
        bool mapped_in_state = false;
        auto direct_it = mappings_.find(page);
        if (direct_it != mappings_.end() && direct_it->second.size_bytes >= kPage64KB) {
            mapped_in_state = true;
        } else {
            for (const auto& [base, m] : mappings_) {
                if (page >= base && page < base + m.size_bytes) {
                    mapped_in_state = true;
                    break;
                }
            }
        }
        if (!mapped_in_state) {
            return 0;
        }
#if defined(_WIN32)
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof(mbi)) == 0u ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0u) {
            return 0;
        }
#endif
        tl_retried_page = page;
        tl_retried_break = break_id;
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

    // Zero-syscall DirectStorage physical page scatter transfer:
    // Resolves every PFN under a single brief mutex hold into persistent section R/W views
    // (no VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2 / TLB shootdowns), then copies
    // outside the lock in parallel across DirectStorage worker threads.
    // Returns: 1 = success, 0 = invalid PFN/arg (reject immediately), -1 = fallback to MapTitleVirtualMemory.
    int32_t TransferPhysicalPagesDirect(const uint64_t* pageArray,
                                        uint64_t numberOfPages,
                                        uint16_t pageOffset,
                                        uint32_t byteSize,
                                        uint32_t isWrite,
                                        void* hostBuffer) {
        if (!pageArray || !hostBuffer || numberOfPages == 0ull || numberOfPages > 65536ull || byteSize == 0u) {
            return 0;
        }
        const uint64_t total_span = static_cast<uint64_t>(pageOffset) + static_cast<uint64_t>(byteSize);
        if (total_span > numberOfPages * kPage64KB) {
            return 0;
        }

        thread_local std::vector<uint8_t*> tl_page_ptrs;
        tl_page_ptrs.resize(static_cast<size_t>(numberOfPages));
        size_t page_bytes = kPage64KB;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (uint64_t i = 0ull; i < numberOfPages; ++i) {
                auto it = pfn_table_.find(pageArray[i]);
                if (it == pfn_table_.end()) {
                    return 0;
                }
                if (i == 0ull) {
                    page_bytes = it->second.page_size_bytes;
                    if (page_bytes != kPage64KB) {
                        return -1;
                    }
                } else if (it->second.page_size_bytes != page_bytes) {
                    return 0;
                }
                uint8_t* ptr = nullptr;
                const size_t phys_off = static_cast<size_t>(it->second.page_index) * page_bytes;
                if (!memory_.ResolvePhysicalViewPtr(it->second.physical_id, phys_off, page_bytes, ptr).ok() ||
                    !ptr) {
                    return -1;
                }
                tl_page_ptrs[static_cast<size_t>(i)] = ptr;
            }
        }

        auto* host_bytes = static_cast<uint8_t*>(hostBuffer);
        size_t copied = 0u;
        size_t cur_off = static_cast<size_t>(pageOffset);
        for (size_t i = 0u; i < tl_page_ptrs.size() && copied < byteSize; ++i) {
            const size_t avail = kPage64KB - cur_off;
            const size_t step = std::min<size_t>(static_cast<size_t>(byteSize) - copied, avail);
            if (isWrite != 0u) {
                std::memcpy(tl_page_ptrs[i] + cur_off, host_bytes + copied, step);
            } else {
                std::memcpy(host_bytes + copied, tl_page_ptrs[i] + cur_off, step);
            }
            copied += step;
            cur_off = 0u;
        }
        return 1;
    }

    void* VirtualAllocApp(void* baseAddress,
                          size_t size,
                          uint32_t allocationType,
                          uint32_t protection,
                          uint32_t memoryType = 0u) {
        if (size == 0 || allocationType == 0 || !ValidateProtect(protection)) {
            SetHostLastError(87u);
            return nullptr;
        }

        constexpr uint32_t kMemPhysical = 0x00400000u;
        constexpr uint32_t kMemReservePlaceholder = 0x00040000u;
        constexpr uint32_t kMemReplacePlaceholder = 0x00004000u;
        const bool is_reserve_placeholder = (allocationType & kMemReservePlaceholder) != 0u;
        const bool is_replace_placeholder = (allocationType & kMemReplacePlaceholder) != 0u;
        const bool reserve_only =
            ((allocationType & MEM_RESERVE) != 0u) && ((allocationType & MEM_COMMIT) == 0u) && !is_replace_placeholder;
        const bool is_graphics = (allocationType & MEM_GRAPHICS) != 0u;
        const bool is_physical = (allocationType & kMemPhysical) != 0u;
        const bool is_large =
            ((allocationType & MEM_4MB_PAGES) != 0u) ||
            (!is_physical && (allocationType & MEM_LARGE_PAGES) != 0u);
        const bool require_below_40bit =
            (baseAddress == nullptr) && !is_physical &&
            ((allocationType & MEM_LARGE_PAGES) != 0u) &&
            ((memoryType & 0xFFFu) == 0x283u);

        // MEM_RESERVE | (MEM_GRAPHICS or MEM_PHYSICAL or MEM_RESERVE_PLACEHOLDER) creates a real OS
        // virtual placeholder so VirtualFree(MEM_PRESERVE_PLACEHOLDER) splits, MEM_REPLACE_PLACEHOLDER,
        // and MapTitlePhysicalPages / XMemMapPhysicalPages(va, ...) work natively (including fixed-base
        // reservations such as 0xE0000000).
        if (reserve_only && (is_graphics || is_physical || is_reserve_placeholder) &&
            UnifiedMemoryManager::SupportsOsPlaceholders()) {
            const MemoryPageSize granule = is_large ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
            const size_t gran_bytes = is_large ? kPage4MB : kPage64KB;
            const bool base_aligned =
                (baseAddress == nullptr) ||
                ((reinterpret_cast<uintptr_t>(baseAddress) & (gran_bytes - 1u)) == 0u);
            if (base_aligned) {
                if (size > (SIZE_MAX - (gran_bytes - 1u))) {
                    SetHostLastError(8u);
                    return nullptr;
                }
                const size_t aligned = AlignUp(size, gran_bytes);
                // MEM_RESERVE-only VA placeholders consume virtual address space, not physical RAM.
                // Retail Scarlett titles routinely reserve 24-64 GiB VA windows for sparse physical mapping;
                // only cap obviously absurd/corrupt reservations (> 1 TiB) rather than the 13.5 GiB physical budget.
                constexpr size_t kMaxVirtualReserveBytes = static_cast<size_t>(1024ull) * 1024ull * 1024ull * 1024ull;
                if (aligned > kMaxVirtualReserveBytes) {
                    SetHostLastError(8u);
                    return nullptr;
                }
                void* base = nullptr;
                Status st = (baseAddress != nullptr)
                                ? memory_.ReserveVirtualPlaceholderAt(baseAddress, aligned, base, granule)
                                : memory_.ReserveVirtualPlaceholder(aligned, base, granule);
                if (st.ok() && base != nullptr) {
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
                if (baseAddress == nullptr) {
                    SetHostLastError(8u);
                    return nullptr;
                }
            }
        }

        constexpr size_t kPage2MB = 2ull * 1024ull * 1024ull;
        const size_t align = require_below_40bit
                                 ? (((allocationType & MEM_4MB_PAGES) != 0u) ? kPage4MB : kPage2MB)
                                 : (is_large ? kPage4MB : ((is_graphics || is_physical) ? kPage64KB : kPage4KB));
        const size_t size_gran = (require_below_40bit && (allocationType & MEM_4MB_PAGES) == 0u) ? kPage64KB : align;
        if (size > (SIZE_MAX - (size_gran - 1u))) {
            SetHostLastError(8u);
            return nullptr;
        }
        const size_t aligned_size = AlignUp(size, size_gran);
        const uint64_t budget = GetTitleBudgetBytes();
        const bool is_gpu_noaccess_window =
            ((protection & 0xFFu) == PAGE_NOACCESS) || ((protection & 0x800u) != 0u);

        if ((allocationType & MEM_COMMIT) != 0u && !is_gpu_noaccess_window &&
            aligned_size > budget) {
            SetHostLastError(8u);
            return nullptr;
        }
        constexpr size_t kMaxNoAccessWindowBytes = static_cast<size_t>(1024ull) * 1024ull * 1024ull * 1024ull;
        if (aligned_size > kMaxNoAccessWindowBytes) {
            SetHostLastError(8u);
            return nullptr;
        }

        // Sub-placeholder reserve (MEM_REPLACE_PLACEHOLDER without MEM_COMMIT) inside a
        // known placeholder MUST remain an OS placeholder so XMemMapPhysicalPages /
        // XMemMapPhysicalPagesScatter can bind physical pages into it via MapViewOfFile3.
        if (baseAddress != nullptr && is_replace_placeholder &&
            (allocationType & MEM_COMMIT) == 0u) {
            if (size > (SIZE_MAX - (kPage64KB - 1u))) {
                SetHostLastError(8u);
                return nullptr;
            }
            const uintptr_t req = reinterpret_cast<uintptr_t>(baseAddress);
            const size_t rep_size = AlignUp(size, kPage64KB);
            if (req + rep_size < req) {
                SetHostLastError(487u);
                return nullptr;
            }
            bool in_known_placeholder = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [b, rec] : app_allocs_) {
                    if (rec.is_placeholder && req >= b && req + rep_size <= b + rec.size) {
                        in_known_placeholder = true;
                        break;
                    }
                }
            }
            if (!in_known_placeholder) {
                void* ph_base = nullptr;
                size_t ph_sz = 0u, ph_gr = 0u;
                bool ph_mapped = false;
                if (memory_.LookupPlaceholderByHostPtr(baseAddress, ph_base, ph_sz, ph_gr, ph_mapped) &&
                    ph_base != nullptr) {
                    in_known_placeholder = true;
                }
            }
#if defined(_WIN32)
            if (!in_known_placeholder) {
                uintptr_t cur = req;
                const uintptr_t req_end = req + rep_size;
                bool all_reserved_private = true;
                while (cur < req_end) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0u ||
                        mbi.State != MEM_RESERVE || mbi.Type != MEM_PRIVATE || mbi.RegionSize == 0u) {
                        all_reserved_private = false;
                        break;
                    }
                    const uintptr_t seg_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    if (seg_end <= cur) {
                        all_reserved_private = false;
                        break;
                    }
                    cur = seg_end;
                }
                if (all_reserved_private) {
                    in_known_placeholder = true;
                }
            }
#endif
            if (!in_known_placeholder) {
                SetHostLastError(487u);
                return nullptr;
            }

            (void)memory_.PromoteReservedRangeToPlaceholder(baseAddress, rep_size);
            (void)memory_.SplitVirtualPlaceholder(baseAddress, rep_size);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto it = app_allocs_.begin(); it != app_allocs_.end(); ++it) {
                    const AppVirtualAllocRecord orig = it->second;
                    if (orig.is_placeholder && req >= orig.base && req + rep_size <= orig.base + orig.size) {
                        app_allocs_.erase(it);
                        if (req > orig.base) {
                            AppVirtualAllocRecord lead = orig;
                            lead.size = static_cast<size_t>(req - orig.base);
                            app_allocs_.emplace(lead.base, lead);
                        }
                        if (req + rep_size < orig.base + orig.size) {
                            AppVirtualAllocRecord tail = orig;
                            tail.base = req + rep_size;
                            tail.size = static_cast<size_t>((orig.base + orig.size) - (req + rep_size));
                            app_allocs_.emplace(tail.base, tail);
                        }
                        break;
                    }
                }
                AppVirtualAllocRecord rec{};
                rec.base = req;
                rec.size = rep_size;
                rec.committed_bytes = 0u;
                rec.alloc_flags = allocationType;
                rec.protection = protection;
                rec.is_placeholder = true;
                rec.is_file_mapping = false;
                app_allocs_[rec.base] = rec;
            }
            SetHostLastError(0u);
            return baseAddress;
        }

        // Explicit MEM_REPLACE_PLACEHOLDER (0x4000): replace a split OS placeholder range directly
        // via VirtualAlloc2(MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER) with fallback to
        // MapViewOfFile3(MEM_REPLACE_PLACEHOLDER) (including E:\OpenScarlett\.tmp sparse section).
        if (baseAddress != nullptr && is_replace_placeholder) {
#if defined(_WIN32)
            const PfnKxVirtualAlloc2 s_va2 = GetKxVirtualAlloc2();
            const size_t rep_size = AlignUp(size, kPage64KB);
            const DWORD win_prot = ToWin32PageProtect(protection);
            void* replaced = nullptr;
            bool replaced_via_file = false;
            if (s_va2 && !ShouldPreferSparseCommitKx(rep_size, /*prefer_write_watch_first=*/true)) {
                replaced = s_va2(
                    GetCurrentProcess(),
                    baseAddress,
                    rep_size,
                    MEM_RESERVE | MEM_COMMIT | kMemReplacePlaceholder | MEM_WRITE_WATCH,
                    win_prot,
                    nullptr,
                    0u);
                if (!replaced) {
                    replaced = s_va2(
                        GetCurrentProcess(),
                        baseAddress,
                        rep_size,
                        MEM_RESERVE | MEM_COMMIT | kMemReplacePlaceholder,
                        win_prot,
                        nullptr,
                        0u);
                }
                if (replaced) {
                    (void)::ResetWriteWatch(replaced, rep_size);
                }
            }
            if (!replaced) {
                const PfnKxMapViewOfFile3 s_map3 = GetKxMapViewOfFile3();
                if (s_map3) {
                    const DWORD map_prot = (win_prot == PAGE_READONLY) ? PAGE_READONLY : PAGE_READWRITE;
                    const DWORD hi = static_cast<DWORD>((static_cast<uint64_t>(rep_size) >> 32) & 0xFFFFFFFFull);
                    const DWORD lo = static_cast<DWORD>(static_cast<uint64_t>(rep_size) & 0xFFFFFFFFull);
                    HANDLE hmap = ShouldPreferSparseCommitKx(rep_size, false)
                                      ? nullptr
                                      : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, nullptr);
                    if (!hmap) {
                        hmap = CreateSparseFileSectionKx(rep_size, L"xrepl");
                    }
                    if (hmap) {
                        replaced = s_map3(
                            hmap, GetCurrentProcess(), baseAddress, 0ull, rep_size,
                            kMemReplacePlaceholder, map_prot, nullptr, 0u);
                        CloseHandle(hmap);
                        if (replaced) {
                            replaced_via_file = true;
                            if (win_prot == PAGE_NOACCESS) {
                                DWORD old_p = 0;
                                (void)VirtualProtect(replaced, rep_size, PAGE_NOACCESS, &old_p);
                            }
                        }
                    }
                }
            }
            if (replaced) {
                const size_t commit_charge = is_gpu_noaccess_window ? 0u : rep_size;
                std::lock_guard<std::mutex> lock(mutex_);
                AppVirtualAllocRecord rec{};
                rec.base = reinterpret_cast<uintptr_t>(replaced);
                rec.size = rep_size;
                rec.committed_bytes = commit_charge;
                rec.alloc_flags = allocationType;
                rec.protection = protection;
                rec.is_placeholder = false;
                rec.is_file_mapping = replaced_via_file;
                app_allocs_[rec.base] = rec;
                if (commit_charge > 0u) {
                    app_committed_bytes_.fetch_add(commit_charge, std::memory_order_acq_rel);
                }
                return replaced;
            }
#endif
        }

        // If committing inside a previously reserved OS placeholder (e.g. UE5 XboxCommon
        // FastAllocator calling XMemVirtualAlloc(va, size, MEM_COMMIT|MEM_PHYSICAL) inside a
        // MEM_RESERVE|MEM_PHYSICAL reservation), commit contiguous unmapped spans via
        // UnifiedMemoryManager::MapPhysicalAlias(MEM_REPLACE_PLACEHOLDER).
        if (baseAddress != nullptr && (allocationType & MEM_COMMIT) != 0u &&
            UnifiedMemoryManager::SupportsOsPlaceholders()) {
            const uintptr_t req = reinterpret_cast<uintptr_t>(baseAddress);
            uintptr_t ph_base = 0u;
            size_t ph_size = 0u;
            bool in_placeholder = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [b, rec] : app_allocs_) {
                    if (rec.is_placeholder && req >= b && req < b + rec.size) {
                        in_placeholder = true;
                        ph_base = b;
                        ph_size = rec.size;
                        break;
                    }
                }
            }
            if (in_placeholder) {
                const size_t commit_gran = kPage64KB;
                const uintptr_t page_start = req & ~(commit_gran - 1u);
                const uintptr_t page_end = std::min<uintptr_t>(
                    AlignUp(req + size, commit_gran), ph_base + ph_size);
                const uint32_t mem_flags = ToUnifiedFlags(allocationType, protection);
                const MemoryProtection prot = ToMemoryProtection(protection);
                uintptr_t cur = page_start;
                while (cur < page_end) {
                    UnifiedMemoryAlias existing{};
                    if (memory_.LookupAliasByHostPtr(reinterpret_cast<void*>(cur), existing)) {
                        cur += commit_gran;
                        continue;
                    }
                    const uintptr_t run_start = cur;
                    cur += commit_gran;
                    while (cur < page_end &&
                           !memory_.LookupAliasByHostPtr(reinterpret_cast<void*>(cur), existing)) {
                        cur += commit_gran;
                    }
                    const size_t run_bytes = static_cast<size_t>(cur - run_start);
                    void* slot_ptr = reinterpret_cast<void*>(run_start);
                    PhysicalAllocation phys{};
                    Status ast = memory_.AllocatePhysicalPages(
                        run_bytes, MemoryPageSize::Page64KB, mem_flags, phys);
                    if (!ast.ok()) {
                        SetHostLastError(8u);
                        return nullptr;
                    }
                    UnifiedMemoryAlias alias{};
                    Status mst = memory_.MapPhysicalAlias(
                        phys.physical_id, 0u, run_bytes, prot, mem_flags, slot_ptr, alias);
                    if (!mst.ok()) {
                        (void)memory_.FreePhysicalPages(phys.physical_id);
                        std::fprintf(stderr,
                            "[OpenScarlett][kernelx] Placeholder commit FAILED at %p (req=%p size=%zu ph=%p+%zu): %s\n",
                            slot_ptr, baseAddress, size, reinterpret_cast<void*>(ph_base), ph_size,
                            mst.message.c_str());
                        std::fflush(stderr);
                        SetHostLastError(487u);
                        return nullptr;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    ActiveTitleMapping mapping{};
                    mapping.base_va = reinterpret_cast<uintptr_t>(alias.host_ptr);
                    mapping.size_bytes = run_bytes;
                    mapping.page_size_bytes = commit_gran;
                    mapping.primary_physical_id = phys.physical_id;
                    mapping.alloc_flags = allocationType;
                    mapping.win32_protect = protection;
                    mapping.owns_placeholder = false;
                    mapping.alias_ptrs.push_back(alias.host_ptr);
                    mappings_.emplace(mapping.base_va, std::move(mapping));
                }
                UpdateAndGetPeakTitleUsed();
                return baseAddress;
            }
        }

        bool allocated_via_file_mapping = false;
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
        const bool allow_below40_sparse =
            !require_below_40bit || (aligned_size >= (64ull * 1024ull * 1024ull));
        const bool prefer_sparse =
            allow_below40_sparse &&
            (baseAddress == nullptr) &&
            ((allocationType & MEM_COMMIT) != 0u) &&
            (is_gpu_noaccess_window
                 ? (aligned_size >= (32ull * 1024ull * 1024ull))
                 : ShouldPreferSparseCommitKx(aligned_size, /*prefer_write_watch_first=*/true));
        void* ptr = nullptr;
        if (!prefer_sparse) {
            ptr = AllocVirtualWriteWatched(
                baseAddress, aligned_size, align, win_alloc, win_prot, require_below_40bit);
        }
        if (!ptr && baseAddress == nullptr && (allocationType & MEM_COMMIT) != 0u) {
            // Host pagefile commit pressure fallback: back the allocation with a sparse NTFS file
            // on E:\OpenScarlett\.tmp (never C:\) so untouched pages consume zero commit charge
            // and zero disk blocks instead of returning NULL (preserving <2^40 when required).
            void* mapped = MapSparseViewAlignedKx(
                aligned_size, align, win_prot, L"xmem", require_below_40bit);
            if (mapped) {
                ptr = mapped;
                allocated_via_file_mapping = true;
            } else if (prefer_sparse) {
                ptr = AllocVirtualWriteWatched(
                    baseAddress, aligned_size, align, win_alloc, win_prot, require_below_40bit);
            }
            if (!ptr && !require_below_40bit && is_gpu_noaccess_window) {
                // GPU-only PAGE_NOACCESS VA window never needs CPU commit; MEM_RESERVE alone
                // provides a valid non-overlapping process VA window with zero commit charge.
                ptr = AllocVirtualWriteWatched(
                    nullptr, aligned_size, align, MEM_RESERVE | (win_alloc & MEM_TOP_DOWN), PAGE_NOACCESS);
            }
        }
        if (!ptr && baseAddress != nullptr && (allocationType & MEM_COMMIT) != 0u &&
            GetLastError() == ERROR_INVALID_ADDRESS) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(baseAddress, &mbi, sizeof(mbi)) != 0u) {
                if (mbi.State == MEM_COMMIT) {
                    ptr = baseAddress;
                } else if (mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE) {
                    const PfnKxMapViewOfFile3 s_map3 = GetKxMapViewOfFile3();
                    if (s_map3) {
                        constexpr DWORD kMemPreservePlaceholder = 0x00000002u;
                        const size_t map_bytes = AlignUp(size, kPage64KB);
                        if (mbi.RegionSize > map_bytes || mbi.BaseAddress != baseAddress) {
                            (void)VirtualFree(baseAddress, map_bytes, MEM_RELEASE | kMemPreservePlaceholder);
                        }
                        const DWORD hi = static_cast<DWORD>((static_cast<uint64_t>(map_bytes) >> 32) & 0xFFFFFFFFull);
                        const DWORD lo = static_cast<DWORD>(static_cast<uint64_t>(map_bytes) & 0xFFFFFFFFull);
                        HANDLE hmap = ShouldPreferSparseCommitKx(map_bytes, false)
                                          ? nullptr
                                          : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, nullptr);
                        if (!hmap) {
                            hmap = CreateSparseFileSectionKx(map_bytes, L"xph");
                        }
                        if (hmap) {
                            const DWORD map_prot = (win_prot == PAGE_READONLY) ? PAGE_READONLY : PAGE_READWRITE;
                            void* mapped = s_map3(
                                hmap, GetCurrentProcess(), baseAddress, 0ull, map_bytes,
                                static_cast<ULONG>(kMemReplacePlaceholder), map_prot, nullptr, 0u);
                            CloseHandle(hmap);
                            if (mapped) {
                                ptr = mapped;
                                allocated_via_file_mapping = true;
                                if (win_prot == PAGE_NOACCESS) {
                                    DWORD old_p = 0;
                                    (void)VirtualProtect(mapped, map_bytes, PAGE_NOACCESS, &old_p);
                                }
                            }
                        }
                    }
                }
            }
        }
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
            const size_t commit_charge =
                (((allocationType & MEM_COMMIT) != 0u) && !is_gpu_noaccess_window) ? aligned_size : 0u;
            auto it = app_allocs_.find(key);
            if (it != app_allocs_.end()) {
                if (commit_charge > 0u) {
                    const size_t new_committed = std::max(it->second.committed_bytes, commit_charge);
                    const size_t delta = new_committed - it->second.committed_bytes;
                    it->second.committed_bytes = new_committed;
                    app_committed_bytes_.fetch_add(delta, std::memory_order_acq_rel);
                }
                it->second.protection = protection;
            } else {
                AppVirtualAllocRecord rec{};
                rec.base = key;
                rec.size = aligned_size;
                rec.committed_bytes = commit_charge;
                rec.alloc_flags = allocationType;
                rec.protection = protection;
                rec.is_placeholder = false;
                rec.is_file_mapping = allocated_via_file_mapping;
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
        constexpr uint32_t kMemCoalescePlaceholders = 0x00000001u;
        constexpr uint32_t kMemPreservePlaceholder = 0x00000002u;
        constexpr uint32_t kAllowedFreeMask =
            MEM_DECOMMIT | MEM_RELEASE | kMemCoalescePlaceholders | kMemPreservePlaceholder;

        if (!address || freeType == 0u || (freeType & ~kAllowedFreeMask) != 0u) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0;
        }
        const bool is_release = (freeType & MEM_RELEASE) != 0u;
        const bool is_decommit = (freeType & MEM_DECOMMIT) != 0u;
        if ((!is_release && !is_decommit) || (is_release && is_decommit)) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0;
        }
        // Per Win32 / Xbox VirtualFree[Ex] contract (xcl #87):
        // MEM_RELEASE MUST have dwSize == 0 (unless placeholder preserve/coalesce flags are set).
        if (is_release && size != 0u &&
            (freeType & (kMemCoalescePlaceholders | kMemPreservePlaceholder)) == 0u) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (is_release && (freeType & (kMemPreservePlaceholder | kMemCoalescePlaceholders)) != 0u) {
            if (size == 0u) {
                SetHostLastError(87u);
                return 0;
            }
            if ((freeType & kMemPreservePlaceholder) != 0u &&
                (freeType & kMemCoalescePlaceholders) == 0u) {
                Status st = memory_.SplitVirtualPlaceholder(address, size);
                if (st.ok()) {
                    SetHostLastError(0u);
                    return 1;
                }
                if (st.code != openscarlett::StatusCode::NotFound) {
                    SetHostLastError(87u);
                    return 0;
                }
            } else if ((freeType & kMemCoalescePlaceholders) != 0u &&
                       (freeType & kMemPreservePlaceholder) == 0u) {
                Status st = memory_.CoalesceVirtualPlaceholders(address, size);
                if (st.ok()) {
                    SetHostLastError(0u);
                    return 1;
                }
                if (st.code != openscarlett::StatusCode::NotFound) {
                    SetHostLastError(87u);
                    return 0;
                }
            }
#if defined(_WIN32)
            return VirtualFree(address, size, static_cast<DWORD>(freeType)) ? 1 : 0;
#else
            SetHostLastError(87u);
            return 0;
#endif
        }

        const uintptr_t key = reinterpret_cast<uintptr_t>(address);
        auto mit = mappings_.find(key);
        if (mit != mappings_.end() && (freeType & MEM_DECOMMIT) != 0u && mit->second.pfns.empty()) {
            const uint64_t phys_id = mit->second.primary_physical_id;
            for (void* p : mit->second.alias_ptrs) {
                (void)memory_.UnmapPhysicalAlias(p);
            }
            mappings_.erase(mit);
            if (phys_id != 0ull) {
                (void)memory_.FreePhysicalPages(phys_id);
            }
            return 1;
        }

        auto it = app_allocs_.find(key);
        if (it != app_allocs_.end()) {
            if (it->second.is_placeholder) {
                if (!is_release) {
                    SetHostLastError(87u);
                    return 0;
                }
                Status st = memory_.ReleaseVirtualPlaceholder(address);
                if (!st.ok()) {
#if defined(_WIN32)
                    if (st.code == openscarlett::StatusCode::NotFound &&
                        VirtualFree(address, 0, MEM_RELEASE)) {
                        app_allocs_.erase(it);
                        SetHostLastError(0u);
                        return 1;
                    }
#endif
                    SetHostLastError(87u);
                    return 0;
                }
                app_allocs_.erase(it);
                SetHostLastError(0u);
                return 1;
            }

#if defined(_WIN32)
            if (it->second.is_file_mapping) {
                if (is_release && !UnmapViewOfFile(address)) {
                    return 0;
                }
            } else if (!VirtualFree(address, size, static_cast<DWORD>(freeType))) {
                return 0;
            }
#else
            if (is_release) {
                std::free(address);
            }
#endif
            if (is_release) {
                if (it->second.committed_bytes > 0) {
                    app_committed_bytes_.fetch_sub(it->second.committed_bytes, std::memory_order_acq_rel);
                }
                app_allocs_.erase(it);
            } else if (is_decommit) {
                const size_t decommit = (size == 0 || size >= it->second.committed_bytes)
                                            ? it->second.committed_bytes
                                            : size;
                it->second.committed_bytes -= decommit;
                app_committed_bytes_.fetch_sub(decommit, std::memory_order_acq_rel);
            }
            SetHostLastError(0u);
            return 1;
        }

        if (is_decommit) {
            for (auto& [b, rec] : app_allocs_) {
                if (rec.is_file_mapping && key >= b && key < b + rec.size) {
                    const size_t max_span = static_cast<size_t>((b + rec.size) - key);
                    const size_t decommit = (size == 0u || size > max_span) ? max_span : size;
#if defined(_WIN32)
                    using PfnDiscardVirtualMemory = DWORD(WINAPI*)(PVOID, SIZE_T);
                    static PfnDiscardVirtualMemory s_discard = []() noexcept -> PfnDiscardVirtualMemory {
                        HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
                        if (!kb) kb = ::GetModuleHandleW(L"kernel32.dll");
                        return kb ? reinterpret_cast<PfnDiscardVirtualMemory>(
                                        ::GetProcAddress(kb, "DiscardVirtualMemory"))
                                  : nullptr;
                    }();
                    if (s_discard && decommit >= kPage4KB) {
                        const uintptr_t pg_beg = AlignUp(key, kPage4KB);
                        const uintptr_t pg_end = (key + decommit) & ~(kPage4KB - 1u);
                        if (pg_end > pg_beg) {
                            (void)s_discard(reinterpret_cast<PVOID>(pg_beg), pg_end - pg_beg);
                        }
                    }
#endif
                    const size_t charge_sub = std::min(decommit, rec.committed_bytes);
                    rec.committed_bytes -= charge_sub;
                    if (charge_sub > 0u) {
                        app_committed_bytes_.fetch_sub(charge_sub, std::memory_order_acq_rel);
                    }
                    SetHostLastError(0u);
                    return 1;
                }
            }
        }

#if defined(_WIN32)
        return VirtualFree(address, size, static_cast<DWORD>(freeType)) ? 1 : 0;
#else
        SetHostLastError(487u);  // ERROR_INVALID_ADDRESS
        return 0;
#endif
    }

    int32_t VirtualFreeExApp(void* hProcess, void* address, size_t size, uint32_t freeType) {
        if (!hProcess) {
            SetHostLastError(6u);  // ERROR_INVALID_HANDLE
            return 0;
        }
#if defined(_WIN32)
        HANDLE proc = static_cast<HANDLE>(hProcess);
        const bool is_current_proc =
            (proc == GetCurrentProcess()) ||
            (GetProcessId(proc) == GetCurrentProcessId() && GetProcessId(proc) != 0u);
        if (is_current_proc) {
            return VirtualFreeApp(address, size, freeType);
        }
        if (!address || freeType == 0u) {
            SetHostLastError(87u);
            return 0;
        }
        if ((freeType & MEM_RELEASE) != 0u && size != 0u &&
            (freeType & (0x00000001u | 0x00000002u)) == 0u) {
            SetHostLastError(87u);
            return 0;
        }
        return VirtualFreeEx(proc, address, size, static_cast<DWORD>(freeType)) ? 1 : 0;
#else
        return VirtualFreeApp(address, size, freeType);
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
        if (size > (static_cast<uintptr_t>(~uintptr_t{0}) - req_start)) {
            SetHostLastError(87u);
            return 0;
        }
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
            SetHostLastError(87u);
            return kHrInvalidArg;
        }
        // Guard against excessively long / malformed thread name strings (> 32767 chars).
        constexpr size_t kMaxThreadNameChars = 32767u;
        size_t name_len = 0u;
        while (name_len <= kMaxThreadNameChars && lpThreadName[name_len] != L'\0') {
            ++name_len;
        }
        if (name_len > kMaxThreadNameChars) {
            SetHostLastError(87u);
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
            thread_names_[tid].assign(lpThreadName, name_len);
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
        thread_names_[0].assign(lpThreadName, name_len);
        return kHrOk;
#endif
    }

    int32_t GetThreadNameImpl(void* hThread,
                              wchar_t* lpThreadName,
                              size_t nBufferLength,
                              size_t* pnRequiredLength) {
        if ((!lpThreadName && nBufferLength > 0u) || (!lpThreadName && !pnRequiredLength)) {
            SetHostLastError(87u);
            return kHrInvalidArg;
        }
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
            if (lpThreadName && nBufferLength > 0u) {
                const size_t copy_chars = nBufferLength - 1u;
                if (copy_chars > 0u) {
                    std::memcpy(lpThreadName, resolved.c_str(), copy_chars * sizeof(wchar_t));
                }
                lpThreadName[copy_chars] = L'\0';
            }
            SetHostLastError(122u);  // ERROR_INSUFFICIENT_BUFFER
            return kHrInsufficientBuffer;
        }
        std::memcpy(lpThreadName, resolved.c_str(), required * sizeof(wchar_t));
        SetHostLastError(0u);
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
                if (!memory_.ReleaseVirtualPlaceholder(reinterpret_cast<void*>(base)).ok()) {
#if defined(_WIN32)
                    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
#endif
                }
            } else {
#if defined(_WIN32)
                if (rec.is_file_mapping) {
                    ::UnmapViewOfFile(reinterpret_cast<void*>(base));
                } else {
                    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
                }
#else
                std::free(reinterpret_cast<void*>(base));
#endif
            }
        }
        app_allocs_.clear();

        for (auto& [ptr, rec] : xmem_allocs_) {
#if defined(_WIN32)
            if (rec.backing == XMemBackingKind::VirtualAllocMem) {
                ::VirtualFree(reinterpret_cast<void*>(ptr), 0u, MEM_RELEASE);
            } else if (rec.backing == XMemBackingKind::FileMappingView) {
                ::UnmapViewOfFile(reinterpret_cast<void*>(ptr));
            } else {
                _aligned_free(reinterpret_cast<void*>(ptr));
            }
#else
            (void)rec;
            std::free(reinterpret_cast<void*>(ptr));
#endif
        }
        xmem_allocs_.clear();

        for (auto& [ptr, sz] : aux_allocs_) {
            (void)sz;
#if defined(_WIN32)
            if (aux_file_mapped_.find(ptr) != aux_file_mapped_.end()) {
                ::UnmapViewOfFile(reinterpret_cast<void*>(ptr));
            } else {
                VirtualFree(reinterpret_cast<void*>(ptr), 0, MEM_RELEASE);
            }
#else
            std::free(reinterpret_cast<void*>(ptr));
#endif
        }
        aux_allocs_.clear();
        aux_file_mapped_.clear();

        pfn_table_.clear();
        free_pfns_.clear();
        next_pfn_ = 0x10ull;
        pools_.clear();
    }

private:
    KernelXState() : memory_(), tile_pool_(memory_) {}

    uint64_t AllocateNextPfnLocked() {
        constexpr uint64_t kMinPfn = 0x10ull;
        constexpr uint64_t kPfnStep = 0x10ull;
        constexpr uint64_t kPfnLimit = 0x2000000ull;
        while (!free_pfns_.empty()) {
            const auto first_it = free_pfns_.begin();
            const uint64_t reused = *first_it;
            free_pfns_.erase(first_it);
            if (reused >= kMinPfn && reused < kPfnLimit && (reused & 0xFull) == 0ull &&
                pfn_table_.find(reused) == pfn_table_.end()) {
                return reused;
            }
        }
        constexpr uint64_t kMaxSlots = (kPfnLimit - kMinPfn) / kPfnStep + 1ull;
        for (uint64_t attempt = 0; attempt < kMaxSlots; ++attempt) {
            if (next_pfn_ < kMinPfn || next_pfn_ >= kPfnLimit || (next_pfn_ & 0xFull) != 0ull) {
                next_pfn_ = kMinPfn;
            }
            const uint64_t candidate = next_pfn_;
            next_pfn_ += kPfnStep;
            if (next_pfn_ >= kPfnLimit) {
                next_pfn_ = kMinPfn;
            }
            if (pfn_table_.find(candidate) == pfn_table_.end()) {
                return candidate;
            }
        }
        return 0ull;
    }

    UnifiedMemoryManager memory_;
    GpuTilePool tile_pool_;
    mutable std::mutex mutex_;
    uint64_t next_pfn_ = 0x10ull;
    std::set<uint64_t> free_pfns_;
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
    std::unordered_set<uintptr_t> aux_file_mapped_;
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
    const uint64_t used = std::min<uint64_t>(state.CurrentTitleUsedBytes(), total);
    const uint64_t peak = std::min<uint64_t>(state.UpdateAndGetPeakTitleUsed(), total);
    const uint64_t avail = total - used;

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

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXJobMemoryStatus(
    TITLEMEMORYSTATUS* lpBuffer) {
    if (!lpBuffer) {
        SetHostLastError(87u);
        RecordTelemetry("JobMemoryStatus", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    if (lpBuffer->dwLength != 0 && lpBuffer->dwLength < sizeof(TITLEMEMORYSTATUS)) {
        SetHostLastError(87u);
        RecordTelemetry("JobMemoryStatus", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    auto& state = KernelXState::Instance();
    const uint64_t total = GetTitleBudgetBytes();
    const uint64_t used = std::min<uint64_t>(state.CurrentTitleUsedBytes(), total);
    const uint64_t peak = std::min<uint64_t>(state.UpdateAndGetPeakTitleUsed(), total);
    const uint64_t avail = total - used;

    lpBuffer->dwLength = static_cast<uint32_t>(sizeof(TITLEMEMORYSTATUS));
    lpBuffer->dwReserved = 0;
    lpBuffer->ullTotalMem = total;
    lpBuffer->ullAvailMem = avail;
    lpBuffer->ullLegacyUsed = used;
    lpBuffer->ullLegacyPeak = peak;
    lpBuffer->ullLegacyAvail = avail;
    lpBuffer->ullTitleUsed = used;
    lpBuffer->ullTitleAvail = avail;

    RecordTelemetry("JobMemoryStatus", CallOutcome::Ok, kHrOk);
    return 1;
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXGlobalMemoryStatusEx(
    void* lpBuffer) {
#if defined(_WIN32)
    auto* ms = static_cast<MEMORYSTATUSEX*>(lpBuffer);
    if (!ms || (ms->dwLength != 0u && ms->dwLength < sizeof(MEMORYSTATUSEX))) {
        SetHostLastError(87u);
        RecordTelemetry("GlobalMemoryStatusEx", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    MEMORYSTATUSEX host_ms{};
    host_ms.dwLength = sizeof(host_ms);
    if (!::GlobalMemoryStatusEx(&host_ms)) {
        host_ms.ullTotalVirtual = 0x00007FFFFFFFE000ull;
        host_ms.ullAvailVirtual = 0x00007FFE00000000ull;
    }
    auto& state = KernelXState::Instance();
    const uint64_t budget = GetTitleBudgetBytes();
    const uint64_t used = std::min<uint64_t>(state.CurrentTitleUsedBytes(), budget);
    const uint64_t avail = budget - used;

    ms->dwLength = sizeof(MEMORYSTATUSEX);
    ms->dwMemoryLoad = budget > 0ull
                           ? static_cast<DWORD>(std::min<uint64_t>(100ull, (used * 100ull) / budget))
                           : 0u;
    ms->ullTotalPhys = budget;
    ms->ullAvailPhys = avail;
    ms->ullTotalPageFile = budget;
    ms->ullAvailPageFile = avail;
    ms->ullTotalVirtual = host_ms.ullTotalVirtual;
    ms->ullAvailVirtual = host_ms.ullAvailVirtual;
    ms->ullAvailExtendedVirtual = 0ull;
    SetHostLastError(0u);
    RecordTelemetry("GlobalMemoryStatusEx", CallOutcome::Ok, kHrOk);
    return 1;
#else
    (void)lpBuffer;
    SetHostLastError(87u);
    return 0;
#endif
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXQueryInformationJobObject(
    void* hJob,
    int jobObjectInfoClass,
    void* lpJobObjectInformation,
    uint32_t cbJobObjectInformationLength,
    uint32_t* lpReturnLength) {
#if defined(_WIN32)
    if (!lpJobObjectInformation) {
        SetHostLastError(87u);
        RecordTelemetry("QueryInformationJobObject", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    const uint64_t budget = GetTitleBudgetBytes();
    auto& state = KernelXState::Instance();
    const uint64_t used = std::min<uint64_t>(state.CurrentTitleUsedBytes(), budget);
    const uint64_t peak = std::min<uint64_t>(state.UpdateAndGetPeakTitleUsed(), budget);

    if (jobObjectInfoClass == static_cast<int>(JobObjectExtendedLimitInformation)) {
        if (cbJobObjectInformationLength < sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)) {
            if (lpReturnLength) *lpReturnLength = sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
            SetHostLastError(24u);  // ERROR_BAD_LENGTH
            RecordTelemetry("QueryInformationJobObject", CallOutcome::Error, kHrInvalidArg);
            return 0;
        }
        auto* ext = static_cast<JOBOBJECT_EXTENDED_LIMIT_INFORMATION*>(lpJobObjectInformation);
        DWORD host_ret = 0u;
        if (!::QueryInformationJobObject(static_cast<HANDLE>(hJob),
                                         JobObjectExtendedLimitInformation,
                                         ext,
                                         cbJobObjectInformationLength,
                                         &host_ret)) {
            if (hJob != nullptr) {
                RecordTelemetry("QueryInformationJobObject", CallOutcome::Error, kHrFail);
                return 0;
            }
            std::memset(ext, 0, sizeof(*ext));
        }
        ext->BasicLimitInformation.LimitFlags |=
            (JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY);
        ext->ProcessMemoryLimit = static_cast<SIZE_T>(budget);
        ext->JobMemoryLimit = static_cast<SIZE_T>(budget);
        ext->PeakProcessMemoryUsed = static_cast<SIZE_T>(
            std::min<uint64_t>(std::max<uint64_t>(peak, ext->PeakProcessMemoryUsed), budget));
        ext->PeakJobMemoryUsed = static_cast<SIZE_T>(
            std::min<uint64_t>(std::max<uint64_t>(peak, ext->PeakJobMemoryUsed), budget));
        if (lpReturnLength) *lpReturnLength = sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
        SetHostLastError(0u);
        RecordTelemetry("QueryInformationJobObject", CallOutcome::Ok, kHrOk);
        return 1;
    }

    DWORD host_ret = 0u;
    const BOOL ok = ::QueryInformationJobObject(
        static_cast<HANDLE>(hJob),
        static_cast<JOBOBJECTINFOCLASS>(jobObjectInfoClass),
        lpJobObjectInformation,
        cbJobObjectInformationLength,
        &host_ret);
    if (lpReturnLength) *lpReturnLength = static_cast<uint32_t>(host_ret);
    (void)used;
    RecordTelemetry("QueryInformationJobObject", ok ? CallOutcome::Ok : CallOutcome::Error,
                    ok ? kHrOk : kHrFail);
    return ok ? 1 : 0;
#else
    (void)hJob;
    (void)jobObjectInfoClass;
    (void)lpJobObjectInformation;
    (void)cbJobObjectInformationLength;
    (void)lpReturnLength;
    return 0;
#endif
}

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXGetProcessInformation(
    void* hProcess,
    int processInformationClass,
    void* processInformation,
    uint32_t processInformationSize) {
#if defined(_WIN32)
    // ProcessAppMemoryInfo == 9 in Win10/GameCore PROCESS_INFORMATION_CLASS
    constexpr int kProcessAppMemoryInfo = 9;
    struct KxAppMemoryInformation {
        uint64_t AvailableCommit;
        uint64_t PrivateCommitUsage;
        uint64_t PeakPrivateCommitUsage;
        uint64_t TotalCommitUsage;
    };
    if (processInformationClass == kProcessAppMemoryInfo) {
        if (!processInformation || processInformationSize < sizeof(KxAppMemoryInformation)) {
            SetHostLastError(87u);
            RecordTelemetry("GetProcessInformation", CallOutcome::Error, kHrInvalidArg);
            return 0;
        }
        auto& state = KernelXState::Instance();
        const uint64_t budget = GetTitleBudgetBytes();
        const uint64_t used = std::min<uint64_t>(state.CurrentTitleUsedBytes(), budget);
        const uint64_t peak = std::min<uint64_t>(state.UpdateAndGetPeakTitleUsed(), budget);
        auto* out = static_cast<KxAppMemoryInformation*>(processInformation);
        out->AvailableCommit = budget - used;
        out->PrivateCommitUsage = used;
        out->PeakPrivateCommitUsage = peak;
        out->TotalCommitUsage = used;
        SetHostLastError(0u);
        RecordTelemetry("GetProcessInformation", CallOutcome::Ok, kHrOk);
        return 1;
    }
    using PfnGetProcessInformation = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    static PfnGetProcessInformation s_fn = []() -> PfnGetProcessInformation {
        HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
        if (!kb) kb = ::GetModuleHandleW(L"kernel32.dll");
        return kb ? reinterpret_cast<PfnGetProcessInformation>(
                        ::GetProcAddress(kb, "GetProcessInformation"))
                  : nullptr;
    }();
    if (!s_fn) {
        SetHostLastError(87u);
        RecordTelemetry("GetProcessInformation", CallOutcome::Error, kHrInvalidArg);
        return 0;
    }
    const BOOL ok = s_fn(
        static_cast<HANDLE>(hProcess), processInformationClass, processInformation, processInformationSize);
    RecordTelemetry("GetProcessInformation", ok ? CallOutcome::Ok : CallOutcome::Error,
                    ok ? kHrOk : kHrFail);
    return ok ? 1 : 0;
#else
    (void)hProcess;
    (void)processInformationClass;
    (void)processInformation;
    (void)processInformationSize;
    return 0;
#endif
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

// Xbox Game OS / XMem working set statistics (kernelx.dll & xmem.dll).
// Self-contained ABI layout matching Win32/GDK size-prefixed memory stats structs.
struct XMEM_WORKING_SET_STATISTICS {
    uint64_t Size;
    uint64_t TotalPhysicalBytes;
    uint64_t WorkingSetBytes;
    uint64_t PeakWorkingSetBytes;
    uint64_t PrivateUsageBytes;
    uint64_t AvailablePhysicalBytes;
    uint64_t PageFaultCount;
    uint64_t TrackedTitleAllocBytes;
};

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XMemGetWorkingSetStatistics(
    uintptr_t arg0,
    void* arg1) {
    // Real Xbox Series X GDK titles (e.g. Minecraft Preview) call the 2-arg ABI:
    //   BOOL XMemGetWorkingSetStatistics(DWORD flags, XMEM_WORKING_SET_STATISTICS* outStats)
    // where arg0 is a non-pointer flag word (e.g. 0x20000001) and arg1 points to a 32-byte
    // uninitialized stack struct whose first two QWORDs are (total_bytes, available_bytes).
    // Preserve backwards compatibility with the 1-arg (void* lpBuffer) test ABI by probing
    // whether arg0 is a committed readable/writable address.
    bool is_two_arg_xbox_abi = false;
    size_t out_region_avail = sizeof(XMEM_WORKING_SET_STATISTICS);
#if defined(_WIN32)
    out_region_avail = 0u;
    if (arg0 != 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T q = ::VirtualQuery(reinterpret_cast<LPCVOID>(arg0), &mbi, sizeof(mbi));
        const bool arg0_committed = (q != 0u && mbi.State == MEM_COMMIT &&
                                     (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0u);
        if (!arg0_committed) {
            if (arg1 != nullptr) {
                MEMORY_BASIC_INFORMATION mbi1{};
                const SIZE_T q1 = ::VirtualQuery(arg1, &mbi1, sizeof(mbi1));
                if (q1 != 0u && mbi1.State == MEM_COMMIT &&
                    (mbi1.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0u) {
                    is_two_arg_xbox_abi = true;
                    const uintptr_t p_addr = reinterpret_cast<uintptr_t>(arg1);
                    const uintptr_t r_end = reinterpret_cast<uintptr_t>(mbi1.BaseAddress) + mbi1.RegionSize;
                    if (r_end > p_addr) {
                        out_region_avail = static_cast<size_t>(r_end - p_addr);
                    }
                }
            } else if ((arg0 & 0x20000000u) != 0u) {
                is_two_arg_xbox_abi = true;
            }
        }
    }
#endif

    void* lpBuffer = is_two_arg_xbox_abi ? arg1 : reinterpret_cast<void*>(arg0);
    if (!lpBuffer) {
        SetHostLastError(87u);
        RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
        return 0;
    }

    uint64_t caller_size = 0ull;
    if (!is_two_arg_xbox_abi) {
        std::memcpy(&caller_size, lpBuffer, sizeof(caller_size));
        if (caller_size != 0ull && caller_size < sizeof(uint64_t)) {
            SetHostLastError(87u);
            RecordTelemetry(__func__, CallOutcome::Error, kHrInvalidArg);
            return 0;
        }
    }

    auto& state = KernelXState::Instance();
    const uint64_t title_budget = GetTitleBudgetBytes();
    const uint64_t tracked_used = std::min<uint64_t>(state.CurrentTitleUsedBytes(), title_budget);
    const uint64_t tracked_peak = std::min<uint64_t>(state.UpdateAndGetPeakTitleUsed(), title_budget);

    XMEM_WORKING_SET_STATISTICS stats{};
    stats.Size = sizeof(XMEM_WORKING_SET_STATISTICS);
    stats.TotalPhysicalBytes = title_budget;
    stats.WorkingSetBytes = tracked_used;
    stats.PeakWorkingSetBytes = tracked_peak;
    stats.PrivateUsageBytes = tracked_used;
    stats.AvailablePhysicalBytes = title_budget - tracked_used;
    stats.PageFaultCount = 0ull;
    stats.TrackedTitleAllocBytes = tracked_used;

#if defined(_WIN32)
    struct OsProcessMemoryCountersEx {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
        SIZE_T PrivateUsage;
    };
    using PfnGetProcessMemoryInfo = BOOL(WINAPI*)(HANDLE, OsProcessMemoryCountersEx*, DWORD);
    PfnGetProcessMemoryInfo pfn_get_pmi = nullptr;
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        pfn_get_pmi = reinterpret_cast<PfnGetProcessMemoryInfo>(
            ::GetProcAddress(k32, "K32GetProcessMemoryInfo"));
    }
    if (!pfn_get_pmi) {
        HMODULE psapi = ::GetModuleHandleW(L"psapi.dll");
        if (psapi) {
            pfn_get_pmi = reinterpret_cast<PfnGetProcessMemoryInfo>(
                ::GetProcAddress(psapi, "GetProcessMemoryInfo"));
        }
    }
    if (pfn_get_pmi) {
        OsProcessMemoryCountersEx pmc{};
        pmc.cb = sizeof(pmc);
        if (pfn_get_pmi(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
            stats.WorkingSetBytes = std::min<uint64_t>(
                title_budget,
                std::max<uint64_t>(tracked_used, static_cast<uint64_t>(pmc.WorkingSetSize)));
            stats.PeakWorkingSetBytes = std::min<uint64_t>(
                title_budget,
                std::max<uint64_t>(
                    std::max<uint64_t>(tracked_peak, stats.WorkingSetBytes),
                    static_cast<uint64_t>(pmc.PeakWorkingSetSize)));
            stats.PrivateUsageBytes = std::min<uint64_t>(
                title_budget,
                std::max<uint64_t>(tracked_used, static_cast<uint64_t>(pmc.PrivateUsage)));
            stats.PageFaultCount = static_cast<uint64_t>(pmc.PageFaultCount);
        }
    }
    stats.AvailablePhysicalBytes =
        (title_budget > stats.WorkingSetBytes) ? (title_budget - stats.WorkingSetBytes) : 0ull;
#endif

    if (is_two_arg_xbox_abi) {
        struct XboxWorkingSetStats32 {
            uint64_t TotalBytes;
            uint64_t AvailableBytes;
            uint64_t WorkingSetBytes;
            uint64_t PeakWorkingSetBytes;
        };
        struct XboxWorkingSetStats78 {
            uint64_t title_total;   // +0x00
            uint64_t title_used;    // +0x08
            uint64_t working_set;   // +0x10
            uint64_t peak_ws;       // +0x18
            uint64_t reserved_20;   // +0x20
            uint64_t commit_total;  // +0x28
            uint64_t commit_used;   // +0x30
            uint64_t reserved_38;   // +0x38
            uint64_t reserved_40;   // +0x40
            uint64_t reserved_48;   // +0x48
            uint64_t reserved_50;   // +0x50
            uint64_t peak_used;     // +0x58
            uint64_t reserved_60;   // +0x60
            uint64_t reserved_68;   // +0x68
            uint64_t reserved_70;   // +0x70
        };
        static_assert(sizeof(XboxWorkingSetStats78) == 0x78u,
                      "XboxWorkingSetStats78 must be exactly 0x78 bytes");
        static_assert(offsetof(XboxWorkingSetStats78, title_total) == 0x00u, "offset +0x00");
        static_assert(offsetof(XboxWorkingSetStats78, title_used) == 0x08u, "offset +0x08");
        static_assert(offsetof(XboxWorkingSetStats78, commit_total) == 0x28u, "offset +0x28");
        static_assert(offsetof(XboxWorkingSetStats78, commit_used) == 0x30u, "offset +0x30");
        static_assert(offsetof(XboxWorkingSetStats78, peak_used) == 0x58u, "offset +0x58");

        bool use_32_byte_compat = (out_region_avail < sizeof(XboxWorkingSetStats78));
        if (!use_32_byte_compat && arg0 == 0x20000001ull) {
            const auto* raw = static_cast<const uint8_t*>(lpBuffer);
            bool first_32_cc = true;
            for (size_t i = 0; i < sizeof(XboxWorkingSetStats32); ++i) {
                if (raw[i] != 0xCCu) {
                    first_32_cc = false;
                    break;
                }
            }
            if (first_32_cc) {
                bool rest_88_cc = true;
                for (size_t i = sizeof(XboxWorkingSetStats32); i < sizeof(XboxWorkingSetStats78); ++i) {
                    if (raw[i] != 0xCCu) {
                        rest_88_cc = false;
                        break;
                    }
                }
                if (!rest_88_cc) {
                    use_32_byte_compat = true;
                }
            }
        }

        if (use_32_byte_compat) {
            XboxWorkingSetStats32 xb{};
            xb.TotalBytes = stats.TotalPhysicalBytes;
            xb.AvailableBytes = stats.AvailablePhysicalBytes;
            xb.WorkingSetBytes = stats.WorkingSetBytes;
            xb.PeakWorkingSetBytes = stats.PeakWorkingSetBytes;
            const size_t copy_len = std::min(out_region_avail, sizeof(xb));
            std::memcpy(lpBuffer, &xb, copy_len);
        } else {
            XboxWorkingSetStats78 xb78{};
            const uint64_t total_bytes = stats.TotalPhysicalBytes;
            const uint64_t used_bytes = std::min<uint64_t>(stats.WorkingSetBytes, total_bytes);
            const uint64_t commit_used_bytes = std::min<uint64_t>(stats.PrivateUsageBytes, total_bytes);
            xb78.title_total = total_bytes;
            xb78.title_used = used_bytes;
            xb78.working_set = stats.WorkingSetBytes;
            xb78.peak_ws = stats.PeakWorkingSetBytes;
            xb78.commit_total = total_bytes;
            xb78.commit_used = commit_used_bytes;
            xb78.peak_used = std::max<uint64_t>(stats.PeakWorkingSetBytes, commit_used_bytes);
            std::memcpy(lpBuffer, &xb78, sizeof(xb78));
        }
    } else {
        const size_t copy_bytes =
            (caller_size == 0ull || caller_size >= sizeof(XMEM_WORKING_SET_STATISTICS))
                ? sizeof(XMEM_WORKING_SET_STATISTICS)
                : static_cast<size_t>(caller_size);
        if (copy_bytes < sizeof(XMEM_WORKING_SET_STATISTICS)) {
            stats.Size = static_cast<uint64_t>(copy_bytes);
        }
        std::memcpy(lpBuffer, &stats, copy_bytes);
    }

    SetHostLastError(0u);
    RecordTelemetry(__func__, CallOutcome::Ok, kHrOk);
    return 1;
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

int32_t OPENSCARLETT_KERNELX_CALL OpenScarlettTransferTitlePhysicalPages(const uint64_t* pPageArray,
                                                                         uint64_t numberOfPages,
                                                                         uint16_t pageOffset,
                                                                         uint32_t byteSize,
                                                                         uint32_t isWrite,
                                                                         void* hostBuffer) {
    return KernelXState::Instance().TransferPhysicalPagesDirect(
        pPageArray, numberOfPages, pageOffset, byteSize, isWrite, hostBuffer);
}

int32_t OPENSCARLETT_KERNELX_CALL OpenScarlettMapPhysicalPagesScatter(void* const* addresses,
                                                                      uint64_t count,
                                                                      const uint64_t* tokens) {
    return KernelXState::Instance().MapPhysicalPagesScatter(addresses, count, tokens);
}

int32_t OPENSCARLETT_KERNELX_CALL OpenScarlettMapTilePageBatch(
    uint32_t batchCount,
    const OPENSCARLETT_TILE_PAGE_BATCH* batches,
    uint64_t poolBaseVa,
    uint32_t poolPages,
    uint32_t flags) {
    return KernelXState::Instance().MapTilePageBatch(
        batchCount, batches, poolBaseVa, poolPages, flags);
}

int32_t OPENSCARLETT_KERNELX_CALL OpenScarlettRetryRemappedPageFault(uintptr_t faultVirtualAddress) {
    return KernelXState::Instance().RetryRemappedPageFault(faultVirtualAddress);
}

void* OPENSCARLETT_KERNELX_CALL OpenScarlettVirtualAllocWithMemType(void* baseAddress,
                                                                    size_t size,
                                                                    uint32_t allocationType,
                                                                    uint32_t memoryType,
                                                                    uint32_t protection) {
    void* ptr = KernelXState::Instance().VirtualAllocApp(
        baseAddress, size, allocationType, protection, memoryType);
    RecordTelemetry("VirtualAllocFromApp", ptr ? CallOutcome::Ok : CallOutcome::Error, ptr ? kHrOk : kHrFail);
    return ptr;
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

int32_t OPENSCARLETT_KERNELX_CALL KernelXVirtualFreeEx(void* hProcess,
                                                       void* address,
                                                       size_t size,
                                                       uint32_t freeType) {
    const int32_t ok = KernelXState::Instance().VirtualFreeExApp(hProcess, address, size, freeType);
    RecordTelemetry("VirtualFreeEx", ok ? CallOutcome::Ok : CallOutcome::Error, ok ? kHrOk : kHrInvalidArg);
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
