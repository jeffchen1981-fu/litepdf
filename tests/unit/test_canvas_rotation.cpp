// On-screen view rotation (#53) on a real PdfCanvas: the rotate command, its
// landing and the strip relayout (spec 3.5, 5.4). Every case turns the page
// through the production PdfCanvas::rotate and then the rig's kick, in the order
// MainWindow's command handler uses.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "CanvasRig.hpp"

using namespace canvas_rig;
using Catch::Approx;

namespace {

// The centre x of a hit quad, in points.
float centre_x(const litepdf::core::Document::PageHit& q) {
    return (q.ul_x + q.ur_x + q.ll_x + q.lr_x) * 0.25f;
}

struct Px { int x; int y; };

// Where the local point (lx, ly), in DIPs from the unturned page's top-left,
// is drawn when the page is turned `quarters` clockwise into the view-frame
// rect (x, y, w, h). Written from the spec's table, not from draw_affine.
Px turned(float x, float y, float w, float h, int quarters, float lx, float ly) {
    float vx = 0.0f, vy = 0.0f;
    switch (quarters) {
        case 1:  vx = x + w - ly; vy = y + lx;     break;
        case 2:  vx = x + w - lx; vy = y + h - ly; break;
        case 3:  vx = x + ly;     vy = y + h - lx; break;
        default: vx = x + lx;     vy = y + ly;     break;
    }
    return Px{static_cast<int>(vx), static_cast<int>(vy)};
}

Px turned(const litepdf::ui::Placement& pl, int quarters, float lx, float ly) {
    return turned(pl.x, pl.y, pl.w, pl.h, quarters, lx, ly);
}

std::uint32_t pixel_at(PdfCanvas& c, Px p) { return Access::painted_pixel(c, p.x, p.y); }

// kMarker's square spans [36, 108] pt on both axes: at zoom 1.0 its centre is
// 72 DIP in from the page's top-left.
constexpr float kSquareCentre = 72.0f;

// A single page of kMarker at custom zoom `pct`, landed.
void land_marker(Rig& rig, float pct) {
    rig.view().set_zoom_pct(pct);
    rig.kick(0);
    rig.land_left();
}

void turn(Rig& rig, bool cw) {
    rig.rotate(cw);
    rig.land_left();
}

// Drags from local (x0, y) to local (x1, y) on the single page, turned
// `quarters`, at zoom 1.0.
void drag_select(Rig& rig, int quarters, float x0, float x1, float y) {
    const auto pl = Access::placement(rig.canvas());
    const Px a = turned(pl, quarters, x0, y);
    const Px b = turned(pl, quarters, x1, y);
    Access::left_down(rig.canvas(), a.x, a.y);
    Access::mouse_move(rig.canvas(), b.x, b.y);
    Access::left_up(rig.canvas(), b.x, b.y);
}

// Page 0's line of text: from x = 150 pt, baseline 60 pt (Helvetica 12), so
// y = 56 is its mid-height and x = 300 lies past its end.
constexpr float kLineStartX = 149.0f;
constexpr float kLineEndX   = 300.0f;
constexpr float kLineMidY   = 56.0f;
constexpr const char* kLineText = "Rotation marker text";

}  // namespace

TEST_CASE("PdfCanvas rotation: the fixture's word sits where the smoke row expects",
          "[canvas][rotation]") {
    const Document doc = open_fixture(kMarker);
    const auto hits = doc.page_hits(kMarkWordPage, "ZENITH", {});
    REQUIRE(hits.size() == 1);
    CHECK(centre_x(hits[0]) == Approx(kMarkWordCentreXPt).margin(0.5));
}

TEST_CASE("PdfCanvas rotation: the rig lands a strip page", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();
    rig.land_strip(0);

    const auto r = Access::strip_page_rect(rig.canvas(), 0);
    // The square spans [36, 108] pt on both axes: its centre is 72 pt in.
    CHECK(is_dark(Access::painted_pixel(rig.canvas(), static_cast<int>(r.x + 72.0),
                                        static_cast<int>(r.y + 72.0))));
}

TEST_CASE("PdfCanvas rotation: the strip at custom zoom relays out turned pages",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();
    rig.canvas().strip_navigate(1);
    rig.land_strip(1);

    rig.rotate(true);

    const auto& layout = Access::strip_layout(rig.canvas());
    CHECK(layout.height(0) == Approx(595.276));
    CHECK(rig.view().current_page() == 1);
    CHECK(Access::strip_scroll_y(rig.canvas()) == Approx(layout.anchor(1)));
}

