#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "core/detail/ZoomMath.hpp"

using litepdf::core::clamp_preset_span;
using litepdf::core::fit_percentage;
using litepdf::core::next_preset;
using litepdf::core::prev_preset;

TEST_CASE("ZoomMath fit percentage derives dips per point from viewport pixels",
          "[core][zoom][math]") {
    // A4 is 595.276 x 841.89 pt. At 96 dpi one DIP is one device pixel, so a
    // 1190.552 px viewport is exactly 2.0x fit-width.
    REQUIRE(fit_percentage(1190.552f, 800.0f, 96.0f, 595.276f, 841.89f, false)
            == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit percentage applies dpi exactly once", "[core][zoom][math]") {
    // The SAME physical viewport at 192 dpi: each DIP is two device pixels, so
    // the percentage halves while the resulting pixmap width in pixels is
    // unchanged. The shipped code applied dpi here AND at every caller, which
    // rendered pixmaps twice as wide as the canvas at 200% scaling.
    const float pct = fit_percentage(1190.552f, 800.0f, 192.0f,
                                     595.276f, 841.89f, false);
    REQUIRE(pct == Catch::Approx(1.0f).epsilon(0.001));
    const float render_scale = pct * (192.0f / 96.0f);
    REQUIRE(595.276f * render_scale == Catch::Approx(1190.552f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit percentage at 144 dpi", "[core][zoom][math]") {
    // The middle rung of the spec's 96 / 144 / 192 sweep. At 150% scaling a
    // 1785.66 px viewport is 1190.44 DIP, so the same 2.0x as the 96 dpi case
    // -- and the pixmap comes out 1785.66 px, matching the canvas exactly.
    const float pct = fit_percentage(1785.66f, 1200.0f, 144.0f,
                                     595.22f, 842.0f, false);
    REQUIRE(pct == Catch::Approx(2.0f).epsilon(0.001));
    REQUIRE(595.22f * pct * (144.0f / 96.0f)
            == Catch::Approx(1785.66f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit page takes the smaller of the two fits", "[core][zoom][math]") {
    // Width fits at 2.0x, height only at 1.0x -> fit-page picks 1.0x.
    REQUIRE(fit_percentage(1190.552f, 841.89f, 96.0f, 595.276f, 841.89f, true)
            == Catch::Approx(1.0f).epsilon(0.001));
    REQUIRE(fit_percentage(1190.552f, 841.89f, 96.0f, 595.276f, 841.89f, false)
            == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("ZoomMath fit percentage for an unequal spread fits the larger page",
          "[core][zoom][math]") {
    // A spread of unequal pages -- legal in PDF, common in scanned books with
    // an inserted plate -- shares one render scale across both slots. Feeding
    // the pair's max extent is what keeps the larger page inside its slot;
    // deriving from the left page alone would overflow it.
    const float slot_px = 600.0f;
    const float left_w = 595.276f,  left_h = 841.89f;
    const float right_w = 1200.0f,  right_h = 2000.0f;
    const float pair_w = (left_w > right_w) ? left_w : right_w;
    const float pair_h = (left_h > right_h) ? left_h : right_h;

    const float pct = fit_percentage(slot_px, 900.0f, 96.0f, pair_w, pair_h, true);
    // Both pages fit inside the slot at that one percentage.
    REQUIRE(left_w  * pct <= Catch::Approx(slot_px));
    REQUIRE(right_w * pct <= Catch::Approx(slot_px));
    REQUIRE(left_h  * pct <= Catch::Approx(900.0f));
    REQUIRE(right_h * pct <= Catch::Approx(900.0f));
}

TEST_CASE("ZoomMath fit percentage tolerates degenerate inputs", "[core][zoom][math]") {
    REQUIRE(fit_percentage(1000.0f, 800.0f, 0.0f,  595.0f, 842.0f, false) > 0.0f);
    REQUIRE(fit_percentage(1000.0f, 800.0f, 96.0f, 0.0f,   842.0f, false)
            == Catch::Approx(1.0f));
    REQUIRE(fit_percentage(0.0f,    0.0f,   96.0f, 595.0f, 842.0f, false)
            == Catch::Approx(0.0f));
}

TEST_CASE("ZoomMath preset ladder steps to the next rung strictly above or below",
          "[core][zoom][math]") {
    REQUIRE(next_preset(1.0f)  == Catch::Approx(1.25f));
    REQUIRE(prev_preset(1.0f)  == Catch::Approx(0.75f));
    // At the ends the ladder returns the input unchanged, which is how
    // zoom_in()/zoom_out() report "no change".
    REQUIRE(next_preset(8.0f)  == Catch::Approx(8.0f));
    REQUIRE(prev_preset(0.25f) == Catch::Approx(0.25f));
    // A fit-derived percentage above the shipped 4.0 ceiling must still find a
    // larger rung. This exact state is what made zoom_in() a permanent no-op.
    REQUIRE(next_preset(5.2f)  == Catch::Approx(6.0f));
    REQUIRE(prev_preset(5.2f)  == Catch::Approx(4.0f));
    // And between rungs in the middle of the table.
    REQUIRE(next_preset(1.1f)  == Catch::Approx(1.25f));
    REQUIRE(prev_preset(1.1f)  == Catch::Approx(1.0f));
}

TEST_CASE("ZoomMath clamp preset span pins values into the table range",
          "[core][zoom][math]") {
    REQUIRE(clamp_preset_span(99.0f) == Catch::Approx(8.0f));
    REQUIRE(clamp_preset_span(0.01f) == Catch::Approx(0.25f));
    REQUIRE(clamp_preset_span(1.5f)  == Catch::Approx(1.5f));
    // NaN-safe: the clamp contract is total, so NaN pins low rather than
    // propagating into zoom_pct_. Coverage carried over from the
    // set_zoom_scale case this task supersedes.
    REQUIRE(clamp_preset_span(std::numeric_limits<float>::quiet_NaN())
            == Catch::Approx(0.25f));
}
