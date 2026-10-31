#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

#include "core/Document.hpp"
#include "core/SearchSession.hpp"
#include "ui/detail/CompletionMath.hpp"
#include "ui/detail/EffectiveLayout.hpp"
#include "ui/detail/PageAnchor.hpp"
#include "ui/detail/ScrollMath.hpp"
#include "core/TextSelection.hpp"
#include "ui/detail/ViewportMath.hpp"
#include "ui/detail/SelectionDrag.hpp"
#include "ui/detail/StripCarry.hpp"
#include "ui/detail/StripLayout.hpp"

// Forward-decl so the header stays COM-free. ComPtr in .cpp only.
struct ID2D1Factory;
struct ID2D1HwndRenderTarget;
struct ID2D1Bitmap;
struct ID2D1SolidColorBrush;

// MuPDF forward decls — header stays mupdf-free; full types only in .cpp.
struct fz_context;
struct fz_pixmap;

namespace litepdf::core { class DocumentView; }

namespace litepdf::ui {

// Posted by render-done callback. WPARAM = fz_pixmap* (kept by worker),
// LPARAM = a heap RenderMeta* { core::EscrowContext escrow; the render's
// {epoch, page, slot, seq} identity }.
// A failed render has a null WPARAM and a meta with an empty escrow (#198), or a
// null LPARAM if that allocation failed. Canvas drops the pixmap through the escrow,
// then lets the escrow go — staying on the pixmap's own MuPDF root even if the
// producing DocumentView has been swapped or destroyed. The escrow also keeps
// the Document's lock table and the root context it was cloned from alive;
// without both, the drop would call through freed memory once the tab had
// closed -- the lock table for the drop itself, the root because MuPDF frees
// the family's colour profiles through it (#61). The identity is
// captured at submit time and decides whether the completion is still
// wanted: accept_completion (ui/detail/CompletionMath.hpp) drops a result
// from a superseded view (issue #35), from a submission a newer batch has
// superseded, from a page the user has left, and a right-slot pixmap
// arriving while the layout is single-page.
// Must match the reservation in MainWindow.cpp (WM_USER + 3 to + 6).
inline constexpr UINT WM_USER_RENDER_DONE = WM_USER + 3;
// (Phase 8 D10) Same payload as WM_USER_RENDER_DONE but the bitmap
// lands in the RIGHT slot of the two-page spread layout instead of the
// (single / left) slot. Posted by PdfCanvas::post_render_done_right.
inline constexpr UINT WM_USER_RENDER_DONE_RIGHT = WM_USER + 4;
// (#98) A worker-built page text for the active tab's current page. LPARAM = a
// heap TextMeta { core::Document::TextPage text; std::uint64_t epoch; }, freed
// by the handler; WPARAM is unused. Posted by PdfCanvas::post_text_ready.
inline constexpr UINT WM_USER_TEXT_READY = WM_USER + 5;
// (#55) A continuous-strip page render finished. WPARAM = fz_pixmap* (the
// worker's shipping ref) or null; LPARAM = a heap StripMeta { core::EscrowContext
// escrow; StripCompletionMeta id; }, allocated for a null result too, so every
// completion carries its {epoch, page, seq, cancelled} identity (spec 5.3). The
// escrow is empty for a null result. Same escrow discipline as
// WM_USER_RENDER_DONE. Posted from PdfCanvasStrip.cpp.
inline constexpr UINT WM_USER_STRIP_DONE = WM_USER + 6;

class PdfCanvas {
public:
    PdfCanvas(HINSTANCE hInstance, HWND parent);
    ~PdfCanvas();

    PdfCanvas(const PdfCanvas&)            = delete;
    PdfCanvas& operator=(const PdfCanvas&) = delete;

    HWND hwnd() const { return hwnd_; }

    // MainWindow owns the view; canvas holds a non-owning raw pointer
    // used to obtain fz_context* for fz_drop_pixmap on render-done.
    // Pass nullptr to clear.
    void set_view(litepdf::core::DocumentView* view);

