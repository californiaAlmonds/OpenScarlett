#pragma once

// Microsoft GDK xgameruntime.dll ABI definitions for OpenScarlett.
// Self-contained (does not include <windows.h>) to avoid Windows SDK collisions.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_XGAMERUNTIME)
#    define OPENSCARLETT_GDK_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_GDK_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_GDK_CALL __stdcall
#else
#  define OPENSCARLETT_GDK_API __attribute__((visibility("default")))
#  define OPENSCARLETT_GDK_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard HRESULT-compatible result type (32-bit signed integer).
typedef int32_t XGameResult;

// Standard Win32 / COM / GDK HRESULT codes.
constexpr XGameResult XGR_S_OK = 0x00000000;
constexpr XGameResult XGR_S_FALSE = 0x00000001;
constexpr XGameResult XGR_E_NOTIMPL = static_cast<XGameResult>(0x80004001u);
constexpr XGameResult XGR_E_NOINTERFACE = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult XGR_E_POINTER = static_cast<XGameResult>(0x80004003u);
constexpr XGameResult XGR_E_ABORT = static_cast<XGameResult>(0x80004004u);
constexpr XGameResult XGR_E_FAIL = static_cast<XGameResult>(0x80004005u);
constexpr XGameResult XGR_E_PENDING = static_cast<XGameResult>(0x8000000Au);
constexpr XGameResult XGR_E_ACCESSDENIED = static_cast<XGameResult>(0x80070005u);
constexpr XGameResult XGR_E_HANDLE = static_cast<XGameResult>(0x80070006u);
constexpr XGameResult XGR_E_OUTOFMEMORY = static_cast<XGameResult>(0x8007000Eu);
constexpr XGameResult XGR_E_NOT_SUPPORTED = static_cast<XGameResult>(0x80070032u);
constexpr XGameResult XGR_E_INVALIDARG = static_cast<XGameResult>(0x80070057u);
constexpr XGameResult XGR_E_NOT_SUFFICIENT_BUFFER = static_cast<XGameResult>(0x8007007Au);
constexpr XGameResult XGR_E_NO_MORE_ITEMS = static_cast<XGameResult>(0x80070103u);
constexpr XGameResult XGR_E_NOT_FOUND = static_cast<XGameResult>(0x80070490u);
constexpr XGameResult XGR_E_GAMERUNTIME_NOT_INITIALIZED = static_cast<XGameResult>(0x89240100u);
constexpr XGameResult XGR_E_GS_UPDATE_TOO_BIG = static_cast<XGameResult>(0x80830002u);
constexpr XGameResult XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<XGameResult>(0x80830007u);
constexpr XGameResult XGR_E_GS_BLOB_NOT_FOUND = static_cast<XGameResult>(0x80830008u);
constexpr XGameResult XGR_E_GS_CONTAINER_NOT_IN_SYNC = static_cast<XGameResult>(0x8083000Bu);

// ---------------------------------------------------------------------------
// Game Runtime Lifecycle
// ---------------------------------------------------------------------------
enum XGameRuntimeFeature : uint32_t {
    XGameRuntimeFeature_XAccessibility = 0,
    XGameRuntimeFeature_XAppCapture = 1,
    XGameRuntimeFeature_XAsync = 2,
    XGameRuntimeFeature_XAsyncProvider = 3,
    XGameRuntimeFeature_XDisplay = 4,
    XGameRuntimeFeature_XGame = 5,
    XGameRuntimeFeature_XGameInvite = 6,
    XGameRuntimeFeature_XGameSave = 7,
    XGameRuntimeFeature_XGameUI = 8,
    XGameRuntimeFeature_XLauncher = 9,
    XGameRuntimeFeature_XNetworking = 10,
    XGameRuntimeFeature_XPackage = 11,
    XGameRuntimeFeature_XPersistentLocalStorage = 12,
    XGameRuntimeFeature_XSpeechSynthesizer = 13,
    XGameRuntimeFeature_XStore = 14,
    XGameRuntimeFeature_XSystem = 15,
    XGameRuntimeFeature_XTaskQueue = 16,
    XGameRuntimeFeature_XThread = 17,
    XGameRuntimeFeature_XUser = 18,
    XGameRuntimeFeature_XError = 19,
    XGameRuntimeFeature_XGameStreaming = 20,
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameRuntimeInitialize(void);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameRuntimeUninitialize(void);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XGameRuntimeIsFeatureAvailable(
    XGameRuntimeFeature feature);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameGetXboxTitleId(uint32_t* titleId);

// ---------------------------------------------------------------------------
// XTaskQueue
// ---------------------------------------------------------------------------
typedef struct XTaskQueueObject* XTaskQueueHandle;
typedef struct XTaskQueuePortObject* XTaskQueuePortHandle;

enum XTaskQueueDispatchMode : uint32_t {
    XTaskQueueDispatchMode_ThreadPool = 0,
    XTaskQueueDispatchMode_SerializedThreadPool = 1,
    XTaskQueueDispatchMode_Manual = 2,
    XTaskQueueDispatchMode_Immediate = 3,
};

enum XTaskQueuePort : uint32_t {
    XTaskQueuePort_Work = 0,
    XTaskQueuePort_Completion = 1,
};

struct XTaskQueueRegistrationToken {
    uint64_t token;
};

typedef void(OPENSCARLETT_GDK_CALL* XTaskQueueCallback)(void* context, bool canceled);
typedef void(OPENSCARLETT_GDK_CALL* XTaskQueueMonitorCallback)(
    void* context,
    XTaskQueueHandle queue,
    XTaskQueuePort port);
typedef void(OPENSCARLETT_GDK_CALL* XTaskQueueTerminatedCallback)(void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueCloseHandle(XTaskQueueHandle queue);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XTaskQueueGetCurrentProcessTaskQueue(
    XTaskQueueHandle* queue);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueSetCurrentProcessTaskQueue(
    XTaskQueueHandle queue);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback callback);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* waitHandle,
    void* callbackContext,
    XTaskQueueCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);

// ---------------------------------------------------------------------------
// XAsync
// ---------------------------------------------------------------------------
struct XAsyncBlock;
typedef void(OPENSCARLETT_GDK_CALL* XAsyncCompletionRoutine)(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine callback;
    unsigned char internal[sizeof(void*) * 4];
};

enum XAsyncOp : uint32_t {
    XAsyncOp_Begin = 0,
    XAsyncOp_DoWork = 1,
    XAsyncOp_GetResult = 2,
    XAsyncOp_Cancel = 3,
    XAsyncOp_Cleanup = 4,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    size_t bufferSize;
    void* buffer;
    void* context;
};

