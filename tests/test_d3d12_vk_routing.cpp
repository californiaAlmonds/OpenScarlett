#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"

namespace {

#if defined(_WIN32)

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

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

private:
    T* ptr_ = nullptr;
};

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

bool EnvFlagEnabled(const char* name) {
    char buf[64]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T';
}

struct ScopedEnvVar {
    std::string name;
    bool had_prev = false;
    std::string prev_val;

    ScopedEnvVar(const char* env_name, const char* new_val) : name(env_name) {
        char buf[512]{};
        const DWORD len = GetEnvironmentVariableA(env_name, buf, static_cast<DWORD>(sizeof(buf)));
        if (len > 0u && len < sizeof(buf)) {
            had_prev = true;
            prev_val.assign(buf, len);
        }
        SetEnvironmentVariableA(env_name, new_val);
    }
    ~ScopedEnvVar() {
        SetEnvironmentVariableA(name.c_str(), had_prev ? prev_val.c_str() : nullptr);
    }
};

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

    std::vector<uint32_t> Finish(uint32_t spv_version = 0x00010300u) const {
        std::vector<uint32_t> out;
        out.reserve(5 + words.size());
        out.insert(out.end(), {0x07230203u, spv_version, 0x4F533700u, bound, 0u});
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

// SPIR-V compute shader: out[gid.x] = in[gid.x] * 3 + 7 (LocalSize = local_x, 1, 1)
std::vector<uint32_t> BuildComputeMulAddSpirv(uint32_t local_x) {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v3u = b.Id(), t_in_v3u = b.Id(), t_in_u32 = b.Id();
    const uint32_t t_rt_u32 = b.Id(), t_buf = b.Id(), t_ptr_buf = b.Id(), t_ptr_u32 = b.Id();
    const uint32_t v_gid = b.Id(), v_in = b.Id(), v_out = b.Id();
    const uint32_t c_u0 = b.Id(), c_i0 = b.Id(), c_3 = b.Id(), c_7 = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});     // OpCapability Shader
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {5, fn_main};  // GLCompute
        SpvBuilder::PushStr(ep, "main");
        ep.push_back(v_gid);
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, local_x, 1, 1});  // LocalSize
    b.Op(71, {v_gid, 11, 28});               // BuiltIn GlobalInvocationId
    b.Op(71, {t_rt_u32, 6, 4});              // ArrayStride 4
    b.Op(71, {t_buf, 3});                    // BufferBlock (SPIR-V 1.0/1.3 compatible)
    b.Op(72, {t_buf, 0, 35, 0});             // MemberOffset 0
    b.Op(71, {v_in, 34, 0});                 // Set 0
    b.Op(71, {v_in, 33, 0});                 // Binding 0
    b.Op(71, {v_out, 34, 0});                // Set 0
    b.Op(71, {v_out, 33, 1});                // Binding 1

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(21, {t_u32, 32, 0});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v3u, t_u32, 3});
    b.Op(32, {t_in_v3u, 1, t_v3u});
    b.Op(32, {t_in_u32, 1, t_u32});
    b.Op(29, {t_rt_u32, t_u32});
    b.Op(30, {t_buf, t_rt_u32});
    b.Op(32, {t_ptr_buf, 2, t_buf});
    b.Op(32, {t_ptr_u32, 2, t_u32});

    b.Op(43, {t_u32, c_u0, 0});
    b.Op(43, {t_i32, c_i0, 0});
    b.Op(43, {t_u32, c_3, 3});
    b.Op(43, {t_u32, c_7, 7});
    b.Op(59, {t_in_v3u, v_gid, 1});
    b.Op(59, {t_ptr_buf, v_in, 2});
    b.Op(59, {t_ptr_buf, v_out, 2});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t p_gx = b.Id(), gx = b.Id(), p_in = b.Id(), val = b.Id();
    const uint32_t mulv = b.Id(), resv = b.Id(), p_out = b.Id();
    b.Op(65, {t_in_u32, p_gx, v_gid, c_u0});
    b.Op(61, {t_u32, gx, p_gx});
    b.Op(65, {t_ptr_u32, p_in, v_in, c_i0, gx});
    b.Op(61, {t_u32, val, p_in});
    b.Op(132, {t_u32, mulv, val, c_3});
    b.Op(128, {t_u32, resv, mulv, c_7});
    b.Op(65, {t_ptr_u32, p_out, v_out, c_i0, gx});
    b.Op(62, {p_out, resv});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// Vertex shader reading loc0=pos (vec4) & loc1=uv/color (vec4), forwarding loc0.
std::vector<uint32_t> BuildPosColorVertexSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id(), t_pv = b.Id(), t_out_pv = b.Id();
    const uint32_t v_in_pos = b.Id(), v_in_col = b.Id(), v_out_pv = b.Id(), v_out_col = b.Id();
    const uint32_t c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {0, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_in_col, v_out_pv, v_out_col});
        b.OpVec(15, ep);
    }
    b.Op(71, {v_in_pos, 30, 0});
    b.Op(71, {v_in_col, 30, 1});
    b.Op(71, {v_out_col, 30, 0});
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
    b.Op(59, {t_in_v4, v_in_col, 1});
    b.Op(59, {t_out_pv, v_out_pv, 3});
    b.Op(59, {t_out_v4, v_out_col, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t pos = b.Id(), col = b.Id(), ptr_pos = b.Id();
    b.Op(61, {t_v4, pos, v_in_pos});
    b.Op(61, {t_v4, col, v_in_col});
    b.Op(65, {t_out_v4, ptr_pos, v_out_pv, c_i0});
    b.Op(62, {ptr_pos, pos});
    b.Op(62, {v_out_col, col});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// Fragment shader outputting loc0 * push_constant.vec4 (16 bytes push constants)
std::vector<uint32_t> BuildPushConstantMulFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_pc = b.Id(), t_ptr_pc = b.Id(), t_ptr_pc_v4 = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id(), v_pc = b.Id();
    const uint32_t c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

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
    b.Op(71, {t_pc, 2});
    b.Op(72, {t_pc, 0, 35, 0});

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_pc, t_v4});
    b.Op(32, {t_ptr_pc, 9, t_pc});
    b.Op(32, {t_ptr_pc_v4, 9, t_v4});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_in_v4, v_in, 1});
    b.Op(59, {t_out_v4, v_out, 3});
    b.Op(59, {t_ptr_pc, v_pc, 9});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t col = b.Id(), p_pc = b.Id(), tint = b.Id(), prod = b.Id();
    b.Op(61, {t_v4, col, v_in});
    b.Op(65, {t_ptr_pc_v4, p_pc, v_pc, c_i0});
    b.Op(61, {t_v4, tint, p_pc});
    b.Op(133, {t_v4, prod, col, tint});
    b.Op(62, {v_out, prod});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// Fragment shader with COLLIDING set=0 binding=0 for BOTH UniformBuffer AND CombinedImageSampler!
