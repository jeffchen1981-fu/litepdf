#pragma once
// ui::detail::SplitterMath — pure clamp helpers for Splitter / VerticalSplitter
// drag math. Header-only / inline so unit tests (which link litepdf_core, NOT
// the UI translation units) can call them directly without dragging in WndProc.
//
// Phase 7 Task 4a (refactor): introduced alongside SplitterCore extraction.
// The horizontal Splitter now delegates its in-WM_MOUSEMOVE clamp to
// compute_drag_target_y; the vertical analog (compute_drag_target_x) is
// in place for Phase 7 Task 4b.
//
// Signature note (per plan §T4a §4a.1, lines 1119-1123): both helpers take
// FOUR args — (mouse_coord, parent_extent, min, max) — even though the math
// is asymmetric:
//   - Y (horizontal Splitter, panel anchored at BOTTOM) uses parent_h:
//       panel_height = clamp(parent_h - mouse_y, min, max)
//   - X (vertical Splitter,   pane  anchored at LEFT)   does NOT use parent_w:
//       pane_width   = clamp(mouse_x,             min, max)
// parent_w is kept in the X signature for symmetry with Y so callers and
// VerticalSplitter (Phase 7 T4b) can pass it uniformly. It is intentionally
// unused inside compute_drag_target_x.

#include <algorithm>

namespace litepdf::ui::detail {

// Horizontal Splitter (panel anchored at BOTTOM).
//
// Caller passes mouse Y in PARENT-CLIENT coords plus the parent's client
// height; the helper computes panel-from-bottom (parent_h - mouse_y) and
// clamps it to [min_h, max_h]. `max_h` is the caller's responsibility:
// typically std::max(min_h, parent_h - reserve) so the panel never crowds
// the main content past a sensible top reserve.
inline int compute_drag_target_y(int mouse_y, int parent_h, int min_h, int max_h) {
    return std::clamp(parent_h - mouse_y, min_h, max_h);
}

// Vertical Splitter (pane anchored at LEFT).
//
// The pane's width equals the mouse X in parent-client coords directly, so
// `parent_w` is unused; it is kept in the signature for symmetry with the
// Y helper. Callers (and VerticalSplitter in Phase 7 T4b) can therefore
// uniformly pass (mouse, parent_extent, min, max) regardless of orientation.
inline int compute_drag_target_x(int mouse_x, [[maybe_unused]] int parent_w,
                                 int min_w, int max_w) {
    return std::clamp(mouse_x, min_w, max_w);
}

}  // namespace litepdf::ui::detail
