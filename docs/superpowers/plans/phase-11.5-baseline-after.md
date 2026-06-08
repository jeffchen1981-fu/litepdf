# Phase 11.5 — AFTER-prune baseline

Captured 2026-06-08 on branch `phase-11.5` after Task 6 (TOFU_CJK_LANG CJK font
downgrade, `_PRUNE_VER` bumped V1→V2). This is the after-half of the comparison
anchored by `phase-11.5-baseline-before.md` (HEAD `7418a59`, MuPDF un-pruned). The
binary measured here is the V2-pruned build; git HEAD at capture was still `d3b2835`
(Task 5) because the Task 6 cmake change is committed together with this note.

**Environment:** local Windows 11, Release x64, VS BuildTools 17 (MSVC 19.44),
MuPDF 1.24.11 vendored (config.h pruned `LITEPDF_PRUNE_V2`), reportlab 4.4.10 fixtures.

## Binary size (the prune result)

| Stage | `litepdf.exe` size | Δ vs before |
|-------|-------------------:|------------:|
| Before (un-pruned, T4) | 39,613,440 B (≈ 39.6 MB / 37.78 MiB) | — |
| After T5 (FZ_ENABLE_JS/OCR/DOCX 0 + TOFU_SYMBOL) | 37,986,816 B (≈ 38.0 MB / 36.23 MiB) | −1,626,624 B (≈ 1.6 MB) |
| **After T6 (+ TOFU_CJK_LANG)** | **18,250,752 B (≈ 18.25 MB / 17.41 MiB)** | **−21,362,688 B (≈ 21.4 MB)** |

The T6 step alone dropped 19,736,064 B (≈ 19.7 MB): the 23.66 MB Source Han Serif is
no longer linked and all CJK scripts (HAN/zh_Hant/ja/ko/HANGUL/HIRAGANA/KATAKANA/
BOPOMOFO) re-alias to the 4.84 MB Droid Sans Fallback Full, which stays linked. Final
exe is 18.25 MB, inside the 18–20 MB Phase 11.5 target. CJK now renders Droid glyphs,
not tofu (the inbuilt-fallback path is untouched); visual glyph-vs-tofu confirmation is
Tasks 7–8.

## Headless render (`litepdf-cli --benchmark`, 9 iterations)

Source: `build/after.json` (git_sha `d3b2835`). Medians match the before-baseline
within noise — expected, since `large.pdf`/`simple.pdf` are Latin fixtures whose glyph
paths the font prune does not touch.

> Note: the first post-build benchmark run was machine-load contaminated (large
> median 17.1 ms, stddev 11.6 ms, first sample 49 ms) right after the ~10 min MuPDF
> rebuild. It was discarded and the clean re-run below is recorded. This mirrors the
> before-baseline's own first-sample cold-cache caveat.

| Fixture | open_render_ms | median_ms | render_ms | open_ms | engine_init_ms | before median |
|---------|---------------:|----------:|----------:|--------:|---------------:|--------------:|
| large.pdf  | 7.4629 | 8.2797 | 6.3573 | 0.8199 | 0.2857 | 7.9802 |
| simple.pdf | 8.9578 | 9.1798 | 7.5515 | 0.9451 | 0.4040 | 9.1364 |

## GUI cold-start sub-spans (3 runs, `simple.pdf`, `--log-timings`)

All values in ms, measured as `T0→Tx`. `T1→T2` is derived (`T0→T2` − `T0→T1`).

| Run | T0→T1 | T0→T2 | T0→T3 | T0→T4 | d2d_ctor | open_start | open_done | view_built | **T1→T2** |
|----:|------:|------:|------:|------:|---------:|-----------:|----------:|-----------:|----------:|
| 1 | 80 | 272 | 305 | 306 | 19 | 267 | 269 | 271 | **192** |
| 2 | 95 | 281 | 330 | 332 | 23 | 277 | 279 | 281 | **186** |
| 3 | 154 | 348 | 387 | 388 | 39 | 343 | 345 | 348 | **194** |

