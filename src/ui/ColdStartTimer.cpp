// LitePDF — ui::ColdStartTimer implementation (Phase 11.5 T2).
#include "ui/ColdStartTimer.hpp"
#include "ui/detail/ColdStartMath.hpp"

#include <windows.h>
#include <cstdio>
#include <cwchar>

namespace {
LARGE_INTEGER g_freq = { 0 };
LARGE_INTEGER g_t[5]   = { { 0 } };   // T0..T4
bool g_marked[5]       = { false };
LARGE_INTEGER g_sub[4] = { { 0 } };   // Sub::D2DFactory..ViewBuilt
bool g_sub_marked[4]   = { false };
bool g_emitted = false;

int64_t ms_between(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    return litepdf::ui::detail::span_ms(a.QuadPart, b.QuadPart, g_freq.QuadPart);
}
}  // namespace

namespace litepdf::ui {

void ColdStartTimer::set_t0() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t[0]);
    g_marked[0] = true;
}

void ColdStartTimer::mark(int which) {
    if (which < 1 || which > 4 || !g_marked[0]) return;
    if (g_marked[which]) return;  // idempotent — ignore later calls
    QueryPerformanceCounter(&g_t[which]);
    g_marked[which] = true;
}

void ColdStartTimer::mark_sub(Sub which) {
    int i = static_cast<int>(which);
    if (i < 0 || i > 3 || !g_marked[0]) return;
    if (g_sub_marked[i]) return;  // idempotent — first write wins
    QueryPerformanceCounter(&g_sub[i]);
    g_sub_marked[i] = true;
}

int64_t ColdStartTimer::elapsed_ms(int n) {
    if (n < 0 || n > 4 || !g_marked[0] || !g_marked[n]) return 0;
    return ms_between(g_t[0], g_t[n]);
}

void ColdStartTimer::emit_if_complete(bool mirror_to_stderr) {
    if (g_emitted) return;
    if (!(g_marked[1] && g_marked[2] && g_marked[3] && g_marked[4])) return;
    g_emitted = true;

    int64_t t1 = ms_between(g_t[0], g_t[1]);
    int64_t t2 = ms_between(g_t[0], g_t[2]);
    int64_t t3 = ms_between(g_t[0], g_t[3]);
    int64_t t4 = ms_between(g_t[0], g_t[4]);

    // Sub-marks (0 if unset). D2DFactory/OpenStart/OpenDone/ViewBuilt as T0->Tsub.
    auto sub = [](int i) -> long long {
        if (!g_sub_marked[i]) return 0;
        return static_cast<long long>(ms_between(g_t[0], g_sub[i]));
    };

    wchar_t line[320];
    std::swprintf(line, 320,
        L"LitePDF cold-start: T0->T1=%lldms T0->T2=%lldms T0->T3=%lldms T0->T4=%lldms"
        L" | d2d_ctor=%lld open_start=%lld open_done=%lld view_built=%lld\n",
        static_cast<long long>(t1), static_cast<long long>(t2),
        static_cast<long long>(t3), static_cast<long long>(t4),
        sub(0), sub(1), sub(2), sub(3));
    OutputDebugStringW(line);
    if (mirror_to_stderr) {
        std::fwprintf(stderr, L"%s", line);
        std::fflush(stderr);
    }
}

}  // namespace litepdf::ui
