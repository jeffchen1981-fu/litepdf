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

using litepdf::ui::detail::clamp_bottom_panel_height;
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

// #93: the results panel's stored height is clamped against the live space
// on every layout. Arguments: (stored_h, avail_h, min_panel_h, min_canvas_h),
// where avail_h is the strip the canvas and the panel share.

TEST_CASE("clamp_bottom_panel_height: a height that fits is kept", "[splitter_math]") {
    REQUIRE(clamp_bottom_panel_height(300, 1000, 160, 200) == 300);
}

TEST_CASE("clamp_bottom_panel_height: a tall panel leaves the canvas its minimum",
          "[splitter_math]") {
    // The #93 repro: 660 px stored, 718 px shared -> the canvas kept 58 px.
    REQUIRE(clamp_bottom_panel_height(660, 718, 160, 200) == 518);
}

TEST_CASE("clamp_bottom_panel_height: a short panel is raised to its minimum",
          "[splitter_math]") {
    REQUIRE(clamp_bottom_panel_height(50, 1000, 160, 200) == 160);
}

TEST_CASE("clamp_bottom_panel_height: the panel minimum wins over the canvas minimum",
          "[splitter_math]") {
    // 300 px cannot hold both minimums; the query row stays reachable.
    REQUIRE(clamp_bottom_panel_height(660, 300, 160, 200) == 160);
}

TEST_CASE("clamp_bottom_panel_height: never taller than the shared strip",
          "[splitter_math]") {
    // Shorter than the panel minimum: the canvas goes to 0, never below,
    // so the splitter cannot be pushed above the strip's top.
    REQUIRE(clamp_bottom_panel_height(660, 100, 160, 200) == 100);
    REQUIRE(clamp_bottom_panel_height(660, 0, 160, 200) == 0);
    REQUIRE(clamp_bottom_panel_height(660, -20, 160, 200) == 0);
}
