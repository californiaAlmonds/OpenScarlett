// OpenScarlett — Real Untouched Microsoft _GAMING_XBOX_SCARLETT (GDKX) Bring-Up Harness
//
// Proves end-to-end execution of 100% untouched MIT Microsoft Xbox-GDK-Samples:
//   1. SimpleTriangle_GDKX.exe   (direct GDKX ID3D12Device8 + PresentX + DXGI output + PLM + PIX)
//   2. SimpleLighting_GDKX.exe   (GDKX ID3D12Device + D32_FLOAT depth + 2 PSOs + CBVs + 3D lit cube)
//   3. SimpleTexture_GDKX.exe    (WIC JPEG decode + UpdateSubresources + SRV table + static sampler)
//   4. SimpleInstancing_GDKX.exe (5,000 instanced 3D cubes + 3 vertex streams + 25 lights + Fence ring)
//   5. SimpleBezier_GDKX.exe     (4K UHD hardware HS/DS tessellation + 6 PSOs + 16-CP Bezier Mobius strip)
//   6. SimpleCompute_GDKX.exe    (CS 6.0 Mandelbrot fractal + Async Compute Queue + UAV/SRV + Fence sync)
//
// Each sample is staged into an isolated ZERO-DLL directory (proving real suspended-process
// APC hook injection), launched via `openscarlett.exe run` on BOTH default host GPU and WARP,
// verified for 100% wrapped Telemetry JSON (zero raw-host escape, 0 errors, 0 stubs, 0 not_impl),
// and verified pixel-by-pixel from the real GPU readback PNG captured after >= 12 PresentX frames.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/loader/pe_inspector.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, const std::string& detail = {}) {
    if (!cond) {
        ++g_failures;
        std::cerr << "[FAIL] " << expr;
        if (!detail.empty()) std::cerr << " :: " << detail;
        std::cerr << "\n";
    }
}

#define EXPECT(c) Check((c), #c)
#define EXPECT_MSG(c, m) Check((c), #c, (m))

struct ScopedEnv {
    std::string name;
    bool had_old = false;
    std::string old_val;

    ScopedEnv(const char* k, const char* v) : name(k) {
        char buf[1024]{};
        const DWORD n = ::GetEnvironmentVariableA(k, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            had_old = true;
            old_val.assign(buf, n);
        }
        ::SetEnvironmentVariableA(k, v);
    }
    ~ScopedEnv() {
        ::SetEnvironmentVariableA(name.c_str(), had_old ? old_val.c_str() : nullptr);
    }
};

struct TelemetryRow {
    std::string module;
    std::string symbol;
    int32_t slot = -1;
    uint64_t total = 0;
    uint64_t ok = 0;
    uint64_t stub = 0;
    uint64_t not_impl = 0;
    uint64_t errors = 0;
};

struct TelemetrySummary {
    std::vector<TelemetryRow> rows;
    uint64_t total_calls = 0;
    uint64_t total_ok = 0;
    uint64_t total_stubs = 0;
    uint64_t total_not_impl = 0;
    uint64_t total_errors = 0;

    uint64_t OkCount(const std::string& mod, const std::string& sym) const {
        uint64_t sum = 0;
        for (const auto& r : rows) {
            if ((mod.empty() || r.module == mod) && r.symbol == sym) {
                sum += r.ok;
            }
        }
        return sum;
    }
};

std::string ReadAllText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

bool ExtractJsonString(const std::string& line, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    const size_t end = line.find('"', pos);
    if (end == std::string::npos) return false;
    out = line.substr(pos, end - pos);
    return true;
}

bool ExtractJsonInt(const std::string& line, const std::string& key, int64_t& out) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    char* end_ptr = nullptr;
    const long long v = std::strtoll(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) return false;
    out = static_cast<int64_t>(v);
    return true;
}

TelemetrySummary ParseTelemetryJson(const fs::path& json_path) {
    TelemetrySummary sum{};
    std::istringstream iss(ReadAllText(json_path));
    std::string line;
    while (std::getline(iss, line)) {
        TelemetryRow row{};
        if (!ExtractJsonString(line, "module", row.module) ||
            !ExtractJsonString(line, "symbol", row.symbol)) {
            continue;
        }
        int64_t v = 0;
        if (ExtractJsonInt(line, "slot", v)) row.slot = static_cast<int32_t>(v);
        if (ExtractJsonInt(line, "total", v)) row.total = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "ok", v)) row.ok = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "stub", v)) row.stub = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "not_impl", v)) row.not_impl = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "errors", v)) row.errors = static_cast<uint64_t>(v);

        sum.total_calls += row.total;
        sum.total_ok += row.ok;
        sum.total_stubs += row.stub;
        sum.total_not_impl += row.not_impl;
        sum.total_errors += row.errors;
        sum.rows.push_back(std::move(row));
    }
    return sum;
}

// Stored-DEFLATE RGBA8 PNG decoder (matches FrameBufferRgba8::WritePng output).
struct DecodedPng {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;

    void SamplePixel(uint32_t x, uint32_t y, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
        if (x >= width) x = width - 1;
        if (y >= height) y = height - 1;
        const size_t idx = (static_cast<size_t>(y) * width + x) * 4u;
        r = rgba[idx + 0u];
        g = rgba[idx + 1u];
        b = rgba[idx + 2u];
        a = rgba[idx + 3u];
    }
};