// Outputs ubo.tint * texture(tex, vec2(0.5, 0.5)).
std::vector<uint32_t> BuildCollidingUboAndTextureFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v2 = b.Id(), t_v4 = b.Id(), t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_ubo = b.Id(), t_ptr_ubo = b.Id(), t_ptr_ubo_v4 = b.Id();
    const uint32_t t_img = b.Id(), t_samp_img = b.Id(), t_ptr_samp_img = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id(), v_ubo = b.Id(), v_tex = b.Id();
    const uint32_t c_i0 = b.Id(), c_half = b.Id(), c_uv = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

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
    b.Op(71, {t_ubo, 2});
    b.Op(72, {t_ubo, 0, 35, 0});
    // Deliberate mixed-heap slot collision: BOTH v_ubo and v_tex decorated with Set 0, Binding 0!
    b.Op(71, {v_ubo, 34, 0});
    b.Op(71, {v_ubo, 33, 0});
    b.Op(71, {v_tex, 34, 0});
    b.Op(71, {v_tex, 33, 0});

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v2, t_f32, 2});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_ubo, t_v4});
    b.Op(32, {t_ptr_ubo, 2, t_ubo});
    b.Op(32, {t_ptr_ubo_v4, 2, t_v4});
    b.Op(25, {t_img, t_f32, 1, 0, 0, 0, 1, 0});  // 2D sampled float image
    b.Op(27, {t_samp_img, t_img});
    b.Op(32, {t_ptr_samp_img, 0, t_samp_img});   // UniformConstant

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(43, {t_f32, c_half, F32Bits(0.5f)});
    b.Op(44, {t_v2, c_uv, c_half, c_half});
    b.Op(59, {t_in_v4, v_in, 1});
    b.Op(59, {t_out_v4, v_out, 3});
    b.Op(59, {t_ptr_ubo, v_ubo, 2});
    b.Op(59, {t_ptr_samp_img, v_tex, 0});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t p_tint = b.Id(), tint = b.Id(), sampled_img = b.Id(), texel = b.Id(), out_c = b.Id();
    b.Op(65, {t_ptr_ubo_v4, p_tint, v_ubo, c_i0});
    b.Op(61, {t_v4, tint, p_tint});
    b.Op(61, {t_samp_img, sampled_img, v_tex});
    b.Op(87, {t_v4, texel, sampled_img, c_uv});  // OpImageSampleImplicitLod
    b.Op(133, {t_v4, out_c, tint, texel});
    b.Op(62, {v_out, out_c});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// Simple pass-through Fragment shader outputting loc0 directly.
std::vector<uint32_t> BuildPassColorFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

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
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});

    b.Op(59, {t_in_v4, v_in, 1});
    b.Op(59, {t_out_v4, v_out, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t c = b.Id();
    b.Op(61, {t_v4, c, v_in});
    b.Op(62, {v_out, c});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// SPIR-V 1.4 MeshEXT shader emitting 1 green fullscreen-centered triangle (3 verts, 1 prim).
std::vector<uint32_t> BuildSimpleMeshSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_u32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v3u = b.Id(), t_v4 = b.Id();
    const uint32_t t_pv = b.Id(), t_arr_pv = b.Id(), t_out_arr_pv = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_arr_v4 = b.Id(), t_out_arr_v4 = b.Id();
    const uint32_t t_arr_idx = b.Id(), t_out_arr_idx = b.Id(), t_out_v3u = b.Id();
    const uint32_t c_u0 = b.Id(), c_u1 = b.Id(), c_u2 = b.Id(), c_u3 = b.Id(), c_i0 = b.Id();
    const uint32_t c_f0 = b.Id(), c_f1 = b.Id(), c_fn08 = b.Id(), c_fp08 = b.Id();
    const uint32_t c_p0 = b.Id(), c_p1 = b.Id(), c_p2 = b.Id(), c_col = b.Id(), c_tri = b.Id();
    const uint32_t v_pv = b.Id(), v_col = b.Id(), v_idx = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});     // Shader
    b.Op(17, {5283});  // MeshShadingEXT
    {
        std::vector<uint32_t> ext;
        SpvBuilder::PushStr(ext, "SPV_EXT_mesh_shader");
        b.OpVec(10, ext);  // OpExtension
    }
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {5365, fn_main};  // MeshEXT
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_pv, v_col, v_idx});
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, 1, 1, 1});    // LocalSize 1,1,1
    b.Op(16, {fn_main, 26, 3});          // OutputVertices 3
    b.Op(16, {fn_main, 5270, 1});        // OutputPrimitivesEXT 1
    b.Op(16, {fn_main, 5298});           // OutputTrianglesEXT

    b.Op(71, {t_pv, 2});
    b.Op(72, {t_pv, 0, 11, 0});          // BuiltIn Position
    b.Op(71, {v_col, 30, 0});            // Location 0
    b.Op(71, {v_idx, 11, 5294});         // BuiltIn PrimitiveTriangleIndicesEXT

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_u32, 32, 0});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v3u, t_u32, 3});
    b.Op(23, {t_v4, t_f32, 4});

    b.Op(43, {t_u32, c_u0, 0});
    b.Op(43, {t_u32, c_u1, 1});
    b.Op(43, {t_u32, c_u2, 2});
    b.Op(43, {t_u32, c_u3, 3});
    b.Op(43, {t_i32, c_i0, 0});

    b.Op(30, {t_pv, t_v4});
    b.Op(28, {t_arr_pv, t_pv, c_u3});
    b.Op(32, {t_out_arr_pv, 3, t_arr_pv});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(28, {t_arr_v4, t_v4, c_u3});
    b.Op(32, {t_out_arr_v4, 3, t_arr_v4});
    b.Op(28, {t_arr_idx, t_v3u, c_u1});
    b.Op(32, {t_out_arr_idx, 3, t_arr_idx});
    b.Op(32, {t_out_v3u, 3, t_v3u});

    b.Op(43, {t_f32, c_f0, F32Bits(0.0f)});
    b.Op(43, {t_f32, c_f1, F32Bits(1.0f)});
    b.Op(43, {t_f32, c_fn08, F32Bits(-0.8f)});
    b.Op(43, {t_f32, c_fp08, F32Bits(0.8f)});
    b.Op(44, {t_v4, c_p0, c_fn08, c_fn08, c_f0, c_f1});
    b.Op(44, {t_v4, c_p1, c_fp08, c_fn08, c_f0, c_f1});
    b.Op(44, {t_v4, c_p2, c_f0, c_fp08, c_f0, c_f1});
    b.Op(44, {t_v4, c_col, c_f0, c_f1, c_f0, c_f1});  // Pure green
    b.Op(44, {t_v3u, c_tri, c_u0, c_u1, c_u2});

    b.Op(59, {t_out_arr_pv, v_pv, 3});
    b.Op(59, {t_out_arr_v4, v_col, 3});
    b.Op(59, {t_out_arr_idx, v_idx, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    b.Op(5295, {c_u3, c_u1});  // OpSetMeshOutputsEXT(3, 1)
    const uint32_t pp0 = b.Id(), pp1 = b.Id(), pp2 = b.Id();
    const uint32_t pc0 = b.Id(), pc1 = b.Id(), pc2 = b.Id(), pi0 = b.Id();
    b.Op(65, {t_out_v4, pp0, v_pv, c_u0, c_i0});
    b.Op(62, {pp0, c_p0});
    b.Op(65, {t_out_v4, pp1, v_pv, c_u1, c_i0});
    b.Op(62, {pp1, c_p1});
    b.Op(65, {t_out_v4, pp2, v_pv, c_u2, c_i0});
    b.Op(62, {pp2, c_p2});
    b.Op(65, {t_out_v4, pc0, v_col, c_u0});
    b.Op(62, {pc0, c_col});
    b.Op(65, {t_out_v4, pc1, v_col, c_u1});
    b.Op(62, {pc1, c_col});
    b.Op(65, {t_out_v4, pc2, v_col, c_u2});
    b.Op(62, {pc2, c_col});
    b.Op(65, {t_out_v3u, pi0, v_idx, c_u0});
    b.Op(62, {pi0, c_tri});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010400u);
}

