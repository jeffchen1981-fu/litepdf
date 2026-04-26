#pragma once

// ui::ThumbnailPane - Win32 owner-draw SysListView32 widget that owns a
// core::ThumbnailModel for geometry. Hidden by default; MainWindow shows it
// (Phase 7 Task 8) when the user toggles the thumbnail pane (F4). T5 lands
// only the skeleton: page-number placeholder rectangles, hit-test, and DPI
// hot-switch handling. T6 plugs in the actual ThumbCache + ThumbnailRenderer
// via setters that this header forward-declares so the public include
// surface stays small.

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::core {
class ThumbCache;
class ThumbnailRenderer;
}  // namespace litepdf::core

namespace litepdf::ui {

class ThumbnailPane {
public:
    using NavigateCb = std::function<void(int page)>;

    ThumbnailPane(HINSTANCE hInstance, HWND parent);
    ~ThumbnailPane();

    ThumbnailPane(const ThumbnailPane&)            = delete;
    ThumbnailPane& operator=(const ThumbnailPane&) = delete;

    HWND hwnd() const;

    void set_on_navigate(NavigateCb cb);

    // Populate the pane with `n` pages. Resets scroll to top.
    void set_page_count(int n);

    // Highlight `page` as the current page. Invalidates the old + new tile
    // rects so the placeholder border repaints. T7 will additionally scroll
    // the new page into view if offscreen; T5 just invalidates.
    void set_current_page(int page);

    void show();
    void hide();
    bool visible() const;

    // Called by MainWindow on WM_DPICHANGED. Updates the model's DPI, asks
    // the cache + renderer to drop stale work (both nullptr-guarded so this
    // is safe to call before T6 wires those in), and invalidates the pane.
    void on_dpi_changed(unsigned new_dpi);

    // Wire in the per-tab cache + renderer (owned by DocumentView; D6/D7).
    // The pointers are nullable: callers may set them in any order, may
    // null them out before tearing down, and may call paint-driving setters
    // (set_page_count, set_current_page, on_dpi_changed) before either
    // pointer is set — the pane falls back to placeholder painting in that
    // case. Both must be set on the UI thread; the pane records the current
    // value lock-free for use by WM_DRAWITEM.
    //
    // The pane MUST be owned by the same object that owns the cache and
    // the renderer (DocumentView in T8) so the destruction order can be
    // controlled: cache + renderer outlive the pane, the pane's dtor
    // cancels in-flight renders before they can post WM_USER_THUMB_READY
    // back to a destroyed HWND.
    void set_renderer(litepdf::core::ThumbnailRenderer* renderer);
    void set_cache(litepdf::core::ThumbCache* cache);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // The Win32 subclass procs (defined in the .cpp at namespace scope so
    // SetWindowSubclass can take their addresses) need to reach impl_ to
    // dispatch on hwndItem and forward navigate clicks. Friend-declared
    // here mirrors TabManager's tab_subclass_proc pattern.
    friend LRESULT CALLBACK thumb_list_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                                     UINT_PTR, DWORD_PTR);
    friend LRESULT CALLBACK thumb_parent_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                                       UINT_PTR, DWORD_PTR);
};

}  // namespace litepdf::ui
