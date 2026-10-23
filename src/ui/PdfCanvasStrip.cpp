// LitePDF -- ui::PdfCanvas, the continuous-scroll strip (#55). The canvas's D2D
// and Win32 side of the strip: the layout switch, the strip's render requests
// and their completions, paint, and the scrollbar. The pure model is
// ui/detail/StripViewport.hpp (layout, scroll, current page) and
// ui/detail/StripSchedule.hpp / StripCompletion.hpp (what to render, what to
// keep); this file only applies them. Spec: docs/superpowers/specs/
// 2026-10-22-continuous-scroll-design.md.
#include "ui/PdfCanvas.hpp"

#include "core/DocumentView.hpp"
#include "core/EscrowContext.hpp"
#include "core/detail/PrefetchPlan.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/PdfCanvasImpl.hpp"
#include "ui/PdfCanvasLayout.hpp"
#include "ui/detail/ScrollMath.hpp"
#include "ui/detail/StripCompletion.hpp"
#include "ui/detail/StripScrollbar.hpp"
#include "ui/detail/ViewportMath.hpp"

#include <d2d1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

// The UI-thread MuPDF calls the completion needs, declared here so this
// translation unit stays free of <mupdf/fitz.h>, as PdfCanvas.cpp does.
extern "C" {
    int  fz_pixmap_width(fz_context*, fz_pixmap*);
    int  fz_pixmap_height(fz_context*, fz_pixmap*);
    int  fz_pixmap_stride(fz_context*, fz_pixmap*);
    unsigned char* fz_pixmap_samples(fz_context*, fz_pixmap*);
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

namespace litepdf::ui {

namespace {

// The WM_USER_STRIP_DONE payload (spec 5.3). Allocated on the worker thread for
// every completion, null results included, and freed by strip_on_done. The
// escrow is empty for a null result: there is no pixmap to drop.
struct StripMeta {
    litepdf::core::EscrowContext escrow;
    StripCompletionMeta          id;
};

// Posts WM_USER_STRIP_DONE for one strip render. Same refcount discipline as
// post_render_done_impl in PdfCanvas.cpp: the worker hands its shipping ref on
// `pix` over to this function, which on success passes it to the UI thread with
// an escrow cloned from `worker_ctx`, and on any failure drops it here (on
// worker_ctx when the clone failed, else on the escrow). The caller never drops
// it again. Unlike the paged message, a null result carries its identity too.
// Returns true iff the message was posted.
//
// When the escrow clone fails, the pixmap is dropped here on worker_ctx and the
// render is posted as a null result, so the UI thread still settles the page
// (spec 5.3 rule 4 or 5) instead of leaving it Pending.
//
// Spec 5.3 known limit: when the payload allocation or PostMessageW fails,
// nothing is posted, and the page stays Pending until it leaves V. A post that
// succeeds but is never dispatched leaks the payload exactly as #74 describes
// for the paged message: the canvas HWND dies only at process exit.
bool post_strip_done(HWND target, fz_pixmap* pix, fz_context* worker_ctx,
                     StripCompletionMeta id) {
    if (pix) {
        // The escrow keeps the Document's lock table and the root context
        // alive, so the UI thread can drop the pixmap even after the tab has
        // closed (#61).
        litepdf::core::EscrowContext escrow =
            litepdf::core::EscrowContext::clone_from(worker_ctx);
        if (escrow.valid()) {
            // Allocate BEFORE moving the escrow in, so the failure path still
            // owns it.
            auto* meta = new (std::nothrow) StripMeta{};
            if (!meta) {
                fz_drop_pixmap(escrow.get(), pix);
                return false;   // `escrow` drops its context, then the table
            }
            meta->escrow = std::move(escrow);
            meta->id     = id;
            if (!PostMessageW(target, WM_USER_STRIP_DONE,
                              reinterpret_cast<WPARAM>(pix),
                              reinterpret_cast<LPARAM>(meta))) {
                fz_drop_pixmap(meta->escrow.get(), pix);
                delete meta;
                return false;
            }
            return true;
        }
        fz_drop_pixmap(worker_ctx, pix);
        // Fall through: the pixmap is gone, but the identity is still posted.
    }
    auto* meta = new (std::nothrow) StripMeta{};
    if (!meta) return false;
    meta->id = id;
    if (!PostMessageW(target, WM_USER_STRIP_DONE, 0,
                      reinterpret_cast<LPARAM>(meta))) {
        delete meta;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// PageBitmapSet

std::uint64_t PageBitmapSet::Submitter::submit(int page, int priority, bool with_text,
                                               std::shared_ptr<std::atomic<bool>> flag) {
    // The strip's own counter (spec 3.1): the paged next_seq binds page anchors.
    const std::uint64_t seq    = ++set_.next_seq;
    const HWND          target = target_;
    const std::uint64_t epoch  = epoch_;
    if (!view_) return seq;
    // The flag is read when the render completes: a null result whose request
    // was cancelled is rule 4, not a failure (spec 5.3).
    litepdf::core::DocumentView::RenderCb cb =
        [target, epoch, page, seq, flag](fz_pixmap* p, fz_context* worker_ctx) {
            post_strip_done(target, p, worker_ctx,
                            StripCompletionMeta{epoch, page, seq, flag && flag->load()});
        };
    litepdf::core::DocumentView::TextCb text_cb;
    if (with_text) {
        text_cb = [target, epoch](litepdf::core::Document::TextPage t) {
            PdfCanvas::post_text_ready(target, std::move(t), epoch);
        };
    }
    view_->request_page_render(page, priority, std::move(cb), std::move(flag),
                               std::move(text_cb));
    return seq;
}

void PageBitmapSet::Submitter::cancel_thumbnails() {
    // spec 5.2 step 4: what every paged page turn's cancel_stale_renders(0) does
    // to the queued thumbnails.
    if (view_) view_->cancel_thumbnail_renders();
}

void PageBitmapSet::Submitter::released(int page) {
    set_.bitmaps.erase(page);
}

void PageBitmapSet::clear() {
    // released() needs neither a view nor a window.
    Submitter release_only(*this, nullptr, nullptr, 0);
    state.clear(release_only);
    bitmaps.clear();
    previous_visible = StripRange{};
    last_scale  = 0;
    last_invert = false;
}

// ---------------------------------------------------------------------------
// Layout

LayoutKind PdfCanvas::layout() const noexcept {
    if (!impl_) return LayoutKind::PagedSingle;
    if (impl_->strip) return LayoutKind::Strip;
    return impl_->dual_page ? LayoutKind::PagedSpread : LayoutKind::PagedSingle;
}

bool PdfCanvas::strip_active() const noexcept {
    return impl_ && impl_->view && impl_->strip && impl_->strip_view.has_layout();
}

void PdfCanvas::switch_layout(LayoutKind next, bool carry_position) {
    if (!impl_) return;
    const bool was_strip = impl_->strip;

    // spec 7.3, deviation 1: the carry is captured FIRST, before anything below
    // -- set_dual_page zeroes the paged pan, the reset forgets the strip's
    // scroll -- changes the canvas. A tab switch carries nothing.
    const bool carry = carry_position && impl_->view && impl_->view->page_count() > 0;
    CarriedPosition carried{};
    double pan_before  = 0.0;
    double zoom_before = 0.0;
    if (carry) {
        zoom_before = impl_->view->zoom_pct();
        carried = capture_layout_carry(pan_before);
    }

    // Before anything below: SetWindowPos sends WM_SIZE synchronously, and
    // SetScrollInfo can set WS_VSCROLL on a window that lacks it, so no strip
    // code may still believe it is in charge once the strip is being left.
    impl_->strip = (next == LayoutKind::Strip);
    if (was_strip && !impl_->strip) impl_->strip_view.reset();
    if (impl_->strip) {
        // Entering the strip, no thumb drag can be in progress: paged layouts
        // have no scroll bar. A drag that a switch out of the strip ended never
        // delivered its SB_ENDSCROLL to the strip handler (the canvas was paged
        // when it arrived), so its flags are cleared here, or the next drag would
        // be ignored once.
        impl_->thumb_drag_live = false;
        impl_->thumb_drag_dead = false;
    } else if (impl_->thumb_drag_live) {
        // Leaving it, a thumb drag in progress ends logically: its queued track
        // messages are ignored until SB_ENDSCROLL (spec 4.5).
        impl_->thumb_drag_dead = true;
    }

    // spec 3.4, the layout-switch reset: cancel the old layout's outstanding
    // work, forget the paged anchor and wheel latch, drop every bitmap, and bump
    // the epoch so a completion of the old layout fails its epoch check.
    if (was_strip) {
        impl_->strip_set.clear();
    } else if (impl_->view) {
        impl_->view->cancel_stale_renders(0);
    }
    impl_->anchor.clear();
    impl_->wheel_flip_seq = 0;
    impl_->current_bitmap.Reset();
    impl_->right_bitmap.Reset();
    impl_->strip_set.bitmaps.clear();   // already empty when the strip was left
    ++impl_->view_epoch;
    set_dual_page(next == LayoutKind::PagedSpread);

    // spec 6.2: the scrollbar exists only while the strip is shown, and the style
    // changes before anything reads GetClientRect, so the first fit already sees
    // the right client width.
    if (hwnd_) {
        const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        const LONG_PTR want  = impl_->strip ? (style | WS_VSCROLL)
                                            : (style & ~static_cast<LONG_PTR>(WS_VSCROLL));
        if (want != style) {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, want);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    // Without a carry the layout itself is built by the kick that follows.
    if (impl_->strip) impl_->strip_view.reset();
    // spec 7.3, deviation 1: applied after the new layout's fit.
    if (carry) apply_layout_carry(next, carried, pan_before, zoom_before);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

std::optional<StripTabState> PdfCanvas::strip_tab_state() const {
    if (!strip_active()) return std::nullopt;
    return impl_->strip_view.save();
}

void PdfCanvas::strip_build_layout() {
    const auto& sizes = impl_->view->page_sizes();
    std::vector<StripPageSize> strip_sizes;
    strip_sizes.reserve(sizes.size());
    for (const auto& s : sizes) {
        strip_sizes.push_back(StripPageSize{s.width_pt, s.height_pt});
    }
    impl_->strip_view.build(std::move(strip_sizes), impl_->view->zoom_pct());
}

namespace {

// A page's height and width in DIPs at `zoom`, from the view's page sizes; 0 for
// an index outside them.
double page_h_dip(const std::vector<litepdf::core::PageSize>& sizes, int page,
                  double zoom) {
    if (page < 0 || static_cast<std::size_t>(page) >= sizes.size()) return 0.0;
    return static_cast<double>(sizes[static_cast<std::size_t>(page)].height_pt) * zoom;
}

double page_w_dip(const std::vector<litepdf::core::PageSize>& sizes, int page,
                  double zoom) {
    if (page < 0 || static_cast<std::size_t>(page) >= sizes.size()) return 0.0;
    return static_cast<double>(sizes[static_cast<std::size_t>(page)].width_pt) * zoom;
}

// spread_vertical for the pair whose left page is `left`, at `zoom`, in a slot
// `slot_h` DIPs high (on_paint's slot is the full canvas height).
SpreadVertical spread_vertical_for(const std::vector<litepdf::core::PageSize>& sizes,
                                   int left, int page_count, double zoom,
                                   double slot_h) {
    const int right = dual_page_compute_right(left, page_count);
    const double h_right = right >= 0 ? page_h_dip(sizes, right, zoom) : -1.0;
    return spread_vertical(page_h_dip(sizes, left, zoom), h_right, slot_h);
}

}  // namespace

CarriedPosition PdfCanvas::capture_layout_carry(double& pan_before) {
    auto* v = impl_->view;
    pan_before = 0.0;
    if (impl_->strip) {
        // From the strip: the derived (or pinned) current page.
        if (!impl_->strip_view.has_layout()) return CarriedPosition{v->current_page(), 0.0};
        pan_before = impl_->strip_view.pan_x_eff();
        return impl_->strip_view.carry();
    }
    // From a paged layout: the geometry on_paint would use, from the page sizes
    // at the zoom on screen, never from the bitmaps -- a toggle that arrives
    // before the previous layout's render landed still carries.
    const auto& sizes  = v->page_sizes();
    const double zoom  = v->zoom_pct();
    const double vp_h  = strip_viewport_dip().h;
    const int    total = v->page_count();
    const double pan_y = impl_->pan_y;
    if (impl_->dual_page) {
        // From a spread: measured on the left page alone, in the painted frame.
        const int left = dual_page_compute_left(v->current_page(), total);
        const SpreadVertical sv = spread_vertical_for(sizes, left, total, zoom, vp_h);
        return carry_from_spread(left, pan_y, sv.left_top_in_union,
                                 page_h_dip(sizes, left, zoom));
    }
    // From paged single: a page that fits is centred, one that overflows starts
    // at the viewport top.
    const int    cur = v->current_page();
    const double h   = page_h_dip(sizes, cur, zoom);
    pan_before = impl_->pan_x;
    return carry_from_paged_single(cur, pan_y, h <= vp_h ? (vp_h - h) / 2.0 : 0.0, h);
}

void PdfCanvas::apply_layout_carry(LayoutKind next, CarriedPosition c, double pan_before,
                                   double zoom_before) {
    auto* v = impl_->view;
    const int total = v->page_count();
    c.page = std::clamp(c.page, 0, total - 1);
    // The carried page becomes current before the refit, which derives the fit
    // from the current page: into a spread, the left page of its pair.
    if (next == LayoutKind::Strip) {
        set_strip_current_page(c.page);
    } else if (next == LayoutKind::PagedSpread) {
        change_current_page(dual_page_compute_left(c.page, total));
    } else {
        change_current_page(c.page);
    }
    // The new layout's fit (the strip's full width, a spread's half-width slot).
    // The kick that follows refits again, to the same value.
    apply_viewport();
    const double zoom = v->zoom_pct();
    const double k = zoom_before > 0.0 ? zoom / zoom_before : 1.0;
    c.offset_dip *= k;
    const double pan = pan_before * k;

    const DipSize vp = strip_viewport_dip();
    const auto& sizes = v->page_sizes();
    switch (next) {
        case LayoutKind::Strip: {
            auto& sv = impl_->strip_view;
            sv.set_viewport(vp.w, vp.h);
            strip_build_layout();
            sv.apply_carry(c);            // scroll to the position, pin the page
            sv.set_desired_pan_x(pan);
            break;
        }
        case LayoutKind::PagedSpread: {
            // pan_x = 0: the union's horizontal layout differs from the strip's.
            const int left = dual_page_compute_left(c.page, total);
            const SpreadVertical sv = spread_vertical_for(sizes, left, total, zoom, vp.h);
            impl_->pan_x = 0.0f;
            impl_->pan_y = spread_pan_y_for(c, sv.left_top_in_union, sv.union_h, vp.h);
            break;
        }
        case LayoutKind::PagedSingle:
            // The switch cleared the anchor, so the paged completion only
            // re-clamps these against the real bitmap (spec 7.3).
            impl_->pan_x = clamp_pan(static_cast<float>(pan),
                                     static_cast<float>(page_w_dip(sizes, c.page, zoom)),
                                     static_cast<float>(vp.w));
            impl_->pan_y = paged_pan_y_for(c, page_h_dip(sizes, c.page, zoom), vp.h);
            break;
    }
}

PdfCanvas::DipSize PdfCanvas::strip_viewport_dip() const {
    DipSize out{0.0, 0.0};
    if (!hwnd_) return out;
    RECT rc;
    if (!GetClientRect(hwnd_, &rc)) return out;
    const UINT dpi = GetDpiForWindow(hwnd_);
    const double ratio = dpi > 0 ? 96.0 / static_cast<double>(dpi) : 1.0;
    out.w = static_cast<double>(rc.right - rc.left) * ratio;
    out.h = static_cast<double>(rc.bottom - rc.top) * ratio;
    return out;
}

void PdfCanvas::strip_kick(const StripKickActions& a, const StripTabState* restore) {
    if (!impl_ || !impl_->view || layout() != LayoutKind::Strip) return;
    auto& sv = impl_->strip_view;
    // The viewport first: WM_SIZE normally set it already, but a kick must not
    // depend on one having arrived since the layout switch.
    const DipSize vp = strip_viewport_dip();
    sv.set_viewport(vp.w, vp.h);
    if (!sv.has_layout()) {
        strip_build_layout();
    } else if (a.relayout) {
        sv.relayout(impl_->view->zoom_pct());   // spec 4.5
    }
    if (restore) sv.restore(*restore);          // spec 7.4
    if (a.restyle) {
        // spec 5.2: cancel every Pending request, keep the bitmaps as stale.
        impl_->strip_set.state.change_render_params();
    }
    strip_after_change();
    if (a.relayout) strip_end_thumb_drag();   // spec 4.5: after the SetScrollInfo above
}

// ---------------------------------------------------------------------------
// Navigation (spec 6.3)

void PdfCanvas::strip_navigate(int page) {
    if (!strip_active()) return;
    // Scrolls to the anchor and pins the page; strip_after_change then makes it
    // the current page (the pin keeps it through the recompute).
    impl_->strip_view.navigate(page);
    strip_after_change();
}

int PdfCanvas::strip_top_visible_page() const {
    if (!impl_ || !impl_->view) return 0;
    if (!strip_active()) return impl_->view->current_page();
    return impl_->strip_view.top_visible_page();
}

// ---------------------------------------------------------------------------
// Current page, scrollbar, scheduling

bool PdfCanvas::set_strip_current_page(int page) {
    if (!impl_ || !impl_->view) return false;
    // spec 4.6: the observer fires only when the value changes, and the strip
    // does not cancel the selection gesture (spec 3.4).
    if (!impl_->view->set_current_page(page)) return false;
    if (impl_->on_page_changed) impl_->on_page_changed(impl_->view->current_page());
    return true;
}

void PdfCanvas::strip_after_change() {
    if (!strip_active()) return;
    // The current page is set before the pass, in the same message (spec 5.2,
    // PR 1's caller contract): the pass's text-carrying P0 targets it.
    const bool changed = set_strip_current_page(impl_->strip_view.recompute_current());
    strip_update_scrollbar();
    strip_reschedule();
    if (changed) {
        // spec 5.2 text prefetch: the current page changed to c. After the
        // pass, so a P0 that already carries c's text makes this a no-op.
        auto* v = impl_->view;
        const int c = v->current_page();
        PageBitmapSet::Submitter sub(impl_->strip_set, v, hwnd_, impl_->view_epoch);
        impl_->strip_set.state.request_text(c, v->has_text_for(c),
                                            v->pending_text_page() == c,
                                            v->l1_contains(c), sub);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PdfCanvas::strip_update_scrollbar() {
    if (!strip_active() || !hwnd_) return;
    // spec 6.2: rounded DIPs; SIF_DISABLENOSCROLL keeps the bar while the strip
    // fits, so the client width -- and with it FitWidth -- never oscillates.
    const StripScrollInfo info = impl_->strip_view.scroll_info();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = info.max;
    si.nPage  = static_cast<UINT>(info.page);
    si.nPos   = info.pos;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void PdfCanvas::strip_reschedule() {
    if (!strip_active()) return;
    auto* v = impl_->view;
    auto& set = impl_->strip_set;
    const auto& sv = impl_->strip_view;

    // spec 5.2: a render scale or polarity change cancels every Pending request
    // and leaves the Ready bitmaps stale, before the pass that re-requests them.
    const float scale  = v->render_scale();
    const bool  invert = v->invert_colors();
    if (scale != set.last_scale || invert != set.last_invert) {
        set.state.change_render_params();
        set.last_scale  = scale;
        set.last_invert = invert;
    }

    const std::vector<litepdf::core::PageSize>& sizes = v->page_sizes();
    ScheduleInput in;
    in.visible         = sv.visible();
    in.visible_by_area = sv.visible_by_area();
    in.prefetch        = sv.prefetch();
    in.retain          = sv.retain();
    in.scale           = scale;
    in.invert          = invert;
    // spec 5.2 / #49: L1's real limits, read from the view.
    in.l1_capacity     = v->l1_capacity();
    in.l1_budget       = v->l1_budget_bytes();
    in.bytes_of = [&sizes, scale](int page) -> std::size_t {
        if (page < 0 || static_cast<std::size_t>(page) >= sizes.size()) return 0;
        const auto& s = sizes[static_cast<std::size_t>(page)];
        return litepdf::core::estimated_pixmap_bytes(s.width_pt, s.height_pt, scale);
    };
    in.previous_first_visible = set.previous_visible.first;
    in.previous_last_visible  = set.previous_visible.last;

    PageBitmapSet::Submitter sub(set, v, hwnd_, impl_->view_epoch);
    set.state.run_pass(in, sub);
    set.previous_visible = in.visible;

    // The map keeps a bitmap exactly while its slot holds one.
    for (auto it = set.bitmaps.begin(); it != set.bitmaps.end();) {
        const PageSlot* slot = set.state.slot(it->first);
        if (!slot || !slot_holds_bitmap(*slot)) {
            it = set.bitmaps.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Input (spec 6.1, 6.2)
//
// Every input ends in strip_after_change, moved or not: a scroll input that hits
// the clamp still ends a navigation pin, and the current page is recomputed after
// every scroll input (spec 4.6). The observer fires only when the page changes,
// so an input that moves nothing costs one scheduling pass.

LRESULT PdfCanvas::strip_on_wheel(int delta) {
    if (!strip_active()) return 0;
    auto& sv = impl_->strip_view;
    // No identity guard and no wheel_flip_seq latch: both hold the paged wheel
    // while a page flip's render is pending, and the strip never flips (spec 6.1,
    // "the wheel's two guards").
    const int notches = consume_notches(delta, impl_->wheel_residual);
    UINT lines = 3;   // the Windows default, and the value if the query fails
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float step = wheel_step_dip(notches, lines, static_cast<float>(sv.viewport_h()));
    // A positive step is toward the top of the document.
    sv.scroll_by(-static_cast<double>(step));
    strip_after_change();
    return 0;
}

LRESULT PdfCanvas::strip_on_hwheel(int raw_delta, HWheelSource src) {
    if (!strip_active()) return 0;
    auto& sv = impl_->strip_view;
    const int notches =
        consume_notches(rightward_delta(raw_delta, src), impl_->hwheel_residual);
    // The settings paged mode reads: a tilt wheel's "characters", and the lines
    // setting for Shift + the vertical wheel.
    UINT units = 3;   // the Windows default, and the value if the query fails
    SystemParametersInfoW(src == HWheelSource::Tilt ? SPI_GETWHEELSCROLLCHARS
                                                    : SPI_GETWHEELSCROLLLINES,
                          0, &units, 0);
    const float step = wheel_step_dip(notches, units, static_cast<float>(sv.viewport_w()));
    // A rightward step reveals the right side, so the desired pan decreases: the
    // direction VK_RIGHT pans.
    sv.pan_x_by(-static_cast<double>(step));
    strip_after_change();
    return 0;
}

LRESULT PdfCanvas::strip_on_key(WPARAM key) {
    if (!strip_active()) return 0;
    StripKey k;
    switch (key) {
        case VK_NEXT:  k = StripKey::PageDown; break;
        case VK_PRIOR: k = StripKey::PageUp;   break;
        case VK_HOME:  k = StripKey::Home;     break;
        case VK_END:   k = StripKey::End;      break;
        case VK_UP:    k = StripKey::LineUp;   break;
        case VK_DOWN:  k = StripKey::LineDown; break;
        case VK_LEFT:  k = StripKey::Left;     break;
        case VK_RIGHT: k = StripKey::Right;    break;
        default:       return 0;
    }
    impl_->strip_view.key(k);
    strip_after_change();
    return 0;
}

LRESULT PdfCanvas::strip_pan_by(float dx, float dy) {
    if (!strip_active()) return 0;
    // Grab-and-drag: the content follows the pointer, so a downward drag shows
    // what is above, and a rightward one moves the pages right, as in paged mode.
    impl_->strip_view.scroll_by(-static_cast<double>(dy));
    impl_->strip_view.pan_x_by(static_cast<double>(dx));
    strip_after_change();
    return 0;
}

LRESULT PdfCanvas::strip_on_vscroll(WPARAM w) {
    if (LOWORD(w) == SB_ENDSCROLL) {
        // The end of any scroll bar interaction, a thumb drag's included. Before
        // the strip_active check, so the latch never outlives the interaction
        // that set it, laid out or not.
        impl_->thumb_drag_live = false;
        impl_->thumb_drag_dead = false;
        // A drag a re-layout ended leaves the thumb where the pointer let go:
        // put it back at the position.
        strip_update_scrollbar();
        return 0;
    }
    if (!strip_active() || !hwnd_) return 0;
    auto& sv = impl_->strip_view;
    switch (LOWORD(w)) {
        // The arrows act as the arrow keys, a click in the track as PgUp / PgDn.
        case SB_LINEUP:   sv.key(StripKey::LineUp);   break;
        case SB_LINEDOWN: sv.key(StripKey::LineDown); break;
        case SB_PAGEUP:   sv.key(StripKey::PageUp);   break;
        case SB_PAGEDOWN: sv.key(StripKey::PageDown); break;
        case SB_TOP:      sv.key(StripKey::Home);     break;
        case SB_BOTTOM:   sv.key(StripKey::End);      break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            impl_->thumb_drag_live = true;
            // A re-layout ended this drag (spec 4.5): its track positions belong
            // to the old layout, so they are ignored until SB_ENDSCROLL.
            if (impl_->thumb_drag_dead) return 0;
            // The 32-bit track position. HIWORD(w) is 16-bit and wraps above
            // 65535 DIPs, a few dozen pages in (spec 6.2).
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask  = SIF_TRACKPOS;
            if (!GetScrollInfo(hwnd_, SB_VERT, &si)) return 0;
            sv.scroll_to_pos(si.nTrackPos);
            break;
        }
        default:
            return 0;
    }
    strip_after_change();
    return 0;
}

void PdfCanvas::strip_end_thumb_drag() noexcept {
    if (impl_ && impl_->thumb_drag_live) impl_->thumb_drag_dead = true;
}

// ---------------------------------------------------------------------------
// Completion (spec 5.3)

void PdfCanvas::strip_on_done(WPARAM w, LPARAM l) {
    auto* pix = reinterpret_cast<fz_pixmap*>(w);
    // Adopt the meta and the escrow first. The escrow must outlive every
    // fz_drop_pixmap below; it is destroyed when this function returns, which
    // drops its context and only then the lock table and the root context the
    // family was cloned from (#61), exactly as in the paged handler.
    std::unique_ptr<StripMeta> meta(reinterpret_cast<StripMeta*>(l));
    litepdf::core::EscrowContext escrow;
    StripCompletionMeta id{};
    const bool has_meta = static_cast<bool>(meta);
    if (meta) {
        escrow = std::move(meta->escrow);
        id     = meta->id;
        meta.reset();
    }

    // #57: before any early return, as the paged handler does.
    if (impl_->on_completion_arrived) impl_->on_completion_arrived();

    if (!has_meta) {
        // Defensive: a payload-less post bypassed post_strip_done. Without the
        // escrow there is no safe context to drop a pixmap on.
        return;
    }
    if (pix && !escrow.valid()) return;   // defensive, as the paged handler
    fz_context* const ectx = escrow.get();

    auto& set = impl_->strip_set;
    // A completion while the strip is not shown is dropped by its epoch: every
    // layout switch bumps view_epoch (spec 3.4).
    const PageSlot* slot = set.state.slot(id.page);
    const StripCompletionAction action =
        classify_strip_completion(slot, id, impl_->view_epoch, pix != nullptr);

    // The page's bitmap goes with its slot (spec 5.1); called after every slot
    // change below.
    auto prune_page = [&set](int page) {
        const PageSlot* s = set.state.slot(page);
        if (!s || !slot_holds_bitmap(*s)) set.bitmaps.erase(page);
    };

    switch (action) {
        case StripCompletionAction::Drop:
            // Rules 1, 2. Every text-only P1 lands here (rule 2), and every
            // completion after a layout or tab switch (rule 1).
            if (pix) fz_drop_pixmap(ectx, pix);
            return;

        case StripCompletionAction::CreateBitmap: {
            if (!impl_->view || layout() != LayoutKind::Strip) {
                // Unreachable while the epoch matches; never leave a pixmap.
                fz_drop_pixmap(ectx, pix);
                return;
            }
            const PageSlot pending = *slot;   // rule 2 guarantees a Pending slot
            const int w_px   = fz_pixmap_width(ectx, pix);
            const int h_px   = fz_pixmap_height(ectx, pix);
            const int stride = fz_pixmap_stride(ectx, pix);
            unsigned char* samples = fz_pixmap_samples(ectx, pix);

            create_render_target();
            if (!impl_->rt) {
                // No render target at all. The page goes to None and waits for
                // the next strip change (an input, a kick, a resize) to schedule
                // it again, as the paged handler stops here. Rescheduling now
                // would resubmit every visible page on every completion, a loop
                // at message speed for as long as no target can be created.
                fz_drop_pixmap(ectx, pix);
                set.state.slots()[id.page] =
                    after_bitmap(pending, BitmapResult::RecreateOrNoTarget);
                prune_page(id.page);
                if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }

            BitmapResult result = BitmapResult::Ok;
            ComPtr<ID2D1Bitmap> bmp;
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE));
            HRESULT hr = impl_->rt->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(w_px),
                            static_cast<UINT32>(h_px)),
                samples, static_cast<UINT32>(stride), &props, &bmp);
            fz_drop_pixmap(ectx, pix);
            if (hr == D2DERR_RECREATE_TARGET) {
                result = BitmapResult::RecreateOrNoTarget;
            } else if (FAILED(hr)) {
                result = BitmapResult::OtherFailure;
            }

            // The slot first. Rescheduling while the matched slot is still
            // Pending at P0 would submit nothing for it (a Pending page is
            // resubmitted only at P1) and leave the page blank.
            set.state.slots()[id.page] = after_bitmap(pending, result);
            if (result == BitmapResult::Ok) {
                set.bitmaps[id.page] = std::move(bmp);
                ColdStartTimer::mark(3);   // first pixmap -> D2D bitmap (idempotent)
                auto* v = impl_->view;
                if (id.page == v->current_page()) {
                    // spec 5.2: the current page's P0 completed.
                    PageBitmapSet::Submitter sub(set, v, hwnd_, impl_->view_epoch);
                    set.state.request_text(id.page, v->has_text_for(id.page),
                                           v->pending_text_page() == id.page,
                                           v->l1_contains(id.page), sub);
                }
            } else {
                // OtherFailure turns a Pending page holding a stale bitmap into
                // Failed; D2DERR_RECREATE_TARGET into None.
                prune_page(id.page);
                if (result == BitmapResult::RecreateOrNoTarget) {
                    // The device was lost: the target is rebuilt on the next
                    // create_render_target, so the lost pages are rescheduled at
                    // once, as the paged handler resubmits its page.
                    discard_render_target();
                    strip_reschedule();
                }
            }
            break;
        }

        case StripCompletionAction::CancelAndReschedule:
            // Rule 4: a flag set after the last sweep.
            set.state.slots()[id.page] = after_null(*slot, true);
            prune_page(id.page);
            strip_reschedule();
            break;

        case StripCompletionAction::MarkFailed:
            // Rule 5: no automatic retry (spec 5.1).
            set.state.slots()[id.page] = after_null(*slot, false);
            prune_page(id.page);
            break;
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Paint (spec 4.2, 5.1)

void PdfCanvas::strip_paint() {
    create_render_target();
    if (!impl_->rt) {
        ValidateRect(hwnd_, nullptr);   // nothing to do; the next paint retries
        return;
    }
    // #135: record the overlay this paint draws, as the paged on_paint does, so
    // on_search_hits_changed compares the next search update against the screen.
    // The strip draws no hits in PR 2 (PR 3 will): without this, hits a paged
    // paint left here would make every search update repaint the whole strip.
    impl_->painted_hits.clear();
    bool drew_bitmap = false;

    impl_->rt->BeginDraw();
    // The chrome colour of the paged layouts (Phase 8 D7).
    const UINT32 bg_rgb = impl_->invert_chrome ? 0x202020u : 0xF0F0F0u;
    impl_->rt->Clear(D2D1::ColorF(bg_rgb));

    if (strip_active()) {
        auto& set = impl_->strip_set;
        const auto& sv = impl_->strip_view;
        const float rt_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        const float scale  = impl_->view->render_scale();
        const bool  invert = impl_->view->invert_colors();

        // A page with nothing to show is a blank page of the page colour; a
        // failed one also gets the spread placeholder's 1-DIP border.
        ComPtr<ID2D1SolidColorBrush> page_brush;
        ComPtr<ID2D1SolidColorBrush> border_brush;
        impl_->rt->CreateSolidColorBrush(
            D2D1::ColorF(impl_->invert_chrome ? 0x000000u : 0xFFFFFFu), &page_brush);
        impl_->rt->CreateSolidColorBrush(
            D2D1::ColorF(impl_->invert_chrome ? 0x404040u : 0xC0C0C0u), &border_brush);

        const StripRange visible = sv.visible();
        for (int page = visible.first; page <= visible.last; ++page) {
            // Viewport-relative, so narrowing to float is exact enough (spec 4.1).
            const StripPageRect r = sv.page_rect(page);
            const float x = static_cast<float>(r.x);
            const float y = static_cast<float>(r.y);
            const D2D1_RECT_F dst = D2D1::RectF(x, y, static_cast<float>(r.x + r.w),
                                                static_cast<float>(r.y + r.h));
            const PageSlot* slot = set.state.slot(page);
            const auto it = set.bitmaps.find(page);
            ID2D1Bitmap* bm = it != set.bitmaps.end() ? it->second.Get() : nullptr;

            if (slot && bm && slot->kind == PageStateKind::Ready &&
                !is_stale(*slot, scale, invert)) {
                // Natural size at the rect's top-left, so MuPDF's 1-px rounding
                // is not resampled (spec 4.2 "Pixel rounding").
                const D2D1_SIZE_F px = bm->GetSize();
                const D2D1_RECT_F natural = D2D1::RectF(
                    x, y, x + bitmap_px_to_dip(px.width, rt_dpi),
                    y + bitmap_px_to_dip(px.height, rt_dpi));
                impl_->rt->DrawBitmap(bm, natural, 1.0f,
                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                drew_bitmap = true;
            } else if (slot && bm && slot_holds_bitmap(*slot)) {
                // Ready-stale, or Pending over its previous bitmap: stretched to
                // the page's current rect, so a zoom never flashes white (5.1).
                impl_->rt->DrawBitmap(bm, dst, 1.0f,
                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                drew_bitmap = true;
            } else {
                if (page_brush) impl_->rt->FillRectangle(dst, page_brush.Get());
                if (slot && slot->kind == PageStateKind::Failed && border_brush) {
                    // Inset by half the stroke so the 1-DIP border lies inside.
                    impl_->rt->DrawRectangle(
                        D2D1::RectF(dst.left + 0.5f, dst.top + 0.5f,
                                    dst.right - 0.5f, dst.bottom - 0.5f),
                        border_brush.Get(), 1.0f);
                }
            }
        }
    }

    const HRESULT hr = impl_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_render_target();
        resubmit_current_page();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (SUCCEEDED(hr) && drew_bitmap) {
        // The first paint that drew a page bitmap: T4 of the cold-start line, in
        // either layout (spec 3.4). Both calls are idempotent.
        ColdStartTimer::mark(4);
        ColdStartTimer::emit_if_complete(log_timings_);
    }
    ValidateRect(hwnd_, nullptr);
}

}  // namespace litepdf::ui
