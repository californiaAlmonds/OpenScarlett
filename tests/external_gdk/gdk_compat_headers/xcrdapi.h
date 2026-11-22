#pragma once

// MIT Xbox Console Runtime Diagnostics (<xcrdapi.h>) public ABI header
// backed by xcrdapi.dll.

#include <cstddef>
#include <cstdint>

#define XCRD_COMPAT_API __declspec(dllimport)
#define XCRD_COMPAT_CALL __stdcall

struct XCrdDiagnosticInfo {
    uint32_t structSize;
    uint32_t initialized;
    uint32_t breadcrumbCount;
    uint32_t errorReportCount;
    uint32_t lastErrorCode;
    uint32_t flags;
    char consoleFamily[64];
    char lastErrorComponent[64];
    char lastErrorMessage[256];
};

extern "C" {

XCRD_COMPAT_API int32_t XCRD_COMPAT_CALL XCrdInitialize(uint32_t flags);
XCRD_COMPAT_API void XCRD_COMPAT_CALL XCrdUninitialize(void);
XCRD_COMPAT_API int32_t XCRD_COMPAT_CALL XCrdGetDiagnosticInfo(XCrdDiagnosticInfo* outInfo);
XCRD_COMPAT_API int32_t XCRD_COMPAT_CALL XCrdSetBreadcrumb(
    uint32_t slot,
    const char* tag,
    uint64_t value);
XCRD_COMPAT_API int32_t XCRD_COMPAT_CALL XCrdGetBreadcrumb(
    uint32_t slot,
    char* outTag,
    size_t tagBufferSize,
    uint64_t* outValue);
XCRD_COMPAT_API int32_t XCRD_COMPAT_CALL XCrdReportError(
    uint32_t errorCode,
    const char* component,
    const char* message);
XCRD_COMPAT_API int32_t XCRD_COMPAT_CALL XCrdFlushDiagnostics(const char* optionalOutputPath);

}  // extern "C"
