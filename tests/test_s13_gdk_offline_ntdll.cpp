// OpenScarlett Sprint 13 — Worker 4 Comprehensive Verification Suite:
//   1. Offline-first Xbox Live XSTS token policy (default E_GAMEUSER_NO_TOKEN_REQUIRED
//      without stalls vs opt-in OPENSCARLETT_MOCK_XSTS=1 synthetic XBL3.0 JWT across
//      UTF-8, UTF-16, and XSAPI XblHttpCallPerformAsync + XblGetErrorCondition)
//   2. XTaskQueueRegisterWaiter repeated re-arming (auto-reset + manual-reset events,
//      6+ consecutive wakeups with zero lost wakeups and clean unregister)
//   3. XTaskQueueTerminate + XTaskQueueDispatch draining (drains queued items cleanly,
//      then returns false immediately even with INFINITE timeout; concurrent waiter wakeup)
//   4. XGameUiShowMessageDialogAsync & XGameUIShowMessageDialogAsync strict button bounds
//      validation + XErrorOptions 3-bit mask validation & XErrorGetOptions
//   5. Clean-room UMS helper shims (ntdll_ums_xs.dll Win32 + Rtl*Ums* & kernelx.dll UMS:
//      completion lists, thread contexts, query/set info, cooperative fiber Execute/Yield/
//      Dequeue/GetNextUmsListItem) + wldp_xs.dll watermark & production policy query rows
//
// Strictly portable (uses std::filesystem::temp_directory_path(), zero hardcoded paths).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/xsapi/xsapi_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr XGameResult kHrGameUserNoTokenRequired = static_cast<XGameResult>(0x89245105u);
constexpr uint32_t kUmsVersion = 0x0100u;
constexpr uint32_t kUmsReasonStartup = 0u;
constexpr uint32_t kUmsReasonYield = 2u;

constexpr uint32_t kUmsInfoUserContext = 1u;
constexpr uint32_t kUmsInfoPriority = 2u;
constexpr uint32_t kUmsInfoAffinity = 3u;
constexpr uint32_t kUmsInfoTeb = 4u;
constexpr uint32_t kUmsInfoIsSuspended = 5u;
constexpr uint32_t kUmsInfoIsTerminated = 6u;

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
#if defined(_WIN32)
        char buf[512]{};
        const DWORD n = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0u && n < sizeof(buf)) {
            had_prev_ = true;
            prev_.assign(buf, n);
        }
        ::SetEnvironmentVariableA(name, value);
