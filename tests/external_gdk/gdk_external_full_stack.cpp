// OpenScarlett MIT Standalone External Xbox-ABI Binary #2:
// Full 19-DLL Live Stack Verification — written PURELY against GDK/Xbox public headers
// (ZERO openscarlett/ includes, ZERO openscarlett_core link!).
//
// Exercises ALL 19 OpenScarlett Xbox/GDK shim DLLs + CoW G:\ / GAME:\ VFS write protection
// in ONE live APC-injected process launched via `openscarlett run <package_dir>`:
//   1. xgameruntime.dll (incl. QueryApiImpl 8 GUIDs + XGameGetXboxTitleId == 0x5CA02002)
//   2. kernelx.dll (loaded via hooked LoadLibraryA: TitleMemoryStatus + 64 KiB physical aliasing)
//   3. xmem.dll (loaded via hooked LoadLibraryA: aligned zero-init XMemAlloc -> kernelx.dll)
//   4. d3d12_xs.dll (DeviceX + WaitFrameEventX + XS2\0 RDNA2 compute auto-lift + shader cache)
//   5. xg_xs.dll (2D mip-tail + 3D thick 64KB_S 6-mip volume incl. mip-tail bit-exact)
//   6. GameInput.dll (GameInputCreate + GetCurrentReading + GetGamepadState + SetRumbleState)
//   7. XAudio2_9.dll (PCM16 + XMA2 0x0166 + IMA-ADPCM 0x0011 + Xbox ADPCM 0x0069 + Opus 0x704F)
//   8. AcpHal.dll (AcpHalAllocateShapeContexts + AcpHalCommand_XmaDecode on G:\sine440.xma2)
//   9. dstorage_xs.dll (raw + GDeflate + half-full auto-submit + cancel isolation)
//  10. winrt_xs.dll (RoInitialize + HSTRING + RoGetActivationFactory AnalyticsInfo)
//  11. Microsoft.Xbox.Services.GDK.C.dll (XblInitialize + Achievements + Profile + Presence)
//  12. pixevt.dll (PIXBeginEvent/SetMarker/EndEvent CPU+CmdList+Queue + ReportCounter)
//  13. xcrdapi.dll (XCrdInitialize + Breadcrumbs + ReportError + FlushDiagnostics)
//  14. libHttpClient.GDK.dll (HCInitialize + async HCHttpCallPerformAsync via XAsync)
//  15. XCurl.dll (curl_easy + curl_multi offline transfer & callback verification)
//  16. Bink2Scarlett.dll (BinkOpenBuffer on G:\clip.bk2 + RGBA/BGRA copy + BinkGoto)
//  17. amd_fidelityfx_loader_xbox.dll (FidelityFX SDK 2.0 ffxQuery/Create/Configure/Dispatch)
//  18. amd_fidelityfx_upscaler_xbox.dll (live GPU FSR1 EASU+RCAS compute upscale)
//  19. XboxBvhBuilder.dll (BLAS/TLAS build, TraceRay, DispatchRays, UpdateBlas)

