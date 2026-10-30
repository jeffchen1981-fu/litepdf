// #55 PR 1 Task 7: the core APIs the continuous-scroll strip drives --
// DocumentView's continuous flag, page_sizes(), request_page_render,
// l1_contains and cancel_thumbnail_renders, and PageCache::l1_contains.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"
#include "core/detail/PageSizeFill.hpp"
#include "ui/PdfCanvasLayout.hpp"

// Plain extern "C" declarations keep this TU MuPDF-header-free, as in
// test_document_view.cpp and test_page_cache_l1.cpp.
extern "C" {
struct fz_context;
struct fz_pixmap;
struct fz_colorspace;
fz_pixmap* fz_new_pixmap(fz_context*, fz_colorspace*, int w, int h, void* seps, int alpha);
fz_colorspace* fz_device_rgb(fz_context*);
void fz_drop_pixmap(fz_context* ctx, fz_pixmap* pix);
void fz_drop_context(fz_context* ctx);
}

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::core::PageCache;
using litepdf::core::PageSize;
using litepdf::core::RenderEngine;

// The HeldWorker / ReleaseOnExit / one_worker_view pattern of
// test_document_view.cpp, copied here (existing tests are not edited). The
// hold uses request_render, so no prefetched neighbour joins the queue.
namespace {

constexpr const char* kEightPages = "tests/fixtures/selection.pdf";

struct HeldWorker {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released;
};

// Releases the held worker on scope exit, a failed REQUIRE included, so the
// view's destructor can drain its engine. Declare it AFTER the view.
struct ReleaseOnExit {
    std::shared_ptr<HeldWorker> held;
    ~ReleaseOnExit() { release(); }
    void release() {
        try { held->release.set_value(); } catch (const std::future_error&) {}
    }
};

std::shared_ptr<HeldWorker> hold_worker(DocumentView& view, int page) {
    auto held = std::make_shared<HeldWorker>();
    held->released = held->release.get_future().share();
    auto entered = held->entered.get_future();
    view.request_render(page, [held](fz_pixmap* p, fz_context* ctx) {
        if (p) fz_drop_pixmap(ctx, p);
        held->entered.set_value();
        (void)held->released.wait_for(std::chrono::seconds(30));
    });
    REQUIRE(entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return held;
}

// One worker, render scale 1 (Custom 100% at 96 dpi), the default L1 budget.
std::unique_ptr<DocumentView> one_worker_view(InlineDispatcher& disp) {
    Document doc;
    REQUIRE_FALSE(doc.open(kEightPages).has_value());
    REQUIRE(doc.page_count() == 8);
    auto view = std::make_unique<DocumentView>(std::move(doc), disp, std::nullopt,
                                               /*workers*/ 1, /*l1*/ 5, /*l2*/ 10,
                                               litepdf::core::kDefaultL1BudgetBytes);
    view->set_viewport(800.0f, 600.0f, 96.0f);
    view->set_zoom_pct(1.0f);
    return view;
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

// What a request_page_render completion saw: whether its pixmap was non-null.
struct Outcome {
    std::promise<bool> got_pixmap;
};

DocumentView::RenderCb record(const std::shared_ptr<Outcome>& out) {
    return [out](fz_pixmap* p, fz_context* ctx) {
        const bool non_null = p != nullptr;
        if (p) fz_drop_pixmap(ctx, p);
        out->got_pixmap.set_value(non_null);
    };
}

bool wait_outcome(std::future<bool>& fut) {
    REQUIRE(fut.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    return fut.get();
}

}  // namespace

TEST_CASE("DocumentView: continuous mode does not refit on a page change",
          "[core][view][strip]") {
    InlineDispatcher disp;
    auto open_spread = [] {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/spread-unequal.pdf").has_value());
        REQUIRE(doc.page_count() == 2);
        return doc;
    };

    DocumentView a(open_spread(), disp);
    a.set_viewport(800.0f, 600.0f, 96.0f);
    REQUIRE(a.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE_FALSE(a.continuous());
    a.set_continuous(true);
    REQUIRE(a.continuous());
    const float a_before = a.zoom_pct();
    REQUIRE(a.set_current_page(1));
    REQUIRE(a.zoom_pct() == a_before);

    // The control: continuous off, the same page change refits.
    DocumentView b(open_spread(), disp);
    b.set_viewport(800.0f, 600.0f, 96.0f);
    const float b_before = b.zoom_pct();
    REQUIRE(b.set_current_page(1));
    REQUIRE(b.zoom_pct() != b_before);
}

// #171, #191: spread-unequal.pdf never shows two pages side by side (a cover and
// an odd tail), so a defect of an unequal pair needs a document that really pairs.
TEST_CASE("Spread fixture: spread-pairs.pdf pairs pages of unequal height",
          "[core][view][strip]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/spread-pairs.pdf").has_value());
    REQUIRE(doc.page_count() == 5);
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    const std::vector<PageSize>& sizes = view.page_sizes();

    REQUIRE(litepdf::ui::dual_page_compute_right(1, 5) == 2);
    CHECK(sizes[1].height_pt < sizes[2].height_pt);   // the right page is taller
    REQUIRE(litepdf::ui::dual_page_compute_right(3, 5) == 4);
    CHECK(sizes[3].height_pt > sizes[4].height_pt);   // the left page is taller
}

TEST_CASE("DocumentView: page_sizes returns one size per page", "[core][view][strip]") {
    InlineDispatcher disp;
    Document doc;
    REQUIRE_FALSE(doc.open(kEightPages).has_value());
    DocumentView view(std::move(doc), disp);

    const std::vector<PageSize>& sizes = view.page_sizes();
    REQUIRE(sizes.size() == 8);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const PageSize expected = view.document().page_size(i);
        REQUIRE(sizes[i].width_pt == expected.width_pt);
        REQUIRE(sizes[i].height_pt == expected.height_pt);
    }
    const PageSize* first_data = sizes.data();
    const std::vector<PageSize>& again = view.page_sizes();
    REQUIRE(&again == &sizes);
    REQUIRE(again.data() == first_data);
}

TEST_CASE("DocumentView: page size fallback when a size cannot be read",
          "[core][view][strip]") {
    using litepdf::core::detail::fill_page_sizes;
    const auto sized = [](std::size_t i) {
        return PageSize{100.0f + static_cast<float>(i), 200.0f + static_cast<float>(i)};
    };

    SECTION("a later page takes page 0's size") {
        std::vector<PageSize> out;
        REQUIRE_NOTHROW(out = fill_page_sizes(6, [&](std::size_t i) {
            if (i == 3) throw std::runtime_error("unreadable");
            return sized(i);
        }));
        REQUIRE(out.size() == 6);
        REQUIRE(out[3].width_pt == out[0].width_pt);
        REQUIRE(out[3].height_pt == out[0].height_pt);
        REQUIRE(out[0].width_pt == 100.0f);
        REQUIRE(out[4].width_pt == 104.0f);
    }
    SECTION("page 0 unreadable gives US Letter, which other failures take too") {
        std::vector<PageSize> out;
        REQUIRE_NOTHROW(out = fill_page_sizes(5, [&](std::size_t i) {
            if (i == 0 || i == 2) throw std::runtime_error("unreadable");
            return sized(i);
        }));
        REQUIRE(out.size() == 5);
        REQUIRE(out[0].width_pt == 612.0f);
        REQUIRE(out[0].height_pt == 792.0f);
        REQUIRE(out[2].width_pt == 612.0f);
        REQUIRE(out[2].height_pt == 792.0f);
        REQUIRE(out[1].width_pt == 101.0f);
    }
    SECTION("a size that is not a positive finite number counts as unreadable") {
        // MuPDF bounds a CBZ page whose image cannot be decoded by fz_empty_rect
        // (x0 = y0 = 0x7fffff80, x1 = y1 = (int)0x80000000), which gives this
        // width and height without throwing.
        const float empty_rect_extent = -2147483648.0f - 2147483520.0f;
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<PageSize> raw = {
            {100.0f, 200.0f}, {empty_rect_extent, empty_rect_extent}, {0.0f, 200.0f},
            {100.0f, -1.0f},  {inf, 200.0f},  {100.0f, nan},  {105.0f, 205.0f},
        };
        std::vector<PageSize> out;
        REQUIRE_NOTHROW(out = fill_page_sizes(raw.size(), [&](std::size_t i) { return raw[i]; }));
        REQUIRE(out.size() == raw.size());
        for (std::size_t i = 1; i <= 5; ++i) {
            INFO("page " << i);
            REQUIRE(out[i].width_pt == 100.0f);
            REQUIRE(out[i].height_pt == 200.0f);
        }
        REQUIRE(out[6].width_pt == 105.0f);
    }
    SECTION("page 0 with an unusable size gives US Letter") {
        std::vector<PageSize> out;
        REQUIRE_NOTHROW(out = fill_page_sizes(3, [&](std::size_t i) {
            if (i == 0) return PageSize{-4294967168.0f, -4294967168.0f};
            if (i == 1) return PageSize{0.0f, 0.0f};
            return sized(i);
        }));
        REQUIRE(out[0].width_pt == 612.0f);
        REQUIRE(out[0].height_pt == 792.0f);
        REQUIRE(out[1].width_pt == 612.0f);
        REQUIRE(out[2].width_pt == 102.0f);
    }
    SECTION("no pages") {
        REQUIRE(fill_page_sizes(0, sized).empty());
    }
}

TEST_CASE("DocumentView: request_page_render returns a token whose flag cancels",
          "[core][view][strip]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp);
    ReleaseOnExit guard{hold_worker(*view, 0)};

    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto out  = std::make_shared<Outcome>();
    auto fut  = out->got_pixmap.get_future();
    const RenderEngine::RenderToken tok =
        view->request_page_render(3, 0, record(out), flag);
    REQUIRE(tok.canceled == flag);

    tok.canceled->store(true);
    guard.release();
    REQUIRE_FALSE(wait_outcome(fut));
}

TEST_CASE("DocumentView: request_page_render releases the background pin",
          "[core][view][strip]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp);
    render_and_wait(*view, 0);
    view->trim_l1_for_background(0);
    REQUIRE_FALSE(view->l1_contains(5));

    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto out  = std::make_shared<Outcome>();
    auto fut  = out->got_pixmap.get_future();
    (void)view->request_page_render(5, 0, record(out), flag);
    REQUIRE(wait_outcome(fut));
    REQUIRE(view->l1_contains(5));
    REQUIRE(view->l1_contains(0));
}

TEST_CASE("PageCache: l1_contains does not change LRU order", "[core][cache][l1][strip]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        PageCache cache(/*l1*/ 2, /*l2*/ 0, ctx);
        auto pix = [ctx] { return fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0); };

