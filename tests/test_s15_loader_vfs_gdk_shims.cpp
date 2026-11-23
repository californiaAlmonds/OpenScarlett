// Sprint 15 Worker 4 end-to-end verification test suite:
//   1. MicrosoftGame.config XML character entity decoding (xcl a081a65)
//   2. VFS host PC folder preservation (xcl 61c6be8), ReplaceFileW CoW redirection (xcl #60),
//      ANSI file/dir hooks (xvalkyrie 8c1c7fb), virtualized GetModuleFileNameW/A &
//      GetModuleHandleW/A (PercyScarlett 3732674), delay-load import inspection (xvalkyrie PR#3)
//   3. XAsyncGetStatus(wait) completion-callback wait & XTaskQueue/XAsync conformance
//      (xbox-emulator #210/#211)
//   4. GDK Runtime offline & ABI conformance: XSystemGetConsoleId (xcl #66),
//      XUserGetGamertag components + E_BOUNDS (xcl #65), XAccessibilityImpl via QueryApiImpl
//      (Leonida #4235), XStoreShowRateAndReviewUIResult 1-byte bool write (AnyXboxX 8427e18),
//      libHttpClient.GDK header/handle/buffer validation (Leonida #4209)
//   5. KernelX & XMem hardening (xcl #87, TuxBox c23c83c) + ETW Controller (xbox-emulator c4b7607)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/vfs_mounts.h"
#include "openscarlett/shims/secondary_shims_abi.h"

namespace fs = std::filesystem;

namespace {

int g_checks_passed = 0;

void RequireImpl(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] test_s15_loader_vfs_gdk_shims.cpp:%d: %s\n", line, expr);
        std::exit(1);
    }
    ++g_checks_passed;
}

#define OS_REQUIRE(expr) RequireImpl(static_cast<bool>(expr), #expr, __LINE__)

void WriteTextFile(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(ofs.good());
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    ofs.flush();
    OS_REQUIRE(ofs.good());
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(ifs.good());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

fs::path MakeTempTestRoot(const char* tag) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
                    ("openscarlett_s15_w4_" + std::string(tag) + "_" + std::to_string(now));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    OS_REQUIRE(!ec && fs::exists(root));
    return root;
}

#if defined(_WIN32)
fs::path GetExeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) return {};
        if (len < buf.size() - 1u) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2u);
    }
    return fs::path(buf).parent_path();
}

HMODULE LoadSiblingDll(const wchar_t* name) {
    fs::path p = GetExeDir() / name;
    HMODULE mod = ::LoadLibraryW(p.c_str());
    if (!mod) {
        mod = ::LoadLibraryW(name);
    }
    return mod;
}
#endif

// ===========================================================================
// 1. MicrosoftGame.config XML Character Entity Decoding (xcl a081a65)
// ===========================================================================
void TestXmlEntityDecoding() {
    std::string decoded;
    OS_REQUIRE(openscarlett::DecodeXmlEntities(
                   "Tom &amp; Jerry &#x2122; &lt;Gold&gt; &quot;ED&quot; &apos;26&apos;", decoded)
                   .ok());
    const std::string expected_tm = "Tom & Jerry \xE2\x84\xA2 <Gold> \"ED\" '26'";
    OS_REQUIRE(decoded == expected_tm);

    // Decimal & 4-byte UTF-8 numeric character references
    OS_REQUIRE(openscarlett::DecodeXmlEntities("&#8482; &#x1F3AE; &#65;", decoded).ok());
    OS_REQUIRE(decoded == "\xE2\x84\xA2 \xF0\x9F\x8E\xAE A");

    // Unterminated / invalid surrogate entity must fail cleanly
    OS_REQUIRE(!openscarlett::DecodeXmlEntities("Bad &amp", decoded).ok());
    OS_REQUIRE(!openscarlett::DecodeXmlEntities("Surrogate &#xD800;", decoded).ok());

    const char* kConfigXml = R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="Acme &amp; Co &#169;" Publisher="CN=Acme &quot;Games&quot;" Version="1.0.0.0"/>
  <TitleId>&#x31;&#x32;345678</TitleId>
  <ExecutableList>
    <Executable Name="Title.exe" Id="Game" TargetDeviceFamily="Scarlett"
                OverrideDisplayName="Play &amp; Win &#x2122; &lt;XSX&gt;"/>
  </ExecutableList>
  <ShellVisuals DisplayName="Tom &amp; Jerry &#x2122; &apos;Deluxe&apos;"/>
</Game>
)";
    openscarlett::GameConfig cfg{};
    OS_REQUIRE(openscarlett::ParseGameConfigXml(kConfigXml, "test.mgc", cfg).ok());
    OS_REQUIRE(cfg.identity.name == "Acme & Co \xC2\xA9");
    OS_REQUIRE(cfg.identity.publisher == "CN=Acme \"Games\"");
    OS_REQUIRE(cfg.has_title_id && cfg.title_id_value == 0x12345678u);
    OS_REQUIRE(cfg.display_name == "Tom & Jerry \xE2\x84\xA2 'Deluxe'");
    OS_REQUIRE(cfg.executables.size() == 1u);
    OS_REQUIRE(cfg.executables[0].override_display_name == "Play & Win \xE2\x84\xA2 <XSX>");
    std::printf("  [PASS] 1. MicrosoftGame.config XML character entity decoding\n");
}

