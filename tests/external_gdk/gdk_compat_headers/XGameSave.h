#pragma once

// MIT Microsoft GDK <XGameSave.h> public C/C++ ABI header.

#include "XAsync.h"
#include "XUser.h"

typedef struct XGameSaveProvider* XGameSaveProviderHandle;
typedef struct XGameSaveContainer* XGameSaveContainerHandle;
typedef struct XGameSaveUpdate* XGameSaveUpdateHandle;

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    int64_t lastModifiedTime;
    bool needsSync;
};

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};

typedef bool(XGR_COMPAT_CALL* XGameSaveContainerInfoCallback)(
    const XGameSaveContainerInfo* container,
    void* context);
typedef bool(XGR_COMPAT_CALL* XGameSaveBlobInfoCallback)(
    const XGameSaveBlobInfo* info,
    void* context);

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XGameSaveProviderHandle* provider);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider);

XGR_COMPAT_API void XGR_COMPAT_CALL XGameSaveCloseProvider(
    XGameSaveProviderHandle provider);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* result);

XGR_COMPAT_API void XGR_COMPAT_CALL XGameSaveCloseContainer(
    XGameSaveContainerHandle context);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* result);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveSubmitUpdate(
    XGameSaveUpdateHandle updateContext);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveSubmitUpdateResult(
    XAsyncBlock* async);

XGR_COMPAT_API void XGR_COMPAT_CALL XGameSaveCloseUpdate(
    XGameSaveUpdateHandle updateContext);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback callback);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback callback);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback callback);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* name,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveDeleteContainerResult(
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota);

#ifdef __cplusplus
}
#endif
