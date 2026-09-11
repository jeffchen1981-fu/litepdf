// PR-A2 Task 7: pure-logic tests for wheel scrolling.
//
// pan_y lives in [vp_h - content_h, 0] with 0 at the page top (the top-left
// origin PR-A1 introduced). Wheel UP is delta > 0, step > 0, and moves pan_y
// toward 0.

#include "ui/detail/ScrollMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using litepdf::ui::apply_wheel;
using litepdf::ui::consume_notches;
using litepdf::ui::Flip;
using litepdf::ui::wheel_step_dip;

TEST_CASE("ScrollMath consumes one whole notch and keeps no residual",
          "[ui][scroll]") {
    int residual = 0;
    REQUIRE(consume_notches(120, residual) == 1);
    REQUIRE(residual == 0);
    REQUIRE(consume_notches(-120, residual) == -1);
    REQUIRE(residual == 0);
}

TEST_CASE("ScrollMath accumulates high-resolution wheel deltas",
          "[ui][scroll]") {
    // A precision wheel delivers |delta| < WHEEL_DELTA. No notch fires until
    // the accumulated total crosses 120, and the remainder carries forward.
    int residual = 0;
    REQUIRE(consume_notches(40, residual) == 0);
    REQUIRE(residual == 40);
    REQUIRE(consume_notches(40, residual) == 0);
    REQUIRE(residual == 80);
    REQUIRE(consume_notches(50, residual) == 1);
    REQUIRE(residual == 10);
}

TEST_CASE("ScrollMath accumulates negative deltas symmetrically",
          "[ui][scroll]") {
    int residual = 0;
    REQUIRE(consume_notches(-50, residual) == 0);
    REQUIRE(residual == -50);
    REQUIRE(consume_notches(-100, residual) == -1);
    REQUIRE(residual == -30);
}

TEST_CASE("ScrollMath cancels an accumulated residual on a reversal",
          "[ui][scroll]") {
    // The residual keeps the SIGN of the motion, so a delta in the opposite
    // direction reduces the pending accumulation instead of compounding it.
    // Without that, flicking the wheel back and forth below one notch would
    // eventually fire a notch in a direction the reader never sustained.
    int residual = 0;
    REQUIRE(consume_notches(80, residual) == 0);
    REQUIRE(residual == 80);
    // Reversal: 80 - 30, not 80 + 30.
    REQUIRE(consume_notches(-30, residual) == 0);
    REQUIRE(residual == 50);
    // Crossing zero is still just accumulation; no whole notch yet.
    REQUIRE(consume_notches(-100, residual) == 0);
    REQUIRE(residual == -50);
    // And the notch fires only once the accumulated total reaches -120.
    REQUIRE(consume_notches(-70, residual) == -1);
    REQUIRE(residual == 0);
}

TEST_CASE("ScrollMath consumes several notches from one fat delta",
          "[ui][scroll]") {
    int residual = 0;
    REQUIRE(consume_notches(360, residual) == 3);
    REQUIRE(residual == 0);
}

TEST_CASE("ScrollMath step is lines times the line height", "[ui][scroll]") {
    REQUIRE(wheel_step_dip(1, 3, 800.0f)  == Catch::Approx(48.0f));
    REQUIRE(wheel_step_dip(-1, 3, 800.0f) == Catch::Approx(-48.0f));
    REQUIRE(wheel_step_dip(2, 3, 800.0f)  == Catch::Approx(96.0f));
}

TEST_CASE("ScrollMath page-scroll sentinel degrades to most of the viewport",
          "[ui][scroll]") {
    // WHEEL_PAGESCROLL is UINT_MAX. A full viewport height would land exactly
    // on the next edge and flip on the following notch; 90% leaves an overlap.
    REQUIRE(wheel_step_dip(1, 0xFFFFFFFFu, 800.0f) == Catch::Approx(720.0f));
}

TEST_CASE("ScrollMath honours a zero-lines setting as no scrolling",
          "[ui][scroll]") {
    REQUIRE(wheel_step_dip(1, 0, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(wheel_step_dip(0, 3, 800.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ScrollMath scrolls within an overflowing page", "[ui][scroll]") {
    // content 2000, viewport 800 -> pan range [-1200, 0].
    const auto down = apply_wheel(-100.0f, 2000.0f, 800.0f, -48.0f);
    REQUIRE(down.flip == Flip::None);
    REQUIRE(down.pan_y == Catch::Approx(-148.0f));

    const auto up = apply_wheel(-100.0f, 2000.0f, 800.0f, 48.0f);
    REQUIRE(up.flip == Flip::None);
    REQUIRE(up.pan_y == Catch::Approx(-52.0f));
}

TEST_CASE("ScrollMath clamps at an edge before it flips", "[ui][scroll]") {
    // A step that merely REACHES the edge scrolls; the flip is the next notch.
    const auto reach = apply_wheel(-1180.0f, 2000.0f, 800.0f, -48.0f);
    REQUIRE(reach.flip == Flip::None);
    REQUIRE(reach.pan_y == Catch::Approx(-1200.0f));

    const auto flip = apply_wheel(-1200.0f, 2000.0f, 800.0f, -48.0f);
    REQUIRE(flip.flip == Flip::Next);
    REQUIRE(flip.pan_y == Catch::Approx(-1200.0f));   // unchanged
}

TEST_CASE("ScrollMath flips backward at the top edge", "[ui][scroll]") {
    const auto reach = apply_wheel(-20.0f, 2000.0f, 800.0f, 48.0f);
    REQUIRE(reach.flip == Flip::None);
    REQUIRE(reach.pan_y == Catch::Approx(0.0f));

    const auto flip = apply_wheel(0.0f, 2000.0f, 800.0f, 48.0f);
    REQUIRE(flip.flip == Flip::Prev);
    REQUIRE(flip.pan_y == Catch::Approx(0.0f));
}

TEST_CASE("ScrollMath flips immediately when the page fits", "[ui][scroll]") {
    // Nothing to scroll: one notch is one page.
    const auto next = apply_wheel(0.0f, 600.0f, 800.0f, -48.0f);
    REQUIRE(next.flip == Flip::Next);
    const auto prev = apply_wheel(0.0f, 600.0f, 800.0f, 48.0f);
    REQUIRE(prev.flip == Flip::Prev);
}

TEST_CASE("ScrollMath does nothing for a zero step", "[ui][scroll]") {
    const auto r = apply_wheel(-100.0f, 2000.0f, 800.0f, 0.0f);
    REQUIRE(r.flip == Flip::None);
    REQUIRE(r.pan_y == Catch::Approx(-100.0f));
}
