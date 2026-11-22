// OpenScarlett Wave 2 — Real External GDK-Header Binary & 19-DLL Live Telemetry Bring-Up Test Harness.
//
// 100% portable for both local HW+WARP machines (NVIDIA GTX 1650 Ti) and WARP-only GitHub Actions CI:
//   - Resolves runtime binary directory via GetModuleFileNameW (safe for hidden-build/ smoke tests)
//   - Stages all scratch packages & shader caches strictly under %TEMP% (E:\ under build_wt.cmd)
//   - Forces OPENSCARLETT_AUDIO_NULL=1 for deterministic CI-safe audio mixing
//   - Launches zero-DLL package directories purely through `openscarlett.exe run <package_dir>`
//   - Verifies untouched official MIT Microsoft Xbox-GDK-Samples (.mgc manifests + real C++ sources)
//     when packages/Xbox-GDK-Samples (or OPENSCARLETT_GDK_SAMPLES_DIR) is present on disk
//   - Strictly asserts not_impl == 0, ALL 19 Xbox/GDK shim DLLs present in unified CallTelemetry JSON,
//     XS2\0 auto-lift + cross-process disk shader cache hit, and 100% byte-identical recursive package hashes!

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

#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/vfs_mounts.h"
#include "openscarlett/middleware/bink2_decoder.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

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

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }

    uint64_t GetCallCount(const std::string& mod, const std::string& sym) const {
        uint64_t sum = 0;
        for (const auto& r : rows) {
            if ((mod.empty() || r.module == mod) && r.symbol == sym) {
                sum += r.total;
            }
        }
        return sum;
    }
};

struct DirSnapshot {
    uint64_t fnv1a_hash = 0;
    std::vector<std::string> entries;
};

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

std::wstring ReadEnvW(const wchar_t* name) {
    const DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring s(n, L'\0');
    const DWORD w = ::GetEnvironmentVariableW(name, s.data(), n);
    if (w == 0 || w >= n) return {};
    s.resize(w);
    return s;
}

fs::path ResolveMsGdkSamplesDir(const fs::path& bin_dir) {
    std::error_code ec;
    const std::wstring env_dir = ReadEnvW(L"OPENSCARLETT_GDK_SAMPLES_DIR");
    if (!env_dir.empty()) {
        const fs::path p(env_dir);
        return fs::exists(p / "Samples" / "System", ec) ? p : fs::path{};
    }
    // Walk up from executable directory (covers build/, .worktrees/<name>/build/, and repo root)
    fs::path cur = bin_dir;
    for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
        const fs::path cand = cur / "packages" / "Xbox-GDK-Samples";
        if (fs::exists(cand / "Samples" / "System", ec)) {
            return cand;
        }
        const fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

std::string ReadFileBytes(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

void WriteBinaryFile(const fs::path& p, const void* data, size_t size) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    OS_REQUIRE(static_cast<bool>(ofs));
}

void WriteTextFile(const fs::path& p, const std::string& text) {
    WriteBinaryFile(p, text.data(), text.size());
}

void AssertNoDllsInDir(const fs::path& dir) {
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");
    }
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

    std::vector<fs::path> rels;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        rels.push_back(fs::relative(entry.path(), root));
    }
    std::sort(rels.begin(), rels.end());

    for (const auto& rel : rels) {
        const fs::path full = root / rel;
        const std::string rel_s = rel.generic_string();
        if (fs::is_directory(full)) {
            const std::string tag = "D:" + rel_s;
            snap.entries.push_back(tag);
            feed(tag.data(), tag.size());
        } else {
            const std::string bytes = ReadFileBytes(full);
            const std::string tag = "F:" + rel_s + ":" + std::to_string(bytes.size());
            snap.entries.push_back(tag);
            feed(tag.data(), tag.size());
            feed(bytes.data(), bytes.size());
        }
    }
    snap.fnv1a_hash = h;
    return snap;
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
    std::istringstream iss(ReadFileBytes(json_path));
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

DWORD RunOpenScarlettPackageDir(
    const fs::path& cli_exe,
    const fs::path& pkg_dir,
    const fs::path& shim_dir,
    const fs::path& vfs_t,
    const fs::path& telemetry_json,
    const std::wstring& guest_args = {}) {
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + pkg_dir.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --telemetry \"" + telemetry_json.wstring() +
        L"\" --timeout-ms 45000";
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    OS_REQUIRE(::CreateProcessW(
                   cli_exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                   0, nullptr, nullptr, &si, &pi) != 0);
    OS_REQUIRE(::WaitForSingleObject(pi.hProcess, 50000u) == WAIT_OBJECT_0);
    DWORD rc = 999u;
    OS_REQUIRE(::GetExitCodeProcess(pi.hProcess, &rc) != 0);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return rc;
}

