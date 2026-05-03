# LitePDF — Print Support (T2) Design

**Date**: 2026-05-03
**Status**: Approved 2026-05-03 — ready for implementation plan via `superpowers:writing-plans`
**Tier**: T2 (standard PrintDlg + page range + copies + scale modes + auto-rotate). T1 / T3 explicitly considered and rejected; see §1.
**Phase placement**: **Phase 8.5** (between Phase 8 Tier-3 completion and Phase 9 icons). See §8.

## 1. Goals & Constraints

LitePDF v0.0.8 is a read-only viewer with zero printing API integration (verified 2026-05-03 against `main`: no `PrintDlg` / `StartDoc` / `OpenPrinter` / `DocumentProperties` calls in the codebase; the only `print` symbol is `fz_print_stext_page_as_text` in `src/core/Document.cpp:308` for text extraction). This spec adds **Tier-2 print support** so users can send the active document to a Windows printer using the standard system Print dialog, with page-range and copy-count selection, three scale modes (fit / actual / custom %), and automatic landscape rotation.

**Tier rationale.** T1 (print-everything-default) is too thin — users will demand page ranges within days. T3 (n-up + duplex + custom preview pane) violates the SumatraPDF-inspired "lean viewer" philosophy in design doc §1, and duplex/n-up are notoriously driver-dependent. T2 leverages the OS dialog's built-in range/copies UI plus a thin pre-flight modal for scale, capturing 90% of user value at ~30% of T3's surface area.

### Quantitative Targets

| Metric | Target |
|---|---|
| LOC budget | ≤ 320 (printing module: PrintJob + PrintProgressDlg + PrintGeometry) + ≤ 25 (UI / accelerator wiring) = **~345 total** |
| Per-page render-and-emit, 600 DPI A4 | ≤ 800 ms on project's HDD baseline |
| Peak additional RAM during print job | ≤ 60 MB (one rendered page held at a time) |
| Cold abort (Esc during print) | ≤ 200 ms before printer driver acknowledges cancel |
| Exe size impact | < 5 KB (GDI lives in already-loaded `gdi32.dll`) |

### Non-goals

- OCR-quality printing of scanned image-only PDFs (no OCR in v1)
- Printing while a document is still loading (Print menu disabled until `Document::is_ready()`)
- Background printing on a worker thread (job runs on UI thread inside the modal progress dialog; cancel via `AbortDoc`)
- Cross-tab "print all open tabs" — one print job per active tab only

## 2. Tech Stack

| Layer | Choice | Rationale |
|---|---|---|
| Print dialog | Win32 `PrintDlgW` from `commdlg.h` | Standard, fully localized by Windows, zero new UI surface to maintain |
| Print flags | `PD_RETURNDC \| PD_PAGENUMS \| PD_NOSELECTION \| PD_USEDEVMODECOPIESANDCOLLATE` | User picks range/copies in OS dialog; we read from `PRINTDLG.lpPageRanges` and `DEVMODE.dmCopies` |
| Render path | MuPDF `fz_new_pixmap_from_page` → BGRA pixmap → `StretchDIBits` to printer DC | Format-agnostic (PDF / ePub / CBZ / XPS all work via existing `Document` allowlist), no new MuPDF device driver to write |
| DPI | `GetDeviceCaps(hdc, LOGPIXELSX/Y)` — render at printer's native logical DPI | Matches printer resolution; avoids cumulative scaling artifacts |
| Cancel | `SetAbortProc` callback returning FALSE when flag is set | Standard Win32 pattern, checked between pages |
| Threading | Synchronous on UI thread inside a modal progress dialog | Print is rare and user-initiated; matches the modal pattern used by Phase 8's `PasswordDialog`; avoids RenderEngine cache pollution |

## 3. Architecture

New module `src/printing/`:

