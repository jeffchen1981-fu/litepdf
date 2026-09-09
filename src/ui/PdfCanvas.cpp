// LitePDF — ui::PdfCanvas: child HWND with a Direct2D render target.
#include "ui/PdfCanvas.hpp"

#include "MainMenu.rc.h"
#include "core/DocumentView.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/PdfCanvasLayout.hpp"
#include "ui/detail/ViewportMath.hpp"

#include <d2d1.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <new>
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

// PR-A1 placement/pan math. These are the ONLY places the canvas converts
// between bitmap pixels, canvas DIPs and PDF points -- see ViewportMath.hpp
// for the unit contract.
using litepdf::ui::bitmap_px_to_dip;
using litepdf::ui::clamp_pan;
using litepdf::ui::pdf_point_to_dip;
using litepdf::ui::place_bitmap;
using litepdf::ui::Placement;
using litepdf::ui::accept_completion;
using litepdf::ui::Slot;
using litepdf::ui::apply_wheel;
using litepdf::ui::consume_notches;
using litepdf::ui::Flip;
using litepdf::ui::wheel_step_dip;
using litepdf::ui::WheelResult;
}  // namespace

namespace litepdf::ui {

// Heap payload riding the completion message's LPARAM: the escrow ctx
// (clone of the worker ctx) plus the identity of the render that produced
// it. Allocated on the worker thread, freed on the UI thread in the
// WM_USER_RENDER_DONE[_RIGHT] handler. wParam still carries the fz_pixmap*
// directly.
//
// epoch  which VIEW (bumped by set_view)          -> is this the current tab?
// page   which PAGE was rendered                  -> has the user paged away?
// slot   which HALF of a spread                   -> see CompletionMath.hpp
// seq    which SUBMISSION BATCH (both halves of a spread share one) -> may
//        this completion consume the pending page anchor?
namespace {
struct RenderMeta {
    fz_context*   escrow;
    std::uint64_t epoch;
    int           page;
    litepdf::ui::Slot slot;
    std::uint64_t seq;
};

// Internal helper: shared escrow + Post logic for both the LEFT/single
// slot (msg = WM_USER_RENDER_DONE) and the RIGHT slot (msg =
// WM_USER_RENDER_DONE_RIGHT). Refcount discipline is identical for both;
// only the message ID and the recorded slot change.
bool post_render_done_impl(HWND target, UINT msg, litepdf::ui::Slot slot,
                           fz_pixmap* pix, fz_context* worker_ctx,
                           std::uint64_t epoch, int page, std::uint64_t seq) {
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
    auto* meta = new (std::nothrow) RenderMeta{escrow, epoch, page, slot, seq};
    if (!meta) {
        fz_drop_pixmap(escrow, pix);
        fz_drop_context(escrow);
        return false;
    }
    if (!PostMessageW(target, msg,
                      reinterpret_cast<WPARAM>(pix),
                      reinterpret_cast<LPARAM>(meta))) {
        fz_drop_pixmap(escrow, pix);
        fz_drop_context(escrow);
        delete meta;
        return false;
    }
    return true;
}
}  // namespace

bool PdfCanvas::post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx,
                                 std::uint64_t epoch,
                                 int page,
                                 std::uint64_t seq) {
    return post_render_done_impl(target, WM_USER_RENDER_DONE, Slot::Left,
                                 pix, worker_ctx, epoch, page, seq);
}

bool PdfCanvas::post_render_done_right(HWND target,
                                       fz_pixmap* pix,
                                       fz_context* worker_ctx,
                                       std::uint64_t epoch,
                                       int page,
                                       std::uint64_t seq) {
    return post_render_done_impl(target, WM_USER_RENDER_DONE_RIGHT, Slot::Right,
                                 pix, worker_ctx, epoch, page, seq);
}

