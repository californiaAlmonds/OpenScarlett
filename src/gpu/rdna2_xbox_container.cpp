#include "openscarlett/gpu/rdna2_xbox_container.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>

namespace openscarlett {

namespace {

uint32_t ReadLe32(const uint8_t* p) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

float BitsToFloat(uint32_t bits) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(bits));
    return f;
}

uint32_t FloatToBits(float f) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void PatchLe32(std::vector<uint8_t>& out, size_t offset, uint32_t v) {
    if (offset + 4u <= out.size()) {
        out[offset + 0u] = static_cast<uint8_t>(v & 0xFFu);
        out[offset + 1u] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        out[offset + 2u] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        out[offset + 3u] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    }
}

std::string ToUpperAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return r;
}

bool IsSystemValueSemantic(const std::string& name) {
    const std::string u = ToUpperAscii(name);
    return u.rfind("SV_", 0) == 0;
}

void ReadStaticSampler(const uint8_t* p, XboxStaticSamplerInfo& s) noexcept {
    s.filter = ReadLe32(p + 0);
    s.address_u = ReadLe32(p + 4);
    s.address_v = ReadLe32(p + 8);
    s.address_w = ReadLe32(p + 12);
    s.mip_lod_bias = BitsToFloat(ReadLe32(p + 16));
    s.max_anisotropy = ReadLe32(p + 20);
    s.comparison_func = ReadLe32(p + 24);
    s.border_color = ReadLe32(p + 28);
    s.min_lod = BitsToFloat(ReadLe32(p + 32));
    s.max_lod = BitsToFloat(ReadLe32(p + 36));
    s.shader_register = ReadLe32(p + 40);
    s.register_space = ReadLe32(p + 44);
    s.shader_visibility = ReadLe32(p + 48);
}

void AppendStaticSampler(std::vector<uint8_t>& out, const XboxStaticSamplerInfo& s) {
    AppendLe32(out, s.filter);
    AppendLe32(out, s.address_u);
    AppendLe32(out, s.address_v);
    AppendLe32(out, s.address_w);
    AppendLe32(out, FloatToBits(s.mip_lod_bias));
    AppendLe32(out, s.max_anisotropy);
    AppendLe32(out, s.comparison_func);
    AppendLe32(out, s.border_color);
    AppendLe32(out, FloatToBits(s.min_lod));
    AppendLe32(out, FloatToBits(s.max_lod));
    AppendLe32(out, s.shader_register);
    AppendLe32(out, s.register_space);
    AppendLe32(out, s.shader_visibility);
}

bool IsValidSemanticAscii(const uint8_t* data, size_t size, uint32_t name_off) noexcept {
    if (!data || name_off >= size) return false;
    const size_t max_len = std::min<size_t>(size - name_off, 128u);
    if (max_len == 0u) return false;
    size_t len = 0u;
    while (len < max_len && data[name_off + len] != 0u) {
        const uint8_t ch = data[name_off + len];
        if (ch < 0x20u || ch > 0x7Eu) return false;
        ++len;
    }
    if (len == 0u || name_off + len >= size || data[name_off + len] != 0u) return false;
    return true;
}

bool TryParseSignatureChunkWithStride(
    const uint8_t* data,
    size_t size,
    uint32_t count,
    uint32_t table_off,
    uint32_t stride,
    bool strict_ascii,
    std::vector<XboxSignatureParamDesc>& out_params) {
    if (stride != 24u && stride != 28u && stride != 32u) return false;
    const uint64_t table_end = static_cast<uint64_t>(table_off) +
                               static_cast<uint64_t>(count) * static_cast<uint64_t>(stride);
    if (table_end > size) return false;

    const bool has_stream = (stride == 28u || stride == 32u);
    const bool has_min_prec = (stride == 32u);

    std::vector<XboxSignatureParamDesc> parsed;
    parsed.reserve(count);
    for (uint32_t i = 0u; i < count; ++i) {
        const uint8_t* rec = data + table_off + static_cast<size_t>(i) * stride;
        XboxSignatureParamDesc e{};
        size_t off = 0u;
        if (has_stream) {
            e.stream = ReadLe32(rec + 0u);
            off = 4u;
        }
        const uint32_t name_off = ReadLe32(rec + off + 0u);
        if (strict_ascii) {
            if (static_cast<uint64_t>(name_off) < table_end || !IsValidSemanticAscii(data, size, name_off)) {
                return false;
            }
        }
        e.semantic_index = ReadLe32(rec + off + 4u);
        e.system_value_type = ReadLe32(rec + off + 8u);
        e.component_type = ReadLe32(rec + off + 12u);
        e.register_index = ReadLe32(rec + off + 16u);
        const uint32_t masks = ReadLe32(rec + off + 20u);
        e.mask = static_cast<uint8_t>(masks & 0xFFu);
        e.rw_mask = static_cast<uint8_t>((masks >> 8) & 0xFFu);
        // Optional extension: upper byte of masks can encode explicit interp mode when non-zero
        const uint8_t interp_hint = static_cast<uint8_t>((masks >> 16) & 0xFFu);
        if (interp_hint >= 1u && interp_hint <= 5u) {
            e.interp_mode = static_cast<Rdna2InterpMode>(interp_hint - 1u);
        } else if (e.component_type == 1u || e.component_type == 2u) {
            // D3D12 / HLSL requires integer stage inputs/outputs to be Flat (nointerpolation)
            e.interp_mode = Rdna2InterpMode::Flat;
        }
        if (has_min_prec) {
            e.min_precision = ReadLe32(rec + 28u);
        }
        if (name_off < size) {
            size_t end = name_off;
            const size_t limit = std::min<size_t>(size, static_cast<size_t>(name_off) + 128u);
            while (end < limit && data[end] != 0u) ++end;
            e.semantic_name.assign(reinterpret_cast<const char*>(data + name_off), end - name_off);
        }
        parsed.push_back(std::move(e));
    }
    out_params = std::move(parsed);
    return true;
}

uint32_t ParseSignatureChunk(
    const uint8_t* data,
    size_t size,
    uint32_t expected_stride,
    std::vector<XboxSignatureParamDesc>& out_params,
    bool allow_non_strict_fallback = true) {
    if (!data || size < 8u) return 0u;
    const uint32_t count = ReadLe32(data + 0u);
    uint32_t table_off = ReadLe32(data + 4u);
    if (table_off == 0u) {
        table_off = 8u;
    }
    if (count > 256u || table_off < 8u || table_off > size) {
        return 0u;
    }

    // Try expected stride first (24 for ISGN/OSGN/PSGN, 28 for OSG5, 32 for ISG1/OSG1/PSG1)
    // with strict semantic-table bounds validation; gracefully adapt if an Xbox container packs
    // 24-byte D3D11_SIGNATURE_PARAMETER_DESC elements vs 32-byte extended elements.
    if (TryParseSignatureChunkWithStride(data, size, count, table_off, expected_stride, true, out_params)) {
        return expected_stride;
    }
    const uint32_t fallbacks[3] = {24u, 32u, 28u};
    for (uint32_t alt : fallbacks) {
        if (alt == expected_stride) continue;
        if (TryParseSignatureChunkWithStride(data, size, count, table_off, alt, true, out_params)) {
            return alt;
        }
    }
    if (allow_non_strict_fallback &&
        TryParseSignatureChunkWithStride(data, size, count, table_off, expected_stride, false, out_params)) {
        return expected_stride;
    }
    return 0u;
}

std::vector<uint8_t> BuildSignatureChunk(
    const std::vector<XboxSignatureParamDesc>& params,
    bool is_v1) {
    const uint32_t count = static_cast<uint32_t>(params.size());
    const size_t stride = is_v1 ? 32u : 24u;
    const size_t table_end = 8u + static_cast<size_t>(count) * stride;

    std::vector<uint8_t> str_pool;
    std::vector<uint32_t> name_offsets;
    name_offsets.reserve(count);
    for (const auto& p : params) {
        name_offsets.push_back(static_cast<uint32_t>(table_end + str_pool.size()));
        const std::string name = p.semantic_name.empty() ? "TEXCOORD" : p.semantic_name;
        str_pool.insert(str_pool.end(), name.begin(), name.end());
        str_pool.push_back(0u);
    }
    while ((str_pool.size() & 3u) != 0u) {
        str_pool.push_back(0u);
    }

    std::vector<uint8_t> out;
    out.reserve(table_end + str_pool.size());
    AppendLe32(out, count);
    AppendLe32(out, 8u);
    for (uint32_t i = 0u; i < count; ++i) {
        const auto& p = params[i];
        if (is_v1) {
            AppendLe32(out, p.stream);
        }
        AppendLe32(out, name_offsets[i]);
        AppendLe32(out, p.semantic_index);
        AppendLe32(out, p.system_value_type);
        AppendLe32(out, p.component_type);
        AppendLe32(out, p.register_index);
        const uint32_t interp_hint =
            (p.interp_mode != Rdna2InterpMode::Perspective)
                ? (static_cast<uint32_t>(p.interp_mode) + 1u)
                : 0u;
        const uint32_t masks = static_cast<uint32_t>(p.mask) |
                               (static_cast<uint32_t>(p.rw_mask) << 8) |
                               (interp_hint << 16);
        AppendLe32(out, masks);
        if (is_v1) {
            AppendLe32(out, p.min_precision);
        }
    }
    out.insert(out.end(), str_pool.begin(), str_pool.end());
    return out;
}

uint32_t NormalizeRegDwordOffset(uint32_t raw_reg) noexcept {
    // Support both dword register offsets (0x2C0A, 0xA1B3) and byte MMIO offsets (0xB028, 0x286CC)
    if (raw_reg >= 0x28000u) {
        return raw_reg >> 2;
    }
    if (raw_reg >= 0xB000u && raw_reg < 0x10000u) {
        return raw_reg >> 2;
    }
    return raw_reg;
}

void ApplyRegisterWrite(uint32_t reg_dw, uint32_t val, Rdna2PgmRegisterSetup& setup) {
    setup.present = true;
    setup.raw_registers.emplace_back(reg_dw, val);

    auto decode_rsrc1 = [&](uint32_t v, ShaderStage stage) {
        setup.pgm_rsrc1 = v;
        if (setup.inferred_stage == ShaderStage::Unknown) {
            setup.inferred_stage = stage;
        }
        const uint32_t vgpr_gran = (setup.wave_size == 64u) ? 4u : 8u;
        setup.vgpr_count = ((v & 0x3Fu) + 1u) * vgpr_gran;
        setup.sgpr_count = (((v >> 6) & 0xFu) + 1u) * 8u;
        setup.wgp_mode = ((v >> 29) & 1u) != 0u;
    };

    auto decode_rsrc2 = [&](uint32_t v, bool is_compute) {
        setup.pgm_rsrc2 = v;
        setup.scratch_enabled = (v & 1u) != 0u;
        setup.user_sgpr_count = (v >> 1) & 0x1Fu;
        if (is_compute) {
            // COMPUTE_PGM_RSRC2[23:15] = LDS_SIZE in 128-dword (512-byte) blocks
            const uint32_t lds_blocks = (v >> 15) & 0x1FFu;
            setup.lds_size_bytes = lds_blocks * 512u;
        } else {
            // SPI_SHADER_PGM_RSRC2_*[28:24] = EXTRA_LDS_SIZE or LDS_SIZE field
            const uint32_t lds_blocks = (v >> 24) & 0xFFu;
            if (lds_blocks > 0u) {
                setup.lds_size_bytes = lds_blocks * 512u;
            }
        }
    };

    auto decode_rsrc3 = [&](uint32_t v) {
        setup.pgm_rsrc3 = v;
        // Wave32/wave64 indicator bit in RSRC3[31] (1 = Wave64, 0 = native RDNA2 Wave32)
        if (((v >> 31) & 1u) != 0u) {
            setup.wave_size = 64u;
            // Recompute VGPR count using wave64 granularity if RSRC1 was already seen
            if (setup.pgm_rsrc1 != 0u) {
                setup.vgpr_count = ((setup.pgm_rsrc1 & 0x3Fu) + 1u) * 4u;
            }
        }
    };

    switch (reg_dw) {
        case kRegSpiShaderPgmRsrc1Ps: decode_rsrc1(val, ShaderStage::Pixel); break;
        case kRegSpiShaderPgmRsrc2Ps: decode_rsrc2(val, false); break;
        case kRegSpiShaderPgmRsrc3Ps: decode_rsrc3(val); break;

        case kRegSpiShaderPgmRsrc1Vs: decode_rsrc1(val, ShaderStage::Vertex); break;
        case kRegSpiShaderPgmRsrc2Vs: decode_rsrc2(val, false); break;
        case kRegSpiShaderPgmRsrc3Vs: decode_rsrc3(val); break;

        case kRegSpiShaderPgmRsrc1Gs: decode_rsrc1(val, ShaderStage::Geometry); break;
        case kRegSpiShaderPgmRsrc2Gs: decode_rsrc2(val, false); break;
        case kRegSpiShaderPgmRsrc3Gs: decode_rsrc3(val); break;

        case kRegSpiShaderPgmRsrc1Hs: decode_rsrc1(val, ShaderStage::Hull); break;
        case kRegSpiShaderPgmRsrc2Hs: decode_rsrc2(val, false); break;
        case kRegSpiShaderPgmRsrc3Hs: decode_rsrc3(val); break;

        case kRegComputePgmRsrc1: decode_rsrc1(val, ShaderStage::Compute); break;
        case kRegComputePgmRsrc2: decode_rsrc2(val, true); break;
        case kRegComputePgmRsrc3: decode_rsrc3(val); break;

        case kRegComputeNumThreadX: setup.threadgroup_x = val & 0xFFFFu; break;
        case kRegComputeNumThreadY: setup.threadgroup_y = val & 0xFFFFu; break;
        case kRegComputeNumThreadZ: setup.threadgroup_z = val & 0xFFFFu; break;

        case kRegSpiPsInputEna:
            setup.spi_ps_input_ena = val;
            setup.persp_sample_ena = (val & kSpiPsInputPerspSampleEna) != 0u;
            setup.persp_center_ena = (val & kSpiPsInputPerspCenterEna) != 0u;
            setup.persp_centroid_ena = (val & kSpiPsInputPerspCentroidEna) != 0u;
            setup.linear_sample_ena = (val & kSpiPsInputLinearSampleEna) != 0u;
            setup.linear_center_ena = (val & kSpiPsInputLinearCenterEna) != 0u;
            setup.linear_centroid_ena = (val & kSpiPsInputLinearCentroidEna) != 0u;
            setup.front_face_ena = (val & kSpiPsInputFrontFaceEna) != 0u;
            break;
        case kRegSpiPsInputAddr:
            setup.spi_ps_input_addr = val;
            break;
        case kRegSpiPsInControl:
            setup.spi_ps_in_control = val;
            break;
        case kRegPaClVsOutCntl:
            setup.pa_cl_vs_out_cntl = val;
            setup.vs_cull_dist_mask = static_cast<uint8_t>(val & 0xFFu);
            setup.vs_clip_dist_mask = static_cast<uint8_t>((val >> 8) & 0xFFu);
            setup.vs_exports_rt_index = ((val >> 19) & 1u) != 0u;
            setup.vs_exports_viewport_index = ((val >> 20) & 1u) != 0u;
            break;
        default:
            if (reg_dw >= kRegSpiPsInputCntl0 && reg_dw < kRegSpiPsInputCntl0 + 32u) {
                Rdna2PsInputCntl cntl{};
                cntl.attr_index = static_cast<uint8_t>(reg_dw - kRegSpiPsInputCntl0);
                cntl.offset = static_cast<uint8_t>(val & kSpiPsInputCntlOffsetMask);
                cntl.flat_shade = (val & kSpiPsInputCntlFlatShadeBit) != 0u;
                cntl.fp16_interp = (val & kSpiPsInputCntlFp16InterpBit) != 0u;
                cntl.raw_value = val;
                bool updated = false;
                for (auto& existing : setup.ps_input_cntls) {
                    if (existing.attr_index == cntl.attr_index) {
                        existing = cntl;
                        updated = true;
                        break;
                    }
                }
                if (!updated) {
                    setup.ps_input_cntls.push_back(cntl);
                }
            }
            break;
    }
}