TEST_CASE("PdfCanvas rotation: every strip page takes its own turned height",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kUnequal);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();

    // One turned size shared by every page would pass on a fixture whose pages
    // are all as wide as each other.
    const Document& doc = rig.view().document();
    const int n = rig.view().page_count();
    bool widths_differ = false;
    for (int i = 1; i < n; ++i) {
        if (doc.page_size(static_cast<std::size_t>(i)).width_pt != doc.page_size(0).width_pt) {
            widths_differ = true;
        }
    }
    REQUIRE(widths_differ);

    rig.rotate(true);

    const auto& layout = Access::strip_layout(rig.canvas());
    REQUIRE(layout.page_count() == n);
    for (int i = 0; i < n; ++i) {
        CAPTURE(i);
        const auto size = doc.page_size(static_cast<std::size_t>(i));
        CHECK(layout.height(i) == Approx(size.width_pt));
        CHECK(layout.width(i) == Approx(size.height_pt));
    }
}

TEST_CASE("PdfCanvas rotation: the strip at fit width keeps the page and refits",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kLarge);
    rig.view().set_zoom_mode_fit_width();
    rig.enter_strip();
    rig.canvas().strip_navigate(30);
    const float before = rig.view().zoom_pct();

    rig.rotate(true);

    CHECK(rig.view().current_page() == 30);
    CHECK(Access::strip_layout(rig.canvas()).zoom_pct() == Approx(rig.view().zoom_pct()));
    // Letter: the page was fitted by its width, 612 pt, and is now fitted by its
    // turned width, 792 pt.
    CHECK(rig.view().zoom_pct() == Approx(before * 612.0f / 792.0f).epsilon(1e-3));
}

TEST_CASE("PdfCanvas rotation: a turn ends the strip's held place", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Spread, kLarge);
    rig.view().set_zoom_pct(1.0f);
    enter_spread_from(rig, 5, 300.0);
    REQUIRE(Access::held_place(rig.canvas()));

    rig.rotate(true);

    CHECK_FALSE(Access::held_place(rig.canvas()));
    CHECK(Access::anchor_kind(rig.canvas()) == PageAnchor::Kind::Top);
    rig.land_both();
    leave_spread(rig);

    const CarriedPosition at = Access::strip_position(rig.canvas());
    CHECK(at.page == 5);
    CHECK(at.offset_dip == 0.0);
}

TEST_CASE("PdfCanvas rotation: a turn returns a panned single page to its top-left",
          "[canvas][rotation]") {
    // At 3.0 the page overflows 1000 x 850 both ways, upright and turned.
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_pct(3.0f);
    rig.kick(0);
    rig.land_left();
    Access::pan_by(rig.canvas(), -200.0f, -300.0f);
    REQUIRE(rig.pan_x() != 0.0f);
    REQUIRE(rig.pan_y() != 0.0f);

    rig.rotate(true);
    rig.land_left();

    CHECK(rig.pan_x() == 0.0f);
    CHECK(rig.pan_y() == 0.0f);
}

TEST_CASE("PdfCanvas rotation: at fit width the Top anchor lands with the new scale",
          "[canvas][rotation]") {
    // Wide, so the turned page still overflows at fit width: 595.276 x 1600 /
    // 841.89 is about 1131 DIP tall. In the 1000 wide rig it would be 707, and
    // pan_y == 0 would hold by the clamp alone.
    Rig rig(1600, kHeight, Layout::Single, kMarker);
    rig.view().set_zoom_mode_fit_width();
    rig.kick(0);
    rig.land_left();
    Access::pan_by(rig.canvas(), 0.0f, -200.0f);
    REQUIRE(rig.pan_y() != 0.0f);

    rig.rotate(true);

    CHECK(Access::anchor_kind(rig.canvas()) == PageAnchor::Kind::Top);
    CHECK_FALSE(Access::anchor_applied(rig.canvas()));
    rig.land_left();
    CHECK(rig.pan_y() == 0.0f);
    CHECK(Access::anchor_applied(rig.canvas()));
}

