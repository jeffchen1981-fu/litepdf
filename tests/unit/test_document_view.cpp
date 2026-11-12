#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/EscrowContext.hpp"
#include "ui/detail/ViewportMath.hpp"

// fz_drop_context is plain extern "C", so a local declaration is enough and
// keeps this TU MuPDF-header-free -- the same approach as
// test_escrow_context.cpp.
extern "C" {
struct fz_context;
struct fz_pixmap;
void fz_drop_context(fz_context* ctx);
void fz_drop_pixmap(fz_context* ctx, fz_pixmap* pix);
}

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::ui::bitmap_px_to_dip;
using litepdf::ui::pdf_point_to_dip;

namespace {

Document open_simple() {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    return doc;
}

// Shared inline dispatcher for all DocumentView unit tests. Each test
// gets its own local one in practice (below) to keep tasks isolated;
// this free function reduces the boilerplate. Keeping it non-static so
// each TU that includes this file is fine — it's only used here.
}  // namespace

TEST_CASE("DocumentView constructs from opened Document", "[core][view][ctor]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    REQUIRE(view.page_count() > 0);
    REQUIRE(view.current_page() == 0);
    // Default zoom mode is FitWidth -- page width fills the canvas, and the
    // overflow below the fold is reached with the wheel or the arrow keys.
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE(view.ui_ctx() != nullptr);
    // source_path should round-trip through the move.
    REQUIRE(view.source_path().filename() == "simple.pdf");
    // Phase 6: search() accessor returns a valid session reference.
    (void)view.search();
}

TEST_CASE("DocumentView throws when given an unopened Document",
          "[core][view][ctor]") {
    InlineDispatcher disp;
    Document doc;  // never opened
    REQUIRE_THROWS_AS(DocumentView(std::move(doc), disp), std::runtime_error);
}

TEST_CASE("DocumentView::set_current_page clamps and signals change",
          "[core][view][page]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    REQUIRE(view.current_page() == 0);

    // Same-index, negative, and out-of-range inputs all clamp to [0, last].
    REQUIRE_FALSE(view.set_current_page(0));
    REQUIRE_FALSE(view.set_current_page(-5));
    REQUIRE(view.current_page() == 0);

    const int last = view.page_count() - 1;
    if (last > 0) {
        REQUIRE(view.set_current_page(last));
        REQUIRE(view.current_page() == last);
        REQUIRE_FALSE(view.set_current_page(9999));
        REQUIRE(view.current_page() == last);
    } else {
        // simple.pdf is 1-page; clamp to 0 for any request.
        REQUIRE_FALSE(view.set_current_page(9999));
        REQUIRE(view.current_page() == 0);
    }
}

