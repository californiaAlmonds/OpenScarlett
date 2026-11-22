#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "openscarlett/core/telemetry.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

std::string HexU32(uint32_t v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08xu", v);
    return buf;
}

uint64_t HashBytesFnv1a(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// SPIR-V 1.3 / 1.4 / 1.6 Constants (Khronos SPIR-V Specification)
// ---------------------------------------------------------------------------
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kSpirvVersion13 = 0x00010300u;
constexpr uint32_t kSpirvVersion14 = 0x00010400u;
constexpr uint32_t kSpirvVersion16 = 0x00010600u;
constexpr uint32_t kSpirvGeneratorOpenScarlett = 0x4F533200u;  // "OS2\0"

enum SpirvOp : uint16_t {
    SpvOpExtension = 10,
    SpvOpExtInstImport = 11,
    SpvOpExtInst = 12,
    SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15,
    SpvOpExecutionMode = 16,
    SpvOpCapability = 17,
    SpvOpTypeVoid = 19,
    SpvOpTypeBool = 20,
    SpvOpTypeInt = 21,
    SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23,
    SpvOpTypeImage = 25,
    SpvOpTypeSampler = 26,
    SpvOpTypeSampledImage = 27,
    SpvOpTypeArray = 28,
    SpvOpTypeRuntimeArray = 29,
    SpvOpTypeStruct = 30,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstantTrue = 41,
    SpvOpConstantFalse = 42,
    SpvOpConstant = 43,
    SpvOpConstantNull = 46,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpImageTexelPointer = 60,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpCompositeConstruct = 80,
    SpvOpCompositeExtract = 81,
    SpvOpSampledImage = 86,
    SpvOpImageSampleExplicitLod = 88,
    SpvOpImageFetch = 95,
    SpvOpImageRead = 98,
    SpvOpImageWrite = 99,
    SpvOpConvertFToU = 109,
    SpvOpConvertFToS = 110,
    SpvOpConvertSToF = 111,
    SpvOpConvertUToF = 112,
    SpvOpBitcast = 124,
    SpvOpFNegate = 127,
    SpvOpIAdd = 128,
    SpvOpFAdd = 129,
    SpvOpISub = 130,
    SpvOpFSub = 131,
    SpvOpIMul = 132,
    SpvOpFMul = 133,
    SpvOpFDiv = 136,
    SpvOpDot = 148,
    SpvOpIsNan = 156,
    SpvOpLogicalOr = 166,
    SpvOpLogicalAnd = 167,
    SpvOpLogicalNot = 168,
    SpvOpSelect = 169,
    SpvOpIEqual = 170,
    SpvOpINotEqual = 171,
    SpvOpUGreaterThan = 172,
    SpvOpSGreaterThan = 173,
    SpvOpUGreaterThanEqual = 174,
    SpvOpSGreaterThanEqual = 175,
    SpvOpULessThan = 176,
    SpvOpSLessThan = 177,
    SpvOpFOrdEqual = 180,
    SpvOpFUnordEqual = 181,
    SpvOpFOrdNotEqual = 182,
    SpvOpFUnordNotEqual = 183,
    SpvOpFOrdLessThan = 184,
    SpvOpFUnordLessThan = 185,
    SpvOpFOrdGreaterThan = 186,
    SpvOpFUnordGreaterThan = 187,
    SpvOpFOrdLessThanEqual = 188,
    SpvOpFUnordLessThanEqual = 189,
    SpvOpFOrdGreaterThanEqual = 190,
    SpvOpFUnordGreaterThanEqual = 191,
    SpvOpShiftRightLogical = 194,
    SpvOpShiftRightArithmetic = 195,
    SpvOpShiftLeftLogical = 196,
    SpvOpBitwiseOr = 197,
    SpvOpBitwiseXor = 198,
    SpvOpBitwiseAnd = 199,
    SpvOpNot = 200,
    SpvOpBitCount = 205,
    SpvOpDPdx = 207,
    SpvOpDPdy = 208,
    SpvOpControlBarrier = 224,
    SpvOpAtomicCompareExchange = 230,
    SpvOpAtomicIAdd = 234,
    SpvOpAtomicAnd = 240,
    SpvOpAtomicOr = 241,
    SpvOpPhi = 245,
    SpvOpLoopMerge = 246,
    SpvOpSelectionMerge = 247,
    SpvOpLabel = 248,
    SpvOpBranch = 249,
    SpvOpBranchConditional = 250,
    SpvOpSwitch = 251,
    SpvOpKill = 252,
    SpvOpReturn = 253,
    SpvOpEmitMeshTasksEXT = 5294,
    SpvOpSetMeshOutputsEXT = 5295,
};

enum GlslStd450Op : uint32_t {
    GlslRoundEven = 2,
    GlslFAbs = 4,
    GlslFloor = 8,
    GlslCeil = 9,
    GlslFract = 10,
    GlslSqrt = 31,
    GlslInverseSqrt = 32,
    GlslFMin = 37,
    GlslUMin = 38,
    GlslSMin = 39,
    GlslFMax = 40,
    GlslUMax = 41,
    GlslSMax = 42,
    GlslFma = 50,
    GlslPackHalf2x16 = 58,
    GlslUnpackHalf2x16 = 62,
};

// SPIR-V Storage Classes
constexpr uint32_t kSpvStorageUniformConstant = 0u;
constexpr uint32_t kSpvStorageInput = 1u;
constexpr uint32_t kSpvStorageUniform = 2u;
constexpr uint32_t kSpvStorageOutput = 3u;
constexpr uint32_t kSpvStorageWorkgroup = 4u;
constexpr uint32_t kSpvStoragePrivate = 6u;
constexpr uint32_t kSpvStorageFunction = 7u;
constexpr uint32_t kSpvStorageImage = 11u;
constexpr uint32_t kSpvStorageStorageBuffer = 12u;
constexpr uint32_t kSpvStorageTaskPayloadWorkgroupEXT = 5402u;

// SPIR-V Image Formats
constexpr uint32_t kSpvImgFmtUnknown = 0u;
constexpr uint32_t kSpvImgFmtRgba32f = 1u;
constexpr uint32_t kSpvImgFmtRgba32ui = 30u;
constexpr uint32_t kSpvImgFmtR32ui = 33u;

// SPIR-V Decorations
constexpr uint32_t kSpvDecBlock = 2u;
constexpr uint32_t kSpvDecArrayStride = 6u;
constexpr uint32_t kSpvDecBuiltIn = 11u;
constexpr uint32_t kSpvDecNoPerspective = 13u;
constexpr uint32_t kSpvDecFlat = 14u;
constexpr uint32_t kSpvDecCentroid = 16u;
constexpr uint32_t kSpvDecSample = 17u;
constexpr uint32_t kSpvDecNonWritable = 24u;
constexpr uint32_t kSpvDecNonReadable = 25u;
constexpr uint32_t kSpvDecLocation = 30u;
constexpr uint32_t kSpvDecBinding = 33u;
constexpr uint32_t kSpvDecDescriptorSet = 34u;
constexpr uint32_t kSpvDecOffset = 35u;
constexpr uint32_t kSpvDecNoContraction = 42u;
constexpr uint32_t kSpvDecPerPrimitiveEXT = 5271u;

// SPIR-V BuiltIns
constexpr uint32_t kSpvBuiltInPosition = 0u;
constexpr uint32_t kSpvBuiltInCullDistance = 4u;
constexpr uint32_t kSpvBuiltInWorkgroupId = 26u;
constexpr uint32_t kSpvBuiltInGlobalInvocationId = 28u;
constexpr uint32_t kSpvBuiltInLocalInvocationIndex = 29u;
constexpr uint32_t kSpvBuiltInVertexIndex = 42u;
constexpr uint32_t kSpvBuiltInPrimitiveTriangleIndicesEXT = 5294u;
constexpr uint32_t kSpvBuiltInCullPrimitiveEXT = 5299u;

// SPIR-V Capabilities & Mesh/Task Shader EXT constants
constexpr uint32_t kSpvCapCullDistance = 45u;
constexpr uint32_t kSpvCapMeshShadingEXT = 5283u;
constexpr uint32_t kSpvExecModelTaskEXT = 5364u;
constexpr uint32_t kSpvExecModelMeshEXT = 5365u;
constexpr uint32_t kSpvExecModeOutputVertices = 26u;
constexpr uint32_t kSpvExecModeOutputPrimitivesEXT = 5270u;
constexpr uint32_t kSpvExecModeOutputTrianglesEXT = 5298u;

const char* HlslInterpModifier(Rdna2InterpMode mode) noexcept {
    switch (mode) {
        case Rdna2InterpMode::NoPerspective: return "noperspective ";
        case Rdna2InterpMode::Flat:          return "nointerpolation ";
        case Rdna2InterpMode::Centroid:      return "centroid ";
        case Rdna2InterpMode::Sample:        return "sample ";
        case Rdna2InterpMode::Perspective:
        default:                             return "";
    }
}

uint32_t ComputeMaxGuestWaves(const Rdna2IrModule& ir) noexcept {
    const uint32_t tg = std::max(1u, ir.threadgroup_x * ir.threadgroup_y * ir.threadgroup_z);
    const uint32_t ws = (ir.wave_size == 64u) ? 64u : 32u;
    return std::max(1u, (tg + ws - 1u) / ws);
}

// SPIR-V Scopes & Semantics
constexpr uint32_t kSpvScopeDevice = 1u;
constexpr uint32_t kSpvScopeWorkgroup = 2u;
constexpr uint32_t kSpvMemSemNone = 0u;
constexpr uint32_t kSpvMemSemAcqRelWorkgroup = 0x108u;

void EmitSpvInst(std::vector<uint32_t>& buf, uint16_t opcode, const std::vector<uint32_t>& operands) {
    const uint32_t wc = static_cast<uint32_t>(operands.size() + 1u);
    buf.push_back((wc << 16) | static_cast<uint32_t>(opcode));
    buf.insert(buf.end(), operands.begin(), operands.end());
}

void AppendSpvString(std::vector<uint32_t>& operands, const char* str) {
    const size_t len = std::strlen(str) + 1;
    const size_t words = (len + 3) / 4;
    for (size_t w = 0; w < words; ++w) {
        uint32_t word = 0;
        for (size_t b = 0; b < 4; ++b) {
            const size_t idx = w * 4 + b;
            if (idx < len) {
                word |= static_cast<uint32_t>(static_cast<uint8_t>(str[idx])) << (b * 8);
            }
        }
        operands.push_back(word);
    }
}

bool ModuleUsesStageInput(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::StageInput) return true;
        }
    }
    return false;
}

uint32_t ModuleParamOutputCount(const Rdna2IrModule& ir) noexcept {
    uint32_t count = ir.exports_param0 ? 1u : 0u;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Export &&
                inst.export_target >= static_cast<uint8_t>(Rdna2ExportTarget::Param0) &&
                inst.export_target <= static_cast<uint8_t>(Rdna2ExportTarget::Param3)) {
                const uint32_t slot = static_cast<uint32_t>(
                    inst.export_target - static_cast<uint8_t>(Rdna2ExportTarget::Param0));
                if (slot + 1u > count) count = slot + 1u;
            }
        }
    }
    if (count > 0u && ir.output_interp_modes.size() > count) {
        count = std::min<uint32_t>(4u, static_cast<uint32_t>(ir.output_interp_modes.size()));
    }
    return std::min<uint32_t>(4u, count);
}

uint32_t ModuleParamInputCount(const Rdna2IrModule& ir) noexcept {
    uint32_t count = 0u;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::StageInput) {
                const uint32_t slot = (inst.imm_u32 >> 2) & 3u;
                if (slot + 1u > count) count = slot + 1u;
            }
        }
    }
    return std::min<uint32_t>(4u, count);
}

Rdna2InterpMode SafeInterpMode(const std::vector<Rdna2InterpMode>& modes, uint32_t slot) noexcept {
    return (slot < modes.size()) ? modes[slot] : Rdna2InterpMode::Perspective;
}

constexpr uint32_t kIrKillExportMagicLifter = 0xDEAD0001u;
constexpr uint32_t kIrLoadCbHeapFlagLifter = 0x80000000u;

bool ModuleUsesThreadId(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::ThreadIdX) return true;
        }
    }
    return false;
}

bool ModuleHasCondBranch(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        if (!bb.instructions.empty() && bb.instructions.back().op == IrOp::CondBranch) {
            return true;
        }
    }
    return false;
}

bool ModuleUsesOp(const Rdna2IrModule& ir, IrOp target_op) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == target_op) return true;
        }
    }
    return false;
}

uint32_t ModuleCullDistanceCount(const Rdna2IrModule& ir) noexcept {
    uint32_t count = 0u;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op != IrOp::Export) continue;
            if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos3)) {
                return 8u;
            }
            if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos2)) {
                count = 4u;
            }
        }
    }
    return count;
}

enum class SpvValKind : uint8_t {
    U32,
    F32,
    Bool,
};

SpvValKind CanonicalSpvKind(IrType t) {
    if (t == IrType::F32) return SpvValKind::F32;
    if (t == IrType::Bool) return SpvValKind::Bool;
    return SpvValKind::U32;
}

// Computes immediate post-dominators for each block in [0..N-1], where N is the virtual exit node.
std::vector<uint32_t> ComputeImmediatePostDominators(const Rdna2IrModule& ir) {
    const size_t n = ir.blocks.size();
    const uint32_t exit_node = static_cast<uint32_t>(n);

    std::vector<std::vector<uint32_t>> succs(n);
    for (size_t b = 0; b < n; ++b) {
        const auto& ins = ir.blocks[b].instructions;
        if (ins.empty() || ins.back().op == IrOp::Return) {
            succs[b].push_back(exit_node);
        } else if (ins.back().op == IrOp::Branch) {
            succs[b].push_back(ins.back().true_block);
        } else if (ins.back().op == IrOp::CondBranch) {
            succs[b].push_back(ins.back().true_block);
            if (ins.back().false_block != ins.back().true_block) {
                succs[b].push_back(ins.back().false_block);
            }
        } else {
            succs[b].push_back(exit_node);
        }
    }

    std::vector<std::vector<uint8_t>> pdom(n + 1, std::vector<uint8_t>(n + 1, 1u));
    pdom[exit_node].assign(n + 1, 0u);
    pdom[exit_node][exit_node] = 1u;

    bool changed = true;
    for (size_t iter = 0; changed && iter < (n + 2) * 8; ++iter) {
        changed = false;
        for (size_t b = n; b-- > 0;) {
            std::vector<uint8_t> next_set(n + 1, 1u);
            if (succs[b].empty()) {
                next_set.assign(n + 1, 0u);
            } else {
                for (uint32_t s : succs[b]) {
                    if (s > exit_node) continue;
                    for (size_t k = 0; k <= n; ++k) {
                        next_set[k] = static_cast<uint8_t>(next_set[k] & pdom[s][k]);
                    }
                }
            }
            next_set[b] = 1u;
            if (next_set != pdom[b]) {
                pdom[b] = std::move(next_set);
                changed = true;
            }
        }
    }

    std::vector<size_t> pdom_size(n + 1, 0);
    for (size_t i = 0; i <= n; ++i) {
        size_t c = 0;
        for (uint8_t bit : pdom[i]) c += bit;
        pdom_size[i] = c;
    }

    std::vector<uint32_t> ipdom(n, exit_node);
    for (size_t b = 0; b < n; ++b) {
        uint32_t best = exit_node;
        size_t best_size = 0;
        for (size_t d = 0; d <= n; ++d) {
            if (d == b || !pdom[b][d]) continue;
            if (pdom_size[d] > best_size) {
                best_size = pdom_size[d];
                best = static_cast<uint32_t>(d);
            }
        }
        ipdom[b] = best;
    }
    return ipdom;
}

std::string MakeUniqueTempPath(const char* prefix, const char* ext) {
    static std::atomic<uint64_t> g_seq{1};
    std::error_code ec;
    std::filesystem::path temp_dir;
#if defined(_WIN32)
    char env_buf[1024]{};
    DWORD env_len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (env_len == 0 || env_len >= sizeof(env_buf)) {
        env_len = GetEnvironmentVariableA("TMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    }
    if (env_len > 0 && env_len < sizeof(env_buf)) {
        temp_dir = std::filesystem::path(env_buf) / "openscarlett_lifter";
    } else if (std::filesystem::exists("E:\\OpenScarlett", ec)) {
        temp_dir = std::filesystem::path(kLifterTempDir);
    } else {
        temp_dir = std::filesystem::temp_directory_path(ec) / "openscarlett_lifter";
    }
#else
    temp_dir = std::filesystem::temp_directory_path(ec) / "openscarlett_lifter";
#endif
    std::filesystem::create_directories(temp_dir, ec);
    uint32_t pid = 0;
#if defined(_WIN32)
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#endif
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << temp_dir.string() << "\\" << prefix << "_" << pid << "_" << seq << ext;
    return oss.str();
}

uint32_t ComputeWaveLanesCapacity(const Rdna2IrModule& ir) noexcept {
    const uint32_t tg = ir.threadgroup_x * ir.threadgroup_y * ir.threadgroup_z;
    uint32_t cap = 64u;
    while (cap < tg && cap < 1024u) {
        cap <<= 1;
    }
    return cap;
}

#if defined(_WIN32)
std::string ResolveDxcExecutable(const std::string& requested_path) {
    std::error_code ec;
    // 1. Prefer bundled dxc.exe right beside the running executable (portable tester bundles)
    char self_buf[MAX_PATH]{};
    const DWORD self_len = GetModuleFileNameA(nullptr, self_buf, static_cast<DWORD>(sizeof(self_buf)));
    if (self_len > 0 && self_len < sizeof(self_buf)) {
        const auto beside_exe = std::filesystem::path(self_buf).parent_path() / "dxc.exe";
        if (std::filesystem::exists(beside_exe, ec)) {
            return beside_exe.string();
        }
    }
    // 2. Check OpenScarlett shim/bundle directory (for APC-hooked guests running outside the bundle dir)
    char shim_dir_buf[1024]{};
    const DWORD shim_len =
        GetEnvironmentVariableA("OPENSCARLETT_SHIM_DIR", shim_dir_buf, static_cast<DWORD>(sizeof(shim_dir_buf)));
    if (shim_len > 0 && shim_len < sizeof(shim_dir_buf)) {
        const auto beside_shims = std::filesystem::path(std::string(shim_dir_buf, shim_len)) / "dxc.exe";
        if (std::filesystem::exists(beside_shims, ec)) {
            return beside_shims.string();
        }
    }
    // 3. Explicit environment override
    char env_dxc[1024]{};
    const DWORD env_len = GetEnvironmentVariableA("OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(sizeof(env_dxc)));
    if (env_len > 0 && env_len < sizeof(env_dxc) && std::filesystem::exists(env_dxc, ec)) {
        return std::string(env_dxc);
    }
    // 3. Explicit caller-requested path
    if (!requested_path.empty() && std::filesystem::exists(requested_path, ec)) {
        return requested_path;
    }
    // 4. System PATH
    char path_buf[MAX_PATH]{};
    const DWORD path_len = SearchPathA(nullptr, "dxc.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
    if (path_len > 0 && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, ec)) {
        return std::string(path_buf);
    }
    // 5. Windows 10/11 SDK fallback
    const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
    if (std::filesystem::exists(kits_bin, ec)) {
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory()) continue;
            const auto cand = entry.path() / "x64" / "dxc.exe";
            if (std::filesystem::exists(cand, ec)) {
                found.push_back(cand);
            }
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) {
            return found.back().string();
        }
    }
    return requested_path;
}
#endif

#if defined(_WIN32)
Status RunExternalProcess(const std::string& cmdline, uint32_t& out_exit_code, std::string& out_output) {
    out_exit_code = 1;
    out_output.clear();

    // Guard CreatePipe + CreateProcessA and restrict handle inheritance strictly to h_write
    // via PROC_THREAD_ATTRIBUTE_HANDLE_LIST so concurrent worker threads (e.g. AsyncPsoCompilePool)
    // never inherit another thread's open temp .hlsl/.dxil or pipe handles.
    // Poison-aware RAII tracking ensures any exception/unwind releases the mutex and closes
    // pipe handles cleanly while allowing subsequent calls to recover automatically.
    static std::mutex g_spawn_mutex;
    static std::atomic<bool> g_spawn_mutex_poisoned{false};
    HANDLE h_read = nullptr;
    HANDLE h_write = nullptr;
    PROCESS_INFORMATION pi{};
    BOOL ok = FALSE;
    DWORD spawn_err = 0;

    {
        std::unique_lock<std::mutex> spawn_lock(g_spawn_mutex);
        if (g_spawn_mutex_poisoned.exchange(false, std::memory_order_acq_rel)) {
            CallTelemetry::Global().Record("rdna2_lifter", "SpawnMutexRecoveredFromPoison", CallOutcome::Ok, 1);
        }
        bool spawn_committed = false;
        struct SpawnCleanupGuard {
            bool& committed;
            HANDLE& hw;
            HANDLE& hr;
            ~SpawnCleanupGuard() {
                if (hw != nullptr) {
                    CloseHandle(hw);
                    hw = nullptr;
                }
                if (!committed) {
                    if (hr != nullptr) {
                        CloseHandle(hr);
                        hr = nullptr;
                    }
                    g_spawn_mutex_poisoned.store(true, std::memory_order_release);
                    CallTelemetry::Global().Record("rdna2_lifter", "SpawnMutexPoisoned", CallOutcome::Error, -1);
                }
            }
        } cleanup{spawn_committed, h_write, h_read};

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&h_read, &h_write, &sa, 0)) {
            spawn_committed = true;
            return Status::Error(StatusCode::IoError, "CreatePipe failed for external shader tool");
        }
        SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOEXA si_ex{};
        si_ex.StartupInfo.cb = sizeof(si_ex);
        si_ex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si_ex.StartupInfo.hStdOutput = h_write;
        si_ex.StartupInfo.hStdError = h_write;
        si_ex.StartupInfo.hStdInput = nullptr;

        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        std::vector<uint8_t> attr_buf(attr_size);
        // Must outlive CreateProcessA and DeleteProcThreadAttributeList (UpdateProcThreadAttribute stores pointer).
        HANDLE inherit_list[1] = {h_write};
        bool has_attr_list = false;
        if (attr_size > 0 &&
            InitializeProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data()), 1, 0, &attr_size)) {
            if (UpdateProcThreadAttribute(
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data()),
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherit_list,
                    sizeof(inherit_list),
                    nullptr,
                    nullptr)) {
                si_ex.lpAttributeList =
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
                has_attr_list = true;
            } else {
                DeleteProcThreadAttributeList(
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data()));
            }
        }

        std::string mutable_cmd = cmdline;
        if (has_attr_list) {
            ok = CreateProcessA(
                nullptr,
                mutable_cmd.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                nullptr,
                &si_ex.StartupInfo,
                &pi);
            spawn_err = ok ? 0 : GetLastError();
            DeleteProcThreadAttributeList(si_ex.lpAttributeList);
        } else {
            si_ex.StartupInfo.cb = sizeof(STARTUPINFOA);
            ok = CreateProcessA(
                nullptr,
                mutable_cmd.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si_ex.StartupInfo,
                &pi);
            spawn_err = ok ? 0 : GetLastError();
        }

        CloseHandle(h_write);
        h_write = nullptr;
        spawn_committed = true;
    }

    if (!ok) {
        CloseHandle(h_read);
        return Status::Error(StatusCode::IoError,
                             "CreateProcessA failed (err=" + std::to_string(spawn_err) + ") for: " + cmdline);
    }

    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(h_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        out_output.append(buf, bytes_read);
    }
    CloseHandle(h_read);

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    out_exit_code = static_cast<uint32_t>(code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Status::Ok();
}
#endif

