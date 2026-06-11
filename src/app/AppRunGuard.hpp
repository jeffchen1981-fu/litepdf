#pragma once
#include <filesystem>

namespace litepdf::app {

class AppRunGuard {
public:
    // Inspects `marker`: if present => previous exit was abnormal. Then
    // creates/refreshes the marker so THIS run is tracked. Construct ONLY in
    // the primary (message-loop-owning) instance, after the single-instance gate.
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
