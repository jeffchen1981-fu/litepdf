// #135: the overlay comparison PdfCanvas uses to skip repaints that would draw
// the same search highlights again (ui/detail/HitOverlay.hpp).
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/HitOverlay.hpp"

#include <vector>

using litepdf::core::SearchSession;
using litepdf::ui::same_overlay;

namespace {

SearchSession::Hit hit(std::size_t page, float x, float y) {
    SearchSession::Hit h{};
    h.page = page;
    h.geom.ul_x = x;          h.geom.ul_y = y;
    h.geom.ur_x = x + 20.0f;  h.geom.ur_y = y;
    h.geom.ll_x = x;          h.geom.ll_y = y + 10.0f;
    h.geom.lr_x = x + 20.0f;  h.geom.lr_y = y + 10.0f;
    return h;
}

}  // namespace

TEST_CASE("HitOverlay empty lists draw the same overlay", "[ui][hit_overlay]") {
    REQUIRE(same_overlay({}, {}));
}

TEST_CASE("HitOverlay identical hits draw the same overlay", "[ui][hit_overlay]") {
    const std::vector<SearchSession::Hit> a{hit(3, 10, 20), hit(3, 10, 60)};
    const std::vector<SearchSession::Hit> b{hit(3, 10, 20), hit(3, 10, 60)};
    REQUIRE(same_overlay(a, b));
}

TEST_CASE("HitOverlay the same hits in another order draw the same overlay",
          "[ui][hit_overlay]") {
    // SearchSession re-sorts every hit by (page, ul_y) with std::sort after each
    // scanned page. Two matches on one line tie, and the unstable sort can swap
    // them, so the painted page's hits come back in another order with nothing
    // on screen changed. Each rectangle is drawn on its own, so order is not
    // part of what the overlay draws.
    const std::vector<SearchSession::Hit> a{hit(3, 10, 20), hit(3, 50, 20), hit(3, 10, 60)};
    const std::vector<SearchSession::Hit> b{hit(3, 50, 20), hit(3, 10, 20), hit(3, 10, 60)};
    REQUIRE(same_overlay(a, b));
}

TEST_CASE("HitOverlay how often a hit occurs counts, not just which hits occur",
          "[ui][hit_overlay]") {
    // Order-insensitive, but a multiset, not a set: these two have the same size
    // and the same distinct hits, and differ only in how often each occurs.
    const std::vector<SearchSession::Hit> a{hit(3, 10, 20), hit(3, 10, 20), hit(3, 10, 60)};
    const std::vector<SearchSession::Hit> b{hit(3, 10, 20), hit(3, 10, 60), hit(3, 10, 60)};
    REQUIRE_FALSE(same_overlay(a, b));
}

TEST_CASE("HitOverlay a hit arriving or leaving changes the overlay", "[ui][hit_overlay]") {
    const std::vector<SearchSession::Hit> one{hit(3, 10, 20)};
    const std::vector<SearchSession::Hit> two{hit(3, 10, 20), hit(3, 10, 60)};
    REQUIRE_FALSE(same_overlay({}, one));    // the painted page's hits land
    REQUIRE_FALSE(same_overlay(one, {}));    // a retype clears them
    REQUIRE_FALSE(same_overlay(one, two));
}

TEST_CASE("HitOverlay the same count at other positions changes the overlay",
          "[ui][hit_overlay]") {
    // A retype that lands as many hits on the painted page as the old query had,
    // but on other words. A count comparison would keep the old highlights.
    const std::vector<SearchSession::Hit> old_q{hit(3, 10, 20), hit(3, 10, 60)};
    const std::vector<SearchSession::Hit> new_q{hit(3, 10, 20), hit(3, 40, 60)};
    REQUIRE_FALSE(same_overlay(old_q, new_q));

    // The comparison is on the source quads and errs toward repainting: a
    // changed corner counts even where the drawn bounds happen to be unchanged.
    // A needless repaint costs one frame; a skipped one leaves wrong highlights.
    for (int corner = 0; corner < 8; ++corner) {
        auto moved = old_q;
        float* f[8] = {&moved[1].geom.ul_x, &moved[1].geom.ul_y,
                       &moved[1].geom.ur_x, &moved[1].geom.ur_y,
                       &moved[1].geom.ll_x, &moved[1].geom.ll_y,
                       &moved[1].geom.lr_x, &moved[1].geom.lr_y};
        *f[corner] += 1.0f;
        INFO("corner field " << corner);
        REQUIRE_FALSE(same_overlay(old_q, moved));
    }
}

TEST_CASE("HitOverlay a hit on another page changes the overlay", "[ui][hit_overlay]") {
    REQUIRE_FALSE(same_overlay({hit(3, 10, 20)}, {hit(4, 10, 20)}));
}

TEST_CASE("HitOverlay the snippet is not drawn so it does not count", "[ui][hit_overlay]") {
    auto a = hit(3, 10, 20);
    auto b = hit(3, 10, 20);
    a.geom.snippet_utf8 = "foo";
    b.geom.snippet_utf8 = "bar";
    REQUIRE(same_overlay({a}, {b}));
}
