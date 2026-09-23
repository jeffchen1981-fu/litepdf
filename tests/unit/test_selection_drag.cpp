// #52: the pure gesture logic PdfCanvas drives (ui/detail/SelectionDrag.hpp).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/SelectionDrag.hpp"

using litepdf::core::SelectMode;
using litepdf::ui::canvas_cursor;
using litepdf::ui::canvas_dip_to_page_point;
using litepdf::ui::CanvasCursor;
using litepdf::ui::ClickCounter;
using litepdf::ui::Gesture;
using litepdf::ui::GestureState;
using litepdf::ui::MouseButton;
using litepdf::ui::PanStep;
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

// --- #58 hand-tool panning -------------------------------------------------

TEST_CASE("SelectionDrag a pan step is the pointer motion since the previous step",
          "[ui][pan]") {
    GestureState g;
    REQUIRE(g.begin_pan(MouseButton::Middle, 100, 200));

    PanStep s = g.pan_step(130, 190);   // the first step measures from the press
    REQUIRE(s.dx_px == 30);
    REQUIRE(s.dy_px == -10);

    s = g.pan_step(125, 250);           // later steps from the previous step
    REQUIRE(s.dx_px == -5);
    REQUIRE(s.dy_px == 60);

    s = g.pan_step(125, 250);           // no motion, no pan
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);
}

TEST_CASE("SelectionDrag pan steps add up to the whole drag with no drift",
          "[ui][pan]") {
    // PdfCanvas applies each step to the pan as it is NOW, so the steps must sum
    // to (end - press) exactly, whatever path the pointer took -- including
    // outside the client area, where captured coordinates go negative.
    GestureState g;
    REQUIRE(g.begin_pan(MouseButton::Left, 50, 50));
    int sum_x = 0, sum_y = 0;
    const int path[][2] = { {60, 40}, {-30, 400}, {-1, -1}, {3000, -2000}, {90, 75} };
    for (const auto& p : path) {
        const PanStep s = g.pan_step(p[0], p[1]);
        sum_x += s.dx_px;
        sum_y += s.dy_px;
    }
    REQUIRE(sum_x == 90 - 50);
    REQUIRE(sum_y == 75 - 50);
}

TEST_CASE("SelectionDrag pan steps are zero unless a pan is live", "[ui][pan]") {
    GestureState g;
    PanStep s = g.pan_step(10, 10);                 // nothing live
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);

    REQUIRE(g.begin_select(SelectMode::Chars, 0, 0));
    s = g.pan_step(40, 40);                         // a SELECTION is live
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);
    REQUIRE(g.abort() == Gesture::Selecting);

    REQUIRE(g.begin_pan(MouseButton::Middle, 0, 0));
    (void)g.pan_step(500, 500);
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::EndPan);
    s = g.pan_step(600, 600);                       // the pan is over
    REQUIRE(s.dx_px == 0);
    REQUIRE(s.dy_px == 0);

    // A new pan measures from ITS press, not from where the last one ended.
    REQUIRE(g.begin_pan(MouseButton::Middle, 10, 10));
    s = g.pan_step(12, 13);
    REQUIRE(s.dx_px == 2);
    REQUIRE(s.dy_px == 3);
}

TEST_CASE("SelectionDrag a space pan belongs to the left button and never clears a selection",
          "[ui][pan]") {
    GestureState g;
    // A plain click first, so mode() is Chars and moved() is false -- the exact
    // state in which a LEFT release means "clear the selection".
    REQUIRE(g.begin_select(SelectMode::Chars, 5, 5));
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::ClearSelection);

    // Space + click with no movement: the same button, mode() Chars and moved()
    // false again, and it must still end as a pan. PdfCanvas's EndPan arm commits and clears
    // nothing, so a Space click keeps the reader's selection.
    REQUIRE(g.begin_pan(MouseButton::Left, 5, 5));
    REQUIRE_FALSE(g.begin_pan(MouseButton::Middle, 5, 5));      // one gesture at a time
    REQUIRE(g.release(MouseButton::Middle) == ReleaseAction::None);
    REQUIRE(g.gesture() == Gesture::Panning);
    REQUIRE(g.release(MouseButton::Left) == ReleaseAction::EndPan);
}

TEST_CASE("SelectionDrag a press that became a pan starts a new click sequence",
          "[ui][pan]") {
    const PointerMetrics m;

    // A real click, then Space + a quick second press, which Windows reports as
    // a double click and the canvas turns into a pan. A plain click right after
    // must be a single click, not the third of a triple (a whole line).
    ClickCounter a;
    REQUIRE(a.press(false, 1000, 100, 100, m) == SelectMode::Chars);
    (void)a.press(true, 1050, 100, 100, m);
    a.forget();
    REQUIRE(a.press(false, 1100, 100, 100, m) == SelectMode::Chars);

    // Space + a press (a pan), then a quick plain press that Windows reports as
    // a double click. Its first half was the pan, so it is a single click, not
    // a word.
    ClickCounter b;
    (void)b.press(false, 2000, 50, 50, m);
    b.forget();
    REQUIRE(b.press(true, 2100, 50, 50, m) == SelectMode::Chars);

    // Counting is normal again afterwards.
    REQUIRE(b.press(false, 3000, 50, 50, m) == SelectMode::Chars);
    REQUIRE(b.press(true, 3050, 50, 50, m) == SelectMode::Words);
}

