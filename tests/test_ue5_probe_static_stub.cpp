#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "openscarlett/launcher/hwtest_runner.h"
#include "openscarlett/loader/apc_injector.h"
#include "openscarlett/loader/pe_inspector.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#define OS_REQUIRE(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "[FAIL] %s:%d: requirement failed: %s\n", __FILE__,          \
                         __LINE__, #cond);                                                    \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

uint32_t AlignUp(uint32_t val, uint32_t align) noexcept {
    return (val + align - 1u) & ~(align - 1u);
}

#if defined(_WIN32)
struct ChildRunResult {
    bool launched = false;
    DWORD exit_code = 0xFFFFFFFFu;
    std::string output;
};

ChildRunResult RunProcessCaptureOutput(const fs::path& exe_path,
                                       const std::wstring& cmd_line,
                                       const fs::path& work_dir,
                                       DWORD timeout_ms = 20000u) {
    ChildRunResult res{};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_read = nullptr;
    HANDLE h_write = nullptr;
    if (!::CreatePipe(&h_read, &h_write, &sa, 0)) {
        return res;
    }
    ::SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_write;
    si.hStdError = h_write;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        &si,
        &pi);

    ::CloseHandle(h_write);
    if (!ok) {
        ::CloseHandle(h_read);
        return res;
    }
    res.launched = true;

    char buf[1024]{};
    DWORD nread = 0;
    while (::ReadFile(h_read, buf, sizeof(buf), &nread, nullptr) && nread > 0) {
        res.output.append(buf, nread);
    }
    ::CloseHandle(h_read);

    const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 124u);
        ::WaitForSingleObject(pi.hProcess, 2000u);
        res.exit_code = 124u;
    } else {
        DWORD ec = 0;
        if (::GetExitCodeProcess(pi.hProcess, &ec)) {
            res.exit_code = ec;
        }
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return res;
}