TEST_CASE("DocumentView FitWidth derives a percentage from viewport pixels",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // 1190.44 px / 595.22 pt = exactly 2.0 at 96 dpi.
    view.set_viewport(1190.44f, 800.0f, 96.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE(view.zoom_pct()     == Catch::Approx(2.0f).epsilon(0.001));
    REQUIRE(view.render_scale() == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("DocumentView render scale carries the dpi factor",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // The same physical viewport at 192 dpi: each DIP is two device pixels, so
    // the percentage halves while the render scale -- and the pixmap width in
    // pixels -- is unchanged.
    view.set_viewport(1190.44f, 800.0f, 192.0f);
    REQUIRE(view.zoom_pct()     == Catch::Approx(1.0f).epsilon(0.001));
    REQUIRE(view.render_scale() == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("DocumentView FitPage recomputes from the stored viewport",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    view.set_viewport(1190.44f, 842.0f, 96.0f);
    REQUIRE(view.zoom_pct() == Catch::Approx(2.0f).epsilon(0.001));
    // Same viewport, fit-page: height binds at exactly 1.0.
    view.set_zoom_mode_fit_page();
    REQUIRE(view.zoom_pct() == Catch::Approx(1.0f).epsilon(0.001));
}

// Spec S5 regression: pdf_point_to_dip(pt, zoom_pct()) -- the overlay path --
// must agree with bitmap_px_to_dip(pt * render_scale(), dpi) -- the pixmap
// path -- at every dpi, for a fixed zoom_pct_. This is the identity the whole
// overlay path rests on, and it is the reversed-unit error's regression test:
// exercised through a real DocumentView (not just the pure ViewportMath /
// ZoomMath functions) so it also pins DocumentView::render_scale() itself.
TEST_CASE("DocumentView quad mapping to dip is dpi invariant",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    // Custom, set explicitly -- not the constructor default -- so zoom_pct_
    // stays fixed across the viewport/dpi changes below instead of being
    // re-derived by a fit mode.
    view.set_zoom_pct(1.5f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);

    const float pt = 100.0f;  // an arbitrary PDF-point coordinate
    for (const float dpi : {96.0f, 144.0f, 192.0f}) {
        // Custom freezes zoom_pct_ but set_viewport still stores the new dpi,
        // which render_scale() depends on.
        view.set_viewport(800.0f, 600.0f, dpi);
        REQUIRE(view.zoom_pct() == Catch::Approx(1.5f));

        const float via_overlay = pdf_point_to_dip(pt, view.zoom_pct());
        const float via_pixmap  = bitmap_px_to_dip(pt * view.render_scale(), dpi);
        REQUIRE(via_overlay == Catch::Approx(via_pixmap));
    }
}

TEST_CASE("DocumentView zoom ladder walks the extended preset table",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_pct(1.0f);
    int up_steps = 0;
    while (view.zoom_in()) {
        ++up_steps;
        REQUIRE(up_steps < 32);
    }
    REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE_FALSE(view.zoom_in());

    int down_steps = 0;
    while (view.zoom_out()) {
        ++down_steps;
        REQUIRE(down_steps < 32);
    }
    REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    REQUIRE_FALSE(view.zoom_out());
}

TEST_CASE("DocumentView zoom in works above the old preset ceiling",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // A wide canvas against a narrow page derives a percentage above 4.0
    // (3000 / 595.22 = 5.04) -- exactly the state in which the shipped
    // zoom_in() could never find a larger rung and silently did nothing.
    view.set_viewport(3000.0f, 900.0f, 96.0f);
    REQUIRE(view.zoom_pct() > 4.0f);
    REQUIRE(view.zoom_in());
    REQUIRE(view.zoom_pct() == Catch::Approx(6.0f));
}

TEST_CASE("DocumentView set_zoom_pct clamps to the extended span",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_pct(99.0f);  REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
    view.set_zoom_pct(0.01f);  REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    view.set_zoom_pct(0.25f);  REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    view.set_zoom_pct(8.0f);   REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
}

// This pins the two lines that make zoom STICK, which is the whole point of
// PR-A1: set_zoom_pct's `zm = Custom` assignment, and set_viewport's early
// return while the mode is Custom. Without the early return, the very next
// set_viewport -- kick_render calls one through apply_viewport on every render,
// resize and DPI change -- would re-derive the fit percentage and silently
// throw the user's zoom away, which is exactly the defect this PR exists to
// fix. Both lines were previously uncovered: deleting either left the suite
// green.
TEST_CASE("DocumentView Custom zoom survives a later set_viewport",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    // Start from a fit mode with a known viewport, so the assertion below
    // cannot pass by the fit happening to agree with the custom value.
    view.set_zoom_mode_fit_width();
    view.set_viewport(1190.44f, 800.0f, 96.0f);
    REQUIRE(view.zoom_pct() == Catch::Approx(2.0f).epsilon(0.001));

    view.set_zoom_pct(3.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE(view.zoom_pct() == Catch::Approx(3.0f));

    // A different viewport: the fit would be 1.0 here (595.22 px / 595.22 pt),
    // so a re-derivation is unmissable.
    view.set_viewport(595.22f, 400.0f, 96.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE(view.zoom_pct() == Catch::Approx(3.0f));

    // The viewport IS stored even while frozen -- leaving Custom must fit the
    // dimensions just handed in, not the stale ones from before the zoom.
    view.set_zoom_mode_fit_width();
    REQUIRE(view.zoom_pct() == Catch::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("DocumentView set_viewport fits the larger page of a spread pair",
          "[core][view][zoom]") {
    // spread-unequal.pdf: page 0 is 420x595, page 1 is 595x842. This is the
    // test that fails if set_viewport ignores pair_page -- the ZoomMath case
    // computes max() in the test body and so cannot detect that.
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/spread-unequal.pdf").has_value());
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    view.set_zoom_mode_fit_page();

    const float slot_px = 600.0f, slot_h_px = 900.0f;

    // Without the pair: derived from page 0 (420x595) alone.
    // min(600/420, 900/595) = 1.42857. At that scale page 1 (595x842) is
    // 850 x 1203 -- overflowing its slot on BOTH axes. Compare width against
    // slot width and height against slot height; crossing them silently
    // asserts nothing.
    view.set_viewport(slot_px, slot_h_px, 96.0f);
    const float solo = view.zoom_pct();
    REQUIRE(solo == Catch::Approx(1.42857f).epsilon(0.001));
    REQUIRE(595.0f * solo > slot_px);        // page 1 too wide for its slot
    REQUIRE(842.0f * solo > slot_h_px);      // and too tall

    // With the pair: derived from max(420,595) x max(595,842).
    view.set_viewport(slot_px, slot_h_px, 96.0f, /*pair_page=*/1);
    const float paired = view.zoom_pct();
    REQUIRE(paired < solo);
    REQUIRE(420.0f * paired <= Catch::Approx(slot_px));     // page 0 width
    REQUIRE(595.0f * paired <= Catch::Approx(slot_h_px));   // page 0 height
    REQUIRE(595.0f * paired <= Catch::Approx(slot_px));     // page 1 width
    REQUIRE(842.0f * paired <= Catch::Approx(slot_h_px));   // page 1 height

    // An out-of-range pair index is ignored, not clamped into a wrong page.
    view.set_viewport(slot_px, slot_h_px, 96.0f, /*pair_page=*/99);
    REQUIRE(view.zoom_pct() == Catch::Approx(solo));
}

// damaged-pages.pdf: pages 2, 3, 5 and 6 cannot be sized. Page 1 is 420 x 400,
// page 4 is 300 x 800, page 0 is 420 x 595. A page that cannot be sized must not
// reach the window procedure as a C++ exception: set_viewport runs there.
TEST_CASE("DocumentView set_viewport fits the page of a pair that can be sized",
          "[core][view][zoom]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/damaged-pages.pdf").has_value());
    REQUIRE_THROWS(doc.page_size(2));   // Control: the fixture still fails.
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    view.set_zoom_mode_fit_page();

    // The right page is damaged: the left page's own fit.
    REQUIRE(view.set_current_page(1));
    view.set_viewport(600.0f, 900.0f, 96.0f, /*pair_page=*/2);
    CHECK(view.zoom_pct() == Catch::Approx(600.0f / 420.0f));

    // Its height decides here. Page 0 is taller, so a stand-in for the damaged
    // page shows: the width above is page 0's too.
    view.set_viewport(1000.0f, 500.0f, 96.0f, /*pair_page=*/2);
    CHECK(view.zoom_pct() == Catch::Approx(500.0f / 400.0f));

    // The left page is damaged: the right page's.
    view.set_current_page(3);
    view.set_viewport(600.0f, 900.0f, 96.0f, /*pair_page=*/4);
    CHECK(view.zoom_pct() == Catch::Approx(900.0f / 800.0f));

    // Its width decides here. Page 0 is wider, so a stand-in in the fit shows.
    view.set_zoom_mode_fit_width();
    view.set_viewport(600.0f, 900.0f, 96.0f, /*pair_page=*/4);
    CHECK(view.zoom_pct() == Catch::Approx(600.0f / 300.0f));
}

TEST_CASE("DocumentView set_viewport fits page 0's size when nothing shown can be sized",
          "[core][view][zoom]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/damaged-pages.pdf").has_value());
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    view.set_zoom_mode_fit_page();
    view.set_viewport(1000.0f, 1000.0f, 96.0f);
    const float page_0 = 1000.0f / 595.0f;
    REQUIRE(view.zoom_pct() == Catch::Approx(page_0));

    // One page away, so that the view has another zoom to keep by mistake.
    view.set_current_page(4);
    REQUIRE(view.zoom_pct() == Catch::Approx(1000.0f / 800.0f));

    // A single damaged page, reached through set_current_page's own refit.
    view.set_current_page(2);
    CHECK(view.zoom_pct() == Catch::Approx(page_0));

    // A pair with both pages damaged.
    view.set_current_page(4);
    view.set_current_page(5);
    view.set_viewport(1000.0f, 1000.0f, 96.0f, /*pair_page=*/6);
    CHECK(view.zoom_pct() == Catch::Approx(page_0));

    // The two calls that set a fit mode on the page already current.
    view.set_zoom_mode_fit_width();
    CHECK(view.zoom_pct() == Catch::Approx(1000.0f / 420.0f));
    view.set_zoom_mode_fit_page();
    CHECK(view.zoom_pct() == Catch::Approx(page_0));
}

TEST_CASE("DocumentView set_viewport leaves out a size that is not usable",
          "[core][view][zoom]") {
    // cbz-sizing.cbz page 12 is an image MuPDF cannot decode: it has no throw, but
    // a width and height of about -4.3e9 pt.
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/cbz-sizing.cbz").has_value());
    REQUIRE(doc.page_size(12).width_pt < 0.0f);   // Control.
    const float page_0 = 800.0f / doc.page_size(0).width_pt;
    const float page_1 = 800.0f / doc.page_size(1).width_pt;
    REQUIRE(page_0 != Catch::Approx(page_1));
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    view.set_viewport(800.0f, 600.0f, 96.0f);
    REQUIRE(view.zoom_pct() == Catch::Approx(page_0));

    view.set_current_page(1);
    REQUIRE(view.zoom_pct() == Catch::Approx(page_1));
    view.set_current_page(12);
    CHECK(view.zoom_pct() == Catch::Approx(page_0));
}

TEST_CASE("DocumentView set_viewport fits US Letter when page 0 has no size either",
          "[core][view][zoom]") {
    // cbz-broken-first.cbz: page 1 is 300 x 450; pages 0, 2 and 3 are images MuPDF
    // cannot decode. Letter's 612 x 792 is written out: an expected value read from
    // kFallbackPageSize would follow any change to it.
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/cbz-broken-first.cbz").has_value());
    REQUIRE(doc.page_count() == 4);
    REQUIRE(doc.page_size(0).width_pt < 0.0f);   // Control: it opened without a size.
    REQUIRE(doc.page_size(1).width_pt == Catch::Approx(300.0f));
    REQUIRE(doc.page_size(1).height_pt == Catch::Approx(450.0f));
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);

    const auto fits_letter = [&view](float letter, float page_1) {
        view.set_viewport(1000.0f, 1000.0f, 96.0f);
        CHECK(view.zoom_pct() == Catch::Approx(letter));

        // One page away, so that the view has another zoom to keep by mistake.
        view.set_current_page(1);
        REQUIRE(view.zoom_pct() == Catch::Approx(page_1));
        view.set_current_page(2);
        CHECK(view.zoom_pct() == Catch::Approx(letter));

        // A pair with both pages damaged.
        view.set_current_page(1);
        REQUIRE(view.zoom_pct() == Catch::Approx(page_1));
        view.set_current_page(2);
        view.set_viewport(1000.0f, 1000.0f, 96.0f, /*pair_page=*/3);
        CHECK(view.zoom_pct() == Catch::Approx(letter));
    };

    SECTION("FitWidth") {
        view.set_zoom_mode_fit_width();
        fits_letter(1000.0f / 612.0f, 1000.0f / 300.0f);
    }
    SECTION("FitPage") {
        view.set_zoom_mode_fit_page();
        fits_letter(1000.0f / 792.0f, 1000.0f / 450.0f);
    }
}

TEST_CASE("DocumentView selection persists across a page change until cleared",
          "[core][view][selection]") {
    InlineDispatcher disp;
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/search.pdf").has_value());
    DocumentView view(std::move(doc), disp);
    REQUIRE_FALSE(view.selection().has_value());

    litepdf::core::TextSelection sel;
    sel.page      = 0;
    sel.anchor    = { 10.0f, 20.0f };
    sel.extent    = { 200.0f, 40.0f };
    sel.text_utf8 = "Lorem";
    view.set_selection(sel);

    // Spec §2, model 1b: a page change does not clear it.
    REQUIRE(view.set_current_page(3));
    REQUIRE(view.selection().has_value());
    REQUIRE(view.selection()->page == 0);
    REQUIRE(view.selection()->text_utf8 == "Lorem");

    view.clear_selection();
    REQUIRE_FALSE(view.selection().has_value());
}

TEST_CASE("DocumentView set_selection replaces the previous selection",
          "[core][view][selection]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);

    litepdf::core::TextSelection first;
    first.page      = 0;
    first.text_utf8 = "first";
    view.set_selection(first);

    litepdf::core::TextSelection second;
    second.page      = 0;
    second.text_utf8 = "second";
    view.set_selection(second);

    REQUIRE(view.selection()->text_utf8 == "second");
}

// #74: DocumentView clones ui_ctx and cache_ctx before it builds PageCache,
// RenderEngine and SearchSession. Only ~DocumentView used to drop those two
// clones, and ~DocumentView never runs for a half-built object -- so a throw
// from any of the three leaked both.
//
// num_workers == 0 is the deterministic seam: RenderEngine's ctor rejects it
// with std::invalid_argument, after the clones exist and after PageCache has
// been built on cache_ctx.
//
// The escrow is the oracle. It holds the family's root alive past the
// Document's death, so family_context_count keeps reporting after the throw:
// root + escrow == 2 when the clones were dropped, 4 when they leaked.
TEST_CASE("DocumentView drops its context clones when a member ctor throws",
          "[core][view][ctor]") {
    InlineDispatcher disp;
    litepdf::core::EscrowContext probe;
    {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);
        probe = litepdf::core::EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(probe.valid());
        REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == 2);

        REQUIRE_THROWS_AS(DocumentView(std::move(doc), disp, {}, 0),
                          std::invalid_argument);
    }

    REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == 2);
}

// #49: which neighbours request_render_with_prefetch queues. The view's only
// worker is held inside the first P0's completion callback, so every later
// request stays queued and the count each call adds is exact. Which neighbour
// is chosen is pinned by test_prefetch_plan.cpp; this pins that the view acts
// on the plan.
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
    ~ReleaseOnExit() {
        try { held->release.set_value(); } catch (const std::future_error&) {}
    }
};

