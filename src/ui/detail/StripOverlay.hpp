#pragma once

// #55 PR 3: the strip's overlays as pure functions. A page's overlays (search
// hits, the selection) are drawn from per-page data the canvas already holds; the
// strip only needs to say which pages those are and where each page sits. No
// Win32, no Direct2D, no MuPDF.

#include <cstddef>
#include <functional>
#include <iterator>
#include <vector>

#include "core/SearchSession.hpp"
#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripViewport.hpp"
#include "ui/detail/ViewportMath.hpp"

namespace litepdf::ui {

// A strip page's rectangle as the Placement the paged overlay code takes (the
// rect narrowed to float).
inline Placement strip_page_placement(const StripPageRect& r) noexcept {
    Placement p;
    p.x = static_cast<float>(r.x);
    p.y = static_cast<float>(r.y);
    p.w = static_cast<float>(r.w);
    p.h = static_cast<float>(r.h);
    return p;
}

// The hits of one page; PdfCanvas::HitsFn has this signature.
using HitsForPage = std::function<std::vector<core::SearchSession::Hit>(std::size_t)>;

// The hits the strip draws: those of every page of `visible`, in page order.
// Empty for an empty range or a null function.
inline std::vector<core::SearchSession::Hit> strip_overlay_hits(
    StripRange visible, const HitsForPage& hits_for_page) {
    std::vector<core::SearchSession::Hit> out;
    if (!hits_for_page) return out;
    for (int p = visible.first; p <= visible.last; ++p) {
        auto page_hits = hits_for_page(static_cast<std::size_t>(p));
        out.insert(out.end(), std::make_move_iterator(page_hits.begin()),
                   std::make_move_iterator(page_hits.end()));
    }
    return out;
}

}  // namespace litepdf::ui
