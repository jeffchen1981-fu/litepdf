# Tab Strip Polish — Design

Date: 2026-04-19
Status: Design approved, awaiting implementation plan
Follows: Phase 5 Task 3 (commit f4d1629) — `TabManager` + `TabList`
Author: Claude (Opus 4.7) with user sign-off

## Motivation

User feedback on the Phase 5 multi-tab strip (2026-04-19):

> tab 很小且不明顯，與隔壁 tab 沒有明顯分界

Current implementation uses the default Win32 `SysTabControl32`
(`WC_TABCONTROLW`) with no owner-draw. Under the Windows 11 flat theme,
`SysTabControl32` renders tabs as near-pure text on the control background
— no tab shape, no border, no separator between adjacent tabs. Active vs
inactive is distinguished only by a subtle font-weight / grayscale
difference. The strip height collapses to ~20 px at 96 DPI, shorter than
the menu bar above it, so the tab row reads as a subtitle rather than a
navigation control.

Evidence: screenshot of 3 open PDFs (`simple.pdf`, `bookmarks.pdf`,
`測試.pdf`) shows the three labels sitting flat on the menu-bar
background with no separators, minimal contrast, and no close affordance.

## Goals

1. Make each tab visibly bounded with a clear separator between adjacent
   tabs.
2. Make the active tab unambiguously identifiable at a glance.
3. Add per-tab close buttons (complements existing middle-click / Ctrl+W).
4. Truncate overflowing labels with an ellipsis instead of an abrupt cut.
5. Respect system dark mode, with hot-switch support (no restart
   required).

## Non-goals

- Drop shadows or animated transitions on the active tab.
- Drag-to-reorder tabs.
- New public API surface on `TabManager` beyond two small delegate hooks.
- Changes to `TabList` or any existing unit test.

## Approach

**Selected: B — Owner-draw via `TCS_OWNERDRAWFIXED` + `WM_DRAWITEM`.**

Rejected alternatives:

- **A — `TCM_SETITEMSIZE` + custom font, no owner-draw.** Cheapest
  (~30 LOC) but the OS theme still owns the paint, so separators and
  active-highlight remain impossible. Fails goals 1–2.
- **C — Replace `SysTabControl32` entirely with a D2D-painted child
  HWND.** Maximum control but requires re-implementing hit-testing,
  keyboard navigation (Ctrl+Tab), focus, DPI, and event routing — large
  rewrite for a polish task. YAGNI.

Approach B keeps `SysTabControl32` (hit-test, keyboard navigation, DPI
metrics, `TCN_SELCHANGE`, and the existing WM_MBUTTON subclass all
continue to work) and only takes over paint. All changes stay inside the
PIMPL boundary of `TabManager.cpp`; the header gains only two small
public delegate methods.

## Visual Specification

96 DPI baseline; all pixel values scale with `MulDiv(value, dpi, 96)`
via `GetDpiForWindow(impl.hwnd)`.

| Element | Normal | Hover | Active |
|---|---|---|---|
| Tab height | 32 px | 32 px | 32 px |
| Tab width | clamp(120, label_w + 44, 240) | same | same |
| Background | transparent | `#F0F0F0` / `#3A3A3A` | `#FFFFFF` / `#2D2D2D` |
| Top accent bar | — | — | 2 px, `COLOR_HOTLIGHT` |
| Text color | `COLOR_GRAYTEXT` | `COLOR_BTNTEXT` | `COLOR_BTNTEXT`, bold |
| Right separator (1 px) | drawn | drawn | not drawn |
| Close button (14×14) | hidden | visible | visible |
| Close button hover | — | red background + white × | same |
| Label draw flags | `DT_END_ELLIPSIS \| DT_SINGLELINE \| DT_VCENTER` | same | same |

Colors listed as `light / dark` pairs switch with detected theme.

### Close button geometry (per tab, in tab-local coords)

```
close_rect = {
  .left   = tab.right - dpi_scale(22),
  .top    = (tab.cy - dpi_scale(14)) / 2,
  .right  = tab.right - dpi_scale(8),
  .bottom = (tab.cy + dpi_scale(14)) / 2,
}
```