void AppendPm4SetReg(std::vector<uint8_t>& out, uint32_t pm4_opcode, uint32_t base_dw,
                     uint32_t reg_dw, const std::vector<uint32_t>& vals) {
    if (vals.empty()) return;
    const uint32_t rel_off = (reg_dw >= base_dw) ? (reg_dw - base_dw) : reg_dw;
    const uint32_t count_field = static_cast<uint32_t>(vals.size());  // (1 + vals.size()) - 1
    const uint32_t hdr = kPm4PacketType3Mask | ((count_field & 0x3FFFu) << 16) | ((pm4_opcode & 0xFFu) << 8);
    AppendLe32(out, hdr);
    AppendLe32(out, rel_off);
    for (uint32_t v : vals) {
        AppendLe32(out, v);
    }
}

bool VisibilityMatchesStage(uint32_t vis, ShaderStage stage) noexcept {
    if (vis == 0u) return true;  // D3D12_SHADER_VISIBILITY_ALL
    switch (stage) {
        case ShaderStage::Vertex:        return vis == 1u;
        case ShaderStage::Hull:          return vis == 2u;
        case ShaderStage::Domain:        return vis == 3u;
        case ShaderStage::Geometry:      return vis == 4u;
        case ShaderStage::Pixel:         return vis == 5u;
        case ShaderStage::Amplification: return vis == 6u;
        case ShaderStage::Mesh:          return vis == 7u;
        case ShaderStage::Compute:
        default:                         return true;
    }
}

const char* ExtendedSop1Name(uint16_t raw_op) noexcept {
    switch (raw_op) {
        case kSop1RawMovB64:          return "s_mov_b64";
        case kSop1RawAndSaveExecB64:  return "s_and_saveexec_b64";
        case kSop1RawOrSaveExecB64:   return "s_or_saveexec_b64";
        case kSop1RawXorSaveExecB64:  return "s_xor_saveexec_b64";
        case kSop1RawAndN2SaveExecB64:return "s_andn2_saveexec_b64";
        case kSop1RawAndSaveExecB32:  return "s_and_saveexec_b32";
        case kSop1RawOrSaveExecB32:   return "s_or_saveexec_b32";
        case kSop1RawXorSaveExecB32:  return "s_xor_saveexec_b32";
        case kSop1RawAndN2SaveExecB32:return "s_andn2_saveexec_b32";
        default:                      return nullptr;
    }
}

const char* ExtendedSop2Name(uint16_t raw_op) noexcept {
    switch (raw_op) {
        case kSop2RawAndB64:   return "s_and_b64";
        case kSop2RawOrB64:    return "s_or_b64";
        case kSop2RawXorB64:   return "s_xor_b64";
        case kSop2RawAndN2B64: return "s_andn2_b64";
        default:               return nullptr;
    }
}

Rdna2Operand DecodeScalarDstOp(uint8_t sdst) {
    if (sdst == 106u) return Rdna2Operand::VccLo();
    if (sdst == 107u) return Rdna2Operand::VccHi();
    if (sdst == 124u) return Rdna2Operand::M0();
    if (sdst == 125u) return Rdna2Operand::Null();
    if (sdst == 126u) return Rdna2Operand::ExecLo();
    if (sdst == 127u) return Rdna2Operand::ExecHi();
    return Rdna2Operand::Sgpr(sdst);
}

Rdna2InterpMode ResolvePsInputInterpMode(
    const XboxSignatureParamDesc& param,
    const Rdna2PgmRegisterSetup& reg_setup) noexcept {
    if (param.component_type == 1u || param.component_type == 2u) {
        return Rdna2InterpMode::Flat;
    }
    if (param.interp_mode != Rdna2InterpMode::Perspective) {
        return param.interp_mode;
    }
    // Check per-attribute SPI_PS_INPUT_CNTL_n FLAT_SHADE bit first
    for (const auto& cntl : reg_setup.ps_input_cntls) {
        if (cntl.attr_index == param.register_index || cntl.offset == param.register_index) {
            if (cntl.flat_shade) {
                return Rdna2InterpMode::Flat;
            }
        }
    }
    // Fall back to global SPI_PS_INPUT_ENA interpolation enable bits
    if (reg_setup.present && reg_setup.spi_ps_input_ena != 0u) {
        const bool any_linear =
            reg_setup.linear_center_ena || reg_setup.linear_centroid_ena || reg_setup.linear_sample_ena;
        const bool any_persp =
            reg_setup.persp_center_ena || reg_setup.persp_centroid_ena || reg_setup.persp_sample_ena;
        if (any_linear && !any_persp) {
            return Rdna2InterpMode::NoPerspective;
        }
        if (reg_setup.persp_sample_ena && !reg_setup.persp_center_ena && !reg_setup.persp_centroid_ena) {
            return Rdna2InterpMode::Sample;
        }
        if (reg_setup.persp_centroid_ena && !reg_setup.persp_center_ena) {
            return Rdna2InterpMode::Centroid;
        }
    }
    return Rdna2InterpMode::Perspective;
}

}  // namespace

