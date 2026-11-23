// MIT xmem.dll shim forwarding Xbox memory allocations to kernelx.dll
// while supporting thread-safe XMemSetAllocationHooks, heap validation
// (XMemCheckDefaultHeaps), and xmem.dll call telemetry.

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#if defined(_MSC_VER)
// Export standard unprefixed Xbox xmem.dll symbols pointing to our thin
// forwarding wrappers, while directly importing the underlying implementations
// from kernelx.dll without C++ symbol redeclaration conflicts.
#pragma comment(linker, "/export:XMemAlloc=XMemShim_XMemAlloc")
#pragma comment(linker, "/export:XMemAllocDefault=XMemShim_XMemAllocDefault")
#pragma comment(linker, "/export:XMemAllocAligned=XMemShim_XMemAllocAligned")
#pragma comment(linker, "/export:XMemFree=XMemShim_XMemFree")
#pragma comment(linker, "/export:XMemFreeDefault=XMemShim_XMemFreeDefault")
#pragma comment(linker, "/export:XMemSize=XMemShim_XMemSize")
#pragma comment(linker, "/export:XMemSizeDefault=XMemShim_XMemSizeDefault")
#pragma comment(linker, "/export:XMemSetAllocationHooks=XMemShim_XMemSetAllocationHooks")
#pragma comment(linker, "/export:XMemCheckDefaultHeaps=XMemShim_XMemCheckDefaultHeaps")
#pragma comment(linker, "/export:XMemGetAuxiliaryTitleMemory=XMemShim_XMemGetAuxiliaryTitleMemory")
#pragma comment(linker, "/export:XMemReleaseAuxiliaryTitleMemory=XMemShim_XMemReleaseAuxiliaryTitleMemory")
#pragma comment(linker, "/export:XMemVirtualQuery=XMemShim_XMemVirtualQuery")
#pragma comment(linker, "/export:XMemDecodeAllocationAttributes=XMemShim_XMemDecodeAllocationAttributes")
#pragma comment(linker, "/export:XMemGetAllocationAttributes=XMemShim_XMemGetAllocationAttributes")
#pragma comment(linker, "/export:XMemMapPhysicalPages=XMemShim_XMemMapPhysicalPages")
#pragma comment(linker, "/export:XMemUnmapPhysicalPages=XMemShim_XMemUnmapPhysicalPages")
#pragma comment(linker, "/export:XMemAllocatePhysicalPages=XMemShim_XMemAllocatePhysicalPages")
#pragma comment(linker, "/export:XMemFreePhysicalPages=XMemShim_XMemFreePhysicalPages")
#pragma comment(linker, "/export:XMemVirtualAlloc=XMemShim_XMemVirtualAlloc")
#pragma comment(linker, "/export:XMemGetWorkingSetStatistics=XMemShim_XMemGetWorkingSetStatistics")
#pragma comment(linker, "/export:XMemTransferMemory=XMemShim_XMemTransferMemory")
#pragma comment(linker, "/export:XMemGetTelemetryCallCountForTest=XMemShim_GetTelemetryCallCountForTest")
#endif

namespace {

constexpr const char* kModName = "xmem.dll";
constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);

using XMemAllocHookFn = void*(OPENSCARLETT_KERNELX_CALL*)(size_t, uint64_t);
using XMemFreeHookFn = void(OPENSCARLETT_KERNELX_CALL*)(void*, uint64_t);
using XMemSizeHookFn = size_t(OPENSCARLETT_KERNELX_CALL*)(void*, uint64_t);

struct XMemHookTable {
    std::mutex mutex;
    std::atomic<XMemAllocHookFn> alloc_hook{nullptr};
    std::atomic<XMemFreeHookFn> free_hook{nullptr};
    std::atomic<XMemSizeHookFn> size_hook{nullptr};
};

XMemHookTable& GetHookTable() {
    static XMemHookTable* s_hooks = new XMemHookTable();
    return *s_hooks;
}

