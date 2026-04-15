#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Task 7: RenderEngine now produces real fz_pixmap output. These tests
// verify non-null delivery for a valid page and null delivery on failure
// (e.g. out-of-range page).
//
// The unit-tests target does not link against MuPDF's public headers
// (litepdf_core links them PRIVATE). We only need a handful of C-ABI
// entry points here — fz_drop_context, fz_drop_pixmap, and the pixmap
// width/height accessors — so we forward-declare them extern "C".
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_context(fz_context*);
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    int  fz_pixmap_width(fz_context*, fz_pixmap*);
    int  fz_pixmap_height(fz_context*, fz_pixmap*);
}

TEST_CASE("RenderEngine renders page 0 to a non-null pixmap with sane dimensions",
          "[core][render][output]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);

    std::mutex m;
    std::condition_variable cv;
    fz_pixmap* got = nullptr;
    bool done = false;

    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* /*ctx*/){
        // We extend the pixmap's lifetime past the worker callback for
        // assertions. fz_pixmap's refcount is atomic-under-FZ_LOCK_ALLOC,
        // so reading and dropping from a different ctx clone is safe.
        std::lock_guard<std::mutex> g(m);
        got = p;
        done = true;
        cv.notify_all();
    }});

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&]{ return done; }));
    }
    REQUIRE(got != nullptr);

    // Read width/height + drop using a fresh clone of the source ctx.
    // MuPDF's refcount ops are atomic over FZ_LOCK_ALLOC, so using a
    // clone for drop is valid.
    fz_context* test_ctx = doc.clone_context();
    REQUIRE(test_ctx != nullptr);
    const int w = fz_pixmap_width(test_ctx, got);
    const int h = fz_pixmap_height(test_ctx, got);
    REQUIRE(w > 0);
    REQUIRE(h > 0);

    fz_drop_pixmap(test_ctx, got);
    fz_drop_context(test_ctx);
}

TEST_CASE("RenderEngine returns nullptr pixmap for out-of-range page",
          "[core][render][output]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    const int pages = static_cast<int>(doc.page_count());
    REQUIRE(pages > 0);

    litepdf::core::RenderEngine engine(doc, 1);

    std::atomic<bool>       done{false};
    std::atomic<fz_pixmap*> got{nullptr};

    engine.submit({pages + 100, 0, 1.0f, [&](fz_pixmap* p, fz_context*){
        got.store(p);
        done.store(true);
    }});

    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(done.load());
    // fz_load_page throws on out-of-range; fz_catch swallows, pix stays null.
    REQUIRE(got.load() == nullptr);
}