    // Monotonic render epoch. Bumped on every set_view to another view (and
    // on every paged set_view), and on every layout switch into or out of the
    // strip (#55), so that an in-flight render submitted for a now-superseded
    // view or layout can be told apart from one submitted for the current one.
    // A same-view set_view in the strip does not bump. Callers capture this
    // at submit time and pass it to post_render_done; the completion
    // handler drops pixmaps whose epoch no longer matches (issue #35).
    std::uint64_t render_epoch() const noexcept;

    // Push the canvas's client extent + dpi into the active DocumentView so the
    // fit percentage is re-derived. In spread mode this passes the HALF-WIDTH
    // slot and the pair's other page, because both slots share one render scale
    // and a spread of unequal pages must fit the larger one.
    //
    // The dual-page submission branches call this: MainWindow::kick_render's dual
    // branch, resubmit_current_page's dual branch, and navigate_to_page's dual
    // branch. kick_render's single-page branch also calls it. The single-page
    // branches of resubmit_current_page and navigate_to_page do not, as deliberate
    // exceptions: navigate_to_page's single branch re-derives the fit by delegation through
    // DocumentView::set_current_page; resubmit_current_page's single branch has no
    // caller that changes the page or the fit mode, and any resize that races a
    // device-loss recovery is corrected by the kick_render in the same WM_SIZE
    // handler. Deriving the fit in only some paths leaves the rest rendering at a
    // stale percentage, which is why this function exists.
    void apply_viewport();

    // Get/set the canvas pan offset, in canvas DIPs. An axis whose content
    // fits the viewport is centered and its pan is 0; an axis that overflows
    // uses a TOP-LEFT origin, with the pan clamped to [viewport - content, 0]
    // (pan 0 = content's leading edge at the viewport's leading edge).
    // Used by MainWindow to snapshot/restore per-tab scroll on tab switch.
    // Both are no-ops if called before the impl is ready.
    struct Pan { float x; float y; };
    Pan  pan() const;
    void set_pan(float x, float y);

    // Post WM_USER_RENDER_DONE to `target` for the pixmap, together with a
    // core::EscrowContext cloned from `worker_ctx` so the UI thread can drop
    // the pixmap with the correct MuPDF root — keeping that root context and
    // its lock table alive — even if the producing DocumentView is torn down
    // before the message lands.
    //
    // Called from the worker thread inside the render callback, which
    // hands its shipping ref on the pixmap over to this helper (see
    // core/RenderEngine.cpp, D2). No extra ref is taken: on success the
    // UI thread inherits that one ref and drops it through the escrow.
    // On any failure (clone OOM, meta allocation, post FALSE) this helper
    // drops the pixmap itself — on worker_ctx if the clone failed, on the
    // escrow otherwise — and then releases the escrow. Either way the
    // caller must never drop the pixmap again. Returns true iff the
    // message was successfully posted. A null `pix` is a failed render: it
    // posts the identity below alone, and `worker_ctx` is not used.
    //
    // Callers: MainWindow::kick_render, resubmit_current_page,
    // navigate_to_page, the WM_MOUSEWHEEL zoom path.
    //
    // IDENTITY (PR-A2). `epoch` is render_epoch() read at submit time — it
    // says which VIEW the render belongs to. `page` says which page, and the
    // choice of function says which slot; together they let the handler drop a
    // pixmap the user has already paged away from. `seq` is next_render_seq()
    // read once for the whole submission batch — it says which SUBMISSION,
    // which is what decides whether this completion may consume the pending
    // page anchor. (epoch, page, slot) alone cannot: a same-page zoom or
    // resize produces a second P0 with an identical triple.
    static bool post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx,
                                 std::uint64_t epoch,
                                 int page,
                                 std::uint64_t seq);

