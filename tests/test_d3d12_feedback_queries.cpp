#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/query_accumulator_x.h"
#include "openscarlett/gpu/sampler_feedback_x.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>

#if defined(_MSC_VER)
#define OS_TEST_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_TEST_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_TEST_UUIDOF(T) IID_##T
#endif
#endif

using namespace openscarlett;
using namespace openscarlett::d3d12_xs;

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, const std::string& detail = {}) {
    if (!cond) {
        ++g_failures;
        std::cerr << "[FAIL] " << expr;
        if (!detail.empty()) std::cerr << " :: " << detail;
        std::cerr << "\n";
    }
}

#define EXPECT_TRUE(cond)            \
    do {                             \
        Check((cond), #cond);        \
    } while (false)

#define EXPECT_OK(expr)                                 \
    do {                                                \
        const openscarlett::Status _os_st = (expr);     \
        Check(_os_st.ok(), #expr, _os_st.message);      \
    } while (false)

#if defined(_WIN32)

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

using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*,
    D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob**,
    ID3DBlob**);

struct GpuContext {
    D3D12AdapterInfo info{};
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0ull;

    ~GpuContext() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool Init(bool prefer_warp) {
        D3D12AdapterSelectionOptions opts{};
        opts.prefer_warp = prefer_warp;
        opts.allow_warp_fallback = true;
        opts.min_feature_level = 0xB000u;  // D3D_FEATURE_LEVEL_11_0
        Status st = SelectD3D12AdapterAndCreateDevice(opts, device.Put(), adapter.Put(), &info);
        if (!st.ok() || !device) {
            std::cerr << "[FAIL] SelectD3D12AdapterAndCreateDevice(warp=" << prefer_warp
                      << "): " << st.message << "\n";
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, OS_TEST_UUIDOF(ID3D12CommandQueue), queue.PutVoid())) ||
            !queue) {
            return false;
        }
        if (FAILED(device->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_TEST_UUIDOF(ID3D12Fence), fence.PutVoid())) ||
            !fence) {
            return false;
        }
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    bool SubmitAndWait(ID3D12CommandList* const* lists, UINT count) {
        queue->ExecuteCommandLists(count, lists);
        const uint64_t target = ++fence_val;
        if (FAILED(queue->Signal(fence.Get(), target))) return false;
        if (fence->GetCompletedValue() < target) {
            if (FAILED(fence->SetEventOnCompletion(target, fence_event))) return false;
            if (WaitForSingleObject(fence_event, 15000u) != WAIT_OBJECT_0) return false;
        }
        return true;
    }

    bool CreateBuffer(
        D3D12_HEAP_TYPE heap_type,
        uint64_t bytes,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initial_state,
        ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        hp.CreationNodeMask = 1u;
        hp.VisibleNodeMask = 1u;

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

        return SUCCEEDED(device->CreateCommittedResource(
                   &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr,
                   OS_TEST_UUIDOF(ID3D12Resource), out.PutVoid())) &&
               out;
    }

    bool ReadbackBuffer(
        ID3D12Resource* src_default_buf,
        D3D12_RESOURCE_STATES src_state,
        uint64_t bytes,
        std::vector<uint8_t>& out_bytes) {
        out_bytes.assign(static_cast<size_t>(bytes), 0u);
        ComPtr<ID3D12Resource> rb;
        if (!CreateBuffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, rb)) {
            return false;
        }
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cmd;
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, OS_TEST_UUIDOF(ID3D12CommandAllocator), alloc.PutVoid()))) {
            return false;
        }
        if (FAILED(device->CreateCommandList(
                0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                OS_TEST_UUIDOF(ID3D12GraphicsCommandList), cmd.PutVoid()))) {
            return false;
        }
        if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = src_default_buf;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = src_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmd->ResourceBarrier(1u, &b);
        }
        cmd->CopyBufferRegion(rb.Get(), 0ull, src_default_buf, 0ull, bytes);
        if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = src_default_buf;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = src_state;
            cmd->ResourceBarrier(1u, &b);
        }
        if (FAILED(cmd->Close())) return false;
        ID3D12CommandList* list = cmd.Get();
        if (!SubmitAndWait(&list, 1u)) return false;

        void* mapped = nullptr;
        D3D12_RANGE rr{0u, static_cast<SIZE_T>(bytes)};
        if (FAILED(rb->Map(0u, &rr, &mapped)) || !mapped) return false;
        std::memcpy(out_bytes.data(), mapped, static_cast<size_t>(bytes));
        D3D12_RANGE wn{0u, 0u};
        rb->Unmap(0u, &wn);
        return true;
    }
};

const char* FeedbackTierName(SamplerFeedbackTier t) noexcept {
    switch (t) {
        case SamplerFeedbackTier::Tier1_0: return "Tier1.0";
        case SamplerFeedbackTier::Tier0_9: return "Tier0.9";
        default: return "NotSupported";
    }
}

