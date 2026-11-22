#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "openscarlett/gpu/vulkan_backend.h"
#include "rdna2_coverage_tool.h"
#include "openscarlett/kernel/console_lsa_policy.h"
#include "openscarlett/loader/apc_injector.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/vfs_mounts.h"
#include "openscarlett/loader/xvd_inspector.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

int RunProbeGpu() {
    std::printf("=== OpenScarlett Host Hardware & Toolchain Probe ===\n");
#if defined(_WIN32)
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    std::printf("Platform     : Windows x64\n");

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        for (UINT idx = 0;; ++idx) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            bool is_sw = ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) ||
                         (desc.VendorId == 0x1414u &&
                          (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0));

            Microsoft::WRL::ComPtr<ID3D12Device> dev;
            HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            if (FAILED(hr)) continue;

            D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
            dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1));
            dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
            dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7));
            dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));

            std::printf("D3D12 [%u]    : %s (%s) | VRAM=%llu MB | Wave=%u..%u | SM=0x%02X | DXR=%u | MeshTier=%u\n",
                        idx,
                        name,
                        is_sw ? "WARP" : "Hardware",
                        static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull)),
                        opt1.WaveLaneCountMin,
                        opt1.WaveLaneCountMax,
                        static_cast<unsigned>(sm.HighestShaderModel),
                        static_cast<unsigned>(opt5.RaytracingTier),
                        static_cast<unsigned>(opt7.MeshShaderTier));
        }
    }
#endif

    openscarlett::VulkanBackend vk;
    auto vk_status = vk.Initialize(true);
    if (vk_status.ok()) {
        const auto& info = vk.GetDeviceInfo();
        const char* mesh_str = "no";
        if (info.supports_mesh_shader) {
            mesh_str = info.supports_task_shader ? "task+mesh" : "mesh";
        }
        std::printf("Vulkan       : %s | API %u.%u.%u | %s | compute_q=%u | gfx_q=%s | VK_EXT_mesh_shader=%s | Win32Swapchain=%s | HDR10_CS=%s\n",
                    info.device_name.c_str(),
                    (info.api_version >> 22) & 0x3FFu,
                    (info.api_version >> 12) & 0x3FFu,
                    info.api_version & 0xFFFu,
                    info.is_hardware ? "Hardware ICD" : "Software Fallback",
                    info.compute_queue_family,
                    info.supports_graphics ? std::to_string(info.graphics_queue_family).c_str() : "none",
                    mesh_str,
                    (info.supports_win32_surface && info.supports_swapchain) ? "yes" : "no",
                    info.supports_hdr10_colorspace ? "yes" : "no");
        vk.Shutdown();
    } else {
        std::printf("Vulkan       : unavailable (%s)\n", vk_status.message.c_str());
    }

    std::filesystem::path detected_dxc;
    std::error_code dxc_ec;
#if defined(_WIN32)
    {
        char self_buf[MAX_PATH]{};
        const DWORD self_len = GetModuleFileNameA(nullptr, self_buf, static_cast<DWORD>(sizeof(self_buf)));
        if (self_len > 0 && self_len < sizeof(self_buf)) {
            const auto beside = std::filesystem::path(self_buf).parent_path() / "dxc.exe";
            if (std::filesystem::exists(beside, dxc_ec)) {
                detected_dxc = beside;
            }
        }
        if (detected_dxc.empty()) {
            char env_dxc[1024]{};
            const DWORD env_len = GetEnvironmentVariableA("OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(sizeof(env_dxc)));
            if (env_len > 0 && env_len < sizeof(env_dxc) && std::filesystem::exists(env_dxc, dxc_ec)) {
                detected_dxc = env_dxc;
            }
        }
        if (detected_dxc.empty()) {
            char path_buf[MAX_PATH]{};
            const DWORD path_len = SearchPathA(nullptr, "dxc.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
            if (path_len > 0 && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, dxc_ec)) {
                detected_dxc = path_buf;
            }
        }
    }
#endif
    if (detected_dxc.empty()) {
        const char* dxc_candidates[] = {
            R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
            R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
        };
        for (const char* p : dxc_candidates) {
            if (std::filesystem::exists(p, dxc_ec)) {
                detected_dxc = p;
                break;
            }
        }
    }
    if (detected_dxc.empty()) {
        const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
        if (std::filesystem::exists(kits_bin, dxc_ec)) {
            std::vector<std::filesystem::path> found;
            for (const auto& entry : std::filesystem::directory_iterator(kits_bin, dxc_ec)) {
                if (!entry.is_directory()) continue;
                const auto cand = entry.path() / "x64" / "dxc.exe";
                if (std::filesystem::exists(cand, dxc_ec)) found.push_back(cand);
            }
            std::sort(found.begin(), found.end());
            if (!found.empty()) detected_dxc = found.back();
        }
    }
    if (!detected_dxc.empty()) {
        std::printf("DXC (DXIL)   : %s\n", detected_dxc.string().c_str());
    } else {
        std::printf("DXC (DXIL)   : not detected (bundle dxc.exe or install Windows SDK)\n");
    }
    return 0;
}

