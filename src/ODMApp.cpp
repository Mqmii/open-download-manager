#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
#endif

#include "ODMApp.h"

#include "YtDlp.h"

#include <Ultralight/ConsoleMessage.h>
#include <Ultralight/View.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

// MediaInfoDLL: the vcpkg-built mediainfo.dll exports the plain C API only
// (the C++ MediaInfoLib::MediaInfo class is not exported), so we use the
// official wrapper that binds the same class interface to those C exports.
// _UNICODE makes the wrapper select the wide (W) variants, matching the
// UTF-16 paths we pass on Windows; it deliberately does NOT define UNICODE,
// so windows.h A/W macro resolution elsewhere is unaffected.
#ifndef _UNICODE
  #define _UNICODE
#endif
#include "MediaInfoDLL/MediaInfoDLL_Static.h"

#if defined(_WIN32)
  #include <windows.h>
  #include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
  #include <shellapi.h>
  #include <commdlg.h>
  #include <shlobj.h>      // SHGetKnownFolderPath
  #include <knownfolders.h> // FOLDERID_Downloads
  #include <dwmapi.h>       // DwmSetWindowAttribute
  #include <timeapi.h>      // timeBeginPeriod / timeEndPeriod
  #pragma comment(lib, "comdlg32.lib")
  #pragma comment(lib, "shell32.lib")
  #pragma comment(lib, "dwmapi.lib")
  #pragma comment(lib, "winmm.lib")
  #endif

namespace odm {

namespace fs = std::filesystem;
//make bytes human readable.
static std::string HumanSize(double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1024.0 && u < 4) { bytes /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << bytes << " " << units[u];
    return os.str();
}

static std::string EscapeJS(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'";  break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// Convert ultralight::JSString (from JSValue::ToString()) to std::string.
static std::string ToStdStr(ultralight::JSString js) {
    ultralight::String s = js;          // JSString -> ultralight::String
    const char* utf8 = s.utf8().data();
    return utf8 ? std::string(utf8) : std::string(); // guard against nullptr (UB)
}

#if defined(_WIN32)
// UTF-8 <-> UTF-16 helpers so native dialogs handle non-ASCII paths
// (e.g. localized Windows folder names) without mojibake.
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

static std::string WideToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}
#endif

// Native save-file dialog. Returns empty string if cancelled.
static std::string NativeSaveDialog(const std::string& suggested_name) {
#if defined(_WIN32)
    std::wstring wname = Utf8ToWide(suggested_name);
    if (wname.size() >= MAX_PATH) wname.resize(MAX_PATH - 1);
    wchar_t buf[MAX_PATH] = {0};
    for (size_t i = 0; i < wname.size(); ++i) buf[i] = wname[i];

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Save downloaded file as";
    ofn.lpstrFilter = L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&ofn)) return WideToUtf8(buf);
    return std::string();
#else
    (void)suggested_name;
    return std::string();
#endif
}

// Return the user's OS Downloads folder, or "" on failure.
static std::string GetDownloadsFolder() {
#if defined(_WIN32)
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) {
        std::wstring ws(path);
        CoTaskMemFree(path);
        // Convert wide string to UTF-8.
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string out(static_cast<size_t>(len) - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                out.data(), len, nullptr, nullptr);
            return out;
        }
    }
    return "";
#else
    // POSIX / macOS fallback: ~/Downloads
    const char* home = std::getenv("HOME");
    if (!home) return "";
    fs::path p = fs::path(home) / "Downloads";
    if (fs::is_directory(p)) return p.string();
    return "";
#endif
}

#if defined(_WIN32)
// --- System tray (minimize-to-tray) ---------------------------------------
// Pressing the title-bar (X) hides the window to the notification area instead
// of quitting. The tray icon is owned by a *dedicated thread* with its own
// message loop: AppCore's internal run loop does not reliably dispatch
// WM_APP-range callback messages to the main window, so a click on a tray icon
// hosted by the main window would never be delivered. A separate hidden window
// on its own thread guarantees the tray notifications are handled.
//
// Left-clicking the tray icon restores the main window; right-clicking shows a
// menu whose "Quit" item performs the real shutdown.
//
// Two rules keep the tray glitch-free:
//  1. The WM_ODM_TRAY notification handler returns immediately — all real
//     work (window creation, SetForegroundWindow) is deferred through
//     WM_ODM_TRAY_OPEN / WM_ODM_TRAY_MENU so the shell is never kept waiting
//     (otherwise Windows paints the "not responding" busy cursor over the
//     tray icon).
//  2. The popup menu is a normally-activated topmost window dismissed via
//     WM_ACTIVATE(WA_INACTIVE), like a real menu — no mouse capture, so the
//     first click on an item always registers.
static const UINT   WM_ODM_TRAY      = WM_APP + 1;
// Deferred work messages, posted by the WM_ODM_TRAY handler to itself so the
// shell's notification callback returns immediately (see TrayWndProc).
static const UINT   WM_ODM_TRAY_OPEN = WM_APP + 2;  // restore the main window
static const UINT   WM_ODM_TRAY_MENU = WM_APP + 3;  // show the context menu
static const UINT   ID_TRAY_OPEN  = 1001;
static const UINT   ID_TRAY_QUIT  = 1002;
static const UINT   ODM_TRAY_UID  = 1;
static const UINT   ID_TRAY_SEP   = 1003;   // owner-draw separator id

// Fallbacks for older SDK headers (NOTIFYICON_VERSION_4 event codes).
#ifndef NIN_SELECT
  #define NIN_SELECT    (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
  #define NIN_KEYSELECT (WM_USER + 1)
#endif
#ifndef NOTIFYICON_VERSION_4
  #define NOTIFYICON_VERSION_4 4
#endif

static WNDPROC g_orig_wndproc = nullptr;  // Ultralight's original window proc
static HWND    g_main_hwnd    = nullptr;  // the Ultralight main window
static HWND    g_tray_wnd     = nullptr;  // hidden window owned by tray thread
static HWND    g_menu_wnd     = nullptr;  // active dark-menu popup (tray thread)
static DWORD   g_tray_tid     = 0;        // tray thread id (for WM_QUIT)
static std::atomic<bool> g_tray_added{false};
static std::atomic<bool> g_real_quit{false};    // set when the user chooses "Quit"

static void AddTrayIcon(HWND owner) {
    if (g_tray_added) return;

    // Reuse the main window's icon (set via WM_SETICON at startup); fall back
    // to loading directly from the embedded resource, then the Windows default.
    // NOTE: this runs on the tray thread, so never use a bare SendMessage to
    // the main window — a busy main thread would wedge the tray (and Windows
    // would show the "not responding" busy cursor over the tray icon).
    // SendMessageTimeout with SMTO_ABORTIFHUNG keeps us responsive.
    HICON icon = nullptr;
    DWORD_PTR iconRes = 0;
    if (SendMessageTimeoutW(g_main_hwnd, WM_GETICON, ICON_SMALL, 0,
                            SMTO_ABORTIFHUNG, 150, &iconRes) && iconRes)
        icon = reinterpret_cast<HICON>(iconRes);
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(g_main_hwnd, GCLP_HICONSM));
    iconRes = 0;
    if (!icon && SendMessageTimeoutW(g_main_hwnd, WM_GETICON, ICON_BIG, 0,
                                     SMTO_ABORTIFHUNG, 150, &iconRes) && iconRes)
        icon = reinterpret_cast<HICON>(iconRes);
    if (!icon)
        icon = reinterpret_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
    if (!icon)
        icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));

    NOTIFYICONDATAW nid = {0};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = owner;
    nid.uID              = ODM_TRAY_UID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_ODM_TRAY;
    nid.hIcon            = icon;
    wcscpy_s(nid.szTip, L"ODM - Open Download Manager");

    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_tray_added = true;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

static void RemoveTrayIcon(HWND owner) {
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = owner;
    nid.uID    = ODM_TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = false;
}

// Bring the main window back from the tray. Called on the tray thread, so we
// use the async variants and best-effort foreground activation.
static void ShowMainWindowFromTray() {
    if (!g_main_hwnd) return;
    if (IsIconic(g_main_hwnd))
        ShowWindowAsync(g_main_hwnd, SW_RESTORE);
    else
        ShowWindowAsync(g_main_hwnd, SW_SHOW);
    // Allow the main window's thread to take the foreground, then request it.
    DWORD main_tid = GetWindowThreadProcessId(g_main_hwnd, nullptr);
    AllowSetForegroundWindow(ASFW_ANY);
    SetForegroundWindow(g_main_hwnd);
    (void)main_tid;
}

// Trigger the real shutdown from the tray thread: post WM_CLOSE to the main
// window (a standard-range message AppCore *does* dispatch) so Ultralight tears
// down cleanly, then end this thread's own message loop.
static void RequestQuitFromTray() {
    g_real_quit = true;
    RemoveTrayIcon(g_tray_wnd);
    if (g_main_hwnd) PostMessageW(g_main_hwnd, WM_CLOSE, 0, 0);
    PostQuitMessage(0);
}

// --- Custom dark popup menu (no TrackPopupMenu) ---------------------------
// Discord/Spotify style: we create our own borderless popup window and draw
// everything ourselves. This is the only reliable way to get rid of
// TrackPopupMenu's white system border.
// Tray menu colours, kept in step with the UI palette in app.css
// (--bg-panel / --bg-hover / --text-primary / --border-color).
static const COLORREF kMenuBg       = RGB(27, 30, 37);    // #1B1E25
static const COLORREF kMenuBgHover  = RGB(42, 47, 57);    // #2A2F39
static const COLORREF kMenuText     = RGB(223, 227, 234); // #DFE3EA
static const COLORREF kMenuSep      = RGB(48, 53, 64);
static const COLORREF kMenuBorder   = RGB(56, 62, 74);

