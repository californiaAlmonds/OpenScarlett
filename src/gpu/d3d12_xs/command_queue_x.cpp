#include "command_queue_x.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <shared_mutex>
#include <unordered_map>

#include "command_list_x.h"
#include "device_x.h"
#include "proxies_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rdna2_lifter.h"

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
    std::atomic<bool> raw_mouse_registered{false};
    std::atomic<bool> raw_mouse_packets_seen{false};
    std::atomic<bool> gi_target_hwnd_sent{false};
    std::atomic<bool> cursor_clipped{false};

    std::mutex fullscreen_mutex;
    LONG_PTR saved_style = 0;
    LONG_PTR saved_ex_style = 0;
    RECT saved_rect{};
    WINDOWPLACEMENT saved_placement{};
    bool has_saved_window_state = false;
};

namespace {

constexpr const char* kModule = "d3d12_xs.dll";

constexpr uint32_t kGiWinEventTargetHwnd = 1u;
constexpr uint32_t kGiWinEventFocus = 2u;
constexpr uint32_t kGiWinEventDeviceChange = 3u;
constexpr uint32_t kGiWinEventMouseWheel = 4u;

using PfnGiBroadcastRawInput = int32_t(__stdcall*)(void*);
using PfnGiBroadcastWindowEvent = int32_t(__stdcall*)(void*, uint32_t, int32_t, int32_t);

struct GameInputBridgeCache {
    std::atomic<HMODULE> mod{nullptr};
    std::atomic<PfnGiBroadcastRawInput> raw_fn{nullptr};
    std::atomic<PfnGiBroadcastWindowEvent> win_fn{nullptr};
};

GameInputBridgeCache& GetGameInputBridge() noexcept {
    static GameInputBridgeCache cache;
    return cache;
}

void EnsureGameInputBridgeResolved() noexcept {
    auto& c = GetGameInputBridge();
    if (c.win_fn.load(std::memory_order_acquire) != nullptr) {
        return;
    }

    auto try_bind = [&](HMODULE m) noexcept -> bool {
        if (!m) {
            return false;
        }
        auto wfn = reinterpret_cast<PfnGiBroadcastWindowEvent>(
            reinterpret_cast<void*>(GetProcAddress(m, "OpenScarlettGameInputBroadcastWindowEvent")));
        if (!wfn) {
            return false;
        }
        auto rfn = reinterpret_cast<PfnGiBroadcastRawInput>(
            reinterpret_cast<void*>(GetProcAddress(m, "OpenScarlettGameInputBroadcastRawInput")));
        if (rfn) {
            c.raw_fn.store(rfn, std::memory_order_release);
        }
        c.win_fn.store(wfn, std::memory_order_release);
        c.mod.store(m, std::memory_order_release);
        return true;
    };

    // Prefer our OpenScarlett GameInput.dll shim path explicitly so co-resident
    // C:\Windows\System32\GameInput.dll (loaded as native host backend) never shadows it.
    wchar_t shim_dir[MAX_PATH]{};
    const DWORD slen = GetEnvironmentVariableW(L"OPENSCARLETT_SHIM_DIR", shim_dir, MAX_PATH);
    if (slen > 0u && slen < MAX_PATH) {
        std::wstring shim_gi(shim_dir, slen);
        if (!shim_gi.empty() && shim_gi.back() != L'\\' && shim_gi.back() != L'/') {
            shim_gi.push_back(L'\\');
        }
        shim_gi.append(L"GameInput.dll");
        if (try_bind(GetModuleHandleW(shim_gi.c_str()))) {
            return;
        }
    }

    // Fallback: sibling GameInput.dll next to d3d12_xs.dll.
    HMODULE self_mod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&EnsureGameInputBridgeResolved),
            &self_mod) != FALSE &&
        self_mod != nullptr) {
        wchar_t self_path[MAX_PATH]{};
        const DWORD plen = GetModuleFileNameW(self_mod, self_path, MAX_PATH);
        if (plen > 0u && plen < MAX_PATH) {
            std::wstring sibling(self_path, plen);
            const size_t slash = sibling.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                sibling.resize(slash + 1u);
                sibling.append(L"GameInput.dll");
                if (try_bind(GetModuleHandleW(sibling.c_str()))) {
                    return;
                }
            }
        }
    }

    (void)try_bind(GetModuleHandleW(L"GameInput.dll"));
}

void BroadcastRawInputToGameInput(void* h_raw_input) noexcept {
    EnsureGameInputBridgeResolved();
    if (auto fn = GetGameInputBridge().raw_fn.load(std::memory_order_acquire)) {
        (void)fn(h_raw_input);
    }
}

bool BroadcastWindowEventToGameInput(
    HWND hwnd, uint32_t event_kind, int32_t p1 = 0, int32_t p2 = 0) noexcept {
    EnsureGameInputBridgeResolved();
    if (auto fn = GetGameInputBridge().win_fn.load(std::memory_order_acquire)) {
        return fn(reinterpret_cast<void*>(hwnd), event_kind, p1, p2) >= 0;
    }
    return false;
}

bool IsGameInputCursorLocked() noexcept {
    using PfnIsAnyCursorLocked = bool(__cdecl*)();
    HMODULE gi_mod = GetModuleHandleW(L"GameInput.dll");
    if (!gi_mod) {
        return false;
    }
    auto fn = reinterpret_cast<PfnIsAnyCursorLocked>(
        reinterpret_cast<void*>(GetProcAddress(gi_mod, "OpenScarlettGameInputIsAnyCursorLocked")));
    return fn ? fn() : false;
}

void TryRegisterRawMouseForWindow(HWND hwnd, WindowCallbackState& state) noexcept {
    if (!hwnd || IsWindow(hwnd) == FALSE || IsWindowVisible(hwnd) == FALSE) {
        return;
    }
    if (state.raw_mouse_registered.load(std::memory_order_acquire)) {
        return;
    }
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01u;  // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02u;      // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = 0u;
    rid.hwndTarget = hwnd;
    if (RegisterRawInputDevices(&rid, 1u, static_cast<UINT>(sizeof(rid))) != FALSE) {
        state.raw_mouse_registered.store(true, std::memory_order_release);
    }
}

uint32_t QueryWindowMonitorRefreshHz(HWND hwnd) noexcept {
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (mon) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi) != FALSE) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) != FALSE &&
                dm.dmDisplayFrequency >= 24u && dm.dmDisplayFrequency <= 500u) {
                return static_cast<uint32_t>(dm.dmDisplayFrequency);
            }
        }
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) != FALSE &&
        dm.dmDisplayFrequency >= 24u && dm.dmDisplayFrequency <= 500u) {
        return static_cast<uint32_t>(dm.dmDisplayFrequency);
    }
    return 60u;
}

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

bool IsBackgroundKeepActiveEnabled() noexcept {
    static const bool s_keep_active = []() {
        const char* vars[] = {
            "OPENSCARLETT_GAMEINPUT_BACKGROUND",
            "OPENSCARLETT_AUTONAV",
            "OPENSCARLETT_LIVE_INPUT_FILE",
            "OPENSCARLETT_RECORD_WAV",
        };
        for (const char* v : vars) {
            char buf[32]{};
            const DWORD n = GetEnvironmentVariableA(v, buf, static_cast<DWORD>(sizeof(buf)));
            if (n > 0u && buf[0] != '0') {
                return true;
            }
        }
        return false;
    }();
    return s_keep_active;
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
            case WM_ERASEBKGND: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                if (hdc) {
                    RECT rc{};
                    if (GetClientRect(hwnd, &rc) != FALSE) {
                        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                    }
                }
                return 1;
            }
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
            case WM_ACTIVATE: {
                const bool active = (LOWORD(wParam) != WA_INACTIVE);
                if (!active && IsBackgroundKeepActiveEnabled()) {
                    state->is_active.store(true, std::memory_order_release);
                    BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, 1, 0);
                    return 0;
                }
                state->is_active.store(active, std::memory_order_release);
                if (active) {
                    TryRegisterRawMouseForWindow(hwnd, *state);
                } else if (state->cursor_clipped.exchange(false, std::memory_order_acq_rel)) {
                    ClipCursor(nullptr);
                }
                BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, active ? 1 : 0, 0);
                break;
            }
            case WM_ACTIVATEAPP: {
                const bool active = (wParam != FALSE);
                if (!active && IsBackgroundKeepActiveEnabled()) {
                    state->is_active.store(true, std::memory_order_release);
                    BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, 1, 0);
                    return 0;
                }
                state->is_active.store(active, std::memory_order_release);
                if (active) {
                    TryRegisterRawMouseForWindow(hwnd, *state);
                } else if (state->cursor_clipped.exchange(false, std::memory_order_acq_rel)) {
                    ClipCursor(nullptr);
                }
                BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, active ? 1 : 0, 0);
                break;
            }
            case WM_SETFOCUS: {
                state->is_active.store(true, std::memory_order_release);
                TryRegisterRawMouseForWindow(hwnd, *state);
                BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, 1, 0);
                break;
            }
            case WM_KILLFOCUS: {
                if (IsBackgroundKeepActiveEnabled()) {
                    state->is_active.store(true, std::memory_order_release);
                    BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, 1, 0);
                    return 0;
                }
                state->is_active.store(false, std::memory_order_release);
                if (state->cursor_clipped.exchange(false, std::memory_order_acq_rel)) {
                    ClipCursor(nullptr);
                }
                BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, 0, 0);
                break;
            }
            case WM_SETCURSOR: {
                if (LOWORD(lParam) == HTCLIENT && state->is_active.load(std::memory_order_acquire) &&
                    IsGameInputCursorLocked()) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            }
            case WM_INPUT: {
                state->raw_mouse_packets_seen.store(true, std::memory_order_release);
                BroadcastRawInputToGameInput(reinterpret_cast<void*>(lParam));
                break;
            }
            case WM_DEVICECHANGE: {
                BroadcastWindowEventToGameInput(hwnd, kGiWinEventDeviceChange, static_cast<int32_t>(wParam), 0);
                break;
            }
            case WM_MOUSEWHEEL: {
                BroadcastWindowEventToGameInput(
                    hwnd, kGiWinEventMouseWheel, 0, static_cast<int32_t>(GET_WHEEL_DELTA_WPARAM(wParam)));
                break;
            }
#if defined(WM_MOUSEHWHEEL)
            case WM_MOUSEHWHEEL: {
                BroadcastWindowEventToGameInput(
                    hwnd, kGiWinEventMouseWheel, static_cast<int32_t>(GET_WHEEL_DELTA_WPARAM(wParam)), 0);
                break;
            }
#endif
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
                if (state->cursor_clipped.exchange(false, std::memory_order_acq_rel)) {
                    ClipCursor(nullptr);
                }
                state->close_requested.store(true, std::memory_order_release);
                if (state->post_quit_on_close.load(std::memory_order_acquire)) {
                    if (!state->quit_posted.exchange(true, std::memory_order_acq_rel)) {
                        PostQuitMessage(0);
                    }
                }
                break;
            }
            case WM_NCDESTROY: {
                if (state->cursor_clipped.exchange(false, std::memory_order_acq_rel)) {
                    ClipCursor(nullptr);
                }
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
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
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

bool IsWritableCpuVaCached(
    uint64_t va,
    size_t byte_size,
    uint64_t& cached_lo,
    uint64_t& cached_hi) noexcept {
    if (va == 0ull || byte_size == 0u || va > UINT64_MAX - byte_size) return false;
    if (va >= cached_lo && (va + byte_size) <= cached_hi) {
        return true;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(va)), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kWritable) == 0u || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const uint64_t region_lo = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress));
    const uint64_t region_hi = region_lo + static_cast<uint64_t>(mbi.RegionSize);
    if ((va + byte_size) > region_hi) {
        return false;
    }
    cached_lo = region_lo;
    cached_hi = region_hi;
    return true;
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

    if (device_x_) {
        device_x_->AddRef();
        // Retail Xbox Series X games present at full 1080p/4K interactively; disable per-frame
        // CPU PCIe readback + FNV1a hashing by default unless explicitly requested via
        // SetReadbackCaptureEnabled(TRUE) or OPENSCARLETT_PRESENTX_CAPTURE_PNG.
        if (device_x_->IsRetailScarlettAbi()) {
            readback_capture_enabled_ = false;
        }
    }
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
        // Retail Xbox Series X|S titles never call Win32 CreateWindowExW; they present
        // directly to the console display via PresentX. Auto-create a live 1280x720 Win32
        // output window on a dedicated message-pump thread when running interactively.
        if (device_x_ && device_x_->IsRetailScarlettAbi()) {
            static std::once_flag s_auto_win_once;
            static HWND s_auto_hwnd = nullptr;
            std::call_once(s_auto_win_once, []() {
                HANDLE ready_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!ready_ev) return;
                std::thread([ready_ev]() {
                    constexpr const wchar_t* kClassName = L"OpenScarlettRetailDisplayClass";
                    WNDCLASSEXW wc{};
                    wc.cbSize = sizeof(wc);
                    wc.style = CS_HREDRAW | CS_VREDRAW;
                    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                        if (msg == WM_CLOSE) {
                            ::DestroyWindow(hwnd);
                            return 0;
                        }
                        if (msg == WM_DESTROY) {
                            ::PostQuitMessage(0);
                            return 0;
                        }
                        return ::DefWindowProcW(hwnd, msg, wp, lp);
                    };
                    wc.hInstance = ::GetModuleHandleW(nullptr);
                    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
                    wc.lpszClassName = kClassName;
                    ::RegisterClassExW(&wc);

                    LONG win_w = 1280;
                    LONG win_h = 720;
                    {
                        char rbuf[64]{};
                        const DWORD rlen = ::GetEnvironmentVariableA("OPENSCARLETT_DISPLAY_RES", rbuf, sizeof(rbuf));
                        if (rlen > 0u && rlen < sizeof(rbuf)) {
                            char* sep = nullptr;
                            const unsigned long ew = std::strtoul(rbuf, &sep, 10);
                            if (sep && (*sep == 'x' || *sep == 'X')) {
                                const unsigned long eh = std::strtoul(sep + 1, nullptr, 10);
                                if (ew >= 320ul && eh >= 180ul && ew <= 3840ul && eh <= 2160ul) {
                                    win_w = static_cast<LONG>(ew);
                                    win_h = static_cast<LONG>(eh);
                                }
                            }
                        }
                    }
                    RECT r{0, 0, win_w, win_h};
                    ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
                    HWND hw = ::CreateWindowExW(
                        0, kClassName, L"OpenScarlett - Xbox Series X|S",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top,
                        nullptr, nullptr, wc.hInstance, nullptr);
                    s_auto_hwnd = hw;
                    if (hw) {
                        ::ShowWindow(hw, SW_SHOW);
                        ::UpdateWindow(hw);
                    }
                    ::SetEvent(ready_ev);
                    if (!hw) return;
                    MSG m{};
                    while (::GetMessageW(&m, nullptr, 0, 0) > 0) {
                        ::TranslateMessage(&m);
                        ::DispatchMessageW(&m);
                    }
                }).detach();
                ::WaitForSingleObject(ready_ev, 3000u);
                ::CloseHandle(ready_ev);
            });
            if (s_auto_hwnd && ::IsWindow(s_auto_hwnd) != FALSE) {
                attached_hwnd_ = s_auto_hwnd;
                device_x_->AdoptTitleWindow(s_auto_hwnd);
                return s_auto_hwnd;
            }
        }
    }
    return nullptr;
}

