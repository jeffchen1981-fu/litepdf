# Search Upgrade (MuPDF 1.27 Stage 3) — Design

**Status:** Approved (brainstorming) — 2026-06-16; **revised v2 after 3-lens spec review** (Opus + Sonnet + Codex, all three independently source-verified against the bundled MuPDF 1.27.2 tree)
**Target release:** v1.1.0 (first post-v1.0 feature)
**Predecessors:** [`2026-06-08-mupdf-1.27-upgrade-design.md`](2026-06-08-mupdf-1.27-upgrade-design.md) (Stage 1/2 — shipped),
[`2026-06-08-mupdf-1.27-spike-findings.md`](../plans/2026-06-08-mupdf-1.27-spike-findings.md) (API spike)

## 1. Context

LitePDF shipped v1.0.0 on the pruned MuPDF 1.27.2 build. The whole point of the
1.27 upgrade (Stage 1/2) was to unlock the experimental case/regex-aware text
matcher so the four documented search limitations can be cleared. Stage 3 is that
activation.

Today `Document::page_hits` ([src/core/Document.cpp:466](../../../src/core/Document.cpp))
calls `fz_search_page`, which is unconditionally case-insensitive, literal-only,
and uncancellable. `SearchFlags { bool match_case; }` and the `abort_flag`
parameter are accepted but ignored (`(void)flags; (void)abort_flag;`). The
FindBar already renders a case-sensitivity latching toggle (`case_pressed`,
[src/ui/FindBar.cpp:171](../../../src/ui/FindBar.cpp)) that currently does
nothing functionally — a visibly broken control.

The flags already thread end-to-end: `FindBar.on_query_changed(text, case)` →
`MainWindow::on_find_query` → `SearchSession::set_query(q, Flags{mc})` → workers
call `Document::page_hits(page, needle, SearchFlags, abort_flag)`. Cross-tab
search uses the same core via `CrossTabSearch::dispatch(q, flags, snapshot)`.

## 2. Goals

Deliver all four capabilities as one cohesive search upgrade:

1. **Case-sensitive search** — make the existing FindBar toggle real
   (`FZ_SEARCH_EXACT` vs `FZ_SEARCH_IGNORE_CASE`).
2. **Mid-page cancellation** — honor `abort_flag` so an in-progress per-page
   scan aborts promptly when the query changes or a tab closes.
3. **Regex search** — `FZ_SEARCH_REGEXP`, surfaced as a new FindBar/ResultsPanel
   toggle, Enter-to-run.
4. **Whole-word search** — surfaced as a new toggle, implemented via regex
   word-boundary wrapping.

Both single-document (FindBar) and cross-tab (ResultsPanel) search get all four,
for consistency.

## 3. Non-goals

- No new search-result UI beyond the toggle buttons (no centered snippet, no
  match-index "m / n" counter — those remain in the post-v1.0 backlog).
- No diacritic-insensitive search (`FZ_SEARCH_IGNORE_DIACRITICS`) — out of scope.
- No regex replace / no search history.
- No change to the `ResultsPanel` docking model.

