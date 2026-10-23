#pragma once
// LitePDF -- ui::PdfCanvas's private state, shared by the canvas's translation
// units (PdfCanvas.cpp and, for the continuous strip (#55), PdfCanvasStrip.cpp).
// Not part of the canvas's interface: include it from those files only.

#include "ui/PdfCanvas.hpp"

#include "core/Document.hpp"
#include "core/SearchSession.hpp"
#include "core/TextSelection.hpp"
#include "ui/detail/PageAnchor.hpp"
#include "ui/detail/SelectionDrag.hpp"
#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripSchedule.hpp"
#include "ui/detail/StripViewport.hpp"

#include <d2d1.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace litepdf::ui {

using Microsoft::WRL::ComPtr;

// #55: the continuous strip's rendered pages (spec 3.2, 5.1). `state` owns each
// page's render state; `bitmaps` holds the D2D bitmap of every page whose slot
// passes slot_holds_bitmap -- Ready, or Pending over its previous bitmap -- and
// nothing else, so the GPU bitmap count stays bounded by StripViewport::retain.
struct PageBitmapSet {
    std::map<int, ComPtr<ID2D1Bitmap>> bitmaps;
    StripRenderState                   state;
    // The strip's own submission counter, one value per request. Never the
    // paged next_seq, which binds paged page anchors (spec 3.1).
    std::uint64_t                      next_seq = 0;
    // V as of the last pass, so the next pass can tell whether V changed and the
    // queued thumbnails are to be cancelled (spec 5.2 step 4).
    StripRange                         previous_visible;
    // The render scale and polarity of the last pass. A change cancels every
    // Pending request and marks the Ready bitmaps stale (spec 5.2).
    float                              last_scale = 0;
    bool                               last_invert = false;

    // StripRenderState's Submitter for the canvas: real render requests against
    // `view`, completions posted to `target` as WM_USER_STRIP_DONE under `epoch`.
    // Defined in PdfCanvasStrip.cpp.
    class Submitter final : public StripRenderState::Submitter {
    public:
        Submitter(PageBitmapSet& set, litepdf::core::DocumentView* view, HWND target,
                  std::uint64_t epoch) noexcept
            : set_(set), view_(view), target_(target), epoch_(epoch) {}
        std::uint64_t submit(int page, int priority, bool with_text,
                             std::shared_ptr<std::atomic<bool>> flag) override;
        void cancel_thumbnails() override;
        void released(int page) override;

    private:
        PageBitmapSet&               set_;
        litepdf::core::DocumentView* view_;
        HWND                         target_;
        std::uint64_t                epoch_;
    };

    // The view goes away or the layout leaves the strip: every Pending request
    // is cancelled (flag store, spec 3.3), every slot and bitmap released, and
    // the per-pass memory forgotten. next_seq keeps counting.
    void clear();
};

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
    // The horizontal wheel's own leftover delta (#56), in "rightward" units
    // (see rightward_delta). Separate from wheel_residual so a diagonal
    // touchpad swipe cannot feed horizontal motion into a vertical notch, and
    // cleared only by bitmap_is_stale() -- never by the pending-flip latch,
    // since this axis never turns the page.
    int                           hwheel_residual = 0;
    // Non-zero between a wheel-driven page flip and the completion that lands the
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
    // drop current_bitmap on a non-null swap -- only on the null one (see
    // set_view's null-view branch) -- so after a tab switch the canvas is
    // still holding, and still painting, the OUTGOING document's page until
    // the incoming render lands. Anything that MEASURES that bitmap has to
    // know it belongs to a different document; see scroll_into_view (Task 6).
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

    // #52: selection fill. Device-bound like the hit brushes above.
    ComPtr<ID2D1SolidColorBrush>  brush_selection_fill;

    // #52: the one live mouse gesture. Its gesture() is the single source of
    // truth for "is a drag in progress" -- there is no separate flag (spec §4.2).
    GestureState                  gesture;
    ClickCounter                  clicks;
    // The page's text, held only while a selection gesture is live.
    // Escrow-backed, so it stays safe to drop even if closing the tab destroys
    // its Document mid-drag (spec §3.3).
    litepdf::core::Document::TextPage drag_text;
    // The selection being dragged out. Painted INSTEAD of the view's committed
    // selection while a gesture is live, and simply discarded on cancel --
    // which is what leaves a double click's committed word intact when a
    // following drag is cancelled.
    std::optional<litepdf::core::TextSelection> live_selection;

    // Hits source and current-hit marker. hits_fn may be null if the
    // owner hasn't wired search yet — paint loop treats that as "no
    // overlay". current_hit is std::nullopt when no hit is selected.
    PdfCanvas::HitsFn             hits_fn;
    std::optional<litepdf::core::SearchSession::Hit> current_hit;
    // #135: the hits the last paint drew -- overlay_hits() as of that paint --
    // so on_search_hits_changed can tell whether another paint would differ.
    std::vector<litepdf::core::SearchSession::Hit> painted_hits;

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
    // Fired at the entry of the render-completion arm (#57).
    PdfCanvas::CompletionArrivedCb on_completion_arrived;

    // --- #55: the continuous strip ---
    // The canvas shows the strip. PdfCanvas::layout() derives the layout from
    // this and dual_page; only switch_layout writes it.
    bool                          strip = false;
    // The strip's viewport model: layout, scroll, pan, current page, scrollbar.
    StripViewport                 strip_view;
    // The strip's rendered pages and their render state.
    PageBitmapSet                 strip_set;
    // A scrollbar thumb drag is in progress (live), and a re-layout or layout
    // switch has ended it logically (dead): the queued track messages are then
    // ignored until SB_ENDSCROLL (spec 4.5). Task 6 handles the scrollbar.
    bool                          thumb_drag_live = false;
    bool                          thumb_drag_dead = false;
};

}  // namespace litepdf::ui
