#pragma once

// MIT Microsoft libHttpClient (<httpClient/httpClient.h>) public C ABI header
// backed by libHttpClient.GDK.dll.

#include "../XAsync.h"

#if defined(_WIN32)
#  define HC_COMPAT_API __declspec(dllimport)
#  define HC_COMPAT_CALL __stdcall
#else
#  define HC_COMPAT_API
#  define HC_COMPAT_CALL
#endif

typedef struct HCCallObject* HCCallHandle;

enum class HCTraceLevel : uint32_t {
    Off = 0,
    Error = 1,
    Warning = 2,
    Important = 3,
    Information = 4,
    Verbose = 5,
};

struct HCInitArgs {
    uint32_t flags;
    void* reserved;
};

#ifdef __cplusplus
extern "C" {
#endif

HC_COMPAT_API HRESULT HC_COMPAT_CALL HCInitialize(const HCInitArgs* args);
HC_COMPAT_API void HC_COMPAT_CALL HCCleanup(void);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCCleanupAsync(XAsyncBlock* async);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCGetLibVersion(const char** version);
HC_COMPAT_API void HC_COMPAT_CALL HCTraceSetTraceToDebugger(bool traceToDebugger);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCSettingsSetTraceLevel(HCTraceLevel traceLevel);

HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallCreate(HCCallHandle* call);
HC_COMPAT_API HCCallHandle HC_COMPAT_CALL HCHttpCallDuplicateHandle(HCCallHandle call);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallCloseHandle(HCCallHandle call);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallRequestSetUrl(
    HCCallHandle call,
    const char* method,
    const char* url);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* headerName,
    const char* headerValue,
    bool allowTracing);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* requestBodyBytes,
    uint32_t requestBodySize);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallRequestSetRequestBodyString(
    HCCallHandle call,
    const char* requestBodyString);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallPerformAsync(
    HCCallHandle call,
    XAsyncBlock* asyncBlock);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallResponseGetStatusCode(
    HCCallHandle call,
    uint32_t* statusCode);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallResponseGetResponseString(
    HCCallHandle call,
    const char** responseString);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallResponseGetResponseBodyBytesSize(
    HCCallHandle call,
    size_t* bufferSize);
HC_COMPAT_API HRESULT HC_COMPAT_CALL HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* headerName,
    const char** headerValue);

#ifdef __cplusplus
}
#endif