// Builds a minimal, self-contained PE32+ (x86-64) executable that statically imports:
//   - fake_unshimmed_xs.dll: ZetaXboxShutdown, FakeXboxInit, AlphaXboxBoot (named, unsorted)
//                            + ordinal @7 + ordinal @19
//   - kernel32.dll:          ExitProcess
// At runtime it calls all 5 imports from fake_unshimmed_xs.dll and exits with code 0
// ONLY if every single one returns 0x80004001 (E_NOTIMPL).
bool WriteSyntheticGuestExe(const fs::path& out_path,
                            const char* missing_dll_name) {
    constexpr uint32_t kFileAlign = 0x200u;
    constexpr uint32_t kSectAlign = 0x1000u;
    constexpr uint32_t kSizeOfHeaders = 0x200u;
    constexpr uint32_t kTextVAddr = 0x1000u;
    constexpr uint32_t kIdataVAddr = 0x2000u;

    // Layout inside .idata (RVA 0x2000):
    //   0x000..0x02F: IAT for missing_dll_name (5 entries + null = 6 * 8 = 48 bytes)
    //   0x030..0x03F: IAT for kernel32.dll     (1 entry + null   = 2 * 8 = 16 bytes)
    //   0x040..0x06F: INT for missing_dll_name (6 * 8 = 48 bytes)
    //   0x070..0x07F: INT for kernel32.dll     (2 * 8 = 16 bytes)
    //   0x080..0x0BB: IMAGE_IMPORT_DESCRIPTOR[3] (3 * 20 = 60 bytes)
    //   0x0C0..     : strings & IMAGE_IMPORT_BY_NAME entries
    constexpr uint32_t kIatMissingOff = 0x000u;
    constexpr uint32_t kIatK32Off = 0x030u;
    constexpr uint32_t kIntMissingOff = 0x040u;
    constexpr uint32_t kIntK32Off = 0x070u;
    constexpr uint32_t kDescOff = 0x080u;
    constexpr uint32_t kStrStartOff = 0x0C0u;

    std::vector<uint8_t> idata(kStrStartOff, 0u);
    auto append_cstr = [&](const std::string& s) -> uint32_t {
        if ((idata.size() & 1u) != 0u) idata.push_back(0u);
        const uint32_t rva = kIdataVAddr + static_cast<uint32_t>(idata.size());
        idata.insert(idata.end(), s.begin(), s.end());
        idata.push_back(0u);
        return rva;
    };
    auto append_ibn = [&](const std::string& sym) -> uint32_t {
        if ((idata.size() & 1u) != 0u) idata.push_back(0u);
        const uint32_t rva = kIdataVAddr + static_cast<uint32_t>(idata.size());
        idata.push_back(0u);
        idata.push_back(0u);  // Hint = 0
        idata.insert(idata.end(), sym.begin(), sym.end());
        idata.push_back(0u);
        return rva;
    };

    const uint32_t rva_dll_missing = append_cstr(missing_dll_name);
    const uint32_t rva_dll_k32 = append_cstr("kernel32.dll");
    const uint32_t rva_ibn_zeta = append_ibn("ZetaXboxShutdown");
    const uint32_t rva_ibn_fake = append_ibn("FakeXboxInit");
    const uint32_t rva_ibn_alpha = append_ibn("AlphaXboxBoot");
    const uint32_t rva_ibn_exit = append_ibn("ExitProcess");

    const uint64_t missing_thunks[6] = {
        static_cast<uint64_t>(rva_ibn_zeta),
        static_cast<uint64_t>(rva_ibn_fake),
        static_cast<uint64_t>(rva_ibn_alpha),
        IMAGE_ORDINAL_FLAG64 | 7ull,
        IMAGE_ORDINAL_FLAG64 | 19ull,
        0ull,
    };
    const uint64_t k32_thunks[2] = {
        static_cast<uint64_t>(rva_ibn_exit),
        0ull,
    };
    std::memcpy(idata.data() + kIatMissingOff, missing_thunks, sizeof(missing_thunks));
    std::memcpy(idata.data() + kIntMissingOff, missing_thunks, sizeof(missing_thunks));
    std::memcpy(idata.data() + kIatK32Off, k32_thunks, sizeof(k32_thunks));
    std::memcpy(idata.data() + kIntK32Off, k32_thunks, sizeof(k32_thunks));

    IMAGE_IMPORT_DESCRIPTOR descs[3]{};
    descs[0].OriginalFirstThunk = kIdataVAddr + kIntMissingOff;
    descs[0].Name = rva_dll_missing;
    descs[0].FirstThunk = kIdataVAddr + kIatMissingOff;
    descs[1].OriginalFirstThunk = kIdataVAddr + kIntK32Off;
    descs[1].Name = rva_dll_k32;
    descs[1].FirstThunk = kIdataVAddr + kIatK32Off;
    std::memcpy(idata.data() + kDescOff, descs, sizeof(descs));

    const uint32_t idata_vsize = static_cast<uint32_t>(idata.size());
    const uint32_t idata_raw_size = AlignUp(idata_vsize, kFileAlign);
    idata.resize(idata_raw_size, 0u);

    // Emit RIP-relative x64 machine code calling all 5 missing_dll imports and verifying E_NOTIMPL.
    std::vector<uint8_t> code;
    auto cur_rva = [&]() -> uint32_t {
        return kTextVAddr + static_cast<uint32_t>(code.size());
    };
    auto emit = [&](std::initializer_list<uint8_t> bs) {
        code.insert(code.end(), bs.begin(), bs.end());
    };
    auto emit_u32 = [&](uint32_t v) {
        const size_t pos = code.size();
        code.resize(pos + 4u);
        std::memcpy(code.data() + pos, &v, 4u);
    };
    auto emit_call_iat = [&](uint32_t iat_rva) {
        emit({0xFF, 0x15});  // call qword ptr [rip + disp32]
        const uint32_t next_rip = cur_rva() + 4u;
        const int32_t disp = static_cast<int32_t>(iat_rva) - static_cast<int32_t>(next_rip);
        emit_u32(static_cast<uint32_t>(disp));
    };

    // Entry point: sub rsp, 0x28
    emit({0x48, 0x83, 0xEC, 0x28});

    std::vector<size_t> fail_patch_offsets;
    for (uint32_t slot = 0; slot < 5u; ++slot) {
        // mov ecx, 0x100 + slot
        emit({0xB9});
        emit_u32(0x100u + slot);
        // call [IAT_missing[slot]]
        emit_call_iat(kIdataVAddr + kIatMissingOff + slot * 8u);
        // cmp eax, 0x80004001 (E_NOTIMPL)
        emit({0x3D, 0x01, 0x40, 0x00, 0x80});
        // jne fail_slot (0F 85 rel32)
        emit({0x0F, 0x85});
        fail_patch_offsets.push_back(code.size());
        emit_u32(0u);
    }

    // All 5 returned E_NOTIMPL -> ExitProcess(0)
    emit({0x31, 0xC9});  // xor ecx, ecx
    emit_call_iat(kIdataVAddr + kIatK32Off);
    emit({0xCC});

    // Per-slot failure handlers: ExitProcess(10 + slot)
    for (uint32_t slot = 0; slot < 5u; ++slot) {
        const uint32_t label_rva = cur_rva();
        const size_t patch_off = fail_patch_offsets[slot];
        const uint32_t next_rip = kTextVAddr + static_cast<uint32_t>(patch_off) + 4u;
        const int32_t rel = static_cast<int32_t>(label_rva) - static_cast<int32_t>(next_rip);
        std::memcpy(code.data() + patch_off, &rel, 4u);

        emit({0xB9});
        emit_u32(10u + slot);
        emit_call_iat(kIdataVAddr + kIatK32Off);
        emit({0xCC});
    }

    const uint32_t text_vsize = static_cast<uint32_t>(code.size());
    const uint32_t text_raw_size = AlignUp(text_vsize, kFileAlign);
    code.resize(text_raw_size, 0xCCu);

    const uint32_t size_of_image = kIdataVAddr + AlignUp(idata_vsize, kSectAlign);
    const uint32_t total_size = kSizeOfHeaders + text_raw_size + idata_raw_size;
    std::vector<uint8_t> image(total_size, 0u);

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
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;

    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.MajorLinkerVersion = 14u;
    nt.OptionalHeader.SizeOfCode = text_raw_size;
    nt.OptionalHeader.SizeOfInitializedData = idata_raw_size;
    nt.OptionalHeader.AddressOfEntryPoint = kTextVAddr;
    nt.OptionalHeader.BaseOfCode = kTextVAddr;
    nt.OptionalHeader.ImageBase = 0x0000000140000000ull;
    nt.OptionalHeader.SectionAlignment = kSectAlign;
    nt.OptionalHeader.FileAlignment = kFileAlign;
    nt.OptionalHeader.MajorOperatingSystemVersion = 6u;
    nt.OptionalHeader.MajorSubsystemVersion = 6u;
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
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = kIdataVAddr + kDescOff;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(descs);
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = kIdataVAddr + kIatMissingOff;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = 0x40u;

    const size_t nt_off = static_cast<size_t>(dos.e_lfanew);
    std::memcpy(image.data() + nt_off, &nt, sizeof(nt));

    IMAGE_SECTION_HEADER sec_text{};
    std::memcpy(sec_text.Name, ".text", 5u);
    sec_text.Misc.VirtualSize = text_vsize;
    sec_text.VirtualAddress = kTextVAddr;
    sec_text.SizeOfRawData = text_raw_size;
    sec_text.PointerToRawData = kSizeOfHeaders;
    sec_text.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    IMAGE_SECTION_HEADER sec_idata{};
    std::memcpy(sec_idata.Name, ".idata", 6u);
    sec_idata.Misc.VirtualSize = idata_vsize;
    sec_idata.VirtualAddress = kIdataVAddr;
    sec_idata.SizeOfRawData = idata_raw_size;
    sec_idata.PointerToRawData = kSizeOfHeaders + text_raw_size;
    sec_idata.Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    const size_t sec_off = nt_off + sizeof(IMAGE_NT_HEADERS64);
    std::memcpy(image.data() + sec_off, &sec_text, sizeof(sec_text));
    std::memcpy(image.data() + sec_off + sizeof(IMAGE_SECTION_HEADER), &sec_idata, sizeof(sec_idata));

    std::memcpy(image.data() + kSizeOfHeaders, code.data(), code.size());
    std::memcpy(image.data() + kSizeOfHeaders + text_raw_size, idata.data(), idata.size());

    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return ofs.good();
}

