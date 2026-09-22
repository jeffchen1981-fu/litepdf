#pragma once

// #52 / #58: pure gesture logic for PdfCanvas text selection and hand-tool
// panning. No Win32, no Direct2D, no MuPDF -- headless-testable, the same
// pattern as ViewportMath.hpp and SplitterMath.hpp. PdfCanvas feeds it message
// coordinates and system metrics and acts on what it returns.

#include "core/TextSelection.hpp"
#include "ui/detail/ViewportMath.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace litepdf::ui {

// Exactly one gesture owns the mouse capture at a time. One enum, not a flag per
// gesture: two independent booleans are what let a middle-button pan start in
// the middle of a left-button selection drag (spec §4.2).
enum class Gesture { None, Selecting, Panning };

// The button whose release ends the live gesture.
enum class MouseButton { Left, Middle };

// System metrics, read by PdfCanvas at message time (GetSystemMetrics,
// GetDoubleClickTime) and passed in so tests can pin them. The defaults are the
// values measured on the development machine.
struct PointerMetrics {
    int           drag_cx   = 4;     // SM_CXDRAG
    int           drag_cy   = 4;     // SM_CYDRAG
    int           dblclk_cx = 4;     // SM_CXDOUBLECLK
    int           dblclk_cy = 4;     // SM_CYDOUBLECLK
    std::uint32_t dblclk_ms = 500;   // GetDoubleClickTime()
};

// Presses -> selection granularity. With CS_DBLCLKS, Win32 reports the second
// press of a pair as WM_LBUTTONDBLCLK but has no triple-click message: the third
// press arrives as a plain WM_LBUTTONDOWN, recognised here by its time and
// distance from the double click.
class ClickCounter {
public:
    core::SelectMode press(bool is_double_click_message, std::uint32_t time_ms,
                           int x_px, int y_px, const PointerMetrics& m) noexcept {
        int count = 1;
        bool shifted = false;
        if (is_double_click_message) {
            if (restart_) {
                // After forget(), Windows' double click pairs this press with a
                // press that was not a click, so it starts a new sequence. From
                // here Windows' pairs run one press behind this sequence (#77):
                // it reports the second click as a plain press and pairs the
                // third with it.
                shifted = true;
            } else {
                count = (shifted_ && last_count_ == 2) ? 3 : 2;
            }
        } else if (follows_quickly(time_ms, x_px, y_px, m)) {
            if (last_count_ == 2) {
                count = 3;
            } else if (shifted_) {
                count   = 2;
                shifted = true;
            }
        }
        restart_ = false;
        shifted_ = shifted;
        last_count_   = count;
        last_time_ms_ = time_ms;
        last_x_       = x_px;
        last_y_       = y_px;
        switch (count) {
            case 2:  return core::SelectMode::Words;
            case 3:  return core::SelectMode::Lines;
            default: return core::SelectMode::Chars;
        }
    }

    // The press just counted was not a click: it started a pan (#58). The next
    // press begins a new sequence -- a single click, even when Windows reports
    // it as the second half of a double click. Without this, Space + click
    // followed by a quick plain click selects a word, and a click, Space + a
    // second press, then a plain click selects a whole line.
    //
    // PdfCanvas also calls it for a middle or right press: Windows does not
    // pair a click across another button, so neither may this counter.
    void forget() noexcept {
        last_count_ = 0;
        restart_    = true;
        shifted_    = false;
    }

private:
    // Within the double-click time and rectangle of the previous press. The
    // rectangle is centred on that press, hence the halved metrics.
    bool follows_quickly(std::uint32_t time_ms, int x_px, int y_px,
                         const PointerMetrics& m) const noexcept {
        // Unsigned subtraction: correct across GetMessageTime's wrap.
        return static_cast<std::uint32_t>(time_ms - last_time_ms_) <= m.dblclk_ms
            && std::abs(x_px - last_x_) <= m.dblclk_cx / 2
            && std::abs(y_px - last_y_) <= m.dblclk_cy / 2;
    }

    int           last_count_   = 0;
    std::uint32_t last_time_ms_ = 0;
    int           last_x_       = 0;
    int           last_y_       = 0;
    bool          restart_      = false;   // set by forget()
    bool          shifted_      = false;   // Windows' pairs run one press behind (see press)
};

// Pointer motion between two pan steps, in client pixels.
struct PanStep {
    int dx_px = 0;
    int dy_px = 0;
};

// What a button release asks PdfCanvas to do. By the time the caller acts, the
// gesture is already over -- see GestureState::release.
enum class ReleaseAction {
    None,              // no live gesture belongs to this button
    ClearSelection,    // a single click that never moved (Chars mode): spec §2, "the next click clears"
    CommitSelection,   // materialise quads + text into the view
    EndPan,            // #58
};

class GestureState {
public:
    Gesture          gesture() const noexcept { return gesture_; }
    core::SelectMode mode()    const noexcept { return mode_; }
    bool             moved()   const noexcept { return moved_; }

    // A left press that selects. Refused while any gesture is live.
    bool begin_select(core::SelectMode mode, int x_px, int y_px) noexcept {
        if (gesture_ != Gesture::None) return false;
        gesture_  = Gesture::Selecting;
        owner_    = MouseButton::Left;
        mode_     = mode;
        moved_    = false;
        origin_x_ = x_px;
        origin_y_ = y_px;
        return true;
    }

    // A press that pans (#58). Refused while any gesture is live -- the mirror of
    // the canvas rule that a middle press cancels a live selection drag.
    bool begin_pan(MouseButton button, int x_px, int y_px) noexcept {
        if (gesture_ != Gesture::None) return false;
        gesture_  = Gesture::Panning;
        owner_    = button;
        moved_    = false;
        origin_x_ = x_px;
        origin_y_ = y_px;
        last_x_   = x_px;
        last_y_   = y_px;
        return true;
    }

