#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_KERNELX)
#    define OPENSCARLETT_KERNELX_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_KERNELX_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_KERNELX_CALL __stdcall
#else
#  define OPENSCARLETT_KERNELX_API __attribute__((visibility("default")))
#  define OPENSCARLETT_KERNELX_CALL
#endif

// Standard Windows & Xbox Game OS (KernelX) virtual/physical memory constants.
#ifndef MEM_COMMIT
#  define MEM_COMMIT 0x00001000u
#endif
#ifndef MEM_RESERVE
#  define MEM_RESERVE 0x00002000u
#endif
#ifndef MEM_DECOMMIT
#  define MEM_DECOMMIT 0x00004000u
#endif
#ifndef MEM_RELEASE
#  define MEM_RELEASE 0x00008000u
#endif
#ifndef MEM_RESET
#  define MEM_RESET 0x00080000u
#endif
#ifndef MEM_TOP_DOWN
#  define MEM_TOP_DOWN 0x00100000u
#endif
#ifndef MEM_WRITE_COMBINE
#  define MEM_WRITE_COMBINE 0x04000000u
#endif
#ifndef MEM_GRAPHICS
#  define MEM_GRAPHICS 0x10000000u
#endif
#ifndef MEM_LARGE_PAGES
#  define MEM_LARGE_PAGES 0x20000000u
#endif
#ifndef MEM_TITLE
#  define MEM_TITLE 0x40000000u
#endif
#ifndef MEM_4MB_PAGES
#  define MEM_4MB_PAGES 0x80000000u
#endif

#ifndef PAGE_NOACCESS
#  define PAGE_NOACCESS 0x00000001u
#endif
#ifndef PAGE_READONLY
#  define PAGE_READONLY 0x00000002u
#endif
#ifndef PAGE_READWRITE
#  define PAGE_READWRITE 0x00000004u
#endif
#ifndef PAGE_WRITECOPY
#  define PAGE_WRITECOPY 0x00000008u
#endif
#ifndef PAGE_EXECUTE
#  define PAGE_EXECUTE 0x00000010u
#endif
#ifndef PAGE_EXECUTE_READ
#  define PAGE_EXECUTE_READ 0x00000020u
#endif
#ifndef PAGE_EXECUTE_READWRITE
#  define PAGE_EXECUTE_READWRITE 0x00000040u
#endif
#ifndef PAGE_GUARD
#  define PAGE_GUARD 0x00000100u
#endif
#ifndef PAGE_NOCACHE
#  define PAGE_NOCACHE 0x00000200u
#endif
#ifndef PAGE_WRITECOMBINE
#  define PAGE_WRITECOMBINE 0x00000400u
#endif
#ifndef PAGE_GPU_COHERENT
#  define PAGE_GPU_COHERENT 0x00010000u
#endif
#ifndef PAGE_GPU_READONLY
#  define PAGE_GPU_READONLY 0x00020000u
#endif

// KernelX GPU tile mapping flags (matching openscarlett::GpuTileMappingFlags).
#define KERNELX_TILE_MAP_NONE 0x00000000u
#define KERNELX_TILE_MAP_NULL 0x00000001u
#define KERNELX_TILE_MAP_READONLY 0x00000002u
#define KERNELX_TILE_MAP_REUSE_SINGLE_TILE 0x00000004u
#define KERNELX_TILE_MAP_SUPERTILE_4MB 0x00000008u

// KernelX GPU tile pool creation flags (matching openscarlett::GpuTilePoolFlags).
#define KERNELX_TILE_POOL_NONE 0x00000000u
#define KERNELX_TILE_POOL_GRAPHICS 0x00000001u
#define KERNELX_TILE_POOL_LARGE_PAGES 0x00000002u
#define KERNELX_TILE_POOL_WRITE_COMBINED 0x00000004u

// Xbox XALLOC_ATTRIBUTES alignment codes (bits 24..27).
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

// Xbox XALLOC_ATTRIBUTES memory type codes (bits 28..31).
#define XALLOC_MEMTYPE_HEAP                               0x0u
#define XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE              0x1u
#define XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE_GPU_READONLY 0x2u
#define XALLOC_MEMTYPE_GRAPHICS_CACHEABLE                 0x3u
#define XALLOC_MEMTYPE_PHYSICAL                           0x4u

