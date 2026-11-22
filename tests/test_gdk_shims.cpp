#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/xsapi/xsapi_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Helpers for XMem hook test
// ---------------------------------------------------------------------------
std::atomic<int> g_custom_alloc_calls{0};
std::atomic<int> g_custom_free_calls{0};

void* OPENSCARLETT_GDK_CALL CustomXMemAlloc(size_t size, uint64_t attributes) {
    g_custom_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    return XMemAllocDefault(size, attributes);
}

void OPENSCARLETT_GDK_CALL CustomXMemFree(void* ptr, uint64_t attributes) {
    g_custom_free_calls.fetch_add(1, std::memory_order_relaxed);
    XMemFreeDefault(ptr, attributes);
}

// ---------------------------------------------------------------------------
// Helpers for XTaskQueue tests
// ---------------------------------------------------------------------------
struct CounterCtx {
    std::atomic<int> count{0};
    std::atomic<int> canceled_count{0};
};

void OPENSCARLETT_GDK_CALL IncrementCb(void* ctx, bool canceled) {
    auto* c = static_cast<CounterCtx*>(ctx);
    if (canceled) {
        c->canceled_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        c->count.fetch_add(1, std::memory_order_relaxed);
    }
}

struct OrderCtx {
    std::mutex mutex;
    std::vector<int> values;
};

struct OrderItem {
    OrderCtx* owner;
    int value;
};

void OPENSCARLETT_GDK_CALL RecordOrderCb(void* ctx, bool canceled) {
    auto* item = static_cast<OrderItem*>(ctx);
    if (!canceled) {
        std::lock_guard<std::mutex> lk(item->owner->mutex);
        item->owner->values.push_back(item->value);
    }
}

void OPENSCARLETT_GDK_CALL MonitorCb(void* ctx, XTaskQueueHandle /*q*/, XTaskQueuePort /*p*/) {
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

void OPENSCARLETT_GDK_CALL TerminatedCb(void* ctx) {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Custom XAsync provider test
// ---------------------------------------------------------------------------
const char kCustomAsyncId[] = "TestCustomAsync";

struct CustomAsyncData {
    uint32_t input = 0;
    uint32_t output = 0;
};

XGameResult OPENSCARLETT_GDK_CALL CustomAsyncProvider(
    XAsyncOp op, const XAsyncProviderData* data) {
    auto* st = static_cast<CustomAsyncData*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            st->output = st->input * 3u + 7u;
            XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(uint32_t)) return XGR_E_NOT_SUFFICIENT_BUFFER;
            std::memcpy(data->buffer, &st->output, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete st;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

void OPENSCARLETT_GDK_CALL CustomAsyncDoneCb(XAsyncBlock* block) {
    static_cast<std::atomic<bool>*>(block->context)->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// XGameSave enumeration helpers
// ---------------------------------------------------------------------------
struct EnumContainersCtx {
    std::vector<std::string> names;
    uint32_t last_blob_count = 0;
};

bool OPENSCARLETT_GDK_CALL EnumContainerCb(const XGameSaveContainerInfo* info, void* ctx) {
    auto* c = static_cast<EnumContainersCtx*>(ctx);
    c->names.emplace_back(info->name);
    c->last_blob_count = info->blobCount;
    return true;
}

bool OPENSCARLETT_GDK_CALL EnumBlobCb(const XGameSaveBlobInfo* info, void* ctx) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    names->emplace_back(info->name);
    return true;
}

bool OPENSCARLETT_GDK_CALL EnumChunkCb(
    void* ctx, const XPackageChunkSelector* /*sel*/, XPackageChunkAvailability avail) {
    if (avail == XPackageChunkAvailability_Ready) {
        static_cast<int*>(ctx)[0] += 1;
    }
    return true;
}

// ===========================================================================
// 1. xgameruntime.dll tests
// ===========================================================================
void TestXGameRuntime() {
    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // XSystem checks
    OS_REQUIRE(XSystemGetDeviceType() == XSystemDeviceType_XboxScarlettAnaconda);
    char cid[64]{};
    size_t cid_used = 0;
    OS_REQUIRE(XSystemGetConsoleId(sizeof(cid), cid, &cid_used) == XGR_S_OK);
    OS_REQUIRE(std::strlen(cid) > 0 && cid_used == std::strlen(cid) + 1);

    char sandbox[32]{};
    size_t sb_used = 0;
    OS_REQUIRE(XSystemGetXboxLiveSandboxId(sizeof(sandbox), sandbox, &sb_used) == XGR_S_OK);
    OS_REQUIRE(std::strcmp(sandbox, "RETAIL") == 0);

    const XSystemAnalyticsInfo ainfo = XSystemGetAnalyticsInfo();
    OS_REQUIRE(ainfo.osVersion.major == 10 && ainfo.osVersion.build >= 22000);
    OS_REQUIRE(std::strcmp(ainfo.family, "Windows.Xbox") == 0);

    const XSystemRuntimeInfo rinfo = XSystemGetRuntimeInfo();
    OS_REQUIRE(rinfo.runtimeVersion.major == 10);
    OS_REQUIRE(!XSystemIsHandleValid(nullptr));
    OS_REQUIRE(XSystemIsHandleValid(cid));

    // XMem + custom hooks
    OS_REQUIRE(XMemSetAllocationHooks(&CustomXMemAlloc, &CustomXMemFree) == XGR_S_OK);
    void* mem = XMemAlloc(256, 0);
    OS_REQUIRE(mem != nullptr);
    std::memset(mem, 0x5A, 256);
    XMemFree(mem, 0);
    OS_REQUIRE(g_custom_alloc_calls.load() == 1);
    OS_REQUIRE(g_custom_free_calls.load() == 1);
    OS_REQUIRE(XMemSetAllocationHooks(nullptr, nullptr) == XGR_S_OK);

    // XPackage & XDisplay
    OS_REQUIRE(XPackageIsPackagedProcess());
    char pkg[128]{};
    OS_REQUIRE(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg), pkg) == XGR_S_OK);
    OS_REQUIRE(std::strstr(pkg, "OpenScarlett") != nullptr);
    int ready_chunks = 0;
    OS_REQUIRE(XPackageEnumerateChunkAvailability(pkg, 0, nullptr, &ready_chunks, &EnumChunkCb) == XGR_S_OK);
    OS_REQUIRE(ready_chunks >= 1);

    XDisplayHdrModeInfo hdr_info{};
    (void)XDisplayTryEnableHdrMode(0, &hdr_info);
    OS_REQUIRE(hdr_info.maxToneMapLuminance > 0.0f);
    XDisplayTimeoutDeferralHandle deferral = nullptr;
    OS_REQUIRE(XDisplayTryAcquireTimeoutDeferral(&deferral) == XGR_S_OK && deferral != nullptr);
    XDisplayCloseTimeoutDeferralHandle(deferral);

    // XTaskQueue: ThreadPool + delayed callback
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_ThreadPool,
                       XTaskQueueDispatchMode_ThreadPool, &q) == XGR_S_OK);
        CounterCtx counter{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, &counter, &IncrementCb) == XGR_S_OK);
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(XTaskQueueSubmitDelayedCallback(q, XTaskQueuePort_Work, 30, &counter, &IncrementCb) == XGR_S_OK);

        const auto deadline = t0 + std::chrono::seconds(3);
        while (counter.count.load() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
        OS_REQUIRE(counter.count.load() == 2);
        OS_REQUIRE(elapsed_ms >= 20);

        std::atomic<bool> term_called{false};
        OS_REQUIRE(XTaskQueueTerminate(q, true, &term_called, &TerminatedCb) == XGR_S_OK);
        OS_REQUIRE(term_called.load());
        XTaskQueueCloseHandle(q);
    }

    // XTaskQueue: SerializedThreadPool preserves FIFO ordering
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_SerializedThreadPool,
                       XTaskQueueDispatchMode_Manual, &q) == XGR_S_OK);
        OrderCtx octx{};
        OrderItem items[4] = {{&octx, 1}, {&octx, 2}, {&octx, 3}, {&octx, 4}};
        for (int i = 0; i < 4; ++i) {
            OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, &items[i], &RecordOrderCb) == XGR_S_OK);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard<std::mutex> lk(octx.mutex);
            if (octx.values.size() == 4) break;
        }
        OS_REQUIRE(octx.values.size() == 4);
        OS_REQUIRE(octx.values[0] == 1 && octx.values[1] == 2 &&
                   octx.values[2] == 3 && octx.values[3] == 4);
        XTaskQueueTerminate(q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(q);
    }

    // XTaskQueue: Manual mode + monitor + Immediate mode
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Immediate, &q) == XGR_S_OK);
        std::atomic<int> monitor_hits{0};
        XTaskQueueRegistrationToken tok{};
        OS_REQUIRE(XTaskQueueRegisterMonitor(q, &monitor_hits, &MonitorCb, &tok) == XGR_S_OK);

        CounterCtx manual_c{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, &manual_c, &IncrementCb) == XGR_S_OK);
        OS_REQUIRE(monitor_hits.load() == 1);
        OS_REQUIRE(manual_c.count.load() == 0);
        OS_REQUIRE(XTaskQueueDispatch(q, XTaskQueuePort_Work, 100));
        OS_REQUIRE(manual_c.count.load() == 1);

        // Immediate completion port runs inline synchronously
        CounterCtx imm_c{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Completion, &imm_c, &IncrementCb) == XGR_S_OK);
        OS_REQUIRE(imm_c.count.load() == 1);

        XTaskQueueUnregisterMonitor(q, tok);
        XTaskQueueCloseHandle(q);
    }

    // XAsync custom provider roundtrip
    {
        std::atomic<bool> done{false};
        XAsyncBlock async{};
        async.context = &done;
        async.callback = &CustomAsyncDoneCb;
        auto* ctx = new CustomAsyncData{14u, 0u};
        OS_REQUIRE(XAsyncBegin(&async, ctx, kCustomAsyncId, kCustomAsyncId, &CustomAsyncProvider) == XGR_S_OK);
        OS_REQUIRE(XAsyncSchedule(&async, 0) == XGR_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&async, true) == XGR_S_OK);
        OS_REQUIRE(done.load());
        size_t sz = 0;
        OS_REQUIRE(XAsyncGetResultSize(&async, &sz) == XGR_S_OK && sz == sizeof(uint32_t));
        uint32_t val = 0;
        size_t used = 0;
        OS_REQUIRE(XAsyncGetResult(&async, kCustomAsyncId, sizeof(val), &val, &used) == XGR_S_OK);
        OS_REQUIRE(used == sizeof(uint32_t) && val == 49u);
    }

    // XUser + XGameSave full disk persistence roundtrip on E: drive.
    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        OS_REQUIRE(XUserAddResult(&ab, &user) == XGR_S_OK && user != nullptr);

        uint64_t xuid = 0;
        OS_REQUIRE(XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0);
        XUserLocalId lid{};
        OS_REQUIRE(XUserGetLocalId(user, &lid) == XGR_S_OK && lid.value != 0);

        char gt[64]{};
        size_t gt_used = 0;
        OS_REQUIRE(XUserGetGamertag(user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
        OS_REQUIRE(std::strlen(gt) > 0);

        XUserState st = XUserState_SignedOut;
        OS_REQUIRE(XUserGetState(user, &st) == XGR_S_OK && st == XUserState_SignedIn);
        XUserAgeGroup age = XUserAgeGroup_Unknown;
        OS_REQUIRE(XUserGetAgeGroup(user, &age) == XGR_S_OK && age == XUserAgeGroup_Adult);
        bool has_priv = false;
        XUserPrivilegeDenyReason deny = XUserPrivilegeDenyReason_Unknown;
        OS_REQUIRE(XUserCheckPrivilege(user, XUserPrivilegeOptions_None, XUserPrivilege_CloudSavedGames, &has_priv, &deny) == XGR_S_OK);
        OS_REQUIRE(has_priv && deny == XUserPrivilegeDenyReason_None);
        uint32_t max_u = 0;
        OS_REQUIRE(XUserGetMaxUsers(&max_u) == XGR_S_OK && max_u >= 4);

        XUserHandle dup = nullptr;
        OS_REQUIRE(XUserDuplicateHandle(user, &dup) == XGR_S_OK);
        OS_REQUIRE(XUserCompare(user, dup) == 0);
        XUserCloseHandle(dup);
    }

    // XGameSave write -> close -> reopen -> read back -> delete
    {
        const char* kScid = "00000000-0000-0000-0000-00005ca0cafe";
        const char* kContainer = "slot_01";
        const std::vector<uint8_t> payload_a = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44};
        const std::vector<uint8_t> payload_b = {'O', 'p', 'e', 'n', 'S', 'c', 'a', 'r', 'l', 'e', 't', 't'};

        // Async init provider
        XAsyncBlock save_init_ab{};
        OS_REQUIRE(XGameSaveInitializeProviderAsync(user, kScid, false, &save_init_ab) == XGR_S_OK);
        XGameSaveProviderHandle prov = nullptr;
        OS_REQUIRE(XGameSaveInitializeProviderResult(&save_init_ab, &prov) == XGR_S_OK && prov != nullptr);

        // Clean any leftover container from prior runs
        (void)XGameSaveDeleteContainer(prov, kContainer);

        XGameSaveContainerHandle cont = nullptr;
        OS_REQUIRE(XGameSaveCreateContainer(prov, kContainer, &cont) == XGR_S_OK);
        XGameSaveUpdateHandle upd = nullptr;
        OS_REQUIRE(XGameSaveCreateUpdate(cont, "Main Save Slot", &upd) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitBlobWrite(upd, "header.bin", payload_a.data(), payload_a.size()) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitBlobWrite(upd, "world.dat", payload_b.data(), payload_b.size()) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitUpdate(upd) == XGR_S_OK);
        XGameSaveCloseUpdate(upd);
        XGameSaveCloseContainer(cont);
        XGameSaveCloseProvider(prov);

        // Reopen fresh provider and read back from disk on E: drive!
        XGameSaveProviderHandle prov2 = nullptr;
        OS_REQUIRE(XGameSaveInitializeProvider(user, kScid, false, &prov2) == XGR_S_OK && prov2 != nullptr);

        EnumContainersCtx ectx{};
        OS_REQUIRE(XGameSaveEnumerateContainerInfo(prov2, &ectx, &EnumContainerCb) == XGR_S_OK);
        OS_REQUIRE(!ectx.names.empty() && ectx.names[0] == kContainer && ectx.last_blob_count == 2);

        XGameSaveContainerHandle cont2 = nullptr;
        OS_REQUIRE(XGameSaveCreateContainer(prov2, kContainer, &cont2) == XGR_S_OK);
        std::vector<std::string> bnames;
        OS_REQUIRE(XGameSaveEnumerateBlobInfo(cont2, &bnames, &EnumBlobCb) == XGR_S_OK);
        OS_REQUIRE(bnames.size() == 2);

        const char* req_names[2] = {"header.bin", "world.dat"};
        uint32_t req_count = 2;
        std::vector<uint8_t> read_buf(1024, 0);
        auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
        OS_REQUIRE(XGameSaveReadBlobData(cont2, req_names, &req_count, read_buf.size(), blobs) == XGR_S_OK);
        OS_REQUIRE(req_count == 2);
        OS_REQUIRE(std::strcmp(blobs[0].info.name, "header.bin") == 0);
        OS_REQUIRE(blobs[0].info.size == payload_a.size());
        OS_REQUIRE(std::memcmp(blobs[0].data, payload_a.data(), payload_a.size()) == 0);
        OS_REQUIRE(std::strcmp(blobs[1].info.name, "world.dat") == 0);
        OS_REQUIRE(blobs[1].info.size == payload_b.size());
        OS_REQUIRE(std::memcmp(blobs[1].data, payload_b.data(), payload_b.size()) == 0);

        // Delete one blob and verify XGR_E_GS_BLOB_NOT_FOUND
        XGameSaveUpdateHandle del_upd = nullptr;
        OS_REQUIRE(XGameSaveCreateUpdate(cont2, "Main Save Slot", &del_upd) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitBlobDelete(del_upd, "world.dat") == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitUpdate(del_upd) == XGR_S_OK);
        XGameSaveCloseUpdate(del_upd);

        const char* missing[1] = {"world.dat"};
        uint32_t mcount = 1;
        OS_REQUIRE(XGameSaveReadBlobData(cont2, missing, &mcount, read_buf.size(), blobs) == XGR_E_GS_BLOB_NOT_FOUND);

        int64_t quota = 0;
        OS_REQUIRE(XGameSaveGetRemainingQuota(prov2, &quota) == XGR_S_OK && quota > 0);

        XGameSaveCloseContainer(cont2);
        OS_REQUIRE(XGameSaveDeleteContainer(prov2, kContainer) == XGR_S_OK);

        EnumContainersCtx after_del{};
        OS_REQUIRE(XGameSaveEnumerateContainerInfo(prov2, &after_del, &EnumContainerCb) == XGR_S_OK);
        OS_REQUIRE(after_del.names.empty());

        XGameSaveCloseProvider(prov2);
    }

    XUserCloseHandle(user);
    std::printf("[PASS] xgameruntime.dll (XTaskQueue, XAsync, XUser, XGameSave disk roundtrip, XSystem, XMem, XPackage, XDisplay)\n");
}