typedef XGameResult(OPENSCARLETT_GDK_CALL* XAsyncProvider)(
    XAsyncOp op,
    const XAsyncProviderData* data);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider provider);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XAsyncComplete(
    XAsyncBlock* asyncBlock,
    XGameResult result,
    size_t requiredBufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XAsyncCancel(XAsyncBlock* asyncBlock);

typedef XGameResult(OPENSCARLETT_GDK_CALL* XAsyncWork)(XAsyncBlock* asyncBlock);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncRun(
    XAsyncBlock* asyncBlock,
    XAsyncWork work);

// ---------------------------------------------------------------------------
// XUser
// ---------------------------------------------------------------------------
typedef struct XUser* XUserHandle;

struct XUserLocalId {
    uint64_t value;
};

enum XUserAddOptions : uint32_t {
    XUserAddOptions_None = 0x00,
    XUserAddOptions_AddDefaultUserSilently = 0x01,
    XUserAddOptions_AllowGuests = 0x02,
    XUserAddOptions_AddDefaultUserAllowingUI = 0x04,
};

enum XUserGamertagComponent : uint32_t {
    XUserGamertagComponent_Classic = 0,
    XUserGamertagComponent_Modern = 1,
    XUserGamertagComponent_ModernSuffix = 2,
    XUserGamertagComponent_UniqueModern = 3,
};

enum XUserState : uint32_t {
    XUserState_SignedIn = 0,
    XUserState_SigningOut = 1,
    XUserState_SignedOut = 2,
};

enum XUserAgeGroup : uint32_t {
    XUserAgeGroup_Unknown = 0,
    XUserAgeGroup_Child = 1,
    XUserAgeGroup_Teen = 2,
    XUserAgeGroup_Adult = 3,
};

enum XUserPrivilegeOptions : uint32_t {
    XUserPrivilegeOptions_None = 0x00,
    XUserPrivilegeOptions_AllUsers = 0x01,
};

enum XUserPrivilege : uint32_t {
    XUserPrivilege_CrossPlay = 185,
    XUserPrivilege_Clubs = 188,
    XUserPrivilege_Sessions = 189,
    XUserPrivilege_Broadcast = 190,
    XUserPrivilege_ManageProfilePrivacy = 196,
    XUserPrivilege_GameDvr = 198,
    XUserPrivilege_MultiplayerParties = 203,
    XUserPrivilege_CloudManageSession = 207,
    XUserPrivilege_CloudJoinSession = 208,
    XUserPrivilege_CloudSavedGames = 209,
    XUserPrivilege_SocialNetworkSharing = 220,
    XUserPrivilege_UserGeneratedContent = 247,
    XUserPrivilege_Communications = 252,
    XUserPrivilege_Multiplayer = 254,
    XUserPrivilege_AddFriends = 255,
};

enum XUserPrivilegeDenyReason : uint32_t {
    XUserPrivilegeDenyReason_None = 0,
    XUserPrivilegeDenyReason_PurchaseRequired = 1,
    XUserPrivilegeDenyReason_Restricted = 2,
    XUserPrivilegeDenyReason_Banned = 3,
    XUserPrivilegeDenyReason_Unknown = 0xFFFFFFFFu,
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserAddAsync(
    XUserAddOptions options,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XUserCloseHandle(XUserHandle user);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetId(
    XUserHandle user,
    uint64_t* userId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetLocalId(
    XUserHandle user,
    XUserLocalId* userLocalId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetState(
    XUserHandle user,
    XUserState* state);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetAgeGroup(
    XUserHandle user,
    XUserAgeGroup* ageGroup);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetMaxUsers(uint32_t* maxUsers);
enum XUserGamerPictureSize : uint32_t {
    XUserGamerPictureSize_Small = 0,
    XUserGamerPictureSize_Medium = 1,
    XUserGamerPictureSize_Large = 2,
    XUserGamerPictureSize_ExtraLarge = 3,
};

constexpr size_t XUserGamertagComponentClassicMaxBytes = 16;
constexpr size_t XUserGamertagComponentModernMaxBytes = 97;
constexpr size_t XUserGamertagComponentModernSuffixMaxBytes = 15;
constexpr size_t XUserGamertagComponentUniqueModernMaxBytes = 101;

OPENSCARLETT_GDK_API int32_t OPENSCARLETT_GDK_CALL XUserCompare(
    XUserHandle user1,
    XUserHandle user2);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed);

struct XUserAppLocalDeviceId {
    uint8_t value[32];
};
static_assert(sizeof(XUserAppLocalDeviceId) == 32, "XUserAppLocalDeviceId must be 32 bytes");

enum XUserGetTokenAndSignatureOptions : uint32_t {
    XUserGetTokenAndSignatureOptions_None = 0x00,
    XUserGetTokenAndSignatureOptions_ForceRefresh = 0x01,
    XUserGetTokenAndSignatureOptions_AllUsers = 0x02,
};

struct XUserGetTokenAndSignatureHttpHeader {
    const char* name;
    const char* value;
};

struct XUserGetTokenAndSignatureData {
    size_t tokenSize;
    size_t signatureSize;
    const char* token;
    const char* signature;
};

struct XUserGetTokenAndSignatureUtf16HttpHeader {
    const wchar_t* name;
    const wchar_t* value;
};

struct XUserGetTokenAndSignatureUtf16Data {
    size_t tokenCount;
    size_t signatureCount;
    const wchar_t* token;
    const wchar_t* signature;
};

enum XUserDefaultAudioEndpointKind : uint32_t {
    XUserDefaultAudioEndpointKind_CommunicationRender = 0,
    XUserDefaultAudioEndpointKind_CommunicationCapture = 1,
};

enum XUserChangeEvent : uint32_t {
    XUserChangeEvent_SignedInAgain = 0,
    XUserChangeEvent_SigningOut = 1,
    XUserChangeEvent_SignedOut = 2,
    XUserChangeEvent_Gamertag = 3,
    XUserChangeEvent_GamerPicture = 4,
    XUserChangeEvent_Privileges = 5,
};

struct XUserDeviceAssociationChange {
    XUserAppLocalDeviceId deviceId;
    XUserLocalId oldUser;
    XUserLocalId newUser;
};

typedef struct XUserSignOutDeferral* XUserSignOutDeferralHandle;

typedef void(OPENSCARLETT_GDK_CALL* XUserChangeEventCallback)(
    void* context,
    XUserLocalId userLocalId,
    XUserChangeEvent event);
typedef void(OPENSCARLETT_GDK_CALL* XUserDefaultAudioEndpointUtf16ChangedCallback)(
    void* context,
    XUserLocalId user,
    XUserDefaultAudioEndpointKind defaultAudioEndpointKind,
    const wchar_t* endpointIdUtf16);
typedef void(OPENSCARLETT_GDK_CALL* XUserDeviceAssociationChangedCallback)(
    void* context,
    const XUserDeviceAssociationChange* change);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetIsGuest(
    XUserHandle user,
    bool* isGuest);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserFindUserById(
    uint64_t userId,
    XUserHandle* handle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserFindUserByLocalId(
    XUserLocalId userLocalId,
    XUserHandle* handle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserFindForDevice(
    const XUserAppLocalDeviceId* deviceId,
    XUserHandle* handle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureAsync(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const char* method,
    const char* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureHttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const wchar_t* method,
    const wchar_t* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureUtf16ResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureUtf16Result(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetDefaultAudioEndpointUtf16(
    XUserLocalId user,
    XUserDefaultAudioEndpointKind defaultAudioEndpointKind,
    size_t endpointIdUtf16Count,
    wchar_t* endpointIdUtf16,
    size_t* endpointIdUtf16Used);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetSignOutDeferral(
    XUserSignOutDeferralHandle* deferral);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XUserCloseSignOutDeferralHandle(
    XUserSignOutDeferralHandle deferral);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserRegisterForChangeEvent(
    XTaskQueueHandle queue,
    void* context,
    XUserChangeEventCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XUserUnregisterForChangeEvent(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserRegisterForDefaultAudioEndpointUtf16Changed(
    XTaskQueueHandle queue,
    void* context,
    XUserDefaultAudioEndpointUtf16ChangedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XUserUnregisterForDefaultAudioEndpointUtf16Changed(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserRegisterForDeviceAssociationChanged(
    XTaskQueueHandle queue,
    void* context,
    XUserDeviceAssociationChangedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XUserUnregisterForDeviceAssociationChanged(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiAsync(
    XUserHandle user,
    const char* url,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiUtf16Async(
    XUserHandle user,
    const wchar_t* url,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiUtf16Result(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserResolvePrivilegeWithUiAsync(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserResolvePrivilegeWithUiResult(
    XAsyncBlock* async);

// ---------------------------------------------------------------------------
// XGameSave
// ---------------------------------------------------------------------------
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

typedef bool(OPENSCARLETT_GDK_CALL* XGameSaveContainerInfoCallback)(
    const XGameSaveContainerInfo* container,
    void* context);
typedef bool(OPENSCARLETT_GDK_CALL* XGameSaveBlobInfoCallback)(
    const XGameSaveBlobInfo* info,
    void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XGameSaveProviderHandle* provider);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameSaveCloseProvider(
    XGameSaveProviderHandle provider);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* result);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameSaveCloseContainer(
    XGameSaveContainerHandle context);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdate(
    XGameSaveUpdateHandle updateContext);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameSaveCloseUpdate(
    XGameSaveUpdateHandle updateContext);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateBlobInfoByName(
    XGameSaveContainerHandle container,
    const char* blobNamePrefix,
    void* context,
    XGameSaveBlobInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetContainerInfo(
    XGameSaveProviderHandle provider,
    const char* containerName,
    void* context,
    XGameSaveContainerInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdateResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* name,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainerResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota);

// ---------------------------------------------------------------------------
// XSystem
// ---------------------------------------------------------------------------
enum XSystemDeviceType : uint32_t {
    XSystemDeviceType_Unknown = 0x00,
    XSystemDeviceType_Pc = 0x01,
    XSystemDeviceType_XboxOne = 0x02,
    XSystemDeviceType_XboxOneS = 0x03,
    XSystemDeviceType_XboxOneX = 0x04,
    XSystemDeviceType_XboxOneXDevkit = 0x05,
    XSystemDeviceType_XboxScarlettLockhart = 0x06,
    XSystemDeviceType_XboxScarlettAnaconda = 0x07,
    XSystemDeviceType_XboxScarlettDevkit = 0x08,
};

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4201)
#endif
struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

static_assert(sizeof(XVersion) == 8, "XVersion must be 8 bytes");

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

constexpr size_t XSystemAppSpecificDeviceIdBytes = 45;

OPENSCARLETT_GDK_API XSystemDeviceType OPENSCARLETT_GDK_CALL XSystemGetDeviceType(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed);
OPENSCARLETT_GDK_API XSystemAnalyticsInfo OPENSCARLETT_GDK_CALL XSystemGetAnalyticsInfo(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed);
OPENSCARLETT_GDK_API XSystemRuntimeInfo OPENSCARLETT_GDK_CALL XSystemGetRuntimeInfo(void);

/// Opaque GDK handle.
typedef void* XSystemHandle;

/// What kind of GDK object a handle points at. Values match the Microsoft
/// GDK `XSystem.h` enum so real GDK clients see the expected constants.
enum XSystemHandleType : uint32_t {
    XSystemHandleType_AppCaptureScreenshotStream = 0x00,
    XSystemHandleType_DisplayTimeoutDeferral     = 0x01,
    XSystemHandleType_GameSaveContainer          = 0x02,
    XSystemHandleType_GameSaveProvider           = 0x03,
    XSystemHandleType_GameSaveUpdate             = 0x04,
    XSystemHandleType_PackageInstallationMonitor = 0x05,
    XSystemHandleType_PackageMount               = 0x06,
    XSystemHandleType_SpeechSynthesizer          = 0x07,
    XSystemHandleType_SpeechSynthesizerStream    = 0x08,
    XSystemHandleType_StoreContext               = 0x09,
    XSystemHandleType_StoreLicense               = 0x0a,
    XSystemHandleType_StoreProductQuery          = 0x0b,
    XSystemHandleType_TaskQueue                  = 0x0c,
    XSystemHandleType_User                       = 0x0d,
    XSystemHandleType_UserSignOutDeferral        = 0x0e,
    XSystemHandleType_GameUiTextEntry            = 0x0f,
    XSystemHandleType_PFXGameSaveConfig          = 0x10,
};

/// Why a handle lifecycle callback fired.
enum XSystemHandleCallbackReason : uint32_t {
    XSystemHandleCallbackReason_Created   = 0x00,
    XSystemHandleCallbackReason_Destroyed = 0x01,
};

/// Process-wide handle lifecycle observer, registered via
/// `XSystemHandleTrack` and fired for every tracked handle.
typedef void(OPENSCARLETT_GDK_CALL* XSystemHandleCallback)(
    XSystemHandle handle,
    XSystemHandleType type,
    XSystemHandleCallbackReason reason,
    void* context);

OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XSystemIsHandleValid(XSystemHandle handle);
/// Registers the process-wide handle lifecycle observer. Passing `nullptr`
/// clears/disables it — the real GDK has no `XSystemHandleUntrack` export.
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSystemHandleTrack(
    XSystemHandleCallback callback,
    void* context);

// ---------------------------------------------------------------------------
// XGameStreaming (local-PC compatibility surface)
// ---------------------------------------------------------------------------
typedef uint64_t XGameStreamingClientId;
constexpr XGameStreamingClientId XGameStreamingAllClients = 0ull;

enum XGameStreamingConnectionState : uint32_t {
    XGameStreamingConnectionState_Disconnected = 0,
    XGameStreamingConnectionState_Connected = 1,
};

typedef void(OPENSCARLETT_GDK_CALL* XGameStreamingConnectionStateChangedCallback)(
    void* context,
    XGameStreamingClientId client,
    XGameStreamingConnectionState state);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameStreamingInitialize(void);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameStreamingUninitialize(void);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XGameStreamingIsStreaming(void);
OPENSCARLETT_GDK_API uint32_t OPENSCARLETT_GDK_CALL XGameStreamingGetClientCount(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetClients(
    uint32_t clientCount,
    XGameStreamingClientId* clients,
    uint32_t* clientsUsed);
OPENSCARLETT_GDK_API XGameStreamingConnectionState OPENSCARLETT_GDK_CALL
XGameStreamingGetConnectionState(XGameStreamingClientId client);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL
XGameStreamingRegisterConnectionStateChanged(
    XTaskQueueHandle queue,
    void* context,
    XGameStreamingConnectionStateChangedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL
XGameStreamingUnregisterConnectionStateChanged(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameStreamingHideTouchControls(void);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameStreamingShowTouchControlLayout(
    const char* layout);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetStreamPhysicalDimensions(
    XGameStreamingClientId client,
    uint32_t* horizontalMm,
    uint32_t* verticalMm);
OPENSCARLETT_GDK_API size_t OPENSCARLETT_GDK_CALL XGameStreamingGetServerLocationNameSize(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetServerLocationName(
    size_t serverLocationNameSize,
    char* serverLocationName);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetLastFrameDisplayed(
    XGameStreamingClientId client,
    uint64_t* frameId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameStreamingIsTouchInputEnabledOnClient(
    XGameStreamingClientId client,
    bool* touchInputEnabled);

// ---------------------------------------------------------------------------
// XMem
// ---------------------------------------------------------------------------
typedef void*(OPENSCARLETT_GDK_CALL* XMemAllocFunction)(size_t size, uint64_t attributes);
typedef void(OPENSCARLETT_GDK_CALL* XMemFreeFunction)(void* pointer, uint64_t attributes);

OPENSCARLETT_GDK_API void* OPENSCARLETT_GDK_CALL XMemAllocDefault(
    size_t size,
    uint64_t attributes);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XMemFreeDefault(
    void* pointer,
    uint64_t attributes);
OPENSCARLETT_GDK_API void* OPENSCARLETT_GDK_CALL XMemAlloc(
    size_t size,
    uint64_t attributes);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XMemFree(
    void* pointer,
    uint64_t attributes);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XMemSetAllocationHooks(
    XMemAllocFunction allocHook,
    XMemFreeFunction freeHook);

// ---------------------------------------------------------------------------
// XPackage
// ---------------------------------------------------------------------------
enum XPackageChunkSelectorType : uint32_t {
    XPackageChunkSelectorType_Language = 1,
    XPackageChunkSelectorType_Tag = 2,
    XPackageChunkSelectorType_Feature = 3,
    XPackageChunkSelectorType_Chunk = 4,
};

struct XPackageChunkSelector {
    XPackageChunkSelectorType type;
    union {
        const char* language;
        const char* tag;
        const char* feature;
        uint32_t chunkId;
    };
};

enum XPackageChunkAvailability : uint32_t {
    XPackageChunkAvailability_Ready = 0,
    XPackageChunkAvailability_Pending = 1,
    XPackageChunkAvailability_Installable = 2,
    XPackageChunkAvailability_Unavailable = 3,
};

typedef bool(OPENSCARLETT_GDK_CALL* XPackageChunkAvailabilityCallback)(
    void* context,
    const XPackageChunkSelector* selector,
    XPackageChunkAvailability availability);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XPackageIsPackagedProcess(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    void* context,
    XPackageChunkAvailabilityCallback callback);

typedef struct XPackageMountObject* XPackageMountHandle;
typedef struct XPackageInstallationMonitor* XPackageInstallationMonitorHandle;

enum XPackageKind : uint32_t {
    XPackageKind_Game = 0,
    XPackageKind_Content = 1,
};

enum XPackageEnumerationScope : uint32_t {
    XPackageEnumerationScope_ThisOnly = 0,
    XPackageEnumerationScope_ThisAndRelated = 1,
};

struct XPackageDetails {
    const char* packageIdentifier;
    XVersion version;
    XPackageKind kind;
    const char* displayName;
    const char* description;
    const char* publisher;
    const char* storeId;
    bool installing;
    uint32_t index;
    uint32_t count;
    bool ageRestricted;
    const char* titleID;
};

struct XPackageFeature {
    const char* id;
    const char* displayName;
    const char* tags;
    bool hidden;
    uint32_t storeIdCount;
    const char* storeIds;
};

struct XPackageInstallationProgress {
    uint64_t totalBytes;
    uint64_t installedBytes;
    uint64_t launchBytes;
    bool launchable;
    bool completed;
};

struct XPackageWriteStats {
    uint64_t interval;
    uint64_t budget;
    uint64_t elapsed;
    uint64_t bytesWritten;
};

typedef bool(OPENSCARLETT_GDK_CALL* XPackageEnumerationCallback)(
    void* context,
    const XPackageDetails* details);
typedef bool(OPENSCARLETT_GDK_CALL* XPackageFeatureEnumerationCallback)(
    void* context,
    const XPackageFeature* feature);
typedef void(OPENSCARLETT_GDK_CALL* XPackageInstallationProgressCallback)(
    void* context,
    XPackageInstallationMonitorHandle monitor);
typedef void(OPENSCARLETT_GDK_CALL* XPackageInstalledCallback)(
    void* context,
    const XPackageDetails* details);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageMount(
    const char* packageIdentifier,
    XPackageMountHandle* mount);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XPackageCloseMountHandle(
    XPackageMountHandle mount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageGetMountPathSize(
    XPackageMountHandle mount,
    size_t* pathSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageGetMountPath(
    XPackageMountHandle mount,
    size_t pathSize,
    char* path);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageGetUserLocale(
    size_t localeSize,
    char* locale);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageGetWriteStats(
    XPackageWriteStats* writeStats);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageCreateInstallationMonitor(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    XTaskQueueHandle queue,
    XPackageInstallationMonitorHandle* installationMonitor);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XPackageCloseInstallationMonitorHandle(
    XPackageInstallationMonitorHandle installationMonitor);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XPackageGetInstallationProgress(
    XPackageInstallationMonitorHandle installationMonitor,
    XPackageInstallationProgress* progress);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XPackageUpdateInstallationMonitor(
    XPackageInstallationMonitorHandle installationMonitor);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageRegisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    void* context,
    XPackageInstallationProgressCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XPackageUnregisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageRegisterPackageInstalled(
    XTaskQueueHandle queue,
    void* context,
    XPackageInstalledCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XPackageUnregisterPackageInstalled(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope scope,
    void* context,
    XPackageEnumerationCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageEnumerateFeatures(
    const char* packageIdentifier,
    void* context,
    XPackageFeatureEnumerationCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageFindChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    XPackageChunkAvailability* availability);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageEstimateDownloadSize(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    uint64_t* downloadSize,
    bool* shouldPresentUserConfirmation);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageChangeChunkInstallOrder(
    const char* packageIdentifier,
    uint32_t chunkCount,
    const uint32_t* chunkIds);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageInstallChunks(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    bool suppressUserConfirmation,
    XTaskQueueHandle queue,
    XPackageInstallationMonitorHandle* installationMonitor);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageInstallChunksAsync(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    bool suppressUserConfirmation,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageInstallChunksResult(
    XAsyncBlock* async,
    XPackageInstallationMonitorHandle* installationMonitor);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageUninstallChunks(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageUninstallUWPInstance(
    const char* packageName);

// ---------------------------------------------------------------------------
// XDisplay
// ---------------------------------------------------------------------------
typedef uint32_t XDisplayHdrModePreference;
constexpr XDisplayHdrModePreference XDisplayHdrModePreference_PreferHdr = 0u;
constexpr XDisplayHdrModePreference XDisplayHdrModePreference_PreferRefreshRate = 1u;

enum XDisplayHdrModeResult : uint32_t {
    XDisplayHdrModeResult_Unknown = 0,
    XDisplayHdrModeResult_Enabled = 1,
    XDisplayHdrModeResult_Disabled = 2,
};

struct XDisplayHdrModeInfo {
    float minToneMapLuminance;
    float maxToneMapLuminance;
    float maxFullFrameToneMapLuminance;
};

typedef struct XDisplayTimeoutDeferral* XDisplayTimeoutDeferralHandle;

OPENSCARLETT_GDK_API XDisplayHdrModeResult OPENSCARLETT_GDK_CALL XDisplayTryEnableHdrMode(
    XDisplayHdrModePreference preferredMode,
    XDisplayHdrModeInfo* displayHdrModeInfo);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XDisplayAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XDisplayTryAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XDisplayCloseTimeoutDeferralHandle(
    XDisplayTimeoutDeferralHandle handle);

// ---------------------------------------------------------------------------
// XLauncher & XGameUI (Wave 1 capital-UI compatibility surface)
// ---------------------------------------------------------------------------
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XLaunchUri(
    XUserHandle requestingUser,
    const char* uri);

enum XGameUIMessageDialogButton : uint32_t {
    XGameUIMessageDialogButton_First = 0,
    XGameUIMessageDialogButton_Second = 1,
    XGameUIMessageDialogButton_Third = 2,
};

enum XGameUITextEntryInputScope : uint32_t {
    XGameUITextEntryInputScope_Default = 0,
    XGameUITextEntryInputScope_Url = 1,
    XGameUITextEntryInputScope_EmailSmtpAddress = 5,
    XGameUITextEntryInputScope_Number = 29,
    XGameUITextEntryInputScope_Password = 31,
    XGameUITextEntryInputScope_TelephoneNumber = 32,
    XGameUITextEntryInputScope_Alphanumeric = 40,
    XGameUITextEntryInputScope_Search = 50,
};

enum XGameUINotificationPositionHint : uint32_t {
    XGameUINotificationPositionHint_BottomCenter = 0,
    XGameUINotificationPositionHint_BottomLeft = 1,
    XGameUINotificationPositionHint_BottomRight = 2,
    XGameUINotificationPositionHint_TopCenter = 3,
    XGameUINotificationPositionHint_TopLeft = 4,
    XGameUINotificationPositionHint_TopRight = 5,
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowMessageDialogAsync(
    XAsyncBlock* async,
    const char* titleText,
    const char* contentText,
    const char* firstButtonText,
    const char* secondButtonText,
    const char* thirdButtonText,
    XGameUIMessageDialogButton defaultButton,
    XGameUIMessageDialogButton cancelButton);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowMessageDialogResult(
    XAsyncBlock* async,
    XGameUIMessageDialogButton* resultButton);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowTextEntryAsync(
    XAsyncBlock* async,
    const char* titleText,
    const char* descriptionText,
    const char* defaultText,
    XGameUITextEntryInputScope inputScope,
    uint32_t maxTextLength);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowTextEntryResultSize(
    XAsyncBlock* async,
    uint32_t* resultTextBufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowTextEntryResult(
    XAsyncBlock* async,
    uint32_t resultTextBufferSize,
    char* resultTextBuffer,
    uint32_t* resultTextBufferUsed);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowPlayerPickerAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* promptText,
    uint32_t selectFromPlayersCount,
    const uint64_t* selectFromPlayers,
    uint32_t preSelectedPlayersCount,
    const uint64_t* preSelectedPlayers,
    uint32_t minSelectionCount,
    uint32_t maxSelectionCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowPlayerPickerResultCount(
    XAsyncBlock* async,
    uint32_t* resultPlayersCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowPlayerPickerResult(
    XAsyncBlock* async,
    uint32_t resultPlayersCount,
    uint64_t* resultPlayers,
    uint32_t* resultPlayersUsed);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowErrorDialogAsync(
    XAsyncBlock* async,
    XGameResult errorCode,
    const char* context);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUIShowErrorDialogResult(
    XAsyncBlock* async);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUISetNotificationPositionHint(
    XGameUINotificationPositionHint position);

// ---------------------------------------------------------------------------
// XGameInvite
// ---------------------------------------------------------------------------
typedef void(OPENSCARLETT_GDK_CALL* XGameInviteEventCallback)(
    void* context,
    const char* inviteUri);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameInviteRegisterForEvent(
    XTaskQueueHandle queue,
    void* context,
    XGameInviteEventCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XGameInviteUnregisterForEvent(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameInviteDispatchTestEventForTest(
    const char* inviteUri);

// ---------------------------------------------------------------------------
// XAccessibility (Closed Captions & High Contrast)
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4201)
#endif
struct XColor {
    union {
        struct {
            uint8_t A;
            uint8_t R;
            uint8_t G;
            uint8_t B;
        };
        uint32_t Value;
    };
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
static_assert(sizeof(XColor) == 4, "XColor must be 4 bytes");

enum XClosedCaptionFontEdgeAttribute : uint32_t {
    XClosedCaptionFontEdgeAttribute_Default = 0,
    XClosedCaptionFontEdgeAttribute_NoEdges = 1,
    XClosedCaptionFontEdgeAttribute_NoEdgeAttribute = 1,
    XClosedCaptionFontEdgeAttribute_RaisedEdges = 2,
    XClosedCaptionFontEdgeAttribute_DepressedEdges = 3,
    XClosedCaptionFontEdgeAttribute_UniformBorderedEdges = 4,
    XClosedCaptionFontEdgeAttribute_UniformedEdges = 4,
    XClosedCaptionFontEdgeAttribute_DropShadowedEdges = 5,
};

enum XClosedCaptionFontStyle : uint32_t {
    XClosedCaptionFontStyle_Default = 0,
    XClosedCaptionFontStyle_MonospacedWithSerifs = 1,
    XClosedCaptionFontStyle_ProportionalWithSerifs = 2,
    XClosedCaptionFontStyle_MonospacedWithoutSerifs = 3,
    XClosedCaptionFontStyle_ProportionalWithoutSerifs = 4,
    XClosedCaptionFontStyle_Casual = 5,
    XClosedCaptionFontStyle_Cursive = 6,
    XClosedCaptionFontStyle_SmallCapitals = 7,
};

struct XClosedCaptionProperties {
    XColor BackgroundColor;
    XColor FontColor;
    XColor WindowColor;
    XClosedCaptionFontEdgeAttribute FontEdgeAttribute;
    XClosedCaptionFontStyle FontStyle;
    float FontScale;
    bool Enabled;
};

enum XHighContrastMode : uint32_t {
    XHighContrastMode_Off = 0,
    XHighContrastMode_Dark = 1,
    XHighContrastMode_Light = 2,
    XHighContrastMode_Other = 3,
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XClosedCaptionGetProperties(
    XClosedCaptionProperties* properties);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XClosedCaptionSetEnabled(
    bool enabled);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XHighContrastGetMode(
    XHighContrastMode* mode);

// ---------------------------------------------------------------------------
// XSpeechSynthesizer
// ---------------------------------------------------------------------------
typedef struct XSpeechSynthesizer* XSpeechSynthesizerHandle;
typedef struct XSpeechSynthesizerStream* XSpeechSynthesizerStreamHandle;

enum XSpeechSynthesizerVoiceGender : uint32_t {
    XSpeechSynthesizerVoiceGender_Female = 0,
    XSpeechSynthesizerVoiceGender_Male = 1,
};

struct XSpeechSynthesizerVoiceInformation {
    const char* Description;
    const char* DisplayName;
    XSpeechSynthesizerVoiceGender Gender;
    const char* VoiceId;
    const char* Language;
};

typedef bool(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerInstalledVoicesCallback)(
    const XSpeechSynthesizerVoiceInformation* information,
    void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCreate(
    XSpeechSynthesizerHandle* speechSynthesizer);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCloseHandle(
    XSpeechSynthesizerHandle speechSynthesizer);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerEnumerateInstalledVoices(
    void* context,
    XSpeechSynthesizerInstalledVoicesCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerSetDefaultVoice(
    XSpeechSynthesizerHandle speechSynthesizer);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerSetCustomVoice(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* voiceId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCreateStreamFromText(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* text,
    XSpeechSynthesizerStreamHandle* speechSynthesisStream);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCreateStreamFromSsml(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* ssml,
    XSpeechSynthesizerStreamHandle* speechSynthesisStream);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerGetStreamDataSize(
    XSpeechSynthesizerStreamHandle speechSynthesisStream,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerGetStreamData(
    XSpeechSynthesizerStreamHandle speechSynthesisStream,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCloseStreamHandle(
    XSpeechSynthesizerStreamHandle speechSynthesisStream);

// ---------------------------------------------------------------------------
// XSpeechToText (XAccessibility)
// ---------------------------------------------------------------------------
enum XSpeechToTextPositionHint : uint32_t {
    XSpeechToTextPositionHint_BottomCenter = 0,
    XSpeechToTextPositionHint_BottomLeft = 1,
    XSpeechToTextPositionHint_BottomRight = 2,
    XSpeechToTextPositionHint_MiddleRight = 3,
    XSpeechToTextPositionHint_MiddleLeft = 4,
    XSpeechToTextPositionHint_TopCenter = 5,
    XSpeechToTextPositionHint_TopLeft = 6,
    XSpeechToTextPositionHint_TopRight = 7,
};

enum XSpeechToTextType : uint32_t {
    XSpeechToTextType_Voice = 0,
    XSpeechToTextType_Text = 1,
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextSetPositionHint(
    XSpeechToTextPositionHint position);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextSendString(
    const char* speakerName,
    const char* content,
    XSpeechToTextType type);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextBeginHypothesisString(
    const char* speakerName,
    const char* content,
    XSpeechToTextType type,
    uint32_t* hypothesisId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextUpdateHypothesisString(
    uint32_t hypothesisId,
    const char* content);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextFinalizeHypothesisString(
    uint32_t hypothesisId,
    const char* content);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextCancelHypothesisString(
    uint32_t hypothesisId);

// ---------------------------------------------------------------------------
// XAppBroadcast & XAppCapture
// ---------------------------------------------------------------------------
constexpr size_t XAppCaptureMaxLocalIdSize = 1024;

typedef struct XAppCaptureScreenshotStream* XAppCaptureScreenshotStreamHandle;

struct XAppBroadcastStatus {
    bool canStartBroadcast;
    bool isAnyAppBroadcasting;
    bool isCaptureResourceUnavailable;
    bool isGameStreamInProgress;
    bool isGpuConstrained;
    bool isAppInactive;
    bool isBlockedForApp;
    bool isDisabledByUser;
    bool isDisabledBySystem;
};

enum XAppCaptureMetadataPriority : uint8_t {
    XAppCaptureMetadataPriority_Informational = 0,
    XAppCaptureMetadataPriority_Important = 1,
};

enum XAppCaptureScreenshotFormatFlag : uint16_t {
    XAppCaptureScreenshotFormatFlag_SDR = 1,
    XAppCaptureScreenshotFormatFlag_HDR = 2,
};

struct XAppCaptureScreenshotFile {
    char path[260];
    size_t fileSize;
    uint32_t width;
    uint32_t height;
};

struct XAppCaptureDiagnosticScreenshotResult {
    size_t fileCount;
    XAppCaptureScreenshotFile files[2];
};

struct XAppCaptureRecordClipResult {
    char path[260];
    size_t fileSize;
    int64_t startTime;
    uint32_t durationInMs;
    uint32_t width;
    uint32_t height;
    uint32_t encoding;
    uint32_t startTimePreciseOffsetHns;
};

struct XAppCaptureTakeScreenshotResult {
    char localId[XAppCaptureMaxLocalIdSize];
    XAppCaptureScreenshotFormatFlag availableScreenshotFormats;
};

typedef void(OPENSCARLETT_GDK_CALL* XAppBroadcastMonitorCallback)(void* context);
typedef void(OPENSCARLETT_GDK_CALL* XAppCaptureMetadataPurgedCallback)(void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppBroadcastGetStatus(
    XUserHandle requestingUser,
    XAppBroadcastStatus* appBroadcastStatus);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XAppBroadcastIsAppBroadcasting(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppBroadcastShowUI(
    XUserHandle requestingUser);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppBroadcastRegisterIsAppBroadcastingChanged(
    XTaskQueueHandle queue,
    void* context,
    XAppBroadcastMonitorCallback appBroadcastMonitorCallback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XAppBroadcastUnregisterIsAppBroadcastingChanged(
    XTaskQueueRegistrationToken token,
    bool wait);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataAddStringEvent(
    const char* name,
    const char* value,
    XAppCaptureMetadataPriority priority);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataAddInt32Event(
    const char* name,
    int32_t value,
    XAppCaptureMetadataPriority priority);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataAddDoubleEvent(
    const char* name,
    double value,
    XAppCaptureMetadataPriority priority);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStartStringState(
    const char* name,
    const char* value,
    XAppCaptureMetadataPriority priority);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStartInt32State(
    const char* name,
    int32_t value,
    XAppCaptureMetadataPriority priority);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStartDoubleState(
    const char* name,
    double value,
    XAppCaptureMetadataPriority priority);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStopState(
    const char* name);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStopAllStates(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataRemainingStorageBytesAvailable(
    uint64_t* value);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureRegisterMetadataPurged(
    XTaskQueueHandle queue,
    void* context,
    XAppCaptureMetadataPurgedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XAppCaptureUnRegisterMetadataPurged(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureTakeDiagnosticScreenshot(
    bool gameDvrShouldBeShutDown,
    XAppCaptureScreenshotFormatFlag screenshotFormat,
    const char* filenamePrefix,
    XAppCaptureDiagnosticScreenshotResult* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureRecordDiagnosticClip(
    int64_t startTime,
    uint32_t durationInMs,
    const char* filenamePrefix,
    XAppCaptureRecordClipResult* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureTakeScreenshot(
    XUserHandle requestingUser,
    XAppCaptureTakeScreenshotResult* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureOpenScreenshotStream(
    const char* localId,
    XAppCaptureScreenshotFormatFlag screenshotFormat,
    XAppCaptureScreenshotStreamHandle* handle,
    uint64_t* totalBytes);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureReadScreenshotStream(
    XAppCaptureScreenshotStreamHandle handle,
    uint64_t startPosition,
    uint32_t bytesToRead,
    uint8_t* buffer,
    uint32_t* bytesWritten);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureCloseScreenshotStream(
    XAppCaptureScreenshotStreamHandle handle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureEnableRecord(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAppCaptureDisableRecord(void);

// ---------------------------------------------------------------------------
// XgrGuid (forward declaration for XStore & QueryApiImpl)
// ---------------------------------------------------------------------------
struct XgrGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};
static_assert(sizeof(XgrGuid) == 16, "XgrGuid must be 16 bytes");

// ---------------------------------------------------------------------------
// XStore
// ---------------------------------------------------------------------------
typedef struct XStoreContext* XStoreContextHandle;
typedef struct XStoreLicense* XStoreLicenseHandle;
typedef struct XStoreProductQuery* XStoreProductQueryHandle;

constexpr size_t XSTORE_STORE_SKU_ID_SIZE = 18;
constexpr size_t XSTORE_IN_APP_OFFER_TOKEN_MAX_SIZE = 64;
constexpr size_t XSTORE_TRIAL_UNIQUE_ID_MAX_SIZE = 64;
constexpr size_t XSTORE_PRICE_MAX_SIZE = 16;
constexpr size_t XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH = 33;

enum XStoreProductKind : uint32_t {
    XStoreProductKind_None = 0x00,
    XStoreProductKind_Consumable = 0x01,
    XStoreProductKind_Durable = 0x02,
    XStoreProductKind_Game = 0x04,
    XStoreProductKind_Pass = 0x08,
    XStoreProductKind_UnmanagedConsumable = 0x10,
};

enum XStoreCanLicenseStatus : uint32_t {
    XStoreCanLicenseStatus_NotLicensableToUser = 0,
    XStoreCanLicenseStatus_Licensable = 1,
    XStoreCanLicenseStatus_LicenseActionNotApplicableToProduct = 2,
};

enum XStoreDurationUnit : uint32_t {
    XStoreDurationUnit_Minute = 0,
    XStoreDurationUnit_Hour = 1,
    XStoreDurationUnit_Day = 2,
    XStoreDurationUnit_Week = 3,
    XStoreDurationUnit_Month = 4,
    XStoreDurationUnit_Year = 5,
};

struct XStoreGameLicense {
    char skuStoreId[XSTORE_STORE_SKU_ID_SIZE];
    bool isActive;
    bool isTrialOwnedByThisUser;
    bool isDiscLicense;
    bool isTrial;
    uint32_t trialTimeRemainingInSeconds;
    char trialUniqueId[XSTORE_TRIAL_UNIQUE_ID_MAX_SIZE];
    int64_t expirationDate;
};

struct XStoreAddonLicense {
    char skuStoreId[XSTORE_STORE_SKU_ID_SIZE];
    char inAppOfferToken[XSTORE_IN_APP_OFFER_TOKEN_MAX_SIZE];
    bool isActive;
    int64_t expirationDate;
};

struct XStoreCanAcquireLicenseResult {
    char licensableSku[9];
    XStoreCanLicenseStatus status;
};

struct XStorePrice {
    float basePrice;
    float price;
    float recurrencePrice;
    const char* currencyCode;
    char formattedBasePrice[XSTORE_PRICE_MAX_SIZE];
    char formattedPrice[XSTORE_PRICE_MAX_SIZE];
    char formattedRecurrencePrice[XSTORE_PRICE_MAX_SIZE];
    bool isOnSale;
    int64_t saleEndDate;
};

struct XStoreImage {
    const char* uri;
    uint32_t height;
    uint32_t width;
    const char* caption;
    const char* imagePurposeTag;
};

struct XStoreVideo {
    const char* uri;
    uint32_t height;
    uint32_t width;
    const char* caption;
    const char* videoPurposeTag;
    XStoreImage previewImage;
};

struct XStoreAvailability {
    const char* availabilityId;
    XStorePrice price;
    int64_t endDate;
};

struct XStoreCollectionData {
    int64_t acquiredDate;
    int64_t startDate;
    int64_t endDate;
    bool isTrial;
    uint32_t trialTimeRemainingInSeconds;
    uint32_t quantity;
    const char* campaignId;
    const char* developerOfferId;
};

struct XStoreSubscriptionInfo {
    bool hasTrialPeriod;
    XStoreDurationUnit trialPeriodUnit;
    uint32_t trialPeriod;
    XStoreDurationUnit billingPeriodUnit;
    uint32_t billingPeriod;
};

struct XStoreSku {
    const char* skuId;
    const char* title;
    const char* description;
    const char* language;
    XStorePrice price;
    bool isTrial;
    bool isInUserCollection;
    XStoreCollectionData collectionData;
    bool isSubscription;
    XStoreSubscriptionInfo subscriptionInfo;
    uint32_t bundledSkusCount;
    const char** bundledSkus;
    uint32_t imagesCount;
    XStoreImage* images;
    uint32_t videosCount;
    XStoreVideo* videos;
    uint32_t availabilitiesCount;
    XStoreAvailability* availabilities;
};

struct XStoreProduct {
    const char* storeId;
    const char* title;
    const char* description;
    const char* language;
    const char* inAppOfferToken;
    char* linkUri;
    XStoreProductKind productKind;
    XStorePrice price;
    bool hasDigitalDownload;
    bool isInUserCollection;
    uint32_t keywordsCount;
    const char** keywords;
    uint32_t skusCount;
    XStoreSku* skus;
    uint32_t imagesCount;
    XStoreImage* images;
    uint32_t videosCount;
    XStoreVideo* videos;
};

struct XStoreConsumableResult {
    uint32_t quantity;
};

struct XStorePackageUpdate {
    char packageIdentifier[XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH];
    bool isMandatory;
};

typedef bool(OPENSCARLETT_GDK_CALL* XStoreProductQueryCallback)(
    const XStoreProduct* product,
    void* context);
typedef void(OPENSCARLETT_GDK_CALL* XStoreGameLicenseChangedCallback)(void* context);
typedef void(OPENSCARLETT_GDK_CALL* XStorePackageLicenseLostCallback)(void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreCreateContext(
    XUserHandle user,
    XStoreContextHandle* storeContextHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XStoreCloseContextHandle(
    XStoreContextHandle storeContextHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XStoreCloseLicenseHandle(
    XStoreLicenseHandle storeLicenseHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XStoreCloseProductsQueryHandle(
    XStoreProductQueryHandle productQueryHandle);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XStoreIsLicenseValid(
    XStoreLicenseHandle storeLicenseHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameLicenseAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameLicenseResult(
    XAsyncBlock* async,
    XStoreGameLicense* license);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreRegisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueHandle queue,
    void* context,
    XStoreGameLicenseChangedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XStoreUnregisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreRegisterPackageLicenseLost(
    XStoreLicenseHandle licenseHandle,
    XTaskQueueHandle queue,
    void* context,
    XStorePackageLicenseLostCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XStoreUnregisterPackageLicenseLost(
    XStoreLicenseHandle licenseHandle,
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAddOnLicensesAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAddOnLicensesResultCount(
    XAsyncBlock* async,
    uint32_t* count);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAddOnLicensesResult(
    XAsyncBlock* async,
    uint32_t count,
    XStoreAddonLicense* addOnLicenses);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForCurrentGameAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForCurrentGameResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForPackageAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    const char* packageIdentifier,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForPackageResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAssociatedProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    uint32_t maxItemsToRetrievePerPage,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAssociatedProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryEntitledProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    uint32_t maxItemsToRetrievePerPage,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryEntitledProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    const char** storeIds,
    size_t storeIdsCount,
    const char** actionFilters,
    size_t actionFiltersCount,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreEnumerateProductsQuery(
    XStoreProductQueryHandle productQueryHandle,
    void* context,
    XStoreProductQueryCallback callback);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XStoreProductsQueryHasMorePages(
    XStoreProductQueryHandle productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreProductsQueryNextPageAsync(
    XStoreProductQueryHandle productQueryHandle,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreProductsQueryNextPageResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForPackageAsync(
    XStoreContextHandle storeContextHandle,
    const char* packageIdentifier,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForPackageResult(
    XAsyncBlock* async,
    XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForStoreIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForStoreIdResult(
    XAsyncBlock* async,
    XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForPackageAsync(
    XStoreContextHandle storeContextHandle,
    const char* packageIdentifier,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForPackageResult(
    XAsyncBlock* async,
    XStoreLicenseHandle* storeLicenseHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForDurablesAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForDurablesResult(
    XAsyncBlock* async,
    XStoreLicenseHandle* storeLicenseHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryConsumableBalanceRemainingAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryConsumableBalanceRemainingResult(
    XAsyncBlock* async,
    XStoreConsumableResult* consumableResult);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreReportConsumableFulfillmentAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    uint32_t quantity,
    XgrGuid trackingId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreReportConsumableFulfillmentResult(
    XAsyncBlock* async,
    XStoreConsumableResult* consumableResult);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameAndDlcPackageUpdatesAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameAndDlcPackageUpdatesResultCount(
    XAsyncBlock* async,
    uint32_t* count);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameAndDlcPackageUpdatesResult(
    XAsyncBlock* async,
    uint32_t count,
    XStorePackageUpdate* packageUpdates);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadPackageUpdatesAsync(
    XStoreContextHandle storeContextHandle,
    const char** packageIdentifiers,
    size_t packageIdentifiersCount,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadPackageUpdatesResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackageUpdatesAsync(
    XStoreContextHandle storeContextHandle,
    const char** packageIdentifiers,
    size_t packageIdentifiersCount,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackageUpdatesResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackagesAsync(
    XStoreContextHandle storeContextHandle,
    const char** storeIds,
    size_t storeIdsCount,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackagesResultCount(
    XAsyncBlock* async,
    uint32_t* count);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackagesResult(
    XAsyncBlock* async,
    uint32_t count,
    char (*packageIdentifiers)[XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH]);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserCollectionsIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* serviceTicket,
    const char* publisherUserId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserCollectionsIdResultSize(
    XAsyncBlock* async,
    size_t* size);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserCollectionsIdResult(
    XAsyncBlock* async,
    size_t size,
    char* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserPurchaseIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* serviceTicket,
    const char* publisherUserId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserPurchaseIdResultSize(
    XAsyncBlock* async,
    size_t* size);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserPurchaseIdResult(
    XAsyncBlock* async,
    size_t size,
    char* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryLicenseTokenAsync(
    XStoreContextHandle storeContextHandle,
    const char** productIds,
    size_t productIdsCount,
    const char* customDeveloperString,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryLicenseTokenResultSize(
    XAsyncBlock* async,
    size_t* size);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryLicenseTokenResult(
    XAsyncBlock* async,
    size_t size,
    char* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreQueryPackageIdentifier(
    const char* storeId,
    size_t size,
    char* packageIdentifier);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XStoreIsAvailabilityPurchasable(
    const XStoreAvailability availability);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowAssociatedProductsUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    XStoreProductKind productKinds,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowAssociatedProductsUIResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowProductPageUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowProductPageUIResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowPurchaseUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    const char* name,
    const char* extendedJsonData,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowPurchaseUIResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowRateAndReviewUIAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowRateAndReviewUIResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowRedeemTokenUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* token,
    const char** allowedStoreIds,
    size_t allowedStoreIdsCount,
    bool disallowCsvRedemption,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XStoreShowRedeemTokenUIResult(
    XAsyncBlock* async);

// ---------------------------------------------------------------------------
// XGameUi
// ---------------------------------------------------------------------------
enum XGameUiNotificationPositionHint : uint32_t {
    XGameUiNotificationPositionHint_BottomCenter = 0,
    XGameUiNotificationPositionHint_BottomLeft = 1,
    XGameUiNotificationPositionHint_BottomRight = 2,
    XGameUiNotificationPositionHint_TopCenter = 3,
    XGameUiNotificationPositionHint_TopLeft = 4,
    XGameUiNotificationPositionHint_TopRight = 5,
};

enum XGameUiMessageDialogButton : uint32_t {
    XGameUiMessageDialogButton_First = 0,
    XGameUiMessageDialogButton_Second = 1,
    XGameUiMessageDialogButton_Third = 2,
};

enum XGameUiTextEntryInputScope : uint32_t {
    XGameUiTextEntryInputScope_Default = 0,
    XGameUiTextEntryInputScope_Url = 1,
    XGameUiTextEntryInputScope_EmailSmtpAddress = 5,
    XGameUiTextEntryInputScope_Number = 29,
    XGameUiTextEntryInputScope_Password = 31,
    XGameUiTextEntryInputScope_TelephoneNumber = 32,
    XGameUiTextEntryInputScope_Alphanumeric = 40,
    XGameUiTextEntryInputScope_Search = 50,
};

enum XGameUiWebAuthenticationOptions : uint32_t {
    XGameUiWebAuthenticationOptions_None = 0x00,
    XGameUiWebAuthenticationOptions_PreferFullscreen = 0x01,
};

struct XGameUiWebAuthenticationResultData {
    XGameResult responseStatus;
    size_t responseCompletionUriSize;
    const char* responseCompletionUri;
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiSetNotificationPositionHint(
    XGameUiNotificationPositionHint position);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowMessageDialogAsync(
    XAsyncBlock* async,
    const char* titleText,
    const char* contentText,
    const char* firstButtonText,
    const char* secondButtonText,
    const char* thirdButtonText,
    XGameUiMessageDialogButton defaultButton,
    XGameUiMessageDialogButton cancelButton);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowMessageDialogResult(
    XAsyncBlock* async,
    XGameUiMessageDialogButton* resultButton);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowErrorDialogAsync(
    XAsyncBlock* async,
    XGameResult errorCode,
    const char* context);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowErrorDialogResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowTextEntryAsync(
    XAsyncBlock* async,
    const char* titleText,
    const char* descriptionText,
    const char* defaultText,
    XGameUiTextEntryInputScope inputScope,
    uint32_t maxTextLength);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowTextEntryResultSize(
    XAsyncBlock* async,
    uint32_t* resultTextBufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowTextEntryResult(
    XAsyncBlock* async,
    uint32_t resultTextBufferSize,
    char* resultTextBuffer,
    uint32_t* resultTextBufferUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowAchievementsAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    uint32_t titleId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowAchievementsResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerPickerAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* promptText,
    uint32_t selectFromPlayersCount,
    const uint64_t* selectFromPlayers,
    uint32_t preSelectedPlayersCount,
    const uint64_t* preSelectedPlayers,
    uint32_t minSelectionCount,
    uint32_t maxSelectionCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerPickerResultCount(
    XAsyncBlock* async,
    uint32_t* resultPlayersCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerPickerResult(
    XAsyncBlock* async,
    uint32_t resultPlayersCount,
    uint64_t* resultPlayers,
    uint32_t* resultPlayersUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerProfileCardAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    uint64_t targetPlayer);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerProfileCardResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowSendGameInviteAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* sessionConfigurationId,
    const char* sessionTemplateName,
    const char* sessionId,
    const char* invitationText,
    const char* customActivationContext);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowSendGameInviteResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* requestUri,
    const char* completionUri);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationWithOptionsAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* requestUri,
    const char* completionUri,
    XGameUiWebAuthenticationOptions options);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XGameUiWebAuthenticationResultData** ptrToBuffer,
    size_t* bufferUsed);

// ---------------------------------------------------------------------------
// XNetworking
// ---------------------------------------------------------------------------
enum XNetworkingConnectivityLevelHint : uint32_t {
    XNetworkingConnectivityLevelHint_Unknown = 0,
    XNetworkingConnectivityLevelHint_None = 1,
    XNetworkingConnectivityLevelHint_LocalAccess = 2,
    XNetworkingConnectivityLevelHint_InternetAccess = 3,
    XNetworkingConnectivityLevelHint_ConstrainedInternetAccess = 4,
};

enum XNetworkingConnectivityCostHint : uint32_t {
    XNetworkingConnectivityCostHint_Unknown = 0,
    XNetworkingConnectivityCostHint_Unrestricted = 1,
    XNetworkingConnectivityCostHint_Fixed = 2,
    XNetworkingConnectivityCostHint_Variable = 3,
};

struct XNetworkingConnectivityHint {
    XNetworkingConnectivityLevelHint connectivityLevel;
    XNetworkingConnectivityCostHint connectivityCost;
    uint32_t ianaInterfaceType;
    bool networkInitialized;
    bool approachingDataLimit;
    bool overDataLimit;
    bool roaming;
};

struct XNetworkingThumbprint {
    uint32_t thumbprintType;
    size_t thumbprintBufferByteCount;
    uint8_t* thumbprintBuffer;
};

struct XNetworkingSecurityInformation {
    uint32_t enabledHttpSecurityProtocolFlags;
    size_t thumbprintCount;
    XNetworkingThumbprint* thumbprints;
};

typedef void(OPENSCARLETT_GDK_CALL* XNetworkingConnectivityHintChangedCallback)(
    void* context,
    const XNetworkingConnectivityHint* connectivityHint);
typedef void(OPENSCARLETT_GDK_CALL* XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback)(
    void* context,
    uint16_t preferredLocalUdpMultiplayerPort);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingGetConnectivityHint(
    XNetworkingConnectivityHint* connectivityHint);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryConnectivityHint(
    XNetworkingConnectivityHint* connectivityHint);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryPreferredLocalUdpMultiplayerPort(
    uint16_t* preferredLocalUdpMultiplayerPort);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(
    XAsyncBlock* async,
    uint16_t* preferredLocalUdpMultiplayerPort);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingRegisterConnectivityHintChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingConnectivityHintChangedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XNetworkingUnregisterConnectivityHintChanged(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueRegistrationToken token,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlAsync(
    const char* url,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlAsyncResultSize(
    XAsyncBlock* async,
    size_t* securityInformationBufferByteCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlAsyncResult(
    XAsyncBlock* async,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** pointerToSecurityInformation);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlUtf16Async(
    const wchar_t* url,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(
    XAsyncBlock* async,
    size_t* securityInformationBufferByteCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(
    XAsyncBlock* async,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** pointerToSecurityInformation);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XNetworkingVerifyServerCertificate(
    void* requestHandle,
    const XNetworkingSecurityInformation* securityInformation);

// ---------------------------------------------------------------------------
// Core Runtime Extensions (PLS, XThread, XError, XGameEvent, XGameInvite, XLaunch, XAccessibility)
// ---------------------------------------------------------------------------
enum XErrorOptions : uint32_t {
    XErrorOptions_None = 0x00,
    XErrorOptions_OutputDebugStringOnError = 0x01,
    XErrorOptions_DebugBreakOnError = 0x02,
    XErrorOptions_FailFastOnError = 0x04,
};

typedef bool(OPENSCARLETT_GDK_CALL* XErrorCallback)(
    XGameResult hr,
    const char* msg,
    void* context);

struct XGameEventDescriptor {
    uint16_t id;
    uint8_t version;
    uint8_t channel;
    uint8_t level;
    uint8_t opcode;
    uint16_t task;
    uint64_t keyword;
};

struct XGameEventDataDescriptor {
    uint64_t ptr;
    uint32_t size;
    uint32_t reserved;
};

struct XPersistentLocalStorageSpaceInfo {
    uint64_t availableFreeBytes;
    uint64_t totalFreeBytes;
    uint64_t usedBytes;
    uint64_t totalBytes;
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStorageGetPathSize(
    size_t* pathSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStorageGetPath(
    size_t pathSize,
    char* path,
    size_t* pathUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStorageGetSpaceInfo(
    XPersistentLocalStorageSpaceInfo* spaceInfo);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStoragePromptUserForSpaceAsync(
    uint64_t requestedBytes,
    XAsyncBlock* asyncBlock);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStoragePromptUserForSpaceResult(
    XAsyncBlock* asyncBlock);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XThreadIsTimeSensitive(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XThreadSetTimeSensitive(
    bool isTimeSensitiveThread);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XThreadAssertNotTimeSensitive(void);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XThreadVerifyNotTimeSensitive(void);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XErrorReport(
    XGameResult hr,
    const char* msg);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XErrorSetCallback(
    XErrorCallback callback,
    void* context);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XErrorSetOptions(
    XErrorOptions optionsDebuggerPresent,
    XErrorOptions optionsDebuggerNotPresent);
OPENSCARLETT_GDK_API uint32_t OPENSCARLETT_GDK_CALL XGameEventWrite(
    uint64_t regHandle,
    const XGameEventDescriptor* eventDescriptor,
    uint32_t userDataCount,
    XGameEventDataDescriptor* userData);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XLaunchNewGame(
    const char* exePath,
    const char* args,
    XUserHandle defaultUser);

// ---------------------------------------------------------------------------
// GDK Static Import Runtime Dispatch (InitializeApiImpl / QueryApiImpl)
// ---------------------------------------------------------------------------
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
constexpr XgrGuid XGR_API_GUID_GAME_UI = {
    0x4F534752u, 0x0009u, 0x4000u, {0x80, 0x09, 0x00, 0x58, 0x55, 0x49, 0x00, 0x09}};
constexpr XgrGuid XGR_API_GUID_GAME_INVITE = {
    0x4F534752u, 0x000Au, 0x4000u, {0x80, 0x0A, 0x00, 0x58, 0x49, 0x4E, 0x00, 0x0A}};
constexpr XgrGuid XGR_API_GUID_ACCESSIBILITY = {
    0x4F534752u, 0x000Bu, 0x4000u, {0x80, 0x0B, 0x00, 0x58, 0x41, 0x43, 0x00, 0x0B}};
constexpr XgrGuid XGR_API_GUID_SPEECH_SYNTHESIZER = {
    0x4F534752u, 0x000Cu, 0x4000u, {0x80, 0x0C, 0x00, 0x58, 0x53, 0x50, 0x00, 0x0C}};
constexpr XgrGuid XGR_API_GUID_STORE = {
    0x4F534752u, 0x000Du, 0x4000u, {0x80, 0x0D, 0x00, 0x58, 0x53, 0x54, 0x00, 0x0D}};
constexpr XgrGuid XGR_API_GUID_NETWORKING = {
    0x4F534752u, 0x000Eu, 0x4000u, {0x80, 0x0E, 0x00, 0x58, 0x4E, 0x57, 0x00, 0x0E}};
constexpr XgrGuid XGR_API_GUID_CORE_RUNTIME = {
    0x4F534752u, 0x000Fu, 0x4000u, {0x80, 0x0F, 0x00, 0x58, 0x43, 0x52, 0x00, 0x0F}};
constexpr XgrGuid XGR_API_GUID_GAME_STREAMING = {
    0x4F534752u, 0x0010u, 0x4000u, {0x80, 0x10, 0x00, 0x58, 0x47, 0x54, 0x00, 0x10}};

typedef XGameResult(OPENSCARLETT_GDK_CALL* XgrQueryInterfaceFn)(
    const void* self,
    const XgrGuid* riid,
    void** ppvObject);
typedef uint32_t(OPENSCARLETT_GDK_CALL* XgrAddRefFn)(const void* self);
typedef uint32_t(OPENSCARLETT_GDK_CALL* XgrReleaseFn)(const void* self);

struct IXgrTaskQueueApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueCreate)(
        XTaskQueueDispatchMode workDispatchMode,
        XTaskQueueDispatchMode completionDispatchMode,
        XTaskQueueHandle* queue);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueDuplicateHandle)(
        XTaskQueueHandle queueHandle,
        XTaskQueueHandle* duplicatedHandle);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueCloseHandle)(XTaskQueueHandle queue);
    bool(OPENSCARLETT_GDK_CALL* XTaskQueueGetCurrentProcessTaskQueue)(XTaskQueueHandle* queue);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueSetCurrentProcessTaskQueue)(XTaskQueueHandle queue);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueSubmitCallback)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        void* callbackContext,
        XTaskQueueCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueSubmitDelayedCallback)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        uint32_t delayMs,
        void* callbackContext,
        XTaskQueueCallback callback);
    bool(OPENSCARLETT_GDK_CALL* XTaskQueueDispatch)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        uint32_t timeoutInMs);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueTerminate)(
        XTaskQueueHandle queue,
        bool wait,
        void* callbackContext,
        XTaskQueueTerminatedCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueRegisterMonitor)(
        XTaskQueueHandle queue,
        void* callbackContext,
        XTaskQueueMonitorCallback callback,
        XTaskQueueRegistrationToken* token);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueUnregisterMonitor)(
        XTaskQueueHandle queue,
        XTaskQueueRegistrationToken token);
    // Extended TaskQueue entries
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueGetPort)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        XTaskQueuePortHandle* portHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueCreateComposite)(
        XTaskQueuePortHandle workPort,
        XTaskQueuePortHandle completionPort,
        XTaskQueueHandle* queue);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueRegisterWaiter)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        void* waitHandle,
        void* callbackContext,
        XTaskQueueCallback callback,
        XTaskQueueRegistrationToken* token);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueUnregisterWaiter)(
        XTaskQueueHandle queue,
        XTaskQueueRegistrationToken token);
};

struct IXgrAsyncApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncBegin)(
        XAsyncBlock* asyncBlock,
        void* context,
        const void* identity,
        const char* identityName,
        XAsyncProvider provider);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncSchedule)(
        XAsyncBlock* asyncBlock,
        uint32_t delayInMs);
    void(OPENSCARLETT_GDK_CALL* XAsyncComplete)(
        XAsyncBlock* asyncBlock,
        XGameResult result,
        size_t requiredBufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncGetStatus)(
        XAsyncBlock* asyncBlock,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncGetResultSize)(
        XAsyncBlock* asyncBlock,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncGetResult)(
        XAsyncBlock* asyncBlock,
        const void* identity,
        size_t bufferSize,
        void* buffer,
        size_t* bufferUsed);
    void(OPENSCARLETT_GDK_CALL* XAsyncCancel)(XAsyncBlock* asyncBlock);
    // Extended Async entry
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncRun)(
        XAsyncBlock* asyncBlock,
        XAsyncWork work);
};

struct IXgrUserApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XUserAddAsync)(
        XUserAddOptions options,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserAddResult)(
        XAsyncBlock* async,
        XUserHandle* newUser);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserDuplicateHandle)(
        XUserHandle handle,
        XUserHandle* duplicatedHandle);
    void(OPENSCARLETT_GDK_CALL* XUserCloseHandle)(XUserHandle user);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetId)(XUserHandle user, uint64_t* userId);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetLocalId)(
        XUserHandle user,
        XUserLocalId* userLocalId);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetGamertag)(
        XUserHandle user,
        XUserGamertagComponent gamertagComponent,
        size_t gamertagSize,
        char* gamertag,
        size_t* gamertagUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetState)(XUserHandle user, XUserState* state);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetAgeGroup)(
        XUserHandle user,
        XUserAgeGroup* ageGroup);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserCheckPrivilege)(
        XUserHandle user,
        XUserPrivilegeOptions options,
        XUserPrivilege privilege,
        bool* hasPrivilege,
        XUserPrivilegeDenyReason* reason);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetMaxUsers)(uint32_t* maxUsers);
    int32_t(OPENSCARLETT_GDK_CALL* XUserCompare)(XUserHandle user1, XUserHandle user2);
    // Extended XUser entries (full XGamingRuntimeThunks parity)
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetGamerPictureAsync)(
        XUserHandle user,
        XUserGamerPictureSize pictureSize,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetGamerPictureResultSize)(
        XAsyncBlock* async,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetGamerPictureResult)(
        XAsyncBlock* async,
        size_t bufferSize,
        uint8_t* buffer,
        size_t* bufferUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetIsGuest)(XUserHandle user, bool* isGuest);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserFindUserById)(uint64_t userId, XUserHandle* handle);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserFindUserByLocalId)(
        XUserLocalId userLocalId,
        XUserHandle* handle);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserFindForDevice)(
        const XUserAppLocalDeviceId* deviceId,
        XUserHandle* handle);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetTokenAndSignatureAsync)(
        XUserHandle user,
        XUserGetTokenAndSignatureOptions options,
        const char* method,
        const char* url,
        size_t headerCount,
        const XUserGetTokenAndSignatureHttpHeader* headers,
        size_t bodySize,
        const void* bodyBuffer,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetTokenAndSignatureResultSize)(
        XAsyncBlock* async,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetTokenAndSignatureResult)(
        XAsyncBlock* async,
        size_t bufferSize,
        void* buffer,
        XUserGetTokenAndSignatureData** ptrToBuffer,
        size_t* bufferUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetTokenAndSignatureUtf16Async)(
        XUserHandle user,
        XUserGetTokenAndSignatureOptions options,
        const wchar_t* method,
        const wchar_t* url,
        size_t headerCount,
        const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
        size_t bodySize,
        const void* bodyBuffer,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetTokenAndSignatureUtf16ResultSize)(
        XAsyncBlock* async,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetTokenAndSignatureUtf16Result)(
        XAsyncBlock* async,
        size_t bufferSize,
        void* buffer,
        XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
        size_t* bufferUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetDefaultAudioEndpointUtf16)(
        XUserLocalId user,
        XUserDefaultAudioEndpointKind defaultAudioEndpointKind,
        size_t endpointIdUtf16Count,
        wchar_t* endpointIdUtf16,
        size_t* endpointIdUtf16Used);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetSignOutDeferral)(
        XUserSignOutDeferralHandle* deferral);
    void(OPENSCARLETT_GDK_CALL* XUserCloseSignOutDeferralHandle)(
        XUserSignOutDeferralHandle deferral);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserRegisterForChangeEvent)(
        XTaskQueueHandle queue,
        void* context,
        XUserChangeEventCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XUserUnregisterForChangeEvent)(
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserRegisterForDefaultAudioEndpointUtf16Changed)(
        XTaskQueueHandle queue,
        void* context,
        XUserDefaultAudioEndpointUtf16ChangedCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XUserUnregisterForDefaultAudioEndpointUtf16Changed)(
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserRegisterForDeviceAssociationChanged)(
        XTaskQueueHandle queue,
        void* context,
        XUserDeviceAssociationChangedCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XUserUnregisterForDeviceAssociationChanged)(
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserResolveIssueWithUiAsync)(
        XUserHandle user,
        const char* url,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserResolveIssueWithUiResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserResolveIssueWithUiUtf16Async)(
        XUserHandle user,
        const wchar_t* url,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserResolveIssueWithUiUtf16Result)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserResolvePrivilegeWithUiAsync)(
        XUserHandle user,
        XUserPrivilegeOptions options,
        XUserPrivilege privilege,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserResolvePrivilegeWithUiResult)(XAsyncBlock* async);
};

struct IXgrGameSaveApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveInitializeProvider)(
        XUserHandle requestingUser,
        const char* configurationId,
        bool syncOnDemand,
        XGameSaveProviderHandle* provider);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveInitializeProviderAsync)(
        XUserHandle requestingUser,
        const char* configurationId,
        bool syncOnDemand,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveInitializeProviderResult)(
        XAsyncBlock* async,
        XGameSaveProviderHandle* provider);
    void(OPENSCARLETT_GDK_CALL* XGameSaveCloseProvider)(XGameSaveProviderHandle provider);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveCreateContainer)(
        XGameSaveProviderHandle provider,
        const char* name,
        XGameSaveContainerHandle* result);
    void(OPENSCARLETT_GDK_CALL* XGameSaveCloseContainer)(XGameSaveContainerHandle context);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveCreateUpdate)(
        XGameSaveContainerHandle container,
        const char* containerDisplayName,
        XGameSaveUpdateHandle* result);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitBlobWrite)(
        XGameSaveUpdateHandle updateContext,
        const char* blobName,
        const uint8_t* data,
        size_t byteCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitBlobDelete)(
        XGameSaveUpdateHandle updateContext,
        const char* blobName);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitUpdate)(XGameSaveUpdateHandle updateContext);
    void(OPENSCARLETT_GDK_CALL* XGameSaveCloseUpdate)(XGameSaveUpdateHandle updateContext);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveReadBlobData)(
        XGameSaveContainerHandle container,
        const char** blobNames,
        uint32_t* countOfBlobs,
        size_t blobsSize,
        XGameSaveBlob* blobData);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveEnumerateContainerInfo)(
        XGameSaveProviderHandle provider,
        void* context,
        XGameSaveContainerInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveEnumerateBlobInfo)(
        XGameSaveContainerHandle container,
        void* context,
        XGameSaveBlobInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveDeleteContainer)(
        XGameSaveProviderHandle provider,
        const char* name);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveGetRemainingQuota)(
        XGameSaveProviderHandle provider,
        int64_t* remainingQuota);
    // Extended XGameSave entries
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveEnumerateContainerInfoByName)(
        XGameSaveProviderHandle provider,
        const char* containerNamePrefix,
        void* context,
        XGameSaveContainerInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveEnumerateBlobInfoByName)(
        XGameSaveContainerHandle container,
        const char* blobNamePrefix,
        void* context,
        XGameSaveBlobInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveGetContainerInfo)(
        XGameSaveProviderHandle provider,
        const char* containerName,
        void* context,
        XGameSaveContainerInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveReadBlobDataAsync)(
        XGameSaveContainerHandle container,
        const char** blobNames,
        uint32_t countOfBlobs,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveReadBlobDataResult)(
        XAsyncBlock* async,
        size_t blobsSize,
        XGameSaveBlob* blobData,
        uint32_t* countOfBlobs);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitUpdateAsync)(
        XGameSaveUpdateHandle updateContext,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitUpdateResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveDeleteContainerAsync)(
        XGameSaveProviderHandle provider,
        const char* name,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveDeleteContainerResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveGetRemainingQuotaAsync)(
        XGameSaveProviderHandle provider,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveGetRemainingQuotaResult)(
        XAsyncBlock* async,
        int64_t* remainingQuota);
};

struct IXgrSystemApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XGameRuntimeInitialize)(void);
    void(OPENSCARLETT_GDK_CALL* XGameRuntimeUninitialize)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameGetXboxTitleId)(uint32_t* titleId);
    XSystemDeviceType(OPENSCARLETT_GDK_CALL* XSystemGetDeviceType)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XSystemGetConsoleId)(
        size_t consoleIdSize,
        char* consoleId,
        size_t* consoleIdUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XSystemGetXboxLiveSandboxId)(
        size_t sandboxIdSize,
        char* sandboxId,
        size_t* sandboxIdUsed);
    XSystemAnalyticsInfo(OPENSCARLETT_GDK_CALL* XSystemGetAnalyticsInfo)(void);
    XSystemRuntimeInfo(OPENSCARLETT_GDK_CALL* XSystemGetRuntimeInfo)(void);
    bool(OPENSCARLETT_GDK_CALL* XSystemIsHandleValid)(XSystemHandle handle);
    // Extended System entries
    bool(OPENSCARLETT_GDK_CALL* XGameRuntimeIsFeatureAvailable)(XGameRuntimeFeature feature);
    XGameResult(OPENSCARLETT_GDK_CALL* XSystemGetAppSpecificDeviceId)(
        size_t appSpecificDeviceIdSize,
        char* appSpecificDeviceId,
        size_t* appSpecificDeviceIdUsed);
    // Appended last so existing slot indices stay ABI-stable.
    XGameResult(OPENSCARLETT_GDK_CALL* XSystemHandleTrack)(
        XSystemHandleCallback callback,
        void* context);
};

struct IXgrMemApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    void*(OPENSCARLETT_GDK_CALL* XMemAllocDefault)(size_t size, uint64_t attributes);
    void(OPENSCARLETT_GDK_CALL* XMemFreeDefault)(void* pointer, uint64_t attributes);
    void*(OPENSCARLETT_GDK_CALL* XMemAlloc)(size_t size, uint64_t attributes);
    void(OPENSCARLETT_GDK_CALL* XMemFree)(void* pointer, uint64_t attributes);
    XGameResult(OPENSCARLETT_GDK_CALL* XMemSetAllocationHooks)(
        XMemAllocFunction allocHook,
        XMemFreeFunction freeHook);
};

struct IXgrPackageApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XPackageGetCurrentProcessPackageIdentifier)(
        size_t bufferSize,
        char* buffer);
    bool(OPENSCARLETT_GDK_CALL* XPackageIsPackagedProcess)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageEnumerateChunkAvailability)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        void* context,
        XPackageChunkAvailabilityCallback callback);
    // Extended XPackage entries (full XGamingRuntimeThunks parity)
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageMount)(
        const char* packageIdentifier,
        XPackageMountHandle* mount);
    void(OPENSCARLETT_GDK_CALL* XPackageCloseMountHandle)(XPackageMountHandle mount);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageGetMountPathSize)(
        XPackageMountHandle mount,
        size_t* pathSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageGetMountPath)(
        XPackageMountHandle mount,
        size_t pathSize,
        char* path);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageGetUserLocale)(size_t localeSize, char* locale);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageGetWriteStats)(XPackageWriteStats* writeStats);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageCreateInstallationMonitor)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        uint32_t minimumUpdateIntervalMs,
        XTaskQueueHandle queue,
        XPackageInstallationMonitorHandle* installationMonitor);
    void(OPENSCARLETT_GDK_CALL* XPackageCloseInstallationMonitorHandle)(
        XPackageInstallationMonitorHandle installationMonitor);
    void(OPENSCARLETT_GDK_CALL* XPackageGetInstallationProgress)(
        XPackageInstallationMonitorHandle installationMonitor,
        XPackageInstallationProgress* progress);
    bool(OPENSCARLETT_GDK_CALL* XPackageUpdateInstallationMonitor)(
        XPackageInstallationMonitorHandle installationMonitor);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageRegisterInstallationProgressChanged)(
        XPackageInstallationMonitorHandle installationMonitor,
        void* context,
        XPackageInstallationProgressCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XPackageUnregisterInstallationProgressChanged)(
        XPackageInstallationMonitorHandle installationMonitor,
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageRegisterPackageInstalled)(
        XTaskQueueHandle queue,
        void* context,
        XPackageInstalledCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XPackageUnregisterPackageInstalled)(
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageEnumeratePackages)(
        XPackageKind kind,
        XPackageEnumerationScope scope,
        void* context,
        XPackageEnumerationCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageEnumerateFeatures)(
        const char* packageIdentifier,
        void* context,
        XPackageFeatureEnumerationCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageFindChunkAvailability)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        XPackageChunkAvailability* availability);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageEstimateDownloadSize)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        uint64_t* downloadSize,
        bool* shouldPresentUserConfirmation);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageChangeChunkInstallOrder)(
        const char* packageIdentifier,
        uint32_t chunkCount,
        const uint32_t* chunkIds);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageInstallChunks)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        uint32_t minimumUpdateIntervalMs,
        bool suppressUserConfirmation,
        XTaskQueueHandle queue,
        XPackageInstallationMonitorHandle* installationMonitor);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageInstallChunksAsync)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        uint32_t minimumUpdateIntervalMs,
        bool suppressUserConfirmation,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageInstallChunksResult)(
        XAsyncBlock* async,
        XPackageInstallationMonitorHandle* installationMonitor);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageUninstallChunks)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageUninstallUWPInstance)(const char* packageName);
};

struct IXgrDisplayApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XDisplayHdrModeResult(OPENSCARLETT_GDK_CALL* XDisplayTryEnableHdrMode)(
        XDisplayHdrModePreference preferredMode,
        XDisplayHdrModeInfo* displayHdrModeInfo);
    XGameResult(OPENSCARLETT_GDK_CALL* XDisplayTryAcquireTimeoutDeferral)(
        XDisplayTimeoutDeferralHandle* handle);
    void(OPENSCARLETT_GDK_CALL* XDisplayCloseTimeoutDeferralHandle)(
        XDisplayTimeoutDeferralHandle handle);
    // Extended Display entry
    XGameResult(OPENSCARLETT_GDK_CALL* XDisplayAcquireTimeoutDeferral)(
        XDisplayTimeoutDeferralHandle* handle);
};

struct IXgrStoreApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XStoreCreateContext)(
        XUserHandle user,
        XStoreContextHandle* storeContextHandle);
    void(OPENSCARLETT_GDK_CALL* XStoreCloseContextHandle)(XStoreContextHandle storeContextHandle);
    void(OPENSCARLETT_GDK_CALL* XStoreCloseLicenseHandle)(XStoreLicenseHandle storeLicenseHandle);
    void(OPENSCARLETT_GDK_CALL* XStoreCloseProductsQueryHandle)(
        XStoreProductQueryHandle productQueryHandle);
    bool(OPENSCARLETT_GDK_CALL* XStoreIsLicenseValid)(XStoreLicenseHandle storeLicenseHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryGameLicenseAsync)(
        XStoreContextHandle storeContextHandle,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryGameLicenseResult)(
        XAsyncBlock* async,
        XStoreGameLicense* license);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreRegisterGameLicenseChanged)(
        XStoreContextHandle storeContextHandle,
        XTaskQueueHandle queue,
        void* context,
        XStoreGameLicenseChangedCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XStoreUnregisterGameLicenseChanged)(
        XStoreContextHandle storeContextHandle,
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreRegisterPackageLicenseLost)(
        XStoreLicenseHandle licenseHandle,
        XTaskQueueHandle queue,
        void* context,
        XStorePackageLicenseLostCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XStoreUnregisterPackageLicenseLost)(
        XStoreLicenseHandle licenseHandle,
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryAddOnLicensesAsync)(
        XStoreContextHandle storeContextHandle,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryAddOnLicensesResultCount)(
        XAsyncBlock* async,
        uint32_t* count);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryAddOnLicensesResult)(
        XAsyncBlock* async,
        uint32_t count,
        XStoreAddonLicense* addOnLicenses);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryProductForCurrentGameAsync)(
        XStoreContextHandle storeContextHandle,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryProductForCurrentGameResult)(
        XAsyncBlock* async,
        XStoreProductQueryHandle* productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryProductForPackageAsync)(
        XStoreContextHandle storeContextHandle,
        XStoreProductKind productKinds,
        const char* packageIdentifier,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryProductForPackageResult)(
        XAsyncBlock* async,
        XStoreProductQueryHandle* productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryAssociatedProductsAsync)(
        XStoreContextHandle storeContextHandle,
        XStoreProductKind productKinds,
        uint32_t maxItemsToRetrievePerPage,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryAssociatedProductsResult)(
        XAsyncBlock* async,
        XStoreProductQueryHandle* productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryEntitledProductsAsync)(
        XStoreContextHandle storeContextHandle,
        XStoreProductKind productKinds,
        uint32_t maxItemsToRetrievePerPage,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryEntitledProductsResult)(
        XAsyncBlock* async,
        XStoreProductQueryHandle* productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryProductsAsync)(
        XStoreContextHandle storeContextHandle,
        XStoreProductKind productKinds,
        const char** storeIds,
        size_t storeIdsCount,
        const char** actionFilters,
        size_t actionFiltersCount,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryProductsResult)(
        XAsyncBlock* async,
        XStoreProductQueryHandle* productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreEnumerateProductsQuery)(
        XStoreProductQueryHandle productQueryHandle,
        void* context,
        XStoreProductQueryCallback callback);
    bool(OPENSCARLETT_GDK_CALL* XStoreProductsQueryHasMorePages)(
        XStoreProductQueryHandle productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreProductsQueryNextPageAsync)(
        XStoreProductQueryHandle productQueryHandle,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreProductsQueryNextPageResult)(
        XAsyncBlock* async,
        XStoreProductQueryHandle* productQueryHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreCanAcquireLicenseForPackageAsync)(
        XStoreContextHandle storeContextHandle,
        const char* packageIdentifier,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreCanAcquireLicenseForPackageResult)(
        XAsyncBlock* async,
        XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreCanAcquireLicenseForStoreIdAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeProductId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreCanAcquireLicenseForStoreIdResult)(
        XAsyncBlock* async,
        XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreAcquireLicenseForPackageAsync)(
        XStoreContextHandle storeContextHandle,
        const char* packageIdentifier,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreAcquireLicenseForPackageResult)(
        XAsyncBlock* async,
        XStoreLicenseHandle* storeLicenseHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreAcquireLicenseForDurablesAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreAcquireLicenseForDurablesResult)(
        XAsyncBlock* async,
        XStoreLicenseHandle* storeLicenseHandle);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryConsumableBalanceRemainingAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeProductId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryConsumableBalanceRemainingResult)(
        XAsyncBlock* async,
        XStoreConsumableResult* consumableResult);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreReportConsumableFulfillmentAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeProductId,
        uint32_t quantity,
        XgrGuid trackingId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreReportConsumableFulfillmentResult)(
        XAsyncBlock* async,
        XStoreConsumableResult* consumableResult);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryGameAndDlcPackageUpdatesAsync)(
        XStoreContextHandle storeContextHandle,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryGameAndDlcPackageUpdatesResultCount)(
        XAsyncBlock* async,
        uint32_t* count);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryGameAndDlcPackageUpdatesResult)(
        XAsyncBlock* async,
        uint32_t count,
        XStorePackageUpdate* packageUpdates);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadPackageUpdatesAsync)(
        XStoreContextHandle storeContextHandle,
        const char** packageIdentifiers,
        size_t packageIdentifiersCount,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadPackageUpdatesResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadAndInstallPackageUpdatesAsync)(
        XStoreContextHandle storeContextHandle,
        const char** packageIdentifiers,
        size_t packageIdentifiersCount,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadAndInstallPackageUpdatesResult)(
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadAndInstallPackagesAsync)(
        XStoreContextHandle storeContextHandle,
        const char** storeIds,
        size_t storeIdsCount,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadAndInstallPackagesResultCount)(
        XAsyncBlock* async,
        uint32_t* count);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreDownloadAndInstallPackagesResult)(
        XAsyncBlock* async,
        uint32_t count,
        char (*packageIdentifiers)[XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH]);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreGetUserCollectionsIdAsync)(
        XStoreContextHandle storeContextHandle,
        const char* serviceTicket,
        const char* publisherUserId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreGetUserCollectionsIdResultSize)(
        XAsyncBlock* async,
        size_t* size);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreGetUserCollectionsIdResult)(
        XAsyncBlock* async,
        size_t size,
        char* result);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreGetUserPurchaseIdAsync)(
        XStoreContextHandle storeContextHandle,
        const char* serviceTicket,
        const char* publisherUserId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreGetUserPurchaseIdResultSize)(
        XAsyncBlock* async,
        size_t* size);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreGetUserPurchaseIdResult)(
        XAsyncBlock* async,
        size_t size,
        char* result);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryLicenseTokenAsync)(
        XStoreContextHandle storeContextHandle,
        const char** productIds,
        size_t productIdsCount,
        const char* customDeveloperString,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryLicenseTokenResultSize)(
        XAsyncBlock* async,
        size_t* size);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryLicenseTokenResult)(
        XAsyncBlock* async,
        size_t size,
        char* result);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreQueryPackageIdentifier)(
        const char* storeId,
        size_t size,
        char* packageIdentifier);
    bool(OPENSCARLETT_GDK_CALL* XStoreIsAvailabilityPurchasable)(
        const XStoreAvailability availability);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowAssociatedProductsUIAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeId,
        XStoreProductKind productKinds,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowAssociatedProductsUIResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowProductPageUIAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeId,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowProductPageUIResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowPurchaseUIAsync)(
        XStoreContextHandle storeContextHandle,
        const char* storeId,
        const char* name,
        const char* extendedJsonData,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowPurchaseUIResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowRateAndReviewUIAsync)(
        XStoreContextHandle storeContextHandle,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowRateAndReviewUIResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowRedeemTokenUIAsync)(
        XStoreContextHandle storeContextHandle,
        const char* token,
        const char** allowedStoreIds,
        size_t allowedStoreIdsCount,
        bool disallowCsvRedemption,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XStoreShowRedeemTokenUIResult)(XAsyncBlock* async);
};

