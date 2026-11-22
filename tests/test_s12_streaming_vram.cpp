#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <windows.h>

#  if defined(_MSC_VER)
#    define OS_UUIDOF(T) __uuidof(T)
#  elif defined(__CRT_UUID_DECL)
#    define OS_UUIDOF(T) __mingw_uuidof<T>()
#  else
#    define OS_UUIDOF(T) IID_##T
#  endif
#endif

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/kernel/title_va_mirror.h"
#include "openscarlett/kernel/vram_residency_manager.h"
#include "os_check.h"

namespace fs = std::filesystem;
using namespace openscarlett;

namespace {

void FillDeterministic(uint8_t* dst, size_t size, uint32_t seed) {
    uint32_t s = seed ^ 0x9E3779B9u;
    for (size_t i = 0u; i < size; ++i) {
        s = s * 1664525u + 1013904223u;
        dst[i] = static_cast<uint8_t>((s >> 24u) ^ ((i * 29u + seed) & 0xFFu));
    }
}

void WriteLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void WriteLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

fs::path GetRepoLocalTempDir() {
    std::error_code ec;
    wchar_t exe_buf[MAX_PATH]{};
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    fs::path base_dir =
        (exe_len > 0u && exe_len < MAX_PATH)
            ? fs::path(exe_buf).parent_path()
            : fs::path(OPENSCARLETT_BINARY_DIR);
    fs::path dir = base_dir / ".tmp" / "s12_streaming_vram";
    fs::create_directories(dir, ec);
    return dir;
}

// Custom XOR-keyed codec for testing DirectStorage custom decompression callbacks.
struct CustomXorContext {
    uint8_t key = 0x5Au;
    std::atomic<uint32_t> invoke_count{0u};
};

int32_t OPENSCARLETT_DSTORAGE_CALL CustomXorDecompressCallback(
    uint8_t format,
    const void* src,
    size_t src_size,
    void* dst,
    size_t dst_size,
    size_t* out_decompressed_size,
    void* user_context) {
    if (format != OS_DSTORAGE_CUSTOM_COMPRESSION_0 || !src || !dst || !out_decompressed_size || !user_context) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
    if (src_size < 4u) {
        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
    }
    const auto* in = static_cast<const uint8_t*>(src);
    if (in[0] != 'X' || in[1] != 'O' || in[2] != 'R' || in[3] != '1') {
        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
    }
    const size_t payload_len = src_size - 4u;
    if (payload_len > dst_size) {
        return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
    }
    auto* ctx = static_cast<CustomXorContext*>(user_context);
    auto* out = static_cast<uint8_t*>(dst);
    for (size_t i = 0u; i < payload_len; ++i) {
        out[i] = static_cast<uint8_t>(in[4u + i] ^ static_cast<uint8_t>(ctx->key + (i & 0x1Fu)));
    }
    *out_decompressed_size = payload_len;
    ctx->invoke_count.fetch_add(1u, std::memory_order_relaxed);
    return OS_DSTORAGE_S_OK;
}

std::vector<uint8_t> EncodeCustomXor(const std::vector<uint8_t>& plain, uint8_t key) {
    std::vector<uint8_t> out(4u + plain.size());
    out[0] = 'X';
    out[1] = 'O';
    out[2] = 'R';
    out[3] = '1';
    for (size_t i = 0u; i < plain.size(); ++i) {
        out[4u + i] = static_cast<uint8_t>(plain[i] ^ static_cast<uint8_t>(key + (i & 0x1Fu)));
    }
    return out;
}

#if defined(_WIN32)
struct D3D12TestBackend {
    std::string label;
    bool is_warp = false;
    ID3D12Device* device = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    D3D12AdapterInfo info{};

    ~D3D12TestBackend() {
        if (device) device->Release();
        if (adapter) adapter->Release();
    }
};

bool CreateTestBackend(bool prefer_warp, D3D12TestBackend& out) {
    D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = prefer_warp;
    opts.allow_warp_fallback = prefer_warp;
    opts.enable_debug_layer = false;
    Status st = SelectD3D12AdapterAndCreateDevice(opts, &out.device, &out.adapter, &out.info);
    if (!st.ok() || !out.device) {
        return false;
    }
    out.is_warp = out.info.is_warp || prefer_warp;
    out.label = out.is_warp ? ("WARP (" + out.info.description + ")")
                            : ("HW-GPU (" + out.info.description + ")");
    return true;
}

bool ReadbackTextureSubresource(
    ID3D12Device* dev,
    ID3D12Resource* tex,
    UINT subresource,
    D3D12_RESOURCE_STATES current_state,
    std::vector<uint8_t>& out_tight_linear) {
    out_tight_linear.clear();
    const D3D12_RESOURCE_DESC rdesc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0u;
    UINT64 row_size = 0ull;
    UINT64 total_bytes = 0ull;
    dev->GetCopyableFootprints(&rdesc, subresource, 1u, 0ull, &fp, &num_rows, &row_size, &total_bytes);
    if (num_rows == 0u || row_size == 0ull || total_bytes == 0ull) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = std::max<uint64_t>(total_bytes, 256ull);
    rb_desc.Height = 1u;
    rb_desc.DepthOrArraySize = 1u;
    rb_desc.MipLevels = 1u;
    rb_desc.SampleDesc.Count = 1u;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* rb = nullptr;
    if (FAILED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, OS_UUIDOF(ID3D12Resource), reinterpret_cast<void**>(&rb))) || !rb) {
        return false;
    }

    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool ok = ev &&
              SUCCEEDED(dev->CreateCommandQueue(&qd, OS_UUIDOF(ID3D12CommandQueue), reinterpret_cast<void**>(&q))) &&
              SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_UUIDOF(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, OS_UUIDOF(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd))) &&
              SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence), reinterpret_cast<void**>(&fence)));

    if (ok) {
        if (current_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER b0{};
            b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b0.Transition = {tex, subresource, current_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
            cmd->ResourceBarrier(1u, &b0);
        }
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = rb;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = fp;

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = tex;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = subresource;

        cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);

        if (current_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER b1{};
            b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b1.Transition = {tex, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, current_state};
            cmd->ResourceBarrier(1u, &b1);
        }
        ok = SUCCEEDED(cmd->Close());
        if (ok) {
            ID3D12CommandList* lists[] = {cmd};
            q->ExecuteCommandLists(1u, lists);
            q->Signal(fence, 1ull);
            if (fence->GetCompletedValue() < 1ull) {
                fence->SetEventOnCompletion(1ull, ev);
                ok = (WaitForSingleObject(ev, 15000u) == WAIT_OBJECT_0);
            }
        }
    }

    if (ok) {
        const void* mapped = nullptr;
        const D3D12_RANGE rr{0, static_cast<SIZE_T>(total_bytes)};
        if (SUCCEEDED(rb->Map(0u, &rr, const_cast<void**>(&mapped))) && mapped) {
            out_tight_linear.resize(static_cast<size_t>(row_size) * num_rows);
            const auto* src_base = static_cast<const uint8_t*>(mapped) + fp.Offset;
            for (UINT r = 0u; r < num_rows; ++r) {
                std::memcpy(
                    out_tight_linear.data() + static_cast<size_t>(r) * static_cast<size_t>(row_size),
                    src_base + static_cast<size_t>(r) * fp.Footprint.RowPitch,
                    static_cast<size_t>(row_size));
            }
            const D3D12_RANGE wr{0, 0};
            rb->Unmap(0u, &wr);
        } else {
            ok = false;
        }
    }

    if (ev) CloseHandle(ev);
    if (fence) fence->Release();
    if (cmd) cmd->Release();
    if (alloc) alloc->Release();
    if (q) q->Release();
    rb->Release();
    return ok;
}

