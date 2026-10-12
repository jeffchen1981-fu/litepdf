#pragma once

// #135: does a search update change what PdfCanvas's hit overlay draws? A scan
// posts one update per page it finishes, from every tab's session, and a canvas
// repaint blocks the UI thread in EndDraw until the next vsync. Repainting only
// when the overlay would differ keeps the UI thread free during a scan. Pure, so
// headless-testable, the same pattern as SelectionDrag.hpp.

#include "core/SearchSession.hpp"

#include <algorithm>
#include <vector>

namespace litepdf::ui {

// True when `a` and `b` draw the same highlights. Compares each hit's page and
// all eight quad corners in order; the snippet is not drawn, so it is ignored.
// Errs toward "different": a needless repaint costs one frame, a skipped one
// leaves wrong highlights on screen.
inline bool same_overlay(const std::vector<core::SearchSession::Hit>& a,
                         const std::vector<core::SearchSession::Hit>& b) noexcept {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](const core::SearchSession::Hit& x, const core::SearchSession::Hit& y) {
            const auto& p = x.geom;
            const auto& q = y.geom;
            return x.page == y.page
                && p.ul_x == q.ul_x && p.ul_y == q.ul_y
                && p.ur_x == q.ur_x && p.ur_y == q.ur_y
                && p.ll_x == q.ll_x && p.ll_y == q.ll_y
                && p.lr_x == q.lr_x && p.lr_y == q.lr_y;
        });
}

}  // namespace litepdf::ui