    // (Phase 8 D10) Variant that posts to the RIGHT slot of the dual-
    // page layout. Same refcount discipline as post_render_done, and both
    // slots of one spread carry the SAME seq.
    static bool post_render_done_right(HWND target,
                                       fz_pixmap* pix,
                                       fz_context* worker_ctx,
                                       std::uint64_t epoch,
                                       int page,
                                       std::uint64_t seq);

    // (#98) Post WM_USER_TEXT_READY for a page text the render worker built.
    // The handle owns its MuPDF escrow, so on every failure branch (meta
    // allocation, PostMessageW) it is simply destroyed here; the caller never
    // frees anything. `epoch` is render_epoch() read at submit time. Returns
    // true iff the message was posted.
    static bool post_text_ready(HWND target,
                                litepdf::core::Document::TextPage text,
                                std::uint64_t epoch);

    // Open a new submission batch: bump the monotonic submission counter and
    // return its new value, which every request in this batch must carry.
    //
    // Call this ONCE per batch, before the request_render* calls — a spread's
    // two renders share one seq. The body also stamps whatever page anchor is
    // pending, which is what carries a navigation intent forward when a newer
    // submission supersedes an older one.
    std::uint64_t next_render_seq();

    // When true, on first real-bitmap paint the canvas calls
    // ColdStartTimer::emit_if_complete(true) so the line is mirrored to stderr.
    void set_log_timings(bool on) { log_timings_ = on; }

    // (Phase 8 D7) Flip the canvas background palette to dark gray when
    // the active view is in Invert Colors mode, or back to light when
    // it isn't. The page bitmap polarity is decided at the engine
    // (D7) — this method only flips the chrome around the bitmap so the
    // overall look matches. Triggers an InvalidateRect.
    void set_invert_chrome(bool on);

    // (Phase 8 D10) Switch between single-page and two-page-spread
    // layout. Discards the right-slot bitmap when going back to single
    // mode so a stale right-page render does not flash on the next
    // toggle. Caller (MainWindow) is responsible for kicking the
    // appropriate render after this returns. Triggers InvalidateRect.
    void set_dual_page(bool on);
    bool dual_page() const noexcept;

    // --- #55: continuous scroll ---

    // The layout on screen: Strip while the canvas shows the continuous strip,
    // else PagedSpread or PagedSingle from the dual-page flag. Derived, so a
    // paged single <-> spread toggle through set_dual_page keeps it right.
    LayoutKind layout() const noexcept;

    // The layout-switch reset of spec 3.4, for a switch into or out of the strip
    // (a paged single <-> spread switch keeps set_dual_page). Cancels any live
    // gesture first (without committing it), then the old layout's work, drops
    // every bitmap, bumps the render epoch, sets the dual-page flag for `next`
    // and adds or removes WS_VSCROLL, so a fit read after it already sees the
    // right client width. The caller kicks a render.
    //
    // `carry_position` (a switch within one tab: the two-page toggle) carries the
    // reader's position (spec 7.3, deviation 1): (page, in-page offset) and the
    // horizontal pan are captured from the old layout before anything changes,
    // all geometry from the view's page sizes x its zoom (never from bitmaps, so
    // a switch before the last render landed still carries); after the reset the
    // carried page becomes current (a spread's left page), the viewport refits
    // for the new layout, and the offset and pan are scaled by the zoom ratio
    // (after / before) and applied -- into the strip by building its layout
    // here. False (a tab switch) skips both: the incoming tab restores its own
    // position with its TabSwitch kick (spec 7.4).
    void switch_layout(LayoutKind next, bool carry_position);

    // The strip's per-tab state for MainWindow's outgoing tab snapshot (spec
    // 7.4): StripViewport::save() while the strip is shown and laid out, else
    // empty.
    std::optional<StripTabState> strip_tab_state() const;

