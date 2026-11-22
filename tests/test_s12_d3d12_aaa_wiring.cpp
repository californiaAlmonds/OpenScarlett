#include <windows.h>

#include <initguid.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "os_check.h"

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            Reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }

private:
    T* ptr_ = nullptr;
};

static constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

static bool HasHardwareD3D12Gpu(openscarlett::D3D12AdapterInfo& out_info) {
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    ComPtr<ID3D12Device> dev;
    ComPtr<IDXGIAdapter1> adapter;
    const openscarlett::Status st = openscarlett::SelectD3D12AdapterAndCreateDevice(
        opts, dev.Put(), adapter.Put(), &out_info);
    return st.ok() && dev.Get() != nullptr && !out_info.is_warp;
}

struct GpuFixture {
    ComPtr<ID3D12DeviceX> device;
    ComPtr<IOpenScarlettAaaRuntime> aaa;
    ComPtr<ID3D12CommandQueueX> queue;
    ComPtr<IOpenScarlettPresentCapture> present_capture;
    ComPtr<IOpenScarlettPresentWindow> present_window;
    ComPtr<IOpenScarlettPresentPacing> present_pacing;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandListX> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0ull;

    ~GpuFixture() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    void Init(bool prefer_warp) {
        OpenScarlettD3D12SetBackendMode(0u);
        OpenScarlettD3D12SetPreferWarp(prefer_warp ? 1 : 0);

        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1u;
        OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(
            nullptr, &params, IID_ID3D12DeviceX, device.PutVoid())));
        OS_REQUIRE(device.Get() != nullptr);

        OS_REQUIRE(SUCCEEDED(device->QueryInterface(
            IID_IOpenScarlettAaaRuntime, aaa.PutVoid())));
        OS_REQUIRE(aaa.Get() != nullptr);

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        OS_REQUIRE(SUCCEEDED(device->CreateCommandQueue(
            &qd, IID_ID3D12CommandQueueX, queue.PutVoid())));
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentCapture, present_capture.PutVoid())));
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentWindow, present_window.PutVoid())));
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentPacing, present_pacing.PutVoid())));

        OS_REQUIRE(SUCCEEDED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_ID3D12CommandAllocator, alloc.PutVoid())));
        OS_REQUIRE(SUCCEEDED(device->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
            IID_ID3D12GraphicsCommandListX, cl.PutVoid())));
        OS_REQUIRE(SUCCEEDED(cl->Close()));

        OS_REQUIRE(SUCCEEDED(device->CreateFence(
            0ull, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, fence.PutVoid())));
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(fence_event != nullptr);
    }

    void ResetList() {
        OS_REQUIRE(SUCCEEDED(alloc->Reset()));
        OS_REQUIRE(SUCCEEDED(cl->Reset(alloc.Get(), nullptr)));
    }

    uint64_t SubmitAndWait() {
        OS_REQUIRE(SUCCEEDED(cl->Close()));
        ID3D12CommandList* lists[1] = {cl.Get()};
        queue->ExecuteCommandLists(1u, lists);
        ++fence_val;
        OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), fence_val)));
        if (fence->GetCompletedValue() < fence_val) {
            OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event)));
            const DWORD wr = WaitForSingleObject(fence_event, 15000u);
            OS_REQUIRE(wr == WAIT_OBJECT_0);
        }
        return fence_val;
    }

    ComPtr<ID3D12Resource> CreateBuffer(
        D3D12_HEAP_TYPE heap_type,
        uint64_t bytes,
        D3D12_RESOURCE_STATES initial_state,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> res;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr,
            IID_ID3D12Resource, res.PutVoid())));
        return res;
    }
};

// =============================================================================
// Test 1: SamplerFeedbackManager LIVE (Create, UAV, CopyDescriptors, Clear,
//         WriteFeedbackSamples, ResolveSubresourceRegion Decode + Encode)
// =============================================================================
void TestSamplerFeedbackLive(GpuFixture& fx, const char* tag) {
    // 1) Target paired texture: 256x256, 6 mips
    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 256u;
    target_desc.Height = 256u;
    target_desc.DepthOrArraySize = 1u;
    target_desc.MipLevels = 6u;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1u;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> target_tex;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &target_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_ID3D12Resource, target_tex.PutVoid())));

    // 2) Two feedback maps created through ID3D12DeviceX::CreateCommittedResource2
    D3D12_RESOURCE_DESC1 fb_desc{};
    fb_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fb_desc.Width = 256u;
    fb_desc.Height = 256u;
    fb_desc.DepthOrArraySize = 1u;
    fb_desc.MipLevels = 6u;
    fb_desc.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
    fb_desc.SampleDesc.Count = 1u;
    fb_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    fb_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    fb_desc.SamplerFeedbackMipRegion.Width = 16u;
    fb_desc.SamplerFeedbackMipRegion.Height = 16u;
    fb_desc.SamplerFeedbackMipRegion.Depth = 1u;

    ComPtr<ID3D12Resource> fb_map_a;
    ComPtr<ID3D12Resource> fb_map_b;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource2(
        &def_hp, D3D12_HEAP_FLAG_NONE, &fb_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
        IID_ID3D12Resource, fb_map_a.PutVoid())));
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource2(
        &def_hp, D3D12_HEAP_FLAG_NONE, &fb_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
        IID_ID3D12Resource, fb_map_b.PutVoid())));

    // 3) CreateSamplerFeedbackUnorderedAccessView + CopyDescriptorsSimple preservation
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4u;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> cpu_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(
        &hd, IID_ID3D12DescriptorHeap, cpu_heap.PutVoid())));

    const UINT inc = fx.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Create feedback UAVs for both maps + CopyDescriptorsSimple preservation
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = cpu_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h1{h0.ptr + inc};
    D3D12_CPU_DESCRIPTOR_HANDLE h2{h0.ptr + 2u * inc};
    fx.device->CreateSamplerFeedbackUnorderedAccessView(target_tex.Get(), fb_map_a.Get(), h0);
    fx.device->CreateSamplerFeedbackUnorderedAccessView(target_tex.Get(), fb_map_b.Get(), h2);
    fx.device->CopyDescriptorsSimple(1u, h1, h0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 4) Destination 16x16 R8_UINT texture for DECODE_SAMPLER_FEEDBACK
    D3D12_RESOURCE_DESC dec_desc{};
    dec_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dec_desc.Width = 16u;
    dec_desc.Height = 16u;
    dec_desc.DepthOrArraySize = 1u;
    dec_desc.MipLevels = 1u;
    dec_desc.Format = DXGI_FORMAT_R8_UINT;
    dec_desc.SampleDesc.Count = 1u;
    dec_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> decoded_tex;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &dec_desc,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
        IID_ID3D12Resource, decoded_tex.PutVoid())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0u;
    UINT64 row_bytes = 0ull;
    UINT64 total_bytes = 0ull;
    fx.device->GetCopyableFootprints(&dec_desc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
    auto rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, AlignUp(total_bytes, 256ull), D3D12_RESOURCE_STATE_COPY_DEST);

    // 5) Clear -> WriteFeedbackSamples (with min-mip atomics) -> ResolveSubresourceRegion(DECODE) -> Readback
    fx.ResetList();
    OS_REQUIRE(SUCCEEDED(fx.aaa->ClearFeedbackMap(fx.cl.Get(), fb_map_a.Get())));
    OS_REQUIRE(SUCCEEDED(fx.aaa->ClearFeedbackMap(fx.cl.Get(), fb_map_b.Get())));

    // Note: on native Tier 0.9/1.0 hardware, writing mip M marks the mip-M region,
    // so keep mip writes in disjoint quadrants and check untouched tile (15, 15).
    const uint32_t sample_quads[] = {
        2u, 3u, 0u, 2u,  // tile (2,3) mip 2
        2u, 3u, 0u, 0u,  // tile (2,3) mip 0 -> min(2, 0) == 0
        8u, 8u, 0u, 1u,  // tile (8,8) mip 1 -> 2x2 region [8..9]x[8..9]
    };
    OS_REQUIRE(SUCCEEDED(fx.aaa->WriteFeedbackSamples(fx.cl.Get(), fb_map_a.Get(), sample_quads, 3u)));

    D3D12_RESOURCE_BARRIER to_res{};
    to_res.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_res.Transition.pResource = fb_map_a.Get();
    to_res.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_res.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_res.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    fx.cl->ResourceBarrier(1u, &to_res);

    fx.cl->ResolveSubresourceRegion(
        decoded_tex.Get(), UINT_MAX, 0u, 0u,
        fb_map_a.Get(), UINT_MAX, nullptr,
        DXGI_FORMAT_R8_UINT,
        D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);

    // Copy decoded R8_UINT to readback AND test ENCODE_SAMPLER_FEEDBACK into fb_map_b!
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = decoded_tex.Get();
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fx.cl->ResourceBarrier(1u, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION dloc{};
    dloc.pResource = rb.Get();
    dloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dloc.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sloc{};
    sloc.pResource = decoded_tex.Get();
    sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sloc.SubresourceIndex = 0u;
    fx.cl->CopyTextureRegion(&dloc, 0u, 0u, 0u, &sloc, nullptr);

    D3D12_RESOURCE_BARRIER enc_barriers[2]{};
    enc_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    enc_barriers[0].Transition.pResource = decoded_tex.Get();
    enc_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    enc_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    enc_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    enc_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    enc_barriers[1].Transition.pResource = fb_map_b.Get();
    enc_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    enc_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    enc_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    fx.cl->ResourceBarrier(2u, enc_barriers);

    fx.cl->ResolveSubresourceRegion(
        fb_map_b.Get(), UINT_MAX, 0u, 0u,
        decoded_tex.Get(), UINT_MAX, nullptr,
        DXGI_FORMAT_R8_UINT,
        D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);

    D3D12_RESOURCE_BARRIER back_uav{};
    back_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    back_uav.Transition.pResource = fb_map_b.Get();
    back_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    back_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    back_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    fx.cl->ResourceBarrier(1u, &back_uav);

    fx.SubmitAndWait();

    void* mapped = nullptr;
    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &mapped)));
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    OS_REQUIRE(bytes[3u * fp.Footprint.RowPitch + 2u] == 0u);
    OS_REQUIRE(bytes[8u * fp.Footprint.RowPitch + 8u] == 1u);
    OS_REQUIRE(bytes[15u * fp.Footprint.RowPitch + 15u] == 0xFFu);
    rb->Unmap(0u, nullptr);

    // Verify ENCODE round-trip by reading back fb_map_b through SamplerFeedbackManager
    std::vector<uint8_t> encoded_rb(256u, 0u);
    uint32_t tx = 0u, ty = 0u;
    OS_REQUIRE(SUCCEEDED(fx.aaa->ReadbackFeedbackMinMip(
        fx.queue.Get(), fb_map_b.Get(),
        encoded_rb.data(), static_cast<uint32_t>(encoded_rb.size()), &tx, &ty)));
    OS_REQUIRE(tx == 16u && ty == 16u);
    OS_REQUIRE(encoded_rb[3u * 16u + 2u] == 0u);
    OS_REQUIRE(encoded_rb[8u * 16u + 8u] == 1u);
    OS_REQUIRE(encoded_rb[15u * 16u + 15u] == 0xFFu);

    D3D12XBOX_AAA_WIRING_STATS stats{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats)));
    OS_REQUIRE(stats.sf_active_maps >= 2u);
    OS_REQUIRE(stats.sf_preserved_views >= 2u);
    OS_REQUIRE(stats.sf_total_clears >= 2u);
    OS_REQUIRE(stats.sf_decode_intercepts >= 1u);
    OS_REQUIRE(stats.sf_encode_intercepts >= 1u);
    std::printf("[%s] PASS SamplerFeedbackManager LIVE (decode+encode+views, tier=%u)\n",
                tag, stats.sf_host_tier);
}

