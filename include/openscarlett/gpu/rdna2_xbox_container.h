#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/descriptor_sgpr_bridge.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"

namespace openscarlett {

// ---------------------------------------------------------------------------
// Public AMD GFX10.3 / RDNA2 PM4 Packet & SH/Context Register Constants
// (Based on public AMD RDNA2 ISA / PM4 / GFX10 register documentation)
// ---------------------------------------------------------------------------
inline constexpr uint32_t kPm4PacketType3Mask = 0xC0000000u;
inline constexpr uint32_t kPm4ItNop = 0x10u;
inline constexpr uint32_t kPm4ItSetContextReg = 0x69u;
inline constexpr uint32_t kPm4ItSetShReg = 0x76u;

inline constexpr uint32_t kRdna2ShRegBaseDword = 0x2C00u;
inline constexpr uint32_t kRdna2ContextRegBaseDword = 0xA000u;

// SH shader program resource registers (dword MMIO offsets)
inline constexpr uint32_t kRegSpiShaderPgmRsrc3Ps = 0x2C07u;
inline constexpr uint32_t kRegSpiShaderPgmRsrc1Ps = 0x2C0Au;
inline constexpr uint32_t kRegSpiShaderPgmRsrc2Ps = 0x2C0Bu;

inline constexpr uint32_t kRegSpiShaderPgmRsrc3Vs = 0x2C47u;
inline constexpr uint32_t kRegSpiShaderPgmRsrc1Vs = 0x2C4Au;
inline constexpr uint32_t kRegSpiShaderPgmRsrc2Vs = 0x2C4Bu;

inline constexpr uint32_t kRegSpiShaderPgmRsrc3Gs = 0x2C87u;
inline constexpr uint32_t kRegSpiShaderPgmRsrc1Gs = 0x2C8Au;
inline constexpr uint32_t kRegSpiShaderPgmRsrc2Gs = 0x2C8Bu;

inline constexpr uint32_t kRegSpiShaderPgmRsrc3Hs = 0x2D07u;
inline constexpr uint32_t kRegSpiShaderPgmRsrc1Hs = 0x2D0Au;
inline constexpr uint32_t kRegSpiShaderPgmRsrc2Hs = 0x2D0Bu;

inline constexpr uint32_t kRegComputeNumThreadX = 0x2E07u;
inline constexpr uint32_t kRegComputeNumThreadY = 0x2E08u;
inline constexpr uint32_t kRegComputeNumThreadZ = 0x2E09u;
inline constexpr uint32_t kRegComputePgmRsrc1 = 0x2E12u;
inline constexpr uint32_t kRegComputePgmRsrc2 = 0x2E13u;
inline constexpr uint32_t kRegComputePgmRsrc3 = 0x2E28u;

// Context registers for VS->PS interpolation & clip/cull linkage (dword MMIO offsets)
inline constexpr uint32_t kRegSpiPsInputCntl0 = 0xA191u;  // 0xA191 .. 0xA1B0 (32 attributes)
inline constexpr uint32_t kRegSpiPsInputEna = 0xA1B3u;
inline constexpr uint32_t kRegSpiPsInputAddr = 0xA1B4u;
inline constexpr uint32_t kRegSpiPsInControl = 0xA1B6u;
inline constexpr uint32_t kRegPaClVsOutCntl = 0xA207u;

// SPI_PS_INPUT_ENA / SPI_PS_INPUT_ADDR public GFX10 bit positions
inline constexpr uint32_t kSpiPsInputPerspSampleEna = (1u << 0);
inline constexpr uint32_t kSpiPsInputPerspCenterEna = (1u << 1);
inline constexpr uint32_t kSpiPsInputPerspCentroidEna = (1u << 2);
inline constexpr uint32_t kSpiPsInputLinearSampleEna = (1u << 4);
inline constexpr uint32_t kSpiPsInputLinearCenterEna = (1u << 5);
inline constexpr uint32_t kSpiPsInputLinearCentroidEna = (1u << 6);
inline constexpr uint32_t kSpiPsInputPosXFloatEna = (1u << 8);
inline constexpr uint32_t kSpiPsInputPosYFloatEna = (1u << 9);
inline constexpr uint32_t kSpiPsInputPosZFloatEna = (1u << 10);
inline constexpr uint32_t kSpiPsInputPosWFloatEna = (1u << 11);
inline constexpr uint32_t kSpiPsInputFrontFaceEna = (1u << 12);

// SPI_PS_INPUT_CNTL_n bitfields
inline constexpr uint32_t kSpiPsInputCntlOffsetMask = 0x1Fu;
inline constexpr uint32_t kSpiPsInputCntlFlatShadeBit = (1u << 10);
inline constexpr uint32_t kSpiPsInputCntlFp16InterpBit = (1u << 16);

// ---------------------------------------------------------------------------
// Extended GFX10.3 64-bit / 32-bit EXEC-mask SOP1 & SOP2 raw opcodes
// ---------------------------------------------------------------------------
inline constexpr uint16_t kSop1RawMovB64 = 0x04u;
inline constexpr uint16_t kSop1RawAndSaveExecB64 = 0x24u;
inline constexpr uint16_t kSop1RawOrSaveExecB64 = 0x25u;
inline constexpr uint16_t kSop1RawXorSaveExecB64 = 0x26u;
inline constexpr uint16_t kSop1RawAndN2SaveExecB64 = 0x27u;
inline constexpr uint16_t kSop1RawAndSaveExecB32 = 0x3Cu;
inline constexpr uint16_t kSop1RawOrSaveExecB32 = 0x3Du;
inline constexpr uint16_t kSop1RawXorSaveExecB32 = 0x3Eu;
inline constexpr uint16_t kSop1RawAndN2SaveExecB32 = 0x3Fu;

inline constexpr uint16_t kSop2RawAndB64 = 0x0Fu;
inline constexpr uint16_t kSop2RawOrB64 = 0x11u;
inline constexpr uint16_t kSop2RawXorB64 = 0x13u;
inline constexpr uint16_t kSop2RawAndN2B64 = 0x15u;

// RDNA2 / GFX10.3 MIMG BVH ray intersection raw opcodes
inline constexpr uint16_t kMimgRawImageBvhIntersectRay = 0xE6u;
inline constexpr uint16_t kMimgRawImageBvh64IntersectRay = 0xE7u;

// Emits raw GFX10.3 SOP1/SOP2 EXEC-mask instructions (s_and_saveexec_b64, s_mov_b64,
// s_or_b64, s_xor_b64, s_andn2_b64, etc.) directly into a word stream.
void EmitRawExecSop1(std::vector<uint32_t>& words, uint16_t raw_op, uint8_t sdst, const Rdna2Operand& ssrc0);
void EmitRawExecSop2(std::vector<uint32_t>& words, uint16_t raw_op, uint8_t sdst,
                     const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);

// Emits a raw GFX10.3 MIMG image_bvh64_intersect_ray (opcode 0xE7) instruction directly into a word stream.
void EmitRawBvh64IntersectRay(
    std::vector<uint32_t>& words,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc,
    uint8_t dmask = 0xFu);

// Fast zero-allocation detector recognizing raw XS2\0 blobs AND Xbox DXBC containers
// carrying XBOX, XS2M, RDNA, PM4R, or RREG chunks (returns false for standard host DXBC/DXIL).
bool IsXboxShaderContainerBlob(const void* data, size_t size) noexcept;

// Extended disassembler: decodes all standard Rdna2Opcode instructions PLUS GFX10.3 64-bit/32-bit
// EXEC-mask manipulation instructions and image_bvh64_intersect_ray (MIMG 0xE7).
Status DisassembleRdna2Extended(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions);

// ---------------------------------------------------------------------------
// DXBC Header, Signature (ISGN/OSGN/PSGN), & RTS0 Root Signature
// ---------------------------------------------------------------------------
struct XboxDxbcHeaderInfo {
    uint8_t md5_digest[16] = {};
    uint32_t version = 0;
    uint32_t total_size = 0;
    uint32_t chunk_count = 0;
    bool md5_valid = false;
};

struct XboxSignatureParamDesc {
    std::string semantic_name;
    uint32_t semantic_index = 0;
    uint32_t system_value_type = 0;
    uint32_t component_type = 3u;  // 1=uint, 2=sint, 3=float
    uint32_t register_index = 0;
    uint8_t mask = 0xFu;
    uint8_t rw_mask = 0xFu;
    uint32_t stream = 0;
    uint32_t min_precision = 0;
    Rdna2InterpMode interp_mode = Rdna2InterpMode::Perspective;
};

enum class XboxRootParamType : uint32_t {
    DescriptorTable = 0u,
    Constants32Bit = 1u,
    Cbv = 2u,
    Srv = 3u,
    Uav = 4u,
};

enum class XboxDescriptorRangeType : uint32_t {
    Srv = 0u,
    Uav = 1u,
    Cbv = 2u,
    Sampler = 3u,
};

struct XboxDescriptorRange {
    XboxDescriptorRangeType range_type = XboxDescriptorRangeType::Srv;
    uint32_t num_descriptors = 1u;
    uint32_t base_shader_register = 0u;
    uint32_t register_space = 0u;
    uint32_t flags = 0u;  // RTS0 v1.1 (D3D12_DESCRIPTOR_RANGE_FLAGS)
    uint32_t offset_in_descriptors_from_table_start = 0xFFFFFFFFu;
};

struct XboxRootParameter {
    XboxRootParamType type = XboxRootParamType::Constants32Bit;
    uint32_t shader_visibility = 0u;  // 0=ALL, 1=VS, 2=HS, 3=DS, 4=GS, 5=PS, 6=AS, 7=MS
    uint32_t shader_register = 0u;
    uint32_t register_space = 0u;
    uint32_t num_32bit_values = 0u;   // For Constants32Bit
    uint32_t descriptor_flags = 0u;   // For v1.1 Cbv/Srv/Uav
    std::vector<XboxDescriptorRange> descriptor_ranges;  // For DescriptorTable
};

struct XboxRootSignatureDesc {
    bool present = false;
    uint32_t version = 1u;  // 1 = v1.0, 2 = v1.1
    uint32_t flags = 0u;    // D3D12_ROOT_SIGNATURE_FLAGS (bit 0 = ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
    std::vector<XboxRootParameter> parameters;
    std::vector<XboxStaticSamplerInfo> static_samplers;
};

// Parses and builds D3D12 serialized RTS0 root signatures (versions 1.0 and 1.1).
Status ParseRts0RootSignature(const uint8_t* data, size_t size, XboxRootSignatureDesc& out_desc);
std::vector<uint8_t> BuildRts0RootSignature(const XboxRootSignatureDesc& desc);

// ---------------------------------------------------------------------------
// PM4 / SH & Context Register Setup Block Parser & Builder
// ---------------------------------------------------------------------------
struct Rdna2PsInputCntl {
    uint8_t attr_index = 0;
    uint8_t offset = 0;
    bool flat_shade = false;
    bool fp16_interp = false;
    uint32_t raw_value = 0;
};

struct Rdna2PgmRegisterSetup {
    bool present = false;
    ShaderStage inferred_stage = ShaderStage::Unknown;

