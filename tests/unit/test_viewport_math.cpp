#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ui/detail/ViewportMath.hpp"

using litepdf::ui::bitmap_px_to_dip;
using litepdf::ui::clamp_pan;
using litepdf::ui::place_bitmap;

TEST_CASE("ViewportMath bitmap px to dip divides by the render target dpi ratio",
          "[ui][viewport]") {
    REQUIRE(bitmap_px_to_dip(1000.0f, 96.0f)  == Catch::Approx(1000.0f));
    REQUIRE(bitmap_px_to_dip(1000.0f, 144.0f) == Catch::Approx(666.6667f).epsilon(0.001));
    REQUIRE(bitmap_px_to_dip(1000.0f, 192.0f) == Catch::Approx(500.0f));
    // Defensive: a zero or negative dpi must not divide by zero.
    REQUIRE(bitmap_px_to_dip(1000.0f, 0.0f)   == Catch::Approx(1000.0f));
}

TEST_CASE("ViewportMath clamp pan centers content that fits", "[ui][viewport]") {
    // Content no larger than the viewport is centered by place_bitmap, so its
    // pan is pinned to zero regardless of what the caller asks for.
    REQUIRE(clamp_pan(  0.0f, 400.0f, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(clamp_pan(-250.0f, 400.0f, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(clamp_pan( 250.0f, 800.0f, 800.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ViewportMath clamp pan uses a top left origin when content overflows",
          "[ui][viewport]") {
    // content 1600, viewport 800 -> valid range [-800, 0].
    REQUIRE(clamp_pan(   0.0f, 1600.0f, 800.0f) == Catch::Approx(0.0f));
    REQUIRE(clamp_pan(-400.0f, 1600.0f, 800.0f) == Catch::Approx(-400.0f));
    REQUIRE(clamp_pan(-800.0f, 1600.0f, 800.0f) == Catch::Approx(-800.0f));
    REQUIRE(clamp_pan(-999.0f, 1600.0f, 800.0f) == Catch::Approx(-800.0f));
    REQUIRE(clamp_pan( 120.0f, 1600.0f, 800.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ViewportMath place bitmap keeps natural size for every viewport",
          "[ui][viewport]") {
    // This is the direct regression assertion for the shipped shrink-to-fit
    // defect: the destination extent must equal the source extent whether the
    // viewport is larger, smaller, or equal.
    const auto bigger  = place_bitmap(400.0f, 500.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(bigger.w  == Catch::Approx(400.0f));
    REQUIRE(bigger.h  == Catch::Approx(500.0f));

    const auto smaller = place_bitmap(2000.0f, 3000.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(smaller.w == Catch::Approx(2000.0f));
    REQUIRE(smaller.h == Catch::Approx(3000.0f));

    const auto equal   = place_bitmap(1000.0f, 900.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(equal.w   == Catch::Approx(1000.0f));
    REQUIRE(equal.h   == Catch::Approx(900.0f));
}

TEST_CASE("ViewportMath place bitmap centers a fitting axis and top aligns an overflowing one",
          "[ui][viewport]") {
    // Width fits (400 <= 1000) -> centered at (1000-400)/2 = 300.
    // Height overflows (3000 > 900) -> top-left origin, pan applied and clamped.
    const auto p = place_bitmap(400.0f, 3000.0f, 1000.0f, 900.0f, 55.0f, -500.0f);
    REQUIRE(p.x == Catch::Approx(300.0f));
    REQUIRE(p.y == Catch::Approx(-500.0f));

    // Pan zero on the overflowing axis means the content top sits at the
    // viewport top -- not centered, which is what the old origin would give.
    const auto top = place_bitmap(400.0f, 3000.0f, 1000.0f, 900.0f, 0.0f, 0.0f);
    REQUIRE(top.y == Catch::Approx(0.0f));

    // Bottom-aligned: pan = viewport - content.
    const auto bottom = place_bitmap(400.0f, 3000.0f, 1000.0f, 900.0f, 0.0f, -2100.0f);
    REQUIRE(bottom.y == Catch::Approx(-2100.0f));
    REQUIRE(bottom.y + bottom.h == Catch::Approx(900.0f));
}

TEST_CASE("ViewportMath pdf point to dip mapping is dpi invariant", "[ui][viewport]") {
    using litepdf::ui::pdf_point_to_dip;
    // The search-hit overlay draws in DIPs. One PDF point is exactly zoom_pct
    // DIPs -- the pixmap is page_pt * render_scale PIXELS, and converting that
    // to DIPs divides the dpi factor back out. So the mapping must not mention
    // dpi at all, and this asserts the formula stayed that way: the shipped
    // code multiplied by a render scale, and an earlier draft of the spec
    // prescribed doing so again, which would double every hit rectangle at
    // 200% scaling.
    REQUIRE(pdf_point_to_dip(100.0f, 1.5f) == Catch::Approx(150.0f));
    REQUIRE(pdf_point_to_dip(100.0f, 1.0f) == Catch::Approx(100.0f));
    // What this CANNOT test, stated plainly so nobody mistakes it for covered:
    // the regression that matters is a caller in PdfCanvas reaching for
    // render_scale() instead of zoom_pct(). This function's signature has no
    // dpi parameter, so no test of it can observe that choice. The guard for
    // the caller is the Task 7 GUI check, item 0.
    //
    // What this DOES pin: the mapping is linear in the percentage and has no
    // hidden dpi term of its own.
    REQUIRE(pdf_point_to_dip(72.0f, 2.0f)
            == Catch::Approx(2.0f * pdf_point_to_dip(72.0f, 1.0f)));
}
