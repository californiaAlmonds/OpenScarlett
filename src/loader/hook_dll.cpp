#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/kernel/console_lsa_policy.h"
#include "openscarlett/loader/apc_injector.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/probe_veh.h"
#include "openscarlett/loader/vfs_mounts.h"

namespace {

// ---------------------------------------------------------------------------
// Self-Contained NT File API ABI Definitions (no <winternl.h> collisions)
// ---------------------------------------------------------------------------
using OS_NTSTATUS = LONG;
constexpr OS_NTSTATUS OS_STATUS_SUCCESS = 0x00000000L;
constexpr OS_NTSTATUS OS_STATUS_ACCESS_DENIED = static_cast<OS_NTSTATUS>(0xC0000022UL);
constexpr OS_NTSTATUS OS_STATUS_OBJECT_NAME_NOT_FOUND = static_cast<OS_NTSTATUS>(0xC0000034UL);
constexpr OS_NTSTATUS OS_STATUS_OBJECT_PATH_NOT_FOUND = static_cast<OS_NTSTATUS>(0xC000003AUL);

constexpr ULONG OS_FILE_SUPERSEDE = 0x00000000UL;
constexpr ULONG OS_FILE_OPEN = 0x00000001UL;
constexpr ULONG OS_FILE_CREATE = 0x00000002UL;
constexpr ULONG OS_FILE_OPEN_IF = 0x00000003UL;
constexpr ULONG OS_FILE_OVERWRITE = 0x00000004UL;
constexpr ULONG OS_FILE_OVERWRITE_IF = 0x00000005UL;

struct OS_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct OS_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    OS_UNICODE_STRING* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct OS_IO_STATUS_BLOCK {
    union {
        OS_NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

struct OS_FILE_BASIC_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
};

struct OpenScarlettTlsSlot0Status {
    uint32_t initialized;
    uint32_t owned_by_hook;
    uint32_t already_claimed_by_host;
    uint32_t preserved_ok;
    uintptr_t saved_value_at_attach;
};

using PfnNtCreateFile = OS_NTSTATUS(NTAPI*)(PHANDLE,
                                            ACCESS_MASK,
                                            OS_OBJECT_ATTRIBUTES*,
                                            OS_IO_STATUS_BLOCK*,
                                            PLARGE_INTEGER,
                                            ULONG,
                                            ULONG,
                                            ULONG,
                                            ULONG,
                                            PVOID,
                                            ULONG);
using PfnNtOpenFile = OS_NTSTATUS(NTAPI*)(PHANDLE,
                                          ACCESS_MASK,
                                          OS_OBJECT_ATTRIBUTES*,
                                          OS_IO_STATUS_BLOCK*,
                                          ULONG,
                                          ULONG);
using PfnNtQueryAttributesFile = OS_NTSTATUS(NTAPI*)(OS_OBJECT_ATTRIBUTES*,
                                                     OS_FILE_BASIC_INFORMATION*);

// ---------------------------------------------------------------------------
// Real Win32 API Function Pointer Types
// ---------------------------------------------------------------------------
using PfnLoadLibraryA = HMODULE(WINAPI*)(LPCSTR);
using PfnLoadLibraryW = HMODULE(WINAPI*)(LPCWSTR);
using PfnLoadLibraryExA = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using PfnLoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using PfnGetModuleHandleA = HMODULE(WINAPI*)(LPCSTR);
using PfnGetModuleHandleW = HMODULE(WINAPI*)(LPCWSTR);
using PfnGetModuleHandleExA = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);
using PfnGetModuleHandleExW = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using PfnGetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using PfnCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PfnCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PfnGetFileAttributesA = DWORD(WINAPI*)(LPCSTR);
using PfnGetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using PfnGetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PfnCreateDirectoryW = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PfnDeleteFileA = BOOL(WINAPI*)(LPCSTR);
using PfnDeleteFileW = BOOL(WINAPI*)(LPCWSTR);
using PfnMoveFileExW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using PfnRemoveDirectoryW = BOOL(WINAPI*)(LPCWSTR);
using PfnFindFirstFileW = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using PfnCreateProcessA = BOOL(WINAPI*)(LPCSTR,
                                        LPSTR,
                                        LPSECURITY_ATTRIBUTES,
                                        LPSECURITY_ATTRIBUTES,
                                        BOOL,
                                        DWORD,
                                        LPVOID,
                                        LPCSTR,
                                        LPSTARTUPINFOA,
                                        LPPROCESS_INFORMATION);
using PfnCreateProcessW = BOOL(WINAPI*)(LPCWSTR,
                                        LPWSTR,
                                        LPSECURITY_ATTRIBUTES,
                                        LPSECURITY_ATTRIBUTES,
                                        BOOL,
                                        DWORD,
                                        LPVOID,
                                        LPCWSTR,
                                        LPSTARTUPINFOW,
                                        LPPROCESS_INFORMATION);
using PfnExitProcess = VOID(WINAPI*)(UINT);
using PfnOutputDebugStringA = VOID(WINAPI*)(LPCSTR);

PfnLoadLibraryA g_real_LoadLibraryA = nullptr;
PfnLoadLibraryW g_real_LoadLibraryW = nullptr;
PfnLoadLibraryExA g_real_LoadLibraryExA = nullptr;
PfnLoadLibraryExW g_real_LoadLibraryExW = nullptr;
PfnGetModuleHandleA g_real_GetModuleHandleA = nullptr;
PfnGetModuleHandleW g_real_GetModuleHandleW = nullptr;
PfnGetModuleHandleExA g_real_GetModuleHandleExA = nullptr;
PfnGetModuleHandleExW g_real_GetModuleHandleExW = nullptr;
PfnGetProcAddress g_real_GetProcAddress = nullptr;
PfnCreateFileA g_real_CreateFileA = nullptr;
PfnCreateFileW g_real_CreateFileW = nullptr;
PfnGetFileAttributesA g_real_GetFileAttributesA = nullptr;
PfnGetFileAttributesW g_real_GetFileAttributesW = nullptr;
PfnGetFileAttributesExW g_real_GetFileAttributesExW = nullptr;
PfnCreateDirectoryW g_real_CreateDirectoryW = nullptr;
PfnDeleteFileA g_real_DeleteFileA = nullptr;
PfnDeleteFileW g_real_DeleteFileW = nullptr;
PfnMoveFileExW g_real_MoveFileExW = nullptr;
PfnRemoveDirectoryW g_real_RemoveDirectoryW = nullptr;
PfnFindFirstFileW g_real_FindFirstFileW = nullptr;
PfnCreateProcessA g_real_CreateProcessA = nullptr;
PfnCreateProcessW g_real_CreateProcessW = nullptr;
PfnCreateProcessA g_kernelbase_CreateProcessA = nullptr;
PfnCreateProcessW g_kernelbase_CreateProcessW = nullptr;
PfnExitProcess g_real_ExitProcess = nullptr;
PfnOutputDebugStringA g_real_OutputDebugStringA = nullptr;
bool g_trace_ods = false;
PfnNtCreateFile g_real_NtCreateFile = nullptr;
PfnNtOpenFile g_real_NtOpenFile = nullptr;
PfnNtQueryAttributesFile g_real_NtQueryAttributesFile = nullptr;

// ---------------------------------------------------------------------------
// Hook Runtime State
// ---------------------------------------------------------------------------
HMODULE g_self_module = nullptr;
HMODULE g_kernel32_mod = nullptr;
HMODULE g_kernelbase_mod = nullptr;
HMODULE g_ntdll_mod = nullptr;
std::wstring g_shim_dir_w;
std::wstring g_hook_dll_path_w;
std::wstring g_guest_dir_w;
std::wstring g_windows_dir_lower_w;
std::wstring g_telemetry_base_w;
std::string g_telemetry_path_utf8;
openscarlett::VfsMounts g_vfs;
openscarlett::DllRoutingTable g_routing_table;
openscarlett::PeForwarderResolver g_pe_forwarder;
openscarlett::MissingImportTrampolinePool g_probe_trampoline_pool;
openscarlett::VehCrashMonitor g_veh_monitor;
bool g_probe_unknown_imports = false;

std::atomic<bool> g_tls_slot0_initialized{false};
std::atomic<bool> g_tls_slot0_owned_by_hook{false};
std::atomic<bool> g_tls_slot0_already_claimed{false};
std::atomic<bool> g_tls_slot0_preserved_ok{false};
std::atomic<uintptr_t> g_tls_slot0_saved_val{0u};

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_flushed_on_exit{false};

SRWLOCK g_state_lock = SRWLOCK_INIT;
SRWLOCK g_patch_lock = SRWLOCK_INIT;
std::unordered_map<HMODULE, std::string> g_tracked_xbox_modules;
std::unordered_set<HMODULE> g_non_xbox_modules;

struct PatchedModuleKey {
    HMODULE base = nullptr;
    uint32_t size_of_image = 0;
    uint32_t timestamp = 0;
};
std::unordered_map<HMODULE, PatchedModuleKey> g_patched_modules;

thread_local uint32_t tl_forwarder_depth = 0;
thread_local bool tl_in_topo_preload = false;
thread_local bool tl_in_create_process_hook = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::wstring ReadEnvW(const wchar_t* name) {
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring buf(needed, L'\0');
    DWORD written = ::GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
}

std::wstring AnsiToWide(LPCSTR text) {
    if (!text || *text == '\0') {
        return {};
    }
    int len = ::MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (len <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), len);
    return out;
}

std::wstring ToLowerAsciiW(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return out;
}

