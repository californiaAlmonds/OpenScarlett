#include "openscarlett/loader/apc_injector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/vfs_mounts.h"

#if defined(_WIN32)
#include <windows.h>
#include <winternl.h>
#endif

namespace openscarlett {

namespace {

#if defined(_WIN32)

std::string FormatWin32Error(const char* action, DWORD err) {
    return std::string(action) + " failed (Win32 error " + std::to_string(err) + ")";
}

std::wstring GetCurrentExecutableDirW() {
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
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

std::wstring ResolveAbsolutePathW(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    auto abs_p = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec) {
        return path;
    }
    return abs_p.lexically_normal().wstring();
}

bool FileExistsW(const std::wstring& path) {
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool HasEnvVarW(const wchar_t* name) {
    return ::GetEnvironmentVariableW(name, nullptr, 0) > 0;
}

std::string SanitizeFolderSegment(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '.' || c == '-' || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

void UpsertEnvEntry(std::vector<std::pair<std::wstring, std::wstring>>& entries,
                    const std::wstring& key,
                    const std::wstring& value) {
    for (auto& kv : entries) {
        if (::_wcsicmp(kv.first.c_str(), key.c_str()) == 0) {
            kv.second = value;
            return;
        }
    }
    entries.emplace_back(key, value);
}

void RemoveEnvEntry(std::vector<std::pair<std::wstring, std::wstring>>& entries,
                    const std::wstring& key) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const std::pair<std::wstring, std::wstring>& kv) {
                           return ::_wcsicmp(kv.first.c_str(), key.c_str()) == 0;
                       }),
        entries.end());
}

