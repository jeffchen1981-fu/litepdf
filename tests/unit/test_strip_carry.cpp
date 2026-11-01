#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

#include "ui/detail/StripCarry.hpp"
#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripScroll.hpp"

using Catch::Approx;
using namespace litepdf::ui;

namespace {

// Ten pages of 100 x 1000 pt at 100 %: tall enough that nothing below clamps.
StripLayout tall_strip() {
    return StripLayout(std::vector<StripPageSize>(10, StripPageSize{100, 1000}), 1.0);
}

}  // namespace

TEST_CASE("StripCarry: a fitting paged page carries offset zero", "[ui][strip]") {
    const CarriedPosition c = carry_from_paged_single(4, 0, 37.5, 300);
    CHECK(c.page == 4);
    CHECK(c.offset_dip == 0.0);

    // The same place navigation puts page 4.
    const StripLayout l = tall_strip();
    CHECK(strip_scroll_for(l, c, 600) == Approx(l.anchor(4)));
}

TEST_CASE("StripCarry: an overflowing paged page carries how far it is scrolled",
          "[ui][strip]") {
    const CarriedPosition c = carry_from_paged_single(2, -250, 0, 1000);
    CHECK(c.page == 2);
    CHECK(c.offset_dip == Approx(250.0));

    // Into a strip: measured from the page's top, not its anchor (not 242).
    const StripLayout l = tall_strip();
    const double scroll = strip_scroll_for(l, c, 600);
    CHECK(scroll - l.top(2) == Approx(250.0));

    // And back out again.
    const CarriedPosition back = carry_from_strip(l, 2, scroll);
    CHECK(back.page == 2);
    CHECK(back.offset_dip == Approx(250.0));
    CHECK(paged_pan_y_for(back, 1000, 600) == Approx(-250.0f));
}

TEST_CASE("StripCarry: a spread with unequal heights is measured on the left page",
          "[ui][strip]") {
    // The left page's top sits 40 DIP inside the union; scrolled up 140 DIP.
    const SpreadVertical sv{40, 0, 800};
    const CarriedPosition c = carry_from_spread(6, 7, -140, sv, 500, 800);
    CHECK(c.page == 6);
    CHECK(c.offset_dip == Approx(100.0));
    CHECK(spread_pan_y_for(c, 40, 800, 600) == Approx(-140.0f));
}

TEST_CASE("StripCarry: a spread past its left page is measured on the right page",
          "[ui][strip]") {
    // The left page spans 40 to 540 of the union, the right one all of it.
    const SpreadVertical sv{40, 0, 800};

    CHECK(carry_from_spread(6, 7, -539, sv, 500, 800).page == 6);

    // At the left page's bottom nothing of it is left on screen.
    const CarriedPosition at_bottom = carry_from_spread(6, 7, -540, sv, 500, 800);
    CHECK(at_bottom.page == 7);
    CHECK(at_bottom.offset_dip == Approx(540.0));

    // Back in: the same pan, from the right page's own top.
    const CarriedPosition past = carry_from_spread(6, 7, -700, sv, 500, 800);
    CHECK(past.page == 7);
    CHECK(past.offset_dip == Approx(700.0));
    CHECK(spread_pan_y_for(past, sv.right_top_in_union, 800, 100) == Approx(-700.0f));

    // A pan past the union stops at the right page's bottom.
    CHECK(carry_from_spread(6, 7, -5000, sv, 500, 800).offset_dip == Approx(800.0));
}

TEST_CASE("StripCarry: a spread with no right page stays on its left page", "[ui][strip]") {
    const SpreadVertical sv{0, 0, 500};
    const CarriedPosition c = carry_from_spread(0, -1, -5000, sv, 500, 0);
    CHECK(c.page == 0);
    CHECK(c.offset_dip == Approx(500.0));
}

TEST_CASE("StripCarry: a right page's own offset does not start at the left page's top",
          "[ui][strip]") {
    // Left page first and short, the right page centred below where it ends: a
    // pan cannot reach past the left page before the right page starts, so the
    // offset stops at 0.
    const SpreadVertical sv{0, 400, 1000};
    const CarriedPosition c = carry_from_spread(2, 3, -300, sv, 300, 200);
    CHECK(c.page == 3);
    CHECK(c.offset_dip == 0.0);
}

TEST_CASE("StripCarry: offsets clamp to the page", "[ui][strip]") {
    const StripLayout l = tall_strip();

    // Inside the gap after page 3: the whole page is above the viewport top.
    const double in_gap = l.top(3) + l.height(3) + 4.0;
    CHECK(carry_from_strip(l, 3, in_gap).offset_dip == Approx(l.height(3)));

    // Above the page's top: nothing of it is scrolled past.
    CHECK(carry_from_strip(l, 3, l.top(3) - 5.0).offset_dip == 0.0);

    // The paged forms clamp the same way.
    CHECK(carry_from_paged_single(0, -5000, 0, 1000).offset_dip == Approx(1000.0));
    CHECK(carry_from_spread(0, -1, 30, SpreadVertical{40, 0, 500}, 500, 0).offset_dip == 0.0);
}

TEST_CASE("StripCarry: the carried scroll and pan clamp to what they land in",
          "[ui][strip]") {
    // Three 100 pt pages: total 332, vp 300, max scroll 32. top(2) + 50 = 274.
    const StripLayout short_strip(std::vector<StripPageSize>(3, StripPageSize{100, 100}), 1.0);
    CHECK(strip_scroll_for(short_strip, {2, 50}, 300) == Approx(32.0));

    // The whole page scrolled past: the paged pan stops at the page's bottom.
    CHECK(paged_pan_y_for({0, 1000}, 1000, 600) == Approx(-400.0f));
    // The spread pan stops at the union's bottom.
    CHECK(spread_pan_y_for({0, 500}, 40, 800, 600) == Approx(-200.0f));
}
