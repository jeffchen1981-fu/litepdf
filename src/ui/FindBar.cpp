#include "ui/FindBar.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace litepdf::ui {

namespace {

// -----------------------------------------------------------------------------
// DIP constants. Bar and child sizes are expressed in device-independent pixels
// (1 DIP = 1 px at 96 DPI) and scaled per the parent window's DPI at creation.
// -----------------------------------------------------------------------------
constexpr int kBarHeightDip        = 32;
// Widened from 380 to fit two new toggle buttons (regex ".*" + whole-word
// "W"), each kBtnCaseWidthDip (28) wide with a kInnerPadDip (4) gap:
// 380 + 2 * (28 + 4) = 444.
constexpr int kBarWidthDip         = 444;
constexpr int kRightMarginDip      = 16;
constexpr int kTopMarginDip        = 8;
constexpr int kInnerPadDip         = 4;

constexpr int kEditWidthDip        = 180;
constexpr int kCounterWidthDip     = 60;
constexpr int kBtnNavWidthDip      = 24;
constexpr int kBtnCaseWidthDip     = 28;
constexpr int kBtnRegexWidthDip    = 28;
constexpr int kBtnWholeWidthDip    = 28;
constexpr int kBtnCloseWidthDip    = 28;

// Control IDs for child HWNDs. Range is arbitrary (5001..) but must not clash
// with menu IDs defined elsewhere; these are purely local to FindBar.
constexpr WORD kIdEdit     = 5001;
constexpr WORD kIdCounter  = 5002;
constexpr WORD kIdBtnPrev  = 5003;
constexpr WORD kIdBtnNext  = 5004;
constexpr WORD kIdBtnCase  = 5005;
constexpr WORD kIdBtnClose = 5006;
constexpr WORD kIdBtnRegex = 5007;
constexpr WORD kIdBtnWhole = 5008;

constexpr UINT_PTR kDebounceTimerId = 0xFB01;
constexpr UINT     kDebounceMs       = 120;

constexpr UINT_PTR kEditSubclassId   = 0xFB02;

constexpr wchar_t kWndClass[] = L"LitePDFFindBar";

std::once_flag g_class_once;

// RAII font wrapper.
using unique_hfont = std::unique_ptr<std::remove_pointer_t<HFONT>,
                                     decltype(&DeleteObject)>;
unique_hfont make_unique_hfont(HFONT h) {
    return unique_hfont(h, &DeleteObject);
}

// -----------------------------------------------------------------------------
// Palette. Kept file-local rather than shared with TabManager to keep this
// task's scope tight. A shared Palette header is a reasonable future cleanup.
// TODO(phase-6.x): consolidate with TabManager Palette into ui/Theme.hpp.
// -----------------------------------------------------------------------------
struct Palette {
    COLORREF bar_bg;
    COLORREF edit_bg;
    COLORREF edit_fg;
    COLORREF counter_fg;
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
            /*bar_bg*/         RGB(0x2B, 0x2B, 0x2B),
            /*edit_bg*/        RGB(0x1E, 0x1E, 0x1E),
            /*edit_fg*/        RGB(0xF2, 0xF2, 0xF2),
            /*counter_fg*/     RGB(0xB0, 0xB0, 0xB0),
            /*btn_normal*/     RGB(0x2B, 0x2B, 0x2B),
            /*btn_hover*/      RGB(0x3A, 0x3A, 0x3A),
            /*btn_pressed*/    RGB(0x50, 0x50, 0x50),
            /*btn_fg*/         RGB(0xE0, 0xE0, 0xE0),
            /*close_hover_bg*/ RGB(0xC4, 0x2B, 0x1C),
            /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
            /*border*/         RGB(0x3A, 0x3A, 0x3A),
        };
    }
    // Light-mode palette. bar_bg is slightly darker than the PDF page
    // white so the bar reads as a distinct panel when floating over a
    // rendered page (original 0xF9 was indistinguishable from #FFFFFF
    // page content). Border is drawn on all 4 edges in WM_ERASEBKGND
    // to give the bar a visible frame even against white content.
    return {
        /*bar_bg*/         RGB(0xEC, 0xEC, 0xEC),
        /*edit_bg*/        RGB(0xFF, 0xFF, 0xFF),
        /*edit_fg*/        RGB(0x1C, 0x1C, 0x1C),
        /*counter_fg*/     RGB(0x60, 0x60, 0x60),
        /*btn_normal*/     RGB(0xEC, 0xEC, 0xEC),
        /*btn_hover*/      RGB(0xDA, 0xDA, 0xDA),
        /*btn_pressed*/    RGB(0xC4, 0xC4, 0xC4),
        /*btn_fg*/         RGB(0x30, 0x30, 0x30),
        /*close_hover_bg*/ RGB(0xE8, 0x11, 0x23),
        /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
        /*border*/         RGB(0xA8, 0xA8, 0xA8),
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

HFONT create_findbar_font(UINT dpi, int pt_size = 9) {
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

}  // namespace

// Forward-declare so Impl can befriend / reference via pointer.
LRESULT CALLBACK find_bar_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
LRESULT CALLBACK find_bar_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                        LPARAM l, UINT_PTR id,
                                        DWORD_PTR ref_data);