// ===========================================================================
// 2. GameInput.dll tests
// ===========================================================================
void TestGameInput() {
    IGameInput* gi = nullptr;
    OS_REQUIRE(GameInputCreate(&gi) == GAMEINPUT_S_OK && gi != nullptr);

    const uint64_t t1 = gi->GetCurrentTimestamp();
    OS_REQUIRE(t1 > 0);

    IGameInputReading* r1 = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindGamepad, nullptr, &r1) == GAMEINPUT_S_OK && r1 != nullptr);
    GameInputGamepadState gp{};
    OS_REQUIRE(r1->GetGamepadState(&gp));
    OS_REQUIRE(r1->GetTimestamp() >= t1);

    IGameInputDevice* dev = nullptr;
    r1->GetDevice(&dev);
    OS_REQUIRE(dev != nullptr);
    const GameInputDeviceInfo* info = dev->GetDeviceInfo();
    OS_REQUIRE(info != nullptr && info->vendorId == 0x045Eu && info->productId == 0x0B12u);
    OS_REQUIRE((dev->GetDeviceStatus() & GameInputDeviceConnected) != 0);

    // Rumble test (set then reset to zero)
    GameInputRumbleParams rumble{0.25f, 0.15f, 0.0f, 0.0f};
    dev->SetRumbleState(&rumble);
    GameInputRumbleParams stop_rumble{0.0f, 0.0f, 0.0f, 0.0f};
    dev->SetRumbleState(&stop_rumble);

    // Second reading + history navigation
    IGameInputReading* r2 = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindGamepad, dev, &r2) == GAMEINPUT_S_OK && r2 != nullptr);
    OS_REQUIRE(r2->GetSequenceNumber(GameInputKindGamepad) > r1->GetSequenceNumber(GameInputKindGamepad));

    IGameInputReading* prev = nullptr;
    OS_REQUIRE(gi->GetPreviousReading(r2, GameInputKindGamepad, dev, &prev) == GAMEINPUT_S_OK && prev != nullptr);
    OS_REQUIRE(prev->GetSequenceNumber(GameInputKindGamepad) == r1->GetSequenceNumber(GameInputKindGamepad));

    IGameInputReading* next = nullptr;
    OS_REQUIRE(gi->GetNextReading(r1, GameInputKindGamepad, dev, &next) == GAMEINPUT_S_OK && next != nullptr);
    OS_REQUIRE(next->GetSequenceNumber(GameInputKindGamepad) == r2->GetSequenceNumber(GameInputKindGamepad));

    // Keyboard state query
    GameInputKeyState keys[8]{};
    const uint32_t kcount = r2->GetKeyState(8, keys);
    OS_REQUIRE(kcount <= 8);

    next->Release();
    prev->Release();
    r2->Release();
    dev->Release();
    r1->Release();
    gi->Release();

    std::printf("[PASS] GameInput.dll (XInput + keyboard fallback, rumble, reading history)\n");
}