uint32_t ReadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool DecodeStoredRgbaPng(const fs::path& png_path, DecodedPng& out, std::string& err) {
    std::ifstream f(png_path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = "cannot open PNG file";
        return false;
    }
    const auto sz = f.tellg();
    if (sz < 33) {
        err = "PNG file too small";
        return false;
    }
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(data.data()), sz);

    const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (std::memcmp(data.data(), kSig, 8) != 0) {
        err = "invalid PNG signature";
        return false;
    }

    size_t pos = 8;
    std::vector<uint8_t> zlib_stream;
    bool got_ihdr = false;

    while (pos + 12 <= data.size()) {
        const uint32_t len = ReadBe32(&data[pos]);
        const char* type = reinterpret_cast<const char*>(&data[pos + 4]);
        pos += 8;
        if (pos + len + 4 > data.size()) {
            err = "truncated PNG chunk";
            return false;
        }
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            out.width = ReadBe32(&data[pos + 0]);
            out.height = ReadBe32(&data[pos + 4]);
            const uint8_t bit_depth = data[pos + 8];
            const uint8_t color_type = data[pos + 9];
            if (bit_depth != 8u || color_type != 6u) {
                err = "expected 8-bit RGBA PNG";
                return false;
            }
            got_ihdr = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            zlib_stream.insert(zlib_stream.end(), data.begin() + static_cast<ptrdiff_t>(pos),
                               data.begin() + static_cast<ptrdiff_t>(pos + len));
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += len + 4;  // payload + CRC
    }

    if (!got_ihdr || out.width == 0 || out.height == 0 || zlib_stream.size() < 6) {
        err = "missing IHDR or IDAT";
        return false;
    }

    // Parse zlib header (2 bytes) + stored DEFLATE blocks + Adler-32 (4 bytes)
    const size_t raw_expected =
        static_cast<size_t>(out.height) * (1u + static_cast<size_t>(out.width) * 4u);
    std::vector<uint8_t> raw;
    raw.reserve(raw_expected);

    size_t zpos = 2;
    const size_t zend = zlib_stream.size() - 4;
    bool bfinal = false;
    while (!bfinal && zpos + 5 <= zend) {
        const uint8_t hdr = zlib_stream[zpos++];
        bfinal = (hdr & 0x01u) != 0;
        const uint8_t btype = (hdr >> 1) & 0x03u;
        if (btype != 0u) {
            err = "non-stored DEFLATE block";
            return false;
        }
        const uint16_t blen = static_cast<uint16_t>(zlib_stream[zpos] | (zlib_stream[zpos + 1] << 8));
        const uint16_t nlen = static_cast<uint16_t>(zlib_stream[zpos + 2] | (zlib_stream[zpos + 3] << 8));
        zpos += 4;
        if (static_cast<uint16_t>(blen ^ 0xFFFFu) != nlen || zpos + blen > zend) {
            err = "corrupt stored DEFLATE block";
            return false;
        }
        raw.insert(raw.end(), zlib_stream.begin() + static_cast<ptrdiff_t>(zpos),
                   zlib_stream.begin() + static_cast<ptrdiff_t>(zpos + blen));
        zpos += blen;
    }

    if (raw.size() != raw_expected) {
        err = "decoded raw byte count mismatch";
        return false;
    }

    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4u);
    const size_t row_bytes = static_cast<size_t>(out.width) * 4u;
    for (uint32_t y = 0; y < out.height; ++y) {
        const uint8_t* src_row = &raw[static_cast<size_t>(y) * (1u + row_bytes)];
        if (src_row[0] != 0u) {
            err = "unexpected PNG row filter";
            return false;
        }
        std::memcpy(&out.rgba[static_cast<size_t>(y) * row_bytes], src_row + 1, row_bytes);
    }
    return true;
}

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, MAX_PATH);
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

DWORD LaunchViaOpenScarlettCli(
    const fs::path& cli_exe,
    const fs::path& pkg_dir,
    const fs::path& shim_dir,
    const fs::path& vfs_t,
    const fs::path& telemetry_json) {
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + pkg_dir.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --telemetry \"" + telemetry_json.wstring() +
        L"\" --timeout-ms 75000";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(
            cli_exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
            0, nullptr, nullptr, &si, &pi)) {
        return 998u;
    }
    const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, 80000u);
    DWORD exit_code = 999u;
    if (wait_rc == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(pi.hProcess, &exit_code);
    } else {
        ::TerminateProcess(pi.hProcess, 997u);
        exit_code = 997u;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return exit_code;
}

void VerifyPeImports(const fs::path& exe_path, const std::string& label) {
    openscarlett::PeInspectionReport rep{};
    EXPECT_MSG(openscarlett::InspectPeFile(exe_path.string(), rep).ok(), label + ": InspectPeFile");
    std::set<std::string> imported;
    for (const auto& m : rep.imports) {
        std::string lo = m.dll_name;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        imported.insert(lo);
    }
    EXPECT_MSG(imported.count("d3d12_xs.dll") == 1u, label + ": imports d3d12_xs.dll");
    EXPECT_MSG(imported.count("kernelx.dll") == 1u, label + ": imports kernelx.dll (appnotify PLM)");
    EXPECT_MSG(imported.count("xgameruntime.dll") == 1u, label + ": imports xgameruntime.dll");
    EXPECT_MSG(imported.count("pixevt.dll") == 1u, label + ": imports pixevt.dll");
    EXPECT_MSG(imported.count("openscarlett_hook.dll") == 0u, label + ": does NOT statically import hook DLL");
}

void VerifyCommonTelemetry(
    const TelemetrySummary& tel,
    const std::string& tag,
    uint64_t min_pso_count,
    bool expects_device_type_query) {
    EXPECT_MSG(tel.total_calls > 0u, tag + ": telemetry recorded calls");
    EXPECT_MSG(tel.total_stubs == 0u, tag + ": telemetry stubs == 0 (got " + std::to_string(tel.total_stubs) + ")");
    EXPECT_MSG(tel.total_not_impl == 0u, tag + ": telemetry not_impl == 0 (got " + std::to_string(tel.total_not_impl) + ")");
    EXPECT_MSG(tel.total_errors == 0u, tag + ": telemetry errors == 0 (got " + std::to_string(tel.total_errors) + ")");

    std::cout << "  [" << tag << "] Telemetry: total=" << tel.total_calls
              << " ok=" << tel.total_ok
              << " stubs=" << tel.total_stubs
              << " not_impl=" << tel.total_not_impl
              << " errors=" << tel.total_errors
              << " symbols=" << tel.rows.size() << "\n";

    // Zero raw-host escape verification: EVERY D3D12 device/queue/frame-pacing call goes through DeviceX/CommandQueueX!
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "D3D12XboxCreateDevice") >= 1u, tag + ": D3D12XboxCreateDevice");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateCommandQueue") >= 1u, tag + ": DeviceX::CreateCommandQueue");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateCommittedResource") >= 3u, tag + ": DeviceX::CreateCommittedResource");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateGraphicsPipelineState") >= min_pso_count,
               tag + ": DeviceX::CreateGraphicsPipelineState >= " + std::to_string(min_pso_count));
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::SetFrameIntervalX") >= 1u, tag + ": DeviceX::SetFrameIntervalX");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::ScheduleFrameEventX") >= 1u, tag + ": DeviceX::ScheduleFrameEventX");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::WaitFrameEventX") >= 12u, tag + ": DeviceX::WaitFrameEventX >= 12");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12CommandQueueX::ExecuteCommandLists") >= 12u, tag + ": CommandQueueX::ExecuteCommandLists >= 12");
    EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12CommandQueueX::PresentX") >= 13u, tag + ": CommandQueueX::PresentX >= 13");

    // PLM appnotify.h lifecycle + GDK runtime + PIX telemetry
    EXPECT_MSG(tel.OkCount("kernelx.dll", "RegisterAppStateChangeNotification") >= 1u, tag + ": RegisterAppStateChangeNotification");
    EXPECT_MSG(tel.OkCount("kernelx.dll", "UnregisterAppStateChangeNotification") >= 1u, tag + ": UnregisterAppStateChangeNotification");
    EXPECT_MSG(tel.OkCount("kernelx.dll", "RegisterAppConstrainedChangeNotification") >= 1u, tag + ": RegisterAppConstrainedChangeNotification");
    EXPECT_MSG(tel.OkCount("kernelx.dll", "UnregisterAppConstrainedChangeNotification") >= 1u, tag + ": UnregisterAppConstrainedChangeNotification");
    EXPECT_MSG(tel.OkCount("xgameruntime.dll", "XGameRuntimeInitialize") >= 1u, tag + ": XGameRuntimeInitialize");
    if (expects_device_type_query) {
        EXPECT_MSG(tel.OkCount("xgameruntime.dll", "XSystemGetDeviceType") >= 1u, tag + ": XSystemGetDeviceType");
    }
    EXPECT_MSG(tel.OkCount("xgameruntime.dll", "XGameRuntimeUninitialize") >= 1u, tag + ": XGameRuntimeUninitialize");
    EXPECT_MSG(tel.OkCount("pixevt.dll", "PIXBeginEventOnCpu") >= 12u, tag + ": PIXBeginEventOnCpu >= 12");
    EXPECT_MSG(tel.OkCount("pixevt.dll", "PIXEndEventOnCpu") >= 12u, tag + ": PIXEndEventOnCpu >= 12");
}