struct IXgrGameUiApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    // Front-of-vtable Wave 1 capital-UI entries (preserved at exact offsets)
    XGameResult(OPENSCARLETT_GDK_CALL* XLaunchUri)(
        XUserHandle requestingUser,
        const char* uri);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowMessageDialogAsync)(
        XAsyncBlock* async,
        const char* titleText,
        const char* contentText,
        const char* firstButtonText,
        const char* secondButtonText,
        const char* thirdButtonText,
        XGameUIMessageDialogButton defaultButton,
        XGameUIMessageDialogButton cancelButton);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowMessageDialogResult)(
        XAsyncBlock* async,
        XGameUIMessageDialogButton* resultButton);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowTextEntryAsync)(
        XAsyncBlock* async,
        const char* titleText,
        const char* descriptionText,
        const char* defaultText,
        XGameUITextEntryInputScope inputScope,
        uint32_t maxTextLength);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowTextEntryResultSize)(
        XAsyncBlock* async,
        uint32_t* resultTextBufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowTextEntryResult)(
        XAsyncBlock* async,
        uint32_t resultTextBufferSize,
        char* resultTextBuffer,
        uint32_t* resultTextBufferUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowPlayerPickerAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        const char* promptText,
        uint32_t selectFromPlayersCount,
        const uint64_t* selectFromPlayers,
        uint32_t preSelectedPlayersCount,
        const uint64_t* preSelectedPlayers,
        uint32_t minSelectionCount,
        uint32_t maxSelectionCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowPlayerPickerResultCount)(
        XAsyncBlock* async,
        uint32_t* resultPlayersCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowPlayerPickerResult)(
        XAsyncBlock* async,
        uint32_t resultPlayersCount,
        uint64_t* resultPlayers,
        uint32_t* resultPlayersUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowErrorDialogAsync)(
        XAsyncBlock* async,
        XGameResult errorCode,
        const char* context);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUIShowErrorDialogResult)(
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUISetNotificationPositionHint)(
        XGameUINotificationPositionHint position);

    // Extended GDK Thunks lowercase-Ui parity entries
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiSetNotificationPositionHint)(
        XGameUiNotificationPositionHint position);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowMessageDialogAsync)(
        XAsyncBlock* async,
        const char* titleText,
        const char* contentText,
        const char* firstButtonText,
        const char* secondButtonText,
        const char* thirdButtonText,
        XGameUiMessageDialogButton defaultButton,
        XGameUiMessageDialogButton cancelButton);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowMessageDialogResult)(
        XAsyncBlock* async,
        XGameUiMessageDialogButton* resultButton);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowErrorDialogAsync)(
        XAsyncBlock* async,
        XGameResult errorCode,
        const char* context);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowErrorDialogResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowTextEntryAsync)(
        XAsyncBlock* async,
        const char* titleText,
        const char* descriptionText,
        const char* defaultText,
        XGameUiTextEntryInputScope inputScope,
        uint32_t maxTextLength);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowTextEntryResultSize)(
        XAsyncBlock* async,
        uint32_t* resultTextBufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowTextEntryResult)(
        XAsyncBlock* async,
        uint32_t resultTextBufferSize,
        char* resultTextBuffer,
        uint32_t* resultTextBufferUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowAchievementsAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        uint32_t titleId);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowAchievementsResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowPlayerPickerAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        const char* promptText,
        uint32_t selectFromPlayersCount,
        const uint64_t* selectFromPlayers,
        uint32_t preSelectedPlayersCount,
        const uint64_t* preSelectedPlayers,
        uint32_t minSelectionCount,
        uint32_t maxSelectionCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowPlayerPickerResultCount)(
        XAsyncBlock* async,
        uint32_t* resultPlayersCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowPlayerPickerResult)(
        XAsyncBlock* async,
        uint32_t resultPlayersCount,
        uint64_t* resultPlayers,
        uint32_t* resultPlayersUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowPlayerProfileCardAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        uint64_t targetPlayer);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowPlayerProfileCardResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowSendGameInviteAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        const char* sessionConfigurationId,
        const char* sessionTemplateName,
        const char* sessionId,
        const char* invitationText,
        const char* customActivationContext);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowSendGameInviteResult)(XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowWebAuthenticationAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        const char* requestUri,
        const char* completionUri);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowWebAuthenticationWithOptionsAsync)(
        XAsyncBlock* async,
        XUserHandle requestingUser,
        const char* requestUri,
        const char* completionUri,
        XGameUiWebAuthenticationOptions options);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowWebAuthenticationResultSize)(
        XAsyncBlock* async,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameUiShowWebAuthenticationResult)(
        XAsyncBlock* async,
        size_t bufferSize,
        void* buffer,
        XGameUiWebAuthenticationResultData** ptrToBuffer,
        size_t* bufferUsed);
};

