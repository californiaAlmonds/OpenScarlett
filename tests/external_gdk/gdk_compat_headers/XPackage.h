#pragma once

// MIT Microsoft GDK <XPackage.h> public C/C++ ABI header.

#include "XGameRuntimeTypes.h"

enum class XPackageChunkSelectorType : uint32_t {
    Language = 1,
    Tag = 2,
    Feature = 3,
    Chunk = 4,
};

struct XPackageChunkSelector {
    XPackageChunkSelectorType type;
    union {
        const char* language;
        const char* tag;
        const char* feature;
        uint32_t chunkId;
    };
};

enum class XPackageChunkAvailability : uint32_t {
    Ready = 0,
    Pending = 1,
    Installable = 2,
    Unavailable = 3,
};

typedef bool(XGR_COMPAT_CALL* XPackageChunkAvailabilityCallback)(
    void* context,
    const XPackageChunkSelector* selector,
    XPackageChunkAvailability availability);

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer);

XGR_COMPAT_API bool XGR_COMPAT_CALL XPackageIsPackagedProcess(void);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    void* context,
    XPackageChunkAvailabilityCallback callback);

#ifdef __cplusplus
}
#endif