// ---------------------------------------------------------------------------
// HLSL Structured Emitter (SM 6.5 & SM 5.0)
// ---------------------------------------------------------------------------
class HlslEmitter {
public:
    HlslEmitter(const Rdna2IrModule& ir, bool use_sm65) : ir_(ir), use_sm65_(use_sm65) {
        for (const auto& bb : ir_.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.id != kInvalidIrValue) {
                    defs_[inst.id] = &inst;
                }
            }
        }
    }

    Status Emit(std::string& out_hlsl, std::string& out_target) {
        if (ir_.blocks.empty()) {
            return Status::Error(StatusCode::ShaderError, "Cannot emit HLSL from empty IR module");
        }

        const bool uses_stage_in = ModuleUsesStageInput(ir_);
        const bool uses_dual_uav = ir_.uses_uav1;
        const bool uses_uav0 = ir_.uses_uav0 || (ir_.uses_uav && !ir_.uses_uav1);
        const bool uses_fma = ModuleUsesOp(ir_, IrOp::FFma) || ModuleUsesOp(ir_, IrOp::FFma16);
        const bool uses_mad = ModuleUsesOp(ir_, IrOp::FMad);
        const bool uses_alignbyte = ModuleUsesOp(ir_, IrOp::AlignByte);
        const bool uses_buf_load_fmt = ModuleUsesOp(ir_, IrOp::BufferLoadFormat);
        const bool uses_cvt_norm =
            ModuleUsesOp(ir_, IrOp::CvtPkNormI16F32) || ModuleUsesOp(ir_, IrOp::CvtPkNormU16F32) ||
            ModuleUsesOp(ir_, IrOp::CvtNormI16F16) || ModuleUsesOp(ir_, IrOp::CvtNormU16F16);
        const bool uses_exact_fp =
            ModuleUsesOp(ir_, IrOp::FFloor) || ModuleUsesOp(ir_, IrOp::FCeil) ||
            ModuleUsesOp(ir_, IrOp::FFract) || ModuleUsesOp(ir_, IrOp::FMin) ||
            ModuleUsesOp(ir_, IrOp::FMax);
        const bool uses_ps_quad_deriv =
            (ir_.stage == ShaderStage::Pixel) &&
            (ir_.uses_pixel_quad_derivatives || ModuleUsesOp(ir_, IrOp::ImageSampleImplicit));
        const bool is_group_stage =
            (ir_.stage == ShaderStage::Compute || ir_.stage == ShaderStage::Mesh ||
             ir_.stage == ShaderStage::Amplification);
        bounded_wave64_dispatcher_ =
            (ir_.wave_size == 64u) &&
            (ir_.stage == ShaderStage::Mesh || ir_.stage == ShaderStage::Amplification ||
             ir_.stage == ShaderStage::Geometry);
        hoisted_mesh_counts_ = false;

        const uint32_t orig_tg = std::max(1u, ir_.threadgroup_x * ir_.threadgroup_y * ir_.threadgroup_z);
        const uint32_t hw_tg_x = (bounded_wave64_dispatcher_ && is_group_stage)
                                     ? ((orig_tg <= 64u) ? 32u : std::max(32u, (orig_tg + 1u) / 2u))
                                     : ir_.threadgroup_x;
        const uint32_t hw_tg_y = (bounded_wave64_dispatcher_ && is_group_stage) ? 1u : ir_.threadgroup_y;
        const uint32_t hw_tg_z = (bounded_wave64_dispatcher_ && is_group_stage) ? 1u : ir_.threadgroup_z;
        const uint32_t hw_tg_total = std::max(1u, hw_tg_x * hw_tg_y * hw_tg_z);

        std::ostringstream hdr;
        hdr << "// Generated by OpenScarlett RDNA2 (XS2) -> HLSL Lifter\n";

        if (ir_.uses_root_constants) {
            hdr << "cbuffer OpenScarlettRootConstants : register(b0) {\n";
            for (uint32_t i = 0; i <= ir_.max_root_const_index; ++i) {
                hdr << "    uint _rc" << i << ";\n";
            }
            hdr << "};\n";
        }
        // Distinct CB heap register space (space1) for CB loads with non-zero base SGPR offset
        // so they never collide with mixed-heap V# slot 0 (u0, space0) or root constants (b0, space0).
        {
            std::vector<uint32_t> cb_heap_regs;
            for (const auto& bb : ir_.blocks) {
                for (const auto& inst : bb.instructions) {
                    if (inst.op == IrOp::LoadUav && (inst.imm_u32 & kIrLoadCbHeapFlagLifter) != 0u) {
                        const uint32_t reg = inst.imm_u32 & 0xFFu;
                        if (std::find(cb_heap_regs.begin(), cb_heap_regs.end(), reg) == cb_heap_regs.end()) {
                            cb_heap_regs.push_back(reg);
                        }
                    }
                }
            }
            std::sort(cb_heap_regs.begin(), cb_heap_regs.end());
            for (uint32_t reg : cb_heap_regs) {
                hdr << "cbuffer OpenScarlettCbHeap" << reg << " : register(b" << reg << ", space1) {\n"
                    << "    uint4 _cb_heap_" << reg << "[4096];\n"
                    << "};\n";
            }
        }
        if (uses_uav0) {
            hdr << "RWByteAddressBuffer _in0 : register(u0);\n";
        }
        if (uses_dual_uav) {
            hdr << "RWByteAddressBuffer _out0 : register(u1);\n";
        }
        uint32_t next_t = 0u;
        if (ir_.uses_texture_srv) {
            const char* tex_elem = (ir_.texture_srv_format == Rdna2TextureFormat::Uint4) ? "uint4" : "float4";
            hdr << "Texture2D<" << tex_elem << "> _tex0 : register(t" << next_t++ << ");\n";
        }
        if (ir_.uses_texture3d_srv) {
            hdr << "Texture3D<float4> _tex3d0 : register(t" << next_t++ << ");\n";
        }
        if (ir_.uses_texture_cube_srv) {
            hdr << "Texture2DArray<float4> _texcube0 : register(t" << next_t++ << ");\n";
        }
        if (ir_.uses_texture_sample) {
            hdr << "SamplerState _samp0 : register(s0);\n";
        }
        uint32_t next_rw_u = uses_dual_uav ? 2u : 1u;
        if (ir_.uses_texture_uav) {
            hdr << "RWTexture2D<float4> _rwtex0 : register(u" << next_rw_u++ << ");\n";
        }
        if (ir_.uses_texture_u32_uav) {
            if (ir_.texture_u32_uav_dim == 2u) {
                hdr << "RWTexture3D<uint> _rwimg_u32 : register(u" << next_rw_u++ << ");\n";
            } else if (ir_.texture_u32_uav_dim == 3u) {
                hdr << "RWTexture2DArray<uint> _rwimg_u32 : register(u" << next_rw_u++ << ");\n";
            } else {
                hdr << "RWTexture2D<uint> _rwimg_u32 : register(u" << next_rw_u++ << ");\n";
            }
        }
        if (ir_.uses_lds && is_group_stage) {
            const uint32_t lds_n = ir_.lds_size_dwords ? ir_.lds_size_dwords : 1024u;
            hdr << "groupshared uint _lds[" << lds_n << "];\n";
        }
        if ((ir_.uses_wave64_emulation || bounded_wave64_dispatcher_) && is_group_stage) {
            const uint32_t w_cap = ComputeWaveLanesCapacity(ir_);
            const uint32_t max_waves = ComputeMaxGuestWaves(ir_);
            hdr << "groupshared uint _w64_ballot[" << (max_waves * 2u) << "];\n";
            hdr << "groupshared uint _w64_lanes[" << w_cap << "];\n";
        }
        if (uses_alignbyte) {
            hdr << "// UB-free v_alignbyte_b32 helper (all shift amounts strictly < 32)\n";
            hdr << "uint _os_alignbyte_b32(uint hi, uint lo, uint byte_sel) {\n"
                   "    uint rsh = (byte_sel & 3u) << 3u;\n"
                   "    uint lsh = (32u - rsh) & 31u;\n"
                   "    return (lo >> rsh) | ((rsh != 0u) ? (hi << lsh) : 0u);\n"
                   "}\n";
        }
        if (uses_cvt_norm) {
            hdr << "// AMD RDNA2 normalized 16-bit integer conversion helpers (NaN->0, clamp, round-to-nearest-even)\n";
            hdr << "uint _os_cvt_snorm16_f32(float x) {\n"
                   "    if ((asuint(x) & 0x7FFFFFFFu) > 0x7F800000u) return 0u;\n"
                   "    float c = (x >= 1.0) ? 1.0 : ((x <= -1.0) ? -1.0 : x);\n"
                   "    int v = clamp((int)round(c * 32767.0), -32767, 32767);\n"
                   "    return asuint(v) & 0xFFFFu;\n"
                   "}\n";
            hdr << "uint _os_cvt_unorm16_f32(float x) {\n"
                   "    if ((asuint(x) & 0x7FFFFFFFu) > 0x7F800000u) return 0u;\n"
                   "    float c = (x >= 1.0) ? 1.0 : ((x <= 0.0) ? 0.0 : x);\n"
                   "    int v = clamp((int)round(c * 65535.0), 0, 65535);\n"
                   "    return asuint(v) & 0xFFFFu;\n"
                   "}\n";
        }
        if (uses_exact_fp) {
            hdr << "// IEEE/ISA-exact NaN quieting + denormal-preserving floor/ceil helpers\n";
            hdr << "float _os_quiet_nan(float x) {\n"
                   "    uint b = asuint(x);\n"
                   "    return ((b & 0x7FFFFFFFu) > 0x7F800000u) ? asfloat(b | 0x00400000u) : x;\n"
                   "}\n";
            hdr << "float _os_floor_f32(float x) {\n"
                   "    uint b = asuint(x);\n"
                   "    if ((b & 0x7FFFFFFFu) > 0x7F800000u) return asfloat(b | 0x00400000u);\n"
                   "    if (((b & 0x7F800000u) == 0u) && ((b & 0x007FFFFFu) != 0u)) {\n"
                   "        return ((b & 0x80000000u) != 0u) ? -1.0 : 0.0;\n"
                   "    }\n"
                   "    return floor(x);\n"
                   "}\n";
            hdr << "float _os_ceil_f32(float x) {\n"
                   "    uint b = asuint(x);\n"
                   "    if ((b & 0x7FFFFFFFu) > 0x7F800000u) return asfloat(b | 0x00400000u);\n"
                   "    if (((b & 0x7F800000u) == 0u) && ((b & 0x007FFFFFu) != 0u)) {\n"
                   "        return ((b & 0x80000000u) != 0u) ? asfloat(0x80000000u) : 1.0;\n"
                   "    }\n"
                   "    return ceil(x);\n"
                   "}\n";
            hdr << "float _os_fmin_f32(float a, float b) {\n"
                   "    uint ba = asuint(a); uint bb = asuint(b);\n"
                   "    bool na = (ba & 0x7FFFFFFFu) > 0x7F800000u;\n"
                   "    bool nb = (bb & 0x7FFFFFFFu) > 0x7F800000u;\n"
                   "    if (na && nb) return asfloat(ba | 0x00400000u);\n"
                   "    if (na) return b;\n"
                   "    if (nb) return a;\n"
                   "    return min(a, b);\n"
                   "}\n";
            hdr << "float _os_fmax_f32(float a, float b) {\n"
                   "    uint ba = asuint(a); uint bb = asuint(b);\n"
                   "    bool na = (ba & 0x7FFFFFFFu) > 0x7F800000u;\n"
                   "    bool nb = (bb & 0x7FFFFFFFu) > 0x7F800000u;\n"
                   "    if (na && nb) return asfloat(ba | 0x00400000u);\n"
                   "    if (na) return b;\n"
                   "    if (nb) return a;\n"
                   "    return max(a, b);\n"
                   "}\n";
        }
        if (uses_buf_load_fmt) {
            hdr << "// UB-free sub-dword merge & MTBUF D16 format-conversion helpers (shifts strictly < 32)\n";
            if (uses_uav0) {
                hdr << "uint _os_load_unaligned_u0(uint byte_addr) {\n"
                       "    uint aligned = byte_addr & ~3u;\n"
                       "    uint bit_off = (byte_addr & 3u) << 3u;\n"
                       "    uint w0 = _in0.Load(aligned);\n"
                       "    uint w1 = (bit_off != 0u) ? _in0.Load(aligned + 4u) : 0u;\n"
                       "    uint hi_sh = (32u - bit_off) & 31u;\n"
                       "    return (w0 >> bit_off) | ((bit_off != 0u) ? (w1 << hi_sh) : 0u);\n"
                       "}\n";
            }
            if (uses_dual_uav) {
                hdr << "uint _os_load_unaligned_u1(uint byte_addr) {\n"
                       "    uint aligned = byte_addr & ~3u;\n"
                       "    uint bit_off = (byte_addr & 3u) << 3u;\n"
                       "    uint w0 = _out0.Load(aligned);\n"
                       "    uint w1 = (bit_off != 0u) ? _out0.Load(aligned + 4u) : 0u;\n"
                       "    uint hi_sh = (32u - bit_off) & 31u;\n"
                       "    return (w0 >> bit_off) | ((bit_off != 0u) ? (w1 << hi_sh) : 0u);\n"
                       "}\n";
            }
            hdr << "uint _os_decode_fmt_comp(uint raw, uint fmt, bool as_d16) {\n"
                   "    if (fmt == 1u || fmt == 10u || fmt == 28u) {\n"
                   "        float f = (float)(raw & 0xFFu) / 255.0;\n"
                   "        return as_d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f);\n"
                   "    }\n"
                   "    if (fmt == 2u || fmt == 11u || fmt == 29u) {\n"
                   "        int s = ((int)((raw & 0xFFu) << 24u)) >> 24;\n"
                   "        float f = clamp((float)s / 127.0, -1.0, 1.0);\n"
                   "        return as_d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f);\n"
                   "    }\n"
                   "    if (fmt == 3u || fmt == 12u || fmt == 30u) {\n"
                   "        return raw & 0xFFu;\n"
                   "    }\n"
                   "    if (fmt == 4u || fmt == 13u || fmt == 31u) {\n"
                   "        uint s = asuint(((int)((raw & 0xFFu) << 24u)) >> 24);\n"
                   "        return as_d16 ? (s & 0xFFFFu) : s;\n"
                   "    }\n"
                   "    if (fmt == 5u || fmt == 17u || fmt == 47u) {\n"
                   "        float f = (float)(raw & 0xFFFFu) / 65535.0;\n"
                   "        return as_d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f);\n"
                   "    }\n"
                   "    if (fmt == 6u || fmt == 18u || fmt == 48u) {\n"
                   "        int s = ((int)((raw & 0xFFFFu) << 16u)) >> 16;\n"
                   "        float f = clamp((float)s / 32767.0, -1.0, 1.0);\n"
                   "        return as_d16 ? (f32tof16(f) & 0xFFFFu) : asuint(f);\n"
                   "    }\n"
                   "    if (fmt == 7u || fmt == 19u || fmt == 49u) {\n"
                   "        return raw & 0xFFFFu;\n"
                   "    }\n"
                   "    if (fmt == 8u || fmt == 20u || fmt == 50u) {\n"
                   "        uint s = asuint(((int)((raw & 0xFFFFu) << 16u)) >> 16);\n"
                   "        return as_d16 ? (s & 0xFFFFu) : s;\n"
                   "    }\n"
                   "    if (fmt == 9u || fmt == 21u || fmt == 51u) {\n"
                   "        uint h = raw & 0xFFFFu;\n"
                   "        return as_d16 ? h : asuint(f16tof32(h));\n"
                   "    }\n"
                   "    if (fmt == 14u || fmt == 15u) {\n"
                   "        return as_d16 ? (raw & 0xFFFFu) : raw;\n"
                   "    }\n"
                   "    return as_d16 ? (f32tof16(asfloat(raw)) & 0xFFFFu) : raw;\n"
                   "}\n";
        }
        if (uses_mad) {
            hdr << "// GFX10.3 unfused denormal-flushing MAD (distinct from single-rounding fused FMA)\n";
            hdr << "float _os_flush_denorm(float x) {\n"
                   "    uint b = asuint(x);\n"
                   "    return ((b & 0x7F800000u) == 0u) ? asfloat(b & 0x80000000u) : x;\n"
                   "}\n";
            hdr << "float _os_mad_f32(float a, float b, float c) {\n"
                   "    precise float fa = _os_flush_denorm(a);\n"
                   "    precise float fb = _os_flush_denorm(b);\n"
                   "    precise float fc = _os_flush_denorm(c);\n"
                   "    precise float prod = _os_flush_denorm(fa * fb);\n"
                   "    return _os_flush_denorm(prod + fc);\n"
                   "}\n";
        }
        if (uses_fma) {
            hdr << "// GFX10.3 single-rounding fused FMA (preserves subnormals & exact single-rounding even on WARP)\n";
            hdr << "float _os_fma_f32(float a, float b, float c) {\n"
                   "    uint ba = asuint(a);\n"
                   "    uint bb = asuint(b);\n"
                   "    bool a_den = ((ba & 0x7F800000u) == 0u) && ((ba & 0x007FFFFFu) != 0u);\n"
                   "    bool b_den = ((bb & 0x7F800000u) == 0u) && ((bb & 0x007FFFFFu) != 0u);\n"
                   "    if (a_den && !b_den && abs(b) >= 1.0) {\n"
                   "        float a_s = asfloat((ba & 0x807FFFFFu) | 0x3F800000u) - asfloat((ba & 0x80000000u) | 0x3F800000u);\n"
                   "        precise float r = mad(a_s, b * asfloat(0x00800000u), c);\n"
                   "        return r;\n"
                   "    }\n"
                   "    if (b_den && !a_den && abs(a) >= 1.0) {\n"
                   "        float b_s = asfloat((bb & 0x807FFFFFu) | 0x3F800000u) - asfloat((bb & 0x80000000u) | 0x3F800000u);\n"
                   "        precise float r = mad(a * asfloat(0x00800000u), b_s, c);\n"
                   "        return r;\n"
                   "    }\n"
                   "    uint ea = (ba >> 23u) & 0xFFu;\n"
                   "    uint eb = (bb >> 23u) & 0xFFu;\n"
                   "    if (ea >= 30u && ea <= 220u && eb >= 30u && eb <= 220u) {\n"
                   "        precise float ca = 4097.0 * a;\n"
                   "        precise float ahi = ca - (ca - a);\n"
                   "        precise float alo = a - ahi;\n"
                   "        precise float cb = 4097.0 * b;\n"
                   "        precise float bhi = cb - (cb - b);\n"
                   "        precise float blo = b - bhi;\n"
                   "        precise float phi = a * b;\n"
                   "        precise float plo = (((ahi * bhi - phi) + ahi * blo) + alo * bhi) + alo * blo;\n"
                   "        precise float s = phi + c;\n"
                   "        precise float v = s - phi;\n"
                   "        precise float e = (phi - (s - v)) + (c - v);\n"
                   "        precise float r = s + (e + plo);\n"
                   "        return r;\n"
                   "    }\n"
                   "    precise float r = mad(a, b, c);\n"
                   "    return r;\n"
                   "}\n";
        }
        if (uses_ps_quad_deriv) {
            hdr << "// Pixel shader helper-lane-aware 2x2 quad affine derivative tracking\n";
            if (use_sm65_) {
                hdr << "float4 _os_sample_affine_quad(float2 uv) {\n"
                       "    float2 _q_uv_x = QuadReadAcrossX(uv);\n"
                       "    float2 _q_uv_y = QuadReadAcrossY(uv);\n"
                       "    float2 _quad_dx = abs(_q_uv_x - uv);\n"
                       "    float2 _quad_dy = abs(_q_uv_y - uv);\n"
                       "    float2 _ddx = ddx_fine(uv);\n"
                       "    float2 _ddy = ddy_fine(uv);\n"
                       "    float2 _grad_x = (dot(_ddx, _ddx) > 0.0) ? _ddx : _quad_dx;\n"
                       "    float2 _grad_y = (dot(_ddy, _ddy) > 0.0) ? _ddy : _quad_dy;\n"
                       "    return _tex0.SampleGrad(_samp0, uv, _grad_x, _grad_y);\n"
                       "}\n";
            } else {
                hdr << "float4 _os_sample_affine_quad(float2 uv) {\n"
                       "    return _tex0.SampleGrad(_samp0, uv, ddx(uv), ddy(uv));\n"
                       "}\n";
            }
        }
        if (ir_.uses_bvh_ray) {
            hdr << "uint4 _os_bvh_intersect_ray(uint node_ptr, float tmax, float3 orig, float3 dir) {\n"
                   "    uint hdr = _in0.Load(node_ptr);\n"
                   "    uint hit_id = hdr & 0x7FFFFFFFu;\n"
                   "    if ((hdr & 0x80000000u) == 0u) {\n"
                   "        float3 v0 = float3(asfloat(_in0.Load(node_ptr + 4u)), asfloat(_in0.Load(node_ptr + 8u)), asfloat(_in0.Load(node_ptr + 12u)));\n"
                   "        float3 v1 = float3(asfloat(_in0.Load(node_ptr + 16u)), asfloat(_in0.Load(node_ptr + 20u)), asfloat(_in0.Load(node_ptr + 24u)));\n"
                   "        float3 v2 = float3(asfloat(_in0.Load(node_ptr + 28u)), asfloat(_in0.Load(node_ptr + 32u)), asfloat(_in0.Load(node_ptr + 36u)));\n"
                   "        float3 e1 = v1 - v0;\n"
                   "        float3 e2 = v2 - v0;\n"
                   "        float3 pvec = cross(dir, e2);\n"
                   "        float det = dot(e1, pvec);\n"
                   "        if (abs(det) > 1e-7) {\n"
                   "            float inv_det = 1.0 / det;\n"
                   "            float3 tvec = orig - v0;\n"
                   "            float u = dot(tvec, pvec) * inv_det;\n"
                   "            float3 qvec = cross(tvec, e1);\n"
                   "            float v = dot(dir, qvec) * inv_det;\n"
                   "            float t = dot(e2, qvec) * inv_det;\n"
                   "            if (u >= 0.0 && v >= 0.0 && (u + v) <= 1.0 && t >= 0.0 && t <= tmax) {\n"
                   "                return uint4(asuint(t), asuint(u), asuint(v), hit_id);\n"
                   "            }\n"
                   "        }\n"
                   "        return uint4(asuint(tmax), 0u, 0u, 0xFFFFFFFFu);\n"
                   "    } else {\n"
                   "        float3 bmin = float3(asfloat(_in0.Load(node_ptr + 4u)), asfloat(_in0.Load(node_ptr + 8u)), asfloat(_in0.Load(node_ptr + 12u)));\n"
                   "        float3 bmax = float3(asfloat(_in0.Load(node_ptr + 16u)), asfloat(_in0.Load(node_ptr + 20u)), asfloat(_in0.Load(node_ptr + 24u)));\n"
                   "        float t_enter = 0.0;\n"
                   "        float t_exit = tmax;\n"
                   "        if (abs(dir.x) > 1e-12) {\n"
                   "            float inv_d = 1.0 / dir.x;\n"
                   "            float t0 = (bmin.x - orig.x) * inv_d;\n"
                   "            float t1 = (bmax.x - orig.x) * inv_d;\n"
                   "            t_enter = max(t_enter, min(t0, t1));\n"
                   "            t_exit = min(t_exit, max(t0, t1));\n"
                   "        } else if (orig.x < bmin.x || orig.x > bmax.x) {\n"
                   "            t_enter = 1e30; t_exit = -1e30;\n"
                   "        }\n"
                   "        if (abs(dir.y) > 1e-12) {\n"
                   "            float inv_d = 1.0 / dir.y;\n"
                   "            float t0 = (bmin.y - orig.y) * inv_d;\n"
                   "            float t1 = (bmax.y - orig.y) * inv_d;\n"
                   "            t_enter = max(t_enter, min(t0, t1));\n"
                   "            t_exit = min(t_exit, max(t0, t1));\n"
                   "        } else if (orig.y < bmin.y || orig.y > bmax.y) {\n"
                   "            t_enter = 1e30; t_exit = -1e30;\n"
                   "        }\n"
                   "        if (abs(dir.z) > 1e-12) {\n"
                   "            float inv_d = 1.0 / dir.z;\n"
                   "            float t0 = (bmin.z - orig.z) * inv_d;\n"
                   "            float t1 = (bmax.z - orig.z) * inv_d;\n"
                   "            t_enter = max(t_enter, min(t0, t1));\n"
                   "            t_exit = min(t_exit, max(t0, t1));\n"
                   "        } else if (orig.z < bmin.z || orig.z > bmax.z) {\n"
                   "            t_enter = 1e30; t_exit = -1e30;\n"
                   "        }\n"
                   "        if (t_enter <= t_exit) {\n"
                   "            return uint4(asuint(t_enter), asuint(t_exit), 0u, hit_id);\n"
                   "        }\n"
                   "        return uint4(asuint(tmax), 0u, 0u, 0xFFFFFFFFu);\n"
                   "    }\n"
                   "}\n";
        }

        const uint32_t param_out_count = ModuleParamOutputCount(ir_);
        const uint32_t param_in_count = uses_stage_in ? std::max(1u, ModuleParamInputCount(ir_)) : 0u;
        cull_dist_count_ =
            (ir_.stage == ShaderStage::Vertex || ir_.stage == ShaderStage::Geometry || ir_.stage == ShaderStage::Mesh)
                ? ModuleCullDistanceCount(ir_)
                : 0u;

        switch (ir_.stage) {
            case ShaderStage::Compute:
                out_target = use_sm65_ ? "cs_6_5" : "cs_5_0";
                hdr << "[numthreads(" << ir_.threadgroup_x << ", " << ir_.threadgroup_y << ", "
                    << ir_.threadgroup_z << ")]\n";
                hdr << "void main(uint3 _tid : SV_DispatchThreadID, uint _gtid : SV_GroupIndex) {\n";
                if (ir_.uses_wave64_emulation) {
                    const uint32_t lane_mask = (ir_.wave_size == 64u) ? 63u : 31u;
                    const uint32_t wave_shift = (ir_.wave_size == 64u) ? 6u : 5u;
                    hdr << "    uint _gw_id = _gtid >> " << wave_shift << "u;\n";
                    hdr << "    uint _w64_lane = _gtid & " << lane_mask << "u;\n";
                    hdr << "    uint _gw_base = _gw_id << " << wave_shift << "u;\n";
                }
                break;
            case ShaderStage::Vertex:
                out_target = use_sm65_ ? "vs_6_5" : "vs_5_0";
                hdr << "struct VSOut {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                for (uint32_t p = 0u; p < param_out_count; ++p) {
                    hdr << "    " << HlslInterpModifier(SafeInterpMode(ir_.output_interp_modes, p))
                        << "float4 param" << p << " : TEXCOORD" << p << ";\n";
                }
                if (cull_dist_count_ > 0u) {
                    hdr << "    float4 cull_dist0 : SV_CullDistance0;\n";
                }
                if (cull_dist_count_ > 4u) {
                    hdr << "    float4 cull_dist1 : SV_CullDistance1;\n";
                }
                hdr << "};\n";
                hdr << "VSOut main(uint _vid : SV_VertexID) {\n";
                hdr << "    VSOut _out;\n";
                hdr << "    _out.pos = float4(0.0, 0.0, 0.0, 1.0);\n";
                for (uint32_t p = 0u; p < param_out_count; ++p) {
                    hdr << "    _out.param" << p << " = float4(0.0, 0.0, 0.0, 1.0);\n";
                }
                if (cull_dist_count_ > 0u) {
                    hdr << "    _out.cull_dist0 = float4(0.0, 0.0, 0.0, 0.0);\n";
                }
                if (cull_dist_count_ > 4u) {
                    hdr << "    _out.cull_dist1 = float4(0.0, 0.0, 0.0, 0.0);\n";
                }
                break;
            case ShaderStage::Pixel:
                out_target = use_sm65_ ? "ps_6_5" : "ps_5_0";
                hdr << "struct PSIn {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                for (uint32_t p = 0u; p < param_in_count; ++p) {
                    hdr << "    " << HlslInterpModifier(SafeInterpMode(ir_.input_interp_modes, p))
                        << "float4 param" << p << " : TEXCOORD" << p << ";\n";
                }
                hdr << "};\n";
                hdr << "float4 main(PSIn _in) : SV_TARGET0 {\n";
                hdr << "    float4 _mrt0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                break;
            case ShaderStage::Geometry:
                out_target = use_sm65_ ? "gs_6_5" : "gs_5_0";
                hdr << "struct GSIn {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                hdr << "};\n";
                hdr << "struct GSOut {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                for (uint32_t p = 0u; p < param_out_count; ++p) {
                    hdr << "    " << HlslInterpModifier(SafeInterpMode(ir_.output_interp_modes, p))
                        << "float4 param" << p << " : TEXCOORD" << p << ";\n";
                }
                if (cull_dist_count_ > 0u) {
                    hdr << "    float4 cull_dist0 : SV_CullDistance0;\n";
                }
                if (cull_dist_count_ > 4u) {
                    hdr << "    float4 cull_dist1 : SV_CullDistance1;\n";
                }
                hdr << "};\n";
                hdr << "[maxvertexcount(6)]\n";
                hdr << "void main(triangle GSIn _gs_in[3], inout TriangleStream<GSOut> _gs_stream) {\n";
                hdr << "    GSOut _gs_out;\n";
                hdr << "    _gs_out.pos = float4(0.0, 0.0, 0.0, 1.0);\n";
                for (uint32_t p = 0u; p < param_out_count; ++p) {
                    hdr << "    _gs_out.param" << p << " = float4(0.0, 0.0, 0.0, 1.0);\n";
                }
                if (cull_dist_count_ > 0u) {
                    hdr << "    _gs_out.cull_dist0 = float4(0.0, 0.0, 0.0, 0.0);\n";
                }
                if (cull_dist_count_ > 4u) {
                    hdr << "    _gs_out.cull_dist1 = float4(0.0, 0.0, 0.0, 0.0);\n";
                }
                if (bounded_wave64_dispatcher_) {
                    hdr << "    uint _w64_ballot[2] = {0u, 0u};\n";
                }
                break;
            case ShaderStage::Mesh: {
                if (!use_sm65_) {
                    return Status::Error(StatusCode::UnsupportedFormat, "Mesh shaders require SM 6.5 (ms_6_5)");
                }
                out_target = "ms_6_5";
                const uint32_t max_v = ir_.mesh_max_vertices ? ir_.mesh_max_vertices : 64u;
                const uint32_t max_p = ir_.mesh_max_primitives ? ir_.mesh_max_primitives : 64u;
                if (ir_.mesh_has_payload) {
                    hdr << "struct ASPayload {\n";
                    hdr << "    uint _data[4];\n";
                    hdr << "};\n";
                }
                hdr << "struct MeshVert {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                for (uint32_t p = 0u; p < param_out_count; ++p) {
                    hdr << "    " << HlslInterpModifier(SafeInterpMode(ir_.output_interp_modes, p))
                        << "float4 param" << p << " : TEXCOORD" << p << ";\n";
                }
                if (cull_dist_count_ > 0u) {
                    hdr << "    float4 cull_dist0 : SV_CullDistance0;\n";
                }
                if (cull_dist_count_ > 4u) {
                    hdr << "    float4 cull_dist1 : SV_CullDistance1;\n";
                }
                hdr << "};\n";
                if (ir_.uses_mesh_cull) {
                    hdr << "struct MeshPrimAttr {\n";
                    hdr << "    bool cull : SV_CullPrimitive;\n";
                    hdr << "};\n";
                }
                hdr << "[outputtopology(\"triangle\")]\n";
                hdr << "[numthreads(" << hw_tg_x << ", " << hw_tg_y << ", " << hw_tg_z << ")]\n";
                hdr << "void main(\n";
                if (ir_.uses_mesh_global_tid) {
                    hdr << "    uint3 _gid : SV_GroupID,\n";
                }
                hdr << "    uint _gtid : SV_GroupIndex,\n";
                if (ir_.mesh_has_payload) {
                    hdr << "    in payload ASPayload _payload,\n";
                }
                hdr << "    out vertices MeshVert _verts[" << max_v << "],\n";
                if (ir_.uses_mesh_cull) {
                    hdr << "    out indices uint3 _prims[" << max_p << "],\n";
                    hdr << "    out primitives MeshPrimAttr _prim_attrs[" << max_p << "])\n{\n";
                } else {
                    hdr << "    out indices uint3 _prims[" << max_p << "])\n{\n";
                }
                hdr << "    uint _mesh_vcnt = " << max_v << "u;\n";
                hdr << "    uint _mesh_pcnt = " << max_p << "u;\n";
                if (ir_.uses_wave64_emulation && !bounded_wave64_dispatcher_) {
                    const uint32_t lane_mask = (ir_.wave_size == 64u) ? 63u : 31u;
                    const uint32_t wave_shift = (ir_.wave_size == 64u) ? 6u : 5u;
                    hdr << "    uint _gw_id = _gtid >> " << wave_shift << "u;\n";
                    hdr << "    uint _w64_lane = _gtid & " << lane_mask << "u;\n";
                    hdr << "    uint _gw_base = _gw_id << " << wave_shift << "u;\n";
                }
                break;
            }
            case ShaderStage::Amplification:
                if (!use_sm65_) {
                    return Status::Error(StatusCode::UnsupportedFormat, "Amplification shaders require SM 6.5 (as_6_5)");
                }
                out_target = "as_6_5";
                hdr << "struct ASPayload {\n";
                hdr << "    uint _data[4];\n";
                hdr << "};\n";
                hdr << "groupshared ASPayload _payload;\n";
                hdr << "[numthreads(" << hw_tg_x << ", " << hw_tg_y << ", " << hw_tg_z << ")]\n";
                hdr << "void main(uint _gtid : SV_GroupIndex) {\n";
                hdr << "    _payload._data[0] = 0u;\n";
                hdr << "    _payload._data[1] = 0u;\n";
                hdr << "    _payload._data[2] = 0u;\n";
                hdr << "    _payload._data[3] = 0u;\n";
                hdr << "    uint _dm_x = 1u;\n";
                hdr << "    uint _dm_y = 1u;\n";
                hdr << "    uint _dm_z = 1u;\n";
                if (ir_.uses_wave64_emulation && !bounded_wave64_dispatcher_) {
                    const uint32_t lane_mask = (ir_.wave_size == 64u) ? 63u : 31u;
                    const uint32_t wave_shift = (ir_.wave_size == 64u) ? 6u : 5u;
                    hdr << "    uint _gw_id = _gtid >> " << wave_shift << "u;\n";
                    hdr << "    uint _w64_lane = _gtid & " << lane_mask << "u;\n";
                    hdr << "    uint _gw_base = _gw_id << " << wave_shift << "u;\n";
                }
                break;
            default:
                return Status::Error(StatusCode::UnsupportedFormat, "Unsupported shader stage for HLSL emission");
        }

        // Cooperative zero-initialization of LDS (groupshared) at shader entry before any LDS read
        if (ir_.uses_lds && is_group_stage) {
            const uint32_t lds_n = ir_.lds_size_dwords ? ir_.lds_size_dwords : 1024u;
            hdr << "    // Zero-initialize groupshared LDS cooperatively at entry\n";
            hdr << "    for (uint _lds_init = _gtid; _lds_init < " << lds_n
                << "u; _lds_init += " << hw_tg_total << "u) {\n";
            hdr << "        _lds[_lds_init] = 0u;\n";
            hdr << "    }\n";
            hdr << "    GroupMemoryBarrierWithGroupSync();\n";
        }

        // Pre-declare thread-private scratch array if used
        if (ir_.uses_scratch) {
            const uint32_t scr_n = ir_.scratch_size_dwords ? ir_.scratch_size_dwords : 64u;
            hdr << "    uint _scratch[" << scr_n << "];\n";
            hdr << "    for (uint _si = 0u; _si < " << scr_n << "u; ++_si) { _scratch[_si] = 0u; }\n";
        }

        // Pre-declare all SSA value locals so multi-block structured/fallback CFG always compiles cleanly.
        for (const auto& bb : ir_.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.id == kInvalidIrValue) continue;
                if (inst.type == IrType::F32) {
                    hdr << "    float _v" << inst.id << " = 0.0;\n";
                } else if (inst.type == IrType::Bool) {
                    hdr << "    bool _v" << inst.id << " = false;\n";
                } else {
                    hdr << "    uint _v" << inst.id << " = 0u;\n";
                }
            }
        }

        // Bounded wave64-on-wave32 two-half execution wrapper for Mesh / Amplification / Geometry shaders
        if (bounded_wave64_dispatcher_) {
            if (is_group_stage) {
                hdr << "    if (_gtid == 0u) { _w64_ballot[0] = 0u; _w64_ballot[1] = 0u; }\n";
                hdr << "    GroupMemoryBarrierWithGroupSync();\n";
            }
            if (ir_.stage == ShaderStage::Mesh) {
                // Hoist uniform SetMeshOutputCounts before the two-half loop so DXC validates single call
                for (const auto& inst : ir_.blocks[0].instructions) {
                    if (inst.op == IrOp::SetMeshOutputCounts) {
                        auto resolve_uniform_u32 = [&](IrValueId id, uint32_t fallback) -> std::string {
                            auto it = defs_.find(id);
                            if (it != defs_.end()) {
                                if (it->second->op == IrOp::ConstU32) return HexU32(it->second->imm_u32);
                                if (it->second->op == IrOp::LoadRootConst) return "_rc" + std::to_string(it->second->imm_u32);
                            }
                            return std::to_string(fallback) + "u";
                        };
                        hdr << "    _mesh_vcnt = " << resolve_uniform_u32(inst.operands[0], 3u) << ";\n";
                        hdr << "    _mesh_pcnt = " << resolve_uniform_u32(inst.operands[1], 1u) << ";\n";
                        hdr << "    SetMeshOutputCounts(_mesh_vcnt, _mesh_pcnt);\n";
                        hoisted_mesh_counts_ = true;
                        break;
                    }
                }
            }
            hdr << "    // Bounded wave64-on-wave32 two-half execution dispatcher (half = 0..1)\n";
            hdr << "    [unroll]\n";
            hdr << "    for (uint _w64_half = 0u; _w64_half < 2u; ++_w64_half) {\n";
            if (is_group_stage) {
                hdr << "        uint _hw_lane = (WaveGetLaneCount() >= 32u) ? (WaveGetLaneIndex() & 31u) : (_gtid & 31u);\n";
                hdr << "        uint _w64_lane = (_w64_half * 32u) + _hw_lane;\n";
                hdr << "        uint _w64_gtid = ((_gtid & ~31u) * 2u) + _w64_lane;\n";
                hdr << "        InterlockedOr(_w64_ballot[_w64_half], 1u << (_w64_lane & 31u));\n";
            } else {
                hdr << "        uint _w64_lane = (_w64_half * 32u) + (WaveGetLaneIndex() & 31u);\n";
                hdr << "        uint _w64_gtid = _w64_lane;\n";
                hdr << "        _w64_ballot[_w64_half] = WaveActiveBallot(true).x;\n";
            }
        }

        const bool wrap_as_single_dispatch =
            (!bounded_wave64_dispatcher_ && ir_.stage == ShaderStage::Amplification);
        const int base_depth = (bounded_wave64_dispatcher_ || wrap_as_single_dispatch) ? 2 : 1;
        Rdna2CfgAnalysis cfg_info{};
        AnalyzeRdna2Cfg(ir_, cfg_info);
        std::ostringstream body;
        std::unordered_set<uint32_t> visited;
        if (!cfg_info.has_cycles && !cfg_info.is_irreducible &&
            EmitStructured(body, 0u, UINT32_MAX, base_depth, visited)) {
            if (wrap_as_single_dispatch) {
                hdr << "    do {\n";
                hdr << body.str();
                hdr << "    } while (false);\n";
            } else {
                hdr << body.str();
            }
        } else {
            EmitStateMachine(hdr, bounded_wave64_dispatcher_ ? 2 : 1);
            if (!bounded_wave64_dispatcher_) {
                if (ir_.stage == ShaderStage::Vertex) {
                    hdr << "    return _out;\n";
                } else if (ir_.stage == ShaderStage::Pixel) {
                    hdr << "    return _mrt0;\n";
                }
            }
        }
        if (bounded_wave64_dispatcher_) {
            hdr << "    }\n";
        }
        if (ir_.stage == ShaderStage::Amplification) {
            if (ir_.uses_lds) {
                hdr << "    GroupMemoryBarrierWithGroupSync();\n";
                hdr << "    if (_gtid == 0u) {\n";
                hdr << "        _payload._data[0] = _lds[0];\n";
                hdr << "        _payload._data[1] = _lds[1];\n";
                hdr << "        _payload._data[2] = _lds[2];\n";
                hdr << "        _payload._data[3] = _lds[3];\n";
                hdr << "    }\n";
                hdr << "    GroupMemoryBarrierWithGroupSync();\n";
            }
            hdr << "    DispatchMesh(_dm_x, _dm_y, _dm_z, _payload);\n";
        }
        hdr << "}\n";

        out_hlsl = hdr.str();
        return Status::Ok();
    }

