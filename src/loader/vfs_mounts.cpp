#include "openscarlett/loader/vfs_mounts.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

constexpr wchar_t kDefaultWorkDirFallbackW[] = L"E:\\OpenScarlett\\.tmp\\openscarlett_run";
constexpr wchar_t kWhiteoutDirNameW[] = L".whiteout";

wchar_t AsciiLowerW(wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') {
        return static_cast<wchar_t>(c - L'A' + L'a');
    }
    return c;
}

std::wstring ToLowerAsciiW(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) {
        c = AsciiLowerW(c);
    }
    return out;
}

char AsciiLowerA(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool IsExtendedPrefixW(std::wstring_view p) noexcept {
    return p.size() >= 4 &&
           (p[0] == L'\\' || p[0] == L'/') &&
           (p[1] == L'\\' || p[1] == L'/') &&
           p[2] == L'?' &&
           (p[3] == L'\\' || p[3] == L'/');
}

bool IsExtendedPrefixA(std::string_view p) noexcept {
    return p.size() >= 4 &&
           (p[0] == '\\' || p[0] == '/') &&
           (p[1] == '\\' || p[1] == '/') &&
           p[2] == '?' &&
           (p[3] == '\\' || p[3] == '/');
}

bool IsNtDosDevicesPrefixW(std::wstring_view p) noexcept {
    return p.size() >= 4 &&
           (p[0] == L'\\' || p[0] == L'/') &&
           p[1] == L'?' &&
           p[2] == L'?' &&
           (p[3] == L'\\' || p[3] == L'/');
}

bool IsNtDosDevicesPrefixA(std::string_view p) noexcept {
    return p.size() >= 4 &&
           (p[0] == '\\' || p[0] == '/') &&
           p[1] == '?' &&
           p[2] == '?' &&
           (p[3] == '\\' || p[3] == '/');
}

bool IsNtGlobalDosDevicesPrefixW(std::wstring_view p) noexcept {
    if (p.size() < 10) {
        return false;
    }
    return (p[0] == L'\\' || p[0] == L'/') &&
           AsciiLowerW(p[1]) == L'g' &&
           AsciiLowerW(p[2]) == L'l' &&
           AsciiLowerW(p[3]) == L'o' &&
           AsciiLowerW(p[4]) == L'b' &&
           AsciiLowerW(p[5]) == L'a' &&
           AsciiLowerW(p[6]) == L'l' &&
           p[7] == L'?' &&
           p[8] == L'?' &&
           (p[9] == L'\\' || p[9] == L'/');
}

bool IsNtGlobalDosDevicesPrefixA(std::string_view p) noexcept {
    if (p.size() < 10) {
        return false;
    }
    return (p[0] == '\\' || p[0] == '/') &&
           AsciiLowerA(p[1]) == 'g' &&
           AsciiLowerA(p[2]) == 'l' &&
           AsciiLowerA(p[3]) == 'o' &&
           AsciiLowerA(p[4]) == 'b' &&
           AsciiLowerA(p[5]) == 'a' &&
           AsciiLowerA(p[6]) == 'l' &&
           p[7] == '?' &&
           p[8] == '?' &&
           (p[9] == '\\' || p[9] == '/');
}

enum class PathPrefixKind : uint8_t {
    None = 0,
    Win32Extended = 1,  // Win32 extended-length prefix (\\?\)
    NtDosDevices = 2,   // NT DOS devices prefix (\??\ or \GLOBAL??\)
};

PathPrefixKind StripDevicePrefixW(std::wstring_view& p) noexcept {
    if (IsExtendedPrefixW(p)) {
        p.remove_prefix(4);
        return PathPrefixKind::Win32Extended;
    }
    if (IsNtDosDevicesPrefixW(p)) {
        p.remove_prefix(4);
        return PathPrefixKind::NtDosDevices;
    }
    if (IsNtGlobalDosDevicesPrefixW(p)) {
        p.remove_prefix(10);
        return PathPrefixKind::NtDosDevices;
    }
    return PathPrefixKind::None;
}

PathPrefixKind StripDevicePrefixA(std::string_view& p) noexcept {
    if (IsExtendedPrefixA(p)) {
        p.remove_prefix(4);
        return PathPrefixKind::Win32Extended;
    }
    if (IsNtDosDevicesPrefixA(p)) {
        p.remove_prefix(4);
        return PathPrefixKind::NtDosDevices;
    }
    if (IsNtGlobalDosDevicesPrefixA(p)) {
        p.remove_prefix(10);
        return PathPrefixKind::NtDosDevices;
    }
    return PathPrefixKind::None;
}

bool MatchGamePrefixW(std::wstring_view p) noexcept {
    return p.size() >= 5 &&
           AsciiLowerW(p[0]) == L'g' &&
           AsciiLowerW(p[1]) == L'a' &&
           AsciiLowerW(p[2]) == L'm' &&
           AsciiLowerW(p[3]) == L'e' &&
           p[4] == L':';
}

bool MatchGamePrefixA(std::string_view p) noexcept {
    return p.size() >= 5 &&
           AsciiLowerA(p[0]) == 'g' &&
           AsciiLowerA(p[1]) == 'a' &&
           AsciiLowerA(p[2]) == 'm' &&
           AsciiLowerA(p[3]) == 'e' &&
           p[4] == ':';
}

VfsDrive DriveCharToEnum(wchar_t c) noexcept {
    switch (AsciiLowerW(c)) {
        case L'g': return VfsDrive::Game;
        case L'd': return VfsDrive::Game;  // Xbox D:\ (title/disc package root) aliases Game drive
        case L't': return VfsDrive::Scratch;
        case L's': return VfsDrive::Save;
        case L'p': return VfsDrive::Save;  // Xbox P:\ (Persistent Local Storage) aliases Save drive
        default: return VfsDrive::None;
    }
}

struct ParsedVirtualPath {
    VfsDrive drive = VfsDrive::None;
    PathPrefixKind prefix_kind = PathPrefixKind::None;
    wchar_t drive_letter = 0;  // Uppercase L'D', L'G', L'T', L'S', L'P' (0 for GAME:)
    std::wstring_view stripped_raw;
    std::vector<std::wstring_view> parts;
};

VfsTranslateStatus ParseVirtualPathW(std::wstring_view virtual_path, ParsedVirtualPath& out) {
    out = {};
    std::wstring_view rest = virtual_path;
    out.prefix_kind = StripDevicePrefixW(rest);
    out.stripped_raw = rest;

    size_t prefix_len = 0;
    if (MatchGamePrefixW(rest)) {
        out.drive = VfsDrive::Game;
        out.drive_letter = 0;
        prefix_len = 5;
    } else if (rest.size() >= 2 && rest[1] == L':') {
        out.drive = DriveCharToEnum(rest[0]);
        if (out.drive != VfsDrive::None) {
            const wchar_t lo = AsciiLowerW(rest[0]);
            out.drive_letter = static_cast<wchar_t>(lo - L'a' + L'A');
        }
        prefix_len = 2;
    }

    if (out.drive == VfsDrive::None) {
        return VfsTranslateStatus::NotVirtual;
    }

    std::wstring_view sub = rest.substr(prefix_len);
    for (wchar_t ch : sub) {
        if (ch == L'\0') {
            return VfsTranslateStatus::RejectedTraversal;
        }
    }
    size_t pos = 0;
    while (pos < sub.size()) {
        while (pos < sub.size() && (sub[pos] == L'\\' || sub[pos] == L'/')) {
            ++pos;
        }
        if (pos >= sub.size()) {
            break;
        }
        size_t end = pos;
        while (end < sub.size() && sub[end] != L'\\' && sub[end] != L'/') {
            ++end;
        }
        std::wstring_view comp = sub.substr(pos, end - pos);
        pos = end;

        if (comp == L".") {
            continue;
        }
        if (comp == L"..") {
            if (out.parts.empty()) {
                return VfsTranslateStatus::RejectedTraversal;
            }
            out.parts.pop_back();
            continue;
        }
        // Reject embedded drive-letter colons inside virtual sub-paths.
        if (comp.find(L':') != std::wstring_view::npos) {
            return VfsTranslateStatus::RejectedTraversal;
        }
        out.parts.push_back(comp);
    }

    return VfsTranslateStatus::Translated;
}

bool SplitNormalizedSubMountPrefixW(std::wstring_view raw_subpath,
                                    std::vector<std::wstring>& out_parts,
                                    std::wstring& out_canonical) {
    out_parts.clear();
    out_canonical.clear();
    std::wstring_view rest = raw_subpath;
    while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t')) {
        rest.remove_prefix(1);
    }
    while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'\t')) {
        rest.remove_suffix(1);
    }
    (void)StripDevicePrefixW(rest);
    if (MatchGamePrefixW(rest)) {
        rest.remove_prefix(5);
    } else if (rest.size() >= 2 && rest[1] == L':' && DriveCharToEnum(rest[0]) == VfsDrive::Game) {
        rest.remove_prefix(2);
    }

    size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && (rest[pos] == L'\\' || rest[pos] == L'/')) {
            ++pos;
        }
        if (pos >= rest.size()) {
            break;
        }
        size_t end = pos;
        while (end < rest.size() && rest[end] != L'\\' && rest[end] != L'/') {
            ++end;
        }
        std::wstring_view comp = rest.substr(pos, end - pos);
        pos = end;
        if (comp == L".") {
            continue;
        }
        if (comp == L"..") {
            if (out_parts.empty()) {
                return false;
            }
            out_parts.pop_back();
            continue;
        }
        if (comp.find(L':') != std::wstring_view::npos || comp.find(L'\0') != std::wstring_view::npos) {
            return false;
        }
        out_parts.emplace_back(comp);
    }
    if (out_parts.empty()) {
        return false;
    }
    for (size_t i = 0; i < out_parts.size(); ++i) {
        if (i > 0) {
            out_canonical.push_back(L'\\');
        }
        out_canonical.append(out_parts[i]);
    }
    return true;
}

bool EqualsIgnoreCaseAsciiW(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (AsciiLowerW(a[i]) != AsciiLowerW(b[i])) {
            return false;
        }
    }
    return true;
}

