#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "ui/detail/StripViewport.hpp"

using namespace litepdf::ui;

namespace {

// Layout A: three pages at zoom 1, tops 8 / 216 / 324, total 632.
std::vector<StripPageSize> sizes_a() { return {{100, 200}, {50, 100}, {100, 300}}; }

// Layout B: ten 100x100 pages at zoom 1, tops 8 + 108k, total 1088.
std::vector<StripPageSize> sizes_b() { return std::vector<StripPageSize>(10, {100, 100}); }

StripViewport make(std::vector<StripPageSize> sizes, double w, double h, double zoom = 1.0) {
    StripViewport v;
    v.set_viewport(w, h);
    v.build(std::move(sizes), zoom);
    return v;
}

}  // namespace

TEST_CASE("StripViewport: navigation puts the page at its anchor and pins it") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.navigate(1);
    CHECK(v.scroll_y() == 208);
    CHECK(v.current_page() == 1);
    CHECK(v.pinned());

    v.set_viewport(300, 140);
    CHECK(v.recompute_current() == 1);
}

TEST_CASE("StripViewport: scroll input ends the pin and sets the travel") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.navigate(1);
    CHECK(v.scroll_by(-50));
    CHECK_FALSE(v.pinned());
    CHECK(v.scroll_y() == 158);

    const StripRange want = prefetch_range(v.layout(), 158, 150, StripTravel::Up);
    CHECK(v.prefetch().first == want.first);
    CHECK(v.prefetch().last == want.last);
}

TEST_CASE("StripViewport: relayout keeps the viewport-top point and scales the pan") {
    StripViewport v = make(sizes_a(), 80, 150);
    v.scroll_to(108);
    v.pan_x_by(-10);

    CHECK(v.relayout(2.0));
    CHECK(v.scroll_y() == 208);
    CHECK(v.pan_x_eff() == -20);

    CHECK_FALSE(v.relayout(2.0));
}

TEST_CASE("StripViewport: keys follow the spec table") {
    StripViewport v = make(sizes_a(), 300, 150);
    CHECK(v.key(StripKey::LineDown));
    CHECK(v.scroll_y() == 100);
    CHECK(v.key(StripKey::PageDown));
    CHECK(v.scroll_y() == 235);

    CHECK(v.key(StripKey::End));
    CHECK(v.scroll_y() == 482);
    CHECK(v.current_page() == 2);
    CHECK(v.pinned());

    CHECK(v.key(StripKey::Home));
    CHECK(v.scroll_y() == 0);
    CHECK(v.current_page() == 0);
    CHECK(v.pinned());

    CHECK(v.key(StripKey::LineDown));
    CHECK_FALSE(v.pinned());
}

TEST_CASE("StripViewport: a visible hit does not scroll and an off-screen one is centred") {
    StripViewport v = make(sizes_b(), 300, 300);

    CHECK_FALSE(v.scroll_hit_into_view(1, 10, 20));  // doc 126-136
    CHECK(v.scroll_y() == 0);
    CHECK(v.current_page() == 1);
    CHECK(v.pinned());

    CHECK(v.scroll_hit_into_view(5, 40, 60));  // doc 588-608
    CHECK(v.scroll_y() == 448);
    CHECK(v.current_page() == 5);

    // doc 450-460 is 2 DIP below the viewport top: inside the 24 DIP margin.
    CHECK(v.scroll_hit_into_view(4, 10, 20));
    CHECK(v.scroll_y() == 305);
    CHECK(v.current_page() == 4);
}

TEST_CASE("StripViewport: visible_by_area starts with the current page") {
    StripViewport v = make(sizes_b(), 300, 300);
    v.scroll_to(788);
    // Scroll input alone leaves the tracker on page 0, which is not in V.
    CHECK(v.visible_by_area() == std::vector<int>{8, 9, 7});

    CHECK(v.recompute_current() == 9);
    CHECK(v.visible_by_area() == std::vector<int>{9, 8, 7});

    StripViewport w = make(sizes_b(), 300, 300);
    w.navigate(9);  // anchor 972 clamps to 788; pinned, no recompute
    CHECK(w.scroll_y() == 788);
    CHECK(w.visible_by_area() == std::vector<int>{9, 8, 7});
}

TEST_CASE("StripViewport: retain covers the prefetch range") {
    for (double y : {0.0, 50.0, 400.0, 788.0}) {
        for (bool up : {false, true}) {
            StripViewport v = make(sizes_b(), 300, 300);
            // Arrive from the far end so the last move sets the wanted travel.
            v.scroll_to(up ? 788 : 0);
            v.scroll_to(y);
            const StripRange p = v.prefetch();
            const StripRange r = v.retain();
            for (int page = p.first; page <= p.last; ++page) CHECK(r.contains(page));
        }
    }
}

TEST_CASE("StripViewport: a gap position survives a zoom change through save and restore") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.scroll_to(212);  // the gap after page 0, halfway
    const StripTabState saved = v.save();
    v.build(sizes_a(), 2.0);
    v.restore(saved);
    CHECK(v.scroll_y() == 412);
    CHECK_FALSE(v.pinned());

    StripViewport p = make(sizes_a(), 80, 150);
    p.pan_x_by(-10);
    const StripTabState s = p.save();
    CHECK(s.zoom_pct == 1.0);
    p.build(sizes_a(), 2.0);
    p.restore(s);
    CHECK(p.pan_x_eff() == -20);
}

TEST_CASE("StripViewport: carry round trip") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.navigate(1);
    v.scroll_by(30);
    CHECK(v.scroll_y() == 238);
    CHECK(v.recompute_current() == 1);

    const CarriedPosition c = v.carry();
    CHECK(c.page == 1);
    CHECK(c.offset_dip == 22);

    StripViewport w = make(sizes_a(), 300, 150);
    w.apply_carry(c);
    CHECK(w.scroll_y() == 238);
    CHECK(w.current_page() == 1);
    CHECK(w.pinned());
}

TEST_CASE("StripViewport: page_rect is viewport-relative and centres narrow pages") {
    StripViewport v = make(sizes_a(), 300, 150);
    v.scroll_to(108);
    const StripPageRect r0 = v.page_rect(0);
    CHECK(r0.x == 100);
    CHECK(r0.y == -100);
    CHECK(r0.w == 100);
    CHECK(r0.h == 200);
    CHECK(v.page_rect(1).x == 125);

    StripViewport n = make(sizes_a(), 80, 150);
    n.scroll_to(108);
    n.pan_x_by(-10);
    CHECK(n.page_rect(0).x == -10);
    CHECK(n.page_rect(1).x == 15);
}

TEST_CASE("StripViewport: can_pan") {
    CHECK_FALSE(make(sizes_a(), 300, 700).can_pan());
    CHECK(make(sizes_a(), 300, 150).can_pan());
    CHECK(make(sizes_a(), 80, 700).can_pan());
}

TEST_CASE("StripViewport: the scrollbar's largest position reaches the exact bottom") {
    StripViewport v = make(sizes_b(), 300, 300.4);
    const StripScrollInfo i = v.scroll_info();
    CHECK(v.scroll_to_pos(i.max - i.page + 1));
    CHECK(v.scroll_y() == 1088 - 300.4);
    CHECK(v.recompute_current() == 9);
}

TEST_CASE("StripViewport: no pages") {
    StripViewport v;
    v.set_viewport(300, 150);
    v.reset();
    CHECK_FALSE(v.has_layout());
    CHECK(v.visible().empty());
    CHECK_FALSE(v.scroll_by(10));
    CHECK_FALSE(v.key(StripKey::End));
}