// ===========================================================================
// 2. VFS Hardening, ReplaceFileW, ANSI Hooks, Virtualized Module APIs, Delay-Load
// ===========================================================================
void TestVfsAndLoaderHardening() {
    const fs::path root = MakeTempTestRoot("vfs_loader");
    const fs::path g_root = root / "G_base";
    const fs::path g_overlay = root / "G_overlay";
    const fs::path t_root = root / "T_scratch";
    const fs::path s_root = root / "S_save";
    std::error_code ec;
    fs::create_directories(g_root, ec);
    fs::create_directories(g_overlay, ec);
    fs::create_directories(t_root, ec);
    fs::create_directories(s_root, ec);

    WriteTextFile(g_root / "config.ini", "BASE_V1");
    WriteTextFile(g_root / "patch_base.ini", "BASE_PATCH_V3");
    WriteTextFile(g_root / "GuestOnly" / "level.pak", "PAK_DATA");

    // 2a. Host PC folder preservation on console drive letters (xcl 61c6be8)
    {
        openscarlett::VfsMountConfig cfg{};
        cfg.g_root = g_root.wstring();
        cfg.g_overlay_root = g_overlay.wstring();
        cfg.t_root = t_root.wstring();
        cfg.s_root = s_root.wstring();
        cfg.preserve_host_drive_folders = true;
        cfg.preserved_host_prefixes = {L"G:\\HostSteamApps", L"D:\\HostTools"};

        openscarlett::VfsMounts vfs(cfg);
        std::wstring host_out;
        // Preserved host folders on G:\ and D:\ stay NotVirtual when unshadowed by guest!
        OS_REQUIRE(vfs.TranslateForAccessW(
                       L"G:\\HostSteamApps\\manifest.acf",
                       openscarlett::VfsAccessMode::ReadOnly, host_out) ==
                   openscarlett::VfsTranslateStatus::NotVirtual);
        OS_REQUIRE(vfs.TranslateForAccessW(
                       L"D:\\HostTools\\dump.exe",
                       openscarlett::VfsAccessMode::ReadOnly, host_out) ==
                   openscarlett::VfsTranslateStatus::NotVirtual);
        // GAME:\ is always strictly virtual even for the same folder name!
        OS_REQUIRE(vfs.TranslateForAccessW(
                       L"GAME:\\HostSteamApps\\manifest.acf",
                       openscarlett::VfsAccessMode::ReadOnly, host_out) ==
                   openscarlett::VfsTranslateStatus::Translated);
        // Existing guest folder always redirects cleanly!
        OS_REQUIRE(vfs.TranslateForAccessW(
                       L"G:\\GuestOnly\\level.pak",
                       openscarlett::VfsAccessMode::ReadOnly, host_out) ==
                   openscarlett::VfsTranslateStatus::Translated);
        // If a guest folder shadows a preserved host prefix, guest takes priority.
        WriteTextFile(g_root / "HostTools" / "guest_tool.bin", "GUEST");
        OS_REQUIRE(vfs.TranslateForAccessW(
                       L"D:\\HostTools\\guest_tool.bin",
                       openscarlett::VfsAccessMode::ReadOnly, host_out) ==
                   openscarlett::VfsTranslateStatus::Translated);
        fs::remove_all(g_root / "HostTools", ec);

        // Disabling preservation forces redirection even for preserved prefixes.
        cfg.preserve_host_drive_folders = false;
        vfs.Configure(cfg);
        OS_REQUIRE(vfs.TranslateForAccessW(
                       L"G:\\HostSteamApps\\manifest.acf",
                       openscarlett::VfsAccessMode::ReadOnly, host_out) ==
                   openscarlett::VfsTranslateStatus::Translated);
    }

    // 2b. Delay-load import inspection & preflight (xvalkyrie PR#3)
    {
        openscarlett::PeInspectionReport rep{};
        rep.file_path = "TestConsoleTitle.exe";
        openscarlett::PeImportModule k32{};
        k32.dll_name = "kernel32.dll";
        k32.symbols.push_back(openscarlett::PeImportSymbol{"CreateFileW", 0u, false, false});
        rep.imports.push_back(k32);

        openscarlett::PeImportModule dl_kx{};
        dl_kx.dll_name = "kernelx.dll";
        dl_kx.symbols.push_back(openscarlett::PeImportSymbol{"XMemAlloc", 0u, false, true});
        rep.imports.push_back(dl_kx);

        openscarlett::PeImportModule dl_d3d{};
        dl_d3d.dll_name = "d3d12_x.dll";
        dl_d3d.symbols.push_back(openscarlett::PeImportSymbol{"D3D12CreateDevice", 0u, false, true});
        rep.imports.push_back(dl_d3d);

        openscarlett::PeImportModule dl_xgr{};
        dl_xgr.dll_name = "XGamingRuntimeThunks.dll";
        dl_xgr.symbols.push_back(openscarlett::PeImportSymbol{"XGameRuntimeInitialize", 0u, false, true});
        rep.imports.push_back(dl_xgr);

        openscarlett::PeForwarderResolver resolver;
        const auto delay_entries = resolver.InspectDelayLoadImports(rep);
        OS_REQUIRE(delay_entries.size() == 3u);
        for (const auto& e : delay_entries) {
            OS_REQUIRE(e.delay_loaded);
            OS_REQUIRE(e.result.ok);
            OS_REQUIRE(e.result.kind == openscarlett::ForwarderResolutionKind::OpenScarlettShim);
        }

        const auto preflight = resolver.BuildPreflightReport(rep);
        OS_REQUIRE(preflight.ready_to_launch);
        OS_REQUIRE(preflight.delay_load_count == 3u);
        OS_REQUIRE(preflight.delay_load_shim_count == 3u);
        OS_REQUIRE(preflight.delay_load_dlls.size() == 3u);
        const std::string json = openscarlett::FormatPeForwarderPreflightJson(preflight);
        OS_REQUIRE(json.find("\"delay_load_count\": 3") != std::string::npos);
        OS_REQUIRE(json.find("\"delay_load_shim_count\": 3") != std::string::npos);
    }

#if defined(_WIN32)
    // 2c. Live hook DLL verification: ReplaceFileW, ANSI file/dir hooks,
    //     virtualized GetModuleFileNameW/A & GetModuleHandleW/A
    HMODULE hook_mod = LoadSiblingDll(L"openscarlett_hook.dll");
    OS_REQUIRE(hook_mod != nullptr);

    using PfnConfigureVfs = int(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*);
    using PfnSetVirtMod = int(__cdecl*)(int, const wchar_t*);
    using PfnGetModFnW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
    using PfnGetModFnA = DWORD(WINAPI*)(HMODULE, LPSTR, DWORD);
    using PfnGetModHandleW = HMODULE(WINAPI*)(LPCWSTR);
    using PfnGetModHandleA = HMODULE(WINAPI*)(LPCSTR);
    using PfnReplaceW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD);
    using PfnFindFirstA = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
    using PfnFindFirstExA = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    using PfnGetAttrExA = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using PfnCreateDirA = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
    using PfnRemoveDirA = BOOL(WINAPI*)(LPCSTR);
    using PfnMoveExA = BOOL(WINAPI*)(LPCSTR, LPCSTR, DWORD);
    using PfnCopyA = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);

    auto fn_cfg_vfs = reinterpret_cast<PfnConfigureVfs>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_ConfigureVfsForTestW"));
    auto fn_set_virt = reinterpret_cast<PfnSetVirtMod>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_SetVirtualModulePathMode"));
    auto fn_mod_fn_w = reinterpret_cast<PfnGetModFnW>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_GetModuleFileNameW"));
    auto fn_mod_fn_a = reinterpret_cast<PfnGetModFnA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_GetModuleFileNameA"));
    auto fn_mod_h_w = reinterpret_cast<PfnGetModHandleW>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_GetModuleHandleW"));
    auto fn_mod_h_a = reinterpret_cast<PfnGetModHandleA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_GetModuleHandleA"));
    auto fn_replace_w = reinterpret_cast<PfnReplaceW>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_ReplaceFileW"));
    auto fn_find_a = reinterpret_cast<PfnFindFirstA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_FindFirstFileA"));
    auto fn_find_ex_a = reinterpret_cast<PfnFindFirstExA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_FindFirstFileExA"));
    auto fn_attr_ex_a = reinterpret_cast<PfnGetAttrExA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_GetFileAttributesExA"));
    auto fn_mkdir_a = reinterpret_cast<PfnCreateDirA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_CreateDirectoryA"));
    auto fn_rmdir_a = reinterpret_cast<PfnRemoveDirA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_RemoveDirectoryA"));
    auto fn_move_a = reinterpret_cast<PfnMoveExA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_MoveFileExA"));
    auto fn_copy_a = reinterpret_cast<PfnCopyA>(
        ::GetProcAddress(hook_mod, "OpenScarlettHook_CopyFileA"));

    OS_REQUIRE(fn_cfg_vfs && fn_set_virt && fn_mod_fn_w && fn_mod_fn_a &&
               fn_mod_h_w && fn_mod_h_a && fn_replace_w && fn_find_a &&
               fn_find_ex_a && fn_attr_ex_a && fn_mkdir_a && fn_rmdir_a &&
               fn_move_a && fn_copy_a);

    OS_REQUIRE(fn_cfg_vfs(g_root.c_str(), g_overlay.c_str(), t_root.c_str(), s_root.c_str()) == 1);

    // ReplaceFileW CoW test #1: overlay replacement replaces base-only target with backup
    WriteTextFile(g_overlay / "config_new.ini", "OVERLAY_V2");
    OS_REQUIRE(fn_replace_w(L"G:\\config.ini", L"G:\\config_new.ini", L"G:\\config.bak", 0u) == TRUE);
    OS_REQUIRE(ReadTextFile(g_root / "config.ini") == "BASE_V1");      // Base MUST remain untouched!
    OS_REQUIRE(ReadTextFile(g_overlay / "config.ini") == "OVERLAY_V2");
    OS_REQUIRE(ReadTextFile(g_overlay / "config.bak") == "BASE_V1");
    OS_REQUIRE(!fs::exists(g_overlay / "config_new.ini"));

    // ReplaceFileW CoW test #2: base-only replacement MUST NOT delete file in g_root!
    OS_REQUIRE(fn_replace_w(L"G:\\config.ini", L"G:\\patch_base.ini", nullptr, 0u) == TRUE);
    OS_REQUIRE(ReadTextFile(g_root / "patch_base.ini") == "BASE_PATCH_V3");  // Base untouched!
    OS_REQUIRE(ReadTextFile(g_overlay / "config.ini") == "BASE_PATCH_V3");

    // ANSI file & directory hooks
    OS_REQUIRE(fn_mkdir_a("T:\\ansi_dir", nullptr) == TRUE);
    OS_REQUIRE(fs::is_directory(t_root / "ansi_dir"));
    WriteTextFile(t_root / "ansi_dir" / "sample.txt", "ANSI_HELLO_WORLD");

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    OS_REQUIRE(fn_attr_ex_a("T:\\ansi_dir\\sample.txt", GetFileExInfoStandard, &fad) == TRUE);
    OS_REQUIRE(fad.nFileSizeLow == 16u);

    OS_REQUIRE(fn_copy_a("T:\\ansi_dir\\sample.txt", "G:\\ansi_copied.txt", TRUE) == TRUE);
    OS_REQUIRE(ReadTextFile(g_overlay / "ansi_copied.txt") == "ANSI_HELLO_WORLD");
    // bFailIfExists == TRUE must fail when destination already exists
    OS_REQUIRE(fn_copy_a("T:\\ansi_dir\\sample.txt", "G:\\ansi_copied.txt", TRUE) == FALSE);

    WIN32_FIND_DATAA fda{};
    HANDLE hf = fn_find_a("G:\\ansi_*.txt", &fda);
    OS_REQUIRE(hf != INVALID_HANDLE_VALUE);
    OS_REQUIRE(std::string(fda.cFileName) == "ansi_copied.txt");
    ::FindClose(hf);

    WIN32_FIND_DATAA fda2{};
    HANDLE hf2 = fn_find_ex_a(
        "G:\\ansi_*.txt", FindExInfoBasic, &fda2, FindExSearchNameMatch, nullptr, 0u);
    OS_REQUIRE(hf2 != INVALID_HANDLE_VALUE);
    OS_REQUIRE(std::string(fda2.cFileName) == "ansi_copied.txt");
    ::FindClose(hf2);

    OS_REQUIRE(fn_move_a("T:\\ansi_dir\\sample.txt", "T:\\ansi_dir\\moved.txt", MOVEFILE_REPLACE_EXISTING) == TRUE);
    OS_REQUIRE(!fs::exists(t_root / "ansi_dir" / "sample.txt"));
    OS_REQUIRE(ReadTextFile(t_root / "ansi_dir" / "moved.txt") == "ANSI_HELLO_WORLD");
    fs::remove(t_root / "ansi_dir" / "moved.txt", ec);
    OS_REQUIRE(fn_rmdir_a("T:\\ansi_dir") == TRUE);
    OS_REQUIRE(!fs::exists(t_root / "ansi_dir"));

    // Virtualized GetModuleFileNameW/A & GetModuleHandleW/A (PercyScarlett 3732674)
    const fs::path exe_dir = GetExeDir();
    OS_REQUIRE(fn_cfg_vfs(exe_dir.c_str(), g_overlay.c_str(), t_root.c_str(), s_root.c_str()) == 1);
    OS_REQUIRE(fn_set_virt(1, L"G:") == 1);

    wchar_t vmod_w[MAX_PATH]{};
    const DWORD vlen_w = fn_mod_fn_w(nullptr, vmod_w, MAX_PATH);
    OS_REQUIRE(vlen_w > 0u);
    const std::wstring vmod_w_str(vmod_w, vlen_w);
    OS_REQUIRE(vmod_w_str.rfind(L"G:\\", 0u) == 0u);
    OS_REQUIRE(vmod_w_str.find(L"test_s15_loader_vfs_gdk_shims.exe") != std::wstring::npos);

    // Truncation semantics check on small buffer
    wchar_t tiny_w[6]{};
    const DWORD tiny_len = fn_mod_fn_w(nullptr, tiny_w, 6u);
    OS_REQUIRE(tiny_len == 6u);
    OS_REQUIRE(::GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    OS_REQUIRE(tiny_w[5] == L'\0');

    char vmod_a[MAX_PATH]{};
    const DWORD vlen_a = fn_mod_fn_a(nullptr, vmod_a, MAX_PATH);
    OS_REQUIRE(vlen_a > 0u);
    const std::string vmod_a_str(vmod_a, vlen_a);
    OS_REQUIRE(vmod_a_str.rfind("G:\\", 0u) == 0u);
    OS_REQUIRE(vmod_a_str.find("test_s15_loader_vfs_gdk_shims.exe") != std::string::npos);

    // Virtualized GetModuleHandleW/A for aliased Xbox DLLs & virtual G:\ exe path
    HMODULE xgr_mod = LoadSiblingDll(L"xgameruntime.dll");
    OS_REQUIRE(xgr_mod != nullptr);
    OS_REQUIRE(fn_mod_h_w(L"XGamingRuntimeThunks.dll") == xgr_mod);
    OS_REQUIRE(fn_mod_h_a("XGamingRuntimeThunks.dll") == xgr_mod);
    OS_REQUIRE(fn_mod_h_w(vmod_w_str.c_str()) == ::GetModuleHandleW(nullptr));

    // Disable virtual mode and verify real host path is restored
    OS_REQUIRE(fn_set_virt(0, nullptr) == 1);
    wchar_t real_w[MAX_PATH]{};
    const DWORD rlen_w = fn_mod_fn_w(nullptr, real_w, MAX_PATH);
    OS_REQUIRE(rlen_w > 0u);
    OS_REQUIRE(std::wstring(real_w, rlen_w).rfind(L"G:\\", 0u) != 0u);
#endif

    fs::remove_all(root, ec);
    std::printf("  [PASS] 2. VFS host-folder preservation, ReplaceFileW, ANSI hooks, virtual module APIs & delay-load\n");
}

