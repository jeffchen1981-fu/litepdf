#pragma once

// #55: the completion rules for the continuous-scroll strip (spec 5.3), as pure
// functions of the page's slot and the completion's identity. No Win32, no
// Direct2D, no MuPDF: the WM_USER_STRIP_DONE handler (PR 2) feeds them the slot,
// the payload identity and the outcome of the bitmap creation, and applies the
// result.
//
// The handler calls on_completion_arrived first, then asks classify_strip_completion
// what to do (rules 1-5, in order); CreateBitmap is followed by after_bitmap with
// the creation result, and the null actions by after_null.

#include <cstdint>

#include "ui/detail/StripSchedule.hpp"

namespace litepdf::ui {

// The WM_USER_STRIP_DONE payload's identity. It is present for null results too.
struct StripCompletionMeta {
    std::uint64_t epoch = 0;
    int page = 0;
    std::uint64_t seq = 0;
    bool cancelled = false;
};

enum class StripCompletionAction {
    Drop,                 // rules 1, 2: nothing to apply
    CreateBitmap,         // rule 3: create the D2D bitmap, then after_bitmap()
    CancelAndReschedule,  // rule 4: after_null(.., true), then a pass
    MarkFailed,           // rule 5: after_null(.., false)
};

// `slot` is the page's current slot, nullptr when it has none.
inline StripCompletionAction classify_strip_completion(const PageSlot* slot,
                                                       const StripCompletionMeta& m,
                                                       std::uint64_t view_epoch,
                                                       bool has_pixmap) noexcept {
    if (m.epoch != view_epoch) return StripCompletionAction::Drop;                      // 1
    if (slot == nullptr || slot->kind != PageStateKind::Pending || slot->seq != m.seq)  // 2
        return StripCompletionAction::Drop;
    if (has_pixmap) return StripCompletionAction::CreateBitmap;                         // 3
    return m.cancelled ? StripCompletionAction::CancelAndReschedule                     // 4
                       : StripCompletionAction::MarkFailed;                             // 5
}

// How the D2D bitmap creation for a pixmap went (rule 3).
enum class BitmapResult {
    Ok,
    RecreateOrNoTarget,  // no render target, or D2DERR_RECREATE_TARGET
    OtherFailure,        // for example a bitmap above the device's maximum size
};

// The slot after rule 3. `pending` is the slot the completion matched.
inline PageSlot after_bitmap(const PageSlot& pending, BitmapResult r) noexcept {
    PageSlot out;
    switch (r) {
    case BitmapResult::Ok:
        // Ready at the submitted parameters; if they differ from the view's (a
        // change the cancel missed) is_stale() makes the next pass re-request it.
        out.kind = PageStateKind::Ready;
        out.scale = pending.scale;
        out.invert = pending.invert;
        break;
    case BitmapResult::RecreateOrNoTarget: break;  // None; the caller discards the target
    case BitmapResult::OtherFailure:
        out.kind = PageStateKind::Failed;
        out.priority = pending.priority;
        break;
    }
    return out;
}

// The slot after rule 4 (cancelled) or rule 5 (not cancelled).
inline PageSlot after_null(const PageSlot& pending, bool cancelled) noexcept {
    if (cancelled) return cancel_transition(pending);
    PageSlot out;
    out.kind = PageStateKind::Failed;
    out.priority = pending.priority;
    return out;
}

}  // namespace litepdf::ui
