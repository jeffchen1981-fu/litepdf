#include "ui/TabManager.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace litepdf::ui {

namespace {
constexpr UINT_PTR kTabSubclassId = 0xAB01;

constexpr int kTabHeightDip  = 32;
constexpr int kTabMinWidthDip = 120;
constexpr int kTabMaxWidthDip = 200;
constexpr int kTabPaddingDip  = 12;
constexpr int kCloseSizeDip   = 14;
constexpr int kCloseRightPadDip = 8;

using unique_hfont = std::unique_ptr<std::remove_pointer_t<HFONT>,
                                     decltype(&DeleteObject)>;
unique_hfont make_unique_hfont(HFONT h) {
    return unique_hfont(h, &DeleteObject);
}

struct Palette {
    COLORREF bg_normal;
    COLORREF bg_hover;
    COLORREF bg_active;
    COLORREF text_inactive;
    COLORREF text_active;
    COLORREF separator;
    COLORREF accent;
    COLORREF close_hover_bg;
    COLORREF close_hover_fg;
    COLORREF close_fg;
};

COLORREF resolve_accent_color() {
    DWORD argb = 0;
    BOOL  opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
        // DwmGetColorizationColor returns 0xAARRGGBB; strip alpha and swap
        // to COLORREF's 0x00BBGGRR layout.
        const BYTE r = (argb >> 16) & 0xFF;
        const BYTE g = (argb >>  8) & 0xFF;
        const BYTE b = (argb >>  0) & 0xFF;
        return RGB(r, g, b);
    }
    return GetSysColor(COLOR_HOTLIGHT);
}

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*bg_normal*/      RGB(0x26, 0x26, 0x26),
            /*bg_hover*/       RGB(0x3A, 0x3A, 0x3A),
            /*bg_active*/      RGB(0x2D, 0x2D, 0x2D),
            /*text_inactive*/  RGB(0xA0, 0xA0, 0xA0),
            /*text_active*/    RGB(0xF2, 0xF2, 0xF2),
            /*separator*/      RGB(0x3A, 0x3A, 0x3A),
            /*accent*/         resolve_accent_color(),
            /*close_hover_bg*/ RGB(0xC4, 0x2B, 0x1C),
            /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
            /*close_fg*/       RGB(0xC8, 0xC8, 0xC8),
        };
    }
    return {
        /*bg_normal*/      RGB(0xF3, 0xF3, 0xF3),
        /*bg_hover*/       RGB(0xEA, 0xEA, 0xEA),
        /*bg_active*/      RGB(0xFF, 0xFF, 0xFF),
        /*text_inactive*/  RGB(0x60, 0x60, 0x60),
        /*text_active*/    RGB(0x1C, 0x1C, 0x1C),
        /*separator*/      RGB(0xD8, 0xD8, 0xD8),
        /*accent*/         resolve_accent_color(),
        /*close_hover_bg*/ RGB(0xE8, 0x11, 0x23),
        /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
        /*close_fg*/       RGB(0x50, 0x50, 0x50),
    };
}

bool detect_dark_mode(HWND hwnd) {
    // 1. DWM immersive-dark-mode attr.
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        if (dark) return true;
    }
    // 2. AppsUseLightTheme registry (system-wide app mode).
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
    // 3. Fallback: assume light.
    return false;
}

RECT close_rect_in(const RECT& tab, UINT dpi) {
    const int sz  = MulDiv(kCloseSizeDip, static_cast<int>(dpi), 96);
    const int pad = MulDiv(kCloseRightPadDip, static_cast<int>(dpi), 96);
    const int cy  = (tab.top + tab.bottom) / 2;
    RECT r;
    r.right  = tab.right - pad;
    r.left   = r.right - sz;
    r.top    = cy - sz / 2;
    r.bottom = r.top + sz;
    return r;
}

HFONT create_tab_font(UINT dpi, bool bold) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);  // 9 pt
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

enum class TabVisualState { Normal, Hover, Active };

struct PaintCtx {
    const Palette& palette;
    HFONT font_normal;
    HFONT font_bold;
    UINT  dpi;
};

