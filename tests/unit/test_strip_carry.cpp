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
    const CarriedPosition c = carry_from_spread(6, -140, 40, 500);
    CHECK(c.page == 6);
    CHECK(c.offset_dip == Approx(100.0));
    CHECK(spread_pan_y_for(c, 40, 800, 600) == Approx(-140.0f));
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
    CHECK(carry_from_spread(0, 30, 40, 500).offset_dip == 0.0);
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
