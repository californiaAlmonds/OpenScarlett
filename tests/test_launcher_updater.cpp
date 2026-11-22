// OpenScarlett Sprint 7 (v0.4.0-alpha) — Comprehensive Launcher, Updater,
// BCrypt SHA-256 Verification, Privacy Sanitization, GPU Probe, PE Inspector,
// and Live Win32/Direct2D GUI Smoke Test Suite.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "launcher_resource.h"
#include "openscarlett/launcher/hwtest_runner.h"
#include "openscarlett/launcher/updater.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

using namespace openscarlett::launcher;

fs::path ResolveBinaryDir(const char* argv0) {
    if (argv0 && argv0[0] != '\0') {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
        if (!p.empty() && fs::exists(p / "openscarlett.exe", ec)) {
            return p;
        }
    }
#ifdef OPENSCARLETT_BINARY_DIR
    {
        std::error_code ec;
        fs::path cfg(OPENSCARLETT_BINARY_DIR);
        if (fs::exists(cfg / "openscarlett.exe", ec)) {
            return cfg;
        }
    }
#endif
    return fs::current_path();
}

// Keep all scratch files inside bin_dir (always on E:\) — NEVER write to C:\.
fs::path ResolveScratchDir(const fs::path& bin_dir) {
    fs::path dir = bin_dir / "_tmp_test_launcher_updater";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// Standard CRC-32 (IEEE 802.3) for building a valid minimal stored PKZIP archive
// without any third-party compression libraries.
uint32_t ComputeCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

template <typename T>
void AppendLe(std::vector<uint8_t>& out, T val) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(val) >> (i * 8u)) & 0xFFu));
    }
}

// Builds a standard valid single-file uncompressed (method 0 / STORE) PKZIP archive.
std::vector<uint8_t> BuildMinimalStoredZip(const std::string& entry_name,
                                           const std::string& entry_content) {
    std::vector<uint8_t> z;
    const uint32_t crc = ComputeCrc32(
        reinterpret_cast<const uint8_t*>(entry_content.data()), entry_content.size());
    const uint32_t sz = static_cast<uint32_t>(entry_content.size());
    const uint16_t nlen = static_cast<uint16_t>(entry_name.size());

    // Local File Header
    AppendLe<uint32_t>(z, 0x04034B50u);
    AppendLe<uint16_t>(z, 20u);   // version needed
    AppendLe<uint16_t>(z, 0u);    // flags
    AppendLe<uint16_t>(z, 0u);    // compression = STORE
    AppendLe<uint16_t>(z, 0u);    // mod time
    AppendLe<uint16_t>(z, 0x5421u);  // mod date
    AppendLe<uint32_t>(z, crc);
    AppendLe<uint32_t>(z, sz);
    AppendLe<uint32_t>(z, sz);
    AppendLe<uint16_t>(z, nlen);
    AppendLe<uint16_t>(z, 0u);    // extra len
    z.insert(z.end(), entry_name.begin(), entry_name.end());
    z.insert(z.end(), entry_content.begin(), entry_content.end());

    const uint32_t cd_offset = static_cast<uint32_t>(z.size());

    // Central Directory Header
    AppendLe<uint32_t>(z, 0x02014B50u);
    AppendLe<uint16_t>(z, 20u);
    AppendLe<uint16_t>(z, 20u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0x5421u);
    AppendLe<uint32_t>(z, crc);
    AppendLe<uint32_t>(z, sz);
    AppendLe<uint32_t>(z, sz);
    AppendLe<uint16_t>(z, nlen);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint32_t>(z, 0u);
    AppendLe<uint32_t>(z, 0u);  // local header offset
    z.insert(z.end(), entry_name.begin(), entry_name.end());

    const uint32_t cd_size = static_cast<uint32_t>(z.size()) - cd_offset;

    // End of Central Directory Record
    AppendLe<uint32_t>(z, 0x06054B50u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 0u);
    AppendLe<uint16_t>(z, 1u);
    AppendLe<uint16_t>(z, 1u);
    AppendLe<uint32_t>(z, cd_size);
    AppendLe<uint32_t>(z, cd_offset);
    AppendLe<uint16_t>(z, 0u);
    return z;
}

struct ChildProcessResult {
    bool launched = false;
    DWORD exit_code = 1;
    std::string stdout_text;
};