void CommandQueueX::PumpPendingRetirementsLocked() noexcept {
    if (!retire_fence_ || pending_retirements_.empty()) return;
    const uint64_t completed = retire_fence_->GetCompletedValue();
    if (completed == UINT64_MAX) return;
    size_t erase_n = 0u;
    while (erase_n < pending_retirements_.size() &&
           pending_retirements_[erase_n].first <= completed) {
        for (ID3D12Resource* r : pending_retirements_[erase_n].second) {
            if (r) r->Release();
        }
        ++erase_n;
    }
    if (erase_n > 0u) {
        pending_retirements_.erase(
            pending_retirements_.begin(),
            pending_retirements_.begin() + static_cast<ptrdiff_t>(erase_n));
    }
}

CommandQueueX::~CommandQueueX() {
    {
        std::lock_guard<std::mutex> rlk(retire_mutex_);
        if (retire_fence_ && !pending_retirements_.empty()) {
            const uint64_t target = pending_retirements_.back().first;
            if (retire_fence_->GetCompletedValue() < target) {
                HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) {
                    if (SUCCEEDED(retire_fence_->SetEventOnCompletion(target, ev))) {
                        ::WaitForSingleObject(ev, 2000u);
                    }
                    ::CloseHandle(ev);
                }
            }
        }
        for (auto& batch : pending_retirements_) {
            for (ID3D12Resource* r : batch.second) {
                if (r) r->Release();
            }
        }
        pending_retirements_.clear();
        if (retire_fence_) {
            retire_fence_->Release();
            retire_fence_ = nullptr;
        }
    }
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
        std::lock_guard<std::mutex> dwlk(deferred_write_mutex_);
        if (deferred_write_fence_) {
            deferred_write_fence_->Release();
            deferred_write_fence_ = nullptr;
        }
    }
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

namespace {
std::atomic<uint32_t> g_prof_exec_cnt{0u};
std::atomic<int64_t> g_prof_exec_qpc{0};
std::atomic<int64_t> g_prof_sync_qpc{0};
std::atomic<uint32_t> g_prof_w32_cnt{0u};
std::atomic<int64_t> g_prof_w32_qpc{0};
}  // namespace