TEST_CASE("PdfCanvas rotation: a panned spread lands at the pair's top",
          "[canvas][rotation]") {
    // Every spread-pairs.pdf page is 420 pt wide, so at 3.0 the turned pair is
    // 1260 DIP tall and overflows the 850 canvas. At 2.0 it would be 840, and
    // pan_y == 0 would hold by the clamp alone.
    Rig rig;
    rig.view().set_zoom_pct(3.0f);
    rig.kick(1);
    rig.land_both();
    Access::pan_by(rig.canvas(), 0.0f, -400.0f);
    REQUIRE(rig.pan_y() != 0.0f);

    rig.rotate(true);
    rig.land_both();

    REQUIRE(Access::content_extent(rig.canvas()).h > static_cast<float>(kHeight));
    CHECK(rig.pan_y() == 0.0f);
}

TEST_CASE("PdfCanvas rotation: a turn during a drag ends the gesture",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.kick(0);
    rig.land_left();

    // Page 0's text line: from x = 150 pt, baseline 60 pt from the top.
    const float pct = rig.view().zoom_pct();
    const int x = static_cast<int>(160.0f * pct + rig.pan_x());
    const int y = static_cast<int>(56.0f * pct + rig.pan_y());
    Access::left_down(rig.canvas(), x, y);
    Access::mouse_move(rig.canvas(), x + 40, y);
    REQUIRE(Access::gesture_live(rig.canvas()));

    rig.canvas().rotate(true);

    CHECK_FALSE(Access::gesture_live(rig.canvas()));
}

// --- Task 5: the paged branches draw turned (spec 3.4, 3.5, 5.4) ---

