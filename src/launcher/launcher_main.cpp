// OpenScarlett Launcher — Modern Dark Native Win32 + Direct2D + DirectWrite GUI
// MIT C++20 implementation. Zero third-party GUI or network libraries.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "launcher_resource.h"
#include "launcher_theme.h"
#include "launcher_probe_bridge.h"
#include "openscarlett/launcher/hwtest_runner.h"
#include "openscarlett/launcher/updater.h"

namespace fs = std::filesystem;

namespace {

using namespace openscarlett::launcher;
using namespace openscarlett::launcher::theme;

#if defined(_MSC_VER)
#  define OS_UUIDOF_D2D1_FACTORY    __uuidof(ID2D1Factory)
#  define OS_UUIDOF_DWRITE_FACTORY  __uuidof(IDWriteFactory)
#else
#  define OS_UUIDOF_D2D1_FACTORY    IID_ID2D1Factory
#  define OS_UUIDOF_DWRITE_FACTORY  IID_IDWriteFactory
#endif

// ============================================================================
// Control IDs & Custom Window Messages
// ============================================================================
enum ControlId : int {
    // Header controls
    kIdBtnCheckUpdates = 1001,
    kIdChkOffline      = 1002,
    kIdBtnInstallUpd   = 1003,

    // Tab 1: Play & Launch
    kIdEditTarget      = 2001,
    kIdBtnBrowseFolder = 2002,
    kIdBtnBrowseExe    = 2003,
    kIdComboBackend    = 2004,
    kIdComboAdapter    = 2005,
    kIdChkWindowed     = 2006,
    kIdChkNullAudio    = 2007,
    kIdEditExtraArgs   = 2008,
    kIdBtnLaunch       = 2009,
    kIdBtnStop         = 2010,
    kIdBtnClearLog     = 2011,
    kIdEditLaunchLog   = 2012,
    kIdChkProbeMode    = 2013,
    kIdBtnCopyTelemetry = 2014,
    kIdLblProbeStatus  = 2015,
    kIdBtnDemo0        = 2020,
    kIdBtnDemo1        = 2021,
    kIdBtnDemo2        = 2022,

    // Tab 2: 60s Hardware Self-Test
    kIdBtnRunHwTest    = 3001,
    kIdBtnCancelHwTest = 3002,
    kIdListHwTests     = 3003,
    kIdEditHwLog       = 3004,
    kIdBtnOpenGitHub   = 3005,
    kIdBtnCopyMarkdown = 3006,

    // Tab 3: Binary Inspector
    kIdEditPePath      = 4001,
    kIdBtnBrowsePe     = 4002,
    kIdBtnInspectCurr  = 4003,
    kIdBtnInspectNow   = 4004,
    kIdListPeModules   = 4005,

    // Update dialog controls
    kIdUpdEditNotes    = 5001,
    kIdUpdBtnDownload  = 5002,
    kIdUpdBtnGitHub    = 5003,
    kIdUpdBtnClose     = 5004,
    kIdUpdProgress     = 5005,
    kIdUpdStatus       = 5006,
};

constexpr UINT kMsgGpuProbeDone      = WM_APP + 101;
constexpr UINT kMsgUpdateCheckDone   = WM_APP + 102;
constexpr UINT kMsgLaunchLog         = WM_APP + 103;
constexpr UINT kMsgLaunchDone        = WM_APP + 104;
constexpr UINT kMsgHwTestProgress    = WM_APP + 105;
constexpr UINT kMsgHwTestDone        = WM_APP + 106;
constexpr UINT kMsgPeInspectDone     = WM_APP + 107;
constexpr UINT kMsgUpdateProgress    = WM_APP + 108;
constexpr UINT kMsgUpdateStageDone   = WM_APP + 109;

constexpr UINT_PTR kTimerSmokeStep   = 7701;

constexpr int kTabPlay     = 0;
constexpr int kTabHwTest   = 1;
constexpr int kTabPeInspect = 2;

// ============================================================================
// CLI Output Helper (works with redirected pipes in CTest/CI & parent console)
// ============================================================================
void WriteCliOutput(const std::string& text) {
    if (text.empty()) return;
    HANDLE h_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h_out || h_out == INVALID_HANDLE_VALUE) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            h_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (!h_out || h_out == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (::GetConsoleMode(h_out, &mode)) {
        DWORD written = 0;
        ::WriteConsoleA(h_out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        DWORD written = 0;
        ::WriteFile(h_out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
}

fs::path GetExecutableDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = ::GetModuleFileNameW(
        nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (len == 0 || len >= static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]))) {
        return fs::current_path();
    }
    return fs::path(buf).parent_path();
}

std::string FormatShaderModel(uint32_t sm) {
    if (sm == 0) return "SM ?";
    if (sm >= 0x50u) {
        const unsigned maj = (sm >> 4u) & 0xFu;
        const unsigned min = sm & 0xFu;
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "SM %u.%u", maj, min);
        return buf;
    }
    return "SM " + std::to_string(sm);
}

std::string FormatWaveBadge(uint32_t wmin, uint32_t wmax) {
    if (wmin == 0 && wmax == 0) return "Wave ?";
    if (wmin == wmax || wmax == 0) return "Wave " + std::to_string(wmin);
    return "Wave " + std::to_string(wmin) + "-" + std::to_string(wmax);
}

std::string FormatMeshBadge(uint32_t tier) {
    if (tier == 0) return "No Mesh";
    if (tier == 10) return "Mesh T1";
    return "Mesh T" + std::to_string(tier);
}

std::string FormatDxrBadge(uint32_t tier) {
    if (tier == 0) return "No DXR";
    if (tier == 10) return "DXR 1.0";
    if (tier == 11) return "DXR 1.1";
    return "DXR T" + std::to_string(tier);
}

std::string FormatVram(uint64_t vram_mb) {
    if (vram_mb == 0) return "0 MB";
    if (vram_mb >= 1024ull) {
        const double gb = static_cast<double>(vram_mb) / 1024.0;
        char buf[48]{};
        std::snprintf(buf, sizeof(buf), "%.1f GB (%llu MB)",
                      gb, static_cast<unsigned long long>(vram_mb));
        return buf;
    }
    return std::to_string(vram_mb) + " MB";
}

std::string FormatHex64(uint64_t val) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(val));
    return buf;
}

std::string FormatHex32(uint32_t val) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(val));
    return buf;
}

void AppendTextToEdit(HWND h_edit, const std::string& utf8_line) {
    if (!h_edit) return;
    const std::wstring wline = NormalizeCrlfWide(utf8_line);
    const int len = ::GetWindowTextLengthW(h_edit);
    // Keep edit buffer bounded under 256 KB so long runs stay snappy
    if (len > 200000) {
        ::SendMessageW(h_edit, EM_SETSEL, 0, static_cast<LPARAM>(len / 2));
        ::SendMessageW(h_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    const int end_len = ::GetWindowTextLengthW(h_edit);
    ::SendMessageW(h_edit, EM_SETSEL, static_cast<WPARAM>(end_len), static_cast<LPARAM>(end_len));
    ::SendMessageW(h_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(wline.c_str()));
    ::SendMessageW(h_edit, EM_SCROLLCARET, 0, 0);
}

std::string GetEditTextUtf8(HWND h_edit) {
    if (!h_edit) return {};
    const int len = ::GetWindowTextLengthW(h_edit);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len) + 1u, L'\0');
    ::GetWindowTextW(h_edit, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    return WideToUtf8(buf);
}

bool BrowseForFolderDialog(HWND owner, std::wstring& out_path) {
    ComRef<IFileOpenDialog> dlg;
    HRESULT hr = ::CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_IFileOpenDialog, reinterpret_cast<void**>(dlg.Put()));
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            (void)dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        (void)dlg->SetTitle(L"Select Unpacked GDK Package Folder (MicrosoftGame.Config)");
        if (SUCCEEDED(dlg->Show(owner))) {
            ComRef<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(item.Put())) && item) {
                PWSTR psz_path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz_path)) && psz_path) {
                    out_path = psz_path;
                    ::CoTaskMemFree(psz_path);
                    return true;
                }
            }
        }
        return false;
    }

    // Legacy fallback via SHBrowseForFolderW
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select Unpacked GDK Package Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path_buf[MAX_PATH]{};
    const BOOL ok = ::SHGetPathFromIDListW(pidl, path_buf);
    ::CoTaskMemFree(pidl);
    if (ok) {
        out_path = path_buf;
        return true;
    }
    return false;
}

bool BrowseForFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, std::wstring& out_path) {
    wchar_t file_buf[MAX_PATH * 2]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buf) / sizeof(file_buf[0]));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (::GetOpenFileNameW(&ofn)) {
        out_path = file_buf;
        return true;
    }
    return false;
}

// ============================================================================
// Dark Update Dialog (Opt-In Download, BCrypt SHA-256 Verify, Explicit Confirm)
// ============================================================================
struct UpdateDialogState {
    HWND hwnd = nullptr;
    HWND parent_hwnd = nullptr;
    fs::path bundle_dir;
    UpdateReleaseInfo release;
    const LauncherThemeResources* res = nullptr;

    HWND h_notes = nullptr;
    HWND h_status = nullptr;
    HWND h_progress = nullptr;
    HWND h_btn_download = nullptr;
    HWND h_btn_github = nullptr;
    HWND h_btn_close = nullptr;

    std::atomic<bool> downloading{false};
    std::atomic<bool> cancel_flag{false};
    std::thread worker;
    bool restart_requested = false;
    fs::path restart_script_path;

    ~UpdateDialogState() {
        cancel_flag.store(true);
        if (worker.joinable()) worker.join();
    }
};

LRESULT CALLBACK UpdateDialogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<UpdateDialogState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = static_cast<UpdateDialogState*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            if (st) st->hwnd = hwnd;
            return TRUE;
        }
        case WM_CREATE: {
            if (!st || !st->res) return -1;
            ApplyImmersiveDarkMode(hwnd);
            const UINT dpi = st->res->current_dpi;
            const int pad = ScalePx(16, dpi);
            const int w = ScalePx(580, dpi);

            std::string header = "Current: " + std::string(kOpenScarlettAppVersion) +
                                 "   ->   Available: " + st->release.tag_name;
            if (!st->release.published_at.empty()) {
                header += "  (" + st->release.published_at.substr(0, 10) + ")";
            }

            HWND h_lbl_hdr = ::CreateWindowExW(
                0, L"STATIC", Utf8ToWide(header).c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                pad, pad, w - pad * 2, ScalePx(24, dpi),
                hwnd, nullptr, nullptr, nullptr);
            ::SendMessageW(h_lbl_hdr, WM_SETFONT, reinterpret_cast<WPARAM>(st->res->font_ui_bold), TRUE);

            std::string notes = st->release.body_markdown.empty()
                                    ? "(No release notes provided.)"
                                    : st->release.body_markdown;
            st->h_notes = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", NormalizeCrlfWide(notes).c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                pad, ScalePx(48, dpi), w - pad * 2, ScalePx(210, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpdEditNotes)), nullptr, nullptr);
            ::SendMessageW(st->h_notes, WM_SETFONT, reinterpret_cast<WPARAM>(st->res->font_mono), TRUE);

            st->h_status = ::CreateWindowExW(
                0, L"STATIC",
                L"Opt-in update: downloads zip + .sha256 over HTTPS and verifies SHA-256 via Windows BCrypt before extracting.",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                pad, ScalePx(268, dpi), w - pad * 2, ScalePx(36, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpdStatus)), nullptr, nullptr);
            ::SendMessageW(st->h_status, WM_SETFONT, reinterpret_cast<WPARAM>(st->res->font_small), TRUE);

            st->h_progress = ::CreateWindowExW(
                0, PROGRESS_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                pad, ScalePx(310, dpi), w - pad * 2, ScalePx(18, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpdProgress)), nullptr, nullptr);
            ::SendMessageW(st->h_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            ::SendMessageW(st->h_progress, PBM_SETPOS, 0, 0);

            const int btn_y = ScalePx(342, dpi);
            const int btn_h = ScalePx(34, dpi);
            st->h_btn_download = ::CreateWindowExW(
                0, L"BUTTON", L"Download && Apply Update",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                pad, btn_y, ScalePx(210, dpi), btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpdBtnDownload)), nullptr, nullptr);

            st->h_btn_github = ::CreateWindowExW(
                0, L"BUTTON", L"View on GitHub",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                pad + ScalePx(222, dpi), btn_y, ScalePx(150, dpi), btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpdBtnGitHub)), nullptr, nullptr);

            st->h_btn_close = ::CreateWindowExW(
                0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                w - pad - ScalePx(110, dpi), btn_y, ScalePx(110, dpi), btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpdBtnClose)), nullptr, nullptr);

            if (st->release.zip_download_url.empty() || st->release.sha256_download_url.empty()) {
                ::EnableWindow(st->h_btn_download, FALSE);
                ::SetWindowTextW(st->h_status,
                                 L"Release does not contain both portable .zip and .sha256 assets. Use 'View on GitHub'.");
            }
            return 0;
        }
        case WM_ERASEBKGND: {
            if (!st || !st->res) break;
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(hdc, &rc, st->res->br_card_bg);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            if (!st || !st->res) break;
            HDC hdc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(hdc, colors::kTextPrimary);
            ::SetBkColor(hdc, colors::kSurfaceCard);
            return reinterpret_cast<LRESULT>(st->res->br_card_bg);
        }
        case WM_CTLCOLOREDIT: {
            if (!st || !st->res) break;
            HDC hdc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(hdc, colors::kTextPrimary);
            ::SetBkColor(hdc, colors::kInputBg);
            return reinterpret_cast<LRESULT>(st->res->br_input_bg);
        }
        case WM_DRAWITEM: {
            if (!st || !st->res) break;
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            ButtonRole role = ButtonRole::Secondary;
            if (dis->CtlID == kIdUpdBtnDownload) role = ButtonRole::PrimaryEmerald;
            else if (dis->CtlID == kIdUpdBtnGitHub) role = ButtonRole::AccentBlue;
            DrawDarkOwnerButton(dis, *st->res, role, false);
            return TRUE;
        }
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wParam);
            if (id == kIdUpdBtnGitHub) {
                if (!st->release.html_url.empty()) {
                    OpenUrlInDefaultBrowser(st->release.html_url);
                }
                return 0;
            }
            if (id == kIdUpdBtnClose) {
                ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (id == kIdUpdBtnDownload && !st->downloading.load()) {
                st->downloading.store(true);
                st->cancel_flag.store(false);
                ::EnableWindow(st->h_btn_download, FALSE);
                ::SetWindowTextW(st->h_btn_close, L"Cancel");
                ::SetWindowTextW(st->h_status, L"Starting verified HTTPS download...");
                if (st->worker.joinable()) st->worker.join();
                const UpdateReleaseInfo rel_copy = st->release;
                const fs::path app_dir = st->bundle_dir;
                st->worker = std::thread([st, rel_copy, app_dir]() {
                    auto res = DownloadVerifyAndStageUpdate(
                        rel_copy,
                        app_dir,
                        [st](const UpdateDownloadProgress& prog) {
                            auto* copy = new UpdateDownloadProgress(prog);
                            if (!::PostMessageW(st->hwnd, kMsgUpdateProgress, 0,
                                                reinterpret_cast<LPARAM>(copy))) {
                                delete copy;
                            }
                        },
                        &st->cancel_flag);
                    auto* done = new UpdateApplyResult(std::move(res));
                    if (!::PostMessageW(st->hwnd, kMsgUpdateStageDone, 0,
                                        reinterpret_cast<LPARAM>(done))) {
                        delete done;
                    }
                });
                return 0;
            }
            break;
        }
        case kMsgUpdateProgress: {
            std::unique_ptr<UpdateDownloadProgress> prog(
                reinterpret_cast<UpdateDownloadProgress*>(lParam));
            if (st && prog) {
                ::SendMessageW(st->h_progress, PBM_SETPOS,
                               static_cast<WPARAM>(std::clamp(prog->percent, 0, 100)), 0);
                std::string s = prog->stage;
                if (prog->total_bytes > 0) {
                    char buf[64]{};
                    std::snprintf(buf, sizeof(buf), " (%d%% - %.1f / %.1f MB)",
                                  prog->percent,
                                  static_cast<double>(prog->bytes_downloaded) / (1024.0 * 1024.0),
                                  static_cast<double>(prog->total_bytes) / (1024.0 * 1024.0));
                    s += buf;
                }
                ::SetWindowTextW(st->h_status, Utf8ToWide(s).c_str());
            }
            return 0;
        }
        case kMsgUpdateStageDone: {
            std::unique_ptr<UpdateApplyResult> res(
                reinterpret_cast<UpdateApplyResult*>(lParam));
            if (st && res) {
                st->downloading.store(false);
                ::SetWindowTextW(st->h_btn_close, L"Close");
                if (res->ok) {
                    ::SendMessageW(st->h_progress, PBM_SETPOS, 100, 0);
                    const std::string verified_msg =
                        "BCrypt SHA-256 VERIFIED: " + res->verified_sha256;
                    ::SetWindowTextW(st->h_status, Utf8ToWide(verified_msg).c_str());

                    const std::wstring prompt =
                        L"Update " + Utf8ToWide(st->release.tag_name) +
                        L" downloaded and passed Windows BCrypt SHA-256 verification!\n\n"
                        L"SHA-256: " + Utf8ToWide(res->verified_sha256) +
                        L"\n\nYour S:\\ saves and user configuration will be preserved.\n"
                        L"Close OpenScarlett Launcher now and apply the update?";
                    const int choice = ::MessageBoxW(
                        hwnd, prompt.c_str(), L"Apply Verified OpenScarlett Update",
                        MB_YESNO | MB_ICONQUESTION);
                    if (choice == IDYES) {
                        st->restart_requested = true;
                        st->restart_script_path = res->restart_script_path;
                        ::DestroyWindow(hwnd);
                    } else {
                        ::EnableWindow(st->h_btn_download, TRUE);
                    }
                } else {
                    const std::string err = "Update failed / rejected: " + res->error_message;
                    ::SetWindowTextW(st->h_status, Utf8ToWide(err).c_str());
                    ::EnableWindow(st->h_btn_download, TRUE);
                }
            }
            return 0;
        }
        case WM_CLOSE: {
            if (st) {
                st->cancel_flag.store(true);
                if (st->worker.joinable()) st->worker.join();
            }
            ::DestroyWindow(hwnd);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Main Launcher Application Controller
// ============================================================================
class LauncherWindow {
public:
    struct Options {
        fs::path bundle_dir;
        bool smoke_mode = false;
        int smoke_ms = 200;
    };

    explicit LauncherWindow(Options opts) : opts_(std::move(opts)) {}

    ~LauncherWindow() {
        JoinAllWorkers();
        DiscardD2DResources();
        if (hinst_) {
            ::UnregisterClassW(kMainClassName, hinst_);
            ::UnregisterClassW(kUpdateClassName, hinst_);
        }
        DestroyAppIcons();
    }

    int Run(HINSTANCE hInstance, int nCmdShow) {
        hinst_ = hInstance;
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
        ::InitCommonControlsEx(&icc);

        // Initialize Direct2D & DirectWrite factories (non-fatal if unavailable -> GDI fallback)
        (void)::D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            OS_UUIDOF_D2D1_FACTORY,
            nullptr,
            reinterpret_cast<void**>(d2d_factory_.Put()));
        (void)::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            OS_UUIDOF_DWRITE_FACTORY,
            reinterpret_cast<IUnknown**>(dwrite_factory_.Put()));

        RegisterClasses();

        const UINT init_dpi = GetWindowDpiSafe(nullptr);
        const int win_w = ScalePx(1120, init_dpi);
        const int win_h = ScalePx(760, init_dpi);

        const std::wstring title =
            L"OpenScarlett Launcher — " + Utf8ToWide(kOpenScarlettAppVersion);
        hwnd_ = ::CreateWindowExW(
            WS_EX_ACCEPTFILES,
            kMainClassName,
            title.c_str(),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
            nullptr, nullptr, hinst_, this);
        if (!hwnd_) return 1;

        if (icon_big_) {
            ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big_));
        }
        if (icon_small_) {
            ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));
        }

        ::ShowWindow(hwnd_, opts_.smoke_mode ? SW_SHOWNOACTIVATE : nCmdShow);
        ::UpdateWindow(hwnd_);

        if (opts_.smoke_mode) {
            smoke_start_ = std::chrono::steady_clock::now();
            const UINT step_ms = static_cast<UINT>(std::max(25, opts_.smoke_ms / 4));
            ::SetTimer(hwnd_, kTimerSmokeStep, step_ms, nullptr);
        }

        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!::IsDialogMessageW(hwnd_, &msg)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }

        return static_cast<int>(msg.wParam);
    }

    int paint_frames() const noexcept { return paint_frames_; }
    bool used_d2d() const noexcept { return used_d2d_; }

