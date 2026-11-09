#pragma once

// PR-A1: pure placement + pan math for PdfCanvas. No Win32, no Direct2D, no
// MuPDF -- so it is unit-testable headless, the same pattern as SplitterMath.hpp
// and PdfCanvasLayout.hpp.
//
// UNITS. Everything here is in render-target DIPs. Bitmaps are created at 96 DPI
// (D2D1::BitmapProperties defaults dpiX/dpiY to 96.0f), so ID2D1Bitmap::GetSize()
// reports PIXELS, while the render target is created at the window's DPI and
// ID2D1RenderTarget::GetSize() reports DIPs. bitmap_px_to_dip is the one
// conversion point; call it on the bitmap extent before anything else here.

#include <cmath>

namespace litepdf::ui {

// Bitmap pixel extent -> render-target DIPs.
inline float bitmap_px_to_dip(float px, float rt_dpi) noexcept {
    if (!(rt_dpi > 0.0f)) return px;   // also rejects NaN
    return px * 96.0f / rt_dpi;
}

// (#194) The pixel extent a page `extent_pt` long renders to at `render_scale`:
// the worker sizes its pixmap with fz_round_rect, which rounds the far edge up
// past a 0.001 slack. A page's bounds start at 0, so the near edge adds nothing.
// Lets a half of a spread still landing be measured as its bitmap will be.
inline float rendered_px(float extent_pt, float render_scale) noexcept {
    const float px = std::ceil(extent_pt * render_scale - 0.001f);
    return px > 0.0f ? px : 0.0f;   // also rejects NaN
}

// (#195) DIPs per PDF point of a page's bitmap as drawn: what places anything
// given in PDF points (search hits, the selection, a press) over that bitmap. A
// zoom or fit change moves the view's zoom_pct at once, while the bitmap on
// screen keeps the scale it was rendered at until its replacement lands.
//
//   A bitmap of the size the current render scale gives is taken to be at the
//   current zoom, exactly: the common case keeps its numbers.
//   Any other size gives its own scale, read from the page's longer side, which
//   pins it closest. MuPDF rounds the far edge up (rendered_px), so `px - 0.5`
//   is the middle of the extents that round to `px`: off by at most half a
//   pixel anywhere on the page.
//   With no usable page or bitmap size, the view's zoom.
inline float drawn_zoom_pct(float bitmap_w_px, float bitmap_h_px,
                            float page_w_pt, float page_h_pt,
                            float render_scale, float zoom_pct, float rt_dpi) noexcept {
    if (!(page_w_pt > 0.0f) || !(page_h_pt > 0.0f)) return zoom_pct;   // also NaN
    if (rendered_px(page_w_pt, render_scale) == bitmap_w_px
        && rendered_px(page_h_pt, render_scale) == bitmap_h_px) {
        return zoom_pct;
    }
    const bool  tall = page_h_pt >= page_w_pt;
    const float px   = tall ? bitmap_h_px : bitmap_w_px;
    const float pt   = tall ? page_h_pt : page_w_pt;
    if (!(px > 0.0f)) return zoom_pct;
    return bitmap_px_to_dip((px - 0.5f) / pt, rt_dpi);
}

// Clamp one axis of the pan offset.
//
//   content <= viewport : the axis is centered by place_bitmap, so the pan is
//                         meaningless and pinned to 0.
//   content >  viewport : TOP-LEFT origin. pan 0 puts the content's leading
//                         edge at the viewport's leading edge; the valid range
//                         is [viewport - content, 0].
//
// NOTE this is a deliberate change from the shipped semantics, where pan was an
// offset from a CENTERED position (PdfCanvas.cpp:857-858 computed
// dx = (vp.width - dst_w) * 0.5f and added pan to it). Under that origin pan 0
// meant "centered", so on an overflowing axis the content's top sat above the
// viewport. No pan value is persisted -- SessionTab carries only path, page,
// zoom mode and zoom scale -- so the change needs no migration.
inline float clamp_pan(float pan, float content, float viewport) noexcept {
    if (!(pan == pan))          return 0.0f;   // NaN
    if (!(content > viewport))  return 0.0f;   // fits (or degenerate) -> centered
    const float lo = viewport - content;       // negative
    if (pan < lo)   return lo;
    if (pan > 0.0f) return 0.0f;
    return pan;
}

// True when the content overflows the viewport on either axis -- exactly when
// clamp_pan leaves some axis a range to pan through. The same `content >
// viewport` test as clamp_pan, so the hand-tool cursor and the pan can never
// disagree about whether there is anything to move. NaN never overflows,
// matching clamp_pan's "degenerate -> pinned" branch.
inline bool content_overflows(float content_w, float content_h,
                              float vp_w, float vp_h) noexcept {
    return content_w > vp_w || content_h > vp_h;
}

struct Placement {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Destination rect for a bitmap drawn at NATURAL SIZE -- w/h always equal the
// source extent, for every viewport. The shipped code instead scaled the
// destination down (and up) to fit the viewport unconditionally, which is why
// changing the render scale never changed what the user saw.
//
// An axis whose content fits is centered; an axis that overflows uses the
// top-left origin described on clamp_pan.
inline Placement place_bitmap(float src_w, float src_h,
                              float vp_w,  float vp_h,
                              float pan_x, float pan_y) noexcept {
    Placement p;
    p.w = src_w;
    p.h = src_h;
    p.x = (src_w > vp_w) ? clamp_pan(pan_x, src_w, vp_w)
                         : (vp_w - src_w) * 0.5f;
    p.y = (src_h > vp_h) ? clamp_pan(pan_y, src_h, vp_h)
                         : (vp_h - src_h) * 0.5f;
    return p;
}

// PDF points -> render-target DIPs for overlay geometry (search-hit quads).
//
// One PDF point is exactly zoom_pct DIPs. Note there is NO dpi term: the pixmap
// is page_pt * render_scale pixels, and converting that to DIPs divides the dpi
// factor straight back out. The shipped overlay multiplied by a render scale
// times the (now removed) fit ratio; using a render scale here would double
// every hit rectangle at 200% scaling.
inline float pdf_point_to_dip(float pt, float zoom_pct) noexcept {
    return pt * zoom_pct;
}

// The margin a search hit keeps from the viewport's top and bottom edge to count
// as fully visible.
inline constexpr float kHitMarginDip = 24.0f;

// True when a quad spanning [q_top, q_bot] in canvas DIPs needs no scroll.
inline bool quad_fully_visible(float q_top, float q_bot, float vp_h) noexcept {
    return q_top >= kHitMarginDip && q_bot <= vp_h - kHitMarginDip;
}

// The pan that puts a quad's centre at the viewport's centre. `quad_center` is
// measured from the top of the quad's own page, and `page_top` is where that page
// sits at pan 0; `content_h` is the height the pan is clamped against. In a spread
// that is the painted union, while `page_top` differs between the two pages.
inline float pan_y_centring_quad(float quad_center, float page_top,
                                 float content_h, float vp_h) noexcept {
    return clamp_pan(vp_h * 0.5f - quad_center - page_top, content_h, vp_h);
}

// Client-area pixels (mouse message coordinates) -> render-target DIPs. The same
// factor as bitmap_px_to_dip; a separate name so each call site says which kind
// of pixel it holds.
inline float client_px_to_dip(float px, float dpi) noexcept {
    return bitmap_px_to_dip(px, dpi);
}

// Render-target DIPs -> PDF points. The inverse of pdf_point_to_dip, and pinned
// to zoom_pct for the same reason.
inline float dip_to_pdf_point(float dip, float zoom_pct) noexcept {
    if (!(zoom_pct > 0.0f)) return 0.0f;   // also rejects NaN
    return dip / zoom_pct;
}

}  // namespace litepdf::ui