private:
    std::string Indent(int depth) const {
        return std::string(static_cast<size_t>(depth) * 4u, ' ');
    }

    IrType TypeOf(IrValueId id) const {
        auto it = defs_.find(id);
        return it != defs_.end() ? it->second->type : IrType::U32;
    }

    std::string AsU32(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::F32) return "asuint(_v" + std::to_string(id) + ")";
        if (t == IrType::Bool) return "(_v" + std::to_string(id) + " ? 1u : 0u)";
        return "_v" + std::to_string(id);
    }

    std::string AsI32(IrValueId id) const {
        return "asint(" + AsU32(id) + ")";
    }

    std::string AsF32(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::F32) return "_v" + std::to_string(id);
        return "asfloat(" + AsU32(id) + ")";
    }

    std::string AsBool(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::Bool) return "_v" + std::to_string(id);
        return "(" + AsU32(id) + " != 0u)";
    }

    std::string EmitReturnStmt() const {
        if (bounded_wave64_dispatcher_) return "continue;";
        if (ir_.stage == ShaderStage::Vertex) return "return _out;";
        if (ir_.stage == ShaderStage::Pixel) return "return _mrt0;";
        if (ir_.stage == ShaderStage::Amplification) return "break;";
        return "return;";
    }

    void EmitPhiCopies(std::ostringstream& os, uint32_t from_bb, uint32_t to_bb, int depth) const {
        if (to_bb >= ir_.blocks.size()) return;
        const auto& dst_bb = ir_.blocks[to_bb];
        struct PhiCopy {
            IrValueId dst_id;
            IrType dst_type;
            IrValueId src_id;
        };
        std::vector<PhiCopy> copies;
        for (const auto& inst : dst_bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) {
                    copies.push_back({inst.id, inst.type, inc.second});
                    break;
                }
            }
        }
        if (copies.empty()) return;
        if (copies.size() == 1u) {
            const auto& c = copies[0];
            os << Indent(depth) << "_v" << c.dst_id << " = ";
            if (c.dst_type == IrType::F32) os << AsF32(c.src_id);
            else if (c.dst_type == IrType::Bool) os << AsBool(c.src_id);
            else os << AsU32(c.src_id);
            os << ";\n";
            return;
        }
        // Parallel-safe copy for multiple loop-carried Phis
        os << Indent(depth) << "{\n";
        for (size_t k = 0u; k < copies.size(); ++k) {
            const auto& c = copies[k];
            const char* ty = (c.dst_type == IrType::F32) ? "float" : (c.dst_type == IrType::Bool ? "bool" : "uint");
            os << Indent(depth + 1) << ty << " _pt" << k << " = ";
            if (c.dst_type == IrType::F32) os << AsF32(c.src_id);
            else if (c.dst_type == IrType::Bool) os << AsBool(c.src_id);
            else os << AsU32(c.src_id);
            os << ";\n";
        }
        for (size_t k = 0u; k < copies.size(); ++k) {
            os << Indent(depth + 1) << "_v" << copies[k].dst_id << " = _pt" << k << ";\n";
        }
        os << Indent(depth) << "}\n";
    }

    bool HasPhiCopies(uint32_t from_bb, uint32_t to_bb) const {
        if (to_bb >= ir_.blocks.size()) return false;
        for (const auto& inst : ir_.blocks[to_bb].instructions) {
            if (inst.op != IrOp::Phi) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) return true;
            }
        }
        return false;
    }

    void EmitNonTerminators(std::ostringstream& os, const IrBasicBlock& bb, int depth) const {
        const std::string ind = Indent(depth);
        const uint32_t lds_mask = (ir_.lds_size_dwords ? ir_.lds_size_dwords : 1024u) - 1u;
        static constexpr char kSwiz[4] = {'x', 'y', 'z', 'w'};

        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Phi || inst.op == IrOp::Branch ||
                inst.op == IrOp::CondBranch || inst.op == IrOp::Return) {
                continue;
            }

            switch (inst.op) {
                case IrOp::ConstU32:
                    if (inst.type == IrType::Bool) {
                        os << ind << "_v" << inst.id << " = " << (inst.imm_u32 != 0u ? "true" : "false") << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = " << HexU32(inst.imm_u32) << ";\n";
                    }
                    break;
                case IrOp::ConstF32:
                    os << ind << "_v" << inst.id << " = asfloat(" << HexU32(FloatToBits(inst.imm_f32)) << ");\n";
                    break;
                case IrOp::ThreadIdX:
                    if (ir_.stage == ShaderStage::Vertex) {
                        os << ind << "_v" << inst.id << " = _vid;\n";
                    } else if (bounded_wave64_dispatcher_) {
                        os << ind << "_v" << inst.id << " = _w64_gtid;\n";
                    } else if (ir_.stage == ShaderStage::Geometry) {
                        os << ind << "_v" << inst.id << " = 0u;\n";
                    } else if (ir_.stage == ShaderStage::Mesh || ir_.stage == ShaderStage::Amplification) {
                        os << ind << "_v" << inst.id << " = _gtid;\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _tid.x;\n";
                    }
                    break;
                case IrOp::MeshGlobalThreadId: {
                    const char* lidx = bounded_wave64_dispatcher_ ? "_w64_gtid" : "_gtid";
                    const uint32_t nt = inst.imm_u32 ? inst.imm_u32 : ir_.mesh_num_threads;
                    os << ind << "_v" << inst.id << " = (_gid.x * " << nt << "u + " << lidx << ");\n";
                    break;
                }
                case IrOp::MergedWaveInfo: {
                    const uint32_t tg_total = std::max(1u, ir_.threadgroup_x * ir_.threadgroup_y * ir_.threadgroup_z);
                    const uint32_t ws = (ir_.wave_size == 64u) ? 64u : 32u;
                    const uint32_t nwaves = std::min(15u, std::max(1u, (tg_total + ws - 1u) / ws));
                    if (ir_.stage == ShaderStage::Mesh) {
                        const char* lidx = bounded_wave64_dispatcher_ ? "_w64_gtid" : "_gtid";
                        const uint32_t wshift = (ws == 64u) ? 6u : 5u;
                        os << ind << "{\n";
                        os << ind << "    uint _mwi_w = (" << lidx << " >> " << wshift << "u);\n";
                        os << ind << "    uint _mwi_es = min(255u, min(" << ws << "u, " << tg_total
                           << "u - (_mwi_w << " << wshift << "u)));\n";
                        os << ind << "    _v" << inst.id << " = _mwi_es | (" << std::min(255u, tg_total)
                           << "u << 8u) | ((_mwi_w & 0xFFu) << 16u) | ((_mwi_w & 0xFu) << 24u) | ("
                           << nwaves << "u << 28u);\n";
                        os << ind << "}\n";
                    } else {
                        Rdna2MergedWaveInfo mwi{};
                        mwi.es_thread_count = static_cast<uint8_t>(std::min(255u, ws));
                        mwi.gs_thread_count = static_cast<uint8_t>(std::min(255u, tg_total));
                        mwi.waves_in_group = static_cast<uint8_t>(nwaves);
                        os << ind << "_v" << inst.id << " = " << HexU32(PackMergedWaveInfo(mwi)) << ";\n";
                    }
                    break;
                }
                case IrOp::StageInput: {
                    const uint32_t pslot = (inst.imm_u32 >> 2) & 3u;
                    os << ind << "_v" << inst.id << " = _in.param" << pslot << "." << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                }
                case IrOp::LoadRootConst:
                    os << ind << "_v" << inst.id << " = _rc" << inst.imm_u32 << ";\n";
                    break;
                case IrOp::LoadUav: {
                    if ((inst.imm_u32 & kIrLoadCbHeapFlagLifter) != 0u) {
                        const uint32_t cb_reg = inst.imm_u32 & 0xFFu;
                        os << ind << "{ uint _cb_dw = ((" << AsU32(inst.operands[0]) << ") >> 2u); "
                           << "uint4 _cb_q = _cb_heap_" << cb_reg << "[(_cb_dw >> 2u) & 4095u]; "
                           << "uint _cb_l = _cb_dw & 3u; "
                           << "_v" << inst.id << " = (_cb_l == 0u) ? _cb_q.x : ((_cb_l == 1u) ? _cb_q.y : ((_cb_l == 2u) ? _cb_q.z : _cb_q.w)); }\n";
                    } else {
                        const char* buf = (inst.imm_u32 == 1u && ir_.uses_uav1) ? "_out0" : "_in0";
                        os << ind << "_v" << inst.id << " = " << buf << ".Load(" << AsU32(inst.operands[0]) << ");\n";
                    }
                    break;
                }
                case IrOp::StoreUav: {
                    const char* buf = (inst.imm_u32 == 1u && ir_.uses_uav1) ? "_out0" : "_in0";
                    if (inst.exec_pred != kInvalidIrValue) {
                        os << ind << "if (" << AsBool(inst.exec_pred) << ") { "
                           << buf << ".Store(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << "); }\n";
                    } else {
                        os << ind << buf << ".Store(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                }
                case IrOp::BufferLoadFormat: {
                    const uint32_t uav_slot = inst.imm_u32 & 0xFu;
                    const uint32_t fmt = (inst.imm_u32 >> 4) & 0x7Fu;
                    const uint32_t comp_idx = (inst.imm_u32 >> 12) & 0xFu;
                    const bool is_d16 = ((inst.imm_u32 >> 16) & 1u) != 0u;
                    const bool has_hi = ((inst.imm_u32 >> 17) & 1u) != 0u;
                    const char* loader = (uav_slot == 1u && ir_.uses_uav1) ? "_os_load_unaligned_u1" : "_os_load_unaligned_u0";
                    uint32_t elem_bytes = 4u;
                    if (fmt == 1u || fmt == 2u || fmt == 3u || fmt == 4u ||
                        fmt == 10u || fmt == 11u || fmt == 12u || fmt == 13u ||
                        fmt == 28u || fmt == 29u || fmt == 30u || fmt == 31u) {
                        elem_bytes = 1u;
                    } else if (fmt == 5u || fmt == 6u || fmt == 7u || fmt == 8u || fmt == 9u ||
                               fmt == 17u || fmt == 18u || fmt == 19u || fmt == 20u || fmt == 21u ||
                               fmt == 47u || fmt == 48u || fmt == 49u || fmt == 50u || fmt == 51u) {
                        elem_bytes = 2u;
                    }
                    const std::string base_addr = AsU32(inst.operands[0]);
                    if (is_d16) {
                        os << ind << "{\n";
                        os << ind << "    uint _bf_lo = _os_decode_fmt_comp(" << loader << "(" << base_addr
                           << " + " << (comp_idx * elem_bytes) << "u), " << fmt << "u, true) & 0xFFFFu;\n";
                        if (has_hi) {
                            os << ind << "    uint _bf_hi = _os_decode_fmt_comp(" << loader << "(" << base_addr
                               << " + " << ((comp_idx + 1u) * elem_bytes) << "u), " << fmt << "u, true) & 0xFFFFu;\n";
                        } else {
                            os << ind << "    uint _bf_hi = 0u;\n";
                        }
                        os << ind << "    _v" << inst.id << " = _bf_lo | (_bf_hi << 16u);\n";
                        os << ind << "}\n";
                    } else {
                        const std::string dec = "_os_decode_fmt_comp(" + std::string(loader) + "(" + base_addr +
                                                " + " + std::to_string(comp_idx * elem_bytes) + "u), " +
                                                std::to_string(fmt) + "u, false)";
                        if (inst.type == IrType::F32) {
                            os << ind << "_v" << inst.id << " = asfloat(" << dec << ");\n";
                        } else {
                            os << ind << "_v" << inst.id << " = " << dec << ";\n";
                        }
                    }
                    break;
                }
                case IrOp::ImageSample:
                    os << ind << "_v" << inst.id << " = _tex0.SampleLevel(_samp0, float2("
                       << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << "), "
                       << AsF32(inst.operands[2]) << ")." << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                case IrOp::ImageSampleImplicit:
                    os << ind << "_v" << inst.id << " = _os_sample_affine_quad(float2("
                       << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << "))."
                       << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                case IrOp::ImageLoad:
                    os << ind << "_v" << inst.id << " = _tex0.Load(int3("
                       << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", 0))."
                       << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                case IrOp::ImageLoadMip: {
                    const uint32_t ch = inst.imm_u32 & 3u;
                    const uint32_t dim = (inst.imm_u32 >> 4) & 7u;
                    if (dim == 2u) {
                        os << ind << "_v" << inst.id << " = _tex3d0.Load(int4("
                           << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", "
                           << AsI32(inst.operands[2]) << ", " << AsI32(inst.operands[3]) << "))."
                           << kSwiz[ch] << ";\n";
                    } else if (dim == 3u) {
                        os << ind << "_v" << inst.id << " = _texcube0.Load(int4("
                           << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", "
                           << AsI32(inst.operands[2]) << ", " << AsI32(inst.operands[3]) << "))."
                           << kSwiz[ch] << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _tex0.Load(int3("
                           << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", "
                           << AsI32(inst.operands[3]) << "))." << kSwiz[ch] << ";\n";
                    }
                    break;
                }
                case IrOp::ImageStore:
                    os << ind << "_rwtex0[uint2(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1])
                       << ")] = float4(" << AsF32(inst.operands[2]) << ", " << AsF32(inst.operands[3])
                       << ", " << AsF32(inst.operands[4]) << ", " << AsF32(inst.operands[5]) << ");\n";
                    break;
                case IrOp::ImageAtomicCmpSwap: {
                    const uint32_t dim = inst.imm_u32 & 7u;
                    if (dim == 2u || dim == 3u) {
                        os << ind << "InterlockedCompareExchange(_rwimg_u32[uint3("
                           << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ", "
                           << AsU32(inst.operands[2]) << ")], " << AsU32(inst.operands[3]) << ", "
                           << AsU32(inst.operands[4]) << ", _v" << inst.id << ");\n";
                    } else {
                        os << ind << "InterlockedCompareExchange(_rwimg_u32[uint2("
                           << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ")], "
                           << AsU32(inst.operands[3]) << ", " << AsU32(inst.operands[4]) << ", _v"
                           << inst.id << ");\n";
                    }
                    break;
                }
                case IrOp::BvhIntersectRay: {
                    const uint32_t comp = inst.imm_u32 & 3u;
                    const std::string call = "_os_bvh_intersect_ray(" + AsU32(inst.operands[0]) + ", " +
                                             AsF32(inst.operands[1]) + ", float3(" +
                                             AsF32(inst.operands[2]) + ", " + AsF32(inst.operands[3]) + ", " +
                                             AsF32(inst.operands[4]) + "), float3(" +
                                             AsF32(inst.operands[5]) + ", " + AsF32(inst.operands[6]) + ", " +
                                             AsF32(inst.operands[7]) + "))." + std::string(1, kSwiz[comp]);
                    if (comp < 3u) {
                        os << ind << "_v" << inst.id << " = asfloat(" << call << ");\n";
                    } else {
                        os << ind << "_v" << inst.id << " = " << call << ";\n";
                    }
                    break;
                }
                case IrOp::LdsLoad:
                    os << ind << "_v" << inst.id << " = _lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                       << lds_mask << "u];\n";
                    break;
                case IrOp::LdsStore:
                    if (inst.exec_pred != kInvalidIrValue) {
                        os << ind << "if (" << AsBool(inst.exec_pred) << ") { _lds[(("
                           << AsU32(inst.operands[0]) << ") >> 2u) & " << lds_mask
                           << "u] = " << AsU32(inst.operands[1]) << "; }\n";
                    } else {
                        os << ind << "_lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & " << lds_mask
                           << "u] = " << AsU32(inst.operands[1]) << ";\n";
                    }
                    break;
                case IrOp::LdsAtomicAdd:
                    if (inst.exec_pred != kInvalidIrValue) {
                        os << ind << "_v" << inst.id << " = 0u; if (" << AsBool(inst.exec_pred)
                           << ") { InterlockedAdd(_lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                           << lds_mask << "u], " << AsU32(inst.operands[1]) << ", _v" << inst.id << "); }\n";
                    } else {
                        os << ind << "InterlockedAdd(_lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                           << lds_mask << "u], " << AsU32(inst.operands[1]) << ", _v" << inst.id << ");\n";
                    }
                    break;
                case IrOp::ScratchLoad: {
                    const uint32_t scr_mask = (ir_.scratch_size_dwords ? ir_.scratch_size_dwords : 64u) - 1u;
                    os << ind << "_v" << inst.id << " = _scratch[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                       << scr_mask << "u];\n";
                    break;
                }
                case IrOp::ScratchStore: {
                    const uint32_t scr_mask = (ir_.scratch_size_dwords ? ir_.scratch_size_dwords : 64u) - 1u;
                    if (inst.exec_pred != kInvalidIrValue) {
                        os << ind << "if (" << AsBool(inst.exec_pred) << ") { _scratch[((" << AsU32(inst.operands[0])
                           << ") >> 2u) & " << scr_mask << "u] = " << AsU32(inst.operands[1]) << "; }\n";
                    } else {
                        os << ind << "_scratch[((" << AsU32(inst.operands[0]) << ") >> 2u) & " << scr_mask
                           << "u] = " << AsU32(inst.operands[1]) << ";\n";
                    }
                    break;
                }
                case IrOp::WaveLaneId:
                    os << ind << "_v" << inst.id << " = _w64_lane;\n";
                    break;
                case IrOp::WaveBallotLo:
                case IrOp::WaveBallotHi: {
                    const uint32_t word_idx = (inst.op == IrOp::WaveBallotHi) ? 1u : 0u;
                    if (ir_.wave_size == 32u && word_idx == 1u) {
                        // Guest Wave32 has no upper 32 lanes (32..63); never bleed into other half of a 64-lane host wave
                        os << ind << "_v" << inst.id << " = 0u;\n";
                    } else if (bounded_wave64_dispatcher_ && ir_.stage == ShaderStage::Geometry) {
                        os << ind << "_w64_ballot[_w64_half] = WaveActiveBallot(" << AsBool(inst.operands[0]) << ").x;\n";
                        os << ind << "_v" << inst.id << " = _w64_ballot[" << word_idx << "];\n";
                    } else if (bounded_wave64_dispatcher_) {
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                        os << ind << "if (_gtid == 0u && _w64_half == 0u) { _w64_ballot[0] = 0u; _w64_ballot[1] = 0u; }\n";
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                        os << ind << "InterlockedOr(_w64_ballot[_w64_half], (" << AsBool(inst.operands[0])
                           << " ? (1u << (_w64_lane & 31u)) : 0u));\n";
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                        os << ind << "_v" << inst.id << " = _w64_ballot[" << word_idx << "];\n";
                    } else {
                        // Per-guest-wave isolated ballot (prevents Wave32 lanes from colliding across halves on 64-lane AMD host waves)
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                        os << ind << "if (_w64_lane == 0u) { _w64_ballot[_gw_id * 2u] = 0u; _w64_ballot[_gw_id * 2u + 1u] = 0u; }\n";
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                        os << ind << "InterlockedOr(_w64_ballot[_gw_id * 2u + (_w64_lane >> 5u)], (" << AsBool(inst.operands[0])
                           << " ? (1u << (_w64_lane & 31u)) : 0u));\n";
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                        os << ind << "_v" << inst.id << " = _w64_ballot[_gw_id * 2u + " << word_idx << "u];\n";
                    }
                    break;
                }
                case IrOp::WaveActiveCountBits64:
                    os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0])
                       << ") + countbits(" << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::WavePrefixCount64:
                    if (inst.imm_u32 == 0u) {
                        os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0])
                           << " & ((_w64_lane < 32u) ? ((1u << (_w64_lane & 31u)) - 1u) : 0xFFFFFFFFu)) + "
                           << AsU32(inst.operands[1]) << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0])
                           << " & ((_w64_lane >= 32u) ? ((1u << ((_w64_lane - 32u) & 31u)) - 1u) : 0u)) + "
                           << AsU32(inst.operands[1]) << ";\n";
                    }
                    break;
                case IrOp::WaveReadFirstLane:
                case IrOp::WaveReadLaneAt: {
                    const uint32_t lane_mask = (ir_.wave_size == 64u) ? 63u : 31u;
                    const uint32_t w_mask = ComputeWaveLanesCapacity(ir_) - 1u;
                    const std::string base_off = bounded_wave64_dispatcher_ ? "0u" : "_gw_base";
                    const std::string lane_expr = (inst.op == IrOp::WaveReadFirstLane)
                                                      ? "0u"
                                                      : ("(" + AsU32(inst.operands[1]) + " & " + std::to_string(lane_mask) + "u)");
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "_w64_lanes[(" << base_off << " + _w64_lane) & " << w_mask << "u] = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    if (inst.type == IrType::F32) {
                        os << ind << "_v" << inst.id << " = asfloat(_w64_lanes[(" << base_off << " + " << lane_expr << ") & " << w_mask << "u]);\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _w64_lanes[(" << base_off << " + " << lane_expr << ") & " << w_mask << "u];\n";
                    }
                    break;
                }
                case IrOp::WavePermLane16: {
                    const uint32_t lane_mask = (ir_.wave_size == 64u) ? 63u : 31u;
                    const uint32_t w_mask = ComputeWaveLanesCapacity(ir_) - 1u;
                    const uint32_t x16_xor = (inst.imm_u32 != 0u) ? 16u : 0u;
                    const std::string base_off = bounded_wave64_dispatcher_ ? "0u" : "_gw_base";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "_w64_lanes[(" << base_off << " + _w64_lane) & " << w_mask << "u] = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "{\n";
                    os << ind << "    uint _pl_lane = _w64_lane;\n";
                    os << ind << "    uint _pl_row = _pl_lane & 15u;\n";
                    os << ind << "    uint _pl_base = (_pl_lane & ~15u) ^ " << x16_xor << "u;\n";
                    os << ind << "    uint _pl_word = (_pl_row < 8u) ? " << AsU32(inst.operands[1]) << " : " << AsU32(inst.operands[2]) << ";\n";
                    os << ind << "    uint _pl_sel = (_pl_word >> ((_pl_row & 7u) << 2u)) & 15u;\n";
                    if (inst.type == IrType::F32) {
                        os << ind << "    _v" << inst.id << " = asfloat(_w64_lanes[(" << base_off << " + ((_pl_base | _pl_sel) & " << lane_mask << "u)) & " << w_mask << "u]);\n";
                    } else {
                        os << ind << "    _v" << inst.id << " = _w64_lanes[(" << base_off << " + ((_pl_base | _pl_sel) & " << lane_mask << "u)) & " << w_mask << "u];\n";
                    }
                    os << ind << "}\n";
                    break;
                }
                case IrOp::SetMeshOutputCounts:
                    if (ir_.stage == ShaderStage::Amplification) {
                        os << ind << "_dm_x = " << AsU32(inst.operands[0]) << ";\n";
                        os << ind << "_dm_y = " << AsU32(inst.operands[1]) << ";\n";
                        os << ind << "_dm_z = 1u;\n";
                    } else if (!hoisted_mesh_counts_) {
                        os << ind << "_mesh_vcnt = " << AsU32(inst.operands[0]) << ";\n";
                        os << ind << "_mesh_pcnt = " << AsU32(inst.operands[1]) << ";\n";
                        os << ind << "SetMeshOutputCounts(_mesh_vcnt, _mesh_pcnt);\n";
                    }
                    break;
                case IrOp::ExportMeshPrim: {
                    const char* idx_var = bounded_wave64_dispatcher_ ? "_w64_gtid" : "_gtid";
                    if (inst.imm_u32 == 1u) {
                        const std::string p = AsU32(inst.operands[0]);
                        os << ind << "if (" << idx_var << " < _mesh_pcnt) { _prims[" << idx_var
                           << "] = uint3((" << p << ") & 0x3FFu, ((" << p
                           << ") >> 10u) & 0x3FFu, ((" << p << ") >> 20u) & 0x3FFu); }\n";
                    } else {
                        os << ind << "if (" << idx_var << " < _mesh_pcnt) { _prims[" << idx_var
                           << "] = uint3(" << AsU32(inst.operands[0]) << ", "
                           << AsU32(inst.operands[1]) << ", " << AsU32(inst.operands[2]) << "); }\n";
                    }
                    break;
                }
                case IrOp::ExportMeshCull: {
                    const char* idx_var = bounded_wave64_dispatcher_ ? "_w64_gtid" : "_gtid";
                    os << ind << "if (" << idx_var << " < _mesh_pcnt) { _prim_attrs[" << idx_var
                       << "].cull = " << AsBool(inst.operands[0]) << "; }\n";
                    break;
                }
                case IrOp::DispatchMeshGroups:
                    os << ind << "_dm_x = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "_dm_y = " << AsU32(inst.operands[1]) << ";\n";
                    os << ind << "_dm_z = " << AsU32(inst.operands[2]) << ";\n";
                    break;
                case IrOp::FAdd:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " + " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FSub:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " - " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FMul:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " * " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FFma:
                    os << ind << "_v" << inst.id << " = _os_fma_f32(" << AsF32(inst.operands[0]) << ", "
                       << AsF32(inst.operands[1]) << ", " << AsF32(inst.operands[2]) << ");\n";
                    break;
                case IrOp::FMad:
                    os << ind << "_v" << inst.id << " = _os_mad_f32(" << AsF32(inst.operands[0]) << ", "
                       << AsF32(inst.operands[1]) << ", " << AsF32(inst.operands[2]) << ");\n";
                    break;
                case IrOp::FFma16: {
                    const uint32_t f = inst.imm_u32;
                    auto f16_expr = [&](IrValueId v, int idx) -> std::string {
                        const bool hi = ((f >> idx) & 1u) != 0u;
                        const bool do_neg = ((f >> (8 + idx)) & 1u) != 0u;
                        const bool do_abs = ((f >> (12 + idx)) & 1u) != 0u;
                        std::string ex = hi ? ("f16tof32((" + AsU32(v) + " >> 16u) & 0xFFFFu)")
                                            : ("f16tof32(" + AsU32(v) + " & 0xFFFFu)");
                        if (do_abs) ex = "abs(" + ex + ")";
                        if (do_neg) ex = "(-" + ex + ")";
                        return ex;
                    };
                    os << ind << "{\n";
                    os << ind << "    float _r16 = _os_fma_f32(" << f16_expr(inst.operands[0], 0) << ", "
                       << f16_expr(inst.operands[1], 1) << ", " << f16_expr(inst.operands[2], 2) << ");\n";
                    if (((f >> 16) & 1u) != 0u) {
                        os << ind << "    _r16 = saturate(_r16);\n";
                    }
                    os << ind << "    _v" << inst.id << " = (f32tof16(_r16) & 0xFFFFu);\n";
                    os << ind << "}\n";
                    break;
                }
                case IrOp::FMin:
                    os << ind << "_v" << inst.id << " = _os_fmin_f32(" << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FMax:
                    os << ind << "_v" << inst.id << " = _os_fmax_f32(" << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FRcp:
                    os << ind << "_v" << inst.id << " = rcp(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FRsq:
                    os << ind << "_v" << inst.id << " = rsqrt(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FSqrt:
                    os << ind << "_v" << inst.id << " = sqrt(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FFloor:
                    os << ind << "_v" << inst.id << " = _os_floor_f32(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FCeil:
                    os << ind << "_v" << inst.id << " = _os_ceil_f32(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FFract:
                    os << ind << "_v" << inst.id << " = _os_quiet_nan(" << AsF32(inst.operands[0]) << " - _os_floor_f32(" << AsF32(inst.operands[0]) << "));\n";
                    break;
                case IrOp::FDot2: {
                    const std::string a = AsU32(inst.operands[0]);
                    const std::string b = AsU32(inst.operands[1]);
                    const std::string c = AsF32(inst.operands[2]);
                    os << ind << "_v" << inst.id << " = (f16tof32(" << a << " & 0xFFFFu) * f16tof32(" << b
                       << " & 0xFFFFu) + f16tof32((" << a << " >> 16u) & 0xFFFFu) * f16tof32((" << b
                       << " >> 16u) & 0xFFFFu) + " << c << ");\n";
                    break;
                }
                case IrOp::IAdd:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " + " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::ISub:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " - " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::IMul:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " * " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::IMin:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(min(" << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << "));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = min(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                case IrOp::IMax:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(max(" << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << "));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = max(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                case IrOp::BitAnd:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " & " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitOr:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " | " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitXor:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " ^ " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitNot:
                    os << ind << "_v" << inst.id << " = (~" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::BitCount32:
                    os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::Shl:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " << (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    break;
                case IrOp::Shr:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(" << AsI32(inst.operands[0]) << " >> (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " >> (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    }
                    break;
                case IrOp::AlignByte:
                    os << ind << "_v" << inst.id << " = _os_alignbyte_b32(" << AsU32(inst.operands[0]) << ", "
                       << AsU32(inst.operands[1]) << ", " << AsU32(inst.operands[2]) << ");\n";
                    break;
                case IrOp::BfeU32:
                    os << ind << "{\n";
                    os << ind << "    uint _bfe_s = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "    uint _bfe_o = " << AsU32(inst.operands[1]) << " & 31u;\n";
                    os << ind << "    uint _bfe_w = " << AsU32(inst.operands[2]) << " & "
                       << ((inst.imm_u32 != 0u) ? "0x7Fu" : "31u") << ";\n";
                    os << ind << "    _v" << inst.id << " = (_bfe_w == 0u) ? 0u : (((_bfe_o + _bfe_w) < 32u) ? ((_bfe_s >> _bfe_o) & ((1u << (_bfe_w & 31u)) - 1u)) : (_bfe_s >> _bfe_o));\n";
                    os << ind << "}\n";
                    break;
                case IrOp::BfeI32:
                    os << ind << "{\n";
                    os << ind << "    uint _bfe_s = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "    uint _bfe_o = " << AsU32(inst.operands[1]) << " & 31u;\n";
                    os << ind << "    uint _bfe_w = " << AsU32(inst.operands[2]) << " & "
                       << ((inst.imm_u32 != 0u) ? "0x7Fu" : "31u") << ";\n";
                    os << ind << "    bool _bfe_lt = (_bfe_o + _bfe_w) < 32u;\n";
                    os << ind << "    uint _bfe_shl = (_bfe_lt && (_bfe_w > 0u)) ? (32u - _bfe_o - _bfe_w) : 0u;\n";
                    os << ind << "    uint _bfe_shr = (_bfe_lt && (_bfe_w > 0u)) ? (32u - _bfe_w) : 0u;\n";
                    os << ind << "    int _bfe_r = (_bfe_w == 0u) ? 0 : (_bfe_lt ? (asint(_bfe_s << _bfe_shl) >> _bfe_shr) : (asint(_bfe_s) >> _bfe_o));\n";
                    os << ind << "    _v" << inst.id << " = asuint(_bfe_r);\n";
                    os << ind << "}\n";
                    break;
                case IrOp::BfiB32:
                    os << ind << "_v" << inst.id << " = ((" << AsU32(inst.operands[0]) << " & "
                       << AsU32(inst.operands[1]) << ") | ((~" << AsU32(inst.operands[0]) << ") & "
                       << AsU32(inst.operands[2]) << "));\n";
                    break;
                case IrOp::PkAddF16:
                case IrOp::PkMulF16:
                case IrOp::PkFmaF16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const uint8_t neg_lo = static_cast<uint8_t>((f >> 8) & 7u);
                    const uint8_t neg_hi = static_cast<uint8_t>((f >> 12) & 7u);
                    const bool clamp = ((f >> 16) & 1u) != 0u;
                    auto lane_expr = [&](IrValueId v, int idx, bool hi_out) -> std::string {
                        const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> idx) & 1u) != 0u;
                        const bool do_neg = (((hi_out ? neg_hi : neg_lo) >> idx) & 1u) != 0u;
                        std::string ex = pick_hi
                                             ? ("f16tof32((" + AsU32(v) + " >> 16u) & 0xFFFFu)")
                                             : ("f16tof32(" + AsU32(v) + " & 0xFFFFu)");
                        return do_neg ? ("(-" + ex + ")") : ex;
                    };
                    os << ind << "{\n";
                    os << ind << "    float _a0 = " << lane_expr(inst.operands[0], 0, false) << ";\n";
                    os << ind << "    float _a1 = " << lane_expr(inst.operands[0], 0, true) << ";\n";
                    os << ind << "    float _b0 = " << lane_expr(inst.operands[1], 1, false) << ";\n";
                    os << ind << "    float _b1 = " << lane_expr(inst.operands[1], 1, true) << ";\n";
                    if (inst.op == IrOp::PkFmaF16) {
                        os << ind << "    float _c0 = " << lane_expr(inst.operands[2], 2, false) << ";\n";
                        os << ind << "    float _c1 = " << lane_expr(inst.operands[2], 2, true) << ";\n";
                        os << ind << "    float _r0 = mad(_a0, _b0, _c0);\n";
                        os << ind << "    float _r1 = mad(_a1, _b1, _c1);\n";
                    } else if (inst.op == IrOp::PkAddF16) {
                        os << ind << "    float _r0 = _a0 + _b0;\n";
                        os << ind << "    float _r1 = _a1 + _b1;\n";
                    } else {
                        os << ind << "    float _r0 = _a0 * _b0;\n";
                        os << ind << "    float _r1 = _a1 * _b1;\n";
                    }
                    if (clamp) {
                        os << ind << "    _r0 = saturate(_r0); _r1 = saturate(_r1);\n";
                    }
                    os << ind << "    _v" << inst.id << " = (f32tof16(_r0) & 0xFFFFu) | ((f32tof16(_r1) & 0xFFFFu) << 16u);\n";
                    os << ind << "}\n";
                    break;
                }
                case IrOp::PkAddU16:
                case IrOp::PkShl16:
                case IrOp::PkShr16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const bool is_shift = (inst.op == IrOp::PkShl16 || inst.op == IrOp::PkShr16);
                    auto u16_expr = [&](IrValueId v, int isa_idx, bool hi_out) -> std::string {
                        const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> isa_idx) & 1u) != 0u;
                        return pick_hi ? ("((" + AsU32(v) + " >> 16u) & 0xFFFFu)")
                                       : ("(" + AsU32(v) + " & 0xFFFFu)");
                    };
                    os << ind << "{\n";
                    os << ind << "    uint _a0 = " << u16_expr(inst.operands[0], is_shift ? 1 : 0, false) << ";\n";
                    os << ind << "    uint _a1 = " << u16_expr(inst.operands[0], is_shift ? 1 : 0, true) << ";\n";
                    os << ind << "    uint _b0 = " << u16_expr(inst.operands[1], is_shift ? 0 : 1, false) << ";\n";
                    os << ind << "    uint _b1 = " << u16_expr(inst.operands[1], is_shift ? 0 : 1, true) << ";\n";
                    if (inst.op == IrOp::PkAddU16) {
                        os << ind << "    uint _r0 = (_a0 + _b0) & 0xFFFFu;\n";
                        os << ind << "    uint _r1 = (_a1 + _b1) & 0xFFFFu;\n";
                    } else if (inst.op == IrOp::PkShl16) {
                        os << ind << "    uint _r0 = (_a0 << (_b0 & 15u)) & 0xFFFFu;\n";
                        os << ind << "    uint _r1 = (_a1 << (_b1 & 15u)) & 0xFFFFu;\n";
                    } else {
                        os << ind << "    uint _r0 = (_a0 >> (_b0 & 15u)) & 0xFFFFu;\n";
                        os << ind << "    uint _r1 = (_a1 >> (_b1 & 15u)) & 0xFFFFu;\n";
                    }
                    os << ind << "    _v" << inst.id << " = _r0 | (_r1 << 16u);\n";
                    os << ind << "}\n";
                    break;
                }
                case IrOp::I2F:
                    os << ind << "_v" << inst.id << " = (float)(" << AsI32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::U2F:
                    os << ind << "_v" << inst.id << " = (float)(" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::F2I:
                    os << ind << "_v" << inst.id << " = asuint((int)(" << AsF32(inst.operands[0]) << "));\n";
                    break;
                case IrOp::F2U:
                    os << ind << "_v" << inst.id << " = (uint)(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::CvtPkNormI16F32:
                    os << ind << "_v" << inst.id << " = _os_cvt_snorm16_f32(" << AsF32(inst.operands[0])
                       << ") | (_os_cvt_snorm16_f32(" << AsF32(inst.operands[1]) << ") << 16u);\n";
                    break;
                case IrOp::CvtPkNormU16F32:
                    os << ind << "_v" << inst.id << " = _os_cvt_unorm16_f32(" << AsF32(inst.operands[0])
                       << ") | (_os_cvt_unorm16_f32(" << AsF32(inst.operands[1]) << ") << 16u);\n";
                    break;
                case IrOp::CvtNormI16F16:
                case IrOp::CvtNormU16F16: {
                    const bool hi = (inst.imm_u32 & 1u) != 0u;
                    const bool do_neg = ((inst.imm_u32 >> 8) & 1u) != 0u;
                    const bool do_abs = ((inst.imm_u32 >> 12) & 1u) != 0u;
                    std::string f_ex = hi ? ("f16tof32((" + AsU32(inst.operands[0]) + " >> 16u) & 0xFFFFu)")
                                          : ("f16tof32(" + AsU32(inst.operands[0]) + " & 0xFFFFu)");
                    if (do_abs) f_ex = "abs(" + f_ex + ")";
                    if (do_neg) f_ex = "(-" + f_ex + ")";
                    const char* fn = (inst.op == IrOp::CvtNormI16F16) ? "_os_cvt_snorm16_f32" : "_os_cvt_unorm16_f32";
                    os << ind << "_v" << inst.id << " = " << fn << "(" << f_ex << ");\n";
                    break;
                }
                case IrOp::CmpEq:
                case IrOp::CmpNe:
                case IrOp::CmpLt:
                case IrOp::CmpGt:
                case IrOp::CmpGe: {
                    const bool is_flt = (TypeOf(inst.operands[0]) == IrType::F32 || TypeOf(inst.operands[1]) == IrType::F32);
                    const std::string a = is_flt ? AsF32(inst.operands[0]) : AsI32(inst.operands[0]);
                    const std::string b = is_flt ? AsF32(inst.operands[1]) : AsI32(inst.operands[1]);
                    const char* sym = "==";
                    if (inst.op == IrOp::CmpNe) sym = "!=";
                    else if (inst.op == IrOp::CmpLt) sym = "<";
                    else if (inst.op == IrOp::CmpGt) sym = ">";
                    else if (inst.op == IrOp::CmpGe) sym = ">=";
                    os << ind << "_v" << inst.id << " = (" << a << " " << sym << " " << b << ");\n";
                    break;
                }
                case IrOp::CmpLtU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " < " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpEqU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " == " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpGtU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " > " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpGeU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " >= " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpF16: {
                    const uint32_t imm = inst.imm_u32;
                    const uint32_t pred = imm & 15u;
                    const uint32_t op_sel = (imm >> 8) & 3u;
                    const uint32_t neg_m = (imm >> 12) & 3u;
                    const uint32_t abs_m = (imm >> 16) & 3u;
                    auto f16_op = [&](IrValueId v, uint32_t bit) -> std::string {
                        std::string ex = ((op_sel & bit) != 0u)
                                             ? ("f16tof32((" + AsU32(v) + " >> 16u) & 0xFFFFu)")
                                             : ("f16tof32(" + AsU32(v) + " & 0xFFFFu)");
                        if ((abs_m & bit) != 0u) ex = "abs(" + ex + ")";
                        if ((neg_m & bit) != 0u) ex = "(-" + ex + ")";
                        return ex;
                    };
                    os << ind << "{\n";
                    os << ind << "    float _fa16 = " << f16_op(inst.operands[0], 1u) << ";\n";
                    os << ind << "    float _fb16 = " << f16_op(inst.operands[1], 2u) << ";\n";
                    os << ind << "    bool _unord16 = isnan(_fa16) || isnan(_fb16);\n";
                    os << ind << "    _v" << inst.id << " = ";
                    switch (pred) {
                        case 0u:  os << "false"; break;
                        case 1u:  os << "(!_unord16 && (_fa16 < _fb16))"; break;
                        case 2u:  os << "(!_unord16 && (_fa16 == _fb16))"; break;
                        case 3u:  os << "(!_unord16 && (_fa16 <= _fb16))"; break;
                        case 4u:  os << "(!_unord16 && (_fa16 > _fb16))"; break;
                        case 5u:  os << "(!_unord16 && (_fa16 != _fb16))"; break;
                        case 6u:  os << "(!_unord16 && (_fa16 >= _fb16))"; break;
                        case 7u:  os << "(!_unord16)"; break;
                        case 8u:  os << "_unord16"; break;
                        case 9u:  os << "(_unord16 || (_fa16 < _fb16))"; break;
                        case 10u: os << "(_unord16 || (_fa16 == _fb16))"; break;
                        case 11u: os << "(_unord16 || (_fa16 <= _fb16))"; break;
                        case 12u: os << "(_unord16 || (_fa16 > _fb16))"; break;
                        case 13u: os << "(_unord16 || (_fa16 != _fb16))"; break;
                        case 14u: os << "(_unord16 || (_fa16 >= _fb16))"; break;
                        case 15u: os << "true"; break;
                        default:  os << "false"; break;
                    }
                    os << ";\n";
                    os << ind << "}\n";
                    break;
                }
                case IrOp::LogicalAnd:
                    os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " && " << AsBool(inst.operands[1]) << ");\n";
                    break;
                case IrOp::LogicalNot:
                    os << ind << "_v" << inst.id << " = (!" << AsBool(inst.operands[0]) << ");\n";
                    break;
                case IrOp::Select:
                    if (inst.type == IrType::F32) {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsF32(inst.operands[1]) << " : " << AsF32(inst.operands[2]) << ");\n";
                    } else if (inst.type == IrType::Bool) {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsBool(inst.operands[1]) << " : " << AsBool(inst.operands[2]) << ");\n";
                    } else {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsU32(inst.operands[1]) << " : " << AsU32(inst.operands[2]) << ");\n";
                    }
                    break;
                case IrOp::Barrier:
                    if (ir_.stage == ShaderStage::Compute || ir_.stage == ShaderStage::Mesh ||
                        ir_.stage == ShaderStage::Amplification) {
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    }
                    break;
                case IrOp::Export: {
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Null) &&
                        inst.imm_u32 == kIrKillExportMagicLifter &&
                        ir_.stage == ShaderStage::Pixel) {
                        if (inst.operands.empty()) {
                            os << ind << "discard;\n";
                        } else {
                            os << ind << "if (" << AsBool(inst.operands[0]) << ") { discard; }\n";
                        }
                        break;
                    }
                    const std::string vec = "float4(" + AsF32(inst.operands[0]) + ", " + AsF32(inst.operands[1]) +
                                            ", " + AsF32(inst.operands[2]) + ", " + AsF32(inst.operands[3]) + ")";
                    const char* mesh_idx = bounded_wave64_dispatcher_ ? "_w64_gtid" : "_gtid";
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                        if (ir_.stage == ShaderStage::Vertex) {
                            os << ind << "_out.pos = " << vec << ";\n";
                        } else if (ir_.stage == ShaderStage::Mesh) {
                            os << ind << "if (" << mesh_idx << " < _mesh_vcnt) { _verts["
                               << mesh_idx << "].pos = " << vec << "; }\n";
                        } else if (ir_.stage == ShaderStage::Geometry) {
                            os << ind << "_gs_out.pos = " << vec << ";\n";
                            os << ind << "_gs_stream.Append(_gs_out);\n";
                        }
                    } else if (inst.export_target >= static_cast<uint8_t>(Rdna2ExportTarget::Param0) &&
                               inst.export_target <= static_cast<uint8_t>(Rdna2ExportTarget::Param3)) {
                        const uint32_t pslot = static_cast<uint32_t>(
                            inst.export_target - static_cast<uint8_t>(Rdna2ExportTarget::Param0));
                        if (ir_.stage == ShaderStage::Vertex) {
                            os << ind << "_out.param" << pslot << " = " << vec << ";\n";
                        } else if (ir_.stage == ShaderStage::Mesh) {
                            os << ind << "if (" << mesh_idx << " < _mesh_vcnt) { _verts["
                               << mesh_idx << "].param" << pslot << " = " << vec << "; }\n";
                        } else if (ir_.stage == ShaderStage::Geometry) {
                            os << ind << "_gs_out.param" << pslot << " = " << vec << ";\n";
                        }
                    } else if ((inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos2) ||
                                inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos3)) &&
                               cull_dist_count_ > 0u) {
                        const char* field =
                            (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos3))
                                ? "cull_dist1"
                                : "cull_dist0";
                        if (ir_.stage == ShaderStage::Vertex) {
                            os << ind << "_out." << field << " = " << vec << ";\n";
                        } else if (ir_.stage == ShaderStage::Mesh) {
                            os << ind << "if (" << mesh_idx << " < _mesh_vcnt) { _verts[" << mesh_idx
                               << "]." << field << " = " << vec << "; }\n";
                        } else if (ir_.stage == ShaderStage::Geometry) {
                            os << ind << "_gs_out." << field << " = " << vec << ";\n";
                        }
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) &&
                               ir_.stage == ShaderStage::Pixel) {
                        os << ind << "_mrt0 = " << vec << ";\n";
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    uint32_t SingleBranchTarget(uint32_t bb_id) const {
        if (bb_id >= ir_.blocks.size()) return UINT32_MAX;
        const auto& ins = ir_.blocks[bb_id].instructions;
        if (ins.empty()) return UINT32_MAX;
        if (ins.back().op == IrOp::Branch) return ins.back().true_block;
        return UINT32_MAX;
    }

    bool IsReturnBlock(uint32_t bb_id) const {
        if (bb_id >= ir_.blocks.size()) return false;
        const auto& ins = ir_.blocks[bb_id].instructions;
        return !ins.empty() && ins.back().op == IrOp::Return;
    }

    bool EmitStructured(
        std::ostringstream& os,
        uint32_t bb_id,
        uint32_t stop_bb,
        int depth,
        std::unordered_set<uint32_t>& active) const {
        while (bb_id != stop_bb) {
            if (bb_id >= ir_.blocks.size()) return false;
            if (!active.insert(bb_id).second) return false;

            const auto& bb = ir_.blocks[bb_id];
            EmitNonTerminators(os, bb, depth);
            if (bb.instructions.empty()) {
                active.erase(bb_id);
                return true;
            }

            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Return) {
                os << Indent(depth) << EmitReturnStmt() << "\n";
                active.erase(bb_id);
                return true;
            }
            if (term.op == IrOp::Branch) {
                const uint32_t next = term.true_block;
                EmitPhiCopies(os, bb_id, next, depth);
                active.erase(bb_id);
                bb_id = next;
                continue;
            }
            if (term.op == IrOp::CondBranch) {
                const uint32_t t_bb = term.true_block;
                const uint32_t f_bb = term.false_block;
                const std::string cond = AsBool(term.operands[0]);

                const uint32_t t_next = SingleBranchTarget(t_bb);
                const uint32_t f_next = SingleBranchTarget(f_bb);

                if (t_bb != f_bb && t_next == f_bb && t_bb != stop_bb) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, f_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}";
                    if (HasPhiCopies(bb_id, f_bb)) {
                        os << " else {\n";
                        EmitPhiCopies(os, bb_id, f_bb, depth + 1);
                        os << Indent(depth) << "}";
                    }
                    os << "\n";
                    active.erase(bb_id);
                    bb_id = f_bb;
                    continue;
                }

                if (t_bb != f_bb && f_next == t_bb && f_bb != stop_bb) {
                    os << Indent(depth) << "if (!(" << cond << ")) {\n";
                    if (!EmitStructured(os, f_bb, t_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}";
                    if (HasPhiCopies(bb_id, t_bb)) {
                        os << " else {\n";
                        EmitPhiCopies(os, bb_id, t_bb, depth + 1);
                        os << Indent(depth) << "}";
                    }
                    os << "\n";
                    active.erase(bb_id);
                    bb_id = t_bb;
                    continue;
                }

                if (t_bb != f_bb && t_next != UINT32_MAX && t_next == f_next) {
                    const uint32_t join_bb = t_next;
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, join_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "} else {\n";
                    if (!EmitStructured(os, f_bb, join_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = join_bb;
                    continue;
                }

                if (IsReturnBlock(t_bb) && !IsReturnBlock(f_bb)) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = f_bb;
                    continue;
                }
                if (IsReturnBlock(f_bb) && !IsReturnBlock(t_bb)) {
                    os << Indent(depth) << "if (!(" << cond << ")) {\n";
                    if (!EmitStructured(os, f_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = t_bb;
                    continue;
                }
                if (IsReturnBlock(t_bb) && IsReturnBlock(f_bb)) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "} else {\n";
                    if (!EmitStructured(os, f_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    return true;
                }

                return false;
            }

            active.erase(bb_id);
            return true;
        }
        return true;
    }

    void EmitStateMachine(std::ostringstream& os, int base_depth = 1) const {
        const std::string i0 = Indent(base_depth);
        const std::string i1 = Indent(base_depth + 1);
        const std::string i2 = Indent(base_depth + 2);
        const std::string i3 = Indent(base_depth + 3);
        const std::string i4 = Indent(base_depth + 4);
        os << i0 << "uint _cfg_state = 0u;\n";
        os << i0 << "[loop] while (true) {\n";
        os << i1 << "if (_cfg_state == 0xFFFFFFFFu) { break; }\n";
        os << i1 << "switch (_cfg_state) {\n";
        for (size_t i = 0; i < ir_.blocks.size(); ++i) {
            const auto& bb = ir_.blocks[i];
            os << i2 << "case " << bb.id << "u: {\n";
            EmitNonTerminators(os, bb, base_depth + 3);
            if (!bb.instructions.empty()) {
                const auto& term = bb.instructions.back();
                if (term.op == IrOp::Return) {
                    os << i3 << "_cfg_state = 0xFFFFFFFFu;\n";
                } else if (term.op == IrOp::Branch) {
                    EmitPhiCopies(os, bb.id, term.true_block, base_depth + 3);
                    os << i3 << "_cfg_state = " << term.true_block << "u;\n";
                } else if (term.op == IrOp::CondBranch) {
                    os << i3 << "if (" << AsBool(term.operands[0]) << ") {\n";
                    EmitPhiCopies(os, bb.id, term.true_block, base_depth + 4);
                    os << i4 << "_cfg_state = " << term.true_block << "u;\n";
                    os << i3 << "} else {\n";
                    EmitPhiCopies(os, bb.id, term.false_block, base_depth + 4);
                    os << i4 << "_cfg_state = " << term.false_block << "u;\n";
                    os << i3 << "}\n";
                } else {
                    os << i3 << "_cfg_state = 0xFFFFFFFFu;\n";
                }
            } else {
                os << i3 << "_cfg_state = 0xFFFFFFFFu;\n";
            }
            os << i3 << "break;\n";
            os << i2 << "}\n";
        }
        os << i2 << "default:\n";
        os << i3 << "_cfg_state = 0xFFFFFFFFu;\n";
        os << i3 << "break;\n";
        os << i1 << "}\n";
        os << i0 << "}\n";
    }

    const Rdna2IrModule& ir_;
    bool use_sm65_ = true;
    bool bounded_wave64_dispatcher_ = false;
    bool hoisted_mesh_counts_ = false;
    uint32_t cull_dist_count_ = 0u;
    std::unordered_map<IrValueId, const IrInstruction*> defs_;
};

}  // namespace

// ---------------------------------------------------------------------------
// SPIR-V 1.3 / 1.4 / 1.6 Binary Emitter (Strict Vulkan 1.1 / 1.2 / 1.3 spirv-val Compliance)
// ---------------------------------------------------------------------------
Status EmitSpirv(const Rdna2IrModule& ir, std::vector<uint32_t>& out_spirv) {
    out_spirv.clear();
    if (ir.blocks.empty()) {
        CallTelemetry::Global().Record("rdna2_lifter", "EmitSpirv", CallOutcome::Error, -1);
        return Status::Error(StatusCode::ShaderError, "Cannot emit SPIR-V from empty IR module");
    }
    if (ir.stage != ShaderStage::Compute && ir.stage != ShaderStage::Vertex &&
        ir.stage != ShaderStage::Pixel && ir.stage != ShaderStage::Mesh &&
        ir.stage != ShaderStage::Amplification) {
        CallTelemetry::Global().Record("rdna2_lifter", "EmitSpirv", CallOutcome::NotImplemented, -1);
        return Status::Error(StatusCode::UnsupportedFormat,
                             "SPIR-V emitter supports Compute/Vertex/Pixel/Mesh/Amplification (use extended stage lifter for HS/DS/GS)");
    }

    const bool is_mesh_stage = (ir.stage == ShaderStage::Mesh);
    const bool is_task_stage = (ir.stage == ShaderStage::Amplification);
    const bool is_mesh_or_task = is_mesh_stage || is_task_stage;
    const bool is_spirv14 = is_mesh_or_task;  // SPIR-V 1.4+ (Mesh=1.4, Task=1.6) requires all globals in OpEntryPoint
    const uint32_t cull_dist_count =
        (ir.stage == ShaderStage::Vertex || is_mesh_stage) ? ModuleCullDistanceCount(ir) : 0u;
    uint32_t next_id = 1;
    auto alloc_id = [&]() -> uint32_t { return next_id++; };

    std::vector<uint32_t> sec_caps;
    std::vector<uint32_t> sec_exts;
    std::vector<uint32_t> sec_ext;
    std::vector<uint32_t> sec_mem;
    std::vector<uint32_t> sec_entry;
    std::vector<uint32_t> sec_exec;
    std::vector<uint32_t> sec_decor;
    std::vector<uint32_t> sec_types;
    std::vector<uint32_t> sec_globals;
    std::vector<uint32_t> sec_fn;

    // 1. Capabilities, Extensions, ExtInstImport, MemoryModel
    EmitSpvInst(sec_caps, SpvOpCapability, {1u});  // Shader
    if (is_mesh_or_task) {
        EmitSpvInst(sec_caps, SpvOpCapability, {kSpvCapMeshShadingEXT});
        std::vector<uint32_t> ext_ops;
        AppendSpvString(ext_ops, "SPV_EXT_mesh_shader");
        EmitSpvInst(sec_exts, SpvOpExtension, ext_ops);
    }
    if (cull_dist_count > 0u) {
        EmitSpvInst(sec_caps, SpvOpCapability, {kSpvCapCullDistance});
    }
    const uint32_t glsl_ext_id = alloc_id();
    {
        std::vector<uint32_t> ops = {glsl_ext_id};
        AppendSpvString(ops, "GLSL.std.450");
        EmitSpvInst(sec_ext, SpvOpExtInstImport, ops);
    }
    EmitSpvInst(sec_mem, SpvOpMemoryModel, {0u, 1u});  // Logical, GLSL450

    // 2. Core scalar / vector / function types
    const uint32_t t_void = alloc_id();
    const uint32_t t_bool = alloc_id();
    const uint32_t t_u32 = alloc_id();
    const uint32_t t_i32 = alloc_id();
    const uint32_t t_f32 = alloc_id();
    const uint32_t t_v2f32 = alloc_id();
    const uint32_t t_v2i32 = alloc_id();
    const uint32_t t_v3i32 = alloc_id();
    const uint32_t t_v3u32 = alloc_id();
    const uint32_t t_v4u32 = alloc_id();
    const uint32_t t_v4f32 = alloc_id();
    const uint32_t t_fn_void = alloc_id();

    EmitSpvInst(sec_types, SpvOpTypeVoid, {t_void});
    EmitSpvInst(sec_types, SpvOpTypeBool, {t_bool});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_u32, 32u, 0u});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_i32, 32u, 1u});
    EmitSpvInst(sec_types, SpvOpTypeFloat, {t_f32, 32u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2f32, t_f32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2i32, t_i32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v3i32, t_i32, 3u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v3u32, t_u32, 3u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v4u32, t_u32, 4u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v4f32, t_f32, 4u});
    EmitSpvInst(sec_types, SpvOpTypeFunction, {t_fn_void, t_void});

    // Deduplicated constant pool (emitted into sec_types)
    std::unordered_map<uint32_t, uint32_t> const_u32_map;
    std::unordered_map<int32_t, uint32_t> const_i32_map;
    std::unordered_map<uint32_t, uint32_t> const_f32_map;
    uint32_t const_true_id = 0;
    uint32_t const_false_id = 0;

    auto get_const_u32 = [&](uint32_t v) -> uint32_t {
        auto it = const_u32_map.find(v);
        if (it != const_u32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_u32, id, v});
        const_u32_map[v] = id;
        return id;
    };

    auto get_const_i32 = [&](int32_t v) -> uint32_t {
        auto it = const_i32_map.find(v);
        if (it != const_i32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_i32, id, static_cast<uint32_t>(v)});
        const_i32_map[v] = id;
        return id;
    };

    auto get_const_f32 = [&](float f) -> uint32_t {
        const uint32_t bits = FloatToBits(f);
        auto it = const_f32_map.find(bits);
        if (it != const_f32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_f32, id, bits});
        const_f32_map[bits] = id;
        return id;
    };

    auto get_const_bool = [&](bool v) -> uint32_t {
        if (v) {
            if (!const_true_id) {
                const_true_id = alloc_id();
                EmitSpvInst(sec_types, SpvOpConstantTrue, {t_bool, const_true_id});
            }
            return const_true_id;
        }
        if (!const_false_id) {
            const_false_id = alloc_id();
            EmitSpvInst(sec_types, SpvOpConstantFalse, {t_bool, const_false_id});
        }
        return const_false_id;
    };

    const uint32_t c_u32_0 = get_const_u32(0u);
    const uint32_t c_u32_1 = get_const_u32(1u);
    const uint32_t c_u32_2 = get_const_u32(2u);
    const uint32_t c_u32_5 = get_const_u32(5u);
    const uint32_t c_u32_7 = get_const_u32(7u);
    const uint32_t c_u32_8 = get_const_u32(8u);
    const uint32_t c_u32_15 = get_const_u32(15u);
    const uint32_t c_u32_16 = get_const_u32(16u);
    const uint32_t c_u32_31 = get_const_u32(31u);
    const uint32_t c_u32_32 = get_const_u32(32u);
    const uint32_t c_u32_63 = get_const_u32(63u);
    const uint32_t c_u32_65535 = get_const_u32(0xFFFFu);
    const uint32_t c_u32_not15 = get_const_u32(~15u);
    const uint32_t c_u32_exp_mask = get_const_u32(0x7F800000u);
    const uint32_t c_u32_mant_mask = get_const_u32(0x007FFFFFu);
    const uint32_t c_u32_sign_mask = get_const_u32(0x80000000u);
    const uint32_t c_u32_sign_mant_mask = get_const_u32(0x807FFFFFu);
    const uint32_t c_u32_f32_one_bits = get_const_u32(0x3F800000u);
    const uint32_t c_u32_allones = get_const_u32(0xFFFFFFFFu);
    const uint32_t c_i32_0 = get_const_i32(0);
    const uint32_t c_f32_0 = get_const_f32(0.0f);
    const uint32_t c_f32_1 = get_const_f32(1.0f);
    const uint32_t c_f32_min_norm = get_const_f32(1.17549435e-38f);  // 0x00800000 = 2^-126

    // 3. Interface variables, descriptors, and workgroup shared memory
    std::vector<uint32_t> interface_vars;
    auto register_spv14_global = [&](uint32_t var_id) {
        if (is_spirv14 && var_id != 0u) {
            interface_vars.push_back(var_id);
        }
    };
    bool emitted_sample_rate_cap = false;
    auto emit_interp_decor = [&](uint32_t var_id, Rdna2InterpMode mode) {
        if (var_id == 0u) return;
        if (mode == Rdna2InterpMode::Flat) {
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_id, kSpvDecFlat});
        } else if (mode == Rdna2InterpMode::NoPerspective) {
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_id, kSpvDecNoPerspective});
        } else if (mode == Rdna2InterpMode::Centroid) {
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_id, kSpvDecCentroid});
        } else if (mode == Rdna2InterpMode::Sample) {
            if (!emitted_sample_rate_cap) {
                EmitSpvInst(sec_caps, SpvOpCapability, {35u});  // SampleRateShading
                emitted_sample_rate_cap = true;
            }
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_id, kSpvDecSample});
        }
    };
    uint32_t var_gid = 0;
    uint32_t var_wg_id = 0;
    uint32_t var_local_idx = 0;
    uint32_t t_ptr_in_u32 = 0;
    uint32_t var_vid = 0;
    uint32_t var_pervertex = 0;
    uint32_t t_ptr_out_f32 = 0;
    uint32_t t_ptr_out_v4f32 = 0;
    uint32_t t_ptr_out_v3u32 = 0;
    uint32_t t_ptr_out_bool = 0;
    uint32_t var_mesh_prims = 0;
    uint32_t var_mesh_cull = 0;
    uint32_t var_task_payload = 0;
    uint32_t t_ptr_tp_u32 = 0;
    uint32_t t_ptr_fn_u32 = 0;
    uint32_t var_dm_x = 0;
    uint32_t var_dm_y = 0;
    uint32_t var_dm_z = 0;
    uint32_t var_param_out[4] = {0u, 0u, 0u, 0u};
    uint32_t var_param_in[4] = {0u, 0u, 0u, 0u};
    uint32_t var_mrt0 = 0;
    const uint32_t param_out_count = ModuleParamOutputCount(ir);
    const uint32_t param_in_count = ModuleUsesStageInput(ir) ? std::max(1u, ModuleParamInputCount(ir)) : 0u;

    // Storage buffers (SPIR-V 1.3 core StorageBuffer + Block)
    // Plus dedicated DescriptorSet 1 read-only CB heap bindings for non-zero CB offsets
    // so they never collide with mixed-heap V# slot 0 (DescriptorSet 0, Binding 0).
    uint32_t var_uav0 = 0;
    uint32_t var_uav1 = 0;
    uint32_t t_ptr_sb_u32 = 0;
    const bool uses_uav0 = ir.uses_uav0 || (ir.uses_uav && !ir.uses_uav1);
    const bool uses_uav1 = ir.uses_uav1;
    std::vector<uint32_t> spv_cb_heap_regs;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::LoadUav && (inst.imm_u32 & kIrLoadCbHeapFlagLifter) != 0u) {
                const uint32_t reg = inst.imm_u32 & 0xFFu;
                if (std::find(spv_cb_heap_regs.begin(), spv_cb_heap_regs.end(), reg) == spv_cb_heap_regs.end()) {
                    spv_cb_heap_regs.push_back(reg);
                }
            }
        }
    }
    std::sort(spv_cb_heap_regs.begin(), spv_cb_heap_regs.end());
    std::unordered_map<uint32_t, uint32_t> var_cb_heap;

    if (uses_uav0 || uses_uav1 || !spv_cb_heap_regs.empty()) {
        const uint32_t t_rta_u32 = alloc_id();
        const uint32_t t_uav_struct = alloc_id();
        const uint32_t t_ptr_sb_struct = alloc_id();
        t_ptr_sb_u32 = alloc_id();

        EmitSpvInst(sec_types, SpvOpTypeRuntimeArray, {t_rta_u32, t_u32});
        EmitSpvInst(sec_types, SpvOpTypeStruct, {t_uav_struct, t_rta_u32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_sb_struct, kSpvStorageStorageBuffer, t_uav_struct});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_sb_u32, kSpvStorageStorageBuffer, t_u32});

        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rta_u32, kSpvDecArrayStride, 4u});
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_uav_struct, 0u, kSpvDecOffset, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_uav_struct, kSpvDecBlock});

        if (uses_uav0) {
            var_uav0 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_sb_struct, var_uav0, kSpvStorageStorageBuffer});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav0, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav0, kSpvDecBinding, 0u});
            register_spv14_global(var_uav0);
        }
        if (uses_uav1) {
            var_uav1 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_sb_struct, var_uav1, kSpvStorageStorageBuffer});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav1, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav1, kSpvDecBinding, 1u});
            register_spv14_global(var_uav1);
        }
        for (uint32_t cb_reg : spv_cb_heap_regs) {
            const uint32_t v_cb = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_sb_struct, v_cb, kSpvStorageStorageBuffer});
            EmitSpvInst(sec_decor, SpvOpDecorate, {v_cb, kSpvDecDescriptorSet, 1u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {v_cb, kSpvDecBinding, cb_reg});
            EmitSpvInst(sec_decor, SpvOpDecorate, {v_cb, kSpvDecNonWritable});
            register_spv14_global(v_cb);
            var_cb_heap[cb_reg] = v_cb;
        }
    }

    // Root constants UBO (individual scalar uint members -> 100% std140 compliant)
    uint32_t var_rc = 0;
    uint32_t t_ptr_unif_u32 = 0;
    if (ir.uses_root_constants) {
        const uint32_t rc_count = ir.max_root_const_index + 1u;
        const uint32_t t_rc_struct = alloc_id();
        std::vector<uint32_t> struct_ops = {t_rc_struct};
        for (uint32_t i = 0; i < rc_count; ++i) {
            struct_ops.push_back(t_u32);
            EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_rc_struct, i, kSpvDecOffset, i * 4u});
        }
        EmitSpvInst(sec_types, SpvOpTypeStruct, struct_ops);
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rc_struct, kSpvDecBlock});

        const uint32_t t_ptr_rc_struct = alloc_id();
        t_ptr_unif_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_rc_struct, kSpvStorageUniform, t_rc_struct});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_unif_u32, kSpvStorageUniform, t_u32});

        var_rc = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_rc_struct, var_rc, kSpvStorageUniform});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rc, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rc, kSpvDecBinding, 2u});
        register_spv14_global(var_rc);
    }

    // MIMG Texture & Sampler descriptors
    uint32_t t_img2d = 0;
    uint32_t t_sampler = 0;
    uint32_t t_sampled_img2d = 0;
    uint32_t var_tex0 = 0;
    uint32_t var_samp0 = 0;
    uint32_t t_rwimg2d = 0;
    uint32_t t_roimg2d = 0;
    uint32_t var_rwtex0 = 0;
    uint32_t var_img_ro = 0;
    uint32_t t_img3d = 0;
    uint32_t var_tex3d = 0;
    uint32_t t_imgcube = 0;
    uint32_t var_texcube = 0;
    uint32_t t_rwimg_u32 = 0;
    uint32_t t_ptr_img_u32 = 0;
    uint32_t var_rwimg_u32 = 0;
    const bool tex_srv_is_u32 = (ir.texture_srv_format == Rdna2TextureFormat::Uint4);

    if (ir.uses_texture_sample || ir.uses_texture2d_mip_srv) {
        t_img2d = alloc_id();
        const uint32_t t_ptr_uc_img = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage,
                    {t_img2d, tex_srv_is_u32 ? t_u32 : t_f32, 1u, 0u, 0u, 0u, 1u, kSpvImgFmtUnknown});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_img, kSpvStorageUniformConstant, t_img2d});
        var_tex0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_img, var_tex0, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex0, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex0, kSpvDecBinding, 3u});
        register_spv14_global(var_tex0);

        if (ir.uses_texture_sample) {
            t_sampler = alloc_id();
            t_sampled_img2d = alloc_id();
            const uint32_t t_ptr_uc_samp = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypeSampler, {t_sampler});
            EmitSpvInst(sec_types, SpvOpTypeSampledImage, {t_sampled_img2d, t_img2d});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_samp, kSpvStorageUniformConstant, t_sampler});
            var_samp0 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_samp, var_samp0, kSpvStorageUniformConstant});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_samp0, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_samp0, kSpvDecBinding, 4u});
            register_spv14_global(var_samp0);
        }
    }

    if (ir.uses_texture_uav || (ir.uses_texture_load && !tex_srv_is_u32)) {
        t_rwimg2d = alloc_id();
        const uint32_t t_ptr_uc_rwimg = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_rwimg2d, t_f32, 1u, 0u, 0u, 0u, 2u, kSpvImgFmtRgba32f});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_rwimg, kSpvStorageUniformConstant, t_rwimg2d});

        if (ir.uses_texture_uav) {
            var_rwtex0 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_rwimg, var_rwtex0, kSpvStorageUniformConstant});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwtex0, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwtex0, kSpvDecBinding, 5u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwtex0, kSpvDecNonReadable});
            register_spv14_global(var_rwtex0);
        }
        if (ir.uses_texture_load && !tex_srv_is_u32) {
            t_roimg2d = t_rwimg2d;
            var_img_ro = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_rwimg, var_img_ro, kSpvStorageUniformConstant});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecBinding, 6u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecNonWritable});
            register_spv14_global(var_img_ro);
        }
    }
    if (ir.uses_texture_load && tex_srv_is_u32) {
        t_roimg2d = alloc_id();
        const uint32_t t_ptr_uc_roimg = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_roimg2d, t_u32, 1u, 0u, 0u, 0u, 2u, kSpvImgFmtRgba32ui});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_roimg, kSpvStorageUniformConstant, t_roimg2d});
        var_img_ro = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_roimg, var_img_ro, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecBinding, 6u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecNonWritable});
        register_spv14_global(var_img_ro);
    }

    if (ir.uses_texture3d_srv) {
        t_img3d = alloc_id();
        const uint32_t t_ptr_uc_img3d = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_img3d, t_f32, 2u, 0u, 0u, 0u, 1u, kSpvImgFmtUnknown});  // 3D
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_img3d, kSpvStorageUniformConstant, t_img3d});
        var_tex3d = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_img3d, var_tex3d, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex3d, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex3d, kSpvDecBinding, 7u});
        register_spv14_global(var_tex3d);
    }

    if (ir.uses_texture_cube_srv) {
        t_imgcube = alloc_id();
        const uint32_t t_ptr_uc_cube = alloc_id();
        // 2D Arrayed (6 cube faces) for integer texel + mip OpImageFetch
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_imgcube, t_f32, 1u, 0u, 1u, 0u, 1u, kSpvImgFmtUnknown});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_cube, kSpvStorageUniformConstant, t_imgcube});
        var_texcube = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_cube, var_texcube, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_texcube, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_texcube, kSpvDecBinding, 8u});
        register_spv14_global(var_texcube);
    }

    if (ir.uses_texture_u32_uav) {
        t_rwimg_u32 = alloc_id();
        const uint32_t t_ptr_uc_u32 = alloc_id();
        t_ptr_img_u32 = alloc_id();
        const uint32_t spv_dim = (ir.texture_u32_uav_dim == 2u) ? 2u : 1u;
        const uint32_t spv_arr = (ir.texture_u32_uav_dim == 3u) ? 1u : 0u;
        EmitSpvInst(sec_types, SpvOpTypeImage,
                    {t_rwimg_u32, t_u32, spv_dim, 0u, spv_arr, 0u, 2u, kSpvImgFmtR32ui});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_u32, kSpvStorageUniformConstant, t_rwimg_u32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_img_u32, kSpvStorageImage, t_u32});
        var_rwimg_u32 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_u32, var_rwimg_u32, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwimg_u32, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwimg_u32, kSpvDecBinding, 9u});
        register_spv14_global(var_rwimg_u32);
    }

    // Thread-private scratch memory (Private storage class array, zero-initialized)
    uint32_t t_ptr_priv_u32 = 0;
    uint32_t var_scratch = 0;
    const uint32_t scratch_size_dwords = ir.scratch_size_dwords ? ir.scratch_size_dwords : 64u;
    const uint32_t c_scratch_mask = get_const_u32(scratch_size_dwords - 1u);
    if (ir.uses_scratch) {
        const uint32_t c_scr_len = get_const_u32(scratch_size_dwords);
        const uint32_t t_scr_arr = alloc_id();
        const uint32_t t_ptr_priv_scr = alloc_id();
        t_ptr_priv_u32 = alloc_id();
        const uint32_t c_null_scr = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_scr_arr, t_u32, c_scr_len});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_priv_scr, kSpvStoragePrivate, t_scr_arr});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_priv_u32, kSpvStoragePrivate, t_u32});
        EmitSpvInst(sec_types, SpvOpConstantNull, {t_scr_arr, c_null_scr});
        var_scratch = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_priv_scr, var_scratch, kSpvStoragePrivate, c_null_scr});
        register_spv14_global(var_scratch);
    }

    // Workgroup shared memory for LDS and/or wave64->wave32 emulation
    uint32_t t_ptr_wg_u32 = 0;
    uint32_t var_lds = 0;
    uint32_t var_w64_ballot = 0;
    uint32_t var_w64_lanes = 0;
    const uint32_t lds_size_dwords = ir.lds_size_dwords ? ir.lds_size_dwords : 1024u;
    const uint32_t c_lds_len_val = ir.uses_lds ? get_const_u32(lds_size_dwords) : 0u;
    const uint32_t c_tg_stride_val =
        (ir.uses_lds && (ir.stage == ShaderStage::Compute || is_task_stage || is_mesh_stage))
            ? get_const_u32(std::max(1u, ir.threadgroup_x * ir.threadgroup_y * ir.threadgroup_z))
            : 0u;
    const uint32_t c_lds_mask = get_const_u32(lds_size_dwords - 1u);
    const uint32_t w_lanes_cap = ComputeWaveLanesCapacity(ir);
    const uint32_t c_w_lanes_mask = get_const_u32(w_lanes_cap - 1u);

    if (ir.uses_lds || ir.uses_wave64_emulation) {
        t_ptr_wg_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_u32, kSpvStorageWorkgroup, t_u32});
    }
    if (ir.uses_lds) {
        const uint32_t t_lds_arr = alloc_id();
        const uint32_t t_ptr_wg_lds = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_lds_arr, t_u32, c_lds_len_val});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_lds, kSpvStorageWorkgroup, t_lds_arr});
        var_lds = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_lds, var_lds, kSpvStorageWorkgroup});
        register_spv14_global(var_lds);
    }
    if (ir.uses_wave64_emulation) {
        const uint32_t max_waves = ComputeMaxGuestWaves(ir);
        const uint32_t c_len_ballot = get_const_u32(max_waves * 2u);
        const uint32_t c_len_lanes = get_const_u32(w_lanes_cap);
        const uint32_t t_arr_2 = alloc_id();
        const uint32_t t_arr_lanes = alloc_id();
        const uint32_t t_ptr_wg_2 = alloc_id();
        const uint32_t t_ptr_wg_lanes = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_2, t_u32, c_len_ballot});
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_lanes, t_u32, c_len_lanes});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_2, kSpvStorageWorkgroup, t_arr_2});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_lanes, kSpvStorageWorkgroup, t_arr_lanes});
        var_w64_ballot = alloc_id();
        var_w64_lanes = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_2, var_w64_ballot, kSpvStorageWorkgroup});
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_lanes, var_w64_lanes, kSpvStorageWorkgroup});
        register_spv14_global(var_w64_ballot);
        register_spv14_global(var_w64_lanes);
    }

    const bool uses_tid = ModuleUsesThreadId(ir);
    const bool uses_stage_in = ModuleUsesStageInput(ir);

    if (ir.stage == ShaderStage::Compute) {
        if (uses_tid || ir.uses_wave64_emulation || ir.uses_lds) {
            t_ptr_in_u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, kSpvStorageInput, t_u32});
        }
        if (uses_tid) {
            const uint32_t t_ptr_in_v3u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v3u32, kSpvStorageInput, t_v3u32});
            var_gid = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v3u32, var_gid, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_gid, kSpvDecBuiltIn, kSpvBuiltInGlobalInvocationId});
            interface_vars.push_back(var_gid);
        }
        if (ir.uses_wave64_emulation || ir.uses_lds) {
            var_local_idx = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_u32, var_local_idx, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_local_idx, kSpvDecBuiltIn, kSpvBuiltInLocalInvocationIndex});
            interface_vars.push_back(var_local_idx);
        }
    }

    if (ir.stage == ShaderStage::Vertex) {
        if (uses_tid) {
            const uint32_t t_ptr_in_i32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_i32, kSpvStorageInput, t_i32});
            var_vid = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_i32, var_vid, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_vid, kSpvDecBuiltIn, kSpvBuiltInVertexIndex});
            interface_vars.push_back(var_vid);
        }
        // Vulkan requires BuiltIn Position in a Block-decorated gl_PerVertex struct
        // (optionally with member 1 BuiltIn CullDistance float[N] when Pos2/Pos3 is exported)
        const uint32_t t_pervertex = alloc_id();
        const uint32_t t_ptr_out_pv = alloc_id();
        t_ptr_out_v4f32 = alloc_id();
        if (cull_dist_count > 0u) {
            const uint32_t c_cull_len = get_const_u32(cull_dist_count);
            const uint32_t t_arr_cull = alloc_id();
            t_ptr_out_f32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_cull, t_f32, c_cull_len});
            EmitSpvInst(sec_types, SpvOpTypeStruct, {t_pervertex, t_v4f32, t_arr_cull});
            EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_pervertex, 1u, kSpvDecBuiltIn, kSpvBuiltInCullDistance});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_f32, kSpvStorageOutput, t_f32});
        } else {
            EmitSpvInst(sec_types, SpvOpTypeStruct, {t_pervertex, t_v4f32});
        }
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_pervertex, kSpvDecBlock});
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_pervertex, 0u, kSpvDecBuiltIn, kSpvBuiltInPosition});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_pv, kSpvStorageOutput, t_pervertex});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, kSpvStorageOutput, t_v4f32});
        var_pervertex = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_pv, var_pervertex, kSpvStorageOutput});
        interface_vars.push_back(var_pervertex);

        for (uint32_t p = 0u; p < param_out_count; ++p) {
            var_param_out[p] = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_param_out[p], kSpvStorageOutput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_param_out[p], kSpvDecLocation, p});
            emit_interp_decor(var_param_out[p], SafeInterpMode(ir.output_interp_modes, p));
            interface_vars.push_back(var_param_out[p]);
        }
    }

    if (ir.stage == ShaderStage::Mesh) {
        const uint32_t max_v = ir.mesh_max_vertices ? ir.mesh_max_vertices : 64u;
        const uint32_t max_p = ir.mesh_max_primitives ? ir.mesh_max_primitives : 64u;
        const uint32_t c_max_v = get_const_u32(max_v);
        const uint32_t c_max_p = get_const_u32(max_p);

        t_ptr_in_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, kSpvStorageInput, t_u32});
        var_local_idx = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_u32, var_local_idx, kSpvStorageInput});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_local_idx, kSpvDecBuiltIn, kSpvBuiltInLocalInvocationIndex});
        interface_vars.push_back(var_local_idx);

        if (ir.uses_mesh_global_tid) {
            const uint32_t t_ptr_in_v3u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v3u32, kSpvStorageInput, t_v3u32});
            var_wg_id = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v3u32, var_wg_id, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_wg_id, kSpvDecBuiltIn, kSpvBuiltInWorkgroupId});
            interface_vars.push_back(var_wg_id);
        }

        // Per-vertex gl_MeshVerticesEXT[] (Block-decorated struct with BuiltIn Position + optional CullDistance)
        const uint32_t t_pervertex = alloc_id();
        const uint32_t t_arr_pv = alloc_id();
        const uint32_t t_ptr_out_pv_arr = alloc_id();
        t_ptr_out_v4f32 = alloc_id();
        if (cull_dist_count > 0u) {
            const uint32_t c_cull_len = get_const_u32(cull_dist_count);
            const uint32_t t_arr_cull = alloc_id();
            t_ptr_out_f32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_cull, t_f32, c_cull_len});
            EmitSpvInst(sec_types, SpvOpTypeStruct, {t_pervertex, t_v4f32, t_arr_cull});
            EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_pervertex, 1u, kSpvDecBuiltIn, kSpvBuiltInCullDistance});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_f32, kSpvStorageOutput, t_f32});
        } else {
            EmitSpvInst(sec_types, SpvOpTypeStruct, {t_pervertex, t_v4f32});
        }
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_pervertex, kSpvDecBlock});
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_pervertex, 0u, kSpvDecBuiltIn, kSpvBuiltInPosition});
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_pv, t_pervertex, c_max_v});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_pv_arr, kSpvStorageOutput, t_arr_pv});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, kSpvStorageOutput, t_v4f32});
        var_pervertex = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_pv_arr, var_pervertex, kSpvStorageOutput});
        interface_vars.push_back(var_pervertex);

        if (param_out_count > 0u) {
            const uint32_t t_arr_v4 = alloc_id();
            const uint32_t t_ptr_out_arr_v4 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_v4, t_v4f32, c_max_v});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_arr_v4, kSpvStorageOutput, t_arr_v4});
            for (uint32_t p = 0u; p < param_out_count; ++p) {
                var_param_out[p] = alloc_id();
                EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_arr_v4, var_param_out[p], kSpvStorageOutput});
                EmitSpvInst(sec_decor, SpvOpDecorate, {var_param_out[p], kSpvDecLocation, p});
                emit_interp_decor(var_param_out[p], SafeInterpMode(ir.output_interp_modes, p));
                interface_vars.push_back(var_param_out[p]);
            }
        }

        // Primitive triangle indices output array
        const uint32_t t_arr_prims = alloc_id();
        const uint32_t t_ptr_out_prims = alloc_id();
        t_ptr_out_v3u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_prims, t_v3u32, c_max_p});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_prims, kSpvStorageOutput, t_arr_prims});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v3u32, kSpvStorageOutput, t_v3u32});
        var_mesh_prims = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_prims, var_mesh_prims, kSpvStorageOutput});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_mesh_prims, kSpvDecBuiltIn, kSpvBuiltInPrimitiveTriangleIndicesEXT});
        interface_vars.push_back(var_mesh_prims);

        // Emit CullPrimitiveEXT as Block+bool struct array for older spirv-val (v2022.4+) compatibility!
        if (ir.uses_mesh_cull) {
            const uint32_t t_cull_blk = alloc_id();
            const uint32_t t_arr_cull = alloc_id();
            const uint32_t t_ptr_out_cull = alloc_id();
            t_ptr_out_bool = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypeStruct, {t_cull_blk, t_bool});
            EmitSpvInst(sec_decor, SpvOpDecorate, {t_cull_blk, kSpvDecBlock});
            EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_cull_blk, 0u, kSpvDecBuiltIn, kSpvBuiltInCullPrimitiveEXT});
            EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_cull, t_cull_blk, c_max_p});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_cull, kSpvStorageOutput, t_arr_cull});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_bool, kSpvStorageOutput, t_bool});
            var_mesh_cull = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_cull, var_mesh_cull, kSpvStorageOutput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_mesh_cull, kSpvDecPerPrimitiveEXT});
            interface_vars.push_back(var_mesh_cull);
        }
    }

    if (is_task_stage) {
        t_ptr_in_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, kSpvStorageInput, t_u32});
        var_local_idx = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_u32, var_local_idx, kSpvStorageInput});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_local_idx, kSpvDecBuiltIn, kSpvBuiltInLocalInvocationIndex});
        interface_vars.push_back(var_local_idx);

        if (ir.uses_mesh_global_tid) {
            const uint32_t t_ptr_in_v3u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v3u32, kSpvStorageInput, t_v3u32});
            var_wg_id = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v3u32, var_wg_id, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_wg_id, kSpvDecBuiltIn, kSpvBuiltInWorkgroupId});
            interface_vars.push_back(var_wg_id);
        }

        // Workgroup task payload in TaskPayloadWorkgroupEXT storage class:
        //   struct TaskPayload { uint data[4]; uint dispatch_x; uint dispatch_y; uint dispatch_z; };
        const uint32_t c_u32_4 = get_const_u32(4u);
        const uint32_t t_tp_arr = alloc_id();
        const uint32_t t_tp_struct = alloc_id();
        const uint32_t t_ptr_tp_struct = alloc_id();
        t_ptr_tp_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_tp_arr, t_u32, c_u32_4});
        EmitSpvInst(sec_types, SpvOpTypeStruct, {t_tp_struct, t_tp_arr, t_u32, t_u32, t_u32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_tp_struct, kSpvStorageTaskPayloadWorkgroupEXT, t_tp_struct});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_tp_u32, kSpvStorageTaskPayloadWorkgroupEXT, t_u32});
        var_task_payload = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable,
                    {t_ptr_tp_struct, var_task_payload, kSpvStorageTaskPayloadWorkgroupEXT});
        interface_vars.push_back(var_task_payload);

        t_ptr_fn_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_fn_u32, kSpvStorageFunction, t_u32});
    }

    if (ir.stage == ShaderStage::Pixel) {
        if (uses_stage_in && param_in_count > 0u) {
            const uint32_t t_ptr_in_v4f32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v4f32, kSpvStorageInput, t_v4f32});
            for (uint32_t p = 0u; p < param_in_count; ++p) {
                var_param_in[p] = alloc_id();
                EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v4f32, var_param_in[p], kSpvStorageInput});
                EmitSpvInst(sec_decor, SpvOpDecorate, {var_param_in[p], kSpvDecLocation, p});
                emit_interp_decor(var_param_in[p], SafeInterpMode(ir.input_interp_modes, p));
                interface_vars.push_back(var_param_in[p]);
            }
        }
        t_ptr_out_v4f32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, kSpvStorageOutput, t_v4f32});
        var_mrt0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_mrt0, kSpvStorageOutput});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_mrt0, kSpvDecLocation, 0u});
        interface_vars.push_back(var_mrt0);
    }

    // 4. Entry point & execution mode
    const uint32_t main_fn_id = alloc_id();
    {
        uint32_t exec_model = 5u;  // GLCompute
        if (ir.stage == ShaderStage::Vertex) exec_model = 0u;
        else if (ir.stage == ShaderStage::Pixel) exec_model = 4u;
        else if (ir.stage == ShaderStage::Mesh) exec_model = kSpvExecModelMeshEXT;
        else if (ir.stage == ShaderStage::Amplification) exec_model = kSpvExecModelTaskEXT;

        std::vector<uint32_t> ops = {exec_model, main_fn_id};
        AppendSpvString(ops, "main");
        ops.insert(ops.end(), interface_vars.begin(), interface_vars.end());
        EmitSpvInst(sec_entry, SpvOpEntryPoint, ops);

        if (ir.stage == ShaderStage::Compute) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode,
                        {main_fn_id, 17u,
                         std::max(1u, ir.threadgroup_x),
                         std::max(1u, ir.threadgroup_y),
                         std::max(1u, ir.threadgroup_z)});
        } else if (ir.stage == ShaderStage::Pixel) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode, {main_fn_id, 7u});  // OriginUpperLeft
        } else if (ir.stage == ShaderStage::Mesh) {
            const uint32_t max_v = ir.mesh_max_vertices ? ir.mesh_max_vertices : 64u;
            const uint32_t max_p = ir.mesh_max_primitives ? ir.mesh_max_primitives : 64u;
            EmitSpvInst(sec_exec, SpvOpExecutionMode,
                        {main_fn_id, 17u,
                         std::max(1u, ir.threadgroup_x),
                         std::max(1u, ir.threadgroup_y),
                         std::max(1u, ir.threadgroup_z)});
            EmitSpvInst(sec_exec, SpvOpExecutionMode, {main_fn_id, kSpvExecModeOutputVertices, max_v});
            EmitSpvInst(sec_exec, SpvOpExecutionMode, {main_fn_id, kSpvExecModeOutputPrimitivesEXT, max_p});
            EmitSpvInst(sec_exec, SpvOpExecutionMode, {main_fn_id, kSpvExecModeOutputTrianglesEXT});
        } else if (ir.stage == ShaderStage::Amplification) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode,
                        {main_fn_id, 17u,
                         std::max(1u, ir.threadgroup_x),
                         std::max(1u, ir.threadgroup_y),
                         std::max(1u, ir.threadgroup_z)});
        }
    }

    // 5. Block labels, CFG cycle/irreducibility analysis, immediate post-dominators, and value IDs
    Rdna2CfgAnalysis spv_cfg{};
    AnalyzeRdna2Cfg(ir, spv_cfg);
    bool has_backward_block_edge = false;
    for (size_t b = 0; b < ir.blocks.size() && !has_backward_block_edge; ++b) {
        for (uint32_t s : ir.blocks[b].successors) {
            if (s <= b) {
                has_backward_block_edge = true;
                break;
            }
        }
    }
    const bool use_spv_state_machine =
        (spv_cfg.has_cycles || spv_cfg.is_irreducible || has_backward_block_edge);

    const std::vector<uint32_t> ipdom = ComputeImmediatePostDominators(ir);
    const bool has_cond_branch = ModuleHasCondBranch(ir);
    const uint32_t exit_node = static_cast<uint32_t>(ir.blocks.size());
    bool need_epilogue = is_task_stage;
    if (!use_spv_state_machine && has_cond_branch) {
        for (size_t b = 0; b < ir.blocks.size(); ++b) {
            const auto& ins = ir.blocks[b].instructions;
            if (!ins.empty() && ins.back().op == IrOp::CondBranch && ipdom[b] == exit_node) {
                need_epilogue = true;
                break;
            }
        }
    }

    std::vector<uint32_t> bb_labels(ir.blocks.size(), 0u);
    for (size_t i = 0; i < ir.blocks.size(); ++i) {
        bb_labels[i] = alloc_id();
    }
    const uint32_t epilogue_label = need_epilogue ? alloc_id() : 0u;

    uint32_t t_ptr_fn_f32 = 0u;
    uint32_t t_ptr_fn_bool = 0u;
    uint32_t var_cfg_state = 0u;
    uint32_t lbl_sm_hdr = 0u;
    uint32_t lbl_sm_dispatch = 0u;
    uint32_t lbl_sm_default = 0u;
    uint32_t lbl_sm_sw_merge = 0u;
    uint32_t lbl_sm_cont = 0u;
    uint32_t lbl_sm_exit = 0u;
    std::unordered_map<IrValueId, uint32_t> val_fn_var;

    if (use_spv_state_machine) {
        if (t_ptr_fn_u32 == 0u) {
            t_ptr_fn_u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_fn_u32, kSpvStorageFunction, t_u32});
        }
        t_ptr_fn_f32 = alloc_id();
        t_ptr_fn_bool = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_fn_f32, kSpvStorageFunction, t_f32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_fn_bool, kSpvStorageFunction, t_bool});
        lbl_sm_hdr = alloc_id();
        lbl_sm_dispatch = alloc_id();
        lbl_sm_default = alloc_id();
        lbl_sm_sw_merge = alloc_id();
        lbl_sm_cont = alloc_id();
        lbl_sm_exit = alloc_id();
    }

    std::unordered_map<IrValueId, uint32_t> val_spv_id;
    std::unordered_map<IrValueId, SpvValKind> val_kind;
    std::vector<std::pair<IrValueId, SpvValKind>> sm_fn_var_order;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.id == kInvalidIrValue) continue;
            const SpvValKind k = CanonicalSpvKind(inst.type);
            val_kind[inst.id] = k;
            if (inst.op == IrOp::ConstU32) {
                if (inst.type == IrType::Bool) {
                    val_spv_id[inst.id] = get_const_bool(inst.imm_u32 != 0u);
                } else {
                    val_spv_id[inst.id] = get_const_u32(inst.imm_u32);
                }
            } else if (inst.op == IrOp::ConstF32) {
                val_spv_id[inst.id] = get_const_f32(inst.imm_f32);
            } else if (use_spv_state_machine) {
                const uint32_t v_id = alloc_id();
                val_fn_var[inst.id] = v_id;
                val_spv_id[inst.id] = v_id;
                sm_fn_var_order.emplace_back(inst.id, k);
            } else {
                val_spv_id[inst.id] = alloc_id();
            }
        }
    }

    auto spv_type_for_kind = [&](SpvValKind k) -> uint32_t {
        if (k == SpvValKind::F32) return t_f32;
        if (k == SpvValKind::Bool) return t_bool;
        return t_u32;
    };

    auto get_raw_val = [&](IrValueId v) -> uint32_t {
        if (use_spv_state_machine) {
            auto it = val_fn_var.find(v);
            if (it != val_fn_var.end()) {
                const uint32_t loaded = alloc_id();
                EmitSpvInst(sec_fn, SpvOpLoad, {spv_type_for_kind(val_kind[v]), loaded, it->second});
                return loaded;
            }
        }
        return val_spv_id[v];
    };

    auto get_as_u32 = [&](IrValueId v) -> uint32_t {
        const uint32_t id = get_raw_val(v);
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::U32) return id;
        if (k == SpvValKind::F32) {
            uint32_t cast_id = alloc_id();
            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, cast_id, id});
            return cast_id;
        }
        uint32_t sel_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sel_id, id, c_u32_1, c_u32_0});
        return sel_id;
    };

    auto get_as_i32 = [&](IrValueId v) -> uint32_t {
        uint32_t u = get_as_u32(v);
        uint32_t i = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, i, u});
        return i;
    };

    auto get_as_f32 = [&](IrValueId v) -> uint32_t {
        const uint32_t id = get_raw_val(v);
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::F32) return id;
        uint32_t u = get_as_u32(v);
        uint32_t cast_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, cast_id, u});
        return cast_id;
    };

    auto get_as_bool = [&](IrValueId v) -> uint32_t {
        const uint32_t id = get_raw_val(v);
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::Bool) return id;
        uint32_t u = get_as_u32(v);
        uint32_t cmp_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, cmp_id, u, c_u32_0});
        return cmp_id;
    };

    auto emit_sm_fn_vars = [&]() {
        if (!use_spv_state_machine) return;
        var_cfg_state = alloc_id();
        EmitSpvInst(sec_fn, SpvOpVariable, {t_ptr_fn_u32, var_cfg_state, kSpvStorageFunction, c_u32_0});
        for (const auto& entry : sm_fn_var_order) {
            const uint32_t v_id = val_fn_var[entry.first];
            if (entry.second == SpvValKind::F32) {
                EmitSpvInst(sec_fn, SpvOpVariable, {t_ptr_fn_f32, v_id, kSpvStorageFunction, c_f32_0});
            } else if (entry.second == SpvValKind::Bool) {
                EmitSpvInst(sec_fn, SpvOpVariable,
                            {t_ptr_fn_bool, v_id, kSpvStorageFunction, get_const_bool(false)});
            } else {
                EmitSpvInst(sec_fn, SpvOpVariable, {t_ptr_fn_u32, v_id, kSpvStorageFunction, c_u32_0});
            }
        }
    };

    auto emit_sm_phi_copies = [&](uint32_t from_bb, uint32_t to_bb) {
        if (!use_spv_state_machine || to_bb >= ir.blocks.size()) return;
        std::vector<std::pair<uint32_t, uint32_t>> staged;  // (dst_fn_var, loaded_val)
        for (const auto& inst : ir.blocks[to_bb].instructions) {
            if (inst.op != IrOp::Phi) continue;
            auto it_dst = val_fn_var.find(inst.id);
            if (it_dst == val_fn_var.end()) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) {
                    const SpvValKind k = CanonicalSpvKind(inst.type);
                    uint32_t v_val = (k == SpvValKind::F32)
                                         ? get_as_f32(inc.second)
                                         : (k == SpvValKind::Bool ? get_as_bool(inc.second)
                                                                  : get_as_u32(inc.second));
                    staged.emplace_back(it_dst->second, v_val);
                    break;
                }
            }
        }
        for (const auto& s : staged) {
            EmitSpvInst(sec_fn, SpvOpStore, {s.first, s.second});
        }
    };

    auto emit_wg_barrier = [&]() {
        const uint32_t c_scope = get_const_u32(kSpvScopeWorkgroup);
        const uint32_t c_sem = get_const_u32(kSpvMemSemAcqRelWorkgroup);
        EmitSpvInst(sec_fn, SpvOpControlBarrier, {c_scope, c_scope, c_sem});
    };

    auto emit_guest_wave_coords = [&](uint32_t& out_gw_id, uint32_t& out_gw_lane, uint32_t& out_gw_base) {
        const uint32_t raw_idx = alloc_id();
        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, raw_idx, var_local_idx});
        const uint32_t shift_c = get_const_u32((ir.wave_size == 64u) ? 6u : 5u);
        const uint32_t mask_c = (ir.wave_size == 64u) ? c_u32_63 : c_u32_31;
        out_gw_id = alloc_id();
        out_gw_lane = alloc_id();
        out_gw_base = alloc_id();
        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, out_gw_id, raw_idx, shift_c});
        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, out_gw_lane, raw_idx, mask_c});
        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, out_gw_base, out_gw_id, shift_c});
    };

    auto emit_lane_id = [&]() -> uint32_t {
        uint32_t gw_id = 0, gw_lane = 0, gw_base = 0;
        emit_guest_wave_coords(gw_id, gw_lane, gw_base);
        return gw_lane;
    };

    // Precompute any Phi incoming edge coercions
    struct PhiEdgeFixup {
        uint32_t target_phi_id;
        SpvValKind expected_kind;
        IrValueId incoming_val;
        uint32_t coerced_spv_id;
    };
    std::unordered_map<uint64_t, uint32_t> phi_edge_override;
    std::vector<std::vector<PhiEdgeFixup>> block_exit_fixups(ir.blocks.size());

    if (!use_spv_state_machine) {
        for (const auto& bb : ir.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.op != IrOp::Phi) continue;
                const SpvValKind expected = CanonicalSpvKind(inst.type);
                for (const auto& inc : inst.phi_incoming) {
                    const uint32_t pred = inc.first;
                    const IrValueId v = inc.second;
                    if (val_kind[v] != expected && pred < ir.blocks.size()) {
                        const uint32_t coerced = alloc_id();
                        block_exit_fixups[pred].push_back({inst.id, expected, v, coerced});
                        phi_edge_override[(static_cast<uint64_t>(inst.id) << 32) | pred] = coerced;
                    }
                }
            }
        }
    }

    // 6. Emit Function Body
    EmitSpvInst(sec_fn, SpvOpFunction, {t_void, main_fn_id, 0u, t_fn_void});
    const uint32_t first_body_target = use_spv_state_machine ? lbl_sm_hdr : bb_labels[0];

    // TaskEXT entry preamble: declare Function-scope variables first, then zero-init TaskPayload + LDS.
    if (is_task_stage) {
        const uint32_t lbl_task_entry = alloc_id();
        const uint32_t lbl_tp_zero = alloc_id();
        const uint32_t lbl_tp_done = alloc_id();

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_task_entry});
        var_dm_x = alloc_id();
        var_dm_y = alloc_id();
        var_dm_z = alloc_id();
        EmitSpvInst(sec_fn, SpvOpVariable, {t_ptr_fn_u32, var_dm_x, kSpvStorageFunction, c_u32_1});
        EmitSpvInst(sec_fn, SpvOpVariable, {t_ptr_fn_u32, var_dm_y, kSpvStorageFunction, c_u32_1});
        EmitSpvInst(sec_fn, SpvOpVariable, {t_ptr_fn_u32, var_dm_z, kSpvStorageFunction, c_u32_1});
        emit_sm_fn_vars();

        const uint32_t init_lidx = alloc_id();
        const uint32_t is_l0 = alloc_id();
        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, init_lidx, var_local_idx});
        EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, is_l0, init_lidx, c_u32_0});
        EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_tp_done, 0u});
        EmitSpvInst(sec_fn, SpvOpBranchConditional, {is_l0, lbl_tp_zero, lbl_tp_done});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_tp_zero});
        for (uint32_t k = 0u; k < 4u; ++k) {
            const uint32_t p_d = alloc_id();
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_d, var_task_payload, c_i32_0, get_const_u32(k)});
            EmitSpvInst(sec_fn, SpvOpStore, {p_d, c_u32_0});
        }
        {
            const uint32_t p_dx = alloc_id();
            const uint32_t p_dy = alloc_id();
            const uint32_t p_dz = alloc_id();
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_dx, var_task_payload, get_const_i32(1)});
            EmitSpvInst(sec_fn, SpvOpStore, {p_dx, c_u32_1});
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_dy, var_task_payload, get_const_i32(2)});
            EmitSpvInst(sec_fn, SpvOpStore, {p_dy, c_u32_1});
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_dz, var_task_payload, get_const_i32(3)});
            EmitSpvInst(sec_fn, SpvOpStore, {p_dz, c_u32_1});
        }
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_tp_done});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_tp_done});
        if (ir.uses_lds && var_lds != 0u) {
            const uint32_t lbl_loop_hdr = alloc_id();
            const uint32_t lbl_loop_body = alloc_id();
            const uint32_t lbl_loop_cont = alloc_id();
            const uint32_t lbl_loop_merge = alloc_id();

            const uint32_t phi_idx = alloc_id();
            const uint32_t next_idx = alloc_id();
            const uint32_t cmp_lt = alloc_id();
            const uint32_t lds_ptr = alloc_id();

            EmitSpvInst(sec_fn, SpvOpBranch, {lbl_loop_hdr});

            EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_hdr});
            EmitSpvInst(sec_fn, SpvOpPhi, {t_u32, phi_idx, init_lidx, lbl_tp_done, next_idx, lbl_loop_cont});
            EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, cmp_lt, phi_idx, c_lds_len_val});
            EmitSpvInst(sec_fn, SpvOpLoopMerge, {lbl_loop_merge, lbl_loop_cont, 0u});
            EmitSpvInst(sec_fn, SpvOpBranchConditional, {cmp_lt, lbl_loop_body, lbl_loop_merge});

            EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_body});
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, lds_ptr, var_lds, phi_idx});
            EmitSpvInst(sec_fn, SpvOpStore, {lds_ptr, c_u32_0});
            EmitSpvInst(sec_fn, SpvOpBranch, {lbl_loop_cont});

            EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_cont});
            EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, next_idx, phi_idx, c_tg_stride_val});
            EmitSpvInst(sec_fn, SpvOpBranch, {lbl_loop_hdr});

            EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_merge});
        }
        emit_wg_barrier();
        EmitSpvInst(sec_fn, SpvOpBranch, {first_body_target});
    }

    // Cooperative Workgroup LDS zero-initialization loop at shader entry followed by OpControlBarrier
    const bool has_compute_lds_entry =
        (ir.uses_lds && (ir.stage == ShaderStage::Compute || is_mesh_stage) &&
         var_lds != 0u && var_local_idx != 0u);
    if (has_compute_lds_entry) {
        const uint32_t lbl_init_entry = alloc_id();
        const uint32_t lbl_loop_hdr = alloc_id();
        const uint32_t lbl_loop_body = alloc_id();
        const uint32_t lbl_loop_cont = alloc_id();
        const uint32_t lbl_loop_merge = alloc_id();

        const uint32_t init_gtid = alloc_id();
        const uint32_t phi_idx = alloc_id();
        const uint32_t next_idx = alloc_id();
        const uint32_t cmp_lt = alloc_id();
        const uint32_t lds_ptr = alloc_id();

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_init_entry});
        emit_sm_fn_vars();
        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, init_gtid, var_local_idx});
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_loop_hdr});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_hdr});
        EmitSpvInst(sec_fn, SpvOpPhi, {t_u32, phi_idx, init_gtid, lbl_init_entry, next_idx, lbl_loop_cont});
        EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, cmp_lt, phi_idx, c_lds_len_val});
        EmitSpvInst(sec_fn, SpvOpLoopMerge, {lbl_loop_merge, lbl_loop_cont, 0u});
        EmitSpvInst(sec_fn, SpvOpBranchConditional, {cmp_lt, lbl_loop_body, lbl_loop_merge});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_body});
        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, lds_ptr, var_lds, phi_idx});
        EmitSpvInst(sec_fn, SpvOpStore, {lds_ptr, c_u32_0});
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_loop_cont});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_cont});
        EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, next_idx, phi_idx, c_tg_stride_val});
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_loop_hdr});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_loop_merge});
        emit_wg_barrier();
        EmitSpvInst(sec_fn, SpvOpBranch, {first_body_target});
    } else if (use_spv_state_machine && !is_task_stage) {
        // Standalone entry block holding all Function-storage OpVariables for state-machine CFG
        const uint32_t lbl_sm_entry = alloc_id();
        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_entry});
        emit_sm_fn_vars();
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_hdr});
    }

    if (use_spv_state_machine) {
        // Structured state-machine loop header + OpSwitch dispatch (handles reducible & irreducible CFGs)
        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_hdr});
        const uint32_t cur_state = alloc_id();
        const uint32_t is_done = alloc_id();
        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, cur_state, var_cfg_state});
        EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, is_done, cur_state, c_u32_allones});
        EmitSpvInst(sec_fn, SpvOpLoopMerge, {lbl_sm_exit, lbl_sm_cont, 0u});
        EmitSpvInst(sec_fn, SpvOpBranchConditional, {is_done, lbl_sm_exit, lbl_sm_dispatch});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_dispatch});
        EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_sm_sw_merge, 0u});
        std::vector<uint32_t> sw_ops = {cur_state, lbl_sm_default};
        for (size_t bi = 0u; bi < ir.blocks.size(); ++bi) {
            sw_ops.push_back(static_cast<uint32_t>(bi));
            sw_ops.push_back(bb_labels[bi]);
        }
        EmitSpvInst(sec_fn, SpvOpSwitch, sw_ops);
    }

    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& bb = ir.blocks[bi];
        EmitSpvInst(sec_fn, SpvOpLabel, {bb_labels[bi]});

        // OpPhi strictly first after OpLabel (DAG mode only; SM mode writes Phi vars on incoming edges)
        if (!use_spv_state_machine) {
            for (const auto& inst : bb.instructions) {
                if (inst.op != IrOp::Phi) continue;
                const uint32_t res_type = spv_type_for_kind(CanonicalSpvKind(inst.type));
                std::vector<uint32_t> ops = {res_type, val_spv_id[inst.id]};
                for (const auto& inc : inst.phi_incoming) {
                    const uint64_t key = (static_cast<uint64_t>(inst.id) << 32) | inc.first;
                    auto ov = phi_edge_override.find(key);
                    const uint32_t v_id = (ov != phi_edge_override.end()) ? ov->second : val_spv_id[inc.second];
                    ops.push_back(v_id);
                    ops.push_back(bb_labels[inc.first]);
                }
                EmitSpvInst(sec_fn, SpvOpPhi, ops);
            }
        }

        // Per-block cache for consecutive BvhIntersectRay component extracts
        std::vector<IrValueId> last_bvh_ops;
        uint32_t last_bvh_vec4_id = 0u;

        // Non-Phi, non-terminator instructions
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Phi || inst.op == IrOp::ConstU32 || inst.op == IrOp::ConstF32 ||
                inst.op == IrOp::Branch || inst.op == IrOp::CondBranch || inst.op == IrOp::Return) {
                continue;
            }
            const uint32_t res = (inst.id != kInvalidIrValue)
                                     ? (use_spv_state_machine ? alloc_id() : val_spv_id[inst.id])
                                     : 0u;

            switch (inst.op) {
                case IrOp::ThreadIdX:
                    if (ir.stage == ShaderStage::Compute) {
                        uint32_t ptr_x = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_in_u32, ptr_x, var_gid, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_x});
                    } else if (ir.stage == ShaderStage::Vertex) {
                        uint32_t vid_i32 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_i32, vid_i32, var_vid});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, vid_i32});
                    } else if (ir.stage == ShaderStage::Mesh || ir.stage == ShaderStage::Amplification) {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, var_local_idx});
                    }
                    break;
                case IrOp::MeshGlobalThreadId: {
                    const uint32_t nt = inst.imm_u32 ? inst.imm_u32 : ir.mesh_num_threads;
                    uint32_t ptr_gx = alloc_id();
                    uint32_t gx = alloc_id();
                    uint32_t lidx = alloc_id();
                    uint32_t prod = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_in_u32, ptr_gx, var_wg_id, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, gx, ptr_gx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, lidx, var_local_idx});
                    EmitSpvInst(sec_fn, SpvOpIMul, {t_u32, prod, gx, get_const_u32(nt)});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, prod, lidx});
                    break;
                }
                case IrOp::MergedWaveInfo: {
                    const uint32_t tg_total = std::max(1u, ir.threadgroup_x * ir.threadgroup_y * ir.threadgroup_z);
                    const uint32_t ws = (ir.wave_size == 64u) ? 64u : 32u;
                    const uint32_t nwaves = std::min(15u, std::max(1u, (tg_total + ws - 1u) / ws));
                    Rdna2MergedWaveInfo mwi{};
                    mwi.es_thread_count = static_cast<uint8_t>(std::min(255u, std::min(ws, tg_total)));
                    mwi.gs_thread_count = static_cast<uint8_t>(std::min(255u, tg_total));
                    mwi.waves_in_group = static_cast<uint8_t>(nwaves);
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, get_const_u32(PackMergedWaveInfo(mwi)), c_u32_0});
                    break;
                }
                case IrOp::StageInput: {
                    const uint32_t pslot = (inst.imm_u32 >> 2) & 3u;
                    const uint32_t vin = (var_param_in[pslot] != 0u) ? var_param_in[pslot] : var_param_in[0];
                    uint32_t v4 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_v4f32, v4, vin});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, v4, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::LoadRootConst: {
                    uint32_t idx = get_const_i32(static_cast<int32_t>(inst.imm_u32));
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_unif_u32, ptr, var_rc, idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::LoadUav: {
                    uint32_t var_buf = (inst.imm_u32 == 1u && var_uav1 != 0u) ? var_uav1 : var_uav0;
                    if ((inst.imm_u32 & kIrLoadCbHeapFlagLifter) != 0u) {
                        auto cb_it = var_cb_heap.find(inst.imm_u32 & 0xFFu);
                        if (cb_it != var_cb_heap.end()) {
                            var_buf = cb_it->second;
                        }
                    }
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr, var_buf, c_i32_0, dword_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::StoreUav: {
                    const uint32_t var_buf = (inst.imm_u32 == 1u && var_uav1 != 0u) ? var_uav1 : var_uav0;
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr, var_buf, c_i32_0, dword_idx});
                    if (inst.exec_pred != kInvalidIrValue) {
                        uint32_t pred = get_as_bool(inst.exec_pred);
                        uint32_t lbl_then = alloc_id();
                        uint32_t lbl_merge = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_merge, 0u});
                        EmitSpvInst(sec_fn, SpvOpBranchConditional, {pred, lbl_then, lbl_merge});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_then});
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_merge});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_merge});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    }
                    break;
                }
                case IrOp::BufferLoadFormat: {
                    // UB-free sub-dword merge (all shift amounts strictly < 32) + MTBUF D16 format conversion
                    const uint32_t uav_slot = inst.imm_u32 & 0xFu;
                    const uint32_t fmt = (inst.imm_u32 >> 4) & 0x7Fu;
                    const uint32_t comp_idx = (inst.imm_u32 >> 12) & 0xFu;
                    const bool is_d16 = ((inst.imm_u32 >> 16) & 1u) != 0u;
                    const bool has_hi = ((inst.imm_u32 >> 17) & 1u) != 0u;
                    const uint32_t var_buf = (uav_slot == 1u && var_uav1 != 0u) ? var_uav1 : var_uav0;
                    uint32_t elem_bytes = 4u;
                    if (fmt == 1u || fmt == 2u || fmt == 3u || fmt == 4u ||
                        fmt == 10u || fmt == 11u || fmt == 12u || fmt == 13u ||
                        fmt == 28u || fmt == 29u || fmt == 30u || fmt == 31u) {
                        elem_bytes = 1u;
                    } else if (fmt == 5u || fmt == 6u || fmt == 7u || fmt == 8u || fmt == 9u ||
                               fmt == 17u || fmt == 18u || fmt == 19u || fmt == 20u || fmt == 21u ||
                               fmt == 47u || fmt == 48u || fmt == 49u || fmt == 50u || fmt == 51u) {
                        elem_bytes = 2u;
                    }
                    const uint32_t c_u32_3 = get_const_u32(3u);
                    const uint32_t c_u32_24 = get_const_u32(24u);
                    const uint32_t c_u32_255 = get_const_u32(255u);
                    const uint32_t c_f32_inv255 = get_const_f32(1.0f / 255.0f);
                    const uint32_t c_f32_inv127 = get_const_f32(1.0f / 127.0f);
                    const uint32_t c_f32_inv65535 = get_const_f32(1.0f / 65535.0f);
                    const uint32_t c_f32_inv32767 = get_const_f32(1.0f / 32767.0f);
                    const uint32_t c_f32_neg1 = get_const_f32(-1.0f);

                    auto load_unaligned_u32 = [&](uint32_t addr_id) -> uint32_t {
                        uint32_t dw0 = alloc_id();
                        uint32_t dw1 = alloc_id();
                        uint32_t ptr0 = alloc_id();
                        uint32_t ptr1 = alloc_id();
                        uint32_t w0 = alloc_id();
                        uint32_t w1 = alloc_id();
                        uint32_t byte_rem = alloc_id();
                        uint32_t bit_off = alloc_id();
                        uint32_t lo_part = alloc_id();
                        uint32_t raw_hi_sh = alloc_id();
                        uint32_t safe_hi_sh = alloc_id();
                        uint32_t hi_shifted = alloc_id();
                        uint32_t nz = alloc_id();
                        uint32_t hi_part = alloc_id();
                        uint32_t merged = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dw0, addr_id, c_u32_2});
                        EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, dw1, dw0, c_u32_1});
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr0, var_buf, c_i32_0, dw0});
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr1, var_buf, c_i32_0, dw1});
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, w0, ptr0});
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, w1, ptr1});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, byte_rem, addr_id, c_u32_3});
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, bit_off, byte_rem, get_const_u32(3u)});
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, lo_part, w0, bit_off});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, raw_hi_sh, c_u32_32, bit_off});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, safe_hi_sh, raw_hi_sh, c_u32_31});
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, hi_shifted, w1, safe_hi_sh});
                        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, nz, bit_off, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, hi_part, nz, hi_shifted, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, merged, lo_part, hi_part});
                        return merged;
                    };

                    auto decode_comp = [&](uint32_t raw_u, bool d16_out) -> uint32_t {
                        auto pack_f32_out = [&](uint32_t f_id) -> uint32_t {
                            if (d16_out) {
                                uint32_t v2 = alloc_id();
                                uint32_t pk = alloc_id();
                                uint32_t lo = alloc_id();
                                EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v2, f_id, c_f32_0});
                                EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, pk, glsl_ext_id, GlslPackHalf2x16, v2});
                                EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lo, pk, c_u32_65535});
                                return lo;
                            }
                            uint32_t u = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, u, f_id});
                            return u;
                        };
                        if (fmt == 1u || fmt == 10u || fmt == 28u) {  // R8 UNORM
                            uint32_t b = alloc_id();
                            uint32_t f = alloc_id();
                            uint32_t s = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, b, raw_u, c_u32_255});
                            EmitSpvInst(sec_fn, SpvOpConvertUToF, {t_f32, f, b});
                            EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, s, f, c_f32_inv255});
                            return pack_f32_out(s);
                        }
                        if (fmt == 2u || fmt == 11u || fmt == 29u) {  // R8 SNORM
                            uint32_t shl = alloc_id();
                            uint32_t shl_i = alloc_id();
                            uint32_t sext = alloc_id();
                            uint32_t f = alloc_id();
                            uint32_t sc = alloc_id();
                            uint32_t lo = alloc_id();
                            uint32_t cl = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, shl, raw_u, c_u32_24});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, shl_i, shl});
                            EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, sext, shl_i, c_u32_24});
                            EmitSpvInst(sec_fn, SpvOpConvertSToF, {t_f32, f, sext});
                            EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, sc, f, c_f32_inv127});
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, lo, glsl_ext_id, GlslFMax, sc, c_f32_neg1});
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, cl, glsl_ext_id, GlslFMin, lo, c_f32_1});
                            return pack_f32_out(cl);
                        }
                        if (fmt == 3u || fmt == 12u || fmt == 30u) {  // R8 UINT
                            uint32_t b = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, b, raw_u, c_u32_255});
                            return b;
                        }
                        if (fmt == 4u || fmt == 13u || fmt == 31u) {  // R8 SINT
                            uint32_t shl = alloc_id();
                            uint32_t shl_i = alloc_id();
                            uint32_t sext = alloc_id();
                            uint32_t su = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, shl, raw_u, c_u32_24});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, shl_i, shl});
                            EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, sext, shl_i, c_u32_24});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, su, sext});
                            if (!d16_out) return su;
                            uint32_t lo = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lo, su, c_u32_65535});
                            return lo;
                        }
                        if (fmt == 5u || fmt == 17u || fmt == 47u) {  // R16 UNORM
                            uint32_t h = alloc_id();
                            uint32_t f = alloc_id();
                            uint32_t s = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, h, raw_u, c_u32_65535});
                            EmitSpvInst(sec_fn, SpvOpConvertUToF, {t_f32, f, h});
                            EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, s, f, c_f32_inv65535});
                            return pack_f32_out(s);
                        }
                        if (fmt == 6u || fmt == 18u || fmt == 48u) {  // R16 SNORM
                            uint32_t shl = alloc_id();
                            uint32_t shl_i = alloc_id();
                            uint32_t sext = alloc_id();
                            uint32_t f = alloc_id();
                            uint32_t sc = alloc_id();
                            uint32_t lo = alloc_id();
                            uint32_t cl = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, shl, raw_u, c_u32_16});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, shl_i, shl});
                            EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, sext, shl_i, c_u32_16});
                            EmitSpvInst(sec_fn, SpvOpConvertSToF, {t_f32, f, sext});
                            EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, sc, f, c_f32_inv32767});
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, lo, glsl_ext_id, GlslFMax, sc, c_f32_neg1});
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, cl, glsl_ext_id, GlslFMin, lo, c_f32_1});
                            return pack_f32_out(cl);
                        }
                        if (fmt == 7u || fmt == 19u || fmt == 49u) {  // R16 UINT
                            uint32_t h = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, h, raw_u, c_u32_65535});
                            return h;
                        }
                        if (fmt == 8u || fmt == 20u || fmt == 50u) {  // R16 SINT
                            uint32_t shl = alloc_id();
                            uint32_t shl_i = alloc_id();
                            uint32_t sext = alloc_id();
                            uint32_t su = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, shl, raw_u, c_u32_16});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, shl_i, shl});
                            EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, sext, shl_i, c_u32_16});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, su, sext});
                            if (!d16_out) return su;
                            uint32_t lo = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lo, su, c_u32_65535});
                            return lo;
                        }
                        if (fmt == 9u || fmt == 21u || fmt == 51u) {  // R16 FLOAT
                            uint32_t h = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, h, raw_u, c_u32_65535});
                            if (d16_out) return h;
                            uint32_t v2 = alloc_id();
                            uint32_t f = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, v2, glsl_ext_id, GlslUnpackHalf2x16, h});
                            EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, f, v2, 0u});
                            return pack_f32_out(f);
                        }
                        if (fmt == 14u || fmt == 15u) {  // R32 UINT/SINT
                            if (!d16_out) return raw_u;
                            uint32_t lo = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lo, raw_u, c_u32_65535});
                            return lo;
                        }
                        if (d16_out) {
                            uint32_t f = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, f, raw_u});
                            return pack_f32_out(f);
                        }
                        return raw_u;
                    };

                    const uint32_t base_addr = get_as_u32(inst.operands[0]);
                    auto addr_at = [&](uint32_t off_bytes) -> uint32_t {
                        if (off_bytes == 0u) return base_addr;
                        uint32_t a = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, a, base_addr, get_const_u32(off_bytes)});
                        return a;
                    };
                    if (is_d16) {
                        const uint32_t lo_u = decode_comp(load_unaligned_u32(addr_at(comp_idx * elem_bytes)), true);
                        uint32_t hi_u = c_u32_0;
                        if (has_hi) {
                            hi_u = decode_comp(load_unaligned_u32(addr_at((comp_idx + 1u) * elem_bytes)), true);
                        }
                        uint32_t hi_sh = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, hi_sh, hi_u, c_u32_16});
                        EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, lo_u, hi_sh});
                    } else {
                        const uint32_t u = decode_comp(load_unaligned_u32(addr_at(comp_idx * elem_bytes)), false);
                        if (CanonicalSpvKind(inst.type) == SpvValKind::F32) {
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, u});
                        } else {
                            EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, u, c_u32_0});
                        }
                    }
                    break;
                }
                case IrOp::ImageSample: {
                    uint32_t img = alloc_id();
                    uint32_t samp = alloc_id();
                    uint32_t si = alloc_id();
                    uint32_t uv = alloc_id();
                    uint32_t rgba = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_img2d, img, var_tex0});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_sampler, samp, var_samp0});
                    EmitSpvInst(sec_fn, SpvOpSampledImage, {t_sampled_img2d, si, img, samp});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2f32, uv, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpImageSampleExplicitLod,
                                {t_v4f32, rgba, si, uv, 2u, get_as_f32(inst.operands[2])});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, rgba, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::ImageSampleImplicit: {
                    // Pixel shader helper-lane-aware affine quad derivatives (OpDPdx/OpDPdy + Grad=0x4)
                    uint32_t img = alloc_id();
                    uint32_t samp = alloc_id();
                    uint32_t si = alloc_id();
                    uint32_t uv = alloc_id();
                    uint32_t duv_dx = alloc_id();
                    uint32_t duv_dy = alloc_id();
                    uint32_t rgba = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_img2d, img, var_tex0});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_sampler, samp, var_samp0});
                    EmitSpvInst(sec_fn, SpvOpSampledImage, {t_sampled_img2d, si, img, samp});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2f32, uv, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpDPdx, {t_v2f32, duv_dx, uv});
                    EmitSpvInst(sec_fn, SpvOpDPdy, {t_v2f32, duv_dy, uv});
                    // ImageOperands::Grad = 0x4
                    EmitSpvInst(sec_fn, SpvOpImageSampleExplicitLod,
                                {t_v4f32, rgba, si, uv, 4u, duv_dx, duv_dy});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, rgba, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::ImageLoad: {
                    uint32_t img = alloc_id();
                    uint32_t coord = alloc_id();
                    uint32_t rgba = alloc_id();
                    const bool is_u32 = (inst.type == IrType::U32);
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_roimg2d, img, var_img_ro});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpImageRead, {is_u32 ? t_v4u32 : t_v4f32, rgba, img, coord});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {is_u32 ? t_u32 : t_f32, res, rgba, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::ImageLoadMip: {
                    const uint32_t ch = inst.imm_u32 & 3u;
                    const uint32_t dim = (inst.imm_u32 >> 4) & 7u;
                    const bool is_u32 = (inst.type == IrType::U32);
                    uint32_t img = alloc_id();
                    uint32_t coord = alloc_id();
                    uint32_t rgba = alloc_id();
                    const uint32_t mip_i32 = get_as_i32(inst.operands[3]);
                    if (dim == 2u) {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_img3d, img, var_tex3d});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v3i32, coord, get_as_i32(inst.operands[0]),
                                     get_as_i32(inst.operands[1]), get_as_i32(inst.operands[2])});
                    } else if (dim == 3u) {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_imgcube, img, var_texcube});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v3i32, coord, get_as_i32(inst.operands[0]),
                                     get_as_i32(inst.operands[1]), get_as_i32(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_img2d, img, var_tex0});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    // ImageOperands::Lod = 0x2
                    EmitSpvInst(sec_fn, SpvOpImageFetch, {is_u32 ? t_v4u32 : t_v4f32, rgba, img, coord, 2u, mip_i32});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {is_u32 ? t_u32 : t_f32, res, rgba, ch});
                    break;
                }
                case IrOp::ImageStore: {
                    uint32_t img = alloc_id();
                    uint32_t coord = alloc_id();
                    uint32_t rgba = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_rwimg2d, img, var_rwtex0});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v4f32, rgba, get_as_f32(inst.operands[2]), get_as_f32(inst.operands[3]),
                                 get_as_f32(inst.operands[4]), get_as_f32(inst.operands[5])});
                    EmitSpvInst(sec_fn, SpvOpImageWrite, {img, coord, rgba});
                    break;
                }
                case IrOp::ImageAtomicCmpSwap: {
                    const uint32_t dim = inst.imm_u32 & 7u;
                    uint32_t coord = alloc_id();
                    if (dim == 2u || dim == 3u) {
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v3i32, coord, get_as_i32(inst.operands[0]),
                                     get_as_i32(inst.operands[1]), get_as_i32(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    uint32_t texel_ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpImageTexelPointer,
                                {t_ptr_img_u32, texel_ptr, var_rwimg_u32, coord, c_i32_0});
                    const uint32_t c_scope = get_const_u32(kSpvScopeDevice);
                    const uint32_t c_sem = get_const_u32(kSpvMemSemNone);
                    const uint32_t cmp_val = get_as_u32(inst.operands[3]);
                    const uint32_t src_val = get_as_u32(inst.operands[4]);
                    // OpAtomicCompareExchange: Value (new=src) first, Comparator (cmp) second
                    EmitSpvInst(sec_fn, SpvOpAtomicCompareExchange,
                                {t_u32, res, texel_ptr, c_scope, c_sem, c_sem, src_val, cmp_val});
                    break;
                }
                case IrOp::BvhIntersectRay: {
                    if (last_bvh_vec4_id == 0u || last_bvh_ops != inst.operands) {
                        // Branchless Möller-Trumbore triangle + slab AABB intersection
                        const uint32_t node_ptr = get_as_u32(inst.operands[0]);
                        const uint32_t tmax = get_as_f32(inst.operands[1]);
                        const uint32_t ox = get_as_f32(inst.operands[2]);
                        const uint32_t oy = get_as_f32(inst.operands[3]);
                        const uint32_t oz = get_as_f32(inst.operands[4]);
                        const uint32_t dx = get_as_f32(inst.operands[5]);
                        const uint32_t dy = get_as_f32(inst.operands[6]);
                        const uint32_t dz = get_as_f32(inst.operands[7]);

                        uint32_t base_dw = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, base_dw, node_ptr, c_u32_2});

                        auto load_uav_u32 = [&](uint32_t dw_off) -> uint32_t {
                            uint32_t idx = base_dw;
                            if (dw_off != 0u) {
                                idx = alloc_id();
                                EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, idx, base_dw, get_const_u32(dw_off)});
                            }
                            uint32_t ptr = alloc_id();
                            uint32_t val = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr, var_uav0, c_i32_0, idx});
                            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, val, ptr});
                            return val;
                        };
                        auto load_uav_f32 = [&](uint32_t dw_off) -> uint32_t {
                            uint32_t u = load_uav_u32(dw_off);
                            uint32_t f = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, f, u});
                            return f;
                        };
                        auto fsub = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFSub, {t_f32, r, a, b});
                            return r;
                        };
                        auto fadd = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, r, a, b});
                            return r;
                        };
                        auto fmul = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, r, a, b});
                            return r;
                        };
                        auto fdiv = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFDiv, {t_f32, r, a, b});
                            return r;
                        };
                        auto fmin = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFMin, a, b});
                            return r;
                        };
                        auto fmax = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFMax, a, b});
                            return r;
                        };
                        auto fabs_f = [&](uint32_t a) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFAbs, a});
                            return r;
                        };
                        auto dot3 = [&](uint32_t ax, uint32_t ay, uint32_t az,
                                        uint32_t bx, uint32_t by, uint32_t bz) -> uint32_t {
                            return fadd(fadd(fmul(ax, bx), fmul(ay, by)), fmul(az, bz));
                        };
                        auto cross3 = [&](uint32_t ax, uint32_t ay, uint32_t az,
                                          uint32_t bx, uint32_t by, uint32_t bz,
                                          uint32_t& rx, uint32_t& ry, uint32_t& rz) {
                            rx = fsub(fmul(ay, bz), fmul(az, by));
                            ry = fsub(fmul(az, bx), fmul(ax, bz));
                            rz = fsub(fmul(ax, by), fmul(ay, bx));
                        };
                        auto land = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, r, a, b});
                            return r;
                        };
                        auto fcmp = [&](uint16_t op, uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, op, {t_bool, r, a, b});
                            return r;
                        };
                        auto sel_f = [&](uint32_t cond, uint32_t tv, uint32_t fv) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, r, cond, tv, fv});
                            return r;
                        };
                        auto sel_u = [&](uint32_t cond, uint32_t tv, uint32_t fv) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, r, cond, tv, fv});
                            return r;
                        };

                        const uint32_t hdr = load_uav_u32(0u);
                        const uint32_t c_highbit = get_const_u32(0x80000000u);
                        const uint32_t c_idmask = get_const_u32(0x7FFFFFFFu);
                        uint32_t hdr_flag = alloc_id();
                        uint32_t is_box = alloc_id();
                        uint32_t hit_id = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, hdr_flag, hdr, c_highbit});
                        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, is_box, hdr_flag, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, hit_id, hdr, c_idmask});

                        const uint32_t v0x = load_uav_f32(1u);
                        const uint32_t v0y = load_uav_f32(2u);
                        const uint32_t v0z = load_uav_f32(3u);
                        const uint32_t v1x = load_uav_f32(4u);
                        const uint32_t v1y = load_uav_f32(5u);
                        const uint32_t v1z = load_uav_f32(6u);
                        const uint32_t v2x = load_uav_f32(7u);
                        const uint32_t v2y = load_uav_f32(8u);
                        const uint32_t v2z = load_uav_f32(9u);

                        // Triangle Möller-Trumbore
                        const uint32_t e1x = fsub(v1x, v0x), e1y = fsub(v1y, v0y), e1z = fsub(v1z, v0z);
                        const uint32_t e2x = fsub(v2x, v0x), e2y = fsub(v2y, v0y), e2z = fsub(v2z, v0z);
                        uint32_t px = 0, py = 0, pz = 0;
                        cross3(dx, dy, dz, e2x, e2y, e2z, px, py, pz);
                        const uint32_t det = dot3(e1x, e1y, e1z, px, py, pz);
                        const uint32_t det_ok = fcmp(SpvOpFOrdGreaterThan, fabs_f(det), get_const_f32(1e-7f));
                        const uint32_t inv_det = fdiv(c_f32_1, sel_f(det_ok, det, c_f32_1));
                        const uint32_t tvx = fsub(ox, v0x), tvy = fsub(oy, v0y), tvz = fsub(oz, v0z);
                        const uint32_t u = fmul(dot3(tvx, tvy, tvz, px, py, pz), inv_det);
                        uint32_t qx = 0, qy = 0, qz = 0;
                        cross3(tvx, tvy, tvz, e1x, e1y, e1z, qx, qy, qz);
                        const uint32_t v = fmul(dot3(dx, dy, dz, qx, qy, qz), inv_det);
                        const uint32_t t = fmul(dot3(e2x, e2y, e2z, qx, qy, qz), inv_det);
                        uint32_t tri_hit = det_ok;
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdGreaterThanEqual, u, c_f32_0));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdGreaterThanEqual, v, c_f32_0));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdLessThanEqual, fadd(u, v), c_f32_1));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdGreaterThanEqual, t, c_f32_0));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdLessThanEqual, t, tmax));
                        const uint32_t tri_t = sel_f(tri_hit, t, tmax);
                        const uint32_t tri_u = sel_f(tri_hit, u, c_f32_0);
                        const uint32_t tri_v = sel_f(tri_hit, v, c_f32_0);
                        const uint32_t tri_id = sel_u(tri_hit, hit_id, c_u32_allones);

                        // AABB Slab test (bmin = v0, bmax = v1)
                        const uint32_t c_eps = get_const_f32(1e-12f);
                        const uint32_t c_big = get_const_f32(1e30f);
                        const uint32_t c_nbig = get_const_f32(-1e30f);
                        auto slab_axis = [&](uint32_t ok, uint32_t dk, uint32_t bmin_k, uint32_t bmax_k,
                                             uint32_t& out_tmin, uint32_t& out_tmax) {
                            const uint32_t nz = fcmp(SpvOpFOrdGreaterThan, fabs_f(dk), c_eps);
                            const uint32_t inv_d = fdiv(c_f32_1, sel_f(nz, dk, c_f32_1));
                            const uint32_t t0 = fmul(fsub(bmin_k, ok), inv_d);
                            const uint32_t t1 = fmul(fsub(bmax_k, ok), inv_d);
                            const uint32_t smin = fmin(t0, t1);
                            const uint32_t smax = fmax(t0, t1);
                            const uint32_t inside = land(
                                fcmp(SpvOpFOrdGreaterThanEqual, ok, bmin_k),
                                fcmp(SpvOpFOrdLessThanEqual, ok, bmax_k));
                            out_tmin = sel_f(nz, smin, sel_f(inside, c_nbig, c_big));
                            out_tmax = sel_f(nz, smax, sel_f(inside, c_big, c_nbig));
                        };
                        uint32_t tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0, tz0 = 0, tz1 = 0;
                        slab_axis(ox, dx, v0x, v1x, tx0, tx1);
                        slab_axis(oy, dy, v0y, v1y, ty0, ty1);
                        slab_axis(oz, dz, v0z, v1z, tz0, tz1);
                        const uint32_t t_enter = fmax(fmax(tx0, ty0), fmax(tz0, c_f32_0));
                        const uint32_t t_exit = fmin(fmin(tx1, ty1), fmin(tz1, tmax));
                        const uint32_t box_hit = fcmp(SpvOpFOrdLessThanEqual, t_enter, t_exit);
                        const uint32_t box_t = sel_f(box_hit, t_enter, tmax);
                        const uint32_t box_u = sel_f(box_hit, t_exit, c_f32_0);
                        const uint32_t box_v = c_f32_0;
                        const uint32_t box_id = sel_u(box_hit, hit_id, c_u32_allones);

                        const uint32_t out_t = sel_f(is_box, box_t, tri_t);
                        const uint32_t out_u = sel_f(is_box, box_u, tri_u);
                        const uint32_t out_v = sel_f(is_box, box_v, tri_v);
                        const uint32_t out_id = sel_u(is_box, box_id, tri_id);

                        uint32_t out_t_u = alloc_id();
                        uint32_t out_u_u = alloc_id();
                        uint32_t out_v_u = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, out_t_u, out_t});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, out_u_u, out_u});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, out_v_u, out_v});

                        last_bvh_vec4_id = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v4u32, last_bvh_vec4_id, out_t_u, out_u_u, out_v_u, out_id});
                        last_bvh_ops = inst.operands;
                    }

                    const uint32_t comp = inst.imm_u32 & 3u;
                    if (comp < 3u) {
                        uint32_t ext_u = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_u32, ext_u, last_bvh_vec4_id, comp});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, ext_u});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_u32, res, last_bvh_vec4_id, 3u});
                    }
                    break;
                }
                case IrOp::LdsLoad: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_lds_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr, var_lds, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::LdsStore: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_lds_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr, var_lds, masked_idx});
                    if (inst.exec_pred != kInvalidIrValue) {
                        uint32_t pred = get_as_bool(inst.exec_pred);
                        uint32_t lbl_then = alloc_id();
                        uint32_t lbl_merge = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_merge, 0u});
                        EmitSpvInst(sec_fn, SpvOpBranchConditional, {pred, lbl_then, lbl_merge});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_then});
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_merge});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_merge});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    }
                    break;
                }
                case IrOp::LdsAtomicAdd: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t addend = get_as_u32(inst.operands[1]);
                    if (inst.exec_pred != kInvalidIrValue) {
                        uint32_t pred = get_as_bool(inst.exec_pred);
                        uint32_t eff_add = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, eff_add, pred, addend, c_u32_0});
                        addend = eff_add;
                    }
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    const uint32_t c_scope = get_const_u32(kSpvScopeWorkgroup);
                    const uint32_t c_sem = get_const_u32(kSpvMemSemNone);
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_lds_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr, var_lds, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpAtomicIAdd, {t_u32, res, ptr, c_scope, c_sem, addend});
                    break;
                }
                case IrOp::ScratchLoad: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_scratch_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_priv_u32, ptr, var_scratch, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::ScratchStore: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_scratch_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_priv_u32, ptr, var_scratch, masked_idx});
                    if (inst.exec_pred != kInvalidIrValue) {
                        uint32_t pred = get_as_bool(inst.exec_pred);
                        uint32_t lbl_then = alloc_id();
                        uint32_t lbl_merge = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_merge, 0u});
                        EmitSpvInst(sec_fn, SpvOpBranchConditional, {pred, lbl_then, lbl_merge});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_then});
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_merge});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_merge});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    }
                    break;
                }
                case IrOp::WaveLaneId: {
                    uint32_t raw_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, raw_idx, var_local_idx});
                    const uint32_t mask = (ir.wave_size == 64u) ? c_u32_63 : c_u32_31;
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, raw_idx, mask});
                    break;
                }
                case IrOp::WaveBallotLo:
                case IrOp::WaveBallotHi: {
                    if (ir.wave_size == 32u && inst.op == IrOp::WaveBallotHi) {
                        // Guest Wave32 has no upper 32 lanes (isolated from 64-lane host wave upper half)
                        EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, c_u32_0, c_u32_0});
                        break;
                    }
                    // Race-free, per-guest-wave isolated ballot (works on 32-lane NVIDIA/WARP and 64-lane AMD host waves!)
                    const uint32_t c_scope = get_const_u32(kSpvScopeWorkgroup);
                    const uint32_t c_sem_none = get_const_u32(kSpvMemSemNone);
                    uint32_t gw_id = 0, lane = 0, gw_base = 0;
                    emit_guest_wave_coords(gw_id, lane, gw_base);
                    uint32_t ballot_base = alloc_id();
                    uint32_t ballot_hi_idx = alloc_id();
                    uint32_t ptr_b0 = alloc_id();
                    uint32_t ptr_b1 = alloc_id();
                    uint32_t clr0 = alloc_id();
                    uint32_t clr1 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, ballot_base, gw_id, c_u32_1});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, ballot_hi_idx, ballot_base, c_u32_1});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_b0, var_w64_ballot, ballot_base});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_b1, var_w64_ballot, ballot_hi_idx});

                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpAtomicAnd, {t_u32, clr0, ptr_b0, c_scope, c_sem_none, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpAtomicAnd, {t_u32, clr1, ptr_b1, c_scope, c_sem_none, c_u32_0});
                    emit_wg_barrier();

                    uint32_t word_idx = alloc_id();
                    uint32_t slot_idx = alloc_id();
                    uint32_t bit_idx = alloc_id();
                    uint32_t bit_mask = alloc_id();
                    uint32_t contrib = alloc_id();
                    uint32_t ptr_bw = alloc_id();
                    uint32_t or_old = alloc_id();
                    uint32_t pred = get_as_bool(inst.operands[0]);

                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, word_idx, lane, c_u32_5});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, slot_idx, ballot_base, word_idx});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, bit_idx, lane, c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, bit_mask, c_u32_1, bit_idx});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, contrib, pred, bit_mask, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_bw, var_w64_ballot, slot_idx});
                    EmitSpvInst(sec_fn, SpvOpAtomicOr, {t_u32, or_old, ptr_bw, c_scope, c_sem_none, contrib});

                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpLoad,
                                {t_u32, res, (inst.op == IrOp::WaveBallotHi) ? ptr_b1 : ptr_b0});
                    break;
                }
                case IrOp::WaveActiveCountBits64: {
                    uint32_t c0 = alloc_id();
                    uint32_t c1 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, c0, get_as_u32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, c1, get_as_u32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, c0, c1});
                    break;
                }
                case IrOp::WavePrefixCount64: {
                    uint32_t lane = emit_lane_id();
                    uint32_t mask_in = get_as_u32(inst.operands[0]);
                    uint32_t init_in = get_as_u32(inst.operands[1]);
                    uint32_t bitmask = alloc_id();
                    if (inst.imm_u32 == 0u) {
                        uint32_t lt32 = alloc_id();
                        uint32_t sh = alloc_id();
                        uint32_t p2 = alloc_id();
                        uint32_t lo_m = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, lt32, lane, c_u32_32});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sh, lt32, lane, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, p2, c_u32_1, sh});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, lo_m, p2, c_u32_1});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, bitmask, lt32, lo_m, c_u32_allones});
                    } else {
                        uint32_t ge32 = alloc_id();
                        uint32_t sub32 = alloc_id();
                        uint32_t sh = alloc_id();
                        uint32_t p2 = alloc_id();
                        uint32_t hi_m = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, ge32, lane, c_u32_32});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, sub32, lane, c_u32_32});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sh, ge32, sub32, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, p2, c_u32_1, sh});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, hi_m, p2, c_u32_1});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, bitmask, ge32, hi_m, c_u32_0});
                    }
                    uint32_t masked = alloc_id();
                    uint32_t cnt = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, mask_in, bitmask});
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, cnt, masked});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, cnt, init_in});
                    break;
                }
                case IrOp::WaveReadFirstLane:
                case IrOp::WaveReadLaneAt: {
                    uint32_t val_u32 = get_as_u32(inst.operands[0]);
                    uint32_t gw_id = 0, lane = 0, gw_base = 0;
                    emit_guest_wave_coords(gw_id, lane, gw_base);
                    uint32_t me_raw = alloc_id();
                    uint32_t me_idx = alloc_id();
                    uint32_t ptr_me = alloc_id();
                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, me_raw, gw_base, lane});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, me_idx, me_raw, c_w_lanes_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_me, var_w64_lanes, me_idx});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr_me, val_u32});
                    emit_wg_barrier();

                    const uint32_t lane_mask = (ir.wave_size == 64u) ? c_u32_63 : c_u32_31;
                    uint32_t src_lane = c_u32_0;
                    if (inst.op == IrOp::WaveReadLaneAt) {
                        src_lane = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, src_lane, get_as_u32(inst.operands[1]), lane_mask});
                    }
                    uint32_t src_raw = alloc_id();
                    uint32_t src_idx = alloc_id();
                    uint32_t ptr_src = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, src_raw, gw_base, src_lane});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, src_idx, src_raw, c_w_lanes_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_src, var_w64_lanes, src_idx});
                    if (CanonicalSpvKind(inst.type) == SpvValKind::F32) {
                        uint32_t loaded = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, loaded, ptr_src});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, loaded});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_src});
                    }
                    break;
                }
                case IrOp::WavePermLane16: {
                    uint32_t val_u32 = get_as_u32(inst.operands[0]);
                    uint32_t s1_u32 = get_as_u32(inst.operands[1]);
                    uint32_t s2_u32 = get_as_u32(inst.operands[2]);
                    uint32_t gw_id = 0, lane = 0, gw_base = 0;
                    emit_guest_wave_coords(gw_id, lane, gw_base);
                    uint32_t me_raw = alloc_id();
                    uint32_t me_idx = alloc_id();
                    uint32_t ptr_me = alloc_id();
                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, me_raw, gw_base, lane});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, me_idx, me_raw, c_w_lanes_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_me, var_w64_lanes, me_idx});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr_me, val_u32});
                    emit_wg_barrier();

                    const uint32_t lane_mask = (ir.wave_size == 64u) ? c_u32_63 : c_u32_31;
                    uint32_t row = alloc_id();
                    uint32_t row_base = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, row, lane, c_u32_15});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, row_base, lane, c_u32_not15});
                    if (inst.imm_u32 != 0u) {
                        uint32_t xbase = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseXor, {t_u32, xbase, row_base, c_u32_16});
                        row_base = xbase;
                    }
                    uint32_t is_hi = alloc_id();
                    uint32_t word = alloc_id();
                    uint32_t nib = alloc_id();
                    uint32_t shift = alloc_id();
                    uint32_t shifted = alloc_id();
                    uint32_t sel = alloc_id();
                    uint32_t tgt_lane_raw = alloc_id();
                    uint32_t tgt_lane = alloc_id();
                    uint32_t tgt_raw = alloc_id();
                    uint32_t tgt = alloc_id();
                    uint32_t ptr_tgt = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, is_hi, row, c_u32_8});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, word, is_hi, s2_u32, s1_u32});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, nib, row, c_u32_7});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, shift, nib, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, shifted, word, shift});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sel, shifted, c_u32_15});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, tgt_lane_raw, row_base, sel});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, tgt_lane, tgt_lane_raw, lane_mask});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, tgt_raw, gw_base, tgt_lane});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, tgt, tgt_raw, c_w_lanes_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_tgt, var_w64_lanes, tgt});
                    if (CanonicalSpvKind(inst.type) == SpvValKind::F32) {
                        uint32_t loaded = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, loaded, ptr_tgt});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, loaded});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_tgt});
                    }
                    break;
                }
                case IrOp::SetMeshOutputCounts:
                    if (is_mesh_stage) {
                        EmitSpvInst(sec_fn, SpvOpSetMeshOutputsEXT,
                                    {get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    } else if (is_task_stage && var_dm_x != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_dm_x, get_as_u32(inst.operands[0])});
                        EmitSpvInst(sec_fn, SpvOpStore, {var_dm_y, get_as_u32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpStore, {var_dm_z, c_u32_1});
                    }
                    break;
                case IrOp::DispatchMeshGroups:
                    if (is_task_stage && var_dm_x != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_dm_x, get_as_u32(inst.operands[0])});
                        EmitSpvInst(sec_fn, SpvOpStore, {var_dm_y, get_as_u32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpStore, {var_dm_z, get_as_u32(inst.operands[2])});
                    }
                    break;
                case IrOp::ExportMeshPrim:
                    if (is_mesh_stage && var_mesh_prims != 0u) {
                        uint32_t lidx = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, lidx, var_local_idx});
                        uint32_t i0 = 0, i1 = 0, i2 = 0;
                        if (inst.imm_u32 == 1u) {
                            const uint32_t p = get_as_u32(inst.operands[0]);
                            const uint32_t c_m10 = get_const_u32(0x3FFu);
                            const uint32_t c_sh10 = get_const_u32(10u);
                            const uint32_t c_sh20 = get_const_u32(20u);
                            uint32_t s1 = alloc_id();
                            uint32_t s2 = alloc_id();
                            i0 = alloc_id();
                            i1 = alloc_id();
                            i2 = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, i0, p, c_m10});
                            EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, s1, p, c_sh10});
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, i1, s1, c_m10});
                            EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, s2, p, c_sh20});
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, i2, s2, c_m10});
                        } else {
                            i0 = get_as_u32(inst.operands[0]);
                            i1 = get_as_u32(inst.operands[1]);
                            i2 = get_as_u32(inst.operands[2]);
                        }
                        uint32_t tri = alloc_id();
                        uint32_t ptr_p = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v3u32, tri, i0, i1, i2});
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_out_v3u32, ptr_p, var_mesh_prims, lidx});
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr_p, tri});
                    }
                    break;
                case IrOp::ExportMeshCull:
                    if (is_mesh_stage && var_mesh_cull != 0u) {
                        uint32_t lidx = alloc_id();
                        uint32_t ptr_c = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, lidx, var_local_idx});
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_out_bool, ptr_c, var_mesh_cull, lidx, c_i32_0});
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr_c, get_as_bool(inst.operands[0])});
                    }
                    break;
                case IrOp::FAdd:
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FSub:
                    EmitSpvInst(sec_fn, SpvOpFSub, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FMul:
                    EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FFma: {
                    // Single-rounding fused FMA that also preserves subnormal inputs on FTZ Vulkan hosts
                    const uint32_t fa = get_as_f32(inst.operands[0]);
                    const uint32_t fb = get_as_f32(inst.operands[1]);
                    const uint32_t fc = get_as_f32(inst.operands[2]);

                    auto is_denorm_and_scale = [&](uint32_t fx, uint32_t& out_is_den, uint32_t& out_scaled) {
                        uint32_t bx = alloc_id();
                        uint32_t exp_b = alloc_id();
                        uint32_t mant_b = alloc_id();
                        uint32_t exp_z = alloc_id();
                        uint32_t mant_nz = alloc_id();
                        uint32_t sm_b = alloc_id();
                        uint32_t s_b = alloc_id();
                        uint32_t u_plus = alloc_id();
                        uint32_t u_base = alloc_id();
                        uint32_t f_plus = alloc_id();
                        uint32_t f_base = alloc_id();
                        out_is_den = alloc_id();
                        out_scaled = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, bx, fx});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, exp_b, bx, c_u32_exp_mask});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, mant_b, bx, c_u32_mant_mask});
                        EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, exp_z, exp_b, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, mant_nz, mant_b, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, out_is_den, exp_z, mant_nz});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sm_b, bx, c_u32_sign_mant_mask});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, s_b, bx, c_u32_sign_mask});
                        EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, u_plus, sm_b, c_u32_f32_one_bits});
                        EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, u_base, s_b, c_u32_f32_one_bits});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, f_plus, u_plus});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, f_base, u_base});
                        EmitSpvInst(sec_fn, SpvOpFSub, {t_f32, out_scaled, f_plus, f_base});
                    };

                    uint32_t a_den = 0, a_scaled = 0;
                    uint32_t b_den = 0, b_scaled = 0;
                    is_denorm_and_scale(fa, a_den, a_scaled);
                    is_denorm_and_scale(fb, b_den, b_scaled);

                    uint32_t abs_a = alloc_id();
                    uint32_t abs_b = alloc_id();
                    uint32_t a_ge1 = alloc_id();
                    uint32_t b_ge1 = alloc_id();
                    uint32_t use_a_scale = alloc_id();
                    uint32_t use_b_scale = alloc_id();
                    uint32_t a_down = alloc_id();
                    uint32_t b_down = alloc_id();
                    uint32_t eff_a1 = alloc_id();
                    uint32_t eff_a = alloc_id();
                    uint32_t eff_b1 = alloc_id();
                    uint32_t eff_b = alloc_id();

                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, abs_a, glsl_ext_id, GlslFAbs, fa});
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, abs_b, glsl_ext_id, GlslFAbs, fb});
                    EmitSpvInst(sec_fn, SpvOpFOrdGreaterThanEqual, {t_bool, a_ge1, abs_a, c_f32_1});
                    EmitSpvInst(sec_fn, SpvOpFOrdGreaterThanEqual, {t_bool, b_ge1, abs_b, c_f32_1});
                    EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, use_a_scale, a_den, b_ge1});
                    EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, use_b_scale, b_den, a_ge1});
                    EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, a_down, fa, c_f32_min_norm});
                    EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, b_down, fb, c_f32_min_norm});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, eff_a1, use_a_scale, a_scaled, fa});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, eff_a, use_b_scale, a_down, eff_a1});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, eff_b1, use_a_scale, b_down, fb});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, eff_b, use_b_scale, b_scaled, eff_b1});
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFma, eff_a, eff_b, fc});
                    break;
                }
                case IrOp::FMad: {
                    // Unfused denormal-flushing MAD with NoContraction so drivers never fuse into FMA
                    auto flush_denorm_spv = [&](uint32_t x_f32, uint32_t out_id) -> uint32_t {
                        uint32_t xb = alloc_id();
                        uint32_t exp_b = alloc_id();
                        uint32_t is_den = alloc_id();
                        uint32_t sign_b = alloc_id();
                        uint32_t sel_b = alloc_id();
                        uint32_t dst_f = (out_id != 0u) ? out_id : alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, xb, x_f32});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, exp_b, xb, c_u32_exp_mask});
                        EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, is_den, exp_b, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sign_b, xb, c_u32_sign_mask});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sel_b, is_den, sign_b, xb});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, dst_f, sel_b});
                        return dst_f;
                    };
                    const uint32_t fa = flush_denorm_spv(get_as_f32(inst.operands[0]), 0u);
                    const uint32_t fb = flush_denorm_spv(get_as_f32(inst.operands[1]), 0u);
                    const uint32_t fc = flush_denorm_spv(get_as_f32(inst.operands[2]), 0u);
                    const uint32_t mul_raw = alloc_id();
                    EmitSpvInst(sec_decor, SpvOpDecorate, {mul_raw, kSpvDecNoContraction});
                    EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, mul_raw, fa, fb});
                    const uint32_t prod = flush_denorm_spv(mul_raw, 0u);
                    const uint32_t add_raw = alloc_id();
                    EmitSpvInst(sec_decor, SpvOpDecorate, {add_raw, kSpvDecNoContraction});
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, add_raw, prod, fc});
                    flush_denorm_spv(add_raw, res);
                    break;
                }
                case IrOp::FFma16: {
                    const uint32_t f = inst.imm_u32;
                    auto unpack_f16 = [&](IrValueId v, int idx) -> uint32_t {
                        uint32_t v2 = alloc_id();
                        uint32_t elem = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst,
                                    {t_v2f32, v2, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(v)});
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract,
                                    {t_f32, elem, v2, (((f >> idx) & 1u) != 0u) ? 1u : 0u});
                        if (((f >> (12 + idx)) & 1u) != 0u) {
                            uint32_t ab = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, ab, glsl_ext_id, GlslFAbs, elem});
                            elem = ab;
                        }
                        if (((f >> (8 + idx)) & 1u) != 0u) {
                            uint32_t ng = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, ng, elem});
                            elem = ng;
                        }
                        return elem;
                    };
                    const uint32_t fa = unpack_f16(inst.operands[0], 0);
                    const uint32_t fb = unpack_f16(inst.operands[1], 1);
                    const uint32_t fc = unpack_f16(inst.operands[2], 2);
                    uint32_t r = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFma, fa, fb, fc});
                    if (((f >> 16) & 1u) != 0u) {
                        uint32_t rlo = alloc_id();
                        uint32_t rcl = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, rlo, glsl_ext_id, GlslFMax, r, c_f32_0});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, rcl, glsl_ext_id, GlslFMin, rlo, c_f32_1});
                        r = rcl;
                    }
                    uint32_t pv2 = alloc_id();
                    uint32_t packed = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, pv2, r, c_f32_0});
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, packed, glsl_ext_id, GlslPackHalf2x16, pv2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, packed, c_u32_65535});
                    break;
                }
                case IrOp::FMin:
                case IrOp::FMax: {
                    const uint32_t fa = get_as_f32(inst.operands[0]);
                    const uint32_t fb = get_as_f32(inst.operands[1]);
                    const uint32_t c_abs_mask = get_const_u32(0x7FFFFFFFu);
                    const uint32_t c_inf_bits = get_const_u32(0x7F800000u);
                    const uint32_t c_qnan_bit = get_const_u32(0x00400000u);
                    uint32_t a_bits = alloc_id(), b_bits = alloc_id();
                    uint32_t aa = alloc_id(), ab = alloc_id();
                    uint32_t na = alloc_id(), nb = alloc_id(), both_n = alloc_id();
                    uint32_t qa_u = alloc_id(), qa_f = alloc_id();
                    uint32_t mm = alloc_id(), s1 = alloc_id(), s2 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, a_bits, fa});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, b_bits, fb});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, aa, a_bits, c_abs_mask});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, ab, b_bits, c_abs_mask});
                    EmitSpvInst(sec_fn, SpvOpUGreaterThan, {t_bool, na, aa, c_inf_bits});
                    EmitSpvInst(sec_fn, SpvOpUGreaterThan, {t_bool, nb, ab, c_inf_bits});
                    EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, both_n, na, nb});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, qa_u, a_bits, c_qnan_bit});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, qa_f, qa_u});
                    EmitSpvInst(sec_fn, SpvOpExtInst,
                                {t_f32, mm, glsl_ext_id, (inst.op == IrOp::FMin) ? GlslFMin : GlslFMax, fa, fb});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, s1, nb, fa, mm});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, s2, na, fb, s1});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, res, both_n, qa_f, s2});
                    break;
                }
                case IrOp::FRcp:
                    EmitSpvInst(sec_fn, SpvOpFDiv, {t_f32, res, c_f32_1, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FRsq:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslInverseSqrt, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FSqrt:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslSqrt, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FFloor:
                case IrOp::FCeil: {
                    // IEEE/ISA-exact NaN quieting + denormal floor/ceil
                    const uint32_t fx = get_as_f32(inst.operands[0]);
                    const uint32_t c_abs_mask = get_const_u32(0x7FFFFFFFu);
                    const uint32_t c_inf_bits = get_const_u32(0x7F800000u);
                    const uint32_t c_qnan_bit = get_const_u32(0x00400000u);
                    uint32_t bx = alloc_id(), ax = alloc_id();
                    uint32_t is_nan = alloc_id();
                    uint32_t qx_u = alloc_id(), qx_f = alloc_id();
                    uint32_t exp_b = alloc_id(), mant_b = alloc_id();
                    uint32_t exp_z = alloc_id(), mant_nz = alloc_id(), is_den = alloc_id();
                    uint32_t sign_b = alloc_id(), is_neg = alloc_id();
                    uint32_t den_res = alloc_id(), norm_res = alloc_id(), non_nan = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, bx, fx});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, ax, bx, c_abs_mask});
                    EmitSpvInst(sec_fn, SpvOpUGreaterThan, {t_bool, is_nan, ax, c_inf_bits});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, qx_u, bx, c_qnan_bit});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, qx_f, qx_u});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, exp_b, bx, c_u32_exp_mask});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, mant_b, bx, c_u32_mant_mask});
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, exp_z, exp_b, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, mant_nz, mant_b, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, is_den, exp_z, mant_nz});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sign_b, bx, c_u32_sign_mask});
                    EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, is_neg, sign_b, c_u32_0});
                    if (inst.op == IrOp::FFloor) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, den_res, is_neg, get_const_f32(-1.0f), c_f32_0});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, norm_res, glsl_ext_id, GlslFloor, fx});
                    } else {
                        const uint32_t neg_zero = get_const_f32(-0.0f);
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, den_res, is_neg, neg_zero, c_f32_1});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, norm_res, glsl_ext_id, GlslCeil, fx});
                    }
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, non_nan, is_den, den_res, norm_res});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, res, is_nan, qx_f, non_nan});
                    break;
                }
                case IrOp::FFract:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFract, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FDot2: {
                    uint32_t va = alloc_id();
                    uint32_t vb = alloc_id();
                    uint32_t dp = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, va, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vb, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpDot, {t_f32, dp, va, vb});
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, res, dp, get_as_f32(inst.operands[2])});
                    break;
                }
                case IrOp::IAdd:
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::ISub:
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::IMul:
                    EmitSpvInst(sec_fn, SpvOpIMul, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::IMin:
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_i32, tmp, glsl_ext_id, GlslSMin,
                                                           get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslUMin,
                                                           get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    }
                    break;
                case IrOp::IMax:
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_i32, tmp, glsl_ext_id, GlslSMax,
                                                           get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslUMax,
                                                           get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    }
                    break;
                case IrOp::BitAnd:
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitOr:
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitXor:
                    EmitSpvInst(sec_fn, SpvOpBitwiseXor, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitNot:
                    EmitSpvInst(sec_fn, SpvOpNot, {t_u32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::BitCount32:
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::Shl: {
                    uint32_t masked = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, res, get_as_u32(inst.operands[0]), masked});
                    break;
                }
                case IrOp::Shr: {
                    uint32_t masked = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, get_as_u32(inst.operands[1]), c_u32_31});
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, tmp, get_as_i32(inst.operands[0]), masked});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, res, get_as_u32(inst.operands[0]), masked});
                    }
                    break;
                }
                case IrOp::AlignByte: {
                    // UB-free v_alignbyte_b32 in SPIR-V: every shift amount strictly < 32
                    const uint32_t hi = get_as_u32(inst.operands[0]);
                    const uint32_t lo = get_as_u32(inst.operands[1]);
                    const uint32_t sel = get_as_u32(inst.operands[2]);
                    const uint32_t c_u32_3 = get_const_u32(3u);
                    uint32_t bs = alloc_id();
                    uint32_t rsh = alloc_id();
                    uint32_t lo_p = alloc_id();
                    uint32_t raw_lsh = alloc_id();
                    uint32_t lsh = alloc_id();
                    uint32_t hi_sh = alloc_id();
                    uint32_t nz = alloc_id();
                    uint32_t hi_p = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, bs, sel, c_u32_3});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, rsh, bs, c_u32_3});
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, lo_p, lo, rsh});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, raw_lsh, c_u32_32, rsh});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lsh, raw_lsh, c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, hi_sh, hi, lsh});
                    EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, nz, rsh, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, hi_p, nz, hi_sh, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, lo_p, hi_p});
                    break;
                }
                case IrOp::BfeU32: {
                    uint32_t s0 = get_as_u32(inst.operands[0]);
                    const uint32_t wid_mask = (inst.imm_u32 != 0u) ? get_const_u32(0x7Fu) : c_u32_31;
                    uint32_t off = alloc_id();
                    uint32_t wid = alloc_id();
                    uint32_t sum = alloc_id();
                    uint32_t is_zero = alloc_id();
                    uint32_t lt32 = alloc_id();
                    uint32_t safe_wid = alloc_id();
                    uint32_t shifted = alloc_id();
                    uint32_t p2 = alloc_id();
                    uint32_t mask = alloc_id();
                    uint32_t masked = alloc_id();
                    uint32_t r_nz = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, off, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, wid, get_as_u32(inst.operands[2]), wid_mask});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, sum, off, wid});
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, is_zero, wid, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, lt32, sum, c_u32_32});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, safe_wid, wid, c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, shifted, s0, off});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, p2, c_u32_1, safe_wid});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, mask, p2, c_u32_1});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, shifted, mask});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, r_nz, lt32, masked, shifted});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, res, is_zero, c_u32_0, r_nz});
                    break;
                }
                case IrOp::BfeI32: {
                    uint32_t s0 = get_as_u32(inst.operands[0]);
                    uint32_t s0_i = get_as_i32(inst.operands[0]);
                    const uint32_t wid_mask = (inst.imm_u32 != 0u) ? get_const_u32(0x7Fu) : c_u32_31;
                    uint32_t off = alloc_id();
                    uint32_t wid = alloc_id();
                    uint32_t sum = alloc_id();
                    uint32_t is_zero = alloc_id();
                    uint32_t not_zero = alloc_id();
                    uint32_t lt32 = alloc_id();
                    uint32_t lt32_nz = alloc_id();
                    uint32_t shl_raw = alloc_id();
                    uint32_t shl_safe = alloc_id();
                    uint32_t shr_raw = alloc_id();
                    uint32_t shr_safe = alloc_id();
                    uint32_t tmp_l = alloc_id();
                    uint32_t tmp_li = alloc_id();
                    uint32_t r_lt = alloc_id();
                    uint32_t r_ge = alloc_id();
                    uint32_t r_nz = alloc_id();
                    uint32_t r_fin = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, off, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, wid, get_as_u32(inst.operands[2]), wid_mask});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, sum, off, wid});
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, is_zero, wid, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpLogicalNot, {t_bool, not_zero, is_zero});
                    EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, lt32, sum, c_u32_32});
                    EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, lt32_nz, lt32, not_zero});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, shl_raw, c_u32_32, sum});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, shl_safe, lt32_nz, shl_raw, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, shr_raw, c_u32_32, wid});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, shr_safe, lt32_nz, shr_raw, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, tmp_l, s0, shl_safe});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, tmp_li, tmp_l});
                    EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, r_lt, tmp_li, shr_safe});
                    EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, r_ge, s0_i, off});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_i32, r_nz, lt32, r_lt, r_ge});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_i32, r_fin, is_zero, c_i32_0, r_nz});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, r_fin});
                    break;
                }
                case IrOp::BfiB32: {
                    uint32_t m = get_as_u32(inst.operands[0]);
                    uint32_t ins = get_as_u32(inst.operands[1]);
                    uint32_t base = get_as_u32(inst.operands[2]);
                    uint32_t not_m = alloc_id();
                    uint32_t p1 = alloc_id();
                    uint32_t p2 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpNot, {t_u32, not_m, m});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, p1, m, ins});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, p2, not_m, base});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, p1, p2});
                    break;
                }
                case IrOp::PkAddF16:
                case IrOp::PkMulF16:
                case IrOp::PkFmaF16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const uint8_t neg_lo = static_cast<uint8_t>((f >> 8) & 7u);
                    const uint8_t neg_hi = static_cast<uint8_t>((f >> 12) & 7u);
                    const bool clamp = ((f >> 16) & 1u) != 0u;
                    auto unpack_sel_v2 = [&](IrValueId v, int idx) -> uint32_t {
                        uint32_t raw_v2 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst,
                                    {t_v2f32, raw_v2, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(v)});
                        const bool lo_hi = ((op_sel >> idx) & 1u) != 0u;
                        const bool hi_hi = ((op_sel_hi >> idx) & 1u) != 0u;
                        const bool n_lo = ((neg_lo >> idx) & 1u) != 0u;
                        const bool n_hi = ((neg_hi >> idx) & 1u) != 0u;
                        if (!lo_hi && hi_hi && !n_lo && !n_hi) {
                            return raw_v2;
                        }
                        uint32_t c0 = alloc_id();
                        uint32_t c1 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, c0, raw_v2, lo_hi ? 1u : 0u});
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, c1, raw_v2, hi_hi ? 1u : 0u});
                        if (n_lo) {
                            uint32_t neg0 = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, neg0, c0});
                            c0 = neg0;
                        }
                        if (n_hi) {
                            uint32_t neg1 = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, neg1, c1});
                            c1 = neg1;
                        }
                        uint32_t v2 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v2, c0, c1});
                        return v2;
                    };
                    uint32_t va = unpack_sel_v2(inst.operands[0], 0);
                    uint32_t vb = unpack_sel_v2(inst.operands[1], 1);
                    uint32_t vr = alloc_id();
                    if (inst.op == IrOp::PkFmaF16) {
                        uint32_t vc = unpack_sel_v2(inst.operands[2], 2);
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vr, glsl_ext_id, GlslFma, va, vb, vc});
                    } else if (inst.op == IrOp::PkAddF16) {
                        EmitSpvInst(sec_fn, SpvOpFAdd, {t_v2f32, vr, va, vb});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpFMul, {t_v2f32, vr, va, vb});
                    }
                    if (clamp) {
                        uint32_t v0 = alloc_id();
                        uint32_t v1 = alloc_id();
                        uint32_t vlo = alloc_id();
                        uint32_t vcl = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v0, c_f32_0, c_f32_0});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v1, c_f32_1, c_f32_1});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vlo, glsl_ext_id, GlslFMax, vr, v0});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vcl, glsl_ext_id, GlslFMin, vlo, v1});
                        vr = vcl;
                    }
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslPackHalf2x16, vr});
                    break;
                }
                case IrOp::PkAddU16:
                case IrOp::PkShl16:
                case IrOp::PkShr16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const bool is_shift = (inst.op == IrOp::PkShl16 || inst.op == IrOp::PkShr16);
                    auto extract_u16 = [&](uint32_t uval, int isa_idx, bool hi_out) -> uint32_t {
                        const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> isa_idx) & 1u) != 0u;
                        if (pick_hi) {
                            uint32_t sh = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, sh, uval, c_u32_16});
                            return sh;
                        }
                        uint32_t lo = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lo, uval, c_u32_65535});
                        return lo;
                    };
                    const uint32_t ua = get_as_u32(inst.operands[0]);
                    const uint32_t ub = get_as_u32(inst.operands[1]);
                    const uint32_t a0 = extract_u16(ua, is_shift ? 1 : 0, false);
                    const uint32_t a1 = extract_u16(ua, is_shift ? 1 : 0, true);
                    const uint32_t b0 = extract_u16(ub, is_shift ? 0 : 1, false);
                    const uint32_t b1 = extract_u16(ub, is_shift ? 0 : 1, true);
                    auto eval_u16 = [&](uint32_t x, uint32_t y) -> uint32_t {
                        uint32_t raw = alloc_id();
                        if (inst.op == IrOp::PkAddU16) {
                            EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, raw, x, y});
                        } else {
                            uint32_t sh = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sh, y, c_u32_15});
                            EmitSpvInst(sec_fn,
                                        (inst.op == IrOp::PkShl16) ? SpvOpShiftLeftLogical : SpvOpShiftRightLogical,
                                        {t_u32, raw, x, sh});
                        }
                        uint32_t m = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, m, raw, c_u32_65535});
                        return m;
                    };
                    const uint32_t r0 = eval_u16(a0, b0);
                    const uint32_t r1 = eval_u16(a1, b1);
                    uint32_t r1_hi = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, r1_hi, r1, c_u32_16});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, r0, r1_hi});
                    break;
                }
                case IrOp::I2F:
                    EmitSpvInst(sec_fn, SpvOpConvertSToF, {t_f32, res, get_as_i32(inst.operands[0])});
                    break;
                case IrOp::U2F:
                    EmitSpvInst(sec_fn, SpvOpConvertUToF, {t_f32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::F2I: {
                    uint32_t tmp = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpConvertFToS, {t_i32, tmp, get_as_f32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    break;
                }
                case IrOp::F2U:
                    EmitSpvInst(sec_fn, SpvOpConvertFToU, {t_u32, res, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::CvtPkNormI16F32:
                case IrOp::CvtPkNormU16F32:
                case IrOp::CvtNormI16F16:
                case IrOp::CvtNormU16F16: {
                    const bool is_signed =
                        (inst.op == IrOp::CvtPkNormI16F32 || inst.op == IrOp::CvtNormI16F16);
                    const bool is_packed =
                        (inst.op == IrOp::CvtPkNormI16F32 || inst.op == IrOp::CvtPkNormU16F32);
                    const uint32_t lo_bound = is_signed ? get_const_f32(-1.0f) : c_f32_0;
                    const uint32_t scale = is_signed ? get_const_f32(32767.0f) : get_const_f32(65535.0f);

                    auto emit_norm16 = [&](uint32_t f_id, uint32_t out_id) {
                        uint32_t is_nan = alloc_id();
                        uint32_t ge_one = alloc_id();
                        uint32_t le_lo = alloc_id();
                        uint32_t cl_lo = alloc_id();
                        uint32_t cl = alloc_id();
                        uint32_t scaled = alloc_id();
                        uint32_t rounded = alloc_id();
                        uint32_t safe_f = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpIsNan, {t_bool, is_nan, f_id});
                        EmitSpvInst(sec_fn, SpvOpFOrdGreaterThanEqual, {t_bool, ge_one, f_id, c_f32_1});
                        EmitSpvInst(sec_fn, SpvOpFOrdLessThanEqual, {t_bool, le_lo, f_id, lo_bound});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, cl_lo, le_lo, lo_bound, f_id});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, cl, ge_one, c_f32_1, cl_lo});
                        EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, scaled, cl, scale});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, rounded, glsl_ext_id, GlslRoundEven, scaled});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, safe_f, is_nan, c_f32_0, rounded});
                        uint32_t uv = alloc_id();
                        if (is_signed) {
                            uint32_t iv = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpConvertFToS, {t_i32, iv, safe_f});
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, uv, iv});
                        } else {
                            EmitSpvInst(sec_fn, SpvOpConvertFToU, {t_u32, uv, safe_f});
                        }
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, out_id, uv, c_u32_65535});
                    };

                    if (is_packed) {
                        uint32_t lo16 = alloc_id();
                        uint32_t hi16 = alloc_id();
                        uint32_t hi_sh = alloc_id();
                        emit_norm16(get_as_f32(inst.operands[0]), lo16);
                        emit_norm16(get_as_f32(inst.operands[1]), hi16);
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, hi_sh, hi16, c_u32_16});
                        EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, lo16, hi_sh});
                    } else {
                        uint32_t v2 = alloc_id();
                        uint32_t f_el = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst,
                                    {t_v2f32, v2, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[0])});
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract,
                                    {t_f32, f_el, v2, (inst.imm_u32 & 1u) != 0u ? 1u : 0u});
                        if (((inst.imm_u32 >> 12) & 1u) != 0u) {
                            uint32_t ab = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, ab, glsl_ext_id, GlslFAbs, f_el});
                            f_el = ab;
                        }
                        if (((inst.imm_u32 >> 8) & 1u) != 0u) {
                            uint32_t ng = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, ng, f_el});
                            f_el = ng;
                        }
                        emit_norm16(f_el, res);
                    }
                    break;
                }
                case IrOp::CmpEq:
                case IrOp::CmpNe:
                case IrOp::CmpLt:
                case IrOp::CmpGt:
                case IrOp::CmpGe: {
                    const bool is_flt = (val_kind[inst.operands[0]] == SpvValKind::F32 ||
                                         val_kind[inst.operands[1]] == SpvValKind::F32);
                    if (is_flt) {
                        uint16_t spv_op = SpvOpFOrdEqual;
                        if (inst.op == IrOp::CmpNe) spv_op = SpvOpFOrdNotEqual;
                        else if (inst.op == IrOp::CmpLt) spv_op = SpvOpFOrdLessThan;
                        else if (inst.op == IrOp::CmpGt) spv_op = SpvOpFOrdGreaterThan;
                        else if (inst.op == IrOp::CmpGe) spv_op = SpvOpFOrdGreaterThanEqual;
                        EmitSpvInst(sec_fn, spv_op, {t_bool, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    } else {
                        uint16_t spv_op = SpvOpIEqual;
                        if (inst.op == IrOp::CmpNe) spv_op = SpvOpINotEqual;
                        else if (inst.op == IrOp::CmpLt) spv_op = SpvOpSLessThan;
                        else if (inst.op == IrOp::CmpGt) spv_op = SpvOpSGreaterThan;
                        else if (inst.op == IrOp::CmpGe) spv_op = SpvOpSGreaterThanEqual;
                        EmitSpvInst(sec_fn, spv_op, {t_bool, res, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    break;
                }
                case IrOp::CmpLtU:
                    EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpEqU:
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpGtU:
                    EmitSpvInst(sec_fn, SpvOpUGreaterThan, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpGeU:
                    EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpF16: {
                    const uint32_t imm = inst.imm_u32;
                    const uint32_t pred = imm & 15u;
                    const uint32_t op_sel = (imm >> 8) & 3u;
                    const uint32_t neg_m = (imm >> 12) & 3u;
                    const uint32_t abs_m = (imm >> 16) & 3u;
                    auto unpack_h = [&](IrValueId v, uint32_t bit) -> uint32_t {
                        uint32_t v2 = alloc_id();
                        uint32_t elem = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, v2, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(v)});
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, elem, v2, ((op_sel & bit) != 0u) ? 1u : 0u});
                        if ((abs_m & bit) != 0u) {
                            uint32_t ab = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, ab, glsl_ext_id, GlslFAbs, elem});
                            elem = ab;
                        }
                        if ((neg_m & bit) != 0u) {
                            uint32_t ng = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, ng, elem});
                            elem = ng;
                        }
                        return elem;
                    };
                    const uint32_t fa = unpack_h(inst.operands[0], 1u);
                    const uint32_t fb = unpack_h(inst.operands[1], 2u);
                    switch (pred) {
                        case 0u: EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, res, get_const_bool(false), get_const_bool(false)}); break;
                        case 1u: EmitSpvInst(sec_fn, SpvOpFOrdLessThan, {t_bool, res, fa, fb}); break;
                        case 2u: EmitSpvInst(sec_fn, SpvOpFOrdEqual, {t_bool, res, fa, fb}); break;
                        case 3u: EmitSpvInst(sec_fn, SpvOpFOrdLessThanEqual, {t_bool, res, fa, fb}); break;
                        case 4u: EmitSpvInst(sec_fn, SpvOpFOrdGreaterThan, {t_bool, res, fa, fb}); break;
                        case 5u: EmitSpvInst(sec_fn, SpvOpFOrdNotEqual, {t_bool, res, fa, fb}); break;
                        case 6u: EmitSpvInst(sec_fn, SpvOpFOrdGreaterThanEqual, {t_bool, res, fa, fb}); break;
                        case 7u: {
                            uint32_t na = alloc_id(), nb = alloc_id(), un = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpIsNan, {t_bool, na, fa});
                            EmitSpvInst(sec_fn, SpvOpIsNan, {t_bool, nb, fb});
                            EmitSpvInst(sec_fn, SpvOpLogicalOr, {t_bool, un, na, nb});
                            EmitSpvInst(sec_fn, SpvOpLogicalNot, {t_bool, res, un});
                            break;
                        }
                        case 8u: {
                            uint32_t na = alloc_id(), nb = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpIsNan, {t_bool, na, fa});
                            EmitSpvInst(sec_fn, SpvOpIsNan, {t_bool, nb, fb});
                            EmitSpvInst(sec_fn, SpvOpLogicalOr, {t_bool, res, na, nb});
                            break;
                        }
                        case 9u:  EmitSpvInst(sec_fn, SpvOpFUnordLessThan, {t_bool, res, fa, fb}); break;
                        case 10u: EmitSpvInst(sec_fn, SpvOpFUnordEqual, {t_bool, res, fa, fb}); break;
                        case 11u: EmitSpvInst(sec_fn, SpvOpFUnordLessThanEqual, {t_bool, res, fa, fb}); break;
                        case 12u: EmitSpvInst(sec_fn, SpvOpFUnordGreaterThan, {t_bool, res, fa, fb}); break;
                        case 13u: EmitSpvInst(sec_fn, SpvOpFUnordNotEqual, {t_bool, res, fa, fb}); break;
                        case 14u: EmitSpvInst(sec_fn, SpvOpFUnordGreaterThanEqual, {t_bool, res, fa, fb}); break;
                        case 15u:
                        default:  EmitSpvInst(sec_fn, SpvOpLogicalOr, {t_bool, res, get_const_bool(true), get_const_bool(true)}); break;
                    }
                    break;
                }
                case IrOp::LogicalAnd:
                    EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, res, get_as_bool(inst.operands[0]), get_as_bool(inst.operands[1])});
                    break;
                case IrOp::LogicalNot:
                    EmitSpvInst(sec_fn, SpvOpLogicalNot, {t_bool, res, get_as_bool(inst.operands[0])});
                    break;
                case IrOp::Select: {
                    const SpvValKind k = CanonicalSpvKind(inst.type);
                    const uint32_t c = get_as_bool(inst.operands[0]);
                    if (k == SpvValKind::F32) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, res, c, get_as_f32(inst.operands[1]), get_as_f32(inst.operands[2])});
                    } else if (k == SpvValKind::Bool) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_bool, res, c, get_as_bool(inst.operands[1]), get_as_bool(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, res, c, get_as_u32(inst.operands[1]), get_as_u32(inst.operands[2])});
                    }
                    break;
                }
                case IrOp::Barrier:
                    if (ir.stage == ShaderStage::Compute || ir.stage == ShaderStage::Mesh ||
                        ir.stage == ShaderStage::Amplification) {
                        emit_wg_barrier();
                    }
                    break;
                case IrOp::Export: {
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Null) &&
                        inst.imm_u32 == kIrKillExportMagicLifter &&
                        ir.stage == ShaderStage::Pixel) {
                        const uint32_t kill_cond = inst.operands.empty()
                                                       ? get_const_bool(true)
                                                       : get_as_bool(inst.operands[0]);
                        const uint32_t lbl_kill = alloc_id();
                        const uint32_t lbl_after = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_after, 0u});
                        EmitSpvInst(sec_fn, SpvOpBranchConditional, {kill_cond, lbl_kill, lbl_after});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_kill});
                        EmitSpvInst(sec_fn, SpvOpKill, {});
                        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_after});
                        break;
                    }
                    if ((inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos2) ||
                         inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos3)) &&
                        cull_dist_count > 0u && var_pervertex != 0u && t_ptr_out_f32 != 0u) {
                        const uint32_t base_c =
                            (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos3)) ? 4u : 0u;
                        uint32_t lidx = 0u;
                        if (is_mesh_stage) {
                            lidx = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, lidx, var_local_idx});
                        }
                        const uint32_t c_i32_1 = get_const_i32(1);
                        for (uint32_t c = 0u; c < 4u && (base_c + c) < cull_dist_count; ++c) {
                            const uint32_t fv = get_as_f32(inst.operands[c]);
                            const uint32_t ptr_cd = alloc_id();
                            if (is_mesh_stage) {
                                EmitSpvInst(sec_fn, SpvOpAccessChain,
                                            {t_ptr_out_f32, ptr_cd, var_pervertex, lidx, c_i32_1,
                                             get_const_u32(base_c + c)});
                            } else {
                                EmitSpvInst(sec_fn, SpvOpAccessChain,
                                            {t_ptr_out_f32, ptr_cd, var_pervertex, c_i32_1,
                                             get_const_u32(base_c + c)});
                            }
                            EmitSpvInst(sec_fn, SpvOpStore, {ptr_cd, fv});
                        }
                        break;
                    }
                    uint32_t vec_id = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {
                        t_v4f32, vec_id,
                        get_as_f32(inst.operands[0]),
                        get_as_f32(inst.operands[1]),
                        get_as_f32(inst.operands[2]),
                        get_as_f32(inst.operands[3]),
                    });
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && var_pervertex != 0u) {
                        uint32_t ptr_pos = alloc_id();
                        if (is_mesh_stage) {
                            uint32_t lidx = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, lidx, var_local_idx});
                            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_out_v4f32, ptr_pos, var_pervertex, lidx, c_i32_0});
                        } else {
                            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_out_v4f32, ptr_pos, var_pervertex, c_i32_0});
                        }
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr_pos, vec_id});
                    } else if (inst.export_target >= static_cast<uint8_t>(Rdna2ExportTarget::Param0) &&
                               inst.export_target <= static_cast<uint8_t>(Rdna2ExportTarget::Param3)) {
                        const uint32_t pslot = static_cast<uint32_t>(
                            inst.export_target - static_cast<uint8_t>(Rdna2ExportTarget::Param0));
                        const uint32_t vout = var_param_out[pslot];
                        if (vout != 0u) {
                            if (is_mesh_stage) {
                                uint32_t lidx = alloc_id();
                                uint32_t ptr_p = alloc_id();
                                EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, lidx, var_local_idx});
                                EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_out_v4f32, ptr_p, vout, lidx});
                                EmitSpvInst(sec_fn, SpvOpStore, {ptr_p, vec_id});
                            } else {
                                EmitSpvInst(sec_fn, SpvOpStore, {vout, vec_id});
                            }
                        }
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) && var_mrt0 != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_mrt0, vec_id});
                    }
                    break;
                }
                default:
                    break;
            }

            if (use_spv_state_machine && inst.id != kInvalidIrValue && res != 0u) {
                auto it_v = val_fn_var.find(inst.id);
                if (it_v != val_fn_var.end()) {
                    EmitSpvInst(sec_fn, SpvOpStore, {it_v->second, res});
                }
            }
        }

        if (use_spv_state_machine) {
            if (!bb.instructions.empty()) {
                const auto& term = bb.instructions.back();
                if (term.op == IrOp::Branch) {
                    emit_sm_phi_copies(static_cast<uint32_t>(bi), term.true_block);
                    EmitSpvInst(sec_fn, SpvOpStore, {var_cfg_state, get_const_u32(term.true_block)});
                    EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_sw_merge});
                } else if (term.op == IrOp::CondBranch) {
                    const uint32_t cond = get_as_bool(term.operands[0]);
                    const uint32_t lbl_t = alloc_id();
                    const uint32_t lbl_f = alloc_id();
                    const uint32_t lbl_m = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_m, 0u});
                    EmitSpvInst(sec_fn, SpvOpBranchConditional, {cond, lbl_t, lbl_f});
                    EmitSpvInst(sec_fn, SpvOpLabel, {lbl_t});
                    emit_sm_phi_copies(static_cast<uint32_t>(bi), term.true_block);
                    EmitSpvInst(sec_fn, SpvOpStore, {var_cfg_state, get_const_u32(term.true_block)});
                    EmitSpvInst(sec_fn, SpvOpBranch, {lbl_m});
                    EmitSpvInst(sec_fn, SpvOpLabel, {lbl_f});
                    emit_sm_phi_copies(static_cast<uint32_t>(bi), term.false_block);
                    EmitSpvInst(sec_fn, SpvOpStore, {var_cfg_state, get_const_u32(term.false_block)});
                    EmitSpvInst(sec_fn, SpvOpBranch, {lbl_m});
                    EmitSpvInst(sec_fn, SpvOpLabel, {lbl_m});
                    EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_sw_merge});
                } else {
                    EmitSpvInst(sec_fn, SpvOpStore, {var_cfg_state, c_u32_allones});
                    EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_sw_merge});
                }
            } else {
                EmitSpvInst(sec_fn, SpvOpStore, {var_cfg_state, c_u32_allones});
                EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_sw_merge});
            }
            continue;
        }

        // Phi edge coercions right before terminator (DAG mode)
        for (const auto& fx : block_exit_fixups[bi]) {
            if (fx.expected_kind == SpvValKind::F32) {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, fx.coerced_spv_id, u});
            } else if (fx.expected_kind == SpvValKind::Bool) {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, fx.coerced_spv_id, u, c_u32_0});
            } else {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, fx.coerced_spv_id, u});
            }
        }

        // Terminator (with mandatory OpSelectionMerge for Vulkan structured CFG)
        if (!bb.instructions.empty()) {
            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Branch) {
                EmitSpvInst(sec_fn, SpvOpBranch, {bb_labels[term.true_block]});
            } else if (term.op == IrOp::CondBranch) {
                if (term.true_block == term.false_block) {
                    EmitSpvInst(sec_fn, SpvOpBranch, {bb_labels[term.true_block]});
                } else {
                    const uint32_t merge_bb = ipdom[bi];
                    const uint32_t merge_lbl = (merge_bb < ir.blocks.size()) ? bb_labels[merge_bb] : epilogue_label;
                    const uint32_t cond = get_as_bool(term.operands[0]);
                    EmitSpvInst(sec_fn, SpvOpSelectionMerge, {merge_lbl, 0u});
                    EmitSpvInst(sec_fn, SpvOpBranchConditional,
                                {cond, bb_labels[term.true_block], bb_labels[term.false_block]});
                }
            } else {
                if (need_epilogue) {
                    EmitSpvInst(sec_fn, SpvOpBranch, {epilogue_label});
                } else {
                    EmitSpvInst(sec_fn, SpvOpReturn, {});
                }
            }
        } else {
            if (need_epilogue) {
                EmitSpvInst(sec_fn, SpvOpBranch, {epilogue_label});
            } else {
                EmitSpvInst(sec_fn, SpvOpReturn, {});
            }
        }
    }

    if (use_spv_state_machine) {
        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_default});
        EmitSpvInst(sec_fn, SpvOpStore, {var_cfg_state, c_u32_allones});
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_sw_merge});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_sw_merge});
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_cont});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_cont});
        EmitSpvInst(sec_fn, SpvOpBranch, {lbl_sm_hdr});

        EmitSpvInst(sec_fn, SpvOpLabel, {lbl_sm_exit});
        if (need_epilogue) {
            EmitSpvInst(sec_fn, SpvOpBranch, {epilogue_label});
        } else {
            EmitSpvInst(sec_fn, SpvOpReturn, {});
        }
    }

    if (need_epilogue) {
        EmitSpvInst(sec_fn, SpvOpLabel, {epilogue_label});
        if (is_task_stage) {
            // Synchronize workgroup writes, commit finalized dispatch counts + LDS payload to
            // TaskPayloadWorkgroupEXT on lane 0, barrier, then broadcast uniform counts into
            // OpEmitMeshTasksEXT as the TaskEXT function terminator.
            emit_wg_barrier();
            const uint32_t gx = alloc_id();
            const uint32_t gy = alloc_id();
            const uint32_t gz = alloc_id();
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, gx, var_dm_x});
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, gy, var_dm_y});
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, gz, var_dm_z});

            const uint32_t ep_lidx = alloc_id();
            const uint32_t ep_is_l0 = alloc_id();
            const uint32_t lbl_tp_wr = alloc_id();
            const uint32_t lbl_tp_emit = alloc_id();
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, ep_lidx, var_local_idx});
            EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, ep_is_l0, ep_lidx, c_u32_0});
            EmitSpvInst(sec_fn, SpvOpSelectionMerge, {lbl_tp_emit, 0u});
            EmitSpvInst(sec_fn, SpvOpBranchConditional, {ep_is_l0, lbl_tp_wr, lbl_tp_emit});

            EmitSpvInst(sec_fn, SpvOpLabel, {lbl_tp_wr});
            {
                const uint32_t p_dx = alloc_id();
                const uint32_t p_dy = alloc_id();
                const uint32_t p_dz = alloc_id();
                EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_dx, var_task_payload, get_const_i32(1)});
                EmitSpvInst(sec_fn, SpvOpStore, {p_dx, gx});
                EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_dy, var_task_payload, get_const_i32(2)});
                EmitSpvInst(sec_fn, SpvOpStore, {p_dy, gy});
                EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_dz, var_task_payload, get_const_i32(3)});
                EmitSpvInst(sec_fn, SpvOpStore, {p_dz, gz});

                const uint32_t fallback_vals[4] = {gx, gy, gz, c_u32_0};
                for (uint32_t k = 0u; k < 4u; ++k) {
                    uint32_t val = fallback_vals[k];
                    if (ir.uses_lds && var_lds != 0u) {
                        const uint32_t lds_slot = std::min(k, lds_size_dwords - 1u);
                        const uint32_t p_lds = alloc_id();
                        val = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, p_lds, var_lds, get_const_u32(lds_slot)});
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, val, p_lds});
                    }
                    const uint32_t p_d = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain,
                                {t_ptr_tp_u32, p_d, var_task_payload, c_i32_0, get_const_u32(k)});
                    EmitSpvInst(sec_fn, SpvOpStore, {p_d, val});
                }
            }
            EmitSpvInst(sec_fn, SpvOpBranch, {lbl_tp_emit});

            EmitSpvInst(sec_fn, SpvOpLabel, {lbl_tp_emit});
            emit_wg_barrier();
            const uint32_t p_ux = alloc_id();
            const uint32_t p_uy = alloc_id();
            const uint32_t p_uz = alloc_id();
            const uint32_t u_gx = alloc_id();
            const uint32_t u_gy = alloc_id();
            const uint32_t u_gz = alloc_id();
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_ux, var_task_payload, get_const_i32(1)});
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, u_gx, p_ux});
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_uy, var_task_payload, get_const_i32(2)});
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, u_gy, p_uy});
            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_tp_u32, p_uz, var_task_payload, get_const_i32(3)});
            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, u_gz, p_uz});
            EmitSpvInst(sec_fn, SpvOpEmitMeshTasksEXT, {u_gx, u_gy, u_gz, var_task_payload});
        } else {
            EmitSpvInst(sec_fn, SpvOpReturn, {});
        }
    }

    EmitSpvInst(sec_fn, SpvOpFunctionEnd, {});

    // 7. Assemble final SPIR-V 1.3 / 1.4 / 1.6 binary stream
    const size_t total_words = 5u + sec_caps.size() + sec_exts.size() + sec_ext.size() + sec_mem.size() +
                               sec_entry.size() + sec_exec.size() + sec_decor.size() +
                               sec_types.size() + sec_globals.size() + sec_fn.size();
    out_spirv.reserve(total_words);
    out_spirv.push_back(kSpirvMagic);
    const uint32_t spv_version = is_task_stage ? kSpirvVersion16
                                               : (is_mesh_stage ? kSpirvVersion14 : kSpirvVersion13);
    out_spirv.push_back(spv_version);
    out_spirv.push_back(kSpirvGeneratorOpenScarlett);
    out_spirv.push_back(next_id);
    out_spirv.push_back(0u);

    auto append_sec = [&](const std::vector<uint32_t>& s) {
        out_spirv.insert(out_spirv.end(), s.begin(), s.end());
    };
    append_sec(sec_caps);
    append_sec(sec_exts);
    append_sec(sec_ext);
    append_sec(sec_mem);
    append_sec(sec_entry);
    append_sec(sec_exec);
    append_sec(sec_decor);
    append_sec(sec_types);
    append_sec(sec_globals);
    append_sec(sec_fn);

    CallTelemetry::Global().Record("rdna2_lifter", "EmitSpirv", CallOutcome::Ok, 0);
    return Status::Ok();
}

Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv) {
    if (spirv.size() < 5u) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary shorter than 5-word header");
    }
    if (spirv[0] != kSpirvMagic) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V magic number");
    }
    if (spirv[1] != kSpirvVersion13 && spirv[1] != kSpirvVersion14 && spirv[1] != kSpirvVersion16) {
        return Status::Error(StatusCode::ShaderError, "Unexpected SPIR-V version (expected 1.3, 1.4, or 1.6)");
    }
    const uint32_t bound = spirv[3];
    if (bound < 2u) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V ID bound");
    }
    if (spirv[4] != 0u) {
        return Status::Error(StatusCode::ShaderError, "Non-zero SPIR-V reserved schema word");
    }

    bool saw_capability = false;
    bool saw_memory_model = false;
    bool saw_entry_point = false;
    bool saw_function = false;
    bool saw_return = false;
    bool saw_function_end = false;
    int fn_depth = 0;
    bool allow_phi = false;

    size_t pos = 5u;
    while (pos < spirv.size()) {
        const uint32_t header = spirv[pos];
        const uint16_t word_count = static_cast<uint16_t>((header >> 16) & 0xFFFFu);
        const uint16_t opcode = static_cast<uint16_t>(header & 0xFFFFu);
        if (word_count == 0u || pos + word_count > spirv.size()) {
            return Status::Error(StatusCode::ShaderError, "Truncated or zero-length SPIR-V instruction");
        }

        if (opcode != SpvOpPhi && opcode != SpvOpLabel) {
            allow_phi = false;
        }

        switch (opcode) {
            case SpvOpCapability:
                if (word_count != 2u) return Status::Error(StatusCode::ShaderError, "Malformed OpCapability");
                saw_capability = true;
                break;
            case SpvOpMemoryModel:
                if (word_count != 3u) return Status::Error(StatusCode::ShaderError, "Malformed OpMemoryModel");
                saw_memory_model = true;
                break;
            case SpvOpEntryPoint:
                if (word_count < 4u || spirv[pos + 2] == 0u || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpEntryPoint");
                }
                saw_entry_point = true;
                break;
            case SpvOpDecorate:
                if (word_count < 3u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpDecorate");
                }
                break;
            case SpvOpMemberDecorate:
                if (word_count < 4u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpMemberDecorate");
                }
                break;
            case SpvOpFunction:
                if (word_count != 5u || spirv[pos + 1] >= bound || spirv[pos + 2] >= bound || spirv[pos + 4] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpFunction");
                }
                saw_function = true;
                ++fn_depth;
                break;
            case SpvOpFunctionEnd:
                if (word_count != 1u || fn_depth <= 0) {
                    return Status::Error(StatusCode::ShaderError, "Unbalanced OpFunctionEnd");
                }
                --fn_depth;
                saw_function_end = true;
                break;
            case SpvOpLabel:
                if (word_count != 2u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpLabel");
                }
                allow_phi = true;
                break;
            case SpvOpPhi:
                if (!allow_phi || word_count < 5u || ((word_count - 3u) & 1u) != 0u ||
                    spirv[pos + 1] >= bound || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed or misplaced OpPhi");
                }
                for (uint16_t k = 3u; k < word_count; ++k) {
                    if (spirv[pos + k] == 0u || spirv[pos + k] >= bound) {
                        return Status::Error(StatusCode::ShaderError, "OpPhi operand ID out of bound");
                    }
                }
                break;
            case SpvOpBranch:
                if (word_count != 2u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpBranch");
                }
                break;
            case SpvOpBranchConditional:
                if (word_count < 4u || spirv[pos + 1] >= bound || spirv[pos + 2] >= bound || spirv[pos + 3] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpBranchConditional");
                }
                break;
            case SpvOpSwitch:
                if (word_count < 3u || ((word_count - 3u) & 1u) != 0u ||
                    spirv[pos + 1] == 0u || spirv[pos + 1] >= bound ||
                    spirv[pos + 2] == 0u || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpSwitch");
                }
                for (uint16_t k = 4u; k < word_count; k = static_cast<uint16_t>(k + 2u)) {
                    if (spirv[pos + k] == 0u || spirv[pos + k] >= bound) {
                        return Status::Error(StatusCode::ShaderError, "OpSwitch target label ID out of bound");
                    }
                }
                break;
            case SpvOpKill:
                if (word_count != 1u) return Status::Error(StatusCode::ShaderError, "Malformed OpKill");
                break;
            case SpvOpReturn:
                if (word_count != 1u) return Status::Error(StatusCode::ShaderError, "Malformed OpReturn");
                saw_return = true;
                break;
            case SpvOpEmitMeshTasksEXT:
                if ((word_count != 4u && word_count != 5u) ||
                    spirv[pos + 1] == 0u || spirv[pos + 1] >= bound ||
                    spirv[pos + 2] == 0u || spirv[pos + 2] >= bound ||
                    spirv[pos + 3] == 0u || spirv[pos + 3] >= bound ||
                    (word_count == 5u && (spirv[pos + 4] == 0u || spirv[pos + 4] >= bound))) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpEmitMeshTasksEXT");
                }
                saw_return = true;
                break;
            default:
                break;
        }
        pos += word_count;
    }

    if (!saw_capability || !saw_memory_model || !saw_entry_point ||
        !saw_function || !saw_return || !saw_function_end || fn_depth != 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V module missing required sections or unbalanced function");
    }
    return Status::Ok();
}

