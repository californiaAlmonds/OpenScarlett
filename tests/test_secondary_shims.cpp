#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi1_6.h>
#endif

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rt_bvh.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/middleware/bink2_decoder.h"
#include "openscarlett/middleware/ffx_gpu_fsr.h"
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/xsapi/xsapi_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

fs::path GetExeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return fs::path(buf).parent_path();
    }
#endif
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path GetScratchDir() {
#if defined(_WIN32)
    char buf[1024]{};
    const DWORD len = GetEnvironmentVariableA("TEMP", buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return fs::path(buf) / "test_secondary_shims";
    }
#endif
    return fs::path(OPENSCARLETT_BINARY_DIR) / "scratch_secondary_shims";
}

bool ProbeHardwareD3D12Gpu(std::string& out_desc) {
#if defined(_WIN32)
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    ID3D12Device* dev = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    openscarlett::D3D12AdapterInfo info{};
    const openscarlett::Status st =
        openscarlett::SelectD3D12AdapterAndCreateDevice(opts, &dev, &adapter, &info);
    if (dev) dev->Release();
    if (adapter) adapter->Release();
    if (st.ok() && !info.is_warp) {
        out_desc = info.description;
        return true;
    }
#endif
    out_desc = "none (WARP-only runner)";
    return false;
}

// ---------------------------------------------------------------------------
// 1. xgameruntime.dll — InitializeApiImpl / QueryApiImpl + OPENSCARLETT_TITLE_ID
// ---------------------------------------------------------------------------
const char kVtableAsyncId[] = "VtableCustomAsync";

struct VtableAsyncCtx {
    uint32_t in_val = 0;
    uint32_t out_val = 0;
    const IXgrAsyncApiVtable* vt = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL VtableAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<VtableAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->out_val = ctx->in_val * 5u + 11u;
            ctx->vt->XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(uint32_t)) return XGR_E_NOT_SUFFICIENT_BUFFER;
            std::memcpy(data->buffer, &ctx->out_val, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

void OPENSCARLETT_GDK_CALL SimpleQueueCb(void* ctx, bool canceled) {
    if (!canceled) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
}

bool OPENSCARLETT_GDK_CALL PackageChunkCb(
    void* ctx,
    const XPackageChunkSelector* /*sel*/,
    XPackageChunkAvailability avail) {
    if (avail == XPackageChunkAvailability_Ready) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

template <typename VtableT>
const VtableT* QueryVtableRequired(const XgrGuid& guid) {
    void* raw = nullptr;
    OS_REQUIRE(QueryApiImpl(&guid, &raw) == XGR_S_OK);
    OS_REQUIRE(raw != nullptr);
    return static_cast<const VtableT*>(raw);
}

void TestXGameRuntimeQueryApi() {
    std::printf("[1/10] xgameruntime.dll InitializeApiImpl / QueryApiImpl (8 GUIDs) + TitleId...\n");

    OS_REQUIRE(InitializeApiImpl() == XGR_S_OK);

    // Null argument checks
    void* raw = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_SYSTEM, nullptr) == XGR_E_POINTER);
    OS_REQUIRE(QueryApiImpl(nullptr, &raw) == XGR_E_INVALIDARG);
    OS_REQUIRE(raw == nullptr);

    // 1a. TaskQueue vtable
    const auto* tq_vt = QueryVtableRequired<IXgrTaskQueueApiVtable>(XGR_API_GUID_TASK_QUEUE);
    OS_REQUIRE(tq_vt->AddRef(tq_vt) >= 2u);
    void* qi_unknown = nullptr;
    OS_REQUIRE(tq_vt->QueryInterface(tq_vt, &XGR_IID_IUNKNOWN, &qi_unknown) == XGR_S_OK);
    OS_REQUIRE(qi_unknown == tq_vt);
    tq_vt->Release(tq_vt);
    tq_vt->Release(tq_vt);

    XTaskQueueHandle queue = nullptr;
    OS_REQUIRE(tq_vt->XTaskQueueCreate(
        XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &queue) == XGR_S_OK);
    std::atomic<int> cb_ran{0};
    OS_REQUIRE(tq_vt->XTaskQueueSubmitCallback(
        queue, XTaskQueuePort_Work, &cb_ran, &SimpleQueueCb) == XGR_S_OK);
    OS_REQUIRE(tq_vt->XTaskQueueDispatch(queue, XTaskQueuePort_Work, 100u));
    OS_REQUIRE(cb_ran.load() == 1);
    OS_REQUIRE(tq_vt->XTaskQueueTerminate(queue, true, nullptr, nullptr) == XGR_S_OK);
    tq_vt->XTaskQueueCloseHandle(queue);

    // 1b. Async vtable
    const auto* async_vt = QueryVtableRequired<IXgrAsyncApiVtable>(XGR_API_GUID_ASYNC);
    {
        XTaskQueueHandle aq = nullptr;
        OS_REQUIRE(tq_vt->XTaskQueueCreate(
            XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &aq) == XGR_S_OK);
        XAsyncBlock ab{};
        ab.queue = aq;
        auto* ctx = new VtableAsyncCtx{10u, 0u, async_vt};
        OS_REQUIRE(async_vt->XAsyncBegin(&ab, ctx, kVtableAsyncId, kVtableAsyncId, &VtableAsyncProvider) == XGR_S_OK);
        OS_REQUIRE(async_vt->XAsyncSchedule(&ab, 0u) == XGR_S_OK);
        OS_REQUIRE(async_vt->XAsyncGetStatus(&ab, true) == XGR_S_OK);
        size_t res_sz = 0;
        OS_REQUIRE(async_vt->XAsyncGetResultSize(&ab, &res_sz) == XGR_S_OK);
        OS_REQUIRE(res_sz == sizeof(uint32_t));
        uint32_t out_val = 0;
        size_t used = 0;
        OS_REQUIRE(async_vt->XAsyncGetResult(&ab, kVtableAsyncId, sizeof(out_val), &out_val, &used) == XGR_S_OK);
        OS_REQUIRE(out_val == 61u && used == sizeof(uint32_t));
        tq_vt->XTaskQueueTerminate(aq, true, nullptr, nullptr);
        tq_vt->XTaskQueueCloseHandle(aq);
    }

    // 1c. User vtable
    const auto* user_vt = QueryVtableRequired<IXgrUserApiVtable>(XGR_API_GUID_USER);
    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        OS_REQUIRE(user_vt->XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        OS_REQUIRE(user_vt->XUserAddResult(&ab, &user) == XGR_S_OK);
        OS_REQUIRE(user != nullptr);
        uint64_t xuid = 0;
        OS_REQUIRE(user_vt->XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0ull);
        char gt[64]{};
        size_t gt_used = 0;
        OS_REQUIRE(user_vt->XUserGetGamertag(
            user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
        OS_REQUIRE(gt_used > 1u && gt[0] != '\0');
        XUserState state = XUserState_SignedOut;
        OS_REQUIRE(user_vt->XUserGetState(user, &state) == XGR_S_OK && state == XUserState_SignedIn);
        bool has_priv = false;
        XUserPrivilegeDenyReason reason = XUserPrivilegeDenyReason_Unknown;
        OS_REQUIRE(user_vt->XUserCheckPrivilege(
            user, XUserPrivilegeOptions_None, XUserPrivilege_Multiplayer, &has_priv, &reason) == XGR_S_OK);
        OS_REQUIRE(has_priv && reason == XUserPrivilegeDenyReason_None);
    }

    // 1d. GameSave vtable
    const auto* gs_vt = QueryVtableRequired<IXgrGameSaveApiVtable>(XGR_API_GUID_GAME_SAVE);
    {
        XGameSaveProviderHandle prov = nullptr;
        OS_REQUIRE(gs_vt->XGameSaveInitializeProvider(
            user, "00000000-0000-0000-0000-00007E570001", false, &prov) == XGR_S_OK);
        XGameSaveContainerHandle cont = nullptr;
        OS_REQUIRE(gs_vt->XGameSaveCreateContainer(prov, "vtable_slot0", &cont) == XGR_S_OK);
        XGameSaveUpdateHandle upd = nullptr;
        OS_REQUIRE(gs_vt->XGameSaveCreateUpdate(cont, "Slot 0", &upd) == XGR_S_OK);
        const uint8_t payload[] = {0x53, 0x43, 0x41, 0x52, 0x4C, 0x45, 0x54, 0x54};
        OS_REQUIRE(gs_vt->XGameSaveSubmitBlobWrite(upd, "header", payload, sizeof(payload)) == XGR_S_OK);
        OS_REQUIRE(gs_vt->XGameSaveSubmitUpdate(upd) == XGR_S_OK);
        gs_vt->XGameSaveCloseUpdate(upd);

        uint8_t read_buf[512]{};
        const char* names[] = {"header"};
        uint32_t count = 1;
        auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf);
        OS_REQUIRE(gs_vt->XGameSaveReadBlobData(cont, names, &count, sizeof(read_buf), blobs) == XGR_S_OK);
        OS_REQUIRE(count == 1u && blobs[0].info.size == sizeof(payload));
        OS_REQUIRE(std::memcmp(blobs[0].data, payload, sizeof(payload)) == 0);
        gs_vt->XGameSaveCloseContainer(cont);
        OS_REQUIRE(gs_vt->XGameSaveDeleteContainer(prov, "vtable_slot0") == XGR_S_OK);
        gs_vt->XGameSaveCloseProvider(prov);
    }
    user_vt->XUserCloseHandle(user);

    // 1e. System vtable + OPENSCARLETT_TITLE_ID override
    const auto* sys_vt = QueryVtableRequired<IXgrSystemApiVtable>(XGR_API_GUID_SYSTEM);
    OS_REQUIRE(sys_vt->XSystemGetDeviceType() == XSystemDeviceType_XboxScarlettAnaconda);
    char cid[64]{};
    size_t cid_used = 0;
    OS_REQUIRE(sys_vt->XSystemGetConsoleId(sizeof(cid), cid, &cid_used) == XGR_S_OK && cid_used > 1u);
    const XSystemAnalyticsInfo ainfo = sys_vt->XSystemGetAnalyticsInfo();
    OS_REQUIRE(std::strcmp(ainfo.family, "Windows.Xbox") == 0);
    const XSystemRuntimeInfo rinfo = sys_vt->XSystemGetRuntimeInfo();
    OS_REQUIRE(rinfo.runtimeVersion.major == 10u);

#if defined(_WIN32)
    {
        char orig_tid[128]{};
        const DWORD orig_len = GetEnvironmentVariableA(
            "OPENSCARLETT_TITLE_ID", orig_tid, static_cast<DWORD>(sizeof(orig_tid)));
        const bool had_orig = (orig_len > 0 && orig_len < sizeof(orig_tid));

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", nullptr);
        uint32_t tid = 0;
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 0x5CA00001u);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "0x1234ABCD");
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 0x1234ABCDu);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "987654321");
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 987654321u);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "invalid_tid_xyz");
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 0x5CA00001u);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", had_orig ? orig_tid : nullptr);
    }
#endif

    // 1f. Mem vtable
    const auto* mem_vt = QueryVtableRequired<IXgrMemApiVtable>(XGR_API_GUID_MEM);
    void* p = mem_vt->XMemAlloc(256u, 0ull);
    OS_REQUIRE(p != nullptr);
    std::memset(p, 0x5A, 256u);
    mem_vt->XMemFree(p, 0ull);

    // 1g. Package vtable
    const auto* pkg_vt = QueryVtableRequired<IXgrPackageApiVtable>(XGR_API_GUID_PACKAGE);
    OS_REQUIRE(pkg_vt->XPackageIsPackagedProcess());
    char pkg_id[128]{};
    OS_REQUIRE(pkg_vt->XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id) == XGR_S_OK);
    std::atomic<int> ready_chunks{0};
    OS_REQUIRE(pkg_vt->XPackageEnumerateChunkAvailability(
        pkg_id, 0u, nullptr, &ready_chunks, &PackageChunkCb) == XGR_S_OK);
    OS_REQUIRE(ready_chunks.load() == 1);

    // 1h. Display vtable
    const auto* disp_vt = QueryVtableRequired<IXgrDisplayApiVtable>(XGR_API_GUID_DISPLAY);
    XDisplayHdrModeInfo hdr_info{0.05f, 300.0f, 250.0f};
    const XDisplayHdrModeResult hdr_res = disp_vt->XDisplayTryEnableHdrMode(1u, &hdr_info);
    if (hdr_res == XDisplayHdrModeResult_Enabled) {
        OS_REQUIRE(hdr_info.maxToneMapLuminance > 0.0f);
        OS_REQUIRE(hdr_info.maxToneMapLuminance >= hdr_info.maxFullFrameToneMapLuminance);
    } else {
        OS_REQUIRE(hdr_res == XDisplayHdrModeResult_Disabled);
        OS_REQUIRE(hdr_info.minToneMapLuminance == 0.0f &&
                   hdr_info.maxToneMapLuminance == 0.0f &&
                   hdr_info.maxFullFrameToneMapLuminance == 0.0f);
    }
    XDisplayTimeoutDeferralHandle deferral = nullptr;
    OS_REQUIRE(disp_vt->XDisplayTryAcquireTimeoutDeferral(&deferral) == XGR_S_OK && deferral != nullptr);
    disp_vt->XDisplayCloseTimeoutDeferralHandle(deferral);

    // Unknown GUID -> E_NOINTERFACE + NotImplemented telemetry with formatted GUID string
    constexpr XgrGuid kUnknownGuid = {
        0xDEADBEEFu, 0x1234u, 0x5678u, {0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44}};
    void* bad_vt = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
    OS_REQUIRE(QueryApiImpl(&kUnknownGuid, &bad_vt) == XGR_E_NOINTERFACE);
    OS_REQUIRE(bad_vt == nullptr);

    uint64_t unk_total = 0;
    uint64_t unk_not_impl = 0;
    OS_REQUIRE(XgrGetTelemetryCallCountForTest(
        "QueryApiImpl({DEADBEEF-1234-5678-9ABC-DEF011223344})", &unk_total, &unk_not_impl) == 1);
    OS_REQUIRE(unk_total >= 1ull && unk_not_impl >= 1ull);

    UninitializeApiImpl();
    std::printf("  PASS: 8/8 API GUID vtables dispatched, unknown GUID logged E_NOINTERFACE.\n");
}

// ---------------------------------------------------------------------------
// 2. pixevt.dll
// ---------------------------------------------------------------------------
void TestPixEvt() {
    std::printf("[2/10] pixevt.dll CPU/CommandList/CommandQueue events & counters...\n");
    const uint64_t before = PIXGetEventCountForTest();

    PIXBeginEventOnCpu(0xFF00FF00ull, "FrameUpdate");
    PIXSetMarkerOnCpu(0xFFFF0000ull, "PhysicsTick");
    PIXEndEventOnCpu();

    int dummy_cmdlist = 0;
    PIXBeginEventOnCommandList(&dummy_cmdlist, 0xFF0000FFull, "GBufferPass");
    PIXSetMarkerOnCommandList(&dummy_cmdlist, 0xFF00FFFFull, "DrawMesh");
    PIXEndEventOnCommandList(&dummy_cmdlist);

    int dummy_queue = 0;
    PIXBeginEventOnCommandQueue(&dummy_queue, 0xFFFFFF00ull, "PresentQueue");
    PIXSetMarkerOnCommandQueue(&dummy_queue, 0xFFFFFFFFull, "Flip");
    PIXEndEventOnCommandQueue(&dummy_queue);

    PIXNotifyWakeFromFenceSignal(&dummy_queue);
    PIXReportCounter(L"GpuFrameTimeMs", 4.25f);
    OS_REQUIRE(PIXGpuCaptureNextFrames(L"test_capture.wpix", 1u) == OS_SHIM_S_OK);
    OS_REQUIRE(PIXGpuCaptureNextFrames(L"bad.wpix", 0u) == OS_SHIM_E_INVALIDARG);

    const uint64_t after = PIXGetEventCountForTest();
    OS_REQUIRE(after - before == 12ull);
    std::printf("  PASS: pixevt recorded %llu events cleanly.\n",
                static_cast<unsigned long long>(after - before));
}

