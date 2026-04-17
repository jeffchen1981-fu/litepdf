#include "core/TabList.hpp"

#include "core/DocumentView.hpp"

#include <algorithm>

namespace litepdf::core {

// Defined out-of-line so DocumentView can be a forward-decl in the header.
// The unique_ptr<DocumentView> member needs the full type here to destroy.
Tab::Tab()  = default;
Tab::~Tab() = default;

Tab* TabList::active() noexcept {
    if (active_ < 0 || static_cast<std::size_t>(active_) >= tabs_.size()) {
        return nullptr;
    }
    return tabs_[static_cast<std::size_t>(active_)].get();
}

Tab* TabList::at(std::size_t i) noexcept {
    return (i < tabs_.size()) ? tabs_[i].get() : nullptr;
}

std::size_t TabList::add(std::unique_ptr<Tab> t) {
    tabs_.push_back(std::move(t));
    return tabs_.size() - 1;
}

int TabList::remove(std::size_t i) {
    if (i >= tabs_.size()) return active_;

    const bool removing_active = (static_cast<int>(i) == active_);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));

    if (tabs_.empty()) {
        active_ = -1;
    } else if (removing_active) {
        // Right-neighbor default; clamp to last tab if we removed the end.
        active_ = static_cast<int>(std::min(i, tabs_.size() - 1));
    } else if (static_cast<std::size_t>(active_) > i) {
        // Active tab was to the right of the removed one; shift down.
        --active_;
    }
    return active_;
}

bool TabList::set_active(std::size_t i) noexcept {
    if (i >= tabs_.size()) return false;
    active_ = static_cast<int>(i);
    return true;
}

}  // namespace litepdf::core