// Base metrics at 96 DPI (100% scaling), tuned to match native Win32 context
// menus. Every value is scaled to the target monitor's DPI at show time (see
// ShowDarkTrayMenu) so the popup matches the size of other apps' tray menus.
static const int kMenuItemH96     = 24;   // per-item height
static const int kMenuSepH96      = 7;    // separator strip height
static const int kMenuPadding96   = 2;    // top/bottom padding
static const int kMenuRadius96    = 8;    // corner radius
static const int kMenuTextLeft96  = 12;   // left text inset
static const int kMenuTextRight96 = 24;   // right inset (used for width calc)
static const int kMenuMinWidth96  = 120;  // minimum popup width

static const wchar_t* kDarkMenuClass = L"ODM_DarkTrayMenu";
static ATOM g_dark_menu_atom = 0;

struct DarkMenuItem {
    UINT id;
    std::wstring label;
    bool separator;
};

struct DarkMenuState {
    std::vector<DarkMenuItem> items;
    int  hoverIndex = -1;
    HWND owner = nullptr;     // tray window to post WM_COMMAND to

    // DPI-scaled metrics, computed once in ShowDarkTrayMenu.
    int   itemH    = kMenuItemH96;
    int   sepH     = kMenuSepH96;
    int   padding  = kMenuPadding96;
    int   radius   = kMenuRadius96;
    int   textLeft = kMenuTextLeft96;
    int   width    = kMenuMinWidth96;
    HFONT font     = nullptr;  // system menu font at this DPI; freed on destroy
};

static int ItemHeight(const DarkMenuState* st, const DarkMenuItem& it) {
    return it.separator ? st->sepH : st->itemH;
}

// Which item is at client Y? -1 if none (padding or separator gap).
static int ItemAtY(const DarkMenuState* st, int y) {
    int top = st->padding;
    for (int i = 0; i < (int)st->items.size(); ++i) {
        int h = ItemHeight(st, st->items[i]);
        if (y >= top && y < top + h) return i;
        top += h;
    }
    return -1;
}

static void RegisterDarkMenuClass() {
    if (g_dark_menu_atom) return;
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;  // subclassed per-instance below
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = nullptr;          // we paint everything
    wc.lpszClassName = kDarkMenuClass;
    g_dark_menu_atom = RegisterClassExW(&wc);
}

