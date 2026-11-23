#include "command_queue_x.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "command_list_x.h"
#include "device_x.h"
#include "proxies_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace openscarlett::d3d12_xs {

struct WindowCallbackState {
    std::atomic<uint32_t> client_width{0};
    std::atomic<uint32_t> client_height{0};
    std::atomic<bool> is_minimized{false};
    std::atomic<bool> is_active{true};
    std::atomic<bool> close_requested{false};
    std::atomic<bool> resize_pending{false};
    std::atomic<bool> post_quit_on_close{false};
    std::atomic<bool> quit_posted{false};
    std::atomic<bool> is_fullscreen{false};
    std::atomic<bool> in_fullscreen_toggle{false};
    std::atomic<uint64_t> alt_enter_toggle_count{0ull};

    std::mutex fullscreen_mutex;
    LONG_PTR saved_style = 0;
    LONG_PTR saved_ex_style = 0;
    RECT saved_rect{};
    WINDOWPLACEMENT saved_placement{};
    bool has_saved_window_state = false;
};

namespace {

constexpr const char* kModule = "d3d12_xs.dll";

inline void RecordQueue(const char* method, int32_t slot, HRESULT hr = S_OK) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        method,
        outcome,
        static_cast<int32_t>(hr),
        slot);
}

struct QueueRegistry {
    std::shared_mutex mutex;
    std::unordered_map<const void*, ID3D12CommandQueue*> map;
    std::vector<CommandQueueX*> active_queues;
};

QueueRegistry& GetQueueRegistry() {
    static QueueRegistry reg;
    return reg;
}

struct SubclassEntry {
    std::shared_ptr<WindowCallbackState> state;
    WNDPROC orig_proc = nullptr;
};

struct SubclassRegistry {
    std::mutex mutex;
    std::unordered_map<HWND, SubclassEntry> map;
};

SubclassRegistry& GetSubclassRegistry() {
    static SubclassRegistry reg;
    return reg;
}

void RefreshClientMetrics(HWND hwnd, WindowCallbackState& state) noexcept {
    RECT cr{};
    if (GetClientRect(hwnd, &cr) != FALSE) {
        const uint32_t cw = (cr.right > cr.left) ? static_cast<uint32_t>(cr.right - cr.left) : 0u;
        const uint32_t ch = (cr.bottom > cr.top) ? static_cast<uint32_t>(cr.bottom - cr.top) : 0u;
        state.client_width.store(cw, std::memory_order_release);
        state.client_height.store(ch, std::memory_order_release);
        state.is_minimized.store(
            (IsIconic(hwnd) != FALSE) || cw == 0u || ch == 0u, std::memory_order_release);
    }
}

HRESULT ToggleBorderlessFullscreenForWindow(HWND hwnd, WindowCallbackState& state) {
    if (!hwnd || IsWindow(hwnd) == FALSE) {
        return E_HANDLE;
    }
    if (state.in_fullscreen_toggle.exchange(true, std::memory_order_acq_rel)) {
        return S_OK;
    }
    struct ScopeReset {
        std::atomic<bool>& flag;
        ~ScopeReset() { flag.store(false, std::memory_order_release); }
    } reset_guard{state.in_fullscreen_toggle};

    const bool was_fullscreen = state.is_fullscreen.load(std::memory_order_acquire);
    if (!was_fullscreen) {
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &wp);

        {
            std::lock_guard<std::mutex> lk(state.fullscreen_mutex);
            state.saved_style = style;
            state.saved_ex_style = ex_style;
            state.saved_rect = rect;
            state.saved_placement = wp;
            state.has_saved_window_state = true;
        }

        RECT target_rect = rect;
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi) != FALSE) {
            target_rect = mi.rcMonitor;
        } else {
            target_rect.left = 0;
            target_rect.top = 0;
            target_rect.right = GetSystemMetrics(SM_CXSCREEN);
            target_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        }

        const bool was_visible = (style & WS_VISIBLE) != 0;
        const LONG_PTR new_style =
            static_cast<LONG_PTR>(WS_POPUP) | (was_visible ? static_cast<LONG_PTR>(WS_VISIBLE) : 0);
        const LONG_PTR ex_strip = static_cast<LONG_PTR>(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        const LONG_PTR new_ex_style = ex_style & ~ex_strip;

        SetWindowLongPtrW(hwnd, GWL_STYLE, new_style);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_ex_style);

        const UINT swp_flags = was_visible
                                   ? (SWP_FRAMECHANGED | SWP_NOOWNERZORDER)
                                   : (SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetWindowPos(
            hwnd,
            was_visible ? HWND_TOP : nullptr,
            target_rect.left,
            target_rect.top,
            target_rect.right - target_rect.left,
            target_rect.bottom - target_rect.top,
            swp_flags);

        state.is_fullscreen.store(true, std::memory_order_release);
        state.resize_pending.store(true, std::memory_order_release);
        RefreshClientMetrics(hwnd, state);
    } else {
        LONG_PTR saved_style = 0;
        LONG_PTR saved_ex_style = 0;
        RECT saved_rect{};
        WINDOWPLACEMENT saved_wp{};
        bool has_saved = false;
        {
            std::lock_guard<std::mutex> lk(state.fullscreen_mutex);
            saved_style = state.saved_style;
            saved_ex_style = state.saved_ex_style;
            saved_rect = state.saved_rect;
            saved_wp = state.saved_placement;
            has_saved = state.has_saved_window_state;
        }

        if (has_saved) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, saved_style);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, saved_ex_style);
            const bool restore_visible = (saved_style & WS_VISIBLE) != 0;
            const UINT swp_flags = restore_visible
                                       ? (SWP_FRAMECHANGED | SWP_NOOWNERZORDER)
                                       : (SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            SetWindowPos(
                hwnd,
                restore_visible ? HWND_NOTOPMOST : nullptr,
                saved_rect.left,
                saved_rect.top,
                saved_rect.right - saved_rect.left,
                saved_rect.bottom - saved_rect.top,
                swp_flags);
            if (restore_visible && saved_wp.showCmd == SW_SHOWMAXIMIZED) {
                SetWindowPlacement(hwnd, &saved_wp);
            }
        } else {
            const LONG_PTR cur_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            const bool cur_visible = (cur_style & WS_VISIBLE) != 0;
            const LONG_PTR fallback_style =
                static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW) |
                (cur_visible ? static_cast<LONG_PTR>(WS_VISIBLE) : 0);
            SetWindowLongPtrW(hwnd, GWL_STYLE, fallback_style);
            SetWindowPos(
                hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        state.is_fullscreen.store(false, std::memory_order_release);
        state.resize_pending.store(true, std::memory_order_release);
        RefreshClientMetrics(hwnd, state);
    }

    return S_OK;
}

LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    std::shared_ptr<WindowCallbackState> state;
    WNDPROC orig_proc = nullptr;
    {
        auto& reg = GetSubclassRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        auto it = reg.map.find(hwnd);
        if (it != reg.map.end()) {
            state = it->second.state;
            orig_proc = it->second.orig_proc;
        }
    }

    if (state) {
        switch (msg) {
            case WM_SIZE: {
                const uint32_t new_w = static_cast<uint32_t>(LOWORD(lParam));
                const uint32_t new_h = static_cast<uint32_t>(HIWORD(lParam));
                const bool min_now = (wParam == SIZE_MINIMIZED) || (new_w == 0u) || (new_h == 0u);
                state->client_width.store(new_w, std::memory_order_release);
                state->client_height.store(new_h, std::memory_order_release);
                state->is_minimized.store(min_now, std::memory_order_release);
                state->resize_pending.store(true, std::memory_order_release);
                break;
            }
            case WM_ACTIVATEAPP: {
                state->is_active.store(wParam != FALSE, std::memory_order_release);
                break;
            }
            case WM_SYSKEYDOWN: {
                const bool alt_down = (lParam & (static_cast<LPARAM>(1) << 29)) != 0;
                const bool is_repeat = (lParam & (static_cast<LPARAM>(1) << 30)) != 0;
                if (wParam == VK_RETURN && alt_down && !is_repeat) {
                    if (SUCCEEDED(ToggleBorderlessFullscreenForWindow(hwnd, *state))) {
                        state->alt_enter_toggle_count.fetch_add(1ull, std::memory_order_relaxed);
                    }
                    return 0;
                }
                break;
            }
            case WM_SYSCHAR: {
                const bool alt_down = (lParam & (static_cast<LPARAM>(1) << 29)) != 0;
                if (wParam == VK_RETURN && alt_down) {
                    return 0;
                }
                break;
            }
            case WM_CLOSE: {
                state->close_requested.store(true, std::memory_order_release);
                const bool post_quit = state->post_quit_on_close.load(std::memory_order_acquire);
                if (post_quit) {
                    if (!state->quit_posted.exchange(true, std::memory_order_acq_rel)) {
                        PostQuitMessage(0);
                    }
                }
                char term_buf[16]{};
                const DWORD tlen = GetEnvironmentVariableA(
                    "OPENSCARLETT_TERMINATE_ON_WINDOW_CLOSE", term_buf, static_cast<DWORD>(sizeof(term_buf)));
                if (tlen > 0u && tlen < sizeof(term_buf) &&
                    (term_buf[0] == '1' || term_buf[0] == 't' || term_buf[0] == 'T')) {
                    ExitProcess(0u);
                }
                break;
            }
            case WM_DESTROY: {
                state->close_requested.store(true, std::memory_order_release);
                if (state->post_quit_on_close.load(std::memory_order_acquire)) {
                    if (!state->quit_posted.exchange(true, std::memory_order_acq_rel)) {
                        PostQuitMessage(0);
                    }
                }
                break;
            }
            case WM_NCDESTROY: {
                LRESULT res = (orig_proc && orig_proc != &SubclassWndProc)
                                  ? CallWindowProcW(orig_proc, hwnd, msg, wParam, lParam)
                                  : DefWindowProcW(hwnd, msg, wParam, lParam);
                {
                    auto& reg = GetSubclassRegistry();
                    std::lock_guard<std::mutex> lk(reg.mutex);
                    auto it = reg.map.find(hwnd);
                    if (it != reg.map.end() && it->second.state == state) {
                        reg.map.erase(it);
                    }
                }
                if (orig_proc && orig_proc != &SubclassWndProc &&
                    GetWindowLongPtrW(hwnd, GWLP_WNDPROC) ==
                        reinterpret_cast<LONG_PTR>(&SubclassWndProc)) {
                    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig_proc));
                }
                return res;
            }
            default:
                break;
        }
    }

    if (orig_proc && orig_proc != &SubclassWndProc) {
        return CallWindowProcW(orig_proc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DXGI_FORMAT NormalizeSwapChainFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return fmt;
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

DXGI_COLOR_SPACE_TYPE ToDxgiColorSpace(
    DXGI_FORMAT sc_format,
    D3D12XBOX_COLOR_SPACE cs) noexcept {
    switch (cs) {
        case D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        case D3D12XBOX_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        case D3D12XBOX_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
            return DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709;
        case D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        default:
            if (sc_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            }
            return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
}

bool IsHdrSwapChainMode(DXGI_FORMAT fmt, D3D12XBOX_COLOR_SPACE cs) noexcept {
    return (cs == D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ||
           (cs == D3D12XBOX_COLOR_SPACE_RGB_FULL_G10_NONE_P709) ||
           (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
           (fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS);
}

float HalfToFloat(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
    const uint32_t mant = static_cast<uint32_t>(h) & 0x03FFu;
    uint32_t f_bits = 0u;
    if (exp == 0u) {
        if (mant == 0u) {
            f_bits = sign;
        } else {
            // Subnormal half-float
            uint32_t m = mant;
            int32_t e = -14;
            while ((m & 0x0400u) == 0u) {
                m <<= 1;
                --e;
            }
            m &= 0x03FFu;
            f_bits = sign | (static_cast<uint32_t>(e + 127) << 23) | (m << 13);
        }
    } else if (exp == 31u) {
        f_bits = sign | 0x7F800000u | (mant << 13);
    } else {
        f_bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &f_bits, sizeof(out));
    return out;
}

void ConvertBt2020LinearToRec709Linear(
    float r2020, float g2020, float b2020,
    float& r709, float& g709, float& b709) noexcept {
    r709 = std::max(0.0f, 1.6604910f * r2020 - 0.5876411f * g2020 - 0.0728499f * b2020);
    g709 = std::max(0.0f, -0.1245505f * r2020 + 1.1328999f * g2020 - 0.0083494f * b2020);
    b709 = std::max(0.0f, -0.0181508f * r2020 - 0.1005789f * g2020 + 1.1187297f * b2020);
}

void ConvertPackedRowToSdrRgba8(
    DXGI_FORMAT src_format,
    D3D12XBOX_COLOR_SPACE color_space,
    D3D12XBOX_TONEMAP_MODE tonemap_mode,
    float paper_white_nits,
    const uint8_t* src_row,
    uint8_t* dst_rgba8_row,
    uint32_t width) noexcept {
    const float ref_white = (paper_white_nits > 1.0f) ? paper_white_nits : 200.0f;

    if (src_format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        src_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        src_format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        for (uint32_t x = 0; x < width; ++x) {
            dst_rgba8_row[x * 4u + 0u] = src_row[x * 4u + 2u];
            dst_rgba8_row[x * 4u + 1u] = src_row[x * 4u + 1u];
            dst_rgba8_row[x * 4u + 2u] = src_row[x * 4u + 0u];
            dst_rgba8_row[x * 4u + 3u] = src_row[x * 4u + 3u];
        }
        return;
    }

    if (src_format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        src_format == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
        const auto* px32 = reinterpret_cast<const uint32_t*>(src_row);
        const bool is_pq = (color_space == D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t v = px32[x];
            const float r_n = static_cast<float>(v & 0x3FFu) / 1023.0f;
            const float g_n = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
            const float b_n = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
            const float a_n = static_cast<float>((v >> 30) & 0x3u) / 3.0f;
            if (is_pq) {
                const float r2020 = OpenScarlettD3D12PqSt2084ToLinearNits(r_n) / ref_white;
                const float g2020 = OpenScarlettD3D12PqSt2084ToLinearNits(g_n) / ref_white;
                const float b2020 = OpenScarlettD3D12PqSt2084ToLinearNits(b_n) / ref_white;
                float r709 = 0.0f;
                float g709 = 0.0f;
                float b709 = 0.0f;
                ConvertBt2020LinearToRec709Linear(r2020, g2020, b2020, r709, g709, b709);
                OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(
                    r709, g709, b709, a_n, tonemap_mode, &dst_rgba8_row[x * 4u]);
            } else {
                dst_rgba8_row[x * 4u + 0u] = static_cast<uint8_t>(std::lround(r_n * 255.0f));
                dst_rgba8_row[x * 4u + 1u] = static_cast<uint8_t>(std::lround(g_n * 255.0f));
                dst_rgba8_row[x * 4u + 2u] = static_cast<uint8_t>(std::lround(b_n * 255.0f));
                dst_rgba8_row[x * 4u + 3u] = static_cast<uint8_t>(std::lround(a_n * 255.0f));
            }
        }
        return;
    }

    if (src_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        src_format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
        // scRGB linear FP16 convention: 1.0 = 80 nits; normalize to reference white nits.
        const float scrgb_scale = 80.0f / ref_white;
        const auto* px16 = reinterpret_cast<const uint16_t*>(src_row);
        for (uint32_t x = 0; x < width; ++x) {
            const float r_lin = HalfToFloat(px16[x * 4u + 0u]) * scrgb_scale;
            const float g_lin = HalfToFloat(px16[x * 4u + 1u]) * scrgb_scale;
            const float b_lin = HalfToFloat(px16[x * 4u + 2u]) * scrgb_scale;
            const float a_lin = HalfToFloat(px16[x * 4u + 3u]);
            OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(
                r_lin, g_lin, b_lin, a_lin, tonemap_mode, &dst_rgba8_row[x * 4u]);
        }
        return;
    }

    std::memcpy(dst_rgba8_row, src_row, static_cast<size_t>(width) * 4u);
}

int64_t QueryQpcNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

bool IsWritableCpuVa(uint64_t va, size_t byte_size) noexcept {
    if (va == 0ull || byte_size == 0u || va > UINT64_MAX - byte_size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(va)), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kWritable) == 0u || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const uint64_t region_end =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
        static_cast<uint64_t>(mbi.RegionSize);
    return (va + byte_size) <= region_end;
}

bool TryParseEnvFcapFps(uint32_t& out_fps) noexcept {
    out_fps = 0u;
    char buf[64]{};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_FCAP", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(buf, &end, 10);
    if (!end || end == buf) return false;
    out_fps = static_cast<uint32_t>(std::min<unsigned long>(v, 10000ul));
    return true;
}

bool TryParseCmdLineFcapFps(uint32_t& out_fps) noexcept {
    out_fps = 0u;
    const char* cmd = GetCommandLineA();
    if (!cmd) return false;
    const char* p = cmd;
    while ((p = std::strstr(p, "--fcap")) != nullptr) {
        const bool boundary_before = (p == cmd || p[-1] == ' ' || p[-1] == '\t');
        const char* after = p + 6;
        if (boundary_before && (*after == ' ' || *after == '\t' || *after == '=')) {
            while (*after == ' ' || *after == '\t' || *after == '=') ++after;
            if (*after >= '0' && *after <= '9') {
                char* end = nullptr;
                const unsigned long v = std::strtoul(after, &end, 10);
                if (end && end != after) {
                    out_fps = static_cast<uint32_t>(std::min<unsigned long>(v, 10000ul));
                    return true;
                }
            }
        }
        p += 6;
    }
    return false;
}

bool IsReadableCpuVa(uint64_t va, size_t byte_size) noexcept {
    if (va == 0ull || byte_size == 0u || va > UINT64_MAX - byte_size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(va)), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kReadable =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kReadable) == 0u || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const uint64_t region_end =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
        static_cast<uint64_t>(mbi.RegionSize);
    return (va + byte_size) <= region_end;
}

template <typename T>
bool EvaluateWaitOp(T current, T reference, T mask, D3D12XBOX_WAIT_VALUE_OP op) noexcept {
    const T lhs = current & mask;
    const T rhs = reference & mask;
    switch (op) {
        case D3D12XBOX_WAIT_VALUE_OP_ALWAYS:        return true;
        case D3D12XBOX_WAIT_VALUE_OP_LESS:          return lhs < rhs;
        case D3D12XBOX_WAIT_VALUE_OP_LESS_EQUAL:    return lhs <= rhs;
        case D3D12XBOX_WAIT_VALUE_OP_EQUAL:         return lhs == rhs;
        case D3D12XBOX_WAIT_VALUE_OP_NOT_EQUAL:     return lhs != rhs;
        case D3D12XBOX_WAIT_VALUE_OP_GREATER_EQUAL: return lhs >= rhs;
        case D3D12XBOX_WAIT_VALUE_OP_GREATER:       return lhs > rhs;
        default:                                    return false;
    }
}

D3D12XBOX_COLOR_SPACE DxgiToXboxColorSpace(DXGI_COLOR_SPACE_TYPE dxgi_cs) noexcept {
    switch (dxgi_cs) {
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            return D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return D3D12XBOX_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
            return D3D12XBOX_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709;
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        default:
            return D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
}

std::string ReadEnvStringA(const char* name) {
    char buf[1024]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return {};
    return std::string(buf, len);
}

uint64_t ReadEnvUint64A(const char* name) noexcept {
    char buf[64]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return 0ull;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(buf, &end, 10);
    return (end && end != buf) ? static_cast<uint64_t>(v) : 0ull;
}

bool EnvFlagTrueA(const char* name) noexcept {
    char buf[32]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

struct FindMainWindowCtx {
    DWORD pid = 0;
    DWORD preferred_tid = 0;
    HWND best_same_thread = nullptr;
    HWND best_any_thread = nullptr;
    HWND best_hidden_same_thread = nullptr;
};

BOOL CALLBACK EnumProcessMainWindowProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindMainWindowCtx*>(lParam);
    DWORD wpid = 0;
    const DWORD wtid = GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) > 0) {
        if (wcscmp(cls, L"ConsoleWindowClass") == 0 ||
            wcscmp(cls, L"MSCTFIME UI") == 0 ||
            wcscmp(cls, L"IME") == 0 ||
            wcscmp(cls, L"Default IME") == 0 ||
            wcscmp(cls, L"tooltips_class32") == 0 ||
            wcscmp(cls, L"DDEMLEvent") == 0 ||
            wcscmp(cls, L"CicMarshalWndClass") == 0 ||
            wcscmp(cls, L"GDI+ Hook Window Class") == 0 ||
            wcscmp(cls, L"OleMainThreadWndClass") == 0) {
            return TRUE;
        }
    }
    RECT cr{};
    if (GetClientRect(hwnd, &cr) == FALSE || (cr.right - cr.left) < 64 || (cr.bottom - cr.top) < 64) {
        return TRUE;
    }
    const bool visible = (IsWindowVisible(hwnd) != FALSE);
    if (visible) {
        if (wtid == ctx->preferred_tid) {
            ctx->best_same_thread = hwnd;
            return FALSE;  // Exact visible match on calling thread
        }
        if (!ctx->best_any_thread) {
            ctx->best_any_thread = hwnd;
        }
    } else if (wtid == ctx->preferred_tid && !ctx->best_hidden_same_thread) {
        // Title-created offscreen/hidden console window fallback (e.g. Minecraft/GDK startup or headless CI)
        if (GetWindowTextLengthW(hwnd) > 0) {
            ctx->best_hidden_same_thread = hwnd;
        }
    }
    return TRUE;
}

HWND DiscoverProcessMainWindow() noexcept {
    FindMainWindowCtx ctx{};
    ctx.pid = GetCurrentProcessId();
    ctx.preferred_tid = GetCurrentThreadId();
    EnumThreadWindows(ctx.preferred_tid, &EnumProcessMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.best_same_thread) {
        return ctx.best_same_thread;
    }
    EnumWindows(&EnumProcessMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.best_same_thread) return ctx.best_same_thread;
    if (ctx.best_any_thread) return ctx.best_any_thread;
    return ctx.best_hidden_same_thread;
}

}  // namespace

// ---------------------------------------------------------------------------
// Sprint 14: FenceWaitDispatcher — dispatches callbacks strictly OUTSIDE mutex
// and wakes all blocked waits immediately on device removal (Leonida #1933).
// ---------------------------------------------------------------------------
FenceWaitDispatcher::FenceWaitDispatcher(DeviceX* parent_device)
    : device_x_(parent_device) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeup_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    device_removed_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

FenceWaitDispatcher::~FenceWaitDispatcher() {
    std::vector<PendingEntry> remaining;
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (stop_event_) SetEvent(stop_event_);
        if (wakeup_event_) SetEvent(wakeup_event_);
        if (device_removed_event_) SetEvent(device_removed_event_);
        if (worker_started_ && worker_thread_.joinable()) {
            to_join = std::move(worker_thread_);
            worker_started_ = false;
        }
    }
    cv_.notify_all();
    if (to_join.joinable()) {
        to_join.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining.swap(pending_);
    }
    for (auto& e : remaining) {
        if (e.event_handle) CloseHandle(e.event_handle);
        if (e.host_fence) e.host_fence->Release();
        if (e.user_fence) e.user_fence->Release();
    }
    if (device_removed_event_) CloseHandle(device_removed_event_);
    if (wakeup_event_) CloseHandle(wakeup_event_);
    if (stop_event_) CloseHandle(stop_event_);
}

