#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

#include "ui/detail/StripScroll.hpp"
#include "ui/detail/StripViewport.hpp"

using Catch::Approx;
using namespace litepdf::ui;

// #55: DocumentView's FitWidth zoom is a float (fit_percentage), while the strip
// works in double. At 638 px / 192 dpi the viewport is 319 DIP and FitWidth's
// zoom is float(319 / 612), so a 612 pt page comes out ~8e-6 DIP wider than the
// viewport. Such a page must still count as fitting: centred, no pan.

namespace {

// The zoom FitWidth computes for a 612 pt page in a 319 DIP viewport.
float fit_zoom() { return 319.0f / 612.0f; }

}  // namespace

TEST_CASE("StripScroll: a page fitted by a float zoom counts as fitting", "[ui][strip]") {
    const float z = fit_zoom();
    const double w = 612.0 * z;
    REQUIRE(w > 319.0);  // the float noise is real

    // Centred, not placed at the pan plus (widest - w) / 2.
    CHECK(page_x(w, 792.0 * z, 0.0, 319.0) == Approx((319.0 - w) / 2).margin(1e-9));
    // Nothing to pan when the widest page is the fitted one.
    CHECK(effective_pan_x(-10.0, w, 319.0) == 0.0);
}

TEST_CASE("StripViewport: a fitted page stays centred beside a wider page", "[ui][strip]") {
    const float z = fit_zoom();
    StripViewport v;
    v.set_viewport(319.0, 300.0);
    v.build({{612, 792}, {792, 612}}, z);
    // Page 0 spans [8, 8 + 792z] ~ [8, 420.8], page 1 starts at ~428.8: at
    // scroll 300 both are on screen, so the landscape page sets the widest.
    v.scroll_to(300.0);
    REQUIRE(v.visible().first == 0);
    REQUIRE(v.visible().last == 1);

    const StripPageRect r = v.page_rect(0);
    CHECK(r.x == Approx(0.0).margin(1e-3));
}

TEST_CASE("StripViewport: a fitted page that fits vertically leaves nothing to pan",
          "[ui][strip]") {
    const float z = fit_zoom();
    StripViewport v;
    v.set_viewport(319.0, 500.0);
    v.build({{612, 792}}, z);  // total 8 + 792z + 8 ~ 428.8 < 500
    CHECK_FALSE(v.can_pan());
}