static void PaintDarkMenu(HWND hwnd, DarkMenuState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;

    // Filled dark background.
    HBRUSH bg = CreateSolidBrush(kMenuBg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Thin border (drawn as a rounded rect outline).
    HPEN pen = CreatePen(PS_SOLID, 1, kMenuBorder);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockBrush(NULL_BRUSH));
    HRGN clip = CreateRoundRectRgn(0, 0, w + 1, h + 1, st->radius, st->radius);
    SelectClipRgn(hdc, clip);
    RoundRect(hdc, 0, 0, w - 1, h - 1, st->radius, st->radius);
    SelectClipRgn(hdc, nullptr);
    DeleteObject(clip);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    // Labels use the system menu font stored on the state (at the target DPI).
    HFONT oldFont = st->font ? (HFONT)SelectObject(hdc, st->font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);

    int y = st->padding;
    for (int i = 0; i < (int)st->items.size(); ++i) {
        const auto& it = st->items[i];
        int ih = ItemHeight(st, it);
        RECT r{0, y, w, y + ih};

        if (it.separator) {
            HPEN sp = CreatePen(PS_SOLID, 1, kMenuSep);
            HPEN op = (HPEN)SelectObject(hdc, sp);
            int my = y + ih / 2;
            MoveToEx(hdc, st->textLeft, my, nullptr);
            LineTo(hdc, w - st->textLeft, my);
            SelectObject(hdc, op);
            DeleteObject(sp);
        } else {
            if (i == st->hoverIndex) {
                HBRUSH hb = CreateSolidBrush(kMenuBgHover);
                FillRect(hdc, &r, hb);
                DeleteObject(hb);
            }
            SetTextColor(hdc, kMenuText);
            RECT rt = r;
            rt.left += st->textLeft;
            DrawTextW(hdc, it.label.c_str(), -1, &rt,
                      DT_VCENTER | DT_SINGLELINE | DT_LEFT);
        }
        y += ih;
    }

    if (oldFont) SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK DarkMenuProc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp) {
    DarkMenuState* st = reinterpret_cast<DarkMenuState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_PAINT:
            if (st) PaintDarkMenu(hwnd, st);
            return 0;

        case WM_ERASEBKGND:
            return 1;   // we paint everything in WM_PAINT

        case WM_MOUSEMOVE: {
            if (!st) break;
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = ItemAtY(st, y);
            // Skip separators — they are not selectable.
            if (idx >= 0 && st->items[idx].separator) idx = -1;
            if (idx != st->hoverIndex) {
                st->hoverIndex = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // Track mouse leave so we can clear hover when the cursor exits.
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (st && st->hoverIndex != -1) {
                st->hoverIndex = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        // The popup owns no class cursor logic of its own beyond IDC_ARROW;
        // answer WM_SETCURSOR explicitly so the cursor never falls through
        // to whatever shape it previously had (which is how the "busy"
        // cursor used to leak onto the menu).
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
            return TRUE;

        // Dismiss exactly like a real menu: when the user clicks anywhere
        // else, another window becomes active and we get WA_INACTIVE.
        // (This replaces the old SetCapture approach, which left the popup
        // shown-but-not-active so the first click was eaten by activation.)
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_LBUTTONUP: {
            if (!st) break;
            int y = GET_Y_LPARAM(lp);
            int idx = ItemAtY(st, y);
            if (idx >= 0 && !st->items[idx].separator) {
                UINT id = st->items[idx].id;
                // Post command to the tray owner window (same IDs as before
                // so the existing WM_COMMAND handler picks it up).
                if (st->owner)
                    PostMessageW(st->owner, WM_COMMAND, id, 0);
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_RBUTTONUP:
            // Right-click anywhere on the popup also dismisses it.
            DestroyWindow(hwnd);
            return 0;

        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (!st) break;
            // Arrow keys move the highlight across selectable items...
            if (wp == VK_UP || wp == VK_DOWN) {
                int n = (int)st->items.size();
                int dir = (wp == VK_DOWN) ? 1 : -1;
                int idx = st->hoverIndex;
                for (int step = 0; step < n; ++step) {
                    idx = (idx + dir + n) % n;
                    if (!st->items[idx].separator) break;
                }
                if (idx != st->hoverIndex && !st->items[idx].separator) {
                    st->hoverIndex = idx;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            // ...and Enter activates the highlighted item.
            if (wp == VK_RETURN) {
                int idx = st->hoverIndex;
                if (idx >= 0 && !st->items[idx].separator && st->owner)
                    PostMessageW(st->owner, WM_COMMAND, st->items[idx].id, 0);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            if (g_menu_wnd == hwnd) g_menu_wnd = nullptr;
            if (st) {
                // Clear the GWLP_USERDATA before deleting so any reentrant
                // message during destruction doesn't dereference freed memory.
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                if (st->font) DeleteObject(st->font);
                delete st;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Per-monitor DPI of the monitor a window currently occupies. The value honors
// the window's DPI-awareness context, so it stays consistent with how the shell
// scales the window (no double-scaling). Falls back to the device DPI, then 96.
static UINT MenuDpiForWindow(HWND hwnd) {
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    static GetDpiForWindowFn pGetDpiForWindow =
        reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (pGetDpiForWindow) {
        UINT dpi = pGetDpiForWindow(hwnd);
        if (dpi) return dpi;
    }
    UINT dpi = 96;
    if (HDC dc = GetDC(hwnd)) {
        int lp = GetDeviceCaps(dc, LOGPIXELSX);
        if (lp) dpi = (UINT)lp;
        ReleaseDC(hwnd, dc);
    }
    return dpi;
}

// The exact system menu font at the given DPI, so labels match native menus.
static HFONT CreateMenuFontForDpi(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    using SPIForDpiFn = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static SPIForDpiFn pSPIForDpi =
        reinterpret_cast<SPIForDpiFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));

    bool ok = false;
    if (pSPIForDpi)
        ok = pSPIForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi) != FALSE;
    if (!ok &&
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        // Older OS: metrics come back at the system DPI; rescale to target.
        UINT sysDpi = 96;
        if (HDC dc = GetDC(nullptr)) {
            int lp = GetDeviceCaps(dc, LOGPIXELSY);
            if (lp) sysDpi = (UINT)lp;
            ReleaseDC(nullptr, dc);
        }
        if (sysDpi)
            ncm.lfMenuFont.lfHeight =
                MulDiv(ncm.lfMenuFont.lfHeight, (int)dpi, (int)sysDpi);
        ok = true;
    }
    if (ok)
        return CreateFontIndirectW(&ncm.lfMenuFont);

    // Last resort: Segoe UI at ~9pt for this DPI.
    int h = -MulDiv(9, (int)dpi, 72);
    return CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// Show the custom dark popup at screen point pt; commands go to `owner`.
// Only one menu may exist at a time: if the shell delivers duplicate
// notifications (e.g. both WM_CONTEXTMENU and a legacy WM_RBUTTONUP), the
// old popup is replaced instead of stacking an identical copy underneath —
// stacked copies were the reason the menu appeared to "not disappear" on the
// first click.
static void ShowDarkTrayMenu(HWND owner, POINT pt) {
    RegisterDarkMenuClass();

    if (g_menu_wnd && IsWindow(g_menu_wnd))
        DestroyWindow(g_menu_wnd);
    g_menu_wnd = nullptr;

    auto* st = new DarkMenuState();
    st->owner = owner;
    st->items.push_back({ID_TRAY_OPEN, L"Open ODM", false});
    st->items.push_back({ID_TRAY_SEP,  L"",         true});
    st->items.push_back({ID_TRAY_QUIT, L"Quit",     false});

    // Create the popup first (hidden) so we can query the DPI of the monitor it
    // actually lands on, then size everything to match native menus. The tray
    // thread is Per-Monitor-Aware (see TrayThreadMain), so GetDpiForWindow
    // returns the true per-monitor DPI and the popup is rendered crisply rather
    // than being bitmap-stretched (over-enlarged) by the shell.
    HWND popup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kDarkMenuClass, L"",
        WS_POPUP,
        pt.x, pt.y, kMenuMinWidth96, kMenuItemH96,
        nullptr, nullptr, GetModuleHandleW(nullptr),
        nullptr);
    if (!popup) { delete st; return; }

    // Attach state and subclass the proc (so each instance has its own
    // GWLP_USERDATA-backed DarkMenuState without a global).
    SetWindowLongPtrW(popup, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    SetWindowLongPtrW(popup, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(DarkMenuProc));

    // Resolve the popup's DPI and derive every metric from it.
    UINT dpi = MenuDpiForWindow(popup);
    st->sepH     = MulDiv(kMenuSepH96,     (int)dpi, 96);
    st->padding  = MulDiv(kMenuPadding96,  (int)dpi, 96);
    st->radius   = MulDiv(kMenuRadius96,   (int)dpi, 96);
    st->textLeft = MulDiv(kMenuTextLeft96, (int)dpi, 96);
    st->font     = CreateMenuFontForDpi(dpi);

    // Measure the system menu font to size rows and width like a native menu.
    int textH = 0, maxTextW = 0;
    if (HDC dc = GetDC(popup)) {
        HFONT oldF = st->font ? (HFONT)SelectObject(dc, st->font) : nullptr;
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) textH = tm.tmHeight;
        for (const auto& it : st->items) {
            if (it.separator) continue;
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, it.label.c_str(),
                                      (int)it.label.size(), &sz) && sz.cx > maxTextW)
                maxTextW = sz.cx;
        }
        if (oldF) SelectObject(dc, oldF);
        ReleaseDC(popup, dc);
    }
    int rowMin = MulDiv(kMenuItemH96, (int)dpi, 96);
    int rowFit = textH + MulDiv(8, (int)dpi, 96);
    st->itemH  = rowMin > rowFit ? rowMin : rowFit;

    int width = maxTextW + st->textLeft + MulDiv(kMenuTextRight96, (int)dpi, 96);
    int minW  = MulDiv(kMenuMinWidth96, (int)dpi, 96);
    if (width < minW) width = minW;
    st->width = width;

    // Total height from the (now DPI-scaled) metrics.
    int h = st->padding * 2;
    for (const auto& it : st->items)
        h += ItemHeight(st, it);

    // Keep the menu on-screen relative to the monitor at the cursor.
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (pt.x + width > mi.rcMonitor.right)  pt.x = mi.rcMonitor.right - width;
    if (pt.y + h     > mi.rcMonitor.bottom) pt.y = mi.rcMonitor.bottom - h;
    if (pt.x < mi.rcMonitor.left)           pt.x = mi.rcMonitor.left;
    if (pt.y < mi.rcMonitor.top)            pt.y = mi.rcMonitor.top;

    // Apply the final position/size and the rounded-corner region.
    SetWindowPos(popup, nullptr, pt.x, pt.y, width, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    HRGN rgn = CreateRoundRectRgn(0, 0, width + 1, h + 1,
                                  st->radius, st->radius);
    SetWindowRgn(popup, rgn, TRUE);

    g_menu_wnd = popup;

    // Behave like a real menu: become the foreground/active window. This is
    // safe here because we are NOT inside the shell's tray notification
    // callback (TrayWndProc defers to WM_ODM_TRAY_MENU), and the user just
    // produced an input event, so the foreground lock is satisfied. Once
    // active, losing activation (click anywhere else) dismisses the menu via
    // WM_ACTIVATE — no mouse capture needed.
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    SetForegroundWindow(popup);
    SetActiveWindow(popup);
}

// Window procedure for the hidden tray-owner window (runs on the tray thread).
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ODM_TRAY: {
            // The shell delivers this notification synchronously (explorer is
            // waiting on us). Do NO real work here: window creation and
            // SetForegroundWindow from inside this callback contend with
            // explorer's own foreground-thread handling, which is exactly
            // what produced the "app not responding" busy cursor over the
            // tray. Post a deferred message to our own queue and return at
            // once; the deferred handler below runs outside the callback.
            UINT event = LOWORD(lParam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT ||
                event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
                PostMessageW(hwnd, WM_ODM_TRAY_OPEN, 0, 0);
            } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                PostMessageW(hwnd, WM_ODM_TRAY_MENU, 0,
                             MAKELPARAM(pt.x, pt.y));
            }
            return 0;
        }

        case WM_ODM_TRAY_OPEN:
            ShowMainWindowFromTray();
            return 0;

        case WM_ODM_TRAY_MENU: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ShowDarkTrayMenu(hwnd, pt);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_OPEN:
                    ShowMainWindowFromTray();
                    return 0;
                case ID_TRAY_QUIT:
                    RequestQuitFromTray();
                    return 0;
            }
            break;

        case WM_DESTROY:
            RemoveTrayIcon(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Entry point for the dedicated tray thread: creates a hidden window, adds the
// tray icon, and pumps its own (unfiltered) message loop.
static void TrayThreadMain() {
    g_tray_tid = GetCurrentThreadId();

    // Render this thread's popups Per-Monitor-Aware so the tray menu is sized by
    // the real monitor DPI and drawn crisply, instead of being bitmap-stretched
    // (over-enlarged/blurry) by the shell. This is per-thread, so the main
    // Ultralight window/thread is unaffected. The literal PER_MONITOR_AWARE_V2
    // handle (-4) avoids depending on newer SDK headers.
    using SetThreadDpiFn = HANDLE (WINAPI*)(HANDLE);
    if (auto pSetThreadDpi = reinterpret_cast<SetThreadDpiFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
        pSetThreadDpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));

    const wchar_t* kClass = L"ODM_TrayHiddenWindow";
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TrayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    g_tray_wnd = CreateWindowExW(0, kClass, L"ODM Tray",
                                 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr,
                                 wc.hInstance, nullptr);
    if (!g_tray_wnd) return;

    AddTrayIcon(g_tray_wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon(g_tray_wnd);
    DestroyWindow(g_tray_wnd);
    g_tray_wnd = nullptr;
}

// Main-window subclass: only intercepts the title-bar (X) to hide to tray.
static LRESULT CALLBACK ODMWndProc(HWND hwnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE && !g_real_quit) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wParam, lParam);
}
#endif // _WIN32

ODMApp::ODMApp() {
    using namespace ultralight;

#if defined(_WIN32)
    // Raise the system timer resolution to 1ms for the lifetime of the app.
    //
    // Windows' default timer granularity is 15.6ms, so Ultralight's 16.67ms
    // animation timer rounds up to 15.6 or 31.2ms: measured in-app, CSS
    // animations and requestAnimationFrame ran at ~49 Hz with regular 30-35ms
    // steps. With 1ms resolution they tick evenly (browsers request the same
    // resolution for exactly this reason).
    //
    // NOTE: this does NOT lift the render frame rate. The bundled Ultralight
    // is the Free edition, which caps every View at 60 FPS
    // (ULTRALIGHT_EDITION_MAX_FPS in Ultralight/Config.h). On a 100 Hz display
    // a 16.67ms cap lands on every second refresh, so the window renders a
    // steady ~50 FPS while the same page in a browser runs at 100. That gap is
    // the drag "smoothness" difference and it cannot be closed from our side;
    // paint cost is only ~3.4ms of each ~20ms frame.
    timeBeginPeriod(1);
#endif

    app_ = App::Create();
    app_->set_listener(this);

    window_ = Window::Create(app_->main_monitor(), 1100, 720, false,
                             kWindowFlags_Titled | kWindowFlags_Resizable |
                             kWindowFlags_Maximizable);
    window_->SetTitle("ODM - Open Download Manager");
    window_->set_listener(this);

#if defined(_WIN32)
    // Enable dark title bar on Windows 10/11.
    HWND hwnd = static_cast<HWND>(window_->native_handle());
    if (hwnd) {
        BOOL dark = TRUE;
        // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win 10 20H1+) or 19 (older).
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

        // DWMWA_CAPTION_COLOR = 35 (Win 11 22000+): set the title bar color
        // to match --bg-main so they look unified.
        COLORREF caption_color = RGB(21, 23, 28); // #15171C, == --bg-main
        DwmSetWindowAttribute(hwnd, 35, &caption_color, sizeof(caption_color));

        // Add the minimize button (Ultralight's WindowFlags has no kWindowFlags_Minimizable).
        {
            LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if (!(style & WS_MINIMIZEBOX))
                SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX);
        }

        // Set a dark background brush to prevent white flash on resize.
        HBRUSH bg = CreateSolidBrush(RGB(21, 23, 28)); // #15171C, == --bg-main
        HBRUSH old = (HBRUSH)SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)bg);
        if (old) DeleteObject(old);

        // Load and apply the app icon from the embedded resource (icon.ico,
        // resource ID 1 in app.rc). Ultralight's window class has no icon of
        // its own, so without this the title bar, Alt+Tab, and system tray
        // all show the generic Windows default icon.
        HMODULE hMod = GetModuleHandleW(nullptr);
        HICON hIconSmall = reinterpret_cast<HICON>(LoadImageW(
            hMod, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
        HICON hIconBig = reinterpret_cast<HICON>(LoadImageW(
            hMod, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR));
        if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
        if (hIconBig)   SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);

        // Minimize-to-tray: subclass the window so the title-bar (X) hides the
        // window to the system tray instead of quitting.
        g_main_hwnd = hwnd;
        g_orig_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(ODMWndProc)));

        // Own the tray icon on a dedicated thread with its own message loop so
        // its click notifications are always delivered (AppCore's run loop does
        // not reliably dispatch WM_APP-range messages to the main window).
        tray_thread_ = std::thread(TrayThreadMain);
    }