int RunInspect(const std::string& path) {
    openscarlett::PeInspectionReport rep;
    auto st = openscarlett::InspectPeFile(path, rep);
    if (!st.ok()) {
        std::fprintf(stderr, "inspect failed: %s\n", st.message.c_str());
        return 1;
    }
    std::printf("File         : %s\n", rep.file_path.c_str());
    std::printf("Format       : PE32+ x64 (%s) | ImageBase=0x%llX | Size=%u B | Sections=%u\n",
                rep.is_dll ? "DLL" : "EXE",
                static_cast<unsigned long long>(rep.image_base),
                rep.image_size,
                rep.section_count);
    size_t forwarded_exports = 0;
    for (const auto& exp : rep.exports) {
        if (exp.is_forwarded) ++forwarded_exports;
    }
    std::printf("Flags        : Relocs=%s | TLS=%s (%zu callbacks) | .pdata=%u SEH entries\n",
                rep.has_relocations ? "yes" : "stripped",
                rep.has_tls ? "yes" : "no",
                rep.tls_callback_rvas.size(),
                rep.exception_entry_count);
    std::printf("Exports      : %zu total (%zu forwarded)\n",
                rep.exports.size(),
                forwarded_exports);
    std::printf("Imports      : %u total (%u console-only, %u GDK, %u delay-loaded) across %zu DLLs\n",
                rep.total_imports,
                rep.console_imports,
                rep.gdk_imports,
                rep.delay_load_imports,
                rep.imports.size());
    for (const auto& mod : rep.imports) {
        std::printf("  - %-36s [%-16s] %zu imports\n",
                    mod.dll_name.c_str(),
                    openscarlett::DllCategoryName(mod.category),
                    mod.symbols.size());
    }
    return 0;
}

std::string QuoteWin32Arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

bool ExtractJsonStringField(const std::string& line, const std::string& key, std::string& out_val) {
    std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        char c = line[pos++];
        if (c == '\\' && pos < line.size()) {
            val.push_back(line[pos++]);
        } else if (c == '"') {
            out_val = std::move(val);
            return true;
        } else {
            val.push_back(c);
        }
    }
    return false;
}

bool ExtractJsonUintField(const std::string& line, const std::string& key, uint64_t& out_val) {
    std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos += pat.size();
    char* end_ptr = nullptr;
    unsigned long long parsed = std::strtoull(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) {
        return false;
    }
    out_val = static_cast<uint64_t>(parsed);
    return true;
}

struct TelemetrySummaryInfo {
    bool file_written = false;
    size_t tracked_symbols = 0;
    size_t blocker_count = 0;
};

TelemetrySummaryInfo PrintTelemetrySummary(const std::string& json_path) {
    TelemetrySummaryInfo info{};
    if (json_path.empty() || !std::filesystem::exists(json_path)) {
        std::printf("Telemetry    : (no report written)\n");
        return info;
    }
    std::ifstream ifs(json_path);
    if (!ifs) {
        std::printf("Telemetry    : %s (unreadable)\n", json_path.c_str());
        return info;
    }
    info.file_written = true;

    struct BlockerRow {
        std::string module;
        std::string symbol;
        uint64_t not_impl = 0;
        uint64_t total = 0;
    };
    size_t total_entries = 0;
    std::vector<BlockerRow> blockers;

    std::string line;
    while (std::getline(ifs, line)) {
        std::string mod;
        std::string sym;
        uint64_t not_impl = 0;
        uint64_t total = 0;
        if (!ExtractJsonStringField(line, "module", mod) ||
            !ExtractJsonStringField(line, "symbol", sym)) {
            continue;
        }
        ++total_entries;
        ExtractJsonUintField(line, "not_impl", not_impl);
        ExtractJsonUintField(line, "total", total);
        if (not_impl > 0) {
            blockers.push_back(BlockerRow{std::move(mod), std::move(sym), not_impl, total});
        }
    }

    info.tracked_symbols = total_entries;
    info.blocker_count = blockers.size();

    std::printf("Telemetry    : %s (%zu symbols tracked, %zu missing blockers)\n",
                json_path.c_str(),
                total_entries,
                blockers.size());
    const size_t show = std::min<size_t>(blockers.size(), 10);
    for (size_t i = 0; i < show; ++i) {
        std::printf("  [BLOCKER]  %s!%s (not_impl=%llu, total=%llu)\n",
                    blockers[i].module.c_str(),
                    blockers[i].symbol.c_str(),
                    static_cast<unsigned long long>(blockers[i].not_impl),
                    static_cast<unsigned long long>(blockers[i].total));
    }
    return info;
}

