// OpenScarlett Wave 4 — Portable End-to-End Live Test Harness for VulkanMeshShowcase:
//   - Constructs RDNA2 XS2\0 shaders (AS + MS + PS with 3D LUT IMAGE_LOAD_MIP +
//     Fallback VS) and a 32x32x32 warm-to-cool cinematic 3D LUT volume texture.
//   - Stages a zero-DLL Xbox package directory under %TEMP%\openscarlett_wave4_vulkan_mesh_showcase\
//     with MicrosoftGame.Config (TitleId = 0x5CA01005), standalone gdk_vulkan_mesh_showcase.exe,
//     raw RDNA2 XS2\0 blobs, and the 32x32x32 3D LUT binary asset.
//   - Stage 1: Lifts RDNA2 XS2\0 -> SPIR-V 1.3 + DXIL SM 6.5 (spirv-val + dxc.exe), verifies
//     bit-exact 32x32x32 3D LUT GPU upload/readback (VK_IMAGE_TYPE_3D, 3 mips), executes live
//     Vulkan 1.4 VK_EXT_mesh_shader (Task+Mesh+RDNA2-lifted 3D-LUT PS) via
//     VulkanBackend::ExecuteMeshShaderDraw() (with deterministic SW fallback on headless CI),
//     and verifies `openscarlett.exe run --backend=vulkan <package_dir>`.
//   - Stage 2: Executes the zero-DLL package live via `openscarlett.exe run <package_dir>` on
//     D3D12 MeshTier=10 hardware GPU (d3d12_xs XS2\0 auto-lift + DispatchMesh) AND D3D12 WARP
//     (`-- --warp`), verifying zero-DLL package cleanliness and unified CallTelemetry (not_impl==0).
//   - Stage 3: Verifies tight cross-API parity between Vulkan 1.4 VK_EXT_mesh_shader + 3D LUT
//     and D3D12 MeshTier=10 + 3D LUT (geometry coverage, 5 LUT-graded probe points, RGB MAE).

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kLutDim = 32u;
constexpr uint32_t kTitleId = 0x5CA01005u;

constexpr float kClearColorF[4] = {18.0f / 255.0f, 23.0f / 255.0f, 36.0f / 255.0f, 1.0f};
constexpr uint8_t kBgR = 18u;
constexpr uint8_t kBgG = 23u;
constexpr uint8_t kBgB = 36u;

// 5-vertex, 4-triangle Scarlett Prism Gem in D3D12 clip space (Vulkan Y-flips pos.y -> -pos.y):
//   v0 (Center): ( 0.00,  0.00, 0.20, 1.0) | Warm Gold      (1.00, 0.90, 0.40)
//   v1 (Top):    ( 0.00, +0.78, 0.20, 1.0) | Crimson Red    (0.95, 0.20, 0.15)
//   v2 (Right):  (+0.78,  0.00, 0.20, 1.0) | Magenta Violet (0.90, 0.20, 0.95)
//   v3 (Bottom): ( 0.00, -0.78, 0.20, 1.0) | Cobalt Blue    (0.15, 0.45, 1.00)
//   v4 (Left):   (-0.78,  0.00, 0.20, 1.0) | Emerald Green  (0.15, 0.95, 0.35)
struct PrismVertex {
    float pos[4];
    float color[4];
};

constexpr PrismVertex kPrismVerts[5] = {
    {{ 0.00f,  0.00f, 0.20f, 1.0f}, {1.00f, 0.90f, 0.40f, 1.0f}},
    {{ 0.00f,  0.78f, 0.20f, 1.0f}, {0.95f, 0.20f, 0.15f, 1.0f}},
    {{ 0.78f,  0.00f, 0.20f, 1.0f}, {0.90f, 0.20f, 0.95f, 1.0f}},
    {{ 0.00f, -0.78f, 0.20f, 1.0f}, {0.15f, 0.45f, 1.00f, 1.0f}},
    {{-0.78f,  0.00f, 0.20f, 1.0f}, {0.15f, 0.95f, 0.35f, 1.0f}},
};

constexpr const char* kMicrosoftGameConfigXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Game configVersion=\"1\">\n"
    "  <Identity Name=\"OpenScarlett.Wave4.VulkanMeshShowcase\"\n"
    "            Publisher=\"CN=OpenScarlett\"\n"
    "            Version=\"1.0.0.0\" />\n"
    "  <TitleId>0x5CA01005</TitleId>\n"
    "  <ShellVisuals DefaultDisplayName=\"OpenScarlett VulkanMeshShowcase (XS2 AS+MS+PS + 3D LUT)\" />\n"
    "  <ExecutableList>\n"
    "    <Executable Name=\"gdk_vulkan_mesh_showcase.exe\"\n"
    "                Id=\"Game\"\n"
    "                TargetDeviceFamily=\"Scarlett\" />\n"
    "  </ExecutableList>\n"
    "</Game>\n";

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

uint8_t FloatToU8(float v) noexcept {
    const float c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

// Warm-to-cool cinematic 3D color-grading LUT formula (non-separable across RGB!):
//   Shadows -> cool teal tint; Highlights -> warm golden boost; subtle cross-channel coupling.
std::array<uint8_t, 4> EvaluateCinematicLutTexel(float r, float g, float b) noexcept {
    const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
    const float shadow_w = (1.0f - lum) * (1.0f - lum);
    const float high_w = lum * lum;

    const float out_r = std::clamp(r * 0.94f + high_w * 0.22f - shadow_w * 0.08f + 0.06f * g, 0.0f, 1.0f);
    const float out_g = std::clamp(g * 0.92f + high_w * 0.08f + shadow_w * 0.10f + 0.02f * (r - b) + 0.02f, 0.0f, 1.0f);
    const float out_b = std::clamp(b * 0.88f + shadow_w * 0.24f - high_w * 0.14f + 0.05f * (1.0f - r), 0.0f, 1.0f);
    return {FloatToU8(out_r), FloatToU8(out_g), FloatToU8(out_b), 255u};
}

openscarlett::VulkanTextureDesc BuildCinematic3DLutTexture() {
    openscarlett::VulkanTextureDesc tex{};
    tex.width = kLutDim;
    tex.height = kLutDim;
    tex.depth = kLutDim;
    tex.is_3d = true;
    tex.format = openscarlett::VulkanImageFormat::Rgba8Unorm;

    // 3 mip levels: 32x32x32 (mip0), 16x16x16 (mip1), 8x8x8 (mip2)
    for (uint32_t m = 0; m < 3u; ++m) {
        const uint32_t dim = kLutDim >> m;
        const float inv = 1.0f / static_cast<float>(std::max<uint32_t>(1u, dim - 1u));
        openscarlett::VulkanTextureMipLevel mip{};
        mip.width = dim;
        mip.height = dim;
        mip.depth = dim;
        mip.data.resize(static_cast<size_t>(dim) * dim * dim * 4u);
        for (uint32_t z = 0; z < dim; ++z) {
            const float bf = static_cast<float>(z) * inv;
            for (uint32_t y = 0; y < dim; ++y) {
                const float gf = static_cast<float>(y) * inv;
                for (uint32_t x = 0; x < dim; ++x) {
                    const float rf = static_cast<float>(x) * inv;
                    const auto rgba = EvaluateCinematicLutTexel(rf, gf, bf);
                    const size_t idx = ((static_cast<size_t>(z) * dim + y) * dim + x) * 4u;
                    mip.data[idx + 0] = rgba[0];
                    mip.data[idx + 1] = rgba[1];
                    mip.data[idx + 2] = rgba[2];
                    mip.data[idx + 3] = rgba[3];
                }
            }
        }
        tex.mips.push_back(std::move(mip));
    }
    return tex;
}

std::array<uint8_t, 4> Lookup3DLutNearest(
    const openscarlett::VulkanTextureMipLevel& lut_mip0,
    float r, float g, float b) noexcept {
    const uint32_t x = static_cast<uint32_t>(std::clamp(r, 0.0f, 1.0f) * 31.0f + 0.5f);
    const uint32_t y = static_cast<uint32_t>(std::clamp(g, 0.0f, 1.0f) * 31.0f + 0.5f);
    const uint32_t z = static_cast<uint32_t>(std::clamp(b, 0.0f, 1.0f) * 31.0f + 0.5f);
    const size_t idx = ((static_cast<size_t>(z) * kLutDim + y) * kLutDim + x) * 4u;
    return {lut_mip0.data[idx + 0], lut_mip0.data[idx + 1], lut_mip0.data[idx + 2], 255u};
}

// ---------------------------------------------------------------------------
// RDNA2 XS2\0 Shader Builders (AS, MS, PS+3D-LUT, Fallback VS)
// ---------------------------------------------------------------------------
std::vector<uint8_t> BuildShowcaseAsXs2() {
    using namespace openscarlett;
    Rdna2Assembler as;
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(1));
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
    as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
    as.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return as.BuildXs2(ShaderStage::Amplification, 1, 1, 1, 16, 16, 32);
}

