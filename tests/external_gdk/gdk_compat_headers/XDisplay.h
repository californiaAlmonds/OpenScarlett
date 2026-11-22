#pragma once

// MIT Microsoft GDK <XDisplay.h> public C/C++ ABI header.

#include "XGameRuntimeTypes.h"

enum class XDisplayHdrModeResult : uint32_t {
    Unknown = 0,
    Enabled = 1,
    Disabled = 2,
};

struct XDisplayHdrModeInfo {
    float minToneMapLuminance;
    float maxToneMapLuminance;
    float maxFullFrameToneMapLuminance;
};

typedef struct XDisplayTimeoutDeferral* XDisplayTimeoutDeferralHandle;

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API XDisplayHdrModeResult XGR_COMPAT_CALL XDisplayTryEnableHdrMode(
    uint32_t preferredHdrMode,
    XDisplayHdrModeInfo* displayHdrModeInfo);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XDisplayTryAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle);

XGR_COMPAT_API void XGR_COMPAT_CALL XDisplayCloseTimeoutDeferralHandle(
    XDisplayTimeoutDeferralHandle handle);

#ifdef __cplusplus
}
#endif
