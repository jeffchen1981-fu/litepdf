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

namespace litepdf::ui {

// Bitmap pixel extent -> render-target DIPs.
inline float bitmap_px_to_dip(float px, float rt_dpi) noexcept {
    if (!(rt_dpi > 0.0f)) return px;   // also rejects NaN
    return px * 96.0f / rt_dpi;
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
