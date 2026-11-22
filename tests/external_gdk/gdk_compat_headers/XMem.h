#pragma once

// MIT Microsoft GDK <XMem.h> public C/C++ ABI header.

#include "XGameRuntimeTypes.h"

#define XALLOC_ALIGNMENT_DEFAULT 0x0u
#define XALLOC_ALIGNMENT_4       0x1u
#define XALLOC_ALIGNMENT_8       0x2u
#define XALLOC_ALIGNMENT_16      0x3u
#define XALLOC_ALIGNMENT_32      0x4u
#define XALLOC_ALIGNMENT_64      0x5u
#define XALLOC_ALIGNMENT_128     0x6u
#define XALLOC_ALIGNMENT_256     0x7u
#define XALLOC_ALIGNMENT_512     0x8u
#define XALLOC_ALIGNMENT_1K      0x9u
#define XALLOC_ALIGNMENT_2K      0xAu
#define XALLOC_ALIGNMENT_4K      0xBu
#define XALLOC_ALIGNMENT_8K      0xCu
#define XALLOC_ALIGNMENT_16K     0xDu
#define XALLOC_ALIGNMENT_32K     0xEu
#define XALLOC_ALIGNMENT_64K     0xFu

#define XALLOC_MEMTYPE_HEAP                               0x0u
#define XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE              0x1u
#define XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE_GPU_READONLY 0x2u
#define XALLOC_MEMTYPE_GRAPHICS_CACHEABLE                 0x3u
#define XALLOC_MEMTYPE_PHYSICAL                           0x4u

#define XALLOC_PAGESIZE_DEFAULT 0x0u
#define XALLOC_PAGESIZE_4KB     0x1u
#define XALLOC_PAGESIZE_64KB    0x2u
#define XALLOC_PAGESIZE_4MB     0x3u

#define XALLOC_FLAG_ZERO_INIT (1ull << 34)

static inline uint64_t MAKE_XALLOC_ATTRIBUTES(uint32_t objectType,
                                              uint32_t heapTracksAttributes,
                                              uint32_t mustSucceed,
                                              uint32_t fixedSize,
                                              uint32_t allocatorId,
                                              uint32_t alignment,
                                              uint32_t memoryType,
                                              uint32_t pageSize,
                                              uint32_t zeroInit) {
    return (static_cast<uint64_t>(objectType & 0x1FFFu)) |
           (static_cast<uint64_t>(heapTracksAttributes & 0x1u) << 13) |
           (static_cast<uint64_t>(mustSucceed & 0x1u) << 14) |
           (static_cast<uint64_t>(fixedSize & 0x1u) << 15) |
           (static_cast<uint64_t>(allocatorId & 0xFFu) << 16) |
           (static_cast<uint64_t>(alignment & 0xFu) << 24) |
           (static_cast<uint64_t>(memoryType & 0xFu) << 28) |
           (static_cast<uint64_t>(pageSize & 0x3u) << 32) |
           (static_cast<uint64_t>(zeroInit & 0x1u) << 34);
}

typedef void*(XGR_COMPAT_CALL* XMemAllocFunction)(size_t size, uint64_t attributes);
typedef void(XGR_COMPAT_CALL* XMemFreeFunction)(void* pointer, uint64_t attributes);

// Dynamic xmem.dll loader typedefs (when calling xmem.dll alongside xgameruntime.dll)
typedef void*(XGR_COMPAT_CALL* PFN_XMemAlloc)(size_t dwSize, uint64_t dwAllocAttributes);
typedef void*(XGR_COMPAT_CALL* PFN_XMemAllocDefault)(size_t dwSize, uint64_t dwAllocAttributes);
typedef void(XGR_COMPAT_CALL* PFN_XMemFree)(void* pAddress, uint64_t dwAllocAttributes);
typedef void(XGR_COMPAT_CALL* PFN_XMemFreeDefault)(void* pAddress, uint64_t dwAllocAttributes);
typedef size_t(XGR_COMPAT_CALL* PFN_XMemSize)(void* pAddress, uint64_t dwAllocAttributes);
typedef size_t(XGR_COMPAT_CALL* PFN_XMemSizeDefault)(void* pAddress, uint64_t dwAllocAttributes);
typedef void*(XGR_COMPAT_CALL* PFN_XMemGetAuxiliaryTitleMemory)(size_t requestedSize, size_t* outActualSize);
typedef int32_t(XGR_COMPAT_CALL* PFN_XMemReleaseAuxiliaryTitleMemory)(void* baseAddress);

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API void* XGR_COMPAT_CALL XMemAllocDefault(size_t size, uint64_t attributes);
XGR_COMPAT_API void XGR_COMPAT_CALL XMemFreeDefault(void* pointer, uint64_t attributes);
XGR_COMPAT_API void* XGR_COMPAT_CALL XMemAlloc(size_t size, uint64_t attributes);
XGR_COMPAT_API void XGR_COMPAT_CALL XMemFree(void* pointer, uint64_t attributes);
XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XMemSetAllocationHooks(
    XMemAllocFunction allocHook,
    XMemFreeFunction freeHook);

#ifdef __cplusplus
}
#endif
