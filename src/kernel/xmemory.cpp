#include "openscarlett/kernel/xmemory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#endif

namespace openscarlett {

namespace {

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

#if defined(_WIN32)
std::mutex& VirtualBlockSetMutex() {
    static std::mutex m;
    return m;
}

std::unordered_set<void*>& VirtualBlockSet() {
    static std::unordered_set<void*> s;
    return s;
}
#endif

#if defined(_WIN32)
constexpr DWORD kMemCoalescePlaceholders = 0x00000001u;
constexpr DWORD kMemPreservePlaceholder = 0x00000002u;
constexpr DWORD kMemReplacePlaceholder = 0x00004000u;
constexpr DWORD kMemReservePlaceholder = 0x00040000u;

// ABI-identical structures for VirtualAlloc2 extended parameters
// (avoids depending on host _WIN32_WINNT macro configuration).
struct OsMemAddressRequirements {
    void* lowest_starting_address;
    void* highest_ending_address;
    SIZE_T alignment;
};

struct alignas(8) OsMemExtendedParameter {
    uint64_t type_and_reserved;  // Low 8 bits: 1 == MemExtendedParameterAddressRequirements
    union {
        uint64_t ulong64;
        void* pointer;
        SIZE_T size;
        HANDLE handle;
        DWORD ulong;
    } value;
};

static_assert(sizeof(OsMemExtendedParameter) == 16,
              "OsMemExtendedParameter must match 16-byte Windows MEM_EXTENDED_PARAMETER layout");

using FnVirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, PVOID, ULONG);
using FnMapViewOfFile3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, PVOID, ULONG);
using FnUnmapViewOfFile2 = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct Win32PlaceholderApi {
    FnVirtualAlloc2 virtual_alloc2 = nullptr;
    FnMapViewOfFile3 map_view_of_file3 = nullptr;
    FnUnmapViewOfFile2 unmap_view_of_file2 = nullptr;

    Win32PlaceholderApi() {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) {
            kb = LoadLibraryW(L"kernelbase.dll");
        }
        if (kb) {
            virtual_alloc2 = reinterpret_cast<FnVirtualAlloc2>(GetProcAddress(kb, "VirtualAlloc2"));
            map_view_of_file3 = reinterpret_cast<FnMapViewOfFile3>(GetProcAddress(kb, "MapViewOfFile3"));
            unmap_view_of_file2 = reinterpret_cast<FnUnmapViewOfFile2>(GetProcAddress(kb, "UnmapViewOfFile2"));
        }
    }

    bool Available() const {
        return virtual_alloc2 != nullptr && map_view_of_file3 != nullptr && unmap_view_of_file2 != nullptr;
    }
};

const Win32PlaceholderApi& PlaceholderApi() {
    static const Win32PlaceholderApi api;
    return api;
}

DWORD ToWin32Protect(MemoryProtection prot) {
    switch (prot) {
        case MemoryProtection::NoAccess: return PAGE_NOACCESS;
        case MemoryProtection::ReadOnly: return PAGE_READONLY;
        case MemoryProtection::ReadWrite: return PAGE_READWRITE;
        case MemoryProtection::WriteCombined: return PAGE_READWRITE | PAGE_WRITECOMBINE;
        default: return PAGE_READWRITE;
    }
}

bool IsPathOnCDriveW(const std::wstring& path) noexcept {
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

std::wstring ResolveXMemorySparseTempDir() {
    wchar_t env_buf[MAX_PATH * 2]{};
    const DWORD env_len = GetEnvironmentVariableW(
        L"OPENSCARLETT_TMP_DIR", env_buf, static_cast<DWORD>(sizeof(env_buf) / sizeof(wchar_t)));
    if (env_len > 0u && env_len < (sizeof(env_buf) / sizeof(wchar_t))) {
        std::wstring custom(env_buf, env_len);
        if (!IsPathOnCDriveW(custom)) {
            CreateDirectoryW(custom.c_str(), nullptr);
            return custom;
        }
    }

    const UINT e_type = GetDriveTypeW(L"E:\\");
    if (e_type != DRIVE_NO_ROOT_DIR && e_type != DRIVE_UNKNOWN) {
        CreateDirectoryW(L"E:\\OpenScarlett", nullptr);
        const std::wstring e_tmp = L"E:\\OpenScarlett\\.tmp";
        if (CreateDirectoryW(e_tmp.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
            return e_tmp;
        }
    }

    HMODULE self_mod = nullptr;
    wchar_t dir_buf[MAX_PATH * 2]{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&UnifiedMemoryManager::SupportsOsPlaceholders),
            &self_mod) &&
        self_mod) {
        const DWORD n = GetModuleFileNameW(
            self_mod, dir_buf, static_cast<DWORD>(sizeof(dir_buf) / sizeof(wchar_t)));
        if (n > 0u && n < (sizeof(dir_buf) / sizeof(wchar_t))) {
            std::wstring p(dir_buf, n);
            const size_t sl = p.find_last_of(L"\\/");
            if (sl != std::wstring::npos) {
                std::wstring candidate = p.substr(0u, sl) + L"\\.tmp";
                if (!IsPathOnCDriveW(candidate)) {
                    CreateDirectoryW(candidate.c_str(), nullptr);
                    return candidate;
                }
            }
        }
    }
    return {};
}

bool ShouldForceSparseSection(size_t aligned_size) noexcept {
    char force_buf[16]{};
    const DWORD flen = GetEnvironmentVariableA(
        "OPENSCARLETT_FORCE_SPARSE_COMMIT", force_buf, static_cast<DWORD>(sizeof(force_buf)));
    if (flen > 0u && force_buf[0] != '0') {
        if (force_buf[0] == 'l' || force_buf[0] == 'L') {
            if (aligned_size >= (4ull * 1024ull * 1024ull)) {
                return true;
            }
        } else {
            return true;
        }
    }
    if (aligned_size >= (32ull * 1024ull * 1024ull)) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms) &&
            ms.ullAvailPageFile <
                static_cast<DWORDLONG>(aligned_size) + (2ull * 1024ull * 1024ull * 1024ull)) {
            return true;
        }
    }
    return false;
}

