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

    Tab();   // out-of-line so DocumentView can stay forward-declared here.
    ~Tab();  // out-of-line so DocumentView can stay forward-declared here.

    Tab(const Tab&)            = delete;
    Tab& operator=(const Tab&) = delete;
};

class TabList {
public:
    TabList()  = default;
    ~TabList() = default;

    TabList(const TabList&)            = delete;
    TabList& operator=(const TabList&) = delete;

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

}  // namespace litepdf::core
