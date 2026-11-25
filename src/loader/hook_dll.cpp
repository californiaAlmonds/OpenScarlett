#include <windows.h>
#include <winreg.h>
#include <psapi.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
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
#include "spatial_audio_bridge.h"

namespace {

// ---------------------------------------------------------------------------
// Self-Contained NT File API ABI Definitions (no <winternl.h> collisions)
// ---------------------------------------------------------------------------
using OS_NTSTATUS = LONG;
constexpr OS_NTSTATUS OS_STATUS_SUCCESS = 0x00000000L;
constexpr OS_NTSTATUS OS_STATUS_BUFFER_OVERFLOW = static_cast<OS_NTSTATUS>(0x80000005UL);
constexpr OS_NTSTATUS OS_STATUS_NO_MORE_FILES = static_cast<OS_NTSTATUS>(0x80000006UL);
constexpr OS_NTSTATUS OS_STATUS_INVALID_INFO_CLASS = static_cast<OS_NTSTATUS>(0xC0000003UL);
constexpr OS_NTSTATUS OS_STATUS_INFO_LENGTH_MISMATCH = static_cast<OS_NTSTATUS>(0xC0000004UL);
constexpr OS_NTSTATUS OS_STATUS_INVALID_PARAMETER = static_cast<OS_NTSTATUS>(0xC000000DUL);
constexpr OS_NTSTATUS OS_STATUS_NO_SUCH_FILE = static_cast<OS_NTSTATUS>(0xC000000FUL);
constexpr OS_NTSTATUS OS_STATUS_ACCESS_DENIED = static_cast<OS_NTSTATUS>(0xC0000022UL);
constexpr OS_NTSTATUS OS_STATUS_BUFFER_TOO_SMALL = static_cast<OS_NTSTATUS>(0xC0000023UL);
constexpr OS_NTSTATUS OS_STATUS_OBJECT_NAME_NOT_FOUND = static_cast<OS_NTSTATUS>(0xC0000034UL);
constexpr OS_NTSTATUS OS_STATUS_OBJECT_NAME_COLLISION = static_cast<OS_NTSTATUS>(0xC0000035UL);
constexpr OS_NTSTATUS OS_STATUS_OBJECT_PATH_NOT_FOUND = static_cast<OS_NTSTATUS>(0xC000003AUL);
constexpr OS_NTSTATUS OS_STATUS_NOT_A_DIRECTORY = static_cast<OS_NTSTATUS>(0xC0000103UL);

constexpr ULONG OS_FILE_SUPERSEDE = 0x00000000UL;
constexpr ULONG OS_FILE_OPEN = 0x00000001UL;
constexpr ULONG OS_FILE_CREATE = 0x00000002UL;
constexpr ULONG OS_FILE_OPEN_IF = 0x00000003UL;
constexpr ULONG OS_FILE_OVERWRITE = 0x00000004UL;
constexpr ULONG OS_FILE_OVERWRITE_IF = 0x00000005UL;

constexpr ULONG OS_FileDirectoryInformation = 1UL;
constexpr ULONG OS_FileFullDirectoryInformation = 2UL;
constexpr ULONG OS_FileBothDirectoryInformation = 3UL;
constexpr ULONG OS_FileRenameInformation = 10UL;
constexpr ULONG OS_FileNamesInformation = 12UL;
constexpr ULONG OS_FileDispositionInformation = 13UL;
constexpr ULONG OS_FileAllocationInformation = 19UL;
constexpr ULONG OS_FileEndOfFileInformation = 20UL;
constexpr ULONG OS_FileIdBothDirectoryInformation = 37UL;
constexpr ULONG OS_FileIdFullDirectoryInformation = 38UL;
constexpr ULONG OS_FileDispositionInformationEx = 64UL;
constexpr ULONG OS_FileRenameInformationEx = 65UL;

constexpr ULONG OS_SL_RESTART_SCAN = 0x00000001UL;
constexpr ULONG OS_SL_RETURN_SINGLE_ENTRY = 0x00000002UL;
constexpr ULONG OS_SL_NO_CURSOR_UPDATE = 0x00000010UL;

constexpr ULONG OS_FILE_DISPOSITION_DELETE = 0x00000001UL;
constexpr ULONG OS_FILE_RENAME_REPLACE_IF_EXISTS = 0x00000001UL;

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

struct OS_FILE_DIRECTORY_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_DIRECTORY_INFORMATION, FileName) == 64u, "NT ABI mismatch");

struct OS_FILE_FULL_DIR_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_FULL_DIR_INFORMATION, FileName) == 68u, "NT ABI mismatch");

struct OS_FILE_BOTH_DIR_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_BOTH_DIR_INFORMATION, FileName) == 94u, "NT ABI mismatch");

struct OS_FILE_ID_FULL_DIR_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    ULONG Reserved;
    LARGE_INTEGER FileId;
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_ID_FULL_DIR_INFORMATION, FileName) == 80u, "NT ABI mismatch");

struct OS_FILE_ID_BOTH_DIR_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    LARGE_INTEGER FileId;
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_ID_BOTH_DIR_INFORMATION, FileName) == 104u, "NT ABI mismatch");

struct OS_FILE_NAMES_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    ULONG FileNameLength;
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_NAMES_INFORMATION, FileName) == 12u, "NT ABI mismatch");

struct OS_FILE_DISPOSITION_INFORMATION {
    BOOLEAN DeleteFile;
};

struct OS_FILE_DISPOSITION_INFORMATION_EX {
    ULONG Flags;
};

struct OS_FILE_ALLOCATION_INFORMATION {
    LARGE_INTEGER AllocationSize;
};

struct OS_FILE_END_OF_FILE_INFORMATION {
    LARGE_INTEGER EndOfFile;
};

struct OS_FILE_RENAME_INFORMATION {
    union {
        BOOLEAN ReplaceIfExists;
        ULONG Flags;
    };
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
};
static_assert(offsetof(OS_FILE_RENAME_INFORMATION, RootDirectory) == sizeof(void*),
              "NT rename ABI mismatch");

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
using PfnNtClose = OS_NTSTATUS(NTAPI*)(HANDLE);
using PfnNtQueryDirectoryFile = OS_NTSTATUS(NTAPI*)(HANDLE,
                                                    HANDLE,
                                                    PVOID,
                                                    PVOID,
                                                    OS_IO_STATUS_BLOCK*,
                                                    PVOID,
                                                    ULONG,
                                                    ULONG,
                                                    BOOLEAN,
                                                    OS_UNICODE_STRING*,
                                                    BOOLEAN);
using PfnNtQueryDirectoryFileEx = OS_NTSTATUS(NTAPI*)(HANDLE,
                                                      HANDLE,
                                                      PVOID,
                                                      PVOID,
                                                      OS_IO_STATUS_BLOCK*,
                                                      PVOID,
                                                      ULONG,
                                                      ULONG,
                                                      ULONG,
                                                      OS_UNICODE_STRING*);
