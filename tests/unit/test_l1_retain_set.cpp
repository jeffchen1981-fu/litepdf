// #159 for the continuous strip (#55 PR 3, Task 2): a background tab keeps a SET
// of pages in L1, not one page or a spread.
//
// Overload trap: a braced list of zero or one int ({} , {page}) converts to int
// by identity and binds the int overload of retain_l1_pages /
// trim_l1_for_background. Every call below constructs std::vector<int>
// explicitly.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/PageCache.hpp"
#include "ui/PdfCanvasLayout.hpp"

// The unit-tests target does not get MuPDF's include path, so the pixmap bits
// are declared by hand, as in test_page_cache_l1.cpp.
extern "C" {
struct fz_context;
struct fz_pixmap;
struct fz_colorspace;
fz_pixmap* fz_new_pixmap(fz_context*, fz_colorspace*, int w, int h, void* seps, int alpha);
fz_colorspace* fz_device_rgb(fz_context*);
void fz_drop_pixmap(fz_context*, fz_pixmap*);
void fz_drop_context(fz_context*);
}

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::core::PageCache;
using litepdf::ui::ShownPages;
using litepdf::ui::background_retain_pages;

namespace {

fz_pixmap* rgb_pixmap(fz_context* ctx, int w, int h) {
    return fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, nullptr, 0);
}

bool l1_hit(PageCache& cache, fz_context* ctx, int page, float scale) {
    fz_pixmap* p = cache.get_pixmap(page, scale);
    if (!p) return false;
    fz_drop_pixmap(ctx, p);
    return true;
}

void render_and_wait(DocumentView& view, int page) {
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    view.request_render(page, [done](fz_pixmap* p, fz_context* ctx) {
        if (p) fz_drop_pixmap(ctx, p);
        done->set_value();
    });
    REQUIRE(fut.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
}

}  // namespace

TEST_CASE("PageCache: retain_l1_pages keeps a set of pages and pins L1 to it",
          "[core][cache][l1][retain]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        PageCache cache(/*l1*/ 8, /*l2*/ 0, ctx);
        for (int p = 0; p < 5; ++p) {
            cache.put_pixmap(p, 1.0f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        }
        REQUIRE(cache.l1_size() == 5);

        cache.retain_l1_pages(1.0f, /*invert=*/false, std::vector<int>{1, 3, 4});
        REQUIRE(cache.l1_size() == 3);
        REQUIRE(cache.l1_bytes() == 900);
        REQUIRE(l1_hit(cache, ctx, 1, 1.0f));
        REQUIRE(l1_hit(cache, ctx, 3, 1.0f));
        REQUIRE(l1_hit(cache, ctx, 4, 1.0f));
        REQUIRE_FALSE(l1_hit(cache, ctx, 0, 1.0f));
        REQUIRE_FALSE(l1_hit(cache, ctx, 2, 1.0f));

        // Pinned: a page outside the set and a kept page at another scale are
        // dropped, not admitted.
        cache.put_pixmap(2, 1.0f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        cache.put_pixmap(3, 1.5f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        REQUIRE(cache.l1_size() == 3);
        REQUIRE_FALSE(l1_hit(cache, ctx, 2, 1.0f));
        REQUIRE_FALSE(l1_hit(cache, ctx, 3, 1.5f));

        cache.release_l1_pin();
        cache.put_pixmap(2, 1.0f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        REQUIRE(cache.l1_size() == 4);
        REQUIRE(l1_hit(cache, ctx, 2, 1.0f));
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache: an empty retain set empties L1", "[core][cache][l1][retain]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        PageCache cache(/*l1*/ 8, /*l2*/ 0, ctx);
        for (int p = 0; p < 3; ++p) {
            cache.put_pixmap(p, 1.0f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        }
        cache.retain_l1_pages(1.0f, /*invert=*/false, std::vector<int>{});
        REQUIRE(cache.l1_size() == 0);
        REQUIRE(cache.l1_bytes() == 0);

        // Nothing is admitted while the empty pin holds, page 0 included.
        cache.put_pixmap(0, 1.0f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        REQUIRE(cache.l1_size() == 0);

        cache.release_l1_pin();
        cache.put_pixmap(0, 1.0f, /*invert=*/false, rgb_pixmap(ctx, 10, 10));
        REQUIRE(cache.l1_size() == 1);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PdfCanvasLayout: background_retain_pages picks the strip set or the shown pages",
          "[ui][layout][retain]") {
    using V = std::vector<int>;
    // A strip tab keeps its visible pages.
    REQUIRE(background_retain_pages(true, V{9, 8, 7}, ShownPages{9, -1}) == V{9, 8, 7});
    // An empty strip set (a minimized window) falls back to the current page.
    REQUIRE(background_retain_pages(true, V{}, ShownPages{4, -1}) == V{4});
    // A paged tab keeps what it shows, whatever the strip set says.
    REQUIRE(background_retain_pages(false, V{9, 8}, ShownPages{3, 4}) == V{3, 4});
    REQUIRE(background_retain_pages(false, V{}, ShownPages{4, -1}) == V{4});
}

TEST_CASE("DocumentView: trim_l1_for_background keeps every page of the set",
          "[core][view][trim]") {
    InlineDispatcher disp;
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/large.pdf").has_value());
    REQUIRE(doc.page_count() >= 6);
    auto view = std::make_unique<DocumentView>(std::move(doc), disp, std::nullopt,
                                               /*workers*/ 1, /*l1*/ 5, /*l2*/ 10,
                                               litepdf::core::kDefaultL1BudgetBytes);
    view->set_viewport(800.0f, 600.0f, 96.0f);
    view->set_zoom_pct(1.0f);
    for (int p = 0; p < 5; ++p) render_and_wait(*view, p);
    REQUIRE(view->l1_entry_count() == 5);

    view->trim_l1_for_background(std::vector<int>{0, 2, 3});
    REQUIRE(view->l1_entry_count() == 3);
    REQUIRE(view->l1_contains(0));
    REQUIRE(view->l1_contains(2));
    REQUIRE(view->l1_contains(3));
    REQUIRE_FALSE(view->l1_contains(1));
    REQUIRE_FALSE(view->l1_contains(4));

    // The next render request releases the pin: page 4 is admitted again.
    render_and_wait(*view, 4);
    REQUIRE(view->l1_contains(4));
}