std::vector<wchar_t> BuildChildEnvironmentBlock(const std::wstring& shim_dir,
                                                const std::wstring& hook_dll_path,
                                                const std::wstring& vfs_g,
                                                const std::wstring& vfs_g_overlay,
                                                const std::wstring& vfs_t,
                                                const std::wstring& vfs_s,
                                                const std::wstring& title_id,
                                                const std::wstring& package_id,
                                                const std::wstring& telemetry_path) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;

    LPWCH raw_env = ::GetEnvironmentStringsW();
    if (raw_env != nullptr) {
        const wchar_t* cur = raw_env;
        while (*cur != L'\0') {
            std::wstring entry(cur);
            cur += entry.size() + 1;
            size_t eq = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (eq == std::wstring::npos) {
                continue;
            }
            entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
        ::FreeEnvironmentStringsW(raw_env);
    }

    // Prepend shim_dir to PATH so static PE imports of Xbox-exclusive DLLs resolve cleanly
    // without touching the guest executable's directory.
    bool updated_path = false;
    for (auto& kv : entries) {
        if (::_wcsicmp(kv.first.c_str(), L"PATH") == 0) {
            if (kv.second.empty()) {
                kv.second = shim_dir;
            } else {
                kv.second = shim_dir + L";" + kv.second;
            }
            updated_path = true;
            break;
        }
    }
    if (!updated_path) {
        entries.emplace_back(L"PATH", shim_dir);
    }

    UpsertEnvEntry(entries, L"OPENSCARLETT_SHIM_DIR", shim_dir);
    if (!hook_dll_path.empty()) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_HOOK_DLL", hook_dll_path);
    }
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_G", vfs_g);
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_G_OVERLAY", vfs_g_overlay);
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_T", vfs_t);
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_S", vfs_s);
    UpsertEnvEntry(entries, L"OPENSCARLETT_SAVE_ROOT", vfs_s);
    if (!HasEnvVarW(L"OPENSCARLETT_PLS_DRIVE_P") && !HasEnvVarW(L"OPENSCARLETT_PLS_PATH")) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_PLS_DRIVE_P", L"1");
    }
    RemoveEnvEntry(entries, L"OPENSCARLETT_CHILD_TELEMETRY_PER_PID");

    if (!title_id.empty()) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_TITLE_ID", title_id);
    } else {
        RemoveEnvEntry(entries, L"OPENSCARLETT_TITLE_ID");
    }

    if (!package_id.empty()) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_PACKAGE_ID", package_id);
    } else {
        RemoveEnvEntry(entries, L"OPENSCARLETT_PACKAGE_ID");
    }

    if (!telemetry_path.empty()) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_PATH", telemetry_path);
        UpsertEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_BASE", telemetry_path);
    } else {
        RemoveEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_PATH");
        RemoveEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_BASE");
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::pair<std::wstring, std::wstring>& a,
                 const std::pair<std::wstring, std::wstring>& b) {
                  return ::_wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
              });

    std::vector<wchar_t> block;
    block.reserve(4096);
    for (const auto& kv : entries) {
        block.insert(block.end(), kv.first.begin(), kv.first.end());
        block.push_back(L'=');
        block.insert(block.end(), kv.second.begin(), kv.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool StartsWithIgnoreCaseAsciiW(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        wchar_t a = text[i];
        wchar_t b = prefix[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool HasEnvKeyIgnoreCase(const std::vector<std::pair<std::wstring, std::wstring>>& entries,
                         const std::wstring& key) {
    for (const auto& kv : entries) {
        if (::_wcsicmp(kv.first.c_str(), key.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring AnsiStrToWideAcp(const char* text) {
    if (!text || *text == '\0') {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), needed);
    return out;
}

bool IsTargetProcessCompatibleX64(HANDLE h_process, DWORD& out_win32_error) {
    out_win32_error = ERROR_SUCCESS;
#if defined(_M_X64) || defined(__x86_64__)
    using PfnIsWow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    auto fn_wow64_2 = k32 ? reinterpret_cast<PfnIsWow64Process2>(
                                ::GetProcAddress(k32, "IsWow64Process2"))
                          : nullptr;
    if (fn_wow64_2 != nullptr) {
        USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!fn_wow64_2(h_process, &process_machine, &native_machine)) {
            out_win32_error = ::GetLastError();
            return false;
        }
        // Native x64 process on x64 host reports process_machine == IMAGE_FILE_MACHINE_UNKNOWN.
        return process_machine == IMAGE_FILE_MACHINE_UNKNOWN &&
               native_machine == IMAGE_FILE_MACHINE_AMD64;
    }
    BOOL is_wow64 = FALSE;
    if (!::IsWow64Process(h_process, &is_wow64)) {
        out_win32_error = ::GetLastError();
        return false;
    }
    return is_wow64 == FALSE;
#else
    (void)h_process;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Static Missing-DLL PE32+ Stub Synthesizer (--probe-unknown-imports)
// ---------------------------------------------------------------------------
bool IsProbeUnknownImportsEnvEnabled() {
    char buf[64]{};
    const DWORD len = ::GetEnvironmentVariableA(
        "OPENSCARLETT_PROBE_UNKNOWN_IMPORTS", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    std::string val(buf, len);
    size_t b = 0;
    while (b < val.size() && std::isspace(static_cast<unsigned char>(val[b])) != 0) ++b;
    size_t e = val.size();
    while (e > b && std::isspace(static_cast<unsigned char>(val[e - 1])) != 0) --e;
    val = val.substr(b, e - b);
    for (char& c : val) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return val == "1" || val == "true" || val == "yes" || val == "on";
}

bool IsOnSystemDriveCW(std::wstring_view path) noexcept {
    return path.size() >= 2u &&
           (path[0] == L'C' || path[0] == L'c') &&
           path[1] == L':';
}

std::wstring GetProbeStubSessionDirW(std::wstring_view shim_dir_w, DWORD child_pid) {
    std::wstring base_root;

    // Prefer TEMP when it is NOT on C:\ (e.g. build_wt.cmd / build.cmd sets TEMP on E:\).
    wchar_t temp_buf[MAX_PATH * 2]{};
    const DWORD temp_len = ::GetTempPathW(
        static_cast<DWORD>(sizeof(temp_buf) / sizeof(temp_buf[0])), temp_buf);
    std::wstring temp_path;
    if (temp_len > 0 && temp_len < sizeof(temp_buf) / sizeof(temp_buf[0])) {
        temp_path.assign(temp_buf, temp_len);
    }

    if (!temp_path.empty() && !IsOnSystemDriveCW(temp_path)) {
        base_root = (std::filesystem::path(temp_path) / L"openscarlett_probe_stubs").wstring();
    } else if (!shim_dir_w.empty()) {
        base_root = (std::filesystem::path(shim_dir_w) / L".probe_stubs").wstring();
    } else if (!temp_path.empty()) {
        base_root = (std::filesystem::path(temp_path) / L"openscarlett_probe_stubs").wstring();
    }

    if (base_root.empty()) {
        return {};
    }
    const std::wstring pid_folder = L"pid_" + std::to_wstring(static_cast<unsigned long>(child_pid));
    return (std::filesystem::path(base_root) / pid_folder).wstring();
}

std::string ExtractDllBasenameAscii(std::string_view raw) {
    size_t b = 0;
    while (b < raw.size() && std::isspace(static_cast<unsigned char>(raw[b])) != 0) ++b;
    size_t e = raw.size();
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1])) != 0) --e;
    std::string_view trimmed = raw.substr(b, e - b);
    const size_t slash = trimmed.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        trimmed = trimmed.substr(slash + 1u);
    }
    return std::string(trimmed);
}

std::string ToLowerAsciiLocal(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool IsApiSetContractAscii(std::string_view basename_lower) noexcept {
    return basename_lower.rfind("api-ms-", 0) == 0 || basename_lower.rfind("ext-ms-", 0) == 0;
}

bool DllExistsInDirectoryW(std::wstring_view dir_w, const std::wstring& filename_w) {
    if (dir_w.empty() || filename_w.empty()) {
        return false;
    }
    const std::filesystem::path full = std::filesystem::path(dir_w) / filename_w;
    return FileExistsW(full.wstring());
}

std::wstring QueryProcessExeDirW(HANDLE h_process) {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD sz = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (!::QueryFullProcessImageNameW(h_process, 0, buf, &sz) || sz == 0) {
        return {};
    }
    return std::filesystem::path(std::wstring(buf, sz)).parent_path().wstring();
}

bool IsDllResolvableOnHost(std::string_view dll_name,
                           std::wstring_view shim_dir_w,
                           std::wstring_view guest_dir_w,
                           std::wstring_view exe_dir_w) {
    const std::string base = ExtractDllBasenameAscii(dll_name);
    if (base.empty()) {
        return true;
    }
    const std::string base_lo = ToLowerAsciiLocal(base);
    if (IsApiSetContractAscii(base_lo) || IsWindowsKnownDll(base_lo)) {
        return true;
    }

    const std::wstring base_w = Utf8ToWide(base);
    if (DllExistsInDirectoryW(shim_dir_w, base_w) ||
        DllExistsInDirectoryW(guest_dir_w, base_w) ||
        DllExistsInDirectoryW(exe_dir_w, base_w)) {
        return true;
    }

    wchar_t sys_dir[MAX_PATH]{};
    const UINT sys_len = ::GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (sys_len > 0 && sys_len < MAX_PATH) {
        if (DllExistsInDirectoryW(std::wstring_view(sys_dir, sys_len), base_w)) {
            return true;
        }
    }

    wchar_t win_dir[MAX_PATH]{};
    const UINT win_len = ::GetWindowsDirectoryW(win_dir, MAX_PATH);
    if (win_len > 0 && win_len < MAX_PATH) {
        if (DllExistsInDirectoryW(std::wstring_view(win_dir, win_len), base_w)) {
            return true;
        }
    }

    return false;
}

uint32_t AlignUpU32(uint32_t val, uint32_t align) noexcept {
    return (val + align - 1u) & ~(align - 1u);
}

// Synthesizes a minimal, self-contained, spec-compliant PE32+ (x86-64) stub DLL
// exporting every requested named and ordinal symbol as an E_NOTIMPL (0x80004001) trampoline
// with a minimal DllMain returning TRUE (1).
bool SynthesizeStaticStubDllPE64(const std::wstring& output_path_w,
                                 const std::string& internal_dll_name,
                                 const std::vector<std::string>& raw_named_imports,
                                 const std::vector<uint16_t>& raw_ordinal_imports) {
    // 1. Deduplicate and sort named imports in strict ascending ASCII byte order
    //    (required by Windows ntdll LdrpNameToOrdinal binary search!).
    std::vector<std::string> named = raw_named_imports;
    std::sort(named.begin(), named.end(), [](const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    });
    named.erase(std::unique(named.begin(), named.end()), named.end());

    // 2. Deduplicate and sort ordinal imports.
    std::vector<uint16_t> ordinals = raw_ordinal_imports;
    std::sort(ordinals.begin(), ordinals.end());
    ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());

    // 3. Assign EAT (AddressOfFunctions) slots and ordinal Base.
    const uint32_t ordinal_base = ordinals.empty() ? 1u : static_cast<uint32_t>(ordinals.front());
    uint32_t eat_count = ordinals.empty()
                             ? 0u
                             : static_cast<uint32_t>(ordinals.back() - ordinals.front()) + 1u;

    // Reserve slots for ordinal imports first, then place named imports in free slots
    // (keeping every AddressOfNameOrdinals entry strictly within uint16_t range).
    std::vector<uint32_t> eat_rvas(eat_count, 0u);
    std::vector<uint8_t> slot_used(eat_count, 0u);

    constexpr uint32_t kTextVAddr = 0x1000u;
    constexpr uint32_t kTrampolineBaseOff = 0x10u;  // 0x00..0x0F holds DllMain
    constexpr uint32_t kTrampolineStride = 8u;
    uint32_t next_trampoline_idx = 0u;

    for (uint16_t ord : ordinals) {
        const uint32_t slot = static_cast<uint32_t>(ord) - ordinal_base;
        if (slot < eat_rvas.size()) {
            eat_rvas[slot] = kTextVAddr + kTrampolineBaseOff + next_trampoline_idx * kTrampolineStride;
            slot_used[slot] = 1u;
            ++next_trampoline_idx;
        }
    }

    std::vector<uint16_t> name_ordinals;
    name_ordinals.reserve(named.size());
    uint32_t free_scan = 0u;
    for (size_t i = 0; i < named.size(); ++i) {
        while (free_scan < slot_used.size() && slot_used[free_scan] != 0u) {
            ++free_scan;
        }
        uint32_t chosen_slot = free_scan;
        if (chosen_slot >= eat_rvas.size()) {
            chosen_slot = static_cast<uint32_t>(eat_rvas.size());
            if (chosen_slot > 0xFFFFu) {
                return false;
            }
            eat_rvas.push_back(0u);
            slot_used.push_back(0u);
        }
        eat_rvas[chosen_slot] = kTextVAddr + kTrampolineBaseOff + next_trampoline_idx * kTrampolineStride;
        slot_used[chosen_slot] = 1u;
        name_ordinals.push_back(static_cast<uint16_t>(chosen_slot));
        ++next_trampoline_idx;
        ++free_scan;
    }

    // 4. Build .text section bytes:
    //    - 0x00: DllMain -> mov eax, 1; ret
    //    - 0x10 + i*8: E_NOTIMPL trampoline -> mov eax, 0x80004001; ret
    constexpr uint32_t kFileAlign = 0x200u;
    constexpr uint32_t kSectAlign = 0x1000u;
    constexpr uint32_t kSizeOfHeaders = 0x200u;

    const uint32_t text_vsize = kTrampolineBaseOff + next_trampoline_idx * kTrampolineStride;
    const uint32_t text_raw_size = AlignUpU32(std::max<uint32_t>(text_vsize, 0x10u), kFileAlign);
    std::vector<uint8_t> text_sec(text_raw_size, 0xCCu);

    // DllMain: mov eax, 1; ret
    static constexpr uint8_t kDllMainCode[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(text_sec.data(), kDllMainCode, sizeof(kDllMainCode));

    // Export trampolines: mov eax, 0x80004001 (E_NOTIMPL); ret
    static constexpr uint8_t kNotImplCode[6] = {0xB8, 0x01, 0x40, 0x00, 0x80, 0xC3};
    for (uint32_t i = 0; i < next_trampoline_idx; ++i) {
        std::memcpy(text_sec.data() + kTrampolineBaseOff + i * kTrampolineStride,
                    kNotImplCode, sizeof(kNotImplCode));
    }

    // 5. Build .edata section bytes (IMAGE_EXPORT_DIRECTORY + EAT + NamePtrs + NameOrds + strings).
    const uint32_t edata_vaddr = kTextVAddr + AlignUpU32(std::max<uint32_t>(text_vsize, 0x10u), kSectAlign);

    std::vector<uint8_t> edata_buf;
    edata_buf.reserve(512u + eat_rvas.size() * 4u + named.size() * 32u);

    auto append_zeros = [&](size_t count) {
        edata_buf.insert(edata_buf.end(), count, 0u);
    };
    auto append_u32 = [&](uint32_t v) {
        const size_t pos = edata_buf.size();
        edata_buf.resize(pos + 4u);
        std::memcpy(edata_buf.data() + pos, &v, 4u);
    };
    auto append_u16 = [&](uint16_t v) {
        const size_t pos = edata_buf.size();
        edata_buf.resize(pos + 2u);
        std::memcpy(edata_buf.data() + pos, &v, 2u);
    };
    auto align_buf = [&](size_t a) {
        const size_t rem = edata_buf.size() % a;
        if (rem != 0u) {
            append_zeros(a - rem);
        }
    };
    auto append_cstr = [&](const std::string& s) -> uint32_t {
        const uint32_t rva = edata_vaddr + static_cast<uint32_t>(edata_buf.size());
        edata_buf.insert(edata_buf.end(), s.begin(), s.end());
        edata_buf.push_back(0u);
        return rva;
    };

    // Reserve 40-byte IMAGE_EXPORT_DIRECTORY at offset 0 of .edata.
    append_zeros(sizeof(IMAGE_EXPORT_DIRECTORY));

    // AddressOfFunctions (uint32_t[NumberOfFunctions])
    align_buf(4u);
    const uint32_t eat_rva = eat_rvas.empty()
                                 ? 0u
                                 : (edata_vaddr + static_cast<uint32_t>(edata_buf.size()));
    for (uint32_t fn_rva : eat_rvas) {
        append_u32(fn_rva);
    }

    // AddressOfNames (uint32_t[NumberOfNames]) - backfilled once strings are appended
    align_buf(4u);
    const size_t name_ptrs_off = edata_buf.size();
    const uint32_t name_ptrs_rva = named.empty()
                                       ? 0u
                                       : (edata_vaddr + static_cast<uint32_t>(name_ptrs_off));
    append_zeros(named.size() * 4u);

    // AddressOfNameOrdinals (uint16_t[NumberOfNames])
    align_buf(2u);
    const uint32_t name_ords_rva = named.empty()
                                       ? 0u
                                       : (edata_vaddr + static_cast<uint32_t>(edata_buf.size()));
    for (uint16_t ord_idx : name_ordinals) {
        append_u16(ord_idx);
    }

    // DLL name string & export name strings
    const std::string dll_label = internal_dll_name.empty() ? "probe_stub.dll" : internal_dll_name;
    const uint32_t dll_name_rva = append_cstr(dll_label);
    for (size_t i = 0; i < named.size(); ++i) {
        const uint32_t sym_rva = append_cstr(named[i]);
        std::memcpy(edata_buf.data() + name_ptrs_off + i * 4u, &sym_rva, 4u);
    }

    IMAGE_EXPORT_DIRECTORY exp_dir{};
    exp_dir.Characteristics = 0u;
    exp_dir.TimeDateStamp = 0x65000000u;
    exp_dir.MajorVersion = 0u;
    exp_dir.MinorVersion = 0u;
    exp_dir.Name = dll_name_rva;
    exp_dir.Base = ordinal_base;
    exp_dir.NumberOfFunctions = static_cast<DWORD>(eat_rvas.size());
    exp_dir.NumberOfNames = static_cast<DWORD>(named.size());
    exp_dir.AddressOfFunctions = eat_rva;
    exp_dir.AddressOfNames = name_ptrs_rva;
    exp_dir.AddressOfNameOrdinals = name_ords_rva;
    std::memcpy(edata_buf.data(), &exp_dir, sizeof(exp_dir));

    const uint32_t edata_vsize = static_cast<uint32_t>(edata_buf.size());
    const uint32_t edata_raw_size = AlignUpU32(edata_vsize, kFileAlign);
    edata_buf.resize(edata_raw_size, 0u);

    // 6. Assemble complete PE32+ headers + sections.
    const uint32_t size_of_image = edata_vaddr + AlignUpU32(edata_vsize, kSectAlign);
    const uint32_t total_file_size = kSizeOfHeaders + text_raw_size + edata_raw_size;
    std::vector<uint8_t> image(total_file_size, 0u);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x40;
    std::memcpy(image.data(), &dos, sizeof(dos));

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 2u;
    nt.FileHeader.TimeDateStamp = 0x65000000u;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;

    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.MajorLinkerVersion = 14u;
    nt.OptionalHeader.MinorLinkerVersion = 0u;
    nt.OptionalHeader.SizeOfCode = text_raw_size;
    nt.OptionalHeader.SizeOfInitializedData = edata_raw_size;
    nt.OptionalHeader.AddressOfEntryPoint = kTextVAddr;
    nt.OptionalHeader.BaseOfCode = kTextVAddr;
    nt.OptionalHeader.ImageBase = 0x0000000180000000ull;
    nt.OptionalHeader.SectionAlignment = kSectAlign;
    nt.OptionalHeader.FileAlignment = kFileAlign;
    nt.OptionalHeader.MajorOperatingSystemVersion = 6u;
    nt.OptionalHeader.MinorOperatingSystemVersion = 0u;
    nt.OptionalHeader.MajorSubsystemVersion = 6u;
    nt.OptionalHeader.MinorSubsystemVersion = 0u;
    nt.OptionalHeader.SizeOfImage = size_of_image;
    nt.OptionalHeader.SizeOfHeaders = kSizeOfHeaders;
    nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    nt.OptionalHeader.DllCharacteristics =
        IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA |
        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
        IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    nt.OptionalHeader.SizeOfStackReserve = 0x100000ull;
    nt.OptionalHeader.SizeOfStackCommit = 0x1000ull;
    nt.OptionalHeader.SizeOfHeapReserve = 0x100000ull;
    nt.OptionalHeader.SizeOfHeapCommit = 0x1000ull;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = edata_vaddr;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = edata_vsize;

    const size_t nt_off = static_cast<size_t>(dos.e_lfanew);
    std::memcpy(image.data() + nt_off, &nt, sizeof(nt));

    IMAGE_SECTION_HEADER sec_text{};
    std::memcpy(sec_text.Name, ".text", 5u);
    sec_text.Misc.VirtualSize = text_vsize;
    sec_text.VirtualAddress = kTextVAddr;
    sec_text.SizeOfRawData = text_raw_size;
    sec_text.PointerToRawData = kSizeOfHeaders;
    sec_text.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    IMAGE_SECTION_HEADER sec_edata{};
    std::memcpy(sec_edata.Name, ".edata", 6u);
    sec_edata.Misc.VirtualSize = edata_vsize;
    sec_edata.VirtualAddress = edata_vaddr;
    sec_edata.SizeOfRawData = edata_raw_size;
    sec_edata.PointerToRawData = kSizeOfHeaders + text_raw_size;
    sec_edata.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    const size_t sec_off = nt_off + sizeof(IMAGE_NT_HEADERS64);
    std::memcpy(image.data() + sec_off, &sec_text, sizeof(sec_text));
    std::memcpy(image.data() + sec_off + sizeof(IMAGE_SECTION_HEADER), &sec_edata, sizeof(sec_edata));

    std::memcpy(image.data() + kSizeOfHeaders, text_sec.data(), text_sec.size());
    std::memcpy(image.data() + kSizeOfHeaders + text_raw_size, edata_buf.data(), edata_buf.size());

    // 7. Write PE32+ stub DLL to disk.
    const std::filesystem::path out_p(output_path_w);
    if (!out_p.parent_path().empty()) {
        EnsureDirectoryTreeW(out_p.parent_path().wstring());
    }
    std::ofstream ofs(out_p, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return ofs.good();
}

void CollectRemoteDescriptorImports(HANDLE h_process,
                                    uintptr_t image_base,
                                    uint32_t image_size,
                                    const IMAGE_IMPORT_DESCRIPTOR& desc,
                                    std::vector<std::string>& out_named,
                                    std::vector<uint16_t>& out_ordinals) {
    const uint32_t thunk_rva =
        (desc.OriginalFirstThunk != 0u && desc.OriginalFirstThunk < image_size)
            ? desc.OriginalFirstThunk
            : desc.FirstThunk;
    if (thunk_rva == 0u || thunk_rva >= image_size) {
        return;
    }

    const size_t max_entries =
        std::min<size_t>(65536u, (static_cast<size_t>(image_size) - thunk_rva) / sizeof(uint64_t));
    constexpr size_t kChunk = 128u;
    uint64_t chunk_buf[kChunk]{};

    for (size_t base_idx = 0; base_idx < max_entries; base_idx += kChunk) {
        const size_t count = std::min<size_t>(kChunk, max_entries - base_idx);
        SIZE_T rw = 0;
        if (!::ReadProcessMemory(
                h_process,
                reinterpret_cast<LPCVOID>(image_base + thunk_rva + base_idx * sizeof(uint64_t)),
                chunk_buf,
                count * sizeof(uint64_t),
                &rw) ||
            rw < sizeof(uint64_t)) {
            break;
        }
        const size_t read_entries = rw / sizeof(uint64_t);
        for (size_t i = 0; i < read_entries; ++i) {
            const uint64_t entry = chunk_buf[i];
            if (entry == 0ull) {
                return;
            }
            if ((entry & IMAGE_ORDINAL_FLAG64) != 0ull) {
                out_ordinals.push_back(static_cast<uint16_t>(entry & 0xFFFFull));
            } else if (entry + 2ull < static_cast<uint64_t>(image_size)) {
                char sym_buf[512]{};
                const size_t sym_rva = static_cast<size_t>(entry) + 2u;  // skip 2-byte Hint
                const size_t max_sym = std::min<size_t>(sizeof(sym_buf) - 1u, image_size - sym_rva);
                SIZE_T sym_rw = 0;
                if (::ReadProcessMemory(h_process,
                                        reinterpret_cast<LPCVOID>(image_base + sym_rva),
                                        sym_buf,
                                        max_sym,
                                        &sym_rw) &&
                    sym_rw > 0) {
                    sym_buf[sym_rw] = '\0';
                    if (sym_buf[0] != '\0') {
                        out_named.emplace_back(sym_buf);
                    }
                }
            }
        }
        if (read_entries < count) {
            break;
        }
    }
}

// Redirect static PE imports of routed DLLs in the suspended child process BEFORE
// LdrInitializeThunk runs using OpenScarlett's unified data-driven DllRoutingTable.
// When OPENSCARLETT_PROBE_UNKNOWN_IMPORTS=1 (--probe-unknown-imports) is active,
// also synthesizes spec-compliant PE32+ E_NOTIMPL stub DLLs for any missing non-Windows
// static DLL imports so ntdll never aborts with STATUS_DLL_NOT_FOUND (0xC0000135).
bool RedirectChildStaticXboxImports(HANDLE h_process,
                                    const std::wstring& shim_dir_w,
                                    const std::wstring& guest_dir_w) {
#if defined(_M_X64) || defined(__x86_64__)
    using PfnNtQueryInformationProcess = NTSTATUS(NTAPI*)(
        HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    HMODULE h_ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto fn_ntq = h_ntdll ? reinterpret_cast<PfnNtQueryInformationProcess>(
                                ::GetProcAddress(h_ntdll, "NtQueryInformationProcess"))
                          : nullptr;
    if (!fn_ntq) {
        return false;
    }

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG ret_len = 0;
    if (fn_ntq(h_process, ProcessBasicInformation, &pbi, sizeof(pbi), &ret_len) != 0 ||
        pbi.PebBaseAddress == nullptr) {
        return false;
    }

    // Read PEB.ImageBaseAddress (offset 0x10 on Windows x64).
    uintptr_t image_base = 0;
    SIZE_T rw = 0;
    const auto peb_addr = reinterpret_cast<uintptr_t>(pbi.PebBaseAddress);
    if (!::ReadProcessMemory(h_process,
                             reinterpret_cast<LPCVOID>(peb_addr + 0x10u),
                             &image_base,
                             sizeof(image_base),
                             &rw) ||
        rw != sizeof(image_base) || image_base == 0) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!::ReadProcessMemory(h_process, reinterpret_cast<LPCVOID>(image_base),
                             &dos, sizeof(dos), &rw) ||
        rw != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    if (!::ReadProcessMemory(h_process,
                             reinterpret_cast<LPCVOID>(image_base + static_cast<uintptr_t>(dos.e_lfanew)),
                             &nt, sizeof(nt), &rw) ||
        rw != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }

    const uint32_t image_size = nt.OptionalHeader.SizeOfImage;
    const auto& imp_dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto& dly_dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    const bool has_imp_dir = (imp_dir.VirtualAddress != 0 && imp_dir.VirtualAddress < image_size);
    const bool has_dly_dir = (dly_dir.VirtualAddress != 0 && dly_dir.VirtualAddress < image_size);
    if (!has_imp_dir && !has_dly_dir) {
        return true;
    }

    const DllRoutingTable routing = DllRoutingTable::CreateWithEnvOverrides();
    const PeForwarderResolver forwarder_resolver(routing);
    const bool probe_unknown_imports = IsProbeUnknownImportsEnvEnabled();
    const std::wstring exe_dir_w = probe_unknown_imports ? QueryProcessExeDirW(h_process) : std::wstring{};

    std::vector<IMAGE_IMPORT_DESCRIPTOR> descs;
    std::vector<std::string> imported_names;
    bool has_console_exclusive_import = false;

    const uintptr_t imp_table_addr = has_imp_dir ? (image_base + imp_dir.VirtualAddress) : 0u;
    if (has_imp_dir) {
        for (size_t idx = 0; idx < 1024; ++idx) {
            IMAGE_IMPORT_DESCRIPTOR d{};
            if (!::ReadProcessMemory(h_process,
                                     reinterpret_cast<LPCVOID>(imp_table_addr + idx * sizeof(d)),
                                     &d, sizeof(d), &rw) ||
                rw != sizeof(d)) {
                break;
            }
            if (d.OriginalFirstThunk == 0 && d.Name == 0 && d.FirstThunk == 0) {
                break;
            }
            std::string dll_name;
            if (d.Name != 0 && d.Name < image_size) {
                char name_buf[260]{};
                const size_t max_read = std::min<size_t>(sizeof(name_buf) - 1, image_size - d.Name);
                if (::ReadProcessMemory(h_process,
                                        reinterpret_cast<LPCVOID>(image_base + d.Name),
                                        name_buf, max_read, &rw) &&
                    rw > 0) {
                    name_buf[rw] = '\0';
                    dll_name = name_buf;
                }
            }
            if (!dll_name.empty() && ClassifyImportDll(dll_name) == DllCategory::ConsoleExclusive) {
                has_console_exclusive_import = true;
            }
            imported_names.push_back(std::move(dll_name));
            descs.push_back(d);
        }
    }

    // Also inspect PE delay-load imports (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) so delay-loaded
    // console DLLs are discovered and redirected cleanly at launch before resume.
    std::vector<IMAGE_DELAYLOAD_DESCRIPTOR> delay_descs;
    std::vector<std::string> delay_names;
    const uintptr_t dly_table_addr = has_dly_dir ? (image_base + dly_dir.VirtualAddress) : 0u;
    if (has_dly_dir) {
        for (size_t idx = 0; idx < 1024; ++idx) {
            if (dly_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) > image_size) {
                break;
            }
            IMAGE_DELAYLOAD_DESCRIPTOR d{};
            if (!::ReadProcessMemory(h_process,
                                     reinterpret_cast<LPCVOID>(dly_table_addr + idx * sizeof(d)),
                                     &d, sizeof(d), &rw) ||
                rw != sizeof(d)) {
                break;
            }
            if (d.DllNameRVA == 0 && d.ImportAddressTableRVA == 0) {
                break;
            }
            std::string dll_name;
            if (d.Attributes.RvaBased != 0 && d.DllNameRVA != 0 && d.DllNameRVA < image_size) {
                char name_buf[260]{};
                const size_t max_read = std::min<size_t>(sizeof(name_buf) - 1, image_size - d.DllNameRVA);
                if (::ReadProcessMemory(h_process,
                                        reinterpret_cast<LPCVOID>(image_base + d.DllNameRVA),
                                        name_buf, max_read, &rw) &&
                    rw > 0) {
                    name_buf[rw] = '\0';
                    dll_name = name_buf;
                }
            }
            if (!dll_name.empty() && ClassifyImportDll(dll_name) == DllCategory::ConsoleExclusive) {
                has_console_exclusive_import = true;
            }
            delay_names.push_back(std::move(dll_name));
            delay_descs.push_back(d);
        }
    }

    auto resolve_redirect_for_dll = [&](const std::string& dll_name,
                                        std::string& out_path,
                                        int& out_priority) -> bool {
        out_path.clear();
        out_priority = 10;
        if (dll_name.empty()) {
            return false;
        }
        DllRouteDecision decision =
            routing.ResolveRoute(dll_name, shim_dir_w, guest_dir_w, has_console_exclusive_import);
        if (decision.policy != DllRoutePolicy::Shim) {
            std::string canon_dll;
            ForwarderResolutionKind fkind = ForwarderResolutionKind::Unresolved;
            bool via_api_set = false;
            bool via_alias = false;
            if (forwarder_resolver.ResolveTargetDll(dll_name, canon_dll, fkind, via_api_set, via_alias) &&
                fkind == ForwarderResolutionKind::OpenScarlettShim) {
                decision = routing.ResolveRoute(
                    canon_dll, shim_dir_w, guest_dir_w, has_console_exclusive_import);
            }
        }
        const bool should_rewrite =
            !decision.resolved_path.empty() &&
            (decision.policy == DllRoutePolicy::Shim ||
             (decision.policy == DllRoutePolicy::NativeGuestDll && decision.from_override));
        if (!should_rewrite) {
            return false;
        }
        out_path = WideToUtf8(decision.resolved_path);
        out_priority = decision.load_priority;
        return !out_path.empty();
    };

    std::vector<std::string> full_paths(descs.size());
    std::vector<int> priorities(descs.size(), 10);
    bool any_static_redirected = false;

    struct MissingDllGroup {
        std::string display_name;
        std::vector<size_t> desc_indices;
        std::vector<std::string> named_imports;
        std::vector<uint16_t> ordinal_imports;
    };
    std::unordered_map<std::string, size_t> missing_key_to_group;
    std::vector<MissingDllGroup> missing_groups;

    for (size_t idx = 0; idx < descs.size(); ++idx) {
        const std::string& dll_name = imported_names[idx];
        if (dll_name.empty()) {
            continue;
        }
        std::string resolved_path;
        int prio = 10;
        if (resolve_redirect_for_dll(dll_name, resolved_path, prio)) {
            full_paths[idx] = std::move(resolved_path);
            priorities[idx] = prio;
            any_static_redirected = true;
            continue;
        }

        // Probe-mode static missing-DLL synthesis: catch any unresolved non-Windows static DLL
        // before ntdll aborts startup with STATUS_DLL_NOT_FOUND (0xC0000135).
        if (probe_unknown_imports &&
            !IsDllResolvableOnHost(dll_name, shim_dir_w, guest_dir_w, exe_dir_w)) {
            const std::string base = ExtractDllBasenameAscii(dll_name);
            const std::string key = ToLowerAsciiLocal(base);
            auto it = missing_key_to_group.find(key);
            if (it == missing_key_to_group.end()) {
                const size_t g_idx = missing_groups.size();
                missing_key_to_group.emplace(key, g_idx);
                MissingDllGroup grp{};
                grp.display_name = base;
                missing_groups.push_back(std::move(grp));
                it = missing_key_to_group.find(key);
            }
            MissingDllGroup& grp = missing_groups[it->second];
            grp.desc_indices.push_back(idx);
            CollectRemoteDescriptorImports(
                h_process, image_base, image_size, descs[idx],
                grp.named_imports, grp.ordinal_imports);
        }
    }

    if (probe_unknown_imports && !missing_groups.empty()) {
        const DWORD child_pid = ::GetProcessId(h_process);
        const std::wstring session_dir_w = GetProbeStubSessionDirW(shim_dir_w, child_pid);
        if (!session_dir_w.empty()) {
            EnsureDirectoryTreeW(session_dir_w);
            for (const auto& grp : missing_groups) {
                std::string safe_file = SanitizeFolderSegment(grp.display_name);
                if (safe_file.empty()) {
                    safe_file = "probe_stub.dll";
                }
                const std::string safe_lo = ToLowerAsciiLocal(safe_file);
                if (safe_lo.size() < 4u || safe_lo.compare(safe_lo.size() - 4u, 4u, ".dll") != 0) {
                    safe_file.append(".dll");
                }
                const std::wstring stub_path_w =
                    (std::filesystem::path(session_dir_w) / Utf8ToWide(safe_file)).wstring();

                if (SynthesizeStaticStubDllPE64(
                        stub_path_w, grp.display_name, grp.named_imports, grp.ordinal_imports)) {
                    const std::string stub_path_u8 = WideToUtf8(stub_path_w);
                    for (size_t d_idx : grp.desc_indices) {
                        full_paths[d_idx] = stub_path_u8;
                        priorities[d_idx] = 8;
                    }
                    any_static_redirected = true;

                    CallTelemetry::Global().Record(
                        "probe_stub",
                        grp.display_name,
                        CallOutcome::NotImplemented,
                        static_cast<int32_t>(0x80004001u),
                        -1);
                    for (const auto& sym : grp.named_imports) {
                        CallTelemetry::Global().Record(
                            grp.display_name,
                            sym,
                            CallOutcome::NotImplemented,
                            static_cast<int32_t>(0x80004001u),
                            -1);
                    }
                    for (uint16_t ord : grp.ordinal_imports) {
                        CallTelemetry::Global().Record(
                            grp.display_name,
                            "#" + std::to_string(ord),
                            CallOutcome::NotImplemented,
                            static_cast<int32_t>(0x80004001u),
                            -1);
                    }

                    std::printf(
                        "[OpenScarlett Probe] Synthesized static E_NOTIMPL stub DLL for missing import '%s' "
                        "(%zu named, %zu ordinal) -> %s\n",
                        grp.display_name.c_str(),
                        grp.named_imports.size(),
                        grp.ordinal_imports.size(),
                        stub_path_u8.c_str());
                    std::fflush(stdout);
                }
            }
        }
    }

    std::vector<std::string> delay_full_paths;
    delay_full_paths.reserve(delay_descs.size());
    uint32_t delay_redirected_count = 0;
    for (size_t idx = 0; idx < delay_descs.size(); ++idx) {
        std::string resolved_path;
        int prio = 10;
        if (delay_descs[idx].Attributes.RvaBased != 0 &&
            resolve_redirect_for_dll(delay_names[idx], resolved_path, prio)) {
            delay_full_paths.push_back(std::move(resolved_path));
            ++delay_redirected_count;
        } else {
            delay_full_paths.emplace_back();
        }
    }

    if (!any_static_redirected && delay_redirected_count == 0u) {
        return true;
    }

    // Pack redirected full ASCII DLL paths (both static & delay-load) into a contiguous buffer
    // allocated within +2 GiB of image_base so every replacement RVA fits in a positive 32-bit offset.
    std::vector<char> str_blob;
    std::vector<uint32_t> str_offsets(descs.size(), 0u);
    for (size_t i = 0; i < descs.size(); ++i) {
        if (!full_paths[i].empty()) {
            str_offsets[i] = static_cast<uint32_t>(str_blob.size());
            str_blob.insert(str_blob.end(), full_paths[i].begin(), full_paths[i].end());
            str_blob.push_back('\0');
        }
    }
    std::vector<uint32_t> delay_str_offsets(delay_descs.size(), 0u);
    for (size_t i = 0; i < delay_descs.size(); ++i) {
        if (!delay_full_paths[i].empty()) {
            delay_str_offsets[i] = static_cast<uint32_t>(str_blob.size());
            str_blob.insert(str_blob.end(), delay_full_paths[i].begin(), delay_full_paths[i].end());
            str_blob.push_back('\0');
        }
    }

    const uintptr_t alloc_gran = 0x10000u;
    const uintptr_t start_probe = (image_base + image_size + alloc_gran - 1u) & ~(alloc_gran - 1u);
    const uintptr_t max_probe = image_base + 0x7FFE0000ull;
    LPVOID remote_str_base = nullptr;
    for (uintptr_t addr = start_probe; addr < max_probe; addr += alloc_gran) {
        remote_str_base = ::VirtualAllocEx(h_process,
                                           reinterpret_cast<LPVOID>(addr),
                                           str_blob.size(),
                                           MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
        if (remote_str_base != nullptr) {
            break;
        }
    }
    if (!remote_str_base) {
        return false;
    }

    if (!::WriteProcessMemory(h_process, remote_str_base, str_blob.data(), str_blob.size(), &rw) ||
        rw != str_blob.size()) {
        ::VirtualFreeEx(h_process, remote_str_base, 0, MEM_RELEASE);
        return false;
    }

    const uint32_t blob_base_rva =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(remote_str_base) - image_base);
    bool wrote_ok = true;

    if (any_static_redirected && !descs.empty()) {
        for (size_t i = 0; i < descs.size(); ++i) {
            if (!full_paths[i].empty()) {
                descs[i].Name = blob_base_rva + str_offsets[i];
                descs[i].TimeDateStamp = 0;
            }
        }

        // Order descriptors using deterministic Kahn's topological sort over PE imports
        // so dependencies always initialize strictly before their importers (breaking ties by load_priority).
        std::vector<DllDependencyNode> dep_nodes;
        dep_nodes.reserve(descs.size());
        for (size_t i = 0; i < descs.size(); ++i) {
            DllDependencyNode node{};
            const std::string& orig_name = imported_names[i];
            node.name = !full_paths[i].empty()
                            ? std::filesystem::path(full_paths[i]).filename().string()
                            : (orig_name.empty() ? ("import_" + std::to_string(i)) : orig_name);
            node.file_path = full_paths[i];
            node.load_priority = priorities[i];
            if (!full_paths[i].empty()) {
                PeInspectionReport dep_report{};
                if (InspectPeFile(full_paths[i], dep_report).ok()) {
                    for (const auto& imp : dep_report.imports) {
                        node.imports.push_back(imp.dll_name);
                    }
                }
            }
            dep_nodes.push_back(std::move(node));
        }

        const DllInitOrderResult topo = ComputeDllInitOrder(dep_nodes);
        std::vector<IMAGE_IMPORT_DESCRIPTOR> sorted_descs;
        sorted_descs.reserve(descs.size());
        for (size_t idx : topo.ordered_indices) {
            sorted_descs.push_back(descs[idx]);
        }
        CallTelemetry::Global().Record(
            "loader",
            "RedirectChildStaticXboxImports:topo_sort",
            CallOutcome::Ok,
            static_cast<int32_t>(sorted_descs.size()),
            -1);

        const size_t table_bytes = sorted_descs.size() * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        DWORD old_prot = 0;
        if (!::VirtualProtectEx(h_process, reinterpret_cast<LPVOID>(imp_table_addr),
                                table_bytes, PAGE_READWRITE, &old_prot)) {
            return false;
        }
        const bool static_ok =
            ::WriteProcessMemory(h_process, reinterpret_cast<LPVOID>(imp_table_addr),
                                 sorted_descs.data(), table_bytes, &rw) != 0 &&
            rw == table_bytes;
        DWORD dummy_prot = 0;
        ::VirtualProtectEx(h_process, reinterpret_cast<LPVOID>(imp_table_addr),
                           table_bytes, old_prot, &dummy_prot);
        wrote_ok = wrote_ok && static_ok;
    }

    if (delay_redirected_count > 0u && !delay_descs.empty()) {
        for (size_t i = 0; i < delay_descs.size(); ++i) {
            if (!delay_full_paths[i].empty()) {
                delay_descs[i].DllNameRVA = blob_base_rva + delay_str_offsets[i];
                delay_descs[i].TimeDateStamp = 0;
            }
        }
        const size_t delay_bytes = delay_descs.size() * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
        DWORD old_prot = 0;
        if (!::VirtualProtectEx(h_process, reinterpret_cast<LPVOID>(dly_table_addr),
                                delay_bytes, PAGE_READWRITE, &old_prot)) {
            return false;
        }
        const bool delay_ok =
            ::WriteProcessMemory(h_process, reinterpret_cast<LPVOID>(dly_table_addr),
                                 delay_descs.data(), delay_bytes, &rw) != 0 &&
            rw == delay_bytes;
        DWORD dummy_prot = 0;
        ::VirtualProtectEx(h_process, reinterpret_cast<LPVOID>(dly_table_addr),
                           delay_bytes, old_prot, &dummy_prot);
        CallTelemetry::Global().Record(
            "loader",
            "RedirectChildStaticXboxImports:delay_load",
            delay_ok ? CallOutcome::Ok : CallOutcome::Error,
            static_cast<int32_t>(delay_redirected_count),
            -1);
        wrote_ok = wrote_ok && delay_ok;
    }

    return wrote_ok;
#else
    (void)h_process;
    (void)shim_dir_w;
    (void)guest_dir_w;
    return false;
#endif
}

#endif  // _WIN32

std::string NormalizeDllGraphKey(std::string_view raw) {
    size_t pos = raw.find_last_of("\\/");
    std::string_view base = (pos == std::string_view::npos) ? raw : raw.substr(pos + 1);
    std::string out(base);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace

DllInitOrderResult ComputeDllInitOrder(const std::vector<DllDependencyNode>& nodes) {
    DllInitOrderResult result{};
    const size_t n = nodes.size();
    if (n == 0) {
        return result;
    }

    // Map normalized DLL basename -> first node index in the target set.
    std::unordered_map<std::string, size_t> name_to_index;
    name_to_index.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        std::string key = NormalizeDllGraphKey(nodes[i].name);
        if (key.empty() && !nodes[i].file_path.empty()) {
            key = NormalizeDllGraphKey(nodes[i].file_path);
        }
        if (!key.empty() && name_to_index.find(key) == name_to_index.end()) {
            name_to_index.emplace(std::move(key), i);
        }
    }

    // Directed graph: edge (dep -> importer) means `dep` must initialize before `importer`.
    std::vector<std::vector<size_t>> dependents(n);
    std::vector<size_t> in_degree(n, 0);

    for (size_t importer = 0; importer < n; ++importer) {
        std::unordered_set<size_t> seen_deps;
        for (const auto& raw_imp : nodes[importer].imports) {
            const std::string dep_key = NormalizeDllGraphKey(raw_imp);
            auto it = name_to_index.find(dep_key);
            if (it == name_to_index.end()) {
                continue;
            }
            const size_t dep = it->second;
            if (dep == importer) {
                continue;  // Ignore self-imports.
            }
            if (seen_deps.insert(dep).second) {
                dependents[dep].push_back(importer);
                ++in_degree[importer];
            }
        }
    }

    // Deterministic min-heap ordered by (load_priority, original_index).
    struct ReadyEntry {
        int priority;
        size_t index;
        bool operator>(const ReadyEntry& other) const noexcept {
            if (priority != other.priority) {
                return priority > other.priority;
            }
            return index > other.index;
        }
    };
    std::priority_queue<ReadyEntry, std::vector<ReadyEntry>, std::greater<ReadyEntry>> ready;
    for (size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) {
            ready.push(ReadyEntry{nodes[i].load_priority, i});
        }
    }

    std::vector<uint8_t> emitted(n, 0);
    result.ordered_indices.reserve(n);
    result.ordered_names.reserve(n);

    auto emit_node = [&](size_t u) {
        emitted[u] = 1;
        result.ordered_indices.push_back(u);
        result.ordered_names.push_back(nodes[u].name);
        for (size_t v : dependents[u]) {
            if (emitted[v] != 0) {
                continue;
            }
            if (in_degree[v] > 0) {
                --in_degree[v];
            }
            if (in_degree[v] == 0) {
                ready.push(ReadyEntry{nodes[v].load_priority, v});
            }
        }
    };

    while (result.ordered_indices.size() < n) {
        if (!ready.empty()) {
            const size_t u = ready.top().index;
            ready.pop();
            if (emitted[u] != 0) {
                continue;
            }
            emit_node(u);
        } else {
            // Cycle detected: peel downstream non-cycle nodes (out-degree 0 in the remaining
            // unemitted subgraph) so we break an actual cycle-forming node rather than a leaf
            // that merely depends on the cycle. Then deterministically pick the candidate with
            // lowest (in_degree, load_priority, original_index) and continue Kahn's sort.
            result.had_cycles = true;
            std::vector<uint8_t> in_core(n, 0);
            std::vector<size_t> rem_out(n, 0);
            for (size_t i = 0; i < n; ++i) {
                if (emitted[i] == 0) {
                    in_core[i] = 1;
                    for (size_t v : dependents[i]) {
                        if (emitted[v] == 0) {
                            ++rem_out[i];
                        }
                    }
                }
            }
            bool peeled = true;
            while (peeled) {
                peeled = false;
                for (size_t i = 0; i < n; ++i) {
                    if (in_core[i] != 0 && rem_out[i] == 0) {
                        in_core[i] = 0;
                        peeled = true;
                        for (size_t p = 0; p < n; ++p) {
                            if (in_core[p] == 0) continue;
                            for (size_t v : dependents[p]) {
                                if (v == i && rem_out[p] > 0) {
                                    --rem_out[p];
                                }
                            }
                        }
                    }
                }
            }
            bool any_core = false;
            for (size_t i = 0; i < n; ++i) {
                if (in_core[i] != 0) {
                    any_core = true;
                    break;
                }
            }

            size_t best = n;
            for (size_t i = 0; i < n; ++i) {
                if (emitted[i] != 0) {
                    continue;
                }
                if (any_core && in_core[i] == 0) {
                    continue;
                }
                if (best == n ||
                    in_degree[i] < in_degree[best] ||
                    (in_degree[i] == in_degree[best] &&
                     nodes[i].load_priority < nodes[best].load_priority) ||
                    (in_degree[i] == in_degree[best] &&
                     nodes[i].load_priority == nodes[best].load_priority &&
                     i < best)) {
                    best = i;
                }
            }
            if (best == n) {
                break;
            }
            result.cycle_broken_names.push_back(nodes[best].name);
            emit_node(best);
        }
    }

    return result;
}

Status ComputeDllInitOrderFromFiles(const std::vector<std::string>& dll_paths,
                                    DllInitOrderResult& out_order) {
    out_order = {};
    if (dll_paths.empty()) {
        return Status::Ok();
    }

    const DllRoutingTable routing = DllRoutingTable::CreateWithEnvOverrides();
    std::vector<DllDependencyNode> nodes;
    nodes.reserve(dll_paths.size());

    for (const auto& path : dll_paths) {
        PeInspectionReport report{};
        auto st = InspectPeFile(path, report);
        if (!st.ok()) {
            return st;
        }
        DllDependencyNode node{};
        node.name = std::filesystem::path(path).filename().string();
        node.file_path = path;
        if (const auto* rule = routing.FindRule(node.name)) {
            node.load_priority = rule->load_priority;
        }
        for (const auto& imp : report.imports) {
            node.imports.push_back(imp.dll_name);
        }
        nodes.push_back(std::move(node));
    }

    out_order = ComputeDllInitOrder(nodes);
    CallTelemetry::Global().Record(
        "loader",
        "ComputeDllInitOrderFromFiles",
        CallOutcome::Ok,
        static_cast<int32_t>(out_order.ordered_indices.size()),
        -1);
    return Status::Ok();
}

Status LoadDllsInTopologicalOrder(const std::vector<std::string>& dll_paths,
                                  std::vector<std::string>* out_loaded_order,
                                  std::vector<void*>* out_handles) {
    if (out_loaded_order) {
        out_loaded_order->clear();
    }
    if (out_handles) {
        out_handles->clear();
    }
#if !defined(_WIN32)
    (void)dll_paths;
    return Status::Error(StatusCode::NotImplemented, "DLL loading is Windows-only");
#else
    DllInitOrderResult order{};
    auto st = ComputeDllInitOrderFromFiles(dll_paths, order);
    if (!st.ok()) {
        return st;
    }

    for (size_t idx : order.ordered_indices) {
        const std::wstring wpath = ResolveAbsolutePathW(Utf8ToWide(dll_paths[idx]));
        HMODULE mod = ::LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            mod = ::LoadLibraryW(wpath.c_str());
        }
        if (!mod) {
            const DWORD err = ::GetLastError();
            CallTelemetry::Global().Record(
                "loader",
                "LoadDllsInTopologicalOrder:" + std::filesystem::path(dll_paths[idx]).filename().string(),
                CallOutcome::Error,
                static_cast<int32_t>(err),
                -1);
            return Status::Error(
                StatusCode::IoError,
                FormatWin32Error(("LoadLibraryW(" + dll_paths[idx] + ")").c_str(), err));
        }
        const std::string fname = std::filesystem::path(dll_paths[idx]).filename().string();
        CallTelemetry::Global().Record(
            "loader",
            "LoadDllsInTopologicalOrder:" + fname,
            CallOutcome::Ok,
            0,
            -1);
        if (out_loaded_order) {
            out_loaded_order->push_back(fname);
        }
        if (out_handles) {
            out_handles->push_back(reinterpret_cast<void*>(mod));
        }
    }
    return Status::Ok();
#endif
}

std::wstring FormatChildTelemetryPathW(std::wstring_view base_telemetry_path, uint32_t child_pid) {
    if (base_telemetry_path.empty() || child_pid == 0u) {
        return {};
    }
    const std::filesystem::path base_p(base_telemetry_path);
    const std::filesystem::path parent_p = base_p.parent_path();
    std::wstring stem = base_p.stem().wstring();

    auto ends_with_ci = [](const std::wstring& s, std::wstring_view suffix) -> bool {
        if (s.size() < suffix.size()) {
            return false;
        }
        const size_t off = s.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i) {
            wchar_t a = s[off + i];
            wchar_t b = suffix[i];
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
            if (a != b) {
                return false;
            }
        }
        return true;
    };

    if (ends_with_ci(stem, L"_telemetry")) {
        stem.resize(stem.size() - 10u);
    }

    // Strip any prior "_pid<digits>" suffix so grandchild paths stay clean.
    if (stem.size() > 5u) {
        size_t pos = stem.size();
        while (pos > 0 && stem[pos - 1] >= L'0' && stem[pos - 1] <= L'9') {
            --pos;
        }
        if (pos < stem.size() && pos >= 4u) {
            const std::wstring prefix_check = stem.substr(0, pos);
            if (ends_with_ci(prefix_check, L"_pid")) {
                stem.resize(pos - 4u);
            }
        }
    }

    if (stem.empty()) {
        stem = L"child";
    }

    const std::wstring filename =
        stem + L"_pid" + std::to_wstring(child_pid) + L"_telemetry.json";
    if (parent_p.empty()) {
        return filename;
    }
    return (parent_p / filename).wstring();
}

std::string FormatChildTelemetryPathUtf8(std::string_view base_telemetry_path, uint32_t child_pid) {
    return WideToUtf8(FormatChildTelemetryPathW(Utf8ToWide(base_telemetry_path), child_pid));
}

std::vector<wchar_t> MergeOpenScarlettEnvironmentBlockW(const void* caller_env,
                                                        bool caller_env_is_unicode,
                                                        std::wstring_view shim_dir,
                                                        std::wstring_view hook_dll_path,
                                                        std::wstring_view telemetry_base_path,
                                                        bool enable_child_per_pid_telemetry) {
#if !defined(_WIN32)
    (void)caller_env;
    (void)caller_env_is_unicode;
    (void)shim_dir;
    (void)hook_dll_path;
    (void)telemetry_base_path;
    (void)enable_child_per_pid_telemetry;
    return {L'\0', L'\0'};
#else
    std::vector<std::pair<std::wstring, std::wstring>> parent_entries;
    LPWCH raw_parent = ::GetEnvironmentStringsW();
    if (raw_parent != nullptr) {
        const wchar_t* cur = raw_parent;
        while (*cur != L'\0') {
            std::wstring entry(cur);
            cur += entry.size() + 1;
            const size_t eq = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (eq == std::wstring::npos) {
                continue;
            }
            parent_entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
        ::FreeEnvironmentStringsW(raw_parent);
    }

    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (caller_env == nullptr) {
        entries = parent_entries;
    } else if (caller_env_is_unicode) {
        const auto* cur = static_cast<const wchar_t*>(caller_env);
        while (*cur != L'\0') {
            std::wstring entry(cur);
            cur += entry.size() + 1;
            const size_t eq = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (eq == std::wstring::npos) {
                continue;
            }
            entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
    } else {
        const auto* cur = static_cast<const char*>(caller_env);
        while (*cur != '\0') {
            std::string entry_a(cur);
            cur += entry_a.size() + 1;
            std::wstring entry = AnsiStrToWideAcp(entry_a.c_str());
            if (entry.empty()) {
                continue;
            }
            const size_t eq = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (eq == std::wstring::npos) {
                continue;
            }
            entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
    }

    // Preserve/merge all OPENSCARLETT_* variables from parent into the child environment.
    if (caller_env != nullptr) {
        for (const auto& pkv : parent_entries) {
            if (StartsWithIgnoreCaseAsciiW(pkv.first, L"OPENSCARLETT_") &&
                !HasEnvKeyIgnoreCase(entries, pkv.first)) {
                entries.push_back(pkv);
            }
        }
    }

    // Ensure shim_dir is prepended to PATH (without duplicating if already at the front).
    if (!shim_dir.empty()) {
        const std::wstring shim_str(shim_dir);
        bool found_path = false;
        for (auto& kv : entries) {
            if (::_wcsicmp(kv.first.c_str(), L"PATH") == 0) {
                found_path = true;
                if (kv.second.empty()) {
                    kv.second = shim_str;
                } else {
                    const size_t first_semi = kv.second.find(L';');
                    const std::wstring first_seg =
                        (first_semi == std::wstring::npos) ? kv.second : kv.second.substr(0, first_semi);
                    if (::_wcsicmp(first_seg.c_str(), shim_str.c_str()) != 0) {
                        kv.second = shim_str + L";" + kv.second;
                    }
                }
                break;
            }
        }
        if (!found_path) {
            entries.emplace_back(L"PATH", shim_str);
        }
        if (!HasEnvKeyIgnoreCase(entries, L"OPENSCARLETT_SHIM_DIR")) {
            UpsertEnvEntry(entries, L"OPENSCARLETT_SHIM_DIR", shim_str);
        }
    }

    if (!hook_dll_path.empty() && !HasEnvKeyIgnoreCase(entries, L"OPENSCARLETT_HOOK_DLL")) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_HOOK_DLL", std::wstring(hook_dll_path));
    }

    if (enable_child_per_pid_telemetry && !telemetry_base_path.empty()) {
        const std::wstring base_str(telemetry_base_path);
        UpsertEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_BASE", base_str);
        if (!HasEnvKeyIgnoreCase(entries, L"OPENSCARLETT_TELEMETRY_PATH")) {
            UpsertEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_PATH", base_str);
        }
        UpsertEnvEntry(entries, L"OPENSCARLETT_CHILD_TELEMETRY_PER_PID", L"1");
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::pair<std::wstring, std::wstring>& a,
                 const std::pair<std::wstring, std::wstring>& b) {
                  return ::_wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
              });

    std::vector<wchar_t> block;
    block.reserve(4096);
    for (const auto& kv : entries) {
        block.insert(block.end(), kv.first.begin(), kv.first.end());
        block.push_back(L'=');
        block.insert(block.end(), kv.second.begin(), kv.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1u) {
        block.push_back(L'\0');
    }
    return block;
#endif
}

bool RedirectSuspendedProcessStaticXboxImports(void* process_handle,
                                               std::wstring_view shim_dir,
                                               std::wstring_view guest_dir) {
#if !defined(_WIN32)
    (void)process_handle;
    (void)shim_dir;
    (void)guest_dir;
    return false;
#else
    if (!process_handle) {
        return false;
    }
    return RedirectChildStaticXboxImports(
        static_cast<HANDLE>(process_handle), std::wstring(shim_dir), std::wstring(guest_dir));
#endif
}

Status InjectHookDllIntoSuspendedProcess(void* process_handle,
                                         void* thread_handle,
                                         std::wstring_view hook_dll_path,
                                         std::wstring_view shim_dir,
                                         std::wstring_view guest_dir,
                                         uint32_t* out_win32_error) {
    if (out_win32_error) {
        *out_win32_error = 0u;
    }
#if !defined(_WIN32)
    (void)process_handle;
    (void)thread_handle;
    (void)hook_dll_path;
    (void)shim_dir;
    (void)guest_dir;
    return Status::Error(StatusCode::NotImplemented, "APC hook injection is Windows-only");
#else
    if (!process_handle || !thread_handle || hook_dll_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid process/thread handle or hook DLL path");
    }

    HANDLE h_process = static_cast<HANDLE>(process_handle);
    HANDLE h_thread = static_cast<HANDLE>(thread_handle);

    DWORD arch_err = ERROR_SUCCESS;
    if (!IsTargetProcessCompatibleX64(h_process, arch_err)) {
        if (out_win32_error) {
            *out_win32_error = arch_err;
        }
        if (arch_err != ERROR_SUCCESS) {
            return Status::Error(StatusCode::DeviceError, FormatWin32Error("IsWow64Process", arch_err));
        }
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Child process architecture does not match x64 openscarlett_hook.dll");
    }

    // Best-effort static PE Xbox import redirection before LdrInitializeThunk runs.
    if (!shim_dir.empty()) {
        (void)RedirectChildStaticXboxImports(
            h_process, std::wstring(shim_dir), std::wstring(guest_dir));
    }

    HMODULE h_kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    FARPROC p_load_library_w = h_kernel32 ? ::GetProcAddress(h_kernel32, "LoadLibraryW") : nullptr;
    if (!p_load_library_w) {
        const DWORD err = ::GetLastError();
        if (out_win32_error) *out_win32_error = err;
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("GetProcAddress(LoadLibraryW)", err));
    }

    const std::wstring hook_w(hook_dll_path);
    const size_t path_bytes = (hook_w.size() + 1u) * sizeof(wchar_t);
    LPVOID remote_path = ::VirtualAllocEx(h_process,
                                          nullptr,
                                          path_bytes,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!remote_path) {
        const DWORD err = ::GetLastError();
        if (out_win32_error) *out_win32_error = err;
        return Status::Error(StatusCode::OutOfMemory, FormatWin32Error("VirtualAllocEx", err));
    }

    SIZE_T written = 0;
    if (!::WriteProcessMemory(h_process, remote_path, hook_w.c_str(), path_bytes, &written) ||
        written != path_bytes) {
        const DWORD err = ::GetLastError();
        if (out_win32_error) *out_win32_error = err;
        ::VirtualFreeEx(h_process, remote_path, 0, MEM_RELEASE);
        return Status::Error(StatusCode::IoError, FormatWin32Error("WriteProcessMemory", err));
    }

    if (::QueueUserAPC(reinterpret_cast<PAPCFUNC>(p_load_library_w),
                       h_thread,
                       reinterpret_cast<ULONG_PTR>(remote_path)) == 0) {
        const DWORD err = ::GetLastError();
        if (out_win32_error) *out_win32_error = err;
        ::VirtualFreeEx(h_process, remote_path, 0, MEM_RELEASE);
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("QueueUserAPC", err));
    }

    return Status::Ok();
#endif
}

Status LaunchWithApcHook(const LaunchOptions& options, LaunchResult& out_result) {
    out_result = {};

#if !defined(_WIN32)
    (void)options;
    return Status::Error(StatusCode::NotImplemented, "APC hook loader is Windows-only");
#else
    if (options.exe_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Target executable or package path is empty");
    }

    // Resolve package directory, MicrosoftGame.Config/.mgc, or direct .exe (with sibling config auto-detect).
    ResolvedPackageTarget pkg_target{};
    auto resolve_st = ResolvePackageLaunchTarget(options.exe_path, pkg_target);
    if (!resolve_st.ok()) {
        return resolve_st;
    }

    const std::wstring exe_w = ResolveAbsolutePathW(Utf8ToWide(pkg_target.exe_path));
    if (!FileExistsW(exe_w)) {
        return Status::Error(StatusCode::NotFound, "Target executable not found: " + WideToUtf8(exe_w));
    }

    const std::wstring exe_dir_w = std::filesystem::path(exe_w).parent_path().wstring();
    const std::wstring pkg_root_w = ResolveAbsolutePathW(Utf8ToWide(pkg_target.package_root));

    std::wstring shim_dir_w = options.shim_dir.empty()
                                  ? GetCurrentExecutableDirW()
                                  : ResolveAbsolutePathW(Utf8ToWide(options.shim_dir));
    if (shim_dir_w.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Unable to determine OpenScarlett shim directory");
    }

    std::wstring hook_dll_w = options.hook_dll_path.empty()
                                  ? (std::filesystem::path(shim_dir_w) / L"openscarlett_hook.dll").wstring()
                                  : ResolveAbsolutePathW(Utf8ToWide(options.hook_dll_path));
    if (!FileExistsW(hook_dll_w)) {
        return Status::Error(StatusCode::NotFound,
                             "openscarlett_hook.dll not found: " + WideToUtf8(hook_dll_w));
    }

    std::string resolved_title_id = options.title_id;
    std::string resolved_package_id = options.package_id;
    std::string title_folder;
    if (pkg_target.has_config) {
        if (resolved_title_id.empty()) {
            resolved_title_id = pkg_target.config.title_id;
        }
        if (resolved_package_id.empty()) {
            resolved_package_id = pkg_target.config.identity.name;
        }
        if (options.title_id.empty()) {
            title_folder = FormatTitleIdFolderName(pkg_target.config);
        }
    }
    if (title_folder.empty() && !resolved_title_id.empty()) {
        title_folder = SanitizeFolderSegment(resolved_title_id);
    }

    // Build default VFS mounts anchored to OPENSCARLETT_WORK_DIR / shim_dir (never C:\ when writable)
    // without modifying the guest directory.
    VfsMountConfig defaults =
        VfsMounts::BuildDefaultConfig(pkg_root_w, ResolveDefaultVfsRootW(shim_dir_w));
    std::wstring vfs_g_w;
    if (!options.vfs_g.empty()) {
        vfs_g_w = ResolveAbsolutePathW(Utf8ToWide(options.vfs_g));
    } else if (pkg_target.has_config && !HasEnvVarW(L"OPENSCARLETT_VFS_G")) {
        vfs_g_w = pkg_root_w;
    } else {
        vfs_g_w = defaults.g_root;
    }

    std::wstring vfs_t_w = options.vfs_t.empty() ? defaults.t_root : ResolveAbsolutePathW(Utf8ToWide(options.vfs_t));

    std::wstring vfs_s_w;
    if (!options.vfs_s.empty()) {
        vfs_s_w = ResolveAbsolutePathW(Utf8ToWide(options.vfs_s));
    } else {
        vfs_s_w = defaults.s_root;
        if (!HasEnvVarW(L"OPENSCARLETT_VFS_S") && !title_folder.empty() && !vfs_s_w.empty()) {
            vfs_s_w = (std::filesystem::path(vfs_s_w) / Utf8ToWide(title_folder)).wstring();
        }
    }

    std::wstring vfs_g_overlay_w;
    if (!options.vfs_g_overlay.empty()) {
        vfs_g_overlay_w = ResolveAbsolutePathW(Utf8ToWide(options.vfs_g_overlay));
    } else {
        vfs_g_overlay_w = defaults.g_overlay_root;
        if (!HasEnvVarW(L"OPENSCARLETT_VFS_G_OVERLAY") && !title_folder.empty() && !vfs_g_overlay_w.empty()) {
            vfs_g_overlay_w = (std::filesystem::path(vfs_g_overlay_w) / Utf8ToWide(title_folder)).wstring();
        }
    }

    std::wstring work_dir_w = options.working_dir.empty()
                                  ? exe_dir_w
                                  : ResolveAbsolutePathW(Utf8ToWide(options.working_dir));

    std::wstring telemetry_w = options.telemetry_output_path.empty()
                                   ? std::wstring{}
                                   : ResolveAbsolutePathW(Utf8ToWide(options.telemetry_output_path));

    // Pre-create writable backing directories on E:\ (NEVER touches vfs_g / target package directory).
    if (!vfs_t_w.empty()) {
        EnsureDirectoryTreeW(vfs_t_w);
    }
    if (!vfs_s_w.empty()) {
        EnsureDirectoryTreeW(vfs_s_w);
    }
    if (!vfs_g_overlay_w.empty()) {
        EnsureDirectoryTreeW(vfs_g_overlay_w);
    }
    if (!telemetry_w.empty()) {
        std::wstring tel_parent = std::filesystem::path(telemetry_w).parent_path().wstring();
        if (!tel_parent.empty()) {
            EnsureDirectoryTreeW(tel_parent);
        }
    }

    out_result.resolved_exe_path = WideToUtf8(exe_w);
    out_result.resolved_config_path = pkg_target.config_path;
    out_result.resolved_shim_dir = WideToUtf8(shim_dir_w);
    out_result.resolved_hook_dll = WideToUtf8(hook_dll_w);
    out_result.resolved_vfs_g = WideToUtf8(vfs_g_w);
    out_result.resolved_vfs_g_overlay = WideToUtf8(vfs_g_overlay_w);
    out_result.resolved_vfs_t = WideToUtf8(vfs_t_w);
    out_result.resolved_vfs_s = WideToUtf8(vfs_s_w);
    out_result.resolved_title_id = resolved_title_id;
    out_result.resolved_package_id = resolved_package_id;
    out_result.resolved_telemetry_path = WideToUtf8(telemetry_w);

    // Build mutable command line: "<exe>" [arguments]
    std::wstring cmd_line = L"\"" + exe_w + L"\"";
    if (!options.arguments.empty()) {
        cmd_line.push_back(L' ');
        cmd_line.append(Utf8ToWide(options.arguments));
    }

    std::vector<wchar_t> env_block = BuildChildEnvironmentBlock(
        shim_dir_w,
        hook_dll_w,
        vfs_g_w,
        vfs_g_overlay_w,
        vfs_t_w,
        vfs_s_w,
        Utf8ToWide(resolved_title_id),
        Utf8ToWide(resolved_package_id),
        telemetry_w);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD creation_flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (!options.inherit_console) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    if (!::CreateProcessW(exe_w.c_str(),
                          cmd_line.data(),
                          nullptr,
                          nullptr,
                          options.inherit_console ? TRUE : FALSE,
                          creation_flags,
                          env_block.data(),
                          work_dir_w.empty() ? nullptr : work_dir_w.c_str(),
                          &si,
                          &pi)) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("CreateProcessW", ::GetLastError()));
    }

    out_result.process_id = static_cast<uint32_t>(pi.dwProcessId);

    auto cleanup_probe_stubs = [&]() {
        if (!IsProbeUnknownImportsEnvEnabled()) {
            return;
        }
        const std::wstring session_dir_w = GetProbeStubSessionDirW(shim_dir_w, pi.dwProcessId);
        if (!session_dir_w.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(session_dir_w), ec);
        }
    };

    auto abort_child = [&](StatusCode code, const char* step, DWORD err) -> Status {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        cleanup_probe_stubs();
        return Status::Error(code, FormatWin32Error(step, err));
    };

    uint32_t inject_err = 0;
    Status inject_st = InjectHookDllIntoSuspendedProcess(
        pi.hProcess, pi.hThread, hook_dll_w, shim_dir_w, vfs_g_w, &inject_err);
    if (!inject_st.ok()) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        cleanup_probe_stubs();
        return inject_st;
    }

    if (::ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        return abort_child(StatusCode::DeviceError, "ResumeThread", ::GetLastError());
    }

    const DWORD wait_ms = (options.timeout_ms == 0) ? INFINITE : static_cast<DWORD>(options.timeout_ms);
    const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        out_result.timed_out = true;
        out_result.exit_code = 124;
        ::TerminateProcess(pi.hProcess, 124);
        ::WaitForSingleObject(pi.hProcess, 3000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        cleanup_probe_stubs();
        return Status::Error(StatusCode::DeviceError,
                             "Guest process timed out after " + std::to_string(options.timeout_ms) + " ms");
    }
    if (wait_rc != WAIT_OBJECT_0) {
        DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        cleanup_probe_stubs();
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("WaitForSingleObject", err));
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        cleanup_probe_stubs();
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("GetExitCodeProcess", err));
    }

    out_result.exit_code = static_cast<uint32_t>(exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    cleanup_probe_stubs();
    return Status::Ok();
#endif
}

