// LitePDF — ui::PdfCanvas: child HWND with a Direct2D render target.
#include "ui/PdfCanvas.hpp"

#include "core/DocumentView.hpp"

#include <d2d1.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <stdexcept>
#pragma comment(lib, "d2d1.lib")

// Forward-decl MuPDF APIs we need on the UI thread so we don't have to
// pull <mupdf/fitz.h> into this translation unit. fz_context / fz_pixmap
// are already forward-declared via core/DocumentView.hpp.
extern "C" {
    int  fz_pixmap_width(fz_context*, fz_pixmap*);
    int  fz_pixmap_height(fz_context*, fz_pixmap*);
    int  fz_pixmap_stride(fz_context*, fz_pixmap*);
    unsigned char* fz_pixmap_samples(fz_context*, fz_pixmap*);
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    struct fz_pixmap* fz_keep_pixmap(struct fz_context*, struct fz_pixmap*);
}

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
    litepdf::core::DocumentView*  view = nullptr;  // non-owning
    // Pan offset in DIPs from the centered/fit position. Reset whenever a
    // new bitmap arrives. No clamping in Phase 3 — user can pan off-canvas.
    float                         pan_x = 0.0f;
    float                         pan_y = 0.0f;
};

void PdfCanvas::set_view(litepdf::core::DocumentView* view) {
    impl_->view = view;
    // Clearing the view invalidates whatever bitmap was tied to its ctx.
    if (!view) {
        impl_->current_bitmap.Reset();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

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
        case WM_LBUTTONDOWN:
            // Click-to-focus: ensures keystrokes (PgUp/PgDn/Home/End) reach us.
            SetFocus(hwnd_);
            return 0;
        case WM_KEYDOWN:
            return on_key_down(w);
        case WM_MOUSEWHEEL: {
            if (!impl_->view) return 0;
            WORD modifiers = GET_KEYSTATE_WPARAM(w);
            if (modifiers & MK_CONTROL) {
                int delta = GET_WHEEL_DELTA_WPARAM(w);
                bool changed = (delta > 0) ? impl_->view->zoom_in()
                                           : impl_->view->zoom_out();
                if (changed) {
                    impl_->view->cancel_stale_renders(0);
                    HWND target = hwnd_;
                    impl_->view->request_render(
                        impl_->view->current_page(),
                        [target](fz_pixmap* p, fz_context* wc) {
                            if (p) fz_keep_pixmap(wc, p);
                            PostMessageW(target, WM_USER_RENDER_DONE,
                                         reinterpret_cast<WPARAM>(p), 0);
                        });
                }
                return 0;
            }
            return DefWindowProcW(hwnd_, WM_MOUSEWHEEL, w, l);
        }
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
        case WM_USER_RENDER_DONE: {
            auto* pix = reinterpret_cast<fz_pixmap*>(w);
            if (!pix) {
                // Render failed (unsupported page etc); just invalidate so paint
                // shows the cleared background.
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (!impl_->view) {
                // View was swapped out; drop the orphan pixmap has no ctx here.
                // Shouldn't happen in practice since set_view precedes render
                // requests. For Phase 3, we leak — pathological path.
                return 0;
            }
            fz_context* ui_ctx = impl_->view->ui_ctx();

            const int w_px   = fz_pixmap_width(ui_ctx, pix);
            const int h_px   = fz_pixmap_height(ui_ctx, pix);
            const int stride = fz_pixmap_stride(ui_ctx, pix);
            unsigned char* samples = fz_pixmap_samples(ui_ctx, pix);

            create_render_target();
            if (!impl_->rt) {
                fz_drop_pixmap(ui_ctx, pix);
                return 0;
            }

            // BGRA -> ID2D1Bitmap. MuPDF produced fz_device_bgr + alpha=1,
            // which matches DXGI_FORMAT_B8G8R8A8_UNORM byte-for-byte. Tell D2D
            // to IGNORE alpha so page edges render fully opaque over the
            // canvas clear color.
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE));
            ComPtr<ID2D1Bitmap> bmp;
            HRESULT hr = impl_->rt->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(w_px),
                            static_cast<UINT32>(h_px)),
                samples, static_cast<UINT32>(stride), &props, &bmp);

            // Drop the worker-kept pixmap ref regardless of CreateBitmap outcome.
            fz_drop_pixmap(ui_ctx, pix);

            if (hr == D2DERR_RECREATE_TARGET) {
                discard_render_target();
                resubmit_current_page();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (FAILED(hr)) {
                // Leave current_bitmap unchanged; just repaint.
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            impl_->current_bitmap = std::move(bmp);
            // Reset pan whenever a new bitmap arrives — zoom/page change
            // both re-render, and we don't try to preserve the view point.
            impl_->pan_x = 0.0f;
            impl_->pan_y = 0.0f;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
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

void PdfCanvas::resubmit_current_page() {
    if (!impl_->view) return;
    HWND target = hwnd_;
    impl_->view->request_render_with_prefetch(
        impl_->view->current_page(),
        [target](fz_pixmap* p, fz_context* worker_ctx) {
            if (p) fz_keep_pixmap(worker_ctx, p);
            PostMessageW(target, WM_USER_RENDER_DONE,
                         reinterpret_cast<WPARAM>(p), 0);
        });
}

LRESULT PdfCanvas::on_key_down(WPARAM key) {
    if (!impl_->view) return 0;

    int cur     = impl_->view->current_page();
    int max_idx = impl_->view->page_count() - 1;

    bool changed = false;
    switch (key) {
        case VK_NEXT:   // PgDn
            changed = impl_->view->set_current_page(cur + 1);
            break;
        case VK_PRIOR:  // PgUp
            changed = impl_->view->set_current_page(cur - 1);
            break;
        case VK_HOME:
            changed = impl_->view->set_current_page(0);
            break;
        case VK_END:
            changed = impl_->view->set_current_page(max_idx);
            break;
        // Arrow keys: pan the bitmap by 100 DIPs. No clamping in Phase 3 —
        // user can pan off-canvas; clamping is Phase 4 polish.
        case VK_LEFT:
            impl_->pan_x += 100.0f;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case VK_RIGHT:
            impl_->pan_x -= 100.0f;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case VK_UP:
            impl_->pan_y += 100.0f;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case VK_DOWN:
            impl_->pan_y -= 100.0f;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        default:
            return 0;
    }

    if (changed) {
        // Cancel stale renders from rapid paging, submit P0 for current
        // page, and prefetch prev/next at P1 (Task 11). Cache fills
        // happen at the engine level so the next PgUp/PgDn is instant.
        HWND target = hwnd_;
        impl_->view->request_render_with_prefetch(
            impl_->view->current_page(),
            [target](fz_pixmap* p, fz_context* worker_ctx) {
                if (p) fz_keep_pixmap(worker_ctx, p);
                PostMessageW(target, WM_USER_RENDER_DONE,
                             reinterpret_cast<WPARAM>(p), 0);
            });
    }
    return 0;
}

void PdfCanvas::on_size(int w, int h) {
    if (impl_->rt && (w > 0 && h > 0)) {
        D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(w),
                                     static_cast<UINT32>(h));
        HRESULT hr = impl_->rt->Resize(sz);
        if (hr == D2DERR_RECREATE_TARGET) {
            discard_render_target();
            resubmit_current_page();
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

    if (impl_->current_bitmap) {
        D2D1_SIZE_F src = impl_->current_bitmap->GetSize();  // DIPs at rt's DPI
        D2D1_SIZE_F vp  = impl_->rt->GetSize();

        // Fit the bitmap inside the viewport preserving aspect ratio.
        float scale = vp.width / src.width;
        if (src.height * scale > vp.height) scale = vp.height / src.height;
        float dst_w = src.width * scale;
        float dst_h = src.height * scale;
        float dx = (vp.width  - dst_w) * 0.5f;
        float dy = (vp.height - dst_h) * 0.5f;

        D2D1_RECT_F dst = D2D1::RectF(dx + impl_->pan_x,
                                      dy + impl_->pan_y,
                                      dx + dst_w + impl_->pan_x,
                                      dy + dst_h + impl_->pan_y);
        impl_->rt->DrawBitmap(impl_->current_bitmap.Get(), dst, 1.0f,
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    HRESULT hr = impl_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_render_target();
        resubmit_current_page();
        // Next paint rebuilds; schedule one.
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    ValidateRect(hwnd_, nullptr);
}

}  // namespace litepdf::ui