// ---------------------------------------------------------------------------
// Extended EXEC-Mask SOP1/SOP2 Emitter & Disassembler
// ---------------------------------------------------------------------------
void EmitRawExecSop1(std::vector<uint32_t>& words, uint16_t raw_op, uint8_t sdst, const Rdna2Operand& ssrc0) {
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    const uint32_t w0 = (0x17Du << 23) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(raw_op & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words.push_back(w0);
    if (need0) {
        words.push_back(lit0);
    }
}

void EmitRawExecSop2(std::vector<uint32_t>& words, uint16_t raw_op, uint8_t sdst,
                     const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1) {
    uint16_t c0 = 0, c1 = 0;
    uint32_t lit0 = 0, lit1 = 0;
    bool need0 = false, need1 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    EncodeRdna2SourceOperand(ssrc1, c1, lit1, need1);
    const uint32_t w0 = (0x2u << 30) |
                        (static_cast<uint32_t>(raw_op & 0x7Fu) << 23) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(c1 & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words.push_back(w0);
    if (need0) {
        words.push_back(lit0);
    } else if (need1) {
        words.push_back(lit1);
    }
}

void EmitRawBvh64IntersectRay(
    std::vector<uint32_t>& words,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc,
    uint8_t dmask) {
    // GFX10.3 / RDNA2 MIMG 64-bit encoding:
    // w0: [31:26] = 0x3C, [24:18] = raw_op[6:0], [11:8] = dmask, [0] = raw_op[7]
    // w1: [20:16] = srsrc (quad-SGPR index), [15:8] = vdata, [7:0] = vaddr
    const uint16_t raw_op = kMimgRawImageBvh64IntersectRay;
    const uint32_t w0 = (0x3Cu << 26) |
                        (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (static_cast<uint32_t>(dmask & 0xFu) << 8) |
                        static_cast<uint32_t>((raw_op >> 7) & 1u);
    const uint32_t w1 = (static_cast<uint32_t>(srsrc & 0x1Fu) << 16) |
                        (static_cast<uint32_t>(vdata) << 8) |
                        static_cast<uint32_t>(vaddr);
    words.push_back(w0);
    words.push_back(w1);
}

bool IsXboxShaderContainerBlob(const void* data, size_t size) noexcept {
    if (!data || size < 4u) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, "XS2\0", 4u) == 0) {
        return size >= kXs2HeaderSizeBytes;
    }
    if (size < 32u || std::memcmp(bytes, "DXBC", 4u) != 0) {
        return false;
    }
    const uint32_t total_size = ReadLe32(bytes + 24u);
    const uint32_t chunk_count = ReadLe32(bytes + 28u);
    if (total_size < 32u || total_size > size || chunk_count == 0u || chunk_count > 128u) {
        return false;
    }
    const uint64_t dir_end = 32ull + static_cast<uint64_t>(chunk_count) * 4ull;
    if (dir_end > total_size) return false;

    for (uint32_t i = 0u; i < chunk_count; ++i) {
        const uint32_t off = ReadLe32(bytes + 32u + static_cast<size_t>(i) * 4u);
        if (static_cast<uint64_t>(off) + 8ull > total_size) continue;
        if (std::memcmp(bytes + off, "XBOX", 4u) == 0 ||
            std::memcmp(bytes + off, "XS2M", 4u) == 0 ||
            std::memcmp(bytes + off, "RDNA", 4u) == 0 ||
            std::memcmp(bytes + off, "PM4R", 4u) == 0 ||
            std::memcmp(bytes + off, "RREG", 4u) == 0) {
            return true;
        }
    }
    return false;
}

Status DisassembleRdna2Extended(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions) {
    // Fast path: if standard DisassembleRdna2 succeeds, use it directly.
    if (auto st = DisassembleRdna2(words, out_instructions); st.ok()) {
        return st;
    }

    out_instructions.clear();
    if (words.empty()) {
        return Status::Error(StatusCode::ParseError, "Empty RDNA2 instruction stream");
    }

    size_t pc = 0u;
    while (pc < words.size()) {
        const uint32_t w0 = words[pc];

        // Check extended MIMG image_bvh64_intersect_ray (raw_op 0xE7)
        if ((w0 >> 26) == 0x3Cu) {
            const uint16_t raw_op = static_cast<uint16_t>(((w0 >> 18) & 0x7Fu) | ((w0 & 1u) << 7));
            if (raw_op == kMimgRawImageBvh64IntersectRay) {
                if (pc + 1u >= words.size()) {
                    return Status::Error(StatusCode::ParseError, "Truncated image_bvh64_intersect_ray instruction");
                }
                const uint32_t w1 = words[pc + 1u];
                Rdna2Instruction inst{};
                inst.word_offset = static_cast<uint32_t>(pc);
                inst.raw_words[0] = w0;
                inst.raw_words[1] = w1;
                inst.word_count = 2u;
                inst.encoding = Rdna2Encoding::MIMG;
                inst.opcode = Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY;
                inst.raw_opcode = raw_op;
                inst.slc = ((w0 >> 25) & 1u) != 0u;
                inst.glc = ((w0 >> 13) & 1u) != 0u;
                inst.mimg_unrm = ((w0 >> 12) & 1u) != 0u;
                inst.mimg_dmask = static_cast<uint8_t>((w0 >> 8) & 0xFu);
                inst.dlc = ((w0 >> 7) & 1u) != 0u;
                inst.mimg_dim = static_cast<uint8_t>((w0 >> 3) & 0x7u);
                inst.ssamp = static_cast<uint8_t>((w1 >> 21) & 0x1Fu);
                inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
                const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
                const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
                inst.dst = Rdna2Operand::Vgpr(vdata);
                inst.src[0] = Rdna2Operand::Vgpr(vaddr);
                inst.src_count = 1u;
                inst.operand_width = Rdna2OperandWidth::Bits64;
                inst.mnemonic = "image_bvh64_intersect_ray";
                inst.disasm = "image_bvh64_intersect_ray v" + std::to_string(vdata) +
                              ", v" + std::to_string(vaddr) + ", s[" +
                              std::to_string(static_cast<uint32_t>(inst.srsrc) * 4u) + ":" +
                              std::to_string(static_cast<uint32_t>(inst.srsrc) * 4u + 3u) + "]";
                out_instructions.push_back(std::move(inst));
                pc += 2u;
                continue;
            }
        }

        // Check extended SOP1 EXEC-mask opcodes
        if ((w0 >> 23) == 0x17Du) {
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            if (const char* name = ExtendedSop1Name(raw_op); name != nullptr) {
                const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
                const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
                Rdna2Instruction inst{};
                inst.word_offset = static_cast<uint32_t>(pc);
                inst.raw_words[0] = w0;
                inst.word_count = 1u;
                uint32_t lit = 0u;
                if (ssrc0 == 255u) {
                    if (pc + 1u >= words.size()) {
                        return Status::Error(StatusCode::ParseError, "Truncated extended SOP1 literal");
                    }
                    lit = words[pc + 1u];
                    inst.raw_words[1] = lit;
                    inst.word_count = 2u;
                }
                inst.encoding = Rdna2Encoding::SOP1;
                inst.opcode = Rdna2Opcode::Unknown;
                inst.raw_opcode = raw_op;
                inst.operand_width = (raw_op >= kSop1RawAndSaveExecB32)
                                         ? Rdna2OperandWidth::Bits32
                                         : Rdna2OperandWidth::Bits64;
                inst.dst = DecodeScalarDstOp(sdst);
                inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
                inst.src_count = 1u;
                inst.mnemonic = name;
                inst.disasm = std::string(name) + " " + FormatRdna2Operand(inst.dst) + ", " +
                              FormatRdna2Operand(inst.src[0]);
                out_instructions.push_back(std::move(inst));
                pc += out_instructions.back().word_count;
                continue;
            }
        }

        // Check extended SOP2 EXEC-mask opcodes
        if ((w0 >> 30) == 0x2u && (w0 >> 23) != 0x17Du && (w0 >> 23) != 0x17Eu &&
            (w0 >> 23) != 0x17Fu && (w0 >> 28) != 0xBu) {
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23) & 0x7Fu);
            if (const char* name = ExtendedSop2Name(raw_op); name != nullptr) {
                const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
                const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
                const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
                Rdna2Instruction inst{};
                inst.word_offset = static_cast<uint32_t>(pc);
                inst.raw_words[0] = w0;
                inst.word_count = 1u;
                uint32_t lit = 0u;
                if (ssrc0 == 255u || ssrc1 == 255u) {
                    if (pc + 1u >= words.size()) {
                        return Status::Error(StatusCode::ParseError, "Truncated extended SOP2 literal");
                    }
                    lit = words[pc + 1u];
                    inst.raw_words[1] = lit;
                    inst.word_count = 2u;
                }
                inst.encoding = Rdna2Encoding::SOP2;
                inst.opcode = Rdna2Opcode::Unknown;
                inst.raw_opcode = raw_op;
                inst.operand_width = Rdna2OperandWidth::Bits64;
                inst.dst = DecodeScalarDstOp(sdst);
                inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
                inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
                inst.src_count = 2u;
                inst.mnemonic = name;
                inst.disasm = std::string(name) + " " + FormatRdna2Operand(inst.dst) + ", " +
                              FormatRdna2Operand(inst.src[0]) + ", " + FormatRdna2Operand(inst.src[1]);
                out_instructions.push_back(std::move(inst));
                pc += out_instructions.back().word_count;
                continue;
            }
        }

        // Standard single instruction decode via DisassembleRdna2 on up to 3 words
        const size_t rem = words.size() - pc;
        bool decoded = false;
        Status last_err = Status::Ok();
        for (size_t try_len = 1u; try_len <= std::min<size_t>(3u, rem); ++try_len) {
            std::vector<uint32_t> slice(words.begin() + static_cast<std::ptrdiff_t>(pc),
                                        words.begin() + static_cast<std::ptrdiff_t>(pc + try_len));
            std::vector<Rdna2Instruction> single;
            last_err = DisassembleRdna2(slice, single);
            if (last_err.ok() && single.size() == 1u && single[0].word_count == try_len) {
                single[0].word_offset = static_cast<uint32_t>(pc);
                out_instructions.push_back(std::move(single[0]));
                pc += try_len;
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            return last_err.ok()
                       ? Status::Error(StatusCode::UnsupportedFormat, "Failed to decode RDNA2 instruction")
                       : last_err;
        }
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// RTS0 Root Signature Parser & Builder (v1.0 & v1.1)
// ---------------------------------------------------------------------------
Status ParseRts0RootSignature(const uint8_t* data, size_t size, XboxRootSignatureDesc& out_desc) {
    out_desc = XboxRootSignatureDesc{};
    if (!data || size < 24u) {
        return Status::Error(StatusCode::ParseError, "RTS0 chunk shorter than 24-byte header");
    }

    const uint32_t version = ReadLe32(data + 0u);
    if (version != 1u && version != 2u) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unsupported RTS0 root signature version: " + std::to_string(version));
    }

    const uint32_t num_params = ReadLe32(data + 4u);
    const uint32_t params_off = ReadLe32(data + 8u);
    const uint32_t num_samplers = ReadLe32(data + 12u);
    const uint32_t samplers_off = ReadLe32(data + 16u);
    const uint32_t flags = ReadLe32(data + 20u);

    if (num_params > 64u || num_samplers > 256u) {
        return Status::Error(StatusCode::ParseError, "RTS0 parameter or static sampler count out of range");
    }
    if (num_params > 0u &&
        static_cast<uint64_t>(params_off) + static_cast<uint64_t>(num_params) * 12ull > size) {
        return Status::Error(StatusCode::ParseError, "RTS0 parameter header table extends past chunk end");
    }
    if (num_samplers > 0u &&
        static_cast<uint64_t>(samplers_off) + static_cast<uint64_t>(num_samplers) * 52ull > size) {
        return Status::Error(StatusCode::ParseError, "RTS0 static sampler table extends past chunk end");
    }

    out_desc.present = true;
    out_desc.version = version;
    out_desc.flags = flags;
    out_desc.parameters.reserve(num_params);

    const bool is_v11 = (version == 2u);
    for (uint32_t i = 0u; i < num_params; ++i) {
        const uint8_t* ph = data + params_off + static_cast<size_t>(i) * 12u;
        const uint32_t ptype_raw = ReadLe32(ph + 0u);
        const uint32_t vis = ReadLe32(ph + 4u);
        const uint32_t data_off = ReadLe32(ph + 8u);

        if (ptype_raw > 4u) {
            return Status::Error(StatusCode::ParseError, "Invalid RTS0 root parameter type");
        }
        XboxRootParameter p{};
        p.type = static_cast<XboxRootParamType>(ptype_raw);
        p.shader_visibility = vis;

        if (p.type == XboxRootParamType::Constants32Bit) {
            if (static_cast<uint64_t>(data_off) + 12ull > size) {
                return Status::Error(StatusCode::ParseError, "RTS0 root constants payload out of bounds");
            }
            p.shader_register = ReadLe32(data + data_off + 0u);
            p.register_space = ReadLe32(data + data_off + 4u);
            p.num_32bit_values = ReadLe32(data + data_off + 8u);
        } else if (p.type == XboxRootParamType::Cbv ||
                   p.type == XboxRootParamType::Srv ||
                   p.type == XboxRootParamType::Uav) {
            const uint64_t rec_sz = is_v11 ? 12ull : 8ull;
            if (static_cast<uint64_t>(data_off) + rec_sz > size) {
                return Status::Error(StatusCode::ParseError, "RTS0 root descriptor payload out of bounds");
            }
            p.shader_register = ReadLe32(data + data_off + 0u);
            p.register_space = ReadLe32(data + data_off + 4u);
            p.descriptor_flags = is_v11 ? ReadLe32(data + data_off + 8u) : 0u;
        } else if (p.type == XboxRootParamType::DescriptorTable) {
            if (static_cast<uint64_t>(data_off) + 8ull > size) {
                return Status::Error(StatusCode::ParseError, "RTS0 descriptor table header out of bounds");
            }
            const uint32_t num_ranges = ReadLe32(data + data_off + 0u);
            const uint32_t ranges_off = ReadLe32(data + data_off + 4u);
            const uint64_t range_stride = is_v11 ? 24ull : 20ull;
            if (num_ranges > 256u ||
                static_cast<uint64_t>(ranges_off) + static_cast<uint64_t>(num_ranges) * range_stride > size) {
                return Status::Error(StatusCode::ParseError, "RTS0 descriptor range array out of bounds");
            }
            p.descriptor_ranges.reserve(num_ranges);
            for (uint32_t r = 0u; r < num_ranges; ++r) {
                const uint8_t* rp = data + ranges_off + static_cast<size_t>(r) * static_cast<size_t>(range_stride);
                XboxDescriptorRange dr{};
                dr.range_type = static_cast<XboxDescriptorRangeType>(ReadLe32(rp + 0u));
                dr.num_descriptors = ReadLe32(rp + 4u);
                dr.base_shader_register = ReadLe32(rp + 8u);
                dr.register_space = ReadLe32(rp + 12u);
                if (is_v11) {
                    dr.flags = ReadLe32(rp + 16u);
                    dr.offset_in_descriptors_from_table_start = ReadLe32(rp + 20u);
                } else {
                    dr.flags = 0u;
                    dr.offset_in_descriptors_from_table_start = ReadLe32(rp + 16u);
                }
                p.descriptor_ranges.push_back(dr);
            }
        }
        out_desc.parameters.push_back(std::move(p));
    }

    out_desc.static_samplers.reserve(num_samplers);
    for (uint32_t i = 0u; i < num_samplers; ++i) {
        XboxStaticSamplerInfo s{};
        ReadStaticSampler(data + samplers_off + static_cast<size_t>(i) * 52u, s);
        out_desc.static_samplers.push_back(s);
    }

    return Status::Ok();
}

std::vector<uint8_t> BuildRts0RootSignature(const XboxRootSignatureDesc& desc) {
    const uint32_t version = (desc.version == 2u) ? 2u : 1u;
    const bool is_v11 = (version == 2u);
    const uint32_t num_params = static_cast<uint32_t>(desc.parameters.size());
    const uint32_t num_samplers = static_cast<uint32_t>(desc.static_samplers.size());

    std::vector<uint8_t> out;
    out.reserve(256u);

    // 24-byte header
    AppendLe32(out, version);
    AppendLe32(out, num_params);
    AppendLe32(out, 24u);  // params_offset
    AppendLe32(out, num_samplers);
    AppendLe32(out, 0u);   // samplers_offset placeholder (patched at byte 16)
    AppendLe32(out, desc.flags);

    // Reserve parameter header slots (12 bytes each)
    const size_t param_hdr_base = out.size();
    for (uint32_t i = 0u; i < num_params; ++i) {
        AppendLe32(out, static_cast<uint32_t>(desc.parameters[i].type));
        AppendLe32(out, desc.parameters[i].shader_visibility);
        AppendLe32(out, 0u);  // data_offset placeholder
    }

    // Write parameter payloads
    for (uint32_t i = 0u; i < num_params; ++i) {
        const auto& p = desc.parameters[i];
        const uint32_t data_off = static_cast<uint32_t>(out.size());
        PatchLe32(out, param_hdr_base + static_cast<size_t>(i) * 12u + 8u, data_off);

        if (p.type == XboxRootParamType::Constants32Bit) {
            AppendLe32(out, p.shader_register);
            AppendLe32(out, p.register_space);
            AppendLe32(out, p.num_32bit_values);
        } else if (p.type == XboxRootParamType::Cbv ||
                   p.type == XboxRootParamType::Srv ||
                   p.type == XboxRootParamType::Uav) {
            AppendLe32(out, p.shader_register);
            AppendLe32(out, p.register_space);
            if (is_v11) {
                AppendLe32(out, p.descriptor_flags);
            }
        } else if (p.type == XboxRootParamType::DescriptorTable) {
            const uint32_t num_ranges = static_cast<uint32_t>(p.descriptor_ranges.size());
            AppendLe32(out, num_ranges);
            const uint32_t ranges_off = static_cast<uint32_t>(out.size() + 4u);
            AppendLe32(out, ranges_off);
            for (const auto& dr : p.descriptor_ranges) {
                AppendLe32(out, static_cast<uint32_t>(dr.range_type));
                AppendLe32(out, dr.num_descriptors);
                AppendLe32(out, dr.base_shader_register);
                AppendLe32(out, dr.register_space);
                if (is_v11) {
                    AppendLe32(out, dr.flags);
                }
                AppendLe32(out, dr.offset_in_descriptors_from_table_start);
            }
        }
    }

    // Write static samplers
    const uint32_t samplers_off = static_cast<uint32_t>(out.size());
    PatchLe32(out, 16u, samplers_off);
    for (const auto& s : desc.static_samplers) {
        AppendStaticSampler(out, s);
    }

    return out;
}

// ---------------------------------------------------------------------------
// PM4 / SH & Context Register Setup Block Parser & Builder
// ---------------------------------------------------------------------------
Status ParsePm4RegisterBlock(const uint8_t* data, size_t size, Rdna2PgmRegisterSetup& out_setup) {
    out_setup = Rdna2PgmRegisterSetup{};
    if (!data || size < 8u || (size & 3u) != 0u) {
        return Status::Error(StatusCode::ParseError, "PM4 register block is empty or unaligned");
    }

    const size_t total_words = size / 4u;

    // Format A: Structured pair block magic "RREG" + pair_count + (reg_dw, val)*
    if (size >= 8u && std::memcmp(data, "RREG", 4u) == 0) {
        const uint32_t count = ReadLe32(data + 4u);
        if (8u + static_cast<size_t>(count) * 8u > size) {
            return Status::Error(StatusCode::ParseError, "Truncated RREG register-pair block");
        }
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t reg = NormalizeRegDwordOffset(ReadLe32(data + 8u + static_cast<size_t>(i) * 8u));
            const uint32_t val = ReadLe32(data + 12u + static_cast<size_t>(i) * 8u);
            ApplyRegisterWrite(reg, val, out_setup);
        }
        return Status::Ok();
    }

    // Format B: Standard AMD PM4 Type-3 packet stream (SET_SH_REG 0x76 / SET_CONTEXT_REG 0x69 / NOP 0x10)
    size_t pos = 0u;
    bool saw_pm4 = false;
    while (pos < total_words) {
        const uint32_t hdr = ReadLe32(data + pos * 4u);
        if ((hdr & kPm4PacketType3Mask) != kPm4PacketType3Mask) {
            break;
        }
        const uint32_t count_m1 = (hdr >> 16) & 0x3FFFu;
        const uint32_t opcode = (hdr >> 8) & 0xFFu;
        const size_t payload_words = static_cast<size_t>(count_m1) + 1u;
        if (pos + 1u + payload_words > total_words) {
            return Status::Error(StatusCode::ParseError, "Truncated PM4 Type-3 packet in register block");
        }
        saw_pm4 = true;
        if (opcode == kPm4ItSetShReg || opcode == kPm4ItSetContextReg) {
            const uint32_t base = (opcode == kPm4ItSetShReg) ? kRdna2ShRegBaseDword : kRdna2ContextRegBaseDword;
            const uint32_t rel_reg = ReadLe32(data + (pos + 1u) * 4u) & 0xFFFFu;
            const uint32_t start_reg = base + rel_reg;
            for (size_t k = 1u; k < payload_words; ++k) {
                const uint32_t val = ReadLe32(data + (pos + 1u + k) * 4u);
                ApplyRegisterWrite(start_reg + static_cast<uint32_t>(k - 1u), val, out_setup);
            }
        }
        pos += 1u + payload_words;
    }

    if (saw_pm4) {
        return Status::Ok();
    }

    // Format C: Raw (reg_offset, value) dword pairs fallback
    if ((total_words & 1u) != 0u) {
        return Status::Error(StatusCode::ParseError, "Invalid raw register-pair block word count");
    }
    for (size_t i = 0u; i < total_words; i += 2u) {
        const uint32_t reg = NormalizeRegDwordOffset(ReadLe32(data + i * 4u));
        const uint32_t val = ReadLe32(data + (i + 1u) * 4u);
        ApplyRegisterWrite(reg, val, out_setup);
    }
    return Status::Ok();
}