void paint_tab(const DRAWITEMSTRUCT* dis, const std::wstring& label,
               TabVisualState state, bool draw_right_separator,
               bool show_close, bool close_is_hot,
               const PaintCtx& pc) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    COLORREF bg = pc.palette.bg_normal;
    if (state == TabVisualState::Hover)  bg = pc.palette.bg_hover;
    if (state == TabVisualState::Active) bg = pc.palette.bg_active;

    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);

    if (state == TabVisualState::Active) {
        const int bar_h = MulDiv(3, static_cast<int>(pc.dpi), 96);
        RECT bar = rc;
        bar.bottom = bar.top + bar_h;
        HBRUSH accent = CreateSolidBrush(pc.palette.accent);
        FillRect(hdc, &bar, accent);
        DeleteObject(accent);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (state == TabVisualState::Active)
        ? pc.palette.text_active : pc.palette.text_inactive);
    HFONT chosen = (state == TabVisualState::Active) ? pc.font_bold
                                                     : pc.font_normal;
    HGDIOBJ old_font = SelectObject(hdc, chosen);

    const int pad = MulDiv(kTabPaddingDip, static_cast<int>(pc.dpi), 96);
    RECT text_rc = rc;
    text_rc.left  += pad;
    const int right_pad = show_close
        ? MulDiv(kCloseSizeDip + kCloseRightPadDip + 6,
                 static_cast<int>(pc.dpi), 96)
        : pad;
    text_rc.right = rc.right - right_pad;
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS
                  | DT_NOPREFIX);

    SelectObject(hdc, old_font);

    if (show_close) {
        RECT cb = close_rect_in(rc, pc.dpi);
        if (close_is_hot) {
            HBRUSH hb = CreateSolidBrush(pc.palette.close_hover_bg);
            FillRect(hdc, &cb, hb);
            DeleteObject(hb);
        }
        COLORREF x_color = close_is_hot ? pc.palette.close_hover_fg
                                        : pc.palette.close_fg;
        HPEN pen = CreatePen(PS_SOLID,
                             MulDiv(1, static_cast<int>(pc.dpi), 96),
                             x_color);
        HGDIOBJ old_pen = SelectObject(hdc, pen);
        const int inset = MulDiv(3, static_cast<int>(pc.dpi), 96);
        MoveToEx(hdc, cb.left + inset, cb.top + inset, nullptr);
        LineTo  (hdc, cb.right - inset, cb.bottom - inset);
        MoveToEx(hdc, cb.right - inset, cb.top + inset, nullptr);
        LineTo  (hdc, cb.left + inset, cb.bottom - inset);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);
    }

    if (draw_right_separator && state != TabVisualState::Active) {
        const int sep_w = 1;
        RECT sep = rc;
        sep.left = rc.right - sep_w;
        const int inset = MulDiv(6, static_cast<int>(pc.dpi), 96);
        sep.top    += inset;
        sep.bottom -= inset;
        HBRUSH b = CreateSolidBrush(pc.palette.separator);
        FillRect(hdc, &sep, b);
        DeleteObject(b);
    }
}
}  // namespace

// Forward declaration at namespace scope (not anonymous) so the friend
// declaration in TabManager matches by exact linkage / lookup.
LRESULT CALLBACK tab_subclass_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                   UINT_PTR id, DWORD_PTR ref_data);

struct TabManager::Impl {
    HWND                        hwnd = nullptr;
    litepdf::core::TabList      list;
    TabManager::SwitchCb        on_switch;
    TabManager::CloseRequestCb  on_close_request;

    // I1 gesture: index of tab where WM_MBUTTONDOWN fired; set to -1
    // between gestures. Mouse is captured between DOWN and UP so we
    // receive the UP even if released off-tab.
    int                         pressed_tab = -1;

    // Called by the subclass proc on WM_MBUTTONUP when the hit-tested tab
    // index is valid. Kept as a member (rather than inline in the proc) so
    // the callback invocation lives in a named C++ function, which static
    // analyzers prefer over loose access from a free-function subclass proc.
    void fire_close_request(int index) {
        if (on_close_request) on_close_request(index);
    }

    int  hovered_tab    = -1;
    bool mouse_tracking = false;
    bool hovered_close = false;
    int  pressed_close = -1;

