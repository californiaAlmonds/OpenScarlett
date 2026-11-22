#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

#include "command_list_x.h"
#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "os_check.h"

// Forward declarations of Sprint 13 clean-room helpers linked from openscarlett_core
// (src/gpu/xg_layout.cpp & src/gpu/xg_gpu_untile.cpp).
namespace openscarlett::xg {

struct XgPlaneUsageMetadataSummary {
    uint32_t plane_usage_flags = 0u;
    uint32_t data_plane_count = 0u;
    uint32_t htile_blocks_x = 0u;
    uint32_t htile_blocks_y = 0u;
    uint32_t htile_padded_width = 0u;
    uint32_t htile_padded_height = 0u;
    uint64_t htile_raw_bytes = 0ull;
    uint64_t htile_pipe_align_bytes = 0ull;
    uint64_t htile_offset_bytes = 0ull;
    uint64_t htile_size_bytes = 0ull;
    uint64_t htile_slice_pitch_bytes = 0ull;
    uint64_t total_size_bytes = 0ull;
};

bool ComputeTexture2DPlaneUsageSummary(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    XgPlaneUsageMetadataSummary& out_summary) noexcept;

}  // namespace openscarlett::xg

namespace openscarlett {

struct XgPlacedTiledSubresourceDesc {
    uint32_t subresource_index = 0u;
    uint32_t mip_level = 0u;
    uint32_t array_slice = 0u;
    uint32_t width_elements = 0u;
    uint32_t height_elements = 0u;
    uint32_t depth_elements = 1u;
    uint32_t tiles_x = 0u;
    uint32_t tiles_y = 0u;
    uint32_t tiles_z = 1u;
    uint32_t tile_size_bytes = 65536u;
    uint32_t bytes_per_element = 4u;
    uint32_t linear_row_pitch_bytes = 256u;
    uint64_t subresource_offset_bytes = 0ull;
    uint64_t heap_subresource_offset_bytes = 0ull;
    uint64_t subresource_size_bytes = 0ull;
    uint32_t tile_pool_start_index_64kb = 0u;
    uint32_t tile_pool_count_64kb = 0u;
};

struct XgPlacedTilePoolEntry {
    uint32_t logical_tile_index = 0u;
    uint32_t heap_tile_index_64kb = 0u;
    uint64_t pool_byte_offset = 0ull;
    bool mapped = true;
};

bool ComputePlacedTiledSubresourceLayout2D(
    const XG_TEXTURE2D_DESC& desc,
    uint64_t placed_heap_offset_bytes,
    uint32_t subresource_index,
    XgPlacedTiledSubresourceDesc& out_sub) noexcept;

bool GatherPlacedTiledPool64KB(
    const void* src_pool_base,
    uint64_t src_pool_size_bytes,
    uint64_t pool_base_offset_bytes,
    const std::vector<XgPlacedTilePoolEntry>& mappings,
    uint32_t tile_size_bytes,
    std::vector<uint8_t>& out_contiguous_tiled);

bool ScatterPlacedTiledPool64KB(
    const void* src_contiguous_tiled,
    uint64_t src_size_bytes,
    uint64_t pool_base_offset_bytes,
    const std::vector<XgPlacedTilePoolEntry>& mappings,
    uint32_t tile_size_bytes,
    void* dst_pool_base,
    uint64_t dst_pool_size_bytes);

}  // namespace openscarlett

namespace {

using namespace openscarlett;

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

struct SpvBuilder {
    std::vector<uint32_t> words;
    uint32_t bound = 1u;

    uint32_t Id() { return bound++; }

    void Op(uint16_t opcode, std::initializer_list<uint32_t> args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1u) << 16u) | opcode);
        for (uint32_t a : args) words.push_back(a);
    }

    void OpVec(uint16_t opcode, const std::vector<uint32_t>& args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1u) << 16u) | opcode);
        words.insert(words.end(), args.begin(), args.end());
    }

    static void PushStr(std::vector<uint32_t>& dst, const char* s) {
        const size_t n = std::strlen(s) + 1u;
        const size_t nw = (n + 3u) / 4u;
        for (size_t w = 0u; w < nw; ++w) {
            uint32_t v = 0u;
            for (size_t b = 0u; b < 4u; ++b) {
                const size_t idx = w * 4u + b;
                if (idx < n) {
                    v |= static_cast<uint32_t>(static_cast<uint8_t>(s[idx])) << (b * 8u);
                }
            }
            dst.push_back(v);
        }
    }

    std::vector<uint32_t> Finish(uint32_t spv_version = 0x00010300u) const {
        std::vector<uint32_t> out;
        out.reserve(5u + words.size());
        out.insert(out.end(), {0x07230203u, spv_version, 0x4F533200u, bound, 0u});
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

// Simple passthrough VS: takes location 0 pos (vec4) and location 1 uv (vec4).
std::vector<uint32_t> BuildBorderSampleVertexSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id(), t_pv = b.Id(), t_out_pv = b.Id();
    const uint32_t v_in_pos = b.Id(), v_in_uv = b.Id(), v_out_pv = b.Id(), v_out_uv = b.Id();
    const uint32_t c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {0, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_in_uv, v_out_pv, v_out_uv});
        b.OpVec(15, ep);
    }
    b.Op(71, {v_in_pos, 30, 0});
    b.Op(71, {v_in_uv, 30, 1});
    b.Op(71, {v_out_uv, 30, 0});
    b.Op(71, {t_pv, 2});
    b.Op(72, {t_pv, 0, 11, 0});

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_pv, t_v4});
    b.Op(32, {t_out_pv, 3, t_pv});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_in_v4, v_in_pos, 1});
    b.Op(59, {t_in_v4, v_in_uv, 1});
    b.Op(59, {t_out_pv, v_out_pv, 3});
    b.Op(59, {t_out_v4, v_out_uv, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t pos = b.Id(), uv = b.Id(), ptr_pos = b.Id();
    b.Op(61, {t_v4, pos, v_in_pos});
    b.Op(61, {t_v4, uv, v_in_uv});
    b.Op(65, {t_out_v4, ptr_pos, v_out_pv, c_i0});
    b.Op(62, {ptr_pos, pos});
    b.Op(62, {v_out_uv, uv});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// Simple PS: samples CombinedImageSampler at set 0 binding 0 using in location 0 uv.xy (Lod=0).
std::vector<uint32_t> BuildBorderSampleFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_v2 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_img = b.Id(), t_samp_img = b.Id(), t_uc_si = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id(), v_tex = b.Id();
    const uint32_t c_f0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {4, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in, v_out});
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 7});       // OriginUpperLeft
    b.Op(71, {v_in, 30, 0});      // Location 0
    b.Op(71, {v_out, 30, 0});     // Location 0
    b.Op(71, {v_tex, 34, 0});     // DescriptorSet 0
    b.Op(71, {v_tex, 33, 0});     // Binding 0

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(23, {t_v2, t_f32, 2});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(25, {t_img, t_f32, 1, 0, 0, 0, 1, 0});  // 2D sampled float image
    b.Op(27, {t_samp_img, t_img});
    b.Op(32, {t_uc_si, 0, t_samp_img});

    b.Op(43, {t_f32, c_f0, F32Bits(0.0f)});
    b.Op(59, {t_in_v4, v_in, 1});
    b.Op(59, {t_out_v4, v_out, 3});
    b.Op(59, {t_uc_si, v_tex, 0});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t in_v = b.Id(), u = b.Id(), v = b.Id(), uv = b.Id();
    const uint32_t si = b.Id(), texel = b.Id();
    b.Op(61, {t_v4, in_v, v_in});
    b.Op(81, {t_f32, u, in_v, 0});
    b.Op(81, {t_f32, v, in_v, 1});
    b.Op(80, {t_v2, uv, u, v});
    b.Op(61, {t_samp_img, si, v_tex});
    b.Op(88, {t_v4, texel, si, uv, 2, c_f0});  // OpImageSampleExplicitLod Lod=0
    b.Op(62, {v_out, texel});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// ---------------------------------------------------------------------------