std::string ToLowerAsciiA(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::wstring GetModulePathW(HMODULE mod) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

bool MatchRoutedShimOrGuestDllW(std::wstring_view raw_name,
                                openscarlett::DllRouteDecision& out_decision,
                                std::wstring& out_target_path) {
    out_decision = {};
    out_target_path.clear();
    if (raw_name.empty()) {
        return false;
    }

    const bool has_path_sep = (raw_name.find_first_of(L"\\/") != std::wstring_view::npos);
    out_decision = g_routing_table.ResolveRouteW(raw_name, g_shim_dir_w, g_guest_dir_w);

    if (out_decision.policy == openscarlett::DllRoutePolicy::Shim) {
        if (!out_decision.resolved_path.empty()) {
            out_target_path = out_decision.resolved_path;
        } else {
            const std::wstring canon_w = openscarlett::Utf8ToWide(out_decision.canonical_name);
            out_target_path = g_shim_dir_w.empty() ? canon_w : (g_shim_dir_w + L"\\" + canon_w);
        }
        return true;
    }

    // Fall back to PeForwarderResolver for mixed-case DLLs & API-set contracts (api-ms-win-*/ext-ms-win-*).
    {
        const std::string raw_u8 = openscarlett::WideToUtf8(raw_name);
        std::string canonical_dll;
        openscarlett::ForwarderResolutionKind fwd_kind = openscarlett::ForwarderResolutionKind::Unresolved;
        bool via_api_set = false;
        bool via_shim_alias = false;
        if (g_pe_forwarder.ResolveTargetDll(raw_u8, canonical_dll, fwd_kind, via_api_set, via_shim_alias)) {
            if (fwd_kind == openscarlett::ForwarderResolutionKind::OpenScarlettShim) {
                out_decision = g_routing_table.ResolveRoute(canonical_dll, g_shim_dir_w, g_guest_dir_w);
                out_decision.policy = openscarlett::DllRoutePolicy::Shim;
                out_decision.requested_name = openscarlett::NormalizeForwarderDllName(raw_u8, true);
                out_decision.canonical_name = canonical_dll;
                out_decision.aliased = via_api_set || via_shim_alias ||
                                       (ToLowerAsciiA(out_decision.requested_name) != ToLowerAsciiA(canonical_dll));
                if (!out_decision.resolved_path.empty()) {
                    out_target_path = out_decision.resolved_path;
                } else {
                    const std::wstring canon_w = openscarlett::Utf8ToWide(canonical_dll);
                    out_target_path = g_shim_dir_w.empty() ? canon_w : (g_shim_dir_w + L"\\" + canon_w);
                }
                return true;
            }
        }
    }

    if (out_decision.policy == openscarlett::DllRoutePolicy::NativeGuestDll &&
        !out_decision.resolved_path.empty() &&
        (!has_path_sep || out_decision.from_override)) {
        out_target_path = out_decision.resolved_path;
        return true;
    }

    return false;
}

bool MatchTrackedShimNameW(std::wstring_view path_or_name, std::string& out_canonical_a) {
    const auto* rule = g_routing_table.FindRule(openscarlett::WideToUtf8(path_or_name));
    if (rule && rule->policy == openscarlett::DllRoutePolicy::Shim) {
        out_canonical_a = rule->canonical_name;
        return true;
    }
    return false;
}

bool IsWindowsSystemModule(HMODULE mod) {
    if (!mod || mod == g_self_module) {
        return true;
    }
    std::wstring path = GetModulePathW(mod);
    if (path.empty()) {
        return true;
    }
    std::wstring lower = ToLowerAsciiW(path);
    for (wchar_t& c : lower) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (!g_windows_dir_lower_w.empty()) {
        const std::wstring sys32 = g_windows_dir_lower_w + L"\\system32\\";
        const std::wstring syswow = g_windows_dir_lower_w + L"\\syswow64\\";
        const std::wstring winsxs = g_windows_dir_lower_w + L"\\winsxs\\";
        if (lower.rfind(sys32, 0) == 0 ||
            lower.rfind(syswow, 0) == 0 ||
            lower.rfind(winsxs, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool IsHookableImportDll(const char* dll_name) {
    if (!dll_name || *dll_name == '\0') {
        return false;
    }
    const std::string lo = openscarlett::NormalizeForwarderDllName(dll_name, true);
    if (lo == "kernel32.dll" ||
        lo == "kernelbase.dll" ||
        lo == "ntdll.dll" ||
        lo.rfind("api-ms-win-core-", 0) == 0) {
        return true;
    }
    if (openscarlett::IsApiSetContractName(lo)) {
        std::string target_dll;
        if (g_pe_forwarder.ResolveApiSetContract(lo, target_dll)) {
            const std::string tlo = ToLowerAsciiA(target_dll);
            return tlo == "kernel32.dll" || tlo == "kernelbase.dll" || tlo == "ntdll.dll";
        }
    }
    return false;
}

void RegisterXboxModule(HMODULE mod, const std::string& canonical_name) {
    if (!mod) {
        return;
    }
    ::AcquireSRWLockExclusive(&g_state_lock);
    g_tracked_xbox_modules[mod] = canonical_name;
    g_non_xbox_modules.erase(mod);
    ::ReleaseSRWLockExclusive(&g_state_lock);
}

bool LookupXboxModuleName(HMODULE mod, std::string& out_name) {
    if (!mod) {
        return false;
    }
    ::AcquireSRWLockShared(&g_state_lock);
    auto it = g_tracked_xbox_modules.find(mod);
    if (it != g_tracked_xbox_modules.end()) {
        out_name = it->second;
        ::ReleaseSRWLockShared(&g_state_lock);
        return true;
    }
    bool known_non_xbox = (g_non_xbox_modules.find(mod) != g_non_xbox_modules.end());
    ::ReleaseSRWLockShared(&g_state_lock);

    if (known_non_xbox) {
        return false;
    }

    std::wstring path = GetModulePathW(mod);
    std::string canon_a;
    if (MatchTrackedShimNameW(path, canon_a)) {
        RegisterXboxModule(mod, canon_a);
        out_name = std::move(canon_a);
        return true;
    }

    ::AcquireSRWLockExclusive(&g_state_lock);
    g_non_xbox_modules.insert(mod);
    ::ReleaseSRWLockExclusive(&g_state_lock);
    return false;
}

int FlushTelemetryInternal(bool only_once) {
    if (only_once) {
        if (g_flushed_on_exit.exchange(true, std::memory_order_acq_rel)) {
            return 1;
        }
    }
    if (g_telemetry_path_utf8.empty()) {
        return 0;
    }
    std::wstring tel_w = openscarlett::Utf8ToWide(g_telemetry_path_utf8);
    std::wstring parent_w = std::filesystem::path(tel_w).parent_path().wstring();
    if (!parent_w.empty()) {
        openscarlett::EnsureDirectoryTreeW(parent_w);
    }
    auto st = openscarlett::CallTelemetry::Global().WriteJsonReport(g_telemetry_path_utf8);
    return st.ok() ? 1 : 0;
}

// Forward declarations for recursive IAT/delay-load patching and forwarded GetProcAddress
void PatchModuleImports(HMODULE mod);
void PatchAllLoadedNonSystemModules();
FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
OS_NTSTATUS NTAPI Hook_NtCreateFile(PHANDLE FileHandle,
                                    ACCESS_MASK DesiredAccess,
                                    OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                    OS_IO_STATUS_BLOCK* IoStatusBlock,
                                    PLARGE_INTEGER AllocationSize,
                                    ULONG FileAttributes,
                                    ULONG ShareAccess,
                                    ULONG CreateDisposition,
                                    ULONG CreateOptions,
                                    PVOID EaBuffer,
                                    ULONG EaLength);
OS_NTSTATUS NTAPI Hook_NtOpenFile(PHANDLE FileHandle,
                                  ACCESS_MASK DesiredAccess,
                                  OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                  OS_IO_STATUS_BLOCK* IoStatusBlock,
                                  ULONG ShareAccess,
                                  ULONG OpenOptions);
OS_NTSTATUS NTAPI Hook_NtQueryAttributesFile(OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                             OS_FILE_BASIC_INFORMATION* FileInformation);
BOOL WINAPI Hook_CreateProcessA(LPCSTR lpApplicationName,
                                LPSTR lpCommandLine,
                                LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                BOOL bInheritHandles,
                                DWORD dwCreationFlags,
                                LPVOID lpEnvironment,
                                LPCSTR lpCurrentDirectory,
                                LPSTARTUPINFOA lpStartupInfo,
                                LPPROCESS_INFORMATION lpProcessInformation);
BOOL WINAPI Hook_CreateProcessW(LPCWSTR lpApplicationName,
                                LPWSTR lpCommandLine,
                                LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                BOOL bInheritHandles,
                                DWORD dwCreationFlags,
                                LPVOID lpEnvironment,
                                LPCWSTR lpCurrentDirectory,
                                LPSTARTUPINFOW lpStartupInfo,
                                LPPROCESS_INFORMATION lpProcessInformation);

// ---------------------------------------------------------------------------
// Topological DLL Dependency Pre-Loader (Kahn's Sort over PE Import Graph)
// ---------------------------------------------------------------------------
bool TryLocateLocalOrRoutedDllW(const std::string& import_dll_name,
                                const std::wstring& importer_dir_w,
                                std::wstring& out_path_w,
                                int& out_priority) {
    out_path_w.clear();
    out_priority = 10;
    if (import_dll_name.empty()) {
        return false;
    }

    // KnownDLLs ALWAYS resolve from System32 first — never shadow them with package-local DLLs.
    if (openscarlett::IsWindowsKnownDll(import_dll_name)) {
        return false;
    }

    const openscarlett::DllRouteDecision decision =
        g_routing_table.ResolveRoute(import_dll_name, g_shim_dir_w, g_guest_dir_w);
    out_priority = decision.load_priority;
    if ((decision.policy == openscarlett::DllRoutePolicy::Shim ||
         decision.policy == openscarlett::DllRoutePolicy::NativeGuestDll) &&
        !decision.resolved_path.empty()) {
        out_path_w = decision.resolved_path;
        return true;
    }

    const std::wstring imp_w = openscarlett::Utf8ToWide(import_dll_name);
    auto check_dir = [&](const std::wstring& dir) -> bool {
        if (dir.empty()) {
            return false;
        }
        const std::wstring cand = (std::filesystem::path(dir) / imp_w).wstring();
        const DWORD attrs = ::GetFileAttributesW(cand.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            out_path_w = cand;
            return true;
        }
        return false;
    };

    if (check_dir(importer_dir_w)) return true;
    if (check_dir(g_guest_dir_w)) return true;
    if (check_dir(g_shim_dir_w)) return true;
    return false;
}

void PreloadDllDependenciesTopologically(const std::wstring& root_dll_path) {
    if (root_dll_path.empty() || tl_in_topo_preload || !g_real_LoadLibraryW || !g_real_GetModuleHandleW) {
        return;
    }

    tl_in_topo_preload = true;
    struct Guard {
        ~Guard() { tl_in_topo_preload = false; }
    } guard;

    std::vector<openscarlett::DllDependencyNode> nodes;
    std::vector<std::wstring> node_paths_w;
    std::unordered_map<std::string, size_t> visited;

    std::vector<std::pair<std::wstring, int>> queue;
    queue.emplace_back(root_dll_path, 10);

    size_t head = 0;
    while (head < queue.size() && nodes.size() < 256u) {
        const std::wstring cur_path_w = queue[head].first;
        const int cur_prio = queue[head].second;
        ++head;

        const std::string cur_path_u8 = openscarlett::WideToUtf8(cur_path_w);
        const std::string cur_fname = std::filesystem::path(cur_path_w).filename().string();
        const std::string cur_key = ToLowerAsciiA(cur_fname);
        if (cur_key.empty() || visited.find(cur_key) != visited.end()) {
            continue;
        }

        openscarlett::PeInspectionReport rep{};
        if (!openscarlett::InspectPeFile(cur_path_u8, rep).ok()) {
            continue;
        }

        visited[cur_key] = nodes.size();
        openscarlett::DllDependencyNode node{};
        node.name = cur_fname;
        node.file_path = cur_path_u8;
        node.load_priority = cur_prio;

        const std::wstring cur_dir_w = std::filesystem::path(cur_path_w).parent_path().wstring();
        for (const auto& imp : rep.imports) {
            node.imports.push_back(imp.dll_name);
            const std::string imp_key = ToLowerAsciiA(imp.dll_name);
            if (visited.find(imp_key) != visited.end()) {
                continue;
            }
            std::wstring dep_path_w;
            int dep_prio = 10;
            if (TryLocateLocalOrRoutedDllW(imp.dll_name, cur_dir_w, dep_path_w, dep_prio)) {
                queue.emplace_back(std::move(dep_path_w), dep_prio);
            }
        }

        nodes.push_back(std::move(node));
        node_paths_w.push_back(cur_path_w);
    }

    if (nodes.size() <= 1u) {
        return;
    }

    const openscarlett::DllInitOrderResult topo = openscarlett::ComputeDllInitOrder(nodes);
    const std::string root_key = ToLowerAsciiA(std::filesystem::path(root_dll_path).filename().string());

    uint32_t preloaded_count = 0;
    for (size_t idx : topo.ordered_indices) {
        const std::string key = ToLowerAsciiA(nodes[idx].name);
        if (key == root_key) {
            continue;  // Caller loads the root DLL itself right after dependencies.
        }
        const std::wstring& dep_path = node_paths_w[idx];
        HMODULE existing = g_real_GetModuleHandleW(dep_path.c_str());
        if (!existing) {
            existing = g_real_GetModuleHandleW(openscarlett::Utf8ToWide(nodes[idx].name).c_str());
        }
        if (existing) {
            continue;
        }
        HMODULE dep_mod = g_real_LoadLibraryExW
                              ? g_real_LoadLibraryExW(dep_path.c_str(), nullptr,
                                                      LOAD_WITH_ALTERED_SEARCH_PATH)
                              : g_real_LoadLibraryW(dep_path.c_str());
        if (dep_mod) {
            ++preloaded_count;
            std::string canon_a;
            if (MatchTrackedShimNameW(dep_path, canon_a)) {
                RegisterXboxModule(dep_mod, canon_a);
            }
            PatchModuleImports(dep_mod);
        }
    }

    if (preloaded_count > 0 && !g_shutting_down.load(std::memory_order_acquire)) {
        openscarlett::CallTelemetry::Global().Record(
            "loader",
            "PreloadDllDependenciesTopologically",
            openscarlett::CallOutcome::Ok,
            static_cast<int32_t>(preloaded_count),
            -1);
    }
}

// ---------------------------------------------------------------------------
// Hook Implementations: Library Loading & Symbol Resolution (Data-Driven)
// ---------------------------------------------------------------------------
HMODULE LoadRoutedDll(const openscarlett::DllRouteDecision& decision,
                      const std::wstring& target_path,
                      const char* api_name,
                      HANDLE hFile,
                      DWORD dwFlags,
                      bool use_ex) {
    constexpr DWORD kDatafilePreloadCheck =
        LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (!use_ex || (dwFlags & kDatafilePreloadCheck) == 0) &&
        !target_path.empty()) {
        PreloadDllDependenciesTopologically(target_path);
    }

    HMODULE loaded = nullptr;
    if (use_ex) {
        DWORD sanitized_flags = dwFlags;
        constexpr DWORD kConflictingSearchFlags =
            LOAD_LIBRARY_SEARCH_SYSTEM32 |
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
            LOAD_LIBRARY_SEARCH_USER_DIRS |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
        if ((sanitized_flags & kConflictingSearchFlags) != 0) {
            sanitized_flags &= ~kConflictingSearchFlags;
        }
        constexpr DWORD kDatafileFlags =
            LOAD_LIBRARY_AS_DATAFILE |
            LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
            LOAD_LIBRARY_AS_IMAGE_RESOURCE;
        if ((sanitized_flags & kDatafileFlags) == 0 && !target_path.empty()) {
            sanitized_flags |= LOAD_WITH_ALTERED_SEARCH_PATH;
        }
        loaded = g_real_LoadLibraryExW(target_path.c_str(), hFile, sanitized_flags);
    } else if (g_real_LoadLibraryExW != nullptr &&
               target_path.find_first_of(L"\\/") != std::wstring::npos) {
        // Use LOAD_WITH_ALTERED_SEARCH_PATH so dependent shims (e.g. XSAPI -> xgameruntime.dll /
        // libHttpClient.GDK.dll) resolve from shim_dir BEFORE C:\Windows\System32\.
        loaded = g_real_LoadLibraryExW(target_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    } else {
        loaded = g_real_LoadLibraryW(target_path.c_str());
    }

    DWORD last_err = ::GetLastError();

    if (!g_shutting_down.load(std::memory_order_acquire)) {
        const bool is_shim = (decision.policy == openscarlett::DllRoutePolicy::Shim);
        if (decision.aliased && !decision.requested_name.empty()) {
            openscarlett::CallTelemetry::Global().Record(
                "loader",
                std::string("dll_alias:") + decision.requested_name + "->" + decision.canonical_name,
                loaded != nullptr ? openscarlett::CallOutcome::Ok
                                  : openscarlett::CallOutcome::NotImplemented,
                loaded != nullptr ? 0 : static_cast<int32_t>(last_err),
                -1);
        }
        if (loaded != nullptr) {
            if (is_shim) {
                RegisterXboxModule(loaded, decision.canonical_name);
            }
            openscarlett::CallTelemetry::Global().Record(
                decision.canonical_name, api_name, openscarlett::CallOutcome::Ok, 0, -1);
            constexpr DWORD kDatafileFlags =
                LOAD_LIBRARY_AS_DATAFILE |
                LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                LOAD_LIBRARY_AS_IMAGE_RESOURCE;
            if ((dwFlags & kDatafileFlags) == 0) {
                PatchModuleImports(loaded);
                PatchAllLoadedNonSystemModules();
            }
        } else if (is_shim) {
            openscarlett::CallTelemetry::Global().Record(
                decision.canonical_name, api_name, openscarlett::CallOutcome::NotImplemented,
                static_cast<int32_t>(last_err), -1);
        }
    }

    ::SetLastError(last_err);
    return loaded;
}

bool TryTranslateVirtualDllPath(std::wstring_view raw_path, std::wstring& out_host_path) {
    if (openscarlett::VfsMounts::DetectVirtualDriveW(raw_path) == openscarlett::VfsDrive::None) {
        return false;
    }
    auto st = g_vfs.TranslateForAccessW(raw_path, openscarlett::VfsAccessMode::ReadOnly, out_host_path, nullptr, true);
    return st == openscarlett::VfsTranslateStatus::Translated;
}

bool TryResolveKnownDllSystem32FirstW(std::wstring_view raw_name, std::wstring& out_sys32_path) {
    out_sys32_path.clear();
    if (raw_name.empty()) {
        return false;
    }
    // Check if an explicit shim override targets this DLL; otherwise enforce KnownDLLs -> System32 first.
    const auto* rule = g_routing_table.FindRule(openscarlett::WideToUtf8(raw_name));
    if (rule && rule->policy == openscarlett::DllRoutePolicy::Shim) {
        return false;
    }
    if (!openscarlett::ResolveKnownDllSystem32PathW(raw_name, out_sys32_path)) {
        return false;
    }
    return !out_sys32_path.empty();
}

HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        std::wstring wide_name = AnsiToWide(lpLibFileName);
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(wide_name, decision, target_path)) {
            return LoadRoutedDll(decision, target_path, "LoadLibraryA", nullptr, 0, false);
        }
        std::wstring known_sys32;
        if (TryResolveKnownDllSystem32FirstW(wide_name, known_sys32)) {
            HMODULE mod = g_real_LoadLibraryW(known_sys32.c_str());
            if (mod != nullptr) {
                return mod;
            }
        }
        std::wstring vfs_host;
        if (TryTranslateVirtualDllPath(wide_name, vfs_host)) {
            HMODULE mod = g_real_LoadLibraryW(vfs_host.c_str());
            DWORD err = ::GetLastError();
            if (mod != nullptr) {
                PatchModuleImports(mod);
                PatchAllLoadedNonSystemModules();
            }
            ::SetLastError(err);
            return mod;
        }
    }
    HMODULE mod = g_real_LoadLibraryA(lpLibFileName);
    DWORD err = ::GetLastError();
    if (mod != nullptr && !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(lpLibFileName, decision, target_path)) {
            return LoadRoutedDll(decision, target_path, "LoadLibraryW", nullptr, 0, false);
        }
        std::wstring known_sys32;
        if (TryResolveKnownDllSystem32FirstW(lpLibFileName, known_sys32)) {
            HMODULE mod = g_real_LoadLibraryW(known_sys32.c_str());
            if (mod != nullptr) {
                return mod;
            }
        }
        std::wstring vfs_host;
        if (TryTranslateVirtualDllPath(lpLibFileName, vfs_host)) {
            HMODULE mod = g_real_LoadLibraryW(vfs_host.c_str());
            DWORD err = ::GetLastError();
            if (mod != nullptr) {
                PatchModuleImports(mod);
                PatchAllLoadedNonSystemModules();
            }
            ::SetLastError(err);
            return mod;
        }
    }
    HMODULE mod = g_real_LoadLibraryW(lpLibFileName);
    DWORD err = ::GetLastError();
    if (mod != nullptr && !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        std::wstring wide_name = AnsiToWide(lpLibFileName);
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(wide_name, decision, target_path)) {
            return LoadRoutedDll(decision, target_path, "LoadLibraryExA", hFile, dwFlags, true);
        }
        std::wstring known_sys32;
        if (TryResolveKnownDllSystem32FirstW(wide_name, known_sys32)) {
            HMODULE mod = g_real_LoadLibraryExW(known_sys32.c_str(), hFile, dwFlags);
            if (mod != nullptr) {
                return mod;
            }
        }
        std::wstring vfs_host;
        if (TryTranslateVirtualDllPath(wide_name, vfs_host)) {
            HMODULE mod = g_real_LoadLibraryExW(vfs_host.c_str(), hFile, dwFlags);
            DWORD err = ::GetLastError();
            constexpr DWORD kDatafileFlags =
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
            if (mod != nullptr && (dwFlags & kDatafileFlags) == 0) {
                PatchModuleImports(mod);
                PatchAllLoadedNonSystemModules();
            }
            ::SetLastError(err);
            return mod;
        }
    }
    HMODULE mod = g_real_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    DWORD err = ::GetLastError();
    constexpr DWORD kDatafileFlags =
        LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (mod != nullptr && (dwFlags & kDatafileFlags) == 0 &&
        !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(lpLibFileName, decision, target_path)) {
            return LoadRoutedDll(decision, target_path, "LoadLibraryExW", hFile, dwFlags, true);
        }
        std::wstring known_sys32;
        if (TryResolveKnownDllSystem32FirstW(lpLibFileName, known_sys32)) {
            HMODULE mod = g_real_LoadLibraryExW(known_sys32.c_str(), hFile, dwFlags);
            if (mod != nullptr) {
                return mod;
            }
        }
        std::wstring vfs_host;
        if (TryTranslateVirtualDllPath(lpLibFileName, vfs_host)) {
            HMODULE mod = g_real_LoadLibraryExW(vfs_host.c_str(), hFile, dwFlags);
            DWORD err = ::GetLastError();
            constexpr DWORD kDatafileFlags =
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
            if (mod != nullptr && (dwFlags & kDatafileFlags) == 0) {
                PatchModuleImports(mod);
                PatchAllLoadedNonSystemModules();
            }
            ::SetLastError(err);
            return mod;
        }
    }
    HMODULE mod = g_real_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    DWORD err = ::GetLastError();
    constexpr DWORD kDatafileFlags =
        LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (mod != nullptr && (dwFlags & kDatafileFlags) == 0 &&
        !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_GetModuleHandleA(LPCSTR lpModuleName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpModuleName != nullptr) {
        std::wstring wide_name = AnsiToWide(lpModuleName);
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(wide_name, decision, target_path) && !target_path.empty()) {
            HMODULE mod = g_real_GetModuleHandleW(target_path.c_str());
            if (mod != nullptr) {
                return mod;
            }
        }
    }
    return g_real_GetModuleHandleA(lpModuleName);
}

