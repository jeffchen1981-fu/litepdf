#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

#include "ui/detail/EffectiveLayout.hpp"
#include "ui/detail/HitOverlay.hpp"
#include "ui/detail/SelectionDrag.hpp"
#include "ui/detail/StripOverlay.hpp"
#include "ui/detail/StripViewport.hpp"

using namespace litepdf::ui;

namespace {

// Layout A (as in test_strip_viewport.cpp): three pages at zoom 1, tops 8 / 216 / 324.
std::vector<StripPageSize> sizes_a() { return {{100, 200}, {50, 100}, {100, 300}}; }

// Layout B: ten 100x100 pages at zoom 1, tops 8 + 108k.
std::vector<StripPageSize> sizes_b() { return std::vector<StripPageSize>(10, {100, 100}); }

StripViewport make(std::vector<StripPageSize> sizes, double w, double h, double zoom = 1.0) {
    StripViewport v;
    v.set_viewport(w, h);
    v.build(std::move(sizes), zoom);
    return v;
}

// One hit per page for pages 1-5; the quad's x is the page so each differs.
HitsForPage one_hit_per_page() {
    return [](std::size_t page) {
        std::vector<litepdf::core::SearchSession::Hit> out;
        if (page < 1 || page > 5) return out;
        litepdf::core::SearchSession::Hit h;
        h.page = page;
        const float x = static_cast<float>(page);
        h.geom.ul_x = x;
        h.geom.ll_x = x;
        h.geom.ur_x = x + 1;
        h.geom.lr_x = x + 1;
        h.geom.ul_y = 0;
        h.geom.ur_y = 0;
        h.geom.ll_y = 1;
        h.geom.lr_y = 1;
        out.push_back(h);
        return out;
    };
}

}  // namespace

TEST_CASE("StripViewport: page_at finds the page under a point") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.scroll_to(108);
    // Page 0 is {100, -100, 100, 200}, page 1 is {125, 108, 50, 100}.
    CHECK(v.page_at(150, 50) == std::optional<int>(0));
    CHECK(v.page_at(100, 0) == std::optional<int>(0));    // left edge inclusive
    CHECK_FALSE(v.page_at(200, 50).has_value());          // right edge exclusive
    CHECK_FALSE(v.page_at(150, 100).has_value());         // bottom edge exclusive: the gap
    CHECK(v.page_at(150, 108) == std::optional<int>(1));  // top edge inclusive
    CHECK_FALSE(v.page_at(110, 120).has_value());         // beside the narrow page

    v.reset();
    CHECK_FALSE(v.page_at(150, 50).has_value());
}

TEST_CASE("StripViewport: page_at ignores points outside the viewport") {
    // Page 0 is {0, 8, 400, 200}: wider and taller than the 300x150 viewport.
    StripViewport v = make(std::vector<StripPageSize>(2, StripPageSize{400, 200}), 300, 150);
    CHECK(v.page_at(150, 50) == std::optional<int>(0));
    CHECK_FALSE(v.page_at(350, 50).has_value());   // right of the viewport (scroll bar side)
    CHECK_FALSE(v.page_at(150, 170).has_value());  // below the viewport
    CHECK_FALSE(v.page_at(-1, 50).has_value());    // left of the viewport
    CHECK(v.page_at(299.9, 50) == std::optional<int>(0));
}

TEST_CASE("StripViewport: retained_for_background caps the visible pages by area") {
    StripViewport v = make(sizes_b(), 300, 300);
    v.scroll_to(788);
    v.recompute_current();
    CHECK(v.retained_for_background(5) == std::vector<int>{9, 8, 7});
    CHECK(v.retained_for_background(2) == std::vector<int>{9, 8});
    CHECK(v.retained_for_background(0).empty());
}

TEST_CASE("StripOverlay: overlay hits are the visible pages' hits in page order") {
    const HitsForPage f = one_hit_per_page();

    const auto hits = strip_overlay_hits(StripRange{2, 4}, f);
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].page == 2);
    CHECK(hits[1].page == 3);
    CHECK(hits[2].page == 4);

    CHECK(strip_overlay_hits(StripRange{}, f).empty());
    CHECK(strip_overlay_hits(StripRange{2, 4}, HitsForPage{}).empty());

    CHECK(same_overlay(strip_overlay_hits({2, 4}, f), strip_overlay_hits({2, 4}, f)));
    CHECK_FALSE(same_overlay(strip_overlay_hits({2, 4}, f), strip_overlay_hits({2, 5}, f)));
}

TEST_CASE("StripOverlay: a drag past the start page clamps to that page") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.scroll_to(108);

    const Placement p1 = strip_page_placement(v.page_rect(1));
    auto at = canvas_dip_to_page_point(150, 300, p1, 1.0f);  // y 300 is over page 2
    CHECK(at.x == 25.0f);
    CHECK(at.y == 100.0f);
    at = canvas_dip_to_page_point(100, 50, p1, 1.0f);
    CHECK(at.x == 0.0f);
    CHECK(at.y == 0.0f);

    // A page-point anchor maps to the same glyph after a zoom.
    REQUIRE(v.relayout(2.0));
    const StripPageRect r = v.page_rect(1);
    at = canvas_dip_to_page_point(static_cast<float>(r.x + 50), static_cast<float>(r.y + 100),
                                  strip_page_placement(r), 2.0f);
    CHECK(at.x == 25.0f);
    CHECK(at.y == 50.0f);
}

TEST_CASE("EffectiveLayout: continuous menu state") {
    auto s = continuous_menu_state(false, true, true);
    CHECK_FALSE(s.enabled);
    CHECK(s.checked);
    s = continuous_menu_state(true, false, false);
    CHECK_FALSE(s.enabled);
    CHECK_FALSE(s.checked);
    s = continuous_menu_state(true, true, true);
    CHECK(s.enabled);
    CHECK(s.checked);
    s = continuous_menu_state(true, true, false);
    CHECK(s.enabled);
    CHECK_FALSE(s.checked);
}

TEST_CASE("EffectiveLayout: continuous toggle") {
    auto t = continuous_toggle(false, false);
    CHECK(t.pref_on);
    CHECK_FALSE(t.spread_on);
    t = continuous_toggle(true, false);
    CHECK_FALSE(t.pref_on);
    CHECK_FALSE(t.spread_on);
    t = continuous_toggle(true, true);
    CHECK(t.pref_on);
    CHECK_FALSE(t.spread_on);
    t = continuous_toggle(false, true);
    CHECK(t.pref_on);
    CHECK_FALSE(t.spread_on);
}
