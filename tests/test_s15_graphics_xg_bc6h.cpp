// Sprint 15 Worker 3: D3D12X, Vulkan, XG 3D & BC6H/BCPack Parity Live Test Suite
//
// Exercises all 5 deliverables end-to-end on live D3D12 HW/WARP, Vulkan ICD/fallback,
// XG 3D packed & unpacked mip chains, and bit-exact BC6H/BCPack/TTPF/rANS codecs.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "gpu/d3d12_xs/command_list_x.h"
#include "gpu/d3d12_xs/device_x.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/gpu/xg_layout.h"
#include "openscarlett/io/bcpack.h"

namespace {

int g_failures = 0;

#define S15_CHECK(cond, msg)                                                           \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s (%s)\n", __FILE__, __LINE__, (msg), \
                         #cond);                                                       \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

void WaitQueueIdle(ID3D12Device* dev, ID3D12CommandQueue* queue, uint64_t& fence_val) {
    ID3D12Fence* fence = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence,
              "CreateFence for sync");
    if (!fence) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    S15_CHECK(ev != nullptr, "CreateEventW for sync");
    ++fence_val;
    S15_CHECK(SUCCEEDED(queue->Signal(fence, fence_val)), "Queue Signal");
    if (fence->GetCompletedValue() < fence_val && ev) {
        S15_CHECK(SUCCEEDED(fence->SetEventOnCompletion(fence_val, ev)), "SetEventOnCompletion");
        S15_CHECK(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "WaitForSingleObject GPU fence");
    }
    if (ev) CloseHandle(ev);
    fence->Release();
}

void WriteBits128(uint64_t& lo, uint64_t& hi, int bit_pos, int count, uint32_t value) noexcept {
    const uint64_t mask = (count >= 32) ? 0xFFFFFFFFull : ((1ull << count) - 1ull);
    const uint64_t v = static_cast<uint64_t>(value) & mask;
    if (bit_pos + count <= 64) {
        lo &= ~(mask << bit_pos);
        lo |= (v << bit_pos);
    } else if (bit_pos >= 64) {
        const int sh = bit_pos - 64;
        hi &= ~(mask << sh);
        hi |= (v << sh);
    } else {
        const int lo_bits = 64 - bit_pos;
        const uint64_t lo_mask = (1ull << lo_bits) - 1ull;
        lo &= ~(lo_mask << bit_pos);
        lo |= ((v & lo_mask) << bit_pos);
        const int hi_bits = count - lo_bits;
        const uint64_t hi_mask = (1ull << hi_bits) - 1ull;
        hi &= ~hi_mask;
        hi |= ((v >> lo_bits) & hi_mask);
    }
}

void WriteBitsReversed128(uint64_t& lo, uint64_t& hi, int bit_pos, int count, uint32_t value) noexcept {
    uint32_t rev = 0u;
    for (int i = 0; i < count; ++i) {
        rev = (rev << 1u) | ((value >> i) & 1u);
    }
    WriteBits128(lo, hi, bit_pos, count, rev);
}

void StoreBlock128(uint64_t lo, uint64_t hi, uint8_t out_blk[16]) noexcept {
    std::memcpy(out_blk, &lo, 8u);
    std::memcpy(out_blk + 8u, &hi, 8u);
}

void FillPattern(std::vector<uint8_t>& buf, uint32_t seed) {
    uint32_t s = seed ^ 0x9E3779B9u;
    for (size_t i = 0u; i < buf.size(); ++i) {
        s ^= s << 13u;
        s ^= s >> 17u;
        s ^= s << 5u;
        buf[i] = static_cast<uint8_t>((s ^ static_cast<uint32_t>(i * 131u)) & 0xFFu);
    }
}