TEST_CASE("SelectionDrag a quick double click right after a pan press selects a word",
          "[ui][pan]") {
    const PointerMetrics m;

    // #77: Space + a stationary click (a pan), then a real double click at the
    // same spot. Windows pairs the pan press with the double click's first
    // press, so that press arrives as WM_LBUTTONDBLCLK and counts as a single
    // click; the double click's second press then arrives as a plain press and
    // must be counted as the second click.
    ClickCounter c;
    (void)c.press(false, 1000, 50, 50, m);
    c.forget();
    REQUIRE(c.press(true,  1100, 50, 50, m) == SelectMode::Chars);
    REQUIRE(c.press(false, 1200, 51, 49, m) == SelectMode::Words);
    // A third quick press completes a triple click. Windows pairs it with the
    // second, so it too arrives as WM_LBUTTONDBLCLK.
    REQUIRE(c.press(true,  1300, 50, 50, m) == SelectMode::Lines);
    // The shift ends there: the next pair is an ordinary double click.
    REQUIRE(c.press(false, 2000, 50, 50, m) == SelectMode::Chars);
    REQUIRE(c.press(true,  2100, 50, 50, m) == SelectMode::Words);

    // The re-paired second click obeys the same time and distance limits.
    ClickCounter late;
    (void)late.press(false, 1000, 50, 50, m);
    late.forget();
    (void)late.press(true, 1100, 50, 50, m);
    REQUIRE(late.press(false, 1601, 50, 50, m) == SelectMode::Chars);

    ClickCounter far;
    (void)far.press(false, 1000, 50, 50, m);
    far.forget();
    (void)far.press(true, 1100, 50, 50, m);
    REQUIRE(far.press(false, 1200, 53, 50, m) == SelectMode::Chars);   // |dx| 3 > 4/2
}

TEST_CASE("SelectionDrag moved and mode read idle once a gesture ends", "[ui][selection]") {
    const PointerMetrics m;

    // #68: gesture() is the single source of truth for a live gesture, so the
    // finished gesture's moved() and mode() must not stay readable after it.
    GestureState released;
    REQUIRE(released.begin_select(SelectMode::Words, 0, 0));
    released.move(50, 0, m);
    REQUIRE(released.moved());
    REQUIRE(released.release(MouseButton::Left) == ReleaseAction::CommitSelection);
    REQUIRE(released.gesture() == Gesture::None);
    REQUIRE_FALSE(released.moved());
    REQUIRE(released.mode() == SelectMode::Chars);

    GestureState aborted;
    REQUIRE(aborted.begin_select(SelectMode::Lines, 0, 0));
    aborted.move(50, 0, m);
    REQUIRE(aborted.abort() == Gesture::Selecting);
    REQUIRE_FALSE(aborted.moved());
    REQUIRE(aborted.mode() == SelectMode::Chars);

    // A pan that moved leaves nothing behind either.
    GestureState panned;
    REQUIRE(panned.begin_pan(MouseButton::Middle, 0, 0));
    panned.move(50, 0, m);
    REQUIRE(panned.release(MouseButton::Middle) == ReleaseAction::EndPan);
    REQUIRE_FALSE(panned.moved());
}

TEST_CASE("SelectionDrag cursor precedence for selection and panning", "[ui][cursor]") {
    using C = CanvasCursor;
    //                     live               space  can_pan over_page
    // Nothing live, no space: the I-beam over a single-page page, else the arrow.
    REQUIRE(canvas_cursor(Gesture::None,      false, true,  true)  == C::IBeam);
    REQUIRE(canvas_cursor(Gesture::None,      false, true,  false) == C::Arrow);
    REQUIRE(canvas_cursor(Gesture::None,      false, false, true)  == C::IBeam);

    // Space held: the move cursor ANYWHERE in the client, margins included,
    // because a Space press pans from there too -- but only when something can
    // pan. Otherwise the arrow: a Space press would pan (a no-op), not select,
    // so an I-beam would promise a selection it will not make.
    REQUIRE(canvas_cursor(Gesture::None,      true,  true,  true)  == C::Move);
    REQUIRE(canvas_cursor(Gesture::None,      true,  true,  false) == C::Move);
    REQUIRE(canvas_cursor(Gesture::None,      true,  false, true)  == C::Arrow);

    // A live pan: the same rule, space or not.
    REQUIRE(canvas_cursor(Gesture::Panning,   false, true,  true)  == C::Move);
    REQUIRE(canvas_cursor(Gesture::Panning,   false, false, true)  == C::Arrow);

    // A live selection keeps the I-beam even if Space goes down mid-drag: Space
    // is read at the press, so it cannot turn this drag into a pan.
    REQUIRE(canvas_cursor(Gesture::Selecting, true,  true,  false) == C::IBeam);
    REQUIRE(canvas_cursor(Gesture::Selecting, false, false, false) == C::IBeam);
}