void FenceWaitDispatcher::EnsureWorkerStartedLocked() {
    if (worker_started_ || stopping_) return;
    worker_started_ = true;
    worker_thread_ = std::thread([this]() { WorkerLoop(); });
}

void FenceWaitDispatcher::DispatchOutsideLock(std::vector<PendingEntry>& ready) {
    for (auto& e : ready) {
        if (e.callback) {
            e.callback(e.user_fence, e.target_value, e.status, e.user_ctx);
        }
        if (e.event_handle) {
            CloseHandle(e.event_handle);
            e.event_handle = nullptr;
        }
        if (e.host_fence) {
            e.host_fence->Release();
            e.host_fence = nullptr;
        }
        if (e.user_fence) {
            e.user_fence->Release();
            e.user_fence = nullptr;
        }
    }
    ready.clear();
}

HRESULT FenceWaitDispatcher::RegisterCallback(
    ID3D12Fence* fence,
    UINT64 target_value,
    FenceCompletionCallbackFn callback,
    void* user_ctx,
    uint64_t* out_registration_id) {
    if (!fence || !callback) return E_INVALIDARG;
    ID3D12Fence* host_fence = UnwrapFence(fence);
    if (!host_fence) return E_INVALIDARG;

    const uint64_t reg_id = next_id_.fetch_add(1ull, std::memory_order_relaxed);
    if (out_registration_id) *out_registration_id = reg_id;

    // Check immediate device removal or already-completed fence.
    HRESULT immediate_status = S_OK;
    bool fire_immediately = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_removed_.load(std::memory_order_acquire)) {
            immediate_status = removed_reason_.load(std::memory_order_acquire);
            if (SUCCEEDED(immediate_status)) immediate_status = DXGI_ERROR_DEVICE_REMOVED;
            fire_immediately = true;
        } else if (device_x_) {
            const HRESULT dev_hr = device_x_->GetDeviceRemovedReason();
            if (FAILED(dev_hr)) {
                device_removed_.store(true, std::memory_order_release);
                removed_reason_.store(dev_hr, std::memory_order_release);
                if (device_removed_event_) SetEvent(device_removed_event_);
                immediate_status = dev_hr;
                fire_immediately = true;
            }
        }
        if (!fire_immediately) {
            const UINT64 cur_val = host_fence->GetCompletedValue();
            if (cur_val == UINT64_MAX) {
                HRESULT dev_hr = device_x_ ? device_x_->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
                if (SUCCEEDED(dev_hr)) dev_hr = DXGI_ERROR_DEVICE_REMOVED;
                device_removed_.store(true, std::memory_order_release);
                removed_reason_.store(dev_hr, std::memory_order_release);
                if (device_removed_event_) SetEvent(device_removed_event_);
                immediate_status = dev_hr;
                fire_immediately = true;
            } else if (cur_val >= target_value) {
                immediate_status = S_OK;
                fire_immediately = true;
            }
        }
        if (!fire_immediately) {
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ev) return E_OUTOFMEMORY;
            const HRESULT set_hr = host_fence->SetEventOnCompletion(target_value, ev);
            if (FAILED(set_hr)) {
                CloseHandle(ev);
                return set_hr;
            }
            PendingEntry entry{};
            entry.id = reg_id;
            entry.user_fence = fence;
            entry.host_fence = host_fence;
            entry.user_fence->AddRef();
            entry.host_fence->AddRef();
            entry.target_value = target_value;
            entry.callback = callback;
            entry.user_ctx = user_ctx;
            entry.event_handle = ev;
            entry.status = S_OK;
            pending_.push_back(entry);
            EnsureWorkerStartedLocked();
            if (wakeup_event_) SetEvent(wakeup_event_);
        }
    }

    if (fire_immediately) {
        // Dispatched strictly OUTSIDE mutex_ so re-entrant callbacks never deadlock!
        callback(fence, target_value, immediate_status, user_ctx);
    }
    return S_OK;
}

HRESULT FenceWaitDispatcher::WaitForFenceValue(
    ID3D12Fence* fence,
    UINT64 target_value,
    UINT timeout_ms) {
    if (!fence) return E_INVALIDARG;
    ID3D12Fence* host_fence = UnwrapFence(fence);
    if (!host_fence) return E_INVALIDARG;

    auto check_removed = [&]() noexcept -> HRESULT {
        if (device_removed_.load(std::memory_order_acquire)) {
            HRESULT r = removed_reason_.load(std::memory_order_acquire);
            return FAILED(r) ? r : DXGI_ERROR_DEVICE_REMOVED;
        }
        if (device_x_) {
            const HRESULT r = device_x_->GetDeviceRemovedReason();
            if (FAILED(r)) {
                NotifyDeviceRemoved(r);
                return r;
            }
        }
        return S_OK;
    };

    const HRESULT initial_rm = check_removed();
    if (FAILED(initial_rm)) return initial_rm;

    const UINT64 cur_val = host_fence->GetCompletedValue();
    if (cur_val == UINT64_MAX) {
        HRESULT r = check_removed();
        if (SUCCEEDED(r)) {
            r = DXGI_ERROR_DEVICE_REMOVED;
            NotifyDeviceRemoved(r);
        }
        return r;
    }
    if (cur_val >= target_value) {
        return S_OK;
    }

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return E_OUTOFMEMORY;

    const HRESULT set_hr = host_fence->SetEventOnCompletion(target_value, ev);
    if (FAILED(set_hr)) {
        CloseHandle(ev);
        const HRESULT rm = check_removed();
        return FAILED(rm) ? rm : set_hr;
    }

    HANDLE handles[2] = {device_removed_event_, ev};
    const DWORD handle_count = device_removed_event_ ? 2u : 1u;
    const DWORD wr = WaitForMultipleObjects(
        handle_count,
        device_removed_event_ ? handles : &ev,
        FALSE,
        timeout_ms);
    CloseHandle(ev);

    if (device_removed_event_ && wr == WAIT_OBJECT_0) {
        HRESULT r = removed_reason_.load(std::memory_order_acquire);
        return FAILED(r) ? r : DXGI_ERROR_DEVICE_REMOVED;
    }
    if (wr == WAIT_TIMEOUT) {
        const HRESULT rm = check_removed();
        return FAILED(rm) ? rm : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (wr == WAIT_FAILED) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const HRESULT post_rm = check_removed();
    if (FAILED(post_rm)) return post_rm;
    if (host_fence->GetCompletedValue() == UINT64_MAX) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    return S_OK;
}

uint32_t FenceWaitDispatcher::PumpReadyCallbacks() {
    std::vector<PendingEntry> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool removed = device_removed_.load(std::memory_order_acquire);
        HRESULT rm_hr = removed_reason_.load(std::memory_order_acquire);
        if (!removed && device_x_) {
            const HRESULT dev_hr = device_x_->GetDeviceRemovedReason();
            if (FAILED(dev_hr)) {
                removed = true;
                rm_hr = dev_hr;
                device_removed_.store(true, std::memory_order_release);
                removed_reason_.store(dev_hr, std::memory_order_release);
                if (device_removed_event_) SetEvent(device_removed_event_);
            }
        }
        if (removed && SUCCEEDED(rm_hr)) {
            rm_hr = DXGI_ERROR_DEVICE_REMOVED;
        }

        auto it = pending_.begin();
        while (it != pending_.end()) {
            bool done = false;
            HRESULT st = S_OK;
            if (removed) {
                done = true;
                st = rm_hr;
            } else if (it->host_fence) {
                const UINT64 cv = it->host_fence->GetCompletedValue();
                if (cv == UINT64_MAX) {
                    done = true;
                    st = DXGI_ERROR_DEVICE_REMOVED;
                } else if (cv >= it->target_value) {
                    done = true;
                    st = S_OK;
                }
            } else {
                done = true;
                st = E_FAIL;
            }
            if (done) {
                it->status = st;
                ready.push_back(*it);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    const uint32_t count = static_cast<uint32_t>(ready.size());
    // Dispatched strictly OUTSIDE mutex_!
    DispatchOutsideLock(ready);
    return count;
}

void FenceWaitDispatcher::NotifyDeviceRemoved(HRESULT removed_reason) {
    if (SUCCEEDED(removed_reason)) {
        removed_reason = DXGI_ERROR_DEVICE_REMOVED;
    }
    std::vector<PendingEntry> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device_removed_.store(true, std::memory_order_release);
        removed_reason_.store(removed_reason, std::memory_order_release);
        if (device_removed_event_) SetEvent(device_removed_event_);
        if (wakeup_event_) SetEvent(wakeup_event_);
        ready.swap(pending_);
        for (auto& e : ready) {
            e.status = removed_reason;
        }
    }
    cv_.notify_all();
    // Dispatch all cancelled/aborted callbacks strictly OUTSIDE mutex_!
    DispatchOutsideLock(ready);
}

void FenceWaitDispatcher::WorkerLoop() {
    for (;;) {
        HANDLE wait_handles[MAXIMUM_WAIT_OBJECTS]{};
        DWORD handle_count = 0u;
        bool has_overflow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
            if (stop_event_) wait_handles[handle_count++] = stop_event_;
            if (wakeup_event_) wait_handles[handle_count++] = wakeup_event_;
            if (device_removed_event_) wait_handles[handle_count++] = device_removed_event_;
            for (const auto& e : pending_) {
                if (!e.event_handle) continue;
                if (handle_count < MAXIMUM_WAIT_OBJECTS) {
                    wait_handles[handle_count++] = e.event_handle;
                } else {
                    has_overflow = true;
                    break;
                }
            }
        }

        const DWORD timeout = has_overflow ? 2u : 50u;
        if (handle_count > 0u) {
            WaitForMultipleObjects(handle_count, wait_handles, FALSE, timeout);
        } else {
            Sleep(10u);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
        }
        (void)PumpReadyCallbacks();
    }
}

CommandQueueX::CommandQueueX(
    DeviceX* parent_device,
    ID3D12CommandQueue* host_queue,
    HWND initial_hwnd)
    : device_x_(parent_device),
      host_queue_(host_queue),
      attached_hwnd_(initial_hwnd),
      window_state_(std::make_shared<WindowCallbackState>()),
      fence_dispatcher_(std::make_unique<FenceWaitDispatcher>(parent_device)) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpc_freq_ = freq.QuadPart;

    if (device_x_) device_x_->AddRef();
    if (host_queue_) {
        host_queue_->AddRef();
        queue_type_ = host_queue_->GetDesc().Type;
    }
    {
        auto& reg = GetQueueRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map[static_cast<const ID3D12Pageable*>(this)] = host_queue_;
        reg.active_queues.push_back(this);
    }
    if (initial_hwnd && IsWindow(initial_hwnd) != FALSE) {
        std::lock_guard<std::mutex> lock(present_mutex_);
        AttachWindowSubclassLocked(initial_hwnd);
    }
}

void CommandQueueX::SetSubmitHook(QueueSubmitHookFn fn, void* user_ctx) noexcept {
    submit_hook_ctx_.store(user_ctx, std::memory_order_release);
    submit_hook_.store(fn, std::memory_order_release);
}

HWND CommandQueueX::EffectiveOutputWindow() const noexcept {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (present_config_.OutputWindow && IsWindow(present_config_.OutputWindow) != FALSE) {
        return present_config_.OutputWindow;
    }
    if (attached_hwnd_ && IsWindow(attached_hwnd_) != FALSE) {
        return attached_hwnd_;
    }
    if (!explicit_window_detached_ && device_x_) {
        HWND dev_hwnd = device_x_->DefaultWindow();
        if (dev_hwnd && IsWindow(dev_hwnd) != FALSE) {
            return dev_hwnd;
        }
    }
    return nullptr;
}

bool CommandQueueX::AdoptTitleWindow(HWND hwnd) noexcept {
    if (!hwnd || IsWindow(hwnd) == FALSE) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        explicit_window_detached_ = false;
        if (swapchain_ && swapchain_hwnd_ != hwnd) {
            ReleaseSwapChainLocked();
        }
        attached_hwnd_ = hwnd;
        AttachWindowSubclassLocked(hwnd);
    }
    if (device_x_) {
        device_x_->AdoptTitleWindow(hwnd);
    }
    return true;
}