HANDLE CreateSparseBackedSection(size_t aligned_size) noexcept {
    const std::wstring backing_dir = ResolveXMemorySparseTempDir();
    if (backing_dir.empty()) {
        return nullptr;
    }
    static std::atomic<uint64_t> s_phys_sparse_seq{1};
    const uint64_t seq = s_phys_sparse_seq.fetch_add(1u, std::memory_order_relaxed);
    wchar_t file_path[MAX_PATH * 2]{};
    std::swprintf(file_path,
                  sizeof(file_path) / sizeof(wchar_t),
                  L"%ls\\xphys_%lu_%llu.bin",
                  backing_dir.c_str(),
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(seq));
    HANDLE hf = CreateFileW(
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
    (void)DeviceIoControl(hf, kFsctlSetSparse, nullptr, 0, nullptr, 0, &bytes_ret, nullptr);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(aligned_size);
    HANDLE section = nullptr;
    if (SetFilePointerEx(hf, li, nullptr, FILE_BEGIN) && SetEndOfFile(hf)) {
        const DWORD hi = static_cast<DWORD>((static_cast<uint64_t>(aligned_size) >> 32) & 0xFFFFFFFFull);
        const DWORD lo = static_cast<DWORD>(static_cast<uint64_t>(aligned_size) & 0xFFFFFFFFull);
        section = CreateFileMappingW(hf, nullptr, PAGE_READWRITE, hi, lo, nullptr);
    }
    CloseHandle(hf);
    return section;
}
#endif

void* AllocAligned(size_t size, size_t alignment, bool& out_zeroed_by_os) {
    out_zeroed_by_os = false;
#if defined(_WIN32)
    const size_t v_size = AlignUp(size, 4096u);
    void* v_ptr = nullptr;
    if (alignment > 65536u && PlaceholderApi().virtual_alloc2 != nullptr) {
        OsMemAddressRequirements addr_req{};
        addr_req.lowest_starting_address = nullptr;
        addr_req.highest_ending_address = nullptr;
        addr_req.alignment = alignment;

        OsMemExtendedParameter ext{};
        ext.type_and_reserved = 1ull;  // MemExtendedParameterAddressRequirements
        ext.value.pointer = &addr_req;

        v_ptr = PlaceholderApi().virtual_alloc2(
            GetCurrentProcess(),
            nullptr,
            v_size,
            MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
            PAGE_READWRITE,
            &ext,
            1u);
        if (!v_ptr) {
            v_ptr = PlaceholderApi().virtual_alloc2(
                GetCurrentProcess(),
                nullptr,
                v_size,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE,
                &ext,
                1u);
        }
    } else if (alignment <= 65536u) {
        v_ptr = VirtualAlloc(
            nullptr, v_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (!v_ptr) {
            v_ptr = VirtualAlloc(nullptr, v_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
    }
    if (v_ptr) {
        (void)ResetWriteWatch(v_ptr, v_size);
        {
            std::lock_guard<std::mutex> lk(VirtualBlockSetMutex());
            VirtualBlockSet().insert(v_ptr);
        }
        out_zeroed_by_os = true;
        return v_ptr;
    }
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void FreeAligned(void* ptr) {
    if (!ptr) return;
#if defined(_WIN32)
    bool was_virtual = false;
    {
        std::lock_guard<std::mutex> lk(VirtualBlockSetMutex());
        auto it = VirtualBlockSet().find(ptr);
        if (it != VirtualBlockSet().end()) {
            VirtualBlockSet().erase(it);
            was_virtual = true;
        }
    }
    if (was_virtual) {
        VirtualFree(ptr, 0, MEM_RELEASE);
        return;
    }
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace

UnifiedMemoryManager::UnifiedMemoryManager() = default;

UnifiedMemoryManager::~UnifiedMemoryManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [ptr, alias] : aliases_) {
#if defined(_WIN32)
        if (alias.from_placeholder && PlaceholderApi().Available()) {
            PlaceholderApi().unmap_view_of_file2(
                GetCurrentProcess(), reinterpret_cast<void*>(ptr), kMemPreservePlaceholder);
        } else {
            UnmapViewOfFile(reinterpret_cast<void*>(ptr));
        }
#else
        (void)ptr;
        (void)alias;
#endif
    }
    aliases_.clear();

    for (auto& [base, res] : placeholders_) {
#if defined(_WIN32)
        (void)base;
        for (const auto& span : res.spans) {
            VirtualFree(reinterpret_cast<void*>(span.base), 0, MEM_RELEASE);
        }
#else
        (void)base;
        (void)res;
#endif
    }
    placeholders_.clear();

    for (auto& [id, pool] : physical_pools_) {
#if defined(_WIN32)
        (void)id;
        if (pool.host_rw_view) {
            UnmapViewOfFile(pool.host_rw_view);
            pool.host_rw_view = nullptr;
        }
        if (pool.os_handle) {
            CloseHandle(static_cast<HANDLE>(pool.os_handle));
            pool.os_handle = nullptr;
        }
#else
        (void)id;
        (void)pool;
#endif
    }
    physical_pools_.clear();

    for (auto& [va, block] : blocks_) {
        (void)va;
        FreeAligned(block.host_ptr);
    }
    blocks_.clear();
}

Status UnifiedMemoryManager::Allocate(size_t size,
                                      MemoryPageSize page_size,
                                      bool gpu_visible,
                                      bool write_combined,
                                      UnifiedMemoryBlock& out_block) {
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Allocation size must be > 0");
    }

    const size_t alignment = static_cast<size_t>(page_size);
    const size_t aligned_size = AlignUp(size, alignment);
    bool zeroed_by_os = false;
    void* mem = AllocAligned(aligned_size, alignment, zeroed_by_os);
    if (!mem) {
        return Status::Error(StatusCode::OutOfMemory, "Unified memory allocation failed");
    }
    if (!zeroed_by_os) {
        std::memset(mem, 0, aligned_size);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const GpuVirtualAddress va = AlignUp(static_cast<size_t>(next_gpu_va_), alignment);
    next_gpu_va_ = va + aligned_size;

    UnifiedMemoryBlock block{};
    block.host_ptr = mem;
    block.gpu_va = va;
    block.size = aligned_size;
    block.page_size = page_size;
    block.gpu_visible = gpu_visible;
    block.write_combined = write_combined;

    blocks_[va] = block;
    total_allocated_bytes_ += aligned_size;
    out_block = block;
    return Status::Ok();
}

Status UnifiedMemoryManager::Free(GpuVirtualAddress gpu_va) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = blocks_.find(gpu_va);
    if (it == blocks_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown GPU virtual address");
    }
    FreeAligned(it->second.host_ptr);
    total_allocated_bytes_ -= it->second.size;
    blocks_.erase(it);
    return Status::Ok();
}

bool UnifiedMemoryManager::LookupByGpuVa(GpuVirtualAddress gpu_va,
                                         UnifiedMemoryBlock& out_block) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [base_va, block] : blocks_) {
        if (gpu_va >= base_va && gpu_va < base_va + block.size) {
            out_block = block;
            return true;
        }
    }
    return false;
}

size_t UnifiedMemoryManager::TotalAllocatedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_allocated_bytes_;
}

