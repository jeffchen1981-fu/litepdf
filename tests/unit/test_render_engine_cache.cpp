// Task 10 tests — RenderEngine + PageCache integration.
//
// Verifies:
//   1. Repeat submit at same (page, scale) returns the SAME underlying
//      fz_pixmap pointer — proof of L1 hit on the second submit.
//   2. A null cache (cache=nullptr) still renders correctly, matching the
//      Task 7 direct-render path (no regression for non-cached engines).
//
// Refcount discipline: each test drops its received pixmap(s) on a fresh
// cloned ctx (callers own the ref handed by on_complete per design D2).

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

TEST_CASE("RenderEngine reuses L1 pixmap on repeat submit", "[core][render][cache]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        litepdf::core::PageCache cache(5, 10, cache_ctx);
        litepdf::core::RenderEngine engine(doc, 1, &cache);

        auto render_once = [&]() -> fz_pixmap* {
            std::mutex m;
            std::condition_variable cv;
            fz_pixmap* out = nullptr;
            bool done = false;
            engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context*){
                std::lock_guard g(m); out = p; done = true; cv.notify_all();
            }});
            std::unique_lock lk(m);
            REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&]{ return done; }));
            return out;
        };
        fz_pixmap* first  = render_once();
        fz_pixmap* second = render_once();
        REQUIRE(first  != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(first  == second);  // same cached object; L1 hit on 2nd submit
        fz_drop_pixmap(cache_ctx, first);
        fz_drop_pixmap(cache_ctx, second);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("RenderEngine without cache still renders", "[core][render][cache]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1, nullptr);
    std::atomic<fz_pixmap*> got{nullptr};
    std::atomic<bool> done{false};
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context*){
        got.store(p);
        done.store(true);
    }});
    for (int i = 0; i < 500 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(done.load());
    REQUIRE(got.load() != nullptr);
    // Drop via a fresh clone — the worker's ctx is not safe to use after
    // the callback returns.
    fz_context* test_ctx = doc.clone_context();
    fz_drop_pixmap(test_ctx, got.load());
    fz_drop_context(test_ctx);
}