void Test1_DxcompilerXsClassificationAndRouting(const fs::path& bin_dir, const fs::path& temp_dir) {
    using namespace openscarlett;

    // Case-insensitive classification
    OS_REQUIRE(ClassifyImportDll("dxcompiler_xs.dll") == DllCategory::ConsoleExclusive);
    OS_REQUIRE(ClassifyImportDll("DXCOMPILER_XS.DLL") == DllCategory::ConsoleExclusive);
    OS_REQUIRE(ClassifyImportDll("C:\\Xbox\\DxCompiler_Xs.dll") == DllCategory::ConsoleExclusive);

    // Routing table lookup & legacy rule count preservation (includes xapu.dll from main)
    DllRoutingTable table;
    OS_REQUIRE(table.ListRules().size() >= 19u);
    OS_REQUIRE(table.ListAllRules().size() >= 24u);
    OS_REQUIRE(table.ListCompleteRules().size() >= 30u);

    const DllRouteRule* rule = table.FindRule("dxcompiler_xs.dll");
    OS_REQUIRE(rule != nullptr);
    OS_REQUIRE(rule->policy == DllRoutePolicy::Shim);
    OS_REQUIRE(rule->canonical_name == "dxcompiler_xs.dll");

    const DllRouteDecision decision = table.ResolveRoute("DXCOMPILER_XS.DLL", bin_dir.wstring());
    OS_REQUIRE(decision.policy == DllRoutePolicy::Shim);
    OS_REQUIRE(decision.canonical_name == "dxcompiler_xs.dll");

    // End-to-end PE inspection + Launcher Binary Inspector routing status verification
    const fs::path dxc_guest_pe = temp_dir / "synthetic_dxc_importer.exe";
    OS_REQUIRE(WriteSyntheticGuestExe(dxc_guest_pe, "dxcompiler_xs.dll"));

    PeInspectionReport pe_rep{};
    OS_REQUIRE(InspectPeFile(dxc_guest_pe.string(), pe_rep).ok());
    OS_REQUIRE(pe_rep.console_imports >= 5u);
    bool found_dxc_mod = false;
    for (const auto& mod : pe_rep.imports) {
        if (mod.dll_name == "dxcompiler_xs.dll") {
            OS_REQUIRE(mod.category == DllCategory::ConsoleExclusive);
            found_dxc_mod = true;
        }
    }
    OS_REQUIRE(found_dxc_mod);

    const auto launcher_sum = openscarlett::launcher::InspectBinaryForLauncher(dxc_guest_pe, bin_dir);
    OS_REQUIRE(launcher_sum.ok);
    bool found_launcher_dxc = false;
    for (const auto& row : launcher_sum.modules) {
        if (row.dll_name == "dxcompiler_xs.dll") {
            OS_REQUIRE(row.category == "ConsoleExclusive");
            OS_REQUIRE(row.routing_status == "OpenScarlett Shim (dxcompiler_xs.dll)");
            found_launcher_dxc = true;
        }
    }
    OS_REQUIRE(found_launcher_dxc);

    std::printf("[PASS] Test 1: dxcompiler_xs.dll classified as ConsoleExclusive & routed to OpenScarlett Shim (dxcompiler_xs.dll)\n");
}

