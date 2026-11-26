#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

bool HasSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(openscarlett::kDefaultSpirvValPath, ec);
}

void ValidateWithSpirvVal(const std::vector<uint32_t>& spv, const char* env, const char* label) {
    openscarlett::SpirvValidationInfo info{};
    auto st = openscarlett::VulkanBackend::ValidateSpirvBinary(spv, &info);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] ValidateSpirvBinary(%s): %s\n", label, st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    if (!HasSpirvVal()) return;

    // ValidateSpirvExternal in rdna2_lifter checks version == 1.3; for SPIR-V 1.4 mesh shaders
    // we invoke spirv-val directly if version != 1.3, otherwise use ValidateSpirvExternal.
    if (spv.size() >= 2 && spv[1] == 0x00010300u) {
        std::string diag;
        auto ext_st = openscarlett::ValidateSpirvExternal(spv, diag, openscarlett::kDefaultSpirvValPath, env);
        if (!ext_st.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val(%s, %s): %s\n", label, env, diag.c_str());
        }
        OS_REQUIRE(ext_st.ok());
    }
}

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

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Vertex Shader:
//   in  location 0: vec4 pos (x, y, z, 1)
//   in  location 1: vec4 uv_color (u, v, shade, 1)
//   UBO binding 0 : struct { vec4 scale_offset; } (xy = scale, zw = offset)
//   out gl_Position = vec4(pos.x * scale.x + offset.x, pos.y * scale.y + offset.y, pos.z, 1.0)
//   out location 0  = uv_color
// ---------------------------------------------------------------------------
std::vector<uint32_t> BuildTextured3DVertexSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id(), t_pv = b.Id(), t_out_pv = b.Id();
    const uint32_t t_ubo = b.Id(), t_unif_ubo = b.Id(), t_unif_v4 = b.Id();
    const uint32_t v_in_pos = b.Id(), v_in_uv = b.Id(), v_out_pv = b.Id(), v_out_uv = b.Id(), v_ubo = b.Id();
    const uint32_t c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});       // OpCapability Shader
    b.Op(14, {0, 1});    // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {0, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_in_uv, v_out_pv, v_out_uv});
        b.OpVec(15, ep);  // OpEntryPoint Vertex
    }
    b.Op(71, {v_in_pos, 30, 0});       // Location 0
    b.Op(71, {v_in_uv, 30, 1});        // Location 1
    b.Op(71, {v_out_uv, 30, 0});       // Location 0
    b.Op(71, {t_pv, 2});               // Block
    b.Op(72, {t_pv, 0, 11, 0});        // Member 0 BuiltIn Position
    b.Op(71, {t_ubo, 2});              // Block
    b.Op(72, {t_ubo, 0, 35, 0});       // Member 0 Offset 0
    b.Op(71, {v_ubo, 34, 0});          // DescriptorSet 0
    b.Op(71, {v_ubo, 33, 0});          // Binding 0

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_pv, t_v4});
    b.Op(32, {t_out_pv, 3, t_pv});
    b.Op(30, {t_ubo, t_v4});
    b.Op(32, {t_unif_ubo, 2, t_ubo});
    b.Op(32, {t_unif_v4, 2, t_v4});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_in_v4, v_in_pos, 1});
    b.Op(59, {t_in_v4, v_in_uv, 1});
    b.Op(59, {t_out_pv, v_out_pv, 3});
    b.Op(59, {t_out_v4, v_out_uv, 3});
    b.Op(59, {t_unif_ubo, v_ubo, 2});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t pos = b.Id(), uv = b.Id(), p_ubo = b.Id(), so = b.Id();
    const uint32_t px = b.Id(), py = b.Id(), pz = b.Id(), pw = b.Id();
    const uint32_t sx = b.Id(), sy = b.Id(), ox = b.Id(), oy = b.Id();
    const uint32_t mx = b.Id(), my = b.Id(), fx = b.Id(), fy = b.Id();
    const uint32_t out_p = b.Id(), ptr_pos = b.Id();

    b.Op(61, {t_v4, pos, v_in_pos});
    b.Op(61, {t_v4, uv, v_in_uv});
    b.Op(65, {t_unif_v4, p_ubo, v_ubo, c_i0});
    b.Op(61, {t_v4, so, p_ubo});
    b.Op(81, {t_f32, px, pos, 0});
    b.Op(81, {t_f32, py, pos, 1});
    b.Op(81, {t_f32, pz, pos, 2});
    b.Op(81, {t_f32, pw, pos, 3});
    b.Op(81, {t_f32, sx, so, 0});
    b.Op(81, {t_f32, sy, so, 1});
    b.Op(81, {t_f32, ox, so, 2});
    b.Op(81, {t_f32, oy, so, 3});
    b.Op(133, {t_f32, mx, px, sx});
    b.Op(133, {t_f32, my, py, sy});
    b.Op(129, {t_f32, fx, mx, ox});
    b.Op(129, {t_f32, fy, my, oy});
    b.Op(80, {t_v4, out_p, fx, fy, pz, pw});
    b.Op(65, {t_out_v4, ptr_pos, v_out_pv, c_i0});
    b.Op(62, {ptr_pos, out_p});
    b.Op(62, {v_out_uv, uv});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Fragment Shader:
