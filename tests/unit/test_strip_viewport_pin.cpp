#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "ui/detail/StripViewport.hpp"

using namespace litepdf::ui;

// #55 PR 2 review, ruling 11: the navigation pin ends at the next VERTICAL
// scroll input only (spec 4.6). Horizontal input cannot change which page is
// on screen, so it keeps the pin.
TEST_CASE("StripViewport: horizontal input keeps the navigation pin") {
    StripViewport v;
    v.set_viewport(60, 150);
    // Ten 100 x 100 pages at zoom 1: wider than the 60 DIP viewport, so there
    // is something to pan horizontally.
    v.build(std::vector<StripPageSize>(10, {100, 100}), 1.0);
    v.navigate(3);
    REQUIRE(v.pinned());
    REQUIRE(v.current_page() == 3);

    CHECK(v.pan_x_by(-10));          // the pan really moved
    CHECK(v.pan_x_eff() == -10);
    CHECK(v.pinned());
    CHECK(v.recompute_current() == 3);

    CHECK(v.key(StripKey::Right));
    CHECK(v.pinned());

    v.scroll_by(10);                 // vertical input ends the pin
    CHECK_FALSE(v.pinned());
}
