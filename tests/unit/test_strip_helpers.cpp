#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "ui/detail/EffectiveLayout.hpp"
#include "ui/detail/StripCarry.hpp"
#include "ui/detail/StripSchedule.hpp"
#include "ui/detail/StripScrollbar.hpp"

using namespace litepdf::ui;

TEST_CASE("StripScrollbar: units are rounded DIPs") {
    const StripScrollInfo top = strip_scroll_info(632, 300, 0);
    CHECK(top.max == 631);
    CHECK(top.page == 300);
    CHECK(top.pos == 0);

    CHECK(strip_scroll_info(632, 300, 100.4).pos == 100);
}

TEST_CASE("StripScrollbar: the largest position maps to the exact maximum") {
    const double total = 632.6;
    const double vp = 300.2;
    // In double the maximum scroll is 332.40000000000003, not the literal 332.4,
    // so compare against the expression.
    const double max_scroll = total - vp;

    const StripScrollInfo at_end = strip_scroll_info(total, vp, max_scroll);
    CHECK(at_end.max == 632);
    CHECK(at_end.page == 300);
    CHECK(at_end.pos == 333);

    CHECK(strip_scroll_from_pos(333, total, vp) == max_scroll);
    CHECK(strip_scroll_from_pos(332, total, vp) == 332);
    CHECK(strip_scroll_from_pos(0, total, vp) == 0);
    CHECK(strip_scroll_from_pos(500, total, vp) == max_scroll);
}

TEST_CASE("StripScrollbar: a strip that fits gives a disabled range") {
    const StripScrollInfo i = strip_scroll_info(500, 700, 0);
    CHECK(i.page > i.max);
    CHECK(i.pos == 0);
    CHECK(strip_scroll_from_pos(5, 500, 700) == 0);
}

TEST_CASE("StripScrollbar: 67 million DIPs fit in int") {
    const double total = 67473268;
    const double vp = 600;
    const StripScrollInfo i = strip_scroll_info(total, vp, total - vp);
    CHECK(i.max == 67473267);
    CHECK(i.page == 600);
    const int largest = i.max - i.page + 1;
    CHECK(i.pos == largest);
    CHECK(strip_scroll_from_pos(largest, total, vp) == total - vp);
}

TEST_CASE("StripCarry: spread_vertical matches the spread paint geometry") {
    const double slot = 600;

    SpreadVertical s = spread_vertical(400, 500, slot);
    CHECK(s.left_top_in_union == 100);
    CHECK(s.union_h == 500);

    s = spread_vertical(1000, 800, slot);
    CHECK(s.left_top_in_union == 0);
    CHECK(s.union_h == 1000);

    s = spread_vertical(500, 900, slot);
    CHECK(s.left_top_in_union == 50);
    CHECK(s.union_h == 900);

    s = spread_vertical(400, -1, slot);
    CHECK(s.left_top_in_union == 100);
    CHECK(s.union_h == 400);
}

TEST_CASE("StripCarry: spread_vertical places the right page in the union") {
    const double slot = 600;
    // Both fit: each page keeps its own centred top.
    CHECK(spread_vertical(400, 500, slot).right_top_in_union == 50);
    // The left page overflows, the right one is centred in the slot.
    CHECK(spread_vertical(1000, 400, slot).right_top_in_union == 100);
    // The right page overflows, so the union starts at its top.
    CHECK(spread_vertical(500, 900, slot).right_top_in_union == 0);
}

TEST_CASE("StripCarry: spread_page_top picks the carried page's own top") {
    const SpreadVertical sv = spread_vertical(500, 900, 600);
    CHECK(spread_page_top(sv, 3, 3) == 50);   // the left page
    CHECK(spread_page_top(sv, 4, 3) == 0);    // the right page
    // #171: 100 DIPs into the taller right page stays 100 DIPs into it. Measured
    // from the left page's top it would land 50 further down. The offset stays
    // inside the pan range, or clamp_pan would give both routes the same value.
    const CarriedPosition c{4, 100};
    CHECK(spread_pan_y_for(c, spread_page_top(sv, c.page, 3), sv.union_h, 600) == -100.0f);
}

TEST_CASE("EffectiveLayout: strip_format_ok accepts the measured formats") {
    CHECK(strip_format_ok(std::filesystem::path("a.PDF"), 1));
    CHECK(strip_format_ok(std::filesystem::path("b.epub"), 1));
    CHECK(strip_format_ok(std::filesystem::path("d.svg"), 1));
    // CBZ passes since #176: a page is sized from its image header.
    CHECK(strip_format_ok(std::filesystem::path("c.cbz"), 1));
    CHECK(strip_format_ok(std::filesystem::path("c.CBZ"), 1));
    CHECK_FALSE(strip_format_ok(std::filesystem::path("e.xps"), 1));
    CHECK_FALSE(strip_format_ok(std::filesystem::path("f.fb2"), 1));
    CHECK_FALSE(strip_format_ok(std::filesystem::path("a.pdf"), 0));
}

TEST_CASE("StripSchedule: slot_holds_bitmap") {
    PageSlot s;
    CHECK_FALSE(slot_holds_bitmap(s));  // None

    s.kind = PageStateKind::Ready;
    CHECK(slot_holds_bitmap(s));

    s.kind = PageStateKind::Pending;
    s.has_stale_bitmap = true;
    CHECK(slot_holds_bitmap(s));

    s.has_stale_bitmap = false;
    CHECK_FALSE(slot_holds_bitmap(s));

    s.kind = PageStateKind::Failed;
    CHECK_FALSE(slot_holds_bitmap(s));
}