std::shared_ptr<HeldWorker> hold_worker(DocumentView& view, int page) {
    auto held = std::make_shared<HeldWorker>();
    held->released = held->release.get_future().share();
    auto entered = held->entered.get_future();
    (void)view.set_current_page(page);
    view.request_render_with_prefetch(page,
        [held](fz_pixmap* p, fz_context* ctx) {
            if (p) fz_drop_pixmap(ctx, p);
            held->entered.set_value();
            (void)held->released.wait_for(std::chrono::seconds(30));
        },
        {});
    REQUIRE(entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return held;
}

// Requests one request_render_with_prefetch(page) adds: its P0 plus the
// neighbours it prefetches.
std::size_t queued_by(DocumentView& view, int page) {
    const std::size_t before = view.queued_render_count();
    (void)view.set_current_page(page);
    view.request_render_with_prefetch(page,
        [](fz_pixmap* p, fz_context* ctx) { if (p) fz_drop_pixmap(ctx, p); }, {});
    return view.queued_render_count() - before;
}

// One worker, render scale 1 (Custom 100% at 96 dpi), the given L1 budget.
std::unique_ptr<DocumentView> one_worker_view(InlineDispatcher& disp,
                                              std::size_t budget) {
    Document doc;
    REQUIRE_FALSE(doc.open(kEightPages).has_value());
    REQUIRE(doc.page_count() == 8);
    auto view = std::make_unique<DocumentView>(std::move(doc), disp, std::nullopt,
                                               /*workers*/ 1, /*l1*/ 5, /*l2*/ 10,
                                               budget);
    view->set_viewport(800.0f, 600.0f, 96.0f);
    view->set_zoom_pct(1.0f);
    return view;
}

}  // namespace