#else
        const char* cur = std::getenv(name);
        if (cur) {
            had_prev_ = true;
            prev_ = cur;
        }
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
#endif
    }

    ~ScopedEnvVar() {
#if defined(_WIN32)
        ::SetEnvironmentVariableA(name_.c_str(), had_prev_ ? prev_.c_str() : nullptr);
#else
        if (had_prev_) {
            setenv(name_.c_str(), prev_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    bool had_prev_ = false;
    std::string prev_;
};

fs::path GetExeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0u && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#endif
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

void OPENSCARLETT_GDK_CALL AsyncFlagCb(XAsyncBlock* block) {
    if (block && block->context) {
        static_cast<std::atomic<bool>*>(block->context)->store(true, std::memory_order_release);
    }
}

// ===========================================================================
// 1. Offline-First XSTS Token Policy (UTF-8, UTF-16, XSAPI)
// ===========================================================================
void TestOfflineFirstXstsPolicy() {
    std::printf("[Test 1] Offline-first XSTS token policy (default no-token vs OPENSCARLETT_MOCK_XSTS=1)...\n");

    const fs::path save_root = fs::temp_directory_path() / "openscarlett_s13_gdk_offline_saves";
    std::error_code ec;
    fs::remove_all(save_root, ec);
    fs::create_directories(save_root, ec);
    ScopedEnvVar save_guard("OPENSCARLETT_SAVE_ROOT", save_root.string().c_str());

    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    XTaskQueueHandle q = nullptr;
    OS_REQUIRE(XTaskQueueCreate(
                   XTaskQueueDispatchMode_ThreadPool,
                   XTaskQueueDispatchMode_Manual,
                   &q) == XGR_S_OK && q != nullptr);

    XAsyncBlock user_ab{};
    OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) == XGR_S_OK);
    XUserHandle user = nullptr;
    OS_REQUIRE(XUserAddResult(&user_ab, &user) == XGR_S_OK && user != nullptr);

    XblInitArgs xbl_init{};
    xbl_init.queue = q;
    xbl_init.scid = "00000000-0000-0000-0000-00005ca00013";
    OS_REQUIRE(XblInitialize(&xbl_init) == XBL_S_OK);
    XblContextHandle xbl_ctx = nullptr;
    OS_REQUIRE(XblContextCreateHandle(user, &xbl_ctx) == XBL_S_OK && xbl_ctx != nullptr);

    // 1A. Default policy (OPENSCARLETT_MOCK_XSTS unset) -> immediate E_GAMEUSER_NO_TOKEN_REQUIRED
    {
        ScopedEnvVar unset_mock("OPENSCARLETT_MOCK_XSTS", nullptr);
        const auto t0 = std::chrono::steady_clock::now();

        // UTF-8 XUserGetTokenAndSignatureAsync with completion callback on Manual queue
        std::atomic<bool> done_u8{false};
        XAsyncBlock ab_u8{};
        ab_u8.queue = q;
        ab_u8.context = &done_u8;
        ab_u8.callback = &AsyncFlagCb;
        OS_REQUIRE(XUserGetTokenAndSignatureAsync(
                       user, XUserGetTokenAndSignatureOptions_None,
                       "GET", "https://xsts.auth.xboxlive.com/xsts/authorize",
                       0, nullptr, 0, nullptr, &ab_u8) == XGR_S_OK);
        OS_REQUIRE(XTaskQueueDispatch(q, XTaskQueuePort_Completion, 2000u));
        OS_REQUIRE(done_u8.load());
        OS_REQUIRE(XAsyncGetStatus(&ab_u8, true) == kHrGameUserNoTokenRequired);

        size_t sz_u8 = 999u;
        OS_REQUIRE(XUserGetTokenAndSignatureResultSize(&ab_u8, &sz_u8) == kHrGameUserNoTokenRequired);
        OS_REQUIRE(sz_u8 == 0u);
        uint8_t dummy_buf[64]{};
        XUserGetTokenAndSignatureData* ptr_u8 = reinterpret_cast<XUserGetTokenAndSignatureData*>(dummy_buf);
        size_t used_u8 = 999u;
        OS_REQUIRE(XUserGetTokenAndSignatureResult(
                       &ab_u8, sizeof(dummy_buf), dummy_buf, &ptr_u8, &used_u8) ==
                   kHrGameUserNoTokenRequired);
        OS_REQUIRE(ptr_u8 == nullptr && used_u8 == 0u);

        // UTF-16 XUserGetTokenAndSignatureUtf16Async
        XAsyncBlock ab_u16{};
        OS_REQUIRE(XUserGetTokenAndSignatureUtf16Async(
                       user, XUserGetTokenAndSignatureOptions_ForceRefresh,
                       L"POST", L"https://user.auth.xboxlive.com/user/authenticate",
                       0, nullptr, 0, nullptr, &ab_u16) == XGR_S_OK);
        size_t sz_u16 = 999u;
        OS_REQUIRE(XUserGetTokenAndSignatureUtf16ResultSize(&ab_u16, &sz_u16) == kHrGameUserNoTokenRequired);
        OS_REQUIRE(sz_u16 == 0u);
        XUserGetTokenAndSignatureUtf16Data* ptr_u16 = nullptr;
        size_t used_u16 = 999u;
        OS_REQUIRE(XUserGetTokenAndSignatureUtf16Result(
                       &ab_u16, sizeof(dummy_buf), dummy_buf, &ptr_u16, &used_u16) ==
                   kHrGameUserNoTokenRequired);
        OS_REQUIRE(ptr_u16 == nullptr && used_u16 == 0u);

        // XSAPI XblGetErrorCondition maps offline no-token HRESULT to XblErrorCondition_Auth
        OS_REQUIRE(XblGetErrorCondition(kHrGameUserNoTokenRequired) == XblErrorCondition_Auth);

        // XSAPI XblHttpCallPerformAsync to Xbox Live XSTS auth endpoint fails immediately offline
        XblHttpCallHandle auth_call = nullptr;
        OS_REQUIRE(XblHttpCallCreate(
                       xbl_ctx, "POST", "https://xsts.auth.xboxlive.com/xsts/authorize", &auth_call) == XBL_S_OK);
        XAsyncBlock http_ab{};
        OS_REQUIRE(XblHttpCallPerformAsync(auth_call, XblHttpCallResponseBodyType_String, &http_ab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&http_ab, true) == kHrGameUserNoTokenRequired);
        uint32_t http_status = 999u;
        XGameResult net_err = XBL_S_OK;
        uint32_t plat_err = 0u;
        OS_REQUIRE(XblHttpCallGetStatusCode(auth_call, &http_status) == XBL_S_OK && http_status == 0u);
        OS_REQUIRE(XblHttpCallGetNetworkErrorCode(auth_call, &net_err, &plat_err) == XBL_S_OK &&
                   net_err == kHrGameUserNoTokenRequired);
        XblHttpCallCloseHandle(auth_call);

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        OS_REQUIRE(elapsed_ms < 2000);
    }

    // 1B. Explicit "0" / "false" ALSO keeps offline-first default
    {
        ScopedEnvVar false_mock("OPENSCARLETT_MOCK_XSTS", "0");
        XAsyncBlock ab{};
        OS_REQUIRE(XUserGetTokenAndSignatureAsync(
                       user, XUserGetTokenAndSignatureOptions_None,
                       "GET", "https://example.invalid/offline",
                       0, nullptr, 0, nullptr, &ab) == XGR_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == kHrGameUserNoTokenRequired);
    }

    // 1C. Opt-in OPENSCARLETT_MOCK_XSTS=1 restores synthetic XBL3.0 JWT tokens
    {
        ScopedEnvVar enable_mock("OPENSCARLETT_MOCK_XSTS", "1");

        // UTF-8 token
        XAsyncBlock ab_u8{};
        OS_REQUIRE(XUserGetTokenAndSignatureAsync(
                       user, XUserGetTokenAndSignatureOptions_None,
                       "GET", "https://title.mgt.xboxlive.com/titles/default",
                       0, nullptr, 0, nullptr, &ab_u8) == XGR_S_OK);
        size_t sz_u8 = 0u;
        OS_REQUIRE(XUserGetTokenAndSignatureResultSize(&ab_u8, &sz_u8) == XGR_S_OK &&
                   sz_u8 > sizeof(XUserGetTokenAndSignatureData));
        std::vector<uint8_t> buf_u8(sz_u8, 0u);
        XUserGetTokenAndSignatureData* data_u8 = nullptr;
        size_t used_u8 = 0u;
        OS_REQUIRE(XUserGetTokenAndSignatureResult(
                       &ab_u8, buf_u8.size(), buf_u8.data(), &data_u8, &used_u8) == XGR_S_OK);
        OS_REQUIRE(data_u8 != nullptr && data_u8->token != nullptr && data_u8->signature != nullptr);
        OS_REQUIRE(std::strstr(data_u8->token, "XBL3.0 x=") != nullptr);
        OS_REQUIRE(used_u8 == sz_u8);

        // UTF-16 token
        XAsyncBlock ab_u16{};
        OS_REQUIRE(XUserGetTokenAndSignatureUtf16Async(
                       user, XUserGetTokenAndSignatureOptions_None,
                       L"GET", L"https://title.mgt.xboxlive.com/titles/default",
                       0, nullptr, 0, nullptr, &ab_u16) == XGR_S_OK);
        size_t sz_u16 = 0u;
        OS_REQUIRE(XUserGetTokenAndSignatureUtf16ResultSize(&ab_u16, &sz_u16) == XGR_S_OK &&
                   sz_u16 > sizeof(XUserGetTokenAndSignatureUtf16Data));
        std::vector<uint8_t> buf_u16(sz_u16, 0u);
        XUserGetTokenAndSignatureUtf16Data* data_u16 = nullptr;
        size_t used_u16 = 0u;
        OS_REQUIRE(XUserGetTokenAndSignatureUtf16Result(
                       &ab_u16, buf_u16.size(), buf_u16.data(), &data_u16, &used_u16) == XGR_S_OK);
        OS_REQUIRE(data_u16 != nullptr && data_u16->token != nullptr &&
                   std::wcsstr(data_u16->token, L"XBL3.0 x=") != nullptr);

        // XSAPI XblHttpCallPerformAsync to XSTS endpoint succeeds with mock token when opted in
        XblHttpCallHandle auth_call = nullptr;
        OS_REQUIRE(XblHttpCallCreate(
                       xbl_ctx, "POST", "https://xsts.auth.xboxlive.com/xsts/authorize", &auth_call) == XBL_S_OK);
        XAsyncBlock http_ab{};
        OS_REQUIRE(XblHttpCallPerformAsync(auth_call, XblHttpCallResponseBodyType_String, &http_ab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&http_ab, true) == XBL_S_OK);
        uint32_t http_status = 0u;
        const char* body = nullptr;
        OS_REQUIRE(XblHttpCallGetStatusCode(auth_call, &http_status) == XBL_S_OK && http_status == 200u);
        OS_REQUIRE(XblHttpCallGetResponseString(auth_call, &body) == XBL_S_OK &&
                   body != nullptr && std::strstr(body, "XBL3.0 x=") != nullptr);
        XblHttpCallCloseHandle(auth_call);
    }

    XblContextCloseHandle(xbl_ctx);
    XblCleanup();
    XUserCloseHandle(user);
    XTaskQueueTerminate(q, true, nullptr, nullptr);
    XTaskQueueCloseHandle(q);
    fs::remove_all(save_root, ec);
    std::printf("  PASS: Offline-first XSTS default & OPENSCARLETT_MOCK_XSTS=1 opt-in verified!\n");
}

