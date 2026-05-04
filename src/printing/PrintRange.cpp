// src/printing/PrintRange.cpp -- Phase 8.5 Task 2
#include "printing/PrintRange.hpp"

#include <algorithm>

namespace litepdf::printing {

std::vector<std::size_t> parse_page_ranges(
    const PageRange* ranges,
    std::size_t      range_count,
    std::size_t      page_count)
{
    std::vector<std::size_t> out;
    if (page_count == 0) return out;

    for (std::size_t i = 0; i < range_count; ++i) {
        const auto& r = ranges[i];
        if (r.from == 0 || r.to == 0)   continue;  // 1-based; 0 is invalid
        if (r.from > r.to)              continue;  // inverted -- drop

        const std::size_t from0 = r.from - 1;
        const std::size_t to0   = std::min<std::size_t>(r.to - 1, page_count - 1);
        if (from0 > to0)                continue;  // entire range past EOF

        for (std::size_t p = from0; p <= to0; ++p) out.push_back(p);
    }
    return out;
}

} // namespace litepdf::printing
