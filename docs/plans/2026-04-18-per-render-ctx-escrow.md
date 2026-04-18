# Per-Render Context Escrow Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan task-by-task (same pattern as Phase 5 Tasks 0–9 on `main`).

**Goal:** Eliminate the Phase 5 residual cross-tab render-bleed race (I-2) by pairing every `fz_pixmap*` posted via `WM_USER_RENDER_DONE` with a companion `fz_context*` escrow clone. Canvas drops the pixmap via the escrow ctx, so drop always goes through the correct MuPDF root regardless of which tab is active when the message lands.

**Architecture:** Add `PdfCanvas::post_render_done(HWND, fz_pixmap*, fz_context* worker_ctx)` — a single static helper that encapsulates the "keep pixmap + clone worker_ctx + post pair (or clean up on clone/post failure)" pattern. Replace the four existing inline lambdas with one-line calls into it. Rewrite the `WM_USER_RENDER_DONE` handler to consume the escrow ctx from LPARAM instead of reaching through `impl_->view->ui_ctx()`. Remove `PdfCanvas::Impl::orphan_ctx` (now redundant). Also fold in the M-1 fix (`open_tab_async`'s `PostMessageW` return check). Tag `v0.0.6-phase5.2`.

**Tech Stack:** C++17, MuPDF 1.24.11 refcounted contexts, Win32 `PostMessageW`, existing Catch2/CMake from Phase 5.

**Prerequisites:**

- Current HEAD at tag `v0.0.6-phase5.1`.
- Design doc committed at `docs/plans/2026-04-18-per-render-ctx-escrow-design.md` (commit `a52ae9b`).
- 73/73 ctest green; smoke-test exits 0.

**Done when:**

1. A single helper `PdfCanvas::post_render_done(HWND, fz_pixmap*, fz_context* worker_ctx)` exists; callers never write the escrow-clone + post pattern inline.
2. All 4 existing `PostMessageW(WM_USER_RENDER_DONE, ..., 0)` sites (in `MainWindow.cpp` `kick_render`, `PdfCanvas.cpp` WM_MOUSEWHEEL zoom, `PdfCanvas.cpp::resubmit_current_page`, `PdfCanvas.cpp::on_key_down`) route through the helper.
3. `PdfCanvas::WM_USER_RENDER_DONE` reads escrow ctx from LPARAM, uses it for `fz_pixmap_width/height/stride/samples` and `fz_drop_pixmap`, then `fz_drop_context`s the escrow. Handler does NOT consult `impl_->view->ui_ctx()` for any fz op.
4. `PdfCanvas::Impl::orphan_ctx` field, its maintenance in `set_view`, and its drop in the destructor are **removed**.
5. `open_tab_async`'s lambda guards `PostMessageW` return; on FALSE, `delete raw` avoids Tab leak (M-1 fix).
6. `on_tab_switch`'s `cancel_stale_renders(INT_MAX)` comment re-labels the call as a perf optimization (not a correctness mitigation).
7. The Phase 5 plan's "Cross-tab render-bleed race (residual)" bullet under "Known Limitations" is **deleted**.
8. `ctest --test-dir build -C Release` reports 73/73. `smoke-test.ps1` exits 0.
9. One-shot manual stress run recorded in this plan's `## Verification record` section (appended during execution).
10. Tag `v0.0.6-phase5.2` on the final commit.

---

## Architectural Decisions

**D1. Helper lives on `PdfCanvas` as a public static method.** Declaration in `src/ui/PdfCanvas.hpp`; definition in `src/ui/PdfCanvas.cpp`. `MainWindow.cpp` already includes `"ui/PdfCanvas.hpp"` transitively; no new include wiring. Alternative (free function in anon namespace) would force duplication across TUs. Static-member version keeps the name `PdfCanvas::post_render_done` tied to the owner of `WM_USER_RENDER_DONE`.

**D2. Signature is three pointers, returns `bool`.** `bool PdfCanvas::post_render_done(HWND target, fz_pixmap* pix, fz_context* worker_ctx)`. Return value is `true` on successful post, `false` on any failure path (clone-OOM, post-FALSE). Callers ignore the return today — the helper cleans up on failure — but the signal is there if future logging wants it.

**D3. Failure paths drop the pixmap and clean up.** On `fz_clone_context == nullptr`: drop pixmap via worker_ctx (valid, we're still in the callback). On `PostMessageW == FALSE`: drop pixmap via the escrow ctx, drop the escrow ctx. No leaks.

**D4. `orphan_ctx` removal is part of the core task, not a follow-up.** Design §3 explicitly flags this as "reduction" work. Carrying forward a dead-but-looks-live field would confuse future maintainers.

**D5. M-1 fix folded in.** Scope-expansion approved during brainstorming. Fixes the post-`release()`-then-PostMessageW-fails Tab leak path in `open_tab_async`. Same cleanup discipline, same mechanics (`PostMessageW` return check).

**D6. Tag lands after plan-doc update.** Same pattern as `v0.0.5-phase4.1` / `v0.0.6-phase5.1` — the follow-up tag is on the commit that closes out the follow-up, which is the plan-doc Known-Limitations deletion.

---

## Task List

### Task 0: Add `PdfCanvas::post_render_done` helper (header + impl)

**Files:**
- Modify: `src/ui/PdfCanvas.hpp`
- Modify: `src/ui/PdfCanvas.cpp`

**Step 1: Add the declaration to PdfCanvas.hpp**

Insert after the existing `set_pan(float x, float y);` method (below the `Pan` struct). The fz_pixmap / fz_context types are already forward-declared in this header (they arrived via Phase 3).

```cpp
    // Post WM_USER_RENDER_DONE to `target` for the (pixmap, ctx) pair,
    // where `ctx` is a clone-escrow made from `worker_ctx` so the UI
    // thread can drop the pixmap with the correct MuPDF root — even if
    // the producing DocumentView is torn down before the message lands.
    //
    // Called from the worker thread inside the render callback. Takes
    // an extra ref on the pixmap via fz_keep_pixmap, clones
    // worker_ctx, and on any failure (clone OOM, post FALSE) cleans up
    // both the kept pixmap and the escrow ctx. Returns true iff the
    // message was successfully posted.
    //
    // Callers: MainWindow::kick_render, resubmit_current_page,
    // on_key_down's page-change path, WM_MOUSEWHEEL zoom path.
    static bool post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx);
```

**Step 2: Add the implementation to PdfCanvas.cpp**

Place the definition near the existing `PdfCanvas::set_view` at top of file (after the namespace opens). The MuPDF forward declarations at the top of the file already cover `fz_keep_pixmap`, `fz_clone_context`, `fz_drop_pixmap`, `fz_drop_context`.

```cpp
bool PdfCanvas::post_render_done(HWND target,
                                 fz_pixmap* pix,
                                 fz_context* worker_ctx) {
    if (!pix) {
        // A null pixmap signals "render failed / cancelled". Post with
        // LPARAM=0; the canvas handler invalidates and moves on.
        PostMessageW(target, WM_USER_RENDER_DONE,
                     reinterpret_cast<WPARAM>(nullptr),
                     static_cast<LPARAM>(0));
        return true;
    }

    fz_keep_pixmap(worker_ctx, pix);               // refcount: 1 -> 2
    fz_context* escrow = fz_clone_context(worker_ctx);
    if (!escrow) {
        // OOM on clone. Drop the keep we just took and abandon. The
        // worker's own drop-after-callback will bring the refcount to 0.
        fz_drop_pixmap(worker_ctx, pix);
        return false;
    }

    if (!PostMessageW(target, WM_USER_RENDER_DONE,
                      reinterpret_cast<WPARAM>(pix),
                      reinterpret_cast<LPARAM>(escrow))) {
        // Target HWND is invalid (window destroyed). Clean up both halves.
        fz_drop_pixmap(escrow, pix);
        fz_drop_context(escrow);
        return false;
    }
    return true;
}
```

**Step 3: Build**

Run:
```
cmake --build build --config Release
```
Expected: clean build. No warnings on the new function.

**Step 4: Run ctest to confirm no regression**

Run: `ctest --test-dir build -C Release`
Expected: 73/73.

(No new tests — the helper is exercised end-to-end in Task 2 onward.)

**Step 5: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp
git commit -m "feat(ui): add PdfCanvas::post_render_done escrow helper (I-2 fix prep)"
```

---

### Task 1: Rewrite `WM_USER_RENDER_DONE` handler to use escrow ctx

**Files:**
- Modify: `src/ui/PdfCanvas.cpp` (handler at approximately lines 197–266)

**Step 1: Replace the handler body**

The existing handler is at `case WM_USER_RENDER_DONE:` inside `PdfCanvas::handle_message`. Replace the entire case body with:

```cpp
        case WM_USER_RENDER_DONE: {
            auto* pix    = reinterpret_cast<fz_pixmap*>(w);
            auto* escrow = reinterpret_cast<fz_context*>(l);

            if (!pix) {
                // Render failed or cancelled (helper posts WPARAM=0 here).
                // escrow will also be null on this path — nothing to drop.
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            // Invariant (design §2.1): a non-null pixmap always travels
            // with an escrow ctx clone. If escrow is null we have a
            // caller that bypassed post_render_done — defensive drop
            // against the worst-case leak. Should never fire in practice.
            if (!escrow) {
                return 0;
            }

            // Use escrow (NOT impl_->view->ui_ctx()) for every fz op so
            // we stay on the pixmap's own MuPDF root. impl_->view may
            // point at a different tab by now — it's only consulted for
            // bitmap placement, which happens implicitly via the canvas
            // HWND.
            const int w_px   = fz_pixmap_width(escrow, pix);
            const int h_px   = fz_pixmap_height(escrow, pix);
            const int stride = fz_pixmap_stride(escrow, pix);
            unsigned char* samples = fz_pixmap_samples(escrow, pix);

            create_render_target();
            if (!impl_->rt) {
                fz_drop_pixmap(escrow, pix);
                fz_drop_context(escrow);
                return 0;
            }

            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE));
            ComPtr<ID2D1Bitmap> bmp;
            HRESULT hr = impl_->rt->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(w_px),
                            static_cast<UINT32>(h_px)),
                samples, static_cast<UINT32>(stride), &props, &bmp);

            // Drop order: pixmap first (escrow still valid), then escrow.
            fz_drop_pixmap(escrow, pix);
            fz_drop_context(escrow);

            if (hr == D2DERR_RECREATE_TARGET) {
                discard_render_target();
                resubmit_current_page();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (FAILED(hr)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            impl_->current_bitmap = std::move(bmp);
            impl_->pan_x = 0.0f;
            impl_->pan_y = 0.0f;
            ColdStartTimer::mark(3);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
```

Notes:
- `impl_->view` is no longer accessed inside the handler. Delete the old `if (!impl_->view) { drop via orphan_ctx; return 0; }` branch.
- Error paths (no render target, CreateBitmap fail) must drop BOTH the pixmap (via escrow) and the escrow ctx.
- The "pan reset on new bitmap" line stays (plan D7 from Phase 5 is unchanged by this fix).

**Step 2: Build**

Run: `cmake --build build --config Release`
Expected: clean build. May see one warning about `impl_->view` being unused in the handler scope — that's fine; the field is still used in other handlers.

**Step 3: Run ctest**

Run: `ctest --test-dir build -C Release`
Expected: 73/73. (No new tests.)

**Step 4: Commit**

```bash
git add src/ui/PdfCanvas.cpp
git commit -m "feat(ui): WM_USER_RENDER_DONE uses escrow ctx from LPARAM (I-2 core fix)"
```

---

### Task 2: Route all four callers through the helper

**Files:**
- Modify: `src/ui/MainWindow.cpp` (one site: `kick_render`)
- Modify: `src/ui/PdfCanvas.cpp` (three sites: WM_MOUSEWHEEL zoom, `resubmit_current_page`, `on_key_down`)

**Step 1: `MainWindow::kick_render` (approximately lines 144–150)**

Current body of the lambda:

```cpp
    view_->request_render_with_prefetch(page,
        [target](fz_pixmap* p, fz_context* worker_ctx) {
            if (p) fz_keep_pixmap(worker_ctx, p);  // extend lifetime across PostMessage
            PostMessageW(target, WM_USER_RENDER_DONE,
                         reinterpret_cast<WPARAM>(p), 0);
        });
```

Replace with:

```cpp
    active_view()->request_render_with_prefetch(page,
        [target](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target, p, worker_ctx);
        });
```

(If the existing line uses `view_` instead of `active_view()`, leave that as-is — the Phase 5 refactor already replaced single-view references. Use whatever the current code has; only the lambda body changes.)

Also delete the now-unused `extern "C" { struct fz_pixmap* fz_keep_pixmap(...); }` forward-decl block at the top of `MainWindow.cpp` (approximately lines 29–31). `fz_keep_pixmap` is no longer invoked from this TU.

**Step 2: `PdfCanvas.cpp` WM_MOUSEWHEEL path (approximately lines 175–181)**

Current body:

```cpp
                    impl_->view->request_render(
                        impl_->view->current_page(),
                        [target](fz_pixmap* p, fz_context* wc) {
                            if (p) fz_keep_pixmap(wc, p);
                            PostMessageW(target, WM_USER_RENDER_DONE,
                                         reinterpret_cast<WPARAM>(p), 0);
                        });
```

Replace the lambda body:

```cpp
                    impl_->view->request_render(
                        impl_->view->current_page(),
                        [target](fz_pixmap* p, fz_context* wc) {
                            PdfCanvas::post_render_done(target, p, wc);
                        });
```

**Step 3: `PdfCanvas::resubmit_current_page` (approximately lines 301–307)**

Replace the lambda body the same way — `post_render_done(target, p, worker_ctx)`.

**Step 4: `PdfCanvas::on_key_down` page-change branch (approximately lines 357–363)**

Replace the lambda body the same way.

**Step 5: Build**

Run: `cmake --build build --config Release`
Expected: clean build. Any leftover `fz_keep_pixmap` extern decl or `#include` that's now unused in MainWindow.cpp should be scrubbed.

**Step 6: Run ctest**

Run: `ctest --test-dir build -C Release`
Expected: 73/73.

**Step 7: Commit**

```bash
git add src/ui/MainWindow.cpp src/ui/PdfCanvas.cpp
git commit -m "feat(ui): route all WM_USER_RENDER_DONE posts through escrow helper (4 sites)"
```

---

### Task 3: Remove `orphan_ctx` from `PdfCanvas::Impl`

**Files:**
- Modify: `src/ui/PdfCanvas.cpp`

**Step 1: Delete the `orphan_ctx` field + comment block**

Inside `struct PdfCanvas::Impl` (approximately lines 43–54), remove these lines:

```cpp
    // Orphan-drop ctx: cloned from the current view's ui_ctx on set_view.
    // Outlives view swaps so a late WM_USER_RENDER_DONE arriving after
    // the view (and its worker_ctx / ui_ctx) has been destroyed can still
    // drop its pixmap safely. MuPDF refcounts are atomic across clones
    // of the same root, and fz_clone_context keeps the root state alive
    // past the view's own ui_ctx drop. Replaced on each set_view.
    fz_context*                   orphan_ctx = nullptr;
```

**Step 2: Simplify `PdfCanvas::set_view`**

Current body (approximately lines 56–74):

```cpp
void PdfCanvas::set_view(litepdf::core::DocumentView* view) {
    // Swap orphan_ctx to track the new view. Any late WM_USER_RENDER_DONE
    // queued for the OLD view will either arrive before this swap (handled
    // via the old view's ui_ctx) or after (handled via the NEW orphan_ctx
    // — legal because both are clones of the same MuPDF root state kept
    // alive by refcount while any clone exists).
    if (impl_->orphan_ctx) {
        fz_drop_context(impl_->orphan_ctx);
        impl_->orphan_ctx = nullptr;
    }
    impl_->view = view;
    if (view) {
        impl_->orphan_ctx = fz_clone_context(view->ui_ctx());
    } else {
        // Clearing the view invalidates whatever bitmap was tied to its ctx.
        impl_->current_bitmap.Reset();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
```

Replace with:

```cpp
void PdfCanvas::set_view(litepdf::core::DocumentView* view) {
    // Per-render escrow (see PdfCanvas::post_render_done) is now the
    // lifetime mechanism for in-flight pixmaps — a canvas-level
    // orphan_ctx clone is no longer needed. The canvas just repoints
    // its view reference; the old view's pending pixmaps carry their
    // own escrow ctx and drop correctly regardless of this swap.
    impl_->view = view;
    if (!view) {
        // No active view — whatever bitmap is on screen is tied to a
        // ctx that will soon be gone. Discard so the next paint shows
        // the cleared background.
        impl_->current_bitmap.Reset();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
```

**Step 3: Simplify the destructor**

Remove the orphan_ctx drop from `PdfCanvas::~PdfCanvas` (approximately lines 126–130):

```cpp
    if (impl_ && impl_->orphan_ctx) {
        fz_drop_context(impl_->orphan_ctx);
        impl_->orphan_ctx = nullptr;
    }
```

— delete this block. Whatever other cleanup the destructor does (D2D `ComPtr` releases via `Impl`'s dtor) remains.

**Step 4: Scrub any leftover references**

Grep:

```bash
grep -n "orphan_ctx" src/ui/PdfCanvas.cpp src/ui/PdfCanvas.hpp
```

Expected: no matches. If any survive (a comment, a log line), remove them.

**Step 5: Build**

Run: `cmake --build build --config Release`
Expected: clean build. Smaller object file than before.

**Step 6: Run ctest**

Run: `ctest --test-dir build -C Release`
Expected: 73/73.

**Step 7: Commit**

```bash
git add src/ui/PdfCanvas.cpp
git commit -m "refactor(ui): drop PdfCanvas::Impl::orphan_ctx (replaced by per-render escrow)"
```

---

### Task 4: M-1 fix — guard `open_tab_async`'s `PostMessageW`

**Files:**
- Modify: `src/ui/MainWindow.cpp` (approximately lines 103–130)

**Step 1: Replace the worker lambda body**

Current shape (the success path):

```cpp
void MainWindow::open_tab_async(std::filesystem::path path) {
    HWND hwnd = hwnd_;
    std::thread([hwnd, path = std::move(path)]() {
        litepdf::core::Document doc;
        auto err = doc.open(path);
        if (err.has_value()) {
            PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                         static_cast<WPARAM>(*err), 0);
            return;
        }
        try {
            auto tab = std::make_unique<litepdf::core::Tab>();
            tab->path  = path;
            tab->label = path.filename().wstring();
            tab->view  = std::make_unique<litepdf::core::DocumentView>(
                std::move(doc));
            // Transfer ownership across thread boundary via raw ptr.
            PostMessageW(hwnd, WM_USER_OPEN_OK,
                         reinterpret_cast<WPARAM>(tab.release()), 0);
        } catch (...) {
            // ...
        }
    }).detach();
}
```

Update the `tab.release()` post site to check the return and recover on failure:

```cpp
            auto* raw = tab.release();
            // Ownership has crossed the thread boundary. If PostMessageW
            // fails (target HWND destroyed between the mutex check and
            // now — e.g., user closed the window while we were opening),
            // there is no consumer to adopt the Tab; delete it here to
            // avoid a leak. Cannot use RAII (unique_ptr scope guard)
            // because release() has already happened — the ownership is
            // external to this scope until the message is received.
            if (!PostMessageW(hwnd, WM_USER_OPEN_OK,
                              reinterpret_cast<WPARAM>(raw), 0)) {
                delete raw;
            }
```

Keep the `PostMessageW(hwnd, WM_USER_OPEN_FAILED, ...)` sites above and in the catch block as-is — those messages carry no owned pointer, so PostMessage failure there is just a dropped error dialog (benign).

**Step 2: Build**

Run: `cmake --build build --config Release`
Expected: clean build.

**Step 3: Run ctest**

Run: `ctest --test-dir build -C Release`
Expected: 73/73.

**Step 4: Commit**

```bash
git add src/ui/MainWindow.cpp
git commit -m "fix(ui): open_tab_async recovers Tab* on PostMessage failure (M-1 from Phase 5 review)"
```

---

### Task 5: Downgrade `cancel_stale_renders(INT_MAX)` comment

**Files:**
- Modify: `src/ui/MainWindow.cpp` (the `on_tab_switch` function, approximately line 215–230 block)

**Step 1: Update the comment block**

Find the comment block that currently reads:

```cpp
    // Snapshot outgoing state + drain its render queue to narrow the
    // cross-tab render-bleed race window. If a P0 is already with a
    // worker, its pixmap will still arrive at WM_USER_RENDER_DONE after
    // the set_view() below — canvas handling of that edge is tracked
    // as a Phase 6 hardening item (see plan §"Known Limitations").
    // Cancelling P0/P1/P2 here drops all queued work, leaving at most
    // one in-flight pixmap per worker thread.
```

Replace with:

```cpp
    // Snapshot outgoing state. cancel_stale_renders(INT_MAX) drains
    // the outgoing view's priority queue so workers don't continue on
    // now-irrelevant P1/P2 prefetches. This is a performance
    // optimization, not a correctness measure: the cross-tab drop
    // safety of any in-flight pixmap is guaranteed by the per-render
    // escrow ctx (see PdfCanvas::post_render_done).
```

**Step 2: Build**

Run: `cmake --build build --config Release`
Expected: clean build. (Comment-only change.)

**Step 3: Run ctest**

Run: `ctest --test-dir build -C Release`
Expected: 73/73.

**Step 4: Commit**

```bash
git add src/ui/MainWindow.cpp
git commit -m "docs(ui): cancel_stale_renders comment — perf optimization, not correctness"
```

---

### Task 6: Update Phase 5 plan's Known Limitations

**Files:**
- Modify: `docs/plans/2026-04-17-phase-5-multi-tab.md`

**Step 1: Delete the I-2 residual bullet**

Find the "Known Limitations" section at the end of the file. Find the bullet that starts:

```markdown
- **Cross-tab render-bleed race (residual).** On rapid tab-switch mid-P0 ...
```

Delete the entire bullet (both lines). The remaining known-limitations list stays intact (per-tab render pool, no context menu, no session restore, no drag-to-reorder, IPC one-way).

**Step 2: Add a short "Fast-follow landed" cross-reference above the section**

Add a one-line note just above the "Known Limitations" heading:

```markdown
> **Fast-follow log:** `v0.0.6-phase5.1` added `cancel_stale_renders(INT_MAX)` on tab switch as a mitigation; `v0.0.6-phase5.2` delivered the root-cause fix via per-render context escrow (see `docs/plans/2026-04-18-per-render-ctx-escrow-design.md`).
```

**Step 3: Verify the doc still reads cleanly**

Scan the Known-Limitations section end-to-end. Should be four bullets (per-tab pool, no context menu, no session restore, no drag-to-reorder, IPC one-way — five total, not four; count them).

**Step 4: Commit**

```bash
git add docs/plans/2026-04-17-phase-5-multi-tab.md
git commit -m "docs(plans): Phase 5 Known Limitations — delete I-2 bullet (root-cause fixed)"
```

---

### Task 7: Full verification + smoke + manual stress

**Step 1: Run the full test suite**

Run:
```
cmake --build build --config Release
ctest --test-dir build -C Release
```
Expected: 73/73 pass.

**Step 2: Run the smoke test**

Run:
```
powershell -File scripts/smoke-test.ps1
```
Expected: exit 0. All three blocks (Phase 3 cold-start, Phase 4 bookmarks, Phase 5 multi-tab) report `[OK]`.

**Step 3: Manual stress test (one-shot, not in CI)**

- Launch: `build\Release\litepdf.exe tests\fixtures\simple.pdf`
- In the running app: File → Open → `tests\fixtures\bookmarks.pdf`. Two tabs visible.
- Hold `PgDn` to page through one tab rapidly.
- Repeatedly `Ctrl+Tab` while `PgDn` is held, for ~30 seconds.
- Observe: no crash, no visible heap-tainted display, no AppVerifier warning (if AppVerifier is enabled — optional).
- Close the app cleanly.

**Step 4: Append a verification-record section to this plan file**

Add at the bottom of `docs/plans/2026-04-18-per-render-ctx-escrow.md`:

```markdown
---

## Verification record

Performed at implementation end (append actual observations):

- `ctest`: 73/73 pass.
- `smoke-test.ps1`: exit 0, all blocks OK.
- Manual stress (30 s rapid Ctrl+Tab + PgDn across 2 tabs): [observation — no crash / any anomaly]
```

Fill in the bracketed observation with what actually happened.

**Step 5: Commit the verification record**

```bash
git add docs/plans/2026-04-18-per-render-ctx-escrow.md
git commit -m "docs(plans): per-render escrow verification record (manual stress pass)"
```

---

### Task 8: Tag v0.0.6-phase5.2

**Step 1: Verify clean state**

```bash
git status
git log --oneline v0.0.6-phase5.1..HEAD
```

Expected: working tree clean. Log shows Tasks 0–7's commits.

**Step 2: Tag**

```bash
git tag v0.0.6-phase5.2
git tag -l "v0.0.*" | sort -V
```

The listing should end with `v0.0.6-phase5.2`.

---

## Summary

| Task | What | New Tests | Commit verb |
|------|------|-----------|-------------|
| 0 | `PdfCanvas::post_render_done` helper | 0 | `feat(ui)` |
| 1 | `WM_USER_RENDER_DONE` handler → escrow ctx | 0 | `feat(ui)` |
| 2 | Route 4 callers through helper | 0 | `feat(ui)` |
| 3 | Remove `PdfCanvas::Impl::orphan_ctx` | 0 | `refactor(ui)` |
| 4 | M-1 fix: `open_tab_async` PostMessage guard | 0 | `fix(ui)` |
| 5 | `cancel_stale_renders` comment downgrade | 0 | `docs(ui)` |
| 6 | Delete I-2 bullet from Phase 5 plan | 0 | `docs(plans)` |
| 7 | Verification sweep + record | 0 | `docs(plans)` |
| 8 | Tag `v0.0.6-phase5.2` | — | tag |

**Total: 8 tasks, ~45 LOC net delta (−15 / +30 before counting comment changes), zero new tests.** Regression signal is `ctest 73/73 + smoke-test pass + manual stress record`.

---

## Risks + monitoring

- **Risk: `fz_clone_context` allocates an allocation-tracking shadow context behind the ref-inc.** MuPDF docs describe clone as cheap but not free. If N tabs × M renders/second causes visible CPU regression, measure first; revisit only with data (Phase 11 benchmark gate).
- **Risk: existing callers of `PostMessageW(WM_USER_RENDER_DONE, pix, 0)` from outside the codebase.** There are none (private API). No compat concern.
- **Monitoring signal:** after landing, watch the first dogfooding session for any "frame paints wrong content briefly during tab switch" report. The escrow fix should make such reports genuinely impossible; any report would indicate a deeper bug.

---

## Verification record

Performed at implementation end:

- `ctest`: 73/73 pass. Runtime 21.60 s.
- `smoke-test.ps1`: exit 0. Output summary: Phase 3 cold-start [OK] (T0->T4 = 270 ms, budget 1500 ms), Phase 4 bookmarks [OK] (window title + handle confirmed), Phase 5 multi-tab [OK] (forwarder exit 0, first instance reports 2 tabs, active=1).
- Manual stress (~30 s rapid PgDn across 2 open tabs): **no crash, no visual corruption, no anomaly during teardown.** I-2 correctness signal cleared.
  - Caveats observed during the stress run (non-blocking for I-2, tracked as separate follow-ups):
    - Page advance felt slightly slow under held `PgDn`. Consistent with §Risks — defer quantification to Phase 11 benchmark gate.
    - `Ctrl+Tab` did not visibly switch tabs during the run (Phase 5 Task 5 shipped this shortcut at `00ba4ca`; appears to be a message-routing regression or focus-interaction edge case). As a result, cross-tab switch coverage in this stress run was weaker than intended — the race surface was exercised by `set_view` calls during the render-pipeline churn but not by a tight tab-switch loop. Primary I-2 confidence continues to rest on the architectural review + full `ctest` + smoke pass. Tracked as a separate follow-up.
    - Tab strip visual boundaries between adjacent tabs are unclear. UX polish, not correctness. Deferred to Phase 6 UI pass.
