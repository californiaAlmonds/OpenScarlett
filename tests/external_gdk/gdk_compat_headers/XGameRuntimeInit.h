#pragma once

// MIT Microsoft GDK <XGameRuntimeInit.h> public C/C++ ABI header.

#include "XDisplay.h"
#include "XGameRuntimeTypes.h"
#include "XGameSave.h"
#include "XMem.h"
#include "XPackage.h"
#include "XSystem.h"
#include "XTaskQueue.h"
#include "XUser.h"
#include "XAsyncProvider.h"

struct XgrGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};
static_assert(sizeof(XgrGuid) == 16, "XgrGuid must be 16 bytes");

constexpr XgrGuid XGR_IID_IUNKNOWN = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr XgrGuid XGR_API_GUID_TASK_QUEUE = {
    0x4F534752u, 0x0001u, 0x4000u, {0x80, 0x01, 0x00, 0x58, 0x54, 0x51, 0x00, 0x01}};
constexpr XgrGuid XGR_API_GUID_ASYNC = {
    0x4F534752u, 0x0002u, 0x4000u, {0x80, 0x02, 0x00, 0x58, 0x41, 0x53, 0x00, 0x02}};
constexpr XgrGuid XGR_API_GUID_USER = {
    0x4F534752u, 0x0003u, 0x4000u, {0x80, 0x03, 0x00, 0x58, 0x55, 0x53, 0x00, 0x03}};
constexpr XgrGuid XGR_API_GUID_GAME_SAVE = {
    0x4F534752u, 0x0004u, 0x4000u, {0x80, 0x04, 0x00, 0x58, 0x47, 0x53, 0x00, 0x04}};
constexpr XgrGuid XGR_API_GUID_SYSTEM = {
    0x4F534752u, 0x0005u, 0x4000u, {0x80, 0x05, 0x00, 0x58, 0x53, 0x59, 0x00, 0x05}};
constexpr XgrGuid XGR_API_GUID_MEM = {
    0x4F534752u, 0x0006u, 0x4000u, {0x80, 0x06, 0x00, 0x58, 0x4D, 0x45, 0x00, 0x06}};
constexpr XgrGuid XGR_API_GUID_PACKAGE = {
    0x4F534752u, 0x0007u, 0x4000u, {0x80, 0x07, 0x00, 0x58, 0x50, 0x4B, 0x00, 0x07}};
constexpr XgrGuid XGR_API_GUID_DISPLAY = {
    0x4F534752u, 0x0008u, 0x4000u, {0x80, 0x08, 0x00, 0x58, 0x44, 0x50, 0x00, 0x08}};

typedef HRESULT(XGR_COMPAT_CALL* XgrQueryInterfaceFn)(
    const void* self,
    const XgrGuid* riid,
    void** ppvObject);
typedef uint32_t(XGR_COMPAT_CALL* XgrAddRefFn)(const void* self);
typedef uint32_t(XGR_COMPAT_CALL* XgrReleaseFn)(const void* self);

struct IXgrTaskQueueApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    HRESULT(XGR_COMPAT_CALL* XTaskQueueCreate)(XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*);
    HRESULT(XGR_COMPAT_CALL* XTaskQueueDuplicateHandle)(XTaskQueueHandle, XTaskQueueHandle*);
    void(XGR_COMPAT_CALL* XTaskQueueCloseHandle)(XTaskQueueHandle);
    bool(XGR_COMPAT_CALL* XTaskQueueGetCurrentProcessTaskQueue)(XTaskQueueHandle*);
    void(XGR_COMPAT_CALL* XTaskQueueSetCurrentProcessTaskQueue)(XTaskQueueHandle);
    HRESULT(XGR_COMPAT_CALL* XTaskQueueSubmitCallback)(XTaskQueueHandle, XTaskQueuePort, void*, XTaskQueueCallback);
    HRESULT(XGR_COMPAT_CALL* XTaskQueueSubmitDelayedCallback)(XTaskQueueHandle, XTaskQueuePort, uint32_t, void*, XTaskQueueCallback);
    bool(XGR_COMPAT_CALL* XTaskQueueDispatch)(XTaskQueueHandle, XTaskQueuePort, uint32_t);
    HRESULT(XGR_COMPAT_CALL* XTaskQueueTerminate)(XTaskQueueHandle, bool, void*, XTaskQueueTerminatedCallback);
    HRESULT(XGR_COMPAT_CALL* XTaskQueueRegisterMonitor)(XTaskQueueHandle, void*, XTaskQueueMonitorCallback, XTaskQueueRegistrationToken*);
    void(XGR_COMPAT_CALL* XTaskQueueUnregisterMonitor)(XTaskQueueHandle, XTaskQueueRegistrationToken);
};