// ===========================================================================
// 2. XTaskQueueRegisterWaiter Re-Arming & Terminated XTaskQueueDispatch Drain
// ===========================================================================
struct WaiterTestCtx {
    std::atomic<int> hits{0};
    std::atomic<int> canceled_hits{0};
#if defined(_WIN32)
    HANDLE ack_event = nullptr;
    HANDLE manual_reset_target = nullptr;
#endif
};

void OPENSCARLETT_GDK_CALL WaiterFiredCb(void* ctx, bool canceled) {
    auto* w = static_cast<WaiterTestCtx*>(ctx);
    if (canceled) {
        w->canceled_hits.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
#if defined(_WIN32)
    if (w->manual_reset_target) {
        ::ResetEvent(w->manual_reset_target);
    }
#endif
    w->hits.fetch_add(1, std::memory_order_acq_rel);
#if defined(_WIN32)
    if (w->ack_event) {
        ::SetEvent(w->ack_event);
    }
#endif
}

struct DrainItemCtx {
    std::atomic<int>* normal_count = nullptr;
    std::atomic<int>* canceled_count = nullptr;
};

void OPENSCARLETT_GDK_CALL DrainCounterCb(void* ctx, bool canceled) {
    auto* d = static_cast<DrainItemCtx*>(ctx);
    if (canceled) {
        d->canceled_count->fetch_add(1, std::memory_order_acq_rel);
    } else {
        d->normal_count->fetch_add(1, std::memory_order_acq_rel);
    }
}

void OPENSCARLETT_GDK_CALL TermFlagCb(void* ctx) {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

void TestXTaskQueueWaiterRearmAndTerminatedDrain() {
    std::printf("[Test 2] XTaskQueueRegisterWaiter re-arming (auto & manual reset) + terminated XTaskQueueDispatch drain...\n");

#if defined(_WIN32)
    // 2A. Auto-reset event signaled 6 consecutive times -> callback fires 6 times, zero lost wakeups
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_ThreadPool,
                       XTaskQueueDispatchMode_ThreadPool,
                       &q) == XGR_S_OK && q != nullptr);

        HANDLE trigger_ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE ack_ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(trigger_ev != nullptr && ack_ev != nullptr);

        WaiterTestCtx wctx{};
        wctx.ack_event = ack_ev;
        XTaskQueueRegistrationToken tok{};
        OS_REQUIRE(XTaskQueueRegisterWaiter(
                       q, XTaskQueuePort_Work, trigger_ev, &wctx, &WaiterFiredCb, &tok) == XGR_S_OK);
        OS_REQUIRE(tok.token != 0ull);

        constexpr int kRounds = 6;
        for (int i = 1; i <= kRounds; ++i) {
            OS_REQUIRE(::SetEvent(trigger_ev) != FALSE);
            OS_REQUIRE(::WaitForSingleObject(ack_ev, 3000u) == WAIT_OBJECT_0);
            OS_REQUIRE(wctx.hits.load(std::memory_order_acquire) == i);
        }

        // Unregister cleanly and verify signaling again no longer fires callback
        XTaskQueueUnregisterWaiter(q, tok);
        OS_REQUIRE(::SetEvent(trigger_ev) != FALSE);
        OS_REQUIRE(::WaitForSingleObject(ack_ev, 80u) == WAIT_TIMEOUT);
        OS_REQUIRE(wctx.hits.load(std::memory_order_acquire) == kRounds);

        ::CloseHandle(ack_ev);
        ::CloseHandle(trigger_ev);
        OS_REQUIRE(XTaskQueueTerminate(q, true, nullptr, nullptr) == XGR_S_OK);
        XTaskQueueCloseHandle(q);
    }

    // 2B. Manual-reset event on Manual dispatch port signaled 5 consecutive times
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Manual,
                       &q) == XGR_S_OK && q != nullptr);

        HANDLE manual_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(manual_ev != nullptr);

        WaiterTestCtx wctx{};
        wctx.manual_reset_target = manual_ev;
        XTaskQueueRegistrationToken tok{};
        OS_REQUIRE(XTaskQueueRegisterWaiter(
                       q, XTaskQueuePort_Completion, manual_ev, &wctx, &WaiterFiredCb, &tok) == XGR_S_OK);

        constexpr int kManualRounds = 5;
        for (int i = 1; i <= kManualRounds; ++i) {
            OS_REQUIRE(::SetEvent(manual_ev) != FALSE);
            OS_REQUIRE(XTaskQueueDispatch(q, XTaskQueuePort_Completion, 3000u));
            OS_REQUIRE(wctx.hits.load(std::memory_order_acquire) == i);
            // Verify manual-reset event did NOT spin-storm extra callbacks!
            OS_REQUIRE(!XTaskQueueDispatch(q, XTaskQueuePort_Completion, 25u));
        }

        XTaskQueueUnregisterWaiter(q, tok);
        ::CloseHandle(manual_ev);
        OS_REQUIRE(XTaskQueueTerminate(q, true, nullptr, nullptr) == XGR_S_OK);
        XTaskQueueCloseHandle(q);
    }
