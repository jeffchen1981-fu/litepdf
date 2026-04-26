#pragma once

// core::ThumbnailModel - pure-logic state for the left thumbnail pane.
// No Win32, no MuPDF, no threads. UI-thread-only callers; not thread-safe.

#include <cstddef>
#include <optional>
#include <utility>
#include <windows.h>  // RECT only - header is otherwise platform-free.

namespace litepdf::core {

class ThumbnailModel {
public:
    struct DipSize { int w; int h; };
    struct Range   { int first; int last; };  // inclusive; empty if last < first.

    ThumbnailModel();
    ~ThumbnailModel();

    void set_page_count(int n);
    void set_dpi(unsigned dpi);
    void set_tile_dip(DipSize d);
    void set_gap_dip(int g);
    void set_viewport_h_px(int h);
    void set_scroll_y_px(int y);

    // Returns {old_page, new_page} on change; {-1, -1} on no-op.
    std::pair<int,int> set_current_page(int p);
    int current_page() const noexcept { return current_page_; }

    int total_height_px() const noexcept;
    int tile_h_px() const noexcept;        // tile height in pixels (no gap)
    int tile_w_px() const noexcept;
    int scroll_y_px() const noexcept { return scroll_y_; }

    Range visible_range() const noexcept;
    Range visible_range_with_buffer() const noexcept;  // +/-1 page

    std::optional<int> page_at_y(int y_px) const noexcept;
    RECT  tile_rect(int page) const noexcept;

    // Adjusts scroll so `page` is in visible_range. No-op if already visible.
    void  scroll_to_make_visible(int page) noexcept;

    int   page_count() const noexcept { return page_count_; }

private:
    int      page_count_     = 0;
    unsigned dpi_            = 96;
    DipSize  tile_dip_       = {120, 160};
    int      gap_dip_        = 8;
    int      viewport_h_     = 0;
    int      scroll_y_       = 0;
    int      current_page_   = 0;
};

}  // namespace litepdf::core