void Test2_StaticMissingDllStubSynthesisLive(const fs::path& bin_dir, const fs::path& temp_dir) {
    const fs::path guest_exe = temp_dir / "fake_guest_probe_test.exe";
    OS_REQUIRE(WriteSyntheticGuestExe(guest_exe, "fake_unshimmed_xs.dll"));

    // Verify PE inspector sees both named and ordinal imports from fake_unshimmed_xs.dll
    openscarlett::PeInspectionReport rep{};
    OS_REQUIRE(openscarlett::InspectPeFile(guest_exe.string(), rep).ok());
    bool found_fake_dll = false;
    bool found_named = false;
    bool found_ord7 = false;
    for (const auto& mod : rep.imports) {
        if (mod.dll_name == "fake_unshimmed_xs.dll") {
            found_fake_dll = true;
            for (const auto& sym : mod.symbols) {
                if (!sym.by_ordinal && sym.name == "FakeXboxInit") found_named = true;
                if (sym.by_ordinal && sym.ordinal == 7u) found_ord7 = true;
            }
        }
    }
    OS_REQUIRE(found_fake_dll && found_named && found_ord7);

    // Ensure fake_unshimmed_xs.dll truly does not exist in bin_dir or temp_dir
    std::error_code ec;
    OS_REQUIRE(!fs::exists(bin_dir / "fake_unshimmed_xs.dll", ec));
    OS_REQUIRE(!fs::exists(temp_dir / "fake_unshimmed_xs.dll", ec));

    // Launch via openscarlett.exe run --probe-unknown-imports
    const fs::path cli_exe = bin_dir / "openscarlett.exe";
    OS_REQUIRE(fs::exists(cli_exe, ec));

    const fs::path tel_path = temp_dir / "probe_stub_telemetry.json";
    const std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run --probe-unknown-imports"
        L" --shim-dir \"" + bin_dir.wstring() + L"\""
        L" --telemetry \"" + tel_path.wstring() + L"\""
        L" --timeout-ms 15000 \"" + guest_exe.wstring() + L"\"";

    const ChildRunResult run = RunProcessCaptureOutput(cli_exe, cmd, temp_dir, 20000u);
    if (!run.launched || run.exit_code != 0u) {
        std::fprintf(stderr, "[DEBUG] openscarlett run exit=%lu output:\n%s\n",
                     static_cast<unsigned long>(run.exit_code), run.output.c_str());
    }
    OS_REQUIRE(run.launched);
    // Exit code 0 proves:
    //  1) No STATUS_DLL_NOT_FOUND (0xC0000135) loader crash
    //  2) All 3 named imports (unsorted in guest PE) + 2 ordinal imports (@7, @19)
    //     resolved cleanly and returned E_NOTIMPL (0x80004001)!
    OS_REQUIRE(run.exit_code == 0u);
    OS_REQUIRE(run.output.find("[OpenScarlett Probe]") != std::string::npos);
    OS_REQUIRE(run.output.find("fake_unshimmed_xs.dll") != std::string::npos);

    // Verify synthesized stub DLL was NOT placed on C:\ (must live on E:\ or non-C:\ path).
    const size_t arrow_pos = run.output.find("-> ");
    OS_REQUIRE(arrow_pos != std::string::npos);
    const std::string stub_path_line = run.output.substr(arrow_pos + 3u);
    OS_REQUIRE(stub_path_line.size() >= 2u);
    OS_REQUIRE(!(stub_path_line[0] == 'C' || stub_path_line[0] == 'c') || stub_path_line[1] != ':');

    std::printf("[PASS] Test 2: Static missing-DLL PE32+ stub synthesis booted isolated guest (3 named + 2 ordinal -> E_NOTIMPL, exit=0)\n");
}