HMODULE WINAPI Hook_GetModuleHandleW(LPCWSTR lpModuleName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpModuleName != nullptr) {
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(lpModuleName, decision, target_path) && !target_path.empty()) {
            HMODULE mod = g_real_GetModuleHandleW(target_path.c_str());
            if (mod != nullptr) {
                return mod;
            }
        }
    }
    return g_real_GetModuleHandleW(lpModuleName);
}

BOOL WINAPI Hook_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
        lpModuleName != nullptr) {
        std::wstring wide_name = AnsiToWide(lpModuleName);
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(wide_name, decision, target_path) && !target_path.empty()) {
            if (g_real_GetModuleHandleExW(dwFlags, target_path.c_str(), phModule)) {
                return TRUE;
            }
        }
    }
    return g_real_GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI Hook_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
        lpModuleName != nullptr) {
        openscarlett::DllRouteDecision decision;
        std::wstring target_path;
        if (MatchRoutedShimOrGuestDllW(lpModuleName, decision, target_path) && !target_path.empty()) {
            if (g_real_GetModuleHandleExW(dwFlags, target_path.c_str(), phModule)) {
                return TRUE;
            }
        }
    }
    return g_real_GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    // Intercept dynamic GetProcAddress lookups for CreateProcessW/A on kernel32/kernelbase
    // and NtCreateFile/NtOpenFile/NtQueryAttributesFile on ntdll.dll.
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        hModule != nullptr &&
        reinterpret_cast<uintptr_t>(lpProcName) > 0xFFFFu) {
        if (hModule == g_kernel32_mod || (g_kernelbase_mod != nullptr && hModule == g_kernelbase_mod)) {
            if (g_real_CreateProcessW != nullptr && std::strcmp(lpProcName, "CreateProcessW") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_CreateProcessW);
            }
            if (g_real_CreateProcessA != nullptr && std::strcmp(lpProcName, "CreateProcessA") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_CreateProcessA);
            }
        } else if (g_ntdll_mod != nullptr && hModule == g_ntdll_mod) {
            if (g_real_NtCreateFile != nullptr && std::strcmp(lpProcName, "NtCreateFile") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtCreateFile);
            }
            if (g_real_NtOpenFile != nullptr && std::strcmp(lpProcName, "NtOpenFile") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtOpenFile);
            }
            if (g_real_NtQueryAttributesFile != nullptr &&
                std::strcmp(lpProcName, "NtQueryAttributesFile") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtQueryAttributesFile);
            }
        }
    }

    // Intercept forwarded exports using PeForwarderResolver (handles mixed-case DLLs,
    // API-set contracts like api-ms-win-*/ext-ms-win-*, and cycle-safe chain resolution)
    // BEFORE Windows resolves them against C:\Windows\System32\.
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        hModule != nullptr && lpProcName != nullptr && tl_forwarder_depth < 16u) {
        openscarlett::PeForwarderTarget fwd{};
        std::string raw_fwd;
        if (openscarlett::LookupMappedPeExportForwarder(hModule, lpProcName, fwd, &raw_fwd)) {
            openscarlett::PeForwarderLookupFn mapped_lookup =
                [&](const std::string& norm_dll,
                    const std::string& sym,
                    uint16_t ord,
                    bool by_ord,
                    std::string& out_next) -> bool {
                HMODULE mod_h = g_real_GetModuleHandleW
                                    ? g_real_GetModuleHandleW(openscarlett::Utf8ToWide(norm_dll).c_str())
                                    : nullptr;
                if (!mod_h) {
                    return false;
                }
                LPCSTR key = by_ord ? reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ord))
                                    : sym.c_str();
                openscarlett::PeForwarderTarget next_fwd{};
                return openscarlett::LookupMappedPeExportForwarder(mod_h, key, next_fwd, &out_next);
            };

            const openscarlett::PeForwarderChainResult chain_res =
                g_pe_forwarder.ResolveForwarder(raw_fwd, mapped_lookup, 16u);

            if (chain_res.cycle_detected || chain_res.depth_exceeded ||
                chain_res.kind == openscarlett::ForwarderResolutionKind::MalformedError) {
                openscarlett::CallTelemetry::Global().Record(
                    "loader",
                    std::string("GetProcAddress:forwarder_error:") + raw_fwd,
                    openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(ERROR_PROC_NOT_FOUND),
                    -1);
                ::SetLastError(ERROR_PROC_NOT_FOUND);
                return nullptr;
            }

            const std::string target_dll_name =
                !chain_res.resolved_dll.empty() ? chain_res.resolved_dll : fwd.dll_name;
            const openscarlett::DllRouteDecision decision =
                g_routing_table.ResolveRoute(target_dll_name, g_shim_dir_w, g_guest_dir_w);

            const bool route_to_shim_or_guest =
                chain_res.routed_to_openscarlett_shim ||
                decision.policy == openscarlett::DllRoutePolicy::Shim ||
                (decision.policy == openscarlett::DllRoutePolicy::NativeGuestDll &&
                 !decision.resolved_path.empty());

            if (route_to_shim_or_guest) {
                openscarlett::DllRouteDecision eff_decision = decision;
                if (chain_res.routed_to_openscarlett_shim &&
                    eff_decision.policy != openscarlett::DllRoutePolicy::Shim) {
                    eff_decision.policy = openscarlett::DllRoutePolicy::Shim;
                    eff_decision.canonical_name = target_dll_name;
                }
                std::wstring target_path = !eff_decision.resolved_path.empty()
                                               ? eff_decision.resolved_path
                                               : (g_shim_dir_w.empty()
                                                      ? openscarlett::Utf8ToWide(eff_decision.canonical_name)
                                                      : (g_shim_dir_w + L"\\" +
                                                         openscarlett::Utf8ToWide(eff_decision.canonical_name)));
                HMODULE target_mod = g_real_GetModuleHandleW(target_path.c_str());
                if (!target_mod) {
                    target_mod = LoadRoutedDll(eff_decision, target_path, "GetProcAddress:forwarder",
                                               nullptr, 0, false);
                }
                FARPROC resolved = nullptr;
                DWORD fwd_err = ERROR_PROC_NOT_FOUND;
                if (target_mod) {
                    ++tl_forwarder_depth;
                    const bool by_ord = chain_res.ok ? chain_res.by_ordinal : fwd.by_ordinal;
                    const uint16_t ord = chain_res.ok ? chain_res.resolved_ordinal : fwd.ordinal;
                    const std::string& sym = chain_res.ok ? chain_res.resolved_symbol : fwd.symbol_name;
                    LPCSTR next_proc = by_ord
                                           ? reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ord))
                                           : sym.c_str();
                    resolved = Hook_GetProcAddress(target_mod, next_proc);
                    fwd_err = ::GetLastError();
                    --tl_forwarder_depth;
                } else {
                    fwd_err = ::GetLastError();
                }

                std::string sym_label;
                if (reinterpret_cast<uintptr_t>(lpProcName) <= 0xFFFFu) {
                    sym_label = "ordinal#" + std::to_string(reinterpret_cast<uintptr_t>(lpProcName));
                } else {
                    sym_label = lpProcName;
                }
                openscarlett::CallTelemetry::Global().Record(
                    eff_decision.canonical_name,
                    sym_label + "->" + raw_fwd,
                    resolved != nullptr ? openscarlett::CallOutcome::Ok
                                        : openscarlett::CallOutcome::NotImplemented,
                    resolved != nullptr ? 0 : static_cast<int32_t>(0x80004001u),
                    -1);
                ::SetLastError(resolved != nullptr ? ERROR_SUCCESS : fwd_err);
                return resolved;
            }
        }
    }

    FARPROC proc = g_real_GetProcAddress(hModule, lpProcName);
    DWORD last_err = ::GetLastError();

    if (!g_shutting_down.load(std::memory_order_acquire) && hModule != nullptr && lpProcName != nullptr) {
        std::string mod_name;
        if (LookupXboxModuleName(hModule, mod_name)) {
            std::string sym_name;
            if (reinterpret_cast<uintptr_t>(lpProcName) <= 0xFFFFu) {
                sym_name = "ordinal#" + std::to_string(reinterpret_cast<uintptr_t>(lpProcName));
            } else {
                sym_name = lpProcName;
            }
            if (proc != nullptr) {
                openscarlett::CallTelemetry::Global().Record(
                    mod_name, sym_name, openscarlett::CallOutcome::Ok, 0, -1);
            } else {
                openscarlett::CallTelemetry::Global().Record(
                    mod_name,
                    sym_name,
                    openscarlett::CallOutcome::NotImplemented,
                    static_cast<int32_t>(0x80004001u),  // E_NOTIMPL
                    -1);
                if (g_probe_unknown_imports) {
                    const bool by_ord = (reinterpret_cast<uintptr_t>(lpProcName) <= 0xFFFFu);
                    const uint16_t ord = by_ord
                                             ? static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpProcName))
                                             : 0u;
                    const std::string raw_sym = by_ord ? std::string{} : std::string(lpProcName);
                    if (void* stub = g_probe_trampoline_pool.GetOrCreateTrampoline(
                            mod_name, raw_sym, ord, by_ord)) {
                        ::SetLastError(ERROR_SUCCESS);
                        return reinterpret_cast<FARPROC>(stub);
                    }
                }
            }
        }
    }

    ::SetLastError(last_err);
    return proc;
}

// ---------------------------------------------------------------------------
// Hook Implementations: NT Native File APIs (NtCreateFile / NtOpenFile / NtQueryAttributesFile)
//   Translates \??\G:\..., \??\D:\..., \??\T:\..., \??\S:\... NT object paths
//   through VfsMounts (including nested package sub-mounts & CoW overlay).
// ---------------------------------------------------------------------------
bool ShouldPassThroughHostDDriveW(std::wstring_view raw_path) {
    // Guard CI runners (e.g. GitHub Actions on D:\a\...) so literal host paths under
    // shim_dir / telemetry / existing host D:\ files are never hijacked.
    std::wstring_view stripped = raw_path;
    if (stripped.size() >= 4 &&
        ((stripped[0] == L'\\' && stripped[1] == L'\\' && stripped[2] == L'?' && stripped[3] == L'\\') ||
         (stripped[0] == L'\\' && stripped[1] == L'?' && stripped[2] == L'?' && stripped[3] == L'\\'))) {
        stripped.remove_prefix(4);
    }
    if (stripped.size() < 2 || (stripped[0] != L'D' && stripped[0] != L'd') || stripped[1] != L':') {
        return false;
    }
    if (!g_shim_dir_w.empty() && (g_shim_dir_w[0] == L'D' || g_shim_dir_w[0] == L'd')) {
        std::wstring lo_in = ToLowerAsciiW(stripped);
        std::wstring lo_shim = ToLowerAsciiW(g_shim_dir_w);
        for (wchar_t& c : lo_in) {
            if (c == L'/') c = L'\\';
        }
        for (wchar_t& c : lo_shim) {
            if (c == L'/') c = L'\\';
        }
        if (lo_in.rfind(lo_shim, 0) == 0) {
            return true;
        }
    }
    std::wstring dummy_vfs;
    const auto st = g_vfs.TranslateForAccessW(
        raw_path, openscarlett::VfsAccessMode::ReadOnly, dummy_vfs, nullptr, false);
    if (st == openscarlett::VfsTranslateStatus::NotVirtual) {
        return true;
    }
    return false;
}

bool TryTranslateNtObjectPath(const OS_OBJECT_ATTRIBUTES* oa,
                              ACCESS_MASK desired_access,
                              ULONG create_disposition,
                              bool is_query_only,
                              const char* api_tag,
                              std::wstring& out_nt_host_path,
                              OS_NTSTATUS& out_error_status) {
    out_nt_host_path.clear();
    out_error_status = OS_STATUS_SUCCESS;
    if (!oa || oa->RootDirectory != nullptr || !oa->ObjectName ||
        !oa->ObjectName->Buffer || oa->ObjectName->Length == 0) {
        return false;
    }

    const size_t char_count = static_cast<size_t>(oa->ObjectName->Length) / sizeof(wchar_t);
    std::wstring_view raw_view(oa->ObjectName->Buffer, char_count);
    while (!raw_view.empty() && raw_view.back() == L'\0') {
        raw_view.remove_suffix(1);
    }
    if (raw_view.empty()) {
        return false;
    }

    const openscarlett::VfsDrive drive = openscarlett::VfsMounts::DetectVirtualDriveW(raw_view);
    if (drive == openscarlett::VfsDrive::None || ShouldPassThroughHostDDriveW(raw_view)) {
        return false;
    }

    constexpr ACCESS_MASK kNtWriteMask =
        GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    const bool wants_write = !is_query_only &&
                             (((desired_access & kNtWriteMask) != 0) ||
                              create_disposition == OS_FILE_SUPERSEDE ||
                              create_disposition == OS_FILE_CREATE ||
                              create_disposition == OS_FILE_OVERWRITE ||
                              create_disposition == OS_FILE_OVERWRITE_IF);

    openscarlett::VfsAccessMode mode = openscarlett::VfsAccessMode::ReadOnly;
    if (!is_query_only && wants_write) {
        if (drive == openscarlett::VfsDrive::Game) {
            if (create_disposition == OS_FILE_SUPERSEDE ||
                create_disposition == OS_FILE_CREATE ||
                create_disposition == OS_FILE_OVERWRITE_IF) {
                mode = openscarlett::VfsAccessMode::WriteCreate;
            } else if (create_disposition == OS_FILE_OPEN_IF) {
                mode = g_vfs.IsVisibleGamePathW(raw_view) ? openscarlett::VfsAccessMode::WriteModify
                                                          : openscarlett::VfsAccessMode::WriteCreate;
            } else {
                mode = openscarlett::VfsAccessMode::WriteModify;
            }
        } else {
            mode = openscarlett::VfsAccessMode::WriteCreate;
        }
    } else if (!is_query_only && drive == openscarlett::VfsDrive::Game &&
               create_disposition == OS_FILE_OPEN_IF && !g_vfs.IsVisibleGamePathW(raw_view)) {
        mode = openscarlett::VfsAccessMode::WriteCreate;
    }

    std::wstring translated;
    bool was_wo = false;
    const openscarlett::VfsTranslateStatus st =
        g_vfs.TranslateForAccessW(raw_view, mode, translated, &was_wo, !is_query_only);

    if (st == openscarlett::VfsTranslateStatus::NotVirtual) {
        return false;
    }
    if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":traversal_blocked", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(OS_STATUS_ACCESS_DENIED), -1);
        out_error_status = OS_STATUS_ACCESS_DENIED;
        return true;
    }
    if (st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":readonly_blocked", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(OS_STATUS_ACCESS_DENIED), -1);
        out_error_status = OS_STATUS_ACCESS_DENIED;
        return true;
    }
    if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":whiteout", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(OS_STATUS_OBJECT_NAME_NOT_FOUND), -1);
        out_error_status = OS_STATUS_OBJECT_NAME_NOT_FOUND;
        return true;
    }
    if (st != openscarlett::VfsTranslateStatus::Translated) {
        out_error_status = OS_STATUS_OBJECT_PATH_NOT_FOUND;
        return true;
    }

    // Ensure NT object namespace prefix (\??\) for Nt* syscalls.
    if (translated.size() >= 4 && translated.compare(0, 4, L"\\\\?\\") == 0) {
        translated[1] = L'?';
    } else if (translated.size() < 4 || translated.compare(0, 4, L"\\??\\") != 0) {
        translated.insert(0, L"\\??\\");
    }
    out_nt_host_path = std::move(translated);
    return true;
}

