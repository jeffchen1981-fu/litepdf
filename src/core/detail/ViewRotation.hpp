#pragma once

// #53: pure page <-> view rotation transform. No Win32, no Direct2D, no MuPDF
// -- the same "no Win32" convention as ui/detail/ViewportMath.hpp:3-5, so it
// is unit-testable headless. It lives in core/detail rather than ui/detail
// because DocumentView (core) and PdfCanvas (ui) both consume it, and core
// headers stay free of ui headers that pull in Win32 or Direct2D.
//
// FRAMES. The "local" frame is the unrotated page frame of one drawn page:
// [0, u.w] x [0, u.h] with u = view_size(view.w, view.h, r). The "view" frame is
// the on-screen frame; `view` is the rect the rotated page occupies in it.
// Rotation is clockwise in quarter turns.

namespace litepdf::core {

struct Rotation {
    int quarters = 0;   // 0..3, clockwise

    constexpr Rotation cw() const noexcept { return Rotation{(quarters + 1) & 3}; }
    constexpr Rotation ccw() const noexcept { return Rotation{(quarters + 3) & 3}; }
    constexpr bool swaps_axes() const noexcept { return quarters == 1 || quarters == 3; }

    friend constexpr bool operator==(Rotation a, Rotation b) noexcept {
        return a.quarters == b.quarters;
    }
    friend constexpr bool operator!=(Rotation a, Rotation b) noexcept {
        return a.quarters != b.quarters;
    }
};

struct ViewSize  { float w; float h; };
struct ViewPoint { float x; float y; };
struct ViewRect  { float x; float y; float w; float h; };

// Same layout and row-vector convention as D2D1_MATRIX_3X2_F:
// x' = x*m11 + y*m21 + dx,  y' = x*m12 + y*m22 + dy.
struct Affine { float m11, m12, m21, m22, dx, dy; };

// Extent of a w x h page after rotation r.
constexpr ViewSize view_size(float w, float h, Rotation r) noexcept {
    return r.swaps_axes() ? ViewSize{h, w} : ViewSize{w, h};
}

// Local -> view map for a page drawn into `view`.
constexpr Affine draw_affine(Rotation r, ViewRect view) noexcept {
    switch (r.quarters & 3) {
        case 1:  return Affine{0.0f, 1.0f, -1.0f, 0.0f, view.x + view.w, view.y};
        case 2:  return Affine{-1.0f, 0.0f, 0.0f, -1.0f, view.x + view.w, view.y + view.h};
        case 3:  return Affine{0.0f, -1.0f, 1.0f, 0.0f, view.x, view.y + view.h};
        default: return Affine{1.0f, 0.0f, 0.0f, 1.0f, view.x, view.y};
    }
}

constexpr ViewPoint local_to_view(ViewPoint local, Rotation r, ViewRect view) noexcept {
    const Affine m = draw_affine(r, view);
    return ViewPoint{local.x * m.m11 + local.y * m.m21 + m.dx,
                     local.x * m.m12 + local.y * m.m22 + m.dy};
}

// Exact inverse of local_to_view: the linear part is a signed permutation, so
// its inverse is its transpose.
constexpr ViewPoint view_to_local(ViewPoint v, Rotation r, ViewRect view) noexcept {
    const Affine m = draw_affine(r, view);
    const float tx = v.x - m.dx;
    const float ty = v.y - m.dy;
    return ViewPoint{tx * m.m11 + ty * m.m12, tx * m.m21 + ty * m.m22};
}

// Bounding box of the two mapped opposite corners.
constexpr ViewRect local_rect_to_view(ViewRect local, Rotation r, ViewRect view) noexcept {
    const ViewPoint a = local_to_view(ViewPoint{local.x, local.y}, r, view);
    const ViewPoint b = local_to_view(ViewPoint{local.x + local.w, local.y + local.h}, r, view);
    const float x0 = a.x < b.x ? a.x : b.x;
    const float y0 = a.y < b.y ? a.y : b.y;
    const float x1 = a.x < b.x ? b.x : a.x;
    const float y1 = a.y < b.y ? b.y : a.y;
    return ViewRect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace litepdf::core
