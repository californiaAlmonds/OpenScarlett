#pragma once

// Microsoft Xbox Services GDK C API (Microsoft.Xbox.Services.GDK.C.dll) ABI.

#include <cstddef>
#include <cstdint>

#include "openscarlett/gdk/xgameruntime_abi.h"

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_XSAPI)
#    define OPENSCARLETT_XSAPI_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_XSAPI_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_XSAPI_CALL __stdcall
#else
#  define OPENSCARLETT_XSAPI_API __attribute__((visibility("default")))
#  define OPENSCARLETT_XSAPI_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard XSAPI HRESULT error codes.
constexpr XGameResult XBL_S_OK = XGR_S_OK;
constexpr XGameResult XBL_E_RUNTIME_ERROR = static_cast<XGameResult>(0x89235100u);
constexpr XGameResult XBL_E_ALREADY_INITIALIZED = static_cast<XGameResult>(0x89235105u);
constexpr XGameResult XBL_E_NOT_INITIALIZED = static_cast<XGameResult>(0x89235106u);
constexpr XGameResult XBL_E_HTTP_STATUS_404_NOT_FOUND = static_cast<XGameResult>(0x80190194u);
constexpr XGameResult XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED = static_cast<XGameResult>(0x8019019Cu);
constexpr XGameResult XBL_E_HTTP_STATUS_429_TOO_MANY_REQUESTS = static_cast<XGameResult>(0x801901ADu);

// Standard XSAPI string buffer capacities.
constexpr size_t XBL_SCID_LENGTH = 38;
constexpr size_t XBL_GAMERTAG_CHAR_SIZE = 16 * 3;
constexpr size_t XBL_MODERN_GAMERTAG_CHAR_SIZE = 12 * 4 + 1;
constexpr size_t XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE = 14 + 1;
constexpr size_t XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE = 16 * 4 + 1;
constexpr size_t XBL_DISPLAY_NAME_CHAR_SIZE = 128;
constexpr size_t XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE = 225;
constexpr size_t XBL_GAMERSCORE_CHAR_SIZE = 16;
constexpr size_t XBL_COLOR_CHAR_SIZE = 7 * 3;
constexpr size_t XBL_RICH_PRESENCE_CHAR_SIZE = 100 * 3;
constexpr size_t XBL_TITLE_NAME_CHAR_SIZE = 100 * 3;
constexpr size_t XBL_XBOX_USER_ID_CHAR_SIZE = 21;

constexpr size_t XBL_TITLE_STORAGE_MIN_UPLOAD_BLOCK_SIZE = 1024;
constexpr size_t XBL_TITLE_STORAGE_DEFAULT_UPLOAD_BLOCK_SIZE = 256 * 1024;
constexpr size_t XBL_TITLE_STORAGE_MAX_UPLOAD_BLOCK_SIZE = 4 * 1024 * 1024;
constexpr size_t XBL_TITLE_STORAGE_DEFAULT_DOWNLOAD_BLOCK_SIZE = 1024 * 1024;
constexpr size_t XBL_TITLE_STORAGE_BLOB_PATH_MAX_LENGTH = 256;
constexpr size_t XBL_TITLE_STORAGE_BLOB_DISPLAY_NAME_MAX_LENGTH = 128;
constexpr size_t XBL_TITLE_STORAGE_BLOB_ETAG_MAX_LENGTH = 128;

typedef int32_t XblFunctionContext;
typedef struct XblRealTimeActivitySubscription* XblRealTimeActivitySubscriptionHandle;

// ---------------------------------------------------------------------------
// Error Conditions & Global Configuration
// ---------------------------------------------------------------------------
enum XblErrorCondition : uint32_t {
    XblErrorCondition_NoError = 0,
    XblErrorCondition_GenericError = 1,
    XblErrorCondition_GenericOutOfRange = 2,
    XblErrorCondition_Auth = 3,
    XblErrorCondition_Network = 4,
    XblErrorCondition_Http = 5,
    XblErrorCondition_Http404NotFound = 6,
    XblErrorCondition_Http412PreconditionFailed = 7,
    XblErrorCondition_Http429TooManyRequests = 8,
    XblErrorCondition_HttpServiceTimeout = 9,
    XblErrorCondition_Rta = 10,
};

enum XblConfigSetting : uint32_t {
    XblConfigSetting_ThisCodeNeedsToBeChanged = 0,
};

struct XblServiceCallRoutedArgs {
    uint64_t xboxUserId;
    const char* httpMethod;
    const char* uri;
    const char* requestHeaders;
    const char* requestBody;
    uint32_t responseCount;
    const char* responseHeaders;
    const char* responseBody;
    const char* etag;
    const char* token;
    const char* signature;
    uint32_t httpStatus;
    const char* fullResponseFormatted;
    int64_t requestTime;
    int64_t responseTime;
    int64_t elapsedCallTime;
};

typedef void(OPENSCARLETT_XSAPI_CALL* XblCallRoutedHandler)(
    XblServiceCallRoutedArgs args,
    void* context);

// ---------------------------------------------------------------------------
// Memory Hooks & Lifecycle & Context
// ---------------------------------------------------------------------------
typedef void*(OPENSCARLETT_XSAPI_CALL* XblMemAllocFunction)(size_t size, uint32_t memoryType);
typedef void(OPENSCARLETT_XSAPI_CALL* XblMemFreeFunction)(void* pointer, uint32_t memoryType);

typedef struct XblContext* XblContextHandle;

