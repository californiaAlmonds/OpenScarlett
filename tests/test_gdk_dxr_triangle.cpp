// OpenScarlett Wave 2 — Portable End-to-End Live Test Harness for DXRTriangle.
//
// Stages standalone Xbox-ABI `gdk_dxr_triangle.exe` into an isolated ZERO-DLL package folder
// with genuine `MicrosoftGame.Config` (TitleId = 0x5CA01003), compiles `DxrTriangleLib.hlsl`
// live with `dxc.exe -T lib_6_3` into `<package_dir>\DxrTriangleLib.cso`, launches strictly via
// `openscarlett.exe run <package_dir>` (APC hook + VFS + CallTelemetry), and verifies:
//   - Hardware GPU (DXR >= 10 -> native hw_dxr + --force-sw parity; DXR == 0 GTX 1650 Ti -> sw_bvh_fallback)
//   - D3D12 WARP (-- --warp -> sw_bvh_fallback)
//   - Bit-exact FNV-1a hash parity between GTX 1650 Ti (DXR=0) & WARP (and pixel parity on DXR >= 10)
//   - Complete CallTelemetry checks with not_impl == 0
//   - 100% byte-identical <package_dir> before & after every run

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

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kExpectedHitPixels = 10112u;

// DXR 1.0 shader library (compiled live with dxc.exe -T lib_6_3).
// Matches Path B's exact camera, two-triangle geometry, barycentric RGB shading & miss color.
constexpr const char* kDxrTriangleLibHlsl = R"HLSL(
RaytracingAccelerationStructure g_Scene : register(t0, space0);
RWTexture2D<float4> g_Output : register(u0, space0);

struct RayPayload {
    float4 color;
};

[shader("raygeneration")]
void RaygenShader() {
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    float2 uv = (float2(idx) + 0.5f) / float2(dim);
    float ndc_x = uv.x * 2.0f - 1.0f;
    float ndc_y = 1.0f - uv.y * 2.0f;

    RayDesc ray;
    ray.Origin = float3(ndc_x, ndc_y, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 10.0f;

    RayPayload payload;
    payload.color = float4(20.0f / 255.0f, 24.0f / 255.0f, 36.0f / 255.0f, 1.0f);

    TraceRay(
        g_Scene,
        RAY_FLAG_NONE,
        0xFF,
        0,
        1,
        0,
        ray,
        payload);

    g_Output[idx] = payload.color;
}

[shader("closesthit")]
void ClosestHitShader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    float w1 = attr.barycentrics.x;
    float w2 = attr.barycentrics.y;
    float w0 = 1.0f - w1 - w2;

    if (PrimitiveIndex() == 0u) {
        payload.color = float4(w0, w1, w2, 1.0f);
    } else {
        float3 c0 = float3(1.0f, 0.9f, 0.2f);
        float3 c1 = float3(0.1f, 0.85f, 1.0f);
        float3 c2 = float3(0.9f, 0.2f, 0.95f);
        float3 rgb = c0 * w0 + c1 * w1 + c2 * w2;
        payload.color = float4(rgb, 1.0f);
    }
}

[shader("miss")]
void MissShader(inout RayPayload payload) {
    payload.color = float4(20.0f / 255.0f, 24.0f / 255.0f, 36.0f / 255.0f, 1.0f);
}
)HLSL";

// Genuine MicrosoftGame.Config with TitleId 0x5CA01003 (accepted by both game_config.cpp and xgameruntime.dll)
constexpr const char* kMicrosoftGameConfigXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Game configVersion=\"1\">\n"
    "  <Identity Name=\"OpenScarlett.DXRTriangle\" Publisher=\"CN=OpenScarlett\" Version=\"1.0.0.0\"/>\n"
    "  <TitleId>0x5CA01003</TitleId>\n"
    "  <ShellVisuals DefaultDisplayName=\"DXRTriangle\"/>\n"
    "  <ExecutableList>\n"
    "    <Executable Name=\"gdk_dxr_triangle.exe\" Id=\"Game\" TargetDeviceFamily=\"Scarlett\"/>\n"
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

    bool HasSymbol(const std::string& sym, int32_t expected_slot = -999, uint64_t min_calls = 1) const {
        for (const auto& r : rows) {
            if (r.symbol == sym) {
                if (expected_slot == -999 || r.slot == expected_slot) {
                    return r.total >= min_calls;
                }
            }
        }
        return false;
    }

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }
};

