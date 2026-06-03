# Phase 11 — Benchmark Regression Gate (Design Spec)

> **Status:** Approved via brainstorming 2026-06-03. Implementation plan to
> follow via `superpowers:writing-plans`. Not yet three-lens reviewed.
>
> **Parent design:** This spec is the implementation-level authority for
> Phase 11. It inherits and refines
> [`docs/plans/2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md)
> §7 (Testing — "Cold-start time, batch benchmark, regression gate +10% fails
> CI") and §3 (the deferred MuPDF feature-flag pruning / binary-size gate), plus
> the Phase 11 row in
> [`docs/plans/2026-04-15-litepdf-roadmap.md`](../../plans/2026-04-15-litepdf-roadmap.md).
> Where this spec and those sections disagree, this spec wins — notably the
> regression metric is measured by the **headless `litepdf-cli` harness**, not
> the GUI cold-start line, and the gate is **self-calibrating against a freshly
> rebuilt base branch** rather than a committed baseline file.
>
> **Starting state:** `v0.0.12-phase10` shipped; `main` clean at `3576426`;
> `VERSION = 0.0.13-dev`. MuPDF pinned at 1.24.11.

## 1. Goal

Add a performance regression gate to CI so that a change which slows cold-start
(the document-open + first-page-render path) or grows the binary is caught on
the pull request that introduces it, instead of discovered after release. The
gate **protects** the current numbers (dev best-of-5 cold-start ≈ 233 ms against
a 1 s target; Release `litepdf.exe` ≈ 39 MB); it does not attempt to improve
them.

Roadmap exit criterion: *baseline mechanism in place, and a deliberate
regression is correctly blocked in CI.*

## 2. Resolved Decisions

Five decisions resolved during brainstorming (2026-06-03):

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | MuPDF submodule upgrade in scope? | **No — gate built against MuPDF 1.24.11** | Upgrading the engine while establishing a perf baseline makes the baseline a moving target. The upgrade (case-sensitive search via `fz_search_page2`, mid-page `fz_cookie` abort, adaptive `SearchDispatcher`) is deferred to a separate **Phase 11.5**, which the gate built here will then validate. See the `TODO(phase-11)` markers in `src/core/Document.hpp/.cpp` and `src/core/SearchSession.cpp` — they belong to 11.5, not this phase. |
| D2 | Which dimensions does the gate cover? | **Cold-start (CPU path) + binary size** | §7 names cold-start; §3 ties a binary-size regression gate to Phase 11. Idle-RAM gating is **deferred** — measuring working set on a CI runner is the noisiest signal and per-tab memory is already guarded by the Phase 2 1000-render stress test. MuPDF feature-flag pruning (the ~2 MB of mujs/gumbo/tesseract) is **deferred to 11.5** because it requires rebuilding MuPDF, which pairs with the upgrade. |
| D3 | Where is the regression number measured? | **Headless `litepdf-cli` harness (best-of-5), gating open+render; GUI `T0->T4` stays as an absolute ceiling** | Cold-start `T0->T4` mixes CPU-bound stages (MuPDF open `T1->T2`, rasterize `T2->T3`) with GPU-bound stages (Direct2D factory `T0->T1`, blit `T3->T4`). On CI there is no GPU, so Direct2D runs under WARP software rendering and the GPU stages are both slow and high-variance. Everyday code changes regress the CPU stages, which the headless harness measures cleanly and GPU-independently. The WARP stages rarely regress and cannot be measured stably — so they are kept out of the ±10% gate and covered only by the existing loose absolute ceiling. |
| D4 | Where does the baseline come from? | **Rebuild the base branch in the same CI job and compare on the same runner (self-calibrating)** | The dominant noise source is GitHub runner heterogeneity (a different CPU model per run can shift a pure-CPU number 10–20%). Measuring both `main` and the PR head on the *same* runner in the *same* job cancels that out, so ±10% is meaningful. Cost: the benchmark job builds MuPDF twice. Accepted — a gate that false-fails gets ignored, which defeats its purpose. No committed baseline file to drift or maintain. |
| D5 | Active cold-start tuning in scope? | **No — gate only** | With ~4× headroom (233 ms vs 1 s target) active optimization is low-ROI now, and the highest-leverage lever (feature-flag pruning) is already deferred to 11.5. "Tuning" becomes reactive: the gate tells you when something regressed. |

## 3. Components

### 3.1 Benchmark harness — extend `src/cli/main.cpp`

`litepdf-cli` already exists (console demo + `--render N`) and is built by CMake.
Add a benchmark mode rather than a new binary:

```
litepdf-cli <fixture> --benchmark [--iterations N] [--json]
```

Behaviour:

- Runs `N` iterations (default 5). Each iteration constructs a fresh
  `core::Document`, times `open()` (`T_open`), then drives `core::RenderEngine`
  to render **page 0** and times to the pixmap callback (`T_render`). Timing
  uses `std::chrono::steady_clock`.
- Reports the **minimum** (best-of-N) of each metric across iterations. Min is
  chosen deliberately: cold-start noise is right-skewed (occasional slowdowns),
  so the minimum best approximates the true work floor and is the most
  noise-resistant aggregate.
- `--json` emits a single machine-readable object to stdout:
  ```json
  {"fixture":"simple.pdf","iterations":5,"open_ms":7,"render_ms":31,"open_render_ms":38}
  ```
  Without `--json`, prints a human-readable summary (dev use).
- Exit non-zero if the fixture fails to open or render (no silent zero metric).

Note on OS file cache: iteration 1 is cold (disk), iterations 2..N are warm.
best-of-N → min → warm-cache CPU time. This is intentional: the gate defends
against **CPU-work** regressions (parsing, caching, rasterization), not disk
seek time, which is hardware/runner-dependent and out of the project's control.

### 3.2 `scripts/benchmark.ps1` (measure)

Runs the harness over the chosen fixtures, collects each timing JSON, measures
`build/Release/litepdf.exe` size in bytes, and writes a single combined result
file:

```json
{
  "exe_bytes": 40894464,
  "fixtures": {
    "simple.pdf": {"open_ms":7,"render_ms":31,"open_render_ms":38},
    "large.pdf":  {"open_ms":210,"render_ms":120,"open_render_ms":330}
  }
}
```

Parameters: `-Exe <path>`, `-Out <result.json>`, `-Iterations <N>`. Mirrors the
single-purpose convention of `check-version-sync.ps1` / `smoke-test.ps1`.
PowerShell 5.1-safe (no `?.` / `??` / ternary — see project memory
`reference_litepdf_powershell_51_only`).

### 3.3 `scripts/check-benchmark-regression.ps1` (compare + gate)

Inputs: `-Base <base.json>`, `-Pr <pr.json>`, thresholds from
`benchmarks/thresholds.json`. Computes per-metric percentage delta for the gated
timing metrics and a byte delta for `exe_bytes`, prints a comparison table, and
exits non-zero if any gated metric exceeds its threshold.

`benchmarks/thresholds.json`:
```json
{"time_regression_pct": 10, "size_regression_bytes": 262144}
```

The 256 KB size tolerance absorbs linker/compiler-version jitter; the gate is
anti-bloat regression protection, **not** a path to the design's aspirational
8 MB target (that gap is owned by the deferred 11.5 pruning work).

**Gated metrics** (everything else in the result file is informational):
- `simple.pdf` → `open_render_ms` (the cold-start CPU representative).
- `large.pdf` → `open_ms` (guards the xref/parse path; 500 pages amplify
  open cost so parsing regressions surface).
- `exe_bytes` (size).

### 3.4 Fixtures

Reuses existing `tests/fixtures/simple.pdf` and `tests/fixtures/large.pdf`
(declared in design §7). No new fixtures.

## 4. CI Integration — `.github/workflows/ci.yml`

Add a `benchmark` job that runs **only on `pull_request`** (on a push to `main`
there is no base to compare against — `main` *is* the baseline). It runs in
parallel with the existing `build-windows` job and does not block or modify it.

Sequence (single job, single runner):

1. Checkout PR head with recursive submodules; configure + build Release;
   `benchmark.ps1 -Out pr.json`.
2. `git worktree add` the PR's base branch tip (`origin/main`) into a sibling
   directory; recursive submodule update; configure + build Release;
   `benchmark.ps1 -Out base.json`. (The current tip of `main` is the
   comparison reference, not the literal `git merge-base` — what matters is
   measuring both exes on the same runner.)
3. `check-benchmark-regression.ps1 -Base base.json -Pr pr.json` → pass/fail.

Both builds happen on the same runner in the same job, so D4's self-calibration
holds. The binary-size delta falls out naturally from building both exes.

Cost: this job builds MuPDF twice (~2× the `build-windows` wall time). Accepted
per D4. If build time becomes painful later, CMake/MuPDF build caching is a
follow-up optimization, explicitly out of scope here.

## 5. Error Handling (fail-closed)

- Harness fails to open/render a fixture → non-zero exit; `benchmark.ps1`
  propagates the failure; the job fails. No fixture silently contributes a 0 ms
  metric.
- Base build or base measurement fails → **hard failure** with a clear message
  ("cannot establish comparison baseline"). The gate never degrades to a silent
  pass when it loses its reference.
- A single anomalous slow iteration → absorbed by best-of-N `min`. If *all* N
  iterations are slow (a real regression) the `min` is still high and the gate
  correctly fails.
- Malformed/missing JSON or a missing gated metric in either result file →
  treated as an error exit, never coerced to 0.

## 6. Testing Strategy

The roadmap exit criterion ("a regression is correctly blocked") is made
**deterministic** by testing the compare logic directly, decoupled from real
timing jitter:

- **Compare-logic unit test** (`scripts/` test, pwsh/Pester per repo
  convention): feed synthetic `base.json` / `pr.json` pairs and assert —
  - a +15 % timing delta on a gated metric → non-zero exit (blocked);
  - a +5 % delta → zero exit (allowed);
  - `exe_bytes` growth beyond the 256 KB tolerance → blocked;
  - growth within tolerance → allowed;
  - a missing gated metric or malformed JSON → error exit.
  This *is* the "regression correctly blocked" evidence — reproducible without
  depending on runner timing.
- **Harness smoke**: CI runs `litepdf-cli simple.pdf --benchmark --json` once
  and asserts the output parses as JSON with all expected fields present.
- **GUI absolute ceiling unchanged**: `scripts/smoke-test.ps1`'s
  `T0->T4 ≤ 1500 ms` check stays exactly as-is (liveness + ceiling). Only its
  comment is clarified to note it is the absolute ceiling, not the regression
  gate.

## 7. What This Phase Does NOT Do (deferred to 11.5 or later)

- MuPDF 1.24.11 → newer upgrade, and everything gated on it: case-sensitive
  search (`fz_search_page2` + `FZ_SEARCH_EXACT`), honoring `fz_cookie.abort`
  for mid-page search cancellation, DPI-/CPU-count-adaptive `SearchDispatcher`
  sizing. The `TODO(phase-11)` code comments refer to this 11.5 work.
- MuPDF feature-flag pruning (mujs / gumbo / tesseract / leptonica) and any move
  toward the 8 MB exe target. Only anti-regression size protection lands here.
- Idle-RAM regression gating.
- Active cold-start optimization.

## 8. Estimated Size

~200 LOC, matching the roadmap's Phase 11 estimate: harness timing extension in
`src/cli/main.cpp`, `scripts/benchmark.ps1`, `scripts/check-benchmark-regression.ps1`,
`benchmarks/thresholds.json`, the compare-logic test, and the `benchmark` CI job.

## 9. Build Sequence (for the implementation plan)

1. Harness: add `--benchmark` / `--iterations` / `--json` to `src/cli/main.cpp`;
   verify JSON locally against `simple.pdf` and `large.pdf`.
2. `scripts/benchmark.ps1`; run locally, confirm combined `result.json` shape.
3. `benchmarks/thresholds.json` + `scripts/check-benchmark-regression.ps1`.
4. Compare-logic unit test (synthetic JSON pairs); prove all five assertions.
5. `benchmark` CI job (PR-only, rebuild-base, parallel to `build-windows`).
6. Clarify the `smoke-test.ps1` ceiling comment.
7. Validate end-to-end: open a throwaway PR with a deliberate slowdown and
   confirm the gate blocks it (exit-criterion evidence).
