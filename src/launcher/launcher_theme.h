#pragma once

// OpenScarlett Launcher — Dark Native Win32 + Direct2D + DirectWrite Theme System
// MIT C++20 implementation. Zero third-party GUI dependencies.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace openscarlett::launcher::theme {

// ============================================================================
// Dark Xbox / GitHub-Dark Inspired Palette
// ============================================================================
namespace colors {

// COLORREF format: 0x00BBGGRR via RGB(r, g, b)
inline constexpr COLORREF kWindowBg        = RGB(0x0D, 0x11, 0x17);  // #0D1117
inline constexpr COLORREF kHeaderBg        = RGB(0x11, 0x16, 0x1E);  // #11161E
inline constexpr COLORREF kSurfaceCard     = RGB(0x16, 0x1B, 0x22);  // #161B22
inline constexpr COLORREF kControlBg       = RGB(0x21, 0x26, 0x2D);  // #21262D
inline constexpr COLORREF kControlHover    = RGB(0x2D, 0x33, 0x3B);  // #2D333B
inline constexpr COLORREF kControlPressed  = RGB(0x1A, 0x1F, 0x26);  // #1A1F26
inline constexpr COLORREF kInputBg         = RGB(0x0D, 0x11, 0x17);  // #0D1117
inline constexpr COLORREF kBorderSubtle    = RGB(0x30, 0x36, 0x3D);  // #30363D
inline constexpr COLORREF kBorderActive    = RGB(0x58, 0xA6, 0xFF);  // #58A6FF

inline constexpr COLORREF kXboxGreen       = RGB(0x10, 0x7C, 0x10);  // #107C10
inline constexpr COLORREF kEmeraldAccent   = RGB(0x2E, 0xA0, 0x43);  // #2EA043
inline constexpr COLORREF kEmeraldHover    = RGB(0x3F, 0xB9, 0x50);  // #3FB950
inline constexpr COLORREF kEmeraldPressed  = RGB(0x19, 0x6C, 0x2E);  // #196C2E

inline constexpr COLORREF kTelemetryBlue   = RGB(0x58, 0xA6, 0xFF);  // #58A6FF
inline constexpr COLORREF kWarningAmber    = RGB(0xD2, 0x99, 0x22);  // #D29922
inline constexpr COLORREF kFailRed         = RGB(0xF8, 0x51, 0x49);  // #F85149
inline constexpr COLORREF kDangerHover     = RGB(0xDA, 0x36, 0x33);  // #DA3633

inline constexpr COLORREF kTextPrimary     = RGB(0xE6, 0xED, 0xF3);  // #E6EDF3
inline constexpr COLORREF kTextSecondary   = RGB(0x8B, 0x94, 0x9E);  // #8B949E
inline constexpr COLORREF kTextDisabled    = RGB(0x48, 0x4F, 0x58);  // #484F58

// Soft badge background tints
inline constexpr COLORREF kBadgeGreenBg    = RGB(0x12, 0x2B, 0x1A);
inline constexpr COLORREF kBadgeBlueBg     = RGB(0x13, 0x23, 0x3A);
inline constexpr COLORREF kBadgeAmberBg    = RGB(0x2F, 0x24, 0x11);
inline constexpr COLORREF kBadgeRedBg      = RGB(0x34, 0x17, 0x1A);
inline constexpr COLORREF kBadgeMutedBg    = RGB(0x21, 0x26, 0x2D);

inline D2D1_COLOR_F ToD2D(COLORREF c, float alpha = 1.0f) noexcept {
    D2D1_COLOR_F out{};
    out.r = static_cast<float>(GetRValue(c)) / 255.0f;
    out.g = static_cast<float>(GetGValue(c)) / 255.0f;
    out.b = static_cast<float>(GetBValue(c)) / 255.0f;
    out.a = alpha;
    return out;
}

}  // namespace colors