// =============================================================================
// Test 2: QueryAccumulatorX LIVE (multi-command-list Timestamp & Occlusion +
//         GPU-QPC Clock Calibration)
// =============================================================================
void TestQueryAccumulatorLive(GpuFixture& fx, const char* tag) {
    D3D12XBOX_AAA_WIRING_STATS stats0{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats0)));
    OS_REQUIRE(stats0.qa_initialized == 1u);

    ComPtr<ID3D12QueryHeap> ts_heap;
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetQueryAccumulatorHeap(2u /*QueryKind::Timestamp*/, ts_heap.Put())));
    OS_REQUIRE(ts_heap.Get() != nullptr);

    D3D12_QUERY_HEAP_DESC occ_hd{};
    occ_hd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    occ_hd.Count = 4u;
    ComPtr<ID3D12QueryHeap> occ_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateQueryHeap(
        &occ_hd, IID_ID3D12QueryHeap, occ_heap.PutVoid())));

    auto ts_rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, 256u, D3D12_RESOURCE_STATE_COPY_DEST);
    auto occ_rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, 256u, D3D12_RESOURCE_STATE_COPY_DEST);

    // Second command allocator + command list to test multi-command-list query accumulation
    ComPtr<ID3D12CommandAllocator> alloc2;
    ComPtr<ID3D12GraphicsCommandListX> cl2;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, alloc2.PutVoid())));
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, cl2.PutVoid())));

    // List 1: Timestamp 0 + Occlusion 0
    fx.ResetList();
    fx.cl->EndQuery(ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u);
    fx.cl->BeginQuery(occ_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0u);
    fx.cl->EndQuery(occ_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0u);
    fx.cl->ResolveQueryData(occ_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0u, 1u, occ_rb.Get(), 0u);
    OS_REQUIRE(SUCCEEDED(fx.cl->Close()));

    // List 2: Timestamp 1 + Resolve both timestamps
    cl2->EndQuery(ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1u);
    cl2->ResolveQueryData(ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u, 2u, ts_rb.Get(), 0u);
    OS_REQUIRE(SUCCEEDED(cl2->Close()));

    ID3D12CommandList* lists[2] = {fx.cl.Get(), cl2.Get()};
    fx.queue->ExecuteCommandLists(2u, lists);
    ++fx.fence_val;
    OS_REQUIRE(SUCCEEDED(fx.queue->Signal(fx.fence.Get(), fx.fence_val)));
    if (fx.fence->GetCompletedValue() < fx.fence_val) {
        OS_REQUIRE(SUCCEEDED(fx.fence->SetEventOnCompletion(fx.fence_val, fx.fence_event)));
        OS_REQUIRE(WaitForSingleObject(fx.fence_event, 15000u) == WAIT_OBJECT_0);
    }

    void* ts_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(ts_rb->Map(0u, nullptr, &ts_ptr)));
    const auto* ts = static_cast<const uint64_t*>(ts_ptr);
    OS_REQUIRE(ts[1] >= ts[0]);
    ts_rb->Unmap(0u, nullptr);

    UINT64 freq = 0ull;
    OS_REQUIRE(SUCCEEDED(fx.queue->GetTimestampFrequency(&freq)));
    OS_REQUIRE(freq > 0ull);

    UINT64 gpu_ts = 0ull, cpu_ts = 0ull;
    OS_REQUIRE(SUCCEEDED(fx.queue->GetClockCalibration(&gpu_ts, &cpu_ts)));
    OS_REQUIRE(cpu_ts > 0ull);

    D3D12XBOX_CLOCK_CALIBRATION_SUMMARY cal{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->CalibrateQueueClock(fx.queue.Get(), &cal)));
    OS_REQUIRE(cal.valid == 1u);
    OS_REQUIRE(cal.gpu_timestamp_frequency_hz > 0ull);
    OS_REQUIRE(cal.cpu_qpc_frequency_hz > 0ull);

    D3D12XBOX_AAA_WIRING_STATS stats{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats)));
    OS_REQUIRE(stats.qa_cl_begin_count >= 1u);
    OS_REQUIRE(stats.qa_cl_end_count >= 3u);
    OS_REQUIRE(stats.qa_cl_resolve_count >= 2u);
    std::printf("[%s] PASS QueryAccumulatorX LIVE (multi-CL ts=%llu->%llu, freq=%llu Hz)\n",
                tag,
                static_cast<unsigned long long>(ts[0]),
                static_cast<unsigned long long>(ts[1]),
                static_cast<unsigned long long>(freq));
}

