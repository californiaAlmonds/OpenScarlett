// OpenScarlett Wave 4 — Agent F: SimpleLighting Multi-Frame 3D Blinn-Phong + XDisplay +
// X3DAudio/XAudio2FX + Live Vulkan 1.4 Cross-Backend Parity Test Harness.
//
// 100% Portable for CI hidden-build smoke tests:
//   - Resolves openscarlett.exe / openscarlett_hook.dll / gdk_simple_lighting.exe via GetModuleFileNameW.
//   - Resolves dxc.exe portably (exe dir -> OPENSCARLETT_DXC_PATH -> PATH -> Windows Kits).
//   - Stages a zero-DLL Xbox package folder under %TEMP% with MicrosoftGame.Config,
//     compiled SM 6.5 + SM 6.2 Blinn-Phong DXIL shaders, and gdk_simple_lighting.exe.
//   - Launches via `openscarlett.exe run <package_dir>` on:
//       1) Live D3D12 Hardware GPU (when present, e.g. NVIDIA GTX 1650 Ti),
//       2) D3D12 WARP (`-- --warp`),
//       3) CLI `--backend=vulkan` routing check (`OPENSCARLETT_GPU_BACKEND=vulkan`).
//   - Verifies zero-DLL package byte-exactness, 3 distinct animated 256x256 P6 PPM frames,
//     Blinn-Phong diffuse/specular/depth occlusion, HW-vs-WARP pixel MAE parity, XDisplay +
//     X3DAudio + XAudio2FX spatial audio metrics, and unified CallTelemetry (not_impl == 0
//     across d3d12_xs.dll, xgameruntime.dll, XAudio2_9.dll, pixevt.dll, vfs).
//   - Also executes the EXACT SAME 3D Blinn-Phong lit scene (vertex normals, D32_SFLOAT depth
//     buffer, UBO + push-constants lighting math) live through VulkanBackend::ExecuteGraphicsDraw()
//     on Vulkan 1.4 (NVIDIA GTX 1650 Ti) + Software Fallback, cross-checking rendered Vulkan
//     output directly against D3D12 for silhouette, specular highlights, depth occlusion, and
//     tight RGB MAE parity!

#include <windows.h>

#include <algorithm>
#include <cctype>
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
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint8_t kBgR = 12u;
constexpr uint8_t kBgG = 16u;
constexpr uint8_t kBgB = 28u;
constexpr float kPi = 3.14159265358979323846f;

constexpr const char* kLightingVsHlsl = R"HLSL(
#define LIGHTING_ROOT_SIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)"

cbuffer LightingConstants : register(b0) {
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_DirLightDir;
    float4 g_DirLightColor;
    float4 g_PointLightPos;
    float4 g_PointLightColor;
    float4 g_MaterialParams;
};

struct VsIn {
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
};

struct VsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
    float4 BaseColor  : COLOR0;
};

[RootSignature(LIGHTING_ROOT_SIG)]
VsOut main(VsIn input) {
    VsOut o;
    float4 worldPos = float4(input.Position, 1.0f);
    o.PositionCS = mul(worldPos, g_ViewProj);
    o.WorldPos = input.Position;
    o.WorldNorm = input.Normal;
    o.BaseColor = input.Color;
    return o;
}
)HLSL";

constexpr const char* kLightingPsHlsl = R"HLSL(
#define LIGHTING_ROOT_SIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)"

cbuffer LightingConstants : register(b0) {
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_DirLightDir;
    float4 g_DirLightColor;
    float4 g_PointLightPos;
    float4 g_PointLightColor;
    float4 g_MaterialParams;
};

struct VsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
    float4 BaseColor  : COLOR0;
};

float3 SafeNorm(float3 v) {
    float d = max(dot(v, v), 1e-8f);
    return v / sqrt(d);
}

float Pow32(float x) {
    float s2 = x * x;
    float s4 = s2 * s2;
    float s8 = s4 * s4;
    float s16 = s8 * s8;
    return s16 * s16;
}

[RootSignature(LIGHTING_ROOT_SIG)]
float4 main(VsOut input) : SV_TARGET0 {
    float3 albedo = input.BaseColor.rgb;
    float specMask = input.BaseColor.a;

    float3 N = SafeNorm(input.WorldNorm);
    float3 V = SafeNorm(g_CameraPos.xyz - input.WorldPos);

    // 1. Directional Key Light (Blinn-Phong diffuse + spec^32)
    float3 Ld = SafeNorm(g_DirLightDir.xyz);
    float NdotLd = max(dot(N, Ld), 0.0f);
    float3 Hd = SafeNorm(Ld + V);
    float NdotHd = max(dot(N, Hd), 0.0f);
    float specD = Pow32(NdotHd);

    float3 dirDiffuse = albedo * g_DirLightColor.rgb * (NdotLd * g_DirLightDir.w);
    float3 dirSpec = g_DirLightColor.rgb * (specD * specMask * g_MaterialParams.x * g_DirLightDir.w);

    // 2. Orbiting Point Light (smooth distance attenuation + Blinn-Phong diffuse & spec^32)
    float3 toP = g_PointLightPos.xyz - input.WorldPos;
    float distSq = dot(toP, toP);
    float dist = sqrt(max(distSq, 1e-6f));
    float3 Lp = toP / dist;
    float atten = g_PointLightPos.w / (1.0f + 0.25f * dist + g_PointLightColor.a * distSq);
    float NdotLp = max(dot(N, Lp), 0.0f);
    float3 Hp = SafeNorm(Lp + V);
    float NdotHp = max(dot(N, Hp), 0.0f);
    float specP = Pow32(NdotHp);

    float3 ptDiffuse = albedo * g_PointLightColor.rgb * (NdotLp * atten);
    float3 ptSpec = g_PointLightColor.rgb * (specP * specMask * g_MaterialParams.y * atten);

    // 3. Hemisphere ambient + Reinhard tone map + gamma 2.0 encoding (exact match with Vulkan SPIR-V)
    float hemi = clamp(N.y * 0.5f + 0.5f, 0.0f, 1.0f);
    float3 ambient = albedo * (g_DirLightColor.a * (0.55f + 0.45f * hemi));

    float3 linearColor = ambient + dirDiffuse + dirSpec + ptDiffuse + ptSpec;
    float3 mapped = linearColor / (linearColor + float3(1.0f, 1.0f, 1.0f));
    float3 finalRgb = sqrt(clamp(mapped, 0.0f, 1.0f));
    return float4(finalRgb, 1.0f);
}
)HLSL";

constexpr const char* kMicrosoftGameConfigXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Game configVersion=\"1\">\n"
    "  <Identity Name=\"OpenScarlett.SimpleLighting\"\n"
    "            Publisher=\"CN=OpenScarlett, O=MIT\"\n"
    "            Version=\"1.0.0.0\" />\n"
    "  <TitleId>0x5CA01003</TitleId>\n"
    "  <MSAAppId>000000004C261003</MSAAppId>\n"
    "  <ShellVisuals DefaultDisplayName=\"OpenScarlett SimpleLighting\" />\n"
    "  <ExecutableList>\n"
    "    <Executable Name=\"gdk_simple_lighting.exe\"\n"
    "                Id=\"Game\"\n"
    "                TargetDeviceFamily=\"Scarlett\" />\n"
    "  </ExecutableList>\n"
    "</Game>\n";

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

    bool HasSymbol(const std::string& sym, int32_t expected_slot = -999, uint64_t min_ok = 1) const {
        for (const auto& r : rows) {
            if (r.symbol == sym) {
                if (expected_slot == -999 || r.slot == expected_slot) {
                    return r.total >= min_ok && r.ok >= min_ok;
                }
            }
        }
        return false;
    }

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }
};

struct DirFileDigest {
    std::string rel_name;
    uint64_t byte_size = 0;
    uint64_t fnv1a_64 = 0;

    bool operator==(const DirFileDigest& other) const noexcept {
        return rel_name == other.rel_name &&
               byte_size == other.byte_size &&
               fnv1a_64 == other.fnv1a_64;
    }
};

struct RunReport {
    std::string mode;
    std::map<std::string, std::string> metrics;
    TelemetrySummary telemetry;
    std::vector<uint8_t> ppm_rgb;
    double wall_ms = 0.0;
};

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

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
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    ifs.seekg(0, std::ios::end);
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

std::vector<DirFileDigest> SnapshotDirectoryBytes(const fs::path& dir) {
    std::vector<DirFileDigest> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        OS_REQUIRE(entry.is_regular_file());
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");

        const auto bytes = ReadBinaryFile(entry.path());
        DirFileDigest d{};
        d.rel_name = fs::relative(entry.path(), dir).generic_string();
        d.byte_size = bytes.size();
        d.fnv1a_64 = Fnv1a64(bytes.data(), bytes.size());
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const DirFileDigest& a, const DirFileDigest& b) {
        return a.rel_name < b.rel_name;
    });
    return out;
}

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec)) {
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

fs::path FindDxcExe() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const auto beside = fs::path(self_buf).parent_path() / L"dxc.exe";
        if (fs::exists(beside, ec)) return beside;
    }
    wchar_t env_dxc[1024]{};
    const DWORD env_len = ::GetEnvironmentVariableW(
        L"OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(1024));
    if (env_len > 0 && env_len < 1024 && fs::exists(env_dxc, ec)) {
        return fs::path(env_dxc);
    }
    wchar_t path_buf[MAX_PATH]{};
    const DWORD path_len = ::SearchPathW(
        nullptr, L"dxc.exe", nullptr, static_cast<DWORD>(MAX_PATH), path_buf, nullptr);
    if (path_len > 0 && path_len < MAX_PATH && fs::exists(path_buf, ec)) {
        return fs::path(path_buf);
    }
    const wchar_t* candidates[] = {
        LR"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
        LR"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
    };
    for (const wchar_t* p : candidates) {
        if (fs::exists(p, ec)) return fs::path(p);
    }
    const fs::path kits_bin(LR"(C:\Program Files (x86)\Windows Kits\10\bin)");
    if (fs::exists(kits_bin, ec)) {
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory()) continue;
            const auto cand = entry.path() / L"x64" / L"dxc.exe";
            if (fs::exists(cand, ec)) found.push_back(cand);
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.back();
    }
    return {};
}

