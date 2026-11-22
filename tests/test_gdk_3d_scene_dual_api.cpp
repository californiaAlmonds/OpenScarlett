// OpenScarlett End-to-End Isolated Dual-API Launch Test for `gdk_3d_scene.exe`.
//
// Stages ONLY `gdk_3d_scene.exe` and `MicrosoftGame.Config` into an isolated zero-DLL temp
// package directory (strictly on E:\OpenScarlett\.tmp\... when running from E:\, never C:\),
// then launches it via `openscarlett.exe run` on BOTH:
//   1. Default D3D12 backend (`--backend=d3d12`)
//   2. Vulkan backend (`--backend=vulkan`)
//
// Verifies for both backends:
//   - Pure Xbox-ABI PE imports (d3d12_xs, xgameruntime, GameInput, XAudio2_9, kernelx; no hook DLL)
//   - Zero DLLs in package folder & byte-identical package tree before == after launch
//   - Clean exit code 0 across 12 animated headless frames
//   - Multiple distinct non-zero PresentX frame hashes (animated camera + hierarchy + 3-bone skinning)
//   - Rich non-black/non-uniform visual output (lit + shadowed variance + BC1/BC3 textured pixels)
//   - Persistent S:\scene_checkpoint.txt written and materialized under --vfs-s
//   - Unified CallTelemetry JSON reports not_impl == 0 across all tracked modules & symbols!

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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

#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/vfs_mounts.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kFallbackGameConfigXml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="OpenScarlett.Scarlett3DScene"
            Publisher="CN=OpenScarlett, O=MIT"
            Version="1.0.0.0" />
  <TitleId>5CA03D01</TitleId>
  <MSAAppId>000000004C263D01</MSAAppId>
  <ShellVisuals DefaultDisplayName="OpenScarlett 3D Scene (Scarlett)"
                PublisherDisplayName="OpenScarlett" />
  <ExecutableList>
    <Executable Name="gdk_3d_scene.exe"
                Id="Game"
                TargetDeviceFamily="Scarlett" />
  </ExecutableList>
</Game>
)XML";

struct TelemetryRow {
    std::string module;
    std::string symbol;
    int32_t slot = -1;
    uint64_t total = 0;
    uint64_t ok = 0;
    uint64_t not_impl = 0;
    uint64_t errors = 0;
};

struct TelemetrySummary {
    std::vector<TelemetryRow> rows;
    std::set<std::string> modules;
    uint64_t total_calls = 0;
    uint64_t total_not_impl = 0;

    bool HasModule(const std::string& m) const {
        return modules.find(m) != modules.end();
    }

    bool HasSymbol(const std::string& sym, uint64_t min_total = 1ull) const {
        for (const auto& r : rows) {
            if (r.symbol == sym && r.total >= min_total && r.not_impl == 0ull) {
                return true;
            }
        }
        return false;
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

void WriteTextFile(const fs::path& p, const std::string& text) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    OS_REQUIRE(static_cast<bool>(ofs));
}

std::map<std::string, std::string> ParseKvFile(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::istringstream iss(ReadTextFile(p));
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return kv;
}

std::vector<DirFileDigest> SnapshotZeroDllDir(const fs::path& dir) {
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
#ifdef OPENSCARLETT_BINARY_DIR
    return fs::path(OPENSCARLETT_BINARY_DIR);
#else
    return fs::current_path();
#endif
}

fs::path ResolveScratchDir(const wchar_t* subdir) {
    std::error_code ec;
    const fs::path bin_dir = ResolveRuntimeBinDir();
    const bool bin_off_sys = !openscarlett::IsPathOnSystemDriveW(bin_dir.wstring());

    // Keep all temp/package staging strictly off C:\ (under E:\OpenScarlett\.tmp\...) on local hosts.
    if (bin_off_sys) {
        const fs::path repo_tmp = LR"(E:\OpenScarlett\.tmp)";
        if (fs::exists(repo_tmp, ec)) {
            return repo_tmp / subdir;
        }
    }

    wchar_t temp_buf[1024]{};
    const DWORD len = ::GetEnvironmentVariableW(L"TEMP", temp_buf, 1024u);
    if (len > 0 && len < 1024u) {
        const fs::path tmp_path(temp_buf);
        if (!bin_off_sys || !openscarlett::IsPathOnSystemDriveW(tmp_path.wstring())) {
            return tmp_path / subdir;
        }
    }
    if (bin_off_sys) {
        const std::wstring work_dir = openscarlett::ResolveDefaultWorkDirW(bin_dir.wstring());
        if (!work_dir.empty()) {
            return fs::path(work_dir) / subdir;
        }
    }
    const fs::path sys_tmp = fs::temp_directory_path(ec);
    if (!ec && !sys_tmp.empty()) {
        return sys_tmp / subdir;
    }
    return bin_dir / subdir;
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
    TelemetrySummary sum{};
    std::istringstream iss(ReadTextFile(json_path));
    std::string line;
    while (std::getline(iss, line)) {
        TelemetryRow r{};
        if (!ExtractJsonString(line, "module", r.module) ||
            !ExtractJsonString(line, "symbol", r.symbol)) {
            continue;
        }
        int64_t v = 0;
        if (ExtractJsonInt(line, "slot", v)) r.slot = static_cast<int32_t>(v);
        if (ExtractJsonInt(line, "total", v)) r.total = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "ok", v)) r.ok = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "not_impl", v)) r.not_impl = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "errors", v)) r.errors = static_cast<uint64_t>(v);
        sum.modules.insert(r.module);
        sum.total_calls += r.total;
        sum.total_not_impl += r.not_impl;
        sum.rows.push_back(std::move(r));
    }
    return sum;
}

