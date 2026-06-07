// LitePDF — pure cold-start span arithmetic (Phase 11.5 Task 1).
// Header-only so unit tests link it without the process-global ColdStartTimer.
// QPC tick delta -> milliseconds; mirrors the body ColdStartTimer::ms_between had.
#pragma once
#include <cstdint>

namespace litepdf::ui::detail {

// Returns (b - a) ticks expressed in ms given QueryPerformanceFrequency `freq`.
// freq == 0 means QueryPerformanceFrequency has not run yet -> 0 (no divide).
inline int64_t span_ms(int64_t a, int64_t b, int64_t freq) {
    if (freq == 0) return 0;
    return (b - a) * 1000 / freq;
}

}  // namespace litepdf::ui::detail