void Test3_LauncherHeadlessCheck(const fs::path& bin_dir) {
    const fs::path launcher_exe = bin_dir / "OpenScarlettLauncher.exe";
    std::error_code ec;
    OS_REQUIRE(fs::exists(launcher_exe, ec));

    const std::wstring cmd = L"\"" + launcher_exe.wstring() + L"\" --headless-check";
    const ChildRunResult run = RunProcessCaptureOutput(launcher_exe, cmd, bin_dir, 20000u);
    if (!run.launched || run.exit_code != 0u) {
        std::fprintf(stderr, "[DEBUG] OpenScarlettLauncher --headless-check exit=%lu output:\n%s\n",
                     static_cast<unsigned long>(run.exit_code), run.output.c_str());
    }
    OS_REQUIRE(run.launched);
    OS_REQUIRE(run.exit_code == 0u);
    OS_REQUIRE(run.output.find("[PASS]") != std::string::npos);

    std::printf("[PASS] Test 3: OpenScarlettLauncher --headless-check exited 0\n");
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
    const fs::path bin_dir(OPENSCARLETT_BINARY_DIR);
    const fs::path temp_dir = bin_dir / "ue5_probe_static_stub_tmp";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    Test1_DxcompilerXsClassificationAndRouting(bin_dir, temp_dir);
    Test2_StaticMissingDllStubSynthesisLive(bin_dir, temp_dir);
    Test3_LauncherHeadlessCheck(bin_dir);

    fs::remove_all(temp_dir, ec);
    std::printf("[ALL PASS] test_ue5_probe_static_stub\n");
    return 0;
#else
    std::printf("[SKIP] Windows-only test\n");
    return 0;
#endif
}
