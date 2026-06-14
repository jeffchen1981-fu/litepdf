# LitePDF Manual Smoke Runbook (interactive items)

Companion to [`SMOKE-CHECKLIST.md`](SMOKE-CHECKLIST.md). The checklist's
session/restore/crash items that can be scripted are exercised by an automated
pass; **this runbook covers the items that require a human** (Explorer
association, drag-and-drop, visual rendering/zoom, true reboot, multi-instance
timing, debugger stack, and install/uninstall). Run on a **Release** build
before every `vX.Y.Z` tag; record pass/fail in the release PR. Any FAIL blocks
the tag.

## Setup (once)

| Item | Value |
|---|---|
| Executable | `build\Release\litepdf.exe` |
| Data dir | `%LOCALAPPDATA%\LitePDF\` (`session.json`, `running.lock`, `crashes\`) |
| Fixtures | `tests\fixtures\`: `simple.pdf`, `large.pdf`, `search.pdf`, `bookmarks.pdf`, `encrypted.pdf` (password `test`), `sample.epub`, `sample.cbz` |

Helper commands (PowerShell):

```powershell
# Reset to a "clean" state (no restore prompt on next launch)
Remove-Item "$env:LOCALAPPDATA\LitePDF\session.json","$env:LOCALAPPDATA\LitePDF\running.lock" -EA SilentlyContinue
# Inspect the persisted session
Get-Content "$env:LOCALAPPDATA\LitePDF\session.json"
# Check for a leftover run marker (present => previous exit was abnormal)
Test-Path "$env:LOCALAPPDATA\LitePDF\running.lock"
```

- **Abnormal exit** = Task Manager *End Task* or `Stop-Process -Name litepdf -Force` (leaves `running.lock`).
- **Normal exit** = window *File→Exit* or close button (clears `running.lock`).

## A. Core viewer (single launch)

| # | Action | Expected (PASS) |
|---|---|---|
| A1 | Double-click `tests\fixtures\simple.pdf` in Explorer (requires the .pdf association, e.g. after an install) | Opens correctly |
| A2 | Drag-and-drop a PDF from Explorer onto the window | Opens as a new tab |
| A3 | Open 3-4 tabs, switch, close the middle one; watch Task Manager memory | Switch/close correct; memory stable (no leak) |
| A4 | `Ctrl+Shift+F` cross-tab search a term (use `search.pdf`) | Hit list appears; clicking a hit navigates to the tab/page |
| A5 | `Ctrl+=` / `Ctrl+-` zoom, mouse scroll, `PgUp`/`PgDn`, `Ctrl+0` reset | All respond correctly and smoothly |
| A6 | `encrypted.pdf` (password `test`); then `sample.epub`, `sample.cbz` | All three render (not blank/garbled) |

## B. Restore visual correctness (Phase 12 headline)

1. Reset to clean.
2. Open 3 tabs: `simple.pdf`, `large.pdf`, `encrypted.pdf` (password `test`).
3. Set a DISTINCT page + zoom per tab (e.g. tab1 p3 fit-width, tab2 p10 200%, tab3 p2 fit-page).
4. Make **tab 2 active**; move/resize the window to a distinct spot. Wait ~2 s.
5. **Abnormal exit** (Task Manager End Task).
6. Relaunch → expect prompt: "LitePDF closed unexpectedly last time. Restore the 3 previously open tab(s)?"

| Check | Expected (PASS) |
|---|---|
| Order | 3 tabs reopen in the ORIGINAL order |
| State | Each tab's page + zoom match pre-close |
| Encrypted | The encrypted tab re-prompts for its password; after `test` it restores its page/zoom |
| Window | Window position/size restored |
| Active | Tab 2 is the active tab |

## C. No-prompt cases

| # | Action | Expected (PASS) |
|---|---|---|
| C1 | Open tabs → **File→Exit** (normal close) → relaunch | NO restore prompt (`running.lock` cleared) |
| C2 | Open tabs → **restart Windows** (Power → Restart) → relaunch after sign-in | NO restore prompt (WM_ENDSESSION cleared the marker) — requires a real reboot |

## D. Concurrency / races (timing-sensitive; logic is code-reviewed, confirm live)

> Produce a "restoring" state first: do B steps 1-5 to get an abnormal exit with tabs.

| # | Action | Expected (PASS) |
|---|---|---|
| D1 | Relaunch; **while the restore prompt is showing**, start a 2nd LitePDF opening another PDF | No deadlock; no double-restore; the 1st prompt still responds |
| D2 | Accept restore; **while the chain is still opening tabs**, open a NEW PDF (2nd instance or drag-drop) | The new PDF opens as its own tab with its own page/zoom; the restore chain still completes correctly (no misattribution) |
| D3 | Accept restore, then **close the window while tabs are still loading** → relaunch | Prompt appears again and re-offers the FULL session (no partial clobber) |

## E. Crash dump opens with a stack

> The dump structure (`MDMP` magic) and retention (newest 5) are auto-verified.
> This item needs a debugger. The shipped exe has no crash trigger, so a sample
> dump must be produced via a temporary `--crashtest` build, or captured from a
> real crash. Symbols require `litepdf.pdb` next to the exe.

| # | Action | Expected (PASS) |
|---|---|---|
| E1 | Open a `%LOCALAPPDATA%\LitePDF\crashes\*.dmp` in Visual Studio or WinDbg | Loads and shows a resolvable call stack (with `litepdf.pdb`) |

## F. Uninstall keep/delete prompt

| # | Action | Expected (PASS) |
|---|---|---|
| F1 | Install via the built installer → launch once (creates `%LOCALAPPDATA%\LitePDF`) → uninstall | Prompts "要一併刪除 LitePDF 的設定、工作階段與當機記錄嗎?" + shows the path; "No" keeps the dir, "Yes" deletes it |
