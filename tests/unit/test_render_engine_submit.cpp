#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("RenderEngine::submit runs callback once with worker ctx", "[core][render][submit]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    std::atomic<int> calls{0};
    std::atomic<fz_context*> seen_ctx{nullptr};
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        seen_ctx.store(ctx);
        (void)p;
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
    auto t1 = engine.submit({0, 0, 1.0f, [](fz_pixmap*, fz_context*){}});
    auto t2 = engine.submit({1, 0, 1.0f, [](fz_pixmap*, fz_context*){}});
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
    engine.submit({0, 0, 1.0f, [&](fz_pixmap*, fz_context*){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++calls;
    }});
    // Stack up additional work while the worker is blocked
    engine.submit({1, 0, 1.0f, [&](fz_pixmap*, fz_context*){ ++calls; }});
    engine.submit({2, 0, 1.0f, [&](fz_pixmap*, fz_context*){ ++calls; }});
    // Give the gate a moment to be picked up, then check queue backlog
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(engine.pending_count() >= 1);  // gate is in flight; 2 others queued
    // Drain
    for (int i = 0; i < 500 && calls.load() < 3; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(calls.load() == 3);
}