struct PdfCanvas::Impl {
    ComPtr<ID2D1Factory>          factory;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap>           current_bitmap;  // Task 6 populates
    D2D1_SIZE_U                   last_size = { 0, 0 };
    litepdf::core::DocumentView*  view = nullptr;  // non-owning
    // Monotonic render epoch, bumped on every set_view. Renders carry the
    // epoch they were submitted under; a completion whose epoch != this is
    // from a superseded view and is dropped, not painted (issue #35).
    std::uint64_t                 view_epoch = 0;
    // Monotonic submission counter, bumped once per submission BATCH by
    // next_render_seq(). Both halves of a spread carry the same value. Unlike
    // view_epoch it does not survive being compared across views -- it exists
    // only to tell two submissions of the SAME page apart, which is what a
    // same-page zoom or resize produces and what (epoch, page, slot) cannot
    // distinguish.
    std::uint64_t                 next_seq = 0;
    // Where the next left/single completion should put the page. Empty for a
    // same-page re-render, which must KEEP the current pan. See
    // ui/detail/PageAnchor.hpp for the lifetime rule.
    AnchorSlot                    anchor;
    // Leftover wheel delta below one full notch. High-resolution wheels and
    // precision touchpads deliver |delta| < WHEEL_DELTA; without this the
    // canvas would either round every fragment up to a full notch or drop it.
    // See ui/detail/ScrollMath.hpp.
    int                           wheel_residual = 0;
    // Non-zero between a wheel-driven page flip and the completion that lands
    // new page. Without it, every further notch in that window flips again:
    // the pan and the bitmap still describe the OLD page, so apply_wheel keeps
    // reporting "already at the edge" and a brisk scroll walks several pages
    // without showing any of them. Cleared by the completion handler and by
    // set_view.
    //
    // It holds the SEQ of the flip's submission batch, not a bare flag, for two
    // reasons found in review. (a) A bare flag cleared by any completion could be
    // released by a stale one, and several stale P0s can be outstanding at once,
    // so "at most one extra flip" was not a bound anyone had proved. (b) If the
    // flip's completion is never posted at all -- post_render_done_impl returns
    // without posting on a clone failure, an allocation failure or a PostMessageW
    // failure -- a flag would latch forever and the wheel would be dead for the
    // rest of the session. Keyed by seq, the block lasts only while the flip's
    // batch is still the newest submitted, so any later render (a keystroke, a
    // zoom, a resize) releases it even when no completion ever arrives.
    std::uint64_t                 wheel_flip_seq = 0;   // 0 = nothing pending
    // The view_epoch that current_bitmap was created under. set_view does NOT
    // drop current_bitmap on a non-null swap -- only on the null one
    // (PdfCanvas.cpp:177-184) -- so after a tab switch the canvas is still
    // holding, and still painting, the OUTGOING document's page until the
    // incoming render lands. Anything that MEASURES that bitmap has to know it
    // belongs to a different document; see scroll_into_view (Task 6).
    std::uint64_t                 bitmap_epoch = 0;
    // ...and which PAGE it shows. navigate_to_page's single-page branch does not
    // drop current_bitmap either, so between a page turn and its completion the
    // canvas is holding the OUTGOING page of the SAME document -- an epoch check
    // alone would call that bitmap trustworthy. Both fields are set together.
    int                           bitmap_page  = -1;
    // Pan offset in canvas DIPs. An axis whose content fits the viewport is
    // centered and its pan is 0; an axis that overflows uses a TOP-LEFT
    // origin with the pan clamped to [viewport - content, 0]. Re-anchored and
    // re-clamped by apply_anchor when a completion lands -- never zeroed
    // unconditionally; see ui/detail/PageAnchor.hpp.
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
    // Fired after a successful Ctrl+wheel zoom so the owner can persist it.
    PdfCanvas::ZoomChangedCb      on_zoom_changed;
};