// ===========================================================================
// 1. Oversized Placed-Texture Transparent Committed Memory Promotion (xcl #86)
// ===========================================================================
void TestOversizedPlacedTexturePromotion(
    ID3D12DeviceX* dev,
    ID3D12CommandQueueX* queue,
    ID3D12CommandAllocator* alloc,
    ID3D12GraphicsCommandListX* cl,
    uint64_t& fence_val) {
    std::printf("[Test 1] Oversized Placed-Texture Transparent Committed Promotion (xcl #86)...\n");

    IOpenScarlettGraphicsParity* parity = nullptr;
    S15_CHECK(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettGraphicsParity, reinterpret_cast<void**>(&parity))) &&
                  parity != nullptr,
              "QueryInterface(IOpenScarlettGraphicsParity)");
    openscarlett::d3d12_xs::IOpenScarlettVramResidencyControl* res_ctrl = nullptr;
    S15_CHECK(SUCCEEDED(dev->QueryInterface(openscarlett::d3d12_xs::IID_IOpenScarlettVramResidencyControl,
                                            reinterpret_cast<void**>(&res_ctrl))) &&
                  res_ctrl != nullptr,
              "QueryInterface(IOpenScarlettVramResidencyControl)");

    D3D12XBOX_GRAPHICS_PARITY_STATS st_before{};
    if (parity) S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&st_before)), "GetGraphicsParityStats before");

    // 1A. Small 64 KiB heap (console-sized) + 1024x1024 RGBA8 texture (4 MiB >> 64 KiB)
    D3D12_HEAP_DESC small_hd{};
    small_hd.SizeInBytes = 65536ull;
    small_hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    small_hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    small_hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

    ID3D12Heap* small_heap = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreateHeap(&small_hd, IID_PPV_ARGS(&small_heap))) && small_heap,
              "CreateHeap 64 KiB");

    D3D12_RESOURCE_DESC big_tex_desc{};
    big_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    big_tex_desc.Width = 1024ull;
    big_tex_desc.Height = 1024u;
    big_tex_desc.DepthOrArraySize = 1u;
    big_tex_desc.MipLevels = 1u;
    big_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    big_tex_desc.SampleDesc.Count = 1u;
    big_tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* promoted_tex = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreatePlacedResource(
                  small_heap, 0ull, &big_tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                  nullptr, IID_PPV_ARGS(&promoted_tex))) &&
                  promoted_tex != nullptr,
              "Oversized placed texture promoted transparently via CreatePlacedResource");

    D3D12XBOX_GRAPHICS_PARITY_STATS st_after1{};
    if (parity) S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&st_after1)), "GetGraphicsParityStats after 1");
    S15_CHECK(st_after1.promoted_oversized_placed_resources == st_before.promoted_oversized_placed_resources + 1ull,
              "promoted_oversized_placed_resources incremented by 1");

    if (res_ctrl) {
        openscarlett::d3d12_xs::D3D12XBOX_VRAM_RESIDENCY_STATS vstats{};
        S15_CHECK(SUCCEEDED(res_ctrl->GetVramResidencyStats(&vstats)), "GetVramResidencyStats");
        S15_CHECK(vstats.promoted_oversized_placed_resources == st_after1.promoted_oversized_placed_resources,
                  "VRAM residency stats mirror promoted_oversized_placed_resources");
    }

    // Verify promoted texture works live on GPU: upload a 16x16 sub-block and read it back bit-exact!
    if (promoted_tex) {
        D3D12_HEAP_PROPERTIES up_hp{};
        up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES rb_hp{};
        rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = 256ull * 16ull;
        buf_desc.Height = 1u;
        buf_desc.DepthOrArraySize = 1u;
        buf_desc.MipLevels = 1u;
        buf_desc.SampleDesc.Count = 1u;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* up_buf = nullptr;
        ID3D12Resource* rb_buf = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(&up_hp, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&up_buf))),
                  "Create upload buf for promoted tex");
        S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&rb_buf))),
                  "Create readback buf for promoted tex");

        if (up_buf && rb_buf) {
            void* mapped = nullptr;
            S15_CHECK(SUCCEEDED(up_buf->Map(0, nullptr, &mapped)) && mapped, "Map upload buf");
            if (mapped) {
                auto* bytes = static_cast<uint8_t*>(mapped);
                std::memset(bytes, 0, static_cast<size_t>(buf_desc.Width));
                for (uint32_t y = 0; y < 16u; ++y) {
                    for (uint32_t x = 0; x < 16u; ++x) {
                        bytes[y * 256u + x * 4u + 0u] = static_cast<uint8_t>(x * 13u + 5u);
                        bytes[y * 256u + x * 4u + 1u] = static_cast<uint8_t>(y * 11u + 9u);
                        bytes[y * 256u + x * 4u + 2u] = 0xA5u;
                        bytes[y * 256u + x * 4u + 3u] = 0xFFu;
                    }
                }
                up_buf->Unmap(0, nullptr);
            }

            S15_CHECK(SUCCEEDED(alloc->Reset()), "alloc Reset");
            S15_CHECK(SUCCEEDED(cl->Reset(alloc, nullptr)), "cl Reset");

            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = up_buf;
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src_loc.PlacedFootprint.Offset = 0ull;
            src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src_loc.PlacedFootprint.Footprint.Width = 16u;
            src_loc.PlacedFootprint.Footprint.Height = 16u;
            src_loc.PlacedFootprint.Footprint.Depth = 1u;
            src_loc.PlacedFootprint.Footprint.RowPitch = 256u;

            D3D12_TEXTURE_COPY_LOCATION tex_loc{};
            tex_loc.pResource = promoted_tex;
            tex_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            tex_loc.SubresourceIndex = 0u;

            cl->CopyTextureRegion(&tex_loc, 512u, 512u, 0u, &src_loc, nullptr);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = promoted_tex;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1u, &b);

            D3D12_TEXTURE_COPY_LOCATION rb_loc = src_loc;
            rb_loc.pResource = rb_buf;
            D3D12_BOX box{512u, 512u, 0u, 528u, 528u, 1u};
            cl->CopyTextureRegion(&rb_loc, 0u, 0u, 0u, &tex_loc, &box);
            S15_CHECK(SUCCEEDED(cl->Close()), "cl Close promoted tex");

            ID3D12CommandList* lists[1] = {cl};
            queue->ExecuteCommandLists(1u, lists);
            WaitQueueIdle(dev, queue, fence_val);

            void* rb_ptr = nullptr;
            S15_CHECK(SUCCEEDED(rb_buf->Map(0, nullptr, &rb_ptr)) && rb_ptr, "Map readback buf");
            if (rb_ptr) {
                const auto* rb_bytes = static_cast<const uint8_t*>(rb_ptr);
                bool pixel_ok = true;
                for (uint32_t y = 0; y < 16u && pixel_ok; ++y) {
                    for (uint32_t x = 0; x < 16u; ++x) {
                        const uint8_t r = rb_bytes[y * 256u + x * 4u + 0u];
                        const uint8_t g = rb_bytes[y * 256u + x * 4u + 1u];
                        const uint8_t bl = rb_bytes[y * 256u + x * 4u + 2u];
                        const uint8_t a = rb_bytes[y * 256u + x * 4u + 3u];
                        if (r != static_cast<uint8_t>(x * 13u + 5u) ||
                            g != static_cast<uint8_t>(y * 11u + 9u) ||
                            bl != 0xA5u || a != 0xFFu) {
                            pixel_ok = false;
                            break;
                        }
                    }
                }
                S15_CHECK(pixel_ok, "Promoted oversized texture GPU upload/readback bit-exact");
                rb_buf->Unmap(0, nullptr);
            }
        }
        if (up_buf) up_buf->Release();
        if (rb_buf) rb_buf->Release();
        promoted_tex->Release();
    }

    // 1B. Normal placed texture that DOES fit inside a large heap must NOT be promoted.
    D3D12_HEAP_DESC big_hd = small_hd;
    big_hd.SizeInBytes = 8ull * 1024ull * 1024ull;
    ID3D12Heap* big_heap = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreateHeap(&big_hd, IID_PPV_ARGS(&big_heap))) && big_heap, "CreateHeap 8 MiB");
    D3D12_RESOURCE_DESC small_tex_desc = big_tex_desc;
    small_tex_desc.Width = 64ull;
    small_tex_desc.Height = 64u;
    ID3D12Resource* normal_placed = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreatePlacedResource(
                  big_heap, 0ull, &small_tex_desc, D3D12_RESOURCE_STATE_COMMON,
                  nullptr, IID_PPV_ARGS(&normal_placed))) &&
                  normal_placed != nullptr,
              "Normal placed texture succeeds without promotion");
    D3D12XBOX_GRAPHICS_PARITY_STATS st_after_normal{};
    if (parity) S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&st_after_normal)), "Stats after normal placed");
    S15_CHECK(st_after_normal.promoted_oversized_placed_resources == st_after1.promoted_oversized_placed_resources,
              "Normal placed texture did NOT trigger promotion");
    if (normal_placed) normal_placed->Release();

    // 1C. HeapOffset >= heap_size (regression guard for old UINT64_MAX bug) promotes cleanly!
    ID3D12Resource* offset_oob_tex = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreatePlacedResource(
                  small_heap, 65536ull, &small_tex_desc, D3D12_RESOURCE_STATE_COMMON,
                  nullptr, IID_PPV_ARGS(&offset_oob_tex))) &&
                  offset_oob_tex != nullptr,
              "Placed texture with HeapOffset >= heap_size promoted cleanly");
    D3D12XBOX_GRAPHICS_PARITY_STATS st_after_oob{};
    if (parity) S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&st_after_oob)), "Stats after oob offset");
    S15_CHECK(st_after_oob.promoted_oversized_placed_resources == st_after1.promoted_oversized_placed_resources + 1ull,
              "HeapOffset >= heap_size incremented promoted counter");
    if (offset_oob_tex) offset_oob_tex->Release();

    // 1D. CreatePlacedResource1 (ID3D12Device8) oversized promotion!
    D3D12_RESOURCE_DESC1 big_tex_desc1{};
    big_tex_desc1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    big_tex_desc1.Width = 512ull;
    big_tex_desc1.Height = 512u;
    big_tex_desc1.DepthOrArraySize = 1u;
    big_tex_desc1.MipLevels = 1u;
    big_tex_desc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    big_tex_desc1.SampleDesc.Count = 1u;
    big_tex_desc1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* promoted_tex1 = nullptr;
    S15_CHECK(SUCCEEDED(dev->CreatePlacedResource1(
                  small_heap, 0ull, &big_tex_desc1, D3D12_RESOURCE_STATE_COMMON,
                  nullptr, IID_PPV_ARGS(&promoted_tex1))) &&
                  promoted_tex1 != nullptr,
              "CreatePlacedResource1 oversized texture promoted transparently");
    D3D12XBOX_GRAPHICS_PARITY_STATS st_after_p1{};
    if (parity) S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&st_after_p1)), "Stats after CreatePlacedResource1");
    S15_CHECK(st_after_p1.promoted_oversized_placed_resources == st_after_oob.promoted_oversized_placed_resources + 1ull,
              "CreatePlacedResource1 incremented promoted counter");
    if (promoted_tex1) promoted_tex1->Release();

    // 1E. CreatePlacedResource2 (ID3D12Device10) oversized promotion when supported on host!
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    ID3D12Device10* dev10 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev10))) && dev10) {
        ID3D12Resource* promoted_tex2 = nullptr;
        const HRESULT hr2 = dev10->CreatePlacedResource2(
            small_heap, 0ull, &big_tex_desc1, D3D12_BARRIER_LAYOUT_COMMON,
            nullptr, 0u, nullptr, IID_PPV_ARGS(&promoted_tex2));
        S15_CHECK(SUCCEEDED(hr2) && promoted_tex2 != nullptr,
                  "CreatePlacedResource2 oversized texture promoted transparently");
        D3D12XBOX_GRAPHICS_PARITY_STATS st_after_p2{};
        if (parity) S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&st_after_p2)), "Stats after CreatePlacedResource2");
        S15_CHECK(st_after_p2.promoted_oversized_placed_resources == st_after_p1.promoted_oversized_placed_resources + 1ull,
                  "CreatePlacedResource2 incremented promoted counter");
        if (promoted_tex2) promoted_tex2->Release();
        dev10->Release();
    }
#endif

    if (big_heap) big_heap->Release();
    if (small_heap) small_heap->Release();
    if (res_ctrl) res_ctrl->Release();
    if (parity) parity->Release();
    std::printf("  PASS: Oversized placed textures transparently promoted across CreatePlacedResource/1/2!\n");
}