void VerifyStandalonePeImports(const fs::path& exe_path, const std::vector<std::string>& required_dlls) {
    openscarlett::PeInspectionReport rep{};
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

void WipePerTitleOverlayAndSave(const wchar_t* title_folder) {
    std::error_code ec;
    const std::wstring env_temp = ReadEnvW(L"TEMP");
    if (!env_temp.empty()) {
        fs::remove_all(fs::path(env_temp) / L"vfs" / L"G_overlay" / title_folder, ec);
    }
    const fs::path default_vfs = openscarlett::ResolveDefaultVfsRootW(ResolveRuntimeBinDir().wstring());
    if (!default_vfs.empty()) {
        fs::remove_all(default_vfs / L"G_overlay" / title_folder, ec);
        fs::remove_all(default_vfs / L"S" / title_folder, ec);
    }
    const std::wstring env_save = ReadEnvW(L"OPENSCARLETT_SAVE_ROOT");
    if (!env_save.empty()) {
        fs::remove_all(fs::path(env_save) / title_folder, ec);
    }
}

std::vector<uint8_t> BuildComputeAddMulXs2() {
    using namespace openscarlett;
    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::LiteralU32(5u));
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::LiteralU32(17u), 3);
    a.EmitVop3(Rdna2Opcode::V_BFI_B32, 4, Rdna2Operand::LiteralU32(0x00FFFF00u),
               Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(2));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
}

std::vector<uint8_t> MakeSolidRgba(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
        px[i * 4u + 0u] = r;
        px[i * 4u + 1u] = g;
        px[i * 4u + 2u] = b;
        px[i * 4u + 3u] = 255u;
    }
    return px;
}

void AssertZeroBlockers(const TelemetrySummary& tel, const char* label) {
    if (tel.total_not_impl != 0ull) {
        std::fprintf(stderr, "[FAIL] %s reported %llu not_impl calls:\n",
                     label, static_cast<unsigned long long>(tel.total_not_impl));
        for (const auto& r : tel.rows) {
            if (r.not_impl > 0) {
                std::fprintf(stderr, "  BLOCKER: %s!%s (not_impl=%llu)\n",
                             r.module.c_str(), r.symbol.c_str(),
                             static_cast<unsigned long long>(r.not_impl));
            }
        }
    }
    OS_REQUIRE(tel.total_not_impl == 0ull);
}

}  // namespace