        SECTION("l1_contains leaves A the oldest") {
            cache.put_pixmap(0, 1.0f, false, pix());   // A
            cache.put_pixmap(1, 1.0f, false, pix());   // B
            REQUIRE(cache.l1_contains(0, 1.0f, false));
            REQUIRE_FALSE(cache.l1_contains(0, 2.0f, false));   // other scale
            REQUIRE_FALSE(cache.l1_contains(0, 1.0f, true));    // other polarity
            REQUIRE_FALSE(cache.l1_contains(2, 1.0f, false));   // absent page
            cache.put_pixmap(2, 1.0f, false, pix());   // C evicts the oldest
            REQUIRE_FALSE(cache.l1_contains(0, 1.0f, false));
            REQUIRE(cache.l1_contains(1, 1.0f, false));
            REQUIRE(cache.l1_contains(2, 1.0f, false));
        }
        SECTION("the control: get_pixmap makes A the newest") {
            cache.put_pixmap(0, 1.0f, false, pix());   // A
            cache.put_pixmap(1, 1.0f, false, pix());   // B
            fz_pixmap* got = cache.get_pixmap(0, 1.0f, false);
            REQUIRE(got != nullptr);
            fz_drop_pixmap(ctx, got);
            cache.put_pixmap(2, 1.0f, false, pix());   // C evicts B
            REQUIRE(cache.l1_contains(0, 1.0f, false));
            REQUIRE_FALSE(cache.l1_contains(1, 1.0f, false));
        }
    }
    fz_drop_context(ctx);
}

