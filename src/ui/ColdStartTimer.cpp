// LitePDF — ui::ColdStartTimer implementation.
#include "ui/ColdStartTimer.hpp"

#include <windows.h>
#include <cstdio>
#include <cwchar>

namespace {
LARGE_INTEGER g_freq = { 0 };
LARGE_INTEGER g_t[5] = { { 0 } };
bool g_marked[5] = { false };
bool g_emitted = false;

int64_t ms_between(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    if (g_freq.QuadPart == 0) return 0;
    return static_cast<int64_t>(
        (b.QuadPart - a.QuadPart) * 1000 / g_freq.QuadPart);
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

int64_t ColdStartTimer::elapsed_ms(int n) {
    if (n < 0 || n > 4 || !g_marked[0] || !g_marked[n]) return 0;
    return ms_between(g_t[0], g_t[n]);
}

void ColdStartTimer::emit_if_complete(bool mirror_to_stderr) {
    if (g_emitted) return;
    if (!(g_marked[1] && g_marked[2] && g_marked[3] && g_marked[4])) return;
    g_emitted = true;

    wchar_t line[256];
    int64_t t1 = ms_between(g_t[0], g_t[1]);
    int64_t t2 = ms_between(g_t[0], g_t[2]);
    int64_t t3 = ms_between(g_t[0], g_t[3]);
    int64_t t4 = ms_between(g_t[0], g_t[4]);
    std::swprintf(line, 256,
        L"LitePDF cold-start: T0->T1=%lldms T0->T2=%lldms T0->T3=%lldms T0->T4=%lldms\n",
        static_cast<long long>(t1), static_cast<long long>(t2),
        static_cast<long long>(t3), static_cast<long long>(t4));
    OutputDebugStringW(line);
    if (mirror_to_stderr) {
        std::fwprintf(stderr, L"%s", line);
        std::fflush(stderr);
    }
}

}  // namespace litepdf::ui
