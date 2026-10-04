# Ctrl+G Go-to-Page Accelerator (#48) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ctrl+G (and View → Go to Page) puts the keyboard in the status bar's page box with its text selected, showing the bar first if it is hidden.

**Architecture:** `ui::StatusBar` gains `page_box_enabled()` and `focus_page_box()`, and re-selects the box's text after `set_page` rewrites a focused, untouched box. `MainWindow` gains `IDM_VIEW_GOTO_PAGE 40065`, a Ctrl+G accelerator, a View-menu item whose enable state uses the same expression as the command's guard, and a `set_status_bar_visible(bool)` helper that the existing toggle now calls. Tab navigation stays inert by design.

**Tech Stack:** C++20, Win32 (`EDIT`, `msctls_statusbar32`, accelerator table, menu resources), PowerShell 5.1 GUI driver.

**Spec:** `docs/superpowers/specs/2026-10-04-goto-page-accelerator-design.md`

## Global Constraints

- Branch: `feat/48-goto-page-accelerator`. Do not commit to `main`.
- Build **Release**, never Debug (Debug fails with `LNK2038`).
- `cmake` / `ctest` on PATH are the wrong ones. Always use
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` and `ctest.exe` in the same directory.
- Run `ctest` from **PowerShell**, from the repo root `C:\Users\User\projects\litepdf` (from Git Bash two `version_script_selftest` cases fail spuriously).
- **Line numbers in this plan are as of commit `01e80f9`**, before any task ran. Locate every edit by the quoted text or the named function; treat a number as a hint.
- No task adds or removes a unit test. Task 1 records the `ctest` count; every later `ctest` run must show that same count, all passing.
- Command ids: `IDM_VIEW_STATUS_BAR 40064`, `IDM_VIEW_GOTO_PAGE 40065` (new), `IDM_TAB_NEXT 40031`, `IDM_TAB_CLOSE 40030`, `IDM_FIND_CLOSE 40047`.
- All code, comments and commit messages in English. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not bump `VERSION`. Do not persist the status bar's visibility anywhere.
- `*.ps1` must run on Windows PowerShell 5.1: no `?.`, `??`, ternary, `&&`, `||`.
- Do not run `ctest` and a GUI driver at the same time. Close every running LitePDF (including an installed copy) before a GUI run, and leave the machine alone while it runs.
- Never change an OS setting (theme, display scale, High Contrast).
- `build/` is git-ignored: everything under `build/gui-check/` is untracked and never committed.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/ui/StatusBar.hpp` / `.cpp` | modify | `page_box_enabled`, `focus_page_box`, re-select in `set_page` |
| `src/ui/MainWindow.hpp` / `.cpp` | modify | `set_status_bar_visible`, `on_goto_page`, `WM_COMMAND` arm, View-menu enable, accelerator |
| `resources/MainMenu.rc.h` | modify | `IDM_VIEW_GOTO_PAGE 40065`, id-reservation comments |
| `resources/litepdf.rc.in` | modify | View → Go to Page item |
| `README.md`, `CHANGELOG.md` | modify | user-facing notes |
| `build/gui-check/goto-page.ps1` | create, untracked | GUI driver |
| `build/gui-check/make-probes-48.sh` | create, untracked | builds `normal-probe.exe` and the `noresel-probe.exe` mutant |

---

## Task 1: GUI driver, and a red run against the current build

**Files:**
- Create (untracked): `build/gui-check/goto-page.ps1`

**Interfaces:**
- Consumes: the current branch, whose `src/` is identical to `main` @ `7c1bde1`.
- Produces: `build/gui-check/goto-page.ps1 -Exe <path> -Mode normal|noresel`, which prints one `PASS`/`FAIL` line per check and a summary `"<mode>: 16 checks, <n> failed"`, exiting 1 if any check failed. Tasks 2 and 3 run it by path. The ctest count recorded in Step 1.

- [ ] **Step 1: Record the test baseline**

