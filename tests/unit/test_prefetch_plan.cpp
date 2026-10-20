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
    // fz_round_rect (floor(x0 + 0.001), ceil(x1 - 0.001)) makes a bbox at most
    // ceil(w) + 1 pixels wide, ceil(h) + 1 high, wherever the page sits: the
    // estimate covers that worst case, which at the origin is one pixel per
    // side more than the 9525x13471 MuPDF allocates there.
    const std::size_t est = estimated_pixmap_bytes(kA4W, kA4H, 16.0f);
    REQUIRE(est == std::size_t{9526} * 13472 * 4);
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

TEST_CASE("PrefetchPlan estimate saturates for a page box past size_t",
          "[core][prefetch]") {
    // A malformed page box: finite, but far past anything size_t holds.
    const std::size_t kMax = std::numeric_limits<std::size_t>::max();
    REQUIRE(estimated_pixmap_bytes(1e30f, 1e30f, 1.0f) == kMax);
    REQUIRE(estimated_pixmap_bytes(3e38f, kA4H, 8.0f) == kMax);
    // And a saturated page never fits, even beside nothing.
    const auto plan = prefetch_plan(kMax, 10, kMax, kDefaultL1BudgetBytes,
                                    TravelDirection::Forward, true, true);
    REQUIRE_FALSE(plan.prev);
    REQUIRE(plan.next);
}

TEST_CASE("PrefetchPlan prefetches both neighbours while three pages fit",
          "[core][prefetch]") {
    for (auto dir : {TravelDirection::Forward, TravelDirection::Backward}) {
        const auto plan = prefetch_plan(100, 100, 100, 300, dir, true, true);  // exactly
        REQUIRE(plan.prev);
        REQUIRE(plan.next);
    }
}

TEST_CASE("PrefetchPlan prefetches only the travel-direction neighbour past three",
          "[core][prefetch]") {
    const auto fwd = prefetch_plan(101, 100, 100, 300, TravelDirection::Forward,
                                   true, true);
    REQUIRE_FALSE(fwd.prev);
    REQUIRE(fwd.next);

    const auto back = prefetch_plan(101, 100, 100, 300, TravelDirection::Backward,
                                    true, true);
    REQUIRE(back.prev);
    REQUIRE_FALSE(back.next);

    // Still one neighbour when even a single page is over the budget: it is the
    // page the next turn shows, and the current page lives on as the canvas's
    // own bitmap.
    const auto huge = prefetch_plan(1000, 1000, 1000, 300, TravelDirection::Forward,
                                    true, true);
    REQUIRE_FALSE(huge.prev);
    REQUIRE(huge.next);
}

TEST_CASE("PrefetchPlan sizes each page of a mixed-size document on its own",
          "[core][prefetch]") {
    // A small page between two large ones: the three do not fit.
    const auto small_cur = prefetch_plan(10, 200, 200, 300, TravelDirection::Forward,
                                         true, true);
    REQUIRE_FALSE(small_cur.prev);
    REQUIRE(small_cur.next);

    // A large page between two small ones: they do.
    const auto large_cur = prefetch_plan(200, 10, 10, 300, TravelDirection::Forward,
                                         true, true);
    REQUIRE(large_cur.prev);
    REQUIRE(large_cur.next);

    // An absent neighbour's bytes do not count.
    const auto first = prefetch_plan(100, 999, 100, 300, TravelDirection::Forward,
                                     false, true);
    REQUIRE_FALSE(first.prev);
    REQUIRE(first.next);
}

TEST_CASE("PrefetchPlan on the first or last page takes the neighbour it has",
          "[core][prefetch]") {
    // Home from a later page reads as a move backward, and the reader then
    // pages forward: the first page's only neighbour is the one to prepare.
    const auto first = prefetch_plan(200, 0, 200, 300, TravelDirection::Backward,
                                     false, true);
    REQUIRE_FALSE(first.prev);
    REQUIRE(first.next);

    const auto last = prefetch_plan(200, 200, 0, 300, TravelDirection::Forward,
                                    true, false);
    REQUIRE(last.prev);
    REQUIRE_FALSE(last.next);

    // Never a neighbour the page lacks, while the pages fit or not.
    const auto fits = prefetch_plan(100, 0, 100, 300, TravelDirection::Forward,
                                    false, true);
    REQUIRE_FALSE(fits.prev);
    REQUIRE(fits.next);
    const auto only = prefetch_plan(400, 0, 0, 300, TravelDirection::Forward,
                                    false, false);
    REQUIRE_FALSE(only.prev);
    REQUIRE_FALSE(only.next);
}

TEST_CASE("PrefetchPlan with an unknown page size keeps today's both-neighbours",
          "[core][prefetch]") {
    // Unknown current page, and an unknown neighbour beside two large known ones.
    const auto cur = prefetch_plan(0, 200, 200, 300, TravelDirection::Forward,
                                   true, true);
    REQUIRE(cur.prev);
    REQUIRE(cur.next);
    const auto nb = prefetch_plan(200, 0, 200, 300, TravelDirection::Forward,
                                  true, true);
    REQUIRE(nb.prev);
    REQUIRE(nb.next);
}

TEST_CASE("PrefetchPlan at the default budget, A4 on a 200% display",
          "[core][prefetch]") {
    // 600%: 3 x 275 MiB fits in 1 GiB -- both neighbours, as before #49.
    const std::size_t r6 = estimated_pixmap_bytes(kA4W, kA4H, 12.0f);
    const auto r600 = prefetch_plan(r6, r6, r6, kDefaultL1BudgetBytes,
                                    TravelDirection::Forward, true, true);
    REQUIRE(r600.prev);
    REQUIRE(r600.next);

    // 800%: 3 x 489 MiB does not, two do -- the current page and the next one.
    const std::size_t top = estimated_pixmap_bytes(kA4W, kA4H, 16.0f);
    REQUIRE(2 * top <= kDefaultL1BudgetBytes);
    const auto r800 = prefetch_plan(top, top, top, kDefaultL1BudgetBytes,
                                    TravelDirection::Forward, true, true);
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