// ---------------------------------------------------------------------------
// 3. xmem.dll (forwarded to kernelx.dll)
// ---------------------------------------------------------------------------
void TestXMem() {
    std::printf("[3/10] xmem.dll aligned XMemAlloc/XMemSize/XMemFree -> kernelx.dll...\n");
#if defined(_WIN32)
    const fs::path xmem_path = GetExeDir() / "xmem.dll";
    HMODULE mod = LoadLibraryA(xmem_path.string().c_str());
    OS_REQUIRE(mod != nullptr);

    auto fn_alloc = reinterpret_cast<PFN_XMemAlloc>(GetProcAddress(mod, "XMemAlloc"));
    auto fn_alloc_def = reinterpret_cast<PFN_XMemAllocDefault>(GetProcAddress(mod, "XMemAllocDefault"));
    auto fn_free = reinterpret_cast<PFN_XMemFree>(GetProcAddress(mod, "XMemFree"));
    auto fn_free_def = reinterpret_cast<PFN_XMemFreeDefault>(GetProcAddress(mod, "XMemFreeDefault"));
    auto fn_size = reinterpret_cast<PFN_XMemSize>(GetProcAddress(mod, "XMemSize"));
    auto fn_size_def = reinterpret_cast<PFN_XMemSizeDefault>(GetProcAddress(mod, "XMemSizeDefault"));
    auto fn_get_aux = reinterpret_cast<PFN_XMemGetAuxiliaryTitleMemory>(
        GetProcAddress(mod, "XMemGetAuxiliaryTitleMemory"));
    auto fn_rel_aux = reinterpret_cast<PFN_XMemReleaseAuxiliaryTitleMemory>(
        GetProcAddress(mod, "XMemReleaseAuxiliaryTitleMemory"));
    auto fn_map = reinterpret_cast<PFN_XMemMapPhysicalPages>(
        GetProcAddress(mod, "XMemMapPhysicalPages"));
    auto fn_unmap = reinterpret_cast<PFN_XMemUnmapPhysicalPages>(
        GetProcAddress(mod, "XMemUnmapPhysicalPages"));
    auto fn_tel = reinterpret_cast<PFN_XMemGetTelemetryCallCountForTest>(
        GetProcAddress(mod, "XMemGetTelemetryCallCountForTest"));

    OS_REQUIRE(fn_alloc && fn_alloc_def && fn_free && fn_free_def);
    OS_REQUIRE(fn_size && fn_size_def && fn_get_aux && fn_rel_aux);
    OS_REQUIRE(fn_map && fn_unmap && fn_tel);

    // 4 KiB alignment (code 0xB in bits 24..27) + zero-init (bit 34)
    const uint64_t attrs_4k_zero = (0xBull << 24) | (1ull << 34);
    constexpr size_t kAllocSize = 8192u;
    void* ptr = fn_alloc(kAllocSize, attrs_4k_zero);
    OS_REQUIRE(ptr != nullptr);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(ptr) & 4095u) == 0u);

    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < kAllocSize; ++i) {
        OS_REQUIRE(bytes[i] == 0u);
    }
    const size_t reported = fn_size(ptr, attrs_4k_zero);
    OS_REQUIRE(reported >= kAllocSize);
    std::memset(bytes, 0xA5, kAllocSize);
    fn_free(ptr, attrs_4k_zero);

    void* def_ptr = fn_alloc_def(512u, 0ull);
    OS_REQUIRE(def_ptr != nullptr);
    OS_REQUIRE(fn_size_def(def_ptr, 0ull) >= 512u);
    fn_free_def(def_ptr, 0ull);

    size_t aux_actual = 0;
    void* aux = fn_get_aux(65536u, &aux_actual);
    OS_REQUIRE(aux != nullptr && aux_actual >= 65536u);
    OS_REQUIRE(fn_rel_aux(aux) != 0);

    uint64_t tot = 0, ok = 0;
    OS_REQUIRE(fn_tel("XMemAlloc", &tot, &ok) == 1 && tot >= 1ull && ok >= 1ull);
    OS_REQUIRE(fn_tel("XMemSize", &tot, &ok) == 1 && tot >= 1ull && ok >= 1ull);
    OS_REQUIRE(fn_tel("XMemFree", &tot, &ok) == 1 && tot >= 1ull && ok >= 1ull);

    FreeLibrary(mod);
    std::printf("  PASS: xmem.dll 4KiB zero-init alloc (%zu B) + aux mem forwarded to kernelx.dll.\n",
                reported);
#endif
}

// ---------------------------------------------------------------------------
// 4. xcrdapi.dll
// ---------------------------------------------------------------------------
void TestXCrdApi() {
    std::printf("[4/10] xcrdapi.dll diagnostics, breadcrumbs, error report & JSON flush...\n");
    OS_REQUIRE(XCrdInitialize(0x1u) == OS_SHIM_S_OK);
    OS_REQUIRE(XCrdSetBreadcrumb(0u, "InitStage", 0x1001ull) == OS_SHIM_S_OK);
    OS_REQUIRE(XCrdSetBreadcrumb(7u, "ShaderCompile", 42ull) == OS_SHIM_S_OK);

    char tag_small[4]{};
    uint64_t val = 0;
    OS_REQUIRE(XCrdGetBreadcrumb(7u, tag_small, sizeof(tag_small), &val) == OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    char tag_buf[64]{};
    OS_REQUIRE(XCrdGetBreadcrumb(7u, tag_buf, sizeof(tag_buf), &val) == OS_SHIM_S_OK);
    OS_REQUIRE(std::strcmp(tag_buf, "ShaderCompile") == 0 && val == 42ull);
    OS_REQUIRE(XCrdGetBreadcrumb(99u, tag_buf, sizeof(tag_buf), &val) == OS_SHIM_E_NOT_FOUND);

    OS_REQUIRE(XCrdReportError(0xC0001234u, "Renderer", "Synthetic diagnostic warning") == OS_SHIM_S_OK);

    XCrdDiagnosticInfo info{};
    info.structSize = sizeof(info);
    OS_REQUIRE(XCrdGetDiagnosticInfo(&info) == OS_SHIM_S_OK);
    OS_REQUIRE(info.initialized == 1u);
    OS_REQUIRE(info.breadcrumbCount >= 2u);
    OS_REQUIRE(info.errorReportCount >= 1u);
    OS_REQUIRE(info.lastErrorCode == 0xC0001234u);
    OS_REQUIRE(std::strcmp(info.lastErrorComponent, "Renderer") == 0);

    const fs::path diag_path = GetScratchDir() / "xcrd_diag.json";
    OS_REQUIRE(XCrdFlushDiagnostics(diag_path.string().c_str()) == OS_SHIM_S_OK);
    std::ifstream ifs(diag_path);
    OS_REQUIRE(ifs.good());
    std::string json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    OS_REQUIRE(json.find("ShaderCompile") != std::string::npos);
    OS_REQUIRE(json.find("Synthetic diagnostic warning") != std::string::npos);

    XCrdUninitialize();
    std::printf("  PASS: xcrdapi breadcrumbs=%u errors=%u flushed JSON (%zu B).\n",
                info.breadcrumbCount, info.errorReportCount, json.size());
}

// ---------------------------------------------------------------------------
// 5. libHttpClient.GDK.dll
// ---------------------------------------------------------------------------
void TestLibHttpClient() {
    std::printf("[5/10] libHttpClient.GDK.dll async HTTP (XAsync + offline loopback & mock)...\n");
    HCInitArgs init_args{};
    OS_REQUIRE(HCInitialize(&init_args) == OS_SHIM_S_OK);

    const char* ver = nullptr;
    OS_REQUIRE(HCGetLibVersion(&ver) == OS_SHIM_S_OK && ver && std::strlen(ver) > 0u);
    HCTraceSetTraceToDebugger(false);
    OS_REQUIRE(HCSettingsSetTraceLevel(HCTraceLevel_Warning) == OS_SHIM_S_OK);
    HCClearMockResponsesForTest();

    // 5a. Default offline loopback GET
    {
        HCCallHandle call = nullptr;
        OS_REQUIRE(HCHttpCallCreate(&call) == OS_SHIM_S_OK && call != nullptr);
        HCCallHandle dup = HCHttpCallDuplicateHandle(call);
        OS_REQUIRE(dup == call);

        OS_REQUIRE(HCHttpCallRequestSetUrl(
            call, "GET", "https://api.openscarlett.local/v1/status") == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallRequestSetHeader(call, "X-Title", "OpenScarlett", true) == OS_SHIM_S_OK);

        XAsyncBlock ab{};
        OS_REQUIRE(HCHttpCallPerformAsync(call, &ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);

        uint32_t status = 0;
        OS_REQUIRE(HCHttpCallResponseGetStatusCode(call, &status) == OS_SHIM_S_OK && status == 200u);

        const char* resp_str = nullptr;
        OS_REQUIRE(HCHttpCallResponseGetResponseString(call, &resp_str) == OS_SHIM_S_OK && resp_str);
        OS_REQUIRE(std::string(resp_str).find("api.openscarlett.local/v1/status") != std::string::npos);

        const char* ct = nullptr;
        OS_REQUIRE(HCHttpCallResponseGetHeader(call, "content-type", &ct) == OS_SHIM_S_OK);
        OS_REQUIRE(ct && std::strcmp(ct, "application/json") == 0);

        OS_REQUIRE(HCHttpCallCloseHandle(dup) == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallCloseHandle(call) == OS_SHIM_S_OK);
    }

    // 5b. Registered mock POST response
    {
        const std::string mock_body = R"({"accepted":true,"token":"OS-HTTP-200"})";
        OS_REQUIRE(HCAddMockResponseForTest(
            "POST",
            "https://api.openscarlett.local/v1/session",
            200u,
            "application/json",
            reinterpret_cast<const uint8_t*>(mock_body.data()),
            mock_body.size()) == OS_SHIM_S_OK);

        HCCallHandle call = nullptr;
        OS_REQUIRE(HCHttpCallCreate(&call) == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallRequestSetUrl(
            call, "POST", "https://api.openscarlett.local/v1/session") == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallRequestSetRequestBodyString(call, R"({"user":"tester"})") == OS_SHIM_S_OK);

        XAsyncBlock ab{};
        OS_REQUIRE(HCHttpCallPerformAsync(call, &ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);

        uint32_t status = 0;
        OS_REQUIRE(HCHttpCallResponseGetStatusCode(call, &status) == OS_SHIM_S_OK && status == 200u);
        size_t body_sz = 0;
        OS_REQUIRE(HCHttpCallResponseGetResponseBodyBytesSize(call, &body_sz) == OS_SHIM_S_OK);
        OS_REQUIRE(body_sz == mock_body.size());
        std::vector<uint8_t> buf(body_sz);
        size_t used = 0;
        OS_REQUIRE(HCHttpCallResponseGetResponseBodyBytes(call, buf.size(), buf.data(), &used) == OS_SHIM_S_OK);
        OS_REQUIRE(used == mock_body.size());
        OS_REQUIRE(std::memcmp(buf.data(), mock_body.data(), mock_body.size()) == 0);
        OS_REQUIRE(HCHttpCallCloseHandle(call) == OS_SHIM_S_OK);
    }

    // Async cleanup via XAsync
    {
        XAsyncBlock cleanup_ab{};
        OS_REQUIRE(HCCleanupAsync(&cleanup_ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&cleanup_ab, true) == XGR_S_OK);
    }
    std::printf("  PASS: libHttpClient.GDK async loopback + mock HTTP 200 verified via XAsync.\n");
}

// ---------------------------------------------------------------------------
// 6. XCurl.dll
// ---------------------------------------------------------------------------
size_t OS_SHIM_CDECL CurlCaptureWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, total);
    return total;
}

void TestXCurl() {
    std::printf("[6/10] XCurl.dll easy + multi offline transfer & callback verification...\n");
    OS_REQUIRE(curl_global_init(XCURL_GLOBAL_DEFAULT) == XCURLE_OK);
    OS_REQUIRE(curl_version() && std::strstr(curl_version(), "XCurl") != nullptr);
    OS_REQUIRE(std::strcmp(curl_easy_strerror(XCURLE_OK), "No error") == 0);

    // 6a. Easy transfer with write + header callbacks
    XCurlEasyHandle easy = curl_easy_init();
    OS_REQUIRE(easy != nullptr);
    XCurlSlist* hdrs = curl_slist_append(nullptr, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, "X-Client: OpenScarlett");
    OS_REQUIRE(hdrs != nullptr);

    std::string body;
    std::string resp_headers;
    int sentinel = 1234;
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://title.openscarlett.local/config") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_HTTPHEADER, hdrs) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEDATA, &body) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_HEADERFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_HEADERDATA, &resp_headers) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_PRIVATE, &sentinel) == XCURLE_OK);

    OS_REQUIRE(curl_easy_perform(easy) == XCURLE_OK);

    long http_code = 0;
    double dl_bytes = 0.0;
    const char* eff_url = nullptr;
    const char* ctype = nullptr;
    void* priv_out = nullptr;
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_RESPONSE_CODE, &http_code) == XCURLE_OK && http_code == 200L);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_SIZE_DOWNLOAD, &dl_bytes) == XCURLE_OK && dl_bytes > 0.0);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_EFFECTIVE_URL, &eff_url) == XCURLE_OK);
    OS_REQUIRE(eff_url && std::strcmp(eff_url, "https://title.openscarlett.local/config") == 0);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_CONTENT_TYPE, &ctype) == XCURLE_OK);
    OS_REQUIRE(ctype && std::strcmp(ctype, "application/json") == 0);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_PRIVATE, &priv_out) == XCURLE_OK && priv_out == &sentinel);
    OS_REQUIRE(body.find("openscarlett-xcurl") != std::string::npos);
    OS_REQUIRE(resp_headers.find("HTTP/1.1 200 OK") != std::string::npos);

    // 6b. Multi transfer cycle with 2 easy handles
    curl_easy_reset(easy);
    XCurlEasyHandle easy2 = curl_easy_init();
    OS_REQUIRE(easy2 != nullptr);

    std::string body1, body2;
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://title.openscarlett.local/m1") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEDATA, &body1) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_URL, "https://title.openscarlett.local/m2") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_POSTFIELDS, "hello=world") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_WRITEFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_WRITEDATA, &body2) == XCURLE_OK);

    XCurlMultiHandle multi = curl_multi_init();
    OS_REQUIRE(multi != nullptr);
    OS_REQUIRE(curl_multi_add_handle(multi, easy) == XCURLM_OK);
    OS_REQUIRE(curl_multi_add_handle(multi, easy2) == XCURLM_OK);

    int32_t running = -1;
    OS_REQUIRE(curl_multi_perform(multi, &running) == XCURLM_OK);
    OS_REQUIRE(running == 0);

    int done_count = 0;
    int32_t in_q = 0;
    while (XCurlMsg* msg = curl_multi_info_read(multi, &in_q)) {
        OS_REQUIRE(msg->msg == XCURLMSG_DONE);
        OS_REQUIRE(msg->data.result == XCURLE_OK);
        ++done_count;
    }
    OS_REQUIRE(done_count == 2);
    OS_REQUIRE(body1.find("/m1") != std::string::npos);
    OS_REQUIRE(body2.find("/m2") != std::string::npos);
    OS_REQUIRE(body2.find("\"postBytes\":11") != std::string::npos);

    OS_REQUIRE(curl_multi_remove_handle(multi, easy) == XCURLM_OK);
    OS_REQUIRE(curl_multi_remove_handle(multi, easy2) == XCURLM_OK);

    // 6c. curl_multi_poll / curl_multi_wait / curl_multi_wakeup / curl_multi_setopt / strerror
    OS_REQUIRE(curl_multi_setopt(multi, XCURLMOPT_MAXCONNECTS, 8L) == XCURLM_OK);
    OS_REQUIRE(std::strcmp(curl_multi_strerror(XCURLM_OK), "No error") == 0);
    OS_REQUIRE(std::strstr(curl_multi_strerror(XCURLM_BAD_HANDLE), "multi") != nullptr);

    // Poll with ready handle returns numfds >= 1 immediately
    OS_REQUIRE(curl_multi_add_handle(multi, easy) == XCURLM_OK);
    XCurlWaitFd extra{};
    extra.fd = 0;
    extra.events = XCURL_WAIT_POLLIN;
    extra.revents = 0x7FFF;
    int32_t numfds = -1;
    OS_REQUIRE(curl_multi_poll(multi, &extra, 1u, 100, &numfds) == XCURLM_OK);
    OS_REQUIRE(numfds >= 1 && extra.revents == 0);

    // Drain handle, then verify curl_multi_wait on idle returns immediately with numfds == 0
    OS_REQUIRE(curl_multi_perform(multi, &running) == XCURLM_OK);
    while (curl_multi_info_read(multi, &in_q) != nullptr) {}
    numfds = -1;
    OS_REQUIRE(curl_multi_wait(multi, nullptr, 0u, 100, &numfds) == XCURLM_OK);
    OS_REQUIRE(numfds == 0);

    // Verify curl_multi_wakeup wakes a sleeping curl_multi_poll cleanly from another thread
    std::thread waker([multi]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)curl_multi_wakeup(multi);
    });
    numfds = -1;
    OS_REQUIRE(curl_multi_poll(multi, nullptr, 0u, 200, &numfds) == XCURLM_OK);
    OS_REQUIRE(numfds == 0);
    waker.join();

    // Negative timeout returns bad function argument
    OS_REQUIRE(curl_multi_poll(multi, nullptr, 0u, -1, &numfds) == XCURLM_BAD_FUNCTION_ARGUMENT);

    OS_REQUIRE(curl_multi_remove_handle(multi, easy) == XCURLM_OK);
    OS_REQUIRE(curl_multi_cleanup(multi) == XCURLM_OK);

    // 6d. curl_easy_duphandle, escape/unescape, curl_free, curl_version_info, curl_share
    XCurlEasyHandle dup_easy = curl_easy_duphandle(easy);
    OS_REQUIRE(dup_easy != nullptr && dup_easy != easy);
    std::string dup_body;
    OS_REQUIRE(curl_easy_setopt(dup_easy, XCURLOPT_WRITEDATA, &dup_body) == XCURLE_OK);
    OS_REQUIRE(curl_easy_perform(dup_easy) == XCURLE_OK);
    OS_REQUIRE(dup_body.find("/m1") != std::string::npos);
    curl_easy_cleanup(dup_easy);

    char* escaped = curl_easy_escape(easy, "hello world/a+b", 0);
    OS_REQUIRE(escaped != nullptr && std::strcmp(escaped, "hello%20world%2Fa%2Bb") == 0);
    int32_t unesc_len = 0;
    char* unescaped = curl_easy_unescape(easy, escaped, 0, &unesc_len);
    OS_REQUIRE(unescaped != nullptr && unesc_len == 15 &&
               std::strcmp(unescaped, "hello world/a+b") == 0);
    curl_free(unescaped);
    curl_free(escaped);

    XCurlVersionInfoData* vinfo = curl_version_info(XCURLVERSION_NOW);
    OS_REQUIRE(vinfo != nullptr && vinfo->version != nullptr && vinfo->protocols != nullptr);

    XCurlShareHandle share = curl_share_init();
    OS_REQUIRE(share != nullptr);
    OS_REQUIRE(curl_share_setopt(share, 1, 2L) == XCURLSHE_OK);
    OS_REQUIRE(std::strcmp(curl_share_strerror(XCURLSHE_OK), "No error") == 0);
    OS_REQUIRE(curl_share_cleanup(share) == XCURLSHE_OK);

    // 6e. curl_url* parser / builder / dup round-trip
    XCurlUHandle u = curl_url();
    OS_REQUIRE(u != nullptr);
    OS_REQUIRE(curl_url_set(
        u, XCURLUPART_URL, "https://api.openscarlett.local:8443/v2/items?a=1#frag", 0u) == XCURLUE_OK);
    OS_REQUIRE(curl_url_set(u, XCURLUPART_QUERY, "b=2", XCURLU_APPENDQUERY) == XCURLUE_OK);

    XCurlUHandle u_dup = curl_url_dup(u);
    OS_REQUIRE(u_dup != nullptr && u_dup != u);
    char* host = nullptr;
    char* port = nullptr;
    char* query = nullptr;
    char* full = nullptr;
    OS_REQUIRE(curl_url_get(u_dup, XCURLUPART_HOST, &host, 0u) == XCURLUE_OK);
    OS_REQUIRE(host && std::strcmp(host, "api.openscarlett.local") == 0);
    OS_REQUIRE(curl_url_get(u_dup, XCURLUPART_PORT, &port, 0u) == XCURLUE_OK);
    OS_REQUIRE(port && std::strcmp(port, "8443") == 0);
    OS_REQUIRE(curl_url_get(u_dup, XCURLUPART_QUERY, &query, 0u) == XCURLUE_OK);
    OS_REQUIRE(query && std::strcmp(query, "a=1&b=2") == 0);
    OS_REQUIRE(curl_url_get(u_dup, XCURLUPART_URL, &full, 0u) == XCURLUE_OK);
    OS_REQUIRE(full &&
               std::strcmp(full, "https://api.openscarlett.local:8443/v2/items?a=1&b=2#frag") == 0);
    curl_free(host);
    curl_free(port);
    curl_free(query);
    curl_free(full);
    curl_url_cleanup(u_dup);
    curl_url_cleanup(u);

    // 6f. Xbox xcurl_* extensions
    xcurl_global_suspend();
    xcurl_global_resume();
    OS_REQUIRE(xcurl_global_set_request_limit(64u) == XCURLE_OK);

    // 6g. Dynamic LoadLibraryA + GetProcAddress check for all 16 real XSAPI-Thunks imports
