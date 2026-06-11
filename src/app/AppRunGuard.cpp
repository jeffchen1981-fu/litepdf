#include "app/AppRunGuard.hpp"
#include <fstream>
#include <system_error>

namespace litepdf::app {

AppRunGuard::AppRunGuard(std::filesystem::path marker) : marker_(std::move(marker)) {
    if (marker_.empty()) return;   // persistence disabled: no marker, never abnormal
    std::error_code ec;
    prev_abnormal_ = std::filesystem::exists(marker_, ec) && !ec;
    std::ofstream(marker_, std::ios::binary | std::ios::trunc) << "running";
}

void AppRunGuard::mark_clean_exit() noexcept {
    if (marker_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(marker_, ec);
}

}  // namespace litepdf::app
