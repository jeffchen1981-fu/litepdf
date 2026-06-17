# Search Upgrade (MuPDF 1.27 Stage 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Activate MuPDF 1.27.2's case/regex-aware text matcher so litepdf gains case-sensitive, regex, whole-word, and mid-page-cancellable search in both the FindBar and the cross-tab ResultsPanel, for v1.1.0.

**Architecture:** Rewrite `Document::page_hits` to use MuPDF's incremental `fz_new_search`/`fz_feed_search`/`fz_search_forwards` loop (case + regex + cancel in one path), driven by a widened `SearchFlags {match_case, whole_word, regex}`. Two pure helper functions (options-int + needle-transform) carry the flag logic and are unit-tested in isolation. The widened flags thread through the existing `SearchSession::Flags` / `CrossTabSearch::dispatch` plumbing; the FindBar and ResultsPanel each gain two/three new latching toggle buttons, with regex search gated on Enter. Each UI task bundles its MainWindow call-site change so every commit compiles.

**Tech Stack:** C++17, MuPDF 1.27.2 (`fz_*` C API), Win32 (owner-draw buttons), Catch2 unit tests, CMake/ctest.

**Spec:** [docs/superpowers/specs/2026-06-16-search-upgrade-design.md](../specs/2026-06-16-search-upgrade-design.md)

**This plan was revised after a 3-lens plan-gate review** (Opus + Sonnet + Codex). Key corrections folded in: tasks merged so each commit compiles; `FZ_STEXT_DEHYPHENATE` parity; explicit Task-2 replacement boundary; wide→utf8 routed through `SearchSession`; all FindBar/ResultsPanel owner-draw touch-points enumerated.

**Build/test commands (this project — memorize):**
- Build (Release; tests link MuPDF MT_StaticRelease so Debug fails with LNK2038):
  `& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release --target litepdf_unit_tests`
  (app target: `litepdf`)
- Run tests: `ctest --test-dir build -C Release --output-on-failure` (sequential), or `build\tests\Release\litepdf_unit_tests.exe "<Catch2 tag/name>"`.
- **Baseline suite (main): 208 cases (207 pass + 1 `[!shouldfail]`).** Cumulative after this plan: +3 (Task 1 `[query]`) +5 (Task 2 `[search]`) and the `[!shouldfail]` flips to a normal pass → **216 cases, all pass**. Verify the exact number you observe at each commit; the deltas above are the contract.

**Scripted GUI smoke harness (Task 6):** drive the GUI from PowerShell via the window handle. Gotchas proven this session: use `MainWindowHandle` (FindWindow can miss across the tool desktop); foreground the window then `SendInput` with a **correctly-aligned x64 `INPUT` union** (a flat struct misaligns `wVk` on x64 — define `INPUT{uint type; union{MOUSEINPUT;KEYBDINPUT}}` with the 4-byte pad); observe state via `%LOCALAPPDATA%\LitePDF\session.json` (per-tab page/zoom). `LITEPDF_NO_RESTORE=1` suppresses the restore prompt.

**Commit convention:** Conventional-commit subject; the CLAUDE.md `Co-Authored-By` trailer is added by the executor on every commit (omitted from examples below).

---

## File structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/core/SearchQuery.hpp` / `.cpp` | Pure flag→options-int + needle-transform + regex-escape helpers. MuPDF-value-free header. | **Create** |
| `src/core/Document.hpp` / `.cpp` | `SearchFlags` widened (3 bools); `page_hits` rewritten to the incremental loop; add `query_compiles`. | Modify |
| `src/core/SearchSession.hpp` / `.cpp` | `Flags` widened; threaded to `page_hits`; add `query_compiles(wstring, Flags)`. | Modify |
| `src/ui/FindBar.hpp` / `.cpp` | Two toggle buttons; widened `QueryChanged`; Enter-to-run regex; invalid affordance; persistence. | Modify |
| `src/ui/ResultsPanel.hpp` / `.cpp` | Three toggle buttons; widened `OnQuerySubmit`. | Modify |
| `src/ui/MainWindow.hpp` / `.cpp` | Build `Flags` from FindBar + ResultsPanel toggles; validate regex; widen handler signatures. | Modify |
| `tests/unit/test_search_query.cpp` | Unit tests for the pure helpers. | **Create** |
| existing search test file (grep `TEST_CASE("...","[document][search]")`) | parity, case/regex/whole-word/cancel/leak; flip `[!shouldfail]`. | Modify |
| `CHANGELOG.md`, `docs/plans/2026-04-15-litepdf-roadmap.md` | `[Unreleased]` entry; known-limitations (R3/R6). | Modify (final task) |

---

## Task 0: Verification spike — incremental-loop parity + regex behavior

Throwaway spike (spec D5 / R1). Confirms the single-page `fz_search_forwards` loop fills the quad collector **identically to `fz_search_page`** (same `FZ_STEXT_DEHYPHENATE` extraction), and that regex/`\b`/invalid-regex behave as the spec claims. Deleted at the end; its confirmed loop becomes Task 2.

