#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace openscarlett::launcher {

struct GpuAdapterEntry {
    uint32_t index = 0;
    std::string name;
    bool is_warp = false;
    uint64_t vram_mb = 0;
    uint32_t wave_min = 0;
    uint32_t wave_max = 0;
    uint32_t shader_model = 0;
    uint32_t dxr_tier = 0;
    uint32_t mesh_tier = 0;
    std::string details;
};

struct GpuTelemetrySnapshot {
    bool probed = false;
    bool probe_ok = false;
    std::string os_version;
    std::string vc_runtime_info;
    std::string primary_gpu_name;
    std::string primary_gpu_summary;
    uint64_t primary_vram_mb = 0;
    uint32_t primary_shader_model = 0;
    uint32_t primary_wave_min = 0;
    uint32_t primary_wave_max = 0;
    uint32_t primary_mesh_tier = 0;
    uint32_t primary_dxr_tier = 0;
    bool vulkan_available = false;
    std::string vulkan_device_name;
    std::string vulkan_api_version;
    std::string vulkan_mesh_shader;
    bool vulkan_swapchain = false;
    bool vulkan_hdr10 = false;
    std::string vulkan_summary_line;
    std::string dxc_path;
    std::string raw_probe_output;
    std::vector<GpuAdapterEntry> adapters;
};

struct HardwareTestRow {
    std::string name;
    std::string description;
    bool passed = false;
    int exit_code = 0;
    double seconds = 0.0;
    std::string highlight;
    std::string output;
};

struct HardwareTestProgress {
    int completed = 0;
    int total = 0;
    std::string current_test_name;
    bool has_completed_row = false;
    HardwareTestRow last_row;
    std::string log_line;
};

struct HardwareTestRunResult {
    bool completed = false;
    bool cancelled = false;
    int passed_count = 0;
    int failed_count = 0;
    int total_count = 0;
    std::string report_markdown;
    std::filesystem::path report_path;
    std::string github_issue_url;
    std::string github_url_mode;  // "complete" | "compact" | "clipboard"
    std::string full_log_text;
    std::vector<HardwareTestRow> rows;
};

enum class LaunchBackendChoice : int {
    Auto = 0,
    D3D12 = 1,
    Vulkan = 2,
};

struct LauncherRunRequest {
    std::string target_path;
    std::filesystem::path shim_dir;
    LaunchBackendChoice backend = LaunchBackendChoice::Auto;
    std::string adapter_substring;  // sets OPENSCARLETT_VK_DEVICE when non-empty
    bool prefer_warp = false;
    bool null_audio = false;
    bool windowed = true;
    std::string extra_args;
    uint32_t timeout_ms = 0;  // 0 = infinite
};

struct LauncherRunResult {
    bool launched = false;
    int exit_code = -1;
    bool timed_out = false;
    uint64_t total_calls = 0;
    size_t tracked_symbols = 0;
    uint64_t not_impl_calls = 0;
    std::filesystem::path telemetry_path;
    std::string log_output;
    std::string error_message;
};

struct BundledDemoEntry {
    std::string id;
    std::string display_name;
    std::string subtitle;
    std::filesystem::path exe_path;
    std::filesystem::path config_path;
    bool available = false;
};

struct PeInspectorModuleRow {
    std::string dll_name;
    std::string category;
    size_t import_count = 0;
    std::string routing_status;  // e.g. "OpenScarlett Shim (d3d12_xs.dll)", "System32 KnownDLL", "Host Windows"
    bool shim_present = true;
    std::vector<std::string> missing_exports;
    std::vector<std::string> symbols;
};

struct PeInspectorSummary {
    bool ok = false;
    std::string error_message;
    std::string file_path;
    bool is_dll = false;
    uint64_t image_base = 0;
    uint32_t image_size = 0;
    uint32_t section_count = 0;
    bool has_relocations = false;
    bool has_tls = false;
    size_t tls_callbacks = 0;
    uint32_t pdata_entries = 0;
    size_t total_exports = 0;
    size_t forwarded_exports = 0;
    uint32_t total_imports = 0;
    uint32_t console_imports = 0;
    uint32_t gdk_imports = 0;
    uint32_t delay_load_imports = 0;
    size_t missing_export_count = 0;
    std::vector<PeInspectorModuleRow> modules;
};

// Privacy sanitization matching tools/run_hardware_test.ps1 (case-insensitive, \ and /).
std::string SanitizeReportText(const std::string& text,
                               const std::filesystem::path& bundle_dir);

// Host OS & VC++ runtime summaries matching tools/run_hardware_test.ps1.
std::string GetFriendlyWindowsOsVersion();
std::string GetVcRuntimeSummary(const std::filesystem::path& bundle_dir);
std::string GetHardwareTestDescription(const std::string& exe_name);

// Probes host GPUs (via openscarlett.exe probe-gpu) and parses structured telemetry.
GpuTelemetrySnapshot ProbeHostGpuTelemetry(const std::filesystem::path& bundle_dir);

// Discovers bundled playable demos beside the launcher (ScarlettDoom, ScarlettArena, gdk_3d_scene).
std::vector<BundledDemoEntry> DiscoverBundledDemos(const std::filesystem::path& bundle_dir);

// Runs openscarlett.exe probe-gpu + all portable test_*.exe binaries asynchronously (blocking caller thread;
// invoke from worker thread), writes sanitized hardware_test_report.md, and builds pre-filled GitHub URL.
HardwareTestRunResult RunHardwareSelfTestSuite(
    const std::filesystem::path& bundle_dir,
    const GpuTelemetrySnapshot* cached_probe,
    const std::function<void(const HardwareTestProgress&)>& on_progress = {},
    const std::atomic<bool>* cancel_flag = nullptr);

// Builds pre-filled GitHub Hardware Report URL (respecting 6200-char URL budget).
std::string BuildGitHubHardwareReportUrl(const GpuTelemetrySnapshot& probe,
                                         const HardwareTestRunResult& run,
                                         std::string* out_mode = nullptr);

// Native Win32 clipboard & shell browser helpers.
bool CopyTextToClipboard(const std::string& utf8_text);
bool OpenUrlInDefaultBrowser(const std::string& url);

// Launches a package directory, MicrosoftGame.Config, or .exe via openscarlett.exe run
// and parses live stdout + unified CallTelemetry JSON.
LauncherRunResult LaunchTargetWithOpenScarlett(
    const std::filesystem::path& bundle_dir,
    const LauncherRunRequest& req,
    const std::function<void(const std::string&)>& on_log_line = {},
    const std::atomic<bool>* cancel_flag = nullptr);

// Inspects any PE32+ .exe/.dll, checking OpenScarlett shim routing & missing exports against shim DLLs.
PeInspectorSummary InspectBinaryForLauncher(const std::filesystem::path& pe_path,
                                            const std::filesystem::path& shim_dir);

}  // namespace openscarlett::launcher
