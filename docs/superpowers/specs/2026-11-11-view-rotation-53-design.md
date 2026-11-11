# On-screen view rotation (#53) — design

Status: designed in chat 2026-11-11; behaviour decisions made by the user, approach chosen after two
Fable reviews against `82f56a0` and a D2D spike. Spec gate not run yet.
Issue: #53. Related decisions: Phase 8 D9 (per-tab view flags are not persisted),
`project_litepdf_held_place_217`, `project_litepdf_continuous_scroll_55`.

## 1. Problem

There is no way to rotate the page on screen; a scan that arrives sideways stays sideways. Rotation
exists only on the print path (`Document::render` `rotate_90`, `src/core/Document.cpp:331`, applied
with `fz_pre_rotate` at `:358`). The on-screen matrix is scale only
(`src/core/RenderEngine.cpp:270`, `fz_scale(req.scale, req.scale)`).

This is view rotation: nothing is written to the file, so it does not conflict with the read-only
identity (design §10 rules out *rotate-and-save*, not this).

## 2. Goal and non-goals

Goal: the reader can turn the whole document by 90° steps in the active tab, in all three layouts
(single page, two-page spread, continuous strip), with fit modes, search highlights, the text
selection, drag-select and copy all correct in the rotated view.

Non-goals (decided with the user):

- Per-page rotation. One rotation per tab, for every page.
- Arbitrary angles. Quarter turns only.
- Persistence. Not saved across restarts; the session schema stays v2 (same as invert and
  two-page, Phase 8 D9). A later issue may persist all three view flags in one schema bump.
- Printing. The print path keeps its own paper-orientation auto-rotate and ignores the view.
- Thumbnails. They stay unrotated (thumbnails do not follow invert either, and the fixed
  120 x 160 portrait tile, `ThumbnailPane.cpp:67`, would stretch a turned page).
- Keeping the reader's in-page point across a rotation (centre-point carry). It would need a
  fourth anchor kind at fit zoom; the reader lands at the page top instead (§3.5).

## 3. Design

### 3.1 Decisions

| # | Decision | Why |
|---|---|---|
| R1 | Render unrotated; draw the bitmap through a D2D world transform. | Bitmaps, `RenderRequest`, L1/L2, `StripSchedule`, text pages and search results stay rotation-free: no cache key axis, no stale-rotation bitmap, no re-render at custom zoom. |
| R2 | One pure page↔view transform serves drawing, overlays, presses and scroll-into-view. | Bitmap and overlays are placed by the same matrix, so they cannot drift apart. |
| R3 | The transform's extent is the **placed rect**, never `page_pt × zoom`. | `fz_round_rect` puts the pixmap's rounding slack at the far edge; a rotation flips it to the near edge. In the strip the placed rect is the layout rect, which is also right over a stale-zoom bitmap or no bitmap. |
| R4 | `DocumentView` keeps unrotated sizes internally and reports view-frame sizes publicly. | The "a page's size never changes for the Document's life" invariant of the caches still holds. |
| R5 | A rotation lands at the current page's top and ends every carry and anchor. | The only landing that is right in every layout; it reuses the existing `Kind::Top` anchor. |
| R6 | Commands: Rotate Clockwise `Ctrl+Shift+=`, Rotate Counterclockwise `Ctrl+Shift+-`. | The Acrobat and SumatraPDF binding. Neither combination is bound today (`MainWindow.cpp:2740-2741` bind `VK_OEM_PLUS` / `VK_OEM_MINUS` with `FCONTROL` only). |