// ===========================================================================
// 2. D3D12X & Xbox Extension Hardening
// ===========================================================================
void TestD3D12XAndXboxExtensionHardening(
    ID3D12DeviceX* dev,
    ID3D12CommandQueueX* queue,
    ID3D12CommandAllocator* alloc,
    ID3D12GraphicsCommandListX* cl,
    uint64_t& fence_val) {
    std::printf("[Test 2] D3D12X & Xbox Extension Hardening...\n");

    IOpenScarlettGraphicsParity* parity = nullptr;
    S15_CHECK(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettGraphicsParity, reinterpret_cast<void**>(&parity))) &&
                  parity != nullptr,
              "QueryInterface(IOpenScarlettGraphicsParity)");

    // 2A. 3D texture console-flag strip & live MSAA quality clamping (xvalkyrie c986c75/f371932)
    {
        D3D12_HEAP_PROPERTIES def_hp{};
        def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC tex3d{};
        tex3d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        tex3d.Width = 16ull;
        tex3d.Height = 16u;
        tex3d.DepthOrArraySize = 8u;
        tex3d.MipLevels = 1u;
        tex3d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex3d.SampleDesc.Count = 1u;
        tex3d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // Inject Xbox console-private flags + desktop-invalid 3D flags
        tex3d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            0x00030000u |
            static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) |
            static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) |
            static_cast<uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));

        ID3D12Resource* res3d = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                      &def_hp, D3D12_HEAP_FLAG_NONE, &tex3d, D3D12_RESOURCE_STATE_COMMON,
                      nullptr, IID_PPV_ARGS(&res3d))) &&
                      res3d != nullptr,
                  "3D texture with console/invalid flags sanitized & created cleanly");
        if (res3d) res3d->Release();

        // Live MSAA quality clamping (bogus Xbox quality 31 + DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN)
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qinfo{};
        qinfo.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        qinfo.SampleCount = 4u;
        qinfo.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        const bool msaa4_supported =
            SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qinfo, sizeof(qinfo))) &&
            qinfo.NumQualityLevels > 0u;
        if (msaa4_supported) {
            D3D12_RESOURCE_DESC msaa_desc{};
            msaa_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            msaa_desc.Width = 64ull;
            msaa_desc.Height = 64u;
            msaa_desc.DepthOrArraySize = 1u;
            msaa_desc.MipLevels = 1u;
            msaa_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            msaa_desc.SampleDesc.Count = 4u;
            msaa_desc.SampleDesc.Quality = 31u;  // Exceeds desktop NumQualityLevels
            msaa_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            ID3D12Resource* msaa_res1 = nullptr;
            S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                          &def_hp, D3D12_HEAP_FLAG_NONE, &msaa_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          nullptr, IID_PPV_ARGS(&msaa_res1))) &&
                          msaa_res1 != nullptr,
                      "4x MSAA texture with Xbox Quality=31 clamped & created");
            if (msaa_res1) {
                const D3D12_RESOURCE_DESC actual = msaa_res1->GetDesc();
                S15_CHECK(actual.SampleDesc.Count == 4u && actual.SampleDesc.Quality < qinfo.NumQualityLevels,
                          "Clamped MSAA Quality < NumQualityLevels");
                msaa_res1->Release();
            }

            msaa_desc.SampleDesc.Quality = DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN;
            ID3D12Resource* msaa_res2 = nullptr;
            S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                          &def_hp, D3D12_HEAP_FLAG_NONE, &msaa_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          nullptr, IID_PPV_ARGS(&msaa_res2))) &&
                          msaa_res2 != nullptr,
                      "4x MSAA texture with CENTER pattern clamped & created");
            if (msaa_res2) msaa_res2->Release();
        }
    }

    // 2B. CreateReservedResource / CreateReservedResource1 ALLOW_CROSS_ADAPTER (Leonida #4496)
    {
        D3D12_RESOURCE_DESC resv_desc{};
        resv_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resv_desc.Width = 256ull;
        resv_desc.Height = 256u;
        resv_desc.DepthOrArraySize = 1u;
        resv_desc.MipLevels = 1u;
        resv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        resv_desc.SampleDesc.Count = 1u;
        resv_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        resv_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        ID3D12Resource* resv0 = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateReservedResource(
                      &resv_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resv0))) &&
                      resv0 != nullptr,
                  "CreateReservedResource with ALLOW_CROSS_ADAPTER succeeds (preserved or fallback)");
        if (resv0) resv0->Release();

        ID3D12Resource* resv1 = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateReservedResource1(
                      &resv_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, nullptr, IID_PPV_ARGS(&resv1))) &&
                      resv1 != nullptr,
                  "CreateReservedResource1 with ALLOW_CROSS_ADAPTER succeeds");
        if (resv1) resv1->Release();
    }

    // 2C. Read-Only DSVs (D24_UNORM_S8_UINT & D32_FLOAT with READ_ONLY_DEPTH | READ_ONLY_STENCIL) (AnyBox #54)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_hd{};
        dsv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv_hd.NumDescriptors = 4u;
        ID3D12DescriptorHeap* dsv_heap = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(&dsv_heap))) && dsv_heap,
                  "Create DSV descriptor heap");
        const UINT dsv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_HEAP_PROPERTIES def_hp{};
        def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC ds_desc{};
        ds_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ds_desc.Width = 64ull;
        ds_desc.Height = 64u;
        ds_desc.DepthOrArraySize = 1u;
        ds_desc.MipLevels = 1u;
        ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        ds_desc.SampleDesc.Count = 1u;
        ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        ID3D12Resource* ds_d24s8 = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                      &def_hp, D3D12_HEAP_FLAG_NONE, &ds_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                      nullptr, IID_PPV_ARGS(&ds_d24s8))) &&
                      ds_d24s8 != nullptr,
                  "Create D24_UNORM_S8_UINT texture");

        ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
        ID3D12Resource* ds_d32 = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                      &def_hp, D3D12_HEAP_FLAG_NONE, &ds_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                      nullptr, IID_PPV_ARGS(&ds_d32))) &&
                      ds_d32 != nullptr,
                  "Create D32_FLOAT texture");

        if (dsv_heap && ds_d24s8 && ds_d32) {
            D3D12_CPU_DESCRIPTOR_HANDLE h0 = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv0{};
            dsv0.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsv0.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv0.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
            dev->CreateDepthStencilView(ds_d24s8, &dsv0, h0);

            // Depth-only D32_FLOAT passed READ_ONLY_DEPTH | READ_ONLY_STENCIL (stencil flag auto-stripped cleanly)
            D3D12_CPU_DESCRIPTOR_HANDLE h1{h0.ptr + dsv_inc};
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv1{};
            dsv1.Format = DXGI_FORMAT_D32_FLOAT;
            dsv1.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv1.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
            dev->CreateDepthStencilView(ds_d32, &dsv1, h1);

            // Also test Depth (plane 0) + Stencil (plane 1) CopyTextureRegion + non-zero footprint offset!
            D3D12_HEAP_PROPERTIES rb_hp{};
            rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rb_desc{};
            rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb_desc.Width = 256ull * 64ull * 2ull + 4096ull;
            rb_desc.Height = 1u;
            rb_desc.DepthOrArraySize = 1u;
            rb_desc.MipLevels = 1u;
            rb_desc.SampleDesc.Count = 1u;
            rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* ds_rb = nullptr;
            S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                          &rb_hp, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                          nullptr, IID_PPV_ARGS(&ds_rb))) &&
                          ds_rb != nullptr,
                      "Create depth/stencil readback buffer");

            S15_CHECK(SUCCEEDED(alloc->Reset()), "alloc Reset for DS/UAV/Noop");
            S15_CHECK(SUCCEEDED(cl->Reset(alloc, nullptr)), "cl Reset for DS/UAV/Noop");

            D3D12_RESOURCE_BARRIER ds_bar{};
            ds_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ds_bar.Transition.pResource = ds_d24s8;
            ds_bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ds_bar.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            ds_bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1u, &ds_bar);

            if (ds_rb) {
                // Plane 0 (Depth) readback at non-zero 512B offset
                D3D12_TEXTURE_COPY_LOCATION dst_p0{};
                dst_p0.pResource = ds_rb;
                dst_p0.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst_p0.PlacedFootprint.Offset = 512ull;
                dst_p0.PlacedFootprint.Footprint.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;  // Normalized to R24_UNORM_X8_TYPELESS!
                dst_p0.PlacedFootprint.Footprint.Width = 64u;
                dst_p0.PlacedFootprint.Footprint.Height = 64u;
                dst_p0.PlacedFootprint.Footprint.Depth = 1u;
                dst_p0.PlacedFootprint.Footprint.RowPitch = 256u;
                D3D12_TEXTURE_COPY_LOCATION src_p0{};
                src_p0.pResource = ds_d24s8;
                src_p0.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src_p0.SubresourceIndex = 0u;  // Plane 0
                cl->CopyTextureRegion(&dst_p0, 0u, 0u, 0u, &src_p0, nullptr);

                // Plane 1 (Stencil) readback at non-zero 512B-aligned offset
                D3D12_TEXTURE_COPY_LOCATION dst_p1{};
                dst_p1.pResource = ds_rb;
                dst_p1.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst_p1.PlacedFootprint.Offset = 512ull + 256ull * 64ull;
                dst_p1.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;  // Normalized to X24_TYPELESS_G8_UINT!
                dst_p1.PlacedFootprint.Footprint.Width = 64u;
                dst_p1.PlacedFootprint.Footprint.Height = 64u;
                dst_p1.PlacedFootprint.Footprint.Depth = 1u;
                dst_p1.PlacedFootprint.Footprint.RowPitch = 256u;
                D3D12_TEXTURE_COPY_LOCATION src_p1{};
                src_p1.pResource = ds_d24s8;
                src_p1.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src_p1.SubresourceIndex = 1u;  // Plane 1 (stencil)
                cl->CopyTextureRegion(&dst_p1, 0u, 0u, 0u, &src_p1, nullptr);
            }

            // 2D. Global UAV Barrier (UAV.pResource == nullptr, xcl #68)
            D3D12_RESOURCE_BARRIER uav_all{};
            uav_all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav_all.UAV.pResource = nullptr;
            cl->ResourceBarrier(1u, &uav_all);

            // 2E. Zero-count no-ops (AnyBox #54/#57/#67)
            cl->RSSetViewports(0u, nullptr);
            cl->RSSetScissorRects(0u, nullptr);
            cl->DrawInstanced(0u, 1u, 0u, 0u);
            cl->DrawInstanced(3u, 0u, 0u, 0u);
            cl->DrawIndexedInstanced(0u, 1u, 0u, 0, 0u);
            cl->Dispatch(0u, 1u, 1u);
            cl->ResourceBarrier(0u, nullptr);
            cl->IASetVertexBuffers(0u, 0u, nullptr);

            // 2F. Unsupported extension does NOT poison subsequent supported GPU commands (Leonida #3958)
            cl->InitializeMetaCommand(nullptr, nullptr, 0u);
            cl->ExecuteMetaCommand(nullptr, nullptr, 0u);

            // Immediately execute a real CopyBufferRegion + non-zero unaligned footprint CopyTextureRegion after the unsupported extension!
            D3D12_HEAP_PROPERTIES up_hp{};
            up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC small_buf{};
            small_buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            small_buf.Width = 4096ull;
            small_buf.Height = 1u;
            small_buf.DepthOrArraySize = 1u;
            small_buf.MipLevels = 1u;
            small_buf.SampleDesc.Count = 1u;
            small_buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* src_b = nullptr;
            ID3D12Resource* dst_b = nullptr;
            S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(&up_hp, D3D12_HEAP_FLAG_NONE, &small_buf,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&src_b))),
                      "Create src_b");
            S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &small_buf,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&dst_b))),
                      "Create dst_b");
            D3D12_RESOURCE_DESC color_tex_desc{};
            color_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            color_tex_desc.Width = 16ull;
            color_tex_desc.Height = 16u;
            color_tex_desc.DepthOrArraySize = 1u;
            color_tex_desc.MipLevels = 1u;
            color_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            color_tex_desc.SampleDesc.Count = 1u;
            ID3D12Resource* color_tex = nullptr;
            S15_CHECK(SUCCEEDED(dev->CreateCommittedResource(&def_hp, D3D12_HEAP_FLAG_NONE, &color_tex_desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&color_tex))),
                      "Create color_tex");

            constexpr uint32_t kMagicWord = 0x53313557u;  // "S15W"
            if (src_b && dst_b && color_tex) {
                void* mp = nullptr;
                if (SUCCEEDED(src_b->Map(0, nullptr, &mp)) && mp) {
                    auto* b = static_cast<uint8_t*>(mp);
                    std::memset(b, 0, 4096u);
                    std::memcpy(b, &kMagicWord, sizeof(kMagicWord));
                    // Place a 4x4 RGBA8 block at UNALIGNED Xbox offset 128B (non-512B multiple!)
                    for (uint32_t y = 0; y < 4u; ++y) {
                        for (uint32_t x = 0; x < 4u; ++x) {
                            b[128u + y * 256u + x * 4u + 0u] = static_cast<uint8_t>(0x10u + x);
                            b[128u + y * 256u + x * 4u + 1u] = static_cast<uint8_t>(0x20u + y);
                            b[128u + y * 256u + x * 4u + 2u] = 0x77u;
                            b[128u + y * 256u + x * 4u + 3u] = 0xFFu;
                        }
                    }
                    src_b->Unmap(0, nullptr);
                }

                // Supported buffer copy after unsupported extension
                cl->CopyBufferRegion(dst_b, 0ull, src_b, 0ull, 64ull);

                // Non-zero DstX=4, DstY=4 + unaligned footprint Offset=128 copy into texture & readback at aligned Offset=512
                D3D12_TEXTURE_COPY_LOCATION fp_src{};
                fp_src.pResource = src_b;
                fp_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                fp_src.PlacedFootprint.Offset = 128ull;
                fp_src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                fp_src.PlacedFootprint.Footprint.Width = 4u;
                fp_src.PlacedFootprint.Footprint.Height = 4u;
                fp_src.PlacedFootprint.Footprint.Depth = 1u;
                fp_src.PlacedFootprint.Footprint.RowPitch = 256u;

                D3D12_TEXTURE_COPY_LOCATION tex_dst{};
                tex_dst.pResource = color_tex;
                tex_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                tex_dst.SubresourceIndex = 0u;
                cl->CopyTextureRegion(&tex_dst, 4u, 4u, 0u, &fp_src, nullptr);

                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = color_tex;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cl->ResourceBarrier(1u, &tb);

                D3D12_TEXTURE_COPY_LOCATION fp_rb = fp_src;
                fp_rb.pResource = dst_b;
                fp_rb.PlacedFootprint.Offset = 512ull;
                D3D12_BOX box{4u, 4u, 0u, 8u, 8u, 1u};
                cl->CopyTextureRegion(&fp_rb, 0u, 0u, 0u, &tex_dst, &box);
            }

            const HRESULT close_hr = cl->Close();
            S15_CHECK(SUCCEEDED(close_hr), "cl Close after unsupported ext + UAV + DS + offset copy");
            if (SUCCEEDED(close_hr)) {
                ID3D12CommandList* lists[1] = {cl};
                queue->ExecuteCommandLists(1u, lists);
                WaitQueueIdle(dev, queue, fence_val);
            }

            if (dst_b) {
                void* rmp = nullptr;
                S15_CHECK(SUCCEEDED(dst_b->Map(0, nullptr, &rmp)) && rmp, "Map dst_b");
                if (rmp) {
                    uint32_t read_word = 0u;
                    std::memcpy(&read_word, rmp, sizeof(read_word));
                    S15_CHECK(read_word == kMagicWord,
                              "Supported CopyBufferRegion after unsupported extension executed cleanly");

                    const auto* b = static_cast<const uint8_t*>(rmp);
                    bool tex_ok = true;
                    for (uint32_t y = 0; y < 4u && tex_ok; ++y) {
                        for (uint32_t x = 0; x < 4u; ++x) {
                            const size_t off = 512u + y * 256u + x * 4u;
                            if (b[off + 0u] != static_cast<uint8_t>(0x10u + x) ||
                                b[off + 1u] != static_cast<uint8_t>(0x20u + y) ||
                                b[off + 2u] != 0x77u || b[off + 3u] != 0xFFu) {
                                tex_ok = false;
                                break;
                            }
                        }
                    }
                    S15_CHECK(tex_ok, "CopyTextureRegion with DstX/DstY + unaligned 128B & aligned 512B footprint offsets bit-exact");
                    dst_b->Unmap(0, nullptr);
                }
            }
            if (color_tex) color_tex->Release();
            if (dst_b) dst_b->Release();
            if (src_b) src_b->Release();
            if (ds_rb) ds_rb->Release();
        }
        if (ds_d32) ds_d32->Release();
        if (ds_d24s8) ds_d24s8->Release();
        if (dsv_heap) dsv_heap->Release();
    }

    // 2G. DXGIXSetVLineNotification & DXGIXSetFrameNotification (xvalkyrie a252811)
    {
        S15_CHECK(DXGIXSetVLineNotification(0u, 540u, INVALID_HANDLE_VALUE) == E_INVALIDARG,
                  "DXGIXSetVLineNotification rejects INVALID_HANDLE_VALUE");
        S15_CHECK(DXGIXSetFrameNotification(0u, INVALID_HANDLE_VALUE) == E_INVALIDARG,
                  "DXGIXSetFrameNotification rejects INVALID_HANDLE_VALUE");

        HANDLE hVLine = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE hFrame = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        S15_CHECK(hVLine != nullptr && hFrame != nullptr, "Create VLine & Frame events");

        S15_CHECK(SUCCEEDED(DXGIXSetVLineNotification(0u, 540u, hVLine)), "Register VLine notification");
        S15_CHECK(SUCCEEDED(DXGIXSetFrameNotification(0u, hFrame)), "Register Frame notification");
        S15_CHECK(WaitForSingleObject(hVLine, 0) == WAIT_TIMEOUT, "VLine unsignaled before PresentX");
        S15_CHECK(WaitForSingleObject(hFrame, 0) == WAIT_TIMEOUT, "Frame unsignaled before PresentX");

        S15_CHECK(SUCCEEDED(queue->PresentX(0u, nullptr, nullptr)), "PresentX(0) triggers notifications");
        S15_CHECK(WaitForSingleObject(hVLine, 0) == WAIT_OBJECT_0, "VLine signaled on PresentX completion");
        S15_CHECK(WaitForSingleObject(hFrame, 0) == WAIT_OBJECT_0, "Frame signaled on PresentX completion");

        // Unregister cleanly and confirm neither fires on subsequent PresentX
        S15_CHECK(SUCCEEDED(DXGIXSetVLineNotification(0u, 0u, nullptr)), "Unregister VLine notification");
        S15_CHECK(SUCCEEDED(DXGIXSetFrameNotification(0u, nullptr)), "Unregister Frame notification");
        ResetEvent(hVLine);
        ResetEvent(hFrame);
        S15_CHECK(SUCCEEDED(queue->PresentX(0u, nullptr, nullptr)), "PresentX(0) after unregister");
        S15_CHECK(WaitForSingleObject(hVLine, 0) == WAIT_TIMEOUT, "VLine remains unsignaled after unregister");
        S15_CHECK(WaitForSingleObject(hFrame, 0) == WAIT_TIMEOUT, "Frame remains unsignaled after unregister");

        if (hVLine) CloseHandle(hVLine);
        if (hFrame) CloseHandle(hFrame);
    }

    // Verify ALL parity counters populated cleanly
    D3D12XBOX_GRAPHICS_PARITY_STATS pstats{};
    if (parity) {
        S15_CHECK(SUCCEEDED(parity->GetGraphicsParityStats(&pstats)), "Final GetGraphicsParityStats");
        S15_CHECK(pstats.console_flags_stripped >= 1ull, "console_flags_stripped >= 1");
        S15_CHECK(pstats.reserved_cross_adapter_preserved + pstats.reserved_cross_adapter_stripped_fallback >= 2ull,
                  "CreateReservedResource ALLOW_CROSS_ADAPTER handled");
        S15_CHECK(pstats.global_uav_barriers_recorded >= 1ull, "global_uav_barriers_recorded >= 1");
        S15_CHECK(pstats.unsupported_cmdlist_extensions_ignored >= 2ull, "unsupported_cmdlist_extensions_ignored >= 2");
        S15_CHECK(pstats.zero_count_noop_calls >= 8ull, "zero_count_noop_calls >= 8");
        S15_CHECK(pstats.readonly_dsv_creations >= 2ull, "readonly_dsv_creations >= 2");
        S15_CHECK(pstats.depth_stencil_plane_copies >= 2ull, "depth_stencil_plane_copies >= 2");
        S15_CHECK(pstats.footprint_offset_copies >= 3ull, "footprint_offset_copies >= 3");
        S15_CHECK(pstats.vline_notifications_signaled >= 1ull, "vline_notifications_signaled >= 1");
        S15_CHECK(pstats.frame_notifications_signaled >= 1ull, "frame_notifications_signaled >= 1");
        std::printf("  PASS: Parity stats | promo=%llu msaaClamp=%llu flagStrip=%llu xAdapter=%llu/%llu uavAll=%llu extSkip=%llu noops=%llu roDsv=%llu dsCopy=%llu fpOff=%llu vline=%llu frame=%llu\n",
                    static_cast<unsigned long long>(pstats.promoted_oversized_placed_resources),
                    static_cast<unsigned long long>(pstats.msaa_quality_clamps),
                    static_cast<unsigned long long>(pstats.console_flags_stripped),
                    static_cast<unsigned long long>(pstats.reserved_cross_adapter_preserved),
                    static_cast<unsigned long long>(pstats.reserved_cross_adapter_stripped_fallback),
                    static_cast<unsigned long long>(pstats.global_uav_barriers_recorded),
                    static_cast<unsigned long long>(pstats.unsupported_cmdlist_extensions_ignored),
                    static_cast<unsigned long long>(pstats.zero_count_noop_calls),
                    static_cast<unsigned long long>(pstats.readonly_dsv_creations),
                    static_cast<unsigned long long>(pstats.depth_stencil_plane_copies),
                    static_cast<unsigned long long>(pstats.footprint_offset_copies),
                    static_cast<unsigned long long>(pstats.vline_notifications_signaled),
                    static_cast<unsigned long long>(pstats.frame_notifications_signaled));
        parity->Release();
    }
}