// =============================================================================
// Test 3: TitleVAMirror LIVE (CreatePlacedResourceX dirty CPU->GPU flush,
//         fence-tracked GPU->CPU writeback, ExecuteIndirect live dirty flush)
// =============================================================================
void TestTitleVaMirrorLive(GpuFixture& fx, const char* tag) {
    // 1) Allocate write-watch CPU page for Title VA
    constexpr size_t kMirrorBytes = 16384u;
    void* cpu_va_ptr = VirtualAlloc(
        nullptr, kMirrorBytes,
        MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
        PAGE_READWRITE);
    OS_REQUIRE(cpu_va_ptr != nullptr);
    const uint64_t title_va = reinterpret_cast<uint64_t>(cpu_va_ptr);

    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = kMirrorBytes;
    buf_desc.Height = 1u;
    buf_desc.DepthOrArraySize = 1u;
    buf_desc.MipLevels = 1u;
    buf_desc.Format = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1u;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> mirrored_buf;
    OS_REQUIRE(SUCCEEDED(fx.device->CreatePlacedResourceX(
        title_va, &buf_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_ID3D12Resource, mirrored_buf.PutVoid())));

    // 2) Mutate CPU page -> ExecuteCommandLists must auto-flush dirty page to GPU!
    auto* cpu_u32 = static_cast<uint32_t*>(cpu_va_ptr);
    cpu_u32[0] = 0xCAFE1234u;
    cpu_u32[1] = 0xDEAD5678u;

    auto rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, 256u, D3D12_RESOURCE_STATE_COPY_DEST);
    fx.ResetList();
    fx.cl->CopyBufferRegion(rb.Get(), 0u, mirrored_buf.Get(), 0u, 16u);
    fx.SubmitAndWait();

    void* rb_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &rb_ptr)));
    const auto* rb_u32 = static_cast<const uint32_t*>(rb_ptr);
    OS_REQUIRE(rb_u32[0] == 0xCAFE1234u);
    OS_REQUIRE(rb_u32[1] == 0xDEAD5678u);
    rb->Unmap(0u, nullptr);

    // 3) GPU writes back into mirrored_buf -> fence-tracked GPU->CPU writeback updates cpu_u32!
    auto up = fx.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 256u, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* up_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(up->Map(0u, nullptr, &up_ptr)));
    auto* up_u32 = static_cast<uint32_t*>(up_ptr);
    up_u32[0] = 0x55AA9911u;
    up_u32[1] = 0x13572468u;
    up->Unmap(0u, nullptr);

    fx.ResetList();
    D3D12_RESOURCE_BARRIER to_dst{};
    to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_dst.Transition.pResource = mirrored_buf.Get();
    to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    fx.cl->ResourceBarrier(1u, &to_dst);
    fx.cl->CopyBufferRegion(mirrored_buf.Get(), 0u, up.Get(), 0u, 16u);
    D3D12_RESOURCE_BARRIER to_common = to_dst;
    to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    fx.cl->ResourceBarrier(1u, &to_common);
    fx.SubmitAndWait();
    // KickoffX pumps TitleVAMirror::CompleteReadbacksOnFence for completed internal queue fences.
    fx.queue->KickoffX();

    OS_REQUIRE(cpu_u32[0] == 0x55AA9911u);
    OS_REQUIRE(cpu_u32[1] == 0x13572468u);

    // 4) ExecuteIndirect with a TitleVAMirror-backed indirect dispatch args buffer!
    void* ind_cpu_ptr = VirtualAlloc(
        nullptr, 4096u,
        MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
        PAGE_READWRITE);
    OS_REQUIRE(ind_cpu_ptr != nullptr);
    D3D12_RESOURCE_DESC ind_desc = buf_desc;
    ind_desc.Width = 4096u;
    ind_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> ind_buf;
    OS_REQUIRE(SUCCEEDED(fx.device->CreatePlacedResourceX(
        reinterpret_cast<uint64_t>(ind_cpu_ptr), &ind_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_ID3D12Resource, ind_buf.PutVoid())));

    // Build simple SM 5.0 compute PSO that writes tid + 100 into RWByteAddressBuffer
    const char* cs_src =
        "RWByteAddressBuffer g_out : register(u0);\n"
        "[numthreads(1, 1, 1)]\n"
        "void main(uint3 tid : SV_DispatchThreadID) {\n"
        "    g_out.Store(tid.x * 4, tid.x + 100u);\n"
        "}\n";
    ComPtr<ID3DBlob> cs_blob, err_blob;
    OS_REQUIRE(SUCCEEDED(D3DCompile(
        cs_src, std::strlen(cs_src), "s12_ind_cs", nullptr, nullptr,
        "main", "cs_5_0", 0u, 0u, cs_blob.Put(), err_blob.Put())));

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.Descriptor.ShaderRegister = 0u;
    rp.Descriptor.RegisterSpace = 0u;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1u;
    rsd.pParameters = &rp;
    ComPtr<ID3DBlob> rs_blob;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rsd, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), err_blob.Put())));
    ComPtr<ID3D12RootSignature> rs;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        IID_ID3D12RootSignature, rs.PutVoid())));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = rs.Get();
    pso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength = cs_blob->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateComputePipelineState(
        &pso_desc, IID_ID3D12PipelineState, pso.PutVoid())));

    D3D12_INDIRECT_ARGUMENT_DESC arg_desc{};
    arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sig_desc{};
    sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sig_desc.NumArgumentDescs = 1u;
    sig_desc.pArgumentDescs = &arg_desc;
    ComPtr<ID3D12CommandSignature> cmd_sig;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommandSignature(
        &sig_desc, nullptr, IID_ID3D12CommandSignature, cmd_sig.PutVoid())));

    auto out_uav = fx.CreateBuffer(
        D3D12_HEAP_TYPE_DEFAULT, 256u,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Mutate indirect dispatch args on CPU right before ExecuteIndirect -> {4, 1, 1}
    auto* disp_args = static_cast<D3D12_DISPATCH_ARGUMENTS*>(ind_cpu_ptr);
    disp_args->ThreadGroupCountX = 4u;
    disp_args->ThreadGroupCountY = 1u;
    disp_args->ThreadGroupCountZ = 1u;

    fx.ResetList();
    fx.cl->SetComputeRootSignature(rs.Get());
    fx.cl->SetPipelineState(pso.Get());
    fx.cl->SetComputeRootUnorderedAccessView(0u, out_uav->GetGPUVirtualAddress());
    fx.cl->ExecuteIndirect(cmd_sig.Get(), 1u, ind_buf.Get(), 0u, nullptr, 0u);
    D3D12_RESOURCE_BARRIER out_bar[2]{};
    out_bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    out_bar[0].UAV.pResource = out_uav.Get();
    out_bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out_bar[1].Transition.pResource = out_uav.Get();
    out_bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out_bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    out_bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fx.cl->ResourceBarrier(2u, out_bar);
    fx.cl->CopyBufferRegion(rb.Get(), 0u, out_uav.Get(), 0u, 16u);
    fx.SubmitAndWait();

    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &rb_ptr)));
    rb_u32 = static_cast<const uint32_t*>(rb_ptr);
    OS_REQUIRE(rb_u32[0] == 100u);
    OS_REQUIRE(rb_u32[1] == 101u);
    OS_REQUIRE(rb_u32[2] == 102u);
    OS_REQUIRE(rb_u32[3] == 103u);
    rb->Unmap(0u, nullptr);

    D3D12XBOX_AAA_WIRING_STATS stats{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats)));
    OS_REQUIRE(stats.va_active_regions >= 2u);
    OS_REQUIRE(stats.va_dirty_pages_flushed >= 2u);
    OS_REQUIRE(stats.va_bytes_uploaded > 0ull);
    OS_REQUIRE(stats.va_bytes_readback > 0ull);
    OS_REQUIRE(stats.va_queue_flushes >= 1u);
    OS_REQUIRE(stats.va_indirect_flushes >= 1u);

    ind_buf.Reset();
    mirrored_buf.Reset();
    VirtualFree(ind_cpu_ptr, 0u, MEM_RELEASE);
    VirtualFree(cpu_va_ptr, 0u, MEM_RELEASE);
    std::printf("[%s] PASS TitleVAMirror LIVE (dirty_flushes=%llu, up=%lluB, rb=%lluB, indirect=%llu)\n",
                tag,
                static_cast<unsigned long long>(stats.va_dirty_pages_flushed),
                static_cast<unsigned long long>(stats.va_bytes_uploaded),
                static_cast<unsigned long long>(stats.va_bytes_readback),
                static_cast<unsigned long long>(stats.va_indirect_flushes));
}

