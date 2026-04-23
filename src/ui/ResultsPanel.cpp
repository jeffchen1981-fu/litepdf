#include "ui/ResultsPanel.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace litepdf::ui {

namespace {

// ----------------------------------------------------------------------------
// Sizing constants (DIP). Scaled per parent's DPI at construction.
// ----------------------------------------------------------------------------
constexpr int kTopRowHeightDip    = 32;
constexpr int kInnerPadDip        = 4;
constexpr int kEditLeftPadDip     = 6;
constexpr int kBtnCloseWidthDip   = 28;
constexpr int kColFileWidthDip    = 200;
constexpr int kColPageWidthDip    = 60;
constexpr int kColSnippetMinDip   = 200;  // last-column floor

// Child control IDs.
constexpr WORD kIdEdit     = 6001;
constexpr WORD kIdBtnClose = 6002;
constexpr WORD kIdList     = 6003;

constexpr UINT_PTR kEditSubclassId = 0xF601;
constexpr UINT_PTR kBtnSubclassId  = 0xF602;

constexpr wchar_t kWndClass[] = L"LitePDFResultsPanel";

std::once_flag g_class_once;

// RAII font wrapper (mirrors FindBar convention).
using unique_hfont = std::unique_ptr<std::remove_pointer_t<HFONT>,
                                     decltype(&DeleteObject)>;
unique_hfont make_unique_hfont(HFONT h) {
    return unique_hfont(h, &DeleteObject);
}

// ----------------------------------------------------------------------------
// Palette — local copy (same deferred-refactor rationale as FindBar).
// TODO(phase-6.x): consolidate into ui/Theme.hpp.
// ----------------------------------------------------------------------------
struct Palette {
    COLORREF panel_bg;
    COLORREF edit_bg;
    COLORREF edit_fg;
    COLORREF list_bg;
    COLORREF list_fg;
    COLORREF btn_normal;
    COLORREF btn_hover;
    COLORREF btn_pressed;
    COLORREF btn_fg;
    COLORREF close_hover_bg;
    COLORREF close_hover_fg;
    COLORREF border;
};

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*panel_bg*/       RGB(0x2B, 0x2B, 0x2B),
            /*edit_bg*/        RGB(0x1E, 0x1E, 0x1E),
            /*edit_fg*/        RGB(0xF2, 0xF2, 0xF2),
            /*list_bg*/        RGB(0x1E, 0x1E, 0x1E),
            /*list_fg*/        RGB(0xE8, 0xE8, 0xE8),
            /*btn_normal*/     RGB(0x2B, 0x2B, 0x2B),
            /*btn_hover*/      RGB(0x3A, 0x3A, 0x3A),
            /*btn_pressed*/    RGB(0x50, 0x50, 0x50),
            /*btn_fg*/         RGB(0xE0, 0xE0, 0xE0),
            /*close_hover_bg*/ RGB(0xC4, 0x2B, 0x1C),
            /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
            /*border*/         RGB(0x3A, 0x3A, 0x3A),
        };
    }
    return {
        /*panel_bg*/       RGB(0xF6, 0xF6, 0xF6),
        /*edit_bg*/        RGB(0xFF, 0xFF, 0xFF),
        /*edit_fg*/        RGB(0x1C, 0x1C, 0x1C),
        /*list_bg*/        RGB(0xFF, 0xFF, 0xFF),
        /*list_fg*/        RGB(0x1C, 0x1C, 0x1C),
        /*btn_normal*/     RGB(0xF6, 0xF6, 0xF6),
        /*btn_hover*/      RGB(0xEA, 0xEA, 0xEA),
        /*btn_pressed*/    RGB(0xD8, 0xD8, 0xD8),
        /*btn_fg*/         RGB(0x30, 0x30, 0x30),
        /*close_hover_bg*/ RGB(0xE8, 0x11, 0x23),
        /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
        /*border*/         RGB(0xD0, 0xD0, 0xD0),
    };
}

bool detect_dark_mode(HWND hwnd) {
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        if (dark) return true;
    }
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD val = 1, cb = sizeof(val);
        LONG r = RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr,
                                  reinterpret_cast<LPBYTE>(&val), &cb);
        RegCloseKey(hk);
        if (r == ERROR_SUCCESS) return val == 0;
    }
    return false;
}

