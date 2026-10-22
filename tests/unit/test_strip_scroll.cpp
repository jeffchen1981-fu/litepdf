#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <limits>
#include <vector>

#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripScroll.hpp"

using Catch::Approx;
using namespace litepdf::ui;

namespace {

StripLayout three_pages(double zoom) {
    return StripLayout({{100, 200}, {50, 100}, {100, 300}}, zoom);
}

StripLayout ten_pages(double zoom = 1.0) {
    return StripLayout(std::vector<StripPageSize>(10, StripPageSize{100, 100}), zoom);
}

}  // namespace

TEST_CASE("StripScroll: clamp keeps scroll inside the strip", "[ui][strip]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // total == 632, vp_h == 300 -> the range is [0, 332].
    CHECK(clamp_strip_scroll(-5, 632, 300) == 0.0);
    CHECK(clamp_strip_scroll(100, 632, 300) == 100.0);
    CHECK(clamp_strip_scroll(400, 632, 300) == 332.0);
    CHECK(clamp_strip_scroll(nan, 632, 300) == 0.0);
    // The strip fits the viewport: every input is 0.
    CHECK(clamp_strip_scroll(-5, 632, 700) == 0.0);
    CHECK(clamp_strip_scroll(0, 632, 700) == 0.0);
    CHECK(clamp_strip_scroll(50, 632, 700) == 0.0);
    CHECK(clamp_strip_scroll(nan, 632, 700) == 0.0);
}

TEST_CASE("StripScroll: relayout keeps the viewport-top point", "[ui][strip]") {
    const StripLayout before = three_pages(1.0);
    const StripLayout after = three_pages(2.0);

    // 108 is halfway down page 0 (top 8, height 200).
    const StripPos in_page = before.position_at(108);
    CHECK(in_page.kind == StripPos::Kind::InPage);
    CHECK(in_page.page == 0);
    CHECK(in_page.fraction == Approx(0.5));
    CHECK(relayout_scroll(in_page, after, 300) == Approx(after.top(0) + 0.5 * 400.0));
    CHECK(relayout_scroll(in_page, after, 300) == Approx(208.0));

    // A point in the gap after page 0 (half of it): top(0) + h + 4.
    const StripPos in_gap = before.position_at(before.top(0) + 200.0 + 4.0);
    CHECK(in_gap.kind == StripPos::Kind::InGapAfter);
    CHECK(in_gap.page == 0);
    CHECK(relayout_scroll(in_gap, after, 300) == Approx(after.top(0) + 400.0 + 4.0));
    CHECK(relayout_scroll(in_gap, after, 300) == Approx(412.0));

    // The result is clamped to the new strip.
    const StripPos last = before.position_at(600);
    CHECK(relayout_scroll(last, after, 300) <= after.total_height() - 300.0);
}

TEST_CASE("StripScroll: current page at the bottom is the last page", "[ui][strip]") {
    const StripLayout l = ten_pages();
    const double vp_h = 500;
    const double total = l.total_height();
    CHECK(derive_current_page(l, total - vp_h, vp_h) == 9);
    CHECK(derive_current_page(l, 0, vp_h) == 0);
    // Pages 3..6 are fully visible and tie: the lowest index wins.
    CHECK(derive_current_page(l, l.anchor(3), vp_h) == 3);
}

TEST_CASE("StripScroll: current page is the one with the largest visible height",
          "[ui][strip]") {
    const StripLayout l = three_pages(1.0);
    // [150, 300): page 0 shows 58, page 1 shows 84.
    CHECK(derive_current_page(l, 150, 150) == 1);
}

TEST_CASE("StripScroll: a strip that fits the viewport is decided by visible height",
          "[ui][strip]") {
    const StripLayout l = three_pages(1.0);
    REQUIRE(l.total_height() == 632.0);
    CHECK(derive_current_page(l, 0, 700) == 2);
    // Near-fit: total - vp_h == 0.25 < 1, so neither rule 1 nor rule 2 fires.
    CHECK(derive_current_page(l, 0, 631.75) == 2);
}

TEST_CASE("StripScroll: no pages means no current page", "[ui][strip]") {
    const StripLayout empty;
    CHECK(derive_current_page(empty, 0, 500) == -1);
}

TEST_CASE("StripScroll: navigation pins until scroll input", "[ui][strip]") {
    const StripLayout l = ten_pages();
    const double vp_h = 500;

    CurrentPageTracker t;
    CHECK(t.current() == 0);
    CHECK_FALSE(t.pinned());

    // At anchor(5) derivation says 5; the navigation target 9 wins while pinned.
    const double mid = l.anchor(5);
    REQUIRE(derive_current_page(l, mid, vp_h) == 5);
    t.navigate(9);
    CHECK(t.pinned());
    CHECK(t.recompute(l, mid, vp_h) == 9);
    // A re-layout (another recompute) still holds the pin.
    const StripLayout bigger = ten_pages(2.0);
    CHECK(t.recompute(bigger, 0, vp_h) == 9);
    CHECK(t.current() == 9);

    t.scroll_input();
    CHECK_FALSE(t.pinned());
    CHECK(t.recompute(l, mid, vp_h) == 5);
    CHECK(t.current() == 5);

    t.navigate(4);
    t.reset();
    CHECK(t.current() == 0);
    CHECK_FALSE(t.pinned());
}

TEST_CASE("StripScroll: desired pan_x survives a wide page leaving view", "[ui][strip]") {
    const double vp_w = 600;
    CHECK(effective_pan_x(-300, 1000, vp_w) == -300.0);
    // The wide page is gone: widest 500 fits, effective 0. The caller keeps
    // `desired` untouched, so it is back when the wide page returns.
    const double desired = -300;
    CHECK(effective_pan_x(desired, 500, vp_w) == 0.0);
    CHECK(effective_pan_x(desired, 1000, vp_w) == -300.0);
    // Clamped at the far edge.
    CHECK(effective_pan_x(-900, 1000, vp_w) == -400.0);
    CHECK(effective_pan_x(50, 1000, vp_w) == 0.0);

    CHECK(apply_pan_x_delta(-300, -500, 1000, vp_w) == -400.0);
    CHECK(apply_pan_x_delta(-300, 100, 1000, vp_w) == -200.0);
    // The delta applies to the effective pan, not the stale desired one.
    CHECK(apply_pan_x_delta(-900, 100, 1000, vp_w) == -300.0);
}

TEST_CASE("StripScroll: page_x centres a narrow page and pans a wide one", "[ui][strip]") {
    const double vp_w = 600, widest = 1000, pan = -200;
    CHECK(page_x(400, widest, pan, vp_w) == 100.0);
    CHECK(page_x(1000, widest, pan, vp_w) == -200.0);
    CHECK(page_x(800, widest, pan, vp_w) == -100.0);
    CHECK(page_x(600, widest, pan, vp_w) == 0.0);   // exactly fits: centred
}