// -----------------------------------------------------------------------------
// Impl — private state for FindBar. PIMPL keeps the public header free of any
// Win32 internals beyond HWND itself.
// -----------------------------------------------------------------------------
struct FindBar::Impl {
    HWND hwnd      = nullptr;
    HWND edit      = nullptr;
    HWND counter   = nullptr;
    HWND btn_prev  = nullptr;
    HWND btn_next  = nullptr;
    HWND btn_case  = nullptr;
    HWND btn_regex = nullptr;
    HWND btn_whole = nullptr;
    HWND btn_close = nullptr;

    // Toggle latching state. Each is visually "pressed" when true and factored
    // into OnQueryChanged so the caller always sees the current search mode.
    bool case_pressed  = false;
    bool whole_pressed = false;
    bool regex_pressed = false;

    // Set when the query is edited while regex mode is on; cleared when the
    // regex query is run (Enter) or when regex mode is turned off. Lets us
    // hold off on compiling/running an in-progress regex until the user
    // confirms with Enter.
    bool regex_dirty = false;

    // Whether the current query is an invalid pattern (e.g. malformed regex).
    // Drives the red Edit-text affordance; cleared on next run/edit.
    bool invalid_pattern = false;

    // Per-button hover / mouse-down state. Hover comes from WM_MOUSEMOVE on
    // the button HWND; pressed is set between WM_LBUTTONDOWN and WM_LBUTTONUP.
    bool hover_prev   = false;
    bool hover_next   = false;
    bool hover_case   = false;
    bool hover_regex  = false;
    bool hover_whole  = false;
    bool hover_close  = false;
    bool pressed_prev = false;
    bool pressed_next = false;
    bool pressed_case = false;
    bool pressed_regex = false;
    bool pressed_whole = false;
    bool pressed_close = false;

    // Cached fonts (DPI-aware). Rebuilt on WM_DPICHANGED in future; for now
    // DPI is captured at WM_CREATE and considered stable for the bar's life.
    unique_hfont font_text  { nullptr, &DeleteObject };
    unique_hfont font_glyph { nullptr, &DeleteObject };

    // Brushes for WM_CTLCOLOR* so Edit/Static paint with the bar bg.
    // Two brushes since the Edit uses a distinct "field" color against the
    // bar background. Rebuilt on theme change.
    HBRUSH bg_brush   = nullptr;
    HBRUSH edit_brush = nullptr;

    bool   dark_mode = false;
    Palette palette  = make_palette(false);
    UINT    dpi      = 96;

    // Callbacks (all fire on the UI thread).
    QueryChanged on_query_changed;
    NavAction    on_next;
    NavAction    on_prev;
    NavAction    on_close;

    // Last query submitted via debounce. Used so we skip firing
    // on_query_changed when the user hits space+backspace back to the same
    // string — the case-toggle flip still goes through because we also factor
    // case_pressed into the equality check.
    std::wstring last_query;
    bool         last_case  = false;
    bool         last_whole = false;
    bool         last_regex = false;