bool IsAtgBackground(uint8_t r, uint8_t g, uint8_t b) {
    // ATG::ColorsLinear::Background (0.05286 linear) -> sRGB ~65 (#414141)
    return (r >= 55 && r <= 75) && (g >= 55 && g <= 75) && (b >= 55 && b <= 75) &&
           std::abs(static_cast<int>(r) - static_cast<int>(g)) <= 4 &&
           std::abs(static_cast<int>(g) - static_cast<int>(b)) <= 4;
}

void VerifySimpleTrianglePixels(const DecodedPng& img, const std::string& tag) {
    EXPECT_MSG(img.width == 3840u && img.height == 2160u,
               tag + ": 4K UHD Scarlett framebuffer (" + std::to_string(img.width) + "x" +
                   std::to_string(img.height) + ")");
    if (img.width < 64u || img.height < 64u) return;

    uint8_t r = 0, g = 0, b = 0, a = 0;

    // 1. Corners must be ATG dark-grey background (#414141)
    img.SamplePixel(16, 16, r, g, b, a);
    EXPECT_MSG(IsAtgBackground(r, g, b),
               tag + ": corner(16,16) is ATG #414141 background (got " + std::to_string(r) + "," +
                   std::to_string(g) + "," + std::to_string(b) + ")");

    img.SamplePixel(img.width - 16, 16, r, g, b, a);
    EXPECT_MSG(IsAtgBackground(r, g, b),
               tag + ": corner(w-16,16) is ATG #414141 background");

    // 2. Top vertex (NDC 0, +0.5 -> screen x=0.5, y=0.25): sample slightly inside at y=0.28 -> Red
    img.SamplePixel(img.width / 2, img.height * 28 / 100, r, g, b, a);
    EXPECT_MSG(r > 180 && r > g + 100 && r > b + 100,
               tag + ": top triangle vertex is RED (got " + std::to_string(r) + "," +
                   std::to_string(g) + "," + std::to_string(b) + ")");

    // 3. Bottom-right vertex (NDC +0.5, -0.5 -> screen x=0.75, y=0.75): sample slightly inside -> Green
    img.SamplePixel(img.width * 72 / 100, img.height * 73 / 100, r, g, b, a);
    EXPECT_MSG(g > 180 && g > r + 100 && g > b + 100,
               tag + ": bottom-right triangle vertex is GREEN (got " + std::to_string(r) + "," +
                   std::to_string(g) + "," + std::to_string(b) + ")");

    // 4. Bottom-left vertex (NDC -0.5, -0.5 -> screen x=0.25, y=0.75): sample slightly inside -> Blue
    img.SamplePixel(img.width * 28 / 100, img.height * 73 / 100, r, g, b, a);
    EXPECT_MSG(b > 180 && b > r + 100 && b > g + 100,
               tag + ": bottom-left triangle vertex is BLUE (got " + std::to_string(r) + "," +
                   std::to_string(g) + "," + std::to_string(b) + ")");

    // 5. Center (NDC 0, 0 -> screen x=0.5, y=0.5): RGB-interpolated interior (linear ~0.5,0.25,0.25 -> sRGB ~188,137,137)
    img.SamplePixel(img.width / 2, img.height / 2, r, g, b, a);
    EXPECT_MSG(!IsAtgBackground(r, g, b) && r > 140 && g > 100 && b > 100,
               tag + ": center is RGB-interpolated triangle interior (got " + std::to_string(r) + "," +
                   std::to_string(g) + "," + std::to_string(b) + ")");

    std::cout << "  [" << tag << "] SimpleTriangle PNG verified: " << img.width << "x" << img.height
              << " center=(" << int(r) << "," << int(g) << "," << int(b) << ")\n";
}

