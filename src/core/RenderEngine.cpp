#include "core/RenderEngine.hpp"
#include "core/Document.hpp"
#include "core/PageCache.hpp"

// RenderEngine is the only TU that includes <mupdf/fitz.h> publicly; the
// header stays MuPDF-free so UI code can consume it without the MuPDF
// include path. Task 7 introduces actual rasterization, so we now need
// the full fitz types (fz_open_document, fz_load_page, fz_new_pixmap_from_page,
// fz_scale, fz_device_rgb, etc.).
#include <mupdf/fitz.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace litepdf::core {

struct RenderEngine::Impl {
    // Wraps a RenderRequest with stable-ordering metadata plus a
    // shared cancel flag copied from the returned RenderToken.
    // priority: lower value == more urgent (runs first).
    // seq: monotonically-increasing arrival tag; earlier seq wins on tie.
    // canceled: shared with the RenderToken; workers check it pre-callback.
    struct QueuedRequest {
        int priority;
        std::size_t seq;
        RenderRequest req;
        std::shared_ptr<std::atomic<bool>> canceled;
    };

    // Heap is a max-heap by default. We want the SMALLEST (priority, seq)
    // on top, so Compare returns true when `a` should sink — standard
    // invert-for-min-heap idiom, now applied against a vector-backed heap
    // (std::push_heap / std::pop_heap / std::make_heap) so we can iterate
    // the queue for cancel_all_below_priority().
    struct Compare {
        bool operator()(const QueuedRequest& a, const QueuedRequest& b) const {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.seq > b.seq;
        }
    };

    std::size_t num_workers = 0;
    // Optional PageCache. Non-owning; caller guarantees lifetime >= engine.
    // When null, worker_loop falls through to the direct render path.
    PageCache* cache = nullptr;
    std::vector<fz_context*> worker_ctxs;
    // Per-worker fz_document handles. MuPDF forbids sharing a fz_document
    // across contexts, so each worker gets its own handle opened on its
    // own cloned ctx. We pay N * parse-cost but gain thread isolation.
    std::vector<fz_document*> worker_docs;
    std::vector<std::thread> workers;
    mutable std::mutex mtx;  // mutable: pending_count() is const but locks
    std::condition_variable cv;
    bool stopping = false;
    // Heap-ordered vector (not std::priority_queue) — iteration required
    // by cancel_all_below_priority().
    std::vector<QueuedRequest> q;
    std::size_t next_seq = 1;  // guarded by mtx (submit holds it anyway)
    std::atomic<std::size_t> next_id{1};