// ===========================================================================
// 3. Vulkan Physical Device Ranking, OPENSCARLETT_VK_GPU & Global UAV Barrier
// ===========================================================================
void TestVulkanRankingSelectorAndGlobalUavBarrier() {
    std::printf("[Test 3] Vulkan Physical Device Ranking, OPENSCARLETT_VK_GPU & Global UAV Barrier (AnyBox #68, xcl #68)...\n");

    auto make_api = [](uint32_t maj, uint32_t min, uint32_t pat) -> uint32_t {
        return (maj << 22u) | (min << 12u) | pat;
    };

    // 3A. Deterministic ranking hierarchy: Discrete > Integrated > Virtual > CPU > Other,
    // weighted by graphics+compute, VK_EXT_mesh_shader, VRAM size, and Vulkan API version.
    std::vector<openscarlett::VulkanPhysicalDeviceCandidate> synth;
    {
        openscarlett::VulkanPhysicalDeviceCandidate cpu{};
        cpu.enumerated_index = 0u;
        cpu.device_name = "Lavapipe Software CPU";
        cpu.device_type = openscarlett::VulkanDeviceType::Cpu;
        cpu.api_version = make_api(1, 3, 280);
        cpu.device_local_vram_bytes = 32ull * 1024ull * 1024ull * 1024ull;
        cpu.supports_compute = true;
        cpu.supports_graphics = true;
        cpu.supports_mesh_shader = true;
        synth.push_back(cpu);

        openscarlett::VulkanPhysicalDeviceCandidate igpu{};
        igpu.enumerated_index = 1u;
        igpu.device_name = "Intel Arc Integrated GPU";
        igpu.device_type = openscarlett::VulkanDeviceType::IntegratedGpu;
        igpu.api_version = make_api(1, 4, 0);
        igpu.device_local_vram_bytes = 8ull * 1024ull * 1024ull * 1024ull;
        igpu.supports_compute = true;
        igpu.supports_graphics = true;
        igpu.supports_mesh_shader = true;
        synth.push_back(igpu);

        openscarlett::VulkanPhysicalDeviceCandidate dgpu_older{};
        dgpu_older.enumerated_index = 2u;
        dgpu_older.device_name = "AMD Radeon RX 6700 XT";
        dgpu_older.device_type = openscarlett::VulkanDeviceType::DiscreteGpu;
        dgpu_older.api_version = make_api(1, 3, 250);
        dgpu_older.device_local_vram_bytes = 12ull * 1024ull * 1024ull * 1024ull;
        dgpu_older.supports_compute = true;
        dgpu_older.supports_graphics = true;
        dgpu_older.supports_mesh_shader = false;
        synth.push_back(dgpu_older);

        openscarlett::VulkanPhysicalDeviceCandidate dgpu_best{};
        dgpu_best.enumerated_index = 3u;
        dgpu_best.device_name = "NVIDIA GeForce RTX 4090";
        dgpu_best.device_type = openscarlett::VulkanDeviceType::DiscreteGpu;
        dgpu_best.api_version = make_api(1, 4, 300);
        dgpu_best.device_local_vram_bytes = 24ull * 1024ull * 1024ull * 1024ull;
        dgpu_best.supports_compute = true;
        dgpu_best.supports_graphics = true;
        dgpu_best.supports_mesh_shader = true;
        synth.push_back(dgpu_best);

        openscarlett::VulkanPhysicalDeviceCandidate vgpu{};
        vgpu.enumerated_index = 4u;
        vgpu.device_name = "VirtIO Virtual GPU";
        vgpu.device_type = openscarlett::VulkanDeviceType::VirtualGpu;
        vgpu.api_version = make_api(1, 3, 0);
        vgpu.device_local_vram_bytes = 4ull * 1024ull * 1024ull * 1024ull;
        vgpu.supports_compute = true;
        vgpu.supports_graphics = true;
        synth.push_back(vgpu);
    }

    const auto ranked = openscarlett::VulkanBackend::RankPhysicalDeviceCandidates(synth);
    S15_CHECK(ranked.size() == 5u, "Ranked size == 5");
    S15_CHECK(ranked[0].enumerated_index == 3u, "Rank #0 is Discrete+Mesh RTX 4090");
    S15_CHECK(ranked[1].enumerated_index == 2u, "Rank #1 is Discrete RX 6700 XT");
    S15_CHECK(ranked[2].enumerated_index == 1u, "Rank #2 is Integrated Intel Arc");
    S15_CHECK(ranked[3].enumerated_index == 4u, "Rank #3 is Virtual GPU");
    S15_CHECK(ranked[4].enumerated_index == 0u, "Rank #4 is CPU Lavapipe");
    for (size_t i = 1u; i < ranked.size(); ++i) {
        S15_CHECK(ranked[i - 1u].rank_score > ranked[i].rank_score, "Strictly decreasing scores");
    }

    // 3B. Selector tests (empty, numeric index, case-insensitive substring, invalid)
    openscarlett::VulkanPhysicalDeviceCandidate chosen{};
    bool matched = false;
    S15_CHECK(openscarlett::VulkanBackend::SelectPhysicalDeviceCandidate(synth, "", chosen, &matched) &&
                  chosen.enumerated_index == 3u && !matched,
              "Empty selector picks highest-ranked GPU");
    S15_CHECK(openscarlett::VulkanBackend::SelectPhysicalDeviceCandidate(synth, "1", chosen, &matched) &&
                  chosen.enumerated_index == 1u && matched,
              "Numeric index selector '1' picks enumerated_index 1");
    S15_CHECK(openscarlett::VulkanBackend::SelectPhysicalDeviceCandidate(synth, "radeon", chosen, &matched) &&
                  chosen.enumerated_index == 2u && matched,
              "Case-insensitive substring 'radeon' picks RX 6700 XT");
    S15_CHECK(!openscarlett::VulkanBackend::SelectPhysicalDeviceCandidate(synth, "99", chosen, &matched),
              "Out-of-range numeric selector fails cleanly");
    S15_CHECK(!openscarlett::VulkanBackend::SelectPhysicalDeviceCandidate(synth, "NonExistentGpuXYZ", chosen, &matched),
              "Unmatched substring selector fails cleanly");

    // 3C. Live Vulkan ICD enumeration, OPENSCARLETT_VK_GPU selection & GlobalUavBarrier execution!
    std::vector<openscarlett::VulkanPhysicalDeviceCandidate> live_gpus;
    const openscarlett::Status enum_st = openscarlett::VulkanBackend::EnumerateRankedPhysicalDevices(live_gpus);
    if (enum_st.ok() && !live_gpus.empty()) {
        for (size_t i = 1u; i < live_gpus.size(); ++i) {
            S15_CHECK(live_gpus[i - 1u].rank_score >= live_gpus[i].rank_score,
                      "Live Vulkan devices ranked descending");
        }

        // Preserve any pre-existing OPENSCARLETT_VK_GPU value
        char prev_env[256]{};
        const DWORD prev_len = GetEnvironmentVariableA("OPENSCARLETT_VK_GPU", prev_env, sizeof(prev_env));

        // Select by numeric index of top GPU
        const std::string idx_str = std::to_string(live_gpus[0].enumerated_index);
        SetEnvironmentVariableA("OPENSCARLETT_VK_GPU", idx_str.c_str());
        {
            openscarlett::VulkanBackend vk;
            S15_CHECK(vk.Initialize(false).ok(), "VulkanBackend::Initialize with numeric OPENSCARLETT_VK_GPU");
            S15_CHECK(vk.GetDeviceInfo().matched_device_filter, "Matched numeric OPENSCARLETT_VK_GPU");
            S15_CHECK(vk.GetDeviceInfo().enumerated_device_index == live_gpus[0].enumerated_index,
                      "Selected exact GPU by index");
            S15_CHECK(vk.GetDeviceInfo().device_rank_score == live_gpus[0].rank_score,
                      "Populated device_rank_score");

            // Test GlobalUavBarrier live on Vulkan ICD!
            openscarlett::VulkanCommandListDesc cld{};
            openscarlett::VulkanCommandItem item{};
            item.op = openscarlett::VulkanCommandOp::GlobalUavBarrier;
            cld.commands.push_back(item);
            openscarlett::VulkanCommandListResult clr{};
            S15_CHECK(vk.RecordCommandList(cld, clr).ok(), "Vulkan RecordCommandList(GlobalUavBarrier) live");
            S15_CHECK(clr.global_uav_barrier_count == 1u && clr.barrier_count == 1u,
                      "GlobalUavBarrier counted");
            S15_CHECK(clr.last_global_uav_src_access_mask == (0x00008000u | 0x00010000u) &&
                          clr.last_global_uav_dst_access_mask == (0x00008000u | 0x00010000u),
                      "GlobalUavBarrier uses full VK_ACCESS_MEMORY_READ|WRITE masks");
            S15_CHECK((clr.last_global_uav_src_stage_mask & (0x00008000u | 0x00000800u)) != 0u,
                      "GlobalUavBarrier covers graphics + compute stages");
        }

        // Select by case-insensitive device name substring
        std::string substr = live_gpus[0].device_name.substr(0u, std::min<size_t>(5u, live_gpus[0].device_name.size()));
        for (char& ch : substr) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        SetEnvironmentVariableA("OPENSCARLETT_VK_GPU", substr.c_str());
        {
            openscarlett::VulkanBackend vk;
            S15_CHECK(vk.Initialize(false).ok(), "VulkanBackend::Initialize with substring OPENSCARLETT_VK_GPU");
            S15_CHECK(vk.GetDeviceInfo().matched_device_filter, "Matched substring OPENSCARLETT_VK_GPU");
        }

        SetEnvironmentVariableA("OPENSCARLETT_VK_GPU", prev_len > 0u ? prev_env : nullptr);
        std::printf("  PASS: Live Vulkan enumerated %zu GPU(s) | top='%s' VRAM=%llu MiB | GlobalUavBarrier live PASS\n",
                    live_gpus.size(), live_gpus[0].device_name.c_str(),
                    static_cast<unsigned long long>(live_gpus[0].device_local_vram_bytes / (1024ull * 1024ull)));
    } else {
        // Software fallback verification when host has no Vulkan ICD
        openscarlett::VulkanBackend vk;
        S15_CHECK(vk.InitializeSoftwareFallback().ok(), "Vulkan software fallback init");
        openscarlett::VulkanCommandListDesc cld{};
        openscarlett::VulkanCommandItem item{};
        item.op = openscarlett::VulkanCommandOp::GlobalUavBarrier;
        cld.commands.push_back(item);
        openscarlett::VulkanCommandListResult clr{};
        S15_CHECK(vk.RecordCommandList(cld, clr).ok(), "Vulkan fallback GlobalUavBarrier");
        S15_CHECK(clr.global_uav_barrier_count == 1u &&
                      clr.last_global_uav_src_access_mask == (0x00008000u | 0x00010000u),
                  "Fallback GlobalUavBarrier masks exact");
        std::printf("  PASS: Synthetic Vulkan ranking + fallback GlobalUavBarrier PASS\n");
    }
}

