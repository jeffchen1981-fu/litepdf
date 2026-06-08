# Phase 11.5 — BEFORE-prune baseline

Captured 2026-06-08 on branch `phase-11.5` at HEAD `7418a59` (Task 3 — cold-start
instrumentation wired, MuPDF **not yet pruned**). This is the comparison anchor for
the Task 5/6 prune and the Task 6 cold-start coupling test ("did dropping the
23.66 MB CJK font move `T1→T2`?").

**Environment:** local Windows 11, Release x64, VS BuildTools 17 (MSVC 19.44),
MuPDF 1.24.11 vendored, reportlab 4.4.10 fixtures.

## Binary size (the prune target)

| Metric | Value |
|--------|-------|
| `litepdf.exe` size | **39,613,440 bytes** (≈ 39.6 MB / 37.78 MiB) |

Goal: shrink to ~18–20 MB (Tasks 5–6). Source Han Serif (23.66 MB) is still
embedded at this baseline.

## Headless render (`litepdf-cli --benchmark`, 9 iterations)

Source: `build/before.json` (git_sha `7418a59`). The first sample is a cold-cache
outlier (34 ms large / 14 ms simple); `median_ms` is the comparison stat.

| Fixture | open_render_ms | median_ms | render_ms | open_ms | engine_init_ms |
|---------|---------------:|----------:|----------:|--------:|---------------:|
| large.pdf  | 7.7173 | 7.9802 | 6.6656 | 0.7618 | 0.2627 |
| simple.pdf | 9.0743 | 9.1364 | 7.6078 | 0.9173 | 0.3938 |

## GUI cold-start sub-spans (3 runs, `simple.pdf`, `--log-timings`)

All values in ms, measured as `T0→Tx`. `T1→T2` is derived (`T0→T2` − `T0→T1`).

| Run | T0→T1 | T0→T2 | T0→T3 | T0→T4 | d2d_ctor | open_start | open_done | view_built | **T1→T2** |
|----:|------:|------:|------:|------:|---------:|-----------:|----------:|-----------:|----------:|
| 1 | 63 | 254 | 282 | 283 | 14 | 248 | 250 | 252 | **191** |
| 2 | 66 | 249 | 277 | 279 | 15 | 245 | 247 | 249 | **183** |
| 3 | 59 | 236 | 267 | 269 | 14 | 232 | 234 | 236 | **177** |

Raw lines:

```
run 1: T0->T1=63ms T0->T2=254ms T0->T3=282ms T0->T4=283ms | d2d_ctor=14 open_start=248 open_done=250 view_built=252
run 2: T0->T1=66ms T0->T2=249ms T0->T3=277ms T0->T4=279ms | d2d_ctor=15 open_start=245 open_done=247 view_built=249
run 3: T0->T1=59ms T0->T2=236ms T0->T3=267ms T0->T4=269ms | d2d_ctor=14 open_start=232 open_done=234 view_built=236
```

### Where the `T1→T2` time goes (instrumentation read-out)

Median `T1→T2` ≈ **183 ms**. The sub-marks localize it:

- **T1 → open_start ≈ 183 ms** — the dominant cost is the gap between `ShowWindow`
  returning (T1) and the open worker thread entering (`open_start`): `std::thread`
  spin-up plus the UI message pump warming up. NOT file I/O.
- **open_start → open_done ≈ 2 ms** — `Document::open` is nearly free headless.
- **open_done → view_built ≈ 2 ms** — the `DocumentView` ctor is cheap.
- **view_built → T2 ≈ 0 ms** — the UI thread dequeues `WM_USER_OPEN_OK`
  immediately; no queue-pumping stall.

**Implication for the coupling test:** `T1→T2` is dominated by thread/pump warm-up,
not by anything the binary-size prune touches directly. If Task 6's 23.66 MB font
drop moves `T1→T2` at all, it would be via reduced page-fault time at process start
(a smaller exe faults in fewer pages), surfacing as a smaller `T0→T1` or
`T1→open_start` rather than a render change. The after-numbers go in
`phase-11.5-baseline-after.md` (Task 6 Step 3).
