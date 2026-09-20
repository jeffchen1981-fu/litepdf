// #52: the pure gesture logic PdfCanvas drives (ui/detail/SelectionDrag.hpp).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/SelectionDrag.hpp"

using litepdf::core::SelectMode;
using litepdf::ui::canvas_dip_to_page_point;
using litepdf::ui::ClickCounter;
using litepdf::ui::Gesture;
using litepdf::ui::GestureState;
using litepdf::ui::MouseButton;
using litepdf::ui::Placement;
using litepdf::ui::PointerMetrics;
using litepdf::ui::ReleaseAction;

TEST_CASE("SelectionDrag click count maps presses to select modes", "[ui][selection]") {
    const PointerMetrics m;   // dblclk 500 ms, 4 x 4 px rectangle
    ClickCounter clicks;
    REQUIRE(clicks.press(false, 1000, 50, 50, m) == SelectMode::Chars);
    REQUIRE(clicks.press(true,  1200, 50, 50, m) == SelectMode::Words);
    REQUIRE(clicks.press(false, 1400, 51, 49, m) == SelectMode::Lines);
    // A fourth press is a fresh single click. (Win32 itself reports the fourth
    // as the second half of a new double click, i.e. WM_LBUTTONDBLCLK -> Words.)
    REQUIRE(clicks.press(false, 1600, 50, 50, m) == SelectMode::Chars);
}

TEST_CASE("SelectionDrag a third press too late or too far is a single click",
          "[ui][selection]") {
    const PointerMetrics m;
    ClickCounter late;
    late.press(false, 1000, 50, 50, m);
    late.press(true,  1100, 50, 50, m);
    REQUIRE(late.press(false, 1601, 50, 50, m) == SelectMode::Chars);

    ClickCounter far;
    far.press(false, 1000, 50, 50, m);
    far.press(true,  1100, 50, 50, m);
    REQUIRE(far.press(false, 1200, 53, 50, m) == SelectMode::Chars);   // |dx| 3 > 4/2

    ClickCounter plain;   // a single click followed by another is not a triple
    plain.press(false, 1000, 50, 50, m);
    REQUIRE(plain.press(false, 1100, 50, 50, m) == SelectMode::Chars);
}

TEST_CASE("SelectionDrag triple click timing survives the message clock wrapping",
          "[ui][selection]") {
    const PointerMetrics m;
    ClickCounter clicks;
    clicks.press(false, 0xFFFFFE00u, 50, 50, m);
    clicks.press(true,  0xFFFFFF00u, 50, 50, m);
    // 0x100 ms after the double click, across the 32-bit wrap.
    REQUIRE(clicks.press(false, 0x00000000u, 50, 50, m) == SelectMode::Lines);
}

TEST_CASE("SelectionDrag a click that never moves clears and a drag commits",
          "[ui][selection]") {
    const PointerMetrics m;   // drag threshold 4 px
    GestureState g;

    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(104, 96, m);                                   // exactly at the threshold
    REQUIRE_FALSE(g.moved());
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);
    REQUIRE(g.gesture() == Gesture::None);

    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(105, 100, m);                                  // one past it
    g.move(100, 100, m);                                  // and back: still a drag
    REQUIRE(g.moved());
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::CommitSelection);
}

TEST_CASE("SelectionDrag a stationary double click commits a word", "[ui][selection]") {
    // CS_DBLCLKS delivers DOWN, UP, DBLCLK, UP -- and no WM_MOUSEMOVE at all.
    // A rule that cleared every release without movement would destroy the word
    // the double click just selected (spec §4.2).
    const PointerMetrics m;
    ClickCounter clicks;
    GestureState g;

    SelectMode mode = clicks.press(false, 1000, 50, 50, m);
    REQUIRE(g.begin_select(mode, 50, 50));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);

    mode = clicks.press(true, 1100, 50, 50, m);
    REQUIRE(mode == SelectMode::Words);
    REQUIRE(g.begin_select(mode, 50, 50));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::CommitSelection);
}

TEST_CASE("SelectionDrag release decides before the capture changed abort runs",
          "[ui][selection]") {
    const PointerMetrics m;
    GestureState g;

    // PdfCanvas order: release() first, then ReleaseCapture -- whose synchronous
    // WM_CAPTURECHANGED calls abort(). abort() must find nothing live.
    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(140, 100, m);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::CommitSelection);
    REQUIRE(g.abort() == Gesture::None);

    // The other order loses the drag: abort() ends it, release() has nothing to
    // decide, and a selection highlighted under the held button copies nothing.
    REQUIRE(g.begin_select(SelectMode::Chars, 100, 100));
    g.move(140, 100, m);
    REQUIRE(g.abort() == Gesture::Selecting);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::None);
}

TEST_CASE("SelectionDrag only one gesture may be live under interleaved buttons",
          "[ui][selection]") {
    GestureState g;

    REQUIRE(g.begin_select(SelectMode::Chars, 10, 10));
    REQUIRE_FALSE(g.begin_pan(MouseButton::Middle, 10, 10));
    REQUIRE_FALSE(g.begin_select(SelectMode::Words, 10, 10));
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::None);   // not its gesture
    REQUIRE(g.gesture() == Gesture::Selecting);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);

    REQUIRE(g.begin_pan(MouseButton::Middle, 0, 0));
    REQUIRE_FALSE(g.begin_select(SelectMode::Chars, 0, 0));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::None);
    REQUIRE(g.gesture() == Gesture::Panning);
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::EndPan);
    REQUIRE(g.gesture() == Gesture::None);
}

TEST_CASE("SelectionDrag losing the capture ends a pan as well as a selection",
          "[ui][selection]") {
    // Were Panning left out of abort(), a pan interrupted by another window
    // taking the capture would leave the canvas refusing every later press.
    GestureState g;
    REQUIRE(g.begin_pan(MouseButton::Middle, 0, 0));
    REQUIRE(g.abort() == Gesture::Panning);
    REQUIRE(g.begin_select(SelectMode::Chars, 0, 0));
    REQUIRE(g.abort() == Gesture::Selecting);
    REQUIRE(g.abort() == Gesture::None);
}

TEST_CASE("SelectionDrag canvas position maps to a clamped page point",
          "[ui][selection]") {
    // US Letter at 150%, drawn with its top-left corner at canvas (100, 50).
    const Placement page{ 100.0f, 50.0f, 612.0f * 1.5f, 792.0f * 1.5f };

    const auto inside = canvas_dip_to_page_point(100.0f + 150.0f, 50.0f + 300.0f, page, 1.5f);
    REQUIRE(inside.x == Catch::Approx(100.0f));
    REQUIRE(inside.y == Catch::Approx(200.0f));

    const auto before = canvas_dip_to_page_point(0.0f, 0.0f, page, 1.5f);
    REQUIRE(before.x == 0.0f);
    REQUIRE(before.y == 0.0f);

    const auto after = canvas_dip_to_page_point(5000.0f, 5000.0f, page, 1.5f);
    REQUIRE(after.x == Catch::Approx(612.0f));
    REQUIRE(after.y == Catch::Approx(792.0f));

    const auto no_zoom = canvas_dip_to_page_point(250.0f, 350.0f, page, 0.0f);
    REQUIRE(no_zoom.x == 0.0f);
    REQUIRE(no_zoom.y == 0.0f);
}