void VerifyPureXboxPeImports(const fs::path& exe_path) {
    openscarlett::PeInspectionReport rep;
    OS_REQUIRE(openscarlett::InspectPeFile(exe_path.string(), rep).ok());
    OS_REQUIRE(!rep.is_dll);

    std::set<std::string> imported;
    for (const auto& mod : rep.imports) {
        std::string lo = mod.dll_name;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(lo != "openscarlett_hook.dll");
        imported.insert(lo);
    }

    OS_REQUIRE(imported.count("d3d12_xs.dll") == 1u);
    OS_REQUIRE(imported.count("xgameruntime.dll") == 1u);
    OS_REQUIRE(imported.count("gameinput.dll") == 1u);
    OS_REQUIRE(imported.count("xaudio2_9.dll") == 1u);
    OS_REQUIRE(imported.count("kernelx.dll") == 1u);
}

DWORD RunProcessW(const fs::path& exe_path, const std::wstring& cmd_line, DWORD timeout_ms = 60000u) {
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
        0u,
        nullptr,
        nullptr,
        &si,
        &pi);
    OS_REQUIRE(ok != FALSE);
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    OS_REQUIRE(wr == WAIT_OBJECT_0);
    DWORD code = 999u;
    OS_REQUIRE(::GetExitCodeProcess(pi.hProcess, &code) != FALSE);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return code;
}

bool EnvFlagTrue(const char* name) {
    char buf[64]{};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T');
}