// Test 1: Vulkan D3D12 OPTIONS2 & OPTIONS7 Capability Probing & Mapping
// ---------------------------------------------------------------------------
void TestVulkanOptions2And7() {
    std::printf("[Test 1] Vulkan live physical-device probe -> D3D12 OPTIONS2 & OPTIONS7...\n");

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());

    const VulkanDeviceInfo& dev = vk.GetDeviceInfo();
    const VulkanFeatureProbe& probe = vk.GetFeatureProbe();

    VulkanD3D12Options2 opts2{};
    VulkanD3D12Options7 opts7{};
    OS_REQUIRE(vk.QueryD3D12Options2(opts2).ok());
    OS_REQUIRE(vk.QueryD3D12Options7(opts7).ok());

    // Live consistency assertions (must hold on BOTH hardware ICD and software fallback).
    OS_REQUIRE(opts2.depth_bounds_test_supported == probe.depth_bounds);
    const auto expected_mesh =
        (probe.has_mesh_shader_ext && probe.mesh_shader && probe.task_shader)
            ? VulkanD3D12MeshShaderTier::Tier1
            : VulkanD3D12MeshShaderTier::NotSupported;
    OS_REQUIRE(opts7.mesh_shader_tier == expected_mesh);

    // Map onto native Windows SDK D3D12_FEATURE_DATA_D3D12_OPTIONS2 (and guarded OPTIONS7).
#if defined(_WIN32)
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 native_opts2{};
    FillNativeD3D12Options2(opts2, native_opts2);
    OS_REQUIRE((native_opts2.DepthBoundsTestSupported != FALSE) == opts2.depth_bounds_test_supported);
    OS_REQUIRE(static_cast<uint32_t>(native_opts2.ProgrammableSamplePositionsTier) ==
               static_cast<uint32_t>(opts2.programmable_sample_positions_tier));

#if defined(D3D12_FEATURE_D3D12_OPTIONS7) || defined(_MSC_VER)
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 native_opts7{};
    FillNativeD3D12Options7(opts7, native_opts7);
    OS_REQUIRE(static_cast<uint32_t>(native_opts7.MeshShaderTier) ==
               static_cast<uint32_t>(opts7.mesh_shader_tier));
    OS_REQUIRE(static_cast<uint32_t>(native_opts7.SamplerFeedbackTier) ==
               static_cast<uint32_t>(opts7.sampler_feedback_tier));
