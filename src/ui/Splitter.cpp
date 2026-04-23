#include "ui/Splitter.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace litepdf::ui {

namespace {

constexpr wchar_t kWndClass[] = L"LitePDFSplitter";

// Clamp bounds for the panel height driven by the drag. Caller can further
// clamp; these are a sane floor/ceiling so the panel never collapses to 0
// or pushes the main content off the top of the window.
constexpr int kMinPanelHeightPx      = 100;
constexpr int kBottomReservePx       = 200;  // min space above the splitter

std::once_flag g_class_once;

// ----------------------------------------------------------------------------
// Palette. Kept file-local (same deferred-refactor rationale as FindBar).
// TODO(phase-6.x): consolidate with FindBar/TabManager Palettes into
// ui/Theme.hpp.
// ----------------------------------------------------------------------------
struct Palette {
    COLORREF bar_bg;
    COLORREF bar_hover;
};

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*bar_bg*/    RGB(0x3A, 0x3A, 0x3A),
            /*bar_hover*/ RGB(0x50, 0x50, 0x50),
        };
    }
    return {
        /*bar_bg*/    RGB(0xD8, 0xD8, 0xD8),
        /*bar_hover*/ RGB(0xB8, 0xB8, 0xB8),
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

}  // namespace

// Forward-declare so Impl can reference via function pointer.
LRESULT CALLBACK splitter_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

// ----------------------------------------------------------------------------
// Impl — private state. Stored in GWLP_USERDATA during WM_CREATE.
// ----------------------------------------------------------------------------
struct Splitter::Impl {
    HWND hwnd = nullptr;

    // Drag gesture state. `dragging` mirrors GetCapture()==hwnd for clarity
    // and so we don't repeatedly call the WinAPI during WM_MOUSEMOVE.
    bool dragging = false;
    bool hover    = false;

    Splitter::OnDrag on_drag;

    bool    dark_mode = false;
    Palette palette   = make_palette(false);
};

LRESULT CALLBACK splitter_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* impl = reinterpret_cast<Splitter::Impl*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        impl = static_cast<Splitter::Impl*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(impl));
        return 0;
    }

    if (!impl) return DefWindowProcW(hwnd, msg, w, l);

    switch (msg) {
        case WM_SETCURSOR: {
            // Show vertical-resize cursor while hovering OR dragging. Returning
            // TRUE tells Windows we set the cursor so it won't use the class
            // cursor.
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            if (!impl->hover) {
                impl->hover = true;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (impl->dragging && impl->on_drag) {
                // Compute desired panel height = distance from bottom of
                // parent client to current cursor position. We work in
                // screen coords so a jitter in hwnd's own position during
                // the drag doesn't feed back into the delta calculation.
                POINT pt_screen;
                pt_screen.x = GET_X_LPARAM(l);
                pt_screen.y = GET_Y_LPARAM(l);
                ClientToScreen(hwnd, &pt_screen);

                HWND parent = GetParent(hwnd);
                if (parent) {
                    RECT pr;
                    GetClientRect(parent, &pr);
                    POINT bottom_left = { 0, pr.bottom };
                    ClientToScreen(parent, &bottom_left);

                    int new_h = static_cast<int>(bottom_left.y - pt_screen.y);
                    const int max_h = std::max(
                        kMinPanelHeightPx,
                        static_cast<int>(pr.bottom) - kBottomReservePx);
                    if (new_h < kMinPanelHeightPx) new_h = kMinPanelHeightPx;
                    if (new_h > max_h)             new_h = max_h;
                    impl->on_drag(new_h);
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (impl->hover && !impl->dragging) {
                impl->hover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            impl->dragging = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (impl->dragging) {
                impl->dragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CAPTURECHANGED: {
            // Another window stole capture (Alt+Tab, dialog). Clear drag
            // state cleanly so we don't keep firing on_drag once capture
            // returns elsewhere.
            if (impl->dragging) {
                impl->dragging = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_ERASEBKGND: {
            HDC  hdc = reinterpret_cast<HDC>(w);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const COLORREF col = (impl->hover || impl->dragging)
                                     ? impl->palette.bar_hover
                                     : impl->palette.bar_bg;
            HBRUSH br = CreateSolidBrush(col);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            return 1;
        }

        case WM_SETTINGCHANGE: {
            // Light theme hot-swap (consistent with FindBar/TabManager).
            HWND parent = GetParent(hwnd);
            const bool new_dark = detect_dark_mode(parent ? parent : hwnd);
            if (new_dark != impl->dark_mode) {
                impl->dark_mode = new_dark;
                impl->palette   = make_palette(new_dark);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_NCDESTROY: {
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
        wc.lpfnWndProc   = splitter_wndproc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_SIZENS);
        wc.hbrBackground = nullptr;  // we paint bg in WM_ERASEBKGND
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
    });
}

}  // namespace

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
Splitter::Splitter(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    register_class_once(hInstance);

    impl_->dark_mode = detect_dark_mode(parent);
    impl_->palette   = make_palette(impl_->dark_mode);

    impl_->hwnd = CreateWindowExW(
        0, kWndClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent, nullptr, hInstance, impl_.get());
}

Splitter::~Splitter() {
    if (impl_ && impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
}

HWND Splitter::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

void Splitter::set_on_drag(OnDrag cb) {
    if (impl_) impl_->on_drag = std::move(cb);
}

void Splitter::set_bounds(const RECT& bounds) {
    if (!impl_ || !impl_->hwnd) return;
    SetWindowPos(impl_->hwnd, nullptr,
                 bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace litepdf::ui
