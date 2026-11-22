// OpenScarlett Live Verification Suite:
// Commercial GDK Thunks (292/292 xgameruntime.dll + 16/16 XCurl.dll) & Full GDK Lifecycle.
//
// Always runs (CI & local):
//   1. Verifies 292/292 public GDK thunk export coverage on OpenScarlett's xgameruntime.dll
//      (both PE export table inspection and live GetProcAddress resolution).
//   2. Verifies 16/16 XCurl.dll export coverage required by commercial XSAPI thunks.
//   3. Exercises the full GDK startup / XTaskQueue / XUser / XGameSave (S:\) / XStore / XSAPI
//      lifecycle against OpenScarlett's DLLs.
//
// Runs automatically when commercial Alexandria IV DEMO GDK thunks are installed locally
// (gracefully skipped with clear log message on CI where the demo is not installed):
//   4. Preloads OpenScarlett's xgameruntime.dll + XCurl.dll and dynamically loads the real
//      shipping XGamingRuntimeThunks.dll + Microsoft.Xbox.Services.141.GDK.C.Thunks.dll.
//   5. Asserts S_OK (ZERO E_NOINTERFACE) for XGameRuntimeInitialize, XGameGetXboxTitleId
//      (expecting 0x6BFFDF10 from MicrosoftGame.Config / OPENSCARLETT_TITLE_ID), and
//      XTaskQueueCreate, plus 16/16 XCurl.dll import resolution on the real XSAPI thunks DLL.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