DWORD RunProcessW(const fs::path& exe_path, const std::wstring& cmd_line, DWORD timeout_ms = 45000) {
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

void CompileShaderWithDxc(const fs::path& dxc_exe,
                          const fs::path& hlsl_path,
                          const fs::path& cso_out_path,
                          const std::wstring& profile,
                          const std::wstring& entry_point) {
    const std::wstring cmd =
        L"\"" + dxc_exe.wstring() + L"\" -T " + profile +
        L" -E " + entry_point + L" -O3 -WX" +
        L" -Fo \"" + cso_out_path.wstring() + L"\" \"" + hlsl_path.wstring() + L"\"";
    const DWORD rc = RunProcessW(dxc_exe, cmd, 30000);
    OS_REQUIRE(rc == 0);
    OS_REQUIRE(fs::exists(cso_out_path));
    OS_REQUIRE(fs::file_size(cso_out_path) > 64u);
}

bool ExtractJsonString(const std::string& line, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        const char c = line[pos++];
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

std::vector<uint8_t> VerifyAndLoadPpm(const fs::path& ppm_path, uint32_t width, uint32_t height) {
    OS_REQUIRE(fs::exists(ppm_path));
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const size_t rgb_bytes = static_cast<size_t>(width) * height * 3u;
    const auto all_bytes = ReadBinaryFile(ppm_path);
    OS_REQUIRE(all_bytes.size() == header.size() + rgb_bytes);
    OS_REQUIRE(std::memcmp(all_bytes.data(), header.data(), header.size()) == 0);
    return std::vector<uint8_t>(all_bytes.begin() + static_cast<std::ptrdiff_t>(header.size()),
                                all_bytes.end());
}

void VerifyStandaloneXboxPeImports(const fs::path& exe_path,
                                   const std::vector<std::string>& required_dlls) {
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
        OS_REQUIRE(lo != "d3d12.dll");
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

DWORD LaunchPackageDirViaOpenScarlettCli(const fs::path& cli_exe,
                                         const fs::path& package_dir,
                                         const fs::path& shim_dir,
                                         const fs::path& vfs_g_overlay,
                                         const fs::path& vfs_t,
                                         const fs::path& vfs_s,
                                         const fs::path& telemetry_json,
                                         const std::wstring& extra_cli_flags = {},
                                         const std::wstring& guest_args = {}) {
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + package_dir.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-g-overlay \"" + vfs_g_overlay.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --vfs-s \"" + vfs_s.wstring() +
        L"\" --telemetry \"" + telemetry_json.wstring() +
        L"\" --timeout-ms 45000";
    if (!extra_cli_flags.empty()) {
        cmd += L" " + extra_cli_flags;
    }
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }
    return RunProcessW(cli_exe, cmd, 50000);
}

void VerifySimpleLightingRun(const std::string& mode,
                             const fs::path& cli_exe,
                             const fs::path& pkg_dir,
                             const fs::path& build_dir,
                             const fs::path& stage_root,
                             const std::vector<DirFileDigest>& pkg_before,
                             const std::wstring& extra_cli_flags,
                             const std::wstring& guest_args,
                             const std::string& expected_backend_env,
                             RunReport& out_report) {
    const fs::path run_ovl = stage_root / ("run_" + mode + "_ovl");
    const fs::path run_t = stage_root / ("run_" + mode + "_T");
    const fs::path run_s = stage_root / ("run_" + mode + "_S");
    const fs::path run_tel = stage_root / ("telemetry_" + mode + ".json");

    const auto t0 = std::chrono::steady_clock::now();
    const DWORD rc = LaunchPackageDirViaOpenScarlettCli(
        cli_exe, pkg_dir, build_dir, run_ovl, run_t, run_s, run_tel, extra_cli_flags, guest_args);
    out_report.wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    OS_REQUIRE(rc == 0);

    // 1. Target package directory remains zero-DLL and 100% byte-identical!
    const auto pkg_after = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_after == pkg_before);

    // 2. Verify all 3 animated 256x256 P6 PPM frames exist and are valid
    const auto f0_rgb = VerifyAndLoadPpm(run_t / "simple_lighting_frame0.ppm", kWidth, kHeight);
    const auto f1_rgb = VerifyAndLoadPpm(run_t / "simple_lighting_frame1.ppm", kWidth, kHeight);
    out_report.ppm_rgb = VerifyAndLoadPpm(run_t / "simple_lighting_frame2.ppm", kWidth, kHeight);
    OS_REQUIRE(f0_rgb != f1_rgb);
    OS_REQUIRE(f1_rgb != out_report.ppm_rgb);
    OS_REQUIRE(f0_rgb != out_report.ppm_rgb);

    // Verify human-readable audio/display report was emitted
    OS_REQUIRE(fs::exists(run_t / "audio_display_report.txt"));
    const std::string rep_text = ReadTextFile(run_t / "audio_display_report.txt");
    OS_REQUIRE(rep_text.find("XDisplay.TimeoutDeferral=Acquired+Closed") != std::string::npos);
    OS_REQUIRE(rep_text.find("X3DAudio.Frame2=") != std::string::npos);

    // 3. Verify metrics.txt (TitleId, backend env, Blinn-Phong pixels, XDisplay, X3DAudio, XAudio2FX)
    out_report.mode = mode;
    out_report.metrics = ParseKeyValueFile(run_t / "metrics.txt");
    const auto& m = out_report.metrics;
    OS_REQUIRE(m.at("sample") == "SimpleLighting");
    OS_REQUIRE(m.at("title_id") == "0x5CA01003");
    OS_REQUIRE(m.at("gpu_backend_env") == expected_backend_env);
    OS_REQUIRE(m.at("width") == "256");
    OS_REQUIRE(m.at("height") == "256");
    OS_REQUIRE(m.at("depth_occlusion_verified") == "1");
    OS_REQUIRE(std::stoull(m.at("triangle_count")) >= 500ull);
    OS_REQUIRE(m.at("frame0_fnv1a") != m.at("frame1_fnv1a"));
    OS_REQUIRE(m.at("frame1_fnv1a") != m.at("frame2_fnv1a"));
    OS_REQUIRE(m.at("frame0_fnv1a") != m.at("frame2_fnv1a"));
    OS_REQUIRE(std::stoull(m.at("specular_pixels")) >= 40ull);
    OS_REQUIRE(std::stoull(m.at("diffuse_pixels")) >= 4000ull);
    OS_REQUIRE(std::stoull(m.at("shadow_pixels")) >= 300ull);
    OS_REQUIRE(std::stoull(m.at("front_cube_pixels")) >= 500ull);
    OS_REQUIRE(std::stoull(m.at("back_sphere_pixels")) >= 2000ull);

    // XDisplay checks
    OS_REQUIRE(m.at("xdisplay_deferral_ok") == "1");
    const uint32_t hdr_result = static_cast<uint32_t>(std::stoul(m.at("xdisplay_hdr_result")));
    const double max_nits = std::stod(m.at("xdisplay_max_nits"));
    if (hdr_result == 1u) {  // XDisplayHdrModeResult_Enabled
        OS_REQUIRE(max_nits > 0.0);
    } else {
        OS_REQUIRE(hdr_result == 2u);  // XDisplayHdrModeResult_Disabled
        OS_REQUIRE(max_nits == 0.0);
    }

    // XAudio2FX + X3DAudio checks (left -> center -> right panning, distance attenuation, Doppler shift)
    OS_REQUIRE(std::fabs(std::stod(m.at("xaudio2fx_reverb_wetdry")) - 42.5) < 0.1);
    OS_REQUIRE(std::fabs(std::stod(m.at("xaudio2fx_reverb_decay")) - 1.35) < 0.05);
    OS_REQUIRE(std::stod(m.at("x3d_f0_gain_l")) > std::stod(m.at("x3d_f0_gain_r")) * 2.0);
    OS_REQUIRE(std::fabs(std::stod(m.at("x3d_f1_gain_l")) - std::stod(m.at("x3d_f1_gain_r"))) < 0.05);
    OS_REQUIRE(std::stod(m.at("x3d_f2_gain_r")) > std::stod(m.at("x3d_f2_gain_l")) * 2.0);
    OS_REQUIRE(std::stod(m.at("x3d_f0_dist")) > std::stod(m.at("x3d_f1_dist")));
    OS_REQUIRE(std::stod(m.at("x3d_f0_doppler")) > 1.03);
    OS_REQUIRE(std::stod(m.at("x3d_f2_doppler")) < 0.97);

    // 4. Verify unified CallTelemetry JSON: not_impl == 0 across all modules
    out_report.telemetry = ParseTelemetryJson(run_tel);
    const auto& tel = out_report.telemetry;
    OS_REQUIRE(tel.total_not_impl == 0);
    OS_REQUIRE(tel.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(tel.HasModule("xgameruntime.dll"));
    OS_REQUIRE(tel.HasModule("XAudio2_9.dll"));
    OS_REQUIRE(tel.HasModule("pixevt.dll"));
    OS_REQUIRE(tel.HasModule("vfs"));

    // Core D3D12X vtable slots
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateDepthStencilView", 21, 1));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, 9));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::SetGraphicsRootConstantBufferView", 38, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::IASetIndexBuffer", 43, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::PresentX", 19, 4));  // 3 frames + PresentX(0)

    // XDisplay + X3DAudio + XAudio2FX + PIX symbols
    OS_REQUIRE(tel.HasSymbol("XGameGetXboxTitleId", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XDisplayTryEnableHdrMode", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XDisplayAcquireTimeoutDeferral", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XDisplayCloseTimeoutDeferralHandle", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XAudio2Create", -1, 1));
    OS_REQUIRE(tel.HasSymbol("X3DAudioInitialize", -1, 1));
    OS_REQUIRE(tel.HasSymbol("X3DAudioCalculate", -1, 3));
    OS_REQUIRE(tel.HasSymbol("XAudio2CreateReverb", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XAudio2CreateVolumeMeter", -1, 1));
    OS_REQUIRE(tel.HasSymbol("PIXBeginEventOnCommandList", -1, 3));
    OS_REQUIRE(tel.HasSymbol("PIXEndEventOnCommandList", -1, 3));
}

// ===========================================================================
// Vulkan 1.4 Live Blinn-Phong Scene Renderer & Parity Cross-Check
// Uses exact same 3D scene geometry, camera, directional + orbiting point
// light, D32_SFLOAT depth buffer, UBO + push-constants lighting math!
// Works on BOTH live Vulkan 1.4.329 Hardware ICD (GTX 1650 Ti) AND Software Fallback.
// ===========================================================================
struct Vec3 {
    float x;
    float y;
    float z;
};

struct Mat4 {
    float m[4][4];
};

struct VkLightingVertex {
    float pos[4];     // location 0: (x, y, z, 1.0)
    float normal[4];  // location 1: (nx, ny, nz, 0.0)
    float color[4];   // location 2: (r, g, b, specStrength)
};
static_assert(sizeof(VkLightingVertex) == 48, "VkLightingVertex must be 48 bytes");

struct VkLightingUbo {
    float view_proj[16];      // offset 0  (row-major floats = column-major transpose -> M*v == mul(v, M_rm))
    float camera_pos[4];      // offset 64
    float dir_light_dir[4];   // offset 80
    float dir_light_color[4]; // offset 96
};
static_assert(sizeof(VkLightingUbo) == 112, "VkLightingUbo must be 112 bytes");

struct VkLightingPushConstants {
    float point_light_pos[4];    // offset 0
    float point_light_color[4];  // offset 16
    float material_params[4];    // offset 32
};
static_assert(sizeof(VkLightingPushConstants) == 48, "VkLightingPushConstants must be 48 bytes");

Vec3 VSub(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float VDot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 VCross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 VNorm(Vec3 v) noexcept {
    const float len = std::sqrt(VDot(v, v));
    if (len < 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v.x / len, v.y / len, v.z / len};
}
Vec3 VRotateYawPitch(Vec3 v, float yaw, float pitch) noexcept {
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float y1 = v.y * cp - v.z * sp;
    const float z1 = v.y * sp + v.z * cp;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    return {v.x * cy + z1 * sy, y1, -v.x * sy + z1 * cy};
}

Mat4 MakeLookAtRH(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 zaxis = VNorm(VSub(eye, target));
    const Vec3 xaxis = VNorm(VCross(up, zaxis));
    const Vec3 yaxis = VCross(zaxis, xaxis);
    Mat4 r{};
    r.m[0][0] = xaxis.x; r.m[0][1] = yaxis.x; r.m[0][2] = zaxis.x;
    r.m[1][0] = xaxis.y; r.m[1][1] = yaxis.y; r.m[1][2] = zaxis.y;
    r.m[2][0] = xaxis.z; r.m[2][1] = yaxis.z; r.m[2][2] = zaxis.z;
    r.m[3][0] = -VDot(xaxis, eye);
    r.m[3][1] = -VDot(yaxis, eye);
    r.m[3][2] = -VDot(zaxis, eye);
    r.m[3][3] = 1.0f;
    return r;
}

Mat4 MakePerspectiveFovRH(float fov_y_rad, float aspect, float near_z, float far_z) noexcept {
    const float y_scale = 1.0f / std::tan(fov_y_rad * 0.5f);
    const float x_scale = y_scale / aspect;
    Mat4 r{};
    r.m[0][0] = x_scale;
    r.m[1][1] = y_scale;
    r.m[2][2] = far_z / (near_z - far_z);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (near_z * far_z) / (near_z - far_z);
    return r;
}

Mat4 MultiplyMat4(const Mat4& a, const Mat4& b) noexcept {
    Mat4 out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[row][k] * b.m[k][col];
            out.m[row][col] = sum;
        }
    }
    return out;
}

bool ProjectWorldToPixel(Vec3 p, const Mat4& vp, uint32_t width, uint32_t height,
                         int32_t& out_x, int32_t& out_y) noexcept {
    const float cx = p.x * vp.m[0][0] + p.y * vp.m[1][0] + p.z * vp.m[2][0] + vp.m[3][0];
    const float cy = p.x * vp.m[0][1] + p.y * vp.m[1][1] + p.z * vp.m[2][1] + vp.m[3][1];
    const float cw = p.x * vp.m[0][3] + p.y * vp.m[1][3] + p.z * vp.m[2][3] + vp.m[3][3];
    if (cw <= 1e-5f) return false;
    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    out_x = static_cast<int32_t>(std::lround((ndc_x * 0.5f + 0.5f) * static_cast<float>(width - 1u)));
    out_y = static_cast<int32_t>(std::lround((0.5f - ndc_y * 0.5f) * static_cast<float>(height - 1u)));
    return out_x >= 0 && out_x < static_cast<int32_t>(width) &&
           out_y >= 0 && out_y < static_cast<int32_t>(height);
}

void BuildSharedLightingScene(std::vector<VkLightingVertex>& verts, std::vector<uint32_t>& indices) {
    verts.clear();
    indices.clear();

    // 1. Front occluding faceted cube (drawn FIRST)
    {
        const Vec3 center{-0.18f, 0.08f, 1.08f};
        const float h = 0.30f;
        const float yaw = 0.45f;
        const float pitch = 0.28f;
        constexpr float kCubeColor[4] = {0.16f, 0.62f, 0.98f, 0.90f};
        struct FaceDef { Vec3 normal; Vec3 corners[4]; };
        const FaceDef faces[6] = {
            {{ 0.0f,  0.0f,  1.0f}, {{-h,  h,  h}, { h,  h,  h}, { h, -h,  h}, {-h, -h,  h}}},
            {{ 0.0f,  0.0f, -1.0f}, {{ h,  h, -h}, {-h,  h, -h}, {-h, -h, -h}, { h, -h, -h}}},
            {{ 1.0f,  0.0f,  0.0f}, {{ h,  h,  h}, { h,  h, -h}, { h, -h, -h}, { h, -h,  h}}},
            {{-1.0f,  0.0f,  0.0f}, {{-h,  h, -h}, {-h,  h,  h}, {-h, -h,  h}, {-h, -h, -h}}},
            {{ 0.0f,  1.0f,  0.0f}, {{-h,  h, -h}, { h,  h, -h}, { h,  h,  h}, {-h,  h,  h}}},
            {{ 0.0f, -1.0f,  0.0f}, {{-h, -h,  h}, { h, -h,  h}, { h, -h, -h}, {-h, -h, -h}}},
        };
        for (uint32_t f = 0; f < 6u; ++f) {
            const uint32_t base = static_cast<uint32_t>(verts.size());
            const Vec3 n = VNorm(VRotateYawPitch(faces[f].normal, yaw, pitch));
            for (uint32_t c = 0; c < 4u; ++c) {
                const Vec3 rp = VRotateYawPitch(faces[f].corners[c], yaw, pitch);
                verts.push_back(VkLightingVertex{
                    {center.x + rp.x, center.y + rp.y, center.z + rp.z, 1.0f},
                    {n.x, n.y, n.z, 0.0f},
                    {kCubeColor[0], kCubeColor[1], kCubeColor[2], kCubeColor[3]},
                });
            }
            indices.insert(indices.end(), {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
        }
    }

    // 2. Main smooth UV-sphere (drawn SECOND -> depth occluded behind front cube!)
    {
        const Vec3 center{0.12f, 0.10f, 0.0f};
        const float radius = 0.85f;
        const uint32_t lon_segs = 24u;
        const uint32_t lat_segs = 16u;
        constexpr float kSphereColor[4] = {0.98f, 0.48f, 0.14f, 1.00f};
        const uint32_t base = static_cast<uint32_t>(verts.size());

        for (uint32_t lat = 0; lat <= lat_segs; ++lat) {
            const float phi = (static_cast<float>(lat) / static_cast<float>(lat_segs)) * kPi;
            const float sp = std::sin(phi);
            const float cp = std::cos(phi);
            for (uint32_t lon = 0; lon <= lon_segs; ++lon) {
                const float theta = (static_cast<float>(lon) / static_cast<float>(lon_segs)) * 2.0f * kPi;
                const float nx = sp * std::cos(theta);
                const float ny = cp;
                const float nz = sp * std::sin(theta);
                verts.push_back(VkLightingVertex{
                    {center.x + nx * radius, center.y + ny * radius, center.z + nz * radius, 1.0f},
                    {nx, ny, nz, 0.0f},
                    {kSphereColor[0], kSphereColor[1], kSphereColor[2], kSphereColor[3]},
                });
            }
        }
        const uint32_t stride = lon_segs + 1u;
        for (uint32_t lat = 0; lat < lat_segs; ++lat) {
            for (uint32_t lon = 0; lon < lon_segs; ++lon) {
                const uint32_t v00 = base + lat * stride + lon;
                const uint32_t v10 = base + (lat + 1u) * stride + lon;
                const uint32_t v01 = base + lat * stride + (lon + 1u);
                const uint32_t v11 = base + (lat + 1u) * stride + (lon + 1u);
                indices.insert(indices.end(), {v00, v10, v01, v10, v11, v01});
            }
        }
    }

    // 3. 4x4 Checkered ground plane (drawn THIRD)
    {
        const float y_plane = -0.78f;
        const float half_span = 2.60f;
        const uint32_t grid_cells = 4u;
        const float cell_w = (2.0f * half_span) / static_cast<float>(grid_cells);
        constexpr float kTileA[4] = {0.26f, 0.44f, 0.48f, 0.55f};
        constexpr float kTileB[4] = {0.16f, 0.25f, 0.34f, 0.35f};

        for (uint32_t gz = 0; gz < grid_cells; ++gz) {
            for (uint32_t gx = 0; gx < grid_cells; ++gx) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                const float x0 = -half_span + static_cast<float>(gx) * cell_w;
                const float x1 = x0 + cell_w;
                const float z0 = -half_span + static_cast<float>(gz) * cell_w;
                const float z1 = z0 + cell_w;
                const float* col = (((gx ^ gz) & 1u) == 0u) ? kTileA : kTileB;
                const Vec3 pts[4] = {
                    {x0, y_plane, z0}, {x1, y_plane, z0}, {x1, y_plane, z1}, {x0, y_plane, z1},
                };
                for (uint32_t c = 0; c < 4u; ++c) {
                    verts.push_back(VkLightingVertex{
                        {pts[c].x, pts[c].y, pts[c].z, 1.0f},
                        {0.0f, 1.0f, 0.0f, 0.0f},
                        {col[0], col[1], col[2], col[3]},
                    });
                }
                indices.insert(indices.end(), {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
            }
        }
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

    std::vector<uint32_t> Finish(uint32_t spv_version = 0x00010300u) const {
        std::vector<uint32_t> out;
        out.reserve(5 + words.size());
        out.insert(out.end(), {0x07230203u, spv_version, 0x4F533400u, bound, 0u});
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

// SPIR-V 1.3 Vertex Shader:
//   in  loc 0: vec4 pos, loc 1: vec4 normal, loc 2: vec4 color
//   UBO set 0 binding 0: struct { mat4 view_proj; vec4 camera_pos; vec4 dir_light_dir; vec4 dir_light_color; }
//   out gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w)  (Vulkan top-down Y flip matching D3D12!)
//   out loc 0: worldPos, loc 1: worldNorm, loc 2: baseColor
std::vector<uint32_t> BuildBlinnPhongVertexSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v4 = b.Id(), t_m4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_pv = b.Id(), t_out_pv = b.Id();
    const uint32_t t_ubo = b.Id(), t_ptr_ubo = b.Id(), t_ptr_ubo_m4 = b.Id();
    const uint32_t v_in_pos = b.Id(), v_in_nrm = b.Id(), v_in_col = b.Id();
    const uint32_t v_out_pv = b.Id(), v_out_pos = b.Id(), v_out_nrm = b.Id(), v_out_col = b.Id();
    const uint32_t v_ubo = b.Id();
    const uint32_t c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});     // OpCapability Shader
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {0, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_in_nrm, v_in_col, v_out_pv, v_out_pos, v_out_nrm, v_out_col});
        b.OpVec(15, ep);
    }

    // Decorations
    b.Op(71, {v_in_pos, 30, 0});
    b.Op(71, {v_in_nrm, 30, 1});
    b.Op(71, {v_in_col, 30, 2});
    b.Op(71, {v_out_pos, 30, 0});
    b.Op(71, {v_out_nrm, 30, 1});
    b.Op(71, {v_out_col, 30, 2});
    b.Op(71, {t_pv, 2});             // Block
    b.Op(72, {t_pv, 0, 11, 0});      // Member 0 BuiltIn Position
    b.Op(71, {t_ubo, 2});            // Block
    b.Op(72, {t_ubo, 0, 5});         // Member 0 ColMajor
    b.Op(72, {t_ubo, 0, 7, 16});     // Member 0 MatrixStride 16
    b.Op(72, {t_ubo, 0, 35, 0});     // Member 0 Offset 0
    b.Op(72, {t_ubo, 1, 35, 64});    // Member 1 Offset 64
    b.Op(72, {t_ubo, 2, 35, 80});    // Member 2 Offset 80
    b.Op(72, {t_ubo, 3, 35, 96});    // Member 3 Offset 96
    b.Op(71, {v_ubo, 34, 0});        // DescriptorSet 0
    b.Op(71, {v_ubo, 33, 0});        // Binding 0

    // Types & Constants
    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(24, {t_m4, t_v4, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_pv, t_v4});
    b.Op(32, {t_out_pv, 3, t_pv});
    b.Op(30, {t_ubo, t_m4, t_v4, t_v4, t_v4});
    b.Op(32, {t_ptr_ubo, 2, t_ubo});
    b.Op(32, {t_ptr_ubo_m4, 2, t_m4});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_in_v4, v_in_pos, 1});
    b.Op(59, {t_in_v4, v_in_nrm, 1});
    b.Op(59, {t_in_v4, v_in_col, 1});
    b.Op(59, {t_out_pv, v_out_pv, 3});
    b.Op(59, {t_out_v4, v_out_pos, 3});
    b.Op(59, {t_out_v4, v_out_nrm, 3});
    b.Op(59, {t_out_v4, v_out_col, 3});
    b.Op(59, {t_ptr_ubo, v_ubo, 2});

    // main()
    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t pos = b.Id(), nrm = b.Id(), col = b.Id();
    const uint32_t p_vp = b.Id(), vp = b.Id(), clip = b.Id();
    const uint32_t cx = b.Id(), cy = b.Id(), cz = b.Id(), cw = b.Id(), neg_cy = b.Id();
    const uint32_t clip_vk = b.Id(), p_out_pos = b.Id();

    b.Op(61, {t_v4, pos, v_in_pos});
    b.Op(61, {t_v4, nrm, v_in_nrm});
    b.Op(61, {t_v4, col, v_in_col});
    b.Op(65, {t_ptr_ubo_m4, p_vp, v_ubo, c_i0});
    b.Op(61, {t_m4, vp, p_vp});
    b.Op(145, {t_v4, clip, vp, pos});        // OpMatrixTimesVector
    b.Op(81, {t_f32, cx, clip, 0});
    b.Op(81, {t_f32, cy, clip, 1});
    b.Op(81, {t_f32, cz, clip, 2});
    b.Op(81, {t_f32, cw, clip, 3});
    b.Op(127, {t_f32, neg_cy, cy});          // OpFNegate (Vulkan Y-flip)
    b.Op(80, {t_v4, clip_vk, cx, neg_cy, cz, cw});
    b.Op(65, {t_out_v4, p_out_pos, v_out_pv, c_i0});
    b.Op(62, {p_out_pos, clip_vk});
    b.Op(62, {v_out_pos, pos});
    b.Op(62, {v_out_nrm, nrm});
    b.Op(62, {v_out_col, col});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// SPIR-V 1.3 Fragment Shader:
//   Exact Blinn-Phong directional + orbiting point light + hemisphere ambient + Reinhard + sqrt gamma 2.0
//   matching kLightingPsHlsl bit-for-math-op!
//   in loc 0: worldPos, loc 1: worldNorm, loc 2: baseColor (rgb=albedo, a=specMask)
//   UBO set 0 binding 0: struct { mat4 view_proj; vec4 camera_pos; vec4 dir_light_dir; vec4 dir_light_color; }
//   PushConstant:        struct { vec4 point_light_pos; vec4 point_light_color; vec4 material_params; }
//   out loc 0: vec4 finalColor
std::vector<uint32_t> BuildBlinnPhongFragmentSpirv() {
    SpvBuilder b;
    const uint32_t ext_glsl = b.Id();
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v3 = b.Id(), t_v4 = b.Id(), t_m4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_ubo = b.Id(), t_ptr_ubo = b.Id(), t_ptr_ubo_v4 = b.Id();
    const uint32_t t_pc = b.Id(), t_ptr_pc = b.Id(), t_ptr_pc_v4 = b.Id();
    const uint32_t v_in_pos = b.Id(), v_in_nrm = b.Id(), v_in_col = b.Id(), v_out_col = b.Id();
    const uint32_t v_ubo = b.Id(), v_pc = b.Id();
    const uint32_t c_i0 = b.Id(), c_i1 = b.Id(), c_i2 = b.Id(), c_i3 = b.Id();
    const uint32_t c_f0 = b.Id(), c_f1 = b.Id(), c_feps8 = b.Id(), c_feps6 = b.Id();
    const uint32_t c_f025 = b.Id(), c_f045 = b.Id(), c_f05 = b.Id(), c_f055 = b.Id();
    const uint32_t c_v3_zero = b.Id(), c_v3_one = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});  // OpCapability Shader
    {
        std::vector<uint32_t> imp = {ext_glsl};
        SpvBuilder::PushStr(imp, "GLSL.std.450");
        b.OpVec(11, imp);  // OpExtInstImport
    }
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {4, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_in_nrm, v_in_col, v_out_col});
        b.OpVec(15, ep);  // OpEntryPoint Fragment
    }
    b.Op(16, {fn_main, 7});  // OriginUpperLeft

    // Decorations
    b.Op(71, {v_in_pos, 30, 0});
    b.Op(71, {v_in_nrm, 30, 1});
    b.Op(71, {v_in_col, 30, 2});
    b.Op(71, {v_out_col, 30, 0});

    b.Op(71, {t_ubo, 2});
    b.Op(72, {t_ubo, 0, 5});
    b.Op(72, {t_ubo, 0, 7, 16});
    b.Op(72, {t_ubo, 0, 35, 0});
    b.Op(72, {t_ubo, 1, 35, 64});
    b.Op(72, {t_ubo, 2, 35, 80});
    b.Op(72, {t_ubo, 3, 35, 96});
    b.Op(71, {v_ubo, 34, 0});
    b.Op(71, {v_ubo, 33, 0});

    b.Op(71, {t_pc, 2});
    b.Op(72, {t_pc, 0, 35, 0});
    b.Op(72, {t_pc, 1, 35, 16});
    b.Op(72, {t_pc, 2, 35, 32});

    // Types
    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v3, t_f32, 3});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(24, {t_m4, t_v4, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_ubo, t_m4, t_v4, t_v4, t_v4});
    b.Op(32, {t_ptr_ubo, 2, t_ubo});
    b.Op(32, {t_ptr_ubo_v4, 2, t_v4});
    b.Op(30, {t_pc, t_v4, t_v4, t_v4});
    b.Op(32, {t_ptr_pc, 9, t_pc});
    b.Op(32, {t_ptr_pc_v4, 9, t_v4});

    // Constants
    b.Op(43, {t_i32, c_i0, 0});
    b.Op(43, {t_i32, c_i1, 1});
    b.Op(43, {t_i32, c_i2, 2});
    b.Op(43, {t_i32, c_i3, 3});
    b.Op(43, {t_f32, c_f0, F32Bits(0.0f)});
    b.Op(43, {t_f32, c_f1, F32Bits(1.0f)});
    b.Op(43, {t_f32, c_feps8, F32Bits(1e-8f)});
    b.Op(43, {t_f32, c_feps6, F32Bits(1e-6f)});
    b.Op(43, {t_f32, c_f025, F32Bits(0.25f)});
    b.Op(43, {t_f32, c_f045, F32Bits(0.45f)});
    b.Op(43, {t_f32, c_f05, F32Bits(0.5f)});
    b.Op(43, {t_f32, c_f055, F32Bits(0.55f)});
    b.Op(44, {t_v3, c_v3_zero, c_f0, c_f0, c_f0});
    b.Op(44, {t_v3, c_v3_one, c_f1, c_f1, c_f1});

    // Variables
    b.Op(59, {t_in_v4, v_in_pos, 1});
    b.Op(59, {t_in_v4, v_in_nrm, 1});
    b.Op(59, {t_in_v4, v_in_col, 1});
    b.Op(59, {t_out_v4, v_out_col, 3});
    b.Op(59, {t_ptr_ubo, v_ubo, 2});
    b.Op(59, {t_ptr_pc, v_pc, 9});

    // Helper lambdas emitting clean SPIR-V SSA instructions
    auto xyz = [&](uint32_t v4) -> uint32_t {
        const uint32_t r = b.Id();
        b.Op(79, {t_v3, r, v4, v4, 0, 1, 2});  // OpVectorShuffle
        return r;
    };
    auto comp = [&](uint32_t v, uint32_t idx) -> uint32_t {
        const uint32_t r = b.Id();
        b.Op(81, {t_f32, r, v, idx});
        return r;
    };
    auto safe_norm = [&](uint32_t v3) -> uint32_t {
        const uint32_t dd = b.Id(), dmax = b.Id(), len = b.Id(), inv = b.Id(), out = b.Id();
        b.Op(148, {t_f32, dd, v3, v3});                      // OpDot
        b.Op(12, {t_f32, dmax, ext_glsl, 40, dd, c_feps8});  // FMax
        b.Op(12, {t_f32, len, ext_glsl, 31, dmax});          // Sqrt
        b.Op(136, {t_f32, inv, c_f1, len});                  // OpFDiv
        b.Op(142, {t_v3, out, v3, inv});                     // OpVectorTimesScalar
        return out;
    };
    auto pow32 = [&](uint32_t x) -> uint32_t {
        const uint32_t s2 = b.Id(), s4 = b.Id(), s8 = b.Id(), s16 = b.Id(), s32 = b.Id();
        b.Op(133, {t_f32, s2, x, x});
        b.Op(133, {t_f32, s4, s2, s2});
        b.Op(133, {t_f32, s8, s4, s4});
        b.Op(133, {t_f32, s16, s8, s8});
        b.Op(133, {t_f32, s32, s16, s16});
        return s32;
    };

    // main()
    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});

    const uint32_t in_pos4 = b.Id(), in_nrm4 = b.Id(), in_col4 = b.Id();
    b.Op(61, {t_v4, in_pos4, v_in_pos});
    b.Op(61, {t_v4, in_nrm4, v_in_nrm});
    b.Op(61, {t_v4, in_col4, v_in_col});

    const uint32_t wpos = xyz(in_pos4);
    const uint32_t wnrm = xyz(in_nrm4);
    const uint32_t albedo = xyz(in_col4);
    const uint32_t spec_mask = comp(in_col4, 3);

    // Load UBO members
    const uint32_t p_cam = b.Id(), cam4 = b.Id();
    const uint32_t p_ddir = b.Id(), ddir4 = b.Id();
    const uint32_t p_dcol = b.Id(), dcol4 = b.Id();
    b.Op(65, {t_ptr_ubo_v4, p_cam, v_ubo, c_i1});  b.Op(61, {t_v4, cam4, p_cam});
    b.Op(65, {t_ptr_ubo_v4, p_ddir, v_ubo, c_i2}); b.Op(61, {t_v4, ddir4, p_ddir});
    b.Op(65, {t_ptr_ubo_v4, p_dcol, v_ubo, c_i3}); b.Op(61, {t_v4, dcol4, p_dcol});

    // Load Push Constants members
    const uint32_t p_ppos = b.Id(), ppos4 = b.Id();
    const uint32_t p_pcol = b.Id(), pcol4 = b.Id();
    const uint32_t p_mat = b.Id(), mat4 = b.Id();
    b.Op(65, {t_ptr_pc_v4, p_ppos, v_pc, c_i0}); b.Op(61, {t_v4, ppos4, p_ppos});
    b.Op(65, {t_ptr_pc_v4, p_pcol, v_pc, c_i1}); b.Op(61, {t_v4, pcol4, p_pcol});
    b.Op(65, {t_ptr_pc_v4, p_mat, v_pc, c_i2});  b.Op(61, {t_v4, mat4, p_mat});

    const uint32_t cam = xyz(cam4);
    const uint32_t ddir_raw = xyz(ddir4);
    const uint32_t dir_int = comp(ddir4, 3);
    const uint32_t dcol = xyz(dcol4);
    const uint32_t amb_str = comp(dcol4, 3);
    const uint32_t ppos = xyz(ppos4);
    const uint32_t pt_int = comp(ppos4, 3);
    const uint32_t pcol = xyz(pcol4);
    const uint32_t pt_quad = comp(pcol4, 3);
    const uint32_t mat_dspec = comp(mat4, 0);
    const uint32_t mat_pspec = comp(mat4, 1);

    // N = SafeNorm(wnrm), V = SafeNorm(cam - wpos)
    const uint32_t N = safe_norm(wnrm);
    const uint32_t to_cam = b.Id();
    b.Op(131, {t_v3, to_cam, cam, wpos});
    const uint32_t V = safe_norm(to_cam);

    // 1. Directional light
    const uint32_t Ld = safe_norm(ddir_raw);
    const uint32_t ndl_d_raw = b.Id(), ndl_d = b.Id();
    b.Op(148, {t_f32, ndl_d_raw, N, Ld});
    b.Op(12, {t_f32, ndl_d, ext_glsl, 40, ndl_d_raw, c_f0});
    const uint32_t hd_raw = b.Id();
    b.Op(129, {t_v3, hd_raw, Ld, V});
    const uint32_t Hd = safe_norm(hd_raw);
    const uint32_t ndh_d_raw = b.Id(), ndh_d = b.Id();
    b.Op(148, {t_f32, ndh_d_raw, N, Hd});
    b.Op(12, {t_f32, ndh_d, ext_glsl, 40, ndh_d_raw, c_f0});
    const uint32_t spec_d = pow32(ndh_d);

    const uint32_t alb_dcol = b.Id(), ndl_d_int = b.Id(), dir_diff = b.Id();
    b.Op(133, {t_v3, alb_dcol, albedo, dcol});
    b.Op(133, {t_f32, ndl_d_int, ndl_d, dir_int});
    b.Op(142, {t_v3, dir_diff, alb_dcol, ndl_d_int});

    const uint32_t sd_m = b.Id(), sd_mw = b.Id(), sd_mwi = b.Id(), dir_spec = b.Id();
    b.Op(133, {t_f32, sd_m, spec_d, spec_mask});
    b.Op(133, {t_f32, sd_mw, sd_m, mat_dspec});
    b.Op(133, {t_f32, sd_mwi, sd_mw, dir_int});
    b.Op(142, {t_v3, dir_spec, dcol, sd_mwi});

    // 2. Orbiting point light
    const uint32_t to_p = b.Id(), dist_sq = b.Id(), dist_sq_c = b.Id(), dist = b.Id(), inv_dist = b.Id(), Lp = b.Id();
    b.Op(131, {t_v3, to_p, ppos, wpos});
    b.Op(148, {t_f32, dist_sq, to_p, to_p});
    b.Op(12, {t_f32, dist_sq_c, ext_glsl, 40, dist_sq, c_feps6});
    b.Op(12, {t_f32, dist, ext_glsl, 31, dist_sq_c});
    b.Op(136, {t_f32, inv_dist, c_f1, dist});
    b.Op(142, {t_v3, Lp, to_p, inv_dist});

    const uint32_t lin_att = b.Id(), q_att = b.Id(), denom1 = b.Id(), denom = b.Id(), atten = b.Id();
    b.Op(133, {t_f32, lin_att, c_f025, dist});
    b.Op(133, {t_f32, q_att, pt_quad, dist_sq});
    b.Op(129, {t_f32, denom1, c_f1, lin_att});
    b.Op(129, {t_f32, denom, denom1, q_att});
    b.Op(136, {t_f32, atten, pt_int, denom});

    const uint32_t ndl_p_raw = b.Id(), ndl_p = b.Id();
    b.Op(148, {t_f32, ndl_p_raw, N, Lp});
    b.Op(12, {t_f32, ndl_p, ext_glsl, 40, ndl_p_raw, c_f0});
    const uint32_t hp_raw = b.Id();
    b.Op(129, {t_v3, hp_raw, Lp, V});
    const uint32_t Hp = safe_norm(hp_raw);
    const uint32_t ndh_p_raw = b.Id(), ndh_p = b.Id();
    b.Op(148, {t_f32, ndh_p_raw, N, Hp});
    b.Op(12, {t_f32, ndh_p, ext_glsl, 40, ndh_p_raw, c_f0});
    const uint32_t spec_p = pow32(ndh_p);

    const uint32_t alb_pcol = b.Id(), ndl_p_att = b.Id(), pt_diff = b.Id();
    b.Op(133, {t_v3, alb_pcol, albedo, pcol});
    b.Op(133, {t_f32, ndl_p_att, ndl_p, atten});
    b.Op(142, {t_v3, pt_diff, alb_pcol, ndl_p_att});

    const uint32_t sp_m = b.Id(), sp_mw = b.Id(), sp_mwa = b.Id(), pt_spec = b.Id();
    b.Op(133, {t_f32, sp_m, spec_p, spec_mask});
    b.Op(133, {t_f32, sp_mw, sp_m, mat_pspec});
    b.Op(133, {t_f32, sp_mwa, sp_mw, atten});
    b.Op(142, {t_v3, pt_spec, pcol, sp_mwa});

    // 3. Ambient + Combine + Reinhard + sqrt gamma 2.0
    const uint32_t ny = comp(N, 1);
    const uint32_t ny_h = b.Id(), hemi_raw = b.Id(), hemi = b.Id();
    const uint32_t hemi_s = b.Id(), hemi_term = b.Id(), amb_s = b.Id(), ambient = b.Id();
    b.Op(133, {t_f32, ny_h, ny, c_f05});
    b.Op(129, {t_f32, hemi_raw, ny_h, c_f05});
    b.Op(12, {t_f32, hemi, ext_glsl, 43, hemi_raw, c_f0, c_f1});  // FClamp
    b.Op(133, {t_f32, hemi_s, c_f045, hemi});
    b.Op(129, {t_f32, hemi_term, c_f055, hemi_s});
    b.Op(133, {t_f32, amb_s, amb_str, hemi_term});
    b.Op(142, {t_v3, ambient, albedo, amb_s});

    const uint32_t sum1 = b.Id(), sum2 = b.Id(), sum3 = b.Id(), lin_col = b.Id();
    b.Op(129, {t_v3, sum1, ambient, dir_diff});
    b.Op(129, {t_v3, sum2, sum1, dir_spec});
    b.Op(129, {t_v3, sum3, sum2, pt_diff});
    b.Op(129, {t_v3, lin_col, sum3, pt_spec});

    const uint32_t lin_p1 = b.Id(), mapped = b.Id(), clamped = b.Id(), gamma_rgb = b.Id();
    b.Op(129, {t_v3, lin_p1, lin_col, c_v3_one});
    b.Op(136, {t_v3, mapped, lin_col, lin_p1});
    b.Op(12, {t_v3, clamped, ext_glsl, 43, mapped, c_v3_zero, c_v3_one});
    b.Op(12, {t_v3, gamma_rgb, ext_glsl, 31, clamped});

    const uint32_t fr = comp(gamma_rgb, 0);
    const uint32_t fg = comp(gamma_rgb, 1);
    const uint32_t fb = comp(gamma_rgb, 2);
    const uint32_t final_rgba = b.Id();
    b.Op(80, {t_v4, final_rgba, fr, fg, fb, c_f1});
    b.Op(62, {v_out_col, final_rgba});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