TEST_CASE("PdfCanvas rotation: the single page draws turned in each direction",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(rig, 1.0f);

    turn(rig, true);
    auto pl = Access::placement(rig.canvas());
    // The bitmap is 596 x 842 px; turned, it is drawn 842 x 596.
    CHECK(pl.w == 842.0f);
    CHECK(pl.h == 596.0f);
    CHECK(is_dark(pixel_at(rig.canvas(), turned(pl, 1, kSquareCentre, kSquareCentre))));
    CHECK(Access::painted_pixel(rig.canvas(), static_cast<int>(pl.x + 72.0f),
                                static_cast<int>(pl.y + 72.0f)) == kWhite);

    turn(rig, true);
    pl = Access::placement(rig.canvas());
    CHECK(is_dark(pixel_at(rig.canvas(), turned(pl, 2, kSquareCentre, kSquareCentre))));

    turn(rig, true);
    pl = Access::placement(rig.canvas());
    CHECK(is_dark(pixel_at(rig.canvas(), turned(pl, 3, kSquareCentre, kSquareCentre))));

    // The square's turned centres, by hand: top-right, bottom-right, bottom-left.
    // A counter-clockwise turn from upright lands where three clockwise ones do.
    Rig ccw(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(ccw, 1.0f);
    turn(ccw, false);
    pl = Access::placement(ccw.canvas());
    CHECK(is_dark(Access::painted_pixel(ccw.canvas(), static_cast<int>(pl.x + 72.0f),
                                        static_cast<int>(pl.y + pl.h - 72.0f))));
}

TEST_CASE("PdfCanvas rotation: the content extent swaps", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(rig, 3.0f);
    const auto before = Access::content_extent(rig.canvas());

    turn(rig, true);

    const auto after = Access::content_extent(rig.canvas());
    CHECK(after.w == before.h);
    CHECK(after.h == before.w);
}

TEST_CASE("PdfCanvas rotation: a press maps to the page point in each rotation",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(rig, 1.0f);

    // A press 100 right and 50 down from the drawn rect's top-left; the local
    // extent is the unturned bitmap's, 596 x 842.
    const float expected[4][2] = {
        {100.0f, 50.0f}, {50.0f, 842.0f - 100.0f},
        {596.0f - 100.0f, 842.0f - 50.0f}, {596.0f - 50.0f, 100.0f}};
    for (int q = 0; q < 4; ++q) {
        CAPTURE(q);
        if (q > 0) turn(rig, true);
        const auto pl = Access::placement(rig.canvas());
        const auto pt = Access::page_point_at(rig.canvas(), static_cast<int>(pl.x) + 100,
                                              static_cast<int>(pl.y) + 50);
        CHECK(pt.x == Approx(expected[q][0]).margin(0.01));
        CHECK(pt.y == Approx(expected[q][1]).margin(0.01));
    }
}

TEST_CASE("PdfCanvas rotation: a hit highlight lands on the turned ink",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    rig.canvas().set_hits_source([](std::size_t page) {
        std::vector<SearchSession::Hit> hits;
        if (page == 0) {
            SearchSession::Hit h{};
            h.page = 0;
            h.geom.ul_x = h.geom.ll_x = 36.0f;
            h.geom.ur_x = h.geom.lr_x = 108.0f;
            h.geom.ul_y = h.geom.ur_y = 36.0f;
            h.geom.ll_y = h.geom.lr_y = 108.0f;
            hits.push_back(h);
        }
        return hits;
    });
    land_marker(rig, 1.0f);

    turn(rig, true);

    const auto pl = Access::placement(rig.canvas());
    // Over the turned square: the hit's fill over black, neither black nor paper.
    const std::uint32_t over_ink =
        pixel_at(rig.canvas(), turned(pl, 1, kSquareCentre, kSquareCentre));
    CHECK(over_ink != kBlack);
    CHECK(over_ink != kWhite);
    // Where the square was before the turn: blank paper, no highlight.
    CHECK(Access::painted_pixel(rig.canvas(), static_cast<int>(pl.x + 72.0f),
                                static_cast<int>(pl.y + 72.0f)) == kWhite);
}

TEST_CASE("PdfCanvas rotation: a turned spread places each page in its slot",
          "[canvas][rotation]") {
    Rig rig;
    rig.view().set_zoom_mode_fit_width();
    rig.canvas().set_hits_source(hits_on_every_page());
    rig.kick(1);
    rig.land_both();

    rig.rotate(true);
    rig.land_both();

    // Pages 1 and 2 turned are 400 and 1600 wide: the wider fits the 496 slot.
    const float z = rig.view().zoom_pct();
    CHECK(z == Approx(496.0f / 1600.0f));
    CHECK(Access::content_extent(rig.canvas()).h == Approx(std::ceil(420.0f * z)));

    // The right page, turned, centred in the right slot (x from 504, 496 x 850).
    const float rw = std::ceil(1600.0f * z);
    const float rh = std::ceil(420.0f * z);
    const float rx = 504.0f + (496.0f - rw) * 0.5f;
    const float ry = (static_cast<float>(kHeight) - rh) * 0.5f;
    // hit_at(2, 100): local 100..200 x 100..118 pt, centre (150, 109).
    const Px at = turned(rx, ry, rw, rh, 1, 150.0f * z, 109.0f * z);
    const std::uint32_t px = pixel_at(rig.canvas(), at);
    CHECK(px != kWhite);
    CHECK(px != kChrome);
}

TEST_CASE("PdfCanvas rotation: a press over a stale-zoom bitmap maps through its drawn rect",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(rig, 1.0f);
    rig.view().set_zoom_pct(2.0f);
    rig.kick(0);   // not landed: the zoom-1.0 bitmap stays on screen

    rig.canvas().rotate(true);

    const auto pl = Access::placement(rig.canvas());
    REQUIRE(pl.w == 842.0f);
    REQUIRE(pl.h == 596.0f);
    // The drawn rect's top-right DIP column is local x = 0 at 90 degrees.
    const auto pt = Access::page_point_at(rig.canvas(), static_cast<int>(pl.x + pl.w) - 1,
                                          static_cast<int>(pl.y));
    CHECK(pt.x == Approx(0.0).margin(0.01));
    CHECK(pt.y == Approx(1.0).margin(0.01));
}

TEST_CASE("PdfCanvas rotation: drag-select on the turned page copies the line",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(rig, 1.0f);
    turn(rig, true);

    drag_select(rig, 1, kLineStartX, kLineEndX, kLineMidY);

    const auto& sel = rig.view().selection();
    REQUIRE(sel.has_value());
    CHECK(sel->text_utf8 == kLineText);
}

TEST_CASE("PdfCanvas rotation: a selection survives the turn", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker(rig, 1.0f);
    drag_select(rig, 0, kLineStartX, kLineEndX, kLineMidY);
    REQUIRE(rig.view().selection().has_value());
    const litepdf::core::TextSelection before = *rig.view().selection();
    REQUIRE(before.text_utf8 == kLineText);
    REQUIRE_FALSE(before.quads.empty());

    turn(rig, true);

    const auto& after = rig.view().selection();
    REQUIRE(after.has_value());
    CHECK(after->text_utf8 == before.text_utf8);
    CHECK(after->page == before.page);
    CHECK(after->quads.size() == before.quads.size());

    // Just inside the first quad's top edge, half way along: above the glyphs.
    const auto& q = before.quads[0];
    const auto pl = Access::placement(rig.canvas());
    const Px at = turned(pl, 1, (q.ul_x + q.ur_x) * 0.5f, q.ul_y + 1.0f);
    const std::uint32_t selected = pixel_at(rig.canvas(), at);
    CHECK(selected != kWhite);
    CHECK((selected & 0xFFu) > ((selected >> 16) & 0xFFu));   // blue over red
    // Not the ink: the same pixel is paper once the selection is gone.
    rig.view().clear_selection();
    CHECK(pixel_at(rig.canvas(), at) == kWhite);
}

TEST_CASE("PdfCanvas rotation: rotation belongs to the view", "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    DocumentView& second = rig.second_view(kMarker);
    // A fresh view is at fit width; the square's centre at 72 assumes zoom 1.0.
    second.set_zoom_pct(1.0f);
    land_marker(rig, 1.0f);
    turn(rig, true);

    rig.canvas().set_view(&second);
    rig.kick_view(second, 0);
    rig.land_left();
    auto pl = Access::placement(rig.canvas());
    CHECK(is_dark(pixel_at(rig.canvas(), turned(pl, 0, kSquareCentre, kSquareCentre))));

    rig.canvas().set_view(&rig.view());
    rig.kick(0);
    rig.land_left();
    pl = Access::placement(rig.canvas());
    CHECK(is_dark(pixel_at(rig.canvas(), turned(pl, 1, kSquareCentre, kSquareCentre))));
}
// --- Task 6: the strip draws turned (spec 3.4, 3.5, 5.4) ---

namespace {

// kMarker's page 0 in the strip at custom zoom 1.0, landed.
void land_marker_strip(Rig& rig) {
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();
    rig.land_strip(0);
}

// One hit on page 0, over kMarker's square ([36, 108] pt on both axes).
PdfCanvas::HitsFn square_hit_source() {
    return [](std::size_t page) {
        std::vector<SearchSession::Hit> hits;
        if (page == 0) {
            SearchSession::Hit h{};
            h.page = 0;
            h.geom.ul_x = h.geom.ll_x = 36.0f;
            h.geom.ur_x = h.geom.lr_x = 108.0f;
            h.geom.ul_y = h.geom.ur_y = 36.0f;
            h.geom.ll_y = h.geom.lr_y = 108.0f;
            hits.push_back(h);
        }
        return hits;
    };
}

// A strip page's layout rect, as the Placement the drawn rect is compared with.
void check_is_layout_rect(const litepdf::ui::Placement& d, const litepdf::ui::StripPageRect& r) {
    CHECK(d.x == static_cast<float>(r.x));
    CHECK(d.y == static_cast<float>(r.y));
    CHECK(d.w == static_cast<float>(r.w));
    CHECK(d.h == static_cast<float>(r.h));
}

}  // namespace

TEST_CASE("PdfCanvas rotation: a fresh strip page is drawn and pressed at its natural size",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker_strip(rig);

    // The bitmap's natural extent, not the layout's 595.276 x 841.89.
    auto d = Access::strip_drawn_rect(rig.canvas(), 0);
    CHECK(d.w == 596.0f);
    CHECK(d.h == 842.0f);

    // 270 degrees: the width's slack is now on the vertical axis, which the
    // turn flips.
    rig.rotate(false);
    rig.land_strip(0);
    d = Access::strip_drawn_rect(rig.canvas(), 0);
    CHECK(d.w == 842.0f);
    CHECK(d.h == 596.0f);

    const auto r = Access::strip_page_rect(rig.canvas(), 0);
    CHECK(d.x == static_cast<float>(r.x));
    CHECK(d.y == static_cast<float>(r.y));
    // At 270 the local x is the distance up from the drawn rect's bottom: a
    // transform from the layout rect would be 0.72 DIP off.
    const int x = static_cast<int>(r.x) + 200;
    const int y = static_cast<int>(r.y + 596.0) - 100;
    const auto pt = Access::strip_page_point_at(rig.canvas(), 0, x, y);
    CHECK(pt.x == Approx(r.y + 596.0 - y).margin(0.05));

    // The square's turned centre: bottom-left.
    const Px centre{static_cast<int>(r.x + 72.0), static_cast<int>(r.y + 596.0 - 72.0)};
    CHECK(is_dark(pixel_at(rig.canvas(), centre)));

    // The hit over the square is drawn under the page's turn: its 0.40-alpha
    // yellow over the black ink.
    rig.canvas().set_hits_source(square_hit_source());
    CHECK_FALSE(is_dark(pixel_at(rig.canvas(), centre)));
    rig.canvas().set_hits_source(nullptr);
}

TEST_CASE("PdfCanvas rotation: a real press and drag in the turned strip map through the drawn rect",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker_strip(rig);
    rig.rotate(false);
    rig.land_strip(0);

    const auto r = Access::strip_page_rect(rig.canvas(), 0);
    const int x0 = static_cast<int>(r.x) + 200;
    const int y0 = static_cast<int>(r.y + 596.0) - 100;
    Access::left_down(rig.canvas(), x0, y0);
    CHECK(Access::live_anchor(rig.canvas()).x == Approx(r.y + 596.0 - y0).margin(0.05));

    Access::mouse_move(rig.canvas(), x0 + 30, y0 - 150);
    CHECK(Access::live_extent(rig.canvas()).x ==
          Approx(r.y + 596.0 - (y0 - 150)).margin(0.05));
    Access::left_up(rig.canvas(), x0 + 30, y0 - 150);
}

TEST_CASE("PdfCanvas rotation: a stale strip page is pressed through its layout rect",
          "[canvas][rotation]") {
    Rig rig(kWidth, kHeight, Layout::Single, kMarker);
    land_marker_strip(rig);
    rig.view().set_zoom_pct(2.0f);
    rig.strip_kick(KickReason::Zoom);   // not landed: the zoom-1.0 bitmap stays
    rig.rotate(true);                   // not landed either
    REQUIRE_FALSE(Access::strip_slot_fresh(rig.canvas(), 0));

    const auto r = Access::strip_page_rect(rig.canvas(), 0);
    // The layout at 2.0, turned: the natural rect would be 842 x 596.
    REQUIRE(r.w == Approx(1683.8).margin(0.1));
    REQUIRE(r.h == Approx(1190.6).margin(0.1));
    check_is_layout_rect(Access::strip_drawn_rect(rig.canvas(), 0), r);

    // Quarter 1's inverse at the layout's zoom.
    const int vx = static_cast<int>(r.x) + 300;
    const int vy = static_cast<int>(r.y) + 200;
    const auto pt = Access::strip_page_point_at(rig.canvas(), 0, vx, vy);
    CHECK(pt.x == Approx((vy - r.y) / 2.0).margin(0.01));
    CHECK(pt.y == Approx((r.x + r.w - vx) / 2.0).margin(0.01));
}

TEST_CASE("PdfCanvas rotation: a failed strip page keeps its placeholder in the turned rect",
          "[canvas][rotation]") {
    // Custom zoom: at a fit mode the turn refits, the render scale changes, and
    // every Failed slot is reset, so no border would be drawn.
    Rig rig(kWidth, kHeight, Layout::Single, kDamaged);
    rig.view().set_zoom_pct(1.0f);
    rig.enter_strip();
    rig.canvas().strip_navigate(2);
    rig.land_strip_failed(2);

    rig.rotate(true);
    REQUIRE(Access::strip_slot_kind(rig.canvas(), 2) == PageStateKind::Failed);

    // Page 2 is damaged; its stand-in is page 0's size, turned.
    const auto r = Access::strip_page_rect(rig.canvas(), 2);
    check_is_layout_rect(Access::strip_drawn_rect(rig.canvas(), 2), r);
    CHECK(r.w == Approx(rig.view().document().page_size(0).height_pt));
    // An integral left edge (194 in this rig), so the 1 DIP stroke covers the
    // column r.x wholly. On a fractional edge that column is part chrome, part
    // page, and would pass the checks below with no border drawn.
    REQUIRE(r.x == std::floor(r.x));

    // The border's left column, mid-height: the border colour, neither the page
    // nor the chrome.
    const std::uint32_t px = Access::painted_pixel(
        rig.canvas(), static_cast<int>(r.x), static_cast<int>(r.y + r.h * 0.5));
    CHECK(px != kWhite);
    CHECK(px != kChrome);
    CHECK(px == 0x00C0C0C0u);
}
