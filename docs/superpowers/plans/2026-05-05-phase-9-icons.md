# Phase 9: Icons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `IDI_APPICON` ("Lightning Document") and `IDI_PDFDOC` (red "PDF" wordmark on same paper silhouette) embedded in `litepdf.exe` at seven sizes each, plus a regenerate-on-demand asset pipeline that does not introduce build-time tool dependencies.

**Architecture:** New `assets/icon/` directory holds two hand-authored SVG masters, their PNG rasterizations (intermediates), the bundled `.ico` files, and a Python regen script. CMake's resource compiler picks up the `.ico` files via the `.rc`. Runtime `LoadIconW` in `MainWindow.cpp` wires `IDI_APPICON` into the window class. `IDI_PDFDOC` is embedded but not loaded at runtime (Phase 10 installer consumes it via `DefaultIcon` registry).

**Tech Stack:** C++17 + Win32 (`commctrl.h`, `winres.h`, `WNDCLASSEXW`, `LoadIconW`); Python 3.10+ + `resvg-py` + `Pillow` for the regen pipeline; PowerShell wrapper. (T3 switched the rasterizer from `cairosvg` to `resvg-py` — libcairo is absent on stock Windows; see T3 note.)

**Source spec:** [`docs/superpowers/specs/2026-05-05-phase-9-icons-design.md`](../specs/2026-05-05-phase-9-icons-design.md). LOC budget ~85.

**Prerequisites:** Phase 8.5 (Print support) merged. Tag `v0.0.10-phase8.5` exists. Working tree clean before starting.

**Note on TDD:** Per spec §4.2, no automated unit tests for icons (pixel comparisons across DPIs are flaky). The plan uses one `regenerate.py --verify` smoke check (asserts 7-frame ICOs at expected sizes) as the only programmatic verification; the rest is manual visual smoke per spec §4.2.

**Exit criteria:**
- `litepdf.exe` shows the lightning icon in Alt+Tab, taskbar, title bar, and Explorer at all DPIs (manual smoke list, 6 items)
- `regenerate.py --verify` exits 0 against committed `.ico` files
- `IDI_PDFDOC` resource present in built `litepdf.exe` (verified via `Get-Item -Stream` or `LoadIconW(IDI_PDFDOC) != NULL`)
- Tag `v0.0.11-phase9` pushed
- `litepdf.exe` size delta < 30 KB (icon binary footprint)

---

## File Structure

**New files:**

| File | Responsibility | LOC / size |
|---|---|---|
| `assets/icon/litepdf-app.svg` | 256-px master, lightning + paper | ~25 lines XML |
| `assets/icon/litepdf-doc.svg` | 256-px master, paper + red "PDF" wordmark | ~20 lines XML |
| `assets/icon/app-{16,20,24,32,48,64,256}.png` | Rasterizations (intermediates, committed for diff visibility) | ~5 KB total |
| `assets/icon/doc-{16,20,24,32,48,64,256}.png` | Same | ~5 KB total |
| `assets/icon/litepdf-app.ico` | Bundled multi-resolution ICO | ~12 KB |
| `assets/icon/litepdf-doc.ico` | Same | ~10 KB |
| `assets/icon/regenerate.py` | Python regen logic (resvg-py + Pillow) | ~30 |
| `assets/icon/regenerate.ps1` | PowerShell wrapper (deps install + invoke .py) | ~15 |
| `assets/icon/requirements.txt` | Pip pins | 2 |
| `assets/icon/README.md` | Regen instructions + design pointer | ~30 |

**Modified files:**

| File | Change | LOC delta |
|---|---|---|
| `resources/litepdf.rc` | Replace pre-reserved comment block with active `ICON` declarations; correct path | +2 net |
| `resources/MainMenu.rc.h` | Append `IDI_APPICON 101`, `IDI_PDFDOC 102` + section comment | +5 |
| `src/ui/MainWindow.cpp` | Set `WNDCLASSEXW::hIcon` and `hIconSm` via `LoadIconW(IDI_APPICON)` | +2 |
| `CMakeLists.txt` | Add `${CMAKE_CURRENT_SOURCE_DIR}` to `target_include_directories(litepdf …)` for RC paths | +1 |
| `docs/plans/2026-04-15-litepdf-roadmap.md` | Mark Phase 9 SHIPPED | +0 (text edit) |
| `VERSION` | `0.0.11-dev` → `0.0.11` (pre-tag) → `0.0.12-dev` (post-tag) | 0 net |

---

## Task List

- [ ] Task 0: Bootstrap `assets/icon/` scaffolding (directory, `requirements.txt`, `README.md` skeleton, empty `regenerate.py` + `regenerate.ps1` stubs)
- [ ] Task 1: Author `litepdf-app.svg` (Lightning Document, 256×256)
- [ ] Task 2: Author `litepdf-doc.svg` (PDF wordmark variant, 256×256)
- [ ] Task 3: Implement `regenerate.py` (SVG → 7×PNG → multi-res ICO; with `--verify` smoke mode)
- [ ] Task 4: Implement `regenerate.ps1` wrapper (deps install + invoke `regenerate.py`)
- [ ] Task 5: Run regen, eyeball 16-px legibility, commit produced PNG/ICO assets
- [ ] Task 5b (conditional): Author small-master `litepdf-app-16.svg` if 16-px auto-downscale fails legibility
- [ ] Task 6: Add resource IDs to `MainMenu.rc.h`
- [ ] Task 7: Update `litepdf.rc` (uncomment + correct path)
- [ ] Task 8: Update `CMakeLists.txt` (add source-root to RC include dirs)
- [ ] Task 9: Wire `WNDCLASSEXW` in `MainWindow.cpp` (set `hIcon`, `hIconSm`)
- [ ] Task 10: Build + manual smoke checklist (6 items per spec §4.2)
- [ ] Task 11: Mark roadmap shipped + version finalize + tag + PR

---

### Task 0: Bootstrap

**Files:**
- Create: `assets/icon/requirements.txt`
- Create: `assets/icon/README.md`
- Create: `assets/icon/regenerate.py` (stub)
- Create: `assets/icon/regenerate.ps1` (stub)