void VerifyBackendLaunch(const std::string& backend,
                         const fs::path& cli_exe,
                         const fs::path& build_dir,
                         const fs::path& pkg_dir,
                         const std::vector<DirFileDigest>& pkg_before,
                         const fs::path& run_root,
                         bool expect_warp) {
    std::error_code ec;
    const fs::path vfs_g_ov = run_root / "G_overlay";
    const fs::path vfs_t = run_root / "T";
    const fs::path vfs_s = run_root / "S";
    const fs::path tel_json = run_root / ("telemetry_" + backend + ".json");
    const fs::path stats_file = run_root / ("stats_" + backend + ".txt");

    fs::remove_all(run_root, ec);
    fs::create_directories(vfs_g_ov, ec);
    fs::create_directories(vfs_t, ec);
    fs::create_directories(vfs_s, ec);

    const std::wstring wbackend(backend.begin(), backend.end());
    const std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + pkg_dir.wstring() +
        L"\" --shim-dir \"" + build_dir.wstring() +
        L"\" --vfs-g \"" + pkg_dir.wstring() +
        L"\" --vfs-g-overlay \"" + vfs_g_ov.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --vfs-s \"" + vfs_s.wstring() +
        L"\" --telemetry \"" + tel_json.wstring() +
        L"\" --backend=" + wbackend +
        L" --timeout-ms 60000 -- --headless --frames 12 --stats-out \"" +
        stats_file.wstring() + L"\"";

    const auto t0 = std::chrono::steady_clock::now();
    const DWORD rc = RunProcessW(cli_exe, cmd, 65000u);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n[gdk_3d_scene --backend=%s (%s)] exit=%lu elapsed=%.1f ms\n",
                backend.c_str(), expect_warp ? "WARP" : "HW/Auto",
                static_cast<unsigned long>(rc), elapsed_ms);
    OS_REQUIRE(rc == 0u);

    // 1. Package directory remains strictly zero-DLL and byte-identical.
    const auto pkg_after = SnapshotZeroDllDir(pkg_dir);
    OS_REQUIRE(pkg_after == pkg_before);

    // 2. Verify rendered 3D frame metrics & visual sanity.
    OS_REQUIRE(fs::exists(stats_file));
    const auto kv = ParseKvFile(stats_file);
    const uint64_t frames = std::strtoull(kv.at("frames_rendered").c_str(), nullptr, 10);
    const uint64_t uniq_hashes = std::strtoull(kv.at("unique_frame_hashes").c_str(), nullptr, 10);
    const uint64_t non_black = std::strtoull(kv.at("non_black_pixels").c_str(), nullptr, 10);
    const uint64_t uniq_colors = std::strtoull(kv.at("unique_colors").c_str(), nullptr, 10);
    const uint64_t lit_px = std::strtoull(kv.at("lit_pixels").c_str(), nullptr, 10);
    const uint64_t shadow_px = std::strtoull(kv.at("shadow_pixels").c_str(), nullptr, 10);
    const uint64_t bc_px = std::strtoull(kv.at("bc_textured_pixels").c_str(), nullptr, 10);
    const double lum_stddev = std::strtod(kv.at("luminance_stddev").c_str(), nullptr);
    const uint64_t audio_bufs = std::strtoull(kv.at("audio_buffers_submitted").c_str(), nullptr, 10);
    const int ckpt_ok = std::atoi(kv.at("checkpoint_written").c_str());
    const int is_warp = std::atoi(kv.at("is_warp").c_str());

    std::printf("  Adapter=%s (warp=%d) | BackendEnv=%s | Frames=%llu | UniqueHashes=%llu\n",
                kv.at("adapter").c_str(), is_warp, kv.at("gpu_backend_env").c_str(),
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(uniq_hashes));
    std::printf("  NonBlack=%llu | UniqColors=%llu | LitPx=%llu | ShadowPx=%llu | BcTexPx=%llu | LumStd=%.2f | Audio=%llu | Ckpt=%d\n",
                static_cast<unsigned long long>(non_black),
                static_cast<unsigned long long>(uniq_colors),
                static_cast<unsigned long long>(lit_px),
                static_cast<unsigned long long>(shadow_px),
                static_cast<unsigned long long>(bc_px),
                lum_stddev,
                static_cast<unsigned long long>(audio_bufs),
                ckpt_ok);

    OS_REQUIRE(kv.at("title") == "gdk_3d_scene");
    OS_REQUIRE(kv.at("title_id") == "0x5CA03D01");
    OS_REQUIRE(kv.at("gpu_backend_env") == backend);
    if (expect_warp) {
        OS_REQUIRE(is_warp == 1);
    }
    OS_REQUIRE(frames == 12ull);
    OS_REQUIRE(uniq_hashes >= 10ull);
    OS_REQUIRE(kv.at("first_frame_hash") != "0x0000000000000000");
    OS_REQUIRE(kv.at("first_frame_hash") != kv.at("last_frame_hash"));
    OS_REQUIRE(non_black >= 50000ull);
    OS_REQUIRE(uniq_colors >= 64ull);
    OS_REQUIRE(lit_px >= 2000ull);
    OS_REQUIRE(shadow_px >= 500ull);
    OS_REQUIRE(bc_px >= 10000ull);
    OS_REQUIRE(lum_stddev >= 10.0);
    OS_REQUIRE(audio_bufs >= 1ull);
    OS_REQUIRE(ckpt_ok == 1);

    // 3. Verify S:\scene_checkpoint.txt was written to VFS S:\ backing directory!
    const fs::path ckpt_path = vfs_s / "scene_checkpoint.txt";
    OS_REQUIRE(fs::exists(ckpt_path));
    const std::string ckpt_text = ReadTextFile(ckpt_path);
    OS_REQUIRE(ckpt_text.find("title=gdk_3d_scene") != std::string::npos);
    OS_REQUIRE(ckpt_text.find("frames_rendered=12") != std::string::npos);

    // 4. Verify CallTelemetry JSON: not_impl == 0 across ALL modules & full Xbox stack coverage!
    const TelemetrySummary tel = ParseTelemetryJson(tel_json);
    std::printf("  Telemetry: %zu symbols / %llu calls | not_impl=%llu | modules=%zu\n",
                tel.rows.size(),
                static_cast<unsigned long long>(tel.total_calls),
                static_cast<unsigned long long>(tel.total_not_impl),
                tel.modules.size());
    OS_REQUIRE(tel.rows.size() > 0u);
    OS_REQUIRE(tel.total_not_impl == 0ull);
    OS_REQUIRE(tel.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(tel.HasModule("xgameruntime.dll"));
    OS_REQUIRE(tel.HasModule("GameInput.dll"));
    OS_REQUIRE(tel.HasModule("XAudio2_9.dll"));
    OS_REQUIRE(tel.HasModule("kernelx.dll"));
    OS_REQUIRE(tel.HasModule("vfs"));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::PresentX", 12ull));
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateDepthStencilView", 2ull));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::CopyTextureRegion", 2ull));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 12ull));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_AUDIO_NULL", L"1") != 0);

    const bool expect_warp = EnvFlagTrue("OPENSCARLETT_PREFER_WARP");

    const fs::path build_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = build_dir / L"openscarlett.exe";
    const fs::path guest_exe = build_dir / L"gdk_3d_scene.exe";
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(build_dir / L"openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(guest_exe));

    // Verify pure Xbox-ABI PE imports before staging
    VerifyPureXboxPeImports(guest_exe);

    // Stage isolated zero-DLL package directory containing ONLY gdk_3d_scene.exe + MicrosoftGame.Config
    const fs::path scratch_root = ResolveScratchDir(L"openscarlett_gdk_3d_scene_dual_api");
    const fs::path pkg_dir = scratch_root / "Scarlett3DScenePkg";
    std::error_code ec;
    fs::remove_all(scratch_root, ec);
    fs::create_directories(pkg_dir, ec);

    fs::copy_file(guest_exe, pkg_dir / L"gdk_3d_scene.exe", fs::copy_options::overwrite_existing);

    // Prefer source MicrosoftGame.Config if available, otherwise emit exact embedded copy.
    std::string config_xml = kFallbackGameConfigXml;
#ifdef OPENSCARLETT_SOURCE_DIR
    {
        const fs::path src_cfg = fs::path(OPENSCARLETT_SOURCE_DIR) /
                                 "tests" / "gdk_3d_scene" / "MicrosoftGame.Config";
        if (fs::exists(src_cfg, ec)) {
            config_xml = ReadTextFile(src_cfg);
        }
    }
#endif
    WriteTextFile(pkg_dir / "MicrosoftGame.Config", config_xml);

    const auto pkg_before = SnapshotZeroDllDir(pkg_dir);
    OS_REQUIRE(pkg_before.size() == 2u);

    // 1. Launch on Default D3D12 Backend (--backend=d3d12)
    VerifyBackendLaunch("d3d12", cli_exe, build_dir, pkg_dir, pkg_before,
                        scratch_root / "run_d3d12", expect_warp);

    // 2. Launch on Vulkan Backend (--backend=vulkan)
    VerifyBackendLaunch("vulkan", cli_exe, build_dir, pkg_dir, pkg_before,
                        scratch_root / "run_vulkan", expect_warp);

    std::printf("\n[PASS] test_gdk_3d_scene_dual_api (%s: D3D12 + Vulkan isolated zero-DLL launch verified!)\n",
                expect_warp ? "WARP" : "Hardware GPU");
    return 0;
}
