#pragma once

// #55: the continuous-scroll strip as pure geometry. No Win32, no Direct2D, no
// MuPDF -- so it is unit-testable headless, like ViewportMath.hpp.
//
// The strip stacks every page of a document top to bottom with a fixed gap G
// above the first page, between neighbours and below the last page:
//
//     y = 0       +-------------------+
//                 |  gap (G)          |
//     top(0)      +-------------------+
//                 |  page 0           |
//                 +-------------------+
//                 |  gap (G)          |
//     top(1)      +-------------------+
//                 |  page 1           |
//                 ...
//     total       |  gap (G)          |
//                 +-------------------+
//
// UNITS. Document coordinates are DIPs in `double` (a float runs out of
// integer resolution near 2^24: a 10,000-page document at 800 % zoom is about
// 67 million DIPs tall, where float's spacing is 8). Page sizes come in as PDF
// points; `zoom_pct` is the scale factor applied to them, so 1.0 maps 1 pt to
// 1 DIP and 8.0 is 800 %. The gap is in DIPs and does not scale with zoom.
//
// All prefix sums are built once in the constructor, so every query is O(1) or
// a binary search. Pages are 0-indexed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace litepdf::ui {

// Space above the first page, between pages and below the last page, in DIPs.
inline constexpr double kStripGapDip = 8.0;

// One page's natural size in PDF points.
struct StripPageSize {
    double w_pt = 0;
    double h_pt = 0;
};

// An inclusive page range. The default is the canonical empty range.
struct StripRange {
    int first = 0;
    int last = -1;
    bool empty() const noexcept { return last < first; }
    bool contains(int p) const noexcept { return p >= first && p <= last; }
};

// A vertical position in the strip, expressed relative to the layout so it
// survives a zoom change. `fraction` is in [0, 1): the share of the page's
// height (InPage) or of the gap (InGapAfter) that lies above the position.
// InGapAfter with page == -1 is the gap above page 0.
struct StripPos {
    enum class Kind { InPage, InGapAfter };
    Kind kind = Kind::InPage;
    int page = 0;
    double fraction = 0;
};

class StripLayout {
public:
    StripLayout() = default;

    StripLayout(std::vector<StripPageSize> sizes, double zoom_pct)
        : sizes_(std::move(sizes)), zoom_(zoom_pct) {
        tops_.reserve(sizes_.size());
        double y = kStripGapDip;
        for (const StripPageSize& s : sizes_) {
            tops_.push_back(y);
            y += s.h_pt * zoom_ + kStripGapDip;
        }
        total_ = sizes_.empty() ? 0.0 : y;
    }

    int page_count() const noexcept { return static_cast<int>(sizes_.size()); }
    double zoom_pct() const noexcept { return zoom_; }

    double width(int i) const { return sizes_[idx(i)].w_pt * zoom_; }
    double height(int i) const { return sizes_[idx(i)].h_pt * zoom_; }

    // G + sum over j < i of (h_j + G).
    double top(int i) const { return tops_[idx(i)]; }

    // Where navigating to page i puts the viewport: the top of the gap above it.
    double anchor(int i) const { return top(i) - kStripGapDip; }

    // G + sum of (h_j + G); 0 for a document with no pages.
    double total_height() const noexcept { return total_; }

    // Pages whose [top, top + h) intersects [y0, y1).
    StripRange visible(double y0, double y1) const {
        if (sizes_.empty() || !(y0 < y1)) return {};
        // First page whose bottom lies below y0: the last page starting at or
        // above y0, unless that one ends first, in which case the next.
        const int after = static_cast<int>(
            std::upper_bound(tops_.begin(), tops_.end(), y0) - tops_.begin());
        int first = after;
        if (after > 0 && bottom(after - 1) > y0) first = after - 1;
        // Last page whose top lies above y1.
        const int last = static_cast<int>(
            std::lower_bound(tops_.begin(), tops_.end(), y1) - tops_.begin()) - 1;
        if (first > last) return {};
        return {first, last};
    }

    // The widest page in r, 0 when r is empty.
    double widest(StripRange r) const {
        double w = 0.0;
        for (int p = std::max(r.first, 0); p <= r.last && p < page_count(); ++p)
            w = std::max(w, width(p));
        return w;
    }

    // The strip position at document y. y is clamped to [0, total_height()), so
    // the result is always a valid position (the gap above page 0 for an empty
    // layout).
    StripPos position_at(double y) const {
        StripPos pos;
        pos.kind = StripPos::Kind::InGapAfter;
        pos.page = -1;
        if (sizes_.empty()) return pos;
        if (!(y > 0.0)) y = 0.0;
        if (!(y < total_)) y = std::nextafter(total_, 0.0);

        const int after = static_cast<int>(
            std::upper_bound(tops_.begin(), tops_.end(), y) - tops_.begin());
        if (after == 0) {                       // the gap above page 0
            pos.fraction = y / kStripGapDip;
            return pos;
        }
        const int p = after - 1;
        const double t = tops_[static_cast<std::size_t>(p)];
        const double h = height(p);
        if (y < t + h) {
            pos.kind = StripPos::Kind::InPage;
            pos.page = p;
            pos.fraction = (y - t) / h;         // y < t + h, so h > 0 here
        } else {
            pos.page = p;                       // the gap below page p
            pos.fraction = (y - (t + h)) / kStripGapDip;
        }
        return pos;
    }

    // The document y of a position; the exact inverse of position_at.
    double y_of(const StripPos& p) const {
        if (p.kind == StripPos::Kind::InPage)
            return top(p.page) + p.fraction * height(p.page);
        if (p.page < 0) return p.fraction * kStripGapDip;
        return bottom(p.page) + p.fraction * kStripGapDip;
    }

private:
    static std::size_t idx(int i) { return static_cast<std::size_t>(i); }
    double bottom(int i) const { return top(i) + height(i); }

    std::vector<StripPageSize> sizes_;
    std::vector<double> tops_;
    double zoom_ = 1.0;
    double total_ = 0.0;
};

}  // namespace litepdf::ui
