// #230: flat page number -> (chapter, page), the table Document uses so it
// does not walk the chapters from the first on every page it loads.
#include "core/detail/ChapterPages.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <vector>

using litepdf::core::detail::ChapterLocation;
using litepdf::core::detail::ChapterPages;

namespace {

// What fz_load_page does: walk the chapters from the first until the running
// sum passes the index.
std::optional<ChapterLocation> walk(const std::vector<int>& counts, std::size_t index) {
    std::size_t start = 0;
    for (std::size_t c = 0; c < counts.size(); ++c) {
        const std::size_t m = static_cast<std::size_t>(counts[c]);
        if (index < start + m)
            return ChapterLocation{static_cast<int>(c), static_cast<int>(index - start)};
        start += m;
    }
    return std::nullopt;
}

std::size_t sum(const std::vector<int>& counts) {
    std::size_t s = 0;
    for (int m : counts) s += static_cast<std::size_t>(m);
    return s;
}

}  // namespace

TEST_CASE("ChapterPages: locate matches a walk from the first chapter", "[core][chapters]") {
    const std::vector<std::vector<int>> tables = {
        {1},
        {3, 1, 4},
        {0, 2, 0, 0, 3, 0},   // empty chapters at the start, middle and end
        {5, 0, 1, 1, 2, 7},
    };
    for (const auto& counts : tables) {
        const std::size_t n = sum(counts);
        const auto table = ChapterPages::from_counts(counts, n);
        REQUIRE(table.has_value());
        CHECK(table->chapter_count() == counts.size());
        for (std::size_t i = 0; i <= n; ++i) {   // one past the end too
            INFO("table of " << counts.size() << " chapters, index " << i);
            const auto want = walk(counts, i);
            const auto got = table->locate(i);
            REQUIRE(got.has_value() == want.has_value());
            if (want) {
                CHECK(got->chapter == want->chapter);
                CHECK(got->page == want->page);
            }
        }
    }
}

TEST_CASE("ChapterPages: refuses counts that do not describe the document", "[core][chapters]") {
    CHECK_FALSE(ChapterPages::from_counts({2, -1, 3}, 4).has_value());
    CHECK_FALSE(ChapterPages::from_counts({2, 3}, 4).has_value());
    CHECK_FALSE(ChapterPages::from_counts({2, 3}, 6).has_value());
    const auto empty = ChapterPages::from_counts({}, 0);
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->locate(0).has_value());
}