// ---------------------------------------------------------------------------
// 1. Sampler Feedback Map Tests (MinMipOpaque + MipRegionUsedOpaque)
// ---------------------------------------------------------------------------
void RunSamplerFeedbackTests(GpuContext& ctx, const std::string& label, bool force_fallback) {
    SamplerFeedbackManager mgr;
    EXPECT_OK(mgr.Initialize(ctx.device.Get()));
    EXPECT_TRUE(mgr.IsInitialized());

    // Create paired target Texture2D (256x256, 5 mips, array_size=2).
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 256u;
    target_desc.Height = 256u;
    target_desc.DepthOrArraySize = 2u;
    target_desc.MipLevels = 5u;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1u;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> target_tex;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, OS_TEST_UUIDOF(ID3D12Resource), target_tex.PutVoid())));

    // Create CPU UAV descriptor heap to verify sampler feedback view preservation & copy.
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4u;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> cpu_heap;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateDescriptorHeap(
        &hd, OS_TEST_UUIDOF(ID3D12DescriptorHeap), cpu_heap.PutVoid())));
    const UINT inc = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE h0 = cpu_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE h1{h0.ptr + inc};
    const D3D12_CPU_DESCRIPTOR_HANDLE h2{h0.ptr + 2u * inc};

    // --- 1A. MIN_MIP_OPAQUE ---
    SamplerFeedbackMapDesc min_desc{};
    min_desc.kind = SamplerFeedbackKind::MinMipOpaque;
    min_desc.target_width = 256u;
    min_desc.target_height = 256u;
    min_desc.target_array_size = 2u;
    min_desc.target_mip_levels = 5u;
    min_desc.mip_region = {16u, 16u, 1u};  // 16x16 tiles per slice
    min_desc.force_software_fallback = force_fallback;

    ComPtr<ID3D12Resource> min_fb;
    SamplerFeedbackMapInfo min_info{};
    EXPECT_OK(mgr.CreateFeedbackMap(min_desc, target_tex.Get(), min_fb.Put(), &min_info));
    EXPECT_TRUE(min_fb.Get() != nullptr);
    EXPECT_TRUE(min_info.map_tiles_x == 16u && min_info.map_tiles_y == 16u);
    EXPECT_TRUE(min_info.paired_target_resource == target_tex.Get());
    if (force_fallback) {
        EXPECT_TRUE(!min_info.using_native_hardware);
    }

    // View preserve & copy verification.
    SamplerFeedbackViewRecord view_rec{};
    EXPECT_OK(mgr.CreateSamplerFeedbackUnorderedAccessView(target_tex.Get(), min_fb.Get(), h0, &view_rec));
    EXPECT_TRUE(view_rec.feedback_resource == min_fb.Get());
    EXPECT_TRUE(view_rec.paired_target_resource == target_tex.Get());
    EXPECT_TRUE(view_rec.is_native_view == min_info.using_native_hardware);

    SamplerFeedbackViewRecord looked_up{};
    EXPECT_TRUE(mgr.LookupPreservedView(h0, looked_up));
    EXPECT_TRUE(looked_up.map_id == min_info.map_id);
    mgr.CopyPreservedView(h1, h0);
    SamplerFeedbackViewRecord copied_up{};
    EXPECT_TRUE(mgr.LookupPreservedView(h1, copied_up));
    EXPECT_TRUE(copied_up.map_id == min_info.map_id && copied_up.dest_cpu_descriptor_ptr == h1.ptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, OS_TEST_UUIDOF(ID3D12CommandAllocator), alloc.PutVoid())));
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        OS_TEST_UUIDOF(ID3D12GraphicsCommandList), cmd.PutVoid())));

    // Clear -> expect every tile to read back as 0xFF (unreferenced).
    EXPECT_OK(mgr.ClearSamplerFeedbackMap(cmd.Get(), min_fb.Get()));
    EXPECT_TRUE(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* l = cmd.Get();
    EXPECT_TRUE(ctx.SubmitAndWait(&l, 1u));

    SamplerFeedbackMinMipReadback rb_clear{};
    EXPECT_OK(mgr.ResolveAndReadbackMinMip(ctx.queue.Get(), min_fb.Get(), rb_clear));
    EXPECT_TRUE(rb_clear.tiles_x == 16u && rb_clear.tiles_y == 16u && rb_clear.array_size == 2u);
    EXPECT_TRUE(rb_clear.min_mip_tiles.size() == 512u);
    bool all_ff = true;
    for (uint8_t v : rb_clear.min_mip_tiles) {
        if (v != kSamplerFeedbackUnreferencedMinMip) {
            all_ff = false;
            break;
        }
    }
    Check(all_ff, "MinMip clear resets all 512 tiles to 0xFF", label);

    // Record overlapping writes (testing min-reduction semantics) across both array slices.
    // Note: on native Tier 0.9/1.0 hardware, writing mip M marks the entire mip-M region
    // (2^M x 2^M base tiles), so untouched checks use tiles outside all written mip regions.
    EXPECT_TRUE(SUCCEEDED(alloc->Reset()));
    EXPECT_TRUE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
    const std::vector<SamplerFeedbackSampleWrite> min_samples = {
        {2u, 4u, 0u, 4u},
        {2u, 4u, 0u, 2u},   // min(4, 2) => 2 (mip-2 region [0..3] x [4..7])
        {2u, 4u, 0u, 3u},   // min(2, 3) => 2
        {0u, 0u, 0u, 0u},
        {15u, 15u, 1u, 1u}, // mip-1 region [14..15] x [14..15]
        {14u, 14u, 1u, 0u}, // mip-0 tile (14, 14)
    };
    EXPECT_OK(mgr.RecordFeedbackSampleWrites(cmd.Get(), min_fb.Get(), min_samples));
    EXPECT_TRUE(SUCCEEDED(cmd->Close()));
    EXPECT_TRUE(ctx.SubmitAndWait(&l, 1u));

    SamplerFeedbackMinMipReadback rb_written{};
    EXPECT_OK(mgr.ResolveAndReadbackMinMip(ctx.queue.Get(), min_fb.Get(), rb_written));
    auto check_tile = [&](uint32_t x, uint32_t y, uint32_t s, uint8_t expected) {
        const uint8_t got = rb_written.At(x, y, s);
        std::ostringstream oss;
        oss << label << " tile(" << x << "," << y << ",s=" << s << ") got=0x"
            << std::hex << static_cast<uint32_t>(got) << " expected=0x"
            << static_cast<uint32_t>(expected);
        Check(got == expected, "rb_written tile value", oss.str());
    };
    check_tile(2u, 4u, 0u, 2u);
    check_tile(0u, 0u, 0u, 0u);
    check_tile(15u, 15u, 1u, 1u);
    check_tile(14u, 14u, 1u, 0u);
    check_tile(12u, 12u, 0u, kSamplerFeedbackUnreferencedMinMip);
    check_tile(1u, 1u, 1u, kSamplerFeedbackUnreferencedMinMip);

    // Re-clear -> verify 0xFF restored across all tiles.
    EXPECT_TRUE(SUCCEEDED(alloc->Reset()));
    EXPECT_TRUE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
    EXPECT_OK(mgr.ClearSamplerFeedbackMap(cmd.Get(), min_fb.Get()));
    EXPECT_TRUE(SUCCEEDED(cmd->Close()));
    EXPECT_TRUE(ctx.SubmitAndWait(&l, 1u));

    SamplerFeedbackMinMipReadback rb_reclear{};
    EXPECT_OK(mgr.ResolveAndReadbackMinMip(ctx.queue.Get(), min_fb.Get(), rb_reclear));
    EXPECT_TRUE(rb_reclear.At(2u, 4u, 0u) == kSamplerFeedbackUnreferencedMinMip);
    EXPECT_TRUE(rb_reclear.At(15u, 15u, 1u) == kSamplerFeedbackUnreferencedMinMip);

    // --- 1B. MIP_REGION_USED_OPAQUE ---
    // Create a matching 1-slice 256x256x5 target texture (D3D12 requires paired target array size to match).
    D3D12_RESOURCE_DESC mru_target_desc = target_desc;
    mru_target_desc.DepthOrArraySize = 1u;
    ComPtr<ID3D12Resource> mru_target_tex;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &mru_target_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, OS_TEST_UUIDOF(ID3D12Resource), mru_target_tex.PutVoid())));

    SamplerFeedbackMapDesc mru_desc{};
    mru_desc.kind = SamplerFeedbackKind::MipRegionUsedOpaque;
    mru_desc.target_width = 256u;
    mru_desc.target_height = 256u;
    mru_desc.target_array_size = 1u;
    mru_desc.target_mip_levels = 5u;
    mru_desc.mip_region = {32u, 32u, 1u};  // 8x8 base tiles
    mru_desc.force_software_fallback = force_fallback;

    ComPtr<ID3D12Resource> mru_fb;
    SamplerFeedbackMapInfo mru_info{};
    EXPECT_OK(mgr.CreateFeedbackMap(mru_desc, mru_target_tex.Get(), mru_fb.Put(), &mru_info));
    EXPECT_TRUE(mru_fb.Get() != nullptr);
    EXPECT_TRUE(mru_info.map_tiles_x == 8u && mru_info.map_tiles_y == 8u);
    EXPECT_OK(mgr.CreateSamplerFeedbackUnorderedAccessView(mru_target_tex.Get(), mru_fb.Get(), h2));

    EXPECT_TRUE(SUCCEEDED(alloc->Reset()));
    EXPECT_TRUE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
    EXPECT_OK(mgr.ClearSamplerFeedbackMap(cmd.Get(), mru_fb.Get()));
    EXPECT_TRUE(SUCCEEDED(cmd->Close()));
    EXPECT_TRUE(ctx.SubmitAndWait(&l, 1u));

    SamplerFeedbackMipRegionUsedReadback mru_clear_rb{};
    EXPECT_OK(mgr.ResolveAndReadbackMipRegionUsed(ctx.queue.Get(), mru_fb.Get(), mru_clear_rb));
    EXPECT_TRUE(mru_clear_rb.base_tiles_x == 8u && mru_clear_rb.base_tiles_y == 8u);
    EXPECT_TRUE(mru_clear_rb.mip_planes.size() == 5u);
    bool all_zero = true;
    for (uint32_t m : mru_clear_rb.tile_mip_bitmasks) {
        if (m != 0u) {
            all_zero = false;
            break;
        }
    }
    Check(all_zero, "MipRegionUsed clear resets all bitmasks to 0", label);

    EXPECT_TRUE(SUCCEEDED(alloc->Reset()));
    EXPECT_TRUE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
    const std::vector<SamplerFeedbackSampleWrite> mru_samples = {
        {1u, 2u, 0u, 0u},
        {1u, 2u, 0u, 3u},
        {6u, 5u, 0u, 2u},
    };
    EXPECT_OK(mgr.RecordFeedbackSampleWrites(cmd.Get(), mru_fb.Get(), mru_samples));
    EXPECT_TRUE(SUCCEEDED(cmd->Close()));
    EXPECT_TRUE(ctx.SubmitAndWait(&l, 1u));

    SamplerFeedbackMipRegionUsedReadback mru_written{};
    EXPECT_OK(mgr.ResolveAndReadbackMipRegionUsed(ctx.queue.Get(), mru_fb.Get(), mru_written));
    EXPECT_TRUE(mru_written.BitmaskAt(1u, 2u) == ((1u << 0u) | (1u << 3u)));
    EXPECT_TRUE(mru_written.BitmaskAt(6u, 5u) == (1u << 2u));
    EXPECT_TRUE(mru_written.BitmaskAt(0u, 0u) == 0u);
    if (mru_written.mip_planes.size() >= 4u) {
        EXPECT_TRUE(mru_written.mip_planes[0].At(1u, 2u) == kSamplerFeedbackRegionUsed);
        EXPECT_TRUE(mru_written.mip_planes[0].At(0u, 0u) == kSamplerFeedbackRegionUnused);
        // For mip 2 (64x64 / 32x32 => 2x2 tiles), base tile (6, 5) maps to (6>>2=1, 5>>2=1).
        EXPECT_TRUE(mru_written.mip_planes[2].tiles_x == 2u && mru_written.mip_planes[2].tiles_y == 2u);
        EXPECT_TRUE(mru_written.mip_planes[2].At(1u, 1u) == kSamplerFeedbackRegionUsed);
        EXPECT_TRUE(mru_written.mip_planes[2].At(0u, 0u) == kSamplerFeedbackRegionUnused);
        // For mip 3 (32x32 / 32x32 => 1x1 tile), base tile (1, 2) maps to (0, 0).
        EXPECT_TRUE(mru_written.mip_planes[3].At(0u, 0u) == kSamplerFeedbackRegionUsed);
    } else {
        Check(false, "mru_written.mip_planes.size() >= 4u", label);
    }

    const SamplerFeedbackManagerStats stats = mgr.GetStats();
    EXPECT_TRUE(stats.active_maps == 2u);
    EXPECT_TRUE(stats.preserved_views == 3u);
    EXPECT_TRUE(stats.total_clears == 3ull);
    EXPECT_TRUE(stats.total_resolves == 5ull);

    mgr.NotifyResourceDestroyed(min_fb.Get());
    EXPECT_OK(mgr.UnregisterFeedbackMap(mru_fb.Get()));
    EXPECT_TRUE(mgr.GetStats().active_maps == 0u);

    std::cout << "  [PASS] SamplerFeedback (" << label
              << ", mode=" << (min_info.using_native_hardware ? "NATIVE" : "GPU_COMPUTE_FALLBACK")
              << ", host_tier=" << FeedbackTierName(stats.host_tier)
              << "): MinMip(0xFF clear, min-reduce, reclear) + MipRegionUsed(bitmask + 5 mip planes)\n";
}

