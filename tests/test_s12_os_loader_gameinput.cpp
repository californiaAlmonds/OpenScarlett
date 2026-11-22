#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/kernel/console_lsa_policy.h"
#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/vfs_mounts.h"
#include "openscarlett/loader/xvd_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

// Self-contained NT types matching hook_dll.cpp
using OS_NTSTATUS = LONG;
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

// GameInput Sprint 9-12 dynamic C ABI mirrors
struct OsGameInputRumbleTelemetry {
    GameInputRumbleParams lastParams;
    uint16_t xinputLeftMotor;
    uint16_t xinputRightMotor;
    uint32_t activeSlot;
    uint64_t updateCount;
    uint64_t impulseTriggerUpdateCount;
    uint64_t lastTimestampUs;
};
struct OsGameInputUserAssociationEvent {
    uint8_t deviceId[32];
    uint64_t oldUserLocalId;
    uint64_t newUserLocalId;
    uint64_t timestampUs;
    uint64_t eventSequence;
};
using OsGameInputUserAssociationCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    void* context,
    const OsGameInputUserAssociationEvent* event);
using OsGameInputDeviceCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentStatus,
    uint32_t previousStatus);

// Spatial Audio COM mirrors matching xaudio2_shim.cpp
constexpr XAudio2Guid kIidSpatialAudioClient = {
    0xBBF8E066u, 0xAAAAu, 0x49BEu, {0x9Au, 0x4Du, 0xFDu, 0x2Au, 0x85u, 0x8Eu, 0xAEu, 0x29u}};
constexpr XAudio2Guid kIidSpatialAudioRenderStream = {
    0xBAB5F473u, 0xB423u, 0x477Bu, {0x85u, 0xF5u, 0xB5u, 0xA3u, 0x32u, 0xA0u, 0x41u, 0x53u}};

struct OS_SpatialAudioObjectRenderStreamActivationParams {
    const OS_WAVEFORMATEX* ObjectFormat;
    uint32_t StaticObjectTypeMask;
    uint32_t MinDynamicObjectCount;
    uint32_t MaxDynamicObjectCount;
    uint32_t Category;
    void* EventHandle;
    void* NotifyObject;
};

struct OS_ISpatialAudioObject : public OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetBuffer(uint8_t** buffer, uint32_t* bufferLength) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetEndOfStream(uint32_t frameCount) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL IsActive(int32_t* isActive) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetAudioObjectType(uint32_t* audioObjectType) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetPosition(float x, float y, float z) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float volume) = 0;
};

struct OS_ISpatialAudioObjectRenderStream : public OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetAvailableDynamicObjectCount(uint32_t* value) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Start() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Stop() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Reset() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL BeginUpdatingAudioObjects(
        uint32_t* availableDynamicObjectCount,
        uint32_t* frameCountPerBuffer) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL ActivateSpatialAudioObject(
        uint32_t type,
        OS_ISpatialAudioObject** audioObject) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL EndUpdatingAudioObjects() = 0;
};

struct OS_ISpatialAudioClient : public OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetStaticObjectPosition(
        uint32_t type, float* x, float* y, float* z) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetNativeStaticObjectTypeMask(uint32_t* mask) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetMaxDynamicObjectCount(uint32_t* value) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL IsAudioObjectFormatSupported(
        const OS_WAVEFORMATEX* objectFormat) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetMaxFrameCount(
        const OS_WAVEFORMATEX* objectFormat, uint32_t* frameCountPerBuffer) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL ActivateSpatialAudioStream(
        const OS_SpatialAudioObjectRenderStreamActivationParams* activationParams,
        const XAudio2Guid* riid,
        void** stream) = 0;
};

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path ResolveScratchRoot() {
    const std::wstring work = openscarlett::ResolveDefaultWorkDirW(ResolveRuntimeBinDir().wstring());
    if (!work.empty()) {
        return (fs::path(work) / L"test_s12_os_loader_gameinput").make_preferred();
    }
    return (ResolveRuntimeBinDir() / L".tmp_s12_os_loader_gameinput").make_preferred();
}

