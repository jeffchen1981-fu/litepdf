# Prefetch a page's text on the render worker — design

**Date:** 2026-10-10
**Baseline:** `main` @ `049d27d`
**Issue:** #98 (a page's first text-selection press stalls the UI thread: ~27 ms
per tab, up to ~220 ms)
**Ships as:** one PR.

## 1. Motivation

The first `Document::text_page(page)` for a page on a tab runs on the UI thread,
at the first left press on that page (`PdfCanvas::on_left_button_down`) or the
first Select All (`PdfCanvas::select_all`). It builds the page's structured text
cold on the tab Document's own context: a median of 27 ms per tab on
Edge-printed PDFs, and 100-220 ms on pages that carry a dozen or more Type3
fonts (#73's measurements, reproduced by `litepdf-cli <file> --bench-selection`).

The render workers have already parsed those fonts by then, but each worker
renders from its own `fz_document`, and MuPDF keys a PDF resource in the store
on its owning document (`pdf-store.c:36`, `pdf_get_indirect_document`). No PDF
resource a worker loads can be found through the tab Document. Process-wide
caches, such as the DirectWrite face cache in `SystemFonts.cpp`, are shared, but
they are not where the cost lies.

### 1.1 Why the issue's suggested remedy is not used

#98 suggests prefetching the current page's `TextPage` off the UI thread by
calling `Document::text_page` on a background thread. That holds `doc_mutex` for
the whole build (`Document.cpp:847-881`), and the UI thread takes the same lock
on paths far more frequent than a press:

- every fit-mode page turn: `DocumentView::set_current_page` → `set_viewport` →
  `Document::page_size` (`DocumentView.cpp:209-210, 236`; `Document.cpp:408`),
  which loads the page on every call. There is no page-size cache;
- every canvas resize refit (#109), through the same `set_viewport`;
- every tab switch and open: `Document::outline()` (`MainWindow.cpp:792`,
  `Document.cpp:608`);
- find-bar typing with regex or whole word: `query_compiles`
  (`MainWindow.cpp:2204`, `Document.cpp:588`).

A prefetch started when a page settles would make the next page turn wait up to
220 ms. The stall would move from the first press to page navigation, which
happens far more often.

## 2. Approach

**Build the text on the render worker, on the worker's own document, right after
it renders the current page, and hand the handle to the UI thread. Skip the
build whenever it could delay a newer render.**

- The worker never takes `doc_mutex`: it uses its own context and `fz_document`.
- The fonts are usually warm. A worker that has just interpreted the page into
  its own store pays about the issue's "warm" column (0.9-4 ms) for the text
  pass. A render served from the pixmap cache, or from a display list the other
  worker built, may find this worker's store cold for that page. Its build can
  then cost up to the cold figure, on the worker. §3.3's skip rules keep such a
  build from delaying a newer render that is already waiting, and §5.4 measures
  it.
- The handoff reuses a mechanism that has already shipped. A `TextPage` owns an
  `EscrowContext`, which keeps the MuPDF root and lock table alive (#61). The
  render path already ships pixmaps to the UI thread the same way
  (`post_render_done_impl`).

Rejected alternatives:

- **The issue's remedy** (§1.1). Making it viable would need page sizes,
  `outline()` and `query_compiles` all moved off `doc_mutex`, and it would still
  leave the fallback path unchanged. It is larger and more fragile than this
  design.
- **A third `fz_document` per tab on a dedicated prefetch thread.** It avoids
  the lock too, but pays the full cold parse on a document whose store nothing
  else warms, plus a third copy of the document in memory.
- **Building from the cached display list** (`fz_new_stext_page_from_display_list`
  over the L2 entry). **Not equivalent:** the L2 list is recorded with
  `fz_run_page` (page contents + annotations + widgets, `document.c:1070-1074`,
  `RenderEngine.cpp:150-151`, `util.c:39`), while `fz_new_stext_page_from_page`
  runs `fz_run_page_contents` only (`util.c:313`). Selection and copy would start
  to include annotation and form-field text, and disagree with `page_text`.
- **Sharing one store across documents.** Impossible by construction: the store
  key carries the owning document.

## 3. Design

### 3.1 One builder (`core`)

Today's body of `Document::text_page` after it takes the lock becomes a single
function shared by both callers. The wiring keeps `Document.hpp` MuPDF-free
(`Document.hpp:4-5`) and needs no new translation unit:

- **`src/core/detail/TextPageAccess.hpp`** (new, core-internal, includes
  `<mupdf/fitz.h>`):
  - the definition of `struct Document::TextPage::Impl`, moved verbatim from
    `Document.cpp:630-654` (members, destructor order and comments unchanged);
  - `struct detail::TextPageAccess { static Document::TextPage build(fz_context*
    ctx, fz_document* doc, int index) noexcept; };`
- **`Document.hpp`** forward-declares `namespace detail { struct TextPageAccess; }`
  and `TextPage` declares `friend struct detail::TextPageAccess;`. No `fz_*` type
  is named.
- **`Document.cpp`** defines `TextPageAccess::build`. No CMake change is needed:
  `litepdf_core` lists its sources explicitly, and none is added.

`build` clones the handle's `EscrowContext` from `ctx`, checks `index` against
`fz_count_pages` inside `fz_try`, loads the page, and calls
`fz_new_stext_page_from_page` with `fz_stext_options opts = {}`. Then it warms
`full_range`, as today, and returns an empty handle on every failure.

- `Document::text_page` keeps its contract (noexcept, deferred `unique_lock` on
  `doc_mutex`, empty handle on every failure). It calls `build` with
  `impl_->ctx` / `impl_->doc` under the lock.
- The worker calls `build` with its own context and document, and takes no lock.

One function is the point. §3.1 of the #52 design
(`2026-09-16-text-selection-copy-hand-tool-design.md`) fixes two options that
must NOT be added (`FZ_STEXT_COLLECT_STRUCTURE`, `FZ_STEXT_DEHYPHENATE`). Two
copies of the option block would let the two paths drift apart silently.

### 3.2 `TextPage` becomes shareable (UI thread only)

A worker-built handle is stored per tab (§3.5). A press must leave the stored
handle in place: `drag_text` is released on button-up, and if the press had
moved the stored handle into `drag_text`, the next press would rebuild cold on
the Document. `Document::TextPage::Impl` is therefore held by `std::shared_ptr`,
and a new member returns a second handle to the same page:

```
[[nodiscard]] TextPage share() const noexcept;   // empty from an empty handle
```

The single-thread rule is unchanged and now covers the shared object: every
handle sharing one `Impl` is used on one thread at a time. In practice that is
the UI thread, because the worker gives up its only handle when it posts. The
header comment at `Document.hpp:248-253` is updated to say so. Move semantics
stay. The copy constructor and copy assignment stay deleted, so a share is
always explicit. `Impl`'s destructor still drops the stext page through the
escrow before the escrow itself goes. It now runs when the last share is
released.

### 3.3 Render request and worker (`core::RenderEngine`)

`RenderEngine.hpp` includes `core/Document.hpp`, which is MuPDF-free. A nested
class cannot be forward-declared, so the include is needed. `RenderRequest`
gains two last members, so existing aggregate initialisers
(`render_to_ppm.cpp:58`, `bench_iteration.cpp:79`) keep compiling:

```
// (#98) Called on the worker with the page's text, built on the worker's own
// document after on_complete has delivered a non-null pixmap. See the skip
// rules: not calling it is always allowed, and the UI then builds synchronously.
std::function<void(Document::TextPage)> on_text;
// (#98) The page whose text is still wanted. on_text runs only while this holds
// req.page_num. Owned by the DocumentView, which writes it on the UI thread.
std::shared_ptr<const std::atomic<int>> text_wanted;
```

Worker behaviour, when `on_text` is set. It applies on **both** successful
delivery paths: the L1 pixmap-cache hit (`RenderEngine.cpp:120-131`) and the full
render. After `on_complete` has run with a non-null pixmap, the worker **skips**
the build (no `on_text` call) if any of these holds:

1. `stopping` is set. The engine is being destroyed: a tab close, or exit.
   `~RenderEngine` drains the queue before it joins (`RenderEngine.cpp:95-96`,
   `379-381`), so without this check every queued text build would run while the
   UI thread waits in `join()`.
2. `text_wanted` no longer holds `req.page_num` (§3.5 says when it changes).
3. A priority-0 request is waiting in the queue, read under `mtx`. That render is
   newer and must not wait behind this build. If it still needs text, it carries
   its own `on_text`.
4. The request's cancel flag is set. No product caller cancels a P0 today:
   `cancel_all_below_priority(0)` flags only priority > 0
   (`RenderEngine.cpp:423`), and `DocumentView` discards its tokens. But
   `RenderEngine::cancel(token)` can flag any priority, so the rule is honoured.

Rules 1 and 3 are read together under **one** acquisition of `mtx`: `stopping`
is a plain `bool` written under that lock (`RenderEngine.cpp:375-378`), so an
unlocked read would be a data race. Rule 2 is an atomic load, and rule 4 is the
existing atomic flag.

Otherwise it calls `TextPageAccess::build(ctx, wdoc, req.page_num)`. After the
build it re-checks rules 1 (again under `mtx`), 2 and 4, then calls
`on_text(std::move(handle))` if the handle is valid. In every other case it
drops the handle on the worker. That is safe: a `TextPage` frees its stext page
through its own escrow, and no other thread holds this one.

The check is not atomic with the start of the build. A page change, newer P0 or
shutdown that lands between the check and the build's first MuPDF call does not
stop that build; the post-build re-check only discards its result. That window
and a build already running are the same residual: one build, which cannot be
pre-empted. (An `fz_cookie` abort could shorten it; §8.)

- The text is built **after** the pixmap is posted, so the same request's
  bitmap never arrives later than today. A newer P0 that is queued before the
  skip check is never delayed by the build (rule 3). A P0 queued after it waits
  for the build only if the other worker is busy too. That residual is bounded
  by one build, and §5.4 measures it.
- No `on_text` call on a null pixmap. A page with no bitmap at all cannot be
  pressed (`own_bitmap()` refuses). A failed same-page re-render leaves the old
  bitmap in place, and the press then falls back to a synchronous build.

### 3.4 Posting to the UI thread (`ui::PdfCanvas`)

A new message `WM_USER_TEXT_READY = WM_USER + 5`, defined in `PdfCanvas.hpp`
beside `WM_USER_RENDER_DONE` (+3) and `WM_USER_RENDER_DONE_RIGHT` (+4). +5 is
unused anywhere in `src/` today; the ids in use are +1, +2, +3, +4, +10, +11,
+12, +17 and +18. `MainWindow.cpp`'s reservation comment at line 59, which today
names only +3, is extended to name +3 to +5 as PdfCanvas's. It carries a heap `TextMeta { Document::TextPage text;
std::uint64_t epoch; }` in LPARAM. `PdfCanvas.hpp` includes
`core/Document.hpp`.

`PdfCanvas::post_text_ready(HWND, Document::TextPage, std::uint64_t epoch)`
mirrors `post_render_done_impl`'s failure branches. They are simpler here,
because the handle owns its escrow:

- `new (std::nothrow)` fails → the handle is destroyed on the worker → return
  false;
- `PostMessageW` fails → `delete meta` → return false. This is the branch that
  runs at process exit, after the canvas HWND has died (canvas HWND dies first);
- posted but never dispatched at exit → leaked, the same accepted stance as #74
  (`PdfCanvas.cpp:168-179`).

Handler: adopt and delete the meta. If `!impl_->view` or
`meta.epoch != impl_->view_epoch`, drop the handle and return. The view may
belong to a closed tab, so it must not be touched. Otherwise call
`impl_->view->offer_text(std::move(handle))`. No invalidate is needed, because
nothing painted changes.

The staleness key is `(epoch, page)` only. `accept_completion`'s `seq` and
`slot` tests are bitmap semantics: `seq` advances on every same-page zoom
(`resubmit_current_page` calls `next_render_seq()`), and would discard a handle
that is still correct. The page test lives in `offer_text`.

### 3.5 The per-tab slot (`core::DocumentView`)

`DocumentView::Impl` holds one `Document::TextPage text_slot`, at most the
current page's text per tab. It also holds a
`std::shared_ptr<std::atomic<int>> text_wanted`, allocated once in the
constructor with the value -1 and **never reassigned**: queued requests hold the
same object. Every "`text_wanted = v`" below means
`text_wanted->store(v, std::memory_order_relaxed)`, and the worker reads it with
a relaxed load. Ordering beyond the value itself is not needed: the handle
travels by `PostMessageW`, and a stale read only wastes or discards a build.
All stores happen on the UI thread.

| Member | Behaviour |
|---|---|
| `bool has_text_for(int page) const noexcept` | `text_slot.page() == page`. |
| `void offer_text(Document::TextPage t)` | Stores `t` iff it is valid, `t.page() == current_page` and `!has_text_for(current_page)`. Otherwise drops it. A handle already in the slot is never replaced, so a sync-built handle is not churned by a late worker one. On store: `text_wanted = -1`. |
| `Document::TextPage text_for(int page)` | If `has_text_for(page)`, returns `text_slot.share()`. Otherwise builds synchronously with `doc.text_page(page)` (today's path). It stores the result iff it is valid and `page == current_page` (then `text_wanted = -1`), and returns it, or a share of it. |
| `set_current_page` | When the page actually changes, drops the slot and sets `text_wanted = -1`, **before** `set_viewport`, which can throw (`Document::page_size` throws `std::runtime_error`). Memory stays bounded to one page per tab. |
| `set_dual_page(true)` | `text_wanted = -1`. |
| `~DocumentView` | `text_wanted = -1` as its first statement, before the thumbnail teardown and `engine.reset()`. The engine's own `stopping` check (§3.3 rule 1) backs it up. |
| `request_render_with_prefetch(page, cb, TextCb on_text = {})` | Iff `on_text` is given and `wants_text_prefetch(page)`: sets `text_wanted = page`, and attaches `on_text` and `text_wanted` to the P0 request. Never to P1 neighbours or thumbnails. |
| `bool wants_text_prefetch(int page) const noexcept` | `!dual_page && !doc.needed_password() && !has_text_for(page)`. Public, for the unit tests. |
| `int pending_text_page() const noexcept` | `text_wanted`'s current value. Test observability only. |

The slot outlives a tab switch, so returning to a tab whose current page was
already prefetched costs nothing. A handle delivered while its tab is inactive
is dropped by the epoch test (§3.4). The next render of that page after the tab
is reactivated, for example its L1 hit, requests the text again. The slot dies
with the tab. Its position among `Impl`'s members does not matter: the handle's
escrow keeps the MuPDF root alive past the Document.

### 3.6 Password-protected documents are not prefetched

The worker documents are never authenticated: `fz_open_document` at
`RenderEngine.cpp:323`, and the only `fz_authenticate_password` call in `src/`
is in `Document::authenticate`. MuPDF tries a blank password at open
(`pdf-xref.c:1858-1859`) and installs whatever key results. On a document with a
real user password, a worker-built stext page is therefore either an error or a
"valid" handle holding empty or garbled text. A valid-but-wrong handle would
make the first press select nothing where today it selects the page's text.

New: `bool Document::needed_password() const noexcept`. It returns true when
`open()` reported `NeedsPassword`, and stays true after a successful
`authenticate()`. It reads `impl_->needs_password`, which `authenticate()` does
not touch and only `close()` resets. `wants_text_prefetch` refuses such
documents, which keep today's synchronous path. The slot still caches their
sync-built handle.

Whether the render workers draw such documents correctly at all is a separate
question with the same cause. It is tracked outside this change (§8).

### 3.7 Callers (`ui`)

- The three single-page submission sites pass a text callback:
  `MainWindow::kick_render` (`MainWindow.cpp:385`),
  `PdfCanvas::resubmit_current_page` (`PdfCanvas.cpp:1369`) and the single-page
  tail of `PdfCanvas::navigate_to_page` (`PdfCanvas.cpp:1736`). The spread
  branches use `request_render` and pass none. The callback captures the
  site's canvas HWND local and its epoch. The HWND local is `target` in the
  first two sites and `target_hwnd` in `navigate_to_page`, whose `target` is
  the `int` page parameter:

  ```
  [hwnd, epoch](litepdf::core::Document::TextPage t) {   // hwnd = that local
      PdfCanvas::post_text_ready(hwnd, std::move(t), epoch);
  }
  ```

- `PdfCanvas::on_left_button_down` and `PdfCanvas::select_all` replace
  `view->document().text_page(page)` with `view->text_for(page)`. Everything
  after that line is unchanged. `drag_text` holds a share. Releasing it on
  button-up or cancel leaves the slot intact, and a later `offer_text` never
  touches a live gesture's handle.

## 4. Behaviour

| Situation | Result |
|---|---|
| Press or Ctrl+A on a page whose prefetch has landed | No text build on the UI thread; the slot's handle is shared. |
| Press after the bitmap but before the prefetch lands | Synchronous build (today's cost); the result fills the slot; the late worker handle is dropped by `offer_text`. |
| Ctrl+A right after a page change or open, before the page's bitmap | Synchronous build, as today. `select_all` has no bitmap gate. |
| Second and later presses on the same page | The slot's handle; no rebuild. Today each press rebuilds warm (0.9-4 ms). |
| Password-protected document | Never prefetched. The first press or Ctrl+A builds synchronously, as today; later ones use the slot. |
| Spread mode | No prefetch. Presses and Select All are already refused there. |
| Page change | Slot dropped and `text_wanted = -1`: an in-flight build for the old page is skipped (rule 2) or its result is dropped. The new page's P0 requests text. |
| Rapid paging, held PgDn | Each passed-through page's P0 skips its build, because the next page's P0 is already queued (rule 3) or the page is no longer wanted (rule 2). The page the reader stops on gets its build. |
| Same-page zoom, resize, DPI change, Invert Colors | Before the page's text has landed, each re-render carries `on_text`. Rule 3 skips every build but the last, and `offer_text` drops duplicates. After it has landed, no text request (`has_text_for` is true). |
| Tab switch away and back | The slot survives in the tab's `DocumentView`. A handle delivered while the tab is inactive is dropped (epoch), and the page's next render requests it again. |
| Tab close, or exit, with builds queued | `~DocumentView` sets `text_wanted = -1`, and the draining workers see `stopping`. No queued build starts while the UI thread joins (rules 1-2). A build that already passed its check finishes first: one build per worker at most (§3.3). |
| Tab closed with a `WM_USER_TEXT_READY` queued | Epoch mismatch: the handle is dropped through its own escrow; the dead view is not touched. |
| Render failed (null pixmap) | No text build. |
| A page with no text (CBZ, image-only) | Valid handle, `full_range` false, same as the synchronous path. |

Selection, quads and copied text are byte-for-byte what `Document::text_page`
produces on every fixture (§5.1). Only where and when the build happens
changes. §8 records the one input class this oracle does not cover.

## 5. Testing

### 5.1 Unit tests (no HWND)

1. **Equivalence oracle.** A worker-built handle (a one-worker `RenderEngine`
   render with `on_text` capturing into a promise, bounded wait) is compared with
   `Document::text_page` on: every page of `selection.pdf`, `search.pdf`,
   `simple.pdf`, `bookmarks.pdf`, the three `cjk-*.pdf`, `sample.epub` and the
   two new fixtures (5.3), `sample.cbz`, and #119's `/Count`-inflated copy of
   `simple.pdf`; and the first, a middle and the last page of
   `large.pdf`, whose ~500 pages would not fit the suite's 120 s timeout. Assert
   equal `full_range` points, equal `highlight(first, last)` quads, and equal
   `copy(first, last)` text. On `selection.pdf` page 3, the page the existing
   selection tests pin word positions on, also compare an interior selection:
   `snap` in Chars, Words and Lines mode on the same point pair, then
   `highlight` and `copy` of each snapped pair. Reuse those pinned words
   (`alpha`, `beta`, `gamma`); do not invent coordinates. The
   whole-page range alone would not notice a difference in word or line
   segmentation. **Non-vacuity:** the wrong-builder mutant, which builds from
   the L2 display list, must fail on `annot.pdf`.
2. **Gate.** `wants_text_prefetch` is false for `encrypted.pdf` after
   `authenticate`, false in spread mode, false once `has_text_for(page)`, and
   true otherwise. `Document::needed_password()` stays true after
   `authenticate()` and becomes false after `close()`.
3. **Slot.**
   - `offer_text` drops a handle for a page other than the current one and keeps
     an existing handle.
   - `set_current_page` to another page empties the slot; to the same page it
     does not.
   - **The slot is actually used:** hold `h1 = text_for(p)` and a probe
     `EscrowContext`. Read `family_context_count(probe.get())`, call
     `text_for(p)` again, and require the count unchanged. A rebuild clones a new
     escrow and raises it by one. Non-vacuity: the same read across a
     `text_for(q)` on another page must rise by one.
4. **Share.** Dropping one of two shared handles leaves the other usable; the
   MuPDF root is released only after both are gone (`live_mupdf_roots`).
5. **Skip rules.** At the `RenderEngine` level, one worker, with a test-owned
   `text_wanted`. In every case the request's `on_complete` blocks on a latch,
   the test thread acts as the UI thread while the worker is parked, then
   releases the latch. A negative case knows the parked request's check has run
   when a priority-1 sentinel request, submitted while parked, completes;
   destroying the engine is never that marker, because `~RenderEngine` sets
   `stopping`. No case depends on timing:
   - **Page no longer wanted:** while parked, the test stores -1 into
     `text_wanted` → no `on_text`. The view's own writes to `text_wanted`
     (`set_current_page`, `set_dual_page`, `offer_text`, the request) are pinned
     separately through `DocumentView::pending_text_page()`.
   - **Cancelled after the pixmap:** while parked, the test sets the request's
     cancel flag → no `on_text`.
   - **Newer P0 queued:** while A is parked, the test submits P0 request B →
     A gets no `on_text`; B, still wanted, gets it.
   - **Engine stopping:** while A is parked, with P0 request B for the same,
     still-wanted page (carrying `on_text`) queued behind it, the test calls `begin_shutdown()`, releases the latch and
     destroys the engine. B is still rendered (the queue drains), and no
     `on_text` runs.
   - **Null pixmap:** a request carrying `on_text` whose cancel flag is set
     before submit (checkpoint A) gets `on_complete(nullptr)` and no `on_text`.
     A null pixmap at the full-render delivery site comes from a failed
     rasterise or invert after a good page load, which no fixture produces
     deterministically; that guard has no separate test. (A request for a page
     past the end would not test it: `build` fails there too.)
   - **Positive control:** the same harness with none of the above → `on_text`
     called once.

   `RenderEngine::begin_shutdown()` (new, idempotent) sets `stopping` under
   `mtx` and wakes the workers. `~RenderEngine` calls it and then joins, as
   today. `~DocumentView` does not call it early: the thumbnail teardown before
   `engine.reset()` still relies on the queue running normally. Its own guard
   is `text_wanted = -1` as its first statement (§3.5), which rule 2 enforces.
6. **Lifetime.** A worker-built handle outlives its `DocumentView` (the engine,
   the worker documents and the Document all destroyed). `copy()` on it still
   works. Order of the checks, following `test_document_view.cpp:330-346`:
   1. with a probe `EscrowContext` held, `family_context_count` falls by the
      handle's one context when the handle is dropped;
   2. then the probe is dropped, and `live_mupdf_roots()` returns to its
      baseline.

   Run it on `selection.pdf` and on `type3.pdf`. Add the same `type3.pdf` case
   for today's path: a `Document::text_page` handle outliving its `Document`.
   That closes the gap the review found: the decouple-then-drop path
   (`pdf-xref.c:1989-1994`, `font.c:188-197`) has never had a test, because no
   fixture has a Type3 font.

Test names are ASCII and prefixed by subsystem. Do not hard-code the ctest
count; `ctest -N` at implementation time is the reference.

### 5.2 Scripted GUI check

An untracked driver under `build/gui-check/`, using a probe exe built in a
throwaway worktree. The probe adds stderr lines in `DocumentView::text_for`
(`text: slot` or `text: build`) and in `offer_text` (`text: stored <page>`); no
product code carries a trace. Oracles:

- **Prefetch hit:** open `selection.pdf` and wait, with a bounded timeout, for
  the `text: stored 0` line. Do not use a fixed sleep: a slow build would
  correctly fall back and fail the check for nothing. Then post a left press
  and release to the `LitePDFPdfCanvas` HWND → `text: slot`.
- **Negative control:** post a page change and Ctrl+A back to back →
  `text: build`. `select_all` has no bitmap gate and the page change has just
  emptied the slot, so this proves the trace can say no. A press would not: it
  is refused until the new page's bitmap lands (`own_bitmap()`).
- **Correctness:** post Ctrl+A → `GetMenuState(IDM_EDIT_COPY)` enabled; post
  Ctrl+C → the clipboard text equals the unit oracle's `copy` for that page.
- **Password gate:** `encrypted.pdf` after the password, Ctrl+A twice →
  `text: build`, then `text: slot`. Ctrl+A is used because it does not depend
  on whether the unauthenticated workers deliver a bitmap.

### 5.3 New fixtures

One new generator, `scripts/generate-prefetch-fixtures.py`, beside the existing
per-fixture generators, writes literal PDF objects with no reportlab dependency.
Both files are uncompressed, so the bytes stay identical across runs. As with
every existing fixture, the generator is run once and both outputs are committed
under `tests/fixtures/`; the build never runs a generator.

- `type3.pdf`: one page of text drawn with a hand-written Type3 font: a few
  glyphs drawn as filled rectangles, with an `/Encoding /Differences` that maps
  them to letters, so the extracted text is known.
- `annot.pdf`: one page with known body text plus a FreeText annotation whose
  explicit `/AP /N` appearance stream draws different, known text. MuPDF runs an
  existing appearance stream as is. The page's `copy` must contain the body
  text and not the annotation text.

### 5.4 Measurement (not a gate)

`--bench-selection` is unchanged, and its `acq1st` column is the regression
guard for the synchronous fallback. Once, during implementation, measure on the
issue's documents (the HTML spec's p145 and p752, the news page): the worker-side
build after a full render, and after an L1 hit on the other worker; and
page-turn bitmap latency with prefetch on vs off while paging steadily. Record
the numbers in the PR. That checks §2's premises and §3.3's residual. It is not
asserted in CI (bench machine noise).

## 6. Risks this design pins

1. **Builder parity.** One builder, options fixed at `{}`, never the display
   list; enforced by the equivalence oracle and its mutant (5.1-1).
2. **Unauthenticated worker documents.** The prefetch is gated on
   `needed_password()` (3.6, 5.1-2).
3. **Handoff and lifetime.** A `(epoch, page)` key, a post helper that frees on
   every failure branch, a handler that never touches a dead view, and a Type3
   lifetime oracle (3.4, 5.1-6).
4. **Worker contention.** A text build must never delay a newer render that is
   already queued, or a tab close (3.3 rules 1-3, 5.1-5, 5.4).

## 7. User-facing docs

`CHANGELOG.md`, `## [Unreleased]` → `### Fixed`: a page's text is now read in
the background once the page is shown, so a text-selection press or Select All
on it no longer pauses LitePDF. A press that comes in before that background
read has finished still reads the text on the spot, as before (#98). README: no change,
since nothing visible or configurable changes.

## 8. Out of scope

- **Search scans' cold build under `doc_mutex`** (`page_hits`, the second half of
  #98). A press no longer waits behind a scan once the page's prefetch has
  landed; the rest gets a separate issue.
- **Render workers and password-protected documents.** Tracked separately; the
  §3.6 gate can be lifted once the workers authenticate.
- **Aborting a build in progress.** `fz_new_stext_page_from_page_with_cookie`
  could stop a build when a newer P0 arrives or the engine stops, which would
  shrink §3.3's one-build residual. Add it only if §5.4 shows that residual
  matters.
- **Prefetching neighbour pages' text.** The P1 renders do not request it.
  Revisit only if measurement shows the first press after a page turn still
  landing before the P0 prefetch.
- **Damaged PDFs.** The tab Document repairs a damaged file during open (#119's
  page-0 lookup), while a worker document repairs lazily on first use. Whether
  the two can ever yield different text for a page is unverified, and the
  equivalence fixtures are all clean. Accepted: a difference could only change
  what a press selects on such a file, never memory safety.
- **The open questions from #98's review.** Workers evicting the Document's
  fonts from the shared store matters less once the Document path is only the
  fallback. Whether a worker using the other worker's display list can run a
  Type3 glyph procedure against that worker's document (`font.c:1798,1816`)
  predates this change; no fixture exercises it.