#include <XGameRuntimeInit.h>
#include <XTaskQueue.h>
#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XGameSave.h>
#include <XUser.h>
#include <XSystem.h>
#include <XMem.h>
#include <XPackage.h>
#include <XDisplay.h>
#include <dstorage_xs.h>
#include <GameInput.h>
#include <pix3.h>
#include <httpClient/httpClient.h>
#include <XCurl/curl.h>
#include <d3d12_xs.h>
#include <xg_xs.h>
#include <xaudio2_xs.h>
#include <acphal.h>
#include <xsapi-c/services_c.h>
#include <winrt_xs.h>
#include <xcrdapi.h>
#include <bink2.h>
#include <ffx_api/ffx_api.h>
#include <XboxBvhBuilder.h>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

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
    std::fprintf(stderr, "[gdk_external_full_stack] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

bool ReadWholeFileW(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
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

bool ReadTextFileW(const wchar_t* path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFileW(path, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
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

std::string ReadEnvA(const char* name) {
    char buf[1024]{};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (n == 0 || n >= sizeof(buf)) return {};
    return std::string(buf, n);
}

void CALLBACK SimpleTqCb(void* ctx, bool canceled) {
    if (!canceled) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
}

bool CALLBACK ReadyChunkCb(void* ctx, const XPackageChunkSelector* /*sel*/, XPackageChunkAvailability avail) {
    if (avail == XPackageChunkAvailability::Ready) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

size_t __cdecl CurlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, bytes);
    return bytes;
}

bool RunXs2ComputeOnDeviceX(
    bool use_warp,
    const std::vector<uint8_t>& cs_xs2,
    const std::vector<uint32_t>& in_vals,
    bool create_twice_for_memhit,
    D3D12XBOX_GPU_HARDWARE_INFO& out_info,
    std::vector<uint32_t>& out_vals) {
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
    dp.Version = 1;

    ComPtr<ID3D12DeviceX> dev;
    if (FAILED(D3D12XboxCreateDevice(nullptr, &dp, IID_ID3D12DeviceX,
                                     reinterpret_cast<void**>(dev.ReleaseAndGetAddressOf()))) || !dev) {
        return false;
    }
    if (FAILED(dev->GetGpuHardwareInfoX(&out_info))) return false;
    D3D12XBOX_FRAME_PIPELINE_TOKEN tok = 0;
    if (FAILED(dev->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &tok)) || tok == 0) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandListX> cmd;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueueX,
                                       reinterpret_cast<void**>(queue.ReleaseAndGetAddressOf()))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                      IID_ID3D12GraphicsCommandListX,
                                      reinterpret_cast<void**>(cmd.ReleaseAndGetAddressOf()))) ||
        FAILED(cmd->Close()) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return false;
    }

    // Exercise PIX GPU command list & command queue markers directly on live D3D12X objects
    PIXBeginEventOnCommandQueue(queue.Get(), PIX_COLOR(255, 128, 0), "XS2ComputeQueue");

    D3D12_ROOT_PARAMETER rparams[2]{};
    rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rparams[0].Descriptor.ShaderRegister = 0;
    rparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rparams[1].Descriptor.ShaderRegister = 1;
    rparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rparams;
    ComPtr<ID3DBlob> rs_blob, rs_err;
    ComPtr<ID3D12RootSignature> root_sig;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)) ||
        FAILED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                        IID_PPV_ARGS(&root_sig)))) {
        PIXEndEventOnCommandQueue(queue.Get());
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
    cpd.pRootSignature = root_sig.Get();
    cpd.CS = {cs_xs2.data(), cs_xs2.size()};

    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso))) || !pso) {
        PIXEndEventOnCommandQueue(queue.Get());
        return false;
    }
    if (create_twice_for_memhit) {
        ComPtr<ID3D12PipelineState> pso2;
        if (FAILED(dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso2))) || !pso2) {
            PIXEndEventOnCommandQueue(queue.Get());
            return false;
        }
    }

    const uint64_t byte_size = static_cast<uint64_t>(in_vals.size()) * sizeof(uint32_t);
    auto make_buf = [&](D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = ht;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = byte_size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = fl;
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)))) {
            return ComPtr<ID3D12Resource>{};
        }
        return r;
    };

    auto in_up = make_buf(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto out_up = make_buf(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto in_gpu = make_buf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto out_gpu = make_buf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto rb = make_buf(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!in_up || !out_up || !in_gpu || !out_gpu || !rb) {
        PIXEndEventOnCommandQueue(queue.Get());
        return false;
    }

    void* mp = nullptr;
    if (FAILED(in_up->Map(0, nullptr, &mp))) return false;
    std::memcpy(mp, in_vals.data(), static_cast<size_t>(byte_size));
    in_up->Unmap(0, nullptr);
    if (FAILED(out_up->Map(0, nullptr, &mp))) return false;
    std::memset(mp, 0, static_cast<size_t>(byte_size));
    out_up->Unmap(0, nullptr);

    if (FAILED(alloc->Reset()) || FAILED(cmd->Reset(alloc.Get(), pso.Get()))) return false;
    PIXBeginEventOnCommandList(cmd.Get(), PIX_COLOR(0, 192, 255), "DispatchXS2Compute");
    cmd->CopyResource(in_gpu.Get(), in_up.Get());
    cmd->CopyResource(out_gpu.Get(), out_up.Get());

    D3D12_RESOURCE_BARRIER to_uav[2]{};
    to_uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav[0].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    to_uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    cmd->ResourceBarrier(2, to_uav);

    cmd->SetComputeRootSignature(root_sig.Get());
    cmd->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
    cmd->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uav_b{};
    uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_b.UAV.pResource = out_gpu.Get();
    cmd->ResourceBarrier(1, &uav_b);

    D3D12_RESOURCE_BARRIER to_cp{};
    to_cp.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_cp.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cmd->ResourceBarrier(1, &to_cp);
    cmd->CopyResource(rb.Get(), out_gpu.Get());
    PIXEndEventOnCommandList(cmd.Get());
    if (FAILED(cmd->Close())) return false;

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    HANDLE fev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fev) return false;
    const bool wait_ok = SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                         SUCCEEDED(fence->SetEventOnCompletion(1, fev)) &&
                         (::WaitForSingleObject(fev, 15000u) == WAIT_OBJECT_0);
    PIXNotifyWakeFromFenceSignal(fev);
    ::CloseHandle(fev);
    PIXEndEventOnCommandQueue(queue.Get());
    if (!wait_ok) return false;

    out_vals.assign(in_vals.size(), 0u);
    if (FAILED(rb->Map(0, nullptr, &mp))) return false;
    std::memcpy(out_vals.data(), mp, static_cast<size_t>(byte_size));
    rb->Unmap(0, nullptr);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        }
    }

    // =========================================================================
    // 1. pixevt.dll
    // =========================================================================
    PIXScopedEventObject root_pix(PIX_COLOR(32, 224, 64), "FullStack19Dlls");
    PIXSetMarker(PIX_COLOR(255, 255, 0), "Start");
    PIXReportCounter(L"FullStackTargetDlls", 19.0f);
    if (PIXGpuCaptureNextFrames(L"fullstack.wpix", 1u) != 0) {
        return Fail("pixevt PIXGpuCaptureNextFrames");
    }

    // =========================================================================
    // 2. xgameruntime.dll (direct + QueryApiImpl across ALL 8 GUIDs + TitleId 5CA02002)
    // =========================================================================
    if (FAILED(XGameRuntimeInitialize())) {
        return Fail("xgameruntime XGameRuntimeInitialize");
    }
    uint32_t title_id = 0;
    if (FAILED(XGameGetXboxTitleId(&title_id)) || title_id != 0x5CA02002u) {
        return Fail("xgameruntime XGameGetXboxTitleId != 0x5CA02002", title_id);
    }
    char pkg_id[128]{};
    if (FAILED(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id)) ||
        std::strcmp(pkg_id, "OpenScarlett.ExternalFullStack") != 0) {
        return Fail("xgameruntime XPackageGetCurrentProcessPackageIdentifier mismatch");
    }

    // InitializeApiImpl / QueryApiImpl for all 8 GDK static-import GUID vtables
    if (FAILED(InitializeApiImpl())) {
        return Fail("xgameruntime InitializeApiImpl");
    }
    {
        auto query_vt = [](const XgrGuid& g) -> const void* {
            void* p = nullptr;
            if (FAILED(QueryApiImpl(&g, &p))) return nullptr;
            return p;
        };
        const auto* tq_vt = static_cast<const IXgrTaskQueueApiVtable*>(query_vt(XGR_API_GUID_TASK_QUEUE));
        const auto* as_vt = static_cast<const IXgrAsyncApiVtable*>(query_vt(XGR_API_GUID_ASYNC));
        const auto* us_vt = static_cast<const IXgrUserApiVtable*>(query_vt(XGR_API_GUID_USER));
        const auto* gs_vt = static_cast<const IXgrGameSaveApiVtable*>(query_vt(XGR_API_GUID_GAME_SAVE));
        const auto* sy_vt = static_cast<const IXgrSystemApiVtable*>(query_vt(XGR_API_GUID_SYSTEM));
        const auto* mm_vt = static_cast<const IXgrMemApiVtable*>(query_vt(XGR_API_GUID_MEM));
        const auto* pk_vt = static_cast<const IXgrPackageApiVtable*>(query_vt(XGR_API_GUID_PACKAGE));
        const auto* dp_vt = static_cast<const IXgrDisplayApiVtable*>(query_vt(XGR_API_GUID_DISPLAY));
        if (!tq_vt || !as_vt || !us_vt || !gs_vt || !sy_vt || !mm_vt || !pk_vt || !dp_vt) {
            return Fail("xgameruntime QueryApiImpl (8 GUIDs)");
        }

        XTaskQueueHandle q = nullptr;
        if (FAILED(tq_vt->XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &q))) {
            return Fail("QueryApiImpl TaskQueueCreate");
        }
        std::atomic<int> hits{0};
        if (FAILED(tq_vt->XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &hits, &SimpleTqCb)) ||
            !tq_vt->XTaskQueueDispatch(q, XTaskQueuePort::Work, 100u) || hits.load() != 1) {
            return Fail("QueryApiImpl TaskQueue dispatch");
        }
        tq_vt->XTaskQueueTerminate(q, true, nullptr, nullptr);
        tq_vt->XTaskQueueCloseHandle(q);

        if (sy_vt->XSystemGetDeviceType() != XSystemDeviceType::XboxScarlettAnaconda) {
            return Fail("QueryApiImpl XSystemGetDeviceType");
        }
        void* xm_p = mm_vt->XMemAlloc(128u, 0ull);
        if (!xm_p) return Fail("QueryApiImpl XMemAlloc");
        mm_vt->XMemFree(xm_p, 0ull);

        std::atomic<int> ready_chunks{0};
        if (!pk_vt->XPackageIsPackagedProcess() ||
            FAILED(pk_vt->XPackageEnumerateChunkAvailability(pkg_id, 0u, nullptr, &ready_chunks, &ReadyChunkCb)) ||
            ready_chunks.load() != 1) {
            return Fail("QueryApiImpl XPackage");
        }
        XDisplayHdrModeInfo hdr{};
        (void)dp_vt->XDisplayTryEnableHdrMode(1u, &hdr);
        XDisplayTimeoutDeferralHandle def = nullptr;
        if (FAILED(dp_vt->XDisplayTryAcquireTimeoutDeferral(&def)) || !def) {
            return Fail("QueryApiImpl XDisplay");
        }
        dp_vt->XDisplayCloseTimeoutDeferralHandle(def);

        tq_vt->Release(tq_vt);
        as_vt->Release(as_vt);
        us_vt->Release(us_vt);
        gs_vt->Release(gs_vt);
        sy_vt->Release(sy_vt);
        mm_vt->Release(mm_vt);
        pk_vt->Release(pk_vt);
        dp_vt->Release(dp_vt);
    }

    // XUser + XGameSave
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
    {
        XAsyncBlock uab{};
        if (FAILED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &uab)) ||
            FAILED(XUserAddResult(&uab, &user)) || !user ||
            FAILED(XUserGetId(user, &xuid)) || xuid == 0ull) {
            return Fail("xgameruntime XUserAddAsync");
        }
        XGameSaveProviderHandle prov = nullptr;
        XGameSaveContainerHandle cont = nullptr;
        XGameSaveUpdateHandle upd = nullptr;
        const uint8_t blob_bytes[8] = {0x53, 0x43, 0x41, 0x52, 0x4C, 0x45, 0x54, 0x54};
        if (FAILED(XGameSaveInitializeProvider(user, "00000000-0000-0000-0000-00005CA02002", false, &prov)) ||
            FAILED(XGameSaveCreateContainer(prov, "fullstack_slot", &cont)) ||
            FAILED(XGameSaveCreateUpdate(cont, "FullStack", &upd)) ||
            FAILED(XGameSaveSubmitBlobWrite(upd, "state.bin", blob_bytes, sizeof(blob_bytes))) ||
            FAILED(XGameSaveSubmitUpdate(upd))) {
            return Fail("xgameruntime XGameSave write");
        }
        XGameSaveCloseUpdate(upd);
        const char* names[1] = {"state.bin"};
        XAsyncBlock rab{};
        if (FAILED(XGameSaveReadBlobDataAsync(cont, names, 1u, &rab)) ||
            FAILED(XAsyncGetStatus(&rab, true))) {
            return Fail("xgameruntime XGameSaveReadBlobDataAsync");
        }
        size_t rsz = 0;
        if (FAILED(XAsyncGetResultSize(&rab, &rsz)) || rsz == 0) {
            return Fail("xgameruntime XAsyncGetResultSize");
        }
        std::vector<uint8_t> rbuf(rsz, 0u);
        uint32_t rcnt = 0;
        auto* rblobs = reinterpret_cast<XGameSaveBlob*>(rbuf.data());
        if (FAILED(XGameSaveReadBlobDataResult(&rab, rsz, rblobs, &rcnt)) || rcnt != 1u ||
            rblobs[0].info.size != sizeof(blob_bytes) ||
            std::memcmp(rblobs[0].data, blob_bytes, sizeof(blob_bytes)) != 0) {
            return Fail("xgameruntime XGameSaveReadBlobDataResult");
        }
        XGameSaveCloseContainer(cont);
        if (FAILED(XGameSaveDeleteContainer(prov, "fullstack_slot"))) {
            return Fail("xgameruntime XGameSaveDeleteContainer");
        }
        XGameSaveCloseProvider(prov);
    }

    // =========================================================================
    // 3. kernelx.dll (dynamic via hooked LoadLibraryA)
    // =========================================================================
    HMODULE h_kernelx = ::LoadLibraryA("kernelx.dll");
    if (!h_kernelx) return Fail("LoadLibraryA(kernelx.dll)", ::GetLastError());
    {
        auto p_console = reinterpret_cast<PfnGetConsoleType>(::GetProcAddress(h_kernelx, "GetConsoleType"));
        auto p_procs = reinterpret_cast<PfnXGetProcessorCount>(::GetProcAddress(h_kernelx, "XGetProcessorCount"));
        auto p_tms = reinterpret_cast<PfnTitleMemoryStatus>(::GetProcAddress(h_kernelx, "TitleMemoryStatus"));
        auto p_alloc = reinterpret_cast<PfnAllocateTitlePhysicalPages>(::GetProcAddress(h_kernelx, "AllocateTitlePhysicalPages"));
        auto p_free = reinterpret_cast<PfnFreeTitlePhysicalPages>(::GetProcAddress(h_kernelx, "FreeTitlePhysicalPages"));
        auto p_map = reinterpret_cast<PfnMapTitleVirtualMemory>(::GetProcAddress(h_kernelx, "MapTitleVirtualMemory"));
        auto p_unmap = reinterpret_cast<PfnUnmapTitleVirtualMemory>(::GetProcAddress(h_kernelx, "UnmapTitleVirtualMemory"));
        if (!p_console || !p_procs || !p_tms || !p_alloc || !p_free || !p_map || !p_unmap) {
            return Fail("kernelx GetProcAddress");
        }
        if (p_console() != 4u || p_procs() != 14u) {
            return Fail("kernelx console/processor mismatch");
        }
        KernelXTitleMemoryStatus tms{};
        tms.dwLength = static_cast<uint32_t>(sizeof(tms));
        if (p_tms(&tms) != 1 || tms.ullTotalMem == 0ull) return Fail("kernelx TitleMemoryStatus");

        uint64_t pages = 1u;
        uint64_t pfn = 0u;
        if (p_alloc(nullptr, kMemGraphics | MEM_COMMIT, &pages, &pfn) != 1 || pfn == 0u) {
            return Fail("kernelx AllocateTitlePhysicalPages");
        }
        void* v1 = p_map(nullptr, 1u, kMemGraphics, PAGE_READWRITE, &pfn);
        void* v2 = p_map(nullptr, 1u, kMemGraphics, PAGE_READWRITE, &pfn);
        if (!v1 || !v2 || v1 == v2) return Fail("kernelx MapTitleVirtualMemory");
        const uint64_t magic = 0x5CA02002AABBCCDDull;
        std::memcpy(v1, &magic, sizeof(magic));
        uint64_t read_back = 0;
        std::memcpy(&read_back, v2, sizeof(read_back));
        if (read_back != magic) return Fail("kernelx physical aliasing mismatch");
        if (p_unmap(v1, 1u) != 1 || p_unmap(v2, 1u) != 1 || p_free(nullptr, 1u, &pfn) != 1) {
            return Fail("kernelx unmap/free");
        }
    }

    // =========================================================================
    // 4. xmem.dll (dynamic via hooked LoadLibraryA -> forwards to kernelx.dll!)
    // =========================================================================
    HMODULE h_xmem = ::LoadLibraryA("xmem.dll");
    if (!h_xmem) return Fail("LoadLibraryA(xmem.dll)", ::GetLastError());
    {
        auto fn_alloc = reinterpret_cast<PFN_XMemAlloc>(::GetProcAddress(h_xmem, "XMemAlloc"));
        auto fn_size = reinterpret_cast<PFN_XMemSize>(::GetProcAddress(h_xmem, "XMemSize"));
        auto fn_free = reinterpret_cast<PFN_XMemFree>(::GetProcAddress(h_xmem, "XMemFree"));
        auto fn_get_aux = reinterpret_cast<PFN_XMemGetAuxiliaryTitleMemory>(::GetProcAddress(h_xmem, "XMemGetAuxiliaryTitleMemory"));
        auto fn_rel_aux = reinterpret_cast<PFN_XMemReleaseAuxiliaryTitleMemory>(::GetProcAddress(h_xmem, "XMemReleaseAuxiliaryTitleMemory"));
        if (!fn_alloc || !fn_size || !fn_free || !fn_get_aux || !fn_rel_aux) {
            return Fail("xmem GetProcAddress");
        }
        const uint64_t attrs = MAKE_XALLOC_ATTRIBUTES(
            0, 0, 0, 0, 0, XALLOC_ALIGNMENT_4K, XALLOC_MEMTYPE_HEAP, XALLOC_PAGESIZE_4KB, 1);
        void* p = fn_alloc(8192u, attrs);
        if (!p || (reinterpret_cast<uintptr_t>(p) & 4095u) != 0u) return Fail("xmem XMemAlloc 4K");
        const auto* bytes = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < 8192u; ++i) {
            if (bytes[i] != 0u) return Fail("xmem zero-init violated");
        }
        if (fn_size(p, attrs) < 8192u) return Fail("xmem XMemSize");
        fn_free(p, attrs);

        size_t aux_sz = 0;
        void* aux = fn_get_aux(k64KiB, &aux_sz);
        if (!aux || aux_sz < k64KiB || fn_rel_aux(aux) == 0) return Fail("xmem aux memory");
    }

    // =========================================================================
    // 5. d3d12_xs.dll (DeviceX + live XS2\0 RDNA2 compute auto-lift + disk shader cache!)
    // =========================================================================
    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    {
        std::vector<uint8_t> cs_xs2;
        if (!ReadWholeFileW(L"G:\\compute_addmul.xs2", cs_xs2) || cs_xs2.size() < 32u) {
            return Fail("Read G:\\compute_addmul.xs2", ::GetLastError());
        }
        std::vector<uint32_t> cs_in(64u);
        for (uint32_t i = 0; i < 64u; ++i) {
            cs_in[i] = 0x11000000u | (i * 13u + 7u);
        }
        std::vector<uint32_t> cs_out;
        if (!RunXs2ComputeOnDeviceX(use_warp, cs_xs2, cs_in, true, gpu_info, cs_out)) {
            return Fail("d3d12_xs XS2 compute auto-lift");
        }
        for (uint32_t i = 0; i < 64u; ++i) {
            const uint32_t v3 = cs_in[i] * 5u + 17u;
            const uint32_t expected = (0x00FFFF00u & v3) | (~0x00FFFF00u & cs_in[i]);
            if (cs_out[i] != expected) {
                return Fail("d3d12_xs XS2 compute output mismatch", i);
            }
        }
    }

    // =========================================================================
    // 6. xg_xs.dll (2D mip-tail + 3D thick 64KB_S 6-mip volume incl. mip-tail)
    // =========================================================================
    {
        // 6a. 2D mip chain + mip tail
        XG_TEXTURE2D_DESC d2{};
        d2.Width = 128u;
        d2.Height = 128u;
        d2.MipLevels = 8u;
        d2.ArraySize = 2u;
        d2.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d2.SampleCount = 1u;
        d2.TileMode = XG_TILE_MODE_2D_THIN_64KB_S_X;
        XG_RESOURCE_LAYOUT l2{};
        if (XGComputeMipLayout(&d2, &l2) != 0 || l2.MipTailStartLevel >= l2.MipLevels) {
            return Fail("xg_xs XGComputeMipLayout 2D mip-tail");
        }
        std::vector<uint8_t> tiled2(static_cast<size_t>(l2.SizeBytes), 0xCCu);
        std::vector<uint8_t> mip0(128u * 128u * 4u);
        for (size_t i = 0; i < mip0.size(); ++i) mip0[i] = static_cast<uint8_t>((i * 41u + 9u) & 0xFFu);
        std::vector<uint8_t> out0(mip0.size(), 0u);
        if (XGTileSurface2D(&d2, 0u, 0u, mip0.data(), 128u * 4u, tiled2.data(), tiled2.size()) != 0 ||
            XGUntileSurface2D(&d2, 0u, 0u, tiled2.data(), tiled2.size(), out0.data(), 128u * 4u) != 0 ||
            std::memcmp(mip0.data(), out0.data(), mip0.size()) != 0) {
            return Fail("xg_xs 2D roundtrip");
        }

        // 6b. 3D volume (32x32x16, 6 mips, 3D_THICK_64KB_S including mip tail)
        XG_TEXTURE3D_DESC d3{};
        d3.Width = 32u;
        d3.Height = 32u;
        d3.Depth = 16u;
        d3.MipLevels = 6u;
        d3.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d3.BindFlags = XG_BIND_SHADER_RESOURCE;
        d3.TileMode = XG_TILE_MODE_3D_THICK_64KB_S;
        XG_RESOURCE_LAYOUT l3{};
        if (XGComputeMipLayout3D(&d3, &l3) != 0 ||
            l3.Dimension != XG_RESOURCE_DIMENSION_TEXTURE3D ||
            l3.MipTailStartLevel >= l3.MipLevels ||
            l3.Mips[l3.MipLevels - 1u].InMipTail != 1u) {
            return Fail("xg_xs XGComputeMipLayout3D");
        }
        IXGTextureAddressComputer* comp3 = nullptr;
        if (XGCreateTexture3DComputer(&d3, &comp3) != 0 || !comp3) {
            return Fail("xg_xs XGCreateTexture3DComputer");
        }
        std::vector<uint8_t> tiled3(static_cast<size_t>(l3.SizeBytes), 0xA5u);
        std::vector<std::vector<uint8_t>> mips3(d3.MipLevels);
        for (uint32_t m = 0; m < d3.MipLevels; ++m) {
            const uint32_t mw = std::max(1u, d3.Width >> m);
            const uint32_t mh = std::max(1u, d3.Height >> m);
            const uint32_t md = std::max(1u, d3.Depth >> m);
            const uint32_t rp = mw * 4u;
            const uint32_t sp = rp * mh;
            mips3[m].resize(static_cast<size_t>(sp) * md);
            for (size_t i = 0; i < mips3[m].size(); ++i) {
                mips3[m][i] = static_cast<uint8_t>((i * 29u + m * 17u + 3u) & 0xFFu);
            }
            if (comp3->CopyIntoSubresource(tiled3.data(), 0u, m, mips3[m].data(), rp, sp) != 0) {
                comp3->Release();
                return Fail("xg_xs 3D CopyIntoSubresource", m);
            }
        }
        for (uint32_t m = 0; m < d3.MipLevels; ++m) {
            const uint32_t mw = std::max(1u, d3.Width >> m);
            const uint32_t mh = std::max(1u, d3.Height >> m);
            const uint32_t rp = mw * 4u;
            const uint32_t sp = rp * mh;
            std::vector<uint8_t> dst(mips3[m].size(), 0u);
            if (comp3->CopyFromSubresource(dst.data(), 0u, m, tiled3.data(), rp, sp) != 0 ||
                std::memcmp(dst.data(), mips3[m].data(), dst.size()) != 0) {
                comp3->Release();
                return Fail("xg_xs 3D CopyFromSubresource bit-exact", m);
            }
        }
        // Direct C exports XGTileSurface3D / XGUntileSurface3D on mip 0
        {
            const uint32_t rp = d3.Width * 4u;
            const uint32_t sp = rp * d3.Height;
            std::vector<uint8_t> tc(static_cast<size_t>(l3.SizeBytes), 0u);
            std::vector<uint8_t> dc(mips3[0].size(), 0u);
            if (XGTileSurface3D(&d3, 0u, mips3[0].data(), rp, sp, tc.data(), tc.size()) != 0 ||
                XGUntileSurface3D(&d3, 0u, tc.data(), tc.size(), dc.data(), rp, sp) != 0 ||
                std::memcmp(dc.data(), mips3[0].data(), dc.size()) != 0) {
                comp3->Release();
                return Fail("xg_xs XGTileSurface3D/XGUntileSurface3D");
            }
        }
        comp3->Release();
    }

    // =========================================================================
    // 7. GameInput.dll
    // =========================================================================
    {
        ComPtr<IGameInput> gi;
        ComPtr<IGameInputReading> rd;
        ComPtr<IGameInputDevice> dev;
        GameInputGamepadState gp{};
        if (FAILED(GameInputCreate(gi.ReleaseAndGetAddressOf())) || !gi ||
            FAILED(gi->GetCurrentReading(GameInputKindGamepad, nullptr, rd.ReleaseAndGetAddressOf())) || !rd ||
            !rd->GetGamepadState(&gp)) {
            return Fail("GameInput");
        }
        rd->GetDevice(dev.ReleaseAndGetAddressOf());
        if (!dev) return Fail("GameInput GetDevice");
        GameInputRumbleParams r0{0.0f, 0.0f, 0.0f, 0.0f};
        dev->SetRumbleState(&r0);
    }

    // =========================================================================
    // 8 & 9. XAudio2_9.dll (PCM16 + XMA2 + IMA-ADPCM + Xbox ADPCM + Opus) + AcpHal.dll
    // =========================================================================
    {
        std::vector<uint8_t> xma_bytes, ima_bytes, xadpcm_bytes, opus_bytes;
        if (!ReadWholeFileW(L"G:\\sine440.xma2", xma_bytes) ||
            !ReadWholeFileW(L"G:\\tone_ima.adpcm", ima_bytes) ||
            !ReadWholeFileW(L"G:\\tone_xadpcm.adpcm", xadpcm_bytes) ||
            !ReadWholeFileW(L"G:\\tone_opus.opus", opus_bytes)) {
            return Fail("Read audio codec assets from G:\\");
        }

        ComPtr<IXAudio2> xa;
        if (FAILED(XAudio2Create(xa.ReleaseAndGetAddressOf(), 0u, XAUDIO2_DEFAULT_PROCESSOR)) || !xa) {
            return Fail("XAudio2Create");
        }
        IXAudio2MasteringVoice* master = nullptr;
        if (FAILED(xa->CreateMasteringVoice(&master, 2u, 48000u)) || !master) {
            return Fail("XAudio2 CreateMasteringVoice");
        }

        auto test_voice = [&](const XA2_WAVEFORMATEX* wfx, const uint8_t* data, size_t bytes, uint32_t frames) -> bool {
            IXAudio2SourceVoice* v = nullptr;
            if (FAILED(xa->CreateSourceVoice(&v, wfx)) || !v) return false;
            XAUDIO2_BUFFER b{};
            b.Flags = XAUDIO2_END_OF_STREAM;
            b.AudioBytes = static_cast<uint32_t>(bytes);
            b.pAudioData = data;
            if (FAILED(v->SubmitSourceBuffer(&b)) || FAILED(v->Start())) {
                v->DestroyVoice();
                return false;
            }
            std::vector<float> mix(static_cast<size_t>(frames) * 2u, 0.0f);
            const HRESULT hr = OpenScarlettXAudio2RenderMixForTest(xa.Get(), mix.data(), frames);
            v->DestroyVoice();
            if (FAILED(hr)) return false;
            double energy = 0.0;
            for (float s : mix) {
                if (!std::isfinite(s)) return false;
                energy += static_cast<double>(s) * static_cast<double>(s);
            }
            return energy > 0.001;
        };

        // 8a. PCM16
        {
            std::vector<int16_t> pcm(1024u * 2u);
            for (uint32_t i = 0; i < 1024u; ++i) {
                const int16_t s = static_cast<int16_t>(
                    0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f) * 32767.0f);
                pcm[i * 2u] = s;
                pcm[i * 2u + 1u] = s;
            }
            XA2_WAVEFORMATEX pf{};
            pf.wFormatTag = XA2_WAVE_FORMAT_PCM;
            pf.nChannels = 2u;
            pf.nSamplesPerSec = 48000u;
            pf.wBitsPerSample = 16u;
            pf.nBlockAlign = 4u;
            pf.nAvgBytesPerSec = 48000u * 4u;
            if (!test_voice(&pf, reinterpret_cast<const uint8_t*>(pcm.data()),
                            pcm.size() * sizeof(int16_t), 1024u)) {
                return Fail("XAudio2 PCM16 voice");
            }
        }
        // 8b. XMA2 (0x0166)
        {
            XA2_XMA2WAVEFORMATEX xf{};
            xf.wfx.wFormatTag = XA2_WAVE_FORMAT_XMA2;
            xf.wfx.nChannels = 1u;
            xf.wfx.nSamplesPerSec = 48000u;
            xf.wfx.wBitsPerSample = 16u;
            xf.wfx.cbSize = sizeof(XA2_XMA2WAVEFORMATEX) - sizeof(XA2_WAVEFORMATEX);
            xf.NumStreams = 1u;
            xf.SamplesEncoded = 4096u;
            xf.BytesPerBlock = 2048u;
            xf.BlockCount = static_cast<uint16_t>(xma_bytes.size() / 2048u);
            if (!test_voice(&xf.wfx, xma_bytes.data(), xma_bytes.size(), 1024u)) {
                return Fail("XAudio2 XMA2 voice");
            }
        }
        // 8c. IMA-ADPCM (0x0011)
        {
            XA2_IMAADPCMWAVEFORMAT ifmt{};
            ifmt.wfx.wFormatTag = XA2_WAVE_FORMAT_IMA_ADPCM;
            ifmt.wfx.nChannels = 2u;
            ifmt.wfx.nSamplesPerSec = 48000u;
            ifmt.wfx.nBlockAlign = 512u;
            ifmt.wfx.wBitsPerSample = 4u;
            ifmt.wfx.cbSize = 2u;
            ifmt.wSamplesPerBlock = 505u;
            if (!test_voice(&ifmt.wfx, ima_bytes.data(), ima_bytes.size(), 1024u)) {
                return Fail("XAudio2 IMA-ADPCM 0x0011 voice");
            }
        }
        // 8d. Xbox ADPCM (0x0069)
        {
            XA2_IMAADPCMWAVEFORMAT xafmt{};
            xafmt.wfx.wFormatTag = XA2_WAVE_FORMAT_XBOX_ADPCM;
            xafmt.wfx.nChannels = 1u;
            xafmt.wfx.nSamplesPerSec = 48000u;
            xafmt.wfx.nBlockAlign = 36u;
            xafmt.wfx.wBitsPerSample = 4u;
            xafmt.wfx.cbSize = 2u;
            xafmt.wSamplesPerBlock = 65u;
            if (!test_voice(&xafmt.wfx, xadpcm_bytes.data(), xadpcm_bytes.size(), 1024u)) {
                return Fail("XAudio2 Xbox ADPCM 0x0069 voice");
            }
        }
        // 8e. Opus (0x704F)
        {
            XA2_OPUSWAVEFORMAT ofmt{};
            ofmt.wfx.wFormatTag = XA2_WAVE_FORMAT_OPUS;
            ofmt.wfx.nChannels = 2u;
            ofmt.wfx.nSamplesPerSec = 48000u;
            ofmt.wfx.wBitsPerSample = 16u;
            ofmt.wfx.cbSize = sizeof(XA2_OPUSWAVEFORMAT) - sizeof(XA2_WAVEFORMATEX);
            ofmt.PreSkip = 0u;
            ofmt.Version = 1u;
            if (!test_voice(&ofmt.wfx, opus_bytes.data(), opus_bytes.size(), 2048u)) {
                return Fail("XAudio2 Opus 0x704F voice");
            }
        }
        master->DestroyVoice();

        // 9. AcpHal.dll
        AcpHalShapeContexts req{4, 2, 1, 1, nullptr};
        AcpHalShapeContexts alloc{};
        if (FAILED(AcpHalAllocateShapeContexts(&req, &alloc))) return Fail("AcpHalAllocateShapeContexts");
        ComPtr<IAcpHal> acp;
        if (FAILED(AcpHalCreate(acp.ReleaseAndGetAddressOf())) || !acp) return Fail("AcpHalCreate");
        AcpHalXmaContextDesc xdesc{48000u, 1u, 512u, 4u, 12u, 0u};
        std::vector<float> acp_pcm(8192u, 0.0f);
        uint32_t written = 0;
        AcpHalCommand cmd{};
        cmd.type = AcpHalCommand_XmaDecode;
        cmd.contextIndex = 0u;
        cmd.inputData = xma_bytes.data();
        cmd.inputBytes = static_cast<uint32_t>(xma_bytes.size());
        cmd.outputFloatPcm = acp_pcm.data();
        cmd.outputCapacitySamples = static_cast<uint32_t>(acp_pcm.size());
        cmd.outSamplesWritten = &written;
        cmd.xmaConfig = xdesc;
        uint64_t pkt_cnt = 0;
        if (FAILED(acp->ConfigureXmaContext(0u, &xdesc)) ||
            FAILED(acp->SubmitCommand(&cmd)) ||
            FAILED(acp->WaitIdle()) ||
            FAILED(acp->GetDecodedPacketCount(0u, &pkt_cnt)) ||
            written < 4096u || pkt_cnt == 0ull) {
            return Fail("AcpHal XMA2 decode");
        }
        acp.Reset();
        if (FAILED(AcpHalReleaseShapeContexts(&alloc))) return Fail("AcpHalReleaseShapeContexts");
    }

    // =========================================================================
    // 10. dstorage_xs.dll (GDeflate + half-full auto-submit + cancel isolation)
    // =========================================================================
    {
        ComPtr<IDStorageFactoryX1> factory;
        if (FAILED(DStorageGetFactory(__uuidof(IDStorageFactoryX1),
                                      reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
            return Fail("dstorage_xs DStorageGetFactory");
        }
        ComPtr<IDStorageFileX> f_raw, f_gdef;
        if (FAILED(factory->OpenFile(L"G:\\asset.bin", __uuidof(IDStorageFileX),
                                     reinterpret_cast<void**>(f_raw.ReleaseAndGetAddressOf()))) ||
            FAILED(factory->OpenFile(L"G:\\asset.gdeflate", __uuidof(IDStorageFileX),
                                     reinterpret_cast<void**>(f_gdef.ReleaseAndGetAddressOf())))) {
            return Fail("dstorage_xs OpenFile");
        }
        DSTORAGE_FILE_INFO raw_i{}, gdef_i{};
        if (FAILED(f_raw->GetFileInformation(&raw_i)) || FAILED(f_gdef->GetFileInformation(&gdef_i))) {
            return Fail("dstorage_xs GetFileInformation");
        }

        DSTORAGE_QUEUE_DESC qd{DSTORAGE_REQUEST_SOURCE_FILE, DSTORAGE_MIN_QUEUE_CAPACITY,
                               DSTORAGE_PRIORITY_NORMAL, "fs_q", nullptr};
        ComPtr<IDStorageQueueX1> q;
        ComPtr<IDStorageStatusArrayX> st;
        if (FAILED(factory->CreateQueue(&qd, __uuidof(IDStorageQueueX1),
                                        reinterpret_cast<void**>(q.ReleaseAndGetAddressOf()))) ||
            FAILED(factory->CreateStatusArray(2u, "fs_st", __uuidof(IDStorageStatusArrayX),
                                              reinterpret_cast<void**>(st.ReleaseAndGetAddressOf())))) {
            return Fail("dstorage_xs CreateQueue/StatusArray");
        }
        std::vector<uint8_t> raw_out(16384u, 0u);
        std::vector<uint8_t> gdef_out(65536u, 0u);
        DSTORAGE_REQUEST r0{};
        r0.Options = {DSTORAGE_COMPRESSION_FORMAT_NONE, DSTORAGE_REQUEST_SOURCE_FILE,
                      DSTORAGE_REQUEST_DESTINATION_MEMORY, {}};
        r0.Source.File = {f_raw.Get(), 0ull, 16384u};
        r0.Destination.Memory = {raw_out.data(), 16384u};
        r0.UncompressedSize = 16384u;
        q->EnqueueRequest(&r0);

        DSTORAGE_REQUEST r1{};
        r1.Options = {DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, DSTORAGE_REQUEST_SOURCE_FILE,
                      DSTORAGE_REQUEST_DESTINATION_MEMORY, {}};
        r1.Source.File = {f_gdef.Get(), 0ull, static_cast<uint32_t>(gdef_i.FileSize)};
        r1.Destination.Memory = {gdef_out.data(), 65536u};
        r1.UncompressedSize = 65536u;
        q->EnqueueRequest(&r1);

        // Cancelled failing request (isolated from FailureCount & error_event)
        DSTORAGE_REQUEST r_bad{};
        r_bad.Options = {DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, DSTORAGE_REQUEST_SOURCE_FILE,
                         DSTORAGE_REQUEST_DESTINATION_MEMORY, {}};
        r_bad.Source.File = {f_raw.Get(), 0ull, 32u};
        r_bad.Destination.Memory = {gdef_out.data(), 32u};
        r_bad.UncompressedSize = 32u;
        r_bad.CancellationTag = 0xBEEF0001ull;
        q->EnqueueRequest(&r_bad);
        q->CancelRequestsWithTag(0xFFFF0000ull, 0xBEEF0000ull);

        HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        q->EnqueueStatus(st.Get(), 0u);
        q->EnqueueSetEvent(ev);
        q->Submit();
        const DWORD w = ::WaitForSingleObject(ev, 10000u);
        ::CloseHandle(ev);
        if (w != WAIT_OBJECT_0 || !st->IsComplete(0u) || FAILED(st->GetHResult(0u))) {
            return Fail("dstorage_xs file+GDeflate+cancel");
        }
        DSTORAGE_ERROR_RECORD er{};
        q->RetrieveErrorRecord(&er);
        if (er.FailureCount != 0u) return Fail("dstorage_xs FailureCount != 0");
        for (size_t i = 0; i < raw_out.size(); ++i) {
            if (raw_out[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xFFu)) return Fail("dstorage_xs raw mismatch");
        }
        for (size_t i = 0; i < gdef_out.size(); ++i) {
            if (gdef_out[i] != static_cast<uint8_t>(((i * 5u) + (i / 64u) * 19u) & 0xFFu)) {
                return Fail("dstorage_xs GDeflate mismatch");
            }
        }

        // Half-full auto-submit WITHOUT Submit()
        DSTORAGE_QUEUE_DESC mqd{DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_MIN_QUEUE_CAPACITY,
                                DSTORAGE_PRIORITY_REALTIME, "fs_auto", nullptr};
        ComPtr<IDStorageQueueX1> mq;
        if (FAILED(factory->CreateQueue(&mqd, __uuidof(IDStorageQueueX1),
                                        reinterpret_cast<void**>(mq.ReleaseAndGetAddressOf())))) {
            return Fail("dstorage_xs auto-submit queue");
        }
        constexpr uint32_t kHalf = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;
        std::vector<std::array<uint8_t, 16>> ms(kHalf), md(kHalf);
        for (uint32_t i = 0; i < kHalf; ++i) {
            ms[i].fill(static_cast<uint8_t>(i + 1u));
            md[i].fill(0u);
        }
        HANDLE aev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (uint32_t i = 0; i < kHalf - 1u; ++i) {
            DSTORAGE_REQUEST mr{};
            mr.Options = {DSTORAGE_COMPRESSION_FORMAT_NONE, DSTORAGE_REQUEST_SOURCE_MEMORY,
                          DSTORAGE_REQUEST_DESTINATION_MEMORY, {}};
            mr.Source.Memory = {ms[i].data(), 16u};
            mr.Destination.Memory = {md[i].data(), 16u};
            mr.UncompressedSize = 16u;
            mq->EnqueueRequest(&mr);
        }
        mq->EnqueueStatus(st.Get(), 1u);
        mq->EnqueueSetEvent(aev);
        {
            DSTORAGE_REQUEST mr{};
            mr.Options = {DSTORAGE_COMPRESSION_FORMAT_NONE, DSTORAGE_REQUEST_SOURCE_MEMORY,
                          DSTORAGE_REQUEST_DESTINATION_MEMORY, {}};
            mr.Source.Memory = {ms[kHalf - 1u].data(), 16u};
            mr.Destination.Memory = {md[kHalf - 1u].data(), 16u};
            mr.UncompressedSize = 16u;
            mq->EnqueueRequest(&mr);
        }
        const DWORD aw = ::WaitForSingleObject(aev, 5000u);
        ::CloseHandle(aev);
        if (aw != WAIT_OBJECT_0 || !st->IsComplete(1u) || FAILED(st->GetHResult(1u))) {
            return Fail("dstorage_xs auto-submit");
        }
        mq->Close();
        q->Close();
        f_gdef->Close();
        f_raw->Close();
    }

    // =========================================================================
    // 11. winrt_xs.dll
    // =========================================================================
    {
        if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return Fail("winrt_xs RoInitialize");
        const wchar_t cls_name[] = L"Windows.System.Profile.AnalyticsInfo";
        HSTRING hs = nullptr;
        if (FAILED(WindowsCreateString(cls_name, 36u, &hs)) || !hs || WindowsGetStringLen(hs) != 36u) {
            return Fail("winrt_xs WindowsCreateString");
        }
        UINT32 raw_len = 0;
        if (!WindowsGetStringRawBuffer(hs, &raw_len) || raw_len != 36u) {
            return Fail("winrt_xs WindowsGetStringRawBuffer");
        }
        XS_IAnalyticsInfoStatics* statics = nullptr;
        if (FAILED(RoGetActivationFactory(hs, XS_IID_IAnalyticsInfoStatics,
                                          reinterpret_cast<void**>(&statics))) ||
            !statics) {
            return Fail("winrt_xs RoGetActivationFactory");
        }
        HSTRING form = nullptr;
        if (FAILED(statics->lpVtbl->get_DeviceForm(statics, &form)) || !form) {
            return Fail("winrt_xs get_DeviceForm");
        }
        WindowsDeleteString(form);
        statics->lpVtbl->Release(statics);
        WindowsDeleteString(hs);
        RoUninitialize();
    }

    // =========================================================================
    // 12. Microsoft.Xbox.Services.GDK.C.dll (XSAPI)
    // =========================================================================
    {
        XblInitArgs init{nullptr, "00000000-0000-0000-0000-00005CA02002"};
        if (FAILED(XblInitialize(&init))) return Fail("XSAPI XblInitialize");
        XblContextHandle xctx = nullptr;
        if (FAILED(XblContextCreateHandle(user, &xctx)) || !xctx) return Fail("XSAPI XblContextCreateHandle");

        XAsyncBlock a_ab{};
        if (FAILED(XblAchievementsUpdateAchievementAsync(xctx, xuid, "ach_wave2_19dll", 100u, &a_ab)) ||
            FAILED(XAsyncGetStatus(&a_ab, true))) {
            return Fail("XSAPI XblAchievementsUpdateAchievementAsync");
        }
        XAsyncBlock p_ab{};
        XblUserProfile prof{};
        if (FAILED(XblProfileGetUserProfileAsync(xctx, xuid, &p_ab)) ||
            FAILED(XblProfileGetUserProfileResult(&p_ab, &prof)) ||
            prof.xboxUserId != xuid || prof.gamertag[0] == '\0') {
            return Fail("XSAPI XblProfileGetUserProfile");
        }
        XAsyncBlock pr_ab{};
        if (FAILED(XblPresenceSetPresenceAsync(xctx, true, nullptr, &pr_ab)) ||
            FAILED(XAsyncGetStatus(&pr_ab, true))) {
            return Fail("XSAPI XblPresenceSetPresenceAsync");
        }
        XblContextCloseHandle(xctx);
        XAsyncBlock c_ab{};
        if (FAILED(XblCleanupAsync(&c_ab)) || FAILED(XAsyncGetStatus(&c_ab, true))) {
            return Fail("XSAPI XblCleanupAsync");
        }
    }

    // =========================================================================
    // 13. xcrdapi.dll
    // =========================================================================
    {
        if (XCrdInitialize(1u) != 0) return Fail("xcrdapi XCrdInitialize");
        if (XCrdSetBreadcrumb(0u, "FullStack19", 0x5CA02002ull) != 0) return Fail("xcrdapi SetBreadcrumb");
        char tag[64]{};
        uint64_t val = 0;
        if (XCrdGetBreadcrumb(0u, tag, sizeof(tag), &val) != 0 ||
            std::strcmp(tag, "FullStack19") != 0 || val != 0x5CA02002ull) {
            return Fail("xcrdapi GetBreadcrumb");
        }
        if (XCrdReportError(0x00010001u, "FullStack", "Synthetic telemetry check") != 0) {
            return Fail("xcrdapi ReportError");
        }
        XCrdDiagnosticInfo di{};
        di.structSize = sizeof(di);
        if (XCrdGetDiagnosticInfo(&di) != 0 || di.initialized != 1u || di.breadcrumbCount < 1u) {
            return Fail("xcrdapi GetDiagnosticInfo");
        }
        const std::string t_root = ReadEnvA("OPENSCARLETT_VFS_T");
        const std::string diag_out = t_root.empty() ? std::string{} : (t_root + "\\xcrd_fullstack.json");
        if (XCrdFlushDiagnostics(diag_out.empty() ? nullptr : diag_out.c_str()) != 0) {
            return Fail("xcrdapi FlushDiagnostics");
        }
        XCrdUninitialize();
    }

    // =========================================================================
    // 14. libHttpClient.GDK.dll
    // =========================================================================
    {
        HCInitArgs hca{};
        if (FAILED(HCInitialize(&hca))) return Fail("libHttpClient HCInitialize");
        const char* ver = nullptr;
        if (FAILED(HCGetLibVersion(&ver)) || !ver || !*ver) return Fail("libHttpClient HCGetLibVersion");
        HCTraceSetTraceToDebugger(false);
        if (FAILED(HCSettingsSetTraceLevel(HCTraceLevel::Warning))) return Fail("libHttpClient SetTraceLevel");

        HCCallHandle call = nullptr;
        if (FAILED(HCHttpCallCreate(&call)) || !call ||
            FAILED(HCHttpCallRequestSetUrl(call, "GET", "https://api.openscarlett.local/v2/bringup")) ||
            FAILED(HCHttpCallRequestSetHeader(call, "X-TitleId", "5CA02002", true))) {
            return Fail("libHttpClient request setup");
        }
        XAsyncBlock hab{};
        if (FAILED(HCHttpCallPerformAsync(call, &hab)) || FAILED(XAsyncGetStatus(&hab, true))) {
            return Fail("libHttpClient HCHttpCallPerformAsync");
        }
        uint32_t sc = 0;
        const char* body = nullptr;
        if (FAILED(HCHttpCallResponseGetStatusCode(call, &sc)) || sc != 200u ||
            FAILED(HCHttpCallResponseGetResponseString(call, &body)) || !body) {
            return Fail("libHttpClient response");
        }
        HCHttpCallCloseHandle(call);
        XAsyncBlock cab{};
        if (FAILED(HCCleanupAsync(&cab)) || FAILED(XAsyncGetStatus(&cab, true))) {
            return Fail("libHttpClient HCCleanupAsync");
        }
    }

    // =========================================================================
    // 15. XCurl.dll
    // =========================================================================
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || !curl_version()) {
            return Fail("XCurl curl_global_init");
        }
        CURL* easy = curl_easy_init();
        if (!easy) return Fail("XCurl curl_easy_init");
        curl_slist* hdrs = curl_slist_append(nullptr, "Accept: application/json");
        std::string resp;
        if (curl_easy_setopt(easy, CURLOPT_URL, "https://title.openscarlett.local/fullstack") != CURLE_OK ||
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs) != CURLE_OK ||
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlWriteCb) != CURLE_OK ||
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &resp) != CURLE_OK ||
            curl_easy_perform(easy) != CURLE_OK) {
            return Fail("XCurl easy perform");
        }
        long code = 0;
        if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK || code != 200L || resp.empty()) {
            return Fail("XCurl easy getinfo");
        }

        // Multi transfer
        curl_easy_reset(easy);
        std::string mresp;
        curl_easy_setopt(easy, CURLOPT_URL, "https://title.openscarlett.local/multi");
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlWriteCb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &mresp);
        CURLM* multi = curl_multi_init();
        int32_t running = -1;
        int32_t in_q = 0;
        if (!multi || curl_multi_add_handle(multi, easy) != CURLM_OK ||
            curl_multi_perform(multi, &running) != CURLM_OK || running != 0) {
            return Fail("XCurl multi perform");
        }
        CURLMsg* msg = curl_multi_info_read(multi, &in_q);
        if (!msg || msg->msg != CURLMSG_DONE || msg->data.result != CURLE_OK || mresp.empty()) {
            return Fail("XCurl multi info");
        }
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        curl_slist_free_all(hdrs);
        curl_global_cleanup();
    }

    // =========================================================================
    // 16. Bink2Scarlett.dll
    // =========================================================================
    {
        std::vector<uint8_t> bk2_bytes;
        if (!ReadWholeFileW(L"G:\\clip.bk2", bk2_bytes) || bk2_bytes.empty()) {
            return Fail("Read G:\\clip.bk2", ::GetLastError());
        }
        HBink bink = BinkOpenBuffer(bk2_bytes.data(), bk2_bytes.size(), 0u);
        if (!bink || bink->Width != 32u || bink->Height != 32u || bink->Frames < 2u) {
            return Fail("Bink2Scarlett BinkOpenBuffer");
        }
        BinkSummary bsum{};
        BinkGetSummary(bink, &bsum);
        if (bsum.IsBink2 != 1u || std::strcmp(bsum.FourCC, "KB2i") != 0) {
            BinkClose(bink);
            return Fail("Bink2Scarlett BinkGetSummary");
        }
        std::vector<uint8_t> rgba(32u * 32u * 4u, 0u);
        std::vector<uint8_t> bgra(32u * 32u * 4u, 0u);
        if (BinkDoFrame(bink) != 0 ||
            BinkCopyToBuffer(bink, rgba.data(), 32 * 4, 32u, 0u, 0u, BINKSURFACE32RGBA) != 0 ||
            BinkCopyToBuffer(bink, bgra.data(), 32 * 4, 32u, 0u, 0u, BINKSURFACE32) != 0 ||
            bgra[0] != rgba[2] || bgra[2] != rgba[0]) {
            BinkClose(bink);
            return Fail("Bink2Scarlett decode/copy");
        }
        BinkNextFrame(bink);
        BinkGoto(bink, 1u, 0u);
        BinkClose(bink);
    }

    // =========================================================================
    // 17 & 18. amd_fidelityfx_loader_xbox.dll -> amd_fidelityfx_upscaler_xbox.dll
    // =========================================================================
    {
        const uint32_t uver = ffxUpscalerGetVersion();
        if (uver == 0u) return Fail("ffxUpscalerGetVersion");

        uint64_t vcnt = 1ull;
        uint64_t vids[1]{};
        const char* vnames[1]{};
        FfxQueryDescGetVersions vq{};
        vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        vq.outputCount = &vcnt;
        vq.versionIds = vids;
        vq.versionNames = vnames;
        if (ffxQuery(nullptr, &vq.header) != FFX_API_RETURN_OK || vids[0] != uver) {
            return Fail("ffxQuery GET_VERSIONS");
        }

        uint32_t rw = 0, rh = 0;
        FfxQueryDescUpscaleGetRenderResolutionFromQualityMode rq{};
        rq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        rq.displayWidth = 1920u;
        rq.displayHeight = 1080u;
        rq.qualityMode = FFX_UPSCALER_QUALITY_QUALITY;
        rq.pOutRenderWidth = &rw;
        rq.pOutRenderHeight = &rh;
        if (ffxQuery(nullptr, &rq.header) != FFX_API_RETURN_OK || rw != 1280u || rh != 720u) {
            return Fail("ffxQuery GETRENDERRESOLUTION");
        }

        constexpr uint32_t kInW = 32u, kInH = 32u, kOutW = 64u, kOutH = 64u;
        FfxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.flags = use_warp ? (FFX_UPSCALER_FLAG_PREFER_WARP | FFX_UPSCALER_FLAG_ENABLE_RCAS)
                            : FFX_UPSCALER_FLAG_ENABLE_RCAS;
        cd.maxRenderWidth = kInW;
        cd.maxRenderHeight = kInH;
        cd.maxUpscaleWidth = kOutW;
        cd.maxUpscaleHeight = kOutH;
        cd.defaultSharpness = 0.8f;

        FfxContext fctx = nullptr;
        if (ffxCreateContext(&fctx, &cd.header, nullptr) != FFX_API_RETURN_OK || !fctx) {
            return Fail("ffxCreateContext");
        }
        float sharp = 0.85f;
        FfxConfigureDescUpscaleKeyValue kv{};
        kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        kv.key = FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS;
        kv.ptr = &sharp;
        if (ffxConfigure(&fctx, &kv.header) != FFX_API_RETURN_OK) {
            ffxDestroyContext(&fctx, nullptr);
            return Fail("ffxConfigure");
        }
        FfxQueryDescUpscaleGetAdapterInfo aq{};
        aq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETADAPTERINFO;
        if (ffxQuery(&fctx, &aq.header) != FFX_API_RETURN_OK || aq.adapterDescription[0] == '\0') {
            ffxDestroyContext(&fctx, nullptr);
            return Fail("ffxQuery GETADAPTERINFO");
        }

        std::vector<uint8_t> in_img(kInW * kInH * 4u);
        for (uint32_t y = 0; y < kInH; ++y) {
            for (uint32_t x = 0; x < kInW; ++x) {
                uint8_t* p = &in_img[(y * kInW + x) * 4u];
                const bool chk = (((x / 4u) + (y / 4u)) & 1u) != 0u;
                p[0] = chk ? 240u : 20u;
                p[1] = chk ? 180u : 60u;
                p[2] = chk ? 40u : 220u;
                p[3] = 255u;
            }
        }
        std::vector<uint8_t> out_img(kOutW * kOutH * 4u, 0u);
        FfxUpscalerDispatchResult d_res{};
        FfxDispatchDescUpscale dd{};
        dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dd.inputRgba8 = in_img.data();
        dd.inputWidth = kInW;
        dd.inputHeight = kInH;
        dd.outputRgba8 = out_img.data();
        dd.outputBufferBytes = out_img.size();
        dd.outputWidth = kOutW;
        dd.outputHeight = kOutH;
        dd.sharpness = -1.0f;
        dd.enableRcas = 0xFFFFFFFFu;
        dd.outResult = &d_res;
        if (ffxDispatch(&fctx, &dd.header) != FFX_API_RETURN_OK ||
            d_res.usedGpuCompute != 1u || d_res.outputFnv1aHash == 0ull) {
            ffxDestroyContext(&fctx, nullptr);
            return Fail("ffxDispatch live GPU FSR1");
        }
        if (ffxDestroyContext(&fctx, nullptr) != FFX_API_RETURN_OK) {
            return Fail("ffxDestroyContext");
        }
    }

    // =========================================================================
    // 19. XboxBvhBuilder.dll
    // =========================================================================
    uint64_t bvh_hash = 0;
    {
        XboxBvhCreateDesc cd{};
        cd.preferWarp = use_warp ? 1u : 0u;
        XboxBvhBuilderHandle bvh = nullptr;
        if (XboxBvhCreateBuilder(&cd, &bvh) != 0 || !bvh) return Fail("XboxBvhCreateBuilder");

        XboxBvhCapabilities caps{};
        if (XboxBvhGetCapabilities(bvh, &caps) != 0 || caps.adapterDescription[0] == '\0') {
            XboxBvhDestroyBuilder(bvh);
            return Fail("XboxBvhGetCapabilities");
        }
        XboxBvhTriangle tris[2]{};
        // Floor quad / two triangles facing camera
        tris[0] = {{-2.0f, -1.0f, 2.0f}, {2.0f, -1.0f, 2.0f}, {0.0f, 2.0f, 2.0f}, {0.8f, 0.2f, 0.2f}, 0.2f};
        tris[1] = {{-2.0f, -1.0f, 2.5f}, {2.0f, -1.0f, 2.5f}, {0.0f, 1.5f, 2.5f}, {0.2f, 0.8f, 0.3f}, 0.1f};

        XboxBvhPrebuildInputs pin{XBOX_BVH_AS_TYPE_BOTTOM_LEVEL, XBOX_BVH_BUILD_ALLOW_UPDATE, 2u, 1u, 12u, 1u};
        XboxBvhPrebuildInfo pinfo{};
        uint64_t blas = 0, tlas = 0;
        if (XboxBvhGetPrebuildInfo(bvh, &pin, &pinfo) != 0 || pinfo.resultDataMaxSizeBytes == 0ull ||
            XboxBvhBuildBlas(bvh, tris, 2u, XBOX_BVH_BUILD_ALLOW_UPDATE, &blas) != 0 || blas == 0ull) {
            XboxBvhDestroyBuilder(bvh);
            return Fail("XboxBvhBuildBlas");
        }

        XboxBvhInstanceDesc inst{};
        inst.blasId = blas;
        inst.transform[0][0] = 1.0f;
        inst.transform[1][1] = 1.0f;
        inst.transform[2][2] = 1.0f;
        inst.instanceId = 0u;
        inst.instanceMask = 0xFFu;
        if (XboxBvhBuildTlas(bvh, &inst, 1u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas) != 0 || tlas == 0ull) {
            XboxBvhDestroyBuilder(bvh);
            return Fail("XboxBvhBuildTlas");
        }

        XboxBvhRay ray{{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}, 0.001f, 50.0f};
        XboxBvhHit hit{};
        if (XboxBvhTraceRay(bvh, tlas, &ray, &hit) != 0 || hit.hit != 1u) {
            XboxBvhDestroyBuilder(bvh);
            return Fail("XboxBvhTraceRay");
        }

        XboxBvhDispatchDesc dd{32u, 32u, 1u, {0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 2.0f}, {1.0f, 2.0f, -1.0f}};
        std::vector<uint8_t> rt_out(32u * 32u * 4u, 0u);
        XboxBvhDispatchStats st1{}, st2{};
        if (XboxBvhDispatchRays(bvh, tlas, &dd, rt_out.data(), rt_out.size(), &st1) != 0 ||
            st1.primaryHits == 0ull || st1.frameHash == 0ull) {
            XboxBvhDestroyBuilder(bvh);
            return Fail("XboxBvhDispatchRays");
        }
        bvh_hash = st1.frameHash;

        tris[0].color[0] = 0.1f;
        tris[0].color[2] = 0.95f;
        uint64_t tlas2 = 0;
        if (XboxBvhUpdateBlas(bvh, blas, tris, 2u) != 0 ||
            XboxBvhBuildTlas(bvh, &inst, 1u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas2) != 0 ||
            XboxBvhDispatchRays(bvh, tlas2, &dd, rt_out.data(), rt_out.size(), &st2) != 0 ||
            st2.frameHash == st1.frameHash) {
            XboxBvhDestroyBuilder(bvh);
            return Fail("XboxBvhUpdateBlas");
        }
        XboxBvhDestroyBuilder(bvh);
    }

    // =========================================================================
    // 20. CoW G:\ / GAME:\ Write Protection Verification
    // =========================================================================
    {
        std::string txt;
        if (!ReadTextFileW(L"G:\\cow_base.txt", txt) || txt != "ORIGINAL_PACKAGE_V1") {
            return Fail("CoW initial G:\\cow_base.txt read");
        }
        // In-place CoW write into G:\cow_base.txt
        HANDLE hm = ::CreateFileW(L"G:\\cow_base.txt", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hm == INVALID_HANDLE_VALUE) return Fail("CoW open G:\\cow_base.txt for write", ::GetLastError());
        ::SetFilePointer(hm, 0, nullptr, FILE_BEGIN);
        const char* patched = "OVERLAY_";
        DWORD wr = 0;
        const BOOL wok = ::WriteFile(hm, patched, 8u, &wr, nullptr);
        ::CloseHandle(hm);
        if (!wok || wr != 8u) return Fail("CoW WriteFile G:\\cow_base.txt");
        if (!ReadTextFileW(L"GAME:\\cow_base.txt", txt) || txt != "OVERLAY__PACKAGE_V1") {
            return Fail("CoW readback GAME:\\cow_base.txt mismatch");
        }

        // Create new directory + file under GAME:\ and read back via G:\ (must stay in overlay!)
        if (!::CreateDirectoryW(L"GAME:\\cow_new_dir", nullptr) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS) {
            return Fail("CoW CreateDirectoryW", ::GetLastError());
        }
        if (!WriteWholeFileW(L"GAME:\\cow_new_dir\\created.txt", "COW_CREATED", 11u) ||
            !ReadTextFileW(L"G:\\cow_new_dir\\created.txt", txt) || txt != "COW_CREATED") {
            return Fail("CoW create/read GAME:\\cow_new_dir\\created.txt");
        }

        // Move base file -> whiteout source, verify destination
        if (!::MoveFileExW(L"G:\\cow_move_src.txt", L"GAME:\\cow_move_dst.txt", MOVEFILE_REPLACE_EXISTING) ||
            ::GetFileAttributesW(L"G:\\cow_move_src.txt") != INVALID_FILE_ATTRIBUTES ||
            !ReadTextFileW(L"G:\\cow_move_dst.txt", txt) || txt != "MOVE_SRC_DATA") {
            return Fail("CoW MoveFileExW");
        }

        // Delete base file -> whiteout verified, original package untouched on disk!
        if (!::DeleteFileW(L"G:\\cow_delete_me.txt") ||
            ::GetFileAttributesW(L"GAME:\\cow_delete_me.txt") != INVALID_FILE_ATTRIBUTES ||
            ::GetLastError() != ERROR_FILE_NOT_FOUND) {
            return Fail("CoW DeleteFileW whiteout");
        }
    }

    // Write metrics to T:\full_stack_metrics.txt
    char metrics[512]{};
    const int n = std::snprintf(
        metrics, sizeof(metrics),
        "sample=ExternalFullStack19\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "title_id=0x%08X\n"
        "package_id=%s\n"
        "bvh_hash=0x%016llX\n"
        "dlls_exercised=19\n"
        "cow_vfs=ok\n",
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        title_id,
        pkg_id,
        static_cast<unsigned long long>(bvh_hash));
    if (n <= 0 || !WriteWholeFileW(L"T:\\full_stack_metrics.txt", metrics, static_cast<size_t>(n))) {
        return Fail("Write T:\\full_stack_metrics.txt");
    }

    XUserCloseHandle(user);
    UninitializeApiImpl();
    XGameRuntimeUninitialize();
    ::FreeLibrary(h_xmem);
    ::FreeLibrary(h_kernelx);

    std::printf("[gdk_external_full_stack] PASS (adapter=%s, TitleId=0x%08X, 19/19 DLLs + CoW OK)\n",
                gpu_info.AdapterName, title_id);
    return 0;
}
