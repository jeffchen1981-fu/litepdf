# CJK System-Font Loader — Reaching the 8 MB exe Target (Design Spec)

> **Status:** v4 — the plan-gate round-2 review (on the derived plan) **corrected a
> load-bearing mechanism error that survived two spec rounds**: a missing-CJK-font
> hook does **not** blank the page (the appearance-synthesis throw is swallowed at
> [pdf-appearance.c:3815](../../../third_party/mupdf/source/pdf/pdf-appearance.c) —
> source-verified). D6 is reframed from "prevents blank page" to a cheap never-NULL
> invariant (empty-vs-tofu CJK form fields on font-less machines); the form-appearance
> integration test is dropped, with the Task-2 unit test as the D6 guard. See §10.
> v3 — brainstormed 2026-06-21, **hardened over two three-lens
> spec-gate review rounds** (Opus `pr-review-toolkit:code-reviewer` + Sonnet
> `feature-dev:code-reviewer` + Codex `codex exec` read-only). Round-1 (on v1)
> surfaced the **blank-page** failure mode, a false caching claim, a second
> `TOFU_CJK_LANG` consult site, and an existing CJK-search regression gate (→ v2).
> Round-2 (on v2) **independently verified D6 safe** (Opus + Codex traced shaping /
> CID widths / appearance-path `state->w=1` / vertical writing / base14
> availability) and found one real spec-completeness BLOCKER (the size-gate
> self-test rebase) plus test-design and lock-discipline tightenings (→ v3). No
> round either round found a defect in the core approach. All folded in — see §10.
> Ready for `superpowers:writing-plans`.
>
> Realizes the **"C-real"** path that
> [`2026-06-07-phase-11.5-size-prune-coldstart-design.md`](2026-06-07-phase-11.5-size-prune-coldstart-design.md)
> §7 scoped and deferred to post-v1.0, and closes the original
> [`2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md) §3
> **8 MB exe target**.
>
> **Starting state:** `main` clean at `59f7338`; `VERSION = 1.1.0`; branch
> `cjk-system-font-loader`. MuPDF pinned at 1.27.2, pruned `LITEPDF_PRUNE_V9`
> (`cmake/ImportMuPDF.cmake`), which keeps the embedded **Droid Sans Fallback
> Full** CJK font via `TOFU_CJK_LANG`. `litepdf.exe` = 12,314,624 bytes
> (~11.74 MB). CI hard-gates size at `size_ceiling_bytes` = 19,000,000
> (`benchmarks/thresholds.json` + `check-benchmark-regression.ps1` self-test) and
> `$maxBytes` = 25 MB (`smoke-test.ps1`).

## 1. Goal

Drop the last embedded CJK font — **Droid Sans Fallback Full (~4.84 MB)** — by
adding the MuPDF prune macro **`TOFU_CJK`** (keeping `TOFU_CJK_LANG`, see D3/§4.3),
and recover CJK rendering by installing a **Windows DirectWrite-backed system-font
loader** via `fz_install_load_system_font_funcs`.

- **Expected size:** exe 12.31 MB → **~7.5 MB**, hitting the 8 MB design target.
  Treated as **measurement-gated**, not assumed (§9, MINOR-6).
- **Verified rendering path:** with `TOFU_CJK` set, `fz_lookup_cjk_font` returns
  no data, so both `fz_new_cjk_font` ([font.c:957](../../../third_party/mupdf/source/fitz/font.c))
  and `pdf_load_substitute_cjk_font` ([pdf-font.c:436](../../../third_party/mupdf/source/pdf/pdf-font.c))
  call `fz_load_system_cjk_font(ctx, "SourceHanSerif", ordering, serif)`, which
  dispatches to our installed `load_cjk_font` hook
  ([font.c:458-476](../../../third_party/mupdf/source/fitz/font.c)).

**Non-goal:** this effort does not restore non-CJK script coverage (Arabic, Thai,
Hebrew, Devanagari, etc.), which `TOFU_NOTO` already drops in V9 and which ship as
tofu in v1.1.0. It does not add a "download font" UX, and does not touch cold-start
or any other phase.

## 2. Scope

**In scope:**
- Add `TOFU_CJK` to the prune set (keep `TOFU_CJK_LANG`) and bump the prune
  version (`cmake/ImportMuPDF.cmake`), including the prune-effective assertion.
- New `src/core/SystemFonts.{hpp,cpp}` unit: a DirectWrite CJK font loader with a
  guaranteed last-resort fallback + a one-line install entry point.
- Install the hook once per `Document` base context (`src/core/Document.cpp`).
- Link `dwrite` into `litepdf_core` (`CMakeLists.txt`).
- Re-baseline the size gates (soft: measured + headroom).
- Re-verify CJK rendering (liveness + existing CJK search test + regenerated
  reference PNGs) and CJK form/widget appearance.
- Update roadmap / known-limitations docs.

**Out of scope (explicit non-goals):**
- The `load_font` (named) and `load_fallback_font` (non-CJK script) hooks — the
  install call passes `nullptr` for both. Consequently the Han **script-fallback**
  path (a non-CJK base font encountering a CJK codepoint, font.c:2124) is **not**
  covered by this effort and renders notdef, consistent with the non-CJK scripts
  already tofu in v1.1.0 (§4.3, §7).
- Any change to the set of `FZ_ENABLE_*` features.
- A user-facing "missing font" dialog.

## 3. Resolved Decisions (brainstorming 2026-06-21 + three-lens review)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Missing-font fallback strategy | **Fully remove embedded CJK; accept + document tofu on font-less machines** | Hitting ~7.5 MB requires dropping the embedded font entirely. The miss case is rare (standard Win10/11 ship JhengHei/YaHei/Yu Gothic/Malgun) and made **non-fatal** by D6. This is the Phase 11.5 §7 C-real intent. |
| D2 | Feature scope | **CJK-only** (`load_cjk_font` hook only) | The 8 MB blocker is the CJK font; non-CJK script coverage (incl. Han script-fallback) is an independent concern. YAGNI — keep code + test surface minimal. |
| D3 | Prune macro | **Add `TOFU_CJK`; KEEP `TOFU_CJK_LANG`** | `TOFU_CJK` alone drops the embedded font (outermost guard, [font-table.h:281](../../../third_party/mupdf/source/fitz/font-table.h)). Keeping `TOFU_CJK_LANG` *also* defined leaves the second consult at [font.c:2124](../../../third_party/mupdf/source/fitz/font.c) compiled out, so the Han script-fallback path stays byte-identical to today (no new `fz_load_fallback_font` calls). Review-corrected (Codex M2). |
| D4 | Loader implementation | **DirectWrite (Approach A), file-path memory model (`fz_new_font_from_file`)** | DirectWrite is free-threaded (matches the render-worker call site), cleanly handles TTC collections (MingLiU is a `.ttc`) and yields the correct face index, and is a natural sibling of the Direct2D stack. `fz_new_font_from_file` lets MuPDF own the font buffer (refcount-clean). **NB (Opus M2):** `fz_new_font_from_file` does a full `fz_read_file` of the whole file into a heap buffer ([font.c:868](../../../third_party/mupdf/source/fitz/font.c)) — **not** an mmap; for a multi-MB TTC this reads the whole collection on each resolution (perf: §4.1 resolution cache + §9). GDI `GetFontData` rejected: per-DC realization + ambiguous TTC face-index. |
| D5 | CJK font style | **Honor the `serif` flag the hook receives** | The two call paths differ: `fz_new_cjk_font` hardcodes `serif=1` ([font.c:957](../../../third_party/mupdf/source/fitz/font.c)) — used by appearance synthesis, so forms/annotations always get 明體/Mincho/Batang (acceptable, matches the requested "SourceHanSerif"); `pdf_load_substitute_cjk_font` passes the PDF descriptor's `PDF_FD_SERIF` flag ([pdf-font.c:507-513](../../../third_party/mupdf/source/pdf/pdf-font.c)), so the substitute path genuinely receives `serif∈{0,1}`. The mapping table handles both. (For forms, the `fz_new_cjk_font` call is only a probe; the visible glyphs come from the Type0 font `pdf_add_cjk_font` writes, re-resolved through the substitute path — the serif outcome is consistent either way — Opus NIT-2.) |
| D6 | Hook never returns NULL | **Last-resort fallback to base14 Helvetica (cheap never-NULL invariant)** | The loader's final fallback returns `fz_lookup_base14_font("Helvetica") → fz_new_font_from_memory`, so the hook never returns NULL. **Corrected rationale (plan round-2 — the original "prevents a blank page" claim was wrong, see §5):** a NULL hook makes `fz_new_cjk_font` throw `FZ_ERROR_ARGUMENT` ([font.c:967](../../../third_party/mupdf/source/fitz/font.c)), but during form/annotation appearance synthesis that throw is **swallowed** by the inner `fz_catch` at [pdf-appearance.c:3815](../../../third_party/mupdf/source/pdf/pdf-appearance.c) (rethrows only `REPAIRED`/`SYSTEM`); the page renders fine and only that CJK **form field** is left empty. So the real, narrow effect of the base14 last-resort: on a font-less machine, CJK **form fields** render `.notdef` tofu instead of empty. base14 is preferred (compiled in under V10 since `TOFU_BASE14` is unset, OS-independent, makes the "no candidate" branch unit-testable — §6). Verified safe: a non-CJK font tagged `cjk=1` renders `.notdef` (CID widths come from the PDF `/W`/`/DW`, vertical from `/W2`/`/DW2` — never the substitute font's metrics). Kept as a 5-line invariant; the elaborate form-appearance **integration** test it once justified is **dropped** — its CI value was nil (CI has CJK fonts, so the last-resort never fires there). |

## 4. Components

### 4.1 New unit — `src/core/SystemFonts.{hpp,cpp}`

**Public surface (one function):**

```cpp
namespace litepdf {
// Install the Windows DirectWrite-backed CJK system-font loader on `ctx`.
// Call once per Document base context, right after fz_register_document_handlers.
// fz_clone_context shares the refcounted fz_font_context (context.c:340), so
// every per-render escrow / worker clone inherits the hook automatically — no
// per-clone install. (Verified: the only fz_new_context in src/ is Document.cpp:84;
// RenderEngine workers and the PdfCanvas escrow are all clones of it.)
void install_system_cjk_font_loader(fz_context* ctx) noexcept;
}
```

`install_system_cjk_font_loader` calls
`fz_install_load_system_font_funcs(ctx, nullptr, &load_cjk_font_cb, nullptr)`
(the `load_font` and `load_fallback_font` hooks are `nullptr` per D2).

**File-scope callback** `load_cjk_font_cb(fz_context*, const char* name, int ordering, int serif)`
matching `fz_load_system_cjk_font_fn` ([font.h:269](../../../third_party/mupdf/include/mupdf/fitz/font.h)).

**Pure mapping function** `cjk_family_candidates(int ordering, int serif)` — returns
an ordered list of Windows family names, no DirectWrite, fully unit-testable.
Ordering constants are `enum { FZ_ADOBE_CNS, FZ_ADOBE_GB, FZ_ADOBE_JAPAN, FZ_ADOBE_KOREA }`
([font.h:138](../../../third_party/mupdf/include/mupdf/fitz/font.h)):

| ordering | serif=1 (primary) | serif=0 |
|---|---|---|
| 0 `FZ_ADOBE_CNS` (Traditional Chinese) | `PMingLiU`, `MingLiU` | `Microsoft JhengHei`, `Microsoft JhengHei UI` |
| 1 `FZ_ADOBE_GB` (Simplified Chinese) | `SimSun`, `NSimSun` | `Microsoft YaHei`, `Microsoft YaHei UI` |
| 2 `FZ_ADOBE_JAPAN` | `MS Mincho`, `MS PMincho` | `Yu Gothic`, `MS Gothic`, `Meiryo` |
| 3 `FZ_ADOBE_KOREA` | `Batang`, `BatangChe` | `Malgun Gothic`, `Gulim` |

Candidates are tried in order; if the requested-style list yields nothing, the
loader falls through to the other style, then to the **D6 last-resort font**.
Unknown `ordering` → still resolves the last-resort font (never NULL).

**DirectWrite resolution (Approach A + file-path memory model):**

1. A process-cached `IDWriteFactory` (`DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, …)`),
   created lazily under a `std::once_flag`. DirectWrite does **not** require
   `CoInitialize` — unlike the print path, which `CoInitializeEx`es for
   `PrintDlgExW`'s own reasons ([PrintJob.cpp:127](../../../src/printing/PrintJob.cpp));
   do **not** add a `CoInitialize` to the render workers. DirectWrite objects are
   free-threaded in practice; the worker pool shares one factory. (If a
   concurrency issue ever surfaces, the cheap fallback is a per-call collection
   fetch or a mutex around the factory.)
2. For each candidate family: `GetSystemFontCollection(&coll, FALSE)` →
   `coll->FindFamilyName(wname, &index, &exists)`; if `exists`,
   `GetFontFamily(index)` → `GetFirstMatchingFont(NORMAL, NORMAL, NORMAL)` →
   `CreateFontFace(&face)`. Bold/italic CJK are **not** requested here; MuPDF
   synthesizes them via `fake_bold`/`fake_italic` flags
   ([pdf-font.c:420-426](../../../third_party/mupdf/source/pdf/pdf-font.c)), which
   is correct since system CJK families rarely ship separate bold faces.
3. `face->GetFiles(&n /*1*/, &fileRef)`; `fileRef->GetLoader(&loader)` +
   `fileRef->GetReferenceKey(&key, &keySize)`; QI `loader` →
   `IDWriteLocalFontFileLoader` (**check the QI result first** — a failure means a
   non-local loader, skip the candidate); then
   `GetFilePathFromKey(key, keySize, pathBuf, …)`. **Keep the `IDWriteFontFile`
   ComPtr in scope until `GetFilePathFromKey` returns** — the key points into the
   file object's storage (Opus m4). `face->GetIndex()` → the TTC face index.
4. Convert the path to UTF-8 and call
   `fz_new_font_from_file(ctx, name, utf8_path, faceIndex, 0)`.
5. A non-local file loader (≈never for system fonts) → skip the candidate.

**Process-wide resolution cache (perf — Codex M4):** the substitute path
(`pdf_load_substitute_cjk_font`) does **not** populate `ctx->font->cjk[]`, and
each RenderEngine worker opens its own `fz_document`
([RenderEngine.cpp:283/323](../../../src/core/RenderEngine.cpp)), so the hook is
invoked per-CID-font **per worker-document** — not once per process. Cache the
**resolved `(ordering, serif) → (utf8_path, faceIndex)`** mapping in a process-wide
table under a mutex, so the DirectWrite enumeration runs once per `(ordering, serif)`
regardless of worker/document count. **Lock discipline (Opus m2 / Sonnet):** use the
double-checked pattern — lock → check cache → on miss **release the mutex**, run the
slow DirectWrite enumeration, **re-lock**, insert (tolerating a redundant concurrent
enumeration). Do **not** hold the cache mutex across the enumeration (it would
serialize every worker's first CJK resolution). This cache mutex is separate from
the factory's `std::once_flag`; the hook is not invoked under any `fz` spinlock
(the FREETYPE/ALLOC locks are in the font-*free* path, not the load path), so a
plain `std::mutex` here cannot deadlock against MuPDF's locks. (The
`fz_new_font_from_file` heap read still happens per context — fonts are
context-bound and cannot be shared across clones; §9 flags this for the benchmark.)

**ABI / exception safety (load-bearing):** the callback is a C function pointer
called from MuPDF's C frame. No C++ exception may cross it:
- The whole body is wrapped `try { … } catch (...) { /* fall to last-resort or */ return nullptr; }`.
- Each `fz_new_font_from_file` call **and the last-resort base14 construction** are
  wrapped in `fz_try/fz_catch`; on MuPDF error, move to the next candidate / the
  last-resort, returning NULL only past the last-resort.
- **Catch *all* MuPDF errors at the last-resort, including `FZ_ERROR_SYSTEM` and
  `FZ_ERROR_TRYLATER`, and return NULL — do not rethrow** (Opus MINOR-1):
  `fz_load_system_cjk_font` ([font.c:464-472](../../../third_party/mupdf/source/fitz/font.c))
  rethrows exactly those two from the hook, so letting one escape the last-resort
  would reopen the blank-page path D6 exists to close.
- Per D6 the loader returns NULL only if even base14 fails to construct
  (pathological); NULL is the documented "no font" signal
  ([font.h:267](../../../third_party/mupdf/include/mupdf/fitz/font.h)).

**COM lifetime:** reuse the existing `PdfCanvas` ComPtr convention (RAII) for the
per-call interface pointers. The cached factory is a Meyers-style function-local
singleton (leak-on-exit acceptable for a process-lifetime resource).

### 4.2 Install point — `src/core/Document.cpp`

One line in `Document::Impl()`, immediately after
[`fz_register_document_handlers(ctx)` (Document.cpp:86)](../../../src/core/Document.cpp):

```cpp
fz_register_document_handlers(ctx);
litepdf::install_system_cjk_font_loader(ctx);   // new
```

The loader lives in `litepdf_core`, so both the GUI and `litepdf-cli`
(benchmark/fixtures, which use `core::Document`) get CJK. Because the font context
is shared across `fz_clone_context` clones, this single install covers every
per-render escrow and per-worker context (verified — §4.1 header comment).

### 4.3 Build config — `cmake/ImportMuPDF.cmake` + `CMakeLists.txt`

- `cmake/ImportMuPDF.cmake`:
  - In the prune define block, **add** `#define TOFU_CJK` (keep `TOFU_CJK_LANG`,
    per D3); bump `_PRUNE_VER` `LITEPDF_PRUNE_V9` → `LITEPDF_PRUNE_V10`. The version
    bump triggers the existing idempotent re-patch: restore `config.h` to pristine,
    re-prepend the current define set, and **delete the ExternalProject build
    stamp** so MuPDF rebuilds (without the stamp delete the rebuild silently
    no-ops and the size measurement lies).
  - **Update the prune-effective assertion** (the `foreach(_t TOFU_SYMBOL
    TOFU_NOTO TOFU_CJK_LANG)` over `font-table.h`, ~line 169): add `TOFU_CJK` so the
    newly-injected macro has live-consult drift coverage (`#ifndef TOFU_CJK` exists
    at [font-table.h:281](../../../third_party/mupdf/source/fitz/font-table.h)).
    Update the status message accordingly (Codex M1 / Sonnet C1 / Opus n2).