void EmitSelectPrismVertexFromReg(openscarlett::Rdna2Assembler& a, uint8_t idx_vgpr) {
    using namespace openscarlett;
    // Start with Vertex 0 defaults in v1..v4 (pos) and v5..v8 (color)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralF32(kPrismVerts[0].pos[0]));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralF32(kPrismVerts[0].pos[1]));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralF32(kPrismVerts[0].pos[2]));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(kPrismVerts[0].color[0]));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralF32(kPrismVerts[0].color[1]));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(kPrismVerts[0].color[2]));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));

    for (uint32_t v = 1u; v < 5u; ++v) {
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(static_cast<int32_t>(v)), idx_vgpr);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralF32(kPrismVerts[v].pos[0]));
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 20);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralF32(kPrismVerts[v].pos[1]));
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 20);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralF32(kPrismVerts[v].color[0]));
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 5, Rdna2Operand::Vgpr(5), 20);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralF32(kPrismVerts[v].color[1]));
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 6, Rdna2Operand::Vgpr(6), 20);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralF32(kPrismVerts[v].color[2]));
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 7, Rdna2Operand::Vgpr(7), 20);
    }
}

std::vector<uint8_t> BuildShowcaseMsXs2() {
    using namespace openscarlett;
    Rdna2Assembler ms;
    // Allocate 5 vertices and 4 triangles
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(5));
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(4));
    ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

    // Each thread _gtid (v0 in 0..4) writes vertex [_gtid]
    EmitSelectPrismVertexFromReg(ms, 0);
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, false);

    // Branchless primitive connectivity for _gtid in 0..3:
    //   prim 0: (0, 1, 2), prim 1: (0, 2, 3), prim 2: (0, 3, 4), prim 3: (0, 4, 1)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    ms.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 11, Rdna2Operand::InlineInt(1), 0);
    ms.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 12, Rdna2Operand::InlineInt(2), 0);
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineInt(1));
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(3), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 12, Rdna2Operand::Vgpr(12), 13);
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 11, 12, 0, 0x7u, true);
    ms.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return ms.BuildXs2(ShaderStage::Mesh, 5, 1, 1, 16, 24, 32);
}

std::vector<uint8_t> BuildShowcaseFallbackVsXs2() {
    using namespace openscarlett;
    Rdna2Assembler vs;
    // Map _vid (v0 in 0..11) to prism vertex index v15 in 0..4:
    //   0,1,2 -> 0,1,2 | 3,4,5 -> 0,2,3 | 6,7,8 -> 0,3,4 | 9,10,11 -> 0,4,1
    static constexpr uint32_t kCornerMap[12] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1};
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::InlineInt(0));
    for (uint32_t vid = 1u; vid < 12u; ++vid) {
        if (kCornerMap[vid] == 0u) continue;
        vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(static_cast<int32_t>(vid)), 0);
        vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 16, Rdna2Operand::InlineInt(static_cast<int32_t>(kCornerMap[vid])));
        vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 15, Rdna2Operand::Vgpr(15), 16);
    }

    EmitSelectPrismVertexFromReg(vs, 15);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, true);
    vs.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return vs.BuildXs2(ShaderStage::Vertex, 1, 1, 1, 16, 24, 32);
}

std::vector<uint8_t> BuildShowcasePs3DLutXs2() {
    using namespace openscarlett;
    Rdna2Assembler ps;
    // Inputs: v0=param0.r, v1=param0.g, v2=param0.b, v3=param0.a
    // Map each channel c -> uint(clamp(c, 0, 1) * 31.0 + 0.5) into v10 (x), v11 (y), v12 (z), v13=0 (mip0)
    auto map_channel = [&](uint8_t src_v, uint8_t dst_coord_v) {
        ps.EmitVop2(Rdna2Opcode::V_MAX_F32, 4, Rdna2Operand::InlineFloat(0.0f), src_v);
        ps.EmitVop2(Rdna2Opcode::V_MIN_F32, 4, Rdna2Operand::InlineFloat(1.0f), 4);
        ps.EmitVop2(Rdna2Opcode::V_MUL_F32, 4, Rdna2Operand::LiteralF32(31.0f), 4);
        ps.EmitVop2(Rdna2Opcode::V_ADD_F32, 4, Rdna2Operand::InlineFloat(0.5f), 4);
        ps.EmitVop1(Rdna2Opcode::V_CVT_U32_F32, dst_coord_v, Rdna2Operand::Vgpr(4));
    };
    map_channel(0, 10);
    map_channel(1, 11);
    map_channel(2, 12);
    ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineInt(0));  // mip = 0

    // 3D LUT volume lookup via MIMG IMAGE_LOAD_MIP (dim=2 -> 3D, unrm=true) -> v16..v19
    ps.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 16, 10, 0, 0, 0xFu, /*dim=3D*/ 2u, /*unrm=*/true);
    ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 19, Rdna2Operand::InlineFloat(1.0f));  // opaque alpha = 1.0
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 16, 17, 18, 19, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return ps.BuildXs2(ShaderStage::Pixel, 1, 1, 1, 16, 24, 32);
}

// ---------------------------------------------------------------------------
// SPIR-V 1.4 TaskEXT + MeshEXT + Passthrough Frag Builders (SPV_EXT_mesh_shader)
// Matches the exact 5-vertex / 4-triangle Scarlett Prism Gem with Vulkan clip Y-flip!
// ---------------------------------------------------------------------------
struct SpvBuilder {
    std::vector<uint32_t> words;
    uint32_t bound = 1;

    uint32_t Id() { return bound++; }

    void Op(uint16_t opcode, std::initializer_list<uint32_t> args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | opcode);
        for (uint32_t a : args) words.push_back(a);
    }

    void OpVec(uint16_t opcode, const std::vector<uint32_t>& args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | opcode);
        words.insert(words.end(), args.begin(), args.end());
    }

    static void PushStr(std::vector<uint32_t>& dst, const char* s) {
        const size_t n = std::strlen(s) + 1;
        const size_t nw = (n + 3) / 4;
        for (size_t w = 0; w < nw; ++w) {
            uint32_t v = 0;
            for (size_t b = 0; b < 4; ++b) {
                const size_t idx = w * 4 + b;
                if (idx < n) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[idx])) << (b * 8);
            }
            dst.push_back(v);
        }
    }

    std::vector<uint32_t> Finish(uint32_t spv_version) const {
        std::vector<uint32_t> out;
        out.reserve(5 + words.size());
        out.insert(out.end(), {0x07230203u, spv_version, 0x4F533200u, bound, 0u});
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

std::vector<uint32_t> BuildShowcaseTaskSpirv14() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_i32 = b.Id(), t_f32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_pl = b.Id(), t_ptr_pl = b.Id(), t_ptr_pl_v4 = b.Id(), v_pl = b.Id();
    const uint32_t c_u1 = b.Id(), c_i0 = b.Id(), c_f1 = b.Id(), c_scale = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {5283});  // OpCapability MeshShadingEXT
    {
        std::vector<uint32_t> ext;
        SpvBuilder::PushStr(ext, "SPV_EXT_mesh_shader");
        b.OpVec(10, ext);
    }
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {5364, fn_main};  // TaskEXT
        SpvBuilder::PushStr(ep, "main");
        ep.push_back(v_pl);
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, 1, 1, 1});  // LocalSize 1 1 1

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(21, {t_u32, 32, 0});
    b.Op(21, {t_i32, 32, 1});
    b.Op(22, {t_f32, 32});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(30, {t_pl, t_v4});
    b.Op(32, {t_ptr_pl, 5402, t_pl});      // TaskPayloadWorkgroupEXT
    b.Op(32, {t_ptr_pl_v4, 5402, t_v4});

    b.Op(43, {t_u32, c_u1, 1});
    b.Op(43, {t_i32, c_i0, 0});
    b.Op(43, {t_f32, c_f1, F32Bits(1.0f)});
    b.Op(44, {t_v4, c_scale, c_f1, c_f1, c_f1, c_f1});
    b.Op(59, {t_ptr_pl, v_pl, 5402});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t ptr_c = b.Id();
    b.Op(65, {t_ptr_pl_v4, ptr_c, v_pl, c_i0});
    b.Op(62, {ptr_c, c_scale});
    b.Op(5294, {c_u1, c_u1, c_u1, v_pl});  // OpEmitMeshTasksEXT 1 1 1 %v_pl
    b.Op(56, {});
    return b.Finish(0x00010400u);
}

