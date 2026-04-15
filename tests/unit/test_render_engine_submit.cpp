#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <thread>

// Task 7 post-review: RenderEngine now delivers real fz_pixmap. Older tests
// ignored the pixmap (nullptr predecessor stub), which now leaks. Drop
// the pixmap in each callback. Forward-decl only the ABI we need — unit
// tests don't see MuPDF headers (PRIVATE on litepdf_core).
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

TEST_CASE("RenderEngine::submit runs callback once with worker ctx", "[core][render][submit]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    std::atomic<int> calls{0};
    std::atomic<fz_context*> seen_ctx{nullptr};
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        seen_ctx.store(ctx);
        if (p) fz_drop_pixmap(ctx, p);
    }});
    for (int i = 0; i < 200 && calls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(calls.load() == 1);
    REQUIRE(seen_ctx.load() != nullptr);
}

TEST_CASE("RenderEngine::submit returns a token with unique id and canceled flag", "[core][render][submit]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    auto t1 = engine.submit({0, 0, 1.0f, [](fz_pixmap* p, fz_context* ctx){
        if (p) fz_drop_pixmap(ctx, p);
    }});
    auto t2 = engine.submit({1, 0, 1.0f, [](fz_pixmap* p, fz_context* ctx){
        if (p) fz_drop_pixmap(ctx, p);
    }});
    REQUIRE(t1.id != t2.id);
    REQUIRE(t1.canceled);
    REQUIRE(t2.canceled);
    REQUIRE(!t1.canceled->load());
}

TEST_CASE("RenderEngine::pending_count reflects queue size (briefly)", "[core][render][submit]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    std::atomic<int> calls{0};
    // Gate the single worker with a slow callback
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    // Stack up additional work while the worker is blocked
    engine.submit({1, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    engine.submit({2, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    // Give the gate a moment to be picked up, then check queue backlog
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(engine.pending_count() >= 1);  // gate is in flight; 2 others queued
    // Drain
    for (int i = 0; i < 500 && calls.load() < 3; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(calls.load() == 3);
}