// ============================================================================
// Test 1: DirectStorage Priority Queues, Auto-Submit, Strict Capacity,
//         Cancelled-Request Filtering, and Custom Decompression Callbacks
// ============================================================================
void TestDirectStorageStreamingHardening() {
    std::printf("[Test 1] DirectStorage Priority Queues, Auto-Submit, Capacity, Cancel & Custom Decomp...\n");

    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    // 1A. Priority queue scheduling: REALTIME beats NORMAL & LOW under concurrent heavy load
    {
        std::vector<uint8_t> heavy_plain(128u * 1024u);
        FillDeterministic(heavy_plain.data(), heavy_plain.size(), 0x5120u);
        std::vector<uint8_t> heavy_comp;
        OS_REQUIRE(GDeflateCompress(heavy_plain.data(), heavy_plain.size(), heavy_comp));

        auto make_queue = [&](DSTORAGE_PRIORITY prio, const char* name) {
            DSTORAGE_QUEUE_DESC qd{};
            qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
            qd.Priority = prio;
            qd.Name = name;
            IDStorageQueueX* q = nullptr;
            OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
            return q;
        };

        IDStorageQueueX* q_low = make_queue(DSTORAGE_PRIORITY_LOW, "prio_low");
        IDStorageQueueX* q_norm = make_queue(DSTORAGE_PRIORITY_NORMAL, "prio_normal");
        IDStorageQueueX* q_high = make_queue(DSTORAGE_PRIORITY_HIGH, "prio_high");
        IDStorageQueueX* q_rt = make_queue(DSTORAGE_PRIORITY_REALTIME, "prio_realtime");

        constexpr uint32_t kNormCount = 40u;
        constexpr uint32_t kLowCount = 16u;
        constexpr uint32_t kHighCount = 8u;
        constexpr uint32_t kRtCount = 8u;

        std::vector<std::vector<uint8_t>> low_dst(kLowCount, std::vector<uint8_t>(heavy_plain.size(), 0u));
        std::vector<std::vector<uint8_t>> norm_dst(kNormCount, std::vector<uint8_t>(heavy_plain.size(), 0u));
        std::vector<std::vector<uint8_t>> high_dst(kHighCount, std::vector<uint8_t>(heavy_plain.size(), 0u));
        std::vector<std::vector<uint8_t>> rt_dst(kRtCount, std::vector<uint8_t>(heavy_plain.size(), 0u));

        auto enqueue_batch = [&](IDStorageQueueX* q, std::vector<std::vector<uint8_t>>& dsts) {
            for (auto& d : dsts) {
                DSTORAGE_REQUEST r{};
                r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
                r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
                r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
                r.Source.Memory.Source = heavy_comp.data();
                r.Source.Memory.Size = static_cast<uint32_t>(heavy_comp.size());
                r.Destination.Memory.Buffer = d.data();
                r.Destination.Memory.Size = static_cast<uint32_t>(d.size());
                r.UncompressedSize = static_cast<uint32_t>(heavy_plain.size());
                q->EnqueueRequest(&r);
            }
        };

        HANDLE ev_low = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE ev_norm = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE ev_high = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE ev_rt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev_low && ev_norm && ev_high && ev_rt);

        enqueue_batch(q_low, low_dst);
        q_low->EnqueueSetEvent(ev_low);

        enqueue_batch(q_norm, norm_dst);
        q_norm->EnqueueSetEvent(ev_norm);

        enqueue_batch(q_high, high_dst);
        q_high->EnqueueSetEvent(ev_high);

        enqueue_batch(q_rt, rt_dst);
        q_rt->EnqueueSetEvent(ev_rt);

        // Submit LOW & NORMAL first, immediately followed by HIGH & REALTIME!
        q_low->Submit();
        q_norm->Submit();
        q_high->Submit();
        q_rt->Submit();

        // REALTIME must complete BEFORE NORMAL and LOW finish!
        OS_REQUIRE(WaitForSingleObject(ev_rt, 10000u) == WAIT_OBJECT_0);
        const bool norm_still_running = (WaitForSingleObject(ev_norm, 0u) == WAIT_TIMEOUT);
        const bool low_still_running = (WaitForSingleObject(ev_low, 0u) == WAIT_TIMEOUT);
        OS_REQUIRE(norm_still_running);
        OS_REQUIRE(low_still_running);

        OS_REQUIRE(WaitForSingleObject(ev_high, 10000u) == WAIT_OBJECT_0);
        OS_REQUIRE(WaitForSingleObject(ev_norm, 15000u) == WAIT_OBJECT_0);
        OS_REQUIRE(WaitForSingleObject(ev_low, 15000u) == WAIT_OBJECT_0);

        for (const auto& d : rt_dst) OS_REQUIRE(d == heavy_plain);
        for (const auto& d : high_dst) OS_REQUIRE(d == heavy_plain);
        for (const auto& d : norm_dst) OS_REQUIRE(d == heavy_plain);
        for (const auto& d : low_dst) OS_REQUIRE(d == heavy_plain);

        OpenScarlettDStoragePriorityStats pstats{};
        OS_REQUIRE(OpenScarlettDStorageGetPriorityStats(&pstats) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(pstats.realtime_dispatched >= kRtCount);
        OS_REQUIRE(pstats.high_dispatched >= kHighCount);
        OS_REQUIRE(pstats.normal_dispatched >= kNormCount);
        OS_REQUIRE(pstats.low_dispatched >= kLowCount);

        CloseHandle(ev_rt);
        CloseHandle(ev_high);
        CloseHandle(ev_norm);
        CloseHandle(ev_low);
        q_rt->Close(); q_rt->Release();
        q_high->Close(); q_high->Release();
        q_norm->Close(); q_norm->Release();
        q_low->Close(); q_low->Release();
        std::printf("  [PASS] Priority queues: REALTIME preempted NORMAL/LOW (RT=%llu HI=%llu NORM=%llu LOW=%llu)\n",
                    static_cast<unsigned long long>(pstats.realtime_dispatched),
                    static_cast<unsigned long long>(pstats.high_dispatched),
                    static_cast<unsigned long long>(pstats.normal_dispatched),
                    static_cast<unsigned long long>(pstats.low_dispatched));
    }

    // 1B. Half-full auto-submit + strict capacity + cancelled request never poisoning RetrieveErrorRecord
    {
        DSTORAGE_QUEUE_DESC qd{};
        qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;  // 128 -> auto-submit at 64
        qd.Priority = DSTORAGE_PRIORITY_HIGH;
        qd.Name = "s12_autosubmit_cancel";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(2u, "s12_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);

        constexpr uint32_t kAuto = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;  // 64
        std::vector<std::array<uint8_t, 64>> src(kAuto);
        std::vector<std::array<uint8_t, 64>> dst(kAuto);
        for (uint32_t i = 0u; i < kAuto; ++i) {
            FillDeterministic(src[i].data(), 64u, 0x700u + i);
            dst[i].fill(0u);
        }

        // Enqueue 60 good + 3 malformed (tagged 0xBAD0xxxx) + status + event, cancel 0xBAD0xxxx,
        // then enqueue 64th request WITHOUT Submit() -> must auto-submit cleanly with 0 errors!
        for (uint32_t i = 0u; i < 60u; ++i) {
            DSTORAGE_REQUEST r{};
            r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.Memory.Source = src[i].data();
            r.Source.Memory.Size = 64u;
            r.Destination.Memory.Buffer = dst[i].data();
            r.Destination.Memory.Size = 64u;
            r.UncompressedSize = 64u;
            r.CancellationTag = 0x10000000ull | i;
            q->EnqueueRequest(&r);
        }
        for (uint32_t i = 0u; i < 3u; ++i) {
            DSTORAGE_REQUEST bad{};
            bad.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
            bad.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            bad.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            bad.Source.Memory.Source = nullptr;
            bad.Source.Memory.Size = 32u;
            bad.Destination.Memory.Buffer = nullptr;
            bad.Destination.Memory.Size = 0u;
            bad.CancellationTag = 0xBAD00000ull | i;
            q->EnqueueRequest(&bad);
        }
        q->CancelRequestsWithTag(0xFFFF0000ull, 0xBAD00000ull);
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);

        // 64th request triggers automatic submission without Submit()
        DSTORAGE_REQUEST r64{};
        r64.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r64.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r64.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r64.Source.Memory.Source = src[63].data();
        r64.Source.Memory.Size = 64u;
        r64.Destination.Memory.Buffer = dst[63].data();
        r64.Destination.Memory.Size = 64u;
        r64.UncompressedSize = 64u;
        q->EnqueueRequest(&r64);

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(WaitForSingleObject(static_cast<HANDLE>(q->GetErrorEvent()), 0u) == WAIT_TIMEOUT);

        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 0u);
        OS_REQUIRE(err.FirstFailure.HResult == OS_DSTORAGE_S_OK);

        for (uint32_t i = 0u; i < 60u; ++i) {
            OS_REQUIRE(dst[i] == src[i]);
        }

        // Strict capacity stress: push 384 requests (3x capacity) into 128-slot queue
        constexpr uint32_t kBurst = 384u;
        std::vector<std::array<uint8_t, 32>> bsrc(kBurst);
        std::vector<std::array<uint8_t, 32>> bdst(kBurst);
        for (uint32_t i = 0u; i < kBurst; ++i) {
            FillDeterministic(bsrc[i].data(), 32u, 0x9000u + i);
            bdst[i].fill(0u);
            DSTORAGE_REQUEST br{};
            br.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            br.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            br.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            br.Source.Memory.Source = bsrc[i].data();
            br.Source.Memory.Size = 32u;
            br.Destination.Memory.Buffer = bdst[i].data();
            br.Destination.Memory.Size = 32u;
            br.UncompressedSize = 32u;
            q->EnqueueRequest(&br);
        }
        ResetEvent(ev);
        q->EnqueueStatus(st, 1u);
        q->EnqueueSetEvent(ev);
        q->Submit();
        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(1u) && st->GetHResult(1u) == OS_DSTORAGE_S_OK);
        for (uint32_t i = 0u; i < kBurst; ++i) {
            OS_REQUIRE(bdst[i] == bsrc[i]);
        }

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        std::printf("  [PASS] Auto-submit (without Submit), 3x capacity backpressure & cancel error-record isolation\n");
    }

    // 1C. Custom decompression callback (file-backed via repo-local .tmp + memory-backed)
    {
        CustomXorContext custom_ctx{};
        custom_ctx.key = 0xA7u;
        OS_REQUIRE(OpenScarlettDStorageRegisterCustomDecompressor(
                       OS_DSTORAGE_CUSTOM_COMPRESSION_0,
                       &CustomXorDecompressCallback,
                       &custom_ctx) == OS_DSTORAGE_S_OK);

        std::vector<uint8_t> plain(16384u);
        FillDeterministic(plain.data(), plain.size(), 0xC0DEu);
        const std::vector<uint8_t> encoded = EncodeCustomXor(plain, custom_ctx.key);

        const fs::path tmp_file = GetRepoLocalTempDir() / "custom_decomp_stream.bin";
        {
            std::ofstream ofs(tmp_file, std::ios::binary | std::ios::trunc);
            OS_REQUIRE(ofs.good());
            ofs.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        }

        IDStorageFileX* file = nullptr;
        const std::wstring wpath = tmp_file.wstring();
        OS_REQUIRE(factory->OpenFile(wpath.c_str(), &IID_IDStorageFileX, reinterpret_cast<void**>(&file)) == OS_DSTORAGE_S_OK);

        DSTORAGE_QUEUE_DESC qd{};
        qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qd.Priority = DSTORAGE_PRIORITY_REALTIME;
        qd.Name = "custom_decomp_q";
        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);

        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(1u, "cust_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);

        std::vector<uint8_t> dst(plain.size(), 0u);
        DSTORAGE_REQUEST r{};
        r.Options.CompressionFormat = static_cast<DSTORAGE_COMPRESSION_FORMAT>(OS_DSTORAGE_CUSTOM_COMPRESSION_0);
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r.Source.File.Source = file;
        r.Source.File.Offset = 0ull;
        r.Source.File.Size = static_cast<uint32_t>(encoded.size());
        r.Destination.Memory.Buffer = dst.data();
        r.Destination.Memory.Size = static_cast<uint32_t>(dst.size());
        r.UncompressedSize = static_cast<uint32_t>(plain.size());
        q->EnqueueRequest(&r);
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(dst == plain);
        OS_REQUIRE(custom_ctx.invoke_count.load() == 1u);

        OS_REQUIRE(OpenScarlettDStorageUnregisterCustomDecompressor(OS_DSTORAGE_CUSTOM_COMPRESSION_0) == OS_DSTORAGE_S_OK);

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        file->Close();
        file->Release();
        std::error_code ec;
        fs::remove(tmp_file, ec);
        std::printf("  [PASS] Custom decompression callback (file-backed NVMe stream, 16 KiB bit-exact)\n");
    }

    factory->Release();
}