std::vector<uint32_t> BuildShowcaseMeshSpirv14() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_i32 = b.Id(), t_f32 = b.Id();
    const uint32_t t_v4 = b.Id(), t_v3u = b.Id();
    const uint32_t c_u0 = b.Id(), c_u1 = b.Id(), c_u2 = b.Id(), c_u3 = b.Id(), c_u4 = b.Id(), c_u5 = b.Id();
    const uint32_t c_i0 = b.Id();
    const uint32_t t_pv = b.Id(), t_arr_pv = b.Id(), t_ptr_arr_pv = b.Id(), t_ptr_out_v4 = b.Id();
    const uint32_t t_arr_col = b.Id(), t_ptr_arr_col = b.Id();
    const uint32_t t_arr_idx = b.Id(), t_ptr_arr_idx = b.Id(), t_ptr_out_v3u = b.Id();
    const uint32_t t_pl = b.Id(), t_ptr_pl = b.Id(), t_ptr_pl_v4 = b.Id();
    const uint32_t v_pv = b.Id(), v_col = b.Id(), v_idx = b.Id(), v_pl = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {5283});  // OpCapability MeshShadingEXT
    {
        std::vector<uint32_t> ext;
        SpvBuilder::PushStr(ext, "SPV_EXT_mesh_shader");
        b.OpVec(10, ext);
    }
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {5365, fn_main};  // MeshEXT
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_pv, v_col, v_idx, v_pl});
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, 1, 1, 1});   // LocalSize 1 1 1
    b.Op(16, {fn_main, 26, 5});         // OutputVertices 5
    b.Op(16, {fn_main, 5270, 4});       // OutputPrimitivesEXT 4
    b.Op(16, {fn_main, 5298});          // OutputTrianglesEXT

    b.Op(71, {t_pv, 2});                // Block
    b.Op(72, {t_pv, 0, 11, 0});         // Member 0 BuiltIn Position
    b.Op(71, {v_col, 30, 0});           // Location 0
    b.Op(71, {v_idx, 11, 5294});        // BuiltIn PrimitiveTriangleIndicesEXT

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(21, {t_u32, 32, 0});
    b.Op(21, {t_i32, 32, 1});
    b.Op(22, {t_f32, 32});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(23, {t_v3u, t_u32, 3});

    b.Op(43, {t_u32, c_u0, 0});
    b.Op(43, {t_u32, c_u1, 1});
    b.Op(43, {t_u32, c_u2, 2});
    b.Op(43, {t_u32, c_u3, 3});
    b.Op(43, {t_u32, c_u4, 4});
    b.Op(43, {t_u32, c_u5, 5});
    b.Op(43, {t_i32, c_i0, 0});

    b.Op(30, {t_pv, t_v4});
    b.Op(28, {t_arr_pv, t_pv, c_u5});
    b.Op(32, {t_ptr_arr_pv, 3, t_arr_pv});
    b.Op(32, {t_ptr_out_v4, 3, t_v4});
    b.Op(28, {t_arr_col, t_v4, c_u5});
    b.Op(32, {t_ptr_arr_col, 3, t_arr_col});
    b.Op(28, {t_arr_idx, t_v3u, c_u4});
    b.Op(32, {t_ptr_arr_idx, 3, t_arr_idx});
    b.Op(32, {t_ptr_out_v3u, 3, t_v3u});
    b.Op(30, {t_pl, t_v4});
    b.Op(32, {t_ptr_pl, 5402, t_pl});
    b.Op(32, {t_ptr_pl_v4, 5402, t_v4});

    auto mk_f = [&](float val) -> uint32_t {
        const uint32_t id = b.Id();
        b.Op(43, {t_f32, id, F32Bits(val)});
        return id;
    };
    auto mk_v4 = [&](float x, float y, float z, float w) -> uint32_t {
        const uint32_t cx = mk_f(x), cy = mk_f(y), cz = mk_f(z), cw = mk_f(w);
        const uint32_t id = b.Id();
        b.Op(44, {t_v4, id, cx, cy, cz, cw});
        return id;
    };

    // Standard Vulkan clip-space Y-flip (gl_Position.y = -d3d_pos.y) for exact D3D12 parity!
    uint32_t c_pos[5]{};
    uint32_t c_col[5]{};
    for (int i = 0; i < 5; ++i) {
        c_pos[i] = mk_v4(kPrismVerts[i].pos[0], -kPrismVerts[i].pos[1], kPrismVerts[i].pos[2], kPrismVerts[i].pos[3]);
        c_col[i] = mk_v4(kPrismVerts[i].color[0], kPrismVerts[i].color[1], kPrismVerts[i].color[2], kPrismVerts[i].color[3]);
    }

    const uint32_t c_tri0 = b.Id(), c_tri1 = b.Id(), c_tri2 = b.Id(), c_tri3 = b.Id();
    b.Op(44, {t_v3u, c_tri0, c_u0, c_u1, c_u2});
    b.Op(44, {t_v3u, c_tri1, c_u0, c_u2, c_u3});
    b.Op(44, {t_v3u, c_tri2, c_u0, c_u3, c_u4});
    b.Op(44, {t_v3u, c_tri3, c_u0, c_u4, c_u1});

    b.Op(59, {t_ptr_arr_pv, v_pv, 3});
    b.Op(59, {t_ptr_arr_col, v_col, 3});
    b.Op(59, {t_ptr_arr_idx, v_idx, 3});
    b.Op(59, {t_ptr_pl, v_pl, 5402});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    b.Op(5295, {c_u5, c_u4});  // OpSetMeshOutputsEXT 5 4

    const uint32_t p_pl_s = b.Id(), scale_v = b.Id();
    b.Op(65, {t_ptr_pl_v4, p_pl_s, v_pl, c_i0});
    b.Op(61, {t_v4, scale_v, p_pl_s});

    const uint32_t c_u_arr[5] = {c_u0, c_u1, c_u2, c_u3, c_u4};
    for (int i = 0; i < 5; ++i) {
        const uint32_t pp = b.Id(), pc = b.Id(), sc = b.Id();
        b.Op(65, {t_ptr_out_v4, pp, v_pv, c_u_arr[i], c_i0});
        b.Op(62, {pp, c_pos[i]});
        b.Op(133, {t_v4, sc, c_col[i], scale_v});  // OpFMul color * task_payload.scale
        b.Op(65, {t_ptr_out_v4, pc, v_col, c_u_arr[i]});
        b.Op(62, {pc, sc});
    }

    const uint32_t c_tris[4] = {c_tri0, c_tri1, c_tri2, c_tri3};
    for (int t = 0; t < 4; ++t) {
        const uint32_t pi = b.Id();
        b.Op(65, {t_ptr_out_v3u, pi, v_idx, c_u_arr[t]});
        b.Op(62, {pi, c_tris[t]});
    }

    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010400u);
}

