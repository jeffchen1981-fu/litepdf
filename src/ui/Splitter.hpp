#pragma once
// Refactored: Splitter now wraps SplitterCore (Phase 7 prereq).
// ui::Splitter — horizontal 4-DIP drag bar for resizing the bottom panel.
// WM_SETCURSOR → IDC_SIZENS; WM_LBUTTONDOWN captures mouse; WM_MOUSEMOVE
// during capture posts height changes to parent via the OnDrag callback.

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class Splitter {
public:
    // Called during drag with the new desired height-from-bottom (in pixels).
    using OnDrag = std::function<void(int new_height_px)>;

    Splitter(HINSTANCE, HWND parent);
    ~Splitter();

    Splitter(const Splitter&)            = delete;
    Splitter& operator=(const Splitter&) = delete;

    HWND hwnd() const;
    void set_on_drag(OnDrag cb);

    // Called during parent on_layout with the splitter's bounds
    // (4 DIP tall horizontal bar at y = client_height - panel_height - 4).
    void set_bounds(const RECT& bounds);

private:
    // Standard PIMPL — Impl is opaque, defined in Splitter.cpp. Post Phase 7
    // T4a refactor the WndProc retrieves a detail::SplitterCore* directly
    // from GWLP_USERDATA (not Splitter::Impl), so the historical reason for
    // Impl being publicly forward-declared no longer applies.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
