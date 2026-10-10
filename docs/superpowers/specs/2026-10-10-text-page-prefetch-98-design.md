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
on its owning document (`pdf-store.c:36`, `pdf_get_indirect_document`). Nothing
a worker loads can be found through the tab Document.

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
it renders the current page, and hand the handle to the UI thread.**

- The worker never takes `doc_mutex`: it uses its own context and `fz_document`.
- The fonts are warm. The worker has just interpreted this page into its own
  store, so the text pass costs about the issue's "warm" column (0.9-4 ms), not
  the cold one. The exception is a render served from the other worker's display
  list or from the pixmap cache: the page is then interpreted by this worker for
  the first time. The cost lands on a worker either way, never on the UI thread.
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
  `RenderEngine.cpp:150-151`), while `fz_new_stext_page_from_page` runs
  `fz_run_page_contents` only (`util.c:313`). Selection and copy would start to
  include annotation and form-field text, and disagree with `page_text`.
- **Sharing one store across documents.** Impossible by construction: the store
  key carries the owning document.

## 3. Design

### 3.1 One builder (`core`)

Today's body of `Document::text_page` after it takes the lock becomes a single
function shared by both callers:

```
// core/detail/TextPageBuilder.hpp -- MuPDF types allowed here.
Document::TextPage build_text_page(fz_context* ctx, fz_document* doc, int index) noexcept;
```

It clones the handle's `EscrowContext` from `ctx`, checks `index` against
`fz_count_pages` inside `fz_try`, loads the page, and calls
`fz_new_stext_page_from_page` with `fz_stext_options opts = {}`. Then it warms
`full_range`, as today. `Document::TextPage` befriends it.

- `Document::text_page` keeps its contract (noexcept, deferred `unique_lock` on
  `doc_mutex`, empty handle on every failure) and calls the builder with
  `impl_->ctx` / `impl_->doc` under the lock.
- The worker calls it with its own context and document, and takes no lock.

One function is the point. Spec §3.1 of the #52 design fixes two options that
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
always explicit.

### 3.3 Render request and worker (`core::RenderEngine`)

`RenderRequest` gains a last member, so existing aggregate initialisers keep
compiling:

```
// (#98) Called on the worker, after on_complete has delivered a non-null
// pixmap, with the page's text built on the worker's own document. Not called
// if the request was cancelled, the render failed, or the build failed.
std::function<void(Document::TextPage)> on_text;
```

Worker behaviour, when `on_text` is set:

- On **both** successful delivery paths (the L1 pixmap-cache hit at
  `RenderEngine.cpp:120-131` and the full render), after `on_complete` has run
  with a non-null pixmap:
  1. If cancelled, stop.
  2. Otherwise call `build_text_page(ctx, wdoc, req.page_num)`.
  3. If cancelled again, or the handle is empty, drop the handle and stop.
  4. Otherwise call `on_text(std::move(handle))`.
- The text is built **after** the pixmap is posted, so the bitmap never arrives
  later than today.
- No `on_text` call on a null pixmap. A page whose render failed cannot be
  pressed anyway (`own_bitmap()` refuses).

Dropping an unwanted handle on the worker is safe. A `TextPage` frees its stext
page through its own escrow, and no other thread touches it.

### 3.4 Posting to the UI thread (`ui::PdfCanvas`)

A new message `WM_USER_TEXT_READY` (the plan picks an id that is free across
`src/` and records the reservation beside `WM_USER_RENDER_DONE`'s in
`MainWindow.cpp`). It carries a heap `TextMeta { Document::TextPage text;
std::uint64_t epoch; }` in LPARAM.

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

`DocumentView::Impl` holds one `Document::TextPage text_slot`: at most the
current page's text, per tab.

