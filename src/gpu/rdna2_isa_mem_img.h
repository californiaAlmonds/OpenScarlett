#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"

namespace openscarlett::rdna2_mem {

// ---------------------------------------------------------------------------
// Semantic classification for GFX10.3 memory / image / LDS / FLAT / EXP ops
// ---------------------------------------------------------------------------
enum class MemImgSemanticClass : uint16_t {
    Unknown = 0,
    BufferLoadFormat,
    BufferStoreFormat,
    BufferLoadRaw,
    BufferStoreRaw,
    BufferAtomic,
    BufferCache,
    TbufferLoadFormat,
    TbufferStoreFormat,
    ImageLoad,
    ImageStore,
    ImageQuery,
    ImageAtomic,
    ImageSample,
    ImageGather4,
    ImageBvhIntersect,
    ImageMsaaLoad,
    DsLoad,
    DsStore,
    DsAtomic,
    DsCrossLane,
    DsOrdered,
    DsGws,
    DsNop,
    FlatLoad,
    FlatStore,
    FlatAtomic,
    GlobalLoad,
    GlobalStore,
    GlobalAtomic,
    ScratchLoad,
    ScratchStore,
    Export,
};

// ---------------------------------------------------------------------------
// GFX10.3 Memory / Image / LDS / FLAT / EXP Opcode Table Entry
// ---------------------------------------------------------------------------
struct MemImgOpcodeEntry {
    Rdna2Encoding encoding;
    uint16_t raw_opcode;
    uint8_t flat_seg;  // 0=FLAT or non-FLAT, 1=SCRATCH, 2=GLOBAL
    const char* mnemonic;
    uint8_t data_dwords;  // 0..4
    bool is_load;
    bool is_store;
    bool is_atomic;
    bool has_rtn;
    bool is_d16;
    bool is_sample;
    bool is_gather;
    bool is_bvh;
    uint16_t semantic_class;
    bool supports_spirv_lift;
    bool supports_dxil_lift;
    bool supports_cpu_ref;
    bool supports_gpu_exec;
};

struct MemImgDecodedInst {
    Rdna2Instruction base;
    const MemImgOpcodeEntry* entry = nullptr;
    bool d16 = false;
    bool a16 = false;
    bool r128 = false;
    bool tfe = false;
    bool lwe = false;
    bool lds = false;
};

struct MemImgEncodeParams {
    uint8_t vdst = 0;
    uint8_t vdata = 0;
    uint8_t vaddr = 0;
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    uint8_t srsrc_sgpr = 0;   // Base SGPR index (multiple of 4, e.g. 0, 4, 8)
    uint8_t ssamp_sgpr = 0;   // Base SGPR index (multiple of 4, e.g. 0, 4, 8)
    uint8_t soffset = 128;    // MUBUF/MTBUF scalar offset code (128 = inline 0)
    uint8_t saddr = 0x7Fu;    // FLAT/GLOBAL/SCRATCH scalar base SGPR (0x7F = off)
    int32_t offset = 0;       // MUBUF/MTBUF 12-bit, FLAT/GLOBAL/SCRATCH 12-bit, DS 16-bit
    uint8_t offset0 = 0;      // DS explicit 8-bit offset0 (used when offset == 0)
    uint8_t offset1 = 0;      // DS explicit 8-bit offset1 (used when offset == 0)
    bool idxen = false;
    bool offen = false;
    bool glc = false;
    bool slc = false;
    bool dlc = false;
    bool lds = false;
    bool tfe = false;
    bool lwe = false;
    bool d16 = false;
    bool a16 = false;
    bool r128 = false;
    bool unrm = false;
    bool gds = false;
    uint8_t dmask = 0xFu;
    uint8_t dim = 1u;  // 0=1D, 1=2D, 2=3D, 3=Cube, 4=1DArray, 5=2DArray, 6=2DMsaa, 7=2DArrayMsaa
    Rdna2MtbufFormat mtbuf_format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
    uint8_t exp_target = 0;  // 0..7=mrt0..7, 8=mrtz, 9=null, 12..15=pos0..3, 20=prim, 32..63=param0..31
    uint8_t exp_en = 0xFu;
    uint8_t exp_vsrc[4] = {0, 1, 2, 3};
    bool exp_done = false;
    bool exp_compr = false;
    bool exp_vm = false;
};

// ---------------------------------------------------------------------------
// GFX10.3 MIMG Image Dimension IDs (dim[2:0])
// ---------------------------------------------------------------------------
inline constexpr uint8_t kMimgDim1D = 0u;
inline constexpr uint8_t kMimgDim2D = 1u;
inline constexpr uint8_t kMimgDim3D = 2u;
inline constexpr uint8_t kMimgDimCube = 3u;
inline constexpr uint8_t kMimgDim1DArray = 4u;
inline constexpr uint8_t kMimgDim2DArray = 5u;
inline constexpr uint8_t kMimgDim2DMsaa = 6u;
inline constexpr uint8_t kMimgDim2DArrayMsaa = 7u;

// ---------------------------------------------------------------------------
// CPU Reference Execution State (Wave32 / Wave64)
// ---------------------------------------------------------------------------
struct MemImgTextureMip {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth_or_layers = 1;
    uint32_t samples = 1;
    // Packed RGBA32F: ((((layer * height + y) * width + x) * samples + s) * 4 + ch)
    std::vector<float> rgba32f;
    // Packed R32_UINT: (((layer * height + y) * width + x) * samples + s)
    std::vector<uint32_t> r32u;
};

struct MemImgTexture {
    uint8_t dim = kMimgDim2D;
    std::vector<MemImgTextureMip> mips;