// ===========================================================================
// 3. XAsyncGetStatus(wait) Completion-Callback Wait & XTaskQueue Conformance
// ===========================================================================
struct CallbackWaitProviderCtx {
    uint32_t payload = 0x5150CAFEu;
    std::atomic<int> work_count{0};
    std::atomic<int> cleanup_count{0};
};

XGameResult OPENSCARLETT_GDK_CALL CallbackWaitProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<CallbackWaitProviderCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->work_count.fetch_add(1, std::memory_order_acq_rel);
            XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (!data->buffer || data->bufferSize < sizeof(uint32_t)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, &ctx->payload, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            ctx->cleanup_count.fetch_add(1, std::memory_order_acq_rel);
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct CallbackWaitState {
    std::atomic<bool> reentrant_status_ok{false};
    std::atomic<bool> result_claimed_ok{false};
    std::atomic<uint32_t> result_value{0u};
    std::atomic<bool> callback_completed_side_effect{false};
};

void OPENSCARLETT_GDK_CALL SlowCompletionCb(XAsyncBlock* ab) {
    auto* st = static_cast<CallbackWaitState*>(ab->context);
    // Re-entrant XAsyncGetStatus(ab, true) inside callback MUST NOT deadlock!
    const XGameResult re_hr = XAsyncGetStatus(ab, true);
    st->reentrant_status_ok.store(re_hr == XGR_S_OK, std::memory_order_release);

    uint32_t val = 0u;
    size_t used = 0u;
    const XGameResult res_hr = XAsyncGetResult(ab, nullptr, sizeof(val), &val, &used);
    st->result_value.store(val, std::memory_order_release);
    st->result_claimed_ok.store(res_hr == XGR_S_OK && used == sizeof(val), std::memory_order_release);

    // Delay before setting side-effect flag so an external thread in XAsyncGetStatus(ab, true)
    // MUST block until this entire callback returns!
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    st->callback_completed_side_effect.store(true, std::memory_order_release);
}

struct OrderCheckItem {
    XAsyncBlock ab{};
    std::atomic<int> step{0};
    std::atomic<bool> order_ok{false};
};

XGameResult OPENSCARLETT_GDK_CALL OrderWorkFn(XAsyncBlock* ab) {
    auto* item = static_cast<OrderCheckItem*>(ab->context);
    int expected = 0;
    item->step.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
    return XGR_S_OK;
}

void OPENSCARLETT_GDK_CALL OrderCompCb(XAsyncBlock* ab) {
    auto* item = static_cast<OrderCheckItem*>(ab->context);
    const int prev = item->step.exchange(2, std::memory_order_acq_rel);
    OS_REQUIRE(XAsyncGetStatus(ab, true) == XGR_S_OK);
    item->order_ok.store(prev == 1, std::memory_order_release);
}

void TestXAsyncAndTaskQueueConformance() {
    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // 3a. XAsyncGetStatus(wait=true) waits for BOTH work AND completion callback even when
    //     the callback itself calls XAsyncGetStatus(wait=true) + XAsyncGetResult!
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_ThreadPool,
                       XTaskQueueDispatchMode_ThreadPool,
                       &q) == XGR_S_OK);

        CallbackWaitProviderCtx pctx{};
        CallbackWaitState cstate{};
        XAsyncBlock ab{};
        ab.queue = q;
        ab.context = &cstate;
        ab.callback = &SlowCompletionCb;

        OS_REQUIRE(XAsyncBegin(&ab, &pctx, &pctx, "S15WaitCb", &CallbackWaitProvider) == XGR_S_OK);
        OS_REQUIRE(XAsyncSchedule(&ab, 0u) == XGR_S_OK);

        // Wait on caller thread: MUST NOT return until SlowCompletionCb has finished its 35ms body!
        const XGameResult wait_hr = XAsyncGetStatus(&ab, true);
        OS_REQUIRE(wait_hr == XGR_S_OK);
        OS_REQUIRE(cstate.reentrant_status_ok.load(std::memory_order_acquire));
        OS_REQUIRE(cstate.result_claimed_ok.load(std::memory_order_acquire));
        OS_REQUIRE(cstate.result_value.load(std::memory_order_acquire) == 0x5150CAFEu);
        OS_REQUIRE(cstate.callback_completed_side_effect.load(std::memory_order_acquire));
        OS_REQUIRE(pctx.cleanup_count.load(std::memory_order_acquire) == 1);

        // Duplicate XAsyncGetResult after claim must fail cleanly.
        uint32_t dup_val = 0u;
        OS_REQUIRE(XAsyncGetResult(&ab, nullptr, sizeof(dup_val), &dup_val, nullptr) == XGR_E_INVALIDARG);

        OS_REQUIRE(XTaskQueueTerminate(q, true, nullptr, nullptr) == XGR_S_OK);
        XTaskQueueCloseHandle(q);
    }

    // 3b. Work-then-completion port ordering & concurrent multi-thread stress
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_ThreadPool,
                       XTaskQueueDispatchMode_ThreadPool,
                       &q) == XGR_S_OK);

        constexpr int kThreads = 4;
        constexpr int kPerThread = 32;
        std::vector<OrderCheckItem> items(kThreads * kPerThread);
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    auto& it = items[static_cast<size_t>(t * kPerThread + i)];
                    it.ab.queue = q;
                    it.ab.context = &it;
                    it.ab.callback = &OrderCompCb;
                    OS_REQUIRE(XAsyncRun(&it.ab, &OrderWorkFn) == XGR_S_OK);
                    OS_REQUIRE(XAsyncGetStatus(&it.ab, true) == XGR_S_OK);
                    OS_REQUIRE(it.order_ok.load(std::memory_order_acquire));
                    OS_REQUIRE(it.step.load(std::memory_order_acquire) == 2);
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        OS_REQUIRE(XTaskQueueTerminate(q, true, nullptr, nullptr) == XGR_S_OK);
        XTaskQueueCloseHandle(q);
    }

    // 3c. Manual queue XAsyncCancel verification
    {
        XTaskQueueHandle mq = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Manual,
                       &mq) == XGR_S_OK);
        CallbackWaitProviderCtx pctx{};
        std::atomic<bool> cb_ran{false};
        XAsyncBlock ab{};
        ab.queue = mq;
        ab.context = &cb_ran;
        ab.callback = [](XAsyncBlock* b) {
            static_cast<std::atomic<bool>*>(b->context)->store(true, std::memory_order_release);
        };
        OS_REQUIRE(XAsyncBegin(&ab, &pctx, &pctx, "S15Cancel", &CallbackWaitProvider) == XGR_S_OK);
        OS_REQUIRE(XAsyncSchedule(&ab, 0u) == XGR_S_OK);
        XAsyncCancel(&ab);
        OS_REQUIRE(XTaskQueueDispatch(mq, XTaskQueuePort_Work, 0u));
        OS_REQUIRE(pctx.work_count.load(std::memory_order_acquire) == 0);
        OS_REQUIRE(XTaskQueueDispatch(mq, XTaskQueuePort_Completion, 0u));
        OS_REQUIRE(cb_ran.load(std::memory_order_acquire));
        OS_REQUIRE(XAsyncGetStatus(&ab, false) == XGR_E_ABORT);
        OS_REQUIRE(pctx.cleanup_count.load(std::memory_order_acquire) == 1);
        OS_REQUIRE(XTaskQueueTerminate(mq, true, nullptr, nullptr) == XGR_S_OK);
        XTaskQueueCloseHandle(mq);
    }

    std::printf("  [PASS] 3. XAsyncGetStatus(wait) completion-callback wait & XTaskQueue conformance\n");
}