    // Graceful shutdown: predicate wakes on stopping || !q.empty(); we only
    // return when stopping AND queue is empty, so in-flight queued requests
    // get drained by remaining workers on dtor.
    //
    // Rendering pipeline (Task 7):
    //   1. Pop request from heap under lock.
    //   2. Checkpoint A — cancel check before touching MuPDF.
    //   3. fz_load_page on the worker's private fz_document.
    //   4. Checkpoint B — cancel check after page load, before rasterize.
    //   5. fz_new_pixmap_from_page at the requested scale. Drop page.
    //   6. Checkpoint C — cancel check before handing ownership to callback.
    //   7. Callback takes ownership of pixmap (D2); if no callback, we drop it.
    //
    // Exception safety: fz_try/fz_catch wraps each MuPDF call individually so
    // a failure in load doesn't leak a pixmap, and a failure in rasterize
    // doesn't leak a page. On catch, pix is null-guarded and ownership is
    // not transferred. Silent failure for Phase 2; Phase 3 will add logging.
    static void worker_loop(Impl* impl, std::size_t idx) {
        fz_context* ctx = impl->worker_ctxs[idx];
        fz_document* wdoc = impl->worker_docs[idx];
        for (;;) {
            RenderRequest req;
            std::shared_ptr<std::atomic<bool>> canceled;
            {
                std::unique_lock<std::mutex> lk(impl->mtx);
                impl->cv.wait(lk, [impl]{ return impl->stopping || !impl->q.empty(); });
                if (impl->stopping && impl->q.empty()) return;
                // pop_heap moves the top element to the back; pop_back
                // destroys that slot. We move the payload out before
                // pop_back destroys the (now moved-from) element.
                std::pop_heap(impl->q.begin(), impl->q.end(), Compare{});
                auto entry = std::move(impl->q.back());
                impl->q.pop_back();
                req = std::move(entry.req);
                canceled = std::move(entry.canceled);
            }

            // Checkpoint A: pre-MuPDF cancel check.
            // D17: even on cancel, callback must fire (with nullptr) so callers
            // tracking pending_tasks can decrement (e.g. ThumbnailRenderer drain).
            if (canceled && canceled->load()) {
                if (req.on_complete) req.on_complete(nullptr, ctx);
                continue;
            }

            // L1 check — pixmap cache hit. On hit, cache has kept a ref for
            // us; we're the caller and must drop (or hand to callback, which
            // then owns the drop).
            // D18: bypass_cache requests skip L1 read entirely so thumb pixmaps
            // never displace main-render entries.
            if (impl->cache && !req.bypass_cache) {
                fz_pixmap* cached = impl->cache->get_pixmap(req.page_num, req.scale, req.invert);
                if (cached) {
                    // D3: L1-hit fast path still honors late cancellation before callback.
                    if (canceled && canceled->load()) {
                        fz_drop_pixmap(ctx, cached);
                        continue;
                    }
                    if (req.on_complete) req.on_complete(cached, ctx);
                    else fz_drop_pixmap(ctx, cached);
                    continue;  // skip MuPDF work entirely
                }
            }

            // L2 check — display list cache hit; else build one.
            // D18: bypass_cache requests skip L2 read so thumb display lists
            // never displace main-render entries.
            fz_display_list* dlist = nullptr;
            if (impl->cache && !req.bypass_cache) {
                dlist = impl->cache->get_display_list(req.page_num);
                // On hit, cache has kept a ref for us. We own this ref and
                // will drop it after rasterize.
            }

            if (!dlist) {
                // Miss: build a display list via fz_load_page +
                // fz_new_display_list_from_page. fz_try/fz_catch wraps each
                // call so a failure cleans up intermediate resources.
                fz_page* page = nullptr;
                fz_try(ctx) {
                    page = fz_load_page(ctx, wdoc, req.page_num);
                    dlist = fz_new_display_list_from_page(ctx, page);
                }
                fz_catch(ctx) {
                    if (dlist) { fz_drop_display_list(ctx, dlist); dlist = nullptr; }
                }
                if (page) fz_drop_page(ctx, page);

                // Populate L2 if we have a cache. Refcount discipline:
                //   fz_new_display_list_from_page → refcount 1 (we own).
                //   fz_keep_display_list          → refcount 2.
                //   put_display_list takes our "original" ref → cache holds 1.
                //   We retain the bump; dlist still points to same object.
                //   After rasterize, fz_drop_display_list → cache's ref stays.
                // D18: bypass_cache also skips L2 write.
                if (dlist && impl->cache && !req.bypass_cache) {
                    fz_keep_display_list(ctx, dlist);
                    impl->cache->put_display_list(req.page_num, dlist);
                }
            }

            // Checkpoint B: post-load / pre-rasterize cancel check.
            // D17: invoke callback with nullptr before continuing so the
            // ThumbnailRenderer pending_tasks drain stays balanced.
            if (canceled && canceled->load()) {
                if (dlist) fz_drop_display_list(ctx, dlist);
                if (req.on_complete) req.on_complete(nullptr, ctx);
                continue;
            }

            // Rasterize from display list (if we have one). fz_device_bgr(ctx)
            // returns a non-owned colorspace singleton; no drop required.
            // BGR + alpha=1 → BGRA buffer, byte-for-byte match for Direct2D
            // DXGI_FORMAT_B8G8R8A8_UNORM. Task 6 (D2D upload) becomes zero
            // channel-swap.
            fz_pixmap* pix = nullptr;
            if (dlist) {
                fz_device* draw_dev = nullptr;
                fz_try(ctx) {
                    fz_matrix m = fz_scale(req.scale, req.scale);
                    // Render onto an OPAQUE WHITE background. The convenience
                    // fz_new_pixmap_from_display_list with alpha=1 clears to
                    // TRANSPARENT; the canvas's D2D bitmap is
                    // D2D1_ALPHA_MODE_IGNORE, which paints transparent pixels as
                    // opaque BLACK — so any page the document does not fill with
                    // its own background (most real-world PDFs, EPUB) rendered
                    // black. Keep the 4-channel BGRA buffer (byte-match for
                    // DXGI_FORMAT_B8G8R8A8_UNORM) but clear it to white first,
                    // then run the page over it. Invert (below) flips white->
                    // black for dark mode, as before.
                    fz_irect bbox = fz_round_rect(
                        fz_transform_rect(fz_bound_display_list(ctx, dlist), m));
                    pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox,
                                                  nullptr, /*alpha*/ 1);
                    fz_clear_pixmap_with_value(ctx, pix, 0xFF);  // opaque white
                    draw_dev = fz_new_draw_device(ctx, fz_identity, pix);
                    fz_run_display_list(ctx, dlist, draw_dev, m,
                                        fz_infinite_rect, nullptr);
                    fz_close_device(ctx, draw_dev);
                }
                fz_always(ctx) {
                    if (draw_dev) fz_drop_device(ctx, draw_dev);
                }
                fz_catch(ctx) {
                    if (pix) { fz_drop_pixmap(ctx, pix); pix = nullptr; }
                }
                fz_drop_display_list(ctx, dlist);
                dlist = nullptr;
            }

            // (Phase 8 D7) Channel-invert the pixmap if requested. Memory-
            // bandwidth-bound (~3 ms on a hot A4 @ 150 DPI; see R3) and
            // happens BEFORE caching so the L1 entry matches the request's
            // polarity. The display list (L2) was already cached untouched
            // — D8 keeps L2 polarity-independent.
            if (pix && req.invert) {
                fz_try(ctx) {
                    fz_invert_pixmap(ctx, pix);
                }
                fz_catch(ctx) {
                    fz_drop_pixmap(ctx, pix);
                    pix = nullptr;
                }
            }

            // Populate L1 if we have a cache and a successful render.
            //   fz_new_pixmap_from_display_list → refcount 1 (we own).
            //   fz_keep_pixmap                  → refcount 2.
            //   put_pixmap takes our "original" ref → cache holds 1.
            //   We retain the bump; hand to callback (caller owns) or drop.
            // D18: bypass_cache also skips L1 write.
            if (pix && impl->cache && !req.bypass_cache) {
                fz_keep_pixmap(ctx, pix);
                impl->cache->put_pixmap(req.page_num, req.scale, req.invert, pix);
            }

            // Checkpoint C: post-rasterize cancel check. If the request was
            // canceled after we already paid to rasterize, drop the pixmap
            // and skip the callback — the consumer no longer wants the work.
            // (Cache still holds its own ref from put_pixmap above.)
            // D17: still invoke callback with nullptr so pending_tasks drain
            // can decrement on the cancel path.
            if (canceled && canceled->load()) {
                if (pix) fz_drop_pixmap(ctx, pix);
                if (req.on_complete) req.on_complete(nullptr, ctx);
                continue;
            }

            // Run user callback OUTSIDE the lock to avoid deadlocks if they
            // call back into the engine (e.g. submit from within on_complete).
            // Ownership of pix transfers to the callback per D2.
            if (req.on_complete) {
                req.on_complete(pix, ctx);
            } else if (pix) {
                // No callback supplied; we own pix — drop to avoid a leak.
                fz_drop_pixmap(ctx, pix);
            }
        }
    }
};