std::vector<std::wstring_view> SplitCanonicalPartsView(std::wstring_view canonical) {
    std::vector<std::wstring_view> out;
    size_t pos = 0;
    while (pos < canonical.size()) {
        size_t sep = canonical.find(L'\\', pos);
        if (sep == std::wstring_view::npos) {
            sep = canonical.size();
        }
        if (sep > pos) {
            out.push_back(canonical.substr(pos, sep - pos));
        }
        pos = sep + 1;
    }
    return out;
}

std::wstring ReadEnvVarW(const wchar_t* name) {
#if defined(_WIN32)
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring out(needed, L'\0');
    DWORD written = ::GetEnvironmentVariableW(name, out.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    out.resize(written);
    return out;
#else
    (void)name;
    return {};
#endif
}

bool HasAbsoluteDrivePrefixW(std::wstring_view p) noexcept {
    return p.size() >= 3 &&
           ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
           p[1] == L':' &&
           (p[2] == L'\\' || p[2] == L'/');
}

std::wstring JoinRootAndParts(const std::wstring& root,
                              const std::vector<std::wstring_view>& parts,
                              PathPrefixKind prefix_kind) {
    std::wstring out;
    if (prefix_kind == PathPrefixKind::Win32Extended &&
        !IsExtendedPrefixW(root) && HasAbsoluteDrivePrefixW(root)) {
        out.append(L"\\\\?\\");
    } else if (prefix_kind == PathPrefixKind::NtDosDevices &&
               !IsNtDosDevicesPrefixW(root) && HasAbsoluteDrivePrefixW(root)) {
        out.append(L"\\??\\");
    }
    out.append(root);
    for (const auto& part : parts) {
        if (!out.empty() && out.back() != L'\\') {
            out.push_back(L'\\');
        }
        out.append(part);
    }
    return out;
}

std::wstring JoinRootAndParts(const std::wstring& root,
                              const std::vector<std::wstring_view>& parts,
                              bool add_extended_prefix) {
    return JoinRootAndParts(
        root,
        parts,
        add_extended_prefix ? PathPrefixKind::Win32Extended : PathPrefixKind::None);
}

std::wstring JoinParts(const std::vector<std::wstring_view>& parts, size_t count) {
    std::wstring out;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out.push_back(L'\\');
        }
        out.append(parts[i]);
    }
    return out;
}

#if defined(_WIN32)