From PowerShell:

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
```

Expected: the build succeeds and `100% tests passed, 0 tests failed out of N`. Write N down; it is the count every later task must reproduce. (It was 404 registered at `7c1bde1`; trust what you see, not this number.)

- [ ] **Step 2: Write the driver**

Save as `build/gui-check/goto-page.ps1` (create `build/gui-check` if it does not exist):

```powershell
#Requires -Version 5.1
# GUI checks for #48 (Ctrl+G focuses the status bar's page box).
#   -Mode normal  : the real build.
#   -Mode noresel : mutant without set_page's re-select (check 08 inverts).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][ValidateSet('normal', 'noresel')][string]$Mode
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class GpW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
    public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion U; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendString(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetMenuStringW(IntPtr m, uint id, StringBuilder s, int n, uint f);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO g);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] a, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static List<IntPtr> Kids(IntPtr p) {
    var r = new List<IntPtr>();
    EnumChildWindows(p, (h, l) => { r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
  public static string Cls(IntPtr h) { var s = new StringBuilder(128); GetClassNameW(h, s, 128); return s.ToString(); }
  public static string Text(IntPtr h) { var s = new StringBuilder(256); SendText(h, 0x000D, (IntPtr)256, s); return s.ToString(); }
  public static string Title(IntPtr h) { var s = new StringBuilder(512); GetWindowTextW(h, s, 512); return s.ToString(); }
  public static string MenuText(IntPtr m, uint id) { var s = new StringBuilder(128); GetMenuStringW(m, id, s, 128, 0); return s.ToString(); }
  public static IntPtr Focus(IntPtr anyWindowOfThread) {
    uint pid; uint tid = GetWindowThreadProcessId(anyWindowOfThread, out pid);
    var g = new GUITHREADINFO(); g.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
    return GetGUIThreadInfo(tid, ref g) ? g.hwndFocus : IntPtr.Zero;
  }
  public static int InputSize() { return Marshal.SizeOf(typeof(INPUT)); }
  // Built in C#, not PowerShell: assigning a nested value-type field from
  // PowerShell mutates a copy and silently sends vk=0.
  static INPUT Key(ushort vk, uint flags) { var i = new INPUT(); i.type = 1; i.U.ki.wVk = vk; i.U.ki.dwFlags = flags; return i; }
  public static uint CtrlG() {
    var a = new INPUT[] { Key(0x11, 0), Key(0x47, 0), Key(0x47, 2), Key(0x11, 2) };   // KEYEVENTF_KEYUP = 2
    return SendInput((uint)a.Length, a, InputSize());
  }
}
'@
[void][GpW]::SetProcessDPIAware()

$repo   = 'C:\Users\User\projects\litepdf'
$simple = Join-Path $repo 'tests\fixtures\simple.pdf'
$spread = Join-Path $repo 'tests\fixtures\spread-unequal.pdf'
foreach ($f in $Exe, $simple, $spread) { if (-not (Test-Path $f)) { throw "missing: $f" } }
if (Get-Process | Where-Object { $_.Name -eq 'litepdf' -or $_.Name -like '*-probe' }) {
    throw 'A LitePDF instance is already running. Close it first.'
}

$script:Results = New-Object System.Collections.ArrayList
function Add-Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $tag = 'FAIL'; if ($Ok) { $tag = 'PASS' }
    [void]$script:Results.Add("$tag  $Name  [$Detail]")
    Write-Host "$tag  $Name  [$Detail]"
}
function Wait-Until([scriptblock]$Cond, [int]$Ms = 4000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $Ms) { if (& $Cond) { return $true }; Start-Sleep -Milliseconds 50 }
    return [bool](& $Cond)
}
function Send-Cmd([int]$Id) { [void][GpW]::PostMessageW($script:Main, 0x0111, [IntPtr]$Id, [IntPtr]::Zero) }
function Get-FocusNow { return [GpW]::Focus($script:Main) }
function Get-Sel {
    $v = [int64][GpW]::SendMessageW($script:Edit, 0x00B0, [IntPtr]::Zero, [IntPtr]::Zero)   # EM_GETSEL
    $len = [int64][GpW]::SendMessageW($script:Edit, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero) # WM_GETTEXTLENGTH
    return @{ Start = ($v -band 0xFFFF); End = (($v -shr 16) -band 0xFFFF); Len = $len }
}
function Test-FullSel { $s = Get-Sel; return (($s.Len -gt 0) -and ($s.Start -eq 0) -and ($s.End -eq $s.Len)) }
function Show-Sel { $s = Get-Sel; return ('sel={0},{1} len={2} focus=0x{3:X} box=0x{4:X}' -f $s.Start, $s.End, $s.Len, [int64](Get-FocusNow), [int64]$script:Edit) }
function Test-BoxReady { return (((Get-FocusNow) -eq $script:Edit) -and (Test-FullSel)) }
# Every menu read sends WM_INITMENUPOPUP first: the View arm writes the enable
# and check states only while handling it, and a posted WM_COMMAND never does.
function Get-ViewState([int]$Id) {
    $view = [GpW]::GetSubMenu([GpW]::GetMenu($script:Main), 2)   # File, Edit, View
    [void][GpW]::SendMessageW($script:Main, 0x0117, $view, [IntPtr]2)   # WM_INITMENUPOPUP
    return [GpW]::GetMenuState($view, [uint32]$Id, 0)                    # MF_BYCOMMAND
}
function Test-Exists([uint32]$State) { return ($State -ne [uint32]::MaxValue) }
function Test-Checked([int]$Id) { $s = Get-ViewState $Id; return ((Test-Exists $s) -and (($s -band 0x8) -ne 0)) }
function Find-Parts {
    $script:Bar = [IntPtr]::Zero; $script:Canvas = [IntPtr]::Zero; $script:Edit = [IntPtr]::Zero
    foreach ($h in [GpW]::Kids($script:Main)) {
        $c = [GpW]::Cls($h)
        if ($c -eq 'msctls_statusbar32') { $script:Bar = $h }
        if ($c -eq 'LitePDFPdfCanvas')   { $script:Canvas = $h }
    }
    if ([int64]$script:Bar -eq 0 -or [int64]$script:Canvas -eq 0) { throw 'status bar or canvas not found' }
    foreach ($h in [GpW]::Kids($script:Bar)) {
        if ([GpW]::GetParent($h) -eq $script:Bar -and [GpW]::Cls($h) -eq 'Edit') { $script:Edit = $h }
    }
    if ([int64]$script:Edit -eq 0) { throw 'page box not found' }
}

