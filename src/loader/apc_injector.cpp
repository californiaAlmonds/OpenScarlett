#include "openscarlett/loader/apc_injector.h"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <filesystem>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/loader/game_config.h"
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

// Redirect static PE imports of routed DLLs in the suspended child process BEFORE
// LdrInitializeThunk runs using OpenScarlett's unified data-driven DllRoutingTable.
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
    if (imp_dir.VirtualAddress == 0 || imp_dir.VirtualAddress >= image_size) {
        return true;
    }

    const DllRoutingTable routing = DllRoutingTable::CreateWithEnvOverrides();

    std::vector<IMAGE_IMPORT_DESCRIPTOR> descs;
    std::vector<std::string> imported_names;
    bool has_console_exclusive_import = false;

    const uintptr_t imp_table_addr = image_base + imp_dir.VirtualAddress;
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

    std::vector<std::string> full_paths;
    std::vector<int> priorities;
    full_paths.reserve(descs.size());
    priorities.reserve(descs.size());
    bool any_redirected = false;

    for (size_t idx = 0; idx < descs.size(); ++idx) {
        const std::string& dll_name = imported_names[idx];
        if (!dll_name.empty()) {
            const DllRouteDecision decision =
                routing.ResolveRoute(dll_name, shim_dir_w, guest_dir_w, has_console_exclusive_import);
            const bool should_rewrite =
                !decision.resolved_path.empty() &&
                (decision.policy == DllRoutePolicy::Shim ||
                 (decision.policy == DllRoutePolicy::NativeGuestDll && decision.from_override));
            if (should_rewrite) {
                full_paths.push_back(WideToUtf8(decision.resolved_path));
                priorities.push_back(decision.load_priority);
                any_redirected = true;
            } else {
                full_paths.emplace_back();
                priorities.push_back(10);
            }
        } else {
            full_paths.emplace_back();
            priorities.push_back(10);
        }
    }

    if (!any_redirected || descs.empty()) {
        return true;
    }

    // Pack redirected full ASCII DLL paths into a contiguous buffer allocated within +2 GiB of image_base
    // so every replacement Name RVA is a valid positive 32-bit offset from image_base.
    std::vector<char> str_blob;
    std::vector<uint32_t> str_offsets(descs.size(), 0u);
    for (size_t i = 0; i < descs.size(); ++i) {
        if (!full_paths[i].empty()) {
            str_offsets[i] = static_cast<uint32_t>(str_blob.size());
            str_blob.insert(str_blob.end(), full_paths[i].begin(), full_paths[i].end());
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
        std::string orig_name;
        if (descs[i].Name != 0 && descs[i].Name < image_size) {
            char name_buf[260]{};
            const size_t max_read = std::min<size_t>(sizeof(name_buf) - 1, image_size - descs[i].Name);
            if (::ReadProcessMemory(h_process,
                                    reinterpret_cast<LPCVOID>(image_base + descs[i].Name),
                                    name_buf, max_read, &rw) &&
                rw > 0) {
                name_buf[rw] = '\0';
                orig_name = name_buf;
            }
        }
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
    const bool wrote_ok =
        ::WriteProcessMemory(h_process, reinterpret_cast<LPVOID>(imp_table_addr),
                             sorted_descs.data(), table_bytes, &rw) != 0 &&
        rw == table_bytes;
    DWORD dummy_prot = 0;
    ::VirtualProtectEx(h_process, reinterpret_cast<LPVOID>(imp_table_addr),
                       table_bytes, old_prot, &dummy_prot);
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

    auto abort_child = [&](StatusCode code, const char* step, DWORD err) -> Status {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
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
        return Status::Error(StatusCode::DeviceError,
                             "Guest process timed out after " + std::to_string(options.timeout_ms) + " ms");
    }
    if (wait_rc != WAIT_OBJECT_0) {
        DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("WaitForSingleObject", err));
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("GetExitCodeProcess", err));
    }

    out_result.exit_code = static_cast<uint32_t>(exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
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