StoreBootstrapDiagnostic DiagnoseStoreBootstrapper(const LaunchResult& result,
                                                   size_t tracked_telemetry_symbols,
                                                   bool telemetry_file_written) {
    StoreBootstrapDiagnostic diag{};

    std::string exe_lower = result.resolved_exe_path;
    for (char& c : exe_lower) {
        if (c == '/') {
            c = '\\';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    diag.in_windows_apps = (exe_lower.find("\\windowsapps\\") != std::string::npos);
    diag.in_xbox_games = (exe_lower.find("\\xboxgames\\") != std::string::npos);

    std::string filename_lo;
    {
        const size_t slash = exe_lower.find_last_of('\\');
        filename_lo = (slash == std::string::npos) ? exe_lower : exe_lower.substr(slash + 1);
    }
    diag.is_gamelaunchhelper = (filename_lo == "gamelaunchhelper.exe");

    diag.has_game_config = !result.resolved_config_path.empty();
    if (!diag.has_game_config && !result.resolved_exe_path.empty()) {
        std::error_code ec;
        const std::filesystem::path exe_dir = std::filesystem::path(result.resolved_exe_path).parent_path();
        if (!exe_dir.empty() && std::filesystem::exists(exe_dir, ec)) {
            if (std::filesystem::exists(exe_dir / "MicrosoftGame.config", ec) ||
                std::filesystem::exists(exe_dir / "MicrosoftGame.Config", ec) ||
                std::filesystem::exists(exe_dir / "MicrosoftGameConfig.mgc", ec)) {
                diag.has_game_config = true;
            }
        }
    }

    // Only flag when the process exited cleanly (code 0, not timed out) with 0 tracked symbols.
    if (result.exit_code != 0 || result.timed_out || tracked_telemetry_symbols > 0) {
        return diag;
    }

    diag.suspected_store_bootstrapper = true;

    std::vector<std::string> signals;
    if (diag.is_gamelaunchhelper) {
        signals.emplace_back("gamelaunchhelper.exe bootstrapper");
    }
    if (diag.in_windows_apps) {
        signals.emplace_back("WindowsApps package path");
    }
    if (diag.in_xbox_games) {
        signals.emplace_back("XboxGames package path");
    }
    if (diag.has_game_config) {
        signals.emplace_back("MicrosoftGame.config present");
    }
    if (!telemetry_file_written) {
        signals.emplace_back("no hook telemetry report written");
    } else {
        signals.emplace_back("0 tracked hook/shim symbols");
    }

    for (size_t i = 0; i < signals.size(); ++i) {
        if (i > 0) diag.reason += ", ";
        diag.reason += signals[i];
    }

    diag.message =
        "Target exited with code 0 and 0 tracked OpenScarlett hook/shim symbols (" + diag.reason + ").\n"
        "               Microsoft Store / Xbox Play Anywhere bootstrapper executables (such as\n"
        "               gamelaunchhelper.exe or AppModel launchers under WindowsApps / XboxGames)\n"
        "               delegate launch to Windows GamingServices and exit immediately with code 0\n"
        "               after spawning a detached AppModel child process.\n"
        "               Supply an unpacked package directory with loose files to run directly.";
    return diag;
}

}  // namespace openscarlett
