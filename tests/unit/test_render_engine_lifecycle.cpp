#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"

// MuPDF C functions used to drop owned objects in tests. Forward-declared so
// this TU stays MuPDF-header-free (matching test_render_engine_cache.cpp).
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    void fz_drop_context(fz_context*);
}

// Task 2 scaffold ignored doc; Task 3 clones ctx so tests now open the fixture.
TEST_CASE("RenderEngine scaffold constructs and destructs cleanly", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    {
        litepdf::core::RenderEngine engine(doc);
        REQUIRE(engine.num_workers() == 2);
        REQUIRE(engine.pending_count() == 0);
    }
    SUCCEED();
}

TEST_CASE("RenderEngine respects custom worker count", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 4);
    REQUIRE(engine.num_workers() == 4);
}

TEST_CASE("RenderEngine::submit returns a populated token", "[core][render][lifecycle]") {
    // Task 4: submit() now yields a nonzero id and an initialized canceled flag.
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    auto tok = engine.submit({0, 0, 1.0f, nullptr});
    REQUIRE(tok.id != 0);
    REQUIRE(tok.canceled);
    REQUIRE(!tok.canceled->load());
}

TEST_CASE("RenderEngine spawns and joins worker threads cleanly", "[core][render][pool]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    auto t0 = std::chrono::steady_clock::now();
    {
        litepdf::core::RenderEngine engine(doc, 2);
        // workers should be idle, not spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    // Ctor + 50ms sleep + dtor should all complete in well under 500ms.
    REQUIRE(elapsed < std::chrono::milliseconds(500));
}

TEST_CASE("RenderEngine with 4 workers starts and joins", "[core][render][pool]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 4);
    REQUIRE(engine.num_workers() == 4);
}

TEST_CASE("RenderEngine throws when Document is not open", "[core][render][pool]") {
    litepdf::core::Document doc;  // never opened
    REQUIRE_THROWS_AS(litepdf::core::RenderEngine(doc, 2), std::runtime_error);
}

TEST_CASE("RenderEngine rejects zero workers", "[core][render][pool]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    REQUIRE_THROWS_AS(litepdf::core::RenderEngine(doc, 0), std::invalid_argument);
}

// Phase 7 D2 + D18 contract: a request with bypass_cache=true must touch
// neither L1 nor L2 — neither read from nor write to the cache. This is what
// allows ThumbnailRenderer to share the engine without polluting main-render
// cache entries.
TEST_CASE("RenderEngine: bypass_cache=true touches neither L1 nor L2",
          "[core][render][bypass]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        litepdf::core::PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        litepdf::core::RenderEngine engine(doc, /*workers=*/1, &cache);

        std::atomic<bool> done{false};
        std::condition_variable cv;
        std::mutex mu;
        litepdf::core::RenderEngine::RenderRequest req;
        req.page_num     = 0;
        req.priority     = 0;
        req.scale        = 0.15f;
        req.bypass_cache = true;
        req.on_complete  = [&](fz_pixmap* pix, fz_context* ctx) {
            if (pix) fz_drop_pixmap(ctx, pix);
            { std::lock_guard<std::mutex> lk(mu); done = true; }
            cv.notify_one();
        };
        engine.submit(std::move(req));

        {
            std::unique_lock<std::mutex> lk(mu);
            REQUIRE(cv.wait_for(lk, std::chrono::seconds(15),
                                [&]{ return done.load(); }));
        }

        // D2 + D18: BOTH cache tiers untouched after a bypass_cache request.
        REQUIRE(cache.l1_size() == 0);
        REQUIRE(cache.l2_size() == 0);
    }
    fz_drop_context(cache_ctx);
}

// Phase 7 D17 contract: every request, even one cancelled at any of the three
// cancel checkpoints, must produce exactly one on_complete invocation. Without
// this, ThumbnailRenderer's pending_tasks drain pattern would deadlock — the
// counter could never decrement on cancel paths. Probabilistic assertion: with
// a 5-deep queue and a single worker we are virtually guaranteed at least one
// nullptr callback (i.e. cancel path observed) once cancel_all_below_priority
// fires before the worker has drained.
TEST_CASE("RenderEngine: cancel still invokes on_complete with nullptr "
          "(D17 contract for ThumbnailRenderer drain)",
          "[core][render][cancel_callback]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, /*workers=*/1, /*cache=*/nullptr);

    std::atomic<int> callbacks{0};
    std::atomic<int> nulls{0};
    fz_context* drop_ctx = doc.clone_context();
    REQUIRE(drop_ctx);

    // Submit at priority 1 (low) so we can cancel them all via
    // cancel_all_below_priority(0), which cancels every entry whose priority
    // value is strictly greater than 0 (lower value = more urgent here).
    for (int i = 0; i < 5; ++i) {
        litepdf::core::RenderEngine::RenderRequest req;
        req.page_num    = 0;
        req.priority    = 1;
        req.scale       = 0.15f;
        req.on_complete = [&](fz_pixmap* pix, fz_context* ctx) {
            callbacks.fetch_add(1, std::memory_order_release);
            if (!pix) {
                nulls.fetch_add(1, std::memory_order_release);
            } else {
                fz_drop_pixmap(ctx, pix);
            }
        };
        engine.submit(std::move(req));
    }
    // Cancel everything in the queue (priority > 0).
    engine.cancel_all_below_priority(/*priority=*/0);

    // Wait for the worker to drain. With D17, every request — completed or
    // cancelled — must produce exactly one on_complete call.
    for (int i = 0; i < 1000 && callbacks.load() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(callbacks.load() == 5);
    // We can't pin the cancelled-vs-completed count exactly (race window),
    // but at least one should have been cancelled with pix=nullptr in a
    // 5-deep queue with 1 worker.
    REQUIRE(nulls.load() >= 1);

    fz_drop_context(drop_ctx);
}