struct DirSnapshot {
    uint64_t fnv1a_hash = 0;
    std::vector<std::string> entries;
};

struct RunReport {
    std::string label;
    std::map<std::string, std::string> metrics;
    TelemetrySummary telemetry;
    fs::path ppm_path;
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
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
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

DirSnapshot HashDirectoryTree(const fs::path& root) {
    DirSnapshot snap{};
    uint64_t h = 14695981039346656037ull;
    auto feed = [&](const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };

    std::vector<fs::path> rel_paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        rel_paths.push_back(fs::relative(entry.path(), root));
    }
    std::sort(rel_paths.begin(), rel_paths.end());

    for (const auto& rel : rel_paths) {
        const fs::path full = root / rel;
        const std::string rel_s = rel.generic_string();
        if (fs::is_directory(full)) {
            const std::string tag = "D:" + rel_s;
            snap.entries.push_back(tag);
            feed(tag.data(), tag.size());
        } else {
            const std::string bytes = ReadTextFile(full);
            const std::string tag = "F:" + rel_s + ":" + std::to_string(bytes.size());
            snap.entries.push_back(tag);
            feed(tag.data(), tag.size());
            feed(bytes.data(), bytes.size());
        }
    }
    snap.fnv1a_hash = h;
    return snap;
}

void AssertNoDllsInDir(const fs::path& dir) {
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");
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
        return (fs::path(temp_buf) / subdir_name).make_preferred();
    }
    const fs::path sys_tmp = fs::temp_directory_path(ec);
    if (!ec && !sys_tmp.empty()) {
        return (sys_tmp / subdir_name).make_preferred();
    }
    return (ResolveRuntimeBinDir() / subdir_name).make_preferred();
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