int RunGuestExecutable(int argc, char** argv) {
    openscarlett::LaunchOptions opts{};
    std::vector<std::string> guest_args;
    std::string gpu_backend_override;
    bool probe_unknown_imports = false;
    bool in_guest_args = false;

    auto normalize_backend = [](std::string raw) -> std::string {
        std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? (c - 'A' + 'a') : c);
        });
        return raw;
    };

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (in_guest_args) {
            guest_args.push_back(arg);
            continue;
        }
        if (arg == "--") {
            in_guest_args = true;
            continue;
        }
        auto take_value = [&](const char* flag, std::string& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "run: missing value for %s\n", flag);
                return false;
            }
            dst = argv[++i];
            return true;
        };

        if (arg == "--shim-dir") {
            if (!take_value("--shim-dir", opts.shim_dir)) return 1;
        } else if (arg == "--vfs-g") {
            if (!take_value("--vfs-g", opts.vfs_g)) return 1;
        } else if (arg == "--vfs-g-overlay") {
            if (!take_value("--vfs-g-overlay", opts.vfs_g_overlay)) return 1;
        } else if (arg == "--vfs-t") {
            if (!take_value("--vfs-t", opts.vfs_t)) return 1;
        } else if (arg == "--vfs-s") {
            if (!take_value("--vfs-s", opts.vfs_s)) return 1;
        } else if (arg == "--telemetry") {
            if (!take_value("--telemetry", opts.telemetry_output_path)) return 1;
        } else if (arg == "--backend") {
            std::string raw;
            if (!take_value("--backend", raw)) return 1;
            const std::string norm = normalize_backend(raw);
            if (norm != "vulkan" && norm != "d3d12") {
                std::fprintf(stderr, "run: invalid --backend value '%s' (expected 'd3d12' or 'vulkan')\n", raw.c_str());
                return 1;
            }
            gpu_backend_override = norm;
        } else if (arg.rfind("--backend=", 0) == 0) {
            const std::string raw = arg.substr(std::string("--backend=").size());
            const std::string norm = normalize_backend(raw);
            if (norm != "vulkan" && norm != "d3d12") {
                std::fprintf(stderr, "run: invalid --backend value '%s' (expected 'd3d12' or 'vulkan')\n", raw.c_str());
                return 1;
            }
            gpu_backend_override = norm;
        } else if (arg == "--probe-unknown-imports") {
            probe_unknown_imports = true;
        } else if (arg == "--timeout-ms") {
            std::string raw;
            if (!take_value("--timeout-ms", raw)) return 1;
            char* end_ptr = nullptr;
            unsigned long val = std::strtoul(raw.c_str(), &end_ptr, 10);
            if (!end_ptr || *end_ptr != '\0') {
                std::fprintf(stderr, "run: invalid --timeout-ms value: %s\n", raw.c_str());
                return 1;
            }
            opts.timeout_ms = static_cast<uint32_t>(val);
        } else if (opts.exe_path.empty() && (!arg.empty() && arg[0] != '-')) {
            opts.exe_path = arg;
        } else if (!opts.exe_path.empty()) {
            guest_args.push_back(arg);
        } else {
            std::fprintf(stderr, "run: unknown option: %s\n", arg.c_str());
            return 1;
        }
    }

    if (opts.exe_path.empty()) {
        std::fprintf(stderr, "run: missing target package directory, MicrosoftGame.Config, or executable path\n");
        return 1;
    }

    for (size_t i = 0; i < guest_args.size(); ++i) {
        if (i > 0) opts.arguments.push_back(' ');
        opts.arguments += QuoteWin32Arg(guest_args[i]);
    }

    if (opts.telemetry_output_path.empty()) {
        std::string stem;
        openscarlett::ResolvedPackageTarget pre_target{};
        if (openscarlett::ResolvePackageLaunchTarget(opts.exe_path, pre_target).ok() &&
            !pre_target.exe_path.empty()) {
            stem = std::filesystem::path(pre_target.exe_path).stem().string();
        } else {
            stem = std::filesystem::path(opts.exe_path).stem().string();
        }
        if (stem.empty()) stem = "guest";
        const std::wstring anchor_w = opts.shim_dir.empty()
                                          ? std::wstring{}
                                          : openscarlett::Utf8ToWide(opts.shim_dir);
        const std::filesystem::path tel_base(openscarlett::ResolveDefaultTelemetryDirW(anchor_w));
        opts.telemetry_output_path = (tel_base / (stem + "_telemetry.json")).string();
    }

    // Save/set/restore OPENSCARLETT_GPU_BACKEND, OPENSCARLETT_BACKEND & OPENSCARLETT_PROBE_UNKNOWN_IMPORTS around LaunchWithApcHook
    bool had_prev_gpu_backend_env = false;
    std::string prev_gpu_backend_env;
    bool had_prev_backend_env = false;
    std::string prev_backend_env;
    bool had_prev_probe_env = false;
    std::string prev_probe_env;