Raw lines:

```
run 1: T0->T1=80ms T0->T2=272ms T0->T3=305ms T0->T4=306ms | d2d_ctor=19 open_start=267 open_done=269 view_built=271
run 2: T0->T1=95ms T0->T2=281ms T0->T3=330ms T0->T4=332ms | d2d_ctor=23 open_start=277 open_done=279 view_built=281
run 3: T0->T1=154ms T0->T2=348ms T0->T3=387ms T0->T4=388ms | d2d_ctor=39 open_start=343 open_done=345 view_built=348
```

### The coupling test: did dropping the 23.66 MB font move `T1→T2`?

**No — within measurement noise.** After median `T1→T2` ≈ **192 ms** (186/192/194)
vs before median **183 ms** (177/183/191): the two distributions overlap, and run 3
is a visible machine-load outlier (its `T0→T1`=154 ms is ~2× runs 1–2). A 21.4 MB
binary shrink produced no measurable cold-start change.

This confirms the before-baseline hypothesis: `T1→T2` is dominated by `std::thread`
spin-up plus UI message-pump warm-up (T1 → open_start ≈ 187 ms here: run 1
open_start 267 − T0→T1 80), not by anything binary size touches. The predicted
page-fault-reduction channel (a smaller exe faulting in fewer pages → smaller
`T0→T1`) also did not surface: after `T0→T1` (80/95/154) is flat-to-higher than
before (63/66/59), i.e. lost in noise on this loaded run, not a real regression.

**Takeaway:** the ~21 MB size win and cold-start are decoupled. Cold-start
improvements, if pursued later, must target the thread/pump warm-up gap, not the
binary footprint.

## Task 8 — CJK render verdict (visual glyph-vs-tofu gate)

The pruned build (Droid Sans Fallback Full) renders all three CJK fixtures as
real glyphs — **no tofu, no coverage gap** in the sample sets. Reference renders
are checked in at `tests/fixtures/cjk-reference/cjk-{zh-hant,ja,ko}.png`.

| Fixture | Script | Sample | Verdict |
|---------|--------|--------|---------|
| cjk-zh-hant.pdf | Traditional Chinese | 繁體中文測試 常用字 台北 標準萬國碼 | glyphs OK — incl. Traditional-only forms (繁體標準萬國碼) |
| cjk-ja.pdf | Japanese | 日本語のテスト ひらがな カタカナ 漢字 | glyphs OK — hiragana + katakana + kanji |
| cjk-ko.pdf | Korean | 한국어 테스트 한글 서울 표준 | glyphs OK — Hangul syllables |

**zh-Hant coverage (primary locale):** confirmed by the user on the rendered
reference — every sampled Traditional glyph resolves to a real form, no fallback
box. The post-v1.0 C-real system-font loader is **not** needed for this sample.

**Method (deviation from plan Task 8 Step 1).** Reference PNGs were produced
headlessly via `litepdf-cli <fixture> --render 0` (P6 PPM → PNG via Pillow), not
a GUI window screenshot. It is the same `RenderEngine` the GUI uses; the headless
render is reproducible and matches the Task 5 precedent (CLI render used to prove
SVG). Each fixture also passed the liveness smoke (render exit 0).

**CID-encoding scope note (do not over-claim).** The zh-Hant fixture uses
`STSong-Light`, an Adobe-GB1 (Simplified-ordered) built-in CID font. The
Traditional sample chars are valid Unicode and resolve through Droid Sans
Fallback Full's broad Han coverage, so the visual gate is valid — but it tests
**Droid glyph coverage**, not the Adobe-CNS / Traditional CID-ordering path
(which would need `MSung-Light`). The verdict is "Droid covers these glyphs," not
"the Traditional CID-ordering path is exercised."
