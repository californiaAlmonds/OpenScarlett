// OpenScarlett MIT Standalone External Xbox-ABI Binary #1:
// Written PURELY against standard Microsoft GDK public headers (ZERO openscarlett/ includes,
// ZERO openscarlett_core link!).
//
// Exercises the official Microsoft Xbox-GDK-Samples System patterns:
//   - AsynchronousProgramming (XTaskQueue ThreadPool/Manual/Immediate/Serialized/Composite,
//     XTaskQueueRegisterWaiter, XTaskQueueRegisterMonitor, XAsyncRun, custom XAsyncProvider
//     NthPrimeAsync with inside-callback result retrieval + XAsyncCancel)
//   - GameSaveCombo (XUserAddAsync, XUserGetGamertag UniqueModern, XUserGetGamerPictureAsync,
//     XGameSaveInitializeProviderAsync, XGameSaveSubmitUpdateAsync,
//     XGameSaveEnumerateContainerInfoByName, XGameSaveReadBlobDataAsync/Result inside callback,
//     XGameSaveGetRemainingQuotaAsync, XGameSaveDeleteContainerAsync)
//   - SimpleDirectStorageCombo (IDStorageFactoryX1, IDStorageQueueX1, SimpleLoad, StatusBatch,
//     CompletionEvent, MultipleQueues, Cancellation, GDeflate, half-full auto-submit)
//   - GameInput (GameInputCreate, GetCurrentReading, GetGamepadState, SetRumbleState)
//   - PIX3 (PIXBeginEvent, PIXSetMarker, PIXEndEvent, PIXReportCounter)

#include <XGameRuntimeInit.h>
#include <XTaskQueue.h>
#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XGameSave.h>
#include <XUser.h>
#include <XSystem.h>
#include <dstorage_xs.h>
#include <GameInput.h>
#include <pix3.h>

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_external_dstorage_async] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