| File | Purpose | LOC est. |
|---|---|---|
| `PrintJob.hpp` | Public API: `struct PrintJob { static bool run(HWND parent, Document& doc, int active_page); };` returns true on success, false on cancel/error | ~30 |
| `PrintJob.cpp` | Orchestrator: pre-flight scale modal → PrintDlg → page loop with cancel polling → cleanup | ~180 |
| `PrintProgressDlg.hpp/.cpp` | In-memory `DLGTEMPLATE` modal showing "Printing page X of Y" + Cancel button. Mirrors Phase 8 `PasswordDialog` (design D1) | ~80 |
| `PrintGeometry.hpp` | Pure-function header: `compute_render_matrix(page_rect, paper_rect, scale_mode, custom_pct, auto_rotate) → fz_matrix`. Extracted for unit testing. | ~30 (header-only) |

**Total**: ~320 LOC in `src/printing/` (30 + 180 + 80 + 30) + ~25 LOC in `src/ui/MainWindow.cpp` (menu item, `IDM_FILE_PRINT`, `Ctrl+P` accelerator, dispatch handler) = **~345 LOC**.

### Boundaries

- `PrintJob` knows only `Document&` and Win32. It does NOT touch `PageCache`, `RenderEngine`, `PdfCanvas`, or any view-layer state. Print intentionally bypasses both cache tiers — printer DPI (300-600) is 4-6× viewer DPI (96-144); a single rendered print page would evict the entire viewer cache.
- Each page render uses a **separately cloned `fz_context`** via `fz_clone_context()` per the project's `mupdf-refcount-conventions` skill. Cloned context is dropped at end of `run()`. Pixmaps are dropped immediately after each `StretchDIBits` returns (escrow lifetime: one page).
- `PrintJob` is stateless from the caller's perspective: stack-allocated within the menu handler, `run()` is the only entry point.

## 4. Data Flow

```
File → Print  (Ctrl+P, IDM_FILE_PRINT)
  ↓
MainWindow::on_print()
  ↓
PrintJob::run(hwnd, active_doc, active_page)
  ↓
[1] Pre-flight scale modal (PrintProgressDlg in "config" mode)
      → user picks Fit / Actual / Custom%; clicks OK or Cancel
  ↓ (cancel returns false)
[2] PrintDlgW(&pd) → user picks printer, page range, copies
  ↓ (cancel returns false; CommDlgExtendedError != 0 → MessageBox)
[3] StartDocW(hdc, &di)  — di.lpszDocName = source file basename
  ↓ (failure → MessageBox + cleanup)
[4] Switch PrintProgressDlg into "progress" mode (parent = main window)
  ↓
[5] for each (page in [first..last]) × dmCopies:
      [5a] PrintProgressDlg::set_progress(current, total)
      [5b] if abort_flag → break
      [5c] StartPage(hdc)
      [5d] matrix = PrintGeometry::compute_render_matrix(...)
      [5e] pix = fz_new_pixmap_from_page(ctx_clone, page, matrix, fz_device_bgr, alpha=0)
      [5f] StretchDIBits(hdc, ...)  // pixmap → printer
      [5g] fz_drop_pixmap(ctx_clone, pix)
      [5h] EndPage(hdc)  — driver may return SP_ERROR (see §6)
  ↓
[6] EndDoc(hdc)  — or AbortDoc if user cancelled
  ↓
[7] DeleteDC(hdc)  — PD_RETURNDC gave us ownership
  ↓
[8] PrintProgressDlg::close()
```

## 5. Scale & Rotation

### Coordinate-space contract

`PrintGeometry::compute_render_matrix` takes two rects with explicitly different units:

- **`page_rect_pt`** — page bounding box in **MuPDF points** (1 pt = 1/72 inch), as returned by `fz_bound_page`. Origin is (0, 0); width and height are `w_pt` and `h_pt`.
- **`paper_rect_px`** — printable area in **printer device pixels**, from `GetDeviceCaps(hdc, HORZRES/VERTRES)`. Origin is (0, 0).
- **`dpi_x`, `dpi_y`** — printer logical DPI from `GetDeviceCaps(hdc, LOGPIXELSX/Y)`.