    // MainWindow::kick_render's strip branch, after it has refit and refreshed
    // the zoom readout as `a` asks: build the strip layout if there is none,
    // else re-layout at the view's zoom when `a.relayout`; apply `restore` when
    // non-null; when this call built the layout and `restore` is null, navigate
    // to the view's current page (spec 7.4); restyle when `a.restyle`; then
    // recompute the current page,
    // update the scrollbar and reschedule. No-op unless the strip is shown.
    void strip_kick(const StripKickActions& a, const StripTabState* restore);

    // Explicit navigation in the strip (spec 6.3): scroll to `page`'s anchor,
    // clamped, make it the current page and pin it (spec 4.6), then recompute,
    // update the scrollbar and reschedule. Runs even when `page` is already the
    // current page, so going there mid-page scrolls back to its top. No page
    // anchor is installed. The caller follows with a Navigate kick. No-op unless
    // the strip is shown and laid out.
    void strip_navigate(int page);

    // The search start page in the strip (spec 6.3, #140): the first page on
    // screen, else the current page when only a gap is showing. The view's
    // current page when the strip is not shown or not laid out.
    int strip_top_visible_page() const;

    // (#159, #55) The pages a background strip tab keeps in L1: the strip's
    // visible pages, at most `cap` (the view's L1 count), the current page
    // first. Empty unless the strip is shown and laid out.
    std::vector<int> strip_retained_pages(std::size_t cap) const;

    // --- Phase 6 Task 9: search hit overlay ---

    // Source for overlay hits. PdfCanvas calls this each paint to fetch
    // the hits for the pages on screen: the shown page when paged, every
    // visible page when the strip is shown. Non-owning; caller must
    // ensure the lambda stays valid while set (or call set_hits_source(nullptr)
    // before the lambda's captures become invalid).
    using HitsFn = std::function<std::vector<litepdf::core::SearchSession::Hit>(std::size_t page)>;
    void set_hits_source(HitsFn fn);

    // Current hit (if any). Drawn in orange (+ outline); others drawn in
    // yellow. Pass std::nullopt to clear current highlight.
    void set_current_hit(std::optional<litepdf::core::SearchSession::Hit> h);

    // #135: a search session reported new hits. Repaints only when the hit
    // overlay would now draw something other than what the last paint drew --
    // a scan reports once per page, from every tab, and nearly all of those
    // leave the painted page untouched. The find bar's own query changes and
    // its close still invalidate directly; a cross-tab search's set_query on the
    // active tab repaints through the first update that follows it.
    void on_search_hits_changed();

    // --- Phase 7 Task 7: page-change observer ---
    //
    // Fires whenever the canvas's tracked current-page changes — covers
    // PgUp/PgDn/Home/End key nav, outline-navigate, search-jump
    // (scroll_into_view), cross-tab search results, AND tab switches
    // (set_view), so a per-tab listener (e.g. ThumbnailPane in T8) sees
    // the correct page on switch instead of going stale until the next
    // page-change event. The callback fires AFTER DocumentView's
    // current_page_ has been updated, with the new page index.
    //
    // Lifetime: caller owns the captured state. Pass nullptr (default
    // construction) to clear. Overwriting replaces the previous binding.
    using PageChangedCb = std::function<void(int new_page)>;
    void set_on_page_changed(PageChangedCb cb);

    // Fires after a successful Ctrl+mouse-wheel zoom, the one zoom path that
    // lives entirely in the canvas (the View-menu zoom commands persist via
    // MainWindow directly). Owner wires this to schedule a session save so a
    // wheel-zoom-only change survives a crash/force-kill. Pass nullptr to clear.
    using ZoomChangedCb = std::function<void()>;
    void set_on_zoom_changed(ZoomChangedCb cb);

    // #57: fires for EVERY render-completion message the canvas receives --
    // accepted, stale, superseded and failed alike -- before the canvas looks
    // at the message. The owner uses it to refresh anything derived from the
    // view's live state rather than from the pixmap (the zoom readout). The
    // callback must not submit renders or change the view. Pass nullptr to
    // clear.
    using CompletionArrivedCb = std::function<void()>;
    void set_on_completion_arrived(CompletionArrivedCb cb);