std::vector<uint8_t> BuildPm4RegisterBlock(const Rdna2PgmRegisterSetup& setup, ShaderStage stage) {
    std::vector<uint8_t> out;
    out.reserve(128u);

    const bool is_wave64 = (setup.wave_size == 64u);
    const uint32_t vgpr_gran = is_wave64 ? 4u : 8u;
    const uint32_t vgprs_field =
        setup.vgpr_count > 0u ? std::min(63u, (setup.vgpr_count + vgpr_gran - 1u) / vgpr_gran - 1u) : 0u;
    const uint32_t sgprs_field =
        setup.sgpr_count > 0u ? std::min(15u, (setup.sgpr_count + 7u) / 8u - 1u) : 0u;

    const uint32_t rsrc1 = (setup.pgm_rsrc1 != 0u)
                               ? setup.pgm_rsrc1
                               : (vgprs_field | (sgprs_field << 6) | (setup.wgp_mode ? (1u << 29) : 0u));

    uint32_t rsrc2 = setup.pgm_rsrc2;
    if (rsrc2 == 0u) {
        const uint32_t lds_blocks = (setup.lds_size_bytes + 511u) / 512u;
        rsrc2 = (setup.scratch_enabled ? 1u : 0u) | ((setup.user_sgpr_count & 0x1Fu) << 1);
        if (stage == ShaderStage::Compute) {
            rsrc2 |= (lds_blocks & 0x1FFu) << 15;
        } else {
            rsrc2 |= (lds_blocks & 0xFFu) << 24;
        }
    }

    uint32_t rsrc3 = setup.pgm_rsrc3;
    if (is_wave64) {
        rsrc3 |= (1u << 31);
    }

    if (stage == ShaderStage::Compute) {
        AppendPm4SetReg(out, kPm4ItSetShReg, kRdna2ShRegBaseDword, kRegComputePgmRsrc1, {rsrc1, rsrc2});
        if (rsrc3 != 0u) {
            AppendPm4SetReg(out, kPm4ItSetShReg, kRdna2ShRegBaseDword, kRegComputePgmRsrc3, {rsrc3});
        }
        if (setup.threadgroup_x > 0u || setup.threadgroup_y > 0u || setup.threadgroup_z > 0u) {
            AppendPm4SetReg(out, kPm4ItSetShReg, kRdna2ShRegBaseDword, kRegComputeNumThreadX,
                            {setup.threadgroup_x ? setup.threadgroup_x : 1u,
                             setup.threadgroup_y ? setup.threadgroup_y : 1u,
                             setup.threadgroup_z ? setup.threadgroup_z : 1u});
        }
    } else {
        uint32_t r1_reg = kRegSpiShaderPgmRsrc1Vs;
        uint32_t r3_reg = kRegSpiShaderPgmRsrc3Vs;
        if (stage == ShaderStage::Pixel) {
            r1_reg = kRegSpiShaderPgmRsrc1Ps;
            r3_reg = kRegSpiShaderPgmRsrc3Ps;
        } else if (stage == ShaderStage::Geometry || stage == ShaderStage::Mesh) {
            r1_reg = kRegSpiShaderPgmRsrc1Gs;
            r3_reg = kRegSpiShaderPgmRsrc3Gs;
        } else if (stage == ShaderStage::Hull || stage == ShaderStage::Amplification) {
            r1_reg = kRegSpiShaderPgmRsrc1Hs;
            r3_reg = kRegSpiShaderPgmRsrc3Hs;
        }
        AppendPm4SetReg(out, kPm4ItSetShReg, kRdna2ShRegBaseDword, r1_reg, {rsrc1, rsrc2});
        if (rsrc3 != 0u) {
            AppendPm4SetReg(out, kPm4ItSetShReg, kRdna2ShRegBaseDword, r3_reg, {rsrc3});
        }
        if ((stage == ShaderStage::Mesh || stage == ShaderStage::Amplification) &&
            (setup.threadgroup_x > 0u || setup.threadgroup_y > 0u || setup.threadgroup_z > 0u)) {
            AppendPm4SetReg(out, kPm4ItSetShReg, kRdna2ShRegBaseDword, kRegComputeNumThreadX,
                            {setup.threadgroup_x ? setup.threadgroup_x : 1u,
                             setup.threadgroup_y ? setup.threadgroup_y : 1u,
                             setup.threadgroup_z ? setup.threadgroup_z : 1u});
        }
    }

    if (setup.spi_ps_input_ena != 0u || setup.spi_ps_input_addr != 0u) {
        const uint32_t addr = setup.spi_ps_input_addr ? setup.spi_ps_input_addr : setup.spi_ps_input_ena;
        AppendPm4SetReg(out, kPm4ItSetContextReg, kRdna2ContextRegBaseDword,
                        kRegSpiPsInputEna, {setup.spi_ps_input_ena, addr});
    }
    for (const auto& cntl : setup.ps_input_cntls) {
        const uint32_t raw = cntl.raw_value
                                 ? cntl.raw_value
                                 : (static_cast<uint32_t>(cntl.offset & kSpiPsInputCntlOffsetMask) |
                                    (cntl.flat_shade ? kSpiPsInputCntlFlatShadeBit : 0u) |
                                    (cntl.fp16_interp ? kSpiPsInputCntlFp16InterpBit : 0u));
        AppendPm4SetReg(out, kPm4ItSetContextReg, kRdna2ContextRegBaseDword,
                        kRegSpiPsInputCntl0 + cntl.attr_index, {raw});
    }
    if (setup.pa_cl_vs_out_cntl != 0u) {
        AppendPm4SetReg(out, kPm4ItSetContextReg, kRdna2ContextRegBaseDword,
                        kRegPaClVsOutCntl, {setup.pa_cl_vs_out_cntl});
    }

    return out;
}

// ---------------------------------------------------------------------------
// User-Data SGPR (s0..s31) Root-Binding Map Builder
// ---------------------------------------------------------------------------
Status BuildUserSgprRootMap(
    const XboxRootSignatureDesc& root_sig,
    const Rdna2PgmRegisterSetup* reg_setup,
    ShaderStage stage,
    XboxRootSgprMap& out_map,
    uint32_t max_inline_user_sgprs) {
    out_map = XboxRootSgprMap{};
    out_map.stage = stage;
    const uint32_t clamped_max = std::clamp(max_inline_user_sgprs, 4u, 32u);
    out_map.max_inline_user_sgprs = clamped_max;
    out_map.static_samplers = root_sig.static_samplers;

    uint32_t next_sgpr = 0u;
    const bool scratch_en = (reg_setup != nullptr && reg_setup->present && reg_setup->scratch_enabled);
    if (scratch_en) {
        // Reserve s[0:1] for flat scratch base pointer when SCRATCH_EN == 1
        out_map.uses_scratch_sgpr = true;
        out_map.slot_kinds[0] = XboxUserSgprSlotKind::ScratchBasePtrLo;
        out_map.slot_kinds[1] = XboxUserSgprSlotKind::ScratchBasePtrHi;
        XboxUserSgprBinding sb{};
        sb.sgpr_index = 0u;
        sb.sgpr_count = 2u;
        sb.root_param_index = 0xFFFFFFFFu;
        sb.kind = XboxUserSgprSlotKind::ScratchBasePtrLo;
        out_map.bindings.push_back(std::move(sb));
        next_sgpr = 2u;
    }

    // Reserve 2 SGPRs for Vertex Buffer Table pointer on Vertex shaders with IA input layout flag
    const bool has_ia_layout = (root_sig.flags & 0x1u) != 0u;
    if (stage == ShaderStage::Vertex && has_ia_layout && next_sgpr + 2u <= clamped_max) {
        out_map.uses_vertex_buffer_table = true;
        out_map.vertex_buffer_table_sgpr = next_sgpr;
        out_map.slot_kinds[next_sgpr + 0u] = XboxUserSgprSlotKind::VertexBufferTablePtrLo;
        out_map.slot_kinds[next_sgpr + 1u] = XboxUserSgprSlotKind::VertexBufferTablePtrHi;
        XboxUserSgprBinding vb{};
        vb.sgpr_index = next_sgpr;
        vb.sgpr_count = 2u;
        vb.root_param_index = 0xFFFFFFFFu;
        vb.kind = XboxUserSgprSlotKind::VertexBufferTablePtrLo;
        out_map.bindings.push_back(std::move(vb));
        next_sgpr += 2u;
    }

    // Pre-calculate total dwords required by visible root parameters
    uint32_t needed_dwords = 0u;
    for (const auto& p : root_sig.parameters) {
        if (!VisibilityMatchesStage(p.shader_visibility, stage)) continue;
        if (p.type == XboxRootParamType::Constants32Bit) {
            needed_dwords += p.num_32bit_values;
        } else if (p.type == XboxRootParamType::DescriptorTable) {
            needed_dwords += 1u;  // 32-bit descriptor-heap-relative table pointer
        } else {
            needed_dwords += 2u;  // 64-bit GPU VA root descriptor (CBV/SRV/UAV)
        }
    }

    const bool will_spill = (next_sgpr + needed_dwords > clamped_max);
    const uint32_t inline_limit = will_spill ? (clamped_max - 2u) : clamped_max;
    uint32_t next_spill_dw = 0u;

    auto allocate_slot = [&](uint32_t root_idx, const XboxRootParameter& p) {
        if (p.type == XboxRootParamType::Constants32Bit) {
            for (uint32_t k = 0u; k < p.num_32bit_values; ++k) {
                XboxUserSgprBinding b{};
                b.root_param_index = root_idx;
                b.kind = XboxUserSgprSlotKind::RootConstant32;
                b.shader_register = p.shader_register;
                b.register_space = p.register_space;
                b.const_dword_offset = k;
                b.sgpr_count = 1u;
                if (next_sgpr < inline_limit) {
                    b.sgpr_index = next_sgpr;
                    out_map.slot_kinds[next_sgpr] = XboxUserSgprSlotKind::RootConstant32;
                    ++next_sgpr;
                } else {
                    b.spilled_to_table = true;
                    b.sgpr_index = inline_limit;
                    b.spill_dword_offset = next_spill_dw++;
                }
                out_map.bindings.push_back(std::move(b));
            }
        } else if (p.type == XboxRootParamType::DescriptorTable) {
            XboxUserSgprBinding b{};
            b.root_param_index = root_idx;
            b.kind = XboxUserSgprSlotKind::DescriptorTablePtr32;
            b.sgpr_count = 1u;
            b.descriptor_ranges = p.descriptor_ranges;
            if (!p.descriptor_ranges.empty()) {
                b.shader_register = p.descriptor_ranges[0].base_shader_register;
                b.register_space = p.descriptor_ranges[0].register_space;
            }
            if (next_sgpr < inline_limit) {
                b.sgpr_index = next_sgpr;
                out_map.slot_kinds[next_sgpr] = XboxUserSgprSlotKind::DescriptorTablePtr32;
                ++next_sgpr;
            } else {
                b.spilled_to_table = true;
                b.sgpr_index = inline_limit;
                b.spill_dword_offset = next_spill_dw++;
            }
            out_map.bindings.push_back(std::move(b));
        } else {
            XboxUserSgprSlotKind lo_kind = XboxUserSgprSlotKind::RootCbvPtrLo;
            XboxUserSgprSlotKind hi_kind = XboxUserSgprSlotKind::RootCbvPtrHi;
            if (p.type == XboxRootParamType::Srv) {
                lo_kind = XboxUserSgprSlotKind::RootSrvPtrLo;
                hi_kind = XboxUserSgprSlotKind::RootSrvPtrHi;
            } else if (p.type == XboxRootParamType::Uav) {
                lo_kind = XboxUserSgprSlotKind::RootUavPtrLo;
                hi_kind = XboxUserSgprSlotKind::RootUavPtrHi;
            }
            XboxUserSgprBinding b{};
            b.root_param_index = root_idx;
            b.kind = lo_kind;
            b.shader_register = p.shader_register;
            b.register_space = p.register_space;
            b.sgpr_count = 2u;
            if (next_sgpr + 2u <= inline_limit) {
                b.sgpr_index = next_sgpr;
                out_map.slot_kinds[next_sgpr + 0u] = lo_kind;
                out_map.slot_kinds[next_sgpr + 1u] = hi_kind;
                next_sgpr += 2u;
            } else {
                b.spilled_to_table = true;
                b.sgpr_index = inline_limit;
                b.spill_dword_offset = next_spill_dw;
                next_spill_dw += 2u;
            }
            out_map.bindings.push_back(std::move(b));
        }
    };

    for (uint32_t i = 0u; i < static_cast<uint32_t>(root_sig.parameters.size()); ++i) {
        if (!VisibilityMatchesStage(root_sig.parameters[i].shader_visibility, stage)) continue;
        allocate_slot(i, root_sig.parameters[i]);
    }

    if (will_spill && next_spill_dw > 0u) {
        out_map.uses_spill_table = true;
        out_map.spill_table_sgpr = inline_limit;
        out_map.spill_table_dwords = next_spill_dw;
        out_map.slot_kinds[inline_limit + 0u] = XboxUserSgprSlotKind::SpillTablePtrLo;
        out_map.slot_kinds[inline_limit + 1u] = XboxUserSgprSlotKind::SpillTablePtrHi;
        next_sgpr = std::max(next_sgpr, inline_limit + 2u);
    }

    out_map.total_user_sgprs = next_sgpr;
    if (reg_setup != nullptr && reg_setup->present && reg_setup->user_sgpr_count > out_map.total_user_sgprs) {
        out_map.total_user_sgprs = std::min(32u, reg_setup->user_sgpr_count);
    }
    return Status::Ok();
}