bool CompileHlsl(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out_blob) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        src, std::strlen(src), nullptr, nullptr, nullptr,
        entry, target, 0u, 0u, out_blob.Put(), err.Put());
    if (FAILED(hr)) {
        if (err) {
            std::fprintf(stderr, "D3DCompile failed: %.*s\n",
                         static_cast<int>(err->GetBufferSize()),
                         static_cast<const char*>(err->GetBufferPointer()));
        }
        return false;
    }
    return out_blob.Get() != nullptr;
}

double ComputeMaeRgba8(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    OS_REQUIRE(a.size() == b.size() && !a.empty());
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])) / 255.0;
    }
    return sum / static_cast<double>(a.size());
}

// Helper: render a standard HLSL DXBC triangle via d3d12_xs and return pixels + hash + stats + timings.
struct StandardTriangleRun {
    std::vector<uint8_t> pixels;
    uint64_t hash = 0ull;
    D3D12XBOX_VULKAN_ROUTING_STATS vk_stats{};
    D3D12XBOX_PRESENT_TIMING_STATS timing1{};
    D3D12XBOX_PRESENT_TIMING_STATS timing2{};
    D3D12XBOX_PRESENT_TIMING_STATS timing_blank{};
    uint32_t active_backend = 0u;
};

StandardTriangleRun RunStandardD3D12Triangle(bool use_vulkan_env, bool prefer_warp) {
    StandardTriangleRun out{};
    OpenScarlettD3D12SetBackendMode(D3D12XBOX_BACKEND_AUTO);
    ScopedEnvVar backend_env("OPENSCARLETT_BACKEND", use_vulkan_env ? "VULKAN" : "d3d12");
    ScopedEnvVar warp_env("OPENSCARLETT_PREFER_WARP", prefer_warp ? "1" : "0");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1u;

    ComPtr<ID3D12DeviceX> dev;
    OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX,
                                               reinterpret_cast<void**>(dev.Put()))));
    out.active_backend = OpenScarlettD3D12GetActiveBackend(dev.Get());

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueueX,
                                                 reinterpret_cast<void**>(queue.Put()))));

    ComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));
    ComPtr<ID3D12GraphicsCommandListX> cl;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                IID_ID3D12GraphicsCommandListX,
                                                reinterpret_cast<void**>(cl.Put()))));

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs_blob, rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
    ComPtr<ID3D12RootSignature> rs;
    OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                  IID_PPV_ARGS(rs.Put()))));

    const char* hlsl = R"(
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; };
VSOut VSMain(uint vid : SV_VertexID) {
    float2 p[3] = { float2(0.0, 0.75), float2(0.75, -0.75), float2(-0.75, -0.75) };
    float4 c[3] = { float4(1,0,0,1), float4(0,1,0,1), float4(0,0,1,1) };
    VSOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.col = c[vid];
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return i.col; }
)";
    ComPtr<ID3DBlob> vs, ps;
    OS_REQUIRE(CompileHlsl(hlsl, "VSMain", "vs_5_0", vs));
    OS_REQUIRE(CompileHlsl(hlsl, "PSMain", "ps_5_0", ps));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
    psd.pRootSignature = rs.Get();
    psd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psd.SampleMask = 0xFFFFFFFFu;
    psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psd.NumRenderTargets = 1u;
    psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psd.SampleDesc.Count = 1u;

    ComPtr<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(pso.Put()))));

    constexpr uint32_t kW = 64u, kH = 64u;
    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kW;
    rt_desc.Height = kH;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 0.1f; cv.Color[1] = 0.2f; cv.Color[2] = 0.3f; cv.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> rt;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(rt.Put()))));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.NumDescriptors = 1u;
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(rtv_heap.Put()))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_cpu);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
    cl->RSSetViewports(1u, &vp);
    cl->RSSetScissorRects(1u, &sc);
    cl->OMSetRenderTargets(1u, &rtv_cpu, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv_cpu, cv.Color, 0u, nullptr);
    cl->SetGraphicsRootSignature(rs.Get());
    cl->SetPipelineState(pso.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3u, 1u, 0u, 0u);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = rt.Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1u, &bar);
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1u, lists);

    ID3D12Resource* planes[] = {rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1u;
    plane.ppResources = planes;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;
    D3D12XBOX_PRESENT_PARAMETERS pres{};

    OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentTimingStats(queue.Get(), &out.timing1)));

    // Present a 2nd frame to verify monotonic fence value & present counter.
    OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentTimingStats(queue.Get(), &out.timing2)));

    uint32_t w = 0u, h = 0u;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetLastPresentHash(queue.Get(), &out.hash, &w, &h)));
    OS_REQUIRE(w == kW && h == kH);
    out.pixels.resize(static_cast<size_t>(kW) * kH * 4u, 0u);
    uint32_t copied = 0u;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), out.pixels.data(), static_cast<uint32_t>(out.pixels.size()), &copied)));
    OS_REQUIRE(copied == out.pixels.size());

    // Blank present (PlaneCount == 0) timing verification.
    OS_REQUIRE(SUCCEEDED(queue->PresentX(0u, nullptr, nullptr)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentTimingStats(queue.Get(), &out.timing_blank)));

    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetVulkanRoutingStats(dev.Get(), &out.vk_stats)));
    return out;
}

