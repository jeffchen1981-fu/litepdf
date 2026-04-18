# Per-Render Context Escrow — Design

**Date**: 2026-04-18
**Status**: Approved, ready for implementation plan
**Scope**: Fast-follow for LitePDF Phase 5 — eliminates the residual cross-tab render-bleed race documented as I-2 in the Phase 5 final review.

## 1. Problem

Phase 5 introduced multi-tab rendering. Each tab owns an independent `DocumentView`; each `DocumentView` has its own MuPDF root context obtained via `fz_new_context()`. When the user switches tabs while a P0 render is still in flight on the outgoing tab, the pixmap arrives at `PdfCanvas::WM_USER_RENDER_DONE` after `canvas_->set_view(incoming)` has already repointed the canvas. The current handler uses `impl_->view->ui_ctx()` (i.e., the **incoming** view's context) for `fz_pixmap_width` and `fz_drop_pixmap`, but the pixmap's allocator pool is tied to the **outgoing** view's root. This lands us at `fz_drop_pixmap(viewB_ctx, viewA_pixmap)`, which invokes MuPDF's allocator on the wrong root's memory arena — undefined behavior.

Phase 5.1 shipped a mitigation: `on_tab_switch` now calls `outgoing->view->cancel_stale_renders(INT_MAX)` to drain the queue, narrowing the race window to at most one in-flight pixmap per worker thread. The root cause — the fact that the pixmap and its required drop-context are decoupled at the message-passing boundary — is not fixed.

## 2. Design: Per-Render Context Escrow

Every pixmap that crosses the `PostMessageW` boundary travels with a companion `fz_context*` that is:

- A fresh `fz_clone_context()` of the worker's own ctx, created **inside** the render callback on the worker thread.
- Guaranteed to share the same MuPDF root as the pixmap (because both were minted from the same `DocumentView`'s clone lineage).
- Independently ref-counted against the root, so it outlives both the worker's `fz_context*` and the `DocumentView` that produced the pixmap.

The escrow ctx is used by the canvas for every `fz_*` call on that pixmap — reading dimensions, reading samples, dropping. The canvas no longer consults `impl_->view->ui_ctx()` for fz operations on render-done; `impl_->view` is demoted to a pure UI-state pointer that selects which tab's bitmap is displayed.

### 2.1 Invariants

1. **Pairing.** Any pixmap delivered via `WM_USER_RENDER_DONE` is paired with a non-null `fz_context*` on LPARAM, unless the pixmap itself is null.
2. **Same root.** The escrow ctx is a clone of the context that produced the pixmap, so its allocator reaches the pixmap's memory pool correctly.
3. **Lifetime order.** The canvas drops the pixmap via the escrow ctx, then drops the escrow ctx. Ctx lifetime ≥ pixmap lifetime.
4. **View independence.** `PdfCanvas::Impl::view` is never used for fz ops after the swap. It drives only D2D bitmap placement and the `WM_USER_RENDER_DONE` early-out when there is no active view.

### 2.2 Comparison with alternatives rejected

- **Option B — per-view generation token.** Requires tagging every render request with a generation, having the canvas compare and discard stale. Does not solve the drop-ctx mismatch — you still need a valid ctx to drop the stale pixmap. Rejected.
- **Option D — synchronous drain on tab switch.** Wait for workers to idle before setting a new view. Simplest but adds 100–500 ms latency on HDD tab-switches during a P0 — violates the design §5.1 "tab switch should feel instant" UX contract. Rejected.

## 3. Components changed

| File | Change |
|---|---|
| `src/ui/MainWindow.cpp` | `kick_render` lambda clones worker_ctx, keeps pixmap, posts `{pixmap, escrow}` to canvas. Lambda guards on clone/post failures. |
| `src/ui/MainWindow.cpp` | `open_tab_async` lambda checks `PostMessageW` return; on FALSE, `delete raw` to avoid Tab leak (addresses M-1 from final review). |
| `src/ui/MainWindow.cpp` | Comment near `cancel_stale_renders(INT_MAX)` in `on_tab_switch` demoted from "correctness mitigation" to "perf optimization — correctness now via per-render escrow ctx". |
| `src/ui/PdfCanvas.cpp` | `WM_USER_RENDER_DONE` handler reads escrow ctx from LPARAM, uses it for every fz op, drops pixmap then ctx. |
| `src/ui/PdfCanvas.cpp` | `struct Impl::orphan_ctx` field and its maintenance in `set_view` removed (now redundant; replaced by per-render escrow). |
| `docs/plans/2026-04-17-phase-5-multi-tab.md` | Delete the "Cross-tab render-bleed race (residual)" bullet from "Known Limitations" — race is fully resolved. |

Public headers are not touched. `PdfCanvas`'s `set_view(DocumentView*)` signature is unchanged.

Estimated diff size: approximately −15 / +15 lines net across two .cpp files. This is a "reduction" refactor — the removal of `orphan_ctx` pays for most of the added lambda complexity.

## 4. Data flow

```
[UI thread]  kick_render(page)
    view->request_render_with_prefetch(page, cb)
    cb captures: target HWND
                                 |
                                 v
[Worker thread N]  render page -> fz_pixmap* pix
    cb(pix, worker_ctx):
      if pix:
        fz_keep_pixmap(worker_ctx, pix)                // refcount 1 -> 2
        escrow = fz_clone_context(worker_ctx)          // ref-inc on root
        if escrow == nullptr:                          // OOM
          fz_drop_pixmap(worker_ctx, pix)
          return
        if !PostMessageW(target, WM_USER_RENDER_DONE, pix, escrow):
          fz_drop_pixmap(escrow, pix)
          fz_drop_context(escrow)
                                 |
                                 v
         ... message queue (may outlive view + worker) ...
                                 |
                                 v
[UI thread]  PdfCanvas::WM_USER_RENDER_DONE
    pix    = WPARAM
    escrow = LPARAM
    use escrow for fz_pixmap_width/height/stride/samples
    CreateBitmapFromMemory -> impl_->current_bitmap
    fz_drop_pixmap(escrow, pix)                        // refcount 2 -> 1 (worker still owns the original ref)
    fz_drop_context(escrow)                            // root ref-dec
    InvalidateRect -> WM_PAINT
```

## 5. Error handling

- **OOM on `fz_clone_context`.** Drop pixmap via worker_ctx (still alive inside the callback), do not post. Render appears dropped; next kick will repair.
- **`PostMessageW` returns FALSE.** HWND has been destroyed. Drop pixmap via escrow, drop escrow. No leak.
- **`open_tab_async`'s `PostMessageW` returns FALSE.** Recover the released `Tab*` via `delete raw`. Comment documents why RAII is not usable here (ownership was released across the thread boundary).
- **Partial pipeline on canvas teardown.** `PdfCanvas::~PdfCanvas` runs during window destruction. Any late `WM_USER_RENDER_DONE` messages already queued will be dropped by the OS when the process exits; their pixmaps + escrow ctxs leak into OS cleanup. Acceptable — process is terminating.

## 6. Testing

No new unit tests. `WM_USER_RENDER_DONE` requires a live D2D render target and a real `fz_pixmap`; integration-level behavior is not unit-testable without a harness. The existing `TabList` tests (9 cases) continue to cover activation-policy invariants, which this change does not touch.

Regression signal:

- `ctest` remains at 73/73.
- `scripts/smoke-test.ps1` Phase 5 block (two-tab forward + active==1) continues to pass.
- One-shot manual stress pass, performed during implementation verification: rapid `Ctrl+Tab` while holding `PgDn` across two open documents for ~30 s. Observe no crash, no AppVerifier heap warning. Not added to CI (too flaky) — run once and record observation in the implementation plan.

## 7. Follow-ups not in scope

- **I-1 (per-tab RenderEngine pool → 2N threads).** Remains a known limitation; Phase 11's benchmark gate will quantify.
- **Stale Phase-3 / Phase-4 comments in PdfCanvas.cpp / DocumentView.hpp.** M-3 from the final review. Defer to a dedicated comment-cleanup chore or fold into Phase 6 prep.
- **M-8: `strip_height(UINT /*dpi*/)` unused parameter.** Defer.

## 8. Done when

1. `WM_USER_RENDER_DONE` handler uses escrow ctx exclusively; `impl_->view->ui_ctx()` no longer appears in the handler.
2. `struct PdfCanvas::Impl::orphan_ctx` and its `set_view` maintenance code are removed.
3. `kick_render`'s lambda clones worker_ctx and posts the pair. Guards on `fz_clone_context == nullptr` and on `PostMessageW == FALSE`.
4. `open_tab_async`'s lambda guards on `PostMessageW == FALSE` (M-1 fix).
5. `cancel_stale_renders(INT_MAX)` comment in `on_tab_switch` re-describes the call as a perf optimization rather than a correctness mitigation.
6. The Phase 5 plan's Known-Limitations bullet about this race is deleted.
7. `ctest` 73/73 green, `smoke-test.ps1` exit 0, manual stress pass observation recorded.
8. Commit history is split into logical units (at minimum: escrow fix, orphan cleanup, M-1 fix, plan-doc update) — exact split decided by the implementation plan.
9. Tagged `v0.0.6-phase5.2` on the final commit (matches the `v0.0.4-phase3.1` / `v0.0.5-phase4.1` / `v0.0.6-phase5.1` follow-up pattern in this repo).
