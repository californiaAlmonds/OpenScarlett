#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"

namespace openscarlett {

// CPU texture mip representation (supports 2D, 3D, and 6-face Cube textures).
struct Rdna2CpuTextureMip {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;  // 1 for 2D, depth for 3D, 6 for Cube
    // Row-major RGBA32F pixels: ((z * height + y) * width + x) * 4 + ch
    std::vector<float> rgba32f;
    // Optional R32_UINT texels for IMAGE_ATOMIC_CMPSWAP: (z * height + y) * width + x
    std::vector<uint32_t> r32u;
};

struct Rdna2CpuTexture {
    uint8_t dim = 1;  // 1=2D, 2=3D, 3=Cube
    std::vector<Rdna2CpuTextureMip> mips;

    void AllocateRgba32f(
        uint8_t dimension,
        uint32_t width,
        uint32_t height,
        uint32_t depth_or_faces = 1,
        uint32_t mip_levels = 1);

    void AllocateR32u(
        uint8_t dimension,
        uint32_t width,
        uint32_t height,
        uint32_t depth_or_faces = 1,
        uint32_t mip_levels = 1);

    std::array<float, 4> LoadRgba32f(uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept;
    std::array<uint32_t, 4> LoadR32uRgba(uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept;
    uint32_t LoadR32u(uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept;
    std::array<float, 4> SampleNearest(float u, float v, float lod) const noexcept;
    void StoreRgba32f(uint32_t x, uint32_t y, uint32_t z, const std::array<float, 4>& rgba) noexcept;
    uint32_t AtomicCmpSwapR32u(uint32_t x, uint32_t y, uint32_t z, uint32_t cmp_val, uint32_t new_val) noexcept;
};

struct Rdna2CpuThreadExport {
    std::array<float, 4> pos0 = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> param0 = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> mrt0 = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<uint32_t, 3> prim = {0u, 0u, 0u};
    bool wrote_pos0 = false;
    bool wrote_param0 = false;
    bool wrote_mrt0 = false;
    bool wrote_prim = false;
    bool culled_prim = false;
};

struct Rdna2CpuExecState {
    // Bound shader resources
    std::vector<uint32_t> root_constants;  // b0[0..15]
    std::vector<uint32_t> uav0;            // u0 (in0 / global / flat / BVH node buffer)
    std::vector<uint32_t> uav1;            // u1 (out0 when srsrc != 0)
    Rdna2CpuTexture tex2d_srv;             // t0 (2D RGBA32F SRV, supports mip chain)
    Rdna2CpuTexture tex3d_srv;             // 3D RGBA32F SRV, supports mip chain
    Rdna2CpuTexture texcube_srv;           // Cube (6 faces) RGBA32F SRV, supports mip chain
    Rdna2CpuTexture tex2d_uav;             // RWTexture2D<float4>
    Rdna2CpuTexture tex_u32_uav;           // RWTexture2D/3D/Cube<uint> for IMAGE_ATOMIC_CMPSWAP

    // Optional per-thread stage input (Pixel Shader PARAM0 -> v0..v3)
    std::vector<std::array<float, 4>> stage_inputs;

    // Execution parameters
    uint32_t dispatch_x = 1;
    uint32_t dispatch_y = 1;
    uint32_t dispatch_z = 1;
    uint32_t lds_size_dwords = 16384;      // 64 KB RDNA2 LDS
    uint32_t scratch_size_dwords = 256;    // 1 KB per-thread scratch
    uint32_t max_steps_per_wave = 1000000;

    // Stage outputs & telemetry
    std::vector<Rdna2CpuThreadExport> exports;
    uint32_t mesh_vertex_count = 0;
    uint32_t mesh_primitive_count = 0;
    std::array<uint32_t, 3> dispatch_mesh_groups = {0u, 0u, 0u};
    uint64_t total_instructions_executed = 0;
    uint32_t barriers_synchronized = 0;
    uint32_t waves_executed = 0;
};

// CPU reference interpreter for decoded AMD RDNA2 / GFX10.3 wave32 & wave64 shaders.
Status ExecuteRdna2Cpu(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2CpuExecState& state);

// Convenience wrapper: parses raw XS2\0 or Xbox-wrapped DXBC, disassembles, and executes on CPU.
Status ExecuteRdna2BlobCpu(
    const std::vector<uint8_t>& blob,
    Rdna2CpuExecState& state);

}  // namespace openscarlett