void Test1_D3D12ToVulkanRoutingParityAndDirectSpirv(bool prefer_warp) {
    std::printf("[Test 1] Direct d3d12_xs -> Vulkan 1.4 routing parity + SPIR-V Gfx/Compute/Mesh...\n");

    // 1A: Standard DXBC GDK triangle run on D3D12 vs Vulkan-routed (0.0000 MAE!)
    const auto d3d_run = RunStandardD3D12Triangle(false, prefer_warp);
    const auto vk_run = RunStandardD3D12Triangle(true, prefer_warp);

    OS_REQUIRE(d3d_run.active_backend == D3D12XBOX_BACKEND_D3D12);
    OS_REQUIRE(vk_run.active_backend == D3D12XBOX_BACKEND_VULKAN);
    OS_REQUIRE(d3d_run.vk_stats.vulkan_routing_active == 0u);
    OS_REQUIRE(vk_run.vk_stats.vulkan_routing_active == 1u);
    OS_REQUIRE(vk_run.vk_stats.routed_command_lists >= 1ull);
    OS_REQUIRE(vk_run.vk_stats.routed_draws >= 1ull);
    OS_REQUIRE(vk_run.vk_stats.routed_barriers >= 1ull);
    OS_REQUIRE(vk_run.vk_stats.routed_rtv_clears >= 1ull);
    OS_REQUIRE(vk_run.vk_stats.routed_presents >= 2ull);

    const double mae = ComputeMaeRgba8(d3d_run.pixels, vk_run.pixels);
    std::printf("  Standard GDK Triangle D3D12 vs Vulkan-Routed: hash=0x%016llX vs 0x%016llX MAE=%.4f "
                "(ICD=%u device=\"%s\" API=%u.%u.%u)\n",
                static_cast<unsigned long long>(d3d_run.hash),
                static_cast<unsigned long long>(vk_run.hash),
                mae,
                vk_run.vk_stats.executed_on_vulkan_icd,
                vk_run.vk_stats.vulkan_device_name,
                vk_run.vk_stats.vulkan_api_major,
                vk_run.vk_stats.vulkan_api_minor,
                vk_run.vk_stats.vulkan_api_patch);
    OS_REQUIRE(mae == 0.0);
    OS_REQUIRE(d3d_run.hash == vk_run.hash);

    // 1B: PresentX separated fence signaling vs DXGI/Vulkan presentation timing stats
    auto check_timings = [](const char* label, const StandardTriangleRun& r) {
        OS_REQUIRE(r.timing1.present_count == 1ull);
        OS_REQUIRE(r.timing2.present_count == 2ull);
        OS_REQUIRE(r.timing_blank.present_count == 3ull);
        OS_REQUIRE(r.timing1.fence_completed_value >= 1ull);
        OS_REQUIRE(r.timing2.fence_completed_value > r.timing1.fence_completed_value);
        OS_REQUIRE(r.timing_blank.fence_completed_value > r.timing2.fence_completed_value);
        OS_REQUIRE(r.timing1.fence_signal_qpc > 0);
        OS_REQUIRE(r.timing1.present_submit_qpc >= r.timing1.fence_signal_qpc);
        OS_REQUIRE(r.timing1.present_complete_qpc >= r.timing1.present_submit_qpc);
        OS_REQUIRE(r.timing1.fence_wait_us >= 0.0);
        OS_REQUIRE(r.timing1.dxgi_present_us >= 0.0);
        OS_REQUIRE(r.timing1.total_presentx_us >= 0.0);
        std::printf("  PresentX Timings (%s): fence_val=%llu->%llu fence_wait=%.1fus present=%.1fus total=%.1fus\n",
                    label,
                    static_cast<unsigned long long>(r.timing1.fence_completed_value),
                    static_cast<unsigned long long>(r.timing2.fence_completed_value),
                    r.timing2.fence_wait_us,
                    r.timing2.dxgi_present_us,
                    r.timing2.total_presentx_us);
    };
    check_timings("D3D12", d3d_run);
    check_timings("Vulkan", vk_run);

    // 1C: Direct SPIR-V Graphics (DrawIndexedInstanced + PushConstants), Compute (Dispatch + Copy),
    //     and Mesh Shader (DispatchMesh) routed directly through d3d12_xs -> Vulkan 1.4!
    ScopedEnvVar vk_env("OPENSCARLETT_GPU_BACKEND", "vulkan");
    OpenScarlettD3D12SetBackendMode(D3D12XBOX_BACKEND_AUTO);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1u;

    ComPtr<ID3D12DeviceX> dev;
    OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_ID3D12DeviceX,
                                               reinterpret_cast<void**>(dev.Put()))));
    OS_REQUIRE(OpenScarlettD3D12GetActiveBackend(dev.Get()) == D3D12XBOX_BACKEND_VULKAN);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueueX,
                                                 reinterpret_cast<void**>(queue.Put()))));
    ComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));
    ComPtr<ID3D12GraphicsCommandListX> cl;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                IID_ID3D12GraphicsCommandListX,
                                                reinterpret_cast<void**>(cl.Put()))));

    // Root signature with 4 root 32-bit constants + SRV + UAV
    D3D12_ROOT_PARAMETER rparams[3]{};
    rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rparams[0].Constants = {0u, 0u, 4u};
    rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rparams[1].Descriptor = {0u, 0u};
    rparams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rparams[2].Descriptor = {0u, 0u};
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3u;
    rsd.pParameters = rparams;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs_blob, rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
    ComPtr<ID3D12RootSignature> rs;
    OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                  IID_PPV_ARGS(rs.Put()))));

    // --- Direct SPIR-V Graphics PSO (DrawIndexedInstanced) ---
    const auto vs_spv = BuildPosColorVertexSpirv();
    const auto ps_spv = BuildPushConstantMulFragmentSpirv();
    D3D12_INPUT_ELEMENT_DESC il[2]{};
    il[0] = {"POSITION", 0u, DXGI_FORMAT_R32G32B32A32_FLOAT, 0u, 0u, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u};
    il[1] = {"COLOR", 0u, DXGI_FORMAT_R32G32B32A32_FLOAT, 0u, 16u, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpsd{};
    gpsd.pRootSignature = rs.Get();
    gpsd.VS = {vs_spv.data(), vs_spv.size() * sizeof(uint32_t)};
    gpsd.PS = {ps_spv.data(), ps_spv.size() * sizeof(uint32_t)};
    gpsd.InputLayout = {il, 2u};
    gpsd.NumRenderTargets = 1u;
    gpsd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpsd.SampleDesc.Count = 1u;
    ComPtr<ID3D12PipelineState> gfx_pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&gpsd, IID_PPV_ARGS(gfx_pso.Put()))));

    // Upload VB (fullscreen quad via 2 triangles / 4 verts) + IB (6 indices)
    struct Vertex { float pos[4]; float col[4]; };
    const Vertex verts[4] = {
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{ 1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{ 1.0f,  1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{-1.0f,  1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    };
    const uint32_t idxs[6] = {0u, 1u, 2u, 0u, 2u, 3u};

    auto make_upload_buf = [&](const void* data, size_t bytes, ComPtr<ID3D12Resource>& out_res) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<size_t>(bytes, 256u);
        bd.Height = 1u; bd.DepthOrArraySize = 1u; bd.MipLevels = 1u;
        bd.SampleDesc.Count = 1u; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out_res.Put()))));
        void* p = nullptr;
        D3D12_RANGE rn{0, 0};
        OS_REQUIRE(SUCCEEDED(out_res->Map(0u, &rn, &p)));
        std::memcpy(p, data, bytes);
        out_res->Unmap(0u, nullptr);
    };

    ComPtr<ID3D12Resource> vb, ib;
    make_upload_buf(verts, sizeof(verts), vb);
    make_upload_buf(idxs, sizeof(idxs), ib);

    // Render target 32x32
    constexpr uint32_t kRtW = 32u, kRtH = 32u;
    D3D12_HEAP_PROPERTIES def_hp{}; def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rtd{};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = kRtW; rtd.Height = kRtH; rtd.DepthOrArraySize = 1u; rtd.MipLevels = 1u;
    rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtd.SampleDesc.Count = 1u;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(rt.Put()))));
    D3D12_DESCRIPTOR_HEAP_DESC rhd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1u, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0u};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(rtv_heap.Put()))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_h);

    // --- Direct SPIR-V Compute PSO (Dispatch + CopyBufferRegion) ---
    const auto cs_spv = BuildComputeMulAddSpirv(32u);
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpsd{};
    cpsd.pRootSignature = rs.Get();
    cpsd.CS = {cs_spv.data(), cs_spv.size() * sizeof(uint32_t)};
    ComPtr<ID3D12PipelineState> cs_pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(cs_pso.Put()))));

    std::vector<uint32_t> cs_in(32u);
    for (uint32_t i = 0; i < 32u; ++i) cs_in[i] = i + 10u;
    ComPtr<ID3D12Resource> cs_in_buf, cs_out_buf, cs_rb_buf;
    make_upload_buf(cs_in.data(), cs_in.size() * sizeof(uint32_t), cs_in_buf);
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256u; bd.Height = 1u; bd.DepthOrArraySize = 1u; bd.MipLevels = 1u;
        bd.SampleDesc.Count = 1u; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &def_hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(cs_out_buf.Put()))));

        D3D12_HEAP_PROPERTIES rb_hp{}; rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &rb_hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(cs_rb_buf.Put()))));
    }

    // Record graphics indexed draw + compute dispatch + copy
    const float clear_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float tint[4] = {0.25f, 0.50f, 0.75f, 1.0f};
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kRtW), static_cast<float>(kRtH), 0.0f, 1.0f};
    cl->RSSetViewports(1u, &vp);
    cl->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv_h, clear_col, 0u, nullptr);
    cl->SetGraphicsRootSignature(rs.Get());
    cl->SetGraphicsRoot32BitConstants(0u, 4u, tint, 0u);
    cl->SetPipelineState(gfx_pso.Get());
    D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex)};
    D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), sizeof(idxs), DXGI_FORMAT_R32_UINT};
    cl->IASetVertexBuffers(0u, 1u, &vbv);
    cl->IASetIndexBuffer(&ibv);
    cl->DrawIndexedInstanced(6u, 1u, 0u, 0, 0u);

    // Compute dispatch
    cl->SetComputeRootSignature(rs.Get());
    cl->SetPipelineState(cs_pso.Get());
    cl->SetComputeRootShaderResourceView(1u, cs_in_buf->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(2u, cs_out_buf->GetGPUVirtualAddress());
    cl->Dispatch(1u, 1u, 1u);
    D3D12_RESOURCE_BARRIER uav_bar{};
    uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    uav_bar.Transition = {cs_out_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1u, &uav_bar);
    cl->CopyBufferRegion(cs_rb_buf.Get(), 0ull, cs_out_buf.Get(), 0ull, cs_in.size() * sizeof(uint32_t));
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1u, lists);

    // Verify SPIR-V indexed graphics draw pixels via PresentX
    ID3D12Resource* planes[] = {rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1u;
    plane.ppResources = planes;
    plane.ResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12XBOX_PRESENT_PARAMETERS pres{};
    OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));

    std::vector<uint8_t> gfx_px(kRtW * kRtH * 4u, 0u);
    uint32_t copied = 0u;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), gfx_px.data(), static_cast<uint32_t>(gfx_px.size()), &copied)));
    const size_t mid = ((kRtH / 2u) * kRtW + (kRtW / 2u)) * 4u;
    // Expected ~(64, 128, 191, 255)
    OS_REQUIRE(std::abs(static_cast<int>(gfx_px[mid + 0]) - 64) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(gfx_px[mid + 1]) - 128) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(gfx_px[mid + 2]) - 191) <= 2);
    OS_REQUIRE(gfx_px[mid + 3] == 255u);

    // Verify SPIR-V compute output via CopyBufferRegion readback
    void* rb_ptr = nullptr;
    D3D12_RANGE rb_rng{0, cs_in.size() * sizeof(uint32_t)};
    OS_REQUIRE(SUCCEEDED(cs_rb_buf->Map(0u, &rb_rng, &rb_ptr)));
    const auto* cs_out = static_cast<const uint32_t*>(rb_ptr);
    for (uint32_t i = 0; i < 32u; ++i) {
        OS_REQUIRE(cs_out[i] == cs_in[i] * 3u + 7u);
    }
    cs_rb_buf->Unmap(0u, nullptr);

    // --- Direct SPIR-V Mesh Shader PSO via CreatePipelineState stream + DispatchMesh ---
    const auto ms_spv = BuildSimpleMeshSpirv();
    const auto pass_ps_spv = BuildPassColorFragmentSpirv();
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(void*) MeshStream {
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } ms;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } ps;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RT_FORMAT_ARRAY v; } rts;
    } stream{};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    stream.ms.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
    stream.ms.v = {ms_spv.data(), ms_spv.size() * sizeof(uint32_t)};
    stream.ps.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
    stream.ps.v = {pass_ps_spv.data(), pass_ps_spv.size() * sizeof(uint32_t)};
    stream.rts.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    stream.rts.v.NumRenderTargets = 1u;
    stream.rts.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_PIPELINE_STATE_STREAM_DESC sdesc{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> mesh_pso;
    OS_REQUIRE(SUCCEEDED(dev->CreatePipelineState(&sdesc, IID_PPV_ARGS(mesh_pso.Put()))));

    OS_REQUIRE(SUCCEEDED(alloc->Reset()));
    OS_REQUIRE(SUCCEEDED(cl->Reset(alloc.Get(), mesh_pso.Get())));
    cl->RSSetViewports(1u, &vp);
    cl->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv_h, clear_col, 0u, nullptr);
    cl->DispatchMesh(1u, 1u, 1u);
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    queue->ExecuteCommandLists(1u, lists);
    OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), gfx_px.data(), static_cast<uint32_t>(gfx_px.size()), &copied)));
    // Mesh shader outputs pure green (0, 255, 0, 255) at center pixel!
    OS_REQUIRE(gfx_px[mid + 0] <= 2u);
    OS_REQUIRE(gfx_px[mid + 1] >= 253u);
    OS_REQUIRE(gfx_px[mid + 2] <= 2u);

    D3D12XBOX_VULKAN_ROUTING_STATS st{};
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetVulkanRoutingStats(dev.Get(), &st)));
    OS_REQUIRE(st.routed_indexed_draws >= 1ull);
    OS_REQUIRE(st.routed_dispatches >= 1ull);
    OS_REQUIRE(st.routed_mesh_dispatches >= 1ull);
    OS_REQUIRE(st.routed_copies >= 1ull);
    OS_REQUIRE(st.spirv_direct_draws >= 1ull);
    OS_REQUIRE(st.spirv_direct_dispatches >= 1ull);
    OS_REQUIRE(st.spirv_direct_mesh_dispatches >= 1ull);
    std::printf("  Direct SPIR-V via d3d12_xs OK: gfx_draws=%llu cs_dispatches=%llu mesh_dispatches=%llu icd_execs=%llu\n",
                static_cast<unsigned long long>(st.spirv_direct_draws),
                static_cast<unsigned long long>(st.spirv_direct_dispatches),
                static_cast<unsigned long long>(st.spirv_direct_mesh_dispatches),
                static_cast<unsigned long long>(st.vulkan_icd_executions));
}