- `CMakeLists.txt`: add `dwrite` to `litepdf_core`'s `target_link_libraries`
  (PRIVATE — it is litepdf code calling DirectWrite).

### 4.4 Size gates (soft, measured + headroom — D3 of the original Q3)

- `benchmarks/thresholds.json`: `size_ceiling_bytes` 19,000,000 → the measured
  post-prune exe + ~1.5 MB headroom (expected ≈ 9,000,000; final value set from
  the real Release build).
- `scripts/check-benchmark-regression.ps1`: **rebase ALL `-SelfTest` synthetic
  `exe_bytes`, not just assertion 8** (Sonnet BLOCKER — verified). The self-test
  (lines ~124-172) hardcodes `exe_bytes ≈ 18,000,000` in the timing/delta
  assertions (1-5) and `base 18900000 / pr 19050000` in assertion 8 — all chosen
  for the *old* 19,000,000 ceiling. `Invoke-BenchmarkCompare` evaluates
  `$ceilFail = ($prBytes -gt size_ceiling_bytes)` on **every** call, so once the
  ceiling drops to ~9,000,000 the ~18 MB PR bytes in the "allowed" assertions
  **2, 3, 5** make `Failed=$true` → those `Check ($r.Failed -eq $false)` lines fail
  → self-test reports 5/8 → the required `build-windows` check blocks the PR. Fix:
  set every timing/delta assertion's `exe_bytes` **well under** the new ceiling
  (e.g. ~8,000,000, preserving the per-assertion *delta* relationships), and make
  only assertion 8 straddle it (`base = ceiling − 200000`, `pr = ceiling + 50000`),
  so all eight still pass for the right reasons (8/8). Any new PowerShell stays
  5.1-parse-safe (no `?.`/`??`/ternary).