// ---------------------------------------------------------------------------
// 2. Query Accumulator Tests (2 Command Lists: Occlusion, Binary, Timestamp, PipelineStats)
// ---------------------------------------------------------------------------
void RunQueryAccumulatorTests(GpuContext& ctx, const std::string& label) {
    QueryAccumulatorX qa;
    QueryAccumulatorConfig cfg{};
    cfg.occlusion_capacity = 32u;
    cfg.timestamp_capacity = 32u;
    cfg.pipeline_stats_capacity = 16u;
    EXPECT_OK(qa.Initialize(ctx.device.Get(), cfg));
    qa.BeginFrame(1ull);
    EXPECT_TRUE(qa.CurrentFrame() == 1ull);

    // Minimal VS+PS graphics pipeline driven by SV_VertexID + 1 root constant (quadrant select).
    constexpr const char* kVsHlsl = R"(
cbuffer DrawSel : register(b0) { uint mode; };
float4 main(uint vid : SV_VertexID) : SV_Position {
    float2 p = float2(0.0, 0.0);
    if (mode == 0u) {
        // Upper-left quadrant triangle (~25% of 64x64 RT => ~512 pixels).
        float2 tri[3] = { float2(-1.0, 1.0), float2(0.0, 1.0), float2(-1.0, 0.0) };
        p = tri[vid];
    } else if (mode == 1u) {
        // Lower-right quadrant triangle (~25% of 64x64 RT => ~512 pixels).
        float2 tri[3] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, -1.0) };
        p = tri[vid];
    } else {
        // Completely offscreen triangle (0 rasterized pixels).
        float2 tri[3] = { float2(2.0, 2.0), float2(3.0, 2.0), float2(2.0, 3.0) };
        p = tri[vid];
    }
    return float4(p, 0.5, 1.0);
}
)";
    constexpr const char* kPsHlsl = R"(