size_t UnifiedMemoryManager::ActiveAllocationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

bool UnifiedMemoryManager::SupportsOsPlaceholders() {
#if defined(_WIN32)
    return PlaceholderApi().Available();
#else
    return false;
#endif
}

Status UnifiedMemoryManager::AllocatePhysicalPages(size_t size,
                                                   MemoryPageSize page_size,
                                                   uint32_t flags,
                                                   PhysicalAllocation& out_phys) {
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Physical page allocation size must be > 0");
    }
    const size_t min_gran = std::max<size_t>(static_cast<size_t>(page_size), 65536u);
    const size_t aligned_size = AlignUp(size, min_gran);

    PhysicalPoolEntry entry{};
    entry.info.size = aligned_size;
    entry.info.page_size = page_size;
    entry.info.flags = flags;

#if defined(_WIN32)
    const DWORD hi = static_cast<DWORD>((static_cast<uint64_t>(aligned_size) >> 32) & 0xFFFFFFFFull);
    const DWORD lo = static_cast<DWORD>(static_cast<uint64_t>(aligned_size) & 0xFFFFFFFFull);
    const bool prefer_sparse = ShouldForceSparseSection(aligned_size);
    HANDLE section = prefer_sparse
                         ? nullptr
                         : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, nullptr);
    if (!section) {
        // Fallback when Windows pagefile commit limit is tight on 8-16 GiB hosts:
        // back the section with an NTFS sparse file on E:\OpenScarlett\.tmp (never C:\)
        // so MapViewOfFile3(MEM_REPLACE_PLACEHOLDER) works natively with zero commit charge
        // and zero disk blocks for untouched pages.
        section = CreateSparseBackedSection(aligned_size);
    }
    if (!section && prefer_sparse) {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, nullptr);
    }
    if (!section) {
        return Status::Error(StatusCode::OutOfMemory, "CreateFileMappingW failed for physical page pool");
    }
    entry.os_handle = static_cast<void*>(section);
#else
    entry.fallback_storage.assign(aligned_size, 0u);
#endif

    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t id = next_physical_id_++;
    entry.info.physical_id = id;
    out_phys = entry.info;
    physical_pools_.emplace(id, std::move(entry));
    total_allocated_bytes_ += aligned_size;
    return Status::Ok();
}

Status UnifiedMemoryManager::FreePhysicalPages(uint64_t physical_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = physical_pools_.find(physical_id);
    if (it == physical_pools_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical page pool id");
    }
    for (const auto& [ptr, alias] : aliases_) {
        (void)ptr;
        if (alias.physical_id == physical_id) {
            return Status::Error(StatusCode::InvalidArgument, "Physical pool still has active virtual aliases");
        }
    }
#if defined(_WIN32)
    if (it->second.host_rw_view) {
        UnmapViewOfFile(it->second.host_rw_view);
        it->second.host_rw_view = nullptr;
    }
    if (it->second.os_handle) {
        CloseHandle(static_cast<HANDLE>(it->second.os_handle));
        it->second.os_handle = nullptr;
    }
#endif
    total_allocated_bytes_ =
        total_allocated_bytes_ >= it->second.info.size ? (total_allocated_bytes_ - it->second.info.size) : 0;
    physical_pools_.erase(it);
    return Status::Ok();
}

