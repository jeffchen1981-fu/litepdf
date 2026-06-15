# LitePDF Pre-Release Smoke Checklist

Run on a **Release** build before every `vX.Y.Z` tag. Record pass/fail + notes in
the release PR description (this file is the reusable template — leave the boxes
unchecked in git). Any FAIL blocks the tag: fix and re-run.

Build: `cmake --build build --target litepdf --config Release` → `build\Release\litepdf.exe`.

## Core viewer (design §6)
- [ ] Double-click a `.pdf` in Explorer opens it correctly
- [ ] Drag-and-drop a PDF onto the window opens it
- [ ] Open multiple tabs, switch, close the middle one; no leak (Task Manager memory stable)
- [ ] Cross-tab search (Ctrl+Shift+F) finds hits and click-to-navigate works
- [ ] Zoom (Ctrl +/-), scroll, PgUp/PgDn keyboard nav
- [ ] Cold-start to first page < 1 s on an HDD-class machine
- [ ] Open encrypted.pdf (password `test`), epub, cbz — all render

## Phase 12 hardening
- [ ] session.json appears under %LOCALAPPDATA%\LitePDF after opening a doc + 2 s
- [ ] Change ONLY zoom (no page change), wait 2 s => new zoom persisted in session.json
- [ ] Force-kill with tabs open (mixed sizes), relaunch => restore prompt appears
- [ ] Restore Yes => same tab ORDER, pages, zooms, window placement, active tab
- [ ] Restore with an encrypted tab => re-prompted for password, then restored
- [ ] Restore with one file deleted => missing tab skipped silently (no dialog storm)
- [ ] Normal File→Exit, relaunch => NO restore prompt
- [ ] Restart Windows with tabs open, relaunch => NO restore prompt
- [ ] Launch a 2nd instance (open another PDF) while 1st shows the restore prompt => no deadlock / no double-restore
- [ ] Launch a 2nd instance forwarding a NEW PDF DURING the restore chain => it opens as its own tab with its own page/zoom; restore still completes correctly
- [ ] Close the window while the restore chain is still opening tabs; relaunch => full session is re-offered (no partial-clobber)
- [ ] Forced crash (debug trigger) writes a .dmp under crashes\ that opens with a stack
- [ ] crashes\ keeps only the newest 5 dumps after repeated crashes
- [ ] Uninstall prompts to keep/delete %LOCALAPPDATA%\LitePDF; "No" keeps the dir