void ValidateSpirvModule(const std::vector<uint32_t>& spv, const char* label) {
    openscarlett::SpirvValidationInfo info{};
    auto st = openscarlett::VulkanBackend::ValidateSpirvBinary(spv, &info);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] ValidateSpirvBinary(%s): %s\n", label, st.message.c_str());
    }
    OS_REQUIRE(st.ok());

    std::error_code ec;
    if (fs::exists(openscarlett::kDefaultSpirvValPath, ec)) {
        std::string diag;
        auto ext = openscarlett::ValidateSpirvExternal(
            spv, diag, openscarlett::kDefaultSpirvValPath, "vulkan1.2");
        if (!ext.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val(%s): %s\n", label, diag.c_str());
        }
        OS_REQUIRE(ext.ok());
    }
}

struct ImageParityStats {
    double rgb_mae = 0.0;
    uint32_t max_diff = 0;
    double cov_rel_diff = 0.0;
};

ImageParityStats CompareRgbAndRgba(const std::vector<uint8_t>& d3d12_rgb,
                                   const openscarlett::FrameBufferRgba8& vk_fb) {
    OS_REQUIRE(vk_fb.width == kWidth && vk_fb.height == kHeight);
    OS_REQUIRE(d3d12_rgb.size() == static_cast<size_t>(kWidth) * kHeight * 3u);
    OS_REQUIRE(vk_fb.pixels.size() == static_cast<size_t>(kWidth) * kHeight * 4u);

    uint64_t sum_abs = 0;
    uint32_t max_d = 0;
    uint32_t d3d_non_bg = 0;
    uint32_t vk_non_bg = 0;

    const size_t n_px = static_cast<size_t>(kWidth) * kHeight;
    for (size_t i = 0; i < n_px; ++i) {
        const uint8_t dr = d3d12_rgb[i * 3u + 0u];
        const uint8_t dg = d3d12_rgb[i * 3u + 1u];
        const uint8_t db = d3d12_rgb[i * 3u + 2u];
        const uint8_t vr = vk_fb.pixels[i * 4u + 0u];
        const uint8_t vg = vk_fb.pixels[i * 4u + 1u];
        const uint8_t vb = vk_fb.pixels[i * 4u + 2u];

        if (!(dr == kBgR && dg == kBgG && db == kBgB)) ++d3d_non_bg;
        if (!(vr == kBgR && vg == kBgG && vb == kBgB)) ++vk_non_bg;

        const uint32_t cr = static_cast<uint32_t>(std::abs(static_cast<int32_t>(dr) - static_cast<int32_t>(vr)));
        const uint32_t cg = static_cast<uint32_t>(std::abs(static_cast<int32_t>(dg) - static_cast<int32_t>(vg)));
        const uint32_t cb = static_cast<uint32_t>(std::abs(static_cast<int32_t>(db) - static_cast<int32_t>(vb)));
        sum_abs += cr + cg + cb;
        max_d = std::max(max_d, std::max(cr, std::max(cg, cb)));
    }

    ImageParityStats st{};
    st.rgb_mae = static_cast<double>(sum_abs) / static_cast<double>(n_px * 3u);
    st.max_diff = max_d;
    st.cov_rel_diff = std::abs(static_cast<double>(d3d_non_bg) - static_cast<double>(vk_non_bg)) /
                      std::max(1.0, static_cast<double>(d3d_non_bg));
    return st;
}

