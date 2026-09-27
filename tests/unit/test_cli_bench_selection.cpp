#include <catch2/catch_test_macros.hpp>

#include "cli/bench_selection.hpp"
#include "core/Document.hpp"

// Contract for litepdf-cli's `--bench-selection` (#73). The timings themselves
// are machine-dependent, so their size is never asserted; what is pinned is
// that every figure is actually measured (> 0) on a page with text, and that a
// page the harness cannot get a handle for fails loudly instead of printing a
// row of zeros.

namespace {
constexpr const char* kFixture = "tests/fixtures/selection.pdf";
}

TEST_CASE("cli bench-selection measures every figure on a page with text",
          "[cli][bench][selection]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open(kFixture).has_value());

    litepdf::cli::SelectionPageTiming t;
    REQUIRE(litepdf::cli::bench_selection_page(kFixture, doc, 0, 3, t));
    CHECK(t.page == 0);
    CHECK(t.chars > 0);
    CHECK(t.quads > 0);
    CHECK(t.scan_first_ms > 0.0);
    CHECK(t.acquire_first_ms > 0.0);
    CHECK(t.acquire_seq_ms > 0.0);
    CHECK(t.acquire_ms > 0.0);
    CHECK(t.search_ms > 0.0);
    for (int m = 0; m < 3; ++m) {
        CHECK(t.move_full_ms[m] > 0.0);
        CHECK(t.move_short_ms[m] > 0.0);
        CHECK(t.release_full_ms[m] > 0.0);
    }
    CHECK(t.select_all_ms > 0.0);
}

TEST_CASE("cli bench-selection fails on a page it cannot open",
          "[cli][bench][selection]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open(kFixture).has_value());

    litepdf::cli::SelectionPageTiming t;
    CHECK_FALSE(litepdf::cli::bench_selection_page(
        kFixture, doc, static_cast<int>(doc.page_count()), 1, t));
}
