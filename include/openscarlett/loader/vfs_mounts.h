#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace openscarlett {

enum class VfsDrive : uint8_t {
    None = 0,
    Game = 1,     // G: or GAME: -> Game package root (read-only base + CoW overlay)
    Scratch = 2,  // T: -> Title scratch directory on E:
    Save = 3,     // S: -> Persistent save directory on E:
};

enum class VfsAccessMode : uint8_t {
    ReadOnly = 0,     // Check whiteout -> overlay -> read-only base
    WriteCreate = 1,  // Never touch g_root; target g_overlay_root, clear whiteout, ensure parent dirs
    WriteModify = 2,  // Never touch g_root; CoW-copy base file into g_overlay_root if not yet overlaid
    Delete = 3,       // Never touch g_root; remove from overlay and record whiteout if present in base
};

enum class VfsTranslateStatus : uint8_t {
    NotVirtual = 0,         // Path does not target G:, GAME:, T:, or S:
    Translated = 1,         // Successfully mapped to host backing path
    RejectedTraversal = 2,  // Attempted '..' escape above mount root or invalid drive injection
    UnconfiguredMount = 3,  // Virtual drive matched, but backing root is empty
    DeletedWhiteout = 4,    // Path on G:/GAME: is hidden by a CoW whiteout marker
    ReadOnlyViolation = 5,  // Write/Delete on G:/GAME: attempted with no CoW overlay configured
};

struct VfsSubMount {
    std::wstring virtual_prefix;  // Normalized relative subpath under G:\ (e.g. L"DLC\\Pack1")
    std::wstring host_root;       // Strictly read-only backing host directory for this sub-mount
};

struct VfsMountConfig {
    std::wstring g_root;          // Backing host directory for G: / GAME: (strictly read-only)
    std::wstring t_root;          // Backing host directory for T:
    std::wstring s_root;          // Backing host directory for S:
    std::wstring g_overlay_root;  // CoW overlay & whiteout directory for G: / GAME:
    std::vector<VfsSubMount> g_sub_mounts;  // Optional nested read-only sub-mounts under G:/GAME:
    bool forbid_system_drive_writes = false;  // When true, mutating operations targeting C:\ or %SystemDrive% are rejected
    bool preserve_host_drive_folders = true;  // Preserve real host PC directories on console drive letters (D:/G:/T:/S:/P:)
    std::vector<std::wstring> preserved_host_prefixes;  // Explicit host path prefixes preserved verbatim when unshadowed
};

// UTF-8 / UTF-16 conversion helpers (safe on Windows x64).
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

// Returns true if the given path targets the Windows system drive (C:\ or %SystemDrive%).
bool IsPathOnSystemDriveW(std::wstring_view path) noexcept;

// Creates a directory tree iteratively using Win32 CreateDirectoryW (no C:\ fallback).
bool EnsureDirectoryTreeW(std::wstring_view path);

// Resolves the default OpenScarlett base scratch/work directory without leaking to C:\.
// Resolution order:
//   1. OPENSCARLETT_WORK_DIR environment variable if set and non-empty.
//   2. Derived from anchor_dir (or the running executable's directory if empty):
//      - Inside a build/bin directory -> "<parent>\.tmp\openscarlett_run" if writable.
//      - Standalone folder            -> "<anchor_dir>\.openscarlett_tmp" if writable.
//   3. Fallback to "%TEMP%\openscarlett" ONLY if the anchor location is read-only.
std::wstring ResolveDefaultWorkDirW(std::wstring_view anchor_dir = {});
std::string ResolveDefaultWorkDirUtf8(std::string_view anchor_dir = {});

// Convenience default directories derived from ResolveDefaultWorkDirW:
//   - VFS root:       "<work_dir>\vfs"
//   - Telemetry dir:  "<work_dir>\telemetry"
std::wstring ResolveDefaultVfsRootW(std::wstring_view anchor_dir = {});
std::wstring ResolveDefaultTelemetryDirW(std::wstring_view anchor_dir = {});

// Virtual drive translator for Xbox Series X|S G:\, GAME:\, T:\, and S:\ mounts,
// with Read-Only Copy-on-Write (CoW) overlay & whiteout tracking for G:\ / GAME:\.
class VfsMounts {
public:
    VfsMounts() = default;
    explicit VfsMounts(const VfsMountConfig& config);

    // Builds default mount config honoring OPENSCARLETT_VFS_G/T/S/G_OVERLAY and
    // OPENSCARLETT_SAVE_ROOT, defaulting writable dirs under ResolveDefaultVfsRootW()
    // (OPENSCARLETT_WORK_DIR -> executable-relative .tmp -> read-only fallback %TEMP%\openscarlett).
    static VfsMountConfig BuildDefaultConfig(std::wstring_view default_g_root = {},
                                             std::wstring_view default_vfs_root = {});