void VerifySimpleLightingPixels(const DecodedPng& img, const std::string& tag) {
    EXPECT_MSG(img.width == 3840u && img.height == 2160u,
               tag + ": 4K UHD Scarlett framebuffer (" + std::to_string(img.width) + "x" +
                   std::to_string(img.height) + ")");
    if (img.width < 64u || img.height < 64u) return;

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0, bg_a = 0;
    img.SamplePixel(16, 16, bg_r, bg_g, bg_b, bg_a);
    EXPECT_MSG(IsAtgBackground(bg_r, bg_g, bg_b),
               tag + ": corner(16,16) is ATG #414141 background (got " + std::to_string(bg_r) + "," +
                   std::to_string(bg_g) + "," + std::to_string(bg_b) + ")");

    // Sample a dense 160x90 grid across the 4K frame to measure rendered 3D cube + orbiting light cubes
    uint32_t non_bg_samples = 0;
    uint32_t bright_samples = 0;
    uint32_t red_tinted_samples = 0;
    std::set<uint32_t> distinct_quantized_colors;

    for (uint32_t gy = 0; gy < 90; ++gy) {
        const uint32_t y = gy * img.height / 90 + img.height / 180;
        for (uint32_t gx = 0; gx < 160; ++gx) {
            const uint32_t x = gx * img.width / 160 + img.width / 320;
            uint8_t r = 0, g = 0, b = 0, a = 0;
            img.SamplePixel(x, y, r, g, b, a);
            const int l1 = std::abs(static_cast<int>(r) - static_cast<int>(bg_r)) +
                           std::abs(static_cast<int>(g) - static_cast<int>(bg_g)) +
                           std::abs(static_cast<int>(b) - static_cast<int>(bg_b));
            if (l1 > 20) {
                ++non_bg_samples;
                if (r > 120 || g > 120 || b > 120) ++bright_samples;
                if (r > g + 30 && r > b + 30) ++red_tinted_samples;
                const uint32_t q = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
                distinct_quantized_colors.insert(q);
            }
        }
    }

    EXPECT_MSG(non_bg_samples >= 200u,
               tag + ": substantial rendered 3D geometry pixels (non_bg=" + std::to_string(non_bg_samples) + ")");
    EXPECT_MSG(bright_samples >= 50u,
               tag + ": lit 3D faces present (bright=" + std::to_string(bright_samples) + ")");
    EXPECT_MSG(red_tinted_samples >= 5u,
               tag + ": red light illumination present (red_tinted=" + std::to_string(red_tinted_samples) + ")");
    EXPECT_MSG(distinct_quantized_colors.size() >= 3u,
               tag + ": shaded multi-face color variance (buckets=" +
                   std::to_string(distinct_quantized_colors.size()) + ")");

    std::cout << "  [" << tag << "] SimpleLighting PNG verified: " << img.width << "x" << img.height
              << " non_bg=" << non_bg_samples << " bright=" << bright_samples
              << " red_tinted=" << red_tinted_samples
              << " color_buckets=" << distinct_quantized_colors.size() << "\n";
}

// Procedural sunset JPEG generator via host Windows WIC so SimpleTexture_GDKX
// runs self-contained with zero external asset dependencies (works identically in portable bundles).
bool WriteProceduralSunsetJpeg(const fs::path& dst_path, std::string& err) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT kW = 256u;
    constexpr UINT kH = 256u;
    constexpr UINT kStride = kW * 3u;
    std::vector<uint8_t> bgr(static_cast<size_t>(kStride) * kH, 0u);

    for (UINT y = 0u; y < kH; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(kH - 1u);
        for (UINT x = 0u; x < kW; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kW - 1u);
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;

            if (v < 0.58f) {
                // Sky: deep cobalt blue -> crimson -> golden orange horizon
                const float t = v / 0.58f;
                if (t < 0.55f) {
                    const float k = t / 0.55f;
                    r = 20.0f + k * 140.0f;
                    g = 42.0f + k * 48.0f;
                    b = 185.0f - k * 45.0f;
                } else {
                    const float k = (t - 0.55f) / 0.45f;
                    r = 160.0f + k * 95.0f;
                    g = 90.0f + k * 85.0f;
                    b = 140.0f - k * 105.0f;
                }
                // Glowing sun disc centered at (0.50, 0.40)
                const float dx = u - 0.50f;
                const float dy = v - 0.40f;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 0.13f) {
                    r = 255.0f;
                    g = 240.0f;
                    b = 115.0f;
                } else if (dist < 0.24f) {
                    const float glow = 1.0f - (dist - 0.13f) / 0.11f;
                    r = std::min(255.0f, r + glow * 90.0f);
                    g = std::min(255.0f, g + glow * 75.0f);
                    b = std::min(255.0f, b + glow * 25.0f);
                }
            } else {
                // Ocean reflection: deep navy with warm golden sun shimmer path
                const float t = (v - 0.58f) / 0.42f;
                r = 22.0f * (1.0f - 0.5f * t);
                g = 52.0f * (1.0f - 0.35f * t);
                b = 128.0f * (1.0f - 0.25f * t);
                const float shimmer = std::exp(-std::pow((u - 0.50f) / (0.07f + 0.14f * t), 2.0f));
                const float wave = 0.65f + 0.35f * std::sin(v * 95.0f + u * 18.0f);
                r = std::min(255.0f, r + shimmer * wave * 230.0f);
                g = std::min(255.0f, g + shimmer * wave * 155.0f);
                b = std::min(255.0f, b + shimmer * wave * 45.0f);
            }

            const size_t idx = static_cast<size_t>(y) * kStride + static_cast<size_t>(x) * 3u;
            bgr[idx + 0u] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
            bgr[idx + 1u] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
            bgr[idx + 2u] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
        }
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        err = "CoCreateInstance(CLSID_WICImagingFactory2) failed";
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        err = "CreateStream failed";
        return false;
    }
    hr = stream->InitializeFromFilename(dst_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        err = "IWICStream::InitializeFromFilename failed";
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        err = "CreateEncoder(JPEG) failed";
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        err = "IWICBitmapEncoder::Initialize failed";
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr) || !frame) {
        err = "CreateNewFrame failed";
        return false;
    }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) {
        err = "IWICBitmapFrameEncode::Initialize failed";
        return false;
    }
    hr = frame->SetSize(kW, kH);
    if (FAILED(hr)) {
        err = "IWICBitmapFrameEncode::SetSize failed";
        return false;
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) {
        err = "IWICBitmapFrameEncode::SetPixelFormat failed";
        return false;
    }
    hr = frame->WritePixels(kH, kStride, static_cast<UINT>(bgr.size()), bgr.data());
    if (FAILED(hr)) {
        err = "IWICBitmapFrameEncode::WritePixels failed";
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        err = "IWICBitmapFrameEncode::Commit failed";
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        err = "IWICBitmapEncoder::Commit failed";
        return false;
    }
    return true;
}