    uint32_t pgm_rsrc1 = 0;
    uint32_t pgm_rsrc2 = 0;
    uint32_t pgm_rsrc3 = 0;

    uint32_t vgpr_count = 0;
    uint32_t sgpr_count = 0;
    uint32_t user_sgpr_count = 0;
    uint32_t wave_size = 32u;
    uint32_t lds_size_bytes = 0;
    bool scratch_enabled = false;
    bool wgp_mode = false;

    uint32_t threadgroup_x = 0;
    uint32_t threadgroup_y = 0;
    uint32_t threadgroup_z = 0;

    uint32_t spi_ps_input_ena = 0;
    uint32_t spi_ps_input_addr = 0;
    uint32_t spi_ps_in_control = 0;
    uint32_t pa_cl_vs_out_cntl = 0;

    bool persp_sample_ena = false;
    bool persp_center_ena = false;
    bool persp_centroid_ena = false;
    bool linear_sample_ena = false;
    bool linear_center_ena = false;
    bool linear_centroid_ena = false;
    bool front_face_ena = false;

    uint8_t vs_cull_dist_mask = 0;
    uint8_t vs_clip_dist_mask = 0;
    bool vs_exports_rt_index = false;
    bool vs_exports_viewport_index = false;

    std::vector<Rdna2PsInputCntl> ps_input_cntls;
    std::vector<std::pair<uint32_t, uint32_t>> raw_registers;  // (dword_reg_offset, value)
};

// Parses PM4 Type-3 SET_SH_REG / SET_CONTEXT_REG packet streams OR structured (reg, val) pair blocks.
Status ParsePm4RegisterBlock(const uint8_t* data, size_t size, Rdna2PgmRegisterSetup& out_setup);
// Builds a PM4 Type-3 packet stream encoding the register setup.
std::vector<uint8_t> BuildPm4RegisterBlock(const Rdna2PgmRegisterSetup& setup, ShaderStage stage);

// ---------------------------------------------------------------------------
// User-Data SGPR (s0..s31) Root-Binding Map Builder
// ---------------------------------------------------------------------------
enum class XboxUserSgprSlotKind : uint8_t {
    Unused = 0,
    RootConstant32,
    RootCbvPtrLo,
    RootCbvPtrHi,
    RootSrvPtrLo,
    RootSrvPtrHi,
    RootUavPtrLo,
    RootUavPtrHi,
    DescriptorTablePtr32,
    VertexBufferTablePtrLo,
    VertexBufferTablePtrHi,
    ScratchBasePtrLo,
    ScratchBasePtrHi,
    SpillTablePtrLo,
    SpillTablePtrHi,
};

struct XboxUserSgprBinding {
    uint32_t sgpr_index = 0;         // Starting SGPR in s0..s31
    uint32_t sgpr_count = 1;         // 1 (const / 32-bit table ptr) or 2 (64-bit VA ptr)
    uint32_t root_param_index = 0;   // Index into XboxRootSignatureDesc::parameters (0xFFFFFFFF for system/VB/spill)
    XboxUserSgprSlotKind kind = XboxUserSgprSlotKind::Unused;
    uint32_t shader_register = 0;
    uint32_t register_space = 0;
    uint32_t const_dword_offset = 0;
    bool spilled_to_table = false;
    uint32_t spill_dword_offset = 0;
    std::vector<XboxDescriptorRange> descriptor_ranges;
};

struct XboxRootSgprMap {
    ShaderStage stage = ShaderStage::Unknown;
    uint32_t total_user_sgprs = 0;
    uint32_t max_inline_user_sgprs = 16u;
    bool uses_scratch_sgpr = false;
    bool uses_vertex_buffer_table = false;
    uint32_t vertex_buffer_table_sgpr = 0xFFFFFFFFu;
    bool uses_spill_table = false;
    uint32_t spill_table_sgpr = 0xFFFFFFFFu;
    uint32_t spill_table_dwords = 0;
    std::array<XboxUserSgprSlotKind, 32> slot_kinds{};
    std::vector<XboxUserSgprBinding> bindings;
    std::vector<XboxStaticSamplerInfo> static_samplers;
};

// Allocates s0..s31 user-data SGPR slots from RTS0 + PM4 register setup according to Xbox D3D12 rules.
Status BuildUserSgprRootMap(
    const XboxRootSignatureDesc& root_sig,
    const Rdna2PgmRegisterSetup* reg_setup,
    ShaderStage stage,
    XboxRootSgprMap& out_map,
    uint32_t max_inline_user_sgprs = 16u);

// Applies mapped s0..s15 user-data SGPR bindings directly onto a DescriptorSgprBridge instance.
Status ApplyRootSgprMapToDescriptorBridge(
    const XboxRootSgprMap& map,
    const std::vector<uint32_t>& root_constant_values,
    const std::vector<uint64_t>& root_descriptor_vas,
    DescriptorSgprBridge& bridge);

struct XboxVsPrologueInfo {
    bool uses_vertex_id_v0 = false;
    bool uses_instance_id_v3 = false;
    bool declared_vertex_id_in_sig = false;
    bool declared_instance_id_in_sig = false;
    uint8_t injected_iid_sentinel_sgpr = 0xFFu;
};

struct XboxBvhIntersectInfo {
    bool detected = false;
    bool is_64bit = false;
    bool a16 = false;
    uint8_t srsrc_sgpr = 0u;
    uint32_t instruction_count = 0u;
    uint32_t threadgroup_x = 64u;
    uint32_t ray_flags = 0u;
    uint32_t instance_mask = 0xFFu;
};

// ---------------------------------------------------------------------------
// Full Xbox dxc_xs DXBC/RDNA2 Container Model, Parser, & Synthetic Builder
// ---------------------------------------------------------------------------
struct XboxParsedShaderContainer {
    XboxShaderBlobFormat format = XboxShaderBlobFormat::Unknown;
    XboxDxbcHeaderInfo header{};
    bool has_xs2 = false;
    Xs2ShaderContainer xs2{};
    bool has_raw_rdna_isa = false;
    std::vector<uint32_t> rdna_isa_words;
    std::string shader_payload_fourcc;   // "XBOX", "XS2M", "RDNA", "DXIL", "SHDR", "SHEX"
    std::vector<uint8_t> shdr_or_shex_payload;