// ===========================================================================
// 3. XAudio2_9.dll + AcpHal.dll tests (PCM16 + real XMA2 0x0166 decoding)
// ===========================================================================
void TestXAudio2AndAcpHal() {
    // Generate real XMA2 2048-byte packets from a 440 Hz mono sine wave.
    openscarlett::Xma2Config xma_cfg{};
    xma_cfg.sample_rate = 48000u;
    xma_cfg.channels = 1u;
    constexpr size_t kXmaFrames = 4096u;
    std::vector<float> sine_mono(kXmaFrames);
    for (size_t i = 0; i < kXmaFrames; ++i) {
        sine_mono[i] = 0.6f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }
    std::vector<uint8_t> xma_packets;
    OS_REQUIRE(openscarlett::Xma2EncodePcmToPackets(sine_mono.data(), kXmaFrames, xma_cfg, xma_packets).ok());
    OS_REQUIRE(!xma_packets.empty() && (xma_packets.size() % openscarlett::kXma2PacketSize) == 0);

    // --- Test XAudio2_9.dll ---
    IXAudio2* xa = nullptr;
    OS_REQUIRE(XAudio2Create(&xa, 0, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&master, 2, 48000) == OS_XAUDIO2_S_OK && master != nullptr);

    IXAudio2SubmixVoice* submix = nullptr;
    OS_REQUIRE(xa->CreateSubmixVoice(&submix, 2, 48000) == OS_XAUDIO2_S_OK && submix != nullptr);
    OS_REQUIRE(submix->SetVolume(0.8f) == OS_XAUDIO2_S_OK);

    // A) Pure XMA2 (wFormatTag == 0x0166) source voice decode & mix verification
    OS_XMA2WAVEFORMATEX xma_fmt{};
    xma_fmt.wfx.wFormatTag = OS_WAVE_FORMAT_XMA2;
    xma_fmt.wfx.nChannels = 1;
    xma_fmt.wfx.nSamplesPerSec = 48000;
    xma_fmt.wfx.wBitsPerSample = 16;
    xma_fmt.wfx.cbSize = sizeof(OS_XMA2WAVEFORMATEX) - sizeof(OS_WAVEFORMATEX);
    xma_fmt.NumStreams = 1;
    xma_fmt.SamplesEncoded = static_cast<uint32_t>(kXmaFrames);
    xma_fmt.BytesPerBlock = openscarlett::kXma2PacketSize;
    xma_fmt.BlockCount = static_cast<uint16_t>(xma_packets.size() / openscarlett::kXma2PacketSize);

    OS_XAUDIO2_SEND_DESCRIPTOR send_desc{0, submix};
    OS_XAUDIO2_VOICE_SENDS sends{1, &send_desc};
    IXAudio2SourceVoice* xma_voice = nullptr;
    OS_REQUIRE(xa->CreateSourceVoice(&xma_voice, &xma_fmt.wfx, 0, 2.0f, nullptr, &sends) == OS_XAUDIO2_S_OK);

    OS_XAUDIO2_BUFFER xma_buf{};
    xma_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    xma_buf.AudioBytes = static_cast<uint32_t>(xma_packets.size());
    xma_buf.pAudioData = xma_packets.data();
    OS_REQUIRE(xma_voice->SubmitSourceBuffer(&xma_buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(xma_voice->Start() == OS_XAUDIO2_S_OK);

    // Render 2048 stereo frames from XMA2 voice alone and verify real waveform energy.
    constexpr uint32_t kMixFrames = 2048u;
    std::vector<float> xma_mix(kMixFrames * 2u, 0.0f);
    OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, xma_mix.data(), kMixFrames) == OS_XAUDIO2_S_OK);
    double xma_energy = 0.0;
    float xma_peak = 0.0f;
    for (float s : xma_mix) {
        xma_energy += static_cast<double>(s) * static_cast<double>(s);
        xma_peak = std::max(xma_peak, std::fabs(s));
    }
    OS_REQUIRE(xma_energy > 1.0 && xma_peak > 0.05f && xma_peak <= 1.0f);

    // B) PCM16 stereo voice + XMA2 voice mixed together
    OS_WAVEFORMATEX pcm_fmt{};
    pcm_fmt.wFormatTag = OS_WAVE_FORMAT_PCM;
    pcm_fmt.nChannels = 2;
    pcm_fmt.nSamplesPerSec = 48000;
    pcm_fmt.wBitsPerSample = 16;
    pcm_fmt.nBlockAlign = 4;
    pcm_fmt.nAvgBytesPerSec = 48000 * 4;

    IXAudio2SourceVoice* pcm_voice = nullptr;
    OS_REQUIRE(xa->CreateSourceVoice(&pcm_voice, &pcm_fmt) == OS_XAUDIO2_S_OK);
    std::vector<int16_t> pcm16(kMixFrames * 2u);
    for (uint32_t i = 0; i < kMixFrames; ++i) {
        const float v = 0.4f * std::sin(2.0f * 3.14159265f * 880.0f * static_cast<float>(i) / 48000.0f);
        const int16_t s = static_cast<int16_t>(v * 32767.0f);
        pcm16[i * 2 + 0] = s;
        pcm16[i * 2 + 1] = s;
    }
    OS_XAUDIO2_BUFFER pcm_buf{};
    pcm_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    pcm_buf.AudioBytes = static_cast<uint32_t>(pcm16.size() * sizeof(int16_t));
    pcm_buf.pAudioData = reinterpret_cast<const uint8_t*>(pcm16.data());
    OS_REQUIRE(pcm_voice->SubmitSourceBuffer(&pcm_buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(pcm_voice->SetVolume(0.5f) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(pcm_voice->SetFrequencyRatio(1.0f) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(pcm_voice->Start() == OS_XAUDIO2_S_OK);

    std::vector<float> combined_mix(kMixFrames * 2u, 0.0f);
    OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, combined_mix.data(), kMixFrames) == OS_XAUDIO2_S_OK);
    double comb_energy = 0.0;
    for (float s : combined_mix) {
        comb_energy += static_cast<double>(s) * static_cast<double>(s);
    }
    OS_REQUIRE(comb_energy > 1.0);

    OS_XAUDIO2_VOICE_STATE vstate{};
    pcm_voice->GetState(&vstate);
    OS_REQUIRE(vstate.SamplesPlayed == kMixFrames);

    OS_XAUDIO2_PERFORMANCE_DATA perf{};
    xa->GetPerformanceData(&perf);
    OS_REQUIRE(perf.TotalSourceVoiceCount == 2);

    pcm_voice->DestroyVoice();
    xma_voice->DestroyVoice();
    submix->DestroyVoice();
    master->DestroyVoice();
    xa->Release();

    // --- Test AcpHal.dll ---
    AcpHalShapeContexts req_ctx{16, 8, 4, 4, nullptr};
    AcpHalShapeContexts alloc_ctx{};
    OS_REQUIRE(AcpHalAllocateShapeContexts(&req_ctx, &alloc_ctx) == ACPHAL_S_OK);
    OS_REQUIRE(alloc_ctx.numXmaContexts == 16 && alloc_ctx.internalAllocation != nullptr);

    IAcpHal* acp = nullptr;
    OS_REQUIRE(AcpHalCreate(&acp) == ACPHAL_S_OK && acp != nullptr);

    AcpHalXmaContextDesc xdesc{};
    xdesc.sampleRate = 48000;
    xdesc.channels = 1;
    xdesc.blockSize = openscarlett::kXma2DefaultBlockSize;
    xdesc.subframesPerFrame = 4;
    xdesc.quantBits = 12;
    OS_REQUIRE(acp->ConfigureXmaContext(0, &xdesc) == ACPHAL_S_OK);

    std::vector<float> acp_pcm(8192, 0.0f);
    uint32_t written = 0;
    AcpHalCommand cmd{};
    cmd.type = AcpHalCommand_XmaDecode;
    cmd.contextIndex = 0;
    cmd.inputData = xma_packets.data();
    cmd.inputBytes = static_cast<uint32_t>(xma_packets.size());
    cmd.outputFloatPcm = acp_pcm.data();
    cmd.outputCapacitySamples = static_cast<uint32_t>(acp_pcm.size());
    cmd.outSamplesWritten = &written;
    cmd.xmaConfig = xdesc;

    OS_REQUIRE(acp->SubmitCommand(&cmd) == ACPHAL_S_OK);
    OS_REQUIRE(acp->WaitIdle() == ACPHAL_S_OK);
    OS_REQUIRE(written >= kXmaFrames);

    double acp_energy = 0.0;
    for (uint32_t i = 0; i < written; ++i) {
        acp_energy += static_cast<double>(acp_pcm[i]) * static_cast<double>(acp_pcm[i]);
    }
    OS_REQUIRE(acp_energy > 1.0);

    uint64_t pkts_decoded = 0;
    OS_REQUIRE(acp->GetDecodedPacketCount(0, &pkts_decoded) == ACPHAL_S_OK && pkts_decoded >= 1);

    acp->Release();
    OS_REQUIRE(AcpHalReleaseShapeContexts(&alloc_ctx) == ACPHAL_S_OK);

    std::printf("[PASS] XAudio2_9.dll + AcpHal.dll (PCM16 + real XMA2 0x0166 decode & float32 mix verified)\n");
}

// ===========================================================================
// 4. dstorage_xs.dll tests (Raw file, GDeflate file, BCPack memory)
// ===========================================================================
void TestDirectStorage() {
    DSTORAGE_CONFIGURATION1 cfg{};
    cfg.NumSubmitThreads = 1;
    cfg.NumBuiltInCpuDecompressionThreads = 1;
    OS_REQUIRE(DStorageSetConfiguration1(&cfg) == OS_DSTORAGE_S_OK);

    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    // Payload 1: 16 KiB raw payload
    std::vector<uint8_t> raw_payload(16384);
    for (size_t i = 0; i < raw_payload.size(); ++i) {
        raw_payload[i] = static_cast<uint8_t>((i * 29 + 7) & 0xFFu);
    }

    // Payload 2: 96 KiB GDeflate-compressed payload (multi-tile!)
    std::vector<uint8_t> gdef_src(96 * 1024);
    for (size_t i = 0; i < gdef_src.size(); ++i) {
        gdef_src[i] = static_cast<uint8_t>(((i * 5) + (i / 64) * 19) & 0xFFu);
    }
    std::vector<uint8_t> gdef_compressed;
    OS_REQUIRE(openscarlett::GDeflateCompress(gdef_src.data(), gdef_src.size(), gdef_compressed));
    OS_REQUIRE(!gdef_compressed.empty());

    // Payload 3: BC7 64x64 BCPack-compressed texture in memory
    constexpr uint32_t kTexW = 64, kTexH = 64;
    const size_t bc7_bytes = static_cast<size_t>((kTexW / 4) * (kTexH / 4) * 16);
    std::vector<uint8_t> bc7_src(bc7_bytes);
    for (size_t b = 0; b < bc7_bytes / 16; ++b) {
        bc7_src[b * 16 + 0] = 0x40;
        for (size_t k = 1; k < 16; ++k) {
            bc7_src[b * 16 + k] = static_cast<uint8_t>((b * 7 + k * 13) & 0xFFu);
        }
    }
    std::vector<uint8_t> bcpack_compressed;
    OS_REQUIRE(openscarlett::BcPackCompress(
                   openscarlett::BcFormat::BC7, kTexW, kTexH,
                   bc7_src.data(), bc7_src.size(), bcpack_compressed)
                   .ok());
    OS_REQUIRE(!bcpack_compressed.empty());

    // Write real combined data file under TEMP (on E:\.tmp locally via build.cmd, portable on tester PCs)
    std::error_code ec;
    fs::path tmp_base = fs::temp_directory_path(ec);
    if (ec || tmp_base.empty()) {
        tmp_base = fs::path(OPENSCARLETT_BINARY_DIR);
    }
    const fs::path test_dir = tmp_base / "openscarlett_dstorage_test";
    fs::create_directories(test_dir, ec);
    const fs::path test_file = test_dir / "pack_stream.bin";
    {
        std::ofstream ofs(test_file, std::ios::binary | std::ios::trunc);
        OS_REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(raw_payload.data()),
                  static_cast<std::streamsize>(raw_payload.size()));
        ofs.write(reinterpret_cast<const char*>(gdef_compressed.data()),
                  static_cast<std::streamsize>(gdef_compressed.size()));
        ofs.flush();
        OS_REQUIRE(ofs.good());
    }

    IDStorageFileX* ds_file = nullptr;
    const std::wstring wpath = test_file.wstring();
    OS_REQUIRE(factory->OpenFile(wpath.c_str(), &IID_IDStorageFileX,
                                 reinterpret_cast<void**>(&ds_file)) == OS_DSTORAGE_S_OK);
    DSTORAGE_FILE_INFO finfo{};
    OS_REQUIRE(ds_file->GetFileInformation(&finfo) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(finfo.FileSize == raw_payload.size() + gdef_compressed.size());

    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "test_dstorage_queue";

    IDStorageQueueX* queue = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX,
                                    reinterpret_cast<void**>(&queue)) == OS_DSTORAGE_S_OK);

    IDStorageStatusArrayX* status = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(4, "test_status", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&status)) == OS_DSTORAGE_S_OK);

    std::vector<uint8_t> raw_out(raw_payload.size(), 0);
    std::vector<uint8_t> gdef_out(gdef_src.size(), 0);
    std::vector<uint8_t> bc7_out(bc7_src.size(), 0);

    // Request 1: Uncompressed file read
    DSTORAGE_REQUEST r1{};
    r1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r1.Source.File.Source = ds_file;
    r1.Source.File.Offset = 0;
    r1.Source.File.Size = static_cast<uint32_t>(raw_payload.size());
    r1.Destination.Memory.Buffer = raw_out.data();
    r1.Destination.Memory.Size = static_cast<uint32_t>(raw_out.size());
    r1.UncompressedSize = static_cast<uint32_t>(raw_payload.size());
    queue->EnqueueRequest(&r1);
    queue->EnqueueStatus(status, 0);

    // Request 2: GDeflate file decompress
    DSTORAGE_REQUEST r2{};
    r2.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    r2.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r2.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r2.Source.File.Source = ds_file;
    r2.Source.File.Offset = raw_payload.size();
    r2.Source.File.Size = static_cast<uint32_t>(gdef_compressed.size());
    r2.Destination.Memory.Buffer = gdef_out.data();
    r2.Destination.Memory.Size = static_cast<uint32_t>(gdef_out.size());
    r2.UncompressedSize = static_cast<uint32_t>(gdef_src.size());
    queue->EnqueueRequest(&r2);
    queue->EnqueueStatus(status, 1);

    // Request 3: BCPack memory decompress
    DSTORAGE_REQUEST r3{};
    r3.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_BCPACK;
    r3.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r3.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r3.Source.Memory.Source = bcpack_compressed.data();
    r3.Source.Memory.Size = static_cast<uint32_t>(bcpack_compressed.size());
    r3.Destination.Memory.Buffer = bc7_out.data();
    r3.Destination.Memory.Size = static_cast<uint32_t>(bc7_out.size());
    r3.UncompressedSize = static_cast<uint32_t>(bc7_src.size());
    queue->EnqueueRequest(&r3);
    queue->EnqueueStatus(status, 2);