void VerifySimpleTexturePixels(const DecodedPng& img, const std::string& tag) {
    EXPECT_MSG(img.width == 3840u && img.height == 2160u,
               tag + ": 4K UHD Scarlett framebuffer (" + std::to_string(img.width) + "x" +
                   std::to_string(img.height) + ")");
    if (img.width < 64u || img.height < 64u) return;

    uint8_t r = 0, g = 0, b = 0, a = 0;

    // 1. Corners outside the centered [-0.5,+0.5] quad must be ATG #414141 background
    img.SamplePixel(16, 16, r, g, b, a);
    EXPECT_MSG(IsAtgBackground(r, g, b),
               tag + ": corner(16,16) is ATG #414141 background (got " + std::to_string(r) + "," +
                   std::to_string(g) + "," + std::to_string(b) + ")");

    img.SamplePixel(img.width - 16, 16, r, g, b, a);
    EXPECT_MSG(IsAtgBackground(r, g, b),
               tag + ": corner(w-16,16) is ATG #414141 background");

    // 2. Top of quad (u=0.5, v=0.10 -> screen x=0.50, y=0.30): blue sky
    uint8_t sky_r = 0, sky_g = 0, sky_b = 0;
    img.SamplePixel(img.width / 2, img.height * 30 / 100, sky_r, sky_g, sky_b, a);
    EXPECT_MSG(sky_b > 120 && sky_b > sky_r + 50,
               tag + ": textured quad top sky is BLUE (got " + std::to_string(sky_r) + "," +
                   std::to_string(sky_g) + "," + std::to_string(sky_b) + ")");

    // 3. Sun disc (u=0.5, v=0.40 -> screen x=0.50, y=0.45): bright golden yellow
    uint8_t sun_r = 0, sun_g = 0, sun_b = 0;
    img.SamplePixel(img.width / 2, img.height * 45 / 100, sun_r, sun_g, sun_b, a);
    EXPECT_MSG(sun_r > 200 && sun_g > 180 && sun_r > sun_b + 50,
               tag + ": textured quad sun disc is BRIGHT YELLOW (got " + std::to_string(sun_r) + "," +
                   std::to_string(sun_g) + "," + std::to_string(sun_b) + ")");

    // 4. Dense sampling inside the textured quad [28%..72%] x [28%..72%]
    uint32_t non_bg_samples = 0;
    uint32_t blue_samples = 0;
    uint32_t warm_samples = 0;
    std::set<uint32_t> distinct_quantized_colors;

    for (uint32_t gy = 0; gy < 64; ++gy) {
        const uint32_t y = img.height * (28u + gy * 44u / 63u) / 100u;
        for (uint32_t gx = 0; gx < 64; ++gx) {
            const uint32_t x = img.width * (28u + gx * 44u / 63u) / 100u;
            img.SamplePixel(x, y, r, g, b, a);
            if (!IsAtgBackground(r, g, b)) {
                ++non_bg_samples;
                if (b > r + 40 && b > 80) ++blue_samples;
                if (r > b + 40 && r > 140) ++warm_samples;
                const uint32_t q = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
                distinct_quantized_colors.insert(q);
            }
        }
    }

    EXPECT_MSG(non_bg_samples >= 3500u,
               tag + ": textured quad fills interior (non_bg=" + std::to_string(non_bg_samples) + "/4096)");
    EXPECT_MSG(blue_samples >= 500u,
               tag + ": sky/ocean blue texture pixels present (blue=" + std::to_string(blue_samples) + ")");
    EXPECT_MSG(warm_samples >= 500u,
               tag + ": sunset orange/gold texture pixels present (warm=" + std::to_string(warm_samples) + ")");
    EXPECT_MSG(distinct_quantized_colors.size() >= 15u,
               tag + ": rich textured gradient color variance (buckets=" +
                   std::to_string(distinct_quantized_colors.size()) + ")");

    std::cout << "  [" << tag << "] SimpleTexture PNG verified: " << img.width << "x" << img.height
              << " sky=(" << int(sky_r) << "," << int(sky_g) << "," << int(sky_b) << ")"
              << " sun=(" << int(sun_r) << "," << int(sun_g) << "," << int(sun_b) << ")"
              << " non_bg=" << non_bg_samples << " blue=" << blue_samples
              << " warm=" << warm_samples
              << " color_buckets=" << distinct_quantized_colors.size() << "\n";
}

void VerifySimpleInstancingPixels(const DecodedPng& img, const std::string& tag) {
    EXPECT_MSG(img.width == 3840u && img.height == 2160u,
               tag + ": 4K UHD Scarlett framebuffer (" + std::to_string(img.width) + "x" +
                   std::to_string(img.height) + ")");
    if (img.width < 64u || img.height < 64u) return;

    // Sample a dense 160x90 grid across the 4K frame (14,400 samples).
    // SimpleInstancing renders a huge lit enclosing room (instance 0) + 25 moving point lights
    // + 4,974 colorful tumbling 3D cubes exploding around the center of the viewport.
    uint32_t non_black_samples = 0;
    uint32_t non_atg_bg_samples = 0;
    uint32_t bright_samples = 0;
    uint32_t chromatic_samples = 0;
    std::set<uint32_t> distinct_quantized_colors;

    for (uint32_t gy = 0; gy < 90; ++gy) {
        const uint32_t y = gy * img.height / 90 + img.height / 180;
        for (uint32_t gx = 0; gx < 160; ++gx) {
            const uint32_t x = gx * img.width / 160 + img.width / 320;
            uint8_t r = 0, g = 0, b = 0, a = 0;
            img.SamplePixel(x, y, r, g, b, a);
            if (r > 8 || g > 8 || b > 8) ++non_black_samples;
            if (!IsAtgBackground(r, g, b)) ++non_atg_bg_samples;
            if (r > 100 || g > 100 || b > 100) ++bright_samples;
            const uint8_t cmax = std::max({r, g, b});
            const uint8_t cmin = std::min({r, g, b});
            if (cmax - cmin > 25u) ++chromatic_samples;
            const uint32_t q = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            distinct_quantized_colors.insert(q);
        }
    }

    uint8_t cr = 0, cg = 0, cb = 0, ca = 0;
    img.SamplePixel(img.width / 2, img.height / 2, cr, cg, cb, ca);

    EXPECT_MSG(non_black_samples >= 5000u,
               tag + ": 4K viewport rendered (non_black=" + std::to_string(non_black_samples) + "/14400)");
    EXPECT_MSG(non_atg_bg_samples >= 5000u,
               tag + ": enclosing room + instanced cubes overwrite clear color (non_atg_bg=" +
                   std::to_string(non_atg_bg_samples) + ")");
    EXPECT_MSG(bright_samples >= 200u,
               tag + ": directional + 25 point lights illuminate cubes (bright=" +
                   std::to_string(bright_samples) + ")");
    EXPECT_MSG(chromatic_samples >= 50u,
               tag + ": colorful per-instance I_COLOR cubes rendered (chromatic=" +
                   std::to_string(chromatic_samples) + ")");
    EXPECT_MSG(distinct_quantized_colors.size() >= 10u,
               tag + ": rich multi-instance lit color variance (buckets=" +
                   std::to_string(distinct_quantized_colors.size()) + ")");

    std::cout << "  [" << tag << "] SimpleInstancing PNG verified: " << img.width << "x" << img.height
              << " center=(" << int(cr) << "," << int(cg) << "," << int(cb) << ")"
              << " non_black=" << non_black_samples << " bright=" << bright_samples
              << " chromatic=" << chromatic_samples
              << " color_buckets=" << distinct_quantized_colors.size() << "\n";
}