// Xbox XALLOC_ATTRIBUTES page size preference (bits 32..33).
#define XALLOC_PAGESIZE_DEFAULT 0x0u
#define XALLOC_PAGESIZE_4KB     0x1u
#define XALLOC_PAGESIZE_64KB    0x2u
#define XALLOC_PAGESIZE_4MB     0x3u

// Xbox XALLOC_ATTRIBUTES zero-initialize & 2 MiB page-hint bits (bits 34..35).
#define XALLOC_FLAG_ZERO_INIT (1ull << 34)
#define XALLOC_FLAG_2MB_PAGES (1ull << 35)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XALLOC_ATTRIBUTES {
    uint64_t dwObjectType : 13;
    uint64_t dwHeapTracksAttributes : 1;
    uint64_t dwMustSucceed : 1;
    uint64_t dwFixedSize : 1;
    uint64_t dwAllocatorId : 8;
    uint64_t dwAlignment : 4;
    uint64_t dwMemoryType : 4;
    uint64_t dwPageSize : 2;
    uint64_t dwZeroInitialize : 1;
    uint64_t dwReserved : 29;
} XALLOC_ATTRIBUTES;

static inline uint64_t MAKE_XALLOC_ATTRIBUTES(uint32_t objectType,
                                              uint32_t heapTracksAttributes,
                                              uint32_t mustSucceed,
                                              uint32_t fixedSize,
                                              uint32_t allocatorId,
                                              uint32_t alignment,
                                              uint32_t memoryType,
                                              uint32_t pageSize,
                                              uint32_t zeroInit) {
    return ((uint64_t)(objectType & 0x1FFFu)) |
           (((uint64_t)(heapTracksAttributes & 0x1u)) << 13) |
           (((uint64_t)(mustSucceed & 0x1u)) << 14) |
           (((uint64_t)(fixedSize & 0x1u)) << 15) |
           (((uint64_t)(allocatorId & 0xFFu)) << 16) |
           (((uint64_t)(alignment & 0xFu)) << 24) |
           (((uint64_t)(memoryType & 0xFu)) << 28) |
           (((uint64_t)(pageSize & 0x3u)) << 32) |
           (((uint64_t)(zeroInit & 0x1u)) << 34);
}

typedef enum KERNELX_CONSOLE_TYPE {
    CONSOLE_TYPE_UNKNOWN = 0,
    CONSOLE_TYPE_XBOX_ONE = 1,
    CONSOLE_TYPE_XBOX_ONE_X = 2,
    CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART = 3,
    CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA = 4,
    CONSOLE_TYPE_XBOX_SCARLETT_DEVKIT = 5,
} KERNELX_CONSOLE_TYPE;

typedef struct TITLEMEMORYSTATUS {
    uint32_t dwLength;
    uint32_t dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullLegacyUsed;
    uint64_t ullLegacyPeak;
    uint64_t ullLegacyAvail;
    uint64_t ullTitleUsed;
    uint64_t ullTitleAvail;
} TITLEMEMORYSTATUS;

typedef struct TOOLINGMEMORYSTATUS {
    uint32_t dwLength;
    uint32_t dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullUsed;
    uint64_t ullPeak;
} TOOLINGMEMORYSTATUS;

typedef struct KERNELX_OS_VERSION {
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t BuildNumber;
    uint16_t Revision;
} KERNELX_OS_VERSION;

// ABI-identical layout matching Win32 MEMORY_BASIC_INFORMATION (48 bytes on x64).
typedef struct XMEM_MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    uint32_t AllocationProtect;
    uint16_t PartitionId;
    uint16_t Reserved0;
    size_t RegionSize;
    uint32_t State;
    uint32_t Protect;
    uint32_t Type;
    uint32_t Reserved1;
} XMEM_MEMORY_BASIC_INFORMATION;

