#include "core/detail/PriorityGate.hpp"

#include <system_error>

namespace litepdf::core::detail {

namespace {

constexpr std::size_t idx(GateClass c) noexcept { return static_cast<std::size_t>(c); }

std::uint64_t waiting_in(const GateState& s, GateClass c) noexcept {
    return s.next_ticket[idx(c)] - s.serving[idx(c)];
}

}  // namespace

bool admissible(const GateState& s, GateClass cls, std::uint64_t ticket) noexcept {
    if (s.held) return false;
    // The oldest waiter of its class.
    if (ticket != s.serving[idx(cls)]) return false;
    // Foreground goes first, always.
    if (cls != GateClass::Foreground && waiting_in(s, GateClass::Foreground) != 0) return false;
    // A Scan waits for a waiting Render, unless kScanAfterRenders Render holds
    // have run since the last Scan hold.
    if (cls == GateClass::Scan && waiting_in(s, GateClass::Render) != 0 &&
        s.renders_since_scan < kScanAfterRenders) {
        return false;
    }
    // ...and then a Render waits for that Scan.
    if (cls == GateClass::Render && waiting_in(s, GateClass::Scan) != 0 &&
        s.renders_since_scan >= kScanAfterRenders) {
        return false;
    }
    return true;
}

void PriorityGate::acquire(GateClass cls) {
    std::unique_lock<std::mutex> lk(m_);
    // Before the ticket: see the header.
    if (st_.held && owner_ == std::this_thread::get_id()) {
        throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
    }
    const std::uint64_t my = st_.next_ticket[idx(cls)]++;
    cv_.wait(lk, [&] { return admissible(st_, cls, my); });
    ++st_.serving[idx(cls)];
    st_.held = true;
    owner_ = std::this_thread::get_id();
    if (cls == GateClass::Render) {
        ++st_.renders_since_scan;
    } else if (cls == GateClass::Scan) {
        st_.renders_since_scan = 0;
    }
}

void PriorityGate::release() noexcept {
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.held = false;
        owner_ = std::thread::id{};
    }
    // Every waiter re-checks: which one is admitted depends on all classes.
    cv_.notify_all();
}

std::size_t PriorityGate::waiting(GateClass cls) const {
    std::lock_guard<std::mutex> lk(m_);
    return static_cast<std::size_t>(waiting_in(st_, cls));
}

}  // namespace litepdf::core::detail
