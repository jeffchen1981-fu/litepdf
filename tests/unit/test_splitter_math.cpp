// Phase 7 Task 4a — unit tests for SplitterCore clamp helpers.
// These cover the pure math the WndProc delegates to during a drag, so
// vertical-orientation reuse (Phase 7 T4b) lands on a tested base.
//
// Both helpers take 4 args per plan §T4a §4a.1 (mouse, parent, min, max).
// Y uses (parent_h - mouse_y) before clamping (panel anchored at bottom);
// X uses mouse_x directly (parent_w intentionally unused, kept for
// signature symmetry).

#include "ui/detail/SplitterMath.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::detail::compute_drag_target_x;
using litepdf::ui::detail::compute_drag_target_y;

TEST_CASE("compute_drag_target_y: clamps below min", "[splitter_math]") {
    // mouse_y near bottom of parent -> small panel -> clamp up to min.
    // parent_h - mouse_y = 1000 - 950 = 50, clamp(50, 100, 800) = 100.
    REQUIRE(compute_drag_target_y(/*mouse_y=*/950, /*parent_h=*/1000,
                                  /*min_h=*/100, /*max_h=*/800) == 100);
}

TEST_CASE("compute_drag_target_y: clamps above max", "[splitter_math]") {
    // mouse_y near top of parent -> large panel -> clamp down to max.
    // parent_h - mouse_y = 1000 - 100 = 900, clamp(900, 100, 800) = 800.
    REQUIRE(compute_drag_target_y(/*mouse_y=*/100, /*parent_h=*/1000,
                                  /*min_h=*/100, /*max_h=*/800) == 800);
}

TEST_CASE("compute_drag_target_x: clamps below min", "[splitter_math]") {
    // mouse_x = 50 (parent_w unused) -> clamp(50, 150, 800) = 150.
    REQUIRE(compute_drag_target_x(/*mouse_x=*/50, /*parent_w=*/1000,
                                  /*min_w=*/150, /*max_w=*/800) == 150);
}

TEST_CASE("compute_drag_target_x: clamps above max", "[splitter_math]") {
    // mouse_x = 900 (parent_w unused) -> clamp(900, 150, 800) = 800.
    REQUIRE(compute_drag_target_x(/*mouse_x=*/900, /*parent_w=*/1000,
                                  /*min_w=*/150, /*max_w=*/800) == 800);
}