    void Configure(const VfsMountConfig& config);

    // Registers a read-only nested sub-mount under G:/GAME: (e.g. "DLC/Pack1" -> E:\dlc_pack1).
    // All writes inside the sub-mount still redirect to g_overlay_root without touching host_ro_root.
    bool AddGameSubMount(std::wstring_view virtual_subpath, std::wstring_view host_ro_root);

    // Registers an explicit host path prefix (e.g. "G:\HostApps" or "D:\Work") that is preserved
    // verbatim as NotVirtual whenever not shadowed by a guest file, overlay entry, or sub-mount.
    bool AddPreservedHostPrefixW(std::wstring_view host_prefix);

    // Creates T:\, S:\, and G_overlay backing directories on disk if configured (never touches g_root).
    bool EnsureWritableMounts() const;

    // Fast O(1) prefix check for G:, GAME:, T:, S: (case-insensitive, supports '\\?\').
    static VfsDrive DetectVirtualDriveW(std::wstring_view path) noexcept;
    static VfsDrive DetectVirtualDriveA(std::string_view path) noexcept;

    // Access-aware translation implementing Read-Only CoW overlay and whiteout semantics on G:/GAME:.
    VfsTranslateStatus TranslateForAccessW(std::wstring_view virtual_path,
                                           VfsAccessMode mode,
                                           std::wstring& out_host_path,
                                           bool* out_was_whiteout = nullptr,
                                           bool ensure_writable_dirs = true) const;

    // Translates a wide Win32 path for read/default access (supports G:, GAME:, T:, S:, '/' and '\', '\\?\').
    VfsTranslateStatus TranslateW(std::wstring_view virtual_path,
                                  std::wstring& out_host_path,
                                  bool ensure_writable_dirs = true) const;

    // Translates a UTF-8 path string for read/default access.
    VfsTranslateStatus TranslateUtf8(std::string_view virtual_path,
                                     std::string& out_host_path,
                                     bool ensure_writable_dirs = true) const;

    // CoW overlay & whiteout helpers for G:/GAME: (never modify g_root).
    bool IsWhiteoutedW(std::wstring_view virtual_path) const;
    bool IsVisibleGamePathW(std::wstring_view virtual_path, bool* out_is_dir = nullptr) const;
    bool DeleteVirtualFileW(std::wstring_view virtual_path, uint32_t& out_win32_error) const;
    bool RemoveVirtualDirectoryW(std::wstring_view virtual_path, uint32_t& out_win32_error) const;
    bool MoveVirtualPathW(std::wstring_view src_path,
                          std::wstring_view dst_path,
                          uint32_t flags,
                          uint32_t& out_win32_error) const;
    bool ReplaceVirtualFileW(std::wstring_view replaced_path,
                             std::wstring_view replacement_path,
                             std::wstring_view backup_path,
                             uint32_t flags,
                             uint32_t& out_win32_error) const;

    // Resolves both read-only base and overlay paths for a G:/GAME: path (used by FindFirstFileW).
    VfsTranslateStatus ResolveGamePairW(std::wstring_view virtual_path,
                                        std::wstring& out_base_path,
                                        std::wstring& out_overlay_path,
                                        std::wstring& out_rel_path,
                                        bool* out_was_whiteout = nullptr) const;

    const std::wstring& GetGameRoot() const noexcept { return g_root_; }
    const std::wstring& GetGameOverlayRoot() const noexcept { return g_overlay_root_; }
    const std::wstring& GetScratchRoot() const noexcept { return t_root_; }
    const std::wstring& GetSaveRoot() const noexcept { return s_root_; }
    const std::vector<VfsSubMount>& GetGameSubMounts() const noexcept { return g_sub_mounts_; }
    bool GetForbidSystemDriveWrites() const noexcept { return forbid_system_drive_writes_; }
    bool GetPreserveHostDriveFolders() const noexcept { return preserve_host_drive_folders_; }
    const std::vector<std::wstring>& GetPreservedHostPrefixes() const noexcept {
        return preserved_host_prefixes_;
    }

private:
    static std::wstring NormalizeRoot(std::wstring_view root);

    std::wstring g_root_;
    std::wstring t_root_;
    std::wstring s_root_;
    std::wstring g_overlay_root_;
    std::vector<VfsSubMount> g_sub_mounts_;
    bool forbid_system_drive_writes_ = false;
    bool preserve_host_drive_folders_ = true;
    std::vector<std::wstring> preserved_host_prefixes_;
};

}  // namespace openscarlett
