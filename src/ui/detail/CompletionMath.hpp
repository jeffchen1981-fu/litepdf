#pragma once

// PR-A2: the render-completion accept predicate, as pure logic.
//
// A completion message carries {epoch, page, slot, seq}. `seq` decides which
// PENDING ANCHOR a completion may consume (see ui/detail/PageAnchor.hpp); this
// file decides the prior question -- whether the pixmap should be painted at
// all.
//
// Four things can make a completion unwanted:
//   epoch  the view was swapped (tab switch) after the render was submitted;
//   seq    a NEWER submission has been issued since this one went out;
//   page   the user paged away after the render was submitted;
//   slot   a RIGHT-slot pixmap arrived while the layout is single-page.
//
// The seq test is what covers the duplicate-P0 case the engine cannot:
// cancel_stale_renders(0) flags only priority > 0, so a zoom / resize / DPI
// change / invert toggle can leave two P0s for the same page in flight with
// identical (epoch, page, slot). Whichever finishes second would otherwise win,
// which on a zoom means the canvas keeps the superseded scale.
//
// SLOT IS LOAD-BEARING. In spread mode the right-slot render is submitted for
// left+1 while current_page() has already been snapped to left, and both
// messages share one WndProc case. Comparing every completion against
// current_page() would reject EVERY right-slot pixmap, leaving the right half
// of each spread as the grey placeholder permanently -- each resubmit takes the
// same path, so it would never recover.

#include <cstdint>

#include "ui/PdfCanvasLayout.hpp"

namespace litepdf::ui {

// Which half of the two-page spread a completion belongs to. A single-page
// render is Left.
enum class Slot { Left, Right };

// True iff the pixmap described by (meta_epoch, meta_seq, meta_page, meta_slot)
// is still wanted by a canvas at (cur_epoch, newest_submitted_seq, cur_page,
// dual, page_count).
//
// `newest_submitted_seq` is the canvas's submission counter -- the seq of the
// most recent batch SENT, not of the last one accepted. Comparing against the
// last ACCEPTED seq is not enough: if the older of two racing P0s happens to
// arrive first, nothing has been accepted yet, so it passes; and if the newer
// one then fails or is cancelled, the superseded pixmap stays on screen. The
// submitted counter knows the newer render exists before either lands.
//
// The test is `>=`, not `>`, because both halves of a spread carry one seq and
// that seq IS the newest submitted: with `>` every completion of the current
// batch would lose to the counter that issued it and nothing would ever paint.
//
// `cur_page` is re-snapped to the pair's LEFT page in dual mode. Every
// submission path already snaps before submitting (MainWindow::kick_render,
// PdfCanvas::resubmit_current_page, PdfCanvas::apply_viewport,
// PdfCanvas::navigate_to_page), so the snap here is defence in depth -- it makes
// the predicate correct regardless of the order a future caller does things in.
inline bool accept_completion(std::uint64_t meta_epoch, std::uint64_t cur_epoch,
                              std::uint64_t meta_seq,
                              std::uint64_t newest_submitted_seq,
                              int meta_page, Slot meta_slot,
                              int cur_page, bool dual, int page_count) noexcept {
    if (meta_epoch != cur_epoch)          return false;
    if (meta_seq < newest_submitted_seq)  return false;
    if (page_count <= 0)                  return false;

    if (!dual) {
        // No right slot exists, so a right-slot pixmap has nowhere to land.
        if (meta_slot == Slot::Right) return false;
        return meta_page == cur_page;
    }

    const int left = dual_page_compute_left(cur_page, page_count);
    if (meta_slot == Slot::Left) return meta_page == left;

    const int right = dual_page_compute_right(left, page_count);
    if (right < 0) return false;   // cover page, or odd tail: no right half
    return meta_page == right;
}

}  // namespace litepdf::ui