Status ValidateSpirvExternal(
    const std::vector<uint32_t>& spirv,
    std::string& out_diagnostics,
    const std::string& spirv_val_path,
    const std::string& target_env) {
    out_diagnostics.clear();
    if (auto st = ValidateSpirvBinary(spirv); !st.ok()) {
        out_diagnostics = st.message;
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -1);
        return st;
    }
#if defined(_WIN32)
    if (!std::filesystem::exists(spirv_val_path)) {
        out_diagnostics = "spirv-val.exe not found at: " + spirv_val_path;
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -2);
        return Status::Error(StatusCode::NotFound, out_diagnostics);
    }

    const std::string spv_path = MakeUniqueTempPath("spv", ".spv");
    {
        std::ofstream ofs(spv_path, std::ios::binary);
        if (!ofs) {
            out_diagnostics = "Failed to open temp SPIR-V file: " + spv_path;
            CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -3);
            return Status::Error(StatusCode::IoError, out_diagnostics);
        }
        ofs.write(reinterpret_cast<const char*>(spirv.data()),
                  static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
    }

    const std::string cmd = "\"" + spirv_val_path + "\" --target-env " + target_env + " \"" + spv_path + "\"";
    uint32_t exit_code = 1;
    Status run_st = RunExternalProcess(cmd, exit_code, out_diagnostics);

    std::error_code ec;
    std::filesystem::remove(spv_path, ec);

    if (!run_st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -4);
        return run_st;
    }
    if (exit_code != 0u) {
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error,
                                       static_cast<int32_t>(exit_code));
        return Status::Error(StatusCode::ShaderError, "spirv-val failed: " + out_diagnostics);
    }

    CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Ok, 0);
    return Status::Ok();