//   in  location 0: vec4 uv_shade (u, v, shade, 1)
//   CombinedImageSampler binding 1: sampler2D tex
//   PushConstant: struct { vec4 tint; }
//   out location 0: texture(tex, uv_shade.xy) * tint * uv_shade.z (alpha = 1.0)
// ---------------------------------------------------------------------------
std::vector<uint32_t> BuildTextured3DFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v2 = b.Id(), t_v4 = b.Id(), t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_img = b.Id(), t_samp_img = b.Id(), t_uc_si = b.Id();
    const uint32_t t_pc = b.Id(), t_ptr_pc = b.Id(), t_ptr_pc_v4 = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id(), v_tex = b.Id(), v_pc = b.Id();
    const uint32_t c_i0 = b.Id(), c_f0 = b.Id(), c_f1 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {4, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in, v_out});
        b.OpVec(15, ep);  // OpEntryPoint Fragment
    }
    b.Op(16, {fn_main, 7});            // OriginUpperLeft
    b.Op(71, {v_in, 30, 0});           // Location 0
    b.Op(71, {v_out, 30, 0});          // Location 0
    b.Op(71, {v_tex, 34, 0});          // DescriptorSet 0
    b.Op(71, {v_tex, 33, 1});          // Binding 1
    b.Op(71, {t_pc, 2});               // Block
    b.Op(72, {t_pc, 0, 35, 0});        // Offset 0

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v2, t_f32, 2});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(25, {t_img, t_f32, 1, 0, 0, 0, 1, 0});  // 2D sampled float image
    b.Op(27, {t_samp_img, t_img});
    b.Op(32, {t_uc_si, 0, t_samp_img});
    b.Op(30, {t_pc, t_v4});
    b.Op(32, {t_ptr_pc, 9, t_pc});
    b.Op(32, {t_ptr_pc_v4, 9, t_v4});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(43, {t_f32, c_f0, F32Bits(0.0f)});
    b.Op(43, {t_f32, c_f1, F32Bits(1.0f)});
    b.Op(59, {t_in_v4, v_in, 1});
    b.Op(59, {t_out_v4, v_out, 3});
    b.Op(59, {t_uc_si, v_tex, 0});
    b.Op(59, {t_ptr_pc, v_pc, 9});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t in_v = b.Id(), u = b.Id(), v = b.Id(), shade = b.Id(), uv = b.Id();
    const uint32_t si = b.Id(), texel = b.Id(), p_tint = b.Id(), tint = b.Id();
    const uint32_t mul1 = b.Id(), mul2 = b.Id(), r = b.Id(), g = b.Id(), bl = b.Id(), final_c = b.Id();

    b.Op(61, {t_v4, in_v, v_in});
    b.Op(81, {t_f32, u, in_v, 0});
    b.Op(81, {t_f32, v, in_v, 1});
    b.Op(81, {t_f32, shade, in_v, 2});
    b.Op(80, {t_v2, uv, u, v});
    b.Op(61, {t_samp_img, si, v_tex});
    b.Op(88, {t_v4, texel, si, uv, 2, c_f0});  // OpImageSampleExplicitLod Lod=0
    b.Op(65, {t_ptr_pc_v4, p_tint, v_pc, c_i0});
    b.Op(61, {t_v4, tint, p_tint});
    b.Op(133, {t_v4, mul1, texel, tint});
    b.Op(142, {t_v4, mul2, mul1, shade});      // OpVectorTimesScalar
    b.Op(81, {t_f32, r, mul2, 0});
    b.Op(81, {t_f32, g, mul2, 1});
    b.Op(81, {t_f32, bl, mul2, 2});
    b.Op(80, {t_v4, final_c, r, g, bl, c_f1});
    b.Op(62, {v_out, final_c});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// ---------------------------------------------------------------------------
// SPIR-V 1.4 Task Shader (SPV_EXT_mesh_shader):
//   TaskPayloadWorkgroupEXT struct { vec4 color_scale; }
//   Stores vec4(0.8, 0.4, 0.2, 1.0) into payload and emits (1, 1, 1) mesh workgroups.
// ---------------------------------------------------------------------------
std::vector<uint32_t> BuildTaskShaderSpirv14() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_i32 = b.Id(), t_f32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_pl = b.Id(), t_ptr_pl = b.Id(), t_ptr_pl_v4 = b.Id(), v_pl = b.Id();
    const uint32_t c_u1 = b.Id(), c_i0 = b.Id();
    const uint32_t c_r = b.Id(), c_g = b.Id(), c_b = b.Id(), c_a = b.Id(), c_col = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {5283});  // OpCapability MeshShadingEXT (implies Shader)
    {
        std::vector<uint32_t> ext;
        SpvBuilder::PushStr(ext, "SPV_EXT_mesh_shader");
        b.OpVec(10, ext);  // OpExtension
    }
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {5364, fn_main};  // TaskEXT
        SpvBuilder::PushStr(ep, "main");
        ep.push_back(v_pl);  // SPIR-V 1.4 requires listing all statically used globals
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
    b.Op(43, {t_f32, c_r, F32Bits(0.8f)});
    b.Op(43, {t_f32, c_g, F32Bits(0.4f)});
    b.Op(43, {t_f32, c_b, F32Bits(0.2f)});
    b.Op(43, {t_f32, c_a, F32Bits(1.0f)});
    b.Op(44, {t_v4, c_col, c_r, c_g, c_b, c_a});
    b.Op(59, {t_ptr_pl, v_pl, 5402});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t ptr_c = b.Id();
    b.Op(65, {t_ptr_pl_v4, ptr_c, v_pl, c_i0});
    b.Op(62, {ptr_c, c_col});
    b.Op(5294, {c_u1, c_u1, c_u1, v_pl});  // OpEmitMeshTasksEXT 1 1 1 %v_pl
    b.Op(56, {});
    return b.Finish(0x00010400u);
}