float4 main() : SV_Target {
    return float4(0.25, 0.75, 0.5, 1.0);
}
)";

    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    EXPECT_TRUE(SUCCEEDED(D3DCompile(
        kVsHlsl, std::strlen(kVsHlsl), "QueryTestVS", nullptr, nullptr,
        "main", "vs_5_0", 0u, 0u, vs_blob.Put(), err_blob.Put())));
    EXPECT_TRUE(SUCCEEDED(D3DCompile(
        kPsHlsl, std::strlen(kPsHlsl), "QueryTestPS", nullptr, nullptr,
        "main", "ps_5_0", 0u, 0u, ps_blob.Put(), err_blob.Put())));

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0u;
    rp.Constants.RegisterSpace = 0u;
    rp.Constants.Num32BitValues = 1u;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1u;
    rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    auto serialize_fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    EXPECT_TRUE(serialize_fn != nullptr);

    ComPtr<ID3DBlob> sig_blob;
    EXPECT_TRUE(SUCCEEDED(serialize_fn(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put())));
    ComPtr<ID3D12RootSignature> root_sig;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateRootSignature(
        0u, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        OS_TEST_UUIDOF(ID3D12RootSignature), root_sig.PutVoid())));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = 0xFFFFFFFFu;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1u;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1u;

    ComPtr<ID3D12PipelineState> pso;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateGraphicsPipelineState(
        &pso_desc, OS_TEST_UUIDOF(ID3D12PipelineState), pso.PutVoid())));

    // Create 64x64 RGBA8 render target + RTV heap.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = 64u;
    rt_desc.Height = 64u;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ComPtr<ID3D12Resource> rt_tex;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr, OS_TEST_UUIDOF(ID3D12Resource), rt_tex.PutVoid())));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = 1u;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateDescriptorHeap(
        &rtv_hd, OS_TEST_UUIDOF(ID3D12DescriptorHeap), rtv_heap.PutVoid())));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    ctx.device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv_h);

    // Record across 2 independent command lists.
    ComPtr<ID3D12CommandAllocator> alloc1, alloc2;
    ComPtr<ID3D12GraphicsCommandList> cl1, cl2;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, OS_TEST_UUIDOF(ID3D12CommandAllocator), alloc1.PutVoid())));
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, OS_TEST_UUIDOF(ID3D12CommandAllocator), alloc2.PutVoid())));
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc1.Get(), pso.Get(),
        OS_TEST_UUIDOF(ID3D12GraphicsCommandList), cl1.PutVoid())));
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2.Get(), pso.Get(),
        OS_TEST_UUIDOF(ID3D12GraphicsCommandList), cl2.PutVoid())));

    const D3D12_VIEWPORT vp{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, 64, 64};
    const float clear_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    // CL #1: Timestamp 0 -> visible tri 0 (Occlusion 0, Binary 1, PipeStats 0) + offscreen tri (Occlusion 2) -> Timestamp 1
    cl1->SetGraphicsRootSignature(root_sig.Get());
    cl1->RSSetViewports(1u, &vp);
    cl1->RSSetScissorRects(1u, &sc);
    cl1->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
    cl1->ClearRenderTargetView(rtv_h, clear_col, 0u, nullptr);
    cl1->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    EXPECT_OK(qa.EndQuery(cl1.Get(), QueryKind::Timestamp, 0u));
    EXPECT_OK(qa.BeginQuery(cl1.Get(), QueryKind::Occlusion, 0u));
    EXPECT_OK(qa.BeginQuery(cl1.Get(), QueryKind::BinaryOcclusion, 1u));
    EXPECT_OK(qa.BeginQuery(cl1.Get(), QueryKind::PipelineStatistics, 0u));
    cl1->SetGraphicsRoot32BitConstant(0u, 0u, 0u);
    cl1->DrawInstanced(3u, 1u, 0u, 0u);
    EXPECT_OK(qa.EndQuery(cl1.Get(), QueryKind::PipelineStatistics, 0u));
    EXPECT_OK(qa.EndQuery(cl1.Get(), QueryKind::BinaryOcclusion, 1u));
    EXPECT_OK(qa.EndQuery(cl1.Get(), QueryKind::Occlusion, 0u));

    EXPECT_OK(qa.BeginQuery(cl1.Get(), QueryKind::Occlusion, 2u));
    cl1->SetGraphicsRoot32BitConstant(0u, 2u, 0u);  // offscreen -> 0 pixels
    cl1->DrawInstanced(3u, 1u, 0u, 0u);
    EXPECT_OK(qa.EndQuery(cl1.Get(), QueryKind::Occlusion, 2u));
    EXPECT_OK(qa.EndQuery(cl1.Get(), QueryKind::Timestamp, 1u));
    EXPECT_TRUE(SUCCEEDED(cl1->Close()));

    // CL #2: Timestamp 2 -> visible tri 1 (Occlusion 3, Binary 4, PipeStats 1) -> Timestamp 3
    cl2->SetGraphicsRootSignature(root_sig.Get());
    cl2->RSSetViewports(1u, &vp);
    cl2->RSSetScissorRects(1u, &sc);
    cl2->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
    cl2->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    EXPECT_OK(qa.EndQuery(cl2.Get(), QueryKind::Timestamp, 2u));
    EXPECT_OK(qa.BeginQuery(cl2.Get(), QueryKind::Occlusion, 3u));
    EXPECT_OK(qa.BeginQuery(cl2.Get(), QueryKind::BinaryOcclusion, 4u));
    EXPECT_OK(qa.BeginQuery(cl2.Get(), QueryKind::PipelineStatistics, 1u));
    cl2->SetGraphicsRoot32BitConstant(0u, 1u, 0u);
    cl2->DrawInstanced(3u, 1u, 0u, 0u);
    EXPECT_OK(qa.EndQuery(cl2.Get(), QueryKind::PipelineStatistics, 1u));
    EXPECT_OK(qa.EndQuery(cl2.Get(), QueryKind::BinaryOcclusion, 4u));
    EXPECT_OK(qa.EndQuery(cl2.Get(), QueryKind::Occlusion, 3u));
    EXPECT_OK(qa.EndQuery(cl2.Get(), QueryKind::Timestamp, 3u));
    EXPECT_TRUE(SUCCEEDED(cl2->Close()));

    ID3D12CommandList* two_lists[2] = {cl1.Get(), cl2.Get()};
    EXPECT_TRUE(ctx.SubmitAndWait(two_lists, 2u));

    // Verify CPU multi-range occlusion readback across both CLs.
    uint64_t occ_sum = 0ull;
    std::vector<uint64_t> occ_vals;
    EXPECT_OK(qa.ReadbackAndAccumulateU64(
        ctx.queue.Get(), QueryKind::Occlusion, {{0u, 1u}, {3u, 1u}}, occ_sum, &occ_vals));
    EXPECT_TRUE(occ_vals.size() == 2u);
    EXPECT_TRUE(occ_vals[0] > 0ull && occ_vals[1] > 0ull);
    EXPECT_TRUE(occ_sum == occ_vals[0] + occ_vals[1]);

    std::vector<uint64_t> offscreen_occ;
    EXPECT_OK(qa.ReadbackU64Range(ctx.queue.Get(), QueryKind::Occlusion, 2u, 1u, offscreen_occ));
    EXPECT_TRUE(offscreen_occ.size() == 1u && offscreen_occ[0] == 0ull);

    uint64_t bin_acc = 0ull;
    std::vector<uint64_t> bin_vals;
    EXPECT_OK(qa.ReadbackAndAccumulateU64(
        ctx.queue.Get(), QueryKind::BinaryOcclusion, {{1u, 1u}, {4u, 1u}}, bin_acc, &bin_vals));
    EXPECT_TRUE(bin_vals.size() == 2u && bin_vals[0] == 1ull && bin_vals[1] == 1ull && bin_acc == 1ull);

    // Verify monotonic timestamps across both CLs.
    std::vector<uint64_t> ts_vals;
    EXPECT_OK(qa.ReadbackU64Range(ctx.queue.Get(), QueryKind::Timestamp, 0u, 4u, ts_vals));
    EXPECT_TRUE(ts_vals.size() == 4u);
    EXPECT_TRUE(ts_vals[0] <= ts_vals[1] && ts_vals[1] <= ts_vals[2] && ts_vals[2] <= ts_vals[3]);

    // Verify accumulated pipeline statistics across both CLs.
    QueryPipelineStatsResult ps_sum{};
    std::vector<QueryPipelineStatsResult> ps_each;
    EXPECT_OK(qa.ReadbackPipelineStatistics(ctx.queue.Get(), {{0u, 2u}}, ps_sum, &ps_each));
    EXPECT_TRUE(ps_each.size() == 2u);
    EXPECT_TRUE(ps_sum.ia_vertices == 6ull);
    EXPECT_TRUE(ps_sum.ia_primitives == 2ull);
    EXPECT_TRUE(ps_sum.vs_invocations >= 6ull);
    EXPECT_TRUE(ps_sum.ps_invocations > 0ull);

    // Verify GPU compute accumulation into a DEFAULT buffer with passing & failing predication!
    ComPtr<ID3D12Resource> gpu_accum_buf;
    ComPtr<ID3D12Resource> pred_upload;
    ComPtr<ID3D12Resource> pred_buf;
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_DEFAULT, 64ull, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, gpu_accum_buf));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_UPLOAD, 64ull, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ, pred_upload));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_DEFAULT, 64ull, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, pred_buf));

    {
        void* mapped = nullptr;
        D3D12_RANGE rn{0u, 0u};
        EXPECT_TRUE(SUCCEEDED(pred_upload->Map(0u, &rn, &mapped)) && mapped != nullptr);
        auto* p64 = static_cast<uint64_t*>(mapped);
        p64[0] = 1ull;  // non-zero (passes EQUAL_ZERO)
        p64[1] = 0ull;  // zero (skips EQUAL_ZERO)
        D3D12_RANGE wr{0u, 16u};
        pred_upload->Unmap(0u, &wr);
    }

    EXPECT_TRUE(SUCCEEDED(alloc1->Reset()));
    EXPECT_TRUE(SUCCEEDED(cl1->Reset(alloc1.Get(), nullptr)));
    cl1->CopyBufferRegion(pred_buf.Get(), 0ull, pred_upload.Get(), 0ull, 16ull);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = pred_buf.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PREDICATION;
        cl1->ResourceBarrier(1u, &b);
    }
    // 1) Overwrite slot 0 with occ0 + occ3
    EXPECT_OK(qa.ResolveAndAccumulateU64Gpu(
        cl1.Get(), QueryKind::Occlusion, {{0u, 1u}, {3u, 1u}},
        gpu_accum_buf.Get(), 0ull, QueryAccumulationOp::Overwrite));
    // 2) Predicated Add (passes because pred[0] == 1 != 0) => 2 * (occ0 + occ3)
    EXPECT_OK(qa.ResolveAndAccumulateU64Gpu(
        cl1.Get(), QueryKind::Occlusion, {{0u, 1u}, {3u, 1u}},
        gpu_accum_buf.Get(), 0ull, QueryAccumulationOp::Add,
        pred_buf.Get(), 0ull, D3D12_PREDICATION_OP_EQUAL_ZERO));
    // 3) Predicated Add (skipped because pred[1] == 0) => stays 2 * (occ0 + occ3)
    EXPECT_OK(qa.ResolveAndAccumulateU64Gpu(
        cl1.Get(), QueryKind::Occlusion, {{0u, 1u}, {3u, 1u}},
        gpu_accum_buf.Get(), 0ull, QueryAccumulationOp::Add,
        pred_buf.Get(), 8ull, D3D12_PREDICATION_OP_EQUAL_ZERO));
    EXPECT_TRUE(SUCCEEDED(cl1->Close()));
    ID3D12CommandList* l1 = cl1.Get();
    EXPECT_TRUE(ctx.SubmitAndWait(&l1, 1u));

    std::vector<uint8_t> accum_bytes;
    EXPECT_TRUE(ctx.ReadbackBuffer(
        gpu_accum_buf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 16ull, accum_bytes));
    uint64_t gpu_accum_val = 0ull;
    std::memcpy(&gpu_accum_val, accum_bytes.data(), sizeof(gpu_accum_val));
    EXPECT_TRUE(gpu_accum_val == 2ull * occ_sum);

    const QueryAccumulatorStats qst = qa.GetStats();
    EXPECT_TRUE(qst.gpu_accumulate_dispatches == 3ull);

    std::cout << "  [PASS] QueryAccumulatorX (" << label << "): occ=(" << occ_vals[0] << "+" << occ_vals[1]
              << "=" << occ_sum << ", offscreen=0), gpu_pred_accum=" << gpu_accum_val
              << ", bin=" << bin_acc << ", ts_delta=" << (ts_vals[3] - ts_vals[0])
              << " ticks, pipe_stats(IAv=" << ps_sum.ia_vertices
              << ", IAp=" << ps_sum.ia_primitives << ", PS=" << ps_sum.ps_invocations << ")\n";
}