#endif
#endif

    // Portable clean-room OPTIONS7 target struct (always exercised on MSVC, Clang-CL, and MinGW-w64).
    struct PortableD3D12Options7 {
        uint32_t MeshShaderTier = 0u;
        uint32_t SamplerFeedbackTier = 0u;
    } port_opts7{};
    FillNativeD3D12Options7(opts7, port_opts7);
    OS_REQUIRE(port_opts7.MeshShaderTier == static_cast<uint32_t>(opts7.mesh_shader_tier));
    OS_REQUIRE(port_opts7.SamplerFeedbackTier == static_cast<uint32_t>(opts7.sampler_feedback_tier));

    if (!vk.is_fallback_mode()) {
        OS_REQUIRE(probe.probed_from_icd);
        OS_REQUIRE(probe.api_major >= 1u);
        std::printf(
            "  Live Vulkan ICD: \"%s\" API=%u.%u.%u depthBounds=%d samplePosTier=%u "
            "meshTier=%u samplerFbTier=%u customBorder=%d (withoutFmt=%d max=%u)\n",
            dev.device_name.c_str(), probe.api_major, probe.api_minor, probe.api_patch,
            opts2.depth_bounds_test_supported ? 1 : 0,
            static_cast<uint32_t>(opts2.programmable_sample_positions_tier),
            static_cast<uint32_t>(opts7.mesh_shader_tier),
            static_cast<uint32_t>(opts7.sampler_feedback_tier),
            probe.custom_border_colors ? 1 : 0,
            probe.custom_border_color_without_format ? 1 : 0,
            probe.max_custom_border_color_samplers);
        if (probe.api_major > 1u || probe.api_minor >= 2u) {
            OS_REQUIRE(probe.vulkan12_probed);
        }
        if (probe.api_major > 1u || probe.api_minor >= 3u) {
            OS_REQUIRE(probe.vulkan13_probed);
        }
    } else {
        std::printf("  [INFO] No hardware Vulkan ICD on runner; verified fallback query consistency.\n");
    }

    // Synthetic mapping edge cases (always tested regardless of host GPU).
    {
        VulkanFeatureProbe empty{};
        const auto o2 = VulkanBackend::MapVulkanToD3D12Options2(empty);
        const auto o7 = VulkanBackend::MapVulkanToD3D12Options7(empty);
        OS_REQUIRE(!o2.depth_bounds_test_supported);
        OS_REQUIRE(o2.programmable_sample_positions_tier ==
                   VulkanD3D12ProgrammableSamplePositionsTier::NotSupported);
        OS_REQUIRE(o7.mesh_shader_tier == VulkanD3D12MeshShaderTier::NotSupported);
        OS_REQUIRE(o7.sampler_feedback_tier == VulkanD3D12SamplerFeedbackTier::NotSupported);
    }
    {
        // Tier 1 programmable sample positions (1x1 grid, 4 subpixel bits, 1x+4x counts)
        VulkanFeatureProbe p{};
        p.depth_bounds = true;
        p.has_sample_locations_ext = true;
        p.sample_location_sample_counts = 0x5u;
        p.max_sample_location_grid_size_x = 1u;
        p.max_sample_location_grid_size_y = 1u;
        p.sample_location_subpixel_bits = 4u;
        p.variable_sample_locations = false;
        const auto o2 = VulkanBackend::MapVulkanToD3D12Options2(p);
        OS_REQUIRE(o2.depth_bounds_test_supported);
        OS_REQUIRE(o2.programmable_sample_positions_tier ==
                   VulkanD3D12ProgrammableSamplePositionsTier::Tier1);

        // Tier 2 requires >= 2x2 grid + variable sample locations
        p.max_sample_location_grid_size_x = 2u;
        p.max_sample_location_grid_size_y = 2u;
        p.variable_sample_locations = true;
        const auto o2_t2 = VulkanBackend::MapVulkanToD3D12Options2(p);
        OS_REQUIRE(o2_t2.programmable_sample_positions_tier ==
                   VulkanD3D12ProgrammableSamplePositionsTier::Tier2);

        // Subpixel precision < 4 bits disqualifies programmable sample positions
        p.sample_location_subpixel_bits = 2u;
        const auto o2_low = VulkanBackend::MapVulkanToD3D12Options2(p);
        OS_REQUIRE(o2_low.programmable_sample_positions_tier ==
                   VulkanD3D12ProgrammableSamplePositionsTier::NotSupported);
    }
    {
        // Mesh shader without task shader is NOT D3D12 Mesh Shader Tier 1
        VulkanFeatureProbe p{};
        p.has_mesh_shader_ext = true;
        p.mesh_shader = true;
        p.task_shader = false;
        OS_REQUIRE(VulkanBackend::MapVulkanToD3D12Options7(p).mesh_shader_tier ==
                   VulkanD3D12MeshShaderTier::NotSupported);

        // Mesh + Task shader => Tier 1 (10)
        p.task_shader = true;
        OS_REQUIRE(VulkanBackend::MapVulkanToD3D12Options7(p).mesh_shader_tier ==
                   VulkanD3D12MeshShaderTier::Tier1);

        // Sampler feedback Tier 0.9 vs Tier 1.0
        p.has_sampler_feedback_ext = true;
        p.sampler_feedback_min_mip = true;
        p.sampler_feedback_mip_region_used = false;
        OS_REQUIRE(VulkanBackend::MapVulkanToD3D12Options7(p).sampler_feedback_tier ==
                   VulkanD3D12SamplerFeedbackTier::Tier0_9);

        p.sampler_feedback_mip_region_used = true;
        OS_REQUIRE(VulkanBackend::MapVulkanToD3D12Options7(p).sampler_feedback_tier ==
                   VulkanD3D12SamplerFeedbackTier::Tier1_0);
    }

    std::printf("[Test 1] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: VK_EXT_custom_border_color Probe, Live Sampling & Standard Fallback
// ---------------------------------------------------------------------------
void TestVulkanCustomBorderColor() {
    std::printf("[Test 2] VK_EXT_custom_border_color probe, live sampling & fallback...\n");

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());

    const VulkanDeviceInfo& dev = vk.GetDeviceInfo();

    // 2a. Probe float custom border color sampler
    {
        VulkanCustomBorderSamplerDesc sdesc{};
        sdesc.border_color_mode = VulkanBorderColorMode::FloatCustom;
        sdesc.custom_border_color_float[0] = 0.20f;
        sdesc.custom_border_color_float[1] = 0.80f;
        sdesc.custom_border_color_float[2] = 0.40f;
        sdesc.custom_border_color_float[3] = 1.00f;
        sdesc.border_format = VulkanImageFormat::Rgba8Unorm;
        sdesc.specify_border_format = false;
        sdesc.force_standard_fallback = false;

        VulkanCustomBorderSamplerResult sres{};
        OS_REQUIRE(vk.ProbeCustomBorderColorSampler(sdesc, sres).ok());
        OS_REQUIRE(sres.created);
        if (!vk.is_fallback_mode() && dev.supports_custom_border_color) {
            OS_REQUIRE(sres.executed_on_vulkan_icd);
            OS_REQUIRE(sres.used_custom_border_color_ext);
            OS_REQUIRE(!sres.used_standard_border_fallback);
            OS_REQUIRE(sres.effective_border_color_mode == VulkanBorderColorMode::FloatCustom);
        }
    }

    // 2b. Probe integer custom border color sampler (with explicit format)
    {
        VulkanCustomBorderSamplerDesc sdesc{};
        sdesc.border_color_mode = VulkanBorderColorMode::IntCustom;
        sdesc.custom_border_color_int[0] = 42;
        sdesc.custom_border_color_int[1] = 84;
        sdesc.custom_border_color_int[2] = 126;
        sdesc.custom_border_color_int[3] = 255;
        sdesc.border_format = VulkanImageFormat::Rgba8Unorm;
        sdesc.specify_border_format = true;

        VulkanCustomBorderSamplerResult sres{};
        OS_REQUIRE(vk.ProbeCustomBorderColorSampler(sdesc, sres).ok());
        OS_REQUIRE(sres.created);
    }

    // 2c. Probe clean standard-border fallback mapping (bright opaque, dark opaque, transparent)
    {
        VulkanCustomBorderSamplerDesc sdesc{};
        sdesc.border_color_mode = VulkanBorderColorMode::FloatCustom;
        sdesc.custom_border_color_float[0] = 0.90f;
        sdesc.custom_border_color_float[1] = 0.85f;
        sdesc.custom_border_color_float[2] = 0.75f;
        sdesc.custom_border_color_float[3] = 1.00f;
        sdesc.force_standard_fallback = true;

        VulkanCustomBorderSamplerResult sres{};
        OS_REQUIRE(vk.ProbeCustomBorderColorSampler(sdesc, sres).ok());
        OS_REQUIRE(sres.created);
        OS_REQUIRE(!sres.used_custom_border_color_ext);
        OS_REQUIRE(sres.used_standard_border_fallback);
        OS_REQUIRE(sres.effective_border_color_mode == VulkanBorderColorMode::FloatOpaqueWhite);

        sdesc.custom_border_color_float[3] = 0.0f;
        OS_REQUIRE(vk.ProbeCustomBorderColorSampler(sdesc, sres).ok());
        OS_REQUIRE(sres.effective_border_color_mode == VulkanBorderColorMode::FloatTransparentBlack);

        sdesc.custom_border_color_float[0] = 0.1f;
        sdesc.custom_border_color_float[1] = 0.1f;
        sdesc.custom_border_color_float[2] = 0.1f;
        sdesc.custom_border_color_float[3] = 1.0f;
        OS_REQUIRE(vk.ProbeCustomBorderColorSampler(sdesc, sres).ok());
        OS_REQUIRE(sres.effective_border_color_mode == VulkanBorderColorMode::FloatOpaqueBlack);
    }

    // 2d. Live Vulkan GPU graphics draw sampling out-of-bounds UV (2.5, 2.5) with custom border color
    struct Vertex {
        float pos[4];
        float uv[4];
    };
    const Vertex verts[3] = {
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {2.5f, 2.5f, 0.0f, 1.0f}},
        {{ 3.0f, -1.0f, 0.0f, 1.0f}, {2.5f, 2.5f, 0.0f, 1.0f}},
        {{-1.0f,  3.0f, 0.0f, 1.0f}, {2.5f, 2.5f, 0.0f, 1.0f}},
    };

    VulkanTextureDesc red_tex{};
    red_tex.width = 2u;
    red_tex.height = 2u;
    red_tex.format = VulkanImageFormat::Rgba8Unorm;
    VulkanTextureMipLevel mip0{};
    mip0.width = 2u;
    mip0.height = 2u;
    mip0.depth = 1u;
    mip0.data = {
        255, 0, 0, 255,  255, 0, 0, 255,
        255, 0, 0, 255,  255, 0, 0, 255,
    };
    red_tex.mips.push_back(std::move(mip0));

    auto make_draw_desc = [&](bool force_fallback) -> VulkanGraphicsDrawDesc {
        VulkanGraphicsDrawDesc gd{};
        gd.vertex_spirv = BuildBorderSampleVertexSpirv();
        gd.fragment_spirv = BuildBorderSampleFragmentSpirv();
        gd.width = 32u;
        gd.height = 32u;
        gd.color_format = VulkanImageFormat::Rgba8Unorm;
        gd.clear_color[0] = 0.0f;
        gd.clear_color[1] = 0.0f;
        gd.clear_color[2] = 0.0f;
        gd.clear_color[3] = 1.0f;
        gd.vertex_bytes.resize(sizeof(verts));
        std::memcpy(gd.vertex_bytes.data(), verts, sizeof(verts));
        gd.vertex_stride = sizeof(Vertex);
        gd.vertex_attributes = {
            {0u, 0u, VulkanVertexFormat::R32G32B32A32Sfloat, 0u},
            {1u, 0u, VulkanVertexFormat::R32G32B32A32Sfloat, 16u},
        };
        gd.vertex_count = 3u;

        VulkanDescriptorBinding db{};
        db.binding = 0u;
        db.kind = VulkanDescriptorKind::CombinedImageSampler;
        db.texture = red_tex;
        db.linear_filter = false;
        db.address_mode_u = VulkanSamplerAddressMode::ClampToBorder;
        db.address_mode_v = VulkanSamplerAddressMode::ClampToBorder;
        db.address_mode_w = VulkanSamplerAddressMode::ClampToBorder;
        db.border_color_mode = VulkanBorderColorMode::FloatCustom;
        db.custom_border_color_float[0] = 0.20f;  // ~51
        db.custom_border_color_float[1] = 0.80f;  // ~204
        db.custom_border_color_float[2] = 0.40f;  // ~102
        db.custom_border_color_float[3] = 1.00f;  // 255
        db.force_standard_border_fallback = force_fallback;
        gd.descriptors.push_back(std::move(db));
        return gd;
    };

    VulkanGraphicsDrawResult custom_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(make_draw_desc(false), custom_res).ok());
    const size_t mid_px = (16u * 32u + 16u) * 4u;
    OS_REQUIRE(mid_px + 4u <= custom_res.framebuffer.pixels.size());
    const uint8_t cr = custom_res.framebuffer.pixels[mid_px + 0u];
    const uint8_t cg = custom_res.framebuffer.pixels[mid_px + 1u];
    const uint8_t cb = custom_res.framebuffer.pixels[mid_px + 2u];
    const uint8_t ca = custom_res.framebuffer.pixels[mid_px + 3u];
    std::printf("  Live custom border sample at UV=(2.5,2.5): RGBA=(%u,%u,%u,%u) icd=%d ext_samplers=%u fb_samplers=%u\n",
                cr, cg, cb, ca,
                custom_res.executed_on_vulkan_icd ? 1 : 0,
                custom_res.custom_border_samplers_used,
                custom_res.custom_border_fallback_samplers_used);

    if (!vk.is_fallback_mode() && dev.supports_custom_border_color) {
        OS_REQUIRE(custom_res.executed_on_vulkan_icd);
        OS_REQUIRE(custom_res.custom_border_samplers_used == 1u);
        OS_REQUIRE(custom_res.custom_border_fallback_samplers_used == 0u);
        OS_REQUIRE(std::abs(static_cast<int>(cr) - 51) <= 2);
        OS_REQUIRE(std::abs(static_cast<int>(cg) - 204) <= 2);
        OS_REQUIRE(std::abs(static_cast<int>(cb) - 102) <= 2);
        OS_REQUIRE(ca >= 253u);
    }

    // Standard border color fallback draw (dark opaque black because avg(0.2, 0.8, 0.4) = 0.467 < 0.5).
    VulkanGraphicsDrawResult fb_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(make_draw_desc(true), fb_res).ok());
    const uint8_t fr = fb_res.framebuffer.pixels[mid_px + 0u];
    const uint8_t fg = fb_res.framebuffer.pixels[mid_px + 1u];
    const uint8_t fb = fb_res.framebuffer.pixels[mid_px + 2u];
    const uint8_t fa = fb_res.framebuffer.pixels[mid_px + 3u];
    std::printf("  Forced fallback border sample at UV=(2.5,2.5): RGBA=(%u,%u,%u,%u) ext_samplers=%u fb_samplers=%u\n",
                fr, fg, fb, fa,
                fb_res.custom_border_samplers_used,
                fb_res.custom_border_fallback_samplers_used);
    OS_REQUIRE(fb_res.custom_border_samplers_used == 0u);
    OS_REQUIRE(fb_res.custom_border_fallback_samplers_used == 1u);
    OS_REQUIRE(fr <= 2u && fg <= 2u && fb <= 2u && fa >= 253u);

    // Software fallback backend parity test (always runs even on HW GPU hosts).
    VulkanBackend vk_sw;
    OS_REQUIRE(vk_sw.InitializeSoftwareFallback().ok());
    VulkanGraphicsDrawResult sw_res{};
    OS_REQUIRE(vk_sw.ExecuteGraphicsDraw(make_draw_desc(false), sw_res).ok());
    OS_REQUIRE(!sw_res.executed_on_vulkan_icd);
    OS_REQUIRE(std::abs(static_cast<int>(sw_res.framebuffer.pixels[mid_px + 0u]) - 51) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(sw_res.framebuffer.pixels[mid_px + 1u]) - 204) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(sw_res.framebuffer.pixels[mid_px + 2u]) - 102) <= 2);
    OS_REQUIRE(sw_res.framebuffer.pixels[mid_px + 3u] >= 253u);

    std::printf("[Test 2] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: Tiled Placed Textures on Real Host GPU AND D3D12 WARP
// ---------------------------------------------------------------------------
#if defined(_WIN32)
void WaitForQueueFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t& fence_val) {
    ++fence_val;
    OS_REQUIRE(SUCCEEDED(queue->Signal(fence, fence_val)));
    if (fence->GetCompletedValue() < fence_val) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);
        OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(fence_val, ev)));
        OS_REQUIRE(WaitForSingleObject(ev, 10000u) == WAIT_OBJECT_0);
        CloseHandle(ev);
    }
}