void WriteTextFile(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    OS_REQUIRE(static_cast<bool>(ofs));
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool RunProcessCaptureStdout(const std::wstring& exe_path,
                             const std::wstring& args,
                             DWORD& out_exit_code,
                             std::string& out_stdout) {
    out_exit_code = 999;
    out_stdout.clear();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return false;
    }
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = L"\"" + exe_path + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    ::CloseHandle(write_pipe);
    if (!ok) {
        ::CloseHandle(read_pipe);
        return false;
    }

    char buf[4096];
    DWORD bytes_read = 0;
    while (::ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        out_stdout.append(buf, bytes_read);
    }
    ::CloseHandle(read_pipe);
    ::WaitForSingleObject(pi.hProcess, 30000);
    ::GetExitCodeProcess(pi.hProcess, &out_exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

template <typename T>
void WriteLe(std::vector<uint8_t>& buf, size_t off, T val) {
    OS_REQUIRE(off + sizeof(T) <= buf.size());
    std::memcpy(buf.data() + off, &val, sizeof(T));
}

std::vector<uint8_t> BuildSyntheticEncryptedXvd() {
    using namespace openscarlett;
    std::vector<uint8_t> hdr(kXvdHeaderTotalSize, 0u);
    std::memcpy(hdr.data() + kXvdMagicOffset, "msft-xvd", 8u);
    const uint32_t flags = XvdVolumeFlags::kReadOnly | XvdVolumeFlags::kEraSpecific;
    WriteLe<uint32_t>(hdr, kXvdVolumeFlagsOffset, flags);
    WriteLe<uint32_t>(hdr, kXvdFormatVersionOffset, 3u);
    WriteLe<uint64_t>(hdr, kXvdDriveSizeOffset, 64ull * kXvdPageSize);
    WriteLe<uint32_t>(hdr, kXvdTypeOffset, static_cast<uint32_t>(XvdDiskType::Fixed));
    WriteLe<uint32_t>(hdr, kXvdContentTypeOffset, static_cast<uint32_t>(XvdContentType::Title));
    WriteLe<uint32_t>(hdr, kXvdBlockSizeOffset, kXvdDefaultBlockSize);
    std::memcpy(hdr.data() + kXvdSandboxIdOffset, "RETAIL", 6u);
    hdr[0x40u] = 0xAAu;  // Signed marker
    hdr[kXvdKeyMaterialOffset] = 0x55u;
    return hdr;
}

// ---------------------------------------------------------------------------
// 1. Nested VFS Mounts + \??\ NT Object Path Translation (G:, D:, T:, S:)
// ---------------------------------------------------------------------------
void TestNestedVfsAndNtObjectPaths(const fs::path& scratch) {
    using namespace openscarlett;
    const fs::path root = scratch / "vfs_nested";
    const fs::path g_base = root / "G_base";
    const fs::path g_ovl = root / "G_overlay";
    const fs::path dlc1 = root / "DLC_Pack1";
    const fs::path dlc1_hd = root / "DLC_Pack1_HD";
    const fs::path t_dir = root / "T_scratch";
    const fs::path s_dir = root / "S_save";

    WriteTextFile(g_base / "base.bin", "BASE_PAYLOAD");
    WriteTextFile(dlc1 / "pack.bin", "DLC1_PAYLOAD");
    WriteTextFile(dlc1_hd / "ultra.dds", "HD_TEXTURE_PAYLOAD");

    VfsMountConfig cfg{};
    cfg.g_root = g_base.wstring();
    cfg.g_overlay_root = g_ovl.wstring();
    cfg.t_root = t_dir.wstring();
    cfg.s_root = s_dir.wstring();
    VfsMounts vfs(cfg);
    // Register shallower before deeper to verify longest-prefix sorting.
    OS_REQUIRE(vfs.AddGameSubMount(L"DLC/Pack1", dlc1.wstring()));
    OS_REQUIRE(vfs.AddGameSubMount(L"D:\\dlc\\pack1\\HD", dlc1_hd.wstring()));

    // Verify \??\ NT prefix + G:, D:, T:, S:, mixed case, forward slashes, longest-prefix sub-mounts
    std::wstring out;
    OS_REQUIRE(vfs.TranslateW(L"\\??\\G:\\base.bin", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\??\\" + (g_base / L"base.bin").wstring());

    OS_REQUIRE(vfs.TranslateW(L"\\??\\D:\\base.bin", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\??\\" + (g_base / L"base.bin").wstring());

    OS_REQUIRE(vfs.TranslateW(L"\\??\\D:/dlc/PACK1/pack.bin", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\??\\" + (dlc1 / L"pack.bin").wstring());

    OS_REQUIRE(vfs.TranslateW(L"\\??\\g:/DLC/pack1/hd/ultra.dds", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\??\\" + (dlc1_hd / L"ultra.dds").wstring());

    OS_REQUIRE(vfs.TranslateW(L"\\GLOBAL??\\T:\\cache\\pso.bin", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\??\\" + (t_dir / L"cache\\pso.bin").wstring());

    OS_REQUIRE(vfs.TranslateW(L"\\??\\S:\\slot0.sav", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\??\\" + (s_dir / L"slot0.sav").wstring());

    // Traversal attack through \??\D:\ must still be blocked
    OS_REQUIRE(vfs.TranslateW(L"\\??\\D:\\..\\escape.txt", out) == VfsTranslateStatus::RejectedTraversal);
    std::printf("[PASS] Nested VFS sub-mounts & NT \\??\\ path translation (G:/D:/T:/S:)\n");
}

// ---------------------------------------------------------------------------
// 2. Hook DLL Live Integration: TLS Slot 0, NtCreateFile/NtOpenFile/NtQueryAttributesFile,
//    PeForwarderResolver, and ConsoleLsaPolicy
// ---------------------------------------------------------------------------
void TestHookDllTlsNtForwarderLsa(const fs::path& bin_dir, const fs::path& scratch) {
    // Place a deterministic sentinel in TLS slot 0 BEFORE loading openscarlett_hook.dll
    // (if slot 0 is already claimed by CRT/host, preserve & restore its original value!).
    LPVOID orig_slot0 = ::TlsGetValue(0u);
    const DWORD orig_err = ::GetLastError();
    const uintptr_t sentinel = 0x00005CA012345678ull;
    const bool can_write_slot0 = (orig_err == ERROR_SUCCESS);
    if (can_write_slot0) {
        ::TlsSetValue(0u, reinterpret_cast<LPVOID>(sentinel));
    }

    const fs::path hook_path = bin_dir / "openscarlett_hook.dll";
    OS_REQUIRE(fs::exists(hook_path));
    HMODULE hook_mod = ::LoadLibraryW(hook_path.c_str());
    OS_REQUIRE(hook_mod != nullptr);

    // Verify TLS slot 0 preservation immediately after DllMain(DLL_PROCESS_ATTACH)!
    if (can_write_slot0) {
        LPVOID after_attach = ::TlsGetValue(0u);
        OS_REQUIRE(reinterpret_cast<uintptr_t>(after_attach) == sentinel);
        ::TlsSetValue(0u, orig_slot0);
    }

    using PfnGetTls0 = int(__cdecl*)(OpenScarlettTlsSlot0Status*);
    auto get_tls0 = reinterpret_cast<PfnGetTls0>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_GetTlsSlot0Status")));
    OS_REQUIRE(get_tls0 != nullptr);
    OpenScarlettTlsSlot0Status tls_st{};
    OS_REQUIRE(get_tls0(&tls_st) == 1);
    OS_REQUIRE(tls_st.initialized == 1u);
    OS_REQUIRE(tls_st.preserved_ok == 1u);
    if (can_write_slot0) {
        OS_REQUIRE(tls_st.saved_value_at_attach == sentinel);
    }

    // Verify live PeForwarderResolver inside openscarlett_hook.dll
    using PfnResolveFwd = int(__cdecl*)(const char*, char*, uint32_t, char*, uint32_t, uint32_t*);
    auto resolve_fwd = reinterpret_cast<PfnResolveFwd>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_ResolveForwarderA")));
    OS_REQUIRE(resolve_fwd != nullptr);
    char out_dll[128]{};
    char out_sym[128]{};
    uint32_t out_kind = 99u;
    OS_REQUIRE(resolve_fwd("API-MS-WIN-GAMING-TCUI-L1-1-0.ShowGameInviteUI",
                           out_dll, sizeof(out_dll), out_sym, sizeof(out_sym), &out_kind) == 1);
    OS_REQUIRE(std::string(out_dll) == "xgameruntime.dll");
    OS_REQUIRE(std::string(out_sym) == "ShowGameInviteUI");
    OS_REQUIRE(out_kind == static_cast<uint32_t>(openscarlett::ForwarderResolutionKind::OpenScarlettShim));

    OS_REQUIRE(resolve_fwd("API-MS-WIN-CORE-SYNCH-L1-2-0.WaitOnAddress",
                           out_dll, sizeof(out_dll), out_sym, sizeof(out_sym), &out_kind) == 1);
    OS_REQUIRE(std::string(out_dll) == "kernelbase.dll");

    // Verify live ConsoleLsaPolicy inside openscarlett_hook.dll loaded from MicrosoftGame.Config
    const fs::path lsa_cfg_path = scratch / "lsa_pkg" / "MicrosoftGame.Config";
    WriteTextFile(
        lsa_cfg_path,
        "<Game configVersion=\"1\">\n"
        "  <Identity Name=\"OpenScarlett.Sprint12Title\" Publisher=\"CN=OpenScarlett\" Version=\"1.2.3.4\"/>\n"
        "  <TitleId>5CA01212</TitleId>\n"
        "  <StoreId>9S12TEST0001</StoreId>\n"
        "  <ShellVisuals DefaultDisplayName=\"Sprint 12 Live Title\"/>\n"
        "  <ExecutableList><Executable Name=\"openscarlett.exe\" Id=\"Game\" TargetDeviceFamily=\"Scarlett\"/></ExecutableList>\n"
        "</Game>\n");

    using PfnReloadLsa = int(__cdecl*)(const char*, char*, uint32_t, uint32_t*);
    auto reload_lsa = reinterpret_cast<PfnReloadLsa>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_ReloadConsoleLsaFromConfigA")));
    OS_REQUIRE(reload_lsa != nullptr);
    char lsa_pfn[256]{};
    uint32_t lsa_tid = 0u;
    OS_REQUIRE(reload_lsa(lsa_cfg_path.string().c_str(), lsa_pfn, sizeof(lsa_pfn), &lsa_tid) == 1);
    OS_REQUIRE(lsa_tid == 0x5CA01212u);
    OS_REQUIRE(std::string(lsa_pfn).rfind("OpenScarlett.Sprint12Title_", 0) == 0);

    // Verify live NtCreateFile / NtOpenFile / NtQueryAttributesFile hooks with nested sub-mounts!
    using PfnConfigVfs = int(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*);
    using PfnAddSub = int(__cdecl*)(const wchar_t*, const wchar_t*);
    using PfnNtCreate = OS_NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, OS_OBJECT_ATTRIBUTES*,
                                            OS_IO_STATUS_BLOCK*, PLARGE_INTEGER, ULONG, ULONG,
                                            ULONG, ULONG, PVOID, ULONG);
    using PfnNtOpen = OS_NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, OS_OBJECT_ATTRIBUTES*,
                                          OS_IO_STATUS_BLOCK*, ULONG, ULONG);
    using PfnNtQueryAttr = OS_NTSTATUS(NTAPI*)(OS_OBJECT_ATTRIBUTES*, OS_FILE_BASIC_INFORMATION*);

    auto cfg_vfs = reinterpret_cast<PfnConfigVfs>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_ConfigureVfsForTestW")));
    auto add_sub = reinterpret_cast<PfnAddSub>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_AddVfsSubMountW")));
    auto nt_create = reinterpret_cast<PfnNtCreate>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_NtCreateFile")));
    auto nt_open = reinterpret_cast<PfnNtOpen>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_NtOpenFile")));
    auto nt_query = reinterpret_cast<PfnNtQueryAttr>(
        reinterpret_cast<void*>(::GetProcAddress(hook_mod, "OpenScarlettHook_NtQueryAttributesFile")));
    OS_REQUIRE(cfg_vfs && add_sub && nt_create && nt_open && nt_query);

    const fs::path nt_root = scratch / "nt_vfs_live";
    const fs::path g_dir = nt_root / "G";
    const fs::path g_ovl = nt_root / "G_ovl";
    const fs::path dlc_dir = nt_root / "DLC_HD";
    const fs::path t_dir = nt_root / "T";
    const fs::path s_dir = nt_root / "S";
    WriteTextFile(g_dir / "game.txt", "NT_GAME_OK");
    WriteTextFile(dlc_dir / "hd.txt", "NT_DLC_HD_OK");

    OS_REQUIRE(cfg_vfs(g_dir.c_str(), g_ovl.c_str(), t_dir.c_str(), s_dir.c_str()) == 1);
    OS_REQUIRE(add_sub(L"DLC\\Pack1\\HD", dlc_dir.c_str()) == 1);

    auto make_oa = [](std::wstring& path_buf, OS_UNICODE_STRING& us, OS_OBJECT_ATTRIBUTES& oa) {
        us.Length = static_cast<USHORT>(path_buf.size() * sizeof(wchar_t));
        us.MaximumLength = static_cast<USHORT>((path_buf.size() + 1u) * sizeof(wchar_t));
        us.Buffer = path_buf.data();
        std::memset(&oa, 0, sizeof(oa));
        oa.Length = sizeof(oa);
        oa.ObjectName = &us;
        oa.Attributes = 0x40u;  // OBJ_CASE_INSENSITIVE
    };

    // 2a. NtQueryAttributesFile on \??\D:\DLC\Pack1\HD\hd.txt
    {
        std::wstring p = L"\\??\\D:\\dlc\\pack1\\HD\\hd.txt";
        OS_UNICODE_STRING us{};
        OS_OBJECT_ATTRIBUTES oa{};
        make_oa(p, us, oa);
        OS_FILE_BASIC_INFORMATION info{};
        const OS_NTSTATUS st = nt_query(&oa, &info);
        OS_REQUIRE(st >= 0);
    }

    // 2b. NtOpenFile on \??\G:\game.txt and read back contents
    {
        std::wstring p = L"\\??\\G:\\game.txt";
        OS_UNICODE_STRING us{};
        OS_OBJECT_ATTRIBUTES oa{};
        make_oa(p, us, oa);
        HANDLE h = nullptr;
        OS_IO_STATUS_BLOCK iosb{};
        const OS_NTSTATUS st = nt_open(&h, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb,
                                       FILE_SHARE_READ, 0x20u /*FILE_SYNCHRONOUS_IO_NONALERT*/);
        OS_REQUIRE(st >= 0 && h != nullptr && h != INVALID_HANDLE_VALUE);
        char buf[64]{};
        DWORD rd = 0;
        OS_REQUIRE(::ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr) != FALSE);
        ::CloseHandle(h);
        OS_REQUIRE(std::string(buf, rd) == "NT_GAME_OK");
    }

    // 2c. NtCreateFile writing to \??\T:\nt_scratch.txt and \??\S:\nt_save.dat
    {
        std::wstring p = L"\\??\\T:\\nt_scratch.txt";
        OS_UNICODE_STRING us{};
        OS_OBJECT_ATTRIBUTES oa{};
        make_oa(p, us, oa);
        HANDLE h = nullptr;
        OS_IO_STATUS_BLOCK iosb{};
        const OS_NTSTATUS st = nt_create(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, nullptr,
                                         FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                         5u /*FILE_OVERWRITE_IF*/, 0x20u, nullptr, 0u);
        OS_REQUIRE(st >= 0 && h != nullptr && h != INVALID_HANDLE_VALUE);
        const char msg[] = "NT_WRITE_T_OK";
        DWORD wr = 0;
        OS_REQUIRE(::WriteFile(h, msg, static_cast<DWORD>(sizeof(msg) - 1), &wr, nullptr) != FALSE);
        ::CloseHandle(h);
        OS_REQUIRE(ReadTextFile(t_dir / "nt_scratch.txt") == "NT_WRITE_T_OK");
    }
    {
        std::wstring p = L"\\??\\S:\\nt_save.dat";
        OS_UNICODE_STRING us{};
        OS_OBJECT_ATTRIBUTES oa{};
        make_oa(p, us, oa);
        HANDLE h = nullptr;
        OS_IO_STATUS_BLOCK iosb{};
        const OS_NTSTATUS st = nt_create(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, nullptr,
                                         FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                         5u /*FILE_OVERWRITE_IF*/, 0x20u, nullptr, 0u);
        OS_REQUIRE(st >= 0 && h != nullptr && h != INVALID_HANDLE_VALUE);
        const char msg[] = "NT_WRITE_S_OK";
        DWORD wr = 0;
        OS_REQUIRE(::WriteFile(h, msg, static_cast<DWORD>(sizeof(msg) - 1), &wr, nullptr) != FALSE);
        ::CloseHandle(h);
        OS_REQUIRE(ReadTextFile(s_dir / "nt_save.dat") == "NT_WRITE_S_OK");
    }

    // 2d. Traversal rejection on NtOpenFile (\??\G:\..\escape.txt)
    {
        std::wstring p = L"\\??\\G:\\..\\escape.txt";
        OS_UNICODE_STRING us{};
        OS_OBJECT_ATTRIBUTES oa{};
        make_oa(p, us, oa);
        HANDLE h = nullptr;
        OS_IO_STATUS_BLOCK iosb{};
        const OS_NTSTATUS st = nt_open(&h, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ, 0x20u);
        OS_REQUIRE(st == static_cast<OS_NTSTATUS>(0xC0000022UL));
    }

    std::printf("[PASS] Hook DLL live TLS slot 0, PeForwarder, ConsoleLsaPolicy & Nt* VFS hooks\n");
}

// ---------------------------------------------------------------------------
// 3. CLI `openscarlett probe <exe|dir|xvd> [--json]` Verification
// ---------------------------------------------------------------------------
void TestOpenscarlettProbeCli(const fs::path& bin_dir, const fs::path& scratch) {
    const fs::path cli = bin_dir / "openscarlett.exe";
    OS_REQUIRE(fs::exists(cli));

    // 3a. Probe encrypted synthetic .xvd (human-readable + --json)
    const fs::path xvd_path = scratch / "probe_sample.xvd";
    {
        const auto bytes = BuildSyntheticEncryptedXvd();
        std::error_code ec;
        fs::create_directories(xvd_path.parent_path(), ec);
        std::ofstream ofs(xvd_path, std::ios::binary | std::ios::trunc);
        OS_REQUIRE(static_cast<bool>(ofs));
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    DWORD exit_code = 999;
    std::string out_txt;
    OS_REQUIRE(RunProcessCaptureStdout(cli.wstring(), L"probe \"" + xvd_path.wstring() + L"\"",
                                       exit_code, out_txt));
    OS_REQUIRE(exit_code == 0);
    OS_REQUIRE(out_txt.find("Preflight Compatibility Probe (XVD/XVC)") != std::string::npos);
    OS_REQUIRE(out_txt.find("ENCRYPTED") != std::string::npos);
    OS_REQUIRE(out_txt.find("unpacked package directory") != std::string::npos);

    std::string out_json;
    OS_REQUIRE(RunProcessCaptureStdout(cli.wstring(), L"probe \"" + xvd_path.wstring() + L"\" --json",
                                       exit_code, out_json));
    OS_REQUIRE(exit_code == 0);
    OS_REQUIRE(out_json.find("\"probe_kind\": \"xvd\"") != std::string::npos);
    OS_REQUIRE(out_json.find("\"is_encrypted\": true") != std::string::npos);
    OS_REQUIRE(out_json.find("\"ready_to_launch\": false") != std::string::npos);

    // 3b. Probe unpacked game directory with MicrosoftGame.Config + PE32+ executable
    const fs::path pkg_dir = scratch / "probe_pkg";
    std::error_code ec;
    fs::create_directories(pkg_dir, ec);
    fs::copy_file(cli, pkg_dir / "SampleGame.exe", fs::copy_options::overwrite_existing, ec);
    OS_REQUIRE(!ec);
    WriteTextFile(
        pkg_dir / "MicrosoftGame.Config",
        "<Game configVersion=\"1\">\n"
        "  <Identity Name=\"OpenScarlett.ProbeGame\" Publisher=\"CN=OpenScarlett\" Version=\"1.0.0.0\"/>\n"
        "  <TitleId>5CA07777</TitleId>\n"
        "  <StoreId>9PROBE000001</StoreId>\n"
        "  <ShellVisuals DefaultDisplayName=\"Probe Test Game\"/>\n"
        "  <ExecutableList><Executable Name=\"SampleGame.exe\" Id=\"Game\" TargetDeviceFamily=\"Scarlett\"/></ExecutableList>\n"
        "</Game>\n");

    OS_REQUIRE(RunProcessCaptureStdout(cli.wstring(), L"probe \"" + pkg_dir.wstring() + L"\"",
                                       exit_code, out_txt));
    OS_REQUIRE(exit_code == 0);
    OS_REQUIRE(out_txt.find("Preflight Compatibility Probe") != std::string::npos);
    OS_REQUIRE(out_txt.find("5CA07777") != std::string::npos);
    OS_REQUIRE(out_txt.find("OpenScarlett.ProbeGame_") != std::string::npos);

    OS_REQUIRE(RunProcessCaptureStdout(cli.wstring(), L"probe \"" + pkg_dir.wstring() + L"\" --json",
                                       exit_code, out_json));
    OS_REQUIRE(exit_code == 0);
    OS_REQUIRE(out_json.find("\"probe_kind\": \"package\"") != std::string::npos);
    OS_REQUIRE(out_json.find("\"has_game_config\": true") != std::string::npos);
    OS_REQUIRE(out_json.find("\"title_id\": \"5CA07777\"") != std::string::npos);
    OS_REQUIRE(out_json.find("\"shimmed_imports\"") != std::string::npos);
    OS_REQUIRE(out_json.find("\"missing_imports\"") != std::string::npos);

    // 3c. Probe direct .exe path
    OS_REQUIRE(RunProcessCaptureStdout(cli.wstring(),
                                       L"probe \"" + (pkg_dir / "SampleGame.exe").wstring() + L"\" --json",
                                       exit_code, out_json));
    OS_REQUIRE(exit_code == 0);
    OS_REQUIRE(out_json.find("\"forwarder_preflight\"") != std::string::npos);

    std::printf("[PASS] CLI openscarlett probe <exe|dir|xvd> [--json]\n");
}

// ---------------------------------------------------------------------------
// 4. GameInput: User-Before-Connect Ordering, Rumble + LT/RT Impulse Triggers,
//    and Keyboard/Mouse Virtual Gamepad Fallback
// ---------------------------------------------------------------------------
struct EventOrderLog {
    std::mutex mu;
    std::vector<std::string> events;
};

void OPENSCARLETT_GAMEINPUT_CALL OnUserAssocCb(
    void* ctx,
    const OsGameInputUserAssociationEvent* ev) {
    auto* log = static_cast<EventOrderLog*>(ctx);
    std::lock_guard<std::mutex> lk(log->mu);
    log->events.push_back("user_assoc:" + std::to_string(ev->newUserLocalId));
}

void OPENSCARLETT_GAMEINPUT_CALL OnDeviceStatusCb(
    uint64_t /*token*/,
    void* ctx,
    IGameInputDevice* /*dev*/,
    uint64_t /*ts*/,
    uint32_t curr,
    uint32_t /*prev*/) {
    auto* log = static_cast<EventOrderLog*>(ctx);
    std::lock_guard<std::mutex> lk(log->mu);
    if ((curr & GameInputDeviceConnected) != 0u) {
        log->events.push_back("device_connected");
    } else {
        log->events.push_back("device_disconnected");
    }
}

void TestGameInputS12() {
    IGameInput* gi = nullptr;
    OS_REQUIRE(GameInputCreate(&gi) == GAMEINPUT_S_OK && gi != nullptr);

    HMODULE gi_mod = ::GetModuleHandleW(L"GameInput.dll");
    OS_REQUIRE(gi_mod != nullptr);

    using PfnRegUserAssoc = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(
        IGameInput*, void*, OsGameInputUserAssociationCallback, uint64_t*);
    using PfnUnregUserAssoc = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(IGameInput*, uint64_t);
    using PfnGetDevUserAssoc = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(
        IGameInputDevice*, uint64_t*, uint64_t*, uint64_t*);
    using PfnRegDevCb = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(
        IGameInput*, IGameInputDevice*, GameInputKind, uint32_t, uint32_t, void*,
        OsGameInputDeviceCallback, uint64_t*);
    using PfnUnregDevCb = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(IGameInput*, uint64_t, uint64_t);
    using PfnHotPlug = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(
        IGameInput*, uint32_t, GameInputKind, int32_t, IGameInputDevice**);
    using PfnGetDevForKind = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(
        IGameInput*, GameInputKind, IGameInputDevice**);
    using PfnGetRumbleTel = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(IGameInputDevice*, void*);

    auto reg_ua = reinterpret_cast<PfnRegUserAssoc>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputRegisterUserAssociationCallback")));
    auto unreg_ua = reinterpret_cast<PfnUnregUserAssoc>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputUnregisterUserAssociationCallback")));
    auto get_dev_ua = reinterpret_cast<PfnGetDevUserAssoc>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputGetDeviceUserAssociation")));
    auto reg_dev = reinterpret_cast<PfnRegDevCb>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputRegisterDeviceCallback")));
    auto unreg_dev = reinterpret_cast<PfnUnregDevCb>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputUnregisterCallback")));
    auto hot_plug = reinterpret_cast<PfnHotPlug>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputSimulateDeviceHotPlug")));
    auto get_dev = reinterpret_cast<PfnGetDevForKind>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputGetDeviceForKind")));
    auto get_rumble = reinterpret_cast<PfnGetRumbleTel>(
        reinterpret_cast<void*>(::GetProcAddress(gi_mod, "OpenScarlettGameInputGetDeviceRumbleTelemetry")));
    OS_REQUIRE(reg_ua && unreg_ua && get_dev_ua && reg_dev && unreg_dev && hot_plug && get_dev && get_rumble);

    // 4a. Default gamepad must already have user association sequence STRICTLY BEFORE connect sequence!
    IGameInputDevice* default_pad = nullptr;
    OS_REQUIRE(get_dev(gi, GameInputKindGamepad, &default_pad) == GAMEINPUT_S_OK && default_pad != nullptr);
    uint64_t def_uid = 0, def_assoc_seq = 0, def_conn_seq = 0;
    OS_REQUIRE(get_dev_ua(default_pad, &def_uid, &def_assoc_seq, &def_conn_seq) == GAMEINPUT_S_OK);
    OS_REQUIRE(def_uid == 1ull);
    OS_REQUIRE(def_assoc_seq > 0ull);
    OS_REQUIRE(def_conn_seq > def_assoc_seq);

    // 4b. Hot-plug gamepad must fire user association callback BEFORE connected device callback!
    EventOrderLog order_log{};
    uint64_t ua_tok = 0, dev_tok = 0;
    OS_REQUIRE(reg_ua(gi, &order_log, &OnUserAssocCb, &ua_tok) == GAMEINPUT_S_OK);
    OS_REQUIRE(reg_dev(gi, nullptr, GameInputKindGamepad, GameInputDeviceConnected, 0u,
                       &order_log, &OnDeviceStatusCb, &dev_tok) == GAMEINPUT_S_OK);
    IGameInputDevice* hot_pad = nullptr;
    OS_REQUIRE(hot_plug(gi, 5u, GameInputKindGamepad, 1, &hot_pad) == GAMEINPUT_S_OK && hot_pad != nullptr);
    OS_REQUIRE(order_log.events.size() >= 2u);
    OS_REQUIRE(order_log.events[0].rfind("user_assoc:", 0) == 0);
    OS_REQUIRE(order_log.events[1] == "device_connected");

    uint64_t hot_uid = 0, hot_assoc_seq = 0, hot_conn_seq = 0;
    OS_REQUIRE(get_dev_ua(hot_pad, &hot_uid, &hot_assoc_seq, &hot_conn_seq) == GAMEINPUT_S_OK);
    OS_REQUIRE(hot_uid != 0ull && hot_conn_seq > hot_assoc_seq);
    hot_pad->Release();
    OS_REQUIRE(unreg_ua(gi, ua_tok) == GAMEINPUT_S_OK);
    OS_REQUIRE(unreg_dev(gi, dev_tok, 0) == GAMEINPUT_S_OK);

    // 4c. Rumble motors + LT/RT impulse triggers (with clamping & NaN safety, safe no-op without physical pad)
    GameInputRumbleParams rp{};
    rp.lowFrequency = 0.5f;
    rp.highFrequency = 1.5f;  // clamps to 1.0
    rp.leftTrigger = 0.8f;
    rp.rightTrigger = -0.2f;  // clamps to 0.0
    default_pad->SetRumbleState(&rp);
    OsGameInputRumbleTelemetry rtel{};
    OS_REQUIRE(get_rumble(default_pad, &rtel) == GAMEINPUT_S_OK);
    OS_REQUIRE(std::fabs(rtel.lastParams.lowFrequency - 0.5f) < 1e-4f);
    OS_REQUIRE(std::fabs(rtel.lastParams.highFrequency - 1.0f) < 1e-4f);
    OS_REQUIRE(std::fabs(rtel.lastParams.leftTrigger - 0.8f) < 1e-4f);
    OS_REQUIRE(std::fabs(rtel.lastParams.rightTrigger - 0.0f) < 1e-4f);
    OS_REQUIRE(rtel.xinputLeftMotor > 0u && rtel.xinputRightMotor == 65535u);
    OS_REQUIRE(rtel.impulseTriggerUpdateCount >= 1ull);
    default_pad->SetRumbleState(nullptr);  // Zero motors safely
    default_pad->Release();

    // 4d. Keyboard + mouse virtual gamepad fallback (gp == nullptr -> synthesized from keys + mouse!)
    GameInputKeyState keys[3]{};
    keys[0].virtualKey = 'W';
    keys[1].virtualKey = VK_SPACE;
    keys[2].virtualKey = VK_UP;
    OS_REQUIRE(OpenScarlettGameInputInjectTestState(gi, nullptr, keys, 3u, 1) == GAMEINPUT_S_OK);
    OS_REQUIRE(OpenScarlettGameInputInjectRawMouseDelta(
                   gi, 10, -5, 0, 0, GameInputMouseLeftButton | GameInputMouseRightButton) == GAMEINPUT_S_OK);

    IGameInputReading* reading = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindGamepad, nullptr, &reading) == GAMEINPUT_S_OK && reading);
    GameInputGamepadState gp{};
    OS_REQUIRE(reading->GetGamepadState(&gp));
    OS_REQUIRE(gp.leftThumbstickY > 0.9f);
    OS_REQUIRE((gp.buttons & GameInputGamepadA) != 0u);
    OS_REQUIRE((gp.buttons & GameInputGamepadDPadUp) != 0u);
    OS_REQUIRE(gp.rightTrigger > 0.9f);   // LMB -> RT
    OS_REQUIRE(gp.leftTrigger > 0.9f);    // RMB -> LT
    OS_REQUIRE(gp.rightThumbstickX > 0.1f && gp.rightThumbstickY > 0.1f);  // Mouse look -> right stick
    reading->Release();

    OS_REQUIRE(OpenScarlettGameInputInjectTestState(gi, nullptr, nullptr, 0u, 0) == GAMEINPUT_S_OK);
    gi->Release();
    std::printf("[PASS] GameInput user-before-connect, rumble+triggers & KB/mouse virtual pad\n");
}

// ---------------------------------------------------------------------------
// 5. xgameruntime: ConsoleLsaPolicy init, semicolon language selectors,
//    XStoreCanAcquireLicense offline semantics, XUserIsStoreUser & strict validation
// ---------------------------------------------------------------------------
struct ChunkEnumResult {
    std::vector<std::pair<std::string, XPackageChunkAvailability>> entries;
};

bool OPENSCARLETT_GDK_CALL OnChunkAvailCb(
    void* ctx,
    const XPackageChunkSelector* sel,
    XPackageChunkAvailability avail) {
    auto* r = static_cast<ChunkEnumResult*>(ctx);
    if (sel && sel->type == XPackageChunkSelectorType_Language && sel->language) {
        r->entries.emplace_back(sel->language, avail);
    }
    return true;
}

void OPENSCARLETT_GDK_CALL DummyChangeCb(void*, XUserLocalId, XUserChangeEvent) {}

void TestXGameRuntimeS12(const fs::path& scratch) {
    const fs::path cfg_path = scratch / "xgr_cfg" / "MicrosoftGame.Config";
    WriteTextFile(
        cfg_path,
        "<Game configVersion=\"1\">\n"
        "  <Identity Name=\"OpenScarlett.XgrS12\" Publisher=\"CN=OpenScarlett\" Version=\"2.0.0.0\"/>\n"
        "  <TitleId>5CA08888</TitleId>\n"
        "  <StoreId>9XGRS1200001</StoreId>\n"
        "  <ShellVisuals DefaultDisplayName=\"XGR S12 Title\"/>\n"
        "</Game>\n");

    ::SetEnvironmentVariableA("OPENSCARLETT_GAME_CONFIG", cfg_path.string().c_str());
    ::SetEnvironmentVariableA("OPENSCARLETT_LOCALE", "en-US");
    ::SetEnvironmentVariableA("OPENSCARLETT_STORE_ADDON_IDS", "9DLCADDON001");

    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // 5a. Verify xgameruntime's ConsoleLsaPolicy was populated on XGameRuntimeInitialize
    HMODULE xgr_mod = ::GetModuleHandleW(L"xgameruntime.dll");
    OS_REQUIRE(xgr_mod != nullptr);
    using PfnGetLsa = XGameResult(OPENSCARLETT_GDK_CALL*)(char*, size_t, uint32_t*);
    auto get_lsa = reinterpret_cast<PfnGetLsa>(
        reinterpret_cast<void*>(::GetProcAddress(xgr_mod, "XgrGetConsoleLsaIdentityForTest")));
    OS_REQUIRE(get_lsa != nullptr);
    char pfn_buf[256]{};
    uint32_t tid = 0u;
    OS_REQUIRE(get_lsa(pfn_buf, sizeof(pfn_buf), &tid) == XGR_S_OK);
    OS_REQUIRE(tid == 0x5CA08888u);
    OS_REQUIRE(std::string(pfn_buf).rfind("OpenScarlett.XgrS12_", 0) == 0);

    // 5b. XPackage* semicolon-joined language selectors ("fr-FR;en-US;de-DE" vs "ja-JP;ko-KR")
    char pkg_id[256]{};
    OS_REQUIRE(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id) == XGR_S_OK);

    XPackageChunkSelector sel_match{};
    sel_match.type = XPackageChunkSelectorType_Language;
    sel_match.language = "fr-FR;en-US;de-DE";
    XPackageChunkAvailability avail = XPackageChunkAvailability_Unavailable;
    OS_REQUIRE(XPackageFindChunkAvailability(pkg_id, 1u, &sel_match, &avail) == XGR_S_OK);
    OS_REQUIRE(avail == XPackageChunkAvailability_Ready);

    ChunkEnumResult enum_res{};
    OS_REQUIRE(XPackageEnumerateChunkAvailability(pkg_id, 1u, &sel_match, &enum_res, &OnChunkAvailCb) == XGR_S_OK);
    OS_REQUIRE(enum_res.entries.size() == 3u);
    OS_REQUIRE(enum_res.entries[0].first == "fr-FR" && enum_res.entries[0].second == XPackageChunkAvailability_Unavailable);
    OS_REQUIRE(enum_res.entries[1].first == "en-US" && enum_res.entries[1].second == XPackageChunkAvailability_Ready);
    OS_REQUIRE(enum_res.entries[2].first == "de-DE" && enum_res.entries[2].second == XPackageChunkAvailability_Unavailable);

    XPackageChunkSelector sel_miss{};
    sel_miss.type = XPackageChunkSelectorType_Language;
    sel_miss.language = "ja-JP;ko-KR";
    OS_REQUIRE(XPackageFindChunkAvailability(pkg_id, 1u, &sel_miss, &avail) == XGR_S_OK);
    OS_REQUIRE(avail == XPackageChunkAvailability_Unavailable);

    // 5c. XUserIsStoreUser == true for valid user, false for null/bogus, plus strict change-event validation
    using PfnIsStoreUser = bool(OPENSCARLETT_GDK_CALL*)(XUserHandle);
    auto is_store_user = reinterpret_cast<PfnIsStoreUser>(
        reinterpret_cast<void*>(::GetProcAddress(xgr_mod, "XUserIsStoreUser")));
    OS_REQUIRE(is_store_user != nullptr);
    OS_REQUIRE(!is_store_user(nullptr));

    XAsyncBlock user_ab{};
    OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) == XGR_S_OK);
    XUserHandle user = nullptr;
    OS_REQUIRE(XUserAddResult(&user_ab, &user) == XGR_S_OK && user != nullptr);
    OS_REQUIRE(is_store_user(user));

    XTaskQueueRegistrationToken tok{999ull};
    OS_REQUIRE(XUserRegisterForChangeEvent(nullptr, nullptr, nullptr, &tok) == XGR_E_INVALIDARG);
    OS_REQUIRE(tok.token == 0ull);
    OS_REQUIRE(XUserRegisterForChangeEvent(nullptr, nullptr, &DummyChangeCb, nullptr) == XGR_E_INVALIDARG);
    OS_REQUIRE(XUserRegisterForChangeEvent(nullptr, nullptr, &DummyChangeCb, &tok) == XGR_S_OK);
    OS_REQUIRE(tok.token != 0ull);
    OS_REQUIRE(XUserUnregisterForChangeEvent(tok, false));

    // 5d. XStoreCanAcquireLicense* offline semantics (owned vs configured addon vs unowned vs invalid)
    XStoreContextHandle sctx = nullptr;
    OS_REQUIRE(XStoreCreateContext(user, &sctx) == XGR_S_OK && sctx != nullptr);

    XAsyncBlock ab_owned_pkg{};
    OS_REQUIRE(XStoreCanAcquireLicenseForPackageAsync(sctx, pkg_id, &ab_owned_pkg) == XGR_S_OK);
    XStoreCanAcquireLicenseResult res_owned_pkg{};
    OS_REQUIRE(XStoreCanAcquireLicenseForPackageResult(&ab_owned_pkg, &res_owned_pkg) == XGR_S_OK);
    OS_REQUIRE(res_owned_pkg.status == XStoreCanLicenseStatus_Licensable);
    OS_REQUIRE(std::string(res_owned_pkg.licensableSku) == "0001");

    XAsyncBlock ab_unowned_pkg{};
    OS_REQUIRE(XStoreCanAcquireLicenseForPackageAsync(sctx, "Unowned.OtherPkg_1.0.0.0_x64__other", &ab_unowned_pkg) == XGR_S_OK);
    XStoreCanAcquireLicenseResult res_unowned_pkg{};
    OS_REQUIRE(XStoreCanAcquireLicenseForPackageResult(&ab_unowned_pkg, &res_unowned_pkg) == XGR_S_OK);
    OS_REQUIRE(res_unowned_pkg.status == XStoreCanLicenseStatus_NotLicensableToUser);

    XAsyncBlock ab_addon{};
    OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdAsync(sctx, "9DLCADDON001", &ab_addon) == XGR_S_OK);
    XStoreCanAcquireLicenseResult res_addon{};
    OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdResult(&ab_addon, &res_addon) == XGR_S_OK);
    OS_REQUIRE(res_addon.status == XStoreCanLicenseStatus_Licensable);

    XAsyncBlock ab_bad{};
    OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdAsync(sctx, "   ", &ab_bad) == XGR_E_INVALIDARG);
    OS_REQUIRE(XStoreCanAcquireLicenseForPackageAsync(nullptr, pkg_id, &ab_bad) == XGR_E_INVALIDARG);

    XStoreCloseContextHandle(sctx);
    XUserCloseHandle(user);
    XGameRuntimeUninitialize();
    ::SetEnvironmentVariableA("OPENSCARLETT_GAME_CONFIG", nullptr);
    ::SetEnvironmentVariableA("OPENSCARLETT_STORE_ADDON_IDS", nullptr);

    std::printf("[PASS] xgameruntime LSA init, semicolon language selectors, XStore offline & XUserIsStoreUser\n");
}

// ---------------------------------------------------------------------------
// 6. XAudio2 Lock-Free 128-Voice Concurrent Stress + ISpatialAudioClient Stability
// ---------------------------------------------------------------------------
void TestXAudio2LockFree128VoicesAndSpatialAudio() {
    ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", "1");

    IXAudio2* xa2 = nullptr;
    OS_REQUIRE(XAudio2Create(&xa2, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa2 != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa2->CreateMasteringVoice(&master, 2u, 48000u) == OS_XAUDIO2_S_OK && master != nullptr);

    OS_WAVEFORMATEX fmt{};
    fmt.wFormatTag = OS_WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 1u;
    fmt.nSamplesPerSec = 48000u;
    fmt.wBitsPerSample = 32u;
    fmt.nBlockAlign = 4u;
    fmt.nAvgBytesPerSec = 48000u * 4u;

    constexpr uint32_t kVoiceCount = 128u;
    std::vector<IXAudio2SourceVoice*> voices(kVoiceCount, nullptr);
    for (uint32_t i = 0u; i < kVoiceCount; ++i) {
        OS_REQUIRE(xa2->CreateSourceVoice(&voices[i], &fmt) == OS_XAUDIO2_S_OK && voices[i] != nullptr);
        OS_REQUIRE(voices[i]->SetVolume(1.0f / static_cast<float>(kVoiceCount)) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(voices[i]->Start() == OS_XAUDIO2_S_OK);
    }

    // Prepare deterministic DC pulse buffer (amplitude 0.5f across 256 frames)
    std::vector<float> pcm(256u, 0.5f);

    // Multi-threaded concurrent submission across 128 voices while mixer thread renders concurrently!
    std::atomic<bool> stop_mixer{false};
    std::atomic<uint64_t> mix_passes{0ull};
    std::atomic<bool> saw_nonzero_mix{false};

    std::thread mixer_thread([&]() {
        std::vector<float> out(128u * 2u, 0.0f);
        while (!stop_mixer.load(std::memory_order_acquire)) {
            if (OpenScarlettXAudio2RenderMixForTest(xa2, out.data(), 128u) == OS_XAUDIO2_S_OK) {
                mix_passes.fetch_add(1ull, std::memory_order_relaxed);
                for (float s : out) {
                    if (std::fabs(s) > 1e-4f) {
                        saw_nonzero_mix.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        }
    });

    constexpr uint32_t kSubmitThreads = 8u;
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitThreads);
    for (uint32_t t = 0u; t < kSubmitThreads; ++t) {
        submitters.emplace_back([&, t]() {
            const uint32_t begin = t * (kVoiceCount / kSubmitThreads);
            const uint32_t end = begin + (kVoiceCount / kSubmitThreads);
            for (int round = 0; round < 12; ++round) {
                for (uint32_t v = begin; v < end; ++v) {
                    OS_XAUDIO2_BUFFER buf{};
                    buf.AudioBytes = static_cast<uint32_t>(pcm.size() * sizeof(float));
                    buf.pAudioData = reinterpret_cast<const uint8_t*>(pcm.data());
                    buf.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(v + 1u));
                    (void)voices[v]->SubmitSourceBuffer(&buf);
                    OS_XAUDIO2_VOICE_STATE st{};
                    voices[v]->GetState(&st);
                }
            }
        });
    }
    for (auto& th : submitters) {
        th.join();
    }

    // Run a few more mix passes to drain & guarantee non-zero signal
    while (mix_passes.load(std::memory_order_relaxed) < 16ull) {
        std::this_thread::yield();
    }
    stop_mixer.store(true, std::memory_order_release);
    mixer_thread.join();

    OS_REQUIRE(saw_nonzero_mix.load(std::memory_order_relaxed));

    HMODULE xa2_mod = ::GetModuleHandleW(L"XAudio2_9.dll");
    OS_REQUIRE(xa2_mod != nullptr);
    using PfnGetLfStats = int32_t(OPENSCARLETT_XAUDIO2_CALL*)(IXAudio2*, uint32_t*, uint32_t*, uint64_t*);
    auto get_lf_stats = reinterpret_cast<PfnGetLfStats>(
        reinterpret_cast<void*>(::GetProcAddress(xa2_mod, "OpenScarlettXAudio2GetLockFreeMixerStats")));
    OS_REQUIRE(get_lf_stats != nullptr);
    uint32_t cap = 0u, peak = 0u;
    uint64_t lf_passes = 0ull;
    OS_REQUIRE(get_lf_stats(xa2, &cap, &peak, &lf_passes) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(cap >= 128u);
    OS_REQUIRE(peak >= 128u);
    OS_REQUIRE(lf_passes >= 16ull);

    for (auto* v : voices) {
        v->DestroyVoice();
    }
    master->DestroyVoice();
    xa2->Release();

    // 6b. ISpatialAudioClient concurrent streaming thread stability & re-entrancy guards
    using PfnCreateSpatial = int32_t(OPENSCARLETT_XAUDIO2_CALL*)(void**);
    auto create_spatial = reinterpret_cast<PfnCreateSpatial>(
        reinterpret_cast<void*>(::GetProcAddress(xa2_mod, "OpenScarlettCreateSpatialAudioClient")));
    OS_REQUIRE(create_spatial != nullptr);

    OS_ISpatialAudioClient* sp_client = nullptr;
    OS_REQUIRE(create_spatial(reinterpret_cast<void**>(&sp_client)) == OS_XAUDIO2_S_OK && sp_client != nullptr);
    uint32_t max_dyn = 0u;
    OS_REQUIRE(sp_client->GetMaxDynamicObjectCount(&max_dyn) == OS_XAUDIO2_S_OK && max_dyn >= 128u);
    OS_REQUIRE(sp_client->IsAudioObjectFormatSupported(&fmt) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(sp_client->IsAudioObjectFormatSupported(nullptr) < 0);

    OS_SpatialAudioObjectRenderStreamActivationParams act{};
    act.ObjectFormat = &fmt;
    act.StaticObjectTypeMask = 0x3Fu;
    act.MaxDynamicObjectCount = 128u;
    OS_ISpatialAudioObjectRenderStream* sp_stream = nullptr;
    OS_REQUIRE(sp_client->ActivateSpatialAudioStream(
                   &act, &kIidSpatialAudioRenderStream, reinterpret_cast<void**>(&sp_stream)) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(sp_stream != nullptr);
    OS_REQUIRE(sp_stream->Start() == OS_XAUDIO2_S_OK);

    // Re-entrancy guard check: nested BeginUpdatingAudioObjects must fail cleanly
    uint32_t avail_dyn = 0u, frames_per_buf = 0u;
    OS_REQUIRE(sp_stream->BeginUpdatingAudioObjects(&avail_dyn, &frames_per_buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(sp_stream->BeginUpdatingAudioObjects(&avail_dyn, &frames_per_buf) < 0);
    OS_REQUIRE(sp_stream->EndUpdatingAudioObjects() == OS_XAUDIO2_S_OK);
    OS_REQUIRE(sp_stream->EndUpdatingAudioObjects() < 0);

    // Heavy concurrent streaming thread stress: 8 threads contending on Begin/Activate/GetBuffer/End
    std::atomic<uint32_t> completed_cycles{0u};
    std::vector<std::thread> sp_threads;
    for (int t = 0; t < 8; ++t) {
        sp_threads.emplace_back([&, t]() {
            for (int iter = 0; iter < 40; ++iter) {
                uint32_t dyn = 0u, fpb = 0u;
                if (sp_stream->BeginUpdatingAudioObjects(&dyn, &fpb) == OS_XAUDIO2_S_OK) {
                    OS_ISpatialAudioObject* obj = nullptr;
                    if (sp_stream->ActivateSpatialAudioObject(1u, &obj) == OS_XAUDIO2_S_OK && obj) {
                        uint8_t* ptr = nullptr;
                        uint32_t len = 0u;
                        if (obj->GetBuffer(&ptr, &len) == OS_XAUDIO2_S_OK && ptr && len > 0u) {
                            std::memset(ptr, 0, len);
                        }
                        (void)obj->SetPosition(static_cast<float>(t), 0.0f, 1.0f);
                        (void)obj->SetVolume(0.75f);
                        (void)obj->SetEndOfStream(fpb);
                        obj->Release();
                    }
                    (void)sp_stream->EndUpdatingAudioObjects();
                    completed_cycles.fetch_add(1u, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& th : sp_threads) {
        th.join();
    }
    OS_REQUIRE(completed_cycles.load(std::memory_order_relaxed) >= 40u);
    OS_REQUIRE(sp_stream->Stop() == OS_XAUDIO2_S_OK);
    OS_REQUIRE(sp_stream->Reset() == OS_XAUDIO2_S_OK);
    sp_stream->Release();
    sp_client->Release();

    std::printf("[PASS] XAudio2 lock-free 128-voice concurrent stress & ISpatialAudioClient stability\n");
}

}  // namespace

int main() {
    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path scratch = ResolveScratchRoot();
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);

    TestNestedVfsAndNtObjectPaths(scratch);
    TestHookDllTlsNtForwarderLsa(bin_dir, scratch);
    TestOpenscarlettProbeCli(bin_dir, scratch);
    TestGameInputS12();
    TestXGameRuntimeS12(scratch);
    TestXAudio2LockFree128VoicesAndSpatialAudio();

    fs::remove_all(scratch, ec);
    std::printf("ALL SPRINT 12 OS/LOADER/TLS/VFS/GAMEINPUT/XGR/XAUDIO2 TESTS PASSED!\n");
    return 0;
}