bool HostPathExistsW(const std::wstring& path, bool* out_is_dir = nullptr) noexcept {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (out_is_dir) {
        *out_is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return true;
}

std::wstring BuildWhiteoutMarkerPath(const std::wstring& overlay_root,
                                     const std::vector<std::wstring_view>& parts,
                                     size_t prefix_count) {
    std::wstring p = overlay_root;
    if (!p.empty() && p.back() != L'\\') {
        p.push_back(L'\\');
    }
    p.append(kWhiteoutDirNameW);
    for (size_t i = 0; i < prefix_count; ++i) {
        p.push_back(L'\\');
        p.append(parts[i]);
    }
    p.append(L".wo");
    return p;
}

bool HasAnyWhiteoutInOverlay(const std::wstring& overlay_root,
                             const std::vector<std::wstring_view>& parts) {
    if (overlay_root.empty() || parts.empty()) {
        return false;
    }
    for (size_t depth = 1; depth <= parts.size(); ++depth) {
        const std::wstring marker = BuildWhiteoutMarkerPath(overlay_root, parts, depth);
        if (HostPathExistsW(marker)) {
            return true;
        }
    }
    return false;
}

bool WriteWhiteoutMarker(const std::wstring& overlay_root,
                         const std::vector<std::wstring_view>& parts) {
    if (overlay_root.empty() || parts.empty()) {
        return false;
    }
    const std::wstring marker = BuildWhiteoutMarkerPath(overlay_root, parts, parts.size());
    const std::wstring parent = std::filesystem::path(marker).parent_path().wstring();
    if (!parent.empty() && !EnsureDirectoryTreeW(parent)) {
        return false;
    }
    HANDLE h = ::CreateFileW(marker.c_str(),
                             GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::CloseHandle(h);
    return true;
}

void ClearWhiteoutMarkers(const std::wstring& overlay_root,
                          const std::vector<std::wstring_view>& parts) {
    if (overlay_root.empty() || parts.empty()) {
        return;
    }
    for (size_t depth = 1; depth <= parts.size(); ++depth) {
        const std::wstring marker = BuildWhiteoutMarkerPath(overlay_root, parts, depth);
        if (HostPathExistsW(marker)) {
            ::SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_NORMAL);
            ::DeleteFileW(marker.c_str());
        }
    }
}

void ClearReadOnlyAttrW(const std::wstring& path) {
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
        ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
}

bool CopyBaseFileToOverlay(const std::wstring& base_path, const std::wstring& overlay_path) {
    const std::wstring parent = std::filesystem::path(overlay_path).parent_path().wstring();
    if (!parent.empty() && !EnsureDirectoryTreeW(parent)) {
        return false;
    }
    if (!::CopyFileW(base_path.c_str(), overlay_path.c_str(), FALSE)) {
        return false;
    }
    ClearReadOnlyAttrW(overlay_path);
    return true;
}

bool IsOverlayDirVisiblyEmpty(const std::wstring& base_dir,
                              const std::wstring& overlay_root,
                              const std::wstring& overlay_dir,
                              const std::vector<std::wstring_view>& dir_parts) {
    auto has_visible_in_dir = [&](const std::wstring& host_dir, bool is_overlay_side) -> bool {
        if (!HostPathExistsW(host_dir)) {
            return false;
        }
        std::wstring pattern = host_dir;
        if (!pattern.empty() && pattern.back() != L'\\') {
            pattern.push_back(L'\\');
        }
        pattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool found_visible = false;
        do {
            const std::wstring_view name(fd.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            if (is_overlay_side && dir_parts.empty() && name == kWhiteoutDirNameW) {
                continue;
            }
            std::vector<std::wstring_view> child_parts = dir_parts;
            child_parts.push_back(name);
            if (!HasAnyWhiteoutInOverlay(overlay_root, child_parts)) {
                found_visible = true;
                break;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return found_visible;
    };

    if (has_visible_in_dir(overlay_dir, true)) {
        return false;
    }
    if (has_visible_in_dir(base_dir, false)) {
        return false;
    }
    return true;
}

std::wstring GetRunningExeDirW() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path().wstring();
}

bool IsDirectoryWritableW(const std::wstring& candidate_dir) {
    if (candidate_dir.empty()) {
        return false;
    }
    if (!EnsureDirectoryTreeW(candidate_dir)) {
        return false;
    }
    const DWORD pid = ::GetCurrentProcessId();
    const DWORD tid = ::GetCurrentThreadId();
    std::wstring probe = candidate_dir;
    if (probe.back() != L'\\') {
        probe.push_back(L'\\');
    }
    probe.append(L".os_write_probe_");
    probe.append(std::to_wstring(pid));
    probe.push_back(L'_');
    probe.append(std::to_wstring(tid));
    probe.append(L".tmp");

    HANDLE h = ::CreateFileW(probe.c_str(),
                             GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::CloseHandle(h);
    return true;
}

std::wstring GetFinalCanonicalPathLowerW(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    HANDLE h = ::CreateFileW(path.c_str(),
                             FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = ::GetFinalPathNameByHandleW(
        h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    if (len >= buf.size()) {
        buf.resize(len + 1, L'\0');
        len = ::GetFinalPathNameByHandleW(
            h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    }
    ::CloseHandle(h);
    if (len == 0 || len >= buf.size()) {
        return {};
    }
    buf.resize(len);
    if (buf.size() >= 8 && buf.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        buf = L"\\\\" + buf.substr(8);
    } else if (IsExtendedPrefixW(buf)) {
        buf.erase(0, 4);
    }
    while (buf.size() > 3 && buf.back() == L'\\') {
        buf.pop_back();
    }
    return ToLowerAsciiW(buf);
}

bool IsSubPathEqualOrUnderLowerW(const std::wstring& candidate_lo, const std::wstring& root_lo) noexcept {
    if (candidate_lo.empty() || root_lo.empty()) {
        return false;
    }
    if (candidate_lo == root_lo) {
        return true;
    }
    if (candidate_lo.size() > root_lo.size() &&
        candidate_lo.compare(0, root_lo.size(), root_lo) == 0) {
        return root_lo.back() == L'\\' || candidate_lo[root_lo.size()] == L'\\';
    }
    return false;
}

VfsTranslateStatus VerifyHostPathContainmentW(const std::wstring& root,
                                              const std::vector<std::wstring_view>& rel_parts,
                                              const std::vector<std::wstring>* extra_ro_roots,
                                              bool is_mutating,
                                              bool forbid_system_drive_writes) {
    if (root.empty()) {
        return VfsTranslateStatus::UnconfiguredMount;
    }
    if (is_mutating && forbid_system_drive_writes && IsPathOnSystemDriveW(root)) {
        return VfsTranslateStatus::ReadOnlyViolation;
    }

    // Check root itself if it exists and is a reparse point.
    const DWORD root_attrs = ::GetFileAttributesW(root.c_str());
    if (root_attrs == INVALID_FILE_ATTRIBUTES) {
        // Root directory not created yet on disk; lexical containment already verified by parser.
        return VfsTranslateStatus::Translated;
    }
    if (is_mutating && forbid_system_drive_writes && (root_attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const std::wstring final_root_check = GetFinalCanonicalPathLowerW(root);
        if (final_root_check.empty() || IsPathOnSystemDriveW(final_root_check)) {
            return VfsTranslateStatus::ReadOnlyViolation;
        }
    }

    std::wstring final_root_lo;
    std::vector<std::wstring> extra_final_lo;
    bool loaded_roots = false;

    auto ensure_roots_loaded = [&]() {
        if (loaded_roots) {
            return;
        }
        loaded_roots = true;
        final_root_lo = GetFinalCanonicalPathLowerW(root);
        if (!is_mutating && extra_ro_roots != nullptr) {
            for (const auto& r : *extra_ro_roots) {
                std::wstring fr = GetFinalCanonicalPathLowerW(r);
                if (!fr.empty()) {
                    extra_final_lo.push_back(std::move(fr));
                }
            }
        }
    };

    std::wstring cur = root;
    for (const auto& part : rel_parts) {
        if (!cur.empty() && cur.back() != L'\\') {
            cur.push_back(L'\\');
        }
        cur.append(part);

        const DWORD attrs = ::GetFileAttributesW(cur.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            // Remaining components do not exist yet on disk.
            break;
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ensure_roots_loaded();
            const std::wstring final_cur_lo = GetFinalCanonicalPathLowerW(cur);
            if (final_cur_lo.empty() || final_root_lo.empty()) {
                return VfsTranslateStatus::RejectedTraversal;
            }
            if (is_mutating && forbid_system_drive_writes && IsPathOnSystemDriveW(final_cur_lo)) {
                return VfsTranslateStatus::RejectedTraversal;
            }
            bool contained = IsSubPathEqualOrUnderLowerW(final_cur_lo, final_root_lo);
            if (!contained && !is_mutating) {
                for (const auto& extra_lo : extra_final_lo) {
                    if (IsSubPathEqualOrUnderLowerW(final_cur_lo, extra_lo)) {
                        contained = true;
                        break;
                    }
                }
            }
            if (!contained) {
                return VfsTranslateStatus::RejectedTraversal;
            }
        }
    }

    return VfsTranslateStatus::Translated;
}

#endif  // _WIN32

std::wstring NormalizePathRootW(std::wstring_view root) {
    if (root.empty()) {
        return {};
    }
    std::wstring out(root);
    for (wchar_t& c : out) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (IsNtGlobalDosDevicesPrefixW(out)) {
        out.erase(0, 10);
    } else if (IsNtDosDevicesPrefixW(out)) {
        out.erase(0, 4);
    }
    const bool had_ext = IsExtendedPrefixW(out);
    if (had_ext) {
        std::wstring inner = std::filesystem::path(out.substr(4)).lexically_normal().wstring();
        out = L"\\\\?\\" + inner;
    } else if (out.find(L"\\..") != std::wstring::npos || out.find(L"\\.") != std::wstring::npos) {
        out = std::filesystem::path(out).lexically_normal().wstring();
    }
    const size_t min_len = had_ext ? 7u : 3u;
    while (out.size() > min_len && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

bool MatchesHostRootPrefixW(std::wstring_view stripped_raw, const std::wstring& host_root) {
    if (stripped_raw.empty() || host_root.empty()) {
        return false;
    }
    std::wstring norm_in(stripped_raw);
    for (wchar_t& c : norm_in) {
        if (c == L'/') {
            c = L'\\';
        } else {
            c = AsciiLowerW(c);
        }
    }
    std::wstring_view hr(host_root);
    (void)StripDevicePrefixW(hr);
    std::wstring norm_hr = ToLowerAsciiW(hr);
    while (norm_hr.size() > 3 && norm_hr.back() == L'\\') {
        norm_hr.pop_back();
    }
    if (norm_in.size() < norm_hr.size()) {
        return false;
    }
    if (norm_in.compare(0, norm_hr.size(), norm_hr) != 0) {
        return false;
    }
    return norm_in.size() == norm_hr.size() || norm_in[norm_hr.size()] == L'\\';
}

bool IsRealHostDrivePathW(const ParsedVirtualPath& parsed,
                          const std::wstring& g_root,
                          const std::wstring& g_overlay_root,
                          const std::wstring& t_root,
                          const std::wstring& s_root,
                          const std::vector<VfsSubMount>& sub_mounts,
                          bool preserve_host_drive_folders,
                          const std::vector<std::wstring>& preserved_host_prefixes) {
    // GAME:\ is always strictly virtual; never treated as a real host volume.
    if (parsed.drive_letter == 0) {
        return false;
    }
    // Always avoid recursive double-redirection if the path already targets a configured
    // backing host directory (e.g. a title installed on D:\ or G:\).
    if (MatchesHostRootPrefixW(parsed.stripped_raw, g_root) ||
        MatchesHostRootPrefixW(parsed.stripped_raw, t_root) ||
        MatchesHostRootPrefixW(parsed.stripped_raw, s_root) ||
        MatchesHostRootPrefixW(parsed.stripped_raw, g_overlay_root)) {
        return true;
    }
    for (const auto& sub : sub_mounts) {
        if (MatchesHostRootPrefixW(parsed.stripped_raw, sub.host_root)) {
            return true;
        }
    }
    if (!preserve_host_drive_folders || parsed.parts.empty()) {
        return false;
    }

    // If the virtual relative path belongs to the mounted guest tree (configured sub-mount,
    // existing guest file/top-level folder, overlay entry, or whiteout marker), keep it virtual.
    if (parsed.drive == VfsDrive::Game) {
        for (const auto& sub : sub_mounts) {
            const auto prefix_parts = SplitCanonicalPartsView(sub.virtual_prefix);
            if (!prefix_parts.empty() && parsed.parts.size() >= prefix_parts.size()) {
                bool match = true;
                for (size_t i = 0; i < prefix_parts.size(); ++i) {
                    if (!EqualsIgnoreCaseAsciiW(parsed.parts[i], prefix_parts[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return false;
                }
            }
        }
#if defined(_WIN32)
        if (!g_root.empty()) {
            const std::wstring base_plain = JoinRootAndParts(g_root, parsed.parts, PathPrefixKind::None);
            if (HostPathExistsW(base_plain)) {
                return false;
            }
            std::vector<std::wstring_view> top_part{parsed.parts[0]};
            const std::wstring top_base = JoinRootAndParts(g_root, top_part, PathPrefixKind::None);
            if (HostPathExistsW(top_base)) {
                return false;
            }
        }
        if (!g_overlay_root.empty()) {
            if (HasAnyWhiteoutInOverlay(g_overlay_root, parsed.parts)) {
                return false;
            }
            const std::wstring ovl_plain = JoinRootAndParts(g_overlay_root, parsed.parts, PathPrefixKind::None);
            if (HostPathExistsW(ovl_plain)) {
                return false;
            }
            std::vector<std::wstring_view> top_part{parsed.parts[0]};
            const std::wstring top_ovl = JoinRootAndParts(g_overlay_root, top_part, PathPrefixKind::None);
            if (HostPathExistsW(top_ovl)) {
                return false;
            }
        }
#endif
    } else {
#if defined(_WIN32)
        const std::wstring& root = (parsed.drive == VfsDrive::Scratch) ? t_root : s_root;
        if (!root.empty()) {
            const std::wstring target_plain = JoinRootAndParts(root, parsed.parts, PathPrefixKind::None);
            if (HostPathExistsW(target_plain)) {
                return false;
            }
            std::vector<std::wstring_view> top_part{parsed.parts[0]};
            const std::wstring top_target = JoinRootAndParts(root, top_part, PathPrefixKind::None);
            if (HostPathExistsW(top_target)) {
                return false;
            }
        }
#endif
    }

    // Explicit host-preserved prefixes (e.g. "G:\SteamLibrary", "D:\HostTools").
    for (const auto& prefix : preserved_host_prefixes) {
        if (MatchesHostRootPrefixW(parsed.stripped_raw, prefix)) {
            return true;
        }
    }

#if defined(_WIN32)
    // Preserve real host PC directories on mounted console drive letters (D:\, G:\, T:\, S:\, P:\)
    // whenever <drive>:\<top-component> exists on the real Windows host volume and does not exist
    // under the guest mount root (xcl 61c6be8).
    std::wstring host_top;
    host_top.push_back(parsed.drive_letter);
    host_top.append(L":\\");
    host_top.append(parsed.parts[0]);
    if (HostPathExistsW(host_top)) {
        return true;
    }
#endif
    return false;
}

void ResolveGameBaseTarget(const std::wstring& g_root,
                           const std::vector<VfsSubMount>& sub_mounts,
                           const std::vector<std::wstring_view>& parts,
                           std::wstring& out_matched_root,
                           std::vector<std::wstring_view>& out_rel_parts) {
    for (const auto& sub : sub_mounts) {
        const auto prefix_parts = SplitCanonicalPartsView(sub.virtual_prefix);
        if (prefix_parts.empty() || parts.size() < prefix_parts.size()) {
            continue;
        }
        bool match = true;
        for (size_t i = 0; i < prefix_parts.size(); ++i) {
            if (!EqualsIgnoreCaseAsciiW(parts[i], prefix_parts[i])) {
                match = false;
                break;
            }
        }
        if (match) {
            out_matched_root = sub.host_root;
            out_rel_parts.assign(parts.begin() + static_cast<std::ptrdiff_t>(prefix_parts.size()),
                                 parts.end());
            return;
        }
    }
    out_matched_root = g_root;
    out_rel_parts = parts;
}

std::vector<std::wstring> CollectAllowedGameReadRoots(const std::wstring& g_root,
                                                      const std::vector<VfsSubMount>& sub_mounts) {
    std::vector<std::wstring> roots;
    roots.reserve(1 + sub_mounts.size());
    if (!g_root.empty()) {
        roots.push_back(g_root);
    }
    for (const auto& sub : sub_mounts) {
        if (!sub.host_root.empty()) {
            roots.push_back(sub.host_root);
        }
    }
    return roots;
}

}  // namespace

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
#if defined(_WIN32)
    int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
#if defined(_WIN32)
    int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

bool EnsureDirectoryTreeW(std::wstring_view path) {
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    std::wstring normalized(path);
    for (wchar_t& c : normalized) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }

    size_t start = 0;
    if (IsExtendedPrefixW(normalized)) {
        start = 4;
    }
    if (normalized.size() >= start + 3 && normalized[start + 1] == L':' && normalized[start + 2] == L'\\') {
        start += 3;
    } else if (normalized.size() >= 2 && normalized[0] == L'\\' && normalized[1] == L'\\') {
        size_t pos = normalized.find(L'\\', 2);
        if (pos == std::wstring::npos) {
            return false;
        }
        pos = normalized.find(L'\\', pos + 1);
        start = (pos == std::wstring::npos) ? normalized.size() : (pos + 1);
    }

    for (size_t i = start; i <= normalized.size(); ++i) {
        if (i == normalized.size() || normalized[i] == L'\\') {
            if (i == start) {
                continue;
            }
            std::wstring prefix = normalized.substr(0, i);
            if (!::CreateDirectoryW(prefix.c_str(), nullptr)) {
                DWORD err = ::GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    DWORD attrs = ::GetFileAttributesW(prefix.c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        return false;
                    }
                }
            }
        }
    }
    DWORD attrs = ::GetFileAttributesW(normalized.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    return false;
#endif
}

bool IsPathOnSystemDriveW(std::wstring_view path) noexcept {
    (void)StripDevicePrefixW(path);
    if (path.size() < 2 || path[1] != L':') {
        return false;
    }
    wchar_t sys_drive = L'c';
    std::wstring env_sys = ReadEnvVarW(L"SystemDrive");
    if (!env_sys.empty()) {
        sys_drive = AsciiLowerW(env_sys[0]);
    }
    const wchar_t drv = AsciiLowerW(path[0]);
    return drv == L'c' || drv == sys_drive;
}

std::wstring ResolveDefaultWorkDirW(std::wstring_view anchor_dir) {
    // 1. Explicit OPENSCARLETT_WORK_DIR override takes highest precedence.
    std::wstring env_work = ReadEnvVarW(L"OPENSCARLETT_WORK_DIR");
    if (!env_work.empty()) {
        return NormalizePathRootW(env_work);
    }

#if defined(_WIN32)
    // 2. If %TEMP% was explicitly redirected off the C:\ system drive (e.g. build.cmd /
    //    build_wt.cmd setting TEMP=E:\OpenScarlett\.tmp\...), honor that isolated scratch root
    //    while still strictly preventing any C:\ leakage.
    std::wstring env_temp = ReadEnvVarW(L"TEMP");
    if (env_temp.empty()) {
        env_temp = ReadEnvVarW(L"TMP");
    }
    if (!env_temp.empty() && !IsPathOnSystemDriveW(env_temp)) {
        std::wstring custom_temp = NormalizePathRootW(env_temp);
        if (!custom_temp.empty() && IsDirectoryWritableW(custom_temp)) {
            return custom_temp;
        }
    }

    // 3. Derive from anchor_dir (or running executable's directory) if writable.
    std::wstring base_dir = anchor_dir.empty() ? GetRunningExeDirW() : NormalizePathRootW(anchor_dir);
    if (!base_dir.empty()) {
        std::filesystem::path p(base_dir);
        std::wstring leaf_lo = ToLowerAsciiW(p.filename().wstring());

        // Handle multi-config build outputs such as "...\build\Release" or "...\bin\Debug".
        if (leaf_lo == L"debug" || leaf_lo == L"release" ||
            leaf_lo == L"relwithdebinfo" || leaf_lo == L"minsizerel") {
            const std::filesystem::path parent = p.parent_path();
            const std::wstring parent_lo = ToLowerAsciiW(parent.filename().wstring());
            if (parent_lo == L"build" || parent_lo == L"bin") {
                p = parent;
                leaf_lo = parent_lo;
            }
        }

        std::wstring candidate;
        if ((leaf_lo == L"build" || leaf_lo == L"bin") &&
            p.has_parent_path() && p.parent_path() != p.root_path()) {
            candidate = NormalizePathRootW((p.parent_path() / L".tmp" / L"openscarlett_run").wstring());
        } else {
            candidate = NormalizePathRootW((p / L".openscarlett_tmp").wstring());
        }

        if (!candidate.empty() && IsDirectoryWritableW(candidate)) {
            return candidate;
        }
    }

    // 4. Only fall back to %TEMP%\openscarlett when the executable/anchor directory is read-only.
    if (!env_temp.empty()) {
        return NormalizePathRootW(env_temp + L"\\openscarlett");
    }
    return std::wstring(kDefaultWorkDirFallbackW);
#else
    (void)anchor_dir;
    return std::wstring(kDefaultWorkDirFallbackW);
#endif
}

std::string ResolveDefaultWorkDirUtf8(std::string_view anchor_dir) {
    return WideToUtf8(ResolveDefaultWorkDirW(Utf8ToWide(anchor_dir)));
}

std::wstring ResolveDefaultVfsRootW(std::wstring_view anchor_dir) {
    return NormalizePathRootW(ResolveDefaultWorkDirW(anchor_dir) + L"\\vfs");
}

std::wstring ResolveDefaultTelemetryDirW(std::wstring_view anchor_dir) {
    return NormalizePathRootW(ResolveDefaultWorkDirW(anchor_dir) + L"\\telemetry");
}

VfsMounts::VfsMounts(const VfsMountConfig& config) {
    Configure(config);
}

VfsMountConfig VfsMounts::BuildDefaultConfig(std::wstring_view default_g_root,
                                             std::wstring_view default_vfs_root) {
    VfsMountConfig cfg{};

    std::wstring base_vfs;
    if (!default_vfs_root.empty()) {
        base_vfs = NormalizeRoot(default_vfs_root);
    } else {
        base_vfs = ResolveDefaultVfsRootW();
    }

    const bool base_is_non_system = !base_vfs.empty() && !IsPathOnSystemDriveW(base_vfs);
    std::wstring env_forbid_sys = ReadEnvVarW(L"OPENSCARLETT_FORBID_SYSTEM_DRIVE_WRITES");
    const bool forbid_sys_writes =
        base_is_non_system &&
        (!env_forbid_sys.empty() && env_forbid_sys != L"0" && env_forbid_sys != L"false");
    cfg.forbid_system_drive_writes = forbid_sys_writes;

    auto accept_writable_env = [&](const std::wstring& val) -> bool {
        if (val.empty()) {
            return false;
        }
        if (forbid_sys_writes && IsPathOnSystemDriveW(val)) {
            return false;
        }
        return true;
    };

    std::wstring env_g = ReadEnvVarW(L"OPENSCARLETT_VFS_G");
    if (!env_g.empty()) {
        cfg.g_root = NormalizeRoot(env_g);
    } else if (!default_g_root.empty()) {
        cfg.g_root = NormalizeRoot(default_g_root);
    }

    std::wstring env_t = ReadEnvVarW(L"OPENSCARLETT_VFS_T");
    if (accept_writable_env(env_t)) {
        cfg.t_root = NormalizeRoot(env_t);
    } else {
        cfg.t_root = NormalizeRoot(base_vfs + L"\\T");
    }

    // Route S:\ strictly to OPENSCARLETT_VFS_S or OPENSCARLETT_SAVE_ROOT (never C:\ when base is off C:\).
    std::wstring env_s = ReadEnvVarW(L"OPENSCARLETT_VFS_S");
    if (accept_writable_env(env_s)) {
        cfg.s_root = NormalizeRoot(env_s);
    } else {
        std::wstring env_save_root = ReadEnvVarW(L"OPENSCARLETT_SAVE_ROOT");
        if (accept_writable_env(env_save_root)) {
            cfg.s_root = NormalizeRoot(env_save_root);
        } else {
            cfg.s_root = NormalizeRoot(base_vfs + L"\\S");
        }
    }

    std::wstring env_g_overlay = ReadEnvVarW(L"OPENSCARLETT_VFS_G_OVERLAY");
    if (accept_writable_env(env_g_overlay)) {
        cfg.g_overlay_root = NormalizeRoot(env_g_overlay);
    } else {
        cfg.g_overlay_root = NormalizeRoot(base_vfs + L"\\G_overlay");
    }

    // Optional semicolon-separated nested G:\ / D:\ read-only sub-mounts:
    //   OPENSCARLETT_VFS_G_SUBMOUNTS="DLC\Pack1=E:\dlc1;Content/Paks=E:\paks"
    // Also accepts OPENSCARLETT_VFS_SUBMOUNTS as an alias.
    auto parse_submounts_env = [&](const wchar_t* env_name) {
        std::wstring env_submounts = ReadEnvVarW(env_name);
        if (env_submounts.empty()) {
            return;
        }
        size_t pos = 0;
        while (pos < env_submounts.size()) {
            size_t sep = env_submounts.find(L';', pos);
            if (sep == std::wstring::npos) {
                sep = env_submounts.size();
            }
            std::wstring_view item(env_submounts.data() + pos, sep - pos);
            pos = sep + 1;
            const size_t eq = item.find(L'=');
            if (eq == std::wstring_view::npos || eq == 0 || eq + 1 >= item.size()) {
                continue;
            }
            std::vector<std::wstring> parts;
            std::wstring canon_prefix;
            if (!SplitNormalizedSubMountPrefixW(item.substr(0, eq), parts, canon_prefix)) {
                continue;
            }
            std::wstring host_root = NormalizeRoot(item.substr(eq + 1));
            if (!host_root.empty()) {
                cfg.g_sub_mounts.push_back(VfsSubMount{std::move(canon_prefix), std::move(host_root)});
            }
        }
    };
    parse_submounts_env(L"OPENSCARLETT_VFS_G_SUBMOUNTS");
    parse_submounts_env(L"OPENSCARLETT_VFS_SUBMOUNTS");

    std::wstring env_preserve = ReadEnvVarW(L"OPENSCARLETT_VFS_PRESERVE_HOST_FOLDERS");
    if (!env_preserve.empty()) {
        const std::wstring lo = ToLowerAsciiW(env_preserve);
        cfg.preserve_host_drive_folders = !(lo == L"0" || lo == L"false" || lo == L"no" || lo == L"off");
    }
    std::wstring env_preserved_paths = ReadEnvVarW(L"OPENSCARLETT_VFS_PRESERVE_HOST_PATHS");
    if (!env_preserved_paths.empty()) {
        size_t pos = 0;
        while (pos < env_preserved_paths.size()) {
            size_t sep = env_preserved_paths.find(L';', pos);
            if (sep == std::wstring::npos) {
                sep = env_preserved_paths.size();
            }
            std::wstring_view item(env_preserved_paths.data() + pos, sep - pos);
            pos = sep + 1;
            std::wstring norm = NormalizeRoot(item);
            if (!norm.empty()) {
                cfg.preserved_host_prefixes.push_back(std::move(norm));
            }
        }
    }

    return cfg;
}

std::wstring VfsMounts::NormalizeRoot(std::wstring_view root) {
    return NormalizePathRootW(root);
}

void VfsMounts::Configure(const VfsMountConfig& config) {
    g_root_ = NormalizeRoot(config.g_root);
    t_root_ = NormalizeRoot(config.t_root);
    s_root_ = NormalizeRoot(config.s_root);
    g_overlay_root_ = NormalizeRoot(config.g_overlay_root);
    forbid_system_drive_writes_ = config.forbid_system_drive_writes;
    preserve_host_drive_folders_ = config.preserve_host_drive_folders;
    preserved_host_prefixes_.clear();
    for (const auto& p : config.preserved_host_prefixes) {
        (void)AddPreservedHostPrefixW(p);
    }
    g_sub_mounts_.clear();
    for (const auto& sub : config.g_sub_mounts) {
        (void)AddGameSubMount(sub.virtual_prefix, sub.host_root);
    }
}

bool VfsMounts::AddPreservedHostPrefixW(std::wstring_view host_prefix) {
    std::wstring norm = NormalizeRoot(host_prefix);
    if (norm.empty()) {
        return false;
    }
    for (const auto& existing : preserved_host_prefixes_) {
        if (EqualsIgnoreCaseAsciiW(existing, norm)) {
            return true;
        }
    }
    preserved_host_prefixes_.push_back(std::move(norm));
    return true;
}

bool VfsMounts::AddGameSubMount(std::wstring_view virtual_subpath, std::wstring_view host_ro_root) {
    std::vector<std::wstring> parts;
    std::wstring canon_prefix;
    if (!SplitNormalizedSubMountPrefixW(virtual_subpath, parts, canon_prefix)) {
        return false;
    }
    std::wstring norm_host = NormalizeRoot(host_ro_root);
    if (norm_host.empty()) {
        return false;
    }
    for (auto& existing : g_sub_mounts_) {
        if (EqualsIgnoreCaseAsciiW(existing.virtual_prefix, canon_prefix)) {
            existing.host_root = std::move(norm_host);
            return true;
        }
    }
    g_sub_mounts_.push_back(VfsSubMount{std::move(canon_prefix), std::move(norm_host)});
    // Sort longest component count first so nested sub-mounts (e.g. DLC\Pack1\HD) beat shallower ones (DLC\Pack1).
    std::stable_sort(g_sub_mounts_.begin(), g_sub_mounts_.end(),
                     [](const VfsSubMount& a, const VfsSubMount& b) {
                         const size_t ca = SplitCanonicalPartsView(a.virtual_prefix).size();
                         const size_t cb = SplitCanonicalPartsView(b.virtual_prefix).size();
                         if (ca != cb) {
                             return ca > cb;
                         }
                         return a.virtual_prefix.size() > b.virtual_prefix.size();
                     });
    return true;
}

bool VfsMounts::EnsureWritableMounts() const {
    bool ok = true;
    auto ensure_safe = [&](const std::wstring& dir) {
        if (dir.empty()) {
            return;
        }
        if (forbid_system_drive_writes_ && IsPathOnSystemDriveW(dir)) {
            ok = false;
            return;
        }
        ok = EnsureDirectoryTreeW(dir) && ok;
    };
    ensure_safe(t_root_);
    ensure_safe(s_root_);
    ensure_safe(g_overlay_root_);
    return ok;
}

VfsDrive VfsMounts::DetectVirtualDriveW(std::wstring_view path) noexcept {
    (void)StripDevicePrefixW(path);
    if (MatchGamePrefixW(path)) {
        return VfsDrive::Game;
    }
    if (path.size() < 2 || path[1] != L':') {
        return VfsDrive::None;
    }
    return DriveCharToEnum(path[0]);
}

VfsDrive VfsMounts::DetectVirtualDriveA(std::string_view path) noexcept {
    (void)StripDevicePrefixA(path);
    if (MatchGamePrefixA(path)) {
        return VfsDrive::Game;
    }
    if (path.size() < 2 || path[1] != ':') {
        return VfsDrive::None;
    }
    switch (AsciiLowerA(path[0])) {
        case 'g': return VfsDrive::Game;
        case 'd': return VfsDrive::Game;
        case 't': return VfsDrive::Scratch;
        case 's': return VfsDrive::Save;
        case 'p': return VfsDrive::Save;
        default: return VfsDrive::None;
    }
}

VfsTranslateStatus VfsMounts::TranslateForAccessW(std::wstring_view virtual_path,
                                                  VfsAccessMode mode,
                                                  std::wstring& out_host_path,
                                                  bool* out_was_whiteout,
                                                  bool ensure_writable_dirs) const {
    if (out_was_whiteout) {
        *out_was_whiteout = false;
    }

    ParsedVirtualPath parsed;
    const VfsTranslateStatus parse_st = ParseVirtualPathW(virtual_path, parsed);
    if (parse_st != VfsTranslateStatus::Translated) {
        return parse_st;
    }

    // Preserve real host PC directories on mounted console drive letters (D:/G:/T:/S:/P:)
    // whenever not shadowed by guest files, overlay entries, or configured sub-mounts.
    if (IsRealHostDrivePathW(parsed, g_root_, g_overlay_root_, t_root_, s_root_, g_sub_mounts_,
                             preserve_host_drive_folders_, preserved_host_prefixes_)) {
        return VfsTranslateStatus::NotVirtual;
    }

    // T:\ and S:\ are directly writable mounts (no CoW overlay needed).
    if (parsed.drive == VfsDrive::Scratch || parsed.drive == VfsDrive::Save) {
        const std::wstring& root = (parsed.drive == VfsDrive::Scratch) ? t_root_ : s_root_;
        if (root.empty()) {
            return VfsTranslateStatus::UnconfiguredMount;
        }
        const bool is_mutating = (mode != VfsAccessMode::ReadOnly);
        if (forbid_system_drive_writes_ && IsPathOnSystemDriveW(root)) {
            if (is_mutating || ensure_writable_dirs) {
                out_host_path.clear();
                return VfsTranslateStatus::ReadOnlyViolation;
            }
        }
#if defined(_WIN32)
        const VfsTranslateStatus contain_st = VerifyHostPathContainmentW(
            root, parsed.parts, nullptr, is_mutating, forbid_system_drive_writes_);
        if (contain_st != VfsTranslateStatus::Translated) {
            out_host_path.clear();
            return contain_st;
        }
#endif
        if (ensure_writable_dirs) {
            EnsureDirectoryTreeW(root);
        }
        out_host_path = JoinRootAndParts(root, parsed.parts, parsed.prefix_kind);
        return VfsTranslateStatus::Translated;
    }

    // G:\ / GAME:\ / D:\ -> Read-Only Copy-on-Write (CoW) overlay semantics with nested sub-mounts!
    if (g_root_.empty() && g_sub_mounts_.empty()) {
        return VfsTranslateStatus::UnconfiguredMount;
    }

    std::wstring matched_base_root;
    std::vector<std::wstring_view> base_rel_parts;
    ResolveGameBaseTarget(g_root_, g_sub_mounts_, parsed.parts, matched_base_root, base_rel_parts);
    if (matched_base_root.empty()) {
        return VfsTranslateStatus::UnconfiguredMount;
    }

    const std::wstring base_plain = JoinRootAndParts(matched_base_root, base_rel_parts, PathPrefixKind::None);
    const std::wstring base_final = JoinRootAndParts(matched_base_root, base_rel_parts, parsed.prefix_kind);

#if !defined(_WIN32)
    (void)mode;
    out_host_path = base_final;
    return VfsTranslateStatus::Translated;
#else
    const std::vector<std::wstring> allowed_ro_roots = CollectAllowedGameReadRoots(g_root_, g_sub_mounts_);
    const VfsTranslateStatus base_contain = VerifyHostPathContainmentW(
        matched_base_root, base_rel_parts, &allowed_ro_roots, false, false);
    if (base_contain != VfsTranslateStatus::Translated) {
        out_host_path.clear();
        return base_contain;
    }

    if (mode == VfsAccessMode::ReadOnly) {
        if (!g_overlay_root_.empty() && HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
            if (out_was_whiteout) {
                *out_was_whiteout = true;
            }
            out_host_path.clear();
            return VfsTranslateStatus::DeletedWhiteout;
        }
        if (!g_overlay_root_.empty() && !parsed.parts.empty()) {
            const VfsTranslateStatus ovl_contain = VerifyHostPathContainmentW(
                g_overlay_root_, parsed.parts, nullptr, false, false);
            if (ovl_contain != VfsTranslateStatus::Translated) {
                out_host_path.clear();
                return ovl_contain;
            }
            const std::wstring overlay_plain = JoinRootAndParts(g_overlay_root_, parsed.parts, PathPrefixKind::None);
            if (HostPathExistsW(overlay_plain)) {
                out_host_path = JoinRootAndParts(g_overlay_root_, parsed.parts, parsed.prefix_kind);
                return VfsTranslateStatus::Translated;
            }
        }
        out_host_path = base_final;
        return VfsTranslateStatus::Translated;
    }

    // All mutating modes (WriteCreate, WriteModify, Delete) MUST NEVER return a path inside g_root_
    // or any read-only sub-mount, and MUST NEVER write to C:\ when forbid_system_drive_writes_ is set!
    if (g_overlay_root_.empty()) {
        out_host_path.clear();
        return VfsTranslateStatus::ReadOnlyViolation;
    }

    const VfsTranslateStatus ovl_mut_contain = VerifyHostPathContainmentW(
        g_overlay_root_, parsed.parts, nullptr, true, forbid_system_drive_writes_);
    if (ovl_mut_contain != VfsTranslateStatus::Translated) {
        out_host_path.clear();
        return ovl_mut_contain;
    }

    const std::wstring overlay_plain = JoinRootAndParts(g_overlay_root_, parsed.parts, PathPrefixKind::None);
    const std::wstring overlay_final = JoinRootAndParts(g_overlay_root_, parsed.parts, parsed.prefix_kind);

    if (mode == VfsAccessMode::WriteCreate) {
        if (ensure_writable_dirs) {
            EnsureDirectoryTreeW(g_overlay_root_);
            if (parsed.parts.size() > 1) {
                const std::wstring parent = std::filesystem::path(overlay_plain).parent_path().wstring();
                if (!parent.empty()) {
                    EnsureDirectoryTreeW(parent);
                }
            }
        }
        ClearWhiteoutMarkers(g_overlay_root_, parsed.parts);
        out_host_path = overlay_final;
        return VfsTranslateStatus::Translated;
    }

    if (mode == VfsAccessMode::WriteModify) {
        if (HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
            if (out_was_whiteout) {
                *out_was_whiteout = true;
            }
            out_host_path.clear();
            return VfsTranslateStatus::DeletedWhiteout;
        }
        if (ensure_writable_dirs) {
            EnsureDirectoryTreeW(g_overlay_root_);
            if (parsed.parts.size() > 1) {
                const std::wstring parent = std::filesystem::path(overlay_plain).parent_path().wstring();
                if (!parent.empty()) {
                    EnsureDirectoryTreeW(parent);
                }
            }
        }
        if (!parsed.parts.empty() && !HostPathExistsW(overlay_plain)) {
            bool base_is_dir = false;
            if (HostPathExistsW(base_plain, &base_is_dir)) {
                if (base_is_dir) {
                    EnsureDirectoryTreeW(overlay_plain);
                } else {
                    CopyBaseFileToOverlay(base_plain, overlay_plain);
                }
            }
        }
        out_host_path = overlay_final;
        return VfsTranslateStatus::Translated;
    }

    // mode == VfsAccessMode::Delete
    if (parsed.parts.empty()) {
        return VfsTranslateStatus::RejectedTraversal;
    }
    if (HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
        if (out_was_whiteout) {
            *out_was_whiteout = true;
        }
        out_host_path.clear();
        return VfsTranslateStatus::DeletedWhiteout;
    }

    bool overlay_is_dir = false;
    if (HostPathExistsW(overlay_plain, &overlay_is_dir)) {
        if (overlay_is_dir) {
            ::RemoveDirectoryW(overlay_plain.c_str());
        } else {
            ClearReadOnlyAttrW(overlay_plain);
            ::DeleteFileW(overlay_plain.c_str());
        }
    }
    if (HostPathExistsW(base_plain)) {
        EnsureDirectoryTreeW(g_overlay_root_);
        WriteWhiteoutMarker(g_overlay_root_, parsed.parts);
        if (out_was_whiteout) {
            *out_was_whiteout = true;
        }
    }
    out_host_path = overlay_final;
    return VfsTranslateStatus::Translated;
#endif
}

VfsTranslateStatus VfsMounts::TranslateW(std::wstring_view virtual_path,
                                         std::wstring& out_host_path,
                                         bool ensure_writable_dirs) const {
    return TranslateForAccessW(virtual_path,
                               VfsAccessMode::ReadOnly,
                               out_host_path,
                               nullptr,
                               ensure_writable_dirs);
}

VfsTranslateStatus VfsMounts::TranslateUtf8(std::string_view virtual_path,
                                            std::string& out_host_path,
                                            bool ensure_writable_dirs) const {
    if (DetectVirtualDriveA(virtual_path) == VfsDrive::None) {
        return VfsTranslateStatus::NotVirtual;
    }
    std::wstring wide_in = Utf8ToWide(virtual_path);
    std::wstring wide_out;
    VfsTranslateStatus st = TranslateW(wide_in, wide_out, ensure_writable_dirs);
    if (st == VfsTranslateStatus::Translated) {
        out_host_path = WideToUtf8(wide_out);
    }
    return st;
}

bool VfsMounts::IsWhiteoutedW(std::wstring_view virtual_path) const {
#if !defined(_WIN32)
    (void)virtual_path;
    return false;
#else
    if (g_overlay_root_.empty()) {
        return false;
    }
    ParsedVirtualPath parsed;
    if (ParseVirtualPathW(virtual_path, parsed) != VfsTranslateStatus::Translated ||
        parsed.drive != VfsDrive::Game ||
        IsRealHostDrivePathW(parsed, g_root_, g_overlay_root_, t_root_, s_root_, g_sub_mounts_,
                             preserve_host_drive_folders_, preserved_host_prefixes_)) {
        return false;
    }
    return HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts);
#endif
}

bool VfsMounts::IsVisibleGamePathW(std::wstring_view virtual_path, bool* out_is_dir) const {
#if !defined(_WIN32)
    (void)virtual_path;
    (void)out_is_dir;
    return false;
#else
    ParsedVirtualPath parsed;
    if (ParseVirtualPathW(virtual_path, parsed) != VfsTranslateStatus::Translated ||
        parsed.drive != VfsDrive::Game || (g_root_.empty() && g_sub_mounts_.empty()) ||
        IsRealHostDrivePathW(parsed, g_root_, g_overlay_root_, t_root_, s_root_, g_sub_mounts_,
                             preserve_host_drive_folders_, preserved_host_prefixes_)) {
        return false;
    }
    if (!g_overlay_root_.empty() && HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
        return false;
    }
    if (!g_overlay_root_.empty() && !parsed.parts.empty()) {
        if (VerifyHostPathContainmentW(g_overlay_root_, parsed.parts, nullptr, false, false) !=
            VfsTranslateStatus::Translated) {
            return false;
        }
        const std::wstring overlay_plain = JoinRootAndParts(g_overlay_root_, parsed.parts, false);
        if (HostPathExistsW(overlay_plain, out_is_dir)) {
            return true;
        }
    }
    std::wstring matched_base_root;
    std::vector<std::wstring_view> base_rel_parts;
    ResolveGameBaseTarget(g_root_, g_sub_mounts_, parsed.parts, matched_base_root, base_rel_parts);
    if (matched_base_root.empty()) {
        return false;
    }
    const std::vector<std::wstring> allowed_ro_roots = CollectAllowedGameReadRoots(g_root_, g_sub_mounts_);
    if (VerifyHostPathContainmentW(matched_base_root, base_rel_parts, &allowed_ro_roots, false, false) !=
        VfsTranslateStatus::Translated) {
        return false;
    }
    const std::wstring base_plain = JoinRootAndParts(matched_base_root, base_rel_parts, false);
    return HostPathExistsW(base_plain, out_is_dir);
#endif
}

bool VfsMounts::DeleteVirtualFileW(std::wstring_view virtual_path, uint32_t& out_win32_error) const {
#if !defined(_WIN32)
    (void)virtual_path;
    out_win32_error = 1;
    return false;
#else
    out_win32_error = ERROR_SUCCESS;
    ParsedVirtualPath parsed;
    const VfsTranslateStatus st = ParseVirtualPathW(virtual_path, parsed);
    if (st == VfsTranslateStatus::RejectedTraversal) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (st != VfsTranslateStatus::Translated) {
        out_win32_error = ERROR_PATH_NOT_FOUND;
        return false;
    }

    if (parsed.drive != VfsDrive::Game) {
        const std::wstring& root = (parsed.drive == VfsDrive::Scratch) ? t_root_ : s_root_;
        if (root.empty()) {
            out_win32_error = ERROR_PATH_NOT_FOUND;
            return false;
        }
        if (VerifyHostPathContainmentW(root, parsed.parts, nullptr, true, forbid_system_drive_writes_) !=
            VfsTranslateStatus::Translated) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
        const std::wstring host = JoinRootAndParts(
            root,
            parsed.parts,
            parsed.prefix_kind == PathPrefixKind::Win32Extended ? PathPrefixKind::Win32Extended
                                                                : PathPrefixKind::None);
        if (!::DeleteFileW(host.c_str())) {
            out_win32_error = ::GetLastError();
            return false;
        }
        return true;
    }

    if ((g_root_.empty() && g_sub_mounts_.empty()) || g_overlay_root_.empty() || parsed.parts.empty()) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (VerifyHostPathContainmentW(g_overlay_root_, parsed.parts, nullptr, true, forbid_system_drive_writes_) !=
        VfsTranslateStatus::Translated) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }

    std::wstring matched_base_root;
    std::vector<std::wstring_view> base_rel_parts;
    ResolveGameBaseTarget(g_root_, g_sub_mounts_, parsed.parts, matched_base_root, base_rel_parts);
    const std::vector<std::wstring> allowed_ro_roots = CollectAllowedGameReadRoots(g_root_, g_sub_mounts_);
    if (!matched_base_root.empty() &&
        VerifyHostPathContainmentW(matched_base_root, base_rel_parts, &allowed_ro_roots, false, false) !=
            VfsTranslateStatus::Translated) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }

    const std::wstring base_plain =
        matched_base_root.empty() ? std::wstring{} : JoinRootAndParts(matched_base_root, base_rel_parts, false);
    const std::wstring overlay_plain = JoinRootAndParts(g_overlay_root_, parsed.parts, false);

    bool overlay_is_dir = false;
    const bool in_overlay = HostPathExistsW(overlay_plain, &overlay_is_dir);
    bool base_is_dir = false;
    const bool in_base = !base_plain.empty() && HostPathExistsW(base_plain, &base_is_dir);

    if (!in_overlay && !in_base) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }
    if ((in_overlay && overlay_is_dir) || (!in_overlay && in_base && base_is_dir)) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }

    if (in_overlay) {
        ClearReadOnlyAttrW(overlay_plain);
        if (!::DeleteFileW(overlay_plain.c_str())) {
            out_win32_error = ::GetLastError();
            return false;
        }
    }
    if (in_base) {
        EnsureDirectoryTreeW(g_overlay_root_);
        if (!WriteWhiteoutMarker(g_overlay_root_, parsed.parts)) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
    }
    return true;
#endif
}

bool VfsMounts::RemoveVirtualDirectoryW(std::wstring_view virtual_path,
                                        uint32_t& out_win32_error) const {
#if !defined(_WIN32)
    (void)virtual_path;
    out_win32_error = 1;
    return false;
#else
    out_win32_error = ERROR_SUCCESS;
    ParsedVirtualPath parsed;
    const VfsTranslateStatus st = ParseVirtualPathW(virtual_path, parsed);
    if (st == VfsTranslateStatus::RejectedTraversal) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (st != VfsTranslateStatus::Translated) {
        out_win32_error = ERROR_PATH_NOT_FOUND;
        return false;
    }

    if (parsed.drive != VfsDrive::Game) {
        const std::wstring& root = (parsed.drive == VfsDrive::Scratch) ? t_root_ : s_root_;
        if (root.empty()) {
            out_win32_error = ERROR_PATH_NOT_FOUND;
            return false;
        }
        if (VerifyHostPathContainmentW(root, parsed.parts, nullptr, true, forbid_system_drive_writes_) !=
            VfsTranslateStatus::Translated) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
        const std::wstring host = JoinRootAndParts(
            root,
            parsed.parts,
            parsed.prefix_kind == PathPrefixKind::Win32Extended ? PathPrefixKind::Win32Extended
                                                                : PathPrefixKind::None);
        if (!::RemoveDirectoryW(host.c_str())) {
            out_win32_error = ::GetLastError();
            return false;
        }
        return true;
    }

    if ((g_root_.empty() && g_sub_mounts_.empty()) || g_overlay_root_.empty() || parsed.parts.empty()) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (VerifyHostPathContainmentW(g_overlay_root_, parsed.parts, nullptr, true, forbid_system_drive_writes_) !=
        VfsTranslateStatus::Translated) {
        out_win32_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }

    std::wstring matched_base_root;
    std::vector<std::wstring_view> base_rel_parts;
    ResolveGameBaseTarget(g_root_, g_sub_mounts_, parsed.parts, matched_base_root, base_rel_parts);
    const std::wstring base_plain =
        matched_base_root.empty() ? std::wstring{} : JoinRootAndParts(matched_base_root, base_rel_parts, false);
    const std::wstring overlay_plain = JoinRootAndParts(g_overlay_root_, parsed.parts, false);

    bool overlay_is_dir = false;
    const bool in_overlay = HostPathExistsW(overlay_plain, &overlay_is_dir);
    bool base_is_dir = false;
    const bool in_base = !base_plain.empty() && HostPathExistsW(base_plain, &base_is_dir);

    if (!in_overlay && !in_base) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }
    if ((in_overlay && !overlay_is_dir) || (!in_overlay && in_base && !base_is_dir)) {
        out_win32_error = ERROR_DIRECTORY;
        return false;
    }

    if (!IsOverlayDirVisiblyEmpty(base_plain, g_overlay_root_, overlay_plain, parsed.parts)) {
        out_win32_error = ERROR_DIR_NOT_EMPTY;
        return false;
    }

    if (in_overlay) {
        ::RemoveDirectoryW(overlay_plain.c_str());
    }
    if (in_base) {
        EnsureDirectoryTreeW(g_overlay_root_);
        if (!WriteWhiteoutMarker(g_overlay_root_, parsed.parts)) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
    }
    return true;
#endif
}

bool VfsMounts::MoveVirtualPathW(std::wstring_view src_path,
                                 std::wstring_view dst_path,
                                 uint32_t flags,
                                 uint32_t& out_win32_error) const {
#if !defined(_WIN32)
    (void)src_path;
    (void)dst_path;
    (void)flags;
    out_win32_error = 1;
    return false;
#else
    out_win32_error = ERROR_SUCCESS;
    VfsDrive src_drive = DetectVirtualDriveW(src_path);
    VfsDrive dst_drive = DetectVirtualDriveW(dst_path);

    // Resolve source host path for reading.
    std::wstring src_host;
    if (src_drive != VfsDrive::None) {
        bool was_wo = false;
        auto st = TranslateForAccessW(src_path, VfsAccessMode::ReadOnly, src_host, &was_wo, true);
        if (st == VfsTranslateStatus::NotVirtual) {
            src_drive = VfsDrive::None;
            src_host.assign(src_path);
        } else if (st == VfsTranslateStatus::RejectedTraversal || st == VfsTranslateStatus::ReadOnlyViolation) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        } else if (st == VfsTranslateStatus::DeletedWhiteout) {
            out_win32_error = ERROR_FILE_NOT_FOUND;
            return false;
        } else if (st != VfsTranslateStatus::Translated) {
            out_win32_error = ERROR_PATH_NOT_FOUND;
            return false;
        }
    } else {
        src_host.assign(src_path);
    }

    bool src_is_dir = false;
    if (!HostPathExistsW(src_host, &src_is_dir)) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }

    // Check if dst_path is a preserved real host path before existence/write checks.
    if (dst_drive != VfsDrive::None) {
        std::wstring dst_probe;
        if (TranslateForAccessW(dst_path, VfsAccessMode::ReadOnly, dst_probe, nullptr, false) ==
            VfsTranslateStatus::NotVirtual) {
            dst_drive = VfsDrive::None;
        }
    }

    // Check destination existence when MOVEFILE_REPLACE_EXISTING is not set.
    const bool replace_existing = (flags & MOVEFILE_REPLACE_EXISTING) != 0;
    if (!replace_existing) {
        if (dst_drive == VfsDrive::Game) {
            if (IsVisibleGamePathW(dst_path)) {
                out_win32_error = ERROR_ALREADY_EXISTS;
                return false;
            }
        } else {
            std::wstring check_dst;
            if (dst_drive != VfsDrive::None) {
                if (TranslateW(dst_path, check_dst, false) == VfsTranslateStatus::Translated &&
                    HostPathExistsW(check_dst)) {
                    out_win32_error = ERROR_ALREADY_EXISTS;
                    return false;
                }
            } else if (HostPathExistsW(std::wstring(dst_path))) {
                out_win32_error = ERROR_ALREADY_EXISTS;
                return false;
            }
        }
    }

    // Resolve destination host path for writing (NEVER inside g_root_ or any sub-mount!).
    std::wstring dst_host;
    if (dst_drive != VfsDrive::None) {
        auto st = TranslateForAccessW(dst_path, VfsAccessMode::WriteCreate, dst_host, nullptr, true);
        if (st == VfsTranslateStatus::NotVirtual) {
            dst_drive = VfsDrive::None;
            dst_host.assign(dst_path);
        } else if (st == VfsTranslateStatus::RejectedTraversal ||
                   st == VfsTranslateStatus::ReadOnlyViolation) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        } else if (st != VfsTranslateStatus::Translated) {
            out_win32_error = ERROR_PATH_NOT_FOUND;
            return false;
        }
    } else {
        dst_host.assign(dst_path);
    }

    // If neither side is on G:, just perform normal host MoveFileExW.
    if (src_drive != VfsDrive::Game && dst_drive != VfsDrive::Game) {
        if (!::MoveFileExW(src_host.c_str(), dst_host.c_str(), static_cast<DWORD>(flags))) {
            out_win32_error = ::GetLastError();
            return false;
        }
        return true;
    }

    // Check if source currently lives in read-only g_root_ / sub-mount (must copy, never move out!).
    bool src_is_in_base_only = false;
    ParsedVirtualPath src_parsed;
    std::wstring src_base_plain;
    if (src_drive == VfsDrive::Game &&
        ParseVirtualPathW(src_path, src_parsed) == VfsTranslateStatus::Translated) {
        if (g_overlay_root_.empty()) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
        const std::wstring src_overlay = JoinRootAndParts(g_overlay_root_, src_parsed.parts, false);
        src_is_in_base_only = !HostPathExistsW(src_overlay);
        std::wstring matched_base_root;
        std::vector<std::wstring_view> base_rel_parts;
        ResolveGameBaseTarget(g_root_, g_sub_mounts_, src_parsed.parts, matched_base_root, base_rel_parts);
        if (!matched_base_root.empty()) {
            src_base_plain = JoinRootAndParts(matched_base_root, base_rel_parts, false);
        }
    }

    if (src_is_dir) {
        std::error_code ec;
        std::filesystem::create_directories(dst_host, ec);
        std::filesystem::copy(src_host,
                              dst_host,
                              std::filesystem::copy_options::recursive |
                                  (replace_existing ? std::filesystem::copy_options::overwrite_existing
                                                    : std::filesystem::copy_options::skip_existing),
                              ec);
        if (ec) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
        if (src_drive == VfsDrive::Game) {
            if (!src_is_in_base_only) {
                std::filesystem::remove_all(src_host, ec);
            }
            if (!src_base_plain.empty() && HostPathExistsW(src_base_plain)) {
                EnsureDirectoryTreeW(g_overlay_root_);
                WriteWhiteoutMarker(g_overlay_root_, src_parsed.parts);
            }
        } else {
            std::filesystem::remove_all(src_host, ec);
        }
        return true;
    }

    // Regular file move/copy.
    if (src_is_in_base_only) {
        if (!::CopyFileW(src_host.c_str(), dst_host.c_str(), replace_existing ? FALSE : TRUE)) {
            out_win32_error = ::GetLastError();
            return false;
        }
        ClearReadOnlyAttrW(dst_host);
        EnsureDirectoryTreeW(g_overlay_root_);
        if (!WriteWhiteoutMarker(g_overlay_root_, src_parsed.parts)) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
        return true;
    }

    DWORD move_flags = static_cast<DWORD>(flags) | MOVEFILE_COPY_ALLOWED;
    if (!::MoveFileExW(src_host.c_str(), dst_host.c_str(), move_flags)) {
        out_win32_error = ::GetLastError();
        return false;
    }
    if (src_drive == VfsDrive::Game) {
        if (!src_base_plain.empty() && HostPathExistsW(src_base_plain)) {
            EnsureDirectoryTreeW(g_overlay_root_);
            WriteWhiteoutMarker(g_overlay_root_, src_parsed.parts);
        }
    }
    return true;
#endif
}

bool VfsMounts::ReplaceVirtualFileW(std::wstring_view replaced_path,
                                    std::wstring_view replacement_path,
                                    std::wstring_view backup_path,
                                    uint32_t flags,
                                    uint32_t& out_win32_error) const {
#if !defined(_WIN32)
    (void)replaced_path;
    (void)replacement_path;
    (void)backup_path;
    (void)flags;
    out_win32_error = 1;
    return false;
#else
    out_win32_error = ERROR_SUCCESS;
    if (replaced_path.empty() || replacement_path.empty()) {
        out_win32_error = ERROR_INVALID_PARAMETER;
        return false;
    }

    VfsDrive replaced_drive = DetectVirtualDriveW(replaced_path);
    VfsDrive replacement_drive = DetectVirtualDriveW(replacement_path);
    VfsDrive backup_drive = backup_path.empty() ? VfsDrive::None : DetectVirtualDriveW(backup_path);

    // 1. Resolve target (replaced) file: must already exist; on G:\ CoW-copy into overlay first.
    std::wstring replaced_host;
    if (replaced_drive != VfsDrive::None) {
        std::wstring probe;
        auto chk = TranslateForAccessW(replaced_path, VfsAccessMode::ReadOnly, probe, nullptr, false);
        if (chk == VfsTranslateStatus::NotVirtual) {
            replaced_drive = VfsDrive::None;
            replaced_host.assign(replaced_path);
        } else if (chk == VfsTranslateStatus::RejectedTraversal ||
                   chk == VfsTranslateStatus::ReadOnlyViolation) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        } else if (chk == VfsTranslateStatus::DeletedWhiteout) {
            out_win32_error = ERROR_FILE_NOT_FOUND;
            return false;
        } else if (chk != VfsTranslateStatus::Translated) {
            out_win32_error = ERROR_PATH_NOT_FOUND;
            return false;
        } else {
            if (replaced_drive == VfsDrive::Game && !IsVisibleGamePathW(replaced_path)) {
                out_win32_error = ERROR_FILE_NOT_FOUND;
                return false;
            }
            auto wst = TranslateForAccessW(
                replaced_path, VfsAccessMode::WriteModify, replaced_host, nullptr, true);
            if (wst == VfsTranslateStatus::RejectedTraversal ||
                wst == VfsTranslateStatus::ReadOnlyViolation) {
                out_win32_error = ERROR_ACCESS_DENIED;
                return false;
            }
            if (wst == VfsTranslateStatus::DeletedWhiteout) {
                out_win32_error = ERROR_FILE_NOT_FOUND;
                return false;
            }
            if (wst != VfsTranslateStatus::Translated) {
                out_win32_error = ERROR_PATH_NOT_FOUND;
                return false;
            }
        }
    } else {
        replaced_host.assign(replaced_path);
    }

    bool replaced_is_dir = false;
    if (!HostPathExistsW(replaced_host, &replaced_is_dir)) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }
    if (replaced_is_dir) {
        out_win32_error = ERROR_UNABLE_TO_REMOVE_REPLACED;
        return false;
    }

    // 2. Resolve replacement file (read source; never delete/rename a file inside read-only g_root_!).
    std::wstring replacement_host;
    if (replacement_drive != VfsDrive::None) {
        auto st = TranslateForAccessW(
            replacement_path, VfsAccessMode::ReadOnly, replacement_host, nullptr, true);
        if (st == VfsTranslateStatus::NotVirtual) {
            replacement_drive = VfsDrive::None;
            replacement_host.assign(replacement_path);
        } else if (st == VfsTranslateStatus::RejectedTraversal ||
                   st == VfsTranslateStatus::ReadOnlyViolation) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        } else if (st == VfsTranslateStatus::DeletedWhiteout) {
            out_win32_error = ERROR_FILE_NOT_FOUND;
            return false;
        } else if (st != VfsTranslateStatus::Translated) {
            out_win32_error = ERROR_PATH_NOT_FOUND;
            return false;
        }
    } else {
        replacement_host.assign(replacement_path);
    }

    bool replacement_is_dir = false;
    if (!HostPathExistsW(replacement_host, &replacement_is_dir)) {
        out_win32_error = ERROR_FILE_NOT_FOUND;
        return false;
    }
    if (replacement_is_dir) {
        out_win32_error = ERROR_UNABLE_TO_MOVE_REPLACEMENT;
        return false;
    }

    // If replacement lives on G:, check whether it resides in read-only g_root_ / sub-mount.
    bool replacement_in_base_only = false;
    ParsedVirtualPath repl_parsed;
    std::wstring repl_base_plain;
    std::wstring staged_replacement;
    if (replacement_drive == VfsDrive::Game &&
        ParseVirtualPathW(replacement_path, repl_parsed) == VfsTranslateStatus::Translated) {
        if (g_overlay_root_.empty()) {
            out_win32_error = ERROR_ACCESS_DENIED;
            return false;
        }
        const std::wstring repl_overlay = JoinRootAndParts(g_overlay_root_, repl_parsed.parts, false);
        replacement_in_base_only = !HostPathExistsW(repl_overlay);
        std::wstring matched_base_root;
        std::vector<std::wstring_view> base_rel_parts;
        ResolveGameBaseTarget(g_root_, g_sub_mounts_, repl_parsed.parts, matched_base_root, base_rel_parts);
        if (!matched_base_root.empty()) {
            repl_base_plain = JoinRootAndParts(matched_base_root, base_rel_parts, false);
        }
        if (replacement_in_base_only) {
            EnsureDirectoryTreeW(g_overlay_root_);
            const std::wstring stage_parent = std::filesystem::path(replaced_host).parent_path().wstring();
            const std::wstring stage_dir = stage_parent.empty() ? g_overlay_root_ : stage_parent;
            EnsureDirectoryTreeW(stage_dir);
            staged_replacement = stage_dir + L"\\.os_replace_stage_" +
                                 std::to_wstring(::GetCurrentProcessId()) + L"_" +
                                 std::to_wstring(::GetTickCount64()) + L".tmp";
            if (!::CopyFileW(replacement_host.c_str(), staged_replacement.c_str(), FALSE)) {
                out_win32_error = ::GetLastError();
                return false;
            }
            ClearReadOnlyAttrW(staged_replacement);
        }
    }

    // 3. Resolve optional backup file path.
    std::wstring backup_host;
    if (!backup_path.empty()) {
        if (backup_drive != VfsDrive::None) {
            auto st = TranslateForAccessW(
                backup_path, VfsAccessMode::WriteCreate, backup_host, nullptr, true);
            if (st == VfsTranslateStatus::NotVirtual) {
                backup_drive = VfsDrive::None;
                backup_host.assign(backup_path);
            } else if (st == VfsTranslateStatus::RejectedTraversal ||
                       st == VfsTranslateStatus::ReadOnlyViolation) {
                if (!staged_replacement.empty()) {
                    ::DeleteFileW(staged_replacement.c_str());
                }
                out_win32_error = ERROR_ACCESS_DENIED;
                return false;
            } else if (st != VfsTranslateStatus::Translated) {
                if (!staged_replacement.empty()) {
                    ::DeleteFileW(staged_replacement.c_str());
                }
                out_win32_error = ERROR_PATH_NOT_FOUND;
                return false;
            }
        } else {
            backup_host.assign(backup_path);
        }
    }

    const std::wstring& effective_replacement =
        staged_replacement.empty() ? replacement_host : staged_replacement;
    ClearReadOnlyAttrW(replaced_host);
    const BOOL ok = ::ReplaceFileW(
        replaced_host.c_str(),
        effective_replacement.c_str(),
        backup_host.empty() ? nullptr : backup_host.c_str(),
        static_cast<DWORD>(flags),
        nullptr,
        nullptr);
    if (!ok) {
        out_win32_error = ::GetLastError();
        if (!staged_replacement.empty()) {
            ::DeleteFileW(staged_replacement.c_str());
        }
        return false;
    }

    if (replacement_drive == VfsDrive::Game && !repl_base_plain.empty() && HostPathExistsW(repl_base_plain)) {
        EnsureDirectoryTreeW(g_overlay_root_);
        (void)WriteWhiteoutMarker(g_overlay_root_, repl_parsed.parts);
    }
    return true;
