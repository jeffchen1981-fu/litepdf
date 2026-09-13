// PR-B Task 1: pure status-bar logic -- page-input parsing, child geometry,
// and the rule that decides whether a page change may overwrite the box.
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/StatusBarMath.hpp"

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
                                          /*edit_w_px=*/48, /*label_w_px=*/72);
    REQUIRE(r.edit_h  == 16);
    REQUIRE(r.edit_y  == 4);
    REQUIRE(r.edit_x  == 4);
    REQUIRE(r.edit_w  == 48);
    REQUIRE(r.label_x == 4 + 48 + 4);
    REQUIRE(r.label_y == r.edit_y);
    REQUIRE(r.label_h == r.edit_h);
    REQUIRE(r.label_w == 72);
}

TEST_CASE("StatusBarMath status_bar_child_rects degrades safely on a tiny bar",
          "[statusbar]") {
    // A bar shorter than twice the padding (bar_h=4 <= 2*pad_px=8) falls back
    // to the `else` branch of both clamps: ctrl_h = bar_h (not bar_h - 2*pad)
    // and y = 0 (not a negative offset). Pin every field so a rewrite that
    // silently returns e.g. edit_h == 0 for an ordinary 24 px bar -- or drops
    // the fallback entirely -- cannot still pass this test.
    const auto r = status_bar_child_rects(/*bar_h=*/4, /*pad_px=*/4,
                                          /*edit_w_px=*/48, /*label_w_px=*/72);
    REQUIRE(r.edit_x   == 4);
    REQUIRE(r.edit_y   == 0);
    REQUIRE(r.edit_w   == 48);
    REQUIRE(r.edit_h   == 4);
    REQUIRE(r.label_x  == 56);
    REQUIRE(r.label_y  == 0);
    REQUIRE(r.label_w  == 72);
    REQUIRE(r.label_h  == 4);
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
