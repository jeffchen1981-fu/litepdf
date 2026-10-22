#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <vector>

// PageSize only; Document.hpp is MuPDF-free.
#include "core/Document.hpp"

// #55: the size of every page for the continuous-scroll strip (spec §4.4),
// with a fallback for a page whose size cannot be read. Pure, so the fallback
// is testable without a fixture that fails to size. DocumentView::page_sizes()
// passes a `size_of` that reads through its own size cache.

namespace litepdf::core::detail {

// US Letter, in points: page 0's stand-in when page 0 itself cannot be sized.
inline constexpr PageSize kFallbackPageSize{612.0f, 792.0f};

// One size per page 0..n-1. A page whose `size_of` throws takes page 0's size;
// page 0 throwing gives kFallbackPageSize. A size failure never escapes.
inline std::vector<PageSize> fill_page_sizes(
        std::size_t n, const std::function<PageSize(std::size_t)>& size_of) {
    std::vector<PageSize> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        try {
            out.push_back(size_of(i));
        } catch (const std::exception&) {
            out.push_back(i == 0 ? kFallbackPageSize : out[0]);
        }
    }
    return out;
}

}  // namespace litepdf::core::detail
