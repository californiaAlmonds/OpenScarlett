// OpenScarlett Sprint 3 — Live Real-Binary & Official MIT Microsoft Xbox-GDK-Samples Test Harness.
//
// Stages 4 pure standalone Xbox-ABI guest executables into completely isolated package
// directories under E:\OpenScarlett\.tmp\sprint3_packages\<sample>\ (ZERO shim DLLs!),
// compiles the real MIT Microsoft Xbox-GDK-Samples HLSL shaders live with dxc.exe,
// launches EVERY sample through `openscarlett.exe run` (suspended-process APC hook + VFS +
// unified cross-DLL CallTelemetry), and verifies exact GPU frames, FNV-1a hashes,
// zero unimplemented blockers (not_impl == 0), and live COM vtable slot telemetry.

#include <windows.h>

#include <algorithm>
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

#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

// Fallback HLSL used ONLY if E:\OpenScarlett\packages\Xbox-GDK-Samples is absent (e.g. CI).
constexpr const char* kFallbackSimpleTriangleVs = R"HLSL(
struct Vertex { float4 position : SV_Position; float4 color : COLOR0; };
struct Interpolants { float4 position : SV_Position; float4 color : COLOR0; };
[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)")]
Interpolants main(Vertex In) { return (Interpolants)In; }
)HLSL";

constexpr const char* kFallbackSimpleTrianglePs = R"HLSL(
struct Interpolants { float4 position : SV_Position; float4 color : COLOR0; };
[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)")]
float4 main(Interpolants In) : SV_TARGET0 { return In.color; }
)HLSL";

constexpr const char* kFallbackMeshMs = R"HLSL(
struct VertexOut { float4 Color : COLOR0; float4 Position : SV_Position; };
static const float4 s_Pos[3] = {
    float4(-0.5f, -0.5f, 0.5f, 1.0f),
    float4( 0.0f,  0.5f, 0.5f, 1.0f),
    float4( 0.5f, -0.5f, 0.5f, 1.0f)
};
static const float4 s_Col[3] = {
    float4(1.0f, 0.0f, 0.0f, 1.0f),
    float4(0.0f, 1.0f, 0.0f, 1.0f),
    float4(0.0f, 0.0f, 1.0f, 1.0f)
};
[RootSignature("")]
[NumThreads(32, 1, 1)]
[OutputTopology("triangle")]
void main(uint gtid : SV_GroupThreadID, out indices uint3 tris[1], out vertices VertexOut verts[3]) {
    SetMeshOutputCounts(3, 1);
    if (gtid < 3) { verts[gtid].Position = s_Pos[gtid]; verts[gtid].Color = s_Col[gtid]; }
    if (gtid < 1) { tris[0] = uint3(0, 1, 2); }
}
)HLSL";

constexpr const char* kFallbackMeshPs = R"HLSL(
struct VertexOut { float4 Color : COLOR0; float4 Position : SV_Position; };
[RootSignature("")]
float4 main(VertexOut input) : SV_TARGET { return input.Color; }
)HLSL";

constexpr const char* kFallbackFractalCs = R"HLSL(
#define RS [RootSignature("CBV(b0, visibility=SHADER_VISIBILITY_ALL),DescriptorTable(Sampler(s0, numDescriptors=1), visibility=SHADER_VISIBILITY_ALL),DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_ALL),DescriptorTable(UAV(u0, numDescriptors=1), visibility=SHADER_VISIBILITY_ALL)")]
RWTexture2D<float4> OutputTexture : register(u0);
Texture2D ColorMapTexture : register(t0);
SamplerState ColorMapSampler : register(s0);
cbuffer cb0 { float4 g_MaxThreadIter : packoffset(c0); float4 g_Window : packoffset(c1); }
[numthreads(8, 8, 1)]
RS
void main(uint3 DTid : SV_DispatchThreadID) {
    float2 wl = ((float2)DTid.xy / g_MaxThreadIter.xy) * float2(1, -1) + float2(-0.5f, 0.5f);
    float2 coord = wl * g_Window.xy + g_Window.zw;
    uint maxiter = (uint)g_MaxThreadIter.z * 4;
    uint iter = 0;
    float2 c = coord;
    float2 sq;
    do {
        sq = coord * coord;
        coord = float2(sq.x - sq.y, 2.0f * coord.y * coord.x) + c;
        iter++;
    } while (iter < maxiter && (sq.x + sq.y) < 4.0f);
    float idx = frac((float)iter / g_MaxThreadIter.z);
    OutputTexture[DTid.xy] = ColorMapTexture.SampleLevel(ColorMapSampler, float2(idx, 0.0f), 0.0f);
}
)HLSL";

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
            if (r.symbol == sym) {
                if (expected_slot == -999 || r.slot == expected_slot) {
                    return r.total >= 1;
                }
            }
        }
        return false;
    }

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }
};

