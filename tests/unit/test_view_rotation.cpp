#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/SearchSession.hpp"
#include "core/detail/ViewRotation.hpp"
#include "ui/detail/PageAnchor.hpp"

using Catch::Approx;
using namespace litepdf::core;

namespace {

ViewPoint apply(const Affine& m, ViewPoint p) {
    return ViewPoint{p.x * m.m11 + p.y * m.m21 + m.dx, p.x * m.m12 + p.y * m.m22 + m.dy};
}

}  // namespace

TEST_CASE("ViewRotation: cw and ccw wrap", "[rotation][view_rotation]") {
    CHECK(Rotation{3}.cw() == Rotation{0});
    CHECK(Rotation{0}.ccw() == Rotation{3});
    CHECK(Rotation{0}.cw() == Rotation{1});
    CHECK(Rotation{2}.ccw() == Rotation{1});
    CHECK(Rotation{1} != Rotation{2});
    CHECK_FALSE(Rotation{0}.swaps_axes());
    CHECK(Rotation{1}.swaps_axes());
    CHECK_FALSE(Rotation{2}.swaps_axes());
    CHECK(Rotation{3}.swaps_axes());

    const ViewSize s1 = view_size(100, 200, Rotation{1});
    CHECK(s1.w == 200.0f);
    CHECK(s1.h == 100.0f);
    const ViewSize s2 = view_size(100, 200, Rotation{2});
    CHECK(s2.w == 100.0f);
    CHECK(s2.h == 200.0f);
}

TEST_CASE("ViewRotation: rotation 0 is a translation", "[rotation][view_rotation]") {
    const Affine m = draw_affine(Rotation{0}, ViewRect{10, 20, 30, 40});
    CHECK(m.m11 == 1.0f);
    CHECK(m.m12 == 0.0f);
    CHECK(m.m21 == 0.0f);
    CHECK(m.m22 == 1.0f);
    CHECK(m.dx == 10.0f);
    CHECK(m.dy == 20.0f);
}

TEST_CASE("ViewRotation: local corners land on the named view corners",
          "[rotation][view_rotation]") {
    const ViewRect view{10, 20, 300, 100};
    // Local frame is 300 x 100 at 0/2 and 100 x 300 at 1/3.
    struct Expect { int q; float lw, lh; ViewPoint tl, tr, br, bl; };
    const Expect cases[] = {
        {0, 300, 100, {10, 20}, {310, 20}, {310, 120}, {10, 120}},
        {1, 100, 300, {310, 20}, {310, 120}, {10, 120}, {10, 20}},
        {2, 300, 100, {310, 120}, {10, 120}, {10, 20}, {310, 20}},
        {3, 100, 300, {10, 120}, {10, 20}, {310, 20}, {310, 120}},
    };
    for (const Expect& e : cases) {
        CAPTURE(e.q);
        const Rotation r{e.q};
        const ViewSize u = view_size(view.w, view.h, r);
        CHECK(u.w == e.lw);
        CHECK(u.h == e.lh);
        const Affine m = draw_affine(r, view);
        const ViewPoint tl = apply(m, ViewPoint{0, 0});
        const ViewPoint tr = apply(m, ViewPoint{u.w, 0});
        const ViewPoint br = apply(m, ViewPoint{u.w, u.h});
        const ViewPoint bl = apply(m, ViewPoint{0, u.h});
        CHECK(tl.x == e.tl.x);
        CHECK(tl.y == e.tl.y);
        CHECK(tr.x == e.tr.x);
        CHECK(tr.y == e.tr.y);
        CHECK(br.x == e.br.x);
        CHECK(br.y == e.br.y);
        CHECK(bl.x == e.bl.x);
        CHECK(bl.y == e.bl.y);
    }
}

TEST_CASE("ViewRotation: an interior point maps to absolute view coordinates",
          "[rotation][view_rotation]") {
    const ViewRect view{0, 0, 50, 80};
    const ViewPoint p{7, 3};
    const ViewPoint expect[] = {{7, 3}, {47, 7}, {43, 77}, {3, 73}};
    for (int q = 0; q < 4; ++q) {
        CAPTURE(q);
        const Rotation r{q};
        const ViewPoint v = local_to_view(p, r, view);
        CHECK(v.x == expect[q].x);
        CHECK(v.y == expect[q].y);
        const ViewPoint back = view_to_local(v, r, view);
        CHECK(back.x == p.x);
        CHECK(back.y == p.y);
    }
}

TEST_CASE("ViewRotation: rounding slack sits on the near edge after a turn",
          "[rotation][view_rotation]") {
    // 842 pt page at render scale 1.25 -> 1052.5 px, rounded up to a 1053 px bitmap.
    const ViewRect view{0, 0, 1053, 700};
    const ViewPoint v = local_to_view(ViewPoint{0, 1052.5f}, Rotation{1}, view);
    CHECK(v.x == Approx(0.5f));
}

TEST_CASE("ViewRotation: a local rect maps to its view bounding box",
          "[rotation][view_rotation]") {
    const ViewRect view{0, 0, 50, 80};
    const ViewRect local{10, 20, 30, 5};
    const ViewRect expect[] = {{10, 20, 30, 5}, {25, 10, 5, 30}, {10, 55, 30, 5}, {20, 40, 5, 30}};
    for (int q = 0; q < 4; ++q) {
        CAPTURE(q);
        const ViewRect r = local_rect_to_view(local, Rotation{q}, view);
        CHECK(r.x == expect[q].x);
        CHECK(r.y == expect[q].y);
        CHECK(r.w == expect[q].w);
        CHECK(r.h == expect[q].h);
    }
}

TEST_CASE("ViewRotation: a hit vertical extent in the view frame",
          "[rotation][view_rotation]") {
    const float W = 200, H = 300;
    const float x0 = 12, y0 = 34, qw = 40, qh = 9;
    const float top[] = {y0, x0, H - y0 - qh, W - x0 - qw};
    for (int q = 0; q < 4; ++q) {
        CAPTURE(q);
        const Rotation r{q};
        const ViewSize s = view_size(W, H, r);
        const ViewRect hit = local_rect_to_view(ViewRect{x0, y0, qw, qh}, r,
                                                ViewRect{0, 0, s.w, s.h});
        CHECK(hit.y == top[q]);
    }
}

TEST_CASE("ViewRotation: hit edges in the view frame", "[rotation][view_rotation]") {
    // A quad 100..200 x 100..118 pt on a 420 x 600 pt page (unturned).
    SearchSession::Hit h{};
    h.page = 0;
    h.geom.ul_x = h.geom.ll_x = 100.0f;
    h.geom.ur_x = h.geom.lr_x = 200.0f;
    h.geom.ul_y = h.geom.ur_y = 100.0f;
    h.geom.ll_y = h.geom.lr_y = 118.0f;
    const float W = 420, H = 600;
    const float top[]    = {100, 100, H - 118, W - 200};
    const float bottom[] = {118, 200, H - 100, W - 100};
    for (int q = 0; q < 4; ++q) {
        CAPTURE(q);
        const Rotation r{q};
        const ViewSize s = view_size(W, H, r);
        CHECK(litepdf::ui::hit_top_pt(h, r, s.w, s.h) == top[q]);
        CHECK(litepdf::ui::hit_bottom_pt(h, r, s.w, s.h) == bottom[q]);
    }
}