Status UnifiedMemoryManager::ResolvePhysicalViewPtr(uint64_t physical_id,
                                                    size_t physical_offset,
                                                    size_t size,
                                                    uint8_t*& out_ptr) {
    out_ptr = nullptr;
    if (size == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Physical view resolve size must be > 0");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto pit = physical_pools_.find(physical_id);
    if (pit == physical_pools_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical pool id");
    }
    auto& pool = pit->second;
    if (physical_offset > pool.info.size || size > pool.info.size - physical_offset) {
        return Status::Error(StatusCode::InvalidArgument, "Physical view range exceeds pool size");
    }
#if defined(_WIN32)
    if (!pool.host_rw_view) {
        if (!pool.os_handle) {
            return Status::Error(StatusCode::DeviceError, "Physical pool missing OS section handle");
        }
        pool.host_rw_view = ::MapViewOfFile(
            static_cast<HANDLE>(pool.os_handle), FILE_MAP_READ | FILE_MAP_WRITE, 0u, 0u, 0u);
        if (!pool.host_rw_view) {
            return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile failed for persistent physical pool view");
        }
    }
    out_ptr = static_cast<uint8_t*>(pool.host_rw_view) + physical_offset;
    return Status::Ok();
#else
    if (pool.fallback_storage.size() < pool.info.size) {
        return Status::Error(StatusCode::DeviceError, "Fallback physical storage uninitialized");
    }
    out_ptr = pool.fallback_storage.data() + physical_offset;
    return Status::Ok();
#endif
}

Status UnifiedMemoryManager::ReserveVirtualPlaceholder(size_t size,
                                                       void*& out_base,
                                                       MemoryPageSize granule) {
    out_base = nullptr;
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Placeholder reservation size must be > 0");
    }
    const size_t gran = std::max<size_t>(static_cast<size_t>(granule), 65536u);
    const size_t aligned_size = AlignUp(size, gran);
#if defined(_WIN32)
    if (!PlaceholderApi().Available()) {
        return Status::Error(StatusCode::NotImplemented, "VirtualAlloc2 placeholder API unavailable");
    }

    constexpr uintptr_t kScarlettMaxVa34Bit = (1ull << 34) - 1ull;  // 16 GiB (34-bit VA for sdsm 22-bit 4KB PFN tables)
    constexpr uintptr_t kScarlettMaxVa40Bit = (1ull << 40) - 1ull;  // 1 TiB (40-bit Scarlett GPU/CPU VA limit)
    constexpr size_t kPrefer34BitMaxBytes = static_cast<size_t>(2ull * 1024ull * 1024ull * 1024ull);
    void* base = nullptr;
    {
        OsMemAddressRequirements addr_req{};
        addr_req.lowest_starting_address = nullptr;
        addr_req.alignment = (gran > 65536u) ? gran : 0u;

        OsMemExtendedParameter ext{};
        ext.type_and_reserved = 1ull;  // MemExtendedParameterAddressRequirements
        ext.value.pointer = &addr_req;

        if (aligned_size <= kPrefer34BitMaxBytes) {
            addr_req.highest_ending_address = reinterpret_cast<void*>(kScarlettMaxVa34Bit);
            base = PlaceholderApi().virtual_alloc2(GetCurrentProcess(),
                                                   nullptr,
                                                   aligned_size,
                                                   MEM_RESERVE | kMemReservePlaceholder,
                                                   PAGE_NOACCESS,
                                                   &ext,
                                                   1u);
        }
        if (!base) {
            addr_req.highest_ending_address = reinterpret_cast<void*>(kScarlettMaxVa40Bit);
            base = PlaceholderApi().virtual_alloc2(GetCurrentProcess(),
                                                   nullptr,
                                                   aligned_size,
                                                   MEM_RESERVE | kMemReservePlaceholder,
                                                   PAGE_NOACCESS,
                                                   &ext,
                                                   1u);
        }
        if (!base) {
            // Fallback without 40-bit cap if host <1 TiB VA space is fragmented.
            if (gran > 65536u) {
                addr_req.highest_ending_address = nullptr;
                base = PlaceholderApi().virtual_alloc2(GetCurrentProcess(),
                                                       nullptr,
                                                       aligned_size,
                                                       MEM_RESERVE | kMemReservePlaceholder,
                                                       PAGE_NOACCESS,
                                                       &ext,
                                                       1u);
            } else {
                base = PlaceholderApi().virtual_alloc2(GetCurrentProcess(),
                                                       nullptr,
                                                       aligned_size,
                                                       MEM_RESERVE | kMemReservePlaceholder,
                                                       PAGE_NOACCESS,
                                                       nullptr,
                                                       0u);
            }
        }
    }
    if (!base) {
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc2 placeholder reservation failed");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    PlaceholderReservation res{};
    res.base = reinterpret_cast<uintptr_t>(base);
    res.total_size = aligned_size;
    res.granule = gran;
    res.spans.push_back({res.base, aligned_size, false});
    placeholders_.emplace(res.base, std::move(res));
    out_base = base;
    return Status::Ok();
#else
    (void)aligned_size;
    return Status::Error(StatusCode::NotImplemented, "OS placeholders require Windows 10+");
#endif
}

Status UnifiedMemoryManager::ReserveVirtualPlaceholderAt(void* requested_base,
                                                         size_t size,
                                                         void*& out_base,
                                                         MemoryPageSize granule) {
    out_base = nullptr;
    if (requested_base == nullptr) {
        return ReserveVirtualPlaceholder(size, out_base, granule);
    }
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Placeholder reservation size must be > 0");
    }
    const size_t gran = std::max<size_t>(static_cast<size_t>(granule), 65536u);
    const uintptr_t req_addr = reinterpret_cast<uintptr_t>(requested_base);
    if ((req_addr & (gran - 1u)) != 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Requested placeholder base is not granule-aligned");
    }
    const size_t aligned_size = AlignUp(size, gran);
#if defined(_WIN32)
    if (!PlaceholderApi().Available()) {
        return Status::Error(StatusCode::NotImplemented, "VirtualAlloc2 placeholder API unavailable");
    }
    void* base = PlaceholderApi().virtual_alloc2(GetCurrentProcess(),
                                                 requested_base,
                                                 aligned_size,
                                                 MEM_RESERVE | kMemReservePlaceholder,
                                                 PAGE_NOACCESS,
                                                 nullptr,
                                                 0u);
    if (!base) {
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc2 fixed-base placeholder reservation failed");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    PlaceholderReservation res{};
    res.base = reinterpret_cast<uintptr_t>(base);
    res.total_size = aligned_size;
    res.granule = gran;
    res.spans.push_back({res.base, aligned_size, false});
    placeholders_.emplace(res.base, std::move(res));
    out_base = base;
    return Status::Ok();
#else
    (void)aligned_size;
    return Status::Error(StatusCode::NotImplemented, "OS placeholders require Windows 10+");
#endif
}

Status UnifiedMemoryManager::PromoteReservedRangeToPlaceholder(void* target_addr, size_t min_bytes) {
    if (!target_addr || min_bytes == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid target range for placeholder promotion");
    }
#if defined(_WIN32)
    if (!PlaceholderApi().Available()) {
        return Status::Error(StatusCode::NotImplemented, "VirtualAlloc2 placeholder API unavailable");
    }
    const uintptr_t req_start = reinterpret_cast<uintptr_t>(target_addr);
    const size_t aligned_min = AlignUp(min_bytes, 65536u);
    const uintptr_t req_end = req_start + aligned_min;
    if ((req_start & 0xFFFFu) != 0u || req_end < req_start) {
        return Status::Error(StatusCode::InvalidArgument, "Target address must be 64 KiB aligned");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [rbase, res] : placeholders_) {
        (void)rbase;
        if (req_start >= res.base && req_end <= res.base + res.total_size) {
            return Status::Ok();
        }
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(target_addr, &mbi, sizeof(mbi)) == 0u ||
        mbi.State != MEM_RESERVE || mbi.Type != MEM_PRIVATE || mbi.AllocationBase == nullptr) {
        return Status::Error(StatusCode::InvalidArgument, "Target range is not a private MEM_RESERVE allocation");
    }

    const uintptr_t alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    if ((alloc_base & 0xFFFFu) != 0u) {
        return Status::Error(StatusCode::InvalidArgument, "AllocationBase is not 64 KiB aligned");
    }

    uintptr_t cur = alloc_base;
    while (true) {
        MEMORY_BASIC_INFORMATION seg{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &seg, sizeof(seg)) == 0u ||
            seg.AllocationBase != mbi.AllocationBase || seg.RegionSize == 0u) {
            break;
        }
        if (seg.State != MEM_RESERVE || seg.Type != MEM_PRIVATE) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "Cannot promote partially committed MEM_RESERVE allocation");
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(seg.BaseAddress) + seg.RegionSize;
        if (next <= cur) break;
        cur = next;
    }

    const size_t total_size = AlignUp(cur - alloc_base, 65536u);
    if (total_size == 0u || req_end > alloc_base + total_size) {
        return Status::Error(StatusCode::InvalidArgument, "Target range exceeds MEM_RESERVE allocation");
    }

    if (!VirtualFree(mbi.AllocationBase, 0u, MEM_RELEASE)) {
        return Status::Error(StatusCode::DeviceError, "VirtualFree failed during placeholder promotion");
    }
    void* promoted = PlaceholderApi().virtual_alloc2(GetCurrentProcess(),
                                                     mbi.AllocationBase,
                                                     total_size,
                                                     MEM_RESERVE | kMemReservePlaceholder,
                                                     PAGE_NOACCESS,
                                                     nullptr,
                                                     0u);
    if (!promoted) {
        (void)VirtualAlloc(mbi.AllocationBase, total_size, MEM_RESERVE, PAGE_NOACCESS);
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc2 failed during placeholder promotion");
    }

    PlaceholderReservation res{};
    res.base = reinterpret_cast<uintptr_t>(promoted);
    res.total_size = total_size;
    res.granule = 65536u;
    res.spans.push_back({res.base, total_size, false});
    placeholders_.emplace(res.base, std::move(res));
    return Status::Ok();