    RECT tab_rect(int i) const {
        RECT r = {};
        if (hwnd && i >= 0) {
            SendMessageW(hwnd, TCM_GETITEMRECT, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&r));
        }
        return r;
    }
    void invalidate_tab(int i) {
        if (!hwnd || i < 0) return;
        RECT r = tab_rect(i);
        InvalidateRect(hwnd, &r, FALSE);
    }

    unique_hfont font_normal { nullptr, &DeleteObject };
    unique_hfont font_bold   { nullptr, &DeleteObject };
    UINT cached_dpi = 0;

    bool dark_mode = false;
    Palette palette = make_palette(false);

    void ensure_fonts(UINT dpi) {
        if (cached_dpi == dpi && font_normal && font_bold) return;
        font_normal = make_unique_hfont(create_tab_font(dpi, /*bold=*/false));
        font_bold   = make_unique_hfont(create_tab_font(dpi, /*bold=*/true));
        cached_dpi  = dpi;
    }
};

TabManager::TabManager(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    impl_->hwnd = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH,
        0, 0, 0, 0,
        parent, nullptr, hInstance, nullptr);
    // Subclass so we can catch WM_MBUTTONUP (middle-click close).
    SetWindowSubclass(impl_->hwnd, tab_subclass_proc,
                      kTabSubclassId, reinterpret_cast<DWORD_PTR>(this));

    // Baseline item size; paint code still computes per-tab width.
    const UINT dpi = GetDpiForWindow(impl_->hwnd);
    SendMessageW(impl_->hwnd, TCM_SETITEMSIZE, 0,
                 MAKELPARAM(MulDiv(kTabMaxWidthDip, dpi, 96),
                            MulDiv(kTabHeightDip, dpi, 96)));
    impl_->ensure_fonts(dpi);
    SendMessageW(impl_->hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font_normal.get()),
                 MAKELPARAM(TRUE, 0));

    impl_->dark_mode = detect_dark_mode(parent);
    impl_->palette   = make_palette(impl_->dark_mode);
}

TabManager::~TabManager() {
    if (impl_ && impl_->hwnd) {
        RemoveWindowSubclass(impl_->hwnd, tab_subclass_proc, kTabSubclassId);
        DestroyWindow(impl_->hwnd);
    }
}

HWND TabManager::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }
int  TabManager::count()        const { return impl_ ? static_cast<int>(impl_->list.size()) : 0; }
int  TabManager::active_index() const { return impl_ ? impl_->list.active_index() : -1; }

litepdf::core::Tab* TabManager::active_tab() {
    return impl_ ? impl_->list.active() : nullptr;
}
litepdf::core::Tab* TabManager::tab_at(int index) {
    if (!impl_ || index < 0) return nullptr;
    return impl_->list.at(static_cast<std::size_t>(index));
}

int TabManager::add_tab(std::unique_ptr<litepdf::core::Tab> t) {
    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT;
    tci.pszText = const_cast<LPWSTR>(t->label.c_str());

    const int old_active = impl_->list.active_index();
    const int new_index  = static_cast<int>(impl_->list.add(std::move(t)));
    SendMessageW(impl_->hwnd, TCM_INSERTITEMW,
                 static_cast<WPARAM>(new_index),
                 reinterpret_cast<LPARAM>(&tci));

    impl_->list.set_active(static_cast<std::size_t>(new_index));
    SendMessageW(impl_->hwnd, TCM_SETCURSEL,
                 static_cast<WPARAM>(new_index), 0);
    if (impl_->on_switch) impl_->on_switch(new_index, old_active);
    return new_index;
}

void TabManager::close_tab(int index) {
    if (index < 0 || index >= count()) return;
    const int old_active = impl_->list.active_index();
    const int new_active = impl_->list.remove(static_cast<std::size_t>(index));
    SendMessageW(impl_->hwnd, TCM_DELETEITEM,
                 static_cast<WPARAM>(index), 0);
    if (new_active >= 0) {
        SendMessageW(impl_->hwnd, TCM_SETCURSEL,
                     static_cast<WPARAM>(new_active), 0);
    }
    if (new_active != old_active && impl_->on_switch) {
        impl_->on_switch(new_active, old_active);
    }
}

bool TabManager::set_active(int index) {
    if (index < 0) return false;
    const int old_active = impl_->list.active_index();
    if (!impl_->list.set_active(static_cast<std::size_t>(index))) return false;
    SendMessageW(impl_->hwnd, TCM_SETCURSEL,
                 static_cast<WPARAM>(index), 0);
    if (impl_->on_switch) impl_->on_switch(index, old_active);
    return true;
}