    // Page-change entry point for external callers (MainWindow's
    // outline-navigate and cross-tab search-results paths). Wraps
    // `view->set_current_page(idx)` and fires the page-change observer
    // when the page actually moves, so all mutation sites flow through
    // one observer fire-point. Returns true iff the page changed.
    // Safe to call before set_view (returns false).
    bool change_current_page(int idx);

    // Say where the page should land when the next submission batch completes.
    //
    // Deliberately NOT folded into change_current_page: a defensive page SNAP
    // (dual-mode canonicalisation) also changes the current page, runs on every
    // spread render including same-page ones, and must leave the pan alone.
    // Callers that navigate install an anchor; callers that canonicalise do not.
    //
    // The anchor is bound to a submission by the next next_render_seq() call,
    // so install it BEFORE kicking the render. install() does not disturb the
    // slot's already-stamped seq, so an anchor installed without a following
    // submission binds to whatever batch is already in flight and is applied
    // when that batch completes. Callers must only install on a path that
    // goes on to submit.
    void set_pending_anchor(PageAnchor anchor);

    // Scroll / page-change such that `h`'s quad is visible with a 24 DIP
    // margin. If already visible, no scroll — only the invalidate. If
    // target page differs from current, page is switched via
    // change_current_page; in a spread, only when the hit is on another pair,
    // and to that pair's left page. Installs a pending Hit anchor on every path
    // EXCEPT the already-visible one, which drops any pending anchor instead,
    // so the caller (MainWindow) must follow this with kick_render -- both to
    // render and to bind that anchor to the submission it opens.
    //
    // In the strip (spec 6.3) it scrolls the strip itself and installs no
    // anchor: no scroll when the quad is inside the viewport by the same 24 DIP
    // margin, else the quad is centred vertically; the hit's page becomes the
    // current page either way. The caller's Navigate kick then only reschedules.
    void scroll_into_view(const litepdf::core::SearchSession::Hit& h);

    // --- #52: text selection ---

    // Select every character on the current page (Edit > Select All). No-op in
    // two-page spread mode (spec §1) or on a page with no text -- which leaves
    // any existing selection alone.
    void select_all();

    // Whether select_all would act: the enable state of Edit > Select All,
    // which must match the dispatch exactly (see MainWindow's WM_INITMENUPOPUP).
    // Does not look at the page's text: on a page with none the item stays
    // enabled and select_all does nothing -- the safe direction, since a
    // grayed item would swallow Ctrl+A.
    bool can_select_all() const;

    // Put the painted selection on the clipboard (Edit > Copy): a live drag's,
    // else the active view's. Touches the clipboard not at all when there is no
    // selection.
    void copy_selection_to_clipboard() const;

    // Whether a live drag selection exists -- the one copy_selection_to_clipboard
    // prefers. Part of Edit > Copy's enable state, which must match the dispatch.
    bool has_live_selection() const noexcept;

private:
    friend struct PdfCanvasTestAccess;   // tests/unit/test_canvas_spread_wiring.cpp

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void register_class_once(HINSTANCE hInstance);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    void create_render_target();
    void discard_render_target();
    void resubmit_current_page();
    void on_paint();
    void on_size(int width, int height);
    LRESULT on_key_down(WPARAM key);

    // Change page, install `anchor`, drop stale bitmaps and submit the render
    // batch. The single funnel for every in-canvas navigation: PgUp / PgDn /
    // Home / End and the wheel's edge flips. No-op when the page does not
    // move, regardless of anchor (nothing to re-render, nothing to re-anchor).
    void navigate_to_page(int target, PageAnchor anchor);

    // Put the page already on screen at its top. Home and End use this when the
    // page they name is the one showing: they mean a position, not only a page,
    // and navigate_to_page declines a move to where you already are.
    LRESULT scroll_to_top();