Status ApplyRootSgprMapToDescriptorBridge(
    const XboxRootSgprMap& map,
    const std::vector<uint32_t>& root_constant_values,
    const std::vector<uint64_t>& root_descriptor_vas,
    DescriptorSgprBridge& bridge) {
    size_t const_cursor = 0u;
    size_t va_cursor = 0u;

    for (const auto& b : map.bindings) {
        if (b.spilled_to_table) continue;
        if (b.sgpr_index >= DescriptorSgprBridge::kRootSgprCount) continue;

        if (b.kind == XboxUserSgprSlotKind::RootConstant32) {
            const uint32_t val =
                (const_cursor < root_constant_values.size()) ? root_constant_values[const_cursor] : 0u;
            ++const_cursor;
            if (auto st = bridge.SetRootSgpr(b.sgpr_index, val, RootSgprKind::InlineConstant32); !st.ok()) {
                return st;
            }
        } else if (b.kind == XboxUserSgprSlotKind::DescriptorTablePtr32) {
            const uint64_t va = (va_cursor < root_descriptor_vas.size()) ? root_descriptor_vas[va_cursor] : 0ull;
            ++va_cursor;
            if (auto st = bridge.SetRootSgpr(b.sgpr_index, static_cast<uint32_t>(va & 0xFFFFFFFFull),
                                             RootSgprKind::TablePtrLo);
                !st.ok()) {
                return st;
            }
        } else if (b.sgpr_count == 2u && b.sgpr_index + 1u < DescriptorSgprBridge::kRootSgprCount) {
            const uint64_t va = (va_cursor < root_descriptor_vas.size()) ? root_descriptor_vas[va_cursor] : 0ull;
            ++va_cursor;
            if (auto st = bridge.SetRootSgprTablePtr(b.sgpr_index, va); !st.ok()) {
                return st;
            }
        }
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Full Xbox DXBC Container Parser & Builder
// ---------------------------------------------------------------------------
namespace {

bool TryExtractPm4AndIsaFromRdnaChunk(
    const std::vector<uint8_t>& chunk_data,
    Rdna2PgmRegisterSetup& out_reg_setup,
    std::vector<uint32_t>& out_isa_words,
    Xs2ShaderContainer& out_xs2,
    bool& out_has_xs2) {
    // 1. Embedded XS2\0 container directly or after a PM4 register prefix
    if (chunk_data.size() >= kXs2HeaderSizeBytes && std::memcmp(chunk_data.data(), "XS2\0", 4u) == 0) {
        if (ParseXs2Container(chunk_data, out_xs2).ok()) {
            out_has_xs2 = true;
            out_isa_words = out_xs2.code_words;
            return true;
        }
    }

    // 2. "RDNA" structured payload:
    //    [0..3]: "RDNA" magic
    //    [4..7]: stage (ShaderStage uint32)
    //    [8..11]: pm4_byte_size
    //    [12..15]: isa_byte_size
    //    [16 .. 16+pm4_byte_size): PM4 register setup block
    //    [16+pm4_byte_size .. +isa_byte_size): XS2\0 OR raw RDNA2 32-bit ISA words
    if (chunk_data.size() >= 16u && std::memcmp(chunk_data.data(), "RDNA", 4u) == 0) {
        const uint32_t stage_raw = ReadLe32(chunk_data.data() + 4u);
        const uint32_t pm4_sz = ReadLe32(chunk_data.data() + 8u);
        const uint32_t isa_sz = ReadLe32(chunk_data.data() + 12u);
        if (16ull + static_cast<uint64_t>(pm4_sz) + static_cast<uint64_t>(isa_sz) <= chunk_data.size()) {
            if (pm4_sz >= 8u) {
                (void)ParsePm4RegisterBlock(chunk_data.data() + 16u, pm4_sz, out_reg_setup);
            }
            const uint8_t* isa_ptr = chunk_data.data() + 16u + pm4_sz;
            if (isa_sz >= kXs2HeaderSizeBytes && std::memcmp(isa_ptr, "XS2\0", 4u) == 0) {
                std::vector<uint8_t> xs2_slice(isa_ptr, isa_ptr + isa_sz);
                if (ParseXs2Container(xs2_slice, out_xs2).ok()) {
                    out_has_xs2 = true;
                    out_isa_words = out_xs2.code_words;
                    return true;
                }
            }
            if (isa_sz >= 4u && (isa_sz & 3u) == 0u) {
                out_isa_words.resize(isa_sz / 4u);
                for (size_t i = 0u; i < out_isa_words.size(); ++i) {
                    out_isa_words[i] = ReadLe32(isa_ptr + i * 4u);
                }
                out_xs2 = Xs2ShaderContainer{};
                out_xs2.stage = static_cast<ShaderStage>(stage_raw);
                out_xs2.code_words = out_isa_words;
                out_xs2.vgpr_count = out_reg_setup.vgpr_count ? out_reg_setup.vgpr_count : 16u;
                out_xs2.sgpr_count = out_reg_setup.sgpr_count ? out_reg_setup.sgpr_count : 16u;
                out_xs2.wave_size = out_reg_setup.wave_size ? out_reg_setup.wave_size : 32u;
                out_xs2.threadgroup_x = out_reg_setup.threadgroup_x ? out_reg_setup.threadgroup_x : 1u;
                out_xs2.threadgroup_y = out_reg_setup.threadgroup_y ? out_reg_setup.threadgroup_y : 1u;
                out_xs2.threadgroup_z = out_reg_setup.threadgroup_z ? out_reg_setup.threadgroup_z : 1u;
                out_has_xs2 = true;
                return true;
            }
        }
    }

    // 3. Scan for embedded "XS2\0" preceded by a PM4 block
    for (size_t off = 4u; off + kXs2HeaderSizeBytes <= chunk_data.size(); off += 4u) {
        if (std::memcmp(chunk_data.data() + off, "XS2\0", 4u) == 0) {
            std::vector<uint8_t> slice(chunk_data.begin() + static_cast<std::ptrdiff_t>(off), chunk_data.end());
            if (ParseXs2Container(slice, out_xs2).ok()) {
                out_has_xs2 = true;
                out_isa_words = out_xs2.code_words;
                if (off >= 8u && !out_reg_setup.present) {
                    (void)ParsePm4RegisterBlock(chunk_data.data(), off, out_reg_setup);
                }
                return true;
            }
        }
    }

    return false;
}

}  // namespace

Status ParseFullXboxShaderContainer(const void* data, size_t size, XboxParsedShaderContainer& out_parsed) {
    out_parsed = XboxParsedShaderContainer{};
    if (!data || size < 4u) {
        return Status::Error(StatusCode::ParseError, "Shader container buffer is empty or truncated");
    }
    const auto* bytes = static_cast<const uint8_t*>(data);

    // Raw XS2\0 fast path
    if (std::memcmp(bytes, "XS2\0", 4u) == 0) {
        std::vector<uint8_t> raw(bytes, bytes + size);
        if (auto st = ParseXs2Container(raw, out_parsed.xs2); !st.ok()) {
            return st;
        }
        out_parsed.format = XboxShaderBlobFormat::RawXs2;
        out_parsed.has_xs2 = true;
        out_parsed.has_raw_rdna_isa = !out_parsed.xs2.code_words.empty();
        out_parsed.rdna_isa_words = out_parsed.xs2.code_words;
        out_parsed.shader_payload_fourcc = "XS2";
        (void)InferShaderRootBindingsWhenRts0Absent(out_parsed);
        return Status::Ok();
    }

    if (size < 32u || std::memcmp(bytes, "DXBC", 4u) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unrecognized shader container magic (expected 'XS2\\0' or 'DXBC')");
    }

    std::memcpy(out_parsed.header.md5_digest, bytes + 4u, 16u);
    out_parsed.header.version = ReadLe32(bytes + 20u);
    out_parsed.header.total_size = ReadLe32(bytes + 24u);
    out_parsed.header.chunk_count = ReadLe32(bytes + 28u);
    out_parsed.header.md5_valid = VerifyDxbcContainerHash(bytes, size);

    if (out_parsed.header.version == 0u || out_parsed.header.total_size < 32u ||
        out_parsed.header.total_size > size) {
        return Status::Error(StatusCode::ParseError, "Invalid DXBC header size/version");
    }
    if (out_parsed.header.chunk_count == 0u || out_parsed.header.chunk_count > 128u) {
        return Status::Error(StatusCode::ParseError, "Invalid DXBC chunk count");
    }
    const uint64_t dir_end = 32ull + static_cast<uint64_t>(out_parsed.header.chunk_count) * 4ull;
    if (dir_end > out_parsed.header.total_size) {
        return Status::Error(StatusCode::ParseError, "DXBC chunk offset directory extends past container end");
    }

    // Parse base PSV0 / chunk metadata too
    XboxDxbcContainer base_info{};
    (void)ParseXboxShaderContainer(data, size, base_info);
    out_parsed.psv0 = base_info.psv0;

    out_parsed.chunks.reserve(out_parsed.header.chunk_count);
    for (uint32_t i = 0u; i < out_parsed.header.chunk_count; ++i) {
        const uint32_t off = ReadLe32(bytes + 32u + static_cast<size_t>(i) * 4u);
        if (static_cast<uint64_t>(off) + 8ull > out_parsed.header.total_size) {
            return Status::Error(StatusCode::ParseError, "DXBC chunk header out of bounds");
        }
        const uint32_t chunk_sz = ReadLe32(bytes + off + 4u);
        if (static_cast<uint64_t>(off) + 8ull + static_cast<uint64_t>(chunk_sz) > out_parsed.header.total_size) {
            return Status::Error(StatusCode::ParseError, "DXBC chunk payload out of bounds");
        }

        XboxDxbcChunk chunk{};
        std::memcpy(chunk.fourcc, bytes + off, 4u);
        chunk.fourcc[4] = '\0';
        chunk.data.assign(bytes + off + 8u, bytes + off + 8u + chunk_sz);

        if (std::memcmp(chunk.fourcc, "XBOX", 4u) == 0 ||
            std::memcmp(chunk.fourcc, "XS2M", 4u) == 0 ||
            std::memcmp(chunk.fourcc, "RDNA", 4u) == 0) {
            if (TryExtractPm4AndIsaFromRdnaChunk(chunk.data, out_parsed.reg_setup,
                                                 out_parsed.rdna_isa_words, out_parsed.xs2,
                                                 out_parsed.has_xs2)) {
                out_parsed.has_raw_rdna_isa = !out_parsed.rdna_isa_words.empty();
                out_parsed.shader_payload_fourcc = chunk.fourcc;
            }
        } else if (std::memcmp(chunk.fourcc, "SHDR", 4u) == 0 ||
                   std::memcmp(chunk.fourcc, "SHEX", 4u) == 0 ||
                   std::memcmp(chunk.fourcc, "DXIL", 4u) == 0) {
            out_parsed.shdr_or_shex_payload = chunk.data;
            if (out_parsed.shader_payload_fourcc.empty()) {
                out_parsed.shader_payload_fourcc = chunk.fourcc;
            }
        } else if (std::memcmp(chunk.fourcc, "PM4R", 4u) == 0 ||
                   std::memcmp(chunk.fourcc, "RREG", 4u) == 0) {
            if (auto st = ParsePm4RegisterBlock(chunk.data.data(), chunk.data.size(), out_parsed.reg_setup);
                !st.ok()) {
                return st;
            }
        } else if (std::memcmp(chunk.fourcc, "ISG1", 4u) == 0) {
            out_parsed.input_sig_element_stride =
                ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 32u, out_parsed.input_signature, false);
            if (out_parsed.input_sig_element_stride == 0u) {
                return Status::Error(StatusCode::ParseError,
                                     "Incomplete or truncated DXBC ISG1 input signature chunk");
            }
        } else if (std::memcmp(chunk.fourcc, "ISGN", 4u) == 0 && out_parsed.input_signature.empty()) {
            out_parsed.input_sig_element_stride =
                ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 24u, out_parsed.input_signature, false);
            if (out_parsed.input_sig_element_stride == 0u) {
                return Status::Error(StatusCode::ParseError,
                                     "Incomplete or truncated DXBC ISGN input signature chunk");
            }
        } else if (std::memcmp(chunk.fourcc, "OSG1", 4u) == 0) {
            out_parsed.output_sig_element_stride =
                ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 32u, out_parsed.output_signature);
        } else if (std::memcmp(chunk.fourcc, "OSG5", 4u) == 0 && out_parsed.output_signature.empty()) {
            out_parsed.output_sig_element_stride =
                ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 28u, out_parsed.output_signature);
        } else if (std::memcmp(chunk.fourcc, "OSGN", 4u) == 0 && out_parsed.output_signature.empty()) {
            out_parsed.output_sig_element_stride =
                ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 24u, out_parsed.output_signature);
        } else if (std::memcmp(chunk.fourcc, "PSG1", 4u) == 0) {
            (void)ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 32u, out_parsed.patch_constant_signature);
        } else if (std::memcmp(chunk.fourcc, "PSGN", 4u) == 0 && out_parsed.patch_constant_signature.empty()) {
            (void)ParseSignatureChunk(chunk.data.data(), chunk.data.size(), 24u, out_parsed.patch_constant_signature);
        } else if (std::memcmp(chunk.fourcc, "RTS0", 4u) == 0) {
            if (auto st = ParseRts0RootSignature(chunk.data.data(), chunk.data.size(), out_parsed.root_signature);
                !st.ok()) {
                return st;
            }
        }

        out_parsed.chunks.push_back(std::move(chunk));
    }

    // Reconcile PM4 register setup & PSV0 metadata into xs2 container header
    if (out_parsed.has_xs2) {
        if (out_parsed.reg_setup.present) {
            if (out_parsed.reg_setup.vgpr_count > 0u) out_parsed.xs2.vgpr_count = out_parsed.reg_setup.vgpr_count;
            if (out_parsed.reg_setup.sgpr_count > 0u) out_parsed.xs2.sgpr_count = out_parsed.reg_setup.sgpr_count;
            if (out_parsed.reg_setup.wave_size == 64u) out_parsed.xs2.wave_size = 64u;
            if (out_parsed.reg_setup.threadgroup_x > 0u) out_parsed.xs2.threadgroup_x = out_parsed.reg_setup.threadgroup_x;
            if (out_parsed.reg_setup.threadgroup_y > 0u) out_parsed.xs2.threadgroup_y = out_parsed.reg_setup.threadgroup_y;
            if (out_parsed.reg_setup.threadgroup_z > 0u) out_parsed.xs2.threadgroup_z = out_parsed.reg_setup.threadgroup_z;
        }
        if (out_parsed.psv0.present) {
            out_parsed.xs2.wave_size = SelectGuestWaveSizeFromPsv0(out_parsed.psv0, out_parsed.xs2.wave_size);
            if (out_parsed.psv0.num_threads_x > 0u || out_parsed.psv0.num_threads_y > 0u ||
                out_parsed.psv0.num_threads_z > 0u) {
                out_parsed.xs2.threadgroup_x = out_parsed.psv0.num_threads_x ? out_parsed.psv0.num_threads_x : 1u;
                out_parsed.xs2.threadgroup_y = out_parsed.psv0.num_threads_y ? out_parsed.psv0.num_threads_y : 1u;
                out_parsed.xs2.threadgroup_z = out_parsed.psv0.num_threads_z ? out_parsed.psv0.num_threads_z : 1u;
            }
        }
    }

    // Refine PS input signature interpolation modes from SPI_PS_INPUT_ENA / CNTL
    if (out_parsed.xs2.stage == ShaderStage::Pixel ||
        out_parsed.reg_setup.inferred_stage == ShaderStage::Pixel) {
        for (auto& inp : out_parsed.input_signature) {
            if (!IsSystemValueSemantic(inp.semantic_name)) {
                inp.interp_mode = ResolvePsInputInterpMode(inp, out_parsed.reg_setup);
            }
        }
    }

    // Build user-data SGPR map if RTS0 is present, otherwise infer from PM4/ISA (mesh & graphics/compute).
    if (out_parsed.root_signature.present) {
        const ShaderStage eff_stage =
            (out_parsed.has_xs2 && out_parsed.xs2.stage != ShaderStage::Unknown)
                ? out_parsed.xs2.stage
                : out_parsed.reg_setup.inferred_stage;
        const uint32_t max_sgprs =
            (out_parsed.reg_setup.present && out_parsed.reg_setup.user_sgpr_count > 16u)
                ? std::min(32u, out_parsed.reg_setup.user_sgpr_count)
                : 16u;
        (void)BuildUserSgprRootMap(out_parsed.root_signature,
                                   out_parsed.reg_setup.present ? &out_parsed.reg_setup : nullptr,
                                   eff_stage,
                                   out_parsed.sgpr_map,
                                   max_sgprs);
    }

    // Always analyze VS v0/v3 system IDs & BVH ray-intersection ops, and infer root bindings when RTS0 absent.
    (void)InferShaderRootBindingsWhenRts0Absent(out_parsed);

    out_parsed.format = out_parsed.has_xs2
                            ? XboxShaderBlobFormat::XboxDxbc
                            : XboxShaderBlobFormat::StandardHostDxbc;
    return Status::Ok();
}

Status ParseFullXboxShaderContainer(const std::vector<uint8_t>& bytes, XboxParsedShaderContainer& out_parsed) {
    return ParseFullXboxShaderContainer(bytes.data(), bytes.size(), out_parsed);
}