struct VulkanLightingSummary {
    std::string device_name;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    uint32_t api_patch = 0;
    bool executed_on_hw_icd = false;
    uint64_t frame_hashes[3]{};
    uint32_t specular_pixels = 0;
    uint32_t diffuse_pixels = 0;
    uint32_t non_bg_pixels = 0;
    ImageParityStats vs_d3d12_hw{};
    ImageParityStats vs_d3d12_warp{};
    ImageParityStats vs_vk_sw{};
    double vk_total_ms = 0.0;
};

VulkanLightingSummary RunLiveVulkanBlinnPhongVerification(
    const std::vector<uint8_t>* d3d12_hw_rgb,
    const std::vector<uint8_t>& d3d12_warp_rgb) {
    using namespace openscarlett;

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());
    const auto info = vk.GetDeviceInfo();

    VulkanBackend sw;
    OS_REQUIRE(sw.InitializeSoftwareFallback().ok());

    const auto vs_spv = BuildBlinnPhongVertexSpirv();
    const auto ps_spv = BuildBlinnPhongFragmentSpirv();
    ValidateSpirvModule(vs_spv, "SimpleLighting_VS");
    ValidateSpirvModule(ps_spv, "SimpleLighting_PS");

    std::vector<VkLightingVertex> verts;
    std::vector<uint32_t> indices;
    BuildSharedLightingScene(verts, indices);

    const Vec3 kCamPos{0.0f, 1.05f, 3.60f};
    const Vec3 kLookTarget{0.0f, -0.08f, 0.0f};
    const Vec3 kUp{0.0f, 1.0f, 0.0f};
    const Mat4 view = MakeLookAtRH(kCamPos, kLookTarget, kUp);
    const Mat4 proj = MakePerspectiveFovRH(45.0f * kPi / 180.0f, 1.0f, 0.2f, 20.0f);
    const Mat4 view_proj = MultiplyMat4(view, proj);

    const Vec3 kPointLightPos[3] = {
        {-2.10f, 0.85f, 1.35f},
        { 0.00f, 1.35f, 1.85f},
        { 1.55f, 0.90f, 2.05f},
    };
    const Vec3 kDirLightDirs[3] = {
        VNorm({-0.55f, 0.85f, 0.62f}),
        VNorm({ 0.10f, 0.92f, 0.68f}),
        VNorm({ 0.62f, 0.80f, 0.60f}),
    };

    VulkanGraphicsDrawDesc draw{};
    draw.vertex_spirv = vs_spv;
    draw.fragment_spirv = ps_spv;
    draw.width = kWidth;
    draw.height = kHeight;
    draw.color_format = VulkanImageFormat::Rgba8Unorm;
    draw.enable_depth = true;
    draw.enable_msaa_4x = false;
    draw.depth_write = true;
    draw.clear_color[0] = static_cast<float>(kBgR) / 255.0f;
    draw.clear_color[1] = static_cast<float>(kBgG) / 255.0f;
    draw.clear_color[2] = static_cast<float>(kBgB) / 255.0f;
    draw.clear_color[3] = 1.0f;
    draw.clear_depth = 1.0f;

    draw.vertex_bytes.resize(verts.size() * sizeof(VkLightingVertex));
    std::memcpy(draw.vertex_bytes.data(), verts.data(), draw.vertex_bytes.size());
    draw.vertex_stride = sizeof(VkLightingVertex);
    draw.vertex_attributes = {
        {0, 0, VulkanVertexFormat::R32G32B32A32Sfloat, 0},
        {1, 0, VulkanVertexFormat::R32G32B32A32Sfloat, 16},
        {2, 0, VulkanVertexFormat::R32G32B32A32Sfloat, 32},
    };
    draw.indices = indices;

    VulkanLightingSummary summary{};
    summary.device_name = info.device_name;
    summary.api_major = info.api_major;
    summary.api_minor = info.api_minor;
    summary.api_patch = info.api_patch;

    VulkanGraphicsDrawResult vk_frame2{};
    const auto t0 = std::chrono::steady_clock::now();

    for (uint32_t frame = 0; frame < 3u; ++frame) {
        VkLightingUbo ubo{};
        std::memcpy(ubo.view_proj, view_proj.m, sizeof(ubo.view_proj));
        ubo.camera_pos[0] = kCamPos.x;
        ubo.camera_pos[1] = kCamPos.y;
        ubo.camera_pos[2] = kCamPos.z;
        ubo.camera_pos[3] = 1.0f;
        ubo.dir_light_dir[0] = kDirLightDirs[frame].x;
        ubo.dir_light_dir[1] = kDirLightDirs[frame].y;
        ubo.dir_light_dir[2] = kDirLightDirs[frame].z;
        ubo.dir_light_dir[3] = 1.65f;
        ubo.dir_light_color[0] = 1.00f;
        ubo.dir_light_color[1] = 0.96f;
        ubo.dir_light_color[2] = 0.90f;
        ubo.dir_light_color[3] = 0.045f;

        VkLightingPushConstants pc{};
        pc.point_light_pos[0] = kPointLightPos[frame].x;
        pc.point_light_pos[1] = kPointLightPos[frame].y;
        pc.point_light_pos[2] = kPointLightPos[frame].z;
        pc.point_light_pos[3] = 2.60f;
        pc.point_light_color[0] = 0.85f;
        pc.point_light_color[1] = 0.94f;
        pc.point_light_color[2] = 1.00f;
        pc.point_light_color[3] = 0.22f;
        pc.material_params[0] = 1.25f;
        pc.material_params[1] = 1.35f;

        VulkanDescriptorBinding b_ubo{};
        b_ubo.binding = 0;
        b_ubo.kind = VulkanDescriptorKind::UniformBuffer;
        b_ubo.buffer_data.resize(sizeof(ubo));
        std::memcpy(b_ubo.buffer_data.data(), &ubo, sizeof(ubo));
        draw.descriptors = {b_ubo};

        draw.push_constants.resize(sizeof(pc));
        std::memcpy(draw.push_constants.data(), &pc, sizeof(pc));

        VulkanGraphicsDrawResult res{};
        auto st = vk.ExecuteGraphicsDraw(draw, res);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Vulkan ExecuteGraphicsDraw(frame %u): %s\n",
                         frame, st.message.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.used_depth_buffer);
        OS_REQUIRE(res.fnv1a_hash != 0ull);
        summary.frame_hashes[frame] = res.fnv1a_hash;
        if (frame == 2u) {
            summary.executed_on_hw_icd = res.executed_on_vulkan_icd;
            vk_frame2 = std::move(res);
        }
    }

    summary.vk_total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // All 3 animated Vulkan frames must have distinct hashes
    OS_REQUIRE(summary.frame_hashes[0] != summary.frame_hashes[1]);
    OS_REQUIRE(summary.frame_hashes[1] != summary.frame_hashes[2]);
    OS_REQUIRE(summary.frame_hashes[0] != summary.frame_hashes[2]);

    // Verify Blinn-Phong specular highlights & depth occlusion on live Vulkan Frame 2
    const auto& fb = vk_frame2.framebuffer;
    OS_REQUIRE(fb.pixels[0] == kBgR && fb.pixels[1] == kBgG && fb.pixels[2] == kBgB);

    int32_t cube_px = 0, cube_py = 0;
    OS_REQUIRE(ProjectWorldToPixel({-0.18f, 0.08f, 1.08f}, view_proj, kWidth, kHeight, cube_px, cube_py));
    double cube_r = 0.0, cube_b = 0.0;
    uint32_t cube_n = 0;
    for (int32_t dy = -3; dy <= 3; ++dy) {
        for (int32_t dx = -3; dx <= 3; ++dx) {
            const uint32_t sx = static_cast<uint32_t>(std::clamp(cube_px + dx, 0, static_cast<int32_t>(kWidth) - 1));
            const uint32_t sy = static_cast<uint32_t>(std::clamp(cube_py + dy, 0, static_cast<int32_t>(kHeight) - 1));
            const size_t off = (static_cast<size_t>(sy) * kWidth + sx) * 4u;
            cube_r += fb.pixels[off + 0u];
            cube_b += fb.pixels[off + 2u];
            ++cube_n;
        }
    }
    OS_REQUIRE((cube_b / cube_n) > (cube_r / cube_n) + 30.0 && (cube_b / cube_n) > 90.0);

    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
        const uint8_t r = fb.pixels[i * 4u + 0u];
        const uint8_t g = fb.pixels[i * 4u + 1u];
        const uint8_t b = fb.pixels[i * 4u + 2u];
        if (r == kBgR && g == kBgG && b == kBgB) continue;
        ++summary.non_bg_pixels;
        const uint32_t luma = (static_cast<uint32_t>(r) * 77u +
                               static_cast<uint32_t>(g) * 150u +
                               static_cast<uint32_t>(b) * 29u) >> 8;
        const uint8_t max_c = std::max(r, std::max(g, b));
        if (max_c >= 215u && luma >= 185u) ++summary.specular_pixels;
        else if (luma >= 65u) ++summary.diffuse_pixels;
    }
    OS_REQUIRE(summary.specular_pixels >= 40u);
    OS_REQUIRE(summary.diffuse_pixels >= 4000u);

    // Cross-check Vulkan Frame 2 vs D3D12 WARP (and D3D12 HW when present!)
    summary.vs_d3d12_warp = CompareRgbAndRgba(d3d12_warp_rgb, fb);
    if (summary.vs_d3d12_warp.rgb_mae >= 3.0 || summary.vs_d3d12_warp.cov_rel_diff >= 0.02) {
        std::fprintf(stderr,
                     "[FAIL] Vulkan vs D3D12-WARP parity: MAE=%.4f maxDiff=%u covRelDiff=%.5f\n",
                     summary.vs_d3d12_warp.rgb_mae, summary.vs_d3d12_warp.max_diff,
                     summary.vs_d3d12_warp.cov_rel_diff);
        OS_REQUIRE(false);
    }

    if (d3d12_hw_rgb != nullptr) {
        summary.vs_d3d12_hw = CompareRgbAndRgba(*d3d12_hw_rgb, fb);
        if (summary.vs_d3d12_hw.rgb_mae >= 3.0 || summary.vs_d3d12_hw.cov_rel_diff >= 0.02) {
            std::fprintf(stderr,
                         "[FAIL] Vulkan vs D3D12-HW parity: MAE=%.4f maxDiff=%u covRelDiff=%.5f\n",
                         summary.vs_d3d12_hw.rgb_mae, summary.vs_d3d12_hw.max_diff,
                         summary.vs_d3d12_hw.cov_rel_diff);
            OS_REQUIRE(false);
        }
    }

    // Also verify Vulkan Software Fallback renders the exact same Frame 2 with tight parity!
    VulkanGraphicsDrawResult sw_frame2{};
    OS_REQUIRE(sw.ExecuteGraphicsDraw(draw, sw_frame2).ok());
    OS_REQUIRE(!sw_frame2.executed_on_vulkan_icd);
    OS_REQUIRE(sw_frame2.used_depth_buffer);
    std::vector<uint8_t> vk_hw_rgb(static_cast<size_t>(kWidth) * kHeight * 3u);
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
        vk_hw_rgb[i * 3u + 0u] = fb.pixels[i * 4u + 0u];
        vk_hw_rgb[i * 3u + 1u] = fb.pixels[i * 4u + 1u];
        vk_hw_rgb[i * 3u + 2u] = fb.pixels[i * 4u + 2u];
    }
    summary.vs_vk_sw = CompareRgbAndRgba(vk_hw_rgb, sw_frame2.framebuffer);
    if (summary.vs_vk_sw.rgb_mae >= 3.0 || summary.vs_vk_sw.cov_rel_diff >= 0.02) {
        std::fprintf(stderr,
                     "[FAIL] Vulkan HW vs Vulkan SW Fallback parity: MAE=%.4f maxDiff=%u covRelDiff=%.5f\n",
                     summary.vs_vk_sw.rgb_mae, summary.vs_vk_sw.max_diff, summary.vs_vk_sw.cov_rel_diff);
        OS_REQUIRE(false);
    }

    sw.Shutdown();
    vk.Shutdown();
    return summary;
}

}  // namespace