#if defined(_WIN32)
    {
        auto read_env_var = [](const char* name, bool& had, std::string& val) {
            char buf[256]{};
            const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
            if (len > 0 && len < sizeof(buf)) {
                had = true;
                val = buf;
            } else if (len == 0 && GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
                had = true;
                val.clear();
            }
        };
        read_env_var("OPENSCARLETT_GPU_BACKEND", had_prev_gpu_backend_env, prev_gpu_backend_env);
        read_env_var("OPENSCARLETT_BACKEND", had_prev_backend_env, prev_backend_env);
        read_env_var("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS", had_prev_probe_env, prev_probe_env);
        if (!gpu_backend_override.empty()) {
            SetEnvironmentVariableA("OPENSCARLETT_GPU_BACKEND", gpu_backend_override.c_str());
            SetEnvironmentVariableA("OPENSCARLETT_BACKEND", gpu_backend_override.c_str());
        }
        if (probe_unknown_imports) {
            SetEnvironmentVariableA("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS", "1");
        }
    }
#else
    if (const char* prev = std::getenv("OPENSCARLETT_GPU_BACKEND")) {
        had_prev_gpu_backend_env = true;
        prev_gpu_backend_env = prev;
    }
    if (const char* prev = std::getenv("OPENSCARLETT_BACKEND")) {
        had_prev_backend_env = true;
        prev_backend_env = prev;
    }
    if (const char* prev = std::getenv("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS")) {
        had_prev_probe_env = true;
        prev_probe_env = prev;
    }
    if (!gpu_backend_override.empty()) {
        setenv("OPENSCARLETT_GPU_BACKEND", gpu_backend_override.c_str(), 1);
        setenv("OPENSCARLETT_BACKEND", gpu_backend_override.c_str(), 1);
    }
    if (probe_unknown_imports) {
        setenv("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS", "1", 1);
    }