    // Unpanned vertical origin of a page `src_h` high in canvas DIPs, i.e.
    // what on_paint would use with pan_y == 0. Single mode: place_bitmap
    // centres a fitting page and pins an overflowing one to 0. Dual mode: the
    // same, because the slot band is the full canvas height and on_paint's
    // union base_y is provably 0 there (a union taller than the band always
    // has t == 0, since an overflowing slot placement has y == 0).
    float page_origin_y(float src_h, float vp_h) const;

    // Pan that centres `h`'s quad vertically, using the same geometry as
    // on_paint. Extracted in Task 6; scroll_into_view and the Hit anchor share
    // it so the pre-render estimate and the post-render placement cannot drift.
    // Measured against the bitmap of the slot showing the hit's page (#191);
    // the current pan when that slot has not landed.
    float pan_y_for_hit(const litepdf::core::SearchSession::Hit& h) const;

    // Apply the anchor batch `seq` may take, and mark it applied if that placed
    // it. Called for each half that lands, and by right_half_failed.
    void place_anchor(std::uint64_t seq);

    // Batch `seq`, the newest, will bring no right bitmap: its render failed, or
    // its pixmap would not fit a bitmap. That ends the pair's landing as a right
    // bitmap would, so what was waiting for the extent runs now, or when this
    // batch's left half lands (#198).
    void right_half_failed(std::uint64_t seq);

    // Turn an anchor into a pan. Called from the completion handler once the
    // arriving bitmap is installed, so the page's real height is known.
    // Kind::None keeps the current pan and only re-clamps it, and in a spread
    // only once both halves have landed or the right one has failed (#171,
    // #198).
    //
    // Returns false when there is nothing to measure against yet (no bitmap,
    // no render target, or a Hit whose page's slot has not landed), and for a
    // Bottom or Hit in a spread until both halves are in or the right one has
    // failed. The caller must NOT mark the anchor applied in that case, or an
    // intent would be retired without ever taking effect -- the reachable paths
    // being a spread whose RIGHT half lands first, while navigate_to_page has
    // just reset both bitmaps, and a hit on the right page when the left half
    // lands first.
    bool apply_anchor(const PageAnchor& anchor);

    // Painted extent plus its origin, in canvas DIPs. `l`/`t` are zero for a
    // single page and non-zero for an unequal spread, where the union of the
    // two slots does not start at the canvas origin.
    struct ContentBox { float l, t, w, h; };

    LRESULT pan_by(float dx, float dy);

    // Plain (unmodified) mouse-wheel scrolling. Scrolls within the page, and
    // flips to the neighbouring page or spread once the pan is already at the
    // edge the wheel is pushing toward.
    LRESULT on_wheel_scroll(int delta);

    // Horizontal wheel scrolling (#56): WM_MOUSEHWHEEL, or Shift + the plain
    // wheel. Writes pan_x ONLY and never turns the page -- at the edge it
    // clamps.
    LRESULT on_hwheel_scroll(int raw_delta, litepdf::ui::HWheelSource src);

    // True while current_bitmap belongs to another view or page than the one
    // showing. set_view and navigate_to_page keep painting the outgoing bitmap
    // until the incoming render lands, and both wheels and pan_by (the arrow
    // keys and the hand tool, #120) drop a step until then. NOT !own_bitmap():
    // it is false when there is no bitmap at all.
    bool bitmap_is_stale() const;

    bool    content_extent(ContentBox& out) const;

    // True when current_bitmap is THIS view's rendering of THIS page, or of the
    // left page of THIS spread. set_view and navigate_to_page's single-page
    // branch both keep painting the outgoing bitmap until the incoming render
    // lands, so anything that measures the bitmap or draws page-space geometry
    // over it has to ask first.
    bool own_bitmap() const;

    // The same question for right_bitmap: THIS view's rendering of the right
    // page of THIS spread. False when the spread has no right page.
    bool own_right_bitmap() const;