ChildProcessResult RunChildCapture(const fs::path& exe_path,
                                   const std::wstring& args,
                                   const std::vector<std::pair<std::wstring, std::wstring>>& env_overrides,
                                   DWORD timeout_ms = 15000u) {
    ChildProcessResult res{};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return res;
    }
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    // Temporarily apply environment overrides (restored immediately after CreateProcessW)
    struct SavedEnv {
        std::wstring key;
        bool existed = false;
        std::wstring value;
    };
    std::vector<SavedEnv> saved;
    for (const auto& kv : env_overrides) {
        SavedEnv s{};
        s.key = kv.first;
        wchar_t buf[1024]{};
        const DWORD n = ::GetEnvironmentVariableW(
            kv.first.c_str(), buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
        if (n > 0 && n < static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]))) {
            s.existed = true;
            s.value = buf;
        }
        saved.push_back(std::move(s));
        ::SetEnvironmentVariableW(kv.first.c_str(), kv.second.c_str());
    }

    std::wstring cmdline = L"\"" + exe_path.wstring() + L"\" " + args;
    std::vector<wchar_t> cmd_mut(cmdline.begin(), cmdline.end());
    cmd_mut.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(), cmd_mut.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, exe_path.parent_path().c_str(), &si, &pi);

    // Restore parent environment immediately
    for (const auto& s : saved) {
        ::SetEnvironmentVariableW(s.key.c_str(), s.existed ? s.value.c_str() : nullptr);
    }
    ::CloseHandle(write_pipe);

    if (!ok) {
        ::CloseHandle(read_pipe);
        return res;
    }
    res.launched = true;

    char buf[1024]{};
    DWORD nread = 0;
    while (::ReadFile(read_pipe, buf, sizeof(buf) - 1, &nread, nullptr) && nread > 0) {
        res.stdout_text.append(buf, nread);
    }
    ::CloseHandle(read_pipe);

    const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 124u);
        res.exit_code = 124u;
    } else {
        ::GetExitCodeProcess(pi.hProcess, &res.exit_code);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return res;
}

// ============================================================================
// Test 1: SemVer Parsing & Comparison
// ============================================================================
void TestSemVer() {
    const SemVer v031a = ParseSemVer("v0.3.1-alpha");
    const SemVer v040a = ParseSemVer("v0.4.0-alpha");
    const SemVer v040b = ParseSemVer("v0.4.0-beta");
    const SemVer v040r = ParseSemVer("v0.4.0-rc.1");
    const SemVer v040  = ParseSemVer("v0.4.0");
    const SemVer v041  = ParseSemVer("v0.4.1");
    const SemVer v100  = ParseSemVer("v1.0.0");

    OS_REQUIRE(v031a.valid && v040a.valid && v040b.valid && v040r.valid);
    OS_REQUIRE(v040.valid && v041.valid && v100.valid);

    OS_REQUIRE(CompareSemVer(v031a, v040a) < 0);
    OS_REQUIRE(CompareSemVer(v040a, v040b) < 0);
    OS_REQUIRE(CompareSemVer(v040b, v040r) < 0);
    OS_REQUIRE(CompareSemVer(v040r, v040) < 0);
    OS_REQUIRE(CompareSemVer(v040, v041) < 0);
    OS_REQUIRE(CompareSemVer(v041, v100) < 0);

    // Symmetry & equality (with and without 'v' prefix)
    OS_REQUIRE(CompareSemVer(v040a, ParseSemVer("0.4.0-alpha")) == 0);
    OS_REQUIRE(CompareSemVer(v100, ParseSemVer("1.0.0")) == 0);
    OS_REQUIRE(CompareSemVer(v100, v041) > 0);

    OS_REQUIRE(IsNewerVersion("v0.3.1-alpha", "v0.4.0-alpha"));
    OS_REQUIRE(IsNewerVersion("v0.4.0-alpha", "v0.4.0"));
    OS_REQUIRE(IsNewerVersion("v0.4.0", "v0.4.1"));
    OS_REQUIRE(IsNewerVersion("v0.4.1", "v1.0.0"));
    OS_REQUIRE(!IsNewerVersion("v0.4.0-alpha", "v0.4.0-alpha"));
    OS_REQUIRE(!IsNewerVersion("v0.4.0", "v0.4.0-alpha"));
    OS_REQUIRE(!IsNewerVersion("v1.0.0", "v0.9.9"));

    // Short major.minor normalization (e.g. "v1.2" -> 1.2.0) & invalid version rejection
    const SemVer v12 = ParseSemVer("v1.2");
    OS_REQUIRE(v12.valid && v12.major == 1 && v12.minor == 2 && v12.patch == 0);
    OS_REQUIRE(!ParseSemVer("").valid);
    OS_REQUIRE(!ParseSemVer("not-a-version").valid);
    OS_REQUIRE(!ParseSemVer("v1").valid);
    OS_REQUIRE(!ParseSemVer("v1.2.x").valid);
    OS_REQUIRE(!ParseSemVer("v-1.0.0").valid);
    OS_REQUIRE(!IsNewerVersion("v0.4.0-alpha", "garbage"));

    std::printf("[PASS] Test 1: SemVer parsing, prerelease ordering & invalid rejection\n");
}