#endif
}

VfsTranslateStatus VfsMounts::ResolveGamePairW(std::wstring_view virtual_path,
                                               std::wstring& out_base_path,
                                               std::wstring& out_overlay_path,
                                               std::wstring& out_rel_path,
                                               bool* out_was_whiteout) const {
    if (out_was_whiteout) {
        *out_was_whiteout = false;
    }
    ParsedVirtualPath parsed;
    const VfsTranslateStatus st = ParseVirtualPathW(virtual_path, parsed);
    if (st != VfsTranslateStatus::Translated) {
        return st;
    }
    if (parsed.drive != VfsDrive::Game ||
        IsRealHostDrivePathW(parsed, g_root_, g_overlay_root_, t_root_, s_root_, g_sub_mounts_,
                             preserve_host_drive_folders_, preserved_host_prefixes_)) {
        return VfsTranslateStatus::NotVirtual;
    }
    if (g_root_.empty() && g_sub_mounts_.empty()) {
        return VfsTranslateStatus::UnconfiguredMount;
    }

    std::wstring matched_base_root;
    std::vector<std::wstring_view> base_rel_parts;
    ResolveGameBaseTarget(g_root_, g_sub_mounts_, parsed.parts, matched_base_root, base_rel_parts);
    if (matched_base_root.empty()) {
        return VfsTranslateStatus::UnconfiguredMount;
    }

#if defined(_WIN32)
    const std::vector<std::wstring> allowed_ro_roots = CollectAllowedGameReadRoots(g_root_, g_sub_mounts_);
    const VfsTranslateStatus base_contain = VerifyHostPathContainmentW(
        matched_base_root, base_rel_parts, &allowed_ro_roots, false, false);
    if (base_contain != VfsTranslateStatus::Translated) {
        return base_contain;
    }
#endif

    out_base_path = JoinRootAndParts(matched_base_root, base_rel_parts, parsed.prefix_kind);
    out_overlay_path = g_overlay_root_.empty()
                           ? std::wstring{}
                           : JoinRootAndParts(g_overlay_root_, parsed.parts, parsed.prefix_kind);
    out_rel_path = JoinParts(parsed.parts, parsed.parts.size());

#if defined(_WIN32)
    if (!g_overlay_root_.empty() && HasAnyWhiteoutInOverlay(g_overlay_root_, parsed.parts)) {
        if (out_was_whiteout) {
            *out_was_whiteout = true;
        }
        return VfsTranslateStatus::DeletedWhiteout;
    }
#endif
    return VfsTranslateStatus::Translated;
}

}  // namespace openscarlett
