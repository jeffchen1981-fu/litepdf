// ui::VerticalSplitter — Phase 7 Task 4b. Thin wrapper over the shared
// detail::SplitterCore declared in ui/detail/SplitterCore.hpp. The dispatch
// logic (handle_message) is defined out-of-line in src/ui/Splitter.cpp and
// reused via the linker — DO NOT redefine handle_message here.
//
// Differences from src/ui/Splitter.cpp (the horizontal sibling):
//   - Distinct kWndClass name to avoid RegisterClassExW collision.
//   - core.orient = Orientation::Vertical.
//   - WNDCLASSEXW.hCursor = IDC_SIZEWE (left/right resize) instead of IDC_SIZENS.
//
// Everything else (Impl shell, WndProc forwarding pattern, ctor/dtor flow,
// set_on_drag/set_bounds forwarders) mirrors Splitter.cpp.

#include "ui/VerticalSplitter.hpp"

#include "ui/detail/SplitterCore.hpp"
#include "ui/detail/SplitterMath.hpp"

#include <windowsx.h>

#include <memory>
#include <mutex>
#include <utility>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace litepdf::ui {

namespace {

constexpr wchar_t kWndClass[] = L"LitePDFVerticalSplitter";

std::once_flag g_class_once;

// Forward-declare so the WNDCLASSEX registration below can name it.
LRESULT CALLBACK vertical_splitter_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

}  // namespace

// ----------------------------------------------------------------------------
// VerticalSplitter::Impl — thin shell that owns a detail::SplitterCore. The
// Core's HWND is the splitter's HWND; the Core is what GWLP_USERDATA points to.
// ----------------------------------------------------------------------------
struct VerticalSplitter::Impl {
    detail::SplitterCore core;
};

namespace {

LRESULT CALLBACK vertical_splitter_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
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
        wc.lpfnWndProc   = vertical_splitter_wndproc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_SIZEWE);
        wc.hbrBackground = nullptr;  // we paint bg in WM_ERASEBKGND
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
    });
}

}  // namespace

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
VerticalSplitter::VerticalSplitter(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    register_class_once(hInstance);

    impl_->core.orient    = detail::Orientation::Vertical;
    impl_->core.dark_mode = detail::detect_dark_mode(parent);
    impl_->core.palette   = detail::make_palette(impl_->core.dark_mode);

    impl_->core.hwnd = CreateWindowExW(
        0, kWndClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent, nullptr, hInstance, &impl_->core);
}

VerticalSplitter::~VerticalSplitter() {
    if (impl_ && impl_->core.hwnd) {
        DestroyWindow(impl_->core.hwnd);
        impl_->core.hwnd = nullptr;
    }
}

HWND VerticalSplitter::hwnd() const { return impl_ ? impl_->core.hwnd : nullptr; }

void VerticalSplitter::set_on_drag(OnDrag cb) {
    if (impl_) impl_->core.on_drag = std::move(cb);
}

void VerticalSplitter::set_bounds(const RECT& bounds) {
    if (!impl_ || !impl_->core.hwnd) return;
    SetWindowPos(impl_->core.hwnd, nullptr,
                 bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace litepdf::ui