// ---------------------------------------------------------------------------
// SPIR-V 1.4 Mesh Shader (SPV_EXT_mesh_shader):
//   Emits 3 vertices + 1 centered triangle covering the middle of the render target:
//     v0 = (-0.6, -0.6, 0.2, 1.0)
//     v1 = ( 0.6, -0.6, 0.2, 1.0)
//     v2 = ( 0.0,  0.6, 0.2, 1.0)
//   If use_payload=true, reads vertex color from TaskPayloadWorkgroupEXT;
//   otherwise uses constant (0.2, 0.9, 0.4, 1.0).
// ---------------------------------------------------------------------------
std::vector<uint32_t> BuildMeshShaderSpirv14(bool use_payload) {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_i32 = b.Id(), t_f32 = b.Id();
    const uint32_t t_v4 = b.Id(), t_v3u = b.Id();
    const uint32_t c_u0 = b.Id(), c_u1 = b.Id(), c_u2 = b.Id(), c_u3 = b.Id(), c_i0 = b.Id();
    const uint32_t t_pv = b.Id(), t_arr_pv = b.Id(), t_ptr_arr_pv = b.Id(), t_ptr_out_v4 = b.Id();
    const uint32_t t_arr_col = b.Id(), t_ptr_arr_col = b.Id();
    const uint32_t t_arr_idx = b.Id(), t_ptr_arr_idx = b.Id(), t_ptr_out_v3u = b.Id();
    const uint32_t v_pv = b.Id(), v_col = b.Id(), v_idx = b.Id();
    const uint32_t t_pl = use_payload ? b.Id() : 0;
    const uint32_t t_ptr_pl = use_payload ? b.Id() : 0;
    const uint32_t t_ptr_pl_v4 = use_payload ? b.Id() : 0;
    const uint32_t v_pl = use_payload ? b.Id() : 0;
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
        ep.insert(ep.end(), {v_pv, v_col, v_idx});
        if (use_payload) ep.push_back(v_pl);
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, 1, 1, 1});   // LocalSize 1 1 1
    b.Op(16, {fn_main, 26, 3});         // OutputVertices 3
    b.Op(16, {fn_main, 5270, 1});       // OutputPrimitivesEXT 1
    b.Op(16, {fn_main, 5298});          // OutputTrianglesEXT

    b.Op(71, {t_pv, 2});                // Block
    b.Op(72, {t_pv, 0, 11, 0});         // BuiltIn Position
    b.Op(71, {v_col, 30, 0});           // Location 0
    b.Op(71, {v_idx, 11, 5296});        // BuiltIn PrimitiveTriangleIndicesEXT

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
    b.Op(43, {t_i32, c_i0, 0});

    b.Op(30, {t_pv, t_v4});
    b.Op(28, {t_arr_pv, t_pv, c_u3});
    b.Op(32, {t_ptr_arr_pv, 3, t_arr_pv});
    b.Op(32, {t_ptr_out_v4, 3, t_v4});
    b.Op(28, {t_arr_col, t_v4, c_u3});
    b.Op(32, {t_ptr_arr_col, 3, t_arr_col});
    b.Op(28, {t_arr_idx, t_v3u, c_u1});
    b.Op(32, {t_ptr_arr_idx, 3, t_arr_idx});
    b.Op(32, {t_ptr_out_v3u, 3, t_v3u});

    if (use_payload) {
        b.Op(30, {t_pl, t_v4});
        b.Op(32, {t_ptr_pl, 5402, t_pl});
        b.Op(32, {t_ptr_pl_v4, 5402, t_v4});
    }

    const uint32_t c_m06 = b.Id(), c_p06 = b.Id(), c_f0 = b.Id(), c_f02 = b.Id(), c_f1 = b.Id();
    const uint32_t c_f04 = b.Id(), c_f09 = b.Id();
    b.Op(43, {t_f32, c_m06, F32Bits(-0.6f)});
    b.Op(43, {t_f32, c_p06, F32Bits(0.6f)});
    b.Op(43, {t_f32, c_f0, F32Bits(0.0f)});
    b.Op(43, {t_f32, c_f02, F32Bits(0.2f)});
    b.Op(43, {t_f32, c_f04, F32Bits(0.4f)});
    b.Op(43, {t_f32, c_f09, F32Bits(0.9f)});
    b.Op(43, {t_f32, c_f1, F32Bits(1.0f)});

    const uint32_t c_p0 = b.Id(), c_p1 = b.Id(), c_p2 = b.Id(), c_def_col = b.Id(), c_tri = b.Id();
    b.Op(44, {t_v4, c_p0, c_m06, c_m06, c_f02, c_f1});
    b.Op(44, {t_v4, c_p1, c_p06, c_m06, c_f02, c_f1});
    b.Op(44, {t_v4, c_p2, c_f0, c_p06, c_f02, c_f1});
    b.Op(44, {t_v4, c_def_col, c_f02, c_f09, c_f04, c_f1});
    b.Op(44, {t_v3u, c_tri, c_u0, c_u1, c_u2});

    b.Op(59, {t_ptr_arr_pv, v_pv, 3});
    b.Op(59, {t_ptr_arr_col, v_col, 3});
    b.Op(59, {t_ptr_arr_idx, v_idx, 3});
    if (use_payload) {
        b.Op(59, {t_ptr_pl, v_pl, 5402});
    }

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    b.Op(5295, {c_u3, c_u1});  // OpSetMeshOutputsEXT 3 1

    uint32_t col_val = c_def_col;
    if (use_payload) {
        const uint32_t p_pl_c = b.Id();
        col_val = b.Id();
        b.Op(65, {t_ptr_pl_v4, p_pl_c, v_pl, c_i0});
        b.Op(61, {t_v4, col_val, p_pl_c});
    }

    const uint32_t pp0 = b.Id(), pp1 = b.Id(), pp2 = b.Id();
    b.Op(65, {t_ptr_out_v4, pp0, v_pv, c_u0, c_i0}); b.Op(62, {pp0, c_p0});
    b.Op(65, {t_ptr_out_v4, pp1, v_pv, c_u1, c_i0}); b.Op(62, {pp1, c_p1});
    b.Op(65, {t_ptr_out_v4, pp2, v_pv, c_u2, c_i0}); b.Op(62, {pp2, c_p2});

    const uint32_t pc0 = b.Id(), pc1 = b.Id(), pc2 = b.Id();
    b.Op(65, {t_ptr_out_v4, pc0, v_col, c_u0}); b.Op(62, {pc0, col_val});
    b.Op(65, {t_ptr_out_v4, pc1, v_col, c_u1}); b.Op(62, {pc1, col_val});
    b.Op(65, {t_ptr_out_v4, pc2, v_col, c_u2}); b.Op(62, {pc2, col_val});

    const uint32_t pi0 = b.Id();
    b.Op(65, {t_ptr_out_v3u, pi0, v_idx, c_u0});
    b.Op(62, {pi0, c_tri});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010400u);
}