void TabManager::set_tab_label(int index, const std::wstring& label) {
    if (index < 0 || index >= count()) return;
    if (auto* t = impl_->list.at(static_cast<std::size_t>(index))) {
        t->label = label;
    }
    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT;
    tci.pszText = const_cast<LPWSTR>(label.c_str());
    SendMessageW(impl_->hwnd, TCM_SETITEMW,
                 static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&tci));
}

void TabManager::set_on_switch(SwitchCb cb) {
    impl_->on_switch = std::move(cb);
}
void TabManager::set_on_close_request(CloseRequestCb cb) {
    impl_->on_close_request = std::move(cb);
}

bool TabManager::handle_notify(const NMHDR* hdr) {
    // Note: TCM_SETCURSEL (fired from set_active / close_tab / add_tab) does
    // NOT emit TCN_SELCHANGE — programmatic selection changes are silent by
    // design. Those paths invoke on_switch directly, so set_active is the
    // single authoritative notification route for programmatic changes, and
    // this TCN_SELCHANGE branch only fires on genuine user interaction.
    if (!hdr || hdr->hwndFrom != impl_->hwnd) return false;
    if (hdr->code == TCN_SELCHANGE) {
        const int new_index = static_cast<int>(
            SendMessageW(impl_->hwnd, TCM_GETCURSEL, 0, 0));
        const int old_active = impl_->list.active_index();
        if (new_index >= 0 && new_index != old_active) {
            impl_->list.set_active(static_cast<std::size_t>(new_index));
            if (impl_->on_switch) impl_->on_switch(new_index, old_active);
        }
        return true;
    }
    return false;
}

int TabManager::strip_height(UINT dpi) const {
    if (!impl_ || !impl_->hwnd) return 0;
    if (dpi == 0) dpi = GetDpiForWindow(impl_->hwnd);
    return MulDiv(kTabHeightDip, static_cast<int>(dpi), 96);
}

void TabManager::set_visible(bool v) {
    if (impl_ && impl_->hwnd) {
        ShowWindow(impl_->hwnd, v ? SW_SHOW : SW_HIDE);
    }
}

bool TabManager::handle_draw_item(const DRAWITEMSTRUCT* dis) {
    if (!impl_ || !dis || dis->hwndItem != impl_->hwnd) return false;
    const int idx = static_cast<int>(dis->itemID);
    if (idx < 0 || idx >= count()) return false;
    auto* tab = impl_->list.at(static_cast<std::size_t>(idx));
    if (!tab) return false;

    const UINT dpi = GetDpiForWindow(impl_->hwnd);
    impl_->ensure_fonts(dpi);

    const bool is_active = (dis->itemState & ODS_SELECTED) != 0;
    TabVisualState state = TabVisualState::Normal;
    if (is_active) {
        state = TabVisualState::Active;
    } else if (idx == impl_->hovered_tab) {
        state = TabVisualState::Hover;
    }

    PaintCtx pc { impl_->palette, impl_->font_normal.get(),
                  impl_->font_bold.get(), dpi };
    const bool has_next = (idx + 1) < count();
    const bool next_is_active = has_next && (idx + 1 == active_index());
    const bool draw_sep = has_next && !is_active && !next_is_active;
    const bool show_close = is_active ||
                            (idx == impl_->hovered_tab);
    const bool close_is_hot = show_close &&
                              (idx == impl_->hovered_tab) &&
                              impl_->hovered_close;
    paint_tab(dis, tab->label, state, draw_sep,
              show_close, close_is_hot, pc);
    return true;
}

void TabManager::handle_theme_change() {
    if (!impl_ || !impl_->hwnd) return;
    HWND parent = GetParent(impl_->hwnd);
    const bool new_dark = detect_dark_mode(parent ? parent : impl_->hwnd);
    if (new_dark == impl_->dark_mode) return;
    impl_->dark_mode = new_dark;
    impl_->palette   = make_palette(new_dark);
    InvalidateRect(impl_->hwnd, nullptr, TRUE);
}