OS_NTSTATUS NTAPI Hook_NtCreateFile(PHANDLE FileHandle,
                                    ACCESS_MASK DesiredAccess,
                                    OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                    OS_IO_STATUS_BLOCK* IoStatusBlock,
                                    PLARGE_INTEGER AllocationSize,
                                    ULONG FileAttributes,
                                    ULONG ShareAccess,
                                    ULONG CreateDisposition,
                                    ULONG CreateOptions,
                                    PVOID EaBuffer,
                                    ULONG EaLength) {
    if (!g_shutting_down.load(std::memory_order_acquire) && ObjectAttributes != nullptr) {
        std::wstring host_nt;
        OS_NTSTATUS err_status = OS_STATUS_SUCCESS;
        if (TryTranslateNtObjectPath(ObjectAttributes, DesiredAccess, CreateDisposition, false,
                                     "NtCreateFile", host_nt, err_status)) {
            if (err_status != OS_STATUS_SUCCESS) {
                if (IoStatusBlock) {
                    IoStatusBlock->Status = err_status;
                    IoStatusBlock->Information = 0;
                }
                return err_status;
            }
            OS_UNICODE_STRING new_name{};
            new_name.Length = static_cast<USHORT>(host_nt.size() * sizeof(wchar_t));
            new_name.MaximumLength = static_cast<USHORT>((host_nt.size() + 1u) * sizeof(wchar_t));
            new_name.Buffer = host_nt.data();
            OS_OBJECT_ATTRIBUTES new_oa = *ObjectAttributes;
            new_oa.ObjectName = &new_name;

            const OS_NTSTATUS st = g_real_NtCreateFile(FileHandle,
                                                       DesiredAccess,
                                                       &new_oa,
                                                       IoStatusBlock,
                                                       AllocationSize,
                                                       FileAttributes,
                                                       ShareAccess,
                                                       CreateDisposition,
                                                       CreateOptions,
                                                       EaBuffer,
                                                       EaLength);
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "NtCreateFile",
                st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                static_cast<int32_t>(st),
                -1);
            return st;
        }
    }
    return g_real_NtCreateFile(FileHandle,
                               DesiredAccess,
                               ObjectAttributes,
                               IoStatusBlock,
                               AllocationSize,
                               FileAttributes,
                               ShareAccess,
                               CreateDisposition,
                               CreateOptions,
                               EaBuffer,
                               EaLength);
}

OS_NTSTATUS NTAPI Hook_NtOpenFile(PHANDLE FileHandle,
                                  ACCESS_MASK DesiredAccess,
                                  OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                  OS_IO_STATUS_BLOCK* IoStatusBlock,
                                  ULONG ShareAccess,
                                  ULONG OpenOptions) {
    if (!g_shutting_down.load(std::memory_order_acquire) && ObjectAttributes != nullptr) {
        std::wstring host_nt;
        OS_NTSTATUS err_status = OS_STATUS_SUCCESS;
        if (TryTranslateNtObjectPath(ObjectAttributes, DesiredAccess, OS_FILE_OPEN, false,
                                     "NtOpenFile", host_nt, err_status)) {
            if (err_status != OS_STATUS_SUCCESS) {
                if (IoStatusBlock) {
                    IoStatusBlock->Status = err_status;
                    IoStatusBlock->Information = 0;
                }
                return err_status;
            }
            OS_UNICODE_STRING new_name{};
            new_name.Length = static_cast<USHORT>(host_nt.size() * sizeof(wchar_t));
            new_name.MaximumLength = static_cast<USHORT>((host_nt.size() + 1u) * sizeof(wchar_t));
            new_name.Buffer = host_nt.data();
            OS_OBJECT_ATTRIBUTES new_oa = *ObjectAttributes;
            new_oa.ObjectName = &new_name;

            const OS_NTSTATUS st = g_real_NtOpenFile(FileHandle,
                                                     DesiredAccess,
                                                     &new_oa,
                                                     IoStatusBlock,
                                                     ShareAccess,
                                                     OpenOptions);
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "NtOpenFile",
                st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                static_cast<int32_t>(st),
                -1);
            return st;
        }
    }
    return g_real_NtOpenFile(FileHandle,
                             DesiredAccess,
                             ObjectAttributes,
                             IoStatusBlock,
                             ShareAccess,
                             OpenOptions);
}

OS_NTSTATUS NTAPI Hook_NtQueryAttributesFile(OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                             OS_FILE_BASIC_INFORMATION* FileInformation) {
    if (!g_shutting_down.load(std::memory_order_acquire) && ObjectAttributes != nullptr) {
        std::wstring host_nt;
        OS_NTSTATUS err_status = OS_STATUS_SUCCESS;
        if (TryTranslateNtObjectPath(ObjectAttributes, 0u, OS_FILE_OPEN, true,
                                     "NtQueryAttributesFile", host_nt, err_status)) {
            if (err_status != OS_STATUS_SUCCESS) {
                return err_status;
            }
            OS_UNICODE_STRING new_name{};
            new_name.Length = static_cast<USHORT>(host_nt.size() * sizeof(wchar_t));
            new_name.MaximumLength = static_cast<USHORT>((host_nt.size() + 1u) * sizeof(wchar_t));
            new_name.Buffer = host_nt.data();
            OS_OBJECT_ATTRIBUTES new_oa = *ObjectAttributes;
            new_oa.ObjectName = &new_name;

            const OS_NTSTATUS st = g_real_NtQueryAttributesFile(&new_oa, FileInformation);
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "NtQueryAttributesFile",
                st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                static_cast<int32_t>(st),
                -1);
            return st;
        }
    }
    return g_real_NtQueryAttributesFile(ObjectAttributes, FileInformation);
}

// ---------------------------------------------------------------------------
// Hook Implementations: Read-Only CoW Overlay Virtual Drive VFS
//   Supports G:\, GAME:\, D:\, T:\, S:\ (never mutates target game package dir!)
// ---------------------------------------------------------------------------
HANDLE CreateVirtualFileW(std::wstring_view virtual_path,
                          const char* api_tag,
                          DWORD dwDesiredAccess,
                          DWORD dwShareMode,
                          LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                          DWORD dwCreationDisposition,
                          DWORD dwFlagsAndAttributes,
                          HANDLE hTemplateFile) {
    const openscarlett::VfsDrive drive = openscarlett::VfsMounts::DetectVirtualDriveW(virtual_path);
    constexpr DWORD kWriteAccessMask =
        GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    const bool wants_write =
        (dwDesiredAccess & kWriteAccessMask) != 0 ||
        dwCreationDisposition == CREATE_NEW ||
        dwCreationDisposition == CREATE_ALWAYS ||
        dwCreationDisposition == TRUNCATE_EXISTING;

    std::wstring host_path;
    bool was_whiteout = false;
    openscarlett::VfsTranslateStatus st = openscarlett::VfsTranslateStatus::NotVirtual;

    if (drive == openscarlett::VfsDrive::Game) {
        if (!wants_write && dwCreationDisposition == OPEN_EXISTING) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_whiteout, true);
        } else if (!wants_write && dwCreationDisposition == OPEN_ALWAYS) {
            // Check traversal & visibility first.
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_whiteout, true);
            if (st != openscarlett::VfsTranslateStatus::RejectedTraversal) {
                if (!g_vfs.IsVisibleGamePathW(virtual_path)) {
                    st = g_vfs.TranslateForAccessW(
                        virtual_path, openscarlett::VfsAccessMode::WriteCreate, host_path, &was_whiteout, true);
                }
            }
        } else if (dwCreationDisposition == CREATE_NEW) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_whiteout, true);
            if (st != openscarlett::VfsTranslateStatus::RejectedTraversal) {
                if (g_vfs.IsVisibleGamePathW(virtual_path)) {
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs", api_tag, openscarlett::CallOutcome::Error,
                        static_cast<int32_t>(ERROR_FILE_EXISTS), -1);
                    ::SetLastError(ERROR_FILE_EXISTS);
                    return INVALID_HANDLE_VALUE;
                }
                st = g_vfs.TranslateForAccessW(
                    virtual_path, openscarlett::VfsAccessMode::WriteCreate, host_path, &was_whiteout, true);
            }
        } else if (dwCreationDisposition == CREATE_ALWAYS) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::WriteCreate, host_path, &was_whiteout, true);
        } else if (dwCreationDisposition == TRUNCATE_EXISTING ||
                   dwCreationDisposition == OPEN_EXISTING) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::WriteModify, host_path, &was_whiteout, true);
        } else {
            // OPEN_ALWAYS with write access: CoW-copy if visible, else create fresh in overlay.
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_whiteout, true);
            if (st != openscarlett::VfsTranslateStatus::RejectedTraversal) {
                const auto mode = g_vfs.IsVisibleGamePathW(virtual_path)
                                      ? openscarlett::VfsAccessMode::WriteModify
                                      : openscarlett::VfsAccessMode::WriteCreate;
                st = g_vfs.TranslateForAccessW(virtual_path, mode, host_path, &was_whiteout, true);
            }
        }
    } else {
        const auto mode = wants_write ? openscarlett::VfsAccessMode::WriteCreate
                                      : openscarlett::VfsAccessMode::ReadOnly;
        st = g_vfs.TranslateForAccessW(virtual_path, mode, host_path, &was_whiteout, true);
    }

    if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":traversal_blocked", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":readonly_blocked", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":whiteout", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
        ::SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (st == openscarlett::VfsTranslateStatus::NotVirtual) {
        const std::wstring passthrough_path(virtual_path);
        return g_real_CreateFileW(passthrough_path.c_str(),
                                  dwDesiredAccess,
                                  dwShareMode,
                                  lpSecurityAttributes,
                                  dwCreationDisposition,
                                  dwFlagsAndAttributes,
                                  hTemplateFile);
    }
    if (st != openscarlett::VfsTranslateStatus::Translated) {
        ::SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE h = g_real_CreateFileW(host_path.c_str(),
                                  dwDesiredAccess,
                                  dwShareMode,
                                  lpSecurityAttributes,
                                  dwCreationDisposition,
                                  dwFlagsAndAttributes,
                                  hTemplateFile);
    DWORD err = ::GetLastError();
    openscarlett::CallTelemetry::Global().Record(
        "vfs",
        api_tag,
        h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
        -1);
    ::SetLastError(err);
    return h;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        return CreateVirtualFileW(lpFileName,
                                  "CreateFileW",
                                  dwDesiredAccess,
                                  dwShareMode,
                                  lpSecurityAttributes,
                                  dwCreationDisposition,
                                  dwFlagsAndAttributes,
                                  hTemplateFile);
    }
    return g_real_CreateFileW(lpFileName,
                              dwDesiredAccess,
                              dwShareMode,
                              lpSecurityAttributes,
                              dwCreationDisposition,
                              dwFlagsAndAttributes,
                              hTemplateFile);
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
            return CreateVirtualFileW(wide_in,
                                      "CreateFileA",
                                      dwDesiredAccess,
                                      dwShareMode,
                                      lpSecurityAttributes,
                                      dwCreationDisposition,
                                      dwFlagsAndAttributes,
                                      hTemplateFile);
        }
    }
    return g_real_CreateFileA(lpFileName,
                              dwDesiredAccess,
                              dwShareMode,
                              lpSecurityAttributes,
                              dwCreationDisposition,
                              dwFlagsAndAttributes,
                              hTemplateFile);
}

DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR lpFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        bool was_wo = false;
        auto st = g_vfs.TranslateForAccessW(
            lpFileName, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesW:whiteout", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            DWORD attrs = g_real_GetFileAttributesW(host_path.c_str());
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "GetFileAttributesW",
                attrs != INVALID_FILE_ATTRIBUTES ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                attrs != INVALID_FILE_ATTRIBUTES ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return attrs;
        }
    }
    return g_real_GetFileAttributesW(lpFileName);
}

DWORD WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring wide_in = AnsiToWide(lpFileName);
        std::wstring host_path;
        bool was_wo = false;
        auto st = g_vfs.TranslateForAccessW(
            wide_in, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesA:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesA:whiteout", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            DWORD attrs = g_real_GetFileAttributesW(host_path.c_str());
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "GetFileAttributesA",
                attrs != INVALID_FILE_ATTRIBUTES ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                attrs != INVALID_FILE_ATTRIBUTES ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return attrs;
        }
    }
    return g_real_GetFileAttributesA(lpFileName);
}

BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        bool was_wo = false;
        auto st = g_vfs.TranslateForAccessW(
            lpFileName, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesExW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesExW:whiteout", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            BOOL ok = g_real_GetFileAttributesExW(host_path.c_str(), fInfoLevelId, lpFileInformation);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "GetFileAttributesExW",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return ok;
        }
    }
    return g_real_GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

BOOL WINAPI Hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName) != openscarlett::VfsDrive::None) {
        const auto drive = openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName);
        std::wstring host_path;
        if (drive == openscarlett::VfsDrive::Game) {
            auto chk = g_vfs.TranslateForAccessW(
                lpPathName, openscarlett::VfsAccessMode::ReadOnly, host_path, nullptr, false);
            if (chk == openscarlett::VfsTranslateStatus::RejectedTraversal) {
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "CreateDirectoryW:traversal_blocked", openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
                ::SetLastError(ERROR_ACCESS_DENIED);
                return FALSE;
            }
            if (g_vfs.IsVisibleGamePathW(lpPathName)) {
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "CreateDirectoryW", openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(ERROR_ALREADY_EXISTS), -1);
                ::SetLastError(ERROR_ALREADY_EXISTS);
                return FALSE;
            }
        }

        auto st = g_vfs.TranslateForAccessW(
            lpPathName, openscarlett::VfsAccessMode::WriteCreate, host_path, nullptr, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal ||
            st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "CreateDirectoryW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            BOOL ok = g_real_CreateDirectoryW(host_path.c_str(), lpSecurityAttributes);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "CreateDirectoryW",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return ok;
        }
    }
    return g_real_CreateDirectoryW(lpPathName, lpSecurityAttributes);
}

BOOL WINAPI Hook_DeleteFileW(LPCWSTR lpFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        uint32_t err = 0;
        bool ok = g_vfs.DeleteVirtualFileW(lpFileName, err);
        openscarlett::CallTelemetry::Global().Record(
            "vfs",
            "DeleteFileW",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
            ok ? 0 : static_cast<int32_t>(err),
            -1);
        ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
        return ok ? TRUE : FALSE;
    }
    return g_real_DeleteFileW(lpFileName);
}

BOOL WINAPI Hook_DeleteFileA(LPCSTR lpFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
            uint32_t err = 0;
            bool ok = g_vfs.DeleteVirtualFileW(wide_in, err);
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "DeleteFileA",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
            return ok ? TRUE : FALSE;
        }
    }
    return g_real_DeleteFileA(lpFileName);
}