HFONT create_panel_font(UINT dpi, int pt_size = 9) {
    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(pt_size, static_cast<int>(dpi), 72);
    lf.lfWeight  = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

int dp(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

// Convert UTF-8 → UTF-16 for ListView display. Snippet lengths in v1 are
// bounded by the needle itself (≤ a few hundred chars), so a single
// MultiByteToWideChar round-trip is fine with no streaming.
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0,
                                       s.c_str(), static_cast<int>(s.size()),
                                       nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

}  // namespace

// Forward-declare so Impl can reference via function pointer.
LRESULT CALLBACK results_panel_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
LRESULT CALLBACK results_edit_subclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                       UINT_PTR id, DWORD_PTR ref_data);
LRESULT CALLBACK results_btn_subclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                      UINT_PTR id, DWORD_PTR ref_data);

// ----------------------------------------------------------------------------
// Impl — private state.
// ----------------------------------------------------------------------------
struct ResultsPanel::Impl {
    HWND hwnd      = nullptr;
    HWND edit      = nullptr;
    HWND btn_close = nullptr;
    HWND listview  = nullptr;

    const litepdf::app::CrossTabSearch* xts = nullptr;  // non-owning

    UINT    dpi       = 96;
    bool    dark_mode = false;
    Palette palette   = make_palette(false);

    unique_hfont font_text  { nullptr, &DeleteObject };
    unique_hfont font_glyph { nullptr, &DeleteObject };

    HBRUSH bg_brush   = nullptr;
    HBRUSH edit_brush = nullptr;

    // Close-button hover/pressed state for owner-draw.
    bool hover_close   = false;
    bool pressed_close = false;

    // Display buffers for LVN_GETDISPINFOW. One buffer per subItem so
    // Windows' synchronous dispinfo calls (which happen one at a time on
    // the UI thread) can read pszText before the next GETDISPINFO arrives.
    // A single shared buffer would also work, but per-column makes it
    // obvious there's no overlap with follow-up requests on the same row.
    std::wstring disp_buf_file;
    std::wstring disp_buf_page;
    std::wstring disp_buf_snippet;

    // Callbacks (all fire on the UI thread).
    OnQuerySubmit on_query_submit;
    OnRowClick    on_row_click;
    OnClose       on_close;

    void track_button_mouse() {
        if (!btn_close) return;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, btn_close, 0 };
        TrackMouseEvent(&tme);
    }
};

namespace {

// Owner-draw close button (mirrors FindBar's close-button look).
void paint_close_button(const DRAWITEMSTRUCT* dis,
                        bool hover, bool pressed,
                        const Palette& pal, HFONT font_glyph, UINT dpi) {
    HDC  hdc = dis->hDC;
    RECT rc  = dis->rcItem;

    COLORREF bg = pal.btn_normal;
    if (hover)   bg = pal.close_hover_bg;
    if (pressed) bg = pal.btn_pressed;

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    const COLORREF fg = hover ? pal.close_hover_fg : pal.btn_fg;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);

    HGDIOBJ old_font = SelectObject(hdc, font_glyph);
    DrawTextW(hdc, L"\u2715", -1, &rc,  // ✕
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old_font);

    if (dis->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, -dp(2, dpi), -dp(2, dpi));
        DrawFocusRect(hdc, &fr);
    }
}

}  // namespace

// ----------------------------------------------------------------------------
// Edit subclass — intercept Enter (submit) and Escape (close) so the Edit
// control doesn't ding on them.
// ----------------------------------------------------------------------------
LRESULT CALLBACK results_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                       LPARAM l, UINT_PTR /*id*/,
                                       DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<ResultsPanel::Impl*>(ref_data);
    if (!impl) return DefSubclassProc(hwnd, msg, w, l);

    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS
                 | DefSubclassProc(hwnd, msg, w, l);
        case WM_KEYDOWN: {
            switch (w) {
                case VK_RETURN: {
                    if (impl->on_query_submit) {
                        const int len = GetWindowTextLengthW(hwnd);
                        std::wstring txt(static_cast<std::size_t>(len), L'\0');
                        if (len > 0) {
                            GetWindowTextW(hwnd, txt.data(), len + 1);
                        }
                        impl->on_query_submit(std::move(txt));
                    }
                    return 0;
                }
                case VK_ESCAPE:
                    if (impl->on_close) impl->on_close();
                    return 0;
            }
            break;
        }
        case WM_CHAR:
            // Swallow the character form of Enter/Escape so the Edit
            // doesn't MessageBeep.
            if (w == VK_RETURN || w == VK_ESCAPE) return 0;
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, results_edit_subclass,
                                 kEditSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// ----------------------------------------------------------------------------
