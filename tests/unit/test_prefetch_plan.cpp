// #49: pure-logic tests for the L1 byte budget's prefetch policy.
//
// A4 is 595.276 x 841.89 pt. On a 200% display (dpi 192) the 800% rung is a
// render scale of 16 and the 600% rung a scale of 12; MuPDF rasterises those
// to 9525x13471 and 7144x10103 BGRA pixels (4 bytes each).

#include "core/PageCache.hpp"
#include "core/detail/PrefetchPlan.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

using litepdf::core::estimated_pixmap_bytes;
using litepdf::core::kDefaultL1BudgetBytes;
using litepdf::core::prefetch_plan;
using litepdf::core::travel_direction;
using litepdf::core::TravelDirection;

namespace {
constexpr float kA4W = 595.276f;
constexpr float kA4H = 841.89f;
}  // namespace

TEST_CASE("PrefetchPlan estimate brackets the pixmap MuPDF allocates",
          "[core][prefetch]") {
    const std::size_t real = std::size_t{9525} * 13471 * 4;
    const std::size_t est  = estimated_pixmap_bytes(kA4W, kA4H, 16.0f);
    // At most one pixel of rounding per edge either way of the real bbox.
    REQUIRE(est >= real);
    REQUIRE(est <= std::size_t{9527} * 13473 * 4);
}

TEST_CASE("PrefetchPlan estimate is 0 (unknown) for a degenerate input",
          "[core][prefetch]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(estimated_pixmap_bytes(0.0f, kA4H, 1.0f) == 0);
    REQUIRE(estimated_pixmap_bytes(kA4W, -1.0f, 1.0f) == 0);
    REQUIRE(estimated_pixmap_bytes(kA4W, kA4H, 0.0f) == 0);
    REQUIRE(estimated_pixmap_bytes(nan, kA4H, 1.0f) == 0);
    REQUIRE(estimated_pixmap_bytes(kA4W, kA4H, inf) == 0);
}

TEST_CASE("PrefetchPlan prefetches both neighbours while three pages fit",
          "[core][prefetch]") {
    for (auto dir : {TravelDirection::Forward, TravelDirection::Backward}) {
        const auto plan = prefetch_plan(100, 300, dir);  // exactly three
        REQUIRE(plan.prev);
        REQUIRE(plan.next);
    }
}

TEST_CASE("PrefetchPlan prefetches only the travel-direction neighbour past three",
          "[core][prefetch]") {
    const auto fwd = prefetch_plan(101, 300, TravelDirection::Forward);
    REQUIRE_FALSE(fwd.prev);
    REQUIRE(fwd.next);

    const auto back = prefetch_plan(101, 300, TravelDirection::Backward);
    REQUIRE(back.prev);
    REQUIRE_FALSE(back.next);

    // Still one neighbour when even a single page is over the budget: it is the
    // page the next turn shows, and the current page lives on as the canvas's
    // own bitmap.
    const auto huge = prefetch_plan(1000, 300, TravelDirection::Forward);
    REQUIRE_FALSE(huge.prev);
    REQUIRE(huge.next);
}

TEST_CASE("PrefetchPlan with an unknown page size keeps today's both-neighbours",
          "[core][prefetch]") {
    const auto plan = prefetch_plan(0, 300, TravelDirection::Forward);
    REQUIRE(plan.prev);
    REQUIRE(plan.next);
}

TEST_CASE("PrefetchPlan at the default budget, A4 on a 200% display",
          "[core][prefetch]") {
    // 600%: 3 x 275 MiB fits in 1 GiB -- both neighbours, as before #49.
    const auto r600 = prefetch_plan(estimated_pixmap_bytes(kA4W, kA4H, 12.0f),
                                    kDefaultL1BudgetBytes, TravelDirection::Forward);
    REQUIRE(r600.prev);
    REQUIRE(r600.next);

    // 800%: 3 x 489 MiB does not, two do -- the current page and the next one.
    const std::size_t top = estimated_pixmap_bytes(kA4W, kA4H, 16.0f);
    REQUIRE(2 * top <= kDefaultL1BudgetBytes);
    const auto r800 = prefetch_plan(top, kDefaultL1BudgetBytes,
                                    TravelDirection::Forward);
    REQUIRE_FALSE(r800.prev);
    REQUIRE(r800.next);
}

TEST_CASE("PrefetchPlan travel direction follows the page change",
          "[core][prefetch]") {
    // No previous page yet: keep the default.
    REQUIRE(travel_direction(3, -1, TravelDirection::Forward) ==
            TravelDirection::Forward);
    REQUIRE(travel_direction(4, 3, TravelDirection::Backward) ==
            TravelDirection::Forward);
    REQUIRE(travel_direction(2, 3, TravelDirection::Forward) ==
            TravelDirection::Backward);
    // A resubmit of the same page (zoom step, resize, device loss) keeps the
    // direction the reader was going in.
    REQUIRE(travel_direction(3, 3, TravelDirection::Backward) ==
            TravelDirection::Backward);
    REQUIRE(travel_direction(3, 3, TravelDirection::Forward) ==
            TravelDirection::Forward);
}