// ---------------------------------------------------------------------------
// 3. Predicated Byte-Granular Buffer Copy Tests (Odd Offsets + Pass/Fail)
// ---------------------------------------------------------------------------
void RunPredicatedByteCopyTests(GpuContext& ctx, const std::string& label) {
    constexpr uint64_t kBufBytes = 256ull;
    constexpr uint8_t kSentinel = 0x5Au;

    ComPtr<ID3D12Resource> src_upload, dst_init_upload, pred_upload;
    ComPtr<ID3D12Resource> src_buf, dst_buf, pred_buf;

    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_UPLOAD, kBufBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, src_upload));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_UPLOAD, kBufBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, dst_init_upload));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_UPLOAD, 64ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, pred_upload));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_DEFAULT, kBufBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, src_buf));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_DEFAULT, kBufBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, dst_buf));
    EXPECT_TRUE(ctx.CreateBuffer(
        D3D12_HEAP_TYPE_DEFAULT, 64ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, pred_buf));

    std::vector<uint8_t> src_pattern(static_cast<size_t>(kBufBytes));
    for (size_t i = 0u; i < src_pattern.size(); ++i) {
        src_pattern[i] = static_cast<uint8_t>((i * 37u + 0xA1u) & 0xFFu);
        if (src_pattern[i] == kSentinel) src_pattern[i] = 0xC3u;
    }

    auto upload_bytes = [](ID3D12Resource* up, const void* data, size_t n) {
        void* p = nullptr;
        D3D12_RANGE rn{0u, 0u};
        if (SUCCEEDED(up->Map(0u, &rn, &p)) && p) {
            std::memcpy(p, data, n);
            D3D12_RANGE wr{0u, n};
            up->Unmap(0u, &wr);
            return true;
        }
        return false;
    };

    std::vector<uint8_t> init_sentinel(static_cast<size_t>(kBufBytes), kSentinel);
    const uint64_t predicates[2] = {1ull, 0ull};  // [0]=non-zero, [1]=zero
    EXPECT_TRUE(upload_bytes(src_upload.Get(), src_pattern.data(), src_pattern.size()));
    EXPECT_TRUE(upload_bytes(dst_init_upload.Get(), init_sentinel.data(), init_sentinel.size()));
    EXPECT_TRUE(upload_bytes(pred_upload.Get(), predicates, sizeof(predicates)));

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, OS_TEST_UUIDOF(ID3D12CommandAllocator), alloc.PutVoid())));
    EXPECT_TRUE(SUCCEEDED(ctx.device->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        OS_TEST_UUIDOF(ID3D12GraphicsCommandList), cmd.PutVoid())));

    cmd->CopyBufferRegion(src_buf.Get(), 0ull, src_upload.Get(), 0ull, kBufBytes);
    cmd->CopyBufferRegion(dst_buf.Get(), 0ull, dst_init_upload.Get(), 0ull, kBufBytes);
    cmd->CopyBufferRegion(pred_buf.Get(), 0ull, pred_upload.Get(), 0ull, sizeof(predicates));

    D3D12_RESOURCE_BARRIER init_barriers[2]{};
    init_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    init_barriers[0].Transition = {src_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    init_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    init_barriers[1].Transition = {pred_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION};
    cmd->ResourceBarrier(2u, init_barriers);

    // Case A: PASSING (pred[0]=1 != 0 with EQUAL_ZERO -> copies 19 bytes at odd offsets 5 -> 3)
    PredicatedByteCopyDesc copy_a{};
    copy_a.dst_buffer = dst_buf.Get();
    copy_a.dst_offset_bytes = 3ull;
    copy_a.src_buffer = src_buf.Get();
    copy_a.src_offset_bytes = 5ull;
    copy_a.num_bytes = 19ull;
    copy_a.predicate_buffer = pred_buf.Get();
    copy_a.predicate_offset_bytes = 0ull;
    copy_a.predicate_op = D3D12_PREDICATION_OP_EQUAL_ZERO;
    PredicatedByteCopyMetrics m_a{};
    EXPECT_OK(PredicatedByteBufferCopy(cmd.Get(), copy_a, &m_a));
    EXPECT_TRUE(m_a.used_native_predication && m_a.sub_4byte_unaligned);

    // Case B: FAILING (pred[1]=0 == 0 with EQUAL_ZERO -> MUST NOT overwrite dst[37..53])
    PredicatedByteCopyDesc copy_b = copy_a;
    copy_b.dst_offset_bytes = 37ull;
    copy_b.src_offset_bytes = 11ull;
    copy_b.num_bytes = 17ull;
    copy_b.predicate_offset_bytes = 8ull;
    copy_b.predicate_op = D3D12_PREDICATION_OP_EQUAL_ZERO;
    EXPECT_OK(PredicatedByteBufferCopy(cmd.Get(), copy_b));

    // Case C: PASSING inverted (pred[1]=0 with NOT_EQUAL_ZERO -> copies 13 bytes at 13 -> 71)
    PredicatedByteCopyDesc copy_c = copy_a;
    copy_c.dst_offset_bytes = 71ull;
    copy_c.src_offset_bytes = 13ull;
    copy_c.num_bytes = 13ull;
    copy_c.predicate_offset_bytes = 8ull;
    copy_c.predicate_op = D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    EXPECT_OK(PredicatedByteBufferCopy(cmd.Get(), copy_c));

    // Case D: FAILING inverted (pred[0]=1 with NOT_EQUAL_ZERO -> MUST NOT overwrite dst[101..111])
    PredicatedByteCopyDesc copy_d = copy_a;
    copy_d.dst_offset_bytes = 101ull;
    copy_d.src_offset_bytes = 17ull;
    copy_d.num_bytes = 11ull;
    copy_d.predicate_offset_bytes = 0ull;
    copy_d.predicate_op = D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    EXPECT_OK(PredicatedByteBufferCopy(cmd.Get(), copy_d));

    EXPECT_TRUE(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* l = cmd.Get();
    EXPECT_TRUE(ctx.SubmitAndWait(&l, 1u));

    std::vector<uint8_t> dst_rb;
    EXPECT_TRUE(ctx.ReadbackBuffer(dst_buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kBufBytes, dst_rb));

    // Build expected buffer.
    std::vector<uint8_t> expected = init_sentinel;
    std::memcpy(expected.data() + 3u, src_pattern.data() + 5u, 19u);
    std::memcpy(expected.data() + 71u, src_pattern.data() + 13u, 13u);

    EXPECT_TRUE(dst_rb == expected);
    std::cout << "  [PASS] PredicatedByteBufferCopy (" << label
              << "): odd-offset EQUAL_ZERO pass(3<-5,19B)+skip(37,17B) & NOT_EQUAL_ZERO pass(71<-13,13B)+skip(101,11B) bit-exact\n";
}

// ---------------------------------------------------------------------------
// 4. GPU-to-Win32 QPC Clock Calibration Tests (D3D12 + Vulkan)
// ---------------------------------------------------------------------------
void RunD3D12ClockCalibrationTest(GpuContext& ctx, const std::string& label) {
    GpuClockCalibrationResult cal{};
    EXPECT_OK(CalibrateGpuClockWithQpc(ctx.queue.Get(), cal, 5u, 2u));
    EXPECT_TRUE(cal.backend_name == "D3D12");
    EXPECT_TRUE(cal.gpu_timestamp_frequency_valid);
    EXPECT_TRUE(cal.gpu_timestamp_frequency_hz > 0ull);
    EXPECT_TRUE(cal.cpu_qpc_frequency_hz > 0ull);
    EXPECT_TRUE(std::isfinite(cal.gpu_tick_period_ns) && cal.gpu_tick_period_ns > 0.0);
    EXPECT_TRUE(std::isfinite(cal.estimated_offset_seconds));
    EXPECT_TRUE(std::isfinite(cal.estimated_drift_ppm));
    EXPECT_TRUE(std::isfinite(cal.min_bracket_error_us) && cal.min_bracket_error_us >= 0.0);
    EXPECT_TRUE(cal.min_bracket_error_us < 50000.0);  // Tight calibration bracket (< 50 ms even under load)
    EXPECT_TRUE(cal.samples.size() == 5u);

    const uint64_t mapped_anchor_qpc = cal.GpuTicksToEstimatedQpc(cal.anchor.gpu_timestamp_ticks);
    const uint64_t anchor_diff = (mapped_anchor_qpc >= cal.anchor.cpu_qpc_ticks)
                                     ? (mapped_anchor_qpc - cal.anchor.cpu_qpc_ticks)
                                     : (cal.anchor.cpu_qpc_ticks - mapped_anchor_qpc);
    EXPECT_TRUE(anchor_diff <= 1ull);
    EXPECT_TRUE(cal.GpuDeltaTicksToMicroseconds(cal.gpu_timestamp_frequency_hz) > 999999.0);

    std::cout << "  [PASS] D3D12 ClockCalibration (" << label << "): gpu_freq="
              << cal.gpu_timestamp_frequency_hz << " Hz, qpc_freq=" << cal.cpu_qpc_frequency_hz
              << " Hz, min_bracket=" << std::fixed << std::setprecision(2) << cal.min_bracket_error_us
              << " us, offset_spread=" << cal.max_offset_spread_us
              << " us, drift=" << cal.estimated_drift_ppm << " ppm\n";
}

void RunVulkanClockCalibrationTest() {
    GpuClockCalibrationResult vk_cal{};
    Status st = CalibrateVulkanGpuClockWithQpc(vk_cal, 5u, 2u);
    if (st.ok()) {
        EXPECT_TRUE(vk_cal.backend_name == "Vulkan");
        EXPECT_TRUE(vk_cal.calibrated_with_vulkan_ext);
        EXPECT_TRUE(vk_cal.gpu_timestamp_frequency_valid);
        EXPECT_TRUE(vk_cal.gpu_timestamp_frequency_hz > 0ull);
        EXPECT_TRUE(vk_cal.cpu_qpc_frequency_hz > 0ull);
        EXPECT_TRUE(std::isfinite(vk_cal.gpu_tick_period_ns) && vk_cal.gpu_tick_period_ns > 0.0);
        EXPECT_TRUE(std::isfinite(vk_cal.estimated_offset_seconds));
        EXPECT_TRUE(std::isfinite(vk_cal.min_bracket_error_us) && vk_cal.min_bracket_error_us < 50000.0);
        EXPECT_TRUE(vk_cal.samples.size() == 5u);
        std::cout << "  [PASS] Vulkan VK_EXT_calibrated_timestamps (" << vk_cal.device_name
                  << "): gpu_freq=" << vk_cal.gpu_timestamp_frequency_hz
                  << " Hz (period=" << std::fixed << std::setprecision(3) << vk_cal.gpu_tick_period_ns
                  << " ns), min_maxDev=" << std::setprecision(2) << vk_cal.min_bracket_error_us
                  << " us, offset_spread=" << vk_cal.max_offset_spread_us
                  << " us, drift=" << vk_cal.estimated_drift_ppm << " ppm\n";
    } else {
        EXPECT_TRUE(st.code == StatusCode::NotImplemented);
        std::cout << "  [SKIP] Vulkan VK_EXT_calibrated_timestamps unavailable on host: " << st.message << "\n";
    }
}

void RunFullAdapterSuite(bool prefer_warp, const char* suite_tag) {
    GpuContext ctx;
    if (!ctx.Init(prefer_warp)) {
        ++g_failures;
        return;
    }
    const std::string label = std::string(suite_tag) + " [" + ctx.info.description + "]";
    std::cout << "=== Adapter Suite: " << label
              << (ctx.info.is_warp ? " (WARP)" : " (HARDWARE)") << " ===\n";

    // Run default path (native hardware when Tier >= 0.9, else GPU compute fallback).
    RunSamplerFeedbackTests(ctx, label, /*force_fallback=*/false);

    // If this adapter has native Sampler Feedback Tier 0.9/1.0, ALSO test the forced fallback path
    // to guarantee 100% parity across both codepaths on the same adapter.
    if (SamplerFeedbackManager::ProbeHostSamplerFeedbackTier(ctx.device.Get()) !=
        SamplerFeedbackTier::NotSupported) {
        RunSamplerFeedbackTests(ctx, label + " [ForcedFallback]", /*force_fallback=*/true);
    }

    RunQueryAccumulatorTests(ctx, label);
    RunPredicatedByteCopyTests(ctx, label);
    RunD3D12ClockCalibrationTest(ctx, label);
}

#endif  // _WIN32

}  // namespace

int main() {
    std::cout << "OpenScarlett Sprint 11 — D3D12 Sampler Feedback, Query Accumulator, Predicated Byte Copy & Clock Calibration\n";
#if defined(_WIN32)
    RunFullAdapterSuite(/*prefer_warp=*/false, "DefaultGPU");
    RunFullAdapterSuite(/*prefer_warp=*/true, "WARP");
    std::cout << "=== Vulkan Calibrated Timestamps Probe ===\n";
    RunVulkanClockCalibrationTest();
#else
    std::cout << "Non-Windows platform: skipping live D3D12/QPC GPU checks.\n";
#endif

    if (g_failures > 0) {
        std::cerr << "\nFAILED with " << g_failures << " failure(s).\n";
        return 1;
    }
    std::cout << "\nALL TESTS PASSED (0 failures).\n";
    return 0;
}
