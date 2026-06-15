#pragma once
#include <filesystem>

namespace litepdf::app {

class AppRunGuard {
public:
    // Inspects `marker`: if present => previous exit was abnormal. Then
    // creates/refreshes the marker so THIS run is tracked. Construct ONLY in
    // the primary (message-loop-owning) instance, after the single-instance gate.
    // May throw std::bad_alloc (path/stream allocation). A marker-write failure
    // is silently swallowed: this run is then untracked (next launch sees a clean
    // exit) — the safe-degrade direction (a missed restore, never a false one).
    explicit AppRunGuard(std::filesystem::path marker);

    bool previous_exit_was_abnormal() const noexcept { return prev_abnormal_; }

    // Mark THIS run as a clean exit (removes the marker). Idempotent — safe to
    // call from both WM_DESTROY and WM_ENDSESSION.
    void mark_clean_exit() noexcept;

private:
    std::filesystem::path marker_;
    bool prev_abnormal_ = false;
};

}  // namespace litepdf::app