void RunPlacedTiledTextureTest(bool use_warp) {
    const char* label = use_warp ? "D3D12 WARP" : "D3D12 Hardware GPU";
    std::printf("[Test 3 - %s] Placed tiled texture, 64KB tile-pool mapping, untile/retile & barriers...\n", label);

    IDXGIAdapter1* chosen_adapter = nullptr;
    if (use_warp) {
        IDXGIFactory4* factory = nullptr;
        OS_REQUIRE(SUCCEEDED(CreateDXGIFactory1(OS_D3D12_IID(IDXGIFactory4), reinterpret_cast<void**>(&factory))));
        OS_REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(OS_D3D12_IID(IDXGIAdapter1), reinterpret_cast<void**>(&chosen_adapter))));
        factory->Release();
    }

    ID3D12DeviceX* dev = nullptr;
    HRESULT hr = D3D12CreateDeviceX(
        chosen_adapter, D3D_FEATURE_LEVEL_12_0,
        IID_ID3D12DeviceX, reinterpret_cast<void**>(&dev));
    if (chosen_adapter) chosen_adapter->Release();
    if (FAILED(hr) || !dev) {
        if (!use_warp) {
            std::printf("  [INFO] No D3D12 hardware adapter on runner (hr=0x%08lX); skipping HW pass.\n",
                        static_cast<unsigned long>(hr));
            return;
        }
        OS_REQUIRE(SUCCEEDED(hr) && dev != nullptr);
    }

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qdesc, OS_D3D12_IID(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))));

    ID3D12CommandAllocator* alloc = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, OS_D3D12_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))));

    ID3D12GraphicsCommandList* cmd = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
        OS_D3D12_IID(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd))));

    openscarlett::d3d12_xs::IOpenScarlettTiledPlacedManager* tiled_mgr = nullptr;
    OS_REQUIRE(SUCCEEDED(cmd->QueryInterface(
        openscarlett::d3d12_xs::IID_IOpenScarlettTiledPlacedManager,
        reinterpret_cast<void**>(&tiled_mgr))));

    IOpenScarlettAaaRuntime* aaa_rt = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->QueryInterface(
        IID_IOpenScarlettAaaRuntime, reinterpret_cast<void**>(&aaa_rt))));

    ID3D12Fence* fence = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), reinterpret_cast<void**>(&fence))));
    uint64_t fence_val = 0ull;

    // Create a 2 MiB DEFAULT heap and place a 256x256 RGBA8 texture at a NON-ZERO 64KB heap offset (196608 = 3*64KB).
    constexpr uint32_t kW = 256u;
    constexpr uint32_t kH = 256u;
    constexpr uint64_t kPlacedHeapOffset = 3ull * 65536ull;  // 196,608 bytes

    D3D12_HEAP_DESC hdesc{};
    hdesc.SizeInBytes = 2ull * 1024ull * 1024ull;
    hdesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hdesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hdesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

    ID3D12Heap* pool_heap = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->CreateHeap(&hdesc, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&pool_heap))));

    D3D12_RESOURCE_DESC tdesc{};
    tdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tdesc.Width = kW;
    tdesc.Height = kH;
    tdesc.DepthOrArraySize = 1u;
    tdesc.MipLevels = 1u;
    tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tdesc.SampleDesc.Count = 1u;
    tdesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* placed_tex = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResource(
        pool_heap, kPlacedHeapOffset, &tdesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&placed_tex))));

    // Secondary placed texture on the same heap (offset 1 MiB) for CopyResource & aliasing barrier check.
    ID3D12Resource* placed_tex2 = nullptr;
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResource(
        pool_heap, 1024ull * 1024ull, &tdesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&placed_tex2))));

    openscarlett::d3d12_xs::D3D12XBOX_TILED_PLACED_DESC tp_desc{};
    tp_desc.placed_heap = pool_heap;
    tp_desc.heap_offset_bytes = kPlacedHeapOffset;
    tp_desc.xg_tile_mode = static_cast<uint32_t>(XG_TILE_MODE_2D_THIN_64KB_S);
    tp_desc.initial_state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST);
    tp_desc.tile_pool_base_offset_bytes = kPlacedHeapOffset;
    OS_REQUIRE(SUCCEEDED(tiled_mgr->RegisterPlacedTiledTexture(placed_tex, &tp_desc)));

    // Verify subresource pitch & 64KB tile-pool metadata.
    openscarlett::d3d12_xs::D3D12XBOX_TILED_PLACED_SUBRESOURCE_PITCH sp{};
    OS_REQUIRE(SUCCEEDED(tiled_mgr->QueryPlacedTiledSubresourcePitch(placed_tex, 0u, &sp)));
    OS_REQUIRE(sp.width_elements == kW);
    OS_REQUIRE(sp.height_elements == kH);
    OS_REQUIRE(sp.tiles_x == 2u);
    OS_REQUIRE(sp.tiles_y == 2u);
    OS_REQUIRE(sp.tile_size_bytes == 65536u);
    OS_REQUIRE(sp.linear_row_pitch_bytes == kW * 4u);  // 1024 (256B aligned)
    OS_REQUIRE(sp.heap_subresource_offset_bytes == kPlacedHeapOffset);
    OS_REQUIRE(sp.tile_pool_start_index_64kb == 3u);
    OS_REQUIRE(sp.tile_pool_count_64kb == 4u);

    // Prepare ground-truth 256x256 RGBA8 image and Xbox 64KB_S tiled representation.
    const uint32_t row_pitch = sp.linear_row_pitch_bytes;
    std::vector<uint8_t> truth_rgba(static_cast<size_t>(row_pitch) * kH, 0u);
    for (uint32_t y = 0u; y < kH; ++y) {
        for (uint32_t x = 0u; x < kW; ++x) {
            const size_t off = static_cast<size_t>(y) * row_pitch + x * 4u;
            truth_rgba[off + 0u] = static_cast<uint8_t>((x * 3u + y) & 0xFFu);
            truth_rgba[off + 1u] = static_cast<uint8_t>((y * 5u + x * 2u) & 0xFFu);
            truth_rgba[off + 2u] = static_cast<uint8_t>(((x ^ y) * 7u + 19u) & 0xFFu);
            truth_rgba[off + 3u] = 0xFFu;
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
    xg_desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S;

    XG_RESOURCE_LAYOUT xg_layout{};
    OS_REQUIRE(xg::ComputeTexture2DLayout(xg_desc, xg_layout));
    OS_REQUIRE(xg_layout.SizeBytes == 4ull * 65536ull);

    std::vector<uint8_t> contiguous_tiled(static_cast<size_t>(xg_layout.SizeBytes), 0u);
    OS_REQUIRE(xg::TileSurface2D(
        xg_desc, 0u, 0u, truth_rgba.data(), row_pitch,
        contiguous_tiled.data(), contiguous_tiled.size()));

    // Bind a non-trivial permuted 64KB tile-pool mapping table:
    // logical (0,0)->pool tile 2, (1,0)->pool tile 0, (0,1)->pool tile 3, (1,1)->pool tile 1.
    const uint32_t perm[4] = {2u, 0u, 3u, 1u};
    std::vector<openscarlett::d3d12_xs::D3D12XBOX_TILED_PLACED_TILE_MAPPING> maps(4u);
    std::vector<XgPlacedTilePoolEntry> pool_entries(4u);
    for (uint32_t ty = 0u; ty < 2u; ++ty) {
        for (uint32_t tx = 0u; tx < 2u; ++tx) {
            const uint32_t li = ty * 2u + tx;
            maps[li].subresource_index = 0u;
            maps[li].logical_tile_x = tx;
            maps[li].logical_tile_y = ty;
            maps[li].logical_tile_z = 0u;
            maps[li].heap_tile_index_64kb = perm[li];
            maps[li].pool_byte_offset = kPlacedHeapOffset + static_cast<uint64_t>(perm[li]) * 65536ull;
            maps[li].mapped = 1u;

            pool_entries[li].logical_tile_index = li;
            pool_entries[li].heap_tile_index_64kb = perm[li];
            pool_entries[li].pool_byte_offset = 0ull;
            pool_entries[li].mapped = true;
        }
    }
    OS_REQUIRE(SUCCEEDED(tiled_mgr->UpdatePlacedTileMappings(placed_tex, 4u, maps.data())));

    openscarlett::d3d12_xs::D3D12XBOX_TILED_PLACED_TILE_MAPPING qm{};
    OS_REQUIRE(SUCCEEDED(tiled_mgr->QueryPlacedTileMapping(placed_tex, 0u, 0u, 0u, 0u, &qm)));
    OS_REQUIRE(qm.heap_tile_index_64kb == 2u && qm.mapped == 1u);

    // Scatter tiled payload into an UPLOAD buffer starting at a 64KB buffer offset.
    constexpr uint64_t kUploadPoolOffset = 65536ull;
    const uint64_t upload_buf_size = kUploadPoolOffset + xg_layout.SizeBytes;
    auto make_host_buf = [&](D3D12_HEAP_TYPE htype, uint64_t sz, D3D12_RESOURCE_STATES st) -> ID3D12Resource* {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = htype;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sz;
        bd.Height = 1u;
        bd.DepthOrArraySize = 1u;
        bd.MipLevels = 1u;
        bd.SampleDesc.Count = 1u;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* b = nullptr;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&b))));
        return b;
    };

    ID3D12Resource* upload_pool_buf = make_host_buf(
        D3D12_HEAP_TYPE_UPLOAD, upload_buf_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        void* p = nullptr;
        D3D12_RANGE rn{0u, 0u};
        OS_REQUIRE(SUCCEEDED(upload_pool_buf->Map(0u, &rn, &p)) && p != nullptr);
        std::memset(p, 0, static_cast<size_t>(upload_buf_size));
        OS_REQUIRE(ScatterPlacedTiledPool64KB(
            contiguous_tiled.data(), contiguous_tiled.size(),
            kUploadPoolOffset, pool_entries, 65536u, p, upload_buf_size));
        D3D12_RANGE wf{0u, static_cast<SIZE_T>(upload_buf_size)};
        upload_pool_buf->Unmap(0u, &wf);
    }

    ID3D12Resource* linear_rb = make_host_buf(
        D3D12_HEAP_TYPE_READBACK, truth_rgba.size(), D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tiled_rb = make_host_buf(
        D3D12_HEAP_TYPE_READBACK, xg_layout.SizeBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    OS_REQUIRE(SUCCEEDED(aaa_rt->SetResourceTileMode(tiled_rb, static_cast<uint32_t>(XG_TILE_MODE_2D_THIN_64KB_S))));

    // Record CopyTextureRegion (permuted 64KB tile-pool gather + GPU untile) -> placed_tex
    D3D12_TEXTURE_COPY_LOCATION dst_tex_loc{};
    dst_tex_loc.pResource = placed_tex;
    dst_tex_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_tex_loc.SubresourceIndex = 0u;

    D3D12_TEXTURE_COPY_LOCATION src_pool_loc{};
    src_pool_loc.pResource = upload_pool_buf;
    src_pool_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_pool_loc.PlacedFootprint.Offset = kUploadPoolOffset;
    src_pool_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_pool_loc.PlacedFootprint.Footprint.Width = kW;
    src_pool_loc.PlacedFootprint.Footprint.Height = kH;
    src_pool_loc.PlacedFootprint.Footprint.Depth = 1u;
    src_pool_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;

    cmd->CopyTextureRegion(&dst_tex_loc, 0u, 0u, 0u, &src_pool_loc, nullptr);

    // Transition barrier COPY_DEST -> COPY_SOURCE + aliasing barrier on placed heap
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = placed_tex;
    barriers[0].Transition.Subresource = 0u;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barriers[1].Aliasing.pResourceBefore = nullptr;
    barriers[1].Aliasing.pResourceAfter = placed_tex;
    cmd->ResourceBarrier(2u, barriers);

    // Linear readback of untiled placed_tex
    D3D12_TEXTURE_COPY_LOCATION lin_rb_loc{};
    lin_rb_loc.pResource = linear_rb;
    lin_rb_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    lin_rb_loc.PlacedFootprint.Offset = 0ull;
    lin_rb_loc.PlacedFootprint.Footprint = src_pool_loc.PlacedFootprint.Footprint;
    cmd->CopyTextureRegion(&lin_rb_loc, 0u, 0u, 0u, &dst_tex_loc, nullptr);

    // Retile round-trip: placed_tex -> tiled_rb (scatters back into permuted 64KB tiles!)
    D3D12_TEXTURE_COPY_LOCATION tiled_rb_loc = lin_rb_loc;
    tiled_rb_loc.pResource = tiled_rb;
    cmd->CopyTextureRegion(&tiled_rb_loc, 0u, 0u, 0u, &dst_tex_loc, nullptr);

    // Also exercise CopyResource between placed_tex -> placed_tex2
    cmd->CopyResource(placed_tex2, placed_tex);

    // Also exercise CopyTiles upload on placed_tex (transition back to COPY_DEST first)
    D3D12_RESOURCE_BARRIER back_to_dst{};
    back_to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    back_to_dst.Transition.pResource = placed_tex;
    back_to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    back_to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back_to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd->ResourceBarrier(1u, &back_to_dst);

    D3D12_TILED_RESOURCE_COORDINATE tile_start{0u, 0u, 0u, 0u};
    D3D12_TILE_REGION_SIZE tile_reg{};
    tile_reg.NumTiles = 4u;
    tile_reg.UseBox = TRUE;
    tile_reg.Width = 2u;
    tile_reg.Height = 2u;
    tile_reg.Depth = 1u;
    cmd->CopyTiles(placed_tex, &tile_start, &tile_reg, upload_pool_buf, kUploadPoolOffset, D3D12_TILE_COPY_FLAG_NONE);

    OS_REQUIRE(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* lists[1] = {cmd};
    queue->ExecuteCommandLists(1u, lists);
    WaitForQueueFence(queue, fence, fence_val);

    // Verify 100% pixel-exact untiled output from placed_tex!
    {
        void* p = nullptr;
        D3D12_RANGE rf{0u, truth_rgba.size()};
        OS_REQUIRE(SUCCEEDED(linear_rb->Map(0u, &rf, &p)) && p != nullptr);
        OS_REQUIRE(std::memcmp(p, truth_rgba.data(), truth_rgba.size()) == 0);
        D3D12_RANGE wn{0u, 0u};
        linear_rb->Unmap(0u, &wn);
    }

    // Verify 100% bit-exact retile round-trip through permuted 64KB tile-pool!
    {
        void* p = nullptr;
        D3D12_RANGE rf{0u, static_cast<SIZE_T>(xg_layout.SizeBytes)};
        OS_REQUIRE(SUCCEEDED(tiled_rb->Map(0u, &rf, &p)) && p != nullptr);
        std::vector<uint8_t> gathered_back;
        OS_REQUIRE(GatherPlacedTiledPool64KB(p, xg_layout.SizeBytes, 0ull, pool_entries, 65536u, gathered_back));
        std::vector<uint8_t> untiled_back(truth_rgba.size(), 0u);
        OS_REQUIRE(xg::UntileSurface2D(
            xg_desc, 0u, 0u, gathered_back.data(), gathered_back.size(), untiled_back.data(), row_pitch));
        OS_REQUIRE(std::memcmp(untiled_back.data(), truth_rgba.data(), truth_rgba.size()) == 0);
        D3D12_RANGE wn{0u, 0u};
        tiled_rb->Unmap(0u, &wn);
    }

    // Verify tracked placed-tiled info & preserved non-zero heap offset!
    openscarlett::d3d12_xs::D3D12XBOX_TILED_PLACED_INFO tinfo{};
    OS_REQUIRE(SUCCEEDED(tiled_mgr->QueryPlacedTiledTextureInfo(placed_tex, &tinfo)));
    std::printf(
        "  [%s] heap_off=%llu untiles=%llu retiles=%llu copy_res=%llu copy_tiles=%llu "
        "barriers=%llu aliasing=%llu flushes=%llu gpu_bridge=%u\n",
        label,
        static_cast<unsigned long long>(tinfo.heap_offset_bytes),
        static_cast<unsigned long long>(tinfo.untile_copy_count),
        static_cast<unsigned long long>(tinfo.retile_copy_count),
        static_cast<unsigned long long>(tinfo.copy_resource_bridge_count),
        static_cast<unsigned long long>(tinfo.copy_tiles_bridge_count),
        static_cast<unsigned long long>(tinfo.barrier_transition_count),
        static_cast<unsigned long long>(tinfo.aliasing_barrier_count),
        static_cast<unsigned long long>(tinfo.coherency_flush_count),
        tinfo.used_gpu_untile_bridge);
    OS_REQUIRE(tinfo.heap_offset_bytes == kPlacedHeapOffset);
    OS_REQUIRE(tinfo.tile_pool_base_offset_bytes == kPlacedHeapOffset);
    OS_REQUIRE(tinfo.untile_copy_count >= 2ull);  // CopyTextureRegion + CopyTiles
    OS_REQUIRE(tinfo.retile_copy_count >= 1ull);
    OS_REQUIRE(tinfo.copy_resource_bridge_count >= 1ull);
    OS_REQUIRE(tinfo.copy_tiles_bridge_count >= 1ull);
    OS_REQUIRE(tinfo.barrier_transition_count >= 2ull);
    OS_REQUIRE(tinfo.aliasing_barrier_count >= 1ull);
    OS_REQUIRE(tinfo.coherency_flush_count >= 1ull);
    OS_REQUIRE(tinfo.current_state == static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST));

    OS_REQUIRE(SUCCEEDED(tiled_mgr->UnregisterPlacedTiledTexture(placed_tex)));

    tiled_rb->Release();
    linear_rb->Release();
    upload_pool_buf->Release();
    placed_tex2->Release();
    placed_tex->Release();
    pool_heap->Release();
    fence->Release();
    aaa_rt->Release();
    tiled_mgr->Release();
    cmd->Release();
    alloc->Release();
    queue->Release();
    dev->Release();
    std::printf("[Test 3 - %s] PASS\n", label);
}
#endif

// ---------------------------------------------------------------------------
// Test 4: XG Stencil-Only DXGI Formats (22, 47, 120) Full Depth-Stencil Planar Layout
// ---------------------------------------------------------------------------
void TestXgStencilOnlyPlanarLayouts() {
    std::printf("[Test 4] XG stencil-only DXGI formats (22, 47, 120) -> full 2-plane depth-stencil parity...\n");

    struct FormatPair {
        uint32_t stencil_fmt;
        uint32_t parent_fmt;
        uint32_t expected_p0_bpe;
        const char* name;
    };
    const FormatPair pairs[] = {
        {22u, 20u, 4u, "X32_TYPELESS_G8X24_UINT vs D32_FLOAT_S8X24_UINT"},
        {47u, 45u, 4u, "X24_TYPELESS_G8_UINT vs D24_UNORM_S8_UINT"},
        {120u, 118u, 2u, "Xbox X16_TYPELESS_G8_UINT vs Xbox D16_UNORM_S8_UINT"},
    };
    const XG_TILE_MODE modes[] = {
        XG_TILE_MODE_2D_THIN_64KB_Z_X,
        XG_TILE_MODE_2D_THIN_4KB_Z,
        XG_TILE_MODE_LINEAR,
    };
    const std::pair<uint32_t, uint32_t> dims[] = {
        {256u, 256u},
        {1920u, 1080u},
        {128u, 64u},
    };

    for (const auto& fp : pairs) {
        for (const auto& m : modes) {
            for (const auto& [w, h] : dims) {
                XG_TEXTURE2D_DESC sdesc{};
                sdesc.Width = w;
                sdesc.Height = h;
                sdesc.MipLevels = 1u;
                sdesc.ArraySize = 1u;
                sdesc.Format = fp.stencil_fmt;
                sdesc.SampleCount = 1u;
                sdesc.BindFlags = XG_BIND_DEPTH_STENCIL;
                sdesc.TileMode = m;

                XG_TEXTURE2D_DESC pdesc = sdesc;
                pdesc.Format = fp.parent_fmt;

                XG_RESOURCE_LAYOUT slay{}, play{};
                OS_REQUIRE(xg::ComputeTexture2DLayout(sdesc, slay));
                OS_REQUIRE(xg::ComputeTexture2DLayout(pdesc, play));

                OS_REQUIRE(slay.PlaneCount == 2u);
                OS_REQUIRE(play.PlaneCount == 2u);
                OS_REQUIRE(slay.FormatInfo.bytes_per_block == fp.expected_p0_bpe);
                OS_REQUIRE(slay.PlaneFormatInfo[0].bytes_per_block == fp.expected_p0_bpe);
                OS_REQUIRE(slay.PlaneFormatInfo[1].bytes_per_block == 1u);
                OS_REQUIRE(slay.PlaneOffsets[0] == play.PlaneOffsets[0]);
                OS_REQUIRE(slay.PlaneOffsets[1] == play.PlaneOffsets[1]);
                OS_REQUIRE(slay.PlaneSizes[0] == play.PlaneSizes[0]);
                OS_REQUIRE(slay.PlaneSizes[1] == play.PlaneSizes[1]);
                OS_REQUIRE(slay.SizeBytes == play.SizeBytes);
                OS_REQUIRE(slay.BaseAlignmentBytes == play.BaseAlignmentBytes);
                OS_REQUIRE(slay.Mips[0].RowPitchBytes == play.Mips[0].RowPitchBytes);
                OS_REQUIRE(slay.Plane1Mips[0].OffsetBytes == play.Plane1Mips[0].OffsetBytes);
                OS_REQUIRE(slay.Plane1Mips[0].RowPitchBytes == play.Plane1Mips[0].RowPitchBytes);
                OS_REQUIRE(slay.Plane1Mips[0].SlicePitchBytes == play.Plane1Mips[0].SlicePitchBytes);

                // Also verify default BindFlags == XG_BIND_NONE promotes 22, 47, 120 to full 2-plane layout!
                XG_TEXTURE2D_DESC sdesc_none = sdesc;
                sdesc_none.BindFlags = XG_BIND_NONE;
                XG_RESOURCE_LAYOUT slay_none{};
                OS_REQUIRE(xg::ComputeTexture2DLayout(sdesc_none, slay_none));
                OS_REQUIRE(slay_none.PlaneCount == 2u);
                OS_REQUIRE(slay_none.SizeBytes == play.SizeBytes);
                OS_REQUIRE(slay_none.PlaneOffsets[1] == play.PlaneOffsets[1]);
            }
        }

        // Bit-exact Plane 0 (depth) + Plane 1 (stencil) tile/untile parity vs parent format!
        XG_TEXTURE2D_DESC sdesc{};
        sdesc.Width = 128u;
        sdesc.Height = 64u;
        sdesc.MipLevels = 1u;
        sdesc.ArraySize = 1u;
        sdesc.Format = fp.stencil_fmt;
        sdesc.SampleCount = 1u;
        sdesc.BindFlags = XG_BIND_DEPTH_STENCIL;
        sdesc.TileMode = XG_TILE_MODE_2D_THIN_64KB_Z_X;
        XG_TEXTURE2D_DESC pdesc = sdesc;
        pdesc.Format = fp.parent_fmt;

        XG_RESOURCE_LAYOUT slay{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(sdesc, slay));
        std::vector<uint8_t> p0_lin(128u * 64u * fp.expected_p0_bpe);
        std::vector<uint8_t> p1_lin(128u * 64u);
        for (size_t i = 0u; i < p0_lin.size(); ++i) p0_lin[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xFFu);
        for (size_t i = 0u; i < p1_lin.size(); ++i) p1_lin[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);

        std::vector<uint8_t> tiled_s(static_cast<size_t>(slay.SizeBytes), 0u);
        std::vector<uint8_t> tiled_p(static_cast<size_t>(slay.SizeBytes), 0u);
        OS_REQUIRE(xg::TileSurface2DPlane(sdesc, 0u, 0u, 0u, p0_lin.data(), 128u * fp.expected_p0_bpe, tiled_s.data(), tiled_s.size()));
        OS_REQUIRE(xg::TileSurface2DPlane(sdesc, 1u, 0u, 0u, p1_lin.data(), 128u, tiled_s.data(), tiled_s.size()));
        OS_REQUIRE(xg::TileSurface2DPlane(pdesc, 0u, 0u, 0u, p0_lin.data(), 128u * fp.expected_p0_bpe, tiled_p.data(), tiled_p.size()));
        OS_REQUIRE(xg::TileSurface2DPlane(pdesc, 1u, 0u, 0u, p1_lin.data(), 128u, tiled_p.data(), tiled_p.size()));
        OS_REQUIRE(tiled_s == tiled_p);

        std::vector<uint8_t> p0_out(p0_lin.size(), 0u);
        std::vector<uint8_t> p1_out(p1_lin.size(), 0u);
        OS_REQUIRE(xg::UntileSurface2DPlane(sdesc, 0u, 0u, 0u, tiled_s.data(), tiled_s.size(), p0_out.data(), 128u * fp.expected_p0_bpe));
        OS_REQUIRE(xg::UntileSurface2DPlane(sdesc, 1u, 0u, 0u, tiled_s.data(), tiled_s.size(), p1_out.data(), 128u));
        OS_REQUIRE(p0_out == p0_lin);
        OS_REQUIRE(p1_out == p1_lin);

        std::printf("  Verified %s: PlaneCount=%u P0=%uB P1=1B P1Off=%llu Total=%llu\n",
                    fp.name, slay.PlaneCount, slay.FormatInfo.bytes_per_block,
                    static_cast<unsigned long long>(slay.PlaneOffsets[1]),
                    static_cast<unsigned long long>(slay.SizeBytes));
    }

    // Verify backwards-compatibility for format 22 explicit SRV-only query (test_xg_dstorage contract).
    {
        XG_TEXTURE2D_DESC srv22{};
        srv22.Width = 64u;
        srv22.Height = 64u;
        srv22.MipLevels = 1u;
        srv22.ArraySize = 1u;
        srv22.Format = 22u;
        srv22.BindFlags = XG_BIND_SHADER_RESOURCE;
        srv22.TileMode = XG_TILE_MODE_2D_THIN_4KB_Z;
        XG_RESOURCE_LAYOUT lay{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(srv22, lay));
        OS_REQUIRE(lay.PlaneCount == 1u);
        OS_REQUIRE(lay.FormatInfo.bytes_per_block == 1u);
    }

    std::printf("[Test 4] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: Single-Mip Depth-Bind Texture HTILE Plane Layout & Plane Usage Flags
// ---------------------------------------------------------------------------
void TestXgSingleMipDepthHtileLayout() {
    std::printf("[Test 5] Single-mip depth-bind texture HTILE plane layout & plane usage flags...\n");

    struct Case {
        uint32_t w;
        uint32_t h;
        uint32_t format;
        uint32_t bind_flags;
        XG_TILE_MODE tile_mode;
        XG_CONSOLE_TYPE console;
        uint32_t expected_blocks_x;
        uint32_t expected_blocks_y;
        uint64_t expected_htile_size;
        uint32_t expected_usage_bits;
        const char* label;
    };

    const Case cases[] = {
        // 1920x1080 D32F 64KB_Z_X Anaconda: padded 1920x1152 -> 240x144 blocks -> 138,240B -> aligned 196,608B (3*64KB)
        {1920u, 1080u, static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT), XG_BIND_DEPTH_STENCIL,
         XG_TILE_MODE_2D_THIN_64KB_Z_X, XG_CONSOLE_TYPE_SCARLETT_ANACONDA,
         240u, 144u, 196608ull, (0x02u | 0x08u | 0x40u), "1920x1080 D32F 64KB_Z_X Anaconda"},

        // 1920x1080 D32F with D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL (0x2)
        {1920u, 1080u, static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS), 0x2u,
         XG_TILE_MODE_2D_THIN_64KB_Z_X, XG_CONSOLE_TYPE_SCARLETT_ANACONDA,
         240u, 144u, 196608ull, (0x02u | 0x08u | 0x40u), "1920x1080 R32_TYPELESS ALLOW_DS Anaconda"},

        // 1920x1080 D32S8 (stencil-only view 22) -> Depth + Stencil + HTILE + SingleMipHtile
        {1920u, 1080u, 22u, XG_BIND_DEPTH_STENCIL,
         XG_TILE_MODE_2D_THIN_64KB_Z_X, XG_CONSOLE_TYPE_SCARLETT_ANACONDA,
         240u, 144u, 196608ull, (0x02u | 0x04u | 0x08u | 0x40u), "1920x1080 X32_G8X24 64KB_Z_X Anaconda"},

        // 1280x720 D32F 64KB_Z_X Anaconda: padded 1280x768 -> 160x96 blocks -> 61,440B -> aligned 65,536B
        {1280u, 720u, static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT), XG_BIND_DEPTH_STENCIL,
         XG_TILE_MODE_2D_THIN_64KB_Z_X, XG_CONSOLE_TYPE_SCARLETT_ANACONDA,
         160u, 96u, 65536ull, (0x02u | 0x08u | 0x40u), "1280x720 D32F 64KB_Z_X Anaconda"},

        // 1280x720 D24S8 (47) Lockhart: padded 1280x768 -> 160x96 -> 61,440B -> 65,536B
        {1280u, 720u, 47u, XG_BIND_DEPTH_STENCIL,
         XG_TILE_MODE_2D_THIN_64KB_Z_X, XG_CONSOLE_TYPE_SCARLETT_LOCKHART,
         160u, 96u, 65536ull, (0x02u | 0x04u | 0x08u | 0x40u), "1280x720 X24_G8 64KB_Z_X Lockhart"},

        // 64x64 D32F 4KB_Z Anaconda: tile 32x32 -> padded 64x64 -> 8x8 blocks -> 256B -> pipe_align 4096B
        {64u, 64u, static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT), XG_BIND_DEPTH_STENCIL,
         XG_TILE_MODE_2D_THIN_4KB_Z, XG_CONSOLE_TYPE_SCARLETT_ANACONDA,
         8u, 8u, 4096ull, (0x02u | 0x08u | 0x40u), "64x64 D32F 4KB_Z Anaconda"},

        // 300x180 Xbox X16_G8 (120) 64KB_Z_X Anaconda: bpe=2 tile 256x128 -> padded 512x256 -> 64x32 -> 8,192B -> 65,536B
        {300u, 180u, 120u, XG_BIND_DEPTH_STENCIL,
         XG_TILE_MODE_2D_THIN_64KB_Z_X, XG_CONSOLE_TYPE_SCARLETT_ANACONDA,
         64u, 32u, 65536ull, (0x02u | 0x04u | 0x08u | 0x40u), "300x180 Xbox X16_G8 64KB_Z_X Anaconda"},
    };

    for (const auto& c : cases) {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = c.w;
        desc.Height = c.h;
        desc.MipLevels = 1u;
        desc.ArraySize = 1u;
        desc.Format = c.format;
        desc.SampleCount = 1u;
        desc.BindFlags = c.bind_flags;
        desc.TileMode = c.tile_mode;

        XG_RESOURCE_METADATA_LAYOUT meta{};
        OS_REQUIRE(xg::ComputeTexture2DMetadataLayout(desc, c.console, meta));
        OS_REQUIRE(meta.Htile.Enabled == 1u);
        OS_REQUIRE(meta.Htile.PipeAligned == 1u);
        OS_REQUIRE(meta.Htile.OffsetBytes >= meta.BaseSurfaceSizeBytes);
        OS_REQUIRE((meta.Htile.OffsetBytes % meta.Htile.AlignmentBytes) == 0ull);
        OS_REQUIRE(meta.Htile.SizeBytes == c.expected_htile_size);
        OS_REQUIRE(meta.Htile.SlicePitchBytes == c.expected_htile_size);
        OS_REQUIRE(meta.TotalSizeWithMetadataBytes >= meta.Htile.OffsetBytes + meta.Htile.SizeBytes);

        // Verify 8x8 tile coverage, 4 B per tile, pipe-interleave alignment, and usage flags.
        xg::XgPlaneUsageMetadataSummary summary{};
        OS_REQUIRE(xg::ComputeTexture2DPlaneUsageSummary(desc, c.console, summary));
        OS_REQUIRE(summary.htile_blocks_x == c.expected_blocks_x);
        OS_REQUIRE(summary.htile_blocks_y == c.expected_blocks_y);
        OS_REQUIRE(summary.htile_raw_bytes ==
                   static_cast<uint64_t>(c.expected_blocks_x) * c.expected_blocks_y * 4ull);
        OS_REQUIRE(summary.htile_size_bytes == c.expected_htile_size);
        OS_REQUIRE(summary.htile_size_bytes >= summary.htile_raw_bytes);  // Never under-allocates!
        OS_REQUIRE(summary.htile_offset_bytes == meta.Htile.OffsetBytes);
        OS_REQUIRE((summary.htile_offset_bytes % summary.htile_pipe_align_bytes) == 0ull);
        OS_REQUIRE(summary.plane_usage_flags == c.expected_usage_bits);

        std::printf("  %-42s blocks=%ux%u raw=%lluB aligned=%lluB off=%lluB flags=0x%02X\n",
                    c.label,
                    summary.htile_blocks_x, summary.htile_blocks_y,
                    static_cast<unsigned long long>(summary.htile_raw_bytes),
                    static_cast<unsigned long long>(summary.htile_size_bytes),
                    static_cast<unsigned long long>(summary.htile_offset_bytes),
                    summary.plane_usage_flags);
    }

    // Verify optimal tile mode for depth-bind textures uses Z_X swizzle.
    OS_REQUIRE(xg::ComputeOptimalTileMode(
                   static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT), 1920u, 1080u, XG_BIND_DEPTH_STENCIL) ==
               XG_TILE_MODE_2D_THIN_64KB_Z_X);

    // Negative check: non-depth R32_TYPELESS SRV must NOT enable HTILE.
    {
        XG_TEXTURE2D_DESC color_desc{};
        color_desc.Width = 256u;
        color_desc.Height = 256u;
        color_desc.MipLevels = 1u;
        color_desc.ArraySize = 1u;
        color_desc.Format = static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS);
        color_desc.SampleCount = 1u;
        color_desc.BindFlags = XG_BIND_SHADER_RESOURCE;
        color_desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S;
        xg::XgPlaneUsageMetadataSummary summary{};
        OS_REQUIRE(xg::ComputeTexture2DPlaneUsageSummary(color_desc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, summary));
        OS_REQUIRE((summary.plane_usage_flags & 0x08u) == 0u);  // No HTILE
        OS_REQUIRE((summary.plane_usage_flags & 0x01u) != 0u);  // Color plane
    }

    std::printf("[Test 5] PASS\n");
}

}  // namespace

int main() {
    TestVulkanOptions2And7();
    TestVulkanCustomBorderColor();
#if defined(_WIN32)
    RunPlacedTiledTextureTest(false);  // Live Hardware GPU
    RunPlacedTiledTextureTest(true);   // D3D12 WARP
#endif
    TestXgStencilOnlyPlanarLayouts();
    TestXgSingleMipDepthHtileLayout();
    std::printf("\nALL SPRINT 13 VK/D3D12/XG/TILED-PLACED TESTS PASSED!\n");
    return 0;
}