// =============================================================================
// Test 4: DescriptorSgprBridge LIVE (SetDescriptorHeaps, RootTable,
//         Root32BitConstants, RootCBV, BindBridgeBufferSrv/Heaps)
// =============================================================================
void TestDescriptorSgprBridgeLive(GpuFixture& fx, const char* tag) {
    D3D12_DESCRIPTOR_HEAP_DESC srv_hd{};
    srv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_hd.NumDescriptors = 8u;
    srv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(
        &srv_hd, IID_ID3D12DescriptorHeap, srv_heap.PutVoid())));

    D3D12_DESCRIPTOR_HEAP_DESC smp_hd{};
    smp_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    smp_hd.NumDescriptors = 4u;
    smp_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> smp_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(
        &smp_hd, IID_ID3D12DescriptorHeap, smp_heap.PutVoid())));

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1u;
    range.BaseShaderRegister = 0u;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0u;
    params[0].Constants.Num32BitValues = 4u;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1u;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1u;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3u;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> rs_blob, err_blob;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rsd, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), err_blob.Put())));
    ComPtr<ID3D12RootSignature> rs;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        IID_ID3D12RootSignature, rs.PutVoid())));

    auto cb_buf = fx.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 256u, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint32_t bindless_idx = 0xFFFFFFFFu;
    OS_REQUIRE(SUCCEEDED(fx.aaa->BindBridgeBufferSrv(cb_buf.Get(), 64u, 4u, &bindless_idx)));
    OS_REQUIRE(bindless_idx < 4096u);

    fx.ResetList();
    ID3D12DescriptorHeap* heaps[2] = {srv_heap.Get(), smp_heap.Get()};
    fx.cl->SetDescriptorHeaps(2u, heaps);
    fx.cl->SetComputeRootSignature(rs.Get());
    fx.cl->SetComputeRoot32BitConstant(0u, 0x11223344u, 0u);
    const uint32_t more[2] = {0x55667788u, 0x99AABBCCu};
    fx.cl->SetComputeRoot32BitConstants(0u, 2u, more, 1u);
    fx.cl->SetComputeRootDescriptorTable(1u, srv_heap->GetGPUDescriptorHandleForHeapStart());
    fx.cl->SetComputeRootConstantBufferView(2u, cb_buf->GetGPUVirtualAddress());
    OS_REQUIRE(SUCCEEDED(fx.aaa->BindBridgeHeaps(fx.cl.Get())));
    fx.SubmitAndWait();

    D3D12XBOX_AAA_WIRING_STATS stats{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats)));
    OS_REQUIRE(stats.db_initialized == 1u);
    OS_REQUIRE(stats.db_heap_binds >= 2u);
    OS_REQUIRE(stats.db_table_binds >= 1u);
    OS_REQUIRE(stats.db_root_const_updates >= 2u);
    OS_REQUIRE(stats.db_root_cbv_updates >= 1u);
    OS_REQUIRE(stats.db_vsharp_bindings >= 1u);
    OS_REQUIRE(stats.db_root_sgprs[0] == 0x11223344u);
    OS_REQUIRE(stats.db_root_sgprs[1] == 0x55667788u);
    const uint64_t mirrored_cbv =
        static_cast<uint64_t>(stats.db_root_sgprs[4]) |
        (static_cast<uint64_t>(stats.db_root_sgprs[5]) << 32u);
    OS_REQUIRE(mirrored_cbv == cb_buf->GetGPUVirtualAddress());
    std::printf("[%s] PASS DescriptorSgprBridge LIVE (vsharp_idx=%u, sgpr0=0x%08X, table_binds=%llu)\n",
                tag, bindless_idx, stats.db_root_sgprs[0],
                static_cast<unsigned long long>(stats.db_table_binds));
}

// =============================================================================
// Test 5: XgGpuUntiler LIVE inside CopyTextureRegion (GFX10 4KB_S tiled upload)
// =============================================================================
void TestXgGpuUntilerLive(GpuFixture& fx, const char* tag) {
    constexpr uint32_t kW = 64u;
    constexpr uint32_t kH = 64u;
    constexpr uint32_t kBpp = 4u;
    constexpr uint32_t kRowPitch = kW * kBpp;

    std::vector<uint8_t> linear_src(kW * kH * kBpp);
    for (uint32_t y = 0u; y < kH; ++y) {
        for (uint32_t x = 0u; x < kW; ++x) {
            const size_t off = (y * kW + x) * 4u;
            linear_src[off + 0u] = static_cast<uint8_t>(x * 3u + 7u);
            linear_src[off + 1u] = static_cast<uint8_t>(y * 5u + 11u);
            linear_src[off + 2u] = static_cast<uint8_t>((x ^ y) * 2u + 19u);
            linear_src[off + 3u] = 0xFFu;
        }
    }

    XG_TEXTURE2D_DESC xg_desc{};
    xg_desc.Width = kW;
    xg_desc.Height = kH;
    xg_desc.MipLevels = 1u;
    xg_desc.ArraySize = 1u;
    xg_desc.Format = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    xg_desc.SampleCount = 1u;
    xg_desc.BindFlags = XG_BIND_SHADER_RESOURCE;
    xg_desc.TileMode = XG_TILE_MODE_2D_THIN_4KB_S;

    XG_RESOURCE_LAYOUT layout{};
    OS_REQUIRE(openscarlett::xg::ComputeTexture2DLayout(xg_desc, layout));
    OS_REQUIRE(layout.SizeBytes >= linear_src.size());

    std::vector<uint8_t> tiled_bytes(static_cast<size_t>(layout.SizeBytes), 0u);
    OS_REQUIRE(openscarlett::xg::TileSurface2D(
        xg_desc, 0u, 0u, linear_src.data(), kRowPitch,
        tiled_bytes.data(), tiled_bytes.size()));

    auto upload_tiled = fx.CreateBuffer(
        D3D12_HEAP_TYPE_UPLOAD,
        AlignUp(tiled_bytes.size(), 256ull),
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* up_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(upload_tiled->Map(0u, nullptr, &up_ptr)));
    std::memcpy(up_ptr, tiled_bytes.data(), tiled_bytes.size());
    upload_tiled->Unmap(0u, nullptr);

    OS_REQUIRE(SUCCEEDED(fx.aaa->SetResourceTileMode(
        upload_tiled.Get(), static_cast<uint32_t>(XG_TILE_MODE_2D_THIN_4KB_S))));
    uint32_t queried_mode = 0u;
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetResourceTileMode(upload_tiled.Get(), &queried_mode)));
    OS_REQUIRE(queried_mode == static_cast<uint32_t>(XG_TILE_MODE_2D_THIN_4KB_S));

    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kW;
    tex_desc.Height = kH;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = 1u;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1u;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> dst_tex;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_ID3D12Resource, dst_tex.PutVoid())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0u;
    UINT64 row_bytes = 0ull;
    UINT64 total_bytes = 0ull;
    fx.device->GetCopyableFootprints(&tex_desc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
    auto rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, AlignUp(total_bytes, 256ull), D3D12_RESOURCE_STATE_COPY_DEST);

    fx.ResetList();
    D3D12_TEXTURE_COPY_LOCATION dloc{};
    dloc.pResource = dst_tex.Get();
    dloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dloc.SubresourceIndex = 0u;
    D3D12_TEXTURE_COPY_LOCATION sloc{};
    sloc.pResource = upload_tiled.Get();
    sloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sloc.PlacedFootprint = fp;
    sloc.PlacedFootprint.Offset = 0ull;
    // Live CopyTextureRegion must detect XG_TILE_MODE_2D_THIN_4KB_S and untile via XgGpuUntiler!
    fx.cl->CopyTextureRegion(&dloc, 0u, 0u, 0u, &sloc, nullptr);

    D3D12_RESOURCE_BARRIER to_src{};
    to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_src.Transition.pResource = dst_tex.Get();
    to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fx.cl->ResourceBarrier(1u, &to_src);

    D3D12_TEXTURE_COPY_LOCATION rb_dst{};
    rb_dst.pResource = rb.Get();
    rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rb_dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION tex_src = dloc;
    fx.cl->CopyTextureRegion(&rb_dst, 0u, 0u, 0u, &tex_src, nullptr);
    fx.SubmitAndWait();

    void* rb_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &rb_ptr)));
    const auto* got = static_cast<const uint8_t*>(rb_ptr);
    for (uint32_t y = 0u; y < kH; ++y) {
        const uint8_t* got_row = got + static_cast<size_t>(y) * fp.Footprint.RowPitch;
        const uint8_t* exp_row = linear_src.data() + static_cast<size_t>(y) * kRowPitch;
        OS_REQUIRE(std::memcmp(got_row, exp_row, kRowPitch) == 0);
    }
    rb->Unmap(0u, nullptr);

    D3D12XBOX_AAA_WIRING_STATS stats{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats)));
    OS_REQUIRE((stats.xg_gpu_untile_dispatches + stats.xg_cpu_fallbacks) >= 1u);
    std::printf("[%s] PASS XgGpuUntiler LIVE (64x64 4KB_S bit-exact, gpu_dispatches=%llu, cpu_fb=%llu)\n",
                tag,
                static_cast<unsigned long long>(stats.xg_gpu_untile_dispatches),
                static_cast<unsigned long long>(stats.xg_cpu_fallbacks));
}

