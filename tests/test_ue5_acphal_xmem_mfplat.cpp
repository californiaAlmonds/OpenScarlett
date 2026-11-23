// OpenScarlett UE5 Xbox Series X Import Bringup Test Suite:
//   1. AcpHal.dll — ApuCreateHeap, ApuAlloc, ApuFree, ApuHeapGetState,
//                   ApuMapVirtualAddress, ApuMapApuAddress, ApuIsVirtualAddressValid
//                   + concurrent multi-thread stress test
//   2. xmem.dll   — All 18 exports, XMemCheckDefaultHeaps, XMemGetWorkingSetStatistics,
//                   XMemGetAuxiliaryTitleMemory/Release, XMemAllocDefault/SizeDefault/FreeDefault,
//                   and XMemSetAllocationHooks (verifying hook invocation vs *Default bypass)
//   3. mfplat_xs.dll — All 46 System32-forwarded MF exports + 5 Xbox/telemetry exports
//                      and full live Media Foundation object/workqueue/WAVEFORMATEX round-trip.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <mfobjects.h>
#  include <mmreg.h>
#  include <objbase.h>
#endif

#include "os_check.h"

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)

fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0u && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

// ===========================================================================
// 1. AcpHal.dll — APU Heap Allocator, Mapping & Concurrent Stress Test
// ===========================================================================
struct TestApuHeapState {
    uint32_t bytesAllocated;
    uint32_t bytesFree;
    uint32_t bytesLost;
    uint32_t maximumBlockSizeAvailable;
    uint32_t allocationCount;
};

