#include "core/ThumbnailModel.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::core::ThumbnailModel;

TEST_CASE("ThumbnailModel: empty model has no visible pages", "[thumbnail_model]") {
    ThumbnailModel m;
    auto r = m.visible_range();
    REQUIRE(r.first == 0);
    REQUIRE(r.last  == -1);  // empty: last < first
}

TEST_CASE("ThumbnailModel: total_height grows with page_count", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(0);
    REQUIRE(m.total_height_px() == 0);
    m.set_page_count(3);
    // 3 tiles x (160 + 8) - 8 (no trailing gap) = 496
    REQUIRE(m.total_height_px() == 496);
}

TEST_CASE("ThumbnailModel: DPI doubling scales pitch and total_height proportionally",
          "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(3);
    m.set_dpi(96);
    REQUIRE(m.tile_h_px()        == 160);
    REQUIRE(m.tile_w_px()        == 120);
    REQUIRE(m.total_height_px()  == 496);   // 3*168 - 8 = 496 at 96 dpi
    m.set_dpi(192);                          // 4K-style 200% scaling
    REQUIRE(m.tile_h_px()        == 320);    // 160 dip * 2
    REQUIRE(m.tile_w_px()        == 240);    // 120 dip * 2
    REQUIRE(m.total_height_px()  == 992);    // 3*336 - 16 = 992 at 192 dpi
}

TEST_CASE("ThumbnailModel: visible_range honors viewport + scroll", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(20);
    m.set_viewport_h_px(400);
    m.set_scroll_y_px(0);
    auto r = m.visible_range();
    // Tile pitch = 168 px; viewport 400 covers pages 0..2 (last partial)
    REQUIRE(r.first == 0);
    REQUIRE(r.last  == 2);
    m.set_scroll_y_px(168);  // skip page 0
    r = m.visible_range();
    REQUIRE(r.first == 1);
    REQUIRE(r.last  == 3);
}

TEST_CASE("ThumbnailModel: visible_range_with_buffer extends +/-1", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(20);
    m.set_viewport_h_px(400);
    m.set_scroll_y_px(168);  // visible 1..3
    auto rb = m.visible_range_with_buffer();
    REQUIRE(rb.first == 0);  // 1 - 1
    REQUIRE(rb.last  == 4);  // 3 + 1
}

TEST_CASE("ThumbnailModel: page_at_y returns hit page within tile", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(5);
    m.set_scroll_y_px(0);
    REQUIRE(m.page_at_y(0).value()   == 0);
    REQUIRE(m.page_at_y(159).value() == 0);
    REQUIRE(m.page_at_y(168).value() == 1);
    REQUIRE_FALSE(m.page_at_y(165).has_value());  // gap region
    REQUIRE_FALSE(m.page_at_y(99999).has_value());  // beyond
}

TEST_CASE("ThumbnailModel: tile_rect returns correct geometry", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(5);
    m.set_scroll_y_px(168);
    auto rc = m.tile_rect(2);  // page 2 is at y = 2*168 - 168 = 168
    REQUIRE(rc.left   == 0);
    REQUIRE(rc.top    == 168);
    REQUIRE(rc.right  == 120);
    REQUIRE(rc.bottom == 328);
}

TEST_CASE("ThumbnailModel: set_current_page reports old/new pair", "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_page_count(10);
    REQUIRE(m.current_page() == 0);
    auto changed = m.set_current_page(5);
    REQUIRE(changed.first  == 0);
    REQUIRE(changed.second == 5);
    REQUIRE(m.current_page() == 5);
    auto same = m.set_current_page(5);
    REQUIRE(same.first  == -1);  // sentinel: no change
    REQUIRE(same.second == -1);
}

TEST_CASE("ThumbnailModel: scroll_to_make_visible brings offscreen page into view at viewport top",
          "[thumbnail_model]") {
    ThumbnailModel m;
    m.set_dpi(96);
    m.set_tile_dip({120, 160});
    m.set_gap_dip(8);
    m.set_page_count(20);
    m.set_viewport_h_px(400);
    m.set_scroll_y_px(0);  // visible 0..2
    m.scroll_to_make_visible(10);
    auto r = m.visible_range();
    REQUIRE(r.first <= 10);
    REQUIRE(r.last  >= 10);
    // Lock down top-alignment: page 10 sits at top of viewport (10 * pitch = 1680).
    REQUIRE(m.scroll_y_px() == 10 * 168);
    // Already-visible page is a no-op:
    int prev = m.scroll_y_px();
    m.scroll_to_make_visible(r.first + 1);
    REQUIRE(m.scroll_y_px() == prev);
}