using PfnNtSetInformationFile = OS_NTSTATUS(NTAPI*)(HANDLE,
                                                    OS_IO_STATUS_BLOCK*,
                                                    PVOID,
                                                    ULONG,
                                                    ULONG);

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
using PfnGetModuleFileNameA = DWORD(WINAPI*)(HMODULE, LPSTR, DWORD);
using PfnGetModuleFileNameW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
using PfnGetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using PfnCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PfnCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
struct OS_CREATEFILE2_EXTENDED_PARAMETERS {
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
};
using PfnCreateFile2 = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, OS_CREATEFILE2_EXTENDED_PARAMETERS*);
using PfnCloseHandle = BOOL(WINAPI*)(HANDLE);
using PfnGetFinalPathNameByHandleW = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
using PfnGetFileAttributesA = DWORD(WINAPI*)(LPCSTR);
using PfnGetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using PfnGetFileAttributesExA = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PfnGetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PfnSetFileAttributesA = BOOL(WINAPI*)(LPCSTR, DWORD);
using PfnSetFileAttributesW = BOOL(WINAPI*)(LPCWSTR, DWORD);
using PfnCreateDirectoryA = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using PfnCreateDirectoryW = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PfnDeleteFileA = BOOL(WINAPI*)(LPCSTR);
using PfnDeleteFileW = BOOL(WINAPI*)(LPCWSTR);
using PfnMoveFileW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using PfnMoveFileExA = BOOL(WINAPI*)(LPCSTR, LPCSTR, DWORD);
using PfnMoveFileExW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using PfnCopyFileA = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);
using PfnCopyFileW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using PfnCopyFile2 = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
using PfnReplaceFileW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using PfnRemoveDirectoryA = BOOL(WINAPI*)(LPCSTR);
using PfnRemoveDirectoryW = BOOL(WINAPI*)(LPCWSTR);
using PfnFindFirstFileA = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using PfnFindFirstFileW = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using PfnFindFirstFileExA = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using PfnFindFirstFileExW = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using PfnGetDiskFreeSpaceExW = BOOL(WINAPI*)(LPCWSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
using PfnGetDiskFreeSpaceW = BOOL(WINAPI*)(LPCWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD);
using PfnSetCurrentDirectoryA = BOOL(WINAPI*)(LPCSTR);
using PfnSetCurrentDirectoryW = BOOL(WINAPI*)(LPCWSTR);
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
using PfnGlobalMemoryStatusEx = BOOL(WINAPI*)(LPMEMORYSTATUSEX);
using PfnRegOpenKeyW = LSTATUS(APIENTRY*)(HKEY, LPCWSTR, PHKEY);
using PfnRegOpenKeyA = LSTATUS(APIENTRY*)(HKEY, LPCSTR, PHKEY);
using PfnRegOpenKeyExW = LSTATUS(APIENTRY*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using PfnRegOpenKeyExA = LSTATUS(APIENTRY*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using PfnRegQueryValueExW = LSTATUS(APIENTRY*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using PfnRegQueryValueExA = LSTATUS(APIENTRY*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using PfnRegGetValueW = LSTATUS(APIENTRY*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using PfnRegGetValueA = LSTATUS(APIENTRY*)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using PfnRegCloseKey = LSTATUS(APIENTRY*)(HKEY);
using PfnWinHttpConnect = LPVOID(WINAPI*)(LPVOID, LPCWSTR, WORD, DWORD);
using PfnCoCreateInstance = HRESULT(STDAPICALLTYPE*)(REFCLSID, LPVOID, DWORD, REFIID, LPVOID*);

PfnLoadLibraryA g_real_LoadLibraryA = nullptr;
PfnLoadLibraryW g_real_LoadLibraryW = nullptr;
PfnLoadLibraryExA g_real_LoadLibraryExA = nullptr;
PfnLoadLibraryExW g_real_LoadLibraryExW = nullptr;
PfnGetModuleHandleA g_real_GetModuleHandleA = nullptr;
PfnGetModuleHandleW g_real_GetModuleHandleW = nullptr;
PfnGetModuleHandleExA g_real_GetModuleHandleExA = nullptr;
PfnGetModuleHandleExW g_real_GetModuleHandleExW = nullptr;
PfnGetModuleFileNameA g_real_GetModuleFileNameA = nullptr;
PfnGetModuleFileNameW g_real_GetModuleFileNameW = nullptr;
PfnGetProcAddress g_real_GetProcAddress = nullptr;
PfnCreateFileA g_real_CreateFileA = nullptr;
PfnCreateFileW g_real_CreateFileW = nullptr;
PfnCreateFile2 g_real_CreateFile2 = nullptr;
PfnCloseHandle g_real_CloseHandle = nullptr;
PfnGetFinalPathNameByHandleW g_real_GetFinalPathNameByHandleW = nullptr;
PfnCreateFile2 g_kernelbase_CreateFile2 = nullptr;
PfnGetFileAttributesA g_real_GetFileAttributesA = nullptr;
PfnGetFileAttributesW g_real_GetFileAttributesW = nullptr;
PfnGetFileAttributesExA g_real_GetFileAttributesExA = nullptr;
PfnGetFileAttributesExW g_real_GetFileAttributesExW = nullptr;
PfnSetFileAttributesA g_real_SetFileAttributesA = nullptr;
PfnSetFileAttributesW g_real_SetFileAttributesW = nullptr;
PfnCreateDirectoryA g_real_CreateDirectoryA = nullptr;
PfnCreateDirectoryW g_real_CreateDirectoryW = nullptr;
PfnDeleteFileA g_real_DeleteFileA = nullptr;
PfnDeleteFileW g_real_DeleteFileW = nullptr;
PfnMoveFileW g_real_MoveFileW = nullptr;
PfnMoveFileExA g_real_MoveFileExA = nullptr;
PfnMoveFileExW g_real_MoveFileExW = nullptr;
PfnCopyFileA g_real_CopyFileA = nullptr;
PfnCopyFileW g_real_CopyFileW = nullptr;
PfnCopyFile2 g_real_CopyFile2 = nullptr;
PfnReplaceFileW g_real_ReplaceFileW = nullptr;
PfnRemoveDirectoryA g_real_RemoveDirectoryA = nullptr;
PfnRemoveDirectoryW g_real_RemoveDirectoryW = nullptr;
PfnFindFirstFileA g_real_FindFirstFileA = nullptr;
PfnFindFirstFileW g_real_FindFirstFileW = nullptr;
PfnFindFirstFileExA g_real_FindFirstFileExA = nullptr;
PfnFindFirstFileExW g_real_FindFirstFileExW = nullptr;
PfnGetDiskFreeSpaceExW g_real_GetDiskFreeSpaceExW = nullptr;
PfnGetDiskFreeSpaceW g_real_GetDiskFreeSpaceW = nullptr;
PfnSetCurrentDirectoryA g_real_SetCurrentDirectoryA = nullptr;
PfnSetCurrentDirectoryW g_real_SetCurrentDirectoryW = nullptr;
PfnCreateProcessA g_real_CreateProcessA = nullptr;
PfnCreateProcessW g_real_CreateProcessW = nullptr;
PfnCreateProcessA g_kernelbase_CreateProcessA = nullptr;
PfnCreateProcessW g_kernelbase_CreateProcessW = nullptr;
PfnExitProcess g_real_ExitProcess = nullptr;
PfnOutputDebugStringA g_real_OutputDebugStringA = nullptr;
PfnGlobalMemoryStatusEx g_real_GlobalMemoryStatusEx = nullptr;
PfnGlobalMemoryStatusEx g_kernelbase_GlobalMemoryStatusEx = nullptr;
PfnRegOpenKeyW g_real_RegOpenKeyW = nullptr;
PfnRegOpenKeyA g_real_RegOpenKeyA = nullptr;
PfnRegOpenKeyExW g_real_RegOpenKeyExW = nullptr;
PfnRegOpenKeyExA g_real_RegOpenKeyExA = nullptr;
PfnRegQueryValueExW g_real_RegQueryValueExW = nullptr;
PfnRegQueryValueExA g_real_RegQueryValueExA = nullptr;
PfnRegGetValueW g_real_RegGetValueW = nullptr;
PfnRegGetValueA g_real_RegGetValueA = nullptr;
PfnRegCloseKey g_real_RegCloseKey = nullptr;
PfnRegOpenKeyExW g_advapi32_RegOpenKeyExW = nullptr;
PfnRegOpenKeyExA g_advapi32_RegOpenKeyExA = nullptr;
PfnRegQueryValueExW g_advapi32_RegQueryValueExW = nullptr;
PfnRegQueryValueExA g_advapi32_RegQueryValueExA = nullptr;
PfnRegGetValueW g_advapi32_RegGetValueW = nullptr;
PfnRegGetValueA g_advapi32_RegGetValueA = nullptr;
PfnRegCloseKey g_advapi32_RegCloseKey = nullptr;
PfnWinHttpConnect g_real_WinHttpConnect = nullptr;
PfnCoCreateInstance g_real_CoCreateInstance = nullptr;
PfnCoCreateInstance g_ole32_CoCreateInstance = nullptr;
void EnsureComProcsResolved() noexcept;
bool g_offline_block_winhttp = false;
bool g_trace_ods = false;
PfnNtCreateFile g_real_NtCreateFile = nullptr;
PfnNtOpenFile g_real_NtOpenFile = nullptr;
PfnNtQueryAttributesFile g_real_NtQueryAttributesFile = nullptr;
PfnNtClose g_real_NtClose = nullptr;
PfnNtQueryDirectoryFile g_real_NtQueryDirectoryFile = nullptr;
PfnNtQueryDirectoryFileEx g_real_NtQueryDirectoryFileEx = nullptr;
PfnNtSetInformationFile g_real_NtSetInformationFile = nullptr;

// ---------------------------------------------------------------------------
// Hook Runtime State
// ---------------------------------------------------------------------------
HMODULE g_self_module = nullptr;
HMODULE g_kernel32_mod = nullptr;
HMODULE g_kernelbase_mod = nullptr;
HMODULE g_ntdll_mod = nullptr;
HMODULE g_advapi32_mod = nullptr;
std::wstring g_shim_dir_w;
std::wstring g_hook_dll_path_w;
std::wstring g_guest_dir_w;
std::wstring g_main_exe_path_w;
std::wstring g_windows_dir_lower_w;
std::wstring g_telemetry_base_w;
std::string g_telemetry_path_utf8;
std::atomic<bool> g_virtualize_module_paths{false};
std::wstring g_virtual_module_drive_w = L"G:";
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
std::unordered_map<HMODULE, PatchedModuleKey> g_patched_crt_vfs_modules;

struct TrackedVfsDirEntry {
    std::wstring name;
    WIN32_FIND_DATAW fd{};
};

struct TrackedVfsHandle {
    std::wstring virtual_path;
    std::wstring host_path;
    openscarlett::VfsDrive drive = openscarlett::VfsDrive::None;
    bool is_overlay = false;
    bool is_directory = false;
    bool dir_enum_initialized = false;
    std::wstring dir_enum_pattern;
    size_t dir_enum_cursor = 0u;
    std::vector<TrackedVfsDirEntry> dir_entries;
};

SRWLOCK g_vfs_handle_lock = SRWLOCK_INIT;
std::unordered_map<HANDLE, TrackedVfsHandle> g_vfs_handles;
std::atomic<uint32_t> g_vfs_handle_count{0u};

thread_local uint32_t tl_forwarder_depth = 0;
thread_local uint32_t tl_vfs_hook_depth = 0;
thread_local bool tl_in_topo_preload = false;
thread_local bool tl_in_create_process_hook = false;

struct ScopedVfsHookGuard {
    ScopedVfsHookGuard() noexcept { ++tl_vfs_hook_depth; }
    ~ScopedVfsHookGuard() { --tl_vfs_hook_depth; }
    ScopedVfsHookGuard(const ScopedVfsHookGuard&) = delete;
    ScopedVfsHookGuard& operator=(const ScopedVfsHookGuard&) = delete;
};

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
        DWORD len = g_real_GetModuleFileNameW
                        ? g_real_GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()))
                        : ::GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
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
        lo == "advapi32.dll" ||
        (g_offline_block_winhttp && lo == "winhttp.dll") ||
        lo.rfind("api-ms-win-core-", 0) == 0) {
        return true;
    }
    if (openscarlett::IsApiSetContractName(lo)) {
        std::string target_dll;
        if (g_pe_forwarder.ResolveApiSetContract(lo, target_dll)) {
            const std::string tlo = ToLowerAsciiA(target_dll);
            return tlo == "kernel32.dll" || tlo == "kernelbase.dll" ||
                   tlo == "ntdll.dll" || tlo == "advapi32.dll";
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
bool IsModuleInShimDir(HMODULE mod);
void EnsureAdvapi32ProcsResolved() noexcept;
void PatchModuleImports(HMODULE mod);
void PatchAllLoadedNonSystemModules();
FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL WINAPI Hook_GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer);
LSTATUS APIENTRY Hook_RegOpenKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult);
LSTATUS APIENTRY Hook_RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult);
LSTATUS APIENTRY Hook_RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LSTATUS APIENTRY Hook_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LSTATUS APIENTRY Hook_RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS APIENTRY Hook_RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS APIENTRY Hook_RegGetValueW(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData);
LSTATUS APIENTRY Hook_RegGetValueA(HKEY hkey, LPCSTR lpSubKey, LPCSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData);
LSTATUS APIENTRY Hook_RegCloseKey(HKEY hKey);
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
OS_NTSTATUS NTAPI Hook_NtClose(HANDLE Handle);
OS_NTSTATUS NTAPI Hook_NtQueryDirectoryFile(HANDLE FileHandle,
                                            HANDLE Event,
                                            PVOID ApcRoutine,
                                            PVOID ApcContext,
                                            OS_IO_STATUS_BLOCK* IoStatusBlock,
                                            PVOID FileInformation,
                                            ULONG Length,
                                            ULONG FileInformationClass,
                                            BOOLEAN ReturnSingleEntry,
                                            OS_UNICODE_STRING* FileName,
                                            BOOLEAN RestartScan);
OS_NTSTATUS NTAPI Hook_NtQueryDirectoryFileEx(HANDLE FileHandle,
                                              HANDLE Event,
                                              PVOID ApcRoutine,
                                              PVOID ApcContext,
                                              OS_IO_STATUS_BLOCK* IoStatusBlock,
                                              PVOID FileInformation,
                                              ULONG Length,
                                              ULONG FileInformationClass,
                                              ULONG QueryFlags,
                                              OS_UNICODE_STRING* FileName);
OS_NTSTATUS NTAPI Hook_NtSetInformationFile(HANDLE FileHandle,
                                            OS_IO_STATUS_BLOCK* IoStatusBlock,
                                            PVOID FileInformation,
                                            ULONG Length,
                                            ULONG FileInformationClass);
HANDLE WINAPI Hook_CreateFile2(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               DWORD dwCreationDisposition,
                               OS_CREATEFILE2_EXTENDED_PARAMETERS* pCreateExParams);
BOOL WINAPI Hook_CloseHandle(HANDLE hObject);
DWORD WINAPI Hook_GetFinalPathNameByHandleW(HANDLE hFile,
                                            LPWSTR lpszFilePath,
                                            DWORD cchFilePath,
                                            DWORD dwFlags);
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

bool IsExplicitSystem32PathW(std::wstring_view raw_name) {
    if (raw_name.empty() || g_windows_dir_lower_w.empty()) {
        return false;
    }
    std::wstring lo = ToLowerAsciiW(raw_name);
    for (wchar_t& c : lo) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (lo.size() >= 4u &&
        (lo.compare(0u, 4u, L"\\\\?\\") == 0 || lo.compare(0u, 4u, L"\\??\\") == 0)) {
        lo.erase(0u, 4u);
    }
    const std::wstring sys32 = g_windows_dir_lower_w + L"\\system32\\";
    return lo.rfind(sys32, 0u) == 0u;
}

HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        // Explicit System32 loads with LOAD_LIBRARY_SEARCH_SYSTEM32 (e.g. our GameInput.dll
        // shim loading host C:\Windows\System32\GameInput.dll for native GIP controller input)
        // must bypass shim redirection so they never recurse back into the shim DLL.
        if ((dwFlags & LOAD_LIBRARY_SEARCH_SYSTEM32) != 0u &&
            IsExplicitSystem32PathW(lpLibFileName)) {
            return g_real_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
        }
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

bool TryResolveVirtualizedModuleHandleW(std::wstring_view wide_name,
                                        DWORD ex_flags,
                                        bool use_ex,
                                        HMODULE* out_mod) {
    if (wide_name.empty() || !out_mod) {
        return false;
    }
    *out_mod = nullptr;

    auto query_host = [&](const std::wstring& path) -> bool {
        if (path.empty()) {
            return false;
        }
        if (use_ex) {
            return g_real_GetModuleHandleExW &&
                   g_real_GetModuleHandleExW(ex_flags, path.c_str(), out_mod) != FALSE;
        }
        if (g_real_GetModuleHandleW) {
            HMODULE m = g_real_GetModuleHandleW(path.c_str());
            if (m != nullptr) {
                *out_mod = m;
                return true;
            }
        }
        return false;
    };

    openscarlett::DllRouteDecision decision;
    std::wstring target_path;
    if (MatchRoutedShimOrGuestDllW(wide_name, decision, target_path)) {
        if (!target_path.empty() && query_host(target_path)) {
            return true;
        }
        if (!decision.canonical_name.empty()) {
            const std::wstring canon_w = openscarlett::Utf8ToWide(decision.canonical_name);
            if (query_host(canon_w)) {
                return true;
            }
            // Fall back to tracked Xbox shim registry by canonical name.
            const std::string canon_lo = ToLowerAsciiA(decision.canonical_name);
            HMODULE tracked = nullptr;
            ::AcquireSRWLockShared(&g_state_lock);
            for (const auto& kv : g_tracked_xbox_modules) {
                if (ToLowerAsciiA(kv.second) == canon_lo) {
                    tracked = kv.first;
                    break;
                }
            }
            ::ReleaseSRWLockShared(&g_state_lock);
            if (tracked != nullptr) {
                if (use_ex) {
                    if (g_real_GetModuleHandleExW &&
                        g_real_GetModuleHandleExW(
                            ex_flags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(tracked),
                            out_mod) != FALSE) {
                        return true;
                    }
                } else {
                    *out_mod = tracked;
                    return true;
                }
            }
        }
    }

    // Also resolve virtualized VFS paths (e.g. "G:\MyGame.exe" or "G:\Plugins\Foo.dll").
    std::wstring vfs_host;
    if (TryTranslateVirtualDllPath(wide_name, vfs_host)) {
        if (query_host(vfs_host)) {
            return true;
        }
        if (!g_main_exe_path_w.empty()) {
            auto norm = [](std::wstring s) {
                for (wchar_t& c : s) {
                    if (c == L'/') c = L'\\';
                }
                if (s.size() >= 4 && s.compare(0, 4, L"\\\\?\\") == 0) {
                    s.erase(0, 4);
                }
                return ToLowerAsciiW(s);
            };
            if (norm(vfs_host) == norm(g_main_exe_path_w)) {
                if (use_ex) {
                    return g_real_GetModuleHandleExW &&
                           g_real_GetModuleHandleExW(ex_flags, nullptr, out_mod) != FALSE;
                }
                if (g_real_GetModuleHandleW) {
                    HMODULE m = g_real_GetModuleHandleW(nullptr);
                    if (m != nullptr) {
                        *out_mod = m;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool TryVirtualizeHostModulePathW(std::wstring_view real_host_path, std::wstring& out_virtual) {
    out_virtual.clear();
    if (real_host_path.empty()) {
        return false;
    }

    auto normalize_cmp = [](std::wstring_view s) {
        std::wstring out(s);
        for (wchar_t& c : out) {
            if (c == L'/') c = L'\\';
        }
        if (out.size() >= 4 &&
            (out.compare(0, 4, L"\\\\?\\") == 0 || out.compare(0, 4, L"\\??\\") == 0)) {
            out.erase(0, 4);
        }
        while (out.size() > 3 && out.back() == L'\\') {
            out.pop_back();
        }
        return out;
    };

    const std::wstring clean_real = normalize_cmp(real_host_path);
    const std::wstring real_lo = ToLowerAsciiW(clean_real);

    std::wstring drive_prefix;
    ::AcquireSRWLockShared(&g_state_lock);
    drive_prefix = g_virtual_module_drive_w.empty() ? L"G:" : g_virtual_module_drive_w;
    ::ReleaseSRWLockShared(&g_state_lock);
    while (!drive_prefix.empty() && (drive_prefix.back() == L'\\' || drive_prefix.back() == L'/')) {
        drive_prefix.pop_back();
    }

    auto match_root = [&](const std::wstring& root, const std::wstring& sub_prefix) -> bool {
        if (root.empty()) {
            return false;
        }
        const std::wstring clean_root = normalize_cmp(root);
        const std::wstring root_lo = ToLowerAsciiW(clean_root);
        if (real_lo.size() <= root_lo.size() || real_lo.compare(0, root_lo.size(), root_lo) != 0) {
            return false;
        }
        if (real_lo[root_lo.size()] != L'\\') {
            return false;
        }
        const std::wstring rel = clean_real.substr(root_lo.size() + 1u);
        out_virtual = drive_prefix + L"\\";
        if (!sub_prefix.empty()) {
            out_virtual += sub_prefix + L"\\";
        }
        out_virtual += rel;
        return true;
    };

    if (match_root(g_vfs.GetGameOverlayRoot(), L"")) {
        return true;
    }
    for (const auto& sub : g_vfs.GetGameSubMounts()) {
        if (match_root(sub.host_root, sub.virtual_prefix)) {
            return true;
        }
    }
    if (match_root(g_vfs.GetGameRoot(), L"")) {
        return true;
    }
    if (match_root(g_guest_dir_w, L"")) {
        return true;
    }
    return false;
}

DWORD CopyModuleFileNameResultW(const std::wstring& value, LPWSTR lpFilename, DWORD nSize) {
    if (!lpFilename || nSize == 0u) {
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    if (value.size() >= static_cast<size_t>(nSize)) {
        const size_t copy_chars = static_cast<size_t>(nSize - 1u);
        if (copy_chars > 0u) {
            std::memcpy(lpFilename, value.data(), copy_chars * sizeof(wchar_t));
        }
        lpFilename[copy_chars] = L'\0';
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    if (!value.empty()) {
        std::memcpy(lpFilename, value.data(), value.size() * sizeof(wchar_t));
    }
    lpFilename[value.size()] = L'\0';
    ::SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(value.size());
}

DWORD CopyModuleFileNameResultA(const std::string& value, LPSTR lpFilename, DWORD nSize) {
    if (!lpFilename || nSize == 0u) {
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    if (value.size() >= static_cast<size_t>(nSize)) {
        const size_t copy_chars = static_cast<size_t>(nSize - 1u);
        if (copy_chars > 0u) {
            std::memcpy(lpFilename, value.data(), copy_chars);
        }
        lpFilename[copy_chars] = '\0';
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    if (!value.empty()) {
        std::memcpy(lpFilename, value.data(), value.size());
    }
    lpFilename[value.size()] = '\0';
    ::SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(value.size());
}

HMODULE WINAPI Hook_GetModuleHandleA(LPCSTR lpModuleName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpModuleName != nullptr) {
        HMODULE mod = nullptr;
        if (TryResolveVirtualizedModuleHandleW(AnsiToWide(lpModuleName), 0u, false, &mod)) {
            return mod;
        }
    }
    return g_real_GetModuleHandleA(lpModuleName);
}

HMODULE WINAPI Hook_GetModuleHandleW(LPCWSTR lpModuleName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpModuleName != nullptr) {
        HMODULE mod = nullptr;
        if (TryResolveVirtualizedModuleHandleW(lpModuleName, 0u, false, &mod)) {
            return mod;
        }
    }
    return g_real_GetModuleHandleW(lpModuleName);
}

BOOL WINAPI Hook_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
        lpModuleName != nullptr && phModule != nullptr) {
        if (TryResolveVirtualizedModuleHandleW(AnsiToWide(lpModuleName), dwFlags, true, phModule)) {
            return TRUE;
        }
    }
    return g_real_GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI Hook_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
        lpModuleName != nullptr && phModule != nullptr) {
        if (TryResolveVirtualizedModuleHandleW(lpModuleName, dwFlags, true, phModule)) {
            return TRUE;
        }
    }
    return g_real_GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

DWORD WINAPI Hook_GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        g_virtualize_module_paths.load(std::memory_order_acquire)) {
        const std::wstring real_path = GetModulePathW(hModule);
        if (!real_path.empty()) {
            std::wstring virt_path;
            if (TryVirtualizeHostModulePathW(real_path, virt_path)) {
                return CopyModuleFileNameResultW(virt_path, lpFilename, nSize);
            }
        }
    }
    return g_real_GetModuleFileNameW(hModule, lpFilename, nSize);
}

DWORD WINAPI Hook_GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        g_virtualize_module_paths.load(std::memory_order_acquire)) {
        const std::wstring real_path = GetModulePathW(hModule);
        if (!real_path.empty()) {
            std::wstring virt_path;
            if (TryVirtualizeHostModulePathW(real_path, virt_path)) {
                const std::string ansi_path = WideToAnsiAcp(virt_path);
                if (!ansi_path.empty()) {
                    return CopyModuleFileNameResultA(ansi_path, lpFilename, nSize);
                }
            }
        }
    }
    return g_real_GetModuleFileNameA(hModule, lpFilename, nSize);
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
            if (g_real_CreateFile2 != nullptr && std::strcmp(lpProcName, "CreateFile2") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_CreateFile2);
            }
            if (g_real_CloseHandle != nullptr && std::strcmp(lpProcName, "CloseHandle") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_CloseHandle);
            }
            if (g_real_GetFinalPathNameByHandleW != nullptr &&
                std::strcmp(lpProcName, "GetFinalPathNameByHandleW") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_GetFinalPathNameByHandleW);
            }
            if ((g_real_GlobalMemoryStatusEx != nullptr ||
                 g_kernelbase_GlobalMemoryStatusEx != nullptr) &&
                std::strcmp(lpProcName, "GlobalMemoryStatusEx") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_GlobalMemoryStatusEx);
            }
        }
        EnsureAdvapi32ProcsResolved();
    EnsureComProcsResolved();
        if (hModule == g_kernel32_mod ||
            (g_kernelbase_mod != nullptr && hModule == g_kernelbase_mod) ||
            (g_advapi32_mod != nullptr && hModule == g_advapi32_mod)) {
            if (std::strcmp(lpProcName, "RegOpenKeyW") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_RegOpenKeyW);
            }
            if (std::strcmp(lpProcName, "RegOpenKeyExW") == 0 && g_real_RegOpenKeyExW != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegOpenKeyExW);
            }
            if (std::strcmp(lpProcName, "RegOpenKeyA") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_RegOpenKeyA);
            }
            if (std::strcmp(lpProcName, "RegOpenKeyExA") == 0 && g_real_RegOpenKeyExA != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegOpenKeyExA);
            }
            if (std::strcmp(lpProcName, "RegQueryValueExW") == 0 && g_real_RegQueryValueExW != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegQueryValueExW);
            }
            if (std::strcmp(lpProcName, "RegQueryValueExA") == 0 && g_real_RegQueryValueExA != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegQueryValueExA);
            }
            if (std::strcmp(lpProcName, "RegGetValueW") == 0 && g_real_RegGetValueW != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegGetValueW);
            }
            if (std::strcmp(lpProcName, "RegGetValueA") == 0 && g_real_RegGetValueA != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegGetValueA);
            }
            if (std::strcmp(lpProcName, "RegCloseKey") == 0 && g_real_RegCloseKey != nullptr) {
                return reinterpret_cast<FARPROC>(&Hook_RegCloseKey);
            }
        }
        if (g_ntdll_mod != nullptr && hModule == g_ntdll_mod) {
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
            if (g_real_NtClose != nullptr && std::strcmp(lpProcName, "NtClose") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtClose);
            }
            if (g_real_NtQueryDirectoryFile != nullptr &&
                std::strcmp(lpProcName, "NtQueryDirectoryFile") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtQueryDirectoryFile);
            }
            if (g_real_NtQueryDirectoryFileEx != nullptr &&
                std::strcmp(lpProcName, "NtQueryDirectoryFileEx") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtQueryDirectoryFileEx);
            }
            if (g_real_NtSetInformationFile != nullptr &&
                std::strcmp(lpProcName, "NtSetInformationFile") == 0) {
                return reinterpret_cast<FARPROC>(&Hook_NtSetInformationFile);
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
    // Preserve real host PC directories on mounted console drive letters (D:/G:/T:/S:/P:)
    // including shim_dir / telemetry / preserved host folders (xcl 61c6be8).
    std::wstring_view stripped = raw_path;
    if (stripped.size() >= 4 &&
        ((stripped[0] == L'\\' && stripped[1] == L'\\' && stripped[2] == L'?' && stripped[3] == L'\\') ||
         (stripped[0] == L'\\' && stripped[1] == L'?' && stripped[2] == L'?' && stripped[3] == L'\\'))) {
        stripped.remove_prefix(4);
    }
    if (stripped.size() < 2 || stripped[1] != L':') {
        return false;
    }
    const wchar_t drv_lo = (stripped[0] >= L'A' && stripped[0] <= L'Z')
                               ? static_cast<wchar_t>(stripped[0] - L'A' + L'a')
                               : stripped[0];
    if (drv_lo != L'd' && drv_lo != L'g' && drv_lo != L't' && drv_lo != L's' && drv_lo != L'p') {
        return false;
    }
    if (!g_shim_dir_w.empty()) {
        const wchar_t shim_drv = (g_shim_dir_w[0] >= L'A' && g_shim_dir_w[0] <= L'Z')
                                     ? static_cast<wchar_t>(g_shim_dir_w[0] - L'A' + L'a')
                                     : g_shim_dir_w[0];
        if (shim_drv == drv_lo) {
            std::wstring lo_in = ToLowerAsciiW(stripped);
            std::wstring lo_shim = ToLowerAsciiW(g_shim_dir_w);
            for (wchar_t& c : lo_in) {
                if (c == L'/') c = L'\\';
            }
            for (wchar_t& c : lo_shim) {
                if (c == L'/') c = L'\\';
            }
            if (lo_in.rfind(lo_shim, 0) == 0 &&
                (lo_in.size() == lo_shim.size() || lo_in[lo_shim.size()] == L'\\')) {
                return true;
            }
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

std::wstring StripNtOrExtPrefixW(std::wstring_view in) {
    if (in.size() >= 4u &&
        (in.compare(0u, 4u, L"\\??\\") == 0 || in.compare(0u, 4u, L"\\\\?\\") == 0)) {
        in.remove_prefix(4u);
    }
    return std::wstring(in);
}

std::wstring NormalizeCanonicalVirtualPathW(std::wstring_view raw) {
    std::wstring s = StripNtOrExtPrefixW(raw);
    for (wchar_t& c : s) {
        if (c == L'/') c = L'\\';
    }
    if (s.size() >= 5u &&
        (s[0] == L'G' || s[0] == L'g') &&
        (s[1] == L'A' || s[1] == L'a') &&
        (s[2] == L'M' || s[2] == L'm') &&
        (s[3] == L'E' || s[3] == L'e') &&
        s[4] == L':') {
        s.replace(0u, 5u, L"G:");
    }
    if (s.size() >= 2u && s[1] == L':') {
        if (s[0] >= L'a' && s[0] <= L'z') {
            s[0] = static_cast<wchar_t>(s[0] - L'a' + L'A');
        }
        if (s.size() == 2u) {
            s.push_back(L'\\');
        } else if (s[2] != L'\\') {
            s.insert(2u, 1u, L'\\');
        }
    }
    std::wstring out;
    out.reserve(s.size());
    bool prev_bs = false;
    for (wchar_t c : s) {
        if (c == L'\\') {
            if (!prev_bs) {
                out.push_back(c);
                prev_bs = true;
            }
        } else {
            out.push_back(c);
            prev_bs = false;
        }
    }
    while (out.size() > 3u && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

bool IsHostPathUnderOverlayW(std::wstring_view host_path) {
    const std::wstring& ov_root = g_vfs.GetGameOverlayRoot();
    if (ov_root.empty()) {
        return false;
    }
    std::wstring h = ToLowerAsciiW(StripNtOrExtPrefixW(host_path));
    std::wstring ov = ToLowerAsciiW(StripNtOrExtPrefixW(ov_root));
    for (wchar_t& c : h) {
        if (c == L'/') c = L'\\';
    }
    for (wchar_t& c : ov) {
        if (c == L'/') c = L'\\';
    }
    while (ov.size() > 3u && ov.back() == L'\\') {
        ov.pop_back();
    }
    if (h.size() < ov.size() || h.compare(0u, ov.size(), ov) != 0) {
        return false;
    }
    return h.size() == ov.size() || h[ov.size()] == L'\\';
}

void TrackVfsHandle(HANDLE h,
                    std::wstring_view virtual_path,
                    std::wstring_view host_path,
                    openscarlett::VfsDrive drive) {
    if (!h || h == INVALID_HANDLE_VALUE || drive == openscarlett::VfsDrive::None) {
        return;
    }
    TrackedVfsHandle entry{};
    entry.virtual_path = NormalizeCanonicalVirtualPathW(virtual_path);
    entry.host_path = StripNtOrExtPrefixW(host_path);
    entry.drive = drive;
    entry.is_overlay = (drive == openscarlett::VfsDrive::Game) && IsHostPathUnderOverlayW(entry.host_path);
    const DWORD attrs = g_real_GetFileAttributesW
                            ? g_real_GetFileAttributesW(entry.host_path.c_str())
                            : ::GetFileAttributesW(entry.host_path.c_str());
    entry.is_directory = (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u);

    ::AcquireSRWLockExclusive(&g_vfs_handle_lock);
    g_vfs_handles[h] = std::move(entry);
    g_vfs_handle_count.store(static_cast<uint32_t>(g_vfs_handles.size()), std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
}

void UntrackVfsHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || g_vfs_handle_count.load(std::memory_order_acquire) == 0u) {
        return;
    }
    ::AcquireSRWLockExclusive(&g_vfs_handle_lock);
    g_vfs_handles.erase(h);
    g_vfs_handle_count.store(static_cast<uint32_t>(g_vfs_handles.size()), std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
}

// Fast lock-contention-free cleanup on passthrough opens: if a tracked VFS handle was
// closed via an unhooked Windows DLL and the OS reused its numeric HANDLE value for a
// non-virtual file, clear the stale entry without taking an exclusive lock in the common case.
void UntrackVfsHandleIfPresent(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || g_vfs_handle_count.load(std::memory_order_acquire) == 0u) {
        return;
    }
    ::AcquireSRWLockShared(&g_vfs_handle_lock);
    const bool present = (g_vfs_handles.find(h) != g_vfs_handles.end());
    ::ReleaseSRWLockShared(&g_vfs_handle_lock);
    if (!present) {
        return;
    }
    UntrackVfsHandle(h);
}

bool LookupTrackedVfsHandleSnapshot(HANDLE h, TrackedVfsHandle& out) {
    if (!h || h == INVALID_HANDLE_VALUE || g_vfs_handle_count.load(std::memory_order_acquire) == 0u) {
        return false;
    }
    ::AcquireSRWLockShared(&g_vfs_handle_lock);
    auto it = g_vfs_handles.find(h);
    const bool found = (it != g_vfs_handles.end());
    if (found) {
        out.virtual_path = it->second.virtual_path;
        out.host_path = it->second.host_path;
        out.drive = it->second.drive;
        out.is_overlay = it->second.is_overlay;
        out.is_directory = it->second.is_directory;
    }
    ::ReleaseSRWLockShared(&g_vfs_handle_lock);
    return found;
}

bool MatchesNtWildcardW(std::wstring_view name, std::wstring_view pattern) {
    if (pattern.empty() || pattern == L"*" || pattern == L"*.*") {
        return true;
    }
    std::wstring n = ToLowerAsciiW(name);
    std::wstring p = ToLowerAsciiW(pattern);
    size_t ni = 0u;
    size_t pi = 0u;
    size_t star_p = std::wstring::npos;
    size_t star_n = 0u;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == L'>' || p[pi] == n[ni])) {
            ++ni;
            ++pi;
        } else if (pi < p.size() && (p[pi] == L'*' || p[pi] == L'<')) {
            star_p = pi++;
            star_n = ni;
        } else if (pi < p.size() && p[pi] == L'"' && n[ni] == L'.') {
            ++ni;
            ++pi;
        } else if (star_p != std::wstring::npos) {
            pi = star_p + 1u;
            ni = ++star_n;
        } else {
            return false;
        }
    }
    while (pi < p.size() && (p[pi] == L'*' || p[pi] == L'<' || p[pi] == L'"')) {
        ++pi;
    }
    return pi == p.size();
}

bool IsWhiteoutMarkerFileNameW(std::wstring_view name) {
    const std::wstring lo = ToLowerAsciiW(name);
    constexpr std::wstring_view kSuffix = L".whiteout";
    if (lo == kSuffix) {
        return true;
    }
    return lo.size() >= kSuffix.size() &&
           lo.compare(lo.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

std::wstring JoinVirtualChildW(std::wstring_view dir_vpath, std::wstring_view child_name) {
    std::wstring res(dir_vpath);
    if (!res.empty() && res.back() != L'\\') {
        res.push_back(L'\\');
    }
    res.append(child_name);
    return res;
}

std::vector<TrackedVfsDirEntry> BuildMergedGameDirEntries(const std::wstring& dir_vpath,
                                                          const std::wstring& pattern) {
    std::vector<TrackedVfsDirEntry> out;
    std::wstring base_dir;
    std::wstring overlay_dir;
    std::wstring rel_dir;
    bool was_wo = false;
    const auto st = g_vfs.ResolveGamePairW(dir_vpath, base_dir, overlay_dir, rel_dir, &was_wo);
    if (st != openscarlett::VfsTranslateStatus::Translated || was_wo) {
        return out;
    }

    std::unordered_set<std::wstring> seen_lower;
    auto scan_dir = [&](const std::wstring& host_dir, bool is_overlay) {
        if (host_dir.empty()) {
            return;
        }
        std::wstring glob = host_dir;
        if (glob.back() != L'\\') glob.push_back(L'\\');
        glob.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE fh = g_real_FindFirstFileW ? g_real_FindFirstFileW(glob.c_str(), &fd)
                                          : ::FindFirstFileW(glob.c_str(), &fd);
        if (fh == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            std::wstring_view fname(fd.cFileName);
            if (is_overlay && IsWhiteoutMarkerFileNameW(fname)) {
                continue;
            }
            const std::wstring lo = ToLowerAsciiW(fname);
            if (seen_lower.find(lo) != seen_lower.end()) {
                continue;
            }
            const bool is_dot = (fname == L"." || fname == L"..");
            if (!is_overlay && !is_dot) {
                const std::wstring child_v = JoinVirtualChildW(dir_vpath, fname);
                if (g_vfs.IsWhiteoutedW(child_v)) {
                    continue;
                }
            }
            if (!MatchesNtWildcardW(fname, pattern)) {
                continue;
            }
            seen_lower.insert(lo);
            TrackedVfsDirEntry rec{};
            rec.name.assign(fname);
            rec.fd = fd;
            out.push_back(std::move(rec));
        } while (::FindNextFileW(fh, &fd));
        ::FindClose(fh);
    };

    // Overlay entries take precedence over base entries (deduplication + shadowing).
    scan_dir(overlay_dir, true);
    scan_dir(base_dir, false);

    // Include virtual sub-mount mount points directly under this directory.
    std::wstring rel_lo = ToLowerAsciiW(rel_dir);
    for (wchar_t& c : rel_lo) {
        if (c == L'/') c = L'\\';
    }
    while (!rel_lo.empty() && rel_lo.back() == L'\\') rel_lo.pop_back();

    for (const auto& sub : g_vfs.GetGameSubMounts()) {
        std::wstring sub_rel = sub.virtual_prefix;
        for (wchar_t& c : sub_rel) {
            if (c == L'/') c = L'\\';
        }
        while (!sub_rel.empty() && sub_rel.back() == L'\\') sub_rel.pop_back();
        if (sub_rel.empty()) continue;
        std::wstring parent_rel;
        std::wstring child_name;
        const size_t slash = sub_rel.rfind(L'\\');
        if (slash == std::wstring::npos) {
            child_name = sub_rel;
        } else {
            parent_rel = ToLowerAsciiW(sub_rel.substr(0u, slash));
            child_name = sub_rel.substr(slash + 1u);
        }
        if (parent_rel != rel_lo || child_name.empty()) {
            continue;
        }
        const std::wstring lo = ToLowerAsciiW(child_name);
        if (seen_lower.find(lo) != seen_lower.end()) {
            continue;
        }
        const std::wstring child_v = JoinVirtualChildW(dir_vpath, child_name);
        if (g_vfs.IsWhiteoutedW(child_v) || !MatchesNtWildcardW(child_name, pattern)) {
            continue;
        }
        seen_lower.insert(lo);
        TrackedVfsDirEntry rec{};
        rec.name = child_name;
        rec.fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        const size_t copy_n = std::min<size_t>(child_name.size(), MAX_PATH - 1u);
        std::memcpy(rec.fd.cFileName, child_name.data(), copy_n * sizeof(wchar_t));
        rec.fd.cFileName[copy_n] = L'\0';
        out.push_back(std::move(rec));
    }

    std::sort(out.begin(), out.end(), [](const TrackedVfsDirEntry& a, const TrackedVfsDirEntry& b) {
        auto rank = [](const std::wstring& s) -> int {
            if (s == L".") return 0;
            if (s == L"..") return 1;
            return 2;
        };
        const int ra = rank(a.name);
        const int rb = rank(b.name);
        if (ra != rb) return ra < rb;
        return ToLowerAsciiW(a.name) < ToLowerAsciiW(b.name);
    });
    return out;
}

uint64_t HashSyntheticFileId(std::wstring_view path) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : path) {
        const wchar_t lc = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        h ^= static_cast<uint64_t>(static_cast<uint16_t>(lc));
        h *= 1099511628211ull;
    }
    return h;
}

bool TryTranslateNtObjectPath(const OS_OBJECT_ATTRIBUTES* oa,
                              ACCESS_MASK desired_access,
                              ULONG create_disposition,
                              bool is_query_only,
                              const char* api_tag,
                              std::wstring& out_nt_host_path,
                              OS_NTSTATUS& out_error_status,
                              std::wstring* out_resolved_virtual = nullptr,
                              openscarlett::VfsDrive* out_drive = nullptr) {
    out_nt_host_path.clear();
    out_error_status = OS_STATUS_SUCCESS;
    if (out_resolved_virtual) out_resolved_virtual->clear();
    if (out_drive) *out_drive = openscarlett::VfsDrive::None;
    if (!oa || !oa->ObjectName) {
        return false;
    }

    std::wstring combined_virtual;
    std::wstring_view raw_view;
    if (oa->ObjectName->Buffer != nullptr && oa->ObjectName->Length > 0u) {
        const size_t char_count = static_cast<size_t>(oa->ObjectName->Length) / sizeof(wchar_t);
        raw_view = std::wstring_view(oa->ObjectName->Buffer, char_count);
        while (!raw_view.empty() && raw_view.back() == L'\0') {
            raw_view.remove_suffix(1u);
        }
    }

    if (oa->RootDirectory != nullptr) {
        TrackedVfsHandle root_snap{};
        if (!LookupTrackedVfsHandleSnapshot(oa->RootDirectory, root_snap)) {
            return false;
        }
        if (raw_view.empty()) {
            combined_virtual = root_snap.virtual_path;
        } else if (openscarlett::VfsMounts::DetectVirtualDriveW(raw_view) != openscarlett::VfsDrive::None) {
            combined_virtual = NormalizeCanonicalVirtualPathW(raw_view);
        } else {
            while (!raw_view.empty() && (raw_view.front() == L'\\' || raw_view.front() == L'/')) {
                raw_view.remove_prefix(1u);
            }
            combined_virtual = JoinVirtualChildW(root_snap.virtual_path, raw_view);
        }
        raw_view = combined_virtual;
    }

    if (raw_view.empty()) {
        return false;
    }

    const openscarlett::VfsDrive drive = openscarlett::VfsMounts::DetectVirtualDriveW(raw_view);
    if (drive == openscarlett::VfsDrive::None || ShouldPassThroughHostDDriveW(raw_view)) {
        return false;
    }
    if (out_drive) *out_drive = drive;
    if (out_resolved_virtual) *out_resolved_virtual = NormalizeCanonicalVirtualPathW(raw_view);

    constexpr ACCESS_MASK kNtWriteMask =
        GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    const bool wants_write = !is_query_only &&
                             (((desired_access & kNtWriteMask) != 0) ||
                              create_disposition == OS_FILE_SUPERSEDE ||
                              create_disposition == OS_FILE_CREATE ||
                              create_disposition == OS_FILE_OVERWRITE ||
                              create_disposition == OS_FILE_OVERWRITE_IF);

    std::wstring translated;
    bool was_wo = false;
    openscarlett::VfsTranslateStatus st = openscarlett::VfsTranslateStatus::NotVirtual;

    if (!is_query_only && drive == openscarlett::VfsDrive::Game &&
        (create_disposition == OS_FILE_OVERWRITE ||
         create_disposition == OS_FILE_OVERWRITE_IF ||
         create_disposition == OS_FILE_SUPERSEDE)) {
        const bool allow_create = (create_disposition != OS_FILE_OVERWRITE);
        st = g_vfs.PrepareOverlayTruncateW(raw_view, allow_create, translated, &was_wo);
    } else {
        openscarlett::VfsAccessMode mode = openscarlett::VfsAccessMode::ReadOnly;
        if (!is_query_only && wants_write) {
            if (drive == openscarlett::VfsDrive::Game) {
                if (create_disposition == OS_FILE_CREATE) {
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
        st = g_vfs.TranslateForAccessW(raw_view, mode, translated, &was_wo, !is_query_only);
        if (st == openscarlett::VfsTranslateStatus::Translated && !is_query_only && wants_write &&
            drive == openscarlett::VfsDrive::Game && IsHostPathUnderOverlayW(translated)) {
            const DWORD attrs = g_real_GetFileAttributesW
                                    ? g_real_GetFileAttributesW(translated.c_str())
                                    : ::GetFileAttributesW(translated.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
                ::SetFileAttributesW(translated.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            }
        }
    }

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
        std::wstring resolved_vpath;
        openscarlett::VfsDrive drive = openscarlett::VfsDrive::None;
        OS_NTSTATUS err_status = OS_STATUS_SUCCESS;
        if (TryTranslateNtObjectPath(ObjectAttributes, DesiredAccess, CreateDisposition, false,
                                     "NtCreateFile", host_nt, err_status, &resolved_vpath, &drive)) {
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
            new_oa.RootDirectory = nullptr;
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
            if (st >= 0 && FileHandle != nullptr && *FileHandle != nullptr) {
                TrackVfsHandle(*FileHandle, resolved_vpath, host_nt, drive);
            }
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "NtCreateFile",
                st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                static_cast<int32_t>(st),
                -1);
            return st;
        }
    }
    const OS_NTSTATUS passthrough_st = g_real_NtCreateFile(FileHandle,
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
    if (passthrough_st >= 0 && FileHandle != nullptr && *FileHandle != nullptr &&
        !g_shutting_down.load(std::memory_order_acquire)) {
        UntrackVfsHandleIfPresent(*FileHandle);
    }
    return passthrough_st;
}

OS_NTSTATUS NTAPI Hook_NtOpenFile(PHANDLE FileHandle,
                                  ACCESS_MASK DesiredAccess,
                                  OS_OBJECT_ATTRIBUTES* ObjectAttributes,
                                  OS_IO_STATUS_BLOCK* IoStatusBlock,
                                  ULONG ShareAccess,
                                  ULONG OpenOptions) {
    if (!g_shutting_down.load(std::memory_order_acquire) && ObjectAttributes != nullptr) {
        std::wstring host_nt;
        std::wstring resolved_vpath;
        openscarlett::VfsDrive drive = openscarlett::VfsDrive::None;
        OS_NTSTATUS err_status = OS_STATUS_SUCCESS;
        if (TryTranslateNtObjectPath(ObjectAttributes, DesiredAccess, OS_FILE_OPEN, false,
                                     "NtOpenFile", host_nt, err_status, &resolved_vpath, &drive)) {
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
            new_oa.RootDirectory = nullptr;
            new_oa.ObjectName = &new_name;

            const OS_NTSTATUS st = g_real_NtOpenFile(FileHandle,
                                                     DesiredAccess,
                                                     &new_oa,
                                                     IoStatusBlock,
                                                     ShareAccess,
                                                     OpenOptions);
            if (st >= 0 && FileHandle != nullptr && *FileHandle != nullptr) {
                TrackVfsHandle(*FileHandle, resolved_vpath, host_nt, drive);
            }
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "NtOpenFile",
                st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                static_cast<int32_t>(st),
                -1);
            return st;
        }
    }
    const OS_NTSTATUS passthrough_st = g_real_NtOpenFile(FileHandle,
                                                         DesiredAccess,
                                                         ObjectAttributes,
                                                         IoStatusBlock,
                                                         ShareAccess,
                                                         OpenOptions);
    if (passthrough_st >= 0 && FileHandle != nullptr && *FileHandle != nullptr &&
        !g_shutting_down.load(std::memory_order_acquire)) {
        UntrackVfsHandleIfPresent(*FileHandle);
    }
    return passthrough_st;
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
            new_oa.RootDirectory = nullptr;
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

OS_NTSTATUS NTAPI Hook_NtClose(HANDLE Handle) {
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        UntrackVfsHandle(Handle);
    }
    return g_real_NtClose(Handle);
}

OS_NTSTATUS QueryVirtualGameDirectoryImpl(HANDLE FileHandle,
                                          HANDLE Event,
                                          OS_IO_STATUS_BLOCK* IoStatusBlock,
                                          PVOID FileInformation,
                                          ULONG Length,
                                          ULONG FileInformationClass,
                                          ULONG QueryFlags,
                                          OS_UNICODE_STRING* FileName,
                                          const char* api_tag) {
    if (!IoStatusBlock || !FileInformation) {
        return OS_STATUS_INVALID_PARAMETER;
    }

    size_t fixed_hdr_size = 0u;
    switch (FileInformationClass) {
        case OS_FileDirectoryInformation:
            fixed_hdr_size = offsetof(OS_FILE_DIRECTORY_INFORMATION, FileName);
            break;
        case OS_FileFullDirectoryInformation:
            fixed_hdr_size = offsetof(OS_FILE_FULL_DIR_INFORMATION, FileName);
            break;
        case OS_FileBothDirectoryInformation:
            fixed_hdr_size = offsetof(OS_FILE_BOTH_DIR_INFORMATION, FileName);
            break;
        case OS_FileIdFullDirectoryInformation:
            fixed_hdr_size = offsetof(OS_FILE_ID_FULL_DIR_INFORMATION, FileName);
            break;
        case OS_FileIdBothDirectoryInformation:
            fixed_hdr_size = offsetof(OS_FILE_ID_BOTH_DIR_INFORMATION, FileName);
            break;
        case OS_FileNamesInformation:
            fixed_hdr_size = offsetof(OS_FILE_NAMES_INFORMATION, FileName);
            break;
        default:
            IoStatusBlock->Status = OS_STATUS_INVALID_INFO_CLASS;
            IoStatusBlock->Information = 0u;
            return OS_STATUS_INVALID_INFO_CLASS;
    }

    if (Length < fixed_hdr_size) {
        IoStatusBlock->Status = OS_STATUS_INFO_LENGTH_MISMATCH;
        IoStatusBlock->Information = 0u;
        return OS_STATUS_INFO_LENGTH_MISMATCH;
    }

    std::wstring req_pattern;
    if (FileName && FileName->Buffer && FileName->Length > 0u) {
        req_pattern.assign(FileName->Buffer, static_cast<size_t>(FileName->Length) / sizeof(wchar_t));
        while (!req_pattern.empty() && req_pattern.back() == L'\0') {
            req_pattern.pop_back();
        }
    }

    const bool restart_scan = (QueryFlags & OS_SL_RESTART_SCAN) != 0u;
    const bool single_entry = (QueryFlags & OS_SL_RETURN_SINGLE_ENTRY) != 0u;
    const bool no_cursor_update = (QueryFlags & OS_SL_NO_CURSOR_UPDATE) != 0u;

    ::AcquireSRWLockExclusive(&g_vfs_handle_lock);
    auto it = g_vfs_handles.find(FileHandle);
    if (it == g_vfs_handles.end()) {
        ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
        return OS_STATUS_INVALID_PARAMETER;
    }
    TrackedVfsHandle& hstate = it->second;
    if (!hstate.is_directory) {
        ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
        IoStatusBlock->Status = OS_STATUS_NOT_A_DIRECTORY;
        IoStatusBlock->Information = 0u;
        return OS_STATUS_NOT_A_DIRECTORY;
    }

    const bool first_call = !hstate.dir_enum_initialized;
    if (first_call || restart_scan) {
        if (!req_pattern.empty()) {
            hstate.dir_enum_pattern = req_pattern;
        } else if (first_call) {
            hstate.dir_enum_pattern = L"*";
        }
        hstate.dir_entries = BuildMergedGameDirEntries(hstate.virtual_path, hstate.dir_enum_pattern);
        hstate.dir_enum_cursor = 0u;
        hstate.dir_enum_initialized = true;
    }

    if (hstate.dir_enum_cursor >= hstate.dir_entries.size()) {
        const bool empty_first = first_call && hstate.dir_entries.empty();
        ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
        const OS_NTSTATUS st = empty_first ? OS_STATUS_NO_SUCH_FILE : OS_STATUS_NO_MORE_FILES;
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = 0u;
        if (Event != nullptr) {
            ::SetEvent(Event);
        }
        return st;
    }

    auto* out_bytes = static_cast<uint8_t*>(FileInformation);
    size_t write_offset = 0u;
    size_t prev_offset = 0u;
    size_t packed = 0u;

    auto filetime_to_li = [](const FILETIME& ft) -> LARGE_INTEGER {
        LARGE_INTEGER li{};
        li.LowPart = ft.dwLowDateTime;
        li.HighPart = static_cast<LONG>(ft.dwHighDateTime);
        return li;
    };

    for (size_t idx = hstate.dir_enum_cursor; idx < hstate.dir_entries.size(); ++idx) {
        const auto& rec = hstate.dir_entries[idx];
        const size_t name_bytes = rec.name.size() * sizeof(wchar_t);
        const size_t unaligned_size = fixed_hdr_size + name_bytes;
        const size_t aligned_offset = (packed == 0u) ? 0u : ((write_offset + 7u) & ~size_t{7u});

        if (aligned_offset + unaligned_size > static_cast<size_t>(Length)) {
            if (packed == 0u) {
                ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
                IoStatusBlock->Status = OS_STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = 0u;
                return OS_STATUS_BUFFER_OVERFLOW;
            }
            break;
        }

        if (packed > 0u) {
            // Patch previous record's NextEntryOffset.
            const ULONG delta = static_cast<ULONG>(aligned_offset - prev_offset);
            std::memcpy(out_bytes + prev_offset, &delta, sizeof(ULONG));
        }

        uint8_t* dst = out_bytes + aligned_offset;
        std::memset(dst, 0, unaligned_size);

        const LARGE_INTEGER ct = filetime_to_li(rec.fd.ftCreationTime);
        const LARGE_INTEGER at = filetime_to_li(rec.fd.ftLastAccessTime);
        const LARGE_INTEGER wt = filetime_to_li(rec.fd.ftLastWriteTime);
        LARGE_INTEGER eof{};
        eof.LowPart = rec.fd.nFileSizeLow;
        eof.HighPart = static_cast<LONG>(rec.fd.nFileSizeHigh);
        LARGE_INTEGER alloc{};
        alloc.QuadPart = (eof.QuadPart + 4095LL) & ~4095LL;
        const ULONG fname_len_bytes = static_cast<ULONG>(name_bytes);
        const ULONG fidx = static_cast<ULONG>(idx + 1u);
        LARGE_INTEGER fid{};
        fid.QuadPart = static_cast<LONGLONG>(HashSyntheticFileId(JoinVirtualChildW(hstate.virtual_path, rec.name)));

        switch (FileInformationClass) {
            case OS_FileDirectoryInformation: {
                auto* info = reinterpret_cast<OS_FILE_DIRECTORY_INFORMATION*>(dst);
                info->FileIndex = fidx;
                info->CreationTime = ct;
                info->LastAccessTime = at;
                info->LastWriteTime = wt;
                info->ChangeTime = wt;
                info->EndOfFile = eof;
                info->AllocationSize = alloc;
                info->FileAttributes = rec.fd.dwFileAttributes;
                info->FileNameLength = fname_len_bytes;
                if (name_bytes > 0u) std::memcpy(info->FileName, rec.name.data(), name_bytes);
                break;
            }
            case OS_FileFullDirectoryInformation: {
                auto* info = reinterpret_cast<OS_FILE_FULL_DIR_INFORMATION*>(dst);
                info->FileIndex = fidx;
                info->CreationTime = ct;
                info->LastAccessTime = at;
                info->LastWriteTime = wt;
                info->ChangeTime = wt;
                info->EndOfFile = eof;
                info->AllocationSize = alloc;
                info->FileAttributes = rec.fd.dwFileAttributes;
                info->FileNameLength = fname_len_bytes;
                if (name_bytes > 0u) std::memcpy(info->FileName, rec.name.data(), name_bytes);
                break;
            }
            case OS_FileBothDirectoryInformation: {
                auto* info = reinterpret_cast<OS_FILE_BOTH_DIR_INFORMATION*>(dst);
                info->FileIndex = fidx;
                info->CreationTime = ct;
                info->LastAccessTime = at;
                info->LastWriteTime = wt;
                info->ChangeTime = wt;
                info->EndOfFile = eof;
                info->AllocationSize = alloc;
                info->FileAttributes = rec.fd.dwFileAttributes;
                info->FileNameLength = fname_len_bytes;
                if (name_bytes > 0u) std::memcpy(info->FileName, rec.name.data(), name_bytes);
                break;
            }
            case OS_FileIdFullDirectoryInformation: {
                auto* info = reinterpret_cast<OS_FILE_ID_FULL_DIR_INFORMATION*>(dst);
                info->FileIndex = fidx;
                info->CreationTime = ct;
                info->LastAccessTime = at;
                info->LastWriteTime = wt;
                info->ChangeTime = wt;
                info->EndOfFile = eof;
                info->AllocationSize = alloc;
                info->FileAttributes = rec.fd.dwFileAttributes;
                info->FileNameLength = fname_len_bytes;
                info->FileId = fid;
                if (name_bytes > 0u) std::memcpy(info->FileName, rec.name.data(), name_bytes);
                break;
            }
            case OS_FileIdBothDirectoryInformation: {
                auto* info = reinterpret_cast<OS_FILE_ID_BOTH_DIR_INFORMATION*>(dst);
                info->FileIndex = fidx;
                info->CreationTime = ct;
                info->LastAccessTime = at;
                info->LastWriteTime = wt;
                info->ChangeTime = wt;
                info->EndOfFile = eof;
                info->AllocationSize = alloc;
                info->FileAttributes = rec.fd.dwFileAttributes;
                info->FileNameLength = fname_len_bytes;
                info->FileId = fid;
                if (name_bytes > 0u) std::memcpy(info->FileName, rec.name.data(), name_bytes);
                break;
            }
            case OS_FileNamesInformation: {
                auto* info = reinterpret_cast<OS_FILE_NAMES_INFORMATION*>(dst);
                info->FileIndex = fidx;
                info->FileNameLength = fname_len_bytes;
                if (name_bytes > 0u) std::memcpy(info->FileName, rec.name.data(), name_bytes);
                break;
            }
            default:
                break;
        }

        prev_offset = aligned_offset;
        write_offset = aligned_offset + unaligned_size;
        ++packed;
        if (single_entry) {
            break;
        }
    }

    if (!no_cursor_update) {
        hstate.dir_enum_cursor += packed;
    }
    ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);

    IoStatusBlock->Status = OS_STATUS_SUCCESS;
    IoStatusBlock->Information = static_cast<ULONG_PTR>(write_offset);
    if (Event != nullptr) {
        ::SetEvent(Event);
    }
    openscarlett::CallTelemetry::Global().Record(
        "vfs", api_tag, openscarlett::CallOutcome::Ok, static_cast<int32_t>(packed), -1);
    return OS_STATUS_SUCCESS;
}

OS_NTSTATUS NTAPI Hook_NtQueryDirectoryFile(HANDLE FileHandle,
                                            HANDLE Event,
                                            PVOID ApcRoutine,
                                            PVOID ApcContext,
                                            OS_IO_STATUS_BLOCK* IoStatusBlock,
                                            PVOID FileInformation,
                                            ULONG Length,
                                            ULONG FileInformationClass,
                                            BOOLEAN ReturnSingleEntry,
                                            OS_UNICODE_STRING* FileName,
                                            BOOLEAN RestartScan) {
    if (!g_shutting_down.load(std::memory_order_acquire) && FileHandle != nullptr) {
        TrackedVfsHandle snap{};
        if (LookupTrackedVfsHandleSnapshot(FileHandle, snap) &&
            snap.drive == openscarlett::VfsDrive::Game && snap.is_directory) {
            ULONG flags = 0u;
            if (RestartScan) flags |= OS_SL_RESTART_SCAN;
            if (ReturnSingleEntry) flags |= OS_SL_RETURN_SINGLE_ENTRY;
            return QueryVirtualGameDirectoryImpl(FileHandle,
                                                 Event,
                                                 IoStatusBlock,
                                                 FileInformation,
                                                 Length,
                                                 FileInformationClass,
                                                 flags,
                                                 FileName,
                                                 "NtQueryDirectoryFile");
        }
    }
    return g_real_NtQueryDirectoryFile(FileHandle,
                                       Event,
                                       ApcRoutine,
                                       ApcContext,
                                       IoStatusBlock,
                                       FileInformation,
                                       Length,
                                       FileInformationClass,
                                       ReturnSingleEntry,
                                       FileName,
                                       RestartScan);
}

OS_NTSTATUS NTAPI Hook_NtQueryDirectoryFileEx(HANDLE FileHandle,
                                              HANDLE Event,
                                              PVOID ApcRoutine,
                                              PVOID ApcContext,
                                              OS_IO_STATUS_BLOCK* IoStatusBlock,
                                              PVOID FileInformation,
                                              ULONG Length,
                                              ULONG FileInformationClass,
                                              ULONG QueryFlags,
                                              OS_UNICODE_STRING* FileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && FileHandle != nullptr) {
        TrackedVfsHandle snap{};
        if (LookupTrackedVfsHandleSnapshot(FileHandle, snap) &&
            snap.drive == openscarlett::VfsDrive::Game && snap.is_directory) {
            return QueryVirtualGameDirectoryImpl(FileHandle,
                                                 Event,
                                                 IoStatusBlock,
                                                 FileInformation,
                                                 Length,
                                                 FileInformationClass,
                                                 QueryFlags,
                                                 FileName,
                                                 "NtQueryDirectoryFileEx");
        }
    }
    return g_real_NtQueryDirectoryFileEx(FileHandle,
                                         Event,
                                         ApcRoutine,
                                         ApcContext,
                                         IoStatusBlock,
                                         FileInformation,
                                         Length,
                                         FileInformationClass,
                                         QueryFlags,
                                         FileName);
}

OS_NTSTATUS NTAPI Hook_NtSetInformationFile(HANDLE FileHandle,
                                            OS_IO_STATUS_BLOCK* IoStatusBlock,
                                            PVOID FileInformation,
                                            ULONG Length,
                                            ULONG FileInformationClass) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        FileHandle != nullptr && FileInformation != nullptr) {
        TrackedVfsHandle snap{};
        if (LookupTrackedVfsHandleSnapshot(FileHandle, snap) &&
            snap.drive == openscarlett::VfsDrive::Game) {
            if (FileInformationClass == OS_FileDispositionInformation ||
                FileInformationClass == OS_FileDispositionInformationEx) {
                bool mark_delete = false;
                if (FileInformationClass == OS_FileDispositionInformation) {
                    if (Length < sizeof(OS_FILE_DISPOSITION_INFORMATION)) {
                        return OS_STATUS_INFO_LENGTH_MISMATCH;
                    }
                    mark_delete =
                        static_cast<const OS_FILE_DISPOSITION_INFORMATION*>(FileInformation)->DeleteFile != 0;
                } else {
                    if (Length < sizeof(OS_FILE_DISPOSITION_INFORMATION_EX)) {
                        return OS_STATUS_INFO_LENGTH_MISMATCH;
                    }
                    mark_delete =
                        (static_cast<const OS_FILE_DISPOSITION_INFORMATION_EX*>(FileInformation)->Flags &
                         OS_FILE_DISPOSITION_DELETE) != 0u;
                }

                if (mark_delete) {
                    if (snap.is_overlay) {
                        const DWORD attrs = g_real_GetFileAttributesW
                                                ? g_real_GetFileAttributesW(snap.host_path.c_str())
                                                : ::GetFileAttributesW(snap.host_path.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
                            ::SetFileAttributesW(snap.host_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                        }
                        const OS_NTSTATUS st = g_real_NtSetInformationFile(
                            FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
                        if (st >= 0 && g_vfs.HasBaseGamePathW(snap.virtual_path)) {
                            (void)g_vfs.WriteWhiteoutForVirtualPathW(snap.virtual_path);
                        }
                        openscarlett::CallTelemetry::Global().Record(
                            "vfs", "NtSetInformationFile:disposition_overlay",
                            st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                            static_cast<int32_t>(st), -1);
                        return st;
                    }
                    // Read-only base handle: NEVER delete base file; record whiteout in overlay instead!
                    const bool wo_ok = g_vfs.WriteWhiteoutForVirtualPathW(snap.virtual_path);
                    const OS_NTSTATUS st = wo_ok ? OS_STATUS_SUCCESS : OS_STATUS_ACCESS_DENIED;
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = st;
                        IoStatusBlock->Information = 0u;
                    }
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs", "NtSetInformationFile:disposition_whiteout",
                        wo_ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                        static_cast<int32_t>(st), -1);
                    return st;
                } else {
                    (void)g_vfs.ClearWhiteoutForVirtualPathW(snap.virtual_path);
                    if (snap.is_overlay) {
                        return g_real_NtSetInformationFile(
                            FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
                    }
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = OS_STATUS_SUCCESS;
                        IoStatusBlock->Information = 0u;
                    }
                    return OS_STATUS_SUCCESS;
                }
            }

            if (FileInformationClass == OS_FileRenameInformation ||
                FileInformationClass == OS_FileRenameInformationEx) {
                const size_t hdr_bytes = offsetof(OS_FILE_RENAME_INFORMATION, FileName);
                if (Length < hdr_bytes) {
                    return OS_STATUS_INFO_LENGTH_MISMATCH;
                }
                const auto* rinfo = static_cast<const OS_FILE_RENAME_INFORMATION*>(FileInformation);
                if (rinfo->FileNameLength == 0u || hdr_bytes + rinfo->FileNameLength > Length) {
                    return OS_STATUS_INVALID_PARAMETER;
                }
                const bool replace_existing =
                    (FileInformationClass == OS_FileRenameInformation)
                        ? (rinfo->ReplaceIfExists != 0)
                        : ((rinfo->Flags & OS_FILE_RENAME_REPLACE_IF_EXISTS) != 0u);

                std::wstring raw_dst(rinfo->FileName,
                                     static_cast<size_t>(rinfo->FileNameLength) / sizeof(wchar_t));
                while (!raw_dst.empty() && raw_dst.back() == L'\0') {
                    raw_dst.pop_back();
                }

                std::wstring dst_vpath;
                if (rinfo->RootDirectory != nullptr) {
                    TrackedVfsHandle root_snap{};
                    if (LookupTrackedVfsHandleSnapshot(rinfo->RootDirectory, root_snap)) {
                        std::wstring_view rv = raw_dst;
                        while (!rv.empty() && (rv.front() == L'\\' || rv.front() == L'/')) {
                            rv.remove_prefix(1u);
                        }
                        dst_vpath = NormalizeCanonicalVirtualPathW(
                            JoinVirtualChildW(root_snap.virtual_path, rv));
                    }
                }
                if (dst_vpath.empty()) {
                    if (openscarlett::VfsMounts::DetectVirtualDriveW(raw_dst) !=
                        openscarlett::VfsDrive::None) {
                        dst_vpath = NormalizeCanonicalVirtualPathW(raw_dst);
                    } else {
                        // Relative name or host path: resolve relative to parent virtual dir.
                        std::wstring mapped;
                        if (g_vfs.TryMapHostPathToVirtualW(raw_dst, mapped, nullptr)) {
                            dst_vpath = NormalizeCanonicalVirtualPathW(mapped);
                        } else {
                            std::wstring parent_v = snap.virtual_path;
                            const size_t slash = parent_v.rfind(L'\\');
                            if (slash != std::wstring::npos && slash >= 2u) {
                                parent_v.resize(slash);
                            }
                            std::wstring_view rv = raw_dst;
                            while (!rv.empty() && (rv.front() == L'\\' || rv.front() == L'/')) {
                                rv.remove_prefix(1u);
                            }
                            dst_vpath = NormalizeCanonicalVirtualPathW(JoinVirtualChildW(parent_v, rv));
                        }
                    }
                }

                if (!replace_existing && g_vfs.IsVisibleGamePathW(dst_vpath)) {
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = OS_STATUS_OBJECT_NAME_COLLISION;
                        IoStatusBlock->Information = 0u;
                    }
                    return OS_STATUS_OBJECT_NAME_COLLISION;
                }

                std::wstring dst_host;
                bool dst_wo = false;
                const auto dst_st = g_vfs.TranslateForAccessW(
                    dst_vpath, openscarlett::VfsAccessMode::WriteCreate, dst_host, &dst_wo, true);
                if (dst_st == openscarlett::VfsTranslateStatus::RejectedTraversal ||
                    dst_st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = OS_STATUS_ACCESS_DENIED;
                        IoStatusBlock->Information = 0u;
                    }
                    return OS_STATUS_ACCESS_DENIED;
                }
                if (dst_st != openscarlett::VfsTranslateStatus::Translated) {
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = OS_STATUS_OBJECT_PATH_NOT_FOUND;
                        IoStatusBlock->Information = 0u;
                    }
                    return OS_STATUS_OBJECT_PATH_NOT_FOUND;
                }

                if (snap.is_overlay) {
                    // Rewrite NT rename target to \??\<dst_overlay_host_path> so the live OS handle renames cleanly.
                    std::wstring nt_dst = dst_host;
                    if (nt_dst.size() >= 4u && nt_dst.compare(0u, 4u, L"\\\\?\\") == 0) {
                        nt_dst[1] = L'?';
                    } else if (nt_dst.size() < 4u || nt_dst.compare(0u, 4u, L"\\??\\") != 0) {
                        nt_dst.insert(0u, L"\\??\\");
                    }
                    const size_t dst_bytes = nt_dst.size() * sizeof(wchar_t);
                    std::vector<uint8_t> buf(hdr_bytes + dst_bytes, 0u);
                    auto* rewritten = reinterpret_cast<OS_FILE_RENAME_INFORMATION*>(buf.data());
                    if (FileInformationClass == OS_FileRenameInformation) {
                        rewritten->ReplaceIfExists = rinfo->ReplaceIfExists;
                    } else {
                        rewritten->Flags = rinfo->Flags;
                    }
                    rewritten->RootDirectory = nullptr;
                    rewritten->FileNameLength = static_cast<ULONG>(dst_bytes);
                    std::memcpy(rewritten->FileName, nt_dst.data(), dst_bytes);

                    const OS_NTSTATUS st = g_real_NtSetInformationFile(
                        FileHandle,
                        IoStatusBlock,
                        buf.data(),
                        static_cast<ULONG>(buf.size()),
                        FileInformationClass);
                    if (st >= 0) {
                        if (g_vfs.HasBaseGamePathW(snap.virtual_path)) {
                            (void)g_vfs.WriteWhiteoutForVirtualPathW(snap.virtual_path);
                        }
                        ::AcquireSRWLockExclusive(&g_vfs_handle_lock);
                        auto hit = g_vfs_handles.find(FileHandle);
                        if (hit != g_vfs_handles.end()) {
                            hit->second.virtual_path = dst_vpath;
                            hit->second.host_path = StripNtOrExtPrefixW(dst_host);
                            hit->second.is_overlay = true;
                        }
                        ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
                    }
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs", "NtSetInformationFile:rename_overlay",
                        st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                        static_cast<int32_t>(st), -1);
                    return st;
                }

                // Read-only base handle: CoW copy to overlay dst + whiteout old virtual path (NEVER touch base!).
                std::error_code ec;
                std::filesystem::path dst_p(dst_host);
                if (dst_p.has_parent_path()) {
                    std::filesystem::create_directories(dst_p.parent_path(), ec);
                }
                const BOOL copied = ::CopyFileW(snap.host_path.c_str(), dst_host.c_str(), replace_existing ? FALSE : TRUE);
                if (!copied) {
                    const DWORD err = ::GetLastError();
                    const OS_NTSTATUS st = (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
                                               ? OS_STATUS_OBJECT_NAME_COLLISION
                                               : OS_STATUS_ACCESS_DENIED;
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = st;
                        IoStatusBlock->Information = 0u;
                    }
                    return st;
                }
                const DWORD dst_attrs = ::GetFileAttributesW(dst_host.c_str());
                if (dst_attrs != INVALID_FILE_ATTRIBUTES && (dst_attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
                    ::SetFileAttributesW(dst_host.c_str(), dst_attrs & ~FILE_ATTRIBUTE_READONLY);
                }
                (void)g_vfs.WriteWhiteoutForVirtualPathW(snap.virtual_path);
                ::AcquireSRWLockExclusive(&g_vfs_handle_lock);
                auto hit = g_vfs_handles.find(FileHandle);
                if (hit != g_vfs_handles.end()) {
                    hit->second.virtual_path = dst_vpath;
                }
                ::ReleaseSRWLockExclusive(&g_vfs_handle_lock);
                if (IoStatusBlock) {
                    IoStatusBlock->Status = OS_STATUS_SUCCESS;
                    IoStatusBlock->Information = 0u;
                }
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "NtSetInformationFile:rename_cow", openscarlett::CallOutcome::Ok, 0, -1);
                return OS_STATUS_SUCCESS;
            }

            if (FileInformationClass == OS_FileEndOfFileInformation ||
                FileInformationClass == OS_FileAllocationInformation) {
                if (Length < sizeof(LARGE_INTEGER)) {
                    return OS_STATUS_INFO_LENGTH_MISMATCH;
                }
                if (!snap.is_overlay) {
                    // Never allow mutating EOF/allocation of a raw read-only base handle.
                    if (IoStatusBlock) {
                        IoStatusBlock->Status = OS_STATUS_ACCESS_DENIED;
                        IoStatusBlock->Information = 0u;
                    }
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs", "NtSetInformationFile:eof_base_blocked",
                        openscarlett::CallOutcome::Error, static_cast<int32_t>(OS_STATUS_ACCESS_DENIED), -1);
                    return OS_STATUS_ACCESS_DENIED;
                }
                const OS_NTSTATUS st = g_real_NtSetInformationFile(
                    FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "NtSetInformationFile:eof_overlay",
                    st >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(st), -1);
                return st;
            }
        }
    }
    return g_real_NtSetInformationFile(
        FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

// ---------------------------------------------------------------------------
// Hook Implementations: Read-Only CoW Overlay Virtual Drive VFS
//   Supports G:\, GAME:\, D:\, T:\, S:\, P:\ (never mutates target game package dir!)
// ---------------------------------------------------------------------------
bool ResolveVirtualOpenPathW(std::wstring_view virtual_path,
                             const char* api_tag,
                             DWORD dwDesiredAccess,
                             DWORD dwCreationDisposition,
                             DWORD dwFlagsAndAttributes,
                             std::wstring& out_host_path,
                             openscarlett::VfsDrive& out_drive,
                             DWORD& out_effective_disposition,
                             bool& out_passthrough,
                             DWORD& out_error) {
    out_host_path.clear();
    out_drive = openscarlett::VfsMounts::DetectVirtualDriveW(virtual_path);
    out_effective_disposition = dwCreationDisposition;
    out_passthrough = false;
    out_error = ERROR_SUCCESS;

    constexpr DWORD kWriteAccessMask =
        GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    const bool wants_write =
        (dwDesiredAccess & kWriteAccessMask) != 0 ||
        dwCreationDisposition == CREATE_NEW ||
        dwCreationDisposition == CREATE_ALWAYS ||
        dwCreationDisposition == TRUNCATE_EXISTING;

    bool was_whiteout = false;
    openscarlett::VfsTranslateStatus st = openscarlett::VfsTranslateStatus::NotVirtual;

    if (out_drive == openscarlett::VfsDrive::Game) {
        if (!wants_write && dwCreationDisposition == OPEN_EXISTING) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, out_host_path, &was_whiteout, true);
        } else if (!wants_write && dwCreationDisposition == OPEN_ALWAYS) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, out_host_path, &was_whiteout, true);
            if (st != openscarlett::VfsTranslateStatus::RejectedTraversal) {
                if (!g_vfs.IsVisibleGamePathW(virtual_path)) {
                    st = g_vfs.TranslateForAccessW(
                        virtual_path, openscarlett::VfsAccessMode::WriteCreate, out_host_path, &was_whiteout, true);
                }
            }
        } else if (dwCreationDisposition == CREATE_NEW) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, out_host_path, &was_whiteout, true);
            if (st != openscarlett::VfsTranslateStatus::RejectedTraversal) {
                if (g_vfs.IsVisibleGamePathW(virtual_path)) {
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs", api_tag, openscarlett::CallOutcome::Error,
                        static_cast<int32_t>(ERROR_FILE_EXISTS), -1);
                    out_error = ERROR_FILE_EXISTS;
                    return false;
                }
                st = g_vfs.TranslateForAccessW(
                    virtual_path, openscarlett::VfsAccessMode::WriteCreate, out_host_path, &was_whiteout, true);
            }
        } else if (dwCreationDisposition == CREATE_ALWAYS) {
            // Prepare overlay for overwrite/create while stripping FILE_ATTRIBUTE_READONLY on
            // any existing overlay copy and NEVER touching the read-only base game file.
            st = g_vfs.PrepareOverlayTruncateW(virtual_path, true, out_host_path, &was_whiteout);
        } else if (dwCreationDisposition == TRUNCATE_EXISTING) {
            // Win32 CreateFileW/CreateFile2 require GENERIC_WRITE (or GENERIC_ALL) for TRUNCATE_EXISTING.
            if ((dwDesiredAccess & (GENERIC_WRITE | GENERIC_ALL)) == 0) {
                out_error = ERROR_INVALID_PARAMETER;
                return false;
            }
            bool is_dir = false;
            if (g_vfs.IsVisibleGamePathW(virtual_path, &is_dir) && is_dir) {
                out_error = ((dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) != 0)
                                ? ERROR_INVALID_PARAMETER
                                : ERROR_ACCESS_DENIED;
                return false;
            }
            // CoW copy-up un-overlaid read-only game files into overlay and strip READONLY
            // BEFORE Win32 truncates the overlay copy to 0 bytes (base file stays 100% intact).
            st = g_vfs.PrepareOverlayTruncateW(virtual_path, false, out_host_path, &was_whiteout);
        } else if (dwCreationDisposition == OPEN_EXISTING) {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::WriteModify, out_host_path, &was_whiteout, true);
            if (st == openscarlett::VfsTranslateStatus::Translated &&
                IsHostPathUnderOverlayW(out_host_path)) {
                const DWORD attrs = g_real_GetFileAttributesW
                                        ? g_real_GetFileAttributesW(out_host_path.c_str())
                                        : ::GetFileAttributesW(out_host_path.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
                    ::SetFileAttributesW(out_host_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                }
            }
        } else {
            st = g_vfs.TranslateForAccessW(
                virtual_path, openscarlett::VfsAccessMode::ReadOnly, out_host_path, &was_whiteout, true);
            if (st != openscarlett::VfsTranslateStatus::RejectedTraversal) {
                const auto mode = g_vfs.IsVisibleGamePathW(virtual_path)
                                      ? openscarlett::VfsAccessMode::WriteModify
                                      : openscarlett::VfsAccessMode::WriteCreate;
                st = g_vfs.TranslateForAccessW(virtual_path, mode, out_host_path, &was_whiteout, true);
                if (st == openscarlett::VfsTranslateStatus::Translated &&
                    IsHostPathUnderOverlayW(out_host_path)) {
                    const DWORD attrs = g_real_GetFileAttributesW
                                            ? g_real_GetFileAttributesW(out_host_path.c_str())
                                            : ::GetFileAttributesW(out_host_path.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
                        ::SetFileAttributesW(out_host_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                    }
                }
            }
        }
    } else {
        const auto mode = wants_write ? openscarlett::VfsAccessMode::WriteCreate
                                      : openscarlett::VfsAccessMode::ReadOnly;
        st = g_vfs.TranslateForAccessW(virtual_path, mode, out_host_path, &was_whiteout, true);
    }

    if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":traversal_blocked", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
        out_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":readonly_blocked", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
        out_error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
        openscarlett::CallTelemetry::Global().Record(
            "vfs", std::string(api_tag) + ":whiteout", openscarlett::CallOutcome::Error,
            static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
        out_error = ERROR_FILE_NOT_FOUND;
        return false;
    }
    if (st == openscarlett::VfsTranslateStatus::NotVirtual) {
        out_passthrough = true;
        return true;
    }
    if (st != openscarlett::VfsTranslateStatus::Translated) {
        out_error = ERROR_PATH_NOT_FOUND;
        return false;
    }
    return true;
}