#if defined(_WIN32)
    {
        const fs::path xcurl_path = GetExeDir() / "XCurl.dll";
        HMODULE xcurl_mod = LoadLibraryA(xcurl_path.string().c_str());
        OS_REQUIRE(xcurl_mod != nullptr);
        static constexpr const char* kRealXsapiThunksCurlImports[16] = {
            "curl_easy_setopt",
            "curl_slist_free_all",
            "curl_easy_strerror",
            "curl_slist_append",
            "curl_global_init",
            "curl_global_cleanup",
            "curl_easy_cleanup",
            "curl_easy_getinfo",
            "curl_multi_init",
            "curl_multi_add_handle",
            "curl_multi_remove_handle",
            "curl_easy_init",
            "curl_multi_poll",
            "curl_multi_perform",
            "curl_multi_cleanup",
            "curl_multi_info_read",
        };
        for (const char* sym : kRealXsapiThunksCurlImports) {
            OS_REQUIRE(GetProcAddress(xcurl_mod, sym) != nullptr);
        }
        FreeLibrary(xcurl_mod);
    }
#endif

    curl_easy_cleanup(easy2);
    curl_easy_cleanup(easy);
    curl_slist_free_all(hdrs);
    curl_global_cleanup();

    std::printf("  PASS: XCurl easy/multi/poll/wakeup/url/xcurl + 16/16 XSAPI-Thunks imports verified.\n");
}

// ---------------------------------------------------------------------------
// 7. Bink2Scarlett.dll (backed by openscarlett::Bink2Decoder)
// ---------------------------------------------------------------------------
std::vector<uint8_t> MakeSolidRgbaFrame(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
        pixels[i * 4u + 0u] = r;
        pixels[i * 4u + 1u] = g;
        pixels[i * 4u + 2u] = b;
        pixels[i * 4u + 3u] = 255u;
    }
    return pixels;
}

double MeanAbsRgbDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    OS_REQUIRE(a.size() == b.size() && (a.size() % 4u) == 0u);
    double sum = 0.0;
    const size_t px = a.size() / 4u;
    for (size_t i = 0; i < px; ++i) {
        sum += std::abs(static_cast<int>(a[i * 4u + 0u]) - static_cast<int>(b[i * 4u + 0u]));
        sum += std::abs(static_cast<int>(a[i * 4u + 1u]) - static_cast<int>(b[i * 4u + 1u]));
        sum += std::abs(static_cast<int>(a[i * 4u + 2u]) - static_cast<int>(b[i * 4u + 2u]));
    }
    return sum / static_cast<double>(px * 3u);
}

void TestBink2Scarlett() {
    std::printf("[7/10] Bink2Scarlett.dll KB2i decode, RGBA/BGRA copy & BinkGoto seek...\n");
    constexpr uint32_t kW = 32u;
    constexpr uint32_t kH = 32u;

    std::vector<std::vector<uint8_t>> src_frames = {
        MakeSolidRgbaFrame(kW, kH, 220u, 40u, 50u),
        MakeSolidRgbaFrame(kW, kH, 40u, 210u, 60u),
        MakeSolidRgbaFrame(kW, kH, 50u, 70u, 230u),
        MakeSolidRgbaFrame(kW, kH, 235u, 220u, 45u),
    };

    openscarlett::Bink2EncodeParams enc{};
    enc.width = kW;
    enc.height = kH;
    enc.fps_num = 60u;
    enc.fps_den = 1u;
    enc.fourcc = "KB2i";
    enc.chroma_format = openscarlett::Bink2ChromaFormat::YCbCr420;
    enc.keyframe_interval = 2u;

    std::vector<uint8_t> container;
    OS_REQUIRE(openscarlett::Bink2EncodeTestStream(src_frames, enc, container).ok());
    OS_REQUIRE(!container.empty());

    HBink bink = BinkOpenBuffer(container.data(), container.size(), 0u);
    OS_REQUIRE(bink != nullptr);
    OS_REQUIRE(bink->Width == kW && bink->Height == kH && bink->Frames == 4u && bink->FrameNum == 1u);
    OS_REQUIRE(std::strlen(BinkGetError()) == 0u);

    BinkSummary sum{};
    BinkGetSummary(bink, &sum);
    OS_REQUIRE(sum.IsBink2 == 1u && std::strcmp(sum.FourCC, "KB2i") == 0);
    OS_REQUIRE(sum.FrameRate == 60u && sum.FrameRateDiv == 1u);

    BinkSetSoundOnOff(bink, 0);
    OS_REQUIRE(bink->SoundOn == 0u);
    OS_REQUIRE(BinkPause(bink, 1) == 1 && BinkWait(bink) == 1);
    OS_REQUIRE(BinkPause(bink, 0) == 0 && BinkWait(bink) == 0);

    std::vector<uint8_t> decoded(static_cast<size_t>(kW) * kH * 4u);
    double worst_mae = 0.0;
    for (uint32_t f = 0; f < 4u; ++f) {
        OS_REQUIRE(bink->FrameNum == f + 1u);
        OS_REQUIRE(BinkDoFrame(bink) == 0);
        OS_REQUIRE(BinkCopyToBuffer(
            bink, decoded.data(), static_cast<int32_t>(kW * 4u), kH, 0u, 0u, OS_BINKSURFACE32RGBA) == 0);
        const double mae = MeanAbsRgbDiff(decoded, src_frames[f]);
        if (mae > worst_mae) worst_mae = mae;
        OS_REQUIRE(mae < 8.0);
        BinkNextFrame(bink);
    }

    // Verify BinkGoto seek to frame 3 + BGRA channel swap check
    BinkGoto(bink, 3u, 0u);
    OS_REQUIRE(bink->FrameNum == 3u);
    OS_REQUIRE(BinkCopyToBuffer(
        bink, decoded.data(), static_cast<int32_t>(kW * 4u), kH, 0u, 0u, OS_BINKSURFACE32RGBA) == 0);
    OS_REQUIRE(MeanAbsRgbDiff(decoded, src_frames[2]) < 8.0);

    std::vector<uint8_t> decoded_bgra(static_cast<size_t>(kW) * kH * 4u);
    OS_REQUIRE(BinkCopyToBuffer(
        bink, decoded_bgra.data(), static_cast<int32_t>(kW * 4u), kH, 0u, 0u, OS_BINKSURFACE32) == 0);
    OS_REQUIRE(decoded_bgra[0] == decoded[2] && decoded_bgra[2] == decoded[0]);

    BinkClose(bink);
    std::printf("  PASS: Bink2Scarlett decoded 4 KB2i frames + seek (worst MAE=%.2f LSB).\n", worst_mae);
}

// ---------------------------------------------------------------------------
// 8. amd_fidelityfx_loader_xbox.dll + amd_fidelityfx_upscaler_xbox.dll
// ---------------------------------------------------------------------------
double ComputeMeanEdgeGradient(const uint8_t* rgba8, uint32_t w, uint32_t h) {
    double sum = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 1u; y + 1u < h; ++y) {
        for (uint32_t x = 1u; x + 1u < w; ++x) {
            const size_t c = (static_cast<size_t>(y) * w + x) * 4u;
            const size_t r = (static_cast<size_t>(y) * w + (x + 1u)) * 4u;
            const size_t b = (static_cast<size_t>(y + 1u) * w + x) * 4u;
            const int gx = std::abs(static_cast<int>(rgba8[r]) - static_cast<int>(rgba8[c]));
            const int gy = std::abs(static_cast<int>(rgba8[b]) - static_cast<int>(rgba8[c]));
            sum += static_cast<double>(gx + gy);
            ++count;
        }
    }
    return count > 0 ? (sum / static_cast<double>(count)) : 0.0;
}