**Files:** Create (throwaway) `tests/unit/spike_search127.cpp`; reference `third_party/mupdf/include/mupdf/fitz/structured-text.h` (912-1062), `tests/fixtures/search.pdf`, `tests/fixtures/large.pdf`.

- [ ] **Step 1: Register the spike file** — add `tests/unit/spike_search127.cpp` to the `litepdf_unit_tests` sources in `tests/CMakeLists.txt` (mirror a sibling `test_*.cpp` entry).

- [ ] **Step 2: Write the spike**

```cpp
// THROWAWAY spike (Task 0) — deleted after the loop protocol is confirmed.
#include <catch2/catch_test_macros.hpp>
#include <mupdf/fitz.h>
#include <filesystem>

namespace {
struct Doc {
    fz_context* ctx; fz_document* doc;
    Doc(const char* p){ ctx=fz_new_context(nullptr,nullptr,FZ_STORE_DEFAULT);
        fz_register_document_handlers(ctx); doc=fz_open_document(ctx,p); }
    ~Doc(){ if(doc) fz_drop_document(ctx,doc); if(ctx) fz_drop_context(ctx); }
};
int legacy_quads(fz_context* ctx, fz_document* doc, int page, const char* needle,
                 fz_quad* out, int max) {
    fz_page* pg = fz_load_page(ctx, doc, page);
    int marks[512] = {};
    int n = fz_search_page(ctx, pg, needle, marks, out, max);
    fz_drop_page(ctx, pg); return n;
}
// Candidate single-page incremental loop. Returns count; *threw on bad regex.
int incr_quads(fz_context* ctx, fz_document* doc, int page, const char* needle,
               fz_search_options opts, fz_quad* out, int max, int abort_after, bool* threw) {
    *threw=false; fz_stext_page* stext=nullptr; fz_search* search=nullptr; int n=0;
    fz_var(stext); fz_var(search); fz_var(n);
    fz_try(ctx) {
        fz_stext_options so = { FZ_STEXT_DEHYPHENATE };  // MATCH fz_search_page (util.c)
        stext = fz_new_stext_page_from_page_number(ctx, doc, page, &so);
        search = fz_new_search(ctx);
        fz_search_set_options(ctx, search, opts, needle);   // throws on bad regex
        bool fed=false;
        for (;;) {
            if (abort_after>=0 && n>=abort_after) break;
            fz_search_result r = fz_search_forwards(ctx, search);
            if (r.reason == FZ_SEARCH_MORE_INPUT) {
                fz_feed_search(ctx, search, fed ? nullptr : fz_keep_stext_page(ctx, stext),
                               r.u.more_input.seq_needed);
                fed = true;
            } else if (r.reason == FZ_SEARCH_MATCH) {
                fz_search_result_details* d = r.u.match.result;
                for (int i=0;i<d->num_quads && n<max;++i) out[n++]=d->quads[i].quad;
                if (n>=max) break;
            } else break;  // FZ_SEARCH_COMPLETE
        }
    }
    fz_always(ctx){ if(search) fz_drop_search(ctx,search); if(stext) fz_drop_stext_page(ctx,stext); }
    fz_catch(ctx){ *threw=true; }
    return n;
}
const std::string FX_SEARCH = (std::filesystem::path("tests/fixtures")/"search.pdf").string();
const std::string FX_LARGE  = (std::filesystem::path("tests/fixtures")/"large.pdf").string();
}

TEST_CASE("spike: parity with fz_search_page", "[spike]") {
    Doc d(FX_SEARCH.c_str()); fz_quad a[512]={}, b[512]={}; bool t=false;
    int nl=legacy_quads(d.ctx,d.doc,0,"lorem",a,512);
    int ni=incr_quads(d.ctx,d.doc,0,"lorem",FZ_SEARCH_IGNORE_CASE,b,512,-1,&t);
    REQUIRE_FALSE(t); REQUIRE(ni==nl);
    for (int i=0;i<ni;++i){ REQUIRE(b[i].ul.x==a[i].ul.x); REQUIRE(b[i].lr.y==a[i].lr.y); }
}
TEST_CASE("spike: case + regex + invalid", "[spike]") {
    Doc d(FX_SEARCH.c_str()); fz_quad q[512]={}; bool t=false;
    int ins=incr_quads(d.ctx,d.doc,0,"Lorem",FZ_SEARCH_IGNORE_CASE,q,512,-1,&t);
    int exa=incr_quads(d.ctx,d.doc,0,"lorem",FZ_SEARCH_EXACT,q,512,-1,&t);
    REQUIRE(ins>0); REQUIRE(exa!=ins);
    int w=incr_quads(d.ctx,d.doc,0,"\\blorem\\b",
        (fz_search_options)(FZ_SEARCH_IGNORE_CASE|FZ_SEARCH_REGEXP),q,512,-1,&t);
    REQUIRE_FALSE(t); REQUIRE(w>0);
    (void)incr_quads(d.ctx,d.doc,0,"foo(",
        (fz_search_options)(FZ_SEARCH_IGNORE_CASE|FZ_SEARCH_REGEXP),q,512,-1,&t);
    REQUIRE(t);
}
TEST_CASE("spike: mid-loop abort", "[spike]") {
    Doc d(FX_LARGE.c_str()); fz_quad q[512]={}; bool t=false;
    int full=incr_quads(d.ctx,d.doc,0,"the",FZ_SEARCH_IGNORE_CASE,q,512,-1,&t);
    int ab  =incr_quads(d.ctx,d.doc,0,"the",FZ_SEARCH_IGNORE_CASE,q,512,2,&t);
    REQUIRE(full>=3); REQUIRE(ab<=3); REQUIRE(ab<full);
}
```

