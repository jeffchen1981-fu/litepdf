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
constexpr int kTabMaxWidthDip = 240;
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

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*bg_normal*/      RGB(0x26, 0x26, 0x26),
            /*bg_hover*/       RGB(0x3A, 0x3A, 0x3A),
            /*bg_active*/      RGB(0x2D, 0x2D, 0x2D),
            /*text_inactive*/  RGB(0xA0, 0xA0, 0xA0),
            /*text_active*/    RGB(0xF2, 0xF2, 0xF2),
            /*separator*/      RGB(0x3A, 0x3A, 0x3A),
            /*accent*/         GetSysColor(COLOR_HOTLIGHT),
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
        /*accent*/         GetSysColor(COLOR_HOTLIGHT),
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

HFONT create_tab_font(UINT dpi, bool bold) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);  // 9 pt
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

void paint_tab_minimal(const DRAWITEMSTRUCT* dis, const std::wstring& label) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    // Background: pale grey for normal, near-white for active.
    const bool is_active = (dis->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(is_active ? RGB(0xFF, 0xFF, 0xFF)
                                           : RGB(0xE8, 0xE8, 0xE8));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Label.
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0x20, 0x20, 0x20));
    RECT text_rc = rc;
    text_rc.left += 8;
    text_rc.right -= 8;
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
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
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_OWNERDRAWFIXED,
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
    paint_tab_minimal(dis, tab->label);
    return true;
}

void TabManager::handle_theme_change() {
    // stub — Task 9 will implement
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
            impl.pressed_tab = -1;
            break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

}  // namespace litepdf::ui
