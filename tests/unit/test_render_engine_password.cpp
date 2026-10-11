#include <catch2/catch_test_macros.hpp>
#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/EscrowContext.hpp"
#include "core/RenderEngine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <vector>

// #126: each RenderEngine worker opens its own fz_document. MuPDF tries a blank
// password on open, and the failed attempt leaves a wrong key in the document
// (one computed while checking ""), so a worker that is never authenticated
// decrypts every content
// stream to garbage and renders an empty page. encrypted-text.pdf has a
// non-blank user password ("test") and a page of large black text, so a
// wrong-key render is distinguishable from a right one (encrypted.pdf's page
// is empty and is not).
//
// The unit-tests target does not see MuPDF's headers; forward-declare the few
// C-ABI entry points the assertions need (see test_render_engine_output.cpp).
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_context(fz_context*);
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    int  fz_pixmap_width(fz_context*, fz_pixmap*);
    int  fz_pixmap_height(fz_context*, fz_pixmap*);
    int  fz_pixmap_stride(fz_context*, fz_pixmap*);
    unsigned char* fz_pixmap_samples(fz_context*, fz_pixmap*);
}

namespace {

constexpr const char* kFixture = "tests/fixtures/encrypted-text.pdf";

// Eight lines of 48 pt bold text at 72 dpi ink tens of thousands of pixels; a
// wrong-key render inks none.
constexpr std::size_t kMinInk = 10000;

void open_and_authenticate(litepdf::core::Document& doc) {
    const auto err = doc.open(kFixture);
    REQUIRE(err.has_value());
    REQUIRE(*err == litepdf::core::Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));
    REQUIRE(doc.is_open());
}

// Pixels whose B, G and R are all below 128: glyph ink on the white page.
std::size_t count_dark_pixels(fz_context* ctx, fz_pixmap* pix) {
    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    const unsigned char* s = fz_pixmap_samples(ctx, pix);
    std::size_t dark = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = s + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            if (px[0] < 128 && px[1] < 128 && px[2] < 128) ++dark;
        }
    }
    return dark;
}

// Submits page 0 `requests` times and returns each render's dark-pixel count.
// More requests than workers, so with two workers the page is rendered by
// both worker documents in the usual case.
std::vector<std::size_t> render_page0_ink(litepdf::core::Document& doc,
                                          litepdf::core::RenderEngine& engine,
                                          std::size_t requests) {
    std::mutex m;
    std::condition_variable cv;
    std::vector<fz_pixmap*> got;
    for (std::size_t i = 0; i < requests; ++i) {
        engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context*) {
            std::lock_guard<std::mutex> g(m);
            got.push_back(p);
            cv.notify_all();
        }});
    }
    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(10),
                            [&] { return got.size() == requests; }));
    }

    fz_context* test_ctx = doc.clone_context();
    REQUIRE(test_ctx != nullptr);
    std::vector<std::size_t> ink;
    for (fz_pixmap* p : got) {
        REQUIRE(p != nullptr);
        ink.push_back(count_dark_pixels(test_ctx, p));
        fz_drop_pixmap(test_ctx, p);
    }
    fz_drop_context(test_ctx);
    return ink;
}

}  // namespace

TEST_CASE("RenderEngine renders a user-password PDF when given its password (#126)",
          "[core][render][password]") {
    litepdf::core::Document doc;
    open_and_authenticate(doc);
    litepdf::core::RenderEngine engine(doc, 2, nullptr, "test");

    for (const std::size_t dark : render_page0_ink(doc, engine, 4)) {
        INFO("dark pixels = " << dark);
        CHECK(dark > kMinInk);
    }
}

TEST_CASE("RenderEngine without the password renders a user-password PDF blank",
          "[core][render][password]") {
    // Negative control: proves the fixture tells a right key from a wrong one,
    // which is what makes the test above discriminating. It pins MuPDF
    // behaviour (the failed blank-password attempt at open leaves a wrong key
    // behind). If a MuPDF upgrade makes this render text,
    // the engine no longer needs the password to render, and the test above
    // no longer proves #126 stays fixed.
    litepdf::core::Document doc;
    open_and_authenticate(doc);
    litepdf::core::RenderEngine engine(doc, 1);

    for (const std::size_t dark : render_page0_ink(doc, engine, 1)) {
        INFO("dark pixels = " << dark);
        CHECK(dark == 0);
    }
}

TEST_CASE("RenderEngine throws when a worker rejects the password",
          "[core][render][password]") {
    // The rejection throws after every worker context is cloned and the
    // rejecting worker's document is adopted, so the rollback must drop both.
    // The escrow is the oracle (see the #74 test in test_document_view.cpp):
    // it keeps the context family countable, and root + escrow == 2 once the
    // two worker clones are gone.
    litepdf::core::Document doc;
    open_and_authenticate(doc);
    fz_context* clone = doc.clone_context();
    REQUIRE(clone != nullptr);
    auto probe = litepdf::core::EscrowContext::clone_from(clone);
    fz_drop_context(clone);
    REQUIRE(probe.valid());
    REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == 2);

    REQUIRE_THROWS_AS(litepdf::core::RenderEngine(doc, 2, nullptr, "wrong"),
                      std::runtime_error);

    REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == 2);
}

TEST_CASE("DocumentView passes the password on to its render workers (#126)",
          "[core][view][render][password]") {
    // The app reaches RenderEngine only through DocumentView, so the tests
    // above would stay green if DocumentView dropped the password. Ink is
    // counted inside the callback, with the worker context it hands over.
    litepdf::app::InlineDispatcher disp;
    litepdf::core::Document doc;
    open_and_authenticate(doc);
    litepdf::core::DocumentView view(std::move(doc), disp, "test");
    view.set_zoom_pct(1.0f);

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::size_t dark = 0;
    view.request_render(0, [&](fz_pixmap* p, fz_context* ctx) {
        const std::size_t ink = p ? count_dark_pixels(ctx, p) : 0;
        if (p) fz_drop_pixmap(ctx, p);
        std::lock_guard<std::mutex> g(m);
        dark = ink;
        done = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; }));
    }
    INFO("dark pixels = " << dark);
    CHECK(dark > kMinInk);
}