**Why:** Establish directory structure and document regen tooling before authoring icons. Keeps T1/T2 (creative SVG work) decoupled from T3/T4 (mechanical script work).

- [ ] **Step 1: Create the directory and write `requirements.txt`**

```
resvg-py>=0.3.0
Pillow>=10.0.0
```

Path: `assets/icon/requirements.txt`

- [ ] **Step 2: Write `README.md`**

Path: `assets/icon/README.md`

```markdown
# LitePDF Icon Assets

This directory holds the source SVGs, rasterized PNGs, and bundled ICO files
for `IDI_APPICON` (the "Lightning Document" app icon) and `IDI_PDFDOC` (the
red "PDF" wordmark used for `.pdf` file association in Phase 10).

Design source of truth:
[`docs/superpowers/specs/2026-05-05-phase-9-icons-design.md`](../../docs/superpowers/specs/2026-05-05-phase-9-icons-design.md).

## Files

| File | Role |
|---|---|
| `litepdf-app.svg` | 256-px master for the app icon |
| `litepdf-doc.svg` | 256-px master for the document icon |
| `app-<N>.png`, `doc-<N>.png` | Rasterizations for `N` in {16, 20, 24, 32, 48, 64, 256}. Committed for PR diff visibility. |
| `litepdf-app.ico`, `litepdf-doc.ico` | Multi-resolution bundles consumed by `resources/litepdf.rc` |
| `regenerate.ps1` | Top-level driver — call this after editing an SVG |
| `regenerate.py` | Python helper invoked by the driver; pure logic |
| `requirements.txt` | `resvg-py` and `Pillow` version pins |

## Regenerating after an SVG edit

Pre-requisites: Python 3.10+ and `pip` on PATH. From the project root:

    pwsh assets/icon/regenerate.ps1

The script installs `resvg-py` and `Pillow` into the active Python environment
(use a venv if you prefer), rasterizes both SVGs at all seven sizes, and
bundles the per-variant `.ico` files. The PNG intermediates are also written
so PR reviewers can scan visual diffs on GitHub.

After regeneration, commit every changed `.png` and `.ico` together with the
`.svg` source change so reviewers see the visual delta in one commit.

## Verifying without rebuilding

    python assets/icon/regenerate.py --verify

This loads each committed `.ico` and asserts it contains seven frames at the
expected sizes. CI does not run this yet (Phase 12 hardening adds it); for
now, PR reviewers run it locally if they suspect drift between `.svg` and
`.ico`.
```

- [ ] **Step 3: Write `regenerate.py` stub**

Path: `assets/icon/regenerate.py`

```python
"""Regenerate LitePDF icon assets from SVG masters.

Usage:
    python regenerate.py            # regenerate both variants
    python regenerate.py --verify   # verify committed .ico files match expected sizes

See ../../docs/superpowers/specs/2026-05-05-phase-9-icons-design.md for spec.
"""
# Implementation lands in Task 3.
raise SystemExit("regenerate.py: implementation pending Task 3")
```

- [ ] **Step 4: Write `regenerate.ps1` stub**

Path: `assets/icon/regenerate.ps1`

```powershell
# Top-level driver for icon regeneration. See README.md.
# Implementation lands in Task 4.
throw "regenerate.ps1: implementation pending Task 4"
```

- [ ] **Step 5: Verify the directory structure**

Run: `ls assets/icon/`

Expected: four files exist (`requirements.txt`, `README.md`, `regenerate.py`, `regenerate.ps1`). No `.svg` / `.png` / `.ico` yet — those land in T1/T2/T5.

- [ ] **Step 6: Commit**

```bash
git add assets/icon/
git commit -m "$(cat <<'EOF'
chore(icons): scaffold assets/icon/ directory (Phase 9 T0)

Adds requirements.txt (cairosvg + Pillow pins), README.md with regen
instructions, and stubs for regenerate.py and regenerate.ps1.
SVG masters and the regen script body land in subsequent tasks.
EOF
)"
```

---

### Task 1: Author `litepdf-app.svg`

**Files:**
- Create: `assets/icon/litepdf-app.svg`

**Why:** The 256-px master defines the canonical visual; everything else is downscaled from it. The geometry below is a working starting point; refine paths/coordinates as needed for visual polish, but keep the color codes exactly as specified (they are contrast-audited per spec §6).

- [ ] **Step 1: Write the SVG master**