void Test2_StagedBc1ToBc7Copies(openscarlett::VulkanBackend& vk, const char* label) {
    std::printf("[Test 2 - %s] Staged BC1-BC7 block-compressed texture copies (non-4x4 mips + custom pitch)...\n", label);

    const openscarlett::VulkanImageFormat bc_formats[] = {
        openscarlett::VulkanImageFormat::Bc1RgbaUnorm,
        openscarlett::VulkanImageFormat::Bc2Unorm,
        openscarlett::VulkanImageFormat::Bc3Unorm,
        openscarlett::VulkanImageFormat::Bc4Unorm,
        openscarlett::VulkanImageFormat::Bc5Unorm,
        openscarlett::VulkanImageFormat::Bc6hUfloat,
        openscarlett::VulkanImageFormat::Bc7Unorm,
    };

    for (size_t fi = 0; fi < std::size(bc_formats); ++fi) {
        const auto fmt = bc_formats[fi];
        const uint32_t block_bytes =
            (fmt == openscarlett::VulkanImageFormat::Bc1RgbaUnorm ||
             fmt == openscarlett::VulkanImageFormat::Bc4Unorm)
                ? 8u
                : 16u;

        // Non-multiple-of-4 mip chain: 10x10 -> 5x5 -> 2x2 -> 1x1!
        const uint32_t mip_dims[4][2] = {{10u, 10u}, {5u, 5u}, {2u, 2u}, {1u, 1u}};
        openscarlett::VulkanCompressedTextureCopyDesc desc{};
        desc.width = 10u;
        desc.height = 10u;
        desc.depth = 1u;
        desc.format = fmt;

        std::vector<std::vector<uint8_t>> expected_tight(4);
        for (uint32_t m = 0; m < 4u; ++m) {
            const uint32_t mw = mip_dims[m][0];
            const uint32_t mh = mip_dims[m][1];
            const uint32_t bx = (mw + 3u) / 4u;
            const uint32_t by = (mh + 3u) / 4u;
            const uint32_t tight_row = bx * block_bytes;
            // Alternate D3D12 256B-aligned pitch and odd custom pitch (+7 bytes padding)
            const uint32_t custom_pitch = (m % 2u == 0u) ? 256u : (tight_row + 7u);

            openscarlett::VulkanCompressedMipCopy mip{};
            mip.width = mw;
            mip.height = mh;
            mip.depth = 1u;
            mip.src_row_pitch_bytes = custom_pitch;
            mip.src_data.assign(static_cast<size_t>(by) * custom_pitch, 0xCCu);
            expected_tight[m].resize(static_cast<size_t>(by) * tight_row);

            for (uint32_t r = 0; r < by; ++r) {
                for (uint32_t c = 0; c < tight_row; ++c) {
                    const uint8_t v = static_cast<uint8_t>((fi * 37u + m * 53u + r * 19u + c * 11u + 3u) & 0xFFu);
                    mip.src_data[static_cast<size_t>(r) * custom_pitch + c] = v;
                    expected_tight[m][static_cast<size_t>(r) * tight_row + c] = v;
                }
            }
            desc.mips.push_back(std::move(mip));
        }

        openscarlett::VulkanCompressedTextureCopyResult res{};
        OS_REQUIRE(vk.CopyCompressedTextureStaged(desc, res).ok());
        OS_REQUIRE(res.staged_mip_count == 4u);
        OS_REQUIRE(res.readback_mips.size() == 4u);
        for (uint32_t m = 0; m < 4u; ++m) {
            OS_REQUIRE(res.readback_mips[m].data == expected_tight[m]);
        }
        if (!vk.is_fallback_mode() && vk.GetDeviceInfo().supports_bc_texture_compression) {
            OS_REQUIRE(res.executed_on_vulkan_icd);
            OS_REQUIRE(!res.used_software_fallback);
        }
    }

    // Also test UploadAndReadbackTexture directly with BC7
    openscarlett::VulkanTextureDesc bc7_tex{};
    bc7_tex.width = 8u;
    bc7_tex.height = 8u;
    bc7_tex.format = openscarlett::VulkanImageFormat::Bc7Unorm;
    openscarlett::VulkanTextureMipLevel m0{8u, 8u, 1u, std::vector<uint8_t>(2u * 2u * 16u, 0x5Au)};
    openscarlett::VulkanTextureMipLevel m1{4u, 4u, 1u, std::vector<uint8_t>(1u * 1u * 16u, 0xA5u)};
    bc7_tex.mips = {m0, m1};
    openscarlett::VulkanTextureRoundtripResult rt{};
    OS_REQUIRE(vk.UploadAndReadbackTexture(bc7_tex, rt).ok());
    OS_REQUIRE(rt.readback_mips.size() == 2u);
    OS_REQUIRE(rt.readback_mips[0].data == m0.data);
    OS_REQUIRE(rt.readback_mips[1].data == m1.data);
    std::printf("  All 7 BC1-BC7 formats + 10x10->1x1 mips + 256B/odd row pitch verified BIT-EXACT (ICD=%d)\n",
                rt.executed_on_vulkan_icd ? 1 : 0);
}