TEST_CASE("DocumentView prefetches both neighbours while three pages fit (#49)",
          "[core][view][prefetch]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, litepdf::core::kDefaultL1BudgetBytes);
    ReleaseOnExit guard{hold_worker(*view, 4)};

    REQUIRE(queued_by(*view, 3) == 3);  // P0 + pages 2 and 4
    REQUIRE(queued_by(*view, 0) == 2);  // P0 + page 1: there is no page -1
    REQUIRE(queued_by(*view, 7) == 2);  // P0 + page 6: there is no page 8
}

TEST_CASE("DocumentView prefetches one neighbour past the byte budget (#49)",
          "[core][view][prefetch]") {
    InlineDispatcher disp;
    // A one-byte budget: every page is over a third of it.
    auto view = one_worker_view(disp, 1);
    ReleaseOnExit guard{hold_worker(*view, 4)};

    REQUIRE(queued_by(*view, 5) == 2);  // forward: P0 + page 6
    REQUIRE(queued_by(*view, 3) == 2);  // backward: P0 + page 2
    // Home: backward onto the first page, whose only neighbour is page 1.
    REQUIRE(queued_by(*view, 0) == 2);
    // End: forward onto the last page, whose only neighbour is page 6.
    REQUIRE(queued_by(*view, 7) == 2);
    REQUIRE(queued_by(*view, 7) == 2);  // a resubmit of the same page
}

