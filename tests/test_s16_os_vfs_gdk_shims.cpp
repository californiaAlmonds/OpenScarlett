// Sprint 16 Workstream 4 Comprehensive Test Suite:
//   1. VFS TRUNCATE_EXISTING / FILE_OVERWRITE / FILE_OVERWRITE_IF on read-only base files
//   2. VFS NtQueryDirectoryFile / Ex merged dedup + whiteout, NtSetInformationFile,
//      GetFinalPathNameByHandleW normalization, and zero C:\ path leaks
//   3. XAsync no-live-op no-op safety & GDK threading COM table slots 8, 16, 18
//   4. XGameSaveCreateContainer official Xbox container name rules (E_GS_INVALID_CONTAINER_NAME)
//   5. Complete offline XDisplay / XUserDevice / XGameEvent / XGameUi / XGameStreaming COM+C parity
//   6. Opt-in non-blocking Native Win32 Host System UI dialog mode (auto-dismiss & cancel)
//   7. All 6 retired non-media stub DLLs (hix_xs, xbtp IOCP zero-leak, xtfpix, gamecoreclient,
//      minuserx, xtfdebugmonitor)

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/loader/vfs_mounts.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void CheckImpl(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define OS_REQUIRE(x) CheckImpl(static_cast<bool>(x), #x, __LINE__)

fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0u || n >= MAX_PATH) {
        return fs::current_path();
    }
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::string ReadFileAll(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

bool WriteFileAll(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();
    return ofs.good();
}

void ClearReadOnlyTree(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const DWORD attrs = ::GetFileAttributesW(it->path().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
            ::SetFileAttributesW(it->path().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }
    fs::remove_all(root, ec);
}

// Local NT ABI mirror matching hook_dll.cpp
using OS_NTSTATUS = LONG;
constexpr OS_NTSTATUS OS_STATUS_SUCCESS = 0x00000000L;
constexpr OS_NTSTATUS OS_STATUS_NO_MORE_FILES = static_cast<OS_NTSTATUS>(0x80000006UL);
constexpr OS_NTSTATUS OS_STATUS_OBJECT_NAME_NOT_FOUND = static_cast<OS_NTSTATUS>(0xC0000034UL);
constexpr ULONG OS_FILE_OPEN = 1UL;
constexpr ULONG OS_FILE_OVERWRITE = 4UL;
constexpr ULONG OS_FILE_OVERWRITE_IF = 5UL;
constexpr ULONG OS_FILE_DIRECTORY_FILE = 0x00000001UL;
constexpr ULONG OS_FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020UL;
constexpr ULONG OS_FILE_NON_DIRECTORY_FILE = 0x00000040UL;
constexpr ULONG OS_OBJ_CASE_INSENSITIVE = 0x00000040UL;
constexpr ULONG OS_SL_RESTART_SCAN = 0x00000001UL;
constexpr ULONG OS_SL_RETURN_SINGLE_ENTRY = 0x00000002UL;
constexpr ULONG OS_SL_NO_CURSOR_UPDATE = 0x00000010UL;

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

struct OS_FILE_DISPOSITION_INFORMATION {
    BOOLEAN DeleteFile;
};

struct OS_FILE_END_OF_FILE_INFORMATION {
    LARGE_INTEGER EndOfFile;
};

struct OS_FILE_ALLOCATION_INFORMATION {
    LARGE_INTEGER AllocationSize;
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

struct OS_CREATEFILE2_EXTENDED_PARAMETERS {
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
};

// ===========================================================================
// Test 1 & 2: VFS TRUNCATE_EXISTING + NT Directory/SetInfo + FinalPath + Zero C:
// ===========================================================================
void TestVfsTruncateAndNtHooks(const fs::path& bin_dir, const fs::path& work_root) {
    const fs::path g_base = work_root / "vfs_g_base";
    const fs::path g_ov = work_root / "vfs_g_overlay";
    const fs::path t_root = work_root / "vfs_t";
    const fs::path s_root = work_root / "vfs_s";
    std::error_code ec;
    fs::create_directories(g_base / "Data", ec);
    fs::create_directories(g_base / "Dir", ec);
    fs::create_directories(g_ov / "Dir", ec);
    fs::create_directories(t_root, ec);
    fs::create_directories(s_root, ec);

    const std::string kOrig1 = "ORIGINAL_READONLY_CONTENT_12345";
    const std::string kOrig2 = "NT_OVERWRITE_BASE_PAYLOAD_99999";
    const std::string kOrig3 = "CREATEFILE2_BASE_PAYLOAD_77777";
    const fs::path f_ro1 = g_base / "Data" / "readonly.cfg";
    const fs::path f_ro2 = g_base / "Data" / "nt_ro.bin";
    const fs::path f_ro3 = g_base / "Data" / "cf2_ro.bin";
    OS_REQUIRE(WriteFileAll(f_ro1, kOrig1));
    OS_REQUIRE(WriteFileAll(f_ro2, kOrig2));
    OS_REQUIRE(WriteFileAll(f_ro3, kOrig3));
    OS_REQUIRE(::SetFileAttributesW(f_ro1.c_str(), FILE_ATTRIBUTE_READONLY) != 0);
    OS_REQUIRE(::SetFileAttributesW(f_ro2.c_str(), FILE_ATTRIBUTE_READONLY) != 0);
    OS_REQUIRE(::SetFileAttributesW(f_ro3.c_str(), FILE_ATTRIBUTE_READONLY) != 0);

    // Prepare merged directory test files
    OS_REQUIRE(WriteFileAll(g_base / "Dir" / "only_base.txt", "base-only"));
    OS_REQUIRE(WriteFileAll(g_base / "Dir" / "shared.txt", "0123456789"));  // 10 bytes
    OS_REQUIRE(WriteFileAll(g_base / "Dir" / "to_delete.txt", "delete-me"));
    OS_REQUIRE(WriteFileAll(g_base / "Dir" / "to_rename.txt", "rename-payload"));
    OS_REQUIRE(::SetFileAttributesW((g_base / "Dir" / "to_rename.txt").c_str(),
                                    FILE_ATTRIBUTE_READONLY) != 0);
    OS_REQUIRE(WriteFileAll(g_ov / "Dir" / "only_overlay.txt", "overlay-only"));
    OS_REQUIRE(WriteFileAll(g_ov / "Dir" / "shared.txt",
                            "OVERLAY_SHADOWS_BASE_EXACTLY_42_BYTES_XXXX"));  // 42 bytes

    HMODULE hook = ::LoadLibraryW((bin_dir / "openscarlett_hook.dll").c_str());
    OS_REQUIRE(hook != nullptr);
    if (!hook) return;

    using PfnCfg = int(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*);
    using PfnCreateW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using PfnCreate2 = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, OS_CREATEFILE2_EXTENDED_PARAMETERS*);
    using PfnCloseH = BOOL(WINAPI*)(HANDLE);
    using PfnFinalPathW = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
    using PfnNtCreate = OS_NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, OS_OBJECT_ATTRIBUTES*,
                                            OS_IO_STATUS_BLOCK*, PLARGE_INTEGER, ULONG, ULONG,
                                            ULONG, ULONG, PVOID, ULONG);
    using PfnNtOpen = OS_NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, OS_OBJECT_ATTRIBUTES*,
                                          OS_IO_STATUS_BLOCK*, ULONG, ULONG);
    using PfnNtClose = OS_NTSTATUS(NTAPI*)(HANDLE);
    using PfnNtQueryDir = OS_NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, OS_IO_STATUS_BLOCK*,
                                              PVOID, ULONG, ULONG, BOOLEAN, OS_UNICODE_STRING*, BOOLEAN);
    using PfnNtQueryDirEx = OS_NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, OS_IO_STATUS_BLOCK*,
                                                PVOID, ULONG, ULONG, ULONG, OS_UNICODE_STRING*);
    using PfnNtSetInfo = OS_NTSTATUS(NTAPI*)(HANDLE, OS_IO_STATUS_BLOCK*, PVOID, ULONG, ULONG);

    auto fn_cfg = reinterpret_cast<PfnCfg>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_ConfigureVfsForTestW")));
    auto fn_cfw = reinterpret_cast<PfnCreateW>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_CreateFileW")));
    auto fn_cf2 = reinterpret_cast<PfnCreate2>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_CreateFile2")));
    auto fn_close = reinterpret_cast<PfnCloseH>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_CloseHandle")));
    auto fn_final = reinterpret_cast<PfnFinalPathW>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_GetFinalPathNameByHandleW")));
    auto fn_nt_create = reinterpret_cast<PfnNtCreate>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_NtCreateFile")));
    auto fn_nt_open = reinterpret_cast<PfnNtOpen>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_NtOpenFile")));
    auto fn_nt_close = reinterpret_cast<PfnNtClose>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_NtClose")));
    auto fn_nt_qdir = reinterpret_cast<PfnNtQueryDir>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_NtQueryDirectoryFile")));
    auto fn_nt_qdirex = reinterpret_cast<PfnNtQueryDirEx>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_NtQueryDirectoryFileEx")));
    auto fn_nt_setinfo = reinterpret_cast<PfnNtSetInfo>(reinterpret_cast<void*>(::GetProcAddress(hook, "OpenScarlettHook_NtSetInformationFile")));

    OS_REQUIRE(fn_cfg && fn_cfw && fn_cf2 && fn_close && fn_final &&
               fn_nt_create && fn_nt_open && fn_nt_close &&
               fn_nt_qdir && fn_nt_qdirex && fn_nt_setinfo);

    OS_REQUIRE(fn_cfg(g_base.c_str(), g_ov.c_str(), t_root.c_str(), s_root.c_str()) == 1);

    // 1a. CreateFileW(TRUNCATE_EXISTING) on un-overlaid READONLY game file
    HANDLE h1 = fn_cfw(L"G:\\Data\\readonly.cfg",
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ,
                       nullptr,
                       TRUNCATE_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    OS_REQUIRE(h1 != INVALID_HANDLE_VALUE);
    if (h1 != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        OS_REQUIRE(::GetFileSizeEx(h1, &sz) != 0 && sz.QuadPart == 0);
        const char msg[] = "TRUNCATED_IN_OVERLAY";
        DWORD written = 0u;
        OS_REQUIRE(::WriteFile(h1, msg, static_cast<DWORD>(sizeof(msg) - 1u), &written, nullptr) != 0);
        OS_REQUIRE(written == sizeof(msg) - 1u);

        // Verify GetFinalPathNameByHandleW normalizes cleanly without leaking overlay prefix!
        wchar_t fbuf[512]{};
        const DWORD req = fn_final(h1, nullptr, 0u, 0u /*VOLUME_NAME_DOS*/);
        OS_REQUIRE(req > 10u);
        const DWORD got_dos = fn_final(h1, fbuf, 512u, 0u /*VOLUME_NAME_DOS*/);
        OS_REQUIRE(got_dos > 0u);
        const std::wstring dos_s(fbuf, got_dos);
        OS_REQUIRE(dos_s == L"\\\\?\\G:\\Data\\readonly.cfg");
        OS_REQUIRE(dos_s.find(L"overlay") == std::wstring::npos);

        const DWORD got_none = fn_final(h1, fbuf, 512u, 0x4u /*VOLUME_NAME_NONE*/);
        OS_REQUIRE(std::wstring(fbuf, got_none) == L"\\Data\\readonly.cfg");

        const DWORD got_nt = fn_final(h1, fbuf, 512u, 0x2u /*VOLUME_NAME_NT*/);
        OS_REQUIRE(std::wstring(fbuf, got_nt) == L"\\Device\\OpenScarlettVfs\\G\\Data\\readonly.cfg");

        OS_REQUIRE(fn_close(h1) != 0);
    }
    // Original base file MUST remain 100% untouched & read-only!
    OS_REQUIRE(ReadFileAll(f_ro1) == kOrig1);
    OS_REQUIRE((::GetFileAttributesW(f_ro1.c_str()) & FILE_ATTRIBUTE_READONLY) != 0u);
    OS_REQUIRE(ReadFileAll(g_ov / "Data" / "readonly.cfg") == "TRUNCATED_IN_OVERLAY");

    // 1a-ext. Direct host handle opened under g_base (NOT via VFS) MUST keep its real host path
    //         in GetFinalPathNameByHandleW (never silently rewritten to \\?\G:\...), whereas an
    //         untracked handle opened inside g_ov MUST still normalize to \\?\G:\... without
    //         leaking the internal overlay prefix.
    {
        HANDLE h_direct_base = ::CreateFileW(
            f_ro1.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        OS_REQUIRE(h_direct_base != INVALID_HANDLE_VALUE);
        if (h_direct_base != INVALID_HANDLE_VALUE) {
            wchar_t fbuf[512]{};
            const DWORD got = fn_final(h_direct_base, fbuf, 512u, 0u /*VOLUME_NAME_DOS*/);
            OS_REQUIRE(got > 0u);
            const std::wstring direct_s(fbuf, got);
            OS_REQUIRE(direct_s.find(L"\\\\?\\G:\\") == std::wstring::npos);
            OS_REQUIRE(direct_s.find(L"readonly.cfg") != std::wstring::npos);
            ::CloseHandle(h_direct_base);
        }

        const fs::path ov_file = g_ov / "Data" / "readonly.cfg";
        HANDLE h_direct_ov = ::CreateFileW(
            ov_file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        OS_REQUIRE(h_direct_ov != INVALID_HANDLE_VALUE);
        if (h_direct_ov != INVALID_HANDLE_VALUE) {
            wchar_t fbuf[512]{};
            const DWORD got = fn_final(h_direct_ov, fbuf, 512u, 0u /*VOLUME_NAME_DOS*/);
            OS_REQUIRE(got > 0u);
            const std::wstring ov_s(fbuf, got);
            OS_REQUIRE(ov_s == L"\\\\?\\G:\\Data\\readonly.cfg");
            ::CloseHandle(h_direct_ov);
        }
    }

    // 1b. NtCreateFile(FILE_OVERWRITE) on un-overlaid READONLY game file
    {
        std::wstring nt_path = L"\\??\\G:\\Data\\nt_ro.bin";
        OS_UNICODE_STRING ustr{
            static_cast<USHORT>(nt_path.size() * sizeof(wchar_t)),
            static_cast<USHORT>((nt_path.size() + 1u) * sizeof(wchar_t)),
            nt_path.data()};
        OS_OBJECT_ATTRIBUTES oa{};
        oa.Length = sizeof(oa);
        oa.ObjectName = &ustr;
        oa.Attributes = OS_OBJ_CASE_INSENSITIVE;
        OS_IO_STATUS_BLOCK iosb{};
        HANDLE h2 = nullptr;
        const OS_NTSTATUS st = fn_nt_create(
            &h2, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb,
            nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
            OS_FILE_OVERWRITE, OS_FILE_NON_DIRECTORY_FILE | OS_FILE_SYNCHRONOUS_IO_NONALERT,
            nullptr, 0u);
        OS_REQUIRE(st == OS_STATUS_SUCCESS && h2 != nullptr);
        if (h2) {
            LARGE_INTEGER sz{};
            OS_REQUIRE(::GetFileSizeEx(h2, &sz) != 0 && sz.QuadPart == 0);
            const char payload[] = "NT_OVERWRITE_OK";
            DWORD wr = 0u;
            OS_REQUIRE(::WriteFile(h2, payload, static_cast<DWORD>(sizeof(payload) - 1u), &wr, nullptr) != 0);

            // Test NtSetInformationFile(FileEndOfFileInformation + FileAllocationInformation) on overlay handle
            OS_FILE_END_OF_FILE_INFORMATION eof_info{};
            eof_info.EndOfFile.QuadPart = 2;  // shrink to "NT"
            OS_IO_STATUS_BLOCK set_iosb{};
            OS_REQUIRE(fn_nt_setinfo(h2, &set_iosb, &eof_info, sizeof(eof_info), 20UL) == OS_STATUS_SUCCESS);
            OS_FILE_ALLOCATION_INFORMATION alloc_info{};
            alloc_info.AllocationSize.QuadPart = 4096;
            OS_REQUIRE(fn_nt_setinfo(h2, &set_iosb, &alloc_info, sizeof(alloc_info), 19UL) == OS_STATUS_SUCCESS);
            OS_REQUIRE(fn_nt_close(h2) == OS_STATUS_SUCCESS);
        }
        OS_REQUIRE(ReadFileAll(f_ro2) == kOrig2);
        OS_REQUIRE((::GetFileAttributesW(f_ro2.c_str()) & FILE_ATTRIBUTE_READONLY) != 0u);
        OS_REQUIRE(ReadFileAll(g_ov / "Data" / "nt_ro.bin") == "NT");
    }

    // 1c. CreateFile2(TRUNCATE_EXISTING) on un-overlaid READONLY game file
    {
        OS_CREATEFILE2_EXTENDED_PARAMETERS ex{};
        ex.dwSize = sizeof(ex);
        ex.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        HANDLE h3 = fn_cf2(L"G:\\Data\\cf2_ro.bin", GENERIC_WRITE, FILE_SHARE_READ, TRUNCATE_EXISTING, &ex);
        OS_REQUIRE(h3 != INVALID_HANDLE_VALUE);
        if (h3 != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            OS_REQUIRE(::GetFileSizeEx(h3, &sz) != 0 && sz.QuadPart == 0);
            OS_REQUIRE(fn_close(h3) != 0);
        }
        OS_REQUIRE(ReadFileAll(f_ro3) == kOrig3);
        OS_REQUIRE((::GetFileAttributesW(f_ro3.c_str()) & FILE_ATTRIBUTE_READONLY) != 0u);
    }

    // 2a. NtSetInformationFile(FileDispositionInformation) whiteout deletion on base file
    {
        std::wstring del_path = L"\\??\\G:\\Dir\\to_delete.txt";
        OS_UNICODE_STRING ustr{
            static_cast<USHORT>(del_path.size() * sizeof(wchar_t)),
            static_cast<USHORT>((del_path.size() + 1u) * sizeof(wchar_t)),
            del_path.data()};
        OS_OBJECT_ATTRIBUTES oa{};
        oa.Length = sizeof(oa);
        oa.ObjectName = &ustr;
        oa.Attributes = OS_OBJ_CASE_INSENSITIVE;
        OS_IO_STATUS_BLOCK iosb{};
        HANDLE hdel = nullptr;
        OS_REQUIRE(fn_nt_open(&hdel, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              OS_FILE_NON_DIRECTORY_FILE | OS_FILE_SYNCHRONOUS_IO_NONALERT) == OS_STATUS_SUCCESS);
        if (hdel) {
            OS_FILE_DISPOSITION_INFORMATION disp{};
            disp.DeleteFile = TRUE;
            OS_REQUIRE(fn_nt_setinfo(hdel, &iosb, &disp, sizeof(disp), 13UL) == OS_STATUS_SUCCESS);
            OS_REQUIRE(fn_nt_close(hdel) == OS_STATUS_SUCCESS);
        }
        // Base file MUST still exist on disk, but opening G:\Dir\to_delete.txt returns NOT_FOUND!
        OS_REQUIRE(fs::exists(g_base / "Dir" / "to_delete.txt"));
        HANDLE hcheck = nullptr;
        OS_REQUIRE(fn_nt_open(&hcheck, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ,
                              OS_FILE_NON_DIRECTORY_FILE) == OS_STATUS_OBJECT_NAME_NOT_FOUND);
    }

    // 2b. NtSetInformationFile(FileRenameInformation) CoW rename on read-only base file
    {
        std::wstring ren_path = L"\\??\\G:\\Dir\\to_rename.txt";
        OS_UNICODE_STRING ustr{
            static_cast<USHORT>(ren_path.size() * sizeof(wchar_t)),
            static_cast<USHORT>((ren_path.size() + 1u) * sizeof(wchar_t)),
            ren_path.data()};
        OS_OBJECT_ATTRIBUTES oa{};
        oa.Length = sizeof(oa);
        oa.ObjectName = &ustr;
        oa.Attributes = OS_OBJ_CASE_INSENSITIVE;
        OS_IO_STATUS_BLOCK iosb{};
        HANDLE hren = nullptr;
        OS_REQUIRE(fn_nt_open(&hren, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              OS_FILE_NON_DIRECTORY_FILE | OS_FILE_SYNCHRONOUS_IO_NONALERT) == OS_STATUS_SUCCESS);
        if (hren) {
            const std::wstring dst = L"G:\\Dir\\renamed_in_overlay.txt";
            const size_t hdr = offsetof(OS_FILE_RENAME_INFORMATION, FileName);
            const size_t nb = dst.size() * sizeof(wchar_t);
            std::vector<uint8_t> rbuf(hdr + nb, 0u);
            auto* ri = reinterpret_cast<OS_FILE_RENAME_INFORMATION*>(rbuf.data());
            ri->ReplaceIfExists = TRUE;
            ri->RootDirectory = nullptr;
            ri->FileNameLength = static_cast<ULONG>(nb);
            std::memcpy(ri->FileName, dst.data(), nb);
            OS_REQUIRE(fn_nt_setinfo(hren, &iosb, rbuf.data(), static_cast<ULONG>(rbuf.size()), 10UL) == OS_STATUS_SUCCESS);
            OS_REQUIRE(fn_nt_close(hren) == OS_STATUS_SUCCESS);
        }
        // Base file untouched, old virtual path whiteouted, new virtual path in overlay!
        OS_REQUIRE(fs::exists(g_base / "Dir" / "to_rename.txt"));
        OS_REQUIRE(ReadFileAll(g_ov / "Dir" / "renamed_in_overlay.txt") == "rename-payload");
    }

    // 2c. NtQueryDirectoryFile & NtQueryDirectoryFileEx merged overlay+base dedup + whiteout filtering
    {
        std::wstring dir_path = L"\\??\\G:\\Dir";
        OS_UNICODE_STRING ustr{
            static_cast<USHORT>(dir_path.size() * sizeof(wchar_t)),
            static_cast<USHORT>((dir_path.size() + 1u) * sizeof(wchar_t)),
            dir_path.data()};
        OS_OBJECT_ATTRIBUTES oa{};
        oa.Length = sizeof(oa);
        oa.ObjectName = &ustr;
        oa.Attributes = OS_OBJ_CASE_INSENSITIVE;
        OS_IO_STATUS_BLOCK iosb{};
        HANDLE hdir = nullptr;
        OS_REQUIRE(fn_nt_create(&hdir, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &iosb,
                                nullptr, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                OS_FILE_OPEN, OS_FILE_DIRECTORY_FILE | OS_FILE_SYNCHRONOUS_IO_NONALERT,
                                nullptr, 0u) == OS_STATUS_SUCCESS);
        OS_REQUIRE(hdir != nullptr);
        if (hdir) {
            std::vector<uint8_t> buf(4096u, 0u);
            std::vector<std::wstring> names;
            int64_t shared_size = -1;
            BOOLEAN restart = TRUE;
            for (;;) {
                OS_IO_STATUS_BLOCK qiosb{};
                const OS_NTSTATUS qst = fn_nt_qdir(
                    hdir, nullptr, nullptr, nullptr, &qiosb,
                    buf.data(), static_cast<ULONG>(buf.size()),
                    3UL /*FileBothDirectoryInformation*/, FALSE, nullptr, restart);
                restart = FALSE;
                if (qst == OS_STATUS_NO_MORE_FILES) break;
                OS_REQUIRE(qst == OS_STATUS_SUCCESS && qiosb.Information > 0u);
                size_t off = 0u;
                for (;;) {
                    auto* info = reinterpret_cast<const OS_FILE_BOTH_DIR_INFORMATION*>(buf.data() + off);
                    std::wstring nm(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    names.push_back(nm);
                    if (nm == L"shared.txt") {
                        shared_size = info->EndOfFile.QuadPart;
                    }
                    if (info->NextEntryOffset == 0u) break;
                    off += info->NextEntryOffset;
                }
            }
            std::set<std::wstring> uniq(names.begin(), names.end());
            OS_REQUIRE(uniq.size() == names.size());  // strict zero duplicates!
            OS_REQUIRE(uniq.count(L"only_base.txt") == 1u);
            OS_REQUIRE(uniq.count(L"only_overlay.txt") == 1u);
            OS_REQUIRE(uniq.count(L"shared.txt") == 1u);
            OS_REQUIRE(uniq.count(L"renamed_in_overlay.txt") == 1u);
            OS_REQUIRE(uniq.count(L"to_delete.txt") == 0u);
            OS_REQUIRE(uniq.count(L"to_rename.txt") == 0u);
            OS_REQUIRE(shared_size == 42);  // overlay shadowed base!
            for (const auto& nm : names) {
                OS_REQUIRE(nm.find(L".whiteout") == std::wstring::npos);
            }

            // NtQueryDirectoryFileEx with wildcard *.txt + single entry + SL_NO_CURSOR_UPDATE
            std::wstring pat = L"*.txt";
            OS_UNICODE_STRING pat_u{
                static_cast<USHORT>(pat.size() * sizeof(wchar_t)),
                static_cast<USHORT>((pat.size() + 1u) * sizeof(wchar_t)),
                pat.data()};
            OS_IO_STATUS_BLOCK ex_iosb{};
            OS_REQUIRE(fn_nt_qdirex(
                hdir, nullptr, nullptr, nullptr, &ex_iosb,
                buf.data(), static_cast<ULONG>(buf.size()),
                38UL /*FileIdFullDirectoryInformation*/,
                OS_SL_RESTART_SCAN | OS_SL_RETURN_SINGLE_ENTRY | OS_SL_NO_CURSOR_UPDATE,
                &pat_u) == OS_STATUS_SUCCESS);
            auto* first_ex = reinterpret_cast<const OS_FILE_ID_FULL_DIR_INFORMATION*>(buf.data());
            const std::wstring first_nm(first_ex->FileName, first_ex->FileNameLength / sizeof(wchar_t));
            OS_REQUIRE(first_ex->FileId.QuadPart != 0);

            // Second call without restart should see the SAME entry because SL_NO_CURSOR_UPDATE was set!
            OS_REQUIRE(fn_nt_qdirex(
                hdir, nullptr, nullptr, nullptr, &ex_iosb,
                buf.data(), static_cast<ULONG>(buf.size()),
                38UL, OS_SL_RETURN_SINGLE_ENTRY, nullptr) == OS_STATUS_SUCCESS);
            auto* second_ex = reinterpret_cast<const OS_FILE_ID_FULL_DIR_INFORMATION*>(buf.data());
            const std::wstring second_nm(second_ex->FileName, second_ex->FileNameLength / sizeof(wchar_t));
            OS_REQUIRE(first_nm == second_nm);

            OS_REQUIRE(fn_nt_close(hdir) == OS_STATUS_SUCCESS);
        }
    }

    // 2d. Verify ZERO C:\ leak in VFS default work/VFS root resolver
    {
        const std::wstring work_dir = openscarlett::ResolveDefaultWorkDirW(bin_dir.wstring());
        OS_REQUIRE(!work_dir.empty());
        // A build or bundle that itself lives on C:\ has no off-C:\ anchor, so the resolver
        // falls back to %TEMP%\openscarlett there. Only require "no C:\" when the binaries are off C:\.
        const std::wstring bin_w = bin_dir.wstring();
        const bool bin_on_c = bin_w.size() >= 2u && bin_w[1] == L':' && (bin_w[0] == L'C' || bin_w[0] == L'c');
        if (!bin_on_c && work_dir.size() >= 2u && work_dir[1] == L':') {
            OS_REQUIRE(work_dir[0] != L'C' && work_dir[0] != L'c');
        }
    }

    // Keep openscarlett_hook.dll loaded for process lifetime because it patched our process IAT.
    (void)hook;
    std::printf("[PASS] Test 1 & 2: VFS TRUNCATE_EXISTING + NT hooks + FinalPath + zero C: leak\n");
}

// ===========================================================================
// Test 3: XAsync No-Live-Op Safety & GDK Threading COM Table Slots 8, 16, 18
// ===========================================================================
std::atomic<int> g_canary_cb_hits{0};
void OPENSCARLETT_GDK_CALL CanaryAsyncCb(XAsyncBlock* /*async*/) {
    g_canary_cb_hits.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<int> g_live_cb_hits{0};
void OPENSCARLETT_GDK_CALL LiveAsyncCb(XAsyncBlock* /*async*/) {
    g_live_cb_hits.fetch_add(1, std::memory_order_relaxed);
}

XGameResult OPENSCARLETT_GDK_CALL TrivialAsyncWork(XAsyncBlock* /*async*/) {
    return XGR_S_OK;
}

struct AllocProviderState {
    uint64_t magic;
    uint32_t value;
};
std::atomic<bool> g_alloc_ctx_was_zeroed{false};
std::atomic<bool> g_alloc_cleanup_called{false};

XGameResult OPENSCARLETT_GDK_CALL TestAllocAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* st = static_cast<AllocProviderState*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            if (st && st->magic == 0ull && st->value == 0u) {
                g_alloc_ctx_was_zeroed.store(true, std::memory_order_release);
                st->magic = 0xCAFEBABE12345678ull;
                st->value = 777u;
            }
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (!data->buffer || data->bufferSize < sizeof(uint32_t)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, &st->value, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            g_alloc_cleanup_called.store(true, std::memory_order_release);
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

void OPENSCARLETT_GDK_CALL ManualSlotCallback(void* ctx, bool canceled) {
    if (!canceled && ctx) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
}

void TestXAsyncAndThreadingComTable() {
    // 3a. Zero-initialized XAsyncBlock with NO live operation: complete/cancel are safe no-ops
    g_canary_cb_hits.store(0);
    XAsyncBlock empty{};
    empty.callback = &CanaryAsyncCb;
    XAsyncComplete(&empty, XGR_S_OK, 16u);
    XAsyncCancel(&empty);
    OS_REQUIRE(g_canary_cb_hits.load() == 0);
    OS_REQUIRE(XAsyncGetStatus(&empty, false) == XGR_E_INVALIDARG);
    OS_REQUIRE(XAsyncGetStatus(&empty, true) == XGR_E_INVALIDARG);
    size_t sz = 999u;
    OS_REQUIRE(XAsyncGetResultSize(&empty, &sz) == XGR_E_INVALIDARG && sz == 0u);
    OS_REQUIRE(XAsyncGetResult(&empty, nullptr, 0u, nullptr, nullptr) == XGR_E_INVALIDARG);

    // Null block safety
    XAsyncComplete(nullptr, XGR_S_OK, 0u);
    XAsyncCancel(nullptr);
    OS_REQUIRE(XAsyncGetStatus(nullptr, true) == XGR_E_INVALIDARG);

    // Already-completed XAsyncBlock: repeat Complete / Cancel must NOT re-fire callback!
    g_live_cb_hits.store(0);
    XAsyncBlock live{};
    live.callback = &LiveAsyncCb;
    OS_REQUIRE(XAsyncRun(&live, &TrivialAsyncWork) == XGR_S_OK);
    OS_REQUIRE(XAsyncGetStatus(&live, true) == XGR_S_OK);
    OS_REQUIRE(g_live_cb_hits.load() == 1);
    XAsyncComplete(&live, XGR_S_OK, 0u);
    XAsyncCancel(&live);
    OS_REQUIRE(g_live_cb_hits.load() == 1);

    // 3b. Real GDK Async/TaskQueue/Thread COM vtable slots 8, 16, 18
    const XgrGuid kClsidAsync = {
        0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};
    void* obj = nullptr;
    OS_REQUIRE(QueryApiImpl(&kClsidAsync, reinterpret_cast<void**>(const_cast<XgrGuid*>(&kClsidAsync)), &obj) == XGR_S_OK);
    OS_REQUIRE(obj != nullptr);
    auto** vtbl = *reinterpret_cast<void***>(obj);
    for (int slot = 3; slot <= 30; ++slot) {
        OS_REQUIRE(vtbl[slot] != nullptr);
    }

    // Slot 8: XAsyncBeginAlloc via COM vtable
    using PfnComBeginAlloc = XGameResult(OPENSCARLETT_GDK_CALL*)(
        void*, XAsyncBlock*, const void*, const char*, XAsyncProvider, size_t);
    auto fn_slot8 = reinterpret_cast<PfnComBeginAlloc>(vtbl[8]);
    g_alloc_ctx_was_zeroed.store(false);
    g_alloc_cleanup_called.store(false);
    XAsyncBlock ab{};
    const char kTag[] = "Slot8AllocTest";
    OS_REQUIRE(fn_slot8(obj, &ab, kTag, kTag, &TestAllocAsyncProvider, sizeof(AllocProviderState)) == XGR_S_OK);
    OS_REQUIRE(g_alloc_ctx_was_zeroed.load());
    OS_REQUIRE(XAsyncSchedule(&ab, 0u) == XGR_S_OK);
    OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);
    uint32_t out_val = 0u;
    size_t used = 0u;
    OS_REQUIRE(XAsyncGetResult(&ab, kTag, sizeof(out_val), &out_val, &used) == XGR_S_OK);
    OS_REQUIRE(out_val == 777u && used == sizeof(uint32_t));
    OS_REQUIRE(g_alloc_cleanup_called.load());

    // Slots 18 (XTaskQueueSubmitCallback) & 16 (XTaskQueueDispatch) via COM vtable
    using PfnComSubmit = XGameResult(OPENSCARLETT_GDK_CALL*)(
        void*, XTaskQueueHandle, XTaskQueuePort, void*, XTaskQueueCallback);
    using PfnComDispatch = bool(OPENSCARLETT_GDK_CALL*)(
        void*, XTaskQueueHandle, XTaskQueuePort, uint32_t);
    auto fn_slot16 = reinterpret_cast<PfnComDispatch>(vtbl[16]);
    auto fn_slot18 = reinterpret_cast<PfnComSubmit>(vtbl[18]);

    XTaskQueueHandle mq = nullptr;
    OS_REQUIRE(XTaskQueueCreate(XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &mq) == XGR_S_OK);
    std::atomic<int> manual_hits{0};
    OS_REQUIRE(fn_slot18(obj, mq, XTaskQueuePort_Work, &manual_hits, &ManualSlotCallback) == XGR_S_OK);
    OS_REQUIRE(manual_hits.load() == 0);
    OS_REQUIRE(fn_slot16(obj, mq, XTaskQueuePort_Work, 0u) == true);
    OS_REQUIRE(manual_hits.load() == 1);
    XTaskQueueCloseHandle(mq);

    std::printf("[PASS] Test 3: XAsync no-live-op no-op & GDK threading COM slots 8/16/18\n");
}

// ===========================================================================
// Test 4: XGameSaveCreateContainer Name Rule Validation
// ===========================================================================
void TestGameSaveContainerNameValidation(const fs::path& work_root) {
    const fs::path save_root = work_root / "gdk_saves";
    ::SetEnvironmentVariableW(L"OPENSCARLETT_SAVE_ROOT", save_root.c_str());

    XGameSaveProviderHandle prov = nullptr;
    OS_REQUIRE(XGameSaveInitializeProvider(nullptr, "00000000-0000-0000-0000-000000001234", false, &prov) == XGR_S_OK);
    OS_REQUIRE(prov != nullptr);

    // Valid container names
    const std::string max256(256u, 'a');
    const char* valid_names[] = {
        "slot1",
        "Save_Slot-01.dat",
        "Profile/SubFolder/slot_2.bin",
        "A",
        max256.c_str(),
    };
    for (const char* vn : valid_names) {
        XGameSaveContainerHandle ch = nullptr;
        OS_REQUIRE(XGameSaveCreateContainer(prov, vn, &ch) == XGR_S_OK && ch != nullptr);
        XGameSaveCloseContainer(ch);
    }

    // Invalid container names MUST return XGR_E_GS_INVALID_CONTAINER_NAME (0x80830001)
    const std::string over257(257u, 'b');
    const char* invalid_names[] = {
        nullptr,
        "",
        over257.c_str(),
        "/leading_slash",
        "trailing_slash/",
        "double//slash",
        "..",
        "../escape",
        "sub/../escape",
        ".",
        "./rel",
        "sub/./rel",
        "has space",
        "back\\slash",
        "colon:name",
        "tab\tname",
        "bad?char",
    };
    for (const char* inv : invalid_names) {
        XGameSaveContainerHandle ch = reinterpret_cast<XGameSaveContainerHandle>(0x1);
        const XGameResult hr = XGameSaveCreateContainer(prov, inv, &ch);
        OS_REQUIRE(hr == XGR_E_GS_INVALID_CONTAINER_NAME);
        OS_REQUIRE(ch == nullptr);
    }

    XGameSaveCloseProvider(prov);
    std::printf("[PASS] Test 4: XGameSaveCreateContainer official name rules (0x80830001)\n");
}

// ===========================================================================
// Test 5: Complete Offline XDisplay/XUserDevice/XGameEvent/XGameUi/XGameStreaming
// ===========================================================================
std::atomic<int> g_dev_assoc_hits{0};
uint64_t g_dev_assoc_new_user = 0ull;
void OPENSCARLETT_GDK_CALL OnDeviceAssocChanged(
    void* /*ctx*/,
    const XUserDeviceAssociationChange* change) {
    if (change) {
        g_dev_assoc_new_user = change->newUser.value;
    }
    g_dev_assoc_hits.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<int> g_stream_conn_hits{0};
XGameStreamingConnectionState g_stream_last_state = XGameStreamingConnectionState_Disconnected;
void OPENSCARLETT_GDK_CALL OnStreamConnChanged(
    void* /*ctx*/,
    XGameStreamingClientId /*client*/,
    XGameStreamingConnectionState state) {
    g_stream_last_state = state;
    g_stream_conn_hits.fetch_add(1, std::memory_order_relaxed);
}

void TestOfflineGdkSubsystemsParity() {
    // 5a. XDisplay COM + C
    {
        const XgrGuid clsid = {0x03F0FE74u, 0xFDD9u, 0x4E5Cu, {0xB6, 0x30, 0xF9, 0x33, 0x9C, 0x47, 0xAC, 0xC5}};
        const XgrGuid iid = {0x35F07670u, 0x706Eu, 0x4BFBu, {0x94, 0x76, 0x09, 0x07, 0x98, 0xC5, 0xEB, 0xF3}};
        void* disp_obj = nullptr;
        OS_REQUIRE(QueryApiImpl(&clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&iid)), &disp_obj) == XGR_S_OK);
        OS_REQUIRE(disp_obj != nullptr);
        void* disp_self = nullptr;
        OS_REQUIRE(QueryApiImpl(&clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&clsid)), &disp_self) == XGR_S_OK);
        OS_REQUIRE(disp_self == disp_obj);
        auto** vtbl = *reinterpret_cast<void***>(disp_obj);
        using PfnGetHdr = XDisplayHdrModeResult(OPENSCARLETT_GDK_CALL*)(void*, XDisplayHdrModeInfo*);
        XDisplayHdrModeInfo info{};
        (void)reinterpret_cast<PfnGetHdr>(vtbl[3])(disp_obj, &info);
        OS_REQUIRE(info.maxToneMapLuminance > 0.0f);

        XDisplayTimeoutDeferralHandle def = nullptr;
        OS_REQUIRE(XDisplayAcquireTimeoutDeferral(&def) == XGR_S_OK && def != nullptr);
        XDisplayCloseTimeoutDeferralHandle(def);
    }

    // 5b. XUserDevice COM + C (FindForDevice / FindUserByDevice / AssociationChanged / AudioEndpoint)
    {
        const XgrGuid clsid = {0x7D824997u, 0x10DCu, 0x45ABu, {0x86, 0xB7, 0x27, 0x37, 0x76, 0x7C, 0x0B, 0xF1}};
        const XgrGuid iid = {0x0CC6A956u, 0xE7E1u, 0x4FDFu, {0x93, 0x41, 0x9D, 0x5D, 0xA6, 0x49, 0xEB, 0xC8}};
        void* dev_obj = nullptr;
        OS_REQUIRE(QueryApiImpl(&clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&iid)), &dev_obj) == XGR_S_OK);
        OS_REQUIRE(dev_obj != nullptr);

        XUserAppLocalDeviceId dev_id{};
        dev_id.value[0] = 0x42u;
        dev_id.value[31] = 0x99u;
        XUserHandle u1 = nullptr;
        OS_REQUIRE(XUserFindUserByDevice(&dev_id, &u1) == XGR_S_OK && u1 != nullptr);
        XUserLocalId lid{};
        OS_REQUIRE(XUserGetLocalId(u1, &lid) == XGR_S_OK);

        g_dev_assoc_hits.store(0);
        XTaskQueueRegistrationToken tok{};
        OS_REQUIRE(XUserRegisterForDeviceAssociationChanged(nullptr, nullptr, &OnDeviceAssocChanged, &tok) == XGR_S_OK);
        OS_REQUIRE(OpenScarlettGdkAssociateDeviceWithUser(&dev_id, lid) == XGR_S_OK);
        OS_REQUIRE(g_dev_assoc_hits.load() == 1 && g_dev_assoc_new_user == lid.value);
        OS_REQUIRE(XUserUnregisterForDeviceAssociationChanged(tok, true));

        wchar_t ep[128]{};
        size_t ep_used = 0u;
        OS_REQUIRE(XUserGetDefaultAudioEndpointUtf16(
            lid, XUserDefaultAudioEndpointKind_CommunicationRender, 128u, ep, &ep_used) == XGR_S_OK);
        OS_REQUIRE(ep_used > 10u);
        OS_REQUIRE(XUserGetDefaultAudioEndpointUtf16(
            lid, static_cast<XUserDefaultAudioEndpointKind>(99u), 128u, ep, &ep_used) == XGR_E_INVALIDARG);
        XUserCloseHandle(u1);
    }

    // 5c. XGameEvent COM + stateful ring counters
    {
        const XgrGuid clsid = {0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}};
        void* ev_obj = nullptr;
        OS_REQUIRE(QueryApiImpl(&clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&clsid)), &ev_obj) == XGR_S_OK);
        OS_REQUIRE(ev_obj != nullptr);
        uint64_t before_cnt = 0u, before_bytes = 0u;
        OS_REQUIRE(OpenScarlettGdkGetGameEventStats(&before_cnt, &before_bytes, nullptr) == XGR_S_OK);

        XGameEventDescriptor desc{};
        desc.id = 42u;
        desc.version = 1u;
        desc.level = 4u;
        const char ev_payload[] = "Sprint16Event";
        XGameEventDataDescriptor dd{};
        dd.ptr = reinterpret_cast<uint64_t>(ev_payload);
        dd.size = static_cast<uint32_t>(sizeof(ev_payload) - 1u);
        auto** vtbl = *reinterpret_cast<void***>(ev_obj);
        using PfnEvWrite = uint32_t(OPENSCARLETT_GDK_CALL*)(
            void*, uint64_t, const XGameEventDescriptor*, uint32_t, XGameEventDataDescriptor*);
        OS_REQUIRE(reinterpret_cast<PfnEvWrite>(vtbl[3])(ev_obj, 1ull, &desc, 1u, &dd) == 0u);
        OS_REQUIRE(XGameEventWrite(1ull, &desc, 1u, nullptr) == 87u);

        uint64_t after_cnt = 0u, after_bytes = 0u;
        uint32_t buffered = 0u;
        OS_REQUIRE(OpenScarlettGdkGetGameEventStats(&after_cnt, &after_bytes, &buffered) == XGR_S_OK);
        OS_REQUIRE(after_cnt == before_cnt + 1u);
        OS_REQUIRE(after_bytes == before_bytes + dd.size);
        OS_REQUIRE(buffered >= 1u);
    }

    // 5d. XGameUi COM (slots 20..23 TextEntry) + offline async APIs
    {
        const XgrGuid clsid = {0xDFCD4649u, 0x4FF8u, 0x4043u, {0xBA, 0x07, 0x35, 0xD6, 0x07, 0xDF, 0x98, 0xB0}};
        const XgrGuid iid = {0x72C4E70Fu, 0x4A67u, 0x481Bu, {0xBB, 0x84, 0xAA, 0x72, 0xED, 0xA1, 0x4F, 0xE5}};
        void* ui_obj = nullptr;
        OS_REQUIRE(QueryApiImpl(&clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&iid)), &ui_obj) == XGR_S_OK);
        OS_REQUIRE(ui_obj != nullptr);
        auto** vtbl = *reinterpret_cast<void***>(ui_obj);
        for (int s = 3; s <= 27; ++s) {
            OS_REQUIRE(vtbl[s] != nullptr);
        }

        XGameUiTextEntryOptions opts{
            XGameUiTextEntryInputScope_Default, XGameUiTextEntryPositionHint_Bottom, 0u};
        XGameUiTextEntryHandle teh = nullptr;
        OS_REQUIRE(XGameUiTextEntryOpen(&opts, 64u, "HelloGDK", 8u, &teh) == XGR_S_OK && teh != nullptr);
        OS_REQUIRE(XSystemIsHandleValid(teh));
        XGameUiTextEntryChangeTypeFlags chg = XGameUiTextEntryChangeTypeFlags_None;
        uint32_t cur = 0u;
        char tbuf[64]{};
        OS_REQUIRE(XGameUiTextEntryGetState(teh, &chg, &cur, nullptr, nullptr, 64u, tbuf) == XGR_S_OK);
        OS_REQUIRE(std::string(tbuf) == "HelloGDK" && cur == 8u);
        OS_REQUIRE(XGameUiTextEntryUpdatePositionHint(teh, XGameUiTextEntryPositionHint_Top) == XGR_S_OK);
        XGameUiTextEntryClose(teh);
        OS_REQUIRE(!XSystemIsHandleValid(teh));
    }

    // 5e. XGameStreaming COM (slots 3..17 zero gaps) + offline default + simulated toggle
    {
        const XgrGuid clsid = {0x0A2192AAu, 0xB2D5u, 0x4D58u, {0x83, 0xBE, 0x38, 0x3B, 0x6D, 0x80, 0x79, 0x9E}};
        const XgrGuid iid = {0x5F5E5169u, 0x746Cu, 0x4001u, {0xAD, 0x1C, 0xDA, 0x72, 0x8D, 0x01, 0xC9, 0xEB}};
        void* st_obj = nullptr;
        OS_REQUIRE(QueryApiImpl(&clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&iid)), &st_obj) == XGR_S_OK);
        OS_REQUIRE(st_obj != nullptr);
        auto** vtbl = *reinterpret_cast<void***>(st_obj);
        for (int s = 3; s <= 17; ++s) {
            OS_REQUIRE(vtbl[s] != nullptr);
        }

        OS_REQUIRE(XGameStreamingInitialize() == XGR_S_OK);
        OS_REQUIRE(!XGameStreamingIsStreaming());
        OS_REQUIRE(XGameStreamingGetClientCount() == 0u);

        g_stream_conn_hits.store(0);
        XTaskQueueRegistrationToken stok{};
        OS_REQUIRE(XGameStreamingRegisterConnectionStateChanged(
            nullptr, nullptr, &OnStreamConnChanged, &stok) == XGR_S_OK);

        const XGameStreamingClientId kClient = 0xAABBCCDD00112233ull;
        OS_REQUIRE(OpenScarlettGdkSetSimulatedStreamingState(1, kClient, 2560u, 1440u, 18u, 1) == XGR_S_OK);
        OS_REQUIRE(g_stream_conn_hits.load() == 1);
        OS_REQUIRE(g_stream_last_state == XGameStreamingConnectionState_Connected);
        OS_REQUIRE(XGameStreamingIsStreaming());
        OS_REQUIRE(XGameStreamingGetClientCount() == 1u);

        XGameStreamingClientId got_clients[2]{};
        uint32_t used_clients = 0u;
        OS_REQUIRE(XGameStreamingGetClients(2u, got_clients, &used_clients) == XGR_S_OK);
        OS_REQUIRE(used_clients == 1u && got_clients[0] == kClient);
        OS_REQUIRE(XGameStreamingGetConnectionState(kClient) == XGameStreamingConnectionState_Connected);

        XGameStreamingDisplayDetails dd{};
        OS_REQUIRE(XGameStreamingGetDisplayDetails(kClient, 0u, 16.0f / 9.0f, 9.0f / 16.0f, &dd) == XGR_S_OK);
        OS_REQUIRE(dd.preferredWidth == 2560u && dd.preferredHeight == 1440u);

        uint32_t in_lat = 0u, out_lat = 0u, sd_lat = 0u;
        OS_REQUIRE(XGameStreamingGetStreamLatency(kClient, &in_lat, &out_lat, &sd_lat) == XGR_S_OK);
        OS_REQUIRE(in_lat == 18000u);

        bool touch_en = false;
        OS_REQUIRE(XGameStreamingIsTouchInputEnabledOnClient(kClient, &touch_en) == XGR_S_OK && touch_en);
        OS_REQUIRE(XGameStreamingGetTouchBundleVersionNameSize(kClient) >= 5u);
        char ver_buf[32]{};
        OS_REQUIRE(XGameStreamingGetTouchBundleVersion(kClient, sizeof(ver_buf), ver_buf) == XGR_S_OK);
        OS_REQUIRE(std::string(ver_buf) == "1.0.0");

        uint64_t frame1 = 0u;
        OS_REQUIRE(XGameStreamingGetLastFrameDisplayed(kClient, &frame1) == XGR_S_OK && frame1 >= 1u);

        // Restore offline default
        OS_REQUIRE(OpenScarlettGdkSetSimulatedStreamingState(0, kClient, 0u, 0u, 0u, 0) == XGR_S_OK);
        OS_REQUIRE(!XGameStreamingIsStreaming());
        OS_REQUIRE(g_stream_last_state == XGameStreamingConnectionState_Disconnected);
        OS_REQUIRE(XGameStreamingUnregisterConnectionStateChanged(stok, true));
        XGameStreamingUninitialize();
    }

    std::printf("[PASS] Test 5: Offline XDisplay/XUserDevice/XGameEvent/XGameUi/XGameStreaming parity\n");
}

