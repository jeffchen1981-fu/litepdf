#pragma once

#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <vector>

// PageSize only; Document.hpp is MuPDF-free.
#include "core/Document.hpp"

// #55: the size of every page for the continuous-scroll strip (spec §4.4),
// with a fallback for a page whose size cannot be read or is not usable. Pure, so the fallback
// is testable without a fixture that fails to size. DocumentView::page_sizes()
// passes a `size_of` that reads through its own size cache.

namespace litepdf::core::detail {

// US Letter, in points: page 0's stand-in when page 0 itself cannot be sized.
inline constexpr PageSize kFallbackPageSize{612.0f, 792.0f};

// A width or height the strip can lay out: positive and finite. MuPDF bounds a
// CBZ page whose image cannot be decoded by fz_empty_rect without throwing,
// which gives about -4.3e9 pt for both.
inline bool page_size_usable(const PageSize& s) {
    return s.width_pt > 0.0f && s.height_pt > 0.0f &&
           std::isfinite(s.width_pt) && std::isfinite(s.height_pt);
}

// `page`'s size, or nothing when reading it throws or gives a size that is not
// usable. A size failure never escapes.
inline std::optional<PageSize> usable_page_size(const Document& doc, std::size_t page) {
    try {
        const PageSize s = doc.page_size(page);
        if (page_size_usable(s)) return s;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

// One size per page 0..n-1. A page whose `size_of` throws, or returns a size
// that is not usable, takes page 0's size; page 0 failing gives
// kFallbackPageSize. A size failure never escapes.
inline std::vector<PageSize> fill_page_sizes(
        std::size_t n, const std::function<PageSize(std::size_t)>& size_of) {
    std::vector<PageSize> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const PageSize fallback = i == 0 ? kFallbackPageSize : out[0];
        try {
            const PageSize s = size_of(i);
            out.push_back(page_size_usable(s) ? s : fallback);
        } catch (const std::exception&) {
            out.push_back(fallback);
        }
    }
    return out;
}

}  // namespace litepdf::core::detail