The returned `PrintMatrix::scale_x` / `scale_y` is in units of **device pixels per point** and is fed directly into `fz_scale()` when building the MuPDF render matrix. Centering translations (`tx`, `ty`) are in **device pixels**.

> **Why separate names matter:** `Rect` is used both for page space (points) and paper space (pixels) in the implementation. Passing them in the wrong order silently produces a scale in px/pt vs. a dimensionless ratio — both are floats and the compiler cannot catch the mistake. The parameter names and this section serve as the load-bearing documentation. Reviewers: verify that callers (only `render_page_to_pixmap` in `PrintJob.cpp`) pass `fz_bound_page(…)` output as the first argument and `GetDeviceCaps(…HORZRES/VERTRES)` as the second.

For each page, before `StretchDIBits`:

1. **Printable area**: `paper_w_px = GetDeviceCaps(hdc, HORZRES)`, same for `VERT`. (Excludes printer hardware margins automatically.)
2. **Page size in points**: `fz_bound_page(ctx, page) → fz_rect`. MuPDF returns sizes in points (1/72 inch). These are `page_w_pt` and `page_h_pt`.
3. **Printer DPI**: `dpi_x = GetDeviceCaps(hdc, LOGPIXELSX)`, `dpi_y = GetDeviceCaps(hdc, LOGPIXELSY)`.
4. **Auto-rotate** (always on for T2; no user toggle): if page-orientation differs from paper-orientation (`(page_w_pt > page_h_pt) != (paper_w_px > paper_h_px)`), prepend `fz_pre_rotate(matrix, 90)`. After rotation, use effective dimensions `eff_w_pt` / `eff_h_pt` for scale computation.
5. **Scale mode** (from pre-flight modal). All scale factors below are in **px/pt** for direct use with `fz_scale()`:
   - **Fit to page** (default): divide paper pixels by page points directly across both axes and take the tighter bound:
     `s_fit = min( paper_w_px / eff_w_pt,  paper_h_px / eff_h_pt )`.
     Result unit: **px/pt** by direct dimensional reasoning — pixels of paper divided by points of page. (Why this is equivalent to the inches-based derivation: the dimensionless ratio `(paper_in / page_in) = (paper_px / dpi) / (page_pt / 72) = paper_px * 72 / (page_pt * dpi)`. Multiplying by the inch-to-px-per-pt conversion `dpi / 72` cancels the dpi factor: `[paper_px * 72 / (page_pt * dpi)] * (dpi / 72) = paper_px / page_pt`. So the px/pt scale equals the simpler `paper_px / page_pt` form regardless of dpi.) Example: A4 @ 300 DPI: `min(2480/595, 3508/842) ≈ 4.166 px/pt`. Applying `fz_scale(s_fit)` to an `eff_w_pt`-wide page produces `eff_w_pt × s_fit` output pixels, which ≤ `paper_w_px` by construction.
   - **Actual size**: one point maps to one physical 1/72-inch on paper; at printer DPI that is `dpi_x / 72` pixels per point:
     `s = dpi_x / 72.0`. Pages larger than the printable area are clipped; user is shown a one-time warning before printing if any page exceeds the paper size.
   - **Custom %**: percentage relative to Fit scale so that "50%" means "half the fit-to-page size":
     `s = (user_pct / 100.0) × s_fit`. Pre-flight modal validates 10 ≤ pct ≤ 400.
     _Rationale for "% of fit" over "% of actual": users think of scale relative to the page fitting the sheet (Acrobat / SumatraPDF precedent). "50%" of actual at 300 DPI would produce a tiny stamp (dpi/72/2 ≈ 2.08 px/pt × 595 pt ≈ 1239 px) identical to 50%-of-fit for standard paper/page size combinations, but the two diverge for non-standard DPIs or exotic page sizes. Fit is the user-visible mental model._
6. **Centering**: translate so the scaled page is centered within the printable area:
   `tx = (paper_w_px - eff_w_pt × s) / 2.0`, similar for `ty`.