#endif

    // 2C. Terminated queue (wait=false): drains all remaining queued items via XTaskQueueDispatch,
    //     fires terminated callback upon drain, and subsequent XTaskQueueDispatch(..., INFINITE)
    //     returns false immediately!
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Manual,
                       &q) == XGR_S_OK && q != nullptr);

        std::atomic<int> normal_cnt{0};
        std::atomic<int> canceled_cnt{0};
        DrainItemCtx dctx{&normal_cnt, &canceled_cnt};

        // Enqueue 4 callbacks on Completion port and 1 normal dispatch before terminate
        for (int i = 0; i < 4; ++i) {
            OS_REQUIRE(XTaskQueueSubmitCallback(
                           q, XTaskQueuePort_Completion, &dctx, &DrainCounterCb) == XGR_S_OK);
        }
        OS_REQUIRE(XTaskQueueDispatch(q, XTaskQueuePort_Completion, 0u));
        OS_REQUIRE(normal_cnt.load() == 1 && canceled_cnt.load() == 0);

        // Terminate with wait=false: 3 items remain queued on Completion port
        std::atomic<bool> term_done{false};
        OS_REQUIRE(XTaskQueueTerminate(q, false, &term_done, &TermFlagCb) == XGR_S_OK);

        // Further submissions rejected cleanly with XGR_E_ABORT
        OS_REQUIRE(XTaskQueueSubmitCallback(
                       q, XTaskQueuePort_Completion, &dctx, &DrainCounterCb) == XGR_E_ABORT);

        // Drain remaining 3 items with INFINITE timeout; 4th call MUST return false immediately!
        int drained_loop = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (XTaskQueueDispatch(q, XTaskQueuePort_Completion, 0xFFFFFFFFu)) {
            ++drained_loop;
            OS_REQUIRE(drained_loop <= 3);
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        OS_REQUIRE(drained_loop == 3);
        OS_REQUIRE(canceled_cnt.load() == 3);
        OS_REQUIRE(term_done.load());
        OS_REQUIRE(elapsed_ms < 500);

        // Subsequent calls on both ports with INFINITE timeout return false immediately
        OS_REQUIRE(!XTaskQueueDispatch(q, XTaskQueuePort_Completion, 0xFFFFFFFFu));
        OS_REQUIRE(!XTaskQueueDispatch(q, XTaskQueuePort_Work, 0xFFFFFFFFu));

        XTaskQueueCloseHandle(q);
    }

    // 2D. Thread blocked inside XTaskQueueDispatch(..., INFINITE) wakes & returns false upon Terminate
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Manual,
                       &q) == XGR_S_OK && q != nullptr);

        std::atomic<bool> dispatcher_started{false};
        std::atomic<bool> dispatch_ret{true};
        std::thread dispatcher([&]() {
            dispatcher_started.store(true, std::memory_order_release);
            const bool r = XTaskQueueDispatch(q, XTaskQueuePort_Completion, 0xFFFFFFFFu);
            dispatch_ret.store(r, std::memory_order_release);
        });

        while (!dispatcher_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(XTaskQueueTerminate(q, false, nullptr, nullptr) == XGR_S_OK);
        dispatcher.join();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        OS_REQUIRE(!dispatch_ret.load(std::memory_order_acquire));
        OS_REQUIRE(elapsed_ms < 1000);

        XTaskQueueCloseHandle(q);
    }

    std::printf("  PASS: XTaskQueueRegisterWaiter re-arming & terminated XTaskQueueDispatch drain verified!\n");
}

// ===========================================================================
// 3. XGameUiShowMessageDialogAsync Strict Button Validation & XErrorOptions
// ===========================================================================
void TestMessageDialogBoundsAndXErrorOptions() {
    std::printf("[Test 3] XGameUiShowMessageDialogAsync strict button bounds & XErrorOptions bit validation...\n");

    // 3A. Valid 1-button, 2-button, and 3-button combinations succeed
    {
        XAsyncBlock ab1{};
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab1, "T", "Body", "OK", nullptr, nullptr,
                       XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_First) == XGR_S_OK);
        XGameUiMessageDialogButton out1 = XGameUiMessageDialogButton_Third;
        OS_REQUIRE(XGameUiShowMessageDialogResult(&ab1, &out1) == XGR_S_OK &&
                   out1 == XGameUiMessageDialogButton_First);

        XAsyncBlock ab2{};
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab2, "T", "Body", "Yes", "No", nullptr,
                       XGameUiMessageDialogButton_Second, XGameUiMessageDialogButton_First) == XGR_S_OK);
        XGameUiMessageDialogButton out2 = XGameUiMessageDialogButton_First;
        OS_REQUIRE(XGameUiShowMessageDialogResult(&ab2, &out2) == XGR_S_OK &&
                   out2 == XGameUiMessageDialogButton_Second);

        XAsyncBlock ab3{};
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab3, "T", "Body", "Save", "Don't Save", "Cancel",
                       XGameUiMessageDialogButton_Third, XGameUiMessageDialogButton_Second) == XGR_S_OK);
        XGameUiMessageDialogButton out3 = XGameUiMessageDialogButton_First;
        OS_REQUIRE(XGameUiShowMessageDialogResult(&ab3, &out3) == XGR_S_OK &&
                   out3 == XGameUiMessageDialogButton_Third);
    }

    // 3B. Invalid button indices / null labels rejected with XGR_E_INVALIDARG
    {
        XAsyncBlock ab{};
        const auto bad_enum = static_cast<XGameUiMessageDialogButton>(3u);
        const auto huge_enum = static_cast<XGameUiMessageDialogButton>(0xFFFFFFFFu);

        // Out-of-range defaultButton or cancelButton
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "A", "B", "C",
                       bad_enum, XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "A", "B", "C",
                       XGameUiMessageDialogButton_First, huge_enum) == XGR_E_INVALIDARG);

        // Pointing at null or empty button labels
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", nullptr, nullptr, nullptr,
                       XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "", nullptr, nullptr,
                       XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "OK", nullptr, nullptr,
                       XGameUiMessageDialogButton_Second, XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "OK", nullptr, nullptr,
                       XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_Second) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "Yes", "No", nullptr,
                       XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_Third) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
                       &ab, "T", "B", "Yes", "No", "",
                       XGameUiMessageDialogButton_Third, XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);

        // Legacy capital-UI export enforces the exact same validation!
        const auto bad_ui_enum = static_cast<XGameUIMessageDialogButton>(99u);
        OS_REQUIRE(XGameUIShowMessageDialogAsync(
                       &ab, "T", "B", "OK", nullptr, nullptr,
                       XGameUIMessageDialogButton_Second, XGameUIMessageDialogButton_First) == XGR_E_INVALIDARG);
        OS_REQUIRE(XGameUIShowMessageDialogAsync(
                       &ab, "T", "B", "OK", "No", "Cancel",
                       bad_ui_enum, XGameUIMessageDialogButton_First) == XGR_E_INVALIDARG);
    }

    // 3C. XErrorOptions bitmask validation + XErrorGetOptions
    {
#if defined(_WIN32)
        HMODULE xgr = ::GetModuleHandleW(L"xgameruntime.dll");
        OS_REQUIRE(xgr != nullptr);
        using PfnXErrorGetOptions = XGameResult(OPENSCARLETT_GDK_CALL*)(XErrorOptions*, XErrorOptions*);
        auto fn_get_opts = reinterpret_cast<PfnXErrorGetOptions>(
            reinterpret_cast<void*>(::GetProcAddress(xgr, "XErrorGetOptions")));
        OS_REQUIRE(fn_get_opts != nullptr);

        // Null pointer validation
        XErrorOptions dbg_o = XErrorOptions_None;
        XErrorOptions nodbg_o = XErrorOptions_None;
        OS_REQUIRE(fn_get_opts(nullptr, &nodbg_o) == XGR_E_INVALIDARG);
        OS_REQUIRE(fn_get_opts(&dbg_o, nullptr) == XGR_E_INVALIDARG);

        // Valid flag combinations stored verbatim
        const auto valid_dbg = static_cast<XErrorOptions>(
            XErrorOptions_OutputDebugStringOnError | XErrorOptions_DebugBreakOnError);
        const auto valid_nodbg = static_cast<XErrorOptions>(
            XErrorOptions_OutputDebugStringOnError | XErrorOptions_FailFastOnError);
        XErrorSetOptions(valid_dbg, valid_nodbg);
        OS_REQUIRE(fn_get_opts(&dbg_o, &nodbg_o) == XGR_S_OK);
        OS_REQUIRE(dbg_o == valid_dbg && nodbg_o == valid_nodbg);

        // Reserved/unknown bits (~0x07) stripped cleanly
        const auto polluted_dbg = static_cast<XErrorOptions>(
            static_cast<uint32_t>(XErrorOptions_OutputDebugStringOnError) | 0xF8u);
        const auto polluted_nodbg = static_cast<XErrorOptions>(0xDEADBEE0u);
        XErrorSetOptions(polluted_dbg, polluted_nodbg);
        OS_REQUIRE(fn_get_opts(&dbg_o, &nodbg_o) == XGR_S_OK);
        OS_REQUIRE(dbg_o == XErrorOptions_OutputDebugStringOnError);
        OS_REQUIRE(nodbg_o == XErrorOptions_None);

        // Restore default None
        XErrorSetOptions(XErrorOptions_None, XErrorOptions_None);
        OS_REQUIRE(fn_get_opts(&dbg_o, &nodbg_o) == XGR_S_OK);
        OS_REQUIRE(dbg_o == XErrorOptions_None && nodbg_o == XErrorOptions_None);
#endif
    }

    std::printf("  PASS: XGameUiShowMessageDialogAsync bounds & XErrorOptions bitmask validation verified!\n");
}