    // ---- tracking helpers ----
    bool track_mouse(HWND button_hwnd) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, button_hwnd, 0 };
        return TrackMouseEvent(&tme) != 0;
    }

    void invalidate_button(HWND b) {
        if (b) InvalidateRect(b, nullptr, FALSE);
    }

    // Dispatch the current Edit text through the debounced callback.
    void fire_query_changed() {
        if (!edit || !on_query_changed) return;
        const int len = GetWindowTextLengthW(edit);
        std::wstring txt(static_cast<std::size_t>(len), L'\0');
        if (len > 0) {
            GetWindowTextW(edit, txt.data(), len + 1);
        }
        if (txt == last_query && case_pressed == last_case &&
            whole_pressed == last_whole && regex_pressed == last_regex) {
            return;
        }
        last_query  = txt;
        last_case   = case_pressed;
        last_whole  = whole_pressed;
        last_regex  = regex_pressed;
        invalid_pattern = false;
        on_query_changed(txt, case_pressed, whole_pressed, regex_pressed);
    }

    // Kill any pending debounce timer and schedule a fresh one. Always
    // kill-first so rapid typing doesn't stack N timers and emit N events.
    void reschedule_debounce() {
        if (!hwnd) return;
        KillTimer(hwnd, kDebounceTimerId);
        SetTimer(hwnd, kDebounceTimerId, kDebounceMs, nullptr);
    }
};

// -----------------------------------------------------------------------------
// Owner-draw helpers.
// -----------------------------------------------------------------------------
namespace {

enum class ButtonKind { Prev, Next, Case, Regex, Whole, Close };

struct ButtonVisual {
    bool hover;
    bool pressed;
};

void paint_button(const DRAWITEMSTRUCT* dis, ButtonKind kind,
                  const ButtonVisual& v, bool latched,
                  const Palette& pal, HFONT font_text, HFONT font_glyph,
                  UINT dpi) {
    HDC  hdc = dis->hDC;
    RECT rc  = dis->rcItem;

    // Background color. Close button gets a red hover color (destructive-ish
    // affordance consistent with tab close). Toggle buttons (Case/Regex/Whole)
    // use the pressed color while latched so the user sees the current mode at
    // a glance.
    COLORREF bg = pal.btn_normal;
    if (kind == ButtonKind::Close && v.hover) {
        bg = pal.close_hover_bg;
    } else if (v.pressed) {
        bg = pal.btn_pressed;
    } else if (latched) {
        bg = pal.btn_pressed;
    } else if (v.hover) {
        bg = pal.btn_hover;
    }
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    COLORREF fg = (kind == ButtonKind::Close && v.hover)
                      ? pal.close_hover_fg
                      : pal.btn_fg;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);

    const wchar_t* glyph = L"";
    HFONT          use_font = font_glyph;
    switch (kind) {
        case ButtonKind::Prev:  glyph = L"\u2039"; break;  // ‹
        case ButtonKind::Next:  glyph = L"\u203A"; break;  // ›
        case ButtonKind::Close: glyph = L"\u2715"; break;  // ✕
        case ButtonKind::Case:
            glyph    = L"Aa";
            use_font = font_text;
            break;
        case ButtonKind::Regex:
            glyph    = L".*";
            use_font = font_text;
            break;
        case ButtonKind::Whole:
            glyph    = L"W";
            use_font = font_text;
            break;
    }

    HGDIOBJ old_font = SelectObject(hdc, use_font);
    DrawTextW(hdc, glyph, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old_font);

    // Focus indicator: the default keyboard-focus rect around an owner-draw
    // button. Drawn when Windows tells us ODS_FOCUS.
    if (dis->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, -dp(2, dpi), -dp(2, dpi));
        DrawFocusRect(hdc, &fr);
    }
}