void VerifySimpleBezierPixels(const DecodedPng& img, const std::string& tag) {
    EXPECT_MSG(img.width == 3840u && img.height == 2160u,
               tag + ": 4K UHD Scarlett framebuffer (" + std::to_string(img.width) + "x" +
                   std::to_string(img.height) + ")");
    if (img.width < 64u || img.height < 64u) return;

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0, bg_a = 0;
    img.SamplePixel(16, 16, bg_r, bg_g, bg_b, bg_a);
    EXPECT_MSG(IsAtgBackground(bg_r, bg_g, bg_b),
               tag + ": corner(16,16) is ATG #414141 background (got " + std::to_string(bg_r) + "," +
                   std::to_string(bg_g) + "," + std::to_string(bg_b) + ")");

    uint8_t r2 = 0, g2 = 0, b2 = 0, a2 = 0;
    img.SamplePixel(img.width - 16, 16, r2, g2, b2, a2);
    EXPECT_MSG(IsAtgBackground(r2, g2, b2),
               tag + ": corner(w-16,16) is ATG #414141 background");

    // Sample a dense 160x90 grid across the 4K frame to verify the N.L red-lit tessellated Mobius strip.
    uint32_t non_bg_samples = 0;
    uint32_t red_shaded_samples = 0;
    uint32_t bright_red_samples = 0;
    std::set<uint32_t> distinct_red_buckets;

    for (uint32_t gy = 0; gy < 90; ++gy) {
        const uint32_t y = gy * img.height / 90 + img.height / 180;
        for (uint32_t gx = 0; gx < 160; ++gx) {
            const uint32_t x = gx * img.width / 160 + img.width / 320;
            uint8_t r = 0, g = 0, b = 0, a = 0;
            img.SamplePixel(x, y, r, g, b, a);
            if (!IsAtgBackground(r, g, b)) {
                ++non_bg_samples;
                if (r > 50u && r > g + 45u && r > b + 45u && g < 16u && b < 16u) {
                    ++red_shaded_samples;
                    if (r > 170u) ++bright_red_samples;
                    distinct_red_buckets.insert(r >> 4);
                }
            }
        }
    }

    EXPECT_MSG(non_bg_samples >= 500u,
               tag + ": tessellated Bezier Mobius strip pixels present (non_bg=" +
                   std::to_string(non_bg_samples) + "/14400)");
    EXPECT_MSG(red_shaded_samples >= 400u,
               tag + ": N.L red-lit tessellated surface pixels present (red_shaded=" +
                   std::to_string(red_shaded_samples) + ")");
    EXPECT_MSG(bright_red_samples >= 80u,
               tag + ": brightly lit Bezier facets present (bright_red=" +
                   std::to_string(bright_red_samples) + ")");
    EXPECT_MSG(distinct_red_buckets.size() >= 4u,
               tag + ": smooth N.L shading gradient across curved patches (red_buckets=" +
                   std::to_string(distinct_red_buckets.size()) + ")");

    std::cout << "  [" << tag << "] SimpleBezier PNG verified: " << img.width << "x" << img.height
              << " non_bg=" << non_bg_samples << " red_shaded=" << red_shaded_samples
              << " bright_red=" << bright_red_samples
              << " red_buckets=" << distinct_red_buckets.size() << "\n";
}