struct XblInitArgs {
    XTaskQueueHandle queue;
    const char* scid;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblMemSetFunctions(
    XblMemAllocFunction memAllocFunc,
    XblMemFreeFunction memFreeFunc);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblMemGetFunctions(
    XblMemAllocFunction* memAllocFunc,
    XblMemFreeFunction* memFreeFunc);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblInitialize(
    const XblInitArgs* args);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblWrapper_XblInitialize(
    const char* scid,
    XTaskQueueHandle internalWorkQueue);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblCleanup(void);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblCleanupAsync(XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XTaskQueueHandle OPENSCARLETT_XSAPI_CALL XblGetAsyncQueue(void);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblGetScid(const char** scid);
OPENSCARLETT_XSAPI_API XblErrorCondition OPENSCARLETT_XSAPI_CALL XblGetErrorCondition(
    XGameResult hr);
OPENSCARLETT_XSAPI_API XblFunctionContext OPENSCARLETT_XSAPI_CALL XblAddServiceCallRoutedHandler(
    XblCallRoutedHandler handler,
    void* context);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblRemoveServiceCallRoutedHandler(
    XblFunctionContext token);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSetOverrideConfiguration(
    const char* overrideLocale);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL
XblDisableAssertsForXboxLiveThrottlingInDevSandboxes(XblConfigSetting setting);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblEventsWriteInGameEvent(
    XblContextHandle xboxLiveContext,
    const char* eventName,
    const char* dimensionsJson,
    const char* measurementsJson);

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextCreateHandle(
    XUserHandle user,
    XblContextHandle* context);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextDuplicateHandle(
    XblContextHandle handle,
    XblContextHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblContextCloseHandle(
    XblContextHandle handle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextGetUser(
    XblContextHandle context,
    XUserHandle* user);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextGetXboxUserId(
    XblContextHandle context,
    uint64_t* xboxUserId);

// Context Settings
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetHttpRetryDelay(
    XblContextHandle context,
    uint32_t* delayInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetHttpRetryDelay(
    XblContextHandle context,
    uint32_t delayInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetHttpTimeoutWindow(
    XblContextHandle context,
    uint32_t* timeoutWindowInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetHttpTimeoutWindow(
    XblContextHandle context,
    uint32_t timeoutWindowInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetLongHttpTimeout(
    XblContextHandle context,
    uint32_t* timeoutInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetLongHttpTimeout(
    XblContextHandle context,
    uint32_t timeoutInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblContextSettingsGetUseCrossPlatformQosServers(XblContextHandle context, bool* value);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblContextSettingsSetUseCrossPlatformQosServers(XblContextHandle context, bool value);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblContextSettingsGetWebsocketTimeoutWindow(
    XblContextHandle context,
    uint32_t* timeoutWindowInSeconds);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblContextSettingsSetWebsocketTimeoutWindow(
    XblContextHandle context,
    uint32_t timeoutWindowInSeconds);

// ---------------------------------------------------------------------------
// Achievements
// ---------------------------------------------------------------------------
typedef struct XblAchievementsResult* XblAchievementsResultHandle;

enum XblAchievementType : uint32_t {
    XblAchievementType_Unknown = 0,
    XblAchievementType_All = 1,
    XblAchievementType_Persistent = 2,
    XblAchievementType_Challenge = 3,
};

enum XblAchievementOrderBy : uint32_t {
    XblAchievementOrderBy_DefaultOrder = 0,
    XblAchievementOrderBy_TitleId = 1,
    XblAchievementOrderBy_UnlockTime = 2,
};

enum XblAchievementProgressState : uint32_t {
    XblAchievementProgressState_Unknown = 0,
    XblAchievementProgressState_Achieved = 1,
    XblAchievementProgressState_NotStarted = 2,
    XblAchievementProgressState_InProgress = 3,
};

enum XblAchievementParticipationType : uint32_t {
    XblAchievementParticipationType_Unknown = 0,
    XblAchievementParticipationType_Individual = 1,
    XblAchievementParticipationType_Group = 2,
};

struct XblAchievementRequirement {
    const char* id;
    const char* currentProgressValue;
    const char* targetProgressValue;
};

struct XblAchievementProgression {
    XblAchievementRequirement* requirements;
    size_t requirementsCount;
    int64_t timeUnlocked;
};

struct XblAchievement {
    const char* id;
    const char* serviceConfigurationId;
    const char* name;
    uint32_t titleId;
    XblAchievementProgressState progressState;
    XblAchievementProgression progression;
    const char* unlockedDescription;
    const char* lockedDescription;
    XblAchievementType type;
    XblAchievementParticipationType participationType;
    uint32_t percentComplete;
    uint32_t gamerscore;
    bool isSecret;
    bool isRevoked;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsGetAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* achievementId,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsGetAchievementResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    uint32_t type,
    bool unlockedOnly,
    uint32_t orderBy,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle,
    bool* hasNext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultGetNextAsync(
    XblAchievementsResultHandle resultHandle,
    uint32_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultGetNextResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsResultDuplicateHandle(
    XblAchievementsResultHandle handle,
    XblAchievementsResultHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblAchievementsResultCloseHandle(
    XblAchievementsResultHandle resultHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsUpdateAchievementForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    const char* serviceConfigurationId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);

// ---------------------------------------------------------------------------
// Presence
// ---------------------------------------------------------------------------
typedef struct XblPresenceRecord* XblPresenceRecordHandle;

enum XblPresenceUserState : uint32_t {
    XblPresenceUserState_Unknown = 0,
    XblPresenceUserState_Online = 1,
    XblPresenceUserState_Away = 2,
    XblPresenceUserState_Offline = 3,
};

enum XblPresenceDeviceType : uint32_t {
    XblPresenceDeviceType_Unknown = 0,
    XblPresenceDeviceType_PC = 5,
    XblPresenceDeviceType_XboxOne = 7,
    XblPresenceDeviceType_Scarlett = 14,
};

enum XblPresenceDetailLevel : uint32_t {
    XblPresenceDetailLevel_Default = 0,
    XblPresenceDetailLevel_User = 1,
    XblPresenceDetailLevel_Device = 2,
    XblPresenceDetailLevel_Title = 3,
    XblPresenceDetailLevel_All = 4,
};

enum XblPresenceTitleState : uint32_t {
    XblPresenceTitleState_Unknown = 0,
    XblPresenceTitleState_Started = 1,
    XblPresenceTitleState_Ended = 2,
};

struct XblPresenceQueryFilters {
    const XblPresenceDeviceType* deviceTypes;
    size_t deviceTypesCount;
    const uint32_t* titleIds;
    size_t titleIdsCount;
    XblPresenceDetailLevel detailLevel;
    bool onlineOnly;
    bool broadcastingOnly;
};

struct XblPresenceRichPresenceIds {
    char scid[XBL_SCID_LENGTH];
    const char* presenceId;
    const char** presenceTokenIds;
    size_t presenceTokenIdsCount;
};

struct XblPresenceTitleRecord {
    uint32_t titleId;
    const char* titleName;
    int64_t lastModified;
    bool titleActive;
    const char* richPresenceString;
    uint32_t viewState;
};

struct XblPresenceDeviceRecord {
    XblPresenceDeviceType deviceType;
    const XblPresenceTitleRecord* titleRecords;
    size_t titleRecordsCount;
};

typedef void(OPENSCARLETT_XSAPI_CALL* XblPresenceDevicePresenceChangedHandler)(
    void* context,
    uint64_t xuid,
    XblPresenceDeviceType deviceType,
    bool isUserLoggedOnDevice);

typedef void(OPENSCARLETT_XSAPI_CALL* XblPresenceTitlePresenceChangedHandler)(
    void* context,
    uint64_t xuid,
    uint32_t titleId,
    XblPresenceTitleState titleState);

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xuid,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceGetPresenceForMultipleUsersAsync(
    XblContextHandle xblContextHandle,
    uint64_t* xuids,
    size_t xuidsCount,
    const XblPresenceQueryFilters* filters,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceGetPresenceForMultipleUsersResultCount(
    XAsyncBlock* async,
    size_t* resultCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceGetPresenceForMultipleUsersResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandles,
    size_t presenceRecordHandlesCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceGetPresenceForSocialGroupAsync(
    XblContextHandle xblContextHandle,
    const char* socialGroupName,
    uint64_t* socialGroupOwnerXuid,
    const XblPresenceQueryFilters* filters,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceGetPresenceForSocialGroupResultCount(
    XAsyncBlock* async,
    size_t* resultCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceGetPresenceForSocialGroupResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandles,
    size_t presenceRecordHandlesCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetXuid(
    XblPresenceRecordHandle handle,
    uint64_t* xuid);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetUserState(
    XblPresenceRecordHandle handle,
    XblPresenceUserState* userState);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetDeviceRecords(
    XblPresenceRecordHandle handle,
    const XblPresenceDeviceRecord** deviceRecords,
    size_t* deviceRecordsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordDuplicateHandle(
    XblPresenceRecordHandle handle,
    XblPresenceRecordHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblPresenceRecordCloseHandle(
    XblPresenceRecordHandle handle);
OPENSCARLETT_XSAPI_API XblFunctionContext OPENSCARLETT_XSAPI_CALL
XblPresenceAddDevicePresenceChangedHandler(
    XblContextHandle xblContextHandle,
    XblPresenceDevicePresenceChangedHandler handler,
    void* context);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceRemoveDevicePresenceChangedHandler(
    XblContextHandle xblContextHandle,
    XblFunctionContext token);
OPENSCARLETT_XSAPI_API XblFunctionContext OPENSCARLETT_XSAPI_CALL
XblPresenceAddTitlePresenceChangedHandler(
    XblContextHandle xblContextHandle,
    XblPresenceTitlePresenceChangedHandler handler,
    void* context);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceRemoveTitlePresenceChangedHandler(
    XblContextHandle xblContextHandle,
    XblFunctionContext token);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceSubscribeToDevicePresenceChange(
    XblContextHandle xblContextHandle,
    uint64_t xuid,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceUnsubscribeFromDevicePresenceChange(
    XblContextHandle xblContextHandle,
    XblRealTimeActivitySubscriptionHandle subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceSubscribeToTitlePresenceChange(
    XblContextHandle xblContextHandle,
    uint64_t xuid,
    uint32_t titleId,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPresenceUnsubscribeFromTitlePresenceChange(
    XblContextHandle xblContextHandle,
    XblRealTimeActivitySubscriptionHandle subscriptionHandle);

// ---------------------------------------------------------------------------
// Profile
// ---------------------------------------------------------------------------
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

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfileResult(
    XAsyncBlock* async,
    XblUserProfile* profile);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesAsync(
    XblContextHandle xboxLiveContext,
    const uint64_t* xboxUserIds,
    size_t xboxUserIdsCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesResultCount(
    XAsyncBlock* async,
    size_t* profileCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesResult(
    XAsyncBlock* async,
    size_t profilesCount,
    XblUserProfile* profiles);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblProfileGetUserProfilesForSocialGroupAsync(
    XblContextHandle xboxLiveContext,
    const char* socialGroup,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblProfileGetUserProfilesForSocialGroupResultCount(
    XAsyncBlock* async,
    size_t* profileCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblProfileGetUserProfilesForSocialGroupResult(
    XAsyncBlock* async,
    size_t profilesCount,
    XblUserProfile* profiles);

// ---------------------------------------------------------------------------
// Title Storage (Local + Persistent Blob Store)
// ---------------------------------------------------------------------------
typedef struct XblTitleStorageBlobMetadataResult* XblTitleStorageBlobMetadataResultHandle;

enum XblTitleStorageType : uint32_t {
    XblTitleStorageType_TrustedPlatformStorage = 0,
    XblTitleStorageType_GlobalStorage = 2,
    XblTitleStorageType_Universal = 5,
};

enum XblTitleStorageBlobType : uint32_t {
    XblTitleStorageBlobType_Unknown = 0,
    XblTitleStorageBlobType_Binary = 1,
    XblTitleStorageBlobType_Json = 2,
    XblTitleStorageBlobType_Config = 3,
};

enum XblTitleStorageETagMatchCondition : uint32_t {
    XblTitleStorageETagMatchCondition_NotUsed = 0,
    XblTitleStorageETagMatchCondition_IfMatch = 1,
    XblTitleStorageETagMatchCondition_IfNotMatch = 2,
};

struct XblTitleStorageBlobMetadata {
    char blobPath[XBL_TITLE_STORAGE_BLOB_PATH_MAX_LENGTH];
    XblTitleStorageBlobType blobType;
    XblTitleStorageType storageType;
    char displayName[XBL_TITLE_STORAGE_BLOB_DISPLAY_NAME_MAX_LENGTH];
    char eTag[XBL_TITLE_STORAGE_BLOB_ETAG_MAX_LENGTH];
    int64_t clientTimestamp;
    size_t length;
    char serviceConfigurationId[XBL_SCID_LENGTH];
    uint64_t xboxUserId;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetQuotaAsync(
    XblContextHandle xboxLiveContext,
    const char* serviceConfigurationId,
    XblTitleStorageType storageType,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetQuotaResult(
    XAsyncBlock* async,
    size_t* usedBytes,
    size_t* quotaBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetBlobMetadataAsync(
    XblContextHandle xboxLiveContext,
    const char* serviceConfigurationId,
    XblTitleStorageType storageType,
    const char* blobPath,
    uint64_t xboxUserId,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetBlobMetadataResult(
    XAsyncBlock* async,
    XblTitleStorageBlobMetadataResultHandle* result);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblTitleStorageBlobMetadataResultGetItems(
    XblTitleStorageBlobMetadataResultHandle resultHandle,
    const XblTitleStorageBlobMetadata** items,
    size_t* itemsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblTitleStorageBlobMetadataResultHasNext(
    XblTitleStorageBlobMetadataResultHandle resultHandle,
    bool* hasNext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblTitleStorageBlobMetadataResultGetNextAsync(
    XblTitleStorageBlobMetadataResultHandle resultHandle,
    uint32_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblTitleStorageBlobMetadataResultGetNextResult(
    XAsyncBlock* async,
    XblTitleStorageBlobMetadataResultHandle* result);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblTitleStorageBlobMetadataResultDuplicateHandle(
    XblTitleStorageBlobMetadataResultHandle handle,
    XblTitleStorageBlobMetadataResultHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL
XblTitleStorageBlobMetadataResultCloseHandle(
    XblTitleStorageBlobMetadataResultHandle handle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageDeleteBlobAsync(
    XblContextHandle xboxLiveContext,
    XblTitleStorageBlobMetadata blobMetadata,
    bool deleteOnlyIfEtagMatches,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageDownloadBlobAsync(
    XblContextHandle xboxLiveContext,
    XblTitleStorageBlobMetadata blobMetadata,
    uint8_t* blobBuffer,
    size_t blobBufferCount,
    XblTitleStorageETagMatchCondition etagMatchCondition,
    const char* selectQuery,
    size_t preferredDownloadBlockSize,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageDownloadBlobResult(
    XAsyncBlock* async,
    XblTitleStorageBlobMetadata* blobMetadata);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageUploadBlobAsync(
    XblContextHandle xboxLiveContext,
    XblTitleStorageBlobMetadata blobMetadata,
    const uint8_t* blobBuffer,
    size_t blobBufferCount,
    XblTitleStorageETagMatchCondition etagMatchCondition,
    size_t preferredUploadBlockSize,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageUploadBlobResult(
    XAsyncBlock* async,
    XblTitleStorageBlobMetadata* blobMetadata);

// ---------------------------------------------------------------------------
// Privacy
// ---------------------------------------------------------------------------
enum XblPermission : uint32_t {
    XblPermission_Unknown = 0,
    XblPermission_CommunicateUsingText = 1,
    XblPermission_CommunicateUsingVideo = 2,
    XblPermission_CommunicateUsingVoice = 3,
    XblPermission_ViewTargetProfile = 4,
    XblPermission_ViewTargetGameHistory = 5,
    XblPermission_ViewTargetVideoHistory = 6,
    XblPermission_ViewTargetMusicHistory = 7,
    XblPermission_ViewTargetExerciseInfo = 8,
    XblPermission_ViewTargetPresence = 9,
    XblPermission_ViewTargetVideoStatus = 10,
    XblPermission_ViewTargetMusicStatus = 11,
    XblPermission_PlayMultiplayer = 12,
    XblPermission_ViewTargetUserCreatedContent = 14,
    XblPermission_BroadcastWithTwitch = 15,
    XblPermission_WriteComment = 16,
    XblPermission_ShareItem = 17,
    XblPermission_ShareTargetContentToExternalNetworks = 18,
};

enum XblAnonymousUserType : uint32_t {
    XblAnonymousUserType_Unknown = 0,
    XblAnonymousUserType_CrossNetworkUser = 1,
    XblAnonymousUserType_CrossNetworkFriend = 2,
};

enum XblPermissionDenyReason : uint32_t {
    XblPermissionDenyReason_Unknown = 0,
    XblPermissionDenyReason_NotAllowed = 1,
    XblPermissionDenyReason_MissingPrivilege = 2,
    XblPermissionDenyReason_PrivilegeRestrictsTarget = 3,
    XblPermissionDenyReason_BlockListRestrictsTarget = 4,
    XblPermissionDenyReason_MuteListRestrictsTarget = 5,
    XblPermissionDenyReason_PrivacySettingsRestrictsTarget = 6,
    XblPermissionDenyReason_CrossNetworkUserMustBeFriend = 7,
};

struct XblPermissionDenyReasonDetails {
    XblPermissionDenyReason reason;
    uint32_t restrictedPrivilege;
    uint32_t restrictedPrivacySetting;
};

struct XblPermissionCheckResult {
    bool isAllowed;
    uint64_t targetXuid;
    XblAnonymousUserType targetUserType;
    XblPermission permissionRequested;
    XblPermissionDenyReasonDetails* reasons;
    size_t reasonsCount;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetAvoidListAsync(
    XblContextHandle xboxLiveContext,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetAvoidListResultCount(
    XAsyncBlock* async,
    size_t* xuidCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetAvoidListResult(
    XAsyncBlock* async,
    size_t xuidCount,
    uint64_t* xuids);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetMuteListAsync(
    XblContextHandle xboxLiveContext,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetMuteListResultCount(
    XAsyncBlock* async,
    size_t* xuidCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetMuteListResult(
    XAsyncBlock* async,
    size_t xuidCount,
    uint64_t* xuids);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionAsync(
    XblContextHandle xboxLiveContext,
    XblPermission permissionToCheck,
    uint64_t targetXuid,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblPermissionCheckResult** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPrivacyCheckPermissionForAnonymousUserAsync(
    XblContextHandle xboxLiveContext,
    XblPermission permissionToCheck,
    XblAnonymousUserType userType,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPrivacyCheckPermissionForAnonymousUserResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPrivacyCheckPermissionForAnonymousUserResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblPermissionCheckResult** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyBatchCheckPermissionAsync(
    XblContextHandle xboxLiveContext,
    XblPermission* permissionsToCheck,
    size_t permissionsCount,
    uint64_t* targetXuids,
    size_t xuidsCount,
    XblAnonymousUserType* targetAnonymousUserTypes,
    size_t targetAnonymousUserTypesCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblPrivacyBatchCheckPermissionResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyBatchCheckPermissionResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblPermissionCheckResult** results,
    size_t* resultsCount,
    size_t* bufferUsed);

// ---------------------------------------------------------------------------
// Social Relationships & Social Manager
// ---------------------------------------------------------------------------
typedef struct XblSocialRelationshipResult* XblSocialRelationshipResultHandle;
typedef struct XblSocialManagerUserGroup* XblSocialManagerUserGroupHandle;

enum XblSocialRelationshipFilter : uint32_t {
    XblSocialRelationshipFilter_All = 0,
    XblSocialRelationshipFilter_Favorite = 1,
    XblSocialRelationshipFilter_LegacyXboxLiveFriends = 2,
};

enum XblSocialNotificationType : uint32_t {
    XblSocialNotificationType_Unknown = 0,
    XblSocialNotificationType_Added = 1,
    XblSocialNotificationType_Changed = 2,
    XblSocialNotificationType_Removed = 3,
};

enum XblReputationFeedbackType : uint32_t {
    XblReputationFeedbackType_FairPlayKillsTeammates = 0,
    XblReputationFeedbackType_FairPlayCheater = 1,
    XblReputationFeedbackType_FairPlayTampering = 2,
    XblReputationFeedbackType_FairPlayQuitter = 3,
    XblReputationFeedbackType_FairPlayKicked = 4,
    XblReputationFeedbackType_CommunicationsInappropriateVideo = 5,
    XblReputationFeedbackType_CommunicationsAbusiveVoice = 6,
    XblReputationFeedbackType_InappropriateUserGeneratedContent = 7,
    XblReputationFeedbackType_PositiveSkilledPlayer = 8,
    XblReputationFeedbackType_PositiveHelpfulPlayer = 9,
    XblReputationFeedbackType_PositiveHighQualityUserGeneratedContent = 10,
    XblReputationFeedbackType_CommsPhishing = 11,
    XblReputationFeedbackType_CommsPictureMessage = 12,
    XblReputationFeedbackType_CommsSpam = 13,
    XblReputationFeedbackType_CommsTextMessage = 14,
    XblReputationFeedbackType_CommsVoiceMessage = 15,
    XblReputationFeedbackType_FairPlayConsoleBanRequest = 16,
    XblReputationFeedbackType_FairPlayIdler = 17,
    XblReputationFeedbackType_FairPlayUserBanRequest = 18,
    XblReputationFeedbackType_UserContentGamerpic = 19,
    XblReputationFeedbackType_UserContentPersonalInfo = 20,
    XblReputationFeedbackType_FairPlayUnsporting = 21,
    XblReputationFeedbackType_FairPlayLeaderboardCheater = 22,
};

struct XblSocialRelationship {
    uint64_t xboxUserId;
    bool isFavorite;
    bool isFollowingCaller;
    const char** socialNetworks;
    size_t socialNetworksCount;
};

struct XblSocialRelationshipChangeEventArgs {
    uint64_t callerXboxUserId;
    XblSocialNotificationType socialNotification;
    uint64_t* xboxUserIds;
    size_t xboxUserIdsCount;
};

typedef void(OPENSCARLETT_XSAPI_CALL* XblSocialRelationshipChangedHandler)(
    const XblSocialRelationshipChangeEventArgs* eventArgs,
    void* context);

struct XblReputationFeedbackItem {
    uint64_t xboxUserId;
    XblReputationFeedbackType feedbackType;
    const char* sessionName;
    const char* reasonMessage;
    const char* evidenceResourceId;
};

enum XblSocialManagerExtraDetailLevel : uint32_t {
    XblSocialManagerExtraDetailLevel_NoExtraDetail = 0x0,
    XblSocialManagerExtraDetailLevel_TitleHistoryLevel = 0x1,
    XblSocialManagerExtraDetailLevel_PreferredColorLevel = 0x2,
    XblSocialManagerExtraDetailLevel_All = 0x3,
};

enum XblPresenceFilter : uint32_t {
    XblPresenceFilter_Unknown = 0,
    XblPresenceFilter_TitleOnline = 1,
    XblPresenceFilter_TitleOffline = 2,
    XblPresenceFilter_AllOnline = 3,
    XblPresenceFilter_AllOffline = 4,
    XblPresenceFilter_AllTitle = 5,
    XblPresenceFilter_All = 6,
};

enum XblRelationshipFilter : uint32_t {
    XblRelationshipFilter_Friends = 0,
    XblRelationshipFilter_Favorite = 1,
};

enum XblSocialUserGroupType : uint32_t {
    XblSocialUserGroupType_FilterType = 0,
    XblSocialUserGroupType_UserListType = 1,
};

enum XblSocialManagerEventType : uint32_t {
    XblSocialManagerEventType_UsersAddedToSocialGraph = 0,
    XblSocialManagerEventType_UsersRemovedFromSocialGraph = 1,
    XblSocialManagerEventType_PresenceChanged = 2,
    XblSocialManagerEventType_ProfilesChanged = 3,
    XblSocialManagerEventType_SocialRelationshipsChanged = 4,
    XblSocialManagerEventType_LocalUserAdded = 5,
    XblSocialManagerEventType_SocialUserGroupLoaded = 6,
    XblSocialManagerEventType_SocialUserGroupUpdated = 7,
    XblSocialManagerEventType_UnknownEvent = 8,
};

struct XblPreferredColor {
    char primaryColor[XBL_COLOR_CHAR_SIZE];
    char secondaryColor[XBL_COLOR_CHAR_SIZE];
    char tertiaryColor[XBL_COLOR_CHAR_SIZE];
};

struct XblTitleHistory {
    bool hasUserPlayed;
    int64_t lastTimeUserPlayed;
};

struct XblSocialManagerPresenceTitleRecord {
    uint32_t titleId;
    bool isTitleActive;
    char presenceText[XBL_RICH_PRESENCE_CHAR_SIZE];
    bool isBroadcasting;
    XblPresenceDeviceType deviceType;
    bool isPrimary;
};

constexpr size_t XBL_NUM_PRESENCE_RECORDS = 6;

struct XblSocialManagerPresenceRecord {
    XblPresenceUserState userState;
    XblSocialManagerPresenceTitleRecord presenceTitleRecords[XBL_NUM_PRESENCE_RECORDS];
    uint32_t presenceTitleRecordCount;
};

struct XblSocialManagerUser {
    uint64_t xboxUserId;
    bool isFavorite;
    bool isFollowingUser;
    bool isFollowedByCaller;
    char displayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char realName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char displayPicUrlRaw[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    bool useAvatar;
    char gamerscore[XBL_GAMERSCORE_CHAR_SIZE];
    char gamertag[XBL_GAMERTAG_CHAR_SIZE];
    char modernGamertag[XBL_MODERN_GAMERTAG_CHAR_SIZE];
    char modernGamertagSuffix[XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE];
    char uniqueModernGamertag[XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE];
    XblSocialManagerPresenceRecord presenceRecord;
    XblTitleHistory titleHistory;
    XblPreferredColor preferredColor;
};

constexpr size_t XBL_SOCIAL_MANAGER_MAX_AFFECTED_USERS_PER_EVENT = 10;

struct XblSocialManagerEvent {
    XUserHandle user;
    XblSocialManagerEventType eventType;
    XGameResult hr;
    XblSocialManagerUserGroupHandle groupAffected;
    XblSocialManagerUser* usersAffected[XBL_SOCIAL_MANAGER_MAX_AFFECTED_USERS_PER_EVENT];
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialGetSocialRelationshipsAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XblSocialRelationshipFilter socialRelationshipFilter,
    size_t startIndex,
    size_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialGetSocialRelationshipsResult(
    XAsyncBlock* async,
    XblSocialRelationshipResultHandle* handle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultGetRelationships(
    XblSocialRelationshipResultHandle resultHandle,
    const XblSocialRelationship** relationships,
    size_t* relationshipsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultHasNext(
    XblSocialRelationshipResultHandle resultHandle,
    bool* hasNext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultGetTotalCount(
    XblSocialRelationshipResultHandle resultHandle,
    size_t* totalCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultGetNextAsync(
    XblContextHandle xboxLiveContext,
    XblSocialRelationshipResultHandle resultHandle,
    size_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultGetNextResult(
    XAsyncBlock* async,
    XblSocialRelationshipResultHandle* handle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultDuplicateHandle(
    XblSocialRelationshipResultHandle handle,
    XblSocialRelationshipResultHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultCloseHandle(XblSocialRelationshipResultHandle resultHandle);
OPENSCARLETT_XSAPI_API XblFunctionContext OPENSCARLETT_XSAPI_CALL
XblSocialAddSocialRelationshipChangedHandler(
    XblContextHandle xboxLiveContext,
    XblSocialRelationshipChangedHandler handler,
    void* handlerContext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRemoveSocialRelationshipChangedHandler(
    XblContextHandle xboxLiveContext,
    XblFunctionContext handlerContext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialSubscribeToSocialRelationshipChange(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialUnsubscribeFromSocialRelationshipChange(
    XblContextHandle xboxLiveContext,
    XblRealTimeActivitySubscriptionHandle subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialSubmitReputationFeedbackAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XblReputationFeedbackType reputationFeedbackType,
    const char* sessionName,
    const char* reasonMessage,
    const char* evidenceResourceId,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialSubmitBatchReputationFeedbackAsync(
    XblContextHandle xboxLiveContext,
    const XblReputationFeedbackItem* feedbackItems,
    size_t feedbackItemsCount,
    XAsyncBlock* async);

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerAddLocalUser(
    XUserHandle user,
    XblSocialManagerExtraDetailLevel extraLevelDetail,
    XTaskQueueHandle queue);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerRemoveLocalUser(
    XUserHandle user);
OPENSCARLETT_XSAPI_API size_t OPENSCARLETT_XSAPI_CALL XblSocialManagerGetLocalUserCount(void);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerGetLocalUsers(
    size_t usersCount,
    XUserHandle* users);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialManagerCreateSocialUserGroupFromFilters(
    XUserHandle user,
    XblPresenceFilter presenceFilter,
    XblRelationshipFilter relationshipFilter,
    XblSocialManagerUserGroupHandle* group);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialManagerCreateSocialUserGroupFromList(
    XUserHandle user,
    uint64_t* xboxUserIdList,
    size_t xboxUserIdListCount,
    XblSocialManagerUserGroupHandle* group);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerDestroySocialUserGroup(
    XblSocialManagerUserGroupHandle group);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerDoWork(
    const XblSocialManagerEvent** socialEvents,
    size_t* socialEventsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetUsers(
    XblSocialManagerUserGroupHandle group,
    const XblSocialManagerUser* const** xboxSocialUsers,
    size_t* xboxSocialUsersCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetLocalUser(
    XblSocialManagerUserGroupHandle group,
    XUserHandle* localUser);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetType(
    XblSocialManagerUserGroupHandle group,
    XblSocialUserGroupType* type);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetFilters(
    XblSocialManagerUserGroupHandle group,
    XblPresenceFilter* presenceFilter,
    XblRelationshipFilter* relationshipFilter);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialManagerUserGroupGetUsersTrackedByGroup(
    XblSocialManagerUserGroupHandle group,
    const uint64_t** trackedUsers,
    size_t* trackedUsersCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUpdateSocialUserGroup(
    XblSocialManagerUserGroupHandle group,
    uint64_t* users,
    size_t usersCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialManagerSetRichPresencePollingStatus(
    XUserHandle user,
    bool shouldEnablePolling);
OPENSCARLETT_XSAPI_API bool OPENSCARLETT_XSAPI_CALL
XblSocialManagerPresenceRecordIsUserPlayingTitle(
    const XblSocialManagerPresenceRecord* presenceRecord,
    uint32_t titleId);

// ---------------------------------------------------------------------------
// HTTP Calls (XblHttpCall*)
// ---------------------------------------------------------------------------
typedef struct XblHttpCall* XblHttpCallHandle;

enum XblHttpCallResponseBodyType : uint32_t {
    XblHttpCallResponseBodyType_String = 0,
    XblHttpCallResponseBodyType_Vector = 1,
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallCreate(
    XblContextHandle xblContext,
    const char* method,
    const char* url,
    XblHttpCallHandle* call);
OPENSCARLETT_XSAPI_API XblHttpCallHandle OPENSCARLETT_XSAPI_CALL XblHttpCallDuplicateHandle(
    XblHttpCallHandle call);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblHttpCallCloseHandle(
    XblHttpCallHandle call);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallPerformAsync(
    XblHttpCallHandle call,
    XblHttpCallResponseBodyType type,
    XAsyncBlock* asyncBlock);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetHeader(
    XblHttpCallHandle call,
    const char* headerName,
    const char* headerValue,
    bool allowTracing);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRequestBodyBytes(
    XblHttpCallHandle call,
    const uint8_t* requestBodyBytes,
    uint32_t requestBodySize);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRequestBodyString(
    XblHttpCallHandle call,
    const char* requestBodyString);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRetryAllowed(
    XblHttpCallHandle call,
    bool retryAllowed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRetryCacheId(
    XblHttpCallHandle call,
    uint32_t retryAfterCacheId);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetLongHttpCall(
    XblHttpCallHandle call,
    bool longHttpCall);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallSetTracing(
    XblHttpCallHandle call,
    bool traceCall);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetRequestUrl(
    XblHttpCallHandle call,
    const char** url);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetStatusCode(
    XblHttpCallHandle call,
    uint32_t* statusCode);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetNetworkErrorCode(
    XblHttpCallHandle call,
    XGameResult* networkErrorCode,
    uint32_t* platformNetworkErrorCode);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblHttpCallGetPlatformNetworkErrorMessage(
    XblHttpCallHandle call,
    const char** platformNetworkErrorMessage);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetNumHeaders(
    XblHttpCallHandle call,
    uint32_t* numHeaders);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetHeaderAtIndex(
    XblHttpCallHandle call,
    uint32_t headerIndex,
    const char** headerName,
    const char** headerValue);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetHeader(
    XblHttpCallHandle call,
    const char* headerName,
    const char** headerValue);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetResponseBodyBytesSize(
    XblHttpCallHandle call,
    size_t* bufferSize);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetResponseBodyBytes(
    XblHttpCallHandle call,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetResponseString(
    XblHttpCallHandle call,
    const char** responseString);

// ---------------------------------------------------------------------------
// User Statistics & Title-Managed Statistics
// ---------------------------------------------------------------------------
struct XblStatistic {
    const char* statisticName;
    const char* statisticType;
    const char* value;
};

struct XblServiceConfigurationStatistic {
    char serviceConfigurationId[XBL_SCID_LENGTH];
    XblStatistic* statistics;
    size_t statisticsCount;
};

struct XblUserStatisticsResult {
    uint64_t xboxUserId;
    XblServiceConfigurationStatistic* serviceConfigStatistics;
    size_t serviceConfigStatisticsCount;
};

struct XblRequestedStatistics {
    char serviceConfigurationId[XBL_SCID_LENGTH];
    const char** statistics;
    uint32_t statisticsCount;
};

struct XblStatisticChangeEventArgs {
    uint64_t xboxUserId;
    char serviceConfigurationId[XBL_SCID_LENGTH];
    XblStatistic latestStatistic;
};

typedef void(OPENSCARLETT_XSAPI_CALL* XblStatisticChangedHandler)(
    XblStatisticChangeEventArgs eventArgs,
    void* context);

enum XblTitleManagedStatType : uint32_t {
    XblTitleManagedStatType_Number = 0,
    XblTitleManagedStatType_String = 1,
};

struct XblTitleManagedStatistic {
    const char* statisticName;
    XblTitleManagedStatType statisticType;
    double numberValue;
    const char* stringValue;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticsAsync(
    XblContextHandle xblContextHandle,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticsResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticsResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsAsync(
    XblContextHandle xblContextHandle,
    uint64_t* xboxUserIds,
    size_t xboxUserIdsCount,
    const char* serviceConfigurationId,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** results,
    size_t* resultsCount,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync(
    XblContextHandle xblContextHandle,
    uint64_t* xboxUserIds,
    uint32_t xboxUserIdsCount,
    const XblRequestedStatistics* requestedServiceConfigurationStatisticsCollection,
    uint32_t requestedServiceConfigurationStatisticsCollectionCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** results,
    size_t* resultsCount,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XblFunctionContext OPENSCARLETT_XSAPI_CALL
XblUserStatisticsAddStatisticChangedHandler(
    XblContextHandle xblContextHandle,
    XblStatisticChangedHandler handler,
    void* handlerContext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsRemoveStatisticChangedHandler(
    XblContextHandle xblContextHandle,
    XblFunctionContext handlerContext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsSubscribeToStatisticChange(
    XblContextHandle xblContextHandle,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsUnsubscribeFromStatisticChange(
    XblContextHandle xblContextHandle,
    XblRealTimeActivitySubscriptionHandle subscriptionHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsUpdateStatsAsync(
    XblContextHandle xblContextHandle,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsDeleteStatsAsync(
    XblContextHandle xblContextHandle,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async);

// ---------------------------------------------------------------------------
// String Verification (XblString*)
// ---------------------------------------------------------------------------
enum XblVerifyStringResultCode : uint32_t {
    XblVerifyStringResultCode_Success = 0,
    XblVerifyStringResultCode_Offensive = 1,
    XblVerifyStringResultCode_TooLong = 2,
    XblVerifyStringResultCode_UnknownError = 3,
};

struct XblVerifyStringResult {
    XblVerifyStringResultCode resultCode;
    char* firstOffendingSubstring;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringAsync(
    XblContextHandle xboxLiveContext,
    const char* stringToVerify,
    XAsyncBlock* asyncBlock);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringResultSize(
    XAsyncBlock* asyncBlock,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringResult(
    XAsyncBlock* asyncBlock,
    size_t bufferSize,
    void* buffer,
    XblVerifyStringResult** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringsAsync(
    XblContextHandle xboxLiveContext,
    const char** stringsToVerify,
    uint64_t stringsCount,
    XAsyncBlock* asyncBlock);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringsResultSize(
    XAsyncBlock* asyncBlock,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringsResult(
    XAsyncBlock* asyncBlock,
    size_t bufferSize,
    void* buffer,
    XblVerifyStringResult** ptrToBufferStrings,
    size_t* stringsCount,
    size_t* bufferUsed);

#ifdef __cplusplus
}
#endif