BOOL WINAPI Hook_RemoveDirectoryW(LPCWSTR lpPathName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpPathName)) {
        uint32_t err = 0;
        bool ok = g_vfs.RemoveVirtualDirectoryW(lpPathName, err);
        openscarlett::CallTelemetry::Global().Record(
            "vfs",
            "RemoveDirectoryW",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
            ok ? 0 : static_cast<int32_t>(err),
            -1);
        ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
        return ok ? TRUE : FALSE;
    }
    return g_real_RemoveDirectoryW(lpPathName);
}

BOOL WINAPI Hook_MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpExistingFileName != nullptr && lpNewFileName != nullptr &&
        ((openscarlett::VfsMounts::DetectVirtualDriveW(lpExistingFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpExistingFileName)) ||
         (openscarlett::VfsMounts::DetectVirtualDriveW(lpNewFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpNewFileName)))) {
        uint32_t err = 0;
        bool ok = g_vfs.MoveVirtualPathW(lpExistingFileName, lpNewFileName, dwFlags, err);
        openscarlett::CallTelemetry::Global().Record(
            "vfs",
            "MoveFileExW",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
            ok ? 0 : static_cast<int32_t>(err),
            -1);
        ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
        return ok ? TRUE : FALSE;
    }
    return g_real_MoveFileExW(lpExistingFileName, lpNewFileName, dwFlags);
}

HANDLE WINAPI Hook_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        const std::wstring_view in_view(lpFileName);
        const auto drive = openscarlett::VfsMounts::DetectVirtualDriveW(in_view);
        const bool has_wildcards = (in_view.find_first_of(L"*?") != std::wstring_view::npos);

        if (drive == openscarlett::VfsDrive::Game && has_wildcards) {
            std::wstring base_pat;
            std::wstring overlay_pat;
            std::wstring rel_pat;
            bool was_wo = false;
            auto st = g_vfs.ResolveGamePairW(in_view, base_pat, overlay_pat, rel_pat, &was_wo);
            if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "FindFirstFileW:traversal_blocked", openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
                ::SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
                ::SetLastError(ERROR_FILE_NOT_FOUND);
                return INVALID_HANDLE_VALUE;
            }
            if (st == openscarlett::VfsTranslateStatus::Translated) {
                std::wstring rel_dir;
                const size_t slash = rel_pat.find_last_of(L'\\');
                if (slash != std::wstring::npos) {
                    rel_dir = rel_pat.substr(0, slash);
                }

                auto is_hidden_entry = [&](const wchar_t* filename, bool from_overlay) -> bool {
                    std::wstring_view fn(filename);
                    if (from_overlay && rel_dir.empty() && fn == L".whiteout") {
                        return true;
                    }
                    if (fn == L"." || fn == L"..") {
                        return false;
                    }
                    std::wstring vpath = L"G:\\";
                    if (!rel_dir.empty()) {
                        vpath += rel_dir + L"\\";
                    }
                    vpath.append(fn);
                    return g_vfs.IsWhiteoutedW(vpath);
                };

                auto find_visible = [&](const std::wstring& pat, bool from_overlay) -> HANDLE {
                    if (pat.empty()) {
                        return INVALID_HANDLE_VALUE;
                    }
                    HANDLE h = g_real_FindFirstFileW(pat.c_str(), lpFindFileData);
                    if (h == INVALID_HANDLE_VALUE) {
                        return INVALID_HANDLE_VALUE;
                    }
                    while (is_hidden_entry(lpFindFileData->cFileName, from_overlay)) {
                        if (!::FindNextFileW(h, lpFindFileData)) {
                            ::FindClose(h);
                            return INVALID_HANDLE_VALUE;
                        }
                    }
                    return h;
                };

                HANDLE h = find_visible(overlay_pat, true);
                if (h == INVALID_HANDLE_VALUE) {
                    h = find_visible(base_pat, false);
                }
                DWORD err = (h != INVALID_HANDLE_VALUE) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
                openscarlett::CallTelemetry::Global().Record(
                    "vfs",
                    "FindFirstFileW",
                    h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                    h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                    -1);
                ::SetLastError(err);
                return h;
            }
        }

        std::wstring host_path;
        bool was_wo = false;
        auto st = g_vfs.TranslateForAccessW(
            lpFileName, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "FindFirstFileW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "FindFirstFileW:whiteout", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            HANDLE h = g_real_FindFirstFileW(host_path.c_str(), lpFindFileData);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "FindFirstFileW",
                h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return h;
        }
    }
    return g_real_FindFirstFileW(lpFileName, lpFindFileData);
}

// ---------------------------------------------------------------------------
// Hook Implementations: Win32 Child-Process Hook Propagation
//   Hooks CreateProcessW / CreateProcessA so non-Store guest launchers
//   (e.g. Unreal Game.exe -> *-Win64-Shipping.exe) propagate OPENSCARLETT_*
//   environment variables, non-clobbering per-PID child telemetry paths, and
//   openscarlett_hook.dll APC injection into spawned Win32 children.
// ---------------------------------------------------------------------------
std::wstring QueryChildProcessImagePathW(HANDLE h_process) {
    if (!h_process) {
        return {};
    }
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD size = static_cast<DWORD>(buf.size());
        if (::QueryFullProcessImageNameW(h_process, 0, buf.data(), &size)) {
            buf.resize(size);
            return buf;
        }
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || buf.size() >= 32768u) {
            return {};
        }
        buf.resize(buf.size() * 2u);
    }
}