    // True in a spread once both halves are this view's and this pair's, or the
    // left one is and the newest batch's right half failed (#198).
    bool spread_landed() const;

    // The height in DIPs of the page `page`, read from the own bitmap of the
    // slot showing it. False when no slot shows that page yet.
    bool shown_page_height(int page, float& out) const;

    // Where on_paint draws the single-page bitmap, in canvas DIPs. False when
    // there is no bitmap or render target yet.
    bool single_page_placement(Placement& out) const;

    // The search hits on_paint draws over the page right now: empty over another
    // view's or page's bitmap, before the render target exists, or with no hits
    // source. In a spread it is the hits of each half whose own bitmap is up
    // (#191). In the strip it is the hits of every page in V, with no bitmap
    // condition (spec 7.1); strip_paint draws them too.
    // on_paint and on_search_hits_changed both ask this, so the two cannot
    // disagree about what is on screen.
    std::vector<litepdf::core::SearchSession::Hit> overlay_hits() const;

    // Draws one search hit: the current-hit fill and stroke when it is the
    // current hit, else the other-hit fill. (ox, oy) is the page's top-left in
    // canvas DIPs and `pct` the DIPs per PDF point; the caller has checked
    // that the render target and both fill brushes exist (overlay_hits does).
    void paint_hit(const litepdf::core::SearchSession::Hit& hit,
                   float ox, float oy, float pct);

    // The selection on_paint draws, or null.
    const litepdf::core::TextSelection* painted_selection() const noexcept;

    // Fills `sel`'s quads with the selection brush. (ox, oy) is the page's
    // top-left in canvas DIPs and `pct` the DIPs per PDF point; draws nothing
    // without the brush. Paged on_paint and the strip (#55) both call it.
    void paint_selection_quads(const litepdf::core::TextSelection& sel,
                               float ox, float oy, float pct);

    // Starts a selection gesture on `page`, drawn at `pl`: clears the committed
    // selection, takes `text` as the drag's text, captures the mouse, and in
    // Word or Line mode selects at press time (spec §4.2). The caller has
    // checked that `text` is valid and no gesture is live. The paged press and
    // the strip's press on the page under the cursor (#55, spec 7.1) share it.
    void begin_selection(int page, const Placement& pl, litepdf::core::SelectMode mode,
                         int x_px, int y_px, litepdf::core::Document::TextPage text);

    // #52 selection gestures. The state machine is ui/detail/SelectionDrag.hpp;
    // spec §4.2 has the message ordering these depend on.
    void on_left_button_down(bool is_double_click_message, int x_px, int y_px);
    void on_mouse_move(int x_px, int y_px);
    void on_left_button_up(int x_px, int y_px);

    // #58 hand-tool panning (spec §5): middle-drag here; left-drag with space
    // held is routed to begin_pan_gesture by on_left_button_down.
    void on_middle_button_down(int x_px, int y_px);
    void on_middle_button_up(int x_px, int y_px);

    // Start a pan owned by `button`: begin_pan, SetCapture, and the pan cursor
    // -- which must be set here, because Windows sends no WM_SETCURSOR to a
    // window that holds the capture.
    void begin_pan_gesture(MouseButton button, int x_px, int y_px);

    // End a gesture that is live while this window does NOT hold the capture.
    // Returns whether it ended one.
    bool cancel_stale_gesture();

    // End any live gesture WITHOUT committing, drop its text handle and release
    // the capture. Never dereferences impl_->view: set_view calls it before
    // repointing, on the tab-close path where the outgoing view is already
    // destroyed (spec §3.3). Safe to re-enter from WM_CAPTURECHANGED.
    void cancel_gesture();

    // Recompute the live selection's highlight from its RAW anchor and extent.
    void refresh_live_selection();

    // Materialise the live selection -- quads and text -- into the active view.
    void commit_live_selection();

