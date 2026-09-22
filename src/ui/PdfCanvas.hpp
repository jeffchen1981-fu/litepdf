#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

#include "core/SearchSession.hpp"
#include "ui/detail/CompletionMath.hpp"
#include "ui/detail/PageAnchor.hpp"
#include "ui/detail/ScrollMath.hpp"
#include "core/TextSelection.hpp"
#include "ui/detail/ViewportMath.hpp"
#include "ui/detail/SelectionDrag.hpp"

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
// On cancel/fail both are null. Canvas drops the pixmap through the escrow,
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
// Must match the reservation in MainWindow.cpp (WM_USER + 3).
inline constexpr UINT WM_USER_RENDER_DONE = WM_USER + 3;
// (Phase 8 D10) Same payload as WM_USER_RENDER_DONE but the bitmap
// lands in the RIGHT slot of the two-page spread layout instead of the
// (single / left) slot. Posted by PdfCanvas::post_render_done_right.
inline constexpr UINT WM_USER_RENDER_DONE_RIGHT = WM_USER + 4;

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

    // Monotonic render epoch. Bumped on every set_view so that an
    // in-flight render submitted for a now-superseded view can be told
    // apart from one submitted for the current view. Callers capture this
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
    // message was successfully posted.
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

    // --- Phase 6 Task 9: search hit overlay ---

    // Source for overlay hits. PdfCanvas calls this each paint to fetch
    // the hits for the currently visible page. Non-owning; caller must
    // ensure the lambda stays valid while set (or call set_hits_source(nullptr)
    // before the lambda's captures become invalid).
    using HitsFn = std::function<std::vector<litepdf::core::SearchSession::Hit>(std::size_t page)>;
    void set_hits_source(HitsFn fn);

    // Current hit (if any). Drawn in orange (+ outline); others drawn in
    // yellow. Pass std::nullopt to clear current highlight.
    void set_current_hit(std::optional<litepdf::core::SearchSession::Hit> h);

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
    // change_current_page. Installs a pending Hit anchor on every path
    // EXCEPT the already-visible one, so the caller (MainWindow) must
    // follow this with kick_render -- both to render and to bind that
    // anchor to the submission it opens.
    void scroll_into_view(const litepdf::core::SearchSession::Hit& h);

    // --- #52: text selection ---

    // Select every character on the current page (Edit > Select All). No-op in
    // two-page spread mode (spec §1) or on a page with no text -- which leaves
    // any existing selection alone.
    void select_all();

    // Put the active view's selection on the clipboard (Edit > Copy). Touches
    // the clipboard not at all when there is no selection.
    void copy_selection_to_clipboard() const;

private:
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

    // Unpanned vertical origin of the LEFT/single page in canvas DIPs, i.e.
    // what on_paint would use with pan_y == 0. Single mode: place_bitmap
    // centres a fitting page and pins an overflowing one to 0. Dual mode: the
    // same, because the slot band is the full canvas height and on_paint's
    // union base_y is provably 0 there (a union taller than the band always
    // has t == 0, since an overflowing slot placement has y == 0).
    float page_origin_y(float src_h, float vp_h) const;

    // Pan that centres `h`'s quad vertically, using the same geometry as
    // on_paint. Extracted in Task 6; scroll_into_view and the Hit anchor share
    // it so the pre-render estimate and the post-render placement cannot drift.
    float pan_y_for_hit(const litepdf::core::SearchSession::Hit& h) const;

    // Turn an anchor into a pan. Called from the completion handler once the
    // arriving bitmap is installed, so the page's real height is known.
    // Kind::None keeps the current pan and only re-clamps it.
    //
    // Returns false when there is nothing to measure against yet (no bitmap,
    // no render target). The caller must NOT mark the anchor applied in that
    // case, or an intent would be retired without ever taking effect -- the
    // reachable path being a spread whose RIGHT half lands first, while
    // navigate_to_page has just reset both bitmaps.
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
    // until the incoming render lands, and both wheels drop a notch until
    // then. NOT own_bitmap(): this compares against the CANONICAL left page in
    // spread mode, and it is false when there is no bitmap at all.
    bool bitmap_is_stale() const;

    bool    content_extent(ContentBox& out) const;

    // True when current_bitmap is THIS view's rendering of THIS page. set_view
    // and navigate_to_page's single-page branch both keep painting the outgoing
    // bitmap until the incoming render lands, so anything that measures the
    // bitmap or draws page-space geometry over it has to ask first.
    bool own_bitmap() const noexcept;

    // Where on_paint draws the single-page bitmap, in canvas DIPs. False when
    // there is no bitmap or render target yet.
    bool single_page_placement(Placement& out) const;

    // The selection on_paint draws, or null.
    const litepdf::core::TextSelection* painted_selection() const noexcept;

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
    void cancel_stale_gesture();

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
    // when pan_by can move anything at all.
    bool can_pan() const;

    // WM_SETCURSOR for the client area.
    void update_cursor();

    // update_cursor outside WM_SETCURSOR: when a gesture ends normally (a
    // selection drag or click, or a pan), and when space goes down or up with
    // the pointer still. Does nothing unless the pointer is over this window
    // or this window holds the capture.
    void refresh_cursor();

    HWND hwnd_ = nullptr;
    bool log_timings_ = false;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
