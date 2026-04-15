#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Task 7 post-review: callbacks that hit the callback path must drop the
// pixmap. Forward-decl only the ABI we need — MuPDF headers are PRIVATE
// on litepdf_core.
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
}

TEST_CASE("RenderEngine::cancel before worker picks up skips callback", "[core][render][cancel]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);

    // Gate the single worker with a slow P0 so the next submission waits in queue.
    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool gate_entered = false;
    engine.submit({99, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        {
            std::lock_guard<std::mutex> g(gate_m);
            gate_entered = true;
            gate_cv.notify_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (p) fz_drop_pixmap(ctx, p);
    }});
    {
        std::unique_lock<std::mutex> lk(gate_m);
        gate_cv.wait(lk, [&]{ return gate_entered; });
    }

    std::atomic<int> calls{0};
    auto tok = engine.submit({1, 2, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    engine.cancel(tok);

    // Wait for gate to drain + cancelation to process
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    REQUIRE(calls.load() == 0);
}

TEST_CASE("RenderEngine::cancel_all_below_priority cancels less-urgent queued tasks",
          "[core][render][cancel]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);

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
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (p) fz_drop_pixmap(ctx, p);
    }});
    {
        std::unique_lock<std::mutex> lk(gate_m);
        gate_cv.wait(lk, [&]{ return gate_entered; });
    }

    std::atomic<int> p1_calls{0};
    std::atomic<int> p2_calls{0};
    std::atomic<int> p3_calls{0};
    engine.submit({1, 1, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++p1_calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    engine.submit({2, 2, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++p2_calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    engine.submit({3, 3, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++p3_calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});

    // Cancel everything strictly lower priority than 1 — that is, priority > 1
    // (P2 and P3 get canceled; P1 survives).
    engine.cancel_all_below_priority(1);

    // Drain
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(p1_calls.load() == 1);
    REQUIRE(p2_calls.load() == 0);
    REQUIRE(p3_calls.load() == 0);
}

TEST_CASE("RenderEngine::cancel on empty token is a no-op", "[core][render][cancel]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    litepdf::core::RenderEngine::RenderToken empty_token;  // default-constructed; canceled is null
    // Should not crash
    engine.cancel(empty_token);
    SUCCEED();
}
