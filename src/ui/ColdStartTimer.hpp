// LitePDF — ui::ColdStartTimer: 5-checkpoint cold-start instrumentation (D9).
//
// Records QueryPerformanceCounter snapshots at:
//   T0 wWinMain entry, T1 ShowWindow returned,
//   T2 WM_USER_OPEN_OK, T3 WM_USER_RENDER_DONE,
//   T4 first real-bitmap EndDraw.
//
// On T4 (idempotent), emits a single OutputDebugStringW line and
// optionally mirrors to stderr when --log-timings was passed.
#pragma once
#include <cstdint>

namespace litepdf::ui {

class ColdStartTimer {
public:
    static void set_t0();              // call on wWinMain entry
    static void mark(int which);       // 1..4
    static void emit_if_complete(bool mirror_to_stderr);
    // Returns elapsed ms from T0 to Tn; 0 if not yet marked.
    static int64_t elapsed_ms(int n);
};

}  // namespace litepdf::ui
