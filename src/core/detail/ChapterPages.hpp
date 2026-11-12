#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

// #230: where a flat page number lies in a multi-chapter document (an ePub's
// spine items). MuPDF's fz_load_page finds it by walking the chapters from the
// first and asking each one's page count; for an ePub each of those asks walks
// the spine from its head, so one page in chapter c costs O(c^2) and sizing
// every page O(N^3). Document builds this table once at open and loads pages
// with fz_load_chapter_page. Pure, so it is testable without MuPDF.

namespace litepdf::core::detail {

struct ChapterLocation {
    int chapter = 0;
    int page = 0;   // within the chapter
};

class ChapterPages {
public:
    // counts[c] = the pages in chapter c. Nothing when the counts cannot be
    // the document's: a negative count, or a sum other than page_count.
    static std::optional<ChapterPages> from_counts(const std::vector<int>& counts,
                                                   std::size_t page_count) {
        std::vector<std::size_t> starts;
        starts.reserve(counts.size() + 1);
        std::size_t start = 0;
        for (int m : counts) {
            if (m < 0) return std::nullopt;
            starts.push_back(start);
            start += static_cast<std::size_t>(m);
        }
        if (start != page_count) return std::nullopt;
        starts.push_back(start);
        return ChapterPages(std::move(starts));
    }

    // The chapter holding flat page `index` and the page within it, as
    // fz_load_page would find them; nothing past the last page. A chapter of
    // 0 pages shares its start with the next chapter and is skipped: the walk
    // passes it because `index < start + 0` is never true.
    std::optional<ChapterLocation> locate(std::size_t index) const noexcept {
        if (index >= starts_.back()) return std::nullopt;
        // The first start greater than index, minus one: the last chapter
        // starting at or before it, which is the one with pages when several
        // share that start.
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), index);
        const std::size_t chapter = static_cast<std::size_t>(it - starts_.begin()) - 1;
        return ChapterLocation{static_cast<int>(chapter),
                               static_cast<int>(index - starts_[chapter])};
    }

    std::size_t chapter_count() const noexcept { return starts_.size() - 1; }

private:
    explicit ChapterPages(std::vector<std::size_t> starts) : starts_(std::move(starts)) {}

    // The first flat page of each chapter, then the page count. Never empty,
    // except in a moved-from object, which must not be used.
    std::vector<std::size_t> starts_;
};

}  // namespace litepdf::core::detail