7. **Final matrix**: `fz_translate(tx, ty) × fz_scale(s, s) × rotation × fz_translate(-page_origin)`. Order matters; verify in `PrintGeometry` unit tests.

**Why a pre-flight modal instead of `PrintDlgEx` hook?** `PrintDlgEx` with a custom property sheet page is the "right" Windows pattern but historically has Per-Monitor-DPI bugs (controls don't scale correctly across monitor switches) and the property-sheet API surface is large. A separate pre-flight modal trades one extra click for ~150 LOC of dialog-template-hook code we'd never want to maintain. Concretely: pre-flight modal in `PrintProgressDlg` mode "config" reuses the same `DLGTEMPLATE` infrastructure as the progress mode, costing ~20 extra LOC vs ~200 for a property-sheet page.

## 6. Error Handling

No silent failures. Every error path informs the user via MessageBox with the failing page number when applicable.

| Failure | Detection | Response |
|---|---|---|
| Pre-flight cancelled | User clicks Cancel | Return false silently |
| PrintDlg cancelled | `PrintDlg` returns 0, `CommDlgExtendedError() == 0` | Return false silently |
| PrintDlg error | `PrintDlg` returns 0, `CommDlgExtendedError() != 0` | MessageBox decoded via `FormatMessageW` |
| `StartDoc` fails | Returns ≤ 0 | MessageBox "Failed to start print job"; cleanup; return false |
| Page render OOM | MuPDF throws on `fz_new_pixmap_from_page` | Catch in C++ wrapper, `AbortDoc`, MessageBox with page number, cleanup, return false |
| User clicks Cancel mid-print | `SetAbortProc` callback returns FALSE | `AbortDoc`; clean exit; return false; informational MessageBox "Print cancelled at page X" |
| Printer goes offline mid-job | `EndPage` returns ≤ 0 | `AbortDoc`; MessageBox with last completed page; return false |
| Out-of-memory during pre-flight or progress dialog creation | `CreateDialogIndirectW` returns NULL | MessageBox; return false; print job never started |

## 7. Testing

Printing is a side-effect-heavy domain. Strategy:

| Test type | Approach | Count |
|---|---|---|
| Unit — geometry math (scale, rotation, centering matrix composition) | `PrintGeometry::compute_render_matrix` is a pure function; test with synthetic page/paper rects across all 3 scale modes × 2 orientations × auto-rotate on/off | ~6 |
| Unit — page-range parser (`PRINTDLG.lpPageRanges` of `1-3,5,7-9`) | Pure parser function in `PrintJob.cpp`, extracted for test | ~3 |
| Unit — abort-flag state machine (`SetAbortProc` callback semantics) | Mock printer DC; verify callback returns FALSE after flag set, TRUE before | ~2 |
| Integration — full print to "Microsoft Print to PDF" virtual printer | New CLI flag `litepdf.exe --print-to-pdf <input> <output>` gated by `#ifdef LITEPDF_TEST_HARNESS`; CI Windows runner asserts output PDF page count matches input | ~2 |
| Manual smoke (one-time per release) | Print `tests/fixtures/bookmarks.pdf` to "Microsoft Print to PDF" + a real local printer, observe output | — |

**Total: +13 tests** (11 unit, 2 integration). Brings cumulative count from 146 (post-Phase-8 with PR #5 merged) to **159**.

CI integration test runs on Windows runners only. Project is Windows-only anyway, so no matrix impact. Note: "Microsoft Print to PDF" is **not** pre-installed on GitHub Actions `windows-latest` (Windows Server 2022) runners — the optional feature `Printing-PrintToPDFServices-Features` must be enabled explicitly in the CI workflow step before the test runs (see plan Task 11 for the required `Enable-WindowsOptionalFeature` step).

## 8. Phase Placement — Selected: Phase 8.5

**User decision 2026-05-03: Option B (Phase 8.5).**

| Option | Where | Pros | Cons |
|---|---|---|---|
| A. Phase 8.x patch | Insert after Phase 8 ships v0.0.9-phase8 | Reuses Phase 8's `DLGTEMPLATE` infrastructure (`PasswordDialog`); fastest to ship | Tight against Phase 8's already ~600 LOC; pushes Phase 9 (icons) by ~1 week |
| **B. New Phase 8.5 — SELECTED** | Between Phase 8 and Phase 9 in the roadmap | Clean phase boundary; doesn't pollute Phase 8's "Tier 3 features" theme; cumulative LOC ~5,580 → ~5,925 still well under the original 8 MB exe target | Adds a new entry to the roadmap; one extra `v0.0.10-phase8.5` tag |
| C. Post-v1.0 | After Phase 12 release | Keeps v1.0 ship date | Print is "table stakes" for a PDF reader; users will complain on launch day; competing-tool review (SumatraPDF / Foxit / Adobe) all ship Day-1 print |

**Rationale.** Print is a baseline expectation for any PDF reader; deferring to post-v1 risks reputational damage at launch. A dedicated Phase 8.5 keeps Phase 8 cohesive (Tier 3 = password, multi-format, dark, dual-page) and Phase 9 cohesive (icons / installer prep). Tag at the end of Phase 8.5 is `v0.0.10-phase8.5`. Roadmap doc `docs/plans/2026-04-15-litepdf-roadmap.md` is updated in the same commit as this status flip.

## 9. Out of Scope / Deferred

Explicitly **NOT** in T2 (revisit for T3 / post-v1):

- **N-up layout** (2 / 4 / 6 pages per sheet) — T3
- **Duplex hint** (`DEVMODE.dmDuplex = DMDUP_VERTICAL/HORIZONTAL`) — T3
- **In-app print preview pane** (separate from the progress dialog; full visual proof of what the printer will receive) — T3
- **Print profiles / saved print configurations** — post-v1
- **Booklet / signature printing** — post-v1
- **Watermark on print** ("DRAFT" overlay, etc.) — post-v1
- **Print annotations only / form data only** — n/a (no annotation/form support in v1)
- **Print to file via XPS Document Writer** — already in scope, inherited for free from the OS PrintDlg's printer list; no extra code
- **Background printing on worker thread** (allow user to keep using viewer while job spools) — post-v1; current modal-progress UX is acceptable for occasional-use feature
- **Printing of encrypted PDFs without re-prompting password** — handled by Phase 8: doc must already be opened (decrypted) before Print menu is enabled

## 10. Cross-references

- **Design doc** `docs/plans/2026-04-15-litepdf-design.md` §10 — was extended in PR (companion to this spec) to mark Print as v1-out-of-scope and reference this spec
- **MuPDF refcount discipline**: `~/.claude/skills/mupdf-refcount-conventions` — `fz_clone_context` per render job, drop pixmap immediately after use
- **Phase 8 PasswordDialog (D1)**: `docs/plans/2026-05-01-phase-8-tier3-completion.md` — same in-memory `DLGTEMPLATE` pattern reused by `PrintProgressDlg`
- **Roadmap**: `docs/plans/2026-04-15-litepdf-roadmap.md` — needs an entry for Phase 8.5 if Option B is chosen

## Summary

| Dimension | Decision |
|---|---|
| Tier | T2 (PrintDlg + range + copies + scale + auto-rotate) |
| Render path | MuPDF pixmap → `StretchDIBits` to printer DC at printer-native DPI |
| Scale UX | Pre-flight modal with Fit / Actual / Custom% combobox (avoids `PrintDlgEx` HiDPI bugs) |
| Threading | Synchronous on UI thread inside modal progress dialog |
| Module | `src/printing/PrintJob.{hpp,cpp}` + `PrintProgressDlg.{hpp,cpp}` + `PrintGeometry.hpp` |
| LOC budget | ~345 total (320 print module + 25 wiring) |
| Tests | +13 (11 unit + 2 integration); 146 → 159 |
| Phase placement | **Phase 8.5** between Phase 8 and Phase 9 (option B selected by user 2026-05-03); tag `v0.0.10-phase8.5` |