void TraceVfsFailureW(const char* api_tag,
                      std::wstring_view raw_path,
                      std::wstring_view host_path,
                      DWORD access,
                      DWORD disp,
                      DWORD err) {
    static const std::string s_trace_path = []() {
        std::wstring w = ReadEnvW(L"OPENSCARLETT_TRACE_VFS_ERR");
        return openscarlett::WideToUtf8(w);
    }();
    if (s_trace_path.empty()) {
        return;
    }
    FILE* f = nullptr;
    if (::fopen_s(&f, s_trace_path.c_str(), "a") == 0 && f) {
        const std::string raw_u8 = openscarlett::WideToUtf8(raw_path);
        const std::string host_u8 = openscarlett::WideToUtf8(host_path);
        std::fprintf(f, "[VFS_ERR] tid=%lu api=%s err=%lu access=0x%08lX disp=%lu raw=%s host=%s\n",
                     static_cast<unsigned long>(::GetCurrentThreadId()),
                     api_tag ? api_tag : "",
                     static_cast<unsigned long>(err),
                     static_cast<unsigned long>(access),
                     static_cast<unsigned long>(disp),
                     raw_u8.c_str(),
                     host_u8.c_str());
        std::fclose(f);
    }
}

HANDLE CreateVirtualFileW(std::wstring_view virtual_path,
                          const char* api_tag,
                          DWORD dwDesiredAccess,
                          DWORD dwShareMode,
                          LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                          DWORD dwCreationDisposition,
                          DWORD dwFlagsAndAttributes,
                          HANDLE hTemplateFile) {
    std::wstring host_path;
    openscarlett::VfsDrive drive = openscarlett::VfsDrive::None;
    DWORD effective_disposition = dwCreationDisposition;
    bool passthrough = false;
    DWORD err = ERROR_SUCCESS;
    PfnCreateFileW real_cfw = g_real_CreateFileW ? g_real_CreateFileW : &::CreateFileW;
    if (!ResolveVirtualOpenPathW(virtual_path, api_tag, dwDesiredAccess, dwCreationDisposition,
                                 dwFlagsAndAttributes, host_path, drive, effective_disposition,
                                 passthrough, err)) {
        TraceVfsFailureW(api_tag, virtual_path, L"<resolve_failed>", dwDesiredAccess, dwCreationDisposition, err);
        ::SetLastError(err);
        return INVALID_HANDLE_VALUE;
    }
    if (passthrough) {
        const std::wstring passthrough_path(virtual_path);
        HANDLE h = real_cfw(passthrough_path.c_str(),
                            dwDesiredAccess,
                            dwShareMode,
                            lpSecurityAttributes,
                            dwCreationDisposition,
                            dwFlagsAndAttributes,
                            hTemplateFile);
        const DWORD e = ::GetLastError();
        if (h != INVALID_HANDLE_VALUE) {
            UntrackVfsHandleIfPresent(h);
        } else {
            TraceVfsFailureW(api_tag, virtual_path, passthrough_path, dwDesiredAccess, dwCreationDisposition, e);
        }
        ::SetLastError(e);
        return h;
    }

    HANDLE h = real_cfw(host_path.c_str(),
                        dwDesiredAccess,
                        dwShareMode,
                        lpSecurityAttributes,
                        effective_disposition,
                        dwFlagsAndAttributes,
                        hTemplateFile);
    err = ::GetLastError();
    if (h != INVALID_HANDLE_VALUE) {
        TrackVfsHandle(h, virtual_path, host_path, drive);
    } else {
        TraceVfsFailureW(api_tag, virtual_path, host_path, dwDesiredAccess, dwCreationDisposition, err);
    }
    openscarlett::CallTelemetry::Global().Record(
        "vfs",
        api_tag,
        h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
        -1);
    ::SetLastError(err);
    return h;
}

