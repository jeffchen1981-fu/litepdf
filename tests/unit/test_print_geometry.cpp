// tests/unit/test_print_geometry.cpp -- Phase 8.5 Task 1
#include "printing/PrintGeometry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace litepdf::printing;
using Catch::Matchers::WithinAbs;

// --------------------------------------------------------------------
// Unit-contract reminder (see spec section 5 and PrintGeometry.hpp comment):
//   page_rect_pt  -> MuPDF points  (fz_bound_page output, 1 pt = 1/72 inch)
//   paper_rect_px -> printer pixels (GetDeviceCaps HORZRES/VERTRES output)
//   dpi           -> GetDeviceCaps LOGPIXELSX/Y
//   Returned scale_x/scale_y are in px/pt (feed directly into fz_scale()).
//   Centering tx/ty are in printer pixels.
// --------------------------------------------------------------------

namespace {
// A4: 210 mm x 297 mm = 595.28 pt x 841.89 pt (rounded to 595 x 842 below).
// At 300 DPI: 210/25.4*300 = 2480.3 -> 2480 px, 297/25.4*300 = 3507.9 -> 3508 px.
constexpr Rect a4_portrait_pt   {0, 0, 595.0f, 842.0f};   // page rect in points
constexpr Rect a4_landscape_pt  {0, 0, 842.0f, 595.0f};   // landscape page, points
constexpr Rect a4_paper_300dpi  {0, 0, 2480.0f, 3508.0f}; // paper rect in printer px
constexpr float dpi = 300.0f;  // matches a4_paper_300dpi

// Fit scale for A4-portrait on A4-paper@300DPI:
//   s = min(2480/595, 3508/842) = min(4.168..., 4.166...) ~ 4.166 px/pt
//   (tighter bound is the height axis)
constexpr float fit_scale_a4 = 3508.0f / 842.0f;  // ~4.1663 px/pt
}

TEST_CASE("fit-to-page on portrait page + portrait paper", "[print-geometry]") {
    auto m = compute_render_matrix(
        a4_portrait_pt, a4_paper_300dpi,
        ScaleMode::FitToPage, /*custom_pct*/0.0f, /*auto_rotate*/true, dpi, dpi);
    REQUIRE_THAT(m.scale_x, WithinAbs(4.166f, 0.02f));
    REQUIRE_THAT(m.scale_y, WithinAbs(4.166f, 0.02f));
    REQUIRE(m.rotate_90 == false);
    // A4 paper matches A4 page at fit scale -- centering offsets near zero.
    REQUIRE_THAT(m.tx, WithinAbs(0.0f, 2.0f));
    REQUIRE_THAT(m.ty, WithinAbs(0.0f, 2.0f));
}

TEST_CASE("auto-rotate engages on landscape page + portrait paper", "[print-geometry]") {
    auto m = compute_render_matrix(
        a4_landscape_pt, a4_paper_300dpi,
        ScaleMode::FitToPage, 0.0f, true, dpi, dpi);
    REQUIRE(m.rotate_90 == true);
    // After rotation effective page is 595 x 842 pt -- same as a4_portrait_pt.
    REQUIRE_THAT(m.scale_x, WithinAbs(4.166f, 0.02f));
}

TEST_CASE("auto-rotate suppressed when flag is off", "[print-geometry]") {
    auto m = compute_render_matrix(
        a4_landscape_pt, a4_paper_300dpi,
        ScaleMode::FitToPage, 0.0f, /*auto_rotate*/false, dpi, dpi);
    REQUIRE(m.rotate_90 == false);
    // Without rotation: page 842x595, paper 2480x3508.
    // s_fit = min(2480/842, 3508/595) = min(2.945, 5.896) = 2.945 px/pt.
    REQUIRE_THAT(m.scale_x, WithinAbs(2.945f, 0.01f));
}

TEST_CASE("actual-size mode yields dpi/72 px-per-pt", "[print-geometry]") {
    // "Actual size" = 1 pt maps to 1/72 physical inch = dpi/72 printer pixels.
    // At 300 DPI: s = 300/72 ~ 4.1667 px/pt.
    auto m = compute_render_matrix(
        a4_portrait_pt, a4_paper_300dpi,
        ScaleMode::ActualSize, 0.0f, true, dpi, dpi);
    REQUIRE_THAT(m.scale_x, WithinAbs(300.0f / 72.0f, 0.002f));
    REQUIRE_THAT(m.scale_y, WithinAbs(300.0f / 72.0f, 0.002f));
}

TEST_CASE("custom-pct 75% yields 75% of fit scale", "[print-geometry]") {
    auto m = compute_render_matrix(
        a4_portrait_pt, a4_paper_300dpi,
        ScaleMode::CustomPct, /*custom_pct*/75.0f, true, dpi, dpi);
    const float expected_s = 0.75f * fit_scale_a4;
    REQUIRE_THAT(m.scale_x, WithinAbs(expected_s, 0.02f));
    REQUIRE_THAT(m.scale_y, WithinAbs(expected_s, 0.02f));
}

TEST_CASE("centering offsets for 50%-of-fit on A4@300dpi", "[print-geometry]") {
    auto m = compute_render_matrix(
        a4_portrait_pt, a4_paper_300dpi,
        ScaleMode::CustomPct, 50.0f, true, dpi, dpi);
    const float s50  = 0.5f * fit_scale_a4;
    const float stamp_w = 595.0f * s50;
    const float stamp_h = 842.0f * s50;
    REQUIRE_THAT(m.tx, WithinAbs((2480.0f - stamp_w) * 0.5f, 1.0f));
    REQUIRE_THAT(m.ty, WithinAbs((3508.0f - stamp_h) * 0.5f, 1.0f));
}