$env:LITEPDF_NO_RESTORE = '1'
$session = Join-Path $env:LOCALAPPDATA 'LitePDF\session.json'
$backup  = Join-Path $env:TEMP 'litepdf-session-guicheck.bak'
$hadSession = Test-Path $session
if ($hadSession) { Copy-Item $session $backup -Force }
# The app deletes running.lock on a clean exit. If the user had one (their last
# real session crashed), put it back so they still get their restore prompt.
$marker = Join-Path $env:LOCALAPPDATA 'LitePDF\running.lock'
$hadMarker = Test-Path $marker

$proc = Start-Process $Exe -ArgumentList ('"{0}"' -f $simple) -PassThru
try {
    $script:Main = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and [int64]$script:Main -eq 0; $i++) { Start-Sleep -Milliseconds 200; $proc.Refresh(); $script:Main = $proc.MainWindowHandle }
    if ([int64]$script:Main -eq 0) { throw 'no main window' }
    [void][GpW]::ShowWindow($script:Main, 1)   # SW_SHOWNORMAL
    Find-Parts
    [void](Wait-Until { [GpW]::Text($script:Edit) -eq '1' } 5000)

    # --- case 1: the real accelerator ---------------------------------------
    # SendInput goes to the FOREGROUND window. A freshly started LitePDF is
    # normally foreground; if it is not, 01 is inconclusive, not a product bug.
    $fg = ([GpW]::GetForegroundWindow() -eq $script:Main)
    Add-Check '00 CONTROL LitePDF is foreground and INPUT is 40 bytes' ($fg -and ([GpW]::InputSize() -eq 40)) ("fg=$fg size=" + [GpW]::InputSize())
    $sent = [GpW]::CtrlG()
    [void](Wait-Until { Test-BoxReady } 2000)
    Add-Check '01 real Ctrl+G focuses the page box and selects all of it' (Test-BoxReady) ("sent=$sent " + (Show-Sel))
    $view = [GpW]::GetSubMenu([GpW]::GetMenu($script:Main), 2)
    $st = Get-ViewState 40065
    $label = [GpW]::MenuText($view, 40065)
    Add-Check '02 View menu has an enabled "Go to Page...<TAB>Ctrl+G"' ((Test-Exists $st) -and (($st -band 0x3) -eq 0) -and ($label -eq "&Go to Page...`tCtrl+G")) ("state=0x{0:X} label='{1}'" -f $st, $label)

    # --- case 2: Ctrl+G while focused re-selects ------------------------------
    [void][GpW]::SendMessageW($script:Edit, 0x00B1, [IntPtr]1, [IntPtr]1)   # EM_SETSEL 1,1
    $s = Get-Sel
    Add-Check '03 CONTROL the driver can collapse the selection' (($s.Start -eq 1) -and ($s.End -eq 1)) (Show-Sel)
    Send-Cmd 40065
    [void](Wait-Until { Test-BoxReady } 2000)
    Add-Check '04 Ctrl+G while focused selects all again' (Test-BoxReady) (Show-Sel)

    # --- case 6: ESC reverts an uncommitted digit ----------------------------
    Send-Cmd 40065
    [void](Wait-Until { Test-BoxReady } 2000)
    $t0 = [GpW]::Text($script:Edit)
    $digit = '2'; if ($t0 -eq '2') { $digit = '3' }
    [void][GpW]::SendMessageW($script:Edit, 0x0102, [IntPtr][int][char]$digit, [IntPtr]1)   # WM_CHAR to the box
    $t1 = [GpW]::Text($script:Edit)
    Add-Check '05 CONTROL a typed digit lands in the box' ($t1 -ne $t0) "'$t0' -> '$t1'"
    Send-Cmd 40047   # IDM_FIND_CLOSE: the ESC accelerator's command
    [void](Wait-Until { (Get-FocusNow) -eq $script:Canvas } 2000)
    $t2 = [GpW]::Text($script:Edit)
    Add-Check '06 ESC returns focus to the canvas and reverts the digit' ((((Get-FocusNow) -eq $script:Canvas)) -and ($t2 -eq $t0)) ("text='$t2' want '$t0' " + (Show-Sel))
    if ([GpW]::Text($script:Edit) -ne $t0) { [void][GpW]::SendString($script:Edit, 0x000C, [IntPtr]::Zero, $t0) }   # keep later cases independent

    # --- case 3: a page change while focused keeps the selection ----------------
    Start-Process $Exe -ArgumentList ('"{0}"' -f $spread) | Out-Null
    [void](Wait-Until { [GpW]::Title($script:Main) -like '*spread-unequal*' } 6000)
    Start-Sleep -Milliseconds 800
    [void](Wait-Until { [GpW]::Text($script:Edit) -eq '1' } 3000)
    Send-Cmd 40065
    [void](Wait-Until { Test-BoxReady } 2000)
    Add-Check '07 CONTROL Ctrl+G on tab 2 focuses and selects the box' (Test-BoxReady) (Show-Sel)
    Send-Cmd 40031   # IDM_TAB_NEXT, wraps to tab 1
    [void](Wait-Until { [GpW]::Title($script:Main) -like '*simple*' } 3000)
    Start-Sleep -Milliseconds 500
    if ($Mode -eq 'normal') {
        Add-Check '08 a tab switch keeps the box focused and fully selected' (Test-BoxReady) (Show-Sel)
    } else {
        $s = Get-Sel
        Add-Check '08 MUTANT a tab switch collapses the selection to 0,0' ((((Get-FocusNow) -eq $script:Edit)) -and ($s.Start -eq 0) -and ($s.End -eq 0)) (Show-Sel)
    }

    # --- case 4: a hidden bar is shown -------------------------------------
    Send-Cmd 40064
    [void](Wait-Until { -not [GpW]::IsWindowVisible($script:Bar) } 2000)
    Start-Sleep -Milliseconds 300
    Add-Check '09 CONTROL hiding the bar moves the focus to the canvas' ((-not [GpW]::IsWindowVisible($script:Bar)) -and ((Get-FocusNow) -eq $script:Canvas)) (Show-Sel)
    Send-Cmd 40065
    [void](Wait-Until { [GpW]::IsWindowVisible($script:Bar) -and (Test-BoxReady) } 2000)
    Add-Check '10 Ctrl+G on a hidden bar shows it, selects the box, checks Status Bar' ([GpW]::IsWindowVisible($script:Bar) -and (Test-BoxReady) -and (Test-Checked 40064)) ((Show-Sel) + ' checked=' + (Test-Checked 40064))

    # --- case 5: no document ---------------------------------------------------
    Send-Cmd 40030; Start-Sleep -Milliseconds 700
    Send-Cmd 40030
    [void](Wait-Until { -not [GpW]::IsWindowEnabled($script:Edit) } 3000)
    Add-Check '11 CONTROL closing both tabs disables the box' (-not [GpW]::IsWindowEnabled($script:Edit)) ''
    # 40064 is a TOGGLE. Before the feature exists, case 4 left the bar hidden,
    # and an unconditional toggle here would show it again.
    if ([GpW]::IsWindowVisible($script:Bar)) { Send-Cmd 40064 }
    [void](Wait-Until { -not [GpW]::IsWindowVisible($script:Bar) } 2000)
    Start-Sleep -Milliseconds 300
    $f0 = Get-FocusNow
    Send-Cmd 40065; Start-Sleep -Milliseconds 700
    Add-Check '12 no document: Ctrl+G leaves the bar hidden and the focus alone' ((-not [GpW]::IsWindowVisible($script:Bar)) -and ((Get-FocusNow) -eq $f0)) ('focus=0x{0:X} was 0x{1:X}' -f [int64](Get-FocusNow), [int64]$f0)
    $st = Get-ViewState 40065
    Add-Check '13 no document: Go to Page is grayed' ((Test-Exists $st) -and (($st -band 0x1) -ne 0)) ('state=0x{0:X}' -f $st)
    Start-Process $Exe -ArgumentList ('"{0}"' -f $simple) | Out-Null
    [void](Wait-Until { [GpW]::IsWindowEnabled($script:Edit) } 6000)
    Start-Sleep -Milliseconds 500
    $st = Get-ViewState 40065
    Add-Check '14 CONTROL with a document Go to Page is enabled' ((Test-Exists $st) -and (($st -band 0x3) -eq 0)) ('state=0x{0:X}' -f $st)
    Send-Cmd 40065
    [void](Wait-Until { [GpW]::IsWindowVisible($script:Bar) -and (Test-BoxReady) } 2000)
    Add-Check '15 CONTROL with a document the same command shows the bar' ([GpW]::IsWindowVisible($script:Bar) -and (Test-BoxReady)) (Show-Sel)
} finally {
    if (-not $proc.HasExited) {
        [void][GpW]::PostMessageW($script:Main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $proc.WaitForExit(5000)) { $proc.Kill(); [void]$proc.WaitForExit(5000) }
    }
    if ($hadSession) { Copy-Item $backup $session -Force } elseif (Test-Path $session) { Remove-Item $session -Force }
    if ($hadMarker -and -not (Test-Path $marker)) { [System.IO.File]::WriteAllText($marker, 'running') }
}