// #164: the direction the single-page prefetch uses follows the reader's moves
// in every layout. move_to() is what every layout's page change reduces to;
// kick() is MainWindow::kick_render's single-page branch, which calls no
// set_current_page first. 8 pages: spread pairs (0) (1,2) (3,4) (5,6) (7).
namespace {

using litepdf::core::TravelDirection;

void move_to(DocumentView& view, int page) { (void)view.set_current_page(page); }

void kick(DocumentView& view, int page) {
    view.request_render_with_prefetch(page,
        [](fz_pixmap* p, fz_context* ctx) { if (p) fz_drop_pixmap(ctx, p); }, {});
}

// Spec 5.3 case 1: single page 1; spread forward to (7), back to (5,6); spread
// off; the single-page kick for page 5, with no page change before it.
void spread_issue_example(DocumentView& view) {
    move_to(view, 1);
    kick(view, 1);
    view.set_dual_page(true);
    for (int p : {1, 3, 5, 7, 5}) move_to(view, p);
    view.set_dual_page(false);
    kick(view, 5);
}

}  // namespace

TEST_CASE("DocumentView travel: spread moves set the direction single page resumes with (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    spread_issue_example(*view);

    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);
}

TEST_CASE("DocumentView travel: strip moves set the direction single page resumes with (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    move_to(*view, 1);
    kick(*view, 1);
    view->set_continuous(true);
    for (int p : {1, 2, 5, 3}) move_to(*view, p);  // the carry, then the scroll
    view->set_continuous(false);
    move_to(*view, 3);  // the carry back (PdfCanvasStrip.cpp:428)
    kick(*view, 3);

    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);
}