// ============================================================================
// Test 2: GitHub Release JSON Parser
// ============================================================================
void TestGitHubReleaseJsonParser() {
    const std::string valid_json = R"JSON({
        "tag_name": "v0.5.0-alpha",
        "name": "OpenScarlett v0.5.0-alpha (Portable)",
        "html_url": "https://github.com/californiaAlmonds/OpenScarlett/releases/tag/v0.5.0-alpha",
        "published_at": "2026-11-22T12:00:00Z",
        "body": "## Highlights\r\n- Added \"Native\" Launcher\n- Fixed C:\\path\\escape\n- Verified SHA-256",
        "assets": [
            {
                "name": "OpenScarlett-v0.5.0-alpha-win64.zip",
                "browser_download_url": "https://github.com/californiaAlmonds/OpenScarlett/releases/download/v0.5.0-alpha/OpenScarlett-v0.5.0-alpha-win64.zip"
            },
            {
                "name": "OpenScarlett-v0.5.0-alpha-win64.zip.sha256",
                "browser_download_url": "https://github.com/californiaAlmonds/OpenScarlett/releases/download/v0.5.0-alpha/OpenScarlett-v0.5.0-alpha-win64.zip.sha256"
            }
        ]
    })JSON";

    UpdateReleaseInfo info{};
    OS_REQUIRE(ParseGitHubReleaseJson(valid_json, info).ok());
    OS_REQUIRE(info.tag_name == "v0.5.0-alpha");
    OS_REQUIRE(info.release_name == "OpenScarlett v0.5.0-alpha (Portable)");
    OS_REQUIRE(info.html_url.find("/releases/tag/v0.5.0-alpha") != std::string::npos);
    OS_REQUIRE(info.published_at == "2026-11-22T12:00:00Z");
    OS_REQUIRE(info.body_markdown.find("\"Native\" Launcher") != std::string::npos);
    OS_REQUIRE(info.body_markdown.find("C:\\path\\escape") != std::string::npos);
    OS_REQUIRE(info.zip_asset_name == "OpenScarlett-v0.5.0-alpha-win64.zip");
    OS_REQUIRE(info.zip_download_url.find(".zip") != std::string::npos);
    OS_REQUIRE(info.sha256_asset_name == "OpenScarlett-v0.5.0-alpha-win64.zip.sha256");
    OS_REQUIRE(info.sha256_download_url.find(".zip.sha256") != std::string::npos);

    // Missing asset handling (release parses tag_name, but zip/sha256 URLs remain empty)
    const std::string no_assets_json = R"({
        "tag_name": "v0.5.1",
        "name": "Source Only",
        "body": "No binaries attached",
        "assets": []
    })";
    UpdateReleaseInfo no_assets{};
    OS_REQUIRE(ParseGitHubReleaseJson(no_assets_json, no_assets).ok());
    OS_REQUIRE(no_assets.tag_name == "v0.5.1");
    OS_REQUIRE(no_assets.zip_download_url.empty());
    OS_REQUIRE(no_assets.sha256_download_url.empty());

    // Malformed / missing tag_name JSON must fail
    UpdateReleaseInfo bad{};
    OS_REQUIRE(!ParseGitHubReleaseJson("", bad).ok());
    OS_REQUIRE(!ParseGitHubReleaseJson("{not json", bad).ok());
    OS_REQUIRE(!ParseGitHubReleaseJson(R"({"name":"missing tag"})", bad).ok());

    std::printf("[PASS] Test 2: GitHub Release JSON parser (assets, escaped notes, missing/malformed handling)\n");
}

