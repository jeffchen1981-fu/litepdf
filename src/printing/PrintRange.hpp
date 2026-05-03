#pragma once
// src/printing/PrintRange.hpp -- Phase 8.5 Task 2
// Pure-function parser: Win32 PRINTPAGERANGE[] -> flat 0-based index list.
// Header has no commdlg.h dependency so unit tests don't need windows.h.

#include <cstddef>
#include <vector>

namespace litepdf::printing {

// 1-based, inclusive on both ends. Mirrors the Win32 PRINTPAGERANGE layout
// using DWORD-equivalent field types for portability.
struct PageRange {
    unsigned int from;
    unsigned int to;
};

// Returns 0-based page indices. Deduplication NOT performed (PrintDlg
// already validates). page_count caps the result; out-of-range entries
// are clamped. Inverted entries (from > to) are silently dropped.
[[nodiscard]] std::vector<std::size_t> parse_page_ranges(
    const PageRange* ranges,
    std::size_t      range_count,
    std::size_t      page_count);

} // namespace litepdf::printing