    // Client pixels -> a clamped point on the page drawn at `page`.
    litepdf::core::SelPoint page_point_at(int x_px, int y_px, const Placement& page) const;

    // True when the painted content overflows the viewport on either axis --
    // when pan_by can move anything at all, except while bitmap_is_stale(),
    // when pan_by drops every step (#120). Deliberately NOT stale-aware: a
    // captured pan gets no WM_SETCURSOR, so a drag begun in that window would
    // keep the arrow after the render lands and the pan works again.
    // In the strip (#55) it answers from StripViewport::can_pan: the strip is
    // taller or wider than the viewport.
    bool can_pan() const;

    // WM_SETCURSOR for the client area.
    void update_cursor();

    // update_cursor outside WM_SETCURSOR: when a gesture ends normally (a
    // selection drag or click, or a pan), and when space goes down or up with
    // the pointer still, and at the end of every strip change (content moved
    // under a still pointer). Does nothing unless the pointer is over this
    // window or this window holds the capture.
    void refresh_cursor();

    // --- #55: the continuous strip (PdfCanvasStrip.cpp) ---

    // The client size in DIPs: the client rect divided by dpi / 96. Needs no
    // render target.
    struct DipSize { double w; double h; };
    DipSize strip_viewport_dip() const;

    // WM_PAINT in the strip (spec 4.2, 5.1).
    void strip_paint();

    // WM_USER_STRIP_DONE (spec 5.3).
    void strip_on_done(WPARAM w, LPARAM l);

    // One scheduling pass (spec 5.2), then the bitmaps of the pages whose slot no
    // longer holds one are released.
    void strip_reschedule();

    // The scrollbar from the viewport model (spec 6.2).
    void strip_update_scrollbar();

    // The common tail of every strip change: recompute the current page, update
    // the scrollbar, reschedule, invalidate, and refresh the cursor. strip_after_change,
    // strip_update_scrollbar and strip_reschedule return at once unless the
    // strip is shown and has a layout.
    void strip_after_change();

    // The strip's current-page setter: DocumentView::set_current_page, and the
    // page-change observer only when the page changed (spec 4.6) -- without
    // change_current_page's cancel_gesture (spec 3.4). Returns whether it changed.
    bool set_strip_current_page(int page);

    // The strip is shown and laid out.
    bool strip_active() const noexcept;

    // Builds the strip layout from the view's page sizes at its zoom, keeping
    // the viewport model's current viewport size.
    void strip_build_layout();

    // switch_layout's carry (spec 7.3, deviation 1), around its reset. The
    // capture reads the layout still on screen; `pan_before` receives its
    // horizontal pan (0 from a spread). The apply runs once the reset has set
    // the dual-page flag and the scroll bar style for `next`.
    CarriedPosition capture_layout_carry(double& pan_before);
    void apply_layout_carry(LayoutKind next, CarriedPosition c, double pan_before,
                            double zoom_before);

    // The strip's input (spec 6.1, 6.2): the strip branches of on_wheel_scroll,
    // on_hwheel_scroll, on_key_down and pan_by, and WM_VSCROLL. Each moves the
    // StripViewport and ends in strip_after_change, moved or not, which derives
    // the current page (spec 4.6). No-ops unless the strip is shown and laid out.
    LRESULT strip_on_wheel(int delta);
    LRESULT strip_on_hwheel(int raw_delta, HWheelSource src);
    LRESULT strip_on_key(WPARAM key);
    LRESULT strip_pan_by(float dx, float dy);
    LRESULT strip_on_vscroll(WPARAM w);

    // A re-layout during a scrollbar thumb drag ends the drag logically (spec
    // 4.5): called after the re-layout's SetScrollInfo, it marks a live drag dead
    // so its queued track positions are ignored until SB_ENDSCROLL.
    void strip_end_thumb_drag() noexcept;

    HWND hwnd_ = nullptr;
    bool log_timings_ = false;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
