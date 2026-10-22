#pragma once

// core::DocumentView — per-tab container bundling a Document with its
// RenderEngine, PageCache, a UI-thread fz_context* clone (D3), and
// (Phase 6) a per-tab SearchSession.
// Phase 3 Task 4. UI-agnostic: the Win32 MainWindow / PdfCanvas built
// on top of this in later Phase 3 tasks owns one DocumentView per tab.
//
// Lifetime contract (enforced by ~DocumentView):
//   1. SearchSession destructed first — drops the strong ref to its
//      internal State; any in-flight worker task still holds its own
//      shared_ptr copy, so State survives until workers finish, but the
//      session-side observer no longer sees updates.
//   2. RenderEngine destructed second — joins worker threads, so no
//      worker can still be touching the cache afterwards.
//   3. PageCache destructed third — drops any pixmaps / display lists
//      still in L1/L2 using the cache fz_context.
//   4. UI-thread fz_context dropped fourth.
//   5. Document cleaned up last via unique_ptr — SearchSession keeps a
//      `const Document&` (page_count() and query_compiles() on the UI
//      thread), so Document must outlive the session. In-flight search
//      tasks do not touch Document (#130): they use only the session's
//      own context, document and mutex, and ~SearchSession drains them
//      before dropping those. Because ~SearchSession drains every task,
//      nothing of the session's is left running on the dispatcher once
//      the session is gone; the dispatcher (owned above DocumentView, in
//      MainWindow, and outliving every tab) only has to outlive the
//      session. Document outliving the session is guaranteed by
//      declaring SearchSession AFTER Document in Impl so it destructs
//      FIRST.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <windows.h>  // HINSTANCE / HWND — Phase 7 Task 8 ensure_thumb_pane.

#include "core/Document.hpp"
#include "core/PageCache.hpp"     // kDefaultL1BudgetBytes (MuPDF-free)
// (#55) RenderEngine::RenderToken is a nested type and cannot be
// forward-declared. RenderEngine.hpp is MuPDF-free.
#include "core/RenderEngine.hpp"
#include "core/TextSelection.hpp"

// Forward decls — keep this header MuPDF-free. Callers that need to
// fz_drop_pixmap the ref delivered to a RenderCb will include
// <mupdf/fitz.h> on their own.
struct fz_pixmap;
struct fz_context;

namespace litepdf::app { class ISearchDispatcher; }
namespace litepdf::ui  { class ThumbnailPane; }

namespace litepdf::core {

class RenderEngine;
class PageCache;
class SearchSession;
class ThumbCache;
class ThumbnailRenderer;

class DocumentView {
public:
    enum class ZoomMode { FitWidth, FitPage, Custom };

    // Takes ownership of an already-opened Document. Constructs the
    // per-tab cache + engine + UI-thread context clone + SearchSession.
    // `dispatcher` must outlive this DocumentView (MainWindow owns one
    // ThreadPoolDispatcher for all tabs — see Phase 6 design §4 D5).
    // Throws std::runtime_error if `doc` is not opened (clone_context
    // returns null) or if context cloning fails (e.g. OOM).
    // `password` is the one `doc` was authenticated with, or none if it
    // needed none; the render workers (#126) and the search session (#130)
    // authenticate their own documents with it and do not keep it. Also
    // throws std::runtime_error if one of them fails to open or
    // authenticate its document. std::optional, not a bare string_view, so
    // a worker count passed in its place fails to compile instead of a
    // literal 0 converting to string_view(nullptr) (see RenderEngine.hpp).
    // `l1_budget_bytes` bounds the cache's L1 pixmaps by bytes as well as
    // by count, and decides which neighbours are prefetched (#49).
    explicit DocumentView(Document doc,
                          litepdf::app::ISearchDispatcher& dispatcher,
                          std::optional<std::string_view> password = std::nullopt,
                          std::size_t num_workers = 2,
                          std::size_t l1_capacity = 5,
                          std::size_t l2_capacity = 10,
                          std::size_t l1_budget_bytes = kDefaultL1BudgetBytes);
    ~DocumentView();

