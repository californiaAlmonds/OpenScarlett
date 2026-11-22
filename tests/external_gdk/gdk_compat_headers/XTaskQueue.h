#pragma once

// MIT Microsoft GDK <XTaskQueue.h> public C/C++ ABI header.

#include "XGameRuntimeTypes.h"

typedef struct XTaskQueueObject* XTaskQueueHandle;
typedef struct XTaskQueuePortObject* XTaskQueuePortHandle;

enum class XTaskQueueDispatchMode : uint32_t {
    ThreadPool = 0,
    SerializedThreadPool = 1,
    Manual = 2,
    Immediate = 3,
};

enum class XTaskQueuePort : uint32_t {
    Work = 0,
    Completion = 1,
};

struct XTaskQueueRegistrationToken {
    uint64_t token;
};

typedef void(XGR_COMPAT_CALL* XTaskQueueCallback)(void* context, bool canceled);
typedef void(XGR_COMPAT_CALL* XTaskQueueMonitorCallback)(
    void* context,
    XTaskQueueHandle queue,
    XTaskQueuePort port);
typedef void(XGR_COMPAT_CALL* XTaskQueueTerminatedCallback)(void* context);

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle);

XGR_COMPAT_API void XGR_COMPAT_CALL XTaskQueueCloseHandle(XTaskQueueHandle queue);

XGR_COMPAT_API bool XGR_COMPAT_CALL XTaskQueueGetCurrentProcessTaskQueue(
    XTaskQueueHandle* queue);

XGR_COMPAT_API void XGR_COMPAT_CALL XTaskQueueSetCurrentProcessTaskQueue(
    XTaskQueueHandle queue);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback callback);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback callback);

XGR_COMPAT_API bool XGR_COMPAT_CALL XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback callback);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback callback,
    XTaskQueueRegistrationToken* token);

XGR_COMPAT_API void XGR_COMPAT_CALL XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XTaskQueueRegisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    HANDLE waitHandle,
    void* callbackContext,
    XTaskQueueCallback callback,
    XTaskQueueRegistrationToken* token);

XGR_COMPAT_API void XGR_COMPAT_CALL XTaskQueueUnregisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);

#ifdef __cplusplus
}
#endif
