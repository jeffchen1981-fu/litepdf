// #117: bookkeeping for the thumbnail pane's in-flight renders.
//
// The pane clears its pending set when it cancels on its own (document swap,
// hide, DPI change, renderer swap), so a cancel notice from the old request
// can arrive after a repaint has already asked for the same page again. That
// stale notice must not drop the newer request, or the pane redraws the row
// and renders the page twice.

#include "ui/detail/ThumbRequests.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::ThumbRequests;

TEST_CASE("ThumbRequests: a page already in flight is not started twice",
          "[ui][thumb_requests]") {
    ThumbRequests r;
    const auto first = r.try_start(3);
    REQUIRE(first.has_value());
    REQUIRE_FALSE(r.try_start(3).has_value());
    REQUIRE(r.try_start(4).has_value());
}

TEST_CASE("ThumbRequests: a cancel for the pending request frees the page and asks for a redraw",
          "[ui][thumb_requests]") {
    ThumbRequests r;
    const auto id = r.try_start(3);
    REQUIRE(id.has_value());
    REQUIRE(r.on_canceled(3, *id));
    REQUIRE(r.try_start(3).has_value());
}

TEST_CASE("ThumbRequests: a stale cancel leaves a newer request alone",
          "[ui][thumb_requests]") {
    ThumbRequests r;
    const auto old_id = r.try_start(3);
    REQUIRE(old_id.has_value());
    r.clear();  // the pane cancelled on its own
    const auto new_id = r.try_start(3);
    REQUIRE(new_id.has_value());
    REQUIRE(*new_id != *old_id);

    REQUIRE_FALSE(r.on_canceled(3, *old_id));   // no redraw
    REQUIRE_FALSE(r.try_start(3).has_value());  // the newer one is still pending
    REQUIRE(r.on_canceled(3, *new_id));
}

TEST_CASE("ThumbRequests: a cancel for a page with nothing pending still asks for a redraw",
          "[ui][thumb_requests]") {
    // After clear() nothing is pending for the page; the row may still be on
    // screen as a placeholder, so one redraw (one new request) is wanted.
    ThumbRequests r;
    const auto id = r.try_start(3);
    REQUIRE(id.has_value());
    r.clear();
    REQUIRE(r.on_canceled(3, *id));
    REQUIRE(r.try_start(3).has_value());
}

TEST_CASE("ThumbRequests: a finished render frees the page",
          "[ui][thumb_requests]") {
    ThumbRequests r;
    REQUIRE(r.try_start(3).has_value());
    r.on_finished(3);
    REQUIRE(r.try_start(3).has_value());
}