HANDLE WINAPI Hook_CreateFile2(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               DWORD dwCreationDisposition,
                               OS_CREATEFILE2_EXTENDED_PARAMETERS* pCreateExParams) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
        std::wstring host_path;
        openscarlett::VfsDrive drive = openscarlett::VfsDrive::None;
        DWORD effective_disposition = dwCreationDisposition;
        bool passthrough = false;
        DWORD err = ERROR_SUCCESS;
        const DWORD flags_and_attrs =
            pCreateExParams ? (pCreateExParams->dwFileFlags | pCreateExParams->dwFileAttributes) : 0u;
        if (!ResolveVirtualOpenPathW(lpFileName, "CreateFile2", dwDesiredAccess,
                                     dwCreationDisposition, flags_and_attrs, host_path, drive,
                                     effective_disposition, passthrough, err)) {
            TraceVfsFailureW("CreateFile2", lpFileName, L"<resolve_failed>",
                             dwDesiredAccess, dwCreationDisposition, err);
            ::SetLastError(err);
            return INVALID_HANDLE_VALUE;
        }
        if (!passthrough) {
            HANDLE h = g_real_CreateFile2(host_path.c_str(),
                                          dwDesiredAccess,
                                          dwShareMode,
                                          effective_disposition,
                                          pCreateExParams);
            err = ::GetLastError();
            if (h != INVALID_HANDLE_VALUE) {
                TrackVfsHandle(h, lpFileName, host_path, drive);
            } else {
                TraceVfsFailureW("CreateFile2", lpFileName, host_path,
                                 dwDesiredAccess, dwCreationDisposition, err);
            }
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "CreateFile2",
                h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok
                                          : openscarlett::CallOutcome::Error,
                h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return h;
        }
    }
    HANDLE h = g_real_CreateFile2(
        lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
    if (h != INVALID_HANDLE_VALUE && !g_shutting_down.load(std::memory_order_acquire)) {
        const DWORD e = ::GetLastError();
        UntrackVfsHandleIfPresent(h);
        ::SetLastError(e);
    }
    return h;
}

BOOL WINAPI Hook_CloseHandle(HANDLE hObject) {
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        UntrackVfsHandle(hObject);
    }
    return g_real_CloseHandle(hObject);
}

DWORD WINAPI Hook_GetFinalPathNameByHandleW(HANDLE hFile,
                                            LPWSTR lpszFilePath,
                                            DWORD cchFilePath,
                                            DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        hFile != nullptr && hFile != INVALID_HANDLE_VALUE) {
        std::wstring virtual_path;
        TrackedVfsHandle snap{};
        if (LookupTrackedVfsHandleSnapshot(hFile, snap) && !snap.virtual_path.empty()) {
            virtual_path = snap.virtual_path;
        } else if (g_real_GetFinalPathNameByHandleW != nullptr) {
            // Only normalize untracked handles if they point into the internal G_overlay
            // directory (preventing overlay prefix leakage without rewriting direct host
            // paths under g_root / t_root / s_root that were never opened via VFS).
            std::wstring probe(512u, L'\0');
            DWORD got = g_real_GetFinalPathNameByHandleW(
                hFile, probe.data(), static_cast<DWORD>(probe.size()), 0u /*VOLUME_NAME_DOS*/);
            if (got >= probe.size() && got > 0u) {
                probe.resize(static_cast<size_t>(got) + 1u);
                got = g_real_GetFinalPathNameByHandleW(
                    hFile, probe.data(), static_cast<DWORD>(probe.size()), 0u);
            }
            if (got > 0u && got < probe.size()) {
                probe.resize(got);
                std::wstring mapped;
                bool from_overlay = false;
                if (g_vfs.TryMapHostPathToVirtualW(probe, mapped, &from_overlay) && from_overlay) {
                    virtual_path = NormalizeCanonicalVirtualPathW(mapped);
                }
            }
        }

        if (!virtual_path.empty()) {
            std::wstring formatted;
            const DWORD vol_mode = dwFlags & 0x7u;
            if (vol_mode == 0x4u /*VOLUME_NAME_NONE*/) {
                if (virtual_path.size() >= 2u && virtual_path[1] == L':') {
                    formatted = virtual_path.substr(2u);
                    if (formatted.empty()) formatted = L"\\";
                } else {
                    formatted = virtual_path;
                }
            } else if (vol_mode == 0x2u /*VOLUME_NAME_NT*/) {
                wchar_t drv = (virtual_path.size() >= 2u && virtual_path[1] == L':') ? virtual_path[0] : L'G';
                std::wstring rest = (virtual_path.size() >= 2u && virtual_path[1] == L':')
                                        ? virtual_path.substr(2u)
                                        : (L"\\" + virtual_path);
                if (rest.empty()) rest = L"\\";
                formatted = L"\\Device\\OpenScarlettVfs\\";
                formatted.push_back(drv);
                formatted.append(rest);
            } else {
                // VOLUME_NAME_DOS (default) or VOLUME_NAME_GUID: canonical \\?\<Drive>:\<rel>
                formatted = L"\\\\?\\" + virtual_path;
            }

            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFinalPathNameByHandleW", openscarlett::CallOutcome::Ok, 0, -1);

            if (lpszFilePath == nullptr || static_cast<size_t>(cchFilePath) <= formatted.size()) {
                ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return static_cast<DWORD>(formatted.size() + 1u);
            }
            std::memcpy(lpszFilePath, formatted.c_str(), (formatted.size() + 1u) * sizeof(wchar_t));
            ::SetLastError(ERROR_SUCCESS);
            return static_cast<DWORD>(formatted.size());
        }
    }
    return g_real_GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
        return CreateVirtualFileW(lpFileName,
                                  "CreateFileW",
                                  dwDesiredAccess,
                                  dwShareMode,
                                  lpSecurityAttributes,
                                  dwCreationDisposition,
                                  dwFlagsAndAttributes,
                                  hTemplateFile);
    }
    HANDLE h = g_real_CreateFileW(lpFileName,
                                  dwDesiredAccess,
                                  dwShareMode,
                                  lpSecurityAttributes,
                                  dwCreationDisposition,
                                  dwFlagsAndAttributes,
                                  hTemplateFile);
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        if (h != INVALID_HANDLE_VALUE) {
            const DWORD e = ::GetLastError();
            UntrackVfsHandleIfPresent(h);
            ::SetLastError(e);
        } else if (tl_vfs_hook_depth == 0 && lpFileName != nullptr) {
            const DWORD e = ::GetLastError();
            ScopedVfsHookGuard guard;
            TraceVfsFailureW("CreateFileW:direct", lpFileName, lpFileName, dwDesiredAccess, dwCreationDisposition, e);
            ::SetLastError(e);
        }
    }
    return h;
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        ScopedVfsHookGuard guard;
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
    HANDLE h = g_real_CreateFileA(lpFileName,
                                  dwDesiredAccess,
                                  dwShareMode,
                                  lpSecurityAttributes,
                                  dwCreationDisposition,
                                  dwFlagsAndAttributes,
                                  hTemplateFile);
    if (h != INVALID_HANDLE_VALUE && !g_shutting_down.load(std::memory_order_acquire)) {
        const DWORD e = ::GetLastError();
        UntrackVfsHandleIfPresent(h);
        ::SetLastError(e);
    }
    return h;
}

DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR lpFileName) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
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
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        ScopedVfsHookGuard guard;
        std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
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
    }
    return g_real_GetFileAttributesA(lpFileName);
}

BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
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

BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        ScopedVfsHookGuard guard;
        const std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
            std::wstring host_path;
            bool was_wo = false;
            auto st = g_vfs.TranslateForAccessW(
                wide_in, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
            if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "GetFileAttributesExA:traversal_blocked", openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
                ::SetLastError(ERROR_ACCESS_DENIED);
                return FALSE;
            }
            if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
                openscarlett::CallTelemetry::Global().Record(
                    "vfs", "GetFileAttributesExA:whiteout", openscarlett::CallOutcome::Error,
                    static_cast<int32_t>(ERROR_FILE_NOT_FOUND), -1);
                ::SetLastError(ERROR_FILE_NOT_FOUND);
                return FALSE;
            }
            if (st == openscarlett::VfsTranslateStatus::Translated && g_real_GetFileAttributesExW) {
                BOOL ok = g_real_GetFileAttributesExW(host_path.c_str(), fInfoLevelId, lpFileInformation);
                DWORD err = ::GetLastError();
                openscarlett::CallTelemetry::Global().Record(
                    "vfs",
                    "GetFileAttributesExA",
                    ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                    ok ? 0 : static_cast<int32_t>(err),
                    -1);
                ::SetLastError(err);
                return ok;
            }
        }
    }
    return g_real_GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

BOOL WINAPI Hook_SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
        std::wstring host_path;
        bool was_wo = false;
        auto st = g_vfs.TranslateForAccessW(
            lpFileName, openscarlett::VfsAccessMode::WriteModify, host_path, &was_wo, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal ||
            st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            return g_real_SetFileAttributesW(host_path.c_str(), dwFileAttributes);
        }
    }
    return g_real_SetFileAttributesW(lpFileName, dwFileAttributes);
}

BOOL WINAPI Hook_SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in) && g_real_SetFileAttributesW) {
            return Hook_SetFileAttributesW(wide_in.c_str(), dwFileAttributes);
        }
    }
    return g_real_SetFileAttributesA(lpFileName, dwFileAttributes);
}
BOOL WINAPI Hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpPathName)) {
        ScopedVfsHookGuard guard;
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

BOOL WINAPI Hook_CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpPathName) != openscarlett::VfsDrive::None) {
        const std::wstring wide_in = AnsiToWide(lpPathName);
        if (!ShouldPassThroughHostDDriveW(wide_in) && g_real_CreateDirectoryW) {
            return Hook_CreateDirectoryW(wide_in.c_str(), lpSecurityAttributes);
        }
    }
    return g_real_CreateDirectoryA(lpPathName, lpSecurityAttributes);
}

BOOL WINAPI Hook_DeleteFileW(LPCWSTR lpFileName) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
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
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        ScopedVfsHookGuard guard;
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
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpPathName)) {
        ScopedVfsHookGuard guard;
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

BOOL WINAPI Hook_RemoveDirectoryA(LPCSTR lpPathName) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpPathName) != openscarlett::VfsDrive::None) {
        ScopedVfsHookGuard guard;
        const std::wstring wide_in = AnsiToWide(lpPathName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
            uint32_t err = 0;
            bool ok = g_vfs.RemoveVirtualDirectoryW(wide_in, err);
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "RemoveDirectoryA",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
            return ok ? TRUE : FALSE;
        }
    }
    return g_real_RemoveDirectoryA(lpPathName);
}

BOOL WINAPI Hook_MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpExistingFileName != nullptr && lpNewFileName != nullptr &&
        ((openscarlett::VfsMounts::DetectVirtualDriveW(lpExistingFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpExistingFileName)) ||
         (openscarlett::VfsMounts::DetectVirtualDriveW(lpNewFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpNewFileName)))) {
        ScopedVfsHookGuard guard;
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

BOOL WINAPI Hook_MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpExistingFileName != nullptr && lpNewFileName != nullptr &&
        ((openscarlett::VfsMounts::DetectVirtualDriveW(lpExistingFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpExistingFileName)) ||
         (openscarlett::VfsMounts::DetectVirtualDriveW(lpNewFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpNewFileName)))) {
        return Hook_MoveFileExW(lpExistingFileName, lpNewFileName, 0u);
    }
    return g_real_MoveFileW(lpExistingFileName, lpNewFileName);
}

BOOL WINAPI Hook_MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpExistingFileName != nullptr && lpNewFileName != nullptr) {
        const bool src_v =
            openscarlett::VfsMounts::DetectVirtualDriveA(lpExistingFileName) != openscarlett::VfsDrive::None;
        const bool dst_v =
            openscarlett::VfsMounts::DetectVirtualDriveA(lpNewFileName) != openscarlett::VfsDrive::None;
        if (src_v || dst_v) {
            ScopedVfsHookGuard guard;
            const std::wstring wide_src = AnsiToWide(lpExistingFileName);
            const std::wstring wide_dst = AnsiToWide(lpNewFileName);
            if ((src_v && !ShouldPassThroughHostDDriveW(wide_src)) ||
                (dst_v && !ShouldPassThroughHostDDriveW(wide_dst))) {
                uint32_t err = 0;
                bool ok = g_vfs.MoveVirtualPathW(wide_src, wide_dst, dwFlags, err);
                openscarlett::CallTelemetry::Global().Record(
                    "vfs",
                    "MoveFileExA",
                    ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                    ok ? 0 : static_cast<int32_t>(err),
                    -1);
                ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
                return ok ? TRUE : FALSE;
            }
        }
    }
    return g_real_MoveFileExA(lpExistingFileName, lpNewFileName, dwFlags);
}

BOOL CopyVirtualFileImplW(std::wstring_view src_path,
                          std::wstring_view dst_path,
                          BOOL bFailIfExists,
                          const char* api_tag) {
    ScopedVfsHookGuard guard;
    std::wstring src_host;
    if (openscarlett::VfsMounts::DetectVirtualDriveW(src_path) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(src_path)) {
        auto st = g_vfs.TranslateForAccessW(
            src_path, openscarlett::VfsAccessMode::ReadOnly, src_host, nullptr, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal ||
            st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        if (st != openscarlett::VfsTranslateStatus::Translated) {
            ::SetLastError(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
    } else {
        src_host.assign(src_path);
    }

    std::wstring dst_host;
    const auto dst_drive = openscarlett::VfsMounts::DetectVirtualDriveW(dst_path);
    if (dst_drive != openscarlett::VfsDrive::None && !ShouldPassThroughHostDDriveW(dst_path)) {
        if (bFailIfExists && dst_drive == openscarlett::VfsDrive::Game && g_vfs.IsVisibleGamePathW(dst_path)) {
            ::SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        auto st = g_vfs.TranslateForAccessW(
            dst_path, openscarlett::VfsAccessMode::WriteCreate, dst_host, nullptr, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal ||
            st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st != openscarlett::VfsTranslateStatus::Translated) {
            ::SetLastError(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
    } else {
        dst_host.assign(dst_path);
    }

    const BOOL ok = g_real_CopyFileW ? g_real_CopyFileW(src_host.c_str(), dst_host.c_str(), bFailIfExists)
                                     : ::CopyFileW(src_host.c_str(), dst_host.c_str(), bFailIfExists);
    const DWORD err = ::GetLastError();
    openscarlett::CallTelemetry::Global().Record(
        "vfs",
        api_tag,
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        ok ? 0 : static_cast<int32_t>(err),
        -1);
    ::SetLastError(err);
    return ok;
}

BOOL WINAPI Hook_CopyFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, BOOL bFailIfExists) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpExistingFileName != nullptr && lpNewFileName != nullptr &&
        ((openscarlett::VfsMounts::DetectVirtualDriveW(lpExistingFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpExistingFileName)) ||
         (openscarlett::VfsMounts::DetectVirtualDriveW(lpNewFileName) != openscarlett::VfsDrive::None &&
          !ShouldPassThroughHostDDriveW(lpNewFileName)))) {
        return CopyVirtualFileImplW(lpExistingFileName, lpNewFileName, bFailIfExists, "CopyFileW");
    }
    return g_real_CopyFileW(lpExistingFileName, lpNewFileName, bFailIfExists);
}

HRESULT WINAPI Hook_CopyFile2(PCWSTR pwszExistingFileName,
                              PCWSTR pwszNewFileName,
                              COPYFILE2_EXTENDED_PARAMETERS* pExtendedParameters) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        pwszExistingFileName != nullptr && pwszNewFileName != nullptr) {
        const bool src_virt =
            openscarlett::VfsMounts::DetectVirtualDriveW(pwszExistingFileName) != openscarlett::VfsDrive::None &&
            !ShouldPassThroughHostDDriveW(pwszExistingFileName);
        const bool dst_virt =
            openscarlett::VfsMounts::DetectVirtualDriveW(pwszNewFileName) != openscarlett::VfsDrive::None &&
            !ShouldPassThroughHostDDriveW(pwszNewFileName);
        if (src_virt || dst_virt) {
            ScopedVfsHookGuard guard;
            std::wstring src_host;
            std::wstring dst_host;
            PCWSTR eff_src = pwszExistingFileName;
            PCWSTR eff_dst = pwszNewFileName;
            if (src_virt) {
                auto st = g_vfs.TranslateForAccessW(
                    pwszExistingFileName, openscarlett::VfsAccessMode::ReadOnly, src_host, nullptr, true);
                if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
                    return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
                }
                if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
                    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                }
                if (st == openscarlett::VfsTranslateStatus::Translated) {
                    eff_src = src_host.c_str();
                }
            }
            if (dst_virt) {
                auto st = g_vfs.TranslateForAccessW(
                    pwszNewFileName, openscarlett::VfsAccessMode::WriteCreate, dst_host, nullptr, true);
                if (st == openscarlett::VfsTranslateStatus::RejectedTraversal ||
                    st == openscarlett::VfsTranslateStatus::ReadOnlyViolation) {
                    return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
                }
                if (st == openscarlett::VfsTranslateStatus::Translated) {
                    eff_dst = dst_host.c_str();
                }
            }
            return g_real_CopyFile2(eff_src, eff_dst, pExtendedParameters);
        }
    }
    return g_real_CopyFile2(pwszExistingFileName, pwszNewFileName, pExtendedParameters);
}

BOOL WINAPI Hook_CopyFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpExistingFileName != nullptr && lpNewFileName != nullptr) {
        const bool src_v =
            openscarlett::VfsMounts::DetectVirtualDriveA(lpExistingFileName) != openscarlett::VfsDrive::None;
        const bool dst_v =
            openscarlett::VfsMounts::DetectVirtualDriveA(lpNewFileName) != openscarlett::VfsDrive::None;
        if (src_v || dst_v) {
            const std::wstring wide_src = AnsiToWide(lpExistingFileName);
            const std::wstring wide_dst = AnsiToWide(lpNewFileName);
            if ((src_v && !ShouldPassThroughHostDDriveW(wide_src)) ||
                (dst_v && !ShouldPassThroughHostDDriveW(wide_dst))) {
                return CopyVirtualFileImplW(wide_src, wide_dst, bFailIfExists, "CopyFileA");
            }
        }
    }
    return g_real_CopyFileA(lpExistingFileName, lpNewFileName, bFailIfExists);
}

BOOL WINAPI Hook_ReplaceFileW(LPCWSTR lpReplacedFileName,
                              LPCWSTR lpReplacementFileName,
                              LPCWSTR lpBackupFileName,
                              DWORD dwReplaceFlags,
                              LPVOID lpExclude,
                              LPVOID lpReserved) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpReplacedFileName != nullptr && lpReplacementFileName != nullptr) {
        const bool rep_v =
            openscarlett::VfsMounts::DetectVirtualDriveW(lpReplacedFileName) != openscarlett::VfsDrive::None &&
            !ShouldPassThroughHostDDriveW(lpReplacedFileName);
        const bool src_v =
            openscarlett::VfsMounts::DetectVirtualDriveW(lpReplacementFileName) != openscarlett::VfsDrive::None &&
            !ShouldPassThroughHostDDriveW(lpReplacementFileName);
        const bool bak_v =
            lpBackupFileName != nullptr &&
            openscarlett::VfsMounts::DetectVirtualDriveW(lpBackupFileName) != openscarlett::VfsDrive::None &&
            !ShouldPassThroughHostDDriveW(lpBackupFileName);
        if (rep_v || src_v || bak_v) {
            ScopedVfsHookGuard guard;
            uint32_t err = 0;
            const bool ok = g_vfs.ReplaceVirtualFileW(
                lpReplacedFileName,
                lpReplacementFileName,
                lpBackupFileName ? std::wstring_view(lpBackupFileName) : std::wstring_view{},
                dwReplaceFlags,
                err);
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "ReplaceFileW",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(ok ? ERROR_SUCCESS : static_cast<DWORD>(err));
            return ok ? TRUE : FALSE;
        }
    }
    return g_real_ReplaceFileW(
        lpReplacedFileName, lpReplacementFileName, lpBackupFileName, dwReplaceFlags, lpExclude, lpReserved);
}

HANDLE WINAPI Hook_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
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

HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR lpFileName,
                                    FINDEX_INFO_LEVELS fInfoLevelId,
                                    LPVOID lpFindFileData,
                                    FINDEX_SEARCH_OPS fSearchOp,
                                    LPVOID lpSearchFilter,
                                    DWORD dwAdditionalFlags) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpFileName)) {
        ScopedVfsHookGuard guard;
        const std::wstring_view in_view(lpFileName);
        const auto drive = openscarlett::VfsMounts::DetectVirtualDriveW(in_view);
        const bool has_wildcards = (in_view.find_first_of(L"*?") != std::wstring_view::npos);
        const bool is_find_data_w = (fInfoLevelId == FindExInfoStandard || fInfoLevelId == FindExInfoBasic);

        if (drive == openscarlett::VfsDrive::Game && has_wildcards && is_find_data_w && lpFindFileData != nullptr) {
            std::wstring base_pat;
            std::wstring overlay_pat;
            std::wstring rel_pat;
            bool was_wo = false;
            auto st = g_vfs.ResolveGamePairW(in_view, base_pat, overlay_pat, rel_pat, &was_wo);
            if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
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
                auto* fd = static_cast<LPWIN32_FIND_DATAW>(lpFindFileData);
                auto is_hidden = [&](const wchar_t* filename, bool from_overlay) -> bool {
                    std::wstring_view fn(filename);
                    if (from_overlay && rel_dir.empty() && fn == L".whiteout") return true;
                    if (fn == L"." || fn == L"..") return false;
                    std::wstring vpath = L"G:\\";
                    if (!rel_dir.empty()) vpath += rel_dir + L"\\";
                    vpath.append(fn);
                    return g_vfs.IsWhiteoutedW(vpath);
                };
                auto find_vis = [&](const std::wstring& pat, bool from_overlay) -> HANDLE {
                    if (pat.empty()) return INVALID_HANDLE_VALUE;
                    HANDLE h = g_real_FindFirstFileExW(
                        pat.c_str(), fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
                    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
                    while (is_hidden(fd->cFileName, from_overlay)) {
                        if (!::FindNextFileW(h, fd)) {
                            ::FindClose(h);
                            return INVALID_HANDLE_VALUE;
                        }
                    }
                    return h;
                };
                HANDLE h = find_vis(overlay_pat, true);
                if (h == INVALID_HANDLE_VALUE) {
                    h = find_vis(base_pat, false);
                }
                DWORD err = (h != INVALID_HANDLE_VALUE) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
                ::SetLastError(err);
                return h;
            }
        }

        std::wstring host_path;
        bool was_wo = false;
        auto st = g_vfs.TranslateForAccessW(
            lpFileName, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            return g_real_FindFirstFileExW(
                host_path.c_str(), fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
        }
    }
    return g_real_FindFirstFileExW(
        lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

HANDLE WINAPI Hook_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        const std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
            const auto drive = openscarlett::VfsMounts::DetectVirtualDriveW(wide_in);
            const bool has_wildcards = (wide_in.find_first_of(L"*?") != std::wstring::npos);

            if (drive == openscarlett::VfsDrive::Game && has_wildcards && lpFindFileData != nullptr) {
                std::wstring base_pat;
                std::wstring overlay_pat;
                std::wstring rel_pat;
                bool was_wo = false;
                auto st = g_vfs.ResolveGamePairW(wide_in, base_pat, overlay_pat, rel_pat, &was_wo);
                if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
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
                    auto is_hidden = [&](const char* filename, bool from_overlay) -> bool {
                        const std::wstring fn = AnsiToWide(filename);
                        if (from_overlay && rel_dir.empty() && fn == L".whiteout") return true;
                        if (fn == L"." || fn == L"..") return false;
                        std::wstring vpath = L"G:\\";
                        if (!rel_dir.empty()) vpath += rel_dir + L"\\";
                        vpath.append(fn);
                        return g_vfs.IsWhiteoutedW(vpath);
                    };
                    auto find_vis = [&](const std::wstring& pat_w, bool from_overlay) -> HANDLE {
                        if (pat_w.empty()) return INVALID_HANDLE_VALUE;
                        const std::string pat_a = WideToAnsiAcp(pat_w);
                        if (pat_a.empty()) return INVALID_HANDLE_VALUE;
                        HANDLE h = g_real_FindFirstFileA(pat_a.c_str(), lpFindFileData);
                        if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
                        while (is_hidden(lpFindFileData->cFileName, from_overlay)) {
                            if (!::FindNextFileA(h, lpFindFileData)) {
                                ::FindClose(h);
                                return INVALID_HANDLE_VALUE;
                            }
                        }
                        return h;
                    };
                    HANDLE h = find_vis(overlay_pat, true);
                    if (h == INVALID_HANDLE_VALUE) {
                        h = find_vis(base_pat, false);
                    }
                    DWORD err = (h != INVALID_HANDLE_VALUE) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs",
                        "FindFirstFileA",
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
                wide_in, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
            if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
                ::SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
                ::SetLastError(ERROR_FILE_NOT_FOUND);
                return INVALID_HANDLE_VALUE;
            }
            if (st == openscarlett::VfsTranslateStatus::Translated) {
                const std::string host_a = WideToAnsiAcp(host_path);
                if (!host_a.empty()) {
                    HANDLE h = g_real_FindFirstFileA(host_a.c_str(), lpFindFileData);
                    DWORD err = ::GetLastError();
                    openscarlett::CallTelemetry::Global().Record(
                        "vfs",
                        "FindFirstFileA",
                        h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                        h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                        -1);
                    ::SetLastError(err);
                    return h;
                }
            }
        }
    }
    return g_real_FindFirstFileA(lpFileName, lpFindFileData);
}

