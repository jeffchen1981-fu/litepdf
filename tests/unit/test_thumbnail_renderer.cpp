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
#include <thread>
#include <windows.h>

// Forward-decl just enough of MuPDF to drop the cloned ctx after the
// PageCache goes out of scope. Mirrors test_render_engine_lifecycle.cpp.
extern "C" {
    struct fz_context;
    void fz_drop_context(fz_context*);
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
        r.submit(0, [&](HBITMAP bm) {
            captured = bm;
            got.fetch_add(1, std::memory_order_release);
        });
        for (int i = 0; i < 500 && got.load(std::memory_order_acquire) == 0; ++i) {
            std::this_thread::sleep_for(10ms);
        }
        REQUIRE(got.load() == 1);
        REQUIRE(captured != nullptr);
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
                r.submit(0, [&](HBITMAP bm) {
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
            r.submit(i, [&](HBITMAP bm) {
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