    DocumentView(const DocumentView&)            = delete;
    DocumentView& operator=(const DocumentView&) = delete;
    DocumentView(DocumentView&&)                 = delete;
    DocumentView& operator=(DocumentView&&)      = delete;

    int  page_count() const;
    int  current_page() const noexcept;
    // Clamps to [0, page_count-1]. A change drops the text slot and, unless
    // the view is continuous or the zoom is Custom, re-derives the fit for
    // the new page.
    bool set_current_page(int idx);

    // (#55) The continuous-scroll strip. While on, a page change does not
    // re-derive the fit: the whole strip has one zoom, refit only at fit
    // events (spec §4.3). Default off. UI thread only.
    void set_continuous(bool on) noexcept;
    bool continuous() const noexcept;

    // (#55) Every page's size, sized once and kept for the life of the view
    // (spec §4.4). A page whose size cannot be read takes page 0's size, and
    // page 0 unreadable gives US Letter (612 x 792 pt); a size failure never
    // throws. Those stand-ins are this list's own: the prefetch plan's size
    // cache (#49) still sees such a page as unknown. UI thread only.
    const std::vector<PageSize>& page_sizes();

    ZoomMode zoom_mode() const noexcept;

    // User-facing magnification. 1.0 means ONE PDF POINT MAPS TO ONE DIP -- the
    // conventional 96-dpi screen ratio browsers also call 100%. It is not
    // physical actual size: a PDF point is 1/72 inch, so 1.0 renders at 0.75x
    // ruler size. The status bar shows it as a percentage (#57).
    float zoom_pct() const noexcept;

    // Point -> PIXEL factor handed to MuPDF (fz_scale) and used as part of the
    // PageCache L1 key. This is the ONLY place the display dpi is applied; the
    // shipped code applied it here AND at every caller, rendering 2x oversized
    // at 200% scaling.
    float render_scale() const noexcept;

    // Set the viewport and, for FitWidth/FitPage, re-derive zoom_pct_ from it.
    // Custom leaves the percentage frozen.
    //
    // The viewport is in DEVICE PIXELS -- the raw GetClientRect extent. The
    // parameter names say _px because the shipped signature named them _dip
    // while every caller passed pixels, which is how the double-dpi defect got
    // in. `dpi` is the window dpi from GetDpiForWindow.
    //
    // `pair_page` is the other page of a two-page spread, or -1 in single-page
    // mode. When set, the fit uses max(width) and max(height) of the two pages
    // so one shared render scale fits both slots; a spread of unequal pages
    // would otherwise overflow the slot whose page is larger.
    //
    // Thread-safety: UI thread only, unchanged from the shipped contract. The
    // derived percentage is read by request_render() to build the CTM; all
    // callers set the viewport before requesting a render in the same message
    // handler, so a worker never observes a torn read.
    void set_viewport(float viewport_w_px, float viewport_h_px,
                      float dpi, int pair_page = -1);

    // Switch fit mode, re-deriving the percentage from the stored viewport.
    void set_zoom_mode_fit_width();
    void set_zoom_mode_fit_page();

    // Directly set a Custom percentage (session restore, and tests). Clamps to
    // the preset span [0.25, 8.0]; non-finite inputs clamp into range. Does not
    // require viewport dims.
    void set_zoom_pct(float pct) noexcept;

    // Step through preset percentages
    // {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0}.
    // Both switch the mode to Custom and return true iff the percentage changed.
    //
    // The table is percentages, which is what the shipped code got wrong: it
    // compared these values against a render scale that was routinely above 12,
    // so zoom_in() never found a larger rung and was a permanent no-op.
    bool zoom_in();
    bool zoom_out();

    // Submit a render through the engine. Callback fires on a worker
    // thread per D2 (pix + worker_ctx) — see RenderEngine.hpp.
    using RenderCb = std::function<void(fz_pixmap*, fz_context*)>;
    void request_render(int page, RenderCb on_complete);

