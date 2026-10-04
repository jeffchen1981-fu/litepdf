# Ctrl+G: keyboard access to the go-to-page box — design

**Date:** 2026-10-04
**Baseline:** `main` @ `7c1bde1`
**Issue:** #48 (page box is mouse-only: no Ctrl+G, and Tab is inert)
**Ships as:** one PR.

## 1. Motivation

v1.3.0 shipped the status bar's go-to-page box with no keyboard route into it.
Clicking is the only way to focus it, which is an accessibility gap rather than a
missing convenience.

Tab does not reach it either. The box carries `WS_TABSTOP` and its subclass
returns `DLGC_WANTALLKEYS` from `WM_GETDLGCODE`, but `IsDialogMessage` appears
nowhere in `src/ui` (its only caller is `src/printing/PrintProgressDlg.cpp`, which
runs its own modal loop), so no dialog manager ever runs tab navigation.

### 1.1 Premises in the issue that no longer hold

- **Next free id.** The issue says 40064. #59 took it (`IDM_VIEW_STATUS_BAR`,
  `resources/MainMenu.rc.h:70`); the next free id is **40065**.
- **Landing with #47.** #47 closed on 2026-09-25. Nothing to coordinate.
- **Test count.** The issue says 303. Do not hard-code a count; `ctest -N` is the
  reference at implementation time.

## 2. Approach

**An accelerator plus a View-menu item. Tab stays inert.**

Rejected: making `WS_TABSTOP` live by routing the main message pump through
`IsDialogMessage`. There is a single pump (`MainWindow.cpp:2531-2536`), so that
changes keyboard handling for every control in the window, and it interacts with
the bare-accelerator ESC routing. The cost is out of proportion to the gap.

## 3. Command, key and menu

- `resources/MainMenu.rc.h`:
  `#define IDM_VIEW_GOTO_PAGE   40065   // Ctrl+G`.
  The "Next free ID" comment moves to **40066**, reserving 40066-40070. Every
  block that consumes ids rewrites that marker; this one does too.
- Accelerator table (`MainWindow.cpp`, the `accels[]` array):
  `{ FCONTROL | FVIRTKEY, 'G', IDM_VIEW_GOTO_PAGE }`. No existing entry uses
  `'G'`, and the standard EDIT control has no Ctrl+G behaviour of its own, so the
  key is not taken from the find box or the page box.
- View menu (`resources/litepdf.rc.in`), a new group below the zoom group:

  ```
  MENUITEM SEPARATOR
  MENUITEM "&Go to Page...\tCtrl+G", IDM_VIEW_GOTO_PAGE
  ```

  The ellipsis reads as "needs more input", which it does. Mnemonic `G` is unused
  in the View popup (O, T, I, T, B, I, O, R today).

## 4. Behaviour

| State when Ctrl+G is pressed | Result |
|---|---|
| Document with pages, bar visible | Focus moves to the page box; its whole text is selected. |
| Document with pages, bar **hidden** | The bar is shown (and stays shown), then focus + select as above. |
| No document, or a 0-page document | Nothing. The menu item is grayed. |
| Page box already focused | Whole text re-selected. |
| A mouse capture is held (drag-select, pan) | Nothing. `TranslateAcceleratorW` sends no `WM_COMMAND` under a capture; the key is dead until the button is released. Expected. |