void PdfCanvas::set_view(litepdf::core::DocumentView* view) {
    // Per-render escrow (see PdfCanvas::post_render_done) is now the
    // lifetime mechanism for in-flight pixmaps — a canvas-level
    // orphan_ctx clone is no longer needed. The canvas just repoints
    // its view reference; the old view's pending pixmaps carry their
    // own escrow ctx and drop correctly regardless of this swap.
    impl_->view = view;
    // Bump the render epoch on every view swap so any render still in
    // flight for the previous view is recognised as stale at completion
    // and dropped instead of painted over the new view (issue #35).
    ++impl_->view_epoch;
    // The RIGHT slot must be dropped on EVERY swap, not only the null one.
    // set_dual_page returns early when the flag already matches, so switching
    // between two tabs that are both in spread mode never cleared it and the
    // outgoing document's right page stayed on screen until a fresh right
    // completion landed.
    impl_->right_bitmap.Reset();
    // New view, new epoch: an anchor installed for the outgoing document
    // describes a page that is no longer on screen.
    impl_->anchor.clear();
    impl_->wheel_flip_seq = 0;
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

std::uint64_t PdfCanvas::render_epoch() const noexcept {
    return impl_ ? impl_->view_epoch : 0;
}

std::uint64_t PdfCanvas::next_render_seq() {
    if (!impl_) return 0;
    ++impl_->next_seq;
    // Bind whatever intent is pending to THIS batch. A resubmit that installs
    // no anchor of its own re-stamps an older pending one, which is what
    // carries a navigation forward when a superseding render replaces the one
    // that was going to consume it -- and what makes a failed render recover,
    // since the retry re-issues the batch.
    impl_->anchor.stamp(impl_->next_seq);
    return impl_->next_seq;
}

void PdfCanvas::apply_viewport() {
    if (!impl_ || !impl_->view || !hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const float dpi_f = static_cast<float>(GetDpiForWindow(hwnd_));
    const float cw_px = static_cast<float>(rc.right - rc.left);
    const float ch_px = static_cast<float>(rc.bottom - rc.top);

    if (!impl_->dual_page) {
        impl_->view->set_viewport(cw_px, ch_px, dpi_f);
        return;
    }
    // Do NOT assume the caller already snapped current_page to the pair's LEFT
    // page. DocumentView derives the fit from current_page and treats pair_page
    // as the other half, so if current_page were the RIGHT page the left page's
    // size would never enter the fit and its slot could overflow. Re-snap here,
    // the same defensive move navigate_to_page's dual branch already makes.
    const int total = impl_->view->page_count();
    const int left  = dual_page_compute_left(impl_->view->current_page(), total);
    if (left != impl_->view->current_page()) {
        // Route through change_current_page so the observer fires for any
        // caller that reaches here without a canonical current_page. No
        // anchor -- this is a snap, and change_current_page leaves the
        // pending one untouched, so a Hit installed by a search landing on
        // the spread's RIGHT page survives.
        change_current_page(left);
    }
    const int right = dual_page_compute_right(left, total);
    const float gutter_px = 8.0f * dpi_f / 96.0f;   // matches the 8 DIP gutter
    const float slot_px   = std::max(0.0f, (cw_px - gutter_px) * 0.5f);
    impl_->view->set_viewport(slot_px, ch_px, dpi_f, right);
}

void PdfCanvas::set_on_page_changed(PageChangedCb cb) {
    if (!impl_) return;
    impl_->on_page_changed = std::move(cb);
}

void PdfCanvas::set_on_zoom_changed(ZoomChangedCb cb) {
    if (!impl_) return;
    impl_->on_zoom_changed = std::move(cb);
}

bool PdfCanvas::change_current_page(int idx) {
    if (!impl_ || !impl_->view) return false;
    const bool changed = impl_->view->set_current_page(idx);
    if (changed && impl_->on_page_changed) {
        impl_->on_page_changed(impl_->view->current_page());
    }
    return changed;
}

void PdfCanvas::set_pending_anchor(PageAnchor anchor) {
    if (!impl_) return;
    impl_->anchor.install(std::move(anchor));
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

    const int  target_pg  = static_cast<int>(h.page);
    const bool page_moved = change_current_page(target_pg);

    // A bitmap from a previous view, or from a previous PAGE, is not evidence
    // about this one. Neither set_view (on a non-null swap) nor
    // navigate_to_page's single-page branch drops current_bitmap, so the canvas
    // can be holding the outgoing document's page after a cross-tab jump, or the
    // outgoing PAGE of this document between a page turn and its completion.
    // Measuring the wanted quad against either can report "already visible" for a
    // hit that is off screen -- and with the conditional install below, that
    // verdict is final: no anchor is left for the completion to correct.
    const bool own_bitmap = impl_->current_bitmap
                            && impl_->bitmap_epoch == impl_->view_epoch
                            && impl_->bitmap_page  == impl_->view->current_page();

    if (page_moved || !own_bitmap || !impl_->rt) {
        // The hit is on a different page, we have nothing rendered, or what we
        // have belongs to another document. In every case the incoming pixmap is
        // the only thing that can place this hit -- a scroll computed from the
        // bitmap on screen would be exactly the stale estimate spec 3.4 is
        // about. Anchor it and let the completion do the work.
        set_pending_anchor(PageAnchor::hit(h));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);
    const float pct    = impl_->view->zoom_pct();

    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });

    // Already visible? Measure against the SAME origin the paint path uses.
    const float origin_y  = page_origin_y(src_h, vp.height) + impl_->pan_y;
    const float q_top_dip = origin_y + pdf_point_to_dip(q_min_y_pt, pct);
    const float q_bot_dip = origin_y + pdf_point_to_dip(q_max_y_pt, pct);
    const float margin    = 24.0f;
    if (q_top_dip >= margin && q_bot_dip <= vp.height - margin) {
        // Visible on the page already showing: DO NOT anchor. The header
        // contract is "If already visible, no scroll -- only the invalidate",
        // and MainWindow kicks a render after every find, so an anchor here
        // would re-centre the view on each F3 through hits that are all on
        // screen together.
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Same page, off screen: scroll now AND anchor. The anchor is computed from
    // the same bitmap the completion will replace with an identical one (same
    // page, same scale), so the two agree; it exists so a render that changes
    // the page height under us still lands the hit correctly.
    set_pending_anchor(PageAnchor::hit(h));
    impl_->pan_y = pan_y_for_hit(h);
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

    litepdf::ui::ColdStartTimer::mark_sub(
        litepdf::ui::ColdStartTimer::Sub::D2DFactory);
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
                    // Route through resubmit_current_page rather than submitting a
                    // single render here: in spread mode this handler refreshed only
                    // the left slot, which was invisible while zoom could not change
                    // displayed size -- and becomes a spread at two different
                    // magnifications the moment Task 4 lands.
                    resubmit_current_page();
                    // Persist the wheel zoom (menu zoom persists via MainWindow).
                    if (impl_->on_zoom_changed) impl_->on_zoom_changed();
                }
                return 0;
            }
            return on_wheel_scroll(GET_WHEEL_DELTA_WPARAM(w));
        }
        case WM_DPICHANGED_BEFOREPARENT:
            // DPI is changing. Discard render target; next paint rebuilds at new DPI.
            // Both slot bitmaps are sized for the OLD DPI and are bound to the
            // outgoing target — discard_render_target() drops both.
            discard_render_target();
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            // Parent just repositioned us. Invalidate so on_paint rebuilds the rt.
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_USER_RENDER_DONE:
        case WM_USER_RENDER_DONE_RIGHT: {
            const bool is_right = (msg == WM_USER_RENDER_DONE_RIGHT);
            auto* pix  = reinterpret_cast<fz_pixmap*>(w);
            auto* meta = reinterpret_cast<RenderMeta*>(l);

            if (!pix) {
                // Render failed or cancelled (helper posts WPARAM=0 here).
                // meta is also null on this path — nothing to drop.
                // This answers a pending wheel flip: no pixmap is coming for it.
                impl_->wheel_flip_seq = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (!meta) {
                // Defensive: pixmap without meta indicates a bypassed
                // post_render_done helper. Don't drop on a guessed ctx.
                return 0;
            }
            fz_context* escrow = meta->escrow;
            const std::uint64_t epoch = meta->epoch;
            const int           page  = meta->page;
            const Slot          slot  = meta->slot;
            const std::uint64_t seq   = meta->seq;
            delete meta;
            if (!escrow) {
                // Defensive: meta without escrow — nothing safe to drop.
                return 0;
            }

            // Is this pixmap still wanted? Four ways it may not be: the view
            // was swapped (issue #35 — a render for the previous tab landing
            // after the switch would otherwise paint over the now-active one),
            // a NEWER submission has been issued since this one went out (the
            // duplicate-P0 case: a zoom / resize / DPI change can leave two P0s
            // for the same page in flight, and the loser must not repaint), the
            // user paged away, or a RIGHT-slot pixmap arrived while the layout
            // is single-page. accept_completion answers all four; see
            // ui/detail/CompletionMath.hpp for why the slot is load-bearing.
            // Drop the pixmap + escrow and bail — do NOT adopt. This path also
            // leaves the pending page anchor alone: it belongs to the CURRENT
            // view, and a stale completion can never consume it anyway (the
            // seq would have to match).
            const int cur_page = impl_->view ? impl_->view->current_page() : 0;
            const int total    = impl_->view ? impl_->view->page_count()   : 0;
            // next_seq is the newest submission ISSUED, which is what the seq
            // test needs -- see ui/detail/CompletionMath.hpp. Comparing against
            // the newest ACCEPTED seq instead would let the older of two racing
            // P0s through whenever it happened to arrive first, and leave its
            // superseded pixmap on screen if the newer render then failed.
            if (!accept_completion(epoch, impl_->view_epoch,
                                   seq, impl_->next_seq,
                                   page, slot,
                                   cur_page, impl_->dual_page, total)) {
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
                impl_->bitmap_epoch   = epoch;
                impl_->bitmap_page    = page;
                ColdStartTimer::mark(3);  // first pixmap -> D2D bitmap
            }
            // Place the page. BOTH slots run this: the pan is clamped against
            // the UNION of the two, so a right-slot delivery changes the height
            // a Bottom anchor measures against, and either half can land first
            // (two workers, and an L1 cache hit returns before any MuPDF work).
            // take() therefore does not retire the anchor -- see
            // ui/detail/PageAnchor.hpp -- and mark_applied() is called ONLY when
            // the placement actually happened. A right half arriving while
            // navigate_to_page has both bitmaps reset cannot measure anything,
            // and must not retire an intent the left half has yet to use.
            const PageAnchor anchor = impl_->anchor.take(seq);
            if (apply_anchor(anchor) && anchor.kind != PageAnchor::Kind::None) {
                impl_->anchor.mark_applied();
            }
            // This batch delivered; release the wheel.
            impl_->wheel_flip_seq = 0;
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

    // Pin the render target to THIS WINDOW's dpi, not the desktop's. The helper
    // defaults dpiX/dpiY to 0.0, which means "desktop dpi" -- wrong on a
    // secondary monitor with a different scale factor, and the reason the
    // WM_DPICHANGED teardown below could not actually rebuild at the new dpi.
    // Bitmaps stay at the D2D default of 96 (so their GetSize() is in pixels);
    // ViewportMath::bitmap_px_to_dip is the single conversion point.
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    HRESULT hr = impl_->factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(),
            rt_dpi, rt_dpi),
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
    // BOTH slot bitmaps: an ID2D1Bitmap belongs to the target that made it, so
    // neither may outlive this call. right_bitmap was missing here, while the
    // WM_DPICHANGED_BEFOREPARENT comment claimed it was already handled.
    impl_->current_bitmap.Reset();
    impl_->right_bitmap.Reset();
    impl_->rt.Reset();
}