#endif

    overlay_ = Overlay::Create(window_, window_->width(), window_->height(), 0, 0);
    overlay_->view()->set_view_listener(this);
    overlay_->view()->set_load_listener(this);

    // Bind engine callbacks.
    downloader_.SetPartCount(8);
    downloader_.SetProgressCallback(
        [this](const ProgressInfo& p) { OnProgress(p); });
    downloader_.SetCompletionCallback(
        [this](const DownloadResult& r) { OnComplete(r); });
    // When the engine finalizes the output path (e.g. after appending a
    // server-suggested extension), tell the UI so it can persist the real
    // path for a later resume.
    downloader_.SetPathResolvedCallback(
        [this](const std::string& id, const std::string& path) {
            std::ostringstream js;
            js << "if (window.UI && UI.onPath) UI.onPath('"
               << EscapeJS(id) << "', '" << EscapeJS(path) << "');";
            PostJS(js.str());
        });
    // HLS engine: identical callback surface, HLS-specific part count.
    hls_.SetPartCount(4);
    hls_.SetProgressCallback(
        [this](const ProgressInfo& p) { OnProgress(p); });
    hls_.SetCompletionCallback(
        [this](const DownloadResult& r) { OnComplete(r); });
    hls_.SetPathResolvedCallback(
        [this](const std::string& id, const std::string& path) {
            std::ostringstream js;
            js << "if (window.UI && UI.onPath) UI.onPath('"
               << EscapeJS(id) << "', '" << EscapeJS(path) << "');";
            PostJS(js.str());
        });


    // Paired-track DASH: same callback surface, same part count as plain HTTP
    // (both tracks are ordinary Range-capable files).
    dash_.SetPartCount(8);
    dash_.SetProgressCallback(
        [this](const ProgressInfo& p) { OnProgress(p); });
    dash_.SetCompletionCallback(
        [this](const DownloadResult& r) { OnComplete(r); });
    dash_.SetPathResolvedCallback(
        [this](const std::string& id, const std::string& path) {
            std::ostringstream js;
            js << "if (window.UI && UI.onPath) UI.onPath('"
               << EscapeJS(id) << "', '" << EscapeJS(path) << "');";
            PostJS(js.str());
        });

    // YouTube fallback engine: single-connection, but the same callbacks, so
    // the UI cannot tell which engine is behind a row.
    ytdlp_.SetProgressCallback(
        [this](const ProgressInfo& p) { OnProgress(p); });
    ytdlp_.SetCompletionCallback(
        [this](const DownloadResult& r) { OnComplete(r); });
    ytdlp_.SetPathResolvedCallback(
        [this](const std::string& id, const std::string& path) {
            std::ostringstream js;
            js << "if (window.UI && UI.onPath) UI.onPath('"
               << EscapeJS(id) << "', '" << EscapeJS(path) << "');";
            PostJS(js.str());
        });
    // An out-of-date yt-dlp is the one thing that reliably breaks YouTube
    // support months after a release. Checked at most weekly, off-thread,
    // and silent either way — nothing here depends on it succeeding.
    ytdlp::SelfUpdateAsync();

    overlay_->view()->LoadURL("file:///index.html");

    // Start the browser bridge (Chrome extension -> this app). If the port is
    // unavailable the app simply runs without the bridge.
#if defined(_WIN32)
    hwnd_ = window_->native_handle();
#endif
    bridge_.Start(47923, [this](const BridgePayload& p) {
        OnExternalDownload(p);
    });
}

ODMApp::~ODMApp() {
#if defined(_WIN32)
    // Ensure the tray thread has exited (normally it stops during OnClose).
    if (tray_thread_.joinable()) {
        if (g_tray_tid) PostThreadMessageW(g_tray_tid, WM_QUIT, 0, 0);
        tray_thread_.join();
    }
    timeEndPeriod(1);   // release the 1ms timer resolution taken in the ctor
#endif
}

void ODMApp::Run() { app_->Run(); }

// --- Window / view / app listeners ----------------------------------------

void ODMApp::OnClose(ultralight::Window*) {
#if defined(_WIN32)
    // Stop the tray thread (it removes the icon as it exits) and join it.
    if (tray_thread_.joinable()) {
        if (g_tray_tid) PostThreadMessageW(g_tray_tid, WM_QUIT, 0, 0);
        tray_thread_.join();
    }
#endif
    // Stop the bridge first so no new hand-offs arrive while we tear down.
    bridge_.Stop();
    downloader_.Stop();
    hls_.Stop();
    dash_.Stop();
    ytdlp_.Stop();
    downloader_.WaitForCompletion();
    hls_.WaitForCompletion();
    dash_.WaitForCompletion();
    ytdlp_.WaitForCompletion();
    app_->Quit();
}

void ODMApp::OnResize(ultralight::Window* window,
                      uint32_t width, uint32_t height) {
    if (overlay_) overlay_->Resize(width, height);
}

void ODMApp::OnChangeCursor(ultralight::View*, ultralight::Cursor cursor) {
    window_->SetCursor(cursor);
}

void ODMApp::OnAddConsoleMessage(ultralight::View*,
                                 const ultralight::ConsoleMessage& msg) {
    // Opt-in (ODM_CONSOLE_LOG=1) so the release build never touches the disk
    // for UI logging.
    static const bool enabled = [] {
#if defined(_WIN32)
        char buf[8] = {};
        DWORD n = GetEnvironmentVariableA("ODM_CONSOLE_LOG", buf, sizeof(buf));
        return n > 0 && buf[0] == '1';
#else
        const char* v = std::getenv("ODM_CONSOLE_LOG");
        return v && v[0] == '1';
#endif
    }();
    if (!enabled) return;

    static std::ofstream log("odm-console.log", std::ios::app);
    if (!log) return;
    ultralight::String8 text = msg.message().utf8();
    log.write(text.data(), static_cast<std::streamsize>(text.length()));
    log << '\n';
    log.flush();
}

void ODMApp::OnUpdate() {
#if defined(_WIN32)
    // AppCore's run loop uses PeekMessage with a filter range that excludes
    // WM_CLOSE (0x0010). The title-bar (X) works because it sends
    // WM_SYSCOMMAND/SC_CLOSE (0x0112) which DefWindowProc translates to
    // WM_CLOSE via SendMessage (synchronous, reaches our WNDPROC directly).
    // But the taskbar thumbnail close button posts WM_CLOSE directly to the
    // queue, where it gets stuck forever because AppCore never retrieves it.
    // Poll for it here on every update tick and handle it ourselves.
    if (g_main_hwnd && !g_real_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, WM_CLOSE, WM_CLOSE, PM_REMOVE)) {
            if (msg.hwnd == g_main_hwnd)
                ShowWindow(g_main_hwnd, SW_HIDE);
            else
                PostMessageW(msg.hwnd, WM_CLOSE, 0, 0);
        }
    }
#endif
    // Run any native tasks queued by other threads (e.g. the bridge listener).
    // These run before the JS flush because they typically enqueue JS.
    {
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lk(native_tasks_mtx_);
            tasks.swap(native_tasks_);
        }
        while (!tasks.empty()) {
            tasks.front()();
            tasks.pop();
        }
    }

    // Drain any pending JS that worker threads queued.
    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lk(js_queue_mtx_);
        local.swap(js_queue_);
    }
    if (!overlay_ || !overlay_->view()) return;
    while (!local.empty()) {
        overlay_->view()->EvaluateScript(
            ultralight::String(local.front().c_str()));
        local.pop();
    }
}

