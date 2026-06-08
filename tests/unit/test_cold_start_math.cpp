// Phase 11.5 Task 1 — unit tests for the pure cold-start span helper.
// Mirrors test_splitter_math.cpp: tests the arithmetic the global
// ColdStartTimer delegates to, so the timer's QPC globals stay untested-but-thin.
#include "ui/detail/ColdStartMath.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::detail::span_ms;

TEST_CASE("span_ms: zero frequency yields 0 (guard before QPF ran)", "[cold_start_math]") {
    REQUIRE(span_ms(/*a=*/0, /*b=*/1000, /*freq=*/0) == 0);
}

TEST_CASE("span_ms: converts ticks to ms by frequency", "[cold_start_math]") {
    // freq = 1000 ticks/sec => 1 tick = 1 ms. b-a = 250 ticks => 250 ms.
    REQUIRE(span_ms(/*a=*/1000, /*b=*/1250, /*freq=*/1000) == 250);
}

TEST_CASE("span_ms: high-frequency counter scales correctly", "[cold_start_math]") {
    // freq = 10,000,000 (typical QPC). 3,000,000 ticks => 300 ms.
    REQUIRE(span_ms(/*a=*/0, /*b=*/3'000'000, /*freq=*/10'000'000) == 300);
}

TEST_CASE("span_ms: equal timestamps yield 0", "[cold_start_math]") {
    REQUIRE(span_ms(/*a=*/5'000, /*b=*/5'000, /*freq=*/10'000'000) == 0);
}