DWORD RunProcessW(const fs::path& exe_path, const std::wstring& cmd_line, DWORD timeout_ms = 60000) {
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

void CompileDxrLibraryWithDxc(
    const fs::path& dxc_exe,
    const fs::path& hlsl_path,
    const fs::path& cso_out_path) {
    std::wstring cmd = L"\"" + dxc_exe.wstring() + L"\" -T lib_6_3 -O3 -Fo \"" +
                       cso_out_path.wstring() + L"\" \"" + hlsl_path.wstring() + L"\"";
    const DWORD rc = RunProcessW(dxc_exe, cmd, 30000);
    OS_REQUIRE(rc == 0);
    OS_REQUIRE(fs::exists(cso_out_path));
    OS_REQUIRE(fs::file_size(cso_out_path) > 128u);

    const std::string cso_bytes = ReadTextFile(cso_out_path);
    OS_REQUIRE(cso_bytes.size() >= 4u && cso_bytes.compare(0, 4, "DXBC") == 0);
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

void VerifyPpmFile(const fs::path& ppm_path, uint32_t width, uint32_t height, std::vector<uint8_t>& out_rgb) {
    OS_REQUIRE(fs::exists(ppm_path));
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const uint64_t rgb_bytes = static_cast<uint64_t>(width) * height * 3ull;
    OS_REQUIRE(fs::file_size(ppm_path) == header.size() + rgb_bytes);

    std::ifstream ifs(ppm_path, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::string prefix(header.size(), '\0');
    ifs.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    OS_REQUIRE(prefix == header);

    out_rgb.resize(static_cast<size_t>(rgb_bytes));
    ifs.read(reinterpret_cast<char*>(out_rgb.data()), static_cast<std::streamsize>(out_rgb.size()));
    OS_REQUIRE(static_cast<bool>(ifs));
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

RunReport LaunchPackageRun(
    const std::string& label,
    const fs::path& cli_exe,
    const fs::path& pkg_dir,
    const fs::path& shim_dir,
    const fs::path& run_root,
    const DirSnapshot& pkg_snap_before,
    const std::wstring& guest_args = {}) {
    std::error_code ec;
    const fs::path vfs_t = run_root / "T";
    const fs::path vfs_s = run_root / "S";
    const fs::path vfs_g_ov = run_root / "G_overlay";
    const fs::path tel_json = run_root / "telemetry.json";
    fs::create_directories(run_root, ec);

    // Launch purely via `openscarlett run <package_dir>` (resolving MicrosoftGame.Config automatically!)
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + pkg_dir.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-g-overlay \"" + vfs_g_ov.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --vfs-s \"" + vfs_s.wstring() +
        L"\" --telemetry \"" + tel_json.wstring() +
        L"\" --timeout-ms 60000";
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const DWORD rc = RunProcessW(cli_exe, cmd, 65000);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    OS_REQUIRE(rc == 0);

    // Verify <package_dir> is 100% byte-identical and still contains zero DLLs!
    const DirSnapshot pkg_snap_after = HashDirectoryTree(pkg_dir);
    OS_REQUIRE(pkg_snap_after.fnv1a_hash == pkg_snap_before.fnv1a_hash);
    OS_REQUIRE(pkg_snap_after.entries == pkg_snap_before.entries);
    AssertNoDllsInDir(pkg_dir);

    RunReport rep{};
    rep.label = label;
    rep.elapsed_ms = elapsed_ms;
    rep.ppm_path = vfs_t / "dxr_triangle.ppm";
    std::vector<uint8_t> rgb;
    VerifyPpmFile(rep.ppm_path, kWidth, kHeight, rgb);

    rep.metrics = ParseKeyValueFile(vfs_t / "metrics.txt");
    rep.telemetry = ParseTelemetryJson(tel_json);

    // Universal invariants across every mode
    OS_REQUIRE(rep.telemetry.total_not_impl == 0ull);
    OS_REQUIRE(rep.metrics.at("sample") == "DXRTriangle");
    OS_REQUIRE(rep.metrics.at("title_id") == "0x5CA01003");
    OS_REQUIRE(rep.metrics.at("width") == "256");
    OS_REQUIRE(rep.metrics.at("height") == "256");
    OS_REQUIRE(rep.metrics.at("hit_pixels") == std::to_string(kExpectedHitPixels));
    OS_REQUIRE(!rep.metrics.at("fnv1a").empty());

    OS_REQUIRE(rep.telemetry.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(rep.telemetry.HasModule("xgameruntime.dll"));
    OS_REQUIRE(rep.telemetry.HasModule("pixevt.dll"));
    OS_REQUIRE(rep.telemetry.HasModule("vfs"));
    OS_REQUIRE(rep.telemetry.HasSymbol("XGameGetXboxTitleId"));
    OS_REQUIRE(rep.telemetry.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

    return rep;
}

void VerifyHwDxrTelemetry(const TelemetrySummary& tel) {
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateStateObject", 62));
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::GetRaytracingAccelerationStructurePrebuildInfo", 63));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::BuildRaytracingAccelerationStructure", 72, 2));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::SetPipelineState1", 75));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::DispatchRays", 76));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::CopyResource", 17));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::PresentX", 19));
}

void VerifySwBvhTelemetry(const TelemetrySummary& tel) {
    OS_REQUIRE(tel.HasModule("XboxBvhBuilder.dll"));
    OS_REQUIRE(tel.HasSymbol("XboxBvhCreateBuilder"));
    OS_REQUIRE(tel.HasSymbol("XboxBvhGetPrebuildInfo", -999, 2));
    OS_REQUIRE(tel.HasSymbol("XboxBvhBuildBlas"));
    OS_REQUIRE(tel.HasSymbol("XboxBvhBuildTlas"));
    OS_REQUIRE(tel.HasSymbol("XboxBvhTraceRay", -999, static_cast<uint64_t>(kWidth) * kHeight));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::CopyTextureRegion", 16));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::PresentX", 19));
}

}  // namespace

