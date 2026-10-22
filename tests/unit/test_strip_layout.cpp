#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

#include "ui/detail/StripLayout.hpp"

using Catch::Approx;
using litepdf::ui::StripLayout;
using litepdf::ui::StripPageSize;
using litepdf::ui::StripPos;
using litepdf::ui::StripRange;

namespace {

StripLayout three_pages(double zoom) {
    return StripLayout({{100, 200}, {50, 100}, {100, 300}}, zoom);
}

}  // namespace

TEST_CASE("StripLayout: tops follow the gap and prefix sums", "[ui][strip]") {
    const StripLayout l = three_pages(1.0);
    CHECK(l.page_count() == 3);
    CHECK(l.top(0) == 8.0);
    CHECK(l.top(1) == 8.0 + 200.0 + 8.0);
    CHECK(l.top(1) == 216.0);
    CHECK(l.top(2) == 216.0 + 100.0 + 8.0);
    CHECK(l.top(2) == 324.0);
    CHECK(l.total_height() == 324.0 + 300.0 + 8.0);
    CHECK(l.total_height() == 632.0);
    CHECK(l.anchor(0) == 0.0);
    CHECK(l.anchor(1) == 208.0);
}

TEST_CASE("StripLayout: zoom scales heights not the gap", "[ui][strip]") {
    const StripLayout l = three_pages(2.0);
    CHECK(l.zoom_pct() == 2.0);
    CHECK(l.height(0) == 400.0);
    CHECK(l.width(0) == 200.0);
    CHECK(l.top(1) == 8.0 + 400.0 + 8.0);
    CHECK(l.top(1) == 416.0);
}

TEST_CASE("StripLayout: visible range at boundaries", "[ui][strip]") {
    const StripLayout l = three_pages(1.0);

    CHECK(l.visible(0, 8).empty());          // the top gap
    const StripRange a = l.visible(0, 9);
    CHECK(a.first == 0);
    CHECK(a.last == 0);

    CHECK(l.visible(208, 216).empty());      // the gap after page 0

    const StripRange b = l.visible(207, 217);
    CHECK(b.first == 0);
    CHECK(b.last == 1);

    CHECK(l.visible(631, 632).empty());      // the bottom gap

    const StripRange c = l.visible(0, 632);
    CHECK(c.first == 0);
    CHECK(c.last == 2);
    CHECK(c.contains(0));
    CHECK(c.contains(2));
    CHECK_FALSE(c.contains(3));
    CHECK_FALSE(c.contains(-1));
}

TEST_CASE("StripLayout: position_at and y_of round-trip in pages and gaps",
          "[ui][strip]") {
    const StripLayout l = three_pages(1.0);

    for (double y : {0.0, 4.0, 8.0, 100.0, 208.0, 212.0, 216.0, 623.9, 628.0}) {
        const StripPos p = l.position_at(y);
        CHECK(l.y_of(p) == Approx(y).margin(1e-9));
        CHECK(p.fraction >= 0.0);
        CHECK(p.fraction < 1.0);
    }

    const StripPos gap_top = l.position_at(4);
    CHECK(gap_top.kind == StripPos::Kind::InGapAfter);
    CHECK(gap_top.page == -1);
    CHECK(gap_top.fraction == Approx(0.5));

    const StripPos gap_mid = l.position_at(212);
    CHECK(gap_mid.kind == StripPos::Kind::InGapAfter);
    CHECK(gap_mid.page == 0);
    CHECK(gap_mid.fraction == Approx(0.5));

    const StripPos in_page = l.position_at(108);
    CHECK(in_page.kind == StripPos::Kind::InPage);
    CHECK(in_page.page == 0);
    CHECK(in_page.fraction == Approx(0.5));

    // A page's first DIP is in the page; its last gap DIP is in the gap.
    CHECK(l.position_at(8).kind == StripPos::Kind::InPage);
    CHECK(l.position_at(208).kind == StripPos::Kind::InGapAfter);
    CHECK(l.position_at(208).page == 0);
}

TEST_CASE("StripLayout: widest of a range", "[ui][strip]") {
    const StripLayout l = three_pages(1.0);
    CHECK(l.widest({1, 1}) == 50.0);
    CHECK(l.widest({0, 2}) == 100.0);
    CHECK(l.widest({}) == 0.0);
}

TEST_CASE("StripLayout: double precision at 10000 pages and 800 percent",
          "[ui][strip]") {
    const StripLayout l(std::vector<StripPageSize>(10000, StripPageSize{595.0, 842.5}),
                        8.0);
    REQUIRE(l.page_count() == 10000);
    CHECK(l.height(0) == 6740.0);
    CHECK(l.top(9999) == 67473260.0);
    CHECK(l.top(9999) - l.top(9998) == 6748.0);

    const double y = l.top(9999) + 1.0;
    const StripPos p = l.position_at(y);
    CHECK(p.kind == StripPos::Kind::InPage);
    CHECK(p.page == 9999);
    CHECK(p.fraction > 0.0);
    CHECK(l.y_of(p) == y);

    // Control (a): the same 10,000 steps accumulated in float lose the step.
    // 842.5 pt makes the sums non-multiples of float's spacing; with 842 pt
    // every sum is a multiple of 8 and exact in float, which would blind this.
    float prev_f = 8.0f;
    float top_f = 8.0f;
    for (int i = 1; i < 10000; ++i) {
        prev_f = top_f;
        top_f += 6740.0f + 8.0f;
    }
    CHECK(top_f - prev_f != 6748.0f);

    // Control (b): at 67 M a float step is 8, so a 1 DIP step is lost.
    CHECK(static_cast<float>(l.top(9999)) + 1.0f == static_cast<float>(l.top(9999)));
}

TEST_CASE("StripLayout: zero pages", "[ui][strip]") {
    const StripLayout l;
    CHECK(l.page_count() == 0);
    CHECK(l.total_height() == 0.0);
    CHECK(l.visible(0, 100).empty());
    CHECK(l.widest({0, 5}) == 0.0);
}