struct IXgrGameInviteApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XGameInviteRegisterForEvent)(
        XTaskQueueHandle queue,
        void* context,
        XGameInviteEventCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XGameInviteUnregisterForEvent)(
        XTaskQueueRegistrationToken token,
        bool wait);
};

struct IXgrAccessibilityApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XClosedCaptionGetProperties)(
        XClosedCaptionProperties* properties);
    XGameResult(OPENSCARLETT_GDK_CALL* XClosedCaptionSetEnabled)(
        bool enabled);
    XGameResult(OPENSCARLETT_GDK_CALL* XHighContrastGetMode)(
        XHighContrastMode* mode);
};

struct IXgrSpeechSynthesizerApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerCreate)(
        XSpeechSynthesizerHandle* speechSynthesizer);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerCloseHandle)(
        XSpeechSynthesizerHandle speechSynthesizer);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerEnumerateInstalledVoices)(
        void* context,
        XSpeechSynthesizerInstalledVoicesCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerSetDefaultVoice)(
        XSpeechSynthesizerHandle speechSynthesizer);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerCreateStreamFromText)(
        XSpeechSynthesizerHandle speechSynthesizer,
        const char* text,
        XSpeechSynthesizerStreamHandle* speechSynthesisStream);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerGetStreamDataSize)(
        XSpeechSynthesizerStreamHandle speechSynthesisStream,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerGetStreamData)(
        XSpeechSynthesizerStreamHandle speechSynthesisStream,
        size_t bufferSize,
        void* buffer,
        size_t* bufferUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XSpeechSynthesizerCloseStreamHandle)(
        XSpeechSynthesizerStreamHandle speechSynthesisStream);
};