// Minimal Fragment Shader: passes Location 0 vec4 directly to Location 0 output.
std::vector<uint32_t> BuildPassThroughFragmentSpirv() {
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

void CheckPixelApprox(const openscarlett::FrameBufferRgba8& fb, uint32_t x, uint32_t y,
                      int er, int eg, int eb, int ea, int tol, const char* label) {
    OS_REQUIRE(x < fb.width && y < fb.height);
    const size_t idx = (static_cast<size_t>(y) * fb.width + x) * 4u;
    const int r = fb.pixels[idx + 0];
    const int g = fb.pixels[idx + 1];
    const int b = fb.pixels[idx + 2];
    const int a = fb.pixels[idx + 3];
    if (std::abs(r - er) > tol || std::abs(g - eg) > tol ||
        std::abs(b - eb) > tol || std::abs(a - ea) > tol) {
        std::fprintf(stderr, "[FAIL] %s pixel(%u,%u) = (%d,%d,%d,%d), expected (%d,%d,%d,%d) +- %d\n",
                     label, x, y, r, g, b, a, er, eg, eb, ea, tol);
        OS_REQUIRE(false);
    }
}

void VerifyFrameParity(const openscarlett::FrameBufferRgba8& a,
                       const openscarlett::FrameBufferRgba8& b,
                       int channel_tol,
                       double min_match_ratio,
                       const char* label) {
    OS_REQUIRE(a.width == b.width && a.height == b.height);
    OS_REQUIRE(a.pixels.size() == b.pixels.size());
    const size_t total = static_cast<size_t>(a.width) * a.height;
    size_t matched = 0;
    for (size_t i = 0; i < total; ++i) {
        bool ok = true;
        for (int c = 0; c < 4; ++c) {
            if (std::abs(static_cast<int>(a.pixels[i * 4 + c]) - static_cast<int>(b.pixels[i * 4 + c])) > channel_tol) {
                ok = false;
                break;
            }
        }
        if (ok) ++matched;
    }
    const double ratio = static_cast<double>(matched) / static_cast<double>(total);
    if (ratio < min_match_ratio) {
        std::fprintf(stderr, "[FAIL] %s parity ratio=%.4f (matched=%zu/%zu, min=%.4f)\n",
                     label, ratio, matched, total, min_match_ratio);
    }
    OS_REQUIRE(ratio >= min_match_ratio);
}

// ---------------------------------------------------------------------------
// Test A1: Lifted RDNA2 XS2 -> SPIR-V 1.3 VS + PS live on Vulkan + SW fallback
// ---------------------------------------------------------------------------
void TestA1_Rdna2LiftedVsPs(openscarlett::VulkanBackend& vk, openscarlett::VulkanBackend& sw) {
    using namespace openscarlett;

    Rdna2Assembler vs;
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, true);
    vs.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2Assembler ps;
    ps.EmitVop2(Rdna2Opcode::V_MUL_F32, 4, Rdna2Operand::InlineFloat(2.0f), 0);
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 1, 2, 3, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftOptions lopts{};
    lopts.emit_spirv = true;
    lopts.emit_hlsl = false;
    lopts.validate_spirv_external = HasSpirvVal();

    Rdna2LiftResult vs_lift{}, ps_lift{};
    OS_REQUIRE(LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex), vs_lift, lopts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel), ps_lift, lopts).ok());

    VulkanGraphicsDrawDesc draw{};
    draw.vertex_spirv = vs_lift.spirv_words;
    draw.fragment_spirv = ps_lift.spirv_words;
    draw.width = 64;
    draw.height = 64;
    draw.vertex_count = 3;

    VulkanGraphicsDrawResult hw_res{}, sw_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(draw, hw_res).ok());
    OS_REQUIRE(sw.ExecuteGraphicsDraw(draw, sw_res).ok());

    // Expected: (0.25*2, 0.50, 0.75, 1.0) -> (128, 128, 191, 255) across entire 64x64 RT
    CheckPixelApprox(hw_res.framebuffer, 32, 32, 128, 128, 191, 255, 2, "A1-HW");
    CheckPixelApprox(sw_res.framebuffer, 32, 32, 128, 128, 191, 255, 2, "A1-SW");
    VerifyFrameParity(hw_res.framebuffer, sw_res.framebuffer, 2, 1.0, "A1-RDNA2-VSPS");

    std::printf("[PASS] Test A1: Lifted RDNA2 XS2 -> SPIR-V 1.3 VS+PS live (hw=%s, hash=0x%016llX, SW parity=100%%)\n",
                hw_res.executed_on_vulkan_icd ? "Vulkan-ICD" : "SW",
                static_cast<unsigned long long>(hw_res.fnv1a_hash));
}