    // ------------------------------------------------------------------
    // (Phase 8 D7/D9) Per-tab "Invert Colors" toggle.
    //
    // When on, all subsequent render requests submitted via this view
    // carry RenderRequest::invert = true, which causes the engine to
    // channel-invert the pixmap (fz_invert_pixmap) before caching it.
    // The L1 cache stores the inverted and non-inverted pixmaps for the
    // same (page, scale) under separate keys, so toggling does not
    // invalidate the previous-polarity cache entries.
    //
    // Default off; not persisted across app restarts (D9). Per-tab so
    // each open document keeps its own preference. UI thread only.
    bool invert_colors() const noexcept;
    void set_invert_colors(bool on);

    // ------------------------------------------------------------------
    // (Phase 8 D10) Per-tab "Two-Page Spread" layout toggle.
    //
    // Layout-only: the engine still renders single pages. PdfCanvas
    // submits two requests (left + right) when this flag is on and
    // blits both side-by-side. Page navigation steps by 2; the
    // cover-page rule (page 0 alone) is honored. Default off, per-tab,
    // not persisted across restarts (D9 scope reuse).
    bool dual_page() const noexcept;
    void set_dual_page(bool on);

    // ------------------------------------------------------------------
    // (#52) This tab's text selection -- at most one (spec §2, model "1b").
    //
    // Document-bound state, so it lives here beside current_page and the zoom,
    // and a tab switch carries it with no code in MainWindow. It also survives
    // a page change: it is cleared only by the canvas (the next click, a new
    // drag) or by clear_selection(). Not persisted to session.json. UI thread
    // only.
    const std::optional<TextSelection>& selection() const noexcept;
    void set_selection(TextSelection selection);
    void clear_selection() noexcept;

    // Bulk cancel on rapid nav (Phase 3 Task 11 wiring).
    void cancel_stale_renders(int keep_priority_threshold);

    // The tab is being left: cancel its queued P1 prefetch and P3 thumbnails
    // (RenderEngine::cancel_background). Queued P0 renders run on.
    void cancel_background_renders();

    // (#159) The tab is being left: drop its L1 pixmaps except those of
    // `page` and `other_page` (-1 for none) at the current render scale and
    // polarity. A background tab then holds what it shows, not a whole L1
    // budget, and switching back is still an L1 hit. Until this view's next
    // render request, L1 admits only those pages, so a render already running
    // cannot refill it (PageCache::retain_l1_pages). UI thread only.
    void trim_l1_for_background(int page, int other_page = -1);

    // Phase 3 Task 11: cancel queued P1/P3 work, submit P0 for `page`
    // with the caller's callback, and submit P1 prefetch for prev/next
    // pages with drop-only callbacks. The pixmaps land in PageCache
    // automatically at the engine level, so the next PgUp/PgDn is
    // served from cache.
    //
    // (#98) Iff `on_text` is given and wants_text_prefetch(page), the P0
    // request (never the P1 neighbours) also asks the worker for the page's
    // text: `on_text` then runs on the worker with the handle, which the
    // caller posts to the UI thread for offer_text(). The worker may skip
    // the build (spec §3.3); not getting `on_text` is always allowed.
    using TextCb = std::function<void(Document::TextPage)>;
    void request_render_with_prefetch(int page, RenderCb on_current_complete,
                                      TextCb on_text = {});

    // (#55) One strip page render at `priority`, at the current render scale
    // and polarity. Releases the background L1 pin first, as both requests
    // above do (#159), and cancels nothing. `cancel_flag` becomes the
    // returned token's flag: storing true to it cancels the request, with no
    // live engine needed (#117). Iff `on_text` is given and
    // wants_text_prefetch(page), the request also asks for the page's text,
    // as request_render_with_prefetch's P0 does (#98).
    RenderEngine::RenderToken request_page_render(
        int page, int priority, RenderCb on_complete,
        std::shared_ptr<std::atomic<bool>> cancel_flag, TextCb on_text = {});

    // (#55) Cancel queued thumbnail renders (P3) only; the strip's P0 and P1
    // requests run on.
    void cancel_thumbnail_renders();