#else
    (void)spirv_val_path;
    (void)target_env;
    return Status::Error(StatusCode::NotImplemented, "ValidateSpirvExternal requires Windows");
#endif
}

// ---------------------------------------------------------------------------
// HLSL Emission & External DXC Compilation
// ---------------------------------------------------------------------------
Status EmitHlsl(const Rdna2IrModule& ir, std::string& out_hlsl, std::string& out_target, bool use_sm65) {
    HlslEmitter emitter(ir, use_sm65);
    Status st = emitter.Emit(out_hlsl, out_target);
    CallTelemetry::Global().Record("rdna2_lifter", "EmitHlsl", st.ok() ? CallOutcome::Ok : CallOutcome::Error,
                                   st.ok() ? 0 : -1);
    return st;
}

Status CompileHlslWithDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path) {
    out_dxil.clear();
    out_diagnostics.clear();
    if (hlsl.empty() || target.empty() || entry_point.empty()) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -1);
        return Status::Error(StatusCode::InvalidArgument, "CompileHlslWithDxc requires non-empty HLSL/target/entry");
    }
#if defined(_WIN32)
    const std::string resolved_dxc = ResolvePortableDxcPath(dxc_path);
    if (!std::filesystem::exists(resolved_dxc)) {
        out_diagnostics = "dxc.exe not found at: " + resolved_dxc;
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -2);
        return Status::Error(StatusCode::NotFound, out_diagnostics);
    }

    const std::string hlsl_path = MakeUniqueTempPath("hlsl", ".hlsl");
    const std::string dxil_path = MakeUniqueTempPath("dxil", ".dxil");

    {
        std::ofstream ofs(hlsl_path, std::ios::binary);
        if (!ofs) {
            out_diagnostics = "Failed to open temp HLSL file: " + hlsl_path;
            CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -3);
            return Status::Error(StatusCode::IoError, out_diagnostics);
        }
        ofs.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
    }

    const std::string cmd = "\"" + resolved_dxc + "\" -T " + target + " -E " + entry_point +
                            " -Fo \"" + dxil_path + "\" \"" + hlsl_path + "\"";
    uint32_t exit_code = 1;
    Status run_st = RunExternalProcess(cmd, exit_code, out_diagnostics);

    std::error_code ec;
    if (run_st.ok() && exit_code == 0u) {
        std::ifstream ifs(dxil_path, std::ios::binary | std::ios::ate);
        if (ifs) {
            const std::streamsize sz = ifs.tellg();
            if (sz > 0) {
                out_dxil.resize(static_cast<size_t>(sz));
                ifs.seekg(0, std::ios::beg);
                ifs.read(reinterpret_cast<char*>(out_dxil.data()), sz);
            }
        }
    }

    std::filesystem::remove(hlsl_path, ec);
    std::filesystem::remove(dxil_path, ec);

    if (!run_st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -4);
        return run_st;
    }
    if (exit_code != 0u) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error,
                                       static_cast<int32_t>(exit_code));
        return Status::Error(StatusCode::ShaderError, "dxc.exe failed: " + out_diagnostics);
    }
    if (out_dxil.size() < 32u || std::memcmp(out_dxil.data(), "DXBC", 4) != 0) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -5);
        return Status::Error(StatusCode::ShaderError, "dxc.exe produced invalid or empty DXIL container");
    }

    CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Ok, 0);
    return Status::Ok();
