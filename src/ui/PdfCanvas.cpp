// LitePDF — ui::PdfCanvas: child HWND with a Direct2D render target.
#include "ui/PdfCanvas.hpp"

#include "MainMenu.rc.h"
#include "core/DocumentView.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/PdfCanvasLayout.hpp"

#include <d2d1.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
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
    fz_context* fz_clone_context(fz_context*);
    void        fz_drop_context(fz_context*);
}

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kCanvasClassName[] = L"LitePDFPdfCanvas";
std::once_flag g_class_registered;
}  // namespace

namespace litepdf::ui {

// Internal helper: shared escrow + Post logic for both the LEFT/single
// slot (msg = WM_USER_RENDER_DONE) and the RIGHT slot (msg =
// WM_USER_RENDER_DONE_RIGHT). Refcount discipline is identical for both;
// only the message ID changes.
namespace {
bool post_render_done_impl(HWND target, UINT msg,
                           fz_pixmap* pix, fz_context* worker_ctx) {
    if (!pix) {
        PostMessageW(target, msg,
                     reinterpret_cast<WPARAM>(nullptr),
                     static_cast<LPARAM>(0));
        return true;
    }
    fz_context* escrow = fz_clone_context(worker_ctx);
    if (!escrow) {
        fz_drop_pixmap(worker_ctx, pix);
        return false;
    }
    if (!PostMessageW(target, msg,
                      reinterpret_cast<WPARAM>(pix),
                      reinterpret_cast<LPARAM>(escrow))) {
        fz_drop_pixmap(escrow, pix);
        fz_drop_context(escrow);
        return false;
    }
    return true;
}
}  // namespace

bool PdfCanvas::post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx) {
    return post_render_done_impl(target, WM_USER_RENDER_DONE, pix, worker_ctx);
}

bool PdfCanvas::post_render_done_right(HWND target,
                                       fz_pixmap* pix,
                                       fz_context* worker_ctx) {
    return post_render_done_impl(target, WM_USER_RENDER_DONE_RIGHT, pix, worker_ctx);
}

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

    // --- Phase 6 Task 9: search hit overlay ---
    // Device-bound brushes. Created in create_render_target, released
    // in discard_render_target (mirroring the rt lifecycle).
    ComPtr<ID2D1SolidColorBrush>  brush_hit_other_fill;
    ComPtr<ID2D1SolidColorBrush>  brush_hit_current_fill;
    ComPtr<ID2D1SolidColorBrush>  brush_hit_current_stroke;

    // Hits source and current-hit marker. hits_fn may be null if the
    // owner hasn't wired search yet — paint loop treats that as "no
    // overlay". current_hit is std::nullopt when no hit is selected.
    PdfCanvas::HitsFn             hits_fn;
    std::optional<litepdf::core::SearchSession::Hit> current_hit;

    // (Phase 8 D7) chrome polarity. Default off (light); flipped on
    // Ctrl+Shift+I via the active view's set_invert_colors handler.
    bool                          invert_chrome = false;

    // (Phase 8 D10) two-page-spread layout state. The right slot
    // bitmap is populated by WM_USER_RENDER_DONE_RIGHT handler when
    // dual_page is on. Reset to null both on dual→single transition
    // and on every page-change in dual mode (the left slot's
    // current_bitmap is also reset on page-change).
    bool                          dual_page = false;
    ComPtr<ID2D1Bitmap>           right_bitmap;

    // --- Phase 7 Task 7: page-change observer ---
    // Fired by change_current_page() after a real page transition, and
    // by set_view() when a non-null view is installed (so a freshly
    // switched tab's listener learns the page immediately, not "stale
    // until first PgDn"). May be null — checked at the fire site.
    PdfCanvas::PageChangedCb      on_page_changed;
};