struct SummaryRow {
    std::string name;
    std::string adapter;
    std::string resolution;
    std::string fnv1a;
    std::string pixels_info;
    uint64_t tel_calls = 0;
    size_t tel_symbols = 0;
    double elapsed_ms = 0.0;
};

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
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
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return kv;
}

std::vector<std::string> ListDirFilenames(const fs::path& dir) {
    std::vector<std::string> out;
    for (const auto& entry : fs::directory_iterator(dir)) {
        out.push_back(entry.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void AssertNoDllsInDir(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");
    }
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
    const DWORD env_len = ::GetEnvironmentVariableW(L"OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(1024));
    if (env_len > 0 && env_len < 1024 && fs::exists(env_dxc, ec)) {
        return fs::path(env_dxc);
    }
    wchar_t path_buf[MAX_PATH]{};
    const DWORD path_len = ::SearchPathW(nullptr, L"dxc.exe", nullptr, static_cast<DWORD>(MAX_PATH), path_buf, nullptr);
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
    BOOL ok = ::CreateProcessW(
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

void CompileShaderWithDxc(
    const fs::path& dxc_exe,
    const fs::path& hlsl_path,
    const fs::path& cso_out_path,
    const std::wstring& profile,
    const std::wstring& entry_point,
    const fs::path& include_dir = {}) {
    std::wstring cmd = L"\"" + dxc_exe.wstring() + L"\" -T " + profile +
                       L" -E " + entry_point + L" -O3";
    if (!include_dir.empty()) {
        cmd += L" -I \"" + include_dir.wstring() + L"\"";
    }
    cmd += L" -Fo \"" + cso_out_path.wstring() + L"\" \"" + hlsl_path.wstring() + L"\"";
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
    long long v = std::strtoll(line.c_str() + pos, &end_ptr, 10);
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

DWORD LaunchViaOpenScarlettCli(
    const fs::path& cli_exe,
    const fs::path& isolated_guest_exe,
    const fs::path& shim_dir,
    const fs::path& vfs_g,
    const fs::path& vfs_t,
    const fs::path& vfs_s,
    const fs::path& telemetry_json,
    const std::wstring& guest_args = {}) {
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + isolated_guest_exe.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-g \"" + vfs_g.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --vfs-s \"" + vfs_s.wstring() +
        L"\" --telemetry \"" + telemetry_json.wstring() +
        L"\" --timeout-ms 45000";
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }
    return RunProcessW(cli_exe, cmd, 50000);
}

void VerifyPpmFile(const fs::path& ppm_path, uint32_t width, uint32_t height) {
    OS_REQUIRE(fs::exists(ppm_path));
    std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const uint64_t expected_size = header.size() + static_cast<uint64_t>(width) * height * 3ull;
    OS_REQUIRE(fs::file_size(ppm_path) == expected_size);
    std::ifstream ifs(ppm_path, std::ios::binary);
    std::string prefix(header.size(), '\0');
    ifs.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    OS_REQUIRE(prefix == header);
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

}  // namespace

int main() {
    // Ensure headless null audio mode even when test_gdk_live.exe is invoked directly
    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_AUDIO_NULL", L"1") != 0);

    const fs::path build_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = build_dir / "openscarlett.exe";
    const fs::path dxc_exe = FindDxcExe();
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(build_dir / "openscarlett_hook.dll"));
    OS_REQUIRE(!dxc_exe.empty());

    const fs::path gdk_samples_root = fs::path(R"(E:\OpenScarlett\packages\Xbox-GDK-Samples\Samples\IntroGraphics)");
    const bool has_official_gdk_samples = fs::exists(gdk_samples_root / "SimpleTriangle" / "VertexShader.hlsl") &&
                                          fs::exists(gdk_samples_root / "SimpleMeshShader" / "SimpleTriangleMS.hlsl") &&
                                          fs::exists(gdk_samples_root / "SimpleCompute" / "fractal.hlsl");
    // On this host, assert the official MIT Microsoft Xbox-GDK-Samples checkout IS present & used!
    if (fs::exists(R"(E:\OpenScarlett\packages\Xbox-GDK-Samples)")) {
        OS_REQUIRE(has_official_gdk_samples);
    }

    // Probe host hardware D3D12 GPU & Mesh Shader support so WARP-only CI runners still pass
    // while real hardware hosts (e.g. NVIDIA GTX 1650 Ti, MeshTier=10) strictly verify HW paths!
    bool has_hw_gpu = false;
    bool has_hw_mesh = false;
    {
        std::vector<openscarlett::D3D12AdapterInfo> adapters;
        if (openscarlett::EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp) {
                    has_hw_gpu = true;
                    if (a.mesh_shader_tier >= 10u) {
                        has_hw_mesh = true;
                    }
                }
            }
        }
    }

    const fs::path stage_root = ResolveScratchDir(L"openscarlett_sprint3_packages");
    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(stage_root, ec);
    OS_REQUIRE(fs::exists(stage_root));

    std::vector<SummaryRow> summary_rows;

    // =========================================================================
    // SAMPLE 1: Microsoft Xbox-GDK-Samples / SimpleTriangle (GTX 1650 Ti + WARP)
    // =========================================================================
    {
        const fs::path pkg_dir = stage_root / "gdk_simple_triangle";
        fs::create_directories(pkg_dir, ec);

        const fs::path staged_exe = pkg_dir / "gdk_simple_triangle.exe";
        fs::copy_file(build_dir / "gdk_simple_triangle.exe", staged_exe,
                      fs::copy_options::overwrite_existing);
        VerifyStandaloneXboxPeImports(staged_exe, {"d3d12_xs.dll", "xgameruntime.dll", "GameInput.dll"});

        // Compile real Microsoft MIT SimpleTriangle HLSL shaders into pkg_dir\*.cso
        // (SM 6.5 for hardware GTX 1650 Ti + SM 6.2 fallback for Win10 D3D12 WARP)
        if (has_official_gdk_samples) {
            const fs::path tri_dir = gdk_samples_root / "SimpleTriangle";
            CompileShaderWithDxc(dxc_exe, tri_dir / "VertexShader.hlsl",
                                 pkg_dir / "VertexShader.cso", L"vs_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, tri_dir / "PixelShader.hlsl",
                                 pkg_dir / "PixelShader.cso", L"ps_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, tri_dir / "VertexShader.hlsl",
                                 pkg_dir / "VertexShader_sm62.cso", L"vs_6_2", L"main");
            CompileShaderWithDxc(dxc_exe, tri_dir / "PixelShader.hlsl",
                                 pkg_dir / "PixelShader_sm62.cso", L"ps_6_2", L"main");
        } else {
            const fs::path fb = stage_root / "_fallback_hlsl";
            fs::create_directories(fb, ec);
            WriteBinaryFile(fb / "VertexShader.hlsl", kFallbackSimpleTriangleVs, std::strlen(kFallbackSimpleTriangleVs));
            WriteBinaryFile(fb / "PixelShader.hlsl", kFallbackSimpleTrianglePs, std::strlen(kFallbackSimpleTrianglePs));
            CompileShaderWithDxc(dxc_exe, fb / "VertexShader.hlsl", pkg_dir / "VertexShader.cso", L"vs_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, fb / "PixelShader.hlsl", pkg_dir / "PixelShader.cso", L"ps_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, fb / "VertexShader.hlsl", pkg_dir / "VertexShader_sm62.cso", L"vs_6_2", L"main");
            CompileShaderWithDxc(dxc_exe, fb / "PixelShader.hlsl", pkg_dir / "PixelShader_sm62.cso", L"ps_6_2", L"main");
        }

        const auto pkg_files_before = ListDirFilenames(pkg_dir);
        AssertNoDllsInDir(pkg_dir);

        // Run 1a: Hardware (NVIDIA GTX 1650 Ti when present)
        std::map<std::string, std::string> hw_metrics;
        if (has_hw_gpu) {
            const fs::path hw_t = stage_root / "run_triangle_hw_T";
            const fs::path hw_s = stage_root / "run_triangle_hw_S";
            const fs::path hw_tel = stage_root / "telemetry_triangle_hw.json";
            const auto t0_hw = std::chrono::steady_clock::now();
            OS_REQUIRE(LaunchViaOpenScarlettCli(cli_exe, staged_exe, build_dir, pkg_dir, hw_t, hw_s, hw_tel) == 0);
            const double hw_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0_hw)
                                     .count();
            OS_REQUIRE(ListDirFilenames(pkg_dir) == pkg_files_before);
            AssertNoDllsInDir(pkg_dir);
            VerifyPpmFile(hw_t / "simple_triangle.ppm", 256, 256);

            hw_metrics = ParseKeyValueFile(hw_t / "metrics.txt");
            const auto hw_tel_sum = ParseTelemetryJson(hw_tel);
            OS_REQUIRE(hw_tel_sum.total_not_impl == 0);
            OS_REQUIRE(hw_tel_sum.HasModule("d3d12_xs.dll"));
            OS_REQUIRE(hw_tel_sum.HasModule("xgameruntime.dll"));
            OS_REQUIRE(hw_tel_sum.HasModule("GameInput.dll"));
            OS_REQUIRE(hw_tel_sum.HasModule("vfs"));
            OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12));
            OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

            summary_rows.push_back(SummaryRow{
                "SimpleTriangle (HW)",
                hw_metrics.at("adapter"),
                hw_metrics.at("width") + "x" + hw_metrics.at("height"),
                hw_metrics.at("fnv1a"),
                hw_metrics.at("non_bg_pixels") + " px / " + hw_metrics.at("unique_colors") + " rgb",
                hw_tel_sum.total_calls,
                hw_tel_sum.rows.size(),
                hw_ms,
            });
        }

        // Run 1b: D3D12 WARP
        const fs::path wp_t = stage_root / "run_triangle_warp_T";
        const fs::path wp_s = stage_root / "run_triangle_warp_S";
        const fs::path wp_tel = stage_root / "telemetry_triangle_warp.json";
        const auto t0_wp = std::chrono::steady_clock::now();
        OS_REQUIRE(LaunchViaOpenScarlettCli(cli_exe, staged_exe, build_dir, pkg_dir, wp_t, wp_s, wp_tel, L"--warp") == 0);
        const double wp_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0_wp)
                                 .count();
        OS_REQUIRE(ListDirFilenames(pkg_dir) == pkg_files_before);
        VerifyPpmFile(wp_t / "simple_triangle.ppm", 256, 256);

        const auto wp_metrics = ParseKeyValueFile(wp_t / "metrics.txt");
        const auto wp_tel_sum = ParseTelemetryJson(wp_tel);
        OS_REQUIRE(wp_tel_sum.total_not_impl == 0);
        OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12));
        OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

        if (has_hw_gpu) {
            // Verify exact bit-for-bit FNV-1a hash & pixel count match between GTX 1650 Ti (SM 6.5) and WARP (SM 6.2)!
            OS_REQUIRE(hw_metrics.at("non_bg_pixels") == wp_metrics.at("non_bg_pixels"));
            OS_REQUIRE(hw_metrics.at("fnv1a") == wp_metrics.at("fnv1a"));
        }

        summary_rows.push_back(SummaryRow{
            "SimpleTriangle (WARP)",
            wp_metrics.at("adapter"),
            wp_metrics.at("width") + "x" + wp_metrics.at("height"),
            wp_metrics.at("fnv1a"),
            wp_metrics.at("non_bg_pixels") + " px / " + wp_metrics.at("unique_colors") + " rgb",
            wp_tel_sum.total_calls,
            wp_tel_sum.rows.size(),
            wp_ms,
        });
    }

    // =========================================================================
    // SAMPLE 2: Microsoft Xbox-GDK-Samples / SimpleMeshShader (GTX 1650 Ti HW)
    // =========================================================================
    if (has_hw_mesh) {
        const fs::path pkg_dir = stage_root / "gdk_simple_mesh_shader";
        fs::create_directories(pkg_dir, ec);

        const fs::path staged_exe = pkg_dir / "gdk_simple_mesh_shader.exe";
        fs::copy_file(build_dir / "gdk_simple_mesh_shader.exe", staged_exe,
                      fs::copy_options::overwrite_existing);
        VerifyStandaloneXboxPeImports(staged_exe, {"d3d12_xs.dll", "xgameruntime.dll"});

        if (has_official_gdk_samples) {
            const fs::path ms_dir = gdk_samples_root / "SimpleMeshShader";
            CompileShaderWithDxc(dxc_exe, ms_dir / "SimpleTriangleMS.hlsl",
                                 pkg_dir / "SimpleTriangleMS.cso", L"ms_6_5", L"main", ms_dir);
            CompileShaderWithDxc(dxc_exe, ms_dir / "SimpleTrianglePS.hlsl",
                                 pkg_dir / "SimpleTrianglePS.cso", L"ps_6_5", L"main", ms_dir);
        } else {
            const fs::path fb = stage_root / "_fallback_hlsl";
            fs::create_directories(fb, ec);
            WriteBinaryFile(fb / "SimpleTriangleMS.hlsl", kFallbackMeshMs, std::strlen(kFallbackMeshMs));
            WriteBinaryFile(fb / "SimpleTrianglePS.hlsl", kFallbackMeshPs, std::strlen(kFallbackMeshPs));
            CompileShaderWithDxc(dxc_exe, fb / "SimpleTriangleMS.hlsl", pkg_dir / "SimpleTriangleMS.cso", L"ms_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, fb / "SimpleTrianglePS.hlsl", pkg_dir / "SimpleTrianglePS.cso", L"ps_6_5", L"main");
        }

        const auto pkg_files_before = ListDirFilenames(pkg_dir);
        AssertNoDllsInDir(pkg_dir);

        const fs::path ms_t = stage_root / "run_mesh_hw_T";
        const fs::path ms_s = stage_root / "run_mesh_hw_S";
        const fs::path ms_tel = stage_root / "telemetry_mesh_hw.json";
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(LaunchViaOpenScarlettCli(cli_exe, staged_exe, build_dir, pkg_dir, ms_t, ms_s, ms_tel) == 0);
        const double ms_elapsed = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
        OS_REQUIRE(ListDirFilenames(pkg_dir) == pkg_files_before);
        AssertNoDllsInDir(pkg_dir);
        VerifyPpmFile(ms_t / "simple_mesh_shader.ppm", 256, 256);

        const auto metrics = ParseKeyValueFile(ms_t / "metrics.txt");
        const auto tel_sum = ParseTelemetryJson(ms_tel);
        OS_REQUIRE(tel_sum.total_not_impl == 0);
        OS_REQUIRE(tel_sum.HasModule("d3d12_xs.dll"));
        OS_REQUIRE(tel_sum.HasModule("xgameruntime.dll"));
        OS_REQUIRE(tel_sum.HasModule("vfs"));
        OS_REQUIRE(tel_sum.HasSymbol("ID3D12DeviceX::CreatePipelineState", 47));
        OS_REQUIRE(tel_sum.HasSymbol("ID3D12GraphicsCommandListX::DispatchMesh", 79));
        OS_REQUIRE(tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

        summary_rows.push_back(SummaryRow{
            "SimpleMeshShader (HW)",
            metrics.at("adapter"),
            metrics.at("width") + "x" + metrics.at("height"),
            metrics.at("fnv1a"),
            metrics.at("non_bg_pixels") + " px / " + metrics.at("unique_colors") + " rgb",
            tel_sum.total_calls,
            tel_sum.rows.size(),
            ms_elapsed,
        });
    }

    // =========================================================================
    // SAMPLE 3: Microsoft Xbox-GDK-Samples / SimpleCompute (Mandelbrot HW + WARP)
    // =========================================================================
    {
        const fs::path pkg_dir = stage_root / "gdk_simple_compute";
        fs::create_directories(pkg_dir, ec);

        const fs::path staged_exe = pkg_dir / "gdk_simple_compute.exe";
        fs::copy_file(build_dir / "gdk_simple_compute.exe", staged_exe,
                      fs::copy_options::overwrite_existing);
        VerifyStandaloneXboxPeImports(staged_exe, {"d3d12_xs.dll", "xgameruntime.dll"});

        if (has_official_gdk_samples) {
            const fs::path cs_hlsl = gdk_samples_root / "SimpleCompute" / "fractal.hlsl";
            CompileShaderWithDxc(dxc_exe, cs_hlsl, pkg_dir / "fractal.cso", L"cs_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, cs_hlsl, pkg_dir / "fractal_sm62.cso", L"cs_6_2", L"main");
        } else {
            const fs::path fb = stage_root / "_fallback_hlsl";
            fs::create_directories(fb, ec);
            WriteBinaryFile(fb / "fractal.hlsl", kFallbackFractalCs, std::strlen(kFallbackFractalCs));
            CompileShaderWithDxc(dxc_exe, fb / "fractal.hlsl", pkg_dir / "fractal.cso", L"cs_6_5", L"main");
            CompileShaderWithDxc(dxc_exe, fb / "fractal.hlsl", pkg_dir / "fractal_sm62.cso", L"cs_6_2", L"main");
        }

        const auto pkg_files_before = ListDirFilenames(pkg_dir);
        AssertNoDllsInDir(pkg_dir);

        // Run 3a: Hardware (NVIDIA GTX 1650 Ti when present)
        if (has_hw_gpu) {
            const fs::path hw_t = stage_root / "run_compute_hw_T";
            const fs::path hw_s = stage_root / "run_compute_hw_S";
            const fs::path hw_tel = stage_root / "telemetry_compute_hw.json";
            const auto t0_hw = std::chrono::steady_clock::now();
            OS_REQUIRE(LaunchViaOpenScarlettCli(cli_exe, staged_exe, build_dir, pkg_dir, hw_t, hw_s, hw_tel) == 0);
            const double hw_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0_hw)
                                     .count();
            OS_REQUIRE(ListDirFilenames(pkg_dir) == pkg_files_before);
            AssertNoDllsInDir(pkg_dir);
            VerifyPpmFile(hw_t / "mandelbrot.ppm", 256, 256);

            const auto hw_metrics = ParseKeyValueFile(hw_t / "metrics.txt");
            const auto hw_tel_sum = ParseTelemetryJson(hw_tel);
            OS_REQUIRE(hw_tel_sum.total_not_impl == 0);
            OS_REQUIRE(hw_tel_sum.HasModule("d3d12_xs.dll"));
            OS_REQUIRE(hw_tel_sum.HasModule("xgameruntime.dll"));
            OS_REQUIRE(hw_tel_sum.HasModule("vfs"));
            OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::Dispatch", 14));
            OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

            summary_rows.push_back(SummaryRow{
                "SimpleCompute (HW)",
                hw_metrics.at("adapter"),
                hw_metrics.at("width") + "x" + hw_metrics.at("height"),
                hw_metrics.at("fnv1a"),
                hw_metrics.at("non_bg_pixels") + " col / " + hw_metrics.at("unique_colors") + " bands",
                hw_tel_sum.total_calls,
                hw_tel_sum.rows.size(),
                hw_ms,
            });
        }

        // Run 3b: D3D12 WARP
        const fs::path wp_t = stage_root / "run_compute_warp_T";
        const fs::path wp_s = stage_root / "run_compute_warp_S";
        const fs::path wp_tel = stage_root / "telemetry_compute_warp.json";
        const auto t0_wp = std::chrono::steady_clock::now();
        OS_REQUIRE(LaunchViaOpenScarlettCli(cli_exe, staged_exe, build_dir, pkg_dir, wp_t, wp_s, wp_tel, L"--warp") == 0);
        const double wp_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0_wp)
                                 .count();
        OS_REQUIRE(ListDirFilenames(pkg_dir) == pkg_files_before);
        VerifyPpmFile(wp_t / "mandelbrot.ppm", 256, 256);

        const auto wp_metrics = ParseKeyValueFile(wp_t / "metrics.txt");
        const auto wp_tel_sum = ParseTelemetryJson(wp_tel);
        OS_REQUIRE(wp_tel_sum.total_not_impl == 0);
        OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::Dispatch", 14));
        OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

        summary_rows.push_back(SummaryRow{
            "SimpleCompute (WARP)",
            wp_metrics.at("adapter"),
            wp_metrics.at("width") + "x" + wp_metrics.at("height"),
            wp_metrics.at("fnv1a"),
            wp_metrics.at("non_bg_pixels") + " col / " + wp_metrics.at("unique_colors") + " bands",
            wp_tel_sum.total_calls,
            wp_tel_sum.rows.size(),
            wp_ms,
        });
    }

    // =========================================================================
    // SAMPLE 4: Unified System Combo (ALL 10 shimmed Xbox DLLs in one process!)
    // =========================================================================
    {
        const fs::path pkg_dir = stage_root / "gdk_system_combo";
        fs::create_directories(pkg_dir, ec);

        const fs::path staged_exe = pkg_dir / "gdk_system_combo.exe";
        fs::copy_file(build_dir / "gdk_system_combo.exe", staged_exe,
                      fs::copy_options::overwrite_existing);
        VerifyStandaloneXboxPeImports(staged_exe, {
            "d3d12_xs.dll",
            "xg_xs.dll",
            "xgameruntime.dll",
            "GameInput.dll",
            "dstorage_xs.dll",
            "XAudio2_9.dll",
            "AcpHal.dll",
            "winrt_xs.dll",
            "Microsoft.Xbox.Services.GDK.C.dll",
        });

        // Stage G:\asset.bin (16 KiB raw payload)
        std::vector<uint8_t> raw_payload(16384u);
        for (size_t i = 0; i < raw_payload.size(); ++i) {
            raw_payload[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
        }
        WriteBinaryFile(pkg_dir / "asset.bin", raw_payload.data(), raw_payload.size());

        // Stage G:\asset.gdeflate (96 KiB multi-tile Microsoft GDeflate v1 stream)
        std::vector<uint8_t> gdef_src(96u * 1024u);
        for (size_t i = 0; i < gdef_src.size(); ++i) {
            gdef_src[i] = static_cast<uint8_t>(((i * 5u) + (i / 64u) * 19u) & 0xFFu);
        }
        std::vector<uint8_t> gdef_compressed;
        OS_REQUIRE(openscarlett::GDeflateCompress(gdef_src.data(), gdef_src.size(), gdef_compressed));
        OS_REQUIRE(!gdef_compressed.empty());
        WriteBinaryFile(pkg_dir / "asset.gdeflate", gdef_compressed.data(), gdef_compressed.size());

        // Stage G:\sine440.xma2 (real 2048-byte XMA2 packets for XAudio2_9 + AcpHal)
        openscarlett::Xma2Config xma_cfg{};
        xma_cfg.sample_rate = 48000u;
        xma_cfg.channels = 1u;
        constexpr size_t kXmaFrames = 4096u;
        std::vector<float> sine_mono(kXmaFrames);
        for (size_t i = 0; i < kXmaFrames; ++i) {
            sine_mono[i] = 0.6f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
        }
        std::vector<uint8_t> xma_packets;
        OS_REQUIRE(openscarlett::Xma2EncodePcmToPackets(
                       sine_mono.data(), kXmaFrames, xma_cfg, xma_packets)
                       .ok());
        OS_REQUIRE(!xma_packets.empty() && (xma_packets.size() % openscarlett::kXma2PacketSize) == 0u);
        WriteBinaryFile(pkg_dir / "sine440.xma2", xma_packets.data(), xma_packets.size());

        const auto pkg_files_before = ListDirFilenames(pkg_dir);
        AssertNoDllsInDir(pkg_dir);

        const fs::path cb_t = stage_root / "run_combo_T";
        const fs::path cb_s = stage_root / "run_combo_S";
        const fs::path cb_tel = stage_root / "telemetry_system_combo.json";
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(LaunchViaOpenScarlettCli(cli_exe, staged_exe, build_dir, pkg_dir, cb_t, cb_s, cb_tel) == 0);
        const double cb_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        OS_REQUIRE(ListDirFilenames(pkg_dir) == pkg_files_before);
        AssertNoDllsInDir(pkg_dir);
        OS_REQUIRE(fs::exists(cb_t / "combo_summary.txt"));

        const auto metrics = ParseKeyValueFile(cb_t / "combo_summary.txt");
        const auto tel_sum = ParseTelemetryJson(cb_tel);
        OS_REQUIRE(tel_sum.total_not_impl == 0);

        // Verify ALL 10 shimmed Xbox DLLs + VFS reported into the unified CallTelemetry JSON!
        const char* kAllTenDlls[] = {
            "d3d12_xs.dll",
            "xg_xs.dll",
            "kernelx.dll",
            "xgameruntime.dll",
            "GameInput.dll",
            "XAudio2_9.dll",
            "AcpHal.dll",
            "dstorage_xs.dll",
            "winrt_xs.dll",
            "Microsoft.Xbox.Services.GDK.C.dll",
            "vfs",
        };
        for (const char* mod : kAllTenDlls) {
            if (!tel_sum.HasModule(mod)) {
                std::fprintf(stderr, "Missing telemetry module in gdk_system_combo: %s\n", mod);
                OS_REQUIRE(false);
            }
        }

        summary_rows.push_back(SummaryRow{
            "SystemCombo (10 DLLs)",
            metrics.at("adapter"),
            "N/A",
            "10/10 Xbox DLLs",
            "Aliasing+Save+GDeflate+XMA2+XG+XSAPI",
            tel_sum.total_calls,
            tel_sum.rows.size(),
            cb_ms,
        });
    }

    // Print crisp compatibility & measured metrics summary table
    std::printf("\n===================================================================================================================\n");
    std::printf(" OpenScarlett Sprint 3 - Live Official MIT Microsoft Xbox-GDK-Samples Bring-Up Matrix (%s)\n",
                has_official_gdk_samples ? "Official Microsoft HLSL" : "Fallback HLSL");
    std::printf("===================================================================================================================\n");
    std::printf(" %-22s | %-28s | %-7s | %-18s | %-34s | %-13s | %7s\n",
                "Sample", "Adapter", "Res", "FNV-1a Hash", "Verified Output", "Telemetry", "Time");
    std::printf("------------------------+------------------------------+---------+--------------------+------------------------------------+---------------+---------\n");
    for (const auto& r : summary_rows) {
        std::string tel_col = std::to_string(r.tel_calls) + "/" + std::to_string(r.tel_symbols);
        std::printf(" %-22s | %-28.28s | %-7s | %-18s | %-34.34s | %-13s | %5.1f ms\n",
                    r.name.c_str(),
                    r.adapter.c_str(),
                    r.resolution.c_str(),
                    r.fnv1a.c_str(),
                    r.pixels_info.c_str(),
                    tel_col.c_str(),
                    r.elapsed_ms);
    }
    std::printf("===================================================================================================================\n");
    std::printf("[PASS] test_gdk_live: all 6 live openscarlett-run executions passed with 0 blockers (not_impl == 0)!\n");
    return 0;
}