HANDLE WINAPI Hook_FindFirstFileExA(LPCSTR lpFileName,
                                    FINDEX_INFO_LEVELS fInfoLevelId,
                                    LPVOID lpFindFileData,
                                    FINDEX_SEARCH_OPS fSearchOp,
                                    LPVOID lpSearchFilter,
                                    DWORD dwAdditionalFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        const std::wstring wide_in = AnsiToWide(lpFileName);
        if (!ShouldPassThroughHostDDriveW(wide_in)) {
            const auto drive = openscarlett::VfsMounts::DetectVirtualDriveW(wide_in);
            const bool has_wildcards = (wide_in.find_first_of(L"*?") != std::wstring::npos);
            const bool is_find_data_a =
                (fInfoLevelId == FindExInfoStandard || fInfoLevelId == FindExInfoBasic);

            if (drive == openscarlett::VfsDrive::Game && has_wildcards &&
                is_find_data_a && lpFindFileData != nullptr) {
                std::wstring base_pat;
                std::wstring overlay_pat;
                std::wstring rel_pat;
                bool was_wo = false;
                auto st = g_vfs.ResolveGamePairW(wide_in, base_pat, overlay_pat, rel_pat, &was_wo);
                if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
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
                    auto* fd = static_cast<LPWIN32_FIND_DATAA>(lpFindFileData);
                    auto is_hidden = [&](const char* filename, bool from_overlay) -> bool {
                        const std::wstring fn = AnsiToWide(filename);
                        if (from_overlay && rel_dir.empty() && fn == L".whiteout") return true;
                        if (fn == L"." || fn == L"..") return false;
                        std::wstring vpath = L"G:\\";
                        if (!rel_dir.empty()) vpath += rel_dir + L"\\";
                        vpath.append(fn);
                        return g_vfs.IsWhiteoutedW(vpath);
                    };
                    auto find_vis = [&](const std::wstring& pat_w, bool from_overlay) -> HANDLE {
                        if (pat_w.empty()) return INVALID_HANDLE_VALUE;
                        const std::string pat_a = WideToAnsiAcp(pat_w);
                        if (pat_a.empty()) return INVALID_HANDLE_VALUE;
                        HANDLE h = g_real_FindFirstFileExA(
                            pat_a.c_str(), fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
                        if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
                        while (is_hidden(fd->cFileName, from_overlay)) {
                            if (!::FindNextFileA(h, fd)) {
                                ::FindClose(h);
                                return INVALID_HANDLE_VALUE;
                            }
                        }
                        return h;
                    };
                    HANDLE h = find_vis(overlay_pat, true);
                    if (h == INVALID_HANDLE_VALUE) {
                        h = find_vis(base_pat, false);
                    }
                    DWORD err = (h != INVALID_HANDLE_VALUE) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
                    ::SetLastError(err);
                    return h;
                }
            }

            std::wstring host_path;
            bool was_wo = false;
            auto st = g_vfs.TranslateForAccessW(
                wide_in, openscarlett::VfsAccessMode::ReadOnly, host_path, &was_wo, true);
            if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
                ::SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            if (st == openscarlett::VfsTranslateStatus::DeletedWhiteout) {
                ::SetLastError(ERROR_FILE_NOT_FOUND);
                return INVALID_HANDLE_VALUE;
            }
            if (st == openscarlett::VfsTranslateStatus::Translated) {
                const std::string host_a = WideToAnsiAcp(host_path);
                if (!host_a.empty()) {
                    return g_real_FindFirstFileExA(
                        host_a.c_str(), fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
                }
            }
        }
    }
    return g_real_FindFirstFileExA(
        lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

BOOL WINAPI Hook_GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName,
                                     PULARGE_INTEGER lpFreeBytesAvailableToCaller,
                                     PULARGE_INTEGER lpTotalNumberOfBytes,
                                     PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpDirectoryName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpDirectoryName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpDirectoryName)) {
        ScopedVfsHookGuard guard;
        std::wstring host_path;
        auto st = g_vfs.TranslateForAccessW(
            lpDirectoryName, openscarlett::VfsAccessMode::ReadOnly, host_path, nullptr, true);
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            return g_real_GetDiskFreeSpaceExW(
                host_path.c_str(), lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
        }
    }
    return g_real_GetDiskFreeSpaceExW(
        lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

BOOL WINAPI Hook_GetDiskFreeSpaceW(LPCWSTR lpRootPathName,
                                   LPDWORD lpSectorsPerCluster,
                                   LPDWORD lpBytesPerSector,
                                   LPDWORD lpNumberOfFreeClusters,
                                   LPDWORD lpTotalNumberOfClusters) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpRootPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpRootPathName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpRootPathName)) {
        ScopedVfsHookGuard guard;
        std::wstring host_path;
        auto st = g_vfs.TranslateForAccessW(
            lpRootPathName, openscarlett::VfsAccessMode::ReadOnly, host_path, nullptr, true);
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            // GetDiskFreeSpaceW expects a root path or directory; ensure trailing backslash.
            if (!host_path.empty() && host_path.back() != L'\\') {
                host_path.push_back(L'\\');
            }
            return g_real_GetDiskFreeSpaceW(
                host_path.c_str(), lpSectorsPerCluster, lpBytesPerSector,
                lpNumberOfFreeClusters, lpTotalNumberOfClusters);
        }
    }
    return g_real_GetDiskFreeSpaceW(
        lpRootPathName, lpSectorsPerCluster, lpBytesPerSector,
        lpNumberOfFreeClusters, lpTotalNumberOfClusters);
}

BOOL WINAPI Hook_SetCurrentDirectoryW(LPCWSTR lpPathName) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName) != openscarlett::VfsDrive::None &&
        !ShouldPassThroughHostDDriveW(lpPathName)) {
        ScopedVfsHookGuard guard;
        std::wstring host_path;
        auto st = g_vfs.TranslateForAccessW(
            lpPathName, openscarlett::VfsAccessMode::ReadOnly, host_path, nullptr, true);
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            return g_real_SetCurrentDirectoryW(host_path.c_str());
        }
    }
    return g_real_SetCurrentDirectoryW(lpPathName);
}

BOOL WINAPI Hook_SetCurrentDirectoryA(LPCSTR lpPathName) {
    if (tl_vfs_hook_depth == 0 &&
        !g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpPathName) != openscarlett::VfsDrive::None) {
        ScopedVfsHookGuard guard;
        const std::wstring wide = openscarlett::Utf8ToWide(lpPathName);
        if (!ShouldPassThroughHostDDriveW(wide)) {
            std::wstring host_path;
            auto st = g_vfs.TranslateForAccessW(
                wide, openscarlett::VfsAccessMode::ReadOnly, host_path, nullptr, true);
            if (st == openscarlett::VfsTranslateStatus::Translated && g_real_SetCurrentDirectoryW) {
                return g_real_SetCurrentDirectoryW(host_path.c_str());
            }
        }
    }
    return g_real_SetCurrentDirectoryA(lpPathName);
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

LPVOID WINAPI Hook_WinHttpConnect(LPVOID hSession, LPCWSTR pswzServerName, WORD nServerPort, DWORD dwReserved) {
    if (g_offline_block_winhttp) {
        ::SetLastError(12029u /*ERROR_WINHTTP_CANNOT_CONNECT*/);
        return nullptr;
    }
    return g_real_WinHttpConnect
               ? g_real_WinHttpConnect(hSession, pswzServerName, nServerPort, dwReserved)
               : nullptr;
}

void EnsureComProcsResolved() noexcept {
    if (g_real_CoCreateInstance == nullptr) {
        HMODULE cb = ::GetModuleHandleW(L"combase.dll");
        if (!cb) cb = ::LoadLibraryW(L"combase.dll");
        if (cb) {
            g_real_CoCreateInstance =
                reinterpret_cast<PfnCoCreateInstance>(::GetProcAddress(cb, "CoCreateInstance"));
        }
        if (HMODULE ole = ::GetModuleHandleW(L"ole32.dll")) {
            g_ole32_CoCreateInstance =
                reinterpret_cast<PfnCoCreateInstance>(::GetProcAddress(ole, "CoCreateInstance"));
            if (g_real_CoCreateInstance == nullptr) {
                g_real_CoCreateInstance = g_ole32_CoCreateInstance;
            }
        }
    }
}

HRESULT STDAPICALLTYPE Hook_CoCreateInstance(
    REFCLSID rclsid, LPVOID pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    EnsureComProcsResolved();
    if (!g_real_CoCreateInstance) {
        if (ppv) *ppv = nullptr;
        return E_FAIL;
    }
    const HRESULT hr = g_real_CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv != nullptr && *ppv != nullptr) {
        openscarlett::loader::WrapMmDeviceEnumeratorIfNeeded(rclsid, riid, ppv);
    }
    return hr;
}

DWORD ReadNormalizedConsoleModeEnv(char (&out_buf)[64]) noexcept {
    out_buf[0] = '\0';
    DWORD len = ::GetEnvironmentVariableA(
        "OPENSCARLETT_CONSOLE_TYPE", out_buf, static_cast<DWORD>(sizeof(out_buf)));
    if (len == 0u || len >= sizeof(out_buf)) {
        len = ::GetEnvironmentVariableA(
            "OPENSCARLETT_CONSOLE_PROFILE", out_buf, static_cast<DWORD>(sizeof(out_buf)));
    }
    if (len == 0u || len >= sizeof(out_buf)) {
        out_buf[0] = '\0';
        return 0u;
    }
    DWORD start = 0u;
    while (start < len && (out_buf[start] == ' ' || out_buf[start] == '\t')) {
        ++start;
    }
    while (len > start && (out_buf[len - 1u] == ' ' || out_buf[len - 1u] == '\t')) {
        --len;
    }
    const DWORD trimmed = len - start;
    for (DWORD i = 0u; i < trimmed; ++i) {
        char c = out_buf[start + i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out_buf[i] = c;
    }
    out_buf[trimmed] = '\0';
    return trimmed;
}

bool IsLockhartProfileToken(std::string_view mode) noexcept {
    return mode == "lockhart" || mode == "seriess" || mode == "series_s" ||
           mode == "series-s" || mode == "xss" || mode == "3" || mode == "6" ||
           mode == "s" || mode == "8gb";
}

bool IsAnacondaProfileToken(std::string_view mode) noexcept {
    return mode == "anaconda" || mode == "seriesx" || mode == "series_x" ||
           mode == "series-x" || mode == "xsx" || mode == "4" || mode == "7" ||
           mode == "x" || mode == "16gb";
}

bool IsDevkitProfileToken(std::string_view mode) noexcept {
    return mode == "devkit" || mode == "scarlett_devkit" ||
           mode == "series_x_devkit" || mode == "8";
}

uint64_t QueryRawHostTotalPhysBytes() noexcept {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    PfnGlobalMemoryStatusEx real_fn = g_real_GlobalMemoryStatusEx
                                          ? g_real_GlobalMemoryStatusEx
                                          : g_kernelbase_GlobalMemoryStatusEx;
    if (real_fn && real_fn(&ms)) {
        return ms.ullTotalPhys;
    }
    if (::GlobalMemoryStatusEx(&ms)) {
        return ms.ullTotalPhys;
    }
    return 0ull;
}

uint64_t ResolveGuestConsoleMemoryCapBytes() noexcept {
    char cap_mb_buf[32]{};
    const DWORD cap_mb_len = ::GetEnvironmentVariableA(
        "OPENSCARLETT_MEMORY_CAP_MB", cap_mb_buf, static_cast<DWORD>(sizeof(cap_mb_buf)));
    if (cap_mb_len > 0u && cap_mb_len < sizeof(cap_mb_buf)) {
        uint64_t parsed_mb = 0ull;
        bool any_digit = false;
        for (DWORD i = 0u; i < cap_mb_len; ++i) {
            const char c = cap_mb_buf[i];
            if (c >= '0' && c <= '9') {
                parsed_mb = parsed_mb * 10ull + static_cast<uint64_t>(c - '0');
                any_digit = true;
            } else {
                break;
            }
        }
        if (any_digit && parsed_mb >= 512ull) {
            return parsed_mb * 1024ull * 1024ull;
        }
    }

    char console_buf[64]{};
    const DWORD console_len = ReadNormalizedConsoleModeEnv(console_buf);
    if (console_len == 0u) {
        return 0ull;
    }
    const std::string_view mode(console_buf, console_len);
    if (IsLockhartProfileToken(mode)) {
        return 8ull * 1024ull * 1024ull * 1024ull;  // 8 GiB Lockhart title budget
    }
    if (mode == "auto") {
        const uint64_t host_phys = QueryRawHostTotalPhysBytes();
        if (host_phys > 0ull && host_phys <= 18ull * 1024ull * 1024ull * 1024ull) {
            return 8ull * 1024ull * 1024ull * 1024ull;
        }
        return 13824ull * 1024ull * 1024ull;
    }
    if (IsAnacondaProfileToken(mode)) {
        return 13824ull * 1024ull * 1024ull;  // 13.5 GiB Anaconda title budget
    }
    if (IsDevkitProfileToken(mode)) {
        return 32ull * 1024ull * 1024ull * 1024ull;
    }
    return 0ull;
}

// ---------------------------------------------------------------------------
// Virtualized HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM\ConsoleMode
// Used by XGamingRuntimeThunks.lib!XSystemGetDeviceType():
//   - 0x60 -> XSystemDeviceType::XboxScarlettLockhart (6)
//   - 0x64 -> XSystemDeviceType::XboxScarlettAnaconda (7)
//   - 0x68 -> XSystemDeviceType::XboxScarlettDevkit   (8)
// Never touches or mutates the real host Windows registry.
// ---------------------------------------------------------------------------
constexpr uintptr_t kSyntheticOemConsoleKeyVal = 0x05CA0EE0u;

inline HKEY GetSyntheticOemConsoleKey() noexcept {
    return reinterpret_cast<HKEY>(kSyntheticOemConsoleKeyVal);
}

DWORD ResolveConsoleModeRegistryDword() noexcept {
    char console_buf[64]{};
    const DWORD console_len = ReadNormalizedConsoleModeEnv(console_buf);
    if (console_len > 0u) {
        const std::string_view mode(console_buf, console_len);
        if (IsLockhartProfileToken(mode)) {
            return 0x60u;  // XboxScarlettLockhart (6)
        }
        if (IsDevkitProfileToken(mode)) {
            return 0x68u;  // XboxScarlettDevkit (8)
        }
        if (IsAnacondaProfileToken(mode)) {
            return 0x64u;  // XboxScarlettAnaconda (7)
        }
        if (mode == "auto") {
            const uint64_t host_phys = QueryRawHostTotalPhysBytes();
            if (host_phys > 0ull && host_phys <= 18ull * 1024ull * 1024ull * 1024ull) {
                return 0x60u;  // Lockhart on <= 18 GiB host RAM
            }
            return 0x64u;  // Anaconda on > 18 GiB host RAM
        }
    }
    return 0x64u;  // Default: XboxScarlettAnaconda (7)
}

bool IsOemConsoleSubKeyW(LPCWSTR sub_key) noexcept {
    if (!sub_key) {
        return false;
    }
    while (*sub_key == L'\\' || *sub_key == L'/') {
        ++sub_key;
    }
    static constexpr wchar_t kTarget[] = L"software\\microsoft\\windows nt\\currentversion\\oem";
    constexpr size_t kTargetLen = (sizeof(kTarget) / sizeof(wchar_t)) - 1u;
    size_t i = 0u;
    for (; i < kTargetLen; ++i) {
        wchar_t c = sub_key[i];
        if (c == L'\0') {
            return false;
        }
        if (c == L'/') {
            c = L'\\';
        } else if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (c != kTarget[i]) {
            return false;
        }
    }
    while (sub_key[i] == L'\\' || sub_key[i] == L'/') {
        ++i;
    }
    return sub_key[i] == L'\0';
}

bool IsOemConsoleSubKeyA(LPCSTR sub_key) noexcept {
    if (!sub_key) {
        return false;
    }
    while (*sub_key == '\\' || *sub_key == '/') {
        ++sub_key;
    }
    static constexpr char kTarget[] = "software\\microsoft\\windows nt\\currentversion\\oem";
    constexpr size_t kTargetLen = sizeof(kTarget) - 1u;
    size_t i = 0u;
    for (; i < kTargetLen; ++i) {
        char c = sub_key[i];
        if (c == '\0') {
            return false;
        }
        if (c == '/') {
            c = '\\';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kTarget[i]) {
            return false;
        }
    }
    while (sub_key[i] == '\\' || sub_key[i] == '/') {
        ++i;
    }
    return sub_key[i] == '\0';
}

bool IsConsoleModeValueNameW(LPCWSTR name) noexcept {
    if (!name) {
        return false;
    }
    static constexpr wchar_t kTarget[] = L"consolemode";
    constexpr size_t kTargetLen = (sizeof(kTarget) / sizeof(wchar_t)) - 1u;
    for (size_t i = 0u; i < kTargetLen; ++i) {
        wchar_t c = name[i];
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (c != kTarget[i]) {
            return false;
        }
    }
    return name[kTargetLen] == L'\0';
}

bool IsConsoleModeValueNameA(LPCSTR name) noexcept {
    if (!name) {
        return false;
    }
    static constexpr char kTarget[] = "consolemode";
    constexpr size_t kTargetLen = sizeof(kTarget) - 1u;
    for (size_t i = 0u; i < kTargetLen; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kTarget[i]) {
            return false;
        }
    }
    return name[kTargetLen] == '\0';
}

void EnsureAdvapi32ProcsResolved() noexcept {
    if (g_advapi32_mod != nullptr) {
        return;
    }
    HMODULE adv = g_real_GetModuleHandleW
                      ? g_real_GetModuleHandleW(L"advapi32.dll")
                      : ::GetModuleHandleW(L"advapi32.dll");
    if (!adv) {
        return;
    }
    PfnGetProcAddress gpa = g_real_GetProcAddress ? g_real_GetProcAddress : &::GetProcAddress;
    if (!g_real_RegOpenKeyW) {
        g_real_RegOpenKeyW = reinterpret_cast<PfnRegOpenKeyW>(gpa(adv, "RegOpenKeyW"));
    }
    if (!g_real_RegOpenKeyA) {
        g_real_RegOpenKeyA = reinterpret_cast<PfnRegOpenKeyA>(gpa(adv, "RegOpenKeyA"));
    }
    g_advapi32_RegOpenKeyExW = reinterpret_cast<PfnRegOpenKeyExW>(gpa(adv, "RegOpenKeyExW"));
    g_advapi32_RegOpenKeyExA = reinterpret_cast<PfnRegOpenKeyExA>(gpa(adv, "RegOpenKeyExA"));
    g_advapi32_RegQueryValueExW = reinterpret_cast<PfnRegQueryValueExW>(gpa(adv, "RegQueryValueExW"));
    g_advapi32_RegQueryValueExA = reinterpret_cast<PfnRegQueryValueExA>(gpa(adv, "RegQueryValueExA"));
    g_advapi32_RegGetValueW = reinterpret_cast<PfnRegGetValueW>(gpa(adv, "RegGetValueW"));
    g_advapi32_RegGetValueA = reinterpret_cast<PfnRegGetValueA>(gpa(adv, "RegGetValueA"));
    g_advapi32_RegCloseKey = reinterpret_cast<PfnRegCloseKey>(gpa(adv, "RegCloseKey"));
    g_advapi32_mod = adv;
}

LSTATUS WriteConsoleModeRegDword(LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData, const char* api_tag) {
    const DWORD mode = ResolveConsoleModeRegistryDword();
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        openscarlett::CallTelemetry::Global().Record(
            "advapi32.dll",
            api_tag,
            openscarlett::CallOutcome::Ok,
            static_cast<int32_t>(mode),
            -1);
    }
    if (lpType) {
        *lpType = REG_DWORD;
    }
    if (!lpData) {
        if (lpcbData) {
            *lpcbData = static_cast<DWORD>(sizeof(DWORD));
        }
        return ERROR_SUCCESS;
    }
    if (!lpcbData) {
        return ERROR_INVALID_PARAMETER;
    }
    if (*lpcbData < sizeof(DWORD)) {
        *lpcbData = static_cast<DWORD>(sizeof(DWORD));
        return ERROR_MORE_DATA;
    }
    std::memcpy(lpData, &mode, sizeof(DWORD));
    *lpcbData = static_cast<DWORD>(sizeof(DWORD));
    return ERROR_SUCCESS;
}

