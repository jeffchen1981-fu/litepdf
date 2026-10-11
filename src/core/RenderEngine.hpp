#pragma once

// core::RenderEngine — thread-pool-backed page renderer.
//
// Design notes (frozen per Phase 2 plan, decisions D1-D9):
//  - PIMPL: no MuPDF headers leak through this header. Only forward
//    declarations of fz_pixmap / fz_context are used.
//  - on_complete fires on a worker thread. The fz_context* passed to
//    the callback is the worker's cloned context and is valid ONLY for
//    the duration of the callback. Do not capture or retain it.
//  - Caller owns the fz_pixmap* delivered to on_complete (D2). It must
//    be dropped with fz_drop_pixmap using a context the caller owns.
//  - cancel() is cooperative, not preemptive: a worker checks the
//    canceled flag at safe points. In-flight renders may still complete.
//  - Not copyable and not movable: will hold std::thread resources once
//    wired in Task 3.
//  - This is the Task 2 scaffold. Thread pool, work queue, cancellation
//    wiring, and actual rendering land in Tasks 3-7.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

// Forward declarations — keep this header MuPDF-free (PIMPL).
struct fz_pixmap;
struct fz_context;

namespace litepdf::core { class PageCache; }

namespace litepdf::core {

class Document;

class RenderEngine {
public:
    struct RenderToken {
        std::size_t id = 0;
        std::shared_ptr<std::atomic<bool>> canceled;
    };

    struct RenderRequest {
        int   page_num  = 0;
        int   priority  = 0;     // 0 = highest; 1 = adjacent; 2 = prefetch
        float scale     = 1.0f;  // 1.0 = 72 dpi
        std::function<void(fz_pixmap*, fz_context*)> on_complete;
        // (Phase 7) when true, this request neither reads from nor populates
        // L1 OR L2. Used by ThumbnailRenderer so thumb pixmaps and their
        // display lists never displace main-render entries. Placed AFTER
        // on_complete so existing 4-element aggregate-init call sites
        // (`{page, prio, scale, callback}`) keep compiling unchanged.
        bool  bypass_cache = false;
        // (Phase 8 D7) Channel-invert the pixmap (fz_invert_pixmap) AFTER
        // rasterize and BEFORE caching. The L1 cache key includes this
        // axis so the inverted and non-inverted pixmaps for the same
        // (page, scale) coexist; L2 (display list) stays polarity-
        // independent (D8). Default false: existing 5-element aggregate
        // call sites keep compiling unchanged.
        bool  invert       = false;
        // (#117) Optional caller-owned cancel flag. When set, submit() uses it
        // as the token's flag instead of making a new one, so on_complete can
        // read it to tell a cancelled request (flag set) from a failed render
        // (flag clear) -- both deliver a null pixmap. cancel_all_below_priority
        // flags only requests that are still queued, so once a worker has
        // taken a request it cannot flag it under a failed render. cancel(token)
        // can flag it at any time, so a caller that also cancels by token can
        // see a failed render reported as cancelled.
        std::shared_ptr<std::atomic<bool>> cancel_flag;
    };

    // Construct a RenderEngine.
    //
    // @param doc          The Document to render. Must outlive this engine.
    // @param num_workers  Number of worker threads (each gets a cloned
    //                     fz_context + fz_document). Must be >= 1.
    // @param cache        Optional PageCache for L1/L2 lookups. If non-null,
    //                     workers consult the cache before rendering and
    //                     populate it after. The cache pointer MUST outlive
    //                     this RenderEngine — the engine does not own it
    //                     and will not drop it in its destructor. Passing
    //                     nullptr falls through to the direct render path
    //                     (equivalent to the pre-cache Task 7 behavior).
    // @param password     The password `doc` was authenticated with, or none
    //                     if it needed none. Each worker's own fz_document is
    //                     authenticated with it; without that, a PDF with a
    //                     user password renders blank (#126). Throws
    //                     std::runtime_error if a worker rejects it. The
    //                     engine's NUL-terminated copy is wiped before the
    //                     constructor returns (Phase 8 D3). std::optional
    //                     rather than a bare string_view, so a stray integer
    //                     cannot compile here: a literal 0 would otherwise
    //                     convert to string_view(nullptr), which is undefined.
    RenderEngine(Document& doc, std::size_t num_workers = 2, PageCache* cache = nullptr,
                 std::optional<std::string_view> password = std::nullopt);
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&) = delete;
    RenderEngine& operator=(RenderEngine&&) = delete;

    RenderToken submit(RenderRequest req);
    void cancel(const RenderToken& token);
    void cancel_all_below_priority(int priority);
    // Flag every queued request except the P0 visible-page renders: P1
    // neighbour prefetch and P3 thumbnails. For a view the user has left (a
    // tab switch). P0 stays: a cancelled P0 still posts a null completion
    // (D17), and PdfCanvas handles a null before its epoch check, so the left
    // tab's cancel would clear the shared canvas's pending wheel flip.
    void cancel_background();

    std::size_t num_workers() const noexcept;
    std::size_t pending_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
