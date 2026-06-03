# Phase 11 — Benchmark Regression Gate (Design Spec)

> **Status:** Approved via brainstorming 2026-06-03; **hardened to v2 after a
> three-lens agent review (Opus + Sonnet + Codex) on 2026-06-03** that found the
> v1 methodology sound but flagged two blockers (a non-existent `large.pdf`
> fixture and a non-existent Pester test convention), a bootstrap chicken-and-egg,
> and several CI-mechanics gaps. v2 resolves all of them (see §10 Review Changelog).
> Implementation plan to follow via `superpowers:writing-plans`.
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
> the GUI cold-start line, and the gate is **self-calibrating against the PR's
> base commit rebuilt on the same runner**.
>
> **Starting state:** `v0.0.12-phase10` shipped; `main` clean at `3576426`;
> `VERSION = 0.0.13-dev`. MuPDF pinned at 1.24.11. Branch: `phase-11-benchmark-gate`.

## 1. Goal

Add a performance regression gate to CI so that a change which slows the
document-open + first-page-render CPU path, or grows the binary, is caught on the
pull request that introduces it. The gate **protects** the current numbers; it
does not improve them.

Two clarifications the v1→v2 review forced:

- **"Cold-start" here means the CPU-bound open+render work**, not disk latency
  or window/GPU latency. Disk-IO and Direct2D/WARP costs are explicitly out of
  the gated metric (see D3 and §3.1's warm-cache note).
- **The headless baseline is currently unknown** and is captured when the harness
  lands (§9). The frequently-cited "≈ 233 ms" is the **GUI `T0->T4`** figure
  (`smoke-test.ps1:88`), which is WARP-inclusive and a *different* number from the
  headless `open_render_ms` this gate uses. Do not seed `thresholds.json` from it.

Roadmap exit criterion: *baseline mechanism in place, and a deliberate regression
is correctly blocked in CI.*

## 2. Resolved Decisions

Eight decisions resolved during brainstorming (2026-06-03). D1–D5 from the
initial design pass; D6–D8 added after the three-lens review.

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | MuPDF submodule upgrade in scope? | **No — gate built against MuPDF 1.24.11** | Upgrading the engine while establishing a perf baseline makes the baseline a moving target. The upgrade (case-sensitive search via `fz_search_page2`, mid-page `fz_cookie` abort, adaptive `SearchDispatcher`) is deferred to **Phase 11.5**, which the gate built here will then validate. The `TODO(phase-11)` markers in `src/core/Document.hpp/.cpp` and `src/core/SearchSession.cpp` belong to 11.5. |
| D2 | Which dimensions does the gate cover? | **Cold-start (CPU path) + binary size** | §7 names cold-start; §3 ties a binary-size regression gate to Phase 11. Idle-RAM gating is **deferred** (noisiest CI signal; per-tab memory already guarded by the Phase 2 1000-render stress test). MuPDF feature-flag pruning (~2 MB mujs/gumbo/tesseract) is **deferred to 11.5** (requires rebuilding MuPDF, which pairs with the upgrade). |
| D3 | Where is the regression number measured? | **Headless `litepdf-cli` harness (best-of-5 min), gating open+render; GUI `T0->T4` stays an absolute ceiling** | `T0->T4` mixes CPU stages (MuPDF open `T1->T2`, rasterize `T2->T3`) with GPU stages (D2D factory `T0->T1`, blit `T3->T4`). CI has no GPU → Direct2D runs under WARP → the GPU stages are slow and high-variance. Everyday changes regress the CPU stages, which the harness measures cleanly and GPU-independently. The WARP stages rarely regress and can't be measured stably, so they stay out of the ±10% gate and under the existing loose absolute ceiling. |
| D4 | Where does the baseline come from? | **Rebuild the PR's base commit in the same CI job and compare on the same runner (self-calibrating)** | Dominant noise source is GitHub runner heterogeneity (different CPU per run shifts a pure-CPU number 10–20%). Measuring base and PR on the *same* runner in the *same* job cancels it, so ±10% is meaningful. Cost: the benchmark job builds MuPDF twice. Accepted — a false-failing gate gets ignored. |
| D5 | Active cold-start tuning in scope? | **No — gate only** | ~4× headroom (the GUI ceiling is 233 ms vs a 1 s target) makes active optimization low-ROI now, and the highest-leverage lever (feature-flag pruning) is deferred to 11.5. "Tuning" becomes reactive: the gate tells you when something regressed. |
| D6 | The `large.pdf` fixture the gate needs does not exist | **Generate a deterministic synthetic `large.pdf` as part of this phase** | The v1 spec wrongly claimed `tests/fixtures/large.pdf` existed (only `simple.pdf` and friends do). A real timing gate also needs a fixture big enough that `open_render_ms` isn't dominated by timer granularity — `simple.pdf` (~15 KB) is too small (review major). A new `scripts/generate-large-fixture.py` (reportlab, mirroring `generate-search-fixture.py`: white-filled pages, deterministic, English text) synthesizes a ~500-page PDF committed as `tests/fixtures/large.pdf`. **`large.pdf` becomes the primary gated timing fixture**; `simple.pdf` stays informational. |
| D7 | Bootstrap: base commit lacks the harness/scripts | **Two-step rollout** | The PR that introduces the gate has a base (`main`) that contains no `--benchmark`, no `benchmark.ps1`, no `thresholds.json` — so a base rebuild can't produce `base.json`. **PR1** lands the harness + scripts + fixture + self-test and runs the benchmark job in **measure-only** mode (build PR, emit `pr.json`, run the harness smoke + the compare-logic self-test; no base comparison). **PR2** enables the rebuild-base comparison. By the time PR2's gate enforces, `main` already has everything it needs. |
| D8 | Which base ref does the gate compare against? | **The PR's immutable base SHA (`github.event.pull_request.base.sha`)** | Comparing against the moving tip of `main` lets the same PR flip pass/fail when `main` advances for unrelated reasons. Pinning the base SHA makes reruns reproducible. |

## 3. Components

### 3.1 Benchmark harness — extend `src/cli/main.cpp`

`litepdf-cli` already exists (console demo + `--render N`) and is built by CMake.
Add a benchmark mode rather than a new binary:

```
litepdf-cli <fixture> --benchmark [--iterations N] [--json]
```

**Argument rules:** `--benchmark` and `--render` are mutually exclusive (error if
both). `--iterations` (default 5) and `--json` are only meaningful with
`--benchmark`. The positional `<fixture>` stays argv[1] as today.

**Per-iteration measurement** (mirrors the existing `--render` await/drop pattern
in `main.cpp`, which is the only correct way to drive `RenderEngine`):

- Construct a **fresh** `core::Document`; time `open()` → `open_ms`.
- Construct a **fresh** `core::RenderEngine(doc, /*workers=*/1, /*cache=*/nullptr)`.
  One worker for determinism; **no `PageCache`** so the metric reflects real
  rasterization, not an L1/L2 cache hit (a reused engine would let iterations
  2..N collapse `min` onto cache-lookup latency and hide render regressions).
- Time **from immediately before `submit({page:0, rot:0, scale:1.0f, cb})` to
  inside the `on_complete` callback** → `render_ms`. `submit()` returns
  immediately and the callback fires on the worker thread; block on the same
  `condition_variable` + `wait_for(10s)` timeout the `--render` path uses, and
  `fz_drop_pixmap` in the callback exactly as `--render` does. `scale=1.0f`
  (72 dpi). `render_ms` includes whatever the worker does to produce the first
  pixmap (incl. any per-worker `fz_open_document`); that is fine because it is
  measured identically on base and PR.
- `open_render_ms = open_ms + render_ms`.

Runs `N` iterations, reports the **minimum** (best-of-N) of each metric. Min is
chosen because cold-start noise is right-skewed (occasional slowdowns); the
minimum best approximates the true work floor and is the most noise-resistant
aggregate. A real regression raises the floor, so `min` still rises.

`--json` emits one machine-readable object to stdout (see §3.2 for the combined
file); without `--json`, prints a human summary (dev use). Exit non-zero if the
fixture fails to open or render (no silent zero metric).

**Warm-cache note (intentional scope boundary):** iteration 1 is cold on the OS
file cache, 2..N warm. `min` therefore measures warm-cache CPU time. This is
deliberate — the gate defends **CPU work** (parsing, rasterization), not disk
seek time, which is runner/hardware-dependent. A regression that is *purely* more
disk IO on open is explicitly **out of scope** for this gate.

### 3.2 `scripts/benchmark.ps1` (measure)

Runs the harness over the gated fixtures, collects each timing JSON, measures the
GUI binary's size, and writes one combined result file. Two **distinct required
parameters** so the size gate can never accidentally measure the CLI binary:

- `-CliExe <path>` — the `litepdf-cli` binary to **run** for timings. Must exist
  and its filename must be `litepdf-cli.exe`; else fail.
- `-GuiExe <path>` — the `litepdf.exe` binary whose **size** is recorded. Must
  exist and its filename must be `litepdf.exe`; else fail.
- `-Out <path>` — output result file (path resolved from caller's cwd).
- `-Iterations <N>` — forwarded to the harness (default 5).

Result schema (carries provenance so a bad comparison can't hide Debug-vs-Release
or wrong-artifact mistakes):

```json
{
  "schema_version": 1,
  "git_sha": "3576426...",
  "config": "Release",
  "runner_os": "windows-2022",
  "gui_exe_path": "build/Release/litepdf.exe",
  "exe_bytes": 40894464,
  "fixtures": {
    "large.pdf":  {"open_ms": <n>, "render_ms": <n>, "open_render_ms": <n>, "samples": [..]},
    "simple.pdf": {"open_ms": <n>, "render_ms": <n>, "open_render_ms": <n>, "samples": [..]}
  }
}
```

`samples` is the raw per-iteration `open_render_ms` array, for variance
diagnostics. PowerShell 5.1-parse-safe (no `?.`/`??`/ternary — project memory
`reference_litepdf_powershell_51_only`); paths resolved via `$PSScriptRoot` /
`Join-Path`, matching `smoke-test.ps1` / `check-version-sync.ps1`.

### 3.3 `scripts/check-benchmark-regression.ps1` (compare + gate)

Inputs: `-Base <base.json>`, `-Pr <pr.json>`; thresholds from
`benchmarks/thresholds.json` (resolved relative to `$PSScriptRoot`, so it behaves
identically from the PR checkout and the base worktree). Also exposes
`-SelfTest` (see §6). Computes a percentage delta for each gated timing metric and
a byte delta for `exe_bytes`, prints a comparison table, and exits non-zero if any
gated metric exceeds its threshold.

`benchmarks/thresholds.json`:
```json
{"time_regression_pct": 10, "size_regression_bytes": 262144}
```

The 256 KB size tolerance absorbs linker/compiler-version jitter; the gate is
anti-bloat regression protection, **not** a path to the design's aspirational
8 MB target (deferred to 11.5 pruning).

**Gated metrics** (the JSON path the compare script reads is explicit):
- `fixtures["large.pdf"]["open_render_ms"]` — primary cold-start CPU representative.
- `fixtures["large.pdf"]["open_ms"]` — guards the xref/parse path (500 pages
  amplify open cost).
- top-level `exe_bytes` — binary size.

`simple.pdf` metrics are **informational** (printed, never gated) — too small to
gate without timer-granularity flapping.

**Numeric validation (fail-closed):** before computing any delta, every gated
field in **both** files must be present, numeric, finite, and `> 0`; otherwise
**error exit** (never coerce a missing/zero base into a 0 that divides or passes).
Non-gated metrics present in `pr.json` but absent from `base.json` are logged as
informational and do **not** fail the gate. Malformed JSON → error exit.

### 3.4 Fixtures

- `tests/fixtures/simple.pdf` — exists; stays informational.
- `tests/fixtures/large.pdf` — **created this phase** by
  `scripts/generate-large-fixture.py` (reportlab; deterministic — pin the PDF
  producer/creation-date so regeneration is byte-stable; white-filled pages per
  `generate-search-fixture.py`'s dark-surface rationale; English filler text).
  Target ~500 pages, a few hundred KB. Committed to the repo so PR and base
  builds read identical bytes. A docstring records the page count and that it is
  load-bearing for the gate.

## 4. CI Integration — `.github/workflows/ci.yml`

A new `benchmark` job, running in parallel with `build-windows` (does not block or
modify it). **Triggers only on `pull_request`, and skips when the PR touches only
docs** (`paths-ignore: ['**/*.md', 'docs/**']`) — so the double MuPDF build is not
paid for documentation PRs (including this spec PR). On `push` to `main` there is
no base to compare, so the job does not run there.

Checkout requirements: `actions/checkout@v5` with `fetch-depth: 0` and
`submodules: recursive` (the default shallow clone would not contain the base
commit; a worktree does not auto-populate submodules).

**Step sequence (`shell: pwsh` for the `.ps1` steps):**

1. Build PR head: `cmake -B build ...` → `cmake --build build --config Release`.
2. `benchmark.ps1 -CliExe build/Release/litepdf-cli.exe -GuiExe build/Release/litepdf.exe -Out pr.json`.
3. Resolve the immutable base: `BASE=${{ github.event.pull_request.base.sha }}`.
   `git worktree add ../base "$BASE"`, then in the worktree
   `git submodule update --init --recursive`.
4. Build base in a **separate binary dir**: `cmake -S ../base -B build-base ...`
   → `cmake --build build-base --config Release`. (The worktree has its own
   submodule working tree, so its MuPDF `.sln` outputs land under
   `../base/third_party/mupdf/...`, isolated from the PR's `build/` — this is
   the load-bearing detail that makes D4's calibration and the size delta honest.)
5. `benchmark.ps1 -CliExe build-base/Release/litepdf-cli.exe -GuiExe build-base/Release/litepdf.exe -Out base.json`.
6. `check-benchmark-regression.ps1 -Base base.json -Pr pr.json` → pass/fail.

Both builds happen on the same runner in the same job (D4). The binary-size delta
falls out of building both exes.

**Two-step rollout (D7):** in **PR1** (this gate's introduction) steps 3–6 are
omitted; the job runs measure-only (steps 1–2) plus the §6 self-test and harness
smoke. **PR2** adds steps 3–6 to enable enforcement. Encode PR1 vs PR2 as the last
two items of §9 rather than a runtime branch, so the CI YAML never has to detect
"does the base have the script."

**PowerShell 5.1 guard:** add a cheap CI step that force-parses each new `.ps1`
under Windows PowerShell 5.1 (`powershell.exe -NoProfile -Command
"[ScriptBlock]::Create((Get-Content -Raw <script>)) > $null"`), so a `?.`/`??`/
ternary slip fails CI even though the run steps use `pwsh` 7 (project memory
`reference_litepdf_powershell_51_only`).

Cost: this job builds MuPDF twice (~2× the `build-windows` wall time, dominated by
the MuPDF `.sln` build). Accepted per D4; it will likely be the longest CI job.
CMake/MuPDF build caching is an explicit out-of-scope follow-up.

## 5. Error Handling (fail-closed)

- Harness fails to open/render a fixture → non-zero exit; `benchmark.ps1`
  propagates; the job fails. No fixture silently contributes a 0 ms metric.
- Base checkout/build/measure fails → **hard failure**, clear message ("cannot
  establish comparison baseline"). The gate never degrades to a silent pass when
  it loses its reference.
- A gated numeric field missing/zero/negative/non-finite in either file → error
  exit (no divide-by-zero, no nonsense delta). See §3.3.
- Differing fixture key sets: a gated key absent in either file → error; a
  non-gated key absent in base → informational, not a failure.
- A single anomalous slow iteration → absorbed by best-of-N `min`. If *all* N are
  slow (a real regression) the `min` is still high and the gate correctly fails.
- **Flake-response policy:** if the gate flakes despite same-runner calibration
  (e.g. noisy-neighbor throttling mid-job), the response is to **widen the
  threshold**, never to add a retry that re-measures only the PR side — that would
  break the same-runner calibration D4 depends on.

## 6. Testing Strategy

The roadmap exit criterion ("a regression is correctly blocked") is made
**deterministic** by testing the compare logic directly, decoupled from real
timing jitter. **No Pester** (the repo has no Pester convention; `windows-latest`
ships only the legacy 3.x module). Instead, `check-benchmark-regression.ps1`
gains a `-SelfTest` switch that runs synthetic-JSON assertions in-process and
exits non-zero on any failure; CI invokes it as a step, and it is also wired as a
CTest `add_test` so `ctest` runs it locally. The five assertions:

1. a +15 % delta on a gated timing metric → non-zero exit (blocked);
2. a +5 % delta → zero exit (allowed);
3. `exe_bytes` growth beyond the 256 KB tolerance → blocked;
4. `exe_bytes` growth within tolerance → allowed;
5. a missing/zero/malformed gated field → error exit.

This *is* the "regression correctly blocked" evidence — reproducible without
runner timing.

Also:
- **Harness smoke**: CI runs `litepdf-cli large.pdf --benchmark --json` once and
  asserts the output parses as JSON with all expected fields present.
- **GUI absolute ceiling unchanged**: `scripts/smoke-test.ps1`'s
  `T0->T4 ≤ 1500 ms` check stays as-is (liveness + ceiling); only its comment is
  clarified to note it is the absolute ceiling, not the regression gate.

## 7. What This Phase Does NOT Do (deferred to 11.5 or later)

- MuPDF 1.24.11 → newer upgrade, and everything gated on it: case-sensitive
  search (`fz_search_page2` + `FZ_SEARCH_EXACT`), honoring `fz_cookie.abort` for
  mid-page search cancellation, and **DPI-/CPU-count-adaptive `SearchDispatcher`
  sizing**. The `TODO(phase-11)` code comments refer to this 11.5 work.
- MuPDF feature-flag pruning (mujs / gumbo / tesseract / leptonica) and any move
  toward the 8 MB exe target. Only anti-regression size protection lands here.
- Idle-RAM regression gating.
- Active cold-start optimization.
- Disk-IO (cold-cache) regression detection — the warm-cache metric (§3.1) does
  not catch it.

## 8. Estimated Size

~250 LOC + one committed fixture: harness timing extension in `src/cli/main.cpp`;
`scripts/generate-large-fixture.py`; `scripts/benchmark.ps1`;
`scripts/check-benchmark-regression.ps1` (incl. `-SelfTest`); a new `benchmarks/`
dir with `thresholds.json`; the `benchmark` CI job; the PS-5.1 parse guard step.
Slightly above the roadmap's ~200 LOC estimate because of the fixture generator
and the self-test, both surfaced by the review.

## 9. Build Sequence (for the implementation plan)

1. `scripts/generate-large-fixture.py`; generate + commit `tests/fixtures/large.pdf`;
   record page count in its docstring.
2. Harness: add `--benchmark` / `--iterations` / `--json` to `src/cli/main.cpp`
   (timing spans per §3.1); verify JSON locally on `simple.pdf` and `large.pdf`.
3. `scripts/benchmark.ps1` (`-CliExe`/`-GuiExe`/`-Out`/`-Iterations`); confirm the
   combined `result.json` shape.
4. `benchmarks/thresholds.json` + `scripts/check-benchmark-regression.ps1`
   (compare + numeric validation).
5. `-SelfTest` switch + CTest `add_test`; prove all five §6 assertions.
6. Clarify the `smoke-test.ps1` ceiling comment.
7. **PR1**: `benchmark` CI job in **measure-only** mode (build PR, `pr.json`,
   harness smoke, self-test, PS-5.1 guard; no base comparison). Land to `main`.
8. **PR2**: extend the job with the rebuild-base steps (`fetch-depth: 0`, base
   SHA worktree, submodule init, `build-base`, compare) to enable enforcement.
   Validate end-to-end by including a deliberate slowdown on a throwaway branch
   and confirming the gate blocks it (exit-criterion evidence).

## 10. Review Changelog (v1 → v2)

Three-lens review (Opus + Sonnet + Codex), 2026-06-03. Cross-model agreement in
brackets.

- **[3/3] Blocker** `large.pdf` didn't exist → D6: generate it; it becomes the
  primary gated fixture (also fixes the `simple.pdf`-too-small flakiness major).
- **[2/3] Blocker** bootstrap chicken-and-egg → D7: two-step rollout.
- **[3/3] Blocker** Pester convention didn't exist → §6: `-SelfTest` switch +
  CTest, no Pester.
- **[2/3] Blocker** `-Exe` could measure the wrong binary → §3.2: split into
  `-CliExe` (run) and `-GuiExe` (size), filename-validated.
- **[3/3] Major** headless timing under-specified / cache contamination → §3.1:
  explicit spans, fresh `Document`+`RenderEngine`, no `PageCache`, await/drop
  per `--render`.
- **[3/3] Major** worktree/submodule/fetch-depth/separate-build-dir → §4: spelled
  out (`fetch-depth: 0`, `submodule update --init --recursive` in the worktree,
  `-B build-base`, MuPDF `.sln` output isolation).
- **[1/3] Major** base-ref nondeterminism → D8: pin `pull_request.base.sha`.
- **[1/3] Major** "233 ms" was the GUI number, not the gated metric → §1: corrected.
- Minors folded in: result-schema provenance, `benchmarks/` dir declared,
  `shell: pwsh`, differing-fixture-key behavior, divide-by-zero guard, docs-only
  `paths-ignore`, PS-5.1 parse guard, numbered self-test assertions.