void PdfCanvas::set_view(litepdf::core::DocumentView* view) {
    // Per-render escrow (see PdfCanvas::post_render_done) is now the
    // lifetime mechanism for in-flight pixmaps — a canvas-level
    // orphan_ctx clone is no longer needed. The canvas just repoints
    // its view reference; the old view's pending pixmaps carry their
    // own escrow ctx and drop correctly regardless of this swap.
    impl_->view = view;
    if (!view) {
        // No active view — whatever bitmap is on screen is tied to a
        // ctx that will soon be gone. Discard so the next paint shows
        // the cleared background.
        impl_->current_bitmap.Reset();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // T7: tab-switch fire-point. The new tab's DocumentView already has
    // its own current_page_ (carried across switches) — broadcast it so
    // a per-tab listener (T8 will wire ThumbnailPane here) shows the
    // correct highlight immediately rather than waiting for the next
    // page-change event. The listener is responsible for no-op'ing if
    // the value matches what it already has.
    if (impl_->on_page_changed) {
        impl_->on_page_changed(view->current_page());
    }
}

void PdfCanvas::set_on_page_changed(PageChangedCb cb) {
    if (!impl_) return;
    impl_->on_page_changed = std::move(cb);
}

bool PdfCanvas::change_current_page(int idx) {
    if (!impl_ || !impl_->view) return false;
    const bool changed = impl_->view->set_current_page(idx);
    if (changed && impl_->on_page_changed) {
        impl_->on_page_changed(impl_->view->current_page());
    }
    return changed;
}

PdfCanvas::Pan PdfCanvas::pan() const {
    if (!impl_) return { 0.0f, 0.0f };
    return { impl_->pan_x, impl_->pan_y };
}

void PdfCanvas::set_pan(float x, float y) {
    if (!impl_) return;
    impl_->pan_x = x;
    impl_->pan_y = y;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::set_hits_source(HitsFn fn) {
    if (!impl_) return;
    impl_->hits_fn = std::move(fn);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::set_current_hit(std::optional<litepdf::core::SearchSession::Hit> h) {
    if (!impl_) return;
    impl_->current_hit = std::move(h);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::set_invert_chrome(bool on) {
    if (!impl_ || impl_->invert_chrome == on) return;
    impl_->invert_chrome = on;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::set_dual_page(bool on) {
    if (!impl_ || impl_->dual_page == on) return;
    impl_->dual_page = on;
    // Always discard the right bitmap on toggle: when going dual→single
    // there is no right slot, and when going single→dual the right slot
    // must be repopulated by the next WM_USER_RENDER_DONE_RIGHT — until
    // it lands, paint a blank placeholder. Drop pan too; the page-pair
    // origin computed by on_paint is different from the single-page
    // origin and a stale pan would push content off-canvas.
    impl_->right_bitmap.Reset();
    impl_->pan_x = 0.0f;
    impl_->pan_y = 0.0f;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool PdfCanvas::dual_page() const noexcept {
    return impl_ && impl_->dual_page;
}

void PdfCanvas::scroll_into_view(const litepdf::core::SearchSession::Hit& h) {
    if (!impl_ || !impl_->view || !hwnd_) return;

    // Page switch if needed. Caller drives kick_render afterwards.
    const int target_pg = static_cast<int>(h.page);
    if (impl_->view->current_page() != target_pg) {
        // Route through change_current_page so the T7 observer fires —
        // search-jump is a real page transition just like PgUp/PgDn.
        change_current_page(target_pg);
        // Reset pan so the new page lands at its natural fit origin;
        // new_pan_y below will then recenter on the hit once the fresh
        // bitmap arrives. Without this, a large stale pan from the old
        // page could push the hit off-screen on the new page.
        impl_->pan_x = 0.0f;
        impl_->pan_y = 0.0f;
    }

    // Transform the hit quad to viewport-DIP space using the same
    // pdf_pt → DIP mapping as on_paint.
    //   quad_dip_top = dst.top  + ul_y * zoom_scale * fit_scale
    //   quad_dip_bot = dst.top  + ll_y * zoom_scale * fit_scale
    // We may not have a bitmap yet (e.g. after a page change), in which
    // case fit_scale is unknown. Fall back to InvalidateRect only.
    if (!impl_->current_bitmap || !impl_->rt) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    D2D1_SIZE_F src = impl_->current_bitmap->GetSize();
    D2D1_SIZE_F vp  = impl_->rt->GetSize();
    float fit_scale = vp.width / src.width;
    if (src.height * fit_scale > vp.height) fit_scale = vp.height / src.height;
    const float dst_h_unpanned = src.height * fit_scale;
    const float dy = (vp.height - dst_h_unpanned) * 0.5f;

    const float zoom = impl_->view->zoom_scale();
    const float s    = zoom * fit_scale;

    // Axis-aligned Y span of the quad in pdf-points.
    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });

    // Already-visible check uses the current pan_y.
    const float q_top_dip = dy + impl_->pan_y + q_min_y_pt * s;
    const float q_bot_dip = dy + impl_->pan_y + q_max_y_pt * s;
    const float margin    = 24.0f;
    if (q_top_dip >= margin && q_bot_dip <= vp.height - margin) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Pan so the quad's center sits at the viewport's vertical center.
    // center_dip = dy + pan_y + q_center_pt * s = vp.height * 0.5
    const float q_center_pt = (q_min_y_pt + q_max_y_pt) * 0.5f;
    impl_->pan_y = vp.height * 0.5f - dy - q_center_pt * s;

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::register_class_once(HINSTANCE hInstance) {
    std::call_once(g_class_registered, [&]() {
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
    });
}

PdfCanvas::PdfCanvas(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>()) {
    register_class_once(hInstance);

    RECT rc;
    GetClientRect(parent, &rc);

    // WS_CLIPSIBLINGS is critical: without it, Direct2D's bitmap blit
    // in on_paint() overwrites the screen pixels of overlapping sibling
    // HWNDs (e.g., the Phase 6 FindBar anchored to canvas top-right).
    // Those siblings are NOT reinvalidated on canvas paint, so they
    // effectively disappear — only system-rendered chrome (e.g., Edit
    // caret blink) remains visible. WS_CLIPSIBLINGS tells GDI/D2D to
    // exclude sibling areas from this window's update region.
    hwnd_ = CreateWindowExW(
        0, kCanvasClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
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
        case WM_KEYDOWN: {
            // Defense-in-depth for tab-navigation shortcuts. Ctrl+Tab /
            // Ctrl+Shift+Tab / Ctrl+W are registered in the main window's
            // accelerator table (see MainWindow::run) and TranslateAccel
            // usually converts them to WM_COMMAND before DispatchMessage
            // ever reaches us. But that conversion depends on the async
            // GetKeyState snapshot at message-retrieval time, which has
            // been observed to briefly desync under heavy repeat input
            // (user report: holding PgDn while hitting Ctrl+Tab with two
            // tabs open swallowed the Ctrl+Tab). Canvas owns keyboard
            // focus in that scenario, so if the accelerator misses, the
            // WM_KEYDOWN lands here -- forward it to the parent as
            // WM_COMMAND so the existing IDM_TAB_* dispatch still fires.
            // Posted rather than Sent so tab-close teardown (which swaps
            // canvas->view) cannot run inside our own WndProc frame.
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && w == VK_TAB) {
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const WORD id = shift ? IDM_TAB_PREV : IDM_TAB_NEXT;
                // HIWORD=1 marks the WM_COMMAND as accelerator-sourced,
                // matching what TranslateAcceleratorW would have posted.
                PostMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM(id, 1), 0);
                return 0;
            }
            if (ctrl && w == 'W') {
                PostMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM(IDM_TAB_CLOSE, 1), 0);
                return 0;
            }
            return on_key_down(w);
        }
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
                        [target](fz_pixmap* p, fz_context* worker_ctx) {
                            PdfCanvas::post_render_done(target, p, worker_ctx);
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
        case WM_USER_RENDER_DONE:
        case WM_USER_RENDER_DONE_RIGHT: {
            const bool is_right = (msg == WM_USER_RENDER_DONE_RIGHT);
            auto* pix    = reinterpret_cast<fz_pixmap*>(w);
            auto* escrow = reinterpret_cast<fz_context*>(l);

            if (!pix) {
                // Render failed or cancelled (helper posts WPARAM=0 here).
                // escrow will also be null on this path — nothing to drop.
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (!escrow) {
                // Defensive: pixmap without escrow indicates a bypassed
                // post_render_done helper. Don't drop on a guessed ctx.
                return 0;
            }

            // (Phase 8 D10) If a RIGHT-slot pixmap arrives but dual mode
            // is no longer active, drop it instead of letting it land on
            // the (now-irrelevant) right slot. Same for the corner where
            // the right slot delivers AFTER a single→dual→single ping-
            // pong but the cancel hadn't yet drained the worker.
            if (is_right && !impl_->dual_page) {
                fz_drop_pixmap(escrow, pix);
                fz_drop_context(escrow);
                return 0;
            }

            const int w_px   = fz_pixmap_width(escrow, pix);
            const int h_px   = fz_pixmap_height(escrow, pix);
            const int stride = fz_pixmap_stride(escrow, pix);
            unsigned char* samples = fz_pixmap_samples(escrow, pix);

            create_render_target();
            if (!impl_->rt) {
                fz_drop_pixmap(escrow, pix);
                fz_drop_context(escrow);
                return 0;
            }

            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE));
            ComPtr<ID2D1Bitmap> bmp;
            HRESULT hr = impl_->rt->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(w_px),
                            static_cast<UINT32>(h_px)),
                samples, static_cast<UINT32>(stride), &props, &bmp);

            fz_drop_pixmap(escrow, pix);
            fz_drop_context(escrow);

            if (hr == D2DERR_RECREATE_TARGET) {
                discard_render_target();
                resubmit_current_page();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (FAILED(hr)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (is_right) {
                impl_->right_bitmap = std::move(bmp);
            } else {
                impl_->current_bitmap = std::move(bmp);
                // Reset pan only when the LEFT slot lands — that's the
                // page-change anchor. A right-slot delivery during a
                // dual-mode page transition arrives after the left
                // bitmap and must NOT clobber the freshly reset pan.
                impl_->pan_x = 0.0f;
                impl_->pan_y = 0.0f;
                ColdStartTimer::mark(3);  // first pixmap -> D2D bitmap
            }
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

    // Hit-overlay brushes. Yellow fill for non-current hits; orange fill
    // + darker orange stroke for the current hit. All three are device-
    // bound and must be recreated on device loss (released in
    // discard_render_target). Failures just leave the ComPtr null — the
    // paint loop null-checks before use.
    impl_->rt->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 0.0f, 0.40f),
        &impl_->brush_hit_other_fill);
    impl_->rt->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 0.647f, 0.0f, 0.50f),
        &impl_->brush_hit_current_fill);
    impl_->rt->CreateSolidColorBrush(
        D2D1::ColorF(0.8f, 0.467f, 0.0f, 1.0f),
        &impl_->brush_hit_current_stroke);
}