// ============================================================================
// Portable COM Smart Pointer (MSVC, Clang-CL, MinGW-w64 clean)
// ============================================================================
template <typename T>
class ComRef {
public:
    ComRef() noexcept = default;
    ~ComRef() noexcept { Reset(); }

    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    ComRef(ComRef&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = p;
    }

private:
    T* ptr_ = nullptr;
};

// ============================================================================
// UTF-8 <-> UTF-16 Conversion Helpers
// ============================================================================
inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::wstring NormalizeCrlfWide(const std::string& utf8_text) {
    std::string crlf;
    crlf.reserve(utf8_text.size() + 64u);
    for (size_t i = 0; i < utf8_text.size(); ++i) {
        const char ch = utf8_text[i];
        if (ch == '\r') {
            if (i + 1u < utf8_text.size() && utf8_text[i + 1u] == '\n') {
                crlf.push_back('\r');
                crlf.push_back('\n');
                ++i;
            } else {
                crlf.push_back('\r');
                crlf.push_back('\n');
            }
        } else if (ch == '\n') {
            crlf.push_back('\r');
            crlf.push_back('\n');
        } else {
            crlf.push_back(ch);
        }
    }
    return Utf8ToWide(crlf);
}

// ============================================================================
// Dynamic DPI Awareness & Windows 10/11 Dark Titlebar Helpers
// ============================================================================
inline void EnablePerMonitorDpiV2() noexcept {
    HMODULE h_user32 = ::GetModuleHandleW(L"user32.dll");
    if (h_user32) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((HANDLE)-4) on Win10 1703+ / Win11.
        using PfnSetDpiCtx = BOOL(WINAPI*)(HANDLE);
        auto fn = reinterpret_cast<PfnSetDpiCtx>(
            reinterpret_cast<void*>(::GetProcAddress(h_user32, "SetProcessDpiAwarenessContext")));
        if (fn) {
            if (fn(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) {
                return;
            }
        }
    }
    ::SetProcessDPIAware();
}

inline UINT GetWindowDpiSafe(HWND hwnd) noexcept {
    HMODULE h_user32 = ::GetModuleHandleW(L"user32.dll");
    if (h_user32 && hwnd) {
        using PfnGetDpiForWindow = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<PfnGetDpiForWindow>(
            reinterpret_cast<void*>(::GetProcAddress(h_user32, "GetDpiForWindow")));
        if (fn) {
            const UINT dpi = fn(hwnd);
            if (dpi >= 96u) return dpi;
        }
    }
    HDC hdc = ::GetDC(hwnd);
    UINT dpi = 96u;
    if (hdc) {
        const int log_x = ::GetDeviceCaps(hdc, LOGPIXELSX);
        if (log_x >= 96) dpi = static_cast<UINT>(log_x);
        ::ReleaseDC(hwnd, hdc);
    }
    return dpi;
}

inline int ScalePx(int px, UINT dpi) noexcept {
    return ::MulDiv(px, static_cast<int>(dpi), 96);
}

inline float ScalePxF(float px, UINT dpi) noexcept {
    return px * (static_cast<float>(dpi) / 96.0f);
}

inline void ApplyImmersiveDarkMode(HWND hwnd) noexcept {
    if (!hwnd) return;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 2004+ / Win11), fallback 19 (Win10 18985)
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20u;
    constexpr DWORD kDwmwaUseImmersiveDarkModeLegacy = 19u;
    constexpr DWORD kDwmwaCaptionColor = 35u;
    constexpr DWORD kDwmwaTextColor = 36u;

    BOOL enable = TRUE;
    HRESULT hr = ::DwmSetWindowAttribute(
        hwnd, kDwmwaUseImmersiveDarkMode, &enable, static_cast<DWORD>(sizeof(enable)));
    if (FAILED(hr)) {
        (void)::DwmSetWindowAttribute(
            hwnd, kDwmwaUseImmersiveDarkModeLegacy, &enable, static_cast<DWORD>(sizeof(enable)));
    }
    // Optional Win11 caption bar matching our #0D1117 window background (gracefully ignored on Win10)
    COLORREF caption_bg = colors::kHeaderBg;
    COLORREF caption_fg = colors::kTextPrimary;
    (void)::DwmSetWindowAttribute(
        hwnd, kDwmwaCaptionColor, &caption_bg, static_cast<DWORD>(sizeof(caption_bg)));
    (void)::DwmSetWindowAttribute(
        hwnd, kDwmwaTextColor, &caption_fg, static_cast<DWORD>(sizeof(caption_fg)));
}

