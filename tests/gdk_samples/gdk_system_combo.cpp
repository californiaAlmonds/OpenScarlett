// OpenScarlett MIT Standalone Xbox-ABI Guest Sample 4:
// Unified GDK System Combo — exercises ALL 10 shimmed Xbox DLLs in ONE live APC-injected process!
//   1. kernelx.dll                        (TitleMemoryStatus, AllocateTitlePhysicalPages + MapTitleVirtualMemory aliasing)
//   2. xgameruntime.dll                   (XSystem, XTaskQueue, XAsync, XUser, XGameSave disk persistence on E:)
//   3. GameInput.dll                      (GameInputCreate, GetCurrentReading, GetGamepadState, SetRumbleState)
//   4. dstorage_xs.dll                    (IDStorageQueueX raw G:\asset.bin + GDeflate G:\asset.gdeflate)
//   5. XAudio2_9.dll                      (PCM16 + real XMA2 0x0166 decode/mix with OPENSCARLETT_AUDIO_NULL=1)
//   6. AcpHal.dll                         (AcpHalAllocateShapeContexts + AcpHalCommand_XmaDecode on real XMA2)
//   7. xg_xs.dll                          (64KB_S texture swizzle/deswizzle bit-exact round-trip)
//   8. winrt_xs.dll                       (RoInitialize + WindowsCreateString round-trip)
//   9. Microsoft.Xbox.Services.GDK.C.dll  (XblInitialize + XblContext + XblProfile + XblAchievements)
//  10. d3d12_xs.dll                       (D3D12XboxCreateDevice + GetGpuHardwareInfoX + WaitFrameEventX)
//
// Deliberately does NOT link openscarlett_core!

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/winrt/winrt_abi.h"
#include "openscarlett/xsapi/xsapi_abi.h"

namespace {

constexpr size_t k64KiB = 65536u;
constexpr uint32_t kMemGraphics = 0x10000000u;

struct KernelXTitleMemoryStatus {
    uint32_t dwLength;
    uint32_t dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullLegacyUsed;
    uint64_t ullLegacyPeak;
    uint64_t ullLegacyAvail;
    uint64_t ullTitleUsed;
    uint64_t ullTitleAvail;
};

using PfnGetConsoleType = uint32_t(__stdcall*)();
using PfnXGetProcessorCount = uint32_t(__stdcall*)();
using PfnTitleMemoryStatus = int32_t(__stdcall*)(KernelXTitleMemoryStatus*);
using PfnAllocateTitlePhysicalPages = int32_t(__stdcall*)(void*, uint32_t, uint64_t*, uint64_t*);
using PfnFreeTitlePhysicalPages = int32_t(__stdcall*)(void*, uint64_t, const uint64_t*);
using PfnMapTitleVirtualMemory = void*(__stdcall*)(void*, uint64_t, uint32_t, uint32_t, const uint64_t*);
using PfnUnmapTitleVirtualMemory = int32_t(__stdcall*)(void*, uint64_t);

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_system_combo] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