// ===========================================================================
// Test 6: Optional Non-Blocking Native Win32 Host System UI Mode
// ===========================================================================
void TestNativeWin32SystemUiMode() {
    ::SetEnvironmentVariableW(L"OPENSCARLETT_HEADLESS", nullptr);
    OS_REQUIRE(OpenScarlettGdkSetNativeSystemUiMode(0, 0u) == XGR_S_OK);

    uint32_t opened0 = 0u, comp0 = 0u, auto0 = 0u, canc0 = 0u;
    OS_REQUIRE(OpenScarlettGdkGetNativeSystemUiStats(&opened0, &comp0, &auto0, &canc0) == XGR_S_OK);

    // 6a. Default headless mode must NOT open native Win32 dialogs
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
            &ab, "Headless", "Body", "OK", "Cancel", nullptr,
            XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_Second) == XGR_S_OK);
        XGameUiMessageDialogButton btn = XGameUiMessageDialogButton_Third;
        OS_REQUIRE(XGameUiShowMessageDialogResult(&ab, &btn) == XGR_S_OK);
        OS_REQUIRE(btn == XGameUiMessageDialogButton_First);

        uint32_t opened1 = 0u;
        OS_REQUIRE(OpenScarlettGdkGetNativeSystemUiStats(&opened1, nullptr, nullptr, nullptr) == XGR_S_OK);
        OS_REQUIRE(opened1 == opened0);
    }

    // 6b. Opt-in native Win32 dialog mode with 40ms auto-dismiss (Message + TextEntry + PlayerPicker)
    OS_REQUIRE(OpenScarlettGdkSetNativeSystemUiMode(1, 40u) == XGR_S_OK);
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
            &ab, "Native Title", "Native Body", "Confirm", "Deny", nullptr,
            XGameUiMessageDialogButton_Second, XGameUiMessageDialogButton_Second) == XGR_S_OK);
        XGameUiMessageDialogButton btn = XGameUiMessageDialogButton_First;
        OS_REQUIRE(XGameUiShowMessageDialogResult(&ab, &btn) == XGR_S_OK);
        OS_REQUIRE(btn == XGameUiMessageDialogButton_Second);
    }
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUiShowTextEntryAsync(
            &ab, "Enter Tag", "Player Tag", "ScarlettHero",
            XGameUiTextEntryInputScope_Default, 32u) == XGR_S_OK);
        uint32_t tsz = 0u;
        OS_REQUIRE(XGameUiShowTextEntryResultSize(&ab, &tsz) == XGR_S_OK && tsz > 0u);
        std::vector<char> tbuf(tsz, '\0');
        uint32_t tused = 0u;
        OS_REQUIRE(XGameUiShowTextEntryResult(&ab, tsz, tbuf.data(), &tused) == XGR_S_OK);
        OS_REQUIRE(std::string(tbuf.data()) == "ScarlettHero");
    }
    {
        const uint64_t candidates[2] = {0x1001ull, 0x2002ull};
        const uint64_t pre[1] = {0x2002ull};
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUiShowPlayerPickerAsync(
            &ab, nullptr, "Pick Squad", 2u, candidates, 1u, pre, 1u, 2u) == XGR_S_OK);
        uint32_t cnt = 0u;
        OS_REQUIRE(XGameUiShowPlayerPickerResultCount(&ab, &cnt) == XGR_S_OK && cnt == 1u);
        uint64_t out_p[2]{};
        uint32_t used_p = 0u;
        OS_REQUIRE(XGameUiShowPlayerPickerResult(&ab, 2u, out_p, &used_p) == XGR_S_OK);
        OS_REQUIRE(used_p == 1u && out_p[0] == 0x2002ull);
    }

    // 6c. Live cancellation of an open native Win32 dialog
    OS_REQUIRE(OpenScarlettGdkSetNativeSystemUiMode(1, 5000u) == XGR_S_OK);
    {
        XAsyncBlock ab{};
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(XGameUiShowMessageDialogAsync(
            &ab, "Cancel Me", "Waiting", "OK", nullptr, nullptr,
            XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_First) == XGR_S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        XAsyncCancel(&ab);
        const XGameResult hr = XAsyncGetStatus(&ab, true);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        OS_REQUIRE(hr == XGR_E_ABORT);
        OS_REQUIRE(elapsed_ms < 2000);
    }

    uint32_t opened2 = 0u, comp2 = 0u, auto2 = 0u, canc2 = 0u;
    OS_REQUIRE(OpenScarlettGdkGetNativeSystemUiStats(&opened2, &comp2, &auto2, &canc2) == XGR_S_OK);
    OS_REQUIRE(opened2 >= opened0 + 4u);
    OS_REQUIRE(comp2 >= comp0 + 3u);
    OS_REQUIRE(auto2 >= auto0 + 3u);
    OS_REQUIRE(canc2 >= canc0 + 1u);

    // Restore safe headless default
    OS_REQUIRE(OpenScarlettGdkSetNativeSystemUiMode(0, 0u) == XGR_S_OK);
    std::printf("[PASS] Test 6: Opt-in non-blocking Native Win32 Host System UI dialogs\n");
}