    void AllocateRgba32f(
        uint8_t dimension,
        uint32_t width,
        uint32_t height,
        uint32_t depth_or_layers = 1,
        uint32_t mip_levels = 1,
        uint32_t samples = 1);

    void AllocateR32u(
        uint8_t dimension,
        uint32_t width,
        uint32_t height,
        uint32_t depth_or_layers = 1,
        uint32_t mip_levels = 1,
        uint32_t samples = 1);

    std::array<float, 4> LoadRgba32f(
        uint32_t x, uint32_t y, uint32_t z, uint32_t mip = 0, uint32_t sample = 0) const noexcept;
    void StoreRgba32f(
        uint32_t x, uint32_t y, uint32_t z, const std::array<float, 4>& rgba,
        uint32_t mip = 0, uint32_t sample = 0) noexcept;
    uint32_t LoadR32u(
        uint32_t x, uint32_t y, uint32_t z, uint32_t mip = 0, uint32_t sample = 0) const noexcept;
    void StoreR32u(
        uint32_t x, uint32_t y, uint32_t z, uint32_t val,
        uint32_t mip = 0, uint32_t sample = 0) noexcept;
    uint32_t* MutableR32uPtr(
        uint32_t x, uint32_t y, uint32_t z, uint32_t mip = 0, uint32_t sample = 0) noexcept;
    std::array<float, 4> Sample(
        float u, float v, float w, float lod = 0.0f, bool linear = false, bool unrm = false) const noexcept;
    std::array<float, 4> Gather4(
        float u, float v, float w, uint32_t channel, bool unrm = false) const noexcept;
};

struct MemImgSampler {
    bool linear_filter = false;
    bool clamp_to_edge = true;
    float lod_bias = 0.0f;
    float min_lod = 0.0f;
    float max_lod = 16.0f;
    // Comparison mode for _c variants: 0=less_equal, 1=less, 2=greater, 3=greater_equal
    uint32_t compare_func = 0;
};

struct MemImgBvhNode {
    // 0 = triangle leaf (v0, v1, v2, prim_id), 1 = box4 internal (4x aabb + 4x child_ptr)
    uint32_t node_type = 0;
    uint32_t primitive_id = 0;
    std::array<float, 3> v0 = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> v1 = {1.0f, 0.0f, 0.0f};
    std::array<float, 3> v2 = {0.0f, 1.0f, 0.0f};
    std::array<std::array<float, 6>, 4> child_aabb{};  // [min_x, min_y, min_z, max_x, max_y, max_z]
    std::array<uint32_t, 4> child_ptrs = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
};

struct MemImgExports {
    std::array<std::array<float, 4>, 8> mrt{};
    uint8_t mrt_written_mask = 0;
    float mrtz = 0.0f;
    bool mrtz_written = false;
    std::array<std::array<float, 4>, 4> pos{};
    uint8_t pos_written_mask = 0;
    std::array<std::array<float, 4>, 32> param{};
    uint32_t param_written_mask = 0;
    std::array<uint32_t, 4> prim{};
    bool prim_written = false;
    bool null_exported = false;
    bool last_compr = false;
    bool last_vm = false;
    bool done_seen = false;
};

struct MemImgExecState {
    uint32_t wave_size = 32;  // 1..64
    uint64_t exec_mask = 0xFFFFFFFFull;
    std::array<std::array<uint32_t, 64>, 64> vgpr{};
    std::array<uint32_t, 106> sgpr{};
    uint32_t m0 = 0;