int main() {
    // Clear inherited VFS/TitleId overrides so openscarlett run <package_dir> reads MicrosoftGame.Config cleanly.
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G_OVERLAY", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_TITLE_ID", nullptr);

    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = bin_dir / "openscarlett.exe";
    const fs::path guest_exe_src = bin_dir / "gdk_dxr_triangle.exe";
    const fs::path dxc_exe = FindDxcExe();

    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(bin_dir / "openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(guest_exe_src));
    OS_REQUIRE(!dxc_exe.empty());

    // Probe host D3D12 hardware adapters (ordered by DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)
    bool has_hw_gpu = false;
    uint32_t hw_dxr_tier = 0u;
    std::string hw_adapter_name;
    {
        std::vector<openscarlett::D3D12AdapterInfo> adapters;
        if (openscarlett::EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp) {
                    has_hw_gpu = true;
                    hw_dxr_tier = a.raytracing_tier;
                    hw_adapter_name = a.description;
                    break;
                }
            }
        }
    }

    const fs::path scratch_root = ResolveScratchDir(L"openscarlett_wave2_dxr_triangle");
    std::error_code ec;
    fs::remove_all(scratch_root, ec);
    fs::create_directories(scratch_root, ec);
    OS_REQUIRE(fs::exists(scratch_root));

    // Stage ZERO-DLL Xbox package folder with MicrosoftGame.Config + gdk_dxr_triangle.exe + DxrTriangleLib.cso
    const fs::path pkg_dir = scratch_root / "pkg_dxr_triangle";
    fs::create_directories(pkg_dir, ec);

    const fs::path staged_exe = pkg_dir / "gdk_dxr_triangle.exe";
    fs::copy_file(guest_exe_src, staged_exe, fs::copy_options::overwrite_existing);
    VerifyStandaloneXboxPeImports(staged_exe, {
        "d3d12_xs.dll",
        "xgameruntime.dll",
        "XboxBvhBuilder.dll",
        "pixevt.dll",
    });

    const fs::path hlsl_src = scratch_root / "DxrTriangleLib.hlsl";
    WriteBinaryFile(hlsl_src, kDxrTriangleLibHlsl, std::strlen(kDxrTriangleLibHlsl));
    CompileDxrLibraryWithDxc(dxc_exe, hlsl_src, pkg_dir / "DxrTriangleLib.cso");

    WriteBinaryFile(pkg_dir / "MicrosoftGame.Config",
                    kMicrosoftGameConfigXml,
                    std::strlen(kMicrosoftGameConfigXml));

    AssertNoDllsInDir(pkg_dir);
    const DirSnapshot pkg_snap_before = HashDirectoryTree(pkg_dir);

    std::vector<RunReport> reports;
    RunReport hw_report{};
    RunReport hw_sw_report{};
    bool ran_hw = false;
    bool ran_hw_dxr = false;

    // 1. Hardware GPU execution (when a non-WARP hardware GPU is present)
    if (has_hw_gpu) {
        ran_hw = true;
        hw_report = LaunchPackageRun(
            hw_dxr_tier >= 10u ? "DXRTriangle (HW DXR)" : "DXRTriangle (HW SW-BVH)",
            cli_exe, pkg_dir, bin_dir, scratch_root / "run_hw", pkg_snap_before);

        OS_REQUIRE(hw_report.metrics.at("is_warp") == "0");

        if (hw_dxr_tier >= 10u) {
            ran_hw_dxr = true;
            OS_REQUIRE(hw_report.metrics.at("mode") == "hw_dxr");
            OS_REQUIRE(std::stoul(hw_report.metrics.at("dxr_tier")) >= 10u);
            VerifyHwDxrTelemetry(hw_report.telemetry);
            reports.push_back(hw_report);

            // Also run --force-sw on DXR >= 10 hardware to prove HW RT-core vs SAH SW BVH parity!
            hw_sw_report = LaunchPackageRun(
                "DXRTriangle (HW --force-sw)",
                cli_exe, pkg_dir, bin_dir, scratch_root / "run_hw_force_sw",
                pkg_snap_before, L"--force-sw");
            OS_REQUIRE(hw_sw_report.metrics.at("is_warp") == "0");
            OS_REQUIRE(hw_sw_report.metrics.at("mode") == "sw_bvh_fallback");
            VerifySwBvhTelemetry(hw_sw_report.telemetry);

            // Compare HW RT-core PPM vs SAH SW BVH PPM across all 65,536 pixels (<= 1 LSB max diff)
            std::vector<uint8_t> hw_rgb;
            std::vector<uint8_t> sw_rgb;
            VerifyPpmFile(hw_report.ppm_path, kWidth, kHeight, hw_rgb);
            VerifyPpmFile(hw_sw_report.ppm_path, kWidth, kHeight, sw_rgb);
            OS_REQUIRE(hw_rgb.size() == sw_rgb.size());
            int max_diff = 0;
            for (size_t i = 0; i < hw_rgb.size(); ++i) {
                const int d = std::abs(static_cast<int>(hw_rgb[i]) - static_cast<int>(sw_rgb[i]));
                if (d > max_diff) max_diff = d;
            }
            OS_REQUIRE(max_diff <= 1);
            std::printf("  [parity] HW DXR (%s, DXR=%u) vs --force-sw SAH BVH: max_channel_diff=%d LSB (exact_hash=%s)\n",
                        hw_adapter_name.c_str(),
                        hw_dxr_tier,
                        max_diff,
                        hw_report.metrics.at("fnv1a") == hw_sw_report.metrics.at("fnv1a") ? "yes" : "no");
            reports.push_back(hw_sw_report);
        } else {
            OS_REQUIRE(hw_report.metrics.at("mode") == "sw_bvh_fallback");
            OS_REQUIRE(hw_report.metrics.at("dxr_tier") == "0");
            VerifySwBvhTelemetry(hw_report.telemetry);
            reports.push_back(hw_report);
        }
    }

    // 2. D3D12 WARP execution (always runs, including WARP-only GitHub Actions CI runners!)
    const RunReport warp_report = LaunchPackageRun(
        "DXRTriangle (WARP)",
        cli_exe, pkg_dir, bin_dir, scratch_root / "run_warp",
        pkg_snap_before, L"--warp");
    OS_REQUIRE(warp_report.metrics.at("is_warp") == "1");
    OS_REQUIRE(warp_report.metrics.at("mode") == "sw_bvh_fallback");
    VerifySwBvhTelemetry(warp_report.telemetry);
    reports.push_back(warp_report);

    // 3. Bit-exact FNV-1a hash & pixel parity between HW SW-BVH and WARP!
    if (ran_hw) {
        const RunReport& ref_sw = ran_hw_dxr ? hw_sw_report : hw_report;
        OS_REQUIRE(ref_sw.metrics.at("fnv1a") == warp_report.metrics.at("fnv1a"));
        OS_REQUIRE(ref_sw.metrics.at("hit_pixels") == warp_report.metrics.at("hit_pixels"));
        OS_REQUIRE(ReadTextFile(ref_sw.ppm_path) == ReadTextFile(warp_report.ppm_path));
    }

    // Print summary table
    std::printf("\n===================================================================================================================\n");
    std::printf(" OpenScarlett Wave 2 - DXRTriangle Bring-Up Results (lib_6_3 DXIL validated via dxc.exe)\n");
    std::printf("===================================================================================================================\n");
    std::printf(" %-26s | %-15s | %-28s | %-18s | %-13s | %7s\n",
                "Run", "Mode", "Adapter", "FNV-1a Hash", "Telemetry", "Time");
    std::printf("----------------------------+-----------------+------------------------------+--------------------+---------------+---------\n");
    for (const auto& r : reports) {
        const std::string tel_col =
            std::to_string(r.telemetry.total_calls) + "/" + std::to_string(r.telemetry.rows.size());
        std::printf(" %-26.26s | %-15.15s | %-28.28s | %-18.18s | %-13s | %5.1f ms\n",
                    r.label.c_str(),
                    r.metrics.at("mode").c_str(),
                    r.metrics.at("adapter").c_str(),
                    r.metrics.at("fnv1a").c_str(),
                    tel_col.c_str(),
                    r.elapsed_ms);
    }
    std::printf("===================================================================================================================\n");
    std::printf("[PASS] test_gdk_dxr_triangle: %zu live openscarlett-run executions passed, 0 blockers (not_impl==0), pkg_hash=0x%016llX byte-identical!\n",
                reports.size(),
                static_cast<unsigned long long>(pkg_snap_before.fnv1a_hash));
    return 0;
}