// Close-button subclass — hover/pressed tracking for owner-draw.
// ----------------------------------------------------------------------------
LRESULT CALLBACK results_btn_subclass(HWND hwnd, UINT msg, WPARAM w,
                                      LPARAM l, UINT_PTR /*id*/,
                                      DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<ResultsPanel::Impl*>(ref_data);
    if (!impl) return DefSubclassProc(hwnd, msg, w, l);

    switch (msg) {
        case WM_MOUSEMOVE:
            if (!impl->hover_close) {
                impl->hover_close = true;
                impl->track_button_mouse();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (impl->hover_close) {
                impl->hover_close = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_LBUTTONDOWN:
            impl->pressed_close = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_LBUTTONUP:
            if (impl->pressed_close) {
                impl->pressed_close = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_CAPTURECHANGED:
            if (impl->pressed_close) {
                impl->pressed_close = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, results_btn_subclass, kBtnSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// ----------------------------------------------------------------------------
// Root WndProc.
// ----------------------------------------------------------------------------
LRESULT CALLBACK results_panel_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* impl = reinterpret_cast<ResultsPanel::Impl*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        impl = static_cast<ResultsPanel::Impl*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(impl));
        return 0;
    }

    if (!impl) return DefWindowProcW(hwnd, msg, w, l);

    switch (msg) {
        case WM_ERASEBKGND: {
            HDC  hdc = reinterpret_cast<HDC>(w);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, impl->bg_brush);
            // 1 px top border so the panel visually separates from the
            // splitter and main content above.
            RECT border = rc;
            border.bottom = rc.top + 1;
            HBRUSH b = CreateSolidBrush(impl->palette.border);
            FillRect(hdc, &border, b);
            DeleteObject(b);
            return 1;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(w);
            SetTextColor(hdc, impl->palette.edit_fg);
            SetBkColor  (hdc, impl->palette.edit_bg);
            if (!impl->edit_brush) {
                impl->edit_brush = CreateSolidBrush(impl->palette.edit_bg);
            }
            return reinterpret_cast<LRESULT>(impl->edit_brush);
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(w);
            SetTextColor(hdc, impl->palette.edit_fg);
            SetBkColor  (hdc, impl->palette.panel_bg);
            return reinterpret_cast<LRESULT>(impl->bg_brush);
        }

        case WM_COMMAND: {
            const WORD id   = LOWORD(w);
            const WORD code = HIWORD(w);
            if (code == BN_CLICKED && id == kIdBtnClose) {
                if (impl->on_close) impl->on_close();
                return 0;
            }
            break;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (!dis) break;
            if (static_cast<WORD>(dis->CtlID) == kIdBtnClose) {
                paint_close_button(dis, impl->hover_close,
                                   impl->pressed_close,
                                   impl->palette,
                                   impl->font_glyph.get(),
                                   impl->dpi);
                return TRUE;
            }
            break;
        }

        case WM_NOTIFY: {
            auto* nmh = reinterpret_cast<NMHDR*>(l);
            if (!nmh || nmh->hwndFrom != impl->listview) break;

            // Virtual-listview pull: Windows asks us to fill in text/image
            // for one (item, subItem) at a time. We consult xts.hits()
            // once per call; hit_count() is cheap (returns vector::size).
            if (nmh->code == LVN_GETDISPINFOW) {
                auto* disp = reinterpret_cast<NMLVDISPINFOW*>(nmh);
                const int idx = disp->item.iItem;
                const int col = disp->item.iSubItem;
                if (!impl->xts || idx < 0) break;
                if (static_cast<std::size_t>(idx) >= impl->xts->hit_count())
                    break;

                if (disp->item.mask & LVIF_TEXT) {
                    auto hits = impl->xts->hits();
                    if (static_cast<std::size_t>(idx) >= hits.size()) break;
                    const auto& h = hits[static_cast<std::size_t>(idx)];

                    switch (col) {
                        case 0:  // File
                            impl->disp_buf_file = h.file_label;
                            disp->item.pszText  = impl->disp_buf_file.data();
                            break;
                        case 1: {  // Page (1-based)
                            wchar_t buf[16];
                            swprintf_s(buf, L"%zu", h.page + 1);
                            impl->disp_buf_page = buf;
                            disp->item.pszText  = impl->disp_buf_page.data();
                            break;
                        }
                        case 2:  // Snippet
                            impl->disp_buf_snippet =
                                utf8_to_wide(h.geom.snippet_utf8);
                            disp->item.pszText =
                                impl->disp_buf_snippet.data();
                            break;
                        default:
                            break;
                    }
                }
                return 0;
            }

            if (nmh->code == NM_DBLCLK || nmh->code == NM_CLICK) {
                auto* ia = reinterpret_cast<LPNMITEMACTIVATE>(nmh);
                if (ia->iItem >= 0 && impl->on_row_click) {
                    impl->on_row_click(
                        static_cast<std::size_t>(ia->iItem));
                }
                return 0;
            }
            break;
        }

        case WM_SETTINGCHANGE: {
            HWND parent = GetParent(hwnd);
            const bool new_dark = detect_dark_mode(parent ? parent : hwnd);
            if (new_dark != impl->dark_mode) {
                impl->dark_mode = new_dark;
                impl->palette   = make_palette(new_dark);
                if (impl->bg_brush)   DeleteObject(impl->bg_brush);
                if (impl->edit_brush) DeleteObject(impl->edit_brush);
                impl->bg_brush   = CreateSolidBrush(impl->palette.panel_bg);
                impl->edit_brush = nullptr;
                if (impl->listview) {
                    ListView_SetBkColor    (impl->listview,
                                            impl->palette.list_bg);
                    ListView_SetTextBkColor(impl->listview,
                                            impl->palette.list_bg);
                    ListView_SetTextColor  (impl->listview,
                                            impl->palette.list_fg);
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_NCDESTROY: {
            if (impl->bg_brush) {
                DeleteObject(impl->bg_brush);
                impl->bg_brush = nullptr;
            }
            if (impl->edit_brush) {
                DeleteObject(impl->edit_brush);
                impl->edit_brush = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

namespace {

void register_class_once(HINSTANCE hInstance) {
    std::call_once(g_class_once, [hInstance] {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = results_panel_wndproc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // we paint bg in WM_ERASEBKGND
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
    });
}

}  // namespace

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
ResultsPanel::ResultsPanel(HINSTANCE hInstance, HWND parent,
                           const litepdf::app::CrossTabSearch& xts)
    : impl_(std::make_unique<Impl>())
{
    register_class_once(hInstance);

    impl_->xts       = &xts;
    impl_->dpi       = GetDpiForWindow(parent);
    if (impl_->dpi == 0) impl_->dpi = 96;
    impl_->dark_mode = detect_dark_mode(parent);
    impl_->palette   = make_palette(impl_->dark_mode);
    impl_->bg_brush  = CreateSolidBrush(impl_->palette.panel_bg);

    // Root panel — created hidden. lpCreateParams stashes Impl* so
    // WM_CREATE can wire GWLP_USERDATA before any other message arrives.
    impl_->hwnd = CreateWindowExW(
        0, kWndClass, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent, nullptr, hInstance, impl_.get());
    if (!impl_->hwnd) return;

    impl_->font_text  = make_unique_hfont(create_panel_font(impl_->dpi, 9));
    impl_->font_glyph = make_unique_hfont(create_panel_font(impl_->dpi, 12));

    // ---- Top row: query Edit + close button ----
    impl_->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0,
        impl_->hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEdit)),
        hInstance, nullptr);
    SendMessageW(impl_->edit, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font_text.get()),
                 MAKELPARAM(TRUE, 0));
    // Placeholder hint (Vista+; harmless on older SDKs where the message
    // is ignored).
    SendMessageW(impl_->edit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Cross-tab search..."));
    SetWindowSubclass(impl_->edit, results_edit_subclass,
                      kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));

    impl_->btn_close = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
        0, 0, 0, 0,
        impl_->hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdBtnClose)),
        hInstance, nullptr);
    SetWindowSubclass(impl_->btn_close, results_btn_subclass,
                      kBtnSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));
    SendMessageW(impl_->btn_close, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font_glyph.get()),
                 MAKELPARAM(TRUE, 0));

    // ---- Virtual ListView ----
    // LVS_OWNERDATA = virtual — we supply text on demand via LVN_GETDISPINFO.
    // LVS_SINGLESEL + LVS_SHOWSELALWAYS keeps the click-to-navigate model
    // obvious (only ever one highlighted row).
    impl_->listview = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA
            | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        impl_->hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdList)),
        hInstance, nullptr);
    if (impl_->listview) {
        ListView_SetExtendedListViewStyle(impl_->listview,
                                          LVS_EX_FULLROWSELECT
                                              | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor    (impl_->listview, impl_->palette.list_bg);
        ListView_SetTextBkColor(impl_->listview, impl_->palette.list_bg);
        ListView_SetTextColor  (impl_->listview, impl_->palette.list_fg);
        SendMessageW(impl_->listview, WM_SETFONT,
                     reinterpret_cast<WPARAM>(impl_->font_text.get()),
                     MAKELPARAM(TRUE, 0));

        // Columns: File / Page / Snippet. Widths in DIP; the Snippet column
        // is stretched in set_bounds() to fill the remaining panel width.
        LVCOLUMNW col = {};
        col.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(L"File");
        col.cx      = dp(kColFileWidthDip, impl_->dpi);
        col.iSubItem = 0;
        ListView_InsertColumn(impl_->listview, 0, &col);

        col.pszText = const_cast<LPWSTR>(L"Page");
        col.cx      = dp(kColPageWidthDip, impl_->dpi);
        col.iSubItem = 1;
        ListView_InsertColumn(impl_->listview, 1, &col);

        col.pszText = const_cast<LPWSTR>(L"Snippet");
        col.cx      = dp(kColSnippetMinDip, impl_->dpi);
        col.iSubItem = 2;
        ListView_InsertColumn(impl_->listview, 2, &col);
    }
}