$failed = @($script:Results | Where-Object { $_ -like 'FAIL*' })
Write-Host ''
Write-Host ("{0}: {1} checks, {2} failed" -f $Mode, $script:Results.Count, $failed.Count)
if ($failed.Count -gt 0) { exit 1 }
```

- [ ] **Step 3: Red run against the current build**

Step 1 built `build\Release\litepdf.exe` from the unchanged source. From PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\goto-page.ps1 -Exe C:\Users\User\projects\litepdf\build\Release\litepdf.exe -Mode normal
```

Expected: `normal: 16 checks, 10 failed`, exit code 1.
- FAIL: 01, 02, 04, 06, 07, 08, 10, 13, 14, 15. There is no command 40065 yet, so nothing focuses the box, the menu item does not exist (`state=0xFFFFFFFF`), and ESC has no focused box to revert.
- PASS: 00, 03, 05, 09, 11, 12 (controls, and 12 holds trivially before the feature exists).

If 00 fails, the run is inconclusive: bring nothing else to the foreground and rerun. If any other check differs from the lists above, stop: the driver is wrong, and fixing it comes before any product code.

- [ ] **Step 4: No commit**

The driver is untracked under `build/`. `git status --short` must print nothing.

---

## Task 2: The feature

**Files:**
- Modify: `src/ui/StatusBar.hpp` (after `page_box_has_focus()`, ~line 101)
- Modify: `src/ui/StatusBar.cpp` (`set_page` ~line 665; new functions after `page_box_has_focus` ~line 729)
- Modify: `src/ui/MainWindow.hpp:91`
- Modify: `src/ui/MainWindow.cpp` (`on_toggle_status_bar` ~644; `WM_INITMENUPOPUP` View arm ~1547; `WM_COMMAND` ~1661; `accels[]` ~2484)
- Modify: `resources/MainMenu.rc.h:69-76`
- Modify: `resources/litepdf.rc.in:77-79`

