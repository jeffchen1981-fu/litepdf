# Text selection + copy (#52) and click-drag hand-tool panning (#58) — design

Date: 2026-09-16
Base: `main` @ `88513f22116992de652c3900e023089bc3f1a494` (v1.3.0)
Issues: [#52](https://github.com/jeffchen1981-fu/litepdf/issues/52), [#58](https://github.com/jeffchen1981-fu/litepdf/issues/58)

LitePDF can find text but cannot hand it to the user: there is no selection model
and no clipboard path. This design adds both, and — because the two features
contend for the same gesture — settles click-drag panning at the same time.

Both issues are designed here; they ship as two PRs, #52 first.

---

## 1. Scope and sequencing

| | PR-1 (#52) | PR-2 (#58) |
|---|---|---|
| Gestures | left-drag, double-click, triple-click | middle-drag, space+left-drag |
| Commands | Ctrl+C, Ctrl+A, Edit menu | — |
| Shared groundwork built here | mouse capture lifecycle, `WM_SETCURSOR`, `CS_DBLCLKS` | consumes it |

The in-repo precedent for the whole capture + `WM_SETCURSOR` lifecycle is
`src/ui/Splitter.cpp:55-175` — the only window in the project that already does
capture-based dragging. Follow it, including its message ordering (§4.2).

`VERSION` is not bumped: this project bumps only at phase boundaries
(`project_litepdf_ship_version_convention`).

### Out of scope, with the reason

- **Selection in two-page spread mode — both painting AND input.** `on_paint`'s
  dual branch returns at `PdfCanvas.cpp:1332`, before the overlay block at
  `:1351`, and its comment records the search-hit overlay as deliberately
  deferred there (`:1231-1232`, "R17 — deferred per plan §Out of scope").
  §4.2's mouse-down must therefore refuse to start a drag while `dual_page` is
  set. It is not enough to skip *painting*: `impl_->current_bitmap` is non-null
  in dual mode — it holds the left slot's bitmap (`PdfCanvas.cpp:1303`) — so a
  "no bitmap, stop" guard does not gate it, and a drag would compute points with
  single-page geometry, producing an invisible but Ctrl+C-copyable wrong selection.
- **Cross-page selection.** A selection is bound to one page. Spanning pages
  needs concatenated stext across pages and belongs with continuous scroll (#55).
- **Rectangular / marquee selection.** Different rendering (a drag rectangle, not
  per-line quads), different use case (tables and columns), and absent from both
  Chrome's and Edge's PDF viewers. (An earlier draft wired `fz_copy_rectangle`
  here because Select All was thought to need it; §3.4 removed that need, so no
  part of the marquee API is used by this design.)
- **Right-click context menu.** Nothing in `src/` calls `TrackPopupMenu`,
  `WM_CONTEXTMENU` or `CreatePopupMenu`, so this is new infrastructure, and it
  would immediately attract unrelated items (rotation #53, a hand-tool mode
  toggle). Its own issue.
- **Rotated-text quads** are drawn as axis-aligned bounding boxes, matching what
  the search overlay already does (`PdfCanvas.cpp:1356-1358`).

---

## 2. Data model

New header `src/core/TextSelection.hpp` — pure data, no MuPDF, no Win32, so it is
headless-unit-testable like `ViewportMath.hpp` and `PdfCanvasLayout.hpp`.

```cpp
namespace litepdf::core {

struct SelPoint { float x = 0.0f, y = 0.0f; };   // page-box-relative points

// Maps to FZ_SELECT_CHARS / _WORDS / _LINES.
enum class SelectMode { Chars, Words, Lines };

struct Quad { float ul_x, ul_y, ur_x, ur_y, ll_x, ll_y, lr_x, lr_y; };

struct TextSelection {
    int        page = -1;

    // RAW, UN-SNAPPED mouse positions. See the warning below.
    // Select All is NOT a separate kind: it is an ordinary Range whose two
    // points bracket the page's first and last characters (§3.4).
    SelPoint   anchor{};
    SelPoint   extent{};
    SelectMode mode = SelectMode::Chars;

    // Materialised when the gesture ends. Both are page-box-relative / UTF-8, so
    // everything afterwards — painting on every WM_PAINT, Ctrl+C, surviving a tab
    // switch — is pure data touching neither MuPDF nor any lock. See §3.3.
    std::vector<Quad> quads;
    std::string       text_utf8;   // CRLF line endings
};

}  // namespace litepdf::core
```

**Units.** Every coordinate in this struct — and everything crossing the
`TextPage` API in §3.1 — is **MuPDF page space**, which is page-box-relative: the
origin is the page box's top-left corner, matching the rendered bitmap's pixel
(0,0). MuPDF establishes that itself for every format LitePDF opens, so no
translation exists anywhere in the selection path (§4.1, corrected at plan time).

### The anchor must stay un-snapped

`fz_snap_selection` takes its two points **in-out** and reorders them:

```c
// third_party/mupdf/source/fitz/stext-search.c:304-308
	start = find_closest_in_page(page, *a);
	end = find_closest_in_page(page, *b);

	if (start > end)
		idx = start, start = end, end = idx;
```

after which `*a` receives the position earlier in reading order and `*b` the
later one — regardless of which was the anchor. Writing the snapped results back
over `anchor`/`extent` therefore destroys the anchor on any backward drag: the
next `WM_MOUSEMOVE` would extend from the *previous cursor position* instead of
from the mouse-down point, and the selection walks across the page.

So: `anchor` and `extent` hold raw mouse positions for the life of the drag, and
snapping is applied only to copies. `TextPage::snap` returns rather than taking
in-out references precisely so this cannot be done by accident. MuPDF's own
viewer keeps the same discipline (`platform/gl/gl-main.c:1560` holds a raw
`static fz_point pt` and recomputes `page_a`/`page_b` from it each frame).

### Where the selection lives

`DocumentView` holds `std::optional<TextSelection>` with
`selection()` / `set_selection()` / `clear_selection()`.

Per-tab state in this codebase has two homes, and the distinction is load-bearing:

| Home | Holds | On tab switch |
|---|---|---|
| `core::Tab` (`TabList.hpp:24`) | `pan_x`, `pan_y`, `outline_visible`, `thumb_visible` | snapshotted and restored by hand (`MainWindow.cpp:614-617`, `:637`) — the canvas is a shared singleton that only *parks* these |
| `core::DocumentView` | `current_page`, `zoom_pct`, `invert_colors()`, `dual_page()` | read straight off `incoming->view` (`MainWindow.cpp:646-652`) |

A selection is document-bound state, so it belongs in the second group and
`on_tab_switch` needs no new code. **Tab *close* is a different matter and is not
covered by that** — see §3.3.

### Lifetime (decision D2, model "1b")

Exactly one selection per tab. It **persists across page changes** and is cleared
only by the next left-click in `Chars` mode, a new drag, or `clear_selection()`.
It is not written to `session.json`.

Rejected alternatives and why:

- *Cleared on page change.* The mainstream readers (Chrome, Edge, Acrobat) are
  continuous-scroll and so never clear on navigation; clearing would be more
  forgetful than the convention it is imitating.
- *One selection per page (`std::map<int, TextSelection>`).* Copy is always
  one-selection-at-a-time, so retained selections on other pages can never be
  acted on. It buys invisible state with no payoff, makes Ctrl+C page-dependent,
  needs a "clear all" affordance that has no home, and maximises the collision
  with the search-hit overlay that shares the same visual channel.

Consequence of persistence: with the selection on page 5 and the reader on page 8,
Ctrl+C copies text that is not on screen. Chrome and Acrobat behave identically;
accepted.

---

## 3. Engine layer

MuPDF 1.27.2 ships most of the selection model
(`include/mupdf/fitz/structured-text.h:671-698`): `fz_snap_selection`,
`fz_highlight_selection` and `fz_copy_selection`. No character-index model,
point-to-character hit-testing, reading-order flattening or quad merging needs to
be written.

`fz_highlight_selection(a, b, quads, max_quads)` is the **only** quad producer in
the whole selection block — there is no whole-page variant. That shaped §3.4:
rather than hand-writing one, Select All is expressed as a point pair this
function already handles.

### 3.1 `Document::TextPage` — an opaque handle on its own escrow context

```cpp
class Document {
public:
    // A reference to one page's extracted text.
    //
    // The handle owns a ref on the fz_stext_page and its own core::EscrowContext
    // — a cloned context that ALSO keeps the MuPDF lock table alive. Both are
    // required for the handle's lifetime to be independent of the Document that
    // produced it (§3.3). EscrowContext lands first, in its own PR, as the fix
    // for #61.
    //
    // Coordinates are MuPDF page space, already page-box-relative (§2 "Units").
    class TextPage {
    public:
        TextPage() noexcept;                 // empty
        ~TextPage();
        TextPage(TextPage&&) noexcept;
        TextPage& operator=(TextPage&&) noexcept;
        TextPage(const TextPage&)            = delete;
        TextPage& operator=(const TextPage&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] int  page()  const noexcept;

        // Snapped COPIES. Deliberately returns rather than taking in-out
        // references: an in-out signature is precisely the shape that lets a
        // caller write the reordered result back over the raw anchor and
        // produce the walking-selection defect described in §2.
        // Chars mode returns its inputs unchanged, as MuPDF's own viewer does.
        // Words/Lines also restore the far end fz_snap_selection leaves unwritten
        // when it lies past the page's last character (§4.2).
        struct Snapped { SelPoint a, b; };
        [[nodiscard]] Snapped snap(SelPoint a, SelPoint b, SelectMode m) const noexcept;

        // Merged per-line highlight quads. Lock-free.
        [[nodiscard]] std::vector<Quad> highlight(SelPoint a, SelPoint b) const;

        // The leading edge of the page's first character and the trailing edge
        // of its last, each at mid-height. Feeding these to highlight() / copy()
        // selects the whole page through the ordinary path — see §3.4 for why
        // Select All is expressed this way rather than as the page's bounding
        // corners, and why edges rather than origins.
        // Returns false for a page with no text.
        [[nodiscard]] bool full_range(SelPoint& first, SelPoint& last) const noexcept;

        // UTF-8, CRLF line endings. Lock-free as well, despite allocating —
        // see §3.2: it runs on the handle's own escrow context.
        [[nodiscard]] std::string copy(SelPoint a, SelPoint b) const;

    private:
        struct Impl;
        // EscrowContext + fz_stext_page*
        std::unique_ptr<Impl> impl_;
    };

    // Empty handle if the document is not open, `page` is out of range, or
    // extraction or the context clone fails. Callers MUST tolerate an empty
    // handle — a drag that cannot acquire one simply does not start.
    [[nodiscard]] TextPage text_page(std::size_t page) const noexcept;
};
```

The header stays PIMPL-clean: no `fz_point`, `fz_quad` or `fz_stext_page` leaks,
for the same reason `PageHit` uses bare floats. (`Document.cpp` itself already
includes `fitz.h` and already walks stext in `page_text` / `page_hits`; the
discipline is about the header.)

**stext options are pinned to `fz_stext_options opts = {}`** — the same as
`page_text` (`Document.cpp:423`). Two flags must specifically NOT be used:

- `FZ_STEXT_COLLECT_STRUCTURE` nests text under `FZ_STEXT_BLOCK_STRUCT`, while
  `fz_enumerate_selection` (`stext-search.c:274`) and `fz_snap_selection` (`:316`)
  walk only top-level `FZ_STEXT_BLOCK_TEXT` blocks — selection would find nothing.
- `FZ_STEXT_DEHYPHENATE` (which `page_hits` uses, `Document.cpp:503`) changes line
  joining and would make copied text disagree with `page_text` for the same region.

`fz_highlight_selection` is a `(quads, max_quads)` interface returning a count,
and silently drops the remainder when the buffer is full — `on_highlight_char`
merges into the last quad *before* the cap test and only drops at
`hits->len == hits->cap` (`stext-search.c:417-418`), so "returned count equals
capacity ⇒ grow and retry" has no false negative. Start at 256 and double.

Error handling: `fz_snap_selection` and `fz_highlight_selection` cannot throw.
`fz_copy_selection` allocates and can throw, so it sits inside
`fz_try` / `fz_catch` exactly like `page_text` does.
`fz_new_stext_page_from_page` rethrows after dropping its partial page
(`util.c:320-324`).

### 3.2 Why the query path needs no lock — and exactly how far that reaches

Verified against the vendored source:

- Lines 55–445 of `stext-search.c` — which contain `find_closest_in_*`,
  `fz_enumerate_selection`, `fz_snap_selection` and `fz_highlight_selection` —
  contain **no** `fz_malloc` / `fz_free` / `fz_throw` / `fz_try` / `fz_lock` /
  `fz_keep_*` / `fz_drop_*` / `fz_new_*` / `fz_append_*` call. `ctx` is passed
  onward in a few places (`:284`, `:289`, `:441`) but nothing in the range
  consumes it.
- `fz_keep_stext_page` / `fz_drop_stext_page` route through `fz_keep_imp` /
  `fz_drop_imp`, which take `FZ_LOCK_ALLOC` around the refcount
  (`include/mupdf/fitz/context.h:928`). `Document::Impl` installs a real
  `std::array<std::mutex, FZ_LOCK_MAX>` lock table (`Document.cpp:38`, `:81-85`),
  so refcounting is genuinely thread-safe.

So `snap()`, `highlight()` and `full_range()` read an immutable structure the
handle owns a ref to, and need no `doc_mutex`.

**The lock-free region does NOT extend to releasing the handle.** The above
covers one file; `fz_drop_stext_page` lives in `stext-device.c:294-305`, and its
zero-refcount path runs `drop_run` → `fz_drop_font` → possibly
`fz_warn(ctx, "FT_Done_Face(%s): %s", …)` (`font.c:218-225`) → `fz_vwarn`, which
mutates `ctx->warn.message` and `ctx->warn.count` with **no lock at all**
(`error.c:104-123`). On a shared context that is the two-threads-one-`fz_context`
race `doc_mutex` exists to prevent. The escrow (§3.3) removes it: `fz_context`
declares `fz_warn_context warn;` as a **by-value member** (`context.h:872`), so
each clone has its own, and a drop on the escrow touches nothing shared. The font
object itself is refcounted under the installed lock table.

**What still takes `doc_mutex`: acquiring a handle, and nothing else.** Building
the stext loads a page from the `Document`'s `fz_document`, which must be
serialised with every other user of that document.

`copy()` does **not** take it, even though it allocates. An earlier draft said it
did, reasoning from the pre-escrow design in which `copy()` ran on the
`Document`'s own context. On the escrow it no longer does, and the claim was both
unnecessary and unimplementable:

- *Unnecessary.* `doc_mutex` exists to serialise the shared context's
  `fz_try` / `fz_catch` error stack (`Document.cpp:62-79`). `fz_context` declares
  `fz_error_context error;` as a **by-value member** (`context.h:871`), so the
  escrow has its own stack. Its allocations go through the allocator callbacks
  under `FZ_LOCK_ALLOC`, which the shared lock table already makes thread-safe.
- *Unimplementable.* The handle holds no reference to the `Document`, and is
  deliberately designed to outlive it — there is no `doc_mutex` it could reach.

This matters because the alternative was measurably worse: `SearchSession::set_query`
submits one `page_hits` task per page to a 2-worker pool
(`SearchSession.cpp:194-199`), and each holds `doc_mutex` for a whole page's
extraction-and-search with no pacing between tasks. `std::mutex` is not fair, so a
UI-thread acquire inside `WM_MOUSEMOVE` could lose repeatedly during a scan. With
the hot path lock-free, a drag contends only once — at mouse-down.

### 3.3 Why the handle carries an escrow context, and why the selection caches its results

**The escrow is a correctness requirement, not an optimisation.** Closing a tab
destroys the `Document` *before* anything informs the canvas:

```cpp
// src/core/TabList.cpp:33-34 — the Tab (and its DocumentView -> Document -> ctx) dies here
    const bool removing_active = (static_cast<int>(i) == active_);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
```

```cpp
// src/ui/TabManager.cpp:352-358 — only afterwards does set_view get reached
    const int new_active = impl_->list.remove(static_cast<std::size_t>(index));
    ...
    if (active_changed && impl_->on_switch) {
        impl_->on_switch(new_active, -1);
```

and `MainWindow::on_tab_close_request` does no canvas teardown before calling
`tabs_->close_tab(index)` (`MainWindow.cpp:732-751`). Mouse capture does not
protect this: capture redirects *mouse* input only, Ctrl+W is in the accelerator
table (`MainWindow.cpp:2238`), and the canvas holds focus during a drag because
`WM_LBUTTONDOWN` calls `SetFocus`. So a handle bound to the `Document`'s own
context would be dropped against a freed `fz_context`. `close_tab`'s own comment
already names this hazard class ("a freed-view touch at worst").

The escrow is the pattern this project already uses for exactly this shape: the
per-render clone-escrow that lets the UI thread drop a pixmap on the right MuPDF
root "even if the producing `DocumentView` is torn down before the message lands"
(`PdfCanvas.hpp`, `post_render_done`), recorded in `mupdf-refcount-conventions`.
Reuse it rather than inventing teardown ordering.

**A cloned context is NOT sufficient on its own, and this is the part that is
easy to get wrong.** `fz_clone_context` `memcpy`s the whole `fz_context`
(`context.c:324-330`), including the `fz_locks_context` — whose `user` pointer
this project sets to a `MuPDFLocks` object owned by `Document::Impl` through a
`std::unique_ptr`:

```cpp
// src/core/Document.cpp:54-84
struct Document::Impl {
    std::unique_ptr<MuPDFLocks> locks;
    ...
    Impl() : locks(std::make_unique<MuPDFLocks>()) {
        locks->fz.user = locks.get();
        locks->fz.lock = &litepdf_lock;
```

MuPDF keeps the master `fz_context` alive as a husk while clones exist (that is
what the `master` / `context_count` fields at `context.h:856-863` are for), but
it knows nothing about *our* lock table. When the `Document` dies, `locks` is
freed, and the escrow's next `fz_lock(ctx, FZ_LOCK_ALLOC)` — which every
`fz_keep_*` / `fz_drop_*` performs — calls `litepdf_lock` with a dangling `user`.
Dropping the stext page on an "independent" escrow would therefore still be a
use-after-free, just one indirection further out than the original defect.

**So `Document::Impl::locks` becomes a `std::shared_ptr<MuPDFLocks>`, and every
escrow holds a copy.** The table then outlives the last clone by construction.

**Holding all three is not enough — they must be released in one order.**
Dropping the stext page takes `FZ_LOCK_ALLOC` and frees through the escrow's
allocator callbacks; dropping the escrow context takes the same lock. So:

1. `fz_drop_stext_page(escrow, page)` — needs a live context *and* live locks
2. `fz_drop_context(escrow)` — needs live locks
3. release the `shared_ptr<MuPDFLocks>` — last, because both steps above call into it

Releasing the lock table first, or dropping the context before the page, reads
freed memory on exactly the post-`Document` path this handle exists to make safe.

**Plan-time refinement: steps 2 and 3 live in one class.** `core::EscrowContext`
(move-only; `clone_from(fz_context*)`, `get()`) holds the cloned context and the
type-erased lock-table reference, and its destructor drops the context before
releasing the table. The table is recovered from the source context itself —
`ctx->locks.user`, trusted only after `ctx->locks.lock` is confirmed to be
litepdf's own callback — so a caller cannot forget to thread it through. It lands
first, as the fix for #61, whose render escrow needs exactly the same thing.
`TextPage::Impl` is then `{ EscrowContext escrow; fz_stext_page* stext; }`, and its
destructor body performs step 1 through the escrow, which C++ keeps alive until
after the body has run.

Two things this design was checked for and found to hold, recorded so they are
not re-litigated: MuPDF keeps the master `fz_context` alive as a husk until its
last clone is dropped (`context.c:167-226`), and `fz_drop_pool` stores no
master-context pointer — it only walks its nodes calling `fz_free(ctx, node)`
(`pool.c:116-130`) — so freeing a page's pool through the escrow after the master
is gone is sound. `Document` creates its context with `fz_new_context(nullptr, …)`,
i.e. the default allocator, which is process-global.

Two consequences to record rather than discover later:

- The **existing per-render escrow has the same latent flaw** — it clones a
  context and drops it on the UI thread, with nothing keeping the lock table
  alive. It is not reachable often (the message must be processed after the
  `Document` dies). Tracked as #61 and fixed first, in its own PR, by moving
  `RenderMeta` onto `EscrowContext`; #52 then builds on that class rather than
  adding a *second* instance of the defect.
- A process-wide singleton lock table was considered and rejected: it would fix
  both for free, but all `Document`s would then share one mutex array, which
  serialises cross-tab work on the allocator lock — a measurable change to the
  tab-level parallelism `Document.cpp:66-79` explicitly documents as preserved.

**Results are cached into the selection at the end of the gesture.** At mouse-up
(or at Select All), the final quads and text are materialised into the
`TextSelection` (§2). Everything afterwards — painting, Ctrl+C, surviving a tab
switch — is then pure data. Together with the escrow this means a `TextPage`
exists only for the duration of a gesture *and* is safe even if the document dies
inside it. Cost: a few KB per tab. Benefit: Ctrl+C is instant and lock-free, and
the paint path stays MuPDF-free.

Note also that app-exit teardown is safe only because `MainWindow.hpp:149-150`
declares `tabs_` before `canvas_`, so the canvas (and any handle it holds) is
destroyed first. That ordering is load-bearing and currently uncommented; the
implementation must add a comment saying so.

### 3.4 Select All is not two corner points

Setting `a` to the page's top-left and `b` to its bottom-right does **not** mean
"everything". `find_closest_in_page` resolves a point lying outside every line by
smallest vertical distance only — it is column-blind, and MuPDF says so:

```c
// third_party/mupdf/source/fitz/stext-search.c:222-225
				else
				{
					// Outside line
					// TODO: closest column?
```

On a two-column page the bottom-right corner resolves to the end of whichever
column ends lowest, so an entire column can fall outside `[start, end]` and be
dropped from both the highlight and the copied text — silently, with the
highlight faithfully showing the truncated result.

But the failure is specifically about points that lie **outside every line**.
A point that lands *on* a character resolves exactly. So Select All does not need
a separate mechanism at all — it needs better points:

**`full_range()` returns the leading-edge midpoint of the page's first character
and the trailing-edge midpoint of its last, in reading order**, found by a trivial
walk of `page->first_block` → lines → chars with no merging and no heuristic.
Feeding those two points to the ordinary `highlight()` and `copy()` resolves
`start` to index 0 and `end` to the index *after* the last character, and
`fz_enumerate_selection` then walks every character in index order.

**Edges, not origins — corrected at plan time.** An earlier version of this section
used the two characters' *origins*. `find_closest_in_line` resolves a point to the
nearest character **boundary** (each character contributes one at its `ll` edge and
one at its `lr` edge), and a selection is the half-open range `[start, end)` —
`fz_enumerate_selection` returns on `++idx == end`. An origin sits on a character's
leading edge, so the last character's origin resolves to the boundary *before* it
and Select All silently dropped the page's final character.

This is strictly better than the alternatives and removes work rather than adding
it:

- **No `highlight_all()`.** An earlier draft had it union each line's char quads
  into one quad and called that "exact, not heuristic". That was wrong.
  `on_highlight_char` starts a **new** quad whenever `is_near` fails —
  horizontal fuzz is `0.5 * ch->size` (`stext-search.c:396-418`, `:434`) — so
  MuPDF deliberately splits one line into several quads across wide gaps. A
  one-quad-per-line walk would paint a blue bar straight across the whitespace
  between table columns. Going through `fz_highlight_selection` reproduces the
  split for free because it *is* the same code.
- **No `fz_copy_rectangle`, and no `SelectKind`.** Select All becomes an ordinary
  `Range`, so both previously-recorded divergences — line joining, and zero-area
  glyphs dropped by `fz_is_empty_rect` — disappear: it is literally the same code
  path as a drag over the whole page.

`full_range()` returns false on a page with no text, and Select All is then a
no-op that leaves any existing selection alone.

---

## 4. Interaction layer (PR-1)

### 4.1 Coordinate mapping, both directions

The existing forward map is pinned to `zoom_pct`, not `render_scale`
(`ViewportMath.hpp`, the overlay block in `PdfCanvas::on_paint`), and is correct.
The reverse map is its inverse:

```cpp
// Render-target DIPs -> page points. Inverse of pdf_point_to_dip.
inline float dip_to_pdf_point(float dip, float zoom_pct) noexcept {
    if (!(zoom_pct > 0.0f)) return 0.0f;   // also rejects NaN
    return dip / zoom_pct;
}
```

A mouse position becomes a point by subtracting the page origin that
`place_bitmap` produced for this paint, then dividing by `zoom_pct`. The result is
clamped to `[0, width_pt] × [0, height_pt]`.

`WM_MOUSEMOVE` coordinates are client **pixels**; the canvas works in DIPs.
Conversion uses the same `px * 96 / dpi` factor as `bitmap_px_to_dip`.

**There is no page-box-origin offset to correct — corrected at plan time.** An
earlier version of this section claimed the rendered bitmap's pixel (0,0) and
stext quads differ by `bounds.x0/y0` for any PDF whose CropBox/MediaBox origin is
non-zero, and prescribed a translation inside `TextPage` plus a normalisation in
`Document::page_hits`. The premise is false. `pdf_page_obj_transform_box`
(`source/pdf/pdf-page.c`) ends by concatenating
`fz_translate(-cropbox.x0, -cropbox.y0)` onto the page CTM: MuPDF itself moves the
CropBox origin to (0,0), so `fz_bound_page`, the render bbox and every stext quad
already share an origin-free frame. Every other format LitePDF opens hard-codes a
zero origin in its `bound_page` (`xps`, `svg`, `epub`, `htdoc` for FB2, `cbz`,
`img`). So neither the translation nor the search-path change exists. The
CropBox-offset fixture page remains (§6.1), now as a test that **pins** this MuPDF
behaviour, so an upgrade that changed it fails loudly.

### 4.2 Drag lifecycle

State lives in `PdfCanvas::Impl`: `Gesture gesture` (`None` / `Selecting` /
`Panning`), `moved_past_threshold`, the `TextPage` handle, and the click-count
tracker. **There is no separate `dragging` boolean** — `gesture` is the single
source of truth for whether a gesture is live, and every row below reads and
writes it. (An earlier draft kept both, which is how capture loss during a *pan*
came to be unhandled: see the `WM_CAPTURECHANGED` row.)

**Message ordering is load-bearing.** `ReleaseCapture()` delivers
`WM_CAPTURECHANGED` to the releasing window **synchronously, inside the call**.
So the button-up arm must finish reading and committing its state *before* it
releases, exactly as `Splitter.cpp:151-169` already does (it clears its
`dragging` flag first, then calls `ReleaseCapture`). Releasing first would let the
`WM_CAPTURECHANGED` arm tear the drag down and release the handle, after which the
commit step would find `gesture` already `None` — a drag that highlights while the
button is held and then vanishes on release, copying nothing.

| Message | Action |
|---|---|
| `WM_LBUTTONDOWN` | `SetFocus`. Stop if `gesture != None`, if `dual_page` is set (§1), if there is no page bitmap, or if `text_page()` returns an empty handle. Otherwise: clear any existing selection, store the clamped point as `anchor`, set `mode` from the click count, clear `moved_past_threshold`, `SetCapture`, `gesture = Selecting`. |
| `WM_LBUTTONDBLCLK` | Same as `WM_LBUTTONDOWN` with `mode = Words`, and immediately commit a word selection (see below). A triple click is detected here and commits `mode = Lines`. |
| `WM_MOUSEMOVE` (`gesture == Selecting`) | Store the clamped point as `extent`; set `moved_past_threshold` once the displacement from `anchor` exceeds `SM_CXDRAG` / `SM_CYDRAG`; snap copies; `highlight()`; `InvalidateRect`. |
| `WM_LBUTTONUP` | **First of all**: feed the message's own coordinates through the `WM_MOUSEMOVE` path. The release position is not always preceded by a move for the same point -- injected input delivers a press and a release with nothing between -- and deciding without it turns a drag into a click that clears the selection. **Then**: decide and commit. If `mode == Chars` and `!moved_past_threshold`, this was a click — clear the selection (1b's "next click clears"). Otherwise materialise `quads` + `text_utf8`. `gesture = None`. **Then**: `if (GetCapture() == hwnd_) ReleaseCapture();` and release the handle. |
| `WM_CAPTURECHANGED` | **Whatever `gesture` is, set it to `None`**, releasing the handle without committing if it was `Selecting`. This must cover `Panning` as well as `Selecting`: capture taken by another window mid-pan would otherwise leave `gesture == Panning` forever, and since every button-down refuses to start while `gesture != None`, the canvas would accept no mouse gesture again for the rest of the session. (After a normal button-up this arm finds `gesture` already `None` and does nothing.) |
| `WM_MBUTTONDOWN` / `WM_RBUTTONDOWN` while `gesture == Selecting` | Abort the selection drag: `gesture = None`, release the handle, keep whatever was already committed, and do **not** start panning from this press. A *stale* selection (below) is not a drag: the middle press cancels it first and pans (#77). Either press also breaks the left click sequence, as it breaks Windows' own double-click pairing (#77). |
| `set_view` | Same teardown, and **first**: before `impl_->view` is reassigned (`PdfCanvas.cpp:239`). The teardown must not dereference `impl_->view` at all — it sets `gesture = None`, releases capture if held, and resets the handle, none of which needs the view. Placing it after the reassignment would run it against the incoming view; placing a view dereference inside it would, on the tab-close path, touch the outgoing view after `TabList::remove` has destroyed it (§3.3). |

**Only one gesture may be live at a time, and the state machine has to say so.**
Without the row above, pressing the middle button during a left-drag starts
panning while the selection drag is still live: every subsequent `WM_MOUSEMOVE`
both extends the selection and pans the page, and whichever button is released
first calls `ReleaseCapture` out from under the other gesture. §5's middle-drag
arm carries the mirror-image rule — it refuses to start while a selection drag is
live. A single `enum class Gesture { None, Selecting, Panning }` in
`PdfCanvas::Impl`, checked at every button-down, is the whole fix; two independent
booleans are what admits the overlap.

**A stationary double or triple click must still select.** With `CS_DBLCLKS`, a
double click delivers `WM_LBUTTONDOWN`, `WM_LBUTTONUP`, `WM_LBUTTONDBLCLK`,
`WM_LBUTTONUP`, and the drag threshold is non-zero (measured on this platform:
`SM_CXDRAG = SM_CYDRAG = 4`, `SM_CXDOUBLECLK = 4`, `GetDoubleClickTime() = 500`).
A rule that cleared every zero-movement release would therefore destroy the word
selection the double click had just made — the single most common "select this
word" gesture in any desktop application. Hence the `mode == Chars` qualifier
above: the click-clears rule applies to single clicks only, and a double or triple
click commits its selection at button-down time, with any subsequent drag
extending it in that mode.

Every exit path releases both the capture and the handle. The transitions are pure
logic and live in `ui/detail/SelectionDrag.hpp` so they can be unit-tested without
an HWND, following `SplitterMath.hpp` / `ViewportMath.hpp`.

**Plan-time refinements.**

- *Live versus committed.* While a gesture is live the canvas paints a **live**
  selection held in `PdfCanvas::Impl`, and writes to `DocumentView` only when the
  gesture commits (at button-up, or at press time for a double or triple click).
  Cancelling therefore just discards the live copy, which is what "keep whatever
  was already committed" in the table above needs.
- *A page change cancels a live drag.* A selection is bound to one page (§1); a
  drag that outlived its page would extend the old page's text with the new page's
  geometry. `change_current_page` is the single funnel, so the cancel lives there.
- *Entering two-page spread mode cancels a live drag* (`set_dual_page`). Refusing
  to *start* a drag in spread mode is not enough: toggling the layout mid-drag
  would otherwise commit a selection the spread cannot paint but Ctrl+C copies. The
  page snap that follows the toggle always calls `change_current_page`, but that
  function cancels only when the page actually changes — and a page that is already
  a spread's left page does not. Found at the plan gate.
- *A live gesture without the capture is stale.* If `SetCapture` did not take, the
  button-up goes elsewhere and no `WM_CAPTURECHANGED` arrives, so the next press
  cancels such a gesture instead of refusing — otherwise every later press would be
  refused for the rest of the session. Found at the plan gate. The next
  `WM_MOUSEMOVE` cancels it too, or a selection would keep extending under a
  pointer whose button is up (#77).
- *Copy during a held drag copies the live selection* — what is painted, not the
  press-time word or line a double or triple click committed (#69).
- *A gesture that captures no text commits nothing* — it clears, rather than
  leaving an empty selection that would enable Copy for nothing.
- *`fz_snap_selection` never writes the far end when it lies past the page's last
  character.* Its end branch runs only for a character at `idx >= end`; when `end`
  is the index after the last character there is none, and the caller's raw
  **second** point stays in place. On a backward word or line drag that point is
  the *earlier* one, and the selection collapses to part of a word. Reachable by
  double-clicking the blank space below the last line and dragging up.
  `TextPage::snap` detects that either raw point resolves to the end of the text
  and restores the far end to the trailing edge from §3.4.

### 4.3 Click count to mode

`CS_DBLCLKS` must be added to the canvas window class — it is currently
`CS_HREDRAW | CS_VREDRAW` (`PdfCanvas.cpp:464`), so `WM_LBUTTONDBLCLK` is never
delivered today.

- single click → `SelectMode::Chars`
- `WM_LBUTTONDBLCLK` → `SelectMode::Words`
- triple click → `SelectMode::Lines`. Win32 has no triple-click message, so the
  canvas tracks the previous click's time and position against
  `GetDoubleClickTime()` and `SM_CXDOUBLECLK` / `SM_CYDOUBLECLK`.

### 4.4 Rendering

The selection is drawn in the existing overlay block, **after** the search hits so
it reads as the active thing, with a translucent blue fill — the Windows
selection convention, and a hue far enough from the existing yellow
`rgba(1,1,0,0.40)` and orange `rgba(1,0.647,0,0.50)` (`PdfCanvas.cpp:763-772`)
that the two channels never merge:

```cpp
impl_->rt->CreateSolidColorBrush(
    D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.35f), &impl_->brush_selection_fill);
```

The brush is device-bound: created in `create_render_target` and released in
`discard_render_target` alongside the hit brushes, with the same null-check
before use. Polarity does not change under Invert Colors, matching the hit
brushes. It paints only when the selection's page is the current page and the
canvas is in single-page mode (§1).

**Bitmap-identity guard.** The overlay block currently keys only on
`view->current_page()`, not on which bitmap is on screen, so during the frame
after a tab switch — where the canvas still holds the outgoing document's bitmap
(`PdfCanvas.cpp:178-183`, `:254-261`) — the incoming view's quads can be painted
over the outgoing page. The guard already exists (`bitmap_epoch` / `bitmap_page`,
used by `scroll_into_view` and the wheel path) and is simply not applied in
`on_paint`. This design applies it to the whole overlay block, which fixes the
latent search-hit case as a by-product rather than adding a second consumer of the
flaw. Called out here because it is a behaviour fix outside #52's literal scope.

### 4.5 Cursor

The class cursor is hardcoded `IDC_ARROW` (`PdfCanvas.cpp:467`) and the canvas has
no `WM_SETCURSOR` handler (`Splitter.cpp:61` has one, but that is a different
window). One is added:

- a selection drag in progress → `IDC_IBEAM`
- panning, or space held (§5): `IDC_SIZEALL` when the content can pan, `IDC_ARROW`
  when it cannot — *refined at plan time for #58*, see §5
- over page content, single-page mode → `IDC_IBEAM`
- otherwise → `IDC_ARROW`

The handler must `SetCursor` and return `TRUE`, or `DefWindowProc` resets it.

### 4.6 Clipboard

`fz_copy_selection(..., crlf = 1)` already yields CRLF-terminated UTF-8, which is
what `CF_UNICODETEXT` wants after widening. Conversion uses
`MultiByteToWideChar(CP_UTF8, ...)` with an explicit byte length, matching
`OutlinePane.cpp:82` and `ResultsPanel.cpp:149` (so `wlen` excludes the
terminator and the allocation is `(wlen + 1) * sizeof(wchar_t)`).

There is no clipboard code anywhere in `src/` to copy a shape from, so the failure
handling is specified here in full. **Every path past a successful
`OpenClipboard` must reach `CloseClipboard`** — an RAII guard, not a sequence of
early returns. Leaving the clipboard open holds it for the rest of the process's
lifetime and makes copy and paste fail in every other application with
`ERROR_CLIPBOARD_NOT_OPEN`.

```
if (text.empty())                          return;   // never touch the clipboard
h = GlobalAlloc(GMEM_MOVEABLE, bytes);
if (!h)                                    return;
GlobalLock(h) -> copy -> GlobalUnlock(h)
if (!OpenClipboard(hwnd))                { GlobalFree(h); return; }   // held elsewhere
// --- from here on, CloseClipboard is guaranteed by the guard ---
if (!EmptyClipboard())                   { GlobalFree(h); return; }
if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);              // we still own it
```

Two ownership rules: after a **successful** `SetClipboardData` the system owns the
handle and it must not be `GlobalFree`d; on every failure path it must be. And an
empty selection performs no clipboard operation at all — opening and emptying the
clipboard to write nothing would destroy whatever the user had.

`OpenClipboard` failing because another process holds it transiently (Explorer,
Office) is a silent no-op: no retry, no message box. Ctrl+C doing nothing once is
better than a modal interrupting a copy.

### 4.7 Ctrl+C and Ctrl+A dispatch

Both are registered as accelerators and dispatched by focus:

```cpp
{ FCONTROL | FVIRTKEY, 'C', IDM_EDIT_COPY       },
{ FCONTROL | FVIRTKEY, 'A', IDM_EDIT_SELECT_ALL },
```

A bare accelerator is consumed by `TranslateAcceleratorW` (`MainWindow.cpp:2277`)
before any child window sees the key — the `IDM_FIND_CLOSE` handler's comment
records exactly this (`:1626-1630`). So the `WM_COMMAND` arm must hand the key
back when an edit control owns the keyboard, keyed on the **window class** rather
than on an enumeration of known controls, so that a future edit control needs no
change here:

```cpp
case IDM_EDIT_COPY: {
    HWND f = GetFocus();
    wchar_t cls[16] = {};                 // zero-init matters: GetFocus() may be NULL,
    GetClassNameW(f, cls, ARRAYSIZE(cls)); // in which case this writes nothing
    if (_wcsicmp(cls, L"Edit") == 0) { SendMessageW(f, WM_COPY, 0, 0); return 0; }
    copy_selection_to_clipboard();
    return 0;
}
```

`IDM_EDIT_SELECT_ALL` mirrors it with `EM_SETSEL, 0, -1`. A standard `EDIT`
returns exactly `L"Edit"` (4 wchars), so the buffer is ample; `_wcsicmp` makes the
`L"EDIT"` creation spelling irrelevant; all three edit controls in the app
(`FindBar.cpp:778`, `StatusBar.cpp:540`, `ResultsPanel.cpp:694`) are plain
`L"EDIT"`, and the status bar's page box is *subclassed*, not superclassed, so its
class name is unchanged. `PasswordDialog`'s edit is inside a modal
`DialogBoxIndirect` loop that never runs `TranslateAcceleratorW`.

An earlier draft handled Ctrl+C in `PdfCanvas`'s `WM_KEYDOWN` instead, on the
grounds that edit controls handle Ctrl+C natively and no dispatch code would be
needed. That is true for the edit controls, but it leaves Ctrl+C **dead** wherever
focus is on the outline TreeView, the thumbnail ListView or the results ListView —
and nothing hands focus back to the canvas after a click in any of them:
`MainWindow::navigate_click` (`:346-368`) does not `SetFocus`, and
`OutlinePane.cpp` / `ThumbnailPane.cpp` contain none. The reachable sequence is:
drag-select, click an outline entry, Ctrl+C, nothing happens while the selection is
still highlighted. (The tab strip is exempt — `TCS_FOCUSNEVER`, `TabManager.cpp:286`.)

### 4.8 Edit menu, and the positional-`GetSubMenu` fix

A new popup is inserted into `resources/litepdf.rc.in` (a CMake-configured
template, not a plain `.rc`) so the bar reads **File | Edit | View | Help**:

```
POPUP "&Edit"
    MENUITEM "&Copy\tCtrl+C",        IDM_EDIT_COPY
    MENUITEM "Select &All\tCtrl+A",  IDM_EDIT_SELECT_ALL
```

New IDs `IDM_EDIT_COPY = 40071`, `IDM_EDIT_SELECT_ALL = 40072`, starting a fresh
block rather than consuming 40064-40070, which `MainMenu.rc.h:69` reserves.

**This insertion breaks `WM_INITMENUPOPUP` unless it is fixed in the same change.**
That handler identifies popups by position — and those are the only two
`GetSubMenu` calls in all of `src/`:

```cpp
// src/ui/MainWindow.cpp:1375, :1388
            if (popup == GetSubMenu(main, 1)) {          // View: Invert / Two-Page checkmarks
            ...
            if (popup != GetSubMenu(main, 0)) return 0;  // File: MRU rebuild, Print graying
```

With Edit at index 1, the View arm would run against a popup that owns neither
`IDM_VIEW_INVERT` nor `IDM_VIEW_DUAL_PAGE`, `CheckMenuItem` would no-op, and the
View menu's checkmarks would silently stop reflecting state. Nothing else in that
handler keys off position — its other work is `EnableMenuItem` and MRU
`DeleteMenu` / `InsertMenuW`, all `MF_BYCOMMAND`.

Both tests become ownership tests, and the handler grows a third arm for Edit.
The arms must be ordered so that the File arm's `return 0` does not swallow it:

```cpp
static bool popup_owns(HMENU popup, UINT id) {
    return GetMenuState(popup, id, MF_BYCOMMAND) != static_cast<UINT>(-1);
}
...
if (popup_owns(popup, IDM_VIEW_INVERT))  { /* View arm  */ return 0; }
if (popup_owns(popup, IDM_EDIT_COPY))    { /* Edit arm  */ return 0; }
if (popup_owns(popup, IDM_FILE_OPEN))    { /* File arm  */ return 0; }
return 0;
```

`GetMenuState` returns `0xFFFFFFFF` for a non-member ID, and a `MF_SEPARATOR` in
the popup does not perturb it. One caveat: `MF_BYCOMMAND` **does** recurse into
nested submenus, so `popup_owns` would false-positive if a popup ever gained one.
The current menu has none; if that changes, the probe IDs must be chosen from the
popup's own top level.

Placing Edit after Help to dodge the bug was rejected: it would leave a
non-conventional menu order standing in for a fix.

**The Edit arm's enable state must follow the focus — corrected at plan time.** An
earlier version grayed both items when no document is open and Copy whenever there
is no selection. But `TranslateAcceleratorW` sends `WM_INITMENUPOPUP` before acting
on an accelerator, and an accelerator whose item is grayed is disabled: the
keystroke is consumed and no `WM_COMMAND` follows. Live-verified on Windows 11
build 26200 with a scratch window (grayed item: `WM_INITMENUPOPUP`, `TranslateAcceleratorW`
returns 1, no `WM_COMMAND`; the same item re-enabled inside that handler: the
`WM_COMMAND` arrives). Graying as first written would have killed Ctrl+C and Ctrl+A
inside the find box whenever the page had no selection. So:

- Copy is enabled when an edit control holds the focus, or the active view has a
  selection, or the canvas has a live drag selection (which Copy copies, #69).
- Select All is enabled when an edit control holds the focus, or a document is
  open **and** the view is in single-page mode (in spread mode it can do nothing,
  §1) **and** no gesture holds the capture. Both this and the dispatch ask
  `PdfCanvas::can_select_all()`. `TranslateAcceleratorW` sends no
  `WM_INITMENUPOPUP` while a capture is held, so any gesture live when this runs
  is stale; `select_all` ends a stale gesture rather than refusing it, or the
  item would gray for nothing (#68).

"An edit control holds the focus" is the same class-name test §4.7's `WM_COMMAND`
arms dispatch on, so the enable state and the dispatch cannot disagree.

---

## 5. Hand-tool panning (PR-2, #58)

The panning model is already complete and simply has no mouse input:
`pan_by(dx, dy)` moves both axes and clamps against `content_extent`
(`PdfCanvas.cpp:878`), the arrow keys drive it (`:1190-1193`), and `place_bitmap`
clamps whichever axis overflows.

**Gestures.** Left-drag belongs to selection, so panning gets:

- **Middle-drag.** `WM_MBUTTONDOWN` captures and records the origin,
  `WM_MOUSEMOVE` feeds the delta to `pan_by`, `WM_MBUTTONUP` releases — with the
  same commit-before-release ordering as §4.2. Deltas are client pixels and must
  be converted to DIPs before reaching `pan_by`. It refuses to start while
  `Gesture != None` (§4.2), which is the mirror of §4.2's rule that a middle
  press aborts a live selection drag: exactly one gesture owns the capture.
- **Space + left-drag.** Holding space switches left-drag from selecting to
  panning. The space state is **read with `GetKeyState(VK_SPACE)` at
  `WM_LBUTTONDOWN` and in `WM_SETCURSOR`, not latched** across `WM_KEYDOWN` /
  `WM_KEYUP`. A latch would stick: hold space over the canvas, Alt+Tab away,
  release space elsewhere, and the canvas never sees the `WM_KEYUP` — it has no
  `WM_KILLFOCUS`, `WM_SETFOCUS` or `WM_ACTIVATE` handling to reset it. (`MainWindow`
  does handle `WM_SETFOCUS`, but only to hand the focus to the canvas — it resets no
  key state; corrected at the #58 plan gate.) Querying on demand makes the whole class unreachable and
  needs no new key arms at all. (`on_key_down` at `:1135-1197` handles no
  `VK_SPACE`, so nothing conflicts either way.)

Direction is grab-and-drag: moving the mouse right moves the content right.

**Cursor.** `IDC_SIZEALL` while panning or with space held, but only when
`content_extent` actually overflows the viewport — `clamp_pan` already makes
panning a no-op when the content fits (`ViewportMath.hpp:35-42`), and the cursor
must not promise otherwise. (`IDC_HAND` is the hyperlink pointer, not a grab hand;
a real grab cursor would need a custom resource and is not worth it here.)

**Capture must be released on every exit path**, `WM_CAPTURECHANGED` included —
the same discipline §4.2 applies to the selection drag, which is why both share
one capture-lifecycle implementation.

**Plan-time refinements (2026-09-21, #58).** Writing the PR-2 plan
(`docs/superpowers/plans/2026-09-21-hand-tool-panning-58.md`) re-checked this
section against `main` @ `b4bb162`. Everything above holds; eleven things it did not
say are settled here (P10 and P11 found at the plan gate):

- **P1** `WM_MBUTTONDBLCLK` must start a pan too. #52 added `CS_DBLCLKS`, which
  applies to every button, so the second of two quick middle presses arrives as
  `WM_MBUTTONDBLCLK`. The right-button arm takes `WM_RBUTTONDBLCLK` likewise.
- **P2** The pan cursor is set when the pan starts, and re-evaluated at release:
  Windows sends no `WM_SETCURSOR` to a window that holds the capture. (Whether it
  synthesises a mouse move after `ReleaseCapture` is not relied on either way.)
- **P3** The space branch of the left press runs *before* the spread-mode, bitmap
  and text-handle refusals, which guard selection; a pan needs none of them.
- **P4** Pan steps are incremental — the pointer motion since the previous move,
  applied to the pan as it is now — so a re-clamp mid-drag never makes the content
  jump, and integer steps sum to the whole drag exactly.
- **P5** A pan's button-up feeds its own coordinates through the move path first,
  as §4.2's does: injected input can deliver a press and a release with no move.
- **P6** The move cursor means "something will pan" while panning as well as on
  hover. With space held and nothing to pan the cursor is the arrow, not the
  I-beam, because a space press would pan, not select. A live selection keeps the
  I-beam even if space goes down mid-drag.
- **P7** `WM_KEYDOWN` / `WM_KEYUP` for `VK_SPACE` re-run the cursor logic (only
  when the pointer is over the canvas). Nothing is latched: the key is still read
  with `GetKeyState` wherever it matters, so the sticking-latch argument above
  stands.
- **P8** A middle press does not take the keyboard focus; a pan changes only the
  view.
- **P9** A pan whose capture is gone is cancelled by the next move, not at the next
  press: its button is up, and panning would drag the page under a passing pointer.
  No GUI check can make this fire; it is covered by review.
- **P10** A space press is not a click. Every press reaches `ClickCounter` first, and
  Windows pairs presses into `WM_LBUTTONDBLCLK` on its own, so a space click followed
  by a quick plain click would select a word, and click / space-press / click would
  select a line. The space branch calls `ClickCounter::forget()`: the next press
  starts a new sequence even when it arrives as a double click. A left press refused
  because a middle-button pan is live calls it too.
- **P11** A selection drag sets its cursor at the press, for P2's reason: started in
  the margin, where the hover cursor is the arrow, it otherwise kept the arrow for its
  whole length. `on_left_button_down` calls `update_cursor()` after `SetCapture`.

---

## 6. Testing, risks, limitations

### 6.1 Tests

Build **Release**, never Debug (MuPDF is `MT_StaticRelease`; Debug gives LNK2038).
Verify with `ctest --test-dir build -C Release`, not by running the exe — and
keep every `TEST_CASE` name ASCII and subsystem-prefixed, because
`catch_discover_tests` mangles non-ASCII names on Windows and `-R` matches the
name, not the tag.

**Every new test file must be added to `tests/CMakeLists.txt`'s `target_sources`.**
A test file that is never compiled is a check that can only pass.

| File | Covers |
|---|---|
| `test_escrow_context.cpp` (new, #61 PR) | the lock table outliving the `Document` while an `EscrowContext` holds it, counted |
| `test_viewport_math.cpp` (extend) | `dip_to_pdf_point` round-trip against `pdf_point_to_dip`; zero / negative / NaN zoom |
| `test_selection_drag.cpp` (new) | drag state transitions including the `WM_CAPTURECHANGED` abort, the commit-before-release ordering, and **`Gesture` exclusivity under interleaved buttons**; click-count → `SelectMode`; **a stationary double click commits a word**; page clamping |
| `test_document_selection.cpp` (new) | against fixtures: `search.pdf`, `simple.pdf`, `cjk-zh-hant.pdf` (multi-byte round-trip), `encrypted.pdf` (a handle only after `authenticate` — its page has no text, so no query runs on decrypted content), `sample.epub`, and a **new `selection.pdf`** whose pages each pin one behaviour below |

The regressions that matter more than the rest, because each is invisible in the
obvious manual test:

1. **Backward drag keeps its anchor** (§2). Snap a pair whose extent precedes its
   anchor and assert the result matches the forward drag. A forward drag never shows
   it.
2. **A stationary double click selects a word** (§4.2). Nothing about a
   click-and-drag exercises it.
3. **Page coordinates stay origin-free on an offset page box** (§4.1). Every
   pre-existing fixture has a zero MediaBox origin, so no current test would notice
   MuPDF ceasing to translate it. `selection.pdf` carries a page with
   `CropBox [36 36 576 756]` and asserts a word's quad lands 72 pt from the crop
   box's corner, not 108.
4. **Select All on a multi-column page and on a page with wide intra-line gaps**
   (§3.4). Assert the copied text equals `page_text()`'s with whitespace removed,
   that it ends with the page's final character, and that a line containing two runs
   separated by more than **0.8 em** yields more than one quad. (0.8 em is
   `SPACE_MAX_DIST` in `stext-device.c`; between 0.15 and 0.8 em MuPDF inserts a
   synthetic space whose quad bridges the gap, so an earlier "0.5 em" fixture could
   not have split.) The first catches the corner-point failure, the second the
   origins failure, the third the one-quad-per-line failure. A single-column fixture
   passes all three while broken.
5. **A word drag from below the text keeps its far end** (§4.2 refinements). Nothing
   about a drag that starts on text exercises it.

The earlier item 5 — `snap` round-trip idempotence in `Chars` mode — is moot:
`snap` passes `Chars` points through unchanged, as MuPDF's own viewer does
(`platform/gl/gl-main.c`), so nothing is snapped and nothing can drift.

Empty-page behaviour is asserted directly — both points resolve to index 0
(`stext-search.c:242`), `fz_enumerate_selection` returns early on `start == end`
(`:267-268`), and `fz_copy_selection` still terminates and extracts a buffer
(`:485-494`), so the result is `""`, not null and not a crash. `sample.png`
wrapped as a page, or a blank page in the new fixture, provides the text-free page.

`large.pdf` must not be used to judge rendering: page 0 overlaps its own text by
design.

### 6.2 Known limitations, recorded

| | |
|---|---|
| R1 | No selection in two-page spread mode — neither painted nor startable (§1), matching the search overlay's R17 deferral. |
| R2 | No cross-page selection; revisit with continuous scroll (#55). |
| R3 | Rotated text highlights as an axis-aligned box, as search hits already do. |
| R4 | Acquiring a `TextPage` (mouse-down, Select All) contends with `page_hits` on `doc_mutex` during an active search scan. Bounded: once per gesture, the query path is lock-free, and `page_hits` honours an abort flag. |
| R5 | Marquee selection deferred to its own issue. `fz_copy_rectangle` is no longer needed by this design at all (§3.4). |
| R6 | `popup_owns` would false-positive if any popup gained a nested submenu (§4.8). |
| R7 | The **existing** per-render clone-escrow did not keep the MuPDF lock table alive — the same latent use-after-free this design avoids for `TextPage` (§3.3). Pre-existing; tracked as [#61](https://github.com/jeffchen1981-fu/litepdf/issues/61) and fixed in its own PR, before #52, by `EscrowContext`. |
| R8 | Right-to-left text: `full_range` uses the left/right quad edges MuPDF uses for left-to-right characters. Untested — no RTL fixture exists. |
| R9 | A double click in the blank space below the last line selects the page's last word: MuPDF resolves the point to the end of the text and word snapping extends back to the word's start. Chrome selects nothing there. |
| R10 | The I-beam cursor shows over the whole page box, including images and margins. |
| R11 | Between a zoom change and the arrival of its render the canvas still shows the old-scale bitmap of the same page, so highlights and a press's pointer mapping are briefly misaligned. Pre-existing for search hits; the window is one render. |
| R12 | A selection needing more than 65,536 separate highlight quads on one page paints only the first 65,536 (a memory bound); the copied text is complete. |
| R14 | A word or line snap that resolves to a line's **trailing** boundary selects the next line's first word (Words) or that whole line (Lines) — MuPDF gives that boundary the same index as the next line's first character (`find_closest_in_line` returns `idx + line_length`, and `idx` carries across lines), so the snap walks into it. The trigger is wider than the margin: anywhere right of the **midpoint of the line's last glyph**, and anywhere outside the line's `+/-size/2` band, which at 12 pt leaves the bottom ~2.2 pt of the line's own glyph boxes resolving to the next line. The mirror case — the top ~2.2 pt — resolves to the leading boundary and selects the line's first word. Measured on MuPDF 1.27.2, `selection.pdf` page 3: Words at (168.5, 67.35) copies `delta` where (167.5, 67.35) copies `gamma`; (100, 75.0) copies `delta`; (150, 60.0) copies `alpha`; Lines there copies `alpha beta gamma`. The last line is unaffected (R9 covers it). Chars mode is unaffected. |
| R13 | Entering two-page spread mode leaves a selection committed in single-page mode in place: unpainted and not extendable (R1/§1), but still copyable with Ctrl+C. D2's clearers are the next left click in `Chars` mode, a new drag, and `clear_selection()` — a layout toggle is none of them, and clearing there would silently discard user state. The same consequence §2 accepts for page changes. |
| R15 | A page change or layout toggle during a pan (PgDn, the wheel at an edge, Ctrl+Shift+D) ends the pan; the button must be pressed again. The `cancel_gesture` calls in `change_current_page` and `set_dual_page` are gesture-agnostic, and a pan commits nothing. |
| R16 | Holding space while the find box has the focus types spaces into it until the left press moves focus to the canvas; the space cursor refresh runs only when the canvas has the focus. |
| R17 | A right-button press during a pan is ignored and the pan continues; §4.2's second-button abort protects a selection drag, and a pan has nothing to protect. |
| R18 | A *cancelled* pan (capture lost, tab switched, page changed) does not re-evaluate the cursor; the move shape can stay until the next `WM_SETCURSOR`, at the latest the next pointer move. Only a normal release refreshes it explicitly. |
| R19 | The cursor is re-evaluated on `WM_SETCURSOR`, when a gesture starts or ends normally, and when space goes down or up — not when a keyboard command changes the layout or zoom under a still pointer. The old shape stays until the pointer moves; pre-existing for the I-beam since #52. |

### 6.3 Review record

Sections 1-3 were reviewed at slot-1 by Fable (`claude-fable-5-1`) before sections
4-6 existed: eight findings, none discarded, six anchors re-verified by the author.
The full spec then ran a Full-tier gate — Opus (Bash-capable, mechanical) and
Sonnet (read-only, consistency and ambiguity) in parallel — producing twelve more
findings across two Criticals, none discarded, with both lenses independently
finding the double-click defect. Codex `gpt-5.6-terra` at `high` then reviewed the
post-fix version and returned four more, including the lock-table flaw that
invalidated the first attempt at the escrow fix.

**Lens 3 is complete, but `gpt-5.6-luna` needed three attempts and ran with a
narrowed scope. Read its result with that in mind.**

- *Attempt 1* consumed 560k tokens and hit a short-window usage limit without
  emitting a report — void. The weekly band was 41%, so a rate window, not an
  exhausted pool.
- *Attempt 2* ran 59 minutes against the same 22-file brief: 241 tool calls, two
  context compactions, the spec re-read at least five times, and no report
  begun. Terminated deliberately — void.
- *Attempt 3* succeeded in 19.5 minutes with 19 tool calls and no compaction, on a
  **narrowed brief**: the spec plus a pre-extracted 653-line evidence bundle, a cap
  of three additional full files, and scope restricted to the `TextPage` lifetime
  (§3.1-3.3) and the drag state machine (§4.2). It therefore did **not** cover
  §1, §2, §4.1, §4.3-4.8, §5 or §6 — those rest on the Claude lenses and terra.
  It returned two findings and two questions, both questions settled into
  corrections, none discarded.

Attempt 3 ran **concurrently with another project's Codex session**, which the
review protocol normally forbids because parallel sessions cross-attribute spend.
That was a deliberate choice over waiting without bound behind a process this
review did not control. The findings are unaffected; the per-run credit figures
for this window are not a clean measurement and should not be used as one.

Totals across all five lenses: twenty-six findings, none discarded.

The findings that changed this design, in the order they appear above: the in-out
snap destroying the anchor (§2); a page-box-origin offset in the coordinate maps and
search's scroll anchoring (§4.1 — later found not to exist, see below); Select All
by corner points, then by per-line union, before landing on first/last character
positions (§3.4); the absent whole-page quad API (§3.4); the handle outliving its
`Document` on tab close, and then the cloned context still outliving the lock table
(§3.3); the unspecified release order inside the handle (§3.3); the unlocked
`fz_warn` on the drop path (§3.2); `copy()`'s stale, unimplementable claim to take
`doc_mutex` (§3.2); `ReleaseCapture` delivering `WM_CAPTURECHANGED` synchronously
(§4.2); the click-clears rule destroying double-click selections (§4.2); two
gestures owning one capture (§4.2, §5); capture loss during a *pan* leaving the
canvas permanently refusing gestures (§4.2); dual-page mode being ungated at
mouse-down (§1, §4.2); the clipboard's missing failure paths (§4.6); the sticking
space latch (§5); the dead Ctrl+C focus states (§4.7); and the positional
`GetSubMenu` breakage (§4.8).

#### Plan-time corrections (2026-09-16)

Writing the implementation plan
(`docs/superpowers/plans/2026-09-16-text-selection-copy-52.md`) re-verified every
claim above against the vendored MuPDF source and `main` @ `88513f2`, and found five
things the five review lenses had not. Each is patched in place above and marked
"corrected at plan time":

1. **Select All by character origins drops the page's final character** (§3.4) —
   now leading and trailing edges.
2. **The page-box-origin offset does not exist** (§4.1). MuPDF translates the
   CropBox origin to (0,0) itself, and every other supported format has a zero
   origin. The translation in `TextPage` and the `page_hits` normalisation are
   dropped; the offset fixture page stays, as a pin.
3. **`fz_snap_selection` leaves the far end unwritten past the last character**
   (§4.2), collapsing a backward word or line drag. `snap` restores it.
4. **Graying Edit > Copy with no selection would kill Ctrl+C in the find box**
   (§4.8), because a grayed item disables its accelerator. Live-verified; the enable
   state now follows the focus.
5. **A 0.5 em gap cannot split a line's highlight** (§6.1) — the threshold is
   0.8 em.

Items 1, 3 and 5 are behaviour of MuPDF code that the spec *quoted* but whose
boundary arithmetic no lens executed. Item 2 reversed a finding the review had
accepted: a claim about what MuPDF does to coordinates had been checked against the
render path and the stext path, but never against the page-transform code that feeds
both.

The same pass moved the lock-table fix into its own class and PR (#61, §3.3) and
made `Chars`-mode `snap` a pass-through, retiring the former §6.1 item 5.
