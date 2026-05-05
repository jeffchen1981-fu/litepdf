# Phase 9: Icons — Design Spec

**Status:** Approved 2026-05-05. Supersedes §9 of [`docs/plans/2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md) (the original §9 was a roadmap-level sketch; this spec is the implementation source of truth).

**Goal:** Ship the LitePDF application icon (`IDI_APPICON`) and PDF document file-icon variant (`IDI_PDFDOC`) embedded in `litepdf.exe` at all required Windows sizes, with a regenerate-on-demand asset pipeline that does not introduce build-time tool dependencies.

**Phase placement:** Phase 9 of the v1.0 roadmap. Prerequisite: Phase 8.5 (Print support) merged. Successor: Phase 10 (Installer) — which consumes `IDI_PDFDOC` via the `DefaultIcon` registry key for `.pdf` association.

**LOC budget:** ~85 (resource embedding + regen .ps1 + regen .py helper + requirements.txt + README; SVG/PNG/ICO are binary assets, not code). Leaves ~15 LOC headroom under the roadmap's 100-LOC budget. Breakdown: `litepdf.rc` +2, `MainMenu.rc.h` +5, `MainWindow.cpp` +2, `CMakeLists.txt` +1, `regenerate.ps1` ~15, `regenerate.py` ~30, `requirements.txt` 2, `README.md` ~30.

---

## 1. Visual Design

### 1.1 Application Icon — "Lightning Document"

A white sheet of paper with a folded top-right corner, overlaid by a blue lightning bolt running diagonally across the sheet.

| Element | Color | Notes |
|---|---|---|
| Paper fill | `#F8F9FA` | Light gray-white |
| Fold shadow | `#DEE2E6` | One shade darker than paper |
| Lightning bolt | `#0B5ED7` | **Bootstrap `btn-primary:hover` shade** — chosen over `#0D6EFD` for ≥ 4.5 contrast on Win11 light taskbars (see §6 Color Decision Log) |
| Background | transparent | RGBA, alpha = 0 outside paper silhouette |
| Meaning | lightning = fast/lightweight; paper = document |

### 1.2 PDF Document File Icon — `IDI_PDFDOC`

Same paper silhouette and fold, with bold red "PDF" wordmark (centered) in place of the lightning bolt.

| Element | Color | Notes |
|---|---|---|
| Paper fill | `#F8F9FA` | Identical to app icon |
| Fold shadow | `#DEE2E6` | Identical |
| "PDF" text | `#D32F2F` | Bold sans-serif, center-aligned |
| Background | transparent | |

Distinct from the app icon so users distinguish "the LitePDF app" (lightning) from "a PDF document associated with LitePDF" (PDF wordmark).

### 1.3 Required Sizes

Each `.ico` bundles seven rasterizations:

| Size | Use case |
|---|---|
| 16×16 | Toolbar, tab strip, small Explorer detail view |
| 20×20 | Win11 small taskbar |
| 24×24 | Explorer details view (mid DPI) |
| 32×32 | Alt+Tab, default file icon |
| 48×48 | Desktop, Start menu tile |
| 64×64 | High-DPI file view |
| 256×256 | Large icon view, installer welcome screen |

### 1.4 Small-size Verification (16/20/24)

The original §9.3 promised "hand-tuned 3-pixel polyline" pixel art at 16×16. **This spec drops that promise** in favor of an auto-downscale pipeline plus an explicit acceptance criterion:

> **Acceptance:** at 100% Explorer detail view, the 16×16 rasterization must remain recognizable as "paper + lightning" (or "paper + PDF" for the doc variant).

If auto-downscale fails this criterion, the implementer authors a separate `litepdf-app-16.svg` master tuned for small sizes (thicker strokes, simplified geometry) used for sizes ≤ 24. This fallback is allowed but not assumed; the default is single-master auto-downscale.

---

## 2. Asset Pipeline

### 2.1 Pipeline Principle: regenerate-on-demand, not build-time

Icons change rarely (target: 0–1 times per year). Coupling icon regeneration to every CMake build forces every contributor and CI runner to install rasterization tools. **This spec commits the final `.ico` binaries to the repository and treats regeneration as an explicit out-of-band step.**

Consequences:
- CMake has no rasterization dependency. The build does `IDI_APPICON ICON "assets/icon/litepdf-app.ico"` (resolved against the source-root resource-compiler include path; see §3.1) and is done.
- CI runners need no extra packages.
- A contributor updating the visual design runs one PowerShell script locally; the resulting `.ico` files land in the same commit as the `.svg` source change.

### 2.2 Tooling

Regeneration script: `assets/icon/regenerate.ps1` (PowerShell, cross-platform via PowerShell 7+ but primary target is Windows).

Tool dependencies (only required for regeneration, never for normal build):

| Tool | Source | Why |
|---|---|---|
| Python 3.10+ | already installed | Script orchestration |
| `cairosvg` | `pip install cairosvg` | SVG → PNG rasterization (LGPL, pure-Python wrapper around Cairo) |
| `Pillow` | `pip install Pillow` | PNG → multi-resolution `.ico` packing |

Rationale for Python over the original `rsvg-convert + ImageMagick` choice:
- Python 3.14 already on the dev machine; `rsvg-convert` and `magick` are not.
- `cairosvg` + `Pillow` install via single `pip install` line (`requirements.txt` in `assets/icon/`).
- Cross-platform without GTK runtime headaches that bedevil librsvg on Windows.

### 2.3 Regeneration flow

```
assets/icon/litepdf-app.svg   (256-px master, hand-authored)
     │
     ├──cairosvg──▶ app-16.png, app-20.png, app-24.png, app-32.png,
     │             app-48.png, app-64.png, app-256.png
     │
     └──Pillow.save(format='ICO')──▶ litepdf-app.ico  (multi-resolution)

(same flow for litepdf-doc.svg → litepdf-doc.ico)
```

Pseudocode for `regenerate.ps1`:

```powershell
$sizes = @(16, 20, 24, 32, 48, 64, 256)
foreach ($variant in 'app','doc') {
    $svg = "assets/icon/litepdf-$variant.svg"
    $pngs = $sizes | ForEach-Object {
        $out = "assets/icon/$variant-$_.png"
        python -c "import cairosvg; cairosvg.svg2png(url='$svg', output_width=$_, output_height=$_, write_to='$out')"
        $out
    }
    python -c "from PIL import Image; ims=[Image.open(p) for p in @($($pngs -join ','))]; ims[0].save('assets/icon/litepdf-$variant.ico', sizes=[(s,s) for s in @($($sizes -join ','))])"
}
```

(The actual script will use a Python helper file rather than inline `-c` heredocs to keep escaping sane; the pseudocode above shows the dataflow.)

### 2.4 File layout

```
assets/icon/
├─ litepdf-app.svg         # 256-px master, hand-authored
├─ litepdf-doc.svg         # document variant
├─ app-16.png              # rasterized intermediates (committed for diff visibility)
├─ app-20.png
├─ app-24.png
├─ app-32.png
├─ app-48.png
├─ app-64.png
├─ app-256.png
├─ doc-{16,20,24,32,48,64,256}.png
├─ litepdf-app.ico         # bundled ICO (consumed by .rc)
├─ litepdf-doc.ico         # bundled ICO (consumed by .rc + Phase 10 installer)
├─ regenerate.ps1          # PowerShell driver
├─ regenerate.py           # Python helper invoked by .ps1
├─ requirements.txt        # cairosvg + Pillow pins
└─ README.md               # regen instructions + design rationale pointers
```

PNG intermediates are committed (not just the ICO) so visual diffs in PRs are trivially viewable on GitHub. Total binary size estimate: ~25 KB across both variants — negligible for the repo.

---

## 3. Source Code Integration

### 3.1 Resource declarations

`resources/litepdf.rc` (existing file) already reserves placeholder slots for these icons (commented out, pre-reserved at IDs 101/102 since Phase 0 bootstrap). Phase 9 uncomments them and corrects the path:

```rc
IDI_APPICON ICON "assets/icon/litepdf-app.ico"
IDI_PDFDOC  ICON "assets/icon/litepdf-doc.ico"
```

The path `assets/icon/...` resolves relative to the project source root, which requires adding the source root to the resource-compiler include directories in `CMakeLists.txt`:

```cmake
target_include_directories(litepdf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "${CMAKE_CURRENT_SOURCE_DIR}"        # for .rc paths like "assets/icon/..."
)
```

Resource IDs declared in `resources/MainMenu.rc.h` (existing file) — appended in a new section. Honors the pre-reservation in litepdf.rc to avoid renumbering risk:

```c
// Phase 9: app and document icon resource IDs.
// Numeric IDs match the reservation in litepdf.rc (since Phase 0 bootstrap).
#define IDI_APPICON 101
#define IDI_PDFDOC  102
```

Numeric IDs are in the 100 range (icon resources). The IDM_ menu-command IDs in the same header live in the 40000+ range, so the two namespaces do not collide.

### 3.2 Window class registration

`src/ui/MainWindow.cpp` `WNDCLASSEXW` registration sets both `hIcon` and `hIconSm`:

```cpp
WNDCLASSEXW wc = {sizeof(wc)};
// ...
wc.hIcon   = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
wc.hIconSm = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
```

`LoadIconW` lets Windows pick the appropriate size from the multi-resolution ICO automatically — no `LR_DEFAULTSIZE` hack needed.

### 3.3 What `IDI_PDFDOC` powers

In Phase 9: declared and embedded only — not loaded by any runtime code. Phase 10 (Installer) consumes it via:

```reg
HKEY_CLASSES_ROOT\.pdf\OpenWithProgids\LitePDF.Document = ""
HKEY_CLASSES_ROOT\LitePDF.Document\DefaultIcon = "<install-dir>\litepdf.exe,-102"
```

(The `-102` references resource ID `IDI_PDFDOC = 102`.) Phase 10 owns that wiring; Phase 9 only ensures the resource is present.

---

## 4. Build & Verification

### 4.1 Build flow (one CMake change)

CMake's existing `add_executable(litepdf WIN32 … resources/litepdf.rc)` already routes the `.rc` through `rc.exe`. Adding two `ICON` entries requires one CMake change: extending the resource-compiler include path so the `.rc` can reference assets via `"assets/icon/<file>.ico"`. See §3.1 for the `target_include_directories` snippet.

### 4.2 Verification (added to manual smoke checklist)

Phase 9 adds these manual smoke items (executed once before tagging):

1. Built `litepdf.exe` shows the lightning icon in **Alt+Tab** (32×32).
2. `litepdf.exe` in **File Explorer details view** (24×24) is recognizable as paper + lightning.
3. `litepdf.exe` in **Explorer's "extra large icons" view** (256×256) shows full detail.
4. Pinning to **taskbar** shows correct icon at the user's DPI.
5. **Window title bar** of an open LitePDF window shows the icon (small, top-left).
6. After running `regenerate.ps1` from a clean check-out, the produced `.ico` byte-matches the committed one (or differs only because the `.svg` was edited).

No automated tests for icons — pixel comparisons across DPIs are flaky and not worth the harness cost.

---

## 5. Failure Modes

| Failure | Cause | Recovery |
|---|---|---|
| 16-px rasterization unreadable | Lightning bolt strokes too thin at 16-px after auto-downscale | Author `litepdf-app-16.svg` with thicker strokes; route sizes ≤ 24 to small-master in `regenerate.ps1`. (Fallback path in §1.4.) |
| `LoadIconW` returns NULL at runtime | Wrong resource ID, rc compilation skipped icon, or `.ico` file missing at build | Validate `MAKEINTRESOURCE` ID matches `IconIds.h`; check `.rc` compiled without warnings; verify `.ico` exists at the path in `.rc`. |
| Win11 dark taskbar icon looks washed out | `#0B5ED7` luminance still leaves contrast ~3.0 on dark bg (passes large-icon AAA but not text) | Acceptable for v1; if reported, evaluate dual-tone (light/dark variant) for v1.1. |
| Contributor edits SVG but forgets to regenerate `.ico` | Manual step is easy to miss | `regenerate.ps1` is run as a CI lint check (Phase 12 hardening adds it); for now, PR reviewer enforces. |
| `cairosvg` install fails on Windows | Missing Cairo DLLs | `requirements.txt` pins `cairosvg>=2.7` which bundles `cairocffi` with Windows wheels; fallback documented in `README.md` is `pip install pyinkscape` or manual Inkscape CLI. |

---

## 6. Color Decision Log

The original §9.1 used `#0D6EFD` (Bootstrap 5 primary). Switched to `#0B5ED7` (Bootstrap `btn-primary:hover`) on 2026-05-05 after contrast audit:

| Background | `#0D6EFD` | `#0B5ED7` |
|---|---|---|
| Win11 light taskbar (~`#F3F3F3`) | ~4.1 (≈ AA fail for text) | ~5.2 (AA pass) |
| Win11 dark taskbar (~`#202020`) | ~3.6 (large-icon AAA) | ~3.0 (large-icon AAA boundary) |

The shift trades a marginal dark-bg loss for a meaningful light-bg gain. Win11 default theme on consumer machines is light; the gain matches the more common scenario. Both colors are within the same Bootstrap blue family (1–3 hue/saturation steps apart), so the visual identity is preserved.

PDF wordmark color `#D32F2F` is unchanged.

---

## 7. Out of Scope (Phase 9)

Deferred deliberately:
- **PDF thumbnail provider** (`IThumbnailProvider` shell extension DLL) — design.md §10 already lists this as v1 out-of-scope; Phase 9 ICO is the fallback Explorer uses when no thumbnail provider is registered.
- **PDF preview handler** (`IPreviewHandler`) — same.
- **Light/dark taskbar dual-tone variants** — single-variant icons for v1; revisit in v1.1 if user feedback shows dark-bg legibility issues.
- **Animated icon** for splash / loading — no splash in this app.
- **High-contrast mode (HCM) variant** — Win11's HCM auto-inverts; manual HCM-tuned ICO is v1.1 territory.
- **`DefaultIcon` registry wiring** — owned by Phase 10 (Installer).

---

## 8. Open Items

None at spec time. Implementation plan will resolve:
- Exact PowerShell heredoc strategy in `regenerate.ps1` (inline `python -c` vs. helper `.py` file)

---

## 9. References

- [Original §9 design sketch](../../plans/2026-04-15-litepdf-design.md) (superseded by this spec)
- [Roadmap entry — Phase 9](../../plans/2026-04-15-litepdf-roadmap.md) — `Icons | "Lightning document" app icon + red PDF document variant; 7 sizes each; multi-res .ico`
- [Phase 8.5 print support spec](2026-05-03-print-support-design.md) — pattern reference for "spec at `docs/superpowers/specs/`, plan at `docs/superpowers/plans/`"
- WCAG 2.1 contrast formula: relative luminance per [w3.org/TR/WCAG21/#dfn-relative-luminance](https://www.w3.org/TR/WCAG21/#dfn-relative-luminance)