// ============================================================================
// Test 2: End-to-End DirectStorage BCPack -> XgGpuUntiler -> Live D3D12 Texture
//         Upload & Readback (Verified on BOTH Host Hardware GPU and WARP!)
// ============================================================================
void TestDirectStorageBcPackToGpuUntilerTexture(const D3D12TestBackend& backend) {
    std::printf("[Test 2] DirectStorage BCPack -> XgGpuUntiler -> Live D3D12 Texture [%s]...\n",
                backend.label.c_str());

    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);

    // 256x256 BC7 = 64x64 blocks * 16 B = 65,536 bytes == exact 1x1 64KB GFX10 macro-tile!
    constexpr uint32_t kW = 256u;
    constexpr uint32_t kH = 256u;
    std::vector<uint8_t> linear_bc7;
    OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC7, kW, kH, 0xBC70u, linear_bc7).ok());
    OS_REQUIRE(linear_bc7.size() == 65536u);

    XG_TEXTURE2D_DESC xg_desc{};
    xg_desc.Width = kW;
    xg_desc.Height = kH;
    xg_desc.MipLevels = 1u;
    xg_desc.ArraySize = 1u;
    xg_desc.Format = DXGI_FORMAT_BC7_UNORM;
    xg_desc.SampleCount = 1u;
    xg_desc.BindFlags = XG_BIND_SHADER_RESOURCE;
    xg_desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S_X;

    XG_RESOURCE_LAYOUT xg_layout{};
    OS_REQUIRE(xg::ComputeTexture2DLayout(xg_desc, xg_layout));
    OS_REQUIRE(xg_layout.SizeBytes == 65536ull);

    // Swizzle BC7 blocks into Xbox GFX10 64KB_S_X tiled layout
    std::vector<uint8_t> tiled_bc7(static_cast<size_t>(xg_layout.SizeBytes), 0u);
    const uint32_t linear_row_pitch = (kW / 4u) * 16u;
    OS_REQUIRE(xg::TileSurface2D(xg_desc, 0u, 0u, linear_bc7.data(), linear_row_pitch,
                                 tiled_bc7.data(), tiled_bc7.size()));
    OS_REQUIRE(tiled_bc7 != linear_bc7);  // genuinely swizzled

    // Compress the GFX10-tiled BC7 surface with BCPack and persist to repo-local temp file
    std::vector<uint8_t> bcpack_stream;
    OS_REQUIRE(BcPackCompress(BcFormat::BC7, kW, kH, tiled_bc7.data(), tiled_bc7.size(), bcpack_stream).ok());

    const fs::path tmp_file = GetRepoLocalTempDir() /
                              (backend.is_warp ? "bc7_tiled_warp.bcpk" : "bc7_tiled_hw.bcpk");
    {
        std::ofstream ofs(tmp_file, std::ios::binary | std::ios::trunc);
        OS_REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(bcpack_stream.data()),
                  static_cast<std::streamsize>(bcpack_stream.size()));
    }

    // Create target D3D12 DEFAULT heap BC7 texture
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW;
    td.Height = kH;
    td.DepthOrArraySize = 1u;
    td.MipLevels = 1u;
    td.Format = DXGI_FORMAT_BC7_UNORM;
    td.SampleDesc.Count = 1u;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* gpu_tex = nullptr;
    OS_REQUIRE(SUCCEEDED(backend.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON,
        nullptr, OS_UUIDOF(ID3D12Resource), reinterpret_cast<void**>(&gpu_tex))));
    OS_REQUIRE(gpu_tex != nullptr);

    IDStorageFileX* ds_file = nullptr;
    const std::wstring wpath = tmp_file.wstring();
    OS_REQUIRE(factory->OpenFile(wpath.c_str(), &IID_IDStorageFileX,
                                 reinterpret_cast<void**>(&ds_file)) == OS_DSTORAGE_S_OK);

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_REALTIME;
    qd.Name = "bcpack_gpu_tex_q";
    qd.Device = backend.device;

    IDStorageQueueX* q = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);

    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(1u, "tex_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OS_REQUIRE(ev != nullptr);

    OpenScarlettDStorageTextureDestDesc tex_dest{};
    tex_dest.d3d12_resource = gpu_tex;
    tex_dest.d3d12_device = backend.device;
    tex_dest.subresource_index = 0u;
    tex_dest.width = kW;
    tex_dest.height = kH;
    tex_dest.dxgi_format = DXGI_FORMAT_BC7_UNORM;
    tex_dest.src_tile_mode = XG_TILE_MODE_2D_THIN_64KB_S_X;
    tex_dest.initial_resource_state = D3D12_RESOURCE_STATE_COMMON;
    tex_dest.final_resource_state = D3D12_RESOURCE_STATE_COMMON;
    tex_dest.prefer_gpu_untiler = 1u;
    tex_dest.allow_cpu_untile_fallback = 1u;

    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_BCPACK;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
    req.Source.File.Source = ds_file;
    req.Source.File.Offset = 0ull;
    req.Source.File.Size = static_cast<uint32_t>(bcpack_stream.size());
    req.Destination.Memory.Buffer = &tex_dest;
    req.Destination.Memory.Size = sizeof(tex_dest);
    req.UncompressedSize = static_cast<uint32_t>(tiled_bc7.size());

    q->EnqueueRequest(&req);
    q->EnqueueStatus(st, 0u);
    q->EnqueueSetEvent(ev);
    q->Submit();

    OS_REQUIRE(WaitForSingleObject(ev, 20000u) == WAIT_OBJECT_0);
    OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(tex_dest.out_decompressed_bytes == 65536u);
    OS_REQUIRE(tex_dest.out_uploaded_bytes == 65536u);

    // Read back the live GPU BC7 texture and assert 100% bit-exact match with original linear BC7!
    std::vector<uint8_t> gpu_readback;
    OS_REQUIRE(ReadbackTextureSubresource(
        backend.device, gpu_tex, 0u, D3D12_RESOURCE_STATE_COMMON, gpu_readback));
    OS_REQUIRE(gpu_readback == linear_bc7);

    CloseHandle(ev);
    st->Release();
    q->Close();
    q->Release();
    ds_file->Close();
    ds_file->Release();
    gpu_tex->Release();
    factory->Release();
    std::error_code ec;
    fs::remove(tmp_file, ec);

    std::printf("  [PASS] File -> BCPack -> %s -> D3D12 BC7 Texture -> GPU Readback: 65,536/65,536 bytes BIT-EXACT\n",
                tex_dest.out_used_gpu_untiler ? "XgGpuUntiler(GPU)" : "XG-CPU-Fallback");
}
#endif

