#pragma once

// MIT Microsoft GDK <XSystem.h> public C/C++ ABI header.

#include "XGameRuntimeTypes.h"

enum class XSystemDeviceType : uint32_t {
    Unknown = 0x00,
    Pc = 0x01,
    XboxOne = 0x02,
    XboxOneS = 0x03,
    XboxOneX = 0x04,
    XboxOneXDevkit = 0x05,
    XboxScarlettLockhart = 0x06,
    XboxScarlettAnaconda = 0x07,
    XboxScarlettDevkit = 0x08,
};

struct XSystemAnalyticsInfo {
    XVersion osVersion;
    XVersion hostingOsVersion;
    char family[64];
    char form[64];
};

struct XSystemRuntimeInfo {
    XVersion runtimeVersion;
    XVersion availableVersion;
};

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API XSystemDeviceType XGR_COMPAT_CALL XSystemGetDeviceType(void);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed);

XGR_COMPAT_API XSystemAnalyticsInfo XGR_COMPAT_CALL XSystemGetAnalyticsInfo(void);

XGR_COMPAT_API XSystemRuntimeInfo XGR_COMPAT_CALL XSystemGetRuntimeInfo(void);

XGR_COMPAT_API bool XGR_COMPAT_CALL XSystemIsHandleValid(void* handle);

#ifdef __cplusplus
}
#endif