Close-button hit-test takes precedence over tab-select: clicking the ×
fires `on_close_request(i)` and does not change the active tab.

### Dark-mode detection

Order of preference, first available wins:

1. `DwmGetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ...)`
2. Registry `HKCU\...\Personalize\AppsUseLightTheme` (DWORD; 0 = dark).
3. Luminance of `GetSysColor(COLOR_WINDOW)` (fallback).

On `WM_SETTINGCHANGE` with `lParam == "ImmersiveColorSet"`, re-detect and
invalidate the tab strip. See **Hot-switch wiring** below.

## Architecture

### File map

| File | Change |
|---|---|
| `src/ui/TabManager.hpp` | +2 public methods: `handle_draw_item`, `handle_theme_change`. No other changes. |
| `src/ui/TabManager.cpp` | +~215 LOC: owner-draw paint, hover tracking, close-button hit-test, theme detection, RAII-wrapped GDI resources. |
| `src/ui/MainWindow.cpp` | +~15 LOC: WM_DRAWITEM and WM_SETTINGCHANGE delegates to `tabs_`. |
| `src/core/TabList.*` | unchanged. |
| `tests/unit/TabListTests.cpp` | unchanged (must remain green). |

### PIMPL boundary

`TabManager::Impl` gains the following members (all private to the .cpp):

- Cached theme state: `bool dark_mode;`
- Cached DPI for font lifecycle: `UINT cached_dpi;`
- RAII-wrapped GDI resources: `unique_hfont font_normal, font_bold;`
  `unique_hbrush brush_hover_bg, brush_active_bg, brush_separator, …;`
- Hover state: `int hovered_tab = -1;` `bool hovered_close = false;`
- Close-button press state: `int pressed_close = -1;` (mirrors existing
  `pressed_tab` for middle-click).
- Whether `TrackMouseEvent(TME_LEAVE)` is currently armed.

`unique_hfont`, `unique_hbrush`, etc. are file-local
`std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)>`
aliases so destruction is automatic and exception-safe.

## Data Flow

### Paint

```
OS invalidates tab strip
  → SysTabControl32 posts WM_DRAWITEM to its parent (MainWindow)
  → MainWindow::window_proc(WM_DRAWITEM)
      → if (tabs_ && tabs_->handle_draw_item(lParam)) return TRUE;
  → TabManager::handle_draw_item(const DRAWITEMSTRUCT*)
      → dispatches to Impl::paint_tab(DRAWITEMSTRUCT*)
      → paint_tab():
          1. Compute state (normal / hover / active) from itemID vs
             active_index() and impl.hovered_tab.
          2. Fill background.
          3. If active: draw 2 px top accent bar.
          4. Draw label with DT_END_ELLIPSIS; reserve right padding for ×.
          5. If active || hovered: draw close button (red bg if
             impl.hovered_close && impl.hovered_tab == itemID).
          6. If not active: draw 1 px right separator.
```

`handle_draw_item` returns `true` iff it handled the struct (i.e. it was
ours), letting `MainWindow` decide whether to fall through to
`DefWindowProc`.

### Hover tracking

```
WM_MOUSEMOVE (in tab_subclass_proc)
  → TCM_HITTEST → tab index i
  → compute close_rect for i; test point → bool on_close
  → if (i, on_close) differs from (impl.hovered_tab, impl.hovered_close):
      InvalidateRect(old tab rect), InvalidateRect(new tab rect)
      update impl.hovered_tab, impl.hovered_close
  → if TME_LEAVE not armed: arm it

WM_MOUSELEAVE
  → InvalidateRect(old hovered tab rect)
  → impl.hovered_tab = -1, impl.hovered_close = false
  → clear TME_LEAVE armed flag
```

### Close button click

Modeled on the existing middle-click close pattern:

