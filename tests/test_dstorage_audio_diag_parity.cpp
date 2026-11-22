#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <d3d12.h>
#  include <dxgi1_4.h>
#  include <windows.h>
#endif

#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t k64KiB = 65536u;
constexpr float kPi = 3.14159265358979323846f;

fs::path GetExeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0u && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#endif
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path GetScratchDir() {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec || base.empty()) {
        base = GetExeDir();
    }
    const fs::path dir = base / "openscarlett_dstorage_audio_diag_parity";
    fs::create_directories(dir, ec);
    return dir;
}

void SetNullAudioEnv(bool enabled) {
#if defined(_WIN32)
    SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", enabled ? "1" : nullptr);
#else
    (void)enabled;
#endif
}

std::vector<uint8_t> MakeDeterministicBytes(size_t n, uint32_t seed) {
    std::vector<uint8_t> out(n);
    uint32_t s = seed ^ 0x9E3779B9u;
    for (size_t i = 0u; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        out[i] = static_cast<uint8_t>((s >> 24u) ^ ((i * 17u + seed) & 0xFFu));
    }
    return out;
}

std::vector<uint8_t> MakeBc7Blocks(uint32_t w, uint32_t h, uint32_t seed) {
    const size_t blocks = static_cast<size_t>((w / 4u) * (h / 4u));
    std::vector<uint8_t> out(blocks * 16u);
    for (size_t b = 0u; b < blocks; ++b) {
        out[b * 16u + 0u] = 0x40u;
        for (size_t k = 1u; k < 16u; ++k) {
            out[b * 16u + k] = static_cast<uint8_t>((b * 11u + k * 23u + seed) & 0xFFu);
        }
    }
    return out;
}

