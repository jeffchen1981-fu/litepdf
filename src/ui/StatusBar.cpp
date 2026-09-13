// LitePDF -- ui::StatusBar: bottom status bar with page indicator + go-to-page.
#include "ui/StatusBar.hpp"

#include "ui/detail/StatusBarMath.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <string>
#include <type_traits>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace litepdf::ui {

namespace {

using unique_hfont = std::unique_ptr<std::remove_pointer_t<HFONT>,
                                     decltype(&DeleteObject)>;

unique_hfont make_unique_hfont(HFONT h) {
    return unique_hfont(h, &DeleteObject);
}

// -----------------------------------------------------------------------------
// Palette — local copy, same deferred-refactor rationale as FindBar's and
// ResultsPanel's ("Kept file-local... TODO(phase-6.x): consolidate into
// ui/Theme.hpp"). Used ONLY when the OS is not in High Contrast mode: HC
// keeps the original GetSysColor path in status_bar_subclass untouched,
// because a system control must track HC automatically (see the comment
// there) -- a custom RGB palette would silence that.
// -----------------------------------------------------------------------------
struct Palette {
    COLORREF bar_bg;
    COLORREF edit_bg;
    COLORREF edit_fg;
    COLORREF label_fg;
    COLORREF disabled_bg;
    COLORREF disabled_fg;
};

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*bar_bg*/      RGB(0x2B, 0x2B, 0x2B),
            /*edit_bg*/     RGB(0x1E, 0x1E, 0x1E),
            /*edit_fg*/     RGB(0xF2, 0xF2, 0xF2),
            /*label_fg*/    RGB(0xB0, 0xB0, 0xB0),
            /*disabled_bg*/ RGB(0x2B, 0x2B, 0x2B),
            /*disabled_fg*/ RGB(0x70, 0x70, 0x70),
        };
    }
    return {
        /*bar_bg*/      RGB(0xEC, 0xEC, 0xEC),
        /*edit_bg*/     RGB(0xFF, 0xFF, 0xFF),
        /*edit_fg*/     RGB(0x1C, 0x1C, 0x1C),
        /*label_fg*/    RGB(0x60, 0x60, 0x60),
        /*disabled_bg*/ RGB(0xEC, 0xEC, 0xEC),
        /*disabled_fg*/ RGB(0x8C, 0x8C, 0x8C),
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

// True while the OS is running under a High Contrast theme. Checked
// independently of dark/light so a HC user's own theme is never overridden by
// this control's custom palette (see the Palette comment above).
bool is_high_contrast_active() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

HFONT create_status_font(UINT dpi, int pt_size = 9) {
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

constexpr int kPadDip    = 4;
constexpr int kEditWDip  = 52;
constexpr int kLabelWDip = 96;

constexpr UINT_PTR kIdEdit         = 1;
constexpr UINT_PTR kIdLabel        = 2;
constexpr UINT_PTR kEditSubclassId = 1;
constexpr UINT_PTR kBarSubclassId  = 2;

// The box holds at most a sloppy paste; parse_page_input rejects anything
// longer on range grounds anyway.
constexpr int kEditTextMax = 15;

}  // namespace

LRESULT CALLBACK status_bar_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l, UINT_PTR id,
                                          DWORD_PTR ref_data);
LRESULT CALLBACK status_bar_subclass(HWND hwnd, UINT msg, WPARAM w,
                                     LPARAM l, UINT_PTR id, DWORD_PTR ref_data);

struct StatusBar::Impl {
    HWND hwnd  = nullptr;
    HWND edit  = nullptr;
    HWND label = nullptr;
    UINT dpi   = 96;
    int  height_px = 0;

    // 0-based current page and the document's page count. -1 / 0 means "no
    // document"; set_empty() restores that state.
    int  cur_page   = -1;
    int  page_count = 0;

    // The exact text the bar last wrote into the box. Compared against the live
    // text to tell "the reader is typing" from "nobody has touched it" --
    // see detail::should_overwrite_page_box.
    std::wstring last_written;

    unique_hfont font { nullptr, &DeleteObject };