RenderEngine::RenderEngine(Document& doc, std::size_t n, PageCache* cache)
    : impl_(std::make_unique<Impl>()) {
    if (n == 0)
        throw std::invalid_argument("RenderEngine: num_workers must be >= 1");

    impl_->num_workers = n;
    impl_->cache = cache;  // non-owning; may be null (direct render path)
    impl_->worker_ctxs.reserve(n);

    // Clone all contexts first; clean up on failure.
    try {
        for (std::size_t i = 0; i < n; ++i) {
            fz_context* ctx = doc.clone_context();
            if (!ctx) {
                throw std::runtime_error("RenderEngine: failed to clone fz_context "
                                         "(is the Document open?)");
            }
            impl_->worker_ctxs.push_back(ctx);
        }
    } catch (...) {
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
        impl_->worker_ctxs.clear();
        throw;
    }

    // Open a per-worker fz_document on each worker's ctx. MuPDF does not
    // allow sharing fz_document across contexts, so each worker parses the
    // file independently. Acceptable cost on Phase 2's 2-worker default.
    //
    // fz_open_document takes UTF-8. std::filesystem::path::string() returns
    // the native ACP on Windows MSVC; path::u8string() returns UTF-8. Match
    // the Document::open conversion so non-ASCII paths round-trip correctly.
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const auto path_u8 = doc.source_path().u8string();
    const std::string path_str(reinterpret_cast<const char*>(path_u8.data()),
                               path_u8.size());
#else
    const std::string path_str = doc.source_path().u8string();
#endif
    if (path_str.empty()) {
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
        impl_->worker_ctxs.clear();
        throw std::runtime_error("RenderEngine: Document has no source path "
                                 "(is the Document open?)");
    }

    try {
        impl_->worker_docs.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            fz_context* ctx = impl_->worker_ctxs[i];
            fz_document* wdoc = nullptr;
            fz_try(ctx) {
                wdoc = fz_open_document(ctx, path_str.c_str());
            }
            fz_catch(ctx) {
                // Leave wdoc null; throw std::runtime_error below.
            }
            if (!wdoc) {
                throw std::runtime_error(
                    "RenderEngine: fz_open_document failed for worker " +
                    std::to_string(i));
            }
            impl_->worker_docs.push_back(wdoc);
        }
    } catch (...) {
        // Rollback: drop any per-worker docs we already opened, each with
        // its own ctx (docs MUST be dropped with the ctx they were opened
        // on — MuPDF does not support cross-context drops), then drop ctxs.
        for (std::size_t i = 0; i < impl_->worker_docs.size(); ++i) {
            fz_drop_document(impl_->worker_ctxs[i], impl_->worker_docs[i]);
        }
        impl_->worker_docs.clear();
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
        impl_->worker_ctxs.clear();
        throw;
    }

    // Spawn workers only after all contexts and docs are ready.
    impl_->workers.reserve(n);
    try {
        for (std::size_t i = 0; i < n; ++i) {
            impl_->workers.emplace_back(&Impl::worker_loop, impl_.get(), i);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->stopping = true;
        }
        impl_->cv.notify_all();
        for (auto& t : impl_->workers) {
            if (t.joinable()) t.join();
        }
        for (std::size_t i = 0; i < impl_->worker_docs.size(); ++i) {
            fz_drop_document(impl_->worker_ctxs[i], impl_->worker_docs[i]);
        }
        impl_->worker_docs.clear();
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
        impl_->worker_ctxs.clear();
        throw;
    }
}