// ===========================================================================
// 1. DirectStorage Submitted-Request Cancellation, intermediate_size Bounds,
//    & Direct Decompression into kernelx/xmem Physical-Page Destinations
// ===========================================================================
void TestDirectStorageSubmittedCancelBoundsAndPhysicalPages() {
    std::printf("[Test 1] dstorage_xs + xcrdapi: submitted cancel, intermediate_size bounds, physical-page decompression...\n");
#if defined(_WIN32)
    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    // 1A. SetStagingBufferSize bounds validation
    OS_REQUIRE(factory->SetStagingBufferSize(0u) == OS_DSTORAGE_E_STAGING_BUFFER_INVALID);
    OS_REQUIRE(factory->SetStagingBufferSize(1024u) == OS_DSTORAGE_E_STAGING_BUFFER_INVALID);
    OS_REQUIRE(factory->SetStagingBufferSize(DSTORAGE_MAX_STAGING_BUFFER_SIZE + 4096u) ==
               OS_DSTORAGE_E_STAGING_BUFFER_INVALID);

    uint32_t cur_staging = 0u;
    OS_REQUIRE(OpenScarlettDStorageGetStagingBufferSize(factory, &cur_staging) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(cur_staging == DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE);

    // Prepare GDeflate (96 KiB) & BCPack (64x64 BC7 = 4 KiB) payloads
    const std::vector<uint8_t> gdef_plain = MakeDeterministicBytes(96u * 1024u, 0x101u);
    std::vector<uint8_t> gdef_comp;
    OS_REQUIRE(openscarlett::GDeflateCompress(gdef_plain.data(), gdef_plain.size(), gdef_comp));

    const std::vector<uint8_t> bc7_plain = MakeBc7Blocks(64u, 64u, 0x202u);
    std::vector<uint8_t> bc7_comp;
    OS_REQUIRE(openscarlett::BcPackCompress(
                   openscarlett::BcFormat::BC7, 64u, 64u,
                   bc7_plain.data(), bc7_plain.size(), bc7_comp)
                   .ok());

    // 1B. Direct OpenScarlettDStorageDecompressWithIntermediate bounds validation
    {
        std::vector<uint8_t> dst(gdef_plain.size(), 0xAAu);
        size_t out_sz = 0u;

        // Invalid intermediate_size (0 and > max)
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                       gdef_comp.data(), gdef_comp.size(),
                       dst.data(), dst.size(), 0u, &out_sz) == OS_DSTORAGE_E_STAGING_BUFFER_INVALID);
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                       gdef_comp.data(), gdef_comp.size(),
                       dst.data(), dst.size(),
                       static_cast<size_t>(DSTORAGE_MAX_STAGING_BUFFER_SIZE) + 1u, &out_sz) ==
                   OS_DSTORAGE_E_STAGING_BUFFER_INVALID);

        // Under-sized intermediate_size
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                       gdef_comp.data(), gdef_comp.size(),
                       dst.data(), dst.size(), gdef_plain.size() - 1u, &out_sz) ==
                   OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);

        // Under-sized destination buffer
        std::vector<uint8_t> tiny_dst(1024u, 0xBBu);
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                       gdef_comp.data(), gdef_comp.size(),
                       tiny_dst.data(), tiny_dst.size(), gdef_plain.size(), &out_sz) ==
                   OS_DSTORAGE_E_REQUEST_TOO_LARGE);

        // Valid intermediate_size -> exact decode
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                       gdef_comp.data(), gdef_comp.size(),
                       dst.data(), dst.size(), gdef_plain.size(), &out_sz) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(out_sz == gdef_plain.size() && dst == gdef_plain);

        // BCPack bounds check + exact decode
        std::vector<uint8_t> bc_dst(bc7_plain.size(), 0u);
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_BCPACK,
                       bc7_comp.data(), bc7_comp.size(),
                       bc_dst.data(), bc_dst.size(), bc7_plain.size() - 1u, &out_sz) ==
                   OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
        OS_REQUIRE(OpenScarlettDStorageDecompressWithIntermediate(
                       DSTORAGE_COMPRESSION_FORMAT_BCPACK,
                       bc7_comp.data(), bc7_comp.size(),
                       bc_dst.data(), bc_dst.size(), bc7_plain.size(), &out_sz) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(out_sz == bc7_plain.size() && bc_dst == bc7_plain);
    }

    // 1C. Queue-level staging buffer limit rejection (set staging to 64 KiB, submit 96 KiB GDeflate)
    {
        OS_REQUIRE(factory->SetStagingBufferSize(DSTORAGE_MIN_STAGING_BUFFER_SIZE) == OS_DSTORAGE_S_OK);

        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qdesc.Name = "bounds_q";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(2u, "bounds_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);

        std::vector<uint8_t> dst96(gdef_plain.size(), 0xCCu);
        OpenScarlettDStorageRequestDesc r_big{};
        r_big.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r_big.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r_big.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_big.Source.Memory.Source = gdef_comp.data();
        r_big.Source.Memory.Size = static_cast<uint32_t>(gdef_comp.size());
        r_big.Destination.Memory.Buffer = dst96.data();
        r_big.Destination.Memory.Size = static_cast<uint32_t>(dst96.size());
        r_big.UncompressedSize = static_cast<uint32_t>(gdef_plain.size());
        OpenScarlettDStorageEnqueueRequestDesc(q, &r_big);
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 1u &&
                   err.FirstFailure.HResult == OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);

        // Restore 32 MiB staging limit and retry -> succeeds!
        OS_REQUIRE(factory->SetStagingBufferSize(DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE) == OS_DSTORAGE_S_OK);
        ResetEvent(ev);
        OpenScarlettDStorageEnqueueRequestDesc(q, &r_big);
        q->EnqueueStatus(st, 1u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(1u) && st->GetHResult(1u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(dst96 == gdef_plain);

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
    }

    // 1D. Submitted & in-flight request cancellation AFTER Submit()
    {
        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qdesc.Name = "submitted_cancel_q";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(1u, "sub_cancel_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);

        // Enqueue 12 multi-tile GDeflate lead requests (tag 0x1000xxxx) to keep worker busy,
        // followed by 24 tagged requests (tag 0xDEAD00xx) that we cancel IMMEDIATELY AFTER Submit().
        constexpr uint32_t kLeadCount = 12u;
        constexpr uint32_t kCancelCount = 24u;
        std::vector<std::vector<uint8_t>> lead_dsts(kLeadCount, std::vector<uint8_t>(gdef_plain.size(), 0u));
        std::vector<std::vector<uint8_t>> cancel_dsts(kCancelCount, std::vector<uint8_t>(gdef_plain.size(), 0xDDu));

        for (uint32_t i = 0u; i < kLeadCount; ++i) {
            OpenScarlettDStorageRequestDesc r{};
            r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.Memory.Source = gdef_comp.data();
            r.Source.Memory.Size = static_cast<uint32_t>(gdef_comp.size());
            r.Destination.Memory.Buffer = lead_dsts[i].data();
            r.Destination.Memory.Size = static_cast<uint32_t>(lead_dsts[i].size());
            r.UncompressedSize = static_cast<uint32_t>(gdef_plain.size());
            r.CancellationTag = 0x10000000ull | i;
            OpenScarlettDStorageEnqueueRequestDesc(q, &r);
        }
        for (uint32_t i = 0u; i < kCancelCount; ++i) {
            OpenScarlettDStorageRequestDesc r{};
            r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.Memory.Source = gdef_comp.data();
            r.Source.Memory.Size = static_cast<uint32_t>(gdef_comp.size());
            r.Destination.Memory.Buffer = cancel_dsts[i].data();
            r.Destination.Memory.Size = static_cast<uint32_t>(cancel_dsts[i].size());
            r.UncompressedSize = static_cast<uint32_t>(gdef_plain.size());
            r.CancellationTag = 0xDEAD0000ull | i;
            OpenScarlettDStorageEnqueueRequestDesc(q, &r);
        }
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);

        // Submit FIRST, then cancel submitted/in-flight 0xDEADxxxx requests!
        q->Submit();
        q->CancelRequestsWithTag(0xFFFF0000ull, 0xDEAD0000ull);

        OS_REQUIRE(WaitForSingleObject(ev, 10000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);

        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 0u);
        OS_REQUIRE(WaitForSingleObject(static_cast<HANDLE>(q->GetErrorEvent()), 0u) == WAIT_TIMEOUT);

        // Every lead request completed bit-exact.
        for (uint32_t i = 0u; i < kLeadCount; ++i) {
            OS_REQUIRE(lead_dsts[i] == gdef_plain);
        }

        // Verify submitted/in-flight cancellation actually canceled queued submitted items
        // and left cancelled destination buffers untouched.
        DSTORAGE_QUEUE_STATS qstats{};
        OS_REQUIRE(OpenScarlettDStorageGetQueueStats(q, &qstats) == OS_DSTORAGE_S_OK);
        OS_REQUIRE((qstats.SubmittedCanceled + qstats.InFlightCanceled) > 0ull);
        OS_REQUIRE(qstats.CompletedCanceled > 0ull);
        OS_REQUIRE(qstats.CompletedFailed == 0ull);

        uint32_t untouched_count = 0u;
        for (uint32_t i = 0u; i < kCancelCount; ++i) {
            if (cancel_dsts[i][0] == 0xDDu && cancel_dsts[i].back() == 0xDDu) {
                ++untouched_count;
            }
        }
        OS_REQUIRE(untouched_count >= qstats.CompletedCanceled);

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        std::printf("  Submitted cancel : submitted=%llu inflight=%llu completed_canceled=%llu untouched=%u\n",
                    static_cast<unsigned long long>(qstats.SubmittedCanceled),
                    static_cast<unsigned long long>(qstats.InFlightCanceled),
                    static_cast<unsigned long long>(qstats.CompletedCanceled),
                    untouched_count);
    }

    // 1E. Direct decompression into kernelx/xmem physical-page destinations (with live VA aliasing!)
    {
        uint64_t page_count = 2u;  // 2 x 64 KiB = 128 KiB
        uint64_t pfns[2] = {};
        OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_COMMIT, &page_count, pfns) == 1);
        OS_REQUIRE(page_count == 2u);

        void* va_write = MapTitleVirtualMemory(nullptr, 2u, MEM_GRAPHICS, PAGE_READWRITE, pfns);
        void* va_alias = XMemMapPhysicalPages(nullptr, 2u, MEM_GRAPHICS, PAGE_READWRITE, pfns);
        OS_REQUIRE(va_write != nullptr && va_alias != nullptr && va_write != va_alias);
        std::memset(va_write, 0x11, 2u * k64KiB);

        // Also allocate an XMem physical-type buffer
        const uint64_t xmem_phys_attr = MAKE_XALLOC_ATTRIBUTES(
            0, 1, 0, 0, 5, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_PHYSICAL, XALLOC_PAGESIZE_64KB, 1);
        void* xmem_dst = XMemAlloc(bc7_plain.size(), xmem_phys_attr);
        OS_REQUIRE(xmem_dst != nullptr);

        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qdesc.Name = "phys_page_q";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(3u, "phys_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);

        // Request 0: GDeflate directly into 128 KiB physical-page mapping
        OpenScarlettDStorageRequestDesc r_phys{};
        r_phys.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r_phys.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r_phys.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_phys.Source.Memory.Source = gdef_comp.data();
        r_phys.Source.Memory.Size = static_cast<uint32_t>(gdef_comp.size());
        r_phys.Destination.Memory.Buffer = va_write;
        r_phys.Destination.Memory.Size = static_cast<uint32_t>(2u * k64KiB);
        r_phys.UncompressedSize = static_cast<uint32_t>(gdef_plain.size());
        OpenScarlettDStorageEnqueueRequestDesc(q, &r_phys);
        q->EnqueueStatus(st, 0u);

        // Request 1: BCPack directly into XMemAlloc(XALLOC_MEMTYPE_PHYSICAL) destination
        OpenScarlettDStorageRequestDesc r_xmem{};
        r_xmem.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_BCPACK;
        r_xmem.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r_xmem.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_xmem.Source.Memory.Source = bc7_comp.data();
        r_xmem.Source.Memory.Size = static_cast<uint32_t>(bc7_comp.size());
        r_xmem.Destination.Memory.Buffer = xmem_dst;
        r_xmem.Destination.Memory.Size = static_cast<uint32_t>(bc7_plain.size());
        r_xmem.UncompressedSize = static_cast<uint32_t>(bc7_plain.size());
        OpenScarlettDStorageEnqueueRequestDesc(q, &r_xmem);
        q->EnqueueStatus(st, 1u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(st->IsComplete(1u) && st->GetHResult(1u) == OS_DSTORAGE_S_OK);

        // Both va_write AND its independent physical alias va_alias MUST hold exact decompressed bytes!
        OS_REQUIRE(std::memcmp(va_write, gdef_plain.data(), gdef_plain.size()) == 0);
        OS_REQUIRE(std::memcmp(va_alias, gdef_plain.data(), gdef_plain.size()) == 0);
        OS_REQUIRE(std::memcmp(xmem_dst, bc7_plain.data(), bc7_plain.size()) == 0);

        DSTORAGE_QUEUE_STATS pstats{};
        OS_REQUIRE(OpenScarlettDStorageGetQueueStats(q, &pstats) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(pstats.PhysicalPageWrites >= 1ull);
        OS_REQUIRE(pstats.XMemDestinationWrites >= 1ull);

        // Request 2: Read-only physical-page destination MUST be rejected cleanly without crashing!
        uint32_t old_prot = 0u;
        OS_REQUIRE(KernelXVirtualProtectFromApp(va_write, 2u * k64KiB, PAGE_READONLY, &old_prot) == 1);
        ResetEvent(ev);
        OpenScarlettDStorageEnqueueRequestDesc(q, &r_phys);
        q->EnqueueStatus(st, 2u);
        q->EnqueueSetEvent(ev);
        q->Submit();
        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(2u) && st->GetHResult(2u) == OS_DSTORAGE_E_INVALIDARG);
        OS_REQUIRE(KernelXVirtualProtectFromApp(va_write, 2u * k64KiB, PAGE_READWRITE, &old_prot) == 1);

        // Also validate xcrdapi.dll intermediate_size + direct decompression into physical pages!
        HMODULE crd_mod = GetModuleHandleW(L"xcrdapi.dll");
        OS_REQUIRE(crd_mod != nullptr);
        using PfnXCrdValidate = int32_t(OS_SHIM_CALL*)(size_t, size_t, size_t);
        using PfnXCrdDecomp = int32_t(OS_SHIM_CALL*)(
            uint32_t, const void*, size_t, void*, size_t, size_t, size_t*);
        using PfnXCrdMiniDump = int32_t(OS_SHIM_CALL*)(uint32_t, const char*);
        auto fn_crd_val = reinterpret_cast<PfnXCrdValidate>(
            reinterpret_cast<void*>(GetProcAddress(crd_mod, "XCrdValidateIntermediateBuffer")));
        auto fn_crd_dec = reinterpret_cast<PfnXCrdDecomp>(
            reinterpret_cast<void*>(GetProcAddress(crd_mod, "XCrdDecompressToBuffer")));
        auto fn_crd_md = reinterpret_cast<PfnXCrdMiniDump>(
            reinterpret_cast<void*>(GetProcAddress(crd_mod, "XCrdStartMiniDumpCapture")));
        OS_REQUIRE(fn_crd_val && fn_crd_dec && fn_crd_md);

        OS_REQUIRE(fn_crd_val(4096u, 0u, 0u) == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_crd_val(4096u, 2048u, 0u) == OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        OS_REQUIRE(fn_crd_val(4096u, 8192u, 4096u) == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_crd_val(4096u, 4096u, 65536u) == OS_SHIM_S_OK);

        std::memset(va_write, 0, 2u * k64KiB);
        size_t crd_written = 0u;
        OS_REQUIRE(fn_crd_dec(1u, gdef_comp.data(), gdef_comp.size(),
                              va_write, 2u * k64KiB, gdef_plain.size() - 1u, &crd_written) ==
                   OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        OS_REQUIRE(fn_crd_dec(1u, gdef_comp.data(), gdef_comp.size(),
                              va_write, 2u * k64KiB, gdef_plain.size(), &crd_written) == OS_SHIM_S_OK);
        OS_REQUIRE(crd_written == gdef_plain.size());
        OS_REQUIRE(std::memcmp(va_alias, gdef_plain.data(), gdef_plain.size()) == 0);
        // Confirm fail-closed crash dump stub stays E_NOTIMPL.
        OS_REQUIRE(fn_crd_md(0u, "test.dmp") == static_cast<int32_t>(0x80004001u));

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        XMemFree(xmem_dst, xmem_phys_attr);
        OS_REQUIRE(UnmapTitleVirtualMemory(va_write, 2u) == 1);
        OS_REQUIRE(XMemUnmapPhysicalPages(va_alias, 2u) == 1);
        OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 2u, pfns) == 1);
        std::printf("  Physical-page decompression: alias coherent + XMem verified + RO rejected!\n");
    }

    factory->Release();
#endif
}

// ===========================================================================
// 2. New HLE/Diagnostic Shims: mfplat_xs, advapi32legacy, KernelTraceControl,
//    hix_xs, and xbtp (dynamic LoadLibrary + exports + CallTelemetry checks)
// ===========================================================================
void TestNewHleAndDiagnosticShims() {
    std::printf("[Test 2] New Shims (mfplat_xs, advapi32legacy, KernelTraceControl, hix_xs, xbtp)...\n");
#if defined(_WIN32)
    const fs::path bin_dir = GetExeDir();
    using PfnTelCount = int32_t(__stdcall*)(const char*, uint64_t*, uint64_t*);

    // 2A. mfplat_xs.dll
    {
        const fs::path dll_path = bin_dir / "mfplat_xs.dll";
        HMODULE mod = LoadLibraryW(dll_path.c_str());
        OS_REQUIRE(mod != nullptr);

        using PfnMFStartup = int32_t(__stdcall*)(uint32_t, uint32_t);
        using PfnMFShutdown = int32_t(__stdcall*)(void);
        using PfnMFCreateDxgi = int32_t(__stdcall*)(uint32_t*, void**);
        using PfnMFResetDxgiX = int32_t(__stdcall*)(void*, void*, uint32_t);
        using PfnMFCreateMt = int32_t(__stdcall*)(void**);
        using PfnMFCreateSmp = int32_t(__stdcall*)(void**);
        using PfnMFCreateBuf = int32_t(__stdcall*)(uint32_t, void**);
        using PfnMFAllocWq = int32_t(__stdcall*)(uint32_t*);
        using PfnMFUnlockWq = int32_t(__stdcall*)(uint32_t);
        using PfnMFGetSysTime = int64_t(__stdcall*)(void);

        auto fn_start = reinterpret_cast<PfnMFStartup>(reinterpret_cast<void*>(GetProcAddress(mod, "MFStartup")));
        auto fn_stop = reinterpret_cast<PfnMFShutdown>(reinterpret_cast<void*>(GetProcAddress(mod, "MFShutdown")));
        auto fn_create_mgr = reinterpret_cast<PfnMFCreateDxgi>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateDXGIDeviceManager")));
        auto fn_reset_x = reinterpret_cast<PfnMFResetDxgiX>(reinterpret_cast<void*>(GetProcAddress(mod, "MFResetDXGIDeviceManagerX")));
        auto fn_create_mt = reinterpret_cast<PfnMFCreateMt>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateMediaType")));
        auto fn_create_smp = reinterpret_cast<PfnMFCreateSmp>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateSample")));
        auto fn_create_buf = reinterpret_cast<PfnMFCreateBuf>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateMemoryBuffer")));
        auto fn_alloc_wq = reinterpret_cast<PfnMFAllocWq>(reinterpret_cast<void*>(GetProcAddress(mod, "MFAllocateWorkQueue")));
        auto fn_unlock_wq = reinterpret_cast<PfnMFUnlockWq>(reinterpret_cast<void*>(GetProcAddress(mod, "MFUnlockWorkQueue")));
        auto fn_time = reinterpret_cast<PfnMFGetSysTime>(reinterpret_cast<void*>(GetProcAddress(mod, "MFGetSystemTime")));
        auto fn_tel = reinterpret_cast<PfnTelCount>(reinterpret_cast<void*>(GetProcAddress(mod, "OpenScarlettMfPlatGetTelemetryCount")));

        OS_REQUIRE(fn_start && fn_stop && fn_create_mgr && fn_reset_x);
        OS_REQUIRE(fn_create_mt && fn_create_smp && fn_create_buf && fn_alloc_wq && fn_unlock_wq && fn_time && fn_tel);

        constexpr uint32_t kMfVersion = (0x0002u << 16u) | 0x0070u;
        OS_REQUIRE(fn_start(kMfVersion, 0x1u) == 0);

        uint32_t reset_tok = 0u;
        void* mgr = nullptr;
        OS_REQUIRE(fn_create_mgr(&reset_tok, &mgr) == 0 && mgr != nullptr && reset_tok != 0u);

        // Null pointer validation on MFResetDXGIDeviceManagerX
        OS_REQUIRE(fn_reset_x(nullptr, nullptr, reset_tok) < 0);
        OS_REQUIRE(fn_reset_x(mgr, nullptr, reset_tok) < 0);

        // Create a real D3D12 device (WARP or hardware) and reset via MFResetDXGIDeviceManagerX!
        ID3D12Device* d3d_dev = nullptr;
        HRESULT dev_hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d_dev));
        if (FAILED(dev_hr) || !d3d_dev) {
            IDXGIFactory4* dxgi_fac = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_fac))) && dxgi_fac) {
                IDXGIAdapter* warp = nullptr;
                if (SUCCEEDED(dxgi_fac->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
                    (void)D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d_dev));
                    warp->Release();
                }
                dxgi_fac->Release();
            }
        }
        if (d3d_dev) {
            // Note: Windows IMFDXGIDeviceManager::ResetDevice expects D3D11 or D3D12/IUnknown
            // (Win10+ accepts D3D11 and rejects D3D12 with E_NOINTERFACE on host MF, which is
            // still a valid forwarded call proving real vtable dispatch). Exercise it cleanly!
            const int32_t rst_hr = fn_reset_x(mgr, d3d_dev, reset_tok);
            OS_REQUIRE(rst_hr == 0 || rst_hr == static_cast<int32_t>(E_NOINTERFACE) ||
                       rst_hr == static_cast<int32_t>(E_INVALIDARG));
            d3d_dev->Release();
        }
        static_cast<IUnknown*>(mgr)->Release();

        // Standard MF forwarded APIs
        uint32_t wq = 0u;
        OS_REQUIRE(fn_alloc_wq(&wq) == 0 && wq != 0u);
        OS_REQUIRE(fn_unlock_wq(wq) == 0);
        OS_REQUIRE(fn_time() >= 0);

        void* mt = nullptr;
        if (fn_create_mt(&mt) == 0 && mt) {
            static_cast<IUnknown*>(mt)->Release();
        }
        void* smp = nullptr;
        if (fn_create_smp(&smp) == 0 && smp) {
            static_cast<IUnknown*>(smp)->Release();
        }
        void* mbuf = nullptr;
        if (fn_create_buf(1024u, &mbuf) == 0 && mbuf) {
            static_cast<IUnknown*>(mbuf)->Release();
        }

        OS_REQUIRE(fn_stop() == 0);

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("MFStartup", &tot, &ok) == 1 && tot >= 1u && ok >= 1u);
        OS_REQUIRE(fn_tel("MFCreateDXGIDeviceManager", &tot, &ok) == 1 && tot >= 1u && ok >= 1u);
        OS_REQUIRE(fn_tel("MFResetDXGIDeviceManagerX", &tot, &ok) == 1 && tot >= 2u);
        OS_REQUIRE(fn_tel("MFShutdown", &tot, &ok) == 1 && tot >= 1u && ok >= 1u);

        FreeLibrary(mod);
        std::printf("  PASS: mfplat_xs.dll (MFResetDXGIDeviceManagerX + host forwarding + telemetry)\n");
    }

    // 2B. advapi32legacy.dll
    {
        const fs::path dll_path = bin_dir / "advapi32legacy.dll";
        HMODULE mod = LoadLibraryW(dll_path.c_str());
        OS_REQUIRE(mod != nullptr);

        using PfnRegOpenW = int32_t(__stdcall*)(void*, const wchar_t*, uint32_t, uint32_t, void**);
        using PfnRegOpenA = int32_t(__stdcall*)(void*, const char*, uint32_t, uint32_t, void**);
        using PfnRegQueryW = int32_t(__stdcall*)(void*, const wchar_t*, uint32_t*, uint32_t*, uint8_t*, uint32_t*);
        using PfnRegQueryA = int32_t(__stdcall*)(void*, const char*, uint32_t*, uint32_t*, uint8_t*, uint32_t*);
        using PfnRegClose = int32_t(__stdcall*)(void*);
        using PfnGetUserW = int32_t(__stdcall*)(wchar_t*, uint32_t*);
        using PfnGetUserA = int32_t(__stdcall*)(char*, uint32_t*);
        using PfnCryptAcqW = int32_t(__stdcall*)(uintptr_t*, const wchar_t*, const wchar_t*, uint32_t, uint32_t);
        using PfnCryptRand = int32_t(__stdcall*)(uintptr_t, uint32_t, uint8_t*);
        using PfnCryptRel = int32_t(__stdcall*)(uintptr_t, uint32_t);
        using PfnEvtReg = uint32_t(__stdcall*)(const void*, void*, void*, uint64_t*);
        using PfnEvtWrite = uint32_t(__stdcall*)(uint64_t, const void*, uint32_t, void*);
        using PfnEvtUnreg = uint32_t(__stdcall*)(uint64_t);

        auto fn_open_w = reinterpret_cast<PfnRegOpenW>(reinterpret_cast<void*>(GetProcAddress(mod, "RegOpenKeyExW")));
        auto fn_open_a = reinterpret_cast<PfnRegOpenA>(reinterpret_cast<void*>(GetProcAddress(mod, "RegOpenKeyExA")));
        auto fn_query_w = reinterpret_cast<PfnRegQueryW>(reinterpret_cast<void*>(GetProcAddress(mod, "RegQueryValueExW")));
        auto fn_query_a = reinterpret_cast<PfnRegQueryA>(reinterpret_cast<void*>(GetProcAddress(mod, "RegQueryValueExA")));
        auto fn_close = reinterpret_cast<PfnRegClose>(reinterpret_cast<void*>(GetProcAddress(mod, "RegCloseKey")));
        auto fn_user_w = reinterpret_cast<PfnGetUserW>(reinterpret_cast<void*>(GetProcAddress(mod, "GetUserNameW")));
        auto fn_user_a = reinterpret_cast<PfnGetUserA>(reinterpret_cast<void*>(GetProcAddress(mod, "GetUserNameA")));
        auto fn_crypt_acq = reinterpret_cast<PfnCryptAcqW>(reinterpret_cast<void*>(GetProcAddress(mod, "CryptAcquireContextW")));
        auto fn_crypt_rnd = reinterpret_cast<PfnCryptRand>(reinterpret_cast<void*>(GetProcAddress(mod, "CryptGenRandom")));
        auto fn_crypt_rel = reinterpret_cast<PfnCryptRel>(reinterpret_cast<void*>(GetProcAddress(mod, "CryptReleaseContext")));
        auto fn_evt_reg = reinterpret_cast<PfnEvtReg>(reinterpret_cast<void*>(GetProcAddress(mod, "EventRegister")));
        auto fn_evt_write = reinterpret_cast<PfnEvtWrite>(reinterpret_cast<void*>(GetProcAddress(mod, "EventWrite")));
        auto fn_evt_unreg = reinterpret_cast<PfnEvtUnreg>(reinterpret_cast<void*>(GetProcAddress(mod, "EventUnregister")));
        auto fn_tel = reinterpret_cast<PfnTelCount>(reinterpret_cast<void*>(GetProcAddress(mod, "OpenScarlettAdvapiLegacyGetTelemetryCount")));

        OS_REQUIRE(fn_open_w && fn_open_a && fn_query_w && fn_query_a && fn_close);
        OS_REQUIRE(fn_user_w && fn_user_a && fn_crypt_acq && fn_crypt_rnd && fn_crypt_rel);
        OS_REQUIRE(fn_evt_reg && fn_evt_write && fn_evt_unreg && fn_tel);

        // Registry read (W & A) against standard Windows NT CurrentVersion key
        void* hkey = nullptr;
        OS_REQUIRE(fn_open_w(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             0u, KEY_READ, &hkey) == 0 && hkey != nullptr);
        wchar_t prod_w[128]{};
        uint32_t type = 0u;
        uint32_t cb = sizeof(prod_w);
        OS_REQUIRE(fn_query_w(hkey, L"ProductName", nullptr, &type,
                              reinterpret_cast<uint8_t*>(prod_w), &cb) == 0 && cb > 0u);
        OS_REQUIRE(fn_close(hkey) == 0);

        void* hkey_a = nullptr;
        OS_REQUIRE(fn_open_a(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             0u, KEY_READ, &hkey_a) == 0 && hkey_a != nullptr);
        char prod_a[128]{};
        cb = sizeof(prod_a);
        OS_REQUIRE(fn_query_a(hkey_a, "ProductName", nullptr, &type,
                              reinterpret_cast<uint8_t*>(prod_a), &cb) == 0 && cb > 0u);
        OS_REQUIRE(fn_close(hkey_a) == 0);

        // GetUserNameW / A
        wchar_t uname_w[128]{};
        uint32_t ulen = 128u;
        OS_REQUIRE(fn_user_w(uname_w, &ulen) != 0 && std::wcslen(uname_w) > 0u);
        char uname_a[128]{};
        ulen = 128u;
        OS_REQUIRE(fn_user_a(uname_a, &ulen) != 0 && std::strlen(uname_a) > 0u);

        // CryptAcquireContextW / CryptGenRandom / CryptReleaseContext
        uintptr_t hprov = 0u;
        OS_REQUIRE(fn_crypt_acq(&hprov, nullptr, nullptr, 1u, 0xF0000000u) != 0 && hprov != 0u);
        std::array<uint8_t, 32> rnd{};
        OS_REQUIRE(fn_crypt_rnd(hprov, static_cast<uint32_t>(rnd.size()), rnd.data()) != 0);
        bool any_nonzero = false;
        for (uint8_t b : rnd) {
            if (b != 0u) any_nonzero = true;
        }
        OS_REQUIRE(any_nonzero);
        OS_REQUIRE(fn_crypt_rel(hprov, 0u) != 0);

        // EventRegister / EventWrite / EventUnregister
        const GUID prov_guid = {0x5CA01234u, 0x1111u, 0x2222u, {1, 2, 3, 4, 5, 6, 7, 8}};
        uint64_t reg_h = 0ull;
        OS_REQUIRE(fn_evt_reg(&prov_guid, nullptr, nullptr, &reg_h) == 0u && reg_h != 0ull);
        const uint8_t evt_desc[16] = {1, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        OS_REQUIRE(fn_evt_write(reg_h, evt_desc, 0u, nullptr) == 0u);
        OS_REQUIRE(fn_evt_unreg(reg_h) == 0u);

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("RegOpenKeyExW", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("CryptGenRandom", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("EventRegister", &tot, &ok) == 1 && ok >= 1u);

        FreeLibrary(mod);
        std::printf("  PASS: advapi32legacy.dll (Reg* W/A, GetUserName W/A, Crypt*, Event* + telemetry)\n");
    }

    // 2C. KernelTraceControl.dll
    {
        const fs::path dll_path = bin_dir / "KernelTraceControl.dll";
        HMODULE mod = LoadLibraryW(dll_path.c_str());
        OS_REQUIRE(mod != nullptr);

        using PfnStart = uint32_t(__stdcall*)(uint64_t*, void*, const void*, uint32_t);
        using PfnStartProc = uint32_t(__stdcall*)(uint64_t*, void*, uint32_t);
        using PfnStop = uint32_t(__stdcall*)(uint64_t, void*);
        using PfnUpdate = uint32_t(__stdcall*)(uint64_t, void*);
        using PfnEnable = uint32_t(__stdcall*)(uint64_t, const void*, uint32_t, uint8_t, uint64_t, uint64_t, uint32_t, void*);
        using PfnQuery = uint32_t(__stdcall*)(uint64_t, uint32_t, void*, uint32_t, uint32_t*);
        using PfnSet = uint32_t(__stdcall*)(uint64_t, uint32_t, void*, uint32_t);
        using PfnMerge = uint32_t(__stdcall*)(const wchar_t*, const wchar_t**, uint32_t, uint32_t);

        auto fn_start = reinterpret_cast<PfnStart>(reinterpret_cast<void*>(GetProcAddress(mod, "StartKernelTrace")));
        auto fn_start_p = reinterpret_cast<PfnStartProc>(reinterpret_cast<void*>(GetProcAddress(mod, "StartKernelTraceForProcess")));
        auto fn_stop = reinterpret_cast<PfnStop>(reinterpret_cast<void*>(GetProcAddress(mod, "StopKernelTrace")));
        auto fn_update = reinterpret_cast<PfnUpdate>(reinterpret_cast<void*>(GetProcAddress(mod, "UpdateKernelTrace")));
        auto fn_enable = reinterpret_cast<PfnEnable>(reinterpret_cast<void*>(GetProcAddress(mod, "EnableTraceEx2")));
        auto fn_query = reinterpret_cast<PfnQuery>(reinterpret_cast<void*>(GetProcAddress(mod, "TraceQueryInformation")));
        auto fn_set = reinterpret_cast<PfnSet>(reinterpret_cast<void*>(GetProcAddress(mod, "TraceSetInformation")));
        auto fn_merge = reinterpret_cast<PfnMerge>(reinterpret_cast<void*>(GetProcAddress(mod, "CreateMergedTraceFile")));
        auto fn_tel = reinterpret_cast<PfnTelCount>(reinterpret_cast<void*>(GetProcAddress(mod, "OpenScarlettKernelTraceGetTelemetryCount")));

        OS_REQUIRE(fn_start && fn_start_p && fn_stop && fn_update && fn_enable && fn_query && fn_set && fn_merge && fn_tel);

        uint64_t h1 = 0ull, h2 = 0ull;
        OS_REQUIRE(fn_start(nullptr, nullptr, nullptr, 0u) == 87u);
        OS_REQUIRE(fn_start(&h1, nullptr, nullptr, 0u) == 0u && h1 != 0ull);
        OS_REQUIRE(fn_start_p(&h2, nullptr, GetCurrentProcessId()) == 0u && h2 != 0ull && h2 != h1);
        const GUID pid = {0x11223344u, 0, 0, {1, 2, 3, 4, 5, 6, 7, 8}};
        OS_REQUIRE(fn_enable(h1, &pid, 1u, 4u, 0xFFull, 0ull, 0u, nullptr) == 0u);
        uint8_t info_buf[32]{};
        uint32_t ret_len = 0u;
        OS_REQUIRE(fn_query(h1, 1u, info_buf, sizeof(info_buf), &ret_len) == 0u && ret_len == sizeof(info_buf));
        OS_REQUIRE(fn_set(h1, 1u, info_buf, sizeof(info_buf)) == 0u);
        OS_REQUIRE(fn_update(h1, nullptr) == 0u);
        OS_REQUIRE(fn_merge(L"merged.etl", nullptr, 0u, 0u) == 0u);
        OS_REQUIRE(fn_stop(h1, nullptr) == 0u);
        OS_REQUIRE(fn_stop(h1, nullptr) == 6u);  // duplicate stop fails with ERROR_INVALID_HANDLE
        OS_REQUIRE(fn_stop(h2, nullptr) == 0u);

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("StartKernelTrace", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("EnableTraceEx2", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("TraceQueryInformation", &tot, &ok) == 1 && ok >= 1u);

        FreeLibrary(mod);
        std::printf("  PASS: KernelTraceControl.dll (Start/Stop/Enable/Query/Merge + telemetry)\n");
    }

    // 2D. hix_xs.dll
    {
        const fs::path dll_path = bin_dir / "hix_xs.dll";
        HMODULE mod = LoadLibraryW(dll_path.c_str());
        OS_REQUIRE(mod != nullptr);

        using PfnInit = int32_t(__stdcall*)(uint32_t);
        using PfnUninit = void(__stdcall*)(void);
        using PfnSetTo = int32_t(__stdcall*)(uint32_t);
        using PfnGetTo = int32_t(__stdcall*)(uint32_t*);
        using PfnHb = int32_t(__stdcall*)(const char*);
        using PfnRegCb = int32_t(__stdcall*)(void*, void*, uint64_t*);
        using PfnUnregCb = int32_t(__stdcall*)(uint64_t);
        using PfnBc = int32_t(__stdcall*)(uint32_t, const char*, uint64_t);
        using PfnDump = int32_t(__stdcall*)(const char*, const char*);
        using PfnStats = int32_t(__stdcall*)(uint64_t*, uint64_t*);

        auto fn_init = reinterpret_cast<PfnInit>(reinterpret_cast<void*>(GetProcAddress(mod, "HixInitialize")));
        auto fn_uninit = reinterpret_cast<PfnUninit>(reinterpret_cast<void*>(GetProcAddress(mod, "HixUninitialize")));
        auto fn_set_to = reinterpret_cast<PfnSetTo>(reinterpret_cast<void*>(GetProcAddress(mod, "HixSetHangTimeoutMs")));
        auto fn_get_to = reinterpret_cast<PfnGetTo>(reinterpret_cast<void*>(GetProcAddress(mod, "HixGetHangTimeoutMs")));
        auto fn_hb = reinterpret_cast<PfnHb>(reinterpret_cast<void*>(GetProcAddress(mod, "HixHeartbeat")));
        auto fn_reg = reinterpret_cast<PfnRegCb>(reinterpret_cast<void*>(GetProcAddress(mod, "HixRegisterHangCallback")));
        auto fn_unreg = reinterpret_cast<PfnUnregCb>(reinterpret_cast<void*>(GetProcAddress(mod, "HixUnregisterHangCallback")));
        auto fn_bc = reinterpret_cast<PfnBc>(reinterpret_cast<void*>(GetProcAddress(mod, "HixRecordBreadcrumb")));
        auto fn_dump = reinterpret_cast<PfnDump>(reinterpret_cast<void*>(GetProcAddress(mod, "HixRequestHangDump")));
        auto fn_stats = reinterpret_cast<PfnStats>(reinterpret_cast<void*>(GetProcAddress(mod, "HixGetStats")));
        auto fn_tel = reinterpret_cast<PfnTelCount>(reinterpret_cast<void*>(GetProcAddress(mod, "OpenScarlettHixGetTelemetryCount")));

        OS_REQUIRE(fn_init && fn_uninit && fn_set_to && fn_get_to && fn_hb && fn_reg && fn_unreg && fn_bc && fn_dump && fn_stats && fn_tel);

        OS_REQUIRE(fn_init(1u) == 0);
        OS_REQUIRE(fn_set_to(0u) < 0);
        OS_REQUIRE(fn_set_to(7500u) == 0);
        uint32_t got_to = 0u;
        OS_REQUIRE(fn_get_to(&got_to) == 0 && got_to == 7500u);
        OS_REQUIRE(fn_hb("RenderThreadFrame1") == 0);
        uint64_t cb_tok = 0ull;
        OS_REQUIRE(fn_reg(reinterpret_cast<void*>(fn_hb), nullptr, &cb_tok) == 0 && cb_tok != 0ull);
        OS_REQUIRE(fn_unreg(cb_tok) == 0);
        OS_REQUIRE(fn_unreg(cb_tok) < 0);

        // Verify breadcrumb forwarding into xcrdapi.dll!
        OS_REQUIRE(XCrdInitialize(1u) == OS_SHIM_S_OK);
        OS_REQUIRE(fn_bc(12u, "HixFrameSync", 0xCAFE5CA0ull) == 0);
        char bc_tag[64]{};
        uint64_t bc_val = 0ull;
        OS_REQUIRE(XCrdGetBreadcrumb(12u, bc_tag, sizeof(bc_tag), &bc_val) == OS_SHIM_S_OK);
        OS_REQUIRE(std::strcmp(bc_tag, "HixFrameSync") == 0 && bc_val == 0xCAFE5CA0ull);
        XCrdUninitialize();

        const fs::path dump_json = GetScratchDir() / "hix_hang_summary.json";
        OS_REQUIRE(fn_dump("SyntheticTimeout", dump_json.string().c_str()) == 0);
        std::ifstream ifs(dump_json);
        OS_REQUIRE(ifs.good());
        const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        OS_REQUIRE(content.find("SyntheticTimeout") != std::string::npos);

        uint64_t hb_c = 0u, dump_c = 0u;
        OS_REQUIRE(fn_stats(&hb_c, &dump_c) == 0 && hb_c >= 1u && dump_c >= 1u);
        fn_uninit();

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("HixHeartbeat", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("HixRequestHangDump", &tot, &ok) == 1 && ok >= 1u);

        FreeLibrary(mod);
        std::printf("  PASS: hix_xs.dll (timeout, heartbeat, xcrdapi breadcrumb forward, dump JSON + telemetry)\n");
    }

    // 2E. xbtp.dll
    {
        const fs::path dll_path = bin_dir / "xbtp.dll";
        HMODULE mod = LoadLibraryW(dll_path.c_str());
        OS_REQUIRE(mod != nullptr);

        using PfnInit = int32_t(__stdcall*)(uint32_t);
        using PfnShut = void(__stdcall*)(void);
        using PfnOpen = int32_t(__stdcall*)(const char*, uint32_t, void**);
        using PfnClose = int32_t(__stdcall*)(void*);
        using PfnWrite = int32_t(__stdcall*)(void*, uint32_t, const void*, uint32_t);
        using PfnFlush = int32_t(__stdcall*)(void*);
        using PfnStats = int32_t(__stdcall*)(void*, uint64_t*, uint64_t*, uint64_t*);

        auto fn_init = reinterpret_cast<PfnInit>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpInitialize")));
        auto fn_shut = reinterpret_cast<PfnShut>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpShutdown")));
        auto fn_open = reinterpret_cast<PfnOpen>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpOpenSession")));
        auto fn_close = reinterpret_cast<PfnClose>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpCloseSession")));
        auto fn_write = reinterpret_cast<PfnWrite>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpWriteEvent")));
        auto fn_flush = reinterpret_cast<PfnFlush>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpFlushSession")));
        auto fn_stats = reinterpret_cast<PfnStats>(reinterpret_cast<void*>(GetProcAddress(mod, "XbtpGetSessionStats")));
        auto fn_tel = reinterpret_cast<PfnTelCount>(reinterpret_cast<void*>(GetProcAddress(mod, "OpenScarlettXbtpGetTelemetryCount")));

        OS_REQUIRE(fn_init && fn_shut && fn_open && fn_close && fn_write && fn_flush && fn_stats && fn_tel);

        OS_REQUIRE(fn_init(0u) == 0);
        void* sess = nullptr;
        OS_REQUIRE(fn_open("", 0u, &sess) < 0);
        OS_REQUIRE(fn_open("FrameTelemetry", 1u, &sess) == 0 && sess != nullptr);

        const uint64_t payload[2] = {0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
        OS_REQUIRE(fn_write(sess, 1001u, payload, sizeof(payload)) == 0);
        OS_REQUIRE(fn_write(sess, 1002u, nullptr, 16u) < 0);
        OS_REQUIRE(fn_flush(sess) == 0);

        uint64_t ev_cnt = 0u, by_cnt = 0u, fl_cnt = 0u;
        OS_REQUIRE(fn_stats(sess, &ev_cnt, &by_cnt, &fl_cnt) == 0);
        OS_REQUIRE(ev_cnt == 1u && by_cnt == sizeof(payload) && fl_cnt == 1u);

        OS_REQUIRE(fn_close(sess) == 0);
        OS_REQUIRE(fn_close(sess) < 0);  // duplicate close fails cleanly
        fn_shut();

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("XbtpOpenSession", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("XbtpWriteEvent", &tot, &ok) == 1 && ok >= 1u);
        OS_REQUIRE(fn_tel("XbtpFlushSession", &tot, &ok) == 1 && ok >= 1u);

        FreeLibrary(mod);
        std::printf("  PASS: xbtp.dll (open/write/flush/stats/close + duplicate-close reject + telemetry)\n");
    }
#endif
}

// ===========================================================================
// 3. XMem & XThread Deterministic Edge-Case Conformance (kernelx + xmem)
// ===========================================================================
void TestXMemAndXThreadEdgeCases() {
    std::printf("[Test 3] XMem & XThread deterministic edge cases (kernelx.dll + xmem.dll)...\n");
#if defined(_WIN32)
    // 3A. Zero-size allocs
    SetLastError(0u);
    OS_REQUIRE(XMemAlloc(0u, 0ull) == nullptr);
    OS_REQUIRE(GetLastError() == 87u);
    SetLastError(0u);
    OS_REQUIRE(XMemAllocDefault(0u, 0ull) == nullptr);
    OS_REQUIRE(GetLastError() == 87u);

    // 3B. Invalid memory type attribute (> XALLOC_MEMTYPE_PHYSICAL) & invalid alignments
    const uint64_t bad_memtype = MAKE_XALLOC_ATTRIBUTES(0, 1, 0, 0, 1, XALLOC_ALIGNMENT_16, 0xFu, 0, 0);
    SetLastError(0u);
    OS_REQUIRE(XMemAlloc(256u, bad_memtype) == nullptr);
    OS_REQUIRE(GetLastError() == 87u);

    OS_REQUIRE(XMemAllocAligned(256u, 3u, 0ull) == nullptr);         // non-power-of-two
    OS_REQUIRE(XMemAllocAligned(256u, 48u, 0ull) == nullptr);        // non-power-of-two
    OS_REQUIRE(XMemAllocAligned(256u, 8u * 1024u * 1024u, 0ull) == nullptr);  // > 4 MiB
    OS_REQUIRE(XMemAlloc(SIZE_MAX - 8u, 0ull) == nullptr);           // overflow guard

    void* p_aligned = XMemAllocAligned(1000u, 4096u, XALLOC_FLAG_ZERO_INIT);
    OS_REQUIRE(p_aligned != nullptr);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p_aligned) % 4096u) == 0u);
    OS_REQUIRE(XMemSize(p_aligned, 0ull) == 1000u);

    // 3C. Duplicate free & null free (deterministic, no crash, sets ERROR_INVALID_PARAMETER)
    SetLastError(0u);
    XMemFree(nullptr, 0ull);
    XMemFreeDefault(nullptr, 0ull);

    SetLastError(0u);
    XMemFree(p_aligned, 0ull);
    OS_REQUIRE(GetLastError() == 0u);
    OS_REQUIRE(XMemSize(p_aligned, 0ull) == 0u);

    SetLastError(0u);
    XMemFree(p_aligned, 0ull);  // duplicate free
    OS_REQUIRE(GetLastError() == 87u);

    void* p_def = XMemAllocDefault(512u, 0ull);
    OS_REQUIRE(p_def != nullptr);
    SetLastError(0u);
    XMemFreeDefault(p_def, 0ull);
    OS_REQUIRE(GetLastError() == 0u);
    SetLastError(0u);
    XMemFreeDefault(p_def, 0ull);  // duplicate default free
    OS_REQUIRE(GetLastError() == 87u);

    // 3D. Verify xmem.dll forwarding for XMemAllocAligned + duplicate free error telemetry
    {
        const fs::path xmem_path = GetExeDir() / "xmem.dll";
        HMODULE xmem_mod = LoadLibraryW(xmem_path.c_str());
        OS_REQUIRE(xmem_mod != nullptr);
        using PfnAligned = void*(OS_SHIM_CALL*)(size_t, size_t, uint64_t);
        auto fn_aligned = reinterpret_cast<PfnAligned>(
            reinterpret_cast<void*>(GetProcAddress(xmem_mod, "XMemAllocAligned")));
        auto fn_free_def = reinterpret_cast<PFN_XMemFreeDefault>(
            reinterpret_cast<void*>(GetProcAddress(xmem_mod, "XMemFreeDefault")));
        auto fn_tel = reinterpret_cast<PFN_XMemGetTelemetryCallCountForTest>(
            reinterpret_cast<void*>(GetProcAddress(xmem_mod, "XMemGetTelemetryCallCountForTest")));
        OS_REQUIRE(fn_aligned && fn_free_def && fn_tel);

        OS_REQUIRE(fn_aligned(128u, 3u, 0ull) == nullptr);
        void* xp = fn_aligned(512u, 256u, 0ull);
        OS_REQUIRE(xp != nullptr && (reinterpret_cast<uintptr_t>(xp) % 256u) == 0u);
        fn_free_def(xp, 0ull);
        fn_free_def(xp, 0ull);  // duplicate free tracked as error in xmem telemetry

        uint64_t tot = 0u, ok = 0u;
        OS_REQUIRE(fn_tel("XMemAllocAligned", &tot, &ok) == 1 && tot >= 2u && ok >= 1u);
        OS_REQUIRE(fn_tel("XMemFreeDefault", &tot, &ok) == 1 && tot > ok);
        FreeLibrary(xmem_mod);
    }

    // 3E. Out-of-range physical pages
    {
        uint64_t zero_pages = 0u;
        uint64_t dummy_pfn = 0u;
        OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS, &zero_pages, &dummy_pfn) == 0);
        uint64_t huge_pages = 0xFFFFFFFFull;
        OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS, &huge_pages, &dummy_pfn) == 0);
        OS_REQUIRE(huge_pages == 0u);

        const uint64_t bogus_pfn = 0xDEADBEEFull;
        OS_REQUIRE(MapTitleVirtualMemory(nullptr, 1u, MEM_GRAPHICS, PAGE_READWRITE, &bogus_pfn) == nullptr);
        OS_REQUIRE(MapTitleVirtualMemory(nullptr, 0xFFFFFFFFull, MEM_GRAPHICS, PAGE_READWRITE, &bogus_pfn) == nullptr);
        OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 1u, &bogus_pfn) == 0);
        OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 0xFFFFFFFFull, &bogus_pfn) == 0);
        OS_REQUIRE(UnmapTitleVirtualMemory(reinterpret_cast<void*>(0x12340000u), 1u) == 0);
    }

    // 3F. XThreadSetAffinityMask / priority clamping / invalid handle handling
    {
        HANDLE cur_thread = GetCurrentThread();
        void* bogus_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(0x0000DEADu));

        // Invalid handles
        OS_REQUIRE(XThreadSetAffinityMask(nullptr, 1ull) == 0ull);
        OS_REQUIRE(GetLastError() == 6u);
        OS_REQUIRE(XThreadSetAffinityMask(INVALID_HANDLE_VALUE, 1ull) == 0ull);
        OS_REQUIRE(XThreadSetAffinityMask(bogus_handle, 1ull) == 0ull);

        OS_REQUIRE(XThreadSetPriority(nullptr, 0) == 0);
        OS_REQUIRE(XThreadSetPriority(INVALID_HANDLE_VALUE, 0) == 0);
        OS_REQUIRE(XThreadSetPriority(bogus_handle, 0) == 0);
        OS_REQUIRE(XThreadGetPriority(INVALID_HANDLE_VALUE) == KERNELX_THREAD_PRIORITY_ERROR_RETURN);
        OS_REQUIRE(SetThreadName(INVALID_HANDLE_VALUE, L"Bad") < 0);
        wchar_t tmp_name[16]{};
        OS_REQUIRE(GetThreadName(INVALID_HANDLE_VALUE, tmp_name, 16u, nullptr) < 0);

        // Invalid affinity masks
        OS_REQUIRE(XThreadSetAffinityMask(cur_thread, 0ull) == 0ull);
        OS_REQUIRE(GetLastError() == 87u);
        OS_REQUIRE(XThreadSetAffinityMask(cur_thread, 1ull << 63u) == 0ull);
        OS_REQUIRE(GetLastError() == 87u);

        // Valid affinity masks (including high Scarlett core bit 12 folded on <=12-core hosts)
        const uint64_t prev1 = XThreadSetAffinityMask(cur_thread, 1ull);
        OS_REQUIRE(prev1 != 0ull);
        const uint64_t prev2 = XThreadSetAffinityMask(cur_thread, 1ull << 12u);
        OS_REQUIRE(prev2 == 1ull);
        OS_REQUIRE(XThreadSetAffinityMask(cur_thread, prev1) != 0ull);

        // Priority clamping across extreme & intermediate values
        const int32_t orig_prio = XThreadGetPriority(cur_thread);
        OS_REQUIRE(orig_prio != KERNELX_THREAD_PRIORITY_ERROR_RETURN);

        OS_REQUIRE(XThreadSetPriority(cur_thread, -999) == 1);
        OS_REQUIRE(XThreadGetPriority(cur_thread) == THREAD_PRIORITY_IDLE);

        OS_REQUIRE(XThreadSetPriority(cur_thread, -7) == 1);
        OS_REQUIRE(XThreadGetPriority(cur_thread) == THREAD_PRIORITY_LOWEST);

        OS_REQUIRE(XThreadSetPriority(cur_thread, 7) == 1);
        OS_REQUIRE(XThreadGetPriority(cur_thread) == THREAD_PRIORITY_HIGHEST);

        OS_REQUIRE(XThreadSetPriority(cur_thread, 0) == 1);
        OS_REQUIRE(XThreadGetPriority(cur_thread) == THREAD_PRIORITY_NORMAL);

        OS_REQUIRE(XThreadSetPriority(cur_thread, orig_prio) == 1);
    }

    std::printf("  PASS: XMem (zero-size, bad alignment/memtype, dup-free, OOR phys) & XThread (affinity/priority/handles)\n");
