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
//   5. Document cleaned up last via unique_ptr — in-flight search
//      workers capture `const Document&` by reference; Document must
//      outlive those tasks. Since the SearchDispatcher is owned ABOVE
//      DocumentView (in MainWindow) and outlives every tab, any task
//      still running after DocumentView destruction keeps a
//      shared_ptr<State> alive but also needs Document alive — that is
//      guaranteed by declaring SearchSession AFTER Document in Impl so
//      it destructs FIRST, and by having the dispatcher outlive all
//      DocumentViews (enforced at the MainWindow level).

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <windows.h>  // HINSTANCE / HWND — Phase 7 Task 8 ensure_thumb_pane.

#include "core/Document.hpp"
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
    explicit DocumentView(Document doc,
                          litepdf::app::ISearchDispatcher& dispatcher,
                          std::size_t num_workers = 2,
                          std::size_t l1_capacity = 5,
                          std::size_t l2_capacity = 10);
    ~DocumentView();

    DocumentView(const DocumentView&)            = delete;
    DocumentView& operator=(const DocumentView&) = delete;
    DocumentView(DocumentView&&)                 = delete;
    DocumentView& operator=(DocumentView&&)      = delete;

    int  page_count() const;
    int  current_page() const noexcept;
    bool set_current_page(int idx);  // clamps to [0, page_count-1]

    ZoomMode zoom_mode() const noexcept;

    // User-facing magnification. 1.0 means ONE PDF POINT MAPS TO ONE DIP -- the
    // conventional 96-dpi screen ratio browsers also call 100%. It is not
    // physical actual size: a PDF point is 1/72 inch, so 1.0 renders at 0.75x
    // ruler size. Nothing surfaces a numeric percentage today; this is the
    // definition an eventual readout must be built on.
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

    // Phase 3 Task 11: cancel stale P1/P2 work, submit P0 for `page`
    // with the caller's callback, and submit P1 prefetch for prev/next
    // pages with drop-only callbacks. The pixmaps land in PageCache
    // automatically at the engine level, so the next PgUp/PgDn is
    // served from cache.
    void request_render_with_prefetch(int page, RenderCb on_current_complete);

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