bool IsProtectedStoreOrSystemExeW(const std::wstring& exe_path) {
    if (exe_path.empty()) {
        return false;
    }
    std::wstring lo = ToLowerAsciiW(exe_path);
    for (wchar_t& c : lo) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (lo.find(L"\\windowsapps\\") != std::wstring::npos) {
        return true;
    }
    if (!g_windows_dir_lower_w.empty()) {
        const std::wstring sys32 = g_windows_dir_lower_w + L"\\system32\\";
        const std::wstring syswow = g_windows_dir_lower_w + L"\\syswow64\\";
        if (lo.rfind(sys32, 0) == 0 || lo.rfind(syswow, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool IsInternalToolchainExeW(const std::wstring& exe_path) {
    if (exe_path.empty()) {
        return false;
    }
    const std::wstring leaf_lo = ToLowerAsciiW(std::filesystem::path(exe_path).filename().wstring());
    return leaf_lo == L"dxc.exe" ||
           leaf_lo == L"spirv-val.exe" ||
           leaf_lo == L"spirv-as.exe" ||
           leaf_lo == L"spirv-dis.exe" ||
           leaf_lo == L"openscarlett.exe" ||
           leaf_lo == L"openscarlettlauncher.exe";
}

std::string WideToAnsiAcp(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

void PropagateHookToCreatedChild(const PROCESS_INFORMATION& pi,
                                 bool caller_suspended,
                                 const char* api_tag) {
    const std::wstring child_exe_w = QueryChildProcessImagePathW(pi.hProcess);

    // Strictly respect Microsoft Store / WindowsApps & Windows system process boundaries,
    // and never APC-inject into OpenScarlett's own internal shader compiler/validator helpers
    // (e.g. dxc.exe / spirv-val.exe spawned by d3d12_xs.dll during RDNA2 auto-lifting).
    if (IsProtectedStoreOrSystemExeW(child_exe_w) || IsInternalToolchainExeW(child_exe_w)) {
        const char* skip_suffix =
            IsInternalToolchainExeW(child_exe_w) ? ":child_skipped_toolchain" : ":child_skipped_protected";
        openscarlett::CallTelemetry::Global().Record(
            "loader",
            std::string(api_tag) + skip_suffix,
            openscarlett::CallOutcome::StubOk,
            static_cast<int32_t>(pi.dwProcessId),
            -1);
        if (!caller_suspended) {
            ::ResumeThread(pi.hThread);
        }
        return;
    }

    if (!g_telemetry_base_w.empty() && pi.dwProcessId != 0u) {
        const std::wstring expected_child_tel =
            openscarlett::FormatChildTelemetryPathW(g_telemetry_base_w, pi.dwProcessId);
        if (!expected_child_tel.empty()) {
            const std::wstring parent_dir =
                std::filesystem::path(expected_child_tel).parent_path().wstring();
            if (!parent_dir.empty()) {
                openscarlett::EnsureDirectoryTreeW(parent_dir);
            }
        }
    }

    const std::wstring child_guest_dir_w =
        !child_exe_w.empty() ? std::filesystem::path(child_exe_w).parent_path().wstring() : g_guest_dir_w;

    uint32_t inject_win32_err = 0u;
    const openscarlett::Status st = openscarlett::InjectHookDllIntoSuspendedProcess(
        pi.hProcess,
        pi.hThread,
        g_hook_dll_path_w,
        g_shim_dir_w,
        child_guest_dir_w,
        &inject_win32_err);

    if (st.ok()) {
        openscarlett::CallTelemetry::Global().Record(
            "loader",
            std::string(api_tag) + ":child_hook_propagated",
            openscarlett::CallOutcome::Ok,
            static_cast<int32_t>(pi.dwProcessId),
            -1);
    } else {
        // Gracefully fall back on ERROR_ACCESS_DENIED (protected process) or incompatible arch
        // without terminating or breaking child launch.
        const std::string sym =
            std::string(api_tag) +
            (inject_win32_err == ERROR_ACCESS_DENIED ? ":child_access_denied_fallback"
                                                     : ":child_inject_skipped");
        openscarlett::CallTelemetry::Global().Record(
            "loader",
            sym,
            openscarlett::CallOutcome::StubOk,
            static_cast<int32_t>(inject_win32_err),
            -1);
    }

    FlushTelemetryInternal(false);

    if (!caller_suspended) {
        ::ResumeThread(pi.hThread);
    }
}

BOOL WINAPI Hook_CreateProcessW(LPCWSTR lpApplicationName,
                                LPWSTR lpCommandLine,
                                LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                BOOL bInheritHandles,
                                DWORD dwCreationFlags,
                                LPVOID lpEnvironment,
                                LPCWSTR lpCurrentDirectory,
                                LPSTARTUPINFOW lpStartupInfo,
                                LPPROCESS_INFORMATION lpProcessInformation) {
    if (!g_real_CreateProcessW) {
        ::SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    if (g_shutting_down.load(std::memory_order_acquire) ||
        tl_in_create_process_hook ||
        !lpProcessInformation ||
        g_hook_dll_path_w.empty()) {
        return g_real_CreateProcessW(lpApplicationName,
                                     lpCommandLine,
                                     lpProcessAttributes,
                                     lpThreadAttributes,
                                     bInheritHandles,
                                     dwCreationFlags,
                                     lpEnvironment,
                                     lpCurrentDirectory,
                                     lpStartupInfo,
                                     lpProcessInformation);
    }

    tl_in_create_process_hook = true;
    struct RecursionGuard {
        ~RecursionGuard() { tl_in_create_process_hook = false; }
    } guard;

    std::wstring translated_app;
    LPCWSTR effective_app = lpApplicationName;
    if (lpApplicationName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpApplicationName) != openscarlett::VfsDrive::None) {
        if (g_vfs.TranslateForAccessW(lpApplicationName,
                                      openscarlett::VfsAccessMode::ReadOnly,
                                      translated_app,
                                      nullptr,
                                      true) == openscarlett::VfsTranslateStatus::Translated) {
            effective_app = translated_app.c_str();
        }
    }

    std::wstring translated_cwd;
    LPCWSTR effective_cwd = lpCurrentDirectory;
    if (lpCurrentDirectory != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpCurrentDirectory) != openscarlett::VfsDrive::None) {
        if (g_vfs.TranslateForAccessW(lpCurrentDirectory,
                                      openscarlett::VfsAccessMode::ReadOnly,
                                      translated_cwd,
                                      nullptr,
                                      true) == openscarlett::VfsTranslateStatus::Translated) {
            effective_cwd = translated_cwd.c_str();
        }
    }

    const bool caller_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    const bool caller_env_unicode = (dwCreationFlags & CREATE_UNICODE_ENVIRONMENT) != 0;

    std::vector<wchar_t> merged_env = openscarlett::MergeOpenScarlettEnvironmentBlockW(
        lpEnvironment,
        caller_env_unicode,
        g_shim_dir_w,
        g_hook_dll_path_w,
        g_telemetry_base_w,
        !g_telemetry_base_w.empty());

    const DWORD child_flags = dwCreationFlags | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    const BOOL created = g_real_CreateProcessW(effective_app,
                                               lpCommandLine,
                                               lpProcessAttributes,
                                               lpThreadAttributes,
                                               bInheritHandles,
                                               child_flags,
                                               merged_env.data(),
                                               effective_cwd,
                                               lpStartupInfo,
                                               lpProcessInformation);
    const DWORD create_err = ::GetLastError();
    if (!created) {
        ::SetLastError(create_err);
        return FALSE;
    }

    PropagateHookToCreatedChild(*lpProcessInformation, caller_suspended, "CreateProcessW");
    ::SetLastError(create_err);
    return TRUE;
}

BOOL WINAPI Hook_CreateProcessA(LPCSTR lpApplicationName,
                                LPSTR lpCommandLine,
                                LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                BOOL bInheritHandles,
                                DWORD dwCreationFlags,
                                LPVOID lpEnvironment,
                                LPCSTR lpCurrentDirectory,
                                LPSTARTUPINFOA lpStartupInfo,
                                LPPROCESS_INFORMATION lpProcessInformation) {
    if (!g_real_CreateProcessA) {
        ::SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    if (g_shutting_down.load(std::memory_order_acquire) ||
        tl_in_create_process_hook ||
        !lpProcessInformation ||
        g_hook_dll_path_w.empty()) {
        return g_real_CreateProcessA(lpApplicationName,
                                     lpCommandLine,
                                     lpProcessAttributes,
                                     lpThreadAttributes,
                                     bInheritHandles,
                                     dwCreationFlags,
                                     lpEnvironment,
                                     lpCurrentDirectory,
                                     lpStartupInfo,
                                     lpProcessInformation);
    }

    tl_in_create_process_hook = true;
    struct RecursionGuard {
        ~RecursionGuard() { tl_in_create_process_hook = false; }
    } guard;

    std::string translated_app_a;
    LPCSTR effective_app = lpApplicationName;
    if (lpApplicationName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpApplicationName) != openscarlett::VfsDrive::None) {
        std::wstring host_w;
        if (g_vfs.TranslateForAccessW(AnsiToWide(lpApplicationName),
                                      openscarlett::VfsAccessMode::ReadOnly,
                                      host_w,
                                      nullptr,
                                      true) == openscarlett::VfsTranslateStatus::Translated) {
            translated_app_a = WideToAnsiAcp(host_w);
            if (!translated_app_a.empty()) {
                effective_app = translated_app_a.c_str();
            }
        }
    }

    std::string translated_cwd_a;
    LPCSTR effective_cwd = lpCurrentDirectory;
    if (lpCurrentDirectory != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpCurrentDirectory) != openscarlett::VfsDrive::None) {
        std::wstring host_w;
        if (g_vfs.TranslateForAccessW(AnsiToWide(lpCurrentDirectory),
                                      openscarlett::VfsAccessMode::ReadOnly,
                                      host_w,
                                      nullptr,
                                      true) == openscarlett::VfsTranslateStatus::Translated) {
            translated_cwd_a = WideToAnsiAcp(host_w);
            if (!translated_cwd_a.empty()) {
                effective_cwd = translated_cwd_a.c_str();
            }
        }
    }

    const bool caller_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    const bool caller_env_unicode = (dwCreationFlags & CREATE_UNICODE_ENVIRONMENT) != 0;

    std::vector<wchar_t> merged_env = openscarlett::MergeOpenScarlettEnvironmentBlockW(
        lpEnvironment,
        caller_env_unicode,
        g_shim_dir_w,
        g_hook_dll_path_w,
        g_telemetry_base_w,
        !g_telemetry_base_w.empty());

    // Win32 CreateProcessA supports Unicode lpEnvironment when CREATE_UNICODE_ENVIRONMENT is set.
    const DWORD child_flags = dwCreationFlags | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    const BOOL created = g_real_CreateProcessA(effective_app,
                                               lpCommandLine,
                                               lpProcessAttributes,
                                               lpThreadAttributes,
                                               bInheritHandles,
                                               child_flags,
                                               merged_env.data(),
                                               effective_cwd,
                                               lpStartupInfo,
                                               lpProcessInformation);
    const DWORD create_err = ::GetLastError();
    if (!created) {
        ::SetLastError(create_err);
        return FALSE;
    }

    PropagateHookToCreatedChild(*lpProcessInformation, caller_suspended, "CreateProcessA");
    ::SetLastError(create_err);
    return TRUE;
}

VOID WINAPI Hook_OutputDebugStringA(LPCSTR lpOutputString) {
    if (g_trace_ods && lpOutputString != nullptr && *lpOutputString != '\0') {
        std::fprintf(stderr, "[guest:ODS] %s", lpOutputString);
        const size_t len = std::strlen(lpOutputString);
        if (len > 0 && lpOutputString[len - 1] != '\n') {
            std::fputc('\n', stderr);
        }
        std::fflush(stderr);
    }
    if (g_real_OutputDebugStringA != nullptr) {
        g_real_OutputDebugStringA(lpOutputString);
    }
}

VOID WINAPI Hook_ExitProcess(UINT uExitCode) {
    if (g_trace_ods && uExitCode != 0u) {
        const DWORD last_err = ::GetLastError();
        std::fprintf(stderr, "[guest:ExitProcess] code=%u last_error=%lu\n",
                     static_cast<unsigned>(uExitCode), static_cast<unsigned long>(last_err));
        std::fflush(stderr);
    }
    FlushTelemetryInternal(true);
    g_shutting_down.store(true, std::memory_order_release);
    g_real_ExitProcess(uExitCode);
}

// ---------------------------------------------------------------------------
// IAT & Delay-Load Import Table Patcher (PE32+ x64) + TLS / .pdata Verification
// ---------------------------------------------------------------------------
struct HookBinding {
    const char* symbol_name;
    void* replacement;
    void** real_slot;
};

void* ResolveHookForSymbol(const char* name) {
    if (!name) {
        return nullptr;
    }
    static const HookBinding kBindings[] = {
        {"LoadLibraryA", reinterpret_cast<void*>(&Hook_LoadLibraryA),
         reinterpret_cast<void**>(&g_real_LoadLibraryA)},
        {"LoadLibraryW", reinterpret_cast<void*>(&Hook_LoadLibraryW),
         reinterpret_cast<void**>(&g_real_LoadLibraryW)},
        {"LoadLibraryExA", reinterpret_cast<void*>(&Hook_LoadLibraryExA),
         reinterpret_cast<void**>(&g_real_LoadLibraryExA)},
        {"LoadLibraryExW", reinterpret_cast<void*>(&Hook_LoadLibraryExW),
         reinterpret_cast<void**>(&g_real_LoadLibraryExW)},
        {"GetModuleHandleA", reinterpret_cast<void*>(&Hook_GetModuleHandleA),
         reinterpret_cast<void**>(&g_real_GetModuleHandleA)},
        {"GetModuleHandleW", reinterpret_cast<void*>(&Hook_GetModuleHandleW),
         reinterpret_cast<void**>(&g_real_GetModuleHandleW)},
        {"GetModuleHandleExA", reinterpret_cast<void*>(&Hook_GetModuleHandleExA),
         reinterpret_cast<void**>(&g_real_GetModuleHandleExA)},
        {"GetModuleHandleExW", reinterpret_cast<void*>(&Hook_GetModuleHandleExW),
         reinterpret_cast<void**>(&g_real_GetModuleHandleExW)},
        {"GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
         reinterpret_cast<void**>(&g_real_GetProcAddress)},
        {"CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA),
         reinterpret_cast<void**>(&g_real_CreateFileA)},
        {"CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW),
         reinterpret_cast<void**>(&g_real_CreateFileW)},
        {"GetFileAttributesA", reinterpret_cast<void*>(&Hook_GetFileAttributesA),
         reinterpret_cast<void**>(&g_real_GetFileAttributesA)},
        {"GetFileAttributesW", reinterpret_cast<void*>(&Hook_GetFileAttributesW),
         reinterpret_cast<void**>(&g_real_GetFileAttributesW)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&Hook_GetFileAttributesExW),
         reinterpret_cast<void**>(&g_real_GetFileAttributesExW)},
        {"CreateDirectoryW", reinterpret_cast<void*>(&Hook_CreateDirectoryW),
         reinterpret_cast<void**>(&g_real_CreateDirectoryW)},
        {"DeleteFileA", reinterpret_cast<void*>(&Hook_DeleteFileA),
         reinterpret_cast<void**>(&g_real_DeleteFileA)},
        {"DeleteFileW", reinterpret_cast<void*>(&Hook_DeleteFileW),
         reinterpret_cast<void**>(&g_real_DeleteFileW)},
        {"MoveFileExW", reinterpret_cast<void*>(&Hook_MoveFileExW),
         reinterpret_cast<void**>(&g_real_MoveFileExW)},
        {"RemoveDirectoryW", reinterpret_cast<void*>(&Hook_RemoveDirectoryW),
         reinterpret_cast<void**>(&g_real_RemoveDirectoryW)},
        {"FindFirstFileW", reinterpret_cast<void*>(&Hook_FindFirstFileW),
         reinterpret_cast<void**>(&g_real_FindFirstFileW)},
        {"CreateProcessA", reinterpret_cast<void*>(&Hook_CreateProcessA),
         reinterpret_cast<void**>(&g_real_CreateProcessA)},
        {"CreateProcessW", reinterpret_cast<void*>(&Hook_CreateProcessW),
         reinterpret_cast<void**>(&g_real_CreateProcessW)},
        {"ExitProcess", reinterpret_cast<void*>(&Hook_ExitProcess),
         reinterpret_cast<void**>(&g_real_ExitProcess)},
        {"OutputDebugStringA", reinterpret_cast<void*>(&Hook_OutputDebugStringA),
         reinterpret_cast<void**>(&g_real_OutputDebugStringA)},
        {"NtCreateFile", reinterpret_cast<void*>(&Hook_NtCreateFile),
         reinterpret_cast<void**>(&g_real_NtCreateFile)},
        {"NtOpenFile", reinterpret_cast<void*>(&Hook_NtOpenFile),
         reinterpret_cast<void**>(&g_real_NtOpenFile)},
        {"NtQueryAttributesFile", reinterpret_cast<void*>(&Hook_NtQueryAttributesFile),
         reinterpret_cast<void**>(&g_real_NtQueryAttributesFile)},
    };
    for (const auto& b : kBindings) {
        if (std::strcmp(name, b.symbol_name) == 0) {
            return (*b.real_slot != nullptr) ? b.replacement : nullptr;
        }
    }
    return nullptr;
}

void* ResolveHookForRealAddress(ULONGLONG addr) {
    if (addr == 0) {
        return nullptr;
    }
    auto match = [addr](auto real_fn, auto hook_fn) -> void* {
        if (real_fn != nullptr && reinterpret_cast<ULONGLONG>(real_fn) == addr) {
            return reinterpret_cast<void*>(hook_fn);
        }
        return nullptr;
    };
    if (void* p = match(g_real_LoadLibraryA, &Hook_LoadLibraryA)) return p;
    if (void* p = match(g_real_LoadLibraryW, &Hook_LoadLibraryW)) return p;
    if (void* p = match(g_real_LoadLibraryExA, &Hook_LoadLibraryExA)) return p;
    if (void* p = match(g_real_LoadLibraryExW, &Hook_LoadLibraryExW)) return p;
    if (void* p = match(g_real_GetModuleHandleA, &Hook_GetModuleHandleA)) return p;
    if (void* p = match(g_real_GetModuleHandleW, &Hook_GetModuleHandleW)) return p;
    if (void* p = match(g_real_GetModuleHandleExA, &Hook_GetModuleHandleExA)) return p;
    if (void* p = match(g_real_GetModuleHandleExW, &Hook_GetModuleHandleExW)) return p;
    if (void* p = match(g_real_GetProcAddress, &Hook_GetProcAddress)) return p;
    if (void* p = match(g_real_CreateFileA, &Hook_CreateFileA)) return p;
    if (void* p = match(g_real_CreateFileW, &Hook_CreateFileW)) return p;
    if (void* p = match(g_real_GetFileAttributesA, &Hook_GetFileAttributesA)) return p;
    if (void* p = match(g_real_GetFileAttributesW, &Hook_GetFileAttributesW)) return p;
    if (void* p = match(g_real_GetFileAttributesExW, &Hook_GetFileAttributesExW)) return p;
    if (void* p = match(g_real_CreateDirectoryW, &Hook_CreateDirectoryW)) return p;
    if (void* p = match(g_real_DeleteFileA, &Hook_DeleteFileA)) return p;
    if (void* p = match(g_real_DeleteFileW, &Hook_DeleteFileW)) return p;
    if (void* p = match(g_real_MoveFileExW, &Hook_MoveFileExW)) return p;
    if (void* p = match(g_real_RemoveDirectoryW, &Hook_RemoveDirectoryW)) return p;
    if (void* p = match(g_real_FindFirstFileW, &Hook_FindFirstFileW)) return p;
    if (void* p = match(g_real_CreateProcessA, &Hook_CreateProcessA)) return p;
    if (void* p = match(g_real_CreateProcessW, &Hook_CreateProcessW)) return p;
    if (void* p = match(g_kernelbase_CreateProcessA, &Hook_CreateProcessA)) return p;
    if (void* p = match(g_kernelbase_CreateProcessW, &Hook_CreateProcessW)) return p;
    if (void* p = match(g_real_ExitProcess, &Hook_ExitProcess)) return p;
    if (void* p = match(g_real_OutputDebugStringA, &Hook_OutputDebugStringA)) return p;
    if (void* p = match(g_real_NtCreateFile, &Hook_NtCreateFile)) return p;
    if (void* p = match(g_real_NtOpenFile, &Hook_NtOpenFile)) return p;
    if (void* p = match(g_real_NtQueryAttributesFile, &Hook_NtQueryAttributesFile)) return p;
    return nullptr;
}

bool WriteIatSlot(ULONGLONG* slot, void* replacement) {
    if (!slot || !replacement) {
        return false;
    }
    const ULONGLONG target = reinterpret_cast<ULONGLONG>(replacement);
    if (*slot == target) {
        return true;
    }
    DWORD old_prot = 0;
    if (!::VirtualProtect(slot, sizeof(ULONGLONG), PAGE_READWRITE, &old_prot)) {
        return false;
    }
    *slot = target;
    DWORD dummy = 0;
    ::VirtualProtect(slot, sizeof(ULONGLONG), old_prot, &dummy);
    return true;
}

void PatchThunkTable(uint8_t* base,
                     uint32_t image_size,
                     uint32_t int_rva,
                     uint32_t iat_rva) {
    if (iat_rva == 0 || iat_rva >= image_size) {
        return;
    }
    auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + iat_rva);
    auto* orig = (int_rva != 0 && int_rva < image_size)
                     ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + int_rva)
                     : nullptr;

    for (size_t i = 0; i < 16384; ++i) {
        if (iat_rva + (i + 1) * sizeof(IMAGE_THUNK_DATA64) > image_size) {
            break;
        }
        if (iat[i].u1.Function == 0) {
            break;
        }

        void* replacement = nullptr;
        if (orig != nullptr && int_rva + (i + 1) * sizeof(IMAGE_THUNK_DATA64) <= image_size) {
            const ULONGLONG ord_val = orig[i].u1.Ordinal;
            if (ord_val == 0) {
                break;
            }
            if ((ord_val & IMAGE_ORDINAL_FLAG64) == 0) {
                const uint32_t name_rva = static_cast<uint32_t>(orig[i].u1.AddressOfData);
                if (name_rva + sizeof(WORD) + 1 < image_size) {
                    const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_rva);
                    replacement = ResolveHookForSymbol(ibn->Name);
                }
            }
        }
        if (!replacement) {
            replacement = ResolveHookForRealAddress(iat[i].u1.Function);
        }
        if (replacement) {
            WriteIatSlot(&iat[i].u1.Function, replacement);
        }
    }
}

void InspectModuleTlsAndPdata(HMODULE mod,
                              const uint8_t* base,
                              const IMAGE_NT_HEADERS64* nt,
                              uint32_t image_size) {
    // 1. Inspect TLS callbacks for telemetry visibility.
    std::vector<uint32_t> tls_rvas;
    if (openscarlett::ReadMappedPeTlsCallbackRvas(mod, tls_rvas) && !tls_rvas.empty()) {
        openscarlett::CallTelemetry::Global().Record(
            "loader", "tls_callbacks_inspected", openscarlett::CallOutcome::Ok,
            static_cast<int32_t>(tls_rvas.size()), -1);
    }

    // 2. Verify x64 .pdata SEH registration; register via RtlAddFunctionTable if missing
    //    (e.g. for custom-mapped PE modules).
#if defined(_M_X64) || defined(__x86_64__)
    const auto& exc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exc_dir.VirtualAddress != 0 && exc_dir.Size >= sizeof(RUNTIME_FUNCTION) &&
        exc_dir.VirtualAddress < image_size &&
        exc_dir.Size <= image_size - exc_dir.VirtualAddress) {
        const auto* first_rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + exc_dir.VirtualAddress);
        if (first_rf->BeginAddress != 0 && first_rf->BeginAddress < image_size) {
            DWORD64 img_base_out = 0;
            const DWORD64 probe_pc = reinterpret_cast<DWORD64>(base + first_rf->BeginAddress);
            if (::RtlLookupFunctionEntry(probe_pc, &img_base_out, nullptr) == nullptr) {
                if (openscarlett::RegisterPeExceptionTable(
                        mod, exc_dir.VirtualAddress, exc_dir.Size)) {
                    const uint32_t entries = exc_dir.Size / static_cast<uint32_t>(sizeof(RUNTIME_FUNCTION));
                    openscarlett::CallTelemetry::Global().Record(
                        "loader", "pdata_registered", openscarlett::CallOutcome::Ok,
                        static_cast<int32_t>(entries), -1);
                }
            }
        }
    }
#else
    (void)base;
    (void)nt;
    (void)image_size;
#endif
}

bool IsModuleInShimDir(HMODULE mod) {
    if (!mod || g_shim_dir_w.empty()) {
        return false;
    }
    std::wstring path = GetModulePathW(mod);
    if (path.empty()) {
        return false;
    }
    std::wstring path_lo = ToLowerAsciiW(path);
    std::wstring shim_lo = ToLowerAsciiW(g_shim_dir_w);
    for (wchar_t& c : path_lo) {
        if (c == L'/') c = L'\\';
    }
    for (wchar_t& c : shim_lo) {
        if (c == L'/') c = L'\\';
    }
    if (!shim_lo.empty() && shim_lo.back() != L'\\') {
        shim_lo.push_back(L'\\');
    }
    return path_lo.rfind(shim_lo, 0) == 0;
}