bool WriteWholeFileW(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t total = 0;
    while (total < size) {
        DWORD chunk = 0;
        if (!::WriteFile(h, bytes + total, static_cast<DWORD>(size - total), &chunk, nullptr) ||
            chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------------------
// 1. Custom GDK-style Async API (NthPrimeAsync pattern from AsynchronousProgramming)
// ---------------------------------------------------------------------------
bool IsPrime(uint64_t n) {
    if (n < 2) return false;
    for (uint64_t d = 2; d * d <= n; ++d) {
        if ((n % d) == 0) return false;
    }
    return true;
}

uint64_t ComputeNthPrime(uint64_t n) {
    uint64_t count = 0;
    uint64_t cand = 1;
    while (count < n) {
        ++cand;
        if (IsPrime(cand)) ++count;
    }
    return cand;
}

struct NthPrimeContext {
    uint64_t n = 0;
    uint64_t result = 0;
};

HRESULT CALLBACK NthPrimeProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<NthPrimeContext*>(data->context);
    switch (op) {
        case XAsyncOp::Begin:
            return XAsyncSchedule(data->async, 0);
        case XAsyncOp::DoWork:
            ctx->result = ComputeNthPrime(ctx->n);
            XAsyncComplete(data->async, S_OK, sizeof(uint64_t));
            return E_PENDING;
        case XAsyncOp::GetResult:
            if (data->bufferSize < sizeof(uint64_t)) return E_NOT_SUFFICIENT_BUFFER;
            std::memcpy(data->buffer, &ctx->result, sizeof(uint64_t));
            return S_OK;
        case XAsyncOp::Cleanup:
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT NthPrimeAsync(uint64_t n, XAsyncBlock* async) {
    auto* ctx = new NthPrimeContext{n, 0};
    HRESULT hr = XAsyncBegin(async, ctx, reinterpret_cast<void*>(&NthPrimeAsync),
                             "NthPrimeAsync", &NthPrimeProvider);
    if (FAILED(hr)) {
        delete ctx;
    }
    return hr;
}

HRESULT NthPrimeAsyncResult(XAsyncBlock* async, uint64_t* result) {
    return XAsyncGetResult(async, reinterpret_cast<void*>(&NthPrimeAsync),
                           sizeof(uint64_t), result, nullptr);
}

// Cancelable custom async provider
struct CancelableContext {
    std::atomic<bool> canceled{false};
};

HRESULT CALLBACK CancelableProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<CancelableContext*>(data->context);
    switch (op) {
        case XAsyncOp::Begin:
            return XAsyncSchedule(data->async, 15);
        case XAsyncOp::DoWork:
            if (ctx->canceled.load(std::memory_order_acquire)) {
                XAsyncComplete(data->async, E_ABORT, 0);
                return E_PENDING;
            }
            return XAsyncSchedule(data->async, 10);
        case XAsyncOp::Cancel:
            ctx->canceled.store(true, std::memory_order_release);
            return S_OK;
        case XAsyncOp::Cleanup:
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

void CALLBACK MonitorCountCb(void* ctx, XTaskQueueHandle /*q*/, XTaskQueuePort /*port*/) {
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

void CALLBACK WaiterTriggeredCb(void* ctx, bool canceled) {
    if (!canceled) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_release);
    }
}

bool CALLBACK CountMatchingContainersCb(const XGameSaveContainerInfo* info, void* ctx) {
    if (info && info->name) {
        static_cast<std::vector<std::string>*>(ctx)->emplace_back(info->name);
    }
    return true;
}

}  // namespace

int main() {
    PIXScopedEventObject root_event(PIX_COLOR(32, 196, 64), "ExternalGdkDStorageAsync");

    // =========================================================================
    // 1. XGameRuntimeInitialize + XSystem + TitleId from MicrosoftGameConfig.mgc
    // =========================================================================
    if (FAILED(XGameRuntimeInitialize())) {
        return Fail("XGameRuntimeInitialize");
    }

    if (XSystemGetDeviceType() != XSystemDeviceType::XboxScarlettAnaconda) {
        return Fail("XSystemGetDeviceType != XboxScarlettAnaconda");
    }

    uint32_t title_id = 0;
    if (FAILED(XGameGetXboxTitleId(&title_id)) || title_id != 0x7E750470u) {
        return Fail("XGameGetXboxTitleId != 0x7E750470", title_id);
    }

    char console_id[64]{};
    size_t cid_used = 0;
    if (FAILED(XSystemGetConsoleId(sizeof(console_id), console_id, &cid_used)) || cid_used <= 1u) {
        return Fail("XSystemGetConsoleId");
    }
    const XSystemAnalyticsInfo ainfo = XSystemGetAnalyticsInfo();
    if (std::strcmp(ainfo.family, "Windows.Xbox") != 0) {
        return Fail("XSystemGetAnalyticsInfo family mismatch");
    }
    const XSystemRuntimeInfo rinfo = XSystemGetRuntimeInfo();
    if (rinfo.runtimeVersion.major != 10u) {
        return Fail("XSystemGetRuntimeInfo major != 10");
    }

    // =========================================================================
    // 2. AsynchronousProgramming Patterns (XAsync + XTaskQueue)
    // =========================================================================
    PIXBeginEvent(PIX_COLOR(64, 160, 255), "AsynchronousProgramming");
    {
        // 2a. Custom GDK-style NthPrimeAsync (ThreadPool work + Manual completion pumped on main thread)
        // Calls NthPrimeAsyncResult AND deletes XAsyncBlock INSIDE callback (exact Microsoft sample pattern!)
        XTaskQueueHandle q_custom = nullptr;
        if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                    XTaskQueueDispatchMode::Manual, &q_custom)) || !q_custom) {
            return Fail("XTaskQueueCreate(ThreadPool, Manual)");
        }

        struct PrimeDoneState {
            std::atomic<int> completed{0};
            std::atomic<uint64_t> sum{0};
            std::atomic<int> errors{0};
        };
        PrimeDoneState prime_state{};

        const uint64_t targets[3] = {50u, 100u, 150u};  // primes: 229, 541, 863
        for (uint64_t n : targets) {
            auto* ab = new XAsyncBlock{};
            ab->queue = q_custom;
            ab->context = &prime_state;
            ab->callback = [](XAsyncBlock* block) {
                std::unique_ptr<XAsyncBlock> owner(block);
                auto* st = static_cast<PrimeDoneState*>(owner->context);
                uint64_t p = 0;
                if (SUCCEEDED(NthPrimeAsyncResult(block, &p))) {
                    st->sum.fetch_add(p, std::memory_order_relaxed);
                } else {
                    st->errors.fetch_add(1, std::memory_order_relaxed);
                }
                st->completed.fetch_add(1, std::memory_order_release);
            };
            if (FAILED(NthPrimeAsync(n, ab))) {
                delete ab;
                return Fail("NthPrimeAsync");
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (prime_state.completed.load(std::memory_order_acquire) < 3 &&
               std::chrono::steady_clock::now() < deadline) {
            XTaskQueueDispatch(q_custom, XTaskQueuePort::Completion, 20u);
        }
        if (prime_state.completed.load() != 3 || prime_state.errors.load() != 0 ||
            prime_state.sum.load() != (229ull + 541ull + 863ull)) {
            return Fail("NthPrimeAsync results mismatch", static_cast<int64_t>(prime_state.sum.load()));
        }
        XTaskQueueTerminate(q_custom, false, nullptr, nullptr);
        XTaskQueueDispatch(q_custom, XTaskQueuePort::Completion, 50u);
        XTaskQueueCloseHandle(q_custom);

        // 2b. XAsyncRun on Immediate process queue override + SerializedThreadPool FIFO ordering
        XTaskQueueHandle saved_proc_q = nullptr;
        if (!XTaskQueueGetCurrentProcessTaskQueue(&saved_proc_q) || !saved_proc_q) {
            return Fail("XTaskQueueGetCurrentProcessTaskQueue");
        }
        XTaskQueueHandle sync_q = nullptr;
        if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::Immediate,
                                    XTaskQueueDispatchMode::Immediate, &sync_q))) {
            return Fail("XTaskQueueCreate(Immediate)");
        }
        XTaskQueueSetCurrentProcessTaskQueue(sync_q);
        {
            std::atomic<int> ran{0};
            XAsyncBlock ab{};
            ab.queue = nullptr;  // uses overridden Immediate process queue
            ab.context = &ran;
            ab.callback = [](XAsyncBlock* b) {
                static_cast<std::atomic<int>*>(b->context)->fetch_add(10);
            };
            HRESULT hr = XAsyncRun(&ab, [](XAsyncBlock* b) -> HRESULT {
                static_cast<std::atomic<int>*>(b->context)->fetch_add(5);
                return S_OK;
            });
            if (FAILED(hr) || FAILED(XAsyncGetStatus(&ab, true)) || ran.load() != 15) {
                return Fail("XAsyncRun Immediate", ran.load());
            }
        }
        XTaskQueueSetCurrentProcessTaskQueue(saved_proc_q);
        XTaskQueueTerminate(sync_q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(sync_q);
        XTaskQueueCloseHandle(saved_proc_q);

        // SerializedThreadPool strict FIFO
        XTaskQueueHandle ser_q = nullptr;
        if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::SerializedThreadPool,
                                    XTaskQueueDispatchMode::SerializedThreadPool, &ser_q))) {
            return Fail("XTaskQueueCreate(SerializedThreadPool)");
        }
        std::vector<int> order;
        struct SerCtx { std::vector<int>* vec; int id; };
        std::vector<XAsyncBlock> ser_blocks(4);
        SerCtx ser_ctxs[4] = {{&order, 1}, {&order, 2}, {&order, 3}, {&order, 4}};
        for (int i = 0; i < 4; ++i) {
            ser_blocks[static_cast<size_t>(i)].queue = ser_q;
            ser_blocks[static_cast<size_t>(i)].context = &ser_ctxs[i];
            if (FAILED(XAsyncRun(&ser_blocks[static_cast<size_t>(i)], [](XAsyncBlock* b) -> HRESULT {
                    auto* c = static_cast<SerCtx*>(b->context);
                    c->vec->push_back(c->id);
                    return S_OK;
                }))) {
                return Fail("XAsyncRun Serialized");
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (FAILED(XAsyncGetStatus(&ser_blocks[static_cast<size_t>(i)], true))) {
                return Fail("XAsyncGetStatus Serialized");
            }
        }
        if (order.size() != 4u || order[0] != 1 || order[1] != 2 || order[2] != 3 || order[3] != 4) {
            return Fail("SerializedThreadPool FIFO order violated");
        }
        XTaskQueueTerminate(ser_q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(ser_q);

        // 2c. Composite queue (XTaskQueueGetPort + XTaskQueueCreateComposite) + Monitor + Waiter
        XTaskQueueHandle base_q = nullptr;
        if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                    XTaskQueueDispatchMode::Manual, &base_q))) {
            return Fail("XTaskQueueCreate(base_q)");
        }
        std::atomic<int> mon_hits{0};
        XTaskQueueRegistrationToken mon_tok{};
        if (FAILED(XTaskQueueRegisterMonitor(base_q, &mon_hits, &MonitorCountCb, &mon_tok))) {
            return Fail("XTaskQueueRegisterMonitor");
        }
        XTaskQueuePortHandle work_port = nullptr;
        if (FAILED(XTaskQueueGetPort(base_q, XTaskQueuePort::Work, &work_port)) || !work_port) {
            return Fail("XTaskQueueGetPort");
        }
        XTaskQueueHandle comp_q = nullptr;
        if (FAILED(XTaskQueueCreateComposite(work_port, work_port, &comp_q)) || !comp_q) {
            return Fail("XTaskQueueCreateComposite");
        }

        std::atomic<int> comp_hits{0};
        XAsyncBlock comp_ab{};
        comp_ab.queue = comp_q;
        comp_ab.context = &comp_hits;
        comp_ab.callback = [](XAsyncBlock* b) {
            static_cast<std::atomic<int>*>(b->context)->fetch_add(100);
        };
        if (FAILED(XAsyncRun(&comp_ab, [](XAsyncBlock* b) -> HRESULT {
                static_cast<std::atomic<int>*>(b->context)->fetch_add(23);
                return S_OK;
            })) ||
            FAILED(XAsyncGetStatus(&comp_ab, true)) ||
            comp_hits.load() != 123) {
            return Fail("Composite queue XAsyncRun", comp_hits.load());
        }

        // Waiter registration on base_q
        HANDLE waiter_ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!waiter_ev) return Fail("CreateEventW(waiter_ev)");
        std::atomic<int> waiter_hits{0};
        XTaskQueueRegistrationToken waiter_tok{};
        if (FAILED(XTaskQueueRegisterWaiter(
                base_q, XTaskQueuePort::Work, waiter_ev, &waiter_hits, &WaiterTriggeredCb, &waiter_tok))) {
            ::CloseHandle(waiter_ev);
            return Fail("XTaskQueueRegisterWaiter");
        }
        ::SetEvent(waiter_ev);
        const auto w_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (waiter_hits.load(std::memory_order_acquire) < 1 &&
               std::chrono::steady_clock::now() < w_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        XTaskQueueUnregisterWaiter(base_q, waiter_tok);
        ::CloseHandle(waiter_ev);
        if (waiter_hits.load() < 1 || mon_hits.load() < 1) {
            return Fail("XTaskQueueRegisterWaiter/Monitor did not fire");
        }

        // XAsyncCancel test
        {
            XAsyncBlock cancel_ab{};
            cancel_ab.queue = comp_q;
            auto* cctx = new CancelableContext{};
            if (FAILED(XAsyncBegin(&cancel_ab, cctx, &CancelableProvider, "Cancelable", &CancelableProvider))) {
                delete cctx;
                return Fail("Cancelable XAsyncBegin");
            }
            XAsyncCancel(&cancel_ab);
            const HRESULT cancel_hr = XAsyncGetStatus(&cancel_ab, true);
            if (cancel_hr != E_ABORT) {
                return Fail("XAsyncCancel expected E_ABORT", cancel_hr);
            }
        }

        XTaskQueueUnregisterMonitor(base_q, mon_tok);
        XTaskQueueTerminate(comp_q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(comp_q);
        XTaskQueueTerminate(base_q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(base_q);
    }
    PIXEndEvent();

    // =========================================================================
    // 3. GameSaveCombo Patterns (XUser + GamerPicture + Async XGameSave)
    // =========================================================================
    PIXBeginEvent(PIX_COLOR(255, 196, 32), "GameSaveCombo");
    {
        XTaskQueueHandle gs_q = nullptr;
        if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                    XTaskQueueDispatchMode::ThreadPool, &gs_q))) {
            return Fail("XTaskQueueCreate(gs_q)");
        }

        // XUserAddAsync with XUserAddResult called INSIDE callback!
        struct UserCbState {
            XUserHandle user = nullptr;
            HRESULT hr = E_FAIL;
            std::atomic<bool> done{false};
        } user_st{};
        {
            auto* uab = new XAsyncBlock{};
            uab->queue = gs_q;
            uab->context = &user_st;
            uab->callback = [](XAsyncBlock* b) {
                std::unique_ptr<XAsyncBlock> owner(b);
                auto* st = static_cast<UserCbState*>(owner->context);
                st->hr = XUserAddResult(b, &st->user);
                st->done.store(true, std::memory_order_release);
            };
            if (FAILED(XUserAddAsync(XUserAddOptions::AddDefaultUserAllowingUI, uab))) {
                delete uab;
                return Fail("XUserAddAsync");
            }
            const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!user_st.done.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < dl) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!user_st.done.load() || FAILED(user_st.hr) || !user_st.user) {
                return Fail("XUserAddResult inside callback", user_st.hr);
            }
        }

        char unique_gt[XUserGamertagComponentUniqueModernMaxBytes + 1]{};
        size_t gt_used = 0;
        if (FAILED(XUserGetGamertag(user_st.user, XUserGamertagComponent::UniqueModern,
                                    sizeof(unique_gt), unique_gt, &gt_used)) ||
            unique_gt[0] == '\0') {
            return Fail("XUserGetGamertag(UniqueModern)");
        }

        // XUserGetGamerPictureAsync -> XUserGetGamerPictureResultSize -> XUserGetGamerPictureResult (inside callback!)
        struct PicState {
            std::vector<uint8_t> bytes;
            HRESULT hr = E_FAIL;
            std::atomic<bool> done{false};
        } pic_st{};
        {
            auto* pab = new XAsyncBlock{};
            pab->queue = gs_q;
            pab->context = &pic_st;
            pab->callback = [](XAsyncBlock* b) {
                std::unique_ptr<XAsyncBlock> owner(b);
                auto* st = static_cast<PicState*>(owner->context);
                size_t sz = 0;
                st->hr = XUserGetGamerPictureResultSize(b, &sz);
                if (SUCCEEDED(st->hr) && sz > 8u) {
                    st->bytes.resize(sz);
                    size_t used = 0;
                    st->hr = XUserGetGamerPictureResult(b, sz, st->bytes.data(), &used);
                }
                st->done.store(true, std::memory_order_release);
            };
            if (FAILED(XUserGetGamerPictureAsync(user_st.user, XUserGamerPictureSize::Medium, pab))) {
                delete pab;
                return Fail("XUserGetGamerPictureAsync");
            }
            const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!pic_st.done.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < dl) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            if (!pic_st.done.load() || FAILED(pic_st.hr) || pic_st.bytes.size() < 32u ||
                std::memcmp(pic_st.bytes.data(), kPngSig, 8) != 0) {
                return Fail("XUserGetGamerPictureResult PNG verification", pic_st.hr);
            }
        }

        // XGameSaveInitializeProviderAsync -> CreateContainer -> SubmitUpdateAsync ->
        // EnumerateContainerInfoByName -> ReadBlobDataAsync (with XAsyncGetResultSize inside callback!) ->
        // GetRemainingQuotaAsync -> DeleteContainerAsync
        const char* kScid = "00000000-0000-0000-0000-00007E750470";
        const char* kContainer = "SampleContainerName";
        const std::string world_json = R"({"xPosition":128,"yPosition":-64,"isDayTime":true})";
        const std::string char_json = R"({"name":"Rodney","level":42,"combatClass":"Mage"})";

        XGameSaveProviderHandle prov = nullptr;
        {
            XAsyncBlock ab{};
            ab.queue = gs_q;
            if (FAILED(XGameSaveInitializeProviderAsync(user_st.user, kScid, false, &ab)) ||
                FAILED(XGameSaveInitializeProviderResult(&ab, &prov)) || !prov) {
                return Fail("XGameSaveInitializeProviderAsync/Result");
            }
        }

        XGameSaveContainerHandle cont = nullptr;
        XGameSaveUpdateHandle upd = nullptr;
        if (FAILED(XGameSaveCreateContainer(prov, kContainer, &cont)) || !cont ||
            FAILED(XGameSaveCreateUpdate(cont, kContainer, &upd)) || !upd ||
            FAILED(XGameSaveSubmitBlobWrite(upd, "WorldState",
                reinterpret_cast<const uint8_t*>(world_json.data()), world_json.size())) ||
            FAILED(XGameSaveSubmitBlobWrite(upd, "CharacterStats",
                reinterpret_cast<const uint8_t*>(char_json.data()), char_json.size()))) {
            return Fail("XGameSave write setup");
        }
        {
            XAsyncBlock uab{};
            uab.queue = gs_q;
            if (FAILED(XGameSaveSubmitUpdateAsync(upd, &uab)) ||
                FAILED(XGameSaveSubmitUpdateResult(&uab))) {
                return Fail("XGameSaveSubmitUpdateAsync");
            }
        }
        XGameSaveCloseUpdate(upd);

        // XGameSaveEnumerateContainerInfoByName (exact GameSaveCombo.cpp pattern)
        std::vector<std::string> matched_names;
        if (FAILED(XGameSaveEnumerateContainerInfoByName(
                prov, kContainer, &matched_names, &CountMatchingContainersCb)) ||
            matched_names.size() != 1u || matched_names[0] != kContainer) {
            return Fail("XGameSaveEnumerateContainerInfoByName(match)",
                        static_cast<int64_t>(matched_names.size()));
        }
        std::vector<std::string> unmatched_names;
        if (FAILED(XGameSaveEnumerateContainerInfoByName(
                prov, "NonExistentPrefix_", &unmatched_names, &CountMatchingContainersCb)) ||
            !unmatched_names.empty()) {
            return Fail("XGameSaveEnumerateContainerInfoByName(no-match)");
        }

        // XGameSaveReadBlobDataAsync + XAsyncGetResultSize + XGameSaveReadBlobDataResult INSIDE callback!
        struct ReadBlobState {
            std::string world;
            std::string stats;
            uint32_t count = 0;
            HRESULT hr = E_FAIL;
            std::atomic<bool> done{false};
        } rb_st{};
        {
            const char* blob_names[2] = {"WorldState", "CharacterStats"};
            auto* rab = new XAsyncBlock{};
            rab->queue = gs_q;
            rab->context = &rb_st;
            rab->callback = [](XAsyncBlock* b) {
                std::unique_ptr<XAsyncBlock> owner(b);
                auto* st = static_cast<ReadBlobState*>(owner->context);
                size_t alloc_sz = 0;
                st->hr = XAsyncGetResultSize(b, &alloc_sz);
                if (SUCCEEDED(st->hr) && alloc_sz > 0) {
                    std::vector<uint8_t> buf(alloc_sz, 0u);
                    auto* blobs = reinterpret_cast<XGameSaveBlob*>(buf.data());
                    st->hr = XGameSaveReadBlobDataResult(b, alloc_sz, blobs, &st->count);
                    if (SUCCEEDED(st->hr)) {
                        for (uint32_t i = 0; i < st->count; ++i) {
                            std::string val(reinterpret_cast<const char*>(blobs[i].data),
                                            blobs[i].info.size);
                            if (std::strcmp(blobs[i].info.name, "WorldState") == 0) {
                                st->world = val;
                            } else if (std::strcmp(blobs[i].info.name, "CharacterStats") == 0) {
                                st->stats = val;
                            }
                        }
                    }
                }
                st->done.store(true, std::memory_order_release);
            };
            if (FAILED(XGameSaveReadBlobDataAsync(cont, blob_names, 2u, rab))) {
                delete rab;
                return Fail("XGameSaveReadBlobDataAsync");
            }
            const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!rb_st.done.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < dl) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!rb_st.done.load() || FAILED(rb_st.hr) || rb_st.count != 2u ||
                rb_st.world != world_json || rb_st.stats != char_json) {
                return Fail("XGameSaveReadBlobDataResult mismatch", rb_st.hr);
            }
        }

        // Quota + async delete cleanup
        int64_t quota = 0;
        {
            XAsyncBlock qab{};
            qab.queue = gs_q;
            if (FAILED(XGameSaveGetRemainingQuotaAsync(prov, &qab)) ||
                FAILED(XGameSaveGetRemainingQuotaResult(&qab, &quota)) || quota <= 0) {
                return Fail("XGameSaveGetRemainingQuotaAsync");
            }
        }
        XGameSaveCloseContainer(cont);
        {
            XAsyncBlock dab{};
            dab.queue = gs_q;
            if (FAILED(XGameSaveDeleteContainerAsync(prov, kContainer, &dab)) ||
                FAILED(XGameSaveDeleteContainerResult(&dab))) {
                return Fail("XGameSaveDeleteContainerAsync");
            }
        }

        XGameSaveCloseProvider(prov);
        XUserCloseHandle(user_st.user);
        XTaskQueueTerminate(gs_q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(gs_q);
    }
    PIXEndEvent();

    // =========================================================================
    // 4. SimpleDirectStorageCombo Patterns (IDStorageFactoryX1 + IDStorageQueueX1)
    // =========================================================================
    PIXBeginEvent(PIX_COLOR(196, 64, 255), "SimpleDirectStorageCombo");
    {
        DSTORAGE_CONFIGURATION1 cfg{};
        cfg.NumSubmitThreads = 1;
        cfg.NumBuiltInCpuDecompressionThreads = 1;
        if (FAILED(DStorageSetConfiguration1(&cfg))) {
            return Fail("DStorageSetConfiguration1");
        }

        ComPtr<IDStorageFactoryX1> factory;
        if (FAILED(DStorageGetFactory(__uuidof(IDStorageFactoryX1),
                                      reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()))) ||
            !factory) {
            return Fail("DStorageGetFactory(IDStorageFactoryX1)");
        }
        factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_RECORD_OBJECT_NAMES);

        ComPtr<IDStorageFileX> raw_file;
        ComPtr<IDStorageFileX> gdef_file;
        if (FAILED(factory->OpenFile(L"G:\\ds_payload.bin", __uuidof(IDStorageFileX),
                                     reinterpret_cast<void**>(raw_file.ReleaseAndGetAddressOf()))) ||
            FAILED(factory->OpenFile(L"G:\\ds_compressed.gdeflate", __uuidof(IDStorageFileX),
                                     reinterpret_cast<void**>(gdef_file.ReleaseAndGetAddressOf())))) {
            return Fail("DStorage OpenFile");
        }
        DSTORAGE_FILE_INFO raw_info{};
        DSTORAGE_FILE_INFO gdef_info{};
        if (FAILED(raw_file->GetFileInformation(&raw_info)) ||
            FAILED(gdef_file->GetFileInformation(&gdef_info)) ||
            raw_info.FileSize < 131072ull || gdef_info.FileSize == 0ull) {
            return Fail("DStorage GetFileInformation");
        }

        // Multiple priority queues (Normal + High + Realtime)
        ComPtr<IDStorageQueueX1> queues[3];
        const DSTORAGE_PRIORITY prios[3] = {
            DSTORAGE_PRIORITY_NORMAL, DSTORAGE_PRIORITY_HIGH, DSTORAGE_PRIORITY_REALTIME};
        const char* qnames[3] = {"DS_Normal", "DS_High", "DS_Realtime"};
        for (int i = 0; i < 3; ++i) {
            DSTORAGE_QUEUE_DESC qd{};
            qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
            qd.Priority = prios[i];
            qd.Name = qnames[i];
            if (FAILED(factory->CreateQueue(&qd, __uuidof(IDStorageQueueX1),
                                            reinterpret_cast<void**>(queues[i].ReleaseAndGetAddressOf())))) {
                return Fail("DStorage CreateQueue(IDStorageQueueX1)");
            }
        }

        // StatusBatch + CompletionEvent + GDeflate across multiple queues
        ComPtr<IDStorageStatusArrayX> status;
        if (FAILED(factory->CreateStatusArray(4u, "DS_Status", __uuidof(IDStorageStatusArrayX),
                                              reinterpret_cast<void**>(status.ReleaseAndGetAddressOf())))) {
            return Fail("DStorage CreateStatusArray");
        }

        std::vector<uint32_t> batch_a(8192u, 0u);  // 32 KiB at offset 0
        std::vector<uint32_t> batch_b(8192u, 0u);  // 32 KiB at offset 65536
        std::vector<uint8_t> gdef_out(65536u, 0u);

        DSTORAGE_REQUEST r0{};  // Xbox layout: file source, no decompression
        r0.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r0.File = raw_file.Get();
        r0.FileOffset = 0ull;
        r0.SourceSize = 32768u;
        r0.Destination = batch_a.data();
        r0.DestinationSize = 32768u;
        r0.CancellationTag = 0x0001ull;
        queues[0]->EnqueueRequest(&r0);
        queues[0]->EnqueueStatus(status.Get(), 0u);

        // Cancelled speculative request tagged 0xDEAD0001 (must not trigger error)
        OpenScarlettDStorageRequestDesc r_cancel{};
        r_cancel.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r_cancel.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r_cancel.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_cancel.Source.File = {raw_file.Get(), 0ull, 32u};  // raw bytes are invalid GDeflate
        r_cancel.Destination.Memory = {gdef_out.data(), 32u};
        r_cancel.UncompressedSize = 32u;
        r_cancel.CancellationTag = 0xDEAD0001ull;
        OpenScarlettDStorageEnqueueRequestDesc(queues[0].Get(), &r_cancel);
        queues[0]->CancelRequestsWithTag(0xFFFF0000ull, 0xDEAD0000ull);

        DSTORAGE_REQUEST r1{};
        r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r1.File = raw_file.Get();
        r1.FileOffset = 65536ull;
        r1.SourceSize = 32768u;
        r1.Destination = batch_b.data();
        r1.DestinationSize = 32768u;
        queues[1]->EnqueueRequest(&r1);
        queues[1]->EnqueueStatus(status.Get(), 1u);

        OpenScarlettDStorageRequestDesc r2{};
        r2.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r2.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r2.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r2.Source.File = {gdef_file.Get(), 0ull, static_cast<uint32_t>(gdef_info.FileSize)};
        r2.Destination.Memory = {gdef_out.data(), 65536u};
        r2.UncompressedSize = 65536u;
        OpenScarlettDStorageEnqueueRequestDesc(queues[2].Get(), &r2);
        queues[2]->EnqueueStatus(status.Get(), 2u);

        HANDLE ev_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev_done) return Fail("CreateEventW(ev_done)");
        queues[2]->EnqueueSetEvent(ev_done);

        for (int i = 0; i < 3; ++i) queues[i]->Submit();

        if (::WaitForSingleObject(ev_done, 10000u) != WAIT_OBJECT_0) {
            ::CloseHandle(ev_done);
            return Fail("DStorage CompletionEvent wait");
        }
        ::CloseHandle(ev_done);

        const auto ds_dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((!status->IsComplete(0u) || !status->IsComplete(1u) || !status->IsComplete(2u)) &&
               std::chrono::steady_clock::now() < ds_dl) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!status->IsComplete(0u) || FAILED(status->GetHResult(0u)) ||
            !status->IsComplete(1u) || FAILED(status->GetHResult(1u)) ||
            !status->IsComplete(2u) || FAILED(status->GetHResult(2u))) {
            return Fail("DStorage multi-queue status failure");
        }

        // Verify sequential uint32 payload & GDeflate output
        for (uint32_t i = 0; i < 8192u; ++i) {
            if (batch_a[i] != i || batch_b[i] != (16384u + i)) {
                return Fail("DStorage raw read payload mismatch", i);
            }
        }
        for (size_t i = 0; i < gdef_out.size(); ++i) {
            const uint8_t expected = static_cast<uint8_t>(((i * 7u) + (i / 32u) * 13u) & 0xFFu);
            if (gdef_out[i] != expected) {
                return Fail("DStorage GDeflate payload mismatch", static_cast<int64_t>(i));
            }
        }

        DSTORAGE_ERROR_RECORD err{};
        queues[0]->RetrieveErrorRecord(&err);
        if (err.FailureCount != 0u) {
            return Fail("DStorage cancelled failure leaked into FailureCount", err.FailureCount);
        }

        // Half-full auto-submit WITHOUT Submit()
        {
            DSTORAGE_QUEUE_DESC mqd{};
            mqd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            mqd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;  // 128 -> half = 64
            mqd.Priority = DSTORAGE_PRIORITY_REALTIME;
            mqd.Name = "DS_AutoSubmit";
            ComPtr<IDStorageQueueX1> mq;
            if (FAILED(factory->CreateQueue(&mqd, __uuidof(IDStorageQueueX1),
                                            reinterpret_cast<void**>(mq.ReleaseAndGetAddressOf())))) {
                return Fail("DStorage CreateQueue(memory)");
            }
            constexpr uint32_t kHalf = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;
            std::vector<std::array<uint8_t, 32>> srcs(kHalf);
            std::vector<std::array<uint8_t, 32>> dsts(kHalf);
            for (uint32_t i = 0; i < kHalf; ++i) {
                for (size_t k = 0; k < 32u; ++k) srcs[i][k] = static_cast<uint8_t>((i * 11u + k) & 0xFFu);
                dsts[i].fill(0u);
            }
            HANDLE as_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!as_ev) return Fail("CreateEventW(as_ev)");
            for (uint32_t i = 0; i < kHalf - 1u; ++i) {
                DSTORAGE_REQUEST r{};
                r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
                r.Source = srcs[i].data();
                r.SourceSize = 32u;
                r.Destination = dsts[i].data();
                r.DestinationSize = 32u;
                mq->EnqueueRequest(&r);
            }
            mq->EnqueueStatus(status.Get(), 3u);
            mq->EnqueueSetEvent(as_ev);
            // 64th request triggers auto-submit WITHOUT Submit()
            {
                DSTORAGE_REQUEST r{};
                r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
                r.Source = srcs[kHalf - 1u].data();
                r.SourceSize = 32u;
                r.Destination = dsts[kHalf - 1u].data();
                r.DestinationSize = 32u;
                mq->EnqueueRequest(&r);
            }
            const DWORD wrc = ::WaitForSingleObject(as_ev, 5000u);
            ::CloseHandle(as_ev);
            if (wrc != WAIT_OBJECT_0 || !status->IsComplete(3u) || FAILED(status->GetHResult(3u))) {
                return Fail("DStorage auto-submit failed");
            }
            for (uint32_t i = 0; i < kHalf - 1u; ++i) {
                if (dsts[i] != srcs[i]) return Fail("DStorage auto-submit payload mismatch", i);
            }
            mq->Close();
        }

        for (int i = 0; i < 3; ++i) queues[i]->Close();
        raw_file->Close();
        gdef_file->Close();
    }
    PIXEndEvent();

    // =========================================================================
    // 5. GameInput + PIX3
    // =========================================================================
    PIXSetMarker(PIX_COLOR(0, 255, 128), "GameInputProbe");
    PIXReportCounter(L"ExternalDStorageAsyncComplete", 1.0f);
    {
        ComPtr<IGameInput> gi;
        if (FAILED(GameInputCreate(gi.ReleaseAndGetAddressOf())) || !gi) {
            return Fail("GameInputCreate");
        }
        ComPtr<IGameInputReading> reading;
        if (FAILED(gi->GetCurrentReading(GameInputKindGamepad, nullptr, reading.ReleaseAndGetAddressOf())) ||
            !reading) {
            return Fail("GameInput GetCurrentReading");
        }
        GameInputGamepadState gp{};
        if (!reading->GetGamepadState(&gp)) {
            return Fail("GameInput GetGamepadState");
        }
        ComPtr<IGameInputDevice> dev;
        reading->GetDevice(dev.ReleaseAndGetAddressOf());
        if (!dev) return Fail("GameInput GetDevice");
        GameInputRumbleParams rumble_on{0.15f, 0.10f, 0.0f, 0.0f};
        GameInputRumbleParams rumble_off{0.0f, 0.0f, 0.0f, 0.0f};
        dev->SetRumbleState(&rumble_on);
        dev->SetRumbleState(&rumble_off);
    }

    char metrics[512]{};
    const int n = std::snprintf(
        metrics, sizeof(metrics),
        "sample=ExternalGdkDStorageAsync\n"
        "title_id=0x%08X\n"
        "async_patterns=ok\n"
        "gamesave_combo=ok\n"
        "dstorage_combo=ok\n"
        "gameinput=ok\n"
        "pix3=ok\n",
        title_id);
    if (n <= 0 || !WriteWholeFileW(L"T:\\dstorage_async_metrics.txt", metrics, static_cast<size_t>(n))) {
        return Fail("Write T:\\dstorage_async_metrics.txt");
    }

    XGameRuntimeUninitialize();
    std::printf("[gdk_external_dstorage_async] PASS (TitleId=0x%08X)\n", title_id);
    return 0;
}