#if defined(_WIN32)
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OS_REQUIRE(ev != nullptr);
    queue->EnqueueSetEvent(ev);
    queue->Submit();
    OS_REQUIRE(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);
#else
    queue->Submit();
    while (!status->IsComplete(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif

    OS_REQUIRE(status->IsComplete(0) && status->GetHResult(0) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(status->IsComplete(1) && status->GetHResult(1) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(status->IsComplete(2) && status->GetHResult(2) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(raw_out == raw_payload);
    OS_REQUIRE(gdef_out == gdef_src);
    OS_REQUIRE(bc7_out == bc7_src);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    OS_REQUIRE(err.FailureCount == 0);

    queue->Close();
    queue->Release();
    status->Release();
    ds_file->Close();
    ds_file->Release();
    factory->Release();

    fs::remove(test_file, ec);
    std::printf("[PASS] dstorage_xs.dll (async raw file + GDeflate file + BCPack memory verified bit-exact)\n");
}

// ===========================================================================
// 5. Microsoft.Xbox.Services.GDK.C.dll (XSAPI) tests
// ===========================================================================
void TestXsapi() {
    OS_REQUIRE(XblMemSetFunctions(nullptr, nullptr) == XBL_S_OK);

    XblInitArgs init_args{};
    init_args.queue = nullptr;
    init_args.scid = "00000000-0000-0000-0000-00005ca0beef";
    OS_REQUIRE(XblInitialize(&init_args) == XBL_S_OK);

    XAsyncBlock user_ab{};
    OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) == XGR_S_OK);
    XUserHandle user = nullptr;
    OS_REQUIRE(XUserAddResult(&user_ab, &user) == XGR_S_OK && user != nullptr);

    XblContextHandle ctx = nullptr;
    OS_REQUIRE(XblContextCreateHandle(user, &ctx) == XBL_S_OK && ctx != nullptr);

    uint64_t xuid = 0;
    OS_REQUIRE(XblContextGetXboxUserId(ctx, &xuid) == XBL_S_OK && xuid != 0);

    // Update achievement to 100%
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblAchievementsUpdateAchievementAsync(ctx, xuid, "ach_first_blood", 100u, &ab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);
    }

    // Query achievements and verify unlocked state
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, xuid, 0, XblAchievementType_All, true,
                       XblAchievementOrderBy_DefaultOrder, 0, 32, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        OS_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK && res != nullptr);
        const XblAchievement* items = nullptr;
        size_t count = 0;
        OS_REQUIRE(XblAchievementsResultGetAchievements(res, &items, &count) == XBL_S_OK);
        OS_REQUIRE(count >= 1);
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(items[i].id, "ach_first_blood") == 0) {
                OS_REQUIRE(items[i].percentComplete == 100u);
                OS_REQUIRE(items[i].progressState == XblAchievementProgressState_Achieved);
                found = true;
            }
        }
        OS_REQUIRE(found);
        XblAchievementsResultCloseHandle(res);
    }

    // Profile query
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblProfileGetUserProfileAsync(ctx, xuid, &ab) == XBL_S_OK);
        XblUserProfile profile{};
        OS_REQUIRE(XblProfileGetUserProfileResult(&ab, &profile) == XBL_S_OK);
        OS_REQUIRE(profile.xboxUserId == xuid);
        OS_REQUIRE(std::strlen(profile.gamertag) > 0);
        OS_REQUIRE(std::atoi(profile.gamerscore) >= 25);
    }

    // Presence set + get
    {
        XblPresenceRichPresenceIds rp{};
        std::snprintf(rp.scid, sizeof(rp.scid), "%s", init_args.scid);
        rp.presenceId = "PlayingCampaign";
        XAsyncBlock set_ab{};
        OS_REQUIRE(XblPresenceSetPresenceAsync(ctx, true, &rp, &set_ab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&set_ab, true) == XBL_S_OK);

        XAsyncBlock get_ab{};
        OS_REQUIRE(XblPresenceGetPresenceAsync(ctx, xuid, &get_ab) == XBL_S_OK);
        XblPresenceRecordHandle prec = nullptr;
        OS_REQUIRE(XblPresenceGetPresenceResult(&get_ab, &prec) == XBL_S_OK && prec != nullptr);
        XblPresenceUserState ustate = XblPresenceUserState_Unknown;
        OS_REQUIRE(XblPresenceRecordGetUserState(prec, &ustate) == XBL_S_OK &&
                   ustate == XblPresenceUserState_Online);
        const XblPresenceDeviceRecord* devs = nullptr;
        size_t dcount = 0;
        OS_REQUIRE(XblPresenceRecordGetDeviceRecords(prec, &devs, &dcount) == XBL_S_OK && dcount == 1);
        OS_REQUIRE(devs[0].deviceType == XblPresenceDeviceType_Scarlett);
        XblPresenceRecordCloseHandle(prec);
    }

    // Social relationships (offline-safe)
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblSocialGetSocialRelationshipsAsync(
                       ctx, xuid, XblSocialRelationshipFilter_All, 0, 16, &ab) == XBL_S_OK);
        XblSocialRelationshipResultHandle sres = nullptr;
        OS_REQUIRE(XblSocialGetSocialRelationshipsResult(&ab, &sres) == XBL_S_OK && sres != nullptr);
        const XblSocialRelationship* rels = nullptr;
        size_t rcount = 99;
        OS_REQUIRE(XblSocialRelationshipResultGetRelationships(sres, &rels, &rcount) == XBL_S_OK);
        OS_REQUIRE(rcount == 0);
        bool has_next = true;
        OS_REQUIRE(XblSocialRelationshipResultHasNext(sres, &has_next) == XBL_S_OK && !has_next);
        XblSocialRelationshipResultCloseHandle(sres);
    }

    // Title-managed stats write + read back
    {
        XblTitleManagedStatistic st{};
        st.statisticName = "HighScore";
        st.statisticType = XblTitleManagedStatType_Number;
        st.numberValue = 98765.0;
        XAsyncBlock wab{};
        OS_REQUIRE(XblTitleManagedStatsWriteAsync(ctx, xuid, &st, 1, &wab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&wab, true) == XBL_S_OK);

        XAsyncBlock rab{};
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticAsync(
                       ctx, xuid, init_args.scid, "HighScore", &rab) == XBL_S_OK);
        size_t rsize = 0;
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticResultSize(&rab, &rsize) == XBL_S_OK && rsize > 0);
        std::vector<uint8_t> buf(rsize);
        XblUserStatisticsResult* res = nullptr;
        size_t used = 0;
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticResult(&rab, buf.size(), buf.data(), &res, &used) == XBL_S_OK);
        OS_REQUIRE(res != nullptr && res->serviceConfigStatisticsCount == 1);
        OS_REQUIRE(res->serviceConfigStatistics[0].statisticsCount == 1);
        OS_REQUIRE(std::strcmp(res->serviceConfigStatistics[0].statistics[0].value, "98765") == 0);
    }

    XblContextCloseHandle(ctx);
    XUserCloseHandle(user);

    XAsyncBlock cleanup_ab{};
    OS_REQUIRE(XblCleanupAsync(&cleanup_ab) == XBL_S_OK);
    OS_REQUIRE(XAsyncGetStatus(&cleanup_ab, true) == XBL_S_OK);

    std::printf("[PASS] Microsoft.Xbox.Services.GDK.C.dll (XSAPI profile, achievements, presence, social, stats)\n");
}

// ===========================================================================
// 6. xgameruntime.dll Thunks Parity & QueryApiImpl (12 GUID tables) tests
// ===========================================================================
void OPENSCARLETT_GDK_CALL DummyUserChangeCb(void*, XUserLocalId, XUserChangeEvent) {}
void OPENSCARLETT_GDK_CALL DummyAudioEpCb(void*, XUserLocalId, XUserDefaultAudioEndpointKind, const wchar_t*) {}
void OPENSCARLETT_GDK_CALL DummyDevAssocCb(void*, const XUserDeviceAssociationChange*) {}
void OPENSCARLETT_GDK_CALL DummyStoreLicenseCb(void*) {}
void OPENSCARLETT_GDK_CALL DummyNetHintCb(void*, const XNetworkingConnectivityHint*) {}
void OPENSCARLETT_GDK_CALL DummyNetPortCb(void*, uint16_t) {}
void OPENSCARLETT_GDK_CALL DummyPkgProgressCb(void*, XPackageInstallationMonitorHandle) {}
void OPENSCARLETT_GDK_CALL DummyPkgInstalledCb(void*, const XPackageDetails*) {}
void OPENSCARLETT_GDK_CALL DummyInviteCb(void*, const char*) {}