    // Dark-mode / High-Contrast state (mirrors ResultsPanel/Splitter's
    // detect_dark_mode + Palette + WM_SETTINGCHANGE hot-swap convention).
    // `high_contrast` gates `palette` off entirely -- see the Palette comment
    // in the anonymous namespace above.
    bool    dark_mode     = false;
    bool    high_contrast = false;
    Palette palette       = make_palette(false);

    // Lazily built background brushes for WM_CTLCOLOREDIT (enabled box) and
    // the disabled WM_CTLCOLORSTATIC branch. Deleted + nulled on a theme
    // swap and rebuilt on the next colour query -- same lifetime pattern as
    // FindBar's / ResultsPanel's edit_brush.
    HBRUSH edit_brush     = nullptr;
    HBRUSH disabled_brush = nullptr;

    // Give the bar control a custom background colour. Visual styles
    // otherwise ignore SB_SETBKCOLOR (a well-known comctl32 v6 gotcha, the
    // same reason ResultsPanel's ListView colours would be at risk), so the
    // control's own themed paint is switched off first -- the standard way
    // to make a status bar's background colour actually stick. Skipped
    // entirely under High Contrast so the OS's own contrast theme keeps
    // drawing the bar untouched.
    void apply_bar_bkcolor() {
        if (!hwnd) return;
        if (high_contrast) {
            // Undo the L"", L"" theme-suppression from the non-HC branch
            // below (nullptr, nullptr restores default visual-style
            // processing) so a light/dark -> High Contrast transition
            // lands the bar back on the OS's own contrast theme instead of
            // staying unthemed. Previously unreachable because nothing
            // delivered WM_SETTINGCHANGE to this control; now that
            // MainWindow forwards it, this path is live.
            SetWindowTheme(hwnd, nullptr, nullptr);
            SendMessageW(hwnd, SB_SETBKCOLOR, 0,
                        static_cast<LPARAM>(CLR_DEFAULT));
            return;
        }
        SetWindowTheme(hwnd, L"", L"");
        SendMessageW(hwnd, SB_SETBKCOLOR, 0,
                    static_cast<LPARAM>(palette.bar_bg));
    }

    StatusBar::OnGoto     on_goto;
    StatusBar::OnFocusOut on_focus_out;
    StatusBar::OnWheel    on_wheel;

    std::wstring edit_text() const {
        if (!edit) return std::wstring();
        wchar_t buf[kEditTextMax + 1] = {};
        const int n = GetWindowTextW(edit, buf, kEditTextMax + 1);
        return std::wstring(buf, (n > 0) ? static_cast<std::size_t>(n) : 0u);
    }

    void write_box(const std::wstring& text) {
        if (!edit) return;
        SetWindowTextW(edit, text.c_str());
        last_written = text;
    }

    // Rewrite the box from cur_page, unconditionally.
    void force_revert() {
        if (cur_page < 0 || page_count <= 0) {
            write_box(L"");
            return;
        }
        write_box(std::to_wstring(cur_page + 1));
    }

