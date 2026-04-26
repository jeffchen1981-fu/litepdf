#pragma once

// ui::VerticalSplitter - vertical 4-DIP drag bar for resizing the
// left thumbnail/outline pane. Thin wrapper over SplitterCore; see
// Phase 7 D9 + Task 4a (the prerequisite refactor).

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class VerticalSplitter {
public:
    using OnDrag = std::function<void(int new_width_px)>;
    VerticalSplitter(HINSTANCE, HWND parent);
    ~VerticalSplitter();

    VerticalSplitter(const VerticalSplitter&)            = delete;
    VerticalSplitter& operator=(const VerticalSplitter&) = delete;

    HWND hwnd() const;
    void set_on_drag(OnDrag cb);
    void set_bounds(const RECT& bounds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