    // Pointer motion while a gesture is live. The threshold is sticky: once
    // crossed, returning to the press point is still a drag. GetSystemMetrics
    // documents SM_CXDRAG as "the number of pixels on either side of a
    // mouse-down point that the mouse pointer can move before a drag operation
    // begins", so crossing means exceeding it. (DragDetect's page instead calls
    // it the width of the drag rectangle; the two Win32 pages disagree, and this
    // follows the metric's own definition. The cost of the other reading is a
    // 2-pixel difference in how far a click may wander before it becomes a drag.)
    void move(int x_px, int y_px, const PointerMetrics& m) noexcept {
        if (gesture_ == Gesture::None || moved_) return;
        if (std::abs(x_px - origin_x_) > m.drag_cx
            || std::abs(y_px - origin_y_) > m.drag_cy) {
            moved_ = true;
        }
    }

    // A pan's pointer motion (#58): the displacement since the previous step,
    // or since the press for the first one. {0, 0} unless a pan is live.
    //
    // Incremental, not measured from the press: PdfCanvas applies each step to
    // the pan as it is NOW, so anything that re-clamps the pan mid-drag (a
    // render landing, a zoom) never makes the content jump to catch up with a
    // stale absolute offset, and dragging back after overshooting an edge moves
    // the content at once instead of through a dead zone. Integer steps sum to
    // the whole drag exactly.
    PanStep pan_step(int x_px, int y_px) noexcept {
        if (gesture_ != Gesture::Panning) return {};
        const PanStep s{ x_px - last_x_, y_px - last_y_ };
        last_x_ = x_px;
        last_y_ = y_px;
        return s;
    }

    // A button release: decides what it means AND ends the gesture, before
    // returning.
    //
    // ORDER IS LOAD-BEARING. ReleaseCapture delivers WM_CAPTURECHANGED
    // synchronously, inside the call, and PdfCanvas answers that with abort().
    // So the caller must call release() FIRST and ReleaseCapture() SECOND;
    // abort() then finds nothing live. The other order ends the drag before
    // release() can read it. Splitter.cpp clears its flag before ReleaseCapture
    // for the same reason.
    ReleaseAction release(MouseButton button) noexcept {
        if (gesture_ == Gesture::None || button != owner_) return ReleaseAction::None;
        const Gesture          ended = gesture_;
        const core::SelectMode mode  = mode_;
        const bool             moved = moved_;
        end();
        if (ended == Gesture::Panning) return ReleaseAction::EndPan;
        if (mode == core::SelectMode::Chars && !moved) return ReleaseAction::ClearSelection;
        return ReleaseAction::CommitSelection;
    }

    // Capture lost, a second button pressed, the page or layout changed, or the
    // view torn down. Ends WHATEVER is live without committing -- Panning included, or a
    // pan interrupted by another window would leave the canvas refusing every
    // later press. Returns what was live (None if nothing was).
    Gesture abort() noexcept {
        const Gesture was = gesture_;
        end();
        return was;
    }

private:
    // gesture() is the single source of truth for whether a gesture is live
    // (#68): a finished gesture leaves mode() and moved() at their idle values,
    // so neither can answer for a drag that is already over.
    void end() noexcept {
        gesture_ = Gesture::None;
        mode_    = core::SelectMode::Chars;
        moved_   = false;
    }

    Gesture          gesture_  = Gesture::None;
    MouseButton      owner_    = MouseButton::Left;
    core::SelectMode mode_     = core::SelectMode::Chars;
    bool             moved_    = false;
    int              origin_x_ = 0;
    int              origin_y_ = 0;
    int              last_x_   = 0;   // previous pan step (#58)
    int              last_y_   = 0;
};

// The canvas cursor (spec §4.5, §5). The move shape is the hand tool's; the
// system has no grab hand (IDC_HAND is the hyperlink pointer).
enum class CanvasCursor { Arrow, IBeam, Move };

// In precedence order:
//   - a live SELECTION keeps the I-beam, Space or not -- Space is read at the
//     press, so going down mid-drag cannot turn the drag into a pan;
//   - a live pan, or Space held: the move shape when something can pan, and
//     the arrow when nothing can. Anywhere in the client, margins included,
//     because a Space press pans from there too. Not the I-beam when nothing
//     can pan: a Space press would pan (a no-op), not select;
//   - otherwise the I-beam over a single-page page and the arrow elsewhere.
inline CanvasCursor canvas_cursor(Gesture live, bool space_held, bool can_pan,
                                  bool over_page) noexcept {
    if (live == Gesture::Selecting) return CanvasCursor::IBeam;
    if (live == Gesture::Panning || space_held) {
        return can_pan ? CanvasCursor::Move : CanvasCursor::Arrow;
    }
    return over_page ? CanvasCursor::IBeam : CanvasCursor::Arrow;
}

// A canvas position in DIPs -> a point on the page in PDF points, clamped to the
// page. `page` is where on_paint drew the bitmap: its top-left corner is the
// page's (0, 0) (see SelPoint) and it spans page.w x page.h DIPs.
inline core::SelPoint canvas_dip_to_page_point(float x_dip, float y_dip,
                                               const Placement& page,
                                               float zoom_pct) noexcept {
    const float w_pt = dip_to_pdf_point(page.w, zoom_pct);
    const float h_pt = dip_to_pdf_point(page.h, zoom_pct);
    core::SelPoint p;
    p.x = std::clamp(dip_to_pdf_point(x_dip - page.x, zoom_pct), 0.0f, w_pt);
    p.y = std::clamp(dip_to_pdf_point(y_dip - page.y, zoom_pct), 0.0f, h_pt);
    return p;
}

}  // namespace litepdf::ui