**Interfaces:**
- Consumes: Task 1's driver at `build/gui-check/goto-page.ps1` and its recorded ctest count.
- Produces: `bool litepdf::ui::StatusBar::page_box_enabled() const`, `void litepdf::ui::StatusBar::focus_page_box()`, `void litepdf::ui::MainWindow::set_status_bar_visible(bool visible)`, `void litepdf::ui::MainWindow::on_goto_page()`, `#define IDM_VIEW_GOTO_PAGE 40065`. In `StatusBar::set_page` the line `if (focused) SendMessageW(impl_->edit, EM_SETSEL, 0, -1);`, which Task 3's mutant removes by exact text.

- [ ] **Step 1: Command id**

In `resources/MainMenu.rc.h`, replace

```c
// #59: status bar toggle. Takes the first id of the old 40064-40070 reservation.
#define IDM_VIEW_STATUS_BAR  40064   // View > Status Bar (no accelerator)

// Next free ID: 40065. Reserve 40065-40070 for future Phase 8.x cleanups.

// #52: text selection. A fresh block, leaving the 40065-40070 reservation alone.
```

with

```c
// #59: status bar toggle. Takes the first id of the old 40064-40070 reservation.
#define IDM_VIEW_STATUS_BAR  40064   // View > Status Bar (no accelerator)

// #48: go-to-page accelerator. Takes the next id of the same reservation.
#define IDM_VIEW_GOTO_PAGE   40065   // Ctrl+G

// Next free ID: 40066. Reserve 40066-40070 for future Phase 8.x cleanups.

// #52: text selection. A fresh block, leaving the 40066-40070 reservation alone.
```

Leave the older `Next free ID: 40048` comment (line 56) alone; it is out of scope.

- [ ] **Step 2: Menu item**

In `resources/litepdf.rc.in`, replace

```
        MENUITEM "&Reset Zoom\tCtrl+0", IDM_ZOOM_RESET
```

with

```
        MENUITEM "&Reset Zoom\tCtrl+0", IDM_ZOOM_RESET
        MENUITEM SEPARATOR
        MENUITEM "&Go to Page...\tCtrl+G", IDM_VIEW_GOTO_PAGE
```

- [ ] **Step 3: `StatusBar` declarations**

In `src/ui/StatusBar.hpp`, directly after the `bool page_box_has_focus() const;` declaration, add:

```cpp

    // #48: the box can take a page number -- the box exists and a document
    // with at least one page is showing. Read from the state set_page() and
    // set_empty() keep, not IsWindowEnabled, so it cannot drift from
    // set_empty(). Ignores visibility: MainWindow shows a hidden bar first.
    // MainWindow's Ctrl+G guard and the View-menu enable state both use it.
    bool page_box_enabled() const;

    // #48 (Ctrl+G): give the page box the keyboard and select all of its text,
    // so the next digit replaces the page number. No-op unless
    // page_box_enabled(). The caller makes sure the bar is visible.
    void focus_page_box();
```

- [ ] **Step 4: `StatusBar` definitions**