#else
    (void)dxc_path;
    return Status::Error(StatusCode::NotImplemented, "CompileHlslWithDxc requires Windows");
#endif
}

std::string ResolvePortableDxcPath(const std::string& requested_path) {
#if defined(_WIN32)
    return ResolveDxcExecutable(requested_path);
#else
    return requested_path;
#endif
}

Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options) {
    out_result = Rdna2LiftResult{};

    XboxParsedShaderContainer full_info{};
    if (auto st = ParseFullXboxShaderContainer(xs2_bytes, full_info); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -1);
        return st;
    }
    if (!full_info.has_xs2) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -1);
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Shader container does not carry an Xbox XS2/RDNA2 payload");
    }
    out_result.container = full_info.xs2;

    if (auto st = DisassembleRdna2Extended(out_result.container.code_words, out_result.instructions); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -2);
        return st;
    }
    Rdna2LiftOptions eff_opts = options;
    if (eff_opts.stage_contract.pixel_inputs.empty() && eff_opts.stage_contract.vertex_fetches.empty()) {
        ApplyXboxContainerToLiftOptions(full_info, eff_opts);
    }
    const StageContractTable* contract_ptr =
        (!eff_opts.stage_contract.vertex_fetches.empty() ||
         !eff_opts.stage_contract.pixel_inputs.empty() ||
         eff_opts.stage_contract.texture_srv_format != Rdna2TextureFormat::Float4)
            ? &eff_opts.stage_contract
            : nullptr;
    if (auto st = BuildRdna2Ir(out_result.container, out_result.instructions, out_result.ir, contract_ptr); !st.ok()) {
        out_result.dxc_diagnostics = st.message;
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -3);
        return st;
    }
    out_result.ir.mesh_has_payload = options.mesh_has_payload;

    if (auto st = OptimizeRdna2Ir(out_result.ir); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -4);
        return st;
    }
    out_result.inserted_lockstep_lds_barriers = out_result.ir.inserted_lockstep_lds_barriers;

    const bool stage_supports_spirv =
        (out_result.ir.stage == ShaderStage::Compute ||
         out_result.ir.stage == ShaderStage::Vertex ||
         out_result.ir.stage == ShaderStage::Pixel ||
         out_result.ir.stage == ShaderStage::Amplification ||
         (out_result.ir.stage == ShaderStage::Mesh && options.emit_mesh_spirv));

    if (options.emit_spirv && stage_supports_spirv) {
        if (auto st = EmitSpirv(out_result.ir, out_result.spirv_words); !st.ok()) {
            CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -5);
            return st;
        }
        if (auto st = ValidateSpirvBinary(out_result.spirv_words); !st.ok()) {
            CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -6);
            return st;
        }
        if (options.validate_spirv_external) {
            const std::string env =
                ((out_result.ir.stage == ShaderStage::Mesh ||
                  out_result.ir.stage == ShaderStage::Amplification) &&
                 options.spirv_target_env == "vulkan1.2")
                    ? "vulkan1.3"
                    : options.spirv_target_env;
            if (auto st = ValidateSpirvExternal(out_result.spirv_words, out_result.spirv_val_diagnostics,
                                                options.spirv_val_path, env);
                !st.ok()) {
                CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -7);
                return st;
            }
        }
    }

    if (options.emit_hlsl) {
        if (auto st = EmitHlsl(out_result.ir, out_result.hlsl_source, out_result.hlsl_target, options.use_sm65);
            !st.ok()) {
            CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -8);
            return st;
        }
        if (!options.target_profile_override.empty()) {
            out_result.hlsl_target = options.target_profile_override;
        }
        if (options.compile_dxil_with_dxc) {
            if (auto st = CompileHlslWithDxc(out_result.hlsl_source, out_result.hlsl_target, "main",
                                             out_result.pc_dxil, out_result.dxc_diagnostics, options.dxc_path);
                !st.ok()) {
                CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -9);
                return st;
            }
            if (options.resign_dxil && !VerifyDxbcContainerHash(out_result.pc_dxil.data(), out_result.pc_dxil.size())) {
                if (auto rs = ResignDxilContainer(out_result.pc_dxil); !rs.ok()) {
                    CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -10);
                    return rs;
                }
            }
            out_result.pc_dxil_hash = HashBytesFnv1a(out_result.pc_dxil.data(), out_result.pc_dxil.size());
        }
    }

    CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Ok, 0);
    return Status::Ok();
}

}  // namespace openscarlett
