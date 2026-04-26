#include "core/ThumbnailModel.hpp"

#include <algorithm>

namespace litepdf::core {

ThumbnailModel::ThumbnailModel()  = default;
ThumbnailModel::~ThumbnailModel() = default;

namespace {
inline int dip_to_px(int dip, unsigned dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}
}  // namespace

void ThumbnailModel::set_page_count(int n)   { page_count_ = std::max(0, n); }
void ThumbnailModel::set_dpi(unsigned dpi)   { dpi_ = (dpi > 0 ? dpi : 96); }
void ThumbnailModel::set_tile_dip(DipSize d) { tile_dip_ = d; }
void ThumbnailModel::set_gap_dip(int g)      { gap_dip_ = std::max(0, g); }
void ThumbnailModel::set_viewport_h_px(int h){ viewport_h_ = std::max(0, h); }

void ThumbnailModel::set_scroll_y_px(int y) {
    const int max_y = std::max(0, total_height_px() - viewport_h_);
    scroll_y_ = std::clamp(y, 0, max_y);
}

std::pair<int,int> ThumbnailModel::set_current_page(int p) {
    p = std::clamp(p, 0, std::max(0, page_count_ - 1));
    if (p == current_page_) return {-1, -1};
    const int old = current_page_;
    current_page_ = p;
    return {old, p};
}

int ThumbnailModel::tile_h_px() const noexcept { return dip_to_px(tile_dip_.h, dpi_); }
int ThumbnailModel::tile_w_px() const noexcept { return dip_to_px(tile_dip_.w, dpi_); }

int ThumbnailModel::total_height_px() const noexcept {
    if (page_count_ == 0) return 0;
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    // n tiles consume n*pitch - gap (no trailing gap after last tile).
    return page_count_ * pitch - dip_to_px(gap_dip_, dpi_);
}

ThumbnailModel::Range ThumbnailModel::visible_range() const noexcept {
    if (page_count_ == 0 || viewport_h_ == 0) return {0, -1};
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    if (pitch <= 0) return {0, -1};
    const int first = std::max(0, scroll_y_ / pitch);
    // Last page whose top is < scroll_y_ + viewport_h_.
    const int last  = std::min(page_count_ - 1,
                               (scroll_y_ + viewport_h_ - 1) / pitch);
    return {first, last};
}

ThumbnailModel::Range ThumbnailModel::visible_range_with_buffer() const noexcept {
    const Range r = visible_range();
    if (r.last < r.first) return r;
    return {std::max(0, r.first - 1),
            std::min(page_count_ - 1, r.last + 1)};
}

std::optional<int> ThumbnailModel::page_at_y(int y_px) const noexcept {
    if (page_count_ == 0) return std::nullopt;
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    if (pitch <= 0) return std::nullopt;
    const int abs_y = scroll_y_ + y_px;
    if (abs_y < 0) return std::nullopt;
    const int page = abs_y / pitch;
    if (page >= page_count_) return std::nullopt;
    const int into_tile = abs_y % pitch;
    if (into_tile >= tile_h_px()) return std::nullopt;  // gap region
    return page;
}

RECT ThumbnailModel::tile_rect(int page) const noexcept {
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    const int top   = page * pitch - scroll_y_;
    return {0, top, tile_w_px(), top + tile_h_px()};
}

void ThumbnailModel::scroll_to_make_visible(int page) noexcept {
    if (page < 0 || page >= page_count_) return;
    const Range r = visible_range();
    if (page >= r.first && page <= r.last) return;  // already visible
    const int pitch = tile_h_px() + dip_to_px(gap_dip_, dpi_);
    set_scroll_y_px(page * pitch);  // align top of page to viewport top
}

}  // namespace litepdf::core