void TestAcpHalApuHeap() {
    std::printf("[Test 1] AcpHal.dll: ApuCreateHeap, ApuAlloc, ApuFree, ApuMap*, ApuHeapGetState + MT stress...\n");

    const fs::path dll_path = GetExeDir() / L"AcpHal.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnApuCreateHeap = int32_t(__stdcall*)(uint32_t, uint32_t);
    using PfnApuAlloc = int32_t(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
    using PfnApuFree = int32_t(__stdcall*)(void*);
    using PfnApuHeapGetState = int32_t(__stdcall*)(TestApuHeapState*, uint32_t);
    using PfnApuMapVirtual = uint32_t(__stdcall*)(const void*);
    using PfnApuMapApu = void*(__stdcall*)(uint32_t);
    using PfnApuIsValid = int32_t(__stdcall*)(const void*, uint32_t);

    auto fn_create_heap = reinterpret_cast<PfnApuCreateHeap>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuCreateHeap")));
    auto fn_alloc = reinterpret_cast<PfnApuAlloc>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuAlloc")));
    auto fn_free = reinterpret_cast<PfnApuFree>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuFree")));
    auto fn_get_state = reinterpret_cast<PfnApuHeapGetState>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuHeapGetState")));
    auto fn_map_va = reinterpret_cast<PfnApuMapVirtual>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuMapVirtualAddress")));
    auto fn_map_apu = reinterpret_cast<PfnApuMapApu>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuMapApuAddress")));
    auto fn_is_valid = reinterpret_cast<PfnApuIsValid>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "ApuIsVirtualAddressValid")));

    // Confirm legacy AcpHal exports are still preserved alongside APU exports
    OS_REQUIRE(::GetProcAddress(mod, "AcpHalCreate") != nullptr);
    OS_REQUIRE(::GetProcAddress(mod, "AcpHalAllocateShapeContexts") != nullptr);
    OS_REQUIRE(::GetProcAddress(mod, "AcpHalReleaseShapeContexts") != nullptr);

    OS_REQUIRE(fn_create_heap && fn_alloc && fn_free && fn_get_state);
    OS_REQUIRE(fn_map_va && fn_map_apu && fn_is_valid);

    // Create 4 MiB cached + 2 MiB non-cached heaps
    constexpr uint32_t kCachedCap = 4u * 1024u * 1024u;
    constexpr uint32_t kNonCachedCap = 2u * 1024u * 1024u;
    OS_REQUIRE(fn_create_heap(kCachedCap, kNonCachedCap) == 0);

    TestApuHeapState st_cached{};
    TestApuHeapState st_noncached{};
    OS_REQUIRE(fn_get_state(&st_cached, 0u) == 0);
    OS_REQUIRE(fn_get_state(&st_noncached, 1u) == 0);
    OS_REQUIRE(st_cached.bytesFree == kCachedCap);
    OS_REQUIRE(st_cached.bytesAllocated == 0u);
    OS_REQUIRE(st_cached.bytesLost == 0u);
    OS_REQUIRE(st_cached.maximumBlockSizeAvailable == kCachedCap);
    OS_REQUIRE(st_cached.allocationCount == 0u);
    OS_REQUIRE(st_noncached.bytesFree == kNonCachedCap);
    OS_REQUIRE(st_noncached.allocationCount == 0u);

    // Validation checks on ApuAlloc
    void* bad_va = nullptr;
    OS_REQUIRE(fn_alloc(nullptr, nullptr, 64u, 64u, 0u) < 0);
    OS_REQUIRE(fn_alloc(&bad_va, nullptr, 0u, 64u, 0u) < 0);
    OS_REQUIRE(fn_alloc(&bad_va, nullptr, 64u, 48u, 0u) < 0);  // non-power-of-2 alignment
    OS_REQUIRE(fn_alloc(&bad_va, nullptr, kCachedCap + 4096u, 64u, 0u) < 0);  // exceeds capacity

    // Allocate 64-byte aligned (cached) & 4096-byte aligned (cached & non-cached)
    void* p64 = nullptr;
    uint32_t apu64 = 0u;
    OS_REQUIRE(fn_alloc(&p64, &apu64, 1024u, 64u, 0u) == 0);
    OS_REQUIRE(p64 != nullptr && apu64 != 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p64) & 63u) == 0u);
    OS_REQUIRE((apu64 & 63u) == 0u);

    void* p4k = nullptr;
    uint32_t apu4k = 0u;
    OS_REQUIRE(fn_alloc(&p4k, &apu4k, 16384u, 4096u, 0u) == 0);
    OS_REQUIRE(p4k != nullptr && apu4k != 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p4k) & 4095u) == 0u);
    OS_REQUIRE((apu4k & 4095u) == 0u);

    void* p_nc = nullptr;
    uint32_t apu_nc = 0u;
    OS_REQUIRE(fn_alloc(&p_nc, &apu_nc, 8192u, 4096u, 1u) == 0);
    OS_REQUIRE(p_nc != nullptr && apu_nc != 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p_nc) & 4095u) == 0u);
    OS_REQUIRE((apu_nc & 4095u) == 0u);

    // Write & read memory cleanly
    std::memset(p64, 0x5Au, 1024u);
    std::memset(p4k, 0xA5u, 16384u);
    std::memset(p_nc, 0x3Cu, 8192u);

    // ApuIsVirtualAddressValid & ApuMapVirtualAddress <-> ApuMapApuAddress round-trip (base + interior!)
    OS_REQUIRE(fn_is_valid(nullptr, 16u) == 0);
    OS_REQUIRE(fn_is_valid(p64, 1024u) == 1);
    OS_REQUIRE(fn_is_valid(static_cast<uint8_t*>(p64) + 512u, 512u) == 1);
    OS_REQUIRE(fn_is_valid(static_cast<uint8_t*>(p64) + 1024u, 1u) == 0);
    OS_REQUIRE(fn_is_valid(p4k, 16384u) == 1);
    OS_REQUIRE(fn_is_valid(p_nc, 8192u) == 1);

    OS_REQUIRE(fn_map_va(p64) == apu64);
    OS_REQUIRE(fn_map_va(static_cast<uint8_t*>(p64) + 333u) == apu64 + 333u);
    OS_REQUIRE(fn_map_apu(apu64) == p64);
    OS_REQUIRE(fn_map_apu(apu64 + 333u) == static_cast<uint8_t*>(p64) + 333u);

    OS_REQUIRE(fn_map_va(p4k) == apu4k);
    OS_REQUIRE(fn_map_apu(apu4k + 4097u) == static_cast<uint8_t*>(p4k) + 4097u);
    OS_REQUIRE(fn_map_va(p_nc) == apu_nc);
    OS_REQUIRE(fn_map_apu(apu_nc + 100u) == static_cast<uint8_t*>(p_nc) + 100u);
    OS_REQUIRE(fn_map_apu(0u) == nullptr);

    // Accurate ApuHeapGetState accounting while allocations are live
    OS_REQUIRE(fn_get_state(&st_cached, 0u) == 0);
    OS_REQUIRE(st_cached.allocationCount == 2u);
    OS_REQUIRE(st_cached.bytesAllocated == 1024u + 16384u);
    OS_REQUIRE(st_cached.bytesAllocated + st_cached.bytesLost + st_cached.bytesFree == kCachedCap);
    OS_REQUIRE(st_cached.maximumBlockSizeAvailable <= st_cached.bytesFree);

    OS_REQUIRE(fn_get_state(&st_noncached, 1u) == 0);
    OS_REQUIRE(st_noncached.allocationCount == 1u);
    OS_REQUIRE(st_noncached.bytesAllocated == 8192u);
    OS_REQUIRE(st_noncached.bytesFree == kNonCachedCap - 8192u);

    // Free all allocations & confirm full coalescing + invalidation
    OS_REQUIRE(fn_free(p64) == 0);
    OS_REQUIRE(fn_is_valid(p64, 1024u) == 0);
    OS_REQUIRE(fn_map_va(p64) == 0u);
    OS_REQUIRE(fn_map_apu(apu64) == nullptr);
    OS_REQUIRE(fn_free(p64) < 0);  // double-free rejected
    OS_REQUIRE(fn_free(nullptr) == 0);

    OS_REQUIRE(fn_free(p4k) == 0);
    OS_REQUIRE(fn_free(p_nc) == 0);

    OS_REQUIRE(fn_get_state(&st_cached, 0u) == 0);
    OS_REQUIRE(st_cached.allocationCount == 0u);
    OS_REQUIRE(st_cached.bytesAllocated == 0u);
    OS_REQUIRE(st_cached.bytesLost == 0u);
    OS_REQUIRE(st_cached.bytesFree == kCachedCap);
    OS_REQUIRE(st_cached.maximumBlockSizeAvailable == kCachedCap);

    OS_REQUIRE(fn_get_state(&st_noncached, 1u) == 0);
    OS_REQUIRE(st_noncached.allocationCount == 0u);
    OS_REQUIRE(st_noncached.bytesAllocated == 0u);
    OS_REQUIRE(st_noncached.bytesFree == kNonCachedCap);
    OS_REQUIRE(st_noncached.maximumBlockSizeAvailable == kNonCachedCap);

    // Concurrent multi-thread stress test (8 threads x 200 alloc/map/verify/free cycles)
    constexpr uint32_t kThreads = 8u;
    constexpr uint32_t kIters = 200u;
    std::atomic<uint32_t> mt_failures{0u};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (uint32_t t = 0u; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            const uint32_t aligns[3] = {64u, 256u, 4096u};
            for (uint32_t i = 0u; i < kIters; ++i) {
                const uint32_t align = aligns[(t + i) % 3u];
                const uint32_t sz = 256u + ((t * 31u + i * 17u) % 4096u);
                const uint32_t flags = (i & 1u);
                void* p = nullptr;
                uint32_t apu = 0u;
                if (fn_alloc(&p, &apu, sz, align, flags) != 0 || !p || apu == 0u) {
                    mt_failures.fetch_add(1u, std::memory_order_relaxed);
                    continue;
                }
                if ((reinterpret_cast<uintptr_t>(p) & (align - 1u)) != 0u ||
                    (apu & (align - 1u)) != 0u ||
                    fn_is_valid(p, sz) != 1 ||
                    fn_map_va(p) != apu ||
                    fn_map_apu(apu) != p) {
                    mt_failures.fetch_add(1u, std::memory_order_relaxed);
                }
                const uint32_t probe_off = sz / 2u;
                if (fn_map_va(static_cast<uint8_t*>(p) + probe_off) != apu + probe_off ||
                    fn_map_apu(apu + probe_off) != static_cast<uint8_t*>(p) + probe_off) {
                    mt_failures.fetch_add(1u, std::memory_order_relaxed);
                }
                std::memset(p, static_cast<int>(t + 1u), sz);
                if (fn_free(p) != 0) {
                    mt_failures.fetch_add(1u, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : workers) {
        th.join();
    }
    OS_REQUIRE(mt_failures.load() == 0u);

    // Zero leaks and 100% coalesced after 1,600 concurrent alloc/free operations!
    OS_REQUIRE(fn_get_state(&st_cached, 0u) == 0);
    OS_REQUIRE(st_cached.allocationCount == 0u && st_cached.bytesAllocated == 0u &&
               st_cached.bytesFree == kCachedCap && st_cached.maximumBlockSizeAvailable == kCachedCap);
    OS_REQUIRE(fn_get_state(&st_noncached, 1u) == 0);
    OS_REQUIRE(st_noncached.allocationCount == 0u && st_noncached.bytesAllocated == 0u &&
               st_noncached.bytesFree == kNonCachedCap &&
               st_noncached.maximumBlockSizeAvailable == kNonCachedCap);

    ::FreeLibrary(mod);
    std::printf("  PASS: AcpHal.dll 10/10 exports + APU heap state + 1,600 MT stress ops verified.\n");
}

// ===========================================================================
// 2. xmem.dll — All 18 Exports, Default Heaps, Stats & Custom Allocation Hooks
// ===========================================================================
struct TestXMemWorkingSetStats {
    uint64_t Size;
    uint64_t TotalPhysicalBytes;
    uint64_t WorkingSetBytes;
    uint64_t PeakWorkingSetBytes;
    uint64_t PrivateUsageBytes;
    uint64_t AvailablePhysicalBytes;
    uint64_t PageFaultCount;
    uint64_t TrackedTitleAllocBytes;
};

using PfnXMemAlloc = void*(__stdcall*)(size_t, uint64_t);
using PfnXMemFree = void(__stdcall*)(void*, uint64_t);
using PfnXMemSize = size_t(__stdcall*)(void*, uint64_t);

PfnXMemAlloc g_xmem_alloc_default = nullptr;
PfnXMemFree g_xmem_free_default = nullptr;
PfnXMemSize g_xmem_size_default = nullptr;

std::atomic<uint32_t> g_hook_alloc_hits{0u};
std::atomic<uint32_t> g_hook_free_hits{0u};
std::atomic<uint32_t> g_hook_size_hits{0u};

void* __stdcall TestCustomAllocHook(size_t dwSize, uint64_t dwAttrs) {
    g_hook_alloc_hits.fetch_add(1u, std::memory_order_relaxed);
    return g_xmem_alloc_default(dwSize, dwAttrs);
}

void __stdcall TestCustomFreeHook(void* pAddr, uint64_t dwAttrs) {
    g_hook_free_hits.fetch_add(1u, std::memory_order_relaxed);
    g_xmem_free_default(pAddr, dwAttrs);
}

size_t __stdcall TestCustomSizeHook(void* pAddr, uint64_t dwAttrs) {
    g_hook_size_hits.fetch_add(1u, std::memory_order_relaxed);
    return g_xmem_size_default(pAddr, dwAttrs) + 77u;  // distinguishable sentinel delta
}

void TestXMemExportsAndHooks() {
    std::printf("[Test 2] xmem.dll: 18 exports, CheckDefaultHeaps, WorkingSetStats, AuxMem, AllocationHooks...\n");

    const fs::path dll_path = GetExeDir() / L"xmem.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    static constexpr const char* kExpectedXMemExports[18] = {
        "XMemAlloc",
        "XMemAllocDefault",
        "XMemAllocAligned",
        "XMemFree",
        "XMemFreeDefault",
        "XMemSize",
        "XMemSizeDefault",
        "XMemSetAllocationHooks",
        "XMemCheckDefaultHeaps",
        "XMemGetAuxiliaryTitleMemory",
        "XMemReleaseAuxiliaryTitleMemory",
        "XMemVirtualQuery",
        "XMemDecodeAllocationAttributes",
        "XMemGetAllocationAttributes",
        "XMemMapPhysicalPages",
        "XMemUnmapPhysicalPages",
        "XMemGetWorkingSetStatistics",
        "XMemGetTelemetryCallCountForTest",
    };
    for (const char* sym : kExpectedXMemExports) {
        OS_REQUIRE(::GetProcAddress(mod, sym) != nullptr);
    }

    using PfnSetHooks = int32_t(__stdcall*)(PfnXMemAlloc, PfnXMemFree, PfnXMemSize);
    using PfnCheckHeaps = int32_t(__stdcall*)(void);
    using PfnGetWs = int32_t(__stdcall*)(void*);
    using PfnGetAux = void*(__stdcall*)(size_t, size_t*);
    using PfnRelAux = int32_t(__stdcall*)(void*);

    auto fn_alloc = reinterpret_cast<PfnXMemAlloc>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemAlloc")));
    auto fn_free = reinterpret_cast<PfnXMemFree>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemFree")));
    auto fn_size = reinterpret_cast<PfnXMemSize>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemSize")));
    g_xmem_alloc_default = reinterpret_cast<PfnXMemAlloc>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemAllocDefault")));
    g_xmem_free_default = reinterpret_cast<PfnXMemFree>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemFreeDefault")));
    g_xmem_size_default = reinterpret_cast<PfnXMemSize>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemSizeDefault")));
    auto fn_set_hooks = reinterpret_cast<PfnSetHooks>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemSetAllocationHooks")));
    auto fn_check_heaps = reinterpret_cast<PfnCheckHeaps>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemCheckDefaultHeaps")));
    auto fn_ws = reinterpret_cast<PfnGetWs>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemGetWorkingSetStatistics")));
    auto fn_get_aux = reinterpret_cast<PfnGetAux>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemGetAuxiliaryTitleMemory")));
    auto fn_rel_aux = reinterpret_cast<PfnRelAux>(reinterpret_cast<void*>(::GetProcAddress(mod, "XMemReleaseAuxiliaryTitleMemory")));

    // 1. XMemCheckDefaultHeaps
    OS_REQUIRE(fn_check_heaps() == 1);

    // 2. XMemGetWorkingSetStatistics
    TestXMemWorkingSetStats ws{};
    ws.Size = sizeof(ws);
    OS_REQUIRE(fn_ws(&ws) != 0);
    OS_REQUIRE(ws.TotalPhysicalBytes > 0ull && ws.WorkingSetBytes > 0ull);

    // 3. Auxiliary title memory round-trip
    size_t aux_actual = 0u;
    void* aux = fn_get_aux(131072u, &aux_actual);
    OS_REQUIRE(aux != nullptr && aux_actual >= 131072u);
    std::memset(aux, 0x7Eu, aux_actual);
    OS_REQUIRE(fn_rel_aux(aux) != 0);

    // 4. Default path before hooks
    void* p0 = g_xmem_alloc_default(1024u, 0ull);
    OS_REQUIRE(p0 != nullptr);
    OS_REQUIRE(g_xmem_size_default(p0, 0ull) == 1024u);
    g_xmem_free_default(p0, 0ull);

    // 5. Register custom allocation hooks -> XMemAlloc/Size/Free invoke hooks, *Default bypasses them!
    g_hook_alloc_hits.store(0u);
    g_hook_free_hits.store(0u);
    g_hook_size_hits.store(0u);

    OS_REQUIRE(fn_set_hooks(&TestCustomAllocHook, &TestCustomFreeHook, &TestCustomSizeHook) == 0);

    void* p_hooked = fn_alloc(512u, 0ull);
    OS_REQUIRE(p_hooked != nullptr);
    OS_REQUIRE(g_hook_alloc_hits.load() == 1u);
    OS_REQUIRE(fn_size(p_hooked, 0ull) == 512u + 77u);
    OS_REQUIRE(g_hook_size_hits.load() == 1u);
    fn_free(p_hooked, 0ull);
    OS_REQUIRE(g_hook_free_hits.load() == 1u);

    // *Default MUST bypass custom hooks completely
    void* p_bypass = g_xmem_alloc_default(256u, 0ull);
    OS_REQUIRE(p_bypass != nullptr);
    OS_REQUIRE(g_xmem_size_default(p_bypass, 0ull) == 256u);
    g_xmem_free_default(p_bypass, 0ull);
    OS_REQUIRE(g_hook_alloc_hits.load() == 1u);
    OS_REQUIRE(g_hook_size_hits.load() == 1u);
    OS_REQUIRE(g_hook_free_hits.load() == 1u);

    // Reset hooks to nullptr -> restores default allocator cleanly!
    OS_REQUIRE(fn_set_hooks(nullptr, nullptr, nullptr) == 0);
    void* p_restored = fn_alloc(640u, 0ull);
    OS_REQUIRE(p_restored != nullptr);
    OS_REQUIRE(fn_size(p_restored, 0ull) == 640u);
    fn_free(p_restored, 0ull);
    OS_REQUIRE(g_hook_alloc_hits.load() == 1u);
    OS_REQUIRE(g_hook_size_hits.load() == 1u);
    OS_REQUIRE(g_hook_free_hits.load() == 1u);

    OS_REQUIRE(fn_check_heaps() == 1);

    ::FreeLibrary(mod);
    std::printf("  PASS: xmem.dll 18/18 exports + hooks vs *Default bypass + heap check verified.\n");
}

// ===========================================================================
// 3. mfplat_xs.dll — All 51 Exports & Live Media Foundation Round-Trip
// ===========================================================================
void TestMfPlatForwardingAndRoundTrip() {
    std::printf("[Test 3] mfplat_xs.dll: 51 exports + live System32 MFPlat.dll round-trip...\n");

    const fs::path dll_path = GetExeDir() / L"mfplat_xs.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    static constexpr const char* kExpectedMfExports[] = {
        // Core creation & platform
        "MFCreateAlignedMemoryBuffer",
        "MFCreateMemoryBuffer",
        "MFCreateSample",
        "MFCreateMediaType",
        "MFCreateAttributes",
        "MFCreateAsyncResult",
        "MFCreateSourceResolver",
        "MFStartup",
        "MFShutdown",
        "MFLockPlatform",
        "MFUnlockPlatform",
        "MFGetSystemTime",
        // Work queues & MMCSS
        "MFLockWorkQueue",
        "MFUnlockWorkQueue",
        "MFAllocateWorkQueue",
        "MFAllocateWorkQueueEx",
        "MFAllocateSerialWorkQueue",
        "MFPutWorkItem",
        "MFPutWorkItem2",
        "MFPutWorkItemEx",
        "MFPutWorkItemEx2",
        "MFPutWaitingWorkItem",
        "MFCancelWorkItem",
        "MFLockSharedWorkQueue",
        "MFGetWorkQueueMMCSSClass",
        "MFGetWorkQueueMMCSSTaskId",
        "MFGetWorkQueueMMCSSPriority",
        // Media types, descriptors, buffers, imaging, events, streams
        "MFCreateWaveFormatExFromMFMediaType",
        "MFInitMediaTypeFromWaveFormatEx",
        "MFCreateEventQueue",
        "MFCreateCollection",
        "MFCreateDXGISurfaceBuffer",
        "MFCreate2DMediaBuffer",
        "MFCreateVideoMediaType",
        "MFCreateAudioMediaType",
        "MFCreatePresentationDescriptor",
        "MFCreateStreamDescriptor",
        "MFCreateMediaTypeHandler",
        "MFGetStrideForBitmapInfoHeader",
        "MFCalculateImageSize",
        "MFCopyImage",
        "MFCreateMediaEvent",
        "MFCreateMediaBufferWrapper",
        "MFCreateTempFile",
        "MFCreateFile",
        "MFCreateMFByteStreamOnStream",
        // Xbox-specific & existing OpenScarlett exports
        "MFCreateDXGIDeviceManager",
        "MFResetDXGIDeviceManagerX",
        "MFCreateMediaEngineClassFactory",
        "MFTEnumEx",
        "OpenScarlettMfPlatGetTelemetryCount",
    };

    for (const char* sym : kExpectedMfExports) {
        OS_REQUIRE(::GetProcAddress(mod, sym) != nullptr);
    }

    using PfnMFStartup = int32_t(__stdcall*)(uint32_t, uint32_t);
    using PfnMFShutdown = int32_t(__stdcall*)(void);
    using PfnMFLockPlat = int32_t(__stdcall*)(void);
    using PfnMFGetTime = int64_t(__stdcall*)(void);
    using PfnMFCreateAlignedBuf = int32_t(__stdcall*)(uint32_t, uint32_t, void**);
    using PfnMFCreateMemBuf = int32_t(__stdcall*)(uint32_t, void**);
    using PfnMFCreateBufWrapper = int32_t(__stdcall*)(void*, uint32_t, uint32_t, void**);
    using PfnMFCreateSample = int32_t(__stdcall*)(void**);
    using PfnMFCreateMT = int32_t(__stdcall*)(void**);
    using PfnMFCreateAttrs = int32_t(__stdcall*)(void**, uint32_t);
    using PfnMFInitMtWfx = int32_t(__stdcall*)(void*, const void*, uint32_t);
    using PfnMFCreateWfxMt = int32_t(__stdcall*)(void*, void**, uint32_t*, uint32_t);
    using PfnMFAllocWq = int32_t(__stdcall*)(uint32_t*);
    using PfnMFAllocWqEx = int32_t(__stdcall*)(uint32_t, uint32_t*);
    using PfnMFAllocSerialWq = int32_t(__stdcall*)(uint32_t, uint32_t*);
    using PfnMFLockWq = int32_t(__stdcall*)(uint32_t);
    using PfnMFUnlockWq = int32_t(__stdcall*)(uint32_t);
    using PfnMFCreateObj = int32_t(__stdcall*)(void**);
    using PfnMFGetStride = int32_t(__stdcall*)(uint32_t, uint32_t, int32_t*);
    using PfnMFCopyImg = int32_t(__stdcall*)(uint8_t*, int32_t, const uint8_t*, int32_t, uint32_t, uint32_t);
    using PfnMFCreateStreamDesc = int32_t(__stdcall*)(uint32_t, uint32_t, void**, void**);
    using PfnMFCreatePresDesc = int32_t(__stdcall*)(uint32_t, void**, void**);
    using PfnTelCount = int32_t(__stdcall*)(const char*, uint64_t*, uint64_t*);

    auto fn_startup = reinterpret_cast<PfnMFStartup>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFStartup")));
    auto fn_shutdown = reinterpret_cast<PfnMFShutdown>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFShutdown")));
    auto fn_lock_plat = reinterpret_cast<PfnMFLockPlat>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFLockPlatform")));
    auto fn_unlock_plat = reinterpret_cast<PfnMFLockPlat>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFUnlockPlatform")));
    auto fn_get_time = reinterpret_cast<PfnMFGetTime>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFGetSystemTime")));
    auto fn_aligned_buf = reinterpret_cast<PfnMFCreateAlignedBuf>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateAlignedMemoryBuffer")));
    auto fn_mem_buf = reinterpret_cast<PfnMFCreateMemBuf>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateMemoryBuffer")));
    auto fn_buf_wrap = reinterpret_cast<PfnMFCreateBufWrapper>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateMediaBufferWrapper")));
    auto fn_sample = reinterpret_cast<PfnMFCreateSample>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateSample")));
    auto fn_mt = reinterpret_cast<PfnMFCreateMT>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateMediaType")));
    auto fn_attrs = reinterpret_cast<PfnMFCreateAttrs>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateAttributes")));
    auto fn_init_wfx = reinterpret_cast<PfnMFInitMtWfx>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFInitMediaTypeFromWaveFormatEx")));
    auto fn_get_wfx = reinterpret_cast<PfnMFCreateWfxMt>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateWaveFormatExFromMFMediaType")));
    auto fn_alloc_wq = reinterpret_cast<PfnMFAllocWq>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFAllocateWorkQueue")));
    auto fn_alloc_wq_ex = reinterpret_cast<PfnMFAllocWqEx>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFAllocateWorkQueueEx")));
    auto fn_alloc_serial_wq = reinterpret_cast<PfnMFAllocSerialWq>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFAllocateSerialWorkQueue")));
    auto fn_lock_wq = reinterpret_cast<PfnMFLockWq>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFLockWorkQueue")));
    auto fn_unlock_wq = reinterpret_cast<PfnMFUnlockWq>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFUnlockWorkQueue")));
    auto fn_evt_q = reinterpret_cast<PfnMFCreateObj>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateEventQueue")));
    auto fn_coll = reinterpret_cast<PfnMFCreateObj>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateCollection")));
    auto fn_src_res = reinterpret_cast<PfnMFCreateObj>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateSourceResolver")));
    auto fn_mt_handler = reinterpret_cast<PfnMFCreateObj>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateMediaTypeHandler")));
    auto fn_stride = reinterpret_cast<PfnMFGetStride>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFGetStrideForBitmapInfoHeader")));
    auto fn_copy_img = reinterpret_cast<PfnMFCopyImg>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCopyImage")));
    auto fn_stream_desc = reinterpret_cast<PfnMFCreateStreamDesc>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreateStreamDescriptor")));
    auto fn_pres_desc = reinterpret_cast<PfnMFCreatePresDesc>(reinterpret_cast<void*>(::GetProcAddress(mod, "MFCreatePresentationDescriptor")));
    auto fn_tel = reinterpret_cast<PfnTelCount>(reinterpret_cast<void*>(::GetProcAddress(mod, "OpenScarlettMfPlatGetTelemetryCount")));

    constexpr uint32_t kMfVersion = (0x0002u << 16u) | 0x0070u;
    OS_REQUIRE(fn_startup(kMfVersion, 0x1u) == 0);
    OS_REQUIRE(fn_lock_plat() == 0);
    OS_REQUIRE(fn_unlock_plat() == 0);
    OS_REQUIRE(fn_get_time() >= 0);

    // 1. Aligned memory buffer (64-byte alignment -> MF_64_BYTE_ALIGNMENT = 63 mask on MF) + wrapper + sample
    IMFMediaBuffer* aligned_buf = nullptr;
    OS_REQUIRE(fn_aligned_buf(4096u, 63u, reinterpret_cast<void**>(&aligned_buf)) == 0 && aligned_buf != nullptr);
    {
        BYTE* ptr = nullptr;
        DWORD max_len = 0u, cur_len = 0u;
        OS_REQUIRE(aligned_buf->Lock(&ptr, &max_len, &cur_len) == S_OK && ptr != nullptr && max_len >= 4096u);
        OS_REQUIRE((reinterpret_cast<uintptr_t>(ptr) & 63u) == 0u);
        std::memset(ptr, 0x42, 256u);
        OS_REQUIRE(aligned_buf->Unlock() == S_OK);
        OS_REQUIRE(aligned_buf->SetCurrentLength(256u) == S_OK);
    }

    IMFMediaBuffer* wrapped_buf = nullptr;
    OS_REQUIRE(fn_buf_wrap(aligned_buf, 64u, 128u, reinterpret_cast<void**>(&wrapped_buf)) == 0 && wrapped_buf != nullptr);
    wrapped_buf->Release();

    IMFMediaBuffer* mem_buf = nullptr;
    OS_REQUIRE(fn_mem_buf(1024u, reinterpret_cast<void**>(&mem_buf)) == 0 && mem_buf != nullptr);
    OS_REQUIRE(mem_buf->SetCurrentLength(128u) == S_OK);

    IMFSample* sample = nullptr;
    OS_REQUIRE(fn_sample(reinterpret_cast<void**>(&sample)) == 0 && sample != nullptr);
    OS_REQUIRE(sample->AddBuffer(aligned_buf) == S_OK);
    OS_REQUIRE(sample->AddBuffer(mem_buf) == S_OK);
    DWORD total_len = 0u;
    OS_REQUIRE(sample->GetTotalLength(&total_len) == S_OK && total_len == 384u);
    sample->Release();
    mem_buf->Release();
    aligned_buf->Release();

    // 2. MFCreateMediaType + MFInitMediaTypeFromWaveFormatEx + MFCreateWaveFormatExFromMFMediaType round-trip!
    IMFMediaType* mt = nullptr;
    OS_REQUIRE(fn_mt(reinterpret_cast<void**>(&mt)) == 0 && mt != nullptr);
    WAVEFORMATEX wfx_in{};
    wfx_in.wFormatTag = WAVE_FORMAT_PCM;
    wfx_in.nChannels = 2u;
    wfx_in.nSamplesPerSec = 48000u;
    wfx_in.wBitsPerSample = 16u;
    wfx_in.nBlockAlign = 4u;
    wfx_in.nAvgBytesPerSec = 48000u * 4u;
    wfx_in.cbSize = 0u;
    OS_REQUIRE(fn_init_wfx(mt, &wfx_in, sizeof(wfx_in)) == 0);

    WAVEFORMATEX* wfx_out = nullptr;
    uint32_t wfx_out_sz = 0u;
    OS_REQUIRE(fn_get_wfx(mt, reinterpret_cast<void**>(&wfx_out), &wfx_out_sz, 0u) == 0);
    OS_REQUIRE(wfx_out != nullptr && wfx_out_sz >= sizeof(WAVEFORMATEX));
    OS_REQUIRE(wfx_out->wFormatTag == WAVE_FORMAT_PCM);
    OS_REQUIRE(wfx_out->nChannels == 2u && wfx_out->nSamplesPerSec == 48000u && wfx_out->wBitsPerSample == 16u);
    ::CoTaskMemFree(wfx_out);

    // 3. StreamDescriptor + PresentationDescriptor + MediaTypeHandler + EventQueue + Collection + SourceResolver + Attributes
    void* mt_arr[1] = {mt};
    void* sdesc = nullptr;
    OS_REQUIRE(fn_stream_desc(0u, 1u, mt_arr, &sdesc) == 0 && sdesc != nullptr);
    void* sdesc_arr[1] = {sdesc};
    void* pdesc = nullptr;
    OS_REQUIRE(fn_pres_desc(1u, sdesc_arr, &pdesc) == 0 && pdesc != nullptr);
    static_cast<IUnknown*>(pdesc)->Release();
    static_cast<IUnknown*>(sdesc)->Release();
    mt->Release();

    void* attrs = nullptr;
    OS_REQUIRE(fn_attrs(&attrs, 8u) == 0 && attrs != nullptr);
    static_cast<IUnknown*>(attrs)->Release();

    void* evt_q = nullptr;
    OS_REQUIRE(fn_evt_q(&evt_q) == 0 && evt_q != nullptr);
    static_cast<IUnknown*>(evt_q)->Release();

    void* coll = nullptr;
    OS_REQUIRE(fn_coll(&coll) == 0 && coll != nullptr);
    static_cast<IUnknown*>(coll)->Release();

    void* src_res = nullptr;
    OS_REQUIRE(fn_src_res(&src_res) == 0 && src_res != nullptr);
    static_cast<IUnknown*>(src_res)->Release();

    void* mth = nullptr;
    OS_REQUIRE(fn_mt_handler(&mth) == 0 && mth != nullptr);
    static_cast<IUnknown*>(mth)->Release();

    // 4. Image helpers: MFGetStrideForBitmapInfoHeader + MFCopyImage
    int32_t nv12_stride = 0;
    OS_REQUIRE(fn_stride(0x3231564Eu, 1920u, &nv12_stride) == 0 && nv12_stride == 1920);
    uint8_t src_img[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t dst_img[16]{};
    OS_REQUIRE(fn_copy_img(dst_img, 4, src_img, 4, 4u, 4u) == 0);
    OS_REQUIRE(std::memcmp(src_img, dst_img, sizeof(src_img)) == 0);

    // 5. Work queues: MFAllocateWorkQueue, MFLockWorkQueue, MFUnlockWorkQueue, MFAllocateSerialWorkQueue, MFAllocateWorkQueueEx
    uint32_t wq = 0u;
    OS_REQUIRE(fn_alloc_wq(&wq) == 0 && wq != 0u);
    OS_REQUIRE(fn_lock_wq(wq) == 0);
    OS_REQUIRE(fn_unlock_wq(wq) == 0);
    uint32_t serial_wq = 0u;
    OS_REQUIRE(fn_alloc_serial_wq(wq, &serial_wq) == 0 && serial_wq != 0u);
    OS_REQUIRE(fn_unlock_wq(serial_wq) == 0);
    OS_REQUIRE(fn_unlock_wq(wq) == 0);

    uint32_t wq_ex = 0u;
    OS_REQUIRE(fn_alloc_wq_ex(0u, &wq_ex) == 0 && wq_ex != 0u);
    OS_REQUIRE(fn_unlock_wq(wq_ex) == 0);

    OS_REQUIRE(fn_shutdown() == 0);

    // 6. Verify telemetry recorded forwarded calls
    uint64_t tot = 0u, ok = 0u;
    OS_REQUIRE(fn_tel("MFCreateAlignedMemoryBuffer", &tot, &ok) == 1 && ok >= 1u);
    OS_REQUIRE(fn_tel("MFInitMediaTypeFromWaveFormatEx", &tot, &ok) == 1 && ok >= 1u);
    OS_REQUIRE(fn_tel("MFCreateWaveFormatExFromMFMediaType", &tot, &ok) == 1 && ok >= 1u);
    OS_REQUIRE(fn_tel("MFLockWorkQueue", &tot, &ok) == 1 && ok >= 1u);
    OS_REQUIRE(fn_tel("MFCreateMediaTypeHandler", &tot, &ok) == 1 && ok >= 1u);

    ::FreeLibrary(mod);
    std::printf("  PASS: mfplat_xs.dll 51/51 exports + live System32 MFPlat round-trip verified.\n");
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestAcpHalApuHeap();
    TestXMemExportsAndHooks();
    TestMfPlatForwardingAndRoundTrip();
    if (SUCCEEDED(com_hr)) {
        ::CoUninitialize();
    }
#endif
    std::printf("[PASS] test_ue5_acphal_xmem_mfplat: all 3 suites passed!\n");
    return 0;
}