// ============================================================================
// Test 3: BCPack BC1-BC7 Multi-Mip + Texture Array + Malformed Input Fuzz Suite
// ============================================================================
void TestBcPackHardeningArraysAndFuzz() {
    std::printf("[Test 3] BCPack BC1-BC7 Multi-Mip/Texture-Array & Adversarial Fuzz Suite...\n");

    const std::array<BcFormat, 7> all_formats = {
        BcFormat::BC1, BcFormat::BC2, BcFormat::BC3,
        BcFormat::BC4, BcFormat::BC5, BcFormat::BC6H, BcFormat::BC7,
    };

    // 3A. Texture Array + Multi-Mip round-trip across ALL 7 formats BC1..BC7 (NPOT 37x29, 4 slices, 6 mips)
    constexpr uint32_t kBaseW = 37u;
    constexpr uint32_t kBaseH = 29u;
    constexpr uint32_t kSlices = 4u;
    const uint32_t kMips = BcCalculateMaxMipLevels(kBaseW, kBaseH);
    OS_REQUIRE(kMips == 6u);  // 37x29 -> 18x14 -> 9x7 -> 4x3 -> 2x1 -> 1x1

    std::vector<uint8_t> sample_array_stream;
    for (BcFormat fmt : all_formats) {
        std::vector<std::vector<uint8_t>> subresources(static_cast<size_t>(kSlices) * kMips);
        for (uint32_t s = 0u; s < kSlices; ++s) {
            for (uint32_t m = 0u; m < kMips; ++m) {
                BcPackMipLevelInfo info{};
                OS_REQUIRE(BcCalculateMipLevelInfo(fmt, kBaseW, kBaseH, m, info));
                const size_t idx = static_cast<size_t>(s) * kMips + m;
                OS_REQUIRE(BcGenerateSyntheticSurface(
                               fmt, info.width, info.height,
                               static_cast<uint32_t>(fmt) * 1000u + s * 37u + m,
                               subresources[idx])
                               .ok());
                OS_REQUIRE(subresources[idx].size() == info.byte_size);
            }
        }

        std::vector<uint8_t> compressed;
        OS_REQUIRE(BcPackCompressTextureArray(fmt, kBaseW, kBaseH, kMips, kSlices, subresources, compressed).ok());
        OS_REQUIRE(BcPackValidateStream(compressed.data(), compressed.size()).ok());

        std::vector<std::vector<uint8_t>> decoded;
        BcPackTextureArrayHeader hdr{};
        OS_REQUIRE(BcPackDecompressTextureArray(compressed.data(), compressed.size(), decoded, &hdr).ok());
        OS_REQUIRE(hdr.format == fmt && hdr.base_width == kBaseW && hdr.base_height == kBaseH);
        OS_REQUIRE(hdr.mip_count == kMips && hdr.array_size == kSlices);
        OS_REQUIRE(decoded == subresources);

        // Single-subresource random-access decode (e.g., slice 3, tail mip 5)
        std::vector<uint8_t> tail_sub;
        OS_REQUIRE(BcPackDecompressTextureArraySubresource(compressed.data(), compressed.size(), 3u, 5u, tail_sub).ok());
        OS_REQUIRE(tail_sub == subresources[3u * kMips + 5u]);
        // Out-of-range slice/mip rejected cleanly
        OS_REQUIRE(!BcPackDecompressTextureArraySubresource(compressed.data(), compressed.size(), kSlices, 0u, tail_sub).ok());
        OS_REQUIRE(!BcPackDecompressTextureArraySubresource(compressed.data(), compressed.size(), 0u, kMips, tail_sub).ok());

        if (fmt == BcFormat::BC7) {
            sample_array_stream = compressed;
        }
    }

    // 3B. Targeted malformed header & predictor/codec attacks
    std::vector<uint8_t> valid_bc7, valid_bc7_packed;
    OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC7, 16u, 16u, 123u, valid_bc7).ok());
    OS_REQUIRE(BcPackCompress(BcFormat::BC7, 16u, 16u, valid_bc7.data(), valid_bc7.size(), valid_bc7_packed).ok());
    OS_REQUIRE(BcPackValidateStream(valid_bc7_packed.data(), valid_bc7_packed.size()).ok());

    std::vector<uint8_t> dummy_out;
    std::vector<std::vector<uint8_t>> dummy_vec;

    // Null / empty / tiny inputs
    OS_REQUIRE(!BcPackValidateStream(nullptr, 0u).ok());
    OS_REQUIRE(!BcPackDecompress(nullptr, 0u, dummy_out).ok());
    OS_REQUIRE(!BcPackDecompressMipChain(nullptr, 0u, dummy_vec).ok());
    OS_REQUIRE(!BcPackDecompressTextureArray(nullptr, 0u, dummy_vec).ok());

    // Malformed texture array header mutations
    auto check_array_reject = [&](auto mutator) {
        std::vector<uint8_t> bad = sample_array_stream;
        mutator(bad);
        OS_REQUIRE(!BcPackDecompressTextureArray(bad.data(), bad.size(), dummy_vec).ok());
    };
    check_array_reject([](auto& b) { b[0] ^= 0xFFu; });                              // bad magic
    check_array_reject([](auto& b) { WriteLe16(b.data() + 4, 99u); });               // bad version
    check_array_reject([](auto& b) { WriteLe16(b.data() + 6, 1u); });                // non-zero flags
    check_array_reject([](auto& b) { WriteLe32(b.data() + 8, 99u); });               // invalid format
    check_array_reject([](auto& b) { WriteLe32(b.data() + 12, 0u); });               // zero width
    check_array_reject([](auto& b) { WriteLe32(b.data() + 16, 0x7FFFFFFFu); });      // huge height
    check_array_reject([](auto& b) { WriteLe32(b.data() + 20, 0u); });               // zero mips
    check_array_reject([](auto& b) { WriteLe32(b.data() + 20, 32u); });              // excessive mips
    check_array_reject([](auto& b) { WriteLe32(b.data() + 24, 0u); });               // zero array_size
    check_array_reject([](auto& b) { WriteLe32(b.data() + 24, 9999u); });            // excessive array_size
    check_array_reject([](auto& b) { WriteLe32(b.data() + 28, 4u); });               // offset inside header
    check_array_reject([](auto& b) {                                                  // overlapping subresources
        const uint32_t off0 = static_cast<uint32_t>(b[28]) |
                              (static_cast<uint32_t>(b[29]) << 8u) |
                              (static_cast<uint32_t>(b[30]) << 16u) |
                              (static_cast<uint32_t>(b[31]) << 24u);
        WriteLe32(b.data() + 36, off0);
    });

    // Truncated array stream
    for (size_t t = 0u; t < std::min<size_t>(sample_array_stream.size(), 96u); ++t) {
        OS_REQUIRE(!BcPackDecompressTextureArray(sample_array_stream.data(), t, dummy_vec).ok());
    }

    // 3C. 500 deterministic adversarial / random-mutation fuzz cases (zero UB, zero crashes!)
    uint32_t rng = 0xF0221234u;
    auto next_rng = [&]() -> uint32_t {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    uint32_t fuzz_rejected = 0u;
    for (uint32_t iter = 0u; iter < 500u; ++iter) {
        std::vector<uint8_t> fuzz;
        const uint32_t mode = iter % 4u;
        if (mode == 0u) {
            // Mutated BCPK stream
            fuzz = valid_bc7_packed;
            const uint32_t flips = 1u + (next_rng() % 6u);
            for (uint32_t f = 0u; f < flips; ++f) {
                fuzz[next_rng() % fuzz.size()] ^= static_cast<uint8_t>((next_rng() & 0xFEu) | 1u);
            }
            if (!BcPackDecompress(fuzz.data(), fuzz.size(), dummy_out).ok()) {
                ++fuzz_rejected;
            }
        } else if (mode == 1u) {
            // Mutated BCPA texture-array stream
            fuzz = sample_array_stream;
            const uint32_t flips = 1u + (next_rng() % 5u);
            for (uint32_t f = 0u; f < flips; ++f) {
                fuzz[next_rng() % fuzz.size()] ^= static_cast<uint8_t>((next_rng() & 0xFEu) | 1u);
            }
            if (!BcPackDecompressTextureArray(fuzz.data(), fuzz.size(), dummy_vec).ok()) {
                ++fuzz_rejected;
            }
        } else if (mode == 2u) {
            // Pure random garbage prefixed with valid BCPK/BCPM/BCPA magic
            const size_t len = 4u + (next_rng() % 256u);
            fuzz.resize(len);
            for (size_t i = 0u; i < len; ++i) fuzz[i] = static_cast<uint8_t>(next_rng() >> 24u);
            const uint32_t magics[3] = {kBcPackMagic, kBcPackMipChainMagic, kBcPackTextureArrayMagic};
            WriteLe32(fuzz.data(), magics[iter % 3u]);
            const bool ok1 = BcPackDecompress(fuzz.data(), fuzz.size(), dummy_out).ok();
            const bool ok2 = BcPackDecompressMipChain(fuzz.data(), fuzz.size(), dummy_vec).ok();
            const bool ok3 = BcPackDecompressTextureArray(fuzz.data(), fuzz.size(), dummy_vec).ok();
            OS_REQUIRE(!ok1 && !ok2 && !ok3);
            ++fuzz_rejected;
        } else {
            // Valid header + corrupted predictor/codec/dimensions/truncation
            fuzz = valid_bc7_packed;
            const size_t cut = 1u + (next_rng() % (fuzz.size() - 1u));
            fuzz.resize(cut);
            OS_REQUIRE(!BcPackDecompress(fuzz.data(), fuzz.size(), dummy_out).ok());
            ++fuzz_rejected;
        }
    }
    OS_REQUIRE(fuzz_rejected == 500u);
    std::printf("  [PASS] BC1-BC7 4-slice x 6-mip NPOT arrays + 500/500 adversarial fuzz inputs cleanly rejected\n");
}