void Test3_MixedHeapDescriptorCollisionSeparation(
    openscarlett::VulkanBackend& vk_hw,
    openscarlett::VulkanBackend& vk_sw) {
    std::printf("[Test 3] Mixed-heap colliding buffer + image descriptor slot separation...\n");

    struct Vertex { float pos[4]; float col[4]; };
    const Vertex verts[3] = {
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{ 3.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{-1.0f,  3.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    };

    openscarlett::VulkanGraphicsDrawDesc desc{};
    desc.vertex_spirv = BuildPosColorVertexSpirv();
    desc.fragment_spirv = BuildCollidingUboAndTextureFragmentSpirv();
    desc.width = 16u;
    desc.height = 16u;
    desc.vertex_stride = sizeof(Vertex);
    desc.vertex_bytes.resize(sizeof(verts));
    std::memcpy(desc.vertex_bytes.data(), verts, sizeof(verts));
    desc.vertex_attributes = {
        {0u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 0u},
        {1u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 16u},
    };
    desc.vertex_count = 3u;

    // Colliding slot 0: UBO tint = (0.5, 1.0, 0.25, 1.0)
    openscarlett::VulkanDescriptorBinding ubo_b{};
    ubo_b.binding = 0u;
    ubo_b.kind = openscarlett::VulkanDescriptorKind::UniformBuffer;
    const float tint[4] = {0.5f, 1.0f, 0.25f, 1.0f};
    ubo_b.buffer_data.resize(sizeof(tint));
    std::memcpy(ubo_b.buffer_data.data(), tint, sizeof(tint));

    // Colliding slot 0: CombinedImageSampler solid (255, 128, 204, 255)
    openscarlett::VulkanDescriptorBinding tex_b{};
    tex_b.binding = 0u;
    tex_b.kind = openscarlett::VulkanDescriptorKind::CombinedImageSampler;
    tex_b.texture.width = 2u;
    tex_b.texture.height = 2u;
    tex_b.texture.format = openscarlett::VulkanImageFormat::Rgba8Unorm;
    openscarlett::VulkanTextureMipLevel tm{};
    tm.width = 2u; tm.height = 2u; tm.depth = 1u;
    tm.data = {
        255, 128, 204, 255,  255, 128, 204, 255,
        255, 128, 204, 255,  255, 128, 204, 255,
    };
    tex_b.texture.mips.push_back(std::move(tm));
    desc.descriptors = {ubo_b, tex_b};

    openscarlett::VulkanGraphicsDrawResult hw_res{}, sw_res{};
    OS_REQUIRE(vk_hw.ExecuteGraphicsDraw(desc, hw_res).ok());
    OS_REQUIRE(vk_sw.ExecuteGraphicsDraw(desc, sw_res).ok());

    OS_REQUIRE(hw_res.descriptor_collisions_resolved == 1u);
    OS_REQUIRE(hw_res.descriptor_slot_remaps.size() == 1u);
    OS_REQUIRE(hw_res.descriptor_slot_remaps[0].original_binding == 0u);
    OS_REQUIRE(hw_res.descriptor_slot_remaps[0].remapped_binding != 0u);
    OS_REQUIRE(hw_res.descriptor_slot_remaps[0].kind == openscarlett::VulkanDescriptorKind::CombinedImageSampler);

    const size_t cidx = (8u * 16u + 8u) * 4u;
    // Expected: R = 0.5*255 = 128, G = 1.0*128 = 128, B = 0.25*204 = 51, A = 255
    OS_REQUIRE(std::abs(static_cast<int>(hw_res.framebuffer.pixels[cidx + 0]) - 128) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(hw_res.framebuffer.pixels[cidx + 1]) - 128) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(hw_res.framebuffer.pixels[cidx + 2]) - 51) <= 2);
    OS_REQUIRE(hw_res.framebuffer.pixels[cidx + 3] == 255u);

    const double mae = ComputeMaeRgba8(hw_res.framebuffer.pixels, sw_res.framebuffer.pixels);
    OS_REQUIRE(mae <= 0.01);
    std::printf("  Mixed-heap slot 0 (UBO + CombinedImageSampler) remapped 0->%u | pixel=(%u,%u,%u,%u) | HW-vs-SW MAE=%.4f\n",
                hw_res.descriptor_slot_remaps[0].remapped_binding,
                hw_res.framebuffer.pixels[cidx + 0],
                hw_res.framebuffer.pixels[cidx + 1],
                hw_res.framebuffer.pixels[cidx + 2],
                hw_res.framebuffer.pixels[cidx + 3],
                mae);
}