int main() {
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_TITLE_ID", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_GPU_BACKEND", nullptr);

    const fs::path build_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = build_dir / L"openscarlett.exe";
    const fs::path dxc_exe = FindDxcExe();
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(build_dir / L"openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(build_dir / L"gdk_simple_lighting.exe"));
    OS_REQUIRE(!dxc_exe.empty());

    // Probe hardware D3D12 GPU availability so WARP-only CI runners pass gracefully
    // while hardware hosts (e.g. NVIDIA GTX 1650 Ti) strictly verify BOTH HW and WARP!
    bool has_hw_gpu = false;
    {
        std::vector<openscarlett::D3D12AdapterInfo> adapters;
        if (openscarlett::EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp) {
                    has_hw_gpu = true;
                    break;
                }
            }
        }
    }

    const fs::path stage_root = ResolveScratchDir(L"openscarlett_wave4_simple_lighting");
    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(stage_root, ec);
    OS_REQUIRE(fs::exists(stage_root));

    // Stage zero-DLL package directory under %TEMP%
    const fs::path pkg_dir = stage_root / L"SimpleLighting_Package";
    const fs::path hlsl_dir = stage_root / L"_hlsl_src";
    fs::create_directories(pkg_dir, ec);
    fs::create_directories(hlsl_dir, ec);

    const fs::path staged_exe = pkg_dir / L"gdk_simple_lighting.exe";
    fs::copy_file(build_dir / L"gdk_simple_lighting.exe", staged_exe,
                  fs::copy_options::overwrite_existing);
    VerifyStandaloneXboxPeImports(staged_exe, {
        "d3d12_xs.dll",
        "xgameruntime.dll",
        "XAudio2_9.dll",
        "pixevt.dll",
    });

    WriteBinaryFile(pkg_dir / L"MicrosoftGame.Config",
                    kMicrosoftGameConfigXml, std::strlen(kMicrosoftGameConfigXml));

    WriteBinaryFile(hlsl_dir / L"LightingVS.hlsl", kLightingVsHlsl, std::strlen(kLightingVsHlsl));
    WriteBinaryFile(hlsl_dir / L"LightingPS.hlsl", kLightingPsHlsl, std::strlen(kLightingPsHlsl));
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"LightingVS.hlsl", pkg_dir / L"LightingVS.cso", L"vs_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"LightingPS.hlsl", pkg_dir / L"LightingPS.cso", L"ps_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"LightingVS.hlsl", pkg_dir / L"LightingVS_sm62.cso", L"vs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"LightingPS.hlsl", pkg_dir / L"LightingPS_sm62.cso", L"ps_6_2", L"main");

    // Pre-launch zero-DLL byte-exact snapshot of package directory
    const auto pkg_before = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_before.size() == 6u);  // exe + MicrosoftGame.Config + 4 CSOs

    // 1. Live D3D12 Hardware GPU Run (when present)
    RunReport hw_report{};
    if (has_hw_gpu) {
        VerifySimpleLightingRun("d3d12_hw", cli_exe, pkg_dir, build_dir, stage_root,
                                pkg_before, L"", L"", "d3d12_default", hw_report);
    }

    // 2. D3D12 WARP Run
    RunReport warp_report{};
    VerifySimpleLightingRun("d3d12_warp", cli_exe, pkg_dir, build_dir, stage_root,
                            pkg_before, L"", L"--warp", "d3d12_default", warp_report);

    // 3. CLI `--backend=vulkan` Routing Run
    RunReport cli_vk_report{};
    VerifySimpleLightingRun("cli_backend_vulkan", cli_exe, pkg_dir, build_dir, stage_root,
                            pkg_before, L"--backend=vulkan", has_hw_gpu ? L"" : L"--warp",
                            "vulkan", cli_vk_report);

    // Compare D3D12 HW vs D3D12 WARP Frame 2 images within tight tolerance
    double hw_warp_mae = 0.0;
    uint32_t hw_warp_max_diff = 0;
    double hw_warp_cov_diff = 0.0;
    if (has_hw_gpu) {
        OS_REQUIRE(hw_report.ppm_rgb.size() == warp_report.ppm_rgb.size());
        uint64_t sum_abs = 0;
        for (size_t i = 0; i < hw_report.ppm_rgb.size(); ++i) {
            const uint32_t d = static_cast<uint32_t>(std::abs(
                static_cast<int32_t>(hw_report.ppm_rgb[i]) -
                static_cast<int32_t>(warp_report.ppm_rgb[i])));
            sum_abs += d;
            if (d > hw_warp_max_diff) hw_warp_max_diff = d;
        }
        hw_warp_mae = static_cast<double>(sum_abs) / static_cast<double>(hw_report.ppm_rgb.size());
        const double hw_non_bg = std::stod(hw_report.metrics.at("non_bg_pixels"));
        const double wp_non_bg = std::stod(warp_report.metrics.at("non_bg_pixels"));
        hw_warp_cov_diff = std::abs(hw_non_bg - wp_non_bg) / std::max(1.0, hw_non_bg);

        if (hw_warp_mae >= 2.5 || hw_warp_cov_diff >= 0.02) {
            std::fprintf(stderr,
                         "[test_gdk_simple_lighting] D3D12 HW vs WARP mismatch: MAE=%.4f maxDiff=%u covRelDiff=%.5f\n",
                         hw_warp_mae, hw_warp_max_diff, hw_warp_cov_diff);
            OS_REQUIRE(false);
        }
    }

    // 4. Live Vulkan 1.4 Blinn-Phong Scene Render & Cross-Backend D3D12 Parity (+ SW Fallback)
    const VulkanLightingSummary vk_sum = RunLiveVulkanBlinnPhongVerification(
        has_hw_gpu ? &hw_report.ppm_rgb : nullptr,
        warp_report.ppm_rgb);

    // Crisp summary output
    std::printf("\n===================================================================================================================\n");
    std::printf(" OpenScarlett Wave 4 - SimpleLighting (3D Blinn-Phong + XDisplay + X3DAudio/FX + D3D12/WARP/Vulkan 1.4 Parity)\n");
    std::printf("===================================================================================================================\n");
    auto print_report = [](const RunReport& r) {
        const auto& m = r.metrics;
        std::printf(" Mode        : %s (%s | env=%s)\n",
                    r.mode.c_str(), m.at("adapter").c_str(), m.at("gpu_backend_env").c_str());
        std::printf(" Geometry    : %s verts / %s indices / %s tris | %sx%s D32_FLOAT\n",
                    m.at("vertex_count").c_str(), m.at("index_count").c_str(),
                    m.at("triangle_count").c_str(), m.at("width").c_str(), m.at("height").c_str());
        std::printf(" Frame Hashes: F0=%s  F1=%s  F2=%s\n",
                    m.at("frame0_fnv1a").c_str(), m.at("frame1_fnv1a").c_str(), m.at("frame2_fnv1a").c_str());
        std::printf(" Lighting Px : Spec=%s | Diff=%s | Shad=%s | Cube=%s | Sphere=%s | Colors=%s\n",
                    m.at("specular_pixels").c_str(), m.at("diffuse_pixels").c_str(),
                    m.at("shadow_pixels").c_str(), m.at("front_cube_pixels").c_str(),
                    m.at("back_sphere_pixels").c_str(), m.at("unique_colors").c_str());
        std::printf(" XDisplay/3D : HDR=%s (max=%s nits) | X3D Pan F0(%s/%s)->F2(%s/%s) | Dop(%s->%s) | Rev=%s%%\n",
                    m.at("xdisplay_hdr_result").c_str(), m.at("xdisplay_max_nits").c_str(),
                    m.at("x3d_f0_gain_l").c_str(), m.at("x3d_f0_gain_r").c_str(),
                    m.at("x3d_f2_gain_l").c_str(), m.at("x3d_f2_gain_r").c_str(),
                    m.at("x3d_f0_doppler").c_str(), m.at("x3d_f2_doppler").c_str(),
                    m.at("xaudio2fx_reverb_wetdry").c_str());
        std::printf(" Telemetry   : %llu calls across %zu symbols / %zu modules (not_impl=%llu) | GPU=%s ms | Wall=%.1f ms\n",
                    static_cast<unsigned long long>(r.telemetry.total_calls),
                    r.telemetry.rows.size(),
                    r.telemetry.modules.size(),
                    static_cast<unsigned long long>(r.telemetry.total_not_impl),
                    m.at("gpu_total_ms").c_str(),
                    r.wall_ms);
        std::printf("-------------------------------------------------------------------------------------------------------------------\n");
    };
    if (has_hw_gpu) print_report(hw_report);
    print_report(warp_report);
    print_report(cli_vk_report);

    std::printf(" Vulkan Live : %s (API %u.%u.%u, hw=%s) | Spec=%u Diff=%u NonBg=%u | %.2f ms\n",
                vk_sum.device_name.c_str(),
                vk_sum.api_major, vk_sum.api_minor, vk_sum.api_patch,
                vk_sum.executed_on_hw_icd ? "Vulkan-ICD" : "SW-Fallback",
                vk_sum.specular_pixels, vk_sum.diffuse_pixels, vk_sum.non_bg_pixels,
                vk_sum.vk_total_ms);
    std::printf(" Vk Hashes   : F0=0x%016llX  F1=0x%016llX  F2=0x%016llX\n",
                static_cast<unsigned long long>(vk_sum.frame_hashes[0]),
                static_cast<unsigned long long>(vk_sum.frame_hashes[1]),
                static_cast<unsigned long long>(vk_sum.frame_hashes[2]));
    if (has_hw_gpu) {
        std::printf(" Parity      : D3D12 HW vs WARP MAE=%.4f (max=%u) | Vulkan vs D3D12-HW MAE=%.4f (max=%u) | Vulkan vs WARP MAE=%.4f | Vk HW vs SW MAE=%.4f\n",
                    hw_warp_mae, hw_warp_max_diff,
                    vk_sum.vs_d3d12_hw.rgb_mae, vk_sum.vs_d3d12_hw.max_diff,
                    vk_sum.vs_d3d12_warp.rgb_mae,
                    vk_sum.vs_vk_sw.rgb_mae);
    } else {
        std::printf(" Parity      : Vulkan vs D3D12-WARP MAE=%.4f (max=%u) | Vk vs SW MAE=%.4f\n",
                    vk_sum.vs_d3d12_warp.rgb_mae, vk_sum.vs_d3d12_warp.max_diff,
                    vk_sum.vs_vk_sw.rgb_mae);
    }
    std::printf("===================================================================================================================\n");
    std::printf("[PASS] test_gdk_simple_lighting completed (zero-DLL pkg, D3D12 HW+WARP, XDisplay, X3DAudio/FX, Vulkan 1.4 parity)!\n");
    return 0;
}
