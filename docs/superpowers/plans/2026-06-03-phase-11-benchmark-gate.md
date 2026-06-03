# Phase 11 — Benchmark Regression Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CI performance + binary-size regression gate so a change that slows the document-open + first-page-render CPU path, or grows `litepdf.exe`, is caught on the pull request that introduces it.

**Architecture:** A headless `litepdf-cli --benchmark` harness times `open_ms + engine_init_ms + render_ms` (best-of-N min) over a deterministic synthetic `large.pdf`; `benchmark.ps1` runs it and records the GUI exe size into one provenance-stamped JSON; `check-benchmark-regression.ps1` compares a PR result against the PR's **base commit rebuilt on the same CI runner** (self-calibrating, kills runner CPU heterogeneity). The gate ships in two PRs: PR1 measure-only (lands the harness, reports noise floors), PR2 enables the rebuild-base comparison. The gate protects the current numbers; it does not improve them.

**Tech Stack:** C++17 (`src/cli/main.cpp`, MuPDF 1.24.11, `core::Document`/`core::RenderEngine`), PowerShell 5.1-safe scripts, Python 3 + reportlab (fixture generator), CMake + CTest, GitHub Actions (`windows-latest`, MSVC v143).

**Source spec:** [`docs/superpowers/specs/2026-06-03-phase-11-benchmark-gate-design.md`](../specs/2026-06-03-phase-11-benchmark-gate-design.md) (v3, approved over two three-lens review rounds). Where this plan and the spec disagree, the spec wins — except the four explicitly-flagged refinements below.

---

## File Structure

**Create:**
- `scripts/generate-large-fixture.py` — reportlab generator for the gated fixture; `--check` byte-diff mode. One responsibility: produce/verify `large.pdf`.
- `tests/fixtures/large.pdf` — committed binary fixture (output of the generator). Gate-load-bearing.
- `scripts/benchmark.ps1` — runs the harness over the gated fixtures, records exe size, writes one combined result JSON. Orchestration + provenance only; all timing math lives in the harness.
- `scripts/check-benchmark-regression.ps1` — compare + gate, plus `-SelfTest`. Owns the gate decision logic and its deterministic self-test.
- `benchmarks/thresholds.json` — the three threshold numbers. Data only.
- `.github/workflows/benchmark.yml` — the gate's own workflow (separate from `ci.yml` so `paths-ignore` scopes the skip correctly).

**Modify:**
- `src/cli/main.cpp` — add `--benchmark`/`--iterations`/`--json` mode (the timing harness). All stats (min/median/stddev) computed here so the PowerShell stays dumb.
- `CMakeLists.txt` (root) — register the `benchmark_selftest` CTest test inside the existing `if(BUILD_TESTING)` block.
- `scripts/smoke-test.ps1` — clarify one comment (the `T0->T4` ceiling is the absolute ceiling, not the regression gate). No behavior change.

## Refinements beyond the spec (for the plan-gate 3-lens reviewer)

These four diverge from a literal reading of the spec. Each is justified; flagged here so the review is delta-scoped and can accept/reject them explicitly.

- **R-A (harness computes median/stddev):** spec §3.2 lists `median_ms`/`stddev_ms` in the combined file but does not say who computes them. This plan computes all timing stats in C++ (Task 2) and has `benchmark.ps1` merely nest the harness JSON. Rationale: PowerShell 5.1 numeric/stats work is error-prone; one place for the math.
- **R-B (`benchmark.ps1 -GitSha` optional param):** spec §3.2's param list is `-CliExe/-GuiExe/-Out/-Iterations`. But `git_sha` provenance for `base.json` is wrong if derived from the PR checkout (the script's cwd) — the base source lives in the `../base` worktree. This plan adds an optional `-GitSha` (defaults to `git rev-parse HEAD`); CI passes `$base` explicitly for `base.json`. Honors the spec's stated purpose for the field ("provenance so a bad comparison can't hide … wrong-artifact mistakes").
- **R-C (CTest interpreter via `find_program`):** spec §6 hardcodes `COMMAND pwsh`. Project memory `reference_litepdf_powershell_51_only` + rationale log (2026-05-31) record that the dev box has **only** Windows PowerShell 5.1, no `pwsh` — so a hardcoded `pwsh` breaks local `ctest`. This plan uses `find_program(LITEPDF_PWSH NAMES pwsh powershell)` so the test runs on the 5.1-only dev box and on CI (which has both). The scripts are authored 5.1-safe, so either interpreter is correct.
- **R-D (pinned reportlab):** spec §3.4 mandates byte-identical regeneration but `--check` is only meaningful if CI's reportlab matches the version that generated the committed fixture. This plan pins `reportlab==4.4.10` (the version verified installed on the build machine, Python 3.14.3) in Task 1's generation **and** in the workflow's install step. (If a future build machine has a different reportlab, regenerate the fixture with it and bump the pin in the docstring + `benchmark.yml` to match — a one-line change in two files.)

## Decisions carried from the spec (do not re-litigate)

D1 no MuPDF upgrade (gate against 1.24.11). D2 gate cold-start CPU path + binary size only. D3 measure headless, GUI `T0->T4` stays an absolute ceiling. D4 self-calibrate by rebuilding the base on the same runner. D5 gate only, no tuning. D6 generate `large.pdf`. D7 two-step rollout (PR1 measure-only → PR2 enforce). D8 pin `github.event.pull_request.base.sha`. **Gate the boundary-agnostic total `open_render_ms`.** Data-driven gating: a timing metric is promoted to gated only if PR1's floor clears the noise (`thresholds.json` carries an absolute `time_min_delta_ms` floor alongside the pct).

## Out of scope (deferred to Phase 11.5+, per spec §7)

MuPDF upgrade and the search features gated on it; MuPDF feature-flag pruning / 8 MB target; idle-RAM gating; active cold-start optimization; disk-IO (cold-cache) regression detection; CMake/MuPDF build caching.

## Release & versioning

No `VERSION` bump and no tag. These are infra PRs, not a user-facing release; this matches the repo's "no version bump on non-release PRs" convention (rationale log 2026-06-01, operational learning `litepdf-no-version-bump-non-release`). After PR2 lands, update the Phase 11 row in `docs/plans/2026-04-15-litepdf-roadmap.md` to done (exit criterion: baseline mechanism in place + a deliberate regression correctly blocked).

---

## Task 1: Deterministic `large.pdf` fixture + generator

**Files:**
- Create: `scripts/generate-large-fixture.py`
- Create (generated, committed): `tests/fixtures/large.pdf`

- [ ] **Step 1: Pin reportlab**

Run: `pip install "reportlab==4.4.10"`
Expected: `Requirement already satisfied` (this is the version verified installed on the build machine). If a different reportlab is installed, use that exact version here, in the script docstring (Step 2), and in `benchmark.yml` (Task 7 Step 4) — all three must match so `--check` byte-identity holds between the committed fixture and CI. Do NOT downgrade reportlab on this machine (Phase 9 T3 recorded that older reportlab loses the `_renderPM` backend here).

- [ ] **Step 2: Create the generator**

Create `scripts/generate-large-fixture.py`:

```python
#!/usr/bin/env python3
"""
Generate tests/fixtures/large.pdf — the primary gated timing fixture for the
Phase 11 benchmark regression gate.

Design (Phase 11 spec §3.4):
  * ~500 pages so the document-open path (xref / page-tree walk) has a chance
    of measurable cost. Whether open_ms actually clears the CI noise floor is
    decided empirically by PR1's measure-only run, not assumed here.
  * Page 0 is deliberately DENSE — many text runs plus vector content — so the
    first-page rasterize (render_ms) is measurable above the timer floor. The
    benchmark renders page 0 only; a sparse page would rasterize in near-zero
    time and give render_ms no signal (Codex round-2 finding).
  * Deterministic: reportlab invariant mode (rl_config.invariant = 1) pins the
    document timestamp and /ID so regeneration is byte-identical. Generated with
    reportlab==4.4.10; the benchmark workflow installs that same pinned version
    before running --check so the byte-diff is meaningful.
  * --check regenerates to a temp path and asserts byte-identity against the
    committed fixture, so any drift (a reportlab bump, an accidental edit) fails
    CI.

This fixture is GATE-LOAD-BEARING: changing its content shifts the baseline
timings the regression gate protects. Regenerate only deliberately.

White page fills are required because LitePDF's PdfCanvas uses a dark D2D
surface; see generate-search-fixture.py for the full rationale.
"""

import argparse
import os
import sys
import tempfile

# Invariant mode MUST be set before the pdfgen machinery stamps the document so
# the fixed timestamp + deterministic /ID take effect. This is what makes
# regeneration byte-stable (the --check contract).
from reportlab import rl_config
rl_config.invariant = 1

from reportlab.lib.pagesizes import letter
from reportlab.lib.colors import white, black
from reportlab.pdfgen import canvas


PAGE_COUNT = 500

OUT_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "large.pdf",
)


def fill_page_white(c, width, height):
    """Draw an explicit white rectangle over the whole page (dark-surface fix)."""
    c.saveState()
    c.setFillColor(white)
    c.setStrokeColor(white)
    c.rect(0, 0, width, height, fill=1, stroke=0)
    c.setFillColor(black)  # restore default for subsequent drawString calls
    c.restoreState()


def draw_dense_page0(c, width, height):
    """Render-heavy first page: many text runs + vector content so render_ms
    has signal above the timer floor (spec §3.4)."""
    fill_page_white(c, width, height)

    # Two columns of ~11pt text gives a few hundred glyph runs on page 0.
    c.setFillColor(black)
    c.setFont("Helvetica", 11)
    left_x = 54
    right_x = width / 2 + 18
    top_y = height - 54
    line_h = 13
    sentence = ("The quick brown fox jumps over the lazy dog while "
                "rendering benchmark page zero.")
    rows = int((top_y - 54) / line_h)
    for col_x in (left_x, right_x):
        y = top_y
        for r in range(rows):
            c.drawString(col_x, y, "%03d %s" % (r, sentence))
            y -= line_h

    # Vector content: stroked rectangles + tick lines so the display list has
    # non-trivial fill/stroke ops, not just text.
    c.setStrokeColor(black)
    c.setLineWidth(0.5)
    for gx in range(0, int(width), 24):
        c.line(gx, 40, gx, 52)
    for i in range(40):
        x0 = 54 + i * 6
        c.rect(x0, 40, 5, 8, fill=0, stroke=1)


def draw_filler_page(c, width, height, page_number):
    """Light filler page: a single heading line. Keeps per-page open cost real
    (page-tree entries) without inflating render time on non-page-0 pages."""
    fill_page_white(c, width, height)
    c.setFillColor(black)
    c.setFont("Helvetica", 12)
    c.drawString(72, height - 72, "Filler page %d of %d." % (page_number, PAGE_COUNT))


def build(out_path):
    width, height = letter
    c = canvas.Canvas(out_path, pagesize=letter)
    # Pinned metadata (belt-and-suspenders on top of invariant mode).
    c.setTitle("LitePDF Phase 11 benchmark fixture")
    c.setAuthor("litepdf")
    c.setSubject("Deterministic large fixture for the cold-start regression gate")
    c.setCreator("generate-large-fixture.py")

    draw_dense_page0(c, width, height)
    c.showPage()
    for n in range(2, PAGE_COUNT + 1):
        draw_filler_page(c, width, height, n)
        c.showPage()
    c.save()


def main():
    parser = argparse.ArgumentParser(description="Generate or verify large.pdf")
    parser.add_argument("--check", action="store_true",
                        help="Regenerate to a temp path and assert byte-identity "
                             "with the committed fixture; exit 1 on drift.")
    args = parser.parse_args()

    if args.check:
        if not os.path.exists(OUT_PATH):
            print("[FAIL] committed fixture missing: %s" % OUT_PATH)
            sys.exit(1)
        with tempfile.TemporaryDirectory() as td:
            tmp = os.path.join(td, "large.pdf")
            build(tmp)
            with open(OUT_PATH, "rb") as f:
                committed = f.read()
            with open(tmp, "rb") as f:
                regenerated = f.read()
        if committed != regenerated:
            print("[FAIL] large.pdf drift: committed=%d B, regenerated=%d B "
                  "(byte-identity broken)" % (len(committed), len(regenerated)))
            sys.exit(1)
        print("[OK] large.pdf byte-identical (%d B)" % len(committed))
        sys.exit(0)

    build(OUT_PATH)
    print("Wrote: %s (%d bytes)" % (os.path.abspath(OUT_PATH), os.path.getsize(OUT_PATH)))


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Generate the fixture**

Run: `python scripts/generate-large-fixture.py`
Expected: `Wrote: ...tests/fixtures/large.pdf (NNNNNN bytes)`.

- [ ] **Step 4: Verify determinism (this is the test)**

Run: `python scripts/generate-large-fixture.py --check`
Expected: `[OK] large.pdf byte-identical (NNNNNN B)`.
If it prints `[FAIL] … drift`, invariant mode did not fully pin the output — do NOT proceed. Check the reportlab version actually installed (`pip show reportlab`) and that `rl_config.invariant = 1` runs before any canvas work; fix until `--check` is green.

- [ ] **Step 5: Commit**

```bash
git add scripts/generate-large-fixture.py tests/fixtures/large.pdf
git commit -m "feat(bench): add deterministic large.pdf fixture + generator"
```

(No `.gitattributes` change needed: `.gitattributes:29` already has an explicit `*.pdf binary` rule, so the committed fixture is never line-ending-normalized or diffed as text.)

---

## Task 2: Benchmark harness in `litepdf-cli`

**Files:**
- Modify: `src/cli/main.cpp`

- [ ] **Step 1: Add the new includes**

In `src/cli/main.cpp`, the include block currently ends at `<string>`. Add three headers used by the benchmark math. Replace:

```cpp
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
```

with:

```cpp
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
```

- [ ] **Step 2: Add the benchmark implementation**

Insert this anonymous-namespace block immediately **before** `int main(int argc, char* argv[]) {` (after the `#ifdef _WIN32 … #endif` include guard near the top of the file):

```cpp
namespace {

struct BenchIteration {
    double open_ms = 0.0;
    double engine_init_ms = 0.0;
    double render_ms = 0.0;
    double open_render_ms = 0.0;
};

// Render page 0 once on a FRESH Document + RenderEngine (no PageCache, one
// worker) and fill `it`. Mirrors the --render await/drop pattern below: the
// callback fires on the worker thread; we block on a condition_variable with a
// 10 s timeout and fz_drop_pixmap inside the callback. Returns false (and
// prints to stderr) if open or render fails — no silent zero metric.
//
// engine_init_ms is NOT zero: the RenderEngine constructor opens the
// per-worker fz_document, so a regression in worker-side open lands here, not
// in render_ms. Gating the total open_render_ms makes the boundary irrelevant.
bool run_one_iteration(const char* path, BenchIteration& it) {
    using clock = std::chrono::steady_clock;
    auto to_ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    auto t0 = clock::now();
    litepdf::core::Document doc;
    auto err = doc.open(path);
    auto t1 = clock::now();
    if (err) {
        std::fprintf(stderr, "Open error: %d\n", static_cast<int>(*err));
        return false;
    }
    it.open_ms = to_ms(t1 - t0);

    auto t2 = clock::now();
    litepdf::core::RenderEngine engine(doc, 1, nullptr);
    auto t3 = clock::now();
    it.engine_init_ms = to_ms(t3 - t2);

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    clock::time_point render_end;

    auto render_start = clock::now();
    engine.submit({
        0,        // page_num
        0,        // priority (0 = highest)
        1.0f,     // scale (1.0 = 72 dpi)
        [&](fz_pixmap* pix, fz_context* ctx) {
            auto end = clock::now();
            std::lock_guard<std::mutex> g(m);
            render_end = end;
            ok = (pix != nullptr);
            if (pix) fz_drop_pixmap(ctx, pix);
            done = true;
            cv.notify_all();
        }
    });

    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; })) {
            std::fprintf(stderr, "Render timed out\n");
            return false;
        }
    }
    if (!ok) {
        std::fprintf(stderr, "Render produced no pixmap\n");
        return false;
    }
    it.render_ms = to_ms(render_end - render_start);
    it.open_render_ms = it.open_ms + it.engine_init_ms + it.render_ms;
    return true;
}

double vmin(const std::vector<double>& v) {
    double best = v.front();
    for (double x : v) if (x < best) best = x;
    return best;
}

double vmedian(std::vector<double> v) {  // by value: sorts a copy
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double vstddev(const std::vector<double>& v) {  // sample stddev (N-1)
    if (v.size() < 2) return 0.0;
    double mean = 0.0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    double acc = 0.0;
    for (double x : v) acc += (x - mean) * (x - mean);
    return std::sqrt(acc / static_cast<double>(v.size() - 1));
}

// Runs `iterations` cold renders and reports best-of-N min of each metric.
// --json emits one compact object on stdout (the per-fixture shape
// benchmark.ps1 nests under the fixture key, §3.2). Returns 0 on success,
// 1 if any iteration fails to open/render.
int run_benchmark(const char* path, int iterations, bool json) {
    std::vector<double> open_s, init_s, render_s, total_s;
    open_s.reserve(iterations);
    init_s.reserve(iterations);
    render_s.reserve(iterations);
    total_s.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        BenchIteration it;
        if (!run_one_iteration(path, it)) {
            std::fprintf(stderr, "Benchmark failed on iteration %d for %s\n", i, path);
            return 1;
        }
        open_s.push_back(it.open_ms);
        init_s.push_back(it.engine_init_ms);
        render_s.push_back(it.render_ms);
        total_s.push_back(it.open_render_ms);
    }

    const double open_min   = vmin(open_s);
    const double init_min   = vmin(init_s);
    const double render_min = vmin(render_s);
    const double total_min  = vmin(total_s);
    const double median     = vmedian(total_s);
    const double stddev     = vstddev(total_s);

    if (json) {
        std::printf("{");
        std::printf("\"open_ms\":%.4f,", open_min);
        std::printf("\"engine_init_ms\":%.4f,", init_min);
        std::printf("\"render_ms\":%.4f,", render_min);
        std::printf("\"open_render_ms\":%.4f,", total_min);
        std::printf("\"samples\":[");
        for (std::size_t i = 0; i < total_s.size(); ++i) {
            std::printf("%s%.4f", (i ? "," : ""), total_s[i]);
        }
        std::printf("],");
        std::printf("\"median_ms\":%.4f,", median);
        std::printf("\"stddev_ms\":%.4f", stddev);
        std::printf("}\n");
    } else {
        std::printf("Benchmark %s (best-of-%d):\n", path, iterations);
        std::printf("  open_ms        : %.3f\n", open_min);
        std::printf("  engine_init_ms : %.3f\n", init_min);
        std::printf("  render_ms      : %.3f\n", render_min);
        std::printf("  open_render_ms : %.3f  (median %.3f, stddev %.3f over %d)\n",
                    total_min, median, stddev, iterations);
    }
    return 0;
}

} // namespace
```

- [ ] **Step 3: Wire arg parsing + dispatch + usage**

In `main()`, replace the current usage line and arg-parsing loop. Replace:

```cpp
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <file> [--render N]\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    int render_page = -1;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            render_page = std::atoi(argv[++i]);
        }
    }
```

with:

```cpp
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <file> [--render N | --benchmark [--iterations N] [--json]]\n",
            argv[0]);
        return 2;
    }

    const char* path = argv[1];
    int render_page = -1;
    bool benchmark = false;
    int iterations = 5;
    bool iterations_set = false;
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            render_page = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--benchmark") == 0) {
            benchmark = true;
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = std::atoi(argv[++i]);
            iterations_set = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        }
    }

    if (benchmark && render_page >= 0) {
        std::fprintf(stderr, "--benchmark and --render are mutually exclusive\n");
        return 2;
    }
    // --iterations / --json are only meaningful with --benchmark (spec §3.1).
    if (!benchmark && (json || iterations_set)) {
        std::fprintf(stderr, "--iterations/--json are only valid with --benchmark\n");
        return 2;
    }
    if (benchmark && iterations < 1) {
        std::fprintf(stderr, "--iterations must be >= 1\n");
        return 2;
    }
    if (benchmark) {
        return run_benchmark(path, iterations, json);
    }
```

(The existing `litepdf::core::Document doc; auto err = doc.open(path);` block and everything below it stays unchanged — the benchmark path returns before reaching it.)

- [ ] **Step 4: Build the harness**

Run: `cmake --build build --config Release --target litepdf-cli`
Expected: builds clean. (Assumes `build/` is already configured; if not, first run `cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release`.)

- [ ] **Step 5: Verify JSON output parses with all fields**

Run: `.\build\Release\litepdf-cli.exe tests/fixtures/large.pdf --benchmark --json | powershell -NoProfile -Command "$j = ($input | Out-String).Trim() | ConvertFrom-Json; 'open_render_ms=' + $j.open_render_ms + ' samples=' + $j.samples.Count + ' median=' + $j.median_ms"`
Expected: one line like `open_render_ms=NN.NN samples=5 median=NN.NN`, i.e. valid JSON with `samples` length 5. (`Out-String` + `.Trim()` mirrors Task 3 Step 3 so the pipeline is robust on PS 5.1 even if the harness ever emits trailing whitespace.)

- [ ] **Step 6: Verify human mode + mutual exclusion**

Run: `.\build\Release\litepdf-cli.exe tests/fixtures/simple.pdf --benchmark`
Expected: a `Benchmark … (best-of-5):` summary block with four metric lines.

Run: `.\build\Release\litepdf-cli.exe tests/fixtures/simple.pdf --benchmark --render 0; "exit=$LASTEXITCODE"`
Expected: stderr `--benchmark and --render are mutually exclusive` and `exit=2`.

(PowerShell needs the leading `.\` to run an executable by relative path; bare `build\Release\…` errors with "not recognized".)

- [ ] **Step 7: Commit**

```bash
git add src/cli/main.cpp
git commit -m "feat(bench): add litepdf-cli --benchmark timing harness"
```

---

## Task 3: `benchmark.ps1` (measure + combine)

**Files:**
- Create: `scripts/benchmark.ps1`

- [ ] **Step 1: Create the script**

Create `scripts/benchmark.ps1`:

```powershell
#!/usr/bin/env pwsh
#Requires -Version 5.1
# Runs the litepdf-cli benchmark harness over the gated fixtures, records the
# GUI exe size, and writes one combined result JSON (Phase 11 spec §3.2).
# Authored 5.1-safe (no ?./??/ternary) so it runs identically under Windows
# PowerShell 5.1 (local) and pwsh 7 (CI). See reference_litepdf_powershell_51_only.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CliExe,
    [Parameter(Mandatory = $true)][string]$GuiExe,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Iterations = 5,
    [string]$GitSha = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# --- Validate the two binaries. Filename-checked so the size gate can never
#     accidentally record the CLI exe, and timings can never come from the GUI.
if (-not (Test-Path $CliExe)) { throw "CliExe not found: $CliExe" }
if (-not (Test-Path $GuiExe)) { throw "GuiExe not found: $GuiExe" }
$cliLeaf = Split-Path -Leaf $CliExe
$guiLeaf = Split-Path -Leaf $GuiExe
if ($cliLeaf -ne "litepdf-cli.exe") { throw "CliExe filename must be litepdf-cli.exe, got '$cliLeaf'" }
if ($guiLeaf -ne "litepdf.exe")     { throw "GuiExe filename must be litepdf.exe, got '$guiLeaf'" }

# --- Run the harness over each gated fixture ------------------------------
$fixtureNames = @("large.pdf", "simple.pdf")
$fixtures = [ordered]@{}
foreach ($name in $fixtureNames) {
    $fixturePath = Join-Path $repoRoot (Join-Path "tests/fixtures" $name)
    if (-not (Test-Path $fixturePath)) { throw "fixture not found: $fixturePath" }

    $raw = & $CliExe $fixturePath --benchmark --iterations $Iterations --json
    if ($LASTEXITCODE -ne 0) {
        throw "harness failed (exit $LASTEXITCODE) on $name"
    }
    $rawText = ($raw | Out-String).Trim()
    try {
        $obj = $rawText | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "harness emitted non-JSON for ${name}: $rawText"
    }
    $fixtures[$name] = $obj
}

# --- Provenance -----------------------------------------------------------
# Default to the checkout this script lives in (correct for PR / local). CI
# passes -GitSha explicitly for base.json, whose source is a separate worktree.
if ([string]::IsNullOrEmpty($GitSha)) {
    $GitSha = (& git -C $repoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrEmpty($GitSha)) {
        throw "git rev-parse HEAD failed in $repoRoot; pass -GitSha explicitly"
    }
}
$runnerOs = $env:ImageOS
if ([string]::IsNullOrEmpty($runnerOs)) { $runnerOs = "local" }

$exeBytes = (Get-Item $GuiExe).Length

$result = [ordered]@{
    schema_version = 1
    git_sha        = $GitSha
    config         = "Release"
    runner_os      = $runnerOs
    cli_exe_path   = $CliExe
    gui_exe_path   = $GuiExe
    exe_bytes      = $exeBytes
    fixtures       = $fixtures
}

# Depth 10: result -> fixtures -> <name> -> samples[] -> elements; well above
# the PS 5.1 default of 2 so the samples array never collapses to a type name.
$json = $result | ConvertTo-Json -Depth 10
# BOM-less UTF-8. PS 5.1's Set-Content -Encoding UTF8 prepends a BOM; write
# BOM-less so the file is clean for every consumer. Resolve $Out via the PS
# provider so a relative path roots at the PS location, not the process CWD
# that [IO.File] would otherwise use.
$outFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
[System.IO.File]::WriteAllText($outFull, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "[OK] wrote $Out (exe_bytes=$exeBytes, git_sha=$GitSha)"
```

- [ ] **Step 2: Run it locally and inspect the result**

Run: `./scripts/benchmark.ps1 -CliExe build/Release/litepdf-cli.exe -GuiExe build/Release/litepdf.exe -Out pr.json`
Expected: `[OK] wrote pr.json (exe_bytes=… git_sha=…)`.
(Requires `build/Release/litepdf.exe` to exist — build the GUI target first if needed: `cmake --build build --config Release --target litepdf`.)

- [ ] **Step 3: Confirm the combined JSON shape**

Run: `powershell -NoProfile -Command "$r = Get-Content pr.json -Raw | ConvertFrom-Json; 'schema=' + $r.schema_version + ' exe_bytes=' + $r.exe_bytes + ' large_total=' + $r.fixtures.'large.pdf'.open_render_ms + ' large_samples=' + $r.fixtures.'large.pdf'.samples.Count"`
Expected: one line showing `schema=1`, a non-zero `exe_bytes`, a non-zero `large_total`, and `large_samples=5`.

- [ ] **Step 4: Commit**

```bash
git add scripts/benchmark.ps1
git commit -m "feat(bench): add benchmark.ps1 harness runner + result combiner"
```

(Leave the local `pr.json` uncommitted — it is a transient artifact, not a repo file. Add it to your scratch cleanup, do NOT `git add` it.)

---

## Task 4: Thresholds + compare/gate script (with `-SelfTest`)

**Files:**
- Create: `benchmarks/thresholds.json`
- Create: `scripts/check-benchmark-regression.ps1`

- [ ] **Step 1: Create the thresholds file**

Create `benchmarks/thresholds.json`:

```json
{"time_regression_pct": 10, "time_min_delta_ms": 5, "size_regression_bytes": 262144}
```

- [ ] **Step 2: Create the compare/gate script**

Create `scripts/check-benchmark-regression.ps1`:

```powershell
#!/usr/bin/env pwsh
#Requires -Version 5.1
# Compares a PR benchmark result against its base (Phase 11 spec §3.3) and
# exits non-zero if any gated metric regresses beyond threshold. Also offers
# -SelfTest: six synthetic-JSON assertions proving the gate logic with no real
# timing (the roadmap "regression correctly blocked" evidence, run as a CTest
# test). 5.1-safe syntax throughout — no ?./??/ternary, no [double]::IsFinite
# (absent on .NET Framework 4.x that backs PS 5.1).
[CmdletBinding(DefaultParameterSetName = "Compare")]
param(
    [Parameter(ParameterSetName = "Compare", Mandatory = $true)][string]$Base,
    [Parameter(ParameterSetName = "Compare", Mandatory = $true)][string]$Pr,
    [Parameter(ParameterSetName = "SelfTest", Mandatory = $true)][switch]$SelfTest
)

$ErrorActionPreference = "Stop"

# --- Gated-metric configuration (data-driven; finalized by PR1 floors) -----
# open_render_ms is the boundary-agnostic total and the primary gate. open_ms
# is added to $GatedTimingMetrics in PR2 ONLY if PR1's measure-only run shows
# it clears the noise floor (spec §3.3). simple.pdf is always informational.
$GatedFixture       = "large.pdf"
$GatedTimingMetrics = @("open_render_ms")

function Get-Thresholds {
    $path = Join-Path (Split-Path -Parent $PSScriptRoot) "benchmarks/thresholds.json"
    if (-not (Test-Path $path)) { throw "thresholds.json not found at $path" }
    $t = (Get-Content $path -Raw) | ConvertFrom-Json
    # Fail-closed: a corrupt thresholds.json must error, not silently gate on a
    # missing/zero/non-finite bound. (Assert-PositiveFinite is defined below; it
    # exists by the time this function is first CALLED in the dispatch path.)
    [void](Assert-PositiveFinite $t.time_regression_pct   "thresholds.time_regression_pct")
    [void](Assert-PositiveFinite $t.time_min_delta_ms     "thresholds.time_min_delta_ms")
    [void](Assert-PositiveFinite $t.size_regression_bytes "thresholds.size_regression_bytes")
    return $t
}

# Validate a value is numeric, finite, and > 0; return it as a double. Throws a
# "BENCH_VALIDATION:" message (recognized by callers) on any failure, so a
# missing/zero/non-finite gated field becomes an error exit, never a 0 that
# divides or silently passes.
function Assert-PositiveFinite {
    param($Value, [string]$Label)
    $isNum = ($Value -is [int]) -or ($Value -is [long]) -or ($Value -is [double]) -or ($Value -is [decimal])
    if (-not $isNum) { throw "BENCH_VALIDATION: $Label is missing or non-numeric" }
    $d = [double]$Value
    if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { throw "BENCH_VALIDATION: $Label is not finite" }
    if ($d -le 0) { throw "BENCH_VALIDATION: $Label must be > 0 (got $d)" }
    return $d
}

# Core comparison. Returns [pscustomobject]@{ Failed; Rows }. Does NOT call
# exit, so -SelfTest can assert on the result. Throws "BENCH_VALIDATION:" on a
# bad gated field (assertion 6).
function Invoke-BenchmarkCompare {
    param($BaseObj, $PrObj, $Thresholds)

    $rows = @()
    $failed = $false

    # exe_bytes — always gated, deterministic.
    $baseBytes = Assert-PositiveFinite $BaseObj.exe_bytes "base.exe_bytes"
    $prBytes   = Assert-PositiveFinite $PrObj.exe_bytes   "pr.exe_bytes"
    $byteDelta = $prBytes - $baseBytes
    $sizeFail  = ($byteDelta -gt $Thresholds.size_regression_bytes)
    if ($sizeFail) { $failed = $true }
    $rows += [pscustomobject]@{
        Metric = "exe_bytes"; Base = $baseBytes; Pr = $prBytes
        Delta = $byteDelta; Pct = ""; Fail = $sizeFail
    }

    # Gated timing metrics on the gated fixture.
    $baseFx = $BaseObj.fixtures.$GatedFixture
    $prFx   = $PrObj.fixtures.$GatedFixture
    if ($null -eq $baseFx) { throw "BENCH_VALIDATION: base.fixtures.$GatedFixture missing" }
    if ($null -eq $prFx)   { throw "BENCH_VALIDATION: pr.fixtures.$GatedFixture missing" }

    foreach ($metric in $GatedTimingMetrics) {
        $b = Assert-PositiveFinite $baseFx.$metric "base.$GatedFixture.$metric"
        $p = Assert-PositiveFinite $prFx.$metric   "pr.$GatedFixture.$metric"
        $absDelta = $p - $b
        $pct = ($absDelta / $b) * 100.0
        $fail = ($pct -gt $Thresholds.time_regression_pct) -and ($absDelta -gt $Thresholds.time_min_delta_ms)
        if ($fail) { $failed = $true }
        $rows += [pscustomobject]@{
            Metric = "$GatedFixture/$metric"; Base = $b; Pr = $p
            Delta = $absDelta; Pct = [math]::Round($pct, 2); Fail = $fail
        }
    }

    return [pscustomobject]@{ Failed = $failed; Rows = $rows }
}

function New-SyntheticResult {
    param([double]$OpenRender, [double]$ExeBytes)
    return [pscustomobject]@{
        exe_bytes = $ExeBytes
        fixtures  = [pscustomobject]@{
            "large.pdf" = [pscustomobject]@{ open_render_ms = $OpenRender }
        }
    }
}

function Invoke-SelfTest {
    $thr = Get-Thresholds
    $script:selfTestOk = $true

    function Check([bool]$cond, [string]$label) {
        if ($cond) { Write-Host "[PASS] $label" }
        else { Write-Host "[FAIL] $label"; $script:selfTestOk = $false }
    }

    # 1: +15% delta, above the absolute min-delta -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 115 40000000) $thr
    Check ($r.Failed -eq $true) "1: +15% (> min-delta) blocks"

    # 2: +5% delta -> allowed (under the pct threshold).
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 105 40000000) $thr
    Check ($r.Failed -eq $false) "2: +5% allowed"

    # 3: +50% but tiny absolute (4 ms < 5 ms floor) -> allowed.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 8 40000000) (New-SyntheticResult 12 40000000) $thr
    Check ($r.Failed -eq $false) "3: +50% but < min-delta allowed"

    # 4: exe growth beyond the 256 KB tolerance -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 100 40300000) $thr
    Check ($r.Failed -eq $true) "4: exe +300000 B blocks"

    # 5: exe growth within tolerance -> allowed.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) (New-SyntheticResult 100 40100000) $thr
    Check ($r.Failed -eq $false) "5: exe +100000 B allowed"

    # 6: a zero gated field -> error (throws BENCH_VALIDATION).
    $threw = $false
    try {
        $bad = [pscustomobject]@{
            exe_bytes = 40000000
            fixtures  = [pscustomobject]@{ "large.pdf" = [pscustomobject]@{ open_render_ms = 0 } }
        }
        Invoke-BenchmarkCompare (New-SyntheticResult 100 40000000) $bad $thr | Out-Null
    } catch {
        if ($_.Exception.Message -like "BENCH_VALIDATION*") { $threw = $true }
    }
    Check $threw "6: zero gated field errors"

    if ($script:selfTestOk) {
        Write-Host "[OK] benchmark self-test: 6/6 passed"
        exit 0
    }
    Write-Host "[FAIL] benchmark self-test had failures"
    exit 1
}

# ---------------------------------------------------------------- dispatch ---
if ($SelfTest) {
    Invoke-SelfTest   # exits
}

if (-not (Test-Path $Base)) { throw "Base result not found: $Base" }
if (-not (Test-Path $Pr))   { throw "PR result not found: $Pr" }
$thr = Get-Thresholds

try {
    $baseObj = (Get-Content $Base -Raw) | ConvertFrom-Json -ErrorAction Stop
    $prObj   = (Get-Content $Pr   -Raw) | ConvertFrom-Json -ErrorAction Stop
} catch {
    Write-Host "[ERROR] malformed benchmark JSON: $($_.Exception.Message)"
    exit 2
}

try {
    $cmp = Invoke-BenchmarkCompare $baseObj $prObj $thr
} catch {
    if ($_.Exception.Message -like "BENCH_VALIDATION*") {
        Write-Host "[ERROR] $($_.Exception.Message)"
        exit 2
    }
    throw
}

Write-Host ""
Write-Host ("{0,-28} {1,16} {2,16} {3,14} {4,8} {5}" -f "Metric", "Base", "Pr", "Delta", "Pct%", "Result")
foreach ($row in $cmp.Rows) {
    $status = "ok"
    if ($row.Fail) { $status = "FAIL" }
    Write-Host ("{0,-28} {1,16} {2,16} {3,14} {4,8} {5}" -f $row.Metric, $row.Base, $row.Pr, $row.Delta, $row.Pct, $status)
}
Write-Host ""

if ($cmp.Failed) {
    Write-Host "[FAIL] benchmark regression gate: one or more gated metrics regressed."
    exit 1
}
Write-Host "[OK] benchmark regression gate: all gated metrics within threshold."
exit 0
```

- [ ] **Step 3: Run the self-test (this is the test)**

Run: `powershell -NoProfile -File scripts/check-benchmark-regression.ps1 -SelfTest`
Expected: six `[PASS]` lines then `[OK] benchmark self-test: 6/6 passed` (exit 0).

- [ ] **Step 4: Verify the PS 5.1 parser accepts both scripts**

Run: `powershell -NoProfile -Command "[ScriptBlock]::Create((Get-Content -Raw scripts/check-benchmark-regression.ps1)) > $null; [ScriptBlock]::Create((Get-Content -Raw scripts/benchmark.ps1)) > $null; 'parse-ok'"`
Expected: `parse-ok` (a `?.`/`??`/ternary slip would throw a parse error here).

- [ ] **Step 5: Commit**

```bash
git add benchmarks/thresholds.json scripts/check-benchmark-regression.ps1
git commit -m "feat(bench): add thresholds + regression compare gate with self-test"
```

---

## Task 5: Wire the self-test into CTest

**Files:**
- Modify: `CMakeLists.txt` (root, inside the existing `if(BUILD_TESTING)` block)

- [ ] **Step 1: Add the CTest registration**

In root `CMakeLists.txt`, the tests block currently reads:

```cmake
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

Replace it with:

```cmake
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)

    # Phase 11: deterministic compare-logic self-test. Use whichever PowerShell
    # is present — pwsh on CI, Windows PowerShell 5.1 on the dev box (which has
    # no pwsh; see reference_litepdf_powershell_51_only). The script is authored
    # 5.1-safe, so either interpreter is correct. $PSScriptRoot-relative
    # threshold resolution + WORKING_DIRECTORY keep the test location-independent.
    find_program(LITEPDF_PWSH NAMES pwsh powershell.exe powershell
                 DOC "PowerShell interpreter for the benchmark self-test")
    if(LITEPDF_PWSH)
        add_test(NAME benchmark_selftest
                 COMMAND "${LITEPDF_PWSH}" -NoProfile -File
                         "${CMAKE_SOURCE_DIR}/scripts/check-benchmark-regression.ps1" -SelfTest)
        set_tests_properties(benchmark_selftest PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    else()
        message(WARNING "No PowerShell interpreter found; benchmark_selftest not registered.")
    endif()
endif()
```

- [ ] **Step 2: Reconfigure so CTest picks up the new test**

Run: `cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release`
Expected: configures clean; CMake STATUS line shows it found a PowerShell interpreter (no WARNING).

- [ ] **Step 3: Run just the self-test through CTest (this is the test)**

Run: `ctest --test-dir build -C Release -R benchmark_selftest --output-on-failure`
Expected: `1/1 Test #N: benchmark_selftest .... Passed`.

- [ ] **Step 4: Confirm the existing suite still runs (no regression)**

Run: `ctest --test-dir build -C Release` (or `--output-on-failure`)
Expected: the full Catch2 suite plus `benchmark_selftest` all pass (Catch2 count unchanged — this plan adds no `TEST_CASE`s).

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "test(bench): register benchmark_selftest as a CTest test"
```

---

## Task 6: Clarify the smoke-test ceiling comment

**Files:**
- Modify: `scripts/smoke-test.ps1`

- [ ] **Step 1: Update the `T0->T4` budget comment**

In `scripts/smoke-test.ps1`, the `T0->T4` parse block reads:

```powershell
if ($timingLine -match "T0->T4=(\d+)\s*ms") {
    $t4 = [int]$Matches[1]
    # Loose CI budget. Dev measured ~233 ms; CI runners are typically 3-5x slower.
    $budget = 1500
```

Replace that comment line with:

```powershell
if ($timingLine -match "T0->T4=(\d+)\s*ms") {
    $t4 = [int]$Matches[1]
    # ABSOLUTE CEILING, not the regression gate. This is the GUI T0->T4 line
    # (WARP-inclusive: D2D factory + blit run under software rendering in CI),
    # which is too GPU-noisy to gate at +/-10%. The Phase 11 benchmark gate
    # (.github/workflows/benchmark.yml) protects the CPU path via the headless
    # litepdf-cli harness; this check only catches a gross liveness/ceiling
    # blowout. Dev measured ~233 ms; CI runners are typically 3-5x slower.
    $budget = 1500
```

- [ ] **Step 2: Verify the script still parses**

Run: `powershell -NoProfile -Command "[ScriptBlock]::Create((Get-Content -Raw scripts/smoke-test.ps1)) > $null; 'parse-ok'"`
Expected: `parse-ok`.

- [ ] **Step 3: Commit**

```bash
git add scripts/smoke-test.ps1
git commit -m "docs(smoke): clarify T0->T4 is the absolute ceiling, not the regression gate"
```

---

## Task 7: PR1 — `benchmark.yml` in measure-only mode

**Files:**
- Create: `.github/workflows/benchmark.yml`

This is the first of the two rollout PRs (D7). It lands the harness and runs the benchmark job in **measure-only** mode: build the PR, emit `pr.json`, run the self-test + harness smoke + fixture `--check` + PS-5.1 parse guard, and **report the observed noise floors** so PR2's gated-metric promotion is data-driven. No base comparison yet. (PR1 uses the default `fetch-depth`; the full-history `fetch-depth: 0` is added in PR2, where the base SHA must be reachable for the worktree — intentionally deferred, not an omission.)

- [ ] **Step 1: Create the workflow**

Create `.github/workflows/benchmark.yml`:

```yaml
name: Benchmark

on:
  pull_request:
    paths-ignore: ['**/*.md', 'docs/**']   # skip the build for docs-only PRs

jobs:
  benchmark:
    runs-on: windows-latest                 # MSVC v143 + pwsh + windows binary layout

    steps:
      - name: Checkout with submodules
        uses: actions/checkout@v5
        with:
          submodules: recursive

      - name: PS 5.1 parse guard
        shell: pwsh
        run: |
          $scripts = @(
            'scripts/benchmark.ps1',
            'scripts/check-benchmark-regression.ps1'
          )
          foreach ($s in $scripts) {
            powershell.exe -NoProfile -Command "[ScriptBlock]::Create((Get-Content -Raw '$s')) | Out-Null"
            if ($LASTEXITCODE -ne 0) { throw "PS 5.1 parse failed for $s" }
          }
          Write-Host "[OK] PS 5.1 parse guard passed"

      - name: Install reportlab (pinned)
        run: pip install "reportlab==4.4.10"

      - name: Fixture determinism check
        run: python scripts/generate-large-fixture.py --check

      - name: Configure
        run: cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

      - name: Build PR head
        run: cmake --build build --config Release --parallel

      - name: Benchmark self-test (compare logic)
        shell: pwsh
        run: ./scripts/check-benchmark-regression.ps1 -SelfTest

      - name: Harness smoke (JSON well-formed)
        shell: pwsh
        run: |
          $out = & build/Release/litepdf-cli.exe tests/fixtures/large.pdf --benchmark --json
          if ($LASTEXITCODE -ne 0) { throw "harness exited $LASTEXITCODE" }
          $j = ($out | Out-String).Trim() | ConvertFrom-Json
          foreach ($f in @('open_ms','engine_init_ms','render_ms','open_render_ms','samples','median_ms','stddev_ms')) {
            if ($null -eq $j.$f) { throw "harness JSON missing field: $f" }
          }
          Write-Host "[OK] harness JSON has all fields (open_render_ms=$($j.open_render_ms))"

      - name: Measure PR
        shell: pwsh
        run: ./scripts/benchmark.ps1 -CliExe build/Release/litepdf-cli.exe -GuiExe build/Release/litepdf.exe -Out pr.json

      - name: Report observed noise floors
        shell: pwsh
        run: |
          $r = Get-Content pr.json -Raw | ConvertFrom-Json
          foreach ($name in @('large.pdf','simple.pdf')) {
            $fx = $r.fixtures.$name
            Write-Host ("{0,-12} open_render_ms min={1,8:N3} median={2,8:N3} stddev={3,7:N3} open_ms_min={4,8:N3} render_ms_min={5,8:N3}" -f `
              $name, $fx.open_render_ms, $fx.median_ms, $fx.stddev_ms, $fx.open_ms, $fx.render_ms)
          }
          Write-Host "exe_bytes=$($r.exe_bytes)"
          Write-Host "PR2 promotion rule: gate a timing metric only if median >= ~10x stddev and >= a few x time_min_delta_ms (5 ms)."

      - name: Upload pr.json
        uses: actions/upload-artifact@v5
        with:
          name: benchmark-pr-json
          path: pr.json
          if-no-files-found: error
```

- [ ] **Step 2: Validate the workflow YAML locally**

Run: `python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/benchmark.yml')); print('yaml-ok')"`
Expected: `yaml-ok`. (If PyYAML is absent, `pip install pyyaml` first, or skip and rely on the GitHub Actions parse on push.)

- [ ] **Step 3: Open PR1 and confirm the job is green**

```bash
git add .github/workflows/benchmark.yml
git commit -m "ci(bench): add measure-only benchmark workflow (PR1 rollout step)"
git push -u origin phase-11-benchmark-gate
gh pr create --title "Phase 11: benchmark regression gate (PR1, measure-only)" --body "First rollout step (D7). Lands the harness, fixture, scripts, and self-test; benchmark job runs measure-only and reports noise floors so PR2 can pick gated metrics from real data."
```

Then watch the run: `gh pr checks --watch`.
Expected: the `Benchmark / benchmark` job succeeds. In its log, the **"Report observed noise floors"** step prints `large.pdf` and `simple.pdf` min/median/stddev — **record these numbers**; they drive Task 8 Step 1. (Per CLAUDE.md rule 14, run the 3-lens plan/PR-merge review before merging — see "Review gates" below.)

- [ ] **Step 4: Merge PR1 to `main`**

After review passes, merge (squash or rebase per repo convention). `main` now contains the harness + scripts + fixture, so PR2's base rebuild has everything it needs.

---

## Task 8: PR2 — enable the rebuild-base gate

**Files:**
- Modify: `.github/workflows/benchmark.yml`
- Modify: `scripts/check-benchmark-regression.ps1` (only if PR1 data promotes `open_ms`)

PR2 adds the base rebuild + comparison and pins which timing metrics are gated based on PR1's reported floors. By now `main` already has the harness, so the base rebuild can produce `base.json`.

- [ ] **Step 1: Lock the gated-metric set from PR1 data**

Review the noise floors PR1's job printed (Task 7 Step 3). `open_render_ms` is the primary gate and stays in `$GatedTimingMetrics`. Promote `open_ms` to a second gate **only if** its `large.pdf` floor cleared the noise (rule of thumb: `median >= ~10x stddev` and `>= a few x 5 ms`). 
  - If PR1 shows `open_ms` has real signal: edit `scripts/check-benchmark-regression.ps1` line `$GatedTimingMetrics = @("open_render_ms")` to `$GatedTimingMetrics = @("open_render_ms", "open_ms")`, and re-run `-SelfTest` to confirm it still passes (the self-test only exercises `open_render_ms`, so it stays green).
  - If PR1 shows `open_ms` is noise-dominated: leave `$GatedTimingMetrics` unchanged and note the decision in the PR2 description.

- [ ] **Step 2: Add the base-rebuild + compare steps to the workflow**

In `.github/workflows/benchmark.yml`, change the checkout to fetch full history (the base SHA must be fetchable), and append the base steps after "Measure PR".

Change the checkout step to:

```yaml
      - name: Checkout with submodules
        uses: actions/checkout@v5
        with:
          fetch-depth: 0          # base SHA must be reachable for the worktree
          submodules: recursive
```

Then, **after** the "Measure PR" step and **before** "Upload pr.json", insert:

```yaml
      - name: Add base worktree at the PR's base SHA
        shell: pwsh
        run: |
          $base = "${{ github.event.pull_request.base.sha }}"
          Write-Host "Base SHA: $base"
          git worktree add ../base $base
          git -C ../base submodule update --init --recursive

      - name: Build base in a separate binary dir
        run: |
          cmake -S ../base -B build-base -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
          cmake --build build-base --config Release --parallel

      - name: Measure base
        shell: pwsh
        run: |
          $base = "${{ github.event.pull_request.base.sha }}"
          ./scripts/benchmark.ps1 -CliExe build-base/Release/litepdf-cli.exe -GuiExe build-base/Release/litepdf.exe -Out base.json -GitSha $base

      - name: Regression gate
        shell: pwsh
        run: ./scripts/check-benchmark-regression.ps1 -Base base.json -Pr pr.json
```

Notes for the reviewer / implementer:
- The base worktree gets its **own** submodule working tree, so MuPDF's `BUILD_IN_SOURCE` outputs land under `../base/third_party/mupdf/...`, isolated from the PR's `build/` (the detail that makes D4's same-runner calibration and the size delta honest — verified against `cmake/ImportMuPDF.cmake`).
- `base.json` is measured with the **PR's** `benchmark.ps1` (schema consistency) but `-GitSha $base` so its provenance is the base commit, not the PR (refinement R-B).
- This job builds MuPDF twice (~2x the `build-windows` wall time); it will be the longest CI job. Accepted per D4. Build caching is an out-of-scope follow-up.

- [ ] **Step 3: Validate the workflow YAML locally**

Run: `python -c "import yaml; yaml.safe_load(open('.github/workflows/benchmark.yml')); print('yaml-ok')"`
Expected: `yaml-ok`.

- [ ] **Step 4: Open PR2 and confirm the gate passes on a clean change**

```bash
git add .github/workflows/benchmark.yml scripts/check-benchmark-regression.ps1
git commit -m "ci(bench): enable rebuild-base regression gate (PR2 rollout step)"
git push
gh pr create --title "Phase 11: enable benchmark regression gate (PR2)" --body "Second rollout step (D7). Adds the base-SHA worktree rebuild + same-runner comparison and pins the gated timing metrics from PR1's observed floors. The benchmark job now fails a PR that regresses open_render_ms beyond +10% AND +5 ms, or grows litepdf.exe beyond 256 KB."
gh pr checks --watch
```

Expected: the `Regression gate` step prints the comparison table and `[OK] benchmark regression gate: all gated metrics within threshold.` (this PR makes no perf change). Run the 3-lens PR-merge review before merging.

- [ ] **Step 5: Prove the gate blocks a real regression (roadmap exit criterion)**

On a **throwaway** branch off PR2's head, introduce a deliberate slowdown so the gate must fail. Example: in `src/cli/main.cpp`'s `run_one_iteration`, immediately after `auto t2 = clock::now();` add `std::this_thread::sleep_for(std::chrono::milliseconds(40));` (include `<thread>`). Commit, push, open a draft PR against the same base.

```bash
git switch -c throwaway/bench-gate-proof
# edit src/cli/main.cpp: add the 40 ms sleep + #include <thread>
git commit -am "test: deliberate slowdown to prove the benchmark gate blocks"
git push -u origin throwaway/bench-gate-proof
gh pr create --draft --title "DO NOT MERGE: benchmark gate proof" --body "Deliberate +40 ms regression. Expect the Benchmark job to FAIL — this is the roadmap exit-criterion evidence."
gh pr checks --watch
```

Expected: the `Regression gate` step prints a `FAIL` row for `large.pdf/open_render_ms` and exits non-zero; the `Benchmark` job is red. **Capture the failing log** (this is the "a deliberate regression is correctly blocked in CI" evidence the roadmap requires). Then close the draft PR and delete the throwaway branch — it is never merged:

```bash
gh pr close throwaway/bench-gate-proof
git push origin --delete throwaway/bench-gate-proof
git switch phase-11-benchmark-gate
git branch -D throwaway/bench-gate-proof
```

- [ ] **Step 6: Merge PR2 and close out the phase**

After review + the exit-criterion evidence, merge PR2. Then update the Phase 11 row in `docs/plans/2026-04-15-litepdf-roadmap.md` to done (baseline mechanism in place; deliberate regression blocked). No `VERSION` bump / tag (non-release infra; see "Release & versioning").

---

## Review gates (CLAUDE.md rule 14)

- **Plan gate (now, before execution):** this plan gets a 3-lens review — Opus (`pr-review-toolkit:code-reviewer`) + Sonnet (`superpowers:code-reviewer`, `model: sonnet`) + Codex (`codex exec --skip-git-repo-check -C docs -s read-only -` on the plan, since it is an uncommitted markdown artifact). Delta-scoped: plan ↔ spec alignment, not re-reviewing the settled spec.
- **PR-merge gates:** PR1 and PR2 each get their own 3-lens review (Opus + Sonnet + Codex `review --base main` on the diff) before merge. The throwaway proof PR (Task 8 Step 5) is never merged, so it has no gate.

## Self-Review (run by the plan author)

**Spec coverage:** §3.1 harness → Task 2; §3.2 `benchmark.ps1` + result schema → Task 3; §3.3 compare/thresholds/validation → Task 4; §3.4 fixtures (`large.pdf` + `--check`) → Task 1; §4 CI workflow + PS-5.1 guard + two-step rollout → Tasks 7-8; §5 fail-closed error handling → Tasks 2 (harness non-zero exit), 3 (`$LASTEXITCODE` propagation), 4 (`Assert-PositiveFinite`, malformed-JSON exit 2); §6 self-test six assertions + CTest + fixture `--check` + smoke ceiling comment → Tasks 4, 5, 7, 6; §9 build sequence → Task ordering 1-8. No spec section left without a task.

**Placeholder scan:** every code step contains complete file content or exact replace-blocks; no "TBD"/"add error handling"/"similar to Task N". The one parameterized value (reportlab version) is a concrete pin (`4.4.10`, the build machine's installed version) with an explicit fallback instruction, not an open placeholder.

**Type/name consistency:** `open_render_ms`, `open_ms`, `engine_init_ms`, `render_ms`, `samples`, `median_ms`, `stddev_ms` are spelled identically in the harness JSON (Task 2), the combined schema (Task 3), and the compare script (Task 4). `$GatedFixture`/`$GatedTimingMetrics`/`Invoke-BenchmarkCompare`/`Assert-PositiveFinite`/`New-SyntheticResult` are referenced consistently within Task 4 and by the CTest command (Task 5). `RenderEngine(doc, 1, nullptr)` and `submit({0, 0, 1.0f, cb})` match the real signatures in `src/core/RenderEngine.hpp`. Binary filenames (`litepdf-cli.exe`, `litepdf.exe`) match `CMakeLists.txt` targets and `benchmark.ps1`'s filename validation.

---

## Plan-gate review log (3-lens, 2026-06-04)

Per CLAUDE.md rule 14, this plan was reviewed by three independent lenses, delta-scoped to plan↔spec alignment and the embedded code's correctness against the real codebase. **All three returned GO / APPROVE with zero blockers and zero majors.**

- **Opus** (`pr-review-toolkit:code-reviewer`): APPROVE. Verified the C++ harness against the real API (`RenderEngine(doc,1,nullptr)`; `submit({0,0,1.0f,cb})`; the eager per-worker open in the `RenderEngine` ctor at `RenderEngine.cpp:251-313`, confirming `engine_init_ms` attribution), PS 5.1 safety (no `IsFinite`), the CMake `BUILD_TESTING` fit, and base-worktree submodule isolation against `ImportMuPDF.cmake`. One real minor: the `.gitattributes` rationale was factually wrong.
- **Sonnet** (`feature-dev:code-reviewer`, model sonnet): raised 2 "blockers" + 3 majors; on verification most were false alarms (PS multi-segment relative paths do execute; `Get-Content -Raw` strips a BOM on read; `[double]::IsInfinity` is present on .NET 4.x; filler page numbering is correct 1-based). The robustness suggestions worth keeping were applied.
- **Codex** (gpt-5.5, `codex exec` read-only): GO. Confirmed C++ API match, clean PS 5.1, correct dual-threshold self-test (tiny-absolute allowed, zero-field errors), correct base isolation, justified refinements, reportlab determinism. Three real minors.

**Applied before execution:**
1. `.gitattributes` rationale corrected — the repo has an explicit `*.pdf binary` rule (`.gitattributes:29`), not implicit auto-detection. *(Opus)*
2. Harness rejects `--iterations`/`--json` without `--benchmark` (usage error), matching spec §3.1. *(Codex)*
3. `benchmark.ps1` checks `git rev-parse HEAD` exit + non-empty before using `git_sha`. *(Codex)*
4. `Get-Thresholds` validates the three threshold fields positive/finite (fail-closed on a corrupt `thresholds.json`). *(Codex)*
5. `benchmark.ps1` writes the result JSON BOM-less via `[IO.File]::WriteAllText` and bumps `ConvertTo-Json -Depth` to 10. *(Sonnet, defensive)*
6. Task 2 verification commands use the `.\`-prefixed exe path and `($input | Out-String).Trim()` in the inline pipeline. *(Sonnet, robustness/consistency)*
7. Task 7 notes that `fetch-depth: 0` is intentionally deferred to PR2. *(Sonnet, clarity)*

**Not actioned (with reason):** `runner_os` source is provenance-only and never gated (cosmetic); `pr.json`/`base.json` are transient scratch and the plan already says not to `git add` them; the Sonnet false alarms above (verified non-issues).