// ===========================================================================
// 4. GDK Runtime Offline & ABI Conformance + libHttpClient.GDK Validation
// ===========================================================================
void TestGdkOfflineAndLibHttpConformance() {
    // 4a. XSystemGetConsoleId dot-separated 8-group format (xcl #66)
    {
        char cid[64]{};
        size_t used = 0u;
        OS_REQUIRE(XSystemGetConsoleId(sizeof(cid), cid, &used) == XGR_S_OK);
        OS_REQUIRE(used == XSystemConsoleIdBytes);  // 39 chars + NUL = 40
        OS_REQUIRE(std::strlen(cid) == 39u);
        for (size_t i = 0u; i < 39u; ++i) {
            if (i % 5u == 4u) {
                OS_REQUIRE(cid[i] == '.');
            } else {
                const char c = cid[i];
                const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
                OS_REQUIRE(hex);
            }
        }
    }

    // 4b. XUserGetGamertag all 4 components + exact E_BOUNDS semantics (xcl #65)
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        XUserHandle user = nullptr;
        OS_REQUIRE(XUserAddResult(&ab, &user) == XGR_S_OK && user != nullptr);

        const XUserGamertagComponent modes[4] = {
            XUserGamertagComponent_Classic,
            XUserGamertagComponent_Modern,
            XUserGamertagComponent_ModernSuffix,
            XUserGamertagComponent_UniqueModern,
        };
        std::string tags[4];
        for (int i = 0; i < 4; ++i) {
            size_t needed = 0u;
            // Size query via (0, nullptr, &needed) -> XGR_E_BOUNDS + exact needed length
            OS_REQUIRE(XUserGetGamertag(user, modes[i], 0u, nullptr, &needed) == XGR_E_BOUNDS);
            OS_REQUIRE(needed >= 2u);

            // Too-small non-null buffer -> XGR_E_BOUNDS + exact needed length
            char small_buf[2]{};
            size_t small_used = 0u;
            if (needed > sizeof(small_buf)) {
                OS_REQUIRE(XUserGetGamertag(user, modes[i], sizeof(small_buf), small_buf, &small_used) ==
                           XGR_E_BOUNDS);
                OS_REQUIRE(small_used == needed);
            }

            // Exact-fit buffer -> XGR_S_OK
            std::vector<char> exact_buf(needed, '\xCC');
            size_t used = 0u;
            OS_REQUIRE(XUserGetGamertag(user, modes[i], exact_buf.size(), exact_buf.data(), &used) ==
                       XGR_S_OK);
            OS_REQUIRE(used == needed);
            OS_REQUIRE(exact_buf[needed - 1u] == '\0');
            tags[i] = exact_buf.data();
        }
        OS_REQUIRE(tags[0].size() < XUserGamertagComponentClassicMaxBytes);
        OS_REQUIRE(tags[3] == (tags[1] + "#" + tags[2]));
        // Invalid component enum -> XGR_E_INVALIDARG
        char dummy[16]{};
        OS_REQUIRE(XUserGetGamertag(user, static_cast<XUserGamertagComponent>(99u), sizeof(dummy), dummy, nullptr) ==
                   XGR_E_INVALIDARG);
        XUserCloseHandle(user);
    }

    // 4c. XAccessibilityImpl offline through QueryApiImpl (Leonida #4235)
    {
        OS_REQUIRE(XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XAccessibility));
        void* raw_vt = nullptr;
        OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_ACCESSIBILITY, &raw_vt) == XGR_S_OK && raw_vt != nullptr);
        auto* acc_vt = static_cast<const IXgrAccessibilityApiVtable*>(raw_vt);
        OS_REQUIRE(acc_vt->XClosedCaptionGetProperties != nullptr);
        OS_REQUIRE(acc_vt->XClosedCaptionSetEnabled != nullptr);
        OS_REQUIRE(acc_vt->XHighContrastGetMode != nullptr);
        OS_REQUIRE(acc_vt->XSpeechToTextSetPositionHint != nullptr);
        OS_REQUIRE(acc_vt->XSpeechToTextSendString != nullptr);
        OS_REQUIRE(acc_vt->XSpeechToTextBeginHypothesisString != nullptr);
        OS_REQUIRE(acc_vt->XSpeechToTextUpdateHypothesisString != nullptr);
        OS_REQUIRE(acc_vt->XSpeechToTextFinalizeHypothesisString != nullptr);
        OS_REQUIRE(acc_vt->XSpeechToTextCancelHypothesisString != nullptr);

        OS_REQUIRE(acc_vt->XClosedCaptionSetEnabled(true) == XGR_S_OK);
        XClosedCaptionProperties cc_props{};
        OS_REQUIRE(acc_vt->XClosedCaptionGetProperties(&cc_props) == XGR_S_OK);
        OS_REQUIRE(cc_props.Enabled);
        XHighContrastMode hc_mode = XHighContrastMode_Dark;
        OS_REQUIRE(acc_vt->XHighContrastGetMode(&hc_mode) == XGR_S_OK);
        OS_REQUIRE(hc_mode == XHighContrastMode_Off);
        uint32_t hyp_id = 0u;
        OS_REQUIRE(acc_vt->XSpeechToTextBeginHypothesisString(
                       "Player1", "Testing", XSpeechToTextType_Voice, &hyp_id) == XGR_S_OK &&
                   hyp_id != 0u);
        OS_REQUIRE(acc_vt->XSpeechToTextFinalizeHypothesisString(hyp_id, "Testing done") == XGR_S_OK);

        // 3-arg COM QueryApiImpl with both official IID and self-IID
        const XgrGuid kClsidAcc = {
            0x3E241977u, 0x6237u, 0x41E9u, {0x85, 0x59, 0x10, 0x2C, 0x2D, 0x99, 0x83, 0xF1}};
        const XgrGuid kIidAcc = {
            0xD722B373u, 0x8C4Du, 0x4692u, {0x9E, 0x51, 0xD6, 0xAD, 0x9B, 0x37, 0xAA, 0x7D}};
        using PfnQueryApi3 = XGameResult(OPENSCARLETT_GDK_CALL*)(const XgrGuid*, const XgrGuid*, void**);
        auto fn_query3 = reinterpret_cast<PfnQueryApi3>(&QueryApiImpl);
        void* com_acc = nullptr;
        OS_REQUIRE(fn_query3(&kClsidAcc, &kIidAcc, &com_acc) == XGR_S_OK && com_acc != nullptr);
        void* com_acc_self = nullptr;
        OS_REQUIRE(fn_query3(&kClsidAcc, &kClsidAcc, &com_acc_self) == XGR_S_OK && com_acc_self != nullptr);
    }

    // 4d. XStoreShowRateAndReviewUIResult strictly 1-byte bool write (AnyXboxX 8427e18)
    {
        XStoreContextHandle store_ctx = nullptr;
        OS_REQUIRE(XStoreCreateContext(nullptr, &store_ctx) == XGR_S_OK && store_ctx != nullptr);
        XAsyncBlock ab{};
        OS_REQUIRE(XStoreShowRateAndReviewUIAsync(store_ctx, &ab) == XGR_S_OK);

        alignas(8) uint8_t guard_buf[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
        auto* out_res = reinterpret_cast<XStoreRateAndReviewResult*>(guard_buf);
        OS_REQUIRE(XStoreShowRateAndReviewUIResult(&ab, out_res) == XGR_S_OK);
        OS_REQUIRE(guard_buf[0] == 0x00u);  // wasUpdated == false
        // Bytes 1..7 MUST be completely untouched (zero 4-byte overwrite)!
        OS_REQUIRE(guard_buf[1] == 0xBBu);
        OS_REQUIRE(guard_buf[2] == 0xCCu);
        OS_REQUIRE(guard_buf[3] == 0xDDu);
        OS_REQUIRE(guard_buf[4] == 0xEEu);
        OS_REQUIRE(guard_buf[5] == 0xFFu);
        OS_REQUIRE(guard_buf[6] == 0x11u);
        OS_REQUIRE(guard_buf[7] == 0x22u);
        XStoreCloseContextHandle(store_ctx);
    }

#if defined(_WIN32)
    // 4e. libHttpClient.GDK header validation, handle-type mismatch & buffer-size query (Leonida #4209)
    {
        HMODULE hc = LoadSiblingDll(L"libHttpClient.GDK.dll");
        OS_REQUIRE(hc != nullptr);

        using PfnHcInit = int32_t(OS_SHIM_CALL*)(const HCInitArgs*);
        using PfnHcCleanup = void(OS_SHIM_CALL*)(void);
        using PfnCallCreate = int32_t(OS_SHIM_CALL*)(HCCallHandle*);
        using PfnCallClose = int32_t(OS_SHIM_CALL*)(HCCallHandle);
        using PfnSetUrl = int32_t(OS_SHIM_CALL*)(HCCallHandle, const char*, const char*);
        using PfnSetHeader = int32_t(OS_SHIM_CALL*)(HCCallHandle, const char*, const char*, bool);
        using PfnPerform = int32_t(OS_SHIM_CALL*)(HCCallHandle, XAsyncBlock*);
        using PfnGetBodyBytes = int32_t(OS_SHIM_CALL*)(HCCallHandle, size_t, uint8_t*, size_t*);
        using PfnWsCreate = int32_t(OS_SHIM_CALL*)(void**, void*, void*, void*, void*);
        using PfnWsSetHeader = int32_t(OS_SHIM_CALL*)(void*, const char*, const char*);
        using PfnWsConnect = int32_t(OS_SHIM_CALL*)(const char*, const char*, void*, XAsyncBlock*);
        using PfnWsClose = int32_t(OS_SHIM_CALL*)(void*);

        auto fn_init = reinterpret_cast<PfnHcInit>(::GetProcAddress(hc, "HCInitialize"));
        auto fn_cleanup = reinterpret_cast<PfnHcCleanup>(::GetProcAddress(hc, "HCCleanup"));
        auto fn_create = reinterpret_cast<PfnCallCreate>(::GetProcAddress(hc, "HCHttpCallCreate"));
        auto fn_close = reinterpret_cast<PfnCallClose>(::GetProcAddress(hc, "HCHttpCallCloseHandle"));
        auto fn_set_url = reinterpret_cast<PfnSetUrl>(::GetProcAddress(hc, "HCHttpCallRequestSetUrl"));
        auto fn_set_hdr = reinterpret_cast<PfnSetHeader>(::GetProcAddress(hc, "HCHttpCallRequestSetHeader"));
        auto fn_perform = reinterpret_cast<PfnPerform>(::GetProcAddress(hc, "HCHttpCallPerformAsync"));
        auto fn_body = reinterpret_cast<PfnGetBodyBytes>(
            ::GetProcAddress(hc, "HCHttpCallResponseGetResponseBodyBytes"));
        auto fn_ws_create = reinterpret_cast<PfnWsCreate>(::GetProcAddress(hc, "HCWebSocketCreate"));
        auto fn_ws_hdr = reinterpret_cast<PfnWsSetHeader>(::GetProcAddress(hc, "HCWebSocketSetHeader"));
        auto fn_ws_conn = reinterpret_cast<PfnWsConnect>(::GetProcAddress(hc, "HCWebSocketConnectAsync"));
        auto fn_ws_close = reinterpret_cast<PfnWsClose>(::GetProcAddress(hc, "HCWebSocketCloseHandle"));
        OS_REQUIRE(fn_init && fn_cleanup && fn_create && fn_close && fn_set_url && fn_set_hdr &&
                   fn_perform && fn_body && fn_ws_create && fn_ws_hdr && fn_ws_conn && fn_ws_close);

        OS_REQUIRE(fn_init(nullptr) == OS_SHIM_S_OK);
        HCCallHandle call = nullptr;
        OS_REQUIRE(fn_create(&call) == OS_SHIM_S_OK && call != nullptr);
        void* ws = nullptr;
        OS_REQUIRE(fn_ws_create(&ws, nullptr, nullptr, nullptr, nullptr) == OS_SHIM_S_OK && ws != nullptr);

        // Header & URL & method validation
        OS_REQUIRE(fn_set_hdr(call, "Bad Header", "val", true) == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_set_hdr(call, "X-Col:on", "val", true) == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_set_hdr(call, "X-Good", "bad\r\nInject: 1", true) == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_set_hdr(call, "X-Valid-Header", "ok-value", true) == OS_SHIM_S_OK);
        OS_REQUIRE(fn_set_url(call, "BAD METHOD", "https://example.com") == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_set_url(call, "GET", "ftp://example.com/file") == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_set_url(call, "GET", "https://") == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_set_url(call, "GET", "https://api.example.com/v1/status") == OS_SHIM_S_OK);

        // WebSocket header & scheme validation
        OS_REQUIRE(fn_ws_hdr(ws, "Bad Space", "v") == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_ws_hdr(ws, "X-Ws", "bad\nval") == OS_SHIM_E_INVALIDARG);
        OS_REQUIRE(fn_ws_hdr(ws, "X-Ws", "good-val") == OS_SHIM_S_OK);
        XAsyncBlock ws_ab{};
        OS_REQUIRE(fn_ws_conn("https://not-ws.example.com", "", ws, &ws_ab) == OS_SHIM_E_INVALIDARG);

        // Handle-type mismatch detection (WebSocket passed to HTTP call & vice-versa -> E_HANDLE!)
        OS_REQUIRE(fn_set_url(reinterpret_cast<HCCallHandle>(ws), "GET", "https://example.com") ==
                   OS_SHIM_E_HANDLE);
        OS_REQUIRE(fn_set_hdr(reinterpret_cast<HCCallHandle>(ws), "X-Test", "1", true) ==
                   OS_SHIM_E_HANDLE);
        OS_REQUIRE(fn_close(reinterpret_cast<HCCallHandle>(ws)) == OS_SHIM_E_HANDLE);
        OS_REQUIRE(fn_ws_hdr(reinterpret_cast<void*>(call), "X-Test", "1") == OS_SHIM_E_HANDLE);
        OS_REQUIRE(fn_ws_close(reinterpret_cast<void*>(call)) == OS_SHIM_E_HANDLE);

        // Perform offline HTTP request and validate buffer-size query behavior
        XAsyncBlock http_ab{};
        OS_REQUIRE(fn_perform(call, &http_ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&http_ab, true) == XGR_S_OK);

        OS_REQUIRE(fn_body(call, 0u, nullptr, nullptr) == OS_SHIM_E_INVALIDARG);
        size_t body_used = 0u;
        OS_REQUIRE(fn_body(call, 0u, nullptr, &body_used) == OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        OS_REQUIRE(body_used > 0u);
        std::vector<uint8_t> body_buf(body_used, 0u);
        size_t got = 0u;
        OS_REQUIRE(fn_body(call, body_buf.size(), body_buf.data(), &got) == OS_SHIM_S_OK);
        OS_REQUIRE(got == body_used);

        OS_REQUIRE(fn_close(call) == OS_SHIM_S_OK);
        OS_REQUIRE(fn_ws_close(ws) == OS_SHIM_S_OK);
        fn_cleanup();
    }
#endif

    std::printf("  [PASS] 4. GDK offline & ABI conformance (ConsoleId, Gamertag E_BOUNDS, XAccessibility, XStore 1B bool, libHttpClient)\n");
}

// ===========================================================================
// 5. KernelX & XMem Hardening + ETW Controller
// ===========================================================================
std::atomic<int> g_custom_xmem_free_null_calls{0};
void* OPENSCARLETT_GDK_CALL GuardAllocHook(size_t sz, uint64_t) {
    return std::malloc(sz == 0 ? 16u : sz);
}
void OPENSCARLETT_GDK_CALL GuardFreeHook(void* p, uint64_t) {
    if (!p) {
        g_custom_xmem_free_null_calls.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    std::free(p);
}

#if defined(_WIN32)
struct TestEtwGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};

struct TestWnodeHeader {
    uint32_t BufferSize;
    uint32_t ProviderId;
    uint64_t HistoricalContext;
    int64_t TimeStamp;
    TestEtwGuid Guid;
    uint32_t ClientContext;
    uint32_t Flags;
};

struct TestEventTraceProperties {
    TestWnodeHeader Wnode;
    uint32_t BufferSize;
    uint32_t MinimumBuffers;
    uint32_t MaximumBuffers;
    uint32_t MaximumFileSize;
    uint32_t LogFileMode;
    uint32_t FlushTimer;
    uint32_t EnableFlags;
    int32_t AgeLimit;
    uint32_t NumberOfBuffers;
    uint32_t FreeBuffers;
    uint32_t EventsLost;
    uint32_t BuffersWritten;
    uint32_t LogBuffersLost;
    uint32_t RealTimeBuffersLost;
    void* LoggerThreadId;
    uint32_t LogFileNameOffset;
    uint32_t LoggerNameOffset;
};

struct EtwCbRecord {
    std::atomic<int> enable_calls{0};
    std::atomic<int> rundown_calls{0};
    std::atomic<int> disable_calls{0};
    std::atomic<uint8_t> last_level{0};
    std::atomic<uint64_t> last_match_any{0};
};

void __stdcall TestEtwEnableCb(
    const void* /*sourceId*/,
    uint32_t isEnabled,
    uint8_t level,
    uint64_t matchAnyKeyword,
    uint64_t /*matchAllKeyword*/,
    void* /*filterData*/,
    void* callbackContext) {
    auto* rec = static_cast<EtwCbRecord*>(callbackContext);
    if (!rec) return;
    if (isEnabled == 1u) {
        rec->enable_calls.fetch_add(1, std::memory_order_acq_rel);
        rec->last_level.store(level, std::memory_order_release);
        rec->last_match_any.store(matchAnyKeyword, std::memory_order_release);
    } else if (isEnabled == 2u) {
        rec->rundown_calls.fetch_add(1, std::memory_order_acq_rel);
    } else if (isEnabled == 0u) {
        rec->disable_calls.fetch_add(1, std::memory_order_acq_rel);
    }
}
#endif

void TestKernelXAndEtwController() {
#if defined(_WIN32)
    struct TestTitleMemStatus {
        uint32_t dwLength;
        uint32_t dwReserved;
        uint64_t ullTotalMem;
        uint64_t ullAvailMem;
        uint64_t ullLegacyUsed;
        uint64_t ullLegacyPeak;
        uint64_t ullLegacyAvail;
        uint64_t ullTitleUsed;
        uint64_t ullTitleAvail;
    };

    HMODULE kx = LoadSiblingDll(L"kernelx.dll");
    OS_REQUIRE(kx != nullptr);

    using PfnKxXMemAlloc = void*(__stdcall*)(size_t, uint64_t);
    using PfnKxXMemFree = void(__stdcall*)(void*, uint64_t);
    using PfnKxAuxAlloc = void*(__stdcall*)(size_t, size_t*);
    using PfnKxTitleMem = int32_t(__stdcall*)(TestTitleMemStatus*);
    using PfnKxVAlloc = void*(__stdcall*)(void*, size_t, uint32_t, uint32_t);
    using PfnKxVFree = int32_t(__stdcall*)(void*, size_t, uint32_t);
    using PfnKxVFreeEx = int32_t(__stdcall*)(void*, void*, size_t, uint32_t);
    using PfnKxSetThreadName = int32_t(__stdcall*)(void*, const wchar_t*);
    using PfnKxGetThreadName = int32_t(__stdcall*)(void*, wchar_t*, size_t, size_t*);

    auto kx_alloc = reinterpret_cast<PfnKxXMemAlloc>(::GetProcAddress(kx, "XMemAlloc"));
    auto kx_free = reinterpret_cast<PfnKxXMemFree>(::GetProcAddress(kx, "XMemFree"));
    auto kx_free_def = reinterpret_cast<PfnKxXMemFree>(::GetProcAddress(kx, "XMemFreeDefault"));
    auto kx_aux = reinterpret_cast<PfnKxAuxAlloc>(::GetProcAddress(kx, "XMemGetAuxiliaryTitleMemory"));
    auto kx_tmem = reinterpret_cast<PfnKxTitleMem>(::GetProcAddress(kx, "TitleMemoryStatus"));
    auto kx_valloc = reinterpret_cast<PfnKxVAlloc>(::GetProcAddress(kx, "VirtualAllocFromApp"));
    auto kx_vfree = reinterpret_cast<PfnKxVFree>(::GetProcAddress(kx, "VirtualFree"));
    auto kx_vfree_ex = reinterpret_cast<PfnKxVFreeEx>(::GetProcAddress(kx, "VirtualFreeEx"));
    auto kx_set_tn = reinterpret_cast<PfnKxSetThreadName>(::GetProcAddress(kx, "SetThreadName"));
    auto kx_get_tn = reinterpret_cast<PfnKxGetThreadName>(::GetProcAddress(kx, "GetThreadName"));
    OS_REQUIRE(kx_alloc && kx_free && kx_free_def && kx_aux && kx_tmem &&
               kx_valloc && kx_vfree && kx_vfree_ex && kx_set_tn && kx_get_tn);

    // 5a. KernelX overflow guards, VirtualFreeEx validation & GetThreadName truncation (xcl #87)
    {
        const size_t near_max = std::numeric_limits<size_t>::max() - 8u;
        OS_REQUIRE(kx_alloc(near_max, 0ull) == nullptr);
        OS_REQUIRE(kx_valloc(nullptr, near_max, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == nullptr);
        size_t aux_actual = 123u;
        OS_REQUIRE(kx_aux(near_max, &aux_actual) == nullptr);
        OS_REQUIRE(aux_actual == 0u);

        void* va = kx_valloc(nullptr, 64u * 1024u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        OS_REQUIRE(va != nullptr);

        // MEM_RELEASE with non-zero dwSize MUST fail with ERROR_INVALID_PARAMETER (87)!
        OS_REQUIRE(kx_vfree(va, 4096u, MEM_RELEASE) == 0);
        OS_REQUIRE(::GetLastError() == ERROR_INVALID_PARAMETER);
        OS_REQUIRE(kx_vfree_ex(::GetCurrentProcess(), va, 4096u, MEM_RELEASE) == 0);
        OS_REQUIRE(::GetLastError() == ERROR_INVALID_PARAMETER);
        // Null process handle -> ERROR_INVALID_HANDLE (6)
        OS_REQUIRE(kx_vfree_ex(nullptr, va, 0u, MEM_RELEASE) == 0);
        OS_REQUIRE(::GetLastError() == ERROR_INVALID_HANDLE);
        // MEM_RELEASE | MEM_DECOMMIT combination -> ERROR_INVALID_PARAMETER
        OS_REQUIRE(kx_vfree_ex(::GetCurrentProcess(), va, 0u, MEM_RELEASE | MEM_DECOMMIT) == 0);
        OS_REQUIRE(::GetLastError() == ERROR_INVALID_PARAMETER);
        // Invalid unallocated base -> clean failure (no crash)
        OS_REQUIRE(kx_vfree_ex(
                       ::GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t{0x12340000u}),
                       0u, MEM_RELEASE) == 0);
        // Valid VirtualFreeEx release succeeds cleanly
        OS_REQUIRE(kx_vfree_ex(::GetCurrentProcess(), va, 0u, MEM_RELEASE) == 1);

        // SetThreadName / GetThreadName bounds & truncation hardening
        OS_REQUIRE(kx_set_tn(nullptr, nullptr) < 0);
        OS_REQUIRE(kx_get_tn(nullptr, nullptr, 8u, nullptr) < 0);
        OS_REQUIRE(kx_get_tn(nullptr, nullptr, 0u, nullptr) < 0);
        OS_REQUIRE(kx_set_tn(nullptr, L"RenderWorkerThread") == 0);

        wchar_t trunc_buf[5] = {L'X', L'X', L'X', L'X', L'X'};
        size_t req_len = 0u;
        const int32_t trunc_hr = kx_get_tn(nullptr, trunc_buf, 5u, &req_len);
        OS_REQUIRE(trunc_hr == static_cast<int32_t>(0x8007007Au));
        OS_REQUIRE(req_len == 19u);  // wcslen(L"RenderWorkerThread") + 1
        OS_REQUIRE(std::wstring(trunc_buf) == L"Rend");

        wchar_t full_buf[64]{};
        OS_REQUIRE(kx_get_tn(nullptr, full_buf, 64u, &req_len) == 0);
        OS_REQUIRE(std::wstring(full_buf) == L"RenderWorkerThread");
    }

    // 5b. XMemFree(nullptr) no-op across kernelx, xmem, and xgameruntime (TuxBox c23c83c)
    {
        TestTitleMemStatus before{};
        before.dwLength = sizeof(before);
        OS_REQUIRE(kx_tmem(&before) != 0);

        kx_free(nullptr, 0ull);
        kx_free_def(nullptr, 0ull);

        TestTitleMemStatus after{};
        after.dwLength = sizeof(after);
        OS_REQUIRE(kx_tmem(&after) != 0);
        OS_REQUIRE(after.ullTitleUsed == before.ullTitleUsed);

        HMODULE xmem = LoadSiblingDll(L"xmem.dll");
        OS_REQUIRE(xmem != nullptr);
        auto fn_xmem_free = reinterpret_cast<PfnKxXMemFree>(::GetProcAddress(xmem, "XMemFree"));
        auto fn_xmem_free_def = reinterpret_cast<PfnKxXMemFree>(::GetProcAddress(xmem, "XMemFreeDefault"));
        OS_REQUIRE(fn_xmem_free && fn_xmem_free_def);
        fn_xmem_free(nullptr, 0ull);
        fn_xmem_free_def(nullptr, 0ull);

        // xgameruntime XMemFree(nullptr) must no-op BEFORE invoking custom free hook
        g_custom_xmem_free_null_calls.store(0, std::memory_order_release);
        OS_REQUIRE(XMemSetAllocationHooks(&GuardAllocHook, &GuardFreeHook) == XGR_S_OK);
        XMemFree(nullptr, 0ull);
        XMemFreeDefault(nullptr, 0ull);
        OS_REQUIRE(g_custom_xmem_free_null_calls.load(std::memory_order_acquire) == 0);
        OS_REQUIRE(XMemSetAllocationHooks(nullptr, nullptr) == XGR_S_OK);
    }

    // 5c. ETW Controller Layer in advapi32legacy.dll, KernelTraceControl.dll & pixevt.dll
    {
        HMODULE adv = LoadSiblingDll(L"advapi32legacy.dll");
        OS_REQUIRE(adv != nullptr);

        using PfnEventReg = uint32_t(__stdcall*)(const void*, void*, void*, uint64_t*);
        using PfnEventUnreg = uint32_t(__stdcall*)(uint64_t);
        using PfnStartTraceW = uint32_t(__stdcall*)(uint64_t*, const wchar_t*, void*);
        using PfnControlTraceW = uint32_t(__stdcall*)(uint64_t, const wchar_t*, void*, uint32_t);
        using PfnEnableTraceEx2 = uint32_t(__stdcall*)(
            uint64_t, const void*, uint32_t, uint8_t, uint64_t, uint64_t, uint32_t, void*);
        using PfnGetStats = int32_t(__stdcall*)(uint64_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*);

        auto fn_ev_reg = reinterpret_cast<PfnEventReg>(::GetProcAddress(adv, "EventRegister"));
        auto fn_ev_unreg = reinterpret_cast<PfnEventUnreg>(::GetProcAddress(adv, "EventUnregister"));
        auto fn_start = reinterpret_cast<PfnStartTraceW>(::GetProcAddress(adv, "StartTraceW"));
        auto fn_ctrl = reinterpret_cast<PfnControlTraceW>(::GetProcAddress(adv, "ControlTraceW"));
        auto fn_enable = reinterpret_cast<PfnEnableTraceEx2>(::GetProcAddress(adv, "EnableTraceEx2"));
        auto fn_stats = reinterpret_cast<PfnGetStats>(
            ::GetProcAddress(adv, "OpenScarlettEtwGetSessionStatsForTest"));
        OS_REQUIRE(fn_ev_reg && fn_ev_unreg && fn_start && fn_ctrl && fn_enable && fn_stats);

        const TestEtwGuid kProvGuid = {
            0x5CA1E770u, 0x1234u, 0x4ABCu, {0x90, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};
        EtwCbRecord cb_rec{};
        uint64_t reg_h = 0ull;
        OS_REQUIRE(fn_ev_reg(&kProvGuid, reinterpret_cast<void*>(&TestEtwEnableCb), &cb_rec, &reg_h) == 0u);
        OS_REQUIRE(reg_h != 0ull);

        TestEventTraceProperties bad_props{};
        bad_props.Wnode.BufferSize = 32u;  // too small
        uint64_t bad_h = 0ull;
        OS_REQUIRE(fn_start(&bad_h, L"OpenScarlettS15Etw", &bad_props) == 24u);  // ERROR_BAD_LENGTH

        TestEventTraceProperties props{};
        props.Wnode.BufferSize = sizeof(props);
        props.BufferSize = 64u;
        props.MinimumBuffers = 4u;
        props.MaximumBuffers = 16u;
        uint64_t sess_h = 0ull;
        OS_REQUIRE(fn_start(&sess_h, L"OpenScarlettS15Etw", &props) == 0u && sess_h != 0ull);
        // Duplicate active session name -> ERROR_ALREADY_EXISTS (183)
        uint64_t dup_h = 0ull;
        OS_REQUIRE(fn_start(&dup_h, L"OpenScarlettS15Etw", &props) == 183u);

        // Enable provider -> fires enable callback
        OS_REQUIRE(fn_enable(sess_h, &kProvGuid, 1u, 4u, 0x55ull, 0ull, 0u, nullptr) == 0u);
        OS_REQUIRE(cb_rec.enable_calls.load(std::memory_order_acquire) == 1);
        OS_REQUIRE(cb_rec.last_level.load(std::memory_order_acquire) == 4u);
        OS_REQUIRE(cb_rec.last_match_any.load(std::memory_order_acquire) == 0x55ull);

        // Capture-state (rundown) -> fires rundown callback
        OS_REQUIRE(fn_enable(sess_h, &kProvGuid, 2u, 4u, 0x55ull, 0ull, 0u, nullptr) == 0u);
        OS_REQUIRE(cb_rec.rundown_calls.load(std::memory_order_acquire) == 1);

        // Flush & Query
        TestEventTraceProperties qprops{};
        qprops.Wnode.BufferSize = sizeof(qprops);
        OS_REQUIRE(fn_ctrl(sess_h, nullptr, &qprops, 3u /*FLUSH*/) == 0u);
        OS_REQUIRE(qprops.BuffersWritten >= 1u);
        TestEventTraceProperties qprops2{};
        qprops2.Wnode.BufferSize = sizeof(qprops2);
        OS_REQUIRE(fn_ctrl(0ull, L"OpenScarlettS15Etw", &qprops2, 0u /*QUERY*/) == 0u);
        OS_REQUIRE(qprops2.Wnode.HistoricalContext == sess_h);

        uint32_t active = 0u, provs = 0u, flushes = 0u, rundowns = 0u;
        OS_REQUIRE(fn_stats(sess_h, &active, &provs, &flushes, &rundowns) == 1);
        OS_REQUIRE(active == 1u && provs == 1u && flushes == 1u && rundowns == 1u);

        // Stop -> fires disable callback and marks session stopped
        OS_REQUIRE(fn_ctrl(sess_h, nullptr, &qprops, 1u /*STOP*/) == 0u);
        OS_REQUIRE(cb_rec.disable_calls.load(std::memory_order_acquire) == 1);
        OS_REQUIRE(fn_ctrl(sess_h, nullptr, &qprops, 0u /*QUERY*/) == 4201u);  // WMI_INSTANCE_NOT_FOUND

        // Restarting same session name succeeds after stop
        uint64_t sess_h2 = 0ull;
        OS_REQUIRE(fn_start(&sess_h2, L"OpenScarlettS15Etw", &props) == 0u && sess_h2 != 0ull);
        OS_REQUIRE(fn_ctrl(sess_h2, nullptr, &props, 1u /*STOP*/) == 0u);
        OS_REQUIRE(fn_ev_unreg(reg_h) == 0u);

        // KernelTraceControl.dll ETW controller path
        HMODULE ktc = LoadSiblingDll(L"KernelTraceControl.dll");
        OS_REQUIRE(ktc != nullptr);
        auto ktc_start = reinterpret_cast<PfnStartTraceW>(::GetProcAddress(ktc, "StartTraceW"));
        auto ktc_ctrl = reinterpret_cast<PfnControlTraceW>(::GetProcAddress(ktc, "ControlTraceW"));
        auto ktc_en = reinterpret_cast<PfnEnableTraceEx2>(::GetProcAddress(ktc, "EnableTraceEx2"));
        OS_REQUIRE(ktc_start && ktc_ctrl && ktc_en);
        uint64_t ktc_h = 0ull;
        OS_REQUIRE(ktc_start(&ktc_h, L"OpenScarlettKtcEtw", &props) == 0u && ktc_h != 0ull);
        OS_REQUIRE(ktc_en(ktc_h, &kProvGuid, 1u, 5u, 0xFFull, 0ull, 0u, nullptr) == 0u);
        OS_REQUIRE(ktc_ctrl(ktc_h, nullptr, &props, 3u /*FLUSH*/) == 0u);
        OS_REQUIRE(ktc_ctrl(ktc_h, nullptr, &props, 1u /*STOP*/) == 0u);

        // pixevt.dll PIX ETW session tracking path
        HMODULE pix = LoadSiblingDll(L"pixevt.dll");
        OS_REQUIRE(pix != nullptr);
        using PfnPixStart = uint32_t(OS_SHIM_CALL*)(const wchar_t*, uint64_t*);
        using PfnPixEnable = uint32_t(OS_SHIM_CALL*)(uint64_t, const void*, uint32_t);
        using PfnPixHandleOp = uint32_t(OS_SHIM_CALL*)(uint64_t);
        using PfnPixStats = int32_t(OS_SHIM_CALL*)(uint64_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
        auto pix_start = reinterpret_cast<PfnPixStart>(::GetProcAddress(pix, "PIXEtwStartSession"));
        auto pix_en = reinterpret_cast<PfnPixEnable>(::GetProcAddress(pix, "PIXEtwEnableProvider"));
        auto pix_flush = reinterpret_cast<PfnPixHandleOp>(::GetProcAddress(pix, "PIXEtwFlushAndRundown"));
        auto pix_stop = reinterpret_cast<PfnPixHandleOp>(::GetProcAddress(pix, "PIXEtwStopSession"));
        auto pix_stats = reinterpret_cast<PfnPixStats>(::GetProcAddress(pix, "PIXEtwGetSessionStatsForTest"));
        OS_REQUIRE(pix_start && pix_en && pix_flush && pix_stop && pix_stats);
        uint64_t pix_h = 0ull;
        OS_REQUIRE(pix_start(L"PixCaptureSession", &pix_h) == 0u && pix_h != 0ull);
        OS_REQUIRE(pix_en(pix_h, &kProvGuid, 1u) == 0u);
        OS_REQUIRE(pix_flush(pix_h) == 0u);
        OS_REQUIRE(pix_stats(pix_h, &active, &provs, &flushes, &rundowns) == 1);
        OS_REQUIRE(active == 1u && provs == 1u && flushes == 1u && rundowns == 1u);
        OS_REQUIRE(pix_stop(pix_h) == 0u);
        OS_REQUIRE(pix_stats(pix_h, &active, &provs, &flushes, &rundowns) == 1);
        OS_REQUIRE(active == 0u && provs == 0u && rundowns == 2u);
    }
#endif

    std::printf("  [PASS] 5. KernelX/XMem hardening & ETW controller layer\n");
}

}  // namespace

int main() {
    std::printf("=== Sprint 15 Worker 4: Loader / VFS / GDK / KernelX / ETW Test Suite ===\n");
    TestXmlEntityDecoding();
    TestVfsAndLoaderHardening();
    TestXAsyncAndTaskQueueConformance();
    TestGdkOfflineAndLibHttpConformance();
    TestKernelXAndEtwController();
    std::printf("=== ALL %d SPRINT 15 W4 CHECKS PASSED ===\n", g_checks_passed);
    return 0;
}