void RunFfxLoaderSession(
    bool prefer_warp,
    const std::vector<uint8_t>& input,
    uint32_t in_w,
    uint32_t in_h,
    uint32_t out_w,
    uint32_t out_h,
    std::vector<uint8_t>& out_pixels,
    FfxUpscalerDispatchResult& out_res) {
    FfxCreateContextDescUpscale create_desc{};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create_desc.flags = prefer_warp
                            ? (FFX_UPSCALER_FLAG_PREFER_WARP | FFX_UPSCALER_FLAG_ENABLE_RCAS)
                            : FFX_UPSCALER_FLAG_ENABLE_RCAS;
    create_desc.maxRenderWidth = in_w;
    create_desc.maxRenderHeight = in_h;
    create_desc.maxUpscaleWidth = out_w;
    create_desc.maxUpscaleHeight = out_h;
    create_desc.defaultSharpness = 0.8f;

    FfxContext ctx = nullptr;
    OS_REQUIRE(ffxCreateContext(&ctx, &create_desc.header, nullptr) == FFX_API_RETURN_OK);
    OS_REQUIRE(ctx != nullptr);

    // Configure sharpness via FidelityFX SDK 2.0 key-value descriptor
    float sharpness = 0.85f;
    FfxConfigureDescUpscaleKeyValue cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
    cfg.key = FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS;
    cfg.ptr = &sharpness;
    OS_REQUIRE(ffxConfigure(&ctx, &cfg.header) == FFX_API_RETURN_OK);

    // Query adapter info
    FfxQueryDescUpscaleGetAdapterInfo adapter_q{};
    adapter_q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETADAPTERINFO;
    OS_REQUIRE(ffxQuery(&ctx, &adapter_q.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(adapter_q.adapterDescription[0] != '\0');
    if (prefer_warp) {
        OS_REQUIRE(adapter_q.outIsWarp == 1u);
    }

    out_pixels.assign(static_cast<size_t>(out_w) * out_h * 4u, 0u);
    std::memset(&out_res, 0, sizeof(out_res));

    FfxDispatchDescUpscale disp{};
    disp.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    disp.inputRgba8 = input.data();
    disp.inputWidth = in_w;
    disp.inputHeight = in_h;
    disp.outputRgba8 = out_pixels.data();
    disp.outputBufferBytes = out_pixels.size();
    disp.outputWidth = out_w;
    disp.outputHeight = out_h;
    disp.sharpness = -1.0f;  // use configured sharpness
    disp.enableRcas = 0xFFFFFFFFu;
    disp.outResult = &out_res;

    OS_REQUIRE(ffxDispatch(&ctx, &disp.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(out_res.usedGpuCompute == 1u);
    OS_REQUIRE(out_res.outputFnv1aHash != 0ull);
    OS_REQUIRE(out_res.easuGroupsX > 0u && out_res.easuGroupsY > 0u);
    OS_REQUIRE(out_res.rcasGroupsX > 0u && out_res.rcasGroupsY > 0u);

    OS_REQUIRE(ffxDestroyContext(&ctx, nullptr) == FFX_API_RETURN_OK);
    OS_REQUIRE(ctx == nullptr);
}

void TestFidelityFxShims(bool has_hw_gpu) {
    std::printf("[8/10] amd_fidelityfx_loader_xbox.dll + upscaler_xbox.dll live GPU FSR1...\n");

    // Provider version & render resolution queries (context-free)
    uint64_t ver_count = 1ull;
    uint64_t ver_ids[1]{};
    const char* ver_names[1]{};
    FfxQueryDescGetVersions vq{};
    vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    vq.outputCount = &ver_count;
    vq.versionIds = ver_ids;
    vq.versionNames = ver_names;
    OS_REQUIRE(ffxQuery(nullptr, &vq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(ver_count == 1ull && ver_ids[0] == ffxUpscalerGetVersion() && ver_names[0] != nullptr);

    uint32_t ren_w = 0, ren_h = 0;
    FfxQueryDescUpscaleGetRenderResolutionFromQualityMode rq{};
    rq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    rq.displayWidth = 1920u;
    rq.displayHeight = 1080u;
    rq.qualityMode = FFX_UPSCALER_QUALITY_QUALITY;
    rq.pOutRenderWidth = &ren_w;
    rq.pOutRenderHeight = &ren_h;
    OS_REQUIRE(ffxQuery(nullptr, &rq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(ren_w == 1280u && ren_h == 720u);

    constexpr uint32_t kInW = 64u, kInH = 64u, kOutW = 128u, kOutH = 128u;
    std::vector<uint8_t> input(static_cast<size_t>(kInW) * kInH * 4u);
    for (uint32_t y = 0; y < kInH; ++y) {
        for (uint32_t x = 0; x < kInW; ++x) {
            uint8_t* p = &input[(static_cast<size_t>(y) * kInW + x) * 4u];
            const bool checker = (((x / 8u) + (y / 8u)) & 1u) != 0u;
            const bool diag = (x + y) >= kInW;
            p[0] = diag ? (checker ? 240u : 20u) : (checker ? 230u : 15u);
            p[1] = diag ? (checker ? 240u : 30u) : (checker ? 60u : 220u);
            p[2] = diag ? (checker ? 240u : 40u) : (checker ? 30u : 230u);
            p[3] = 255u;
        }
    }

    std::vector<uint8_t> warp_out;
    FfxUpscalerDispatchResult warp_res{};
    RunFfxLoaderSession(true, input, kInW, kInH, kOutW, kOutH, warp_out, warp_res);
    OS_REQUIRE(warp_res.isWarp == 1u);

    openscarlett::FrameBufferRgba8 bil_fb{};
    OS_REQUIRE(openscarlett::FfxGpuFsrPipeline::UpscaleBilinearCpu(
        input.data(), kInW, kInH, kOutW, kOutH, bil_fb).ok());
    const double bil_grad = ComputeMeanEdgeGradient(bil_fb.pixels.data(), kOutW, kOutH);
    const double warp_grad = ComputeMeanEdgeGradient(warp_out.data(), kOutW, kOutH);
    OS_REQUIRE(warp_grad > bil_grad);

    if (has_hw_gpu) {
        std::vector<uint8_t> hw_out;
        FfxUpscalerDispatchResult hw_res{};
        RunFfxLoaderSession(false, input, kInW, kInH, kOutW, kOutH, hw_out, hw_res);
        OS_REQUIRE(hw_res.isWarp == 0u);
        int max_diff = 0;
        for (size_t i = 0; i < hw_out.size(); ++i) {
            const int d = std::abs(static_cast<int>(hw_out[i]) - static_cast<int>(warp_out[i]));
            if (d > max_diff) max_diff = d;
        }
        OS_REQUIRE(max_diff <= 2);
        std::printf("  PASS: FFX HW='%s' FNV1a=0x%016llX | WARP='%s' FNV1a=0x%016llX | EdgeGrad=%.2f>%.2f | MaxDiff=%d LSB.\n",
                    hw_res.adapterDescription,
                    static_cast<unsigned long long>(hw_res.outputFnv1aHash),
                    warp_res.adapterDescription,
                    static_cast<unsigned long long>(warp_res.outputFnv1aHash),
                    warp_grad, bil_grad, max_diff);
    } else {
        std::printf("  PASS: FFX WARP='%s' FNV1a=0x%016llX | EdgeGrad=%.2f>%.2f (WARP-only CI).\n",
                    warp_res.adapterDescription,
                    static_cast<unsigned long long>(warp_res.outputFnv1aHash),
                    warp_grad, bil_grad);
    }
}

// ---------------------------------------------------------------------------
// 9. XboxBvhBuilder.dll (backed by DxrHardwareEngine + BvhAccelerationStructure)
// ---------------------------------------------------------------------------
std::vector<XboxBvhTriangle> ConvertRtTriangles(const std::vector<openscarlett::RtTriangle>& src) {
    std::vector<XboxBvhTriangle> out;
    out.reserve(src.size());
    for (const auto& t : src) {
        XboxBvhTriangle b{};
        b.v0[0] = t.v0.x; b.v0[1] = t.v0.y; b.v0[2] = t.v0.z;
        b.v1[0] = t.v1.x; b.v1[1] = t.v1.y; b.v1[2] = t.v1.z;
        b.v2[0] = t.v2.x; b.v2[1] = t.v2.y; b.v2[2] = t.v2.z;
        b.color[0] = t.color.x; b.color[1] = t.color.y; b.color[2] = t.color.z;
        b.reflectivity = t.reflectivity;
        out.push_back(b);
    }
    return out;
}

struct BvhSessionResult {
    XboxBvhCapabilities caps{};
    XboxBvhDispatchStats initial_stats{};
    XboxBvhDispatchStats updated_stats{};
    std::vector<uint8_t> initial_pixels;
};

BvhSessionResult RunXboxBvhSession(bool prefer_warp) {
    BvhSessionResult res{};

    XboxBvhCreateDesc cdesc{};
    cdesc.preferWarp = prefer_warp ? 1u : 0u;
    XboxBvhBuilderHandle builder = nullptr;
    OS_REQUIRE(XboxBvhCreateBuilder(&cdesc, &builder) == OS_SHIM_S_OK && builder != nullptr);

    OS_REQUIRE(XboxBvhGetCapabilities(builder, &res.caps) == OS_SHIM_S_OK);
    OS_REQUIRE(res.caps.adapterDescription[0] != '\0');
    if (prefer_warp) {
        OS_REQUIRE(res.caps.isWarp == 1u);
    }

    const auto room_src = openscarlett::BuildDefaultRtTestScene();
    const auto room_tris = ConvertRtTriangles(room_src);
    auto cube_src = openscarlett::BuildUnitCubeMesh({0.15f, 0.78f, 0.95f}, 0.35f);
    auto cube_tris = ConvertRtTriangles(cube_src);

    XboxBvhPrebuildInputs pre_in{};
    pre_in.asType = XBOX_BVH_AS_TYPE_BOTTOM_LEVEL;
    pre_in.buildFlags = XBOX_BVH_BUILD_ALLOW_UPDATE | XBOX_BVH_BUILD_PREFER_FAST_TRACE;
    pre_in.primitiveCount = static_cast<uint32_t>(room_tris.size());
    pre_in.geometryCount = 1u;
    pre_in.vertexStride = 12u;
    pre_in.isOpaque = 1u;
    XboxBvhPrebuildInfo pre_info{};
    OS_REQUIRE(XboxBvhGetPrebuildInfo(builder, &pre_in, &pre_info) == OS_SHIM_S_OK);
    OS_REQUIRE(pre_info.resultDataMaxSizeBytes > 0ull && (pre_info.resultDataMaxSizeBytes % 256ull) == 0ull);
    OS_REQUIRE(pre_info.scratchDataSizeBytes > 0ull && (pre_info.scratchDataSizeBytes % 256ull) == 0ull);

    uint64_t room_blas = 0, cube_blas = 0;
    OS_REQUIRE(XboxBvhBuildBlas(
        builder, room_tris.data(), static_cast<uint32_t>(room_tris.size()),
        XBOX_BVH_BUILD_PREFER_FAST_TRACE, &room_blas) == OS_SHIM_S_OK && room_blas != 0ull);
    OS_REQUIRE(XboxBvhBuildBlas(
        builder, cube_tris.data(), static_cast<uint32_t>(cube_tris.size()),
        XBOX_BVH_BUILD_ALLOW_UPDATE | XBOX_BVH_BUILD_PREFER_FAST_TRACE, &cube_blas) == OS_SHIM_S_OK &&
        cube_blas != 0ull);

    XboxBvhInstanceDesc insts[2]{};
    insts[0].blasId = room_blas;
    openscarlett::MakeIdentity3x4(insts[0].transform);
    insts[0].instanceId = 0u;
    insts[0].instanceMask = 0xFFu;

    insts[1].blasId = cube_blas;
    openscarlett::MakeScaleTranslation3x4(0.7f, 0.7f, 0.7f, -0.9f, 0.5f, 0.3f, insts[1].transform);
    insts[1].instanceId = 1u;
    insts[1].instanceMask = 0xFFu;

    uint64_t tlas_id = 0;
    OS_REQUIRE(XboxBvhBuildTlas(
        builder, insts, 2u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas_id) == OS_SHIM_S_OK && tlas_id != 0ull);

    // Single-ray trace directly on TLAS
    XboxBvhRay ray{};
    ray.origin[0] = 0.0f; ray.origin[1] = 1.0f; ray.origin[2] = -3.5f;
    ray.direction[0] = 0.0f; ray.direction[1] = -0.1f; ray.direction[2] = 0.994987f;
    ray.tMin = 0.001f;
    ray.tMax = 100.0f;
    XboxBvhHit hit{};
    OS_REQUIRE(XboxBvhTraceRay(builder, tlas_id, &ray, &hit) == OS_SHIM_S_OK);
    OS_REQUIRE(hit.hit == 1u && hit.t > 0.0f);

    // Full raytraced frame dispatch
    constexpr uint32_t kW = 80u, kH = 60u;
    XboxBvhDispatchDesc ddesc{};
    ddesc.width = kW;
    ddesc.height = kH;
    ddesc.maxBounces = 2u;
    ddesc.cameraPos[0] = 0.0f; ddesc.cameraPos[1] = 1.0f; ddesc.cameraPos[2] = -3.5f;
    ddesc.cameraLookAt[0] = 0.0f; ddesc.cameraLookAt[1] = 0.5f; ddesc.cameraLookAt[2] = 0.0f;
    ddesc.lightPos[0] = 1.5f; ddesc.lightPos[1] = 3.0f; ddesc.lightPos[2] = -1.5f;

    res.initial_pixels.assign(static_cast<size_t>(kW) * kH * 4u, 0u);
    OS_REQUIRE(XboxBvhDispatchRays(
        builder, tlas_id, &ddesc, res.initial_pixels.data(), res.initial_pixels.size(), &res.initial_stats) ==
        OS_SHIM_S_OK);
    OS_REQUIRE(res.initial_stats.primaryRays == static_cast<uint64_t>(kW) * kH);
    OS_REQUIRE(res.initial_stats.primaryHits > res.initial_stats.primaryRays / 2u);
    OS_REQUIRE(res.initial_stats.frameHash != 0ull);

    // Update cube BLAS color & vertex offset, rebuild TLAS, verify frame hash changes!
    for (auto& t : cube_tris) {
        t.v0[1] += 0.35f; t.v1[1] += 0.35f; t.v2[1] += 0.35f;
        t.color[0] = 0.95f; t.color[1] = 0.20f; t.color[2] = 0.15f;
    }
    OS_REQUIRE(XboxBvhUpdateBlas(
        builder, cube_blas, cube_tris.data(), static_cast<uint32_t>(cube_tris.size())) == OS_SHIM_S_OK);
    uint64_t tlas_updated = 0;
    OS_REQUIRE(XboxBvhBuildTlas(
        builder, insts, 2u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas_updated) == OS_SHIM_S_OK);
    std::vector<uint8_t> updated_pixels(static_cast<size_t>(kW) * kH * 4u, 0u);
    OS_REQUIRE(XboxBvhDispatchRays(
        builder, tlas_updated, &ddesc, updated_pixels.data(), updated_pixels.size(), &res.updated_stats) ==
        OS_SHIM_S_OK);
    OS_REQUIRE(res.updated_stats.frameHash != 0ull);
    OS_REQUIRE(res.updated_stats.frameHash != res.initial_stats.frameHash);

    XboxBvhDestroyBuilder(builder);
    return res;
}

void TestXboxBvhBuilder(bool has_hw_gpu) {
    std::printf("[9/10] XboxBvhBuilder.dll BLAS/TLAS build, update, TraceRay & DispatchRays...\n");
    const BvhSessionResult warp = RunXboxBvhSession(true);
    const double hit_pct =
        100.0 * static_cast<double>(warp.initial_stats.primaryHits) /
        static_cast<double>(warp.initial_stats.primaryRays);

    if (has_hw_gpu) {
        const BvhSessionResult hw = RunXboxBvhSession(false);
        OS_REQUIRE(hw.caps.isWarp == 0u);
        OS_REQUIRE(hw.initial_stats.frameHash == warp.initial_stats.frameHash);
        OS_REQUIRE(hw.updated_stats.frameHash == warp.updated_stats.frameHash);
        OS_REQUIRE(hw.initial_pixels == warp.initial_pixels);
        std::printf("  PASS: BVH HW='%s' (tier=%u) == WARP bit-exact hash=0x%016llX (hit=%.1f%%, updated=0x%016llX).\n",
                    hw.caps.adapterDescription,
                    hw.caps.hardwareTier,
                    static_cast<unsigned long long>(hw.initial_stats.frameHash),
                    hit_pct,
                    static_cast<unsigned long long>(hw.updated_stats.frameHash));
    } else {
        std::printf("  PASS: BVH WARP='%s' hash=0x%016llX (hit=%.1f%%, updated=0x%016llX, WARP-only CI).\n",
                    warp.caps.adapterDescription,
                    static_cast<unsigned long long>(warp.initial_stats.frameHash),
                    hit_pct,
                    static_cast<unsigned long long>(warp.updated_stats.frameHash));
    }
}

// ---------------------------------------------------------------------------
// 10. XSAPI (Microsoft.Xbox.Services.GDK.C.dll) Dynamic Runtime Parity Suite
// ---------------------------------------------------------------------------
#if defined(_WIN32)
void OPENSCARLETT_XSAPI_CALL TestServiceCallRoutedCb(
    XblServiceCallRoutedArgs args, void* context) {
    if (args.httpStatus == 200u && context) {
        static_cast<std::atomic<int>*>(context)->fetch_add(1, std::memory_order_relaxed);
    }
}

void OPENSCARLETT_XSAPI_CALL TestStatChangedCb(
    XblStatisticChangeEventArgs args, void* context) {
    if (args.latestStatistic.statisticName && context) {
        static_cast<std::atomic<int>*>(context)->fetch_add(1, std::memory_order_relaxed);
    }
}

void OPENSCARLETT_XSAPI_CALL TestSocialChangedCb(
    const XblSocialRelationshipChangeEventArgs* /*args*/, void* /*context*/) {}

void OPENSCARLETT_XSAPI_CALL TestDevicePresenceCb(
    void* /*context*/, uint64_t /*xuid*/, XblPresenceDeviceType /*dev*/, bool /*loggedOn*/) {}

void OPENSCARLETT_XSAPI_CALL TestTitlePresenceCb(
    void* /*context*/, uint64_t /*xuid*/, uint32_t /*titleId*/, XblPresenceTitleState /*state*/) {}

template <typename FnT>
FnT ResolveXsapi(HMODULE mod, const char* name) {
    FARPROC p = GetProcAddress(mod, name);
    OS_REQUIRE(p != nullptr);
    return reinterpret_cast<FnT>(p);
}

void TestXsapiDynamicParity() {
    std::printf("[10/11] Microsoft.Xbox.Services.GDK.C.dll dynamic parity & end-to-end tests...\n");
    const fs::path xsapi_path = GetExeDir() / "Microsoft.Xbox.Services.GDK.C.dll";
    HMODULE mod = LoadLibraryA(xsapi_path.string().c_str());
    OS_REQUIRE(mod != nullptr);

#define LOAD_XBL(fn) auto p_##fn = ResolveXsapi<decltype(fn)*>(mod, #fn)

    // Core & Context & Settings
    LOAD_XBL(XblWrapper_XblInitialize);
    LOAD_XBL(XblCleanup);
    LOAD_XBL(XblGetAsyncQueue);
    LOAD_XBL(XblGetScid);
    LOAD_XBL(XblGetErrorCondition);
    LOAD_XBL(XblMemSetFunctions);
    LOAD_XBL(XblMemGetFunctions);
    LOAD_XBL(XblAddServiceCallRoutedHandler);
    LOAD_XBL(XblRemoveServiceCallRoutedHandler);
    LOAD_XBL(XblSetOverrideConfiguration);
    LOAD_XBL(XblDisableAssertsForXboxLiveThrottlingInDevSandboxes);
    LOAD_XBL(XblEventsWriteInGameEvent);
    LOAD_XBL(XblContextCreateHandle);
    LOAD_XBL(XblContextCloseHandle);
    LOAD_XBL(XblContextGetXboxUserId);
    LOAD_XBL(XblContextSettingsGetHttpRetryDelay);
    LOAD_XBL(XblContextSettingsSetHttpRetryDelay);
    LOAD_XBL(XblContextSettingsGetHttpTimeoutWindow);
    LOAD_XBL(XblContextSettingsSetHttpTimeoutWindow);
    LOAD_XBL(XblContextSettingsGetLongHttpTimeout);
    LOAD_XBL(XblContextSettingsSetLongHttpTimeout);
    LOAD_XBL(XblContextSettingsGetUseCrossPlatformQosServers);
    LOAD_XBL(XblContextSettingsSetUseCrossPlatformQosServers);
    LOAD_XBL(XblContextSettingsGetWebsocketTimeoutWindow);
    LOAD_XBL(XblContextSettingsSetWebsocketTimeoutWindow);

    // Achievements
    LOAD_XBL(XblAchievementsUpdateAchievementAsync);
    LOAD_XBL(XblAchievementsGetAchievementAsync);
    LOAD_XBL(XblAchievementsGetAchievementResult);
    LOAD_XBL(XblAchievementsGetAchievementsForTitleIdAsync);
    LOAD_XBL(XblAchievementsGetAchievementsForTitleIdResult);
    LOAD_XBL(XblAchievementsResultGetAchievements);
    LOAD_XBL(XblAchievementsResultHasNext);
    LOAD_XBL(XblAchievementsResultGetNextAsync);
    LOAD_XBL(XblAchievementsResultGetNextResult);
    LOAD_XBL(XblAchievementsResultCloseHandle);

    // Profile
    LOAD_XBL(XblProfileGetUserProfilesAsync);
    LOAD_XBL(XblProfileGetUserProfilesResultCount);
    LOAD_XBL(XblProfileGetUserProfilesResult);
    LOAD_XBL(XblProfileGetUserProfilesForSocialGroupAsync);
    LOAD_XBL(XblProfileGetUserProfilesForSocialGroupResultCount);
    LOAD_XBL(XblProfileGetUserProfilesForSocialGroupResult);

    // Title Storage
    LOAD_XBL(XblTitleStorageGetQuotaAsync);
    LOAD_XBL(XblTitleStorageGetQuotaResult);
    LOAD_XBL(XblTitleStorageGetBlobMetadataAsync);
    LOAD_XBL(XblTitleStorageGetBlobMetadataResult);
    LOAD_XBL(XblTitleStorageBlobMetadataResultGetItems);
    LOAD_XBL(XblTitleStorageBlobMetadataResultHasNext);
    LOAD_XBL(XblTitleStorageBlobMetadataResultGetNextAsync);
    LOAD_XBL(XblTitleStorageBlobMetadataResultGetNextResult);
    LOAD_XBL(XblTitleStorageBlobMetadataResultDuplicateHandle);
    LOAD_XBL(XblTitleStorageBlobMetadataResultCloseHandle);
    LOAD_XBL(XblTitleStorageUploadBlobAsync);
    LOAD_XBL(XblTitleStorageUploadBlobResult);
    LOAD_XBL(XblTitleStorageDownloadBlobAsync);
    LOAD_XBL(XblTitleStorageDownloadBlobResult);
    LOAD_XBL(XblTitleStorageDeleteBlobAsync);

    // Privacy
    LOAD_XBL(XblPrivacyCheckPermissionAsync);
    LOAD_XBL(XblPrivacyCheckPermissionResultSize);
    LOAD_XBL(XblPrivacyCheckPermissionResult);
    LOAD_XBL(XblPrivacyCheckPermissionForAnonymousUserAsync);
    LOAD_XBL(XblPrivacyCheckPermissionForAnonymousUserResultSize);
    LOAD_XBL(XblPrivacyCheckPermissionForAnonymousUserResult);
    LOAD_XBL(XblPrivacyBatchCheckPermissionAsync);
    LOAD_XBL(XblPrivacyBatchCheckPermissionResultSize);
    LOAD_XBL(XblPrivacyBatchCheckPermissionResult);
    LOAD_XBL(XblPrivacyGetAvoidListAsync);
    LOAD_XBL(XblPrivacyGetAvoidListResultCount);
    LOAD_XBL(XblPrivacyGetAvoidListResult);
    LOAD_XBL(XblPrivacyGetMuteListAsync);
    LOAD_XBL(XblPrivacyGetMuteListResultCount);
    LOAD_XBL(XblPrivacyGetMuteListResult);

    // Social & Social Manager
    LOAD_XBL(XblSocialGetSocialRelationshipsAsync);
    LOAD_XBL(XblSocialGetSocialRelationshipsResult);
    LOAD_XBL(XblSocialRelationshipResultGetTotalCount);
    LOAD_XBL(XblSocialRelationshipResultGetNextAsync);
    LOAD_XBL(XblSocialRelationshipResultGetNextResult);
    LOAD_XBL(XblSocialRelationshipResultCloseHandle);
    LOAD_XBL(XblSocialAddSocialRelationshipChangedHandler);
    LOAD_XBL(XblSocialRemoveSocialRelationshipChangedHandler);
    LOAD_XBL(XblSocialSubscribeToSocialRelationshipChange);
    LOAD_XBL(XblSocialUnsubscribeFromSocialRelationshipChange);
    LOAD_XBL(XblSocialSubmitReputationFeedbackAsync);
    LOAD_XBL(XblSocialSubmitBatchReputationFeedbackAsync);
    LOAD_XBL(XblSocialManagerAddLocalUser);
    LOAD_XBL(XblSocialManagerRemoveLocalUser);
    LOAD_XBL(XblSocialManagerGetLocalUserCount);
    LOAD_XBL(XblSocialManagerGetLocalUsers);
    LOAD_XBL(XblSocialManagerCreateSocialUserGroupFromFilters);
    LOAD_XBL(XblSocialManagerCreateSocialUserGroupFromList);
    LOAD_XBL(XblSocialManagerDestroySocialUserGroup);
    LOAD_XBL(XblSocialManagerDoWork);
    LOAD_XBL(XblSocialManagerUserGroupGetUsers);
    LOAD_XBL(XblSocialManagerUserGroupGetLocalUser);
    LOAD_XBL(XblSocialManagerUserGroupGetType);
    LOAD_XBL(XblSocialManagerUserGroupGetFilters);
    LOAD_XBL(XblSocialManagerUserGroupGetUsersTrackedByGroup);
    LOAD_XBL(XblSocialManagerUpdateSocialUserGroup);
    LOAD_XBL(XblSocialManagerSetRichPresencePollingStatus);
    LOAD_XBL(XblSocialManagerPresenceRecordIsUserPlayingTitle);

    // HttpCall
    LOAD_XBL(XblHttpCallCreate);
    LOAD_XBL(XblHttpCallDuplicateHandle);
    LOAD_XBL(XblHttpCallCloseHandle);
    LOAD_XBL(XblHttpCallPerformAsync);
    LOAD_XBL(XblHttpCallRequestSetHeader);
    LOAD_XBL(XblHttpCallRequestSetRequestBodyBytes);
    LOAD_XBL(XblHttpCallRequestSetRequestBodyString);
    LOAD_XBL(XblHttpCallRequestSetRetryAllowed);
    LOAD_XBL(XblHttpCallRequestSetRetryCacheId);
    LOAD_XBL(XblHttpCallRequestSetLongHttpCall);
    LOAD_XBL(XblHttpCallSetTracing);
    LOAD_XBL(XblHttpCallGetRequestUrl);
    LOAD_XBL(XblHttpCallGetStatusCode);
    LOAD_XBL(XblHttpCallGetNetworkErrorCode);
    LOAD_XBL(XblHttpCallGetPlatformNetworkErrorMessage);
    LOAD_XBL(XblHttpCallGetNumHeaders);
    LOAD_XBL(XblHttpCallGetHeaderAtIndex);
    LOAD_XBL(XblHttpCallGetHeader);
    LOAD_XBL(XblHttpCallGetResponseBodyBytesSize);
    LOAD_XBL(XblHttpCallGetResponseBodyBytes);
    LOAD_XBL(XblHttpCallGetResponseString);

    // UserStatistics & TitleManagedStats
    LOAD_XBL(XblUserStatisticsGetSingleUserStatisticsAsync);
    LOAD_XBL(XblUserStatisticsGetSingleUserStatisticsResultSize);
    LOAD_XBL(XblUserStatisticsGetSingleUserStatisticsResult);
    LOAD_XBL(XblUserStatisticsGetMultipleUserStatisticsAsync);
    LOAD_XBL(XblUserStatisticsGetMultipleUserStatisticsResultSize);
    LOAD_XBL(XblUserStatisticsGetMultipleUserStatisticsResult);
    LOAD_XBL(XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync);
    LOAD_XBL(XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize);
    LOAD_XBL(XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult);
    LOAD_XBL(XblUserStatisticsAddStatisticChangedHandler);
    LOAD_XBL(XblUserStatisticsRemoveStatisticChangedHandler);
    LOAD_XBL(XblUserStatisticsSubscribeToStatisticChange);
    LOAD_XBL(XblUserStatisticsUnsubscribeFromStatisticChange);
    LOAD_XBL(XblTitleManagedStatsUpdateStatsAsync);
    LOAD_XBL(XblTitleManagedStatsDeleteStatsAsync);

    // String Verification
    LOAD_XBL(XblStringVerifyStringAsync);
    LOAD_XBL(XblStringVerifyStringResultSize);
    LOAD_XBL(XblStringVerifyStringResult);
    LOAD_XBL(XblStringVerifyStringsAsync);
    LOAD_XBL(XblStringVerifyStringsResultSize);
    LOAD_XBL(XblStringVerifyStringsResult);

    // Presence
    LOAD_XBL(XblPresenceGetPresenceForMultipleUsersAsync);
    LOAD_XBL(XblPresenceGetPresenceForMultipleUsersResultCount);
    LOAD_XBL(XblPresenceGetPresenceForMultipleUsersResult);
    LOAD_XBL(XblPresenceGetPresenceForSocialGroupAsync);
    LOAD_XBL(XblPresenceGetPresenceForSocialGroupResultCount);
    LOAD_XBL(XblPresenceGetPresenceForSocialGroupResult);
    LOAD_XBL(XblPresenceRecordCloseHandle);
    LOAD_XBL(XblPresenceAddDevicePresenceChangedHandler);
    LOAD_XBL(XblPresenceRemoveDevicePresenceChangedHandler);
    LOAD_XBL(XblPresenceAddTitlePresenceChangedHandler);
    LOAD_XBL(XblPresenceRemoveTitlePresenceChangedHandler);
    LOAD_XBL(XblPresenceSubscribeToDevicePresenceChange);
    LOAD_XBL(XblPresenceUnsubscribeFromDevicePresenceChange);
    LOAD_XBL(XblPresenceSubscribeToTitlePresenceChange);
    LOAD_XBL(XblPresenceUnsubscribeFromTitlePresenceChange);

#undef LOAD_XBL

    // Setup clean save root inside scratch dir
    const fs::path xsapi_root = GetScratchDir() / "xsapi_parity_saves";
    std::error_code ec;
    fs::remove_all(xsapi_root, ec);
    SetEnvironmentVariableA("OPENSCARLETT_SAVE_ROOT", xsapi_root.string().c_str());

    XTaskQueueHandle queue = nullptr;
    OS_REQUIRE(XTaskQueueCreate(
        XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &queue) == XGR_S_OK);

    // 10a. Core / Wrapper init / Context settings / Events
    const char* kTestScid = "00000000-0000-0000-0000-00007E575A01";
    OS_REQUIRE(p_XblWrapper_XblInitialize(kTestScid, queue) == XBL_S_OK);
    OS_REQUIRE(p_XblGetAsyncQueue() != nullptr);
    const char* out_scid = nullptr;
    OS_REQUIRE(p_XblGetScid(&out_scid) == XBL_S_OK && out_scid &&
               std::strcmp(out_scid, kTestScid) == 0);
    OS_REQUIRE(p_XblGetErrorCondition(XBL_S_OK) == XblErrorCondition_NoError);
    OS_REQUIRE(p_XblGetErrorCondition(XBL_E_HTTP_STATUS_404_NOT_FOUND) ==
               XblErrorCondition_Http404NotFound);
    OS_REQUIRE(p_XblSetOverrideConfiguration("en-US") == XBL_S_OK);
    p_XblDisableAssertsForXboxLiveThrottlingInDevSandboxes(XblConfigSetting_ThisCodeNeedsToBeChanged);
    XblMemAllocFunction alloc_fn = nullptr;
    XblMemFreeFunction free_fn = nullptr;
    OS_REQUIRE(p_XblMemGetFunctions(&alloc_fn, &free_fn) == XBL_S_OK);
    OS_REQUIRE(p_XblMemSetFunctions(nullptr, nullptr) == XBL_S_OK);

    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        OS_REQUIRE(XUserAddResult(&ab, &user) == XGR_S_OK && user != nullptr);
    }

    XblContextHandle ctx = nullptr;
    OS_REQUIRE(p_XblContextCreateHandle(user, &ctx) == XBL_S_OK && ctx != nullptr);
    uint64_t xuid = 0;
    OS_REQUIRE(p_XblContextGetXboxUserId(ctx, &xuid) == XBL_S_OK && xuid != 0ull);
    OS_REQUIRE(p_XblEventsWriteInGameEvent(ctx, "LevelStart", R"({"level":1})", R"({"ms":12})") ==
               XBL_S_OK);

    uint32_t uval = 0;
    bool bval = false;
    OS_REQUIRE(p_XblContextSettingsSetHttpRetryDelay(ctx, 5u) == XBL_S_OK);
    OS_REQUIRE(p_XblContextSettingsGetHttpRetryDelay(ctx, &uval) == XBL_S_OK && uval == 5u);
    OS_REQUIRE(p_XblContextSettingsSetHttpTimeoutWindow(ctx, 45u) == XBL_S_OK);
    OS_REQUIRE(p_XblContextSettingsGetHttpTimeoutWindow(ctx, &uval) == XBL_S_OK && uval == 45u);
    OS_REQUIRE(p_XblContextSettingsSetLongHttpTimeout(ctx, 600u) == XBL_S_OK);
    OS_REQUIRE(p_XblContextSettingsGetLongHttpTimeout(ctx, &uval) == XBL_S_OK && uval == 600u);
    OS_REQUIRE(p_XblContextSettingsSetUseCrossPlatformQosServers(ctx, true) == XBL_S_OK);
    OS_REQUIRE(p_XblContextSettingsGetUseCrossPlatformQosServers(ctx, &bval) == XBL_S_OK && bval);
    OS_REQUIRE(p_XblContextSettingsSetWebsocketTimeoutWindow(ctx, 180u) == XBL_S_OK);
    OS_REQUIRE(p_XblContextSettingsGetWebsocketTimeoutWindow(ctx, &uval) == XBL_S_OK && uval == 180u);

    // 10b. Title Storage: Upload -> Quota -> Metadata + GetNext -> Download -> Delete -> 404
    {
        const std::string blob1_data = R"({"slot":1,"gold":777})";
        const std::string blob2_data = R"({"slot":2,"gold":999})";
        XblTitleStorageBlobMetadata m1{};
        std::memcpy(m1.blobPath, "saves/slot1.json", 17);
        m1.blobType = XblTitleStorageBlobType_Json;
        m1.storageType = XblTitleStorageType_TrustedPlatformStorage;
        std::memcpy(m1.displayName, "Slot 1", 7);

        XblTitleStorageBlobMetadata uploaded1{};
        {
            XAsyncBlock ab{};
            OS_REQUIRE(p_XblTitleStorageUploadBlobAsync(
                ctx, m1, reinterpret_cast<const uint8_t*>(blob1_data.data()), blob1_data.size(),
                XblTitleStorageETagMatchCondition_NotUsed, 0u, &ab) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageUploadBlobResult(&ab, &uploaded1) == XBL_S_OK);
            OS_REQUIRE(uploaded1.length == blob1_data.size() && uploaded1.eTag[0] != '\0');
        }

        XblTitleStorageBlobMetadata m2 = m1;
        std::memcpy(m2.blobPath, "saves/slot2.json", 17);
        XblTitleStorageBlobMetadata uploaded2{};
        {
            XAsyncBlock ab{};
            OS_REQUIRE(p_XblTitleStorageUploadBlobAsync(
                ctx, m2, reinterpret_cast<const uint8_t*>(blob2_data.data()), blob2_data.size(),
                XblTitleStorageETagMatchCondition_NotUsed, 0u, &ab) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageUploadBlobResult(&ab, &uploaded2) == XBL_S_OK);
        }

        // Quota check
        {
            XAsyncBlock ab{};
            size_t used_b = 0, quota_b = 0;
            OS_REQUIRE(p_XblTitleStorageGetQuotaAsync(
                ctx, kTestScid, XblTitleStorageType_TrustedPlatformStorage, &ab) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageGetQuotaResult(&ab, &used_b, &quota_b) == XBL_S_OK);
            OS_REQUIRE(used_b == blob1_data.size() + blob2_data.size() && quota_b > used_b);
        }

        // Metadata pagination (page size = 1)
        {
            XAsyncBlock ab{};
            XblTitleStorageBlobMetadataResultHandle meta_res = nullptr;
            OS_REQUIRE(p_XblTitleStorageGetBlobMetadataAsync(
                ctx, kTestScid, XblTitleStorageType_TrustedPlatformStorage, "saves/", xuid,
                0u, 1u, &ab) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageGetBlobMetadataResult(&ab, &meta_res) == XBL_S_OK);
            XblTitleStorageBlobMetadataResultHandle meta_dup = nullptr;
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultDuplicateHandle(meta_res, &meta_dup) == XBL_S_OK);
            const XblTitleStorageBlobMetadata* items = nullptr;
            size_t count = 0;
            bool has_next = false;
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultGetItems(meta_res, &items, &count) == XBL_S_OK);
            OS_REQUIRE(count == 1u && items != nullptr);
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultHasNext(meta_res, &has_next) == XBL_S_OK);
            OS_REQUIRE(has_next);

            XAsyncBlock ab2{};
            XblTitleStorageBlobMetadataResultHandle meta_next = nullptr;
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultGetNextAsync(meta_res, 10u, &ab2) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultGetNextResult(&ab2, &meta_next) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultGetItems(meta_next, &items, &count) == XBL_S_OK);
            OS_REQUIRE(count == 1u);
            OS_REQUIRE(p_XblTitleStorageBlobMetadataResultHasNext(meta_next, &has_next) == XBL_S_OK);
            OS_REQUIRE(!has_next);
            p_XblTitleStorageBlobMetadataResultCloseHandle(meta_next);
            p_XblTitleStorageBlobMetadataResultCloseHandle(meta_dup);
            p_XblTitleStorageBlobMetadataResultCloseHandle(meta_res);
        }

        // Download round-trip + ETag IfMatch check
        {
            std::vector<uint8_t> dl_buf(64, 0);
            XblTitleStorageBlobMetadata dl_meta{};
            XAsyncBlock ab{};
            OS_REQUIRE(p_XblTitleStorageDownloadBlobAsync(
                ctx, uploaded1, dl_buf.data(), dl_buf.size(),
                XblTitleStorageETagMatchCondition_IfMatch, nullptr, 0u, &ab) == XBL_S_OK);
            OS_REQUIRE(p_XblTitleStorageDownloadBlobResult(&ab, &dl_meta) == XBL_S_OK);
            OS_REQUIRE(dl_meta.length == blob1_data.size());
            OS_REQUIRE(std::memcmp(dl_buf.data(), blob1_data.data(), blob1_data.size()) == 0);
        }

        // Delete & verify 404 on subsequent download
        {
            XAsyncBlock ab{};
            OS_REQUIRE(p_XblTitleStorageDeleteBlobAsync(ctx, uploaded1, true, &ab) == XBL_S_OK);
            OS_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);

            std::vector<uint8_t> dl_buf(64, 0);
            XAsyncBlock ab_missing{};
            OS_REQUIRE(p_XblTitleStorageDownloadBlobAsync(
                ctx, uploaded1, dl_buf.data(), dl_buf.size(),
                XblTitleStorageETagMatchCondition_NotUsed, nullptr, 0u, &ab_missing) == XBL_S_OK);
            OS_REQUIRE(XAsyncGetStatus(&ab_missing, true) == XBL_E_HTTP_STATUS_404_NOT_FOUND);
        }
    }

    // 10c. Privacy (CheckPermission, Anonymous, Batch, Avoid/Mute)
    {
        XAsyncBlock ab{};
        OS_REQUIRE(p_XblPrivacyCheckPermissionAsync(
            ctx, XblPermission_PlayMultiplayer, 2533274790395905ull, &ab) == XBL_S_OK);
        size_t sz = 0;
        OS_REQUIRE(p_XblPrivacyCheckPermissionResultSize(&ab, &sz) == XBL_S_OK &&
                   sz >= sizeof(XblPermissionCheckResult));
        std::vector<uint8_t> buf(sz);
        XblPermissionCheckResult* pres = nullptr;
        OS_REQUIRE(p_XblPrivacyCheckPermissionResult(&ab, buf.size(), buf.data(), &pres, nullptr) ==
                   XBL_S_OK);
        OS_REQUIRE(pres != nullptr && pres->isAllowed && pres->reasonsCount == 0u);

        XAsyncBlock ab_anon{};
        OS_REQUIRE(p_XblPrivacyCheckPermissionForAnonymousUserAsync(
            ctx, XblPermission_CommunicateUsingText, XblAnonymousUserType_CrossNetworkUser,
            &ab_anon) == XBL_S_OK);
        OS_REQUIRE(p_XblPrivacyCheckPermissionForAnonymousUserResultSize(&ab_anon, &sz) == XBL_S_OK);
        buf.resize(sz);
        OS_REQUIRE(p_XblPrivacyCheckPermissionForAnonymousUserResult(
            &ab_anon, buf.size(), buf.data(), &pres, nullptr) == XBL_S_OK);
        OS_REQUIRE(pres != nullptr && pres->isAllowed);

        XblPermission perms[2] = {XblPermission_PlayMultiplayer, XblPermission_ViewTargetProfile};
        uint64_t xuids[2] = {xuid, xuid + 1ull};
        XblAnonymousUserType anons[1] = {XblAnonymousUserType_CrossNetworkFriend};
        XAsyncBlock ab_batch{};
        OS_REQUIRE(p_XblPrivacyBatchCheckPermissionAsync(
            ctx, perms, 2u, xuids, 2u, anons, 1u, &ab_batch) == XBL_S_OK);
        OS_REQUIRE(p_XblPrivacyBatchCheckPermissionResultSize(&ab_batch, &sz) == XBL_S_OK);
        buf.resize(sz);
        XblPermissionCheckResult* batch_res = nullptr;
        size_t batch_cnt = 0;
        OS_REQUIRE(p_XblPrivacyBatchCheckPermissionResult(
            &ab_batch, buf.size(), buf.data(), &batch_res, &batch_cnt, nullptr) == XBL_S_OK);
        OS_REQUIRE(batch_cnt == 6u && batch_res[0].isAllowed && batch_res[5].isAllowed);

        XAsyncBlock ab_avoid{}, ab_mute{};
        size_t avoid_cnt = 99, mute_cnt = 99;
        OS_REQUIRE(p_XblPrivacyGetAvoidListAsync(ctx, &ab_avoid) == XBL_S_OK);
        OS_REQUIRE(p_XblPrivacyGetAvoidListResultCount(&ab_avoid, &avoid_cnt) == XBL_S_OK &&
                   avoid_cnt == 0u);
        OS_REQUIRE(p_XblPrivacyGetAvoidListResult(&ab_avoid, 0u, nullptr) == XBL_S_OK);
        OS_REQUIRE(p_XblPrivacyGetMuteListAsync(ctx, &ab_mute) == XBL_S_OK);
        OS_REQUIRE(p_XblPrivacyGetMuteListResultCount(&ab_mute, &mute_cnt) == XBL_S_OK &&
                   mute_cnt == 0u);
        OS_REQUIRE(p_XblPrivacyGetMuteListResult(&ab_mute, 0u, nullptr) == XBL_S_OK);
    }

    // 10d. Profile (Multiple Users + Social Group)
    {
        const uint64_t xuids[2] = {xuid, 2533274790399999ull};
        XAsyncBlock ab{};
        OS_REQUIRE(p_XblProfileGetUserProfilesAsync(ctx, xuids, 2u, &ab) == XBL_S_OK);
        size_t cnt = 0;
        OS_REQUIRE(p_XblProfileGetUserProfilesResultCount(&ab, &cnt) == XBL_S_OK && cnt == 2u);
        XblUserProfile profs[2]{};
        OS_REQUIRE(p_XblProfileGetUserProfilesResult(&ab, 2u, profs) == XBL_S_OK);
        OS_REQUIRE(profs[0].xboxUserId == xuid && profs[1].xboxUserId == xuids[1]);
        OS_REQUIRE(profs[0].gamertag[0] != '\0' && profs[1].gamertag[0] != '\0');

        XAsyncBlock ab_sg{};
        OS_REQUIRE(p_XblProfileGetUserProfilesForSocialGroupAsync(ctx, "People", &ab_sg) == XBL_S_OK);
        OS_REQUIRE(p_XblProfileGetUserProfilesForSocialGroupResultCount(&ab_sg, &cnt) == XBL_S_OK &&
                   cnt == 0u);
        OS_REQUIRE(p_XblProfileGetUserProfilesForSocialGroupResult(&ab_sg, 0u, nullptr) == XBL_S_OK);
    }

    // 10e. Social + Social Manager
    {
        XAsyncBlock ab{};
        XblSocialRelationshipResultHandle sres = nullptr;
        OS_REQUIRE(p_XblSocialGetSocialRelationshipsAsync(
            ctx, xuid, XblSocialRelationshipFilter_All, 0u, 10u, &ab) == XBL_S_OK);
        OS_REQUIRE(p_XblSocialGetSocialRelationshipsResult(&ab, &sres) == XBL_S_OK);
        size_t tot = 99;
        OS_REQUIRE(p_XblSocialRelationshipResultGetTotalCount(sres, &tot) == XBL_S_OK && tot == 0u);
        XAsyncBlock ab_next{};
        XblSocialRelationshipResultHandle snext = nullptr;
        OS_REQUIRE(p_XblSocialRelationshipResultGetNextAsync(ctx, sres, 10u, &ab_next) == XBL_S_OK);
        OS_REQUIRE(p_XblSocialRelationshipResultGetNextResult(&ab_next, &snext) == XBL_S_OK);
        p_XblSocialRelationshipResultCloseHandle(snext);
        p_XblSocialRelationshipResultCloseHandle(sres);

        XblFunctionContext stok = p_XblSocialAddSocialRelationshipChangedHandler(
            ctx, &TestSocialChangedCb, nullptr);
        OS_REQUIRE(stok > 0);
        OS_REQUIRE(p_XblSocialRemoveSocialRelationshipChangedHandler(ctx, stok) == XBL_S_OK);
        XblRealTimeActivitySubscriptionHandle sub = nullptr;
        OS_REQUIRE(p_XblSocialSubscribeToSocialRelationshipChange(ctx, xuid, &sub) == XBL_S_OK &&
                   sub != nullptr);
        OS_REQUIRE(p_XblSocialUnsubscribeFromSocialRelationshipChange(ctx, sub) == XBL_S_OK);

        XAsyncBlock ab_rep{}, ab_brep{};
        OS_REQUIRE(p_XblSocialSubmitReputationFeedbackAsync(
            ctx, xuid, XblReputationFeedbackType_PositiveHelpfulPlayer, nullptr, "GG", nullptr,
            &ab_rep) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab_rep, true) == XBL_S_OK);
        XblReputationFeedbackItem fitem{xuid, XblReputationFeedbackType_PositiveSkilledPlayer, nullptr, "Great", nullptr};
        OS_REQUIRE(p_XblSocialSubmitBatchReputationFeedbackAsync(ctx, &fitem, 1u, &ab_brep) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab_brep, true) == XBL_S_OK);

        // Social Manager lifecycle
        OS_REQUIRE(p_XblSocialManagerAddLocalUser(
            user, XblSocialManagerExtraDetailLevel_All, queue) == XBL_S_OK);
        OS_REQUIRE(p_XblSocialManagerGetLocalUserCount() == 1u);
        XUserHandle local_users[1]{};
        OS_REQUIRE(p_XblSocialManagerGetLocalUsers(1u, local_users) == XBL_S_OK &&
                   local_users[0] != nullptr);
        OS_REQUIRE(p_XblSocialManagerSetRichPresencePollingStatus(user, true) == XBL_S_OK);

        XblSocialManagerUserGroupHandle grp_filter = nullptr;
        OS_REQUIRE(p_XblSocialManagerCreateSocialUserGroupFromFilters(
            user, XblPresenceFilter_AllOnline, XblRelationshipFilter_Friends, &grp_filter) == XBL_S_OK);
        XblSocialUserGroupType gtype = XblSocialUserGroupType_UserListType;
        XblPresenceFilter pfilt = XblPresenceFilter_Unknown;
        XblRelationshipFilter rfilt = XblRelationshipFilter_Favorite;
        OS_REQUIRE(p_XblSocialManagerUserGroupGetType(grp_filter, &gtype) == XBL_S_OK &&
                   gtype == XblSocialUserGroupType_FilterType);
        OS_REQUIRE(p_XblSocialManagerUserGroupGetFilters(grp_filter, &pfilt, &rfilt) == XBL_S_OK &&
                   pfilt == XblPresenceFilter_AllOnline && rfilt == XblRelationshipFilter_Friends);

        uint64_t list_xuids[2] = {xuid, 2533274790398888ull};
        XblSocialManagerUserGroupHandle grp_list = nullptr;
        OS_REQUIRE(p_XblSocialManagerCreateSocialUserGroupFromList(
            user, list_xuids, 2u, &grp_list) == XBL_S_OK);
        XUserHandle grp_owner = nullptr;
        OS_REQUIRE(p_XblSocialManagerUserGroupGetLocalUser(grp_list, &grp_owner) == XBL_S_OK &&
                   grp_owner == user);
        const XblSocialManagerUser* const* sm_users = nullptr;
        size_t sm_ucnt = 0;
        OS_REQUIRE(p_XblSocialManagerUserGroupGetUsers(grp_list, &sm_users, &sm_ucnt) == XBL_S_OK &&
                   sm_ucnt == 2u && sm_users[0]->xboxUserId == xuid);
        const uint64_t* tracked = nullptr;
        size_t tracked_cnt = 0;
        OS_REQUIRE(p_XblSocialManagerUserGroupGetUsersTrackedByGroup(
            grp_list, &tracked, &tracked_cnt) == XBL_S_OK && tracked_cnt == 2u);

        uint64_t updated_xuids[1] = {2533274790397777ull};
        OS_REQUIRE(p_XblSocialManagerUpdateSocialUserGroup(grp_list, updated_xuids, 1u) == XBL_S_OK);
        const XblSocialManagerEvent* events = nullptr;
        size_t ev_cnt = 0;
        OS_REQUIRE(p_XblSocialManagerDoWork(&events, &ev_cnt) == XBL_S_OK && ev_cnt >= 3u);

        XblSocialManagerPresenceRecord prec{};
        prec.presenceTitleRecordCount = 1u;
        prec.presenceTitleRecords[0].titleId = 0x5CA00001u;
        prec.presenceTitleRecords[0].isTitleActive = true;
        OS_REQUIRE(p_XblSocialManagerPresenceRecordIsUserPlayingTitle(&prec, 0x5CA00001u));
        OS_REQUIRE(!p_XblSocialManagerPresenceRecordIsUserPlayingTitle(&prec, 0x11111111u));

        OS_REQUIRE(p_XblSocialManagerDestroySocialUserGroup(grp_list) == XBL_S_OK);
        OS_REQUIRE(p_XblSocialManagerDestroySocialUserGroup(grp_filter) == XBL_S_OK);
        OS_REQUIRE(p_XblSocialManagerRemoveLocalUser(user) == XBL_S_OK);
    }

    // 10f. XblHttpCall* + ServiceCallRoutedHandler
    {
        std::atomic<int> routed_hits{0};
        XblFunctionContext rtok = p_XblAddServiceCallRoutedHandler(
            &TestServiceCallRoutedCb, &routed_hits);
        OS_REQUIRE(rtok > 0);

        XblHttpCallHandle call = nullptr;
        OS_REQUIRE(p_XblHttpCallCreate(
            ctx, "POST", "https://xsapi.openscarlett.local/v1/echo", &call) == XBL_S_OK);
        XblHttpCallHandle dup_call = p_XblHttpCallDuplicateHandle(call);
        OS_REQUIRE(dup_call == call);
        OS_REQUIRE(p_XblHttpCallRequestSetHeader(call, "X-Test", "1", true) == XBL_S_OK);
        const uint8_t raw_bytes[] = {'{', '}'};
        OS_REQUIRE(p_XblHttpCallRequestSetRequestBodyBytes(call, raw_bytes, 2u) == XBL_S_OK);
        OS_REQUIRE(p_XblHttpCallRequestSetRequestBodyString(call, R"({"ping":true})") == XBL_S_OK);
        OS_REQUIRE(p_XblHttpCallRequestSetRetryAllowed(call, true) == XBL_S_OK);
        OS_REQUIRE(p_XblHttpCallRequestSetRetryCacheId(call, 7u) == XBL_S_OK);
        OS_REQUIRE(p_XblHttpCallRequestSetLongHttpCall(call, false) == XBL_S_OK);
        OS_REQUIRE(p_XblHttpCallSetTracing(call, true) == XBL_S_OK);

        XAsyncBlock ab{};
        OS_REQUIRE(p_XblHttpCallPerformAsync(call, XblHttpCallResponseBodyType_String, &ab) ==
                   XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);

        const char* req_url = nullptr;
        uint32_t status = 0;
        XGameResult net_err = XGR_E_FAIL;
        uint32_t plat_err = 1;
        const char* plat_msg = nullptr;
        uint32_t num_hdrs = 0;
        const char* hname = nullptr;
        const char* hval = nullptr;
        const char* ct_val = nullptr;
        size_t body_sz = 0;
        const char* body_str = nullptr;
        OS_REQUIRE(p_XblHttpCallGetRequestUrl(call, &req_url) == XBL_S_OK && req_url);
        OS_REQUIRE(p_XblHttpCallGetStatusCode(call, &status) == XBL_S_OK && status == 200u);
        OS_REQUIRE(p_XblHttpCallGetNetworkErrorCode(call, &net_err, &plat_err) == XBL_S_OK &&
                   net_err == XBL_S_OK && plat_err == 0u);
        OS_REQUIRE(p_XblHttpCallGetPlatformNetworkErrorMessage(call, &plat_msg) == XBL_S_OK);
        OS_REQUIRE(p_XblHttpCallGetNumHeaders(call, &num_hdrs) == XBL_S_OK && num_hdrs >= 2u);
        OS_REQUIRE(p_XblHttpCallGetHeaderAtIndex(call, 0u, &hname, &hval) == XBL_S_OK && hname && hval);
        OS_REQUIRE(p_XblHttpCallGetHeader(call, "content-type", &ct_val) == XBL_S_OK &&
                   ct_val && std::strcmp(ct_val, "application/json") == 0);
        OS_REQUIRE(p_XblHttpCallGetResponseBodyBytesSize(call, &body_sz) == XBL_S_OK && body_sz > 0u);
        std::vector<uint8_t> body_bytes(body_sz);
        size_t body_used = 0;
        OS_REQUIRE(p_XblHttpCallGetResponseBodyBytes(call, body_bytes.size(), body_bytes.data(), &body_used) ==
                   XBL_S_OK && body_used == body_sz);
        OS_REQUIRE(p_XblHttpCallGetResponseString(call, &body_str) == XBL_S_OK &&
                   body_str && std::strstr(body_str, "openscarlett-xsapi-http") != nullptr);
        OS_REQUIRE(routed_hits.load() == 1);

        p_XblHttpCallCloseHandle(dup_call);
        p_XblHttpCallCloseHandle(call);
        p_XblRemoveServiceCallRoutedHandler(rtok);
    }

    // 10g. Achievements single + pagination
    {
        for (int i = 1; i <= 3; ++i) {
            XAsyncBlock ab{};
            const std::string aid = std::to_string(i);
            OS_REQUIRE(p_XblAchievementsUpdateAchievementAsync(
                ctx, xuid, aid.c_str(), static_cast<uint32_t>(i * 30), &ab) == XBL_S_OK);
            OS_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);
        }
        XAsyncBlock ab_one{};
        XblAchievementsResultHandle one_res = nullptr;
        OS_REQUIRE(p_XblAchievementsGetAchievementAsync(ctx, xuid, kTestScid, "2", &ab_one) == XBL_S_OK);
        OS_REQUIRE(p_XblAchievementsGetAchievementResult(&ab_one, &one_res) == XBL_S_OK);
        const XblAchievement* achs = nullptr;
        size_t acnt = 0;
        OS_REQUIRE(p_XblAchievementsResultGetAchievements(one_res, &achs, &acnt) == XBL_S_OK &&
                   acnt == 1u && achs[0].percentComplete == 60u);
        p_XblAchievementsResultCloseHandle(one_res);

        XAsyncBlock ab_p1{};
        XblAchievementsResultHandle p1 = nullptr;
        OS_REQUIRE(p_XblAchievementsGetAchievementsForTitleIdAsync(
            ctx, xuid, 0u, XblAchievementType_All, false, XblAchievementOrderBy_DefaultOrder,
            0u, 2u, &ab_p1) == XBL_S_OK);
        OS_REQUIRE(p_XblAchievementsGetAchievementsForTitleIdResult(&ab_p1, &p1) == XBL_S_OK);
        bool has_next = false;
        OS_REQUIRE(p_XblAchievementsResultHasNext(p1, &has_next) == XBL_S_OK && has_next);
        XAsyncBlock ab_p2{};
        XblAchievementsResultHandle p2 = nullptr;
        OS_REQUIRE(p_XblAchievementsResultGetNextAsync(p1, 2u, &ab_p2) == XBL_S_OK);
        OS_REQUIRE(p_XblAchievementsResultGetNextResult(&ab_p2, &p2) == XBL_S_OK);
        OS_REQUIRE(p_XblAchievementsResultGetAchievements(p2, &achs, &acnt) == XBL_S_OK && acnt == 1u);
        OS_REQUIRE(p_XblAchievementsResultHasNext(p2, &has_next) == XBL_S_OK && !has_next);
        p_XblAchievementsResultCloseHandle(p2);
        p_XblAchievementsResultCloseHandle(p1);
    }

    // 10h. UserStatistics & TitleManagedStats (Update + ChangedHandler + Multi + Delete)
    {
        std::atomic<int> stat_cb_hits{0};
        XblFunctionContext st_tok = p_XblUserStatisticsAddStatisticChangedHandler(
            ctx, &TestStatChangedCb, &stat_cb_hits);
        OS_REQUIRE(st_tok > 0);
        XblRealTimeActivitySubscriptionHandle st_sub = nullptr;
        OS_REQUIRE(p_XblUserStatisticsSubscribeToStatisticChange(
            ctx, xuid, kTestScid, "Score", &st_sub) == XBL_S_OK && st_sub != nullptr);

        XblTitleManagedStatistic stats_in[2]{};
        stats_in[0].statisticName = "Score";
        stats_in[0].statisticType = XblTitleManagedStatType_Number;
        stats_in[0].numberValue = 4200.0;
        stats_in[1].statisticName = "Rank";
        stats_in[1].statisticType = XblTitleManagedStatType_String;
        stats_in[1].stringValue = "Gold";
        XAsyncBlock ab_upd{};
        OS_REQUIRE(p_XblTitleManagedStatsUpdateStatsAsync(ctx, stats_in, 2u, &ab_upd) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab_upd, true) == XBL_S_OK);
        OS_REQUIRE(stat_cb_hits.load() == 2);

        const char* stat_names[2] = {"Score", "Rank"};
        XAsyncBlock ab_s{};
        OS_REQUIRE(p_XblUserStatisticsGetSingleUserStatisticsAsync(
            ctx, xuid, kTestScid, stat_names, 2u, &ab_s) == XBL_S_OK);
        size_t sz = 0;
        OS_REQUIRE(p_XblUserStatisticsGetSingleUserStatisticsResultSize(&ab_s, &sz) == XBL_S_OK);
        std::vector<uint8_t> buf(sz);
        XblUserStatisticsResult* ures = nullptr;
        OS_REQUIRE(p_XblUserStatisticsGetSingleUserStatisticsResult(
            &ab_s, buf.size(), buf.data(), &ures, nullptr) == XBL_S_OK);
        OS_REQUIRE(ures && ures->serviceConfigStatisticsCount == 1u &&
                   ures->serviceConfigStatistics[0].statisticsCount == 2u &&
                   std::strcmp(ures->serviceConfigStatistics[0].statistics[0].value, "4200") == 0 &&
                   std::strcmp(ures->serviceConfigStatistics[0].statistics[1].value, "Gold") == 0);

        uint64_t ux[2] = {xuid, xuid + 1ull};
        XAsyncBlock ab_m{};
        OS_REQUIRE(p_XblUserStatisticsGetMultipleUserStatisticsAsync(
            ctx, ux, 2u, kTestScid, stat_names, 2u, &ab_m) == XBL_S_OK);
        OS_REQUIRE(p_XblUserStatisticsGetMultipleUserStatisticsResultSize(&ab_m, &sz) == XBL_S_OK);
        buf.resize(sz);
        XblUserStatisticsResult* mres = nullptr;
        size_t mcnt = 0;
        OS_REQUIRE(p_XblUserStatisticsGetMultipleUserStatisticsResult(
            &ab_m, buf.size(), buf.data(), &mres, &mcnt, nullptr) == XBL_S_OK);
        OS_REQUIRE(mcnt == 2u && mres[0].xboxUserId == xuid);

        XblRequestedStatistics rsc{};
        std::memcpy(rsc.serviceConfigurationId, kTestScid, std::strlen(kTestScid) + 1u);
        rsc.statistics = stat_names;
        rsc.statisticsCount = 2u;
        XAsyncBlock ab_ms{};
        OS_REQUIRE(p_XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync(
            ctx, ux, 1u, &rsc, 1u, &ab_ms) == XBL_S_OK);
        OS_REQUIRE(p_XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize(
            &ab_ms, &sz) == XBL_S_OK);
        buf.resize(sz);
        OS_REQUIRE(p_XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult(
            &ab_ms, buf.size(), buf.data(), &mres, &mcnt, nullptr) == XBL_S_OK && mcnt == 1u);

        const char* del_names[1] = {"Rank"};
        XAsyncBlock ab_del{};
        OS_REQUIRE(p_XblTitleManagedStatsDeleteStatsAsync(ctx, del_names, 1u, &ab_del) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab_del, true) == XBL_S_OK);

        OS_REQUIRE(p_XblUserStatisticsUnsubscribeFromStatisticChange(ctx, st_sub) == XBL_S_OK);
        OS_REQUIRE(p_XblUserStatisticsRemoveStatisticChangedHandler(ctx, st_tok) == XBL_S_OK);
    }

    // 10i. String Verification + Multi-User Presence
    {
        XAsyncBlock ab_v1{};
        OS_REQUIRE(p_XblStringVerifyStringAsync(ctx, "OpenScarlett Player", &ab_v1) == XBL_S_OK);
        size_t sz = 0;
        OS_REQUIRE(p_XblStringVerifyStringResultSize(&ab_v1, &sz) == XBL_S_OK);
        std::vector<uint8_t> buf(sz);
        XblVerifyStringResult* vres = nullptr;
        OS_REQUIRE(p_XblStringVerifyStringResult(&ab_v1, buf.size(), buf.data(), &vres, nullptr) ==
                   XBL_S_OK);
        OS_REQUIRE(vres != nullptr && vres->resultCode == XblVerifyStringResultCode_Success);

        const char* strs[2] = {"Alpha", "Beta"};
        XAsyncBlock ab_v2{};
        OS_REQUIRE(p_XblStringVerifyStringsAsync(ctx, strs, 2ull, &ab_v2) == XBL_S_OK);
        OS_REQUIRE(p_XblStringVerifyStringsResultSize(&ab_v2, &sz) == XBL_S_OK);
        buf.resize(sz);
        size_t vcnt = 0;
        OS_REQUIRE(p_XblStringVerifyStringsResult(
            &ab_v2, buf.size(), buf.data(), &vres, &vcnt, nullptr) == XBL_S_OK);
        OS_REQUIRE(vcnt == 2u && vres[0].resultCode == XblVerifyStringResultCode_Success &&
                   vres[1].resultCode == XblVerifyStringResultCode_Success);

        uint64_t px[2] = {xuid, 2533274790391111ull};
        XAsyncBlock ab_pm{}, ab_pg{};
        OS_REQUIRE(p_XblPresenceGetPresenceForMultipleUsersAsync(
            ctx, px, 2u, nullptr, &ab_pm) == XBL_S_OK);
        size_t pcnt = 0;
        OS_REQUIRE(p_XblPresenceGetPresenceForMultipleUsersResultCount(&ab_pm, &pcnt) == XBL_S_OK &&
                   pcnt == 2u);
        XblPresenceRecordHandle precs[2]{};
        OS_REQUIRE(p_XblPresenceGetPresenceForMultipleUsersResult(&ab_pm, precs, 2u) == XBL_S_OK);
        p_XblPresenceRecordCloseHandle(precs[0]);
        p_XblPresenceRecordCloseHandle(precs[1]);

        OS_REQUIRE(p_XblPresenceGetPresenceForSocialGroupAsync(
            ctx, "People", nullptr, nullptr, &ab_pg) == XBL_S_OK);
        OS_REQUIRE(p_XblPresenceGetPresenceForSocialGroupResultCount(&ab_pg, &pcnt) == XBL_S_OK &&
                   pcnt == 0u);
        OS_REQUIRE(p_XblPresenceGetPresenceForSocialGroupResult(&ab_pg, nullptr, 0u) == XBL_S_OK);

        XblFunctionContext dtok = p_XblPresenceAddDevicePresenceChangedHandler(
            ctx, &TestDevicePresenceCb, nullptr);
        XblFunctionContext ttok = p_XblPresenceAddTitlePresenceChangedHandler(
            ctx, &TestTitlePresenceCb, nullptr);
        OS_REQUIRE(dtok > 0 && ttok > 0);
        XblRealTimeActivitySubscriptionHandle dsub = nullptr, tsub = nullptr;
        OS_REQUIRE(p_XblPresenceSubscribeToDevicePresenceChange(ctx, xuid, &dsub) == XBL_S_OK);
        OS_REQUIRE(p_XblPresenceSubscribeToTitlePresenceChange(ctx, xuid, 0x5CA00001u, &tsub) ==
                   XBL_S_OK);
        OS_REQUIRE(p_XblPresenceUnsubscribeFromDevicePresenceChange(ctx, dsub) == XBL_S_OK);
        OS_REQUIRE(p_XblPresenceUnsubscribeFromTitlePresenceChange(ctx, tsub) == XBL_S_OK);
        OS_REQUIRE(p_XblPresenceRemoveDevicePresenceChangedHandler(ctx, dtok) == XBL_S_OK);
        OS_REQUIRE(p_XblPresenceRemoveTitlePresenceChangedHandler(ctx, ttok) == XBL_S_OK);
    }

    p_XblContextCloseHandle(ctx);
    XUserCloseHandle(user);
    p_XblCleanup();
    OS_REQUIRE(XTaskQueueTerminate(queue, true, nullptr, nullptr) == XGR_S_OK);
    XTaskQueueCloseHandle(queue);
    FreeLibrary(mod);
    std::printf("  PASS: XSAPI dynamic parity verified across all 10 Xbl* families.\n");
}
#endif