TEST_CASE("DocumentView travel: a spread snap to the left page is not a move (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    move_to(*view, 4);
    view->set_dual_page(true);
    for (int p : {3, 5, 3}) move_to(*view, p);  // the snap, then (5,6), (3,4)
    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);

    move_to(*view, 6);  // a go-to on a right page
    move_to(*view, 5);  // its snap to the pair's left page
    REQUIRE(view->prefetch_travel() == TravelDirection::Forward);
}

TEST_CASE("DocumentView travel: the prefetch after spread off renders the neighbour behind (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    // One worker and a one-byte budget: one neighbour is prefetched, and L1
    // keeps only its newest entry, so once the prefetch lands L1 holds just
    // that neighbour. The only earlier renders are pages 1 and 2.
    auto view = one_worker_view(disp, 1);

    spread_issue_example(*view);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!view->l1_contains(4) && !view->l1_contains(6) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(view->l1_contains(4));
    REQUIRE_FALSE(view->l1_contains(6));
}

// #164: a layout switch is not a move. The key resets when the flag really
// changes, and the next single-page kick seeds it from the view's own page.
TEST_CASE("DocumentView travel: turning the spread on is not a move (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    move_to(*view, 2);
    move_to(*view, 4);  // Forward; page 4 is the right page of (3,4)
    view->set_dual_page(true);
    move_to(*view, 3);  // the snap

    REQUIRE(view->prefetch_travel() == TravelDirection::Forward);
}

TEST_CASE("DocumentView travel: setting continuous to its current value keeps the reference (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    view->set_continuous(true);
    move_to(*view, 4);
    move_to(*view, 5);  // Forward
    view->set_continuous(true);  // what every tab switch does
    move_to(*view, 4);

    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);
}

TEST_CASE("DocumentView travel: the first page turn after spread off counts (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    spread_issue_example(*view);  // ends Backward, at single page 5
    move_to(*view, 6);
    kick(*view, 6);

    REQUIRE(view->prefetch_travel() == TravelDirection::Forward);
}

TEST_CASE("DocumentView travel: the first page turn after a continuous flip on a tab switch counts (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    view->set_continuous(true);
    move_to(*view, 5);
    move_to(*view, 3);  // Backward
    view->set_continuous(false);
    kick(*view, 3);  // a tab switch to a paged-single target sets no page
    move_to(*view, 4);
    kick(*view, 4);

    REQUIRE(view->prefetch_travel() == TravelDirection::Forward);
}