In `src/ui/StatusBar.cpp`, directly after the body of `StatusBar::page_box_has_focus()`, add:

```cpp

bool StatusBar::page_box_enabled() const {
    // `edit` too: if the status bar control failed to create, the constructor
    // returned with no box, yet set_page() still records page_count.
    return impl_ && impl_->edit && impl_->page_count > 0;
}

void StatusBar::focus_page_box() {
    if (!page_box_enabled()) return;
    // Focus first, then select: the EDIT's own focus handling could move a
    // selection made before it had the focus.
    SetFocus(impl_->edit);
    SendMessageW(impl_->edit, EM_SETSEL, 0, -1);
}
```

- [ ] **Step 5: Re-select after a focused overwrite**

In `StatusBar::set_page`, replace

```cpp
        if (detail::should_overwrite_page_box(focused, impl_->edit_text(),
                                              impl_->last_written)) {
            impl_->force_revert();
        }
```

with

```cpp
        if (detail::should_overwrite_page_box(focused, impl_->edit_text(),
                                              impl_->last_written)) {
            impl_->force_revert();
            // #48: SetWindowTextW drops a focused EDIT's selection to (0,0)
            // even when the text is unchanged, so a page change after Ctrl+G
            // (a tab switch, a wheel notch over the canvas) would turn "type to
            // replace" into "type to prepend". The reader has not touched the
            // text, so the next keystroke should replace it -- whether the box
            // was entered by Ctrl+G or by a click.
            if (focused) SendMessageW(impl_->edit, EM_SETSEL, 0, -1);
        }
```

The `if (focused)` prefix is load-bearing for Task 3: it makes this line textually distinct from the `EM_SETSEL` in `focus_page_box()`.

- [ ] **Step 6: `MainWindow` declarations**

In `src/ui/MainWindow.hpp`, replace

```cpp
    void on_toggle_status_bar();         // IDM_VIEW_STATUS_BAR (#59)
```

with

```cpp
    void on_toggle_status_bar();         // IDM_VIEW_STATUS_BAR (#59)
    // Show or hide the status bar and re-fit the canvas to the new height.
    // The toggle calls it, and so does Ctrl+G on a hidden bar (#48).
    void set_status_bar_visible(bool visible);
    void on_goto_page();                 // IDM_VIEW_GOTO_PAGE (#48)
```

- [ ] **Step 7: `MainWindow` definitions**

In `src/ui/MainWindow.cpp`, replace the whole of `MainWindow::on_toggle_status_bar()`:

```cpp
void MainWindow::on_toggle_status_bar() {
    // #59: window-level state, so no active_view() gate -- the toggle works
    // with no document open.
    if (!status_bar_) return;
    status_bar_->set_visible(!status_bar_->visible());
    on_layout();
    // The canvas just changed height. Its own WM_SIZE resubmits only when the
    // render target has to be recreated, so without this a fit mode would keep
    // the fit it derived for the old height.
    if (auto* view = active_view()) kick_render(view->current_page());
}
```

with

```cpp
void MainWindow::on_toggle_status_bar() {
    // #59: window-level state, so no active_view() gate -- the toggle works
    // with no document open.
    if (!status_bar_) return;
    set_status_bar_visible(!status_bar_->visible());
}

void MainWindow::set_status_bar_visible(bool visible) {
    if (!status_bar_ || status_bar_->visible() == visible) return;
    status_bar_->set_visible(visible);
    on_layout();
    // The canvas just changed height. Its own WM_SIZE resubmits only when the
    // render target has to be recreated, so without this a fit mode would keep
    // the fit it derived for the old height.
    if (auto* view = active_view()) kick_render(view->current_page());
}

void MainWindow::on_goto_page() {
    // #48. This guard is required, not redundant. The GRAYED menu state stops
    // only the routes that pass WM_INITMENUPOPUP (the accelerator and the
    // menu); a posted WM_COMMAND skips it. And while a mouse-opened View popup
    // is up, a second instance can open a 0-page document between the enable
    // check and the click. Keep it the same expression as the View arm's
    // EnableMenuItem for IDM_VIEW_GOTO_PAGE.
    if (!status_bar_ || !status_bar_->page_box_enabled()) return;
    // The box lives in the status bar, so a hidden bar is shown first and
    // stays shown: there is no reliable hook to hide it again afterwards (the
    // box's WM_KILLFOCUS reverts text and tells no one), and the bar's
    // visibility is not persisted anyway.
    if (!status_bar_->visible()) set_status_bar_visible(true);
    status_bar_->focus_page_box();
}
```

- [ ] **Step 8: View-menu enable state**

In the `WM_INITMENUPOPUP` View arm, replace

```cpp
                const bool bar_on = status_bar_ && status_bar_->visible();
                CheckMenuItem(popup, IDM_VIEW_STATUS_BAR,
                              MF_BYCOMMAND
                              | (bar_on ? MF_CHECKED : MF_UNCHECKED));
                return 0;
```

with