LSTATUS APIENTRY Hook_RegOpenKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
    if (hKey == HKEY_LOCAL_MACHINE && IsOemConsoleSubKeyW(lpSubKey)) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    if (hKey == GetSyntheticOemConsoleKey() && (!lpSubKey || lpSubKey[0] == L'\0')) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    EnsureAdvapi32ProcsResolved();
    EnsureComProcsResolved();
    if (g_real_RegOpenKeyW) {
        return g_real_RegOpenKeyW(hKey, lpSubKey, phkResult);
    }
    if (!phkResult) {
        return ERROR_INVALID_PARAMETER;
    }
    if (!lpSubKey || lpSubKey[0] == L'\0') {
        *phkResult = hKey;
        return ERROR_SUCCESS;
    }
    return g_real_RegOpenKeyExW
               ? g_real_RegOpenKeyExW(hKey, lpSubKey, 0u, MAXIMUM_ALLOWED, phkResult)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult) {
    if (hKey == HKEY_LOCAL_MACHINE && IsOemConsoleSubKeyA(lpSubKey)) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    if (hKey == GetSyntheticOemConsoleKey() && (!lpSubKey || lpSubKey[0] == '\0')) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    EnsureAdvapi32ProcsResolved();
    EnsureComProcsResolved();
    if (g_real_RegOpenKeyA) {
        return g_real_RegOpenKeyA(hKey, lpSubKey, phkResult);
    }
    if (!phkResult) {
        return ERROR_INVALID_PARAMETER;
    }
    if (!lpSubKey || lpSubKey[0] == '\0') {
        *phkResult = hKey;
        return ERROR_SUCCESS;
    }
    return g_real_RegOpenKeyExA
               ? g_real_RegOpenKeyExA(hKey, lpSubKey, 0u, MAXIMUM_ALLOWED, phkResult)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegOpenKeyExW(
    HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
    if (hKey == HKEY_LOCAL_MACHINE && IsOemConsoleSubKeyW(lpSubKey)) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    if (hKey == GetSyntheticOemConsoleKey() && (!lpSubKey || lpSubKey[0] == L'\0')) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    return g_real_RegOpenKeyExW
               ? g_real_RegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegOpenKeyExA(
    HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
    if (hKey == HKEY_LOCAL_MACHINE && IsOemConsoleSubKeyA(lpSubKey)) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    if (hKey == GetSyntheticOemConsoleKey() && (!lpSubKey || lpSubKey[0] == '\0')) {
        if (!phkResult) {
            return ERROR_INVALID_PARAMETER;
        }
        *phkResult = GetSyntheticOemConsoleKey();
        return ERROR_SUCCESS;
    }
    return g_real_RegOpenKeyExA
               ? g_real_RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegQueryValueExW(
    HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) {
    if (hKey == GetSyntheticOemConsoleKey()) {
        if (IsConsoleModeValueNameW(lpValueName)) {
            return WriteConsoleModeRegDword(lpType, lpData, lpcbData, "RegQueryValueExW:OEM.ConsoleMode");
        }
        return ERROR_FILE_NOT_FOUND;
    }
    return g_real_RegQueryValueExW
               ? g_real_RegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegQueryValueExA(
    HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) {
    if (hKey == GetSyntheticOemConsoleKey()) {
        if (IsConsoleModeValueNameA(lpValueName)) {
            return WriteConsoleModeRegDword(lpType, lpData, lpcbData, "RegQueryValueExA:OEM.ConsoleMode");
        }
        return ERROR_FILE_NOT_FOUND;
    }
    return g_real_RegQueryValueExA
               ? g_real_RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegGetValueW(
    HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData) {
    const bool match_hklm_oem = (hkey == HKEY_LOCAL_MACHINE && IsOemConsoleSubKeyW(lpSubKey));
    const bool match_syn_oem = (hkey == GetSyntheticOemConsoleKey() && (!lpSubKey || lpSubKey[0] == L'\0'));
    if (match_hklm_oem || match_syn_oem) {
        if (!IsConsoleModeValueNameW(lpValue)) {
            if ((dwFlags & RRF_ZEROONFAILURE) != 0u && pvData && pcbData && *pcbData > 0u) {
                std::memset(pvData, 0, *pcbData);
            }
            return ERROR_FILE_NOT_FOUND;
        }
        const DWORD type_mask = dwFlags & 0x0000FFFFu;
        if (type_mask != 0u && (type_mask & RRF_RT_REG_DWORD) == 0u) {
            if ((dwFlags & RRF_ZEROONFAILURE) != 0u && pvData && pcbData && *pcbData > 0u) {
                std::memset(pvData, 0, *pcbData);
            }
            return ERROR_UNSUPPORTED_TYPE;
        }
        const LSTATUS st = WriteConsoleModeRegDword(
            pdwType, static_cast<LPBYTE>(pvData), pcbData, "RegGetValueW:OEM.ConsoleMode");
        if (st != ERROR_SUCCESS && (dwFlags & RRF_ZEROONFAILURE) != 0u && pvData && pcbData && *pcbData > 0u) {
            std::memset(pvData, 0, *pcbData);
        }
        return st;
    }
    if (hkey == GetSyntheticOemConsoleKey()) {
        return ERROR_FILE_NOT_FOUND;
    }
    return g_real_RegGetValueW
               ? g_real_RegGetValueW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegGetValueA(
    HKEY hkey, LPCSTR lpSubKey, LPCSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData) {
    const bool match_hklm_oem = (hkey == HKEY_LOCAL_MACHINE && IsOemConsoleSubKeyA(lpSubKey));
    const bool match_syn_oem = (hkey == GetSyntheticOemConsoleKey() && (!lpSubKey || lpSubKey[0] == '\0'));
    if (match_hklm_oem || match_syn_oem) {
        if (!IsConsoleModeValueNameA(lpValue)) {
            if ((dwFlags & RRF_ZEROONFAILURE) != 0u && pvData && pcbData && *pcbData > 0u) {
                std::memset(pvData, 0, *pcbData);
            }
            return ERROR_FILE_NOT_FOUND;
        }
        const DWORD type_mask = dwFlags & 0x0000FFFFu;
        if (type_mask != 0u && (type_mask & RRF_RT_REG_DWORD) == 0u) {
            if ((dwFlags & RRF_ZEROONFAILURE) != 0u && pvData && pcbData && *pcbData > 0u) {
                std::memset(pvData, 0, *pcbData);
            }
            return ERROR_UNSUPPORTED_TYPE;
        }
        const LSTATUS st = WriteConsoleModeRegDword(
            pdwType, static_cast<LPBYTE>(pvData), pcbData, "RegGetValueA:OEM.ConsoleMode");
        if (st != ERROR_SUCCESS && (dwFlags & RRF_ZEROONFAILURE) != 0u && pvData && pcbData && *pcbData > 0u) {
            std::memset(pvData, 0, *pcbData);
        }
        return st;
    }
    if (hkey == GetSyntheticOemConsoleKey()) {
        return ERROR_FILE_NOT_FOUND;
    }
    return g_real_RegGetValueA
               ? g_real_RegGetValueA(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData)
               : ERROR_INVALID_FUNCTION;
}

LSTATUS APIENTRY Hook_RegCloseKey(HKEY hKey) {
    if (hKey == GetSyntheticOemConsoleKey()) {
        return ERROR_SUCCESS;
    }
    return g_real_RegCloseKey ? g_real_RegCloseKey(hKey) : ERROR_INVALID_FUNCTION;
}

BOOL WINAPI Hook_GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer) {
    PfnGlobalMemoryStatusEx real_fn = g_real_GlobalMemoryStatusEx
                                          ? g_real_GlobalMemoryStatusEx
                                          : g_kernelbase_GlobalMemoryStatusEx;
    const BOOL ok = real_fn ? real_fn(lpBuffer) : ::GlobalMemoryStatusEx(lpBuffer);
    if (!ok || !lpBuffer || g_shutting_down.load(std::memory_order_acquire)) {
        return ok;
    }

    const uint64_t cap_bytes = ResolveGuestConsoleMemoryCapBytes();
    if (cap_bytes == 0ull) {
        return ok;
    }

    // Never cap queries made by OpenScarlett shims (kernelx.dll / xmem.dll sparse-file commit
    // checks) or Windows system/driver DLLs — only cap guest/title executable & DLL queries.
    HMODULE caller_mod = nullptr;
    const PVOID ret_addr = _ReturnAddress();
    const BOOL got_mod = g_real_GetModuleHandleExW
                             ? g_real_GetModuleHandleExW(
                                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(ret_addr),
                                   &caller_mod)
                             : ::GetModuleHandleExW(
                                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(ret_addr),
                                   &caller_mod);
    if (got_mod && caller_mod != nullptr) {
        if (caller_mod == g_self_module ||
            IsModuleInShimDir(caller_mod) ||
            IsWindowsSystemModule(caller_mod)) {
            return ok;
        }
    }

    if (lpBuffer->ullTotalPhys > cap_bytes) {
        lpBuffer->ullTotalPhys = cap_bytes;
    }
    if (lpBuffer->ullAvailPhys > cap_bytes) {
        lpBuffer->ullAvailPhys = cap_bytes;
    }
    // GameOS titles do not have a multi-dozen-GiB desktop swapfile; cap commit limits so
    // engines like RAGE9 do not size streaming/virtual pools off host ullAvailPageFile.
    if (lpBuffer->ullTotalPageFile > cap_bytes) {
        lpBuffer->ullTotalPageFile = cap_bytes;
    }
    if (lpBuffer->ullAvailPageFile > lpBuffer->ullAvailPhys) {
        lpBuffer->ullAvailPageFile = lpBuffer->ullAvailPhys;
    }
    if (lpBuffer->ullTotalPhys > 0ull) {
        const uint64_t used = (lpBuffer->ullTotalPhys >= lpBuffer->ullAvailPhys)
                                  ? (lpBuffer->ullTotalPhys - lpBuffer->ullAvailPhys)
                                  : 0ull;
        lpBuffer->dwMemoryLoad = static_cast<DWORD>(
            std::min<uint64_t>(100ull, (used * 100ull) / lpBuffer->ullTotalPhys));
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Exact x86-64 Zen 2 Instruction Length Decoder & Thread-Safe Hot-Patcher
// ---------------------------------------------------------------------------
constexpr size_t kMaxX64InsnBytes = 15u;
SRWLOCK g_veh_hotpatch_lock = SRWLOCK_INIT;

struct HotpatchedSiteEntry {
    uint8_t length = 0u;
};

std::unordered_map<uintptr_t, HotpatchedSiteEntry>* GetHotpatchedSitesLocked() {
    // Intentionally heap-leaked singleton so worker threads hitting VEH during
    // process teardown never race static destructor order.
    static auto* s_sites = new std::unordered_map<uintptr_t, HotpatchedSiteEntry>();
    return s_sites;
}

bool IsReadableCodePageProtect(DWORD prot) noexcept {
    if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD base = prot & 0xFFu;
    return base == PAGE_EXECUTE ||
           base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY ||
           base == PAGE_READONLY ||
           base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY;
}

bool IsAddressInMainExecutableImage(uintptr_t addr, size_t len = 1u) noexcept {
    struct ExeBounds {
        uintptr_t start = 0u;
        uintptr_t end = 0u;
    };
    static const ExeBounds s_bounds = []() noexcept -> ExeBounds {
        ExeBounds b{};
        HMODULE exe = g_real_GetModuleHandleW ? g_real_GetModuleHandleW(nullptr) : ::GetModuleHandleW(nullptr);
        if (!exe) return b;
        const auto* base = reinterpret_cast<const uint8_t*>(exe);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return b;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0u) return b;
        b.start = reinterpret_cast<uintptr_t>(base);
        b.end = b.start + static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
        return b;
    }();
    return s_bounds.start != 0u && addr >= s_bounds.start &&
           len <= (s_bounds.end - s_bounds.start) && (addr + len) <= s_bounds.end;
}

size_t ReadInsnBytesSafe64(const uint8_t* rip_ptr, uint8_t (&out)[kMaxX64InsnBytes]) noexcept {
    std::memset(out, 0, sizeof(out));
    if (!rip_ptr) {
        return 0u;
    }
    uintptr_t cursor = reinterpret_cast<uintptr_t>(rip_ptr);
    if (IsAddressInMainExecutableImage(cursor, kMaxX64InsnBytes)) {
        std::memcpy(out, rip_ptr, kMaxX64InsnBytes);
        return kMaxX64InsnBytes;
    }
    size_t copied = 0u;
    while (copied < kMaxX64InsnBytes) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u) {
            break;
        }
        if (mbi.State != MEM_COMMIT || !IsReadableCodePageProtect(mbi.Protect)) {
            break;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor) {
            break;
        }
        const size_t step = std::min<size_t>(
            kMaxX64InsnBytes - copied, static_cast<size_t>(region_end - cursor));
        std::memcpy(out + copied, reinterpret_cast<const void*>(cursor), step);
        copied += step;
        cursor += step;
    }
    return copied;
}

struct DecodedPrefixes64 {
    size_t opcode_offset = 0u;
    uint8_t rex = 0u;
    uint8_t last_f2_f3 = 0u;  // 0, 0xF2, or 0xF3 (group-1 mandatory prefix priority)
    bool has_66 = false;
    bool has_67 = false;
    bool has_lock = false;
};

bool ScanPrefixes64(const uint8_t* b, size_t avail, DecodedPrefixes64& out) noexcept {
    out = DecodedPrefixes64{};
    size_t i = 0u;
    while (i < avail && i + 1u < kMaxX64InsnBytes) {
        const uint8_t c = b[i];
        if (c == 0x66u) {
            out.has_66 = true;
            out.rex = 0u;  // AMD64 rule: REX before a legacy prefix is ignored/superseded
            ++i;
        } else if (c == 0x67u) {
            out.has_67 = true;
            out.rex = 0u;
            ++i;
        } else if (c == 0xF2u || c == 0xF3u) {
            out.last_f2_f3 = c;
            out.rex = 0u;
            ++i;
        } else if (c == 0xF0u) {
            out.has_lock = true;
            out.rex = 0u;
            ++i;
        } else if (c == 0x26u || c == 0x2Eu || c == 0x36u ||
                   c == 0x3Eu || c == 0x64u || c == 0x65u) {
            out.rex = 0u;
            ++i;
        } else if ((c & 0xF0u) == 0x40u) {
            out.rex = c;
            ++i;
        } else {
            // Includes 2/3-byte VEX (0xC5/0xC4), EVEX (0x62), XOP (0x8F), and 0x0F escape.
            break;
        }
    }
    out.opcode_offset = i;
    return i < avail;
}

size_t DecodeModRmInsnLength64(
    const uint8_t* b,
    size_t avail,
    size_t modrm_offset,
    size_t imm_bytes = 0u) noexcept {
    if (modrm_offset >= avail) {
        return 0u;
    }
    const uint8_t modrm = b[modrm_offset];
    const uint8_t mod = static_cast<uint8_t>((modrm >> 6u) & 3u);
    const uint8_t rm = static_cast<uint8_t>(modrm & 7u);
    size_t len = modrm_offset + 1u;
    if (mod != 3u) {
        if (rm == 4u) {
            // SIB byte follows ModRM (applies to both rsp [REX.B=0] and r12 [REX.B=1]).
            if (len >= avail) {
                return 0u;
            }
            const uint8_t sib = b[len++];
            if (mod == 0u && (sib & 7u) == 5u) {
                // base == 5 (rbp / r13) with mod == 00 encodes disp32 with no base register.
                len += 4u;
            }
        } else if (mod == 0u && rm == 5u) {
            // RIP-relative [rip + disp32] (or [eip + disp32] with 0x67) for both REX.B=0 and REX.B=1.
            len += 4u;
        }
        if (mod == 1u) {
            len += 1u;  // disp8
        } else if (mod == 2u) {
            len += 4u;  // disp32
        }
    }
    len += imm_bytes;
    if (len > kMaxX64InsnBytes || len > avail) {
        return 0u;
    }
    return len;
}

uint64_t ReadContextGpr64(const CONTEXT* ctx, uint32_t reg_idx) noexcept {
    switch (reg_idx & 15u) {
        case 0u: return ctx->Rax;
        case 1u: return ctx->Rcx;
        case 2u: return ctx->Rdx;
        case 3u: return ctx->Rbx;
        case 4u: return ctx->Rsp;
        case 5u: return ctx->Rbp;
        case 6u: return ctx->Rsi;
        case 7u: return ctx->Rdi;
        case 8u: return ctx->R8;
        case 9u: return ctx->R9;
        case 10u: return ctx->R10;
        case 11u: return ctx->R11;
        case 12u: return ctx->R12;
        case 13u: return ctx->R13;
        case 14u: return ctx->R14;
        default: return ctx->R15;
    }
}

void WriteContextGpr64(CONTEXT* ctx, uint32_t reg_idx, uint64_t val) noexcept {
    switch (reg_idx & 15u) {
        case 0u: ctx->Rax = val; break;
        case 1u: ctx->Rcx = val; break;
        case 2u: ctx->Rdx = val; break;
        case 3u: ctx->Rbx = val; break;
        case 4u: ctx->Rsp = val; break;
        case 5u: ctx->Rbp = val; break;
        case 6u: ctx->Rsi = val; break;
        case 7u: ctx->Rdi = val; break;
        case 8u: ctx->R8 = val; break;
        case 9u: ctx->R9 = val; break;
        case 10u: ctx->R10 = val; break;
        case 11u: ctx->R11 = val; break;
        case 12u: ctx->R12 = val; break;
        case 13u: ctx->R13 = val; break;
        case 14u: ctx->R14 = val; break;
        default: ctx->R15 = val; break;
    }
}

bool ComputeEffectiveAddress64(
    const CONTEXT* ctx,
    const DecodedPrefixes64& pfx,
    const uint8_t* b,
    size_t modrm_offset,
    size_t insn_len,
    uint64_t& out_ea) noexcept {
    out_ea = 0ull;
    const uint8_t modrm = b[modrm_offset];
    const uint8_t mod = static_cast<uint8_t>((modrm >> 6u) & 3u);
    const uint8_t rm_raw = static_cast<uint8_t>(modrm & 7u);
    if (mod == 3u) {
        return false;
    }
    const uint32_t rex_x = (pfx.rex & 0x02u) ? 8u : 0u;
    const uint32_t rex_b = (pfx.rex & 0x01u) ? 8u : 0u;
    size_t pos = modrm_offset + 1u;
    uint64_t ea = 0ull;

    if (rm_raw == 4u) {
        const uint8_t sib = b[pos++];
        const uint32_t scale_shift = (sib >> 6u) & 3u;
        const uint32_t index_reg = ((sib >> 3u) & 7u) | rex_x;
        const uint32_t base_raw = sib & 7u;
        if (mod == 0u && base_raw == 5u) {
            int32_t disp32 = 0;
            std::memcpy(&disp32, b + pos, 4u);
            pos += 4u;
            ea += static_cast<uint64_t>(static_cast<int64_t>(disp32));
        } else {
            ea += ReadContextGpr64(ctx, base_raw | rex_b);
        }
        // index_reg == 4 (RSP with REX.X=0) encodes "no index"; R12 (index_reg == 12) is scaled.
        if (index_reg != 4u) {
            ea += ReadContextGpr64(ctx, index_reg) << scale_shift;
        }
    } else if (mod == 0u && rm_raw == 5u) {
        int32_t disp32 = 0;
        std::memcpy(&disp32, b + pos, 4u);
        pos += 4u;
        const uint64_t next_rip = pfx.has_67
                                      ? ((ctx->Rip + insn_len) & 0xFFFFFFFFull)
                                      : (ctx->Rip + insn_len);
        ea = next_rip + static_cast<uint64_t>(static_cast<int64_t>(disp32));
    } else {
        ea = ReadContextGpr64(ctx, rm_raw | rex_b);
    }

    if (mod == 1u) {
        const int8_t disp8 = static_cast<int8_t>(b[pos++]);
        ea += static_cast<uint64_t>(static_cast<int64_t>(disp8));
    } else if (mod == 2u) {
        int32_t disp32 = 0;
        std::memcpy(&disp32, b + pos, 4u);
        pos += 4u;
        ea += static_cast<uint64_t>(static_cast<int64_t>(disp32));
    }

    if (pos != insn_len) {
        return false;
    }
    if (pfx.has_67) {
        ea &= 0xFFFFFFFFull;
    }
    out_ea = ea;
    return true;
}

void HotPatchToNopsThreadSafe(uint8_t* p, size_t insn_len) noexcept {
    if (!p || insn_len == 0u || insn_len > kMaxX64InsnBytes) {
        return;
    }
    const uintptr_t site_rip = reinterpret_cast<uintptr_t>(p);
    ::AcquireSRWLockExclusive(&g_veh_hotpatch_lock);
    auto* sites = GetHotpatchedSitesLocked();
    if (sites->find(site_rip) == sites->end()) {
        DWORD old_prot = 0u;
        if (::VirtualProtect(p, insn_len, PAGE_EXECUTE_READWRITE, &old_prot)) {
            if (insn_len > 1u) {
                // Stage 0xCC (INT3) at byte 0 while filling tail NOPs so concurrent CPU cores
                // never fetch a torn instruction boundary mid-patch.
                _InterlockedExchange8(reinterpret_cast<volatile char*>(p), static_cast<char>(0xCCu));
                std::atomic_thread_fence(std::memory_order_seq_cst);
                std::memset(p + 1u, 0x90, insn_len - 1u);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                _InterlockedExchange8(reinterpret_cast<volatile char*>(p), static_cast<char>(0x90u));
            } else {
                _InterlockedExchange8(reinterpret_cast<volatile char*>(p), static_cast<char>(0x90u));
            }
            (*sites)[site_rip] = HotpatchedSiteEntry{static_cast<uint8_t>(insn_len)};
            DWORD dummy = 0u;
            ::VirtualProtect(p, insn_len, old_prot, &dummy);
            ::FlushInstructionCache(::GetCurrentProcess(), p, insn_len);
        }
    }
    ::ReleaseSRWLockExclusive(&g_veh_hotpatch_lock);
}

LONG CALLBACK Zen2IllegalInstructionVeh(PEXCEPTION_POINTERS ep) {
    static_assert(offsetof(CONTEXT, Xmm15) - offsetof(CONTEXT, Xmm0) == 15u * sizeof(M128A),
                  "CONTEXT XMM0..XMM15 register array must be contiguous");

    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD exc_code = ep->ExceptionRecord->ExceptionCode;
    auto* ctx = ep->ContextRecord;
    auto* p = reinterpret_cast<uint8_t*>(ctx->Rip);
    if (!p) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (exc_code == EXCEPTION_BREAKPOINT) {
        // Check if another worker thread hit our transient 0xCC barrier during HotPatchToNopsThreadSafe.
        {
            const uintptr_t rip_val = static_cast<uintptr_t>(ctx->Rip);
            uintptr_t resume_rip = 0u;
            ::AcquireSRWLockShared(&g_veh_hotpatch_lock);
            auto* sites = GetHotpatchedSitesLocked();
            auto it = sites->find(rip_val);
            if (it == sites->end() && rip_val > 0u) {
                it = sites->find(rip_val - 1u);
            }
            if (it != sites->end()) {
                resume_rip = it->first + it->second.length;
            }
            ::ReleaseSRWLockShared(&g_veh_hotpatch_lock);
            if (resume_rip != 0u) {
                ctx->Rip = resume_rip;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        static const int continue_int3_mode = []() {
            char buf[16]{};
            if (::GetEnvironmentVariableA("OPENSCARLETT_CONTINUE_INT3", buf, sizeof(buf)) > 0) {
                return (buf[0] == '0') ? 0 : 1;
            }
            return -1;  // Auto: continue single-byte inline __debugbreak() inside guest title EXE
        }();
        static const std::string s_veh_log_path = []() {
            char buf[MAX_PATH]{};
            const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_VEH_LOG", buf, sizeof(buf));
            return (n > 0u && n < sizeof(buf)) ? std::string(buf, n) : std::string();
        }();
        {
            static std::atomic<uint32_t> s_int3_diag_cnt{0u};
            const uint32_t diag_n = s_int3_diag_cnt.fetch_add(1u, std::memory_order_relaxed);
            if (diag_n < 16u) {
                auto resolve_addr = [](uintptr_t addr, char* out_buf, size_t out_cap) {
                    HMODULE m = nullptr;
                    char mpath[MAX_PATH]{};
                    if (addr >= 0x10000u &&
                        ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                             reinterpret_cast<LPCSTR>(addr), &m) &&
                        m) {
                        ::GetModuleFileNameA(m, mpath, static_cast<DWORD>(sizeof(mpath)));
                        const char* base = std::strrchr(mpath, '\\');
                        base = base ? (base + 1) : mpath;
                        const uintptr_t off = addr - reinterpret_cast<uintptr_t>(m);
                        std::snprintf(out_buf, out_cap, "%s+0x%llX", base, static_cast<unsigned long long>(off));
                    } else {
                        std::snprintf(out_buf, out_cap, "0x%llX", static_cast<unsigned long long>(addr));
                    }
                };
                auto peek_ascii = [](uintptr_t cand, char* out_s, size_t cap) -> bool {
                    out_s[0] = '\0';
                    if (cand < 0x10000u || cap < 8u) return false;
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (::VirtualQuery(reinterpret_cast<const void*>(cand), &mbi, sizeof(mbi)) == 0u ||
                        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0u) {
                        return false;
                    }
                    const size_t avail = std::min<size_t>(
                        cap - 1u,
                        static_cast<size_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - cand));
                    if (avail < 4u) return false;
                    const auto* s = reinterpret_cast<const unsigned char*>(cand);
                    size_t k = 0u;
                    for (; k < std::min<size_t>(avail, 120u); ++k) {
                        if (s[k] == 0u) break;
                        if (s[k] < 0x20u && s[k] != '\t' && s[k] != '\n' && s[k] != '\r') return false;
                        if (s[k] > 0x7Eu) return false;
                        out_s[k] = (s[k] == '\n' || s[k] == '\r') ? ' ' : static_cast<char>(s[k]);
                    }
                    out_s[k] = '\0';
                    return k >= 4u;
                };

                char rip_sym[160]{};
                resolve_addr(static_cast<uintptr_t>(ctx->Rip), rip_sym, sizeof(rip_sym));

                std::string stack_syms;
                MEMORY_BASIC_INFORMATION sp_mbi{};
                if (ctx->Rsp >= 0x10000u &&
                    ::VirtualQuery(reinterpret_cast<const void*>(ctx->Rsp), &sp_mbi, sizeof(sp_mbi)) != 0u &&
                    sp_mbi.State == MEM_COMMIT && (sp_mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0u) {
                    const uintptr_t sp_end =
                        std::min<uintptr_t>(ctx->Rsp + 0x120u,
                                            reinterpret_cast<uintptr_t>(sp_mbi.BaseAddress) + sp_mbi.RegionSize);
                    int found_ret = 0;
                    for (uintptr_t sp = (ctx->Rsp & ~uintptr_t(7u)); sp + 8u <= sp_end && found_ret < 10; sp += 8u) {
                        const uintptr_t word = *reinterpret_cast<const uintptr_t*>(sp);
                        HMODULE m = nullptr;
                        if (word >= 0x10000u &&
                            ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                 reinterpret_cast<LPCSTR>(word), &m) &&
                            m) {
                            MEMORY_BASIC_INFORMATION cmbi{};
                            if (::VirtualQuery(reinterpret_cast<const void*>(word), &cmbi, sizeof(cmbi)) != 0u &&
                                (cmbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                 PAGE_EXECUTE_WRITECOPY)) != 0u) {
                                char sym[160]{};
                                resolve_addr(word, sym, sizeof(sym));
                                if (!stack_syms.empty()) stack_syms += " <- ";
                                stack_syms += sym;
                                ++found_ret;
                            }
                        }
                    }
                }

                std::string reg_strs;
                const std::pair<const char*, uintptr_t> regs[] = {
                    {"rcx", ctx->Rcx}, {"rdx", ctx->Rdx}, {"r8", ctx->R8}, {"r9", ctx->R9},
                    {"rax", ctx->Rax}, {"rbx", ctx->Rbx}, {"rsi", ctx->Rsi}, {"rdi", ctx->Rdi}};
                for (const auto& [rname, rval] : regs) {
                    char sbuf[128]{};
                    if (peek_ascii(rval, sbuf, sizeof(sbuf))) {
                        reg_strs += std::string(" ") + rname + "=\"" + sbuf + "\"";
                    }
                }

                char report[2048]{};
                std::snprintf(
                    report, sizeof(report),
                    "[OpenScarlett][VEH-INT3] #%u tid=%lu rip=0x%llX (%s) rax=0x%llX rbx=0x%llX rcx=0x%llX "
                    "rdx=0x%llX rsi=0x%llX rdi=0x%llX r8=0x%llX r9=0x%llX r14=0x%llX rsp=0x%llX%s\n"
                    "  stack: %s\n",
                    diag_n, static_cast<unsigned long>(::GetCurrentThreadId()),
                    static_cast<unsigned long long>(ctx->Rip), rip_sym,
                    static_cast<unsigned long long>(ctx->Rax), static_cast<unsigned long long>(ctx->Rbx),
                    static_cast<unsigned long long>(ctx->Rcx), static_cast<unsigned long long>(ctx->Rdx),
                    static_cast<unsigned long long>(ctx->Rsi), static_cast<unsigned long long>(ctx->Rdi),
                    static_cast<unsigned long long>(ctx->R8), static_cast<unsigned long long>(ctx->R9),
                    static_cast<unsigned long long>(ctx->R14), static_cast<unsigned long long>(ctx->Rsp),
                    reg_strs.c_str(), stack_syms.empty() ? "<none>" : stack_syms.c_str());
                std::fprintf(stderr, "%s", report);
                std::fflush(stderr);
                if (!s_veh_log_path.empty()) {
                    FILE* vf = nullptr;
                    if (::fopen_s(&vf, s_veh_log_path.c_str(), "ab") == 0 && vf) {
                        std::fwrite(report, 1u, std::strlen(report), vf);
                        std::fflush(vf);
                        std::fclose(vf);
                    }
                }
            }
        }

        if (continue_int3_mode != 0) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(p, &mbi, sizeof(mbi)) != 0u && mbi.State == MEM_COMMIT && *p == 0xCCu) {
                bool allow_continue = (continue_int3_mode > 0);
                if (!allow_continue) {
                    // In retail/master Xbox titles (e.g. game_scarlett_master.exe), non-fatal
                    // Assertf/__debugbreak() sites emit a single 0xCC followed by valid recovery
                    // instructions (not CC CC alignment padding).
                    HMODULE hit_mod = nullptr;
                    const HMODULE main_exe = ::GetModuleHandleW(nullptr);
                    if (main_exe != nullptr &&
                        ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                             reinterpret_cast<LPCSTR>(p), &hit_mod) &&
                        hit_mod == main_exe) {
                        const uintptr_t reg_end =
                            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                        if (reinterpret_cast<uintptr_t>(p) + 1u < reg_end && p[1] != 0xCCu) {
                            // Detect leaf chunk-counter end-of-table cold assert (`je +0x38 -> CC EB C5`
                            // with `C3` ret at `p - 16` when `rdx == r9`): return valid chunk count in `eax`
                            // instead of jumping backward into out-of-bounds sentinel `[rdx + 8]`!
                            if (reinterpret_cast<uintptr_t>(p) >=
                                    reinterpret_cast<uintptr_t>(mbi.BaseAddress) + 16u &&
                                reinterpret_cast<uintptr_t>(p) + 2u < reg_end &&
                                p[1] == 0xEBu && p[2] == 0xC5u && p[-16] == 0xC3u &&
                                ctx->Rdx == ctx->R9) {
                                ctx->Rip = reinterpret_cast<uintptr_t>(p - 16);
                                return EXCEPTION_CONTINUE_EXECUTION;
                            }
                            // Detect EnqueueChunksForRead end-of-table cold assert (`CC E9 A5 FD FF FF`
                            // preceded by `31 C0 E9 3E FF FF FF` epilogue jump when `rdi == rdx`):
                            // return valid chunk count in `eax` via epilogue instead of looping into OOB `[rdi]`.
                            if (reinterpret_cast<uintptr_t>(p) >=
                                    reinterpret_cast<uintptr_t>(mbi.BaseAddress) + 8u &&
                                reinterpret_cast<uintptr_t>(p) + 5u < reg_end &&
                                p[1] == 0xE9u && p[-7] == 0x31u && p[-6] == 0xC0u &&
                                p[-5] == 0xE9u && ctx->Rdi == ctx->Rdx) {
                                ctx->Rip = reinterpret_cast<uintptr_t>(p - 5);
                                return EXCEPTION_CONTINUE_EXECUTION;
                            }
                            allow_continue = true;
                        }
                    }
                }
                if (allow_continue) {
                    ctx->Rip += 1u;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        FlushTelemetryInternal(true);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (exc_code != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Fast path for concurrent threads whose #UD was queued right as a sibling thread NOP-patched Rip.
    {
        const uintptr_t rip_val = static_cast<uintptr_t>(ctx->Rip);
        uint8_t patched_len = 0u;
        ::AcquireSRWLockShared(&g_veh_hotpatch_lock);
        auto* sites = GetHotpatchedSitesLocked();
        auto it = sites->find(rip_val);
        if (it != sites->end()) {
            patched_len = it->second.length;
        }
        ::ReleaseSRWLockShared(&g_veh_hotpatch_lock);
        if (patched_len > 0u) {
            ctx->Rip += patched_len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    uint8_t insn[kMaxX64InsnBytes]{};
    const size_t avail = ReadInsnBytesSafe64(p, insn);
    DecodedPrefixes64 pfx{};
    if (avail >= 3u && ScanPrefixes64(insn, avail, pfx) && !pfx.has_lock) {
        const size_t op_off = pfx.opcode_offset;

        // 1. Two-byte 0F 01 <ext> AMD Zen 2 instructions (CLZERO, MONITORX, MWAITX, RDPRU, MCOMMIT).
        if (op_off + 2u < avail && insn[op_off] == 0x0Fu && insn[op_off + 1u] == 0x01u) {
            const uint8_t ext = insn[op_off + 2u];
            const size_t insn_len = op_off + 3u;
            if (insn_len <= kMaxX64InsnBytes) {
                // 1a. CLZERO ([67/REX] 0F 01 FC): zeroes 64-byte cache line at [rAX & ~63].
                if (ext == 0xFCu && !pfx.has_66 && pfx.last_f2_f3 == 0u) {
                    const uint64_t addr = pfx.has_67
                                              ? (static_cast<uint64_t>(static_cast<uint32_t>(ctx->Rax)) & ~63ull)
                                              : (ctx->Rax & ~63ull);
                    std::memset(reinterpret_cast<void*>(addr), 0, 64u);
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // 1b. MCOMMIT (F3 [REX] 0F 01 FA): AMD memory-commit barrier -> hot-patch to NOPs.
                if (ext == 0xFAu && pfx.last_f2_f3 == 0xF3u && !pfx.has_66) {
                    HotPatchToNopsThreadSafe(p, insn_len);
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // 1c. MONITORX ([67/REX] 0F 01 FA) & MWAITX ([67/REX] 0F 01 FB).
                if ((ext == 0xFAu || ext == 0xFBu) && pfx.last_f2_f3 == 0u && !pfx.has_66) {
                    if (ext == 0xFAu) {
                        HotPatchToNopsThreadSafe(p, insn_len);
                    } else {
                        thread_local uintptr_t s_last_mwaitx_rip = 0u;
                        thread_local uint32_t s_mwaitx_streak = 0u;
                        if (reinterpret_cast<uintptr_t>(p) == s_last_mwaitx_rip) {
                            ++s_mwaitx_streak;
                        } else {
                            s_last_mwaitx_rip = reinterpret_cast<uintptr_t>(p);
                            s_mwaitx_streak = 1u;
                        }
                        if (s_mwaitx_streak <= 2u) {
                            ::SwitchToThread();
                        } else {
                            using PfnNtDelayExecution = LONG(NTAPI*)(BOOLEAN, PLARGE_INTEGER);
                            static const PfnNtDelayExecution s_nt_delay = []() noexcept -> PfnNtDelayExecution {
                                HMODULE ntdll = g_real_GetModuleHandleW
                                                    ? g_real_GetModuleHandleW(L"ntdll.dll")
                                                    : ::GetModuleHandleW(L"ntdll.dll");
                                if (!ntdll) return nullptr;
                                PfnGetProcAddress gpa = g_real_GetProcAddress ? g_real_GetProcAddress : &::GetProcAddress;
                                return reinterpret_cast<PfnNtDelayExecution>(gpa(ntdll, "NtDelayExecution"));
                            }();
                            if (s_nt_delay) {
                                LARGE_INTEGER rel{};
                                rel.QuadPart = -2500LL;  // 250 us relative micro-sleep (matching Zen 2 MWAITX idle)
                                (void)s_nt_delay(FALSE, &rel);
                            } else {
                                ::Sleep(1u);
                            }
                        }
                    }
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // 1d. RDPRU ([REX] 0F 01 FD): read processor register -> return RDTSC in EDX:EAX.
                if (ext == 0xFDu && pfx.last_f2_f3 == 0u && !pfx.has_66) {
                    const uint64_t tsc = __rdtsc();
                    ctx->Rax = static_cast<uint32_t>(tsc & 0xFFFFFFFFull);
                    ctx->Rdx = static_cast<uint32_t>(tsc >> 32u);
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        // 2. CLWB (66 [REX] 0F AE /6) and CLFLUSHOPT (66 [REX] 0F AE /7):
        //    Supported on Xbox Series X Zen 2 and Intel Ice Lake+, missing on Skylake/Coffee Lake.
        //    Hot-patch exact ModRM+SIB+disp instruction bytes to NOPs.
        if (pfx.has_66 && pfx.last_f2_f3 == 0u &&
            op_off + 2u < avail && insn[op_off] == 0x0Fu && insn[op_off + 1u] == 0xAEu) {
            const uint8_t modrm = insn[op_off + 2u];
            const uint8_t mod = static_cast<uint8_t>((modrm >> 6u) & 3u);
            const uint8_t reg = static_cast<uint8_t>((modrm >> 3u) & 7u);
            if (mod != 3u && (reg == 6u || reg == 7u)) {
                const size_t insn_len = DecodeModRmInsnLength64(insn, avail, op_off + 2u, 0u);
                if (insn_len > 0u) {
                    HotPatchToNopsThreadSafe(p, insn_len);
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        // 3. RDPID r64 (F3 [REX] 0F C7 /7, mod == 3):
        //    Supported on Zen 2 & Ice Lake+, missing on Coffee Lake / Comet Lake laptops.
        if (pfx.last_f2_f3 == 0xF3u && !pfx.has_66 &&
            op_off + 2u < avail && insn[op_off] == 0x0Fu && insn[op_off + 1u] == 0xC7u) {
            const uint8_t modrm = insn[op_off + 2u];
            const uint8_t mod = static_cast<uint8_t>((modrm >> 6u) & 3u);
            const uint8_t reg = static_cast<uint8_t>((modrm >> 3u) & 7u);
            const uint8_t rm_raw = static_cast<uint8_t>(modrm & 7u);
            if (mod == 3u && reg == 7u) {
                const size_t insn_len = DecodeModRmInsnLength64(insn, avail, op_off + 2u, 0u);
                if (insn_len > 0u) {
                    unsigned int aux = 0u;
                    (void)__rdtscp(&aux);
                    const uint32_t dst_gpr = static_cast<uint32_t>(rm_raw) |
                                             ((pfx.rex & 0x01u) ? 8u : 0u);
                    WriteContextGpr64(ctx, dst_gpr, static_cast<uint64_t>(aux));
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        // 4. AMD SSE4A (Xbox Series X|S Zen 2 hardware ISA absent on all Intel CPUs):
        //    - INSERTQ xmm1, xmm2, imm8, imm8 : F2 [REX] 0F 78 /r ib ib
        //    - INSERTQ xmm1, xmm2             : F2 [REX] 0F 79 /r
        //    - EXTRQ   xmm1, imm8, imm8       : 66 [REX] 0F 78 /0 ib ib
        //    - EXTRQ   xmm1, xmm2             : 66 [REX] 0F 79 /r
        //    - MOVNTSD m64, xmm1              : F2 [REX] 0F 2B /r
        //    - MOVNTSS m32, xmm1              : F3 [REX] 0F 2B /r
        if (op_off + 2u < avail && insn[op_off] == 0x0Fu) {
            const uint8_t op2 = insn[op_off + 1u];
            if ((op2 == 0x78u || op2 == 0x79u) &&
                (pfx.last_f2_f3 == 0xF2u || (pfx.has_66 && pfx.last_f2_f3 == 0u))) {
                const uint8_t modrm = insn[op_off + 2u];
                const uint8_t mod = static_cast<uint8_t>((modrm >> 6u) & 3u);
                const uint8_t reg_raw = static_cast<uint8_t>((modrm >> 3u) & 7u);
                const uint8_t rm_raw = static_cast<uint8_t>(modrm & 7u);
                const size_t imm_cnt = (op2 == 0x78u) ? 2u : 0u;
                const size_t insn_len = DecodeModRmInsnLength64(insn, avail, op_off + 2u, imm_cnt);
                if (mod == 3u && insn_len > 0u) {
                    const uint32_t reg_idx =
                        static_cast<uint32_t>(reg_raw) | ((pfx.rex & 0x04u) ? 8u : 0u);
                    const uint32_t rm_idx =
                        static_cast<uint32_t>(rm_raw) | ((pfx.rex & 0x01u) ? 8u : 0u);
                    M128A* xmms = &ctx->Xmm0;
                    auto make_mask = [](uint32_t bit_len) noexcept -> uint64_t {
                        return (bit_len == 0u || bit_len >= 64u) ? ~0ull : ((1ull << bit_len) - 1ull);
                    };
                    if (pfx.last_f2_f3 == 0xF2u && op2 == 0x78u) {
                        const uint32_t bit_len = static_cast<uint32_t>(insn[op_off + 3u] & 0x3Fu);
                        const uint32_t bit_idx = static_cast<uint32_t>(insn[op_off + 4u] & 0x3Fu);
                        const uint64_t mask = make_mask(bit_len);
                        const uint64_t dst = xmms[reg_idx].Low;
                        const uint64_t src = xmms[rm_idx].Low;
                        xmms[reg_idx].Low = (dst & ~(mask << bit_idx)) | ((src & mask) << bit_idx);
                        ctx->Rip += insn_len;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    if (pfx.last_f2_f3 == 0xF2u && op2 == 0x79u) {
                        const uint64_t ctrl = static_cast<uint64_t>(xmms[rm_idx].High);
                        const uint32_t bit_len = static_cast<uint32_t>(ctrl & 0x3Full);
                        const uint32_t bit_idx = static_cast<uint32_t>((ctrl >> 8u) & 0x3Full);
                        const uint64_t mask = make_mask(bit_len);
                        const uint64_t dst = xmms[reg_idx].Low;
                        const uint64_t src = xmms[rm_idx].Low;
                        xmms[reg_idx].Low = (dst & ~(mask << bit_idx)) | ((src & mask) << bit_idx);
                        ctx->Rip += insn_len;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    if (pfx.has_66 && pfx.last_f2_f3 == 0u && op2 == 0x78u && reg_raw == 0u) {
                        const uint32_t bit_len = static_cast<uint32_t>(insn[op_off + 3u] & 0x3Fu);
                        const uint32_t bit_idx = static_cast<uint32_t>(insn[op_off + 4u] & 0x3Fu);
                        const uint64_t mask = make_mask(bit_len);
                        xmms[rm_idx].Low = (xmms[rm_idx].Low >> bit_idx) & mask;
                        ctx->Rip += insn_len;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    if (pfx.has_66 && pfx.last_f2_f3 == 0u && op2 == 0x79u) {
                        const uint64_t ctrl = xmms[rm_idx].Low;
                        const uint32_t bit_len = static_cast<uint32_t>(ctrl & 0x3Full);
                        const uint32_t bit_idx = static_cast<uint32_t>((ctrl >> 8u) & 0x3Full);
                        const uint64_t mask = make_mask(bit_len);
                        xmms[reg_idx].Low = (xmms[reg_idx].Low >> bit_idx) & mask;
                        ctx->Rip += insn_len;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            } else if (op2 == 0x2Bu && (pfx.last_f2_f3 == 0xF2u || pfx.last_f2_f3 == 0xF3u)) {
                const uint8_t modrm = insn[op_off + 2u];
                const uint8_t mod = static_cast<uint8_t>((modrm >> 6u) & 3u);
                const uint8_t reg_raw = static_cast<uint8_t>((modrm >> 3u) & 7u);
                const size_t insn_len = DecodeModRmInsnLength64(insn, avail, op_off + 2u, 0u);
                uint64_t ea = 0ull;
                if (mod != 3u && insn_len > 0u &&
                    ComputeEffectiveAddress64(ctx, pfx, insn, op_off + 2u, insn_len, ea)) {
                    const uint32_t reg_idx =
                        static_cast<uint32_t>(reg_raw) | ((pfx.rex & 0x04u) ? 8u : 0u);
                    const M128A* xmms = &ctx->Xmm0;
                    if (pfx.last_f2_f3 == 0xF2u) {
                        const uint64_t v64 = xmms[reg_idx].Low;
                        std::memcpy(reinterpret_cast<void*>(ea), &v64, sizeof(v64));
                    } else {
                        const uint32_t v32 = static_cast<uint32_t>(xmms[reg_idx].Low & 0xFFFFFFFFull);
                        std::memcpy(reinterpret_cast<void*>(ea), &v32, sizeof(v32));
                    }
                    ctx->Rip += insn_len;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
    }

    if (g_trace_ods) {
        static std::atomic<uint32_t> s_unhandled_ud{0u};
        if (s_unhandled_ud.fetch_add(1u, std::memory_order_relaxed) < 4u) {
            HMODULE mod = nullptr;
            char mod_name[MAX_PATH]{};
            uintptr_t rva = 0u;
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(ctx->Rip), &mod) &&
                mod) {
                ::GetModuleFileNameA(mod, mod_name, static_cast<DWORD>(sizeof(mod_name)));
                rva = static_cast<uintptr_t>(ctx->Rip) - reinterpret_cast<uintptr_t>(mod);
            }
            uint8_t bytes[12]{};
            size_t dump_avail = 0u;
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(p, &mbi, sizeof(mbi)) != 0u && mbi.State == MEM_COMMIT) {
                const uintptr_t seg_end =
                    reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                const uintptr_t cur_ip = reinterpret_cast<uintptr_t>(p);
                if (seg_end > cur_ip) {
                    dump_avail = std::min<size_t>(sizeof(bytes), static_cast<size_t>(seg_end - cur_ip));
                    std::memcpy(bytes, p, dump_avail);
                }
            }
            std::fprintf(stderr,
                         "[OpenScarlett][VEH-UD] rip=0x%llX (%s+0x%llX) avail=%zu bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X rax=0x%llX rcx=0x%llX rdx=0x%llX\n",
                         static_cast<unsigned long long>(ctx->Rip), mod_name,
                         static_cast<unsigned long long>(rva), dump_avail,
                         bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                         bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                         static_cast<unsigned long long>(ctx->Rax),
                         static_cast<unsigned long long>(ctx->Rcx),
                         static_cast<unsigned long long>(ctx->Rdx));
            std::fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
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

void* ResolveVfsHookForSymbol(const char* name) {
    if (!name) {
        return nullptr;
    }
    static const HookBinding kVfsBindings[] = {
        {"CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA),
         reinterpret_cast<void**>(&g_real_CreateFileA)},
        {"CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW),
         reinterpret_cast<void**>(&g_real_CreateFileW)},
        {"CreateFile2", reinterpret_cast<void*>(&Hook_CreateFile2),
         reinterpret_cast<void**>(&g_real_CreateFile2)},
        {"CloseHandle", reinterpret_cast<void*>(&Hook_CloseHandle),
         reinterpret_cast<void**>(&g_real_CloseHandle)},
        {"GetFinalPathNameByHandleW", reinterpret_cast<void*>(&Hook_GetFinalPathNameByHandleW),
         reinterpret_cast<void**>(&g_real_GetFinalPathNameByHandleW)},
        {"GetFileAttributesA", reinterpret_cast<void*>(&Hook_GetFileAttributesA),
         reinterpret_cast<void**>(&g_real_GetFileAttributesA)},
        {"GetFileAttributesW", reinterpret_cast<void*>(&Hook_GetFileAttributesW),
         reinterpret_cast<void**>(&g_real_GetFileAttributesW)},
        {"GetFileAttributesExA", reinterpret_cast<void*>(&Hook_GetFileAttributesExA),
         reinterpret_cast<void**>(&g_real_GetFileAttributesExA)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&Hook_GetFileAttributesExW),
         reinterpret_cast<void**>(&g_real_GetFileAttributesExW)},
        {"SetFileAttributesA", reinterpret_cast<void*>(&Hook_SetFileAttributesA),
         reinterpret_cast<void**>(&g_real_SetFileAttributesA)},
        {"SetFileAttributesW", reinterpret_cast<void*>(&Hook_SetFileAttributesW),
         reinterpret_cast<void**>(&g_real_SetFileAttributesW)},
        {"CreateDirectoryA", reinterpret_cast<void*>(&Hook_CreateDirectoryA),
         reinterpret_cast<void**>(&g_real_CreateDirectoryA)},
        {"CreateDirectoryW", reinterpret_cast<void*>(&Hook_CreateDirectoryW),
         reinterpret_cast<void**>(&g_real_CreateDirectoryW)},
        {"DeleteFileA", reinterpret_cast<void*>(&Hook_DeleteFileA),
         reinterpret_cast<void**>(&g_real_DeleteFileA)},
        {"DeleteFileW", reinterpret_cast<void*>(&Hook_DeleteFileW),
         reinterpret_cast<void**>(&g_real_DeleteFileW)},
        {"MoveFileW", reinterpret_cast<void*>(&Hook_MoveFileW),
         reinterpret_cast<void**>(&g_real_MoveFileW)},
        {"MoveFileExA", reinterpret_cast<void*>(&Hook_MoveFileExA),
         reinterpret_cast<void**>(&g_real_MoveFileExA)},
        {"MoveFileExW", reinterpret_cast<void*>(&Hook_MoveFileExW),
         reinterpret_cast<void**>(&g_real_MoveFileExW)},
        {"CopyFileA", reinterpret_cast<void*>(&Hook_CopyFileA),
         reinterpret_cast<void**>(&g_real_CopyFileA)},
        {"CopyFileW", reinterpret_cast<void*>(&Hook_CopyFileW),
         reinterpret_cast<void**>(&g_real_CopyFileW)},
        {"CopyFile2", reinterpret_cast<void*>(&Hook_CopyFile2),
         reinterpret_cast<void**>(&g_real_CopyFile2)},
        {"ReplaceFileW", reinterpret_cast<void*>(&Hook_ReplaceFileW),
         reinterpret_cast<void**>(&g_real_ReplaceFileW)},
        {"RemoveDirectoryA", reinterpret_cast<void*>(&Hook_RemoveDirectoryA),
         reinterpret_cast<void**>(&g_real_RemoveDirectoryA)},
        {"RemoveDirectoryW", reinterpret_cast<void*>(&Hook_RemoveDirectoryW),
         reinterpret_cast<void**>(&g_real_RemoveDirectoryW)},
        {"FindFirstFileA", reinterpret_cast<void*>(&Hook_FindFirstFileA),
         reinterpret_cast<void**>(&g_real_FindFirstFileA)},
        {"FindFirstFileW", reinterpret_cast<void*>(&Hook_FindFirstFileW),
         reinterpret_cast<void**>(&g_real_FindFirstFileW)},
        {"FindFirstFileExA", reinterpret_cast<void*>(&Hook_FindFirstFileExA),
         reinterpret_cast<void**>(&g_real_FindFirstFileExA)},
        {"FindFirstFileExW", reinterpret_cast<void*>(&Hook_FindFirstFileExW),
         reinterpret_cast<void**>(&g_real_FindFirstFileExW)},
        {"GetDiskFreeSpaceExW", reinterpret_cast<void*>(&Hook_GetDiskFreeSpaceExW),
         reinterpret_cast<void**>(&g_real_GetDiskFreeSpaceExW)},
        {"GetDiskFreeSpaceW", reinterpret_cast<void*>(&Hook_GetDiskFreeSpaceW),
         reinterpret_cast<void**>(&g_real_GetDiskFreeSpaceW)},
        {"SetCurrentDirectoryA", reinterpret_cast<void*>(&Hook_SetCurrentDirectoryA),
         reinterpret_cast<void**>(&g_real_SetCurrentDirectoryA)},
        {"SetCurrentDirectoryW", reinterpret_cast<void*>(&Hook_SetCurrentDirectoryW),
         reinterpret_cast<void**>(&g_real_SetCurrentDirectoryW)},
    };
    for (const auto& b : kVfsBindings) {
        if (std::strcmp(name, b.symbol_name) == 0) {
            return (*b.real_slot != nullptr) ? b.replacement : nullptr;
        }
    }
    return nullptr;
}

void* ResolveVfsHookForRealAddress(ULONGLONG addr) {
    if (addr == 0) {
        return nullptr;
    }
    auto match = [addr](auto real_fn, auto hook_fn) -> void* {
        if (real_fn != nullptr && reinterpret_cast<ULONGLONG>(real_fn) == addr) {
            return reinterpret_cast<void*>(hook_fn);
        }
        return nullptr;
    };
    if (void* p = match(g_real_CreateFileA, &Hook_CreateFileA)) return p;
    if (void* p = match(g_real_CreateFileW, &Hook_CreateFileW)) return p;
    if (void* p = match(g_real_CreateFile2, &Hook_CreateFile2)) return p;
    if (void* p = match(g_real_CloseHandle, &Hook_CloseHandle)) return p;
    if (void* p = match(g_real_GetFinalPathNameByHandleW, &Hook_GetFinalPathNameByHandleW)) return p;
    if (void* p = match(g_real_GetFileAttributesA, &Hook_GetFileAttributesA)) return p;
    if (void* p = match(g_real_GetFileAttributesW, &Hook_GetFileAttributesW)) return p;
    if (void* p = match(g_real_GetFileAttributesExA, &Hook_GetFileAttributesExA)) return p;
    if (void* p = match(g_real_GetFileAttributesExW, &Hook_GetFileAttributesExW)) return p;
    if (void* p = match(g_real_SetFileAttributesA, &Hook_SetFileAttributesA)) return p;
    if (void* p = match(g_real_SetFileAttributesW, &Hook_SetFileAttributesW)) return p;
    if (void* p = match(g_real_CreateDirectoryA, &Hook_CreateDirectoryA)) return p;
    if (void* p = match(g_real_CreateDirectoryW, &Hook_CreateDirectoryW)) return p;
    if (void* p = match(g_real_DeleteFileA, &Hook_DeleteFileA)) return p;
    if (void* p = match(g_real_DeleteFileW, &Hook_DeleteFileW)) return p;
    if (void* p = match(g_real_MoveFileW, &Hook_MoveFileW)) return p;
    if (void* p = match(g_real_MoveFileExA, &Hook_MoveFileExA)) return p;
    if (void* p = match(g_real_MoveFileExW, &Hook_MoveFileExW)) return p;
    if (void* p = match(g_real_CopyFileA, &Hook_CopyFileA)) return p;
    if (void* p = match(g_real_CopyFileW, &Hook_CopyFileW)) return p;
    if (void* p = match(g_real_CopyFile2, &Hook_CopyFile2)) return p;
    if (void* p = match(g_real_ReplaceFileW, &Hook_ReplaceFileW)) return p;
    if (void* p = match(g_real_RemoveDirectoryA, &Hook_RemoveDirectoryA)) return p;
    if (void* p = match(g_real_RemoveDirectoryW, &Hook_RemoveDirectoryW)) return p;
    if (void* p = match(g_real_FindFirstFileA, &Hook_FindFirstFileA)) return p;
    if (void* p = match(g_real_FindFirstFileW, &Hook_FindFirstFileW)) return p;
    if (void* p = match(g_real_FindFirstFileExA, &Hook_FindFirstFileExA)) return p;
    if (void* p = match(g_real_FindFirstFileExW, &Hook_FindFirstFileExW)) return p;
    if (void* p = match(g_real_GetDiskFreeSpaceExW, &Hook_GetDiskFreeSpaceExW)) return p;
    if (void* p = match(g_real_GetDiskFreeSpaceW, &Hook_GetDiskFreeSpaceW)) return p;
    if (void* p = match(g_real_SetCurrentDirectoryA, &Hook_SetCurrentDirectoryA)) return p;
    if (void* p = match(g_real_SetCurrentDirectoryW, &Hook_SetCurrentDirectoryW)) return p;
    return nullptr;
}

void* ResolveHookForSymbol(const char* name) {
    if (!name) {
        return nullptr;
    }
    if (void* vfs_hook = ResolveVfsHookForSymbol(name)) {
        return vfs_hook;
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
        {"GetModuleFileNameA", reinterpret_cast<void*>(&Hook_GetModuleFileNameA),
         reinterpret_cast<void**>(&g_real_GetModuleFileNameA)},
        {"GetModuleFileNameW", reinterpret_cast<void*>(&Hook_GetModuleFileNameW),
         reinterpret_cast<void**>(&g_real_GetModuleFileNameW)},
        {"GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
         reinterpret_cast<void**>(&g_real_GetProcAddress)},
        {"CreateProcessA", reinterpret_cast<void*>(&Hook_CreateProcessA),
         reinterpret_cast<void**>(&g_real_CreateProcessA)},
        {"CreateProcessW", reinterpret_cast<void*>(&Hook_CreateProcessW),
         reinterpret_cast<void**>(&g_real_CreateProcessW)},
        {"ExitProcess", reinterpret_cast<void*>(&Hook_ExitProcess),
         reinterpret_cast<void**>(&g_real_ExitProcess)},
        {"OutputDebugStringA", reinterpret_cast<void*>(&Hook_OutputDebugStringA),
         reinterpret_cast<void**>(&g_real_OutputDebugStringA)},
        {"GlobalMemoryStatusEx", reinterpret_cast<void*>(&Hook_GlobalMemoryStatusEx),
         reinterpret_cast<void**>(&g_real_GlobalMemoryStatusEx)},
        {"RegOpenKeyW", reinterpret_cast<void*>(&Hook_RegOpenKeyW),
         reinterpret_cast<void**>(&g_real_RegOpenKeyExW)},
        {"RegOpenKeyA", reinterpret_cast<void*>(&Hook_RegOpenKeyA),
         reinterpret_cast<void**>(&g_real_RegOpenKeyExA)},
        {"RegOpenKeyExW", reinterpret_cast<void*>(&Hook_RegOpenKeyExW),
         reinterpret_cast<void**>(&g_real_RegOpenKeyExW)},
        {"RegOpenKeyExA", reinterpret_cast<void*>(&Hook_RegOpenKeyExA),
         reinterpret_cast<void**>(&g_real_RegOpenKeyExA)},
        {"RegQueryValueExW", reinterpret_cast<void*>(&Hook_RegQueryValueExW),
         reinterpret_cast<void**>(&g_real_RegQueryValueExW)},
        {"RegQueryValueExA", reinterpret_cast<void*>(&Hook_RegQueryValueExA),
         reinterpret_cast<void**>(&g_real_RegQueryValueExA)},
        {"RegGetValueW", reinterpret_cast<void*>(&Hook_RegGetValueW),
         reinterpret_cast<void**>(&g_real_RegGetValueW)},
        {"RegGetValueA", reinterpret_cast<void*>(&Hook_RegGetValueA),
         reinterpret_cast<void**>(&g_real_RegGetValueA)},
        {"RegCloseKey", reinterpret_cast<void*>(&Hook_RegCloseKey),
         reinterpret_cast<void**>(&g_real_RegCloseKey)},
        {"WinHttpConnect", reinterpret_cast<void*>(&Hook_WinHttpConnect),
         reinterpret_cast<void**>(&g_real_WinHttpConnect)},
        {"CoCreateInstance", reinterpret_cast<void*>(&Hook_CoCreateInstance),
         reinterpret_cast<void**>(&g_real_CoCreateInstance)},
        {"NtCreateFile", reinterpret_cast<void*>(&Hook_NtCreateFile),
         reinterpret_cast<void**>(&g_real_NtCreateFile)},
        {"NtOpenFile", reinterpret_cast<void*>(&Hook_NtOpenFile),
         reinterpret_cast<void**>(&g_real_NtOpenFile)},
        {"NtQueryAttributesFile", reinterpret_cast<void*>(&Hook_NtQueryAttributesFile),
         reinterpret_cast<void**>(&g_real_NtQueryAttributesFile)},
        {"NtClose", reinterpret_cast<void*>(&Hook_NtClose),
         reinterpret_cast<void**>(&g_real_NtClose)},
        {"NtQueryDirectoryFile", reinterpret_cast<void*>(&Hook_NtQueryDirectoryFile),
         reinterpret_cast<void**>(&g_real_NtQueryDirectoryFile)},
        {"NtQueryDirectoryFileEx", reinterpret_cast<void*>(&Hook_NtQueryDirectoryFileEx),
         reinterpret_cast<void**>(&g_real_NtQueryDirectoryFileEx)},
        {"NtSetInformationFile", reinterpret_cast<void*>(&Hook_NtSetInformationFile),
         reinterpret_cast<void**>(&g_real_NtSetInformationFile)},
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
    if (void* vfs_hook = ResolveVfsHookForRealAddress(addr)) {
        return vfs_hook;
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
    if (void* p = match(g_real_GetModuleFileNameA, &Hook_GetModuleFileNameA)) return p;
    if (void* p = match(g_real_GetModuleFileNameW, &Hook_GetModuleFileNameW)) return p;
    if (void* p = match(g_real_GetProcAddress, &Hook_GetProcAddress)) return p;
    if (void* p = match(g_kernelbase_CreateFile2, &Hook_CreateFile2)) return p;
    if (void* p = match(g_real_CreateProcessA, &Hook_CreateProcessA)) return p;
    if (void* p = match(g_real_CreateProcessW, &Hook_CreateProcessW)) return p;
    if (void* p = match(g_kernelbase_CreateProcessA, &Hook_CreateProcessA)) return p;
    if (void* p = match(g_kernelbase_CreateProcessW, &Hook_CreateProcessW)) return p;
    if (void* p = match(g_real_ExitProcess, &Hook_ExitProcess)) return p;
    if (void* p = match(g_real_OutputDebugStringA, &Hook_OutputDebugStringA)) return p;
    if (void* p = match(g_real_GlobalMemoryStatusEx, &Hook_GlobalMemoryStatusEx)) return p;
    if (void* p = match(g_kernelbase_GlobalMemoryStatusEx, &Hook_GlobalMemoryStatusEx)) return p;
    EnsureAdvapi32ProcsResolved();
    EnsureComProcsResolved();
    if (void* p = match(g_real_RegOpenKeyW, &Hook_RegOpenKeyW)) return p;
    if (void* p = match(g_real_RegOpenKeyA, &Hook_RegOpenKeyA)) return p;
    if (void* p = match(g_real_RegOpenKeyExW, &Hook_RegOpenKeyExW)) return p;
    if (void* p = match(g_advapi32_RegOpenKeyExW, &Hook_RegOpenKeyExW)) return p;
    if (void* p = match(g_real_RegOpenKeyExA, &Hook_RegOpenKeyExA)) return p;
    if (void* p = match(g_advapi32_RegOpenKeyExA, &Hook_RegOpenKeyExA)) return p;
    if (void* p = match(g_real_RegQueryValueExW, &Hook_RegQueryValueExW)) return p;
    if (void* p = match(g_advapi32_RegQueryValueExW, &Hook_RegQueryValueExW)) return p;
    if (void* p = match(g_real_RegQueryValueExA, &Hook_RegQueryValueExA)) return p;
    if (void* p = match(g_advapi32_RegQueryValueExA, &Hook_RegQueryValueExA)) return p;
    if (void* p = match(g_real_RegGetValueW, &Hook_RegGetValueW)) return p;
    if (void* p = match(g_advapi32_RegGetValueW, &Hook_RegGetValueW)) return p;
    if (void* p = match(g_real_RegGetValueA, &Hook_RegGetValueA)) return p;
    if (void* p = match(g_advapi32_RegGetValueA, &Hook_RegGetValueA)) return p;
    if (void* p = match(g_real_RegCloseKey, &Hook_RegCloseKey)) return p;
    if (void* p = match(g_advapi32_RegCloseKey, &Hook_RegCloseKey)) return p;
    if (void* p = match(g_real_WinHttpConnect, &Hook_WinHttpConnect)) return p;
    EnsureComProcsResolved();
    if (void* p = match(g_real_CoCreateInstance, &Hook_CoCreateInstance)) return p;
    if (void* p = match(g_ole32_CoCreateInstance, &Hook_CoCreateInstance)) return p;
    if (void* p = match(g_real_NtCreateFile, &Hook_NtCreateFile)) return p;
    if (void* p = match(g_real_NtOpenFile, &Hook_NtOpenFile)) return p;
    if (void* p = match(g_real_NtQueryAttributesFile, &Hook_NtQueryAttributesFile)) return p;
    if (void* p = match(g_real_NtClose, &Hook_NtClose)) return p;
    if (void* p = match(g_real_NtQueryDirectoryFile, &Hook_NtQueryDirectoryFile)) return p;
    if (void* p = match(g_real_NtQueryDirectoryFileEx, &Hook_NtQueryDirectoryFileEx)) return p;
    if (void* p = match(g_real_NtSetInformationFile, &Hook_NtSetInformationFile)) return p;
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
                     uint32_t iat_rva,
                     bool vfs_only = false) {
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
                    replacement = vfs_only ? ResolveVfsHookForSymbol(ibn->Name)
                                           : ResolveHookForSymbol(ibn->Name);
                }
            }
        }
        if (!replacement) {
            replacement = vfs_only ? ResolveVfsHookForRealAddress(iat[i].u1.Function)
                                   : ResolveHookForRealAddress(iat[i].u1.Function);
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

// RAII pin guard preventing concurrent module unload races during import sweeps.
class ScopedModulePin {
public:
    explicit ScopedModulePin(HMODULE candidate) noexcept {
        if (!candidate) {
            return;
        }
        HMODULE pinned = nullptr;
        const BOOL ok = g_real_GetModuleHandleExW
            ? g_real_GetModuleHandleExW(
                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                  reinterpret_cast<LPCWSTR>(candidate),
                  &pinned)
            : ::GetModuleHandleExW(
                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                  reinterpret_cast<LPCWSTR>(candidate),
                  &pinned);
        if (ok && pinned == candidate) {
            mod_ = pinned;
        } else if (ok && pinned != nullptr) {
            ::FreeLibrary(pinned);
        }
    }

    ~ScopedModulePin() {
        if (mod_ != nullptr) {
            ::FreeLibrary(mod_);
        }
    }

    ScopedModulePin(const ScopedModulePin&) = delete;
    ScopedModulePin& operator=(const ScopedModulePin&) = delete;

    explicit operator bool() const noexcept { return mod_ != nullptr; }
    HMODULE Get() const noexcept { return mod_; }

private:
    HMODULE mod_ = nullptr;
};

void PatchModuleImports(HMODULE mod) {
    if (!mod || mod == g_self_module) {
        return;
    }

    // Pin the module across the entire PE header / IAT / delay-load walk so concurrent
    // FreeLibrary calls on another thread cannot unmap the image mid-sweep.
    ScopedModulePin pin(mod);
    if (!pin) {
        return;
    }
    if (IsWindowsSystemModule(mod)) {
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
        uint32_t module_handle_rva = 0;
        bool is_delay_load = false;
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

    const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto& dly_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];

    // Pre-scan both static & delay-load imports to detect console-exclusive PE targets.
    bool module_has_console_exclusive = false;
    if (!is_shim_module) {
        if (imp_dir.VirtualAddress != 0 && imp_dir.VirtualAddress < image_size) {
            auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
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
        if (!module_has_console_exclusive && dly_dir.VirtualAddress != 0 && dly_dir.VirtualAddress < image_size) {
            auto* desc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dly_dir.VirtualAddress);
            for (size_t idx = 0; idx < 1024; ++idx) {
                if (dly_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) > image_size) {
                    break;
                }
                const auto& d = desc[idx];
                if (d.DllNameRVA == 0 && d.ImportAddressTableRVA == 0) {
                    break;
                }
                if (d.Attributes.RvaBased == 0 || d.DllNameRVA == 0 || d.DllNameRVA >= image_size) {
                    continue;
                }
                const char* dll_name = reinterpret_cast<const char*>(base + d.DllNameRVA);
                if (openscarlett::ClassifyImportDll(dll_name) ==
                    openscarlett::DllCategory::ConsoleExclusive) {
                    module_has_console_exclusive = true;
                    break;
                }
            }
        }
    }

    auto resolve_shim_for_descriptor = [&](const char* dll_name,
                                           openscarlett::DllRouteDecision& out_decision) -> bool {
        out_decision = g_routing_table.ResolveRoute(
            dll_name, g_shim_dir_w, g_guest_dir_w, module_has_console_exclusive);
        if (out_decision.policy != openscarlett::DllRoutePolicy::Shim) {
            std::string canon_dll;
            openscarlett::ForwarderResolutionKind fkind =
                openscarlett::ForwarderResolutionKind::Unresolved;
            bool via_api_set = false;
            bool via_alias = false;
            if (g_pe_forwarder.ResolveTargetDll(dll_name, canon_dll, fkind, via_api_set, via_alias) &&
                fkind == openscarlett::ForwarderResolutionKind::OpenScarlettShim) {
                out_decision = g_routing_table.ResolveRoute(
                    canon_dll, g_shim_dir_w, g_guest_dir_w, module_has_console_exclusive);
                out_decision.requested_name = openscarlett::NormalizeForwarderDllName(dll_name, true);
                out_decision.aliased = true;
            }
        }
        return out_decision.policy == openscarlett::DllRoutePolicy::Shim &&
               !out_decision.resolved_path.empty();
    };

    // 1. Standard Import Address Table (IMAGE_DIRECTORY_ENTRY_IMPORT)
    if (imp_dir.VirtualAddress != 0 && imp_dir.VirtualAddress < image_size) {
        auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
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
                openscarlett::DllRouteDecision decision;
                if (resolve_shim_for_descriptor(dll_name, decision)) {
                    deferred_shim_iats.push_back(
                        DeferredShimIat{std::move(decision), d.OriginalFirstThunk, d.FirstThunk, 0u, false});
                }
            }
        }
    }

    // 2. Delay-Load Import Table (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
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
            if (IsHookableImportDll(dll_name)) {
                PatchThunkTable(base, image_size, d.ImportNameTableRVA, d.ImportAddressTableRVA);
            } else if (!is_shim_module) {
                openscarlett::DllRouteDecision decision;
                if (resolve_shim_for_descriptor(dll_name, decision)) {
                    deferred_shim_iats.push_back(DeferredShimIat{
                        std::move(decision),
                        d.ImportNameTableRVA,
                        d.ImportAddressTableRVA,
                        d.ModuleHandleRVA,
                        true});
                }
            }
        }
    }

    g_patched_modules[mod] = PatchedModuleKey{mod, image_size, timestamp};
    ::ReleaseSRWLockExclusive(&g_patch_lock);

    // 3. Rebind static & delay-load IAT slots for routed/aliased Xbox shims (outside g_patch_lock).
    bool has_deferred_unbound = false;
    for (const auto& entry : deferred_shim_iats) {
        HMODULE shim_mod = g_real_GetModuleHandleW
                               ? g_real_GetModuleHandleW(entry.decision.resolved_path.c_str())
                               : nullptr;
        if (!shim_mod && g_initialized.load(std::memory_order_acquire)) {
            shim_mod = LoadRoutedDll(entry.decision, entry.decision.resolved_path,
                                     entry.is_delay_load ? "DelayIAT:route" : "IAT:route",
                                     nullptr, 0, false);
        }
        if (shim_mod != nullptr) {
            if (entry.is_delay_load && entry.module_handle_rva != 0 &&
                entry.module_handle_rva + sizeof(ULONGLONG) <= image_size) {
                auto* mod_slot = reinterpret_cast<ULONGLONG*>(base + entry.module_handle_rva);
                (void)WriteIatSlot(mod_slot, reinterpret_cast<void*>(shim_mod));
            }
            const uint32_t patched = RebindShimIatTable(
                base, image_size, entry.int_rva, entry.iat_rva, shim_mod);
            if (patched > 0 && !g_shutting_down.load(std::memory_order_acquire)) {
                if (entry.is_delay_load) {
                    openscarlett::CallTelemetry::Global().Record(
                        "loader",
                        std::string("delay_iat_bound:") + entry.decision.canonical_name,
                        openscarlett::CallOutcome::Ok,
                        static_cast<int32_t>(patched),
                        -1);
                } else if (entry.decision.aliased && !entry.decision.requested_name.empty()) {
                    openscarlett::CallTelemetry::Global().Record(
                        "loader",
                        std::string("iat_alias:") + entry.decision.requested_name + "->" +
                            entry.decision.canonical_name,
                        openscarlett::CallOutcome::Ok,
                        static_cast<int32_t>(patched),
                        -1);
                }
            }
        } else if (!g_initialized.load(std::memory_order_acquire)) {
            // Still inside DllMain loader lock: defer rebinding until the first post-init sweep.
            has_deferred_unbound = true;
        }
    }

    if (has_deferred_unbound) {
        ::AcquireSRWLockExclusive(&g_patch_lock);
        g_patched_modules.erase(mod);
        ::ReleaseSRWLockExclusive(&g_patch_lock);
    }

    InspectModuleTlsAndPdata(mod, base, nt, image_size);
}

// Patch VFS-only file APIs inside C/C++ runtime DLLs (ucrtbase.dll, msvcp140.dll, etc.)
// so CRT stdio/fstream/filesystem calls (_wfsopen, fopen, _wstat64, remove, std::filesystem,
// LevelDB) on Xbox virtual drives (P:\, S:\, T:\, G:\, D:\) translate cleanly through VfsMounts.
void PatchCrtRuntimeVfsImports() {
    static constexpr const wchar_t* kCrtDlls[] = {
        L"ucrtbase.dll",
        L"ucrtbased.dll",
        L"msvcp140.dll",
        L"msvcp140d.dll",
        L"msvcp140_1.dll",
        L"msvcp140_2.dll",
        L"vcruntime140.dll",
        L"vcruntime140d.dll",
    };

    for (const wchar_t* dll_name : kCrtDlls) {
        HMODULE mod = g_real_GetModuleHandleW ? g_real_GetModuleHandleW(dll_name)
                                              : ::GetModuleHandleW(dll_name);
        if (!mod || mod == g_self_module) {
            continue;
        }
        ScopedModulePin pin(mod);
        if (!pin) {
            continue;
        }

        auto* base = reinterpret_cast<uint8_t*>(mod);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            continue;
        }
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            continue;
        }

        const uint32_t image_size = nt->OptionalHeader.SizeOfImage;
        const uint32_t timestamp = nt->FileHeader.TimeDateStamp;

        ::AcquireSRWLockExclusive(&g_patch_lock);
        auto it = g_patched_crt_vfs_modules.find(mod);
        if (it != g_patched_crt_vfs_modules.end() &&
            it->second.size_of_image == image_size &&
            it->second.timestamp == timestamp) {
            ::ReleaseSRWLockExclusive(&g_patch_lock);
            continue;
        }

        const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imp_dir.VirtualAddress != 0 && imp_dir.VirtualAddress < image_size) {
            auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
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
                const char* imp_dll = reinterpret_cast<const char*>(base + d.Name);
                if (IsHookableImportDll(imp_dll)) {
                    PatchThunkTable(base, image_size, d.OriginalFirstThunk, d.FirstThunk, true);
                }
            }
        }

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
                if (d.Attributes.RvaBased == 0 || d.DllNameRVA == 0 || d.DllNameRVA >= image_size) {
                    continue;
                }
                const char* imp_dll = reinterpret_cast<const char*>(base + d.DllNameRVA);
                if (IsHookableImportDll(imp_dll)) {
                    PatchThunkTable(base, image_size, d.ImportNameTableRVA, d.ImportAddressTableRVA, true);
                }
            }
        }

        g_patched_crt_vfs_modules[mod] = PatchedModuleKey{mod, image_size, timestamp};
        ::ReleaseSRWLockExclusive(&g_patch_lock);
    }
}

void PatchAllLoadedNonSystemModules() {
    PatchCrtRuntimeVfsImports();
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
        ScopedModulePin pin(mod);
        if (!pin) {
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
    g_real_GetModuleFileNameA = reinterpret_cast<PfnGetModuleFileNameA>(::GetProcAddress(k32, "GetModuleFileNameA"));
    g_real_GetModuleFileNameW = reinterpret_cast<PfnGetModuleFileNameW>(::GetProcAddress(k32, "GetModuleFileNameW"));
    g_real_GetProcAddress = reinterpret_cast<PfnGetProcAddress>(::GetProcAddress(k32, "GetProcAddress"));
    g_real_CreateFileA = reinterpret_cast<PfnCreateFileA>(::GetProcAddress(k32, "CreateFileA"));
    g_real_CreateFileW = reinterpret_cast<PfnCreateFileW>(::GetProcAddress(k32, "CreateFileW"));
    g_real_CreateFile2 = reinterpret_cast<PfnCreateFile2>(::GetProcAddress(k32, "CreateFile2"));
    if (g_kernelbase_mod != nullptr) {
        g_kernelbase_CreateFile2 =
            reinterpret_cast<PfnCreateFile2>(::GetProcAddress(g_kernelbase_mod, "CreateFile2"));
        if (g_real_CreateFile2 == nullptr) {
            g_real_CreateFile2 = g_kernelbase_CreateFile2;
        }
    }
    g_real_CloseHandle = reinterpret_cast<PfnCloseHandle>(::GetProcAddress(k32, "CloseHandle"));
    g_real_GetFinalPathNameByHandleW = reinterpret_cast<PfnGetFinalPathNameByHandleW>(
        ::GetProcAddress(k32, "GetFinalPathNameByHandleW"));
    g_real_GetFileAttributesA = reinterpret_cast<PfnGetFileAttributesA>(::GetProcAddress(k32, "GetFileAttributesA"));
    g_real_GetFileAttributesW = reinterpret_cast<PfnGetFileAttributesW>(::GetProcAddress(k32, "GetFileAttributesW"));
    g_real_GetFileAttributesExA = reinterpret_cast<PfnGetFileAttributesExA>(::GetProcAddress(k32, "GetFileAttributesExA"));
    g_real_GetFileAttributesExW = reinterpret_cast<PfnGetFileAttributesExW>(::GetProcAddress(k32, "GetFileAttributesExW"));
    g_real_SetFileAttributesA = reinterpret_cast<PfnSetFileAttributesA>(::GetProcAddress(k32, "SetFileAttributesA"));
    g_real_SetFileAttributesW = reinterpret_cast<PfnSetFileAttributesW>(::GetProcAddress(k32, "SetFileAttributesW"));
    g_real_CreateDirectoryA = reinterpret_cast<PfnCreateDirectoryA>(::GetProcAddress(k32, "CreateDirectoryA"));
    g_real_CreateDirectoryW = reinterpret_cast<PfnCreateDirectoryW>(::GetProcAddress(k32, "CreateDirectoryW"));
    g_real_DeleteFileA = reinterpret_cast<PfnDeleteFileA>(::GetProcAddress(k32, "DeleteFileA"));
    g_real_DeleteFileW = reinterpret_cast<PfnDeleteFileW>(::GetProcAddress(k32, "DeleteFileW"));
    g_real_MoveFileW = reinterpret_cast<PfnMoveFileW>(::GetProcAddress(k32, "MoveFileW"));
    g_real_MoveFileExA = reinterpret_cast<PfnMoveFileExA>(::GetProcAddress(k32, "MoveFileExA"));
    g_real_MoveFileExW = reinterpret_cast<PfnMoveFileExW>(::GetProcAddress(k32, "MoveFileExW"));
    g_real_CopyFileA = reinterpret_cast<PfnCopyFileA>(::GetProcAddress(k32, "CopyFileA"));
    g_real_CopyFileW = reinterpret_cast<PfnCopyFileW>(::GetProcAddress(k32, "CopyFileW"));
    g_real_CopyFile2 = reinterpret_cast<PfnCopyFile2>(::GetProcAddress(k32, "CopyFile2"));
    if (!g_real_CopyFile2 && g_kernelbase_mod != nullptr) {
        g_real_CopyFile2 = reinterpret_cast<PfnCopyFile2>(::GetProcAddress(g_kernelbase_mod, "CopyFile2"));
    }
    g_real_ReplaceFileW = reinterpret_cast<PfnReplaceFileW>(::GetProcAddress(k32, "ReplaceFileW"));
    g_real_RemoveDirectoryA = reinterpret_cast<PfnRemoveDirectoryA>(::GetProcAddress(k32, "RemoveDirectoryA"));
    g_real_RemoveDirectoryW = reinterpret_cast<PfnRemoveDirectoryW>(::GetProcAddress(k32, "RemoveDirectoryW"));
    g_real_FindFirstFileA = reinterpret_cast<PfnFindFirstFileA>(::GetProcAddress(k32, "FindFirstFileA"));
    g_real_FindFirstFileW = reinterpret_cast<PfnFindFirstFileW>(::GetProcAddress(k32, "FindFirstFileW"));
    g_real_FindFirstFileExA = reinterpret_cast<PfnFindFirstFileExA>(::GetProcAddress(k32, "FindFirstFileExA"));
    g_real_FindFirstFileExW = reinterpret_cast<PfnFindFirstFileExW>(::GetProcAddress(k32, "FindFirstFileExW"));
    g_real_GetDiskFreeSpaceExW = reinterpret_cast<PfnGetDiskFreeSpaceExW>(::GetProcAddress(k32, "GetDiskFreeSpaceExW"));
    g_real_GetDiskFreeSpaceW = reinterpret_cast<PfnGetDiskFreeSpaceW>(::GetProcAddress(k32, "GetDiskFreeSpaceW"));
    g_real_SetCurrentDirectoryA = reinterpret_cast<PfnSetCurrentDirectoryA>(::GetProcAddress(k32, "SetCurrentDirectoryA"));
    g_real_SetCurrentDirectoryW = reinterpret_cast<PfnSetCurrentDirectoryW>(::GetProcAddress(k32, "SetCurrentDirectoryW"));
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
    g_real_GlobalMemoryStatusEx =
        reinterpret_cast<PfnGlobalMemoryStatusEx>(::GetProcAddress(k32, "GlobalMemoryStatusEx"));
    if (g_kernelbase_mod != nullptr) {
        g_kernelbase_GlobalMemoryStatusEx = reinterpret_cast<PfnGlobalMemoryStatusEx>(
            ::GetProcAddress(g_kernelbase_mod, "GlobalMemoryStatusEx"));
        if (g_real_GlobalMemoryStatusEx == nullptr) {
            g_real_GlobalMemoryStatusEx = g_kernelbase_GlobalMemoryStatusEx;
        }
        g_real_RegOpenKeyExW =
            reinterpret_cast<PfnRegOpenKeyExW>(::GetProcAddress(g_kernelbase_mod, "RegOpenKeyExW"));
        g_real_RegOpenKeyExA =
            reinterpret_cast<PfnRegOpenKeyExA>(::GetProcAddress(g_kernelbase_mod, "RegOpenKeyExA"));
        g_real_RegQueryValueExW = reinterpret_cast<PfnRegQueryValueExW>(
            ::GetProcAddress(g_kernelbase_mod, "RegQueryValueExW"));
        g_real_RegQueryValueExA = reinterpret_cast<PfnRegQueryValueExA>(
            ::GetProcAddress(g_kernelbase_mod, "RegQueryValueExA"));
        g_real_RegGetValueW =
            reinterpret_cast<PfnRegGetValueW>(::GetProcAddress(g_kernelbase_mod, "RegGetValueW"));
        g_real_RegGetValueA =
            reinterpret_cast<PfnRegGetValueA>(::GetProcAddress(g_kernelbase_mod, "RegGetValueA"));
        g_real_RegCloseKey =
            reinterpret_cast<PfnRegCloseKey>(::GetProcAddress(g_kernelbase_mod, "RegCloseKey"));
    }
    if (!g_real_RegOpenKeyExW) {
        g_real_RegOpenKeyExW = reinterpret_cast<PfnRegOpenKeyExW>(::GetProcAddress(k32, "RegOpenKeyExW"));
    }
    if (!g_real_RegOpenKeyExA) {
        g_real_RegOpenKeyExA = reinterpret_cast<PfnRegOpenKeyExA>(::GetProcAddress(k32, "RegOpenKeyExA"));
    }
    if (!g_real_RegQueryValueExW) {
        g_real_RegQueryValueExW =
            reinterpret_cast<PfnRegQueryValueExW>(::GetProcAddress(k32, "RegQueryValueExW"));
    }
    if (!g_real_RegQueryValueExA) {
        g_real_RegQueryValueExA =
            reinterpret_cast<PfnRegQueryValueExA>(::GetProcAddress(k32, "RegQueryValueExA"));
    }
    if (!g_real_RegGetValueW) {
        g_real_RegGetValueW = reinterpret_cast<PfnRegGetValueW>(::GetProcAddress(k32, "RegGetValueW"));
    }
    if (!g_real_RegGetValueA) {
        g_real_RegGetValueA = reinterpret_cast<PfnRegGetValueA>(::GetProcAddress(k32, "RegGetValueA"));
    }
    if (!g_real_RegCloseKey) {
        g_real_RegCloseKey = reinterpret_cast<PfnRegCloseKey>(::GetProcAddress(k32, "RegCloseKey"));
    }
    EnsureAdvapi32ProcsResolved();
    EnsureComProcsResolved();
    {
        char off_buf[16]{};
        const DWORD off_len = ::GetEnvironmentVariableA("OPENSCARLETT_OFFLINE", off_buf, static_cast<DWORD>(sizeof(off_buf)));
        g_offline_block_winhttp = (off_len > 0u && off_buf[0] != '0');
        HMODULE h_winhttp = ::GetModuleHandleW(L"winhttp.dll");
        if (h_winhttp != nullptr) {
            g_real_WinHttpConnect =
                reinterpret_cast<PfnWinHttpConnect>(::GetProcAddress(h_winhttp, "WinHttpConnect"));
        }
    }
    g_trace_ods = (ReadEnvW(L"OPENSCARLETT_TRACE_ODS") == L"1");
    if (g_ntdll_mod != nullptr) {
        g_real_NtCreateFile =
            reinterpret_cast<PfnNtCreateFile>(::GetProcAddress(g_ntdll_mod, "NtCreateFile"));
        g_real_NtOpenFile =
            reinterpret_cast<PfnNtOpenFile>(::GetProcAddress(g_ntdll_mod, "NtOpenFile"));
        g_real_NtQueryAttributesFile = reinterpret_cast<PfnNtQueryAttributesFile>(
            ::GetProcAddress(g_ntdll_mod, "NtQueryAttributesFile"));
        g_real_NtClose =
            reinterpret_cast<PfnNtClose>(::GetProcAddress(g_ntdll_mod, "NtClose"));
        g_real_NtQueryDirectoryFile = reinterpret_cast<PfnNtQueryDirectoryFile>(
            ::GetProcAddress(g_ntdll_mod, "NtQueryDirectoryFile"));
        g_real_NtQueryDirectoryFileEx = reinterpret_cast<PfnNtQueryDirectoryFileEx>(
            ::GetProcAddress(g_ntdll_mod, "NtQueryDirectoryFileEx"));
        g_real_NtSetInformationFile = reinterpret_cast<PfnNtSetInformationFile>(
            ::GetProcAddress(g_ntdll_mod, "NtSetInformationFile"));
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
    g_main_exe_path_w = main_exe_path;
    std::wstring main_exe_dir = std::filesystem::path(main_exe_path).parent_path().wstring();
    const std::wstring default_vfs_root = openscarlett::ResolveDefaultVfsRootW(g_shim_dir_w);
    g_vfs.Configure(openscarlett::VfsMounts::BuildDefaultConfig(main_exe_dir, default_vfs_root));
    g_guest_dir_w = g_vfs.GetGameRoot().empty() ? main_exe_dir : g_vfs.GetGameRoot();
    if (!g_guest_dir_w.empty()) {
        ::SetEnvironmentVariableW(L"OPENSCARLETT_GUEST_DIR", g_guest_dir_w.c_str());
    }

    const std::wstring virt_mod_env = ToLowerAsciiW(ReadEnvW(L"OPENSCARLETT_VIRTUALIZE_MODULE_PATHS"));
    g_virtualize_module_paths.store(
        virt_mod_env == L"1" || virt_mod_env == L"true" || virt_mod_env == L"yes",
        std::memory_order_release);
    const std::wstring virt_drv_env = ReadEnvW(L"OPENSCARLETT_VIRTUAL_MODULE_DRIVE");
    if (!virt_drv_env.empty()) {
        ::AcquireSRWLockExclusive(&g_state_lock);
        g_virtual_module_drive_w = virt_drv_env;
        ::ReleaseSRWLockExclusive(&g_state_lock);
    }

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

    // Install Zen 2 CLWB/CLFLUSHOPT/CLZERO/MCOMMIT emulation VEH for pre-Ice-Lake Intel CPUs.
    ::AddVectoredExceptionHandler(1u, &Zen2IllegalInstructionVeh);

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

__declspec(dllexport) int __cdecl OpenScarlettHook_IsTelemetryActive() {
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return 0;
    }
    return (g_probe_unknown_imports || !g_telemetry_path_utf8.empty()) ? 1 : 0;
}

__declspec(dllexport) void __cdecl OpenScarlettHook_RecordCall(const char* module_name,
                                                               const char* symbol_name,
                                                               uint8_t outcome,
                                                               int32_t hr,
                                                               int32_t vtable_slot) {
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return;
    }
    if (!g_probe_unknown_imports && g_telemetry_path_utf8.empty()) {
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

__declspec(dllexport) int __cdecl OpenScarlettHook_PatchModuleImports(HMODULE mod) {
    if (!mod) {
        return 0;
    }
    PatchModuleImports(mod);
    return 1;
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

__declspec(dllexport) int __cdecl OpenScarlettHook_SetVirtualModulePathMode(
    int enable,
    const wchar_t* virtual_drive_prefix) {
    ::AcquireSRWLockExclusive(&g_state_lock);
    if (virtual_drive_prefix && virtual_drive_prefix[0] != L'\0') {
        g_virtual_module_drive_w = virtual_drive_prefix;
    } else if (enable) {
        if (g_virtual_module_drive_w.empty()) {
            g_virtual_module_drive_w = L"G:";
        }
    }
    ::ReleaseSRWLockExclusive(&g_state_lock);
    g_virtualize_module_paths.store(enable != 0, std::memory_order_release);
    return 1;
}

__declspec(dllexport) DWORD WINAPI OpenScarlettHook_GetModuleFileNameW(
    HMODULE hModule,
    LPWSTR lpFilename,
    DWORD nSize) {
    return Hook_GetModuleFileNameW(hModule, lpFilename, nSize);
}

__declspec(dllexport) DWORD WINAPI OpenScarlettHook_GetModuleFileNameA(
    HMODULE hModule,
    LPSTR lpFilename,
    DWORD nSize) {
    return Hook_GetModuleFileNameA(hModule, lpFilename, nSize);
}

__declspec(dllexport) HMODULE WINAPI OpenScarlettHook_GetModuleHandleW(LPCWSTR lpModuleName) {
    return Hook_GetModuleHandleW(lpModuleName);
}

__declspec(dllexport) HMODULE WINAPI OpenScarlettHook_GetModuleHandleA(LPCSTR lpModuleName) {
    return Hook_GetModuleHandleA(lpModuleName);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_ReplaceFileW(
    LPCWSTR lpReplacedFileName,
    LPCWSTR lpReplacementFileName,
    LPCWSTR lpBackupFileName,
    DWORD dwReplaceFlags) {
    return Hook_ReplaceFileW(
        lpReplacedFileName, lpReplacementFileName, lpBackupFileName, dwReplaceFlags, nullptr, nullptr);
}

__declspec(dllexport) HANDLE WINAPI OpenScarlettHook_CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile) {
    return Hook_CreateFileW(
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile);
}

__declspec(dllexport) HANDLE WINAPI OpenScarlettHook_FindFirstFileA(
    LPCSTR lpFileName,
    LPWIN32_FIND_DATAA lpFindFileData) {
    return Hook_FindFirstFileA(lpFileName, lpFindFileData);
}

__declspec(dllexport) HANDLE WINAPI OpenScarlettHook_FindFirstFileExA(
    LPCSTR lpFileName,
    FINDEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFindFileData,
    FINDEX_SEARCH_OPS fSearchOp,
    LPVOID lpSearchFilter,
    DWORD dwAdditionalFlags) {
    return Hook_FindFirstFileExA(
        lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_GetFileAttributesExA(
    LPCSTR lpFileName,
    GET_FILEEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFileInformation) {
    return Hook_GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_CreateDirectoryA(
    LPCSTR lpPathName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    return Hook_CreateDirectoryA(lpPathName, lpSecurityAttributes);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_RemoveDirectoryA(LPCSTR lpPathName) {
    return Hook_RemoveDirectoryA(lpPathName);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_MoveFileExA(
    LPCSTR lpExistingFileName,
    LPCSTR lpNewFileName,
    DWORD dwFlags) {
    return Hook_MoveFileExA(lpExistingFileName, lpNewFileName, dwFlags);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_CopyFileA(
    LPCSTR lpExistingFileName,
    LPCSTR lpNewFileName,
    BOOL bFailIfExists) {
    return Hook_CopyFileA(lpExistingFileName, lpNewFileName, bFailIfExists);
}

__declspec(dllexport) HANDLE WINAPI OpenScarlettHook_CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile) {
    return Hook_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                            dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

__declspec(dllexport) HANDLE WINAPI OpenScarlettHook_CreateFile2(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    DWORD dwCreationDisposition,
    OS_CREATEFILE2_EXTENDED_PARAMETERS* pCreateExParams) {
    return Hook_CreateFile2(
        lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
}

__declspec(dllexport) BOOL WINAPI OpenScarlettHook_CloseHandle(HANDLE hObject) {
    return Hook_CloseHandle(hObject);
}

__declspec(dllexport) DWORD WINAPI OpenScarlettHook_GetFinalPathNameByHandleW(
    HANDLE hFile,
    LPWSTR lpszFilePath,
    DWORD cchFilePath,
    DWORD dwFlags) {
    return Hook_GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtClose(HANDLE Handle) {
    return Hook_NtClose(Handle);
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtQueryDirectoryFile(
    HANDLE FileHandle,
    HANDLE Event,
    PVOID ApcRoutine,
    PVOID ApcContext,
    OS_IO_STATUS_BLOCK* IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    ULONG FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    OS_UNICODE_STRING* FileName,
    BOOLEAN RestartScan) {
    return Hook_NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                                     FileInformation, Length, FileInformationClass,
                                     ReturnSingleEntry, FileName, RestartScan);
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtQueryDirectoryFileEx(
    HANDLE FileHandle,
    HANDLE Event,
    PVOID ApcRoutine,
    PVOID ApcContext,
    OS_IO_STATUS_BLOCK* IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    ULONG FileInformationClass,
    ULONG QueryFlags,
    OS_UNICODE_STRING* FileName) {
    return Hook_NtQueryDirectoryFileEx(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                                       FileInformation, Length, FileInformationClass,
                                       QueryFlags, FileName);
}

__declspec(dllexport) OS_NTSTATUS NTAPI OpenScarlettHook_NtSetInformationFile(
    HANDLE FileHandle,
    OS_IO_STATUS_BLOCK* IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    ULONG FileInformationClass) {
    return Hook_NtSetInformationFile(
        FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
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