// =============================================================================
// Test 6: GXDK D3D12X Extensions LIVE (KickoffX, Write32BitValueX, Wait32/64,
//         RSSetDepthBiasX, SetPredicationX, FlushGpuCacheRange, DispatchMesh(x,y,z))
// =============================================================================
bool CompileShaderWithDxc(
    const char* hlsl,
    const char* entry,
    const char* profile,
    std::vector<uint8_t>& out_dxil) {
    const std::filesystem::path dxc_candidates[] = {
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\dxc.exe",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22621.0\\x64\\dxc.exe",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.19041.0\\x64\\dxc.exe",
    };
    std::wstring dxc_path = L"dxc.exe";
    for (const auto& c : dxc_candidates) {
        if (std::filesystem::exists(c)) {
            dxc_path = c.wstring();
            break;
        }
    }

    // Write scratch HLSL/DXIL next to the running test executable (portable in dist/ & strictly on E:\ during builds).
    wchar_t exe_buf[MAX_PATH]{};
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    std::filesystem::path base_dir =
        (exe_len > 0u && exe_len < MAX_PATH)
            ? std::filesystem::path(exe_buf).parent_path()
            : std::filesystem::path(OPENSCARLETT_BINARY_DIR);
    if (std::filesystem::exists(base_dir / L"dxc.exe")) {
        dxc_path = (base_dir / L"dxc.exe").wstring();
    }
    const std::filesystem::path scratch_dir = base_dir / "s12_dxc_scratch";
    std::error_code ec;
    std::filesystem::create_directories(scratch_dir, ec);

    static std::atomic<uint32_t> s_seq{0u};
    const uint32_t id = s_seq.fetch_add(1u, std::memory_order_relaxed);
    const std::filesystem::path hlsl_path = scratch_dir / ("sh_" + std::to_string(id) + ".hlsl");
    const std::filesystem::path dxil_path = scratch_dir / ("sh_" + std::to_string(id) + ".dxil");

    {
        std::ofstream ofs(hlsl_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(hlsl, static_cast<std::streamsize>(std::strlen(hlsl)));
    }

    auto to_wide = [](const char* s) {
        std::wstring w;
        for (const char* p = s; *p; ++p) w.push_back(static_cast<wchar_t>(*p));
        return w;
    };

    std::wstring cmd = L"\"" + dxc_path + L"\" -T " + to_wide(profile) +
                       L" -E " + to_wide(entry) +
                       L" -Fo \"" + dxil_path.wstring() + L"\" \"" + hlsl_path.wstring() + L"\"";
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 15000u);
    DWORD exit_code = 1u;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0u) return false;

    std::ifstream ifs(dxil_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    out_dxil.resize(static_cast<size_t>(sz));
    ifs.seekg(0);
    ifs.read(reinterpret_cast<char*>(out_dxil.data()), sz);
    return true;
}