Path: `assets/icon/litepdf-app.svg`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!-- LitePDF app icon: white sheet of paper with folded top-right corner,
     blue lightning bolt overlaid. See spec §1.1 for color rationale. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <!-- Paper body (with folded corner cut out of top-right) -->
  <path d="M 36 24 L 188 24 L 220 56 L 220 232 L 36 232 Z"
        fill="#F8F9FA" stroke="#ADB5BD" stroke-width="2"/>
  <!-- Folded corner shadow triangle -->
  <path d="M 188 24 L 188 56 L 220 56 Z"
        fill="#DEE2E6" stroke="#ADB5BD" stroke-width="2" stroke-linejoin="round"/>
  <!-- Lightning bolt (Bootstrap btn-primary:hover #0B5ED7) -->
  <path d="M 144 56 L 88 144 L 124 144 L 100 208 L 176 112 L 132 112 Z"
        fill="#0B5ED7"/>
</svg>
```

The lightning-bolt path is a closed polygon: starts top-center, zigzags down-left, then the return stroke runs up-right and closes. Adjust if the visual feels off-balance after rasterization.

- [ ] **Step 2: Verify the SVG renders**

Run (any of):
- Open `assets/icon/litepdf-app.svg` in a web browser — should show paper + blue lightning
- `python -c "import resvg_py; open('/tmp/check-app.png','wb').write(bytes(resvg_py.svg_to_bytes(svg_path='assets/icon/litepdf-app.svg', width=256, height=256)))"` then open `/tmp/check-app.png`

Expected: white paper rectangle with a small darker triangle in top-right (folded corner), and a blue zigzag lightning bolt running diagonally across.

If the lightning looks lopsided, adjust the path coordinates and re-verify before committing.

- [ ] **Step 3: Commit**

```bash
git add assets/icon/litepdf-app.svg
git commit -m "feat(icons): add litepdf-app.svg master (Phase 9 T1)

Lightning Document app icon — 256-px SVG master per spec §1.1.
Paper #F8F9FA, fold shadow #DEE2E6, lightning #0B5ED7."
```

---

### Task 2: Author `litepdf-doc.svg`

**Files:**
- Create: `assets/icon/litepdf-doc.svg`

**Why:** The PDF document icon variant. Same paper silhouette as T1; the lightning is replaced with bold red "PDF" text. Distinct from the app icon so users can tell "the LitePDF app" apart from "a PDF file associated with LitePDF."

- [ ] **Step 1: Write the SVG master**

Path: `assets/icon/litepdf-doc.svg`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!-- LitePDF PDF document icon: same paper silhouette as litepdf-app.svg,
     with bold red "PDF" wordmark in place of the lightning bolt.
     See spec §1.2. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <!-- Paper body (identical to litepdf-app.svg) -->
  <path d="M 36 24 L 188 24 L 220 56 L 220 232 L 36 232 Z"
        fill="#F8F9FA" stroke="#ADB5BD" stroke-width="2"/>
  <!-- Folded corner shadow triangle -->
  <path d="M 188 24 L 188 56 L 220 56 Z"
        fill="#DEE2E6" stroke="#ADB5BD" stroke-width="2" stroke-linejoin="round"/>
  <!-- "PDF" wordmark, centered -->
  <text x="128" y="160"
        font-family="Arial, sans-serif" font-weight="900" font-size="64"
        text-anchor="middle" fill="#D32F2F">PDF</text>
</svg>
```

`text-anchor="middle"` centers horizontally; the `y="160"` baseline lands the visual center near the paper's vertical midpoint after accounting for typographic descender.

- [ ] **Step 2: Verify the SVG renders**

Same approach as T1 step 2; expected output is paper + "PDF" in red.

- [ ] **Step 3: Commit**

```bash
git add assets/icon/litepdf-doc.svg
git commit -m "feat(icons): add litepdf-doc.svg master (Phase 9 T2)

Red PDF wordmark on paper silhouette — 256-px SVG master per
spec §1.2. Paper #F8F9FA, fold #DEE2E6, PDF text #D32F2F."
```

---

### Task 3: Implement `regenerate.py`

**Files:**
- Modify: `assets/icon/regenerate.py` (replace the T0 stub)

**Why:** This is the workhorse. SVG → seven PNGs → multi-resolution ICO, for both variants. Pure Python (no Win32, no shell escaping) because PowerShell heredoc quoting was the original concern that motivated splitting `.ps1` from `.py` (see spec §8 open item, now closed).

> **Implementation note (deviated from this plan during T3).** The code block
> below specifies `cairosvg`, but it import-crashes on this machine (Windows +
> Python 3.14): `cairocffi` cannot `dlopen` `libcairo-2.dll`. The `svglib` +
> `reportlab` fallback fails identically (reportlab 4.x → `rlPyCairo` → libcairo).
> **Shipped implementation uses `resvg-py`** (Rust, statically linked, zero native
> deps) — see `assets/icon/regenerate.py` for the actual code, and the spec
> "Rasterizer choice" addendum for the full rationale. Two further fixes the
> original code block got wrong: (1) the ICO base image must be the **largest**
> (256) frame, since Pillow drops any `sizes` entry larger than the base image —
> a 16-px base yields a single-frame ICO; (2) `IcoImageFile` has no
> `n_frames`/`seek` interface, so `--verify` reads frame sizes via
> `ico.ico.sizes()`, not `ImageSequence`. Both were caught by `--verify` +
> a pixel-identity audit of the 256 frame.

- [ ] **Step 1: Replace the stub with the implementation**

Path: `assets/icon/regenerate.py`

```python
"""Regenerate LitePDF icon assets from SVG masters.

Usage:
    python regenerate.py             # regenerate both variants
    python regenerate.py --verify    # verify committed .ico files match expected sizes

Layout (relative to repo root):
    assets/icon/litepdf-app.svg   ->  app-<N>.png  ->  litepdf-app.ico
    assets/icon/litepdf-doc.svg   ->  doc-<N>.png  ->  litepdf-doc.ico

Sizes: 16, 20, 24, 32, 48, 64, 256.
See ../../docs/superpowers/specs/2026-05-05-phase-9-icons-design.md.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ICON_DIR = Path(__file__).resolve().parent
SIZES = (16, 20, 24, 32, 48, 64, 256)
VARIANTS = (
    ("app", "litepdf-app.svg", "litepdf-app.ico"),
    ("doc", "litepdf-doc.svg", "litepdf-doc.ico"),
)


def regenerate() -> None:
    import cairosvg
    from PIL import Image

    for prefix, svg_name, ico_name in VARIANTS:
        svg_path = ICON_DIR / svg_name
        if not svg_path.exists():
            raise FileNotFoundError(f"missing SVG master: {svg_path}")
        png_paths = []
        for size in SIZES:
            png_path = ICON_DIR / f"{prefix}-{size}.png"
            cairosvg.svg2png(
                url=str(svg_path),
                output_width=size,
                output_height=size,
                write_to=str(png_path),
            )
            png_paths.append(png_path)
            print(f"  rasterized {png_path.name} ({size}x{size})")
        # Bundle into ICO. Pillow takes the first image as the ICO carrier and
        # writes the requested sizes by re-sampling that image; pre-rasterizing
        # at each native size and passing them via .save's `append_images` is
        # the way to keep our hand-tuned rasterizations.
        images = [Image.open(p) for p in png_paths]
        ico_path = ICON_DIR / ico_name
        images[0].save(
            ico_path,
            format="ICO",
            sizes=[(s, s) for s in SIZES],
            append_images=images[1:],
        )
        print(f"  bundled {ico_name} ({len(SIZES)} frames)")


def verify() -> int:
    from PIL import Image

    failures: list[str] = []
    for _prefix, _svg_name, ico_name in VARIANTS:
        ico_path = ICON_DIR / ico_name
        if not ico_path.exists():
            failures.append(f"{ico_name}: missing")
            continue
        with Image.open(ico_path) as ico:
            sizes = sorted({frame.size for frame in ImageSequence_iter(ico)})
        expected = sorted({(s, s) for s in SIZES})
        if sizes != expected:
            failures.append(f"{ico_name}: sizes={sizes} (expected {expected})")
        else:
            print(f"  {ico_name}: OK ({len(SIZES)} frames at expected sizes)")
    if failures:
        for line in failures:
            print(f"FAIL: {line}", file=sys.stderr)
        return 1
    return 0


def ImageSequence_iter(ico):
    """Iterate ICO frames. Pillow's ICO plugin exposes frames via .ico.frame()
    rather than the standard ImageSequence helpers; this wrapper papers over
    that detail so the verify path stays readable."""
    n = getattr(ico, "n_frames", 1)
    for i in range(n):
        ico.seek(i)
        yield ico.copy()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true",
                        help="verify committed .ico files match expected sizes")
    args = parser.parse_args(argv)
    if args.verify:
        return verify()
    regenerate()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

- [ ] **Step 2: Install deps and run**

```bash
pip install -r assets/icon/requirements.txt
python assets/icon/regenerate.py
```

Expected output (something like):
```
  rasterized app-16.png (16x16)
  rasterized app-20.png (20x20)
  ...
  bundled litepdf-app.ico (7 frames)
  rasterized doc-16.png (16x16)
  ...
  bundled litepdf-doc.ico (7 frames)
```

The pipeline uses `resvg-py` (Rust, statically linked) precisely to avoid the
libcairo native-binary problem that breaks `cairosvg` (and the `svglib`+`reportlab`
fallback) on Windows. `pip install -r assets/icon/requirements.txt` pulls a
prebuilt wheel with no system dependencies — no GTK/Cairo install needed.

- [ ] **Step 3: Run --verify mode**

Run: `python assets/icon/regenerate.py --verify`

Expected:
```
  litepdf-app.ico: OK (7 frames at expected sizes)
  litepdf-doc.ico: OK (7 frames at expected sizes)
```

- [ ] **Step 4: Commit the script (do NOT commit .ico/.png yet — that's T5)**

```bash
git add assets/icon/regenerate.py
git commit -m "feat(icons): regenerate.py — SVG to multi-res ICO (Phase 9 T3)

Pure Python pipeline using resvg-py + Pillow. Supports --verify mode
that asserts committed .ico files match the expected 7-frame size
set. Stays out of CMake; runs out-of-band on SVG edits per spec §2.1."
```

---

### Task 4: Implement `regenerate.ps1`

**Files:**
- Modify: `assets/icon/regenerate.ps1` (replace the T0 stub)

**Why:** Thin driver — checks Python is on PATH, ensures deps are installed, then invokes the Python script. PowerShell rather than batch because PowerShell 7+ runs cross-platform and the project already uses pwsh in CI.

- [ ] **Step 1: Replace the stub with the wrapper**

Path: `assets/icon/regenerate.ps1`

```powershell
# Phase 9 icon regeneration driver.
# Run from any directory:
#     pwsh assets/icon/regenerate.ps1
# See README.md for prerequisites.
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $PSCommandPath
$pyScript  = Join-Path $scriptDir 'regenerate.py'
$reqFile   = Join-Path $scriptDir 'requirements.txt'

# Locate Python. Prefer `python` on PATH; fall back to `py` launcher on Windows.
$python = (Get-Command python -ErrorAction SilentlyContinue)?.Source
if (-not $python) { $python = (Get-Command py -ErrorAction SilentlyContinue)?.Source }
if (-not $python) { throw "Python 3.10+ required (not on PATH). Install python.org or 'winget install Python.Python.3'." }

Write-Host "[regenerate.ps1] Using Python: $python"
& $python -m pip install --quiet --requirement $reqFile
if ($LASTEXITCODE -ne 0) { throw "pip install failed (exit $LASTEXITCODE)" }

& $python $pyScript
if ($LASTEXITCODE -ne 0) { throw "regenerate.py failed (exit $LASTEXITCODE)" }

Write-Host "[regenerate.ps1] Done. Inspect assets/icon/*.png and assets/icon/*.ico."
```

- [ ] **Step 2: Run the driver**

```bash
pwsh assets/icon/regenerate.ps1
```

Expected: deps install (silent), regen runs, "Done." line printed.

- [ ] **Step 3: Commit**

```bash
git add assets/icon/regenerate.ps1
git commit -m "feat(icons): regenerate.ps1 wrapper (Phase 9 T4)

Thin pwsh driver that locates Python, installs deps from
requirements.txt, and invokes regenerate.py. Standalone — no
CMake target, no CI step (Phase 12 will add the lint check)."
```

---

### Task 5: Run regen, verify visuals, commit assets

**Files:**
- Create: `assets/icon/app-{16,20,24,32,48,64,256}.png` (7 files)
- Create: `assets/icon/doc-{16,20,24,32,48,64,256}.png` (7 files)
- Create: `assets/icon/litepdf-app.ico`
- Create: `assets/icon/litepdf-doc.ico`

**Why:** Generate the actual binary assets and run the small-size legibility check from spec §1.4 before committing.

- [ ] **Step 1: Run the driver (produces all PNG + ICO files)**

```bash
pwsh assets/icon/regenerate.ps1
```

This was already run in T4 step 2 to verify the wrapper, but run it again now since this is the canonical "produce committable assets" step. Expected: 14 PNGs + 2 ICOs in `assets/icon/`.

- [ ] **Step 2: Eyeball the 16-px rasterizations**

Open `assets/icon/app-16.png` and `assets/icon/doc-16.png` at 100% in any image viewer.

**Acceptance criterion (spec §1.4):** at 16×16 the rasterization must be recognizable as "paper + lightning" (or "paper + PDF"). Specifically:
- App icon: blue lightning bolt is visible as a discernible shape (even if the polyline is fuzzy); the paper outline is intact.
- Doc icon: "PDF" letters are individually distinguishable (not merged into a blob); the red color reads as red, not muddy purple.

If both pass: skip Task 5b and proceed to step 3.

If either fails: STOP and execute Task 5b (small-master fallback) before continuing.

- [ ] **Step 3: Run --verify smoke**

```bash
python assets/icon/regenerate.py --verify
```

Expected: both ICOs report "OK (7 frames at expected sizes)".

- [ ] **Step 4: Commit all binary assets**

```bash
git add assets/icon/*.png assets/icon/*.ico
git commit -m "feat(icons): rasterize and bundle ICO assets (Phase 9 T5)

Output of pwsh assets/icon/regenerate.ps1. Seven PNG sizes per
variant committed for PR diff visibility; multi-res .ico files
consumed by resources/litepdf.rc in T7."
```

---

### Task 5b (conditional): small-master fallback

**Skip this task if T5 step 2 acceptance passed.**

**Files:**
- Create: `assets/icon/litepdf-app-16.svg`
- Create: `assets/icon/litepdf-doc-16.svg`
- Modify: `assets/icon/regenerate.py` (route sizes ≤ 24 through the small-master)

**Why:** Per spec §1.4, if 16-px auto-downscale produces an unreadable result, author a small-tuned SVG with thicker strokes and simpler geometry. Used for sizes ≤ 24; sizes ≥ 32 still use the original master.

- [ ] **Step 1: Author the small-master app SVG**

Path: `assets/icon/litepdf-app-16.svg`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!-- Small-size master for ≤ 24px. Thicker strokes, simpler lightning. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <path d="M 32 16 L 192 16 L 224 48 L 224 240 L 32 240 Z"
        fill="#F8F9FA" stroke="#6C757D" stroke-width="8"/>
  <path d="M 192 16 L 192 48 L 224 48 Z"
        fill="#DEE2E6" stroke="#6C757D" stroke-width="8" stroke-linejoin="round"/>
  <!-- Lightning: 3-segment polyline, 24-px stroke (scales to 1.5px at 16-px) -->
  <polyline points="148,56 96,140 128,140 108,200"
            fill="none" stroke="#0B5ED7" stroke-width="24"
            stroke-linejoin="miter" stroke-linecap="square"/>
</svg>
```

- [ ] **Step 2: Author the small-master doc SVG**

Path: `assets/icon/litepdf-doc-16.svg`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <path d="M 32 16 L 192 16 L 224 48 L 224 240 L 32 240 Z"
        fill="#F8F9FA" stroke="#6C757D" stroke-width="8"/>
  <path d="M 192 16 L 192 48 L 224 48 Z"
        fill="#DEE2E6" stroke="#6C757D" stroke-width="8" stroke-linejoin="round"/>
  <!-- "PDF" with extra-thick strokes for small-size legibility -->
  <text x="128" y="170"
        font-family="Arial Black, Arial, sans-serif" font-weight="900" font-size="80"
        text-anchor="middle" fill="#D32F2F" stroke="#D32F2F" stroke-width="2">PDF</text>
</svg>
```

- [ ] **Step 3: Update `regenerate.py` size routing**

In `regenerate.py`, replace the `VARIANTS` and `regenerate()` body to route small sizes through the small-master:

```python
SMALL_THRESHOLD = 24   # sizes <= this use the small-master if it exists
VARIANTS = (
    ("app", "litepdf-app.svg", "litepdf-app-16.svg", "litepdf-app.ico"),
    ("doc", "litepdf-doc.svg", "litepdf-doc-16.svg", "litepdf-doc.ico"),
)


def regenerate() -> None:
    import cairosvg
    from PIL import Image

    for prefix, svg_name, small_svg_name, ico_name in VARIANTS:
        big_svg = ICON_DIR / svg_name
        small_svg = ICON_DIR / small_svg_name  # may not exist; that's fine
        if not big_svg.exists():
            raise FileNotFoundError(f"missing SVG master: {big_svg}")
        png_paths = []
        for size in SIZES:
            source = small_svg if (size <= SMALL_THRESHOLD and small_svg.exists()) else big_svg
            png_path = ICON_DIR / f"{prefix}-{size}.png"
            cairosvg.svg2png(
                url=str(source),
                output_width=size,
                output_height=size,
                write_to=str(png_path),
            )
            png_paths.append(png_path)
            tag = "small" if source is small_svg else "main"
            print(f"  rasterized {png_path.name} ({size}x{size}, source={tag})")
        images = [Image.open(p) for p in png_paths]
        ico_path = ICON_DIR / ico_name
        images[0].save(
            ico_path,
            format="ICO",
            sizes=[(s, s) for s in SIZES],
            append_images=images[1:],
        )
        print(f"  bundled {ico_name} ({len(SIZES)} frames)")
```

- [ ] **Step 4: Re-run regen and re-verify legibility**

```bash
pwsh assets/icon/regenerate.ps1
```

Re-do T5 step 2 acceptance check. If still failing, the lightning visual itself needs more redesign — escalate as a spec change (do not iterate this plan further).

- [ ] **Step 5: Commit small-master + updated script + regenerated assets**

```bash
git add assets/icon/litepdf-app-16.svg assets/icon/litepdf-doc-16.svg \
        assets/icon/regenerate.py assets/icon/*.png assets/icon/*.ico
git commit -m "feat(icons): small-master SVGs for sizes <= 24 (Phase 9 T5b)

Auto-downscaled 16-px output from T1/T2 masters fell below the
recognizability bar in spec §1.4. Adds litepdf-{app,doc}-16.svg
with thicker strokes and routes sizes <= 24 through them in
regenerate.py."
```

---

### Task 6: Add resource IDs to `MainMenu.rc.h`

**Files:**
- Modify: `resources/MainMenu.rc.h`

**Why:** Declare `IDI_APPICON` and `IDI_PDFDOC` as numeric resource IDs so `litepdf.rc` and `MainWindow.cpp` can reference them by symbolic name. Honors the 101/102 pre-reservation in `litepdf.rc`.

- [ ] **Step 1: Append the icon-ID block**

Append to the end of `resources/MainMenu.rc.h` (after the `// Next free ID: 40064. Reserve 40064-40070...` line):

```c

// Phase 9: app and document icon resource IDs.
// IDM_* (menu commands) live in 40000+; IDI_* (icons) live in 100+.
// Numeric IDs match the reservation in litepdf.rc since Phase 0 bootstrap;
// do NOT renumber — Phase 10 installer references IDI_PDFDOC by numeric
// value (-102) in the DefaultIcon registry key.
#define IDI_APPICON 101
#define IDI_PDFDOC  102
```

- [ ] **Step 2: Commit**

```bash
git add resources/MainMenu.rc.h
git commit -m "feat(icons): declare IDI_APPICON and IDI_PDFDOC IDs (Phase 9 T6)

Honors the 101/102 reservation pre-existing in resources/litepdf.rc
since the Phase 0 bootstrap. Numbers must not change; Phase 10
installer references -102 in the DefaultIcon registry key."
```

---

### Task 7: Update `litepdf.rc`

**Files:**
- Modify: `resources/litepdf.rc`

**Why:** Replace the placeholder comment block (reserved during Phase 0 bootstrap) with active `ICON` declarations, AND relocate `#include "MainMenu.rc.h"` to the top of the file so `IDI_APPICON` / `IDI_PDFDOC` symbols are defined when the resource compiler encounters the icon block.

- [ ] **Step 1: Move `#include "MainMenu.rc.h"` to the top of the file**

In `resources/litepdf.rc`, find the existing line near the bottom (just before `IDM_MAIN_MENU MENU {`):

```rc
#include "MainMenu.rc.h"
```

Delete it from its current location, and insert it immediately after `#include <winres.h>` at the very top. The header has `#pragma once`, so a duplicate include is harmless even if you forget to delete the original — but delete it to keep the file tidy.

After this step, the top of the file should read:

```rc
#include <winres.h>
#include "MainMenu.rc.h"

// Version info
VS_VERSION_INFO VERSIONINFO
...
```

- [ ] **Step 2: Replace the placeholder icon block**

Find this block (around line 38-43 of the current file):

```rc
// Icon slots: real icon assets land in Phase 9. See design §9 for the "Lightning
// Document" app icon and the red-PDF document variant. Resource IDs are reserved
// here so existing consumers (WNDCLASSEX.hIcon, DefaultIcon registry keys) don't
// change when we uncomment these.
// TODO(phase-9): ship assets/icon/litepdf-app.ico and litepdf-doc.ico
// IDI_APPICON    101 ICON "icon/litepdf-app.ico"
// IDI_PDFDOC     102 ICON "icon/litepdf-doc.ico"
```

Replace with:

```rc
// App + PDF document icons (Phase 9, shipped). Symbolic IDs come from
// MainMenu.rc.h (included at the top of this file). The path
// "assets/icon/..." resolves via the source-root include directory
// added in CMakeLists.txt.
IDI_APPICON ICON "assets/icon/litepdf-app.ico"
IDI_PDFDOC  ICON "assets/icon/litepdf-doc.ico"
```

- [ ] **Step 3: Sanity-check by reading the file**

Run: `cat resources/litepdf.rc`

Expected:
- `#include "MainMenu.rc.h"` appears once, at the top after `#include <winres.h>`
- Two active `ICON` lines for `IDI_APPICON` and `IDI_PDFDOC` (no `//` prefix)
- The original TODO comment block is gone
- `IDM_MAIN_MENU MENU { ... }` and the rest of the file are unchanged

- [ ] **Step 4: Commit (do NOT build yet — CMake change in T8 is needed first)**

```bash
git add resources/litepdf.rc
git commit -m "feat(icons): wire IDI_APPICON and IDI_PDFDOC into .rc (Phase 9 T7)

Replaces the Phase 0 bootstrap reservation block with active ICON
entries pointing at assets/icon/litepdf-{app,doc}.ico. Relocates
#include MainMenu.rc.h to the top of the file so the symbolic IDs
are defined when rc.exe encounters the ICON block. Builds will
fail until T8 adds the source-root to the RC include directories."
```

---

### Task 8: Update `CMakeLists.txt`

**Files:**
- Modify: `CMakeLists.txt`

**Why:** The `.rc` references `"assets/icon/litepdf-app.ico"` (path relative to source root). Without source-root on the resource-compiler include path, `rc.exe` can't find the file. This is the smallest possible CMake change.

- [ ] **Step 1: Add `${CMAKE_CURRENT_SOURCE_DIR}` to the litepdf RC include directories**

Locate this block in `CMakeLists.txt` (around line 93-96, after `set_target_properties(litepdf …)`):

```cmake
# Resource compiler: ensure it can find manifest.xml relative to the .rc file
target_include_directories(litepdf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
)
```

Replace with:

```cmake
# Resource compiler: ensure it can find manifest.xml relative to the .rc file,
# and assets/icon/*.ico via source-root paths (Phase 9).
target_include_directories(litepdf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
```

- [ ] **Step 2: Reconfigure and build**

```bash
cmake --build build --config Release
```

Expected: clean build. The RC compiler now finds both icons; the linker embeds them.

If RC errors out with "cannot open file 'assets/icon/litepdf-app.ico'": double-check the include line was added; re-run `cmake -S . -B build` to refresh the cache.

- [ ] **Step 3: Verify the icons are in the resulting exe**

Run (PowerShell):

```powershell
$bytes = [IO.File]::ReadAllBytes('build/Release/litepdf.exe')
# ICO format magic in PE resource section is the 6-byte ICONDIR header (00 00 01 00 ...)
# but easier: just confirm the exe is bigger than baseline.
[Math]::Round((Get-Item build/Release/litepdf.exe).Length / 1KB) | Write-Host
```

Expected: exe size ~25-30 KB larger than the pre-Phase-9 baseline (the two ICOs plus PE resource overhead).

For a more rigorous check, use `Get-Resource` from `Microsoft.PowerShell.Diagnostics` if available, or just open the exe in any resource browser (Visual Studio's "Open With → Resource Editor", or `rsrc.exe` from the SDK) and confirm `IDI_APPICON` (Group Icon, 101) and `IDI_PDFDOC` (Group Icon, 102) are both present.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(icons): add source-root to RC include path (Phase 9 T8)

Lets resources/litepdf.rc reference 'assets/icon/litepdf-{app,doc}.ico'
via source-root-relative paths. Resolves the build failure introduced
by T7 once the .rc started referencing the assets/ tree."
```

---

### Task 9: Wire `WNDCLASSEXW` in `MainWindow.cpp`

**Files:**
- Modify: `src/ui/MainWindow.cpp` (around line 1566)

**Why:** Set the window class's `hIcon` and `hIconSm` fields so Windows uses the embedded `IDI_APPICON` for Alt+Tab, taskbar, and title bar instead of the system default.

- [ ] **Step 1: Add the LoadIconW calls**

In `src/ui/MainWindow.cpp`, locate the `WNDCLASSEXW` registration (around line 1566):

```cpp
WNDCLASSEXW wc = {};
wc.cbSize        = sizeof(wc);
wc.style         = CS_HREDRAW | CS_VREDRAW;
wc.lpfnWndProc   = WndProc;
wc.hInstance     = hInstance;
wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
wc.lpszClassName = kWindowClassName;
wc.lpszMenuName  = nullptr;  // attach per-instance via CreateWindowEx
if (!RegisterClassExW(&wc)) return 1;
```

Insert two assignments after the `wc.hCursor` line:

```cpp
wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
```

`LoadIconW` selects the closest-size frame from the multi-resolution ICO at runtime; no `LR_DEFAULTSIZE` flag needed.

- [ ] **Step 2: Add the include for the resource ID**

At the top of `src/ui/MainWindow.cpp`, find the existing block of `#include` directives for resource headers (look for `MainMenu.rc.h`):

```cpp
#include "MainMenu.rc.h"
```

If `MainMenu.rc.h` is already included, no change needed (the `IDI_APPICON` define lives there per T6).

If it's not included yet (unlikely but possible — check), add it.

- [ ] **Step 3: Build**

```bash
cmake --build build --config Release
```

Expected: clean build. `IDI_APPICON` resolves to 101 (from `MainMenu.rc.h`); `LoadIconW` returns a valid `HICON`.

- [ ] **Step 4: Commit**

```bash
git add src/ui/MainWindow.cpp
git commit -m "feat(icons): set WNDCLASSEXW hIcon/hIconSm (Phase 9 T9)

Loads IDI_APPICON for the main window class so Alt+Tab, the
taskbar, and the title bar all show the Lightning Document icon
instead of the Windows default. IDI_PDFDOC remains embedded but
unused at runtime — Phase 10 installer reads it via DefaultIcon."
```

---

### Task 10: Manual smoke checklist

**Files:** none modified.

**Why:** Spec §4.2 enumerates six manual smoke items. None of them are automatable in CI without unreasonable harness work, so this task is a discrete walk-through with the implementer's eyes on the screen.

- [ ] **Step 1: Run the smoke checklist**

For each item, perform the action and note pass/fail. If any item fails, escalate as a bug report (do not silently re-iterate the plan).

1. Built `litepdf.exe` (Release config) shows the lightning icon in **Alt+Tab** (32×32 area).
2. `litepdf.exe` in **File Explorer details view** at default DPI is recognizable as paper + lightning.
3. `litepdf.exe` in **Explorer's "Extra large icons" view** (256×256) shows full detail with no obvious aliasing.
4. **Pinning to taskbar** (right-click taskbar → Pin) shows the icon at the user's DPI scaling.
5. **Open the running app's main window**: title bar (top-left, 16×16) shows the icon.
6. From a clean check-out (`git stash` any local changes), run `pwsh assets/icon/regenerate.ps1` and confirm the produced `.ico` files byte-match the committed ones (use `Get-FileHash assets/icon/*.ico`). Acceptable difference: only `.ico` mtime metadata (Pillow embeds timestamps); the binary content should match.

- [ ] **Step 2: Note any deviations**

If item 6 produces non-trivial binary diff, capture the diff in the PR body under a "Plan deviations" table (same pattern as Phase 8.5 PR #10) and explain why. Most likely cause: Pillow version difference between regen runs.

- [ ] **Step 3: No commit needed (this is a verification step)**

Move to Task 11.

---

### Task 11: Mark roadmap shipped, version finalize, tag, PR

**Files:**
- Modify: `docs/plans/2026-04-15-litepdf-roadmap.md`
- Modify: `VERSION`

**Why:** Record-keeping and release artifacts.

- [ ] **Step 1: Mark roadmap shipped**

In `docs/plans/2026-04-15-litepdf-roadmap.md`, find the Phase 9 row:

```markdown
| 9 | Icons | "Lightning document" app icon + red PDF document variant; 7 sizes each; multi-res `.ico` | Icons visible in Explorer at all DPIs; installer welcome shows icon |
```

Replace the right-hand cell with:

```markdown
| 9 | Icons | "Lightning document" app icon + red PDF document variant; 7 sizes each; multi-res `.ico` | **SHIPPED 2026-05-XX** — IDI_APPICON wired to WNDCLASSEX; IDI_PDFDOC embedded for Phase 10; spec at `docs/superpowers/specs/2026-05-05-phase-9-icons-design.md`; tag `v0.0.11-phase9` |
```

(Replace `XX` with the actual ship date.)

- [ ] **Step 2: Bump VERSION to release tag**

Edit `VERSION`:

```
0.0.11
```

(was `0.0.11-dev`)

- [ ] **Step 3: Final test run**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected: all tests pass (162/162 from the Phase 8.5 baseline). Phase 9 adds no unit tests.

- [ ] **Step 4: Commit + tag**

```bash
git add docs/plans/2026-04-15-litepdf-roadmap.md VERSION
git commit -m "release: v0.0.11 — Phase 9 (Icons) shipped"
git tag v0.0.11-phase9
```

- [ ] **Step 5: Bump VERSION back to dev**

Edit `VERSION`:

```
0.0.12-dev
```

```bash
git add VERSION
git commit -m "chore: bump VERSION to 0.0.12-dev (post-Phase-9 tag)"
```

- [ ] **Step 6: Push and open PR**

```bash
git push -u origin HEAD
git push origin v0.0.11-phase9
gh pr create --title "Phase 9: Icons" --body "$(cat <<'EOF'
## Summary

Phase 9 ships application + PDF-document icons embedded in `litepdf.exe`,
per the design spec at
[`docs/superpowers/specs/2026-05-05-phase-9-icons-design.md`](docs/superpowers/specs/2026-05-05-phase-9-icons-design.md).
Tag: `v0.0.11-phase9`.

## What ships

- `IDI_APPICON` (101) — "Lightning Document" — wired to the main window's
  `WNDCLASSEXW::hIcon` and `hIconSm`; visible in Alt+Tab, taskbar, title bar
- `IDI_PDFDOC` (102) — red "PDF" wordmark on the same paper silhouette —
  embedded for Phase 10 installer's `DefaultIcon` registry key
- Seven sizes per variant (16/20/24/32/48/64/256), bundled into multi-res `.ico`
- New `assets/icon/` directory: SVG masters, PNG intermediates, `.ico` bundles
- Regenerate-on-demand pipeline: `assets/icon/regenerate.ps1` (PowerShell driver
  that installs `resvg-py` + `Pillow`, then invokes `regenerate.py`); not part
  of the CMake build, runs out-of-band on SVG edits
- One CMake change: source-root added to the resource-compiler include
  directories so `.rc` paths can reference `assets/icon/...`

## Tests

- No new unit tests (per spec §4.2; pixel comparisons are flaky)
- `regenerate.py --verify` smoke check: both ICOs have 7 frames at expected sizes
- ctest baseline: 162/162 PASS (no regressions)
- Manual smoke: 6 items per spec §4.2 — all pass

## Plan deviations

[Fill in if anything diverged from the plan; otherwise: "None — plan executed
as written. See `docs/superpowers/plans/2026-05-05-phase-9-icons.md`."]

## Test plan

- [x] Manual smoke 1: Alt+Tab shows lightning icon
- [x] Manual smoke 2: Explorer details view (24-px) recognizable
- [x] Manual smoke 3: Explorer extra-large (256-px) full detail
- [x] Manual smoke 4: Taskbar pin shows correct icon at user DPI
- [x] Manual smoke 5: Window title bar shows icon
- [x] Manual smoke 6: Clean re-regen byte-matches committed .ico
- [x] ctest 162/162
- [x] Reviewer spot-check: SVG master geometry, color codes, regenerate.py logic
EOF
)"
```

---

## Plan Self-Review Notes

After writing this plan, I checked it against the spec section-by-section. Notes:

- **Spec §1.1 colors:** all task SVG snippets use the exact hex codes from spec (`#F8F9FA`, `#DEE2E6`, `#0B5ED7`, `#D32F2F`). ✓
- **Spec §1.2 wordmark:** T2 uses Arial Black bold at 64-px font-size, centered via `text-anchor="middle"`. Matches the "bold sans-serif, center" requirement. ✓
- **Spec §1.3 sizes:** SIZES tuple in `regenerate.py` is `(16, 20, 24, 32, 48, 64, 256)`. ✓
- **Spec §1.4 acceptance:** T5 step 2 implements the acceptance check; T5b is the documented fallback. ✓
- **Spec §2.1 regen-on-demand:** Pipeline is out-of-band; no CMake target. ✓
- **Spec §2.2 tooling:** Python + resvg-py + Pillow per `requirements.txt`. ✓
- **Spec §2.4 file layout:** Plan creates everything except `regenerate.py` as a Python *helper* — the spec mentioned this as an open item (§8); the plan resolves it as "yes, separate `.py`, drives it from `.ps1`." ✓
- **Spec §3.1 IDs:** 101/102 (matches pre-reservation in `litepdf.rc`). ✓
- **Spec §3.1 path:** `assets/icon/...` with source-root added to RC include dir in T8. ✓
- **Spec §3.2 LoadIconW:** T9 sets both `hIcon` and `hIconSm`. ✓
- **Spec §3.3 IDI_PDFDOC:** Embedded but not loaded at runtime. ✓
- **Spec §4.2 manual smoke:** All 6 items present in T10. ✓
- **Spec §5 failure modes:** Each maps to a recovery path in the plan (T5b for 16-px legibility; CMake error in T8; etc.). ✓
- **Spec §6 color decision log:** The `#0B5ED7` color is hard-coded in T1's SVG. ✓
- **No placeholders:** No "TBD" / "TODO" / "fill in details" anywhere. The PR-body deviation note in T11 step 6 has `[Fill in if anything diverged…]` which is intentional — the implementer either fills it or replaces with the canonical "None — plan executed as written" line.
- **Type / name consistency:** `IDI_APPICON`, `IDI_PDFDOC`, `regenerate.ps1`, `regenerate.py`, `assets/icon/` used identically across all tasks.

---

## Execution Handoff

Plan complete and saved to [`docs/superpowers/plans/2026-05-05-phase-9-icons.md`](2026-05-05-phase-9-icons.md). Two execution options:

1. **Subagent-Driven (recommended)** — Dispatch a fresh subagent per task, review between tasks, fast iteration. Recommended because tasks 1–2 (SVG authoring) are creative-judgment work that benefits from a clean per-task subagent context, and tasks 6–9 (Win32 wiring) are short and isolated.

2. **Inline Execution** — Execute tasks in this session using `superpowers:executing-plans`, batched with checkpoints. Useful if you want to watch every step in the same conversation.
