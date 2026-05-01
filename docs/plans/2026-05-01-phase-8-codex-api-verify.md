# Phase 8 Plan — API Surface Verification Report

Verification target: `docs/plans/2026-05-01-phase-8-tier3-completion.md`
Method: read-only static analysis vs. current source tree.

---

## VERIFIED

### MuPDF API
- `fz_invert_pixmap(fz_context*, fz_pixmap*)` exists at `third_party/mupdf/include/mupdf/fitz/pixmap.h:300` (exact prototype).
- `fz_pixmap_samples(fz_context*, const fz_pixmap*)` at `third_party/mupdf/include/mupdf/fitz/pixmap.h:243` (returns `unsigned char*`).
- `fz_pixmap_stride(fz_context*, const fz_pixmap*)` at `pixmap.h:248` (returns `int`).
- `fz_keep_pixmap` at `pixmap.h:191`, `fz_drop_pixmap` at `pixmap.h:199`.
- `fz_needs_password` and `fz_authenticate_password` are used at `src/core/Document.cpp:203` and `:239` (plan's understanding of the auth wrapper is correct).
- Pre-flight #5 claim that `Document::open` keeps the doc alive after returning `NeedsPassword` is correct: see `src/core/Document.cpp:200-208` (sets `impl_->path`, calls `fz_needs_password`, returns `NeedsPassword` while `impl_->doc` remains alive).

### litepdf core API (Document)
- `Document::open(const std::filesystem::path&)` returns `std::optional<OpenError>` — `src/core/Document.hpp:53` and `src/core/Document.cpp:176`. Test snippet `auto err = d.open("..."); REQUIRE(!err.has_value())` is correct.
- `OpenError` enum has `FileNotFound`, `UnsupportedFormat`, `NeedsPassword`, `Corrupted` (plus `BadPassword`, `OutOfMemory`, `Other`) at `src/core/Document.hpp:33-41`.
- `Document::is_open()` at `src/core/Document.hpp:55`.
- `Document::page_count()` at `src/core/Document.hpp:62`.
- `Document::outline()` at `src/core/Document.hpp:75`.
- Allowlist `looks_like_supported_document(...)` covers `.pdf/.epub/.cbz/.xps/.fb2/.svg` at `src/core/Document.cpp:113-152`. Plan's line range claim is correct.

### litepdf core API (RenderEngine / DocumentView / PageCache)
- `RenderRequest` field order in `src/core/RenderEngine.hpp:42-53`: `page_num, priority, scale, on_complete, bypass_cache`. Plan T3.1 proposed `bool invert` field added after `bypass_cache` — slot layout consistent.
- `RenderEngine::cancel_all_below_priority(int)` at `src/core/RenderEngine.hpp:77` and `.cpp:382`.
- `DocumentView::cancel_stale_renders(int keep_priority_threshold)` at `src/core/DocumentView.hpp:110` and `.cpp:279-281` (forwards to engine `cancel_all_below_priority`). Plan calls it on the view, which is correct.
- `DocumentView::current_page()`, `page_count()`, `request_render(int, RenderCb)`, `request_render_with_prefetch(int, RenderCb)`, `thumb_pane()`, `ensure_thumb_pane(HINSTANCE, HWND)` exist at `src/core/DocumentView.hpp:77-78, 107, 117, 151-158`.
- `DocumentView::ZoomMode { FitWidth, FitPage, Custom }` at `src/core/DocumentView.hpp:57`.
- `PageCache(std::size_t l1_capacity, std::size_t l2_capacity, fz_context* ctx)` at `src/core/PageCache.hpp:43`.
- `PageCache::l1_size()` and `l2_size()` exist at `src/core/PageCache.hpp:81-82` (for tests).
- `ThumbnailRenderer::pending_tasks{0}` drain pattern exists at `src/core/ThumbnailRenderer.cpp:59`, drain loop `:64-66`, fetch_add `:80`, fetch_sub `:93`.

### Win32 / dialog
- `GetDpiForWindow` is widely used (`MainWindow.cpp`, `FindBar.cpp`, `ResultsPanel.cpp`, `TabManager.cpp`, `ThumbnailPane.cpp`). Available on the project's target SDK.
- `DialogBoxIndirectParamW`, `DLGTEMPLATE`, `DLGITEMTEMPLATE`, `SecureZeroMemory`, `WideCharToMultiByte(CP_UTF8, ...)`, `GetDlgItemTextW`, `SetDlgItemTextW`, `GetDlgItem`, `EndDialog`, `SetWindowLongPtrW(... DWLP_USER)` — all standard Win32 from `<windows.h>` with the well-known atom encodings (0xFFFF + 0x0080 BUTTON / 0x0081 EDIT / 0x0082 STATIC). Style constants `ES_PASSWORD`, `ES_AUTOHSCROLL`, `BS_DEFPUSHBUTTON`, `BS_PUSHBUTTON`, `WS_TABSTOP`, `WS_BORDER`, `SS_LEFT`, `DS_MODALFRAME`, `DS_CENTER`, `DS_SETFONT`, `WS_POPUP`, `WS_CAPTION`, `WS_SYSMENU` are standard. (Spot-check passes; not exhaustively re-greppable from local repo without Windows SDK headers, but all are universally documented.)

### litepdf UI API
- `MainWindow::open_tab_async` at `src/ui/MainWindow.hpp:46` and implementation at `MainWindow.cpp:120-175`.
- `MainWindow::active_view()` at `MainWindow.hpp:79` and `.cpp:102`.
- `MainWindow::kick_render(int)` at `MainWindow.hpp:47` and `.cpp:177`.
- `MainWindow::on_layout()` at `MainWindow.hpp:49` and `.cpp:210`.
- `TabManager::tab_at(int)`, `count()`, `active_tab()`, etc., at `src/ui/TabManager.hpp:36-39`.
- `Tab::view` is `std::unique_ptr<DocumentView>` at `src/core/TabList.hpp:25`.
- `Tab::outline_visible` at `TabList.hpp:33`, `Tab::thumb_visible` at `TabList.hpp:40`.
- `IDR_ACCEL` does NOT exist (correctly noted in the plan's §"Re-Planning Lessons" L1321). Accelerators are programmatic in `MainWindow.cpp` `ACCEL accels[]`.
- `ACCEL accels[]` exists in `MainWindow.cpp` (real start: line 1379). Existing bindings: `Ctrl+O` (FILE_OPEN), `Ctrl+=`/`Ctrl+-`/`Ctrl+0` (zoom), `F4` (THUMBS), `F5` (OUTLINE), `Ctrl+W`, `Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+1..9`, `Ctrl+F`, `F3`, `Shift+F3`, `Ctrl+Shift+F`, `F6`, `Esc`. **`Ctrl+D` is NOT bound** (plan T0.3 claim correct).
- `IDM_VIEW_THUMBS = 40060` at `resources/MainMenu.rc.h:60` (highest existing ID; 40061 is next free as plan claims).
- `resources/litepdf.rc:60-61` View popup has the `Toggle &Thumbnails\tF4` anchor with the SEPARATOR at line 62 — exactly the structure plan T0.2 expects to insert between.

### Resource files
- `litepdf.rc` VERSIONINFO is `FILEVERSION 0,0,6,0` and `PRODUCTVERSION 0,0,6,0` at `resources/litepdf.rc:5-6`. String values `FileVersion "0.0.6.0"` line 19 and `ProductVersion "0.0.6.0"` line 24. Plan claim correct (sat at `0,0,6,0` since Phase 5).

### Fixtures
- `tests/fixtures/encrypted.pdf` exists.
- Password is `"test"` per `tests/unit/test_document_password.cpp:21` (`REQUIRE(doc.authenticate("test"))`).
- `tests/fixtures/sample.epub` and `tests/fixtures/sample.cbz` exist.
- `tests/fixtures/sample.png` exists (negative-case fixture).
- `tests/fixtures/{simple,bookmarks,corrupt,search}.pdf`, `測試.pdf` all exist.
- No XPS/FB2/SVG fixture present (plan D5/D6 acknowledges this).

### Test harness
- `tests/CMakeLists.txt` is the registration file; no `tests/unit/CMakeLists.txt` exists. Plan §"Re-Planning Lessons" L1322 is correct.

### `VERSION` file
- Currently `0.0.8-dev` per `VERSION:1`.

---

## DRIFT FOUND

### D1. P0 — `Document::context()` does not exist; method is `clone_context()`
- **Plan claim:** Checklist includes `Document::context()`; T3.5 test snippet uses `d.context()` repeatedly (e.g. `PageCache cache(/*l1=*/4, /*l2=*/4, d.context())`, `RenderEngine eng(d.context(), cache, /*workers=*/1)`, `fz_keep_pixmap(d.context(), p)`, `fz_pixmap_samples(d.context(), pix_normal)`).
- **Actual:** `src/core/Document.hpp:154` declares `[[nodiscard]] fz_context* clone_context() const;` (returns a freshly **cloned** context that the caller must `fz_drop_context()`). There is NO `context()` accessor.
- **Suggested fix:** Replace every `d.context()` in the T3.5 snippet with one cached `fz_context* ui_ctx = d.clone_context();` near the start of each test (and `fz_drop_context(ui_ctx)` in cleanup). Note that the existing canonical pattern at `tests/unit/test_render_engine_cache.cpp:32-34` does exactly this. Also: the test's keep/drop calls would fail compile against `d.context()`.

### D2. P0 — `RenderEngine` constructor signature in T3.5 is wrong
- **Plan claim (T3.5):** `RenderEngine eng(d.context(), cache, /*workers=*/1);` (positional: ctx, cache, workers).
- **Actual:** `RenderEngine.hpp:67`: `RenderEngine(Document& doc, std::size_t num_workers = 2, PageCache* cache = nullptr);` — first arg is `Document&`, not `fz_context*`; second is workers, third is cache pointer.
- **Suggested fix:** Replace with `RenderEngine eng(d, /*workers=*/1, &cache);` (matches `test_render_engine_cache.cpp:36`).

### D3. P0 — `cache.l1_hit_count()` accessor does not exist
- **Plan claim (T3.5):** "the existing helper already exposes a `cache_hit_count()` accessor"; later snippet shows `REQUIRE(cache.l1_hit_count() == 1);`.
- **Actual:** `PageCache.hpp` exposes only `l1_size()` and `l2_size()`. No hit-count instrumentation exists on the engine or the cache (greps across both `src/core/PageCache.cpp` and `tests/unit/test_render_engine_cache.cpp` find none — the existing cache test asserts hit by **pointer identity**, not by counter: `REQUIRE(first == second)`).
- **Suggested fix:** Either (a) drop the hit-count assertion and use pointer-identity hit detection (the canonical pattern), or (b) extend the plan to include `PageCache::l1_hit_count()` as a new instrumentation API in T3 and add a separate sub-task to expose + bump it from `RenderEngine::Impl::run_worker`. (a) is far cheaper.

### D4. P0 — Plan references `fz_run_page` but RenderEngine never calls it
- **Plan claim:** D7 — "the worker calls `fz_invert_pixmap(ctx, pix)` immediately after `fz_run_page` in `RenderEngine::Impl::run_worker`"; T3.1 — "after `fz_run_page`, if `req.invert`, call `fz_invert_pixmap`".
- **Actual:** `grep -r fz_run_page src/` returns zero hits. Rasterization is performed via `fz_new_pixmap_from_display_list(ctx, dlist, m, fz_device_bgr(ctx), 1)` at `src/core/RenderEngine.cpp:189`. There is no `fz_run_page` site.
- **Suggested fix:** Update D7 / T3.1 to read "immediately after `fz_new_pixmap_from_display_list` returns a non-null pixmap, before the L1 write at `RenderEngine.cpp:208`". The intent (post-rasterize, pre-cache) still works.

### D5. P1 — RenderEngine cache touch points are at lines 121 / 139 / 167 / 208, NOT 113 / 129 / 155 / 192
- **Plan claim (D8 + T3.1 table):** "L1 read at L113 + L1 write at L192. The L2 read at L129 and L2 write at L155".
- **Actual (current `src/core/RenderEngine.cpp`):**
  - L1 read (`cache->get_pixmap`): line **121** (claim 113 is off by 8).
  - L2 read (`cache->get_display_list`): line **139** (claim 129 is off by 10).
  - L2 write (`put_display_list`): line **167** (claim 155 is off by 12).
  - L1 write (`put_pixmap`): line **208** (claim 192 is off by 16).
- **Suggested fix:** Update D8 + the T3.1 cache-touch-points table to lines **121 / 139 / 167 / 208**. The L1-vs-L2 mapping (L1 = 121+208, L2 = 139+167) is correct; only the line numbers drift.

### D6. P1 — About-dialog literal is at MainWindow.cpp:936, not :917
- **Plan claim (multiple sites — Pre-flight reference, T5.2, "Version bump + tag" section):** `About-dialog literal at line 917 is currently "LitePDF v0.0.8"`.
- **Actual:** `MainWindow.cpp:917` is an `OPENFILENAME` field (`ofn.nFilterIndex = 1;`). The actual `L"LitePDF v0.0.8\n\n"` literal is at **`MainWindow.cpp:936`** inside the `IDM_HELP_ABOUT` `MessageBoxW` block.
- **Suggested fix:** Replace every "MainWindow.cpp:917" reference with "MainWindow.cpp:936". Step 5.2's `"v0.0.8" -> "v0.0.9"` instruction is otherwise correct.

### D7. P1 — `tests/unit/test_document_formats.cpp` already exists; T2 is a modify, not create
- **Plan claim (T2 Files):** "Create: `tests/unit/test_document_formats.cpp` (~80 LOC)."
- **Actual:** File exists with 2 `TEST_CASE`s (ePub + CBZ open). It is already registered in `tests/CMakeLists.txt:30`. Step 2.2's "expect FAIL (file missing); add to `tests/CMakeLists.txt`" workflow is invalid — the file and its CMake registration are both in place.
- **Suggested fix:** Change T2 wording to "Modify `tests/unit/test_document_formats.cpp` — add the XPS allowlist case + the .png reject case". Drop the `tests/CMakeLists.txt` modification. Adjust the cumulative test count: T2 adds **+2** (XPS allowlist + sample.png reject), not +4. Re-derive downstream cumulatives accordingly (see also D9 for full re-derivation).

### D8. P1 — `Ctrl+T` is not currently bound; plan misstates the existing accel set
- **Plan claim (T0.3):** "existing Phase 6/7 bindings are `Ctrl+F`, `Ctrl+Shift+F`, `Ctrl+1..9` (tab-goto), `Ctrl+T`, `Ctrl+W`, `Ctrl+O`, F4/F5".
- **Actual (`MainWindow.cpp:1379-1408`):** No `Ctrl+T` entry. The full current set: Ctrl+O, Ctrl+= / Ctrl+- / Ctrl+0, F4, F5, Ctrl+W, Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1..9, Ctrl+F, F3, Shift+F3, Ctrl+Shift+F, F6, Esc.
- **Suggested fix:** Drop `Ctrl+T` from the plan's enumeration. (`Ctrl+T` was never wired — there is no IDM_TAB_NEW.) The plan's choice of Ctrl+D for dual-page is still uncontested.

### D9. P1 — `accels[]` is at line 1379, not "around line 1360"
- **Plan claim (T0.3):** "`ACCEL accels[]` (around line 1360 — the same array that holds VK_F4 / VK_F5 / Ctrl+O)"; later "(read lines 1361–1390)".
- **Actual:** `MainWindow.cpp:1379` declares `ACCEL accels[] = {`. Range to read is 1379-1408.
- **Suggested fix:** Update line numbers in T0.3 to "around line 1379" / "read lines 1379-1408".

### D10. P1 — `Document::looks_like_supported_document` is a free function in an anonymous namespace, not a member
- **Plan claim (D4 + multiple sites):** `Document::looks_like_supported_document` (e.g. "Document::looks_like_supported_document already allowlists ...").
- **Actual:** `src/core/Document.cpp:113` declares it inside an anonymous namespace at TU scope: `bool looks_like_supported_document(const std::filesystem::path& path)`. It is not a member of `Document`. Not callable from outside the TU.
- **Suggested fix:** Drop the `Document::` qualifier when prose-referring to it. Has no impact on the implementation (plan's design uses it only as the existing internal allowlist that does not need extending) — but a future test that wanted to call it directly would fail. The plan's T2 tests open via `Document::open` (correct) so this drift is documentation-only.

### D11. P1 — Smoke-test helpers `Test-Step` / `Start-Litepdf` do not exist in `scripts/smoke-test.ps1`
- **Plan claim (T5.1):** `Test-Step "Open sample.epub" { Start-Litepdf "tests/fixtures/sample.epub" }` etc.
- **Actual:** `scripts/smoke-test.ps1` uses raw `Start-Process -FilePath $exe -ArgumentList ... -PassThru -NoNewWindow` and polls `$proc.MainWindowHandle` (lines 43-57, 123, 163, 239, 298, 384). No `Test-Step` or `Start-Litepdf` function definitions exist in the file.
- **Suggested fix:** Either (a) update T5.1 to follow the existing `Start-Process` + MainWindowHandle-poll pattern (cleanest — match the existing 4 smoke blocks), or (b) introduce `Test-Step` and `Start-Litepdf` helpers as a separate, called-out refactor sub-task. (a) is the smaller patch and matches the style of every other smoke block.

### D12. P1 — DocumentView's `request_render` has no priority argument
- **Plan claim (T4.2):** `view->request_render(left, ...)` at priority 0 and `view->request_render(right, ...)` at priority 1.
- **Actual:** `src/core/DocumentView.hpp:107` — `void request_render(int page, RenderCb on_complete);` Priority is hard-coded inside the implementation (and only `request_render_with_prefetch` issues a P0+P1+P2 fan-out). No public per-call priority.
- **Suggested fix:** Either (a) extend `request_render` with an optional priority arg as part of T4 (small change, ~5 LOC + comments), or (b) pivot dual-page to call `request_render_with_prefetch(left, ...)` for the left + a bare `request_render(right, ...)` for the right — accepting that the right page lands at the engine's default priority. The plan should be explicit about this; current text reads as if priority is already a public arg.

### D13. P2 — Plan T4.2.5 uses fictional `FitMode::FitPage` / `FitMode::FitWidth` / `compute_fit_zoom` names
- **Plan claim (T4.2.5):** `if (mode == FitMode::FitWidth) ...`, `if (mode == FitMode::FitPage) ...`; "Find `PdfCanvas::set_zoom_mode`".
- **Actual:** Enum is `litepdf::core::DocumentView::ZoomMode { FitWidth, FitPage, Custom }` at `DocumentView.hpp:57`. `set_zoom_mode` is on `DocumentView`, not on `PdfCanvas`. No `compute_fit_zoom` exists. Helpers `client_dip_width()`, `client_dip_height()`, `view_->page_width_dip(page)`, `view_->page_height_dip(page)` do not exist either — they are pseudocode names invented by the plan.
- **Suggested fix:** Plan already labels this section "pseudocode; locate actual signature before editing", which is acceptable. Recommend tightening: rename `FitMode` to `DocumentView::ZoomMode`, and explicitly state the sub-task "expose page DIP dimensions on DocumentView (or compute inside PdfCanvas using existing helpers)" as a prerequisite, with a specific source location for the existing fit-zoom code.

### D14. P2 — Tag-baseline cumulative test count: T2 contributes +2, not +4
- **Plan claim (T5.4 derivation table):** "T2: 4 [document][format] | +4". Cumulative reaches 147 / 148.
- **Actual:** `test_document_formats.cpp` already has 2 of those 4 cases (ePub + CBZ). T2 only adds 2 NEW cases (XPS allowlist + .png reject).
- **Suggested fix:** Update the table:
  - baseline: 130/131 (unchanged)
  - +T1.2 (3 password_dialog) → 133/134
  - +T1.5b (4 password_retry) → 137/138
  - +T2 (**2** [document][format]) → **139/140** (not 141/142)
  - +T3.2 (1 page_cache invert) → 140/141
  - +T3.5 (2 render_engine invert) → 142/143
  - +T4 (3 dual_page) → **145/146** (not 147/148)
- The "Done When" line "ctest 147/147" should also drop to **145/145** (or 146/146 if `phase-7-polish` merged).

### D15. P2 — `fz_invert_pixmap` claim "near line 300" should be "exactly line 300"
- Cosmetic. Plan says "line ~300"; it's actually exactly line 300. No fix needed; flagging for completeness.

---

## AMBIGUOUS / UNVERIFIABLE

- **Cold-start budget impact of `fz_invert_pixmap` (R3).** Plan estimates 2-5 ms SSD / 5-15 ms HDD. Cannot benchmark statically; this is the kind of risk that needs runtime measurement. Tag it as monitor-at-implementation.
- **`pix` non-null guarantee from `fz_new_pixmap_from_display_list`.** The plan's `if (req.invert && pix) fz_invert_pixmap(ctx, pix);` guard is correct — the existing code at `RenderEngine.cpp:185-198` shows `pix` may be null on `fz_catch`. Verified the guard is necessary.
- **`ThumbnailRenderer` integration with the new `invert` axis.** Plan T3 doesn't call out whether thumbnails inherit the per-tab invert flag. Currently `ThumbnailRenderer` submits with `bypass_cache=true` and presumably `invert=false`. If users expect thumbs to also invert, T3 needs an extra wire. Recommend explicit decision in plan (likely: "thumbs stay un-inverted in v1, by D9 polish parity").
- **`Document::looks_like_supported_document` extension `.cbz` magic-byte check.** `Document.cpp:143-149` requires a ZIP magic header (`PK\x03\x04`) on `.cbz`. T2's `tests/fixtures/sample.cbz` must therefore be a real ZIP. Could not verify the file's magic without reading binary content; plan can rely on the current presence-of-fixture pre-flight check (#3).
- **`PasswordDialog`, `password_retry.hpp`, `try_authenticate_with_retry`.** PLANNED — not yet present. T1 introduces them. Correct (no drift to flag).
- **`DocumentView::invert_colors_`, `dual_page_`, `set_invert_colors`, `set_dual_page`, `dual_page()`, `invert_colors()`, `PdfCanvas::set_invert_chrome`, `PdfCanvas::set_dual_page`, `dual_page_compute_left/right`, `PdfCanvasLayout.hpp`.** PLANNED — introduced in T3 / T4. Correct.
- **`RenderRequest::invert` field.** PLANNED — added in T3.1. Correct.
- **`PageCache` L1 key extension to `(page, scale, invert)`.** PLANNED — added in T3 step. Correct.
- **`Document::OpenError::BadPassword` semantics.** Plan does not exercise `BadPassword` (plan calls `authenticate` and inspects the bool return). The enum value exists but seems unused in the open path — `Document::open` only returns `NeedsPassword`, never `BadPassword`. `BadPassword` is presumably a future-reserved value. No drift, but plan implementers should know not to expect to see it.
- **`SecureZeroMemory` on `std::wstring::data()`.** Plan calls `SecureZeroMemory(entered_w.data(), entered_w.size() * sizeof(wchar_t))`. C++17's `std::basic_string::data()` returns a writable `CharT*`; this is well-formed. Behavior is correct on MSVC. (Note: `SecureZeroMemory` is in `<windows.h>` via `winnt.h` — ubiquitously available.)
- **`DialogBoxIndirectParamW` returning `INT_PTR`.** Standard Win32; signature matches plan.
- **`DLGTEMPLATE`/`DLGITEMTEMPLATE` field names (`style`, `dwExtendedStyle`, `cdit`, `x/y/cx/cy`, `id`).** Standard; not greppable from local repo without Windows SDK headers, but universally documented. Plan's usage matches the MS sample at the URL it cites.
- **`docs/plans/2026-04-15-litepdf-design.md` §3 / §4.1 / §6.1 / §10 reference targets.** Did not open this file (out of scope per task focus); plan's claim that it has those sections is assumed correct.