// Full 292-entry public GDK thunk export table matching shipping XGamingRuntimeThunks.dll.
// List of documented public Microsoft GDK C API symbol names (sorted alphabetically).
constexpr std::array<const char*, 292> kRequiredXgameruntimeExports = {{
    "QueryApiImpl",
    "XAppBroadcastGetStatus",
    "XAppBroadcastIsAppBroadcasting",
    "XAppBroadcastRegisterIsAppBroadcastingChanged",
    "XAppBroadcastShowUI",
    "XAppBroadcastUnregisterIsAppBroadcastingChanged",
    "XAppCaptureCloseScreenshotStream",
    "XAppCaptureDisableRecord",
    "XAppCaptureEnableRecord",
    "XAppCaptureMetadataAddDoubleEvent",
    "XAppCaptureMetadataAddInt32Event",
    "XAppCaptureMetadataAddStringEvent",
    "XAppCaptureMetadataRemainingStorageBytesAvailable",
    "XAppCaptureMetadataStartDoubleState",
    "XAppCaptureMetadataStartInt32State",
    "XAppCaptureMetadataStartStringState",
    "XAppCaptureMetadataStopAllStates",
    "XAppCaptureMetadataStopState",
    "XAppCaptureOpenScreenshotStream",
    "XAppCaptureReadScreenshotStream",
    "XAppCaptureRecordDiagnosticClip",
    "XAppCaptureRegisterMetadataPurged",
    "XAppCaptureTakeDiagnosticScreenshot",
    "XAppCaptureTakeScreenshot",
    "XAppCaptureUnRegisterMetadataPurged",
    "XAsyncBegin",
    "XAsyncCancel",
    "XAsyncComplete",
    "XAsyncGetResult",
    "XAsyncGetResultSize",
    "XAsyncGetStatus",
    "XAsyncRun",
    "XAsyncSchedule",
    "XClosedCaptionGetProperties",
    "XClosedCaptionSetEnabled",
    "XDisplayAcquireTimeoutDeferral",
    "XDisplayCloseTimeoutDeferralHandle",
    "XDisplayTryEnableHdrMode",
    "XErrorReport",
    "XErrorSetCallback",
    "XErrorSetOptions",
    "XGameEventWrite",
    "XGameGetXboxTitleId",
    "XGameInviteRegisterForEvent",
    "XGameInviteUnregisterForEvent",
    "XGameRuntimeInitialize",
    "XGameRuntimeIsFeatureAvailable",
    "XGameRuntimeUninitialize",
    "XGameSaveCloseContainer",
    "XGameSaveCloseProvider",
    "XGameSaveCloseUpdate",
    "XGameSaveCreateContainer",
    "XGameSaveCreateUpdate",
    "XGameSaveDeleteContainer",
    "XGameSaveDeleteContainerAsync",
    "XGameSaveDeleteContainerResult",
    "XGameSaveEnumerateBlobInfo",
    "XGameSaveEnumerateBlobInfoByName",
    "XGameSaveEnumerateContainerInfo",
    "XGameSaveEnumerateContainerInfoByName",
    "XGameSaveGetContainerInfo",
    "XGameSaveGetRemainingQuota",
    "XGameSaveGetRemainingQuotaAsync",
    "XGameSaveGetRemainingQuotaResult",
    "XGameSaveInitializeProvider",
    "XGameSaveInitializeProviderAsync",
    "XGameSaveInitializeProviderResult",
    "XGameSaveReadBlobData",
    "XGameSaveReadBlobDataAsync",
    "XGameSaveReadBlobDataResult",
    "XGameSaveSubmitBlobDelete",
    "XGameSaveSubmitBlobWrite",
    "XGameSaveSubmitUpdate",
    "XGameSaveSubmitUpdateAsync",
    "XGameSaveSubmitUpdateResult",
    "XGameUiSetNotificationPositionHint",
    "XGameUiShowAchievementsAsync",
    "XGameUiShowAchievementsResult",
    "XGameUiShowErrorDialogAsync",
    "XGameUiShowErrorDialogResult",
    "XGameUiShowMessageDialogAsync",
    "XGameUiShowMessageDialogResult",
    "XGameUiShowPlayerPickerAsync",
    "XGameUiShowPlayerPickerResult",
    "XGameUiShowPlayerPickerResultCount",
    "XGameUiShowPlayerProfileCardAsync",
    "XGameUiShowPlayerProfileCardResult",
    "XGameUiShowSendGameInviteAsync",
    "XGameUiShowSendGameInviteResult",
    "XGameUiShowTextEntryAsync",
    "XGameUiShowTextEntryResult",
    "XGameUiShowTextEntryResultSize",
    "XGameUiShowWebAuthenticationAsync",
    "XGameUiShowWebAuthenticationResult",
    "XGameUiShowWebAuthenticationResultSize",
    "XGameUiShowWebAuthenticationWithOptionsAsync",
    "XHighContrastGetMode",
    "XLaunchNewGame",
    "XLaunchUri",
    "XNetworkingGetConnectivityHint",
    "XNetworkingQueryPreferredLocalUdpMultiplayerPort",
    "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync",
    "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult",
    "XNetworkingQuerySecurityInformationForUrlAsync",
    "XNetworkingQuerySecurityInformationForUrlAsyncResult",
    "XNetworkingQuerySecurityInformationForUrlAsyncResultSize",
    "XNetworkingQuerySecurityInformationForUrlUtf16Async",
    "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult",
    "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize",
    "XNetworkingRegisterConnectivityHintChanged",
    "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged",
    "XNetworkingUnregisterConnectivityHintChanged",
    "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged",
    "XNetworkingVerifyServerCertificate",
    "XPackageChangeChunkInstallOrder",
    "XPackageCloseInstallationMonitorHandle",
    "XPackageCloseMountHandle",
    "XPackageCreateInstallationMonitor",
    "XPackageEnumerateChunkAvailability",
    "XPackageEnumerateFeatures",
    "XPackageEnumeratePackages",
    "XPackageEstimateDownloadSize",
    "XPackageFindChunkAvailability",
    "XPackageGetCurrentProcessPackageIdentifier",
    "XPackageGetInstallationProgress",
    "XPackageGetMountPath",
    "XPackageGetMountPathSize",
    "XPackageGetUserLocale",
    "XPackageGetWriteStats",
    "XPackageInstallChunks",
    "XPackageInstallChunksAsync",
    "XPackageInstallChunksResult",
    "XPackageIsPackagedProcess",
    "XPackageMount",
    "XPackageRegisterInstallationProgressChanged",
    "XPackageRegisterPackageInstalled",
    "XPackageUninstallChunks",
    "XPackageUninstallUWPInstance",
    "XPackageUnregisterInstallationProgressChanged",
    "XPackageUnregisterPackageInstalled",
    "XPackageUpdateInstallationMonitor",
    "XPersistentLocalStorageGetPath",
    "XPersistentLocalStorageGetPathSize",
    "XSpeechSynthesizerCloseHandle",
    "XSpeechSynthesizerCloseStreamHandle",
    "XSpeechSynthesizerCreate",
    "XSpeechSynthesizerCreateStreamFromSsml",
    "XSpeechSynthesizerCreateStreamFromText",
    "XSpeechSynthesizerEnumerateInstalledVoices",
    "XSpeechSynthesizerGetStreamData",
    "XSpeechSynthesizerGetStreamDataSize",
    "XSpeechSynthesizerSetCustomVoice",
    "XSpeechSynthesizerSetDefaultVoice",
    "XSpeechToTextBeginHypothesisString",
    "XSpeechToTextCancelHypothesisString",
    "XSpeechToTextFinalizeHypothesisString",
    "XSpeechToTextSendString",
    "XSpeechToTextSetPositionHint",
    "XSpeechToTextUpdateHypothesisString",
    "XStoreAcquireLicenseForDurablesAsync",
    "XStoreAcquireLicenseForDurablesResult",
    "XStoreAcquireLicenseForPackageAsync",
    "XStoreAcquireLicenseForPackageResult",
    "XStoreCanAcquireLicenseForPackageAsync",
    "XStoreCanAcquireLicenseForPackageResult",
    "XStoreCanAcquireLicenseForStoreIdAsync",
    "XStoreCanAcquireLicenseForStoreIdResult",
    "XStoreCloseContextHandle",
    "XStoreCloseLicenseHandle",
    "XStoreCloseProductsQueryHandle",
    "XStoreCreateContext",
    "XStoreDownloadAndInstallPackageUpdatesAsync",
    "XStoreDownloadAndInstallPackageUpdatesResult",
    "XStoreDownloadAndInstallPackagesAsync",
    "XStoreDownloadAndInstallPackagesResult",
    "XStoreDownloadAndInstallPackagesResultCount",
    "XStoreDownloadPackageUpdatesAsync",
    "XStoreDownloadPackageUpdatesResult",
    "XStoreEnumerateProductsQuery",
    "XStoreGetUserCollectionsIdAsync",
    "XStoreGetUserCollectionsIdResult",
    "XStoreGetUserCollectionsIdResultSize",
    "XStoreGetUserPurchaseIdAsync",
    "XStoreGetUserPurchaseIdResult",
    "XStoreGetUserPurchaseIdResultSize",
    "XStoreIsAvailabilityPurchasable",
    "XStoreIsLicenseValid",
    "XStoreProductsQueryHasMorePages",
    "XStoreProductsQueryNextPageAsync",
    "XStoreProductsQueryNextPageResult",
    "XStoreQueryAddOnLicensesAsync",
    "XStoreQueryAddOnLicensesResult",
    "XStoreQueryAddOnLicensesResultCount",
    "XStoreQueryAssociatedProductsAsync",
    "XStoreQueryAssociatedProductsResult",
    "XStoreQueryConsumableBalanceRemainingAsync",
    "XStoreQueryConsumableBalanceRemainingResult",
    "XStoreQueryEntitledProductsAsync",
    "XStoreQueryEntitledProductsResult",
    "XStoreQueryGameAndDlcPackageUpdatesAsync",
    "XStoreQueryGameAndDlcPackageUpdatesResult",
    "XStoreQueryGameAndDlcPackageUpdatesResultCount",
    "XStoreQueryGameLicenseAsync",
    "XStoreQueryGameLicenseResult",
    "XStoreQueryLicenseTokenAsync",
    "XStoreQueryLicenseTokenResult",
    "XStoreQueryLicenseTokenResultSize",
    "XStoreQueryPackageIdentifier",
    "XStoreQueryProductForCurrentGameAsync",
    "XStoreQueryProductForCurrentGameResult",
    "XStoreQueryProductForPackageAsync",
    "XStoreQueryProductForPackageResult",
    "XStoreQueryProductsAsync",
    "XStoreQueryProductsResult",
    "XStoreRegisterGameLicenseChanged",
    "XStoreRegisterPackageLicenseLost",
    "XStoreReportConsumableFulfillmentAsync",
    "XStoreReportConsumableFulfillmentResult",
    "XStoreShowAssociatedProductsUIAsync",
    "XStoreShowAssociatedProductsUIResult",
    "XStoreShowProductPageUIAsync",
    "XStoreShowProductPageUIResult",
    "XStoreShowPurchaseUIAsync",
    "XStoreShowPurchaseUIResult",
    "XStoreShowRateAndReviewUIAsync",
    "XStoreShowRateAndReviewUIResult",
    "XStoreShowRedeemTokenUIAsync",
    "XStoreShowRedeemTokenUIResult",
    "XStoreUnregisterGameLicenseChanged",
    "XStoreUnregisterPackageLicenseLost",
    "XSystemGetAnalyticsInfo",
    "XSystemGetConsoleId",
    "XSystemGetXboxLiveSandboxId",
    "XTaskQueueCloseHandle",
    "XTaskQueueCreate",
    "XTaskQueueCreateComposite",
    "XTaskQueueDispatch",
    "XTaskQueueDuplicateHandle",
    "XTaskQueueGetCurrentProcessTaskQueue",
    "XTaskQueueGetPort",
    "XTaskQueueRegisterMonitor",
    "XTaskQueueRegisterWaiter",
    "XTaskQueueSetCurrentProcessTaskQueue",
    "XTaskQueueSubmitCallback",
    "XTaskQueueSubmitDelayedCallback",
    "XTaskQueueTerminate",
    "XTaskQueueUnregisterMonitor",
    "XTaskQueueUnregisterWaiter",
    "XThreadAssertNotTimeSensitive",
    "XThreadIsTimeSensitive",
    "XThreadSetTimeSensitive",
    "XThreadVerifyNotTimeSensitive",
    "XUserAddAsync",
    "XUserAddResult",
    "XUserCheckPrivilege",
    "XUserCloseHandle",
    "XUserCloseSignOutDeferralHandle",
    "XUserCompare",
    "XUserDuplicateHandle",
    "XUserFindForDevice",
    "XUserFindUserById",
    "XUserFindUserByLocalId",
    "XUserGetAgeGroup",
    "XUserGetDefaultAudioEndpointUtf16",
    "XUserGetGamerPictureAsync",
    "XUserGetGamerPictureResult",
    "XUserGetGamerPictureResultSize",
    "XUserGetGamertag",
    "XUserGetId",
    "XUserGetIsGuest",
    "XUserGetLocalId",
    "XUserGetMaxUsers",
    "XUserGetSignOutDeferral",
    "XUserGetState",
    "XUserGetTokenAndSignatureAsync",
    "XUserGetTokenAndSignatureResult",
    "XUserGetTokenAndSignatureResultSize",
    "XUserGetTokenAndSignatureUtf16Async",
    "XUserGetTokenAndSignatureUtf16Result",
    "XUserGetTokenAndSignatureUtf16ResultSize",
    "XUserRegisterForChangeEvent",
    "XUserRegisterForDefaultAudioEndpointUtf16Changed",
    "XUserRegisterForDeviceAssociationChanged",
    "XUserResolveIssueWithUiAsync",
    "XUserResolveIssueWithUiResult",
    "XUserResolveIssueWithUiUtf16Async",
    "XUserResolveIssueWithUiUtf16Result",
    "XUserResolvePrivilegeWithUiAsync",
    "XUserResolvePrivilegeWithUiResult",
    "XUserUnregisterForChangeEvent",
    "XUserUnregisterForDefaultAudioEndpointUtf16Changed",
    "XUserUnregisterForDeviceAssociationChanged",
}};