void ODMApp::OnDOMReady(ultralight::View* caller,
                        uint64_t /*frame_id*/,
                        bool /*is_main_frame*/,
                        const ultralight::String& /*url*/) {
    using namespace ultralight;

    js_ctx_ = caller->LockJSContext();
    SetJSContext(js_ctx_->ctx());

    JSObject global = JSGlobalObject();

    global["StartDownload"] =
        BindJSCallback(&ODMApp::JS_StartDownload);
    global["StopDownload"] =
        BindJSCallback(&ODMApp::JS_StopDownload);
    global["ApplySettings"] =
        BindJSCallback(&ODMApp::JS_ApplySettings);
    global["PickSavePath"] =
        BindJSCallbackWithRetval(&ODMApp::JS_PickSavePath);
    global["PickFolder"] =
        BindJSCallbackWithRetval(&ODMApp::JS_PickFolder);
    global["GetDiskSpace"] =
        BindJSCallbackWithRetval(&ODMApp::JS_GetDiskSpace);
    global["OpenFile"] =
        BindJSCallbackWithRetval(&ODMApp::JS_OpenFile);
    global["OpenWith"] =
        BindJSCallbackWithRetval(&ODMApp::JS_OpenWith);
    global["OpenFolder"] =
        BindJSCallbackWithRetval(&ODMApp::JS_OpenFolder);
    global["OpenDownloadsFolder"] =
        BindJSCallbackWithRetval(&ODMApp::JS_OpenDownloadsFolder);
    global["DeleteFile"] =
        BindJSCallbackWithRetval(&ODMApp::JS_DeleteFile);
    global["RequestMediaInfo"] =
        BindJSCallback(&ODMApp::JS_RequestMediaInfo);
    global["ProbeUrl"] =
        BindJSCallback(&ODMApp::JS_ProbeUrl);
    global["GetPartInfo"] =
        BindJSCallbackWithRetval(&ODMApp::JS_GetPartInfo);

    // The UI script has already run; ask it to refresh the disk widget now
    // that the native bridge is available.
    PostJS("if (typeof updateDiskWidget === 'function') updateDiskWidget();");
}

// --- JavaScript entry points ----------------------------------------------

void ODMApp::JS_StartDownload(const ultralight::JSObject&,
                              const ultralight::JSArgs& args) {
    if (downloader_.IsRunning() || hls_.IsRunning() || dash_.IsRunning() ||
        ytdlp_.IsRunning()) {
        PostJS("UI.onStatus('A download is already running.');");
        return;
    }
    if (args.size() < 1 || !args[0].IsString()) {
        PostJS("UI.onStatus('Please paste a download link first.');");
        return;
    }

    std::string url = ToStdStr(args[0].ToString());
    std::string out_path;
    if (args.size() >= 2 && args[1].IsString()) {
        out_path = ToStdStr(args[1].ToString());
    }
    // args[2] is the UI's download id, echoed back in progress/complete events
    // so the frontend can route them to the exact row (never by status).
    std::string id;
    if (args.size() >= 3 && args[2].IsString())
        id = ToStdStr(args[2].ToString());

    // args[3] (optional) is a one-shot request-context JSON captured by the
    // browser extension: {filename?, referrer?, cookies?, userAgent?,
    // headers?{...}}. It applies only to this Start.
    RequestContext req_ctx;
    std::string suggested_name;
    std::string job_type;
    std::string audio_url;
    int max_height = 0;   // YouTube quality menu; 0 = best available
    if (args.size() >= 4 && args[3].IsString()) {
        std::string ctx_json = ToStdStr(args[3].ToString());
        std::map<std::string, std::string> m;
        if (!ctx_json.empty() && ParseSimpleJson(ctx_json, m)) {
            req_ctx.referrer   = m["referrer"];
            req_ctx.cookies    = m["cookies"];
            req_ctx.user_agent = m["userAgent"];
            job_type = m["type"];
            audio_url = m["audioUrl"];   // paired DASH: the audio rung
            if (!m["height"].empty()) max_height = std::atoi(m["height"].c_str());
            for (const auto& kv : m) {
                if (kv.first.rfind("headers.", 0) == 0)
                    req_ctx.extra_headers.push_back(
                        kv.first.substr(8) + ": " + kv.second);
            }
            // Only when the browser actually suggested one: SanitizeForPath
            // answers "download.bin" for an empty string, which would turn
            // "no name was supplied" into a name that then beats every better
            // source we have (a YouTube title, say) and lands a video on disk
            // as download.bin — with an extension no muxer can write.
            if (!m["filename"].empty())
                suggested_name = Downloader::SanitizeForPath(m["filename"]);
        }
    }

    // A watch page is not a file: there is nothing to name, size or fetch
    // until yt-dlp has told us what the page actually holds. That answer
    // comes back on a worker thread and re-enters at BeginDownload.
    if (job_type == "ytdlp" ||
        (job_type.empty() && ytdlp::IsSupportedUrl(url))) {
        StartYouTubeDownload(url, out_path, id, suggested_name, max_height);
        return;
    }

    BeginDownload(std::move(url), std::move(out_path), std::move(id),
                  std::move(req_ctx), std::move(suggested_name),
                  std::move(job_type), std::move(audio_url));
}

void ODMApp::BeginDownload(std::string url, std::string out_path,
                           std::string id, RequestContext req_ctx,
                           std::string suggested_name, std::string job_type,
                           std::string audio_url) {
    // Derive a safe filename from the URL (used when the caller gave no path,
    // or only a directory). A browser-suggested filename wins when present.
    auto name_from_url = [&url]() -> std::string {
        size_t slash = url.find_last_of('/');
        std::string name = (slash != std::string::npos)
                               ? url.substr(slash + 1)
                               : "download.bin";
        size_t q = name.find_first_of("?#");
        if (q != std::string::npos) name = name.substr(0, q);
        name = Downloader::SanitizeForPath(name);
        return name.empty() ? std::string("download.bin") : name;
    };
    const std::string derived_name =
        suggested_name.empty() ? name_from_url() : suggested_name;

    std::error_code ec;
    if (out_path.empty()) {
        // Default to <OS Downloads>/DownloadManager/<name>. The folder is
        // created below via create_directories. If the OS Downloads folder
        // itself is unavailable we bail out with a clear error instead of
        // silently writing to some arbitrary fallback location.
        std::string dl_folder = GetDownloadsFolder();
        if (dl_folder.empty()) {
            PostJS("UI.onStatus('Could not resolve the OS Downloads folder.');");
            return;
        }
        out_path = (fs::path(dl_folder) / "DownloadManager" / derived_name).string();
    } else {
        // Directory intent must NOT depend on the disk state alone: a
        // deleted custom folder (Options) previously failed is_directory()
        // and the folder PATH was silently used as the output FILE name —
        // every download then overwrote one extension-less file. The UI now
        // marks folder paths with a trailing separator, so treat "existing
        // directory OR trailing separator" as a folder and recreate it.
        const char back = out_path.back();
        if (fs::is_directory(out_path, ec) || back == '\\' || back == '/')
            out_path = (fs::path(out_path) / derived_name).string();
    }
    // Ensure the destination directory exists so CreateFile can't fail with
    // "Cannot open output file" for a not-yet-created parent folder.
    fs::create_directories(fs::path(out_path).parent_path(), ec);

    // Never silently overwrite a finished file with the same name: fresh
    // jobs get "name (1).ext"; resume sidecars keep the original path.
    {
        out_path = UniquifyPath(out_path);
        // Told unconditionally, not only when the name had to be uniquified:
        // a YouTube row is created before anything about the file is known
        // (the URL's basename is "watch"), so this is the moment the list
        // learns the real name the video is being saved under.
        std::ostringstream js;
        js << "if (window.UI && UI.onPath) UI.onPath('" << EscapeJS(id)
           << "', '" << EscapeJS(out_path) << "');";
        PostJS(js.str());
    }

    PostJS("UI.onStatus('Connecting to server...');");
    {
        std::ostringstream js;
        js << "UI.onProgress('" << EscapeJS(id) << "', 0, '0 B', '0 B', 0, '', 0, 0, 0);";
        PostJS(js.str());
    }

    // Route by job type: HLS playlists go to the stream engine.
    if (job_type == "hls") {
        hls_.Start(url, out_path, id, req_ctx);
    } else if (job_type == "dash") {
        // `url` is the chosen video rung, `audio_url` its audio track; the
        // engine downloads both and remuxes them into one file.
        dash_.Start(url, audio_url, out_path, id, req_ctx);
    } else if (job_type == "ytdlp-direct") {
        // No plain media URL exists for this video — yt-dlp fetches it itself.
        ytdlp_.Start(url, out_path, id);
    } else {
        downloader_.Start(url, out_path, id, req_ctx);
    }
}