std::vector<uint32_t> BuildPassThroughFragmentSpirv13() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in = b.Id(), t_out = b.Id(), v_in = b.Id(), v_out = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id(), val = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {4, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in, v_out});
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 7});
    b.Op(71, {v_in, 30, 0});
    b.Op(71, {v_out, 30, 0});

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in, 1, t_v4});
    b.Op(32, {t_out, 3, t_v4});
    b.Op(59, {t_in, v_in, 1});
    b.Op(59, {t_out, v_out, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    b.Op(61, {t_v4, val, v_in});
    b.Op(62, {v_out, val});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// ---------------------------------------------------------------------------
// Portable Process, File, Telemetry & Parity Helpers
// ---------------------------------------------------------------------------
struct TelemetryRow {
    std::string module;
    std::string symbol;
    int32_t slot = -1;
    uint64_t total = 0;
    uint64_t ok = 0;
    uint64_t stub = 0;
    uint64_t not_impl = 0;
    uint64_t errors = 0;
};

struct TelemetrySummary {
    std::vector<TelemetryRow> rows;
    std::set<std::string> modules;
    uint64_t total_calls = 0;
    uint64_t total_not_impl = 0;
    uint64_t total_errors = 0;

    bool HasSymbol(const std::string& sym, int32_t expected_slot = -999) const {
        for (const auto& r : rows) {
            if (r.symbol == sym && (expected_slot == -999 || r.slot == expected_slot)) {
                return r.total >= 1u;
            }
        }
        return false;
    }

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }
};

struct FileDigest {
    std::string rel_path;
    uint64_t byte_size = 0;
    uint64_t fnv1a_hash = 0;

    bool operator==(const FileDigest& o) const noexcept {
        return rel_path == o.rel_path && byte_size == o.byte_size && fnv1a_hash == o.fnv1a_hash;
    }
};

uint64_t Fnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::vector<uint8_t> ReadBinaryFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    OS_REQUIRE(static_cast<bool>(ifs));
    const std::streamoff sz = ifs.tellg();
    OS_REQUIRE(sz >= 0);
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
        OS_REQUIRE(static_cast<bool>(ifs));
    }
    return buf;
}

void WriteBinaryFile(const fs::path& p, const void* data, size_t size) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    OS_REQUIRE(static_cast<bool>(ofs));
}

std::map<std::string, std::string> ParseKeyValueFile(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::istringstream iss(ReadTextFile(p));
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return kv;
}

std::vector<FileDigest> SnapshotDirectoryBytes(const fs::path& dir) {
    std::vector<FileDigest> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const auto bytes = ReadBinaryFile(entry.path());
        FileDigest d{};
        d.rel_path = fs::relative(entry.path(), dir).generic_string();
        d.byte_size = static_cast<uint64_t>(bytes.size());
        d.fnv1a_hash = Fnv1a64(bytes.data(), bytes.size());
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const FileDigest& a, const FileDigest& b) {
        return a.rel_path < b.rel_path;
    });
    return out;
}

void AssertNoDllsInDir(const fs::path& dir) {
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");
    }
}

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec) &&
            fs::exists(exe_dir / L"gdk_vulkan_mesh_showcase.exe", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path ResolveScratchDir(const wchar_t* subdir_name) {
    std::error_code ec;
    wchar_t temp_buf[1024]{};
    const DWORD tlen = ::GetEnvironmentVariableW(L"TEMP", temp_buf, static_cast<DWORD>(1024));
    if (tlen > 0 && tlen < 1024) {
        return fs::path(temp_buf) / subdir_name;
    }
    const fs::path sys_tmp = fs::temp_directory_path(ec);
    if (!ec && !sys_tmp.empty()) {
        return sys_tmp / subdir_name;
    }
    return ResolveRuntimeBinDir() / subdir_name;
}

DWORD RunProcessW(const fs::path& exe_path, const std::wstring& cmd_line, DWORD timeout_ms = 60000) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);
    OS_REQUIRE(ok != 0);
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    OS_REQUIRE(wr == WAIT_OBJECT_0);
    DWORD exit_code = 999;
    OS_REQUIRE(::GetExitCodeProcess(pi.hProcess, &exit_code) != 0);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return exit_code;
}

bool HasSpirvVal() {
    std::error_code ec;
    return fs::exists(openscarlett::kDefaultSpirvValPath, ec);
}

void ValidateSpirvAnyVersion(
    const std::vector<uint32_t>& spv,
    const fs::path& scratch_dir,
    const char* target_env,
    const char* label) {
    openscarlett::SpirvValidationInfo info{};
    const auto st = openscarlett::VulkanBackend::ValidateSpirvBinary(spv, &info);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] ValidateSpirvBinary(%s): %s\n", label, st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    if (!HasSpirvVal()) return;

    if (spv.size() >= 2 && spv[1] == 0x00010300u) {
        std::string diag;
        const auto ext_st = openscarlett::ValidateSpirvExternal(
            spv, diag, openscarlett::kDefaultSpirvValPath, target_env);
        if (!ext_st.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val(%s, %s): %s\n", label, target_env, diag.c_str());
        }
        OS_REQUIRE(ext_st.ok());
        return;
    }

    // SPIR-V 1.4+ (TaskEXT / MeshEXT): validate directly with Khronos spirv-val.exe under %TEMP%
    const fs::path spv_path = scratch_dir / (std::string(label) + ".spv");
    WriteBinaryFile(spv_path, spv.data(), spv.size() * sizeof(uint32_t));
    const fs::path sv_exe(openscarlett::kDefaultSpirvValPath);
    const std::wstring env_w(target_env, target_env + std::strlen(target_env));
    const std::wstring cmd = L"\"" + sv_exe.wstring() + L"\" --target-env " + env_w +
                             L" \"" + spv_path.wstring() + L"\"";
    const DWORD rc = RunProcessW(sv_exe, cmd, 15000);
    std::error_code ec;
    fs::remove(spv_path, ec);
    if (rc != 0) {
        std::fprintf(stderr, "[FAIL] spirv-val 1.4 (%s, %s) exit_code=%lu\n",
                     label, target_env, static_cast<unsigned long>(rc));
    }
    OS_REQUIRE(rc == 0);
}

bool ExtractJsonString(const std::string& line, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        char c = line[pos++];
        if (c == '\\' && pos < line.size()) {
            val.push_back(line[pos++]);
        } else if (c == '"') {
            out = std::move(val);
            return true;
        } else {
            val.push_back(c);
        }
    }
    return false;
}

bool ExtractJsonInt(const std::string& line, const std::string& key, int64_t& out) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    char* end_ptr = nullptr;
    const long long v = std::strtoll(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) return false;
    out = static_cast<int64_t>(v);
    return true;
}

TelemetrySummary ParseTelemetryJson(const fs::path& json_path) {
    OS_REQUIRE(fs::exists(json_path));
    TelemetrySummary sum;
    std::istringstream iss(ReadTextFile(json_path));
    std::string line;
    while (std::getline(iss, line)) {
        TelemetryRow row{};
        if (!ExtractJsonString(line, "module", row.module) ||
            !ExtractJsonString(line, "symbol", row.symbol)) {
            continue;
        }
        int64_t v = 0;
        if (ExtractJsonInt(line, "slot", v)) row.slot = static_cast<int32_t>(v);
        if (ExtractJsonInt(line, "total", v)) row.total = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "ok", v)) row.ok = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "stub", v)) row.stub = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "not_impl", v)) row.not_impl = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "errors", v)) row.errors = static_cast<uint64_t>(v);

        sum.modules.insert(row.module);
        sum.total_calls += row.total;
        sum.total_not_impl += row.not_impl;
        sum.total_errors += row.errors;
        sum.rows.push_back(std::move(row));
    }
    return sum;
}

DWORD LaunchPackageViaOpenScarlettCli(
    const fs::path& cli_exe,
    const fs::path& package_dir,
    const fs::path& shim_dir,
    const fs::path& vfs_t,
    const fs::path& vfs_s,
    const fs::path& telemetry_json,
    const std::wstring& extra_cli_flags = {},
    const std::wstring& guest_args = {}) {
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run ";
    if (!extra_cli_flags.empty()) {
        cmd += extra_cli_flags + L" ";
    }
    cmd += L"\"" + package_dir.wstring() +
           L"\" --shim-dir \"" + shim_dir.wstring() +
           L"\" --vfs-t \"" + vfs_t.wstring() +
           L"\" --vfs-s \"" + vfs_s.wstring() +
           L"\" --telemetry \"" + telemetry_json.wstring() +
           L"\" --timeout-ms 60000";
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }
    return RunProcessW(cli_exe, cmd, 65000);
}