// The queued P3 (a thumbnail's priority) is the positive half: without it a
// no-op cancel_thumbnail_renders would pass. The P0 and P1 are the strip's.
TEST_CASE("DocumentView: cancel_thumbnail_renders flags a queued P3 but no strip request",
          "[core][view][strip]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp);
    ReleaseOnExit guard{hold_worker(*view, 0)};

    auto flag0 = std::make_shared<std::atomic<bool>>(false);
    auto flag1 = std::make_shared<std::atomic<bool>>(false);
    auto flag3 = std::make_shared<std::atomic<bool>>(false);
    auto out0  = std::make_shared<Outcome>();
    auto out1  = std::make_shared<Outcome>();
    auto out3  = std::make_shared<Outcome>();
    auto fut0  = out0->got_pixmap.get_future();
    auto fut1  = out1->got_pixmap.get_future();
    auto fut3  = out3->got_pixmap.get_future();
    (void)view->request_page_render(2, 0, record(out0), flag0);
    (void)view->request_page_render(3, 1, record(out1), flag1);
    (void)view->request_page_render(4, 3, record(out3), flag3);

    view->cancel_thumbnail_renders();
    REQUIRE(flag3->load());
    REQUIRE_FALSE(flag0->load());
    REQUIRE_FALSE(flag1->load());

    guard.release();
    REQUIRE(wait_outcome(fut0));
    REQUIRE(wait_outcome(fut1));
    REQUIRE_FALSE(wait_outcome(fut3));
    REQUIRE_FALSE(flag0->load());
    REQUIRE_FALSE(flag1->load());
}