RenderEngine::~RenderEngine() {
    if (impl_) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->stopping = true;
        }
        impl_->cv.notify_all();
        for (auto& t : impl_->workers) {
            if (t.joinable()) t.join();
        }
        // Docs must be dropped on the SAME ctx they were opened on, and
        // BEFORE the ctxs themselves are dropped.
        for (std::size_t i = 0; i < impl_->worker_docs.size(); ++i) {
            fz_drop_document(impl_->worker_ctxs[i], impl_->worker_docs[i]);
        }
        impl_->worker_docs.clear();
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
    }
}

// Starvation note: a steady stream of high-priority submissions can
// indefinitely delay low-priority work. Acceptable for Phase 2 — UI
// submissions are bounded per paint cycle. Revisit if needed later.
RenderEngine::RenderToken RenderEngine::submit(RenderRequest req) {
    RenderToken tok;
    tok.id = impl_->next_id.fetch_add(1);
    tok.canceled = std::make_shared<std::atomic<bool>>(false);
    int prio = req.priority;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        std::size_t seq = impl_->next_seq++;
        impl_->q.push_back({prio, seq, std::move(req), tok.canceled});
        std::push_heap(impl_->q.begin(), impl_->q.end(), Impl::Compare{});
    }
    impl_->cv.notify_one();
    return tok;
}

void RenderEngine::cancel(const RenderToken& token) {
    // No lock required: the flag is a std::atomic, and the shared_ptr
    // keeps it alive for as long as either side holds a reference.
    if (token.canceled) token.canceled->store(true);
}

void RenderEngine::cancel_all_below_priority(int p) {
    // "Below priority p" means less urgent, i.e. priority value strictly
    // greater than p (lower value = more urgent in this engine).
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (auto& entry : impl_->q) {
        if (entry.priority > p && entry.canceled) {
            entry.canceled->store(true);
        }
    }
    // We do NOT remove entries from the heap — flagging is enough, and
    // leaving the heap untouched avoids any re-heapify cost.
}

std::size_t RenderEngine::num_workers() const noexcept { return impl_->num_workers; }

std::size_t RenderEngine::pending_count() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->q.size();
}

} // namespace litepdf::core