// ===========================================================================
// 4. Clean-Room UMS Shims (ntdll_ums_xs.dll + kernelx.dll)
// ===========================================================================
#if defined(_WIN32)
struct UmsStartupInfoAbi {
    uint32_t UmsVersion;
    void* CompletionList;
    void(__stdcall* SchedulerProc)(uint32_t Reason, uintptr_t ActivationPayload, void* SchedulerParam);
    void* SchedulerParam;
};

struct UmsApiTable {
    int32_t(__stdcall* CreateList)(void**);
    int32_t(__stdcall* DeleteList)(void*);
    int32_t(__stdcall* GetEvent)(void*, void**);
    int32_t(__stdcall* Dequeue)(void*, uint32_t, void**);
    void*(__stdcall* GetNext)(void*);
    int32_t(__stdcall* CreateCtx)(void**);
    int32_t(__stdcall* DeleteCtx)(void*);
    int32_t(__stdcall* QueryInfo)(void*, uint32_t, void*, uint32_t, uint32_t*);
    int32_t(__stdcall* SetInfo)(void*, uint32_t, void*, uint32_t);
    void*(__stdcall* GetCurrent)(void);
    int32_t(__stdcall* EnterMode)(const void*);
    int32_t(__stdcall* Execute)(void*);
    int32_t(__stdcall* YieldUms)(void*);
    int32_t(__stdcall* BindWorker)(void*, void*, void*);
    int32_t(__stdcall* EnqueueItem)(void*, void*);
};

struct UmsTestHarness {
    UmsApiTable api{};
    void* comp_list = nullptr;
    void* ctx1 = nullptr;
    void* ctx2 = nullptr;
    int worker_steps = 0;
    int startup_calls = 0;
    int yield_calls = 0;
    uint32_t yield_token = 0x5CA01337u;
    void* observed_current_in_worker = nullptr;
    uintptr_t observed_yield_ctx = 0u;
    void* observed_yield_param = nullptr;
};

void __stdcall UmsTestWorkerProc(void* param) {
    auto* h = static_cast<UmsTestHarness*>(param);
    h->observed_current_in_worker = h->api.GetCurrent();
    h->worker_steps = 1;
    OS_REQUIRE(h->api.YieldUms(&h->yield_token) == 1);
    h->worker_steps = 2;
}

UmsTestHarness* g_active_ums_harness = nullptr;

void __stdcall UmsTestSchedulerProcFull(uint32_t reason, uintptr_t payload, void* param) {
    UmsTestHarness* h = g_active_ums_harness;
    OS_REQUIRE(h != nullptr);
    if (reason == kUmsReasonStartup) {
        OS_REQUIRE(param == h);
        ++h->startup_calls;
        OS_REQUIRE(h->api.Execute(h->ctx1) == 1);
        OS_REQUIRE(h->worker_steps == 1);
        OS_REQUIRE(h->yield_calls == 1);
        OS_REQUIRE(h->api.Execute(h->ctx1) == 1);
        OS_REQUIRE(h->worker_steps == 2);

        void* head = nullptr;
        OS_REQUIRE(h->api.Dequeue(h->comp_list, 0u, &head) == 1 && head == h->ctx1);
        OS_REQUIRE(h->api.GetNext(head) == nullptr);

        uint32_t terminated = 0u;
        uint32_t ret_len = 0u;
        OS_REQUIRE(h->api.QueryInfo(h->ctx1, kUmsInfoIsTerminated, &terminated, sizeof(terminated), &ret_len) == 1);
        OS_REQUIRE(terminated == 1u && ret_len == sizeof(uint32_t));
    } else if (reason == kUmsReasonYield) {
        ++h->yield_calls;
        h->observed_yield_ctx = payload;
        h->observed_yield_param = param;
        void* head = nullptr;
        OS_REQUIRE(h->api.Dequeue(h->comp_list, 0u, &head) == 1 && head == h->ctx1);
        OS_REQUIRE(h->api.GetNext(head) == nullptr);
    }
}