uint32_t RebindShimIatTable(uint8_t* base,
                            uint32_t image_size,
                            uint32_t int_rva,
                            uint32_t iat_rva,
                            HMODULE shim_mod) {
    if (!shim_mod || !g_real_GetProcAddress || iat_rva == 0 || iat_rva >= image_size) {
        return 0;
    }
    if (int_rva == 0 || int_rva >= image_size) {
        return 0;
    }
    auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + iat_rva);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + int_rva);
    uint32_t patched = 0;

    for (size_t i = 0; i < 16384; ++i) {
        if (iat_rva + (i + 1) * sizeof(IMAGE_THUNK_DATA64) > image_size ||
            int_rva + (i + 1) * sizeof(IMAGE_THUNK_DATA64) > image_size) {
            break;
        }
        const ULONGLONG ord_val = orig[i].u1.Ordinal;
        if (ord_val == 0 || iat[i].u1.Function == 0) {
            break;
        }
        LPCSTR proc_key = nullptr;
        if ((ord_val & IMAGE_ORDINAL_FLAG64) != 0) {
            proc_key = reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ord_val & 0xFFFFu));
        } else {
            const uint32_t name_rva = static_cast<uint32_t>(orig[i].u1.AddressOfData);
            if (name_rva + sizeof(WORD) + 1 >= image_size) {
                continue;
            }
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_rva);
            proc_key = ibn->Name;
        }
        FARPROC shim_proc = g_real_GetProcAddress(shim_mod, proc_key);
        if (shim_proc != nullptr) {
            if (WriteIatSlot(&iat[i].u1.Function, reinterpret_cast<void*>(shim_proc))) {
                ++patched;
            }
        }
    }
    return patched;
}

void PatchModuleImports(HMODULE mod) {
    if (!mod || IsWindowsSystemModule(mod)) {
        return;
    }

    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return;
    }

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }

    const uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    const uint32_t timestamp = nt->FileHeader.TimeDateStamp;
    const bool is_shim_module = IsModuleInShimDir(mod);

    struct DeferredShimIat {
        openscarlett::DllRouteDecision decision;
        uint32_t int_rva = 0;
        uint32_t iat_rva = 0;
    };
    std::vector<DeferredShimIat> deferred_shim_iats;

    ::AcquireSRWLockExclusive(&g_patch_lock);
    auto it = g_patched_modules.find(mod);
    if (it != g_patched_modules.end() &&
        it->second.size_of_image == image_size &&
        it->second.timestamp == timestamp) {
        ::ReleaseSRWLockExclusive(&g_patch_lock);
        return;
    }

    // 1. Standard Import Address Table (IMAGE_DIRECTORY_ENTRY_IMPORT)
    const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.VirtualAddress != 0 && imp_dir.VirtualAddress < image_size) {
        auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
        bool module_has_console_exclusive = false;
        if (!is_shim_module) {
            for (size_t idx = 0; idx < 2048; ++idx) {
                if (imp_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) > image_size) {
                    break;
                }
                const auto& d = desc[idx];
                if (d.OriginalFirstThunk == 0 && d.Name == 0 && d.FirstThunk == 0) {
                    break;
                }
                if (d.Name == 0 || d.Name >= image_size) {
                    continue;
                }
                const char* dll_name = reinterpret_cast<const char*>(base + d.Name);
                if (openscarlett::ClassifyImportDll(dll_name) ==
                    openscarlett::DllCategory::ConsoleExclusive) {
                    module_has_console_exclusive = true;
                    break;
                }
            }
        }
        for (size_t idx = 0; idx < 2048; ++idx) {
            if (imp_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) > image_size) {
                break;
            }
            const auto& d = desc[idx];
            if (d.OriginalFirstThunk == 0 && d.Name == 0 && d.FirstThunk == 0) {
                break;
            }
            if (d.Name == 0 || d.Name >= image_size) {
                continue;
            }
            const char* dll_name = reinterpret_cast<const char*>(base + d.Name);
            if (IsHookableImportDll(dll_name)) {
                PatchThunkTable(base, image_size, d.OriginalFirstThunk, d.FirstThunk);
            } else if (!is_shim_module) {
                auto decision = g_routing_table.ResolveRoute(
                    dll_name, g_shim_dir_w, g_guest_dir_w, module_has_console_exclusive);
                if (decision.policy != openscarlett::DllRoutePolicy::Shim) {
                    std::string canon_dll;
                    openscarlett::ForwarderResolutionKind fkind =
                        openscarlett::ForwarderResolutionKind::Unresolved;
                    bool via_api_set = false;
                    bool via_alias = false;
                    if (g_pe_forwarder.ResolveTargetDll(dll_name, canon_dll, fkind, via_api_set, via_alias) &&
                        fkind == openscarlett::ForwarderResolutionKind::OpenScarlettShim) {
                        decision = g_routing_table.ResolveRoute(
                            canon_dll, g_shim_dir_w, g_guest_dir_w, module_has_console_exclusive);
                        decision.requested_name = openscarlett::NormalizeForwarderDllName(dll_name, true);
                        decision.aliased = true;
                    }
                }
                if (decision.policy == openscarlett::DllRoutePolicy::Shim &&
                    !decision.resolved_path.empty()) {
                    deferred_shim_iats.push_back(
                        DeferredShimIat{decision, d.OriginalFirstThunk, d.FirstThunk});
                }
            }
        }
    }

    // 2. Delay-Load Import Table (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
    const auto& dly_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dly_dir.VirtualAddress != 0 && dly_dir.VirtualAddress < image_size) {
        auto* desc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dly_dir.VirtualAddress);
        for (size_t idx = 0; idx < 1024; ++idx) {
            if (dly_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) > image_size) {
                break;
            }
            const auto& d = desc[idx];
            if (d.DllNameRVA == 0 && d.ImportAddressTableRVA == 0) {
                break;
            }
            if (d.Attributes.RvaBased == 0) {
                continue;
            }
            if (d.DllNameRVA == 0 || d.DllNameRVA >= image_size) {
                continue;
            }
            const char* dll_name = reinterpret_cast<const char*>(base + d.DllNameRVA);
            if (!IsHookableImportDll(dll_name)) {
                continue;
            }
            PatchThunkTable(base, image_size, d.ImportNameTableRVA, d.ImportAddressTableRVA);
        }
    }

    g_patched_modules[mod] = PatchedModuleKey{mod, image_size, timestamp};
    ::ReleaseSRWLockExclusive(&g_patch_lock);

    // 3. Rebind static IAT slots for routed/aliased Xbox shims (outside g_patch_lock).
    for (const auto& entry : deferred_shim_iats) {
        HMODULE shim_mod = g_real_GetModuleHandleW
                               ? g_real_GetModuleHandleW(entry.decision.resolved_path.c_str())
                               : nullptr;
        if (!shim_mod && g_initialized.load(std::memory_order_acquire)) {
            shim_mod = LoadRoutedDll(entry.decision, entry.decision.resolved_path,
                                     "IAT:route", nullptr, 0, false);
        }
        if (shim_mod != nullptr) {
            const uint32_t patched = RebindShimIatTable(
                base, image_size, entry.int_rva, entry.iat_rva, shim_mod);
            if (patched > 0 && entry.decision.aliased && !entry.decision.requested_name.empty() &&
                !g_shutting_down.load(std::memory_order_acquire)) {
                openscarlett::CallTelemetry::Global().Record(
                    "loader",
                    std::string("iat_alias:") + entry.decision.requested_name + "->" +
                        entry.decision.canonical_name,
                    openscarlett::CallOutcome::Ok,
                    static_cast<int32_t>(patched),
                    -1);
            }
        }
    }

    InspectModuleTlsAndPdata(mod, base, nt, image_size);
}

void PatchAllLoadedNonSystemModules() {
    std::vector<HMODULE> mods(256);
    DWORD bytes_needed = 0;
    if (::EnumProcessModules(::GetCurrentProcess(),
                             mods.data(),
                             static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                             &bytes_needed)) {
        size_t count = bytes_needed / sizeof(HMODULE);
        if (count > mods.size()) {
            mods.resize(count);
            if (::EnumProcessModules(::GetCurrentProcess(),
                                     mods.data(),
                                     static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                                     &bytes_needed)) {
                count = std::min(mods.size(), static_cast<size_t>(bytes_needed / sizeof(HMODULE)));
            } else {
                count = 0;
            }
        }
        mods.resize(count);
    } else {
        mods.clear();
    }

    HMODULE main_exe = g_real_GetModuleHandleW ? g_real_GetModuleHandleW(nullptr) : ::GetModuleHandleW(nullptr);
    if (main_exe != nullptr && std::find(mods.begin(), mods.end(), main_exe) == mods.end()) {
        mods.push_back(main_exe);
    }

    for (HMODULE mod : mods) {
        if (!mod || mod == g_self_module) {
            continue;
        }
        std::wstring path = GetModulePathW(mod);
        std::string canon_a;
        if (MatchTrackedShimNameW(path, canon_a)) {
            RegisterXboxModule(mod, canon_a);
        }
        PatchModuleImports(mod);
    }
}

bool InitializeHookRuntime(HMODULE self_mod) {
    g_self_module = self_mod;

    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        return false;
    }
    g_kernel32_mod = k32;
    g_kernelbase_mod = ::GetModuleHandleW(L"kernelbase.dll");
    g_ntdll_mod = ::GetModuleHandleW(L"ntdll.dll");

    g_real_LoadLibraryA = reinterpret_cast<PfnLoadLibraryA>(::GetProcAddress(k32, "LoadLibraryA"));
    g_real_LoadLibraryW = reinterpret_cast<PfnLoadLibraryW>(::GetProcAddress(k32, "LoadLibraryW"));
    g_real_LoadLibraryExA = reinterpret_cast<PfnLoadLibraryExA>(::GetProcAddress(k32, "LoadLibraryExA"));
    g_real_LoadLibraryExW = reinterpret_cast<PfnLoadLibraryExW>(::GetProcAddress(k32, "LoadLibraryExW"));
    g_real_GetModuleHandleA = reinterpret_cast<PfnGetModuleHandleA>(::GetProcAddress(k32, "GetModuleHandleA"));
    g_real_GetModuleHandleW = reinterpret_cast<PfnGetModuleHandleW>(::GetProcAddress(k32, "GetModuleHandleW"));
    g_real_GetModuleHandleExA = reinterpret_cast<PfnGetModuleHandleExA>(::GetProcAddress(k32, "GetModuleHandleExA"));
    g_real_GetModuleHandleExW = reinterpret_cast<PfnGetModuleHandleExW>(::GetProcAddress(k32, "GetModuleHandleExW"));
    g_real_GetProcAddress = reinterpret_cast<PfnGetProcAddress>(::GetProcAddress(k32, "GetProcAddress"));
    g_real_CreateFileA = reinterpret_cast<PfnCreateFileA>(::GetProcAddress(k32, "CreateFileA"));
    g_real_CreateFileW = reinterpret_cast<PfnCreateFileW>(::GetProcAddress(k32, "CreateFileW"));
    g_real_GetFileAttributesA = reinterpret_cast<PfnGetFileAttributesA>(::GetProcAddress(k32, "GetFileAttributesA"));
    g_real_GetFileAttributesW = reinterpret_cast<PfnGetFileAttributesW>(::GetProcAddress(k32, "GetFileAttributesW"));
    g_real_GetFileAttributesExW = reinterpret_cast<PfnGetFileAttributesExW>(::GetProcAddress(k32, "GetFileAttributesExW"));
    g_real_CreateDirectoryW = reinterpret_cast<PfnCreateDirectoryW>(::GetProcAddress(k32, "CreateDirectoryW"));
    g_real_DeleteFileA = reinterpret_cast<PfnDeleteFileA>(::GetProcAddress(k32, "DeleteFileA"));
    g_real_DeleteFileW = reinterpret_cast<PfnDeleteFileW>(::GetProcAddress(k32, "DeleteFileW"));
    g_real_MoveFileExW = reinterpret_cast<PfnMoveFileExW>(::GetProcAddress(k32, "MoveFileExW"));
    g_real_RemoveDirectoryW = reinterpret_cast<PfnRemoveDirectoryW>(::GetProcAddress(k32, "RemoveDirectoryW"));
    g_real_FindFirstFileW = reinterpret_cast<PfnFindFirstFileW>(::GetProcAddress(k32, "FindFirstFileW"));
    g_real_CreateProcessA = reinterpret_cast<PfnCreateProcessA>(::GetProcAddress(k32, "CreateProcessA"));
    g_real_CreateProcessW = reinterpret_cast<PfnCreateProcessW>(::GetProcAddress(k32, "CreateProcessW"));
    if (g_kernelbase_mod != nullptr) {
        g_kernelbase_CreateProcessA =
            reinterpret_cast<PfnCreateProcessA>(::GetProcAddress(g_kernelbase_mod, "CreateProcessA"));
        g_kernelbase_CreateProcessW =
            reinterpret_cast<PfnCreateProcessW>(::GetProcAddress(g_kernelbase_mod, "CreateProcessW"));
    }
    g_real_ExitProcess = reinterpret_cast<PfnExitProcess>(::GetProcAddress(k32, "ExitProcess"));
    g_real_OutputDebugStringA =
        reinterpret_cast<PfnOutputDebugStringA>(::GetProcAddress(k32, "OutputDebugStringA"));
    g_trace_ods = (ReadEnvW(L"OPENSCARLETT_TRACE_ODS") == L"1");
    if (g_ntdll_mod != nullptr) {
        g_real_NtCreateFile =
            reinterpret_cast<PfnNtCreateFile>(::GetProcAddress(g_ntdll_mod, "NtCreateFile"));
        g_real_NtOpenFile =
            reinterpret_cast<PfnNtOpenFile>(::GetProcAddress(g_ntdll_mod, "NtOpenFile"));
        g_real_NtQueryAttributesFile = reinterpret_cast<PfnNtQueryAttributesFile>(
            ::GetProcAddress(g_ntdll_mod, "NtQueryAttributesFile"));
    }

    // Resolve %SystemRoot% for Windows system DLL exclusion
    wchar_t win_dir[MAX_PATH]{};
    UINT win_len = ::GetSystemWindowsDirectoryW(win_dir, MAX_PATH);
    if (win_len > 0 && win_len < MAX_PATH) {
        g_windows_dir_lower_w = ToLowerAsciiW(std::wstring_view(win_dir, win_len));
        while (!g_windows_dir_lower_w.empty() && g_windows_dir_lower_w.back() == L'\\') {
            g_windows_dir_lower_w.pop_back();
        }
    }

    // Parse OpenScarlett environment variables
    const std::wstring self_path = GetModulePathW(self_mod);
    g_hook_dll_path_w = ReadEnvW(L"OPENSCARLETT_HOOK_DLL");
    if (g_hook_dll_path_w.empty()) {
        g_hook_dll_path_w = self_path;
    }

    g_shim_dir_w = ReadEnvW(L"OPENSCARLETT_SHIM_DIR");
    if (g_shim_dir_w.empty()) {
        g_shim_dir_w = std::filesystem::path(self_path).parent_path().wstring();
    }
    while (g_shim_dir_w.size() > 3 && (g_shim_dir_w.back() == L'\\' || g_shim_dir_w.back() == L'/')) {
        g_shim_dir_w.pop_back();
    }

    const std::wstring tel_w = ReadEnvW(L"OPENSCARLETT_TELEMETRY_PATH");
    const std::wstring tel_base_w = ReadEnvW(L"OPENSCARLETT_TELEMETRY_BASE");
    g_telemetry_base_w = !tel_base_w.empty() ? tel_base_w : tel_w;

    const std::wstring child_per_pid = ReadEnvW(L"OPENSCARLETT_CHILD_TELEMETRY_PER_PID");
    if (child_per_pid == L"1" && !g_telemetry_base_w.empty()) {
        const std::wstring child_tel_w =
            openscarlett::FormatChildTelemetryPathW(g_telemetry_base_w, ::GetCurrentProcessId());
        if (!child_tel_w.empty()) {
            g_telemetry_path_utf8 = openscarlett::WideToUtf8(child_tel_w);
            ::SetEnvironmentVariableW(L"OPENSCARLETT_TELEMETRY_PATH", child_tel_w.c_str());
            ::SetEnvironmentVariableW(L"OPENSCARLETT_CHILD_TELEMETRY_PER_PID", nullptr);
            openscarlett::CallTelemetry::Global().Record(
                "loader",
                "child_hook_attached",
                openscarlett::CallOutcome::Ok,
                static_cast<int32_t>(::GetCurrentProcessId()),
                -1);
        }
    } else if (!tel_w.empty()) {
        g_telemetry_path_utf8 = openscarlett::WideToUtf8(tel_w);
    }

    // Build unified data-driven DLL routing table & PeForwarderResolver honoring env overrides.
    g_routing_table = openscarlett::DllRoutingTable::CreateWithEnvOverrides();
    g_pe_forwarder = openscarlett::PeForwarderResolver(g_routing_table);

    std::wstring main_exe_path = GetModulePathW(nullptr);
    std::wstring main_exe_dir = std::filesystem::path(main_exe_path).parent_path().wstring();
    const std::wstring default_vfs_root = openscarlett::ResolveDefaultVfsRootW(g_shim_dir_w);
    g_vfs.Configure(openscarlett::VfsMounts::BuildDefaultConfig(main_exe_dir, default_vfs_root));
    g_guest_dir_w = g_vfs.GetGameRoot().empty() ? main_exe_dir : g_vfs.GetGameRoot();

    // Register package-local guest DLLs with PeForwarderResolver.
    if (!g_guest_dir_w.empty()) {
        std::error_code lec;
        for (const auto& entry : std::filesystem::directory_iterator(g_guest_dir_w, lec)) {
            if (!entry.is_regular_file(lec)) continue;
            if (ToLowerAsciiW(entry.path().extension().wstring()) == L".dll") {
                g_pe_forwarder.RegisterNativeGuestDll(entry.path().filename().string());
            }
        }
    }

    // Wire ConsoleLsaPolicy LIVE from MicrosoftGame.Config (if present) + environment overrides.
    {
        auto& lsa = openscarlett::ConsoleLsaPolicy::Global();
        std::string config_path_u8;
        const std::wstring env_cfg_w = ReadEnvW(L"OPENSCARLETT_GAME_CONFIG");
        if (!env_cfg_w.empty()) {
            config_path_u8 = openscarlett::WideToUtf8(env_cfg_w);
        } else {
            openscarlett::ResolvedPackageTarget pkg_target{};
            if (!g_guest_dir_w.empty() &&
                openscarlett::ResolvePackageLaunchTarget(openscarlett::WideToUtf8(g_guest_dir_w), pkg_target).ok() &&
                pkg_target.has_config && !pkg_target.config_path.empty()) {
                config_path_u8 = pkg_target.config_path;
            } else if (!main_exe_path.empty() &&
                       openscarlett::ResolvePackageLaunchTarget(openscarlett::WideToUtf8(main_exe_path), pkg_target).ok() &&
                       pkg_target.has_config && !pkg_target.config_path.empty()) {
                config_path_u8 = pkg_target.config_path;
            }
        }
        bool lsa_loaded = false;
        if (!config_path_u8.empty()) {
            lsa_loaded = lsa.LoadFromMicrosoftGameConfigFile(config_path_u8).ok();
        }
        const std::wstring env_sandbox_w = ReadEnvW(L"OPENSCARLETT_SANDBOX_ID");
        if (!env_sandbox_w.empty()) {
            lsa.SetSandboxId(openscarlett::WideToUtf8(env_sandbox_w));
        }
        const std::wstring env_store_w = ReadEnvW(L"OPENSCARLETT_STORE_ID");
        if (!env_store_w.empty()) {
            lsa.SetStoreId(openscarlett::WideToUtf8(env_store_w));
        }
        const std::wstring env_content_w = ReadEnvW(L"OPENSCARLETT_CONTENT_ID");
        if (!env_content_w.empty()) {
            lsa.SetContentId(openscarlett::WideToUtf8(env_content_w));
        }
        openscarlett::CallTelemetry::Global().Record(
            "loader",
            "console_lsa_policy_initialized",
            openscarlett::CallOutcome::Ok,
            lsa_loaded ? 1 : 0,
            -1);
    }

    g_probe_unknown_imports = openscarlett::IsProbeUnknownImportsEnabledByEnv();
    if (g_probe_unknown_imports) {
        g_veh_monitor.SetTrampolinePool(&g_probe_trampoline_pool);
        if (!g_telemetry_path_utf8.empty()) {
            const std::string veh_json = g_telemetry_path_utf8 + ".veh_bringup.json";
            g_veh_monitor.SetAutoExportPath(veh_json, {openscarlett::WideToUtf8(g_shim_dir_w)});
        }
        (void)g_veh_monitor.Install(1);
    }

    PatchAllLoadedNonSystemModules();
    g_initialized.store(true, std::memory_order_release);
    FlushTelemetryInternal(false);
    return true;
}

}  // namespace