TEST_CASE("DocumentView travel: the first kick after a switch keys the view's own page (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    move_to(*view, 6);
    move_to(*view, 7);  // Forward
    view->set_continuous(true);
    view->set_continuous(false);
    // A session restore kick on a document that shrank: the saved page is
    // past the end, and the view holds page 7. Its renders fail in MuPDF.
    kick(*view, 20);
    move_to(*view, 7);

    REQUIRE(view->prefetch_travel() == TravelDirection::Forward);
}

TEST_CASE("DocumentView travel: spread off into the strip onto the held right page is not a move (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    view->set_dual_page(true);
    for (int p : {3, 5, 3}) move_to(*view, p);
    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);

    view->set_dual_page(false);
    view->set_continuous(true);
    move_to(*view, 4);  // the carry onto the held right page
    view->set_continuous(false);
    move_to(*view, 4);
    kick(*view, 4);
    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);
}

TEST_CASE("DocumentView travel: a carry onto the current page sets the reference (#164)",
          "[core][view][prefetch][travel]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, 1);

    move_to(*view, 5);
    move_to(*view, 3);
    REQUIRE(view->prefetch_travel() == TravelDirection::Backward);

    view->set_continuous(true);
    move_to(*view, 3);  // single page 3 -> strip: the carry lands on the current page
    move_to(*view, 4);  // the first scroll
    REQUIRE(view->prefetch_travel() == TravelDirection::Forward);
}

