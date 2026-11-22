#pragma once

// MIT Microsoft GDK <XAsyncProvider.h> public C/C++ ABI header.

#include "XAsync.h"

enum class XAsyncOp : uint32_t {
    Begin = 0,
    DoWork = 1,
    GetResult = 2,
    Cancel = 3,
    Cleanup = 4,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    size_t bufferSize;
    void* buffer;
    void* context;
};

typedef HRESULT(XGR_COMPAT_CALL* XAsyncProvider)(
    XAsyncOp op,
    const XAsyncProviderData* data);

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider provider);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs);

XGR_COMPAT_API void XGR_COMPAT_CALL XAsyncComplete(
    XAsyncBlock* asyncBlock,
    HRESULT result,
    size_t requiredBufferSize);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed);

#ifdef __cplusplus
}
#endif
