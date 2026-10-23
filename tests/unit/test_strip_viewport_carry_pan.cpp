#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "ui/detail/StripViewport.hpp"

using namespace litepdf::ui;

// #55 PR 2 Task 8: a carry into the strip arrives with the outgoing layout's
// horizontal pan (spec 7.3). It is stored as the desired pan, and only the
// effective pan clamps it, so a page that becomes wider later can reach it.
TEST_CASE("StripViewport: set_desired_pan_x keeps the value and the pin") {
    StripViewport v;
    v.set_viewport(300, 150);
    // Two 400 x 100 pages at zoom 1: wider than the 300 DIP viewport, so the
    // effective pan ranges over [-100, 0].
    v.build(std::vector<StripPageSize>(2, {400, 100}), 1.0);
    v.apply_carry(CarriedPosition{1, 20});
    REQUIRE(v.pinned());

    v.set_desired_pan_x(-60);
    CHECK(v.pan_x_eff() == -60);
    CHECK(v.pinned());               // not an input: the carried page stays pinned
    CHECK(v.save().desired_pan_x == -60);

    v.set_desired_pan_x(-250);       // past the range: clamped when read
    CHECK(v.pan_x_eff() == -100);
    CHECK(v.save().desired_pan_x == -250);
}
