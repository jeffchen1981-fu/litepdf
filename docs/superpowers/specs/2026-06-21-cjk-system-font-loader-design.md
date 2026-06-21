# CJK System-Font Loader — Reaching the 8 MB exe Target (Design Spec)

> **Status:** v1 — brainstormed 2026-06-21. Realizes the **"C-real"** path that
> [`2026-06-07-phase-11.5-size-prune-coldstart-design.md`](2026-06-07-phase-11.5-size-prune-coldstart-design.md)
> §7 scoped and deferred to post-v1.0 ("implement a Windows CJK system-font
> loader … then set full `TOFU_CJK` to drop all embedded CJK"). Also closes the
> original [`2026-04-15-litepdf-design.md`](../../plans/2026-04-15-litepdf-design.md)
> §3 **8 MB exe target**. Ready for `superpowers:writing-plans`.
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
switching the MuPDF prune from `TOFU_CJK_LANG` to **`TOFU_CJK`**, and recover CJK
rendering by installing a **Windows DirectWrite-backed system-font loader** via
`fz_install_load_system_font_funcs`.

- **Expected size:** exe 12.31 MB → **~7.5 MB**, hitting the 8 MB design target.
- **Verified rendering path:** with `TOFU_CJK` set, `fz_lookup_cjk_font` returns
  no data, so `fz_new_cjk_font` ([font.c:957](../../../third_party/mupdf/source/fitz/font.c))
  and `pdf_load_substitute_cjk_font` ([pdf-font.c:436](../../../third_party/mupdf/source/pdf/pdf-font.c))
  both call `fz_load_system_cjk_font(ctx, "SourceHanSerif", ordering, serif)`,
  which dispatches to our installed `load_cjk_font` hook
  ([font.c:458-476](../../../third_party/mupdf/source/fitz/font.c)).

**Non-goal:** this effort does not restore non-CJK script coverage (Arabic, Thai,
Hebrew, Devanagari, etc.), which `TOFU_NOTO` already drops in V9 and which ship
as tofu today in v1.1.0. It does not add a "download font" UX, and does not touch
cold-start or any other phase.

## 2. Scope

**In scope:**
- Flip `TOFU_CJK_LANG` → `TOFU_CJK` and bump the prune version (`cmake/ImportMuPDF.cmake`).
- New `src/core/SystemFonts.{hpp,cpp}` unit: a DirectWrite CJK font loader + a
  one-line install entry point.
- Install the hook once per `Document` base context (`src/core/Document.cpp`).
- Link `dwrite` into `litepdf_core` (`CMakeLists.txt`).
- Re-baseline the size gates (soft: measured + headroom).
- Re-verify CJK rendering (regenerate reference PNGs; liveness test).
- Update roadmap / known-limitations docs.

**Out of scope (explicit non-goals):**
- The `load_font` (named) and `load_fallback_font` (non-CJK script) hooks — the
  install call passes `nullptr` for both. Restoring non-CJK script coverage is a
  separate, independently-schedulable effort with zero size cost but a larger
  test surface; it is deliberately deferred.
- Any change to the set of `FZ_ENABLE_*` features.
- A user-facing "missing font" dialog.

## 3. Resolved Decisions (brainstorming, 2026-06-21)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Missing-font fallback strategy | **Fully remove embedded CJK; accept + document tofu on font-less machines** | Hitting ~7.5 MB requires dropping the embedded font entirely. The miss case is rare (standard Win10/11 ship JhengHei/YaHei/Yu Gothic/Malgun) and **non-fatal** (§5). This is exactly the Phase 11.5 §7 C-real intent. |
| D2 | Feature scope | **CJK-only** (`load_cjk_font` hook only) | The 8 MB blocker is the CJK font; non-CJK script coverage is an independent concern. YAGNI — keep the code + test surface minimal. |
| D3 | Size gate hardness | **Soft: ceiling = measured post-prune + ~1.5 MB headroom** | Consistent with the Phase 11.5 convention. A hard 8.0 MB gate leaves ~0.5 MB headroom and would turn any future small addition into a false regression. The gate's job is to catch re-bloat, not to approach the limit. |
| D4 | Loader implementation | **DirectWrite (Approach A), file-path memory model (Approach C simplification)** | DirectWrite is free-threaded (matches the render-worker call site), cleanly handles TTC collections (MingLiU is a `.ttc` with PMingLiU/MingLiU/HKSCS faces) and yields the correct face index, and is a natural sibling of the existing Direct2D stack. The file-path model (`fz_new_font_from_file`) lets MuPDF own the font mapping — the refcount-clean choice. GDI `GetFontData` was rejected: per-DC font realization + ambiguous TTC face-index handling are real pain points on Traditional-Chinese MingLiU. |
| D5 | CJK font style | **Honor MuPDF's `serif` flag** | `fz_new_cjk_font` requests `serif=1` ([font.c:957](../../../third_party/mupdf/source/fitz/font.c)) and the requested name is "SourceHanSerif". Mapping serif→明體/Mincho/Batang preserves that semantic; `serif=0` maps to the sans (黑體/Gothic/Malgun) families. |

## 4. Components

### 4.1 New unit — `src/core/SystemFonts.{hpp,cpp}`

**Public surface (one function):**

```cpp
namespace litepdf {
// Install the Windows DirectWrite-backed CJK system-font loader on `ctx`.
// Call once per Document base context, right after fz_register_document_handlers.
// fz_clone_context shares the refcounted fz_font_context (context.c:340), so
// every per-render escrow clone inherits the hook automatically — no per-clone
// install, no re-install on clone.
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

Candidates are tried in order. If the requested-style list yields nothing, the
loader falls through to the other style as a last resort before returning NULL.
Unknown `ordering` → NULL.

**DirectWrite resolution (Approach A + file-path memory model):**

1. A process-cached `IDWriteFactory` (`DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, …)`),
   created lazily under a `std::once_flag`. The SHARED factory is thread-safe and
   process-lifetime; DirectWrite does **not** require `CoInitialize`.
2. For each candidate family: `GetSystemFontCollection(&coll, FALSE)` →
   `coll->FindFamilyName(wname, &index, &exists)`; if `exists`,
   `GetFontFamily(index)` → `GetFirstMatchingFont(NORMAL, NORMAL, NORMAL)` →
   `CreateFontFace(&face)`.
3. `face->GetFiles(&n /*1*/, &fileRef)`; `fileRef->GetLoader(&loader)` +
   `fileRef->GetReferenceKey(&key, &keySize)`; QI `loader` →
   `IDWriteLocalFontFileLoader`; `GetFilePathFromKey(key, keySize, pathBuf, …)` →
   a `wchar_t` font-file path. `face->GetIndex()` → the TTC face index.
4. Convert the path to UTF-8 and call
   `fz_new_font_from_file(ctx, name, utf8_path, faceIndex, 0)` — MuPDF opens and
   owns the font file mapping (the Approach-C simplification).
5. A non-local file loader (essentially never for system fonts) → skip the
   candidate.

**ABI / exception safety (load-bearing):** the callback is a C function pointer
invoked from MuPDF's C frame. No C++ exception may cross that boundary:
- The whole body is wrapped `try { … } catch (...) { return nullptr; }` to absorb
  any `std::`/COM-wrapper throw.
- The `fz_new_font_from_file` call is wrapped in `fz_try/fz_catch`, returning
  `nullptr` on MuPDF error (without rethrowing).
- Returning `nullptr` is the documented "no font found" signal
  ([font.h:267](../../../third_party/mupdf/include/mupdf/fitz/font.h)).

**Thread-safety:** the callback runs on render-worker threads. The only shared
state is the once-initialized SHARED factory; `GetSystemFontCollection` returns a
fresh snapshot and is safe to call concurrently. No litepdf-side mutable state.

**COM lifetime:** reuse the existing `PdfCanvas` ComPtr convention (RAII) for the
per-call interface pointers. The cached factory is a Meyers-style
function-local singleton (leak-on-exit acceptable for a process-lifetime
resource).

### 4.2 Install point — `src/core/Document.cpp`

One line in `Document::Impl()`, immediately after
[`fz_register_document_handlers(ctx)` (Document.cpp:86)](../../../src/core/Document.cpp):

```cpp
fz_register_document_handlers(ctx);
litepdf::install_system_cjk_font_loader(ctx);   // new
```

The loader lives in `litepdf_core`, so both the GUI and `litepdf-cli`
(benchmark/fixtures, which use `core::Document`) get CJK. Because the font
context is shared across `fz_clone_context` clones, this single install covers
every per-render escrow context.

### 4.3 Build config — `cmake/ImportMuPDF.cmake` + `CMakeLists.txt`

- `cmake/ImportMuPDF.cmake`: in the prune define block, replace `TOFU_CJK_LANG`
  with `TOFU_CJK`; bump `_PRUNE_VER` `LITEPDF_PRUNE_V9` → `LITEPDF_PRUNE_V10`. The
  version bump triggers the existing idempotent re-patch path: restore `config.h`
  to pristine, re-prepend the current define set, and delete the ExternalProject
  build stamp so MuPDF actually rebuilds (the spec's mechanism — without the stamp
  delete the rebuild silently no-ops and the size measurement lies).
- `CMakeLists.txt`: add `dwrite` to `litepdf_core`'s `target_link_libraries`
  (PRIVATE — it is litepdf code calling DirectWrite).

### 4.4 Size gates (soft, measured + headroom — D3)

- `benchmarks/thresholds.json`: `size_ceiling_bytes` 19,000,000 → the measured
  post-prune exe + ~1.5 MB headroom (expected ≈ 9,000,000; final value set from
  the real Release build).
- `scripts/check-benchmark-regression.ps1`: the `-SelfTest` 8th assertion
  ("exe above size_ceiling_bytes blocks") is coupled to the ceiling value — update
  its fixture so the self-test still asserts 8/8. Any new PowerShell stays
  5.1-parse-safe (no `?.`/`??`/ternary).
- `scripts/smoke-test.ps1`: `$maxBytes` 25 MB → ~12 MB.

The exact gate numbers are intentionally **not** hard-coded in this spec; the
implementation plan sets them from the actual post-prune Release build.

## 5. Error Handling (fail-soft to tofu, never crash)

The failure path is traced and verified non-fatal:

1. Our callback returns NULL (no matching system font).
2. `pdf_load_substitute_cjk_font` then calls `fz_lookup_cjk_font`, which returns
   NULL under `TOFU_CJK`, so it `fz_throw(FZ_ERROR_SYNTAX, "cannot find builtin
   CJK font")` ([pdf-font.c:445](../../../third_party/mupdf/source/pdf/pdf-font.c)).
3. That throw is **caught** at `pdf_try_load_font`
   ([pdf-interpret.c:783-801](../../../third_party/mupdf/source/pdf/pdf-interpret.c)):
   not TRYLATER/SYSTEM → `fz_report_error` (warn) + `desc = NULL`, then
   `desc = pdf_load_hail_mary_font(...)` substitutes the base14 Helvetica
   hail-mary.

**Result:** the page renders fully; only the CJK runs become `.notdef` tofu boxes
(the hail-mary has no CJK glyphs). No blank page, no render abort, no crash.
Latin text, graphics, and images are unaffected. This is the documented,
accepted limitation of D1.

Other cases:
- COM/HRESULT failure mid-resolution → treat as "candidate not found", try the
  next candidate, ultimately NULL → same hail-mary path.
- No `FZ_ENABLE_*` link-breakage risk: this flips a `TOFU_*` macro and adds
  `dwrite.lib`; it disables no feature path.

## 6. Testing Strategy

- **Unit (TDD red-green core):** `cjk_family_candidates(ordering, serif)` returns
  the expected ordered family list for all 8 `(ordering × serif)` pairs, and NULL
  for an unknown ordering. Pure function, no OS dependency — the DirectWrite glue
  is thin I/O that the fixtures exercise.
- **Liveness:** open each of `tests/fixtures/cjk-zh-hant.pdf`, `cjk-ja.pdf`,
  `cjk-ko.pdf`, render page 1 → non-blank pixmap, no throw/crash. Passes
  regardless of glyph fidelity, so the windows-2022 CI runner stays green even if
  its installed CJK font set differs from the dev machine.
- **Manual visual (the real CJK gate, per Phase 11.5 §4.3):** on the zh-TW dev
  machine, **regenerate `tests/fixtures/cjk-reference/*.png`** against the new
  system-font render (PMingLiU / SimSun / MS Mincho / Batang), eyeball real glyphs
  vs tofu, re-commit as the new baseline; attach before/after to the PR. The old
  reference PNGs were rendered against Droid Sans Fallback and are now stale.
- **Determinism gate unaffected:** `scripts/generate-cjk-fixture.py --check`
  validates PDF-bytes determinism (reportlab/zlib), not rendering — the fixtures
  don't embed fonts, so it stays green. **Do not** add a render-hash gate for CJK:
  system-font output is not byte-reproducible across machines/Windows versions.
- **GUI smoke:** open the zh-Hant fixture in the GUI, screenshot, confirm real
  glyphs (per `reference_litepdf_scripted_gui_smoke`).
- **No perf regression:** full ctest suite + `large.pdf` benchmark. CJK
  resolution is one-time-per-ROS, cached in `ctx->font->cjk[ordering]`.

## 7. What This Effort Does NOT Do

- Restore non-CJK script coverage (Arabic/Thai/Hebrew/Devanagari…) — `TOFU_NOTO`
  keeps those as tofu, as in v1.1.0. A future effort can add the `load_font` /
  `load_fallback_font` hooks (zero size cost) if desired.
- Add a missing-font UX.
- Touch cold-start, the benchmark harness internals, or any unrelated MuPDF flag.

## 8. Build Sequence (for the implementation plan)

1. **Loader unit (TDD):** write `cjk_family_candidates` red-green; then the
   DirectWrite resolution + `install_system_cjk_font_loader`. Link `dwrite`.
2. **Install:** wire `install_system_cjk_font_loader` into `Document::Impl()`.
3. **Prune flip:** `TOFU_CJK_LANG` → `TOFU_CJK`, `_PRUNE_VER` → V10 in
   `ImportMuPDF.cmake`; delete the ExternalProject stamp; rebuild MuPDF + litepdf
   Release; **measure the exe size**.
4. **CJK verification:** liveness test on the three fixtures; regenerate the
   reference PNGs on the dev machine; manual visual confirm; GUI smoke screenshot.
5. **Size gates:** set `size_ceiling_bytes` (+ self-test 8th assertion) and
   `smoke-test.ps1 $maxBytes` from the measured exe + headroom.
6. **Docs:** flip roadmap C-real/8 MB notes to DONE with the final size; record
   the CJK-needs-system-font known limitation.

## 9. Open Questions / Risks

- **CI runner CJK fonts:** if `windows-2022` lacks a given CJK family, a future
  *render-asserting* CI check would tofu. Mitigated by keeping CI to liveness-only
  (no pixel/hash assertion on CJK) and the dev-machine manual-visual gate.
- **Final size:** ~7.5 MB is projected from the 4.84 MB font `.obj`; the real
  number is measured in step 3 and drives the gate values. If it lands materially
  above 8 MB, investigate before setting the ceiling (no further font lever
  remains, so a surprise would indicate something else grew).