## 4. Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Scope = all four capabilities, single effort, v1.1.0 | Case/regex/whole-word are what users want; folding mid-page cancel in now avoids rewriting `page_hits` twice. |
| D2 | Regex execution is **Enter-to-run**; literal/case/whole-word stay **live** (search-as-you-type). Exact Enter semantics defined in §5.3. | A half-typed regex (`foo(`) is invalid and a full-document regex scan per keystroke is expensive. Whole-word, though regex-backed internally, stays live (the Enter gate binds to the user-facing regex toggle only). Enter already means "next match" in the FindBar (FindBar.cpp:447-449), so the dual meaning must be disambiguated — see §5.3. |
| D3 | Whole-word and regex are **combinable** (VS Code semantics) | Whole-word wraps the (escaped literal or raw regex) pattern in `\b…\b`; both toggles can be on at once. When both are on, the Enter gate still applies (the regex toggle governs Enter regardless of whole-word). |
| D4 | Whole-word implemented via `FZ_SEARCH_REGEXP` + `\b…\b` wrapping | Reuses the regex engine. `\b` IS supported by the bundled mujs regex engine (verified: `thirdparty/mujs/regexp.c`), so the R1 post-filter fallback is a contingency, not the expected path. |
| D5 | **`page_hits` uses the incremental `fz_search` loop** (`fz_new_search` → `fz_feed_search` → `fz_search_forwards`), explicitly copying each match's quads into the existing capped collector (see §5.1). `fz_match_stext_page` is the documented fallback. **Plan task 1 verifies the incremental-loop collector/truncation parity** against the old `fz_search_page` output. | One path supports case + regex + cancellation. `fz_match_stext_page_cb`'s callback-abort is a proven no-op in 1.27.2 (spike test 3). The spike verified the `marks[]/quads[]/kMaxQuads` collector contract ONLY on `fz_match_stext_page` (via `oldsearch_cb`), NOT on the incremental loop — so the incremental loop's collector parity is a verification task, not an assumption. |
| D6 | Cross-tab search gets the same flags + ResultsPanel toggles | Core `page_hits` is shared; one change benefits both. Inconsistent search behavior across the same app would confuse users. New cross-tab toggles default **off** (preserving today's case-insensitive literal behavior). |
| D7 | `\b` whole-word is correct for **ASCII word characters only**; documented known-limitation, not special-cased | mujs `\b` uses `[A-Za-z0-9_]` word-ness (`mujs/regexp.c` `iswordchar`). So whole-word is best-effort for accented Latin (`café`), Cyrillic/Greek, and CJK (no inter-character word breaks). Acceptable for v1.1.0; documented. |

## 5. Component changes

### 5.1 Core — `SearchFlags` + `Document::page_hits`
- Extend `SearchFlags` to `{ bool match_case; bool whole_word; bool regex; }`
  (header at [src/core/Document.hpp:95](../../../src/core/Document.hpp); update
  the stale "no-op on 1.24.11" comment).
- **Empty-query short-circuit runs on the RAW user input, BEFORE the needle
  transform.** If the raw query is empty, return immediately (clear / scan
  complete) — the `\b…\b` wrapping is never applied to an empty string (else
  whole-word + empty → `\b\b`, a valid regex matching every word boundary).
- Map flags → `fz_search_options` (enum values confirmed in
  `structured-text.h`: `EXACT=0`, `IGNORE_CASE=1`, `REGEXP=4`):
  - base = `match_case ? FZ_SEARCH_EXACT : FZ_SEARCH_IGNORE_CASE` (use the named
    constants; `EXACT`'s value of 0 contributes no bits to the OR but the code
    must not rely on the literal 0).
  - `| FZ_SEARCH_REGEXP` when `regex || whole_word`. Combined values are valid
    and engine-supported (case-insensitive regex = `IGNORE_CASE | REGEXP`,
    handled by `split_options`).
- Needle transform (pure function, unit-tested), applied only to a non-empty raw
  query:
  - plain: needle as-is.
  - whole_word & !regex: `\b` + regex-escape(needle) + `\b`.
  - whole_word & regex: `\b(?:` + needle + `)\b`.
  - regex & !whole_word: needle as-is (regex).
- **Regex flavor** is mujs / ECMAScript-style (`stext-search.c` includes
  `mujs/regexp.h`), NOT PCRE: character classes, alternation, quantifiers,
  `\d`/`\w`/`\b` work; PCRE-isms (lookbehind, named groups, `\p{…}`) do not.
  The haystack is space-normalized (newlines/tabs/NBSP → space) before matching
  — regex authors should treat the page as one space-joined run.
- **Matcher body — incremental loop with explicit collector copy** (D5):
  extract `fz_stext_page` via `fz_new_stext_page_from_page`, then run
  `fz_new_search` → `fz_feed_search` → `fz_search_forwards`, checking
  `abort_flag` between matches. Each `FZ_SEARCH_MATCH` yields
  `fz_search_result_details` with `num_quads ≥ 1` (a hit may span lines); copy
  each `details->quads[i].quad` into the existing `quads[]/marks[]` arrays,
  breaking at `kMaxQuads` so `n == kMaxQuads` still means "tail dropped"
  (truncation semantics unchanged). One `marks[]` group per hit, matching the
  old `fz_search_page` output — **verified by plan task 1 against existing
  fixtures.**
- **Refcount discipline** (`fz_feed_search` TAKES OWNERSHIP of the stext page it
  is handed — `structured-text.h` ~L1052, dropped by `fz_drop_search`). To keep
  cleanup uniform and balanced: feed `fz_keep_stext_page(ctx, stext)` (the search
  gets its own ref), keep the caller's own ref, and in `fz_always` drop BOTH the
  caller's `fz_stext_page` AND the `fz_search` object (`fz_drop_search`). Declare
  both with `fz_var` so the invalid-regex throw path still cleans up. Follow the
  `mupdf-refcount-conventions` discipline; a leak/refcount test (including the
  cancel-mid-loop and invalid-regex-throw paths) guards this.
- Invalid regex: `fz_search_set_options` → `init_regexp` calls `js_regcomp`,
  which throws `fz_throw(FZ_ERROR_ARGUMENT, "regcomp failure")` on a bad pattern
  (verified in `stext-search.c`). The existing `fz_try/catch` in `page_hits`
  catches it; surface a distinct "invalid pattern" outcome (not an empty result)
  so the UI can show the error state. Mechanism (return type / sentinel)
  finalized in the plan.

### 5.2 Plumbing — `SearchSession` + `CrossTabSearch`
- `SearchSession::Flags` ([src/core/SearchSession.hpp:34](../../../src/core/SearchSession.hpp))
  gains `whole_word`, `regex`. `set_query` already takes `Flags`.
- Only the `CrossTabSearch::dispatch(q, flags, snapshot)` boundary is unchanged
  (it already takes `Flags`). The UPSTREAM call sites that build the `Flags` do
  NOT carry the new bits today and MUST be widened — the compiler will NOT catch
  a default-`false` field that silently disables a feature:
  - [src/ui/FindBar.cpp](../../../src/ui/FindBar.cpp) `QueryChanged` callback →
    `MainWindow::on_find_query_changed` (~MainWindow.cpp:1687) → `set_query(q, {…})`.
  - [src/ui/ResultsPanel.cpp](../../../src/ui/ResultsPanel.cpp) `OnQuerySubmit`
    (carries text only today) → `MainWindow::on_results_query` (~1809) →
    `MainWindow::on_cross_tab_search` (~1824-1828), which currently hard-codes
    `Flags f{}`. This MUST be rebuilt from the ResultsPanel toggle state.

### 5.3 UI — FindBar
- Add two latching toggle buttons next to the existing case toggle:
  **`.*`** (regex, tooltip `"Regular expression (Enter to search)"`) and
  **`W`** (whole word, tooltip `"Whole word"`). Same owner-draw/latching
  pattern as the case toggle. Tooltip strings are English (project rule).
- **Enter semantics** (resolves the conflict with the existing
  Enter=next-match / Shift+Enter=prev binding at FindBar.cpp:447-449):
  - Regex toggle **off**: unchanged — query is live (debounced), Enter = next
    match, Shift+Enter = previous.
  - Regex toggle **on**: typing does NOT auto-run. A "dirty" flag is set on each
    edit. Enter when dirty = compile + run the regex (clears dirty); Enter when
    not dirty = next match; Shift+Enter = previous (when not dirty). F3 /
    Shift+F3 navigation is unaffected.
  - This Enter gate is governed by the regex toggle only; whole-word being on
    (with regex off) stays live.
- Invalid regex (on run): red border + "Invalid pattern" affordance; existing
  results are NOT cleared.
- `on_query_changed` callback widens from `(text, case)` to carry the three
  flags (a `SearchFlags`-shaped struct).
- **Toggle persistence:** the three toggles latch for the session and are shared
  app-wide (one search-options state), surviving find-bar hide/show and document
  switches; reset to defaults (all off) on app restart (no on-disk config — see
  the installer-encoding note that litepdf writes no settings file).

### 5.4 UI — ResultsPanel (cross-tab)
- Add the same three toggles to the ResultsPanel query row, defaulting **off**
  (preserves today's case-insensitive literal cross-tab behavior; no silent
  shift for existing users). Same Enter-to-run-regex semantics as §5.3.
- Required signature changes (see §5.2): widen `ResultsPanel::OnQuerySubmit` to
  carry flags, widen `MainWindow::on_results_query` to accept them, and rebuild
  the `Flags` in `MainWindow::on_cross_tab_search` from the toggle state before
  `cross_tab_->dispatch(q, flags, …)`.

## 6. Data flow (unchanged shape, widened payload)

```
FindBar toggles + Enter ─┐
                         ├─► SearchFlags{case,word,regex} ─► SearchSession::set_query
ResultsPanel toggles ────┘                                 └─► CrossTabSearch::dispatch
                                   │
                                   ▼
                 Document::page_hits(page, needle, flags, abort_flag)
                   → needle transform → fz_search_options
                   → incremental fz_search loop (abort_flag checked) → PageHit[]
```

## 7. Error handling

- **Invalid regex:** caught at `page_hits` (`fz_throw(FZ_ERROR_ARGUMENT)`),
  reported as an "invalid pattern" state; UI shows the error and keeps prior
  results.
- **Empty needle:** the empty-query short-circuit runs on the RAW input BEFORE
  the needle transform (§5.1), so `\b…\b` is never built from an empty string.
- **Truncation** (`n == kMaxQuads == 256`): unchanged tail-drop behavior.
- **Abort mid-scan:** `abort_flag` is checked between matches in the incremental
  loop; partial hits collected so far are discarded by the existing epoch/cancel
  machinery in `SearchSession`. **Granularity is per-match, not per-instruction:**
  a single catastrophic-backtracking regex (e.g. `(a+)+$`) inside one
  `fz_search_forwards` call cannot be interrupted and runs under `doc_mutex`.
  Documented as a known limitation (R6); not mitigated in v1.1.0.

## 8. Testing (TDD)

Core unit tests (extend the existing `[search]` suite, reuse current fixtures):
- flag → `fz_search_options` mapping: assert the **exact integer** produced for
  all 8 combinations (e.g. `{false,false,false}`→1, `{true,false,false}`→0,
  `{false,false,true}`→5, `{true,true,false}`→4, …), to catch a mapping typo.
- needle transform: literal escaping, `\b…\b` wrapping, regex wrapping; and that
  an empty raw query short-circuits BEFORE producing `\b\b`.
- case sensitivity: `EXACT` "Lorem" finds 0 where "lorem" exists; `IGNORE_CASE`
  finds it. Convert the current `[!shouldfail]` case tripwire into a normal
  passing test (note the suite-count delta in the plan, as the predecessor did).
- regex: a pattern hits expected count; an invalid pattern yields the
  invalid-state outcome (not a crash, not silent empty).
- whole-word: `\bcat\b` matches "cat" but not "category".
- **incremental-loop collector parity (plan task 1 + a locked regression test):**
  the incremental loop's `marks[]/quads[]` output and `n == kMaxQuads` truncation
  match the old `fz_search_page` output on the existing fixtures (incl. a
  multi-quad / line-spanning hit).
- mid-page cancel: a discriminating test proving the incremental loop stops
  early when `abort_flag` is set (per spike test 4). Use the `InlineDispatcher`
  to signal abort; the loop must not exit `doc_mutex` early.
- refcount/leak guard covering three paths: normal scan, **cancel mid-loop**,
  and **invalid-regex throw** (the path most likely to leak `fz_search`).

GUI behavior (regex Enter-to-run semantics, invalid-regex affordance, toggle
latching + persistence, cross-tab toggles) is verified by manual/scripted smoke,
not unit tests.

## 9. Risks / early verification

| # | Risk | Mitigation |
|---|------|-----------|
| R1 (downgraded after source verification) | Regex flavor / `\b` / invalid-signal were "unverified" in v1, but the bundled source answers them: flavor = mujs/ECMAScript (not PCRE), `\b` IS supported, invalid patterns `fz_throw(FZ_ERROR_ARGUMENT)`. | Plan task 1 is a **thin confirmation test** (one good + one bad pattern, assert hit-count and the invalid-state outcome) folded into the collector-parity spike — NOT a full investigative spike. The `\b`-post-filter fallback is now a remote contingency. |
| R2 | Explicit `fz_new_stext_page_from_page` per `page_hits` may cost more than `fz_search_page`'s internal extraction. | Confirm with the benchmark harness's search-latency metric; if material, cache/reuse stext within a scan. |
| R3 | `\b` whole-word is correct only for ASCII word characters; accented Latin (`café`), Cyrillic/Greek, and CJK are best-effort. | Documented known-limitation (D7). No code mitigation in v1.1.0. |
| R4 | Widening the flag plumbing touches multiple call sites, and a default-`false` field can silently disable a feature (compiler won't catch). | Enumerated in §5.2/§5.4 (FindBar callback, ResultsPanel `OnQuerySubmit`, `on_results_query`, `on_cross_tab_search` `Flags{}`). Covered by behavior tests asserting the new flags reach `page_hits` for BOTH single-doc and cross-tab. |
| R5 | Live whole-word (regex-backed) on a large document has the same per-keystroke regex-engine cost as a live regex would. | Validate live whole-word latency with the benchmark harness; if p95 exceeds the bar, gate whole-word on Enter too (same as regex). |
| R6 | Catastrophic-backtracking regex (`(a+)+$`) runs uninterruptibly inside one `fz_search_forwards` call, holding `doc_mutex`. | Documented known-limitation; mid-page cancel granularity is per-match. Not mitigated in v1.1.0. |

## 10. Versioning

Per the project convention, VERSION bumps to `1.1.0` only at the release
boundary (the ship PR), not per task.
CHANGELOG gets an `## [Unreleased]` → `## [1.1.0-...]` entry at ship. Tag
`v1.1.0-...` on merge.