void ODMApp::StartYouTubeDownload(const std::string& page_url,
                                  const std::string& out_path,
                                  const std::string& id,
                                  const std::string& suggested_name,
                                  int max_height) {
    if (!ytdlp::Available()) {
        PostJS("UI.onStatus('YouTube support needs yt-dlp.exe next to ODM.exe.');");
        std::ostringstream js;
        js << "if (window.UI && UI.onComplete) UI.onComplete('" << EscapeJS(id)
           << "', 'error', '', 'yt-dlp.exe is missing from the ODM folder.');";
        PostJS(js.str());
        return;
    }

    PostJS("UI.onStatus('Reading the video page...');");

    std::thread([this, page_url, out_path, id, suggested_name, max_height] {
        ytdlp::MediaInfo info;
        const bool ok = ytdlp::Resolve(page_url, &info, max_height);

        // Everything below touches the engines and the UI, both of which are
        // main-thread only.
        PostNativeTask([this, page_url, out_path, id, suggested_name, info, ok,
                        max_height] {
            // The page title is the only sensible file name here; the URL
            // basename is "watch". A name the extension supplied still wins.
            std::string name = suggested_name;
            if (name.empty() && !info.title.empty())
                name = Downloader::SanitizeForPath(info.title);
            if (name.empty()) name = "youtube-video";
            // The extension is not cosmetic here: the two tracks are joined by
            // libavformat, which picks the output container from it. A name
            // ending in anything else (or nothing) fails the mux after both
            // tracks have already been downloaded.
            const std::string ext = "." + (info.ext.empty() ? "mp4" : info.ext);
            if (name.size() < ext.size() ||
                name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
                name += ext;

            if (!ok && !info.identified) {
                // yt-dlp could not even tell what video this is (a feed URL, a
                // private or removed video, no network). Handing the job to
                // the fallback engine here would download SOMETHING and call
                // it a success — report the reason instead.
                std::ostringstream js;
                js << "if (window.UI && UI.onComplete) UI.onComplete('"
                   << EscapeJS(id) << "', 'error', '', '"
                   << EscapeJS(info.error.empty()
                                   ? "Could not read this YouTube video."
                                   : info.error) << "');";
                PostJS(js.str());
                return;
            }
            if (!ok || info.video_url.empty()) {
                // The video is real but has no plain Range-capable URL: hand
                // the whole job to yt-dlp instead of failing. The quality cap
                // rides on the engine because BeginDownload routes by type
                // alone and knows nothing about YouTube.
                ytdlp_.SetMaxHeight(max_height);
                BeginDownload(page_url, out_path, id, RequestContext{}, name,
                              "ytdlp-direct", "");
                return;
            }
            // The normal case: two ordinary https tracks, downloaded by the
            // multi-segment engine and muxed by us — same as any DASH job.
            RequestContext ctx;
            ctx.referrer = "https://www.youtube.com/";
            BeginDownload(info.video_url, out_path, id, ctx, name,
                          info.audio_url.empty() ? "" : "dash",
                          info.audio_url);
        });
    }).detach();
}

void ODMApp::JS_StopDownload(const ultralight::JSObject&,
                             const ultralight::JSArgs&) {
    if (!downloader_.IsRunning() && !hls_.IsRunning() && !dash_.IsRunning() &&
        !ytdlp_.IsRunning()) {
        PostJS("UI.onStatus('No active download to stop.');");
        return;
    }
    PostJS("UI.onStatus('Stopping...');");
    downloader_.Stop();
    hls_.Stop();
    dash_.Stop();
    ytdlp_.Stop();
}

void ODMApp::JS_ApplySettings(const ultralight::JSObject&,
                              const ultralight::JSArgs& args) {
    if (args.size() >= 1 && args[0].IsNumber())
        downloader_.SetPartCount(static_cast<int>(args[0].ToNumber()));
    if (args.size() >= 2 && args[1].IsNumber())
        downloader_.SetMultiSegmentThreshold(
            static_cast<uint64_t>(args[1].ToNumber()));
    if (args.size() >= 3 && args[2].IsNumber())
        downloader_.SetSpeedLimit(static_cast<uint64_t>(args[2].ToNumber()));
    // HLS engine: forward the same limits (threshold is HTTP-only).
    if (args.size() >= 1 && args[0].IsNumber())
        hls_.SetPartCount(static_cast<int>(args[0].ToNumber()));
    if (args.size() >= 3 && args[2].IsNumber())
        hls_.SetSpeedLimit(static_cast<uint64_t>(args[2].ToNumber()));
    // DASH tracks and the yt-dlp fallback obey the same cap (part count is
    // fixed for both: DASH inherits the plain engine's, yt-dlp has one
    // connection by construction).
    if (args.size() >= 3 && args[2].IsNumber()) {
        dash_.SetSpeedLimit(static_cast<uint64_t>(args[2].ToNumber()));
        ytdlp_.SetSpeedLimit(static_cast<uint64_t>(args[2].ToNumber()));
    }
    // args[3] (downloadPath) is stored in localStorage on the JS side.
}

ultralight::JSValue
ODMApp::JS_PickSavePath(const ultralight::JSObject&,
                        const ultralight::JSArgs& args) {
    std::string file_name = "download.bin";
    if (args.size() >= 1 && args[0].IsString())
        file_name = ToStdStr(args[0].ToString());

    std::string chosen = NativeSaveDialog(file_name);
    return ultralight::JSValue(chosen.c_str());
}

// Native folder-selection dialog (Windows only).
static std::string NativeFolderDialog() {
#if defined(_WIN32)
    BROWSEINFOW bi = {0};
    bi.lpszTitle = L"Select a download folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::string result;
    if (pidl) {
        wchar_t path[MAX_PATH] = {0};
        if (SHGetPathFromIDListW(pidl, path)) result = WideToUtf8(path);
        IMalloc* im = nullptr;
        if (SUCCEEDED(SHGetMalloc(&im))) {
            im->Free(pidl);
            im->Release();
        }
    }
    return result;
#else
    return std::string();
#endif
}

ultralight::JSValue
ODMApp::JS_PickFolder(const ultralight::JSObject&,
                      const ultralight::JSArgs&) {
    std::string chosen = NativeFolderDialog();
    return ultralight::JSValue(chosen.c_str());
}

ultralight::JSValue
ODMApp::JS_GetDiskSpace(const ultralight::JSObject&,
                        const ultralight::JSArgs&) {
    // Get the download folder path.
    std::string dl_folder = GetDownloadsFolder();
    if (dl_folder.empty()) dl_folder = (fs::current_path() / "downloads").string();

    // Get the drive root from the path (e.g. "C:\").
    fs::path root = fs::path(dl_folder).root_path();
    if (root.empty()) root = "C:\\";

    std::error_code ec;
    auto info = fs::space(root, ec);
    if (ec) return ultralight::JSValue("{}");

    double total = static_cast<double>(info.capacity);
    double free  = static_cast<double>(info.available);
    double usedPct = total > 0 ? ((total - free) / total) * 100.0 : 0;
    int pctInt = static_cast<int>(usedPct + 0.5);

    std::string driveStr = root.string();
    // Strip trailing backslash for display.
    if (!driveStr.empty() && driveStr.back() == '\\') driveStr.pop_back();

    std::ostringstream json;
    json << "{\"pct\":" << pctInt
         << ",\"free\":\"" << HumanSize(free) << "\""
         << ",\"total\":\"" << HumanSize(total) << "\""
         << ",\"drive\":\"" << EscapeJS(driveStr) << "\""
         << ",\"path\":\"" << EscapeJS(dl_folder) << "\""
         << "}";
    return ultralight::JSValue(json.str().c_str());
}

ultralight::JSValue
ODMApp::JS_OpenFile(const ultralight::JSObject&,
                    const ultralight::JSArgs& args) {
    std::string file_path;
    if (args.size() >= 1 && args[0].IsString())
        file_path = ToStdStr(args[0].ToString());
    if (file_path.empty()) {
        PostJS("UI.onStatus('No file selected.');");
        return ultralight::JSValue(false);
    }

    std::error_code ec;
    if (!fs::exists(file_path, ec)) {
        PostJS("UI.onStatus('File does not exist.');");
        return ultralight::JSValue(false);
    }

#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        PostJS("UI.onStatus('Failed to open file.');");
        return ultralight::JSValue(false);
    }
    std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, wpath.data(), wlen);

    HINSTANCE h = ShellExecuteW(nullptr, L"open", wpath.data(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(h) <= 32) {
        PostJS("UI.onStatus('Failed to open file.');");
        return ultralight::JSValue(false);
    }
#else
    (void)0; // POSIX implementation not provided yet.
#endif

    return ultralight::JSValue(true);
}

ultralight::JSValue
ODMApp::JS_OpenWith(const ultralight::JSObject&,
                    const ultralight::JSArgs& args) {
    std::string file_path;
    if (args.size() >= 1 && args[0].IsString())
        file_path = ToStdStr(args[0].ToString());
    if (file_path.empty()) {
        PostJS("UI.onStatus('No file selected.');");
        return ultralight::JSValue(false);
    }

    std::error_code ec;
    if (!fs::exists(file_path, ec)) {
        PostJS("UI.onStatus('File does not exist.');");
        return ultralight::JSValue(false);
    }

#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        PostJS("UI.onStatus('Failed to open file.');");
        return ultralight::JSValue(false);
    }
    // Same widening dance as JS_OpenFile; the wstring keeps the buffer
    // alive for the worker thread below.
    std::vector<wchar_t> wbuf(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, wbuf.data(), wlen);
    const std::wstring wpath(wbuf.data());

    // SHOpenWithDialog blocks until the user picks an app, and it needs COM on
    // the calling thread. Running it inline would freeze rendering for as long
    // as the chooser is open, so it gets its own thread and its own apartment.
    HWND owner = static_cast<HWND>(hwnd_);
    std::thread([this, wpath, owner] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        OPENASINFO info{};
        info.pcszFile  = wpath.c_str();
        info.pcszClass = nullptr;
        // EXEC launches the chosen app; ALLOW_REGISTRATION offers the
        // "always use this app" checkbox, matching Explorer's own dialog.
        info.oaifInFlags = OAIF_EXEC | OAIF_ALLOW_REGISTRATION;
        const HRESULT hr = SHOpenWithDialog(owner, &info);
        if (SUCCEEDED(co)) CoUninitialize();
        // The user cancelling is not an error worth reporting.
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            PostJS("UI.onStatus('Could not open the \"Open with\" dialog.');");
    }).detach();
#else
    (void)0; // POSIX implementation not provided yet.
#endif

    return ultralight::JSValue(true);
}

