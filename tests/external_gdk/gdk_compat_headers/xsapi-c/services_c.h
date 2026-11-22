#pragma once

// MIT Microsoft Xbox Services GDK C (<xsapi-c/services_c.h>) public ABI header
// backed by Microsoft.Xbox.Services.GDK.C.dll.

#include "../XAsync.h"
#include "../XUser.h"

#define XSAPI_COMPAT_API __declspec(dllimport)
#define XSAPI_COMPAT_CALL __stdcall

constexpr size_t XBL_SCID_LENGTH = 38;
constexpr size_t XBL_GAMERTAG_CHAR_SIZE = 16 * 3;
constexpr size_t XBL_MODERN_GAMERTAG_CHAR_SIZE = 12 * 4 + 1;
constexpr size_t XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE = 14 + 1;
constexpr size_t XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE = 16 * 4 + 1;
constexpr size_t XBL_DISPLAY_NAME_CHAR_SIZE = 128;
constexpr size_t XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE = 225;
constexpr size_t XBL_GAMERSCORE_CHAR_SIZE = 16;

typedef struct XblContext* XblContextHandle;

struct XblInitArgs {
    XTaskQueueHandle queue;
    const char* scid;
};

struct XblPresenceRichPresenceIds {
    char scid[XBL_SCID_LENGTH];
    const char* presenceId;
    const char** presenceTokenIds;
    size_t presenceTokenIdsCount;
};

struct XblUserProfile {
    uint64_t xboxUserId;
    char appDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char appDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gameDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char gameDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gamerscore[XBL_GAMERSCORE_CHAR_SIZE];
    char gamertag[XBL_GAMERTAG_CHAR_SIZE];
    char modernGamertag[XBL_MODERN_GAMERTAG_CHAR_SIZE];
    char modernGamertagSuffix[XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE];
    char uniqueModernGamertag[XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE];
};

extern "C" {

XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblInitialize(const XblInitArgs* args);
XSAPI_COMPAT_API void XSAPI_COMPAT_CALL XblCleanup(void);
XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblCleanupAsync(XAsyncBlock* async);
XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblContextCreateHandle(
    XUserHandle user, XblContextHandle* context);
XSAPI_COMPAT_API void XSAPI_COMPAT_CALL XblContextCloseHandle(XblContextHandle handle);
XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);
XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async);
XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblProfileGetUserProfileResult(
    XAsyncBlock* async,
    XblUserProfile* profile);
XSAPI_COMPAT_API HRESULT XSAPI_COMPAT_CALL XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async);

}  // extern "C"
