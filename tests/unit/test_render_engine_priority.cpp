#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Task 7 post-review: drop the pixmap each callback receives. Forward-decl
// the MuPDF C ABI we need — unit tests don't see MuPDF headers (PRIVATE
// on litepdf_core).
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

TEST_CASE("RenderEngine runs higher-priority tasks before lower", "[core][render][priority]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);  // single worker forces serialization

    std::vector<int> order;
    std::mutex m;

    auto push = [&](int tag, int prio) {
        engine.submit({tag, prio, 1.0f, [&, tag](fz_pixmap* p, fz_context* ctx){
            { std::lock_guard<std::mutex> g(m); order.push_back(tag); }
            if (p) fz_drop_pixmap(ctx, p);
        }});
    };

    // Gate: hold the worker with a slow P0 so we can stack priorities.
    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool gate_entered = false;
    engine.submit({99, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        {
            std::lock_guard<std::mutex> g(gate_m);
            gate_entered = true;
            gate_cv.notify_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        { std::lock_guard<std::mutex> g(m); order.push_back(99); }
        if (p) fz_drop_pixmap(ctx, p);
    }});
    {
        std::unique_lock<std::mutex> lk(gate_m);
        gate_cv.wait(lk, [&]{ return gate_entered; });
    }

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
    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool gate_entered = false;
    engine.submit({99, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        {
            std::lock_guard<std::mutex> g(gate_m);
            gate_entered = true;
            gate_cv.notify_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        { std::lock_guard<std::mutex> g(m); order.push_back(99); }
        if (p) fz_drop_pixmap(ctx, p);
    }});
    {
        std::unique_lock<std::mutex> lk(gate_m);
        gate_cv.wait(lk, [&]{ return gate_entered; });
    }

    // 4 tasks all at P1, submitted A,B,C,D — expect FIFO
    for (int tag : {10, 20, 30, 40}) {
        engine.submit({tag, 1, 1.0f, [&, tag](fz_pixmap* p, fz_context* ctx){
            { std::lock_guard<std::mutex> g(m); order.push_back(tag); }
            if (p) fz_drop_pixmap(ctx, p);
        }});
    }

    for (int i = 0; i < 500; ++i) {
        { std::lock_guard<std::mutex> g(m); if (order.size() >= 5) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard<std::mutex> g(m);
    REQUIRE(order == std::vector<int>{99, 10, 20, 30, 40});
}