void PdfCanvas::discard_render_target() {
    // Brushes first — they're device-bound to the rt.
    impl_->brush_hit_other_fill.Reset();
    impl_->brush_hit_current_fill.Reset();
    impl_->brush_hit_current_stroke.Reset();
    impl_->current_bitmap.Reset();
    impl_->rt.Reset();
}

void PdfCanvas::resubmit_current_page() {
    if (!impl_->view) return;
    HWND target = hwnd_;
    if (impl_->dual_page) {
        // (Phase 8 D10) Spread mode: D2DERR_RECREATE_TARGET recovery
        // also has to cover the right slot or the right page stays
        // blank until the user pages forward. Same submission shape as
        // on_key_down's dual branch.
        const int cur   = impl_->view->current_page();
        const int total = impl_->view->page_count();
        const int left  = dual_page_compute_left(cur, total);
        const int right = dual_page_compute_right(left, total);
        impl_->view->cancel_stale_renders(0);
        impl_->view->request_render(left,
            [target](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target, p, worker_ctx);
            });
        if (right >= 0) {
            impl_->view->request_render(right,
                [target](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target, p, worker_ctx);
                });
        }
        return;
    }
    impl_->view->request_render_with_prefetch(
        impl_->view->current_page(),
        [target](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target, p, worker_ctx);
        });
}

