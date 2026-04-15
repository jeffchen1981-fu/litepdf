#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "core/Document.hpp"
#include "core/DocumentView.hpp"

using litepdf::core::Document;
using litepdf::core::DocumentView;

namespace {

Document open_simple() {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    return doc;
}

}  // namespace

TEST_CASE("DocumentView constructs from opened Document", "[core][view][ctor]") {
    DocumentView view(open_simple());
    REQUIRE(view.page_count() > 0);
    REQUIRE(view.current_page() == 0);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE(view.ui_ctx() != nullptr);
    // source_path should round-trip through the move.
    REQUIRE(view.source_path().filename() == "simple.pdf");
}

TEST_CASE("DocumentView throws when given an unopened Document",
          "[core][view][ctor]") {
    Document doc;  // never opened
    REQUIRE_THROWS_AS(DocumentView(std::move(doc)), std::runtime_error);
}

TEST_CASE("DocumentView::set_current_page clamps and signals change",
          "[core][view][page]") {
    DocumentView view(open_simple());
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

TEST_CASE("DocumentView FitWidth scale derivation at 96 DPI",
          "[core][view][zoom]") {
    DocumentView view(open_simple());
    // simple.pdf page 0 is ~595.2 x 842.0 pt (A4).
    // FitWidth at 1190.4 DIP × 800 DIP × 96 DPI:
    //   phys_px_w = 1190.4 * (96/96) = 1190.4
    //   scale     = 1190.4 / 595.2 = 2.0
    view.set_zoom_mode(DocumentView::ZoomMode::FitWidth,
                       1190.4f, 800.0f, 96.0f);
    REQUIRE(view.zoom_scale() == Catch::Approx(2.0f).epsilon(0.001));
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
}

TEST_CASE("DocumentView FitPage scale derivation at 96 DPI",
          "[core][view][zoom]") {
    DocumentView view(open_simple());
    // FitPage at 595.2 × 421.0 DIP × 96 DPI on 595.2×842 page:
    //   fit_w = 595.2/595.2 = 1.0
    //   fit_h = 421.0/842.0 = 0.5
    //   min   = 0.5
    view.set_zoom_mode(DocumentView::ZoomMode::FitPage,
                       595.2f, 421.0f, 96.0f);
    REQUIRE(view.zoom_scale() == Catch::Approx(0.5f).epsilon(0.001));
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitPage);
}

TEST_CASE("DocumentView zoom_in/out cycles presets and bounds correctly",
          "[core][view][zoom]") {
    DocumentView view(open_simple());

    // Ramp up to the top preset.
    int up_steps = 0;
    while (view.zoom_in()) {
        ++up_steps;
        REQUIRE(up_steps < 32);  // guard against runaway
    }
    REQUIRE(view.zoom_scale() == Catch::Approx(4.0f));
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE_FALSE(view.zoom_in());  // already at max

    // Ramp back down.
    int down_steps = 0;
    while (view.zoom_out()) {
        ++down_steps;
        REQUIRE(down_steps < 32);
    }
    REQUIRE(view.zoom_scale() == Catch::Approx(0.5f));
    REQUIRE_FALSE(view.zoom_out());  // already at min
}