    std::vector<uint8_t> uav0;  // Byte-addressable buffer 0 (srsrc == 0)
    std::vector<uint8_t> uav1;  // Byte-addressable buffer 1 (srsrc != 0)
    std::vector<uint8_t> lds;   // 64 KB byte-addressable LDS
    std::array<std::vector<uint8_t>, 64> scratch;  // Per-lane byte-addressable scratch

    // Textures indexed by dim (0=1D, 1=2D, 2=3D, 3=Cube, 4=1DArray, 5=2DArray, 6=2DMsaa, 7=2DArrayMsaa)
    std::array<MemImgTexture, 8> textures;
    MemImgTexture rw_texture;
    MemImgSampler sampler;

    std::vector<MemImgBvhNode> bvh_nodes;
    std::array<MemImgExports, 64> exports;

    uint32_t append_consume_counter = 0;
    uint32_t gws_barrier_count = 0;
    uint32_t buffer_stride_bytes = 16;
    Rdna2MtbufFormat default_mubuf_format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
    uint32_t gl0_inv_count = 0;
    uint32_t gl1_inv_count = 0;

    MemImgExecState() {
        lds.assign(65536u, 0u);
        for (auto& lane_scratch : scratch) {
            lane_scratch.assign(1024u, 0u);
        }
    }
};

// ---------------------------------------------------------------------------
// Public API (openscarlett::rdna2_mem)
// ---------------------------------------------------------------------------
const std::vector<MemImgOpcodeEntry>& GetMemImgOpcodeTable() noexcept;

const MemImgOpcodeEntry* LookupMemImgOpcode(
    Rdna2Encoding enc,
    uint16_t raw_opcode,
    uint8_t flat_seg = 0) noexcept;

const MemImgOpcodeEntry* LookupMemImgByName(const char* mnemonic) noexcept;

size_t GetUniqueMemImgMnemonicCount() noexcept;

Status DecodeMemImgAt(
    const uint32_t* words,
    size_t word_count,
    size_t pc,
    MemImgDecodedInst& out_inst);

Status DecodeMemImgStream(
    const std::vector<uint32_t>& words,
    std::vector<MemImgDecodedInst>& out_insts);

bool EncodeMemImgInst(
    const char* mnemonic,
    const MemImgEncodeParams& params,
    std::vector<uint32_t>& out_words);

// MTBUF unified 7-bit format metadata & unpack/pack helpers
uint32_t MtbufFormatByteStride(Rdna2MtbufFormat fmt) noexcept;
uint32_t MtbufFormatComponentBytes(Rdna2MtbufFormat fmt) noexcept;
uint32_t UnpackMtbufFormatComponent(
    const uint8_t* bytes,
    size_t buf_size,
    uint32_t base_byte_addr,
    Rdna2MtbufFormat fmt,
    uint32_t comp_idx,
    bool as_d16) noexcept;
void PackMtbufFormatComponent(
    uint8_t* bytes,
    size_t buf_size,
    uint32_t base_byte_addr,
    Rdna2MtbufFormat fmt,
    uint32_t comp_idx,
    uint32_t value_bits,
    bool from_d16) noexcept;

// Seeds standard compute-thread VGPRs (v0=lane, v1=lane*4, v2=lane*16, v3=lane*8) matching LiftMemImgShader.
void SeedDefaultComputeVgprs(MemImgExecState& state) noexcept;

// CPU reference execution for MUBUF/MTBUF/MIMG/DS/FLAT/GLOBAL/SCRATCH/EXP.
Status ExecuteMemImgCpu(
    const std::vector<MemImgDecodedInst>& insts,
    MemImgExecState& state);

// Lifts decoded memory/image/LDS/FLAT/EXP shader into SPIR-V 1.3 + SM 6.5 HLSL (+ optional spirv-val & dxc).
Status LiftMemImgShader(
    const Xs2ShaderContainer& container,
    const std::vector<MemImgDecodedInst>& insts,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options = {});

}  // namespace openscarlett::rdna2_mem