void PdfCanvas::resubmit_current_page() {
    if (!impl_->view) return;
    HWND target = hwnd_;
    const std::uint64_t epoch = impl_->view_epoch;
    const std::uint64_t seq   = next_render_seq();
    if (impl_->dual_page) {
        // (Phase 8 D10) Spread mode: D2DERR_RECREATE_TARGET recovery
        // also has to cover the right slot or the right page stays
        // blank until the user pages forward. Same submission shape as
        // on_key_down's dual branch, and one seq for both halves.
        const int cur   = impl_->view->current_page();
        const int total = impl_->view->page_count();
        const int left  = dual_page_compute_left(cur, total);
        const int right = dual_page_compute_right(left, total);
        impl_->view->cancel_stale_renders(0);
        apply_viewport();
        impl_->view->request_render(left,
            [target, epoch, left, seq](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target, p, worker_ctx, epoch, left, seq);
            });
        if (right >= 0) {
            impl_->view->request_render(right,
                [target, epoch, right, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target, p, worker_ctx,
                                                      epoch, right, seq);
                });
        }
        return;
    }
    const int page = impl_->view->current_page();
    impl_->view->request_render_with_prefetch(page,
        [target, epoch, page, seq](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target, p, worker_ctx, epoch, page, seq);
        });
}