#if defined(_WIN32)
// ============================================================================
// Test 4: HTILE / CMASK / DCC Fast-Clear Metadata Coherency Helpers
//         (Verified on BOTH Host Hardware GPU and D3D12 WARP!)
// ============================================================================
void TestFastClearMetadataCoherency(const D3D12TestBackend& backend) {
    std::printf("[Test 4] HTILE / CMASK / DCC Fast-Clear Metadata Coherency [%s]...\n",
                backend.label.c_str());

    XgGpuUntiler untiler;
    const bool gpu_ready = untiler.InitializeD3D12WithDevice(backend.device).ok();
    XgGpuUntiler* untiler_ptr = gpu_ready ? &untiler : nullptr;

    constexpr uint32_t kW = 64u;
    constexpr uint32_t kH = 64u;
    const XgFastClearRect clear_rect{0u, 0u, 32u, 32u};  // top-left quadrant fast-cleared

    // 4A. DCC pipe-aligned fast-clear coherency (RGBA8)
    {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = kW;
        desc.Height = kH;
        desc.MipLevels = 1u;
        desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleCount = 1u;
        desc.BindFlags = XG_BIND_RENDER_TARGET | XG_BIND_SHADER_RESOURCE;
        desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S;

        const uint32_t row_pitch = kW * 4u;
        std::vector<uint8_t> src_linear(static_cast<size_t>(row_pitch) * kH);
        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                // Deliberately put garbage 0xDEADBEEF inside fast-clear rect to prove metadata overrides it!
                const uint32_t px = (x < 32u && y < 32u)
                                        ? 0xDEADBEEFu
                                        : (0xFF000000u | (x << 8u) | y);
                std::memcpy(src_linear.data() + (y * kW + x) * 4u, &px, 4u);
            }
        }

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(desc, layout));
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0u);
        OS_REQUIRE(xg::TileSurface2D(desc, 0u, 0u, src_linear.data(), row_pitch, tiled.data(), tiled.size()));

        const float clear_rgba[4] = {0.25f, 0.50f, 0.75f, 1.0f};
        std::array<uint32_t, 4> clear_reg{};
        EncodeFormatFastClearRegister(desc.Format, clear_rgba, 0.0f, 0u, clear_reg);

        std::vector<uint8_t> dcc_meta;
        OS_REQUIRE(BuildDccFastClearMetadata(
                       desc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, 0u, 0u,
                       true, XG_DCC_STATE_CLEAR_REG, clear_rect, dcc_meta)
                       .ok());

        XgGpuSurface2DRequest req{};
        req.desc = desc;
        req.direction = XgGpuDirection::Untile;
        req.metadata_mode = XgGpuMetadataMode::Dcc;
        req.console_type = XG_CONSOLE_TYPE_SCARLETT_ANACONDA;
        req.dcc_pipe_aligned = 1u;
        req.clear_reg_value = clear_reg;

        std::vector<uint8_t> out_linear;
        XgFastClearMetadataSyncStats stats{};
        OS_REQUIRE(ExpandAndSyncFastClearMetadata2D(
                       untiler_ptr, req, tiled, dcc_meta, out_linear, true, false, &stats)
                       .ok());
        OS_REQUIRE(stats.fast_cleared_blocks_expanded > 0u);
        OS_REQUIRE(stats.uncompressed_blocks_preserved > 0u);

        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                uint32_t actual = 0u;
                std::memcpy(&actual, out_linear.data() + (y * kW + x) * 4u, 4u);
                const uint32_t expected = (x < 32u && y < 32u)
                                              ? clear_reg[0]
                                              : (0xFF000000u | (x << 8u) | y);
                OS_REQUIRE(actual == expected);
            }
        }
        // Metadata must now be reset to expanded (0xFF)
        for (uint8_t b : dcc_meta) OS_REQUIRE(b == 0xFFu);
    }

    // 4B. HTILE fast depth-clear coherency (D32_FLOAT)
    {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = kW;
        desc.Height = kH;
        desc.MipLevels = 1u;
        desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleCount = 1u;
        desc.BindFlags = XG_BIND_DEPTH_STENCIL;
        desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_Z;

        const uint32_t row_pitch = kW * 4u;
        std::vector<uint8_t> src_linear(static_cast<size_t>(row_pitch) * kH);
        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                const float z = (x < 32u && y < 32u) ? -999.0f : (0.1f + 0.001f * static_cast<float>(x + y));
                std::memcpy(src_linear.data() + (y * kW + x) * 4u, &z, 4u);
            }
        }

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(desc, layout));
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0u);
        OS_REQUIRE(xg::TileSurface2D(desc, 0u, 0u, src_linear.data(), row_pitch, tiled.data(), tiled.size()));

        std::vector<uint8_t> htile_meta;
        std::array<uint32_t, 4> clear_reg{};
        OS_REQUIRE(BuildHtileFastClearMetadata(
                       desc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, 0u,
                       clear_rect, 1.0f, 0u, htile_meta, clear_reg)
                       .ok());

        XgGpuSurface2DRequest req{};
        req.desc = desc;
        req.direction = XgGpuDirection::Untile;
        req.metadata_mode = XgGpuMetadataMode::Htile;
        req.console_type = XG_CONSOLE_TYPE_SCARLETT_ANACONDA;
        req.clear_reg_value = clear_reg;

        std::vector<uint8_t> out_linear;
        XgFastClearMetadataSyncStats stats{};
        OS_REQUIRE(ExpandAndSyncFastClearMetadata2D(
                       untiler_ptr, req, tiled, htile_meta, out_linear, true, false, &stats)
                       .ok());
        OS_REQUIRE(stats.fast_cleared_blocks_expanded == 16u);  // 32/8 * 32/8 = 16 tiles

        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                uint32_t actual = 0u;
                std::memcpy(&actual, out_linear.data() + (y * kW + x) * 4u, 4u);
                if (x < 32u && y < 32u) {
                    OS_REQUIRE(actual == clear_reg[0]);  // 1.0f = 0x3F800000
                } else {
                    uint32_t orig = 0u;
                    std::memcpy(&orig, src_linear.data() + (y * kW + x) * 4u, 4u);
                    OS_REQUIRE(actual == orig);
                }
            }
        }
    }

    // 4C. CMASK fast color-clear coherency (RGBA8)
    {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = kW;
        desc.Height = kH;
        desc.MipLevels = 1u;
        desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleCount = 1u;
        desc.BindFlags = XG_BIND_RENDER_TARGET;
        desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S;

        const uint32_t row_pitch = kW * 4u;
        std::vector<uint8_t> src_linear(static_cast<size_t>(row_pitch) * kH);
        FillDeterministic(src_linear.data(), src_linear.size(), 0xC4A5u);
        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(desc, layout));
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0u);
        OS_REQUIRE(xg::TileSurface2D(desc, 0u, 0u, src_linear.data(), row_pitch, tiled.data(), tiled.size()));

        const float clear_rgba[4] = {1.0f, 0.0f, 0.5f, 1.0f};
        std::array<uint32_t, 4> clear_reg{};
        EncodeFormatFastClearRegister(desc.Format, clear_rgba, 0.0f, 0u, clear_reg);

        std::vector<uint8_t> cmask_meta;
        OS_REQUIRE(BuildCmaskFastClearMetadata(
                       desc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, 0u, clear_rect, cmask_meta)
                       .ok());

        XgGpuSurface2DRequest req{};
        req.desc = desc;
        req.direction = XgGpuDirection::Untile;
        req.metadata_mode = XgGpuMetadataMode::Cmask;
        req.console_type = XG_CONSOLE_TYPE_SCARLETT_ANACONDA;
        req.clear_reg_value = clear_reg;

        std::vector<uint8_t> out_linear;
        XgFastClearMetadataSyncStats stats{};
        OS_REQUIRE(ExpandAndSyncFastClearMetadata2D(
                       untiler_ptr, req, tiled, cmask_meta, out_linear, true, false, &stats)
                       .ok());
        OS_REQUIRE(stats.fast_cleared_blocks_expanded == 16u);
        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                uint32_t actual = 0u;
                std::memcpy(&actual, out_linear.data() + (y * kW + x) * 4u, 4u);
                if (x < 32u && y < 32u) {
                    OS_REQUIRE(actual == clear_reg[0]);
                } else {
                    uint32_t orig = 0u;
                    std::memcpy(&orig, src_linear.data() + (y * kW + x) * 4u, 4u);
                    OS_REQUIRE(actual == orig);
                }
            }
        }
        for (uint8_t b : cmask_meta) OS_REQUIRE(b == 0xFFu);
    }

    untiler.Shutdown();
    std::printf("  [PASS] DCC (pipe-aligned) + HTILE (D32F) + CMASK fast-clear expand & reset coherent (%s)\n",
                gpu_ready ? "GPU-Compute" : "CPU-Fallback");
}