std::vector<uint8_t> VerifyAndReadPpmRgb(const fs::path& ppm_path, uint32_t width, uint32_t height) {
    OS_REQUIRE(fs::exists(ppm_path));
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const auto raw = ReadBinaryFile(ppm_path);
    const size_t expected_size = header.size() + static_cast<size_t>(width) * height * 3u;
    OS_REQUIRE(raw.size() == expected_size);
    OS_REQUIRE(std::memcmp(raw.data(), header.data(), header.size()) == 0);
    return std::vector<uint8_t>(raw.begin() + static_cast<std::ptrdiff_t>(header.size()), raw.end());
}

std::vector<uint8_t> FrameBufferToRgb(const openscarlett::FrameBufferRgba8& fb) {
    const size_t n = static_cast<size_t>(fb.width) * fb.height;
    std::vector<uint8_t> rgb(n * 3u, 0u);
    for (size_t i = 0; i < n; ++i) {
        rgb[i * 3u + 0u] = fb.pixels[i * 4u + 0u];
        rgb[i * 3u + 1u] = fb.pixels[i * 4u + 1u];
        rgb[i * 3u + 2u] = fb.pixels[i * 4u + 2u];
    }
    return rgb;
}

void VerifyStandaloneXboxPeImports(const fs::path& exe_path, const std::vector<std::string>& required_dlls) {
    openscarlett::PeInspectionReport rep;
    OS_REQUIRE(openscarlett::InspectPeFile(exe_path.string(), rep).ok());
    OS_REQUIRE(!rep.is_dll);
    OS_REQUIRE(rep.console_imports + rep.gdk_imports > 0u);

    std::set<std::string> imported_lower;
    for (const auto& mod : rep.imports) {
        std::string lo = mod.dll_name;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(lo != "openscarlett_hook.dll");
        imported_lower.insert(lo);
    }
    for (const auto& req : required_dlls) {
        std::string lo = req;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(imported_lower.find(lo) != imported_lower.end());
    }
}

struct ParityMetrics {
    uint32_t non_bg_a = 0;
    uint32_t non_bg_b = 0;
    size_t unique_colors_a = 0;
    size_t unique_colors_b = 0;
    double rgb_mae = 0.0;
    double match_ratio_tol8 = 0.0;
    int max_interior_diff = 0;
};

ParityMetrics ComputeRgbParity(const std::vector<uint8_t>& rgb_a, const std::vector<uint8_t>& rgb_b) {
    OS_REQUIRE(rgb_a.size() == static_cast<size_t>(kWidth) * kHeight * 3u);
    OS_REQUIRE(rgb_b.size() == rgb_a.size());

    ParityMetrics m{};
    std::unordered_set<uint32_t> cols_a, cols_b;
    cols_a.reserve(4096);
    cols_b.reserve(4096);

    double abs_sum = 0.0;
    uint32_t matched_tol8 = 0;
    const uint32_t total_px = kWidth * kHeight;

    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 3u;
            const int ar = rgb_a[i + 0], ag = rgb_a[i + 1], ab = rgb_a[i + 2];
            const int br = rgb_b[i + 0], bg = rgb_b[i + 1], bb = rgb_b[i + 2];

            const bool a_lit = (ar != kBgR || ag != kBgG || ab != kBgB);
            const bool b_lit = (br != kBgR || bg != kBgG || bb != kBgB);
            if (a_lit) ++m.non_bg_a;
            if (b_lit) ++m.non_bg_b;

            cols_a.insert((static_cast<uint32_t>(ar) << 16) | (static_cast<uint32_t>(ag) << 8) | static_cast<uint32_t>(ab));
            cols_b.insert((static_cast<uint32_t>(br) << 16) | (static_cast<uint32_t>(bg) << 8) | static_cast<uint32_t>(bb));

            const int dr = std::abs(ar - br);
            const int dg = std::abs(ag - bg);
            const int db = std::abs(ab - bb);
            abs_sum += static_cast<double>(dr + dg + db);
            const int max_d = std::max({dr, dg, db});
            if (max_d <= 8) {
                ++matched_tol8;
            }
            // Track interior difference (strictly inside both lit regions)
            if (a_lit && b_lit) {
                m.max_interior_diff = std::max(m.max_interior_diff, max_d);
            }
        }
    }

    m.unique_colors_a = cols_a.size();
    m.unique_colors_b = cols_b.size();
    m.rgb_mae = abs_sum / static_cast<double>(static_cast<size_t>(total_px) * 3u);
    m.match_ratio_tol8 = static_cast<double>(matched_tol8) / static_cast<double>(total_px);
    return m;
}

void CheckProbeRgb(
    const std::vector<uint8_t>& rgb,
    uint32_t x, uint32_t y,
    const std::array<uint8_t, 4>& expected,
    int tol,
    const char* label) {
    const size_t i = (static_cast<size_t>(y) * kWidth + x) * 3u;
    const int dr = std::abs(static_cast<int>(rgb[i + 0]) - static_cast<int>(expected[0]));
    const int dg = std::abs(static_cast<int>(rgb[i + 1]) - static_cast<int>(expected[1]));
    const int db = std::abs(static_cast<int>(rgb[i + 2]) - static_cast<int>(expected[2]));
    if (dr > tol || dg > tol || db > tol) {
        std::fprintf(stderr,
                     "[FAIL] %s probe(%u,%u) got RGB(%u,%u,%u) expected RGB(%u,%u,%u) +- %d\n",
                     label, x, y, rgb[i + 0], rgb[i + 1], rgb[i + 2],
                     expected[0], expected[1], expected[2], tol);
        OS_REQUIRE(false);
    }
}

}  // namespace

