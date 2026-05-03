#pragma once
// src/printing/PrintGeometry.hpp -- Phase 8.5 Task 1
// Pure-function helpers for print render matrix composition.
// No Win32, no MuPDF includes -- POD-only types so this header can be
// included into unit tests without any platform dependencies.
//
// UNIT CONTRACT (critical -- both Rect params are floats; compiler cannot
// catch swapped arguments):
//   page_rect_pt  -- page bounding box in MuPDF POINTS (1 pt = 1/72 inch),
//                    as returned by fz_bound_page().
//   paper_rect_px -- printable area in PRINTER DEVICE PIXELS, from
//                    GetDeviceCaps(hdc, HORZRES/VERTRES).
//   dpi_x, dpi_y  -- from GetDeviceCaps(hdc, LOGPIXELSX/Y).
//
// Returned PrintMatrix::scale_x/scale_y are in px/pt (device pixels per
// MuPDF point) -- feed directly into fz_scale(). Centering tx/ty are in
// printer device pixels.

#include <algorithm>

namespace litepdf::printing {

// Rect stores (x0, y0, x1, y1). Used for both page rects (in points) and
// paper rects (in pixels) -- callers MUST respect the per-parameter unit
// contract documented above.
struct Rect {
    float x0, y0, x1, y1;
    constexpr float width()  const { return x1 - x0; }
    constexpr float height() const { return y1 - y0; }
};

// Result of compute_render_matrix(). Decomposed because MuPDF uses
// fz_matrix and Win32 uses XFORM; caller composes the final matrix
// in its own type. We just emit the scalar parts.
struct PrintMatrix {
    float scale_x;   // px/pt -- feed into fz_scale(scale_x, scale_y)
    float scale_y;   // px/pt
    bool  rotate_90; // true -> caller prepends a 90 degree pre-rotation
    float tx;        // centering translation in printer pixels
    float ty;        // centering translation in printer pixels
};

enum class ScaleMode {
    FitToPage,  // s_fit = min(paper_w / page_w, paper_h / page_h)  [px/pt]
    ActualSize, // 1 pt -> 1/72 inch on paper -> dpi/72 px/pt
    CustomPct,  // s = (custom_pct / 100) * s_fit
};

// Computes scale + rotation + centering matrix components.
// page_rect_pt:  fz_bound_page() output (MuPDF points, 1 pt = 1/72 inch).
// paper_rect_px: GetDeviceCaps(HORZRES/VERTRES) (printer device pixels).
// dpi_x, dpi_y:  GetDeviceCaps(LOGPIXELSX/Y).
[[nodiscard]] inline PrintMatrix compute_render_matrix(
    Rect  page_rect_pt,
    Rect  paper_rect_px,
    ScaleMode mode,
    float custom_pct,
    bool  auto_rotate,
    float dpi_x,
    float dpi_y)
{
    (void)dpi_y;  // currently using dpi_x for ActualSize (printers are square-pixel)

    const float page_w  = page_rect_pt.width();
    const float page_h  = page_rect_pt.height();
    const float paper_w = paper_rect_px.width();
    const float paper_h = paper_rect_px.height();

    // Auto-rotate when page and paper orientations differ.
    const bool need_rotate = auto_rotate &&
        ((page_w > page_h) != (paper_w > paper_h));

    // Effective page dimensions in points after rotation.
    const float eff_w_pt = need_rotate ? page_h : page_w;
    const float eff_h_pt = need_rotate ? page_w : page_h;

    // Fit scale in px/pt: paper_in_w / page_in_w simplifies to paper_w / eff_w_pt
    // because the dpi factor cancels out (see spec section 5):
    //   ratio = (paper_w * 72) / (eff_w_pt * dpi)            [dimensionless]
    //   x (dpi / 72)  ->  paper_w / eff_w_pt                 [px/pt]
    const float s_fit = std::min(paper_w / eff_w_pt, paper_h / eff_h_pt);

    // Scale per mode (all in px/pt).
    float s = s_fit;
    switch (mode) {
        case ScaleMode::FitToPage:
            s = s_fit;
            break;
        case ScaleMode::ActualSize:
            // 1 pt = 1/72 physical inch; at dpi_x dots/inch that is dpi_x/72 px/pt.
            s = dpi_x / 72.0f;
            break;
        case ScaleMode::CustomPct:
            // "50%" means half the fit-to-page scale, matching user expectation
            // (Acrobat / SumatraPDF precedent). NOT 50% of 1 px/pt.
            s = (custom_pct / 100.0f) * s_fit;
            break;
    }

    // Centering translation in printer pixels.
    const float tx = (paper_w - eff_w_pt * s) * 0.5f;
    const float ty = (paper_h - eff_h_pt * s) * 0.5f;

    return PrintMatrix{
        /*scale_x*/   s,
        /*scale_y*/   s,
        /*rotate_90*/ need_rotate,
        /*tx*/        tx,
        /*ty*/        ty,
    };
}

} // namespace litepdf::printing