struct IXgrNetworkingApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingGetConnectivityHint)(
        XNetworkingConnectivityHint* connectivityHint);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQueryConnectivityHint)(
        XNetworkingConnectivityHint* connectivityHint);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQueryPreferredLocalUdpMultiplayerPort)(
        uint16_t* preferredLocalUdpMultiplayerPort);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync)(
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult)(
        XAsyncBlock* async,
        uint16_t* preferredLocalUdpMultiplayerPort);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingRegisterConnectivityHintChanged)(
        XTaskQueueHandle queue,
        void* context,
        XNetworkingConnectivityHintChangedCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XNetworkingUnregisterConnectivityHintChanged)(
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged)(
        XTaskQueueHandle queue,
        void* context,
        XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged)(
        XTaskQueueRegistrationToken token,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQuerySecurityInformationForUrlAsync)(
        const char* url,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQuerySecurityInformationForUrlAsyncResultSize)(
        XAsyncBlock* async,
        size_t* securityInformationBufferByteCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQuerySecurityInformationForUrlAsyncResult)(
        XAsyncBlock* async,
        size_t securityInformationBufferByteCount,
        size_t* securityInformationBufferByteCountUsed,
        uint8_t* securityInformationBuffer,
        XNetworkingSecurityInformation** pointerToSecurityInformation);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQuerySecurityInformationForUrlUtf16Async)(
        const wchar_t* url,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize)(
        XAsyncBlock* async,
        size_t* securityInformationBufferByteCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult)(
        XAsyncBlock* async,
        size_t securityInformationBufferByteCount,
        size_t* securityInformationBufferByteCountUsed,
        uint8_t* securityInformationBuffer,
        XNetworkingSecurityInformation** pointerToSecurityInformation);
    XGameResult(OPENSCARLETT_GDK_CALL* XNetworkingVerifyServerCertificate)(
        void* requestHandle,
        const XNetworkingSecurityInformation* securityInformation);
};

struct IXgrCoreRuntimeApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    bool(OPENSCARLETT_GDK_CALL* XGameRuntimeIsFeatureAvailable)(XGameRuntimeFeature feature);
    XGameResult(OPENSCARLETT_GDK_CALL* XPersistentLocalStorageGetPathSize)(size_t* pathSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XPersistentLocalStorageGetPath)(
        size_t pathSize,
        char* path,
        size_t* pathUsed);
    bool(OPENSCARLETT_GDK_CALL* XThreadIsTimeSensitive)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XThreadSetTimeSensitive)(bool isTimeSensitiveThread);
    void(OPENSCARLETT_GDK_CALL* XThreadAssertNotTimeSensitive)(void);
    bool(OPENSCARLETT_GDK_CALL* XThreadVerifyNotTimeSensitive)(void);
    void(OPENSCARLETT_GDK_CALL* XErrorReport)(XGameResult hr, const char* msg);
    void(OPENSCARLETT_GDK_CALL* XErrorSetCallback)(XErrorCallback callback, void* context);
    void(OPENSCARLETT_GDK_CALL* XErrorSetOptions)(
        XErrorOptions optionsDebuggerPresent,
        XErrorOptions optionsDebuggerNotPresent);
    uint32_t(OPENSCARLETT_GDK_CALL* XGameEventWrite)(
        uint64_t regHandle,
        const XGameEventDescriptor* eventDescriptor,
        uint32_t userDataCount,
        XGameEventDataDescriptor* userData);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameInviteRegisterForEvent)(
        XTaskQueueHandle queue,
        void* context,
        XGameInviteEventCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XGameInviteUnregisterForEvent)(
        XTaskQueueRegistrationToken token,
        bool wait);
    void(OPENSCARLETT_GDK_CALL* XLaunchNewGame)(
        const char* exePath,
        const char* args,
        XUserHandle defaultUser);
    XGameResult(OPENSCARLETT_GDK_CALL* XLaunchUri)(XUserHandle requestingUser, const char* uri);
    XGameResult(OPENSCARLETT_GDK_CALL* XHighContrastGetMode)(XHighContrastMode* mode);
    XGameResult(OPENSCARLETT_GDK_CALL* XClosedCaptionGetProperties)(
        XClosedCaptionProperties* properties);
    XGameResult(OPENSCARLETT_GDK_CALL* XClosedCaptionSetEnabled)(bool enabled);
};