void STDMETHODCALLTYPE CommandQueueX::ExecuteCommandLists(
    UINT NumCommandLists,
    ID3D12CommandList* const* ppCommandLists) {
    const int64_t exec_t0_qpc = QueryQpcNow();
    struct ExecProfGuard {
        int64_t t0;
        ~ExecProfGuard() {
            g_prof_exec_cnt.fetch_add(1u, std::memory_order_relaxed);
            g_prof_exec_qpc.fetch_add(QueryQpcNow() - t0, std::memory_order_relaxed);
        }
    } exec_prof_guard{exec_t0_qpc};
    // Auto-sync all dirty/modified unified-memory textures, unified heap buffers, & TitleVAMirror
    // pages BEFORE submitting host command lists so GPU shaders never sample stale CPU memory.
    auto dump_host_info_queue = [](ID3D12Device* host_dev, const char* tag) {
        if (!host_dev) return;
        ID3D12InfoQueue* iq = nullptr;
        if (FAILED(host_dev->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) return;
        const UINT64 nmsg = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 mi = 0u; mi < nmsg && mi < 64u; ++mi) {
            SIZE_T len = 0u;
            iq->GetMessage(mi, nullptr, &len);
            if (len > 0u) {
                std::vector<uint8_t> mb(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(mb.data());
                if (SUCCEEDED(iq->GetMessage(mi, msg, &len)) && msg->pDescription &&
                    msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                    std::fprintf(stderr, "[OpenScarlett][%s][D3D12-IQ sev=%d id=%d] %s\n",
                                 tag, static_cast<int>(msg->Severity),
                                 static_cast<int>(msg->ID), msg->pDescription);
                }
            }
        }
        iq->ClearStoredMessages();
        iq->Release();
        std::fflush(stderr);
    };
    if (device_x_ && host_queue_) {
        const HRESULT pre_rem = (device_x_->IsRetailScarlettAbi() && device_x_->HostDevice())
                                    ? device_x_->HostDevice()->GetDeviceRemovedReason()
                                    : S_OK;
        if (FAILED(pre_rem)) {
            static std::atomic<bool> s_pre_rem{false};
            if (!s_pre_rem.exchange(true)) {
                std::fprintf(stderr, "[OpenScarlett][EXEC-PRE-REMOVED] rem=0x%08lX n=%u qtype=%d\n",
                             static_cast<unsigned long>(pre_rem), NumCommandLists, static_cast<int>(queue_type_));
                dump_host_info_queue(device_x_->HostDevice(), "EXEC-PRE-REMOVED");
                std::fflush(stderr);
            }
        }
        const int64_t sync_t0 = QueryQpcNow();
        (void)device_x_->SyncAllUnifiedResourcesBeforeSubmit(host_queue_, queue_type_);
        g_prof_sync_qpc.fetch_add(QueryQpcNow() - sync_t0, std::memory_order_relaxed);
        if (SUCCEEDED(pre_rem) && device_x_->IsRetailScarlettAbi() && device_x_->HostDevice()) {
            const HRESULT mid_rem = device_x_->HostDevice()->GetDeviceRemovedReason();
            if (FAILED(mid_rem)) {
                static std::atomic<bool> s_mid_rem{false};
                if (!s_mid_rem.exchange(true)) {
                    std::fprintf(stderr, "[OpenScarlett][EXEC-POST-SYNC-REMOVED] rem=0x%08lX n=%u qtype=%d\n",
                                 static_cast<unsigned long>(mid_rem), NumCommandLists, static_cast<int>(queue_type_));
                    dump_host_info_queue(device_x_->HostDevice(), "EXEC-POST-SYNC-REMOVED");
                    std::fflush(stderr);
                }
            }
        }
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
    std::vector<ID3D12Resource*> batch_transients;
    UINT valid_list_count = 0u;
    std::vector<CommandListX::DeferredStateTransition> deferred_state_transitions;
    for (UINT i = 0; i < NumCommandLists; ++i) {
        if (CommandListX* clx = CommandListX::TryFromCommandList(ppCommandLists[i])) {
            if (clx->LastCloseFailed()) {
                // Still honor CPU fence/marker writes & retirements so CPU wait loops never stall.
                clx->ExtractDeferredCpuOps(deferred_writes, deferred_waits);
                clx->ExtractTransientResources(batch_transients);
                continue;  // Never pass an unclosed/invalid command list to host D3D12 queue
            }
            clx->ExtractPendingMirrorWritebacks(pending_writeback_vas);
            clx->ExtractDeferredCpuOps(deferred_writes, deferred_waits);
            clx->ExtractDeferredStateTransitions(deferred_state_transitions);
            clx->ExtractTransientResources(batch_transients);
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
        if (device_x_ && device_x_->IsRetailScarlettAbi() && device_x_->HostDevice()) {
            const HRESULT post_rem = device_x_->HostDevice()->GetDeviceRemovedReason();
            if (FAILED(post_rem)) {
                static std::atomic<bool> s_post_rem{false};
                if (!s_post_rem.exchange(true)) {
                    std::fprintf(stderr, "[OpenScarlett][EXEC-POST-SUBMIT-REMOVED] rem=0x%08lX valid=%u qtype=%d\n",
                                 static_cast<unsigned long>(post_rem), valid_list_count, static_cast<int>(queue_type_));
                    dump_host_info_queue(device_x_->HostDevice(), "EXEC-POST-SUBMIT-REMOVED");
                    std::fflush(stderr);
                }
            }
        }
    }
    if (!batch_transients.empty() || !pending_retirements_.empty()) {
        std::lock_guard<std::mutex> rlk(retire_mutex_);
        if (!batch_transients.empty() && device_x_ && device_x_->HostDevice()) {
            if (!retire_fence_) {
                (void)device_x_->HostDevice()->CreateFence(
                    0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence),
                    reinterpret_cast<void**>(&retire_fence_));
            }
            if (retire_fence_) {
                const uint64_t fv = ++retire_fence_val_;
                if (SUCCEEDED(host_queue_->Signal(retire_fence_, fv))) {
                    pending_retirements_.emplace_back(fv, std::move(batch_transients));
                } else {
                    for (ID3D12Resource* r : batch_transients) {
                        if (r) r->Release();
                    }
                }
            } else {
                for (ID3D12Resource* r : batch_transients) {
                    if (r) r->Release();
                }
            }
        }
        PumpPendingRetirementsLocked();
    }
    if (device_x_ && CommandListX::IsDiagTraceEnabled() && device_x_->HostDevice()) {
        static const bool s_sync_diag_drain = EnvFlagTrueA("OPENSCARLETT_SYNC_DIAG_DRAIN");
        static std::mutex s_diag_mu;
        static std::atomic<bool> s_dumped{false};
        if (!s_dumped.load(std::memory_order_acquire)) {
            if (s_sync_diag_drain) {
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
            }
            const HRESULT rem_hr = device_x_->HostDevice()->GetDeviceRemovedReason();
            static const bool dump_dbg_iq = []() {
                char v[16]{};
                return ::GetEnvironmentVariableA("OPENSCARLETT_ENABLE_D3D12_DEBUG", v, sizeof(v)) > 0 && v[0] == '1';
            }();
            if (dump_dbg_iq) {
                static std::atomic<uint32_t> s_iq_printed{0u};
                ID3D12InfoQueue* iq = nullptr;
                if (s_iq_printed.load(std::memory_order_relaxed) < 80u &&
                    SUCCEEDED(device_x_->HostDevice()->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
                    const UINT64 nmsg = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
                    for (UINT64 mi = 0u; mi < nmsg && s_iq_printed.load(std::memory_order_relaxed) < 80u; ++mi) {
                        SIZE_T len = 0u;
                        iq->GetMessage(mi, nullptr, &len);
                        if (len > 0u) {
                            std::vector<uint8_t> mb(len);
                            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(mb.data());
                            if (SUCCEEDED(iq->GetMessage(mi, msg, &len)) && msg->pDescription &&
                                msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                                s_iq_printed.fetch_add(1u, std::memory_order_relaxed);
                                std::fprintf(stderr, "[D3D12-DBG sev=%d id=%d] %s\n",
                                             static_cast<int>(msg->Severity),
                                             static_cast<int>(msg->ID),
                                             msg->pDescription);
                            }
                        }
                    }
                    iq->ClearStoredMessages();
                    iq->Release();
                    std::fflush(stderr);
                }
            }
            static const bool dump_all_ops = []() {
                char v[16]{};
                return ::GetEnvironmentVariableA("OPENSCARLETT_DUMP_CL_OPS", v, sizeof(v)) > 0 && v[0] == '1';
            }();
            static const uint32_t dump_after_exec = []() -> uint32_t {
                char v[32]{};
                if (::GetEnvironmentVariableA("OPENSCARLETT_DUMP_CL_AFTER_EXEC", v, sizeof(v)) > 0) {
                    return static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
                }
                return 0u;
            }();
            static const uint32_t dump_after_lift = []() -> uint32_t {
                char v[32]{};
                if (::GetEnvironmentVariableA("OPENSCARLETT_DUMP_AFTER_LIFT", v, sizeof(v)) > 0) {
                    return static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
                }
                return 0u;
            }();
            static std::atomic<uint32_t> s_dump_exec_count{0u};
            static std::atomic<uint32_t> s_dump_dii_count{0u};
            static std::atomic<uint32_t> s_dump_lift_count{0u};
            static std::array<std::string, 6> s_recent_exec_ring;
            static uint32_t s_recent_exec_pos = 0u;
            if (dump_all_ops) {
                const uint32_t seq = s_dump_exec_count.fetch_add(1u, std::memory_order_relaxed);
                {
                    std::string snap;
                    char hdr[128];
                    std::snprintf(hdr, sizeof(hdr),
                                  "\n=== RING EXEC #%u (n=%u, valid=%u, queue_type=%d, rem=0x%08X) ===\n",
                                  seq, NumCommandLists, valid_list_count, static_cast<int>(queue_type_),
                                  static_cast<unsigned>(rem_hr));
                    snap += hdr;
                    for (UINT i = 0; i < NumCommandLists; ++i) {
                        if (CommandListX* clx = CommandListX::TryFromCommandList(ppCommandLists[i])) {
                            const auto ops = clx->CopyDiagOps();
                            char lh[64];
                            std::snprintf(lh, sizeof(lh), "--- List[%u] (%zu ops) ---\n", i, ops.size());
                            snap += lh;
                            for (const auto& op : ops) {
                                snap += "  ";
                                snap += op;
                                snap += '\n';
                            }
                        }
                    }
                    std::lock_guard<std::mutex> dlk(s_diag_mu);
                    s_recent_exec_ring[s_recent_exec_pos % s_recent_exec_ring.size()] = std::move(snap);
                    ++s_recent_exec_pos;
                }
                bool has_dii = false;
                if (dump_after_exec == 0u && dump_after_lift == 0u && seq >= 25u &&
                    s_dump_dii_count.load(std::memory_order_relaxed) < 4u) {
                    for (UINT i = 0; i < NumCommandLists && !has_dii; ++i) {
                        if (CommandListX* clx = CommandListX::TryFromCommandList(ppCommandLists[i])) {
                            const auto ops = clx->CopyDiagOps();
                            for (const auto& op : ops) {
                                if (op.find("DrawIndexedInstanced") != std::string::npos) {
                                    has_dii = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                const bool dump_now =
                    (dump_after_lift > 0u)
                        ? (device_x_ && device_x_->LazyLiftCount() >= dump_after_lift &&
                           s_dump_lift_count.fetch_add(1u, std::memory_order_relaxed) < 6u)
                        : ((dump_after_exec > 0u)
                               ? (seq >= dump_after_exec && seq < dump_after_exec + 6u)
                               : ((seq < 5u) || (has_dii && s_dump_dii_count.fetch_add(1u, std::memory_order_relaxed) < 4u)));
                if (dump_now) {
                    std::lock_guard<std::mutex> dlk(s_diag_mu);
                    FILE* f = nullptr;
                    if (::fopen_s(&f, CommandListX::DiagTraceLogPath(), "a") == 0 && f) {
                        const uint32_t idx = (s_recent_exec_pos - 1u) % static_cast<uint32_t>(s_recent_exec_ring.size());
                        std::fwrite(s_recent_exec_ring[idx].data(), 1u, s_recent_exec_ring[idx].size(), f);
                        std::fclose(f);
                    }
                }
            }
            if (FAILED(rem_hr) && !s_dumped.exchange(true)) {
                if (dump_all_ops) {
                    std::lock_guard<std::mutex> dlk(s_diag_mu);
                    FILE* f = nullptr;
                    if (::fopen_s(&f, CommandListX::DiagTraceLogPath(), "a") == 0 && f) {
                        std::fprintf(f, "\n========== DEVICE REMOVED (rem=0x%08X) LAST 6 EXECS ==========\n",
                                     static_cast<unsigned>(rem_hr));
                        const uint32_t n = static_cast<uint32_t>(s_recent_exec_ring.size());
                        for (uint32_t k = 0u; k < n; ++k) {
                            const uint32_t idx = (s_recent_exec_pos + k) % n;
                            if (!s_recent_exec_ring[idx].empty()) {
                                std::fwrite(s_recent_exec_ring[idx].data(), 1u, s_recent_exec_ring[idx].size(), f);
                            }
                        }
                        std::fclose(f);
                    }
                }
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

    // Apply deferred unified-texture / TitleVAMirror resource state transitions AFTER
    // host_queue_->ExecuteCommandLists so pre-submit SyncAllUnifiedResourcesBeforeSubmit
    // sees the true pre-submission resource states (fixing WARP & D3D12 Debug Layer barrier errors).
    if (device_x_ && !deferred_state_transitions.empty()) {
        for (const auto& st : deferred_state_transitions) {
            if (st.host_resource) {
                device_x_->NotifyResourceStateTransition(st.host_resource, st.after_state);
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
            PumpMirrorReadbackCompletionLocked();
            const uint32_t rb_slot = rb_ring_idx_ % kReadbackRingSlots;
            rb_ring_idx_ = (rb_ring_idx_ + 1u) % kReadbackRingSlots;
            const uint64_t slot_wait = rb_ring_fences_[rb_slot];
            if (slot_wait > 0ull && present_fence_->GetCompletedValue() < slot_wait) {
                if (SUCCEEDED(present_fence_->SetEventOnCompletion(slot_wait, present_fence_event_))) {
                    WaitForSingleObject(present_fence_event_, 15000u);
                }
                PumpMirrorReadbackCompletionLocked();
            }
            ID3D12CommandAllocator* rb_alloc = rb_ring_allocs_[rb_slot];
            ID3D12GraphicsCommandList* rb_cmd = rb_ring_cmds_[rb_slot];
            if (rb_alloc && rb_cmd &&
                SUCCEEDED(rb_alloc->Reset()) &&
                SUCCEEDED(rb_cmd->Reset(rb_alloc, nullptr))) {
                const uint64_t rb_fence = ++present_fence_val_;
                uint32_t scheduled = 0u;
                for (uint64_t va : pending_writeback_vas) {
                    openscarlett::TitleVaRegionInfo rinfo{};
                    if (device_x_->GetTitleVaMirror().LookupRegion(va, rinfo)) {
                        if (device_x_->GetTitleVaMirror().ScheduleGpuReadback(
                                rb_cmd, va, rb_fence, 0ull, 0ull, rinfo.current_state).ok()) {
                            ++scheduled;
                        }
                    }
                }
                if (scheduled > 0u && SUCCEEDED(rb_cmd->Close())) {
                    ID3D12CommandList* rb_lists[] = {rb_cmd};
                    host_queue_->ExecuteCommandLists(1u, rb_lists);
                    host_queue_->Signal(present_fence_, rb_fence);
                    rb_ring_fences_[rb_slot] = rb_fence;
                } else {
                    rb_cmd->Close();
                }
            }
        }
    }

    if (!deferred_writes.empty()) {
        static const bool s_sync_dw = EnvFlagTrueA("OPENSCARLETT_SYNC_DEFERRED_WRITES");
        const int64_t qpc_freq = qpc_freq_;
        auto commit_writes = [qpc_freq](const std::vector<DeferredCpuWrite32>& writes) {
            uint64_t cached_lo = 0ull;
            uint64_t cached_hi = 0ull;
            for (const auto& wr : writes) {
                if (wr.is_64bit) {
                    if (IsWritableCpuVaCached(wr.address, sizeof(uint64_t), cached_lo, cached_hi)) {
                        uint64_t v = wr.value;
                        if (wr.is_timestamp) {
                            const int64_t now = QueryQpcNow();
                            v = (qpc_freq > 0)
                                    ? static_cast<uint64_t>((static_cast<double>(now) * 99750000.0) /
                                                            static_cast<double>(qpc_freq))
                                    : static_cast<uint64_t>(now);
                        }
                        ::InterlockedExchange64(
                            reinterpret_cast<volatile LONG64*>(static_cast<uintptr_t>(wr.address)),
                            static_cast<LONG64>(v));
                    }
                } else if (IsWritableCpuVaCached(wr.address, sizeof(uint32_t), cached_lo, cached_hi)) {
                    ::InterlockedExchange(
                        reinterpret_cast<volatile LONG*>(static_cast<uintptr_t>(wr.address)),
                        static_cast<LONG>(static_cast<uint32_t>(wr.value)));
                }
            }
        };

        bool dispatched_async = false;
        if (!s_sync_dw && fence_dispatcher_ && host_queue_ && device_x_ &&
            device_x_->IsRetailScarlettAbi() && device_x_->HostDevice()) {
            ID3D12Fence* dw_fence = nullptr;
            uint64_t dw_val = 0ull;
            {
                std::lock_guard<std::mutex> dwlk(deferred_write_mutex_);
                if (!deferred_write_fence_) {
                    (void)device_x_->HostDevice()->CreateFence(
                        0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence),
                        reinterpret_cast<void**>(&deferred_write_fence_));
                }
                if (deferred_write_fence_) {
                    dw_val = ++deferred_write_fence_val_;
                    if (SUCCEEDED(host_queue_->Signal(deferred_write_fence_, dw_val))) {
                        dw_fence = deferred_write_fence_;
                    }
                }
            }
            if (dw_fence) {
                struct AsyncWriteBatch {
                    std::vector<DeferredCpuWrite32> writes;
                    int64_t qpc_freq = 0;
                };
                auto* batch = new (std::nothrow) AsyncWriteBatch{std::move(deferred_writes), qpc_freq};
                if (batch) {
                    const HRESULT rhr = fence_dispatcher_->RegisterCallback(
                        dw_fence,
                        dw_val,
                        [](ID3D12Fence*, UINT64, HRESULT, void* user_ctx) {
                            std::unique_ptr<AsyncWriteBatch> owned(static_cast<AsyncWriteBatch*>(user_ctx));
                            if (!owned) return;
                            uint64_t cached_lo = 0ull;
                            uint64_t cached_hi = 0ull;
                            for (const auto& wr : owned->writes) {
                                if (wr.is_64bit) {
                                    if (IsWritableCpuVaCached(wr.address, sizeof(uint64_t), cached_lo, cached_hi)) {
                                        uint64_t v = wr.value;
                                        if (wr.is_timestamp) {
                                            const int64_t now = QueryQpcNow();
                                            v = (owned->qpc_freq > 0)
                                                    ? static_cast<uint64_t>((static_cast<double>(now) * 99750000.0) /
                                                                            static_cast<double>(owned->qpc_freq))
                                                    : static_cast<uint64_t>(now);
                                        }
                                        ::InterlockedExchange64(
                                            reinterpret_cast<volatile LONG64*>(static_cast<uintptr_t>(wr.address)),
                                            static_cast<LONG64>(v));
                                    }
                                } else if (IsWritableCpuVaCached(wr.address, sizeof(uint32_t), cached_lo, cached_hi)) {
                                    ::InterlockedExchange(
                                        reinterpret_cast<volatile LONG*>(static_cast<uintptr_t>(wr.address)),
                                        static_cast<LONG>(static_cast<uint32_t>(wr.value)));
                                }
                            }
                        },
                        batch,
                        nullptr);
                    if (SUCCEEDED(rhr)) {
                        dispatched_async = true;
                    } else {
                        deferred_writes = std::move(batch->writes);
                        delete batch;
                    }
                }
            }
        }
        if (!dispatched_async) {
            {
                std::lock_guard<std::mutex> lock(present_mutex_);
                if (SUCCEEDED(EnsurePresentHelperObjectsLocked())) {
                    WaitForInternalFenceLocked();
                }
            }
            commit_writes(deferred_writes);
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
            case VulkanRecordedOpKind::ExecuteIndirect: {
                const bool is_dispatch =
                    (op.indirect_action_type == static_cast<uint32_t>(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH));
                const bool is_mesh = (op.indirect_action_type == 10u);
                const bool is_indexed =
                    (op.indirect_action_type == static_cast<uint32_t>(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED));
                if (is_dispatch) ++dispatches_cnt;
                else if (is_mesh) ++mesh_cnt;
                else if (is_indexed) ++indexed_draws_cnt;
                else ++draws_cnt;

                openscarlett::VulkanCommandItem item{};
                item.op = is_dispatch ? openscarlett::VulkanCommandOp::Dispatch : openscarlett::VulkanCommandOp::Nop;
                item.x = std::max<uint32_t>(1u, op.indirect_max_count);
                item.y = 1u;
                item.z = 1u;
                vk_cl_desc.commands.push_back(item);

                if (op.is_spirv_direct && op.pso && op.indirect_arg_resource) {
                    VulkanPipelineInfo pi{};
                    if (device_x_->LookupVulkanPipeline(op.pso, pi)) {
                        const D3D12_RESOURCE_DESC ad = op.indirect_arg_resource->GetDesc();
                        std::vector<uint8_t> arg_bytes;
                        if (device_x_->ReadVulkanBufferData(
                                op.indirect_arg_resource, 0ull, ad.Width, arg_bytes) &&
                            !arg_bytes.empty()) {
                            if (is_dispatch && !pi.cs_spirv.empty()) {
                                openscarlett::VulkanIndirectComputeDesc cdesc{};
                                cdesc.spirv = pi.cs_spirv;
                                cdesc.argument_buffer_bytes = std::move(arg_bytes);
                                cdesc.argument_buffer_offset = op.indirect_arg_offset;
                                uint64_t in_off = 0ull, out_off = 0ull;
                                ID3D12Resource* in_res =
                                    !op.root_srvs.empty()
                                        ? device_x_->ResolveVulkanBufferByGpuVa(op.root_srvs[0], &in_off)
                                        : nullptr;
                                ID3D12Resource* out_res =
                                    !op.root_uavs.empty()
                                        ? device_x_->ResolveVulkanBufferByGpuVa(op.root_uavs[0], &out_off)
                                        : nullptr;
                                if (in_res) {
                                    std::vector<uint8_t> raw_in;
                                    const uint64_t in_sz = in_res->GetDesc().Width - in_off;
                                    if (device_x_->ReadVulkanBufferData(in_res, in_off, in_sz, raw_in)) {
                                        cdesc.input_u32.resize(raw_in.size() / 4u);
                                        if (!cdesc.input_u32.empty()) {
                                            std::memcpy(cdesc.input_u32.data(), raw_in.data(),
                                                        cdesc.input_u32.size() * 4u);
                                        }
                                    }
                                }
                                if (out_res) {
                                    cdesc.output_word_count =
                                        static_cast<uint32_t>((out_res->GetDesc().Width - out_off) / 4ull);
                                }
                                openscarlett::VulkanIndirectComputeResult cres{};
                                if (vk->ExecuteIndirectCompute(cdesc, cres).ok()) {
                                    ++spirv_dispatches_cnt;
                                    if (cres.executed_on_vulkan_icd) ++icd_execs_cnt;
                                    if (out_res && !cres.output_u32.empty()) {
                                        device_x_->WriteVulkanBufferData(
                                            out_res, out_off, cres.output_u32.data(),
                                            cres.output_u32.size() * sizeof(uint32_t));
                                    }
                                }
                            } else if ((!is_mesh && !pi.vs_spirv.empty() && !pi.ps_spirv.empty()) ||
                                       (is_mesh && !pi.ms_spirv.empty() && !pi.ps_spirv.empty())) {
                                openscarlett::VulkanIndirectDrawDesc idesc{};
                                idesc.kind = is_mesh ? openscarlett::VulkanIndirectCommandKind::DrawMeshTasks
                                                     : (is_indexed ? openscarlett::VulkanIndirectCommandKind::DrawIndexed
                                                                   : openscarlett::VulkanIndirectCommandKind::Draw);
                                idesc.argument_buffer_bytes = std::move(arg_bytes);
                                idesc.argument_buffer_offset = op.indirect_arg_offset;
                                idesc.command_stride = op.indirect_stride;
                                idesc.max_command_count = op.indirect_max_count;
                                idesc.draw_arg_offset_in_stride = op.indirect_action_offset;
                                idesc.indirect_push_constant_arg_offset = op.indirect_const_arg_offset;
                                idesc.indirect_push_constant_bytes = op.indirect_const_bytes;
                                idesc.indirect_push_constant_dst_offset = op.indirect_const_dst_offset;
                                if (op.indirect_count_resource) {
                                    std::vector<uint8_t> cnt_bytes;
                                    if (device_x_->ReadVulkanBufferData(
                                            op.indirect_count_resource, op.indirect_count_offset, 4ull, cnt_bytes) &&
                                        cnt_bytes.size() >= 4u) {
                                        idesc.use_count_buffer = true;
                                        std::memcpy(&idesc.count_buffer_value, cnt_bytes.data(), 4u);
                                        idesc.count_buffer_offset = 0ull;
                                    }
                                }
                                uint32_t w = (op.viewport_width > 0u) ? op.viewport_width : 64u;
                                uint32_t h = (op.viewport_height > 0u) ? op.viewport_height : 64u;
                                if (op.rtv_resource) {
                                    const D3D12_RESOURCE_DESC rdesc = op.rtv_resource->GetDesc();
                                    w = static_cast<uint32_t>(std::max<UINT64>(1ull, rdesc.Width));
                                    h = std::max<UINT>(1u, rdesc.Height);
                                }
                                if (is_mesh) {
                                    idesc.mesh.task_spirv = pi.as_spirv;
                                    idesc.mesh.mesh_spirv = pi.ms_spirv;
                                    idesc.mesh.fragment_spirv = pi.ps_spirv;
                                    idesc.mesh.task_entry = pi.as_entry;
                                    idesc.mesh.mesh_entry = pi.ms_entry;
                                    idesc.mesh.fragment_entry = pi.ps_entry;
                                    idesc.mesh.width = w;
                                    idesc.mesh.height = h;
                                    idesc.mesh.color_format = pi.color_format;
                                    idesc.mesh.enable_depth = pi.enable_depth;
                                    if (op.has_clear_color) {
                                        std::memcpy(idesc.mesh.clear_color, op.clear_color, sizeof(op.clear_color));
                                    }
                                    idesc.mesh.clear_depth = op.clear_depth;
                                    if (!op.root_constants.empty()) {
                                        idesc.mesh.push_constants.resize(op.root_constants.size() * 4u);
                                        std::memcpy(idesc.mesh.push_constants.data(), op.root_constants.data(),
                                                    idesc.mesh.push_constants.size());
                                    }
                                } else {
                                    idesc.graphics.vertex_spirv = pi.vs_spirv;
                                    idesc.graphics.fragment_spirv = pi.ps_spirv;
                                    idesc.graphics.vertex_entry = pi.vs_entry;
                                    idesc.graphics.fragment_entry = pi.ps_entry;
                                    idesc.graphics.width = w;
                                    idesc.graphics.height = h;
                                    idesc.graphics.color_format = pi.color_format;
                                    idesc.graphics.enable_depth = pi.enable_depth;
                                    idesc.graphics.depth_write = pi.depth_write;
                                    if (op.has_clear_color) {
                                        std::memcpy(idesc.graphics.clear_color, op.clear_color, sizeof(op.clear_color));
                                    }
                                    idesc.graphics.clear_depth = op.clear_depth;
                                    if (op.has_vbv && op.vbv.BufferLocation != 0ull && op.vbv.SizeInBytes > 0u) {
                                        uint64_t vb_off = 0ull;
                                        ID3D12Resource* vb_res =
                                            device_x_->ResolveVulkanBufferByGpuVa(op.vbv.BufferLocation, &vb_off);
                                        if (vb_res) {
                                            device_x_->ReadVulkanBufferData(
                                                vb_res, vb_off, op.vbv.SizeInBytes, idesc.graphics.vertex_bytes);
                                        }
                                        idesc.graphics.vertex_stride =
                                            (op.vbv.StrideInBytes > 0u) ? op.vbv.StrideInBytes : pi.vertex_stride_hint;
                                        idesc.graphics.vertex_attributes = pi.vertex_attrs;
                                    }
                                    if (is_indexed && op.has_ibv && op.ibv.BufferLocation != 0ull && op.ibv.SizeInBytes > 0u) {
                                        uint64_t ib_off = 0ull;
                                        ID3D12Resource* ib_res =
                                            device_x_->ResolveVulkanBufferByGpuVa(op.ibv.BufferLocation, &ib_off);
                                        std::vector<uint8_t> raw_ib;
                                        if (ib_res && device_x_->ReadVulkanBufferData(ib_res, ib_off, op.ibv.SizeInBytes, raw_ib)) {
                                            if (op.ibv.Format == DXGI_FORMAT_R16_UINT) {
                                                const size_t cnt = raw_ib.size() / 2u;
                                                idesc.graphics.indices.resize(cnt);
                                                for (size_t k = 0; k < cnt; ++k) {
                                                    uint16_t v16 = 0u;
                                                    std::memcpy(&v16, raw_ib.data() + k * 2u, 2u);
                                                    idesc.graphics.indices[k] = static_cast<uint32_t>(v16);
                                                }
                                            } else {
                                                const size_t cnt = raw_ib.size() / 4u;
                                                idesc.graphics.indices.resize(cnt);
                                                std::memcpy(idesc.graphics.indices.data(), raw_ib.data(), cnt * 4u);
                                            }
                                        }
                                    }
                                    if (!op.root_constants.empty()) {
                                        idesc.graphics.push_constants.resize(op.root_constants.size() * 4u);
                                        std::memcpy(idesc.graphics.push_constants.data(), op.root_constants.data(),
                                                    idesc.graphics.push_constants.size());
                                    }
                                }
                                openscarlett::VulkanIndirectDrawResult ires{};
                                if (vk->ExecuteIndirectGraphicsDraw(idesc, ires).ok()) {
                                    if (is_mesh) ++spirv_mesh_cnt;
                                    else ++spirv_draws_cnt;
                                    if (ires.executed_on_vulkan_icd) ++icd_execs_cnt;
                                    if (op.rtv_resource) {
                                        device_x_->StoreVulkanRenderTargetFramebuffer(op.rtv_resource, ires.framebuffer);
                                    }
                                }
                            }
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
    ID3D12Fence* host_fence = UnwrapFence(pFence);
    const HRESULT hr = host_queue_->Signal(host_fence, Value);
    if (SUCCEEDED(hr) && pFence && IsKnownProxyPointer(pFence)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pFence->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::Fence) {
                auto* fp = static_cast<FenceXProxy*>(pFence);
                fp->NotifyGpuSignal(Value);
                if (fp->IsCpuFencePtrQueried() && fence_dispatcher_ && host_fence &&
                    host_fence->GetCompletedValue() < Value) {
                    (void)fence_dispatcher_->RegisterCallback(
                        pFence,
                        Value,
                        [](ID3D12Fence* uf, UINT64 target_val, HRESULT status, void*) {
                            if (SUCCEEDED(status) && uf) {
                                static_cast<FenceXProxy*>(uf)->AdvanceCpuFenceWord(target_val);
                            }
                        },
                        nullptr,
                        nullptr);
                }
            }
            insp->Release();
        }
    }
    if (device_x_ && device_x_->HasMirroredTitleVaRegions()) {
        std::lock_guard<std::mutex> lock(present_mutex_);
        PumpMirrorReadbackCompletionLocked();
    }
    RecordQueue("ID3D12CommandQueueX::Signal", 14, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Wait(ID3D12Fence* pFence, UINT64 Value) {
    const HRESULT hr = host_queue_->Wait(UnwrapFence(pFence), Value);
    if (device_x_ && device_x_->HasMirroredTitleVaRegions()) {
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
    if (hwnd && IsWindow(hwnd) != FALSE) {
        SetClassLongPtrW(
            hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
    }
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
    TryRegisterRawMouseForWindow(hwnd, *window_state_);
    if (IsWindowVisible(hwnd) != FALSE &&
        BroadcastWindowEventToGameInput(hwnd, kGiWinEventTargetHwnd, 0, 0)) {
        window_state_->gi_target_hwnd_sent.store(true, std::memory_order_release);
    }
    if (IsBackgroundKeepActiveEnabled()) {
        window_state_->is_active.store(true, std::memory_order_release);
        BroadcastWindowEventToGameInput(hwnd, kGiWinEventFocus, 1, 0);
        PostMessageW(hwnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageW(hwnd, WM_SETFOCUS, 0, 0);
    }
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
        if (pacing_timer_) {
            pacing_timer_high_res_ = true;
        } else {
            pacing_timer_high_res_ = false;
            pacing_timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }

    if (present_alloc_ && present_cmd_ && present_fence_ && present_fence_event_ &&
        present_ring_allocs_[kPresentRingSlots - 1u] && present_ring_cmds_[kPresentRingSlots - 1u] &&
        rb_ring_allocs_[kReadbackRingSlots - 1u] && rb_ring_cmds_[kReadbackRingSlots - 1u]) {
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
    for (uint32_t i = 0u; i < kPresentRingSlots; ++i) {
        if (!present_ring_allocs_[i]) {
            HRESULT hr = dev->CreateCommandAllocator(list_type, IID_PPV_ARGS(&present_ring_allocs_[i]));
            if (FAILED(hr)) return hr;
        }
        if (!present_ring_cmds_[i]) {
            HRESULT hr = dev->CreateCommandList(
                0, list_type, present_ring_allocs_[i], nullptr, IID_PPV_ARGS(&present_ring_cmds_[i]));
            if (FAILED(hr)) return hr;
            present_ring_cmds_[i]->Close();
        }
    }
    for (uint32_t i = 0u; i < kReadbackRingSlots; ++i) {
        if (!rb_ring_allocs_[i]) {
            HRESULT hr = dev->CreateCommandAllocator(list_type, IID_PPV_ARGS(&rb_ring_allocs_[i]));
            if (FAILED(hr)) return hr;
        }
        if (!rb_ring_cmds_[i]) {
            HRESULT hr = dev->CreateCommandList(
                0, list_type, rb_ring_allocs_[i], nullptr, IID_PPV_ARGS(&rb_ring_cmds_[i]));
            if (FAILED(hr)) return hr;
            rb_ring_cmds_[i]->Close();
        }
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
    // Always triple-buffer FLIP_DISCARD swapchains (3 backbuffers) to absorb scene-shift
    // frame-time spikes smoothly without single-frame pipeline bubbles.
    const uint32_t req_buffer_count = 3u;

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
        const int64_t spin_margin_us = pacing_timer_high_res_ ? 400LL : 1500LL;
        if (rem_us > spin_margin_us + 250LL) {
            const int64_t sleep_us = rem_us - spin_margin_us;
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

    // Resync cleanly whenever a heavy frame overran target_qpc so we never rush the next
    // frame into a bursty double-present judder spike.
    last_paced_qpc_ = (cur_qpc > target_qpc) ? cur_qpc : target_qpc;
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

void CommandQueueX::ReleaseCompositeObjectsLocked() {
    if (composite_rt_) { composite_rt_->Release(); composite_rt_ = nullptr; }
    if (composite_srv_heap_) { composite_srv_heap_->Release(); composite_srv_heap_ = nullptr; }
    if (composite_rtv_heap_) { composite_rtv_heap_->Release(); composite_rtv_heap_ = nullptr; }
    if (composite_pso_) { composite_pso_->Release(); composite_pso_ = nullptr; }
    if (composite_rs_) { composite_rs_->Release(); composite_rs_ = nullptr; }
    composite_pso_rt_format_ = DXGI_FORMAT_UNKNOWN;
    composite_rt_width_ = 0u;
    composite_rt_height_ = 0u;
    composite_rt_format_ = DXGI_FORMAT_UNKNOWN;
}

HRESULT CommandQueueX::EnsureCompositeObjectsLocked(
    uint32_t width, uint32_t height, DXGI_FORMAT rt_format) {
    if (composite_compile_failed_ || !device_x_ || !device_x_->HostDevice() ||
        queue_type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || width == 0u || height == 0u) {
        return E_FAIL;
    }
    ID3D12Device* dev = device_x_->HostDevice();

    if (!composite_rs_) {
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 2u;
        srv_range.BaseShaderRegister = 0u;
        srv_range.RegisterSpace = 0u;
        srv_range.OffsetInDescriptorsFromTableStart = 0u;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1u;
        params[0].DescriptorTable.pDescriptorRanges = &srv_range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0u;
        params[1].Constants.RegisterSpace = 0u;
        params[1].Constants.Num32BitValues = 12u;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxAnisotropy = 1u;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0u;
        samp.RegisterSpace = 0u;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 2u;
        rs_desc.pParameters = params;
        rs_desc.NumStaticSamplers = 1u;
        rs_desc.pStaticSamplers = &samp;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        using PfnSerialize = HRESULT(WINAPI*)(
            const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        auto ser_fn = reinterpret_cast<PfnSerialize>(
            openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        if (!ser_fn) {
            composite_compile_failed_ = true;
            return E_FAIL;
        }
        ID3DBlob* rs_blob = nullptr;
        HRESULT hr = ser_fn(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr);
        if (FAILED(hr) || !rs_blob) {
            composite_compile_failed_ = true;
            return FAILED(hr) ? hr : E_FAIL;
        }
        hr = dev->CreateRootSignature(
            0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&composite_rs_));
        rs_blob->Release();
        if (FAILED(hr) || !composite_rs_) {
            composite_compile_failed_ = true;
            return FAILED(hr) ? hr : E_FAIL;
        }
    }

    if (!composite_pso_ || composite_pso_rt_format_ != rt_format) {
        static std::mutex s_dxil_mu;
        static std::vector<uint8_t> s_vs_dxil;
        static std::vector<uint8_t> s_ps_dxil;
        static bool s_compiled = false;
        static bool s_failed = false;
        {
            std::lock_guard<std::mutex> lk(s_dxil_mu);
            if (!s_compiled && !s_failed) {
                static constexpr const char* kCompHlsl = R"(
Texture2D<float4> Plane0Tex : register(t0);
Texture2D<float4> Plane1Tex : register(t1);
SamplerState LinearClamp : register(s0);
cbuffer CompParams : register(b0) {
    float4 DstUvRect;
    float4 SrcUvRect;
    uint4 Flags;
};
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
float4 PSMain(VSOut i) : SV_Target0 {
    float4 base = Plane0Tex.SampleLevel(LinearClamp, i.uv, 0.0);
    if (Flags.x != 0u) {
        float2 rel = (i.uv - DstUvRect.xy) * DstUvRect.zw;
        if (all(rel >= 0.0) && all(rel <= 1.0)) {
            float2 uv1 = SrcUvRect.xy + rel * SrcUvRect.zw;
            float4 over = Plane1Tex.SampleLevel(LinearClamp, uv1, 0.0);
            float a = saturate(over.a);
            float3 over_premul = float3(0.0, 0.0, 0.0);
            if (a > (0.5 / 255.0)) {
                if (Flags.y == 1u) {
                    over_premul = over.rgb * a;
                } else if (Flags.y == 2u) {
                    over_premul = over.rgb;
                } else {
                    float max_c = max(over.r, max(over.g, over.b));
                    over_premul = (max_c > a + (1.0 / 255.0)) ? (over.rgb * a) : over.rgb;
                }
            }
            base.rgb = over_premul + base.rgb * (1.0 - a);
        }
    }
    if (Flags.z != 0u) {
        float3 c = saturate(base.rgb);
        float3 lin = (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
        base.rgb = pow(max(lin, 0.0), 1.0 / 2.2);
    }
    base.a = 1.0;
    return base;
}
)";
                std::string diag;
                auto vst = openscarlett::CompileHlslInProcessDxc(
                    kCompHlsl, "vs_6_0", "VSMain", s_vs_dxil, diag);
                if (!vst.ok() || s_vs_dxil.empty()) {
                    vst = openscarlett::CompileHlslWithDxc(
                        kCompHlsl, "vs_6_0", "VSMain", s_vs_dxil, diag);
                }
                auto pst = vst.ok()
                               ? openscarlett::CompileHlslInProcessDxc(
                                     kCompHlsl, "ps_6_0", "PSMain", s_ps_dxil, diag)
                               : vst;
                if (vst.ok() && (!pst.ok() || s_ps_dxil.empty())) {
                    pst = openscarlett::CompileHlslWithDxc(
                        kCompHlsl, "ps_6_0", "PSMain", s_ps_dxil, diag);
                }
                if ((!vst.ok() || !pst.ok() || s_vs_dxil.empty() || s_ps_dxil.empty())) {
                    ID3DBlob* vs_b = nullptr;
                    ID3DBlob* ps_b = nullptr;
                    const HRESULT vhr = D3DCompile(
                        kCompHlsl, std::strlen(kCompHlsl), "PresentXCompVS",
                        nullptr, nullptr, "VSMain", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &vs_b, nullptr);
                    const HRESULT phr_fx = SUCCEEDED(vhr)
                        ? D3DCompile(
                              kCompHlsl, std::strlen(kCompHlsl), "PresentXCompPS",
                              nullptr, nullptr, "PSMain", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &ps_b, nullptr)
                        : E_FAIL;
                    if (SUCCEEDED(vhr) && SUCCEEDED(phr_fx) && vs_b && ps_b) {
                        const auto* vp = static_cast<const uint8_t*>(vs_b->GetBufferPointer());
                        const auto* pp = static_cast<const uint8_t*>(ps_b->GetBufferPointer());
                        s_vs_dxil.assign(vp, vp + vs_b->GetBufferSize());
                        s_ps_dxil.assign(pp, pp + ps_b->GetBufferSize());
                    }
                    if (vs_b) vs_b->Release();
                    if (ps_b) ps_b->Release();
                }
                if (!s_vs_dxil.empty() && !s_ps_dxil.empty()) {
                    s_compiled = true;
                } else {
                    s_failed = true;
                }
            }
        }
        if (!s_compiled) {
            composite_compile_failed_ = true;
            return E_FAIL;
        }
        if (composite_pso_) {
            WaitForInternalFenceLocked();
            composite_pso_->Release();
            composite_pso_ = nullptr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = composite_rs_;
        pso_desc.VS = {s_vs_dxil.data(), s_vs_dxil.size()};
        pso_desc.PS = {s_ps_dxil.data(), s_ps_dxil.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1u;
        pso_desc.RTVFormats[0] = rt_format;
        pso_desc.SampleDesc.Count = 1u;
        const HRESULT phr = dev->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&composite_pso_));
        if (FAILED(phr) || !composite_pso_) {
            composite_compile_failed_ = true;
            return FAILED(phr) ? phr : E_FAIL;
        }
        composite_pso_rt_format_ = rt_format;
    }

    if (!composite_rtv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1u;
        const HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&composite_rtv_heap_));
        if (FAILED(hr)) return hr;
    }
    if (!composite_srv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = (kPresentRingSlots + 1u) * 2u;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&composite_srv_heap_));
        if (FAILED(hr)) return hr;
    }

    if (!composite_rt_ || composite_rt_width_ != width || composite_rt_height_ != height ||
        composite_rt_format_ != rt_format) {
        if (composite_rt_) {
            WaitForInternalFenceLocked();
            composite_rt_->Release();
            composite_rt_ = nullptr;
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = rt_format;
        rd.SampleDesc.Count = 1u;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const HRESULT hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&composite_rt_));
        if (FAILED(hr) || !composite_rt_) return FAILED(hr) ? hr : E_FAIL;
        dev->CreateRenderTargetView(
            composite_rt_, nullptr, composite_rtv_heap_->GetCPUDescriptorHandleForHeapStart());
        composite_rt_width_ = width;
        composite_rt_height_ = height;
        composite_rt_format_ = rt_format;
    }
    return S_OK;
}

void CommandQueueX::ReleasePresentObjectsLocked() {
    DetachWindowSubclassLocked();
    ReleaseSwapChainLocked();
    ReleaseCompositeObjectsLocked();
    if (tonemap_upload_buf_) { tonemap_upload_buf_->Release(); tonemap_upload_buf_ = nullptr; }
    if (readback_buf_) { readback_buf_->Release(); readback_buf_ = nullptr; }
    for (uint32_t i = 0u; i < kPresentRingSlots; ++i) {
        if (present_ring_cmds_[i]) { present_ring_cmds_[i]->Release(); present_ring_cmds_[i] = nullptr; }
        if (present_ring_allocs_[i]) { present_ring_allocs_[i]->Release(); present_ring_allocs_[i] = nullptr; }
        present_ring_fences_[i] = 0ull;
    }
    present_ring_idx_ = 0u;
    for (uint32_t i = 0u; i < kReadbackRingSlots; ++i) {
        if (rb_ring_cmds_[i]) { rb_ring_cmds_[i]->Release(); rb_ring_cmds_[i] = nullptr; }
        if (rb_ring_allocs_[i]) { rb_ring_allocs_[i]->Release(); rb_ring_allocs_[i] = nullptr; }
        rb_ring_fences_[i] = 0ull;
    }
    rb_ring_idx_ = 0u;
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
    D3D12_RESOURCE_STATES caller_state =
        (device_x_ && device_x_->IsRetailScarlettAbi())
            ? device_x_->GetRetailResourceState(src_tex, pPlaneParameters[0].ResourceState)
            : pPlaneParameters[0].ResourceState;
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        constexpr UINT kTexAllowed =
            static_cast<UINT>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_READ) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_DEST) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        caller_state = static_cast<D3D12_RESOURCE_STATES>(static_cast<UINT>(caller_state) & kTexAllowed);
    }

    // Detect optional Xbox Plane 1 hardware UI/video overlay (used by RAGE9 / Series S|X multi-plane titles).
    // Retail Scarlett D3D12XBOX_PRESENT_PLANE_PARAMETERS stores:
    //   +0x18: const D3D12_RECT* pSourceRect (pixel coordinates)
    //   +0x20: const D3D12XBOX_RECT_NORMALIZED* pDestRectNormalized (float [0..1] left, top, right, bottom)
    ID3D12Resource* overlay_tex = nullptr;
    D3D12_RESOURCE_DESC overlay_desc{};
    D3D12_RESOURCE_STATES overlay_caller_state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RECT overlay_src_rect{};
    float overlay_dst_norm[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bool has_overlay_dst_norm = false;
    if (PlaneCount >= 2u &&
        pPlaneParameters[1].ResourceCount > 0u &&
        pPlaneParameters[1].ppResources &&
        pPlaneParameters[1].ppResources[0]) {
        ID3D12Resource* cand = UnwrapResource(pPlaneParameters[1].ppResources[0]);
        if (cand && cand != src_tex) {
            const D3D12_RESOURCE_DESC cd = cand->GetDesc();
            if (cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                cd.Width > 0u && cd.Height > 0u) {
                overlay_tex = cand;
                overlay_desc = cd;
                if (device_x_ && device_x_->IsRetailScarlettAbi()) {
                    const auto* raw_bytes = reinterpret_cast<const uint8_t*>(&pPlaneParameters[1]);
                    const D3D12_RECT* psr = nullptr;
                    const float* pdr_f = nullptr;
                    std::memcpy(&psr, raw_bytes + 0x18u, sizeof(psr));
                    std::memcpy(&pdr_f, raw_bytes + 0x20u, sizeof(pdr_f));
                    auto is_valid_16b = [](const void* p) noexcept -> bool {
                        if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000u) return false;
                        MEMORY_BASIC_INFORMATION mbi{};
                        return ::VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                               mbi.State == MEM_COMMIT &&
                               (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0u &&
                               reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize >=
                                   reinterpret_cast<uintptr_t>(p) + 16u;
                    };
                    if (is_valid_16b(psr)) {
                        std::memcpy(&overlay_src_rect, psr, sizeof(D3D12_RECT));
                    }
                    if (is_valid_16b(pdr_f)) {
                        float tmp[4]{};
                        std::memcpy(tmp, pdr_f, sizeof(tmp));
                        if (tmp[2] > tmp[0] && tmp[3] > tmp[1] &&
                            tmp[0] >= -4.0f && tmp[2] <= 4.0f &&
                            tmp[1] >= -4.0f && tmp[3] <= 4.0f) {
                            std::memcpy(overlay_dst_norm, tmp, sizeof(tmp));
                            has_overlay_dst_norm = true;
                        }
                    }
                } else {
                    overlay_src_rect = pPlaneParameters[1].SourceRect;
                    const auto& dr = pPlaneParameters[1].DestRect;
                    if (dr.right > dr.left && dr.bottom > dr.top) {
                        const float ref_w = (static_cast<uint32_t>(dr.right) > width)
                                                ? static_cast<float>(dr.right)
                                                : static_cast<float>(width);
                        const float ref_h = (static_cast<uint32_t>(dr.bottom) > height)
                                                ? static_cast<float>(dr.bottom)
                                                : static_cast<float>(height);
                        overlay_dst_norm[0] = static_cast<float>(dr.left) / ref_w;
                        overlay_dst_norm[1] = static_cast<float>(dr.top) / ref_h;
                        overlay_dst_norm[2] = static_cast<float>(dr.right) / ref_w;
                        overlay_dst_norm[3] = static_cast<float>(dr.bottom) / ref_h;
                        has_overlay_dst_norm = true;
                    }
                }
                overlay_caller_state =
                    (device_x_ && device_x_->IsRetailScarlettAbi())
                        ? device_x_->GetRetailResourceState(cand, D3D12_RESOURCE_STATE_COMMON)
                        : pPlaneParameters[1].ResourceState;
                if (device_x_ && device_x_->IsRetailScarlettAbi()) {
                    constexpr UINT kTexAllowed =
                        static_cast<UINT>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_READ) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_DEST) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                    overlay_caller_state = static_cast<D3D12_RESOURCE_STATES>(
                        static_cast<UINT>(overlay_caller_state) & kTexAllowed);
                }
                static std::atomic<uint32_t> s_mp_logs{0u};
                if (s_mp_logs.fetch_add(1u, std::memory_order_relaxed) < 5u) {
                    std::fprintf(
                        stderr,
                        "[OpenScarlett][d3d12_xs] PresentX Multi-Plane: P0=%ux%u(fmt=%d st=0x%X) + "
                        "P1=%llux%u(fmt=%d st=0x%X src=[%ld,%ld..%ld,%ld] dst=[%.3f,%.3f..%.3f,%.3f])\n",
                        width, height, static_cast<int>(src_desc.Format), static_cast<unsigned>(caller_state),
                        static_cast<unsigned long long>(overlay_desc.Width), overlay_desc.Height,
                        static_cast<int>(overlay_desc.Format), static_cast<unsigned>(overlay_caller_state),
                        overlay_src_rect.left, overlay_src_rect.top, overlay_src_rect.right, overlay_src_rect.bottom,
                        overlay_dst_norm[0], overlay_dst_norm[1], overlay_dst_norm[2], overlay_dst_norm[3]);
                    std::fflush(stderr);
                }
            }
        }
    }

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
    const std::string diag_capture_dir_path = ReadEnvStringA("OPENSCARLETT_DIAG_CAPTURE_DIR");
    const std::string legacy_capture_dir_path = ReadEnvStringA("OPENSCARLETT_CAPTURE_DIR");
    const std::string& capture_dir_path =
        !diag_capture_dir_path.empty() ? diag_capture_dir_path : legacy_capture_dir_path;
    const uint64_t explicit_capture_interval_ms = ReadEnvUint64A("OPENSCARLETT_CAPTURE_INTERVAL_MS");
    const uint64_t capture_interval_ms =
        (explicit_capture_interval_ms > 0ull) ? explicit_capture_interval_ms : 1000ull;
    static uint64_t s_cap_t0_ms = 0ull;
    static uint64_t s_cap_last_ms = 0ull;
    static uint64_t s_cap_last_numbered_ms = 0ull;
    static uint64_t s_cap_last_frame = 0ull;
    static uint32_t s_cap_seq = 0u;
    static uint32_t s_cap_numbered_seq = 0u;
    const uint64_t now_cap_ms = GetTickCount64();
    if (s_cap_t0_ms == 0ull) s_cap_t0_ms = now_cap_ms;
    const bool want_periodic_capture =
        !capture_dir_path.empty() &&
        (s_cap_last_ms == 0ull || (now_cap_ms - s_cap_last_ms) >= capture_interval_ms);
    const uint64_t exit_after_frames = ReadEnvUint64A("OPENSCARLETT_PRESENTX_EXIT_AFTER_FRAMES");
    const uint64_t this_non_blank_frame = non_blank_present_count_ + 1ull;
    const bool want_capture_this_frame =
        want_periodic_capture ||
        (!capture_png_path.empty() &&
         ((exit_after_frames > 0ull && this_non_blank_frame == exit_after_frames) ||
          (exit_after_frames == 0ull &&
           (this_non_blank_frame == 1ull || (this_non_blank_frame % 25ull) == 0ull))));

    HWND target_hwnd = ResolveTargetOutputWindowLocked(pPresentParameters, force_headless);
    const bool use_swapchain = (target_hwnd != nullptr && IsWindow(target_hwnd) != FALSE);

    const bool vk_routing_active =
        device_x_ && device_x_->IsVulkanRoutingActive() &&
        device_x_->GetVulkanBackend() && device_x_->GetVulkanBackend()->IsInitialized();
    openscarlett::FrameBufferRgba8 vk_direct_rt_fb{};
    const bool has_vk_direct_rt_fb =
        vk_routing_active && device_x_->TryGetVulkanRenderTargetFramebuffer(src_tex, vk_direct_rt_fb);

    uint32_t out_width = width;
    uint32_t out_height = height;
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        static const std::pair<uint32_t, uint32_t> s_disp_res = []() -> std::pair<uint32_t, uint32_t> {
            const std::string s = ReadEnvStringA("OPENSCARLETT_DISPLAY_RES");
            if (!s.empty()) {
                char* end1 = nullptr;
                const unsigned long rw = std::strtoul(s.c_str(), &end1, 10);
                if (end1 && (*end1 == 'x' || *end1 == 'X')) {
                    char* end2 = nullptr;
                    const unsigned long rh = std::strtoul(end1 + 1, &end2, 10);
                    if (rw >= 64ul && rw <= 7680ul && rh >= 64ul && rh <= 4320ul) {
                        return {static_cast<uint32_t>(rw), static_cast<uint32_t>(rh)};
                    }
                }
            }
            return {0u, 0u};
        }();
        if (s_disp_res.first > 0u && s_disp_res.second > 0u) {
            out_width = s_disp_res.first;
            out_height = s_disp_res.second;
        }
    }

    const DXGI_FORMAT comp_rt_fmt = NormalizeSwapChainFormat(src_desc.Format);
    const bool need_gpu_scale = (out_width != width || out_height != height);
    const bool need_sdr_srgb_to_g22 =
        (device_x_ && device_x_->IsRetailScarlettAbi()) &&
        static_cast<DXGI_COLOR_SPACE_TYPE>(effective_cs) != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
        static_cast<DXGI_COLOR_SPACE_TYPE>(effective_cs) != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 &&
        (comp_rt_fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
         comp_rt_fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
         comp_rt_fmt == DXGI_FORMAT_R10G10B10A2_UNORM);
    const bool use_gpu_plane_composite =
        ((overlay_tex != nullptr) || need_gpu_scale || need_sdr_srgb_to_g22) && !vk_routing_active &&
        SUCCEEDED(EnsureCompositeObjectsLocked(out_width, out_height, comp_rt_fmt));
    if (!use_gpu_plane_composite) {
        out_width = width;
        out_height = height;
    }

    bool window_minimized = false;
    // Size the DXGI flip-model swapchain (DXGI_SCALING_STRETCH) to match the output plane
    // (out_width x out_height) so CopyTextureRegion fills 100% of the backbuffer.
    const uint32_t target_bb_width = out_width;
    const uint32_t target_bb_height = out_height;
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
    }

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
            const DWORD wait_ms = swapchain_occluded_ ? 16u : 50u;
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
    const bool want_sync_readback =
        (!device_x_ || !device_x_->IsRetailScarlettAbi()) && readback_capture_enabled_;
    const bool need_readback =
        !has_vk_direct_rt_fb && !skip_intermediate_readback &&
        (vk_routing_active || (!use_swapchain) || want_sync_readback || need_tonemap_bb_upload || want_capture_this_frame);

    const D3D12_RESOURCE_DESC active_copy_desc =
        (use_gpu_plane_composite && composite_rt_) ? composite_rt_->GetDesc() : src_desc;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size_bytes = 0;
    UINT64 total_bytes = 0;
    if (need_readback) {
        device_x_->HostDevice()->GetCopyableFootprints(
            &active_copy_desc, 0, 1, 0, &footprint, &num_rows, &row_size_bytes, &total_bytes);
        hr = EnsureReadbackBufferLocked(total_bytes);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
    }

    const bool async_swapchain_fast_path =
        direct_bb_copy_compatible && !need_readback && !need_tonemap_bb_upload && !vk_routing_active;
    uint32_t active_ring_slot = 0u;

    if (direct_bb_copy_compatible || need_readback) {
        ID3D12CommandAllocator* active_alloc = present_alloc_;
        ID3D12GraphicsCommandList* active_cmd = present_cmd_;
        if (async_swapchain_fast_path) {
            active_ring_slot = present_ring_idx_ % kPresentRingSlots;
            present_ring_idx_ = (present_ring_idx_ + 1u) % kPresentRingSlots;
            active_alloc = present_ring_allocs_[active_ring_slot];
            active_cmd = present_ring_cmds_[active_ring_slot];
            const uint64_t slot_wait_val = present_ring_fences_[active_ring_slot];
            if (slot_wait_val > 0ull && present_fence_->GetCompletedValue() < slot_wait_val) {
                if (SUCCEEDED(present_fence_->SetEventOnCompletion(slot_wait_val, present_fence_event_))) {
                    WaitForSingleObject(present_fence_event_, 15000u);
                }
            }
        } else if (present_fence_val_ > 0ull && present_fence_->GetCompletedValue() < present_fence_val_) {
            if (SUCCEEDED(present_fence_->SetEventOnCompletion(present_fence_val_, present_fence_event_))) {
                WaitForSingleObject(present_fence_event_, 15000u);
            }
        }

        hr = active_alloc->Reset();
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
        hr = active_cmd->Reset(active_alloc, nullptr);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }

        ID3D12Resource* copy_src_tex = src_tex;
        if (use_gpu_plane_composite && composite_rt_ && composite_pso_ && composite_rs_ &&
            composite_rtv_heap_ && composite_srv_heap_) {
            auto to_srv_fmt = [](DXGI_FORMAT f) noexcept -> DXGI_FORMAT {
                switch (f) {
                    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
                    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
                    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
                    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
                    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                    default: return f;
                }
            };
            const uint32_t srv_pair_idx = async_swapchain_fast_path ? active_ring_slot : kPresentRingSlots;
            const UINT srv_inc = device_x_->HostDevice()->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = composite_srv_heap_->GetCPUDescriptorHandleForHeapStart();
            srv_cpu.ptr += static_cast<SIZE_T>(srv_pair_idx * 2u) * srv_inc;
            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = composite_srv_heap_->GetGPUDescriptorHandleForHeapStart();
            srv_gpu.ptr += static_cast<UINT64>(srv_pair_idx * 2u) * srv_inc;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv0{};
            srv0.Format = to_srv_fmt(src_desc.Format);
            srv0.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv0.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv0.Texture2D.MipLevels = 1u;
            device_x_->HostDevice()->CreateShaderResourceView(src_tex, &srv0, srv_cpu);

            D3D12_CPU_DESCRIPTOR_HANDLE srv1_cpu = srv_cpu;
            srv1_cpu.ptr += srv_inc;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv1{};
            srv1.Format = to_srv_fmt(overlay_tex ? overlay_desc.Format : src_desc.Format);
            srv1.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv1.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv1.Texture2D.MipLevels = 1u;
            device_x_->HostDevice()->CreateShaderResourceView(
                overlay_tex ? overlay_tex : src_tex, &srv1, srv1_cpu);

            D3D12_RESOURCE_BARRIER pre_barriers[3]{};
            UINT num_pre = 0u;
            if (caller_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                auto& b = pre_barriers[num_pre++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = src_tex;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = caller_state;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            if (overlay_tex && overlay_caller_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                auto& b = pre_barriers[num_pre++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = overlay_tex;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = overlay_caller_state;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            {
                auto& b = pre_barriers[num_pre++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = composite_rt_;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            active_cmd->ResourceBarrier(num_pre, pre_barriers);

            float comp_consts[12]{};
            // DstUvRect: (minU, minV, invWidthU, invHeightV)
            comp_consts[0] = 0.0f;
            comp_consts[1] = 0.0f;
            comp_consts[2] = 1.0f;
            comp_consts[3] = 1.0f;
            if (overlay_tex) {
                if (has_overlay_dst_norm) {
                    const float du = overlay_dst_norm[2] - overlay_dst_norm[0];
                    const float dv = overlay_dst_norm[3] - overlay_dst_norm[1];
                    if (du > 0.0f && dv > 0.0f) {
                        comp_consts[0] = overlay_dst_norm[0];
                        comp_consts[1] = overlay_dst_norm[1];
                        comp_consts[2] = 1.0f / du;
                        comp_consts[3] = 1.0f / dv;
                    }
                }
                // SrcUvRect: (u0, v0, du, dv)
                comp_consts[4] = 0.0f;
                comp_consts[5] = 0.0f;
                comp_consts[6] = 1.0f;
                comp_consts[7] = 1.0f;
                const auto& sr = overlay_src_rect;
                if (sr.right > sr.left && sr.bottom > sr.top &&
                    static_cast<uint64_t>(sr.right) <= overlay_desc.Width &&
                    static_cast<uint32_t>(sr.bottom) <= overlay_desc.Height) {
                    comp_consts[4] = static_cast<float>(sr.left) / static_cast<float>(overlay_desc.Width);
                    comp_consts[5] = static_cast<float>(sr.top) / static_cast<float>(overlay_desc.Height);
                    comp_consts[6] = static_cast<float>(sr.right - sr.left) / static_cast<float>(overlay_desc.Width);
                    comp_consts[7] = static_cast<float>(sr.bottom - sr.top) / static_cast<float>(overlay_desc.Height);
                }
            }
            const uint32_t has_overlay_flag = (overlay_tex != nullptr) ? 1u : 0u;
            const uint32_t plane1_alpha_mode = [this]() -> uint32_t {
                std::string mode = ReadEnvStringA("OPENSCARLETT_PLANE1_ALPHA");
                for (char& ch : mode) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (mode == "auto" || mode == "0") return 0u;
                if (mode == "straight" || mode == "1") return 1u;
                if (mode == "premul" || mode == "premultiplied" || mode == "2") return 2u;
                // Xbox hardware DCN MPO composites Plane 1 using premultiplied alpha
                // Out.rgb = Plane1.rgb + Plane0.rgb * (1 - Plane1.a) (with a <= 0.5/255 -> 0).
                return (device_x_ && device_x_->IsRetailScarlettAbi()) ? 2u : 0u;
            }();
            const uint32_t sdr_srgb_g22_flag = need_sdr_srgb_to_g22 ? 1u : 0u;
            std::memcpy(&comp_consts[8], &has_overlay_flag, sizeof(uint32_t));
            std::memcpy(&comp_consts[9], &plane1_alpha_mode, sizeof(uint32_t));
            std::memcpy(&comp_consts[10], &sdr_srgb_g22_flag, sizeof(uint32_t));

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = composite_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(out_width), static_cast<float>(out_height), 0.0f, 1.0f};
            D3D12_RECT sc{0, 0, static_cast<LONG>(out_width), static_cast<LONG>(out_height)};
            ID3D12DescriptorHeap* heaps[] = {composite_srv_heap_};
            active_cmd->SetPipelineState(composite_pso_);
            active_cmd->SetGraphicsRootSignature(composite_rs_);
            active_cmd->SetDescriptorHeaps(1u, heaps);
            active_cmd->SetGraphicsRootDescriptorTable(0u, srv_gpu);
            active_cmd->SetGraphicsRoot32BitConstants(1u, 12u, comp_consts, 0u);
            active_cmd->RSSetViewports(1u, &vp);
            active_cmd->RSSetScissorRects(1u, &sc);
            active_cmd->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
            active_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            active_cmd->DrawInstanced(3u, 1u, 0u, 0u);

            D3D12_RESOURCE_BARRIER post_barriers[3]{};
            UINT num_post = 0u;
            {
                auto& b = post_barriers[num_post++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = composite_rt_;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            if (caller_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                auto& b = post_barriers[num_post++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = src_tex;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter = caller_state;
            }
            if (overlay_tex && overlay_caller_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                auto& b = post_barriers[num_post++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = overlay_tex;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter = overlay_caller_state;
            }
            active_cmd->ResourceBarrier(num_post, post_barriers);
            copy_src_tex = composite_rt_;
        } else if (caller_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            // Transition plane 0 to COPY_SOURCE if needed.
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = src_tex;
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = caller_state;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            active_cmd->ResourceBarrier(1, &to_copy);
        }

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = copy_src_tex;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        ID3D12Resource* backbuffer = nullptr;
        if (direct_bb_copy_compatible && swapchain_) {
            const UINT bb_idx = swapchain_->GetCurrentBackBufferIndex();
            hr = swapchain_->GetBuffer(bb_idx, IID_PPV_ARGS(&backbuffer));
            if (SUCCEEDED(hr) && backbuffer) {
                const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
                const uint32_t copy_w =
                    std::min<uint32_t>(out_width, static_cast<uint32_t>(bb_desc.Width));
                const uint32_t copy_h = std::min<uint32_t>(out_height, bb_desc.Height);

                D3D12_RESOURCE_BARRIER bb_to_dst{};
                bb_to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb_to_dst.Transition.pResource = backbuffer;
                bb_to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb_to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bb_to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                active_cmd->ResourceBarrier(1, &bb_to_dst);

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

                    active_cmd->CopyTextureRegion(&bb_dst, 0, 0, 0, &src_loc, &src_box);
                }

                D3D12_RESOURCE_BARRIER bb_to_present{};
                bb_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb_to_present.Transition.pResource = backbuffer;
                bb_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                active_cmd->ResourceBarrier(1, &bb_to_present);
            }
        }

        if (need_readback && readback_buf_) {
            D3D12_TEXTURE_COPY_LOCATION rb_dst{};
            rb_dst.pResource = readback_buf_;
            rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rb_dst.PlacedFootprint = footprint;
            active_cmd->CopyTextureRegion(&rb_dst, 0, 0, 0, &src_loc, nullptr);
        }

        // Restore plane 0 back to caller_state (when not already restored by composite pass).
        if (copy_src_tex == src_tex && caller_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER back_barrier{};
            back_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            back_barrier.Transition.pResource = src_tex;
            back_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            back_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            back_barrier.Transition.StateAfter = caller_state;
            active_cmd->ResourceBarrier(1, &back_barrier);
        }

        hr = active_cmd->Close();
        if (backbuffer) {
            backbuffer->Release();
            backbuffer = nullptr;
        }
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }

        ID3D12CommandList* cmd_lists[] = {active_cmd};
        host_queue_->ExecuteCommandLists(1, cmd_lists);
        if (device_x_ && device_x_->IsRetailScarlettAbi() && device_x_->HostDevice()) {
            const HRESULT prem = device_x_->HostDevice()->GetDeviceRemovedReason();
            if (FAILED(prem)) {
                static std::atomic<bool> s_pr_rem{false};
                if (!s_pr_rem.exchange(true)) {
                    std::fprintf(stderr,
                                 "[OpenScarlett][PRESENT-POST-REMOVED] rem=0x%08lX frame=%llu %ux%u fmt=%d st=0x%X\n",
                                 static_cast<unsigned long>(prem),
                                 static_cast<unsigned long long>(present_count_),
                                 width, height, static_cast<int>(src_desc.Format),
                                 static_cast<unsigned>(caller_state));
                    std::fflush(stderr);
                }
            }
        }
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
        const size_t total_packed = packed_row_bytes * out_height;
        last_present_raw_pixels_.resize(total_packed);
        last_present_raw_format_ = active_copy_desc.Format;

        const size_t rgba8_row_bytes = static_cast<size_t>(out_width) * 4u;
        last_present_pixels_.resize(rgba8_row_bytes * out_height);

        for (uint32_t y = 0; y < out_height; ++y) {
            const uint8_t* src_row =
                rb_bytes + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
            std::memcpy(
                last_present_raw_pixels_.data() + static_cast<size_t>(y) * packed_row_bytes,
                src_row,
                packed_row_bytes);
            uint8_t* dst_rgba8_row =
                last_present_pixels_.data() + static_cast<size_t>(y) * rgba8_row_bytes;
            ConvertPackedRowToSdrRgba8(
                active_copy_desc.Format,
                effective_cs,
                present_config_.ToneMapMode,
                present_config_.PaperWhiteNits,
                src_row,
                dst_rgba8_row,
                out_width);
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
            const uint32_t copy_w = std::min<uint32_t>(out_width, static_cast<uint32_t>(bb_desc.Width));
            const uint32_t copy_h = std::min<uint32_t>(out_height, bb_desc.Height);

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
                            last_present_pixels_.data() + static_cast<size_t>(y) * out_width * 4u;
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

    // Ensure Raw Input mouse, GameInput target HWND, & 3D hidden-cursor confinement stay synced.
    if (do_swapchain_present && target_hwnd && window_state_) {
        if (!window_state_->raw_mouse_registered.load(std::memory_order_acquire)) {
            TryRegisterRawMouseForWindow(target_hwnd, *window_state_);
        }
        if (!window_state_->gi_target_hwnd_sent.load(std::memory_order_acquire) &&
            IsWindowVisible(target_hwnd) != FALSE) {
            if (BroadcastWindowEventToGameInput(target_hwnd, kGiWinEventTargetHwnd, 0, 0)) {
                window_state_->gi_target_hwnd_sent.store(true, std::memory_order_release);
            }
        }
        if (async_swapchain_fast_path) {
            const bool focused =
                window_state_->is_active.load(std::memory_order_acquire) &&
                (GetForegroundWindow() == target_hwnd);
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            const bool cursor_hidden =
                (GetCursorInfo(&ci) != FALSE) && ((ci.flags & CURSOR_SHOWING) == 0u);
            const bool user_cursor_locked = IsGameInputCursorLocked();
            if (focused && (cursor_hidden || user_cursor_locked)) {
                RECT cr{};
                if (GetClientRect(target_hwnd, &cr) != FALSE && cr.right > cr.left && cr.bottom > cr.top) {
                    POINT tl{cr.left, cr.top};
                    POINT br{cr.right, cr.bottom};
                    ClientToScreen(target_hwnd, &tl);
                    ClientToScreen(target_hwnd, &br);
                    RECT sr{tl.x, tl.y, br.x, br.y};
                    if (ClipCursor(&sr) != FALSE) {
                        window_state_->cursor_clipped.store(true, std::memory_order_release);
                    }
                    if (user_cursor_locked && !cursor_hidden &&
                        window_state_->raw_mouse_packets_seen.load(std::memory_order_acquire)) {
                        SetCursorPos((sr.left + sr.right) / 2, (sr.top + sr.bottom) / 2);
                    }
                }
            } else if (window_state_->cursor_clipped.exchange(false, std::memory_order_acq_rel)) {
                ClipCursor(nullptr);
            }
        }
    }

    // Use native hardware DXGI VSync (SyncInterval=1) on interactive retail swapchains when
    // requested frame_interval_us matches the monitor's refresh cadence: eliminates DWM phase-beat
    // judder, drops dwm.exe compositor CPU dramatically, and removes software spin-wait overhead.
    bool use_hw_vsync_present = false;
    if (should_pace && async_swapchain_fast_path && do_swapchain_present && swapchain_ &&
        !fcap_override && !swapchain_occluded_) {
        static const bool s_force_sw_pacing = []() {
            char b[16]{};
            const DWORD n = GetEnvironmentVariableA("OPENSCARLETT_FORCE_SOFTWARE_PACING", b, sizeof(b));
            return (n > 0u && n < sizeof(b) && (b[0] == '1' || b[0] == 't' || b[0] == 'T'));
        }();
        if (!s_force_sw_pacing) {
            if (cached_monitor_hwnd_ != target_hwnd || cached_monitor_refresh_hz_ == 0u) {
                cached_monitor_refresh_hz_ = QueryWindowMonitorRefreshHz(target_hwnd);
                cached_monitor_hwnd_ = target_hwnd;
            }
            const uint32_t mon_interval_us =
                (cached_monitor_refresh_hz_ > 0u) ? (1000000u / cached_monitor_refresh_hz_) : 16667u;
            const uint32_t lo_us = (mon_interval_us * 88u) / 100u;
            const uint32_t hi_us = (mon_interval_us * 112u) / 100u;
            if (frame_interval_us >= lo_us && frame_interval_us <= hi_us) {
                use_hw_vsync_present = true;
            }
        }
    }

    if (should_pace && !use_hw_vsync_present) {
        PaceFrameLocked(frame_interval_us);
        ++paced_frame_count_;
    } else if (!use_hw_vsync_present) {
        const int64_t now_qpc = QueryQpcNow();
        if (last_present_qpc_ != 0 && qpc_freq_ > 0) {
            last_frame_ms_ = static_cast<double>(now_qpc - last_present_qpc_) * 1000.0 /
                             static_cast<double>(qpc_freq_);
        }
        last_present_qpc_ = now_qpc;
        last_paced_qpc_ = 0;
    }

    // Phase 1: GPU Fence Signaling & Synchronization (separated from DXGI/Vulkan presentation).
    // For interactive swapchain presents without readback, signal our ring fence asynchronously
    // WITHOUT draining the GPU to idle so CPU and GPU pipeline concurrently at full frame rate.
    int64_t fence_sig_qpc = 0;
    double fence_wait_us = 0.0;
    uint64_t fence_completed_val = 0ull;
    if (async_swapchain_fast_path) {
        const uint64_t slot_val = ++present_fence_val_;
        fence_sig_qpc = QueryQpcNow();
        hr = host_queue_->Signal(present_fence_, slot_val);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
        present_ring_fences_[active_ring_slot] = slot_val;
        fence_completed_val = present_fence_->GetCompletedValue();
        if (device_x_ && device_x_->HasMirroredTitleVaRegions()) {
            (void)device_x_->GetTitleVaMirror().CompleteReadbacksOnFence(fence_completed_val);
        }
    } else {
        hr = WaitForInternalFenceWithTimingLocked(&fence_sig_qpc, &fence_wait_us, &fence_completed_val);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
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
        const UINT sync_interval = use_hw_vsync_present ? 1u : 0u;
        const UINT present_flags =
            (!use_hw_vsync_present && ((swapchain_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0))
                ? DXGI_PRESENT_ALLOW_TEARING
                : 0u;
        present_submit_qpc = QueryQpcNow();
        hr = swapchain_->Present(sync_interval, present_flags);
        if (FAILED(hr) && (present_flags != 0u || sync_interval != 0u)) {
            hr = swapchain_->Present(0u, 0u);
        }
        present_complete_qpc = QueryQpcNow();
        if (use_hw_vsync_present) {
            if (last_present_qpc_ != 0 && qpc_freq_ > 0) {
                last_frame_ms_ = static_cast<double>(present_complete_qpc - last_present_qpc_) * 1000.0 /
                                 static_cast<double>(qpc_freq_);
            }
            last_present_qpc_ = present_complete_qpc;
            last_paced_qpc_ = 0;
            ++paced_frame_count_;
        }
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

    last_present_width_ = has_vk_direct_rt_fb ? vk_direct_rt_fb.width : out_width;
    last_present_height_ = has_vk_direct_rt_fb ? vk_direct_rt_fb.height : out_height;
    ++present_count_;
    ++non_blank_present_count_;
    if (device_x_ && device_x_->IsRetailScarlettAbi() && (present_count_ <= 8u || (present_count_ % 20u) == 0u)) {
        const uint32_t ec = g_prof_exec_cnt.exchange(0u, std::memory_order_relaxed);
        const int64_t eq = g_prof_exec_qpc.exchange(0, std::memory_order_relaxed);
        const int64_t sq = g_prof_sync_qpc.exchange(0, std::memory_order_relaxed);
        const uint32_t wc = g_prof_w32_cnt.exchange(0u, std::memory_order_relaxed);
        const int64_t wq = g_prof_w32_qpc.exchange(0, std::memory_order_relaxed);
        static const bool s_trace_prof = []() {
            char v[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        if (s_trace_prof && present_count_ <= 240u) {
            const double to_ms = (qpc_freq_ > 0) ? (1000.0 / static_cast<double>(qpc_freq_)) : 0.0;
            std::fprintf(stderr,
                         "[OpenScarlett][PROF] f=%llu exec=%u(%.2fms, sync=%.2fms) w32=%u(%.2fms) fint=%uus\n",
                         static_cast<unsigned long long>(present_count_), ec,
                         static_cast<double>(eq) * to_ms, static_cast<double>(sq) * to_ms,
                         wc, static_cast<double>(wq) * to_ms,
                         device_x_->FrameIntervalMicroseconds());
            std::fflush(stderr);
        }
    } else if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        g_prof_exec_cnt.store(0u, std::memory_order_relaxed);
        g_prof_exec_qpc.store(0, std::memory_order_relaxed);
        g_prof_sync_qpc.store(0, std::memory_order_relaxed);
        g_prof_w32_cnt.store(0u, std::memory_order_relaxed);
        g_prof_w32_qpc.store(0, std::memory_order_relaxed);
    }
    if (device_x_) {
        device_x_->GetQueryAccumulator().BeginFrame(present_count_);
        device_x_->OnPresentFrameComplete(present_count_);
    }

    if (want_capture_this_frame && !last_present_pixels_.empty() &&
        last_present_width_ > 0u && last_present_height_ > 0u) {
        if (!capture_png_path.empty()) {
            openscarlett::FrameBufferRgba8 fb{};
            try {
                fb.width = last_present_width_;
                fb.height = last_present_height_;
                fb.pixels.swap(last_present_pixels_);
                if (fb.WritePng(capture_png_path).ok()) {
                    capture_png_written_ = true;
                }
            } catch (const std::bad_alloc&) {
                // Skip diagnostic capture rather than crashing guest under tight host commit limits.
            }
            if (last_present_pixels_.empty() && !fb.pixels.empty()) {
                last_present_pixels_.swap(fb.pixels);
            }
        }
        if (want_periodic_capture) {
            const uint64_t dt_ms = (s_cap_last_ms > 0ull) ? std::max<uint64_t>(1ull, now_cap_ms - s_cap_last_ms) : 1000ull;
            const uint64_t df = (s_cap_last_frame > 0ull && present_count_ >= s_cap_last_frame)
                                    ? (present_count_ - s_cap_last_frame)
                                    : 1ull;
            const double fps_1s = static_cast<double>(df) * 1000.0 / static_cast<double>(dt_ms);
            s_cap_last_ms = now_cap_ms;
            s_cap_last_frame = present_count_;
            const uint32_t seq = ++s_cap_seq;
            const uint64_t elapsed_ms = now_cap_ms - s_cap_t0_ms;

            // Downsample to max 1280x720 for lightweight 1-second PNG capture (< 3.6 MiB).
            uint32_t out_w = last_present_width_;
            uint32_t out_h = last_present_height_;
            try {
                openscarlett::FrameBufferRgba8 out_fb{};
                if (out_w > 1280u || out_h > 720u) {
                    const double scale = std::min(1280.0 / static_cast<double>(out_w),
                                                  720.0 / static_cast<double>(out_h));
                    out_w = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(out_w * scale)));
                    out_h = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(out_h * scale)));
                    out_fb.width = out_w;
                    out_fb.height = out_h;
                    out_fb.pixels.resize(static_cast<size_t>(out_w) * out_h * 4u);
                    const uint8_t* src_px = last_present_pixels_.data();
                    uint8_t* dst_px = out_fb.pixels.data();
                    for (uint32_t y = 0u; y < out_h; ++y) {
                        const uint32_t sy = static_cast<uint32_t>(
                            (static_cast<uint64_t>(y) * last_present_height_) / out_h);
                        const uint8_t* srow = src_px + static_cast<size_t>(sy) * last_present_width_ * 4u;
                        uint8_t* drow = dst_px + static_cast<size_t>(y) * out_w * 4u;
                        for (uint32_t x = 0u; x < out_w; ++x) {
                            const uint32_t sx = static_cast<uint32_t>(
                                (static_cast<uint64_t>(x) * last_present_width_) / out_w);
                            std::memcpy(drow + static_cast<size_t>(x) * 4u,
                                        srow + static_cast<size_t>(sx) * 4u, 4u);
                        }
                    }
                } else {
                    out_fb.width = out_w;
                    out_fb.height = out_h;
                    out_fb.pixels = last_present_pixels_;
                }
                // Match Xbox HDMI / DXGI_ALPHA_MODE_IGNORE opaque scanout on Plane 0 diagnostic PNGs.
                for (size_t a_off = 3u; a_off < out_fb.pixels.size(); a_off += 4u) {
                    out_fb.pixels[a_off] = 255u;
                }
                (void)::CreateDirectoryA(capture_dir_path.c_str(), nullptr);
                static const uint32_t kMaxNumberedCaptureFrames = []() {
                    char mbuf[32]{};
                    if (::GetEnvironmentVariableA("OPENSCARLETT_CAPTURE_MAX_FRAMES", mbuf, sizeof(mbuf)) > 0) {
                        const unsigned long v = std::strtoul(mbuf, nullptr, 10);
                        if (v > 0ul && v <= 100000ul) return static_cast<uint32_t>(v);
                    }
                    return 2400u;
                }();
                if (explicit_capture_interval_ms > 0ull) {
                    if (s_cap_numbered_seq < kMaxNumberedCaptureFrames &&
                        (s_cap_last_numbered_ms == 0ull ||
                         (now_cap_ms - s_cap_last_numbered_ms) >= explicit_capture_interval_ms)) {
                        s_cap_last_numbered_ms = now_cap_ms;
                        const uint32_t fseq = ++s_cap_numbered_seq;
                        char png_name[MAX_PATH]{};
                        std::snprintf(png_name, sizeof(png_name), "%s\\frame_%04u_%llums.png",
                                      capture_dir_path.c_str(), fseq,
                                      static_cast<unsigned long long>(elapsed_ms));
                        (void)out_fb.WritePng(png_name);
                    }
                } else if (diag_capture_dir_path.empty() && seq <= kMaxNumberedCaptureFrames) {
                    char png_name[MAX_PATH]{};
                    std::snprintf(png_name, sizeof(png_name), "%s\\sec_%04u_%llums.png",
                                  capture_dir_path.c_str(), seq,
                                  static_cast<unsigned long long>(elapsed_ms));
                    (void)out_fb.WritePng(png_name);
                }
                const std::string latest_path = capture_dir_path + "\\latest.png";
                (void)out_fb.WritePng(latest_path);
            } catch (const std::bad_alloc&) {
            }

            // Capture live host RAM/commit, guest working set, and DXGI GPU VRAM/L0 usage.
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            (void)K32GetProcessMemoryInfo(GetCurrentProcess(),
                                          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            (void)GlobalMemoryStatusEx(&ms);
            uint64_t vram_used_mb = 0ull, vram_budget_mb = 0ull, l0_used_mb = 0ull;
            if (device_x_ && device_x_->HostDevice()) {
                const LUID luid = device_x_->HostDevice()->GetAdapterLuid();
                IDXGIFactory4* fac4 = nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac4))) && fac4) {
                    IDXGIAdapter3* ad3 = nullptr;
                    if (SUCCEEDED(fac4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad3))) && ad3) {
                        DXGI_QUERY_VIDEO_MEMORY_INFO local_info{};
                        DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal_info{};
                        if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info))) {
                            vram_used_mb = local_info.CurrentUsage >> 20u;
                            vram_budget_mb = local_info.Budget >> 20u;
                        }
                        if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal_info))) {
                            l0_used_mb = nonlocal_info.CurrentUsage >> 20u;
                        }
                        ad3->Release();
                    }
                    fac4->Release();
                }
            }
            const uint32_t lifted = device_x_ ? device_x_->LazyLiftCount() : 0u;
            const HRESULT rem_now = (device_x_ && device_x_->HostDevice())
                                        ? device_x_->HostDevice()->GetDeviceRemovedReason()
                                        : S_OK;
            const std::string tel_path = capture_dir_path + "\\telemetry_1s.jsonl";
            FILE* tf = nullptr;
            if (::fopen_s(&tf, tel_path.c_str(), "a") == 0 && tf) {
                std::fprintf(
                    tf,
                    "{\"sec\":%u,\"elapsed_ms\":%llu,\"frame\":%llu,\"fps\":%.2f,"
                    "\"src\":\"%ux%u\",\"cap\":\"%ux%u\","
                    "\"guest_ws_mb\":%llu,\"guest_commit_mb\":%llu,"
                    "\"host_free_ram_mb\":%llu,\"host_free_commit_mb\":%llu,\"mem_load_pct\":%lu,"
                    "\"gpu_vram_used_mb\":%llu,\"gpu_vram_budget_mb\":%llu,\"gpu_l0_used_mb\":%llu,"
                    "\"lifted_psos\":%u,\"removed_hr\":\"0x%08lX\"}\n",
                    seq, static_cast<unsigned long long>(elapsed_ms),
                    static_cast<unsigned long long>(present_count_), fps_1s,
                    last_present_width_, last_present_height_, out_w, out_h,
                    static_cast<unsigned long long>(pmc.WorkingSetSize >> 20u),
                    static_cast<unsigned long long>(pmc.PrivateUsage >> 20u),
                    static_cast<unsigned long long>(ms.ullAvailPhys >> 20u),
                    static_cast<unsigned long long>(ms.ullAvailPageFile >> 20u),
                    static_cast<unsigned long>(ms.dwMemoryLoad),
                    static_cast<unsigned long long>(vram_used_mb),
                    static_cast<unsigned long long>(vram_budget_mb),
                    static_cast<unsigned long long>(l0_used_mb),
                    lifted, static_cast<unsigned long>(rem_now));
                std::fclose(tf);
            }
        }
        // Diagnostic: if Scaleform/UI rendered into an offscreen RTV before blitting to the
        // swapchain plane, read back that offscreen UI RTV as well to isolate UI draw vs blit.
        ID3D12Resource* ui_rtv =
            (EnvFlagTrueA("OPENSCARLETT_CAPTURE_UI_RTV") && device_x_) ? device_x_->LastDiiRtv() : nullptr;
        if (ui_rtv && ui_rtv != src_tex && readback_buf_ && present_alloc_ && present_cmd_) {
            const D3D12_RESOURCE_DESC ud = ui_rtv->GetDesc();
            if (ud.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ud.Width == width && ud.Height == height) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT ufp{};
                UINT urows = 0u;
                UINT64 ursz = 0ull, utot = 0ull;
                device_x_->HostDevice()->GetCopyableFootprints(&ud, 0u, 1u, 0ull, &ufp, &urows, &ursz, &utot);
                if (utot <= readback_buf_->GetDesc().Width &&
                    SUCCEEDED(present_alloc_->Reset()) && SUCCEEDED(present_cmd_->Reset(present_alloc_, nullptr))) {
                    const D3D12_RESOURCE_STATES ui_before = device_x_->GetRetailResourceState(
                        ui_rtv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    D3D12_RESOURCE_BARRIER b0{};
                    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b0.Transition.pResource = ui_rtv;
                    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b0.Transition.StateBefore = ui_before;
                    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    if (ui_before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                        present_cmd_->ResourceBarrier(1u, &b0);
                    }
                    D3D12_TEXTURE_COPY_LOCATION ul_src{};
                    ul_src.pResource = ui_rtv;
                    ul_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_TEXTURE_COPY_LOCATION ul_dst{};
                    ul_dst.pResource = readback_buf_;
                    ul_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    ul_dst.PlacedFootprint = ufp;
                    present_cmd_->CopyTextureRegion(&ul_dst, 0u, 0u, 0u, &ul_src, nullptr);
                    if (ui_before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                        std::swap(b0.Transition.StateBefore, b0.Transition.StateAfter);
                        present_cmd_->ResourceBarrier(1u, &b0);
                    }
                    if (SUCCEEDED(present_cmd_->Close())) {
                        ID3D12CommandList* ul_lists[] = {present_cmd_};
                        host_queue_->ExecuteCommandLists(1u, ul_lists);
                        const uint64_t uv = ++present_fence_val_;
                        if (SUCCEEDED(host_queue_->Signal(present_fence_, uv))) {
                            if (present_fence_->GetCompletedValue() < uv) {
                                present_fence_->SetEventOnCompletion(uv, present_fence_event_);
                                WaitForSingleObject(present_fence_event_, 5000u);
                            }
                            void* umap = nullptr;
                            D3D12_RANGE ur{0u, static_cast<SIZE_T>(utot)};
                            if (SUCCEEDED(readback_buf_->Map(0u, &ur, &umap)) && umap) {
                                uint32_t max_r = 0, max_g = 0, max_b = 0, max_a = 0;
                                uint64_t nz_rgb = 0, nz_a = 0;
                                std::vector<uint8_t> u_rgba(static_cast<size_t>(width) * height * 4u);
                                const auto* srcp = static_cast<const uint8_t*>(umap);
                                for (uint32_t y = 0u; y < height; ++y) {
                                    const uint8_t* row = srcp + static_cast<size_t>(y) * ufp.Footprint.RowPitch;
                                    uint8_t* dstrow = u_rgba.data() + static_cast<size_t>(y) * width * 4u;
                                    std::memcpy(dstrow, row, static_cast<size_t>(width) * 4u);
                                    for (uint32_t x = 0u; x < width; ++x) {
                                        const uint8_t r = row[x * 4u + 0u];
                                        const uint8_t g = row[x * 4u + 1u];
                                        const uint8_t b = row[x * 4u + 2u];
                                        const uint8_t a = row[x * 4u + 3u];
                                        if (r > max_r) max_r = r;
                                        if (g > max_g) max_g = g;
                                        if (b > max_b) max_b = b;
                                        if (a > max_a) max_a = a;
                                        if (r | g | b) ++nz_rgb;
                                        if (a) ++nz_a;
                                    }
                                }
                                D3D12_RANGE zw{0u, 0u};
                                readback_buf_->Unmap(0u, &zw);
                                std::fprintf(stderr,
                                             "[OpenScarlett][UI-RTV] res=%p %ux%u fmt=%d max_rgba=(%u,%u,%u,%u) nz_rgb=%llu nz_a=%llu\n",
                                             static_cast<void*>(ui_rtv), width, height, static_cast<int>(ud.Format),
                                             max_r, max_g, max_b, max_a,
                                             static_cast<unsigned long long>(nz_rgb),
                                             static_cast<unsigned long long>(nz_a));
                                std::fflush(stderr);
                                openscarlett::FrameBufferRgba8 ufb{};
                                ufb.width = width;
                                ufb.height = height;
                                ufb.pixels = std::move(u_rgba);
                                (void)ufb.WritePng(capture_png_path + ".ui.png");
                            }
                        }
                    }
                }
            }
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
    struct W32ProfGuard {
        int64_t t0;
        ~W32ProfGuard() {
            g_prof_w32_cnt.fetch_add(1u, std::memory_order_relaxed);
            g_prof_w32_qpc.fetch_add(QueryQpcNow() - t0, std::memory_order_relaxed);
        }
    } w32_prof{QueryQpcNow()};
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
            if (fence_dispatcher_) {
                (void)fence_dispatcher_->PumpReadyCallbacks();
            }
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
            if (fence_dispatcher_) {
                (void)fence_dispatcher_->PumpReadyCallbacks();
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
            if (fence_dispatcher_) {
                (void)fence_dispatcher_->PumpReadyCallbacks();
            }
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
            if (fence_dispatcher_) {
                (void)fence_dispatcher_->PumpReadyCallbacks();
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