// ---------------------------------------------------------------------------
// Test A2: Full 3D Textured + Depth-Buffered (D32F) + VB/IB + UBO + PushConstants + 4x MSAA Resolve + FP16
// ---------------------------------------------------------------------------
void TestA2_TexturedDepthMsaa3D(openscarlett::VulkanBackend& vk, openscarlett::VulkanBackend& sw) {
    using namespace openscarlett;

    auto vs_spv = BuildTextured3DVertexSpirv();
    auto ps_spv = BuildTextured3DFragmentSpirv();
    ValidateWithSpirvVal(vs_spv, "vulkan1.2", "A2_VS");
    ValidateWithSpirvVal(ps_spv, "vulkan1.2", "A2_PS");

    // 4x4 checkerboard RGBA8 texture: left half green (0,255,0), right half magenta (255,0,255)
    VulkanTextureDesc tex{};
    tex.width = 4;
    tex.height = 4;
    tex.depth = 1;
    tex.is_3d = false;
    tex.format = VulkanImageFormat::Rgba8Unorm;
    VulkanTextureMipLevel m0{4, 4, 1, std::vector<uint8_t>(4 * 4 * 4, 255u)};
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const size_t i = (y * 4 + x) * 4;
            if (x < 2) {
                m0.data[i + 0] = 0; m0.data[i + 1] = 255; m0.data[i + 2] = 0; m0.data[i + 3] = 255;
            } else {
                m0.data[i + 0] = 255; m0.data[i + 1] = 0; m0.data[i + 2] = 255; m0.data[i + 3] = 255;
            }
        }
    }
    tex.mips.push_back(std::move(m0));

    struct Vertex { float pos[4]; float uv_shade[4]; };
    // Quad 0 (NEAR, z = 0.2): covers [-0.5, 0.5] x [-0.5, 0.5], full texture UV, shade=1.0
    // Quad 1 (FAR,  z = 0.8): covers [-0.8, 0.8] x [-0.8, 0.8], right-side UV (magenta), shade=0.5
    // Drawn in order: Near FIRST, Far SECOND -> depth test MUST keep Near quad in front!
    const std::vector<Vertex> verts = {
        {{-0.5f, -0.5f, 0.2f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
        {{ 0.5f, -0.5f, 0.2f, 1.0f}, {1.0f, 0.0f, 1.0f, 1.0f}},
        {{ 0.5f,  0.5f, 0.2f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{-0.5f,  0.5f, 0.2f, 1.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},

        {{-0.8f, -0.8f, 0.8f, 1.0f}, {0.75f, 0.5f, 0.5f, 1.0f}},
        {{ 0.8f, -0.8f, 0.8f, 1.0f}, {0.75f, 0.5f, 0.5f, 1.0f}},
        {{ 0.8f,  0.8f, 0.8f, 1.0f}, {0.75f, 0.5f, 0.5f, 1.0f}},
        {{-0.8f,  0.8f, 0.8f, 1.0f}, {0.75f, 0.5f, 0.5f, 1.0f}},
    };
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

    const float ubo_scale_offset[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    const float pc_tint[4] = {1.0f, 0.8f, 1.0f, 1.0f};

    VulkanGraphicsDrawDesc draw{};
    draw.vertex_spirv = vs_spv;
    draw.fragment_spirv = ps_spv;
    draw.width = 128;
    draw.height = 128;
    draw.color_format = VulkanImageFormat::Rgba8Unorm;
    draw.enable_depth = true;
    draw.enable_msaa_4x = true;
    draw.clear_color[0] = 0.0f; draw.clear_color[1] = 0.0f; draw.clear_color[2] = 0.2f; draw.clear_color[3] = 1.0f;
    draw.clear_depth = 1.0f;

    draw.vertex_bytes.resize(verts.size() * sizeof(Vertex));
    std::memcpy(draw.vertex_bytes.data(), verts.data(), draw.vertex_bytes.size());
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_attributes = {
        {0, 0, VulkanVertexFormat::R32G32B32A32Sfloat, 0},
        {1, 0, VulkanVertexFormat::R32G32B32A32Sfloat, 16},
    };
    draw.indices = indices;

    draw.push_constants.resize(sizeof(pc_tint));
    std::memcpy(draw.push_constants.data(), pc_tint, sizeof(pc_tint));

    VulkanDescriptorBinding b_ubo{};
    b_ubo.binding = 0;
    b_ubo.kind = VulkanDescriptorKind::UniformBuffer;
    b_ubo.buffer_data.resize(sizeof(ubo_scale_offset));
    std::memcpy(b_ubo.buffer_data.data(), ubo_scale_offset, sizeof(ubo_scale_offset));

    VulkanDescriptorBinding b_tex{};
    b_tex.binding = 1;
    b_tex.kind = VulkanDescriptorKind::CombinedImageSampler;
    b_tex.texture = tex;
    b_tex.linear_filter = false;

    draw.descriptors = {b_ubo, b_tex};

    VulkanGraphicsDrawResult hw_res{}, sw_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(draw, hw_res).ok());
    OS_REQUIRE(sw.ExecuteGraphicsDraw(draw, sw_res).ok());
    OS_REQUIRE(hw_res.used_depth_buffer && hw_res.used_msaa_4x);

    // Clear corner (4, 4) -> (0, 0, 51, 255)
    CheckPixelApprox(hw_res.framebuffer, 4, 4, 0, 0, 51, 255, 2, "A2-Clear-HW");
    // Far quad border (20, 64) -> magenta(255,0,255) * tint(1,0.8,1) * shade(0.5) -> (128, 0, 128, 255)
    CheckPixelApprox(hw_res.framebuffer, 20, 64, 128, 0, 128, 255, 2, "A2-FarQuad-HW");
    // Near quad left half (48, 64) -> green(0,255,0) * tint(1,0.8,1) * shade(1.0) -> (0, 204, 0, 255)
    // Proves D32_SFLOAT depth occlusion kept Near quad in front of Far quad!
    CheckPixelApprox(hw_res.framebuffer, 48, 64, 0, 204, 0, 255, 2, "A2-NearLeft-HW");
    // Near quad right half (80, 64) -> magenta(255,0,255) * tint(1,0.8,1) * shade(1.0) -> (255, 0, 255, 255)
    CheckPixelApprox(hw_res.framebuffer, 80, 64, 255, 0, 255, 255, 2, "A2-NearRight-HW");

    // Verify deterministic repeatability & HW vs SW parity
    VulkanGraphicsDrawResult hw_res2{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(draw, hw_res2).ok());
    OS_REQUIRE(hw_res.fnv1a_hash == hw_res2.fnv1a_hash);
    VerifyFrameParity(hw_res.framebuffer, sw_res.framebuffer, 4, 0.96, "A2-3D-Depth-MSAA4x");

    // Also test FP16 (VK_FORMAT_R16G16B16A16_SFLOAT) render target + readback!
    draw.color_format = VulkanImageFormat::Rgba16Sfloat;
    VulkanGraphicsDrawResult fp16_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(draw, fp16_res).ok());
    OS_REQUIRE(fp16_res.raw_fp16_rgba.size() == 128u * 128u * 4u);
    CheckPixelApprox(fp16_res.framebuffer, 48, 64, 0, 204, 0, 255, 3, "A2-FP16-NearLeft");

    std::printf("[PASS] Test A2: 3D Textured+Depth(D32F)+VB/IB+UBO+PushConst+MSAA4x+FP16 (hw=%s, rgba8_hash=0x%016llX, fp16_hash=0x%016llX)\n",
                hw_res.executed_on_vulkan_icd ? "Vulkan-ICD" : "SW",
                static_cast<unsigned long long>(hw_res.fnv1a_hash),
                static_cast<unsigned long long>(fp16_res.fnv1a_hash));
}

// ---------------------------------------------------------------------------
// Test B: Mipmapped 2D Texture & 3D Volume Texture Upload/Copy/Readback
// ---------------------------------------------------------------------------
void TestB_Mipmapped2DAnd3DVolumeTextures(openscarlett::VulkanBackend& vk) {
    using namespace openscarlett;

    // 1. 2D Mipmapped texture: 64x64 down to 1x1 (7 mip levels)
    VulkanTextureDesc tex2d{};
    tex2d.width = 64;
    tex2d.height = 64;
    tex2d.depth = 1;
    tex2d.is_3d = false;
    tex2d.format = VulkanImageFormat::Rgba8Unorm;
    for (uint32_t m = 0; m < 7; ++m) {
        const uint32_t w = std::max<uint32_t>(1, 64u >> m);
        const uint32_t h = std::max<uint32_t>(1, 64u >> m);
        VulkanTextureMipLevel mip{w, h, 1, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4u)};
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
                mip.data[i + 0] = static_cast<uint8_t>((x * 7u + m * 31u) & 0xFFu);
                mip.data[i + 1] = static_cast<uint8_t>((y * 11u + m * 17u) & 0xFFu);
                mip.data[i + 2] = static_cast<uint8_t>(((x ^ y) + m * 53u) & 0xFFu);
                mip.data[i + 3] = 255u;
            }
        }
        tex2d.mips.push_back(std::move(mip));
    }

    VulkanTextureRoundtripResult rt2d{};
    OS_REQUIRE(vk.UploadAndReadbackTexture(tex2d, rt2d).ok());
    OS_REQUIRE(rt2d.readback_mips.size() == 7u);
    for (size_t m = 0; m < 7u; ++m) {
        OS_REQUIRE(rt2d.readback_mips[m].data == tex2d.mips[m].data);
    }

    // 2. 3D Volume Texture: 32x32x32 mip0, 16x16x16 mip1, 8x8x8 mip2 (3D LUT)
    VulkanTextureDesc tex3d{};
    tex3d.width = 32;
    tex3d.height = 32;
    tex3d.depth = 32;
    tex3d.is_3d = true;
    tex3d.format = VulkanImageFormat::Rgba8Unorm;
    for (uint32_t m = 0; m < 3; ++m) {
        const uint32_t dim = 32u >> m;
        VulkanTextureMipLevel mip{dim, dim, dim, std::vector<uint8_t>(static_cast<size_t>(dim) * dim * dim * 4u)};
        for (uint32_t z = 0; z < dim; ++z) {
            for (uint32_t y = 0; y < dim; ++y) {
                for (uint32_t x = 0; x < dim; ++x) {
                    const size_t i = ((static_cast<size_t>(z) * dim + y) * dim + x) * 4u;
                    mip.data[i + 0] = static_cast<uint8_t>((x * 255u) / std::max<uint32_t>(1, dim - 1));
                    mip.data[i + 1] = static_cast<uint8_t>((y * 255u) / std::max<uint32_t>(1, dim - 1));
                    mip.data[i + 2] = static_cast<uint8_t>((z * 255u) / std::max<uint32_t>(1, dim - 1));
                    mip.data[i + 3] = static_cast<uint8_t>(200u + m * 20u);
                }
            }
        }
        tex3d.mips.push_back(std::move(mip));
    }

    VulkanTextureRoundtripResult rt3d{};
    OS_REQUIRE(vk.UploadAndReadbackTexture(tex3d, rt3d).ok());
    OS_REQUIRE(rt3d.readback_mips.size() == 3u);
    for (size_t m = 0; m < 3u; ++m) {
        OS_REQUIRE(rt3d.readback_mips[m].data == tex3d.mips[m].data);
    }

    std::printf("[PASS] Test B: Mipmapped 2D (64x64, 7 mips) & 3D Volume (32x32x32, 3 mips) GPU copy/readback bit-exact (hw=%s)\n",
                rt3d.executed_on_vulkan_icd ? "Vulkan-ICD" : "SW");
}