HWND CommandQueueX::ResolveTargetOutputWindowLocked(
    const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters,
    bool force_headless) {
    if (force_headless) {
        return nullptr;
    }
    if (pPresentParameters && pPresentParameters->OutputWindow &&
        IsWindow(pPresentParameters->OutputWindow) != FALSE) {
        if (!attached_hwnd_) {
            attached_hwnd_ = pPresentParameters->OutputWindow;
            explicit_window_detached_ = false;
            if (device_x_ && !device_x_->DefaultWindow()) {
                device_x_->AdoptTitleWindow(pPresentParameters->OutputWindow);
            }
        }
        return pPresentParameters->OutputWindow;
    }
    if (present_config_.OutputWindow && IsWindow(present_config_.OutputWindow) != FALSE) {
        return present_config_.OutputWindow;
    }
    if (attached_hwnd_ && IsWindow(attached_hwnd_) != FALSE) {
        return attached_hwnd_;
    }
    if (attached_hwnd_ && IsWindow(attached_hwnd_) == FALSE) {
        // Attached window was destroyed; clear stale handle so a newly created title window can be adopted.
        attached_hwnd_ = nullptr;
    }
    if (!explicit_window_detached_) {
        if (device_x_) {
            HWND dev_hwnd = device_x_->DefaultWindow();
            if (dev_hwnd && IsWindow(dev_hwnd) != FALSE) {
                attached_hwnd_ = dev_hwnd;
                return dev_hwnd;
            }
        }
        HWND discovered = DiscoverProcessMainWindow();
        if (discovered && IsWindow(discovered) != FALSE) {
            attached_hwnd_ = discovered;
            if (device_x_ && !device_x_->DefaultWindow()) {
                device_x_->AdoptTitleWindow(discovered);
            }
            return discovered;
        }
    }
    return nullptr;
}

CommandQueueX::~CommandQueueX() {
    {
        auto& reg = GetQueueRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map.erase(static_cast<const ID3D12Pageable*>(this));
        auto it = std::find(reg.active_queues.begin(), reg.active_queues.end(), this);
        if (it != reg.active_queues.end()) {
            reg.active_queues.erase(it);
        }
    }
    fence_dispatcher_.reset();
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        ReleasePresentObjectsLocked();
    }
    if (host_queue_) host_queue_->Release();
    if (device_x_) device_x_->Release();
}

void CommandQueueX::NotifyDeviceRemovedForDevice(DeviceX* dev, HRESULT removed_reason) noexcept {
    if (!dev) return;
    std::vector<CommandQueueX*> targets;
    {
        auto& reg = GetQueueRegistry();
        std::shared_lock<std::shared_mutex> lock(reg.mutex);
        for (CommandQueueX* q : reg.active_queues) {
            if (q && q->device_x_ == dev) {
                q->AddRef();
                targets.push_back(q);
            }
        }
    }
    for (CommandQueueX* q : targets) {
        (void)q->NotifyDeviceRemoved(removed_reason);
        q->Release();
    }
}

ID3D12Pageable* CommandQueueX::UnwrapHostPageable(ID3D12Pageable* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetQueueRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(maybe_wrapped));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

ID3D12CommandQueue* CommandQueueX::UnwrapHostQueue(ID3D12CommandQueue* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetQueueRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(static_cast<ID3D12Pageable*>(maybe_wrapped)));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

// IUnknown (slots 0..2)
HRESULT STDMETHODCALLTYPE CommandQueueX::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, E_POINTER);
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12CommandQueue)) ||
        IsEqualIID(riid, IID_ID3D12CommandQueueX)) {
        *ppvObject = static_cast<ID3D12CommandQueueX*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettPresentCapture)) {
        *ppvObject = static_cast<IOpenScarlettPresentCapture*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettPresentWindow)) {
        *ppvObject = static_cast<IOpenScarlettPresentWindow*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettPresentHdr)) {
        *ppvObject = static_cast<IOpenScarlettPresentHdr*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettPresentTiming)) {
        *ppvObject = static_cast<IOpenScarlettPresentTiming*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettPresentPacing)) {
        *ppvObject = static_cast<IOpenScarlettPresentPacing*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettFenceWaitDispatcher)) {
        *ppvObject = static_cast<IOpenScarlettFenceWaitDispatcher*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, E_NOINTERFACE);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CommandQueueX::AddRef() {
    const ULONG refs = ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordQueue("ID3D12CommandQueueX::AddRef", 1, S_OK);
    return refs;
}

ULONG STDMETHODCALLTYPE CommandQueueX::Release() {
    RecordQueue("ID3D12CommandQueueX::Release", 2, S_OK);
    const ULONG rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete this;
    }
    return rem;
}

