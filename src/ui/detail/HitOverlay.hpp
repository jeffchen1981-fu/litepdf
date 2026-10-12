#pragma once

// #135: does a search update change what PdfCanvas's hit overlay draws? A scan
// posts one update per page it finishes, from every tab's session, and a canvas
// repaint blocks the UI thread in EndDraw until the next vsync. Repainting only
// when the overlay would differ keeps the UI thread free during a scan. Pure, so
// headless-testable, the same pattern as SelectionDrag.hpp.

#include "core/SearchSession.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace litepdf::ui {

// True when `a` and `b` draw the same highlights: the same multiset of (page,
// eight quad corners). Order is ignored -- each rectangle is drawn on its own,
// and SearchSession's unstable std::sort can swap two hits on one line between
// updates. The snippet is not drawn, so it is ignored too. Otherwise errs toward
// "different": a needless repaint costs one frame, a skipped one leaves wrong
// highlights on screen.
inline bool same_overlay(const std::vector<core::SearchSession::Hit>& a,
                         const std::vector<core::SearchSession::Hit>& b) {
    if (a.size() != b.size()) return false;
    using Key = std::pair<std::size_t, std::array<float, 8>>;
    auto keys = [](const std::vector<core::SearchSession::Hit>& hits) {
        std::vector<Key> out;
        out.reserve(hits.size());
        for (const auto& h : hits) {
            const auto& q = h.geom;
            out.push_back({h.page, {q.ul_x, q.ul_y, q.ur_x, q.ur_y,
                                    q.ll_x, q.ll_y, q.lr_x, q.lr_y}});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    return keys(a) == keys(b);
}

}  // namespace litepdf::ui