// #159: a tab going to the background keeps only the L1 pixmaps of the pages
// it shows, at its current render scale and polarity.
namespace {

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

TEST_CASE("DocumentView background trim keeps the shown pages at the current scale and polarity (#159)",
          "[core][view][trim]") {
    InlineDispatcher disp;
    auto view = one_worker_view(disp, litepdf::core::kDefaultL1BudgetBytes);
    render_and_wait(*view, 0);
    render_and_wait(*view, 1);
    render_and_wait(*view, 2);
    view->set_invert_colors(true);
    render_and_wait(*view, 1);   // page 1, other polarity
    view->set_invert_colors(false);
    view->set_zoom_pct(2.0f);
    render_and_wait(*view, 1);   // page 1, other scale
    view->set_zoom_pct(1.0f);
    REQUIRE(view->l1_entry_count() == 5);

    SECTION("single page") {
        view->trim_l1_for_background(1);
        REQUIRE(view->l1_entry_count() == 1);
        REQUIRE(view->l1_holds(1));
        REQUIRE_FALSE(view->l1_holds(0));
    }
    SECTION("both halves of a spread") {
        view->trim_l1_for_background(1, 2);
        REQUIRE(view->l1_entry_count() == 2);
        REQUIRE(view->l1_holds(1));
        REQUIRE(view->l1_holds(2));
    }
    SECTION("the polarity in force decides which entry stays") {
        view->set_invert_colors(true);
        view->trim_l1_for_background(1);
        REQUIRE(view->l1_entry_count() == 1);
        REQUIRE(view->l1_holds(1));    // the inverted one
        view->set_invert_colors(false);
        REQUIRE_FALSE(view->l1_holds(1));
    }
    SECTION("the next render request releases the pin") {
        view->trim_l1_for_background(1);
        render_and_wait(*view, 0);
        REQUIRE(view->l1_entry_count() == 2);
        REQUIRE(view->l1_holds(0));
        REQUIRE(view->l1_holds(1));
    }
}

// ---- #53 view rotation ------------------------------------------------------
// The size caches stay unrotated; the readers report the view frame.

TEST_CASE("DocumentView rotation: known_page_size swaps at 90 and 270, not 180",
          "[core][view][rotation]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    const auto real = view.document().page_size(0);
    // Cross-check against the fixture's literal size.
    REQUIRE(real.width_pt == Catch::Approx(595.22f));
    REQUIRE(real.height_pt == Catch::Approx(842.0f));

    const auto check_frame = [&](bool want_swapped) {
        const auto s = view.known_page_size(0);
        REQUIRE(s.has_value());
        if (want_swapped) {
            CHECK(s->width_pt == real.height_pt);
            CHECK(s->height_pt == real.width_pt);
        } else {
            CHECK(s->width_pt == real.width_pt);
            CHECK(s->height_pt == real.height_pt);
        }
    };

    check_frame(false);
    view.rotate_cw();
    REQUIRE(view.rotation().quarters == 1);
    check_frame(true);
    view.rotate_cw();
    check_frame(false);
    view.rotate_cw();
    check_frame(true);
    view.rotate_cw();
    REQUIRE(view.rotation() == litepdf::core::Rotation{0});
    check_frame(false);

    view.rotate_ccw();
    REQUIRE(view.rotation().quarters == 3);
    check_frame(true);
}

TEST_CASE("DocumentView rotation: fit width refits from the turned size",
          "[core][view][rotation]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    const auto real = view.document().page_size(0);

    view.set_viewport(1000.0f, 800.0f, 96.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    CHECK(view.zoom_pct() == Catch::Approx(1000.0f / real.width_pt));
    CHECK(view.zoom_pct() == Catch::Approx(1000.0f / 595.22f));

    SECTION("fit width follows the turned width") {
        view.rotate_cw();
        view.set_viewport(1000.0f, 800.0f, 96.0f);
        CHECK(view.zoom_pct() == Catch::Approx(1000.0f / real.height_pt));
        CHECK(view.zoom_pct() == Catch::Approx(1000.0f / 842.0f));
    }

    SECTION("custom zoom is unchanged by the turn and the set_viewport") {
        view.set_zoom_pct(1.5f);
        view.rotate_cw();
        CHECK(view.zoom_pct() == 1.5f);
        view.set_viewport(1000.0f, 800.0f, 96.0f);
        CHECK(view.zoom_pct() == 1.5f);
    }
}

TEST_CASE("DocumentView rotation: fit page refits from the turned size",
          "[core][view][rotation]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    const auto real = view.document().page_size(0);

    view.set_zoom_mode_fit_page();
    view.rotate_cw();
    view.set_viewport(1000.0f, 800.0f, 96.0f);
    const float expected = std::min(1000.0f / real.height_pt, 800.0f / real.width_pt);
    CHECK(view.zoom_pct() == Catch::Approx(expected));
    CHECK(view.zoom_pct() == Catch::Approx(1.1876f).margin(0.001f));
}

TEST_CASE("DocumentView rotation: a resize after the turn fits the turned page",
          "[core][view][rotation]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    const auto real = view.document().page_size(0);

    view.rotate_cw();
    view.set_viewport(1000.0f, 800.0f, 96.0f);
    view.set_viewport(1200.0f, 800.0f, 96.0f);
    CHECK(view.zoom_pct() == Catch::Approx(1200.0f / real.height_pt));
    CHECK(view.zoom_pct() == Catch::Approx(1200.0f / 842.0f));
}

// R1 guard: a turn only records the rotation. At a custom zoom the render
// scale does not change, so the repeat request is a cache hit and starts no
// render. A turn that dropped or cancelled the cache would start one.
TEST_CASE("DocumentView rotation: a turn at custom zoom starts no render",
          "[core][view][rotation]") {
    using namespace std::chrono;
    InlineDispatcher disp;
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/selection.pdf").has_value());
    REQUIRE(doc.page_count() == 8);
    DocumentView view(std::move(doc), disp);   // default capacities
    view.set_viewport(800.0f, 600.0f, 96.0f);
    view.set_zoom_pct(1.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);

    // Settled: nothing queued and no render started for 300 ms.
    const auto wait_settled = [&] {
        const auto deadline = steady_clock::now() + seconds(10);
        auto quiet_since    = steady_clock::now();
        auto started        = view.renders_started();
        while (steady_clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(10));
            const auto now_started = view.renders_started();
            if (view.queued_render_count() != 0 || now_started != started) {
                started     = now_started;
                quiet_since = steady_clock::now();
            } else if (steady_clock::now() - quiet_since >= milliseconds(300)) {
                return true;
            }
        }
        return false;
    };
    const auto request_and_wait = [&] {
        auto done = std::make_shared<std::promise<void>>();
        auto fut  = done->get_future();
        (void)view.set_current_page(1);
        view.request_render_with_prefetch(1,
            [done](fz_pixmap* p, fz_context* ctx) {
                if (p) fz_drop_pixmap(ctx, p);
                done->set_value();
            }, {});
        REQUIRE(fut.wait_for(seconds(10)) == std::future_status::ready);
    };

    request_and_wait();
    REQUIRE(wait_settled());
    const std::size_t baseline = view.renders_started();
    REQUIRE(baseline > 0);

    view.rotate_cw();
    request_and_wait();
    REQUIRE(wait_settled());
    CHECK(view.renders_started() == baseline);
}