// ID3D12Object (slots 3..6)
HRESULT STDMETHODCALLTYPE CommandQueueX::GetPrivateData(
    REFGUID guid, UINT* pDataSize, void* pData) {
    const HRESULT hr = host_queue_->GetPrivateData(guid, pDataSize, pData);
    RecordQueue("ID3D12CommandQueueX::GetPrivateData", 3, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetPrivateData(
    REFGUID guid, UINT DataSize, const void* pData) {
    const HRESULT hr = host_queue_->SetPrivateData(guid, DataSize, pData);
    RecordQueue("ID3D12CommandQueueX::SetPrivateData", 4, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetPrivateDataInterface(
    REFGUID guid, const IUnknown* pData) {
    const HRESULT hr = host_queue_->SetPrivateDataInterface(guid, pData);
    RecordQueue("ID3D12CommandQueueX::SetPrivateDataInterface", 5, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetName(LPCWSTR Name) {
    const HRESULT hr = host_queue_->SetName(Name);
    RecordQueue("ID3D12CommandQueueX::SetName", 6, hr);
    return hr;
}

// ID3D12DeviceChild (slot 7)
HRESULT STDMETHODCALLTYPE CommandQueueX::GetDevice(REFIID riid, void** ppvDevice) {
    const HRESULT hr = device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
    RecordQueue("ID3D12CommandQueueX::GetDevice", 7, hr);
    return hr;
}

// ID3D12CommandQueue (slots 8..18)
void STDMETHODCALLTYPE CommandQueueX::UpdateTileMappings(
    ID3D12Resource* pResource,
    UINT NumResourceRegions,
    const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
    const D3D12_TILE_REGION_SIZE* pResourceRegionSizes,
    ID3D12Heap* pHeap,
    UINT NumRanges,
    const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
    const UINT* pHeapRangeStartOffsets,
    const UINT* pRangeTileCounts,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    host_queue_->UpdateTileMappings(
        UnwrapResource(pResource), NumResourceRegions, pResourceRegionStartCoordinates, pResourceRegionSizes,
        UnwrapHeap(pHeap), NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts, Flags);
    RecordQueue("ID3D12CommandQueueX::UpdateTileMappings", 8, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::CopyTileMappings(
    ID3D12Resource* pDstResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
    ID3D12Resource* pSrcResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pRegionSize,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    host_queue_->CopyTileMappings(
        UnwrapResource(pDstResource), pDstRegionStartCoordinate, UnwrapResource(pSrcResource),
        pSrcRegionStartCoordinate, pRegionSize, Flags);
    RecordQueue("ID3D12CommandQueueX::CopyTileMappings", 9, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::ExecuteCommandLists(
    UINT NumCommandLists,
    ID3D12CommandList* const* ppCommandLists) {
    // Auto-sync all dirty/modified unified-memory textures, unified heap buffers, & TitleVAMirror
    // pages BEFORE submitting host command lists so GPU shaders never sample stale CPU memory.
    if (device_x_ && host_queue_) {
        (void)device_x_->SyncAllUnifiedResourcesBeforeSubmit(host_queue_, queue_type_);
    }

    // Unwrap any CommandListX* wrappers back to native host ID3D12CommandList* pointers.
    if (NumCommandLists == 0 || !ppCommandLists) {
        host_queue_->ExecuteCommandLists(NumCommandLists, ppCommandLists);
        RecordQueue("ID3D12CommandQueueX::ExecuteCommandLists", 10, S_OK);
        return;
    }

    constexpr UINT kStackLimit = 64u;
    ID3D12CommandList* stack_lists[kStackLimit];
    std::vector<ID3D12CommandList*> heap_lists;
    ID3D12CommandList** raw_lists = stack_lists;

    if (NumCommandLists > kStackLimit) {
        heap_lists.resize(NumCommandLists);
        raw_lists = heap_lists.data();
    }

    std::vector<uint64_t> pending_writeback_vas;
    std::vector<DeferredCpuWrite32> deferred_writes;
    std::vector<DeferredCpuWait> deferred_waits;
    UINT valid_list_count = 0u;
    for (UINT i = 0; i < NumCommandLists; ++i) {
        if (CommandListX* clx = CommandListX::TryFromCommandList(ppCommandLists[i])) {
            if (clx->LastCloseFailed()) {
                continue;  // Never pass an unclosed/invalid command list to host D3D12 queue
            }
            clx->ExtractPendingMirrorWritebacks(pending_writeback_vas);
            clx->ExtractDeferredCpuOps(deferred_writes, deferred_waits);
        }
        raw_lists[valid_list_count++] = CommandListX::UnwrapHostList(ppCommandLists[i]);
    }

    // Enforce any CPU/mirrored Wait32BitValueX / Wait64BitValueX recorded on command lists.
    for (const auto& w : deferred_waits) {
        if (w.is_64bit) {
            (void)Wait64BitValueX(w.address, w.value, w.mask, w.op);
        } else {
            (void)Wait32BitValueX(
                w.address, static_cast<uint32_t>(w.value), static_cast<uint32_t>(w.mask), w.op);
        }
    }

    if (const auto hook = submit_hook_.load(std::memory_order_acquire)) {
        hook(host_queue_, valid_list_count, raw_lists, submit_hook_ctx_.load(std::memory_order_acquire));
    }

    if (valid_list_count > 0u) {
        host_queue_->ExecuteCommandLists(valid_list_count, raw_lists);
    }
    if (device_x_ && CommandListX::IsDiagTraceEnabled() && device_x_->HostDevice()) {
        // Flush this submission immediately so GetDeviceRemovedReason pinpoints the exact offending list.
        static std::mutex s_diag_mu;
        static std::atomic<bool> s_dumped{false};
        if (!s_dumped.load(std::memory_order_acquire)) {
            ID3D12Fence* tmp_fence = nullptr;
            if (SUCCEEDED(device_x_->HostDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tmp_fence))) && tmp_fence) {
                if (SUCCEEDED(host_queue_->Signal(tmp_fence, 1))) {
                    HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (ev) {
                        if (tmp_fence->GetCompletedValue() < 1) {
                            tmp_fence->SetEventOnCompletion(1, ev);
                            ::WaitForSingleObject(ev, 5000);
                        }
                        ::CloseHandle(ev);
                    }
                }
                tmp_fence->Release();
            }
            const HRESULT rem_hr = device_x_->HostDevice()->GetDeviceRemovedReason();
            if (FAILED(rem_hr) && !s_dumped.exchange(true)) {
                std::lock_guard<std::mutex> dlk(s_diag_mu);
                FILE* f = nullptr;
                if (::fopen_s(&f, CommandListX::DiagTraceLogPath(), "a") == 0 && f) {
                    std::fprintf(f, "\n=== DEVICE REMOVED AFTER ExecuteCommandLists(n=%u, valid=%u, queue_type=%d) hr=0x%08X ===\n",
                                 NumCommandLists, valid_list_count, static_cast<int>(queue_type_), static_cast<unsigned>(rem_hr));
                    for (UINT i = 0; i < NumCommandLists; ++i) {
                        if (CommandListX* clx = CommandListX::TryFromCommandList(ppCommandLists[i])) {
                            const auto ops = clx->CopyDiagOps();
                            std::fprintf(f, "--- List[%u] (%zu ops) ---\n", i, ops.size());
                            for (const auto& op : ops) {
                                std::fprintf(f, "  %s\n", op.c_str());
                            }
                        }
                    }
                    std::fclose(f);
                }
            }
        }
    }

    // Schedule fence-tracked GPU->CPU writeback for any TitleVAMirror resources written by these lists.
    if (device_x_ && !pending_writeback_vas.empty() &&
        host_queue_ && queue_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::sort(pending_writeback_vas.begin(), pending_writeback_vas.end());
        pending_writeback_vas.erase(
            std::unique(pending_writeback_vas.begin(), pending_writeback_vas.end()),
            pending_writeback_vas.end());
        std::lock_guard<std::mutex> lock(present_mutex_);
        if (SUCCEEDED(EnsurePresentHelperObjectsLocked())) {
            // Wait for any previous use of present_alloc_ before resetting it.
            if (present_fence_->GetCompletedValue() < present_fence_val_) {
                if (SUCCEEDED(present_fence_->SetEventOnCompletion(present_fence_val_, present_fence_event_))) {
                    WaitForSingleObject(present_fence_event_, 15000u);
                }
                PumpMirrorReadbackCompletionLocked();
            }
            if (SUCCEEDED(present_alloc_->Reset()) &&
                SUCCEEDED(present_cmd_->Reset(present_alloc_, nullptr))) {
                const uint64_t rb_fence = ++present_fence_val_;
                uint32_t scheduled = 0u;
                for (uint64_t va : pending_writeback_vas) {
                    openscarlett::TitleVaRegionInfo rinfo{};
                    if (device_x_->GetTitleVaMirror().LookupRegion(va, rinfo)) {
                        if (device_x_->GetTitleVaMirror().ScheduleGpuReadback(
                                present_cmd_, va, rb_fence, 0ull, 0ull, rinfo.current_state).ok()) {
                            ++scheduled;
                        }
                    }
                }
                if (scheduled > 0u && SUCCEEDED(present_cmd_->Close())) {
                    ID3D12CommandList* rb_lists[] = {present_cmd_};
                    host_queue_->ExecuteCommandLists(1u, rb_lists);
                    host_queue_->Signal(present_fence_, rb_fence);
                } else {
                    present_cmd_->Close();
                }
            }
        }
    }

    if (!deferred_writes.empty()) {
        {
            std::lock_guard<std::mutex> lock(present_mutex_);
            if (SUCCEEDED(EnsurePresentHelperObjectsLocked())) {
                WaitForInternalFenceLocked();
            }
        }
        for (const auto& wr : deferred_writes) {
            if (IsWritableCpuVa(wr.address, sizeof(uint32_t))) {
                *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(wr.address)) = wr.value;
            }
        }
    }

    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        for (UINT i = 0; i < NumCommandLists; ++i) {
            CommandListX* clx = CommandListX::TryFromCommandList(ppCommandLists[i]);
            if (clx) {
                VulkanRecordedBatch batch{};
                clx->CopyVulkanRecordedBatch(batch);
                ExecuteVulkanRecordedBatch(batch);
            }
        }
    }

    RecordQueue("ID3D12CommandQueueX::ExecuteCommandLists", 10, S_OK);
}

void CommandQueueX::ExecuteVulkanRecordedBatch(const VulkanRecordedBatch& batch) {
    if (!device_x_ || !device_x_->IsVulkanRoutingActive()) return;
    openscarlett::VulkanBackend* vk = device_x_->GetVulkanBackend();
    if (!vk || !vk->IsInitialized()) return;

    openscarlett::VulkanCommandListDesc vk_cl_desc{};
    vk_cl_desc.submit_empty_fence_sync = true;

    uint64_t draws_cnt = 0ull;
    uint64_t indexed_draws_cnt = 0ull;
    uint64_t dispatches_cnt = 0ull;
    uint64_t mesh_cnt = 0ull;
    uint64_t copies_cnt = 0ull;
    uint64_t barriers_cnt = 0ull;
    uint64_t rtv_clears_cnt = 0ull;
    uint64_t dsv_clears_cnt = 0ull;
    uint64_t icd_execs_cnt = 0ull;
    uint64_t spirv_draws_cnt = 0ull;
    uint64_t spirv_dispatches_cnt = 0ull;
    uint64_t spirv_mesh_cnt = 0ull;

    for (const auto& op : batch.ops) {
        switch (op.kind) {
            case VulkanRecordedOpKind::Barrier: {
                const uint32_t n = std::max<uint32_t>(1u, op.barrier_count);
                const uint32_t n_global = std::min<uint32_t>(n, op.global_uav_barrier_count);
                barriers_cnt += n;
                for (uint32_t b = 0; b < n; ++b) {
                    openscarlett::VulkanCommandItem item{};
                    item.op = (b < n_global)
                                  ? openscarlett::VulkanCommandOp::GlobalUavBarrier
                                  : openscarlett::VulkanCommandOp::PipelineBarrier;
                    vk_cl_desc.commands.push_back(item);
                }
                break;
            }
            case VulkanRecordedOpKind::ClearRtv: {
                ++rtv_clears_cnt;
                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::PipelineBarrier;
                vk_cl_desc.commands.push_back(item);
                break;
            }
            case VulkanRecordedOpKind::ClearDsv: {
                ++dsv_clears_cnt;
                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::PipelineBarrier;
                vk_cl_desc.commands.push_back(item);
                break;
            }
            case VulkanRecordedOpKind::CopyBuffer: {
                ++copies_cnt;
                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::PipelineBarrier;
                vk_cl_desc.commands.push_back(item);
                if (op.src_resource && op.dst_resource && op.num_bytes > 0ull) {
                    std::vector<uint8_t> shadow;
                    if (device_x_->ReadVulkanBufferData(op.src_resource, op.src_offset, op.num_bytes, shadow) &&
                        !shadow.empty()) {
                        // Wait for host queue so host CopyBufferRegion doesn't overwrite Vulkan compute results.
                        {
                            std::lock_guard<std::mutex> lk(present_mutex_);
                            if (SUCCEEDED(EnsurePresentHelperObjectsLocked())) {
                                WaitForInternalFenceLocked();
                            }
                        }
                        device_x_->WriteVulkanBufferData(
                            op.dst_resource, op.dst_offset, shadow.data(), shadow.size());
                    }
                }
                break;
            }
            case VulkanRecordedOpKind::CopyTexture:
            case VulkanRecordedOpKind::CopyResource: {
                ++copies_cnt;
                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::PipelineBarrier;
                vk_cl_desc.commands.push_back(item);
                if (op.src_resource && op.dst_resource) {
                    openscarlett::FrameBufferRgba8 src_fb{};
                    if (device_x_->TryGetVulkanRenderTargetFramebuffer(op.src_resource, src_fb)) {
                        device_x_->StoreVulkanRenderTargetFramebuffer(op.dst_resource, src_fb);
                    }
                }
                break;
            }
            case VulkanRecordedOpKind::DrawInstanced:
            case VulkanRecordedOpKind::DrawIndexedInstanced: {
                const bool indexed = (op.kind == VulkanRecordedOpKind::DrawIndexedInstanced);
                if (indexed) ++indexed_draws_cnt;
                else ++draws_cnt;

                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::Nop;
                item.x = op.x;
                item.y = op.y;
                vk_cl_desc.commands.push_back(item);

                if (op.is_spirv_direct && op.pso) {
                    VulkanPipelineInfo pi{};
                    if (device_x_->LookupVulkanPipeline(op.pso, pi) &&
                        !pi.vs_spirv.empty() && !pi.ps_spirv.empty()) {
                        openscarlett::VulkanGraphicsDrawDesc gdesc{};
                        gdesc.vertex_spirv = pi.vs_spirv;
                        gdesc.fragment_spirv = pi.ps_spirv;
                        gdesc.vertex_entry = pi.vs_entry;
                        gdesc.fragment_entry = pi.ps_entry;
                        gdesc.color_format = pi.color_format;
                        gdesc.enable_depth = pi.enable_depth;
                        gdesc.depth_write = pi.depth_write;
                        if (op.has_clear_color) {
                            std::memcpy(gdesc.clear_color, op.clear_color, sizeof(gdesc.clear_color));
                        }
                        gdesc.clear_depth = op.clear_depth;
                        if (op.rtv_resource) {
                            const D3D12_RESOURCE_DESC rdesc = op.rtv_resource->GetDesc();
                            gdesc.width = static_cast<uint32_t>(std::max<UINT64>(1ull, rdesc.Width));
                            gdesc.height = std::max<UINT>(1u, rdesc.Height);
                        } else if (op.viewport_width > 0u && op.viewport_height > 0u) {
                            gdesc.width = op.viewport_width;
                            gdesc.height = op.viewport_height;
                        }
                        if (op.has_vbv && op.vbv.BufferLocation != 0ull && op.vbv.SizeInBytes > 0u) {
                            uint64_t vb_off = 0ull;
                            ID3D12Resource* vb_res =
                                device_x_->ResolveVulkanBufferByGpuVa(op.vbv.BufferLocation, &vb_off);
                            if (vb_res) {
                                device_x_->ReadVulkanBufferData(
                                    vb_res, vb_off, op.vbv.SizeInBytes, gdesc.vertex_bytes);
                            }
                            gdesc.vertex_stride =
                                (op.vbv.StrideInBytes > 0u) ? op.vbv.StrideInBytes : pi.vertex_stride_hint;
                            gdesc.vertex_attributes = pi.vertex_attrs;
                        }
                        if (indexed && op.has_ibv && op.ibv.BufferLocation != 0ull && op.ibv.SizeInBytes > 0u) {
                            uint64_t ib_off = 0ull;
                            ID3D12Resource* ib_res =
                                device_x_->ResolveVulkanBufferByGpuVa(op.ibv.BufferLocation, &ib_off);
                            std::vector<uint8_t> raw_ib;
                            if (ib_res &&
                                device_x_->ReadVulkanBufferData(ib_res, ib_off, op.ibv.SizeInBytes, raw_ib)) {
                                if (op.ibv.Format == DXGI_FORMAT_R16_UINT) {
                                    const size_t cnt = std::min<size_t>(op.x, raw_ib.size() / 2u);
                                    gdesc.indices.resize(cnt);
                                    for (size_t k = 0; k < cnt; ++k) {
                                        uint16_t idx16 = 0u;
                                        std::memcpy(&idx16, raw_ib.data() + k * 2u, 2u);
                                        gdesc.indices[k] = static_cast<uint32_t>(idx16);
                                    }
                                } else {
                                    const size_t cnt = std::min<size_t>(op.x, raw_ib.size() / 4u);
                                    gdesc.indices.resize(cnt);
                                    std::memcpy(gdesc.indices.data(), raw_ib.data(), cnt * sizeof(uint32_t));
                                }
                            }
                        } else {
                            gdesc.vertex_count = std::max<uint32_t>(1u, op.x);
                        }
                        if (!op.root_constants.empty()) {
                            gdesc.push_constants.resize(op.root_constants.size() * sizeof(uint32_t));
                            std::memcpy(gdesc.push_constants.data(), op.root_constants.data(),
                                        gdesc.push_constants.size());
                        }
                        for (uint32_t bi = 0; bi < op.root_cbvs.size(); ++bi) {
                            if (op.root_cbvs[bi] == 0ull) continue;
                            uint64_t boff = 0ull;
                            ID3D12Resource* bres =
                                device_x_->ResolveVulkanBufferByGpuVa(op.root_cbvs[bi], &boff);
                            std::vector<uint8_t> bbytes;
                            if (bres && device_x_->ReadVulkanBufferData(bres, boff, 0ull, bbytes)) {
                                openscarlett::VulkanDescriptorBinding db{};
                                db.binding = bi;
                                db.kind = openscarlett::VulkanDescriptorKind::UniformBuffer;
                                db.buffer_data = std::move(bbytes);
                                gdesc.descriptors.push_back(std::move(db));
                            }
                        }
                        openscarlett::VulkanGraphicsDrawResult gres{};
                        if (vk->ExecuteGraphicsDraw(gdesc, gres).ok()) {
                            ++spirv_draws_cnt;
                            if (gres.executed_on_vulkan_icd) ++icd_execs_cnt;
                            if (op.rtv_resource) {
                                device_x_->StoreVulkanRenderTargetFramebuffer(op.rtv_resource, gres.framebuffer);
                            }
                        }
                    }
                }
                break;
            }
            case VulkanRecordedOpKind::Dispatch: {
                ++dispatches_cnt;
                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::Dispatch;
                item.x = std::max<uint32_t>(1u, op.x);
                item.y = std::max<uint32_t>(1u, op.y);
                item.z = std::max<uint32_t>(1u, op.z);
                vk_cl_desc.commands.push_back(item);

                if (op.is_spirv_direct && op.pso) {
                    VulkanPipelineInfo pi{};
                    if (device_x_->LookupVulkanPipeline(op.pso, pi) && !pi.cs_spirv.empty()) {
                        ID3D12Resource* in_res = nullptr;
                        uint64_t in_off = 0ull;
                        for (D3D12_GPU_VIRTUAL_ADDRESS va : op.root_srvs) {
                            if (va != 0ull) {
                                in_res = device_x_->ResolveVulkanBufferByGpuVa(va, &in_off);
                                if (in_res) break;
                            }
                        }
                        ID3D12Resource* out_res = nullptr;
                        uint64_t out_off = 0ull;
                        for (D3D12_GPU_VIRTUAL_ADDRESS va : op.root_uavs) {
                            if (va != 0ull) {
                                out_res = device_x_->ResolveVulkanBufferByGpuVa(va, &out_off);
                                if (out_res) break;
                            }
                        }
                        std::vector<uint8_t> raw_in;
                        if (in_res) {
                            device_x_->ReadVulkanBufferData(in_res, in_off, 0ull, raw_in);
                        }
                        std::vector<uint32_t> input_u32;
                        if (raw_in.size() >= 4u) {
                            input_u32.resize(raw_in.size() / 4u);
                            std::memcpy(input_u32.data(), raw_in.data(), input_u32.size() * sizeof(uint32_t));
                        } else {
                            input_u32.assign(std::max<uint32_t>(1u, op.x), 0u);
                        }
                        std::vector<uint32_t> output_u32;
                        openscarlett::VulkanComputeDispatchOptions copts{};
                        openscarlett::VulkanComputeDispatchResult cres{};
                        if (vk->ExecuteComputeShaderEx(
                                pi.cs_spirv, input_u32, output_u32,
                                std::max<uint32_t>(1u, op.x), copts, cres).ok()) {
                            ++spirv_dispatches_cnt;
                            if (cres.executed_on_vulkan_icd) ++icd_execs_cnt;
                            if (out_res && !output_u32.empty()) {
                                device_x_->WriteVulkanBufferData(
                                    out_res, out_off, output_u32.data(), output_u32.size() * sizeof(uint32_t));
                            }
                        }
                    }
                }
                break;
            }
            case VulkanRecordedOpKind::DispatchMesh: {
                ++mesh_cnt;
                openscarlett::VulkanCommandItem item{};
                item.op = openscarlett::VulkanCommandOp::Dispatch;
                item.x = std::max<uint32_t>(1u, op.x);
                item.y = std::max<uint32_t>(1u, op.y);
                item.z = std::max<uint32_t>(1u, op.z);
                vk_cl_desc.commands.push_back(item);

                if (op.is_spirv_direct && op.pso) {
                    VulkanPipelineInfo pi{};
                    if (device_x_->LookupVulkanPipeline(op.pso, pi) &&
                        !pi.ms_spirv.empty() && !pi.ps_spirv.empty()) {
                        openscarlett::VulkanMeshDrawDesc mdesc{};
                        mdesc.task_spirv = pi.as_spirv;
                        mdesc.mesh_spirv = pi.ms_spirv;
                        mdesc.fragment_spirv = pi.ps_spirv;
                        mdesc.task_entry = pi.as_entry;
                        mdesc.mesh_entry = pi.ms_entry;
                        mdesc.fragment_entry = pi.ps_entry;
                        mdesc.group_count_x = std::max<uint32_t>(1u, op.x);
                        mdesc.group_count_y = std::max<uint32_t>(1u, op.y);
                        mdesc.group_count_z = std::max<uint32_t>(1u, op.z);
                        mdesc.color_format = pi.color_format;
                        mdesc.enable_depth = pi.enable_depth;
                        if (op.has_clear_color) {
                            std::memcpy(mdesc.clear_color, op.clear_color, sizeof(mdesc.clear_color));
                        }
                        mdesc.clear_depth = op.clear_depth;
                        if (op.rtv_resource) {
                            const D3D12_RESOURCE_DESC rdesc = op.rtv_resource->GetDesc();
                            mdesc.width = static_cast<uint32_t>(std::max<UINT64>(1ull, rdesc.Width));
                            mdesc.height = std::max<UINT>(1u, rdesc.Height);
                        } else if (op.viewport_width > 0u && op.viewport_height > 0u) {
                            mdesc.width = op.viewport_width;
                            mdesc.height = op.viewport_height;
                        }
                        if (!op.root_constants.empty()) {
                            mdesc.push_constants.resize(op.root_constants.size() * sizeof(uint32_t));
                            std::memcpy(mdesc.push_constants.data(), op.root_constants.data(),
                                        mdesc.push_constants.size());
                        }
                        openscarlett::VulkanMeshDrawResult mres{};
                        const openscarlett::Status mst = vk->ExecuteMeshShaderDraw(mdesc, mres);
                        if (mst.ok()) {
                            ++spirv_mesh_cnt;
                            if (mres.executed_on_mesh_shader_hw) ++icd_execs_cnt;
                            if (op.rtv_resource) {
                                device_x_->StoreVulkanRenderTargetFramebuffer(op.rtv_resource, mres.framebuffer);
                            }
                        } else {
                            std::fprintf(stderr, "[OpenScarlett][d3d12_xs] Vulkan DispatchMesh failed: %s\n",
                                         mst.message.c_str());
                        }
                    }
                }
                break;
            }
        }
    }

    // Record & submit real VkCommandBuffer on the live Vulkan ICD for every routed command list.
    openscarlett::VulkanCommandListResult vk_cl_res{};
    if (vk->RecordCommandList(vk_cl_desc, vk_cl_res).ok()) {
        if (vk_cl_res.executed_on_vulkan_icd) {
            ++icd_execs_cnt;
        }
    }

    device_x_->RecordVulkanRoutingCounters(
        1ull, draws_cnt, indexed_draws_cnt, dispatches_cnt, mesh_cnt,
        copies_cnt, barriers_cnt, rtv_clears_cnt, dsv_clears_cnt,
        0ull, icd_execs_cnt, spirv_draws_cnt, spirv_dispatches_cnt, spirv_mesh_cnt);
}

void STDMETHODCALLTYPE CommandQueueX::SetMarker(UINT Metadata, const void* pData, UINT Size) {
    host_queue_->SetMarker(Metadata, pData, Size);
    RecordQueue("ID3D12CommandQueueX::SetMarker", 11, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::BeginEvent(UINT Metadata, const void* pData, UINT Size) {
    host_queue_->BeginEvent(Metadata, pData, Size);
    RecordQueue("ID3D12CommandQueueX::BeginEvent", 12, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::EndEvent() {
    host_queue_->EndEvent();
    RecordQueue("ID3D12CommandQueueX::EndEvent", 13, S_OK);
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Signal(ID3D12Fence* pFence, UINT64 Value) {
    const HRESULT hr = host_queue_->Signal(UnwrapFence(pFence), Value);
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        PumpMirrorReadbackCompletionLocked();
    }
    RecordQueue("ID3D12CommandQueueX::Signal", 14, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Wait(ID3D12Fence* pFence, UINT64 Value) {
    const HRESULT hr = host_queue_->Wait(UnwrapFence(pFence), Value);
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        PumpMirrorReadbackCompletionLocked();
    }
    RecordQueue("ID3D12CommandQueueX::Wait", 15, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetTimestampFrequency(UINT64* pFrequency) {
    if (!pFrequency) {
        RecordQueue("ID3D12CommandQueueX::GetTimestampFrequency", 16, E_POINTER);
        return E_POINTER;
    }
    HRESULT hr = host_queue_->GetTimestampFrequency(pFrequency);
    if (SUCCEEDED(hr) && *pFrequency > 0ull) {
        cached_gpu_ts_freq_.store(*pFrequency, std::memory_order_release);
    } else if (qpc_freq_ > 0) {
        *pFrequency = static_cast<UINT64>(qpc_freq_);
        cached_gpu_ts_freq_.store(*pFrequency, std::memory_order_release);
        hr = S_OK;
    }
    RecordQueue("ID3D12CommandQueueX::GetTimestampFrequency", 16, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetClockCalibration(
    UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) {
    if (!pGpuTimestamp || !pCpuTimestamp) {
        RecordQueue("ID3D12CommandQueueX::GetClockCalibration", 17, E_POINTER);
        return E_POINTER;
    }
    HRESULT hr = host_queue_->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
    if (FAILED(hr)) {
        const uint64_t qpc_now = static_cast<uint64_t>(QueryQpcNow());
        uint64_t gpu_freq = cached_gpu_ts_freq_.load(std::memory_order_acquire);
        if (gpu_freq == 0ull) {
            (void)GetTimestampFrequency(&gpu_freq);
        }
        *pCpuTimestamp = qpc_now;
        if (qpc_freq_ > 0 && gpu_freq > 0ull) {
            *pGpuTimestamp = static_cast<uint64_t>(
                (static_cast<double>(qpc_now) / static_cast<double>(qpc_freq_)) *
                static_cast<double>(gpu_freq));
        } else {
            *pGpuTimestamp = qpc_now;
        }
        hr = S_OK;
    }
    RecordQueue("ID3D12CommandQueueX::GetClockCalibration", 17, hr);
    return hr;
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE CommandQueueX::GetDesc(D3D12_COMMAND_QUEUE_DESC* pRet) {
    D3D12_COMMAND_QUEUE_DESC* desc = host_queue_->GetDesc(pRet);
    RecordQueue("ID3D12CommandQueueX::GetDesc", 18, S_OK);
    return desc;
}
#else
D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE CommandQueueX::GetDesc() {
    const D3D12_COMMAND_QUEUE_DESC desc = host_queue_->GetDesc();
    RecordQueue("ID3D12CommandQueueX::GetDesc", 18, S_OK);
    return desc;
}
#endif

// Window subclass management
void CommandQueueX::AttachWindowSubclassLocked(HWND hwnd) {
    if (subclassed_hwnd_ == hwnd) {
        return;
    }
    DetachWindowSubclassLocked();

    if (!hwnd || IsWindow(hwnd) == FALSE) {
        return;
    }

    const bool keep_post_quit =
        window_state_ ? window_state_->post_quit_on_close.load(std::memory_order_acquire) : false;
    window_state_ = std::make_shared<WindowCallbackState>();
    window_state_->post_quit_on_close.store(keep_post_quit, std::memory_order_release);
    RefreshClientMetrics(hwnd, *window_state_);

    auto& reg = GetSubclassRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);

    auto existing = reg.map.find(hwnd);
    WNDPROC cur_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (cur_proc == &SubclassWndProc && existing != reg.map.end()) {
        existing->second.state = window_state_;
        subclassed_hwnd_ = hwnd;
        return;
    }

    SubclassEntry entry{};
    entry.state = window_state_;
    entry.orig_proc = cur_proc;
    reg.map[hwnd] = entry;

    SetLastError(0);
    const LONG_PTR prev = SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassWndProc));
    if (prev == 0 && GetLastError() != 0) {
        reg.map.erase(hwnd);
        return;
    }
    reg.map[hwnd].orig_proc = reinterpret_cast<WNDPROC>(prev);
    subclassed_hwnd_ = hwnd;
}

void CommandQueueX::DetachWindowSubclassLocked() {
    HWND old_hwnd = subclassed_hwnd_;
    subclassed_hwnd_ = nullptr;
    if (!old_hwnd) {
        return;
    }

    WNDPROC orig_proc = nullptr;
    {
        auto& reg = GetSubclassRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        auto it = reg.map.find(old_hwnd);
        if (it != reg.map.end() && it->second.state == window_state_) {
            orig_proc = it->second.orig_proc;
            reg.map.erase(it);
        }
    }

    if (orig_proc && orig_proc != &SubclassWndProc && IsWindow(old_hwnd) != FALSE) {
        if (GetWindowLongPtrW(old_hwnd, GWLP_WNDPROC) ==
            reinterpret_cast<LONG_PTR>(&SubclassWndProc)) {
            SetWindowLongPtrW(old_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig_proc));
        }
    }
}

void CommandQueueX::ReleaseVulkanSwapChainLocked() {
    if (vk_swapchain_handle_ != 0ull && device_x_ && device_x_->GetVulkanBackend() &&
        device_x_->GetVulkanBackend()->IsInitialized()) {
        device_x_->GetVulkanBackend()->DestroyWin32Swapchain(vk_swapchain_handle_);
    }
    vk_swapchain_handle_ = 0ull;
    vk_swapchain_info_ = openscarlett::VulkanSwapchainInfo{};
    vk_swapchain_hwnd_ = nullptr;
    vk_swapchain_width_ = 0u;
    vk_swapchain_height_ = 0u;
    vk_swapchain_headless_ = false;
}

HRESULT CommandQueueX::EnsureVulkanSwapChainLocked(
    HWND hwnd, uint32_t width, uint32_t height, bool prefer_hdr10, bool headless) {
    if (!device_x_ || !device_x_->GetVulkanBackend() || !device_x_->GetVulkanBackend()->IsInitialized()) {
        return E_FAIL;
    }
    if (vk_swapchain_handle_ != 0ull &&
        vk_swapchain_hwnd_ == hwnd &&
        vk_swapchain_width_ == width &&
        vk_swapchain_height_ == height &&
        vk_swapchain_headless_ == headless) {
        return S_OK;
    }
    ReleaseVulkanSwapChainLocked();
    openscarlett::VulkanSwapchainDesc sc_desc{};
    sc_desc.hwnd = headless ? nullptr : static_cast<void*>(hwnd);
    sc_desc.width = std::max<uint32_t>(1u, width);
    sc_desc.height = std::max<uint32_t>(1u, height);
    sc_desc.prefer_hdr10 = prefer_hdr10;
    sc_desc.headless_offscreen = headless;
    openscarlett::VulkanSwapchainInfo info{};
    openscarlett::Status st = device_x_->GetVulkanBackend()->CreateWin32Swapchain(sc_desc, info);
    if (!st.ok()) {
        return E_FAIL;
    }
    vk_swapchain_handle_ = info.handle;
    vk_swapchain_info_ = info;
    vk_swapchain_hwnd_ = hwnd;
    vk_swapchain_width_ = width;
    vk_swapchain_height_ = height;
    vk_swapchain_headless_ = headless;
    return S_OK;
}

void CommandQueueX::ReleaseSwapChainLocked() {
    ReleaseVulkanSwapChainLocked();
    if (present_fence_ && host_queue_) {
        WaitForInternalFenceLocked();
    }
    if (frame_latency_waitable_object_) {
        CloseHandle(frame_latency_waitable_object_);
        frame_latency_waitable_object_ = nullptr;
    }
    if (swapchain4_) {
        swapchain4_->Release();
        swapchain4_ = nullptr;
    }
    if (swapchain_) {
        swapchain_->Release();
        swapchain_ = nullptr;
    }
    swapchain_hwnd_ = nullptr;
    swapchain_width_ = 0;
    swapchain_height_ = 0;
    swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    swapchain_flags_ = 0;
    swapchain_buffer_count_ = 0;
    swapchain_max_latency_ = 0;
    swapchain_occluded_ = false;
    active_color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    cs_support_flags_ = 0u;
    color_space_applied_ = false;
    hdr_metadata_applied_ = false;
    tone_map_fallback_active_ = false;
    last_paced_qpc_ = 0;
}

// PresentX internal helpers
HRESULT CommandQueueX::EnsurePresentHelperObjectsLocked() {
    if (!pacing_timer_) {
        pacing_timer_ = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!pacing_timer_) {
            pacing_timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }

    if (present_alloc_ && present_cmd_ && present_fence_ && present_fence_event_) {
        return S_OK;
    }
    ID3D12Device* dev = device_x_->HostDevice();
    const D3D12_COMMAND_LIST_TYPE list_type = host_queue_->GetDesc().Type;

    if (!present_alloc_) {
        HRESULT hr = dev->CreateCommandAllocator(list_type, IID_PPV_ARGS(&present_alloc_));
        if (FAILED(hr)) return hr;
    }
    if (!present_cmd_) {
        HRESULT hr = dev->CreateCommandList(
            0, list_type, present_alloc_, nullptr, IID_PPV_ARGS(&present_cmd_));
        if (FAILED(hr)) return hr;
        present_cmd_->Close();
    }
    if (!present_fence_) {
        HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence_));
        if (FAILED(hr)) return hr;
    }
    if (!present_fence_event_) {
        present_fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!present_fence_event_) return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT CommandQueueX::EnsureReadbackBufferLocked(uint64_t required_bytes) {
    if (readback_buf_ && readback_buf_capacity_ >= required_bytes) {
        return S_OK;
    }
    if (readback_buf_) {
        readback_buf_->Release();
        readback_buf_ = nullptr;
        readback_buf_capacity_ = 0;
    }

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = required_bytes;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.Format = DXGI_FORMAT_UNKNOWN;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device_x_->HostDevice()->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buf_));
    if (SUCCEEDED(hr)) {
        readback_buf_capacity_ = required_bytes;
    }
    return hr;
}

HRESULT CommandQueueX::EnsureToneMapUploadBufferLocked(uint64_t required_bytes) {
    if (tonemap_upload_buf_ && tonemap_upload_capacity_ >= required_bytes) {
        return S_OK;
    }
    if (tonemap_upload_buf_) {
        tonemap_upload_buf_->Release();
        tonemap_upload_buf_ = nullptr;
        tonemap_upload_capacity_ = 0;
    }

    D3D12_HEAP_PROPERTIES up_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = required_bytes;
    up_desc.Height = 1;
    up_desc.DepthOrArraySize = 1;
    up_desc.MipLevels = 1;
    up_desc.Format = DXGI_FORMAT_UNKNOWN;
    up_desc.SampleDesc.Count = 1;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device_x_->HostDevice()->CreateCommittedResource(
        &up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tonemap_upload_buf_));
    if (SUCCEEDED(hr)) {
        tonemap_upload_capacity_ = required_bytes;
    }
    return hr;
}

void CommandQueueX::ConfigureSwapChainHdrLocked(
    DXGI_FORMAT src_format,
    D3D12XBOX_COLOR_SPACE req_color_space) {
    if (!swapchain_) return;

    const bool src_is_hdr = IsHdrSwapChainMode(src_format, req_color_space);
    const bool force_sdr = (present_config_.ForceSdrToneMapFallback != 0u);
    const DXGI_COLOR_SPACE_TYPE target_cs =
        force_sdr ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                  : ToDxgiColorSpace(swapchain_format_, req_color_space);

    if (!swapchain4_) {
        swapchain_->QueryInterface(IID_PPV_ARGS(&swapchain4_));
    }

    UINT support = 0u;
    color_space_applied_ = false;
    cs_support_flags_ = 0u;
    if (SUCCEEDED(swapchain_->CheckColorSpaceSupport(target_cs, &support))) {
        cs_support_flags_ = support;
        if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0u) {
            if (SUCCEEDED(swapchain_->SetColorSpace1(target_cs))) {
                active_color_space_ = target_cs;
                color_space_applied_ = true;
            }
        }
    }

    // Query output HDR capability via IDXGIOutput6 to detect SDR display tone-map fallback.
    bool display_supports_hdr = false;
    IDXGIOutput* out = nullptr;
    if (SUCCEEDED(swapchain_->GetContainingOutput(&out)) && out) {
        IDXGIOutput6* out6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&out6))) && out6) {
            DXGI_OUTPUT_DESC1 desc1{};
            if (SUCCEEDED(out6->GetDesc1(&desc1))) {
                display_supports_hdr =
                    (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            }
            out6->Release();
        }
        out->Release();
    }

    hdr_metadata_applied_ = false;
    if (swapchain4_ && !force_sdr &&
        (target_cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || hdr_metadata_explicit_)) {
        DXGI_HDR_METADATA_HDR10 dxgi_meta{};
        dxgi_meta.RedPrimary[0] = hdr_metadata_.RedPrimary[0];
        dxgi_meta.RedPrimary[1] = hdr_metadata_.RedPrimary[1];
        dxgi_meta.GreenPrimary[0] = hdr_metadata_.GreenPrimary[0];
        dxgi_meta.GreenPrimary[1] = hdr_metadata_.GreenPrimary[1];
        dxgi_meta.BluePrimary[0] = hdr_metadata_.BluePrimary[0];
        dxgi_meta.BluePrimary[1] = hdr_metadata_.BluePrimary[1];
        dxgi_meta.WhitePoint[0] = hdr_metadata_.WhitePoint[0];
        dxgi_meta.WhitePoint[1] = hdr_metadata_.WhitePoint[1];
        dxgi_meta.MaxMasteringLuminance = hdr_metadata_.MaxMasteringLuminance;
        dxgi_meta.MinMasteringLuminance = hdr_metadata_.MinMasteringLuminance;
        dxgi_meta.MaxContentLightLevel = hdr_metadata_.MaxContentLightLevel;
        dxgi_meta.MaxFrameAverageLightLevel = hdr_metadata_.MaxFrameAverageLightLevel;
        if (SUCCEEDED(swapchain4_->SetHDRMetaData(
                DXGI_HDR_METADATA_TYPE_HDR10, sizeof(dxgi_meta), &dxgi_meta))) {
            hdr_metadata_applied_ = true;
        }
    }

    const bool format_fell_back_to_sdr =
        src_is_hdr && (swapchain_format_ == DXGI_FORMAT_R8G8B8A8_UNORM ||
                       swapchain_format_ == DXGI_FORMAT_B8G8R8A8_UNORM);
    tone_map_fallback_active_ =
        src_is_hdr && (force_sdr || !display_supports_hdr || !color_space_applied_ || format_fell_back_to_sdr);
}

HRESULT CommandQueueX::EnsureSwapChainLocked(
    HWND hwnd,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12XBOX_COLOR_SPACE color_space,
    bool force_resize) {
    const bool force_sdr = (present_config_.ForceSdrToneMapFallback != 0u);
    const DXGI_FORMAT requested_fmt =
        (present_config_.PreferredFormat != DXGI_FORMAT_UNKNOWN)
            ? present_config_.PreferredFormat
            : format;
    const DXGI_FORMAT sc_fmt =
        force_sdr ? DXGI_FORMAT_R8G8B8A8_UNORM : NormalizeSwapChainFormat(requested_fmt);
    const uint32_t req_max_latency =
        device_x_ ? std::clamp(device_x_->MaxFrameLatency(), 1u, 3u) : 2u;
    const uint32_t req_buffer_count = std::clamp(req_max_latency + 1u, 2u, 3u);

    if (swapchain_ && swapchain_hwnd_ == hwnd) {
        if (!force_resize &&
            swapchain_width_ == width &&
            swapchain_height_ == height &&
            swapchain_format_ == sc_fmt) {
            // Apply dynamic SetFrameIntervalX max-frame-latency updates cleanly.
            if ((swapchain_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
                const uint32_t clamped_lat =
                    std::clamp(req_max_latency, 1u, std::max(swapchain_buffer_count_, 1u));
                if (clamped_lat != swapchain_max_latency_) {
                    if (SUCCEEDED(swapchain_->SetMaximumFrameLatency(clamped_lat))) {
                        swapchain_max_latency_ = clamped_lat;
                    }
                }
            }
            const DXGI_COLOR_SPACE_TYPE desired_cs =
                force_sdr ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                          : ToDxgiColorSpace(swapchain_format_, color_space);
            if (desired_cs != active_color_space_ ||
                (IsHdrSwapChainMode(format, color_space) && !color_space_applied_)) {
                ConfigureSwapChainHdrLocked(format, color_space);
            }
            return S_OK;
        }

        WaitForInternalFenceLocked();
        const UINT resize_bufs =
            (swapchain_buffer_count_ >= 2u) ? swapchain_buffer_count_ : req_buffer_count;
        HRESULT resize_hr = swapchain_->ResizeBuffers(
            resize_bufs, width, height, sc_fmt, swapchain_flags_);
        if (SUCCEEDED(resize_hr)) {
            swapchain_width_ = width;
            swapchain_height_ = height;
            swapchain_format_ = sc_fmt;
            swapchain_occluded_ = false;
            ConfigureSwapChainHdrLocked(format, color_space);
            return S_OK;
        }
        // Resize failed — recreate cleanly.
        ReleaseSwapChainLocked();
    } else if (swapchain_) {
        ReleaseSwapChainLocked();
    }

    IDXGIFactory4* factory4 = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory4));
    if (FAILED(hr) || !factory4) {
        return hr;
    }

    bool allow_tearing = false;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory4->QueryInterface(IID_PPV_ARGS(&factory5))) && factory5) {
        BOOL tearing_supported = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &tearing_supported,
                sizeof(tearing_supported)))) {
            allow_tearing = (tearing_supported != FALSE);
        }
        factory5->Release();
    }

    // Build ordered fallback flag list: waitable+tearing -> waitable -> tearing -> 0
    uint32_t flag_candidates[4]{};
    uint32_t candidate_count = 0;
    if (allow_tearing) {
        flag_candidates[candidate_count++] =
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    flag_candidates[candidate_count++] = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allow_tearing) {
        flag_candidates[candidate_count++] = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    flag_candidates[candidate_count++] = 0u;

    DXGI_FORMAT fmt_candidates[2] = {sc_fmt, DXGI_FORMAT_R8G8B8A8_UNORM};
    const uint32_t fmt_candidate_count = (sc_fmt != DXGI_FORMAT_R8G8B8A8_UNORM) ? 2u : 1u;

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = req_buffer_count;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc1 = nullptr;
    uint32_t chosen_flags = 0u;
    DXGI_FORMAT chosen_fmt = sc_fmt;
    hr = E_FAIL;
    for (uint32_t fi = 0; fi < fmt_candidate_count && !sc1; ++fi) {
        sc_desc.Format = fmt_candidates[fi];
        for (uint32_t i = 0; i < candidate_count; ++i) {
            sc_desc.Flags = flag_candidates[i];
            hr = factory4->CreateSwapChainForHwnd(host_queue_, hwnd, &sc_desc, nullptr, nullptr, &sc1);
            if (SUCCEEDED(hr) && sc1) {
                chosen_flags = flag_candidates[i];
                chosen_fmt = fmt_candidates[fi];
                break;
            }
        }
    }

    // Disable DXGI's built-in exclusive fullscreen Alt+Enter interception.
    factory4->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory4->Release();

    if (FAILED(hr) || !sc1) {
        return hr;
    }

    hr = sc1->QueryInterface(IID_PPV_ARGS(&swapchain_));
    if (SUCCEEDED(hr) && swapchain_) {
        sc1->QueryInterface(IID_PPV_ARGS(&swapchain4_));
    }
    sc1->Release();
    if (SUCCEEDED(hr) && swapchain_) {
        swapchain_hwnd_ = hwnd;
        swapchain_width_ = width;
        swapchain_height_ = height;
        swapchain_format_ = chosen_fmt;
        swapchain_flags_ = chosen_flags;
        swapchain_buffer_count_ = req_buffer_count;
        swapchain_occluded_ = false;

        if ((chosen_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
            const uint32_t clamped_lat = std::clamp(req_max_latency, 1u, req_buffer_count);
            if (SUCCEEDED(swapchain_->SetMaximumFrameLatency(clamped_lat))) {
                swapchain_max_latency_ = clamped_lat;
            } else {
                swapchain_max_latency_ = 1u;
            }
            frame_latency_waitable_object_ = swapchain_->GetFrameLatencyWaitableObject();
        } else {
            swapchain_max_latency_ = req_max_latency;
        }
        ConfigureSwapChainHdrLocked(format, color_space);
    }
    return hr;
}

void CommandQueueX::PaceFrameLocked(uint32_t frame_interval_us) {
    if (qpc_freq_ <= 0 || frame_interval_us == 0u) {
        return;
    }

    const int64_t now_qpc = QueryQpcNow();
    if (last_paced_qpc_ == 0) {
        last_paced_qpc_ = now_qpc;
        last_present_qpc_ = now_qpc;
        last_frame_ms_ = 0.0;
        return;
    }

    const int64_t interval_ticks =
        (static_cast<int64_t>(frame_interval_us) * qpc_freq_) / 1000000LL;
    const int64_t target_qpc = last_paced_qpc_ + interval_ticks;

    int64_t cur_qpc = now_qpc;
    if (cur_qpc < target_qpc) {
        const int64_t rem_us = ((target_qpc - cur_qpc) * 1000000LL) / qpc_freq_;
        if (rem_us > 2000LL) {
            const int64_t sleep_us = rem_us - 1500LL;
            if (pacing_timer_) {
                LARGE_INTEGER due{};
                due.QuadPart = -(sleep_us * 10LL);
                if (SetWaitableTimer(pacing_timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
                    WaitForSingleObject(pacing_timer_, 200u);
                }
            } else {
                const DWORD sleep_ms = static_cast<DWORD>(sleep_us / 1000LL);
                if (sleep_ms > 0u) {
                    Sleep(sleep_ms);
                }
            }
        }
        for (;;) {
            cur_qpc = QueryQpcNow();
            if (cur_qpc >= target_qpc) {
                break;
            }
            YieldProcessor();
        }
    }

    last_frame_ms_ =
        static_cast<double>(cur_qpc - last_present_qpc_) * 1000.0 / static_cast<double>(qpc_freq_);
    last_present_qpc_ = cur_qpc;

    // Preserve phase when on time; resync cleanly if a frame overran by more than one interval.
    if (cur_qpc <= target_qpc + interval_ticks) {
        last_paced_qpc_ = target_qpc;
    } else {
        last_paced_qpc_ = cur_qpc;
    }
}

HRESULT CommandQueueX::WaitForInternalFenceWithTimingLocked(
    int64_t* out_signal_qpc,
    double* out_wait_us,
    uint64_t* out_completed_value) {
    if (fence_dispatcher_ && fence_dispatcher_->IsDeviceRemoved()) {
        HRESULT r = fence_dispatcher_->RemovedReason();
        return FAILED(r) ? r : DXGI_ERROR_DEVICE_REMOVED;
    }
    if (!present_fence_ || !present_fence_event_) {
        if (out_signal_qpc) *out_signal_qpc = QueryQpcNow();
        if (out_wait_us) *out_wait_us = 0.0;
        if (out_completed_value) *out_completed_value = present_fence_val_;
        return S_OK;
    }
    const uint64_t target = ++present_fence_val_;
    const int64_t sig_qpc = QueryQpcNow();
    HRESULT hr = host_queue_->Signal(present_fence_, target);
    if (FAILED(hr)) return hr;
    const uint64_t cur_completed = present_fence_->GetCompletedValue();
    if (cur_completed == UINT64_MAX) {
        HRESULT r = device_x_ ? device_x_->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
        if (SUCCEEDED(r)) r = DXGI_ERROR_DEVICE_REMOVED;
        if (fence_dispatcher_) fence_dispatcher_->NotifyDeviceRemoved(r);
        return r;
    }
    if (cur_completed < target) {
        hr = present_fence_->SetEventOnCompletion(target, present_fence_event_);
        if (FAILED(hr)) return hr;
        HANDLE rm_ev = fence_dispatcher_ ? fence_dispatcher_->DeviceRemovedEventHandle() : nullptr;
        DWORD wr = WAIT_FAILED;
        if (rm_ev) {
            HANDLE waits[2] = {rm_ev, present_fence_event_};
            wr = WaitForMultipleObjects(2u, waits, FALSE, 15000u);
            if (wr == WAIT_OBJECT_0) {
                HRESULT r = fence_dispatcher_->RemovedReason();
                return FAILED(r) ? r : DXGI_ERROR_DEVICE_REMOVED;
            }
            if (wr != (WAIT_OBJECT_0 + 1u)) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            wr = WaitForSingleObject(present_fence_event_, 15000u);
            if (wr != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    const int64_t done_qpc = QueryQpcNow();
    if (out_signal_qpc) *out_signal_qpc = sig_qpc;
    if (out_wait_us) {
        *out_wait_us = (qpc_freq_ > 0)
                           ? (static_cast<double>(done_qpc - sig_qpc) * 1000000.0 / static_cast<double>(qpc_freq_))
                           : 0.0;
    }
    const uint64_t completed_now = present_fence_->GetCompletedValue();
    if (device_x_) {
        (void)device_x_->GetTitleVaMirror().CompleteReadbacksOnFence(completed_now);
    }
    if (out_completed_value) {
        *out_completed_value = completed_now;
    }
    return S_OK;
}

HRESULT CommandQueueX::WaitForInternalFenceLocked() {
    return WaitForInternalFenceWithTimingLocked(nullptr, nullptr, nullptr);
}

void CommandQueueX::ReleasePresentObjectsLocked() {
    DetachWindowSubclassLocked();
    ReleaseSwapChainLocked();
    if (tonemap_upload_buf_) { tonemap_upload_buf_->Release(); tonemap_upload_buf_ = nullptr; }
    if (readback_buf_) { readback_buf_->Release(); readback_buf_ = nullptr; }
    if (present_cmd_) { present_cmd_->Release(); present_cmd_ = nullptr; }
    if (present_alloc_) { present_alloc_->Release(); present_alloc_ = nullptr; }
    if (present_fence_) { present_fence_->Release(); present_fence_ = nullptr; }
    if (present_fence_event_) { CloseHandle(present_fence_event_); present_fence_event_ = nullptr; }
    if (pacing_timer_) { CloseHandle(pacing_timer_); pacing_timer_ = nullptr; }
    readback_buf_capacity_ = 0;
    tonemap_upload_capacity_ = 0;
}

// ID3D12CommandQueueX (slots 19..21; real GDKX 3-arg PresentX)
HRESULT STDMETHODCALLTYPE CommandQueueX::PresentX(
    UINT PlaneCount,
    const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
    const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
    if (device_x_ && device_x_->IsDeviceRemoved()) {
        const HRESULT rem_hr = device_x_->GetDeviceRemovedReason();
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, rem_hr);
        return rem_hr;
    }
    const int64_t total_start_qpc = QueryQpcNow();

    // Xbox GDK titles (e.g. Xbox-GDK-Samples DeviceResources::~DeviceResources & resize)
    // legitimately call PresentX(0, nullptr, nullptr) to present a blank screen before teardown.
    if (PlaneCount == 0) {
        std::lock_guard<std::mutex> lock(present_mutex_);
        EnsurePresentHelperObjectsLocked();
        int64_t sig_qpc = 0;
        double wait_us = 0.0;
        uint64_t completed_val = 0ull;
        WaitForInternalFenceWithTimingLocked(&sig_qpc, &wait_us, &completed_val);
        const int64_t pres_qpc = QueryQpcNow();
        ++present_count_;
        last_timing_stats_.fence_signal_qpc = sig_qpc;
        last_timing_stats_.fence_wait_us = wait_us;
        last_timing_stats_.fence_completed_value = completed_val;
        last_timing_stats_.present_submit_qpc = pres_qpc;
        last_timing_stats_.present_complete_qpc = pres_qpc;
        last_timing_stats_.dxgi_present_us = 0.0;
        last_timing_stats_.total_presentx_us =
            (qpc_freq_ > 0)
                ? (static_cast<double>(pres_qpc - total_start_qpc) * 1000000.0 / static_cast<double>(qpc_freq_))
                : 0.0;
        last_timing_stats_.present_count = present_count_;
        {
            uint64_t sig_vlines = 0ull;
            uint64_t sig_frames = 0ull;
            SignalRegisteredDisplayNotificationsOnPresent(&sig_vlines, &sig_frames);
            if (device_x_ && (sig_vlines > 0ull || sig_frames > 0ull)) {
                device_x_->RecordDisplayNotificationsSignaled(sig_vlines, sig_frames);
            }
        }
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, S_OK);
        return S_OK;
    }
    if (!pPlaneParameters ||
        pPlaneParameters[0].ResourceCount == 0 ||
        !pPlaneParameters[0].ppResources ||
        !pPlaneParameters[0].ppResources[0]) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, E_INVALIDARG);
        return E_INVALIDARG;
    }

    ID3D12Resource* src_tex = UnwrapResource(pPlaneParameters[0].ppResources[0]);
    const D3D12_RESOURCE_DESC src_desc = src_tex->GetDesc();
    if (src_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        src_desc.Width == 0 || src_desc.Height == 0) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const uint32_t width = static_cast<uint32_t>(src_desc.Width);
    const uint32_t height = src_desc.Height;
    const D3D12_RESOURCE_STATES caller_state = pPlaneParameters[0].ResourceState;

    // Ensure window subclass is active and pump pending Win32 messages WITHOUT holding present_mutex_.
    const bool force_headless = EnvFlagTrueA("OPENSCARLETT_PRESENTX_HEADLESS");
    HWND pump_hwnd = nullptr;
    std::shared_ptr<WindowCallbackState> pump_state;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        pump_hwnd = ResolveTargetOutputWindowLocked(pPresentParameters, force_headless);
        if (pump_hwnd && IsWindow(pump_hwnd) != FALSE) {
            AttachWindowSubclassLocked(pump_hwnd);
            pump_state = window_state_;
        }
    }

    if (pump_hwnd && IsWindow(pump_hwnd) != FALSE &&
        GetWindowThreadProcessId(pump_hwnd, nullptr) == GetCurrentThreadId()) {
        MSG msg{};
        constexpr uint32_t kMaxMessagesPerPresent = 64u;
        for (uint32_t i = 0; i < kMaxMessagesPerPresent &&
                             PeekMessageW(&msg, pump_hwnd, 0, 0, PM_REMOVE) != FALSE;
             ++i) {
            if (msg.message == WM_QUIT) {
                if (pump_state) {
                    pump_state->close_requested.store(true, std::memory_order_release);
                }
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    std::lock_guard<std::mutex> lock(present_mutex_);

    HRESULT hr = EnsurePresentHelperObjectsLocked();
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }

    D3D12XBOX_COLOR_SPACE effective_cs = DxgiToXboxColorSpace(pPlaneParameters[0].ColorSpace);
    if (effective_cs == D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        if (present_config_.ColorSpace != D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
            effective_cs = present_config_.ColorSpace;
        } else if (src_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                   src_desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
            effective_cs = D3D12XBOX_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }
    }
    last_presented_src_format_ = src_desc.Format;
    last_presented_color_space_ = effective_cs;

    const std::string capture_png_path = ReadEnvStringA("OPENSCARLETT_PRESENTX_CAPTURE_PNG");
    const uint64_t exit_after_frames = ReadEnvUint64A("OPENSCARLETT_PRESENTX_EXIT_AFTER_FRAMES");
    const uint64_t this_non_blank_frame = non_blank_present_count_ + 1ull;
    const bool want_capture_this_frame =
        !capture_png_path.empty() &&
        ((exit_after_frames > 0ull && this_non_blank_frame == exit_after_frames) ||
         (exit_after_frames == 0ull && (this_non_blank_frame == 1ull || this_non_blank_frame == 10ull)));

    HWND target_hwnd = ResolveTargetOutputWindowLocked(pPresentParameters, force_headless);
    const bool use_swapchain = (target_hwnd != nullptr && IsWindow(target_hwnd) != FALSE);

    bool window_minimized = false;
    uint32_t target_bb_width = width;
    uint32_t target_bb_height = height;
    if (use_swapchain) {
        AttachWindowSubclassLocked(target_hwnd);
        RECT cr{};
        const bool got_cr = (GetClientRect(target_hwnd, &cr) != FALSE);
        const uint32_t cw =
            (got_cr && cr.right > cr.left) ? static_cast<uint32_t>(cr.right - cr.left) : 0u;
        const uint32_t ch =
            (got_cr && cr.bottom > cr.top) ? static_cast<uint32_t>(cr.bottom - cr.top) : 0u;
        const bool iconic = (IsIconic(target_hwnd) != FALSE);
        const bool cb_min =
            window_state_ ? window_state_->is_minimized.load(std::memory_order_acquire) : false;
        window_minimized = iconic || cb_min || (cw == 0u || ch == 0u);
        if (!window_minimized) {
            target_bb_width = cw;
            target_bb_height = ch;
        }
    }

    const bool vk_routing_active =
        device_x_ && device_x_->IsVulkanRoutingActive() &&
        device_x_->GetVulkanBackend() && device_x_->GetVulkanBackend()->IsInitialized();
    openscarlett::FrameBufferRgba8 vk_direct_rt_fb{};
    const bool has_vk_direct_rt_fb =
        vk_routing_active && device_x_->TryGetVulkanRenderTargetFramebuffer(src_tex, vk_direct_rt_fb);

    bool do_swapchain_present = !vk_routing_active && use_swapchain && !window_minimized;
    if (do_swapchain_present) {
        const bool resize_pending =
            window_state_
                ? window_state_->resize_pending.exchange(false, std::memory_order_acq_rel)
                : false;
        hr = EnsureSwapChainLocked(
            target_hwnd, target_bb_width, target_bb_height,
            src_desc.Format, effective_cs, resize_pending);
        if (FAILED(hr)) {
            if ((!use_swapchain || readback_capture_enabled_) ||
                hr == DXGI_ERROR_UNSUPPORTED ||
                hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ||
                hr == E_ACCESSDENIED) {
                // Headless Session-0 CI environments without DWM may reject CreateSwapChainForHwnd;
                // degrade cleanly to headless present instead of failing the frame.
                do_swapchain_present = false;
                hr = S_OK;
            } else {
                RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
                return hr;
            }
        } else if (frame_latency_waitable_object_) {
            const DWORD wait_ms = swapchain_occluded_ ? 50u : 1000u;
            const DWORD wr = WaitForSingleObjectEx(frame_latency_waitable_object_, wait_ms, TRUE);
            if (wr == WAIT_OBJECT_0) {
                ++latency_waits_;
            }
        }
    } else if (IsHdrSwapChainMode(src_desc.Format, effective_cs)) {
        // Offscreen HDR readback automatically uses SDR tone-map conversion for RGBA8 output.
        tone_map_fallback_active_ = true;
    }

    const bool direct_bb_copy_compatible =
        do_swapchain_present && swapchain_ &&
        (NormalizeSwapChainFormat(src_desc.Format) == swapchain_format_);
    const bool need_tonemap_bb_upload =
        do_swapchain_present && swapchain_ && !direct_bb_copy_compatible &&
        (swapchain_format_ == DXGI_FORMAT_R8G8B8A8_UNORM ||
         swapchain_format_ == DXGI_FORMAT_B8G8R8A8_UNORM);
    const bool skip_intermediate_readback =
        (exit_after_frames > 0ull && !want_capture_this_frame && !need_tonemap_bb_upload && !vk_routing_active);
    const bool need_readback =
        !has_vk_direct_rt_fb && !skip_intermediate_readback &&
        (vk_routing_active || (!use_swapchain) || readback_capture_enabled_ || need_tonemap_bb_upload || want_capture_this_frame);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size_bytes = 0;
    UINT64 total_bytes = 0;
    if (need_readback) {
        device_x_->HostDevice()->GetCopyableFootprints(
            &src_desc, 0, 1, 0, &footprint, &num_rows, &row_size_bytes, &total_bytes);
        hr = EnsureReadbackBufferLocked(total_bytes);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
    }

    if (direct_bb_copy_compatible || need_readback) {
        hr = present_alloc_->Reset();
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
        hr = present_cmd_->Reset(present_alloc_, nullptr);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }

        // Transition plane 0 to COPY_SOURCE if needed.
        if (caller_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = src_tex;
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = caller_state;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            present_cmd_->ResourceBarrier(1, &to_copy);
        }

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = src_tex;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        ID3D12Resource* backbuffer = nullptr;
        if (direct_bb_copy_compatible && swapchain_) {
            const UINT bb_idx = swapchain_->GetCurrentBackBufferIndex();
            hr = swapchain_->GetBuffer(bb_idx, IID_PPV_ARGS(&backbuffer));
            if (SUCCEEDED(hr) && backbuffer) {
                const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
                const uint32_t copy_w =
                    std::min<uint32_t>(width, static_cast<uint32_t>(bb_desc.Width));
                const uint32_t copy_h = std::min<uint32_t>(height, bb_desc.Height);

                D3D12_RESOURCE_BARRIER bb_to_dst{};
                bb_to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb_to_dst.Transition.pResource = backbuffer;
                bb_to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb_to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bb_to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                present_cmd_->ResourceBarrier(1, &bb_to_dst);

                if (copy_w > 0u && copy_h > 0u) {
                    D3D12_TEXTURE_COPY_LOCATION bb_dst{};
                    bb_dst.pResource = backbuffer;
                    bb_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    bb_dst.SubresourceIndex = 0;

                    D3D12_BOX src_box{};
                    src_box.left = 0u;
                    src_box.top = 0u;
                    src_box.front = 0u;
                    src_box.right = copy_w;
                    src_box.bottom = copy_h;
                    src_box.back = 1u;

                    present_cmd_->CopyTextureRegion(&bb_dst, 0, 0, 0, &src_loc, &src_box);
                }

                D3D12_RESOURCE_BARRIER bb_to_present{};
                bb_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb_to_present.Transition.pResource = backbuffer;
                bb_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                present_cmd_->ResourceBarrier(1, &bb_to_present);
            }
        }

        if (need_readback && readback_buf_) {
            D3D12_TEXTURE_COPY_LOCATION rb_dst{};
            rb_dst.pResource = readback_buf_;
            rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rb_dst.PlacedFootprint = footprint;
            present_cmd_->CopyTextureRegion(&rb_dst, 0, 0, 0, &src_loc, nullptr);
        }

        // Restore plane 0 back to caller_state.
        if (caller_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER back_barrier{};
            back_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            back_barrier.Transition.pResource = src_tex;
            back_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            back_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            back_barrier.Transition.StateAfter = caller_state;
            present_cmd_->ResourceBarrier(1, &back_barrier);
        }

        hr = present_cmd_->Close();
        if (backbuffer) {
            backbuffer->Release();
            backbuffer = nullptr;
        }
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }

        ID3D12CommandList* cmd_lists[] = {present_cmd_};
        host_queue_->ExecuteCommandLists(1, cmd_lists);
    }

    auto process_readback = [&]() -> HRESULT {
        if (!need_readback || !readback_buf_) return S_OK;
        void* mapped = nullptr;
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
        HRESULT mhr = readback_buf_->Map(0, &read_range, &mapped);
        if (FAILED(mhr) || !mapped) {
            return FAILED(mhr) ? mhr : E_FAIL;
        }

        const auto* rb_bytes = static_cast<const uint8_t*>(mapped);
        const size_t packed_row_bytes = static_cast<size_t>(row_size_bytes);
        const size_t total_packed = packed_row_bytes * height;
        last_present_raw_pixels_.resize(total_packed);
        last_present_raw_format_ = src_desc.Format;

        const size_t rgba8_row_bytes = static_cast<size_t>(width) * 4u;
        last_present_pixels_.resize(rgba8_row_bytes * height);

        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src_row =
                rb_bytes + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
            std::memcpy(
                last_present_raw_pixels_.data() + static_cast<size_t>(y) * packed_row_bytes,
                src_row,
                packed_row_bytes);
            uint8_t* dst_rgba8_row =
                last_present_pixels_.data() + static_cast<size_t>(y) * rgba8_row_bytes;
            ConvertPackedRowToSdrRgba8(
                src_desc.Format,
                effective_cs,
                present_config_.ToneMapMode,
                present_config_.PaperWhiteNits,
                src_row,
                dst_rgba8_row,
                width);
        }

        D3D12_RANGE write_none{0, 0};
        readback_buf_->Unmap(0, &write_none);

        last_present_hash_ = openscarlett::ComputeFnv1a64(
            last_present_pixels_.data(), last_present_pixels_.size());
        return S_OK;
    };

    bool readback_processed = false;
    if (need_tonemap_bb_upload && swapchain_) {
        // Wait for HDR source readback, tone-map to SDR RGBA8, and upload to SDR backbuffer.
        hr = WaitForInternalFenceLocked();
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
        hr = process_readback();
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
        readback_processed = true;

        ID3D12Resource* backbuffer = nullptr;
        const UINT bb_idx = swapchain_->GetCurrentBackBufferIndex();
        hr = swapchain_->GetBuffer(bb_idx, IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr) && backbuffer) {
            const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
            const uint32_t copy_w = std::min<uint32_t>(width, static_cast<uint32_t>(bb_desc.Width));
            const uint32_t copy_h = std::min<uint32_t>(height, bb_desc.Height);

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT bb_fp{};
            UINT bb_rows = 0u;
            UINT64 bb_row_size = 0ull;
            UINT64 bb_total = 0ull;
            device_x_->HostDevice()->GetCopyableFootprints(
                &bb_desc, 0u, 1u, 0ull, &bb_fp, &bb_rows, &bb_row_size, &bb_total);

            hr = EnsureToneMapUploadBufferLocked(bb_total);
            if (SUCCEEDED(hr) && tonemap_upload_buf_) {
                void* up_ptr = nullptr;
                D3D12_RANGE rnone{0u, 0u};
                if (SUCCEEDED(tonemap_upload_buf_->Map(0u, &rnone, &up_ptr)) && up_ptr) {
                    auto* up_bytes = static_cast<uint8_t*>(up_ptr);
                    std::memset(up_bytes, 0, static_cast<size_t>(bb_total));
                    const bool bb_is_bgra = (bb_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
                    for (uint32_t y = 0; y < copy_h; ++y) {
                        const uint8_t* src_r =
                            last_present_pixels_.data() + static_cast<size_t>(y) * width * 4u;
                        uint8_t* dst_r = up_bytes + static_cast<size_t>(y) * bb_fp.Footprint.RowPitch;
                        if (bb_is_bgra) {
                            for (uint32_t x = 0; x < copy_w; ++x) {
                                dst_r[x * 4u + 0u] = src_r[x * 4u + 2u];
                                dst_r[x * 4u + 1u] = src_r[x * 4u + 1u];
                                dst_r[x * 4u + 2u] = src_r[x * 4u + 0u];
                                dst_r[x * 4u + 3u] = src_r[x * 4u + 3u];
                            }
                        } else {
                            std::memcpy(dst_r, src_r, static_cast<size_t>(copy_w) * 4u);
                        }
                    }
                    D3D12_RANGE wwritten{0u, static_cast<SIZE_T>(bb_total)};
                    tonemap_upload_buf_->Unmap(0u, &wwritten);

                    if (SUCCEEDED(present_alloc_->Reset()) &&
                        SUCCEEDED(present_cmd_->Reset(present_alloc_, nullptr))) {
                        D3D12_RESOURCE_BARRIER bb_to_dst{};
                        bb_to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        bb_to_dst.Transition.pResource = backbuffer;
                        bb_to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        bb_to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                        bb_to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                        present_cmd_->ResourceBarrier(1u, &bb_to_dst);

                        D3D12_TEXTURE_COPY_LOCATION bb_dst{};
                        bb_dst.pResource = backbuffer;
                        bb_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        bb_dst.SubresourceIndex = 0u;

                        D3D12_TEXTURE_COPY_LOCATION up_src{};
                        up_src.pResource = tonemap_upload_buf_;
                        up_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        up_src.PlacedFootprint = bb_fp;

                        present_cmd_->CopyTextureRegion(&bb_dst, 0u, 0u, 0u, &up_src, nullptr);

                        D3D12_RESOURCE_BARRIER bb_to_present{};
                        bb_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        bb_to_present.Transition.pResource = backbuffer;
                        bb_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        bb_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                        bb_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                        present_cmd_->ResourceBarrier(1u, &bb_to_present);

                        if (SUCCEEDED(present_cmd_->Close())) {
                            ID3D12CommandList* up_lists[] = {present_cmd_};
                            host_queue_->ExecuteCommandLists(1u, up_lists);
                        }
                    }
                }
            }
            backbuffer->Release();
        }
    }

    bool fcap_override = false;
    uint32_t fcap_fps = 0u;
    const uint32_t frame_interval_us =
        ResolveEffectiveFrameIntervalUsLocked(fcap_override, fcap_fps);
    last_effective_interval_us_ = frame_interval_us;
    const bool immediate_flag =
        (pPresentParameters && (pPresentParameters->Flags & D3D12XBOX_PRESENT_FLAG_IMMEDIATE) != 0);
    const bool should_pace =
        ((use_swapchain && !window_minimized) || fcap_override) &&
        !immediate_flag && frame_interval_us > 0u;

    if (should_pace) {
        PaceFrameLocked(frame_interval_us);
        ++paced_frame_count_;
    } else {
        const int64_t now_qpc = QueryQpcNow();
        if (last_present_qpc_ != 0 && qpc_freq_ > 0) {
            last_frame_ms_ = static_cast<double>(now_qpc - last_present_qpc_) * 1000.0 /
                             static_cast<double>(qpc_freq_);
        }
        last_present_qpc_ = now_qpc;
        last_paced_qpc_ = 0;
    }

    // Phase 1: GPU Fence Signaling & Synchronization (separated from DXGI/Vulkan presentation).
    int64_t fence_sig_qpc = 0;
    double fence_wait_us = 0.0;
    uint64_t fence_completed_val = 0ull;
    hr = WaitForInternalFenceWithTimingLocked(&fence_sig_qpc, &fence_wait_us, &fence_completed_val);
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }

    if (has_vk_direct_rt_fb) {
        last_present_pixels_ = vk_direct_rt_fb.pixels;
        last_present_raw_pixels_ = vk_direct_rt_fb.pixels;
        last_present_raw_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
        last_present_hash_ = openscarlett::ComputeFnv1a64(
            last_present_pixels_.data(), last_present_pixels_.size());
        readback_processed = true;
    } else if (!readback_processed && need_readback && readback_buf_) {
        hr = process_readback();
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
        readback_processed = true;
    }

    // Phase 2: DXGI or Vulkan Presentation Submission.
    int64_t present_submit_qpc = QueryQpcNow();
    int64_t present_complete_qpc = present_submit_qpc;
    double dxgi_present_us = 0.0;

    if (vk_routing_active) {
        openscarlett::VulkanBackend* vk = device_x_->GetVulkanBackend();
        openscarlett::FrameBufferRgba8 present_fb{};
        present_fb.width = has_vk_direct_rt_fb ? vk_direct_rt_fb.width : width;
        present_fb.height = has_vk_direct_rt_fb ? vk_direct_rt_fb.height : height;
        if (last_present_pixels_.size() == static_cast<size_t>(present_fb.width) * present_fb.height * 4u) {
            present_fb.pixels = last_present_pixels_;
        } else {
            present_fb.pixels.assign(static_cast<size_t>(present_fb.width) * present_fb.height * 4u, 0u);
        }

        const bool headless_vk = !(use_swapchain && !window_minimized);
        hr = EnsureVulkanSwapChainLocked(
            headless_vk ? nullptr : target_hwnd,
            headless_vk ? present_fb.width : target_bb_width,
            headless_vk ? present_fb.height : target_bb_height,
            IsHdrSwapChainMode(src_desc.Format, effective_cs),
            headless_vk);
        if (SUCCEEDED(hr) && vk_swapchain_handle_ != 0ull) {
            openscarlett::VulkanPresentResult vk_pres{};
            present_submit_qpc = QueryQpcNow();
            openscarlett::Status pst = vk->PresentRgba8ToSwapchain(
                vk_swapchain_handle_, present_fb, vk_pres, /*readback_frame=*/true);
            present_complete_qpc = QueryQpcNow();
            if (pst.ok()) {
                if (vk_pres.readback_available &&
                    vk_pres.readback_framebuffer.width == present_fb.width &&
                    vk_pres.readback_framebuffer.height == present_fb.height &&
                    !vk_pres.readback_framebuffer.pixels.empty()) {
                    last_present_pixels_ = std::move(vk_pres.readback_framebuffer.pixels);
                }
                last_present_hash_ = openscarlett::ComputeFnv1a64(
                    last_present_pixels_.data(), last_present_pixels_.size());
                device_x_->RecordVulkanRoutingCounters(
                    0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull,
                    1ull, vk->is_fallback_mode() ? 0ull : 1ull, 0ull, 0ull, 0ull);
            } else {
                hr = S_OK;  // Degrade cleanly if Win32 surface was invalidated mid-frame
            }
        } else {
            hr = S_OK;
        }
        dxgi_present_us =
            (qpc_freq_ > 0)
                ? (static_cast<double>(present_complete_qpc - present_submit_qpc) * 1000000.0 /
                   static_cast<double>(qpc_freq_))
                : 0.0;
    } else if (do_swapchain_present && swapchain_) {
        const UINT present_flags =
            ((swapchain_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0)
                ? DXGI_PRESENT_ALLOW_TEARING
                : 0u;
        present_submit_qpc = QueryQpcNow();
        hr = swapchain_->Present(0u, present_flags);
        if (FAILED(hr) && present_flags != 0u) {
            hr = swapchain_->Present(0u, 0u);
        }
        present_complete_qpc = QueryQpcNow();
        dxgi_present_us =
            (qpc_freq_ > 0)
                ? (static_cast<double>(present_complete_qpc - present_submit_qpc) * 1000000.0 /
                   static_cast<double>(qpc_freq_))
                : 0.0;
        if (hr == DXGI_STATUS_OCCLUDED) {
            swapchain_occluded_ = true;
            hr = S_OK;
        } else if (SUCCEEDED(hr)) {
            swapchain_occluded_ = false;
        }
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
    }

    last_present_width_ = has_vk_direct_rt_fb ? vk_direct_rt_fb.width : width;
    last_present_height_ = has_vk_direct_rt_fb ? vk_direct_rt_fb.height : height;
    ++present_count_;
    ++non_blank_present_count_;
    if (device_x_) {
        device_x_->GetQueryAccumulator().BeginFrame(present_count_);
    }

    if (want_capture_this_frame && !last_present_pixels_.empty() &&
        last_present_width_ > 0u && last_present_height_ > 0u) {
        openscarlett::FrameBufferRgba8 fb{};
        fb.width = last_present_width_;
        fb.height = last_present_height_;
        fb.pixels = last_present_pixels_;
        if (fb.WritePng(capture_png_path).ok()) {
            capture_png_written_ = true;
        }
    }

    if (exit_after_frames > 0ull && non_blank_present_count_ >= exit_after_frames && !auto_exit_posted_) {
        auto_exit_posted_ = true;
        const DWORD cur_tid = GetCurrentThreadId();
        const DWORD win_tid =
            (target_hwnd && IsWindow(target_hwnd) != FALSE)
                ? GetWindowThreadProcessId(target_hwnd, nullptr)
                : cur_tid;
        PostQuitMessage(0);
        if (win_tid != 0u && win_tid != cur_tid) {
            PostThreadMessageW(win_tid, WM_QUIT, 0, 0);
        }
    }

    const int64_t total_end_qpc = QueryQpcNow();
    last_timing_stats_.fence_signal_qpc = fence_sig_qpc;
    last_timing_stats_.fence_wait_us = fence_wait_us;
    last_timing_stats_.fence_completed_value = fence_completed_val;
    last_timing_stats_.present_submit_qpc = present_submit_qpc;
    last_timing_stats_.present_complete_qpc = present_complete_qpc;
    last_timing_stats_.dxgi_present_us = dxgi_present_us;
    last_timing_stats_.total_presentx_us =
        (qpc_freq_ > 0)
            ? (static_cast<double>(total_end_qpc - total_start_qpc) * 1000000.0 /
               static_cast<double>(qpc_freq_))
            : 0.0;
    last_timing_stats_.present_count = present_count_;
    {
        uint64_t sig_vlines = 0ull;
        uint64_t sig_frames = 0ull;
        SignalRegisteredDisplayNotificationsOnPresent(&sig_vlines, &sig_frames);
        if (device_x_ && (sig_vlines > 0ull || sig_frames > 0ull)) {
            device_x_->RecordDisplayNotificationsSignaled(sig_vlines, sig_frames);
        }
    }

    RecordQueue("ID3D12CommandQueueX::PresentX", 19, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SuspendX(uint32_t Flags) {
    (void)Flags;
    suspended_.store(true, std::memory_order_release);
    RecordQueue("ID3D12CommandQueueX::SuspendX", 20, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::ResumeX() {
    suspended_.store(false, std::memory_order_release);
    RecordQueue("ID3D12CommandQueueX::ResumeX", 21, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::KickoffX() {
    if (device_x_ && host_queue_) {
        (void)device_x_->SyncAllUnifiedResourcesBeforeSubmit(host_queue_, queue_type_);
    }
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        if (present_fence_ && host_queue_) {
            const uint64_t target = ++present_fence_val_;
            host_queue_->Signal(present_fence_, target);
            PumpMirrorReadbackCompletionLocked();
        }
    }
    kickoff_count_.fetch_add(1ull, std::memory_order_relaxed);
    RecordQueue("ID3D12CommandQueueX::KickoffX", 22, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Wait32BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint32_t Value,
    uint32_t Mask,
    D3D12XBOX_WAIT_VALUE_OP Operation) {
    if (Address == 0ull) {
        RecordQueue("ID3D12CommandQueueX::Wait32BitValueX", 23, E_INVALIDARG);
        return E_INVALIDARG;
    }
    void* cpu_ptr = nullptr;
    uint64_t rem_bytes = 0ull;
    if (device_x_ && device_x_->GetTitleVaMirror().ResolveTitleVaToCpu(Address, cpu_ptr, rem_bytes) &&
        cpu_ptr && rem_bytes >= sizeof(uint32_t)) {
        // Mirrored Title-VA address: poll CPU backing memory while pumping GPU->CPU writeback completions.
        const auto* ptr = reinterpret_cast<const volatile uint32_t*>(cpu_ptr);
        const int64_t start_qpc = QueryQpcNow();
        const int64_t timeout_ticks = (qpc_freq_ > 0) ? (qpc_freq_ * 2LL) : 0LL;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(present_mutex_);
                PumpMirrorReadbackCompletionLocked();
            }
            const uint32_t cur = *ptr;
            if (EvaluateWaitOp<uint32_t>(cur, Value, Mask, Operation)) {
                RecordQueue("ID3D12CommandQueueX::Wait32BitValueX", 23, S_OK);
                return S_OK;
            }
            if (timeout_ticks > 0 && (QueryQpcNow() - start_qpc) >= timeout_ticks) {
                const HRESULT hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                RecordQueue("ID3D12CommandQueueX::Wait32BitValueX", 23, hr);
                return hr;
            }
            SwitchToThread();
        }
    }
    if (IsReadableCpuVa(Address, sizeof(uint32_t))) {
        const auto* ptr = reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(Address));
        const int64_t start_qpc = QueryQpcNow();
        const int64_t timeout_ticks = (qpc_freq_ > 0) ? (qpc_freq_ * 2LL) : 0LL;  // 2s bounded wait
        for (;;) {
            const uint32_t cur = *ptr;
            if (EvaluateWaitOp<uint32_t>(cur, Value, Mask, Operation)) {
                RecordQueue("ID3D12CommandQueueX::Wait32BitValueX", 23, S_OK);
                return S_OK;
            }
            if (timeout_ticks > 0 && (QueryQpcNow() - start_qpc) >= timeout_ticks) {
                const HRESULT hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                RecordQueue("ID3D12CommandQueueX::Wait32BitValueX", 23, hr);
                return hr;
            }
            SwitchToThread();
        }
    }
    // GPU-only virtual address: synchronize submitted queue work cleanly via internal fence.
    HRESULT hr = S_OK;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        hr = EnsurePresentHelperObjectsLocked();
        if (SUCCEEDED(hr)) {
            hr = WaitForInternalFenceLocked();
        }
    }
    RecordQueue("ID3D12CommandQueueX::Wait32BitValueX", 23, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Wait64BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint64_t Value,
    uint64_t Mask,
    D3D12XBOX_WAIT_VALUE_OP Operation) {
    if (Address == 0ull) {
        RecordQueue("ID3D12CommandQueueX::Wait64BitValueX", 24, E_INVALIDARG);
        return E_INVALIDARG;
    }
    void* cpu_ptr = nullptr;
    uint64_t rem_bytes = 0ull;
    if (device_x_ && device_x_->GetTitleVaMirror().ResolveTitleVaToCpu(Address, cpu_ptr, rem_bytes) &&
        cpu_ptr && rem_bytes >= sizeof(uint64_t)) {
        const auto* ptr = reinterpret_cast<const volatile uint64_t*>(cpu_ptr);
        const int64_t start_qpc = QueryQpcNow();
        const int64_t timeout_ticks = (qpc_freq_ > 0) ? (qpc_freq_ * 2LL) : 0LL;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(present_mutex_);
                PumpMirrorReadbackCompletionLocked();
            }
            const uint64_t cur = *ptr;
            if (EvaluateWaitOp<uint64_t>(cur, Value, Mask, Operation)) {
                RecordQueue("ID3D12CommandQueueX::Wait64BitValueX", 24, S_OK);
                return S_OK;
            }
            if (timeout_ticks > 0 && (QueryQpcNow() - start_qpc) >= timeout_ticks) {
                const HRESULT hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                RecordQueue("ID3D12CommandQueueX::Wait64BitValueX", 24, hr);
                return hr;
            }
            SwitchToThread();
        }
    }
    if (IsReadableCpuVa(Address, sizeof(uint64_t))) {
        const auto* ptr = reinterpret_cast<const volatile uint64_t*>(static_cast<uintptr_t>(Address));
        const int64_t start_qpc = QueryQpcNow();
        const int64_t timeout_ticks = (qpc_freq_ > 0) ? (qpc_freq_ * 2LL) : 0LL;  // 2s bounded wait
        for (;;) {
            const uint64_t cur = *ptr;
            if (EvaluateWaitOp<uint64_t>(cur, Value, Mask, Operation)) {
                RecordQueue("ID3D12CommandQueueX::Wait64BitValueX", 24, S_OK);
                return S_OK;
            }
            if (timeout_ticks > 0 && (QueryQpcNow() - start_qpc) >= timeout_ticks) {
                const HRESULT hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                RecordQueue("ID3D12CommandQueueX::Wait64BitValueX", 24, hr);
                return hr;
            }
            SwitchToThread();
        }
    }
    // GPU-only virtual address: synchronize submitted queue work cleanly via internal fence.
    HRESULT hr = S_OK;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        hr = EnsurePresentHelperObjectsLocked();
        if (SUCCEEDED(hr)) {
            hr = WaitForInternalFenceLocked();
        }
    }
    RecordQueue("ID3D12CommandQueueX::Wait64BitValueX", 24, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Write32BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint32_t Value,
    uint32_t Flags) {
    if (Address == 0ull) {
        RecordQueue("ID3D12CommandQueueX::Write32BitValueX", 25, E_INVALIDARG);
        return E_INVALIDARG;
    }

    uint64_t buf_off = 0ull;
    ID3D12Resource* host_buf =
        device_x_ ? device_x_->ResolveBufferResourceByGpuVa(Address, &buf_off) : nullptr;
    if (host_buf && host_queue_) {
        std::lock_guard<std::mutex> lock(present_mutex_);
        HRESULT hr = EnsurePresentHelperObjectsLocked();
        if (SUCCEEDED(hr)) {
            if (present_fence_->GetCompletedValue() < present_fence_val_) {
                if (SUCCEEDED(present_fence_->SetEventOnCompletion(present_fence_val_, present_fence_event_))) {
                    WaitForSingleObject(present_fence_event_, 15000u);
                }
                PumpMirrorReadbackCompletionLocked();
            }
            ID3D12GraphicsCommandList2* cmd2 = nullptr;
            if (SUCCEEDED(present_cmd_->QueryInterface(
                    OS_D3D12_IID(ID3D12GraphicsCommandList2), reinterpret_cast<void**>(&cmd2))) &&
                cmd2) {
                if (SUCCEEDED(present_alloc_->Reset()) &&
                    SUCCEEDED(present_cmd_->Reset(present_alloc_, nullptr))) {
                    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param{};
                    param.Dest = device_x_->TranslateGuestGpuVa(Address);
                    param.Value = Value;
                    D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
                    if ((Flags & D3D12XBOX_WRITE_VALUE_FLAG_MARKER_IN) != 0u) {
                        mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
                    } else if ((Flags & D3D12XBOX_WRITE_VALUE_FLAG_MARKER_OUT) != 0u) {
                        mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
                    }
                    cmd2->WriteBufferImmediate(1u, &param, &mode);
                    uint64_t mirror_va = 0ull;
                    const uint64_t rb_fence = ++present_fence_val_;
                    if (device_x_->LookupMirroredTitleVaForResource(host_buf, mirror_va) &&
                        mirror_va != 0ull) {
                        openscarlett::TitleVaRegionInfo rinfo{};
                        if (device_x_->GetTitleVaMirror().LookupRegion(mirror_va, rinfo)) {
                            (void)device_x_->GetTitleVaMirror().ScheduleGpuReadback(
                                present_cmd_, mirror_va, rb_fence, 0ull, 0ull, rinfo.current_state);
                        }
                    }
                    if (SUCCEEDED(present_cmd_->Close())) {
                        ID3D12CommandList* lists[] = {present_cmd_};
                        host_queue_->ExecuteCommandLists(1u, lists);
                        host_queue_->Signal(present_fence_, rb_fence);
                    }
                }
                cmd2->Release();
            }
        }
        RecordQueue("ID3D12CommandQueueX::Write32BitValueX", 25, hr);
        return hr;
    }

    if (IsWritableCpuVa(Address, sizeof(uint32_t))) {
        if ((Flags & D3D12XBOX_WRITE_VALUE_FLAG_MARKER_OUT) != 0u) {
            std::lock_guard<std::mutex> lock(present_mutex_);
            if (SUCCEEDED(EnsurePresentHelperObjectsLocked())) {
                WaitForInternalFenceLocked();
            }
        }
        *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(Address)) = Value;
        RecordQueue("ID3D12CommandQueueX::Write32BitValueX", 25, S_OK);
        return S_OK;
    }

    RecordQueue("ID3D12CommandQueueX::Write32BitValueX", 25, S_OK);
    return S_OK;
}