#endif

    const std::string active_backend = !gpu_backend_override.empty()
                                           ? gpu_backend_override
                                           : (had_prev_backend_env && !prev_backend_env.empty()
                                                  ? prev_backend_env
                                                  : (had_prev_gpu_backend_env && !prev_gpu_backend_env.empty()
                                                         ? prev_gpu_backend_env
                                                         : "d3d12 (default)"));

    openscarlett::LaunchResult res{};
    auto st = openscarlett::LaunchWithApcHook(opts, res);

    if (!gpu_backend_override.empty()) {
#if defined(_WIN32)
        SetEnvironmentVariableA("OPENSCARLETT_GPU_BACKEND",
                                had_prev_gpu_backend_env ? prev_gpu_backend_env.c_str() : nullptr);
        SetEnvironmentVariableA("OPENSCARLETT_BACKEND",
                                had_prev_backend_env ? prev_backend_env.c_str() : nullptr);
#else
        if (had_prev_gpu_backend_env) {
            setenv("OPENSCARLETT_GPU_BACKEND", prev_gpu_backend_env.c_str(), 1);
        } else {
            unsetenv("OPENSCARLETT_GPU_BACKEND");
        }
        if (had_prev_backend_env) {
            setenv("OPENSCARLETT_BACKEND", prev_backend_env.c_str(), 1);
        } else {
            unsetenv("OPENSCARLETT_BACKEND");
        }
#endif
    }
    if (probe_unknown_imports) {
#if defined(_WIN32)
        SetEnvironmentVariableA("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS",
                                had_prev_probe_env ? prev_probe_env.c_str() : nullptr);
#else
        if (had_prev_probe_env) {
            setenv("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS", prev_probe_env.c_str(), 1);
        } else {
            unsetenv("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS");
        }
#endif
    }

    std::printf("=== OpenScarlett Suspended-Process APC Launch ===\n");
    std::printf("GPU Backend  : %s\n", active_backend.c_str());
    if (!res.resolved_config_path.empty()) {
        std::printf("Game Config  : %s\n", res.resolved_config_path.c_str());
    }
    if (!res.resolved_title_id.empty()) {
        std::printf("TitleId      : %s\n", res.resolved_title_id.c_str());
    }
    if (!res.resolved_package_id.empty()) {
        std::printf("PackageId    : %s\n", res.resolved_package_id.c_str());
    }
    std::printf("Target EXE   : %s\n",
                res.resolved_exe_path.empty() ? opts.exe_path.c_str() : res.resolved_exe_path.c_str());
    std::printf("Shim Dir     : %s\n", res.resolved_shim_dir.c_str());
    std::printf("VFS G:\\      : %s (read-only base)\n", res.resolved_vfs_g.c_str());
    std::printf("VFS G: CoW   : %s\n", res.resolved_vfs_g_overlay.c_str());
    std::printf("VFS T:\\      : %s\n", res.resolved_vfs_t.c_str());
    std::printf("VFS S:\\      : %s\n", res.resolved_vfs_s.c_str());
    std::printf("Guest PID    : %u\n", res.process_id);
    std::printf("Exit Code    : %u%s\n", res.exit_code, res.timed_out ? " (TIMED OUT)" : "");
    const TelemetrySummaryInfo tel_info = PrintTelemetrySummary(res.resolved_telemetry_path);

    if (st.ok()) {
        const auto diag = openscarlett::DiagnoseStoreBootstrapper(
            res, tel_info.tracked_symbols, tel_info.file_written);
        if (diag.suspected_store_bootstrapper) {
            std::printf("Diagnostic   : %s\n", diag.message.c_str());
        }
    }

    if (!st.ok()) {
        std::fprintf(stderr, "run failed: %s\n", st.message.c_str());
        return res.timed_out ? 124 : 1;
    }
    return static_cast<int>(res.exit_code);
}

std::string EscapeJsonMain(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8u);
    for (char c : input) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    char esc[8]{};
                    std::snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned char>(c));
                    out.append(esc);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string ToLowerAsciiMain(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool HasXvdExtensionOrMagic(const std::filesystem::path& p) {
    const std::string ext = ToLowerAsciiMain(p.extension().string());
    if (ext == ".xvd" || ext == ".xvc") {
        return true;
    }
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        return false;
    }
    ifs.seekg(static_cast<std::streamoff>(openscarlett::kXvdMagicOffset), std::ios::beg);
    char magic[8]{};
    ifs.read(magic, 8);
    return ifs.gcount() == 8 && std::memcmp(magic, "msft-xvd", 8) == 0;
}