| Member | Behaviour |
|---|---|
| `bool has_text_for(int page) const noexcept` | `text_slot.page() == page`. |
| `void offer_text(Document::TextPage t)` | Stores `t` iff it is valid, `t.page() == current_page`, and `!has_text_for(current_page)`. Otherwise drops it. A handle already in the slot is never replaced, so a sync-built handle is not churned by a late worker one. |
| `Document::TextPage text_for(int page)` | If `has_text_for(page)`, returns `text_slot.share()`. Otherwise builds synchronously with `doc.text_page(page)` (today's path), stores it if valid, and returns a share. |
| `set_current_page` | Drops the slot when the page actually changes, so memory stays bounded to one page per tab. |
| `request_render_with_prefetch(page, cb, TextCb on_text = {})` | Attaches `on_text` to the P0 request iff `wants_text_prefetch(page)`. Never to P1 neighbours or thumbnails. |
| `bool wants_text_prefetch(int page) const noexcept` | `!dual_page && !doc.needed_password() && !has_text_for(page)`. Public, for the unit tests. |

The slot outlives a tab switch, so returning to a tab whose current page was
already prefetched costs nothing. It dies with the tab. Its position among
`Impl`'s members does not matter: the handle's escrow keeps the MuPDF root alive
past the Document.

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
`authenticate()`; it reads `impl_->needs_password`, which only `close()`
resets. `wants_text_prefetch` refuses such documents, which keep today's
synchronous path. The slot still caches their sync-built handle.

Whether the render workers draw such documents correctly at all is a separate
question with the same cause. It is tracked outside this change (§8).

### 3.7 Callers (`ui`)

- The three single-page submission sites pass a text callback:
  `MainWindow::kick_render`, `PdfCanvas::resubmit_current_page` and the
  single-page tail of `PdfCanvas::navigate_to_page`. The spread branches pass
  none. The callback captures `target` and `epoch`:

  ```
  [target, epoch](litepdf::core::Document::TextPage t) {
      PdfCanvas::post_text_ready(target, std::move(t), epoch);
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
| Ctrl+A immediately after open, before any bitmap | Synchronous build, as today. |
| Second and later presses on the same page | The slot's handle; no rebuild. Today each press rebuilds warm (0.9-4 ms). |
| Password-protected document | Never prefetched; first press builds synchronously, as today; later presses use the slot. |
| Spread mode | No prefetch. Presses and Select All are already refused there. |
| Page change | Slot dropped; the new page's P0 render requests text. |
| Same-page zoom, resize, DPI change, Invert Colors | Re-render without a text request (`has_text_for` is true). A request cancelled by the invert or layout toggle delivers no text. |
| Tab switch away and back | The slot survives in the tab's `DocumentView`. |
| Tab closed with a `WM_USER_TEXT_READY` queued | Epoch mismatch: the handle is dropped through its own escrow; the dead view is not touched. |
| Render failed (null pixmap) | No text build. |
| A page with no text (CBZ, image-only) | Valid handle, `full_range` false, same as the synchronous path. |

Results are unchanged in every case. Selection, quads and copied text are
byte-for-byte what `Document::text_page` produces (§5.1). Only where and when
the build happens changes.

## 5. Testing

### 5.1 Unit tests (no HWND)

1. **Equivalence oracle.** For every page of `selection.pdf`, `search.pdf`,
   `large.pdf`, `simple.pdf`, `bookmarks.pdf`, the three `cjk-*.pdf`,
   `sample.epub` and the new Type3 fixture (5.3): a worker-built handle (a
   `DocumentView` render with `on_text` capturing into a promise, bounded wait)
   against `Document::text_page`. Assert equal `full_range` points, equal
   `highlight(first, last)` quads, and equal `copy(first, last)` text. The
   wrong-builder mutant, building from the L2 display list, must fail it on a
   fixture with annotation text. No current fixture is known to carry any; the
   plan checks, and adds a FreeText annotation page to a generator if needed.
2. **Gate.** `wants_text_prefetch` is false for `encrypted.pdf` after
   `authenticate`, false in spread mode, false once `has_text_for(page)`, and
   true otherwise. `Document::needed_password()` stays true after
   `authenticate()` and becomes false after `close()`.
3. **Slot.** `offer_text` drops a handle for a page other than the current one
   and keeps an existing handle. `set_current_page` to another page empties the
   slot; to the same page it does not. `text_for` on a filled slot returns a
   handle for that page.
4. **Share.** Dropping one of two shared handles leaves the other usable; the
   MuPDF root is released only after both are gone (`live_mupdf_roots`).
5. **Cancellation.** A request whose cancel flag is set before submit gets
   `on_complete(nullptr)` and no `on_text`.
6. **Lifetime.** A worker-built handle outlives its `DocumentView` (the engine,
   the worker documents and the Document all destroyed). `copy()` on it still
   works, and dropping it returns `live_mupdf_roots` and
   `family_context_count` to baseline. Run it on `selection.pdf` and on the Type3
   fixture. Add the same Type3 case for today's path, a `Document::text_page`
   handle outliving its `Document`. That closes the gap the review found: the
   decouple-then-drop path (`pdf-xref.c:1989-1994`, `font.c:188-197`) has never
   had a test, because no fixture has a Type3 font.

Test names are ASCII and prefixed by subsystem. Do not hard-code the ctest
count; `ctest -N` at implementation time is the reference.

### 5.2 Scripted GUI check

An untracked driver under `build/gui-check/`, using a probe exe built in a
throwaway worktree. The probe adds one stderr line in `DocumentView::text_for`
(`text: slot` or `text: build`), and no product code carries a trace. Oracles:

- **Prefetch hit:** open `selection.pdf`, wait for the status-bar zoom readout,
  then 500 ms. Post a left press and release to the `LitePDFPdfCanvas` HWND →
  `text: slot`.
- **Negative control (race):** post a page change and the press back to back
  (the #120 recipe) → `text: build`. This proves the trace can say no.
- **Correctness:** post Ctrl+A → `GetMenuState(IDM_EDIT_COPY)` enabled; post
  Ctrl+C → the clipboard text equals the unit oracle's `copy` for that page.
- **Password gate:** `encrypted.pdf` after the password → `text: build` on the
  first press, `text: slot` on the second.

### 5.3 New fixture

`type3.pdf`: one page whose text is drawn with a hand-written Type3 font (a few
glyphs drawn as filled rectangles, with an `/Encoding /Differences` that maps
them to letters, so that the extracted text is known). It is written by a new
`scripts/generate-type3-fixture.py`, beside the existing per-fixture generators,
as literal PDF objects with no reportlab dependency. It is uncompressed, so the
bytes stay identical across runs.

### 5.4 Measurement (not a gate)

`--bench-selection` is unchanged, and its `acq1st` column is the regression
guard for the synchronous fallback. Once, during implementation, measure the
worker-side build on the issue's documents (cold Document vs worker after
render) and record the numbers in the PR. That checks §2's "warm" premise; it is
not asserted in CI (bench machine noise).

## 6. Risks this design pins

1. **Builder parity.** One builder, options fixed at `{}`, never the display
   list; enforced by the equivalence oracle and its mutant (5.1-1).
2. **Unauthenticated worker documents.** The prefetch is gated on
   `needed_password()` (3.6, 5.1-2).
3. **Handoff and lifetime.** A `(epoch, page)` key, a post helper that frees on
   every failure branch, a handler that never touches a dead view, and a Type3
   lifetime oracle (3.4, 5.1-6).

## 7. User-facing docs

`CHANGELOG.md`, `## [Unreleased]` → `### Fixed`: the first text-selection press
or Select All on a page no longer pauses LitePDF while the page's text is read;
it is read in the background once the page is shown (#98). README: no change,
since nothing visible or configurable changes.

## 8. Out of scope

- **Search scans' cold build under `doc_mutex`** (`page_hits`, the second half of
  #98). A press no longer waits behind a scan once the page's prefetch has
  landed; the rest gets a separate issue.
- **Render workers and password-protected documents.** Tracked separately; the
  §3.6 gate can be lifted once the workers authenticate.
- **Prefetching neighbour pages' text.** The P1 renders do not request it. Revisit
  only if measurement shows the first press after a page turn still landing
  before the P0 prefetch.
- **The open questions from #98's review.** Workers evicting the Document's
  fonts from the shared store matters less once the Document path is only the
  fallback. Whether a worker using the other worker's display list can run a
  Type3 glyph procedure against that worker's document (`font.c:1798,1816`)
  predates this change; no fixture exercises it.
