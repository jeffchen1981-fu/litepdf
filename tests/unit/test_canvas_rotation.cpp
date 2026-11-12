// On-screen view rotation (#53) on a real PdfCanvas: the rotate command, its
// landing and the strip relayout (spec 3.5, 5.4). Every case turns the page
// through the production PdfCanvas::rotate and then the rig's kick, in the order
// MainWindow's command handler uses.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CanvasRig.hpp"

using namespace canvas_rig;
using Catch::Approx;

namespace {

// The centre x of a hit quad, in points.
float centre_x(const litepdf::core::Document::PageHit& q) {
    return (q.ul_x + q.ur_x + q.ll_x + q.lr_x) * 0.25f;
}

}  // namespace

TEST_CASE("PdfCanvas rotation: the fixture's word sits where the smoke row expects",
          "[canvas][rotation]") {
    const Document doc = open_fixture(kMarker);
    const auto hits = doc.page_hits(kMarkWordPage, "ZENITH", {});
    REQUIRE(hits.size() == 1);
    CHECK(centre_x(hits[0]) == Approx(kMarkWordCentreXPt).margin(0.5));
}

TEST_CASE("PdfCanvas rotation: the rig lands a strip page", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();
    rig.land_strip(0);

    const auto r = Access::strip_page_rect(rig.canvas(), 0);
    // The square spans [36, 108] pt on both axes: its centre is 72 pt in.
    CHECK(is_dark(Access::painted_pixel(rig.canvas(), static_cast<int>(r.x + 72.0),
                                        static_cast<int>(r.y + 72.0))));
}

TEST_CASE("PdfCanvas rotation: the strip at custom zoom relays out turned pages",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();
    rig.canvas().strip_navigate(1);
    rig.land_strip(1);

    rig.rotate(true);

    const auto& layout = Access::strip_layout(rig.canvas());
    CHECK(layout.height(0) == Approx(595.276));
    CHECK(rig.view().current_page() == 1);
    CHECK(Access::strip_scroll_y(rig.canvas()) == Approx(layout.anchor(1)));
}

TEST_CASE("PdfCanvas rotation: every strip page takes its own turned height",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kUnequal);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();

    // One turned size shared by every page would pass on a fixture whose pages
    // are all as wide as each other.
    const Document& doc = rig.view().document();
    const int n = rig.view().page_count();
    bool widths_differ = false;
    for (int i = 1; i < n; ++i) {
        if (doc.page_size(static_cast<std::size_t>(i)).width_pt != doc.page_size(0).width_pt) {
            widths_differ = true;
        }
    }
    REQUIRE(widths_differ);

    rig.rotate(true);

    const auto& layout = Access::strip_layout(rig.canvas());
    REQUIRE(layout.page_count() == n);
    for (int i = 0; i < n; ++i) {
        CAPTURE(i);
        const auto size = doc.page_size(static_cast<std::size_t>(i));
        CHECK(layout.height(i) == Approx(size.width_pt));
        CHECK(layout.width(i) == Approx(size.height_pt));
    }
}

TEST_CASE("PdfCanvas rotation: the strip at fit width keeps the page and refits",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kLarge);
    rig.view().set_zoom_mode_fit_width();
    rig.enter_strip();
    rig.canvas().strip_navigate(30);
    const float before = rig.view().zoom_pct();

    rig.rotate(true);

    CHECK(rig.view().current_page() == 30);
    CHECK(Access::strip_layout(rig.canvas()).zoom_pct() == Approx(rig.view().zoom_pct()));
    // Letter: the page was fitted by its width, 612 pt, and is now fitted by its
    // turned width, 792 pt.
    CHECK(rig.view().zoom_pct() == Approx(before * 612.0f / 792.0f).epsilon(1e-3));
}

TEST_CASE("PdfCanvas rotation: a turn ends the strip's held place", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Spread, kLarge);
    rig.view().set_zoom_pct(1.0f);
    enter_spread_from(rig, 5, 300.0);
    REQUIRE(Access::held_place(rig.canvas()));

    rig.rotate(true);

    CHECK_FALSE(Access::held_place(rig.canvas()));
    CHECK(Access::anchor_kind(rig.canvas()) == PageAnchor::Kind::Top);
    rig.land_both();
    leave_spread(rig);

    const CarriedPosition at = Access::strip_position(rig.canvas());
    CHECK(at.page == 5);
    CHECK(at.offset_dip == 0.0);
}

TEST_CASE("PdfCanvas rotation: a turn returns a panned single page to its top-left",
          "[canvas][rotation]") {
    // At 3.0 the page overflows 1000 x 850 both ways, upright and turned.
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_pct(3.0f);
    rig.kick(0);
    rig.land_left();
    Access::pan_by(rig.canvas(), -200.0f, -300.0f);
    REQUIRE(rig.pan_x() != 0.0f);
    REQUIRE(rig.pan_y() != 0.0f);

    rig.rotate(true);
    rig.land_left();

    CHECK(rig.pan_x() == 0.0f);
    CHECK(rig.pan_y() == 0.0f);
}

TEST_CASE("PdfCanvas rotation: at fit width the Top anchor lands with the new scale",
          "[canvas][rotation]") {
    // Wide, so the turned page still overflows at fit width: 595.276 x 1600 /
    // 841.89 is about 1131 DIP tall. In the 1000 wide rig it would be 707, and
    // pan_y == 0 would hold by the clamp alone.
    Rig rig(1600, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_mode_fit_width();
    rig.kick(0);
    rig.land_left();
    Access::pan_by(rig.canvas(), 0.0f, -200.0f);
    REQUIRE(rig.pan_y() != 0.0f);

    rig.rotate(true);

    CHECK(Access::anchor_kind(rig.canvas()) == PageAnchor::Kind::Top);
    CHECK_FALSE(Access::anchor_applied(rig.canvas()));
    rig.land_left();
    CHECK(rig.pan_y() == 0.0f);
    CHECK(Access::anchor_applied(rig.canvas()));
}

TEST_CASE("PdfCanvas rotation: a panned spread lands at the pair's top",
          "[canvas][rotation]") {
    // Every spread-pairs.pdf page is 420 pt wide, so at 3.0 the turned pair is
    // 1260 DIP tall and overflows the 850 canvas. At 2.0 it would be 840, and
    // pan_y == 0 would hold by the clamp alone.
    Rig rig;
    rig.view().set_zoom_pct(3.0f);
    rig.kick(1);
    rig.land_both();
    Access::pan_by(rig.canvas(), 0.0f, -400.0f);
    REQUIRE(rig.pan_y() != 0.0f);

    rig.rotate(true);
    rig.land_both();

    REQUIRE(Access::content_extent(rig.canvas()).h > static_cast<float>(kHeight));
    CHECK(rig.pan_y() == 0.0f);
}

TEST_CASE("PdfCanvas rotation: a turn during a drag ends the gesture",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.kick(0);
    rig.land_left();

    // Page 0's text line: from x = 150 pt, baseline 60 pt from the top.
    const float pct = rig.view().zoom_pct();
    const int x = static_cast<int>(160.0f * pct + rig.pan_x());
    const int y = static_cast<int>(56.0f * pct + rig.pan_y());
    Access::left_down(rig.canvas(), x, y);
    Access::mouse_move(rig.canvas(), x + 40, y);
    REQUIRE(Access::gesture_live(rig.canvas()));

    rig.canvas().rotate(true);

    CHECK_FALSE(Access::gesture_live(rig.canvas()));
}
