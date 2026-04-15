#pragma once

// core::DocumentView — per-tab container bundling a Document with its
// RenderEngine, PageCache, and a UI-thread fz_context* clone (D3).
// Phase 3 Task 4. UI-agnostic: the Win32 MainWindow / PdfCanvas built
// on top of this in later Phase 3 tasks owns one DocumentView per tab.
//
// Lifetime contract (enforced by ~DocumentView):
//   1. RenderEngine destructed first — joins worker threads, so no
//      worker can still be touching the cache afterwards.
//   2. PageCache destructed second — drops any pixmaps / display lists
//      still in L1/L2 using the UI-thread fz_context.
//   3. UI-thread fz_context dropped third.
//   4. Document cleaned up last via unique_ptr.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>

#include "core/Document.hpp"

// Forward decls — keep this header MuPDF-free. Callers that need to
// fz_drop_pixmap the ref delivered to a RenderCb will include
// <mupdf/fitz.h> on their own.
struct fz_pixmap;
struct fz_context;

namespace litepdf::core {

class RenderEngine;
class PageCache;

class DocumentView {
public:
    enum class ZoomMode { FitWidth, FitPage, Custom };

    // Takes ownership of an already-opened Document. Constructs the
    // per-tab cache + engine + UI-thread context clone.
    // Throws std::runtime_error if `doc` is not opened (clone_context
    // returns null) or if context cloning fails (e.g. OOM).
    explicit DocumentView(Document doc,
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
