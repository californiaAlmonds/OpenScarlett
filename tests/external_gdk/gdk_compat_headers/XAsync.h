#pragma once

// MIT Microsoft GDK <XAsync.h> public C/C++ ABI header.

#include "XTaskQueue.h"

struct XAsyncBlock;
typedef void(XGR_COMPAT_CALL* XAsyncCompletionRoutine)(XAsyncBlock* asyncBlock);
typedef HRESULT(XGR_COMPAT_CALL* XAsyncWork)(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine callback;
    unsigned char internal[sizeof(void*) * 4];
};

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize);

XGR_COMPAT_API void XGR_COMPAT_CALL XAsyncCancel(XAsyncBlock* asyncBlock);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XAsyncRun(
    XAsyncBlock* asyncBlock,
    XAsyncWork work);

#ifdef __cplusplus
}
#endif
