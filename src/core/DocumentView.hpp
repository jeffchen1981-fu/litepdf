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

#include <windows.h>  // HINSTANCE / HWND — Phase 7 Task 8 ensure_thumb_pane.

#include "core/Document.hpp"

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
    float    zoom_scale() const noexcept;

    // Set zoom mode and recompute scale for the given viewport / DPI.
    //
    // Thread-safety: must be called on the UI thread only. The computed
    // scale_ is read by request_render() to build the CTM, and although
    // scale_ is a plain float (not atomic), in Phase 3 all callers
    // (kick_render, WM_SIZE, WM_DPICHANGED, zoom_in/out) run on the
    // UI thread and always call set_zoom_mode before request_render in
    // the same message handler — so the worker never observes a torn
    // read. If Phase 5+ introduces off-thread zoom changes, scale_
    // must become atomic or protected by a mutex.
    void     set_zoom_mode(ZoomMode mode,
                           float viewport_w_dip,
                           float viewport_h_dip,
                           float dpi = 96.0f);

    // Cycle through preset levels {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0}.
    // Both switch the mode to Custom and return true iff the scale changed.
    bool zoom_in();
    bool zoom_out();

    // Submit a render through the engine. Callback fires on a worker
    // thread per D2 (pix + worker_ctx) — see RenderEngine.hpp.
    using RenderCb = std::function<void(fz_pixmap*, fz_context*)>;
    void request_render(int page, RenderCb on_complete);

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
