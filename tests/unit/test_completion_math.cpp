// PR-A2 Task 1: pure-logic tests for the render-completion accept predicate.
//
// The spread case is the load-bearing one: in dual mode the RIGHT slot is
// submitted for left+1 while current_page() is already snapped to left, so a
// predicate that compared every completion against current_page() would reject
// every right-slot pixmap and leave half of every spread grey.

#include "ui/detail/CompletionMath.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::accept_completion;
using litepdf::ui::Slot;

// Argument order, to keep the calls below readable:
//   (meta_epoch, cur_epoch, meta_seq, newest_submitted_seq,
//    meta_page, meta_slot, cur_page, dual, page_count)

TEST_CASE("CompletionMath accepts a matching single-page completion",
          "[ui][completion]") {
    REQUIRE(accept_completion(7, 7, 5, 5, 4, Slot::Left, 4, false, 10));
}

TEST_CASE("CompletionMath rejects a completion from a superseded view epoch",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(6, 7, 5, 5, 4, Slot::Left, 4, false, 10));
    // Even a perfectly matching page loses to an epoch mismatch.
    REQUIRE_FALSE(accept_completion(0, 1, 1, 1, 0, Slot::Left, 0, false, 10));
}

TEST_CASE("CompletionMath rejects a superseded submission of the same page",
          "[ui][completion]") {
    // The duplicate-P0 defect. cancel_stale_renders(0) does not cancel an
    // in-flight P0 (RenderEngine cancels priority > p only), so a zoom, resize,
    // DPI change or invert toggle can leave two P0s for the SAME page racing.
    // (epoch, page, slot) are identical for both; only the seq differs, and the
    // older one must not repaint the canvas at the superseded scale -- INCLUDING
    // when it is the one that arrives first, which is why the comparison is
    // against the newest SUBMITTED seq and not the newest accepted one.
    REQUIRE_FALSE(accept_completion(7, 7, 4, 5, 4, Slot::Left, 4, false, 10));
    REQUIRE(accept_completion(7, 7, 5, 5, 4, Slot::Left, 4, false, 10));
    // A seq above the newest submitted cannot occur -- the counter is bumped
    // before the request is issued -- but must not be rejected if it somehow
    // does: dropping a live render is worse than accepting an impossible one.
    REQUIRE(accept_completion(7, 7, 6, 5, 4, Slot::Left, 4, false, 10));
}

TEST_CASE("CompletionMath accepts both halves of one spread submission",
          "[ui][completion]") {
    // Both slots of a spread carry the SAME seq, and that seq is also the
    // newest submitted, so the test must use >= and not >. With > NOTHING would
    // ever be painted: every completion of the current batch would lose to the
    // counter that issued it.
    REQUIRE(accept_completion(7, 7, 5, 5, 3, Slot::Left,  3, true, 10));
    REQUIRE(accept_completion(7, 7, 5, 5, 4, Slot::Right, 3, true, 10));
}

TEST_CASE("CompletionMath rejects a left completion for another page",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 3, Slot::Left, 4, false, 10));
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 5, Slot::Left, 4, false, 10));
}

TEST_CASE("CompletionMath rejects a right completion in single-page mode",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 4, Slot::Right, 4, false, 10));
    // Not even the page that WOULD be the spread partner is accepted.
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 5, Slot::Right, 4, false, 10));
}

TEST_CASE("CompletionMath accepts the right completion for left plus one",
          "[ui][completion]") {
    // The spread-blanking regression. Pair (3,4), current_page snapped to 3.
    REQUIRE(accept_completion(7, 7, 5, 5, 3, Slot::Left,  3, true, 10));
    REQUIRE(accept_completion(7, 7, 5, 5, 4, Slot::Right, 3, true, 10));
    // And the left slot still rejects the partner page.
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 4, Slot::Left, 3, true, 10));
}

TEST_CASE("CompletionMath re-snaps an unsnapped current page in dual mode",
          "[ui][completion]") {
    // Every submission path snaps current_page to the pair's LEFT before
    // submitting, so this is defence in depth rather than a live path. It
    // costs one call and makes the predicate independent of call ordering.
    // Page 4 belongs to pair (3,4): left 3, right 4.
    REQUIRE(accept_completion(7, 7, 5, 5, 3, Slot::Left,  4, true, 10));
    REQUIRE(accept_completion(7, 7, 5, 5, 4, Slot::Right, 4, true, 10));
}

TEST_CASE("CompletionMath rejects a right completion for a pair that has none",
          "[ui][completion]") {
    // Cover page: page 0 renders alone, dual_page_compute_right returns -1.
    REQUIRE(accept_completion(7, 7, 5, 5, 0, Slot::Left, 0, true, 10));
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 1, Slot::Right, 0, true, 10));
    // Odd tail: 4-page document, pair (3,-). Left 3 exists, right does not.
    REQUIRE(accept_completion(7, 7, 5, 5, 3, Slot::Left, 3, true, 4));
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 4, Slot::Right, 3, true, 4));
}

TEST_CASE("CompletionMath rejects everything for a document with no pages",
          "[ui][completion]") {
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 0, Slot::Left,  0, false, 0));
    REQUIRE_FALSE(accept_completion(7, 7, 5, 5, 0, Slot::Right, 0, true,  0));
}