// Full 16-entry XCurl.dll import set required by shipping Microsoft.Xbox.Services.141.GDK.C.Thunks.dll.
constexpr std::array<const char*, 16> kRequiredXCurlExports = {{
    "curl_easy_cleanup",
    "curl_easy_getinfo",
    "curl_easy_init",
    "curl_easy_setopt",
    "curl_easy_strerror",
    "curl_global_cleanup",
    "curl_global_init",
    "curl_multi_add_handle",
    "curl_multi_cleanup",
    "curl_multi_info_read",
    "curl_multi_init",
    "curl_multi_perform",
    "curl_multi_poll",
    "curl_multi_remove_handle",
    "curl_slist_append",
    "curl_slist_free_all",
}};

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"xgameruntime.dll", ec) &&
            fs::exists(exe_dir / L"XCurl.dll", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path ResolveScratchDir(const wchar_t* subdir) {
    std::error_code ec;
    wchar_t temp_buf[1024]{};
    const DWORD len = ::GetEnvironmentVariableW(L"TEMP", temp_buf, 1024u);
    if (len > 0 && len < 1024u) {
        return fs::path(temp_buf) / subdir;
    }
    const fs::path sys_tmp = fs::temp_directory_path(ec);
    if (!ec && !sys_tmp.empty()) {
        return sys_tmp / subdir;
    }
    return ResolveRuntimeBinDir() / subdir;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string GetLoadedModulePathUtf8(const wchar_t* mod_name) {
    HMODULE h = ::GetModuleHandleW(mod_name);
    if (!h) return "<not-loaded>";
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(h, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return "<error>";
    return WideToUtf8(buf);
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? (c - 'A' + 'a') : c);
    });
    return s;
}

