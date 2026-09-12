#pragma once

// ui::StatusBar -- PR-B: a msctls_statusbar32 docked at the bottom of
// MainWindow, hosting the page indicator and the go-to-page input.
//
// Single part, children at fixed offsets -- no SB_SETPARTS. MainWindow owns
// positioning: it asks for height_px() in on_layout, reserves that much at the
// bottom of the client area, and calls set_bounds().
//
// The control is sent WM_SIZE in exactly two places -- construction and
// update_dpi -- and only to make it compute its own themed natural height for
// the current font, which is then read back and handed to MainWindow. Every
// other position change goes through set_bounds(), so the control never
// self-docks behind the layout's back.

#include <functional>
#include <memory>

#include <windows.h>

namespace litepdf::ui {

class StatusBar {
public:
    // A page was committed in the box. The argument is a ZERO-BASED index --
    // the 1-based UI value is converted inside the bar (detail::parse_page_input).
    using OnGoto = std::function<void(int page_index)>;
    // The box is done with the keyboard: Esc, or any commit (valid or not).
    // The owner returns focus to the canvas.
    using OnFocusOut = std::function<void()>;
    // WM_MOUSEWHEEL arrived while the box had focus. WM_MOUSEWHEEL goes to the
    // FOCUSED window, so without forwarding, the wheel would be dead whenever
    // the reader had clicked into the page box.
    using OnWheel = std::function<void(WPARAM, LPARAM)>;

    StatusBar(HINSTANCE hInstance, HWND parent);
    ~StatusBar();

    StatusBar(const StatusBar&)            = delete;
    StatusBar& operator=(const StatusBar&) = delete;

    // Forward-declared PUBLICLY, not privately: the two subclass procedures in
    // StatusBar.cpp are free functions and receive an Impl* as their ref_data,
    // so they have to be able to name the type. Same reason FindBar.hpp
    // declares FindBar::Impl in its public section. Impl stays opaque here.
    struct Impl;

    HWND hwnd() const;

    // Natural height at the current DPI, in pixels. Measured from the control
    // itself at construction and re-measured by update_dpi().
    int height_px() const;

    // Position the bar in parent-client coordinates and re-lay the children.
    void set_bounds(const RECT& bounds);

    // Rebuild the font and re-measure after a DPI change.
    void update_dpi(UINT dpi);

    // Show a live document: `page_index` is 0-based, the box shows page_index+1.
    // Honours detail::should_overwrite_page_box, so it will not clobber digits
    // the reader is in the middle of typing.
    void set_page(int page_index, int page_count);

    // No document (last tab closed): clear both children and disable the box.
    void set_empty();

    // True while the page box holds the keyboard focus.
    //
    // MainWindow needs this because ESC is a BARE ACCELERATOR in this app
    // (`{ FVIRTKEY, VK_ESCAPE, IDM_FIND_CLOSE }`), and TranslateAcceleratorW
    // runs before the message is dispatched to any child -- so ESC never
    // reaches this control's WM_KEYDOWN and the status bar cannot claim it on
    // its own. The IDM_FIND_CLOSE arm in Task 5 asks this question instead.
    bool page_box_has_focus() const;

    void set_on_goto(OnGoto cb);
    void set_on_focus_out(OnFocusOut cb);
    void set_on_wheel(OnWheel cb);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