// ===========================================================================
// 4. XG 3D Packed Mip Tails & Unpacked Tiled 3D Mip Chains (TuxBox 949862a/d2e5108)
// ===========================================================================
void TestXg3dPackedAndUnpackedMipChains() {
    std::printf("[Test 4] XG 3D Packed Mip Tails & Unpacked Tiled 3D Mip Chains (TuxBox 949862a/d2e5108)...\n");

    const std::array<XG_TILE_MODE, 5> sw64k_3d_modes = {
        XG_TILE_MODE_3D_THICK_64KB_S,
        XG_TILE_MODE_3D_THICK_64KB_Z,
        XG_TILE_MODE_3D_THICK_64KB_S_X,
        XG_TILE_MODE_3D_THICK_64KB_Z_X,
        XG_TILE_MODE_3D_THICK_64KB_R_X,
    };

    const std::array<uint32_t, 5> formats = {
        DXGI_FORMAT_R8_UNORM,
        DXGI_FORMAT_R16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_BC7_UNORM,
    };

    struct VolCase {
        uint32_t w, h, d, mips;
    };
    const std::array<VolCase, 2> cases = {{
        {64u, 64u, 32u, 7u},  // Power-of-two full 7-mip chain
        {37u, 29u, 19u, 6u},  // NPOT full 6-mip chain
    }};

    for (const auto& vc : cases) {
        for (XG_TILE_MODE mode : sw64k_3d_modes) {
            for (uint32_t fmt : formats) {
                XG_TEXTURE3D_DESC desc{};
                desc.Width = vc.w;
                desc.Height = vc.h;
                desc.Depth = vc.d;
                desc.MipLevels = vc.mips;
                desc.Format = fmt;
                desc.TileMode = mode;

                XG_RESOURCE_LAYOUT packed_layout{};
                openscarlett::xg::Xg3dMipChainSummary packed_sum{};
                S15_CHECK(openscarlett::xg::ComputeTexture3DLayoutEx(
                              desc, openscarlett::xg::Xg3dMipTailMode::PackedMipTail,
                              packed_layout, &packed_sum),
                          "ComputeTexture3DLayoutEx PackedMipTail");
                S15_CHECK(openscarlett::xg::ValidateTexture3DLayoutBounds(packed_layout),
                          "ValidateTexture3DLayoutBounds packed");
                S15_CHECK(packed_sum.packed_mip_count > 0u, "Packed 3D mip chain has packed tail mips");
                S15_CHECK(packed_sum.mip_tail_size_bytes == 65536ull, "64KB 3D mip tail size == 65536");

                XG_RESOURCE_LAYOUT unpacked_layout{};
                openscarlett::xg::Xg3dMipChainSummary unpacked_sum{};
                S15_CHECK(openscarlett::xg::ComputeTexture3DLayoutEx(
                              desc, openscarlett::xg::Xg3dMipTailMode::UnpackedTiledMips,
                              unpacked_layout, &unpacked_sum),
                          "ComputeTexture3DLayoutEx UnpackedTiledMips");
                S15_CHECK(openscarlett::xg::ValidateTexture3DLayoutBounds(unpacked_layout),
                          "ValidateTexture3DLayoutBounds unpacked");
                S15_CHECK(unpacked_sum.packed_mip_count == 0u && unpacked_sum.unpacked_mip_count == vc.mips,
                          "Unpacked 3D mip chain has 0 packed mips & all mips unpacked");
                S15_CHECK(packed_layout.SizeBytes < unpacked_layout.SizeBytes,
                          "Packed 3D mip tail saves memory vs unpacked 3D mip chain");

                // Verify lossless round-trip across ALL mip levels for BOTH packed & unpacked layouts!
                for (int pass = 0; pass < 2; ++pass) {
                    const XG_RESOURCE_LAYOUT& lay = (pass == 0) ? packed_layout : unpacked_layout;
                    std::vector<uint8_t> tiled(static_cast<size_t>(lay.SizeBytes), 0xCCu);
                    std::vector<std::vector<uint8_t>> srcs(lay.MipLevels);

                    for (uint32_t m = 0u; m < lay.MipLevels; ++m) {
                        const XG_MIPLEVEL_LAYOUT& ml = lay.Mips[m];
                        S15_CHECK(ml.DepthPitchBytes > 0ull, "3D mip DepthPitchBytes populated");
                        const uint32_t bpe = lay.FormatInfo.bytes_per_block;
                        const uint32_t rp = ml.WidthElements * bpe;
                        const uint32_t sp = rp * ml.HeightElements;
                        srcs[m].resize(static_cast<size_t>(sp) * ml.DepthElements);
                        FillPattern(srcs[m], static_cast<uint32_t>(mode) * 1000u + fmt * 37u + m * 17u + pass);
                        S15_CHECK(openscarlett::xg::TileSurface3DWithLayout(
                                      lay, m, srcs[m].data(), rp, sp, tiled.data(), tiled.size()),
                                  "TileSurface3DWithLayout");
                    }

                    for (uint32_t m = 0u; m < lay.MipLevels; ++m) {
                        const XG_MIPLEVEL_LAYOUT& ml = lay.Mips[m];
                        const uint32_t bpe = lay.FormatInfo.bytes_per_block;
                        const uint32_t rp = ml.WidthElements * bpe;
                        const uint32_t sp = rp * ml.HeightElements;
                        std::vector<uint8_t> dst(srcs[m].size(), 0u);
                        S15_CHECK(openscarlett::xg::UntileSurface3DWithLayout(
                                      lay, m, tiled.data(), tiled.size(), dst.data(), rp, sp),
                                  "UntileSurface3DWithLayout");
                        S15_CHECK(std::memcmp(srcs[m].data(), dst.data(), srcs[m].size()) == 0,
                                  "3D mip chain round-trip bit-exact");
                    }
                }
            }
        }
    }

    // Bounds-check rejection tests
    {
        XG_TEXTURE3D_DESC d{32u, 32u, 16u, 6u, DXGI_FORMAT_R8G8B8A8_UNORM, 0u, XG_TILE_MODE_3D_THICK_64KB_S_X};
        XG_RESOURCE_LAYOUT lay{};
        S15_CHECK(openscarlett::xg::ComputeTexture3DLayoutEx(
                      d, openscarlett::xg::Xg3dMipTailMode::PackedMipTail, lay, nullptr),
                  "Compute layout for bounds test");
        std::vector<uint8_t> dummy(16u, 0u);
        std::vector<uint8_t> out_lin(32u * 32u * 16u * 4u, 0u);
        S15_CHECK(!openscarlett::xg::UntileSurface3DWithLayout(lay, 0u, dummy.data(), dummy.size(), out_lin.data(), 0u, 0u),
                  "Reject undersized tiled buffer");
        XG_RESOURCE_LAYOUT corrupt = lay;
        corrupt.Mips[corrupt.MipTailStartLevel].MipTailSubTileOffsetBytes = 65536u;
        S15_CHECK(!openscarlett::xg::ValidateTexture3DLayoutBounds(corrupt),
                  "Reject corrupted mip-tail sub-offset beyond 64KB slice");
    }
    std::printf("  PASS: All 6 SW_64KB_* 3D modes x 5 formats x POT/NPOT packed & unpacked mip chains BIT-EXACT!\n");
}

