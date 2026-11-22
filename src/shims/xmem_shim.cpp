// MIT xmem.dll shim forwarding Xbox memory allocations to kernelx.dll
// while recording xmem.dll call telemetry.

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/core/telemetry.h"

#include <cstddef>
#include <cstdint>

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
#pragma comment(linker, "/export:XMemGetAuxiliaryTitleMemory=XMemShim_XMemGetAuxiliaryTitleMemory")
#pragma comment(linker, "/export:XMemReleaseAuxiliaryTitleMemory=XMemShim_XMemReleaseAuxiliaryTitleMemory")
#pragma comment(linker, "/export:XMemVirtualQuery=XMemShim_XMemVirtualQuery")
#pragma comment(linker, "/export:XMemDecodeAllocationAttributes=XMemShim_XMemDecodeAllocationAttributes")
#pragma comment(linker, "/export:XMemGetAllocationAttributes=XMemShim_XMemGetAllocationAttributes")
#pragma comment(linker, "/export:XMemMapPhysicalPages=XMemShim_XMemMapPhysicalPages")
#pragma comment(linker, "/export:XMemUnmapPhysicalPages=XMemShim_XMemUnmapPhysicalPages")
#pragma comment(linker, "/export:XMemGetTelemetryCallCountForTest=XMemShim_GetTelemetryCallCountForTest")
#endif

namespace {

constexpr const char* kModName = "xmem.dll";
constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);

inline void RecordOutcome(const char* sym, bool ok) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        ok ? 0 : kHrFail,
        -1);
}

}  // namespace

extern "C" {

void* OPENSCARLETT_KERNELX_CALL XMemShim_XMemAlloc(size_t dwSize, uint64_t dwAllocAttributes) {
    void* ptr = ::XMemAlloc(dwSize, dwAllocAttributes);
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
#if defined(_WIN32)
    SetLastError(0u);
#endif
    ::XMemFree(pAddress, dwAllocAttributes);
#if defined(_WIN32)
    const bool ok = (pAddress == nullptr) || (GetLastError() == 0u);
#else
    const bool ok = true;
#endif
    RecordOutcome("XMemFree", ok);
}

void OPENSCARLETT_KERNELX_CALL XMemShim_XMemFreeDefault(void* pAddress, uint64_t dwAllocAttributes) {
#if defined(_WIN32)
    SetLastError(0u);
#endif
    ::XMemFreeDefault(pAddress, dwAllocAttributes);
#if defined(_WIN32)
    const bool ok = (pAddress == nullptr) || (GetLastError() == 0u);
#else
    const bool ok = true;
#endif
    RecordOutcome("XMemFreeDefault", ok);
}

size_t OPENSCARLETT_KERNELX_CALL XMemShim_XMemSize(void* pAddress, uint64_t dwAllocAttributes) {
    const size_t sz = ::XMemSize(pAddress, dwAllocAttributes);
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
    uint32_t flAllocationType,
    uint32_t flProtect,
    const uint64_t* pPageArray) {
    void* ptr = ::XMemMapPhysicalPages(
        virtualAddress, numberOfPages, flAllocationType, flProtect, pPageArray);
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