std::vector<uint8_t> BuildFullXboxDxbcContainer(const XboxFullDxbcBuildDesc& desc) {
    std::vector<XboxDxbcChunk> chunks;

    if (!desc.input_signature.empty()) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, desc.use_sig_v1 ? "ISG1" : "ISGN", 5u);
        c.data = BuildSignatureChunk(desc.input_signature, desc.use_sig_v1);
        chunks.push_back(std::move(c));
    }
    if (!desc.output_signature.empty()) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, desc.use_sig_v1 ? "OSG1" : "OSGN", 5u);
        c.data = BuildSignatureChunk(desc.output_signature, desc.use_sig_v1);
        chunks.push_back(std::move(c));
    }
    if (!desc.patch_constant_signature.empty()) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, desc.use_sig_v1 ? "PSG1" : "PSGN", 5u);
        c.data = BuildSignatureChunk(desc.patch_constant_signature, desc.use_sig_v1);
        chunks.push_back(std::move(c));
    }
    if (desc.root_signature.present || !desc.root_signature.parameters.empty() ||
        !desc.root_signature.static_samplers.empty()) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, "RTS0", 5u);
        c.data = BuildRts0RootSignature(desc.root_signature);
        chunks.push_back(std::move(c));
    }
    if (!desc.shdr_or_shex_payload.empty()) {
        XboxDxbcChunk c{};
        const std::string tag = (desc.aux_shader_fourcc.size() == 4u) ? desc.aux_shader_fourcc : "SHEX";
        std::memcpy(c.fourcc, tag.data(), 4u);
        c.fourcc[4] = '\0';
        c.data = desc.shdr_or_shex_payload;
        chunks.push_back(std::move(c));
    }

    std::vector<uint8_t> pm4_bytes;
    if (desc.reg_setup.present) {
        pm4_bytes = BuildPm4RegisterBlock(desc.reg_setup, desc.stage);
    }

    if (!pm4_bytes.empty() && !desc.embed_pm4_in_shader_chunk) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, "PM4R", 5u);
        c.data = pm4_bytes;
        chunks.push_back(std::move(c));
    }

    if (!desc.xs2_blob.empty() || !desc.raw_rdna_isa_words.empty()) {
        XboxDxbcChunk c{};
        const std::string tag = (desc.xbox_chunk_fourcc.size() == 4u) ? desc.xbox_chunk_fourcc : "XBOX";
        std::memcpy(c.fourcc, tag.data(), 4u);
        c.fourcc[4] = '\0';

        if (tag == "RDNA" || (desc.embed_pm4_in_shader_chunk && !pm4_bytes.empty())) {
            std::vector<uint8_t> isa_payload = desc.xs2_blob;
            if (isa_payload.empty() && !desc.raw_rdna_isa_words.empty()) {
                isa_payload.reserve(desc.raw_rdna_isa_words.size() * 4u);
                for (uint32_t w : desc.raw_rdna_isa_words) {
                    AppendLe32(isa_payload, w);
                }
            }
            const std::vector<uint8_t> embedded_pm4 = desc.embed_pm4_in_shader_chunk ? pm4_bytes : std::vector<uint8_t>{};
            c.data.reserve(16u + embedded_pm4.size() + isa_payload.size());
            c.data.push_back('R');
            c.data.push_back('D');
            c.data.push_back('N');
            c.data.push_back('A');
            AppendLe32(c.data, static_cast<uint32_t>(desc.stage));
            AppendLe32(c.data, static_cast<uint32_t>(embedded_pm4.size()));
            AppendLe32(c.data, static_cast<uint32_t>(isa_payload.size()));
            c.data.insert(c.data.end(), embedded_pm4.begin(), embedded_pm4.end());
            c.data.insert(c.data.end(), isa_payload.begin(), isa_payload.end());
        } else {
            c.data = desc.xs2_blob;
        }
        chunks.push_back(std::move(c));
    }

    if (desc.psv0.present) {
        XboxDxbcBuildDesc psv_tmp{};
        psv_tmp.psv0 = desc.psv0;
        const auto psv_dxbc = BuildXboxDxbcContainer(psv_tmp);
        XboxDxbcContainer parsed_tmp{};
        if (ParseXboxShaderContainer(psv_dxbc, parsed_tmp).ok()) {
            for (const auto& ch : parsed_tmp.chunks) {
                if (std::memcmp(ch.fourcc, "PSV0", 4u) == 0) {
                    chunks.push_back(ch);
                    break;
                }
            }
        }
    }

    const uint32_t chunk_count = static_cast<uint32_t>(chunks.size());
    uint32_t cur_off = 32u + chunk_count * 4u;
    std::vector<uint32_t> offsets;
    offsets.reserve(chunk_count);
    for (const auto& ch : chunks) {
        offsets.push_back(cur_off);
        cur_off += 8u + static_cast<uint32_t>(ch.data.size());
    }
    const uint32_t total_size = cur_off;

    std::vector<uint8_t> out;
    out.reserve(total_size);
    out.push_back('D');
    out.push_back('X');
    out.push_back('B');
    out.push_back('C');
    for (int i = 0; i < 16; ++i) out.push_back(0u);
    AppendLe32(out, 1u);
    AppendLe32(out, total_size);
    AppendLe32(out, chunk_count);
    for (uint32_t off : offsets) AppendLe32(out, off);
    for (const auto& ch : chunks) {
        out.push_back(static_cast<uint8_t>(ch.fourcc[0]));
        out.push_back(static_cast<uint8_t>(ch.fourcc[1]));
        out.push_back(static_cast<uint8_t>(ch.fourcc[2]));
        out.push_back(static_cast<uint8_t>(ch.fourcc[3]));
        AppendLe32(out, static_cast<uint32_t>(ch.data.size()));
        out.insert(out.end(), ch.data.begin(), ch.data.end());
    }

    uint8_t digest[16]{};
    ComputeDxbcContainerHash(out.data(), out.size(), digest);
    std::memcpy(out.data() + 4u, digest, 16u);
    return out;
}