ultralight::JSValue
ODMApp::JS_OpenFolder(const ultralight::JSObject&,
                      const ultralight::JSArgs& args) {
    std::string file_path;
    if (args.size() >= 1 && args[0].IsString())
        file_path = ToStdStr(args[0].ToString());
    if (file_path.empty()) {
        PostJS("UI.onStatus('No file selected.');");
        return ultralight::JSValue(false);
    }

    std::error_code ec;
    fs::path p = file_path;
    if (!fs::exists(p, ec)) {
        PostJS("UI.onStatus('File does not exist.');");
        return ultralight::JSValue(false);
    }

#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        PostJS("UI.onStatus('Failed to open folder.');");
        return ultralight::JSValue(false);
    }
    std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, wpath.data(), wlen);

    // Open Explorer with the file selected.
    std::wstring wfile(wpath.data());
    std::wstring params = L"/select,\"" + wfile + L"\"";
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(h) <= 32) {
        PostJS("UI.onStatus('Failed to open folder.');");
        return ultralight::JSValue(false);
    }
#else
    (void)0; // POSIX implementation not provided yet.
#endif

    return ultralight::JSValue(true);
}

// Open the folder where downloads are actually saved. Resolution order:
//   1. The optional path argument (the user's configured Options download
//      folder) — used only if it is a real, existing directory. Downloads land
//      directly inside it (no "DownloadManager" subfolder), so we open it
//      as-is.
//   2. <OS Downloads>/DownloadManager — the default location used by
//      JS_StartDownload when no custom folder is configured. Created on demand
//      so the folder is ready even before the first download.
// Returns a JSON string so the JS side can surface a precise toast:
//   {"ok":true,"path":"C:\\..."} | {"ok":false,"reason":"not_found|no_downloads_folder","path":...}
ultralight::JSValue
ODMApp::JS_OpenDownloadsFolder(const ultralight::JSObject&,
                               const ultralight::JSArgs& args) {
    auto make_result = [](bool ok, const std::string& reason,
                          const std::string& path) {
        std::ostringstream json;
        json << "{\"ok\":" << (ok ? "true" : "false")
             << ",\"reason\":\"" << EscapeJS(reason) << "\""
             << ",\"path\":\"" << EscapeJS(path) << "\"}";
        return ultralight::JSValue(json.str().c_str());
    };

#if defined(_WIN32)
    auto open_in_explorer = [](const std::string& path) -> bool {
        std::wstring wp = Utf8ToWide(path);
        HINSTANCE h = ShellExecuteW(nullptr, L"open", wp.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t>(h) > 32;
    };
#else
    auto open_in_explorer = [](const std::string&) -> bool { return false; };
#endif

    std::error_code ec;
    std::string preferred;
    if (args.size() >= 1 && args[0].IsString())
        preferred = ToStdStr(args[0].ToString());

    // 1. Honor the user's configured folder when it is an existing directory.
    if (!preferred.empty()) {
        fs::path p(preferred);
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            if (open_in_explorer(preferred))
                return make_result(true, "", preferred);
        }
        // Fall through: the configured path is missing or invalid; try the
        // default DownloadManager location instead of erroring out outright.
    }

    // 2. <OS Downloads>/DownloadManager — the default download destination.
    //    Created on demand (matches JS_StartDownload's create_directories
    //    behavior) so clicking the menu before any download still opens a
    //    valid, ready-to-use folder.
    std::string dl_folder = GetDownloadsFolder();
    if (dl_folder.empty())
        return make_result(false, "no_downloads_folder", "");

    fs::path dm = fs::path(dl_folder) / "DownloadManager";
    fs::create_directories(dm, ec);
    if (ec || !fs::is_directory(dm, ec))
        return make_result(false, "not_found", "");

    std::string s = dm.string();
    if (open_in_explorer(s))
        return make_result(true, "", s);

    return make_result(false, "not_found", s);
}

// Permanently delete a downloaded file from disk. Used by the "Delete From
// Disk" context-menu action. Sends the file to the Recycle Bin when possible
// (safer, user can recover); falls back to permanent delete if that fails.
// Args[0]: absolute file path. Returns true on success, false otherwise.
ultralight::JSValue
ODMApp::JS_DeleteFile(const ultralight::JSObject&,
                      const ultralight::JSArgs& args) {
    std::string file_path;
    if (args.size() >= 1 && args[0].IsString())
        file_path = ToStdStr(args[0].ToString());
    if (file_path.empty()) {
        PostJS("UI.onStatus('No file path provided.');");
        return ultralight::JSValue(false);
    }

    std::error_code ec;
    if (!fs::exists(file_path, ec)) {
        // Already gone — treat as success so the UI can clean up its entry.
        return ultralight::JSValue(true);
    }

#if defined(_WIN32)
    // Send to Recycle Bin via SHFileOperationW (double-null-terminated string,
    // fOF flags: silent + allow undo + no confirmation for safety dialogs).
    std::wstring wp = Utf8ToWide(file_path);
    std::wstring buf = wp + L'\0';   // SHFILEOPSTRUCT needs double-null term

    SHFILEOPSTRUCTW op = {0};
    op.hwnd   = nullptr;
    op.wFunc  = FO_DELETE;
    op.pFrom  = buf.c_str();
    op.pTo    = nullptr;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI |
                FOF_SILENT;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted)
        return ultralight::JSValue(true);
    // Recycle Bin failed (e.g. recycle bin disabled, path on network share)
    // — fall through to permanent delete as a last resort.
#endif

    if (fs::remove(file_path, ec))
        return ultralight::JSValue(true);

    PostJS("UI.onStatus('Could not delete the file.');");
    return ultralight::JSValue(false);
}

void ODMApp::JS_RequestMediaInfo(const ultralight::JSObject&,
                                 const ultralight::JSArgs& args) {
    std::string path, req_id;
    if (args.size() >= 1 && args[0].IsString())
        path = ToStdStr(args[0].ToString());
    if (args.size() >= 2 && args[1].IsString())
        req_id = ToStdStr(args[1].ToString());

    // Opening and scanning the file hits the disk (and can take a moment on
    // large MKVs), so run it on a detached worker and post the result back.
    // PostJS is already called from downloader worker threads, so it is safe
    // to use from here as well.
    std::thread([this, path, req_id] {
        std::string info_utf8;
#if defined(_WIN32)
        std::error_code ec;
        if (!path.empty() && fs::exists(path, ec)) {
            MediaInfoDLL::MediaInfo mi;
            // Standard field set (the MediaInfo GUI default), text output.
            mi.Option(L"Complete", L"0");
            mi.Option(L"Output", L"Text");
            if (mi.Open(Utf8ToWide(path)) > 0) {
                info_utf8 = WideToUtf8(mi.Inform().c_str());
                mi.Close();
            }
        }
#endif
        std::ostringstream js;
        js << "if (window.UI && UI.onMediaInfo) UI.onMediaInfo('"
           << EscapeJS(req_id) << "', '" << EscapeJS(info_utf8) << "');";
        PostJS(js.str());
    }).detach();
}

// --- URL probe (Add-Download modal "what IS this link?") --------------------
namespace {

struct ProbeInfo {
    bool        ok = false;
    long        http = 0;
    std::string mime;
    std::string filename;    // from Content-Disposition (may be empty)
    std::string final_url;   // after redirects
    double      size = -1;   // bytes; -1 = unknown
    bool        resumable = false;
    long long   cl_header = -1;   // raw Content-Length seen in headers
};

std::string PercentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = nib(s[i + 1]), lo = nib(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Header collector: filename, resumability and sizes live in headers only.
size_t ProbeHeaderCb(char* buf, size_t sz, size_t n, void* ud) {
    auto* pi = static_cast<ProbeInfo*>(ud);
    std::string line(buf, sz * n);
    std::string low = line;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });

    auto value_of = [&](size_t prefix_len) {
        std::string v = line.substr(prefix_len);
        const size_t a = v.find_first_not_of(" \t");
        const size_t b = v.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };

    if (low.rfind("accept-ranges:", 0) == 0) {
        if (low.find("bytes") != std::string::npos) pi->resumable = true;
    } else if (low.rfind("content-range:", 0) == 0) {
        // "bytes 0-0/12345" -> total size (the 1-byte GET fallback path).
        const size_t slash = line.rfind('/');
        if (slash != std::string::npos) {
            const double total = atof(line.c_str() + slash + 1);
            if (total > 0) pi->size = total;
        }
        pi->resumable = true;
    } else if (low.rfind("content-length:", 0) == 0) {
        pi->cl_header = atoll(value_of(15).c_str());
    } else if (low.rfind("content-disposition:", 0) == 0) {
        const std::string v = value_of(20);
        // RFC 6266: filename*=UTF-8''percent-encoded beats filename="...".
        size_t p = v.find("filename*=");
        if (p != std::string::npos) {
            std::string f = v.substr(p + 10);
            const size_t q = f.find("''");
            if (q != std::string::npos) f = f.substr(q + 2);
            const size_t semi = f.find(';');
            if (semi != std::string::npos) f = f.substr(0, semi);
            pi->filename = PercentDecode(f);
        } else if ((p = v.find("filename=")) != std::string::npos) {
            std::string f = v.substr(p + 9);
            if (!f.empty() && f[0] == '"') {
                const size_t endq = f.find('"', 1);
                f = f.substr(1, endq == std::string::npos ? std::string::npos
                                                          : endq - 1);
            } else {
                const size_t semi = f.find(';');
                if (semi != std::string::npos) f = f.substr(0, semi);
            }
            pi->filename = f;
        }
    }
    return sz * n;
}

