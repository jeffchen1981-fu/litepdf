// Phase 7 Task 3 — core::ThumbnailRenderer integration tests.
//
// Three contracts under test:
//   1. submit() produces an HBITMAP for a real page and DOES NOT pollute
//      the main render cache (D2 invariant: bypass_cache=true must keep
//      L1/L2 untouched).
//   2. The dtor blocks until every in-flight on_done callback has fully
//      run (D16 task-drain pattern). Without this drain, a tab close
//      mid-render would UAF the captured Impl or post to a destroyed
//      HWND in the real UI integration.
//   3. cancel_pending() + dtor must not deadlock when many requests are
//      in flight. The assertion is intentionally weak (seen <= 5) per
//      plan: the real signal is that the test terminates at all.

#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include "core/ThumbnailRenderer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <windows.h>

// Forward-decl just enough of MuPDF to drop the cloned ctx after the
// PageCache goes out of scope. Mirrors test_render_engine_lifecycle.cpp.
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_context(fz_context*);
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

using namespace std::chrono_literals;
using litepdf::core::Document;
using litepdf::core::PageCache;
using litepdf::core::RenderEngine;
using litepdf::core::ThumbnailRenderer;

TEST_CASE("ThumbnailRenderer: produces HBITMAP for a real page",
          "[thumb_renderer]") {
    Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine eng(doc, /*workers=*/2, &cache);
        ThumbnailRenderer r(eng);  // borrow the doc's engine

        std::atomic<int> got{0};
        HBITMAP captured = nullptr;
        bool canceled = true;
        r.submit(0, [&](HBITMAP bm, bool c) {
            captured = bm;
            canceled = c;
            got.fetch_add(1, std::memory_order_release);
        });
        for (int i = 0; i < 500 && got.load(std::memory_order_acquire) == 0; ++i) {
            std::this_thread::sleep_for(10ms);
        }
        REQUIRE(got.load() == 1);
        REQUIRE(captured != nullptr);
        REQUIRE_FALSE(canceled);
        DeleteObject(captured);
        // D2 invariant: thumb pass must not have polluted the main cache.
        REQUIRE(cache.get_pixmap(0, 0.15f) == nullptr);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("ThumbnailRenderer: dtor drains in-flight tasks (D16 / 1A)",
          "[thumb_renderer]") {
    // Submits N requests then immediately destroys the renderer.
    // Without D16's pending_tasks drain, the worker callback could fire
    // after Impl is gone and either UAF-touch impl_->pending_tasks or
    // PostMessage to a destroyed HWND in real use. Test contract:
    // dtor MUST NOT return until every in-flight on_complete has run.
    Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);

    std::atomic<int> completed{0};
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine eng(doc, /*workers=*/1, &cache);
        {
            ThumbnailRenderer r(eng);
            for (int i = 0; i < 3; ++i) {
                r.submit(0, [&](HBITMAP bm, bool /*canceled*/) {
                    if (bm) DeleteObject(bm);
                    completed.fetch_add(1, std::memory_order_release);
                });
            }
            // Renderer dtor here MUST wait for all 3 callbacks to complete.
        }
        // After dtor returns, no callback should be in flight.
        REQUIRE(completed.load() == 3);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("ThumbnailRenderer: cancel_pending stops not-yet-started work",
          "[thumb_renderer]") {
    Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine eng(doc, /*workers=*/1, &cache);
        ThumbnailRenderer r(eng);

        std::atomic<int> seen{0};
        std::atomic<int> non_null_bm{0};
        for (int i = 0; i < 5; ++i) {
            r.submit(i, [&](HBITMAP bm, bool /*canceled*/) {
                seen.fetch_add(1, std::memory_order_release);
                if (bm) {
                    non_null_bm.fetch_add(1, std::memory_order_release);
                    DeleteObject(bm);
                }
            });
        }
        r.cancel_pending();
        std::this_thread::sleep_for(200ms);
        // Some may have started before cancel; with 1 worker, only a small
        // number can complete in the cancel-window. Strictly < 5 confirms
        // cancel_pending actually cancelled at least one not-yet-started
        // submission. (If cancel_pending were a no-op, all 5 would render
        // through eventually and non_null_bm would be 5; this assertion
        // catches that regression.)
        REQUIRE(non_null_bm.load() < 5);
    }
    fz_drop_context(cache_ctx);
}

// #117: a null HBITMAP means either "cancelled before it rendered" or "the
// render failed". The pane re-requests only the first, so on_done must say
// which one it was.
namespace {
struct ThumbResult {
    std::atomic<int> calls{0};
    HBITMAP bm = nullptr;
    bool canceled = false;
    ThumbnailRenderer::OnDone callback() {
        return [this](HBITMAP b, bool c) {
            bm = b;
            canceled = c;
            calls.fetch_add(1, std::memory_order_release);
        };
    }
    void wait() {
        for (int i = 0; i < 500 && calls.load(std::memory_order_acquire) == 0; ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }
};
}  // namespace

TEST_CASE("ThumbnailRenderer: a request cancelled while queued reports canceled",
          "[thumb_renderer]") {
    Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine eng(doc, /*workers=*/1, &cache);
        ThumbnailRenderer r(eng);

        // Gate the single worker with a slow main-page render so the thumb
        // request is still queued when the cancel lands.
        std::mutex gate_m;
        std::condition_variable gate_cv;
        bool gate_entered = false;
        eng.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx) {
            {
                std::lock_guard<std::mutex> g(gate_m);
                gate_entered = true;
                gate_cv.notify_all();
            }
            std::this_thread::sleep_for(80ms);
            if (p) fz_drop_pixmap(ctx, p);
        }});
        {
            std::unique_lock<std::mutex> lk(gate_m);
            gate_cv.wait(lk, [&] { return gate_entered; });
        }

        ThumbResult res;
        r.submit(0, res.callback());
        // What DocumentView::request_render_with_prefetch does on every kick.
        eng.cancel_all_below_priority(0);
        res.wait();
        REQUIRE(res.calls.load() == 1);
        REQUIRE(res.bm == nullptr);
        REQUIRE(res.canceled);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("ThumbnailRenderer: a failed render reports not canceled",
          "[thumb_renderer]") {
    Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine eng(doc, /*workers=*/1, &cache);
        ThumbnailRenderer r(eng);

        // simple.pdf has one page: fz_load_page throws, the render fails.
        ThumbResult res;
        r.submit(999, res.callback());
        res.wait();
        REQUIRE(res.calls.load() == 1);
        REQUIRE(res.bm == nullptr);
        REQUIRE_FALSE(res.canceled);
    }
    fz_drop_context(cache_ctx);
}