ButtonKind button_kind_of(WORD id) {
    switch (id) {
        case kIdBtnPrev:  return ButtonKind::Prev;
        case kIdBtnNext:  return ButtonKind::Next;
        case kIdBtnCase:  return ButtonKind::Case;
        case kIdBtnRegex: return ButtonKind::Regex;
        case kIdBtnWhole: return ButtonKind::Whole;
        default:          return ButtonKind::Close;
    }
}

void get_button_state(const FindBar::Impl& impl, WORD id,
                      ButtonVisual& out) {
    switch (id) {
        case kIdBtnPrev:
            out = { impl.hover_prev,  impl.pressed_prev  }; break;
        case kIdBtnNext:
            out = { impl.hover_next,  impl.pressed_next  }; break;
        case kIdBtnCase:
            out = { impl.hover_case,  impl.pressed_case  }; break;
        case kIdBtnRegex:
            out = { impl.hover_regex, impl.pressed_regex }; break;
        case kIdBtnWhole:
            out = { impl.hover_whole, impl.pressed_whole }; break;
        case kIdBtnClose:
        default:
            out = { impl.hover_close, impl.pressed_close }; break;
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Button subclass — handles hover + pressed + click tracking for each of the
// four owner-draw buttons. We subclass rather than rely on BN_CLICKED alone
// because (a) WM_MOUSEMOVE on a BS_OWNERDRAW button doesn't bubble up to the
// parent, so hover must be tracked here; (b) we need BN_SETFOCUS/BN_KILLFOCUS
// to keep pressed state consistent with capture.
// -----------------------------------------------------------------------------
LRESULT CALLBACK find_bar_button_subclass(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l, UINT_PTR /*id*/,
                                          DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<FindBar::Impl*>(ref_data);
    if (!impl) return DefSubclassProc(hwnd, msg, w, l);
    const WORD ctl_id = static_cast<WORD>(GetDlgCtrlID(hwnd));

    auto hover_ptr = [&]() -> bool* {
        switch (ctl_id) {
            case kIdBtnPrev:  return &impl->hover_prev;
            case kIdBtnNext:  return &impl->hover_next;
            case kIdBtnCase:  return &impl->hover_case;
            case kIdBtnRegex: return &impl->hover_regex;
            case kIdBtnWhole: return &impl->hover_whole;
            case kIdBtnClose: return &impl->hover_close;
        }
        return nullptr;
    };
    auto pressed_ptr = [&]() -> bool* {
        switch (ctl_id) {
            case kIdBtnPrev:  return &impl->pressed_prev;
            case kIdBtnNext:  return &impl->pressed_next;
            case kIdBtnCase:  return &impl->pressed_case;
            case kIdBtnRegex: return &impl->pressed_regex;
            case kIdBtnWhole: return &impl->pressed_whole;
            case kIdBtnClose: return &impl->pressed_close;
        }
        return nullptr;
    };

    switch (msg) {
        case WM_MOUSEMOVE: {
            bool* h = hover_ptr();
            if (h && !*h) {
                *h = true;
                impl->track_mouse(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            bool* h = hover_ptr();
            if (h && *h) {
                *h = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            bool* p = pressed_ptr();
            if (p) {
                *p = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_LBUTTONUP: {
            bool* p = pressed_ptr();
            if (p && *p) {
                *p = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_CAPTURECHANGED: {
            bool* p = pressed_ptr();
            if (p && *p) {
                *p = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_NCDESTROY: {
            RemoveWindowSubclass(hwnd, find_bar_button_subclass, ctl_id);
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// -----------------------------------------------------------------------------
// Edit subclass — intercept Esc / Enter / F3 for navigation shortcuts; pass
// everything else through so regular typing works unchanged.
// -----------------------------------------------------------------------------
LRESULT CALLBACK find_bar_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                        LPARAM l, UINT_PTR /*id*/,
                                        DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<FindBar::Impl*>(ref_data);
    if (!impl) return DefSubclassProc(hwnd, msg, w, l);

    switch (msg) {
        case WM_GETDLGCODE:
            // Tell the dialog manager to route Enter/Escape/Tab chars here
            // so WM_CHAR/WM_KEYDOWN actually reach this subclass.
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS
                 | DefSubclassProc(hwnd, msg, w, l);
        case WM_KEYDOWN: {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            switch (w) {
                case VK_ESCAPE:
                    if (impl->on_close) impl->on_close();
                    return 0;  // consumed
                case VK_RETURN:
                    // Regex mode defers running until Enter: if the query was
                    // edited since the last run (regex_dirty), compile + run it
                    // now instead of navigating hits.
                    if (impl->regex_pressed && impl->regex_dirty) {
                        impl->regex_dirty = false;
                        impl->fire_query_changed();
                    } else if (shift) {
                        if (impl->on_prev) impl->on_prev();
                    } else {
                        if (impl->on_next) impl->on_next();
                    }
                    return 0;
                case VK_F3:
                    if (shift) { if (impl->on_prev) impl->on_prev(); }
                    else        { if (impl->on_next) impl->on_next(); }
                    return 0;
            }
            break;
        }
        case WM_CHAR: {
            // Swallow the character form of Enter/Escape so the Edit control
            // doesn't ding (MessageBeep) complaining it can't handle them.
            if (w == VK_RETURN || w == VK_ESCAPE) return 0;
            break;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, find_bar_edit_subclass,
                                 kEditSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// -----------------------------------------------------------------------------
// Root WndProc — dispatch for the bar HWND itself. Stores FindBar::Impl* in
// GWLP_USERDATA (set from CREATESTRUCT.lpCreateParams in WM_CREATE).
// -----------------------------------------------------------------------------
LRESULT CALLBACK find_bar_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* impl = reinterpret_cast<FindBar::Impl*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        impl = static_cast<FindBar::Impl*>(cs->lpCreateParams);
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
            // Draw a 1px border on ALL 4 edges so the bar reads as a
            // distinct panel even when floating over white PDF content.
            // The original bottom-only border relied on bar_bg contrast
            // against the page, which failed against white pages.
            HBRUSH b = CreateSolidBrush(impl->palette.border);
            RECT top_edge    = { rc.left,  rc.top,        rc.right,    rc.top + 1    };
            RECT bottom_edge = { rc.left,  rc.bottom - 1, rc.right,    rc.bottom     };
            RECT left_edge   = { rc.left,  rc.top,        rc.left + 1, rc.bottom     };
            RECT right_edge  = { rc.right - 1, rc.top,    rc.right,    rc.bottom     };
            FillRect(hdc, &top_edge,    b);
            FillRect(hdc, &bottom_edge, b);
            FillRect(hdc, &left_edge,   b);
            FillRect(hdc, &right_edge,  b);
            DeleteObject(b);
            return 1;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(w);
            // Red text signals an invalid pattern (e.g. malformed regex). The
            // bg stays the normal field color; only the text turns red.
            SetTextColor(hdc, impl->invalid_pattern
                                  ? RGB(0xD0, 0x2B, 0x1C)
                                  : impl->palette.edit_fg);
            SetBkColor (hdc, impl->palette.edit_bg);
            if (!impl->edit_brush) {
                impl->edit_brush = CreateSolidBrush(impl->palette.edit_bg);
            }
            return reinterpret_cast<LRESULT>(impl->edit_brush);
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(w);
            SetTextColor(hdc, impl->palette.counter_fg);
            SetBkColor  (hdc, impl->palette.bar_bg);
            return reinterpret_cast<LRESULT>(impl->bg_brush);
        }

        case WM_COMMAND: {
            const WORD id   = LOWORD(w);
            const WORD code = HIWORD(w);
            if (id == kIdEdit && code == EN_CHANGE) {
                // Editing clears any stale "invalid pattern" affordance.
                if (impl->invalid_pattern) {
                    impl->invalid_pattern = false;
                    InvalidateRect(impl->edit, nullptr, FALSE);
                }
                if (impl->regex_pressed) {
                    // Regex compiles can be expensive / partial mid-typing;
                    // hold off until the user confirms with Enter.
                    impl->regex_dirty = true;
                } else {
                    impl->reschedule_debounce();
                }
                return 0;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case kIdBtnPrev:
                        if (impl->on_prev) impl->on_prev();
                        return 0;
                    case kIdBtnNext:
                        if (impl->on_next) impl->on_next();
                        return 0;
                    case kIdBtnCase:
                        impl->case_pressed = !impl->case_pressed;
                        impl->invalidate_button(impl->btn_case);
                        impl->fire_query_changed();
                        return 0;
                    case kIdBtnRegex:
                        impl->regex_pressed = !impl->regex_pressed;
                        // Enabling regex requires an explicit Enter before the
                        // first compile/run; disabling reverts to live search.
                        impl->regex_dirty = impl->regex_pressed;
                        impl->invalidate_button(impl->btn_regex);
                        if (!impl->regex_pressed) impl->fire_query_changed();
                        return 0;
                    case kIdBtnWhole:
                        impl->whole_pressed = !impl->whole_pressed;
                        impl->invalidate_button(impl->btn_whole);
                        impl->fire_query_changed();  // whole-word stays live
                        return 0;
                    case kIdBtnClose:
                        if (impl->on_close) impl->on_close();
                        return 0;
                }
            }
            break;
        }

        case WM_TIMER: {
            if (w == kDebounceTimerId) {
                KillTimer(hwnd, kDebounceTimerId);
                impl->fire_query_changed();
                return 0;
            }
            break;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (!dis) break;
            const WORD id = static_cast<WORD>(dis->CtlID);
            if (id != kIdBtnPrev && id != kIdBtnNext
             && id != kIdBtnCase && id != kIdBtnRegex
             && id != kIdBtnWhole && id != kIdBtnClose) {
                break;
            }
            ButtonVisual v{};
            get_button_state(*impl, id, v);
            const bool latched =
                (id == kIdBtnCase  && impl->case_pressed)  ||
                (id == kIdBtnRegex && impl->regex_pressed) ||
                (id == kIdBtnWhole && impl->whole_pressed);
            paint_button(dis, button_kind_of(id), v, latched,
                         impl->palette, impl->font_text.get(),
                         impl->font_glyph.get(), impl->dpi);
            return TRUE;
        }

        case WM_SETTINGCHANGE: {
            // TODO(phase-6.x): full dark-mode hot-swap. For now we do a light
            // touch: re-detect and repaint if the mode flipped. No relaunch
            // needed in the common case.
            HWND parent = GetParent(hwnd);
            const bool new_dark = detect_dark_mode(parent ? parent : hwnd);
            if (new_dark != impl->dark_mode) {
                impl->dark_mode = new_dark;
                impl->palette   = make_palette(new_dark);
                if (impl->bg_brush)   DeleteObject(impl->bg_brush);
                if (impl->edit_brush) DeleteObject(impl->edit_brush);
                impl->bg_brush   = CreateSolidBrush(impl->palette.bar_bg);
                impl->edit_brush = nullptr;  // lazy-rebuilt on next CTLCOLOREDIT
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }

        case WM_NCDESTROY: {
            KillTimer(hwnd, kDebounceTimerId);
            if (impl->bg_brush) {
                DeleteObject(impl->bg_brush);
                impl->bg_brush = nullptr;
            }
            if (impl->edit_brush) {
                DeleteObject(impl->edit_brush);
                impl->edit_brush = nullptr;
            }
            // unique_hfont dtors will run when Impl dies (FindBar dtor).
            // Clear the GWLP_USERDATA so any stray message after this returns
            // the neutral DefWindowProc path.
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
        wc.lpfnWndProc   = find_bar_wndproc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // we paint bg in WM_ERASEBKGND
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
    });
}

HWND create_button(HINSTANCE hInstance, HWND parent, WORD id, int w, int h) {
    return CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
        0, 0, w, h,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        hInstance, nullptr);
}

}  // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
FindBar::FindBar(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    register_class_once(hInstance);

    impl_->dpi       = GetDpiForWindow(parent);
    if (impl_->dpi == 0) impl_->dpi = 96;
    impl_->dark_mode = detect_dark_mode(parent);
    impl_->palette   = make_palette(impl_->dark_mode);
    impl_->bg_brush  = CreateSolidBrush(impl_->palette.bar_bg);

    // Sizing.
    const int bar_w = dp(kBarWidthDip,  impl_->dpi);
    const int bar_h = dp(kBarHeightDip, impl_->dpi);

    // Create bar (hidden). Pass `this` Impl via lpCreateParams so WM_CREATE
    // can stash it in GWLP_USERDATA before any other message arrives.
    impl_->hwnd = CreateWindowExW(
        0, kWndClass, L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, bar_w, bar_h,
        parent, nullptr, hInstance, impl_.get());
    if (!impl_->hwnd) return;  // silently no-op if class/window failed

    // Cache fonts.
    impl_->font_text  = make_unique_hfont(create_findbar_font(impl_->dpi, 9));
    impl_->font_glyph = make_unique_hfont(create_findbar_font(impl_->dpi, 12));

    // Child controls. Positions are fixed up in reposition().
    const int ctrl_h = bar_h - dp(kInnerPadDip * 2, impl_->dpi);
    const int pad    = dp(kInnerPadDip, impl_->dpi);

    impl_->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        pad, pad, dp(kEditWidthDip, impl_->dpi), ctrl_h,
        impl_->hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEdit)),
        hInstance, nullptr);
    SendMessageW(impl_->edit, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font_text.get()),
                 MAKELPARAM(TRUE, 0));
    SetWindowSubclass(impl_->edit, find_bar_edit_subclass,
                      kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));

    impl_->counter = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        0, pad, dp(kCounterWidthDip, impl_->dpi), ctrl_h,
        impl_->hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdCounter)),
        hInstance, nullptr);
    SendMessageW(impl_->counter, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font_text.get()),
                 MAKELPARAM(TRUE, 0));

    impl_->btn_prev  = create_button(hInstance, impl_->hwnd, kIdBtnPrev,
                                     dp(kBtnNavWidthDip, impl_->dpi), ctrl_h);
    impl_->btn_next  = create_button(hInstance, impl_->hwnd, kIdBtnNext,
                                     dp(kBtnNavWidthDip, impl_->dpi), ctrl_h);
    impl_->btn_case  = create_button(hInstance, impl_->hwnd, kIdBtnCase,
                                     dp(kBtnCaseWidthDip, impl_->dpi), ctrl_h);
    impl_->btn_regex = create_button(hInstance, impl_->hwnd, kIdBtnRegex,
                                     dp(kBtnRegexWidthDip, impl_->dpi), ctrl_h);
    impl_->btn_whole = create_button(hInstance, impl_->hwnd, kIdBtnWhole,
                                     dp(kBtnWholeWidthDip, impl_->dpi), ctrl_h);
    impl_->btn_close = create_button(hInstance, impl_->hwnd, kIdBtnClose,
                                     dp(kBtnCloseWidthDip, impl_->dpi), ctrl_h);

    // Subclass all owner-draw buttons for hover / pressed tracking.
    for (HWND b : { impl_->btn_prev, impl_->btn_next,
                    impl_->btn_case, impl_->btn_regex,
                    impl_->btn_whole, impl_->btn_close }) {
        if (!b) continue;
        const UINT_PTR id = static_cast<UINT_PTR>(GetDlgCtrlID(b));
        SetWindowSubclass(b, find_bar_button_subclass, id,
                          reinterpret_cast<DWORD_PTR>(impl_.get()));
        SendMessageW(b, WM_SETFONT,
                     reinterpret_cast<WPARAM>(impl_->font_text.get()),
                     MAKELPARAM(TRUE, 0));
    }
}

FindBar::~FindBar() {
    if (impl_ && impl_->hwnd) {
        // WM_NCDESTROY will fire during DestroyWindow → it clears timer and
        // brush. unique_hfonts in Impl are destroyed when Impl is released
        // below (by unique_ptr dtor).
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
}

HWND FindBar::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

void FindBar::show_or_focus(const std::wstring& prefill) {
    if (!impl_ || !impl_->hwnd) return;
    if (!prefill.empty()) {
        SetWindowTextW(impl_->edit, prefill.c_str());
    }
    ShowWindow(impl_->hwnd, SW_SHOW);
    SetWindowPos(impl_->hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetFocus(impl_->edit);
    // Caret at end of text so typing appends to any prefill.
    SendMessageW(impl_->edit, EM_SETSEL,
                 static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
}

void FindBar::hide() {
    if (!impl_ || !impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_HIDE);
    // Cancel any pending debounce so a hidden bar doesn't fire once we reopen.
    KillTimer(impl_->hwnd, kDebounceTimerId);
    // Closing clears the search session (MainWindow::on_find_close). If regex
    // mode is on, mark the query dirty so the next reopen's Enter re-runs the
    // pattern instead of navigating the now-empty session.
    if (impl_->regex_pressed) impl_->regex_dirty = true;
}

bool FindBar::visible() const {
    return impl_ && impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

void FindBar::reposition(const RECT& canvas_rect) {
    if (!impl_ || !impl_->hwnd) return;
    const UINT dpi = impl_->dpi;
    const int bar_w = dp(kBarWidthDip,  dpi);
    const int bar_h = dp(kBarHeightDip, dpi);
    const int x = canvas_rect.right - bar_w - dp(kRightMarginDip, dpi);
    const int y = canvas_rect.top   + dp(kTopMarginDip, dpi);

    SetWindowPos(impl_->hwnd, HWND_TOP, x, y, bar_w, bar_h,
                 SWP_NOACTIVATE);

    // Position children left-to-right with kInnerPadDip gaps.
    const int pad     = dp(kInnerPadDip, dpi);
    const int ctrl_h  = bar_h - pad * 2;
    int cx = pad;

    const int w_edit    = dp(kEditWidthDip,    dpi);
    const int w_counter = dp(kCounterWidthDip, dpi);
    const int w_nav     = dp(kBtnNavWidthDip,  dpi);
    const int w_case    = dp(kBtnCaseWidthDip, dpi);
    const int w_regex   = dp(kBtnRegexWidthDip, dpi);
    const int w_whole   = dp(kBtnWholeWidthDip, dpi);
    const int w_close   = dp(kBtnCloseWidthDip, dpi);

    SetWindowPos(impl_->edit,      nullptr, cx, pad, w_edit,    ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_edit + pad;
    SetWindowPos(impl_->counter,   nullptr, cx, pad, w_counter, ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_counter + pad;
    SetWindowPos(impl_->btn_prev,  nullptr, cx, pad, w_nav,     ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_nav;
    SetWindowPos(impl_->btn_next,  nullptr, cx, pad, w_nav,     ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_nav + pad;
    SetWindowPos(impl_->btn_case,  nullptr, cx, pad, w_case,    ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_case + pad;
    SetWindowPos(impl_->btn_regex, nullptr, cx, pad, w_regex,   ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_regex + pad;
    SetWindowPos(impl_->btn_whole, nullptr, cx, pad, w_whole,   ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    cx += w_whole + pad;
    SetWindowPos(impl_->btn_close, nullptr, cx, pad, w_close,   ctrl_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void FindBar::set_counter(const std::wstring& txt) {
    if (!impl_ || !impl_->counter) return;
    SetWindowTextW(impl_->counter, txt.c_str());
}

void FindBar::set_invalid_pattern(bool invalid) {
    if (!impl_) return;
    if (impl_->invalid_pattern == invalid) return;
    impl_->invalid_pattern = invalid;
    // Force a repaint so WM_CTLCOLOREDIT re-runs with the new text color.
    if (impl_->edit) InvalidateRect(impl_->edit, nullptr, FALSE);
}

void FindBar::set_on_query_changed(QueryChanged cb) {
    if (impl_) impl_->on_query_changed = std::move(cb);
}
void FindBar::set_on_next (NavAction cb) { if (impl_) impl_->on_next  = std::move(cb); }
void FindBar::set_on_prev (NavAction cb) { if (impl_) impl_->on_prev  = std::move(cb); }
void FindBar::set_on_close(NavAction cb) { if (impl_) impl_->on_close = std::move(cb); }

}  // namespace litepdf::ui
