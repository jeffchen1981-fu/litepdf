#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
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
    int  fz_pixmap_components(fz_context*, fz_pixmap*);
    int  fz_pixmap_colorants(fz_context*, fz_pixmap*);
    int  fz_pixmap_alpha(fz_context*, fz_pixmap*);
    int  fz_pixmap_stride(fz_context*, fz_pixmap*);
    unsigned char* fz_pixmap_samples(fz_context*, fz_pixmap*);
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

    // Task 0.3 (Phase 3): rasterize uses fz_device_bgr + alpha=1, producing
    // a BGRA buffer that Direct2D can upload byte-for-byte into a
    // DXGI_FORMAT_B8G8R8A8_UNORM bitmap (no channel swap on the UI thread).
    //
    // MuPDF accounting:
    //   fz_pixmap_components == pix->n                     (colorants + spots + alpha)
    //   fz_pixmap_colorants  == pix->n - pix->alpha - pix->s (color-only)
    //   fz_pixmap_alpha      == 1 for alpha=1
    // For BGR+alpha=1 with no spots: n=4, colorants=3, alpha=1.
    const int components = fz_pixmap_components(test_ctx, got);
    REQUIRE(components == 4);          // B, G, R, A
    const int colorants = fz_pixmap_colorants(test_ctx, got);
    REQUIRE(colorants == 3);           // BGR color channels
    const int has_alpha = fz_pixmap_alpha(test_ctx, got);
    REQUIRE(has_alpha == 1);
    const int stride = fz_pixmap_stride(test_ctx, got);
    REQUIRE(stride >= w * 4);          // 4 bytes/pixel minimum

    fz_drop_pixmap(test_ctx, got);
    fz_drop_context(test_ctx);
}

TEST_CASE("RenderEngine renders an opaque WHITE page background",
          "[core][render][output]") {
    // Regression for the black-background bug: simple.pdf does NOT paint its
    // own page background. The render must clear the BGRA pixmap to opaque
    // white before running the page; otherwise it stays transparent and the
    // canvas's D2D bitmap (D2D1_ALPHA_MODE_IGNORE) paints it as opaque BLACK.
    // Empirically the bug gave a mean BGR brightness ~55; the fix gives ~233.
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);

    std::mutex m;
    std::condition_variable cv;
    fz_pixmap* got = nullptr;
    bool done = false;
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context*){
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

    fz_context* test_ctx = doc.clone_context();
    REQUIRE(test_ctx != nullptr);
    const int w = fz_pixmap_width(test_ctx, got);
    const int h = fz_pixmap_height(test_ctx, got);
    const int stride = fz_pixmap_stride(test_ctx, got);
    const unsigned char* s = fz_pixmap_samples(test_ctx, got);
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(s != nullptr);

    // Mean of the B,G,R channels (skip alpha) across the page. A predominantly
    // white page is well above mid-grey; the transparent/black-bg regression
    // collapses toward ~55.
    unsigned long long sum = 0, count = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = s + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            sum += px[0]; sum += px[1]; sum += px[2];
            count += 3;
        }
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(count);
    INFO("mean BGR brightness = " << mean);
    REQUIRE(mean > 150.0);

    fz_drop_pixmap(test_ctx, got);
    fz_drop_context(test_ctx);
}

// Regression for issue #32 ("CBZ renders no pixmap / blank-black page").
// The root cause was a degenerate fixture: the shipped sample.cbz held two
// 1x1 black-pixel PNGs, so the page was 0.8x0.8 pt and rendered to a 1x1
// pixmap that, scaled to fill the canvas, looked like a blank black page.
// The CBZ document handler and the PNG + JPEG image codecs all survive the
// MuPDF binary-size prune (#28) and render real comics correctly. This test
// guards both: (a) the fixture stays non-degenerate, and (b) a future prune
// does not silently drop the cbz handler or an image codec. sample.cbz mixes
// one PNG page and one JPEG page on purpose so both codecs are exercised.
TEST_CASE("CBZ pages render to non-degenerate pixmaps (issue #32)",
          "[core][render][formats][cbz]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/sample.cbz").has_value());
    REQUIRE(doc.page_count() >= 2);
    litepdf::core::RenderEngine engine(doc, 1);

    // Render one page synchronously and hand the pixmap back to the caller
    // (ref extended past the worker callback, as in the cases above).
    auto render_page = [&](int page) -> fz_pixmap* {
        std::mutex m;
        std::condition_variable cv;
        fz_pixmap* got = nullptr;
        bool done = false;
        engine.submit({page, 0, 1.0f, [&](fz_pixmap* p, fz_context*){
            std::lock_guard<std::mutex> g(m);
            got = p;
            done = true;
            cv.notify_all();
        }});
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&]{ return done; }));
        return got;
    };

    fz_context* test_ctx = doc.clone_context();
    REQUIRE(test_ctx != nullptr);

    // Both pages must produce a real pixmap larger than 1x1. The degenerate
    // fixture collapsed to exactly 1x1 (and the no-pixmap failure to nullptr).
    for (int page = 0; page < 2; ++page) {
        INFO("rendering cbz page " << page);
        fz_pixmap* pm = render_page(page);
        REQUIRE(pm != nullptr);
        REQUIRE(fz_pixmap_width(test_ctx, pm)  > 1);
        REQUIRE(fz_pixmap_height(test_ctx, pm) > 1);
        fz_drop_pixmap(test_ctx, pm);
    }

    // Page 0 must not be a black void: a typical comic page is predominantly
    // light. This catches "rendered but all black" in addition to the size
    // collapse. (Mirrors the white-background regression assertion above.)
    fz_pixmap* p0 = render_page(0);
    REQUIRE(p0 != nullptr);
    const int w = fz_pixmap_width(test_ctx, p0);
    const int h = fz_pixmap_height(test_ctx, p0);
    const int stride = fz_pixmap_stride(test_ctx, p0);
    const unsigned char* s = fz_pixmap_samples(test_ctx, p0);
    REQUIRE(s != nullptr);
    unsigned long long sum = 0, count = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = s + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            sum += px[0]; sum += px[1]; sum += px[2];
            count += 3;
        }
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(count);
    INFO("mean BGR brightness = " << mean);
    REQUIRE(mean > 150.0);

    fz_drop_pixmap(test_ctx, p0);
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