void RunUmsApiSuite(HMODULE mod, const char* dll_label) {
    UmsTestHarness h{};
    auto load = [&](const char* sym) -> void* {
        void* p = reinterpret_cast<void*>(::GetProcAddress(mod, sym));
        OS_REQUIRE(p != nullptr);
        return p;
    };
    h.api.CreateList = reinterpret_cast<decltype(h.api.CreateList)>(load("CreateUmsCompletionList"));
    h.api.DeleteList = reinterpret_cast<decltype(h.api.DeleteList)>(load("DeleteUmsCompletionList"));
    h.api.GetEvent = reinterpret_cast<decltype(h.api.GetEvent)>(load("GetUmsCompletionListEvent"));
    h.api.Dequeue = reinterpret_cast<decltype(h.api.Dequeue)>(load("DequeueUmsCompletionListItems"));
    h.api.GetNext = reinterpret_cast<decltype(h.api.GetNext)>(load("GetNextUmsListItem"));
    h.api.CreateCtx = reinterpret_cast<decltype(h.api.CreateCtx)>(load("CreateUmsThreadContext"));
    h.api.DeleteCtx = reinterpret_cast<decltype(h.api.DeleteCtx)>(load("DeleteUmsThreadContext"));
    h.api.QueryInfo = reinterpret_cast<decltype(h.api.QueryInfo)>(load("QueryUmsThreadInformation"));
    h.api.SetInfo = reinterpret_cast<decltype(h.api.SetInfo)>(load("SetUmsThreadInformation"));
    h.api.GetCurrent = reinterpret_cast<decltype(h.api.GetCurrent)>(load("GetCurrentUmsThread"));
    h.api.EnterMode = reinterpret_cast<decltype(h.api.EnterMode)>(load("EnterUmsSchedulingMode"));
    h.api.Execute = reinterpret_cast<decltype(h.api.Execute)>(load("ExecuteUmsThread"));
    h.api.YieldUms = reinterpret_cast<decltype(h.api.YieldUms)>(load("UmsThreadYield"));
    h.api.BindWorker = reinterpret_cast<decltype(h.api.BindWorker)>(load("OpenScarlettUmsBindWorkerProc"));
    h.api.EnqueueItem = reinterpret_cast<decltype(h.api.EnqueueItem)>(load("OpenScarlettUmsEnqueueCompletionItem"));

    // Outside UMS worker: GetCurrentUmsThread == nullptr, UmsThreadYield fails cleanly
    OS_REQUIRE(h.api.GetCurrent() == nullptr);
    OS_REQUIRE(h.api.YieldUms(nullptr) == 0);

    // Create completion list + event + empty non-blocking dequeue
    OS_REQUIRE(h.api.CreateList(&h.comp_list) == 1 && h.comp_list != nullptr);
    void* ev_raw = nullptr;
    OS_REQUIRE(h.api.GetEvent(h.comp_list, &ev_raw) == 1 && ev_raw != nullptr);
    HANDLE ev = static_cast<HANDLE>(ev_raw);
    OS_REQUIRE(::WaitForSingleObject(ev, 0u) == WAIT_TIMEOUT);

    void* empty_head = reinterpret_cast<void*>(0x1);
    OS_REQUIRE(h.api.Dequeue(h.comp_list, 0u, &empty_head) == 1 && empty_head == nullptr);

    // Create 2 UMS thread contexts & verify Set/QueryUmsThreadInformation roundtrips
    OS_REQUIRE(h.api.CreateCtx(&h.ctx1) == 1 && h.ctx1 != nullptr);
    OS_REQUIRE(h.api.CreateCtx(&h.ctx2) == 1 && h.ctx2 != nullptr);

    void* custom_tag = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFEBABEu));
    int32_t prio_in = 2;
    uintptr_t aff_in = 0x3Fu;
    OS_REQUIRE(h.api.SetInfo(h.ctx1, kUmsInfoUserContext, &custom_tag, sizeof(custom_tag)) == 1);
    OS_REQUIRE(h.api.SetInfo(h.ctx1, kUmsInfoPriority, &prio_in, sizeof(prio_in)) == 1);
    OS_REQUIRE(h.api.SetInfo(h.ctx1, kUmsInfoAffinity, &aff_in, sizeof(aff_in)) == 1);
    // Read-only class UmsThreadTeb rejects SetUmsThreadInformation
    void* bogus_teb = nullptr;
    OS_REQUIRE(h.api.SetInfo(h.ctx1, kUmsInfoTeb, &bogus_teb, sizeof(bogus_teb)) == 0);

    void* tag_out = nullptr;
    int32_t prio_out = 0;
    uintptr_t aff_out = 0u;
    void* teb_out = nullptr;
    uint32_t susp_out = 1u;
    uint32_t term_out = 1u;
    uint32_t ret_len = 0u;
    OS_REQUIRE(h.api.QueryInfo(h.ctx1, kUmsInfoUserContext, &tag_out, sizeof(tag_out), &ret_len) == 1 &&
               tag_out == custom_tag && ret_len == sizeof(void*));
    OS_REQUIRE(h.api.QueryInfo(h.ctx1, kUmsInfoPriority, &prio_out, sizeof(prio_out), &ret_len) == 1 &&
               prio_out == 2);
    OS_REQUIRE(h.api.QueryInfo(h.ctx1, kUmsInfoAffinity, &aff_out, sizeof(aff_out), &ret_len) == 1 &&
               aff_out == 0x3Fu);
    OS_REQUIRE(h.api.QueryInfo(h.ctx1, kUmsInfoTeb, &teb_out, sizeof(teb_out), &ret_len) == 1 &&
               teb_out != nullptr);
    OS_REQUIRE(h.api.QueryInfo(h.ctx1, kUmsInfoIsSuspended, &susp_out, sizeof(susp_out), &ret_len) == 1 &&
               susp_out == 0u);
    OS_REQUIRE(h.api.QueryInfo(h.ctx1, kUmsInfoIsTerminated, &term_out, sizeof(term_out), &ret_len) == 1 &&
               term_out == 0u);

    // Multi-item enqueue -> event signaled -> dequeue linked list -> GetNextUmsListItem chain
    OS_REQUIRE(h.api.EnqueueItem(h.comp_list, h.ctx1) == 1);
    OS_REQUIRE(h.api.EnqueueItem(h.comp_list, h.ctx2) == 1);
    OS_REQUIRE(::WaitForSingleObject(ev, 1000u) == WAIT_OBJECT_0);
    // Non-empty completion list cannot be deleted until drained
    OS_REQUIRE(h.api.DeleteList(h.comp_list) == 0);

    void* chain_head = nullptr;
    OS_REQUIRE(h.api.Dequeue(h.comp_list, 0u, &chain_head) == 1 && chain_head == h.ctx1);
    OS_REQUIRE(h.api.GetNext(chain_head) == h.ctx2);
    OS_REQUIRE(h.api.GetNext(h.ctx2) == nullptr);
    OS_REQUIRE(::WaitForSingleObject(ev, 0u) == WAIT_TIMEOUT);

    // Cooperative fiber EnterUmsSchedulingMode + ExecuteUmsThread + UmsThreadYield + resume!
    OS_REQUIRE(h.api.BindWorker(h.ctx1, reinterpret_cast<void*>(&UmsTestWorkerProc), &h) == 1);
    UmsStartupInfoAbi startup{};
    startup.UmsVersion = kUmsVersion;
    startup.CompletionList = h.comp_list;
    startup.SchedulerProc = &UmsTestSchedulerProcFull;
    startup.SchedulerParam = &h;

    g_active_ums_harness = &h;
    OS_REQUIRE(h.api.EnterMode(&startup) == 1);
    g_active_ums_harness = nullptr;

    OS_REQUIRE(h.startup_calls == 1);
    OS_REQUIRE(h.yield_calls == 1);
    OS_REQUIRE(h.worker_steps == 2);
    OS_REQUIRE(h.observed_current_in_worker == h.ctx1);
    OS_REQUIRE(h.observed_yield_ctx == reinterpret_cast<uintptr_t>(h.ctx1));
    OS_REQUIRE(h.observed_yield_param == &h.yield_token);

    // Clean teardown
    OS_REQUIRE(h.api.DeleteCtx(h.ctx1) == 1);
    OS_REQUIRE(h.api.DeleteCtx(h.ctx2) == 1);
    OS_REQUIRE(h.api.DeleteList(h.comp_list) == 1);
    OS_REQUIRE(h.api.DeleteList(h.comp_list) == 0);  // double delete rejected

    std::printf("  PASS: %s UMS completion list, thread context, fiber Execute/Yield/Dequeue verified!\n",
                dll_label);
}
#endif