    std::vector<XboxSignatureParamDesc> input_signature;          // ISGN / ISG1
    std::vector<XboxSignatureParamDesc> output_signature;         // OSGN / OSG5 / OSG1
    std::vector<XboxSignatureParamDesc> patch_constant_signature; // PSGN / PSG1

    XboxRootSignatureDesc root_signature{};  // RTS0 (v1.0 or v1.1)
    Rdna2PgmRegisterSetup reg_setup{};       // PM4 SET_SH_REG / SET_CONTEXT_REG / PM4R
    XboxPsv0Info psv0{};                     // PSV0 runtime metadata
    XboxRootSgprMap sgpr_map{};              // Derived s0..s31 user-data binding map

    std::vector<XboxDxbcChunk> chunks;

    // Signature element stride (24=ISGN/OSGN, 28=OSG5, 32=ISG1/OSG1), inferred root bindings,
    // VS v0/v3 prologue metadata, and RDNA2 BVH ray-intersection detection.
    uint32_t input_sig_element_stride = 0u;
    uint32_t output_sig_element_stride = 0u;
    bool root_signature_inferred = false;
    XboxRootSignatureDesc inferred_root_signature{};
    XboxVsPrologueInfo vs_prologue{};
    XboxBvhIntersectInfo bvh_info{};
};

struct XboxFullDxbcBuildDesc {
    ShaderStage stage = ShaderStage::Compute;
    std::vector<uint8_t> xs2_blob;                 // Optional XS2 payload (placed in xbox_chunk_fourcc)
    std::vector<uint32_t> raw_rdna_isa_words;      // Optional raw RDNA2 ISA words (for "RDNA" chunk)
    std::string xbox_chunk_fourcc = "XBOX";        // "XBOX", "XS2M", or "RDNA"
    bool embed_pm4_in_shader_chunk = false;        // If true, embeds PM4 register block alongside ISA in XBOX/RDNA
    Rdna2PgmRegisterSetup reg_setup{};             // Optional PM4 register setup (PM4R chunk or embedded)
    std::vector<XboxSignatureParamDesc> input_signature;
    std::vector<XboxSignatureParamDesc> output_signature;
    std::vector<XboxSignatureParamDesc> patch_constant_signature;
    bool use_sig_v1 = true;                        // Emit ISG1/OSG1/PSG1 (true) or ISGN/OSGN/PSGN (false)
    XboxRootSignatureDesc root_signature{};        // Optional full RTS0 v1.0 / v1.1 root signature
    XboxPsv0Info psv0{};                           // Optional PSV0 chunk
    std::vector<uint8_t> shdr_or_shex_payload;     // Optional SHDR / SHEX / DXIL auxiliary chunk
    std::string aux_shader_fourcc = "SHEX";
};

Status ParseFullXboxShaderContainer(const void* data, size_t size, XboxParsedShaderContainer& out_parsed);
Status ParseFullXboxShaderContainer(const std::vector<uint8_t>& bytes, XboxParsedShaderContainer& out_parsed);
std::vector<uint8_t> BuildFullXboxDxbcContainer(const XboxFullDxbcBuildDesc& desc);

// Populates Rdna2LiftOptions (FSSM pixel inputs, interpolation modes, vertex fetch mappings, etc.)
// directly from a parsed Xbox shader container.
void ApplyXboxContainerToLiftOptions(const XboxParsedShaderContainer& container, Rdna2LiftOptions& opts);

// Infers descriptor-table and root-constant bindings from PM4/SGPR setup and RDNA2 ISA
// when RTS0 is absent (especially Mesh/Amplification pipelines).
bool InferShaderRootBindingsWhenRts0Absent(XboxParsedShaderContainer& container);

struct XboxContainerLiftOptions {
    Rdna2LiftOptions lift_options{};
    bool enable_vs_system_id_prologue = true;
    bool infer_missing_root_signature = true;
    bool use_in_process_dxc = true;
    const XboxParsedShaderContainer* paired_other_stage = nullptr;
};

struct XboxContainerLiftResult {
    XboxParsedShaderContainer parsed{};
    Rdna2LiftResult lift{};
    bool vs_v0_auto_loaded = false;
    bool vs_v3_instance_id_auto_loaded = false;
    bool root_signature_inferred = false;
    bool uses_ray_query_bridge = false;
};

// Container-aware Xbox DXBC / PM4 / RDNA2 shader lifter:
//  - Handles 24/28/32-byte ISGN/OSGN/OSG5/ISG1/OSG1 signatures, PM4 registers, and inferred root tables
//  - Auto-loads VS v0 (SV_VertexID) and v3 (SV_InstanceID) prologues into HLSL & SPIR-V
//  - Applies paired-stage interpolation linkage reconciliation when paired_other_stage is supplied
//  - Compiles HLSL via in-process dxcompiler.dll (with dxc.exe fallback) and validates SPIR-V
Status LiftXboxShaderContainer(
    const void* data,
    size_t size,
    XboxContainerLiftResult& out_result,
    const XboxContainerLiftOptions& options = {});

// ---------------------------------------------------------------------------
// VS -> PS Interpolation Modifier PSO Linkage Validator & Matcher
// ---------------------------------------------------------------------------
struct XboxInterpLinkageEntry {
    std::string semantic_name;
    uint32_t semantic_index = 0;
    uint32_t location = 0;
    Rdna2InterpMode vs_mode = Rdna2InterpMode::Perspective;
    Rdna2InterpMode ps_mode = Rdna2InterpMode::Perspective;
    Rdna2InterpMode resolved_mode = Rdna2InterpMode::Perspective;
    bool matched = false;
    std::string hlsl_qualifier;
    uint32_t spirv_decoration = 0;  // 0=default perspective, 13=NoPerspective, 14=Flat, 16=Centroid, 17=Sample
};

struct XboxPsoInterpLinkageReport {
    bool all_compatible = true;
    std::vector<XboxInterpLinkageEntry> entries;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

const char* HlslInterpQualifierName(Rdna2InterpMode mode) noexcept;
uint32_t SpirvInterpDecoration(Rdna2InterpMode mode) noexcept;

// Matches VS OSGN/OSG1 outputs against PS ISGN/ISG1 + SPI_PS_INPUT_ENA/ADDR/CNTL interpolation modes,
// validates compatibility, and optionally updates vs_ir->output_interp_modes & ps_ir->input_interp_modes.
Status ValidateAndMatchStageInterpolation(
    const XboxParsedShaderContainer& vs_container,
    const XboxParsedShaderContainer& ps_container,
    XboxPsoInterpLinkageReport& out_report,
    Rdna2IrModule* vs_ir = nullptr,
    Rdna2IrModule* ps_ir = nullptr);

// ---------------------------------------------------------------------------
// CFG Dominance, SCC, Natural Loop & Irreducible-CFG Analysis (implemented in rdna2_ir.cpp)
// ---------------------------------------------------------------------------
struct Rdna2CfgAnalysis {
    bool is_irreducible = false;
    bool has_cycles = false;
    uint32_t reachable_block_count = 0;
    uint32_t natural_loop_count = 0;
    uint32_t irreducible_scc_count = 0;
    std::vector<uint32_t> loop_headers;
    std::vector<std::pair<uint32_t, uint32_t>> back_edges;  // (latch_bb, header_bb)
    std::vector<std::vector<uint32_t>> irreducible_sccs;
};

void AnalyzeRdna2Cfg(const Rdna2IrModule& ir, Rdna2CfgAnalysis& out_analysis);

// ---------------------------------------------------------------------------
// In-Process dxcompiler.dll Compiler Helpers (implemented in shader_cache.cpp)
// ---------------------------------------------------------------------------
bool IsInProcessDxcAvailable(const std::string& hint_dxc_path = {});

Status CompileHlslInProcessDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& hint_dxc_path = {});

}  // namespace openscarlett