LRESULT PdfCanvas::on_key_down(WPARAM key) {
    if (!impl_->view) return 0;

    int cur     = impl_->view->current_page();
    int max_idx = impl_->view->page_count() - 1;

    bool changed = false;
    switch (key) {
        case VK_NEXT: {  // PgDn
            int next;
            if (impl_->dual_page) {
                // (Phase 8 T4) Snap from the current spread's LEFT page,
                // letting dual_page_step_next_left handle the cover→1
                // bootstrap explicitly — a plain `cur_left + 2` stride
                // overshoots from cover (0+2=2) and skips spread (1,2).
                const int total    = impl_->view->page_count();
                const int cur_left = dual_page_compute_left(cur, total);
                next = dual_page_step_next_left(cur_left, total);
            } else {
                next = std::min(cur + 1, max_idx);
            }
            changed = change_current_page(next);
            break;
        }
        case VK_PRIOR: {  // PgUp
            int prev;
            if (impl_->dual_page) {
                // Symmetric step-back via the helper. `cur_left == 1`
                // (first spread) snaps to 0 (cover); `cur_left >= 3`
                // walks back by 2.
                const int total    = impl_->view->page_count();
                const int cur_left = dual_page_compute_left(cur, total);
                prev = dual_page_step_prev_left(cur_left, total);
            } else {
                prev = std::max(cur - 1, 0);
            }
            changed = change_current_page(prev);
            break;
        }
        case VK_HOME:
            changed = change_current_page(0);
            break;
        case VK_END:
            changed = change_current_page(max_idx);
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
        HWND target = hwnd_;
        if (impl_->dual_page) {
            // (Phase 8 D10) Spread mode: pair changed, so both bitmaps
            // are stale. Clear them so on_paint shows the chrome
            // background (and the empty-right placeholder when the new
            // pair has no right page) until the new renders land.
            impl_->current_bitmap.Reset();
            impl_->right_bitmap.Reset();
            impl_->view->cancel_stale_renders(0);
            // Defensive re-snap: D15 (programmatic page-jump) and any
            // future entry point that leaves current_page on a non-LEFT-
            // aligned page must not silently mis-pair the spread. Snap
            // the LEFT here (and write it back so observers see the
            // canonical state) instead of trusting current_page raw.
            const int total    = impl_->view->page_count();
            const int left     = dual_page_compute_left(
                                     impl_->view->current_page(), total);
            if (left != impl_->view->current_page()) {
                impl_->view->set_current_page(left);
            }
            const int right = dual_page_compute_right(left, total);
            impl_->view->request_render(left,
                [target](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done(target, p, worker_ctx);
                });
            if (right >= 0) {
                impl_->view->request_render(right,
                    [target](fz_pixmap* p, fz_context* worker_ctx) {
                        PdfCanvas::post_render_done_right(target, p, worker_ctx);
                    });
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            // Cancel stale renders from rapid paging, submit P0 for current
            // page, and prefetch prev/next at P1 (Task 11). Cache fills
            // happen at the engine level so the next PgUp/PgDn is instant.
            impl_->view->request_render_with_prefetch(
                impl_->view->current_page(),
                [target](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done(target, p, worker_ctx);
                });
        }
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
    const bool had_bitmap = static_cast<bool>(impl_->current_bitmap);

    impl_->rt->BeginDraw();
    // (Phase 8 D7) Chrome polarity flip — dark canvas for Invert Colors,
    // matches the inverted page bitmap so the surround does not glare.
    const UINT32 bg_rgb = impl_->invert_chrome ? 0x202020u : 0xF0F0F0u;
    impl_->rt->Clear(D2D1::ColorF(bg_rgb));

    // (Phase 8 D10) Two-page-spread layout. Each page gets a half-
    // canvas-width slot minus an 8 DIP gutter. Cover-page (page 0) and
    // odd-tail cases render the LEFT slot only and leave the RIGHT slot
    // as an empty placeholder rectangle. Search-hit overlay is disabled
    // in dual mode for v1 (R17 — deferred per plan §"Out of scope").
    if (impl_->dual_page) {
        D2D1_SIZE_F vp = impl_->rt->GetSize();
        const float gutter   = 8.0f;
        const float slot_w   = std::max(0.0f, (vp.width - gutter) * 0.5f);
        const float slot_h   = vp.height;
        const float left_x0  = 0.0f;
        const float right_x0 = slot_w + gutter;

        auto draw_slot = [&](ID2D1Bitmap* bm, float x0) {
            if (!bm) return;
            D2D1_SIZE_F src = bm->GetSize();
            float fit = (src.width  > 0.0f) ? (slot_w / src.width)  : 1.0f;
            if (src.height * fit > slot_h && src.height > 0.0f) {
                fit = slot_h / src.height;
            }
            float dst_w = src.width  * fit;
            float dst_h = src.height * fit;
            float dx = x0 + (slot_w - dst_w) * 0.5f;
            float dy = (slot_h - dst_h) * 0.5f;
            D2D1_RECT_F dst = D2D1::RectF(
                dx + impl_->pan_x,         dy + impl_->pan_y,
                dx + dst_w + impl_->pan_x, dy + dst_h + impl_->pan_y);
            impl_->rt->DrawBitmap(bm, dst, 1.0f,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        };
        draw_slot(impl_->current_bitmap.Get(), left_x0);
        draw_slot(impl_->right_bitmap.Get(),   right_x0);

        // Empty-right placeholder when there is no right page (cover or
        // odd-tail). Without it the user's first Ctrl+Shift+D press on
        // page 0 looks suspiciously like "nothing happened" — see plan
        // R15 for the rationale.
        if (!impl_->right_bitmap) {
            const D2D1_RECT_F r = D2D1::RectF(
                right_x0 + 4.0f, 4.0f,
                right_x0 + slot_w - 4.0f, slot_h - 4.0f);
            ComPtr<ID2D1SolidColorBrush> placeholder;
            const UINT32 ph_rgb = impl_->invert_chrome ? 0x404040u : 0xC0C0C0u;
            if (SUCCEEDED(impl_->rt->CreateSolidColorBrush(
                    D2D1::ColorF(ph_rgb), &placeholder))) {
                impl_->rt->DrawRectangle(r, placeholder.Get(), 1.0f);
            }
        }

        HRESULT hr_dual = impl_->rt->EndDraw();
        if (hr_dual == D2DERR_RECREATE_TARGET) {
            discard_render_target();
            resubmit_current_page();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (SUCCEEDED(hr_dual) && had_bitmap) {
            ColdStartTimer::mark(4);
            ColdStartTimer::emit_if_complete(log_timings_);
        }
        ValidateRect(hwnd_, nullptr);
        return;
    }

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

        // --- Phase 6 Task 9: search hit overlay ---
        // PDF-point → viewport-DIP mapping:
        //   bitmap_pixel = pdf_point * zoom_scale   (RenderEngine passes
        //     fz_scale(scale, scale) to MuPDF; bitmap DPI is the D2D
        //     default of 96, so bitmap DIPs equal bitmap pixels.)
        //   viewport_DIP = bitmap_DIP * scale + dst_origin
        // where scale/dst_origin are the fit-to-viewport values above.
        // Combined: viewport_DIP = pdf_pt * (zoom_scale * scale)
        //                        + (dst.left, dst.top).
        // MuPDF 1.24 page coords are top-left origin, Y-down — matching
        // D2D — so no Y-flip. Quads are drawn as axis-aligned bounding
        // boxes (v1); rotated-text quads would need a transformed
        // geometry in a follow-up.
        if (impl_->hits_fn && impl_->view
            && impl_->brush_hit_other_fill && impl_->brush_hit_current_fill) {
            const float zoom = impl_->view->zoom_scale();
            const float s    = zoom * scale;
            const float ox   = dst.left;
            const float oy   = dst.top;
            const std::size_t pg = static_cast<std::size_t>(
                impl_->view->current_page());

            const auto hits = impl_->hits_fn(pg);
            for (const auto& hit : hits) {
                const auto& q = hit.geom;
                // Axis-aligned bounding box from the 4 quad corners.
                const float min_x = std::min({ q.ul_x, q.ur_x, q.ll_x, q.lr_x });
                const float max_x = std::max({ q.ul_x, q.ur_x, q.ll_x, q.lr_x });
                const float min_y = std::min({ q.ul_y, q.ur_y, q.ll_y, q.lr_y });
                const float max_y = std::max({ q.ul_y, q.ur_y, q.ll_y, q.lr_y });

                D2D1_RECT_F r = D2D1::RectF(
                    ox + min_x * s,
                    oy + min_y * s,
                    ox + max_x * s,
                    oy + max_y * s);

                const bool is_current =
                    impl_->current_hit.has_value()
                    && impl_->current_hit->page == hit.page
                    && impl_->current_hit->geom.ul_x == q.ul_x
                    && impl_->current_hit->geom.ul_y == q.ul_y
                    && impl_->current_hit->geom.lr_x == q.lr_x
                    && impl_->current_hit->geom.lr_y == q.lr_y;

                if (is_current) {
                    impl_->rt->FillRectangle(r, impl_->brush_hit_current_fill.Get());
                    if (impl_->brush_hit_current_stroke) {
                        impl_->rt->DrawRectangle(
                            r, impl_->brush_hit_current_stroke.Get(), 1.0f);
                    }
                } else {
                    impl_->rt->FillRectangle(r, impl_->brush_hit_other_fill.Get());
                }
            }
        }
    }

    HRESULT hr = impl_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_render_target();
        resubmit_current_page();
        // Next paint rebuilds; schedule one.
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (SUCCEEDED(hr) && had_bitmap) {
        // T4 — first paint that actually drew a real bitmap. mark()/emit
        // are both idempotent so subsequent paints are no-ops.
        ColdStartTimer::mark(4);
        ColdStartTimer::emit_if_complete(log_timings_);
    }
    ValidateRect(hwnd_, nullptr);
}

}  // namespace litepdf::ui