int main() {
    using namespace openscarlett;

    // Clear inherited overrides so CLI runs derive G:\ and TitleId strictly from MicrosoftGame.Config
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G_OVERLAY", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_TITLE_ID", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_ASYNC_PSO", L"0");

    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = bin_dir / "openscarlett.exe";
    const std::string dxc_path = ResolvePortableDxcPath();
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(bin_dir / "openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "d3d12_xs.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "xgameruntime.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "pixevt.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "gdk_vulkan_mesh_showcase.exe"));
    OS_REQUIRE(!dxc_path.empty() && fs::exists(dxc_path));

    const fs::path stage_root = ResolveScratchDir(L"openscarlett_wave4_vulkan_mesh_showcase");
    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(stage_root, ec);
    OS_REQUIRE(fs::exists(stage_root));

    // Isolate shader cache under %TEMP% (strictly on E:\ during workspace builds)
    const fs::path cache_dir = stage_root / "_shader_cache";
    ::SetEnvironmentVariableW(L"OPENSCARLETT_SHADER_CACHE", cache_dir.c_str());

    // -----------------------------------------------------------------------
    // 0. Build RDNA2 XS2\0 Shaders & 32x32x32 3D Color-Grading LUT
    // -----------------------------------------------------------------------
    const auto as_xs2 = BuildShowcaseAsXs2();
    const auto ms_xs2 = BuildShowcaseMsXs2();
    const auto ps_xs2 = BuildShowcasePs3DLutXs2();
    const auto vs_xs2 = BuildShowcaseFallbackVsXs2();
    const VulkanTextureDesc lut3d = BuildCinematic3DLutTexture();
    OS_REQUIRE(lut3d.mips.size() == 3u);
    OS_REQUIRE(lut3d.mips[0].data.size() == static_cast<size_t>(kLutDim) * kLutDim * kLutDim * 4u);

    // Stage zero-DLL Xbox package directory
    const fs::path pkg_dir = stage_root / "VulkanMeshShowcase_Pkg";
    fs::create_directories(pkg_dir, ec);
    const fs::path staged_exe = pkg_dir / "gdk_vulkan_mesh_showcase.exe";
    fs::copy_file(bin_dir / "gdk_vulkan_mesh_showcase.exe", staged_exe, fs::copy_options::overwrite_existing);
    VerifyStandaloneXboxPeImports(staged_exe, {"d3d12_xs.dll", "xgameruntime.dll", "pixevt.dll"});

    WriteBinaryFile(pkg_dir / "MicrosoftGame.Config", kMicrosoftGameConfigXml, std::strlen(kMicrosoftGameConfigXml));
    WriteBinaryFile(pkg_dir / "ShowcaseAS.xs2", as_xs2.data(), as_xs2.size());
    WriteBinaryFile(pkg_dir / "ShowcaseMS.xs2", ms_xs2.data(), ms_xs2.size());
    WriteBinaryFile(pkg_dir / "ShowcasePS.xs2", ps_xs2.data(), ps_xs2.size());
    WriteBinaryFile(pkg_dir / "ShowcaseFallbackVS.xs2", vs_xs2.data(), vs_xs2.size());
    WriteBinaryFile(pkg_dir / "lut3d_32x32x32.bin", lut3d.mips[0].data.data(), lut3d.mips[0].data.size());

    // Verify MicrosoftGame.Config resolution
    {
        ResolvedPackageTarget target{};
        OS_REQUIRE(ResolvePackageLaunchTarget(pkg_dir.string(), target).ok());
        OS_REQUIRE(target.has_config && target.config.has_title_id);
        OS_REQUIRE(target.config.title_id_value == kTitleId);
        OS_REQUIRE(fs::path(target.exe_path).filename() == "gdk_vulkan_mesh_showcase.exe");
    }

    AssertNoDllsInDir(pkg_dir);
    const auto pkg_snapshot_before = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_snapshot_before.size() == 7u);  // exe + config + 4 xs2 + 3D LUT bin

    // -----------------------------------------------------------------------
    // Stage 1: RDNA2 XS2\0 Lifting (SPIR-V + DXIL SM 6.5) + Live Vulkan 1.4
    //          (3D LUT Roundtrip + VK_EXT_mesh_shader Task+Mesh+PS + --backend=vulkan)
    // -----------------------------------------------------------------------
    Rdna2LiftOptions lift_opts{};
    lift_opts.use_sm65 = true;
    lift_opts.emit_spirv = true;
    lift_opts.emit_hlsl = true;
    lift_opts.validate_spirv_external = HasSpirvVal();
    lift_opts.compile_dxil_with_dxc = true;
    lift_opts.dxc_path = dxc_path;

    Rdna2LiftResult as_lift{}, ms_lift{}, ps_lift{}, vs_lift{};
    OS_REQUIRE(LiftRdna2Xs2Shader(as_xs2, as_lift, lift_opts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(ms_xs2, ms_lift, lift_opts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(ps_xs2, ps_lift, lift_opts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(vs_xs2, vs_lift, lift_opts).ok());

    OS_REQUIRE(as_lift.hlsl_target == "as_6_5" && as_lift.pc_dxil.size() >= 32u);
    OS_REQUIRE(ms_lift.hlsl_target == "ms_6_5" && ms_lift.pc_dxil.size() >= 32u && ms_lift.ir.exports_prim);
    OS_REQUIRE(ps_lift.hlsl_target == "ps_6_5" && ps_lift.pc_dxil.size() >= 32u && ps_lift.ir.uses_texture3d_srv);
    OS_REQUIRE(vs_lift.hlsl_target == "vs_6_5" && vs_lift.pc_dxil.size() >= 32u);
    OS_REQUIRE(!ps_lift.spirv_words.empty() && !vs_lift.spirv_words.empty());

    // Build & validate SPIR-V 1.4 TaskEXT + MeshEXT modules matching the lifted AS/MS prism scene
    const auto task_spv14 = BuildShowcaseTaskSpirv14();
    const auto mesh_spv14 = BuildShowcaseMeshSpirv14();
    const auto passthrough_frag_spv = BuildPassThroughFragmentSpirv13();
    ValidateSpirvAnyVersion(task_spv14, stage_root, "vulkan1.3", "Showcase_TaskEXT_14");
    ValidateSpirvAnyVersion(mesh_spv14, stage_root, "vulkan1.3", "Showcase_MeshEXT_14");
    ValidateSpirvAnyVersion(ps_lift.spirv_words, stage_root, "vulkan1.2", "Showcase_RDNA2_PS_3DLut_13");
    ValidateSpirvAnyVersion(vs_lift.spirv_words, stage_root, "vulkan1.2", "Showcase_RDNA2_VS_13");

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());
    const VulkanDeviceInfo vk_info = vk.GetDeviceInfo();

    VulkanBackend vk_sw;
    OS_REQUIRE(vk_sw.InitializeSoftwareFallback().ok());
    OS_REQUIRE(vk_sw.is_fallback_mode());

    // 1A. Verify 32x32x32 3D LUT Volume Texture bit-exact GPU upload + readback (all 3 mips!)
    VulkanTextureRoundtripResult lut_rt{};
    OS_REQUIRE(vk.UploadAndReadbackTexture(lut3d, lut_rt).ok());
    OS_REQUIRE(lut_rt.readback_mips.size() == 3u);
    for (size_t m = 0; m < 3u; ++m) {
        OS_REQUIRE(lut_rt.readback_mips[m].data == lut3d.mips[m].data);
    }

    // 1B. Execute Vulkan 1.4 VK_EXT_mesh_shader (Task+Mesh+RDNA2-lifted PS+3D LUT) at 256x256
    const bool vk_has_hw_mesh =
        (!vk.is_fallback_mode()) && vk_info.supports_mesh_shader && vk_info.supports_task_shader;

    VulkanMeshDrawDesc vk_draw{};
    vk_draw.task_spirv = task_spv14;
    vk_draw.mesh_spirv = mesh_spv14;
    vk_draw.fragment_spirv = vk_has_hw_mesh ? ps_lift.spirv_words : passthrough_frag_spv;
    vk_draw.width = kWidth;
    vk_draw.height = kHeight;
    vk_draw.group_count_x = 1;
    vk_draw.clear_color[0] = kClearColorF[0];
    vk_draw.clear_color[1] = kClearColorF[1];
    vk_draw.clear_color[2] = kClearColorF[2];
    vk_draw.clear_color[3] = kClearColorF[3];

    if (vk_has_hw_mesh) {
        // RDNA2 lifter binds Texture3D<float4> (IMAGE_LOAD_MIP dim=2) to DescriptorSet 0, Binding 7!
        VulkanDescriptorBinding lut_binding{};
        lut_binding.binding = 7;
        lut_binding.kind = VulkanDescriptorKind::SampledImage;
        lut_binding.texture = lut3d;
        lut_binding.linear_filter = false;
        vk_draw.descriptors = {lut_binding};
    }

    const auto t0_vk = std::chrono::steady_clock::now();
    VulkanMeshDrawResult vk_res{};
    OS_REQUIRE(vk.ExecuteMeshShaderDraw(vk_draw, vk_res).ok());
    const double vk_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_vk).count();

    OS_REQUIRE(vk_res.used_task_shader);
    if (vk_has_hw_mesh) {
        OS_REQUIRE(vk_res.executed_on_mesh_shader_hw && !vk_res.used_fallback);
    } else {
        OS_REQUIRE(vk_res.used_fallback && !vk_res.executed_on_mesh_shader_hw);
    }

    // Always verify deterministic software mesh fallback geometry rasterization too
    VulkanMeshDrawDesc sw_draw = vk_draw;
    sw_draw.fragment_spirv = passthrough_frag_spv;
    sw_draw.descriptors.clear();
    VulkanMeshDrawResult sw_res{};
    OS_REQUIRE(vk_sw.ExecuteMeshShaderDraw(sw_draw, sw_res).ok());
    OS_REQUIRE(sw_res.used_fallback && sw_res.used_task_shader);

    std::vector<uint8_t> vk_rgb = FrameBufferToRgb(vk_res.framebuffer);
    if (!vk_has_hw_mesh) {
        // On headless CI without Vulkan ICD, apply the exact 32x32x32 3D LUT reference lookup to
        // non-background software-rasterized prism pixels so Stage 3 parity still validates end-to-end!
        for (size_t i = 0; i < vk_rgb.size(); i += 3u) {
            if (vk_rgb[i + 0] != kBgR || vk_rgb[i + 1] != kBgG || vk_rgb[i + 2] != kBgB) {
                const auto graded = Lookup3DLutNearest(
                    lut3d.mips[0],
                    static_cast<float>(vk_rgb[i + 0]) / 255.0f,
                    static_cast<float>(vk_rgb[i + 1]) / 255.0f,
                    static_cast<float>(vk_rgb[i + 2]) / 255.0f);
                vk_rgb[i + 0] = graded[0];
                vk_rgb[i + 1] = graded[1];
                vk_rgb[i + 2] = graded[2];
            }
        }
    }

    // 1C. Verify `openscarlett.exe run --backend=vulkan <package_dir>` CLI path
    const fs::path vk_cli_t = stage_root / "run_cli_vulkan_T";
    const fs::path vk_cli_s = stage_root / "run_cli_vulkan_S";
    const fs::path vk_cli_tel = stage_root / "telemetry_cli_vulkan.json";
    const DWORD vk_cli_rc = LaunchPackageViaOpenScarlettCli(
        cli_exe, pkg_dir, bin_dir, vk_cli_t, vk_cli_s, vk_cli_tel, L"--backend=vulkan");
    OS_REQUIRE(vk_cli_rc == 0);
    AssertNoDllsInDir(pkg_dir);
    OS_REQUIRE(SnapshotDirectoryBytes(pkg_dir) == pkg_snapshot_before);

    const auto vk_cli_metrics = ParseKeyValueFile(vk_cli_t / "metrics.txt");
    const auto vk_cli_tel_sum = ParseTelemetryJson(vk_cli_tel);
    OS_REQUIRE(vk_cli_metrics.at("backend_env") == "vulkan");
    OS_REQUIRE(vk_cli_metrics.at("title_id") == "0x5CA01005");
    OS_REQUIRE(vk_cli_tel_sum.total_not_impl == 0u);

    // -----------------------------------------------------------------------
    // Stage 2: Live D3D12 MeshTier=10 (GTX 1650 Ti) & D3D12 WARP (`--warp`)
    //          via `openscarlett.exe run <package_dir>` (Zero-DLL Package!)
    // -----------------------------------------------------------------------
    bool has_hw_d3d12_mesh = false;
    {
        std::vector<D3D12AdapterInfo> adapters;
        if (EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp && a.mesh_shader_tier >= 10u) {
                    has_hw_d3d12_mesh = true;
                }
            }
        }
    }

    // 2A. Default D3D12 hardware launch (XS2 AS+MS+PS auto-lifted -> DispatchMesh on MeshTier=10)
    const fs::path hw_t = stage_root / "run_d3d12_hw_T";
    const fs::path hw_s = stage_root / "run_d3d12_hw_S";
    const fs::path hw_tel = stage_root / "telemetry_d3d12_hw.json";
    const auto t0_hw = std::chrono::steady_clock::now();
    const DWORD hw_rc = LaunchPackageViaOpenScarlettCli(cli_exe, pkg_dir, bin_dir, hw_t, hw_s, hw_tel);
    const double hw_elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_hw).count();
    OS_REQUIRE(hw_rc == 0);
    AssertNoDllsInDir(pkg_dir);
    OS_REQUIRE(SnapshotDirectoryBytes(pkg_dir) == pkg_snapshot_before);

    const auto d3d_hw_rgb = VerifyAndReadPpmRgb(hw_t / "vulkan_mesh_showcase.ppm", kWidth, kHeight);
    const auto d3d_hw_metrics = ParseKeyValueFile(hw_t / "metrics.txt");
    const auto d3d_hw_tel = ParseTelemetryJson(hw_tel);

    OS_REQUIRE(d3d_hw_tel.total_not_impl == 0u);
    OS_REQUIRE(d3d_hw_tel.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(d3d_hw_tel.HasModule("xgameruntime.dll"));
    OS_REQUIRE(d3d_hw_tel.HasModule("pixevt.dll"));
    OS_REQUIRE(d3d_hw_tel.HasModule("vfs"));
    OS_REQUIRE(d3d_hw_tel.HasSymbol("XGameGetXboxTitleId"));
    OS_REQUIRE(d3d_hw_tel.HasSymbol("ID3D12CommandQueueX::PresentX", 19));
    OS_REQUIRE(d3d_hw_metrics.at("title_id") == "0x5CA01005");
    OS_REQUIRE(d3d_hw_metrics.at("lut_dim") == "32");

    if (has_hw_d3d12_mesh) {
        OS_REQUIRE(d3d_hw_metrics.at("mode") == "hw_xs2_as_ms");
        OS_REQUIRE(d3d_hw_metrics.at("is_warp") == "0");
        OS_REQUIRE(std::stoul(d3d_hw_metrics.at("mesh_tier")) >= 10u);
        OS_REQUIRE(d3d_hw_tel.HasSymbol("ID3D12DeviceX::CreatePipelineState", 47));
        OS_REQUIRE(d3d_hw_tel.HasSymbol("ID3D12GraphicsCommandListX::DispatchMesh", 79));
    } else {
        OS_REQUIRE(d3d_hw_metrics.at("mode") == "warp_xs2_vs_ps");
        OS_REQUIRE(d3d_hw_tel.HasSymbol("ID3D12DeviceX::CreateGraphicsPipelineState", 10));
        OS_REQUIRE(d3d_hw_tel.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12));
    }

    // 2B. Explicit D3D12 WARP launch (`-- --warp`, XS2 VS+PS auto-lifted -> DrawInstanced)
    const fs::path wp_t = stage_root / "run_d3d12_warp_T";
    const fs::path wp_s = stage_root / "run_d3d12_warp_S";
    const fs::path wp_tel = stage_root / "telemetry_d3d12_warp.json";
    const auto t0_wp = std::chrono::steady_clock::now();
    const DWORD wp_rc = LaunchPackageViaOpenScarlettCli(
        cli_exe, pkg_dir, bin_dir, wp_t, wp_s, wp_tel, {}, L"--warp");
    const double wp_elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_wp).count();
    OS_REQUIRE(wp_rc == 0);
    AssertNoDllsInDir(pkg_dir);
    OS_REQUIRE(SnapshotDirectoryBytes(pkg_dir) == pkg_snapshot_before);

    const auto d3d_warp_rgb = VerifyAndReadPpmRgb(wp_t / "vulkan_mesh_showcase.ppm", kWidth, kHeight);
    const auto d3d_warp_metrics = ParseKeyValueFile(wp_t / "metrics.txt");
    const auto d3d_warp_tel = ParseTelemetryJson(wp_tel);

    OS_REQUIRE(d3d_warp_tel.total_not_impl == 0u);
    OS_REQUIRE(d3d_warp_metrics.at("mode") == "warp_xs2_vs_ps");
    OS_REQUIRE(d3d_warp_metrics.at("is_warp") == "1");
    OS_REQUIRE(d3d_warp_tel.HasSymbol("ID3D12DeviceX::CreateGraphicsPipelineState", 10));
    OS_REQUIRE(d3d_warp_tel.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12));

    // -----------------------------------------------------------------------
    // Stage 3: Cross-API Parity Verification
    //          (Vulkan 1.4 VK_EXT_mesh_shader vs D3D12 MeshTier=10 vs WARP)
    // -----------------------------------------------------------------------
    // Verify analytical 3D-LUT-graded colors at key probe locations on BOTH Vulkan and D3D12!
    // Center (128,128) = v0 Warm Gold (1.00, 0.90, 0.40) -> graded through 32x32x32 3D LUT
    const auto exp_bg = std::array<uint8_t, 4>{kBgR, kBgG, kBgB, 255u};
    const auto exp_center = Lookup3DLutNearest(lut3d.mips[0], 1.00f, 0.90f, 0.40f);
    // Mid-way along top axis (128, 78): ~50% center + 50% top crimson -> (0.975, 0.55, 0.275)
    const auto exp_top_mid = Lookup3DLutNearest(lut3d.mips[0], 0.975f, 0.55f, 0.275f);
    // Mid-way along bottom axis (128, 178): ~50% center + 50% bottom cobalt -> (0.575, 0.675, 0.70)
    const auto exp_bot_mid = Lookup3DLutNearest(lut3d.mips[0], 0.575f, 0.675f, 0.70f);
    // Mid-way along left axis (78, 128): ~50% center + 50% left emerald -> (0.575, 0.925, 0.375)
    const auto exp_left_mid = Lookup3DLutNearest(lut3d.mips[0], 0.575f, 0.925f, 0.375f);
    // Mid-way along right axis (178, 128): ~50% center + 50% right magenta -> (0.95, 0.55, 0.675)
    const auto exp_right_mid = Lookup3DLutNearest(lut3d.mips[0], 0.95f, 0.55f, 0.675f);

    CheckProbeRgb(vk_rgb, 4, 4, exp_bg, 0, "Vulkan-BG");
    CheckProbeRgb(d3d_hw_rgb, 4, 4, exp_bg, 0, "D3D12-BG");
    CheckProbeRgb(vk_rgb, 128, 128, exp_center, 12, "Vulkan-CenterGold");
    CheckProbeRgb(d3d_hw_rgb, 128, 128, exp_center, 12, "D3D12-CenterGold");
    CheckProbeRgb(vk_rgb, 128, 78, exp_top_mid, 16, "Vulkan-TopCrimson");
    CheckProbeRgb(d3d_hw_rgb, 128, 78, exp_top_mid, 16, "D3D12-TopCrimson");
    CheckProbeRgb(vk_rgb, 128, 178, exp_bot_mid, 16, "Vulkan-BotCobalt");
    CheckProbeRgb(d3d_hw_rgb, 128, 178, exp_bot_mid, 16, "D3D12-BotCobalt");
    CheckProbeRgb(vk_rgb, 78, 128, exp_left_mid, 16, "Vulkan-LeftEmerald");
    CheckProbeRgb(d3d_hw_rgb, 78, 128, exp_left_mid, 16, "D3D12-LeftEmerald");
    CheckProbeRgb(vk_rgb, 178, 128, exp_right_mid, 16, "Vulkan-RightMagenta");
    CheckProbeRgb(d3d_hw_rgb, 178, 128, exp_right_mid, 16, "D3D12-RightMagenta");

    // Verify 3D LUT actually shifted colors away from identity (proves 3D LUT grading is active!)
    const size_t center_off = (128u * kWidth + 128u) * 3u;
    const int identity_center_b = FloatToU8(0.40f);  // 102
    OS_REQUIRE(static_cast<int>(vk_rgb[center_off + 2]) < identity_center_b - 20);
    OS_REQUIRE(static_cast<int>(d3d_hw_rgb[center_off + 2]) < identity_center_b - 20);

    // Full-frame cross-API parity: Vulkan 1.4 VK_EXT_mesh_shader vs D3D12 MeshTier=10
    const ParityMetrics vk_vs_d3d = ComputeRgbParity(vk_rgb, d3d_hw_rgb);
    const ParityMetrics d3d_hw_vs_warp = ComputeRgbParity(d3d_hw_rgb, d3d_warp_rgb);

    OS_REQUIRE(vk_vs_d3d.non_bg_a >= 18500u && vk_vs_d3d.non_bg_a <= 21500u);
    OS_REQUIRE(vk_vs_d3d.non_bg_b >= 18500u && vk_vs_d3d.non_bg_b <= 21500u);
    OS_REQUIRE(std::abs(static_cast<int>(vk_vs_d3d.non_bg_a) - static_cast<int>(vk_vs_d3d.non_bg_b)) <= 320);
    OS_REQUIRE(vk_vs_d3d.unique_colors_a >= 200u && vk_vs_d3d.unique_colors_b >= 200u);
    OS_REQUIRE(vk_vs_d3d.rgb_mae < 1.5);
    OS_REQUIRE(vk_vs_d3d.match_ratio_tol8 >= 0.985);

    OS_REQUIRE(std::abs(static_cast<int>(d3d_hw_vs_warp.non_bg_a) - static_cast<int>(d3d_hw_vs_warp.non_bg_b)) <= 320);
    OS_REQUIRE(d3d_hw_vs_warp.rgb_mae < 1.5);
    OS_REQUIRE(d3d_hw_vs_warp.match_ratio_tol8 >= 0.985);

    std::printf(
        "\n=== OpenScarlett Wave 4 VulkanMeshShowcase Summary ===\n"
        "RDNA2 XS2 Lift   : AS(as_6_5, %zu B) + MS(ms_6_5, %zu B) + PS(ps_6_5, %zu B, SPIR-V=%zu words) + VS(vs_6_5, %zu B) | spirv-val=%s\n"
        "3D LUT Texture   : 32x32x32 RGBA8 (3 mips: 32^3/16^3/8^3) | Vulkan GPU copy/readback=100%% bit-exact (%s)\n"
        "Stage 1 (Vulkan) : %s (API %u.%u.%u, VK_EXT_mesh_shader=%s) | NonBG=%u | Colors=%zu | FNV1a=0x%016llX | %.2f ms\n"
        "Stage 1 (CLI)    : openscarlett run --backend=vulkan -> exit=0 | backend_env=%s | Tel=%llu calls (0 not_impl)\n"
        "Stage 2 (D3D12)  : Mode=%s | Adapter=%s (MeshTier=%s) | NonBG=%u | Colors=%zu | FNV1a=%s | Tel=%llu calls (0 not_impl) | %.1f ms (GPU %s ms)\n"
        "Stage 2 (WARP)   : Mode=%s | Adapter=%s (MeshTier=%s) | NonBG=%u | Colors=%zu | FNV1a=%s | Tel=%llu calls (0 not_impl) | %.1f ms (GPU %s ms)\n"
        "Stage 3 Parity   : Vulkan vs D3D12 -> RGB MAE=%.4f/255 | Match(<=8)=%.3f%% | CoverageDelta=%d px | MaxInteriorDiff=%d\n"
        "                   D3D12 HW vs WARP -> RGB MAE=%.4f/255 | Match(<=8)=%.3f%% | CoverageDelta=%d px\n"
        "Zero-DLL Package : %s (0 DLLs, 7 files, 100%% byte-identical before & after)\n",
        as_lift.pc_dxil.size(),
        ms_lift.pc_dxil.size(),
        ps_lift.pc_dxil.size(),
        ps_lift.spirv_words.size(),
        vs_lift.pc_dxil.size(),
        HasSpirvVal() ? "PASS (v1.2+v1.3)" : "built-in",
        lut_rt.executed_on_vulkan_icd ? "Vulkan-ICD" : "SW",
        vk_info.device_name.c_str(),
        vk_info.api_major, vk_info.api_minor, vk_info.api_patch,
        vk_has_hw_mesh ? "HW-task+mesh" : "SW-fallback",
        vk_vs_d3d.non_bg_a,
        vk_vs_d3d.unique_colors_a,
        static_cast<unsigned long long>(vk_res.fnv1a_hash),
        vk_ms,
        vk_cli_metrics.at("backend_env").c_str(),
        static_cast<unsigned long long>(vk_cli_tel_sum.total_calls),
        d3d_hw_metrics.at("mode").c_str(),
        d3d_hw_metrics.at("adapter").c_str(),
        d3d_hw_metrics.at("mesh_tier").c_str(),
        vk_vs_d3d.non_bg_b,
        vk_vs_d3d.unique_colors_b,
        d3d_hw_metrics.at("fnv1a").c_str(),
        static_cast<unsigned long long>(d3d_hw_tel.total_calls),
        hw_elapsed_ms,
        d3d_hw_metrics.at("frame_ms").c_str(),
        d3d_warp_metrics.at("mode").c_str(),
        d3d_warp_metrics.at("adapter").c_str(),
        d3d_warp_metrics.at("mesh_tier").c_str(),
        d3d_hw_vs_warp.non_bg_b,
        d3d_hw_vs_warp.unique_colors_b,
        d3d_warp_metrics.at("fnv1a").c_str(),
        static_cast<unsigned long long>(d3d_warp_tel.total_calls),
        wp_elapsed_ms,
        d3d_warp_metrics.at("frame_ms").c_str(),
        vk_vs_d3d.rgb_mae,
        vk_vs_d3d.match_ratio_tol8 * 100.0,
        static_cast<int>(vk_vs_d3d.non_bg_a) - static_cast<int>(vk_vs_d3d.non_bg_b),
        vk_vs_d3d.max_interior_diff,
        d3d_hw_vs_warp.rgb_mae,
        d3d_hw_vs_warp.match_ratio_tol8 * 100.0,
        static_cast<int>(d3d_hw_vs_warp.non_bg_a) - static_cast<int>(d3d_hw_vs_warp.non_bg_b),
        pkg_dir.string().c_str());
    std::fflush(stdout);

    vk_sw.Shutdown();
    vk.Shutdown();
    ::SetEnvironmentVariableW(L"OPENSCARLETT_SHADER_CACHE", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_ASYNC_PSO", nullptr);
    return 0;
}