struct CoverageResult {
    size_t matched = 0;
    size_t total = 0;
    std::vector<std::string> missing;
};

template <size_t N>
CoverageResult VerifyDllExports(const fs::path& dll_path,
                                HMODULE loaded_mod,
                                const std::array<const char*, N>& expected) {
    openscarlett::PeInspectionReport rep;
    OS_REQUIRE(openscarlett::InspectPeFile(dll_path.string(), rep).ok());
    OS_REQUIRE(rep.is_dll);

    std::set<std::string> pe_names;
    for (const auto& exp : rep.exports) {
        if (!exp.name.empty()) {
            pe_names.insert(exp.name);
        }
    }

    CoverageResult res{};
    res.total = N;
    for (const char* name : expected) {
        const bool in_pe = pe_names.find(name) != pe_names.end();
        const FARPROC proc = loaded_mod ? ::GetProcAddress(loaded_mod, name) : nullptr;
        if (in_pe && proc != nullptr) {
            ++res.matched;
        } else {
            res.missing.emplace_back(name);
        }
    }
    return res;
}

void VerifyCoreGdkAndXsapiLifecycle(HMODULE h_xgr, HMODULE h_xcurl, HMODULE h_xsapi) {
    std::printf("\n--- [3/4] Full OpenScarlett GDK + Save + Store + XCurl + XSAPI Lifecycle ---\n");

    // Configure clean scratch save root and test TitleId.
    const fs::path save_root = ResolveScratchDir(L"openscarlett_real_thunks_saves");
    std::error_code ec;
    fs::remove_all(save_root, ec);
    fs::create_directories(save_root, ec);
    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_SAVE_ROOT", save_root.c_str()) != 0);
    OS_REQUIRE(::SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "6BFFDF10") != 0);

    // 3a. XCurl basic handle lifecycle
    using CurlGlobalInitFn = int(__cdecl*)(long);
    using CurlGlobalCleanupFn = void(__cdecl*)();
    using CurlEasyInitFn = void*(__cdecl*)();
    using CurlEasyCleanupFn = void(__cdecl*)(void*);
    using CurlMultiInitFn = void*(__cdecl*)();
    using CurlMultiCleanupFn = int(__cdecl*)(void*);

    auto p_curl_ginit = reinterpret_cast<CurlGlobalInitFn>(::GetProcAddress(h_xcurl, "curl_global_init"));
    auto p_curl_gclean = reinterpret_cast<CurlGlobalCleanupFn>(::GetProcAddress(h_xcurl, "curl_global_cleanup"));
    auto p_curl_einit = reinterpret_cast<CurlEasyInitFn>(::GetProcAddress(h_xcurl, "curl_easy_init"));
    auto p_curl_eclean = reinterpret_cast<CurlEasyCleanupFn>(::GetProcAddress(h_xcurl, "curl_easy_cleanup"));
    auto p_curl_minit = reinterpret_cast<CurlMultiInitFn>(::GetProcAddress(h_xcurl, "curl_multi_init"));
    auto p_curl_mclean = reinterpret_cast<CurlMultiCleanupFn>(::GetProcAddress(h_xcurl, "curl_multi_cleanup"));
    OS_REQUIRE(p_curl_ginit && p_curl_gclean && p_curl_einit && p_curl_eclean && p_curl_minit && p_curl_mclean);

    OS_REQUIRE(p_curl_ginit(3L) == 0);
    void* easy = p_curl_einit();
    OS_REQUIRE(easy != nullptr);
    void* multi = p_curl_minit();
    OS_REQUIRE(multi != nullptr);
    OS_REQUIRE(p_curl_mclean(multi) == 0);
    p_curl_eclean(easy);
    p_curl_gclean();
    std::printf("  [OK] XCurl.dll global/easy/multi lifecycle\n");

    // 3b. xgameruntime lifecycle: startup -> TitleId -> XTaskQueue -> XUser -> XGameSave -> XStore
    auto p_init = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)()>(
        ::GetProcAddress(h_xgr, "XGameRuntimeInitialize"));
    auto p_uninit = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)()>(
        ::GetProcAddress(h_xgr, "XGameRuntimeUninitialize"));
    auto p_title_id = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(uint32_t*)>(
        ::GetProcAddress(h_xgr, "XGameGetXboxTitleId"));
    auto p_tq_create = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*)>(
        ::GetProcAddress(h_xgr, "XTaskQueueCreate"));
    auto p_tq_close = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)(XTaskQueueHandle)>(
        ::GetProcAddress(h_xgr, "XTaskQueueCloseHandle"));
    auto p_async_status = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(XAsyncBlock*, bool)>(
        ::GetProcAddress(h_xgr, "XAsyncGetStatus"));
    auto p_user_add = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(XUserAddOptions, XAsyncBlock*)>(
        ::GetProcAddress(h_xgr, "XUserAddAsync"));
    auto p_user_res = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(XAsyncBlock*, XUserHandle*)>(
        ::GetProcAddress(h_xgr, "XUserAddResult"));
    auto p_user_tag = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XUserHandle, XUserGamertagComponent, size_t, char*, size_t*)>(
        ::GetProcAddress(h_xgr, "XUserGetGamertag"));
    auto p_user_close = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)(XUserHandle)>(
        ::GetProcAddress(h_xgr, "XUserCloseHandle"));
    auto p_save_init = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XUserHandle, const char*, bool, XGameSaveProviderHandle*)>(
        ::GetProcAddress(h_xgr, "XGameSaveInitializeProvider"));
    auto p_save_close = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)(XGameSaveProviderHandle)>(
        ::GetProcAddress(h_xgr, "XGameSaveCloseProvider"));
    auto p_save_cont = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XGameSaveProviderHandle, const char*, XGameSaveContainerHandle*)>(
        ::GetProcAddress(h_xgr, "XGameSaveCreateContainer"));
    auto p_save_cont_close = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)(XGameSaveContainerHandle)>(
        ::GetProcAddress(h_xgr, "XGameSaveCloseContainer"));
    auto p_save_upd = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XGameSaveContainerHandle, const char*, XGameSaveUpdateHandle*)>(
        ::GetProcAddress(h_xgr, "XGameSaveCreateUpdate"));
    auto p_save_write = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XGameSaveUpdateHandle, const char*, const uint8_t*, size_t)>(
        ::GetProcAddress(h_xgr, "XGameSaveSubmitBlobWrite"));
    auto p_save_submit = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(XGameSaveUpdateHandle)>(
        ::GetProcAddress(h_xgr, "XGameSaveSubmitUpdate"));
    auto p_save_upd_close = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)(XGameSaveUpdateHandle)>(
        ::GetProcAddress(h_xgr, "XGameSaveCloseUpdate"));
    auto p_save_read = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(
        XGameSaveContainerHandle, const char**, uint32_t*, size_t, XGameSaveBlob*)>(
        ::GetProcAddress(h_xgr, "XGameSaveReadBlobData"));
    auto p_store_create = reinterpret_cast<XGameResult(OPENSCARLETT_GDK_CALL*)(XUserHandle, void**)>(
        ::GetProcAddress(h_xgr, "XStoreCreateContext"));
    auto p_store_close = reinterpret_cast<void(OPENSCARLETT_GDK_CALL*)(void*)>(
        ::GetProcAddress(h_xgr, "XStoreCloseContextHandle"));

    OS_REQUIRE(p_init && p_uninit && p_title_id && p_tq_create && p_tq_close && p_async_status);
    OS_REQUIRE(p_user_add && p_user_res && p_user_tag && p_user_close);
    OS_REQUIRE(p_save_init && p_save_close && p_save_cont && p_save_cont_close);
    OS_REQUIRE(p_save_upd && p_save_write && p_save_submit && p_save_upd_close && p_save_read);
    OS_REQUIRE(p_store_create && p_store_close);

    OS_REQUIRE(p_init() == XGR_S_OK);

    uint32_t tid = 0;
    OS_REQUIRE(p_title_id(&tid) == XGR_S_OK);
    OS_REQUIRE(tid == 0x6BFFDF10u);

    XTaskQueueHandle queue = nullptr;
    OS_REQUIRE(p_tq_create(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &queue) == XGR_S_OK);
    OS_REQUIRE(queue != nullptr);

    XAsyncBlock user_async{};
    user_async.queue = queue;
    OS_REQUIRE(p_user_add(XUserAddOptions_AddDefaultUserSilently, &user_async) == XGR_S_OK);
    OS_REQUIRE(p_async_status(&user_async, true) == XGR_S_OK);
    XUserHandle user = nullptr;
    OS_REQUIRE(p_user_res(&user_async, &user) == XGR_S_OK && user != nullptr);

    char gamertag[64]{};
    size_t gt_used = 0;
    OS_REQUIRE(p_user_tag(user, XUserGamertagComponent_Classic, sizeof(gamertag), gamertag, &gt_used) == XGR_S_OK);
    OS_REQUIRE(gt_used > 1u);

    // Persistent XGameSave write + readback round-trip
    XGameSaveProviderHandle save_prov = nullptr;
    OS_REQUIRE(p_save_init(user, "00000000-0000-0000-0000-00006bffdf10", false, &save_prov) == XGR_S_OK);
    OS_REQUIRE(save_prov != nullptr);

    XGameSaveContainerHandle save_cont = nullptr;
    OS_REQUIRE(p_save_cont(save_prov, "thunks_profile", &save_cont) == XGR_S_OK && save_cont != nullptr);

    XGameSaveUpdateHandle save_upd = nullptr;
    OS_REQUIRE(p_save_upd(save_cont, "Thunks Profile", &save_upd) == XGR_S_OK && save_upd != nullptr);
    const std::string payload = "OPENSCARLETT_GDK_SAVE_OK_6BFFDF10";
    OS_REQUIRE(p_save_write(
                   save_upd,
                   "checkpoint",
                   reinterpret_cast<const uint8_t*>(payload.data()),
                   payload.size()) == XGR_S_OK);
    OS_REQUIRE(p_save_submit(save_upd) == XGR_S_OK);
    p_save_upd_close(save_upd);

    const char* blob_names[1] = {"checkpoint"};
    uint32_t blob_count = 1u;
    std::vector<uint8_t> read_buf(4096u, 0u);
    auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
    OS_REQUIRE(p_save_read(save_cont, blob_names, &blob_count, read_buf.size(), blobs) == XGR_S_OK);
    OS_REQUIRE(blob_count == 1u);
    OS_REQUIRE(blobs[0].info.size == payload.size());
    OS_REQUIRE(std::memcmp(blobs[0].data, payload.data(), payload.size()) == 0);

    p_save_cont_close(save_cont);
    p_save_close(save_prov);

    // XStore context create/close
    void* store_ctx = nullptr;
    OS_REQUIRE(p_store_create(user, &store_ctx) == XGR_S_OK && store_ctx != nullptr);
    p_store_close(store_ctx);

    // XSAPI (Microsoft.Xbox.Services.GDK.C.dll) init / context / cleanup
    if (h_xsapi != nullptr) {
        struct XblInitArgsLocal {
            XTaskQueueHandle queue;
            const char* scid;
        };
        using XblInitFn = int32_t(__cdecl*)(const XblInitArgsLocal*);
        using XblCtxCreateFn = int32_t(__cdecl*)(XUserHandle, void**);
        using XblCtxCloseFn = void(__cdecl*)(void*);
        using XblCleanupAsyncFn = int32_t(__cdecl*)(XAsyncBlock*);

        auto p_xbl_init = reinterpret_cast<XblInitFn>(::GetProcAddress(h_xsapi, "XblInitialize"));
        auto p_xbl_ctx = reinterpret_cast<XblCtxCreateFn>(::GetProcAddress(h_xsapi, "XblContextCreateHandle"));
        auto p_xbl_close = reinterpret_cast<XblCtxCloseFn>(::GetProcAddress(h_xsapi, "XblContextCloseHandle"));
        auto p_xbl_clean = reinterpret_cast<XblCleanupAsyncFn>(::GetProcAddress(h_xsapi, "XblCleanupAsync"));
        OS_REQUIRE(p_xbl_init && p_xbl_ctx && p_xbl_close && p_xbl_clean);

        XblInitArgsLocal args{queue, "00000000-0000-0000-0000-00006bffdf10"};
        OS_REQUIRE(p_xbl_init(&args) == 0);
        void* xbl_ctx = nullptr;
        OS_REQUIRE(p_xbl_ctx(user, &xbl_ctx) == 0 && xbl_ctx != nullptr);
        p_xbl_close(xbl_ctx);

        XAsyncBlock clean_async{};
        clean_async.queue = queue;
        OS_REQUIRE(p_xbl_clean(&clean_async) == 0);
        OS_REQUIRE(p_async_status(&clean_async, true) == XGR_S_OK);
        std::printf("  [OK] Microsoft.Xbox.Services.GDK.C.dll XblInitialize/Context/Cleanup\n");
    }

    p_user_close(user);
    p_tq_close(queue);
    p_uninit();

    std::printf("  [OK] xgameruntime.dll full lifecycle (TitleId=0x%08X, Gamertag='%s', Save+Store OK)\n",
                tid, gamertag);
}