```
WM_LBUTTONDOWN
  → TCM_HITTEST → tab index i
  → if i >= 0 && point is inside close_rect for i:
      SetCapture; impl.pressed_close = i; InvalidateRect(tab i)
      return 0  (do NOT forward — block tab-select)
  → else: fall through to DefSubclassProc (normal select)

WM_LBUTTONUP
  → if impl.pressed_close >= 0:
      hit = TCM_HITTEST + close_rect test at release point
      released_close = (hit == impl.pressed_close)
      impl.pressed_close = -1; ReleaseCapture
      if released_close: impl.fire_close_request(hit)
      return 0  (block forward)
  → else: fall through

WM_CAPTURECHANGED
  → impl.pressed_close = -1 (mirror existing MBUTTON cancel path)
```

### Hot-switch theme

```
MainWindow::window_proc(WM_SETTINGCHANGE)
  → if wParam == 0 && lParam points to L"ImmersiveColorSet":
      if (tabs_) tabs_->handle_theme_change();

TabManager::handle_theme_change()
  → re-detect dark mode
  → if changed: update cached palette, InvalidateRect(hwnd, nullptr, TRUE)
```

`WM_SETTINGCHANGE` is broadcast only to top-level windows; the tab
control is a child, so the delegate hop through `MainWindow` is
necessary.

## Error Handling

| Scenario | Handling |
|---|---|
| GDI resource leak | RAII `unique_hfont` / `unique_hbrush` etc. as `Impl` members. |
| DPI change (monitor move) | `MainWindow` already handles `WM_DPICHANGED` and calls `on_layout()`. Paint path reads `GetDpiForWindow(impl.hwnd)` each paint; fonts rebuilt lazily when `cached_dpi != current_dpi`. |
| `DwmGetWindowAttribute` unavailable (very old Win10) | Fall through to registry, then luminance heuristic. Worst case: light theme — same as today. |
| `DRAWITEMSTRUCT::itemID` out of range | Early-return guard (defensive; shouldn't occur but TCM has a race between item removal and a pending WM_DRAWITEM). |
| `on_close_request` deletes the tab under the cursor, then WM_LBUTTONUP lands on a different tab | `pressed_close` is reset before `fire_close_request` is called (same discipline as `pressed_tab`); no UAF possible. |
| `WM_SETTINGCHANGE` fires for unrelated settings | The `lParam == L"ImmersiveColorSet"` check filters noise. |

## Testing

- **TabList unit tests (9)** — unchanged, must remain green. Run via
  `ctest` in Release build.
- **TabManager owner-draw** — no unit tests. Mocking HDC + pixel-diff
  infra cost outweighs the benefit for a paint-only change, and this
  matches the project's existing convention ("data model tested,
  UI rendered manually").
- **Manual QA checklist** (run against `build/Release/litepdf.exe` after
  implementation):

  1. Open 1 tab — strip is visible, tab is clearly bounded, active
     highlight is obvious.
  2. Open 3 tabs — vertical 1 px separator between each pair of
     adjacent inactive tabs.
  3. Hover an inactive tab — background tints and × appears.
  4. Hover the × — it turns red; tab body hover state preserved.
  5. Click the × — the tab closes; active tab does not change if the
     closed tab was inactive.
  6. Click an inactive tab body (not the ×) — it becomes active; no
     close.
  7. Middle-click any tab — closes it (regression check).
  8. Ctrl+W — closes active tab (regression check).
  9. Ctrl+Tab / Ctrl+Shift+Tab — cycles active tab (regression check).
  10. Open a PDF with a very long filename — label truncates with `…`.
  11. Toggle Windows Settings → Colors → Dark mode while litepdf is
      running — the tab strip repaints with dark palette without
      restart.
  12. Drag the window to a HiDPI monitor — tab height, font, and ×
      size scale correctly.

- **Build / CI** — `cmake --build build --config Release` + full
  `ctest` green.

## Out-of-scope / future

- Tab drag-reorder.
- Active tab drop shadow or animated color transitions.
- Per-tab unsaved-changes indicator (dot in title).
- New tab (+) button inline in the strip.

## Open questions

None at design-approval time. All three user decision points were
resolved during brainstorming:
- Approach: B.
- Visual spec (32 px, hover-only ×, dark-mode aware): accepted as-is.
- Hot-switch dark mode via `WM_SETTINGCHANGE`: accepted (+~12 LOC over
  restart-required baseline).
