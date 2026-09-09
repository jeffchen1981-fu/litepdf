#pragma once

// PR-A2: where a page should land when its pixmap arrives.
//
// Before this PR the completion handler zeroed the pan unconditionally
// (PdfCanvas.cpp, WM_USER_RENDER_DONE). That is right for a page turn and
// wrong for everything else: pan to the bottom of a page and press Zoom In and
// the view snaps back to the top; switch tabs and MainWindow's restored
// per-tab pan (MainWindow.cpp:602) is destroyed by the very next completion.
//
// An anchor names the intent instead:
//
//   Top     pan_y = 0                     PgDn / Home / End / outline click /
//                                         thumbnail click / wheel flip forward
//   Bottom  pan_y = viewport - content    wheel flip backward
//   Hit     centre the quad, 24 DIP margin  search navigation
//   None    keep the current pan, re-clamp  same-page re-render (resize, DPI,
//                                           zoom, pane toggle, invert, tab
//                                           switch)
//
// Both pan formulas depend on PR-A1 having moved the origin: under the old
// centred origin pan_y = 0 meant "centred", not "top".
//
// LIFETIME. (epoch, page, slot) does not identify a request -- a same-page zoom
// or resize produces a second P0 with an identical triple -- so the anchor
// records the SEQ of the submission batch that carried it and is applied only
// by a completion whose seq matches.
//
// TAKING IS NOT RETIRING. A spread submits two renders under ONE seq and both
// must apply the anchor: Bottom measures against the UNION of the two slots,
// which is not final until the second half lands, and either half can arrive
// first (two workers, and an L1 cache hit returns before any MuPDF work). So
// take() does not mutate; the canvas calls mark_applied() when it actually
// applied one; and the anchor is retired by the NEXT stamp(). That yields:
//   - a failed render (null completion) never marks applied, so the retry's
//     stamp carries the intent forward -- the recovery path;
//   - a same-page re-render after a completed navigation finds the anchor
//     retired, so it keeps the user's pan instead of snapping to the top;
//   - set_view clears outright (new view, new epoch, different document);
//   - epoch-mismatch drops and null completions leave it alone, which is safe
//     precisely because of the seq match: a stale completion can never take an
//     anchor that belongs to a different batch.

#include <cstdint>
#include <utility>

#include "core/SearchSession.hpp"

namespace litepdf::ui {

struct PageAnchor {
    enum class Kind { None, Top, Bottom, Hit };

    Kind kind = Kind::None;
    // Meaningful only when kind == Kind::Hit. Carried by value so the anchor
    // outlives the SearchSession::next()/prev() result that produced it.
    litepdf::core::SearchSession::Hit target{};

    static PageAnchor none() noexcept { return PageAnchor{}; }

    static PageAnchor top() noexcept {
        PageAnchor a;
        a.kind = Kind::Top;
        return a;
    }

    static PageAnchor bottom() noexcept {
        PageAnchor a;
        a.kind = Kind::Bottom;
        return a;
    }

    static PageAnchor hit(const litepdf::core::SearchSession::Hit& h) {
        PageAnchor a;
        a.kind   = Kind::Hit;
        a.target = h;
        return a;
    }
};

// The canvas's single pending-anchor slot. Nothing outside PdfCanvas owns one.
class AnchorSlot {
public:
    // Record an intent. Replaces whatever was pending -- the newer navigation
    // is the one the user asked for -- and clears the applied mark, so the
    // fresh intent is not retired by its own stamp(). The seq is not known yet;
    // stamp() binds it when the submission batch is issued.
    void install(PageAnchor a) {
        anchor_   = std::move(a);
        applied_  = false;
    }

    // Open a submission batch. Called once per batch from
    // PdfCanvas::next_render_seq(). An anchor that has ALREADY been applied is
    // retired here: its navigation is done, and the batch now opening is a
    // same-page re-render that must keep the user's pan. An anchor that was
    // never applied is carried forward to the new seq instead -- that is what
    // makes a failed or cancelled render recover on the retry.
    void stamp(std::uint64_t seq) noexcept {
        if (applied_) {
            anchor_  = PageAnchor::none();
            applied_ = false;
        }
        seq_ = seq;
    }

    // The anchor for `completion_seq`, or Kind::None. Does NOT mutate: both
    // halves of a spread carry one seq and both must apply it (see the LIFETIME
    // note above). An out-of-order completion gets None and leaves the slot
    // untouched, so it can neither apply nor destroy a live intent.
    PageAnchor take(std::uint64_t completion_seq) const {
        if (anchor_.kind == PageAnchor::Kind::None) return PageAnchor::none();
        if (completion_seq != seq_)                 return PageAnchor::none();
        return anchor_;
    }

    // The canvas calls this only after an application actually happened -- not
    // merely after take() returned something. A completion that took an anchor
    // but could not apply it (no bitmap yet) must NOT mark it, or the intent
    // would be retired without ever taking effect.
    void mark_applied() noexcept { applied_ = true; }

    void clear() {
        anchor_  = PageAnchor::none();
        applied_ = false;
    }

    bool pending() const noexcept {
        return anchor_.kind != PageAnchor::Kind::None;
    }
    bool applied() const noexcept { return applied_; }

private:
    PageAnchor    anchor_{};
    std::uint64_t seq_     = 0;
    bool          applied_ = false;
};

}  // namespace litepdf::ui