    void relayout() {
        if (!hwnd) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        const auto r = detail::status_bar_child_rects(
            rc.bottom - rc.top, dp(kPadDip, dpi),
            dp(kEditWDip, dpi), dp(kLabelWDip, dpi));
        if (edit) {
            SetWindowPos(edit, nullptr, r.edit_x, r.edit_y, r.edit_w, r.edit_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (label) {
            SetWindowPos(label, nullptr, r.label_x, r.label_y,
                         r.label_w, r.label_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Repaint after any text change. BOTH windows must be invalidated, and
    // that is the whole reason this is a function rather than one line.
    //
    // The label erases nothing (NULL_BRUSH + TRANSPARENT bkmode), so the only
    // thing that can clear its old pixels is the BAR painting its background
    // across that strip -- which it can do only because the bar has no
    // WS_CLIPCHILDREN. Invalidating the bar alone is not obviously enough
    // either, since a parent's invalid region does not propagate to children.
    // Invalidate both and rely on the documented order: within one update
    // cycle a parent paints before its children.
    //
    // The case that fails if this is wrong is a SHRINKING label -- "/ 128"
    // becoming "/ 2", or going empty when the last tab closes, leaving "28" or
    // the whole old string behind. Task 4 Step 3 checks exactly that
    // transition; if stale glyphs survive it, the fallback is to give the
    // label an opaque background brush instead of transparency (one line in
    // status_bar_subclass), accepting a possible slight mismatch against a
    // themed bar. Do not "fix" it by deleting either Invalidate call.
    void repaint() {
        if (hwnd)  InvalidateRect(hwnd, nullptr, TRUE);
        if (label) InvalidateRect(label, nullptr, FALSE);
    }

    // Re-measure the control's natural height for the current font.
    void measure() {
        if (!hwnd) return;
        // A status bar computes its own height only when it PROCESSES WM_SIZE.
        // The window is created 0x0, so without this the GetWindowRect below
        // would read 0 every time and the fallback would be the only answer
        // this function ever gave. Sending it here also repositions the control
        // to the parent's bottom edge, which is harmless: on_layout calls
        // set_bounds() immediately afterwards and owns the position from then on.
        SendMessageW(hwnd, WM_SIZE, 0, 0);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const int measured = wr.bottom - wr.top;
        // Guard the degenerate case so on_layout can never reserve a negative
        // strip -- 22 DIP is the classic status bar height at 100%.
        height_px = (measured > 0) ? measured : dp(22, dpi);
    }

    // Commit whatever is in the box.
    void commit() {
        const auto parsed = detail::parse_page_input(edit_text(), page_count);
        if (parsed.has_value() && on_goto) {
            on_goto(*parsed);
        }
        // Normalise unconditionally: a successful goto has already updated
        // cur_page through the owner's page-change observer, and a goto to the
        // page we are already on changes nothing -- both end with the box
        // showing the canonical 1-based page rather than "007" or "129".
        force_revert();
        if (on_focus_out) on_focus_out();
    }
};

// -----------------------------------------------------------------------------
// Status bar subclass -- background brushes for the two children.
//
// The "/ N" STATIC wants to be transparent so it paints over the themed bar
// instead of a grey rectangle: NULL_BRUSH plus TRANSPARENT bkmode means it
// draws text and erases nothing, so the bar underneath must repaint first.
// set_page() invalidates the whole bar with fErase for that reason, and the bar
// is deliberately NOT created with WS_CLIPCHILDREN so the erase reaches under
// the label.
//
// The page box must NOT get that treatment, and this is the trap: Windows
// routes a DISABLED (or read-only) EDIT's background query to
// WM_CTLCOLORSTATIC, not to WM_CTLCOLOREDIT. The box is disabled from
// construction until a document opens -- set_empty() runs at the end of the
// constructor -- so a blanket NULL_BRUSH here would leave the box's client area
// unfilled on the very first frame, and again after Ctrl+W on the last tab.
// The two are told apart by HWND. FindBar handles the same pair with two real
// brushes (FindBar.cpp, WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC) and returns a
// NULL_BRUSH from neither.
// -----------------------------------------------------------------------------
LRESULT CALLBACK status_bar_subclass(HWND hwnd, UINT msg, WPARAM w,
                                     LPARAM l, UINT_PTR /*id*/,
                                     DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<StatusBar::Impl*>(ref_data);

    switch (msg) {
        case WM_CTLCOLOREDIT: {
            // The ENABLED page box. A disabled one is routed to
            // WM_CTLCOLORSTATIC below, not here -- see that case.
            auto hdc = reinterpret_cast<HDC>(w);
            auto ctl = reinterpret_cast<HWND>(l);
            if (!(impl && ctl == impl->edit)) break;
            if (impl->high_contrast) {
                // System colours track High Contrast automatically; see the
                // WM_CTLCOLORSTATIC comment below for the full rationale.
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            SetTextColor(hdc, impl->palette.edit_fg);
            SetBkColor(hdc, impl->palette.edit_bg);
            if (!impl->edit_brush) {
                impl->edit_brush = CreateSolidBrush(impl->palette.edit_bg);
            }
            return reinterpret_cast<LRESULT>(impl->edit_brush);
        }
        case WM_CTLCOLORSTATIC: {
            auto hdc = reinterpret_cast<HDC>(w);
            auto ctl = reinterpret_cast<HWND>(l);
            // Neither branch below falls through to DefSubclassProc -- both
            // return their brush directly -- so this is the only place that
            // will ever set the DC's text colour. Leaving it unset defaults to
            // black, which is invisible against a black High Contrast bar
            // background. Under High Contrast we keep using system colours
            // (not the custom Palette below) because this is a system control
            // that must track HC automatically; FindBar's palette machinery
            // gets away with a fixed RGB set only because that bar is
            // custom-drawn and never has to answer to HC.
            if (impl && ctl == impl->edit) {
                // Disabled page box: the standard disabled-field look.
                if (impl->high_contrast) {
                    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
                    SetBkColor(hdc, GetSysColor(COLOR_3DFACE));
                    return reinterpret_cast<LRESULT>(
                        GetSysColorBrush(COLOR_3DFACE));
                }
                SetTextColor(hdc, impl->palette.disabled_fg);
                SetBkColor(hdc, impl->palette.disabled_bg);
                if (!impl->disabled_brush) {
                    impl->disabled_brush =
                        CreateSolidBrush(impl->palette.disabled_bg);
                }
                return reinterpret_cast<LRESULT>(impl->disabled_brush);
            }
            if (impl && impl->high_contrast) {
                SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
            } else if (impl) {
                SetTextColor(hdc, impl->palette.label_fg);
            } else {
                SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
            }
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_SETTINGCHANGE: {
            // Light theme hot-swap (consistent with FindBar/ResultsPanel/
            // Splitter). Re-derives both dark-mode and High-Contrast state
            // together since either one changes what status_bar_subclass
            // should paint above.
            if (impl) {
                HWND parent = GetParent(hwnd);
                const bool new_dark = detect_dark_mode(parent ? parent : hwnd);
                const bool new_hc   = is_high_contrast_active();
                if (new_dark != impl->dark_mode ||
                    new_hc   != impl->high_contrast) {
                    impl->dark_mode     = new_dark;
                    impl->high_contrast = new_hc;
                    impl->palette       = make_palette(new_dark);
                    if (impl->edit_brush) {
                        DeleteObject(impl->edit_brush);
                        impl->edit_brush = nullptr;
                    }
                    if (impl->disabled_brush) {
                        DeleteObject(impl->disabled_brush);
                        impl->disabled_brush = nullptr;
                    }
                    impl->apply_bar_bkcolor();
                    InvalidateRect(hwnd, nullptr, TRUE);
                    if (impl->edit)  InvalidateRect(impl->edit, nullptr, TRUE);
                    if (impl->label) InvalidateRect(impl->label, nullptr, FALSE);
                }
            }
            break;
        }
        case WM_NCDESTROY:
            if (impl) {
                if (impl->edit_brush) {
                    DeleteObject(impl->edit_brush);
                    impl->edit_brush = nullptr;
                }
                if (impl->disabled_brush) {
                    DeleteObject(impl->disabled_brush);
                    impl->disabled_brush = nullptr;
                }
            }
            RemoveWindowSubclass(hwnd, status_bar_subclass, kBarSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// -----------------------------------------------------------------------------
// Edit subclass -- Enter commits, Esc reverts, and the wheel is handed back to
// the owner. Everything else passes through so ordinary typing, selection and
// paste behave exactly like a plain EDIT.
//
// Pattern mirrors find_bar_edit_subclass in FindBar.cpp.
// -----------------------------------------------------------------------------
LRESULT CALLBACK status_bar_edit_subclass(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l, UINT_PTR /*id*/,
                                          DWORD_PTR ref_data) {
    auto* impl = reinterpret_cast<StatusBar::Impl*>(ref_data);
    if (!impl) return DefSubclassProc(hwnd, msg, w, l);

    switch (msg) {
        case WM_GETDLGCODE:
            // Route Enter/Escape/characters here rather than to the dialog
            // manager, so WM_KEYDOWN below actually sees them.
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS
                 | DefSubclassProc(hwnd, msg, w, l);

        case WM_KEYDOWN:
            // ESC is deliberately absent from this switch. It is a BARE
            // ACCELERATOR in this app, so TranslateAcceleratorW converts it to
            // WM_COMMAND(IDM_FIND_CLOSE) before the message is ever dispatched
            // to this control -- a `case VK_ESCAPE` here would be dead code
            // that reads as live. MainWindow's IDM_FIND_CLOSE arm owns ESC and
            // hands focus back to the canvas; WM_KILLFOCUS below then reverts.
            // (VK_RETURN is NOT in the accelerator table, so it does arrive.)
            if (w == VK_RETURN) {
                impl->commit();
                return 0;  // consumed
            }
            break;

        case WM_CHAR:
            // Swallow the character form so the EDIT does not MessageBeep about
            // a key it cannot handle. Only VK_RETURN can actually arrive here
            // today: the pump skips TranslateMessage entirely for a translated
            // accelerator, so ESC never becomes a WM_CHAR either. The ESC
            // disjunct is kept as a one-token guard against the accelerator
            // table changing, not as a claim that it fires.
            if (w == VK_RETURN || w == VK_ESCAPE) return 0;
            break;

        case WM_KILLFOCUS:
            // THE single place uncommitted text is discarded. Leaving the box
            // by any route -- clicking the canvas, Alt+Tab, ESC, a pane toggle,
            // the commit's own focus handback -- lands here. Without it the box
            // keeps showing a page the document is not on, and worse,
            // should_overwrite_page_box then refuses every later update forever,
            // because the live text no longer matches what the bar last wrote.
            // Idempotent: it rewrites from cur_page, which a completed commit
            // has already updated through the owner's page-change observer.
            impl->force_revert();
            break;  // fall through to the EDIT's own caret teardown

        case WM_MOUSEWHEEL:
            // WM_MOUSEWHEEL is delivered to the FOCUSED window. With the caret
            // in this box the canvas would never see a notch, so hand it over.
            // FindBar's and ResultsPanel's edits do not do this -- there the
            // wheel belongs to their own list.
            if (impl->on_wheel) {
                impl->on_wheel(w, l);
                return 0;
            }
            break;

        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, status_bar_edit_subclass,
                                 kEditSubclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

StatusBar::StatusBar(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>()) {
    impl_->dpi = GetDpiForWindow(parent);
    // Same guard FindBar carries in the identical situation: a 0 here would
    // propagate into dp() and make even measure()'s fallback height 0, which
    // would lay the bar out zero-tall.
    if (impl_->dpi == 0) impl_->dpi = 96;

    impl_->dark_mode     = detect_dark_mode(parent);
    impl_->high_contrast = is_high_contrast_active();
    impl_->palette       = make_palette(impl_->dark_mode);

    // No WS_CLIPCHILDREN: the bar must be able to erase the strip under the
    // label, which paints transparently (see status_bar_subclass).
    impl_->hwnd = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        parent, nullptr, hInstance, nullptr);
    if (!impl_->hwnd) return;

    // ref_data carries Impl* so the WM_CTLCOLORSTATIC arm can tell the page box
    // from the label by HWND. Registered before the children exist, which is
    // fine: impl_->edit is null until then and no colour query can name it.
    SetWindowSubclass(impl_->hwnd, status_bar_subclass, kBarSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));

    impl_->apply_bar_bkcolor();

    impl_->font = make_unique_hfont(create_status_font(impl_->dpi));
    SendMessageW(impl_->hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));

    impl_->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
        0, 0, 0, 0,
        impl_->hwnd, reinterpret_cast<HMENU>(kIdEdit), hInstance, nullptr);
    SendMessageW(impl_->edit, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));
    SendMessageW(impl_->edit, EM_SETLIMITTEXT,
                 static_cast<WPARAM>(kEditTextMax), 0);
    SetWindowSubclass(impl_->edit, status_bar_edit_subclass, kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));

    impl_->label = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        impl_->hwnd, reinterpret_cast<HMENU>(kIdLabel), hInstance, nullptr);
    SendMessageW(impl_->label, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font.get()),
                 MAKELPARAM(TRUE, 0));

    impl_->measure();
    set_empty();
}

StatusBar::~StatusBar() {
    if (impl_ && impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
}

HWND StatusBar::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

int StatusBar::height_px() const { return impl_ ? impl_->height_px : 0; }

void StatusBar::set_bounds(const RECT& bounds) {
    if (!impl_ || !impl_->hwnd) return;
    SetWindowPos(impl_->hwnd, nullptr,
                 bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    impl_->relayout();
}

void StatusBar::update_dpi(UINT dpi) {
    if (!impl_ || !impl_->hwnd) return;
    impl_->dpi = dpi;
    // Keep the old font alive until every WM_SETFONT below has landed.
    // Reassigning impl_->font directly would run the old HFONT's deleter
    // during the assignment, leaving all three windows pointing at a freed
    // GDI handle for the duration of the sends. old_font dies at the end of
    // this function, after nobody references it any more.
    auto old_font = std::move(impl_->font);
    impl_->font    = make_unique_hfont(create_status_font(dpi));
    const WPARAM f = reinterpret_cast<WPARAM>(impl_->font.get());
    SendMessageW(impl_->hwnd, WM_SETFONT, f, MAKELPARAM(TRUE, 0));
    if (impl_->edit)  SendMessageW(impl_->edit,  WM_SETFONT, f, MAKELPARAM(TRUE, 0));
    if (impl_->label) SendMessageW(impl_->label, WM_SETFONT, f, MAKELPARAM(TRUE, 0));
    impl_->measure();
    impl_->relayout();
}

void StatusBar::set_page(int page_index, int page_count) {
    if (!impl_) return;
    // Route the empty case through set_empty() rather than duplicating its
    // focus-handback guard here: EnableWindow on a focused window leaves focus
    // NULL, and MainWindow has no WM_KEYDOWN handler of its own to recover
    // from that. set_empty() already hands focus back before disabling.
    if (page_count <= 0) {
        set_empty();
        return;
    }
    impl_->cur_page   = page_index;
    impl_->page_count = page_count;

    if (impl_->edit) {
        EnableWindow(impl_->edit, TRUE);
        const bool focused = (GetFocus() == impl_->edit);
        if (detail::should_overwrite_page_box(focused, impl_->edit_text(),
                                              impl_->last_written)) {
            impl_->force_revert();
        }
    }
    if (impl_->label) {
        const std::wstring text = L"/ " + std::to_wstring(page_count);
        SetWindowTextW(impl_->label, text.c_str());
    }
    impl_->repaint();
}

void StatusBar::set_empty() {
    if (!impl_) return;
    impl_->cur_page   = -1;
    impl_->page_count = 0;
    if (impl_->edit) {
        // Hand focus back BEFORE disabling. Disabling a window that holds the
        // focus leaves the focus NULL, and MainWindow has no WM_KEYDOWN handler
        // of its own -- the keyboard would be dead until the reader clicked the
        // canvas. Reachable: caret in the box, then Ctrl+W (an accelerator, so
        // it fires regardless of focus) closing the last tab.
        if (GetFocus() == impl_->edit && impl_->on_focus_out) {
            impl_->on_focus_out();
        }
        impl_->write_box(L"");
        EnableWindow(impl_->edit, FALSE);
    }
    if (impl_->label) SetWindowTextW(impl_->label, L"");
    impl_->repaint();
}

bool StatusBar::page_box_has_focus() const {
    return impl_ && impl_->edit && GetFocus() == impl_->edit;
}

void StatusBar::set_on_goto(OnGoto cb) {
    if (impl_) impl_->on_goto = std::move(cb);
}

void StatusBar::set_on_focus_out(OnFocusOut cb) {
    if (impl_) impl_->on_focus_out = std::move(cb);
}

void StatusBar::set_on_wheel(OnWheel cb) {
    if (impl_) impl_->on_wheel = std::move(cb);
}

}  // namespace litepdf::ui