extern "C" {

__declspec(dllexport) void __cdecl OpenScarlettHook_RecordCall(const char* module_name,
                                                               const char* symbol_name,
                                                               uint8_t outcome,
                                                               int32_t hr,
                                                               int32_t vtable_slot) {
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return;
    }
    const uint8_t clamped = (outcome <= 3u) ? outcome : 3u;
    openscarlett::CallTelemetry::Global().Record(
        module_name ? module_name : "",
        symbol_name ? symbol_name : "",
        static_cast<openscarlett::CallOutcome>(clamped),
        hr,
        vtable_slot);
    if (g_probe_unknown_imports) {
        openscarlett::RecordProbeApiCall(
            module_name ? module_name : "",
            symbol_name ? symbol_name : "",
            0ull,
            hr);
    }
}

__declspec(dllexport) int __cdecl OpenScarlettHook_FlushTelemetry() {
    return FlushTelemetryInternal(false);
}

__declspec(dllexport) uint32_t __cdecl OpenScarlettHook_InvokeModuleTlsCallbacks(HMODULE mod,
                                                                                 DWORD reason) {
    const uint32_t count = openscarlett::InvokePeTlsCallbacks(mod, reason, nullptr);
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        openscarlett::CallTelemetry::Global().Record(
            "loader", "tls_callbacks_invoked", openscarlett::CallOutcome::Ok,
            static_cast<int32_t>(count), -1);
    }
    return count;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_RegisterModuleExceptionTable(HMODULE mod) {
    uint32_t count = 0;
    const bool ok = openscarlett::RegisterPeExceptionTableFromImage(mod, &count);
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        openscarlett::CallTelemetry::Global().Record(
            "loader", "pdata_register_helper",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
            static_cast<int32_t>(count), -1);
    }
    return ok ? 1 : 0;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_LoadDllsTopologically(
    uint32_t count,
    const wchar_t* const* dll_paths,
    char* out_order_csv,
    uint32_t out_order_csv_cap,
    HMODULE* out_handles) {
    if (count == 0u || !dll_paths) {
        return 0;
    }

    std::vector<std::string> paths_u8;
    paths_u8.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!dll_paths[i] || dll_paths[i][0] == L'\0') {
            return 0;
        }
        paths_u8.push_back(openscarlett::WideToUtf8(dll_paths[i]));
    }

    std::vector<std::string> loaded_order;
    std::vector<void*> handles;
    const auto st = openscarlett::LoadDllsInTopologicalOrder(paths_u8, &loaded_order, &handles);
    if (!st.ok()) {
        return 0;
    }

    if (out_handles) {
        for (uint32_t i = 0; i < count && i < handles.size(); ++i) {
            out_handles[i] = reinterpret_cast<HMODULE>(handles[i]);
        }
    }

    for (void* h : handles) {
        if (h) {
            PatchModuleImports(reinterpret_cast<HMODULE>(h));
        }
    }

    if (out_order_csv && out_order_csv_cap > 0u) {
        std::string csv;
        for (size_t i = 0; i < loaded_order.size(); ++i) {
            if (i > 0u) csv.push_back(',');
            csv.append(loaded_order[i]);
        }
        const size_t copy_len = std::min<size_t>(csv.size(), static_cast<size_t>(out_order_csv_cap - 1u));
        std::memcpy(out_order_csv, csv.data(), copy_len);
        out_order_csv[copy_len] = '\0';
    }

    if (!g_shutting_down.load(std::memory_order_acquire)) {
        openscarlett::CallTelemetry::Global().Record(
            "loader", "OpenScarlettHook_LoadDllsTopologically",
            openscarlett::CallOutcome::Ok,
            static_cast<int32_t>(loaded_order.size()),
            -1);
    }
    return 1;
}

__declspec(dllexport) uint32_t __cdecl OpenScarlettHook_GetRoutedDllCount() {
    return static_cast<uint32_t>(g_routing_table.ListCompleteRules().size());
}

__declspec(dllexport) int __cdecl OpenScarlettHook_ResolveKnownDllPathW(
    const wchar_t* dll_name,
    wchar_t* out_path,
    uint32_t out_path_cap) {
    if (!dll_name || !out_path || out_path_cap == 0u) {
        return 0;
    }
    std::wstring resolved;
    if (!TryResolveKnownDllSystem32FirstW(dll_name, resolved)) {
        return 0;
    }
    if (resolved.size() + 1u > static_cast<size_t>(out_path_cap)) {
        return 0;
    }
    std::memcpy(out_path, resolved.c_str(), (resolved.size() + 1u) * sizeof(wchar_t));
    return 1;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_GetTlsSlot0Status(
    OpenScarlettTlsSlot0Status* out_status) {
    if (!out_status) {
        return 0;
    }
    out_status->initialized = g_tls_slot0_initialized.load(std::memory_order_acquire) ? 1u : 0u;
    out_status->owned_by_hook = g_tls_slot0_owned_by_hook.load(std::memory_order_acquire) ? 1u : 0u;
    out_status->already_claimed_by_host =
        g_tls_slot0_already_claimed.load(std::memory_order_acquire) ? 1u : 0u;
    out_status->preserved_ok = g_tls_slot0_preserved_ok.load(std::memory_order_acquire) ? 1u : 0u;
    out_status->saved_value_at_attach = g_tls_slot0_saved_val.load(std::memory_order_acquire);
    return 1;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_ResolveForwarderA(
    const char* forwarder,
    char* out_dll,
    uint32_t out_dll_cap,
    char* out_sym,
    uint32_t out_sym_cap,
    uint32_t* out_kind) {
    if (!forwarder || *forwarder == '\0') {
        return 0;
    }
    const auto res = g_pe_forwarder.ResolveForwarder(forwarder);
    if (out_kind) {
        *out_kind = static_cast<uint32_t>(res.kind);
    }
    if (out_dll && out_dll_cap > 0u) {
        const size_t n = std::min<size_t>(res.resolved_dll.size(), static_cast<size_t>(out_dll_cap - 1u));
        std::memcpy(out_dll, res.resolved_dll.data(), n);
        out_dll[n] = '\0';
    }
    if (out_sym && out_sym_cap > 0u) {
        const size_t n = std::min<size_t>(res.resolved_symbol.size(), static_cast<size_t>(out_sym_cap - 1u));
        std::memcpy(out_sym, res.resolved_symbol.data(), n);
        out_sym[n] = '\0';
    }
    return res.ok ? 1 : 0;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_ReloadConsoleLsaFromConfigA(
    const char* config_path,
    char* out_pfn,
    uint32_t out_pfn_cap,
    uint32_t* out_title_id) {
    auto& lsa = openscarlett::ConsoleLsaPolicy::Global();
    if (config_path && *config_path != '\0') {
        if (!lsa.LoadFromMicrosoftGameConfigFile(config_path).ok()) {
            return 0;
        }
    }
    const auto pkg = lsa.GetPackageIdentity();
    if (out_title_id) {
        *out_title_id = pkg.title_id;
    }
    if (out_pfn && out_pfn_cap > 0u) {
        const size_t n = std::min<size_t>(pkg.package_family_name.size(),
                                          static_cast<size_t>(out_pfn_cap - 1u));
        std::memcpy(out_pfn, pkg.package_family_name.data(), n);
        out_pfn[n] = '\0';
    }
    return 1;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_ConfigureVfsForTestW(
    const wchar_t* g_root,
    const wchar_t* g_overlay,
    const wchar_t* t_root,
    const wchar_t* s_root) {
    openscarlett::VfsMountConfig cfg{};
    if (g_root) cfg.g_root = g_root;
    if (g_overlay) cfg.g_overlay_root = g_overlay;
    if (t_root) cfg.t_root = t_root;
    if (s_root) cfg.s_root = s_root;
    g_vfs.Configure(cfg);
    return 1;
}

__declspec(dllexport) int __cdecl OpenScarlettHook_AddVfsSubMountW(
    const wchar_t* virtual_subpath,
    const wchar_t* host_ro_root) {
    if (!virtual_subpath || !host_ro_root) {
        return 0;
    }
    return g_vfs.AddGameSubMount(virtual_subpath, host_ro_root) ? 1 : 0;
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtCreateFile(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    OS_OBJECT_ATTRIBUTES* ObjectAttributes,
    OS_IO_STATUS_BLOCK* IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength) {
    return Hook_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                             AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                             CreateOptions, EaBuffer, EaLength);
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtOpenFile(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    OS_OBJECT_ATTRIBUTES* ObjectAttributes,
    OS_IO_STATUS_BLOCK* IoStatusBlock,
    ULONG ShareAccess,
    ULONG OpenOptions) {
    return Hook_NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                           ShareAccess, OpenOptions);
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtQueryAttributesFile(
    OS_OBJECT_ATTRIBUTES* ObjectAttributes,
    OS_FILE_BASIC_INFORMATION* FileInformation) {
    return Hook_NtQueryAttributesFile(ObjectAttributes, FileInformation);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            ::DisableThreadLibraryCalls(hinstDLL);
            // Reserve & preserve TLS slot 0 BEFORE any runtime initialization touches TLS,
            // preventing UE5 / real title TLS slot 0 collisions during APC hook attach.
            const DWORD prev_last_err = ::GetLastError();
            LPVOID saved_slot0 = ::TlsGetValue(0u);
            const DWORD slot0_get_err = ::GetLastError();
            const DWORD probe_idx = ::TlsAlloc();
            bool owned_slot0 = false;
            if (probe_idx == 0u) {
                owned_slot0 = true;
                g_tls_slot0_owned_by_hook.store(true, std::memory_order_release);
                g_tls_slot0_already_claimed.store(false, std::memory_order_release);
                ::TlsSetValue(0u, saved_slot0);
            } else {
                if (probe_idx != TLS_OUT_OF_INDEXES) {
                    ::TlsFree(probe_idx);
                }
                g_tls_slot0_owned_by_hook.store(false, std::memory_order_release);
                g_tls_slot0_already_claimed.store(true, std::memory_order_release);
            }
            g_tls_slot0_saved_val.store(reinterpret_cast<uintptr_t>(saved_slot0),
                                        std::memory_order_release);

            InitializeHookRuntime(hinstDLL);

            // Guarantee TLS slot 0 was never clobbered during hook initialization.
            LPVOID post_slot0 = ::TlsGetValue(0u);
            if (slot0_get_err == ERROR_SUCCESS && post_slot0 != saved_slot0) {
                ::TlsSetValue(0u, saved_slot0);
                post_slot0 = ::TlsGetValue(0u);
            }
            const bool preserved_ok = (owned_slot0 || slot0_get_err != ERROR_SUCCESS || post_slot0 == saved_slot0);
            g_tls_slot0_preserved_ok.store(preserved_ok, std::memory_order_release);
            g_tls_slot0_initialized.store(true, std::memory_order_release);
            ::SetEnvironmentVariableW(
                L"OPENSCARLETT_TLS_SLOT0_STATE",
                owned_slot0 ? L"reserved" : L"preserved_host");
            openscarlett::CallTelemetry::Global().Record(
                "loader",
                "tls_slot0_preserved",
                preserved_ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                owned_slot0 ? 0 : 1,
                -1);
            ::SetLastError(prev_last_err);
            return TRUE;
        }
        case DLL_PROCESS_DETACH:
            if (g_probe_unknown_imports) {
                g_veh_monitor.Uninstall();
            }
            FlushTelemetryInternal(true);
            g_shutting_down.store(true, std::memory_order_release);
            if (lpvReserved == nullptr &&
                g_tls_slot0_owned_by_hook.exchange(false, std::memory_order_acq_rel)) {
                ::TlsFree(0u);
            }
            return TRUE;
        default:
            return TRUE;
    }
}