The round trip back is already wired and does not change: Enter commits (the
subclass's `VK_RETURN` → `commit()` → `on_focus_out` → canvas), and ESC is claimed
by the `IDM_FIND_CLOSE` arm, which checks `page_box_has_focus()` first and
returns focus to the canvas. `WM_KILLFOCUS` reverts uncommitted text.

Ctrl+G from the find bar or the results panel moves focus to the box and leaves
both visible, as Ctrl+F does in the other direction. Neither control handles
`WM_KILLFOCUS`.

After Alt+Tab away and back, focus lands on the canvas, not the box
(`MainWindow`'s `WM_SETFOCUS` forwards to the canvas). Pre-existing, expected.

### 4.1 Why a hidden bar is shown and left on

- **A no-op** would be the keyboard route failing silently whenever the bar is
  hidden, which is the issue's own complaint.
- **A transient show** (re-hide when the box loses focus) has no reliable hook:
  the box's `WM_KILLFOCUS` only reverts text and does not call `on_focus_out`, so
  a click-away, Alt+Tab or pane toggle would leave the bar shown with nothing to
  hide it. Only Enter and ESC go through a path that could.
- **Leaving it on costs nothing durable.** Visibility is not persisted (#59
  decision), so it resets next launch.

Consequences the reader sees: the View > Status Bar checkmark turns on (it is
read live in the View arm of `WM_INITMENUPOPUP`). There is no accelerator for
Status Bar; the keyboard route to hide it again is Alt+V, B.

## 5. Implementation

### 5.1 `ui::StatusBar`

Two new members:

- `bool page_box_enabled() const` — `impl_->page_count > 0`. Read from the
  state `set_page` / `set_empty` maintain, **not** `IsWindowEnabled`, so it
  cannot drift from `set_empty`'s intent. Independent of visibility.
- `void focus_page_box()` — `SetFocus(edit)` then `SendMessageW(edit, EM_SETSEL,
  0, -1)`. That order: selecting before focusing invites the EDIT's own
  focus handling to move the selection.

Visibility stays `MainWindow`'s job, because showing the bar requires a re-layout.

### 5.2 Keep the selection across a focused overwrite (`StatusBar::set_page`)

After Ctrl+G the box is focused and its text equals `last_written`, which is
exactly the state in which `set_page` rewrites it
(`detail::should_overwrite_page_box` returns true when focused and untouched).
`write_box` uses `SetWindowTextW`, and an EDIT drops its selection to (0,0) on
`WM_SETTEXT` even for byte-identical text. So Ctrl+G, then Ctrl+Tab (or a wheel
notch over the canvas, which is reachable while the box holds focus), then typing
`5` on page 1 yields `51`.

Fix: in the focused-overwrite branch of `set_page`, after `force_revert()`,
re-issue `EM_SETSEL 0,-1`. The text was untouched by the reader, so "the next
keystroke replaces it" is the correct state.

This also changes the click entry path: a box entered by mouse that sees a page
change now ends up fully selected rather than with the caret at 0. That is the
better behaviour for an untouched value and is accepted deliberately.

### 5.3 `MainWindow`

- **`WM_COMMAND`, `IDM_VIEW_GOTO_PAGE`:**
  1. `if (!status_bar_ || !status_bar_->page_box_enabled()) return 0;` —
     **required, not redundant.** A mouse-opened View popup keeps dispatching
     messages, so a second-instance open or a last-tab close can change the state
     between the menu's enable check and the click.
  2. If `!status_bar_->visible()`, call a new
     `MainWindow::set_status_bar_visible(bool)`. That is today's body of
     `on_toggle_status_bar()` (`set_visible`, `on_layout()`, `kick_render` of the
     active page) taking the target state; `on_toggle_status_bar()` becomes
     `set_status_bar_visible(!status_bar_->visible())`. Do not call the toggle
     bare: it inverts state, and the call would be correct only because of the
     guard in front of it.
  3. `status_bar_->focus_page_box()`.
- **`WM_INITMENUPOPUP`, View arm** (the `popup_owns(popup, IDM_VIEW_INVERT)`
  block, before its `return 0`): `EnableMenuItem(popup, IDM_VIEW_GOTO_PAGE, ...)`
  with `status_bar_ && status_bar_->page_box_enabled()`. `TranslateAcceleratorW`
  sends `WM_INITMENUPOPUP` before acting on an accelerator, and a GRAYED item
  swallows the key with no `WM_COMMAND`. That is the intended outcome for no
  document, which is why this condition and step 1's must be the same
  expression.

The show-then-focus order is safe: nothing in `on_layout()`, `kick_render()` or
the render-completion path calls `SetFocus` or writes the box except through
`set_page`, which §5.2 covers.

## 6. Testing

### 6.1 Unit tests

None. `page_box_enabled()` is one comparison and everything else is window
code; there is no pure logic to extract into `ui/detail`. The ctest count does
not move. `ctest --test-dir build -C Release` must stay all-pass (Release build;
Debug fails to link the tests).

### 6.2 Scripted GUI check

An untracked PowerShell driver under `build/gui-check/`, following the #57/#59
driver (`docs/superpowers/plans/2026-10-03-status-bar-zoom-readout-hide-57-59.md`).
Oracles a separate process can actually read:

- **Focus:** `GetGUIThreadInfo(tid).hwndFocus`. Not `GetFocus()`, which returns
  NULL for a window on another thread's queue.
- **Selection:** `SendMessageW(box, EM_GETSEL)`; full range is
  `start == 0 && end == text length`.
- **Gray state:** send `WM_INITMENUPOPUP` for the View popup, then
  `GetMenuState(viewPopup, 40065, MF_BYCOMMAND)`.
- **Bar visibility:** `IsWindowVisible` on the status bar.

Input: **one** real `SendInput` Ctrl+G to prove the accelerator binding; every
other case posts `WM_COMMAND 40065`.

Cases:

1. Document open, bar visible, Ctrl+G via `SendInput` → focus is the box, full
   selection.
2. Ctrl+G again while focused → still full selection.
3. **§5.2 regression:** two documents open; Ctrl+G; post `WM_COMMAND IDM_TAB_NEXT`
   → box still focused, full selection. Negative control on the pre-change
   binary, which has no Ctrl+G: click the box with `SendInput`, send
   `EM_SETSEL 0,-1` from the driver, confirm the full range, post
   `IDM_TAB_NEXT`, and record `start == end == 0`. That proves the case can
   fail.
4. Bar hidden (post `IDM_VIEW_STATUS_BAR`), Ctrl+G → bar visible, focus is the
   box, full selection.
5. No document → `WM_COMMAND 40065` leaves focus where it was; View popup reports
   40065 GRAYED. Negative control: with a document open it reports enabled.
6. ESC after Ctrl+G → focus returns to the canvas, box shows the current page.

## 7. Out of scope

- Tab navigation into or out of the box.
- A Go-to-Page dialog.
- An accelerator for View > Status Bar.