#endif
}

// ===========================================================================
// 4. WASAPI MMDevice Endpoint ID Formats + Opus / IMA-ADPCM Conformance
// ===========================================================================
void TestWasapiEndpointFormatsAndCodecs() {
    std::printf("[Test 4] WASAPI MMDevice endpoint ID formats (wide/narrow/SWD/GUID/default) + Opus/ADPCM...\n");

    wchar_t canon_w[256]{};
    char canon_a[256]{};

    // Valid wide formats
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(nullptr, canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(canon_w[0] == L'\0');
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(L"", canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(L"default", canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(L"DEFAULT", canon_w, 256u) == OS_XAUDIO2_S_OK);

    const wchar_t* kCanonIdW = L"{0.0.0.00000000}.{12345678-9abc-def0-1234-56789abcdef0}";
    const wchar_t* kSwdFullW =
        L"\\\\?\\SWD#MMDEVAPI#{0.0.0.00000000}.{12345678-9ABC-DEF0-1234-56789ABCDEF0}#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    const wchar_t* kSwdShortW =
        L"\\\\?\\SWD#MMDEVAPI#{0.0.0.00000000}.{12345678-9abc-def0-1234-56789abcdef0}";
    const wchar_t* kBareGuidW = L"{12345678-9ABC-DEF0-1234-56789ABCDEF0}";

    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(kCanonIdW, canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(std::wcscmp(canon_w, kCanonIdW) == 0);

    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(kSwdFullW, canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(std::wcscmp(canon_w, kCanonIdW) == 0);

    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(kSwdShortW, canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(std::wcscmp(canon_w, kCanonIdW) == 0);

    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(kBareGuidW, canon_w, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(std::wcscmp(canon_w, kCanonIdW) == 0);

    // Valid narrow formats
    const char* kCanonIdA = "{0.0.0.00000000}.{12345678-9abc-def0-1234-56789abcdef0}";
    const char* kSwdFullA =
        "\\\\?\\SWD#MMDEVAPI#{0.0.0.00000000}.{12345678-9ABC-DEF0-1234-56789ABCDEF0}#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdA(nullptr, canon_a, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdA("", canon_a, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdA("default", canon_a, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdA(kCanonIdA, canon_a, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(std::strcmp(canon_a, kCanonIdA) == 0);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdA(kSwdFullA, canon_a, 256u) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(std::strcmp(canon_a, kCanonIdA) == 0);

    // Malformed endpoint IDs rejected cleanly
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(L"bogus-endpoint", canon_w, 256u) == OS_XAUDIO2_E_INVALIDARG);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(L"{0.0.0.00000000}.{zzzzzzzz-0000-0000-0000-000000000000}", canon_w, 256u) == OS_XAUDIO2_E_INVALIDARG);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdW(L"\\\\?\\SWD#MMDEVAPI#not-a-guid", canon_w, 256u) == OS_XAUDIO2_E_INVALIDARG);
    OS_REQUIRE(OpenScarlettXAudio2ValidateEndpointIdA("invalid!!!", canon_a, 256u) == OS_XAUDIO2_E_INVALIDARG);

    // Live WASAPI endpoint probing across default, canonical, and SWD formats
    SetNullAudioEnv(false);
    OS_XAUDIO2_ENDPOINT_PROBE probe_def{};
    OS_REQUIRE(OpenScarlettXAudio2ProbeWasapiEndpointW(nullptr, &probe_def) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(probe_def.format_valid == 1u && probe_def.is_default == 1u);

    OS_XAUDIO2_ENDPOINT_PROBE probe_swd{};
    OS_REQUIRE(OpenScarlettXAudio2ProbeWasapiEndpointW(kSwdFullW, &probe_swd) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(probe_swd.format_valid == 1u && probe_swd.is_default == 0u);
    OS_REQUIRE(std::wcscmp(probe_swd.canonical_id, kCanonIdW) == 0);

    // If a real host render endpoint exists, re-probe using BOTH its real canonical ID
    // AND a synthesized \\?\SWD#MMDEVAPI#<real-id>#{e6327cad-...} WinRT path!
    if (probe_def.exact_device_found == 1u && probe_def.canonical_id[0] != L'\0') {
        OS_XAUDIO2_ENDPOINT_PROBE probe_real{};
        OS_REQUIRE(OpenScarlettXAudio2ProbeWasapiEndpointW(probe_def.canonical_id, &probe_real) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(probe_real.format_valid == 1u && probe_real.exact_device_found == 1u);
        OS_REQUIRE(probe_real.mix_sample_rate >= 8000u && probe_real.mix_channels >= 1u);

        const std::wstring swd_real =
            std::wstring(L"\\\\?\\SWD#MMDEVAPI#") + probe_def.canonical_id +
            L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
        OS_XAUDIO2_ENDPOINT_PROBE probe_swd_real{};
        OS_REQUIRE(OpenScarlettXAudio2ProbeWasapiEndpointW(swd_real.c_str(), &probe_swd_real) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(probe_swd_real.format_valid == 1u && probe_swd_real.exact_device_found == 1u);
        std::printf("  Live WASAPI host endpoint exact match OK (%uHz %uch)\n",
                    probe_real.mix_sample_rate, probe_real.mix_channels);
    }

    // CreateMasteringVoice accepts all valid endpoint formats and rejects malformed ones
    SetNullAudioEnv(true);
    IXAudio2* xa = nullptr;
    OS_REQUIRE(XAudio2Create(&xa, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

    IXAudio2MasteringVoice* bad_master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&bad_master, 2u, 48000u, 0u, L"malformed-id") == OS_XAUDIO2_E_INVALIDARG);
    OS_REQUIRE(bad_master == nullptr);

    for (const wchar_t* ep : {static_cast<const wchar_t*>(nullptr), L"", L"default", kCanonIdW, kSwdFullW, kBareGuidW}) {
        IXAudio2MasteringVoice* mv = nullptr;
        OS_REQUIRE(xa->CreateMasteringVoice(&mv, 2u, 48000u, 0u, ep) == OS_XAUDIO2_S_OK && mv != nullptr);
        mv->DestroyVoice();
    }

    // Verify Opus invalid sample rate rejection + valid Opus & IMA-ADPCM decoding through XAudio2
    {
        openscarlett::OpusMftDecoder bad_dec;
        openscarlett::OpusDecoderConfig bad_cfg{};
        bad_cfg.sample_rate = 44100u;  // Not a valid RFC 6716 Opus rate
        bad_cfg.channels = 2u;
        OS_REQUIRE(!bad_dec.Initialize(bad_cfg).ok());
    }

    IXAudio2MasteringVoice* mv = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&mv, 2u, 48000u, 0u, kSwdFullW) == OS_XAUDIO2_S_OK && mv != nullptr);

    // Decode Opus packet stream via XAudio2 source voice
    {
        std::vector<std::vector<uint8_t>> pkts;
        for (uint32_t i = 0u; i < 4u; ++i) {
            pkts.push_back(openscarlett::BuildOpusTestPacket(2u, i, 64u));
        }
        const auto stream = openscarlett::PackOpusLengthPrefixedStream(pkts, false);
        OS_OPUSWAVEFORMAT ofmt{};
        ofmt.wfx.wFormatTag = OS_WAVE_FORMAT_OPUS;
        ofmt.wfx.nChannels = 2u;
        ofmt.wfx.nSamplesPerSec = 48000u;
        ofmt.wfx.wBitsPerSample = 16u;
        ofmt.wfx.cbSize = sizeof(OS_OPUSWAVEFORMAT) - sizeof(OS_WAVEFORMATEX);
        ofmt.Version = 1u;

        IXAudio2SourceVoice* ov = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&ov, &ofmt.wfx) == OS_XAUDIO2_S_OK && ov != nullptr);
        OS_XAUDIO2_BUFFER obuf{};
        obuf.Flags = OS_XAUDIO2_END_OF_STREAM;
        obuf.AudioBytes = static_cast<uint32_t>(stream.size());
        obuf.pAudioData = stream.data();
        OS_REQUIRE(ov->SubmitSourceBuffer(&obuf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(ov->Start() == OS_XAUDIO2_S_OK);

        std::vector<float> mix(2048u * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), 2048u) == OS_XAUDIO2_S_OK);
        float peak = 0.0f;
        for (float s : mix) {
            OS_REQUIRE(std::isfinite(s));
            peak = std::max(peak, std::fabs(s));
        }
        OS_REQUIRE(peak > 0.001f);
        ov->DestroyVoice();
    }

    // Decode IMA-ADPCM via XAudio2 source voice
    {
        constexpr size_t kFrames = 505u * 4u;
        std::vector<float> src(kFrames);
        for (size_t i = 0u; i < kFrames; ++i) {
            src[i] = 0.5f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        }
        openscarlett::ImaAdpcmConfig icfg{};
        icfg.sample_rate = 48000u;
        icfg.channels = 1u;
        std::vector<uint8_t> blocks;
        uint16_t align = 0u, spb = 0u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       src.data(), kFrames, icfg, blocks, &align, &spb)
                       .ok());

        OS_IMAADPCMWAVEFORMAT afmt{};
        afmt.wfx.wFormatTag = OS_WAVE_FORMAT_IMA_ADPCM;
        afmt.wfx.nChannels = 1u;
        afmt.wfx.nSamplesPerSec = 48000u;
        afmt.wfx.nBlockAlign = align;
        afmt.wfx.wBitsPerSample = 4u;
        afmt.wfx.cbSize = 2u;
        afmt.wSamplesPerBlock = spb;

        IXAudio2SourceVoice* av = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&av, &afmt.wfx) == OS_XAUDIO2_S_OK && av != nullptr);
        OS_XAUDIO2_BUFFER abuf{};
        abuf.Flags = OS_XAUDIO2_END_OF_STREAM;
        abuf.AudioBytes = static_cast<uint32_t>(blocks.size());
        abuf.pAudioData = blocks.data();
        OS_REQUIRE(av->SubmitSourceBuffer(&abuf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(av->Start() == OS_XAUDIO2_S_OK);

        std::vector<float> mix(kFrames * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), static_cast<uint32_t>(kFrames)) ==
                   OS_XAUDIO2_S_OK);
        float peak = 0.0f;
        for (float s : mix) {
            OS_REQUIRE(std::isfinite(s));
            peak = std::max(peak, std::fabs(s));
        }
        OS_REQUIRE(peak > 0.1f);
        av->DestroyVoice();
    }

    mv->DestroyVoice();
    xa->Release();
    std::printf("  PASS: All WASAPI MMDevice endpoint formats + Opus/IMA-ADPCM decoding verified!\n");
}

}  // namespace

int main() {
    TestDirectStorageSubmittedCancelBoundsAndPhysicalPages();
    TestNewHleAndDiagnosticShims();
    TestXMemAndXThreadEdgeCases();
    TestWasapiEndpointFormatsAndCodecs();
    std::printf("ALL DSTORAGE / MFPLAT / DIAG SHIMS / XMEM / XTHREAD / WASAPI PARITY TESTS PASSED!\n");
    return 0;
}