// ---------------------------------------------------------------------------
// 11. PE Export Audit Across xgameruntime.dll, XSAPI & All 9 Secondary Shims
// ---------------------------------------------------------------------------
void TestPeExportAudit() {
    std::printf("[11/11] PE64 export table audit across xgameruntime, XSAPI & 9 shim DLLs...\n");
    struct DllExpectation {
        const char* dll_name;
        std::vector<const char*> required_exports;
    };
    const std::vector<DllExpectation> specs = {
        {"xgameruntime.dll", {"InitializeApiImpl", "UninitializeApiImpl", "QueryApiImpl", "XGameGetXboxTitleId"}},
        {"Microsoft.Xbox.Services.GDK.C.dll", {"XblWrapper_XblInitialize", "XblGetAsyncQueue", "XblGetScid", "XblGetErrorCondition", "XblMemGetFunctions", "XblContextSettingsGetHttpRetryDelay", "XblAchievementsGetAchievementAsync", "XblAchievementsResultGetNextAsync", "XblProfileGetUserProfilesAsync", "XblTitleStorageUploadBlobAsync", "XblTitleStorageDownloadBlobAsync", "XblTitleStorageDeleteBlobAsync", "XblPrivacyCheckPermissionAsync", "XblPrivacyBatchCheckPermissionAsync", "XblSocialManagerAddLocalUser", "XblSocialManagerDoWork", "XblHttpCallCreate", "XblHttpCallPerformAsync", "XblUserStatisticsGetMultipleUserStatisticsAsync", "XblTitleManagedStatsUpdateStatsAsync", "XblTitleManagedStatsDeleteStatsAsync", "XblStringVerifyStringAsync", "XblPresenceGetPresenceForMultipleUsersAsync"}},
        {"pixevt.dll", {"PIXBeginEventOnCpu", "PIXEndEventOnCpu", "PIXSetMarkerOnCpu", "PIXBeginEventOnCommandList", "PIXEndEventOnCommandList", "PIXSetMarkerOnCommandList", "PIXBeginEventOnCommandQueue", "PIXEndEventOnCommandQueue", "PIXSetMarkerOnCommandQueue", "PIXNotifyWakeFromFenceSignal", "PIXReportCounter", "PIXGpuCaptureNextFrames", "PIXGetEventCountForTest"}},
        {"xmem.dll", {"XMemAlloc", "XMemAllocDefault", "XMemFree", "XMemFreeDefault", "XMemSize", "XMemSizeDefault", "XMemGetAuxiliaryTitleMemory", "XMemReleaseAuxiliaryTitleMemory", "XMemMapPhysicalPages", "XMemUnmapPhysicalPages"}},
        {"xcrdapi.dll", {"XCrdInitialize", "XCrdUninitialize", "XCrdGetDiagnosticInfo", "XCrdSetBreadcrumb", "XCrdGetBreadcrumb", "XCrdReportError", "XCrdFlushDiagnostics"}},
        {"libHttpClient.GDK.dll", {"HCInitialize", "HCCleanup", "HCCleanupAsync", "HCGetLibVersion", "HCTraceSetTraceToDebugger", "HCSettingsSetTraceLevel", "HCHttpCallCreate", "HCHttpCallDuplicateHandle", "HCHttpCallCloseHandle", "HCHttpCallRequestSetUrl", "HCHttpCallRequestSetHeader", "HCHttpCallRequestSetRequestBodyBytes", "HCHttpCallRequestSetRequestBodyString", "HCHttpCallPerformAsync", "HCHttpCallResponseGetStatusCode", "HCHttpCallResponseGetResponseString", "HCHttpCallResponseGetResponseBodyBytes", "HCHttpCallResponseGetResponseBodyBytesSize", "HCHttpCallResponseGetHeader", "HCAddMockResponseForTest"}},
        {"XCurl.dll", {"curl_global_init", "curl_global_init_mem", "curl_global_cleanup", "curl_version", "curl_version_info", "curl_free", "curl_easy_init", "curl_easy_duphandle", "curl_easy_cleanup", "curl_easy_reset", "curl_easy_setopt", "curl_easy_perform", "curl_easy_getinfo", "curl_easy_strerror", "curl_easy_escape", "curl_easy_unescape", "curl_slist_append", "curl_slist_free_all", "curl_multi_init", "curl_multi_cleanup", "curl_multi_add_handle", "curl_multi_remove_handle", "curl_multi_perform", "curl_multi_poll", "curl_multi_wait", "curl_multi_wakeup", "curl_multi_setopt", "curl_multi_strerror", "curl_multi_info_read", "curl_url", "curl_url_cleanup", "curl_url_dup", "curl_url_get", "curl_url_set", "xcurl_global_init_mem", "xcurl_global_suspend", "xcurl_global_resume", "xcurl_global_set_request_limit"}},
        {"Bink2Scarlett.dll", {"BinkOpen", "BinkOpenBuffer", "BinkClose", "BinkDoFrame", "BinkNextFrame", "BinkWait", "BinkCopyToBuffer", "BinkGoto", "BinkGetSummary", "BinkGetError", "BinkSetSoundOnOff", "BinkPause"}},
        {"amd_fidelityfx_upscaler_xbox.dll", {"ffxUpscalerGetVersion", "ffxUpscalerCreateContext", "ffxUpscalerDestroyContext", "ffxUpscalerDispatchRgba8", "ffxUpscalerConfigure", "ffxUpscalerQuery"}},
        {"amd_fidelityfx_loader_xbox.dll", {"ffxQuery", "ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxDispatch"}},
        {"XboxBvhBuilder.dll", {"XboxBvhCreateBuilder", "XboxBvhDestroyBuilder", "XboxBvhGetCapabilities", "XboxBvhGetPrebuildInfo", "XboxBvhBuildBlas", "XboxBvhUpdateBlas", "XboxBvhBuildTlas", "XboxBvhTraceRay", "XboxBvhDispatchRays"}},
    };

    const fs::path exe_dir = GetExeDir();
    size_t total_exports = 0;
    for (const auto& spec : specs) {
        const fs::path dll_path = exe_dir / spec.dll_name;
        openscarlett::PeInspectionReport rep{};
        OS_REQUIRE(openscarlett::InspectPeFile(dll_path.string(), rep).ok());
        OS_REQUIRE(rep.is_pe64 && rep.is_dll);

#if defined(_WIN32)
        HMODULE mod = LoadLibraryA(dll_path.string().c_str());
        OS_REQUIRE(mod != nullptr);

        const auto* base = reinterpret_cast<const uint8_t*>(mod);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        OS_REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        OS_REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
        const auto& exp_dir_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        OS_REQUIRE(exp_dir_entry.VirtualAddress != 0u);
        const auto* exp_dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exp_dir_entry.VirtualAddress);
        const auto* name_rvas = reinterpret_cast<const uint32_t*>(base + exp_dir->AddressOfNames);

        std::unordered_set<std::string> exported;
        for (uint32_t i = 0; i < exp_dir->NumberOfNames; ++i) {
            exported.insert(reinterpret_cast<const char*>(base + name_rvas[i]));
        }

        for (const char* req : spec.required_exports) {
            OS_REQUIRE(exported.find(req) != exported.end());
            OS_REQUIRE(GetProcAddress(mod, req) != nullptr);
        }
        total_exports += exported.size();
        std::printf("  %-36s : %3zu exports verified\n", spec.dll_name, exported.size());
        FreeLibrary(mod);
#endif
    }
    std::printf("  PASS: All 11 DLL PE export tables audited (%zu total exports).\n", total_exports);
}

}  // namespace

int main() {
    std::string hw_desc;
    const bool has_hw_gpu = ProbeHardwareD3D12Gpu(hw_desc);
    std::printf("=== OpenScarlett Secondary Shims & QueryApiImpl Suite (HW GPU: %s) ===\n",
                hw_desc.c_str());

    TestXGameRuntimeQueryApi();
    TestPixEvt();
    TestXMem();
    TestXCrdApi();
    TestLibHttpClient();
    TestXCurl();
    TestBink2Scarlett();
    TestFidelityFxShims(has_hw_gpu);
    TestXboxBvhBuilder(has_hw_gpu);
#if defined(_WIN32)
    TestXsapiDynamicParity();
#endif
    TestPeExportAudit();

    std::printf("ALL 11 SECONDARY SHIMS, XSAPI & QUERYAPI TESTS PASSED!\n");
    return 0;
}