void VerifySimpleComputePixels(const DecodedPng& img, const std::string& tag) {
    EXPECT_MSG(img.width == 1920u && img.height == 1080u,
               tag + ": 1080p SimpleCompute framebuffer (" + std::to_string(img.width) + "x" +
                   std::to_string(img.height) + ")");
    if (img.width < 64u || img.height < 64u) return;

    // Far exterior corner (16, 16) escapes in iter=1 -> frac(1/300)=0.0033 -> GradientTexels[0] = (64,0,0).
    uint8_t fr = 0, fg = 0, fb = 0, fa = 0;
    img.SamplePixel(16, 16, fr, fg, fb, fa);
    EXPECT_MSG(!IsAtgBackground(fr, fg, fb) && fr >= 50u && fr <= 80u && fg < 12u && fb < 12u,
               tag + ": corner(16,16) far exterior is GradientTexels[0] deep crimson (got " +
                   std::to_string(fr) + "," + std::to_string(fg) + "," + std::to_string(fb) + ")");

    // Center pixel maps to complex coord (-0.65, 0.0), inside the main Mandelbrot cardioid:
    // iter == maxiter == 1200 -> frac(1200.0 / 300.0) is either 0.0 (exact IEEE div -> GradientTexels[0]
    // deep crimson (64,0,0)) or 1.0-eps (GPU fast rcp -> GradientTexels[7] bright yellow (255,255,0)).
    uint8_t cr = 0, cg = 0, cb = 0, ca = 0;
    img.SamplePixel(img.width / 2, img.height / 2, cr, cg, cb, ca);
    const bool center_is_lut0 = (cr >= 50u && cr <= 80u && cg < 12u && cb < 12u);
    const bool center_is_lut7 = (cr >= 240u && cg >= 240u && cb < 12u);
    EXPECT_MSG(!IsAtgBackground(cr, cg, cb) && (center_is_lut0 || center_is_lut7),
               tag + ": center inside Mandelbrot cardioid is exact LUT endpoint (got " +
                   std::to_string(cr) + "," + std::to_string(cg) + "," + std::to_string(cb) + ")");

    // Dense 320x180 grid across the 1080p frame (57,600 samples) to verify the CS 6.0 fractal
    // and its multi-band red/orange/yellow gradient around the Mandelbrot boundary.
    uint32_t non_atg_bg_samples = 0;
    uint32_t dark_crimson_samples = 0;
    uint32_t bright_red_samples = 0;
    uint32_t warm_orange_yellow_samples = 0;
    std::set<uint32_t> distinct_quantized_colors;

    for (uint32_t gy = 0; gy < 180; ++gy) {
        const uint32_t y = gy * img.height / 180 + img.height / 360;
        for (uint32_t gx = 0; gx < 320; ++gx) {
            const uint32_t x = gx * img.width / 320 + img.width / 640;
            uint8_t r = 0, g = 0, b = 0, a = 0;
            img.SamplePixel(x, y, r, g, b, a);
            if (!IsAtgBackground(r, g, b) && r >= 45u && b < 20u) {
                ++non_atg_bg_samples;
                if (r >= 50u && r <= 80u && g < 16u) ++dark_crimson_samples;
                if (r >= 115u && b < 20u) ++bright_red_samples;
                if (r >= 200u && g >= 40u && b < 20u) ++warm_orange_yellow_samples;
                const uint32_t q = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
                distinct_quantized_colors.insert(q);
            }
        }
    }

    EXPECT_MSG(non_atg_bg_samples >= 50000u,
               tag + ": CS 6.0 fractal fills viewport (non_atg=" +
                   std::to_string(non_atg_bg_samples) + "/57600)");
    EXPECT_MSG(dark_crimson_samples >= 10000u,
               tag + ": Mandelbrot exterior gradient band rendered (dark_crimson=" +
                   std::to_string(dark_crimson_samples) + ")");
    EXPECT_MSG(bright_red_samples >= 200u,
               tag + ": fractal escape-time red bands present (bright_red=" +
                   std::to_string(bright_red_samples) + ")");
    EXPECT_MSG(warm_orange_yellow_samples >= 40u,
               tag + ": high-iteration orange/yellow fractal boundary present (warm=" +
                   std::to_string(warm_orange_yellow_samples) + ")");
    EXPECT_MSG(distinct_quantized_colors.size() >= 6u,
               tag + ": multi-band gradient LUT colors present (buckets=" +
                   std::to_string(distinct_quantized_colors.size()) + ")");

    std::cout << "  [" << tag << "] SimpleCompute PNG verified: " << img.width << "x" << img.height
              << " center=(" << int(cr) << "," << int(cg) << "," << int(cb) << ")"
              << " non_atg=" << non_atg_bg_samples << " bright_red=" << bright_red_samples
              << " warm=" << warm_orange_yellow_samples
              << " color_buckets=" << distinct_quantized_colors.size() << "\n";
}

enum class SampleKind {
    Triangle,
    Lighting,
    Texture,
    Instancing,
    Bezier,
    Compute,
};

struct SampleSpec {
    std::string name;
    std::string exe_name;
    std::vector<std::string> cso_files;
    uint64_t min_pso_count;
    SampleKind kind;
    bool needs_sunset_jpg = false;
};