void TestGxdkExtensionsAndMesh3DLive(GpuFixture& fx, const char* tag) {
    // 1) Write32BitValueX on GPU buffer VA & CPU pointer VA + Queue::Write32BitValueX
    auto gpu_w32 = fx.CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, 256u, D3D12_RESOURCE_STATE_COPY_DEST);
    auto rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, 256u, D3D12_RESOURCE_STATE_COPY_DEST);
    alignas(8) uint32_t cpu_cl_marker = 0u;
    alignas(8) uint32_t cpu_q_marker = 0u;
    alignas(8) std::atomic<uint32_t> wait32_cell{0x12345678u};
    alignas(8) std::atomic<uint64_t> wait64_cell{0x0102030405060708ull};

    fx.queue->Write32BitValueX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&cpu_q_marker),
        0x778899AAu, D3D12XBOX_WRITE_VALUE_FLAG_NONE);
    OS_REQUIRE(cpu_q_marker == 0x778899AAu);

    // Queue Wait32/Wait64 with asynchronous signal from background thread
    alignas(8) std::atomic<uint32_t> async_wait32{0u};
    std::thread sig_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        async_wait32.store(0xCAFE0001u, std::memory_order_release);
    });
    fx.queue->Wait32BitValueX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&async_wait32),
        0xCAFE0001u, 0xFFFFFFFFu, D3D12XBOX_WAIT_VALUE_OP_EQUAL);
    sig_thread.join();
    OS_REQUIRE(async_wait32.load(std::memory_order_acquire) == 0xCAFE0001u);

    fx.queue->Wait64BitValueX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&wait64_cell),
        0x0102030405060708ull, 0xFFFFFFFFFFFFFFFFull, D3D12XBOX_WAIT_VALUE_OP_GREATER_EQUAL);

    // 2) SetPredicationX LIVE on GPU: pred[0]=0, pred[1]=1
    auto pred_buf = fx.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 256u, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(pred_buf->Map(0u, nullptr, &p_ptr)));
    auto* p_u64 = static_cast<uint64_t*>(p_ptr);
    p_u64[0] = 0ull;  // offset 0: zero
    p_u64[1] = 1ull;  // offset 8: non-zero
    pred_buf->Unmap(0u, nullptr);

    auto src_val = fx.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 256u, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* s_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(src_val->Map(0u, nullptr, &s_ptr)));
    auto* s_u32 = static_cast<uint32_t*>(s_ptr);
    s_u32[0] = 0x11111111u;
    s_u32[1] = 0x11111111u;
    s_u32[2] = 0x11111111u;
    s_u32[3] = 0xDEADBEEFu;
    src_val->Unmap(0u, nullptr);

    auto pred_dst = fx.CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, 256u, D3D12_RESOURCE_STATE_COPY_DEST);

    fx.ResetList();
    // RSSetDepthBiasX
    fx.cl->RSSetDepthBiasX(-2.5f, 0.05f, 1.75f);
    float db = 0.0f, dbc = 0.0f, ssdb = 0.0f;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CommandListGetDepthBiasX(fx.cl.Get(), &db, &dbc, &ssdb)));
    OS_REQUIRE(db == -2.5f && dbc == 0.05f && ssdb == 1.75f);

    // CommandList Wait32/Wait64 + Write32 (GPU VA + CPU VA) + FlushGpuCacheRange + KickoffX
    fx.cl->Wait32BitValueX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&wait32_cell),
        0x12345678u, 0xFFFFFFFFu, D3D12XBOX_WAIT_VALUE_OP_EQUAL);
    fx.cl->Wait64BitValueX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&wait64_cell),
        0x0102030405060708ull, 0xFFFFFFFFFFFFFFFFull, D3D12XBOX_WAIT_VALUE_OP_EQUAL);
    fx.cl->Write32BitValueX(gpu_w32->GetGPUVirtualAddress(), 0xABCD1234u, D3D12XBOX_WRITE_VALUE_FLAG_NONE);
    fx.cl->Write32BitValueX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(&cpu_cl_marker),
        0x44332211u, D3D12XBOX_WRITE_VALUE_FLAG_MARKER_OUT);
    fx.cl->FlushGpuCacheRange(D3D12XBOX_FLUSH_ALL, gpu_w32->GetGPUVirtualAddress(), 256u);
    fx.cl->KickoffX();

    // Seed pred_dst[0..2] with 0x11111111
    fx.cl->CopyBufferRegion(pred_dst.Get(), 0u, src_val.Get(), 0u, 12u);

    // Slot 0: pred=0, EQUAL_ZERO -> MUST SKIP -> stays 0x11111111
    fx.cl->SetPredicationX(pred_buf->GetGPUVirtualAddress() + 0u, D3D12_PREDICATION_OP_EQUAL_ZERO, 0u);
    fx.cl->CopyBufferRegion(pred_dst.Get(), 0u, src_val.Get(), 12u, 4u);

    // Slot 1: pred=1, EQUAL_ZERO -> MUST EXECUTE -> becomes 0xDEADBEEF
    fx.cl->SetPredicationX(pred_buf->GetGPUVirtualAddress() + 8u, D3D12_PREDICATION_OP_EQUAL_ZERO, 0u);
    fx.cl->CopyBufferRegion(pred_dst.Get(), 4u, src_val.Get(), 12u, 4u);

    // Slot 2: pred=0, EQUAL_ZERO + INVERT (= NOT_EQUAL_ZERO) -> MUST EXECUTE -> becomes 0xDEADBEEF
    fx.cl->SetPredicationX(
        pred_buf->GetGPUVirtualAddress() + 0u,
        D3D12_PREDICATION_OP_EQUAL_ZERO,
        D3D12XBOX_PREDICATION_FLAG_INVERT);
    fx.cl->CopyBufferRegion(pred_dst.Get(), 8u, src_val.Get(), 12u, 4u);

    // Clear predication
    fx.cl->SetPredicationX(0ull, D3D12_PREDICATION_OP_EQUAL_ZERO, 0u);

    D3D12_RESOURCE_BARRIER to_src[2]{};
    to_src[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_src[0].Transition.pResource = gpu_w32.Get();
    to_src[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_src[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_src[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_src[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_src[1].Transition.pResource = pred_dst.Get();
    to_src[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_src[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_src[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fx.cl->ResourceBarrier(2u, to_src);

    fx.cl->CopyBufferRegion(rb.Get(), 0u, gpu_w32.Get(), 0u, 4u);
    fx.cl->CopyBufferRegion(rb.Get(), 16u, pred_dst.Get(), 0u, 12u);
    fx.SubmitAndWait();
    fx.queue->KickoffX();

    OS_REQUIRE(cpu_cl_marker == 0x44332211u);
    void* rb_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &rb_ptr)));
    const auto* rbu = static_cast<const uint32_t*>(rb_ptr);
    OS_REQUIRE(rbu[0] == 0xABCD1234u);
    OS_REQUIRE(rbu[4] == 0x11111111u);
    OS_REQUIRE(rbu[5] == 0xDEADBEEFu);
    OS_REQUIRE(rbu[6] == 0xDEADBEEFu);
    rb->Unmap(0u, nullptr);

    // 3) DispatchMesh(2, 3, 2) LIVE 3D Mesh Shader on MeshShaderTier>=1 GPUs (y > 1, z > 1),
    //    and graceful 3D group tracking on non-MeshShader Win10 inbox WARP.
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
    const bool hw_mesh =
        SUCCEEDED(fx.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7))) &&
        opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
    if (std::strcmp(tag, "HW_GPU") == 0) {
        OS_REQUIRE(hw_mesh);
    }

    if (hw_mesh) {
    const char* ms_hlsl =
        "struct VOut { float4 pos : SV_Position; float4 col : COLOR0; };\n"
        "[outputtopology(\"triangle\")]\n"
        "[numthreads(1, 1, 1)]\n"
        "void main(uint3 gid : SV_GroupID, out vertices VOut verts[3], out indices uint3 tris[1]) {\n"
        "    SetMeshOutputCounts(3, 1);\n"
        "    // 32x32 RT: place each (gx, gy, gz) in 2x3x2 at cell (gx*4 + gz*8, gy*4), covering a 4x4 region\n"
        "    float px = (float)(gid.x * 4u + gid.z * 8u);\n"
        "    float py = (float)(gid.y * 4u);\n"
        "    float x0 = (px / 16.0f) - 1.0f;\n"
        "    float x1 = ((px + 4.0f) / 16.0f) - 1.0f;\n"
        "    float y0 = 1.0f - (py / 16.0f);\n"
        "    float y1 = 1.0f - ((py + 4.0f) / 16.0f);\n"
        "    float4 c = float4((float)(gid.x * 100u + 50u) / 255.0f,\n"
        "                      (float)(gid.y * 80u + 40u) / 255.0f,\n"
        "                      (float)(gid.z * 100u + 60u) / 255.0f, 1.0f);\n"
        "    verts[0].pos = float4(x0, y0, 0.0f, 1.0f); verts[0].col = c;\n"
        "    verts[1].pos = float4(x1, y0, 0.0f, 1.0f); verts[1].col = c;\n"
        "    verts[2].pos = float4(x0, y1, 0.0f, 1.0f); verts[2].col = c;\n"
        "    tris[0] = uint3(0, 1, 2);\n"
        "}\n";
    const char* ps_hlsl =
        "struct VOut { float4 pos : SV_Position; float4 col : COLOR0; };\n"
        "float4 main(VOut i) : SV_Target { return i.col; }\n";

    std::vector<uint8_t> ms_dxil, ps_dxil;
    OS_REQUIRE(CompileShaderWithDxc(ms_hlsl, "main", "ms_6_5", ms_dxil));
    OS_REQUIRE(CompileShaderWithDxc(ps_hlsl, "main", "ps_6_5", ps_dxil));

    D3D12_ROOT_SIGNATURE_DESC empty_rsd{};
    ComPtr<ID3DBlob> rs_blob, err_blob;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &empty_rsd, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), err_blob.Put())));
    ComPtr<ID3D12RootSignature> mesh_rs;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        IID_ID3D12RootSignature, mesh_rs.PutVoid())));

    struct alignas(void*) MeshPsoStream {
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; ID3D12RootSignature* rs; } root;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_SHADER_BYTECODE bc; } ms;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_SHADER_BYTECODE bc; } ps;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_RT_FORMAT_ARRAY rtvs; } rtv;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_RASTERIZER_DESC rast; } rast;
    } stream{};
    stream.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    stream.root.rs = mesh_rs.Get();
    stream.ms.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
    stream.ms.bc = {ms_dxil.data(), ms_dxil.size()};
    stream.ps.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
    stream.ps.bc = {ps_dxil.data(), ps_dxil.size()};
    stream.rtv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    stream.rtv.rtvs.NumRenderTargets = 1u;
    stream.rtv.rtvs.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    stream.rast.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
    stream.rast.rast.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rast.rast.CullMode = D3D12_CULL_MODE_NONE;
    stream.rast.rast.DepthClipEnable = TRUE;

    D3D12_PIPELINE_STATE_STREAM_DESC sdesc{};
    sdesc.SizeInBytes = sizeof(stream);
    sdesc.pPipelineStateSubobjectStream = &stream;
    ComPtr<ID3D12PipelineState> mesh_pso;
    OS_REQUIRE(SUCCEEDED(fx.device->CreatePipelineState(
        &sdesc, IID_ID3D12PipelineState, mesh_pso.PutVoid())));

    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = 32u;
    rt_desc.Height = 32u;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
        IID_ID3D12Resource, rt.PutVoid())));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = 1u;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(
        &rtv_hd, IID_ID3D12DescriptorHeap, rtv_heap.PutVoid())));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    fx.device->CreateRenderTargetView(rt.Get(), nullptr, rtv_h);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rt_fp{};
    UINT rt_rows = 0u;
    UINT64 rt_row_bytes = 0ull, rt_total = 0ull;
    fx.device->GetCopyableFootprints(&rt_desc, 0u, 1u, 0ull, &rt_fp, &rt_rows, &rt_row_bytes, &rt_total);
    auto rt_rb = fx.CreateBuffer(D3D12_HEAP_TYPE_READBACK, AlignUp(rt_total, 256ull), D3D12_RESOURCE_STATE_COPY_DEST);

    fx.ResetList();
    const float clear_col[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    fx.cl->ClearRenderTargetView(rtv_h, clear_col, 0u, nullptr);
    fx.cl->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, 32, 32};
    fx.cl->RSSetViewports(1u, &vp);
    fx.cl->RSSetScissorRects(1u, &sc);
    fx.cl->SetGraphicsRootSignature(mesh_rs.Get());
    fx.cl->SetPipelineState(mesh_pso.Get());
    fx.cl->DispatchMesh(2u, 3u, 2u);

    D3D12_RESOURCE_BARRIER rt_to_src{};
    rt_to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rt_to_src.Transition.pResource = rt.Get();
    rt_to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rt_to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fx.cl->ResourceBarrier(1u, &rt_to_src);

    D3D12_TEXTURE_COPY_LOCATION dloc{};
    dloc.pResource = rt_rb.Get();
    dloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dloc.PlacedFootprint = rt_fp;
    D3D12_TEXTURE_COPY_LOCATION sloc{};
    sloc.pResource = rt.Get();
    sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sloc.SubresourceIndex = 0u;
    fx.cl->CopyTextureRegion(&dloc, 0u, 0u, 0u, &sloc, nullptr);
    fx.SubmitAndWait();

    void* rt_ptr = nullptr;
    OS_REQUIRE(SUCCEEDED(rt_rb->Map(0u, nullptr, &rt_ptr)));
    const auto* pixels = static_cast<const uint8_t*>(rt_ptr);
    for (uint32_t gz = 0u; gz < 2u; ++gz) {
        for (uint32_t gy = 0u; gy < 3u; ++gy) {
            for (uint32_t gx = 0u; gx < 2u; ++gx) {
                const uint32_t px = gx * 4u + gz * 8u + 1u;
                const uint32_t py = gy * 4u + 1u;
                const uint8_t* p = pixels + static_cast<size_t>(py) * rt_fp.Footprint.RowPitch + px * 4u;
                const int er = static_cast<int>(gx * 100u + 50u);
                const int eg = static_cast<int>(gy * 80u + 40u);
                const int eb = static_cast<int>(gz * 100u + 60u);
                OS_REQUIRE(std::abs(static_cast<int>(p[0]) - er) <= 2);
                OS_REQUIRE(std::abs(static_cast<int>(p[1]) - eg) <= 2);
                OS_REQUIRE(std::abs(static_cast<int>(p[2]) - eb) <= 2);
                OS_REQUIRE(p[3] == 255u);
            }
        }
    }
    rt_rb->Unmap(0u, nullptr);
    } else {
        fx.ResetList();
        fx.cl->DispatchMesh(2u, 3u, 2u);
        fx.SubmitAndWait();
    }

    D3D12XBOX_AAA_WIRING_STATS stats{};
    OS_REQUIRE(SUCCEEDED(fx.aaa->GetAaaWiringStats(&stats)));
    OS_REQUIRE(stats.cmdlist_kickoffs >= 1u);
    OS_REQUIRE(stats.cmdlist_write32_calls >= 2u);
    OS_REQUIRE(stats.cmdlist_predicationx_calls >= 4u);
    OS_REQUIRE(stats.cmdlist_flush_cache_calls >= 1u);
    OS_REQUIRE(stats.cmdlist_dispatch_mesh_calls >= 1u);
    OS_REQUIRE(stats.last_mesh_x == 2u);
    OS_REQUIRE(stats.last_mesh_y == 3u);
    OS_REQUIRE(stats.last_mesh_z == 2u);
    OS_REQUIRE(stats.total_mesh_groups >= 12u);
    std::printf("[%s] PASS GXDK Extensions + DispatchMesh(2,3,2)=12 groups LIVE\n", tag);
}