// IOpenScarlettPresentCapture
HRESULT STDMETHODCALLTYPE CommandQueueX::SetOutputWindow(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        explicit_window_detached_ = (hwnd == nullptr);
        if (attached_hwnd_ != hwnd ||
            subclassed_hwnd_ != hwnd ||
            (swapchain_ && swapchain_hwnd_ != hwnd)) {
            if (swapchain_ && swapchain_hwnd_ != hwnd) {
                ReleaseSwapChainLocked();
            }
            attached_hwnd_ = hwnd;
            if (hwnd && IsWindow(hwnd) != FALSE) {
                AttachWindowSubclassLocked(hwnd);
            } else {
                DetachWindowSubclassLocked();
            }
        }
    }
    if (device_x_ && hwnd && IsWindow(hwnd) != FALSE && !device_x_->DefaultWindow()) {
        device_x_->AdoptTitleWindow(hwnd);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetReadbackCaptureEnabled(BOOL enabled) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    readback_capture_enabled_ = (enabled != FALSE);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetLastPresentStats(
    uint64_t* out_fnv1a_hash,
    uint32_t* out_width,
    uint32_t* out_height,
    uint64_t* out_present_count) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (out_fnv1a_hash) *out_fnv1a_hash = last_present_hash_;
    if (out_width) *out_width = last_present_width_;
    if (out_height) *out_height = last_present_height_;
    if (out_present_count) *out_present_count = present_count_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::CopyLastPresentPixels(
    uint8_t* out_rgba8,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (out_copied_bytes) *out_copied_bytes = 0;
    if (last_present_pixels_.empty()) return S_FALSE;
    if (!out_rgba8) return E_POINTER;
    const uint32_t to_copy = static_cast<uint32_t>(
        std::min<size_t>(buffer_bytes, last_present_pixels_.size()));
    std::memcpy(out_rgba8, last_present_pixels_.data(), to_copy);
    if (out_copied_bytes) *out_copied_bytes = to_copy;
    return (to_copy == last_present_pixels_.size()) ? S_OK : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

// IOpenScarlettPresentWindow
HRESULT STDMETHODCALLTYPE CommandQueueX::GetPresentWindowState(
    D3D12XBOX_PRESENT_WINDOW_STATE* out_state) {
    if (!out_state) return E_POINTER;
    std::memset(out_state, 0, sizeof(*out_state));

    std::lock_guard<std::mutex> lock(present_mutex_);
    HWND effective_hwnd = subclassed_hwnd_ ? subclassed_hwnd_ : attached_hwnd_;
    if (!effective_hwnd && !explicit_window_detached_ && device_x_) {
        HWND dev_hwnd = device_x_->DefaultWindow();
        if (dev_hwnd && IsWindow(dev_hwnd) != FALSE) {
            effective_hwnd = dev_hwnd;
        }
    }
    const bool has_win = (effective_hwnd != nullptr && IsWindow(effective_hwnd) != FALSE);

    if (window_state_) {
        out_state->is_fullscreen = window_state_->is_fullscreen.load(std::memory_order_acquire) ? 1u : 0u;
        const bool cb_min = window_state_->is_minimized.load(std::memory_order_acquire);
        const bool iconic = has_win && (IsIconic(effective_hwnd) != FALSE);
        out_state->is_minimized = (cb_min || iconic) ? 1u : 0u;
        out_state->is_active = window_state_->is_active.load(std::memory_order_acquire) ? 1u : 0u;
        out_state->close_requested = window_state_->close_requested.load(std::memory_order_acquire) ? 1u : 0u;
        if (has_win || subclassed_hwnd_ != nullptr) {
            out_state->width = window_state_->client_width.load(std::memory_order_acquire);
            out_state->height = window_state_->client_height.load(std::memory_order_acquire);
        } else {
            out_state->width = swapchain_width_ ? swapchain_width_ : last_present_width_;
            out_state->height = swapchain_height_ ? swapchain_height_ : last_present_height_;
        }
    } else {
        out_state->width = swapchain_width_ ? swapchain_width_ : last_present_width_;
        out_state->height = swapchain_height_ ? swapchain_height_ : last_present_height_;
        out_state->is_active = 1u;
    }

    out_state->swapchain_flags = swapchain_flags_;
    out_state->frame_interval_us = device_x_ ? device_x_->FrameIntervalMicroseconds() : 0u;
    out_state->max_frame_latency = device_x_ ? device_x_->MaxFrameLatency() : 1u;
    out_state->present_count = present_count_;
    out_state->latency_waits = latency_waits_;
    out_state->last_frame_ms = last_frame_ms_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::ToggleBorderlessFullscreen() {
    HWND target_hwnd = nullptr;
    std::shared_ptr<WindowCallbackState> state;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        target_hwnd = subclassed_hwnd_ ? subclassed_hwnd_ : attached_hwnd_;
        if (!target_hwnd || IsWindow(target_hwnd) == FALSE) {
            return E_HANDLE;
        }
        AttachWindowSubclassLocked(target_hwnd);
        state = window_state_;
    }
    if (!state) {
        return E_FAIL;
    }
    return ToggleBorderlessFullscreenForWindow(target_hwnd, *state);
}

HANDLE STDMETHODCALLTYPE CommandQueueX::GetFrameLatencyWaitableObject() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    return frame_latency_waitable_object_;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetWindowCloseBehavior(BOOL post_quit_on_close) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (!window_state_) {
        window_state_ = std::make_shared<WindowCallbackState>();
    }
    window_state_->post_quit_on_close.store(post_quit_on_close != FALSE, std::memory_order_release);
    window_state_->quit_posted.store(false, std::memory_order_release);
    return S_OK;
}

// IOpenScarlettPresentHdr
HRESULT STDMETHODCALLTYPE CommandQueueX::SetPresentConfig(
    const D3D12XBOX_PRESENTX_CONFIG* config) {
    if (!config) return E_POINTER;
    std::lock_guard<std::mutex> lock(present_mutex_);
    present_config_ = *config;
    if (present_config_.PaperWhiteNits <= 0.0f) {
        present_config_.PaperWhiteNits = 200.0f;
    }
    if (present_config_.MaxDisplayNits <= 0.0f) {
        present_config_.MaxDisplayNits = 1000.0f;
    }
    if (config->OutputWindow && config->OutputWindow != attached_hwnd_) {
        if (swapchain_ && swapchain_hwnd_ != config->OutputWindow) {
            ReleaseSwapChainLocked();
        }
        attached_hwnd_ = config->OutputWindow;
        if (IsWindow(config->OutputWindow) != FALSE) {
            AttachWindowSubclassLocked(config->OutputWindow);
        }
    }
    if (swapchain_) {
        const DXGI_FORMAT desired_fmt =
            (present_config_.ForceSdrToneMapFallback != 0u)
                ? DXGI_FORMAT_R8G8B8A8_UNORM
                : ((present_config_.PreferredFormat != DXGI_FORMAT_UNKNOWN)
                       ? NormalizeSwapChainFormat(present_config_.PreferredFormat)
                       : swapchain_format_);
        if (desired_fmt != swapchain_format_) {
            ReleaseSwapChainLocked();
        } else {
            ConfigureSwapChainHdrLocked(
                present_config_.PreferredFormat != DXGI_FORMAT_UNKNOWN
                    ? present_config_.PreferredFormat
                    : swapchain_format_,
                present_config_.ColorSpace);
        }
    }
    RecordQueue("ID3D12CommandQueueX::SetPresentConfig", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetHdrMetaData(
    const D3D12XBOX_HDR10_METADATA* metadata) {
    if (!metadata) return E_POINTER;
    std::lock_guard<std::mutex> lock(present_mutex_);
    hdr_metadata_ = *metadata;
    hdr_metadata_explicit_ = true;
    if (swapchain_) {
        ConfigureSwapChainHdrLocked(
            last_presented_src_format_ != DXGI_FORMAT_UNKNOWN
                ? last_presented_src_format_
                : swapchain_format_,
            last_presented_color_space_);
    }
    RecordQueue("ID3D12CommandQueueX::SetHdrMetaData", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetHdrSwapChainState(
    D3D12XBOX_HDR_SWAPCHAIN_STATE* out_state) {
    if (!out_state) return E_POINTER;
    std::memset(out_state, 0, sizeof(*out_state));
    std::lock_guard<std::mutex> lock(present_mutex_);
    out_state->SwapChainFormat = swapchain_format_;
    out_state->ActiveColorSpace = active_color_space_;
    out_state->HasSwapChain4 = swapchain4_ ? 1u : 0u;
    out_state->ColorSpaceSupportFlags = cs_support_flags_;
    out_state->ColorSpaceApplied = color_space_applied_ ? 1u : 0u;
    out_state->HdrMetaDataApplied = hdr_metadata_applied_ ? 1u : 0u;
    out_state->ToneMapFallbackActive = tone_map_fallback_active_ ? 1u : 0u;
    out_state->ToneMapMode = present_config_.ToneMapMode;
    out_state->LastPresentedSourceFormat = last_presented_src_format_;
    out_state->LastPresentedColorSpace = last_presented_color_space_;
    out_state->ActiveMetaData = hdr_metadata_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::CopyLastPresentRawPixels(
    uint8_t* out_raw,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes,
    DXGI_FORMAT* out_format) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (out_copied_bytes) *out_copied_bytes = 0u;
    if (out_format) *out_format = last_present_raw_format_;
    if (last_present_raw_pixels_.empty()) return S_FALSE;
    if (!out_raw) return E_POINTER;
    const uint32_t to_copy = static_cast<uint32_t>(
        std::min<size_t>(buffer_bytes, last_present_raw_pixels_.size()));
    std::memcpy(out_raw, last_present_raw_pixels_.data(), to_copy);
    if (out_copied_bytes) *out_copied_bytes = to_copy;
    return (to_copy == last_present_raw_pixels_.size())
               ? S_OK
               : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

// IOpenScarlettPresentTiming
HRESULT STDMETHODCALLTYPE CommandQueueX::GetPresentTimingStats(
    D3D12XBOX_PRESENT_TIMING_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::lock_guard<std::mutex> lock(present_mutex_);
    *out_stats = last_timing_stats_;
    return S_OK;
}

uint32_t CommandQueueX::ResolveEffectiveFrameIntervalUsLocked(
    bool& out_override_active,
    uint32_t& out_fcap_fps) const noexcept {
    out_override_active = false;
    out_fcap_fps = 0u;
    if (fcap_override_active_) {
        out_override_active = true;
        out_fcap_fps = fcap_override_fps_;
        return (fcap_override_fps_ > 0u) ? (1000000u / fcap_override_fps_) : 0u;
    }
    uint32_t env_fps = 0u;
    if (TryParseEnvFcapFps(env_fps)) {
        out_override_active = true;
        out_fcap_fps = env_fps;
        return (env_fps > 0u) ? (1000000u / env_fps) : 0u;
    }
    uint32_t cmd_fps = 0u;
    if (TryParseCmdLineFcapFps(cmd_fps)) {
        out_override_active = true;
        out_fcap_fps = cmd_fps;
        return (cmd_fps > 0u) ? (1000000u / cmd_fps) : 0u;
    }
    return device_x_ ? device_x_->FrameIntervalMicroseconds() : 0u;
}

void CommandQueueX::PumpMirrorReadbackCompletionLocked() noexcept {
    if (!device_x_ || !present_fence_) return;
    const uint64_t completed = present_fence_->GetCompletedValue();
    (void)device_x_->GetTitleVaMirror().CompleteReadbacksOnFence(completed);
}

// IOpenScarlettPresentPacing
HRESULT STDMETHODCALLTYPE CommandQueueX::SetFrameCapFps(
    uint32_t fps_or_zero_for_uncapped,
    BOOL override_active) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    fcap_override_active_ = (override_active != FALSE);
    fcap_override_fps_ = fps_or_zero_for_uncapped;
    last_paced_qpc_ = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetPresentPacingState(
    D3D12XBOX_PRESENT_PACING_STATE* out_state) {
    if (!out_state) return E_POINTER;
    std::memset(out_state, 0, sizeof(*out_state));
    std::lock_guard<std::mutex> lock(present_mutex_);
    bool ov_active = false;
    uint32_t ov_fps = 0u;
    const uint32_t eff_us = ResolveEffectiveFrameIntervalUsLocked(ov_active, ov_fps);
    out_state->set_frame_interval_us = device_x_ ? device_x_->FrameIntervalMicroseconds() : 0u;
    out_state->fcap_override_fps = ov_fps;
    out_state->fcap_override_active = ov_active ? 1u : 0u;
    out_state->effective_frame_interval_us = eff_us;
    out_state->paced_frame_count = paced_frame_count_;
    if (window_state_) {
        out_state->alt_enter_toggle_count =
            window_state_->alt_enter_toggle_count.load(std::memory_order_acquire);
        out_state->is_fullscreen =
            window_state_->is_fullscreen.load(std::memory_order_acquire) ? 1u : 0u;
    }
    out_state->last_frame_ms = last_frame_ms_;
    return S_OK;
}

// IOpenScarlettFenceWaitDispatcher
HRESULT STDMETHODCALLTYPE CommandQueueX::RegisterFenceCallback(
    ID3D12Fence* fence,
    UINT64 target_value,
    FenceCompletionCallbackFn callback,
    void* user_ctx,
    uint64_t* out_registration_id) {
    if (!fence_dispatcher_) return E_FAIL;
    return fence_dispatcher_->RegisterCallback(
        fence, target_value, callback, user_ctx, out_registration_id);
}

HRESULT STDMETHODCALLTYPE CommandQueueX::WaitForFenceValue(
    ID3D12Fence* fence,
    UINT64 target_value,
    UINT timeout_ms) {
    if (!fence_dispatcher_) return E_FAIL;
    return fence_dispatcher_->WaitForFenceValue(fence, target_value, timeout_ms);
}

HRESULT STDMETHODCALLTYPE CommandQueueX::PumpFenceCallbacks(
    uint32_t* out_dispatched_count) {
    if (!fence_dispatcher_) return E_FAIL;
    const uint32_t count = fence_dispatcher_->PumpReadyCallbacks();
    if (out_dispatched_count) *out_dispatched_count = count;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::NotifyDeviceRemoved(HRESULT removed_reason) {
    if (!fence_dispatcher_) return E_FAIL;
    fence_dispatcher_->NotifyDeviceRemoved(removed_reason);
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
