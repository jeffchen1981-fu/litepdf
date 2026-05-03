// Phase 8 Task 4: pure-logic tests for the cover-page + odd-tail rules.

#include "ui/PdfCanvasLayout.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::dual_page_compute_left;
using litepdf::ui::dual_page_compute_right;
using litepdf::ui::dual_page_step_next_left;
using litepdf::ui::dual_page_step_prev_left;

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

// Phase 8 Task 4 regression: cover→PgDn must land on spread (1,2), NOT
// on page 2 alone. The naive `cur_left + 2` stride overshoots from
// cover (0+2=2) and silently skips page 1; dual_page_step_next_left
// handles the 0→1 bootstrap explicitly. (Surfaced by the T4 reviewer
// pass on commit 50fb402.)
TEST_CASE("dual_page: PgDn from cover lands on first spread, not page 2",
          "[dual_page][regression]") {
    REQUIRE(dual_page_step_next_left(/*cur_left=*/0, /*total=*/3)  == 1);
    REQUIRE(dual_page_step_next_left(/*cur_left=*/0, /*total=*/10) == 1);
}

TEST_CASE("dual_page: step_next_left walks pair grid then clamps to last",
          "[dual_page][step]") {
    // 3-page doc: cover, then (1,2). Last LEFT clamp = 2 (alone).
    REQUIRE(dual_page_step_next_left(0, 3) == 1);
    REQUIRE(dual_page_step_next_left(1, 3) == 2);
    REQUIRE(dual_page_step_next_left(2, 3) == 2);  // already at last
    // 10-page doc: pairs (0,-) (1,2) (3,4) (5,6) (7,8) (9,-).
    REQUIRE(dual_page_step_next_left(1, 10) == 3);
    REQUIRE(dual_page_step_next_left(7, 10) == 9);
    REQUIRE(dual_page_step_next_left(9, 10) == 9);
    // 1-page doc: only the cover exists.
    REQUIRE(dual_page_step_next_left(0, 1) == 0);
}

TEST_CASE("dual_page: step_prev_left snaps to cover and walks back by 2",
          "[dual_page][step]") {
    REQUIRE(dual_page_step_prev_left(0, 10) == 0);  // already cover
    REQUIRE(dual_page_step_prev_left(1, 10) == 0);  // first spread → cover
    REQUIRE(dual_page_step_prev_left(3, 10) == 1);
    REQUIRE(dual_page_step_prev_left(5, 10) == 3);
    REQUIRE(dual_page_step_prev_left(9, 10) == 7);
}
