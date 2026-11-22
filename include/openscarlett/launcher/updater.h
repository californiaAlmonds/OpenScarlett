#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "openscarlett/core/status.h"

namespace openscarlett::launcher {

inline constexpr const char* kOpenScarlettAppVersion = "v0.5.0-alpha";
inline constexpr const char* kOpenScarlettDefaultRepoOwner = "californiaAlmonds";
inline constexpr const char* kOpenScarlettDefaultRepoName = "OpenScarlett";

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;  // empty = stable release (sorts newer than any prerelease of same M.m.p)
    bool valid = false;
};

struct UpdateReleaseInfo {
    std::string tag_name;
    std::string release_name;
    std::string body_markdown;
    std::string html_url;
    std::string published_at;
    std::string zip_asset_name;
    std::string zip_download_url;
    std::string sha256_asset_name;
    std::string sha256_download_url;
    bool update_available = false;
};

struct UpdateCheckResult {
    bool ok = false;
    bool skipped_offline = false;
    std::string current_version;
    std::string error_message;
    UpdateReleaseInfo release;
};

struct Sha256VerifyResult {
    bool ok = false;
    std::string computed_hex;
    std::string expected_hex;
    std::string error_message;
};

struct UpdateDownloadProgress {
    std::string stage;  // e.g. "Downloading zip...", "Verifying SHA-256...", "Extracting..."
    uint64_t bytes_downloaded = 0;
    uint64_t total_bytes = 0;
    int percent = 0;  // 0..100
};

struct UpdateApplyResult {
    bool ok = false;
    std::filesystem::path staging_dir;
    std::filesystem::path restart_script_path;
    std::string verified_sha256;
    std::string error_message;
};

// SemVer parsing and comparison (e.g. "v0.3.1-alpha" < "v0.4.0-alpha" < "v0.4.0").
SemVer ParseSemVer(const std::string& version_str);
// Returns <0 if a < b, 0 if equal, >0 if a > b.
int CompareSemVer(const SemVer& a, const SemVer& b);
bool IsNewerVersion(const std::string& current_version, const std::string& candidate_version);

// Returns true when OPENSCARLETT_NO_UPDATE_CHECK is set to "1" / "true" / "yes".
bool IsUpdateCheckDisabledByEnv();

// Minimal JSON parser for GitHub /releases/latest response.
Status ParseGitHubReleaseJson(const std::string& json_text, UpdateReleaseInfo& out_info);

// Parses a "<64-hex-sha256>  <filename>" checksum file. Rejects malformed/non-64-hex text.
Status ParseSha256ChecksumFile(const std::string& checksum_text,
                               std::string& out_hex_lower,
                               std::string* out_filename = nullptr);

// Windows BCrypt SHA-256 hashing helpers (zero third-party dependencies).
Status ComputeBufferSha256Hex(const uint8_t* data, size_t size, std::string& out_hex_lower);
Status ComputeFileSha256Hex(const std::filesystem::path& file_path, std::string& out_hex_lower);

// Verifies a local file against expected checksum text (either raw 64-hex or "<hash>  <file>").
Sha256VerifyResult VerifyFileSha256(const std::filesystem::path& file_path,
                                    const std::string& expected_checksum_text);

// Opt-in HTTPS query via native WinHTTP to GitHub Releases API (NEVER installs silently).
UpdateCheckResult CheckLatestGitHubRelease(
    const std::string& current_version = kOpenScarlettAppVersion,
    bool offline_mode = false,
    const std::string& repo_owner = kOpenScarlettDefaultRepoOwner,
    const std::string& repo_name = kOpenScarlettDefaultRepoName);

// Downloads BOTH zip and .sha256 via WinHTTP HTTPS into a staging folder beside app_dir,
// verifies exact SHA-256 via Windows BCrypt BEFORE extraction (hard-fails & deletes temp
// files on mismatch), extracts staged files, and writes a transparent restart-and-swap
// helper script preserving S:\ saves & user config.
UpdateApplyResult DownloadVerifyAndStageUpdate(
    const UpdateReleaseInfo& release,
    const std::filesystem::path& app_dir,
    const std::function<void(const UpdateDownloadProgress&)>& on_progress = {},
    const std::atomic<bool>* cancel_flag = nullptr);

// Stages an already-downloaded local zip + sha256 file (also used by offline/unit tests).
UpdateApplyResult VerifyAndStageLocalUpdateFiles(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& sha256_path,
    const std::filesystem::path& app_dir,
    const std::string& tag_name,
    const std::function<void(const UpdateDownloadProgress&)>& on_progress = {});

// Launches the generated transparent restart helper script (detached) so the launcher can exit.
Status LaunchUpdateRestartHelper(const std::filesystem::path& restart_script_path,
                                 uint32_t parent_pid);

}  // namespace openscarlett::launcher