void Test4_PushConstantBudgetGuard(openscarlett::VulkanBackend& vk) {
    std::printf("[Test 4] Device push-constant byte-budget guard...\n");
    const uint32_t dev_limit = vk.GetDeviceInfo().max_push_constants_size;
    OS_REQUIRE(dev_limit >= 128u);

    struct Vertex { float pos[4]; float col[4]; };
    const Vertex verts[3] = {
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{ 3.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{-1.0f,  3.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    };

    openscarlett::VulkanGraphicsDrawDesc desc{};
    desc.vertex_spirv = BuildPosColorVertexSpirv();
    desc.fragment_spirv = BuildPushConstantMulFragmentSpirv();
    desc.width = 16u;
    desc.height = 16u;
    desc.vertex_stride = sizeof(Vertex);
    desc.vertex_bytes.resize(sizeof(verts));
    std::memcpy(desc.vertex_bytes.data(), verts, sizeof(verts));
    desc.vertex_attributes = {
        {0u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 0u},
        {1u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 16u},
    };
    desc.vertex_count = 3u;

    // 4A: Within budget (16 bytes)
    const float tint[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    desc.push_constants.resize(16u);
    std::memcpy(desc.push_constants.data(), tint, 16u);
    openscarlett::VulkanGraphicsDrawResult ok_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(desc, ok_res).ok());
    OS_REQUIRE(!ok_res.push_constants_exceeded_budget);
    OS_REQUIRE(ok_res.effective_push_constant_bytes == 16u);

    // 4B: Over budget via per-draw override (32B payload vs 16B budget -> clamped to 16B, executes cleanly!)
    desc.push_constants.resize(32u, 0u);
    std::memcpy(desc.push_constants.data(), tint, 16u);
    desc.push_constant_budget_override = 16u;
    openscarlett::VulkanGraphicsDrawResult clamped_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(desc, clamped_res).ok());
    OS_REQUIRE(clamped_res.push_constants_exceeded_budget);
    OS_REQUIRE(clamped_res.effective_push_constant_bytes == 16u);
    OS_REQUIRE(clamped_res.device_push_constant_limit == 16u);

    // 4C: Over device hardware limit (dev_limit + 64 bytes -> clamped to dev_limit, never crashes!)
    desc.push_constant_budget_override = 0u;
    desc.push_constants.assign(static_cast<size_t>(dev_limit) + 64u, 0u);
    std::memcpy(desc.push_constants.data(), tint, 16u);
    openscarlett::VulkanGraphicsDrawResult over_dev_res{};
    OS_REQUIRE(vk.ExecuteGraphicsDraw(desc, over_dev_res).ok());
    OS_REQUIRE(over_dev_res.push_constants_exceeded_budget);
    OS_REQUIRE(over_dev_res.effective_push_constant_bytes == (dev_limit & ~3u));

    // 4D: Backend-level test override
    vk.SetPushConstantBudgetOverrideForTest(64u);
    OS_REQUIRE(vk.GetEffectivePushConstantBudget() == 64u);
    vk.SetPushConstantBudgetOverrideForTest(0u);
    OS_REQUIRE(vk.GetEffectivePushConstantBudget() == dev_limit);

    std::printf("  Device maxPushConstantsSize=%uB | 16B ok, 32B/16B guarded, %uB/%uB guarded cleanly\n",
                dev_limit, dev_limit + 64u, dev_limit);
}

void Test5_VkDeviceSubstringSelection(const openscarlett::VulkanDeviceInfo& baseline_info) {
    std::printf("[Test 5] OPENSCARLETT_VK_DEVICE adapter substring selection...\n");

    if (!baseline_info.is_fallback && baseline_info.device_name.size() >= 3u) {
        // Match case-insensitive substring of the detected hardware adapter (e.g. "gtx" or "nvidia")
        std::string sub = baseline_info.device_name.substr(0, std::min<size_t>(6u, baseline_info.device_name.size()));
        for (char& c : sub) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        ScopedEnvVar env("OPENSCARLETT_VK_DEVICE", sub.c_str());
        openscarlett::VulkanBackend matched_vk;
        OS_REQUIRE(matched_vk.Initialize(false).ok());
        OS_REQUIRE(!matched_vk.is_fallback_mode());
        OS_REQUIRE(matched_vk.GetDeviceInfo().matched_device_filter);
        OS_REQUIRE(matched_vk.GetDeviceInfo().selected_device_filter == sub);
        OS_REQUIRE(matched_vk.GetDeviceInfo().device_name == baseline_info.device_name);
        std::printf("  Matched OPENSCARLETT_VK_DEVICE=\"%s\" -> \"%s\"\n",
                    sub.c_str(), matched_vk.GetDeviceInfo().device_name.c_str());
    }

    // Non-matching filter: strict mode returns clean error, fallback mode degrades cleanly to software
    {
        ScopedEnvVar env("OPENSCARLETT_VK_DEVICE", "NONEXISTENT_GPU_ADAPTER_9999");
        openscarlett::VulkanBackend strict_vk;
        OS_REQUIRE(!strict_vk.Initialize(false).ok());
        OS_REQUIRE(!strict_vk.GetDeviceInfo().matched_device_filter);

        openscarlett::VulkanBackend fallback_vk;
        OS_REQUIRE(fallback_vk.Initialize(true).ok());
        OS_REQUIRE(fallback_vk.is_fallback_mode());
        OS_REQUIRE(!fallback_vk.GetDeviceInfo().matched_device_filter);
        OS_REQUIRE(fallback_vk.GetDeviceInfo().selected_device_filter == "NONEXISTENT_GPU_ADAPTER_9999");
        std::printf("  Non-matching OPENSCARLETT_VK_DEVICE handled cleanly (strict=error, fallback=software)\n");
    }
}

void Test6_Wave32OnWave64Compute(openscarlett::VulkanBackend& vk) {
    std::printf("[Test 6] Wave32 compute execution on Wave64-only device mode...\n");
    const auto& info = vk.GetDeviceInfo();
    OS_REQUIRE(info.subgroup_size > 0u);

    const auto cs_spv = BuildComputeMulAddSpirv(32u);
    std::vector<uint32_t> input(48u);
    for (uint32_t i = 0; i < 48u; ++i) input[i] = i * 5u + 1u;

    // 6A: Native subgroup mode
    std::vector<uint32_t> native_out;
    openscarlett::VulkanComputeDispatchOptions native_opts{};
    native_opts.requested_wave_size = 32u;
    openscarlett::VulkanComputeDispatchResult native_res{};
    OS_REQUIRE(vk.ExecuteComputeShaderEx(cs_spv, input, native_out, 2u, native_opts, native_res).ok());
    OS_REQUIRE(native_out.size() >= 48u);

    // 6B: Simulated Wave64-only device mode (Wave32 shader on Wave64 GPU)
    std::vector<uint32_t> w64_out;
    openscarlett::VulkanComputeDispatchOptions w64_opts{};
    w64_opts.requested_wave_size = 32u;
    w64_opts.wave32_on_wave64_safe = true;
    w64_opts.simulated_device_subgroup_size = 64u;
    openscarlett::VulkanComputeDispatchResult w64_res{};
    OS_REQUIRE(vk.ExecuteComputeShaderEx(cs_spv, input, w64_out, 2u, w64_opts, w64_res).ok());
    OS_REQUIRE(w64_res.device_subgroup_size == 64u);
    OS_REQUIRE(w64_res.effective_wave_size == 32u);
    OS_REQUIRE(w64_res.wave32_on_wave64_adapted);
    OS_REQUIRE(w64_res.padded_buffer_elements >= 64u && (w64_res.padded_buffer_elements % 64u) == 0u);
    OS_REQUIRE(w64_out.size() == native_out.size());
    for (uint32_t i = 0; i < 48u; ++i) {
        OS_REQUIRE(native_out[i] == input[i] * 3u + 7u);
        OS_REQUIRE(w64_out[i] == native_out[i]);
    }
    std::printf("  Native subgroup_size=%u (stages=0x%X) | Wave32-on-Wave64 adapted (padded=%u elems, bit-exact)\n",
                info.subgroup_size, info.supported_subgroup_stages, w64_res.padded_buffer_elements);
}

void Test7_SoftwareFallbackAndWarpGraceful() {
    std::printf("[Test 7] Graceful WARP + Vulkan software fallback routing...\n");
    ScopedEnvVar force_sw("OPENSCARLETT_VK_FORCE_SOFTWARE", "1");
    const auto sw_run = RunStandardD3D12Triangle(true, /*prefer_warp=*/true);
    OS_REQUIRE(sw_run.active_backend == D3D12XBOX_BACKEND_VULKAN);
    OS_REQUIRE(sw_run.vk_stats.vulkan_routing_active == 1u);
    OS_REQUIRE(sw_run.vk_stats.vulkan_software_fallback == 1u);
    OS_REQUIRE(sw_run.vk_stats.executed_on_vulkan_icd == 0u);
    OS_REQUIRE(sw_run.vk_stats.routed_presents >= 2ull);
    OS_REQUIRE(sw_run.hash != 0ull);
    std::printf("  WARP + Vulkan Software Fallback OK (hash=0x%016llX, presents=%llu)\n",
                static_cast<unsigned long long>(sw_run.hash),
                static_cast<unsigned long long>(sw_run.vk_stats.routed_presents));
}

#endif  // _WIN32

}  // namespace

