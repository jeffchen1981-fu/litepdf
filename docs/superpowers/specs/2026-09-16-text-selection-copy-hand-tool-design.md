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
    // points are the first and last character origins on the page (§3.4).
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
`TextPage` API in §3.1 — is **page-box-relative**: the origin is the page box's
top-left corner, matching the rendered bitmap's pixel (0,0). This is *not*
absolute PDF user space when a page's box origin is non-zero. §3.1 does the
translation; §4.1 explains why it must exist at all.

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
    // The handle owns THREE things, and all three are required for its
    // lifetime to be independent of the Document that produced it (§3.3):
    // a ref on the fz_stext_page, its own fz_clone_context escrow, and a
    // shared_ptr to the MuPDF lock table. The third is the one that is easy
    // to miss — see the warning in §3.3.
    //
    // All coordinates crossing this API are PAGE-BOX-RELATIVE (§2 "Units").
    // The implementation adds/subtracts the page box origin when talking to
    // MuPDF, so no caller ever sees absolute PDF user space.
    class TextPage {
    public:
        TextPage() = default;               // empty
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
        struct Snapped { SelPoint a, b; };
        [[nodiscard]] Snapped snap(SelPoint a, SelPoint b, SelectMode m) const;

        // Merged per-line highlight quads. Lock-free.
        [[nodiscard]] std::vector<Quad> highlight(SelPoint a, SelPoint b) const;

        // The origins of the page's first and last characters in reading
        // order. Feeding these to highlight() / copy() selects the whole page
        // through the ordinary path — see §3.4 for why Select All is expressed
        // this way rather than as the page's bounding corners.
        // Returns false for a page with no text.
        [[nodiscard]] bool full_range(SelPoint& first, SelPoint& last) const;

        // UTF-8, CRLF line endings. Lock-free as well, despite allocating —
        // see §3.2: it runs on the handle's own escrow context.
        [[nodiscard]] std::string copy(SelPoint a, SelPoint b) const;

    private:
        struct Impl;
        // fz_stext_page* + escrow fz_context* + shared_ptr<MuPDFLocks>
        std::unique_ptr<Impl> impl_;
    };

    // Empty handle if the document is not open, `page` is out of range, or the
    // context clone fails (OOM). Callers MUST tolerate an empty handle — a drag
    // that cannot acquire one simply does not start.
    [[nodiscard]] TextPage text_page(std::size_t page) const;
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
`TextPage::Impl` writes the first two steps in an **explicit destructor body**, and
declares its members in the order `locks`, `escrow`, `page`, so that C++'s
reverse-declaration member destruction releases `locks` after the body has run.
Both halves are required and both get a comment saying why: an explicit body alone
is fragile to a later reordering of the members, and member order alone is
invisible to a reader.

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
  `Document` dies), and fixing it means threading the `shared_ptr` through
  `post_render_done`. That is outside #52 and gets its own issue; what #52 must
  not do is add a *second* instance of the defect.
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

**`full_range()` returns the origin of the page's first character and the origin
of its last, in reading order**, found by a trivial walk of
`page->first_block` → lines → chars with no merging and no heuristic. Feeding
those two points to the ordinary `highlight()` and `copy()` resolves `start` to
index 0 and `end` to the last index, and `fz_enumerate_selection` then walks every
character in index order.

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

### 4.1 Coordinate mapping, both directions — including the page box origin

The existing forward map is pinned to `zoom_pct`, not `render_scale`
(`ViewportMath.hpp:78`, `PdfCanvas.cpp:1351-1390`) and is otherwise correct, but
it **omits the page box origin**, which is latent-and-cosmetic for a search
highlight and functional for a selection (it copies the wrong text).

The rendered bitmap's pixel (0,0) corresponds to the page's *bounds* origin, not
to page-space (0,0): the pixmap bbox is derived from the transformed page bounds —

```cpp
// src/core/RenderEngine.cpp:200-202
                    fz_irect bbox = fz_round_rect(
                        fz_transform_rect(fz_bound_display_list(ctx, dlist), m));
                    pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox,
```

— while `fz_stext_page` char quads are in absolute page space. For any PDF whose
CropBox/MediaBox origin is non-zero (cropped scans, imposed documents) the two
differ by `bounds.x0/y0`.

**This design contains the problem inside `TextPage`** (§3.1 "Units"): the handle
adds `bounds.x0/y0` to points going into MuPDF and subtracts it from quads coming
out, so every coordinate outside the engine layer is page-box-relative and the
canvas's maps stay origin-free:

```cpp
// Render-target DIPs -> page-box-relative points. Inverse of pdf_point_to_dip.
inline float dip_to_pdf_point(float dip, float zoom_pct) noexcept {
    if (!(zoom_pct > 0.0f)) return 0.0f;   // also rejects NaN
    return dip / zoom_pct;
}
```

A mouse position becomes a point by subtracting the page origin that
`place_bitmap` produced for this paint, then dividing by `zoom_pct`. The result is
clamped to `[0, width_pt] × [0, height_pt]` — well-defined precisely because the
coordinates are page-box-relative.

`WM_MOUSEMOVE` coordinates are client **pixels**; the canvas works in DIPs.
Conversion uses the same `px * 96 / dpi` factor as `bitmap_px_to_dip`.

**The search feature has the same origin omission in two places, not one**, and
fixing only the overlay would be worse than fixing neither. Besides the paint
block, the *scroll anchoring* consumes raw quad coordinates:

```cpp
// src/ui/PdfCanvas.cpp:1019-1022 (pan_y_for_hit); :436-439 is the same shape
    const float q_min_y_pt = std::min({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_max_y_pt = std::max({ h.geom.ul_y, h.geom.ur_y,
                                        h.geom.ll_y, h.geom.lr_y });
    const float q_center = pdf_point_to_dip((q_min_y_pt + q_max_y_pt) * 0.5f, pct);
```