    // ------------------------------------------------------------------
    // (#98) The per-tab text slot: at most one text page, the current
    // page's (spec §3.5). Dropped on a page change; dies with the tab. A
    // handle in the slot is never replaced. UI thread only.

    // The slot holds `page`'s text.
    [[nodiscard]] bool has_text_for(int page) const noexcept;

    // A render of `page` should ask for its text: single-page mode and the
    // slot does not hold it yet. Public for the unit tests.
    [[nodiscard]] bool wants_text_prefetch(int page) const noexcept;

    // A worker-built handle arriving on the UI thread. Kept iff it is valid,
    // for the current page, and the slot is empty; otherwise dropped.
    void offer_text(Document::TextPage t);

    // `page`'s text for a selection press or Select All: a share of the
    // slot's handle when it holds `page`, else a synchronous build (today's
    // path), which fills the slot when `page` is the current page. Empty
    // when the build fails; callers must tolerate that.
    [[nodiscard]] Document::TextPage text_for(int page);

    // The page whose text a queued P0 may still build, -1 for none. Test
    // observability only; nothing in the product reads it.
    [[nodiscard]] int pending_text_page() const noexcept;

    // (#49) Render requests queued in this view's engine and not yet taken by
    // a worker, cancelled ones included. Test observability only.
    [[nodiscard]] std::size_t queued_render_count() const noexcept;

    // (#55) Renders this view's engine has started: not cancelled before a
    // worker took them, not served from L1, not thumbnails. Test observability
    // only.
    [[nodiscard]] std::size_t renders_started() const noexcept;

    // (#159) L1 entries this view's cache holds, and whether one is `page` at
    // the current render scale and polarity. Test observability only.
    [[nodiscard]] std::size_t l1_entry_count() const;
    [[nodiscard]] bool l1_holds(int page) const;

    // (#55) Whether L1 holds `page` at the current render scale and polarity.
    // Unlike l1_holds it leaves the entry's LRU recency alone, so the strip
    // scheduler can ask without reordering L1 (PageCache::l1_contains).
    [[nodiscard]] bool l1_contains(int page) const;

    // UI thread's own fz_context clone (D3). Stays valid for the
    // lifetime of the DocumentView. Use for fz_drop_pixmap on refs
    // the caller moves onto the UI thread.
    fz_context* ui_ctx() const noexcept;

    // Source path (for window title bar etc.).
    const std::filesystem::path& source_path() const;

    // Read-only access to the underlying Document (for outline queries,
    // page count, etc.). The Document reference is valid for the
    // lifetime of this DocumentView.
    const Document& document() const;

    // Per-tab SearchSession. The reference is stable for the lifetime
    // of this DocumentView; do NOT cache across tab close/reopen.
    SearchSession&       search();
    const SearchSession& search() const;

    // Phase 7 Task 8: per-tab thumbnail pane (D2/D3/D11 — each tab has
    // its own ThumbCache, ThumbnailRenderer, and ThumbnailPane). The
    // pane and its cache + renderer are LAZILY created on first F4 press
    // for the tab — saves ~50 KB per never-thumbed tab. Subsequent calls
    // return the existing pane.
    //
    // The pane subclasses `parent_hwnd`'s WndProc; caller (MainWindow)
    // must guarantee `parent_hwnd` outlives the pane, which is enforced
    // by destruction order: ~DocumentView (via Impl) destroys the pane
    // before MainWindow's HWND tear-down.
    //
    // Returns the (possibly newly-created) pane. Never null after this
    // call. Set up with set_renderer + set_cache + set_page_count(...)
    // so the first F4 press already shows thumbs.
    litepdf::ui::ThumbnailPane* ensure_thumb_pane(HINSTANCE hInstance,
                                                  HWND parent_hwnd);

    // Returns the pane if it exists, or nullptr if F4 has never been
    // pressed for this tab. Safe to call any time. Used by MainWindow's
    // page-change observer + WM_DPICHANGED dispatcher to skip the pane
    // when the tab has never thumbed.
    litepdf::ui::ThumbnailPane* thumb_pane() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
