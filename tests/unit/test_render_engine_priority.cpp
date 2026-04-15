#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

TEST_CASE("RenderEngine runs higher-priority tasks before lower", "[core][render][priority]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);  // single worker forces serialization

    std::vector<int> order;
    std::mutex m;

    auto push = [&](int tag, int prio) {
        engine.submit({tag, prio, 1.0f, [&, tag](fz_pixmap*, fz_context*){
            std::lock_guard<std::mutex> g(m); order.push_back(tag);
        }});
    };

    // Gate: hold the worker with a slow P0 so we can stack priorities.
    engine.submit({99, 0, 1.0f, [&](fz_pixmap*, fz_context*){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> g(m); order.push_back(99);
    }});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Now stack P2, P0, P1 — expect P0 first, then P1, then P2.
    push(2, 2);
    push(0, 0);
    push(1, 1);

    // Drain
    for (int i = 0; i < 500; ++i) {
        { std::lock_guard<std::mutex> g(m); if (order.size() >= 4) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard<std::mutex> g(m);
    REQUIRE(order == std::vector<int>{99, 0, 1, 2});
}

TEST_CASE("RenderEngine runs same-priority tasks in FIFO order", "[core][render][priority]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);

    std::vector<int> order;
    std::mutex m;

    // Gate
    engine.submit({99, 0, 1.0f, [&](fz_pixmap*, fz_context*){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> g(m); order.push_back(99);
    }});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // 4 tasks all at P1, submitted A,B,C,D — expect FIFO
    for (int tag : {10, 20, 30, 40}) {
        engine.submit({tag, 1, 1.0f, [&, tag](fz_pixmap*, fz_context*){
            std::lock_guard<std::mutex> g(m); order.push_back(tag);
        }});
    }

    for (int i = 0; i < 500; ++i) {
        { std::lock_guard<std::mutex> g(m); if (order.size() >= 5) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard<std::mutex> g(m);
    REQUIRE(order == std::vector<int>{99, 10, 20, 30, 40});
}