// ===========================================================================
// 5. BC6H Bit-Exact Decoder + Signed-Zero + BCPack Transitions + 4 GiB TTPF/rANS
// ===========================================================================
void TestBc6hSignedZeroBcpTransitionsAndTtpfRans() {
    std::printf("[Test 5] BC6H Bit-Exact Decoder + Signed-Zero + BCPack Transitions + 4 GiB TTPF/rANS...\n");

    // 5A. Exact FinishUnquantize signed-zero rule (SixNG #24)
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(-1, true) == 0x0000u,
              "SF16 FinishUnquantize(-1) MUST be +0.0 (0x0000), NOT -0.0 (0x8000)!");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(-2, true) == 0x8001u,
              "SF16 FinishUnquantize(-2) == 0x8001 (-subnormal)");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(0, true) == 0x0000u, "SF16 FinishUnquantize(0) == 0x0000");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(0, false) == 0x0000u, "UF16 FinishUnquantize(0) == 0x0000");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(1, true) == 0x0000u, "SF16 FinishUnquantize(+1) == 0x0000");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(2, true) == 0x0001u, "SF16 FinishUnquantize(+2) == 0x0001");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(0xFFFF, false) == 0x7BFFu, "UF16 max == 0x7BFF (FLT16_MAX)");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(0x7FFF, true) == 0x7BFFu, "SF16 +max == 0x7BFF");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(-0x7FFF, true) == 0xFBFFu, "SF16 -max == 0xFBFF");
    S15_CHECK(openscarlett::Bc6hFinishUnquantize(-32768, true) == 0xFBFFu, "SF16 -32768 clamped safely to 0xFBFF");

    // 5B. Mode 13 (16.4, 1-region) SF16 signed-zero block decoding + reversed high bits rw[15:10]
    {
        uint64_t lo = 0ull, hi = 0ull;
        WriteBits128(lo, hi, 0, 5, 0x0Fu);  // Mode 13
        // R: rw = -1 (0xFFFF), delta = +1 -> e1 = 0 -> texel 0 (w=0) has interp=-1 -> +0.0 (0x0000)!
        // G: gw = -2 (0xFFFE), delta = 0 -> e1 = -2 -> texel 0 has interp=-2 -> 0x8001!
        // B: bw = +15360 (0x3C00 in 16b raw; FinishUnquantize(15360)=14880=0x3A20), delta = +4 -> e1 = 15364
        const uint32_t rw = 0xFFFFu;
        const uint32_t gw = 0xFFFEu;
        const uint32_t bw = 0x3C00u;
        WriteBits128(lo, hi, 5, 10, rw & 0x3FFu);
        WriteBitsReversed128(lo, hi, 39, 6, (rw >> 10u) & 0x3Fu);
        WriteBits128(lo, hi, 35, 4, 1u);   // rx delta = +1
        WriteBits128(lo, hi, 15, 10, gw & 0x3FFu);
        WriteBitsReversed128(lo, hi, 49, 6, (gw >> 10u) & 0x3Fu);
        WriteBits128(lo, hi, 45, 4, 0u);   // gx delta = 0
        WriteBits128(lo, hi, 25, 10, bw & 0x3FFu);
        WriteBitsReversed128(lo, hi, 59, 6, (bw >> 10u) & 0x3Fu);
        WriteBits128(lo, hi, 55, 4, 4u);   // bx delta = +4

        // Set texel 0 index=0 (3 bits at 65..67), texel 1 index=15 (4 bits at 68..71 -> weight=64 -> e1!)
        WriteBits128(lo, hi, 65, 3, 0u);
        WriteBits128(lo, hi, 68, 4, 15u);

        uint8_t blk[16]{};
        StoreBlock128(lo, hi, blk);

        openscarlett::Bc6hBlockInfo bi{};
        S15_CHECK(openscarlett::Bc6hInspectBlockMode(blk, bi) && bi.mode_index == 13u &&
                      bi.region_count == 1u && bi.endpoint_bits == 16u && bi.is_transformed,
                  "Inspect Mode 13 metadata");

        uint16_t rgb[48]{};
        openscarlett::Bc6hDecodeBlock(blk, /*is_signed=*/true, rgb);
        S15_CHECK(rgb[0] == 0x0000u, "Mode 13 SF16 texel 0 R (-1) MUST decode to +0.0 (0x0000), NOT -0.0");
        S15_CHECK(rgb[1] == 0x8001u, "Mode 13 SF16 texel 0 G (-2) decodes to 0x8001");
        S15_CHECK(rgb[2] == openscarlett::Bc6hFinishUnquantize(0x3C00, true), "Mode 13 SF16 texel 0 B exact");
        S15_CHECK(rgb[3] == 0x0000u, "Mode 13 SF16 texel 1 R (e1=0) == 0x0000");
        S15_CHECK(rgb[4] == 0x8001u, "Mode 13 SF16 texel 1 G (-2) == 0x8001");
        S15_CHECK(rgb[5] == openscarlett::Bc6hFinishUnquantize(0x3C04, true), "Mode 13 SF16 texel 1 B (e1=0x3C04) exact");
    }

    // 5C. Mode 12 (12.8, 1-region) UF16 reversed high bits rw[11:10] verification
    {
        uint64_t lo = 0ull, hi = 0ull;
        WriteBits128(lo, hi, 0, 5, 0x0Bu);  // Mode 12
        // Set rw = 0xFFF (max 12-bit -> Unquantize=0xFFFF -> FinishUnquantize=0x7BFF)
        // Set gw = 0x800 (rw[11]=1, rw[10]=0 -> tests exact bit-reversal order!)
        // Set bw = 0x400 (bw[11]=0, bw[10]=1 -> opposite bit-reversal order!)
        WriteBits128(lo, hi, 5, 10, 0x3FFu);
        WriteBitsReversed128(lo, hi, 43, 2, 0x3u);
        WriteBits128(lo, hi, 15, 10, 0x000u);
        WriteBitsReversed128(lo, hi, 53, 2, 0x2u);  // 0x800
        WriteBits128(lo, hi, 25, 10, 0x000u);
        WriteBitsReversed128(lo, hi, 63, 2, 0x1u);  // 0x400

        uint8_t blk[16]{};
        StoreBlock128(lo, hi, blk);
        uint16_t rgb[48]{};
        openscarlett::Bc6hDecodeBlock(blk, /*is_signed=*/false, rgb);
        const int32_t unq_g = ((0x800 << 16) + 0x8000) >> 12;
        const int32_t unq_b = ((0x400 << 16) + 0x8000) >> 12;
        S15_CHECK(rgb[0] == 0x7BFFu, "Mode 12 UF16 rw=0xFFF -> 0x7BFF");
        S15_CHECK(rgb[1] == openscarlett::Bc6hFinishUnquantize(unq_g, false), "Mode 12 UF16 gw=0x800 reversed high bits exact");
        S15_CHECK(rgb[2] == openscarlett::Bc6hFinishUnquantize(unq_b, false), "Mode 12 UF16 bw=0x400 reversed high bits exact");
    }

    // 5D. All 14 valid BC6H modes + 4 reserved modes inspection, decode & 2-region partition test
    {
        static constexpr uint8_t kModePrefixes[14] = {
            0x00u, 0x01u, 0x02u, 0x06u, 0x0Au, 0x0Eu, 0x12u,
            0x16u, 0x1Au, 0x1Eu, 0x03u, 0x07u, 0x0Bu, 0x0Fu,
        };
        static constexpr uint8_t kReservedPrefixes[4] = {0x13u, 0x17u, 0x1Bu, 0x1Fu};

        // Test 2-region Mode 0 with partition 17 (anchor2 = 2; subset 1 at texels 1,2,3,7)
        {
            uint64_t lo = 0ull, hi = 0ull;
            WriteBits128(lo, hi, 0, 2, 0x00u);   // Mode 0
            WriteBits128(lo, hi, 5, 10, 512u);   // rw = 512
            WriteBits128(lo, hi, 15, 10, 256u);  // gw = 256
            WriteBits128(lo, hi, 25, 10, 128u);  // bw = 128
            WriteBits128(lo, hi, 65, 5, 10u);    // ry delta = +10 -> 522
            WriteBits128(lo, hi, 77, 5, 17u);    // partition 17 (texel 0 -> subset 0, texel 2 -> subset 1)
            uint8_t blk[16]{};
            StoreBlock128(lo, hi, blk);
            uint16_t rgb[48]{};
            openscarlett::Bc6hDecodeBlock(blk, false, rgb);
            const int32_t unq_r0 = ((512 << 16) + 0x8000) >> 10;
            const int32_t unq_r1 = ((522 << 16) + 0x8000) >> 10;
            S15_CHECK(rgb[0 * 3 + 0] == openscarlett::Bc6hFinishUnquantize(unq_r0, false),
                      "Mode 0 partition 17 subset 0 R exact");
            S15_CHECK(rgb[2 * 3 + 0] == openscarlett::Bc6hFinishUnquantize(unq_r1, false),
                      "Mode 0 partition 17 subset 1 (anchor 2) R exact");
        }

        // Build a 16x16 BC6H surface (16 blocks) cycling through ALL 14 valid modes + reserved modes
        // with rapid 1-region <-> 2-region transitions (SixNG #15/#20/#21)!
        std::vector<uint8_t> bc6h_surface(64u * 16u, 0u);  // 32x32 pixels = 64 blocks
        for (uint32_t b = 0u; b < 64u; ++b) {
            uint8_t* blk = bc6h_surface.data() + static_cast<size_t>(b) * 16u;
            // Alternate 1-region (modes 10..13) and 2-region (modes 0..9) + reserved modes
            uint8_t prefix = 0u;
            if ((b % 16u) == 15u) {
                prefix = kReservedPrefixes[(b / 16u) & 3u];
            } else if ((b & 1u) == 0u) {
                prefix = kModePrefixes[10u + ((b / 2u) % 4u)];  // 1-region
            } else {
                prefix = kModePrefixes[(b / 2u) % 10u];  // 2-region
            }
            for (uint32_t k = 1u; k < 16u; ++k) {
                blk[k] = static_cast<uint8_t>((b * 37u + k * 53u) & 0xFFu);
            }
            blk[0] = static_cast<uint8_t>((prefix & 0x1Fu) | ((b * 7u) & 0xE0u));

            openscarlett::Bc6hBcpPresetInfo pinfo{};
            const bool valid = openscarlett::Bc6hResolveBcpPreset(blk, pinfo);
            if ((b % 16u) == 15u) {
                S15_CHECK(!valid && pinfo.preset_index == 14u && pinfo.region_count == 0u,
                          "Reserved BC6H mode maps safely to preset 14");
                uint16_t rgb[48]{};
                openscarlett::Bc6hDecodeBlock(blk, true, rgb);
                bool all_zero = true;
                for (uint16_t v : rgb) {
                    if (v != 0u) all_zero = false;
                }
                S15_CHECK(all_zero, "Reserved BC6H block decodes to exact 0x0000");
            } else {
                S15_CHECK(valid && pinfo.preset_index < 14u &&
                              pinfo.region_count == (((b & 1u) == 0u) ? 1u : 2u),
                          "Valid BC6H mode preset & region count exact");
            }
        }

        // Decode whole surface (UF16 & SF16)
        std::vector<uint16_t> rgba_f16;
        S15_CHECK(openscarlett::Bc6hDecodeSurface(bc6h_surface.data(), bc6h_surface.size(), 32u, 32u, false, rgba_f16).ok(),
                  "Bc6hDecodeSurface UF16");
        S15_CHECK(rgba_f16.size() == 32u * 32u * 4u && rgba_f16[3] == 0x3C00u, "Bc6hDecodeSurface output dims & alpha");

        // Round-trip 1-region <-> 2-region mixed BC6H surface through BOTH container BCPack and HW BCPack!
        std::vector<uint8_t> packed_container;
        std::vector<uint8_t> unpacked_container;
        S15_CHECK(openscarlett::BcPackCompress(
                      openscarlett::BcFormat::BC6H, 32u, 32u,
                      bc6h_surface.data(), bc6h_surface.size(), packed_container).ok(),
                  "BcPackCompress mixed 1r/2r BC6H");
        S15_CHECK(openscarlett::BcPackDecompress(
                      packed_container.data(), packed_container.size(), unpacked_container).ok(),
                  "BcPackDecompress mixed 1r/2r BC6H");
        S15_CHECK(unpacked_container == bc6h_surface, "Container BCPack mixed 1r/2r BC6H bit-exact");

        std::vector<uint8_t> hw_bs;
        std::vector<uint8_t> hw_out(bc6h_surface.size(), 0u);
        size_t hw_written = 0u;
        S15_CHECK(openscarlett::BcPackHwEncodeBitstream(
                      openscarlett::BcFormat::BC6H, bc6h_surface.data(), bc6h_surface.size(), hw_bs, 8u).ok(),
                  "BcPackHwEncodeBitstream mixed 1r/2r BC6H");
        S15_CHECK(openscarlett::BcPackHwDecodeBitstream(
                      openscarlett::BcFormat::BC6H, hw_bs.data(), hw_bs.size(),
                      hw_out.data(), hw_out.size(), &hw_written).ok(),
                  "BcPackHwDecodeBitstream mixed 1r/2r BC6H");
        S15_CHECK(hw_written == bc6h_surface.size() && hw_out == bc6h_surface,
                  "HW BCPack mixed 1r/2r BC6H transitions bit-exact");
    }

    // 5E. 64-bit / >4 GiB TTPF codec-mask & strict bounds-checked rANS (bcpack-rs c51ec52)
    {
        S15_CHECK(openscarlett::TtpfComputeCodecBitMask(0u) == 1ull, "TTPF mask bit 0");
        S15_CHECK(openscarlett::TtpfComputeCodecBitMask(31u) == (1ull << 31u), "TTPF mask bit 31");
        S15_CHECK(openscarlett::TtpfComputeCodecBitMask(32u) == (1ull << 32u), "TTPF mask bit 32 (no 32-bit truncation!)");
        S15_CHECK(openscarlett::TtpfComputeCodecBitMask(47u) == (1ull << 47u), "TTPF mask bit 47");
        S15_CHECK(openscarlett::TtpfComputeCodecBitMask(63u) == (1ull << 63u), "TTPF mask bit 63");
        S15_CHECK(openscarlett::TtpfComputeCodecBitMask(64u) == 0ull, "TTPF mask bit 64 rejected safely");

        const uint64_t mask_hi =
            openscarlett::TtpfComputeCodecBitMask(40u) | openscarlett::TtpfComputeCodecBitMask(63u);
        openscarlett::TtpfCodecMaskInfo tinfo{};
        S15_CHECK(openscarlett::TtpfValidatePayloadBounds(
                      6ull << 30u, 5ull << 30u, 512ull << 20u, mask_hi, &tinfo),
                  "TTPF >4 GiB payload validated cleanly in 64-bit");
        S15_CHECK(tinfo.exceeds_4gib && tinfo.active_codec_count == 2u && tinfo.codec_mask == mask_hi,
                  "TTPF >4 GiB metadata exact");
        S15_CHECK(!openscarlett::TtpfValidatePayloadBounds(
                      6ull << 30u, UINT64_MAX - 16ull, 64ull, mask_hi, nullptr),
                  "TTPF rejects 64-bit offset+size overflow");
        S15_CHECK(!openscarlett::TtpfValidatePayloadBounds(
                      6ull << 30u, 5ull << 30u, 2ull << 30u, mask_hi, nullptr),
                  "TTPF rejects tile extending past logical_size");
        S15_CHECK(!openscarlett::TtpfValidatePayloadBounds(
                      6ull << 30u, 0ull, 4096ull, 0ull, nullptr),
                  "TTPF rejects zero codec_mask");

        // rANS lossless round-trip (empty, uniform, structured, pseudo-random)
        const std::array<size_t, 4> sizes = {0u, 128u, 4096u, 16384u};
        for (size_t sz : sizes) {
            std::vector<uint8_t> plain(sz, 0u);
            if (sz == 128u) {
                std::fill(plain.begin(), plain.end(), static_cast<uint8_t>(0x5Au));
            } else if (sz == 4096u) {
                for (size_t i = 0u; i < sz; ++i) plain[i] = static_cast<uint8_t>((i / 7u) & 0x1Fu);
            } else if (sz > 0u) {
                FillPattern(plain, 0xBC60u + static_cast<uint32_t>(sz));
            }
            std::vector<uint8_t> enc;
            std::vector<uint8_t> dec;
            S15_CHECK(openscarlett::BcPackRansEncode(plain.data(), plain.size(), enc).ok(), "rANS encode");
            S15_CHECK(openscarlett::BcPackRansDecode(enc.data(), enc.size(), plain.size(), dec).ok(), "rANS decode");
            S15_CHECK(dec == plain, "rANS lossless round-trip exact");

            // Strict bounds fuzzing: EVERY truncated prefix must fail cleanly with zero OOB reads!
            if (sz == 4096u) {
                for (size_t cut = 0u; cut < enc.size(); ++cut) {
                    std::vector<uint8_t> bad_out;
                    S15_CHECK(!openscarlett::BcPackRansDecode(enc.data(), cut, plain.size(), bad_out).ok(),
                              "rANS rejects truncated stream cleanly");
                }
                // Corrupt frequency table sum != 4096
                std::vector<uint8_t> bad_freq = enc;
                bad_freq[12u] ^= 0x01u;
                std::vector<uint8_t> bad_out;
                S15_CHECK(!openscarlett::BcPackRansDecode(bad_freq.data(), bad_freq.size(), plain.size(), bad_out).ok(),
                          "rANS rejects corrupted frequency table");
                // Corrupt final state < lower bound
                std::vector<uint8_t> bad_state = enc;
                bad_state[12u + 512u + 3u] = 0u;
                S15_CHECK(!openscarlett::BcPackRansDecode(bad_state.data(), bad_state.size(), plain.size(), bad_out).ok(),
                          "rANS rejects state below lower bound");
            }
        }
    }

    std::printf("  PASS: BC6H bit-exact 14-mode + SF16 signed-zero + 1r/2r BCPack + >4GiB TTPF/rANS PASS!\n");
}

}  // namespace