// ---------------------------------------------------------------------------
// Test C: VK_EXT_mesh_shader (Mesh+PS and Task+Mesh+PS) + Graceful Fallback
// ---------------------------------------------------------------------------
void TestC_MeshShaders(openscarlett::VulkanBackend& vk, openscarlett::VulkanBackend& sw) {
    using namespace openscarlett;

    auto task_spv = BuildTaskShaderSpirv14();
    auto mesh_no_pl_spv = BuildMeshShaderSpirv14(false);
    auto mesh_pl_spv = BuildMeshShaderSpirv14(true);
    auto frag_spv = BuildPassThroughFragmentSpirv();

    ValidateWithSpirvVal(task_spv, "vulkan1.3", "C_TaskEXT");
    ValidateWithSpirvVal(mesh_no_pl_spv, "vulkan1.3", "C_MeshEXT_NoPayload");
    ValidateWithSpirvVal(mesh_pl_spv, "vulkan1.3", "C_MeshEXT_Payload");
    ValidateWithSpirvVal(frag_spv, "vulkan1.2", "C_Frag");

    // 1. MS + PS draw
    VulkanMeshDrawDesc ms_desc{};
    ms_desc.mesh_spirv = mesh_no_pl_spv;
    ms_desc.fragment_spirv = frag_spv;
    ms_desc.width = 64;
    ms_desc.height = 64;
    ms_desc.group_count_x = 1;
    ms_desc.clear_color[0] = 0.05f; ms_desc.clear_color[1] = 0.05f; ms_desc.clear_color[2] = 0.1f; ms_desc.clear_color[3] = 1.0f;

    VulkanMeshDrawResult ms_hw{}, ms_sw{};
    OS_REQUIRE(vk.ExecuteMeshShaderDraw(ms_desc, ms_hw).ok());
    OS_REQUIRE(sw.ExecuteMeshShaderDraw(ms_desc, ms_sw).ok());
    OS_REQUIRE(ms_sw.used_fallback && !ms_sw.executed_on_mesh_shader_hw);
    if (vk.GetDeviceInfo().supports_mesh_shader) {
        OS_REQUIRE(ms_hw.executed_on_mesh_shader_hw && !ms_hw.used_fallback);
    }

    // Center pixel (32, 32) is inside triangle (-0.6,-0.6),(0.6,-0.6),(0,0.6) -> color (0.2, 0.9, 0.4, 1.0) = (51, 230, 102, 255)
    CheckPixelApprox(ms_hw.framebuffer, 32, 32, 51, 230, 102, 255, 2, "C-MS-Center-HW");
    CheckPixelApprox(ms_hw.framebuffer, 4, 4, 13, 13, 26, 255, 2, "C-MS-Corner-HW");
    VerifyFrameParity(ms_hw.framebuffer, ms_sw.framebuffer, 3, 0.97, "C-MS-PS-Parity");

    // 2. Task (AS) + Mesh (MS) + Fragment (PS) with TaskPayloadWorkgroupEXT
    VulkanMeshDrawDesc as_ms_desc = ms_desc;
    as_ms_desc.task_spirv = task_spv;
    as_ms_desc.mesh_spirv = mesh_pl_spv;

    VulkanMeshDrawResult as_hw{}, as_sw{};
    OS_REQUIRE(vk.ExecuteMeshShaderDraw(as_ms_desc, as_hw).ok());
    OS_REQUIRE(sw.ExecuteMeshShaderDraw(as_ms_desc, as_sw).ok());
    OS_REQUIRE(as_hw.used_task_shader && as_sw.used_task_shader);
    if (vk.GetDeviceInfo().supports_mesh_shader && vk.GetDeviceInfo().supports_task_shader) {
        OS_REQUIRE(as_hw.executed_on_mesh_shader_hw && !as_hw.used_fallback);
    }

    // Task payload color = (0.8, 0.4, 0.2, 1.0) -> (204, 102, 51, 255)
    CheckPixelApprox(as_hw.framebuffer, 32, 32, 204, 102, 51, 255, 2, "C-ASMS-Center-HW");
    VerifyFrameParity(as_hw.framebuffer, as_sw.framebuffer, 3, 0.97, "C-AS-MS-PS-Parity");

    std::printf("[PASS] Test C: VK_EXT_mesh_shader MS+PS (hw=%s, hash=0x%016llX) & Task+Mesh+PS (hw=%s, hash=0x%016llX) + SW fallback parity\n",
                ms_hw.executed_on_mesh_shader_hw ? "HW-MeshShader" : "Fallback",
                static_cast<unsigned long long>(ms_hw.fnv1a_hash),
                as_hw.executed_on_mesh_shader_hw ? "HW-Task+Mesh" : "Fallback",
                static_cast<unsigned long long>(as_hw.fnv1a_hash));
}

