#pragma once

// OpenScarlett - MIT License
// GFX10.3 (RDNA2) 779-opcode ISA coverage auditor & 3,000+ shader corpus bench.

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

inline constexpr uint32_t kGfx103TotalShaderOpcodes = 779u;
inline constexpr uint32_t kRdna2CoverageSchemaVersion = 1u;

struct Rdna2BenchOptions {
    bool full_corpus = false;
    uint32_t corpus_count = 3120u;
    uint32_t external_val_sample = 432u;
    bool run_live_gpu = true;
    bool run_external_validators = true;
    bool validate_all_external = false;
    bool quiet = false;
    std::string json_output_path;
    std::string spirv_val_path;
    std::string dxc_path;
};

struct Rdna2FamilyCoverage {
    std::string family;
    uint32_t total = 0u;
    uint32_t decoded = 0u;
    uint32_t lifted_spirv = 0u;
    uint32_t lifted_dxil = 0u;
    uint32_t cpu_ref = 0u;
    uint32_t gpu_verified = 0u;
};

struct Rdna2StageCoverage {
    std::string stage;  // "CS", "VS", "PS", "MS", "AS"
    uint32_t total = 0u;
    uint32_t passed = 0u;
    uint32_t disasm_passed = 0u;
    uint32_t spirv_passed = 0u;
    uint32_t dxil_passed = 0u;
    uint32_t cpu_ref_passed = 0u;
    uint32_t gpu_passed = 0u;
};

struct Rdna2WorkloadCoverage {
    std::string workload;
    std::string description;
    uint32_t total = 0u;
    uint32_t passed = 0u;
    uint32_t spirv_passed = 0u;
    uint32_t dxil_passed = 0u;
    uint32_t cpu_ref_passed = 0u;
    uint32_t gpu_passed = 0u;
};

struct Rdna2CoverageReport {
    uint32_t schema_version = kRdna2CoverageSchemaVersion;
    std::string isa_target = "GFX10.3 (RDNA2 / Xbox Series X|S)";

    uint32_t opcodes_total = kGfx103TotalShaderOpcodes;
    uint32_t opcodes_decoded = 0u;
    uint32_t opcodes_lifted_spirv = 0u;
    uint32_t opcodes_lifted_dxil = 0u;
    uint32_t opcodes_cpu_ref = 0u;
    uint32_t opcodes_gpu_verified = 0u;

    uint32_t corpus_shaders_total = 0u;
    uint32_t corpus_shaders_passed = 0u;
    uint32_t corpus_unique_hashes = 0u;
    uint32_t corpus_spirv_validated = 0u;
    uint32_t spirv_val_external_validated = 0u;
    uint32_t corpus_dxil_compiled = 0u;
    uint32_t corpus_dxil_resigned_verified = 0u;
    uint32_t corpus_cpu_ref_executed = 0u;
    uint32_t corpus_gpu_executed = 0u;
    uint32_t corpus_gpu_hw_executed = 0u;
    uint32_t corpus_gpu_warp_executed = 0u;
    uint32_t corpus_vulkan_executed = 0u;

    bool has_sv_isa_extension = false;
    bool has_mem_isa_extension = false;
    bool has_spirv_val_tool = false;
    bool has_dxc_tool = false;
    bool has_d3d12_hw = false;
    bool has_d3d12_warp = false;
    bool has_vulkan_hw = false;
    std::string d3d12_hw_adapter;
    std::string vulkan_adapter;
    double elapsed_seconds = 0.0;

    std::vector<Rdna2FamilyCoverage> families;
    std::vector<Rdna2StageCoverage> stages;
    std::vector<Rdna2WorkloadCoverage> workloads;
};

// Returns the exact size of the standard GFX10.3 shader-reachable opcode reference catalog (779).
uint32_t GetGfx103ReferenceCatalogSize() noexcept;

// Audits implemented opcodes across core rdna2_disasm/ir/lifter/interpreter plus optional
// rdna2_sv (Worker 1) and rdna2_mem (Worker 2) ISA extensions against the 779-opcode catalog.
Status AuditRdna2IsaCoverage(Rdna2CoverageReport& out_report);

// Runs the full ISA coverage audit, deterministic GFX10.3 shader corpus generation & validation,
// optional spirv-val / DXC SM 6.5 compilation + MD5 re-signing, CPU reference checks, and
// optional live GPU (D3D12 HW + WARP + Vulkan) parity verification.
Status RunRdna2CoverageBenchmark(const Rdna2BenchOptions& opts, Rdna2CoverageReport& out_report);

// Serializes the coverage report to deterministic, human-readable JSON (schema_version = 1).
std::string SerializeRdna2CoverageJson(const Rdna2CoverageReport& report);

// Writes the serialized JSON coverage report to disk (creating parent directories if needed).
Status WriteRdna2CoverageJsonFile(const std::string& path, const Rdna2CoverageReport& report);

// CLI entrypoint:
//   openscarlett rdna2-bench [--full] [--fast] [--json <path>] [--corpus-count N]
//                            [--external-sample N] [--validate-all-external]
//                            [--no-gpu] [--no-external] [--quiet]
int RunRdna2BenchCli(int argc, char** argv);

}  // namespace openscarlett