#else
    return Status::Error(StatusCode::NotImplemented, "OS placeholders require Windows 10+");
#endif
}

Status UnifiedMemoryManager::ReleaseVirtualPlaceholder(void* base) {
    if (!base) {
        return Status::Error(StatusCode::InvalidArgument, "Null placeholder base");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t key = reinterpret_cast<uintptr_t>(base);
    auto it = placeholders_.find(key);
    if (it == placeholders_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown placeholder reservation");
    }
    for (const auto& span : it->second.spans) {
        if (span.mapped) {
            return Status::Error(StatusCode::InvalidArgument, "Placeholder reservation still has mapped aliases");
        }
    }
#if defined(_WIN32)
    // Coalesce any remaining split unmapped spans first, then release cleanly.
    if (it->second.spans.size() > 1u) {
        CoalesceAroundLocked(it->second, 0u);
    }
    for (const auto& span : it->second.spans) {
        if (!VirtualFree(reinterpret_cast<void*>(span.base), 0, MEM_RELEASE)) {
            return Status::Error(StatusCode::DeviceError, "VirtualFree failed while releasing placeholder");
        }
    }
#endif
    placeholders_.erase(it);
    return Status::Ok();
}

Status UnifiedMemoryManager::SplitSpanLocked(PlaceholderReservation& res,
                                             size_t span_idx,
                                             uintptr_t req_start,
                                             size_t aligned_size,
                                             size_t& out_target_idx) {
    const PlaceholderSpan orig = res.spans[span_idx];
    const uintptr_t orig_end = orig.base + orig.size;
    const uintptr_t req_end = req_start + aligned_size;

    if (orig.base == req_start && orig_end == req_end) {
        out_target_idx = span_idx;
        return Status::Ok();
    }

#if defined(_WIN32)
    // Reconcile against actual OS placeholder boundaries in case guest code split the
    // placeholder directly via Win32 VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER).
    MEMORY_BASIC_INFORMATION mbi{};
    const bool have_mbi =
        (VirtualQuery(reinterpret_cast<LPCVOID>(req_start), &mbi, sizeof(mbi)) != 0u) &&
        (mbi.State == MEM_RESERVE) && (mbi.Type == MEM_PRIVATE) && (mbi.AllocationBase != nullptr);
    const uintptr_t os_base = have_mbi ? reinterpret_cast<uintptr_t>(mbi.AllocationBase) : orig.base;
    const uintptr_t os_end =
        have_mbi ? (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) : orig_end;

    if (have_mbi && os_base == req_start && os_end == req_end) {
        // Already split to exact [req_start, req_end) at the OS level.
    } else if (have_mbi && os_base == req_start && os_end > req_end) {
        if (!VirtualFree(reinterpret_cast<void*>(req_start), aligned_size, MEM_RELEASE | kMemPreservePlaceholder)) {
            return Status::Error(StatusCode::OutOfMemory, "Failed trailing split of pre-split virtual placeholder");
        }
    } else if (have_mbi && os_base < req_start && os_end == req_end) {
        if (!VirtualFree(reinterpret_cast<void*>(req_start), aligned_size, MEM_RELEASE | kMemPreservePlaceholder)) {
            return Status::Error(StatusCode::OutOfMemory, "Failed edge split of pre-split virtual placeholder");
        }
    } else if (req_start > orig.base && req_end < orig_end) {
        // Windows VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) requires the split
        // region to touch either the beginning or the end of the placeholder.
        // A middle split therefore uses two sequential edge splits.
        const size_t lead_size = req_start - orig.base;
        if (!VirtualFree(reinterpret_cast<void*>(orig.base), lead_size, MEM_RELEASE | kMemPreservePlaceholder)) {
            return Status::Error(StatusCode::OutOfMemory, "Failed leading split of virtual placeholder");
        }
        if (!VirtualFree(reinterpret_cast<void*>(req_start), aligned_size, MEM_RELEASE | kMemPreservePlaceholder)) {
            // Re-coalesce the first split so OS state remains consistent with tracker.
            VirtualFree(reinterpret_cast<void*>(orig.base), orig.size, MEM_RELEASE | kMemCoalescePlaceholders);
            return Status::Error(StatusCode::OutOfMemory, "Failed trailing split of virtual placeholder");
        }
    } else {
        if (!VirtualFree(reinterpret_cast<void*>(req_start), aligned_size, MEM_RELEASE | kMemPreservePlaceholder)) {
            return Status::Error(StatusCode::OutOfMemory, "Failed to split virtual memory placeholder");
        }
    }
#endif

    std::vector<PlaceholderSpan> replacement;
    if (req_start > orig.base) {
        replacement.push_back({orig.base, req_start - orig.base, false});
    }
    replacement.push_back({req_start, aligned_size, false});
    if (req_end < orig_end) {
        replacement.push_back({req_end, orig_end - req_end, false});
    }

    res.spans.erase(res.spans.begin() + static_cast<std::ptrdiff_t>(span_idx));
    res.spans.insert(res.spans.begin() + static_cast<std::ptrdiff_t>(span_idx),
                     replacement.begin(),
                     replacement.end());

    for (size_t i = span_idx; i < res.spans.size(); ++i) {
        if (res.spans[i].base == req_start && res.spans[i].size == aligned_size) {
            out_target_idx = i;
            return Status::Ok();
        }
    }
    return Status::Error(StatusCode::DeviceError, "Internal error locating split placeholder span");
}

void UnifiedMemoryManager::CoalesceAroundLocked(PlaceholderReservation& res, size_t span_idx) {
    if (span_idx >= res.spans.size() || res.spans[span_idx].mapped) {
        return;
    }

    size_t first = span_idx;
    while (first > 0u && !res.spans[first - 1u].mapped) {
        --first;
    }
    size_t last = span_idx;
    while (last + 1u < res.spans.size() && !res.spans[last + 1u].mapped) {
        ++last;
    }
    if (last <= first) {
        return;
    }

    const uintptr_t coalesce_base = res.spans[first].base;
    const uintptr_t coalesce_end = res.spans[last].base + res.spans[last].size;
    const size_t coalesce_size = coalesce_end - coalesce_base;

#if defined(_WIN32)
    if (!VirtualFree(reinterpret_cast<void*>(coalesce_base),
                     coalesce_size,
                     MEM_RELEASE | kMemCoalescePlaceholders)) {
        return;
    }
#endif

    res.spans[first] = PlaceholderSpan{coalesce_base, coalesce_size, false};
    res.spans.erase(res.spans.begin() + static_cast<std::ptrdiff_t>(first + 1u),
                    res.spans.begin() + static_cast<std::ptrdiff_t>(last + 1u));
}

Status UnifiedMemoryManager::SplitVirtualPlaceholder(void* split_addr, size_t split_size) {
    if (!split_addr || split_size == 0 || (reinterpret_cast<uintptr_t>(split_addr) & 0xFFFFu) != 0u ||
        (split_size & 0xFFFFu) != 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Placeholder split address and size must be non-null and 64 KiB aligned");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t req_start = reinterpret_cast<uintptr_t>(split_addr);
    const uintptr_t req_end = req_start + split_size;

    for (auto& [rbase, res] : placeholders_) {
        (void)rbase;
        if (req_start >= res.base && req_end <= res.base + res.total_size) {
            for (size_t i = 0; i < res.spans.size(); ++i) {
                const auto& sp = res.spans[i];
                if (!sp.mapped && req_start >= sp.base && req_end <= sp.base + sp.size) {
                    size_t target_idx = 0;
                    return SplitSpanLocked(res, i, req_start, split_size, target_idx);
                }
            }
            return Status::Error(StatusCode::InvalidArgument,
                                 "Split range is not contained within a single unmapped placeholder span");
        }
    }
    return Status::Error(StatusCode::NotFound, "Address is not inside a tracked placeholder reservation");
}

Status UnifiedMemoryManager::CoalesceVirtualPlaceholders(void* base_addr, size_t total_size) {
    if (!base_addr || total_size == 0 || (reinterpret_cast<uintptr_t>(base_addr) & 0xFFFFu) != 0u ||
        (total_size & 0xFFFFu) != 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Placeholder coalesce address and size must be non-null and 64 KiB aligned");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t req_start = reinterpret_cast<uintptr_t>(base_addr);
    const uintptr_t req_end = req_start + total_size;

    for (auto& [rbase, res] : placeholders_) {
        (void)rbase;
        if (req_start >= res.base && req_end <= res.base + res.total_size) {
            size_t first = res.spans.size();
            size_t last = res.spans.size();
            for (size_t i = 0; i < res.spans.size(); ++i) {
                if (res.spans[i].base == req_start) {
                    first = i;
                }
                if (res.spans[i].base + res.spans[i].size == req_end) {
                    last = i;
                }
            }
            if (first == res.spans.size() || last == res.spans.size() || first > last) {
                return Status::Error(StatusCode::InvalidArgument,
                                     "Coalesce range does not match exact placeholder span boundaries");
            }
            for (size_t i = first; i <= last; ++i) {
                if (res.spans[i].mapped) {
                    return Status::Error(StatusCode::InvalidArgument,
                                         "Cannot coalesce mapped placeholder spans");
                }
            }
            if (first == last) {
                return Status::Ok();
            }
#if defined(_WIN32)
            if (!VirtualFree(base_addr, total_size, MEM_RELEASE | kMemCoalescePlaceholders)) {
                return Status::Error(StatusCode::DeviceError,
                                     "VirtualFree(MEM_COALESCE_PLACEHOLDERS) failed");
            }
#endif
            res.spans[first] = PlaceholderSpan{req_start, total_size, false};
            res.spans.erase(res.spans.begin() + static_cast<std::ptrdiff_t>(first + 1u),
                            res.spans.begin() + static_cast<std::ptrdiff_t>(last + 1u));
            return Status::Ok();
        }
    }
    return Status::Error(StatusCode::NotFound, "Address is not inside a tracked placeholder reservation");
}

size_t UnifiedMemoryManager::PlaceholderSpanCount(const void* base) const {
    if (!base) {
        return 0u;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = placeholders_.find(reinterpret_cast<uintptr_t>(base));
    if (it == placeholders_.end()) {
        return 0u;
    }
    return it->second.spans.size();
}

Status UnifiedMemoryManager::MapPhysicalAlias(uint64_t physical_id,
                                              size_t physical_offset,
                                              size_t size,
                                              MemoryProtection protection,
                                              uint32_t flags,
                                              void* placeholder_addr,
                                              UnifiedMemoryAlias& out_alias) {
    if (size == 0 || (physical_offset & 0xFFFFu) != 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Alias size must be > 0 and offset 64 KiB aligned");
    }
    const size_t aligned_size = AlignUp(size, 65536u);

    std::lock_guard<std::mutex> lock(mutex_);
    auto pit = physical_pools_.find(physical_id);
    if (pit == physical_pools_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical pool id");
    }
    auto& pool = pit->second;
    if (physical_offset > pool.info.size || aligned_size > pool.info.size - physical_offset) {
        return Status::Error(StatusCode::InvalidArgument, "Physical alias range exceeds pool size");
    }

    void* mapped_ptr = nullptr;
    bool used_placeholder = false;

#if defined(_WIN32)
    HANDLE section = static_cast<HANDLE>(pool.os_handle);
    if (placeholder_addr != nullptr) {
        if (!PlaceholderApi().Available()) {
            return Status::Error(StatusCode::NotImplemented, "MapViewOfFile3 unavailable");
        }
        const uintptr_t req_start = reinterpret_cast<uintptr_t>(placeholder_addr);
        const uintptr_t req_end = req_start + aligned_size;
        if ((req_start & 0xFFFFu) != 0u) {
            return Status::Error(StatusCode::InvalidArgument, "Placeholder target must be 64 KiB aligned");
        }

        // Locate enclosing placeholder reservation.
        PlaceholderReservation* owner = nullptr;
        for (auto& [rbase, res] : placeholders_) {
            (void)rbase;
            if (req_start >= res.base && req_end <= res.base + res.total_size) {
                owner = &res;
                break;
            }
        }
        if (!owner) {
            return Status::Error(StatusCode::InvalidArgument, "Target address is not inside a reserved placeholder");
        }

        // Ensure a single unmapped span covers [req_start, req_end).
        // If [req_start, req_end) spans multiple adjacent unmapped placeholders, coalesce them first.
        size_t span_idx = owner->spans.size();
        for (size_t i = 0; i < owner->spans.size(); ++i) {
            const auto& sp = owner->spans[i];
            if (!sp.mapped && req_start >= sp.base && req_start < sp.base + sp.size) {
                if (req_end > sp.base + sp.size) {
                    CoalesceAroundLocked(*owner, i);
                    // Re-scan after coalescing.
                    for (size_t j = 0; j < owner->spans.size(); ++j) {
                        const auto& sp2 = owner->spans[j];
                        if (!sp2.mapped && req_start >= sp2.base && req_end <= sp2.base + sp2.size) {
                            span_idx = j;
                            break;
                        }
                    }
                } else {
                    span_idx = i;
                }
                break;
            }
        }
        if (span_idx == owner->spans.size()) {
            return Status::Error(StatusCode::InvalidArgument, "Target address is not an unmapped placeholder range");
        }

        size_t target_idx = 0;
        Status split_st = SplitSpanLocked(*owner, span_idx, req_start, aligned_size, target_idx);
        if (!split_st.ok()) {
            return split_st;
        }

        const DWORD page_prot =
            (protection == MemoryProtection::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE;
        mapped_ptr = PlaceholderApi().map_view_of_file3(
            section,
            GetCurrentProcess(),
            placeholder_addr,
            static_cast<ULONG64>(physical_offset),
            aligned_size,
            kMemReplacePlaceholder,
            page_prot,
            nullptr,
            0u);
        if (!mapped_ptr) {
            // Re-coalesce back cleanly on mapping failure.
            CoalesceAroundLocked(*owner, target_idx);
            return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile3 failed on placeholder");
        }
        owner->spans[target_idx].mapped = true;
        used_placeholder = true;
    } else {
        const DWORD access =
            (protection == MemoryProtection::ReadOnly) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
        const DWORD off_hi = static_cast<DWORD>((static_cast<uint64_t>(physical_offset) >> 32) & 0xFFFFFFFFull);
        const DWORD off_lo = static_cast<DWORD>(static_cast<uint64_t>(physical_offset) & 0xFFFFFFFFull);
        mapped_ptr = MapViewOfFile(section, access, off_hi, off_lo, aligned_size);
        if (!mapped_ptr) {
            return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile failed for physical alias");
        }
    }
    if (protection == MemoryProtection::NoAccess) {
        DWORD old_prot = 0;
        VirtualProtect(mapped_ptr, aligned_size, PAGE_NOACCESS, &old_prot);
    }
#else
    (void)placeholder_addr;
    mapped_ptr = pool.fallback_storage.data() + physical_offset;
#endif

    const GpuVirtualAddress gpu_va = AlignUp(static_cast<size_t>(next_gpu_va_), 65536u);
    next_gpu_va_ = gpu_va + aligned_size;

    UnifiedMemoryAlias alias{};
    alias.host_ptr = mapped_ptr;
    alias.gpu_va = gpu_va;
    alias.physical_id = physical_id;
    alias.physical_offset = physical_offset;
    alias.size = aligned_size;
    alias.protection = protection;
    alias.flags = flags;
    alias.from_placeholder = used_placeholder;

    aliases_[reinterpret_cast<uintptr_t>(mapped_ptr)] = alias;
    out_alias = alias;
    return Status::Ok();
}

Status UnifiedMemoryManager::UnmapPhysicalAlias(void* host_ptr) {
    if (!host_ptr) {
        return Status::Error(StatusCode::InvalidArgument, "Null alias pointer");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t key = reinterpret_cast<uintptr_t>(host_ptr);
    auto it = aliases_.find(key);
    if (it == aliases_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical memory alias");
    }
    const bool from_ph = it->second.from_placeholder;
#if defined(_WIN32)
    BOOL ok = FALSE;
    if (from_ph && PlaceholderApi().Available()) {
        ok = PlaceholderApi().unmap_view_of_file2(GetCurrentProcess(), host_ptr, kMemPreservePlaceholder);
    } else {
        ok = UnmapViewOfFile(host_ptr);
    }
    if (!ok) {
        return Status::Error(StatusCode::DeviceError, "Failed to unmap physical memory alias");
    }
#endif
    if (from_ph) {
        for (auto& [rbase, res] : placeholders_) {
            (void)rbase;
            if (key >= res.base && key < res.base + res.total_size) {
                for (size_t i = 0; i < res.spans.size(); ++i) {
                    if (res.spans[i].base == key) {
                        res.spans[i].mapped = false;
                        CoalesceAroundLocked(res, i);
                        break;
                    }
                }
                break;
            }
        }
    }
    aliases_.erase(it);
    return Status::Ok();
}

Status UnifiedMemoryManager::ProtectAlias(void* host_ptr, size_t size, MemoryProtection new_protection) {
    if (!host_ptr || size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid alias protection arguments");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t key = reinterpret_cast<uintptr_t>(host_ptr);
    auto it = aliases_.find(key);
    if (it == aliases_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical memory alias");
    }
#if defined(_WIN32)
    DWORD old_prot = 0;
    const DWORD win_prot = (new_protection == MemoryProtection::WriteCombined)
                               ? PAGE_READWRITE
                               : ToWin32Protect(new_protection);
    if (!VirtualProtect(host_ptr, size, win_prot, &old_prot)) {
        return Status::Error(StatusCode::DeviceError, "VirtualProtect failed on alias");
    }
#endif
    it->second.protection = new_protection;
    return Status::Ok();
}

bool UnifiedMemoryManager::LookupAliasByHostPtr(const void* host_ptr, UnifiedMemoryAlias& out_alias) const {
    if (!host_ptr) {
        return false;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(host_ptr);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [base, alias] : aliases_) {
        if (addr >= base && addr < base + alias.size) {
            out_alias = alias;
            return true;
        }
    }
    return false;
}

size_t UnifiedMemoryManager::ActivePhysicalPoolCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return physical_pools_.size();
}

size_t UnifiedMemoryManager::ActiveAliasCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aliases_.size();
}

bool UnifiedMemoryManager::LookupBlockByHostPtr(const void* host_ptr,
                                                UnifiedMemoryBlock& out_block) const {
    if (!host_ptr) {
        return false;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(host_ptr);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [va, block] : blocks_) {
        (void)va;
        const uintptr_t base = reinterpret_cast<uintptr_t>(block.host_ptr);
        if (addr >= base && addr < base + block.size) {
            out_block = block;
            return true;
        }
    }
    return false;
}

bool UnifiedMemoryManager::LookupPlaceholderByHostPtr(const void* host_ptr,
                                                      void*& out_base,
                                                      size_t& out_size,
                                                      size_t& out_granule,
                                                      bool& out_mapped) const {
    out_base = nullptr;
    out_size = 0u;
    out_granule = 0u;
    out_mapped = false;
    if (!host_ptr) {
        return false;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(host_ptr);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [rbase, res] : placeholders_) {
        (void)rbase;
        if (addr >= res.base && addr < res.base + res.total_size) {
            for (const auto& sp : res.spans) {
                if (addr >= sp.base && addr < sp.base + sp.size) {
                    out_base = reinterpret_cast<void*>(sp.base);
                    out_size = sp.size;
                    out_granule = res.granule;
                    out_mapped = sp.mapped;
                    return true;
                }
            }
            out_base = reinterpret_cast<void*>(res.base);
            out_size = res.total_size;
            out_granule = res.granule;
            out_mapped = false;
            return true;
        }
    }
    return false;
}

bool DecodeEraAllocationAttributes(uint64_t raw_attributes,
                                   EraAllocationAttributes& out_decoded) noexcept {
    out_decoded = EraAllocationAttributes{};
    out_decoded.raw_attributes = raw_attributes;

    const uint32_t obj_type = static_cast<uint32_t>(raw_attributes & 0x1FFFu);
    const uint32_t heap_tracks = static_cast<uint32_t>((raw_attributes >> 13u) & 0x1u);
    const uint32_t must_succeed = static_cast<uint32_t>((raw_attributes >> 14u) & 0x1u);
    const uint32_t fixed_size = static_cast<uint32_t>((raw_attributes >> 15u) & 0x1u);
    const uint32_t alloc_id = static_cast<uint32_t>((raw_attributes >> 16u) & 0xFFu);
    const uint32_t align_code = static_cast<uint32_t>((raw_attributes >> 24u) & 0xFu);
    const uint32_t mem_type = static_cast<uint32_t>((raw_attributes >> 28u) & 0xFu);
    const uint32_t page_code = static_cast<uint32_t>((raw_attributes >> 32u) & 0x3u);
    const uint32_t zero_init = static_cast<uint32_t>((raw_attributes >> 34u) & 0x1u);
    const uint32_t page_2mb_hint = static_cast<uint32_t>((raw_attributes >> 35u) & 0x1u);

    if (mem_type > 4u) {
        return false;
    }

    uint64_t align_bytes = 16ull;
    if (align_code != 0u) {
        align_bytes = 1ull << (align_code + 1u);  // 1 -> 4B .. 15 -> 64KB
    }

    uint64_t page_bytes = 4096ull;
    uint32_t alloc_flags = 0x40000000u;  // MEM_TITLE
    if (page_2mb_hint != 0u) {
        page_bytes = 2ull * 1024ull * 1024ull;
        alloc_flags |= 0x20000000u;  // MEM_LARGE_PAGES (2 MiB)
    } else if (page_code == 1u) {
        page_bytes = 4096ull;
    } else if (page_code == 2u) {
        page_bytes = 65536ull;
    } else if (page_code == 3u) {
        page_bytes = 4ull * 1024ull * 1024ull;
        alloc_flags |= 0x80000000u | 0x20000000u;  // MEM_4MB_PAGES | MEM_LARGE_PAGES
    } else {
        page_bytes = (mem_type == 0u) ? 4096ull : 65536ull;
    }

    if (page_code != 0u && align_bytes < page_bytes) {
        align_bytes = page_bytes;
    }

    uint32_t protect_flags = 0x00000004u;  // PAGE_READWRITE
    switch (mem_type) {
        case 0u:  // XALLOC_MEMTYPE_HEAP
            break;
        case 1u:  // XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE
            alloc_flags |= 0x10000000u | 0x04000000u;  // MEM_GRAPHICS | MEM_WRITE_COMBINE
            protect_flags |= 0x00000400u;              // PAGE_WRITECOMBINE
            break;
        case 2u:  // XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE_GPU_READONLY
            alloc_flags |= 0x10000000u | 0x04000000u;  // MEM_GRAPHICS | MEM_WRITE_COMBINE
            protect_flags |= 0x00000400u | 0x00020000u;  // PAGE_WRITECOMBINE | PAGE_GPU_READONLY
            break;
        case 3u:  // XALLOC_MEMTYPE_GRAPHICS_CACHEABLE
            alloc_flags |= 0x10000000u;                // MEM_GRAPHICS
            protect_flags |= 0x00010000u;              // PAGE_GPU_COHERENT
            break;
        case 4u:  // XALLOC_MEMTYPE_PHYSICAL
            alloc_flags |= 0x10000000u;                // MEM_GRAPHICS
            break;
        default:
            return false;
    }

    out_decoded.object_type = obj_type;
    out_decoded.heap_tracks_attributes = heap_tracks;
    out_decoded.must_succeed = must_succeed;
    out_decoded.fixed_size = fixed_size;
    out_decoded.allocator_id = alloc_id;
    out_decoded.alignment_code = align_code;
    out_decoded.memory_type = mem_type;
    out_decoded.page_size_code = page_code;
    out_decoded.zero_initialize = zero_init;
    out_decoded.alloc_flags = alloc_flags;
    out_decoded.protect_flags = protect_flags;
    out_decoded.alignment_bytes = align_bytes;
    out_decoded.page_size_bytes = page_bytes;
    return true;
}

size_t UnifiedMemoryManager::ActivePlaceholderCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return placeholders_.size();
}

}  // namespace openscarlett