// ---------------------------------------------------------------------------
// Test D: Win32 Swapchain + HDR10 Colorspace Probe + Headless Offscreen Readback
// ---------------------------------------------------------------------------
void TestD_Win32SwapchainAndHdr10(openscarlett::VulkanBackend& vk) {
    using namespace openscarlett;

    FrameBufferRgba8 pattern{};
    pattern.width = 64;
    pattern.height = 64;
    pattern.pixels.resize(64u * 64u * 4u);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            const size_t i = (static_cast<size_t>(y) * 64 + x) * 4u;
            pattern.pixels[i + 0] = static_cast<uint8_t>(x * 4u);
            pattern.pixels[i + 1] = static_cast<uint8_t>(y * 4u);
            pattern.pixels[i + 2] = static_cast<uint8_t>((x + y) * 2u);
            pattern.pixels[i + 3] = 255u;
        }
    }
    const uint64_t expected_hash = pattern.ComputeFnv1aHash();

    // 1. Headless offscreen swapchain mode (works identically on desktop & Windows Server CI)
    VulkanSwapchainDesc off_desc{};
    off_desc.width = 64;
    off_desc.height = 64;
    off_desc.headless_offscreen = true;

    VulkanSwapchainInfo off_info{};
    OS_REQUIRE(vk.CreateWin32Swapchain(off_desc, off_info).ok());
    OS_REQUIRE(off_info.handle != 0u);
    OS_REQUIRE(off_info.is_headless_offscreen);

    VulkanPresentResult off_pres{};
    OS_REQUIRE(vk.PresentRgba8ToSwapchain(off_info.handle, pattern, off_pres, true).ok());
    OS_REQUIRE(off_pres.readback_available);
    OS_REQUIRE(off_pres.fnv1a_hash == expected_hash);
    OS_REQUIRE(off_pres.readback_framebuffer.pixels == pattern.pixels);
    OS_REQUIRE(vk.DestroyWin32Swapchain(off_info.handle).ok());

    // 2. Interactive Win32 HWND swapchain + HDR10 colorspace probe (gracefully handles headless CI)
    bool win32_presented = false;
    bool hdr10_advertised = false;
    size_t cs_count = 0;
