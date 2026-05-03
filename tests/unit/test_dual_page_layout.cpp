// Phase 8 Task 4: pure-logic tests for the cover-page + odd-tail rules.

#include "ui/PdfCanvasLayout.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::dual_page_compute_left;
using litepdf::ui::dual_page_compute_right;

TEST_CASE("dual_page: cover page 0 is alone", "[dual_page]") {
    REQUIRE(dual_page_compute_left(0, 10) == 0);
    REQUIRE(dual_page_compute_right(0, 10) == -1);
}

TEST_CASE("dual_page: pairs from page 1 onward", "[dual_page]") {
    REQUIRE(dual_page_compute_left(1, 10) == 1);
    REQUIRE(dual_page_compute_left(2, 10) == 1);
    REQUIRE(dual_page_compute_left(3, 10) == 3);
    REQUIRE(dual_page_compute_left(4, 10) == 3);
    REQUIRE(dual_page_compute_left(5, 10) == 5);
    REQUIRE(dual_page_compute_left(6, 10) == 5);
    REQUIRE(dual_page_compute_right(1, 10) == 2);
    REQUIRE(dual_page_compute_right(3, 10) == 4);
    REQUIRE(dual_page_compute_right(5, 10) == 6);
}

TEST_CASE("dual_page: odd-tail last page renders alone", "[dual_page]") {
    // 9-page doc: pairs (0,-) (1,2) (3,4) (5,6) (7,8). Page 8 is right of 7.
    REQUIRE(dual_page_compute_left(8, 9) == 7);
    REQUIRE(dual_page_compute_right(7, 9) == 8);
    // 10-page doc: pairs (0,-) (1,2) (3,4) (5,6) (7,8) (9,-).
    REQUIRE(dual_page_compute_left(9, 10) == 9);
    REQUIRE(dual_page_compute_right(9, 10) == -1);
}