// =============================================================================
// Test 7: DeviceX Host-Like Extensions (CheckDriverMatchingIdentifier,
//         EnumerateMetaCommands/CreateMetaCommand, RemoveDevice propagation)
// =============================================================================
void TestDeviceHostExtensionsAndRemoveDevice(GpuFixture& fx, bool prefer_warp, const char* tag) {
    // 1) CheckDriverMatchingIdentifier
    D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER sw_id{};
    const GUID kSwGuid = {0x4F534431u, 0x3258u, 0x4458u, {0x52u, 0x53u, 0x57u, 0x41u, 0x53u, 0x00u, 0x00u, 0x01u}};
    sw_id.DriverOpaqueGUID = kSwGuid;
    const auto sw_status = fx.device->CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, &sw_id);
    OS_REQUIRE(sw_status == D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE);

    D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER rand_id{};
    rand_id.DriverOpaqueGUID.Data1 = 0x11223344u;
    const auto rand_status = fx.device->CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, &rand_id);
    OS_REQUIRE(rand_status == D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED ||
               rand_status == D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION ||
               rand_status == D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE ||
               rand_status == D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE);
    const auto null_status = fx.device->CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, nullptr);
    OS_REQUIRE(null_status == D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION ||
               null_status == D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);

    // 2) EnumerateMetaCommands / CreateMetaCommand
    UINT mc_count = 0u;
    OS_REQUIRE(SUCCEEDED(fx.device->EnumerateMetaCommands(&mc_count, nullptr)));
    if (mc_count > 0u) {
        std::vector<D3D12_META_COMMAND_DESC> descs(mc_count);
        UINT queried = mc_count;
        OS_REQUIRE(SUCCEEDED(fx.device->EnumerateMetaCommands(&queried, descs.data())));
    }
    const GUID fake_mc = {0x99887766u, 0x5544u, 0x3322u, {1, 2, 3, 4, 5, 6, 7, 8}};
    void* mc_out = reinterpret_cast<void*>(0x1);
    const HRESULT mc_hr = fx.device->CreateMetaCommand(
        fake_mc, 0u, nullptr, 0u, IID_ID3D12MetaCommand, &mc_out);
    OS_REQUIRE(FAILED(mc_hr));
    OS_REQUIRE(mc_out == nullptr);

    // 3) RemoveDevice + GetDeviceRemovedReason + PresentX error propagation on a disposable device
    GpuFixture disp;
    disp.Init(prefer_warp);
    auto dummy_rt = disp.CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, 256u, D3D12_RESOURCE_STATE_COMMON);
    OS_REQUIRE(disp.device->GetDeviceRemovedReason() == S_OK);
    disp.device->RemoveDevice();
    const HRESULT reason = disp.device->GetDeviceRemovedReason();
    OS_REQUIRE(FAILED(reason));

    D3D12XBOX_AAA_WIRING_STATS dstats{};
    OS_REQUIRE(SUCCEEDED(disp.aaa->GetAaaWiringStats(&dstats)));
    OS_REQUIRE(FAILED(static_cast<HRESULT>(dstats.device_removed_reason)));

    ID3D12Resource* dummy_res[1] = {dummy_rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1u;
    plane.ppResources = dummy_res;
    D3D12XBOX_PRESENT_PARAMETERS pparams{};
    const HRESULT present_hr = disp.queue->PresentX(1u, &plane, &pparams);
    OS_REQUIRE(FAILED(present_hr));

    std::printf("[%s] PASS DeviceX Host Extensions + RemoveDevice propagation (mc=%u, reason=0x%08X)\n",
                tag, mc_count, static_cast<unsigned>(reason));
}