int main() {
    std::printf("=== Sprint 15 Worker 3: D3D12X, Vulkan, XG 3D & BC6H/BCPack Parity Suite ===\n");

    // Create live D3D12X device (hardware GPU or WARP fallback)
    ID3D12DeviceX* dev = nullptr;
    HRESULT hr = D3D12CreateDeviceX(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX,
                                    reinterpret_cast<void**>(&dev));
    if (FAILED(hr) || !dev) {
        OpenScarlettD3D12SetPreferWarp(1);
        hr = D3D12CreateDeviceX(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX,
                                reinterpret_cast<void**>(&dev));
    }
    S15_CHECK(SUCCEEDED(hr) && dev != nullptr, "D3D12CreateDeviceX succeeded");

    if (dev) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueueX* queue = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueueX,
                                                    reinterpret_cast<void**>(&queue))) &&
                      queue != nullptr,
                  "CreateCommandQueue");

        ID3D12CommandAllocator* alloc = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&alloc))) &&
                      alloc != nullptr,
                  "CreateCommandAllocator");

        ID3D12GraphicsCommandListX* cl = nullptr;
        S15_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                                   IID_ID3D12GraphicsCommandListX,
                                                   reinterpret_cast<void**>(&cl))) &&
                      cl != nullptr,
                  "CreateCommandList");
        if (cl) cl->Close();

        uint64_t fence_val = 0ull;
        if (queue && alloc && cl) {
            TestOversizedPlacedTexturePromotion(dev, queue, alloc, cl, fence_val);
            TestD3D12XAndXboxExtensionHardening(dev, queue, alloc, cl, fence_val);
        }

        if (cl) cl->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
        dev->Release();
    }

    TestVulkanRankingSelectorAndGlobalUavBarrier();
    TestXg3dPackedAndUnpackedMipChains();
    TestBc6hSignedZeroBcpTransitionsAndTtpfRans();

    if (g_failures == 0) {
        std::printf("=== ALL SPRINT 15 GRAPHICS / XG 3D / BC6H TESTS PASSED (0 failures) ===\n");
        return 0;
    }
    std::fprintf(stderr, "=== FAILED: %d check(s) failed ===\n", g_failures);
    return 1;
}