// ============================================================================
// Test 3: Windows BCrypt SHA-256, Staging, Tampered Zip & Offline Guards
// ============================================================================
void TestSha256AndUpdateStaging(const fs::path& scratch_dir) {
    // 3A. Known NIST SHA-256 vectors
    std::string hex_empty;
    OS_REQUIRE(ComputeBufferSha256Hex(nullptr, 0u, hex_empty).ok());
    OS_REQUIRE(hex_empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const uint8_t abc[3] = {'a', 'b', 'c'};
    std::string hex_abc;
    OS_REQUIRE(ComputeBufferSha256Hex(abc, sizeof(abc), hex_abc).ok());
    OS_REQUIRE(hex_abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 3B. Malformed checksum file rejection
    std::string parsed_hex;
    OS_REQUIRE(!ParseSha256ChecksumFile("", parsed_hex).ok());
    OS_REQUIRE(!ParseSha256ChecksumFile("deadbeef  short.zip", parsed_hex).ok());
    OS_REQUIRE(!ParseSha256ChecksumFile(
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz  bad.zip",
        parsed_hex).ok());

    // 3C. Valid local zip + .sha256 -> VerifyFileSha256 + VerifyAndStageLocalUpdateFiles
    const fs::path app_dir = scratch_dir / "mock_app";
    const fs::path dl_dir = scratch_dir / "mock_downloads";
    std::error_code ec;
    fs::create_directories(app_dir / "saves", ec);
    fs::create_directories(dl_dir, ec);
    {
        std::ofstream save_f(app_dir / "saves" / "slot0.sav", std::ios::binary);
        save_f << "USER_SAVE_DATA_MUST_SURVIVE";
    }

    const auto zip_bytes = BuildMinimalStoredZip("OpenScarlett_README.txt",
                                                 "OpenScarlett v0.5.0-alpha test payload\n");
    const fs::path zip_path = dl_dir / "OpenScarlett-v0.5.0-alpha-win64.zip";
    {
        std::ofstream ofs(zip_path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(zip_bytes.data()),
                  static_cast<std::streamsize>(zip_bytes.size()));
    }

    std::string zip_hash;
    OS_REQUIRE(ComputeFileSha256Hex(zip_path, zip_hash).ok());
    OS_REQUIRE(zip_hash.size() == 64u);

    const fs::path sha_path = dl_dir / "OpenScarlett-v0.5.0-alpha-win64.zip.sha256";
    {
        std::ofstream ofs(sha_path, std::ios::binary | std::ios::trunc);
        ofs << zip_hash << "  OpenScarlett-v0.5.0-alpha-win64.zip\n";
    }

    const auto ver_ok = VerifyFileSha256(zip_path, ReadTextFile(sha_path));
    OS_REQUIRE(ver_ok.ok);
    OS_REQUIRE(ver_ok.computed_hex == zip_hash);
    OS_REQUIRE(ver_ok.expected_hex == zip_hash);

    int progress_hits = 0;
    const auto stage_ok = VerifyAndStageLocalUpdateFiles(
        zip_path, sha_path, app_dir, "v0.5.0-alpha",
        [&](const UpdateDownloadProgress&) { ++progress_hits; });
    if (!stage_ok.ok) {
        std::fprintf(stderr, "VerifyAndStageLocalUpdateFiles failed: %s\n",
                     stage_ok.error_message.c_str());
    }
    OS_REQUIRE(stage_ok.ok);
    OS_REQUIRE(stage_ok.verified_sha256 == zip_hash);
    OS_REQUIRE(fs::exists(stage_ok.restart_script_path, ec));
    OS_REQUIRE(progress_hits > 0);

    const std::string script_txt = ReadTextFile(stage_ok.restart_script_path);
    OS_REQUIRE(!script_txt.empty());
    // Verify restart helper script is transparent batch script & preserves saves/config
    std::string script_lo = script_txt;
    std::transform(script_lo.begin(), script_lo.end(), script_lo.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    OS_REQUIRE(script_lo.find("saves") != std::string::npos);

    // 3D. Tampered zip rejection: flip 1 byte -> VerifyFileSha256 fails AND staging hard-fails & cleans temp!
    const fs::path tampered_zip = dl_dir / "tampered.zip";
    {
        auto bad_bytes = zip_bytes;
        bad_bytes[bad_bytes.size() / 2u] ^= 0x01u;
        std::ofstream ofs(tampered_zip, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(bad_bytes.data()),
                  static_cast<std::streamsize>(bad_bytes.size()));
    }
    const auto ver_bad = VerifyFileSha256(tampered_zip, ReadTextFile(sha_path));
    OS_REQUIRE(!ver_bad.ok);
    OS_REQUIRE(ver_bad.computed_hex != ver_bad.expected_hex);

    const fs::path tamper_app_dir = scratch_dir / "mock_app_tamper";
    fs::create_directories(tamper_app_dir, ec);
    const auto stage_bad = VerifyAndStageLocalUpdateFiles(
        tampered_zip, sha_path, tamper_app_dir, "v0.5.0-alpha");
    OS_REQUIRE(!stage_bad.ok);
    OS_REQUIRE(!stage_bad.error_message.empty());
    // Verify no leftover staged payload directory remains after tampered rejection
    if (!stage_bad.staging_dir.empty()) {
        OS_REQUIRE(!fs::exists(stage_bad.staging_dir, ec));
    }

    // 3E. Offline & OPENSCARLETT_NO_UPDATE_CHECK=1 guard (zero network I/O!)
    {
        wchar_t prev[64]{};
        const DWORD pn = ::GetEnvironmentVariableW(L"OPENSCARLETT_NO_UPDATE_CHECK", prev, 64);
        ::SetEnvironmentVariableW(L"OPENSCARLETT_NO_UPDATE_CHECK", L"1");
        OS_REQUIRE(IsUpdateCheckDisabledByEnv());
        const auto env_skip = CheckLatestGitHubRelease(kOpenScarlettAppVersion, false);
        OS_REQUIRE(env_skip.skipped_offline);

        ::SetEnvironmentVariableW(L"OPENSCARLETT_NO_UPDATE_CHECK", nullptr);
        OS_REQUIRE(!IsUpdateCheckDisabledByEnv());
        const auto flag_skip = CheckLatestGitHubRelease(kOpenScarlettAppVersion, true);
        OS_REQUIRE(flag_skip.skipped_offline);

        ::SetEnvironmentVariableW(L"OPENSCARLETT_NO_UPDATE_CHECK", pn > 0 ? prev : nullptr);
    }

    // 3F. Regression: paths with spaces/parentheses + poisoned PSModulePath (GitHub Actions pwsh7 edge case)
    {
        struct ScopedEnvOverride {
            std::wstring key;
            bool existed = false;
            std::wstring prev_val;
            ScopedEnvOverride(std::wstring k, const wchar_t* new_val) : key(std::move(k)) {
                const DWORD needed = ::GetEnvironmentVariableW(key.c_str(), nullptr, 0);
                if (needed > 0) {
                    prev_val.resize(needed);
                    const DWORD got = ::GetEnvironmentVariableW(key.c_str(), prev_val.data(), needed);
                    if (got > 0 && got < needed) {
                        prev_val.resize(got);
                        existed = true;
                    }
                }
                ::SetEnvironmentVariableW(key.c_str(), new_val);
            }
            ~ScopedEnvOverride() {
                ::SetEnvironmentVariableW(key.c_str(), existed ? prev_val.c_str() : nullptr);
            }
        };

        const fs::path spaced_root = scratch_dir / "CI Dir With Spaces (x64 & Release)";
        const fs::path spaced_app = spaced_root / "OpenScarlett App (Portable)";
        const fs::path spaced_dl = spaced_root / "Download Cache (Spaces)";
        fs::create_directories(spaced_app / "saves", ec);
        fs::create_directories(spaced_dl, ec);

        const std::string spaced_payload = "OpenScarlett spaced-path + poisoned PSModulePath payload\n";
        const auto spaced_zip_bytes = BuildMinimalStoredZip("OpenScarlett_Spaced_Test.txt", spaced_payload);
        const fs::path spaced_zip = spaced_dl / "OpenScarlett (v0.5.1) win64.zip";
        {
            std::ofstream ofs(spaced_zip, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(spaced_zip_bytes.data()),
                      static_cast<std::streamsize>(spaced_zip_bytes.size()));
        }

        std::string spaced_hash;
        OS_REQUIRE(ComputeFileSha256Hex(spaced_zip, spaced_hash).ok());
        const fs::path spaced_sha = spaced_dl / "OpenScarlett (v0.5.1) win64.zip.sha256";
        {
            std::ofstream ofs(spaced_sha, std::ios::binary | std::ios::trunc);
            ofs << spaced_hash << "  OpenScarlett (v0.5.1) win64.zip\n";
        }

        // Poison PSModulePath like a GitHub Actions pwsh7 step + bogus nonexistent path.
        ScopedEnvOverride poison_ps(
            L"PSModulePath",
            L"C:\\Program Files\\PowerShell\\7\\Modules;Z:\\nonexistent_poisoned_psmodulepath");

        int spaced_progress = 0;
        const auto stage_spaced = VerifyAndStageLocalUpdateFiles(
            spaced_zip, spaced_sha, spaced_app, "v0.5.1-alpha",
            [&](const UpdateDownloadProgress&) { ++spaced_progress; });
        if (!stage_spaced.ok) {
            std::fprintf(stderr, "Spaced-path / poisoned PSModulePath staging failed: %s\n",
                         stage_spaced.error_message.c_str());
        }
        OS_REQUIRE(stage_spaced.ok);
        OS_REQUIRE(stage_spaced.verified_sha256 == spaced_hash);
        OS_REQUIRE(fs::exists(stage_spaced.restart_script_path, ec));
        OS_REQUIRE(spaced_progress > 0);

        const fs::path extracted_file =
            stage_spaced.staging_dir / "extracted" / "OpenScarlett_Spaced_Test.txt";
        OS_REQUIRE(fs::exists(extracted_file, ec));
        OS_REQUIRE(ReadTextFile(extracted_file) == spaced_payload);

        const std::string spaced_script = ReadTextFile(stage_spaced.restart_script_path);
        OS_REQUIRE(spaced_script.find("OpenScarlett App (Portable)") != std::string::npos);
    }

    std::printf("[PASS] Test 3: BCrypt SHA-256 vectors, valid staging, tampered zip rejection, spaces+PSModulePath regression & offline guard\n");
}

// ============================================================================
// Test 4: Privacy Path Sanitization (SanitizeReportText)
// ============================================================================
void TestPrivacySanitization(const fs::path& bin_dir) {
    auto get_env_utf8 = [](const char* name) -> std::string {
        char buf[1024]{};
        const DWORD n = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
    };

    const std::string userprofile = get_env_utf8("USERPROFILE");
    const std::string temp_dir = get_env_utf8("TEMP");
    const std::string local_app = get_env_utf8("LOCALAPPDATA");
    const std::string appdata = get_env_utf8("APPDATA");
    const std::string username = get_env_utf8("USERNAME");

    std::string bundle_fwd = bin_dir.string();
    std::replace(bundle_fwd.begin(), bundle_fwd.end(), '\\', '/');
    std::string bundle_upper = bin_dir.string();
    std::transform(bundle_upper.begin(), bundle_upper.end(), bundle_upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    std::ostringstream dirty;
    dirty << "BundleBS=" << bin_dir.string() << "\\openscarlett.exe\n";
    dirty << "BundleFS=" << bundle_fwd << "/openscarlett.exe\n";
    dirty << "BundleUC=" << bundle_upper << "\\kernelx.dll\n";
    if (!userprofile.empty()) dirty << "Profile=" << userprofile << "\\Documents\\secret.txt\n";
    if (!temp_dir.empty()) dirty << "Temp=" << temp_dir << "\\tmp.log\n";
    if (!local_app.empty()) dirty << "LocalApp=" << local_app << "\\cache.bin\n";
    if (!appdata.empty()) dirty << "AppData=" << appdata << "\\cfg.ini\n";
    dirty << R"(GenericWin=C:\Users\AliceSmith\Downloads\OpenScarlett\log.txt)" << "\n";
    dirty << "GenericFwd=c:/users/BobJones/Desktop/run.log\n";

    const std::string clean = SanitizeReportText(dirty.str(), bin_dir);
    OS_REQUIRE(clean.find("AliceSmith") == std::string::npos);
    OS_REQUIRE(clean.find("BobJones") == std::string::npos);
    OS_REQUIRE(clean.find(bin_dir.string()) == std::string::npos);
    OS_REQUIRE(clean.find(bundle_fwd) == std::string::npos);
    OS_REQUIRE(clean.find(bundle_upper) == std::string::npos);
    if (!userprofile.empty()) OS_REQUIRE(clean.find(userprofile) == std::string::npos);
    if (!local_app.empty()) OS_REQUIRE(clean.find(local_app) == std::string::npos);
    if (!appdata.empty()) OS_REQUIRE(clean.find(appdata) == std::string::npos);
    if (!username.empty() && username.size() >= 3u) {
        // Ensure C:\Users\<USERNAME> was scrubbed
        OS_REQUIRE(clean.find("Users\\" + username) == std::string::npos);
    }

    // Synthetic source-build & multi-config paths must mask <REPO_DIR> without leaking parent path
    {
        const std::string src_dirty =
            "Exe=D:\\src\\OpenScarlett\\build\\openscarlett.exe\n"
            "Vfs=D:\\src\\OpenScarlett\\.tmp\\openscarlett_run\\vfs\\G_overlay\n";
        const std::string src_clean = SanitizeReportText(src_dirty, fs::path("D:\\src\\OpenScarlett\\build"));
        OS_REQUIRE(src_clean.find("D:\\src\\OpenScarlett") == std::string::npos);
        OS_REQUIRE(src_clean.find("<BUNDLE_DIR>\\openscarlett.exe") != std::string::npos);
        OS_REQUIRE(src_clean.find("<REPO_DIR>\\.tmp\\openscarlett_run\\vfs\\G_overlay") != std::string::npos);

        const std::string rel_clean = SanitizeReportText(
            "Vfs=D:/src/OpenScarlett/.tmp/openscarlett_run/vfs/G_overlay\n",
            fs::path("D:\\src\\OpenScarlett\\build\\Release"));
        OS_REQUIRE(rel_clean.find("OpenScarlett") == std::string::npos);
        OS_REQUIRE(rel_clean.find("<REPO_DIR>/.tmp/openscarlett_run/vfs/G_overlay") != std::string::npos);
    }

    std::printf("[PASS] Test 4: Privacy sanitization (bundle, env paths, / and \\, case-insensitive, Users\\<name>)\n");
}

// ============================================================================
// Test 5: GPU Probe, Demo Discovery, PE Inspector & GitHub URL Budget
// ============================================================================
void TestBridgeSubsystems(const fs::path& bin_dir) {
    // 5A. GPU Telemetry probe
    const auto gpu = ProbeHostGpuTelemetry(bin_dir);
    OS_REQUIRE(gpu.probed);
    OS_REQUIRE(gpu.probe_ok);
    OS_REQUIRE(!gpu.adapters.empty());
    OS_REQUIRE(!gpu.os_version.empty());

    // 5B. Bundled demos discovery
    const auto demos = DiscoverBundledDemos(bin_dir);
    OS_REQUIRE(!demos.empty());
    for (const auto& d : demos) {
        OS_REQUIRE(!d.id.empty());
        OS_REQUIRE(!d.display_name.empty());
    }

    // 5C. PE Inspector on gdk_3d_scene.exe (or openscarlett.exe fallback)
    std::error_code ec;
    fs::path pe_target = bin_dir / "gdk_3d_scene.exe";
    if (!fs::exists(pe_target, ec)) {
        pe_target = bin_dir / "openscarlett.exe";
    }
    OS_REQUIRE(fs::exists(pe_target, ec));
    const auto pe = InspectBinaryForLauncher(pe_target, bin_dir);
    OS_REQUIRE(pe.ok);
    OS_REQUIRE(!pe.is_dll);
    OS_REQUIRE(pe.section_count >= 2u);
    OS_REQUIRE(pe.total_imports > 0u);
    OS_REQUIRE(!pe.modules.empty());
    OS_REQUIRE(pe.missing_export_count == 0u);
    for (const auto& m : pe.modules) {
        OS_REQUIRE(!m.dll_name.empty());
        OS_REQUIRE(!m.routing_status.empty());
    }

    // 5D. GitHub Hardware Report URL 6200-char budget across complete / compact / clipboard modes
    HardwareTestRunResult small_run{};
    small_run.completed = true;
    small_run.passed_count = 3;
    small_run.total_count = 3;
    small_run.report_markdown = "## Short Report\nAll 3 tests passed.\n";
    small_run.rows.push_back({"test_codecs.exe", "Codec tests", true, 0, 0.12, "OK", ""});
    std::string mode_small;
    const std::string url_small = BuildGitHubHardwareReportUrl(gpu, small_run, &mode_small);
    OS_REQUIRE(!url_small.empty());
    OS_REQUIRE(url_small.size() <= 6200u);
    OS_REQUIRE(url_small.find("https://github.com/") == 0u);

    HardwareTestRunResult huge_run{};
    huge_run.completed = true;
    huge_run.passed_count = 40;
    huge_run.failed_count = 2;
    huge_run.total_count = 42;
    huge_run.report_markdown = std::string(18000u, 'A');
    huge_run.full_log_text = std::string(18000u, 'L');
    for (int i = 0; i < 42; ++i) {
        const bool failed = (i == 40 || i == 41);
        huge_run.rows.push_back({
            "test_very_long_hardware_suite_item_" + std::to_string(i) + ".exe",
            "Long description for stress-testing URL budget enforcement",
            !failed, failed ? 1 : 0, 1.25,
            failed ? "[FAIL] OS_REQUIRE_FAILED_MARKER" : std::string(160u, 'H'),
            failed ? "[FAIL] OS_REQUIRE(!StartsWithDriveCW(work_build)) line 1264\n" : std::string(300u, 'O'),
        });
    }
    std::string mode_huge;
    const std::string url_huge = BuildGitHubHardwareReportUrl(gpu, huge_run, &mode_huge);
    OS_REQUIRE(!url_huge.empty());
    OS_REQUIRE(url_huge.size() <= 6200u);
    OS_REQUIRE(mode_huge == "compact");
    // Even with a 42-test suite exceeding the full-table budget, failing test diagnostics must be embedded!
    OS_REQUIRE(url_huge.find("StartsWithDriveCW") != std::string::npos);

    std::printf("[PASS] Test 5: GPU probe (%zu adapters), demo discovery, PE inspector (%s, 0 missing), GitHub URL budget (%zu/%zu chars)\n",
                gpu.adapters.size(), pe_target.filename().string().c_str(),
                url_small.size(), url_huge.size());
}

// ============================================================================
// Test 6: OpenScarlettLauncher.exe CLI, Headless & Live GUI Smoke Tests
// ============================================================================
void TestLauncherExecutableSmoke(const fs::path& bin_dir) {
    const fs::path launcher_exe = bin_dir / "OpenScarlettLauncher.exe";
    std::error_code ec;
    OS_REQUIRE(fs::exists(launcher_exe, ec));

    // 6A. --version
    {
        const auto r = RunChildCapture(
            launcher_exe, L"--version",
            {{L"OPENSCARLETT_NO_UPDATE_CHECK", L"1"}});
        OS_REQUIRE(r.launched);
        OS_REQUIRE(r.exit_code == 0u);
        OS_REQUIRE(r.stdout_text.find(kOpenScarlettAppVersion) != std::string::npos);
    }

    // 6B. --headless-check (real GPU)
    {
        const auto r = RunChildCapture(
            launcher_exe, L"--headless-check",
            {{L"OPENSCARLETT_NO_UPDATE_CHECK", L"1"}}, 20000u);
        OS_REQUIRE(r.launched);
        OS_REQUIRE(r.exit_code == 0u);
        OS_REQUIRE(r.stdout_text.find("[PASS]") != std::string::npos);
    }

    // 6C. --smoke-gui (real GPU / hardware D2D)
    {
        const auto r = RunChildCapture(
            launcher_exe, L"--smoke-gui --smoke-ms 150",
            {{L"OPENSCARLETT_NO_UPDATE_CHECK", L"1"}}, 15000u);
        OS_REQUIRE(r.launched);
        OS_REQUIRE(r.exit_code == 0u);
        OS_REQUIRE(r.stdout_text.find("[PASS]") != std::string::npos);
    }

    // 6D. --smoke-gui under D3D12 WARP (OPENSCARLETT_PREFER_WARP=1)
    {
        const auto r = RunChildCapture(
            launcher_exe, L"--smoke-gui --smoke-ms 150",
            {{L"OPENSCARLETT_NO_UPDATE_CHECK", L"1"}, {L"OPENSCARLETT_PREFER_WARP", L"1"}},
            15000u);
        OS_REQUIRE(r.launched);
        OS_REQUIRE(r.exit_code == 0u);
        OS_REQUIRE(r.stdout_text.find("[PASS]") != std::string::npos);
    }

    // 6E. Verify docs/icon.ico is genuinely embedded as IDI_APP_ICON in OpenScarlettLauncher.exe
    {
        HMODULE hmod = ::LoadLibraryExW(
            launcher_exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
        OS_REQUIRE(hmod != nullptr);

        const LPCWSTR kRtGroupIconW =
            MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(RT_GROUP_ICON));
        HRSRC hrsrc = ::FindResourceW(
            hmod, MAKEINTRESOURCEW(IDI_APP_ICON), kRtGroupIconW);
        OS_REQUIRE(hrsrc != nullptr);

        HICON icon_32 = static_cast<HICON>(::LoadImageW(
            hmod, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        OS_REQUIRE(icon_32 != nullptr);
        ::DestroyIcon(icon_32);

        HICON icon_16 = static_cast<HICON>(::LoadImageW(
            hmod, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        OS_REQUIRE(icon_16 != nullptr);
        ::DestroyIcon(icon_16);

        ::FreeLibrary(hmod);
    }

    std::printf("[PASS] Test 6: OpenScarlettLauncher.exe --version, --headless-check, --smoke-gui (HW + WARP) & embedded IDI_APP_ICON\n");
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path bin_dir = ResolveBinaryDir(argc > 0 ? argv[0] : nullptr);
    const fs::path scratch_dir = ResolveScratchDir(bin_dir);

    TestSemVer();
    TestGitHubReleaseJsonParser();
    TestSha256AndUpdateStaging(scratch_dir);
    TestPrivacySanitization(bin_dir);
    TestBridgeSubsystems(bin_dir);
    TestLauncherExecutableSmoke(bin_dir);

    std::error_code ec;
    fs::remove_all(scratch_dir, ec);

    std::printf("=== ALL 6 LAUNCHER & UPDATER TESTS PASSED ===\n");
    return 0;
}