struct IXgrGameStreamingApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingInitialize)(void);
    void(OPENSCARLETT_GDK_CALL* XGameStreamingUninitialize)(void);
    bool(OPENSCARLETT_GDK_CALL* XGameStreamingIsStreaming)(void);
    uint32_t(OPENSCARLETT_GDK_CALL* XGameStreamingGetClientCount)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingGetClients)(
        uint32_t clientCount,
        XGameStreamingClientId* clients,
        uint32_t* clientsUsed);
    XGameStreamingConnectionState(OPENSCARLETT_GDK_CALL* XGameStreamingGetConnectionState)(
        XGameStreamingClientId client);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingRegisterConnectionStateChanged)(
        XTaskQueueHandle queue,
        void* context,
        XGameStreamingConnectionStateChangedCallback callback,
        XTaskQueueRegistrationToken* token);
    bool(OPENSCARLETT_GDK_CALL* XGameStreamingUnregisterConnectionStateChanged)(
        XTaskQueueRegistrationToken token,
        bool wait);
    void(OPENSCARLETT_GDK_CALL* XGameStreamingHideTouchControls)(void);
    void(OPENSCARLETT_GDK_CALL* XGameStreamingShowTouchControlLayout)(const char* layout);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingGetStreamPhysicalDimensions)(
        XGameStreamingClientId client,
        uint32_t* horizontalMm,
        uint32_t* verticalMm);
    size_t(OPENSCARLETT_GDK_CALL* XGameStreamingGetServerLocationNameSize)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingGetServerLocationName)(
        size_t serverLocationNameSize,
        char* serverLocationName);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingGetLastFrameDisplayed)(
        XGameStreamingClientId client,
        uint64_t* frameId);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameStreamingIsTouchInputEnabledOnClient)(
        XGameStreamingClientId client,
        bool* touchInputEnabled);
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL InitializeApiImpl(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL InitializeApiImplEx(
    uint64_t versionHigh,
    uint64_t versionLow,
    uint64_t options);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL UninitializeApiImpl(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL QueryApiImpl(
    const XgrGuid* apiId,
    void** ppApiVtable
#ifdef __cplusplus
    ,
    void** ppApiVtable3 = nullptr
#endif
);
OPENSCARLETT_GDK_API int32_t OPENSCARLETT_GDK_CALL XgrGetTelemetryCallCountForTest(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outNotImplCalls);

#ifdef __cplusplus
}
#endif