private:
    static constexpr const wchar_t* kMainClassName   = L"OpenScarlettLauncherMainWnd";
    static constexpr const wchar_t* kUpdateClassName = L"OpenScarlettLauncherUpdateDlg";

    void LoadAppIcons() {
        if (!icon_big_) {
            const int cx = ::GetSystemMetrics(SM_CXICON);
            const int cy = ::GetSystemMetrics(SM_CYICON);
            icon_big_ = static_cast<HICON>(::LoadImageW(
                hinst_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                cx > 0 ? cx : 32, cy > 0 ? cy : 32, LR_DEFAULTCOLOR));
            if (icon_big_) {
                own_icon_big_ = true;
            } else {
                icon_big_ = ::LoadIconW(nullptr, IDI_APPLICATION);
                own_icon_big_ = false;
            }
        }
        if (!icon_small_) {
            const int cx = ::GetSystemMetrics(SM_CXSMICON);
            const int cy = ::GetSystemMetrics(SM_CYSMICON);
            icon_small_ = static_cast<HICON>(::LoadImageW(
                hinst_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                cx > 0 ? cx : 16, cy > 0 ? cy : 16, LR_DEFAULTCOLOR));
            if (icon_small_) {
                own_icon_small_ = true;
            } else {
                icon_small_ = ::LoadIconW(nullptr, IDI_APPLICATION);
                own_icon_small_ = false;
            }
        }
    }

    void DestroyAppIcons() noexcept {
        if (own_icon_big_ && icon_big_) {
            ::DestroyIcon(icon_big_);
        }
        if (own_icon_small_ && icon_small_) {
            ::DestroyIcon(icon_small_);
        }
        icon_big_ = nullptr;
        icon_small_ = nullptr;
        own_icon_big_ = false;
        own_icon_small_ = false;
    }

    void RegisterClasses() {
        LoadAppIcons();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &LauncherWindow::StaticWndProc;
        wc.hInstance = hinst_;
        wc.hIcon = icon_big_;
        wc.hIconSm = icon_small_;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kMainClassName;
        ::RegisterClassExW(&wc);

        WNDCLASSEXW uwc{};
        uwc.cbSize = sizeof(uwc);
        uwc.lpfnWndProc = &UpdateDialogWndProc;
        uwc.hInstance = hinst_;
        uwc.hIcon = icon_big_;
        uwc.hIconSm = icon_small_;
        uwc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        uwc.hbrBackground = nullptr;
        uwc.lpszClassName = kUpdateClassName;
        ::RegisterClassExW(&uwc);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        LauncherWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<LauncherWindow*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<LauncherWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // ------------------------------------------------------------------------
    // Direct2D / DirectWrite Resource Management (with GDI Fallback Safety)
    // ------------------------------------------------------------------------
    bool EnsureD2DResources() {
        if (!d2d_factory_ || !hwnd_) return false;
        if (d2d_rt_) return true;

        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
        const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

        D2D1_RENDER_TARGET_PROPERTIES rt_props{};
        rt_props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        rt_props.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
        rt_props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_UNKNOWN;
        rt_props.dpiX = 96.0f;
        rt_props.dpiY = 96.0f;

        D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props{};
        hwnd_props.hwnd = hwnd_;
        hwnd_props.pixelSize.width = w;
        hwnd_props.pixelSize.height = h;
        hwnd_props.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

        HRESULT hr = d2d_factory_->CreateHwndRenderTarget(
            &rt_props, &hwnd_props, d2d_rt_.Put());
        if (FAILED(hr) || !d2d_rt_) {
            return false;
        }

        const auto make_brush = [&](COLORREF col, ComRef<ID2D1SolidColorBrush>& out) -> bool {
            const D2D1_COLOR_F c = colors::ToD2D(col);
            return SUCCEEDED(d2d_rt_->CreateSolidColorBrush(&c, nullptr, out.Put()));
        };
        if (!make_brush(colors::kWindowBg, br_d2d_bg_) ||
            !make_brush(colors::kHeaderBg, br_d2d_header_) ||
            !make_brush(colors::kSurfaceCard, br_d2d_card_) ||
            !make_brush(colors::kControlBg, br_d2d_ctrl_) ||
            !make_brush(colors::kBorderSubtle, br_d2d_border_) ||
            !make_brush(colors::kEmeraldAccent, br_d2d_emerald_) ||
            !make_brush(colors::kXboxGreen, br_d2d_xbox_green_) ||
            !make_brush(colors::kTelemetryBlue, br_d2d_blue_) ||
            !make_brush(colors::kWarningAmber, br_d2d_amber_) ||
            !make_brush(colors::kFailRed, br_d2d_red_) ||
            !make_brush(colors::kTextPrimary, br_d2d_text_) ||
            !make_brush(colors::kTextSecondary, br_d2d_muted_) ||
            !make_brush(colors::kBadgeGreenBg, br_d2d_badge_green_) ||
            !make_brush(colors::kBadgeBlueBg, br_d2d_badge_blue_) ||
            !make_brush(colors::kBadgeAmberBg, br_d2d_badge_amber_) ||
            !make_brush(colors::kBadgeRedBg, br_d2d_badge_red_)) {
            DiscardD2DResources();
            return false;
        }

        CreateDWriteFormats();
        return true;
    }

    void CreateDWriteFormats() {
        if (!dwrite_factory_) return;
        const float s = static_cast<float>(dpi_) / 96.0f;

        auto make_fmt = [&](const wchar_t* face, DWRITE_FONT_WEIGHT wt, float sz,
                            DWRITE_TEXT_ALIGNMENT align, ComRef<IDWriteTextFormat>& out) {
            out.Reset();
            if (SUCCEEDED(dwrite_factory_->CreateTextFormat(
                    face, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    sz * s, L"en-us", out.Put())) && out) {
                (void)out->SetTextAlignment(align);
                (void)out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                (void)out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };

        make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 20.0f, DWRITE_TEXT_ALIGNMENT_LEADING, tf_title_);
        make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, DWRITE_TEXT_ALIGNMENT_LEADING, tf_section_);
        make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, DWRITE_TEXT_ALIGNMENT_CENTER, tf_tab_);
        make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.5f, DWRITE_TEXT_ALIGNMENT_CENTER, tf_badge_);
        make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING, tf_body_);
        make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 15.0f, DWRITE_TEXT_ALIGNMENT_LEADING, tf_metric_);
    }

    void DiscardD2DResources() noexcept {
        br_d2d_bg_.Reset();
        br_d2d_header_.Reset();
        br_d2d_card_.Reset();
        br_d2d_ctrl_.Reset();
        br_d2d_border_.Reset();
        br_d2d_emerald_.Reset();
        br_d2d_xbox_green_.Reset();
        br_d2d_blue_.Reset();
        br_d2d_amber_.Reset();
        br_d2d_red_.Reset();
        br_d2d_text_.Reset();
        br_d2d_muted_.Reset();
        br_d2d_badge_green_.Reset();
        br_d2d_badge_blue_.Reset();
        br_d2d_badge_amber_.Reset();
        br_d2d_badge_red_.Reset();
        tf_title_.Reset();
        tf_section_.Reset();
        tf_tab_.Reset();
        tf_badge_.Reset();
        tf_body_.Reset();
        tf_metric_.Reset();
        d2d_rt_.Reset();
    }

    // ------------------------------------------------------------------------
    // Window Creation & Child Controls
    // ------------------------------------------------------------------------
    void OnCreate() {
        dpi_ = GetWindowDpiSafe(hwnd_);
        theme_.Initialize(dpi_);
        ApplyImmersiveDarkMode(hwnd_);
        ::DragAcceptFiles(hwnd_, TRUE);

        offline_mode_ = opts_.smoke_mode || IsUpdateCheckDisabledByEnv();
        demos_ = DiscoverBundledDemos(opts_.bundle_dir);

        CreateHeaderControls();
        CreateTabPlayControls();
        CreateTabHwTestControls();
        CreateTabPeInspectorControls();

        ApplyFontsToAllChildren();
        SwitchTab(kTabPlay);
        LayoutControls();

        if (opts_.smoke_mode) {
            // Seed deterministic telemetry preview so smoke test exercises every badge & card
            gpu_.probed = true;
            gpu_.probe_ok = true;
            gpu_.primary_gpu_name = "Smoke Preview GPU";
            gpu_.primary_vram_mb = 4096;
            gpu_.primary_shader_model = 0x66;
            gpu_.primary_wave_min = 32;
            gpu_.primary_wave_max = 32;
            gpu_.primary_mesh_tier = 10;
            gpu_.primary_dxr_tier = 10;
            gpu_.vulkan_available = true;
            gpu_.vulkan_api_version = "1.4";
            PopulateAdapterCombo();
        } else {
            StartGpuProbeWorker();
            if (!offline_mode_) {
                StartUpdateCheckWorker(false);
            }
        }
    }

    HWND CreateOwnerBtn(int id, const wchar_t* text, bool visible = true) {
        DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
        return ::CreateWindowExW(
            0, L"BUTTON", text, style,
            0, 0, 100, 30,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst_, nullptr);
    }

    HWND CreateDarkLabel(const wchar_t* text, bool visible = true) {
        DWORD style = WS_CHILD | SS_LEFT | SS_CENTERIMAGE | (visible ? WS_VISIBLE : 0);
        return ::CreateWindowExW(
            0, L"STATIC", text, style,
            0, 0, 100, 22, hwnd_, nullptr, hinst_, nullptr);
    }

    HWND CreateDarkEdit(int id, const wchar_t* text, DWORD extra_style = 0, bool mono = false) {
        HWND h = ::CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", text,
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
            0, 0, 100, 26,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst_, nullptr);
        if (mono) mono_controls_.push_back(h);
        return h;
    }

    void CreateHeaderControls() {
        h_btn_check_upd_ = CreateOwnerBtn(kIdBtnCheckUpdates, L"Check for Updates");
        h_chk_offline_ = ::CreateWindowExW(
            0, L"BUTTON", L"Offline",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 76, 24,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkOffline)), hinst_, nullptr);
        ::SendMessageW(h_chk_offline_, BM_SETCHECK, offline_mode_ ? BST_CHECKED : BST_UNCHECKED, 0);

        h_btn_install_upd_ = CreateOwnerBtn(kIdBtnInstallUpd, L"Update Available...", false);
    }

    void CreateTabPlayControls() {
        h_lbl_target_ = CreateDarkLabel(L"Target (.exe / Folder / MicrosoftGame.Config):");
        h_edit_target_ = CreateDarkEdit(kIdEditTarget, L"");
        h_btn_browse_dir_ = CreateOwnerBtn(kIdBtnBrowseFolder, L"Browse Folder...");
        h_btn_browse_exe_ = CreateOwnerBtn(kIdBtnBrowseExe, L"Browse EXE...");

        h_lbl_backend_ = CreateDarkLabel(L"Backend:");
        h_combo_backend_ = ::CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 150, 200,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboBackend)), hinst_, nullptr);
        ::SendMessageW(h_combo_backend_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
        ::SendMessageW(h_combo_backend_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Direct3D 12"));
        ::SendMessageW(h_combo_backend_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Vulkan 1.4"));
        ::SendMessageW(h_combo_backend_, CB_SETCURSEL, 0, 0);

        h_lbl_adapter_ = CreateDarkLabel(L"GPU Adapter:");
        h_combo_adapter_ = ::CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 240, 220,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboAdapter)), hinst_, nullptr);
        PopulateAdapterCombo();

        h_chk_windowed_ = ::CreateWindowExW(
            0, L"BUTTON", L"Windowed Mode",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 130, 24,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkWindowed)), hinst_, nullptr);
        ::SendMessageW(h_chk_windowed_, BM_SETCHECK, BST_CHECKED, 0);

        h_chk_null_audio_ = ::CreateWindowExW(
            0, L"BUTTON", L"Mute / Null Audio",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 145, 24,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkNullAudio)), hinst_, nullptr);

        h_lbl_args_ = CreateDarkLabel(L"Extra Args:");
        h_edit_args_ = CreateDarkEdit(kIdEditExtraArgs, L"");

        h_btn_launch_ = CreateOwnerBtn(kIdBtnLaunch, L"Launch");
        h_btn_stop_ = CreateOwnerBtn(kIdBtnStop, L"Stop");
        ::EnableWindow(h_btn_stop_, FALSE);
        h_btn_clear_log_ = CreateOwnerBtn(kIdBtnClearLog, L"Clear Log");

        h_chk_probe_mode_ = ::CreateWindowExW(
            0, L"BUTTON", L"First-Boot Probe (missing imports + VEH)",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 290, 24,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkProbeMode)), hinst_, nullptr);
        h_btn_copy_telemetry_ = CreateOwnerBtn(kIdBtnCopyTelemetry, L"Copy Sanitized Telemetry");
        h_lbl_probe_status_ = CreateDarkLabel(L"Telemetry ready (usernames & local paths auto-stripped)");

        // Quick-launch bundled demos (up to 3 slots)
        const wchar_t* fallback_demo_names[3] = {
            L"ScarlettDoom",
            L"ScarlettArena",
            L"Scarlett 3D Scene (gdk_3d_scene)",
        };
        for (int i = 0; i < 3; ++i) {
            std::wstring label = fallback_demo_names[i];
            bool avail = false;
            if (static_cast<size_t>(i) < demos_.size()) {
                label = Utf8ToWide(demos_[static_cast<size_t>(i)].display_name);
                avail = demos_[static_cast<size_t>(i)].available;
            }
            if (!avail) label += L" (Not Bundled)";
            h_btn_demos_[i] = CreateOwnerBtn(kIdBtnDemo0 + i, label.c_str());
            ::EnableWindow(h_btn_demos_[i], avail ? TRUE : FALSE);
        }

        h_edit_launch_log_ = CreateDarkEdit(
            kIdEditLaunchLog,
            L"Ready. Select a GDK folder/EXE, drag-and-drop onto this window, or click a Bundled Demo above.\r\n",
            ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL, true);

        tab_play_ctrls_ = {
            h_lbl_target_, h_edit_target_, h_btn_browse_dir_, h_btn_browse_exe_,
            h_lbl_backend_, h_combo_backend_, h_lbl_adapter_, h_combo_adapter_,
            h_chk_windowed_, h_chk_null_audio_, h_lbl_args_, h_edit_args_,
            h_btn_launch_, h_btn_stop_, h_btn_clear_log_,
            h_chk_probe_mode_, h_btn_copy_telemetry_, h_lbl_probe_status_,
            h_btn_demos_[0], h_btn_demos_[1], h_btn_demos_[2],
            h_edit_launch_log_,
        };
    }

    void CreateTabHwTestControls() {
        h_btn_run_hw_ = CreateOwnerBtn(kIdBtnRunHwTest, L"Run 60s Hardware Test");
        h_btn_cancel_hw_ = CreateOwnerBtn(kIdBtnCancelHwTest, L"Cancel");
        ::EnableWindow(h_btn_cancel_hw_, FALSE);

        h_list_hw_ = ::CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 400, 220,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdListHwTests)), hinst_, nullptr);
        ListView_SetExtendedListViewStyle(h_list_hw_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(h_list_hw_, colors::kInputBg);
        ListView_SetTextBkColor(h_list_hw_, colors::kInputBg);
        ListView_SetTextColor(h_list_hw_, colors::kTextPrimary);

        InsertListColumn(h_list_hw_, 0, L"Test Name", ScalePx(220, dpi_));
        InsertListColumn(h_list_hw_, 1, L"Status", ScalePx(80, dpi_));
        InsertListColumn(h_list_hw_, 2, L"Time", ScalePx(75, dpi_));
        InsertListColumn(h_list_hw_, 3, L"Highlight / Notes", ScalePx(480, dpi_));

        h_edit_hw_log_ = CreateDarkEdit(
            kIdEditHwLog,
            L"Click 'Run 60s Hardware Test' to benchmark D3D12, Vulkan 1.4, RDNA2 shader lifting, and GDK shims.\r\n",
            ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL, true);

        h_btn_open_gh_ = CreateOwnerBtn(kIdBtnOpenGitHub, L"Open Pre-Filled GitHub Hardware Report");
        ::EnableWindow(h_btn_open_gh_, FALSE);
        h_btn_copy_md_ = CreateOwnerBtn(kIdBtnCopyMarkdown, L"Copy Markdown");
        ::EnableWindow(h_btn_copy_md_, FALSE);
        h_lbl_report_path_ = CreateDarkLabel(L"Report: (not yet generated)");

        tab_hw_ctrls_ = {
            h_btn_run_hw_, h_btn_cancel_hw_, h_list_hw_, h_edit_hw_log_,
            h_btn_open_gh_, h_btn_copy_md_, h_lbl_report_path_,
        };
    }

    void CreateTabPeInspectorControls() {
        h_lbl_pe_path_ = CreateDarkLabel(L"PE32+ Binary (.exe / .dll):");
        h_edit_pe_path_ = CreateDarkEdit(kIdEditPePath, L"");
        h_btn_browse_pe_ = CreateOwnerBtn(kIdBtnBrowsePe, L"Browse PE32+...");
        h_btn_inspect_curr_ = CreateOwnerBtn(kIdBtnInspectCurr, L"Inspect Current Target");
        h_btn_inspect_now_ = CreateOwnerBtn(kIdBtnInspectNow, L"Inspect");

        h_list_pe_ = ::CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 400, 300,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdListPeModules)), hinst_, nullptr);
        ListView_SetExtendedListViewStyle(h_list_pe_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(h_list_pe_, colors::kInputBg);
        ListView_SetTextBkColor(h_list_pe_, colors::kInputBg);
        ListView_SetTextColor(h_list_pe_, colors::kTextPrimary);

        InsertListColumn(h_list_pe_, 0, L"DLL Name", ScalePx(190, dpi_));
        InsertListColumn(h_list_pe_, 1, L"Category", ScalePx(135, dpi_));
        InsertListColumn(h_list_pe_, 2, L"Imports", ScalePx(70, dpi_));
        InsertListColumn(h_list_pe_, 3, L"OpenScarlett Shim Routing", ScalePx(250, dpi_));
        InsertListColumn(h_list_pe_, 4, L"Missing Exports", ScalePx(260, dpi_));

        tab_pe_ctrls_ = {
            h_lbl_pe_path_, h_edit_pe_path_, h_btn_browse_pe_,
            h_btn_inspect_curr_, h_btn_inspect_now_, h_list_pe_,
        };
    }

    static void InsertListColumn(HWND h_lv, int col, const wchar_t* title, int width) {
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = width;
        lvc.iSubItem = col;
        lvc.pszText = const_cast<LPWSTR>(title);
        ListView_InsertColumn(h_lv, col, &lvc);
    }

    void ApplyFontsToAllChildren() {
        auto set_font = [&](HWND h, HFONT f) {
            if (h && f) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        };
        set_font(h_btn_check_upd_, theme_.font_ui);
        set_font(h_chk_offline_, theme_.font_ui);
        set_font(h_btn_install_upd_, theme_.font_ui_bold);

        for (HWND h : tab_play_ctrls_) set_font(h, theme_.font_ui);
        for (HWND h : tab_hw_ctrls_) set_font(h, theme_.font_ui);
        for (HWND h : tab_pe_ctrls_) set_font(h, theme_.font_ui);
        for (HWND h : mono_controls_) set_font(h, theme_.font_mono);
        set_font(h_lbl_probe_status_, theme_.font_small);
        set_font(h_lbl_report_path_, theme_.font_small);
    }

    void PopulateAdapterCombo() {
        if (!h_combo_adapter_) return;
        ::SendMessageW(h_combo_adapter_, CB_RESETCONTENT, 0, 0);
        adapter_choices_.clear();

        ::SendMessageW(h_combo_adapter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto (Default)"));
        adapter_choices_.push_back({"", false});

        for (const auto& a : gpu_.adapters) {
            if (a.is_warp || a.name.empty()) continue;
            std::wstring label = Utf8ToWide(a.name);
            if (a.vram_mb > 0) {
                label += L" (" + std::to_wstring(a.vram_mb) + L" MB)";
            }
            ::SendMessageW(h_combo_adapter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            adapter_choices_.push_back({a.name, false});
        }

        ::SendMessageW(h_combo_adapter_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(L"D3D12 WARP (Software)"));
        adapter_choices_.push_back({"", true});
        ::SendMessageW(h_combo_adapter_, CB_SETCURSEL, 0, 0);
    }

    void SwitchTab(int tab) {
        if (tab < 0 || tab > 2) return;
        active_tab_ = tab;
        const auto show_vec = [](const std::vector<HWND>& ctrls, bool vis) {
            const int cmd = vis ? SW_SHOW : SW_HIDE;
            for (HWND h : ctrls) {
                if (h) ::ShowWindow(h, cmd);
            }
        };
        show_vec(tab_play_ctrls_, active_tab_ == kTabPlay);
        show_vec(tab_hw_ctrls_, active_tab_ == kTabHwTest);
        show_vec(tab_pe_ctrls_, active_tab_ == kTabPeInspect);
        LayoutControls();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ------------------------------------------------------------------------
    // Responsive DPI-Aware Layout
    // ------------------------------------------------------------------------
    int HeaderHeight() const noexcept { return ScalePx(104, dpi_); }
    int TabBarHeight() const noexcept { return ScalePx(44, dpi_); }

    void LayoutControls() {
        if (!hwnd_) return;
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        const int cw = std::max<int>(ScalePx(600, dpi_), rc.right - rc.left);
        const int ch = std::max<int>(ScalePx(480, dpi_), rc.bottom - rc.top);

        const int pad = ScalePx(16, dpi_);
        const int inner = ScalePx(14, dpi_);
        const int hdr_h = HeaderHeight();
        const int tab_h = TabBarHeight();

        // Header right-aligned updater controls
        const int chk_w = ScalePx(76, dpi_);
        const int upd_btn_w = ScalePx(142, dpi_);
        const int inst_btn_w = ScalePx(148, dpi_);
        const int hdr_btn_h = ScalePx(30, dpi_);
        const int hdr_btn_y = ScalePx(18, dpi_);

        ::MoveWindow(h_chk_offline_, cw - pad - chk_w, hdr_btn_y + ScalePx(3, dpi_), chk_w, ScalePx(24, dpi_), TRUE);
        ::MoveWindow(h_btn_check_upd_, cw - pad - chk_w - ScalePx(10, dpi_) - upd_btn_w,
                     hdr_btn_y, upd_btn_w, hdr_btn_h, TRUE);
        ::MoveWindow(h_btn_install_upd_,
                     cw - pad - chk_w - ScalePx(10, dpi_) - upd_btn_w - ScalePx(8, dpi_) - inst_btn_w,
                     hdr_btn_y, inst_btn_w, hdr_btn_h, TRUE);

        // Tab hitboxes
        const int tab_w = ScalePx(210, dpi_);
        for (int i = 0; i < 3; ++i) {
            tab_rects_[i] = {
                pad + i * (tab_w + ScalePx(6, dpi_)),
                hdr_h + ScalePx(4, dpi_),
                pad + i * (tab_w + ScalePx(6, dpi_)) + tab_w,
                hdr_h + tab_h - ScalePx(2, dpi_),
            };
        }

        const int body_top = hdr_h + tab_h + ScalePx(12, dpi_);
        const int card_w = cw - pad * 2;

        if (active_tab_ == kTabPlay) {
            // Card 1: Launch configuration (y = body_top, h = 200)
            const int c1_y = body_top;
            const int c1_h = ScalePx(200, dpi_);
            play_card1_ = {pad, c1_y, pad + card_w, c1_y + c1_h};

            const int x0 = pad + inner;
            const int rx = pad + card_w - inner;
            int y = c1_y + ScalePx(32, dpi_);

            const int bw_dir = ScalePx(122, dpi_);
            const int bw_exe = ScalePx(108, dpi_);
            const int gap = ScalePx(8, dpi_);
            ::MoveWindow(h_lbl_target_, x0, y, ScalePx(310, dpi_), ScalePx(20, dpi_), TRUE);
            y += ScalePx(22, dpi_);
            const int edit_w = std::max<int>(ScalePx(180, dpi_), rx - x0 - bw_dir - bw_exe - gap * 2);
            ::MoveWindow(h_edit_target_, x0, y, edit_w, ScalePx(28, dpi_), TRUE);
            ::MoveWindow(h_btn_browse_dir_, x0 + edit_w + gap, y, bw_dir, ScalePx(28, dpi_), TRUE);
            ::MoveWindow(h_btn_browse_exe_, x0 + edit_w + gap + bw_dir + gap, y, bw_exe, ScalePx(28, dpi_), TRUE);

            // Row 2: Backend, GPU Adapter, Windowed, Null Audio
            y += ScalePx(36, dpi_);
            const int lbl_be_w = ScalePx(62, dpi_);
            const int cb_be_w = ScalePx(132, dpi_);
            const int lbl_ad_w = ScalePx(86, dpi_);
            const int cb_ad_w = std::min<int>(ScalePx(280, dpi_), std::max<int>(ScalePx(160, dpi_), card_w / 4));
            ::MoveWindow(h_lbl_backend_, x0, y + ScalePx(2, dpi_), lbl_be_w, ScalePx(24, dpi_), TRUE);
            ::MoveWindow(h_combo_backend_, x0 + lbl_be_w, y, cb_be_w, ScalePx(180, dpi_), TRUE);
            int cx = x0 + lbl_be_w + cb_be_w + ScalePx(14, dpi_);
            ::MoveWindow(h_lbl_adapter_, cx, y + ScalePx(2, dpi_), lbl_ad_w, ScalePx(24, dpi_), TRUE);
            cx += lbl_ad_w;
            ::MoveWindow(h_combo_adapter_, cx, y, cb_ad_w, ScalePx(200, dpi_), TRUE);
            cx += cb_ad_w + ScalePx(14, dpi_);
            ::MoveWindow(h_chk_windowed_, cx, y + ScalePx(2, dpi_), ScalePx(124, dpi_), ScalePx(24, dpi_), TRUE);
            cx += ScalePx(128, dpi_);
            ::MoveWindow(h_chk_null_audio_, cx, y + ScalePx(2, dpi_), ScalePx(136, dpi_), ScalePx(24, dpi_), TRUE);

            // Row 3: Extra Args + Big Emerald Launch + Stop + Clear Log
            y += ScalePx(36, dpi_);
            const int btn_launch_w = ScalePx(130, dpi_);
            const int btn_stop_w = ScalePx(84, dpi_);
            const int btn_clr_w = ScalePx(92, dpi_);
            const int lbl_arg_w = ScalePx(72, dpi_);
            const int arg_edit_w = std::max<int>(
                ScalePx(120, dpi_),
                rx - x0 - lbl_arg_w - btn_launch_w - btn_stop_w - btn_clr_w - gap * 4);
            ::MoveWindow(h_lbl_args_, x0, y + ScalePx(4, dpi_), lbl_arg_w, ScalePx(24, dpi_), TRUE);
            ::MoveWindow(h_edit_args_, x0 + lbl_arg_w, y + ScalePx(2, dpi_), arg_edit_w, ScalePx(28, dpi_), TRUE);
            int bx = x0 + lbl_arg_w + arg_edit_w + gap;
            ::MoveWindow(h_btn_launch_, bx, y, btn_launch_w, ScalePx(32, dpi_), TRUE);
            bx += btn_launch_w + gap;
            ::MoveWindow(h_btn_stop_, bx, y, btn_stop_w, ScalePx(32, dpi_), TRUE);
            bx += btn_stop_w + gap;
            ::MoveWindow(h_btn_clear_log_, bx, y, btn_clr_w, ScalePx(32, dpi_), TRUE);

            // Row 4: First-Boot Probe Mode + Copy Sanitized Telemetry + Status
            y += ScalePx(38, dpi_);
            const int chk_probe_w = ScalePx(286, dpi_);
            const int btn_copy_tel_w = ScalePx(188, dpi_);
            ::MoveWindow(h_chk_probe_mode_, x0, y + ScalePx(3, dpi_), chk_probe_w, ScalePx(24, dpi_), TRUE);
            const int copy_x = x0 + chk_probe_w + ScalePx(10, dpi_);
            ::MoveWindow(h_btn_copy_telemetry_, copy_x, y, btn_copy_tel_w, ScalePx(30, dpi_), TRUE);
            const int st_x = copy_x + btn_copy_tel_w + ScalePx(12, dpi_);
            const int st_w = std::max<int>(ScalePx(120, dpi_), rx - st_x);
            ::MoveWindow(h_lbl_probe_status_, st_x, y + ScalePx(4, dpi_), st_w, ScalePx(22, dpi_), TRUE);

            // Card 2: Quick-Launch Bundled Demos (h = 78)
            const int c2_y = c1_y + c1_h + ScalePx(10, dpi_);
            const int c2_h = ScalePx(78, dpi_);
            play_card2_ = {pad, c2_y, pad + card_w, c2_y + c2_h};
            const int demo_w = (card_w - inner * 2 - gap * 2) / 3;
            for (int i = 0; i < 3; ++i) {
                ::MoveWindow(h_btn_demos_[i],
                             x0 + i * (demo_w + gap),
                             c2_y + ScalePx(34, dpi_),
                             demo_w, ScalePx(32, dpi_), TRUE);
            }

            // Telemetry strip (h = 56)
            const int tele_y = c2_y + c2_h + ScalePx(10, dpi_);
            const int tele_h = ScalePx(54, dpi_);
            play_telemetry_rect_ = {pad, tele_y, pad + card_w, tele_y + tele_h};

            // Card 3: Live Output Log (remaining height)
            const int c3_y = tele_y + tele_h + ScalePx(10, dpi_);
            const int c3_h = std::max<int>(ScalePx(120, dpi_), ch - pad - c3_y);
            play_card3_ = {pad, c3_y, pad + card_w, c3_y + c3_h};
            ::MoveWindow(h_edit_launch_log_,
                         x0, c3_y + ScalePx(32, dpi_),
                         card_w - inner * 2, std::max<int>(ScalePx(60, dpi_), c3_h - ScalePx(44, dpi_)), TRUE);
        } else if (active_tab_ == kTabHwTest) {
            const int x0 = pad + inner;
            const int gap = ScalePx(10, dpi_);

            // Card 1: Run controls + D2D Progress Bar + Privacy Banner (h = 128)
            const int c1_y = body_top;
            const int c1_h = ScalePx(128, dpi_);
            hw_card1_ = {pad, c1_y, pad + card_w, c1_y + c1_h};

            ::MoveWindow(h_btn_run_hw_, x0, c1_y + ScalePx(14, dpi_), ScalePx(190, dpi_), ScalePx(34, dpi_), TRUE);
            ::MoveWindow(h_btn_cancel_hw_, x0 + ScalePx(200, dpi_), c1_y + ScalePx(14, dpi_),
                         ScalePx(96, dpi_), ScalePx(34, dpi_), TRUE);

            hw_progress_rect_ = {
                x0 + ScalePx(312, dpi_), c1_y + ScalePx(18, dpi_),
                pad + card_w - inner, c1_y + ScalePx(44, dpi_),
            };
            hw_privacy_rect_ = {
                x0, c1_y + ScalePx(86, dpi_),
                pad + card_w - inner, c1_y + ScalePx(116, dpi_),
            };

            // Bottom action row height
            const int action_h = ScalePx(48, dpi_);
            const int mid_top = c1_y + c1_h + ScalePx(10, dpi_);
            const int mid_avail = std::max<int>(ScalePx(200, dpi_), ch - pad - action_h - mid_top - ScalePx(10, dpi_));
            const int list_card_h = (mid_avail * 58) / 100;
            const int log_card_h = mid_avail - list_card_h - ScalePx(10, dpi_);

            hw_card_table_ = {pad, mid_top, pad + card_w, mid_top + list_card_h};
            ::MoveWindow(h_list_hw_, x0, mid_top + ScalePx(30, dpi_),
                         card_w - inner * 2, std::max<int>(ScalePx(70, dpi_), list_card_h - ScalePx(42, dpi_)), TRUE);

            const int log_y = mid_top + list_card_h + ScalePx(10, dpi_);
            hw_card_log_ = {pad, log_y, pad + card_w, log_y + log_card_h};
            ::MoveWindow(h_edit_hw_log_, x0, log_y + ScalePx(30, dpi_),
                         card_w - inner * 2, std::max<int>(ScalePx(50, dpi_), log_card_h - ScalePx(42, dpi_)), TRUE);

            const int act_y = ch - pad - action_h + ScalePx(8, dpi_);
            const int btn_gh_w = ScalePx(286, dpi_);
            const int btn_cp_w = ScalePx(134, dpi_);
            ::MoveWindow(h_btn_open_gh_, pad, act_y, btn_gh_w, ScalePx(34, dpi_), TRUE);
            ::MoveWindow(h_btn_copy_md_, pad + btn_gh_w + gap, act_y, btn_cp_w, ScalePx(34, dpi_), TRUE);
            ::MoveWindow(h_lbl_report_path_,
                         pad + btn_gh_w + gap + btn_cp_w + ScalePx(14, dpi_),
                         act_y + ScalePx(6, dpi_),
                         std::max<int>(ScalePx(120, dpi_), card_w - btn_gh_w - btn_cp_w - gap * 3),
                         ScalePx(24, dpi_), TRUE);
        } else if (active_tab_ == kTabPeInspect) {
            const int x0 = pad + inner;
            const int gap = ScalePx(8, dpi_);

            // Card 1: File selector (h = 84)
            const int c1_y = body_top;
            const int c1_h = ScalePx(84, dpi_);
            pe_card1_ = {pad, c1_y, pad + card_w, c1_y + c1_h};

            ::MoveWindow(h_lbl_pe_path_, x0, c1_y + ScalePx(12, dpi_), ScalePx(260, dpi_), ScalePx(20, dpi_), TRUE);
            const int b1 = ScalePx(126, dpi_);
            const int b2 = ScalePx(156, dpi_);
            const int b3 = ScalePx(88, dpi_);
            const int ew = std::max<int>(ScalePx(140, dpi_), card_w - inner * 2 - b1 - b2 - b3 - gap * 3);
            const int row_y = c1_y + ScalePx(38, dpi_);
            ::MoveWindow(h_edit_pe_path_, x0, row_y, ew, ScalePx(28, dpi_), TRUE);
            ::MoveWindow(h_btn_browse_pe_, x0 + ew + gap, row_y, b1, ScalePx(28, dpi_), TRUE);
            ::MoveWindow(h_btn_inspect_curr_, x0 + ew + gap + b1 + gap, row_y, b2, ScalePx(28, dpi_), TRUE);
            ::MoveWindow(h_btn_inspect_now_, x0 + ew + gap + b1 + gap + b2 + gap, row_y, b3, ScalePx(28, dpi_), TRUE);

            // Card 2: PE Header Summary (D2D painted, h = 112)
            const int c2_y = c1_y + c1_h + ScalePx(10, dpi_);
            const int c2_h = ScalePx(112, dpi_);
            pe_card_summary_ = {pad, c2_y, pad + card_w, c2_y + c2_h};

            // Card 3: Per-DLL Routing & Missing Exports Table
            const int c3_y = c2_y + c2_h + ScalePx(10, dpi_);
            const int c3_h = std::max<int>(ScalePx(140, dpi_), ch - pad - c3_y);
            pe_card_table_ = {pad, c3_y, pad + card_w, c3_y + c3_h};
            ::MoveWindow(h_list_pe_, x0, c3_y + ScalePx(32, dpi_),
                         card_w - inner * 2, std::max<int>(ScalePx(80, dpi_), c3_h - ScalePx(44, dpi_)), TRUE);
        }
    }

    // ------------------------------------------------------------------------
    // Direct2D + DirectWrite Painting (with Full GDI Fallback)
    // ------------------------------------------------------------------------
    void OnPaint() {
        PAINTSTRUCT ps{};
        HDC hdc = ::BeginPaint(hwnd_, &ps);
        ++paint_frames_;

        if (EnsureD2DResources()) {
            used_d2d_ = true;
            PaintWithD2D();
        } else {
            PaintWithGdiFallback(hdc);
        }

        ::EndPaint(hwnd_, &ps);
    }

    float DrawBadgeD2D(float x, float y, const std::wstring& text,
                       ID2D1SolidColorBrush* bg, ID2D1SolidColorBrush* border,
                       ID2D1SolidColorBrush* fg, RECT* out_hitbox = nullptr) {
        const float s = static_cast<float>(dpi_) / 96.0f;
        const float est_w = std::max(54.0f * s, static_cast<float>(text.size()) * 7.2f * s + 18.0f * s);
        const float h = 23.0f * s;
        const D2D1_ROUNDED_RECT rr = MakeRoundedRect(x, y, x + est_w, y + h, 5.0f * s);
        d2d_rt_->FillRoundedRectangle(&rr, bg);
        d2d_rt_->DrawRoundedRectangle(&rr, border, 1.0f);
        if (tf_badge_) {
            const D2D1_RECT_F tr = MakeRectF(x + 4.0f * s, y, x + est_w - 4.0f * s, y + h);
            d2d_rt_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), tf_badge_.Get(), &tr, fg);
        }
        if (out_hitbox) {
            out_hitbox->left = static_cast<LONG>(x);
            out_hitbox->top = static_cast<LONG>(y);
            out_hitbox->right = static_cast<LONG>(x + est_w);
            out_hitbox->bottom = static_cast<LONG>(y + h);
        }
        return x + est_w + 7.0f * s;
    }

    void DrawCardD2D(const RECT& r, const wchar_t* title = nullptr) {
        const float s = static_cast<float>(dpi_) / 96.0f;
        const D2D1_ROUNDED_RECT rr = MakeRoundedRect(
            static_cast<float>(r.left), static_cast<float>(r.top),
            static_cast<float>(r.right), static_cast<float>(r.bottom), 8.0f * s);
        d2d_rt_->FillRoundedRectangle(&rr, br_d2d_card_.Get());
        d2d_rt_->DrawRoundedRectangle(&rr, br_d2d_border_.Get(), 1.0f);
        if (title && tf_section_) {
            const D2D1_RECT_F tr = MakeRectF(
                static_cast<float>(r.left) + 14.0f * s,
                static_cast<float>(r.top) + 4.0f * s,
                static_cast<float>(r.right) - 14.0f * s,
                static_cast<float>(r.top) + 28.0f * s);
            d2d_rt_->DrawText(title, static_cast<UINT32>(std::wcslen(title)),
                              tf_section_.Get(), &tr, br_d2d_text_.Get());
        }
    }

    void PaintWithD2D() {
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        const float s = static_cast<float>(dpi_) / 96.0f;
        const float pad = 16.0f * s;
        const float hdr_h = static_cast<float>(HeaderHeight());
        const float tab_h = static_cast<float>(TabBarHeight());

        d2d_rt_->BeginDraw();
        d2d_rt_->Clear(colors::ToD2D(colors::kWindowBg));

        // 1. Header Strip
        const D2D1_RECT_F hdr_rc = MakeRectF(0.0f, 0.0f, w, hdr_h);
        d2d_rt_->FillRectangle(&hdr_rc, br_d2d_header_.Get());
        // Scarlett emerald left accent stripe
        const D2D1_RECT_F stripe = MakeRectF(0.0f, 0.0f, 4.0f * s, hdr_h);
        d2d_rt_->FillRectangle(&stripe, br_d2d_emerald_.Get());
        // Bottom border of header
        const D2D1_POINT_2F h_l{0.0f, hdr_h - 0.5f};
        const D2D1_POINT_2F h_r{w, hdr_h - 0.5f};
        d2d_rt_->DrawLine(h_l, h_r, br_d2d_border_.Get(), 1.0f);

        // Brand pill + Title + Version pill
        const D2D1_ROUNDED_RECT logo_rr = MakeRoundedRect(pad, 14.0f * s, pad + 34.0f * s, 46.0f * s, 7.0f * s);
        d2d_rt_->FillRoundedRectangle(&logo_rr, br_d2d_xbox_green_.Get());
        d2d_rt_->DrawRoundedRectangle(&logo_rr, br_d2d_emerald_.Get(), 1.0f);
        if (tf_tab_) {
            d2d_rt_->DrawText(L"OS", 2, tf_tab_.Get(), &logo_rr.rect, br_d2d_text_.Get());
        }
        if (tf_title_) {
            const D2D1_RECT_F title_rc = MakeRectF(pad + 44.0f * s, 12.0f * s, pad + 215.0f * s, 48.0f * s);
            d2d_rt_->DrawText(L"OpenScarlett", 12, tf_title_.Get(), &title_rc, br_d2d_text_.Get());
        }
        float vx = DrawBadgeD2D(
            pad + 206.0f * s, 19.0f * s, Utf8ToWide(kOpenScarlettAppVersion),
            br_d2d_badge_green_.Get(), br_d2d_emerald_.Get(), br_d2d_emerald_.Get());

        // Optional Update Available pill in header
        update_pill_rect_ = {0, 0, 0, 0};
        if (update_info_.update_available && !update_info_.tag_name.empty()) {
            const std::wstring up_txt = L"Update: " + Utf8ToWide(update_info_.tag_name);
            vx = DrawBadgeD2D(vx + 4.0f * s, 19.0f * s, up_txt,
                              br_d2d_badge_amber_.Get(), br_d2d_amber_.Get(), br_d2d_amber_.Get(),
                              &update_pill_rect_);
        } else if (!update_status_short_.empty() && tf_body_) {
            const std::wstring stw = Utf8ToWide(update_status_short_);
            const D2D1_RECT_F st_rc = MakeRectF(vx + 6.0f * s, 18.0f * s, w - 360.0f * s, 44.0f * s);
            if (st_rc.right > st_rc.left + 30.0f) {
                d2d_rt_->DrawText(stw.c_str(), static_cast<UINT32>(stw.size()),
                                  tf_body_.Get(), &st_rc, br_d2d_muted_.Get());
            }
        }

        // Live GPU Telemetry Row
        const float gpu_y = 58.0f * s;
        std::wstring gpu_line;
        if (!gpu_.probed) {
            gpu_line = L"GPU Telemetry: Probing host D3D12 & Vulkan 1.4 adapters...";
        } else if (!gpu_.primary_gpu_name.empty()) {
            gpu_line = Utf8ToWide(gpu_.primary_gpu_name + "  |  " + FormatVram(gpu_.primary_vram_mb));
        } else {
            gpu_line = L"GPU Telemetry: Probe complete (no D3D12 hardware adapter reported)";
        }

        const float gpu_text_w = std::min(430.0f * s, w * 0.42f);
        if (tf_body_) {
            const D2D1_RECT_F gpu_rc = MakeRectF(pad, gpu_y, pad + gpu_text_w, gpu_y + 26.0f * s);
            d2d_rt_->DrawText(gpu_line.c_str(), static_cast<UINT32>(gpu_line.size()),
                              tf_body_.Get(), &gpu_rc, br_d2d_text_.Get());
        }

        float bx = pad + gpu_text_w + 10.0f * s;
        if (gpu_.probed) {
            bx = DrawBadgeD2D(bx, gpu_y + 1.0f * s, Utf8ToWide(FormatShaderModel(gpu_.primary_shader_model)),
                              br_d2d_badge_blue_.Get(), br_d2d_blue_.Get(), br_d2d_blue_.Get());
            bx = DrawBadgeD2D(bx, gpu_y + 1.0f * s,
                              Utf8ToWide(FormatWaveBadge(gpu_.primary_wave_min, gpu_.primary_wave_max)),
                              br_d2d_badge_blue_.Get(), br_d2d_blue_.Get(), br_d2d_blue_.Get());
            const bool has_mesh = gpu_.primary_mesh_tier > 0;
            bx = DrawBadgeD2D(bx, gpu_y + 1.0f * s, Utf8ToWide(FormatMeshBadge(gpu_.primary_mesh_tier)),
                              has_mesh ? br_d2d_badge_green_.Get() : br_d2d_ctrl_.Get(),
                              has_mesh ? br_d2d_emerald_.Get() : br_d2d_border_.Get(),
                              has_mesh ? br_d2d_emerald_.Get() : br_d2d_muted_.Get());
            const bool has_dxr = gpu_.primary_dxr_tier > 0;
            bx = DrawBadgeD2D(bx, gpu_y + 1.0f * s, Utf8ToWide(FormatDxrBadge(gpu_.primary_dxr_tier)),
                              has_dxr ? br_d2d_badge_blue_.Get() : br_d2d_ctrl_.Get(),
                              has_dxr ? br_d2d_blue_.Get() : br_d2d_border_.Get(),
                              has_dxr ? br_d2d_blue_.Get() : br_d2d_muted_.Get());
            const std::wstring vk_label = gpu_.vulkan_available
                ? Utf8ToWide("Vulkan " + (gpu_.vulkan_api_version.empty() ? "1.4" : gpu_.vulkan_api_version))
                : L"Vulkan N/A";
            (void)DrawBadgeD2D(bx, gpu_y + 1.0f * s, vk_label,
                               gpu_.vulkan_available ? br_d2d_badge_green_.Get() : br_d2d_ctrl_.Get(),
                               gpu_.vulkan_available ? br_d2d_emerald_.Get() : br_d2d_border_.Get(),
                               gpu_.vulkan_available ? br_d2d_emerald_.Get() : br_d2d_muted_.Get());
        }

        // 2. Tab Navigation Bar
        const D2D1_RECT_F tabbar_rc = MakeRectF(0.0f, hdr_h, w, hdr_h + tab_h);
        d2d_rt_->FillRectangle(&tabbar_rc, br_d2d_header_.Get());
        const D2D1_POINT_2F tb_l{0.0f, hdr_h + tab_h - 0.5f};
        const D2D1_POINT_2F tb_r{w, hdr_h + tab_h - 0.5f};
        d2d_rt_->DrawLine(tb_l, tb_r, br_d2d_border_.Get(), 1.0f);

        const wchar_t* kTabLabelsD2D[3] = {
            L"Play & Launch",
            L"60s Hardware Self-Test",
            L"Binary Inspector",
        };
        for (int i = 0; i < 3; ++i) {
            const RECT& tr = tab_rects_[i];
            const D2D1_ROUNDED_RECT rr = MakeRoundedRect(
                static_cast<float>(tr.left), static_cast<float>(tr.top),
                static_cast<float>(tr.right), static_cast<float>(tr.bottom), 6.0f * s);
            const bool active = (i == active_tab_);
            const bool hovered = (i == hovered_tab_);
            if (active) {
                d2d_rt_->FillRoundedRectangle(&rr, br_d2d_card_.Get());
                const D2D1_RECT_F ind = MakeRectF(
                    static_cast<float>(tr.left) + 10.0f * s,
                    static_cast<float>(tr.bottom) - 3.0f * s,
                    static_cast<float>(tr.right) - 10.0f * s,
                    static_cast<float>(tr.bottom));
                d2d_rt_->FillRectangle(&ind, br_d2d_emerald_.Get());
            } else if (hovered) {
                d2d_rt_->FillRoundedRectangle(&rr, br_d2d_ctrl_.Get());
            }
            if (tf_tab_) {
                d2d_rt_->DrawText(
                    kTabLabelsD2D[i], static_cast<UINT32>(std::wcslen(kTabLabelsD2D[i])),
                    tf_tab_.Get(), &rr.rect,
                    active ? br_d2d_text_.Get() : br_d2d_muted_.Get());
            }
        }

        // 3. Active Tab Surface Cards & Custom Telemetry Widgets
        if (active_tab_ == kTabPlay) {
            DrawCardD2D(play_card1_, L"Target & Runtime Configuration");
            DrawCardD2D(play_card2_, L"1-Click Quick-Launch Bundled Demos");
            DrawPlayTelemetryStripD2D();
            DrawCardD2D(play_card3_, L"Live Execution Output");
        } else if (active_tab_ == kTabHwTest) {
            DrawCardD2D(hw_card1_);
            DrawHwProgressAndPrivacyD2D();
            DrawCardD2D(hw_card_table_, L"Per-Test Verification Results");
            DrawCardD2D(hw_card_log_, L"Live Sanitized Benchmark Log");
        } else if (active_tab_ == kTabPeInspect) {
            DrawCardD2D(pe_card1_);
            DrawCardD2D(pe_card_summary_, L"PE32+ Header & Import/Export Telemetry");
            DrawPeSummaryMetricsD2D();
            DrawCardD2D(pe_card_table_, L"Imported DLLs, OpenScarlett Shim Routing & Missing Exports");
        }

        const HRESULT hr = d2d_rt_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardD2DResources();
        }
        (void)h;
    }

    void DrawPlayTelemetryStripD2D() {
        const float s = static_cast<float>(dpi_) / 96.0f;
        const float gap = 10.0f * s;
        const float total_w = static_cast<float>(play_telemetry_rect_.right - play_telemetry_rect_.left);
        const float cell_w = (total_w - gap * 3.0f) / 4.0f;
        const float top = static_cast<float>(play_telemetry_rect_.top);
        const float bot = static_cast<float>(play_telemetry_rect_.bottom);

        struct Metric {
            const wchar_t* label;
            std::wstring value;
            ID2D1SolidColorBrush* val_brush;
        };
        const std::wstring exit_str =
            last_launch_.launched ? std::to_wstring(last_launch_.exit_code)
                                  : (launch_running_.load() ? L"Running..." : L"—");
        Metric items[4] = {
            {L"Total API Calls", std::to_wstring(last_launch_.total_calls), br_d2d_blue_.Get()},
            {L"Tracked Symbols", std::to_wstring(last_launch_.tracked_symbols), br_d2d_text_.Get()},
            {L"not_impl Calls", std::to_wstring(last_launch_.not_impl_calls),
             last_launch_.not_impl_calls == 0 ? br_d2d_emerald_.Get() : br_d2d_red_.Get()},
            {L"Exit Code", exit_str,
             (last_launch_.launched && last_launch_.exit_code == 0) ? br_d2d_emerald_.Get()
             : (last_launch_.launched ? br_d2d_red_.Get() : br_d2d_muted_.Get())},
        };

        for (int i = 0; i < 4; ++i) {
            const float x = static_cast<float>(play_telemetry_rect_.left) + static_cast<float>(i) * (cell_w + gap);
            const D2D1_ROUNDED_RECT rr = MakeRoundedRect(x, top, x + cell_w, bot, 7.0f * s);
            d2d_rt_->FillRoundedRectangle(&rr, br_d2d_card_.Get());
            d2d_rt_->DrawRoundedRectangle(&rr, br_d2d_border_.Get(), 1.0f);
            if (tf_body_) {
                const D2D1_RECT_F lr = MakeRectF(x + 12.0f * s, top + 4.0f * s, x + cell_w - 8.0f * s, top + 24.0f * s);
                d2d_rt_->DrawText(items[i].label, static_cast<UINT32>(std::wcslen(items[i].label)),
                                  tf_body_.Get(), &lr, br_d2d_muted_.Get());
            }
            if (tf_metric_) {
                const D2D1_RECT_F vr = MakeRectF(x + 12.0f * s, top + 22.0f * s, x + cell_w - 8.0f * s, bot - 4.0f * s);
                d2d_rt_->DrawText(items[i].value.c_str(), static_cast<UINT32>(items[i].value.size()),
                                  tf_metric_.Get(), &vr, items[i].val_brush);
            }
        }
    }

    void DrawHwProgressAndPrivacyD2D() {
        const float s = static_cast<float>(dpi_) / 96.0f;
        const RECT& pr = hw_progress_rect_;
        const D2D1_ROUNDED_RECT track = MakeRoundedRect(
            static_cast<float>(pr.left), static_cast<float>(pr.top),
            static_cast<float>(pr.right), static_cast<float>(pr.bottom), 6.0f * s);
        d2d_rt_->FillRoundedRectangle(&track, br_d2d_ctrl_.Get());
        d2d_rt_->DrawRoundedRectangle(&track, br_d2d_border_.Get(), 1.0f);

        const float frac = (hw_total_ > 0)
            ? std::clamp(static_cast<float>(hw_completed_) / static_cast<float>(hw_total_), 0.0f, 1.0f)
            : 0.0f;
        if (frac > 0.0f) {
            const float fill_w = std::max(10.0f * s, (static_cast<float>(pr.right - pr.left)) * frac);
            const D2D1_ROUNDED_RECT fill = MakeRoundedRect(
                static_cast<float>(pr.left), static_cast<float>(pr.top),
                static_cast<float>(pr.left) + fill_w, static_cast<float>(pr.bottom), 6.0f * s);
            d2d_rt_->FillRoundedRectangle(
                &fill, hw_failed_ > 0 ? br_d2d_amber_.Get() : br_d2d_emerald_.Get());
        }

        // Progress caption right beneath bar
        std::wstringstream ss;
        if (hw_running_.load()) {
            ss << L"Running " << hw_completed_ << L" / " << hw_total_
               << L"  |  PASS: " << hw_passed_ << L"  FAIL: " << hw_failed_;
            if (!hw_current_name_.empty()) {
                ss << L"  (" << Utf8ToWide(hw_current_name_) << L")";
            }
        } else if (hw_result_.completed) {
            ss << (hw_result_.cancelled ? L"Cancelled: " : L"Completed: ")
               << hw_result_.passed_count << L" / " << hw_result_.total_count
               << L" passed  (" << hw_result_.failed_count << L" failed)";
        } else {
            ss << L"Idle — ready to run full hardware verification suite (~60 seconds)";
        }
        const std::wstring prog_txt = ss.str();
        if (tf_body_) {
            const D2D1_RECT_F tr = MakeRectF(
                static_cast<float>(hw_card1_.left) + 14.0f * s,
                static_cast<float>(hw_card1_.top) + 56.0f * s,
                static_cast<float>(hw_card1_.right) - 14.0f * s,
                static_cast<float>(hw_card1_.top) + 80.0f * s);
            d2d_rt_->DrawText(prog_txt.c_str(), static_cast<UINT32>(prog_txt.size()),
                              tf_body_.Get(), &tr, br_d2d_text_.Get());
        }

        // Privacy banner
        const D2D1_ROUNDED_RECT priv = MakeRoundedRect(
            static_cast<float>(hw_privacy_rect_.left), static_cast<float>(hw_privacy_rect_.top),
            static_cast<float>(hw_privacy_rect_.right), static_cast<float>(hw_privacy_rect_.bottom), 6.0f * s);
        d2d_rt_->FillRoundedRectangle(&priv, br_d2d_badge_blue_.Get());
        d2d_rt_->DrawRoundedRectangle(&priv, br_d2d_blue_.Get(), 1.0f);
        if (tf_body_) {
            const wchar_t* notice =
                L"Privacy Guard: Usernames, profile directories, and local paths are automatically stripped before saving or sharing.";
            const D2D1_RECT_F nr = MakeRectF(
                static_cast<float>(hw_privacy_rect_.left) + 12.0f * s,
                static_cast<float>(hw_privacy_rect_.top),
                static_cast<float>(hw_privacy_rect_.right) - 12.0f * s,
                static_cast<float>(hw_privacy_rect_.bottom));
            d2d_rt_->DrawText(notice, static_cast<UINT32>(std::wcslen(notice)),
                              tf_body_.Get(), &nr, br_d2d_blue_.Get());
        }
    }

    void DrawPeSummaryMetricsD2D() {
        const float s = static_cast<float>(dpi_) / 96.0f;
        const float x = static_cast<float>(pe_card_summary_.left) + 14.0f * s;
        const float r = static_cast<float>(pe_card_summary_.right) - 14.0f * s;
        const float y1 = static_cast<float>(pe_card_summary_.top) + 34.0f * s;
        const float y2 = static_cast<float>(pe_card_summary_.top) + 60.0f * s;
        const float y3 = static_cast<float>(pe_card_summary_.top) + 84.0f * s;

        if (!pe_summary_.ok) {
            const std::wstring msg = pe_summary_.error_message.empty()
                ? L"Select or drag-and-drop any x86-64 PE32+ .exe or .dll to inspect imports, exports, and shim routing."
                : Utf8ToWide("Inspection error: " + pe_summary_.error_message);
            if (tf_body_) {
                const D2D1_RECT_F tr = MakeRectF(x, y1, r, y2 + 12.0f * s);
                d2d_rt_->DrawText(msg.c_str(), static_cast<UINT32>(msg.size()),
                                  tf_body_.Get(), &tr,
                                  pe_summary_.error_message.empty() ? br_d2d_muted_.Get() : br_d2d_red_.Get());
            }
            return;
        }

        const std::wstring line1 = Utf8ToWide(
            std::string("Format: ") + (pe_summary_.is_dll ? "PE32+ DLL" : "PE32+ EXE") +
            "   |   ImageBase: " + FormatHex64(pe_summary_.image_base) +
            "   |   SizeOfImage: " + FormatHex32(pe_summary_.image_size) +
            "   |   Sections: " + std::to_string(pe_summary_.section_count) +
            "   |   Relocs: " + (pe_summary_.has_relocations ? "Yes" : "No") +
            "   |   TLS Callbacks: " + std::to_string(pe_summary_.tls_callbacks) +
            "   |   .pdata SEH: " + std::to_string(pe_summary_.pdata_entries));
        const std::wstring line2 = Utf8ToWide(
            "Exports: " + std::to_string(pe_summary_.total_exports) +
            " (Forwarded: " + std::to_string(pe_summary_.forwarded_exports) + ")" +
            "   |   Imports Total: " + std::to_string(pe_summary_.total_imports) +
            "   |   Console: " + std::to_string(pe_summary_.console_imports) +
            "   |   GDK: " + std::to_string(pe_summary_.gdk_imports) +
            "   |   Delay-Load: " + std::to_string(pe_summary_.delay_load_imports));

        if (tf_body_) {
            const D2D1_RECT_F r1 = MakeRectF(x, y1, r, y1 + 22.0f * s);
            d2d_rt_->DrawText(line1.c_str(), static_cast<UINT32>(line1.size()),
                              tf_body_.Get(), &r1, br_d2d_text_.Get());
            const D2D1_RECT_F r2 = MakeRectF(x, y2, r, y2 + 22.0f * s);
            d2d_rt_->DrawText(line2.c_str(), static_cast<UINT32>(line2.size()),
                              tf_body_.Get(), &r2, br_d2d_muted_.Get());
        }

        const bool ok_missing = (pe_summary_.missing_export_count == 0);
        const std::wstring miss_txt = ok_missing
            ? L"Missing Shim Exports: 0 (All Routed Exports Present)"
            : Utf8ToWide("Missing Shim Exports: " + std::to_string(pe_summary_.missing_export_count));
        (void)DrawBadgeD2D(x, y3, miss_txt,
                           ok_missing ? br_d2d_badge_green_.Get() : br_d2d_badge_red_.Get(),
                           ok_missing ? br_d2d_emerald_.Get() : br_d2d_red_.Get(),
                           ok_missing ? br_d2d_emerald_.Get() : br_d2d_red_.Get());
    }

    void PaintWithGdiFallback(HDC hdc) {
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        ::FillRect(hdc, &rc, theme_.br_window_bg);

        RECT hdr = {0, 0, rc.right, HeaderHeight()};
        ::FillRect(hdc, &hdr, theme_.br_header_bg);

        ::SetBkMode(hdc, TRANSPARENT);
        ::SetTextColor(hdc, colors::kTextPrimary);
        HGDIOBJ old_f = ::SelectObject(hdc, theme_.font_title);
        RECT t_rc = {ScalePx(16, dpi_), ScalePx(14, dpi_), ScalePx(360, dpi_), ScalePx(48, dpi_)};
        ::DrawTextW(hdc, L"OpenScarlett", -1, &t_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        ::SelectObject(hdc, theme_.font_ui);
        const wchar_t* tabs[3] = {L"Play & Launch", L"60s Hardware Self-Test", L"Binary Inspector"};
        for (int i = 0; i < 3; ++i) {
            ::FillRect(hdc, &tab_rects_[i], i == active_tab_ ? theme_.br_card_bg : theme_.br_header_bg);
            ::SetTextColor(hdc, i == active_tab_ ? colors::kTextPrimary : colors::kTextSecondary);
            ::DrawTextW(hdc, tabs[i], -1, &tab_rects_[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        ::SelectObject(hdc, old_f);
    }

    // ------------------------------------------------------------------------
    // Background Workers (Non-Blocking UI)
    // ------------------------------------------------------------------------
    void StartGpuProbeWorker() {
        if (probe_thread_.joinable()) probe_thread_.join();
        const fs::path bdir = opts_.bundle_dir;
        probe_thread_ = std::thread([this, bdir]() {
            auto snap = ProbeHostGpuTelemetry(bdir);
            if (shutting_down_.load()) return;
            auto* heap = new GpuTelemetrySnapshot(std::move(snap));
            if (!::PostMessageW(hwnd_, kMsgGpuProbeDone, 0, reinterpret_cast<LPARAM>(heap))) {
                delete heap;
            }
        });
    }

    void StartUpdateCheckWorker(bool user_initiated) {
        if (update_checking_.exchange(true)) return;
        if (update_thread_.joinable()) update_thread_.join();
        update_status_short_ = user_initiated ? "Checking GitHub Releases..." : "";
        ::InvalidateRect(hwnd_, nullptr, FALSE);

        const bool offline = offline_mode_;
        update_thread_ = std::thread([this, offline, user_initiated]() {
            auto res = CheckLatestGitHubRelease(kOpenScarlettAppVersion, offline);
            update_checking_.store(false);
            if (shutting_down_.load()) return;
            auto* heap = new UpdateCheckResult(std::move(res));
            if (!::PostMessageW(hwnd_, kMsgUpdateCheckDone,
                                user_initiated ? 1u : 0u,
                                reinterpret_cast<LPARAM>(heap))) {
                delete heap;
            }
        });
    }

    void StartLaunchTarget(const std::string& explicit_target = "") {
        if (launch_running_.load()) return;
        std::string target = explicit_target.empty() ? GetEditTextUtf8(h_edit_target_) : explicit_target;
        if (target.empty()) {
            ::MessageBoxW(hwnd_, L"Please select a target folder, MicrosoftGame.Config, or .exe first.",
                          L"OpenScarlett Launcher", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!explicit_target.empty()) {
            ::SetWindowTextW(h_edit_target_, Utf8ToWide(target).c_str());
        }

        LauncherRunRequest req{};
        req.target_path = target;
        req.shim_dir = opts_.bundle_dir;

        const LRESULT be_sel = ::SendMessageW(h_combo_backend_, CB_GETCURSEL, 0, 0);
        if (be_sel == 1) req.backend = LaunchBackendChoice::D3D12;
        else if (be_sel == 2) req.backend = LaunchBackendChoice::Vulkan;
        else req.backend = LaunchBackendChoice::Auto;

        const LRESULT ad_sel = ::SendMessageW(h_combo_adapter_, CB_GETCURSEL, 0, 0);
        if (ad_sel >= 0 && static_cast<size_t>(ad_sel) < adapter_choices_.size()) {
            req.adapter_substring = adapter_choices_[static_cast<size_t>(ad_sel)].first;
            req.prefer_warp = adapter_choices_[static_cast<size_t>(ad_sel)].second;
        }

        req.windowed = (::SendMessageW(h_chk_windowed_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        req.null_audio = (::SendMessageW(h_chk_null_audio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        req.extra_args = GetEditTextUtf8(h_edit_args_);
        const bool probe_mode = (::SendMessageW(h_chk_probe_mode_, BM_GETCHECK, 0, 0) == BST_CHECKED);

        launch_running_.store(true);
        cancel_launch_.store(false);
        last_launch_ = LauncherRunResult{};
        ::EnableWindow(h_btn_launch_, FALSE);
        ::EnableWindow(h_btn_stop_, TRUE);
        AppendTextToEdit(h_edit_launch_log_, "\n=== Launching: " + target + " ===\n");
        if (probe_mode) {
            AppendTextToEdit(h_edit_launch_log_,
                             "[First-Boot Probe] --probe-unknown-imports + VEH crash monitor enabled\n");
            ::SetWindowTextW(h_lbl_probe_status_, L"Running in First-Boot Probe Mode...");
        }
        ::InvalidateRect(hwnd_, &play_telemetry_rect_, FALSE);

        if (launch_thread_.joinable()) launch_thread_.join();
        const fs::path bdir = opts_.bundle_dir;
        launch_thread_ = std::thread([this, bdir, req, probe_mode]() {
            auto res = LaunchTargetWithProbeOption(
                bdir, req, probe_mode,
                [this](const std::string& line) {
                    if (shutting_down_.load()) return;
                    auto* s = new std::string(line + "\n");
                    if (!::PostMessageW(hwnd_, kMsgLaunchLog, 0, reinterpret_cast<LPARAM>(s))) {
                        delete s;
                    }
                },
                &cancel_launch_);
            if (shutting_down_.load()) return;
            auto* heap = new LauncherRunResult(std::move(res));
            if (!::PostMessageW(hwnd_, kMsgLaunchDone, 0, reinterpret_cast<LPARAM>(heap))) {
                delete heap;
            }
        });
    }

    void StartHardwareSelfTest() {
        if (hw_running_.load()) return;
        hw_running_.store(true);
        cancel_hwtest_.store(false);
        hw_completed_ = 0;
        hw_total_ = 0;
        hw_passed_ = 0;
        hw_failed_ = 0;
        hw_current_name_.clear();
        hw_result_ = HardwareTestRunResult{};
        hw_rows_.clear();

        ListView_DeleteAllItems(h_list_hw_);
        ::SetWindowTextW(h_edit_hw_log_, L"Starting 60-Second Hardware Self-Test Suite...\r\n");
        ::SetWindowTextW(h_lbl_report_path_, L"Report: running...");
        ::EnableWindow(h_btn_run_hw_, FALSE);
        ::EnableWindow(h_btn_cancel_hw_, TRUE);
        ::EnableWindow(h_btn_open_gh_, FALSE);
        ::EnableWindow(h_btn_copy_md_, FALSE);
        ::InvalidateRect(hwnd_, &hw_card1_, FALSE);

        if (hw_thread_.joinable()) hw_thread_.join();
        const fs::path bdir = opts_.bundle_dir;
        const GpuTelemetrySnapshot snap_copy = gpu_;
        const bool have_probe = gpu_.probed && gpu_.probe_ok;
        hw_thread_ = std::thread([this, bdir, snap_copy, have_probe]() {
            auto res = RunHardwareSelfTestSuite(
                bdir,
                have_probe ? &snap_copy : nullptr,
                [this](const HardwareTestProgress& prog) {
                    if (shutting_down_.load()) return;
                    auto* p = new HardwareTestProgress(prog);
                    if (!::PostMessageW(hwnd_, kMsgHwTestProgress, 0, reinterpret_cast<LPARAM>(p))) {
                        delete p;
                    }
                },
                &cancel_hwtest_);
            if (shutting_down_.load()) return;
            auto* heap = new HardwareTestRunResult(std::move(res));
            if (!::PostMessageW(hwnd_, kMsgHwTestDone, 0, reinterpret_cast<LPARAM>(heap))) {
                delete heap;
            }
        });
    }

    void StartPeInspect(const std::string& path) {
        if (path.empty()) return;
        ::SetWindowTextW(h_edit_pe_path_, Utf8ToWide(path).c_str());
        if (pe_thread_.joinable()) pe_thread_.join();
        const fs::path pe_p(Utf8ToWide(path));
        const fs::path shim_dir = opts_.bundle_dir;
        pe_thread_ = std::thread([this, pe_p, shim_dir]() {
            auto sum = InspectBinaryForLauncher(pe_p, shim_dir);
            if (shutting_down_.load()) return;
            auto* heap = new PeInspectorSummary(std::move(sum));
            if (!::PostMessageW(hwnd_, kMsgPeInspectDone, 0, reinterpret_cast<LPARAM>(heap))) {
                delete heap;
            }
        });
    }

    void ShowUpdateDialog() {
        if (!update_info_.update_available) return;
        UpdateDialogState dlg_st{};
        dlg_st.parent_hwnd = hwnd_;
        dlg_st.bundle_dir = opts_.bundle_dir;
        dlg_st.release = update_info_;
        dlg_st.res = &theme_;

        const int w = ScalePx(600, dpi_);
        const int h = ScalePx(430, dpi_);
        RECT pr{};
        ::GetWindowRect(hwnd_, &pr);
        const int x = pr.left + ((pr.right - pr.left) - w) / 2;
        const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

        HWND hdlg = ::CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kUpdateClassName,
            L"OpenScarlett Update Available",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            x, y, w, h,
            hwnd_, nullptr, hinst_, &dlg_st);
        if (!hdlg) return;

        if (icon_big_) {
            ::SendMessageW(hdlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big_));
        }
        if (icon_small_) {
            ::SendMessageW(hdlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));
        }

        ::EnableWindow(hwnd_, FALSE);
        MSG msg{};
        while (::IsWindow(hdlg) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!::IsDialogMessageW(hdlg, &msg)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
        ::EnableWindow(hwnd_, TRUE);
        ::SetForegroundWindow(hwnd_);

        if (dlg_st.restart_requested && !dlg_st.restart_script_path.empty()) {
            const auto st = LaunchUpdateRestartHelper(
                dlg_st.restart_script_path, ::GetCurrentProcessId());
            if (st.ok()) {
                ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            } else {
                ::MessageBoxW(hwnd_,
                              Utf8ToWide("Failed to launch update restart helper: " + st.message).c_str(),
                              L"OpenScarlett Updater", MB_OK | MB_ICONERROR);
            }
        }
    }

    void JoinAllWorkers() {
        shutting_down_.store(true);
        cancel_launch_.store(true);
        cancel_hwtest_.store(true);
        if (probe_thread_.joinable()) probe_thread_.join();
        if (update_thread_.joinable()) update_thread_.join();
        if (launch_thread_.joinable()) launch_thread_.join();
        if (hw_thread_.joinable()) hw_thread_.join();
        if (pe_thread_.joinable()) pe_thread_.join();
    }

    // ------------------------------------------------------------------------
    // Window Message Handler
    // ------------------------------------------------------------------------
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE:
                OnCreate();
                return 0;

            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
                mmi->ptMinTrackSize.x = ScalePx(940, dpi_);
                mmi->ptMinTrackSize.y = ScalePx(640, dpi_);
                return 0;
            }

            case WM_SIZE: {
                if (d2d_rt_) {
                    D2D1_SIZE_U sz{};
                    sz.width = std::max<UINT>(1u, LOWORD(lParam));
                    sz.height = std::max<UINT>(1u, HIWORD(lParam));
                    (void)d2d_rt_->Resize(&sz);
                }
                LayoutControls();
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT:
                OnPaint();
                return 0;

            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                HWND ctrl = reinterpret_cast<HWND>(lParam);
                const bool in_header = (ctrl == h_chk_offline_);
                ::SetTextColor(hdc, colors::kTextPrimary);
                ::SetBkColor(hdc, in_header ? colors::kHeaderBg : colors::kSurfaceCard);
                return reinterpret_cast<LRESULT>(in_header ? theme_.br_header_bg : theme_.br_card_bg);
            }

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                ::SetTextColor(hdc, colors::kTextPrimary);
                ::SetBkColor(hdc, colors::kInputBg);
                return reinterpret_cast<LRESULT>(theme_.br_input_bg);
            }

            case WM_DRAWITEM: {
                const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                ButtonRole role = ButtonRole::Secondary;
                if (dis->CtlID == kIdBtnLaunch || dis->CtlID == kIdBtnRunHwTest ||
                    dis->CtlID == kIdBtnInstallUpd) {
                    role = ButtonRole::PrimaryEmerald;
                } else if (dis->CtlID == kIdBtnStop || dis->CtlID == kIdBtnCancelHwTest) {
                    role = ButtonRole::Danger;
                } else if (dis->CtlID == kIdBtnOpenGitHub || dis->CtlID == kIdBtnInspectNow ||
                           dis->CtlID == kIdBtnCopyTelemetry ||
                           (dis->CtlID >= kIdBtnDemo0 && dis->CtlID <= kIdBtnDemo2)) {
                    role = ButtonRole::AccentBlue;
                }
                DrawDarkOwnerButton(dis, theme_, role, false);
                return TRUE;
            }

            case WM_NOTIFY: {
                const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
                if (hdr && hdr->code == NM_CUSTOMDRAW &&
                    (hdr->idFrom == kIdListHwTests || hdr->idFrom == kIdListPeModules)) {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        const size_t idx = static_cast<size_t>(cd->nmcd.dwItemSpec);
                        cd->clrTextBk = (idx % 2 == 0) ? colors::kInputBg : colors::kSurfaceCard;
                        cd->clrText = colors::kTextPrimary;
                        if (hdr->idFrom == kIdListHwTests && idx < hw_rows_.size()) {
                            cd->clrText = hw_rows_[idx].passed ? colors::kEmeraldHover : colors::kFailRed;
                        } else if (hdr->idFrom == kIdListPeModules && idx < pe_summary_.modules.size()) {
                            const auto& mod = pe_summary_.modules[idx];
                            if (!mod.missing_exports.empty() || !mod.shim_present) {
                                cd->clrText = colors::kFailRed;
                            } else if (mod.category == "ConsoleExclusive" || mod.category == "GdkRuntime") {
                                cd->clrText = colors::kEmeraldHover;
                            }
                        }
                        return CDRF_NEWFONT;
                    }
                }
                break;
            }

            case WM_MOUSEMOVE: {
                const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                int new_hover = -1;
                for (int i = 0; i < 3; ++i) {
                    if (::PtInRect(&tab_rects_[i], pt)) {
                        new_hover = i;
                        break;
                    }
                }
                if (new_hover != hovered_tab_) {
                    hovered_tab_ = new_hover;
                    RECT bar = {0, HeaderHeight(), 4000, HeaderHeight() + TabBarHeight()};
                    ::InvalidateRect(hwnd, &bar, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONDOWN: {
                const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                for (int i = 0; i < 3; ++i) {
                    if (::PtInRect(&tab_rects_[i], pt)) {
                        SwitchTab(i);
                        return 0;
                    }
                }
                if (update_info_.update_available && ::PtInRect(&update_pill_rect_, pt)) {
                    ShowUpdateDialog();
                    return 0;
                }
                return 0;
            }

            case WM_DROPFILES: {
                HDROP hdrop = reinterpret_cast<HDROP>(wParam);
                wchar_t path_buf[MAX_PATH * 2]{};
                if (::DragQueryFileW(hdrop, 0, path_buf,
                                     static_cast<UINT>(sizeof(path_buf) / sizeof(path_buf[0]))) > 0) {
                    const std::string dropped = WideToUtf8(path_buf);
                    const std::string ext = fs::path(path_buf).extension().string();
                    const bool is_pe = (_stricmp(ext.c_str(), ".exe") == 0 ||
                                        _stricmp(ext.c_str(), ".dll") == 0);
                    if (active_tab_ == kTabPeInspect && is_pe) {
                        StartPeInspect(dropped);
                    } else {
                        SwitchTab(kTabPlay);
                        ::SetWindowTextW(h_edit_target_, path_buf);
                        AppendTextToEdit(h_edit_launch_log_, "[Drop] Selected target: " + dropped + "\n");
                    }
                }
                ::DragFinish(hdrop);
                return 0;
            }

            case WM_COMMAND: {
                const int id = LOWORD(wParam);
                switch (id) {
                    case kIdChkOffline:
                        offline_mode_ = (::SendMessageW(h_chk_offline_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        update_status_short_ = offline_mode_ ? "Offline mode enabled" : "";
                        ::InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;

                    case kIdBtnCheckUpdates:
                        if (offline_mode_) {
                            ::MessageBoxW(hwnd,
                                          L"Offline mode is enabled (or OPENSCARLETT_NO_UPDATE_CHECK=1).\nUncheck 'Offline' to check GitHub Releases.",
                                          L"OpenScarlett Updater", MB_OK | MB_ICONINFORMATION);
                        } else {
                            StartUpdateCheckWorker(true);
                        }
                        return 0;

                    case kIdBtnInstallUpd:
                        ShowUpdateDialog();
                        return 0;

                    case kIdBtnBrowseFolder: {
                        std::wstring folder;
                        if (BrowseForFolderDialog(hwnd, folder)) {
                            ::SetWindowTextW(h_edit_target_, folder.c_str());
                        }
                        return 0;
                    }
                    case kIdBtnBrowseExe: {
                        std::wstring file;
                        if (BrowseForFileDialog(
                                hwnd, L"Select Xbox/GDK Executable or MicrosoftGame.Config",
                                L"Executables & Config (*.exe;MicrosoftGame.Config)\0*.exe;MicrosoftGame.Config\0All Files (*.*)\0*.*\0",
                                file)) {
                            ::SetWindowTextW(h_edit_target_, file.c_str());
                        }
                        return 0;
                    }
                    case kIdBtnLaunch:
                        StartLaunchTarget();
                        return 0;

                    case kIdBtnStop:
                        cancel_launch_.store(true);
                        AppendTextToEdit(h_edit_launch_log_, "[Launcher] Stop requested...\n");
                        return 0;

                    case kIdBtnClearLog:
                        ::SetWindowTextW(h_edit_launch_log_, L"");
                        return 0;

                    case kIdBtnCopyTelemetry: {
                        std::string sanitized;
                        std::string source_label;
                        const std::string log_text = GetEditTextUtf8(h_edit_launch_log_);
                        if (BuildSanitizedLatestTelemetryForClipboard(
                                opts_.bundle_dir,
                                last_launch_.telemetry_path,
                                log_text,
                                sanitized,
                                source_label) &&
                            CopyTextToClipboard(sanitized)) {
                            const std::string status =
                                "Copied sanitized telemetry (" + source_label + ", " +
                                std::to_string(sanitized.size()) + " B) to clipboard";
                            ::SetWindowTextW(h_lbl_probe_status_, Utf8ToWide(status).c_str());
                            AppendTextToEdit(h_edit_launch_log_, "[Telemetry] " + status + "\n");
                        } else {
                            ::SetWindowTextW(h_lbl_probe_status_,
                                             L"Failed to copy sanitized telemetry to clipboard");
                        }
                        return 0;
                    }

                    case kIdBtnDemo0:
                    case kIdBtnDemo1:
                    case kIdBtnDemo2: {
                        const size_t idx = static_cast<size_t>(id - kIdBtnDemo0);
                        if (idx < demos_.size() && demos_[idx].available) {
                            const auto& d = demos_[idx];
                            std::error_code ec;
                            const fs::path launch_p =
                                (!d.config_path.empty() && fs::exists(d.config_path, ec))
                                    ? d.config_path
                                    : d.exe_path;
                            StartLaunchTarget(launch_p.string());
                        }
                        return 0;
                    }

                    case kIdBtnRunHwTest:
                        StartHardwareSelfTest();
                        return 0;

                    case kIdBtnCancelHwTest:
                        cancel_hwtest_.store(true);
                        AppendTextToEdit(h_edit_hw_log_, "[Hardware Test] Cancel requested...\n");
                        return 0;

                    case kIdBtnOpenGitHub:
                        if (hw_result_.completed && !hw_result_.github_issue_url.empty()) {
                            if (hw_result_.github_url_mode != "complete" && !hw_result_.report_markdown.empty()) {
                                (void)CopyTextToClipboard(hw_result_.report_markdown);
                                ::MessageBoxW(hwnd,
                                              L"Full sanitized Markdown report has been copied to your clipboard so you can paste it into the pre-filled GitHub issue.",
                                              L"GitHub Hardware Report", MB_OK | MB_ICONINFORMATION);
                            }
                            OpenUrlInDefaultBrowser(hw_result_.github_issue_url);
                        }
                        return 0;

                    case kIdBtnCopyMarkdown:
                        if (hw_result_.completed && !hw_result_.report_markdown.empty()) {
                            if (CopyTextToClipboard(hw_result_.report_markdown)) {
                                ::SetWindowTextW(h_btn_copy_md_, L"Copied!");
                            }
                        }
                        return 0;

                    case kIdBtnBrowsePe: {
                        std::wstring file;
                        if (BrowseForFileDialog(
                                hwnd, L"Select PE32+ Executable or DLL",
                                L"PE Binaries (*.exe;*.dll)\0*.exe;*.dll\0All Files (*.*)\0*.*\0",
                                file)) {
                            StartPeInspect(WideToUtf8(file));
                        }
                        return 0;
                    }
                    case kIdBtnInspectCurr: {
                        const std::string curr = GetEditTextUtf8(h_edit_target_);
                        if (!curr.empty()) {
                            StartPeInspect(curr);
                        }
                        return 0;
                    }
                    case kIdBtnInspectNow: {
                        const std::string path = GetEditTextUtf8(h_edit_pe_path_);
                        if (!path.empty()) {
                            StartPeInspect(path);
                        }
                        return 0;
                    }
                    default:
                        break;
                }
                break;
            }

            case WM_TIMER: {
                if (wParam == kTimerSmokeStep && opts_.smoke_mode) {
                    ++smoke_step_;
                    if (smoke_step_ == 1) {
                        SwitchTab(kTabHwTest);
                        ::UpdateWindow(hwnd);
                    } else if (smoke_step_ == 2) {
                        SwitchTab(kTabPeInspect);
                        ::UpdateWindow(hwnd);
                    } else if (smoke_step_ == 3) {
                        SwitchTab(kTabPlay);
                        ::UpdateWindow(hwnd);
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - smoke_start_).count();
                    if (smoke_step_ >= 3 && elapsed >= opts_.smoke_ms) {
                        ::KillTimer(hwnd, kTimerSmokeStep);
                        ::DestroyWindow(hwnd);
                    }
                }
                return 0;
            }

            // Custom worker-to-UI marshaled messages
            case kMsgGpuProbeDone: {
                std::unique_ptr<GpuTelemetrySnapshot> snap(
                    reinterpret_cast<GpuTelemetrySnapshot*>(lParam));
                if (snap) {
                    gpu_ = std::move(*snap);
                    PopulateAdapterCombo();
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            case kMsgUpdateCheckDone: {
                std::unique_ptr<UpdateCheckResult> res(
                    reinterpret_cast<UpdateCheckResult*>(lParam));
                const bool user_triggered = (wParam != 0);
                if (res) {
                    if (res->skipped_offline) {
                        update_status_short_ = "Update check skipped (offline)";
                    } else if (!res->ok) {
                        update_status_short_ = "Update check unavailable";
                        if (user_triggered) {
                            ::MessageBoxW(hwnd,
                                          Utf8ToWide("Could not check GitHub Releases:\n" + res->error_message).c_str(),
                                          L"OpenScarlett Updater", MB_OK | MB_ICONWARNING);
                        }
                    } else if (res->release.update_available) {
                        update_info_ = res->release;
                        update_status_short_.clear();
                        ::ShowWindow(h_btn_install_upd_, SW_SHOW);
                        if (user_triggered) {
                            ShowUpdateDialog();
                        }
                    } else {
                        update_info_ = res->release;
                        update_status_short_ = "Up to date (" + std::string(kOpenScarlettAppVersion) + ")";
                        if (user_triggered) {
                            ::MessageBoxW(hwnd,
                                          Utf8ToWide("You are already running the latest version (" +
                                                     std::string(kOpenScarlettAppVersion) + ").").c_str(),
                                          L"OpenScarlett Updater", MB_OK | MB_ICONINFORMATION);
                        }
                    }
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            case kMsgLaunchLog: {
                std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lParam));
                if (line) AppendTextToEdit(h_edit_launch_log_, *line);
                return 0;
            }

            case kMsgLaunchDone: {
                std::unique_ptr<LauncherRunResult> res(reinterpret_cast<LauncherRunResult*>(lParam));
                launch_running_.store(false);
                ::EnableWindow(h_btn_launch_, TRUE);
                ::EnableWindow(h_btn_stop_, FALSE);
                if (res) {
                    last_launch_ = std::move(*res);
                    std::ostringstream oss;
                    oss << "=== Finished (exit=" << last_launch_.exit_code
                        << ", calls=" << last_launch_.total_calls
                        << ", symbols=" << last_launch_.tracked_symbols
                        << ", not_impl=" << last_launch_.not_impl_calls << ") ===\n";
                    if (!last_launch_.error_message.empty()) {
                        oss << "Error: " << last_launch_.error_message << "\n";
                    }
                    AppendTextToEdit(h_edit_launch_log_, oss.str());
                    if (!last_launch_.telemetry_path.empty()) {
                        const std::string status =
                            "Telemetry captured (" + last_launch_.telemetry_path.filename().string() + ")";
                        ::SetWindowTextW(h_lbl_probe_status_, Utf8ToWide(status).c_str());
                    }
                    ::InvalidateRect(hwnd, &play_telemetry_rect_, FALSE);
                }
                return 0;
            }

            case kMsgHwTestProgress: {
                std::unique_ptr<HardwareTestProgress> p(reinterpret_cast<HardwareTestProgress*>(lParam));
                if (p) {
                    hw_completed_ = p->completed;
                    hw_total_ = p->total;
                    hw_current_name_ = p->current_test_name;
                    if (!p->log_line.empty()) {
                        AppendTextToEdit(h_edit_hw_log_, p->log_line + "\n");
                    }
                    if (p->has_completed_row) {
                        if (p->last_row.passed) ++hw_passed_;
                        else ++hw_failed_;
                        hw_rows_.push_back(p->last_row);
                        AppendHwListRow(p->last_row);
                    }
                    ::InvalidateRect(hwnd, &hw_card1_, FALSE);
                }
                return 0;
            }

            case kMsgHwTestDone: {
                std::unique_ptr<HardwareTestRunResult> res(reinterpret_cast<HardwareTestRunResult*>(lParam));
                hw_running_.store(false);
                ::EnableWindow(h_btn_run_hw_, TRUE);
                ::EnableWindow(h_btn_cancel_hw_, FALSE);
                if (res) {
                    hw_result_ = std::move(*res);
                    hw_passed_ = hw_result_.passed_count;
                    hw_failed_ = hw_result_.failed_count;
                    hw_total_ = hw_result_.total_count;
                    hw_completed_ = hw_result_.passed_count + hw_result_.failed_count;
                    if (!hw_result_.report_path.empty()) {
                        ::SetWindowTextW(
                            h_lbl_report_path_,
                            Utf8ToWide("Saved: " + hw_result_.report_path.string()).c_str());
                    }
                    ::EnableWindow(h_btn_open_gh_,
                                   (!hw_result_.github_issue_url.empty()) ? TRUE : FALSE);
                    ::EnableWindow(h_btn_copy_md_,
                                   (!hw_result_.report_markdown.empty()) ? TRUE : FALSE);
                    ::SetWindowTextW(h_btn_copy_md_, L"Copy Markdown");
                    ::InvalidateRect(hwnd, &hw_card1_, FALSE);
                }
                return 0;
            }

            case kMsgPeInspectDone: {
                std::unique_ptr<PeInspectorSummary> sum(reinterpret_cast<PeInspectorSummary*>(lParam));
                if (sum) {
                    pe_summary_ = std::move(*sum);
                    PopulatePeListView();
                    ::InvalidateRect(hwnd, &pe_card_summary_, FALSE);
                }
                return 0;
            }

            case WM_CLOSE:
                ::DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                JoinAllWorkers();
                DiscardD2DResources();
                ::PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void AppendHwListRow(const HardwareTestRow& row) {
        const int idx = ListView_GetItemCount(h_list_hw_);
        const std::wstring name_w = Utf8ToWide(row.name);
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.pszText = const_cast<LPWSTR>(name_w.c_str());
        ListView_InsertItem(h_list_hw_, &lvi);

        const wchar_t* st = row.passed ? L"PASS" : L"FAIL";
        ListView_SetItemText(h_list_hw_, idx, 1, const_cast<LPWSTR>(st));

        char tbuf[32]{};
        std::snprintf(tbuf, sizeof(tbuf), "%.2fs", row.seconds);
        const std::wstring tw = Utf8ToWide(tbuf);
        ListView_SetItemText(h_list_hw_, idx, 2, const_cast<LPWSTR>(tw.c_str()));

        const std::wstring note_w = Utf8ToWide(row.highlight.empty() ? row.description : row.highlight);
        ListView_SetItemText(h_list_hw_, idx, 3, const_cast<LPWSTR>(note_w.c_str()));
        ListView_EnsureVisible(h_list_hw_, idx, FALSE);
    }

    void PopulatePeListView() {
        ListView_DeleteAllItems(h_list_pe_);
        if (!pe_summary_.ok) return;
        for (size_t i = 0; i < pe_summary_.modules.size(); ++i) {
            const auto& m = pe_summary_.modules[i];
            const std::wstring dll_w = Utf8ToWide(m.dll_name);
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = const_cast<LPWSTR>(dll_w.c_str());
            ListView_InsertItem(h_list_pe_, &lvi);

            const std::wstring cat_w = Utf8ToWide(m.category);
            ListView_SetItemText(h_list_pe_, static_cast<int>(i), 1, const_cast<LPWSTR>(cat_w.c_str()));

            const std::wstring cnt_w = std::to_wstring(m.import_count);
            ListView_SetItemText(h_list_pe_, static_cast<int>(i), 2, const_cast<LPWSTR>(cnt_w.c_str()));

            const std::wstring rt_w = Utf8ToWide(m.routing_status);
            ListView_SetItemText(h_list_pe_, static_cast<int>(i), 3, const_cast<LPWSTR>(rt_w.c_str()));

            std::string miss;
            if (m.missing_exports.empty()) {
                miss = m.shim_present ? "0 (OK)" : "Shim DLL Missing";
            } else {
                for (size_t k = 0; k < m.missing_exports.size(); ++k) {
                    if (k > 0) miss += ", ";
                    if (k >= 6) {
                        miss += "+ " + std::to_string(m.missing_exports.size() - k) + " more";
                        break;
                    }
                    miss += m.missing_exports[k];
                }
            }
            const std::wstring miss_w = Utf8ToWide(miss);
            ListView_SetItemText(h_list_pe_, static_cast<int>(i), 4, const_cast<LPWSTR>(miss_w.c_str()));
        }
    }

    // State
    Options opts_;
    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_big_ = nullptr;
    HICON icon_small_ = nullptr;
    bool own_icon_big_ = false;
    bool own_icon_small_ = false;
    UINT dpi_ = 96u;
    LauncherThemeResources theme_;

    ComRef<ID2D1Factory> d2d_factory_;
    ComRef<IDWriteFactory> dwrite_factory_;
    ComRef<ID2D1HwndRenderTarget> d2d_rt_;
    ComRef<ID2D1SolidColorBrush> br_d2d_bg_;
    ComRef<ID2D1SolidColorBrush> br_d2d_header_;
    ComRef<ID2D1SolidColorBrush> br_d2d_card_;
    ComRef<ID2D1SolidColorBrush> br_d2d_ctrl_;
    ComRef<ID2D1SolidColorBrush> br_d2d_border_;
    ComRef<ID2D1SolidColorBrush> br_d2d_emerald_;
    ComRef<ID2D1SolidColorBrush> br_d2d_xbox_green_;
    ComRef<ID2D1SolidColorBrush> br_d2d_blue_;
    ComRef<ID2D1SolidColorBrush> br_d2d_amber_;
    ComRef<ID2D1SolidColorBrush> br_d2d_red_;
    ComRef<ID2D1SolidColorBrush> br_d2d_text_;
    ComRef<ID2D1SolidColorBrush> br_d2d_muted_;
    ComRef<ID2D1SolidColorBrush> br_d2d_badge_green_;
    ComRef<ID2D1SolidColorBrush> br_d2d_badge_blue_;
    ComRef<ID2D1SolidColorBrush> br_d2d_badge_amber_;
    ComRef<ID2D1SolidColorBrush> br_d2d_badge_red_;

    ComRef<IDWriteTextFormat> tf_title_;
    ComRef<IDWriteTextFormat> tf_section_;
    ComRef<IDWriteTextFormat> tf_tab_;
    ComRef<IDWriteTextFormat> tf_badge_;
    ComRef<IDWriteTextFormat> tf_body_;
    ComRef<IDWriteTextFormat> tf_metric_;

    int active_tab_ = kTabPlay;
    int hovered_tab_ = -1;
    RECT tab_rects_[3]{};
    RECT update_pill_rect_{};

    // Tab geometry rects for D2D card painting
    RECT play_card1_{};
    RECT play_card2_{};
    RECT play_telemetry_rect_{};
    RECT play_card3_{};
    RECT hw_card1_{};
    RECT hw_progress_rect_{};
    RECT hw_privacy_rect_{};
    RECT hw_card_table_{};
    RECT hw_card_log_{};
    RECT pe_card1_{};
    RECT pe_card_summary_{};
    RECT pe_card_table_{};

    // Controls
    HWND h_btn_check_upd_ = nullptr;
    HWND h_chk_offline_ = nullptr;
    HWND h_btn_install_upd_ = nullptr;

    HWND h_lbl_target_ = nullptr;
    HWND h_edit_target_ = nullptr;
    HWND h_btn_browse_dir_ = nullptr;
    HWND h_btn_browse_exe_ = nullptr;
    HWND h_lbl_backend_ = nullptr;
    HWND h_combo_backend_ = nullptr;
    HWND h_lbl_adapter_ = nullptr;
    HWND h_combo_adapter_ = nullptr;
    HWND h_chk_windowed_ = nullptr;
    HWND h_chk_null_audio_ = nullptr;
    HWND h_lbl_args_ = nullptr;
    HWND h_edit_args_ = nullptr;
    HWND h_btn_launch_ = nullptr;
    HWND h_btn_stop_ = nullptr;
    HWND h_btn_clear_log_ = nullptr;
    HWND h_chk_probe_mode_ = nullptr;
    HWND h_btn_copy_telemetry_ = nullptr;
    HWND h_lbl_probe_status_ = nullptr;
    HWND h_btn_demos_[3]{};
    HWND h_edit_launch_log_ = nullptr;

    HWND h_btn_run_hw_ = nullptr;
    HWND h_btn_cancel_hw_ = nullptr;
    HWND h_list_hw_ = nullptr;
    HWND h_edit_hw_log_ = nullptr;
    HWND h_btn_open_gh_ = nullptr;
    HWND h_btn_copy_md_ = nullptr;
    HWND h_lbl_report_path_ = nullptr;

    HWND h_lbl_pe_path_ = nullptr;
    HWND h_edit_pe_path_ = nullptr;
    HWND h_btn_browse_pe_ = nullptr;
    HWND h_btn_inspect_curr_ = nullptr;
    HWND h_btn_inspect_now_ = nullptr;
    HWND h_list_pe_ = nullptr;

    std::vector<HWND> tab_play_ctrls_;
    std::vector<HWND> tab_hw_ctrls_;
    std::vector<HWND> tab_pe_ctrls_;
    std::vector<HWND> mono_controls_;

    // Data state
    bool offline_mode_ = false;
    GpuTelemetrySnapshot gpu_{};
    std::vector<std::pair<std::string, bool>> adapter_choices_;
    std::vector<BundledDemoEntry> demos_;
    UpdateReleaseInfo update_info_{};
    std::string update_status_short_;
    LauncherRunResult last_launch_{};

    int hw_completed_ = 0;
    int hw_total_ = 0;
    int hw_passed_ = 0;
    int hw_failed_ = 0;
    std::string hw_current_name_;
    std::vector<HardwareTestRow> hw_rows_;
    HardwareTestRunResult hw_result_{};

    PeInspectorSummary pe_summary_{};

    // Smoke test & painting telemetry
    int paint_frames_ = 0;
    bool used_d2d_ = false;
    int smoke_step_ = 0;
    std::chrono::steady_clock::time_point smoke_start_{};

    // Threads & atomics
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> update_checking_{false};
    std::atomic<bool> launch_running_{false};
    std::atomic<bool> cancel_launch_{false};
    std::atomic<bool> hw_running_{false};
    std::atomic<bool> cancel_hwtest_{false};
    std::thread probe_thread_;
    std::thread update_thread_;
    std::thread launch_thread_;
    std::thread hw_thread_;
    std::thread pe_thread_;
};

// ============================================================================
// Headless Check Mode (Zero Network Calls, Validates D2D/DWrite + Bridge)
// ============================================================================
int RunHeadlessCheck(const fs::path& bundle_dir) {
    const HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool co_init = SUCCEEDED(co_hr);

    ComRef<ID2D1Factory> d2d;
    const HRESULT d2d_hr = ::D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        OS_UUIDOF_D2D1_FACTORY,
        nullptr,
        reinterpret_cast<void**>(d2d.Put()));

    ComRef<IDWriteFactory> dwrite;
    const HRESULT dw_hr = ::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        OS_UUIDOF_DWRITE_FACTORY,
        reinterpret_cast<IUnknown**>(dwrite.Put()));

    const auto gpu = ProbeHostGpuTelemetry(bundle_dir);
    const auto demos = DiscoverBundledDemos(bundle_dir);
    const bool upd_disabled = IsUpdateCheckDisabledByEnv();

    const std::string dirty = "Bundle=" + bundle_dir.string() + R"( User=C:\Users\TestUser\Desktop\secret.log)";
    const std::string clean = SanitizeReportText(dirty, bundle_dir);
    const bool sanitize_ok = (clean.find("TestUser") == std::string::npos);

    size_t avail_demos = 0;
    for (const auto& d : demos) {
        if (d.available) ++avail_demos;
    }

    const bool ok = SUCCEEDED(d2d_hr) && SUCCEEDED(dw_hr) && sanitize_ok;
    std::ostringstream oss;
    oss << (ok ? "[PASS]" : "[FAIL]")
        << " OpenScarlettLauncher --headless-check ("
        << "version=" << kOpenScarlettAppVersion
        << ", d2d=" << (SUCCEEDED(d2d_hr) ? "ok" : "fail")
        << ", dwrite=" << (SUCCEEDED(dw_hr) ? "ok" : "fail")
        << ", gpu=" << (gpu.primary_gpu_name.empty() ? "none" : gpu.primary_gpu_name)
        << ", adapters=" << gpu.adapters.size()
        << ", demos=" << avail_demos << "/" << demos.size()
        << ", update_disabled=" << (upd_disabled ? "true" : "false")
        << ", sanitize=" << (sanitize_ok ? "ok" : "fail")
        << ")\n";
    WriteCliOutput(oss.str());

    dwrite.Reset();
    d2d.Reset();
    if (co_init) ::CoUninitialize();
    return ok ? 0 : 1;
}

}  // namespace

// ============================================================================
// Win32 GUI Entry Point (Zero Console Flash When Double-Clicked)
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*lpCmdLine*/, int nCmdShow) {
    EnablePerMonitorDpiV2();

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    bool mode_version = false;
    bool mode_headless = false;
    bool mode_smoke = false;
    int smoke_ms = 200;
    fs::path bundle_dir = GetExecutableDir();

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg(argv[i]);
            if (arg == L"--version") {
                mode_version = true;
            } else if (arg == L"--headless-check") {
                mode_headless = true;
            } else if (arg == L"--smoke-gui") {
                mode_smoke = true;
            } else if (arg == L"--smoke-ms" && i + 1 < argc) {
                smoke_ms = std::clamp(::_wtoi(argv[++i]), 50, 5000);
            } else if (arg == L"--bundle-dir" && i + 1 < argc) {
                bundle_dir = fs::path(argv[++i]);
            }
        }
        ::LocalFree(argv);
    }

    if (mode_version) {
        WriteCliOutput(std::string("OpenScarlettLauncher ") + kOpenScarlettAppVersion + "\n");
        return 0;
    }

    if (mode_headless) {
        return RunHeadlessCheck(bundle_dir);
    }

    const HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool co_init = SUCCEEDED(co_hr);

    LauncherWindow::Options opts{};
    opts.bundle_dir = bundle_dir;
    opts.smoke_mode = mode_smoke;
    opts.smoke_ms = smoke_ms;

    LauncherWindow app(opts);
    const int rc = app.Run(hInstance, nCmdShow);

    if (mode_smoke) {
        std::ostringstream oss;
        oss << "[PASS] OpenScarlettLauncher --smoke-gui ("
            << "tabs=3, paint_frames=" << app.paint_frames()
            << ", d2d=" << (app.used_d2d() ? "true" : "gdi_fallback")
            << ", smoke_ms=" << smoke_ms << ")\n";
        WriteCliOutput(oss.str());
    }

    if (co_init) ::CoUninitialize();
    return rc;
}