ResultsPanel::~ResultsPanel() {
    if (impl_ && impl_->hwnd) {
        // WM_NCDESTROY clears brushes. unique_hfonts destruct when Impl dies.
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
}

HWND ResultsPanel::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

void ResultsPanel::show_and_focus_edit() {
    if (!impl_ || !impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_SHOW);
    SetWindowPos(impl_->hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (impl_->edit) {
        SetFocus(impl_->edit);
        SendMessageW(impl_->edit, EM_SETSEL,
                     static_cast<WPARAM>(0), static_cast<LPARAM>(-1));
    }
}

void ResultsPanel::hide() {
    if (!impl_ || !impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_HIDE);
}

bool ResultsPanel::visible() const {
    return impl_ && impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

void ResultsPanel::set_bounds(const RECT& bounds) {
    if (!impl_ || !impl_->hwnd) return;
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    SetWindowPos(impl_->hwnd, nullptr,
                 bounds.left, bounds.top, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const UINT dpi      = impl_->dpi;
    const int  top_h    = dp(kTopRowHeightDip, dpi);
    const int  pad      = dp(kInnerPadDip,     dpi);
    const int  edit_pad = dp(kEditLeftPadDip,  dpi);
    const int  btn_w    = dp(kBtnCloseWidthDip, dpi);
    const int  ctrl_h   = top_h - pad * 2;

    // Top row: Edit takes everything left of the close button.
    if (impl_->edit) {
        const int edit_x = edit_pad;
        const int edit_y = pad;
        const int edit_w = std::max(0,
            w - edit_pad - btn_w - pad * 2);
        SetWindowPos(impl_->edit, nullptr, edit_x, edit_y, edit_w, ctrl_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (impl_->btn_close) {
        const int btn_x = std::max(0, w - btn_w - pad);
        SetWindowPos(impl_->btn_close, nullptr,
                     btn_x, pad, btn_w, ctrl_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // ListView fills the rest.
    if (impl_->listview) {
        const int lv_y = top_h;
        const int lv_h = std::max(0, h - lv_y);
        SetWindowPos(impl_->listview, nullptr, 0, lv_y, w, lv_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        // Stretch the snippet column to consume any leftover width. The
        // File + Page columns keep their DIP widths; Snippet floors at
        // kColSnippetMinDip so the panel at its narrowest still shows
        // at least some snippet text.
        const int file_w    = dp(kColFileWidthDip, dpi);
        const int page_w    = dp(kColPageWidthDip, dpi);
        const int snippet_w = std::max(dp(kColSnippetMinDip, dpi),
                                       w - file_w - page_w);
        ListView_SetColumnWidth(impl_->listview, 2, snippet_w);
    }
}

void ResultsPanel::refresh_count() {
    if (!impl_ || !impl_->listview || !impl_->xts) return;
    // LVSICF_NOSCROLL avoids the visual jump-to-top artifact that a plain
    // SetItemCount causes every time the count grows during an in-flight
    // cross-tab scan.
    ListView_SetItemCountEx(impl_->listview,
                            static_cast<int>(impl_->xts->hit_count()),
                            LVSICF_NOSCROLL);
}

void ResultsPanel::set_on_query_submit(OnQuerySubmit cb) {
    if (impl_) impl_->on_query_submit = std::move(cb);
}
void ResultsPanel::set_on_row_click(OnRowClick cb) {
    if (impl_) impl_->on_row_click = std::move(cb);
}
void ResultsPanel::set_on_close(OnClose cb) {
    if (impl_) impl_->on_close = std::move(cb);
}

}  // namespace litepdf::ui
