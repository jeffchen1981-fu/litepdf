// (#55) RenderEngine::renders_started(): the count of requests that passed
// checkpoint A and missed L1, i.e. the ones that rasterise. The strip
// scheduler's cancellation gate reads it. Test observability only.
#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

// MuPDF headers are PRIVATE on litepdf_core; forward-declare the ABI we need.
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

namespace {

constexpr const char* kFixture = "tests/fixtures/selection.pdf";  // 8 pages

// Renders `page` at scale 1 and waits for the callback. Returns whether the
// pixmap was non-null.
bool render_and_wait(RenderEngine& engine, int page) {
    auto done = std::make_shared<std::promise<bool>>();
    auto fut = done->get_future();
    RenderEngine::RenderRequest req;
    req.page_num = page;
    req.priority = 0;
    req.scale = 1.0f;
    req.on_complete = [done](fz_pixmap* p, fz_context* ctx) {
        const bool ok = p != nullptr;
        if (p) fz_drop_pixmap(ctx, p);
        done->set_value(ok);
    };
    engine.submit(std::move(req));
    REQUIRE(fut.wait_for(10s) == std::future_status::ready);
    return fut.get();
}

}  // namespace

TEST_CASE("RenderEngine: renders_started counts L1 misses only",
          "[core][render][counter]") {
    Document doc;
    REQUIRE(!doc.open(kFixture).has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine engine(doc, /*workers=*/1, &cache);
        REQUIRE(engine.renders_started() == 0);

        REQUIRE(render_and_wait(engine, 0));
        REQUIRE(engine.renders_started() == 1);

        // Same page, same scale: served from L1, not counted.
        REQUIRE(render_and_wait(engine, 0));
        REQUIRE(engine.renders_started() == 1);

        REQUIRE(render_and_wait(engine, 1));
        REQUIRE(engine.renders_started() == 2);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("RenderEngine: renders_started skips cancelled and bypass requests",
          "[core][render][counter]") {
    Document doc;
    REQUIRE(!doc.open(kFixture).has_value());
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    {
        PageCache cache(/*l1=*/4, /*l2=*/4, cache_ctx);
        RenderEngine engine(doc, /*workers=*/1, &cache);

        // Hold the single worker inside page 0's callback. The render itself
        // has already been counted by then.
        std::mutex m;
        std::condition_variable cv;
        bool entered = false;
        std::promise<void> release;
        std::shared_future<void> release_fut = release.get_future().share();
        // Declared after the engine: unblocks the worker before ~RenderEngine
        // joins it, even if a REQUIRE below unwinds.
        struct ReleaseGuard {
            std::promise<void>& p;
            bool released = false;
            void go() { if (!released) { released = true; p.set_value(); } }
            ~ReleaseGuard() { go(); }
        } guard{release};

        RenderEngine::RenderRequest hold;
        hold.page_num = 0;
        hold.priority = 0;
        hold.scale = 1.0f;
        hold.on_complete = [&](fz_pixmap* p, fz_context* ctx) {
            {
                std::lock_guard<std::mutex> g(m);
                entered = true;
            }
            cv.notify_all();
            release_fut.wait();
            if (p) fz_drop_pixmap(ctx, p);
        };
        engine.submit(std::move(hold));
        {
            std::unique_lock<std::mutex> lk(m);
            REQUIRE(cv.wait_for(lk, 10s, [&] { return entered; }));
        }
        REQUIRE(engine.renders_started() == 1);

        std::atomic<int> callbacks{0};
        std::promise<void> all_done;
        auto all_done_fut = all_done.get_future();
        auto on_done = [&](fz_pixmap* p, fz_context* ctx) {
            if (p) fz_drop_pixmap(ctx, p);
            if (callbacks.fetch_add(1) + 1 == 2) all_done.set_value();
        };

        // Page 2: cancelled before the worker can take it.
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        RenderEngine::RenderRequest cancel_req;
        cancel_req.page_num = 2;
        cancel_req.priority = 0;
        cancel_req.scale = 1.0f;
        cancel_req.on_complete = on_done;
        cancel_req.cancel_flag = cancelled;
        engine.submit(std::move(cancel_req));
        cancelled->store(true);

        // Page 3: a thumbnail-style request that bypasses the caches.
        RenderEngine::RenderRequest bypass_req;
        bypass_req.page_num = 3;
        bypass_req.priority = 3;
        bypass_req.scale = 0.15f;
        bypass_req.on_complete = on_done;
        bypass_req.bypass_cache = true;
        engine.submit(std::move(bypass_req));

        guard.go();
        REQUIRE(all_done_fut.wait_for(10s) == std::future_status::ready);
        REQUIRE(engine.renders_started() == 1);
    }
    fz_drop_context(cache_ctx);
}