int RunProbe(int argc, char** argv) {
    std::string target_path;
    bool json_mode = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            json_mode = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "probe: unknown option: %s\n", arg.c_str());
            return 1;
        } else if (target_path.empty()) {
            target_path = arg;
        } else {
            std::fprintf(stderr, "probe: unexpected argument: %s\n", arg.c_str());
            return 1;
        }
    }
    if (target_path.empty()) {
        std::fprintf(stderr, "probe: missing target path (<exe|dir|xvd>)\n");
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path input_p(target_path);
    if (!std::filesystem::exists(input_p, ec)) {
        std::fprintf(stderr, "probe: target does not exist: %s\n", target_path.c_str());
        return 1;
    }

    // Case 1: Direct .xvd / .xvc container file (12 KiB header geometry inspection).
    if (std::filesystem::is_regular_file(input_p, ec) && HasXvdExtensionOrMagic(input_p)) {
        openscarlett::XvdInspectionReport xvd{};
        const auto st = openscarlett::InspectXvdFile(target_path, xvd);
        if (!st.ok()) {
            std::fprintf(stderr, "probe: XVD inspection failed: %s\n", st.message.c_str());
            return 1;
        }
        if (json_mode) {
            std::ostringstream oss;
            oss << "{\n"
                << "  \"probe_kind\": \"xvd\",\n"
                << "  \"target\": \"" << EscapeJsonMain(target_path) << "\",\n"
                << "  \"ready_to_launch\": " << (xvd.can_inspect_filesystem_directly ? "true" : "false") << ",\n"
                << "  \"xvd\": " << openscarlett::FormatXvdInspectionJson(xvd) << "\n"
                << "}\n";
            std::fputs(oss.str().c_str(), stdout);
        } else {
            std::printf("=== OpenScarlett Preflight Compatibility Probe (XVD/XVC) ===\n");
            std::fputs(openscarlett::FormatXvdInspectionText(xvd).c_str(), stdout);
        }
        return 0;
    }

    // Case 2: Executable, MicrosoftGame.Config/.mgc, or unpacked game directory.
    openscarlett::ResolvedPackageTarget pkg_target{};
    std::string exe_path;
    std::string package_root;
    std::vector<openscarlett::XvdInspectionReport> dir_xvds;

    if (openscarlett::ResolvePackageLaunchTarget(target_path, pkg_target).ok() &&
        !pkg_target.exe_path.empty()) {
        exe_path = pkg_target.exe_path;
        package_root = pkg_target.package_root;
    } else if (std::filesystem::is_directory(input_p, ec)) {
        package_root = std::filesystem::absolute(input_p, ec).string();
        std::vector<std::filesystem::path> found_exes;
        std::vector<std::filesystem::path> found_xvds;
        for (const auto& entry : std::filesystem::directory_iterator(input_p, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = ToLowerAsciiMain(entry.path().extension().string());
            if (ext == ".exe") {
                found_exes.push_back(entry.path());
            } else if (ext == ".xvd" || ext == ".xvc") {
                found_xvds.push_back(entry.path());
            }
        }
        std::sort(found_exes.begin(), found_exes.end());
        std::sort(found_xvds.begin(), found_xvds.end());
        if (!found_exes.empty()) {
            exe_path = found_exes.front().string();
        } else if (!found_xvds.empty()) {
            openscarlett::XvdInspectionReport xvd{};
            const auto st = openscarlett::InspectXvdFile(found_xvds.front().string(), xvd);
            if (!st.ok()) {
                std::fprintf(stderr, "probe: XVD inspection failed: %s\n", st.message.c_str());
                return 1;
            }
            if (json_mode) {
                std::ostringstream oss;
                oss << "{\n"
                    << "  \"probe_kind\": \"xvd\",\n"
                    << "  \"target\": \"" << EscapeJsonMain(found_xvds.front().string()) << "\",\n"
                    << "  \"ready_to_launch\": " << (xvd.can_inspect_filesystem_directly ? "true" : "false") << ",\n"
                    << "  \"xvd\": " << openscarlett::FormatXvdInspectionJson(xvd) << "\n"
                    << "}\n";
                std::fputs(oss.str().c_str(), stdout);
            } else {
                std::printf("=== OpenScarlett Preflight Compatibility Probe (XVD/XVC) ===\n");
                std::fputs(openscarlett::FormatXvdInspectionText(xvd).c_str(), stdout);
            }
            return 0;
        }
    } else if (std::filesystem::is_regular_file(input_p, ec)) {
        exe_path = std::filesystem::absolute(input_p, ec).string();
        package_root = std::filesystem::path(exe_path).parent_path().string();
    }

    if (exe_path.empty()) {
        std::fprintf(stderr, "probe: no PE32+ executable or XVD/XVC container found at: %s\n",
                     target_path.c_str());
        return 1;
    }

    openscarlett::PeInspectionReport pe_rep{};
    const auto pe_st = openscarlett::InspectPeFile(exe_path, pe_rep);
    if (!pe_st.ok()) {
        std::fprintf(stderr, "probe: PE inspection failed for %s: %s\n",
                     exe_path.c_str(), pe_st.message.c_str());
        return 1;
    }

    openscarlett::ConsoleLsaPolicy lsa_policy;
    bool has_lsa_config = false;
    if (pkg_target.has_config && !pkg_target.config_path.empty()) {
        if (lsa_policy.LoadFromMicrosoftGameConfigFile(pkg_target.config_path).ok()) {
            has_lsa_config = true;
        } else {
            lsa_policy.ApplyGameConfig(pkg_target.config);
            has_lsa_config = true;
        }
    }

    openscarlett::PeForwarderResolver resolver(openscarlett::DllRoutingTable::CreateWithEnvOverrides());
    auto register_local_dlls_and_xvds = [&](const std::string& dir_str) {
        if (dir_str.empty()) return;
        std::error_code lec;
        const std::filesystem::path dir_p(dir_str);
        if (!std::filesystem::is_directory(dir_p, lec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir_p, lec)) {
            if (!entry.is_regular_file(lec)) continue;
            const std::string ext = ToLowerAsciiMain(entry.path().extension().string());
            if (ext == ".dll") {
                resolver.RegisterNativeGuestDll(entry.path().filename().string());
            } else if (ext == ".xvd" || ext == ".xvc") {
                openscarlett::XvdInspectionReport xvd{};
                if (openscarlett::InspectXvdFile(entry.path().string(), xvd).ok()) {
                    dir_xvds.push_back(std::move(xvd));
                }
            }
        }
    };
    register_local_dlls_and_xvds(package_root);
    const std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
    if (!exe_dir.empty() && exe_dir != package_root) {
        register_local_dlls_and_xvds(exe_dir);
    }

    const openscarlett::PeForwarderPreflightReport preflight = resolver.BuildPreflightReport(pe_rep);
    const uint32_t missing_imports =
        preflight.unresolved_count + preflight.cycle_error_count + preflight.malformed_error_count;
    const openscarlett::ConsolePackageIdentity pkg_id = lsa_policy.GetPackageIdentity();

    if (json_mode) {
        std::ostringstream oss;
        oss << "{\n"
            << "  \"probe_kind\": \"" << (pkg_target.has_config ? "package" : "pe") << "\",\n"
            << "  \"target\": \"" << EscapeJsonMain(target_path) << "\",\n"
            << "  \"exe_path\": \"" << EscapeJsonMain(exe_path) << "\",\n"
            << "  \"package_root\": \"" << EscapeJsonMain(package_root) << "\",\n"
            << "  \"has_game_config\": " << (pkg_target.has_config ? "true" : "false") << ",\n"
            << "  \"config_path\": \"" << EscapeJsonMain(pkg_target.config_path) << "\",\n";
        if (has_lsa_config) {
            oss << "  \"game_config\": {\n"
                << "    \"identity_name\": \"" << EscapeJsonMain(pkg_id.package_name) << "\",\n"
                << "    \"version\": \"" << EscapeJsonMain(pkg_id.version) << "\",\n"
                << "    \"display_name\": \"" << EscapeJsonMain(pkg_id.display_name) << "\",\n"
                << "    \"title_id\": \"" << EscapeJsonMain(pkg_id.title_id_hex) << "\",\n"
                << "    \"package_family_name\": \"" << EscapeJsonMain(pkg_id.package_family_name) << "\",\n"
                << "    \"store_id\": \"" << EscapeJsonMain(pkg_id.store_id) << "\",\n"
                << "    \"sandbox_id\": \"" << EscapeJsonMain(pkg_id.sandbox_id) << "\"\n"
                << "  },\n";
        }
        oss << "  \"summary\": {\n"
            << "    \"total_imports\": " << preflight.total_entries << ",\n"
            << "    \"shimmed_imports\": " << preflight.openscarlett_shim_count << ",\n"
            << "    \"windows_system_imports\": " << preflight.windows_system_count << ",\n"
            << "    \"apiset_redirected_imports\": " << preflight.apiset_redirected_count << ",\n"
            << "    \"native_guest_imports\": " << preflight.native_guest_count << ",\n"
            << "    \"missing_imports\": " << missing_imports << ",\n"
            << "    \"ready_to_launch\": " << (preflight.ready_to_launch ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"forwarder_preflight\": " << openscarlett::FormatPeForwarderPreflightJson(preflight) << ",\n"
            << "  \"xvd_containers\": [";
        for (size_t i = 0; i < dir_xvds.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\n" << openscarlett::FormatXvdInspectionJson(dir_xvds[i]);
        }
        oss << "]\n"
            << "}\n";
        std::fputs(oss.str().c_str(), stdout);
    } else {
        std::printf("=== OpenScarlett Preflight Compatibility Probe ===\n");
        std::printf("Target       : %s\n", target_path.c_str());
        std::printf("Executable   : %s (PE32+ x64 %s, %u sections)\n",
                    exe_path.c_str(), pe_rep.is_dll ? "DLL" : "EXE", pe_rep.section_count);
        if (pkg_target.has_config) {
            std::printf("Game Config  : %s\n", pkg_target.config_path.c_str());
            std::printf("Title        : %s | TitleId=%s | PFN=%s | Sandbox=%s\n",
                        pkg_id.display_name.empty() ? pkg_id.package_name.c_str() : pkg_id.display_name.c_str(),
                        pkg_id.title_id_hex.c_str(),
                        pkg_id.package_family_name.c_str(),
                        pkg_id.sandbox_id.c_str());
        }
        std::printf("Summary      : total=%u | shimmed=%u | windows=%u | apiset=%u | guest_dll=%u | missing=%u | ready=%s\n",
                    preflight.total_entries,
                    preflight.openscarlett_shim_count,
                    preflight.windows_system_count,
                    preflight.apiset_redirected_count,
                    preflight.native_guest_count,
                    missing_imports,
                    preflight.ready_to_launch ? "YES" : "NO");
        std::fputs(openscarlett::FormatPeForwarderPreflightText(preflight).c_str(), stdout);
        if (!dir_xvds.empty()) {
            std::printf("XVD Containers Found: %zu\n", dir_xvds.size());
            for (const auto& xvd : dir_xvds) {
                std::fputs(openscarlett::FormatXvdInspectionText(xvd).c_str(), stdout);
            }
        }
    }
    return 0;
}

void PrintUsage() {
    std::printf(
        "OpenScarlett - Xbox Series X|S (Scarlett) translation layer for Windows x64\n"
        "Usage:\n"
        "  openscarlett probe-gpu               Probe host D3D12, Vulkan, and DXC capabilities\n"
        "  openscarlett probe <exe|dir|xvd> [--json]\n"
        "                                       Preflight compatibility report (PE imports, forwarders, config, XVD)\n"
        "  openscarlett inspect <exe_or_dll>    Inspect PE32+ x64 imports, exports, TLS & .pdata\n"
        "  openscarlett rdna2-bench [options]   Audit 779 GFX10.3 opcodes & validate 3,000+ shader corpus\n"
        "      --full | --fast                  Run full 3,120-shader corpus or fast 240-shader sweep\n"
        "      --json <path>                    Write deterministic JSON coverage report\n"
        "      --corpus-count <N>               Override number of generated corpus shaders\n"
        "      --no-gpu | --no-external         Skip live GPU execution or external spirv-val/dxc\n"
        "  openscarlett run <package_dir_or_config_or_exe> [options] [-- <guest-args...>]\n"
        "      --shim-dir <dir>                 Directory containing OpenScarlett shim DLLs\n"
        "      --vfs-g <dir>                    Backing directory for virtual G:\\ / GAME:\\ (read-only)\n"
        "      --vfs-g-overlay <dir>            Copy-on-Write overlay directory for G:\\ / GAME:\\ writes\n"
        "      --vfs-t <dir>                    Backing directory for virtual T:\\ (title scratch)\n"
        "      --vfs-s <dir>                    Backing directory for virtual S:\\ (persistent saves)\n"
        "      --telemetry <json>               Output path for unified CallTelemetry JSON report\n"
        "      --backend <d3d12|vulkan>         Select GPU backend via OPENSCARLETT_GPU_BACKEND\n"
        "      --probe-unknown-imports          Synthesize E_NOTIMPL trampolines & install VEH crash monitor\n"
        "      --timeout-ms <ms>                Maximum runtime in milliseconds (0 = infinite)\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 0;
    }
    std::string cmd = argv[1];
    if (cmd == "probe-gpu") return RunProbeGpu();
    if (cmd == "probe") return RunProbe(argc, argv);
    if (cmd == "inspect" && argc >= 3) return RunInspect(argv[2]);
    if (cmd == "rdna2-bench") return openscarlett::RunRdna2BenchCli(argc, argv);
    if (cmd == "run") return RunGuestExecutable(argc, argv);
    PrintUsage();
    return 1;
}