bool OPENSCARLETT_GDK_CALL EnumStoreProductCb(const XStoreProduct* prod, void* ctx) {
    auto* ids = static_cast<std::vector<std::string>*>(ctx);
    if (prod && prod->storeId) {
        ids->emplace_back(prod->storeId);
    }
    return true;
}

bool OPENSCARLETT_GDK_CALL EnumPackageDetailsCb(void* ctx, const XPackageDetails* details) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    if (details && details->packageIdentifier) {
        names->emplace_back(details->packageIdentifier);
    }
    return true;
}

bool OPENSCARLETT_GDK_CALL EnumPackageFeatureCb(void* ctx, const XPackageFeature* feat) {
    auto* ids = static_cast<std::vector<std::string>*>(ctx);
    if (feat && feat->id) {
        ids->emplace_back(feat->id);
    }
    return true;
}

bool OPENSCARLETT_GDK_CALL TestErrorCb(XGameResult hr, const char* msg, void* ctx) {
    auto* hits = static_cast<std::atomic<int>*>(ctx);
    if (hr == XGR_E_FAIL && msg && std::strcmp(msg, "synthetic_error") == 0) {
        hits->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void TestXGameRuntimeThunksParity() {
    OS_REQUIRE(InitializeApiImpl() == XGR_S_OK);

    // --- 1. XUser* full parity (direct + QueryApiImpl) ---
    void* raw_user_vt = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_USER, &raw_user_vt) == XGR_S_OK && raw_user_vt != nullptr);
    const auto* user_vt = static_cast<const IXgrUserApiVtable*>(raw_user_vt);

    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        OS_REQUIRE(user_vt->XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        OS_REQUIRE(user_vt->XUserAddResult(&ab, &user) == XGR_S_OK && user != nullptr);

        bool is_guest = true;
        OS_REQUIRE(XUserGetIsGuest(user, &is_guest) == XGR_S_OK && !is_guest);

        uint64_t xuid = 0;
        XUserLocalId lid{};
        OS_REQUIRE(XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0);
        OS_REQUIRE(XUserGetLocalId(user, &lid) == XGR_S_OK && lid.value != 0);

        XUserHandle found_by_id = nullptr;
        OS_REQUIRE(user_vt->XUserFindUserById(xuid, &found_by_id) == XGR_S_OK && found_by_id != nullptr);
        OS_REQUIRE(XUserCompare(user, found_by_id) == 0);
        XUserCloseHandle(found_by_id);

        XUserHandle found_by_lid = nullptr;
        OS_REQUIRE(user_vt->XUserFindUserByLocalId(lid, &found_by_lid) == XGR_S_OK && found_by_lid != nullptr);
        XUserCloseHandle(found_by_lid);

        XUserAppLocalDeviceId dev_id{};
        dev_id.value[0] = 0x42;
        XUserHandle found_dev = nullptr;
        OS_REQUIRE(user_vt->XUserFindForDevice(&dev_id, &found_dev) == XGR_S_OK && found_dev != nullptr);
        XUserCloseHandle(found_dev);

        // Token & signature (UTF-8 & UTF-16) — opt in to synthetic XSTS tokens
        {
            struct ScopedMockXstsEnv {
                bool had_prev = false;
                std::string prev;
                ScopedMockXstsEnv() {
#if defined(_WIN32)
                    char buf[256]{};
                    const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_MOCK_XSTS", buf, sizeof(buf));
                    if (n > 0 && n < sizeof(buf)) {
                        had_prev = true;
                        prev.assign(buf, n);
                    }
                    ::SetEnvironmentVariableA("OPENSCARLETT_MOCK_XSTS", "1");
#else
                    const char* v = std::getenv("OPENSCARLETT_MOCK_XSTS");
                    if (v) {
                        had_prev = true;
                        prev = v;
                    }
                    setenv("OPENSCARLETT_MOCK_XSTS", "1", 1);
#endif
                }
                ~ScopedMockXstsEnv() {
#if defined(_WIN32)
                    ::SetEnvironmentVariableA("OPENSCARLETT_MOCK_XSTS", had_prev ? prev.c_str() : nullptr);
#else
                    if (had_prev) setenv("OPENSCARLETT_MOCK_XSTS", prev.c_str(), 1);
                    else unsetenv("OPENSCARLETT_MOCK_XSTS");
#endif
                }
            } mock_xsts_guard;

            // Token & signature (UTF-8)
            {
                XAsyncBlock tab{};
                OS_REQUIRE(user_vt->XUserGetTokenAndSignatureAsync(
                               user, XUserGetTokenAndSignatureOptions_None,
                               "GET", "https://example.invalid/api",
                               0, nullptr, 0, nullptr, &tab) == XGR_S_OK);
                size_t tsz = 0;
                OS_REQUIRE(user_vt->XUserGetTokenAndSignatureResultSize(&tab, &tsz) == XGR_S_OK);
                OS_REQUIRE(tsz > sizeof(XUserGetTokenAndSignatureData));
                std::vector<uint8_t> tbuf(tsz, 0);
                XUserGetTokenAndSignatureData* tdata = nullptr;
                size_t tused = 0;
                OS_REQUIRE(user_vt->XUserGetTokenAndSignatureResult(
                               &tab, tbuf.size(), tbuf.data(), &tdata, &tused) == XGR_S_OK);
                OS_REQUIRE(tdata != nullptr && tdata->token != nullptr && tdata->signature != nullptr);
                OS_REQUIRE(std::strstr(tdata->token, "XBL3.0") != nullptr);
                OS_REQUIRE(tused == tsz);
            }

            // Token & signature (UTF-16)
            {
                XAsyncBlock wab{};
                OS_REQUIRE(XUserGetTokenAndSignatureUtf16Async(
                               user, XUserGetTokenAndSignatureOptions_ForceRefresh,
                               L"POST", L"https://example.invalid/utf16",
                               0, nullptr, 0, nullptr, &wab) == XGR_S_OK);
                size_t wsz = 0;
                OS_REQUIRE(XUserGetTokenAndSignatureUtf16ResultSize(&wab, &wsz) == XGR_S_OK);
                OS_REQUIRE(wsz > sizeof(XUserGetTokenAndSignatureUtf16Data));
                std::vector<uint8_t> wbuf(wsz, 0);
                XUserGetTokenAndSignatureUtf16Data* wdata = nullptr;
                size_t wused = 0;
                OS_REQUIRE(XUserGetTokenAndSignatureUtf16Result(
                               &wab, wbuf.size(), wbuf.data(), &wdata, &wused) == XGR_S_OK);
                OS_REQUIRE(wdata != nullptr && wdata->token != nullptr && wdata->signature != nullptr);
                OS_REQUIRE(wdata->tokenCount > 5 && wdata->signatureCount > 5);
            }
        }

        // Default audio endpoint UTF-16
        wchar_t ep[128]{};
        size_t ep_used = 0;
        OS_REQUIRE(user_vt->XUserGetDefaultAudioEndpointUtf16(
                       lid, XUserDefaultAudioEndpointKind_CommunicationRender,
                       128, ep, &ep_used) == XGR_S_OK);
        OS_REQUIRE(ep_used > 1 && ep[0] == L'{');

        // Sign-out deferral
        XUserSignOutDeferralHandle sdef = nullptr;
        OS_REQUIRE(XUserGetSignOutDeferral(&sdef) == XGR_S_OK && sdef != nullptr);
        XUserCloseSignOutDeferralHandle(sdef);

        // Change / audio / device registrations
        XTaskQueueRegistrationToken tok1{}, tok2{}, tok3{};
        OS_REQUIRE(XUserRegisterForChangeEvent(nullptr, nullptr, &DummyUserChangeCb, &tok1) == XGR_S_OK);
        OS_REQUIRE(tok1.token != 0 && XUserUnregisterForChangeEvent(tok1, true));
        OS_REQUIRE(!XUserUnregisterForChangeEvent(tok1, true));

        OS_REQUIRE(XUserRegisterForDefaultAudioEndpointUtf16Changed(
                       nullptr, nullptr, &DummyAudioEpCb, &tok2) == XGR_S_OK);
        OS_REQUIRE(XUserUnregisterForDefaultAudioEndpointUtf16Changed(tok2, true));

        OS_REQUIRE(XUserRegisterForDeviceAssociationChanged(
                       nullptr, nullptr, &DummyDevAssocCb, &tok3) == XGR_S_OK);
        OS_REQUIRE(XUserUnregisterForDeviceAssociationChanged(tok3, true));

        // Resolve issue & privilege UI asyncs
        {
            XAsyncBlock r1{}, r2{}, r3{};
            OS_REQUIRE(XUserResolveIssueWithUiAsync(user, "https://example.invalid", &r1) == XGR_S_OK);
            OS_REQUIRE(XUserResolveIssueWithUiResult(&r1) == XGR_S_OK);
            OS_REQUIRE(XUserResolveIssueWithUiUtf16Async(user, L"https://example.invalid", &r2) == XGR_S_OK);
            OS_REQUIRE(XUserResolveIssueWithUiUtf16Result(&r2) == XGR_S_OK);
            OS_REQUIRE(user_vt->XUserResolvePrivilegeWithUiAsync(
                           user, XUserPrivilegeOptions_None, XUserPrivilege_Multiplayer, &r3) == XGR_S_OK);
            OS_REQUIRE(user_vt->XUserResolvePrivilegeWithUiResult(&r3) == XGR_S_OK);
        }
    }

    // --- 2. XStore* full parity (direct + QueryApiImpl) ---
    void* raw_store_vt = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_STORE, &raw_store_vt) == XGR_S_OK && raw_store_vt != nullptr);
    const auto* store_vt = static_cast<const IXgrStoreApiVtable*>(raw_store_vt);
    {
        XStoreContextHandle sctx = nullptr;
        OS_REQUIRE(store_vt->XStoreCreateContext(user, &sctx) == XGR_S_OK && sctx != nullptr);

        // Owned game license
        {
            XAsyncBlock ab{};
            OS_REQUIRE(store_vt->XStoreQueryGameLicenseAsync(sctx, &ab) == XGR_S_OK);
            XStoreGameLicense lic{};
            OS_REQUIRE(store_vt->XStoreQueryGameLicenseResult(&ab, &lic) == XGR_S_OK);
            OS_REQUIRE(lic.isActive && !lic.isTrial && std::strlen(lic.skuStoreId) > 0);
        }

        // License changed registration
        XTaskQueueRegistrationToken ltok{};
        OS_REQUIRE(XStoreRegisterGameLicenseChanged(sctx, nullptr, nullptr, &DummyStoreLicenseCb, &ltok) == XGR_S_OK);
        OS_REQUIRE(XStoreUnregisterGameLicenseChanged(sctx, ltok, true));

        // Add-on licenses (0)
        {
            XAsyncBlock ab{};
            OS_REQUIRE(XStoreQueryAddOnLicensesAsync(sctx, &ab) == XGR_S_OK);
            uint32_t cnt = 99u;
            OS_REQUIRE(XStoreQueryAddOnLicensesResultCount(&ab, &cnt) == XGR_S_OK && cnt == 0u);
            OS_REQUIRE(XStoreQueryAddOnLicensesResult(&ab, 0u, nullptr) == XGR_S_OK);
        }

        // Product query for current game + enumeration + pagination
        {
            XAsyncBlock ab{};
            OS_REQUIRE(store_vt->XStoreQueryProductForCurrentGameAsync(sctx, &ab) == XGR_S_OK);
            XStoreProductQueryHandle qh = nullptr;
            OS_REQUIRE(store_vt->XStoreQueryProductForCurrentGameResult(&ab, &qh) == XGR_S_OK && qh != nullptr);
            std::vector<std::string> ids;
            OS_REQUIRE(store_vt->XStoreEnumerateProductsQuery(qh, &ids, &EnumStoreProductCb) == XGR_S_OK);
            OS_REQUIRE(ids.size() == 1 && !ids[0].empty());
            OS_REQUIRE(!store_vt->XStoreProductsQueryHasMorePages(qh));

            XAsyncBlock np{};
            OS_REQUIRE(XStoreProductsQueryNextPageAsync(qh, &np) == XGR_S_OK);
            XStoreProductQueryHandle qh2 = nullptr;
            OS_REQUIRE(XStoreProductsQueryNextPageResult(&np, &qh2) == XGR_S_OK && qh2 != nullptr);
            XStoreCloseProductsQueryHandle(qh2);
            XStoreCloseProductsQueryHandle(qh);
        }

        // Package, associated, entitled, explicit store ID queries
        {
            char cur_pkg[128]{};
            OS_REQUIRE(XPackageGetCurrentProcessPackageIdentifier(sizeof(cur_pkg), cur_pkg) == XGR_S_OK);

            XAsyncBlock ab1{}, ab2{}, ab3{}, ab4{};
            OS_REQUIRE(XStoreQueryProductForPackageAsync(sctx, XStoreProductKind_Game, cur_pkg, &ab1) == XGR_S_OK);
            XStoreProductQueryHandle q1 = nullptr;
            OS_REQUIRE(XStoreQueryProductForPackageResult(&ab1, &q1) == XGR_S_OK && q1 != nullptr);
            XStoreCloseProductsQueryHandle(q1);

            OS_REQUIRE(XStoreQueryAssociatedProductsAsync(sctx, XStoreProductKind_Durable, 10, &ab2) == XGR_S_OK);
            XStoreProductQueryHandle q2 = nullptr;
            OS_REQUIRE(XStoreQueryAssociatedProductsResult(&ab2, &q2) == XGR_S_OK && q2 != nullptr);
            XStoreCloseProductsQueryHandle(q2);

            OS_REQUIRE(XStoreQueryEntitledProductsAsync(sctx, XStoreProductKind_Game, 10, &ab3) == XGR_S_OK);
            XStoreProductQueryHandle q3 = nullptr;
            OS_REQUIRE(XStoreQueryEntitledProductsResult(&ab3, &q3) == XGR_S_OK && q3 != nullptr);
            XStoreCloseProductsQueryHandle(q3);

            const char* sids[] = {"9OS000000001"};
            OS_REQUIRE(XStoreQueryProductsAsync(sctx, XStoreProductKind_Game, sids, 1, nullptr, 0, &ab4) == XGR_S_OK);
            XStoreProductQueryHandle q4 = nullptr;
            OS_REQUIRE(XStoreQueryProductsResult(&ab4, &q4) == XGR_S_OK && q4 != nullptr);
            XStoreCloseProductsQueryHandle(q4);

            // CanAcquire + Acquire license for package & durable + IsLicenseValid + LicenseLost
            XAsyncBlock ca1{}, ca2{}, ac1{}, ac2{};
            OS_REQUIRE(XStoreCanAcquireLicenseForPackageAsync(sctx, cur_pkg, &ca1) == XGR_S_OK);
            XStoreCanAcquireLicenseResult car1{};
            OS_REQUIRE(XStoreCanAcquireLicenseForPackageResult(&ca1, &car1) == XGR_S_OK);
            OS_REQUIRE(car1.status == XStoreCanLicenseStatus_Licensable);

            OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdAsync(sctx, "9OS000000001", &ca2) == XGR_S_OK);
            XStoreCanAcquireLicenseResult car2{};
            OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdResult(&ca2, &car2) == XGR_S_OK);
            OS_REQUIRE(car2.status == XStoreCanLicenseStatus_Licensable);

            OS_REQUIRE(XStoreAcquireLicenseForPackageAsync(sctx, cur_pkg, &ac1) == XGR_S_OK);
            XStoreLicenseHandle l1 = nullptr;
            OS_REQUIRE(XStoreAcquireLicenseForPackageResult(&ac1, &l1) == XGR_S_OK && l1 != nullptr);
            OS_REQUIRE(XStoreIsLicenseValid(l1));
            XTaskQueueRegistrationToken lost_tok{};
            OS_REQUIRE(XStoreRegisterPackageLicenseLost(l1, nullptr, nullptr, &DummyStoreLicenseCb, &lost_tok) == XGR_S_OK);
            OS_REQUIRE(XStoreUnregisterPackageLicenseLost(l1, lost_tok, true));
            XStoreCloseLicenseHandle(l1);
            OS_REQUIRE(!XStoreIsLicenseValid(l1));

            OS_REQUIRE(XStoreAcquireLicenseForDurablesAsync(sctx, "9OS000000001", &ac2) == XGR_S_OK);
            XStoreLicenseHandle l2 = nullptr;
            OS_REQUIRE(XStoreAcquireLicenseForDurablesResult(&ac2, &l2) == XGR_S_OK && l2 != nullptr);
            OS_REQUIRE(XStoreIsLicenseValid(l2));
            XStoreCloseLicenseHandle(l2);
        }

        // Consumables, updates, tokens, package id, availability, UI
        {
            XAsyncBlock cb{}, cf{};
            OS_REQUIRE(XStoreQueryConsumableBalanceRemainingAsync(sctx, "9CON00000001", &cb) == XGR_S_OK);
            XStoreConsumableResult cres{42u};
            OS_REQUIRE(XStoreQueryConsumableBalanceRemainingResult(&cb, &cres) == XGR_S_OK && cres.quantity == 0u);

            XgrGuid trk{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
            OS_REQUIRE(XStoreReportConsumableFulfillmentAsync(sctx, "9CON00000001", 1u, trk, &cf) == XGR_S_OK);
            OS_REQUIRE(XStoreReportConsumableFulfillmentResult(&cf, &cres) == XGR_S_OK && cres.quantity == 0u);

            XAsyncBlock u1{}, u2{}, u3{}, u4{};
            OS_REQUIRE(XStoreQueryGameAndDlcPackageUpdatesAsync(sctx, &u1) == XGR_S_OK);
            uint32_t ucnt = 5u;
            OS_REQUIRE(XStoreQueryGameAndDlcPackageUpdatesResultCount(&u1, &ucnt) == XGR_S_OK && ucnt == 0u);
            OS_REQUIRE(XStoreQueryGameAndDlcPackageUpdatesResult(&u1, 0u, nullptr) == XGR_S_OK);

            OS_REQUIRE(XStoreDownloadPackageUpdatesAsync(sctx, nullptr, 0, &u2) == XGR_S_OK);
            OS_REQUIRE(XStoreDownloadPackageUpdatesResult(&u2) == XGR_S_OK);
            OS_REQUIRE(XStoreDownloadAndInstallPackageUpdatesAsync(sctx, nullptr, 0, &u3) == XGR_S_OK);
            OS_REQUIRE(XStoreDownloadAndInstallPackageUpdatesResult(&u3) == XGR_S_OK);
            OS_REQUIRE(XStoreDownloadAndInstallPackagesAsync(sctx, nullptr, 0, &u4) == XGR_S_OK);
            uint32_t dcnt = 5u;
            OS_REQUIRE(XStoreDownloadAndInstallPackagesResultCount(&u4, &dcnt) == XGR_S_OK && dcnt == 0u);
            OS_REQUIRE(XStoreDownloadAndInstallPackagesResult(&u4, 0u, nullptr) == XGR_S_OK);

            // Collections ID, Purchase ID, License Token
            XAsyncBlock cid_ab{}, pid_ab{}, ltok_ab{};
            size_t sz = 0;
            char str_buf[128]{};
            OS_REQUIRE(XStoreGetUserCollectionsIdAsync(sctx, "ticket", "pubuser", &cid_ab) == XGR_S_OK);
            OS_REQUIRE(XStoreGetUserCollectionsIdResultSize(&cid_ab, &sz) == XGR_S_OK && sz > 1 && sz < sizeof(str_buf));
            OS_REQUIRE(XStoreGetUserCollectionsIdResult(&cid_ab, sizeof(str_buf), str_buf) == XGR_S_OK);
            OS_REQUIRE(std::strlen(str_buf) > 0);

            OS_REQUIRE(XStoreGetUserPurchaseIdAsync(sctx, "ticket", "pubuser", &pid_ab) == XGR_S_OK);
            OS_REQUIRE(XStoreGetUserPurchaseIdResultSize(&pid_ab, &sz) == XGR_S_OK && sz > 1 && sz < sizeof(str_buf));
            OS_REQUIRE(XStoreGetUserPurchaseIdResult(&pid_ab, sizeof(str_buf), str_buf) == XGR_S_OK);

            const char* pids[] = {"9OS000000001"};
            OS_REQUIRE(XStoreQueryLicenseTokenAsync(sctx, pids, 1, "devstr", &ltok_ab) == XGR_S_OK);
            OS_REQUIRE(XStoreQueryLicenseTokenResultSize(&ltok_ab, &sz) == XGR_S_OK && sz > 1 && sz < sizeof(str_buf));
            OS_REQUIRE(XStoreQueryLicenseTokenResult(&ltok_ab, sizeof(str_buf), str_buf) == XGR_S_OK);

            char pkg_out[XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH]{};
            OS_REQUIRE(XStoreQueryPackageIdentifier("9OS000000001", sizeof(pkg_out), pkg_out) == XGR_S_OK);
            OS_REQUIRE(std::strlen(pkg_out) > 0);

            XStoreAvailability avail{};
            OS_REQUIRE(!XStoreIsAvailabilityPurchasable(avail));
            avail.availabilityId = "AVAIL0000001";
            OS_REQUIRE(XStoreIsAvailabilityPurchasable(avail));

            XAsyncBlock ui1{}, ui2{}, ui3{}, ui4{}, ui5{};
            OS_REQUIRE(XStoreShowAssociatedProductsUIAsync(sctx, "9OS000000001", XStoreProductKind_Durable, &ui1) == XGR_S_OK);
            OS_REQUIRE(XStoreShowAssociatedProductsUIResult(&ui1) == XGR_S_OK);
            OS_REQUIRE(XStoreShowProductPageUIAsync(sctx, "9OS000000001", &ui2) == XGR_S_OK);
            OS_REQUIRE(XStoreShowProductPageUIResult(&ui2) == XGR_S_OK);
            OS_REQUIRE(XStoreShowPurchaseUIAsync(sctx, "9OS000000001", nullptr, nullptr, &ui3) == XGR_S_OK);
            OS_REQUIRE(XStoreShowPurchaseUIResult(&ui3) == XGR_E_ABORT);
            OS_REQUIRE(XStoreShowRateAndReviewUIAsync(sctx, &ui4) == XGR_S_OK);
            OS_REQUIRE(XStoreShowRateAndReviewUIResult(&ui4) == XGR_S_OK);
            OS_REQUIRE(XStoreShowRedeemTokenUIAsync(sctx, nullptr, nullptr, 0, false, &ui5) == XGR_S_OK);
            OS_REQUIRE(XStoreShowRedeemTokenUIResult(&ui5) == XGR_S_OK);
        }

        store_vt->XStoreCloseContextHandle(sctx);
    }

    // --- 3. XGameUi* full parity (direct + QueryApiImpl) ---
    void* raw_ui_vt = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_GAME_UI, &raw_ui_vt) == XGR_S_OK && raw_ui_vt != nullptr);
    const auto* ui_vt = static_cast<const IXgrGameUiApiVtable*>(raw_ui_vt);
    {
        OS_REQUIRE(ui_vt->XGameUiSetNotificationPositionHint(XGameUiNotificationPositionHint_TopRight) == XGR_S_OK);

        XAsyncBlock m_ab{};
        OS_REQUIRE(ui_vt->XGameUiShowMessageDialogAsync(
                       &m_ab, "Title", "Body", "OK", "Cancel", nullptr,
                       XGameUiMessageDialogButton_Second, XGameUiMessageDialogButton_Second) == XGR_S_OK);
        XGameUiMessageDialogButton btn = XGameUiMessageDialogButton_First;
        OS_REQUIRE(ui_vt->XGameUiShowMessageDialogResult(&m_ab, &btn) == XGR_S_OK);
        OS_REQUIRE(btn == XGameUiMessageDialogButton_Second);

        XAsyncBlock e_ab{};
        OS_REQUIRE(XGameUiShowErrorDialogAsync(&e_ab, XGR_E_FAIL, "ctx") == XGR_S_OK);
        OS_REQUIRE(XGameUiShowErrorDialogResult(&e_ab) == XGR_S_OK);

        XAsyncBlock t_ab{};
        OS_REQUIRE(XGameUiShowTextEntryAsync(
                       &t_ab, "Name", "Enter name", "ScarlettHero",
                       XGameUiTextEntryInputScope_Default, 8) == XGR_S_OK);
        uint32_t tsz = 0;
        OS_REQUIRE(XGameUiShowTextEntryResultSize(&t_ab, &tsz) == XGR_S_OK && tsz == 9u);
        char tbuf[32]{};
        uint32_t tused = 0;
        OS_REQUIRE(XGameUiShowTextEntryResult(&t_ab, sizeof(tbuf), tbuf, &tused) == XGR_S_OK);
        OS_REQUIRE(std::strcmp(tbuf, "Scarlett") == 0 && tused == 9u);

        XAsyncBlock a_ab{}, p_ab{}, pc_ab{}, inv_ab{};
        OS_REQUIRE(XGameUiShowAchievementsAsync(&a_ab, user, 0x5CA00001u) == XGR_S_OK);
        OS_REQUIRE(XGameUiShowAchievementsResult(&a_ab) == XGR_S_OK);

        const uint64_t pre[2] = {0x1111ULL, 0x2222ULL};
        OS_REQUIRE(XGameUiShowPlayerPickerAsync(&p_ab, user, "Pick", 2, pre, 2, pre, 0, 2) == XGR_S_OK);
        uint32_t pcnt = 0;
        OS_REQUIRE(XGameUiShowPlayerPickerResultCount(&p_ab, &pcnt) == XGR_S_OK && pcnt == 2u);
        uint64_t pout[2]{};
        uint32_t pused = 0;
        OS_REQUIRE(XGameUiShowPlayerPickerResult(&p_ab, 2, pout, &pused) == XGR_S_OK);
        OS_REQUIRE(pused == 2u && pout[0] == 0x1111ULL && pout[1] == 0x2222ULL);

        OS_REQUIRE(XGameUiShowPlayerProfileCardAsync(&pc_ab, user, 0x1111ULL) == XGR_S_OK);
        OS_REQUIRE(XGameUiShowPlayerProfileCardResult(&pc_ab) == XGR_S_OK);
        OS_REQUIRE(XGameUiShowSendGameInviteAsync(&inv_ab, user, "scid", "tmpl", "sid", "hi", nullptr) == XGR_S_OK);
        OS_REQUIRE(XGameUiShowSendGameInviteResult(&inv_ab) == XGR_S_OK);

        XAsyncBlock wa_ab{};
        OS_REQUIRE(XGameUiShowWebAuthenticationAsync(
                       &wa_ab, user, "https://login.invalid", "https://done.invalid") == XGR_S_OK);
        size_t wasz = 0;
        OS_REQUIRE(XGameUiShowWebAuthenticationResultSize(&wa_ab, &wasz) == XGR_S_OK && wasz > sizeof(XGameUiWebAuthenticationResultData));
        std::vector<uint8_t> wabuf(wasz, 0);
        XGameUiWebAuthenticationResultData* wadata = nullptr;
        size_t waused = 0;
        OS_REQUIRE(XGameUiShowWebAuthenticationResult(&wa_ab, wabuf.size(), wabuf.data(), &wadata, &waused) == XGR_S_OK);
        OS_REQUIRE(wadata != nullptr && wadata->responseStatus == XGR_E_ABORT);
        OS_REQUIRE(std::strcmp(wadata->responseCompletionUri, "https://done.invalid") == 0);
    }

    // --- 4. XNetworking* full parity (direct + QueryApiImpl) ---
    void* raw_net_vt = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_NETWORKING, &raw_net_vt) == XGR_S_OK && raw_net_vt != nullptr);
    const auto* net_vt = static_cast<const IXgrNetworkingApiVtable*>(raw_net_vt);
    {
        XNetworkingConnectivityHint hint{};
        OS_REQUIRE(net_vt->XNetworkingGetConnectivityHint(&hint) == XGR_S_OK);
        OS_REQUIRE(hint.networkInitialized && hint.connectivityLevel == XNetworkingConnectivityLevelHint_InternetAccess);

        XNetworkingConnectivityHint hint2{};
        OS_REQUIRE(net_vt->XNetworkingQueryConnectivityHint(&hint2) == XGR_S_OK && hint2.networkInitialized);

        uint16_t port = 0;
        OS_REQUIRE(net_vt->XNetworkingQueryPreferredLocalUdpMultiplayerPort(&port) == XGR_S_OK && port == 3074u);

        XAsyncBlock pab{};
        OS_REQUIRE(XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(&pab) == XGR_S_OK);
        uint16_t async_port = 0;
        OS_REQUIRE(XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(&pab, &async_port) == XGR_S_OK && async_port == 3074u);

        XTaskQueueRegistrationToken ntok1{}, ntok2{};
        OS_REQUIRE(XNetworkingRegisterConnectivityHintChanged(nullptr, nullptr, &DummyNetHintCb, &ntok1) == XGR_S_OK);
        OS_REQUIRE(XNetworkingUnregisterConnectivityHintChanged(ntok1, true));
        OS_REQUIRE(XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(nullptr, nullptr, &DummyNetPortCb, &ntok2) == XGR_S_OK);
        OS_REQUIRE(XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(ntok2, true));

        // Security info UTF-8 & UTF-16 + VerifyServerCertificate
        XAsyncBlock sec1{}, sec2{};
        OS_REQUIRE(XNetworkingQuerySecurityInformationForUrlAsync("https://xboxlive.com", &sec1) == XGR_S_OK);
        size_t ssz1 = 0;
        OS_REQUIRE(XNetworkingQuerySecurityInformationForUrlAsyncResultSize(&sec1, &ssz1) == XGR_S_OK);
        std::vector<uint8_t> sbuf1(ssz1, 0);
        XNetworkingSecurityInformation* sinfo1 = nullptr;
        size_t sused1 = 0;
        OS_REQUIRE(XNetworkingQuerySecurityInformationForUrlAsyncResult(&sec1, sbuf1.size(), &sused1, sbuf1.data(), &sinfo1) == XGR_S_OK);
        OS_REQUIRE(sinfo1 != nullptr && sinfo1->enabledHttpSecurityProtocolFlags != 0u);
        int dummy_req = 1;
        OS_REQUIRE(XNetworkingVerifyServerCertificate(&dummy_req, sinfo1) == XGR_S_OK);

        OS_REQUIRE(XNetworkingQuerySecurityInformationForUrlUtf16Async(L"https://xboxlive.com", &sec2) == XGR_S_OK);
        size_t ssz2 = 0;
        OS_REQUIRE(XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(&sec2, &ssz2) == XGR_S_OK);
        std::vector<uint8_t> sbuf2(ssz2, 0);
        XNetworkingSecurityInformation* sinfo2 = nullptr;
        size_t sused2 = 0;
        OS_REQUIRE(XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(&sec2, sbuf2.size(), &sused2, sbuf2.data(), &sinfo2) == XGR_S_OK);
        OS_REQUIRE(sinfo2 != nullptr && sinfo2->enabledHttpSecurityProtocolFlags != 0u);
    }

    // --- 5. XPackage* full parity (direct + QueryApiImpl) ---
    void* raw_pkg_vt = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_PACKAGE, &raw_pkg_vt) == XGR_S_OK && raw_pkg_vt != nullptr);
    const auto* pkg_vt = static_cast<const IXgrPackageApiVtable*>(raw_pkg_vt);
    {
        char pkg_id[128]{};
        OS_REQUIRE(pkg_vt->XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id) == XGR_S_OK);

        XPackageMountHandle mh = nullptr;
        OS_REQUIRE(pkg_vt->XPackageMount(pkg_id, &mh) == XGR_S_OK && mh != nullptr);
        size_t msz = 0;
        OS_REQUIRE(pkg_vt->XPackageGetMountPathSize(mh, &msz) == XGR_S_OK && msz > 1);
        std::vector<char> mpath(msz, '\0');
        OS_REQUIRE(pkg_vt->XPackageGetMountPath(mh, mpath.size(), mpath.data()) == XGR_S_OK);
        OS_REQUIRE(std::strlen(mpath.data()) > 0);
        pkg_vt->XPackageCloseMountHandle(mh);

        char loc[32]{};
        OS_REQUIRE(XPackageGetUserLocale(sizeof(loc), loc) == XGR_S_OK && std::strlen(loc) >= 2);

        XPackageWriteStats ws{};
        OS_REQUIRE(XPackageGetWriteStats(&ws) == XGR_S_OK && ws.budget > 0);

        XPackageInstallationMonitorHandle mon = nullptr;
        OS_REQUIRE(XPackageCreateInstallationMonitor(pkg_id, 0, nullptr, 1000, nullptr, &mon) == XGR_S_OK && mon != nullptr);
        XPackageInstallationProgress prog{};
        XPackageGetInstallationProgress(mon, &prog);
        OS_REQUIRE(prog.completed && prog.launchable && prog.installedBytes == prog.totalBytes);
        OS_REQUIRE(!XPackageUpdateInstallationMonitor(mon));

        XTaskQueueRegistrationToken ptok1{}, ptok2{};
        OS_REQUIRE(XPackageRegisterInstallationProgressChanged(mon, nullptr, &DummyPkgProgressCb, &ptok1) == XGR_S_OK);
        OS_REQUIRE(XPackageUnregisterInstallationProgressChanged(mon, ptok1, true));
        XPackageCloseInstallationMonitorHandle(mon);

        OS_REQUIRE(XPackageRegisterPackageInstalled(nullptr, nullptr, &DummyPkgInstalledCb, &ptok2) == XGR_S_OK);
        OS_REQUIRE(XPackageUnregisterPackageInstalled(ptok2, true));

        std::vector<std::string> pkgs;
        OS_REQUIRE(XPackageEnumeratePackages(XPackageKind_Game, XPackageEnumerationScope_ThisOnly, &pkgs, &EnumPackageDetailsCb) == XGR_S_OK);
        OS_REQUIRE(pkgs.size() == 1);

        std::vector<std::string> feats;
        OS_REQUIRE(XPackageEnumerateFeatures(pkg_id, &feats, &EnumPackageFeatureCb) == XGR_S_OK);
        OS_REQUIRE(feats.size() == 1 && feats[0] == "BaseGame");

        XPackageChunkAvailability cavail = XPackageChunkAvailability_Unavailable;
        OS_REQUIRE(XPackageFindChunkAvailability(pkg_id, 0, nullptr, &cavail) == XGR_S_OK);
        OS_REQUIRE(cavail == XPackageChunkAvailability_Ready);

        uint64_t dl_sz = 999;
        bool confirm = true;
        OS_REQUIRE(XPackageEstimateDownloadSize(pkg_id, 0, nullptr, &dl_sz, &confirm) == XGR_S_OK);
        OS_REQUIRE(dl_sz == 0 && !confirm);
        OS_REQUIRE(XPackageChangeChunkInstallOrder(pkg_id, 0, nullptr) == XGR_S_OK);

        XPackageInstallationMonitorHandle im1 = nullptr;
        OS_REQUIRE(XPackageInstallChunks(pkg_id, 0, nullptr, 1000, true, nullptr, &im1) == XGR_S_OK && im1 != nullptr);
        XPackageCloseInstallationMonitorHandle(im1);

        XAsyncBlock ic_ab{};
        OS_REQUIRE(XPackageInstallChunksAsync(pkg_id, 0, nullptr, 1000, true, &ic_ab) == XGR_S_OK);
        XPackageInstallationMonitorHandle im2 = nullptr;
        OS_REQUIRE(XPackageInstallChunksResult(&ic_ab, &im2) == XGR_S_OK && im2 != nullptr);
        XPackageCloseInstallationMonitorHandle(im2);

        OS_REQUIRE(XPackageUninstallChunks(pkg_id, 0, nullptr) == XGR_S_OK);
        OS_REQUIRE(XPackageUninstallUWPInstance(pkg_id) == XGR_S_OK);
    }

    // --- 6. Core Runtime Parity (direct + QueryApiImpl) ---
    void* raw_core_vt = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_CORE_RUNTIME, &raw_core_vt) == XGR_S_OK && raw_core_vt != nullptr);
    const auto* core_vt = static_cast<const IXgrCoreRuntimeApiVtable*>(raw_core_vt);
    {
        OS_REQUIRE(core_vt->XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XStore));
        OS_REQUIRE(core_vt->XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XNetworking));

        // XDisplayAcquireTimeoutDeferral alias
        XDisplayTimeoutDeferralHandle ddef = nullptr;
        OS_REQUIRE(XDisplayAcquireTimeoutDeferral(&ddef) == XGR_S_OK && ddef != nullptr);
        XDisplayCloseTimeoutDeferralHandle(ddef);

        // XGameSaveEnumerateBlobInfoByName + XGameSaveGetContainerInfo
        {
            const char* kScid = "00000000-0000-0000-0000-00005ca0face";
            XGameSaveProviderHandle prov = nullptr;
            OS_REQUIRE(XGameSaveInitializeProvider(user, kScid, false, &prov) == XGR_S_OK);
            (void)XGameSaveDeleteContainer(prov, "meta_slot");
            XGameSaveContainerHandle cont = nullptr;
            OS_REQUIRE(XGameSaveCreateContainer(prov, "meta_slot", &cont) == XGR_S_OK);
            XGameSaveUpdateHandle upd = nullptr;
            OS_REQUIRE(XGameSaveCreateUpdate(cont, "Meta Slot", &upd) == XGR_S_OK);
            const uint8_t d1[4] = {1, 2, 3, 4};
            OS_REQUIRE(XGameSaveSubmitBlobWrite(upd, "cfg_audio", d1, sizeof(d1)) == XGR_S_OK);
            OS_REQUIRE(XGameSaveSubmitBlobWrite(upd, "sav_level1", d1, sizeof(d1)) == XGR_S_OK);
            OS_REQUIRE(XGameSaveSubmitUpdate(upd) == XGR_S_OK);
            XGameSaveCloseUpdate(upd);

            std::vector<std::string> filtered_blobs;
            OS_REQUIRE(XGameSaveEnumerateBlobInfoByName(cont, "cfg_", &filtered_blobs, &EnumBlobCb) == XGR_S_OK);
            OS_REQUIRE(filtered_blobs.size() == 1 && filtered_blobs[0] == "cfg_audio");

            EnumContainersCtx cinfo{};
            OS_REQUIRE(XGameSaveGetContainerInfo(prov, "meta_slot", &cinfo, &EnumContainerCb) == XGR_S_OK);
            OS_REQUIRE(cinfo.names.size() == 1 && cinfo.names[0] == "meta_slot" && cinfo.last_blob_count == 2);

            XGameSaveCloseContainer(cont);
            OS_REQUIRE(XGameSaveDeleteContainer(prov, "meta_slot") == XGR_S_OK);
            XGameSaveCloseProvider(prov);
        }

        // Persistent Local Storage
        size_t pls_sz = 0;
        OS_REQUIRE(core_vt->XPersistentLocalStorageGetPathSize(&pls_sz) == XGR_S_OK && pls_sz > 1);
        std::vector<char> pls_path(pls_sz, '\0');
        size_t pls_used = 0;
        OS_REQUIRE(core_vt->XPersistentLocalStorageGetPath(pls_path.size(), pls_path.data(), &pls_used) == XGR_S_OK);
        OS_REQUIRE(pls_used == pls_sz && std::strlen(pls_path.data()) > 0);

        // XThread
        OS_REQUIRE(!core_vt->XThreadIsTimeSensitive());
        OS_REQUIRE(core_vt->XThreadVerifyNotTimeSensitive());
        OS_REQUIRE(core_vt->XThreadSetTimeSensitive(true) == XGR_S_OK);
        OS_REQUIRE(core_vt->XThreadIsTimeSensitive());
        OS_REQUIRE(!core_vt->XThreadVerifyNotTimeSensitive());
        OS_REQUIRE(core_vt->XThreadSetTimeSensitive(false) == XGR_S_OK);
        core_vt->XThreadAssertNotTimeSensitive();

        // XError
        std::atomic<int> err_hits{0};
        core_vt->XErrorSetOptions(XErrorOptions_None, XErrorOptions_None);
        core_vt->XErrorSetCallback(&TestErrorCb, &err_hits);
        core_vt->XErrorReport(XGR_E_FAIL, "synthetic_error");
        OS_REQUIRE(err_hits.load() == 1);
        core_vt->XErrorSetCallback(nullptr, nullptr);

        // XGameEventWrite, XGameInvite, XLaunch, XHighContrast, XClosedCaption
        XGameEventDescriptor edesc{};
        OS_REQUIRE(core_vt->XGameEventWrite(0, &edesc, 0, nullptr) == 0u);

        XTaskQueueRegistrationToken itok{};
        OS_REQUIRE(core_vt->XGameInviteRegisterForEvent(nullptr, nullptr, &DummyInviteCb, &itok) == XGR_S_OK);
        OS_REQUIRE(core_vt->XGameInviteUnregisterForEvent(itok, true));

        core_vt->XLaunchNewGame("game.exe", "", user);
        OS_REQUIRE(core_vt->XLaunchUri(user, "ms-xbox://home") == XGR_S_OK);

        XHighContrastMode hcm = XHighContrastMode_Other;
        OS_REQUIRE(core_vt->XHighContrastGetMode(&hcm) == XGR_S_OK && hcm == XHighContrastMode_Off);

        OS_REQUIRE(core_vt->XClosedCaptionSetEnabled(true) == XGR_S_OK);
        XClosedCaptionProperties cc{};
        OS_REQUIRE(core_vt->XClosedCaptionGetProperties(&cc) == XGR_S_OK && cc.Enabled && cc.FontScale == 1.0f);
        OS_REQUIRE(core_vt->XClosedCaptionSetEnabled(false) == XGR_S_OK);
    }

    // --- 7. 3-arg real XGamingRuntimeThunks QueryApiImpl(clsid, iid, ppv) COM ABI ---
    {
        static const XgrGuid kRealClsid = {
            0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};
        static const XgrGuid kRealIid = {
            0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};
        void* out_iface = nullptr;
        const XGameResult hr3 = QueryApiImpl(
            &kRealClsid,
            reinterpret_cast<void**>(const_cast<XgrGuid*>(&kRealIid)),
            &out_iface);
        OS_REQUIRE(hr3 == XGR_S_OK);
        OS_REQUIRE(out_iface != nullptr);

        static const XgrGuid kUnknownClsid = {
            0xDEADBEEFu, 0x1234u, 0x5678u, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02}};
        static const XgrGuid kUnknownIid = {
            0xCAFEBABEu, 0x4321u, 0x8765u, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};
        void* out_unknown = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234u));
        const XGameResult hr_unk = QueryApiImpl(
            &kUnknownClsid,
            reinterpret_cast<void**>(const_cast<XgrGuid*>(&kUnknownIid)),
            &out_unknown);
        OS_REQUIRE(hr_unk == XGR_E_NOINTERFACE);
        OS_REQUIRE(out_unknown == nullptr);
    }

    XUserCloseHandle(user);
    UninitializeApiImpl();
    std::printf("[PASS] xgameruntime.dll thunks parity (XUser, XStore, XGameUi, XNetworking, XPackage, Core + 15-GUID & 3-arg QueryApiImpl)\n");
}

}  // namespace

int main() {
#if defined(_WIN32)
    _putenv_s("OPENSCARLETT_AUDIO_NULL", "1");
#else
    setenv("OPENSCARLETT_AUDIO_NULL", "1", 1);
#endif

    TestXGameRuntime();
    TestXGameRuntimeThunksParity();
    TestGameInput();
    TestXAudio2AndAcpHal();
    TestDirectStorage();
    TestXsapi();

    XGameRuntimeUninitialize();

    std::printf("[PASS] test_gdk_shims: all 6 GDK runtime shims verified end-to-end!\n");
    return 0;
}
