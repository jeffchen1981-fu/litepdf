#pragma once

// bench_iteration — testable extraction of litepdf-cli's `--benchmark`
// per-iteration render.
//
// Renders page 0 once on a FRESH single-worker RenderEngine (no PageCache) and
// records open / engine-init / render timings into a BenchIteration. Pulled out
// of main() so a unit test can drive the bounded-wait + engine-teardown
// sequence directly (main() itself is not unit-testable), and to make the
// teardown-safety guarantee explicit and tested. Mirrors render_to_ppm.{hpp,cpp}
// for the `--render` path. See tests/unit/test_cli_bench_teardown.cpp.

#include <chrono>

namespace litepdf::core { class Document; }

namespace litepdf::cli {

// Per-iteration benchmark metrics, all milliseconds. open_render_ms is the
// gated total (open + engine_init + render): gating the sum makes the boundary
// between engine_init and render irrelevant to the regression gate.
//
// engine_init_ms is NOT zero: the RenderEngine constructor opens the per-worker
// fz_document, so a regression in worker-side open lands here, not in render_ms.
struct BenchIteration {
    double open_ms = 0.0;
    double engine_init_ms = 0.0;
    double render_ms = 0.0;
    double open_render_ms = 0.0;
};

// Open `path`, render page 0 once on a fresh single-worker RenderEngine, and
// fill `out`. Blocks up to `timeout` for the render to complete.
//
// Returns true on success (out fully populated). Returns false — and prints a
// one-line diagnostic to stderr — if open fails, the render times out, or the
// render produces no pixmap. Never a silent zero metric.
//
// Teardown-safe by construction: the wait/result state is heap-allocated and
// co-owned (std::shared_ptr) by the worker callback, so a render that only
// finishes inside ~RenderEngine() (the timeout path drains the still-in-flight
// job) writes into live state regardless of stack-destruction order. The
// original inline benchmark code instead captured stack locals by reference and
// leaned on declaring the sync primitives before the engine; this construction
// removes that load-bearing ordering requirement entirely.
bool run_one_iteration(const char* path, BenchIteration& out,
                       std::chrono::milliseconds timeout);

} // namespace litepdf::cli
