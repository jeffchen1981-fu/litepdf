#pragma once

#include "core/Document.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

// OutlinePane wraps a Win32 TreeView (SysTreeView32) that displays
// PDF bookmark/outline entries. Hidden by default; MainWindow shows it
// when a document with outline entries is opened.
//
// Communication: on TVN_SELCHANGED the pane calls the on_navigate
// callback with the target page index (0-based). If the entry has
// no target page (kNoPage), the callback is not fired.
class OutlinePane {
public:
    using NavigateCb = std::function<void(int page)>;

    OutlinePane(HINSTANCE hInstance, HWND parent);
    ~OutlinePane();

    OutlinePane(const OutlinePane&)            = delete;
    OutlinePane& operator=(const OutlinePane&) = delete;

    HWND hwnd() const;

    void set_on_navigate(NavigateCb cb);

    // Populate the tree from Document outline entries. Clears any
    // previous items. Handles nested items via OutlineEntry::depth.
    void populate(const std::vector<litepdf::core::Document::OutlineEntry>& entries);

    // Remove all tree items.
    void clear();

    void show();
    void hide();
    bool visible() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
