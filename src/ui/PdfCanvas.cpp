// LitePDF — ui::PdfCanvas: child HWND with a Direct2D render target.
#include "ui/PdfCanvas.hpp"

#include <d2d1.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <stdexcept>
#pragma comment(lib, "d2d1.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kCanvasClassName[] = L"LitePDFPdfCanvas";
bool g_class_registered = false;
}  // namespace

namespace litepdf::ui {

struct PdfCanvas::Impl {
    ComPtr<ID2D1Factory>          factory;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap>           current_bitmap;  // Task 6 populates
    D2D1_SIZE_U                   last_size = { 0, 0 };
};

void PdfCanvas::register_class_once(HINSTANCE hInstance) {
    if (g_class_registered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PdfCanvas::WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // we paint ourselves (WM_ERASEBKGND returns 1)
    wc.lpszClassName = kCanvasClassName;
    if (!RegisterClassExW(&wc))
        throw std::runtime_error("Failed to register PdfCanvas window class");
    g_class_registered = true;
}

PdfCanvas::PdfCanvas(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>()) {
    register_class_once(hInstance);

    RECT rc;
    GetClientRect(parent, &rc);

    hwnd_ = CreateWindowExW(
        0, kCanvasClassName, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent, nullptr, hInstance, this);
    if (!hwnd_)
        throw std::runtime_error("Failed to create PdfCanvas HWND");

    // D2D factory — single-threaded (we only draw on UI thread).
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory),
                                   reinterpret_cast<void**>(impl_->factory.GetAddressOf()));
    if (FAILED(hr))
        throw std::runtime_error("D2D1CreateFactory failed");
}

PdfCanvas::~PdfCanvas() = default;

LRESULT CALLBACK PdfCanvas::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    PdfCanvas* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<PdfCanvas*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PdfCanvas*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self)
        return self->handle_message(hwnd, msg, w, l);
    return DefWindowProcW(hwnd, msg, w, l);
}

LRESULT PdfCanvas::handle_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_SIZE:
            on_size(LOWORD(l), HIWORD(l));
            return 0;
        case WM_ERASEBKGND:
            return 1;  // prevent flicker; we paint the background in on_paint
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_DPICHANGED_BEFOREPARENT:
            // DPI is changing. Discard render target; next paint rebuilds at new DPI.
            // current_bitmap is sized for the OLD DPI — discard it too
            // (discard_render_target() resets both).
            discard_render_target();
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            // Parent just repositioned us. Invalidate so on_paint rebuilds the rt.
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

void PdfCanvas::create_render_target() {
    if (impl_->rt) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U sz = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top));
    if (sz.width == 0) sz.width = 1;
    if (sz.height == 0) sz.height = 1;

    HRESULT hr = impl_->factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, sz),
        &impl_->rt);
    if (FAILED(hr)) {
        // Leave rt null; next paint tries again.
        impl_->rt.Reset();
        return;
    }
    impl_->last_size = sz;
}

void PdfCanvas::discard_render_target() {
    impl_->current_bitmap.Reset();
    impl_->rt.Reset();
}

void PdfCanvas::on_size(int w, int h) {
    if (impl_->rt && (w > 0 && h > 0)) {
        D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(w),
                                     static_cast<UINT32>(h));
        HRESULT hr = impl_->rt->Resize(sz);
        if (hr == D2DERR_RECREATE_TARGET) {
            discard_render_target();
        } else if (SUCCEEDED(hr)) {
            impl_->last_size = sz;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::on_paint() {
    create_render_target();
    if (!impl_->rt) {
        ValidateRect(hwnd_, nullptr);  // nothing to do
        return;
    }
    impl_->rt->BeginDraw();
    impl_->rt->Clear(D2D1::ColorF(0xF0F0F0));  // light gray

    // Task 6 draws current_bitmap here.

    HRESULT hr = impl_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_render_target();
        // Next paint rebuilds; schedule one.
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    ValidateRect(hwnd_, nullptr);
}

}  // namespace litepdf::ui
