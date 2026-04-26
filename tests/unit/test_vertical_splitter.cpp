// Phase 7 Task 4b — unit tests for VerticalSplitter clamp math.
//
// VerticalSplitter is a thin wrapper over detail::SplitterCore whose drag
// dispatch delegates the orientation-specific clamp to compute_drag_target_x
// (declared in ui/detail/SplitterMath.hpp, exercised here directly as a free
// helper — same approach as test_splitter_math.cpp from T4a).
//
// We do NOT exercise ctor/dtor here: VerticalSplitter requires a real
// HINSTANCE + parent HWND, which the unit test runner doesn't provide. The
// widget gets exercised via MainWindow integration in T8.

#include "ui/detail/SplitterMath.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::detail::compute_drag_target_x;

TEST_CASE("VerticalSplitter math: clamps below min", "[vsplitter]") {
    REQUIRE(compute_drag_target_x(50, 1000, 150, 800) == 150);
}

TEST_CASE("VerticalSplitter math: clamps above max", "[vsplitter]") {
    REQUIRE(compute_drag_target_x(900, 1000, 150, 800) == 800);
}
