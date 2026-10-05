// PR-B Task 1: pure status-bar logic -- page-input parsing, child geometry,
// and the rule that decides whether a page change may overwrite the box.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "ui/detail/StatusBarMath.hpp"

using litepdf::ui::detail::format_zoom_pct;
using litepdf::ui::detail::parse_page_input;
using litepdf::ui::detail::should_overwrite_page_box;
using litepdf::ui::detail::status_bar_child_rects;

TEST_CASE("StatusBarMath parse_page_input accepts an in-range 1-based page", "[statusbar]") {
    REQUIRE(parse_page_input(L"1", 128)   == 0);
    REQUIRE(parse_page_input(L"12", 128)  == 11);
    REQUIRE(parse_page_input(L"128", 128) == 127);
}

TEST_CASE("StatusBarMath parse_page_input rejects out of range", "[statusbar]") {
    // DocumentView::set_current_page CLAMPS, so an accepted 9999 would jump
    // silently to the last page. Rejection here is the only guard.
    REQUIRE_FALSE(parse_page_input(L"129", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"0", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input rejects empty and whitespace only", "[statusbar]") {
    REQUIRE_FALSE(parse_page_input(L"", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"   ", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"\t", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input rejects non numeric", "[statusbar]") {
    // ES_NUMBER blocks non-digits from the KEYBOARD but not from a paste.
    REQUIRE_FALSE(parse_page_input(L"abc", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"1a", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"-5", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"1.5", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"1 2", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input tolerates surrounding whitespace", "[statusbar]") {
    REQUIRE(parse_page_input(L"  7  ", 128) == 6);
    REQUIRE(parse_page_input(L"\t7", 128)   == 6);
}

TEST_CASE("StatusBarMath parse_page_input tolerates leading zeros", "[statusbar]") {
    REQUIRE(parse_page_input(L"007", 128) == 6);
}

TEST_CASE("StatusBarMath parse_page_input does not overflow on a long digit string",
          "[statusbar]") {
    // Must be REJECTED as out of range, not wrapped into range.
    REQUIRE_FALSE(parse_page_input(L"99999999999999999999", 128).has_value());
    REQUIRE_FALSE(parse_page_input(L"4294967297", 128).has_value());
}

TEST_CASE("StatusBarMath parse_page_input rejects everything when there is no document",
          "[statusbar]") {
    REQUIRE_FALSE(parse_page_input(L"1", 0).has_value());
    REQUIRE_FALSE(parse_page_input(L"1", -1).has_value());
}

TEST_CASE("StatusBarMath status_bar_child_rects centers children and lays them left to right",
          "[statusbar]") {
    const auto r = status_bar_child_rects(/*bar_h=*/24, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*edit_h_px=*/16,
                                          /*label_w_px=*/72, /*zoom_w_px=*/40);
    REQUIRE(r.edit_h  == 16);
    REQUIRE(r.edit_y  == 4);
    REQUIRE(r.edit_x  == 4);
    REQUIRE(r.edit_w  == 48);
    REQUIRE(r.label_x == 4 + 48 + 4);
    REQUIRE(r.label_y == 4);
    REQUIRE(r.label_h == 16);
    REQUIRE(r.label_w == 72);
    // The zoom readout: one padding step right of the "/ N" label's rectangle.
    REQUIRE(r.zoom_x  == 4 + 48 + 4 + 72 + 4);
    REQUIRE(r.zoom_y  == r.label_y);
    REQUIRE(r.zoom_h  == r.label_h);
    REQUIRE(r.zoom_w  == 40);
}

TEST_CASE("StatusBarMath status_bar_child_rects sizes the page box from its font, not the padding",
          "[statusbar]") {
    // #113, the numbers measured on the real bar. At 96 DPI the bar is 22 px
    // and the padding 4, which left the box 14 px tall -- a 10 px client area
    // under a 15 px line of 9 pt Segoe UI, so the EDIT clipped the bottom of
    // every digit. The box must take the height its font needs (line 15 +
    // frame 4 = 19) and sit centred, while the labels keep the padded strip.
    const auto r96 = status_bar_child_rects(/*bar_h=*/22, /*pad_px=*/4,
                                            /*edit_w_px=*/52, /*edit_h_px=*/19,
                                            /*label_w_px=*/96, /*zoom_w_px=*/45);
    REQUIRE(r96.edit_h  == 19);
    REQUIRE(r96.edit_y  == 1);   // (22 - 19) / 2, the odd pixel goes below
    REQUIRE(r96.label_y == 4);
    REQUIRE(r96.label_h == 14);
    REQUIRE(r96.zoom_y  == 4);
    REQUIRE(r96.zoom_h  == 14);

    // 192 DPI: bar 44, padding 8, line 32 + frame 4.
    const auto r192 = status_bar_child_rects(/*bar_h=*/44, /*pad_px=*/8,
                                             /*edit_w_px=*/104, /*edit_h_px=*/36,
                                             /*label_w_px=*/192, /*zoom_w_px=*/90);
    REQUIRE(r192.edit_h  == 36);
    REQUIRE(r192.edit_y  == 4);
    REQUIRE(r192.label_y == 8);
    REQUIRE(r192.label_h == 28);
}

TEST_CASE("StatusBarMath status_bar_child_rects never lets the page box overhang the bar",
          "[statusbar]") {
    // A box taller than the bar would be cut by the bar's own client edge;
    // clamp to the bar rather than hang off it.
    const auto r = status_bar_child_rects(/*bar_h=*/16, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*edit_h_px=*/30,
                                          /*label_w_px=*/72, /*zoom_w_px=*/40);
    REQUIRE(r.edit_h == 16);
    REQUIRE(r.edit_y == 0);
}

TEST_CASE("StatusBarMath status_bar_child_rects falls back to the padded strip without a font height",
          "[statusbar]") {
    // edit_h_px <= 0 means the caller could not measure the font; the box then
    // gets the same padded strip as the labels -- the pre-#113 layout.
    for (const int unknown : {0, -5}) {
        const auto r = status_bar_child_rects(/*bar_h=*/24, /*pad_px=*/4,
                                              /*edit_w_px=*/48, unknown,
                                              /*label_w_px=*/72, /*zoom_w_px=*/40);
        REQUIRE(r.edit_h == 16);
        REQUIRE(r.edit_y == 4);
    }
}

TEST_CASE("StatusBarMath status_bar_child_rects degrades safely on a tiny bar",
          "[statusbar]") {
    // A bar shorter than twice the padding (bar_h=4 <= 2*pad_px=8) falls back
    // to the `else` branch of both clamps: ctrl_h = bar_h (not bar_h - 2*pad)
    // and y = 0 (not a negative offset). Pin every field so a rewrite that
    // silently returns e.g. edit_h == 0 for an ordinary 24 px bar -- or drops
    // the fallback entirely -- cannot still pass this test.
    const auto r = status_bar_child_rects(/*bar_h=*/4, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*edit_h_px=*/19,
                                          /*label_w_px=*/72, /*zoom_w_px=*/40);
    REQUIRE(r.edit_x   == 4);
    REQUIRE(r.edit_y   == 0);
    REQUIRE(r.edit_w   == 48);
    REQUIRE(r.edit_h   == 4);
    REQUIRE(r.label_x  == 56);
    REQUIRE(r.label_y  == 0);
    REQUIRE(r.label_w  == 72);
    REQUIRE(r.label_h  == 4);
    REQUIRE(r.zoom_x   == 132);
    REQUIRE(r.zoom_y   == 0);
    REQUIRE(r.zoom_w   == 40);
    REQUIRE(r.zoom_h   == 4);
}

TEST_CASE("StatusBarMath should_overwrite_page_box allows overwrite when unfocused",
          "[statusbar]") {
    REQUIRE(should_overwrite_page_box(false, L"anything", L"7"));
}

TEST_CASE("StatusBarMath should_overwrite_page_box allows overwrite of untouched text",
          "[statusbar]") {
    REQUIRE(should_overwrite_page_box(true, L"7", L"7"));
}

TEST_CASE("StatusBarMath should_overwrite_page_box refuses to clobber typing",
          "[statusbar]") {
    // A wheel flip while the reader is mid-keystroke must not eat the digits.
    REQUIRE_FALSE(should_overwrite_page_box(true, L"12", L"7"));
    REQUIRE_FALSE(should_overwrite_page_box(true, L"", L"7"));
}

TEST_CASE("StatusBarMath format_zoom_pct formats ladder values", "[statusbar]") {
    REQUIRE(format_zoom_pct(1.0f)  == L"100%");
    REQUIRE(format_zoom_pct(0.25f) == L"25%");
    REQUIRE(format_zoom_pct(8.0f)  == L"800%");
}

TEST_CASE("StatusBarMath format_zoom_pct rounds to nearest, halves away from zero",
          "[statusbar]") {
    REQUIRE(format_zoom_pct(1.374f) == L"137%");
    REQUIRE(format_zoom_pct(1.375f) == L"138%");
    // The tie rule: 12.5 is exact in float, so this separates round-half-away
    // (13) from round-half-to-even (12).
    REQUIRE(format_zoom_pct(0.125f) == L"13%");
}

TEST_CASE("StatusBarMath format_zoom_pct shows fit values above the ladder",
          "[statusbar]") {
    // fit_percentage is unbounded above; only set_zoom_pct clamps.
    REQUIRE(format_zoom_pct(38.4f) == L"3840%");
    REQUIRE(format_zoom_pct(99.0f) == L"9900%");
    REQUIRE(format_zoom_pct(99.99f) == L"9999%");
}

TEST_CASE("StatusBarMath format_zoom_pct is empty above four digits", "[statusbar]") {
    // The label is sized for "9999%". A value that cannot be shown whole is
    // not shown.
    REQUIRE(format_zoom_pct(100.0f).empty());
    REQUIRE(format_zoom_pct(1e30f).empty());
    // volatile keeps the overflow at run time; a constant argument trips C4756.
    volatile float huge = std::numeric_limits<float>::max();
    REQUIRE(format_zoom_pct(huge).empty());
}

TEST_CASE("StatusBarMath format_zoom_pct is empty for zero and negatives",
          "[statusbar]") {
    // A minimized window drives the fit percentage to exactly 0.
    REQUIRE(format_zoom_pct(0.0f).empty());
    REQUIRE(format_zoom_pct(0.004f).empty());   // would round to 0
    REQUIRE(format_zoom_pct(-1.0f).empty());
    // Finite input, but pct * 100 overflows to -infinity.
    // volatile keeps the overflow at run time; a constant argument trips C4756.
    volatile float huge = std::numeric_limits<float>::max();
    REQUIRE(format_zoom_pct(-huge).empty());
}

TEST_CASE("StatusBarMath format_zoom_pct is empty for non-finite input",
          "[statusbar]") {
    REQUIRE(format_zoom_pct(std::numeric_limits<float>::quiet_NaN()).empty());
    REQUIRE(format_zoom_pct(std::numeric_limits<float>::infinity()).empty());
    REQUIRE(format_zoom_pct(-std::numeric_limits<float>::infinity()).empty());
}
