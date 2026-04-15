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
    RenderEngine(Document& doc, std::size_t num_workers = 2, PageCache* cache = nullptr);
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&) = delete;
    RenderEngine& operator=(RenderEngine&&) = delete;

    RenderToken submit(RenderRequest req);
    void cancel(const RenderToken& token);
    void cancel_all_below_priority(int priority);

    std::size_t num_workers() const noexcept;
    std::size_t pending_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