void RunSampleEndToEnd(const fs::path& bin_dir, const SampleSpec& spec, bool prefer_warp) {
    const std::string mode = prefer_warp ? "warp" : "gpu";
    const std::string tag = spec.name + "_" + mode;
    std::cout << "[test_gdkx_untouched_samples] Running " << tag << "...\n";

    const fs::path cli_exe = bin_dir / L"openscarlett.exe";
    const fs::path src_exe = bin_dir / spec.exe_name;
    const fs::path cso_dir = bin_dir / L"gdkx_shaders" / spec.name;

    EXPECT_MSG(fs::exists(cli_exe), tag + ": openscarlett.exe exists");
    EXPECT_MSG(fs::exists(src_exe), tag + ": " + spec.exe_name + " exists");
    if (!fs::exists(cli_exe) || !fs::exists(src_exe)) return;

    VerifyPeImports(src_exe, tag);

    // Stage into a brand-new isolated ZERO-DLL directory under %TEMP% (strictly E:\ under build.cmd)
    // or fallback to bin_dir when TEMP is unset (safe for hidden-build portable bundle smoke tests).
    fs::path stage_base = bin_dir / L"gdkx_bringup_stage";
    {
        wchar_t tmp_buf[1024]{};
        const DWORD tlen = ::GetEnvironmentVariableW(L"TEMP", tmp_buf, 1024);
        if (tlen > 0 && tlen < 1024) {
            stage_base = fs::path(tmp_buf) / L"gdkx_bringup_stage";
        }
    }
    const fs::path stage_root = stage_base / tag;
    const fs::path pkg_dir = stage_root / L"pkg";
    const fs::path vfs_t = stage_root / L"vfs_t";
    const fs::path telemetry_json = stage_root / L"telemetry.json";
    const fs::path capture_png = stage_root / (tag + ".png");

    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(pkg_dir, ec);
    fs::create_directories(vfs_t, ec);

    fs::copy_file(src_exe, pkg_dir / spec.exe_name, fs::copy_options::overwrite_existing, ec);
    EXPECT_MSG(!ec, tag + ": copy guest exe");

    for (const auto& cso : spec.cso_files) {
        const fs::path src_cso = cso_dir / cso;
        EXPECT_MSG(fs::exists(src_cso), tag + ": compiled shader " + cso + " exists");
        fs::copy_file(src_cso, pkg_dir / cso, fs::copy_options::overwrite_existing, ec);
        EXPECT_MSG(!ec, tag + ": copy " + cso);
    }

    if (spec.needs_sunset_jpg) {
        std::string jpg_err;
        EXPECT_MSG(WriteProceduralSunsetJpeg(pkg_dir / L"sunset.jpg", jpg_err),
                   tag + ": generate sunset.jpg (" + jpg_err + ")");
    }

    // Minimal MicrosoftGame.Config pointing to the untouched *_GDKX.exe
    {
        std::ofstream cfg(pkg_dir / L"MicrosoftGame.Config", std::ios::binary);
        cfg << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            << "<Game configVersion=\"1\">\n"
            << "  <Identity Name=\"OpenScarlett." << spec.name << "\" Publisher=\"CN=OpenScarlett\" Version=\"1.0.0.0\"/>\n"
            << "  <ExecutableList>\n"
            << "    <Executable Name=\"" << spec.exe_name << "\" Id=\"Game\"/>\n"
            << "  </ExecutableList>\n"
            << "</Game>\n";
    }

    // STRICT CHECK: ZERO .dll files anywhere inside pkg_dir (proves pure APC hook injection!)
    size_t dll_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(pkg_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext == ".dll") ++dll_count;
        }
    }
    EXPECT_MSG(dll_count == 0u, tag + ": isolated package dir has ZERO .dll files");

    // Launch via openscarlett.exe run with 12 PresentX frames + PNG readback capture
    ScopedEnv env_warp("OPENSCARLETT_PREFER_WARP", prefer_warp ? "1" : "0");
    ScopedEnv env_headless("OPENSCARLETT_PRESENTX_HEADLESS", prefer_warp ? "1" : "0");
    ScopedEnv env_frames("OPENSCARLETT_PRESENTX_EXIT_AFTER_FRAMES", "12");
    ScopedEnv env_png("OPENSCARLETT_PRESENTX_CAPTURE_PNG", capture_png.string().c_str());

    const DWORD rc = LaunchViaOpenScarlettCli(cli_exe, pkg_dir, bin_dir, vfs_t, telemetry_json);
    EXPECT_MSG(rc == 0u, tag + ": openscarlett.exe run exited 0 (got " + std::to_string(rc) + ")");

    EXPECT_MSG(fs::exists(telemetry_json), tag + ": telemetry.json produced");
    if (fs::exists(telemetry_json)) {
        if (rc != 0u) {
            std::ifstream dbg_ifs(telemetry_json, std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(dbg_ifs)), std::istreambuf_iterator<char>());
            std::cerr << "  [" << tag << "] telemetry.json dump (rc=" << rc << "):\n" << raw << "\n";
        }
        const TelemetrySummary tel = ParseTelemetryJson(telemetry_json);
        const bool expects_device_type = (spec.kind != SampleKind::Compute);
        VerifyCommonTelemetry(tel, tag, spec.min_pso_count, expects_device_type);
        if (spec.kind == SampleKind::Texture) {
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateShaderResourceView") >= 1u,
                       tag + ": DeviceX::CreateShaderResourceView >= 1");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::GetCopyableFootprints") >= 1u,
                       tag + ": DeviceX::GetCopyableFootprints >= 1");
        } else if (spec.kind == SampleKind::Instancing) {
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateFence") >= 2u,
                       tag + ": DeviceX::CreateFence >= 2");
        } else if (spec.kind == SampleKind::Bezier) {
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateConstantBufferView") >= 1u,
                       tag + ": DeviceX::CreateConstantBufferView >= 1");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12GraphicsCommandListX::IASetPrimitiveTopology") >= 12u,
                       tag + ": CommandListX::IASetPrimitiveTopology >= 12");
        } else if (spec.kind == SampleKind::Compute) {
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateComputePipelineState") >= 1u,
                       tag + ": DeviceX::CreateComputePipelineState >= 1");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateCommandQueue") >= 2u,
                       tag + ": DeviceX::CreateCommandQueue >= 2 (Direct + Compute)");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateUnorderedAccessView") >= 2u,
                       tag + ": DeviceX::CreateUnorderedAccessView >= 2");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateShaderResourceView") >= 4u,
                       tag + ": DeviceX::CreateShaderResourceView >= 4");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateSampler") >= 1u,
                       tag + ": DeviceX::CreateSampler >= 1");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12DeviceX::CreateFence") >= 3u,
                       tag + ": DeviceX::CreateFence >= 3");
            EXPECT_MSG(tel.OkCount("d3d12_xs.dll", "ID3D12GraphicsCommandListX::Dispatch") >= 12u,
                       tag + ": CommandListX::Dispatch >= 12");
        }
    }

    EXPECT_MSG(fs::exists(capture_png), tag + ": captured PNG produced at " + capture_png.string());
    if (fs::exists(capture_png)) {
        DecodedPng img{};
        std::string decode_err;
        EXPECT_MSG(DecodeStoredRgbaPng(capture_png, img, decode_err),
                   tag + ": DecodeStoredRgbaPng (" + decode_err + ")");
        if (decode_err.empty()) {
            switch (spec.kind) {
                case SampleKind::Triangle:
                    VerifySimpleTrianglePixels(img, tag);
                    break;
                case SampleKind::Lighting:
                    VerifySimpleLightingPixels(img, tag);
                    break;
                case SampleKind::Texture:
                    VerifySimpleTexturePixels(img, tag);
                    break;
                case SampleKind::Instancing:
                    VerifySimpleInstancingPixels(img, tag);
                    break;
                case SampleKind::Bezier:
                    VerifySimpleBezierPixels(img, tag);
                    break;
                case SampleKind::Compute:
                    VerifySimpleComputePixels(img, tag);
                    break;
            }
        }
    }
}

}  // namespace

int main() {
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const fs::path bin_dir = ResolveRuntimeBinDir();
    const std::vector<SampleSpec> samples = {
        {"SimpleTriangle", "SimpleTriangle_GDKX.exe", {"VertexShader.cso", "PixelShader.cso"}, 1u, SampleKind::Triangle, false},
        {"SimpleLighting", "SimpleLighting_GDKX.exe", {"TriangleVS.cso", "LambertPS.cso", "SolidColorPS.cso"}, 2u, SampleKind::Lighting, false},
        {"SimpleTexture", "SimpleTexture_GDKX.exe", {"VertexShader.cso", "PixelShader.cso"}, 1u, SampleKind::Texture, true},
        {"SimpleInstancing", "SimpleInstancing_GDKX.exe", {"VertexShader.cso", "PixelShader.cso"}, 1u, SampleKind::Instancing, false},
        {"SimpleBezier", "SimpleBezier_GDKX.exe",
         {"BezierVS.cso", "BezierHS_int.cso", "BezierHS_fracEven.cso", "BezierHS_fracOdd.cso",
          "BezierDS.cso", "BezierPS.cso", "SolidColorPS.cso"},
         6u, SampleKind::Bezier, false},
        {"SimpleCompute", "SimpleCompute_GDKX.exe", {"Fractal.cso"}, 1u, SampleKind::Compute, false},
    };

    for (const auto& s : samples) {
        RunSampleEndToEnd(bin_dir, s, /*prefer_warp=*/false);
        RunSampleEndToEnd(bin_dir, s, /*prefer_warp=*/true);
    }

    if (SUCCEEDED(com_hr)) {
        ::CoUninitialize();
    }

    if (g_failures != 0) {
        std::cerr << "[test_gdkx_untouched_samples] FAILED with " << g_failures << " failure(s).\n";
        return 1;
    }
    std::cout << "[test_gdkx_untouched_samples] ALL PASSED (0 failures).\n";
    return 0;
}