// ============================================================================
// D2D Geometry Convenience Helpers (Header-version-independent across toolchains)
// ============================================================================
inline D2D1_RECT_F MakeRectF(float l, float t, float r, float b) noexcept {
    D2D1_RECT_F rc{};
    rc.left = l;
    rc.top = t;
    rc.right = r;
    rc.bottom = b;
    return rc;
}

inline D2D1_ROUNDED_RECT MakeRoundedRect(float l, float t, float r, float b, float radius) noexcept {
    D2D1_ROUNDED_RECT rr{};
    rr.rect = MakeRectF(l, t, r, b);
    rr.radiusX = radius;
    rr.radiusY = radius;
    return rr;
}

// ============================================================================
// Button Visual Roles for Owner-Drawn Dark Buttons
// ============================================================================
enum class ButtonRole : int {
    Secondary = 0,
    PrimaryEmerald = 1,
    Danger = 2,
    AccentBlue = 3,
};

// ============================================================================
// RAII GDI Resources (Brushes, Pens, Fonts) for Controls & GDI Fallback
// ============================================================================
struct LauncherThemeResources {
    HBRUSH br_window_bg = nullptr;
    HBRUSH br_header_bg = nullptr;
    HBRUSH br_card_bg = nullptr;
    HBRUSH br_control_bg = nullptr;
    HBRUSH br_input_bg = nullptr;
    HPEN pen_border = nullptr;

    HFONT font_ui = nullptr;
    HFONT font_ui_bold = nullptr;
    HFONT font_title = nullptr;
    HFONT font_small = nullptr;
    HFONT font_mono = nullptr;

    UINT current_dpi = 96u;

    LauncherThemeResources() = default;
    ~LauncherThemeResources() { Destroy(); }

    LauncherThemeResources(const LauncherThemeResources&) = delete;
    LauncherThemeResources& operator=(const LauncherThemeResources&) = delete;

    void Initialize(UINT dpi) {
        Destroy();
        current_dpi = (dpi >= 96u) ? dpi : 96u;

        br_window_bg  = ::CreateSolidBrush(colors::kWindowBg);
        br_header_bg  = ::CreateSolidBrush(colors::kHeaderBg);
        br_card_bg    = ::CreateSolidBrush(colors::kSurfaceCard);
        br_control_bg = ::CreateSolidBrush(colors::kControlBg);
        br_input_bg   = ::CreateSolidBrush(colors::kInputBg);
        pen_border    = ::CreatePen(PS_SOLID, 1, colors::kBorderSubtle);

        font_ui = CreateScaledFont(L"Segoe UI", 14, FW_NORMAL);
        font_ui_bold = CreateScaledFont(L"Segoe UI", 14, FW_SEMIBOLD);
        font_title = CreateScaledFont(L"Segoe UI", 22, FW_SEMIBOLD);
        font_small = CreateScaledFont(L"Segoe UI", 12, FW_NORMAL);
        font_mono = CreateScaledFont(L"Consolas", 13, FW_NORMAL);
    }

    void Destroy() noexcept {
        auto del_obj = [](auto& handle) {
            if (handle) {
                ::DeleteObject(handle);
                handle = nullptr;
            }
        };
        del_obj(br_window_bg);
        del_obj(br_header_bg);
        del_obj(br_card_bg);
        del_obj(br_control_bg);
        del_obj(br_input_bg);
        del_obj(pen_border);
        del_obj(font_ui);
        del_obj(font_ui_bold);
        del_obj(font_title);
        del_obj(font_small);
        del_obj(font_mono);
    }

private:
    HFONT CreateScaledFont(const wchar_t* face, int px_96, int weight) const noexcept {
        LOGFONTW lf{};
        lf.lfHeight = -ScalePx(px_96, current_dpi);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        size_t i = 0;
        for (; i + 1u < LF_FACESIZE && face[i] != L'\0'; ++i) {
            lf.lfFaceName[i] = face[i];
        }
        lf.lfFaceName[i] = L'\0';
        return ::CreateFontIndirectW(&lf);
    }
};

