#pragma once

// #52: pure gesture logic for PdfCanvas text selection (and, in #58, panning).
// No Win32, no Direct2D, no MuPDF -- headless-testable, the same pattern as
// ViewportMath.hpp and SplitterMath.hpp. PdfCanvas feeds it message coordinates
// and system metrics and acts on what it returns.

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
        if (is_double_click_message) {
            count = 2;
        } else if (last_count_ == 2
                   // Unsigned subtraction: correct across GetMessageTime's wrap.
                   && static_cast<std::uint32_t>(time_ms - last_time_ms_) <= m.dblclk_ms
                   && std::abs(x_px - last_x_) <= m.dblclk_cx / 2
                   && std::abs(y_px - last_y_) <= m.dblclk_cy / 2) {
            count = 3;
        }
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

private:
    int           last_count_   = 0;
    std::uint32_t last_time_ms_ = 0;
    int           last_x_       = 0;
    int           last_y_       = 0;
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
        const Gesture ended = gesture_;
        gesture_ = Gesture::None;
        if (ended == Gesture::Panning) return ReleaseAction::EndPan;
        if (mode_ == core::SelectMode::Chars && !moved_) return ReleaseAction::ClearSelection;
        return ReleaseAction::CommitSelection;
    }

    // Capture lost, a second button pressed, the page or layout changed, or the
    // view torn down. Ends WHATEVER is live without committing -- Panning included, or a
    // pan interrupted by another window would leave the canvas refusing every
    // later press. Returns what was live (None if nothing was).
    Gesture abort() noexcept {
        const Gesture was = gesture_;
        gesture_ = Gesture::None;
        return was;
    }

private:
    Gesture          gesture_  = Gesture::None;
    MouseButton      owner_    = MouseButton::Left;
    core::SelectMode mode_     = core::SelectMode::Chars;
    bool             moved_    = false;
    int              origin_x_ = 0;
    int              origin_y_ = 0;
};

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