- [ ] **Step 3: Build + run** — `...--target litepdf_unit_tests` then `build\tests\Release\litepdf_unit_tests.exe "[spike]"`. Expected: 3 PASS. If parity fails, the likely culprits are the `FZ_STEXT_DEHYPHENATE` option (must match) or the `seq_needed`/feed sequencing — adjust until parity holds and **record the working loop** (it is Task 2's core).

- [ ] **Step 4: Delete the spike** — remove `tests/unit/spike_search127.cpp` and its `tests/CMakeLists.txt` entry. Do not commit it. (No diff remains → no commit; proceed to Task 1.)

---

## Task 1: Pure search-query helpers (`SearchQuery.hpp/.cpp`)

**Files:** Create `src/core/SearchQuery.hpp`, `src/core/SearchQuery.cpp`, `tests/unit/test_search_query.cpp`; modify `CMakeLists.txt` (add `.cpp` to `litepdf_core`), `tests/CMakeLists.txt` (add the test).

> The 8 `SearchFlags` combinations collapse to 4 distinct `fz_search_options` values (both `whole_word` and `regex` set the `REGEXP` bit); `search_options_value` is tested at its 4 outputs, the orthogonal `whole_word`×`regex` axis by the needle-transform test.

- [ ] **Step 1: Failing test** — `tests/unit/test_search_query.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "core/SearchQuery.hpp"
using namespace litepdf::core;

TEST_CASE("search_options_value", "[search][query]") {
    REQUIRE(search_options_value(false, false) == 1);  // ignore-case literal
    REQUIRE(search_options_value(true,  false) == 0);  // exact literal
    REQUIRE(search_options_value(false, true)  == 5);  // ignore-case regex
    REQUIRE(search_options_value(true,  true)  == 4);  // exact regex
}
TEST_CASE("regex_escape", "[search][query]") {
    REQUIRE(regex_escape("a.b*c") == "a\\.b\\*c");
    REQUIRE(regex_escape("(x)+")  == "\\(x\\)\\+");
    REQUIRE(regex_escape("plain") == "plain");
}
TEST_CASE("build_search_needle", "[search][query]") {
    REQUIRE(build_search_needle("cat",  false, false) == "cat");
    REQUIRE(build_search_needle("cat",  false, true)  == "cat");
    REQUIRE(build_search_needle("a.b",  true,  false) == "\\ba\\.b\\b");
    REQUIRE(build_search_needle("ca|t", true,  true)  == "\\b(?:ca|t)\\b");
}
```

- [ ] **Step 2: Verify it fails** — build; expected FAIL: `core/SearchQuery.hpp` not found.

- [ ] **Step 3: Header** — `src/core/SearchQuery.hpp`:
```cpp
#pragma once
// Pure, MuPDF-free helpers translating litepdf SearchFlags into the matcher
// needle string and options integer. The int equals fz_search_options
// (static_assert'd in the .cpp).
#include <string>
#include <string_view>
namespace litepdf::core {
int         search_options_value(bool match_case, bool needs_regex);
std::string regex_escape(std::string_view literal);
std::string build_search_needle(std::string_view raw, bool whole_word, bool regex);
}  // namespace litepdf::core
```

- [ ] **Step 4: Implementation** — `src/core/SearchQuery.cpp`:
```cpp
#include "core/SearchQuery.hpp"
#include <mupdf/fitz.h>
namespace litepdf::core {
int search_options_value(bool match_case, bool needs_regex) {
    static_assert(FZ_SEARCH_EXACT==0 && FZ_SEARCH_IGNORE_CASE==1 && FZ_SEARCH_REGEXP==4,
                  "fz_search_options drifted");
    int v = match_case ? FZ_SEARCH_EXACT : FZ_SEARCH_IGNORE_CASE;
    if (needs_regex) v |= FZ_SEARCH_REGEXP;
    return v;
}
std::string regex_escape(std::string_view s) {
    static const std::string_view meta = "\\^$.|?*+()[]{}";   // first char is a backslash
    std::string out; out.reserve(s.size()+8);
    for (char c : s) { if (meta.find(c)!=std::string_view::npos) out.push_back('\\'); out.push_back(c); }
    return out;
}
std::string build_search_needle(std::string_view raw, bool whole_word, bool regex) {
    if (!whole_word) return std::string(raw);
    std::string body = regex ? ("(?:" + std::string(raw) + ")") : regex_escape(raw);
    return "\\b" + body + "\\b";
}
}  // namespace litepdf::core
```

- [ ] **Step 5: CMake** — add `src/core/SearchQuery.cpp` to `litepdf_core` in `CMakeLists.txt` (next to `SearchSession.cpp`); add `tests/unit/test_search_query.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 6: Verify pass** — build + `litepdf_unit_tests.exe "[query]"` → 3 PASS.

- [ ] **Step 7: Commit** — `feat(core): pure SearchQuery helpers (options-int + needle transform)`

---

## Task 2: Rewrite `Document::page_hits` for the incremental matcher

**Files:** Modify `src/core/Document.hpp`, `src/core/Document.cpp`, and the existing search test file (grep `[!shouldfail]` to locate it).

- [ ] **Step 1: Widen `SearchFlags` + add `query_compiles` decl (Document.hpp)** — replace the struct + stale comment (~88-97):
```cpp
    // Search options. On MuPDF 1.27.2 all three are honored via the
    // incremental fz_search matcher; see SearchQuery.hpp for the
    // flag->needle/options translation.
    struct SearchFlags {
        bool match_case = false;
        bool whole_word = false;
        bool regex      = false;
    };
```
Add near `page_hits` (~130):
```cpp
    // One-shot validity check: compiles the regex/whole-word needle under
    // `flags`. Returns false iff the pattern fails to compile. Empty needle or
    // a pure-literal query → true (no compile needed). Returns true when no
    // document is open (can't validate; the UI only calls this with an active
    // doc). Cheap: no page is loaded/searched.
    [[nodiscard]] bool query_compiles(std::string_view needle_utf8, SearchFlags flags) const;
```
Update the `@param flags`/`@param abort_flag` doc block (~118-130) to say both are now honored on 1.27.2.

- [ ] **Step 2: Failing tests (existing search test file)** — add:
```cpp
TEST_CASE("page_hits parity: ignore-case literal count", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    auto h = d.page_hits(0, "lorem", {false,false,false}, nullptr);
    REQUIRE(h.size() == 12);   // search.pdf page 0 has 12 "Lorem" (see fixture header)
}
TEST_CASE("page_hits honors match_case", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    auto ci = d.page_hits(0, "Lorem", {false,false,false}, nullptr);
    auto cs = d.page_hits(0, "lorem", {true, false,false}, nullptr);
    REQUIRE(ci.size() == 12);
    REQUIRE(cs.size() == 0);   // EXACT: lowercase needle, capitalized text
}
TEST_CASE("page_hits regex + whole_word", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    REQUIRE(d.page_hits(0, "lo.em", {false,false,true}, nullptr).size() > 0);  // '.'='r'
    REQUIRE(d.page_hits(0, "lorem", {false,true, false}, nullptr).size() > 0); // whole word
}
TEST_CASE("query_compiles rejects bad regex", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/search.pdf").has_value());
    REQUIRE(d.query_compiles("lo.em", {false,false,true}));
    REQUIRE_FALSE(d.query_compiles("foo(", {false,false,true}));
    REQUIRE(d.query_compiles("", {false,false,true}));
}
TEST_CASE("page_hits abort_flag + leak stress", "[document][search]") {
    Document d; REQUIRE_FALSE(d.open("tests/fixtures/large.pdf").has_value());
    std::atomic<int> stop{1};
    REQUIRE(d.page_hits(0, "the", {false,false,false}, &stop).size() == 0);  // pre-aborted
    // Stress the cancel + invalid-regex cleanup paths (no leak/crash over many iters).
    for (int i = 0; i < 200; ++i) {
        std::atomic<int> s{0};
        (void)d.page_hits(0, "the", {false,false,false}, &s);
        (void)d.page_hits(0, "foo(", {false,false,true}, nullptr);  // throws → caught → empty
    }
    REQUIRE(d.page_hits(0, "the", {false,false,false}, nullptr).size() > 0);  // still works
}
```
(Confirm the `search.pdf` "12 Lorem" count from the fixture's test-header comment; if the existing parity test states a different number, use that.)

- [ ] **Step 3: Verify failure** — build target `litepdf_unit_tests`. Expected: link error (`query_compiles` undefined) / assertion failures.

- [ ] **Step 4: Rewrite `page_hits` + add `query_compiles` (Document.cpp)** — add `#include "core/SearchQuery.hpp"` at the top. In `page_hits`: **keep line 480 (the `std::lock_guard`); replace lines 482-522** (the `(void)flags;` comment block through the entire `fz_try/always/catch`, including the old `kMaxQuads`/`quads[]`/`marks[]`/`n`/`pg` declarations at 499-502) with the block below; **keep lines 524-546 unchanged** (the truncation log + per-quad PageHit extraction, which reference only `n`/`quads`):
```cpp
    const std::string needle =
        build_search_needle(needle_utf8, flags.whole_word, flags.regex);
    const auto options = static_cast<fz_search_options>(
        search_options_value(flags.match_case, flags.regex || flags.whole_word));

    fz_context* ctx = impl_->ctx;
    fz_stext_page* stext = nullptr;
    fz_search* search = nullptr;
    constexpr int kMaxQuads = 256;
    fz_quad quads[kMaxQuads] = {};
    int n = 0;
    fz_var(stext); fz_var(search); fz_var(n);

    fz_try(ctx) {
        fz_stext_options so = { FZ_STEXT_DEHYPHENATE };   // match legacy fz_search_page
        stext = fz_new_stext_page_from_page_number(ctx, impl_->doc,
                                                   static_cast<int>(page), &so);
        search = fz_new_search(ctx);
        fz_search_set_options(ctx, search, options, needle.c_str());  // throws on bad regex
        bool fed = false;
        for (;;) {
            if (abort_flag && abort_flag->load() != 0) break;        // mid-page cancel
            fz_search_result r = fz_search_forwards(ctx, search);
            if (r.reason == FZ_SEARCH_MORE_INPUT) {
                // Single page: feed it once (search keeps its OWN ref); later
                // requests get NULL = end-of-doc.
                fz_feed_search(ctx, search,
                               fed ? nullptr : fz_keep_stext_page(ctx, stext),
                               r.u.more_input.seq_needed);
                fed = true;
            } else if (r.reason == FZ_SEARCH_MATCH) {
                fz_search_result_details* d = r.u.match.result;
                for (int i = 0; i < d->num_quads && n < kMaxQuads; ++i)
                    quads[n++] = d->quads[i].quad;
                if (n >= kMaxQuads) break;
            } else {  // FZ_SEARCH_COMPLETE
                break;
            }
        }
    }
    fz_always(ctx) {
        if (search) fz_drop_search(ctx, search);     // releases the fed (kept) ref
        if (stext)  fz_drop_stext_page(ctx, stext);  // releases our own ref
    }
    fz_catch(ctx) {
        std::fprintf(stderr, "litepdf: page_hits failed on page %zu: %s\n",
                     page, fz_caught_message(ctx));
        return out;   // invalid regex / load failure → empty (UI gates via query_compiles)
    }
```
> If Task 0 found a different feed/seq sequence, use the proven one. `fz_new_stext_page_from_page_number` is confirmed present at `third_party/mupdf/include/mupdf/fitz/util.h:87`.

Add `query_compiles` after `page_hits`:
```cpp
bool Document::query_compiles(std::string_view needle_utf8, SearchFlags flags) const {
    if (!is_open() || needle_utf8.empty()) return true;
    if (!flags.regex && !flags.whole_word) return true;  // literal always compiles
    const std::string needle =
        build_search_needle(needle_utf8, flags.whole_word, flags.regex);
    const auto options = static_cast<fz_search_options>(
        search_options_value(flags.match_case, true));
    std::lock_guard<std::mutex> lk(impl_->doc_mutex);
    fz_context* ctx = impl_->ctx;
    fz_search* search = nullptr;
    bool ok = true;
    fz_var(search); fz_var(ok);
    fz_try(ctx) {
        search = fz_new_search(ctx);
        fz_search_set_options(ctx, search, options, needle.c_str());  // compiles regex
    }
    fz_always(ctx) { if (search) fz_drop_search(ctx, search); }
    fz_catch(ctx)  { ok = false; }
    return ok;
}
```

- [ ] **Step 5: Flip the `[!shouldfail]` tripwire** — the existing tripwire searches lowercase `"lorem"` with `match_case=true` and asserts `hits.empty()` under `[!shouldfail]` (documenting the old no-op). Under the new matcher that assertion is now genuinely TRUE (EXACT excludes "Lorem"), so just **remove the `[!shouldfail]` tag** and update its comment to "case-sensitive search now excludes wrong-case matches." No body change.

- [ ] **Step 6: Verify pass** — build + `litepdf_unit_tests.exe "[search]"` (matches `[document][search]` too); then full `ctest --test-dir build -C Release`. Expected: all green, no `[!shouldfail]` remaining, the 200-iter stress case clean. Record the exact suite total in the commit message (contract: +5 here, the tripwire now a normal pass).

- [ ] **Step 7: Commit** — `feat(core): case/regex/whole-word + mid-page-cancel search via fz_search loop`

---

## Task 3: Thread flags through `SearchSession` + add `query_compiles`

**Files:** Modify `src/core/SearchSession.hpp`, `src/core/SearchSession.cpp`.

- [ ] **Step 1: Widen `Flags` (hpp ~34)**
```cpp
    struct Flags {
        bool match_case = false;
        bool whole_word = false;
        bool regex      = false;
    };
```
Add a declaration (near `set_query`):
```cpp
    // True iff `q` compiles as a search needle under `f` (delegates to
    // Document::query_compiles after UTF-16→UTF-8). Pure check; no scan.
    bool query_compiles(const std::wstring& q, Flags f) const;
```

- [ ] **Step 2: Pass all three flags to `page_hits` (cpp)** — find the worker lambda that calls `Document::page_hits` (grep `page_hits`; the captured flags local is named `captured_flags`). Build the 3-field `SearchFlags`:
```cpp
    Document::SearchFlags df{ captured_flags.match_case,
                              captured_flags.whole_word,
                              captured_flags.regex };
    auto hits = /*doc ref*/.page_hits(page, /*needle utf8*/, df, /*&abort_flag*/);
```
(Use the file's existing locals for the doc ref, needle, and abort flag — do not rename them.)

- [ ] **Step 3: Implement `query_compiles`** — reuse the file-local `utf16_to_utf8` (SearchSession.cpp ~67):
```cpp
bool SearchSession::query_compiles(const std::wstring& q, Flags f) const {
    const std::string utf8 = utf16_to_utf8(q);   // existing file-local helper
    Document::SearchFlags df{ f.match_case, f.whole_word, f.regex };
    return impl_->doc.query_compiles(utf8, df);    // use the Impl's Document ref name
}
```
(Match the Impl's actual Document-reference member name — grep `Document` in the Impl struct.)

- [ ] **Step 4: Build + run** — `litepdf_unit_tests.exe "[search]"` (+ `[session]` if present). Expected: PASS; old behavior unchanged (new fields default false).

- [ ] **Step 5: Commit** — `feat(core): SearchSession flags widen + query_compiles`

---

## Task 4: FindBar toggles + Enter gate + MainWindow find wiring (one compiling unit)

Bundles the FindBar UI and its MainWindow call-site change so the tree compiles. The existing **case toggle** is the template. Template anchors in `src/ui/FindBar.cpp`: `kBarWidthDip` const (26), `paint_button` (259, takes a single latch bool), `ButtonKind` enum (252), `button_kind_of` (316), `get_button_state` (325), button-subclass hover/pressed lambdas (356-373), the root-wndproc `WM_COMMAND` button `switch(id)` (538-553) and the `EN_CHANGE` live-fire (533), `WM_DRAWITEM` (567, with an id guard ~571 + the `paint_button` call ~577), `VK_RETURN` keydown (447-450; note line 461 is the WM_CHAR ding-suppressor, separate), button creation (711), layout `reposition` (766-805), `Impl` members (162-245), `fire_query_changed` (224-236).

**Files:** Modify `src/ui/FindBar.hpp`, `src/ui/FindBar.cpp`, `src/ui/MainWindow.hpp`, `src/ui/MainWindow.cpp`.

- [ ] **Step 1: Widen the callback + add the affordance setter (FindBar.hpp:19)**
```cpp
    using QueryChanged = std::function<void(std::wstring, bool, bool, bool)>;  // text, case, whole, regex
```
Add a public method:
```cpp
    void set_invalid_pattern(bool invalid);  // red field + "Invalid pattern"; cleared on next run/edit
```

- [ ] **Step 2: Impl state (after `case_pressed`, ~173)**
```cpp
    bool whole_pressed = false, regex_pressed = false;
    bool regex_dirty = false;      // set on edit while regex on; cleared on run
    bool invalid_pattern = false;
    HWND btn_whole = nullptr, btn_regex = nullptr;
    bool hover_whole = false, hover_regex = false;
    bool pressed_whole = false, pressed_regex = false;
    bool last_whole = false, last_regex = false;
```

- [ ] **Step 3: `fire_query_changed` (replace body 224-236)**
```cpp
    void fire_query_changed() {
        if (!edit || !on_query_changed) return;
        const int len = GetWindowTextLengthW(edit);
        std::wstring txt(static_cast<std::size_t>(len), L'\0');
        if (len > 0) GetWindowTextW(edit, txt.data(), len + 1);
        if (txt == last_query && case_pressed == last_case &&
            whole_pressed == last_whole && regex_pressed == last_regex) return;
        last_query = txt; last_case = case_pressed;
        last_whole = whole_pressed; last_regex = regex_pressed;
        invalid_pattern = false;
        on_query_changed(txt, case_pressed, whole_pressed, regex_pressed);
    }
```

- [ ] **Step 4: Suppress live-fire while regex is on (root WM_COMMAND EN_CHANGE, ~533)** — change the `if (id == kIdEdit && code == EN_CHANGE) impl->reschedule_debounce();` to:
```cpp
            if (id == kIdEdit && code == EN_CHANGE) {
                if (impl->regex_pressed) impl->regex_dirty = true;  // wait for Enter
                else impl->reschedule_debounce();
            }
```

- [ ] **Step 5: Enter gate (VK_RETURN keydown, ~447-450)** — replace the `case VK_RETURN:` body:
```cpp
                case VK_RETURN:
                    if (impl->regex_pressed && impl->regex_dirty) {
                        impl->regex_dirty = false;
                        impl->fire_query_changed();          // compile + run now
                    } else if (GetKeyState(VK_SHIFT) & 0x8000) {
                        if (impl->on_prev) impl->on_prev();
                    } else {
                        if (impl->on_next) impl->on_next();
                    }
                    return 0;
```
(Leave the WM_CHAR VK_RETURN/VK_ESCAPE ding-swallow at ~461 as-is.)

- [ ] **Step 6: Two new buttons across ALL six owner-draw touch-points**
  1. **IDs:** add `kIdBtnRegex`, `kIdBtnWhole` next to `kIdBtnCase`.
  2. **`ButtonKind` (252):** add `Regex, Whole`.
  3. **`button_kind_of` (316):** add `case kIdBtnRegex: return ButtonKind::Regex;` / `case kIdBtnWhole: return ButtonKind::Whole;`.
  4. **`paint_button` (259):** in the glyph switch, render `".*"` for `Regex` and `"W"` for `Whole`, mirroring the `Case` glyph. The latch bool is already a `paint_button` parameter; at the **WM_DRAWITEM call (~577)** compute it per-id:
     `bool latched = (id==kIdBtnCase && impl->case_pressed) || (id==kIdBtnRegex && impl->regex_pressed) || (id==kIdBtnWhole && impl->whole_pressed);` and pass `latched` instead of `impl->case_pressed`.
  5. **WM_DRAWITEM id guard (~571):** ensure `kIdBtnRegex`/`kIdBtnWhole` are NOT filtered out before `paint_button` (add them to whatever id set the guard allows).
  6. **`get_button_state` (325) + subclass hover/pressed lambdas (356-373):** add `kIdBtnRegex`→`hover_regex`/`pressed_regex` and `kIdBtnWhole`→`hover_whole`/`pressed_whole` cases.

- [ ] **Step 7: Click handling (root WM_COMMAND `switch(id)`, 538-553)** — add cases:
```cpp
                case kIdBtnRegex:
                    impl->regex_pressed = !impl->regex_pressed;
                    impl->regex_dirty = impl->regex_pressed;     // require Enter on enable
                    impl->invalidate_button(impl->btn_regex);
                    if (!impl->regex_pressed) impl->fire_query_changed();  // back to live
                    return 0;
                case kIdBtnWhole:
                    impl->whole_pressed = !impl->whole_pressed;
                    impl->invalidate_button(impl->btn_whole);
                    impl->fire_query_changed();                  // whole-word stays live
                    return 0;
```

- [ ] **Step 8: Create + lay out + widen the bar** — create `btn_regex`/`btn_whole` at the `btn_case` creation site (711) with `create_button(...)`, add them to the enable/show vector (~718) and set English tooltips (`"Regular expression (Enter to search)"`, `"Whole word"`) via the bar's existing tooltip mechanism (grep `TTM_`/`TOOLTIP`; if none, set a standard tooltip control or skip + note). **Bump `kBarWidthDip` (26) by `2*(case-button-width + inner-pad)`** so the row `[prev][next][Aa][.*][W][close]` fits; update `reposition` (give each new button its own `cx` slot, 766-805) and the WM_CREATE size path that reads `kBarWidthDip`.

- [ ] **Step 9: `set_invalid_pattern` + persistence** — implement the method: store `impl_->invalid_pattern`, `InvalidateRect(impl_->edit, nullptr, FALSE)`; in the Edit's `WM_CTLCOLOREDIT` handler, set red text when `invalid_pattern` is true. Persistence: the latch bools live on `Impl` (created once per app, outlives hide/show) — do NOT reset them in `hide()`/`show_or_focus()`.

- [ ] **Step 10: MainWindow find wiring (MainWindow.hpp + .cpp)** — widen `on_find_query_changed`'s declaration (MainWindow.hpp ~86) and definition (~1687) to `(std::wstring q, bool mc, bool ww, bool rx)`. Update the `find_bar_->set_on_query_changed(...)` lambda (~926) to `(std::wstring q, bool mc, bool ww, bool rx)` forwarding all four. In `on_find_query_changed`:
```cpp
    core::SearchSession::Flags f{ mc, ww, rx };
    if (auto* v = active_view()) {
        if ((rx || ww) && !v->search().query_compiles(q, f)) {
            if (find_bar_) find_bar_->set_invalid_pattern(true);
            return;
        }
        if (find_bar_) find_bar_->set_invalid_pattern(false);
        v->search().set_query(q, f);
    }
```
(`query_compiles` takes the `std::wstring` — no UTF-8 conversion needed in MainWindow.)

- [ ] **Step 11: Build + smoke-compile** — build target `litepdf`. Expected: links. (GUI behavior verified in Task 6.)

- [ ] **Step 12: Commit** — `feat(ui): FindBar regex/whole-word toggles + Enter-to-run + invalid affordance`

---

## Task 5: ResultsPanel toggles + cross-tab wiring (one compiling unit)

Cross-tab query row gains case / regex / whole-word toggles (default off), bundled with the MainWindow cross-tab dispatch change. ResultsPanel currently owner-draws ONLY a close button (`ResultsPanel.cpp:212-241`, subclass 292-335 tracks close only, WM_DRAWITEM ~388/400, edit-width layout ~672-687). There is **no shared latch/ButtonKind machinery to reuse** — build it here mirroring the (now-complete) FindBar pattern.

**Files:** Modify `src/ui/ResultsPanel.hpp`, `src/ui/ResultsPanel.cpp`, `src/ui/MainWindow.hpp`, `src/ui/MainWindow.cpp`.

- [ ] **Step 1: Widen `OnQuerySubmit` (ResultsPanel.hpp:22)**
```cpp
    using OnQuerySubmit = std::function<void(std::wstring, bool, bool, bool)>;  // q, case, whole, regex
```

- [ ] **Step 2: Add latch state + 3 toggle buttons** — in `ResultsPanel::Impl`: `bool case_pressed=false, whole_pressed=false, regex_pressed=false, regex_dirty=false;` plus `HWND btn_case/btn_whole/btn_regex` and per-button `hover_*`/`pressed_*` bools. Add `kIdBtnCase/Regex/Whole` ids. Introduce a `ButtonKind { Close, Case, Regex, Whole }` + extend the file-local `paint_button` to take a latch bool and render `"Aa"/".*"/"W"` glyphs (mirror FindBar). Extend the WM_DRAWITEM handler (~400) to dispatch all four ids (compute the per-id latch as in FindBar Step 6.4) and the subclass (292-335) to track hover/pressed for all four.

- [ ] **Step 3: Layout — make room for 3 buttons** — the query-row edit-width math (~675-687) computes the Edit width from one close button; subtract three additional button widths (+ pads) so the row reads `[edit][Aa][.*][W][x]`. Default all three latches OFF.

- [ ] **Step 4: Submit with flags + toggle re-submit + Enter gate** — where ResultsPanel fires `on_query_submit(txt)` (grep it), pass `(txt, case_pressed, whole_pressed, regex_pressed)`. Cross-tab search is already submit-on-Enter, so the regex gate is automatic; on a toggle click, re-fire the submit with the current query (so toggling re-runs). For regex specifically, set `regex_dirty` on edit and only submit on Enter (mirror FindBar) — or, since the panel is already Enter-submit, simply always submit on Enter and let `query_compiles` gate validity in MainWindow (Step 6). Pick the simpler Enter-submit path and note it.

- [ ] **Step 5: MainWindow cross-tab wiring** — the cross-tab dispatch is **inline inside `MainWindow::on_results_query`** (MainWindow.cpp ~1809-1828; there is no separate `on_cross_tab_search` method). Widen `on_results_query`'s declaration (MainWindow.hpp ~94) and definition (~1809) and the `results_panel_->set_on_query_submit(...)` lambda (~956) to `(std::wstring q, bool mc, bool ww, bool rx)`. Replace the hard-coded `litepdf::core::SearchSession::Flags f{};` (~1827) with:
```cpp
    litepdf::core::SearchSession::Flags f{ mc, ww, rx };
```
Cross-tab invalid-regex handling: keep it simple — if `(rx || ww)` and the query doesn't compile against the first snapshot tab's `search().query_compiles(q, f)`, skip the dispatch (optionally flag the ResultsPanel field); otherwise `cross_tab_->dispatch(q, f, std::move(snapshot))` unchanged. Add a one-line comment for whichever path you choose.

- [ ] **Step 6: Build + full suite** — build target `litepdf`; then `ctest --test-dir build -C Release`. Expected: links + all green.

- [ ] **Step 7: Commit** — `feat(ui): ResultsPanel case/whole-word/regex toggles + cross-tab flag plumbing`

---

## Task 6: Verify, benchmark, document, finalize

**Files:** Modify `CHANGELOG.md`, `docs/plans/2026-04-15-litepdf-roadmap.md`.

- [ ] **Step 1: Scripted GUI smoke** — build `litepdf`, launch with `tests/fixtures/search.pdf` (using the harness described in the preamble). Verify: Ctrl+F live hits → `Aa` filters case → `W` filters whole-word (still live) → enable `.*`, type `lo.em`, Enter (regex runs) → type `foo(`, Enter (red "Invalid pattern", prior results kept) → Ctrl+Shift+F → ResultsPanel toggles behave the same across tabs. Capture before/after screenshots.

- [ ] **Step 2: Benchmark (R2 + R5)** — run the benchmark harness search path on `large.pdf` for literal / case-sensitive / live-whole-word / regex. Confirm no regression beyond the CI gate and that live whole-word p95 is acceptable; if not, gate whole-word on Enter (mirror regex) and note it. Record numbers in the commit message.

- [ ] **Step 3: CHANGELOG + roadmap** — under `## [Unreleased]` in `CHANGELOG.md`:
```markdown
### Added
- **Search: case-sensitive, regex, and whole-word.** The FindBar and cross-tab
  results panel gain Aa / .* / W toggles. Regex uses ECMAScript syntax and runs
  on Enter; literal/case/whole-word search remain live.

### Changed
- Search cancels mid-page promptly when the query changes or a tab closes.

### Known limitations
- Whole-word boundaries are ASCII-only (accented/CJK words are best-effort).
- A catastrophic-backtracking regex cannot be interrupted mid-match.
```
In the roadmap's search known-limitations section, mark case-sensitive / regex / whole-word / mid-page-cancel cleared in v1.1.0.

- [ ] **Step 4: Full suite + commit** — `ctest --test-dir build -C Release --output-on-failure` → all green.
`docs+test: search-upgrade smoke evidence, benchmark, CHANGELOG/roadmap for v1.1.0`

> VERSION bump to `1.1.0` + the `v1.1.0-...` tag happen at the **ship PR** (project convention), which runs the 3-lens PR-merge gate (incl. Codex).
