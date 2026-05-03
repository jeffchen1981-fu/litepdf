// Phase 8 Task 3 — RenderEngine + invert axis integration.
//
// Verifies:
//   1. Two render requests for the same (page, scale) at OPPOSITE invert
//      polarity produce distinct cached pixmaps. Proves the L1 third
//      axis (D8) keeps both polarities live so toggling Ctrl+Shift+I
//      neither evicts nor confuses the cache.
//   2. Resubmitting at the SAME polarity returns the SAME pointer, i.e.
//      L1 hit on the second submit. Mirrors test_render_engine_cache's
//      identity-check pattern but with the new third-axis key.

#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    void fz_drop_context(fz_context*);
}

namespace {
fz_pixmap* render_blocking(litepdf::core::RenderEngine& engine, bool invert) {
    std::mutex m;
    std::condition_variable cv;
    fz_pixmap* out = nullptr;
    bool done = false;
    litepdf::core::RenderEngine::RenderRequest req;
    req.page_num = 0;
    req.priority = 0;
    req.scale    = 1.0f;
    req.invert   = invert;
    req.on_complete = [&](fz_pixmap* p, fz_context*) {
        std::lock_guard g(m);
        out  = p;
        done = true;
        cv.notify_all();
    };
    engine.submit(std::move(req));
    std::unique_lock lk(m);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&]{ return done; }));
    return out;
}
}  // namespace

TEST_CASE("RenderEngine: invert produces a distinct L1 entry from non-invert",
          "[core][render][invert]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 4, /*l2*/ 4, cache_ctx);
        litepdf::core::RenderEngine engine(doc, /*workers*/ 1, &cache);

        fz_pixmap* p_normal = render_blocking(engine, /*invert=*/false);
        fz_pixmap* p_invert = render_blocking(engine, /*invert=*/true);
        REQUIRE(p_normal != nullptr);
        REQUIRE(p_invert != nullptr);
        REQUIRE(p_normal != p_invert);   // distinct objects per D8
        REQUIRE(cache.l1_size() == 2);   // both polarities live in L1

        fz_drop_pixmap(cache_ctx, p_normal);
        fz_drop_pixmap(cache_ctx, p_invert);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("RenderEngine: same-polarity resubmit hits L1 (cache identity)",
          "[core][render][invert][cache]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 4, /*l2*/ 4, cache_ctx);
        litepdf::core::RenderEngine engine(doc, /*workers*/ 1, &cache);

        fz_pixmap* first  = render_blocking(engine, /*invert=*/true);
        fz_pixmap* second = render_blocking(engine, /*invert=*/true);
        REQUIRE(first  != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(first == second);        // identical pointer -> L1 cache hit

        fz_drop_pixmap(cache_ctx, first);
        fz_drop_pixmap(cache_ctx, second);
    }
    fz_drop_context(cache_ctx);
}