// Painted content box in canvas DIPs -- what clamp_pan must measure against.
// Single page: the bitmap's DIP size at the origin. Spread: the UNION of the
// two slot placements, INCLUDING its left/top edge, because in an unequal
// spread that edge is not zero and the paint path has to subtract it.
//
// Measuring a spread against one bitmap would be wrong in the direction that
// hides the bug: each page fits inside its own half-width slot, so clamp_pan's
// "content fits -> pin to 0" branch would fire on every arrow key and
// horizontal panning would silently do nothing.
//
// Returns false when there is nothing rendered yet.
bool PdfCanvas::content_extent(ContentBox& out) const {
    if (!impl_ || !impl_->rt || !impl_->current_bitmap) return false;
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    auto dip_size = [&](ID2D1Bitmap* bm, float& w, float& h) {
        const D2D1_SIZE_F px = bm->GetSize();
        w = bitmap_px_to_dip(px.width,  rt_dpi);
        h = bitmap_px_to_dip(px.height, rt_dpi);
    };

    float lw = 0.0f, lh = 0.0f;
    dip_size(impl_->current_bitmap.Get(), lw, lh);
    if (!impl_->dual_page) {
        out = ContentBox{0.0f, 0.0f, lw, lh};
        return true;
    }

    // Same band geometry as on_paint's dual branch.
    const float gutter   = 8.0f;
    const float slot_w   = std::max(0.0f, (vp.width - gutter) * 0.5f);
    const float slot_h   = vp.height;
    const float left_x0  = 0.0f;
    const float right_x0 = slot_w + gutter;

    const Placement le = place_bitmap(lw, lh, slot_w, slot_h, 0.0f, 0.0f);
    float l = left_x0 + le.x;
    float r = left_x0 + le.x + le.w;
    float t = le.y;
    float b = le.y + le.h;

    if (impl_->right_bitmap) {
        float rw = 0.0f, rh = 0.0f;
        dip_size(impl_->right_bitmap.Get(), rw, rh);
        const Placement re = place_bitmap(rw, rh, slot_w, slot_h, 0.0f, 0.0f);
        l = std::min(l, right_x0 + re.x);
        r = std::max(r, right_x0 + re.x + re.w);
        t = std::min(t, re.y);
        b = std::max(b, re.y + re.h);
    }
    out = ContentBox{l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    return true;
}

LRESULT PdfCanvas::pan_by(float dx, float dy) {
    ContentBox box{};
    if (!content_extent(box)) return 0;
    const D2D1_SIZE_F vp = impl_->rt->GetSize();
    impl_->pan_x = clamp_pan(impl_->pan_x + dx, box.w, vp.width);
    impl_->pan_y = clamp_pan(impl_->pan_y + dy, box.h, vp.height);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return 0;
}

LRESULT PdfCanvas::on_wheel_scroll(int delta) {
    if (!impl_ || !impl_->view) return 0;

    // A flip is already on its way. Until its pixmap lands, pan_y and the
    // bitmaps still describe the OUTGOING page, so apply_wheel would keep
    // saying "at the edge" and every further notch would flip again -- a brisk
    // scroll would skip several pages without showing any of them. Drop the
    // notch AND the residual, so a fast spin does not fire the moment the new
    // page arrives.
    //
    // The block holds only while the flip's batch is STILL the newest submitted.
    // Anything else that submits a render -- a keystroke, a zoom, a resize --
    // moves next_seq past it and releases the wheel, which is what keeps a flip
    // whose completion was never posted at all from disabling the wheel for the
    // rest of the session.
    if (impl_->wheel_flip_seq != 0 && impl_->wheel_flip_seq == impl_->next_seq) {
        impl_->wheel_residual = 0;
        return 0;
    }

    const int notches = consume_notches(delta, impl_->wheel_residual);
    if (notches == 0) return 0;   // a fractional notch is never a page flip

    ContentBox box{};
    if (!content_extent(box)) return 0;   // nothing rendered yet
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    UINT lines = 3;   // the Windows default, and the value if the query fails
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float step = wheel_step_dip(notches, lines, vp.height);

    const WheelResult r = apply_wheel(impl_->pan_y, box.h, vp.height, step);
    if (r.flip == Flip::None) {
        impl_->pan_y = r.pan_y;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    // At the edge: turn the page. Forward lands at the new page's top;
    // backward lands at the previous page's BOTTOM, so scrolling back shows
    // the content the reader just scrolled past instead of skipping it.
    const int cur     = impl_->view->current_page();
    const int total   = impl_->view->page_count();
    const int max_idx = total - 1;
    int target;
    if (impl_->dual_page) {
        const int cur_left = dual_page_compute_left(cur, total);
        target = (r.flip == Flip::Next)
                     ? dual_page_step_next_left(cur_left, total)
                     : dual_page_step_prev_left(cur_left, total);
        // CANONICALISE before comparing. dual_page_step_next_left clamps an
        // overshoot to the LAST page, which in an odd-page document is a RIGHT
        // page whose pair is the spread we are already on: in a 3-page file
        // step_next_left(1, 3) == 2 while compute_left(2, 3) == 1. Comparing
        // the raw value would pass the guard below, re-render the same spread,
        // and throw the reader back to its top.
        target = dual_page_compute_left(target, total);
    } else {
        target = (r.flip == Flip::Next) ? std::min(cur + 1, max_idx)
                                        : std::max(cur - 1, 0);
    }
    // At the first or last page the step clamps to where we already are. Bail:
    // the document has no more pages and the pan is already at the edge.
    // Compare canonical to canonical -- every kick and navigate snaps
    // current_page to the pair's left, so `cur` should already be left-aligned
    // in spread mode, but relying on that couples this guard to an invariant
    // maintained four call sites away for no benefit.
    const int cur_canon = impl_->dual_page ? dual_page_compute_left(cur, total)
                                           : cur;
    if (target == cur_canon) return 0;

    navigate_to_page(target, (r.flip == Flip::Next) ? PageAnchor::top()
                                                    : PageAnchor::bottom());
    // navigate_to_page opened a submission batch, so next_seq now names it.
    impl_->wheel_flip_seq = impl_->next_seq;
    return 0;
}

float PdfCanvas::page_origin_y(float src_h, float vp_h) const {
    // place_bitmap with pan 0 gives exactly what on_paint uses as the page's
    // unpanned top: the centred position when the page fits, and 0 when it
    // overflows. Width does not affect the vertical result, so any positive
    // width will do here.
    return place_bitmap(src_h, src_h, src_h, vp_h, 0.0f, 0.0f).y;
}

float PdfCanvas::pan_y_for_hit(const litepdf::core::SearchSession::Hit& h) const {
    if (!impl_ || !impl_->current_bitmap || !impl_->rt || !impl_->view) {
        return impl_ ? impl_->pan_y : 0.0f;
    }
    ContentBox box{};
    if (!content_extent(box)) return impl_->pan_y;

    const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();
    const D2D1_SIZE_F vp     = impl_->rt->GetSize();
    const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);
    const float pct    = impl_->view->zoom_pct();

    // Quad centre in PDF points -> DIPs, measured from the page's own top.
    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_center = pdf_point_to_dip((q_min_y_pt + q_max_y_pt) * 0.5f, pct);

    // Centre the quad in the viewport. page_origin_y is the page's unpanned
    // top, so the pan needed to put the quad at vp/2 is the difference.
    //
    // The clamp measures against box.h, the PAINTED union, not against src_h.
    // on_paint clamps the same way; using the left bitmap's own height here
    // disagreed with the paint path in a spread whose right page is taller
    // (PR-A1 escalated item E2), landing the hit off-centre. In single-page
    // mode box.h IS src_h, so this is the same number the old code produced.
    return clamp_pan(vp.height * 0.5f - q_center - page_origin_y(src_h, vp.height),
                     box.h, vp.height);
}

bool PdfCanvas::apply_anchor(const PageAnchor& anchor) {
    ContentBox box{};
    if (!content_extent(box)) return false;
    const D2D1_SIZE_F vp = impl_->rt->GetSize();

    switch (anchor.kind) {
        case PageAnchor::Kind::Top:
            impl_->pan_y = 0.0f;
            break;
        case PageAnchor::Kind::Bottom:
            // The far end of the pan range. clamp_pan below turns this into 0
            // when the content fits, which is the right answer -- a page that
            // fits has no distinct bottom to land on.
            impl_->pan_y = vp.height - box.h;
            break;
        case PageAnchor::Kind::Hit:
            impl_->pan_y = pan_y_for_hit(anchor.target);
            break;
        case PageAnchor::Kind::None:
            // Same-page re-render: keep the pan exactly where the user left
            // it. Only the clamp below applies, because the content may have
            // changed size (zoom, DPI, pane toggle, window resize).
            break;
    }
    impl_->pan_x = clamp_pan(impl_->pan_x, box.w, vp.width);
    impl_->pan_y = clamp_pan(impl_->pan_y, box.h, vp.height);
    return true;
}

LRESULT PdfCanvas::scroll_to_top() {
    ContentBox box{};
    if (!content_extent(box)) return 0;
    const D2D1_SIZE_F vp = impl_->rt->GetSize();
    impl_->pan_y = clamp_pan(0.0f, box.h, vp.height);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return 0;
}

void PdfCanvas::navigate_to_page(int target, PageAnchor anchor) {
    if (!impl_ || !impl_->view) return;
    if (target == impl_->view->current_page()) return;

    // Install BEFORE the page change so an observer that re-enters cannot see a
    // moved page with a stale anchor, and before next_render_seq() below, which
    // is what binds it to this batch.
    set_pending_anchor(std::move(anchor));
    change_current_page(target);

    HWND target_hwnd = hwnd_;
    const std::uint64_t epoch = impl_->view_epoch;
    const std::uint64_t seq   = next_render_seq();
    if (impl_->dual_page) {
        // (Phase 8 D10) Spread mode: the pair changed, so both bitmaps are
        // stale. Clear them so on_paint shows the chrome background (and the
        // empty-right placeholder when the new pair has no right page) until
        // the new renders land.
        impl_->current_bitmap.Reset();
        impl_->right_bitmap.Reset();
        impl_->view->cancel_stale_renders(0);
        // Defensive re-snap: any entry point that leaves current_page on a
        // non-LEFT-aligned page must not silently mis-pair the spread. Snap
        // the LEFT here (and write it back so observers see the canonical
        // state) instead of trusting current_page raw.
        const int total = impl_->view->page_count();
        const int left  = dual_page_compute_left(impl_->view->current_page(),
                                                 total);
        if (left != impl_->view->current_page()) {
            // A snap, not a navigation: change_current_page never touches the
            // anchor, so a Hit installed moments earlier -- a search landing on
            // the RIGHT page of a spread -- survives it untouched.
            change_current_page(left);
        }
        apply_viewport();
        const int right = dual_page_compute_right(left, total);
        impl_->view->request_render(left,
            [target_hwnd, epoch, left, seq](fz_pixmap* p, fz_context* worker_ctx) {
                PdfCanvas::post_render_done(target_hwnd, p, worker_ctx, epoch,
                                            left, seq);
            });
        if (right >= 0) {
            impl_->view->request_render(right,
                [target_hwnd, epoch, right, seq](fz_pixmap* p, fz_context* worker_ctx) {
                    PdfCanvas::post_render_done_right(target_hwnd, p, worker_ctx,
                                                      epoch, right, seq);
                });
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // Cancel stale renders from rapid paging, submit P0 for the current page,
    // and prefetch prev/next at P1 so the next PgUp/PgDn is instant.
    const int page = impl_->view->current_page();
    impl_->view->request_render_with_prefetch(page,
        [target_hwnd, epoch, page, seq](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target_hwnd, p, worker_ctx, epoch,
                                        page, seq);
        });
}

LRESULT PdfCanvas::on_key_down(WPARAM key) {
    if (!impl_->view) return 0;

    const int cur     = impl_->view->current_page();
    const int max_idx = impl_->view->page_count() - 1;

    switch (key) {
        case VK_NEXT: {  // PgDn
            int next;
            if (impl_->dual_page) {
                // (Phase 8 T4) Snap from the current spread's LEFT page,
                // letting dual_page_step_next_left handle the cover->1
                // bootstrap explicitly — a plain `cur_left + 2` stride
                // overshoots from cover (0+2=2) and skips spread (1,2).
                const int total    = impl_->view->page_count();
                const int cur_left = dual_page_compute_left(cur, total);
                next = dual_page_step_next_left(cur_left, total);
            } else {
                next = std::min(cur + 1, max_idx);
            }
            navigate_to_page(next, PageAnchor::top());
            return 0;
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
            // PgUp lands at the TOP of the previous page, unlike the wheel's
            // backward flip which lands at its bottom: a key press is a
            // discrete jump, while wheel scrolling is continuous motion whose
            // content must not skip.
            navigate_to_page(prev, PageAnchor::top());
            return 0;
        }
        // Home / End name a position, not just a page. navigate_to_page returns
        // early when the target is the page already showing, so on the first or
        // last page these would otherwise do nothing at all to a reader who has
        // scrolled down -- and "go to the top" is exactly what they mean.
        case VK_HOME:
            if (cur == 0) return scroll_to_top();
            navigate_to_page(0, PageAnchor::top());
            return 0;
        case VK_END:
            if (cur == max_idx) return scroll_to_top();
            navigate_to_page(max_idx, PageAnchor::top());
            return 0;
        // Arrow keys pan by 100 DIP, clamped to the content.
        case VK_LEFT:  return pan_by( 100.0f,    0.0f);
        case VK_RIGHT: return pan_by(-100.0f,    0.0f);
        case VK_UP:    return pan_by(   0.0f,  100.0f);
        case VK_DOWN:  return pan_by(   0.0f, -100.0f);
        default:
            return 0;
    }
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

        const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));

        // Unpanned placement of each slot; the pan is clamped once against the
        // painted union of both (content_extent), never per slot -- with
        // unequal pages a per-slot clamp describes neither what is on screen
        // nor what the arrow keys move.
        auto slot_placement = [&](ID2D1Bitmap* bm) -> Placement {
            if (!bm) return Placement{};
            const D2D1_SIZE_F src_px = bm->GetSize();
            return place_bitmap(bitmap_px_to_dip(src_px.width,  rt_dpi),
                                bitmap_px_to_dip(src_px.height, rt_dpi),
                                slot_w, slot_h, 0.0f, 0.0f);
        };
        const Placement le = slot_placement(impl_->current_bitmap.Get());
        const Placement re = slot_placement(impl_->right_bitmap.Get());

        ContentBox box{};
        content_extent(box);
        const float pan_x = clamp_pan(impl_->pan_x, box.w, vp.width);
        const float pan_y = clamp_pan(impl_->pan_y, box.h, slot_h);

        // When the union overflows, pan is measured from ITS left/top edge, not
        // from the canvas origin -- otherwise both clamp endpoints are offset by
        // box.l and neither reaches a painted edge (pan 0 leaves a blank strip,
        // pan at the limit stops short of the viewport edge). When it fits, the
        // slots keep their natural centered positions and the base is zero.
        const float base_x = (box.w > vp.width) ? -box.l : 0.0f;
        const float base_y = (box.h > slot_h)   ? -box.t : 0.0f;

        auto draw_slot = [&](ID2D1Bitmap* bm, float x0, const Placement& pl) {
            if (!bm) return;
            const D2D1_RECT_F dst = D2D1::RectF(x0 + pl.x + base_x + pan_x,
                                                pl.y + base_y + pan_y,
                                                x0 + pl.x + pl.w + base_x + pan_x,
                                                pl.y + pl.h + base_y + pan_y);
            // Confine each page to its own band. `place_bitmap` returns the
            // bitmap's FULL width (p.w == src_w) whatever the slot measures, so
            // once the user zooms past the spread fit -- Zoom In and Ctrl+wheel
            // both allow it, and nothing clamps to the fit -- the left page's
            // dst runs past right_x0 and the right draw_slot call, which paints
            // second, lands on top of it. Before PR-A1 dual mode shrink-to-fit
            // made zoom inert here, so the overlap could not happen.
            //
            // This hides the overflow rather than making it reachable: the pan
            // is clamped once against the union of both slots, so the clipped
            // part cannot be panned into view. That belongs with PR-A2's
            // scrolling work; a clipped page still beats one page painting over
            // its neighbour. ALIASED matches the axis-aligned band edges and
            // avoids a blend pass on a full-height rect.
            //
            // Corner case: when the two pages differ in width and the left page
            // fits its slot but the right page overflows, the clip hides a strip
            // of the right page that was visible before. This is an accepted cost
            // of the fixed-band layout—hiding content is strictly better than one
            // page painting over another. Per-slot scrolling in PR-A2 resolves it.
            impl_->rt->PushAxisAlignedClip(
                D2D1::RectF(x0, 0.0f, x0 + slot_w, slot_h),
                D2D1_ANTIALIAS_MODE_ALIASED);
            impl_->rt->DrawBitmap(bm, dst, 1.0f,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            impl_->rt->PopAxisAlignedClip();
        };
        draw_slot(impl_->current_bitmap.Get(), left_x0,  le);
        draw_slot(impl_->right_bitmap.Get(),   right_x0, re);

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
        const D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();  // PIXELS
        const D2D1_SIZE_F vp     = impl_->rt->GetSize();              // DIPs
        const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        const float src_w  = bitmap_px_to_dip(src_px.width,  rt_dpi);
        const float src_h  = bitmap_px_to_dip(src_px.height, rt_dpi);

        // Natural size: no shrink-to-fit. The shipped code scaled the
        // destination to fit the viewport unconditionally, which is why a
        // changed render scale never changed the displayed size.
        const Placement pl = place_bitmap(src_w, src_h, vp.width, vp.height,
                                          impl_->pan_x, impl_->pan_y);
        const D2D1_RECT_F dst = D2D1::RectF(pl.x, pl.y, pl.x + pl.w, pl.y + pl.h);
        impl_->rt->DrawBitmap(impl_->current_bitmap.Get(), dst, 1.0f,
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        // --- Phase 6 Task 9: search hit overlay ---
        // PDF-point -> canvas-DIP mapping:
        //   canvas_DIP = pdf_point_to_dip(pdf_pt, zoom_pct)
        //              + (dst.left, dst.top)
        // where dst is the natural-size placement computed above.
        // MuPDF 1.24 page coords are top-left origin, Y-down — matching
        // D2D — so no Y-flip. Quads are drawn as axis-aligned bounding
        // boxes (v1); rotated-text quads would need a transformed
        // geometry in a follow-up.
        if (impl_->hits_fn && impl_->view
            && impl_->brush_hit_other_fill && impl_->brush_hit_current_fill) {
            // One PDF point is exactly zoom_pct DIPs: the pixmap is
            // page_pt * render_scale pixels wide, and dividing that by
            // rt_dpi/96 to reach DIPs cancels the dpi factor back out. Using
            // render_scale() here instead would double every hit rectangle at
            // 200% scaling.
            const float pct = impl_->view->zoom_pct();
            const float ox  = dst.left;
            const float oy  = dst.top;
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
                    ox + pdf_point_to_dip(min_x, pct),
                    oy + pdf_point_to_dip(min_y, pct),
                    ox + pdf_point_to_dip(max_x, pct),
                    oy + pdf_point_to_dip(max_y, pct));

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