void VerifyRealCommercialThunksIfPresent(const fs::path& bin_dir) {
    std::printf("\n--- [4/4] Real Commercial GDK Thunks (Alexandria IV DEMO) ---\n");

    const fs::path alex_plugins =
        LR"(E:\Program Files\Alexandria IV DEMO\Content\Alexandria IV DEMO_Data\Plugins\x86_64)";
    const fs::path real_xgr_path = alex_plugins / L"XGamingRuntimeThunks.dll";
    const fs::path real_xsapi_path = alex_plugins / L"Microsoft.Xbox.Services.141.GDK.C.Thunks.dll";

    std::error_code ec;
    if (!fs::exists(real_xgr_path, ec) || !fs::exists(real_xsapi_path, ec)) {
        std::printf("  [SKIP] Commercial Alexandria IV DEMO plugins not installed at %s (CI/clean host).\n",
                    alex_plugins.string().c_str());
        return;
    }

    // Confirm OpenScarlett's xgameruntime.dll and XCurl.dll are already loaded from bin_dir.
    const std::string active_xgr = GetLoadedModulePathUtf8(L"xgameruntime.dll");
    const std::string active_xcurl = GetLoadedModulePathUtf8(L"XCurl.dll");
    const std::string bin_dir_lower = ToLowerAscii(bin_dir.string());
    std::printf("  Preloaded xgameruntime.dll : %s\n", active_xgr.c_str());
    std::printf("  Preloaded XCurl.dll        : %s\n", active_xcurl.c_str());
    OS_REQUIRE(ToLowerAscii(active_xgr).find(bin_dir_lower) != std::string::npos);
    OS_REQUIRE(ToLowerAscii(active_xcurl).find(bin_dir_lower) != std::string::npos);

    // Set TitleId to Alexandria IV DEMO's TitleId (0x6BFFDF10 from MicrosoftGame.Config).
    OS_REQUIRE(::SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "6BFFDF10") != 0);

    // 4a. Load REAL shipping Microsoft.Xbox.Services.141.GDK.C.Thunks.dll against OpenScarlett XCurl.dll.
    HMODULE h_real_xsapi = ::LoadLibraryExW(real_xsapi_path.c_str(), nullptr, 0);
    if (!h_real_xsapi) {
        std::fprintf(stderr, "  [FAIL] LoadLibraryExW(Microsoft.Xbox.Services.141.GDK.C.Thunks.dll) err=%lu\n",
                     static_cast<unsigned long>(::GetLastError()));
    }
    OS_REQUIRE(h_real_xsapi != nullptr);
    OS_REQUIRE(ToLowerAscii(GetLoadedModulePathUtf8(L"XCurl.dll")).find(bin_dir_lower) != std::string::npos);

    using XblGetErrorConditionFn = int32_t(__cdecl*)(int32_t);
    auto p_err_cond = reinterpret_cast<XblGetErrorConditionFn>(
        ::GetProcAddress(h_real_xsapi, "XblGetErrorCondition"));
    OS_REQUIRE(p_err_cond != nullptr);
    const int32_t err_cond = p_err_cond(0);
    OS_REQUIRE(err_cond == 0);
    std::printf("  [OK] Real Microsoft.Xbox.Services.141.GDK.C.Thunks.dll loaded (16/16 XCurl imports bound, XblGetErrorCondition(S_OK)=%d)\n",
                err_cond);

    // 4b. Load REAL shipping XGamingRuntimeThunks.dll against OpenScarlett xgameruntime.dll.
    HMODULE h_real_xgr = ::LoadLibraryExW(real_xgr_path.c_str(), nullptr, 0);
    if (!h_real_xgr) {
        std::fprintf(stderr, "  [FAIL] LoadLibraryExW(XGamingRuntimeThunks.dll) err=%lu\n",
                     static_cast<unsigned long>(::GetLastError()));
    }
    OS_REQUIRE(h_real_xgr != nullptr);

    using InitFn = HRESULT(__cdecl*)(void);
    using UninitFn = void(__cdecl*)(void);
    using TitleIdFn = HRESULT(__cdecl*)(uint32_t*);
    using TqCreateFn = HRESULT(__cdecl*)(uint32_t, uint32_t, void**);
    using TqCloseFn = void(__cdecl*)(void*);
    using MaxUsersFn = HRESULT(__cdecl*)(uint32_t*);
    using StoreCreateFn = HRESULT(__cdecl*)(void*, void**);
    using StoreCloseFn = void(__cdecl*)(void*);

    auto fn_init = reinterpret_cast<InitFn>(::GetProcAddress(h_real_xgr, "XGameRuntimeInitialize"));
    auto fn_uninit = reinterpret_cast<UninitFn>(::GetProcAddress(h_real_xgr, "XGameRuntimeUninitialize"));
    auto fn_tid = reinterpret_cast<TitleIdFn>(::GetProcAddress(h_real_xgr, "XGameGetXboxTitleId"));
    auto fn_tq_create = reinterpret_cast<TqCreateFn>(::GetProcAddress(h_real_xgr, "XTaskQueueCreate"));
    auto fn_tq_close = reinterpret_cast<TqCloseFn>(::GetProcAddress(h_real_xgr, "XTaskQueueCloseHandle"));
    auto fn_max_users = reinterpret_cast<MaxUsersFn>(::GetProcAddress(h_real_xgr, "XUserGetMaxUsers"));
    auto fn_store_create = reinterpret_cast<StoreCreateFn>(::GetProcAddress(h_real_xgr, "XStoreCreateContext"));
    auto fn_store_close = reinterpret_cast<StoreCloseFn>(::GetProcAddress(h_real_xgr, "XStoreCloseContextHandle"));
    OS_REQUIRE(fn_init && fn_uninit && fn_tid && fn_tq_create && fn_tq_close);

    const HRESULT hr_init = fn_init();
    std::printf("  Real XGamingRuntimeThunks!XGameRuntimeInitialize() -> 0x%08lX\n",
                static_cast<unsigned long>(hr_init));
    OS_REQUIRE(hr_init == S_OK);
    OS_REQUIRE(hr_init != E_NOINTERFACE);

    uint32_t real_tid = 0;
    const HRESULT hr_tid = fn_tid(&real_tid);
    std::printf("  Real XGamingRuntimeThunks!XGameGetXboxTitleId()    -> 0x%08lX (TitleId=0x%08X)\n",
                static_cast<unsigned long>(hr_tid), real_tid);
    OS_REQUIRE(hr_tid == S_OK);
    OS_REQUIRE(real_tid == 0x6BFFDF10u);

    void* real_q = nullptr;
    const HRESULT hr_tq = fn_tq_create(0u, 0u, &real_q);
    std::printf("  Real XGamingRuntimeThunks!XTaskQueueCreate()       -> 0x%08lX (queue=%p)\n",
                static_cast<unsigned long>(hr_tq), real_q);
    OS_REQUIRE(hr_tq == S_OK);
    OS_REQUIRE(real_q != nullptr);
    fn_tq_close(real_q);

    if (fn_max_users) {
        uint32_t max_u = 0;
        const HRESULT hr_u = fn_max_users(&max_u);
        std::printf("  Real XGamingRuntimeThunks!XUserGetMaxUsers()       -> 0x%08lX (max=%u)\n",
                    static_cast<unsigned long>(hr_u), max_u);
        OS_REQUIRE(hr_u == S_OK && max_u >= 1u);
    }
    if (fn_store_create && fn_store_close) {
        void* sctx = nullptr;
        const HRESULT hr_s = fn_store_create(nullptr, &sctx);
        std::printf("  Real XGamingRuntimeThunks!XStoreCreateContext()    -> 0x%08lX (ctx=%p)\n",
                    static_cast<unsigned long>(hr_s), sctx);
        OS_REQUIRE(hr_s == S_OK && sctx != nullptr);
        fn_store_close(sctx);
    }

    fn_uninit();
    ::FreeLibrary(h_real_xgr);
    ::FreeLibrary(h_real_xsapi);
    std::printf("  [OK] Real commercial GDK thunks verified with ZERO E_NOINTERFACE!\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path xgr_dll = bin_dir / L"xgameruntime.dll";
    const fs::path xcurl_dll = bin_dir / L"XCurl.dll";
    const fs::path xsapi_dll = bin_dir / L"Microsoft.Xbox.Services.GDK.C.dll";

    std::printf("=== OpenScarlett Real GDK Thunks Live Verification ===\n");
    std::printf("Runtime bin dir : %s\n", bin_dir.string().c_str());

    OS_REQUIRE(fs::exists(xgr_dll));
    OS_REQUIRE(fs::exists(xcurl_dll));

    // Preload OpenScarlett DLLs explicitly by full path from bin_dir.
    HMODULE h_xgr = ::LoadLibraryW(xgr_dll.c_str());
    HMODULE h_xcurl = ::LoadLibraryW(xcurl_dll.c_str());
    HMODULE h_xsapi = fs::exists(xsapi_dll) ? ::LoadLibraryW(xsapi_dll.c_str()) : nullptr;
    OS_REQUIRE(h_xgr != nullptr);
    OS_REQUIRE(h_xcurl != nullptr);

    // 1. Verify 292/292 xgameruntime.dll export coverage
    std::printf("\n--- [1/4] xgameruntime.dll Export Coverage (Target: 292/292) ---\n");
    const CoverageResult xgr_cov = VerifyDllExports(xgr_dll, h_xgr, kRequiredXgameruntimeExports);
    std::printf("  xgameruntime.dll coverage : %zu/%zu (missing=%zu)\n",
                xgr_cov.matched, xgr_cov.total, xgr_cov.missing.size());
    for (size_t i = 0; i < xgr_cov.missing.size(); ++i) {
        std::printf("    [MISSING %2zu] %s\n", i + 1, xgr_cov.missing[i].c_str());
    }

    // 2. Verify 16/16 XCurl.dll export coverage
    std::printf("\n--- [2/4] XCurl.dll Export Coverage (Target: 16/16) ---\n");
    const CoverageResult xcurl_cov = VerifyDllExports(xcurl_dll, h_xcurl, kRequiredXCurlExports);
    std::printf("  XCurl.dll coverage        : %zu/%zu (missing=%zu)\n",
                xcurl_cov.matched, xcurl_cov.total, xcurl_cov.missing.size());
    for (size_t i = 0; i < xcurl_cov.missing.size(); ++i) {
        std::printf("    [MISSING %2zu] %s\n", i + 1, xcurl_cov.missing[i].c_str());
    }
    OS_REQUIRE(xcurl_cov.matched == xcurl_cov.total);

    // 3. Full GDK / Save / Store / XCurl / XSAPI lifecycle
    VerifyCoreGdkAndXsapiLifecycle(h_xgr, h_xcurl, h_xsapi);

    // Enforce 292/292 xgameruntime.dll export coverage strictly!
    OS_REQUIRE(xgr_cov.matched == xgr_cov.total);

    // 4. Real commercial Alexandria IV DEMO thunks (when installed locally)
    VerifyRealCommercialThunksIfPresent(bin_dir);

    if (h_xsapi) ::FreeLibrary(h_xsapi);
    ::FreeLibrary(h_xcurl);
    ::FreeLibrary(h_xgr);

    std::printf("\n[PASS] test_real_gdk_thunks_live (292/292 xgameruntime + 16/16 XCurl + lifecycle verified)\n");
    return 0;
}