int main() {
#if !defined(_WIN32)
    std::printf("test_d3d12_vk_routing skipped (non-Windows)\n");
    return 0;
#else
    const bool prefer_warp = EnvFlagEnabled("OPENSCARLETT_PREFER_WARP");
    OpenScarlettD3D12SetPreferWarp(prefer_warp ? 1 : 0);
    std::printf("=== OpenScarlett Sprint 7: d3d12_xs -> Vulkan 1.4 Routing & Cross-Vendor Tests (PreferWarp=%d) ===\n",
                prefer_warp ? 1 : 0);

    openscarlett::VulkanBackend vk_hw;
    OS_REQUIRE(vk_hw.Initialize(/*allow_software_fallback=*/true).ok());
    openscarlett::VulkanBackend vk_sw;
    OS_REQUIRE(vk_sw.InitializeSoftwareFallback().ok());

    Test1_D3D12ToVulkanRoutingParityAndDirectSpirv(prefer_warp);
    Test2_StagedBc1ToBc7Copies(vk_hw, vk_hw.is_fallback_mode() ? "SW" : "ICD");
    Test2_StagedBc1ToBc7Copies(vk_sw, "SW-Fallback");
    Test3_MixedHeapDescriptorCollisionSeparation(vk_hw, vk_sw);
    Test4_PushConstantBudgetGuard(vk_hw);
    Test5_VkDeviceSubstringSelection(vk_hw.GetDeviceInfo());
    Test6_Wave32OnWave64Compute(vk_hw);
    Test6_Wave32OnWave64Compute(vk_sw);
    Test7_SoftwareFallbackAndWarpGraceful();

    std::printf("=== ALL 7 SPRINT 7 D3D12->VULKAN ROUTING & CROSS-VENDOR TESTS PASSED ===\n");
    return 0;
#endif
}
