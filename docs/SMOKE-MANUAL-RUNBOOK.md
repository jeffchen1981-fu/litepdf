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

## Coverage status — automated pass 2026-06-15 (fixed build, commit `88a1150`)

A scripted GUI pass (PowerShell driving `PostMessage(WM_COMMAND)` + GDI screen
capture — no computer-use; see the `reference_litepdf_scripted_gui_smoke` memo)
exercised the items below on `build\Release\litepdf.exe`. Items marked AUTO-PASS
need no re-run for routine releases. The human items in sections A–F still gate
the tag.

| Item | Result |
|---|---|
| A3 multi-tab switch + close the active middle tab (#33) | AUTO-PASS — canvas rebinds to the neighbour immediately |
| A4 cross-tab search: panel populates + yellow highlights + row-activate → orange current hit | AUTO-PASS |
| A6 `sample.cbz` shows real content (#32) + `encrypted.pdf` renders | AUTO-PASS (epub render CLI-verified) |
| B restore: original ORDER + correct ACTIVE tab | AUTO-PASS |
| B restore: encrypted tab re-prompts for password, then restores | AUTO-PASS |
| B restore: canvas shows the ACTIVE tab's page, not the last-restored tab (#35) | FIXED in `88a1150` + AUTO-PASS (previously showed the wrong tab until a manual switch) |
| C1 normal close → no restore prompt | AUTO-PASS |
| C2 OS shutdown/logoff clears the marker → no prompt | PASS — sim-verified, then confirmed by a REAL reboot 2026-06-15: with the app open, after restart `running.lock` was absent and launch showed no restore prompt (session.json timestamped at shutdown proves on_clean_exit ran via WM_ENDSESSION, not an OS force-kill) |
| D1 2nd instance opening a PDF while the restore prompt is up | AUTO-PASS — no deadlock, no double-restore; the forwarded file gets its own tab and the restore still completes |
| A1 double-click file association → opens in LitePDF | PASS (computer-use): double-clicking `search.pdf` opened it as a new tab (forwarded into the running instance) |
| A2 drag-and-drop a PDF onto the window | PASS (computer-use): dragging `bookmarks.pdf` from Explorer opened it as a new active tab |
| A5 zoom / scroll / keyboard nav | PASS: Zoom Out visibly shrinks; Zoom In is correctly a no-op only when fit-width already exceeds the 4.0 max preset (large/maximized windows — minor UX quirk, not a bug). #34 accelerator gaps stand |
| D2 2nd instance opens a PDF *during* the restore chain → own tab + chain still completes | PASS: froze the chain at an encrypted tab's password prompt, injected `bookmarks.pdf` via a 2nd instance, then resumed — final set = 3 restored + 1 injected, saved-active correct |
| D3 close window *mid-restore-chain* → full session re-offered | PASS (deterministic via a temporary env-gated restore delay, reverted): a mid-chain WM_CLOSE left `session.json` at the full count + kept the marker; next launch re-offered the full set; clean exit, no crash dump |

Still REQUIRES a human (detailed below): **E1** debugger stack (needs WinDbg/VS
+ symbol setup), **F1** install/uninstall (destructive; build a fresh installer
first).

How D2/D3 were made testable (the chain is otherwise sub-second): **D2** — put
`encrypted.pdf` in the restore set; its password prompt freezes the chain,
giving an unbounded window to inject a 2nd-instance open. **D3** — per-tab opens
are too fast to slow with volume alone (MuPDF opens lazily), so a temporary
env-gated `Sleep` in `restore_open_next` (`LITEPDF_RESTORE_DELAY_MS`) made the
mid-chain close deterministic; revert it before committing.

## A. Core viewer (single launch)

| # | Action | Expected (PASS) |
|---|---|---|
| A1 | Double-click `tests\fixtures\simple.pdf` in Explorer (requires the .pdf association, e.g. after an install) | Opens correctly |
| A2 | Drag-and-drop a PDF from Explorer onto the window | Opens as a new tab |
| A3 | Open 3-4 tabs, switch, close the middle one; watch Task Manager memory | Switch/close correct; memory stable (no leak) |
| A4 | `Ctrl+Shift+F` cross-tab search a term (use `search.pdf`) | Hit list appears; clicking a hit navigates to the tab/page |
| A5 | Zoom (View menu + `Ctrl`+mouse-wheel), mouse scroll, arrow-key pan, page nav | Smooth + correct. KNOWN (#34, not a regression): the `Ctrl+=`/`Ctrl+-`/`Ctrl+0` and `PgUp`/`PgDn` *accelerators* are dead; use the View menu / mouse-wheel-zoom / arrow keys instead |
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

The dump structure (`MDMP` magic) and retention (newest 5) are auto-verified.
Resolving a *stack* needs a debugger + symbols. A ready-to-open sample dump from
a RelWithDebInfo build is already on disk, with matching symbols:

| Artifact | Path |
|---|---|
| Sample dump | `%LOCALAPPDATA%\LitePDF\crashes\litepdf-389208-158592406.dmp` |
| Symbols | `build\RelWithDebInfo\litepdf.pdb` (+ `litepdf.exe`) |

**Structural pre-check (no debugger needed, done 2026-06-15):** parsing the
sample dump confirms it is a valid `MDMP` (x64, 14 streams) with 6 threads and a
real captured stack (thread 0 = 16,248 bytes), an Exception stream (code
`0xC0000005`, the crash-test access violation), 33 modules, and a `litepdf.exe`
PDB70 CodeView record pointing to `litepdf.pdb` (GUID
`022C9B6C-1946-47ED-938E-70033107A197`, age 2) — and that pdb is present on disk.
So the dump is complete and symbolizable; only the function-name resolution below
needs an actual debugger (none is currently installed — `winget install
Microsoft.WinDbg` is the lightest).

| # | Action | Expected (PASS) |
|---|---|---|
| E1 | Open the dump in **Visual Studio** (File → Open → File → the `.dmp`, then "Debug with Native Only") OR **WinDbg** | Loads and the call stack resolves to `litepdf!...` frames (not just `litepdf+0x...` module+offset) |

WinDbg / `cdb` recipe (install "Debugging Tools for Windows" from the Windows SDK
first — neither is currently on PATH):

```
windbg -z "%LOCALAPPDATA%\LitePDF\crashes\litepdf-389208-158592406.dmp"
# at the prompt (replace <repo> with this checkout's path):
.sympath+ <repo>\build\RelWithDebInfo
.reload /f
k
```

> **Task 12 release-config gap (carry into `/ship`):** the **Release** config
> emits NO `litepdf.pdb` (default `/O2`, no `/Zi`/`/DEBUG`), so dumps from the
> *shipped* exe resolve to module+offset only. Build Release as RelWithDebInfo
> (or add `/Zi`+`/DEBUG`) and archive `litepdf.pdb` in `release.yml`.

## F. Uninstall keep/delete prompt

Requires a real install + uninstall, so it modifies installed software — run it
yourself; do not script it. The keep/delete prompt wording is the thing under
test (Task 10), so build a FRESH installer from the current source first.

| Artifact | Path / command |
|---|---|
| Build installer | Compile `installer\litepdf.iss` with Inno Setup (`ISCC.exe installer\litepdf.iss`) → `installer\Output\*.exe` |
| Installed uninstaller (existing copy) | `%LOCALAPPDATA%\Programs\LitePDF\unins000.exe` |

| # | Action | Expected (PASS) |
|---|---|---|
| F1a | Install the freshly-built installer; launch once, open a file, close (creates `%LOCALAPPDATA%\LitePDF\` with `session.json`) | Data dir present |
| F1b | Run the uninstaller (`unins000.exe`, or Settings → Apps → LitePDF → Uninstall) | Prompts "要一併刪除 LitePDF 的設定、工作階段與當機記錄嗎?" and shows the `%LOCALAPPDATA%\LitePDF` path |
| F1c | Click **否 (No)** | Uninstall completes; `%LOCALAPPDATA%\LitePDF\` is KEPT |
| F1d | Re-install → re-create data → uninstall → click **是 (Yes)** | `%LOCALAPPDATA%\LitePDF\` is DELETED |