// ============================================================================
// Test 5: Physical Page Double-Mapping / Aliasing (64 KB / 2 MB / 4 MB)
// ============================================================================
void TestPhysicalPageDoubleMapping(const D3D12TestBackend& backend) {
    std::printf("[Test 5] Physical Page Double-Mapping (64 KB / 2 MB / 4 MB) [%s]...\n",
                backend.label.c_str());

    PhysicalPageAliasManager mgr;

    const std::array<std::pair<PhysicalPageGranularity, const char*>, 3> grans = {{
        {PhysicalPageGranularity::Page64KB, "64KB"},
        {PhysicalPageGranularity::Page2MB, "2MB"},
        {PhysicalPageGranularity::Page4MB, "4MB"},
    }};

    for (const auto& [gran, name] : grans) {
        const uint64_t page_bytes = static_cast<uint64_t>(gran);
        uint64_t phys_id = 0ull;
        OS_REQUIRE(mgr.AllocatePhysicalBlock(gran, 2u, phys_id).ok());

        // Map Alias A covering pages [0, 2) and Alias B covering page [1, 2) (sub-range alias!)
        PhysicalAliasMapping alias_a{};
        PhysicalAliasMapping alias_b{};
        OS_REQUIRE(mgr.MapAlias(phys_id, 0u, 2u, 0x100000000ull + page_bytes, alias_a).ok());
        OS_REQUIRE(mgr.MapAlias(phys_id, 1u, 1u, 0x200000000ull + page_bytes, alias_b).ok());

        OS_REQUIRE(alias_a.cpu_va != nullptr && alias_b.cpu_va != nullptr);
        OS_REQUIRE(alias_a.cpu_va != alias_b.cpu_va);
        OS_REQUIRE(alias_a.size_bytes == 2ull * page_bytes);
        OS_REQUIRE(alias_b.size_bytes == page_bytes);

        // Write through Alias A into physical page 1 -> immediately visible through Alias B at offset 0!
        auto* pa_page1 = static_cast<uint8_t*>(alias_a.cpu_va) + page_bytes;
        auto* pb_page0 = static_cast<uint8_t*>(alias_b.cpu_va);

        std::vector<uint8_t> pattern_a(4096u);
        FillDeterministic(pattern_a.data(), pattern_a.size(), static_cast<uint32_t>(page_bytes) ^ 0xAAu);
        std::memcpy(pa_page1 + 1024u, pattern_a.data(), pattern_a.size());
        OS_REQUIRE(std::memcmp(pb_page0 + 1024u, pattern_a.data(), pattern_a.size()) == 0);

        // Write back through Alias B -> immediately visible through Alias A!
        std::vector<uint8_t> pattern_b(4096u);
        FillDeterministic(pattern_b.data(), pattern_b.size(), static_cast<uint32_t>(page_bytes) ^ 0xBBu);
        std::memcpy(pb_page0 + page_bytes - 4096u, pattern_b.data(), pattern_b.size());
        OS_REQUIRE(std::memcmp(pa_page1 + page_bytes - 4096u, pattern_b.data(), pattern_b.size()) == 0);

        // Coherent physical-page dirty tracking across aliases
        OS_REQUIRE(mgr.MarkAliasRangeDirty(alias_b.alias_id, 0u, 4096u).ok());
        std::vector<uint32_t> dirty_pages;
        OS_REQUIRE(mgr.QueryDirtyPhysicalPages(phys_id, false, dirty_pages).ok());
        OS_REQUIRE(dirty_pages.size() == 1u && dirty_pages[0] == 1u);

        // Cannot free physical block while aliases remain mapped
        OS_REQUIRE(!mgr.FreePhysicalBlock(phys_id).ok());

        // For 64KB block, also verify SyncDirtyPagesToTitleVaMirror into a live GPU shadow buffer!
        if (gran == PhysicalPageGranularity::Page64KB) {
            TitleVAMirror mirror;
            TitleVaRegionInfo rinfo{};
            OS_REQUIRE(mirror.AllocateRegion(2ull * page_bytes, kPageSize64KB,
                                             TitleVaResourceKind::Buffer, backend.device, rinfo).ok());
            FlushStats fstats{};
            OS_REQUIRE(mgr.SyncDirtyPagesToTitleVaMirror(phys_id, mirror, rinfo.title_va, nullptr, &fstats).ok());
            OS_REQUIRE(fstats.regions_flushed == 1u);
            OS_REQUIRE(fstats.bytes_uploaded >= page_bytes);

            void* mirror_cpu = nullptr;
            uint64_t rem = 0ull;
            OS_REQUIRE(mirror.ResolveTitleVaToCpu(rinfo.title_va + page_bytes + 1024u, mirror_cpu, rem));
            OS_REQUIRE(std::memcmp(mirror_cpu, pattern_a.data(), pattern_a.size()) == 0);
        }

        // Verify VA resolution
        PhysicalAliasMapping resolved{};
        OS_REQUIRE(mgr.ResolveAliasByVa(pb_page0 + 512u, resolved));
        OS_REQUIRE(resolved.alias_id == alias_b.alias_id);

        OS_REQUIRE(mgr.UnmapAlias(alias_a.alias_id).ok());
        OS_REQUIRE(mgr.UnmapAlias(alias_b.alias_id).ok());
        OS_REQUIRE(mgr.FreePhysicalBlock(phys_id).ok());
        std::printf("  [PASS] %s double-mapping: Alias A <-> Alias B hardware-coherent + dirty tracking OK\n", name);
    }

    const PhysicalAliasStats stats = mgr.GetStats();
    OS_REQUIRE(stats.active_physical_blocks == 0ull && stats.active_aliases == 0ull);
    OS_REQUIRE(stats.total_alias_mappings_created == 6ull);
}

