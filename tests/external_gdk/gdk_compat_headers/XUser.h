#pragma once

// MIT Microsoft GDK <XUser.h> public C/C++ ABI header.

#include "XAsync.h"

typedef struct XUser* XUserHandle;

struct XUserLocalId {
    uint64_t value;
};

enum class XUserAddOptions : uint32_t {
    None = 0x00,
    AddDefaultUserSilently = 0x01,
    AllowGuests = 0x02,
    AddDefaultUserAllowingUI = 0x04,
};

inline XUserAddOptions operator|(XUserAddOptions a, XUserAddOptions b) noexcept {
    return static_cast<XUserAddOptions>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

enum class XUserGamertagComponent : uint32_t {
    Classic = 0,
    Modern = 1,
    ModernSuffix = 2,
    UniqueModern = 3,
};

constexpr size_t XUserGamertagComponentClassicMaxBytes = 16;
constexpr size_t XUserGamertagComponentModernMaxBytes = 97;
constexpr size_t XUserGamertagComponentModernSuffixMaxBytes = 15;
constexpr size_t XUserGamertagComponentUniqueModernMaxBytes = 101;

enum class XUserState : uint32_t {
    SignedIn = 0,
    SigningOut = 1,
    SignedOut = 2,
};

enum class XUserAgeGroup : uint32_t {
    Unknown = 0,
    Child = 1,
    Teen = 2,
    Adult = 3,
};

enum class XUserPrivilegeOptions : uint32_t {
    None = 0x00,
    AllUsers = 0x01,
};

enum class XUserPrivilege : uint32_t {
    CrossPlay = 185,
    Clubs = 188,
    Sessions = 189,
    Broadcast = 190,
    ManageProfilePrivacy = 196,
    GameDvr = 198,
    MultiplayerParties = 203,
    CloudManageSession = 207,
    CloudJoinSession = 208,
    CloudSavedGames = 209,
    SocialNetworkSharing = 220,
    UserGeneratedContent = 247,
    Communications = 252,
    Multiplayer = 254,
    AddFriends = 255,
};

enum class XUserPrivilegeDenyReason : uint32_t {
    None = 0,
    PurchaseRequired = 1,
    Restricted = 2,
    Banned = 3,
    Unknown = 0xFFFFFFFFu,
};

enum class XUserGamerPictureSize : uint32_t {
    Small = 0,
    Medium = 1,
    Large = 2,
    ExtraLarge = 3,
};

#ifdef __cplusplus
extern "C" {
#endif

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserAddAsync(
    XUserAddOptions options,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle);

XGR_COMPAT_API void XGR_COMPAT_CALL XUserCloseHandle(XUserHandle user);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetId(
    XUserHandle user,
    uint64_t* userId);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetLocalId(
    XUserHandle user,
    XUserLocalId* userLocalId);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetState(
    XUserHandle user,
    XUserState* state);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetAgeGroup(
    XUserHandle user,
    XUserAgeGroup* ageGroup);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetMaxUsers(uint32_t* maxUsers);

XGR_COMPAT_API int32_t XGR_COMPAT_CALL XUserCompare(
    XUserHandle user1,
    XUserHandle user2);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);

XGR_COMPAT_API HRESULT XGR_COMPAT_CALL XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed);

#ifdef __cplusplus
}
#endif
