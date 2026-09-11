// PR-A2 Task 2: pure-logic tests for the page-anchor lifetime.
//
// The anchor answers "where should the page land when the pixmap arrives?".
// It is keyed by the submission sequence number because (epoch, page, slot)
// does not identify a request -- a same-page zoom or resize produces a second
// P0 with an identical triple, and the older completion must not steal the
// newer render's anchor.

#include "ui/detail/PageAnchor.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::ui::AnchorSlot;
using litepdf::ui::PageAnchor;

namespace {
litepdf::core::SearchSession::Hit make_hit(std::size_t page, float top) {
    litepdf::core::SearchSession::Hit h{};
    h.page = page;
    h.geom.ul_y = top;
    h.geom.ur_y = top;
    h.geom.ll_y = top + 12.0f;
    h.geom.lr_y = top + 12.0f;
    return h;
}
}  // namespace

TEST_CASE("PageAnchor factories carry their kind", "[ui][anchor]") {
    REQUIRE(PageAnchor::none().kind   == PageAnchor::Kind::None);
    REQUIRE(PageAnchor::top().kind    == PageAnchor::Kind::Top);
    REQUIRE(PageAnchor::bottom().kind == PageAnchor::Kind::Bottom);

    const PageAnchor a = PageAnchor::hit(make_hit(4, 300.0f));
    REQUIRE(a.kind == PageAnchor::Kind::Hit);
    REQUIRE(a.target.page == 4u);
    REQUIRE(a.target.geom.ul_y == 300.0f);
}

TEST_CASE("PageAnchor slot starts empty", "[ui][anchor]") {
    AnchorSlot slot;
    REQUIRE_FALSE(slot.pending());
    REQUIRE_FALSE(slot.applied());
    // An empty slot never claims a completion, whatever seq it carries.
    REQUIRE(slot.take(0).kind == PageAnchor::Kind::None);
    REQUIRE(slot.take(1).kind == PageAnchor::Kind::None);
}

TEST_CASE("PageAnchor slot is taken only by its own seq", "[ui][anchor]") {
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(5);
    REQUIRE(slot.pending());

    // An older completion must not take it, and must not clear it either.
    REQUIRE(slot.take(4).kind == PageAnchor::Kind::None);
    REQUIRE(slot.pending());

    // A newer completion (should be impossible, but must not steal it either).
    REQUIRE(slot.take(6).kind == PageAnchor::Kind::None);
    REQUIRE(slot.pending());

    REQUIRE(slot.take(5).kind == PageAnchor::Kind::Top);
}

TEST_CASE("PageAnchor slot serves both halves of one spread", "[ui][anchor]") {
    // A spread's two renders share one seq and BOTH must apply the anchor: the
    // union height that Bottom measures against is only final once the second
    // half lands, and either half can arrive first.
    AnchorSlot slot;
    slot.install(PageAnchor::bottom());
    slot.stamp(9);

    REQUIRE(slot.take(9).kind == PageAnchor::Kind::Bottom);
    slot.mark_applied();
    // The other half of the same spread still gets it.
    REQUIRE(slot.take(9).kind == PageAnchor::Kind::Bottom);
    REQUIRE(slot.applied());
}

TEST_CASE("PageAnchor slot retires an applied anchor at the next stamp",
          "[ui][anchor]") {
    // This is what keeps a same-page re-render (zoom, resize, invert, pane
    // toggle) from re-applying a navigation that already happened -- the defect
    // that would otherwise snap the view back to the top on every Zoom In.
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(2);
    REQUIRE(slot.take(2).kind == PageAnchor::Kind::Top);
    slot.mark_applied();

    slot.stamp(3);                      // a new submission batch opens
    REQUIRE_FALSE(slot.pending());
    REQUIRE_FALSE(slot.applied());
    REQUIRE(slot.take(3).kind == PageAnchor::Kind::None);
}

TEST_CASE("PageAnchor slot carries an UNAPPLIED intent to a new seq",
          "[ui][anchor]") {
    // The failed-render recovery path. The render that was going to consume the
    // anchor never delivered (cancelled or failed -> null completion), so
    // mark_applied was never called and the retry inherits the intent.
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(2);
    slot.stamp(3);                      // resubmit; nothing was ever applied
    REQUIRE(slot.take(2).kind == PageAnchor::Kind::None);
    REQUIRE(slot.take(3).kind == PageAnchor::Kind::Top);
}

TEST_CASE("PageAnchor install replaces a pending anchor", "[ui][anchor]") {
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(2);
    slot.install(PageAnchor::hit(make_hit(7, 90.0f)));
    slot.stamp(3);

    const PageAnchor got = slot.take(3);
    REQUIRE(got.kind == PageAnchor::Kind::Hit);
    REQUIRE(got.target.page == 7u);
    REQUIRE(got.target.geom.ul_y == 90.0f);
}

TEST_CASE("PageAnchor install clears a previous applied mark", "[ui][anchor]") {
    // Otherwise a fresh navigation installed after an applied one would be
    // retired by its own stamp before any completion could see it.
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(2);
    (void)slot.take(2);
    slot.mark_applied();

    slot.install(PageAnchor::bottom());
    slot.stamp(3);
    REQUIRE(slot.pending());
    REQUIRE(slot.take(3).kind == PageAnchor::Kind::Bottom);
}

TEST_CASE("PageAnchor stamp on an empty slot installs nothing",
          "[ui][anchor]") {
    // Same-page re-renders (resize, DPI change, zoom, pane toggle, invert)
    // submit with no anchor. The completion must then KEEP the current pan and
    // merely re-clamp it -- which is the Kind::None branch at the call site.
    AnchorSlot slot;
    slot.stamp(11);
    REQUIRE_FALSE(slot.pending());
    REQUIRE(slot.take(11).kind == PageAnchor::Kind::None);
}

TEST_CASE("PageAnchor clear drops a pending anchor", "[ui][anchor]") {
    // set_view clears the slot: a new view means a new epoch, and the old
    // view's anchor describes a document that is no longer on screen.
    AnchorSlot slot;
    slot.install(PageAnchor::top());
    slot.stamp(4);
    slot.clear();
    REQUIRE_FALSE(slot.pending());
    REQUIRE_FALSE(slot.applied());
    REQUIRE(slot.take(4).kind == PageAnchor::Kind::None);
}