```cpp
                const bool bar_on = status_bar_ && status_bar_->visible();
                CheckMenuItem(popup, IDM_VIEW_STATUS_BAR,
                              MF_BYCOMMAND
                              | (bar_on ? MF_CHECKED : MF_UNCHECKED));
                // #48: the same expression as on_goto_page()'s guard.
                // TranslateAcceleratorW sends WM_INITMENUPOPUP before acting
                // on Ctrl+G, and a GRAYED item swallows the key with no
                // WM_COMMAND -- the intended outcome with no page to go to.
                const bool can_goto = status_bar_ && status_bar_->page_box_enabled();
                EnableMenuItem(popup, IDM_VIEW_GOTO_PAGE,
                               MF_BYCOMMAND
                               | (can_goto ? MF_ENABLED : MF_GRAYED));
                return 0;
```

- [ ] **Step 9: Command arm**

In the `WM_COMMAND` switch, replace

```cpp
                case IDM_VIEW_STATUS_BAR:
                    on_toggle_status_bar();
                    return 0;
```

with

```cpp
                case IDM_VIEW_STATUS_BAR:
                    on_toggle_status_bar();
                    return 0;
                case IDM_VIEW_GOTO_PAGE:
                    on_goto_page();
                    return 0;
```

- [ ] **Step 10: Accelerator**

In `accels[]`, replace

```cpp
        { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET    },
```

with

```cpp
        { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET    },
        { FCONTROL | FVIRTKEY, 'G',          IDM_VIEW_GOTO_PAGE },  // #48
```

- [ ] **Step 11: Build and run the unit tests**

From PowerShell:

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
```

Expected: the build succeeds with no new warning from the edited files (a local that shadows an outer name, C4456, is the usual one in `WM_COMMAND`), and `100% tests passed` with the same count Task 1 recorded.

- [ ] **Step 12: Green run**

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\goto-page.ps1 -Exe C:\Users\User\projects\litepdf\build\Release\litepdf.exe -Mode normal
```

Expected: `normal: 16 checks, 0 failed`, exit code 0. If 00 fails, rerun (foreground race, not a product failure). Any other FAIL goes through superpowers:systematic-debugging before continuing.

- [ ] **Step 13: Commit**

```bash
git add resources/MainMenu.rc.h resources/litepdf.rc.in src/ui/StatusBar.hpp src/ui/StatusBar.cpp src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat: Ctrl+G focuses the status bar's page box (#48)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 3: Mutant probe for the re-select

**Files:**
- Create (untracked): `build/gui-check/make-probes-48.sh`
- Create (untracked): `build/gui-check/normal-probe.exe`, `build/gui-check/noresel-probe.exe` (overwrites any older `normal-probe.exe`)
- Temporarily modify, then restore: `src/ui/StatusBar.cpp`

**Interfaces:**
- Consumes: Task 2's commit (working tree clean); in `StatusBar::set_page`, the exact line `if (focused) SendMessageW(impl_->edit, EM_SETSEL, 0, -1);`; the driver `build/gui-check/goto-page.ps1 -Exe <path> -Mode normal|noresel`.
- Produces: two PASS/FAIL tables. Nothing is committed.

The mutant removes exactly one call: the `EM_SETSEL` that Task 2 Step 5 added after `force_revert()`. The one in `focus_page_box()` stays. That proves check 08 can fail and that this line is what makes it pass.

- [ ] **Step 1: Write the probe script**

Save as `build/gui-check/make-probes-48.sh`. It is a script file so the `trap` and the `exit` are scoped to it:

```bash
cd /c/Users/User/projects/litepdf
test -z "$(git status --short)" || { echo "working tree not clean"; exit 1; }
# Whatever happens below -- a failed build, Ctrl+C -- the source is put back.
trap 'git checkout -- src/ui/StatusBar.cpp' EXIT INT TERM
mkdir -p build/gui-check
rm -f build/gui-check/noresel-probe.exe build/gui-check/normal-probe.exe
python - <<'EOF'
import io
p = "src/ui/StatusBar.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
both = "SendMessageW(impl_->edit, EM_SETSEL, 0, -1);"
old = "if (focused) " + both
assert s.count(both) == 2, ("EM_SETSEL calls", s.count(both))
assert s.count(old) == 1, ("re-select line", s.count(old))
at = s.index(old)
ctx = s[max(0, at - 900):at]
assert "impl_->force_revert();" in ctx and "should_overwrite_page_box" in ctx, "re-select line is not in set_page's overwrite branch"
s = s.replace(old, "/* noresel probe */")
assert s.count(both) == 1, ("focus_page_box call must survive", s.count(both))
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("mutant patched")
EOF
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --target litepdf --config Release \
  && cp build/Release/litepdf.exe build/gui-check/noresel-probe.exe \
  || echo "MUTANT BUILD FAILED - no noresel-probe.exe was made"