// ============================================================================
// Test 6: VramResidencyManager LRU Eviction & Transparent Page-In under Tight Budget
//         (Verified on BOTH Host Hardware GPU and D3D12 WARP!)
// ============================================================================
void TestVramResidencyManagerLru(const D3D12TestBackend& backend) {
    std::printf("[Test 6] VramResidencyManager LRU Eviction & Page-In [%s]...\n",
                backend.label.c_str());

    for (bool release_on_evict : {false, true}) {
        VramResidencyManager vram;
        VramResidencyConfig cfg{};
        cfg.vram_budget_bytes = 8ull * 1024ull * 1024ull;  // Tight 8 MiB synthetic budget
        cfg.release_gpu_resource_on_evict = release_on_evict;
        cfg.auto_evict_on_pressure = true;
        OS_REQUIRE(vram.Initialize(backend.device, cfg).ok());

        // Allocate 16 x 2 MiB = 32 MiB virtual (4x the 8 MiB budget!)
        constexpr uint32_t kCount = 16u;
        constexpr uint64_t kAllocSize = 2ull * 1024ull * 1024ull;
        std::vector<uint64_t> handles(kCount, 0ull);
        std::vector<std::vector<uint8_t>> expected(kCount);

        for (uint32_t i = 0u; i < kCount; ++i) {
            VramAllocationDesc ad{};
            ad.size_bytes = kAllocSize;
            ad.alignment = kPageSize64KB;
            ad.debug_name = "s12_res_" + std::to_string(i);
            ad.start_resident = true;
            ad.start_pinned = (i == 0u);  // Pin allocation 0 so it MUST stay resident forever!
            OS_REQUIRE(vram.Allocate(ad, handles[i]).ok());

            // Write unique 16 KiB header + tail pattern (fast & deterministic)
            expected[i].resize(16384u);
            FillDeterministic(expected[i].data(), expected[i].size(), 0xE000u + i * 97u);
            OS_REQUIRE(vram.WriteData(handles[i], 0ull, expected[i].data(), expected[i].size()).ok());
            OS_REQUIRE(vram.WriteData(handles[i], kAllocSize - expected[i].size(),
                                      expected[i].data(), expected[i].size()).ok());

            // Budget invariant must hold after EVERY allocation!
            const VramResidencyStats st = vram.GetStats();
            OS_REQUIRE(st.resident_bytes <= cfg.vram_budget_bytes);
        }

        const VramResidencyStats mid_stats = vram.GetStats();
        OS_REQUIRE(mid_stats.using_d3d12_device);
        OS_REQUIRE(mid_stats.total_virtual_bytes == kCount * kAllocSize);  // 32 MiB virtual
        OS_REQUIRE(mid_stats.resident_bytes <= cfg.vram_budget_bytes);     // <= 8 MiB resident
        OS_REQUIRE(mid_stats.total_evictions >= 12ull);
        OS_REQUIRE(mid_stats.d3d12_evict_calls >= 12ull);

        // Allocation 0 was pinned -> MUST still be resident!
        VramAllocationInfo pin_info{};
        OS_REQUIRE(vram.GetAllocationInfo(handles[0], pin_info));
        OS_REQUIRE(pin_info.is_resident && pin_info.pin_count == 1u && pin_info.eviction_count == 0ull);

        // Allocation 1 (early unpinned) MUST have been evicted!
        VramAllocationInfo early_info{};
        OS_REQUIRE(vram.GetAllocationInfo(handles[1], early_info));
        OS_REQUIRE(!early_info.is_resident && early_info.eviction_count >= 1ull);

        // Now re-touch & read back ALL 16 allocations via GPU VRAM (triggering transparent page-in!)
        std::vector<uint8_t> head_rb(16384u);
        std::vector<uint8_t> tail_rb(16384u);
        for (uint32_t i = 0u; i < kCount; ++i) {
            OS_REQUIRE(vram.Touch(handles[i]).ok());
            VramAllocationInfo info{};
            OS_REQUIRE(vram.GetAllocationInfo(handles[i], info));
            OS_REQUIRE(info.is_resident);
            OS_REQUIRE(vram.GetResidentD3D12Resource(handles[i]) != nullptr);

            OS_REQUIRE(vram.ReadData(handles[i], 0ull, head_rb.data(), head_rb.size(), true).ok());
            OS_REQUIRE(vram.ReadData(handles[i], kAllocSize - tail_rb.size(),
                                     tail_rb.data(), tail_rb.size(), true).ok());
            OS_REQUIRE(head_rb == expected[i]);
            OS_REQUIRE(tail_rb == expected[i]);
            OS_REQUIRE(vram.GetStats().resident_bytes <= cfg.vram_budget_bytes);
        }

        // Unpin allocation 0 and verify explicit EvictToBudget(0) evicts everything cleanly
        OS_REQUIRE(vram.Unpin(handles[0]).ok());
        uint64_t ev_cnt = 0ull, ev_bytes = 0ull;
        OS_REQUIRE(vram.EvictToBudget(0ull, &ev_cnt, &ev_bytes).ok());
        OS_REQUIRE(ev_cnt > 0ull);
        OS_REQUIRE(vram.GetStats().resident_bytes == 0ull);

        const VramResidencyStats final_stats = vram.GetStats();
        OS_REQUIRE(final_stats.total_page_ins >= 12ull);
        if (!release_on_evict) {
            OS_REQUIRE(final_stats.d3d12_make_resident_calls >= 12ull);
        }

        for (uint64_t h : handles) {
            OS_REQUIRE(vram.Free(h).ok());
        }
        OS_REQUIRE(vram.GetStats().active_allocations == 0ull);
        vram.Shutdown();

        std::printf("  [PASS] 32 MiB virtual in 8 MiB budget (%s): evictions=%llu page_ins=%llu d3d12_evict=%llu make_res=%llu BIT-EXACT\n",
                    release_on_evict ? "HardRelease" : "D3D12Evict/MakeResident",
                    static_cast<unsigned long long>(final_stats.total_evictions),
                    static_cast<unsigned long long>(final_stats.total_page_ins),
                    static_cast<unsigned long long>(final_stats.d3d12_evict_calls),
                    static_cast<unsigned long long>(final_stats.d3d12_make_resident_calls));
    }
}
#endif

}  // namespace