struct IXgrAsyncApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    HRESULT(XGR_COMPAT_CALL* XAsyncBegin)(XAsyncBlock*, void*, const void*, const char*, XAsyncProvider);
    HRESULT(XGR_COMPAT_CALL* XAsyncSchedule)(XAsyncBlock*, uint32_t);
    void(XGR_COMPAT_CALL* XAsyncComplete)(XAsyncBlock*, HRESULT, size_t);
    HRESULT(XGR_COMPAT_CALL* XAsyncGetStatus)(XAsyncBlock*, bool);
    HRESULT(XGR_COMPAT_CALL* XAsyncGetResultSize)(XAsyncBlock*, size_t*);
    HRESULT(XGR_COMPAT_CALL* XAsyncGetResult)(XAsyncBlock*, const void*, size_t, void*, size_t*);
    void(XGR_COMPAT_CALL* XAsyncCancel)(XAsyncBlock*);
};

struct IXgrUserApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    HRESULT(XGR_COMPAT_CALL* XUserAddAsync)(XUserAddOptions, XAsyncBlock*);
    HRESULT(XGR_COMPAT_CALL* XUserAddResult)(XAsyncBlock*, XUserHandle*);
    HRESULT(XGR_COMPAT_CALL* XUserDuplicateHandle)(XUserHandle, XUserHandle*);
    void(XGR_COMPAT_CALL* XUserCloseHandle)(XUserHandle);
    HRESULT(XGR_COMPAT_CALL* XUserGetId)(XUserHandle, uint64_t*);
    HRESULT(XGR_COMPAT_CALL* XUserGetLocalId)(XUserHandle, XUserLocalId*);
    HRESULT(XGR_COMPAT_CALL* XUserGetGamertag)(XUserHandle, XUserGamertagComponent, size_t, char*, size_t*);
    HRESULT(XGR_COMPAT_CALL* XUserGetState)(XUserHandle, XUserState*);
    HRESULT(XGR_COMPAT_CALL* XUserGetAgeGroup)(XUserHandle, XUserAgeGroup*);
    HRESULT(XGR_COMPAT_CALL* XUserCheckPrivilege)(XUserHandle, XUserPrivilegeOptions, XUserPrivilege, bool*, XUserPrivilegeDenyReason*);
    HRESULT(XGR_COMPAT_CALL* XUserGetMaxUsers)(uint32_t*);
    int32_t(XGR_COMPAT_CALL* XUserCompare)(XUserHandle, XUserHandle);
};

struct IXgrGameSaveApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    HRESULT(XGR_COMPAT_CALL* XGameSaveInitializeProvider)(XUserHandle, const char*, bool, XGameSaveProviderHandle*);
    HRESULT(XGR_COMPAT_CALL* XGameSaveInitializeProviderAsync)(XUserHandle, const char*, bool, XAsyncBlock*);
    HRESULT(XGR_COMPAT_CALL* XGameSaveInitializeProviderResult)(XAsyncBlock*, XGameSaveProviderHandle*);
    void(XGR_COMPAT_CALL* XGameSaveCloseProvider)(XGameSaveProviderHandle);
    HRESULT(XGR_COMPAT_CALL* XGameSaveCreateContainer)(XGameSaveProviderHandle, const char*, XGameSaveContainerHandle*);
    void(XGR_COMPAT_CALL* XGameSaveCloseContainer)(XGameSaveContainerHandle);
    HRESULT(XGR_COMPAT_CALL* XGameSaveCreateUpdate)(XGameSaveContainerHandle, const char*, XGameSaveUpdateHandle*);
    HRESULT(XGR_COMPAT_CALL* XGameSaveSubmitBlobWrite)(XGameSaveUpdateHandle, const char*, const uint8_t*, size_t);
    HRESULT(XGR_COMPAT_CALL* XGameSaveSubmitBlobDelete)(XGameSaveUpdateHandle, const char*);
    HRESULT(XGR_COMPAT_CALL* XGameSaveSubmitUpdate)(XGameSaveUpdateHandle);
    void(XGR_COMPAT_CALL* XGameSaveCloseUpdate)(XGameSaveUpdateHandle);
    HRESULT(XGR_COMPAT_CALL* XGameSaveReadBlobData)(XGameSaveContainerHandle, const char**, uint32_t*, size_t, XGameSaveBlob*);
    HRESULT(XGR_COMPAT_CALL* XGameSaveEnumerateContainerInfo)(XGameSaveProviderHandle, void*, XGameSaveContainerInfoCallback);
    HRESULT(XGR_COMPAT_CALL* XGameSaveEnumerateBlobInfo)(XGameSaveContainerHandle, void*, XGameSaveBlobInfoCallback);
    HRESULT(XGR_COMPAT_CALL* XGameSaveDeleteContainer)(XGameSaveProviderHandle, const char*);
    HRESULT(XGR_COMPAT_CALL* XGameSaveGetRemainingQuota)(XGameSaveProviderHandle, int64_t*);
};

