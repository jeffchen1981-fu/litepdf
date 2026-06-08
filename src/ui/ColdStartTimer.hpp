// LitePDF — ui::ColdStartTimer: cold-start instrumentation (Phase 11.5 T2).
//
// Coarse checkpoints (unchanged contract):
//   T0 wWinMain entry, T1 ShowWindow returned,
//   T2 WM_USER_OPEN_OK, T3 WM_USER_RENDER_DONE,
//   T4 first real-bitmap EndDraw.
//
// Sub-marks split the otherwise-opaque T1->T2 window:
//   D2DFactory  — D2D1CreateFactory done (PdfCanvas ctor, in WM_CREATE, before T1;
//                 recorded here to size the factory cost inside T0->T1)
//   OpenStart   — open worker thread entered (before Document::open)
//   OpenDone    — Document::open returned
//   ViewBuilt   — DocumentView ctor returned, just before PostMessage(WM_USER_OPEN_OK)
// The (ViewBuilt vs T2) gap distinguishes "open work slow" (gap small) from
// "UI thread not pumping the queue" (gap large). It does NOT pre-attribute the
// UI-thread case to any specific cause.
//
// THREADING: the globals carry no atomics/fences. Cross-thread visibility is
// provided by the PostMessageW -> GetMessage happens-before edge: the open
// worker writes OpenStart/OpenDone/ViewBuilt BEFORE PostMessageW(WM_USER_OPEN_OK),
// and the UI thread reads them only after dequeuing that message. Single-writer-
// per-slot is necessary but NOT sufficient — the message handoff is the
// load-bearing guarantee. Do not read a sub-mark on a thread that did not cross
// that edge.
//
// On T4 (idempotent) emits a single OutputDebugStringW line and optionally
// mirrors to stderr when --log-timings was passed.
#pragma once
#include <cstdint>

namespace litepdf::ui {

class ColdStartTimer {
public:
    enum class Sub { D2DFactory = 0, OpenStart = 1, OpenDone = 2, ViewBuilt = 3 };

    static void set_t0();                 // call on wWinMain entry
    static void mark(int which);          // coarse 1..4
    static void mark_sub(Sub which);      // sub-marks (idempotent, first write wins)
    static void emit_if_complete(bool mirror_to_stderr);
    // Elapsed ms from T0 to coarse Tn; 0 if not yet marked.
    static int64_t elapsed_ms(int n);
};

}  // namespace litepdf::ui
