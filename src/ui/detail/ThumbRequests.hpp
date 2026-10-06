#pragma once

// #117: the thumbnail pane's in-flight render bookkeeping, as pure logic.
//
// The pane asks for a thumb only from WM_DRAWITEM on a cache miss, and only
// once per page while a request for it is in flight. Each request gets an id
// that travels with its cancel notice (WM_USER_THUMB_CANCELED), because the
// pane also clears the set when it cancels on its own (document swap, hide,
// DPI change, renderer swap). A cancel notice from before that clear can then
// arrive after a repaint has asked for the same page again. Without the id it
// would drop the newer request and force another redraw: the page would render
// twice, and while the canvas keeps cancelling, the pair would keep renewing
// itself instead of settling back to one.
//
// UI-thread only.

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace litepdf::ui {

class ThumbRequests {
public:
    // Records a new request for `page` and returns its id, or nullopt when a
    // request for `page` is already in flight.
    std::optional<std::uint32_t> try_start(int page) {
        const std::uint32_t id = ++last_id_;
        if (!pending_.emplace(page, id).second) return std::nullopt;
        return id;
    }

    // The render finished, with or without a bitmap.
    void on_finished(int page) { pending_.erase(page); }

    // The request `id` for `page` was cancelled before it rendered. Returns true
    // when the row should be redrawn so WM_DRAWITEM asks for the page again:
    // when `id` is the pending request, or nothing is pending for the page.
    // Returns false for a stale notice while a newer request is in flight.
    bool on_canceled(int page, std::uint32_t id) {
        const auto it = pending_.find(page);
        if (it == pending_.end()) return true;
        if (it->second != id) return false;
        pending_.erase(it);
        return true;
    }

    // The pane cancelled every request itself; notices for them may follow.
    void clear() { pending_.clear(); }

private:
    std::unordered_map<int, std::uint32_t> pending_;
    std::uint32_t last_id_ = 0;
};

}  // namespace litepdf::ui