struct IXgrSystemApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    HRESULT(XGR_COMPAT_CALL* XGameRuntimeInitialize)(void);
    void(XGR_COMPAT_CALL* XGameRuntimeUninitialize)(void);
    HRESULT(XGR_COMPAT_CALL* XGameGetXboxTitleId)(uint32_t*);
    XSystemDeviceType(XGR_COMPAT_CALL* XSystemGetDeviceType)(void);
    HRESULT(XGR_COMPAT_CALL* XSystemGetConsoleId)(size_t, char*, size_t*);
    HRESULT(XGR_COMPAT_CALL* XSystemGetXboxLiveSandboxId)(size_t, char*, size_t*);
    XSystemAnalyticsInfo(XGR_COMPAT_CALL* XSystemGetAnalyticsInfo)(void);
    XSystemRuntimeInfo(XGR_COMPAT_CALL* XSystemGetRuntimeInfo)(void);
    bool(XGR_COMPAT_CALL* XSystemIsHandleValid)(void*);
};

struct IXgrMemApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    void*(XGR_COMPAT_CALL* XMemAllocDefault)(size_t, uint64_t);
    void(XGR_COMPAT_CALL* XMemFreeDefault)(void*, uint64_t);
    void*(XGR_COMPAT_CALL* XMemAlloc)(size_t, uint64_t);
    void(XGR_COMPAT_CALL* XMemFree)(void*, uint64_t);
    HRESULT(XGR_COMPAT_CALL* XMemSetAllocationHooks)(XMemAllocFunction, XMemFreeFunction);
};

struct IXgrPackageApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    HRESULT(XGR_COMPAT_CALL* XPackageGetCurrentProcessPackageIdentifier)(size_t, char*);
    bool(XGR_COMPAT_CALL* XPackageIsPackagedProcess)(void);
    HRESULT(XGR_COMPAT_CALL* XPackageEnumerateChunkAvailability)(const char*, uint32_t, const XPackageChunkSelector*, void*, XPackageChunkAvailabilityCallback);
};

struct IXgrDisplayApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;
    XDisplayHdrModeResult(XGR_COMPAT_CALL* XDisplayTryEnableHdrMode)(uint32_t, XDisplayHdrModeInfo*);
    HRESULT(XGR_COMPAT_CALL* XDisplayTryAcquireTimeoutDeferral)(XDisplayTimeoutDeferralHandle*);
    void(XGR_COMPAT_CALL* XDisplayCloseTimeoutDeferralHandle)(XDisplayTimeoutDeferralHandle);
};

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameRuntimeInitialize(void);
XGR_COMPAT_API void XGR_COMPAT_CALL XGameRuntimeUninitialize(void);
XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameGetXboxTitleId(uint32_t* titleId);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL InitializeApiImpl(void);
XGR_COMPAT_API void XGR_COMPAT_CALL UninitializeApiImpl(void);
XGR_COMPAT_API HRESULT XGR_COMPAT_CALL QueryApiImpl(
    const XgrGuid* apiId,
    void** ppApiVtable);

#ifdef __cplusplus
}
#endif