void ApplyXboxContainerToLiftOptions(const XboxParsedShaderContainer& container, Rdna2LiftOptions& opts) {
    if (container.xs2.stage == ShaderStage::Pixel) {
        opts.stage_contract.pixel_inputs.clear();
        uint32_t param_loc = 0u;
        for (const auto& inp : container.input_signature) {
            if (IsSystemValueSemantic(inp.semantic_name)) continue;
            FssmPixelInputEntry entry{};
            entry.param_slot = param_loc;
            entry.semantic_location = param_loc;
            entry.interp_mode = ResolvePsInputInterpMode(inp, container.reg_setup);
            entry.component_mask = inp.mask;
            opts.stage_contract.pixel_inputs.push_back(entry);
            ++param_loc;
        }
    }
    if (container.xs2.stage == ShaderStage::Vertex && container.sgpr_map.uses_vertex_buffer_table) {
        if (opts.stage_contract.vertex_fetches.empty()) {
            uint32_t byte_off = 0u;
            uint32_t idx = 0u;
            for (const auto& inp : container.input_signature) {
                if (IsSystemValueSemantic(inp.semantic_name)) continue;
                VertexFetchEntry vf{};
                vf.table_index = idx;
                vf.vb_slot = 0u;
                vf.byte_offset = byte_off;
                vf.dst_vgpr = static_cast<uint8_t>(idx * 4u);
                uint8_t comps = 0u;
                for (uint8_t b = 0u; b < 4u; ++b) {
                    if ((inp.mask & (1u << b)) != 0u) ++comps;
                }
                vf.num_components = comps ? comps : 4u;
                vf.format = (inp.component_type == 1u)
                                ? Rdna2MtbufFormat::R32_UINT
                                : (inp.component_type == 2u ? Rdna2MtbufFormat::R32_SINT
                                                            : Rdna2MtbufFormat::R32G32B32A32_FLOAT);
                opts.stage_contract.vertex_fetches.push_back(vf);
                byte_off += static_cast<uint32_t>(vf.num_components) * 4u;
                ++idx;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// VS -> PS Interpolation Modifier PSO Linkage Validator & Matcher
// ---------------------------------------------------------------------------
const char* HlslInterpQualifierName(Rdna2InterpMode mode) noexcept {
    switch (mode) {
        case Rdna2InterpMode::Flat:          return "nointerpolation";
        case Rdna2InterpMode::NoPerspective: return "noperspective";
        case Rdna2InterpMode::Centroid:      return "centroid";
        case Rdna2InterpMode::Sample:        return "sample";
        case Rdna2InterpMode::Perspective:
        default:                             return "linear";
    }
}

uint32_t SpirvInterpDecoration(Rdna2InterpMode mode) noexcept {
    switch (mode) {
        case Rdna2InterpMode::NoPerspective: return 13u;  // Decoration::NoPerspective
        case Rdna2InterpMode::Flat:          return 14u;  // Decoration::Flat
        case Rdna2InterpMode::Centroid:      return 16u;  // Decoration::Centroid
        case Rdna2InterpMode::Sample:        return 17u;  // Decoration::Sample
        case Rdna2InterpMode::Perspective:
        default:                             return 0u;   // Default perspective interpolation
    }
}

Status ValidateAndMatchStageInterpolation(
    const XboxParsedShaderContainer& vs_container,
    const XboxParsedShaderContainer& ps_container,
    XboxPsoInterpLinkageReport& out_report,
    Rdna2IrModule* vs_ir,
    Rdna2IrModule* ps_ir) {
    out_report = XboxPsoInterpLinkageReport{};

    uint32_t loc = 0u;
    for (const auto& ps_in : ps_container.input_signature) {
        if (IsSystemValueSemantic(ps_in.semantic_name)) continue;

        const std::string ps_sem_u = ToUpperAscii(ps_in.semantic_name);
        const XboxSignatureParamDesc* matched_vs = nullptr;
        for (const auto& vs_out : vs_container.output_signature) {
            if (IsSystemValueSemantic(vs_out.semantic_name)) continue;
            if (ToUpperAscii(vs_out.semantic_name) == ps_sem_u &&
                vs_out.semantic_index == ps_in.semantic_index) {
                matched_vs = &vs_out;
                break;
            }
        }

        XboxInterpLinkageEntry entry{};
        entry.semantic_name = ps_in.semantic_name;
        entry.semantic_index = ps_in.semantic_index;
        entry.location = loc++;
        entry.ps_mode = ResolvePsInputInterpMode(ps_in, ps_container.reg_setup);

        if (!matched_vs) {
            entry.matched = false;
            out_report.all_compatible = false;
            out_report.errors.push_back(
                "Missing VS output for PS input semantic " + ps_in.semantic_name +
                std::to_string(ps_in.semantic_index));
            entry.resolved_mode = entry.ps_mode;
        } else {
            entry.matched = true;
            entry.vs_mode = matched_vs->interp_mode;
            // PS interpolation contract is authoritative in D3D12/Vulkan PSO linkage;
            // integer semantics must strictly be Flat on both sides.
            const bool ps_is_int = (ps_in.component_type == 1u || ps_in.component_type == 2u);
            const bool vs_is_int = (matched_vs->component_type == 1u || matched_vs->component_type == 2u);
            if (ps_is_int != vs_is_int) {
                out_report.all_compatible = false;
                out_report.errors.push_back(
                    "Component type mismatch (int vs float) on semantic " + ps_in.semantic_name +
                    std::to_string(ps_in.semantic_index));
            }
            if (ps_is_int || vs_is_int) {
                entry.resolved_mode = Rdna2InterpMode::Flat;
            } else if (entry.ps_mode != Rdna2InterpMode::Perspective) {
                entry.resolved_mode = entry.ps_mode;
                if (entry.vs_mode != Rdna2InterpMode::Perspective && entry.vs_mode != entry.ps_mode) {
                    out_report.warnings.push_back(
                        "Reconciled VS->PS interpolation qualifier mismatch on " + ps_in.semantic_name +
                        std::to_string(ps_in.semantic_index) + ": VS=" +
                        HlslInterpQualifierName(entry.vs_mode) + " -> PS=" +
                        HlslInterpQualifierName(entry.ps_mode));
                }
            } else {
                entry.resolved_mode = entry.vs_mode;
            }
        }

        entry.hlsl_qualifier = HlslInterpQualifierName(entry.resolved_mode);
        entry.spirv_decoration = SpirvInterpDecoration(entry.resolved_mode);
        out_report.entries.push_back(std::move(entry));
    }

    if (vs_ir != nullptr || ps_ir != nullptr) {
        const size_t n = out_report.entries.size();
        if (vs_ir != nullptr && n > 0u) {
            if (vs_ir->output_interp_modes.size() < n) {
                vs_ir->output_interp_modes.resize(n, Rdna2InterpMode::Perspective);
            }
            for (const auto& e : out_report.entries) {
                vs_ir->output_interp_modes[e.location] = e.resolved_mode;
            }
        }
        if (ps_ir != nullptr && n > 0u) {
            if (ps_ir->input_interp_modes.size() < n) {
                ps_ir->input_interp_modes.resize(n, Rdna2InterpMode::Perspective);
            }
            for (const auto& e : out_report.entries) {
                ps_ir->input_interp_modes[e.location] = e.resolved_mode;
            }
        }
        if (vs_ir != nullptr && ps_ir != nullptr) {
            ReconcileStageInterpolation(*vs_ir, *ps_ir);
        }
    }

    return out_report.all_compatible
               ? Status::Ok()
               : Status::Error(StatusCode::ShaderError,
                               out_report.errors.empty() ? "VS->PS interpolation linkage failed"
                                                         : out_report.errors.front());
}

// ---------------------------------------------------------------------------
// Sprint 13: VS v0/v3 Prologue Analysis, BVH RayQuery Detection,
//            Missing-RTS0 Mesh/Shader Root-Table Inference, & Container Lifting
// ---------------------------------------------------------------------------
namespace {

void AnalyzeVsSystemIdsAndBvhOps(
    const XboxParsedShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    XboxVsPrologueInfo& out_vs,
    XboxBvhIntersectInfo& out_bvh) {
    out_vs = XboxVsPrologueInfo{};
    out_bvh = XboxBvhIntersectInfo{};

    const ShaderStage eff_stage =
        (container.has_xs2 && container.xs2.stage != ShaderStage::Unknown)
            ? container.xs2.stage
            : container.reg_setup.inferred_stage;

    if (eff_stage == ShaderStage::Vertex) {
        for (const auto& sig : container.input_signature) {
            const std::string sem = ToUpperAscii(sig.semantic_name);
            if (sem == "SV_VERTEXID") {
                out_vs.declared_vertex_id_in_sig = true;
            } else if (sem == "SV_INSTANCEID") {
                out_vs.declared_instance_id_in_sig = true;
            }
        }
    }

    std::array<bool, 256> written_vgpr{};
    auto check_vgpr_read = [&](uint8_t idx) {
        if (eff_stage != ShaderStage::Vertex) return;
        if (idx == 0u && !written_vgpr[0]) {
            out_vs.uses_vertex_id_v0 = true;
        } else if (idx == 3u && !written_vgpr[3]) {
            out_vs.uses_instance_id_v3 = true;
        }
    };

    for (const auto& inst : instructions) {
        // Detect RDNA2 BVH ray-intersection instructions (0xE6 image_bvh_intersect_ray, 0xE7 image_bvh64_intersect_ray)
        if (inst.encoding == Rdna2Encoding::MIMG &&
            (inst.opcode == Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY ||
             inst.raw_opcode == kMimgRawImageBvhIntersectRay ||
             inst.raw_opcode == kMimgRawImageBvh64IntersectRay)) {
            out_bvh.detected = true;
            if (inst.raw_opcode == kMimgRawImageBvh64IntersectRay) {
                out_bvh.is_64bit = true;
            }
            out_bvh.srsrc_sgpr = static_cast<uint8_t>(inst.srsrc * 4u);
            ++out_bvh.instruction_count;
            out_bvh.threadgroup_x = container.xs2.threadgroup_x ? container.xs2.threadgroup_x : 64u;
        }

        if (eff_stage == ShaderStage::Vertex) {
            // Check reads BEFORE marking destination written.
            if (inst.opcode == Rdna2Opcode::V_FMAC_F32 ||
                inst.opcode == Rdna2Opcode::V_FMAC_F16 ||
                inst.opcode == Rdna2Opcode::V_MAC_F32) {
                if (inst.dst.kind == Rdna2OperandKind::Vgpr) {
                    check_vgpr_read(static_cast<uint8_t>(inst.dst.reg_index));
                }
            }
            for (uint8_t s = 0u; s < inst.src_count && s < 4u; ++s) {
                if (inst.src[s].kind == Rdna2OperandKind::Vgpr) {
                    // For MUBUF/MTBUF loads, src[0] is the vdata destination register (not a read).
                    if ((inst.encoding == Rdna2Encoding::MUBUF || inst.encoding == Rdna2Encoding::MTBUF) &&
                        inst.opcode != Rdna2Opcode::BUFFER_STORE_DWORD && s == 0u) {
                        continue;
                    }
                    // For EXP, only enabled channels are read.
                    if (inst.encoding == Rdna2Encoding::EXP && ((inst.exp_en & (1u << s)) == 0u)) {
                        continue;
                    }
                    check_vgpr_read(static_cast<uint8_t>(inst.src[s].reg_index));
                }
            }
            if (inst.encoding == Rdna2Encoding::MIMG && inst.src_count > 0u &&
                inst.src[0].kind == Rdna2OperandKind::Vgpr &&
                inst.opcode != Rdna2Opcode::IMAGE_STORE) {
                const uint8_t base = static_cast<uint8_t>(inst.src[0].reg_index);
                check_vgpr_read(base);
                if (base + 1u < 256u) check_vgpr_read(static_cast<uint8_t>(base + 1u));
            }

            // Mark destination VGPRs written.
            if (inst.dst.kind == Rdna2OperandKind::Vgpr && inst.dst.reg_index < 256u) {
                written_vgpr[inst.dst.reg_index] = true;
                if (inst.encoding == Rdna2Encoding::MIMG && inst.opcode != Rdna2Opcode::IMAGE_STORE) {
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    uint8_t reg = static_cast<uint8_t>(inst.dst.reg_index);
                    for (uint32_t c = 0u; c < 4u && reg < 256u; ++c) {
                        if ((dmask & (1u << c)) != 0u) {
                            written_vgpr[reg++] = true;
                        }
                    }
                }
            }
        }
    }
}

bool IsIdentChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

std::string ReplaceWholeToken(const std::string& src, const std::string& token, const std::string& repl) {
    if (token.empty()) return src;
    std::string out;
    out.reserve(src.size() + 32u);
    size_t pos = 0u;
    while (pos < src.size()) {
        const size_t hit = src.find(token, pos);
        if (hit == std::string::npos) {
            out.append(src, pos, std::string::npos);
            break;
        }
        const bool left_ok = (hit == 0u) || !IsIdentChar(src[hit - 1u]);
        const size_t end = hit + token.size();
        const bool right_ok = (end >= src.size()) || !IsIdentChar(src[end]);
        out.append(src, pos, hit - pos);
        if (left_ok && right_ok) {
            out.append(repl);
        } else {
            out.append(token);
        }
        pos = end;
    }
    return out;
}

void PatchVsHlslForInstanceIdPrologue(
    std::string& hlsl,
    bool inject_v3_iid,
    uint8_t sentinel_sgpr,
    bool orig_uses_root_consts,
    uint32_t orig_max_root_const) {
    // 1. Add SV_InstanceID parameter to VS entry signature
    const std::string old_sig = "VSOut main(uint _vid : SV_VertexID)";
    const std::string new_sig = "VSOut main(uint _vid : SV_VertexID, uint _iid : SV_InstanceID)";
    const size_t sig_pos = hlsl.find(old_sig);
    if (sig_pos != std::string::npos) {
        hlsl.replace(sig_pos, old_sig.size(), new_sig);
    }

    if (!inject_v3_iid || sentinel_sgpr >= 16u) {
        return;
    }

    // 2. Clean up synthetic cbuffer _os_root_consts if it was only added/expanded for sentinel_sgpr
    const std::string cb_hdr = "cbuffer _os_root_consts : register(b0) {\n";
    const size_t cb_pos = hlsl.find(cb_hdr);
    if (cb_pos != std::string::npos) {
        const size_t cb_end = hlsl.find("};\n", cb_pos);
        if (cb_end != std::string::npos) {
            if (!orig_uses_root_consts) {
                hlsl.erase(cb_pos, (cb_end + 3u) - cb_pos);
            } else if (orig_max_root_const < sentinel_sgpr) {
                std::ostringstream rebuilt;
                rebuilt << cb_hdr;
                for (uint32_t i = 0u; i <= orig_max_root_const; ++i) {
                    rebuilt << "    uint _rc" << i << ";\n";
                }
                rebuilt << "};\n";
                hlsl.replace(cb_pos, (cb_end + 3u) - cb_pos, rebuilt.str());
            }
        }
    }

    // 3. Replace all references to _rc<sentinel_sgpr> with _iid (SV_InstanceID)
    hlsl = ReplaceWholeToken(hlsl, "_rc" + std::to_string(sentinel_sgpr), "_iid");
}

bool PatchVsSpirvForInstanceIdPrologue(std::vector<uint32_t>& spirv, uint8_t sentinel_sgpr) {
    if (spirv.size() < 5u || spirv[0] != 0x07230203u || sentinel_sgpr >= 16u) {
        return false;
    }
    constexpr uint16_t kOpEntryPoint = 15u;
    constexpr uint16_t kOpTypeInt = 21u;
    constexpr uint16_t kOpTypePointer = 32u;
    constexpr uint16_t kOpConstant = 43u;
    constexpr uint16_t kOpFunction = 54u;
    constexpr uint16_t kOpVariable = 59u;
    constexpr uint16_t kOpLoad = 61u;
    constexpr uint16_t kOpAccessChain = 65u;
    constexpr uint16_t kOpDecorate = 71u;
    constexpr uint16_t kOpBitcast = 124u;
    constexpr uint32_t kScInput = 1u;
    constexpr uint32_t kScUniform = 2u;
    constexpr uint32_t kDecBuiltIn = 11u;
    constexpr uint32_t kBuiltInInstanceIndex = 43u;

    uint32_t next_id = spirv[3];
    uint32_t t_i32 = 0u;
    uint32_t t_u32 = 0u;
    uint32_t t_ptr_in_i32 = 0u;
    uint32_t c_i32_sentinel = 0u;
    std::set<uint32_t> uniform_vars;

    size_t entry_point_pos = 0u;
    size_t last_decor_end = 0u;
    size_t first_func_pos = 0u;

    size_t pos = 5u;
    while (pos < spirv.size()) {
        const uint32_t hdr = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
        const uint16_t op = static_cast<uint16_t>(hdr & 0xFFFFu);
        if (wc == 0u || pos + wc > spirv.size()) return false;

        if (op == kOpEntryPoint && entry_point_pos == 0u) {
            entry_point_pos = pos;
        } else if (op == kOpDecorate) {
            last_decor_end = pos + wc;
        } else if (op == kOpTypeInt && wc == 4u && spirv[pos + 2u] == 32u) {
            if (spirv[pos + 3u] == 1u) t_i32 = spirv[pos + 1u];
            else if (spirv[pos + 3u] == 0u) t_u32 = spirv[pos + 1u];
        } else if (op == kOpTypePointer && wc == 4u &&
                   spirv[pos + 2u] == kScInput && t_i32 != 0u && spirv[pos + 3u] == t_i32) {
            t_ptr_in_i32 = spirv[pos + 1u];
        } else if (op == kOpConstant && wc == 4u &&
                   t_i32 != 0u && spirv[pos + 1u] == t_i32 && spirv[pos + 3u] == sentinel_sgpr) {
            c_i32_sentinel = spirv[pos + 2u];
        } else if (op == kOpVariable && wc >= 4u && spirv[pos + 3u] == kScUniform) {
            uniform_vars.insert(spirv[pos + 2u]);
        } else if (op == kOpFunction && first_func_pos == 0u) {
            first_func_pos = pos;
        }
        pos += wc;
    }

    if (entry_point_pos == 0u || first_func_pos == 0u || t_i32 == 0u || t_u32 == 0u ||
        c_i32_sentinel == 0u || uniform_vars.empty()) {
        return false;
    }

    // Locate OpAccessChain %ptr_ty %ptr %var_rc %c_i32_sentinel followed by OpLoad %t_u32 %res %ptr
    size_t ac_pos = 0u;
    uint16_t ac_wc = 0u;
    size_t ld_pos = 0u;
    uint16_t ld_wc = 0u;
    uint32_t loaded_res_id = 0u;

    pos = first_func_pos;
    while (pos < spirv.size()) {
        const uint32_t hdr = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
        const uint16_t op = static_cast<uint16_t>(hdr & 0xFFFFu);
        if (wc == 0u || pos + wc > spirv.size()) return false;

        if (op == kOpAccessChain && wc == 5u &&
            uniform_vars.count(spirv[pos + 3u]) > 0u &&
            spirv[pos + 4u] == c_i32_sentinel) {
            const uint32_t ptr_id = spirv[pos + 2u];
            const size_t next_p = pos + wc;
            if (next_p < spirv.size()) {
                const uint32_t nh = spirv[next_p];
                const uint16_t nwc = static_cast<uint16_t>((nh >> 16) & 0xFFFFu);
                const uint16_t nop = static_cast<uint16_t>(nh & 0xFFFFu);
                if (nop == kOpLoad && nwc >= 4u &&
                    spirv[next_p + 1u] == t_u32 && spirv[next_p + 3u] == ptr_id) {
                    ac_pos = pos;
                    ac_wc = wc;
                    ld_pos = next_p;
                    ld_wc = nwc;
                    loaded_res_id = spirv[next_p + 2u];
                    break;
                }
            }
        }
        pos += wc;
    }
    if (ac_pos == 0u || loaded_res_id == 0u) {
        return false;
    }

    const bool need_ptr_type = (t_ptr_in_i32 == 0u);
    if (need_ptr_type) {
        t_ptr_in_i32 = next_id++;
    }
    const uint32_t var_iid = next_id++;
    const uint32_t tmp_iid_i32 = next_id++;

    std::vector<uint32_t> patched;
    patched.reserve(spirv.size() + 32u);
    // Header
    patched.push_back(spirv[0]);
    patched.push_back(spirv[1]);
    patched.push_back(spirv[2]);
    patched.push_back(next_id);
    patched.push_back(spirv[4]);

    pos = 5u;
    bool inserted_decor = false;
    bool inserted_var = false;
    while (pos < spirv.size()) {
        const uint32_t hdr = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);

        if (pos == entry_point_pos) {
            // Append var_iid to OpEntryPoint interface list
            const uint16_t new_wc = static_cast<uint16_t>(wc + 1u);
            patched.push_back((static_cast<uint32_t>(new_wc) << 16) | (hdr & 0xFFFFu));
            for (uint16_t k = 1u; k < wc; ++k) patched.push_back(spirv[pos + k]);
            patched.push_back(var_iid);
            pos += wc;
            continue;
        }
        if (pos == first_func_pos && !inserted_var) {
            if (need_ptr_type) {
                patched.push_back((4u << 16) | kOpTypePointer);
                patched.push_back(t_ptr_in_i32);
                patched.push_back(kScInput);
                patched.push_back(t_i32);
            }
            patched.push_back((4u << 16) | kOpVariable);
            patched.push_back(t_ptr_in_i32);
            patched.push_back(var_iid);
            patched.push_back(kScInput);
            inserted_var = true;
        }
        if (pos == ac_pos) {
            // Replace OpAccessChain + OpLoad with OpLoad %t_i32 %tmp %var_iid + OpBitcast %t_u32 %res %tmp
            patched.push_back((4u << 16) | kOpLoad);
            patched.push_back(t_i32);
            patched.push_back(tmp_iid_i32);
            patched.push_back(var_iid);

            patched.push_back((4u << 16) | kOpBitcast);
            patched.push_back(t_u32);
            patched.push_back(loaded_res_id);
            patched.push_back(tmp_iid_i32);
            pos = ld_pos + ld_wc;
            continue;
        }

        for (uint16_t k = 0u; k < wc; ++k) {
            patched.push_back(spirv[pos + k]);
        }
        pos += wc;

        if (!inserted_decor && pos == last_decor_end) {
            patched.push_back((4u << 16) | kOpDecorate);
            patched.push_back(var_iid);
            patched.push_back(kDecBuiltIn);
            patched.push_back(kBuiltInInstanceIndex);
            inserted_decor = true;
        }
    }

    if (!inserted_decor || !inserted_var) {
        return false;
    }
    if (!ValidateSpirvBinary(patched).ok()) {
        return false;
    }
    spirv = std::move(patched);
    return true;
}

}  // namespace

bool InferShaderRootBindingsWhenRts0Absent(XboxParsedShaderContainer& container) {
    if (!container.has_xs2 || container.rdna_isa_words.empty()) {
        return false;
    }

    std::vector<Rdna2Instruction> instructions;
    if (!DisassembleRdna2Extended(container.rdna_isa_words, instructions).ok()) {
        return false;
    }

    // Always populate VS v0/v3 prologue metadata and BVH ray-intersection detection.
    AnalyzeVsSystemIdsAndBvhOps(container, instructions, container.vs_prologue, container.bvh_info);

    if (container.root_signature.present) {
        return false;
    }

    const ShaderStage eff_stage =
        (container.xs2.stage != ShaderStage::Unknown)
            ? container.xs2.stage
            : container.reg_setup.inferred_stage;

    std::array<bool, 106> written_sgpr{};
    bool uses_root_consts = false;
    uint32_t max_root_const = 0u;
    bool uses_smem_cbv = false;
    std::set<uint32_t> uav_slots;
    uint32_t srv_count = 0u;
    bool uses_tex2d_srv = false;
    bool uses_tex3d_srv = false;
    bool uses_texcube_srv = false;
    bool uses_sampler = false;
    bool uses_tex_uav = false;
    bool uses_tex_u32_uav = false;

    for (const auto& inst : instructions) {
        // Track unread SGPRs s0..s15 read directly (inline root constants; skip s3 merged_wave_info on MS/GS)
        for (uint8_t s = 0u; s < inst.src_count && s < 4u; ++s) {
            if (inst.src[s].kind == Rdna2OperandKind::Sgpr && inst.src[s].reg_index < 16u) {
                const uint32_t idx = inst.src[s].reg_index;
                if (inst.encoding == Rdna2Encoding::SMEM) continue;
                if ((eff_stage == ShaderStage::Mesh || eff_stage == ShaderStage::Geometry) && idx == 3u) {
                    continue;
                }
                if (!written_sgpr[idx]) {
                    uses_root_consts = true;
                    max_root_const = std::max(max_root_const, idx);
                }
            }
        }

        if (inst.encoding == Rdna2Encoding::SMEM) {
            uses_smem_cbv = true;
        } else if (inst.encoding == Rdna2Encoding::MUBUF || inst.encoding == Rdna2Encoding::MTBUF) {
            const uint32_t slot = (inst.srsrc == 0u) ? 0u : 1u;
            uav_slots.insert(slot);
        } else if (inst.encoding == Rdna2Encoding::FLAT &&
                   (inst.opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD ||
                    inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD ||
                    inst.opcode == Rdna2Opcode::FLAT_LOAD_DWORD ||
                    inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD)) {
            uav_slots.insert(0u);
        } else if (inst.encoding == Rdna2Encoding::MIMG) {
            if (inst.opcode == Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY ||
                inst.raw_opcode == kMimgRawImageBvhIntersectRay ||
                inst.raw_opcode == kMimgRawImageBvh64IntersectRay) {
                // Multi-window BVH4 RayQuery ABI: b0 + t0..t2 + u0
                srv_count = std::max(srv_count, 3u);
                uav_slots.insert(0u);
            } else if (inst.opcode == Rdna2Opcode::IMAGE_LOAD ||
                       inst.opcode == Rdna2Opcode::IMAGE_SAMPLE ||
                       inst.opcode == Rdna2Opcode::IMAGE_SAMPLE_LZ) {
                uses_tex2d_srv = true;
                if (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE ||
                    inst.opcode == Rdna2Opcode::IMAGE_SAMPLE_LZ) {
                    uses_sampler = true;
                }
            } else if (inst.opcode == Rdna2Opcode::IMAGE_LOAD_MIP) {
                if (inst.mimg_dim == 2u) uses_tex3d_srv = true;
                else if (inst.mimg_dim == 3u) uses_texcube_srv = true;
                else uses_tex2d_srv = true;
            } else if (inst.opcode == Rdna2Opcode::IMAGE_STORE) {
                uses_tex_uav = true;
            } else if (inst.opcode == Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP) {
                uses_tex_uav = true;
                uses_tex_u32_uav = true;
            }
        }

        if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 106u) {
            written_sgpr[inst.dst.reg_index] = true;
        }
    }

    const uint32_t tex_srv_total =
        (uses_tex2d_srv ? 1u : 0u) + (uses_tex3d_srv ? 1u : 0u) + (uses_texcube_srv ? 1u : 0u);
    srv_count = std::max(srv_count, tex_srv_total);

    uint32_t uav_count = 0u;
    if (!uav_slots.empty()) {
        uav_count = (*uav_slots.rbegin()) + 1u;
    }
    if (uses_tex_uav) {
        const uint32_t base_rw_u = (uav_count >= 2u) ? 2u : 1u;
        uav_count = std::max(uav_count, base_rw_u + (uses_tex_u32_uav ? 2u : 1u));
    }

    const bool is_mesh_or_amp =
        (eff_stage == ShaderStage::Mesh || eff_stage == ShaderStage::Amplification);
    const bool has_any_resource =
        uses_root_consts || uses_smem_cbv || srv_count > 0u || uav_count > 0u ||
        uses_sampler || container.bvh_info.detected;
    const bool mesh_has_user_sgprs =
        is_mesh_or_amp && container.reg_setup.present && container.reg_setup.user_sgpr_count > 0u;

    if (!has_any_resource && !mesh_has_user_sgprs) {
        return false;
    }

    XboxRootSignatureDesc inferred{};
    inferred.present = true;
    inferred.version = 2u;  // D3D_ROOT_SIGNATURE_VERSION_1_1
    inferred.flags = 0u;

    // 1. Inline root constants / CBV b0
    if (container.bvh_info.detected) {
        XboxRootParameter cbv{};
        cbv.type = XboxRootParamType::Cbv;
        cbv.shader_visibility = 0u;
        cbv.shader_register = 0u;
        cbv.register_space = 0u;
        inferred.parameters.push_back(std::move(cbv));
    } else if (uses_root_consts) {
        XboxRootParameter rc{};
        rc.type = XboxRootParamType::Constants32Bit;
        rc.shader_visibility = 0u;
        rc.shader_register = 0u;
        rc.register_space = 0u;
        rc.num_32bit_values = max_root_const + 1u;
        inferred.parameters.push_back(std::move(rc));
    }

    // 2. Inferred CBV/SRV/UAV descriptor table (xbox-emulator #129 mesh shader table inference)
    XboxRootParameter tbl{};
    tbl.type = XboxRootParamType::DescriptorTable;
    tbl.shader_visibility = 0u;  // D3D12_SHADER_VISIBILITY_ALL
    if (uses_smem_cbv && !uses_root_consts && !container.bvh_info.detected) {
        XboxDescriptorRange cbv_r{};
        cbv_r.range_type = XboxDescriptorRangeType::Cbv;
        cbv_r.num_descriptors = 1u;
        cbv_r.base_shader_register = 0u;
        cbv_r.register_space = 0u;
        tbl.descriptor_ranges.push_back(cbv_r);
    }
    if (srv_count > 0u) {
        XboxDescriptorRange srv_r{};
        srv_r.range_type = XboxDescriptorRangeType::Srv;
        srv_r.num_descriptors = srv_count;
        srv_r.base_shader_register = 0u;
        srv_r.register_space = 0u;
        tbl.descriptor_ranges.push_back(srv_r);
    }
    if (uav_count > 0u) {
        XboxDescriptorRange uav_r{};
        uav_r.range_type = XboxDescriptorRangeType::Uav;
        uav_r.num_descriptors = uav_count;
        uav_r.base_shader_register = 0u;
        uav_r.register_space = 0u;
        tbl.descriptor_ranges.push_back(uav_r);
    }
    if (tbl.descriptor_ranges.empty() && mesh_has_user_sgprs) {
        // Mesh shader without RTS0 declaring PM4 user SGPRs: infer a default SRV table (t0)
        XboxDescriptorRange srv_r{};
        srv_r.range_type = XboxDescriptorRangeType::Srv;
        srv_r.num_descriptors = 1u;
        srv_r.base_shader_register = 0u;
        srv_r.register_space = 0u;
        tbl.descriptor_ranges.push_back(srv_r);
    }
    if (!tbl.descriptor_ranges.empty()) {
        inferred.parameters.push_back(std::move(tbl));
    }

    // 3. Inferred Sampler descriptor table if IMAGE_SAMPLE* is used
    if (uses_sampler) {
        XboxRootParameter stbl{};
        stbl.type = XboxRootParamType::DescriptorTable;
        stbl.shader_visibility = 0u;
        XboxDescriptorRange sr{};
        sr.range_type = XboxDescriptorRangeType::Sampler;
        sr.num_descriptors = 1u;
        sr.base_shader_register = 0u;
        sr.register_space = 0u;
        stbl.descriptor_ranges.push_back(sr);
        inferred.parameters.push_back(std::move(stbl));
    }

    container.inferred_root_signature = std::move(inferred);
    container.root_signature_inferred = true;

    const uint32_t max_sgprs =
        (container.reg_setup.present && container.reg_setup.user_sgpr_count > 16u)
            ? std::min(32u, container.reg_setup.user_sgpr_count)
            : 16u;
    (void)BuildUserSgprRootMap(
        container.inferred_root_signature,
        container.reg_setup.present ? &container.reg_setup : nullptr,
        eff_stage,
        container.sgpr_map,
        max_sgprs);
    return true;
}

Status LiftXboxShaderContainer(
    const void* data,
    size_t size,
    XboxContainerLiftResult& out_result,
    const XboxContainerLiftOptions& options) {
    out_result = XboxContainerLiftResult{};
    if (!data || size < 4u) {
        return Status::Error(StatusCode::InvalidArgument, "Empty Xbox shader container buffer");
    }

    if (auto st = ParseFullXboxShaderContainer(data, size, out_result.parsed); !st.ok()) {
        return st;
    }
    if (!out_result.parsed.has_xs2) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Container does not carry an Xbox XS2/RDNA2 shader payload");
    }

    out_result.root_signature_inferred = out_result.parsed.root_signature_inferred;
    out_result.uses_ray_query_bridge = out_result.parsed.bvh_info.detected;
    out_result.lift.container = out_result.parsed.xs2;

    if (auto st = DisassembleRdna2Extended(out_result.parsed.xs2.code_words, out_result.lift.instructions);
        !st.ok()) {
        return st;
    }

    Rdna2LiftOptions eff_opts = options.lift_options;
    if (eff_opts.stage_contract.pixel_inputs.empty() && eff_opts.stage_contract.vertex_fetches.empty()) {
        ApplyXboxContainerToLiftOptions(out_result.parsed, eff_opts);
    }

    // VS v0 (SV_VertexID) + v3 (SV_InstanceID) auto-load prologue (xbox-emulator #125)
    const bool is_vs = (out_result.parsed.xs2.stage == ShaderStage::Vertex);
    const bool want_iid_v3 =
        is_vs && options.enable_vs_system_id_prologue && out_result.parsed.vs_prologue.uses_instance_id_v3;
    const bool want_iid_sig =
        is_vs && options.enable_vs_system_id_prologue &&
        (want_iid_v3 || out_result.parsed.vs_prologue.declared_instance_id_in_sig);

    bool orig_uses_rc = false;
    uint32_t orig_max_rc = 0u;
    uint8_t sentinel_sgpr = 0xFFu;
    std::vector<Rdna2Instruction> build_instructions = out_result.lift.instructions;

    if (want_iid_v3) {
        std::array<bool, 16> sgpr_touched{};
        std::array<bool, 16> sgpr_written{};
        bool has_getpc = false;
        for (const auto& inst : out_result.lift.instructions) {
            if (inst.opcode == Rdna2Opcode::S_GETPC_B64) has_getpc = true;
            for (uint8_t s = 0u; s < inst.src_count && s < 4u; ++s) {
                if (inst.src[s].kind == Rdna2OperandKind::Sgpr && inst.src[s].reg_index < 16u) {
                    const uint32_t idx = inst.src[s].reg_index;
                    sgpr_touched[idx] = true;
                    if (!sgpr_written[idx] && inst.encoding != Rdna2Encoding::SMEM) {
                        orig_uses_rc = true;
                        orig_max_rc = std::max(orig_max_rc, idx);
                    }
                }
            }
            if (inst.encoding == Rdna2Encoding::MUBUF || inst.encoding == Rdna2Encoding::MTBUF ||
                inst.encoding == Rdna2Encoding::MIMG) {
                const uint32_t base = static_cast<uint32_t>(inst.srsrc) * 4u;
                for (uint32_t k = 0u; k < 4u && base + k < 16u; ++k) sgpr_touched[base + k] = true;
            } else if (inst.encoding == Rdna2Encoding::SMEM) {
                const uint32_t base = static_cast<uint32_t>(inst.sbase) * 2u;
                for (uint32_t k = 0u; k < 4u && base + k < 16u; ++k) sgpr_touched[base + k] = true;
            }
            if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index < 16u) {
                sgpr_touched[inst.dst.reg_index] = true;
                sgpr_written[inst.dst.reg_index] = true;
            }
        }
        for (int s = 15; s >= 0; --s) {
            if (!sgpr_touched[static_cast<size_t>(s)]) {
                sentinel_sgpr = static_cast<uint8_t>(s);
                break;
            }
        }
        if (sentinel_sgpr == 0xFFu) {
            sentinel_sgpr = 15u;
        }
        if (!has_getpc) {
            Rdna2Instruction mov_iid{};
            mov_iid.word_offset = 0u;
            mov_iid.word_count = 1u;
            mov_iid.encoding = Rdna2Encoding::VOP1;
            mov_iid.opcode = Rdna2Opcode::V_MOV_B32;
            mov_iid.dst = Rdna2Operand::Vgpr(3u);
            mov_iid.src[0] = Rdna2Operand::Sgpr(sentinel_sgpr);
            mov_iid.src_count = 1u;
            mov_iid.mnemonic = "v_mov_b32";
            mov_iid.disasm = "v_mov_b32 v3, s" + std::to_string(sentinel_sgpr);

            build_instructions.clear();
            build_instructions.reserve(out_result.lift.instructions.size() + 1u);
            build_instructions.push_back(std::move(mov_iid));
            for (auto inst : out_result.lift.instructions) {
                inst.word_offset += 1u;
                build_instructions.push_back(std::move(inst));
            }
            out_result.parsed.vs_prologue.injected_iid_sentinel_sgpr = sentinel_sgpr;
        }
    }

    const StageContractTable* contract_ptr =
        (!eff_opts.stage_contract.vertex_fetches.empty() ||
         !eff_opts.stage_contract.pixel_inputs.empty() ||
         eff_opts.stage_contract.texture_srv_format != Rdna2TextureFormat::Float4)
            ? &eff_opts.stage_contract
            : nullptr;

    if (auto st = BuildRdna2Ir(out_result.lift.container, build_instructions, out_result.lift.ir, contract_ptr);
        !st.ok()) {
        return st;
    }
    out_result.lift.ir.mesh_has_payload = eff_opts.mesh_has_payload;

    // Optional paired VS<->PS interpolation linkage reconciliation
    if (options.paired_other_stage != nullptr) {
        XboxPsoInterpLinkageReport linkage{};
        if (out_result.parsed.xs2.stage == ShaderStage::Vertex &&
            options.paired_other_stage->xs2.stage == ShaderStage::Pixel) {
            (void)ValidateAndMatchStageInterpolation(
                out_result.parsed, *options.paired_other_stage, linkage, &out_result.lift.ir, nullptr);
        } else if (out_result.parsed.xs2.stage == ShaderStage::Pixel &&
                   options.paired_other_stage->xs2.stage == ShaderStage::Vertex) {
            (void)ValidateAndMatchStageInterpolation(
                *options.paired_other_stage, out_result.parsed, linkage, nullptr, &out_result.lift.ir);
        }
    }

    if (auto st = OptimizeRdna2Ir(out_result.lift.ir); !st.ok()) {
        return st;
    }
    out_result.lift.inserted_lockstep_lds_barriers = out_result.lift.ir.inserted_lockstep_lds_barriers;

    const bool stage_supports_spirv =
        (out_result.lift.ir.stage == ShaderStage::Compute ||
         out_result.lift.ir.stage == ShaderStage::Vertex ||
         out_result.lift.ir.stage == ShaderStage::Pixel ||
         out_result.lift.ir.stage == ShaderStage::Amplification ||
         (out_result.lift.ir.stage == ShaderStage::Mesh && eff_opts.emit_mesh_spirv));

    if (eff_opts.emit_spirv && stage_supports_spirv) {
        if (auto st = EmitSpirv(out_result.lift.ir, out_result.lift.spirv_words); !st.ok()) {
            return st;
        }
        if (want_iid_v3 && sentinel_sgpr < 16u) {
            (void)PatchVsSpirvForInstanceIdPrologue(out_result.lift.spirv_words, sentinel_sgpr);
        }
        if (auto st = ValidateSpirvBinary(out_result.lift.spirv_words); !st.ok()) {
            return st;
        }
        if (eff_opts.validate_spirv_external) {
            const std::string env =
                ((out_result.lift.ir.stage == ShaderStage::Mesh ||
                  out_result.lift.ir.stage == ShaderStage::Amplification) &&
                 eff_opts.spirv_target_env == "vulkan1.2")
                    ? "vulkan1.3"
                    : eff_opts.spirv_target_env;
            if (auto st = ValidateSpirvExternal(out_result.lift.spirv_words, out_result.lift.spirv_val_diagnostics,
                                                eff_opts.spirv_val_path, env);
                !st.ok()) {
                return st;
            }
        }
    }

    if (eff_opts.emit_hlsl) {
        if (auto st = EmitHlsl(out_result.lift.ir, out_result.lift.hlsl_source,
                               out_result.lift.hlsl_target, eff_opts.use_sm65);
            !st.ok()) {
            return st;
        }
        if (want_iid_sig) {
            PatchVsHlslForInstanceIdPrologue(
                out_result.lift.hlsl_source, want_iid_v3, sentinel_sgpr, orig_uses_rc, orig_max_rc);
            out_result.vs_v3_instance_id_auto_loaded = true;
        }
        if (is_vs) {
            out_result.vs_v0_auto_loaded =
                out_result.parsed.vs_prologue.uses_vertex_id_v0 ||
                out_result.parsed.vs_prologue.declared_vertex_id_in_sig;
        }
        if (!eff_opts.target_profile_override.empty()) {
            out_result.lift.hlsl_target = eff_opts.target_profile_override;
        }
        if (eff_opts.compile_dxil_with_dxc) {
            Status dxc_st = Status::Error(StatusCode::NotFound, "DXC not invoked");
            if (options.use_in_process_dxc && IsInProcessDxcAvailable(eff_opts.dxc_path)) {
                dxc_st = CompileHlslInProcessDxc(
                    out_result.lift.hlsl_source, out_result.lift.hlsl_target, "main",
                    out_result.lift.pc_dxil, out_result.lift.dxc_diagnostics, eff_opts.dxc_path);
            }
            if (!dxc_st.ok()) {
                dxc_st = CompileHlslWithDxc(
                    out_result.lift.hlsl_source, out_result.lift.hlsl_target, "main",
                    out_result.lift.pc_dxil, out_result.lift.dxc_diagnostics, eff_opts.dxc_path);
            }
            if (!dxc_st.ok()) {
                return dxc_st;
            }
            if (eff_opts.resign_dxil &&
                !VerifyDxbcContainerHash(out_result.lift.pc_dxil.data(), out_result.lift.pc_dxil.size())) {
                if (auto rs = ResignDxilContainer(out_result.lift.pc_dxil); !rs.ok()) {
                    return rs;
                }
            }
        }
    }

    return Status::Ok();
}

}  // namespace openscarlett