int main() {
    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_AUDIO_NULL", L"1") != 0);
    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_ASYNC_PSO", L"0") != 0);
    // Clear inherited VFS overrides so MicrosoftGame.Config per-TitleId auto-derivation runs cleanly
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G_OVERLAY", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_S", nullptr);

    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = bin_dir / "openscarlett.exe";
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(bin_dir / "openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "gdk_external_dstorage_async.exe"));
    OS_REQUIRE(fs::exists(bin_dir / "gdk_external_full_stack.exe"));

    const fs::path scratch_root = ResolveScratchDir(L"openscarlett_wave2_external_gdk");
    std::error_code ec;
    fs::remove_all(scratch_root, ec);
    fs::create_directories(scratch_root, ec);
    OS_REQUIRE(fs::exists(scratch_root));

    const fs::path shader_cache_dir = scratch_root / "shader_cache";
    fs::create_directories(shader_cache_dir, ec);
    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_SHADER_CACHE", shader_cache_dir.c_str()) != 0);

    // Probe host HW D3D12 GPU availability (supports both GTX 1650 Ti HW and WARP-only CI runners)
    bool has_hw_gpu = false;
    std::string hw_name = "none (WARP-only CI)";
    {
        std::vector<openscarlett::D3D12AdapterInfo> adapters;
        if (openscarlett::EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp) {
                    has_hw_gpu = true;
                    hw_name = a.description;
                    break;
                }
            }
        }
    }

    std::printf("=== OpenScarlett Wave 2: External GDK-Header Binary & 19-DLL Live Bring-Up ===\n");
    std::printf("Bin Dir   : %s\n", bin_dir.string().c_str());
    std::printf("Scratch   : %s\n", scratch_root.string().c_str());
    std::printf("Host GPU  : %s\n", hw_name.c_str());

    // =========================================================================
    // 0. Official MIT Microsoft Xbox-GDK-Samples .mgc Manifest Verification
    // =========================================================================
    const fs::path ms_samples_root = ResolveMsGdkSamplesDir(bin_dir);
    const fs::path ms_sys_samples = ms_samples_root / "Samples" / "System";
    const fs::path ms_ds_mgc = ms_sys_samples / "SimpleDirectStorageCombo" / "MicrosoftGameConfig.mgc";
    const fs::path ms_gs_mgc = ms_sys_samples / "GameSaveCombo" / "MicrosoftGameConfig.mgc";
    const fs::path ms_ap_mgc = ms_sys_samples / "AsynchronousProgramming" / "MicrosoftGameConfig.mgc";
    const bool has_official_mgc =
        !ms_samples_root.empty() && fs::exists(ms_ds_mgc) && fs::exists(ms_gs_mgc) && fs::exists(ms_ap_mgc);
    if (!ms_samples_root.empty()) {
        OS_REQUIRE(has_official_mgc);
    }
    if (has_official_mgc) {
        openscarlett::GameConfig cfg_ds{}, cfg_gs{}, cfg_ap{};
        OS_REQUIRE(openscarlett::LoadGameConfigFile(ms_ds_mgc.string(), cfg_ds).ok());
        OS_REQUIRE(cfg_ds.SelectPrimaryExecutable() != nullptr);
        OS_REQUIRE(cfg_ds.SelectPrimaryExecutable()->name == "SimpleDirectStorageCombo_xs.exe");
        OS_REQUIRE(cfg_ds.SelectPrimaryExecutable()->target_device_family == "Scarlett");

        OS_REQUIRE(openscarlett::LoadGameConfigFile(ms_gs_mgc.string(), cfg_gs).ok());
        OS_REQUIRE(cfg_gs.has_title_id && cfg_gs.title_id_value == 0x7E750470u);
        OS_REQUIRE(cfg_gs.SelectPrimaryExecutable() != nullptr);
        OS_REQUIRE(cfg_gs.SelectPrimaryExecutable()->name == "GameSaveCombo.exe");

        OS_REQUIRE(openscarlett::LoadGameConfigFile(ms_ap_mgc.string(), cfg_ap).ok());
        OS_REQUIRE(cfg_ap.SelectPrimaryExecutable() != nullptr);
        OS_REQUIRE(cfg_ap.SelectPrimaryExecutable()->name == "AsynchronousProgramming.exe");

        std::printf("[PASS] Verified 3/3 untouched official Microsoft .mgc manifests "
                    "(SimpleDirectStorageCombo_xs.exe, GameSaveCombo.exe [0x%08X], AsynchronousProgramming.exe)\n",
                    cfg_gs.title_id_value);
    }

    // =========================================================================
    // 1. External Binary #1: gdk_external_dstorage_async.exe (Zero-DLL Package Dir)
    // =========================================================================
    {
        const fs::path pkg_dir = scratch_root / "pkg_dstorage_async";
        fs::create_directories(pkg_dir, ec);

        fs::path staged_exe;
        if (has_official_mgc) {
            // Stage the 100% UNMODIFIED official Microsoft GameSaveCombo/MicrosoftGameConfig.mgc!
            fs::copy_file(ms_gs_mgc, pkg_dir / "MicrosoftGameConfig.mgc",
                          fs::copy_options::overwrite_existing);
            staged_exe = pkg_dir / "GameSaveCombo.exe";
        } else {
            WriteTextFile(
                pkg_dir / "MicrosoftGame.Config",
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<Game configVersion=\"1\">\n"
                "  <Identity Name=\"OpenScarlett.ExternalDStorageAsync\" Publisher=\"CN=OpenScarlett\" Version=\"1.0.0.0\"/>\n"
                "  <TitleId>7E750470</TitleId>\n"
                "  <ExecutableList>\n"
                "    <Executable Name=\"GameSaveCombo.exe\" Id=\"Game\" TargetDeviceFamily=\"Scarlett\"/>\n"
                "  </ExecutableList>\n"
                "</Game>\n");
            staged_exe = pkg_dir / "GameSaveCombo.exe";
        }
        fs::copy_file(bin_dir / "gdk_external_dstorage_async.exe", staged_exe,
                      fs::copy_options::overwrite_existing);
        VerifyStandalonePeImports(staged_exe, {
            "xgameruntime.dll",
            "dstorage_xs.dll",
            "GameInput.dll",
            "pixevt.dll",
        });

        // Stage G:\ds_payload.bin (128 KiB sequential uint32_t) and G:\ds_compressed.gdeflate (64 KiB)
        std::vector<uint32_t> seq_u32(32768u);
        for (uint32_t i = 0; i < seq_u32.size(); ++i) seq_u32[i] = i;
        WriteBinaryFile(pkg_dir / "ds_payload.bin", seq_u32.data(), seq_u32.size() * sizeof(uint32_t));

        std::vector<uint8_t> gdef_plain(65536u);
        for (size_t i = 0; i < gdef_plain.size(); ++i) {
            gdef_plain[i] = static_cast<uint8_t>(((i * 7u) + (i / 32u) * 13u) & 0xFFu);
        }
        std::vector<uint8_t> gdef_comp;
        OS_REQUIRE(openscarlett::GDeflateCompress(gdef_plain.data(), gdef_plain.size(), gdef_comp));
        WriteBinaryFile(pkg_dir / "ds_compressed.gdeflate", gdef_comp.data(), gdef_comp.size());

        AssertNoDllsInDir(pkg_dir);
        WipePerTitleOverlayAndSave(L"7E750470");
        const DirSnapshot before = HashDirectoryTree(pkg_dir);

        const fs::path vfs_t = scratch_root / "run_dstorage_async_T";
        const fs::path tel_path = scratch_root / "telemetry_dstorage_async.json";
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(RunOpenScarlettPackageDir(cli_exe, pkg_dir, bin_dir, vfs_t, tel_path) == 0u);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        const DirSnapshot after = HashDirectoryTree(pkg_dir);
        OS_REQUIRE(after.fnv1a_hash == before.fnv1a_hash);
        OS_REQUIRE(after.entries == before.entries);
        AssertNoDllsInDir(pkg_dir);
        OS_REQUIRE(fs::exists(vfs_t / "dstorage_async_metrics.txt"));

        const TelemetrySummary tel = ParseTelemetryJson(tel_path);
        AssertZeroBlockers(tel, "gdk_external_dstorage_async");
        OS_REQUIRE(tel.HasModule("xgameruntime.dll"));
        OS_REQUIRE(tel.HasModule("dstorage_xs.dll"));
        OS_REQUIRE(tel.HasModule("GameInput.dll"));
        OS_REQUIRE(tel.HasModule("pixevt.dll"));
        OS_REQUIRE(tel.HasModule("vfs"));
        OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "XAsyncRun") >= 1u);
        OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "XTaskQueueCreateComposite") >= 1u);
        OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "XTaskQueueRegisterWaiter") >= 1u);
        OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "XGameSaveEnumerateContainerInfoByName") >= 1u);
        OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "XGameSaveReadBlobDataAsync") >= 1u);
        OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "XUserGetGamerPictureAsync") >= 1u);
        OS_REQUIRE(tel.GetCallCount("dstorage_xs.dll", "IDStorageQueueX::EnqueueSetEvent") >= 1u);
        OS_REQUIRE(tel.GetCallCount("dstorage_xs.dll", "IDStorageQueueX::CancelRequestsWithTag") >= 1u);
        OS_REQUIRE(tel.GetCallCount("GameInput.dll", "GameInputCreate") >= 1u);
        OS_REQUIRE(tel.GetCallCount("pixevt.dll", "PIXBeginEventOnCpu") >= 1u);

        std::printf("[PASS] External #1 (gdk_external_dstorage_async, %s): "
                    "%zu modules | %zu symbols | %llu calls | not_impl=0 | pkg=0x%016llX | %.1f ms\n",
                    has_official_mgc ? "untouched official GameSaveCombo.mgc" : "CI MicrosoftGame.Config",
                    tel.modules.size(), tel.rows.size(),
                    static_cast<unsigned long long>(tel.total_calls),
                    static_cast<unsigned long long>(after.fnv1a_hash),
                    elapsed_ms);
    }

    // =========================================================================
    // 2. Untouched Official MIT Microsoft SimpleDirectStorageCombo C++ Sources
    //    (when packages/Xbox-GDK-Samples was available at build time)
    // =========================================================================
    const fs::path ms_real_exe = bin_dir / "gdk_ms_directstorage_real.exe";
    if (fs::exists(ms_real_exe) && has_official_mgc) {
        const fs::path pkg_dir = scratch_root / "pkg_ms_directstorage_real";
        fs::create_directories(pkg_dir, ec);

        // 100% UNMODIFIED official Microsoft SimpleDirectStorageCombo/MicrosoftGameConfig.mgc!
        // OpenScarlett must pick TargetDeviceFamily="Scarlett" -> SimpleDirectStorageCombo_xs.exe!
        fs::copy_file(ms_ds_mgc, pkg_dir / "MicrosoftGameConfig.mgc",
                      fs::copy_options::overwrite_existing);
        const fs::path staged_exe = pkg_dir / "SimpleDirectStorageCombo_xs.exe";
        fs::copy_file(ms_real_exe, staged_exe, fs::copy_options::overwrite_existing);
        VerifyStandalonePeImports(staged_exe, {"dstorage_xs.dll"});

        // Stage 4 MiB sequential uint32_t datafile.dat matching Microsoft's VALIDATE_READS formula!
        constexpr uint32_t kNumU32 = 1024u * 1024u;  // 4 MiB
        std::vector<uint32_t> datafile(kNumU32);
        for (uint32_t i = 0; i < kNumU32; ++i) datafile[i] = i;
        WriteBinaryFile(pkg_dir / "datafile.dat", datafile.data(), datafile.size() * sizeof(uint32_t));

        AssertNoDllsInDir(pkg_dir);
        const DirSnapshot before = HashDirectoryTree(pkg_dir);

        const fs::path vfs_t = scratch_root / "run_ms_ds_real_T";
        const fs::path tel_path = scratch_root / "telemetry_ms_ds_real.json";
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(RunOpenScarlettPackageDir(cli_exe, pkg_dir, bin_dir, vfs_t, tel_path) == 0u);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        const DirSnapshot after = HashDirectoryTree(pkg_dir);
        OS_REQUIRE(after.fnv1a_hash == before.fnv1a_hash);
        OS_REQUIRE(after.entries == before.entries);
        AssertNoDllsInDir(pkg_dir);

        const TelemetrySummary tel = ParseTelemetryJson(tel_path);
        AssertZeroBlockers(tel, "gdk_ms_directstorage_real");
        OS_REQUIRE(tel.HasModule("dstorage_xs.dll"));
        OS_REQUIRE(tel.GetCallCount("dstorage_xs.dll", "DStorageGetFactory") >= 2u);
        OS_REQUIRE(tel.GetCallCount("dstorage_xs.dll", "IDStorageQueueX::EnqueueRequest") >= 100u);
        OS_REQUIRE(tel.GetCallCount("dstorage_xs.dll", "IDStorageQueueX::EnqueueSetEvent") >= 10u);

        std::printf("[PASS] Official MIT MS SimpleDirectStorageCombo (untouched .cpp + .mgc): "
                    "%zu modules | %zu symbols | %llu calls | not_impl=0 | pkg=0x%016llX | %.1f ms\n",
                    tel.modules.size(), tel.rows.size(),
                    static_cast<unsigned long long>(tel.total_calls),
                    static_cast<unsigned long long>(after.fnv1a_hash),
                    elapsed_ms);
    }

    // =========================================================================
    // 3. External Binary #2: gdk_external_full_stack.exe (ALL 19 Xbox/GDK DLLs + CoW + XS2 Auto-Lift)
    // =========================================================================
    {
        const fs::path pkg_dir = scratch_root / "pkg_full_stack_19";
        fs::create_directories(pkg_dir, ec);

        WriteTextFile(
            pkg_dir / "MicrosoftGame.Config",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<Game configVersion=\"1\">\n"
            "  <Identity Name=\"OpenScarlett.ExternalFullStack\" Publisher=\"CN=OpenScarlett\" Version=\"1.0.0.0\"/>\n"
            "  <TitleId>5CA02002</TitleId>\n"
            "  <ExecutableList>\n"
            "    <Executable Name=\"ExternalFullStack19_xs.exe\" Id=\"Game\" TargetDeviceFamily=\"Scarlett\"/>\n"
            "  </ExecutableList>\n"
            "</Game>\n");

        const fs::path staged_exe = pkg_dir / "ExternalFullStack19_xs.exe";
        fs::copy_file(bin_dir / "gdk_external_full_stack.exe", staged_exe,
                      fs::copy_options::overwrite_existing);

        // Verify all 17 statically imported Xbox/GDK DLLs (kernelx.dll + xmem.dll loaded dynamically)
        VerifyStandalonePeImports(staged_exe, {
            "d3d12_xs.dll",
            "xg_xs.dll",
            "xgameruntime.dll",
            "GameInput.dll",
            "XAudio2_9.dll",
            "AcpHal.dll",
            "dstorage_xs.dll",
            "winrt_xs.dll",
            "Microsoft.Xbox.Services.GDK.C.dll",
            "pixevt.dll",
            "xcrdapi.dll",
            "libHttpClient.GDK.dll",
            "XCurl.dll",
            "Bink2Scarlett.dll",
            "amd_fidelityfx_upscaler_xbox.dll",
            "amd_fidelityfx_loader_xbox.dll",
            "XboxBvhBuilder.dll",
        });

        // Stage raw + GDeflate DirectStorage assets
        std::vector<uint8_t> raw_asset(16384u);
        for (size_t i = 0; i < raw_asset.size(); ++i) raw_asset[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
        WriteBinaryFile(pkg_dir / "asset.bin", raw_asset.data(), raw_asset.size());

        std::vector<uint8_t> gdef_plain(65536u);
        for (size_t i = 0; i < gdef_plain.size(); ++i) {
            gdef_plain[i] = static_cast<uint8_t>(((i * 5u) + (i / 64u) * 19u) & 0xFFu);
        }
        std::vector<uint8_t> gdef_comp;
        OS_REQUIRE(openscarlett::GDeflateCompress(gdef_plain.data(), gdef_plain.size(), gdef_comp));
        WriteBinaryFile(pkg_dir / "asset.gdeflate", gdef_comp.data(), gdef_comp.size());

        // Stage XMA2 (0x0166)
        {
            openscarlett::Xma2Config xcfg{};
            xcfg.sample_rate = 48000u;
            xcfg.channels = 1u;
            std::vector<float> sine(4096u);
            for (size_t i = 0; i < sine.size(); ++i) {
                sine[i] = 0.6f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
            }
            std::vector<uint8_t> xma_pkts;
            OS_REQUIRE(openscarlett::Xma2EncodePcmToPackets(sine.data(), sine.size(), xcfg, xma_pkts).ok());
            WriteBinaryFile(pkg_dir / "sine440.xma2", xma_pkts.data(), xma_pkts.size());
        }
        // Stage IMA-ADPCM (0x0011 stereo) & Xbox ADPCM (0x0069 mono)
        {
            constexpr size_t kImaFrames = 2020u;
            std::vector<float> stereo(kImaFrames * 2u);
            for (size_t i = 0; i < kImaFrames; ++i) {
                stereo[i * 2u] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
                stereo[i * 2u + 1u] = 0.5f * std::sin(2.0f * 3.14159265f * 660.0f * static_cast<float>(i) / 48000.0f);
            }
            openscarlett::ImaAdpcmConfig icfg{48000u, 2u, 0u, 0u, false};
            std::vector<uint8_t> ima_blocks;
            OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(stereo.data(), kImaFrames, icfg, ima_blocks).ok());
            WriteBinaryFile(pkg_dir / "tone_ima.adpcm", ima_blocks.data(), ima_blocks.size());

            constexpr size_t kXFrames = 65u * 24u;
            std::vector<float> mono(kXFrames);
            for (size_t i = 0; i < kXFrames; ++i) {
                mono[i] = 0.55f * std::sin(2.0f * 3.14159265f * 520.0f * static_cast<float>(i) / 48000.0f);
            }
            openscarlett::ImaAdpcmConfig xacfg{48000u, 1u, 0u, 0u, true};
            std::vector<uint8_t> xadpcm_blocks;
            OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(mono.data(), kXFrames, xacfg, xadpcm_blocks).ok());
            WriteBinaryFile(pkg_dir / "tone_xadpcm.adpcm", xadpcm_blocks.data(), xadpcm_blocks.size());
        }
        // Stage Opus (0x704F length-prefixed RFC 6716 CELT packets)
        {
            std::vector<std::vector<uint8_t>> pkts;
            for (uint32_t i = 0; i < 6u; ++i) {
                pkts.push_back(openscarlett::BuildOpusTestPacket(2u, i + 1u, 64u));
            }
            const auto opus_stream = openscarlett::PackOpusLengthPrefixedStream(pkts, false);
            WriteBinaryFile(pkg_dir / "tone_opus.opus", opus_stream.data(), opus_stream.size());
        }
        // Stage Bink2 KB2i clip
        {
            std::vector<std::vector<uint8_t>> frames = {
                MakeSolidRgba(32u, 32u, 220u, 40u, 50u),
                MakeSolidRgba(32u, 32u, 40u, 210u, 60u),
                MakeSolidRgba(32u, 32u, 50u, 70u, 230u),
            };
            openscarlett::Bink2EncodeParams bp{};
            bp.width = 32u;
            bp.height = 32u;
            bp.fps_num = 60u;
            bp.fps_den = 1u;
            bp.fourcc = "KB2i";
            bp.chroma_format = openscarlett::Bink2ChromaFormat::YCbCr420;
            bp.keyframe_interval = 2u;
            std::vector<uint8_t> bk2;
            OS_REQUIRE(openscarlett::Bink2EncodeTestStream(frames, bp, bk2).ok());
            WriteBinaryFile(pkg_dir / "clip.bk2", bk2.data(), bk2.size());
        }
        // Stage XS2\0 RDNA2 compute shader
        {
            const auto xs2 = BuildComputeAddMulXs2();
            WriteBinaryFile(pkg_dir / "compute_addmul.xs2", xs2.data(), xs2.size());
        }
        // Stage CoW test files inside read-only G:\ package
        WriteTextFile(pkg_dir / "cow_base.txt", "ORIGINAL_PACKAGE_V1");
        WriteTextFile(pkg_dir / "cow_move_src.txt", "MOVE_SRC_DATA");
        WriteTextFile(pkg_dir / "cow_delete_me.txt", "DELETE_TARGET_BYTES");

        AssertNoDllsInDir(pkg_dir);
        const DirSnapshot pkg_before = HashDirectoryTree(pkg_dir);

        const std::vector<const char*> all_19_shims = {
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
            "pixevt.dll",
            "xmem.dll",
            "xcrdapi.dll",
            "libHttpClient.GDK.dll",
            "XCurl.dll",
            "Bink2Scarlett.dll",
            "amd_fidelityfx_loader_xbox.dll",
            "amd_fidelityfx_upscaler_xbox.dll",
            "XboxBvhBuilder.dll",
        };

        auto run_full_stack = [&](const char* label, bool warp, bool expect_disk_hit, const wchar_t* tag) {
            WipePerTitleOverlayAndSave(L"5CA02002");
            const fs::path vfs_t = scratch_root / (std::wstring(L"run_fs_") + tag + L"_T");
            const fs::path tel_path = scratch_root / (std::wstring(L"telemetry_fs_") + tag + L".json");

            const auto t0 = std::chrono::steady_clock::now();
            OS_REQUIRE(RunOpenScarlettPackageDir(
                           cli_exe, pkg_dir, bin_dir, vfs_t, tel_path,
                           warp ? L"--warp" : L"") == 0u);
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

            // 100% byte-identical package verification after CoW writes/moves/deletes!
            const DirSnapshot pkg_after = HashDirectoryTree(pkg_dir);
            OS_REQUIRE(pkg_after.fnv1a_hash == pkg_before.fnv1a_hash);
            OS_REQUIRE(pkg_after.entries == pkg_before.entries);
            AssertNoDllsInDir(pkg_dir);
            OS_REQUIRE(fs::exists(vfs_t / "full_stack_metrics.txt"));
            OS_REQUIRE(fs::exists(vfs_t / "xcrd_fullstack.json"));

            const TelemetrySummary tel = ParseTelemetryJson(tel_path);
            AssertZeroBlockers(tel, label);

            // Strictly assert ALL 19 Xbox/GDK shim DLLs + vfs + loader appear in telemetry!
            size_t shims_hit = 0;
            for (const char* mod : all_19_shims) {
                if (!tel.HasModule(mod)) {
                    std::fprintf(stderr, "[FAIL] %s missing telemetry module: %s\n", label, mod);
                }
                OS_REQUIRE(tel.HasModule(mod));
                ++shims_hit;
            }
            OS_REQUIRE(shims_hit == 19u);
            OS_REQUIRE(tel.HasModule("vfs"));
            OS_REQUIRE(tel.HasModule("loader"));

            // Verify critical symbols across subsystems + shader cache hit mode
            OS_REQUIRE(tel.GetCallCount("xgameruntime.dll", "QueryApiImpl") >= 8u);
            OS_REQUIRE(tel.GetCallCount("xmem.dll", "XMemAlloc") >= 1u);
            OS_REQUIRE(tel.GetCallCount("kernelx.dll", "XMemAlloc") >= 1u);
            OS_REQUIRE(tel.GetCallCount("xg_xs.dll", "XGTileSurface3D") >= 1u);
            OS_REQUIRE(tel.GetCallCount("amd_fidelityfx_loader_xbox.dll", "ffxDispatch") >= 1u);
            OS_REQUIRE(tel.GetCallCount("amd_fidelityfx_upscaler_xbox.dll", "ffxUpscalerDispatchRgba8") >= 1u);
            OS_REQUIRE(tel.GetCallCount("XboxBvhBuilder.dll", "XboxBvhDispatchRays") >= 2u);
            OS_REQUIRE(tel.GetCallCount("Bink2Scarlett.dll", "BinkDoFrame") >= 1u);
            OS_REQUIRE(tel.GetCallCount("libHttpClient.GDK.dll", "HCHttpCallPerformAsync") >= 1u);
            OS_REQUIRE(tel.GetCallCount("XCurl.dll", "curl_multi_perform") >= 1u);
            OS_REQUIRE(tel.GetCallCount("xcrdapi.dll", "XCrdFlushDiagnostics") >= 1u);
            OS_REQUIRE(tel.GetCallCount("pixevt.dll", "PIXBeginEventOnCpu") >= 1u);
            OS_REQUIRE(tel.GetCallCount("vfs", "DeleteFileW") >= 1u);
            OS_REQUIRE(tel.GetCallCount("vfs", "MoveFileExW") >= 1u);

            const uint64_t sc_miss = tel.GetCallCount("shader_cache", "CacheMiss");
            const uint64_t sc_mem = tel.GetCallCount("shader_cache", "MemoryHit");
            const uint64_t sc_disk = tel.GetCallCount("shader_cache", "DiskHit");
            OS_REQUIRE(sc_mem >= 1u);
            // Internal shader compiler helpers (dxc.exe) must NEVER be APC-injected as guest children!
            OS_REQUIRE(tel.GetCallCount("loader", "CreateProcessA:child_hook_propagated") == 0u);
            if (expect_disk_hit) {
                OS_REQUIRE(sc_disk >= 1u);
                OS_REQUIRE(sc_miss == 0u);
            } else {
                OS_REQUIRE(sc_miss >= 1u);
                OS_REQUIRE(tel.GetCallCount("rdna2_lifter", "LiftRdna2Xs2Shader") >= 1u);
                if (tel.GetCallCount("rdna2_lifter", "CompileHlslWithDxc") >= 1u) {
                    OS_REQUIRE(tel.GetCallCount("loader", "CreateProcessA:child_skipped_toolchain") >= 1u);
                }
            }

            std::printf("[PASS] External #2 (%s): %zu/19 shims + vfs/loader | %zu symbols | %llu calls | "
                        "not_impl=0 | Cache(miss=%llu,mem=%llu,disk=%llu) | pkg=0x%016llX | %.1f ms\n",
                        label, shims_hit, tel.rows.size(),
                        static_cast<unsigned long long>(tel.total_calls),
                        static_cast<unsigned long long>(sc_miss),
                        static_cast<unsigned long long>(sc_mem),
                        static_cast<unsigned long long>(sc_disk),
                        static_cast<unsigned long long>(pkg_after.fnv1a_hash),
                        elapsed_ms);
        };

        if (has_hw_gpu) {
            const fs::path hw_cache = shader_cache_dir / "hw";
            fs::remove_all(hw_cache, ec);
            fs::create_directories(hw_cache, ec);
            OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_SHADER_CACHE", hw_cache.c_str()) != 0);
            run_full_stack("FullStack19 HW Cold", /*warp=*/false, /*expect_disk_hit=*/false, L"hw_cold");
            run_full_stack("FullStack19 HW Warm", /*warp=*/false, /*expect_disk_hit=*/true, L"hw_warm");
        }
        {
            const fs::path warp_cache = shader_cache_dir / "warp";
            fs::remove_all(warp_cache, ec);
            fs::create_directories(warp_cache, ec);
            OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_SHADER_CACHE", warp_cache.c_str()) != 0);
            run_full_stack("FullStack19 WARP Cold", /*warp=*/true, /*expect_disk_hit=*/false, L"warp_cold");
            run_full_stack("FullStack19 WARP Warm", /*warp=*/true, /*expect_disk_hit=*/true, L"warp_warm");
        }
    }

    std::printf("ALL WAVE 2 EXTERNAL GDK-HEADER BRING-UP & 19-DLL TELEMETRY TESTS PASSED (not_impl == 0)!\n");
    return 0;
}