**Spike (R1's premise), 2026-11-11, throwaway, not in the repo.** `DrawBitmap(src)` under the
rotation matrix against `DrawBitmap(CPU-pre-rotated src)` under identity at the same destination
rect, LINEAR interpolation, bitmaps at 96 dpi as in the canvas. Targets: WIC (software), D3D11
hardware, WARP; dpi 96/120/144/168/192; 90/180/270; destination offsets 0, integral, half-DIP,
(7.25, 3.75), (33.333, 12.6). Result: 220/225 pixel-identical; the other 5 only at the arbitrary
fractional offset, max channel difference 1/255. `D2D1::Matrix3x2F::Rotation(90)` is
`{_11=0, _12=1, _21=-1, _22=0}`: clockwise on a y-down frame, the same as MuPDF `fz_rotate(90)`
(`third_party/mupdf/source/fitz/geometry.c:155-159`).

### 3.2 The pure transform — `src/core/detail/ViewRotation.hpp`

No Win32, no Direct2D, no MuPDF (the `ui/detail/ViewportMath.hpp:3-5` convention). It lives under
`core/detail` because `DocumentView` (core) and the canvas (ui) both use it.

- `struct Rotation { int quarters; }` — 0..3 clockwise quarter turns; `cw()` / `ccw()` wrap.
  `swaps_axes()` is true at 1 and 3.
- `view_size(w, h, rot)` — `(h, w)` when the axes swap, else `(w, h)`.
- `struct Affine { float m11, m12, m21, m22, dx, dy; }` — a plain 2-D affine, same layout as
  `D2D1_MATRIX_3X2_F`; `PdfCanvas.cpp` converts it.
- `draw_affine(rot, view_rect)` — maps the **unrotated local frame** `[0, uw] x [0, uh]` onto
  `view_rect`, where `(uw, uh) = view_size(view_rect.w, view_rect.h, rot)` swapped back. Clockwise:
  local top-left lands on the view rect's top-right at 90°, bottom-right at 180°, bottom-left at
  270°.
- `local_to_view(p, rot, view_rect)` / `view_to_local(p, rot, view_rect)` — the same map and its
  inverse, for single points.
- `local_rect_to_view(r, rot, view_rect)` — the bounding box of a local rect in the view frame
  (scroll-into-view).

"Local frame" is the unrotated page, in DIPs, origin at the page's top-left: exactly the frame the
overlays already use (`quad_bounds_dip(q, ox, oy, zoom)` with `ox = oy = 0`).

### 3.3 `DocumentView` — the per-tab state

- `Impl::rotation` (a `Rotation`, default 0); `rotation()`, `rotate_cw()`, `rotate_ccw()`.
  A rotation cancels no render and touches no cache.
- `known_size` and the strip caches (`page_sizes`, `strip_sizes`, `strip_sized`,
  `DocumentView.cpp:103, 108-109`) keep unrotated sizes. Internal users stay on them:
  `estimated_bytes` (area only), the prefetch plan, CBZ read-ahead, the `PageSizeFill` stand-ins.
- Public `known_page_size()` returns `view_size(...)` of the cached size.
- Public `page_sizes()` returns a view-frame vector. At rotation 0 it is `strip_sizes` itself (the
  "same vector" test, `test_document_view_strip.cpp:184-186`, keeps holding). Otherwise a second
  vector, built **lazily** on the first `page_sizes()` call after a rotation — a rotation in paged
  mode must not trigger the full sizing pass (`DocumentView.cpp:323-337`).
- `set_viewport`'s fit uses view-frame sizes (`DocumentView.cpp:382-385`), so fit width and fit
  page refit after a rotation. Custom zoom keeps its percentage.
- Amend the `page_sizes()` doc comment (`DocumentView.hpp:114-118`, "sized once and kept for the
  life of the view"): the sizing is still once; the reported frame follows the rotation. The
  continuous-scroll spec §4.4 gets the same note by reference to this spec.

External callers checked (all want view-frame or are area-only): `PdfCanvas.cpp:642`
(`shown_page_pct`), `:1749` (`pending_page_size`); `PdfCanvasStrip.cpp:287` (layout build),
`:360`, `:439` (carries), `:603` (`bytes_of`); `src/cli/main.cpp:253` (bench, always rotation 0).
`PrintJob.cpp:292` reads the `Document`, not the view.

### 3.4 Drawing — `PdfCanvas` / `PdfCanvasStrip`

The render target's transform is identity everywhere today (no `SetTransform` in `src`); pan is
rect arithmetic. That stays true outside the per-page blocks below.

- **`view_px(bm)`** replaces the six `bm->GetSize()` readers (`PdfCanvas.cpp:628, 644, 652, 1670,
  2308`; `PdfCanvasStrip.cpp:950`) and swaps at 90/270. Placement, `content_extent`,
  `shown_page_pct`'s `drawn_zoom_pct` and the cursor tests then see view-frame extents.
- **Per page** (single page; each spread slot; each strip page): compute the view-frame placed
  rect as today, then `SetTransform(draw_affine(rot, rect))`, and in the local frame draw the
  bitmap into `[0, uw] x [0, uh]`, then the hits (`paint_hit(hit, 0, 0, pct)`) and the selection,
  then `SetTransform(identity)`. The overlay origin becomes the local origin; today it is
  `dst.left / dst.top` (`PdfCanvas.cpp:2363, 2441`) or the strip rect (`PdfCanvasStrip.cpp:983`).
- The spread's `PushAxisAlignedClip` (`PdfCanvas.cpp:2360-2362`) is pushed under identity, before
  the slot's transform, and popped after the transform is reset.
- The strip's stale-zoom stretch (`PdfCanvasStrip.cpp:957-962`) needs no special case: the local
  frame is defined from the page rect (R3), so a stale bitmap is stretched in the local frame as
  today and then rotated.
- The canvas reads `view->rotation()` at paint time. No copy is mirrored onto the canvas.

### 3.5 Presses, scroll-into-view, landing

- **Presses.** `page_point_at` (`PdfCanvas.cpp:791-799`) maps the canvas point through
  `view_to_local` with the same placed rect, then applies the existing clamp
  (`canvas_dip_to_page_point`, `SelectionDrag.hpp:265-274`) in the local frame, then hands page
  points to MuPDF. Both drag endpoints are mapped independently; MuPDF orders them
  (`stext-search.c:261-265, 304-305`), so drag direction does not matter. `TextSelection` stays in
  page space and survives a rotation.
- **Scroll-into-view** takes the hit's view-frame box from `local_rect_to_view`: `scroll_into_view`
  (`PdfCanvas.cpp:474-567`), `pan_y_for_hit` (`:1953-1983`), `quad_fully_visible`,
  `StripViewport::scroll_hit_into_view`, `hit_top_pt` / `hit_bottom_pt` (`PageAnchor.hpp:93-98`)
  and `landing_pan_y` (`PdfCanvasStrip.cpp:326-344`). At 90/270 the vertical extent comes from the
  quad's x extent.
- **Landing (R5).** New `PdfCanvas::on_rotation_changed()`, called by the command handler **before**
  the kick:
  1. read the current page (strip: `view->current_page()`, before any reset);
  2. `cancel_gesture()`;
  3. paged and spread: `pan_x = pan_y = 0`, then `set_pending_anchor(PageAnchor::top())`, which
     already clears `held_place` and `held_page` (`PdfCanvas.cpp:387-395`); the anchor re-places
     after a fit-mode re-render. At custom zoom the kick still submits and an L1 hit still posts a
     completion, so the anchor applies either way; zeroing the pan first avoids one frame at the
     old pan;
  4. strip: clear `held_place` / `held_page`, then `strip_view.reset()` only;
  5. `InvalidateRect` (the single-page kick does not invalidate, and `same_overlay` compares
     page-space hits, which a rotation does not change).
- **`KickReason::Rotate`** (`EffectiveLayout.hpp`): strip actions `{refit, refresh_readout,
  relayout}`. With the layout reset, `strip_kick`'s existing `built_here` branch
  (`PdfCanvasStrip.cpp:490-502`) builds at the refitted zoom from the new view-frame sizes and
  navigates to `view->current_page()`. One owner for the build, no new action flag. The paged
  branches of `kick_render` do not read the reason (`MainWindow.cpp:343-344`) and need no case.
- In the strip the landing is the top of the gap above the page (`StripLayout::anchor`,
  `StripLayout.hpp:101`), 8 DIP above the page, as for any navigation.

### 3.6 Commands — `MainWindow` and resources

- `IDM_VIEW_ROTATE_CW 40067`, `IDM_VIEW_ROTATE_CCW 40068` (the reserved 40067-40070 block,
  `resources/MainMenu.rc.h:78`; update the "next free" comment).
- View menu items in `resources/litepdf.rc.in`, with the accelerator text; actions, no check mark.
- Accelerators `{FCONTROL|FSHIFT|FVIRTKEY, VK_OEM_PLUS}` / `{..., VK_OEM_MINUS}`.
- Enabled only with an active document, in the `WM_INITMENUPOPUP` View arm
  (`MainWindow.cpp:1689-1725`). The handler returns early without an active view.
- Handler: `view->rotate_cw()` (or `ccw`), `canvas_->on_rotation_changed()`,
  `kick_render(page, KickReason::Rotate)`. Every branch of `kick_render` already refreshes the zoom
  readout (`MainWindow.cpp:354, 395, 416`), so a fit-mode refit shows its new percentage.
- Tab switch needs nothing new: the rotation lives on the tab's `DocumentView`, and the canvas
  reads it at paint time.

## 4. Accepted behaviour (not changed, written down)

- **Search hits are positioned vertically only**, as today (`scroll_into_view` writes `pan_y` only,
  `PdfCanvas.cpp:565`). At 90° a hit on a page wider than the viewport can sit off-screen
  horizontally; this is already true of wide pages.
- **The reader's in-page point is not kept** across a rotation (§2).
- **On a US layout `Ctrl+Shift+=` is what some users type for "Ctrl +".** Today it does nothing;
  after this change it rotates. Zoom in stays on `Ctrl+=`. Same binding as Acrobat and SumatraPDF.
- **A spread kick cancels queued prefetch and thumbnail work** (`cancel_stale_renders(0)`,
  `MainWindow.cpp:388`), as it does for any spread kick today.
- **Ctrl+wheel zoom has no anchor point**; it is unaffected by rotation.

## 5. Testing

Unit tests run in the existing `litepdf_unit_tests` binary; ASCII test names prefixed by subsystem.
Every test that guards a seam has an absolute expectation, not only a round trip.

### 5.1 Pure — new `tests/unit/test_view_rotation.cpp`

- `cw` / `ccw` wrap; `swaps_axes`; `view_size`.
- `draw_affine` for all four rotations: the four local corners land on named view-rect corners
  (90°: local top-left → view top-right; 180°: → bottom-right; 270°: → bottom-left), multiplied by
  hand, no D2D.
- `local_to_view` absolute expectations for an interior asymmetric point at every rotation, plus
  the round trip with `view_to_local`.
- **Rounding slack (R3):** an 842 pt page at render scale 1.25 has a 1053 px placed extent; at 90°
  page point `(0, 842)` maps to view x = 0.5 px, not 0.
- `local_rect_to_view` at every rotation; hit top/bottom in the view frame (90°: x, 270°: W − x).

### 5.2 `DocumentView` — `test_document_view.cpp`, `test_document_view_strip.cpp`

- `known_page_size` and `page_sizes()` swap at 90/270, not at 180, identity after four turns.
- At rotation 0 `page_sizes()` is still the same vector as before.
- A rotation in paged mode does not run the full sizing pass.
- On a portrait page, fit width's zoom changes after a rotation; custom zoom's does not.
- A rotation plus kick at custom zoom starts no new render (`renders_started()` unchanged). This is
  a guard of R1, not a seam test.

### 5.3 `EffectiveLayout` — `KickReason::Rotate` row: `{refit, refresh_readout, relayout}`.

### 5.4 Canvas rig — new `tests/unit/test_canvas_rotation.cpp`

Real `PdfCanvas` on a hidden window (`reference_litepdf_canvas_test_rig`). The test process is DPI
unaware, so 1 px = 1 DIP. Pixels are read with `CopyFromRenderTarget` after `WM_PAINT`.

New fixture `tests/fixtures/rotation-marker.pdf`, generated by
`scripts/generate-rotation-fixture.py` (reportlab, `pageCompression=0`): two portrait pages, a
solid black square near the top-left corner, and one line of known text near the top.

Cases:

1. Single page, custom zoom: after 90° the square's pixels are at the top-right; 180°
   bottom-right; 270° bottom-left.
2. `content_extent` swaps at 90°.
3. `page_point_at` at a known canvas point returns the expected page point at each rotation.
4. A search hit's highlight rectangle lands over the rotated text (pixel or rect check).
5. `scroll_into_view` at 90° uses the hit's x extent (an overflowing page, a hit far right in
   page space ends up vertically in view).
6. Spread on `spread-pairs.pdf` (not `spread-unequal.pdf`, which never pairs on screen): the right
   slot draws rotated inside its clip, its hit is placed in the right slot, and a pair of unequal
   pages gives the expected extent.
7. Rotation over a stale-zoom bitmap, paged and strip: with the zoom completion held, a press at
   the rect's view top-right corner at 90° returns page `(0, 0)` (R3).
8. Drag-select at 90° then copy returns the fixture's known text.
9. Strip at **custom zoom**: after a rotation `layout().height(0)` equals the page width, the
   current page is unchanged, and the viewport is at the page's anchor.
10. Strip at fit width on `large.pdf` page 30: after a rotation the current page is 30 and
    `layout().zoom_pct()` is the refitted zoom.
11. Carry: strip at **custom zoom**, scrolled to a held offset > 0, spread on (sets `held_place`),
    rotate, spread off: lands at the page top, not the held offset.
12. Single page at custom zoom, panned down, rotate: `pan_y == 0` after the completion.
13. A rotation during a live drag cancels the gesture.

### 5.5 GUI smoke — `scripts/smoke-test.ps1`

The default fixture is `simple.pdf` (one A4 portrait page), `LITEPDF_NO_RESTORE=1`, fit width,
and the default layout is the strip.

- **Range row:** post `WM_COMMAND IDM_VIEW_ROTATE_CW`; the existing `canvas-scroll` probe reads the
  strip's range, which is predictable: `8 + n·(h_view·z + 8)` with the refitted `z`. Four turns
  restore the original range.
- **Direction row:** open `search.pdf`, rotate clockwise once, Find Next, and compare the
  `canvas-scroll` position with the one predicted from the hit's x extent; counter-clockwise
  predicts `W − x`. This catches a swapped command binding, which no unit test can (`MainWindow`
  is not in the test binary).
- **Accelerator row:** real `SendInput` `Ctrl+Shift+=` only after asserting
  `GetForegroundWindow()` is the app; otherwise skip with a logged reason. `smoke-test.ps1` has no
  `SendInput` today.
- **Menu row:** the `menu-state` probe reports both items disabled with no document.

Each row is proven able to fail (`reference_litepdf_smoke_script_rules`).

### 5.6 Mutation

One mutant per seam, each must turn a test red: the size swap; `view_px`'s swap; `draw_affine`'s
corner mapping; `view_to_local`; `local_rect_to_view`; the `held_place` clear; `strip_view.reset()`;
the Top landing; the cw/ccw binding (smoke direction row). Report the kill count.

### 5.7 Full run

Full `ctest -C Release`, serial, from PowerShell. The smoke run as for any UI change.

## 6. Out of scope, release

Out of scope: per-page rotation, persistence, thumbnails, print, centre-point carry, horizontal
hit centring. Release: one CHANGELOG line under Unreleased ("Rotate the view in 90° steps:
View > Rotate Clockwise / Counterclockwise"). `VERSION` is not bumped by this change.