void TestUmsAndWldpShims() {
    std::printf("[Test 4] Clean-room UMS shims (ntdll_ums_xs + kernelx) & wldp_xs policy query rows...\n");
#if defined(_WIN32)
    const fs::path bin_dir = GetExeDir();

    // 4A. ntdll_ums_xs.dll (portable across MSVC, Clang-CL, MinGW-w64)
    {
        const fs::path dll_path = bin_dir / "ntdll_ums_xs.dll";
        HMODULE mod = ::LoadLibraryW(dll_path.c_str());
        OS_REQUIRE(mod != nullptr);

        RunUmsApiSuite(mod, "ntdll_ums_xs.dll");

        // Also verify ntdll Rtl*Ums* NTSTATUS wrappers
        using PfnRtlCreateList = int32_t(__stdcall*)(void**);
        using PfnRtlDeleteList = int32_t(__stdcall*)(void*);
        using PfnRtlCreateCtx = int32_t(__stdcall*)(void**);
        using PfnRtlDeleteCtx = int32_t(__stdcall*)(void*);
        using PfnRtlSetInfo = int32_t(__stdcall*)(void*, uint32_t, void*, uint32_t);
        using PfnRtlQueryInfo = int32_t(__stdcall*)(void*, uint32_t, void*, uint32_t, uint32_t*);
        using PfnTel = int32_t(__stdcall*)(const char*, uint64_t*, uint64_t*);

        auto fn_rtl_cl = reinterpret_cast<PfnRtlCreateList>(reinterpret_cast<void*>(::GetProcAddress(mod, "RtlCreateUmsCompletionList")));
        auto fn_rtl_dl = reinterpret_cast<PfnRtlDeleteList>(reinterpret_cast<void*>(::GetProcAddress(mod, "RtlDeleteUmsCompletionList")));
        auto fn_rtl_cc = reinterpret_cast<PfnRtlCreateCtx>(reinterpret_cast<void*>(::GetProcAddress(mod, "RtlCreateUmsThreadContext")));
        auto fn_rtl_dc = reinterpret_cast<PfnRtlDeleteCtx>(reinterpret_cast<void*>(::GetProcAddress(mod, "RtlDeleteUmsThreadContext")));
        auto fn_rtl_si = reinterpret_cast<PfnRtlSetInfo>(reinterpret_cast<void*>(::GetProcAddress(mod, "RtlSetUmsThreadInformation")));
        auto fn_rtl_qi = reinterpret_cast<PfnRtlQueryInfo>(reinterpret_cast<void*>(::GetProcAddress(mod, "RtlQueryUmsThreadInformation")));
        auto fn_tel = reinterpret_cast<PfnTel>(reinterpret_cast<void*>(::GetProcAddress(mod, "OpenScarlettUmsGetTelemetryCount")));
        OS_REQUIRE(fn_rtl_cl && fn_rtl_dl && fn_rtl_cc && fn_rtl_dc && fn_rtl_si && fn_rtl_qi && fn_tel);

        void* rl = nullptr;
        void* rc = nullptr;
        OS_REQUIRE(fn_rtl_cl(&rl) == 0 && rl != nullptr);
        OS_REQUIRE(fn_rtl_cc(&rc) == 0 && rc != nullptr);
        int32_t p_in = 7;
        int32_t p_out = 0;
        uint32_t rlen = 0u;
        OS_REQUIRE(fn_rtl_si(rc, kUmsInfoPriority, &p_in, sizeof(p_in)) == 0);
        OS_REQUIRE(fn_rtl_qi(rc, kUmsInfoPriority, &p_out, sizeof(p_out), &rlen) == 0 && p_out == 7);
        OS_REQUIRE(fn_rtl_dc(rc) == 0);
        OS_REQUIRE(fn_rtl_dl(rl) == 0);

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("EnterUmsSchedulingMode", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("UmsThreadYield", &tot, &ok) == 1 && ok >= 1u);

        ::FreeLibrary(mod);
    }

    // 4B. kernelx.dll UMS exports (verified on MSVC / Clang-CL; MinGW uses kernelx_mingw.def)
#  if !defined(OPENSCARLETT_MINGW)
    {
        HMODULE kx = ::GetModuleHandleW(L"kernelx.dll");
        if (!kx) {
            const fs::path kx_path = bin_dir / "kernelx.dll";
            kx = ::LoadLibraryW(kx_path.c_str());
        }
        OS_REQUIRE(kx != nullptr);
        RunUmsApiSuite(kx, "kernelx.dll");
    }
#  endif

    // 4C. wldp_xs.dll (Windows Lockdown Policy watermark / production policy query rows)
    {
        const fs::path wldp_path = bin_dir / "wldp_xs.dll";
        HMODULE mod = ::LoadLibraryW(wldp_path.c_str());
        OS_REQUIRE(mod != nullptr);

        struct OsUnicodeStr {
            uint16_t Length;
            uint16_t MaximumLength;
            const wchar_t* Buffer;
        };
        auto make_us = [](const wchar_t* s) -> OsUnicodeStr {
            const uint16_t bytes = static_cast<uint16_t>(std::wcslen(s) * sizeof(wchar_t));
            return OsUnicodeStr{bytes, static_cast<uint16_t>(bytes + sizeof(wchar_t)), s};
        };

        using PfnDynTrust = int32_t(__stdcall*)(void*, void*, uint32_t);
        using PfnApproved = int32_t(__stdcall*)(const void*, void*, int32_t*, uint32_t);
        using PfnMode = int32_t(__stdcall*)(uint32_t*);
        using PfnRestr = int32_t(__stdcall*)(uint32_t*);
        using PfnPolicy = int32_t(__stdcall*)(const void*, uint32_t*, uint32_t);
        using PfnProd = int32_t(__stdcall*)(int32_t*);
        using PfnSecPol = int32_t(__stdcall*)(const void*, const void*, const void*, uint32_t*, void*, uint32_t*);
        using PfnTel = int32_t(__stdcall*)(const char*, uint64_t*, uint64_t*);

        auto fn_dyn = reinterpret_cast<PfnDynTrust>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpQueryDynamicCodeTrust")));
        auto fn_app = reinterpret_cast<PfnApproved>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpIsClassInApprovedList")));
        auto fn_mode = reinterpret_cast<PfnMode>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpQueryWindowsLockdownMode")));
        auto fn_restr = reinterpret_cast<PfnRestr>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpQueryWindowsLockdownRestriction")));
        auto fn_pol = reinterpret_cast<PfnPolicy>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpGetLockdownPolicy")));
        auto fn_prod = reinterpret_cast<PfnProd>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpIsProductionConfiguration")));
        auto fn_sec = reinterpret_cast<PfnSecPol>(reinterpret_cast<void*>(::GetProcAddress(mod, "WldpQuerySecurityPolicy")));
        auto fn_tel = reinterpret_cast<PfnTel>(reinterpret_cast<void*>(::GetProcAddress(mod, "OpenScarlettWldpGetTelemetryCount")));

        OS_REQUIRE(fn_dyn && fn_app && fn_mode && fn_restr && fn_pol && fn_prod && fn_sec && fn_tel);

        // Dynamic code trust
        uint8_t dummy_img[16]{};
        OS_REQUIRE(fn_dyn(nullptr, nullptr, 0u) == static_cast<int32_t>(0x80070057u));
        OS_REQUIRE(fn_dyn(nullptr, dummy_img, sizeof(dummy_img)) == 0);

        // Approved class list
        const uint8_t fake_clsid[16]{1, 2, 3, 4};
        int32_t approved = 0;
        OS_REQUIRE(fn_app(fake_clsid, nullptr, &approved, 0u) == 0 && approved == 1);

        // Lockdown mode / restriction / policy / production configuration
        uint32_t mode = 99u;
        uint32_t restr = 99u;
        uint32_t state = 0u;
        int32_t is_prod = 0;
        OS_REQUIRE(fn_mode(&mode) == 0 && mode == 0u);  // WLDP_WINDOWS_LOCKDOWN_MODE_UNLOCKED
        OS_REQUIRE(fn_restr(&restr) == 0 && restr == 0u);
        OS_REQUIRE(fn_pol(nullptr, &state, 0u) == 0 && state == 0x80000000u);
        OS_REQUIRE(fn_prod(&is_prod) == 0 && is_prod == 1);

        // Security policy rows: watermark (disabled), production (true), sandbox ("RETAIL")
        const OsUnicodeStr prov = make_us(L"Microsoft");
        const OsUnicodeStr key = make_us(L"XboxOS\\Policy");

        const OsUnicodeStr wm_name = make_us(L"ShowWatermark");
        uint32_t vtype = 99u;
        uint32_t vval = 99u;
        uint32_t vsize = sizeof(vval);
        OS_REQUIRE(fn_sec(&prov, &key, &wm_name, &vtype, &vval, &vsize) == 0);
        OS_REQUIRE(vtype == 0u && vval == 0u && vsize == sizeof(uint32_t));

        const OsUnicodeStr prod_name = make_us(L"ProductionConfiguration");
        vsize = sizeof(vval);
        OS_REQUIRE(fn_sec(&prov, &key, &prod_name, &vtype, &vval, &vsize) == 0);
        OS_REQUIRE(vtype == 0u && vval == 1u);

        const OsUnicodeStr sb_name = make_us(L"Sandbox");
        uint32_t sb_sz = 0u;
        OS_REQUIRE(fn_sec(&prov, &key, &sb_name, &vtype, nullptr, &sb_sz) ==
                   static_cast<int32_t>(0x8007007Au));
        OS_REQUIRE(vtype == 3u && sb_sz >= 14u);
        std::vector<wchar_t> sb_buf(sb_sz / sizeof(wchar_t) + 1u, L'\0');
        OS_REQUIRE(fn_sec(&prov, &key, &sb_name, &vtype, sb_buf.data(), &sb_sz) == 0);
        OS_REQUIRE(std::wcscmp(sb_buf.data(), L"RETAIL") == 0);

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("WldpQuerySecurityPolicy", &tot, &ok) == 1 && ok >= 3u);
        OS_REQUIRE(fn_tel("WldpIsProductionConfiguration", &tot, &ok) == 1 && ok >= 1u);

        ::FreeLibrary(mod);
        std::printf("  PASS: wldp_xs.dll dynamic code trust, lockdown mode, production & watermark rows verified!\n");
    }
#endif
}

}  // namespace

int main() {
    TestOfflineFirstXstsPolicy();
    TestXTaskQueueWaiterRearmAndTerminatedDrain();
    TestMessageDialogBoundsAndXErrorOptions();
    TestUmsAndWldpShims();
    std::printf("[PASS] test_s13_gdk_offline_ntdll (all 4 suites green)\n");
    return 0;
}