// ===========================================================================
// Test 7: ALL 6 Retired Non-Media Stub DLLs (True Functional Implementations)
// ===========================================================================
std::atomic<int> g_hix_hang_cb_hits{0};
void __stdcall OnHixHangCb(const char* /*reason*/, void* ctx) {
    if (ctx) static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

std::atomic<int> g_hix_dump_cb_hits{0};
void __stdcall OnHixMinidumpCb(const char* /*reason*/, const char* /*path*/, void* ctx) {
    if (ctx) static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

struct GcLifecycleTrace {
    std::vector<uint32_t> transitions;
};
void __stdcall OnGcLifecycleCb(uint32_t /*prev*/, uint32_t next, void* ctx) {
    if (ctx) static_cast<GcLifecycleTrace*>(ctx)->transitions.push_back(next);
}

struct DbgMonTrace {
    std::atomic<int> hits{0};
    std::string last_channel;
    std::string last_text;
};
void __stdcall OnDbgMonCb(uint32_t /*sev*/, const char* ch, const char* txt, void* ctx) {
    if (ctx) {
        auto* t = static_cast<DbgMonTrace*>(ctx);
        t->last_channel = ch ? ch : "";
        t->last_text = txt ? txt : "";
        t->hits.fetch_add(1, std::memory_order_relaxed);
    }
}

void TestRetired6StubDlls(const fs::path& bin_dir, const fs::path& work_root) {
    // 7.1 hix_xs.dll
    {
        HMODULE m = ::LoadLibraryW((bin_dir / "hix_xs.dll").c_str());
        OS_REQUIRE(m != nullptr);
        auto fn_init = reinterpret_cast<int32_t(__stdcall*)(uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixInitialize")));
        auto fn_uninit = reinterpret_cast<void(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "HixUninitialize")));
        auto fn_active = reinterpret_cast<int32_t(__stdcall*)(int32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixIsSessionActive")));
        auto fn_reg_wd = reinterpret_cast<int32_t(__stdcall*)(const char*, uint32_t, uint64_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixRegisterWatchdog")));
        auto fn_hb_wd = reinterpret_cast<int32_t(__stdcall*)(uint64_t, const char*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixWatchdogHeartbeat")));
        auto fn_chk_wd = reinterpret_cast<int32_t(__stdcall*)(uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixCheckWatchdogs")));
        auto fn_unreg_wd = reinterpret_cast<int32_t(__stdcall*)(uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixUnregisterWatchdog")));
        auto fn_reg_cb = reinterpret_cast<int32_t(__stdcall*)(void*, void*, uint64_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixRegisterHangCallback")));
        auto fn_unreg_cb = reinterpret_cast<int32_t(__stdcall*)(uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixUnregisterHangCallback")));
        auto fn_reg_md = reinterpret_cast<int32_t(__stdcall*)(void*, void*, uint64_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixRegisterMinidumpCallback")));
        auto fn_unreg_md = reinterpret_cast<int32_t(__stdcall*)(uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixUnregisterMinidumpCallback")));
        auto fn_bc = reinterpret_cast<int32_t(__stdcall*)(uint32_t, const char*, uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixRecordBreadcrumb")));
        auto fn_log = reinterpret_cast<int32_t(__stdcall*)(const char*, const char*, uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixLogEvent")));
        auto fn_dump = reinterpret_cast<int32_t(__stdcall*)(const char*, const char*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixRequestHangDump")));
        auto fn_last = reinterpret_cast<int32_t(__stdcall*)(char*, uint32_t, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixGetLastHangReport")));
        auto fn_ring = reinterpret_cast<int32_t(__stdcall*)(uint64_t*, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "HixGetRingLogCount")));
        OS_REQUIRE(fn_init && fn_uninit && fn_active && fn_reg_wd && fn_hb_wd && fn_chk_wd &&
                   fn_unreg_wd && fn_reg_cb && fn_unreg_cb && fn_reg_md && fn_unreg_md &&
                   fn_bc && fn_log && fn_dump && fn_last && fn_ring);

        int32_t act = 0;
        OS_REQUIRE(fn_init(1u) == 0);
        OS_REQUIRE(fn_active(&act) == 0 && act == 1);
        uint64_t wd_id = 0u;
        OS_REQUIRE(fn_reg_wd("RenderThread", 15u, &wd_id) == 0 && wd_id != 0u);
        OS_REQUIRE(fn_hb_wd(wd_id, "Frame1") == 0);
        OS_REQUIRE(fn_bc(3u, "DrawStage", 0xABCDu) == 0);
        OS_REQUIRE(fn_log("gpu", "present_start", 100u) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        uint32_t hung = 0u;
        OS_REQUIRE(fn_chk_wd(&hung) == 0 && hung >= 1u);

        g_hix_hang_cb_hits.store(0);
        g_hix_dump_cb_hits.store(0);
        uint64_t cbtok = 0u, mdtok = 0u;
        OS_REQUIRE(fn_reg_cb(reinterpret_cast<void*>(&OnHixHangCb), &g_hix_hang_cb_hits, &cbtok) == 0);
        OS_REQUIRE(fn_reg_md(reinterpret_cast<void*>(&OnHixMinidumpCb), &g_hix_dump_cb_hits, &mdtok) == 0);
        const fs::path hix_json = work_root / "hix_hang_report.json";
        OS_REQUIRE(fn_dump("RenderWatchdogTimeout", hix_json.string().c_str()) == 0);
        OS_REQUIRE(g_hix_hang_cb_hits.load() == 1 && g_hix_dump_cb_hits.load() == 1);
        OS_REQUIRE(fn_unreg_cb(cbtok) == 0);
        OS_REQUIRE(fn_unreg_md(mdtok) == 0);
        OS_REQUIRE(fn_unreg_wd(wd_id) == 0);

        const std::string disk_json = ReadFileAll(hix_json);
        OS_REQUIRE(disk_json.find("RenderWatchdogTimeout") != std::string::npos);
        OS_REQUIRE(disk_json.find("DrawStage") != std::string::npos);
        OS_REQUIRE(disk_json.find("ring_events") != std::string::npos);

        uint32_t needed = 0u;
        OS_REQUIRE(fn_last(nullptr, 0u, &needed) == 0 && needed > 50u);
        std::vector<char> mem_json(needed, '\0');
        OS_REQUIRE(fn_last(mem_json.data(), needed, &needed) == 0);
        OS_REQUIRE(std::string(mem_json.data()).find("present_start") != std::string::npos);

        uint64_t total_ring = 0u;
        uint32_t buf_ring = 0u;
        OS_REQUIRE(fn_ring(&total_ring, &buf_ring) == 0 && total_ring >= 4u && buf_ring >= 4u);
        fn_uninit();
        OS_REQUIRE(fn_active(&act) == 0 && act == 0);
        ::FreeLibrary(m);
    }

    // 7.2 xbtp.dll (Real Windows IOCP transport + strict zero-leak handle/memory release!)
    {
        HMODULE m = ::LoadLibraryW((bin_dir / "xbtp.dll").c_str());
        OS_REQUIRE(m != nullptr);
        auto fn_init = reinterpret_cast<int32_t(__stdcall*)(uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpInitialize")));
        auto fn_sd = reinterpret_cast<void(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpShutdown")));
        auto fn_open = reinterpret_cast<int32_t(__stdcall*)(const char*, uint32_t, void**)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpOpenSession")));
        auto fn_close = reinterpret_cast<int32_t(__stdcall*)(void*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpCloseSession")));
        auto fn_wev = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t, const void*, uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpWriteEvent")));
        auto fn_rev = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t, uint32_t*, void*, uint32_t, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpReadEvent")));
        auto fn_och = reinterpret_cast<int32_t(__stdcall*)(void*, const char*, uint32_t, void**)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpOpenChannel")));
        auto fn_snd = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t, uint32_t, const void*, uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpSendPacket")));
        auto fn_rcv = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t, uint32_t, uint32_t*, void*, uint32_t, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpReceivePacket")));
        auto fn_cch = reinterpret_cast<int32_t(__stdcall*)(void*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpCloseChannel")));
        auto fn_stats = reinterpret_cast<int32_t(__stdcall*)(uint32_t*, uint32_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XbtpGetTransportStats")));
        OS_REQUIRE(fn_init && fn_sd && fn_open && fn_close && fn_wev && fn_rev &&
                   fn_och && fn_snd && fn_rcv && fn_cch && fn_stats);

        OS_REQUIRE(fn_init(0u) == 0);
        void* sess = nullptr;
        OS_REQUIRE(fn_open("IocpSession", 0u, &sess) == 0 && sess != nullptr);
        const char ev_msg[] = "IOCP_EVENT_DATA";
        OS_REQUIRE(fn_wev(sess, 77u, ev_msg, static_cast<uint32_t>(sizeof(ev_msg) - 1u)) == 0);
        uint32_t got_eid = 0u, got_bytes = 0u;
        char rbuf[64]{};
        OS_REQUIRE(fn_rev(sess, 250u, &got_eid, rbuf, sizeof(rbuf), &got_bytes) == 0);
        OS_REQUIRE(got_eid == 77u && std::string(rbuf, got_bytes) == "IOCP_EVENT_DATA");

        // Bidirectional named/numbered channel over IOCP
        void* ch = nullptr;
        OS_REQUIRE(fn_och(sess, "debug.channel", 5u, &ch) == 0 && ch != nullptr);
        const char c2h[] = "CLIENT_TO_HOST";
        const char h2c[] = "HOST_TO_CLIENT";
        OS_REQUIRE(fn_snd(ch, 0u, 10u, c2h, static_cast<uint32_t>(sizeof(c2h) - 1u)) == 0);
        OS_REQUIRE(fn_snd(ch, 1u, 20u, h2c, static_cast<uint32_t>(sizeof(h2c) - 1u)) == 0);

        uint32_t pt = 0u, pb = 0u;
        OS_REQUIRE(fn_rcv(ch, 0u, 250u, &pt, rbuf, sizeof(rbuf), &pb) == 0);
        OS_REQUIRE(pt == 10u && std::string(rbuf, pb) == "CLIENT_TO_HOST");
        OS_REQUIRE(fn_rcv(ch, 1u, 250u, &pt, rbuf, sizeof(rbuf), &pb) == 0);
        OS_REQUIRE(pt == 20u && std::string(rbuf, pb) == "HOST_TO_CLIENT");
        // Empty queue timed receive returns WAIT_TIMEOUT HRESULT (0x80070102)
        OS_REQUIRE(fn_rcv(ch, 0u, 10u, &pt, rbuf, sizeof(rbuf), &pb) == static_cast<int32_t>(0x80070102u));

        // Clean cancellation of blocked IOCP receiver on close!
        std::atomic<bool> waiter_running{false};
        std::atomic<int32_t> worker_hr{0};
        std::thread waiter([&]() {
            uint32_t t = 0u, b = 0u;
            char tmp[16]{};
            waiter_running.store(true, std::memory_order_release);
            worker_hr.store(fn_rcv(ch, 0u, 5000u, &t, tmp, sizeof(tmp), &b), std::memory_order_release);
        });
        while (!waiter_running.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        OS_REQUIRE(fn_cch(ch) == 0);
        waiter.join();
        OS_REQUIRE(worker_hr.load(std::memory_order_acquire) == static_cast<int32_t>(0x80004004u));
        OS_REQUIRE(fn_close(sess) == 0);

        // Strict ZERO-LEAK handle & unread packet drain stress test (32 sessions + 32 channels = 96 IOCPs)
        DWORD handles_before = 0u;
        OS_REQUIRE(::GetProcessHandleCount(::GetCurrentProcess(), &handles_before) != 0);
        for (int i = 0; i < 32; ++i) {
            void* s = nullptr;
            void* c = nullptr;
            OS_REQUIRE(fn_open("LeakCheck", 0u, &s) == 0 && s != nullptr);
            OS_REQUIRE(fn_och(s, "LeakCh", static_cast<uint32_t>(i), &c) == 0 && c != nullptr);
            const char unread[] = "UNREAD_PACKET_MUST_BE_DRAINED";
            OS_REQUIRE(fn_wev(s, 1u, unread, static_cast<uint32_t>(sizeof(unread))) == 0);
            OS_REQUIRE(fn_snd(c, 0u, 2u, unread, static_cast<uint32_t>(sizeof(unread))) == 0);
            OS_REQUIRE(fn_snd(c, 1u, 3u, unread, static_cast<uint32_t>(sizeof(unread))) == 0);
            // Closing session closes child channel AND drains every unread packet!
            OS_REQUIRE(fn_close(s) == 0);
        }
        uint32_t open_s = 99u, open_c = 99u;
        uint64_t active_iocp = 99u, sent = 0u, recvd = 0u, drained = 0u;
        OS_REQUIRE(fn_stats(&open_s, &open_c, &active_iocp, &sent, &recvd, &drained) == 0);
        OS_REQUIRE(open_s == 0u && open_c == 0u && active_iocp == 0u);
        OS_REQUIRE(drained >= 96u);

        DWORD handles_after = 0u;
        OS_REQUIRE(::GetProcessHandleCount(::GetCurrentProcess(), &handles_after) != 0);
        OS_REQUIRE(handles_after <= handles_before + 2u);  // Zero IOCP handle leaks!
        fn_sd();
        ::FreeLibrary(m);
    }

    // 7.3 xtfpix.dll
    {
        HMODULE m = ::LoadLibraryW((bin_dir / "xtfpix.dll").c_str());
        OS_REQUIRE(m != nullptr);
        auto fn_init = reinterpret_cast<int32_t(__stdcall*)(uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixInitialize")));
        auto fn_bc = reinterpret_cast<int32_t(__stdcall*)(const wchar_t*, uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixBeginCapture")));
        auto fn_ic = reinterpret_cast<int32_t(__stdcall*)(int32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixIsCapturing")));
        auto fn_be = reinterpret_cast<int32_t(__stdcall*)(uint64_t, const char*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixBeginEvent")));
        auto fn_mk = reinterpret_cast<int32_t(__stdcall*)(uint64_t, const char*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixSetMarker")));
        auto fn_ee = reinterpret_cast<int32_t(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixEndEvent")));
        auto fn_ec = reinterpret_cast<int32_t(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixEndCapture")));
        auto fn_sum = reinterpret_cast<int32_t(__stdcall*)(uint64_t*, uint64_t*, uint64_t*, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixGetCaptureSummary")));
        auto fn_last = reinterpret_cast<int32_t(__stdcall*)(wchar_t*, uint32_t, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixGetLastCapturePath")));
        auto fn_sd = reinterpret_cast<int32_t(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfPixShutdown")));
        OS_REQUIRE(fn_init && fn_bc && fn_ic && fn_be && fn_mk && fn_ee && fn_ec && fn_sum && fn_last && fn_sd);

        const fs::path cap_path = work_root / "pix_frame_capture.json";
        int32_t cap = 0;
        OS_REQUIRE(fn_init(0u) == 0);
        OS_REQUIRE(fn_bc(cap_path.c_str(), 0x3u) == 0);
        OS_REQUIRE(fn_ic(&cap) == 0 && cap == 1);
        OS_REQUIRE(fn_be(0xFF00FF00ull, "FramePass") == 0);
        OS_REQUIRE(fn_be(0xFF0000FFull, "ShadowMap") == 0);
        OS_REQUIRE(fn_mk(0xFFFF0000ull, "DrawMesh") == 0);
        OS_REQUIRE(fn_ee() == 0);
        OS_REQUIRE(fn_ee() == 0);
        OS_REQUIRE(fn_ec() == 0);
        OS_REQUIRE(fn_ic(&cap) == 0 && cap == 0);

        uint64_t caps = 0u, mks = 0u, evs = 0u;
        uint32_t depth = 99u;
        OS_REQUIRE(fn_sum(&caps, &mks, &evs, &depth) == 0 && caps >= 1u && mks >= 1u && evs >= 2u && depth == 0u);
        wchar_t lp[512]{};
        uint32_t lwr = 0u;
        OS_REQUIRE(fn_last(lp, 512u, &lwr) == 0 && std::wstring(lp) == cap_path.wstring());

        const std::string cap_json = ReadFileAll(cap_path);
        OS_REQUIRE(cap_json.find("FramePass") != std::string::npos);
        OS_REQUIRE(cap_json.find("ShadowMap") != std::string::npos);
        OS_REQUIRE(cap_json.find("DrawMesh") != std::string::npos);
        OS_REQUIRE(cap_json.find("\"depth\": 1") != std::string::npos);
        OS_REQUIRE(fn_sd() == 0);
        ::FreeLibrary(m);
    }

    // 7.4 gamecoreclient.dll
    {
        HMODULE m = ::LoadLibraryW((bin_dir / "gamecoreclient.dll").c_str());
        OS_REQUIRE(m != nullptr);
        auto fn_init = reinterpret_cast<int32_t(__stdcall*)(uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientInitialize")));
        auto fn_sd = reinterpret_cast<int32_t(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientShutdown")));
        auto fn_open = reinterpret_cast<int32_t(__stdcall*)(const char*, void**)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientOpenSession")));
        auto fn_close = reinterpret_cast<int32_t(__stdcall*)(void*)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientCloseSession")));
        auto fn_state = reinterpret_cast<int32_t(__stdcall*)(uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientGetServiceState")));
        auto fn_life = reinterpret_cast<int32_t(__stdcall*)(uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientNotifyLifecycleEvent")));
        auto fn_reg = reinterpret_cast<int32_t(__stdcall*)(void*, void*, uint64_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientRegisterLifecycleCallback")));
        auto fn_unreg = reinterpret_cast<int32_t(__stdcall*)(uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientUnregisterLifecycleCallback")));
        auto fn_setid = reinterpret_cast<int32_t(__stdcall*)(const char*, const char*, const char*)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientSetPackageIdentity")));
        auto fn_getid = reinterpret_cast<int32_t(__stdcall*)(char*, uint32_t, char*, uint32_t, char*, uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "GameCoreClientGetPackageIdentity")));
        auto fn_unload = reinterpret_cast<int32_t(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "DllCanUnloadNow")));
        OS_REQUIRE(fn_init && fn_sd && fn_open && fn_close && fn_state && fn_life &&
                   fn_reg && fn_unreg && fn_setid && fn_getid && fn_unload);

        OS_REQUIRE(fn_init(0u) == 0);
        OS_REQUIRE(fn_unload() == 1);  // S_FALSE while active
        OS_REQUIRE(fn_setid("App.Game", "Publisher.Game_1234", "XDKS.1") == 0);
        char aid[64]{}, pfn[64]{}, sbx[64]{};
        OS_REQUIRE(fn_getid(aid, 64u, pfn, 64u, sbx, 64u) == 0);
        OS_REQUIRE(std::string(aid) == "App.Game" && std::string(pfn) == "Publisher.Game_1234" && std::string(sbx) == "XDKS.1");

        GcLifecycleTrace trace{};
        uint64_t ltok = 0u;
        OS_REQUIRE(fn_reg(reinterpret_cast<void*>(&OnGcLifecycleCb), &trace, &ltok) == 0);
        void* sess = nullptr;
        OS_REQUIRE(fn_open("App.Game", &sess) == 0 && sess != nullptr);
        uint32_t plm = 0u;
        OS_REQUIRE(fn_life(2u) == 0);  // Constrained
        OS_REQUIRE(fn_state(&plm) == 0 && plm == 2u);
        OS_REQUIRE(fn_life(3u) == 0);  // Suspended
        OS_REQUIRE(fn_state(&plm) == 0 && plm == 3u);
        OS_REQUIRE(fn_life(4u) == 0);  // Resumed
        OS_REQUIRE(fn_state(&plm) == 0 && plm == 4u);
        OS_REQUIRE(trace.transitions.size() == 3u && trace.transitions[0] == 2u && trace.transitions[2] == 4u);

        OS_REQUIRE(fn_close(sess) == 0);
        OS_REQUIRE(fn_unreg(ltok) == 0);
        OS_REQUIRE(fn_sd() == 0);
        OS_REQUIRE(fn_unload() == 0);  // S_OK once fully shut down!
        ::FreeLibrary(m);
    }

    // 7.5 minuserx.dll
    {
        HMODULE m = ::LoadLibraryW((bin_dir / "minuserx.dll").c_str());
        OS_REQUIRE(m != nullptr);
        auto fn_init = reinterpret_cast<int32_t(__stdcall*)(uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXInitialize")));
        auto fn_scale = reinterpret_cast<int32_t(__stdcall*)(uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXGetDisplayScaleFactor")));
        auto fn_cw = reinterpret_cast<int32_t(__stdcall*)(const wchar_t*, int32_t, int32_t, uint32_t, void**)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXCreateWindow")));
        auto fn_dw = reinterpret_cast<int32_t(__stdcall*)(void*)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXDestroyWindow")));
        auto fn_post = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t, uintptr_t, intptr_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXPostMessage")));
        auto fn_peek = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t*, uintptr_t*, intptr_t*, uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXPeekMessage")));
        auto fn_rect = reinterpret_cast<int32_t(__stdcall*)(void*, int32_t*, int32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXGetClientRect")));
        auto fn_setcur = reinterpret_cast<int32_t(__stdcall*)(int32_t, int32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXSetCursorPos")));
        auto fn_getcur = reinterpret_cast<int32_t(__stdcall*)(int32_t*, int32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXGetCursorPos")));
        auto fn_sd = reinterpret_cast<int32_t(__stdcall*)()>(reinterpret_cast<void*>(::GetProcAddress(m, "MinUserXShutdown")));
        OS_REQUIRE(fn_init && fn_scale && fn_cw && fn_dw && fn_post && fn_peek && fn_rect && fn_setcur && fn_getcur && fn_sd);

        OS_REQUIRE(fn_init(0u) == 0);
        uint32_t scale = 0u;
        OS_REQUIRE(fn_scale(&scale) == 0 && scale >= 100u && scale <= 500u);
        void* hwnd = nullptr;
        OS_REQUIRE(fn_cw(L"MinUserXTest", 320, 240, 0u, &hwnd) == 0 && hwnd != nullptr);
        int32_t cw = 0, ch = 0;
        OS_REQUIRE(fn_rect(hwnd, &cw, &ch) == 0 && cw == 320 && ch == 240);
        OS_REQUIRE(fn_post(hwnd, WM_APP + 7u, 123u, 456) == 0);
        uint32_t got_msg = 0u;
        uintptr_t got_wp = 0u;
        intptr_t got_lp = 0;
        bool saw_custom = false;
        for (int tries = 0; tries < 16; ++tries) {
            if (fn_peek(hwnd, &got_msg, &got_wp, &got_lp, 1u) == 0) break;
            if (got_msg == WM_APP + 7u && got_wp == 123u && got_lp == 456) {
                saw_custom = true;
                break;
            }
        }
        OS_REQUIRE(saw_custom);
        OS_REQUIRE(fn_setcur(128, 256) == 0);
        int32_t cx = 0, cy = 0;
        OS_REQUIRE(fn_getcur(&cx, &cy) == 0 && cx == 128 && cy == 256);
        OS_REQUIRE(fn_dw(hwnd) == 0);
        OS_REQUIRE(fn_sd() == 0);
        ::FreeLibrary(m);
    }

    // 7.6 xtfdebugmonitor.dll
    {
        HMODULE m = ::LoadLibraryW((bin_dir / "xtfdebugmonitor.dll").c_str());
        OS_REQUIRE(m != nullptr);
        auto fn_cr = reinterpret_cast<int32_t(__stdcall*)(const wchar_t*, void**)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfCreateDebugMonitorClient")));
        auto fn_st = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorStart")));
        auto fn_sp = reinterpret_cast<int32_t(__stdcall*)(void*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorStop")));
        auto fn_cl = reinterpret_cast<int32_t(__stdcall*)(void*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorClose")));
        auto fn_at = reinterpret_cast<int32_t(__stdcall*)(int32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorIsAttached")));
        auto fn_wex = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t, const char*, const char*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorWriteOutputEx")));
        auto fn_rd = reinterpret_cast<int32_t(__stdcall*)(void*, uint32_t*, char*, uint32_t, char*, uint32_t, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorReadOutput")));
        auto fn_reg = reinterpret_cast<int32_t(__stdcall*)(void*, void*, void*, uint64_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorRegisterCallback")));
        auto fn_unreg = reinterpret_cast<int32_t(__stdcall*)(uint64_t)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorUnregisterCallback")));
        auto fn_stats = reinterpret_cast<int32_t(__stdcall*)(void*, uint64_t*, uint64_t*, uint32_t*)>(reinterpret_cast<void*>(::GetProcAddress(m, "XtfDebugMonitorGetStats")));
        OS_REQUIRE(fn_cr && fn_st && fn_sp && fn_cl && fn_at && fn_wex && fn_rd && fn_reg && fn_unreg && fn_stats);

        void* c1 = nullptr;
        void* c2 = nullptr;
        int32_t attached = -1;
        OS_REQUIRE(fn_cr(L"127.0.0.1", &c1) == 0 && c1 != nullptr);
        OS_REQUIRE(fn_cr(L"127.0.0.1", &c2) == 0 && c2 != nullptr);
        OS_REQUIRE(fn_at(&attached) == 0 && attached == 0);
        OS_REQUIRE(fn_st(c1, 0u) == 0);
        OS_REQUIRE(fn_st(c2, 0u) == 0);
        // No callback & flags=0 -> attached == 0 (preserves test_s7_pe_gdk_runtime)
        OS_REQUIRE(fn_at(&attached) == 0 && attached == 0);

        DbgMonTrace tr{};
        uint64_t cbtok = 0u;
        OS_REQUIRE(fn_reg(c2, reinterpret_cast<void*>(&OnDbgMonCb), &tr, &cbtok) == 0);
        OS_REQUIRE(fn_at(&attached) == 0 && attached == 1);

        // Multi-client fan-out write from c1 delivers to c2 callback AND c2 unread queue!
        OS_REQUIRE(fn_wex(c1, 2u, "ShaderLift", "Wave32 compiled OK") == 0);
        OS_REQUIRE(tr.hits.load() == 1 && tr.last_channel == "ShaderLift" && tr.last_text == "Wave32 compiled OK");

        uint32_t sev = 0u, more = 99u;
        char ch_buf[64]{}, txt_buf[128]{};
        OS_REQUIRE(fn_rd(c2, &sev, ch_buf, sizeof(ch_buf), txt_buf, sizeof(txt_buf), &more) == 0);
        OS_REQUIRE(sev == 2u && std::string(ch_buf) == "ShaderLift" && std::string(txt_buf) == "Wave32 compiled OK" && more == 0u);

        uint64_t written = 0u, ring_cnt = 0u;
        uint32_t active_cli = 0u;
        OS_REQUIRE(fn_stats(nullptr, &written, &ring_cnt, &active_cli) == 0);
        OS_REQUIRE(written >= 1u && ring_cnt >= 1u && active_cli == 2u);

        OS_REQUIRE(fn_unreg(cbtok) == 0);
        OS_REQUIRE(fn_sp(c1) == 0);
        OS_REQUIRE(fn_sp(c2) == 0);
        OS_REQUIRE(fn_at(&attached) == 0 && attached == 0);
        OS_REQUIRE(fn_cl(c1) == 0);
        OS_REQUIRE(fn_cl(c2) == 0);
        ::FreeLibrary(m);
    }

    std::printf("[PASS] Test 7: All 6 retired non-media stub DLLs (hix_xs, xbtp IOCP, xtfpix, gamecoreclient, minuserx, xtfdebugmonitor)\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path bin_dir = GetExeDir();
    const fs::path work_root =
        fs::temp_directory_path() /
        ("openscarlett_s16_w4_test_" + std::to_string(::GetCurrentProcessId()));
    ClearReadOnlyTree(work_root);
    std::error_code ec;
    fs::create_directories(work_root, ec);

    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    TestVfsTruncateAndNtHooks(bin_dir, work_root);
    TestXAsyncAndThreadingComTable();
    TestGameSaveContainerNameValidation(work_root);
    TestOfflineGdkSubsystemsParity();
    TestNativeWin32SystemUiMode();
    TestRetired6StubDlls(bin_dir, work_root);

    XGameRuntimeUninitialize();
    ClearReadOnlyTree(work_root);

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("=== ALL SPRINT 16 WORKSTREAM 4 TESTS PASSED ===\n");
    return 0;
}