int main() {
    std::printf("=== OpenScarlett Sprint 12 Streaming / BCPack / Metadata / VRAM Residency Suite ===\n");

    TestBcPackHardeningArraysAndFuzz();

#if defined(_WIN32)
    TestDirectStorageStreamingHardening();

    // Run ALL GPU tests on BOTH Hardware GPU (if present) AND D3D12 WARP!
    uint32_t backends_tested = 0u;

    {
        D3D12TestBackend hw{};
        if (CreateTestBackend(false, hw)) {
            TestDirectStorageBcPackToGpuUntilerTexture(hw);
            TestFastClearMetadataCoherency(hw);
            TestPhysicalPageDoubleMapping(hw);
            TestVramResidencyManagerLru(hw);
            ++backends_tested;
        } else {
            std::printf("[INFO] No hardware D3D12 GPU detected (headless runner); continuing with WARP.\n");
        }
    }

    {
        D3D12TestBackend warp{};
        OS_REQUIRE(CreateTestBackend(true, warp));
        TestDirectStorageBcPackToGpuUntilerTexture(warp);
        TestFastClearMetadataCoherency(warp);
        TestPhysicalPageDoubleMapping(warp);
        TestVramResidencyManagerLru(warp);
        ++backends_tested;
    }

    OS_REQUIRE(backends_tested >= 1u);
    std::printf("ALL SPRINT 12 STREAMING, BCPACK, METADATA & VRAM RESIDENCY TESTS PASSED (%u D3D12 backends)!\n",
                backends_tested);
#else
    std::printf("ALL SPRINT 12 NON-WINDOWS TESTS PASSED!\n");
#endif
    return 0;
}