git checkout -- src/ui/StatusBar.cpp
git diff --quiet -- src/ui/StatusBar.cpp || { echo "SOURCE NOT RESTORED - stop and fix by hand"; exit 1; }
test -z "$(git status --short)" || { echo "SOURCE NOT RESTORED - stop and fix by hand"; exit 1; }
echo "source restored"
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --target litepdf --config Release
cp build/Release/litepdf.exe build/gui-check/normal-probe.exe
```

- [ ] **Step 2: Build both exes**

```bash
bash build/gui-check/make-probes-48.sh
```

Expected: `mutant patched`, `source restored`, two builds succeed, no `MUTANT BUILD FAILED` line, and `build/gui-check/` holds `noresel-probe.exe` and `normal-probe.exe`. If `python` is not found, use `py -3`. Afterwards `git status --short` prints nothing.

- [ ] **Step 3: Run the real build**

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\goto-page.ps1 -Exe C:\Users\User\projects\litepdf\build\gui-check\normal-probe.exe -Mode normal
```

Expected: `normal: 16 checks, 0 failed`.

- [ ] **Step 4: Run the mutant**

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\build\gui-check\goto-page.ps1 -Exe C:\Users\User\projects\litepdf\build\gui-check\noresel-probe.exe -Mode noresel
```

Expected: `noresel: 16 checks, 0 failed`, with check 08 reading `PASS  08 MUTANT a tab switch collapses the selection to 0,0`. Every other check passes on the mutant too, because the removed line matters only when a page change reaches a focused box.

If check 08 FAILS on the mutant (the selection survived), either the mutant was not built from the patched source, or something else re-selects the box after a tab switch that the spec does not know about. Investigate before continuing.

- [ ] **Step 5: Save the evidence**

Copy both summaries and the full check lists into `build/gui-check/results-48.txt` (untracked). Nothing to commit.

---

## Task 4: Docs, final verification, user-assisted checks

**Files:**
- Modify: `README.md` (Keyboard shortcuts table ~line 69; Page indicator line ~line 45)
- Modify: `CHANGELOG.md` (`## [Unreleased]`, line 10)

**Interfaces:**
- Consumes: the finished feature from Task 2; Task 3's evidence (this task does not need it).
- Produces: the branch ready for the PR gate.

- [ ] **Step 1: README shortcut row**

In `README.md`, replace

```markdown
| Ctrl+0             | Reset zoom                          |
```

with

```markdown
| Ctrl+0             | Reset zoom                          |
| Ctrl+G             | Go to page (focus the page box)     |
```

- [ ] **Step 2: README feature line**

Replace the line that starts `- **Page indicator**`:

```markdown
- **Page indicator** — status bar showing the current page and page count, with a box you can type a page into (v1.3.0). It also shows the current zoom percentage, and View → Status Bar hides it (v1.4.0)
```

with

```markdown
- **Page indicator** — status bar showing the current page and page count, with a box you can type a page into (v1.3.0). It also shows the current zoom percentage, and View → Status Bar hides it (v1.4.0). Ctrl+G (View → Go to Page) puts the cursor in the box, showing the bar if it is hidden (unreleased)
```

- [ ] **Step 3: CHANGELOG**

In `CHANGELOG.md`, replace

```markdown
## [Unreleased]

## [1.4.0] — 2026-10-03 — Text selection, panning, and sideways scrolling
```

with

```markdown
## [Unreleased]

### Added

- Ctrl+G and View → Go to Page put the cursor in the status bar's page box with
  the page number selected, so typing a number and pressing Enter goes there
  (#48). If the status bar is hidden, Ctrl+G shows it. Esc returns to the page.

## [1.4.0] — 2026-10-03 — Text selection, panning, and sideways scrolling
```

Do not edit the 1.4.0 entry's "While it is hidden there is no page box to type a page into": it was true of that release.

- [ ] **Step 4: Full verification**

From PowerShell:

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --config Release
Push-Location C:\Users\User\projects\litepdf
& $ctest --test-dir build -C Release
Pop-Location
powershell -ExecutionPolicy Bypass -File C:\Users\User\projects\litepdf\scripts\smoke-test.ps1 -ExpectDev
```

Expected: the build succeeds; `100% tests passed` with Task 1's count; the smoke test exits 0.

- [ ] **Step 5: Commit**

```bash
git add README.md CHANGELOG.md
git commit -m "docs: Ctrl+G go to page (#48)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Hand the user the checks only a person can do**

Ask the user to run these on `build\Release\litepdf.exe` and record each answer:

1. **Real keyboard.** Open a multi-page PDF, press Ctrl+G, type a page number and press Enter. The page changes and the cursor leaves the box.
2. **Menu.** Open the View menu. The last group shows `Go to Page...` with `Ctrl+G` on the right. Close every tab and open View again: the item is grayed.
3. **Hidden bar.** View → Status Bar to hide the bar, then press Ctrl+G: the bar comes back with the page number selected. Press Esc, then Alt+V, B: the bar hides again.

A failure in any of them goes back through superpowers:systematic-debugging before the PR gate.