typedef struct XMEM_DECODED_ALLOC_ATTRIBUTES {
    uint32_t objectType;
    uint32_t heapTracksAttributes;
    uint32_t mustSucceed;
    uint32_t fixedSize;
    uint32_t allocatorId;
    uint32_t alignmentCode;
    uint32_t memoryType;
    uint32_t pageSizeCode;
    uint32_t zeroInitialize;
    uint32_t allocFlags;       // MEM_GRAPHICS | MEM_TITLE | MEM_WRITE_COMBINE | MEM_LARGE_PAGES | MEM_4MB_PAGES
    uint32_t protectFlags;     // PAGE_* flags including PAGE_WRITECOMBINE / PAGE_GPU_*
    uint64_t alignmentBytes;   // Decoded alignment in bytes
    uint64_t pageSizeBytes;    // Decoded page size in bytes (4KB / 64KB / 2MB / 4MB)
    uint64_t rawAttributes;    // Original 64-bit XALLOC_ATTRIBUTES bitfield
} XMEM_DECODED_ALLOC_ATTRIBUTES;

// ---------------------------------------------------------------------------
// Scarlett Game OS (kernelx.dll) Memory Status & XMem Allocator
// ---------------------------------------------------------------------------
OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL TitleMemoryStatus(
    TITLEMEMORYSTATUS* lpBuffer);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL ToolingMemoryStatus(
    TOOLINGMEMORYSTATUS* lpBuffer);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL XMemAlloc(
    size_t dwSize,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL XMemAllocDefault(
    size_t dwSize,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL XMemAllocAligned(
    size_t dwSize,
    size_t alignment,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL XMemFree(
    void* pAddress,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL XMemFreeDefault(
    void* pAddress,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API size_t OPENSCARLETT_KERNELX_CALL XMemSize(
    void* pAddress,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API size_t OPENSCARLETT_KERNELX_CALL XMemSizeDefault(
    void* pAddress,
    uint64_t dwAllocAttributes);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL XMemGetAuxiliaryTitleMemory(
    size_t requestedSize,
    size_t* outActualSize);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XMemReleaseAuxiliaryTitleMemory(
    void* baseAddress);

// Queries tracked Xbox unified/ERA allocations (falling back to host VirtualQuery).
// `lpBuffer` accepts Win32 MEMORY_BASIC_INFORMATION* or XMEM_MEMORY_BASIC_INFORMATION*.
OPENSCARLETT_KERNELX_API size_t OPENSCARLETT_KERNELX_CALL XMemVirtualQuery(
    const void* lpAddress,
    void* lpBuffer,
    size_t dwLength);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XMemDecodeAllocationAttributes(
    uint64_t dwAllocAttributes,
    XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XMemGetAllocationAttributes(
    const void* lpAddress,
    XMEM_DECODED_ALLOC_ATTRIBUTES* outDecoded);

// ---------------------------------------------------------------------------
// Scarlett Physical Page Allocation & Virtual Aliasing (VirtualAlloc2/MapViewOfFile3)
// ---------------------------------------------------------------------------
OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL AllocateTitlePhysicalPages(
    void* hProcess,
    uint32_t flAllocationType,
    uint64_t* pNumberOfPages,
    uint64_t* pPageArray);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL FreeTitlePhysicalPages(
    void* hProcess,
    uint64_t numberOfPages,
    const uint64_t* pPageArray);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL MapTitleVirtualMemory(
    void* virtualAddress,
    uint64_t numberOfPages,
    uint32_t flAllocationType,
    uint32_t flProtect,
    const uint64_t* pPageArray);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL UnmapTitleVirtualMemory(
    void* virtualAddress,
    uint64_t numberOfPages);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL MapTitlePhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages,
    uint32_t flAllocationType,
    uint32_t flProtect,
    const uint64_t* pPageArray);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL UnmapTitlePhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL XMemMapPhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages,
    uint32_t flAllocationType,
    uint32_t flProtect,
    const uint64_t* pPageArray);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XMemUnmapPhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL QueryTitlePhysicalPages(
    const void* virtualAddress,
    uint64_t* outPhysicalId,
    uint64_t* outSizeBytes,
    uint32_t* outFlags);

// Note: kernelx.dll also exports unprefixed VirtualAllocFromApp, VirtualProtectFromApp,
// CreateWaitableTimerExW, and QueryUnbiasedInterruptTime in its PE export directory.
// The KernelX-prefixed declarations below prevent clashes when <windows.h> is included.
OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL KernelXVirtualAllocFromApp(
    void* baseAddress,
    size_t size,
    uint32_t allocationType,
    uint32_t protection);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXVirtualProtectFromApp(
    void* address,
    size_t size,
    uint32_t newProtection,
    uint32_t* oldProtection);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXVirtualFree(
    void* address,
    size_t size,
    uint32_t freeType);

// ---------------------------------------------------------------------------
// Scarlett System, Thread & Timer Services
// ---------------------------------------------------------------------------
OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL GetConsoleType(void);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL GetSystemOSVersion(
    KERNELX_OS_VERSION* outVersion);

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL XGetProcessorCount(void);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL SetThreadName(
    void* hThread,
    const wchar_t* lpThreadName);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL GetThreadName(
    void* hThread,
    wchar_t* lpThreadName,
    size_t nBufferLength,
    size_t* pnRequiredLength);

#define KERNELX_THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF

OPENSCARLETT_KERNELX_API uint64_t OPENSCARLETT_KERNELX_CALL XThreadSetAffinityMask(
    void* hThread,
    uint64_t dwThreadAffinityMask);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XThreadSetPriority(
    void* hThread,
    int32_t nPriority);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL XThreadGetPriority(
    void* hThread);

OPENSCARLETT_KERNELX_API void* OPENSCARLETT_KERNELX_CALL KernelXCreateWaitableTimerExW(
    void* lpTimerAttributes,
    const wchar_t* lpTimerName,
    uint32_t dwFlags,
    uint32_t dwDesiredAccess);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXQueryUnbiasedInterruptTime(
    uint64_t* unbiasedTime);

// ---------------------------------------------------------------------------
// Scarlett GPU Tiled / Reserved Resource Pool C ABI
// ---------------------------------------------------------------------------
OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXCreateTilePool(
    uint32_t tileCount,
    uint32_t flags,
    uint64_t* outPoolId);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXDestroyTilePool(
    uint64_t poolId);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXReserveTiledResource(
    uint32_t widthTiles,
    uint32_t heightTiles,
    uint64_t* outResourceId,
    void** outVirtualBase);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXReleaseTiledResource(
    uint64_t resourceId);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXMapTileRange(
    uint64_t resourceId,
    uint32_t tileX,
    uint32_t tileY,
    uint64_t poolId,
    uint32_t poolTileOffset,
    uint32_t tileCount,
    uint32_t flags);

// ---------------------------------------------------------------------------
// Xbox Game Core PLM Notifications (appnotify.h)
// ---------------------------------------------------------------------------
#ifndef OPENSCARLETT_APPNOTIFY_TYPES_DEFINED
#define OPENSCARLETT_APPNOTIFY_TYPES_DEFINED 1
typedef void* PAPPSTATE_REGISTRATION;
typedef void* PAPPCONSTRAIN_REGISTRATION;
typedef void (OPENSCARLETT_KERNELX_CALL* PAPPSTATE_CHANGE_ROUTINE)(uint8_t Quiesced, void* Context);
typedef void (OPENSCARLETT_KERNELX_CALL* PAPPCONSTRAIN_CHANGE_ROUTINE)(uint8_t Constrained, void* Context);
#endif

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL RegisterAppStateChangeNotification(
    PAPPSTATE_CHANGE_ROUTINE Routine,
    void* Context,
    PAPPSTATE_REGISTRATION* Registration);

OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL UnregisterAppStateChangeNotification(
    PAPPSTATE_REGISTRATION Registration);

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL RegisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_CHANGE_ROUTINE Routine,
    void* Context,
    PAPPCONSTRAIN_REGISTRATION* Registration);

OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL UnregisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_REGISTRATION Registration);

// Runtime / test dispatch & inspection helpers for PLM notification callbacks.
OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL KernelXDispatchAppStateChange(
    uint8_t Quiesced);

OPENSCARLETT_KERNELX_API uint32_t OPENSCARLETT_KERNELX_CALL KernelXDispatchAppConstrainedChange(
    uint8_t Constrained);

OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL KernelXGetAppNotificationCounts(
    uint32_t* outStateCount,
    uint32_t* outConstrainedCount);

// ---------------------------------------------------------------------------
// CallTelemetry Inspection Helpers (for verifying DLL-local telemetry)
// ---------------------------------------------------------------------------
OPENSCARLETT_KERNELX_API void OPENSCARLETT_KERNELX_CALL KernelXResetTelemetry(void);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXGetTelemetryCallCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outErrorCalls);

OPENSCARLETT_KERNELX_API int32_t OPENSCARLETT_KERNELX_CALL KernelXWriteTelemetryJson(
    const char* filePath);

#ifdef __cplusplus
}
#endif