bool ReadWholeFileW(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD total = 0;
    while (total < static_cast<DWORD>(out.size())) {
        DWORD chunk = 0;
        if (!::ReadFile(h, out.data() + total,
                        static_cast<DWORD>(out.size()) - total, &chunk, nullptr) ||
            chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool WriteWholeFileW(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written_total = 0;
    while (written_total < size) {
        DWORD chunk = 0;
        const DWORD to_write = static_cast<DWORD>(size - written_total);
        if (!::WriteFile(h, bytes + written_total, to_write, &chunk, nullptr) || chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        written_total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

// XTaskQueue + XAsync helpers
void OPENSCARLETT_GDK_CALL TaskQueueIncCb(void* ctx, bool canceled) {
    if (!canceled) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
}

const char kComboAsyncId[] = "GdkSystemComboAsync";

struct ComboAsyncState {
    uint32_t in_val = 0;
    uint32_t out_val = 0;
};

XGameResult OPENSCARLETT_GDK_CALL ComboAsyncProvider(
    XAsyncOp op, const XAsyncProviderData* data) {
    auto* st = static_cast<ComboAsyncState*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            st->out_val = st->in_val * 5u + 11u;
            XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(uint32_t)) return XGR_E_NOT_SUFFICIENT_BUFFER;
            std::memcpy(data->buffer, &st->out_val, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete st;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

void OPENSCARLETT_GDK_CALL ComboAsyncDoneCb(XAsyncBlock* block) {
    static_cast<std::atomic<bool>*>(block->context)->store(true, std::memory_order_release);
}

bool OPENSCARLETT_GDK_CALL CountContainersCb(const XGameSaveContainerInfo* /*info*/, void* ctx) {
    (*static_cast<int*>(ctx)) += 1;
    return true;
}

double QpcMs(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq) {
    if (freq.QuadPart == 0) return 0.0;
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

}  // namespace

int main() {
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    // =========================================================================
    // 1. kernelx.dll (loaded dynamically via hooked LoadLibraryA!)
    // =========================================================================
    HMODULE h_kernelx = ::LoadLibraryA("kernelx.dll");
    if (!h_kernelx) {
        return Fail("LoadLibraryA(kernelx.dll)", ::GetLastError());
    }
    auto p_get_console = reinterpret_cast<PfnGetConsoleType>(::GetProcAddress(h_kernelx, "GetConsoleType"));
    auto p_get_procs = reinterpret_cast<PfnXGetProcessorCount>(::GetProcAddress(h_kernelx, "XGetProcessorCount"));
    auto p_tms = reinterpret_cast<PfnTitleMemoryStatus>(::GetProcAddress(h_kernelx, "TitleMemoryStatus"));
    auto p_alloc_phys = reinterpret_cast<PfnAllocateTitlePhysicalPages>(
        ::GetProcAddress(h_kernelx, "AllocateTitlePhysicalPages"));
    auto p_free_phys = reinterpret_cast<PfnFreeTitlePhysicalPages>(
        ::GetProcAddress(h_kernelx, "FreeTitlePhysicalPages"));
    auto p_map_vm = reinterpret_cast<PfnMapTitleVirtualMemory>(
        ::GetProcAddress(h_kernelx, "MapTitleVirtualMemory"));
    auto p_unmap_vm = reinterpret_cast<PfnUnmapTitleVirtualMemory>(
        ::GetProcAddress(h_kernelx, "UnmapTitleVirtualMemory"));
    if (!p_get_console || !p_get_procs || !p_tms || !p_alloc_phys ||
        !p_free_phys || !p_map_vm || !p_unmap_vm) {
        return Fail("GetProcAddress on kernelx.dll");
    }

    if (p_get_console() != 4u /* CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA */) {
        return Fail("kernelx GetConsoleType != Scarlett Anaconda");
    }
    if (p_get_procs() != 14u) {
        return Fail("kernelx XGetProcessorCount != 14");
    }

    KernelXTitleMemoryStatus tms{};
    tms.dwLength = static_cast<uint32_t>(sizeof(KernelXTitleMemoryStatus));
    if (p_tms(&tms) != 1 || tms.ullTotalMem != (27ull * 512ull * 1024ull * 1024ull)) {
        return Fail("kernelx TitleMemoryStatus");
    }

    // Live 64 KiB physical-virtual aliasing across two distinct virtual addresses
    uint64_t page_count = 1u;
    uint64_t pfn64 = 0u;
    if (p_alloc_phys(nullptr, kMemGraphics | MEM_COMMIT, &page_count, &pfn64) != 1 ||
        page_count != 1u || pfn64 == 0u) {
        return Fail("kernelx AllocateTitlePhysicalPages");
    }
    void* va1 = p_map_vm(nullptr, 1u, kMemGraphics, PAGE_READWRITE, &pfn64);
    void* va2 = p_map_vm(nullptr, 1u, kMemGraphics, PAGE_READWRITE, &pfn64);
    if (!va1 || !va2 || va1 == va2) {
        return Fail("kernelx MapTitleVirtualMemory aliasing");
    }
    auto* b1 = static_cast<uint8_t*>(va1);
    auto* b2 = static_cast<uint8_t*>(va2);
    const uint64_t alias_magic = 0x534341524C455454ull;  // "SCARLETT"
    std::memcpy(b1, &alias_magic, sizeof(alias_magic));
    for (size_t i = sizeof(alias_magic); i < k64KiB; ++i) {
        b1[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xFFu);
    }
    uint64_t read_magic = 0;
    std::memcpy(&read_magic, b2, sizeof(read_magic));
    if (read_magic != alias_magic || std::memcmp(b1, b2, k64KiB) != 0) {
        return Fail("kernelx 64 KiB physical alias payload mismatch");
    }
    if (p_unmap_vm(va1, 1u) != 1 || p_unmap_vm(va2, 1u) != 1 || p_free_phys(nullptr, 1u, &pfn64) != 1) {
        return Fail("kernelx unmap/free physical pages");
    }

    // =========================================================================
    // 2. xgameruntime.dll (XSystem, XTaskQueue, XAsync, XUser, XGameSave)
    // =========================================================================
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        return Fail("XGameRuntimeInitialize");
    }
    if (XSystemGetDeviceType() != XSystemDeviceType_XboxScarlettAnaconda) {
        return Fail("XSystemGetDeviceType != Anaconda");
    }

    // XTaskQueue callback execution
    {
        XTaskQueueHandle tq = nullptr;
        if (XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                             XTaskQueueDispatchMode_ThreadPool, &tq) != XGR_S_OK || !tq) {
            return Fail("XTaskQueueCreate");
        }
        std::atomic<int> hits{0};
        if (XTaskQueueSubmitCallback(tq, XTaskQueuePort_Work, &hits, &TaskQueueIncCb) != XGR_S_OK) {
            return Fail("XTaskQueueSubmitCallback");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (hits.load() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (hits.load() != 1) {
            return Fail("XTaskQueue callback did not fire");
        }
        XTaskQueueTerminate(tq, true, nullptr, nullptr);
        XTaskQueueCloseHandle(tq);
    }

    // XAsyncBegin / XAsyncSchedule / XAsyncComplete / XAsyncGetResult
    {
        std::atomic<bool> done{false};
        XAsyncBlock ab{};
        ab.context = &done;
        ab.callback = &ComboAsyncDoneCb;
        auto* ctx = new ComboAsyncState{19u, 0u};
        if (XAsyncBegin(&ab, ctx, kComboAsyncId, kComboAsyncId, &ComboAsyncProvider) != XGR_S_OK ||
            XAsyncSchedule(&ab, 0) != XGR_S_OK ||
            XAsyncGetStatus(&ab, true) != XGR_S_OK ||
            !done.load()) {
            return Fail("XAsync custom provider execution");
        }
        uint32_t out_val = 0;
        size_t used = 0;
        if (XAsyncGetResult(&ab, kComboAsyncId, sizeof(out_val), &out_val, &used) != XGR_S_OK ||
            used != sizeof(uint32_t) || out_val != (19u * 5u + 11u)) {
            return Fail("XAsyncGetResult mismatch", out_val);
        }
    }

    // XUserAddAsync + XUserGetId + XUserGetGamertag
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
    {
        XAsyncBlock user_ab{};
        if (XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) != XGR_S_OK ||
            XUserAddResult(&user_ab, &user) != XGR_S_OK || !user) {
            return Fail("XUserAddAsync/Result");
        }
        if (XUserGetId(user, &xuid) != XGR_S_OK || xuid == 0) {
            return Fail("XUserGetId");
        }
        char gt[64]{};
        size_t gt_used = 0;
        if (XUserGetGamertag(user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) != XGR_S_OK ||
            gt[0] == '\0') {
            return Fail("XUserGetGamertag");
        }
    }

    // XGameSave write -> close -> reopen -> read -> verify -> delete on E: save root
    {
        const char* kScid = "00000000-0000-0000-0000-00005ca03333";
        const char* kContainer = "live_combo_slot";
        const uint8_t payload[] = {0x53, 0x43, 0x41, 0x52, 0x4C, 0x45, 0x54, 0x54, 0x03, 0x20, 0x26, 0xFF};

        XAsyncBlock init_ab{};
        XGameSaveProviderHandle prov = nullptr;
        if (XGameSaveInitializeProviderAsync(user, kScid, false, &init_ab) != XGR_S_OK ||
            XGameSaveInitializeProviderResult(&init_ab, &prov) != XGR_S_OK || !prov) {
            return Fail("XGameSaveInitializeProviderAsync");
        }

        XGameSaveContainerHandle cont = nullptr;
        XGameSaveUpdateHandle upd = nullptr;
        if (XGameSaveCreateContainer(prov, kContainer, &cont) != XGR_S_OK ||
            XGameSaveCreateUpdate(cont, "Sprint3 Save", &upd) != XGR_S_OK ||
            XGameSaveSubmitBlobWrite(upd, "checkpoint.dat", payload, sizeof(payload)) != XGR_S_OK ||
            XGameSaveSubmitUpdate(upd) != XGR_S_OK) {
            return Fail("XGameSave write");
        }
        XGameSaveCloseUpdate(upd);
        XGameSaveCloseContainer(cont);
        XGameSaveCloseProvider(prov);

        // Reopen fresh provider and verify persistence from disk on E:
        XGameSaveProviderHandle prov2 = nullptr;
        XGameSaveContainerHandle cont2 = nullptr;
        if (XGameSaveInitializeProvider(user, kScid, false, &prov2) != XGR_S_OK ||
            XGameSaveCreateContainer(prov2, kContainer, &cont2) != XGR_S_OK) {
            return Fail("XGameSave reopen");
        }

        const char* req_names[1] = {"checkpoint.dat"};
        uint32_t req_count = 1;
        std::vector<uint8_t> read_buf(512, 0);
        auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
        if (XGameSaveReadBlobData(cont2, req_names, &req_count, read_buf.size(), blobs) != XGR_S_OK ||
            req_count != 1 || blobs[0].info.size != sizeof(payload) ||
            std::memcmp(blobs[0].data, payload, sizeof(payload)) != 0) {
            return Fail("XGameSaveReadBlobData verification");
        }
        XGameSaveCloseContainer(cont2);

        if (XGameSaveDeleteContainer(prov2, kContainer) != XGR_S_OK) {
            return Fail("XGameSaveDeleteContainer");
        }
        int remaining = 0;
        if (XGameSaveEnumerateContainerInfo(prov2, &remaining, &CountContainersCb) != XGR_S_OK ||
            remaining != 0) {
            return Fail("XGameSave container not deleted", remaining);
        }
        XGameSaveCloseProvider(prov2);
    }

    // =========================================================================
    // 3. GameInput.dll (GetCurrentReading, GetGamepadState, SetRumbleState)
    // =========================================================================
    {
        IGameInput* gi = nullptr;
        if (GameInputCreate(&gi) != GAMEINPUT_S_OK || !gi) {
            return Fail("GameInputCreate");
        }
        IGameInputReading* r = nullptr;
        if (gi->GetCurrentReading(GameInputKindGamepad, nullptr, &r) != GAMEINPUT_S_OK || !r) {
            return Fail("GameInput GetCurrentReading");
        }
        GameInputGamepadState gp{};
        if (!r->GetGamepadState(&gp)) {
            return Fail("GameInput GetGamepadState");
        }
        IGameInputDevice* dev = nullptr;
        r->GetDevice(&dev);
        if (!dev) {
            return Fail("GameInput GetDevice");
        }
        GameInputRumbleParams on_rumble{0.2f, 0.1f, 0.0f, 0.0f};
        GameInputRumbleParams off_rumble{0.0f, 0.0f, 0.0f, 0.0f};
        dev->SetRumbleState(&on_rumble);
        dev->SetRumbleState(&off_rumble);
        dev->Release();
        r->Release();
        gi->Release();
    }

    // =========================================================================
    // 4. dstorage_xs.dll (real raw G:\asset.bin + GDeflate G:\asset.gdeflate)
    // =========================================================================
    {
        DSTORAGE_CONFIGURATION1 cfg{};
        cfg.NumSubmitThreads = 1;
        cfg.NumBuiltInCpuDecompressionThreads = 1;
        if (DStorageSetConfiguration1(&cfg) != OS_DSTORAGE_S_OK) {
            return Fail("DStorageSetConfiguration1");
        }

        IDStorageFactoryX* factory = nullptr;
        if (DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) != OS_DSTORAGE_S_OK ||
            !factory) {
            return Fail("DStorageGetFactory");
        }

        IDStorageFileX* raw_file = nullptr;
        IDStorageFileX* gdef_file = nullptr;
        if (factory->OpenFile(L"G:\\asset.bin", &IID_IDStorageFileX,
                              reinterpret_cast<void**>(&raw_file)) != OS_DSTORAGE_S_OK || !raw_file) {
            return Fail("DStorage OpenFile(G:\\asset.bin)");
        }
        if (factory->OpenFile(L"G:\\asset.gdeflate", &IID_IDStorageFileX,
                              reinterpret_cast<void**>(&gdef_file)) != OS_DSTORAGE_S_OK || !gdef_file) {
            return Fail("DStorage OpenFile(G:\\asset.gdeflate)");
        }

        DSTORAGE_FILE_INFO raw_info{};
        DSTORAGE_FILE_INFO gdef_info{};
        if (raw_file->GetFileInformation(&raw_info) != OS_DSTORAGE_S_OK ||
            gdef_file->GetFileInformation(&gdef_info) != OS_DSTORAGE_S_OK ||
            raw_info.FileSize != 16384ull || gdef_info.FileSize == 0ull) {
            return Fail("DStorage GetFileInformation");
        }

        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
        qdesc.Name = "combo_dstorage_queue";

        IDStorageQueueX* dsq = nullptr;
        IDStorageStatusArrayX* status = nullptr;
        if (factory->CreateQueue(&qdesc, &IID_IDStorageQueueX,
                                 reinterpret_cast<void**>(&dsq)) != OS_DSTORAGE_S_OK || !dsq ||
            factory->CreateStatusArray(4, "combo_status", &IID_IDStorageStatusArrayX,
                                       reinterpret_cast<void**>(&status)) != OS_DSTORAGE_S_OK || !status) {
            return Fail("DStorage CreateQueue/CreateStatusArray");
        }

        constexpr uint32_t kRawBytes = 16384u;
        constexpr uint32_t kGDefUncompBytes = 96u * 1024u;
        std::vector<uint8_t> raw_out(kRawBytes, 0u);
        std::vector<uint8_t> gdef_out(kGDefUncompBytes, 0u);

        OpenScarlettDStorageRequestDesc r0{};
        r0.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r0.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r0.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r0.Source.File.Source = raw_file;
        r0.Source.File.Offset = 0;
        r0.Source.File.Size = kRawBytes;
        r0.Destination.Memory.Buffer = raw_out.data();
        r0.Destination.Memory.Size = kRawBytes;
        r0.UncompressedSize = kRawBytes;
        OpenScarlettDStorageEnqueueRequestDesc(dsq, &r0);
        dsq->EnqueueStatus(status, 0);

        OpenScarlettDStorageRequestDesc r1{};
        r1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r1.Source.File.Source = gdef_file;
        r1.Source.File.Offset = 0;
        r1.Source.File.Size = static_cast<uint32_t>(gdef_info.FileSize);
        r1.Destination.Memory.Buffer = gdef_out.data();
        r1.Destination.Memory.Size = kGDefUncompBytes;
        r1.UncompressedSize = kGDefUncompBytes;
        OpenScarlettDStorageEnqueueRequestDesc(dsq, &r1);
        dsq->EnqueueStatus(status, 1);

        HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) return Fail("CreateEventW for DStorage");
        dsq->EnqueueSetEvent(ev);
        dsq->Submit();
        const DWORD wait_rc = ::WaitForSingleObject(ev, 10000);
        ::CloseHandle(ev);
        if (wait_rc != WAIT_OBJECT_0 ||
            !status->IsComplete(0) || status->GetHResult(0) != OS_DSTORAGE_S_OK ||
            !status->IsComplete(1) || status->GetHResult(1) != OS_DSTORAGE_S_OK) {
            return Fail("DStorage async completion/status");
        }

        // Verify bit-exact deterministic payloads staged in G:\asset.bin and G:\asset.gdeflate
        for (size_t i = 0; i < raw_out.size(); ++i) {
            const uint8_t expected = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
            if (raw_out[i] != expected) {
                return Fail("DStorage G:\\asset.bin payload mismatch", static_cast<int64_t>(i));
            }
        }
        for (size_t i = 0; i < gdef_out.size(); ++i) {
            const uint8_t expected = static_cast<uint8_t>(((i * 5u) + (i / 64u) * 19u) & 0xFFu);
            if (gdef_out[i] != expected) {
                return Fail("DStorage G:\\asset.gdeflate decompressed mismatch", static_cast<int64_t>(i));
            }
        }

        DSTORAGE_ERROR_RECORD err{};
        dsq->RetrieveErrorRecord(&err);
        if (err.FailureCount != 0u) {
            return Fail("DStorage FailureCount != 0", err.FailureCount);
        }

        dsq->Close();
        dsq->Release();
        status->Release();
        gdef_file->Close();
        gdef_file->Release();
        raw_file->Close();
        raw_file->Release();
        factory->Release();
    }

    // =========================================================================
    // 5 & 6. XAudio2_9.dll + AcpHal.dll (PCM16 + real XMA2 0x0166 from G:\sine440.xma2)
    // =========================================================================
    {
        std::vector<uint8_t> xma_packets;
        if (!ReadWholeFileW(L"G:\\sine440.xma2", xma_packets) ||
            xma_packets.empty() || (xma_packets.size() % 2048u) != 0u) {
            return Fail("Read G:\\sine440.xma2", ::GetLastError());
        }

        IXAudio2* xa = nullptr;
        if (XAudio2Create(&xa, 0, OS_XAUDIO2_DEFAULT_PROCESSOR) != OS_XAUDIO2_S_OK || !xa) {
            return Fail("XAudio2Create");
        }
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SubmixVoice* submix = nullptr;
        if (xa->CreateMasteringVoice(&master, 2, 48000) != OS_XAUDIO2_S_OK || !master ||
            xa->CreateSubmixVoice(&submix, 2, 48000) != OS_XAUDIO2_S_OK || !submix) {
            return Fail("XAudio2 CreateMasteringVoice/CreateSubmixVoice");
        }

        OS_XMA2WAVEFORMATEX xma_fmt{};
        xma_fmt.wfx.wFormatTag = OS_WAVE_FORMAT_XMA2;
        xma_fmt.wfx.nChannels = 1;
        xma_fmt.wfx.nSamplesPerSec = 48000;
        xma_fmt.wfx.wBitsPerSample = 16;
        xma_fmt.wfx.cbSize = sizeof(OS_XMA2WAVEFORMATEX) - sizeof(OS_WAVEFORMATEX);
        xma_fmt.NumStreams = 1;
        xma_fmt.SamplesEncoded = 4096u;
        xma_fmt.BytesPerBlock = 2048u;
        xma_fmt.BlockCount = static_cast<uint16_t>(xma_packets.size() / 2048u);

        OS_XAUDIO2_SEND_DESCRIPTOR send_desc{0, submix};
        OS_XAUDIO2_VOICE_SENDS sends{1, &send_desc};
        IXAudio2SourceVoice* xma_voice = nullptr;
        if (xa->CreateSourceVoice(&xma_voice, &xma_fmt.wfx, 0, 2.0f, nullptr, &sends) != OS_XAUDIO2_S_OK ||
            !xma_voice) {
            return Fail("XAudio2 CreateSourceVoice(XMA2)");
        }
        OS_XAUDIO2_BUFFER xma_buf{};
        xma_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        xma_buf.AudioBytes = static_cast<uint32_t>(xma_packets.size());
        xma_buf.pAudioData = xma_packets.data();
        if (xma_voice->SubmitSourceBuffer(&xma_buf) != OS_XAUDIO2_S_OK ||
            xma_voice->Start() != OS_XAUDIO2_S_OK) {
            return Fail("XAudio2 XMA2 submit/start");
        }

        constexpr uint32_t kMixFrames = 2048u;
        std::vector<float> xma_mix(kMixFrames * 2u, 0.0f);
        if (OpenScarlettXAudio2RenderMixForTest(xa, xma_mix.data(), kMixFrames) != OS_XAUDIO2_S_OK) {
            return Fail("XAudio2 render XMA2 mix");
        }
        double xma_energy = 0.0;
        for (float s : xma_mix) xma_energy += static_cast<double>(s) * static_cast<double>(s);
        if (xma_energy <= 1.0) {
            return Fail("XAudio2 XMA2 decoded energy too low");
        }

        // PCM16 voice
        OS_WAVEFORMATEX pcm_fmt{};
        pcm_fmt.wFormatTag = OS_WAVE_FORMAT_PCM;
        pcm_fmt.nChannels = 2;
        pcm_fmt.nSamplesPerSec = 48000;
        pcm_fmt.wBitsPerSample = 16;
        pcm_fmt.nBlockAlign = 4;
        pcm_fmt.nAvgBytesPerSec = 48000 * 4;

        IXAudio2SourceVoice* pcm_voice = nullptr;
        if (xa->CreateSourceVoice(&pcm_voice, &pcm_fmt) != OS_XAUDIO2_S_OK || !pcm_voice) {
            return Fail("XAudio2 CreateSourceVoice(PCM)");
        }
        std::vector<int16_t> pcm16(kMixFrames * 2u);
        for (uint32_t i = 0; i < kMixFrames; ++i) {
            const float v = 0.4f * std::sin(2.0f * 3.14159265f * 880.0f * static_cast<float>(i) / 48000.0f);
            const int16_t s = static_cast<int16_t>(v * 32767.0f);
            pcm16[i * 2u + 0u] = s;
            pcm16[i * 2u + 1u] = s;
        }
        OS_XAUDIO2_BUFFER pcm_buf{};
        pcm_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        pcm_buf.AudioBytes = static_cast<uint32_t>(pcm16.size() * sizeof(int16_t));
        pcm_buf.pAudioData = reinterpret_cast<const uint8_t*>(pcm16.data());
        if (pcm_voice->SubmitSourceBuffer(&pcm_buf) != OS_XAUDIO2_S_OK ||
            pcm_voice->Start() != OS_XAUDIO2_S_OK) {
            return Fail("XAudio2 PCM submit/start");
        }

        std::vector<float> comb_mix(kMixFrames * 2u, 0.0f);
        if (OpenScarlettXAudio2RenderMixForTest(xa, comb_mix.data(), kMixFrames) != OS_XAUDIO2_S_OK) {
            return Fail("XAudio2 render PCM+XMA2 mix");
        }
        double comb_energy = 0.0;
        for (float s : comb_mix) comb_energy += static_cast<double>(s) * static_cast<double>(s);
        if (comb_energy <= 1.0) {
            return Fail("XAudio2 combined energy too low");
        }

        pcm_voice->DestroyVoice();
        xma_voice->DestroyVoice();
        submix->DestroyVoice();
        master->DestroyVoice();
        xa->Release();

        // AcpHal.dll hardware ACP XMA2 decode verification
        AcpHalShapeContexts req_ctx{8, 4, 2, 2, nullptr};
        AcpHalShapeContexts alloc_ctx{};
        if (AcpHalAllocateShapeContexts(&req_ctx, &alloc_ctx) != ACPHAL_S_OK) {
            return Fail("AcpHalAllocateShapeContexts");
        }
        IAcpHal* acp = nullptr;
        if (AcpHalCreate(&acp) != ACPHAL_S_OK || !acp) {
            return Fail("AcpHalCreate");
        }
        AcpHalXmaContextDesc xdesc{};
        xdesc.sampleRate = 48000;
        xdesc.channels = 1;
        xdesc.blockSize = 512;
        xdesc.subframesPerFrame = 4;
        xdesc.quantBits = 12;
        if (acp->ConfigureXmaContext(0, &xdesc) != ACPHAL_S_OK) {
            return Fail("AcpHal ConfigureXmaContext");
        }
        std::vector<float> acp_pcm(8192, 0.0f);
        uint32_t samples_written = 0;
        AcpHalCommand cmd{};
        cmd.type = AcpHalCommand_XmaDecode;
        cmd.contextIndex = 0;
        cmd.inputData = xma_packets.data();
        cmd.inputBytes = static_cast<uint32_t>(xma_packets.size());
        cmd.outputFloatPcm = acp_pcm.data();
        cmd.outputCapacitySamples = static_cast<uint32_t>(acp_pcm.size());
        cmd.outSamplesWritten = &samples_written;
        cmd.xmaConfig = xdesc;
        if (acp->SubmitCommand(&cmd) != ACPHAL_S_OK || acp->WaitIdle() != ACPHAL_S_OK ||
            samples_written < 4096u) {
            return Fail("AcpHal XMA2 decode", samples_written);
        }
        double acp_energy = 0.0;
        for (uint32_t i = 0; i < samples_written; ++i) {
            acp_energy += static_cast<double>(acp_pcm[i]) * static_cast<double>(acp_pcm[i]);
        }
        if (acp_energy <= 1.0) {
            return Fail("AcpHal decoded PCM energy too low");
        }
        acp->Release();
        if (AcpHalReleaseShapeContexts(&alloc_ctx) != ACPHAL_S_OK) {
            return Fail("AcpHalReleaseShapeContexts");
        }
    }

    // =========================================================================
    // 7. xg_xs.dll (64KB_S swizzle/deswizzle bit-exact round-trip)
    // =========================================================================
    {
        XG_TEXTURE2D_DESC tdesc{};
        tdesc.Width = 256u;
        tdesc.Height = 256u;
        tdesc.MipLevels = 1u;
        tdesc.ArraySize = 1u;
        tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tdesc.SampleCount = 1u;
        tdesc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S;

        XG_RESOURCE_LAYOUT layout{};
        if (XGComputeMipLayout(&tdesc, &layout) != 0 || layout.SizeBytes == 0ull) {
            return Fail("XGComputeMipLayout");
        }
        const uint32_t row_pitch = 256u * 4u;
        const size_t linear_bytes = static_cast<size_t>(row_pitch) * 256u;
        std::vector<uint8_t> src(linear_bytes);
        for (size_t i = 0; i < linear_bytes; ++i) {
            src[i] = static_cast<uint8_t>((i * 131u + (i >> 4) * 17u) & 0xFFu);
        }
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xAAu);
        std::vector<uint8_t> dst(linear_bytes, 0x00u);

        if (XGTileSurface2D(&tdesc, 0, 0, src.data(), row_pitch, tiled.data(), tiled.size()) != 0 ||
            XGUntileSurface2D(&tdesc, 0, 0, tiled.data(), tiled.size(), dst.data(), row_pitch) != 0 ||
            std::memcmp(src.data(), dst.data(), linear_bytes) != 0) {
            return Fail("xg_xs 64KB_S swizzle/deswizzle round-trip");
        }
    }

    // =========================================================================
    // 8. winrt_xs.dll (RoInitialize + WindowsCreateString)
    // =========================================================================
    {
        if (RoInitialize(SX_RO_INIT_MULTITHREADED) != SX_WINRT_S_OK) {
            return Fail("winrt_xs RoInitialize");
        }
        const uint16_t raw_text[] = {'S', 'c', 'a', 'r', 'l', 'e', 't', 't', 'G', 'D', 'K', 0};
        SX_HSTRING hs = nullptr;
        if (WindowsCreateString(raw_text, 11u, &hs) != SX_WINRT_S_OK || !hs) {
            return Fail("winrt_xs WindowsCreateString");
        }
        uint32_t len = 0;
        const uint16_t* buf = WindowsGetStringRawBuffer(hs, &len);
        if (!buf || len != 11u || WindowsGetStringLen(hs) != 11u ||
            std::memcmp(buf, raw_text, 11u * sizeof(uint16_t)) != 0) {
            return Fail("winrt_xs HSTRING verification");
        }
        WindowsDeleteString(hs);
        RoUninitialize();
    }

    // =========================================================================
    // 9. Microsoft.Xbox.Services.GDK.C.dll (XSAPI)
    // =========================================================================
    {
        XblInitArgs xbl_init{};
        xbl_init.queue = nullptr;
        xbl_init.scid = "00000000-0000-0000-0000-00005ca03333";
        if (XblInitialize(&xbl_init) != XBL_S_OK) {
            return Fail("XblInitialize");
        }
        XblContextHandle xbl_ctx = nullptr;
        if (XblContextCreateHandle(user, &xbl_ctx) != XBL_S_OK || !xbl_ctx) {
            return Fail("XblContextCreateHandle");
        }

        XAsyncBlock ach_ab{};
        if (XblAchievementsUpdateAchievementAsync(
                xbl_ctx, xuid, "ach_sprint3_live", 100u, &ach_ab) != XBL_S_OK ||
            XAsyncGetStatus(&ach_ab, true) != XBL_S_OK) {
            return Fail("XblAchievementsUpdateAchievementAsync");
        }

        XAsyncBlock prof_ab{};
        XblUserProfile profile{};
        if (XblProfileGetUserProfileAsync(xbl_ctx, xuid, &prof_ab) != XBL_S_OK ||
            XblProfileGetUserProfileResult(&prof_ab, &profile) != XBL_S_OK ||
            profile.xboxUserId != xuid || profile.gamertag[0] == '\0') {
            return Fail("XblProfileGetUserProfileAsync/Result");
        }

        XblContextCloseHandle(xbl_ctx);
        XAsyncBlock cleanup_ab{};
        if (XblCleanupAsync(&cleanup_ab) != XBL_S_OK ||
            XAsyncGetStatus(&cleanup_ab, true) != XBL_S_OK) {
            return Fail("XblCleanupAsync");
        }
    }

    // =========================================================================
    // 10. d3d12_xs.dll (DeviceX probe in same process -> all 10 Xbox DLLs verified!)
    // =========================================================================
    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    {
        D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
        dp.Version = 1;
        ID3D12DeviceX* dev = nullptr;
        HRESULT hr = D3D12XboxCreateDevice(
            nullptr, &dp, IID_ID3D12DeviceX, reinterpret_cast<void**>(&dev));
        if (FAILED(hr) || !dev) {
            return Fail("d3d12_xs D3D12XboxCreateDevice", hr);
        }
        hr = dev->GetGpuHardwareInfoX(&gpu_info);
        D3D12XBOX_FRAME_PIPELINE_TOKEN tok = 0;
        HRESULT hr2 = dev->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &tok);
        dev->Release();
        if (FAILED(hr) || FAILED(hr2) || tok == 0) {
            return Fail("d3d12_xs GetGpuHardwareInfoX / WaitFrameEventX", hr);
        }
    }

    ::QueryPerformanceCounter(&qpc_end);
    const double total_ms = QpcMs(qpc_start, qpc_end, qpc_freq);

    char summary[512]{};
    const int summary_len = std::snprintf(
        summary, sizeof(summary),
        "sample=SystemCombo\n"
        "adapter=%s\n"
        "dlls_exercised=10\n"
        "xuid=%llu\n"
        "total_ms=%.3f\n",
        gpu_info.AdapterName,
        static_cast<unsigned long long>(xuid),
        total_ms);
    if (summary_len <= 0 ||
        !WriteWholeFileW(L"T:\\combo_summary.txt", summary, static_cast<size_t>(summary_len))) {
        return Fail("Write T:\\combo_summary.txt", ::GetLastError());
    }

    XUserCloseHandle(user);
    XGameRuntimeUninitialize();
    ::FreeLibrary(h_kernelx);

    std::printf(
        "[gdk_system_combo] PASS | All 10 Xbox DLLs verified live | Adapter=%s | %.2f ms\n",
        gpu_info.AdapterName, total_ms);
    std::fflush(stdout);
    return 0;
}
