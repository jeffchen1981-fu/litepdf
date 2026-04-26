#pragma once

// core::TabList — pure-logic tab collection used by ui::TabManager.
// No Win32 types in this header so the class can be unit-tested without
// HWND creation. Manages ownership of Tab structs and the active index.
//
// Activation policy on remove (D4 from Phase 5 plan):
//   - Removing a non-active tab shifts indices; active stays on the same
//     Tab instance (its index may decrement).
//   - Removing the active non-last tab activates the right neighbor.
//   - Removing the active last tab activates the new last tab (left).
//   - Removing the final remaining tab leaves the list empty (active_index=-1).

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace litepdf::core {

class DocumentView;

struct Tab {
    std::unique_ptr<DocumentView> view;
    std::wstring                  label;
    std::filesystem::path         path;

    // UI-state snapshot saved on deactivation, restored on activation.
    // pan_x/pan_y are in DIPs, matching PdfCanvas::Impl pan state.
    float pan_x            = 0.0f;
    float pan_y            = 0.0f;
    bool  outline_visible  = false;
    // Phase 7 Task 8 / D11: per-tab thumbnail pane visibility. Default
    // hidden on new tabs (D12). The actual pane is lazily created by
    // DocumentView::ensure_thumb_pane on first F4 press, so this flag
    // can be true with view->thumb_pane() == nullptr only for a brief
    // window inside the F4 handler — we defensively read the live
    // pane->visible() snapshot when switching away.
    bool  thumb_visible    = false;

    // Out-of-line so DocumentView can stay forward-declared here: the
    // unique_ptr<DocumentView> member needs the full type to destroy.
    // The user-declared dtor also suppresses implicit move operations,
    // which is intentional: Tab is always held via unique_ptr and is
    // never moved by value.
    Tab();
    ~Tab();

    Tab(const Tab&)            = delete;
    Tab& operator=(const Tab&) = delete;
};

class TabList {
public:
    TabList()  = default;
    ~TabList() = default;

    TabList(const TabList&)            = delete;
    TabList& operator=(const TabList&) = delete;
    TabList(TabList&&)                 = default;
    TabList& operator=(TabList&&)      = default;

    std::size_t size()  const noexcept { return tabs_.size(); }
    bool        empty() const noexcept { return tabs_.empty(); }
    int         active_index() const noexcept { return active_; }

    Tab* active() noexcept;
    Tab* at(std::size_t i) noexcept;

    // Appends at the end; returns the new tab's index. Does NOT activate.
    std::size_t add(std::unique_ptr<Tab> t);

    // Removes tab at i. Returns the new active_index() (may be -1 if
    // the list is now empty). Out-of-range i is a no-op returning the
    // current active_index().
    int remove(std::size_t i);

    // Sets active; returns false (and does nothing) if i is out of range.
    bool set_active(std::size_t i) noexcept;

private:
    std::vector<std::unique_ptr<Tab>> tabs_;
    int                               active_ = -1;
};

// Pure-function helpers mapping Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+N
// accelerator presses to the resulting active index. Kept as free
// functions so MainWindow's WM_COMMAND dispatch and unit tests can
// share the exact same arithmetic. Return -1 means "no change": the
// caller should leave the current active tab alone.
inline int next_tab_index(int active, int count) noexcept {
    if (count < 2 || active < 0) return -1;
    return (active + 1) % count;
}
inline int prev_tab_index(int active, int count) noexcept {
    if (count < 2 || active < 0) return -1;
    return (active - 1 + count) % count;
}
// Ctrl+1..9 maps 1-indexed user shortcut to 0-indexed tab position.
// Returns -1 when the requested tab does not exist (e.g. Ctrl+5 with
// 3 tabs open), matching Chrome/Edge "silent no-op" behaviour.
inline int goto_tab_index(int one_indexed, int count) noexcept {
    const int target = one_indexed - 1;
    if (target < 0 || target >= count) return -1;
    return target;
}

}  // namespace litepdf::core
