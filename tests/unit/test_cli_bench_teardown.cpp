#include <catch2/catch_test_macros.hpp>

#include "cli/bench_iteration.hpp"

#include <chrono>

// Contract + teardown guard for litepdf-cli's `--benchmark` per-iteration
// render (litepdf::cli::run_one_iteration).
//
// The benchmark helper is the sibling of the `--render` path that PR #24
// hardened. It carried the same teardown hazard: a render still in flight when
// the 10 s wait times out is drained by ~RenderEngine() during stack unwind,
// firing the worker callback. The pre-hardening code captured stack locals by
// `[&]` and leaned on declaring the sync primitives before the engine so they
// outlived it. run_one_iteration now co-owns its wait/result state via a
// std::shared_ptr captured BY VALUE, so the drained callback writes into live
// heap state regardless of stack-destruction order.
//
// Unlike render_to_ppm, the benchmark helper has no surviving observable side
// channel (it writes timings into internal WaitState, then returns false on
// timeout and the caller ignores `out`). The raw use-after-free is also
// sanitizer-invisible (destruct-within-live-frame on still-addressable stack).
// So the teardown case below is necessarily a smoke test: it proves the
// dangerous drain path runs without crashing or hanging, and that the helper
// still works afterward. The real positive coverage is the success-path case,
// which is the first unit test of the benchmark iteration logic at all.
//
// One branch the sibling test locks is intentionally absent here: the no-pixmap
// (got_pixmap == false) path. run_one_iteration always renders page 0, so there
// is no out-of-range page to force a null pixmap — render_page_to_ppm takes a
// page parameter and tests rc == 2, this helper does not. That branch is
// structurally identical to the tested render_to_ppm path, so it is covered by
// inspection rather than a dedicated case (adding a page parameter purely for
// testability would be scope creep the benchmark never uses).

namespace {
constexpr const char* kFixture = "tests/fixtures/simple.pdf";
}

// Success path: a generous timeout lets the render complete before the wait, so
// run_one_iteration returns true with a fully populated, self-consistent
// BenchIteration. This is the load-bearing coverage of the open + engine-init +
// render + timing sequence the benchmark gate depends on.
TEST_CASE("run_one_iteration: success fills consistent metrics and returns true",
          "[cli][benchmark][success]") {
    litepdf::cli::BenchIteration it;
    const bool ok = litepdf::cli::run_one_iteration(
        kFixture, it, std::chrono::seconds(10));

    REQUIRE(ok);

    // Every metric is a real measured duration: non-negative, and the gated
    // total is exactly the sum of its parts (the contract run_benchmark relies
    // on when it pushes open_render_ms as the regression-gated number).
    REQUIRE(it.open_ms >= 0.0);
    REQUIRE(it.engine_init_ms >= 0.0);
    REQUIRE(it.render_ms >= 0.0);
    REQUIRE(it.open_render_ms ==
            it.open_ms + it.engine_init_ms + it.render_ms);
    // A real page render is not instantaneous; this guards against a future
    // change silently zeroing render_ms (e.g. reading render_end before the
    // callback wrote it).
    REQUIRE(it.render_ms > 0.0);
}

// Open-failure contract: a path that cannot open returns false (no silent zero
// metric). Deterministic, and locks the early-return branch.
TEST_CASE("run_one_iteration: unopenable path returns false",
          "[cli][benchmark][open-error]") {
    litepdf::cli::BenchIteration it;
    const bool ok = litepdf::cli::run_one_iteration(
        "tests/fixtures/does-not-exist-9f3c.pdf", it, std::chrono::seconds(10));

    REQUIRE_FALSE(ok);
}

// Teardown path: a 0 ms timeout almost always loses the race, so the wait times
// out and returns false while the render is still in flight — forcing
// ~RenderEngine() to drain the worker callback during teardown. With the
// shared_ptr construction that drain writes into live heap state, not freed
// stack. We can't observe the drained write directly (no surviving side
// channel), so the proof of safety is twofold: the loop runs the dangerous
// path repeatedly without crashing or hanging, and a normal iteration still
// succeeds afterward (the helper and the engine machinery survived the drains
// intact). A freakishly fast machine could win the 0 ms race (returning true);
// that is equally teardown-safe and acceptable.
TEST_CASE("run_one_iteration: 0 ms wait drives the timeout-teardown path safely",
          "[cli][benchmark][teardown]") {
    for (int i = 0; i < 5; ++i) {
        litepdf::cli::BenchIteration it;
        const bool ok = litepdf::cli::run_one_iteration(
            kFixture, it, std::chrono::milliseconds(0));
        // Either outcome is fine — the load-bearing fact is that we returned at
        // all (the in-flight callback was drained without touching freed
        // memory). The test would crash or hang, not assert-fail, on the UAF.
        (void)ok;
    }

    // Positive evidence the repeated teardown-drains left everything healthy: a
    // normal iteration still completes successfully.
    litepdf::cli::BenchIteration it;
    const bool ok = litepdf::cli::run_one_iteration(
        kFixture, it, std::chrono::seconds(10));
    REQUIRE(ok);
}