// ============================================================================
// Crisp Owner-Drawn Dark Modern Win32 Push Button Renderer
// ============================================================================
inline void DrawDarkOwnerButton(const DRAWITEMSTRUCT* dis,
                                const LauncherThemeResources& res,
                                ButtonRole role,
                                bool is_hovered) noexcept {
    if (!dis || !dis->hDC) return;

    HDC hdc = dis->hDC;
    const RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0u;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0u;
    const bool focused  = (dis->itemState & ODS_FOCUS) != 0u;

    COLORREF fill = colors::kControlBg;
    COLORREF border = colors::kBorderSubtle;
    COLORREF text_col = colors::kTextPrimary;

    if (disabled) {
        fill = colors::kSurfaceCard;
        border = colors::kBorderSubtle;
        text_col = colors::kTextDisabled;
    } else if (role == ButtonRole::PrimaryEmerald) {
        fill = pressed ? colors::kEmeraldPressed
                       : (is_hovered ? colors::kEmeraldHover : colors::kEmeraldAccent);
        border = colors::kEmeraldHover;
        text_col = RGB(0xFF, 0xFF, 0xFF);
    } else if (role == ButtonRole::Danger) {
        fill = pressed ? RGB(0x8E, 0x22, 0x20)
                       : (is_hovered ? colors::kDangerHover : colors::kBadgeRedBg);
        border = colors::kFailRed;
        text_col = RGB(0xFF, 0xFF, 0xFF);
    } else if (role == ButtonRole::AccentBlue) {
        fill = pressed ? RGB(0x1B, 0x35, 0x57)
                       : (is_hovered ? RGB(0x21, 0x42, 0x6E) : colors::kBadgeBlueBg);
        border = colors::kTelemetryBlue;
        text_col = colors::kTextPrimary;
    } else {
        fill = pressed ? colors::kControlPressed
                       : (is_hovered ? colors::kControlHover : colors::kControlBg);
        border = focused ? colors::kBorderActive : colors::kBorderSubtle;
        text_col = colors::kTextPrimary;
    }

    // Clear corner background to match surrounding card/header surface
    HBRUSH bg_clear = ::CreateSolidBrush(colors::kSurfaceCard);
    ::FillRect(hdc, &rc, bg_clear);
    ::DeleteObject(bg_clear);

    HBRUSH hbr = ::CreateSolidBrush(fill);
    HPEN hpen = ::CreatePen(PS_SOLID, focused && !disabled ? 2 : 1, border);
    HGDIOBJ old_br = ::SelectObject(hdc, hbr);
    HGDIOBJ old_pen = ::SelectObject(hdc, hpen);

    const int radius = ScalePx(6, res.current_dpi);
    ::RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    ::SelectObject(hdc, old_br);
    ::SelectObject(hdc, old_pen);
    ::DeleteObject(hbr);
    ::DeleteObject(hpen);

    wchar_t label[128]{};
    ::GetWindowTextW(dis->hwndItem, label, static_cast<int>(sizeof(label) / sizeof(label[0])));

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, text_col);
    HFONT use_font = (role == ButtonRole::PrimaryEmerald) ? res.font_ui_bold : res.font_ui;
    HGDIOBJ old_font = use_font ? ::SelectObject(hdc, use_font) : nullptr;

    RECT text_rc = rc;
    if (pressed && !disabled) {
        text_rc.top += 1;
        text_rc.bottom += 1;
    }
    ::DrawTextW(hdc, label, -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (old_font) {
        ::SelectObject(hdc, old_font);
    }
}

}  // namespace openscarlett::launcher::theme
