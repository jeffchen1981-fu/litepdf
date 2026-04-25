#include "ui/Splitter.hpp"

#include "ui/detail/SplitterCore.hpp"
#include "ui/detail/SplitterMath.hpp"

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

std::once_flag g_class_once;

// Forward-declare so the WNDCLASSEX registration below can name it.
LRESULT CALLBACK splitter_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

}  // namespace

// ----------------------------------------------------------------------------
// Splitter::Impl — thin shell that owns a detail::SplitterCore. The Core's
// HWND is the splitter's HWND; the Core is what GWLP_USERDATA points to.
// ----------------------------------------------------------------------------
struct Splitter::Impl {
    detail::SplitterCore core;
};

}  // namespace litepdf::ui

// ----------------------------------------------------------------------------
// detail::SplitterCore::handle_message — defined here (rather than in the
// header) so VerticalSplitter.cpp (Phase 7 T4b) reuses the SAME symbol via
// the linker. Both Splitter.cpp and VerticalSplitter.cpp include the header
// and link against this single out-of-line definition.
//
// The helpers it calls (make_palette / detect_dark_mode / compute_drag_target_*)
// are inline in headers, so the dispatcher does not depend on any file-local
// symbol from Splitter.cpp.
// ----------------------------------------------------------------------------
namespace litepdf::ui::detail {

LRESULT SplitterCore::handle_message(HWND hwnd_in, UINT msg, WPARAM w, LPARAM l) {
    // Local clamp bounds. Mirrors Splitter.cpp's anonymous-namespace
    // constants so VerticalSplitter.cpp doesn't have to re-define them.
    // (Keeping them inside this function — rather than as inline header
    // constants — keeps the public detail surface minimal.)
    constexpr int kMinExtentPx     = 100;
    constexpr int kReservePx       = 200;

    switch (msg) {
        case WM_SETCURSOR: {
            // Show the resize cursor (vertical for horizontal splitter,
            // horizontal for vertical splitter) while hovering OR dragging.
            // Returning TRUE tells Windows we set the cursor so it won't use
            // the class cursor.
            LPCWSTR cursor = (orient == Orientation::Horizontal)
                                 ? IDC_SIZENS
                                 : IDC_SIZEWE;
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            if (!hover) {
                hover = true;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd_in, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd_in, nullptr, FALSE);
            }
            if (dragging && on_drag) {
                // Convert the mouse position to PARENT-CLIENT coords and
                // delegate the orientation-specific from-edge math + clamp
                // to compute_drag_target_{x,y}.
                //
                // Why parent-client and not screen coords: pre-refactor we
                // used `bottom_left.y - pt_screen.y`, where both terms were
                // ClientToScreen-mapped from the parent. Subtracting cancels
                // the screen-origin offset, so working entirely in parent-
                // client is bit-equivalent: parent_h - mouse_y_in_parent ==
                // bottom_y_screen - mouse_y_screen.
                POINT pt_screen;
                pt_screen.x = GET_X_LPARAM(l);
                pt_screen.y = GET_Y_LPARAM(l);
                ClientToScreen(hwnd_in, &pt_screen);

                HWND parent = GetParent(hwnd_in);
                if (parent) {
                    POINT pt_in_parent = pt_screen;
                    ScreenToClient(parent, &pt_in_parent);

                    RECT pr;
                    GetClientRect(parent, &pr);

                    if (orient == Orientation::Horizontal) {
                        // Horizontal splitter: panel anchored at bottom.
                        // panel_h = parent_h - mouse_y_in_parent_client.
                        const int max_h = (std::max)(
                            kMinExtentPx,
                            static_cast<int>(pr.bottom) - kReservePx);
                        const int new_h = compute_drag_target_y(
                            static_cast<int>(pt_in_parent.y),
                            static_cast<int>(pr.bottom),
                            kMinExtentPx,
                            max_h);
                        on_drag(new_h);
                    } else {
                        // Vertical splitter (Phase 7 T4b): pane anchored at
                        // left. pane_w = mouse_x_in_parent_client. parent_w
                        // is unused in compute_drag_target_x but passed for
                        // signature symmetry.
                        const int max_w = (std::max)(
                            kMinExtentPx,
                            static_cast<int>(pr.right) - kReservePx);
                        const int new_w = compute_drag_target_x(
                            static_cast<int>(pt_in_parent.x),
                            static_cast<int>(pr.right),
                            kMinExtentPx,
                            max_w);
                        on_drag(new_w);
                    }
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (hover && !dragging) {
                hover = false;
                InvalidateRect(hwnd_in, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            dragging = true;
            SetCapture(hwnd_in);
            InvalidateRect(hwnd_in, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (dragging) {
                dragging = false;
                if (GetCapture() == hwnd_in) ReleaseCapture();
                InvalidateRect(hwnd_in, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CAPTURECHANGED: {
            // Another window stole capture (Alt+Tab, dialog). Clear drag
            // state cleanly so we don't keep firing on_drag once capture
            // returns elsewhere.
            if (dragging) {
                dragging = false;
                InvalidateRect(hwnd_in, nullptr, FALSE);
            }
            return 0;
        }

        case WM_ERASEBKGND: {
            HDC  hdc = reinterpret_cast<HDC>(w);
            RECT rc;
            GetClientRect(hwnd_in, &rc);
            const COLORREF col = (hover || dragging)
                                     ? palette.bar_hover
                                     : palette.bar_bg;
            HBRUSH br = CreateSolidBrush(col);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            return 1;
        }

        case WM_SETTINGCHANGE: {
            // Light theme hot-swap (consistent with FindBar/TabManager).
            HWND parent = GetParent(hwnd_in);
            const bool new_dark = detect_dark_mode(parent ? parent : hwnd_in);
            if (new_dark != dark_mode) {
                dark_mode = new_dark;
                palette   = make_palette(new_dark);
                InvalidateRect(hwnd_in, nullptr, TRUE);
            }
            return 0;
        }

        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd_in, GWLP_USERDATA, 0);
            break;
        }
    }
    return DefWindowProcW(hwnd_in, msg, w, l);
}

}  // namespace litepdf::ui::detail

namespace litepdf::ui {

namespace {

LRESULT CALLBACK splitter_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* core = reinterpret_cast<detail::SplitterCore*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        core = static_cast<detail::SplitterCore*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(core));
        return 0;
    }

    if (!core) return DefWindowProcW(hwnd, msg, w, l);
    return core->handle_message(hwnd, msg, w, l);
}

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

    impl_->core.orient    = detail::Orientation::Horizontal;
    impl_->core.dark_mode = detail::detect_dark_mode(parent);
    impl_->core.palette   = detail::make_palette(impl_->core.dark_mode);

    impl_->core.hwnd = CreateWindowExW(
        0, kWndClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent, nullptr, hInstance, &impl_->core);
}

Splitter::~Splitter() {
    if (impl_ && impl_->core.hwnd) {
        DestroyWindow(impl_->core.hwnd);
        impl_->core.hwnd = nullptr;
    }
}

HWND Splitter::hwnd() const { return impl_ ? impl_->core.hwnd : nullptr; }

void Splitter::set_on_drag(OnDrag cb) {
    if (impl_) impl_->core.on_drag = std::move(cb);
}

void Splitter::set_bounds(const RECT& bounds) {
    if (!impl_ || !impl_->core.hwnd) return;
    SetWindowPos(impl_->core.hwnd, nullptr,
                 bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace litepdf::ui