LRESULT CALLBACK tab_subclass_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                   UINT_PTR /*id*/, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<TabManager*>(ref_data);
    if (!self || !self->impl_) {
        return DefSubclassProc(hwnd, msg, w, l);
    }
    auto& impl = *self->impl_;

    switch (msg) {
        case WM_MBUTTONDOWN: {
            TCHITTESTINFO hti = {};
            hti.pt.x = GET_X_LPARAM(l);
            hti.pt.y = GET_Y_LPARAM(l);
            const int hit = static_cast<int>(
                SendMessageW(hwnd, TCM_HITTEST, 0,
                             reinterpret_cast<LPARAM>(&hti)));
            impl.pressed_tab = hit;  // -1 if no tab under cursor
            if (hit >= 0) SetCapture(hwnd);
            break;
        }
        case WM_MBUTTONUP: {
            const int pressed = impl.pressed_tab;
            impl.pressed_tab = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (pressed < 0) break;
            TCHITTESTINFO hti = {};
            hti.pt.x = GET_X_LPARAM(l);
            hti.pt.y = GET_Y_LPARAM(l);
            const int released_on = static_cast<int>(
                SendMessageW(hwnd, TCM_HITTEST, 0,
                             reinterpret_cast<LPARAM>(&hti)));
            // Fire only when DOWN and UP hit-test to the same tab index.
            if (released_on == pressed) {
                impl.fire_close_request(pressed);
            }
            break;
        }
        case WM_CAPTURECHANGED:
            // Another window stole capture (Alt+Tab, dialog, etc.).
            // Cancel the in-flight gesture.
            impl.pressed_tab   = -1;
            impl.pressed_close = -1;
            break;
        case WM_LBUTTONDOWN: {
            TCHITTESTINFO hti = {};
            hti.pt.x = GET_X_LPARAM(l);
            hti.pt.y = GET_Y_LPARAM(l);
            const int i = static_cast<int>(
                SendMessageW(hwnd, TCM_HITTEST, 0,
                             reinterpret_cast<LPARAM>(&hti)));
            if (i >= 0) {
                RECT tr = impl.tab_rect(i);
                RECT cb = close_rect_in(tr, GetDpiForWindow(hwnd));
                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                if (PtInRect(&cb, pt)) {
                    impl.pressed_close = i;
                    SetCapture(hwnd);
                    impl.invalidate_tab(i);
                    return 0;  // block tab-select
                }
            }
            break;  // fall through for tab-select
        }
        case WM_LBUTTONUP: {
            if (impl.pressed_close >= 0) {
                const int pressed = impl.pressed_close;
                impl.pressed_close = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                TCHITTESTINFO hti = {};
                hti.pt.x = GET_X_LPARAM(l);
                hti.pt.y = GET_Y_LPARAM(l);
                const int released_on = static_cast<int>(
                    SendMessageW(hwnd, TCM_HITTEST, 0,
                                 reinterpret_cast<LPARAM>(&hti)));
                if (released_on == pressed) {
                    RECT tr = impl.tab_rect(released_on);
                    RECT cb = close_rect_in(tr, GetDpiForWindow(hwnd));
                    POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                    if (PtInRect(&cb, pt)) {
                        impl.fire_close_request(released_on);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            TCHITTESTINFO hti = {};
            hti.pt.x = GET_X_LPARAM(l);
            hti.pt.y = GET_Y_LPARAM(l);
            const int i = static_cast<int>(
                SendMessageW(hwnd, TCM_HITTEST, 0,
                             reinterpret_cast<LPARAM>(&hti)));
            if (i != impl.hovered_tab) {
                const int old_hover = impl.hovered_tab;
                impl.hovered_tab = i;
                if (old_hover >= 0) impl.invalidate_tab(old_hover);
                if (i >= 0) impl.invalidate_tab(i);
            }
            bool new_close_hover = false;
            if (i >= 0) {
                RECT tr = impl.tab_rect(i);
                RECT cb = close_rect_in(tr, GetDpiForWindow(hwnd));
                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                new_close_hover = PtInRect(&cb, pt);
            }
            if (new_close_hover != impl.hovered_close) {
                impl.hovered_close = new_close_hover;
                if (i >= 0) impl.invalidate_tab(i);
            }
            if (!impl.mouse_tracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                impl.mouse_tracking = true;
            }
            break;
        }
        case WM_MOUSELEAVE: {
            impl.mouse_tracking = false;
            const int old_hover = impl.hovered_tab;
            impl.hovered_tab = -1;
            impl.hovered_close = false;
            if (old_hover >= 0) impl.invalidate_tab(old_hover);
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

}  // namespace litepdf::ui