Correcting the overlay alone would leave F3 and cross-tab navigation centring the
viewport `bounds.y0` away from the hit it is drawing a box around — overlay and
navigation disagreeing about the same quad, which is exactly the defect class
issue #50 is already open on. So the normalisation is applied where the hit
geometry is *produced* (`Document::page_hits`), not at each consumer, and all
three sites — overlay, `pan_y_for_hit`, and the `scroll_into_view` visibility test
— then read the same page-box-relative frame. Called out in the PR description as
a search-path fix carried by a selection PR.

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
| `WM_LBUTTONUP` | **First**: decide and commit. If `mode == Chars` and `!moved_past_threshold`, this was a click — clear the selection (1b's "next click clears"). Otherwise materialise `quads` + `text_utf8`. `gesture = None`. **Then**: `if (GetCapture() == hwnd_) ReleaseCapture();` and release the handle. |
| `WM_CAPTURECHANGED` | **Whatever `gesture` is, set it to `None`**, releasing the handle without committing if it was `Selecting`. This must cover `Panning` as well as `Selecting`: capture taken by another window mid-pan would otherwise leave `gesture == Panning` forever, and since every button-down refuses to start while `gesture != None`, the canvas would accept no mouse gesture again for the rest of the session. (After a normal button-up this arm finds `gesture` already `None` and does nothing.) |
| `WM_MBUTTONDOWN` / `WM_RBUTTONDOWN` while `gesture == Selecting` | Abort the selection drag: `gesture = None`, release the handle, keep whatever was already committed, and do **not** start panning from this press. |
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

- over page content, single-page mode, not panning → `IDC_IBEAM`
- panning, or space held with pannable content (§5) → `IDC_SIZEALL`
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

The Edit arm grays both items when no document is open, and additionally grays
Copy when there is no selection.

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
  `WM_KILLFOCUS`, `WM_SETFOCUS` or `WM_ACTIVATE` handling to reset it, and neither
  does `MainWindow`. Querying on demand makes the whole class unreachable and
  needs no new key arms at all. (`on_key_down` at `:1135-1197` handles no
  `VK_SPACE`, so nothing conflicts either way.)

Direction is grab-and-drag: moving the mouse right moves the content right.

**Cursor.** `IDC_SIZEALL` while panning, and on hover only when `content_extent`
actually overflows the viewport — `clamp_pan` already makes panning a no-op when
the content fits (`ViewportMath.hpp:35-42`), and the cursor must not promise
otherwise. (`IDC_HAND` is the hyperlink pointer, not a grab hand; a real grab
cursor would need a custom resource and is not worth it here.)

**Capture must be released on every exit path**, `WM_CAPTURECHANGED` included —
the same discipline §4.2 applies to the selection drag, which is why both share
one capture-lifecycle implementation.

---

## 6. Testing, risks, limitations

### 6.1 Tests

Build **Release**, never Debug (MuPDF is `MT_StaticRelease`; Debug gives LNK2038).
Verify with `ctest --test-dir build -C Release`, not by running the exe — and
keep every `TEST_CASE` name ASCII and subsystem-prefixed, because
`catch_discover_tests` mangles non-ASCII names on Windows and `-R` matches the
name, not the tag.

**Both new test files must be added to `tests/CMakeLists.txt`'s `target_sources`.**
A test file that is never compiled is a check that can only pass.

| File | Covers |
|---|---|
| `test_viewport_math.cpp` (extend) | `dip_to_pdf_point` round-trip against `pdf_point_to_dip`; zero / negative / NaN zoom |
| `test_selection_math.cpp` (new) | drag state transitions including the `WM_CAPTURECHANGED` abort, the commit-before-release ordering, and **`Gesture` exclusivity under interleaved buttons**; click-count → `SelectMode`; **a stationary double click commits a word**; page-box clamping |
| `test_document_selection.cpp` (new) | against fixtures: `search.pdf` (known text), `simple.pdf`, `cjk-zh-hant.pdf` (multi-byte round-trip), `encrypted.pdf` (selection after `authenticate`), `sample.epub`, and a **new fixture with a non-zero MediaBox origin** |

Four regressions matter more than the rest, because each is invisible in the
obvious manual test:

1. **Backward drag keeps its anchor** (§2). Snap a pair whose extent precedes its
   anchor and assert the stored anchor is unchanged. A forward drag never shows it.
2. **A stationary double click selects a word** (§4.2). Nothing about a
   click-and-drag exercises it.
3. **Non-zero page box origin** (§4.1). Every existing fixture has MediaBox origin
   `0 0` — verified across all ten — so no current test can catch an offset. The
   new fixture should use something like `MediaBox [36 36 648 828]`, which makes
   the failure manifest as a 36 pt displacement.
4. **Select All on a multi-column page and on a page with wide intra-line gaps**
   (§3.4). Assert the copied text length equals `page_text()`'s, and that a line
   containing two runs separated by more than `0.5 em` yields more than one quad.
   The first catches the corner-point failure; the second catches the
   one-quad-per-line failure. A single-column fixture passes both while broken.
5. **`snap` round-trip idempotence in `Chars` mode.** MuPDF's own viewer snaps only
   for `WORDS` / `LINES` and passes raw points through in `CHARS`
   (`gl-main.c:1584-1601`), whereas §4.2 snaps on every move. `fz_snap_selection`
   writes `*a = ch->origin` (a baseline origin) which `fz_highlight_selection` then
   re-resolves through `find_closest_in_page`; whether that round-trip is
   idempotent or drifts by one character per move was not settled by reading. Test
   it directly: snap the same pair twice and assert the second call is a no-op. If
   it is not, `Chars` mode must pass raw points through as the gl viewer does.

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
| R7 | The **existing** per-render clone-escrow does not keep the MuPDF lock table alive and has the same latent use-after-free this design fixes for `TextPage` (§3.3). Pre-existing, not caused here; tracked as [#61](https://github.com/jeffchen1981-fu/litepdf/issues/61). |

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
snap destroying the anchor (§2); the page box origin missing from the coordinate
maps *and* from search's scroll anchoring (§4.1, §3.1 "Units"); Select All by
corner points, then by per-line union, before landing on first/last character
origins (§3.4); the handle outliving its `Document` on tab close, and then the
cloned context still outliving the lock table (§3.3); the unlocked `fz_warn` on
the drop path (§3.2); `ReleaseCapture` delivering `WM_CAPTURECHANGED`
synchronously (§4.2); the click-clears rule destroying double-click selections
(§4.2); two gestures owning one capture (§4.2, §5); dual-page mode being ungated
at mouse-down (§1, §4.2); the clipboard's missing failure paths (§4.6); the
sticking space latch (§5); the dead Ctrl+C focus states (§4.7); and the positional
`GetSubMenu` breakage (§4.8).

The findings that changed this design, in the order they appear above: the in-out
snap destroying the anchor (§2); the page box origin missing from both coordinate
maps (§4.1, §3.1 "Units"); the absent whole-page quad API (§3.4); the handle
outliving its `Document` on tab close (§3.3); the unlocked `fz_warn` on the drop
path (§3.2); `ReleaseCapture` delivering `WM_CAPTURECHANGED` synchronously (§4.2);
the click-clears rule destroying double-click selections (§4.2); dual-page mode
being ungated at mouse-down (§1, §4.2); the clipboard's missing failure paths
(§4.6); the sticking space latch (§5); the dead Ctrl+C focus states (§4.7); the
positional `GetSubMenu` breakage (§4.8); the unspecified release order inside the
handle (§3.3); capture loss during a *pan* leaving the canvas permanently refusing
gestures (§4.2); and `copy()`'s stale, unimplementable claim to take `doc_mutex`
(§3.2).