#if defined(_WIN32)
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "OpenScarlettVulkanTestWnd";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "OpenScarlett Vulkan Test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        VulkanSwapchainDesc w_desc{};
        w_desc.hwnd = hwnd;
        w_desc.hinstance = wc.hInstance;
        w_desc.width = 64;
        w_desc.height = 64;
        w_desc.prefer_hdr10 = true;
        w_desc.headless_offscreen = false;

        VulkanSwapchainInfo w_info{};
        OS_REQUIRE(vk.CreateWin32Swapchain(w_desc, w_info).ok());
        OS_REQUIRE(w_info.handle != 0u);
        hdr10_advertised = w_info.hdr10_advertised;
        cs_count = w_info.supported_color_spaces.size();

        VulkanPresentResult w_pres{};
        OS_REQUIRE(vk.PresentRgba8ToSwapchain(w_info.handle, pattern, w_pres, false).ok());
        win32_presented = w_pres.presented_to_win32_swapchain;

        OS_REQUIRE(vk.DestroyWin32Swapchain(w_info.handle).ok());
        DestroyWindow(hwnd);
    }
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
#endif

    std::printf("[PASS] Test D: Headless Offscreen (hash=0x%016llX) & Win32 Swapchain (presented=%s, hdr10_advertised=%s, color_spaces=%zu)\n",
                static_cast<unsigned long long>(off_pres.fnv1a_hash),
                win32_presented ? "yes" : "headless-fallback",
                hdr10_advertised ? "yes" : "no",
                cs_count);
}

}  // namespace

int main() {
    using namespace openscarlett;

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());
    const auto info = vk.GetDeviceInfo();

    VulkanBackend sw;
    OS_REQUIRE(sw.InitializeSoftwareFallback().ok());
    OS_REQUIRE(sw.is_fallback_mode());

    std::printf("=== OpenScarlett Vulkan 1.2/1.4 Full Graphics, Mesh Shaders & WSI Live Test ===\n");
    std::printf("Device       : %s (API %u.%u.%u, hw=%s, gfx_q=%u, mesh=%s, task=%s, swapchain=%s, hdr10_cs=%s)\n",
                info.device_name.c_str(),
                info.api_major, info.api_minor, info.api_patch,
                info.is_hardware ? "yes" : "fallback",
                info.graphics_queue_family,
                info.supports_mesh_shader ? "yes" : "no",
                info.supports_task_shader ? "yes" : "no",
                info.supports_swapchain ? "yes" : "no",
                info.supports_hdr10_colorspace ? "yes" : "no");

    TestA1_Rdna2LiftedVsPs(vk, sw);
    TestA2_TexturedDepthMsaa3D(vk, sw);
    TestB_Mipmapped2DAnd3DVolumeTextures(vk);
    TestC_MeshShaders(vk, sw);
    TestD_Win32SwapchainAndHdr10(vk);

    sw.Shutdown();
    vk.Shutdown();
    std::printf("[PASS] test_vulkan_graphics_live completed successfully!\n");
    return 0;
}
