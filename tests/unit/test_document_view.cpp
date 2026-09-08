#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;

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
    // PR-A1 ships FitPage; PR-A2 restores FitWidth once wheel scrolling exists.
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitPage);
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