- `scripts/smoke-test.ps1`: **change the `$maxBytes` value** `25MB` → ~`12MB`
  (line 27) **and** update the now-stale comment (lines 25-26 reference "Droid Sans
  Fallback Full / ~18-20 MB") — updating only the comment leaves the gate at 25 MB.

Exact gate numbers are intentionally **not** hard-coded in this spec; the plan
sets them from the actual post-prune Release build.

## 5. Error Handling (fail-soft to tofu, never crash, never blank)

Two distinct font-load entry points reach our hook; both are now non-fatal:

**(a) Content-stream CID fonts** (the common path): callback returns NULL only if
the last-resort font also fails (pathological). If it ever does, `fz_lookup_cjk_font`
returns NULL under `TOFU_CJK`, so `pdf_load_substitute_cjk_font`
`fz_throw(FZ_ERROR_SYNTAX, …)` ([pdf-font.c:445](../../../third_party/mupdf/source/pdf/pdf-font.c)),
caught at `pdf_try_load_font` ([pdf-interpret.c:785-798](../../../third_party/mupdf/source/pdf/pdf-interpret.c))
→ base14 hail-mary → page renders, CJK runs are `.notdef` tofu.

**(b) Form/widget/annotation appearance synthesis**
([pdf-appearance.c:1670-1691](../../../third_party/mupdf/source/pdf/pdf-appearance.c))
calls `fz_new_cjk_font` during `pdf_update_appearance`, which runs on `fz_load_page`
via `pdf_update_page` ([pdf-annot.c:419](../../../third_party/mupdf/source/pdf/pdf-annot.c)).
A NULL hook makes `fz_new_cjk_font` throw `FZ_ERROR_ARGUMENT`
([font.c:967](../../../third_party/mupdf/source/fitz/font.c)) — but **verified by
source-reading (plan round-2)** that throw is **swallowed**: the inner `fz_catch`
at [pdf-appearance.c:3815](../../../third_party/mupdf/source/pdf/pdf-appearance.c)
rethrows only `REPAIRED`/`SYSTEM` and `fz_warn`s the rest, and the outer catch at
[pdf-appearance.c:3842](../../../third_party/mupdf/source/pdf/pdf-appearance.c) then
sees no error in flight, so `pdf_update_appearance` returns normally. The field's
appearance stream is simply not created → the widget renders **nothing** (empty
field); **the page is not blanked and the pixmap is non-null.** (The earlier
"blank page / dropped display list" model in spec v1–v3 was wrong — it did not
trace past the inner catch.)

**The D6 base14 last-resort (b):** because the hook returns a guaranteed font rather
than NULL, appearance synthesis succeeds and the CJK form field renders `.notdef`
tofu (widths from the PDF, not the substitute font). So D6 turns an **empty** CJK
form field into a **tofu** one — a minor cosmetic improvement, not crash/blank
prevention. **Result on a font-less machine: full page render in all cases —
content CJK tofus via the hail-mary (a); CJK form fields render tofu (with D6) or
empty (without) — never blank, never a crash.** Latin text, graphics, and images are
unaffected. This is the documented, accepted limitation of D1.

Other cases:
- COM/HRESULT failure mid-resolution → treat as "candidate not found", try the
  next candidate, then the last-resort font.
- No `FZ_ENABLE_*` link-breakage risk: this adds a `TOFU_*` macro and `dwrite.lib`;
  it disables no feature path.

## 6. Testing Strategy

- **Unit (TDD red-green core):** (1) `cjk_family_candidates(ordering, serif)`
  returns the expected ordered family list for all 8 `(ordering × serif)` pairs and
  an empty list for an unknown ordering — pure, no OS dependency. (2) **D6
  last-resort coverage (Sonnet MAJOR):** call the hook with a bogus `ordering`
  (e.g. 99) so all candidates are skipped and assert it returns **non-NULL** (the
  base14 last-resort). This is deterministic on any machine — base14 needs no
  DirectWrite — so it guards the D6 contract even on a CI runner that has CJK fonts
  (where the normal path never reaches the last-resort).
- **Existing CJK search/extraction gate (must stay green — Codex M5):**
  `tests/unit/test_document_search.cpp`'s `"page_hits: Unicode needle matches CJK
  text"` case (search.pdf p2, a non-embedded STSong CID fixture) already asserts
  CJK extraction/search. Run it post-prune: extraction maps glyphs→Unicode via
  ToUnicode/CMaps ([pdf-op-run.c](../../../third_party/mupdf/source/pdf/pdf-op-run.c)),
  independent of the backing font, so it should stay green — and proves the font
  switch did not regress shipped search. Add a `Document::page_text` liveness
  assertion on a CJK fixture as a companion.
- **Liveness:** open each of `tests/fixtures/cjk-zh-hant.pdf`, `cjk-ja.pdf`,
  `cjk-ko.pdf`, render page 1 → non-blank pixmap, no throw/crash. Passes
  regardless of glyph fidelity, so the windows-2022 CI runner stays green even if
  its installed CJK font set differs from the dev machine.
- **D6 guard is the Task-2 unit test, not an integration fixture.** The deterministic
  D6 guard is the `resolve_cjk_system_font(ctx, …, ordering=99, …) != NULL`
  unit test above (no system CJK font needed). A form-appearance **integration**
  fixture+render test was considered and **dropped**: it cannot fire the last-resort
  on CI (which has CJK fonts) and the no-D6 failure is a benign empty form field, not
  a blank page (§5b), so the elaborate hand-built AcroForm fixture is not worth its
  cost. CJK form-field rendering on a font-less machine is covered by the
  known-limitation note, not an automated gate.
- **Manual visual (the real fidelity gate, per Phase 11.5 §4.3):** on the zh-TW
  dev machine, **regenerate `tests/fixtures/cjk-reference/*.png`** against the new
  system-font render, eyeball real glyphs vs tofu, and **confirm the TTC face index
  selected the intended face** (TC renders PMingLiU, not MingLiU_HKSCS — Opus m3),
  re-commit as the new baseline; attach before/after to the PR. The old reference
  PNGs were Droid Sans and are now stale.
- **Determinism gate unaffected:** `scripts/generate-cjk-fixture.py --check`
  validates PDF-bytes determinism (reportlab/zlib), not rendering — fixtures don't
  embed fonts, so it stays green. **Do not** add a render-hash gate for CJK:
  system-font output is not byte-reproducible across machines/Windows versions.
- **GUI smoke:** open the zh-Hant fixture in the GUI, screenshot, confirm real
  glyphs (per `reference_litepdf_scripted_gui_smoke`).
- **No perf regression:** full ctest suite + `large.pdf` benchmark; plus confirm
  the per-worker CJK resolution (§4.1 cache) does not regress CJK page render
  timing materially (§9).

## 7. What This Effort Does NOT Do

- Cover the Han **script-fallback** path (non-CJK base font hitting a CJK
  codepoint, font.c:2124) — that needs the `load_fallback_font` hook (left
  `nullptr`); it renders notdef, like the non-CJK scripts already tofu in v1.1.0.
- Restore non-CJK script coverage (Arabic/Thai/Hebrew/Devanagari…). A future
  effort can add `load_font` / `load_fallback_font` (zero size cost) if desired.
- Add a missing-font UX.
- Touch cold-start, the benchmark harness internals, or any unrelated MuPDF flag.

## 8. Build Sequence (for the implementation plan)

1. **Loader unit (TDD):** `cjk_family_candidates` red-green; then the DirectWrite
   resolution + D6 last-resort + the `(ordering,serif)→path` cache +
   `install_system_cjk_font_loader`. Link `dwrite`.
2. **Install:** wire `install_system_cjk_font_loader` into `Document::Impl()`.
3. **Prune flip:** add `TOFU_CJK` (keep `TOFU_CJK_LANG`), `_PRUNE_VER` → V10, update
   the assertion foreach in `ImportMuPDF.cmake`; delete the ExternalProject stamp;
   rebuild MuPDF + litepdf Release; **measure the exe size** (size-win gate, §9).
4. **CJK verification:** existing `test_document_search.cpp` CJK case green +
   `page_text` liveness; the three fixture liveness tests; CJK form-appearance
   liveness; regenerate reference PNGs + manual visual (incl. PMingLiU face check);
   GUI smoke.
5. **Size gates:** set `size_ceiling_bytes` (+ recomputed self-test synthetics) and
   `smoke-test.ps1 $maxBytes`/comment from the measured exe + headroom.
6. **Docs:** flip roadmap C-real/8 MB notes to DONE with the final size; record the
   CJK-needs-system-font + script-fallback known limitations.

## 9. Open Questions / Risks

- **Size win is measurement-gated (Codex M6):** `TOFU_CJK` removes the CJK entries
  from `font-table.h:281`, but the MuPDF resource project still lists the CJK font
  objects ([libresources.vcxproj:297-302](../../../third_party/mupdf/platform/win32/libresources.vcxproj)).
  The linker should drop the now-unreferenced archive members, but the ~7.5 MB is
  **not proven** until the step-3 Release link/size check. If it lands materially
  above 8 MB, investigate before setting the ceiling (no further font lever
  remains, so a surprise indicates something else grew).
- **Per-worker font read:** `fz_new_font_from_file` heap-reads the whole system
  font (a multi-MB TTC) per context; the §4.1 cache removes only the DirectWrite
  *enumeration* cost, not the per-context read. Confirm in the benchmark that a CJK
  page render is not materially slower. **Pre-decided fallback shape** (Opus NIT-1):
  if it regresses, read each font file once into a process-wide buffer keyed by
  path and hand it to `fz_new_font_from_buffer` via
  `fz_new_buffer_from_shared_data` ([font.c:856](../../../third_party/mupdf/source/fitz/font.c))
  so N worker-documents share one read instead of re-reading.
- **CI runner CJK fonts:** if `windows-2022` lacks a given CJK family, a future
  *render-asserting* CI check would tofu. Mitigated by liveness-only CI (no
  pixel/hash assertion on CJK) + the dev-machine manual-visual gate.

## 10. Review Changelog

### v3 → v4 (plan-gate round-2 corrected the appearance mechanism, 2026-06-21)

The plan-gate round-2 review (Opus) traced deeper than the two spec rounds and
found that the **"blank page" failure model was wrong** — a fact source-verified
here. A NULL CJK hook makes `fz_new_cjk_font` throw `FZ_ERROR_ARGUMENT`
(font.c:967), but during appearance synthesis the inner `fz_catch` at
pdf-appearance.c:3815 swallows it (rethrows only `REPAIRED`/`SYSTEM`) and the outer
catch at pdf-appearance.c:3842 sees no error in flight, so `pdf_update_appearance`
returns normally. The page is **not** blanked; the pixmap is **non-null**; only the
CJK form field is left empty. (Codex spec-round-1 asserted the blank-page model and
Opus spec-round-2 re-confirmed it — both stopped at the `fz_new_cjk_font` throw and
did not trace into the appearance catch. Lesson: reviewer consensus is not
verification.) Changes:

- **D6 reframed** from "prevents a blank page" to a **cheap never-NULL invariant**:
  its only real effect is rendering CJK **form fields** as tofu rather than empty on
  font-less machines. The base14 last-resort + its deterministic Task-2 unit test
  are **kept** (5 lines, OS-independent, unit-testable).
- **Form-appearance integration test + hand-built AcroForm fixture DROPPED**
  (§6) — it cannot fire the last-resort on CI (which has CJK fonts) and the no-D6
  failure is a benign empty field, not a blank page, so the machinery isn't worth it.
- §5(b) rewritten to the real (swallowed-throw) mechanism with the source citations.

### v2 → v3 (three-lens round-2, 2026-06-21)

Opus + Sonnet + Codex, delta-scoped to the v2 changes. **No defect in the core
approach;** D6 independently verified safe by both Opus and Codex. Folded in:

- **[Opus + Codex] D6 verified SAFE** — a non-CJK font tagged `cjk=1` renders
  `.notdef` tofu without breaking shaping, CID widths (`/W`/`/DW`), vertical
  writing (`/W2`/`/DW2`), or the appearance path (hardcoded `state->w=1`,
  pdf-appearance.c:1804; the `fz_new_cjk_font` result is a probe dropped at
  pdf-appearance.c:1673). base14 confirmed compiled in under V10. → D6 strengthened
  to specify **base14 Helvetica** as the last resort (OS-independent + unit-testable)
  and §5(b) notes the appearance path is not hail-mary-protected.
- **[Sonnet BLOCKER] Size-gate self-test rebase is bigger than assertion 8** — the
  timing assertions (1-5) hardcode ~18 MB `exe_bytes` that exceed a ~9 MB ceiling,
  flipping the "allowed" assertions 2/3/5 to failed. → §4.4 now requires rebasing
  **all** self-test synthetics.
- **[Sonnet MAJOR] D6 not actually tested** — added a deterministic unit test
  (bogus ordering → non-NULL last-resort, §6) and specified the CJK-form fixture's
  load-bearing properties (CJK `/V`, no `/AP`, non-embedded CID font) + the
  non-white-pixel assertion.
- **[Opus MINOR-1] Last-resort must swallow `FZ_ERROR_SYSTEM`/`TRYLATER`** —
  `fz_load_system_cjk_font` rethrows those two; §4.1 ABI-safety updated to catch
  them at the last-resort and return NULL.
- **[Opus m2 / Sonnet] Cache lock discipline** — §4.1 specifies the double-checked
  pattern (release the mutex across the DirectWrite enumeration; no `fz`-lock
  deadlock risk).
- Minors: `RenderEngine.cpp` cite corrected to 153-155; `smoke-test.ps1 $maxBytes`
  *value* change called out explicitly (not just the comment); `IDWriteLocalFontFileLoader`
  QI-result check before `GetFilePathFromKey`; §9 pre-decides the shared-buffer
  perf fallback; D5 probe-vs-render clarification.

### v1 → v2 (three-lens spec gate, 2026-06-21)

Opus (`pr-review-toolkit:code-reviewer`) + Sonnet (`feature-dev:code-reviewer`) +
Codex (`codex exec` read-only). No BLOCKER. Folded in:

- **[Codex M3 / Opus M1] Blank-page failure mode** — appearance synthesis calls
  `fz_new_cjk_font` directly (pdf-appearance.c:1670-1691) before content render;
  a NULL hook return throws `FZ_ERROR_ARGUMENT` (font.c:967) and litepdf's worker
  drops the display list (RenderEngine.cpp:149). → New **D6**: last-resort font so
  the hook never returns NULL; §5 rewritten to trace both throw sites and how D6
  closes the blank-page path.
- **[Codex M2] Second `TOFU_CJK_LANG` consult** (font.c:2124). → **D3** changed to
  *add* `TOFU_CJK` and *keep* `TOFU_CJK_LANG`, leaving that path byte-identical;
  §4.3 assertion *adds* `TOFU_CJK` rather than swapping.
- **[Codex M4] False caching claim** — `ctx->font->cjk[]` is only populated by
  `fz_new_cjk_font`, not the substitute path; per-worker documents
  (RenderEngine.cpp:283/323) re-resolve. → removed the false claim; added the
  process-wide `(ordering,serif)→path` resolution cache (§4.1) + §9 perf risk.
- **[Codex M5] Existing CJK search test** (test_document_search.cpp) becomes an
  automated extraction-regression gate (§6).
- **[Codex M1 / Sonnet C1 / Opus n2] CMake assertion** must add `TOFU_CJK` (§4.3).
- **[Opus M2] `fz_new_font_from_file` is a full-file read, not mmap** — D4 wording
  fixed; perf noted.
- **[Sonnet M3] Self-test synthetic bytes** must be recomputed against the new
  ceiling (§4.4).
- **[Sonnet C2 / Opus] `serif` flag semantics** clarified in D5 (hardcoded `serif=1`
  from `fz_new_cjk_font`; PDF-derived on the substitute path).
- **[Codex M6] Size win is measurement-gated** (libresources still lists the objs) — §9.
- Minors: GetReferenceKey pointer lifetime (Opus m4); verify TTC face = PMingLiU
  (Opus m3); DirectWrite needs no CoInitialize, contrast the print path (Opus m1);
  soften the SHARED-factory thread-safety to "free-threaded in practice" (Opus m2);
  bold/italic via `fake_bold` synthesis (Sonnet N3); smoke-test comment (Sonnet N4);
  pdf-interpret.c catch line cite corrected to 785-798 (Sonnet N2).