inline void RecordOutcome(const char* sym, bool ok) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        ok ? 0 : kHrFail,
        -1);
}

inline void RecordHresult(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
}

}  // namespace

extern "C" {

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XMemGetWorkingSetStatistics(
    uintptr_t arg0,
    void* arg1);

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemSetAllocationHooks(
    XMemAllocHookFn allocHook,
    XMemFreeHookFn freeHook,
    XMemSizeHookFn sizeHook) {
    auto& hooks = GetHookTable();
    {
        std::lock_guard<std::mutex> lk(hooks.mutex);
        hooks.alloc_hook.store(allocHook, std::memory_order_release);
        hooks.free_hook.store(freeHook, std::memory_order_release);
        hooks.size_hook.store(sizeHook, std::memory_order_release);
    }
    RecordOutcome("XMemSetAllocationHooks", true);
    return 0;  // S_OK
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemCheckDefaultHeaps(void) {
    bool ok = true;
#if defined(_WIN32)
    HANDLE proc_heap = ::GetProcessHeap();
    if (proc_heap != nullptr) {
        ok = (::HeapValidate(proc_heap, 0u, nullptr) != FALSE);
    }
#endif
    RecordOutcome("XMemCheckDefaultHeaps", ok);
    return ok ? 1 : 0;
}

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemAlloc(size_t dwSize, uint64_t dwAllocAttributes) {
    const auto hook = GetHookTable().alloc_hook.load(std::memory_order_acquire);
    void* ptr = hook ? hook(dwSize, dwAllocAttributes)
                     : ::XMemAlloc(dwSize, dwAllocAttributes);
    RecordOutcome("XMemAlloc", ptr != nullptr);
    return ptr;
}

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemAllocDefault(size_t dwSize, uint64_t dwAllocAttributes) {
    void* ptr = ::XMemAllocDefault(dwSize, dwAllocAttributes);
    RecordOutcome("XMemAllocDefault", ptr != nullptr);
    return ptr;
}

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemAllocAligned(
    size_t dwSize, size_t alignment, uint64_t dwAllocAttributes) {
    void* ptr = ::XMemAllocAligned(dwSize, alignment, dwAllocAttributes);
    RecordOutcome("XMemAllocAligned", ptr != nullptr);
    return ptr;
}

void OPENSCARLETT_KERNELX_CALL XMemShim_XMemFree(void* pAddress, uint64_t dwAllocAttributes) {
    const auto hook = GetHookTable().free_hook.load(std::memory_order_acquire);
    if (!pAddress && !hook) {
#if defined(_WIN32)
        SetLastError(0u);
#endif
        RecordOutcome("XMemFree", true);
        return;
    }
#if defined(_WIN32)
    SetLastError(0u);
#endif
    if (hook) {
        hook(pAddress, dwAllocAttributes);
    } else {
        ::XMemFree(pAddress, dwAllocAttributes);
    }
#if defined(_WIN32)
    const bool ok = hook ? true : ((pAddress == nullptr) || (GetLastError() == 0u));
#else
    const bool ok = true;
#endif
    RecordOutcome("XMemFree", ok);
}

void OPENSCARLETT_KERNELX_CALL XMemShim_XMemFreeDefault(void* pAddress, uint64_t dwAllocAttributes) {
    if (!pAddress) {
#if defined(_WIN32)
        SetLastError(0u);
#endif
        RecordOutcome("XMemFreeDefault", true);
        return;
    }
#if defined(_WIN32)
    SetLastError(0u);
#endif
    ::XMemFreeDefault(pAddress, dwAllocAttributes);
#if defined(_WIN32)
    const bool ok = (GetLastError() == 0u);
#else
    const bool ok = true;
#endif
    RecordOutcome("XMemFreeDefault", ok);
}

size_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemSize(void* pAddress, uint64_t dwAllocAttributes) {
    const auto hook = GetHookTable().size_hook.load(std::memory_order_acquire);
    const size_t sz = hook ? hook(pAddress, dwAllocAttributes)
                           : ::XMemSize(pAddress, dwAllocAttributes);
    RecordOutcome("XMemSize", pAddress == nullptr || sz > 0u);
    return sz;
}

size_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemSizeDefault(void* pAddress, uint64_t dwAllocAttributes) {
    const size_t sz = ::XMemSizeDefault(pAddress, dwAllocAttributes);
    RecordOutcome("XMemSizeDefault", pAddress == nullptr || sz > 0u);
    return sz;
}

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemGetAuxiliaryTitleMemory(
    size_t requestedSize,
    size_t* outActualSize) {
    void* ptr = ::XMemGetAuxiliaryTitleMemory(requestedSize, outActualSize);
    RecordOutcome("XMemGetAuxiliaryTitleMemory", ptr != nullptr);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemReleaseAuxiliaryTitleMemory(void* baseAddress) {
    const int32_t ok = ::XMemReleaseAuxiliaryTitleMemory(baseAddress);
    RecordOutcome("XMemReleaseAuxiliaryTitleMemory", ok != 0);
    return ok;
}

size_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemVirtualQuery(
    const void* lpAddress,
    void* lpBuffer,
    size_t dwLength) {
    const size_t bytes = ::XMemVirtualQuery(lpAddress, lpBuffer, dwLength);
    RecordOutcome("XMemVirtualQuery", bytes > 0u);
    return bytes;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemDecodeAllocationAttributes(
    uint64_t dwAllocAttributes,
    XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded) {
    const int32_t ok = ::XMemDecodeAllocationAttributes(dwAllocAttributes, outDecoded);
    RecordOutcome("XMemDecodeAllocationAttributes", ok != 0);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemGetAllocationAttributes(
    const void* lpAddress,
    XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded) {
    const int32_t ok = ::XMemGetAllocationAttributes(lpAddress, outDecoded);
    RecordOutcome("XMemGetAllocationAttributes", ok != 0);
    return ok;
}

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemMapPhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages,
    uintptr_t arg2,
    uint32_t arg3,
    const uint64_t* arg4) {
    // Support BOTH:
    //  - Retail Xbox Series X 3-arg ABI: XMemMapPhysicalPages(va, numPages, pPageArray)
    //  - Legacy 5-arg ABI: XMemMapPhysicalPages(va, numPages, flAllocationType, flProtect, pPageArray)
    bool is_three_arg = false;
#if defined(_WIN32)
    if (arg2 >= 0x10000ull) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(arg2), &mbi, sizeof(mbi)) != 0u &&
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0u) {
            is_three_arg = true;
        }
    }
#endif
    void* ptr = nullptr;
    if (is_three_arg) {
        ptr = ::XMemMapPhysicalPages(
            virtualAddress,
            numberOfPages,
            0x10000000u /*MEM_GRAPHICS*/,
            0x04u /*PAGE_READWRITE*/,
            reinterpret_cast<const uint64_t*>(arg2));
    } else {
        ptr = ::XMemMapPhysicalPages(
            virtualAddress,
            numberOfPages,
            static_cast<uint32_t>(arg2),
            arg3,
            arg4);
    }
    RecordOutcome("XMemMapPhysicalPages", ptr != nullptr);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemUnmapPhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages) {
    const int32_t ok = ::XMemUnmapPhysicalPages(virtualAddress, numberOfPages);
    RecordOutcome("XMemUnmapPhysicalPages", ok != 0);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemAllocatePhysicalPages(
    uint32_t flAllocationType,
    uint64_t* pNumberOfPages,
    uint64_t* pPageArray) {
    const int32_t ok = ::AllocateTitlePhysicalPages(
        nullptr, flAllocationType, pNumberOfPages, pPageArray);
    RecordOutcome("XMemAllocatePhysicalPages", ok != 0);
    return ok;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemFreePhysicalPages(
    uint64_t numberOfPages,
    const uint64_t* pPageArray) {
    const int32_t ok = ::FreeTitlePhysicalPages(nullptr, numberOfPages, pPageArray);
    RecordOutcome("XMemFreePhysicalPages", ok != 0);
    return ok;
}

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemVirtualAlloc(
    void* lpAddress,
    size_t dwSize,
    uint32_t flAllocationType,
    uint32_t dwMemoryType,
    uint32_t flProtect) {
    (void)dwMemoryType;
    void* ptr = ::KernelXVirtualAllocFromApp(lpAddress, dwSize, flAllocationType, flProtect);
    static const bool trace_xmem = []() {
#if defined(_WIN32)
        char buf[16] = {};
        const DWORD n = GetEnvironmentVariableA("OPENSCARLETT_TRACE_XMEM", buf, static_cast<DWORD>(sizeof(buf)));
        return n > 0u && buf[0] != '0';
#else
        return false;
#endif
    }();
    if (trace_xmem) {
        std::fprintf(stderr,
            "[OpenScarlett][xmem] XMemVirtualAlloc addr=%p size=0x%zX alloc=0x%X memType=0x%X prot=0x%X -> %p\n",
            lpAddress, dwSize, flAllocationType, dwMemoryType, flProtect, ptr);
        std::fflush(stderr);
    }
    if (!ptr && trace_xmem) {
        std::fprintf(stderr,
            "[OpenScarlett][xmem] XMemVirtualAlloc FAILED addr=%p size=%zu alloc=0x%X memType=0x%X prot=0x%X err=%lu\n",
            lpAddress, dwSize, flAllocationType, dwMemoryType, flProtect,
            static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
    }
    RecordOutcome("XMemVirtualAlloc", ptr != nullptr);
    return ptr;
}

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemGetWorkingSetStatistics(
    uintptr_t arg0,
    void* arg1) {
    const int32_t ok = ::XMemGetWorkingSetStatistics(arg0, arg1);
    RecordOutcome("XMemGetWorkingSetStatistics", ok != 0);
    return ok;
}

#if defined(_WIN32)
HRESULT WINAPI XMemShim_XMemTransferMemory(
    void* destinationAddress,
    const void* sourceAddress,
    SIZE_T sizeInBytes,
    UINT64 flags) {
    (void)flags;
    if (!destinationAddress || !sourceAddress) {
        RecordHresult("XMemTransferMemory", static_cast<int32_t>(E_POINTER));
        return E_POINTER;
    }
    if (sizeInBytes == 0u) {
        RecordHresult("XMemTransferMemory", static_cast<int32_t>(S_OK));
        return S_OK;
    }
    std::memmove(destinationAddress, sourceAddress, sizeInBytes);
    RecordHresult("XMemTransferMemory", static_cast<int32_t>(S_OK));
    return S_OK;
}
#else
int32_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemTransferMemory(
    void* destinationAddress,
    const void* sourceAddress,
    size_t sizeInBytes,
    uint64_t flags) {
    (void)flags;
    if (!destinationAddress || !sourceAddress) {
        RecordHresult("XMemTransferMemory", kHrPointer);
        return kHrPointer;
    }
    if (sizeInBytes == 0u) {
        RecordHresult("XMemTransferMemory", 0);
        return 0;
    }
    std::memmove(destinationAddress, sourceAddress, sizeInBytes);
    RecordHresult("XMemTransferMemory", 0);
    return 0;
}
#endif

int32_t OPENSCARLETT_KERNELX_CALL XMemShim_GetTelemetryCallCountForTest(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    if (!symbolName) {
        return 0;
    }
    const auto entries = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& e : entries) {
        if (e.module_name == kModName && e.symbol_name == symbolName) {
            if (outTotalCalls) *outTotalCalls = e.total_calls;
            if (outOkCalls) *outOkCalls = e.ok_calls;
            return 1;
        }
    }
    if (outTotalCalls) *outTotalCalls = 0;
    if (outOkCalls) *outOkCalls = 0;
    return 0;
}

}  // extern "C"