// =============================================================================
// Test 8: PresentX Frame Pacing (SetFrameIntervalX + OPENSCARLETT_FCAP +
//         SetFrameCapFps) & Alt+Enter Borderless Fullscreen Toggle LIVE
// =============================================================================
LRESULT CALLBACK S12WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateHiddenTestWindow() {
    static const wchar_t* kCls = L"OpenScarlettS12AaaWindowClass";
    static bool s_reg = false;
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &S12WndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = kCls;
        OS_REQUIRE(RegisterClassExW(&wc) != 0);
        s_reg = true;
    }
    HWND hwnd = CreateWindowExW(
        0u, kCls, L"OpenScarlett S12 PresentX Pacing",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 240,
        nullptr, nullptr, hinst, nullptr);
    OS_REQUIRE(hwnd != nullptr);
    return hwnd;
}

void TestPresentXPacingAndAltEnterLive(GpuFixture& fx, const char* tag) {
    HWND hwnd = CreateHiddenTestWindow();
    OS_REQUIRE(SUCCEEDED(fx.present_capture->SetOutputWindow(hwnd)));

    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = 128u;
    rt_desc.Height = 128u;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> frame_tex;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_ID3D12Resource, frame_tex.PutVoid())));

    // 1) SetFrameIntervalX(20000 us = 50 FPS)
    OS_REQUIRE(SUCCEEDED(fx.device->SetFrameIntervalX(nullptr, 20000u, 2u, 0u)));
    D3D12XBOX_PRESENT_PACING_STATE ps{};
    OS_REQUIRE(SUCCEEDED(fx.present_pacing->GetPresentPacingState(&ps)));
    OS_REQUIRE(ps.set_frame_interval_us == 20000u);
    OS_REQUIRE(ps.effective_frame_interval_us == 20000u);

    // 2) Override with OPENSCARLETT_FCAP=40 (25ms interval) and verify live pacing timing
    SetEnvironmentVariableA("OPENSCARLETT_FCAP", "40");
    ID3D12Resource* frame_res[1] = {frame_tex.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = 1u;
    plane.ResourceCount = 1u;
    plane.ppResources = frame_res;
    D3D12XBOX_PRESENT_PARAMETERS pparams{};

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        OS_REQUIRE(SUCCEEDED(fx.queue->PresentX(1u, &plane, &pparams)));
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    OS_REQUIRE(SUCCEEDED(fx.present_pacing->GetPresentPacingState(&ps)));
    OS_REQUIRE(ps.effective_frame_interval_us == 25000u);
    OS_REQUIRE(ps.paced_frame_count >= 2u);
    OS_REQUIRE(elapsed_ms >= 35);

    // 3) Explicit SetFrameCapFps override takes precedence over env
    OS_REQUIRE(SUCCEEDED(fx.present_pacing->SetFrameCapFps(50u, TRUE)));
    OS_REQUIRE(SUCCEEDED(fx.present_pacing->GetPresentPacingState(&ps)));
    OS_REQUIRE(ps.fcap_override_active == 1u);
    OS_REQUIRE(ps.fcap_override_fps == 50u);
    OS_REQUIRE(ps.effective_frame_interval_us == 20000u);

    // Clear overrides
    OS_REQUIRE(SUCCEEDED(fx.present_pacing->SetFrameCapFps(0u, FALSE)));
    SetEnvironmentVariableA("OPENSCARLETT_FCAP", nullptr);

    // 4) Alt+Enter borderless fullscreen toggle via WM_SYSKEYDOWN (VK_RETURN + bit 29)
    D3D12XBOX_PRESENT_WINDOW_STATE ws{};
    OS_REQUIRE(SUCCEEDED(fx.present_window->GetPresentWindowState(&ws)));
    OS_REQUIRE(ws.is_fullscreen == 0u);

    const LPARAM kAltDownLParam = static_cast<LPARAM>(1u << 29);
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, kAltDownLParam);
    OS_REQUIRE(SUCCEEDED(fx.present_window->GetPresentWindowState(&ws)));
    OS_REQUIRE(ws.is_fullscreen == 1u);

    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, kAltDownLParam);
    OS_REQUIRE(SUCCEEDED(fx.present_window->GetPresentWindowState(&ws)));
    OS_REQUIRE(ws.is_fullscreen == 0u);

    OS_REQUIRE(SUCCEEDED(fx.present_pacing->GetPresentPacingState(&ps)));
    OS_REQUIRE(ps.alt_enter_toggle_count == 2u);

    OS_REQUIRE(SUCCEEDED(fx.present_capture->SetOutputWindow(nullptr)));
    DestroyWindow(hwnd);
    std::printf("[%s] PASS PresentX Pacing (3 frames @ 40fps = %lld ms) + Alt+Enter toggle (count=%llu)\n",
                tag, static_cast<long long>(elapsed_ms),
                static_cast<unsigned long long>(ps.alt_enter_toggle_count));
}

void RunAllTestsOnBackend(bool prefer_warp, const char* tag) {
    std::printf("\n=== Sprint 12 D3D12X AAA Live Wiring Suite [%s] ===\n", tag);
    GpuFixture fx;
    fx.Init(prefer_warp);

    TestSamplerFeedbackLive(fx, tag);
    TestQueryAccumulatorLive(fx, tag);
    TestTitleVaMirrorLive(fx, tag);
    TestDescriptorSgprBridgeLive(fx, tag);
    TestXgGpuUntilerLive(fx, tag);
    TestGxdkExtensionsAndMesh3DLive(fx, tag);
    TestPresentXPacingAndAltEnterLive(fx, tag);
    // Run RemoveDevice last per adapter because D3D12CreateDevice shares a per-adapter singleton.
    TestDeviceHostExtensionsAndRemoveDevice(fx, prefer_warp, tag);
}

}  // namespace

int main() {
    openscarlett::D3D12AdapterInfo hw_info{};
    const bool has_hw = HasHardwareD3D12Gpu(hw_info);
    if (has_hw) {
        std::printf("Detected hardware D3D12 GPU: %s\n", hw_info.description.c_str());
        RunAllTestsOnBackend(false, "HW_GPU");
    } else {
        std::printf("No hardware D3D12 GPU detected; running WARP suite only.\n");
    }

    RunAllTestsOnBackend(true, "WARP");
    std::printf("\nALL Sprint 12 D3D12X AAA Live Wiring tests PASSED!\n");
    return 0;
}