// Body sink for the GET fallback: the first payload byte proves the headers
// are complete — abort immediately, we never want the actual file here.
size_t ProbeBodyCb(char*, size_t, size_t, void*) {
    return 0;   // CURLE_WRITE_ERROR -> transfer stops, headers already parsed
}

// One curl attempt. `head` = HEAD request, else GET with Range: 0-0.
ProbeInfo ProbeAttempt(const std::string& url, bool head,
                       const std::string& cookies, const std::string& referrer,
                       const std::string& ua) {
    ProbeInfo pi;
    CURL* curl = curl_easy_init();
    if (!curl) return pi;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ProbeHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &pi);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     ua.empty() ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                  "AppleWebKit/537.36 (KHTML, like Gecko) "
                                  "Chrome/126.0.0.0 Safari/537.36"
                                : ua.c_str());
    if (!cookies.empty())  curl_easy_setopt(curl, CURLOPT_COOKIE, cookies.c_str());
    if (!referrer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, referrer.c_str());
    if (head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ProbeBodyCb);
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &pi.http);
    char* ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) pi.mime = ct;
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) pi.final_url = eff;
    if (pi.size < 0) {
        // 206 fills size via Content-Range above; otherwise Content-Length
        // is the file size (on HEAD or a server that ignored the Range).
        if (pi.http != 206 && pi.cl_header > 1) pi.size = double(pi.cl_header);
        else {
            curl_off_t cl = -1;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
            if (cl > 1) pi.size = double(cl);
        }
    }
    // The GET fallback aborts on purpose (write error) — that is success.
    pi.ok = (rc == CURLE_OK || (!head && rc == CURLE_WRITE_ERROR)) &&
            pi.http >= 200 && pi.http < 400;
    curl_easy_cleanup(curl);
    return pi;
}

std::string JsonEsc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); out += b; }
                else out += static_cast<char>(c);
        }
    }
    return out;
}

} // namespace

void ODMApp::JS_ProbeUrl(const ultralight::JSObject&,
                         const ultralight::JSArgs& args) {
    auto arg_str = [&](size_t i) {
        return args.size() > i && args[i].IsString() ? ToStdStr(args[i].ToString())
                                                     : std::string();
    };
    const std::string url = arg_str(0), req_id = arg_str(1);
    const std::string cookies = arg_str(2), referrer = arg_str(3), ua = arg_str(4);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;

    // Network round-trips must never block the UI thread; same detached
    // worker + PostJS pattern as RequestMediaInfo.
    std::thread([this, url, req_id, cookies, referrer, ua] {
        // HEAD first (cheapest); many video CDNs reject it (403/405) or
        // return no useful headers -> retry as a 1-byte ranged GET.
        ProbeInfo pi = ProbeAttempt(url, true, cookies, referrer, ua);
        if (!pi.ok || (pi.mime.empty() && pi.size < 0))
            pi = ProbeAttempt(url, false, cookies, referrer, ua);

        std::ostringstream json;
        json << "{\"ok\":" << (pi.ok ? "true" : "false")
             << ",\"http\":" << pi.http
             << ",\"mime\":\"" << JsonEsc(pi.mime) << "\""
             << ",\"size\":" << (pi.size > 0 ? pi.size : -1)
             << ",\"filename\":\"" << JsonEsc(pi.filename) << "\""
             << ",\"finalUrl\":\"" << JsonEsc(pi.final_url) << "\""
             << ",\"resumable\":" << (pi.resumable ? "true" : "false") << "}";

        std::ostringstream js;
        js << "if (window.UI && UI.onUrlProbe) UI.onUrlProbe('"
           << EscapeJS(req_id) << "', '" << EscapeJS(json.str()) << "');";
        PostJS(js.str());
    }).detach();
}

ultralight::JSValue ODMApp::JS_GetPartInfo(const ultralight::JSObject&,
                                           const ultralight::JSArgs& args) {
    std::string id;
    if (args.size() >= 1 && args[0].IsString())
        id = ToStdStr(args[0].ToString());
    std::string json = downloader_.GetPartInfoJson(id);
    return ultralight::JSValue(json.c_str());
}

// --- Engine callbacks ------------------------------------------------------

void ODMApp::OnProgress(const ProgressInfo& info) {
    double pct = 0;
    if (info.total_bytes > 0)
        pct = std::min(100.0, (info.downloaded_bytes / info.total_bytes) * 100.0);

    std::ostringstream js;
    // The last three arguments are the RAW byte counts. The UI must compute
    // its ETA from these, never from the humanized strings — those use
    // independently chosen units (e.g. "512 KB" vs "3.5 MB/s"), and mixing
    // their parseFloat() values yields a garbage ETA.
    js << "UI.onProgress('"
       << EscapeJS(info.id) << "', "
       << pct << ", '"
       << EscapeJS(HumanSize(info.downloaded_bytes)) << "', '"
       << EscapeJS(info.total_bytes > 0 ? HumanSize(info.total_bytes) : std::string("?"))
       << "', " << info.active_parts << ", '"
       << EscapeJS(HumanSize(info.speed_bps)) << "/s"
       << "', " << static_cast<unsigned long long>(info.downloaded_bytes)
       << ", " << static_cast<unsigned long long>(info.total_bytes)
       << ", " << static_cast<unsigned long long>(info.speed_bps)
       << ");";
    PostJS(js.str());
}

void ODMApp::OnComplete(const DownloadResult& result) {
    std::ostringstream js;
    js << "UI.onComplete('"
       << EscapeJS(result.id) << "', '"
       << (result.status == DownloadStatus::Completed ? "ok" :
           result.status == DownloadStatus::Stopped   ? "stopped" : "error")
       << "', '" << EscapeJS(result.file_path) << "', '"
       << EscapeJS(result.error_message) << "');";
    PostJS(js.str());

    // Disk usage changed after a download finished/stopped, refresh the widget.
    PostJS("if (typeof updateDiskWidget === 'function') updateDiskWidget();");
}

// --- Browser bridge --------------------------------------------------------

void ODMApp::OnExternalDownload(const BridgePayload& payload) {
    // Marshal to the main thread: window focus and JS evaluation are only
    // safe there. The payload is copied into the task.
    PostNativeTask([this, payload] {
        BringWindowToFront();
        // Extra request headers captured by the extension are forwarded as a
        // JSON object so the UI can attach them to the next StartDownload
        // context (JS_StartDownload parses the "headers." prefix into
        // RequestContext::extra_headers).
        std::ostringstream hdrs;
        hdrs << "{";
        bool first = true;
        for (const auto& kv : payload.headers) {
            if (!first) hdrs << ",";
            first = false;
            hdrs << "\"" << EscapeJS(kv.first) << "\":\""
                 << EscapeJS(kv.second) << "\"";
        }
        hdrs << "}";
        std::ostringstream js;
        js << "if (window.UI && UI.onExternalDownload) UI.onExternalDownload('"
           << EscapeJS(payload.url) << "', '"
           << EscapeJS(payload.filename) << "', '"
           << EscapeJS(payload.referrer) << "', '"
           << EscapeJS(payload.cookies) << "', '"
           << EscapeJS(payload.user_agent) << "', '"
           << EscapeJS(hdrs.str()) << "', '"
           << EscapeJS(payload.type) << "', '"
           << EscapeJS(payload.audio_url) << "', '"
           << EscapeJS(payload.height) << "');";
        PostJS(js.str());
    });
}

void ODMApp::BringWindowToFront() {
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (!hwnd) return;

    // Unhide (from tray) or un-minimize first.
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOW);

    // Windows blocks SetForegroundWindow from background processes (foreground
    // lock). The bridge request originates from the browser — which is the
    // actual foreground process — so ODM is just a background socket server
    // receiving the hand-off. The AttachThreadInput trick merges our input
    // queue with the foreground window's thread, which makes Windows treat
    // our SetForegroundWindow call as if it came from the foreground process
    // itself. This is what makes the window reliably pop to the front even
    // when the browser (or its Save-As dialog) currently holds focus.
    HWND fore = GetForegroundWindow();
    DWORD foreTID = fore ? GetWindowThreadProcessId(fore, nullptr) : 0;
    DWORD myTID = GetCurrentThreadId();
    bool attached = false;
    if (foreTID && foreTID != myTID) {
        attached = AttachThreadInput(myTID, foreTID, TRUE);
    }

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (attached)
        AttachThreadInput(myTID, foreTID, FALSE);

    // SetForegroundWindow's return value is unreliable — it can return TRUE
    // while merely flashing the taskbar. Check the actual foreground window
    // and fall back to flashing if we didn't actually get focus.
    if (GetForegroundWindow() != hwnd) {
        FLASHWINFO fi{};
        fi.cbSize    = sizeof(fi);
        fi.hwnd      = hwnd;
        fi.dwFlags   = FLASHW_ALL | FLASHW_TIMERNOFG;
        fi.uCount    = 3;
        fi.dwTimeout = 0;
        FlashWindowEx(&fi);
    }
#endif
}

// --- Thread-safe JS dispatch ----------------------------------------------

void ODMApp::PostJS(const std::string& script) {
    {
        std::lock_guard<std::mutex> lk(js_queue_mtx_);
        js_queue_.push(script);
    }
    // AppCore's run loop polls OnUpdate frequently; no explicit wake needed.
}

void ODMApp::PostNativeTask(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(native_tasks_mtx_);
    native_tasks_.push(std::move(fn));
}

} // namespace odm