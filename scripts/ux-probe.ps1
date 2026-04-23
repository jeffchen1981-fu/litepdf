# ux-probe.ps1 - Programmatic UX verification helper for LitePDF Phase 4
#
# Usage:
#   ux-probe.ps1 capture <output.png>                     - Capture litepdf window
#   ux-probe.ps1 children                                 - List child controls
#   ux-probe.ps1 sendkey <vk_hex>                         - Send WM_KEYDOWN/UP to litepdf (e.g. 0x74 for F5)
#   ux-probe.ps1 mru-read                                 - Read HKCU\Software\LitePDF\MRU entries
#   ux-probe.ps1 mru-write <path1> [path2] ...            - Write entries to MRU (most-recent first)
#   ux-probe.ps1 mru-clear                                - Delete the MRU registry key
#   ux-probe.ps1 file-menu                                - Trigger WM_INITMENUPOPUP, enumerate File submenu items
#   ux-probe.ps1 tab-enum                                 - JSON dump of tab control state (count/active/labels)
#   ux-probe.ps1 find-bar-state                           - JSON dump of find-bar child state (present/visible/rect)
#   ux-probe.ps1 find-open                                - Post WM_COMMAND IDM_FIND (40042) — convenience wrapper
#   ux-probe.ps1 cross-tab-find                           - Post WM_COMMAND IDM_CROSS_TAB_FIND (40045) — show ResultsPanel
#   ux-probe.ps1 results-panel-state                      - JSON dump of ResultsPanel child state (present/visible/rect)

param(
  [Parameter(Mandatory=$true, Position=0)] [string] $Action,
  [Parameter(Position=1)] [string] $Arg1,
  [Parameter(Position=2, ValueFromRemainingArguments=$true)] [string[]] $Rest
)

# --- Win32 P/Invoke ---
Add-Type -Namespace W -Name U32 -MemberDefinition @"
[DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Auto)]
public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
[DllImport("user32.dll", SetLastError=true)]
public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string name);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll", CharSet=CharSet.Auto)]
public static extern int GetClassName(IntPtr h, System.Text.StringBuilder buf, int n);
[DllImport("user32.dll", CharSet=CharSet.Auto)]
public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder buf, int n);
[DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
[DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
[DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr m, int pos);
[DllImport("user32.dll", CharSet=CharSet.Auto)]
public static extern int GetMenuString(IntPtr m, uint id, System.Text.StringBuilder buf, int n, uint flag);
[DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flag);
[DllImport("user32.dll", SetLastError=true)]
public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flag);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }
"@

function Get-LitePdfHwnd {
  $hwnd = [W.U32]::FindWindow('LitePDFMainWindow', $null)
  if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [W.U32]::FindWindow($null, 'LitePDF') }
  if ($hwnd -eq [IntPtr]::Zero) {
    # Try by partial title match via Get-Process
    $p = Get-Process litepdf -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { $hwnd = $p.MainWindowHandle }
  }
  return $hwnd
}

function Capture-Window([IntPtr]$hwnd, [string]$outPath) {
  Add-Type -AssemblyName System.Drawing
  $r = New-Object W.U32+RECT
  [void][W.U32]::GetWindowRect($hwnd, [ref]$r)
  $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  # PW_RENDERFULLCONTENT = 2 — works for DWM-composited windows
  [void][W.U32]::PrintWindow($hwnd, $hdc, 2)
  $g.ReleaseHdc($hdc)
  $g.Dispose()
  $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host "Captured ${w}x${h} -> $outPath"
}

function ScreenCap-Window([IntPtr]$hwnd, [string]$outPath) {
  # Capture from screen DC — sees actual GPU-composited content (D2D, etc.)
  # Window must be foreground + unobstructed for accurate capture.
  Add-Type -AssemblyName System.Drawing
  $r = New-Object W.U32+RECT
  [void][W.U32]::GetWindowRect($hwnd, [ref]$r)
  $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
  [void][W.U32]::SetForegroundWindow($hwnd)
  Start-Sleep -Milliseconds 300
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, [System.Drawing.Size]::new($w, $h))
  $g.Dispose()
  $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host "ScreenCap ${w}x${h} -> $outPath"
}

function Resize-Window([IntPtr]$hwnd, [int]$dw, [int]$dh) {
  $r = New-Object W.U32+RECT
  [void][W.U32]::GetWindowRect($hwnd, [ref]$r)
  $cw = $r.Right - $r.Left; $ch = $r.Bottom - $r.Top
  $nw = $cw + $dw; $nh = $ch + $dh
  $SWP_NOMOVE = 0x0002; $SWP_NOZORDER = 0x0004
  [void][W.U32]::SetWindowPos($hwnd, [IntPtr]::Zero, 0, 0, $nw, $nh, ($SWP_NOMOVE -bor $SWP_NOZORDER))
  Write-Host "Resized ${cw}x${ch} -> ${nw}x${nh}"
}

function List-Children([IntPtr]$parent, [int]$indent = 0) {
  # Use EnumChildWindows-equivalent via FindWindowEx loop with explicit empty IntPtr.Zero handling.
  # PowerShell's $null can mis-marshal as empty string instead of NULL.
  $child = [IntPtr]::Zero
  $maxIter = 50
  while ($maxIter-- -gt 0) {
    $child = [W.U32]::FindWindowEx($parent, $child, [String]::Empty, [String]::Empty)
    if ($child -eq [IntPtr]::Zero) {
      # Try with actual nulls via reflection
      $child = [IntPtr]::Zero
      break
    }
    $cls = New-Object System.Text.StringBuilder 256
    [void][W.U32]::GetClassName($child, $cls, 256)
    $vis = [W.U32]::IsWindowVisible($child)
    $r = New-Object W.U32+RECT
    [void][W.U32]::GetWindowRect($child, [ref]$r)
    $sp = ' ' * ($indent * 2)
    Write-Host ("{0}HWND=0x{1:X8}  class={2,-20}  visible={3}  size={4}x{5}" -f `
      $sp, $child.ToInt64(), $cls.ToString(), $vis, ($r.Right-$r.Left), ($r.Bottom-$r.Top))
    if ($indent -lt 2) { List-Children $child ($indent + 1) }
  }
}

function Find-TreeView([IntPtr]$parent) {
  # SysTreeView32 is the standard Win32 TreeView class
  return [W.U32]::FindWindowEx($parent, [IntPtr]::Zero, "SysTreeView32", [String]::Empty)
}

function Send-VK([IntPtr]$hwnd, [int]$vk) {
  # Bring foreground first so the accelerator dispatch works
  [void][W.U32]::SetForegroundWindow($hwnd)
  Start-Sleep -Milliseconds 100
  $WM_KEYDOWN = 0x0100; $WM_KEYUP = 0x0101
  [void][W.U32]::PostMessage($hwnd, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]0)
  Start-Sleep -Milliseconds 50
  [void][W.U32]::PostMessage($hwnd, $WM_KEYUP, [IntPtr]$vk, [IntPtr]0x40000000)
  Start-Sleep -Milliseconds 200
}

function Read-MRU {
  $key = 'HKCU:\Software\LitePDF\MRU'
  if (-not (Test-Path $key)) { Write-Host "(no MRU key)"; return }
  $count = (Get-ItemProperty $key -Name Count -ErrorAction SilentlyContinue).Count
  Write-Host "Count: $count"
  for ($i = 0; $i -lt 10; $i++) {
    $v = (Get-ItemProperty $key -Name "Entry$i" -ErrorAction SilentlyContinue)."Entry$i"
    if ($v) { Write-Host "  Entry${i}: $v" }
  }
}

function Write-MRU([string[]]$paths) {
  $key = 'HKCU:\Software\LitePDF\MRU'
  if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
  $n = [Math]::Min($paths.Count, 10)
  Set-ItemProperty $key -Name Count -Value $n -Type DWord
  for ($i = 0; $i -lt $n; $i++) {
    Set-ItemProperty $key -Name "Entry$i" -Value $paths[$i] -Type String
  }
  for ($i = $n; $i -lt 10; $i++) {
    Remove-ItemProperty $key -Name "Entry$i" -ErrorAction SilentlyContinue
  }
  Write-Host "Wrote $n MRU entries"
}

function Clear-MRU {
  $key = 'HKCU:\Software\LitePDF\MRU'
  if (Test-Path $key) { Remove-Item $key -Recurse -Force; Write-Host "Cleared MRU key" }
  else { Write-Host "(MRU key already absent)" }
}

function File-Menu([IntPtr]$hwnd) {
  $menu = [W.U32]::GetMenu($hwnd)
  if ($menu -eq [IntPtr]::Zero) { Write-Host "(no menu)"; return }
  $fileSub = [W.U32]::GetSubMenu($menu, 0)
  if ($fileSub -eq [IntPtr]::Zero) { Write-Host "(no File submenu)"; return }
  # Trigger WM_INITMENUPOPUP so MRU rebuilds
  $WM_INITMENUPOPUP = 0x0117
  [void][W.U32]::SendMessage($hwnd, $WM_INITMENUPOPUP, $fileSub, [IntPtr]0)
  $count = [W.U32]::GetMenuItemCount($fileSub)
  Write-Host "File submenu has $count items:"
  for ($i = 0; $i -lt $count; $i++) {
    $id = [W.U32]::GetMenuItemID($fileSub, $i)
    $sb = New-Object System.Text.StringBuilder 256
    [void][W.U32]::GetMenuString($fileSub, [uint32]$i, $sb, 256, 0x400)  # MF_BYPOSITION
    $state = [W.U32]::GetMenuState($fileSub, [uint32]$i, 0x400)
    $text = $sb.ToString()
    if ($id -eq 0xFFFFFFFF -or $id -eq 0) {
      Write-Host ("  [{0,2}] (separator)         state=0x{1:X4}" -f $i, $state)
    } else {
      Write-Host ("  [{0,2}] id={1,5}  text='{2}'  state=0x{3:X4}" -f $i, $id, $text, $state)
    }
  }
}

function Tab-Enum([IntPtr]$parent) {
  # Find the SysTabControl32 child of the LitePDF main window, read tab count,
  # active index, and each tab's label via TCM_GETITEMW. Emits one line of JSON
  # to stdout so callers (smoke-test.ps1) can parse directly. Any diagnostics
  # must go to stderr via [Console]::Error.WriteLine so stdout stays JSON-clean.
  $tabc = [W.U32]::FindWindowEx($parent, [IntPtr]::Zero, "SysTabControl32", [String]::Empty)
  if ($tabc -eq [IntPtr]::Zero) {
    Write-Output '{"count":0,"active":-1,"labels":[]}'
    return
  }
  $TCM_GETITEMCOUNT = 0x1304
  $TCM_GETCURSEL    = 0x130B
  $TCM_GETITEMW     = 0x133C
  $TCIF_TEXT        = 0x0001

  $count  = [W.U32]::SendMessage($tabc, $TCM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
  $active = [W.U32]::SendMessage($tabc, $TCM_GETCURSEL,    [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()

  # Allocate a text buffer in unmanaged memory. 260 wchars covers MAX_PATH
  # which is well above any legitimate Tab::label (currently path.filename()).
  # Under-sizing would silently truncate — TCM_GETITEMW has no error signal.
  $bufLen  = 260
  $bufSize = $bufLen * 2
  $bufPtr  = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($bufSize)

  # TCITEMW on x64: UINT mask (4) + DWORD dwState (4) + DWORD dwStateMask (4)
  # + 4 bytes padding + LPWSTR pszText (8) + int cchTextMax (4) + int iImage (4)
  # + LPARAM lParam (8). Offsets: mask=0, pszText=16, cchTextMax=24. Use 64 bytes
  # to be safe against any compiler padding.
  $structSize = 64
  $structPtr  = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($structSize)

  $labels = @()
  try {
    for ($i = 0; $i -lt $count; $i++) {
      # Zero the struct and the text buffer each iteration.
      for ($z = 0; $z -lt $structSize; $z++) {
        [System.Runtime.InteropServices.Marshal]::WriteByte($structPtr, $z, 0)
      }
      for ($z = 0; $z -lt $bufSize; $z++) {
        [System.Runtime.InteropServices.Marshal]::WriteByte($bufPtr, $z, 0)
      }
      [System.Runtime.InteropServices.Marshal]::WriteInt32($structPtr, 0,  $TCIF_TEXT)
      [System.Runtime.InteropServices.Marshal]::WriteIntPtr($structPtr, 16, $bufPtr)
      [System.Runtime.InteropServices.Marshal]::WriteInt32($structPtr, 24, $bufLen)

      [void][W.U32]::SendMessage($tabc, $TCM_GETITEMW, [IntPtr]$i, $structPtr)
      $label = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($bufPtr)
      if ($null -eq $label) { $label = "" }
      $labels += $label
    }
  } finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($structPtr)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($bufPtr)
  }

  $obj = [ordered]@{ count = $count; active = $active; labels = $labels }
  # ConvertTo-Json -Compress forces a single line; force array on labels so a
  # single-tab case still serializes as ["foo"] not "foo".
  $json = $obj | ConvertTo-Json -Compress
  Write-Output $json
}

# --- Dispatch ---
switch ($Action) {
  'capture' {
    if (-not $Arg1) { Write-Error "Usage: capture <out.png>"; exit 1 }
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    Write-Host "Found HWND 0x$($h.ToInt64().ToString('X8'))"
    Capture-Window $h $Arg1
  }
  'children' {
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    Write-Host "Top HWND: 0x$($h.ToInt64().ToString('X8'))"
    List-Children $h
  }
  'treeview' {
    # Find SysTreeView32 child + report its visibility + size
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    $tv = Find-TreeView $h
    if ($tv -eq [IntPtr]::Zero) { Write-Host "(no TreeView child found)"; exit 0 }
    $vis = [W.U32]::IsWindowVisible($tv)
    $r = New-Object W.U32+RECT
    [void][W.U32]::GetWindowRect($tv, [ref]$r)
    Write-Host ("TreeView HWND=0x{0:X8}  visible={1}  size={2}x{3}" -f $tv.ToInt64(), $vis, ($r.Right-$r.Left), ($r.Bottom-$r.Top))
  }
  'screencap' {
    if (-not $Arg1) { Write-Error "Usage: screencap <out.png>"; exit 1 }
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    ScreenCap-Window $h $Arg1
  }
  'resize' {
    if (-not $Arg1) { Write-Error "Usage: resize <dw> <dh>"; exit 1 }
    $dw = [int]$Arg1
    $dh = if ($Rest) { [int]$Rest[0] } else { 0 }
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    Resize-Window $h $dw $dh
  }
  'sendkey' {
    if (-not $Arg1) { Write-Error "Usage: sendkey <vk_hex>"; exit 1 }
    $vk = [Convert]::ToInt32($Arg1, 16)
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    Send-VK $h $vk
    Write-Host "Sent VK 0x$($vk.ToString('X2')) to HWND 0x$($h.ToInt64().ToString('X8'))"
  }
  'mru-read'   { Read-MRU }
  'mru-write'  {
    $list = @()
    if ($Arg1) { $list += $Arg1 }
    if ($Rest) { $list += $Rest }
    if (-not $list) { Write-Error "Usage: mru-write <path1> [path2] ..."; exit 1 }
    Write-MRU $list
  }
  'mru-clear'  { Clear-MRU }
  'file-menu'  {
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    File-Menu $h
  }
  'send-cmd' {
    # PostMessage WM_COMMAND with the given control ID (e.g. 40020 for IDM_MRU_1)
    if (-not $Arg1) { Write-Error "Usage: send-cmd <id>"; exit 1 }
    $id = [int]$Arg1
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    $WM_COMMAND = 0x0111
    [void][W.U32]::PostMessage($h, $WM_COMMAND, [IntPtr]$id, [IntPtr]0)
    Write-Host "Posted WM_COMMAND id=$id to HWND 0x$($h.ToInt64().ToString('X8'))"
  }
  'find-dialog' {
    # Look for an active dialog (class "#32770") that's a child of the litepdf top-level window
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    # Find any "#32770" dialog window (MessageBox uses this class)
    $dlg = [W.U32]::FindWindow("#32770", [String]::Empty)
    if ($dlg -eq [IntPtr]::Zero) { Write-Host "(no dialog found)"; exit 0 }
    $title = New-Object System.Text.StringBuilder 256
    [void][W.U32]::GetWindowText($dlg, $title, 256)
    Write-Host ("Dialog HWND=0x{0:X8}  title='{1}'" -f $dlg.ToInt64(), $title.ToString())
    # Try to capture its text content via child enumeration (static control with text)
    $static = [W.U32]::FindWindowEx($dlg, [IntPtr]::Zero, "Static", [String]::Empty)
    while ($static -ne [IntPtr]::Zero) {
      $txt = New-Object System.Text.StringBuilder 1024
      [void][W.U32]::GetWindowText($static, $txt, 1024)
      $t = $txt.ToString()
      if ($t.Length -gt 0 -and $t -ne 'LitePDF') { Write-Host "  Text: $t" }
      $static = [W.U32]::FindWindowEx($dlg, $static, "Static", [String]::Empty)
    }
  }
  'dismiss-dialog' {
    # Send Enter to the active dialog to dismiss it (default = OK button)
    $dlg = [W.U32]::FindWindow("#32770", [String]::Empty)
    if ($dlg -eq [IntPtr]::Zero) { Write-Error "no dialog found"; exit 1 }
    [void][W.U32]::SetForegroundWindow($dlg)
    Start-Sleep -Milliseconds 150
    $WM_KEYDOWN = 0x0100; $WM_KEYUP = 0x0101
    [void][W.U32]::PostMessage($dlg, $WM_KEYDOWN, [IntPtr]0x0D, [IntPtr]0)  # VK_RETURN
    [void][W.U32]::PostMessage($dlg, $WM_KEYUP, [IntPtr]0x0D, [IntPtr]0x40000000)
    Write-Host "Sent VK_RETURN to dialog HWND 0x$($dlg.ToInt64().ToString('X8'))"
  }
  'find-bar-state' {
    # Locate a LitePDFFindBar child of the main window, report its
    # presence / visibility / rect as one line of JSON on stdout. Missing
    # window reports present=false rather than a non-zero exit, so
    # callers can poll without try/catch noise.
    #
    # TODO(phase-6.x): also report counter text (Static child of the
    # find bar) so the smoke test can assert "N hits" after a query.
    # Requires a second FindWindowEx for the Static with counter style,
    # plus careful buffer handling similar to Tab-Enum above.
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) {
      Write-Output '{"present":false,"visible":false}'
      exit 0
    }
    $fb = [W.U32]::FindWindowEx($h, [IntPtr]::Zero, "LitePDFFindBar", [String]::Empty)
    if ($fb -eq [IntPtr]::Zero) {
      Write-Output '{"present":false,"visible":false}'
      exit 0
    }
    $vis = [W.U32]::IsWindowVisible($fb)
    $r = New-Object W.U32+RECT
    [void][W.U32]::GetWindowRect($fb, [ref]$r)
    $obj = [ordered]@{
      present = $true
      visible = [bool]$vis
      rect    = @{ left = $r.Left; top = $r.Top; right = $r.Right; bottom = $r.Bottom }
    }
    $obj | ConvertTo-Json -Compress
  }
  'find-open' {
    # Convenience: post WM_COMMAND IDM_FIND (40042) and return. Smoke
    # test has its own send-cmd call; this wrapper exists so a human
    # running the script manually doesn't need to remember the magic id.
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    $WM_COMMAND = 0x0111
    [void][W.U32]::PostMessage($h, $WM_COMMAND, [IntPtr]40042, [IntPtr]0)
    Write-Host "Posted IDM_FIND to HWND 0x$($h.ToInt64().ToString('X8'))"
  }
  'cross-tab-find' {
    # Phase 6 Task 14: post WM_COMMAND IDM_CROSS_TAB_FIND (40045). This
    # shows the bottom-docked ResultsPanel (creating height on first call).
    # Caller is responsible for the subsequent assertions (results-panel-state
    # below) — this action is intentionally fire-and-forget so it composes
    # into scripted UX tours the same way find-open does.
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) { Write-Error "litepdf window not found"; exit 1 }
    $WM_COMMAND = 0x0111
    [void][W.U32]::PostMessage($h, $WM_COMMAND, [IntPtr]40045, [IntPtr]0)
    Write-Host "Posted IDM_CROSS_TAB_FIND to HWND 0x$($h.ToInt64().ToString('X8'))"
  }
  'results-panel-state' {
    # Report whether the bottom-docked ResultsPanel child is present and
    # visible. Contract matches find-bar-state: stdout is always one line
    # of JSON, missing state reports present=false rather than a non-zero
    # exit so smoke-test can poll without try/catch noise.
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) {
      Write-Output '{"present":false,"visible":false}'
      exit 0
    }
    $rp = [W.U32]::FindWindowEx($h, [IntPtr]::Zero, "LitePDFResultsPanel", [String]::Empty)
    if ($rp -eq [IntPtr]::Zero) {
      Write-Output '{"present":false,"visible":false}'
      exit 0
    }
    $vis = [W.U32]::IsWindowVisible($rp)
    $r = New-Object W.U32+RECT
    [void][W.U32]::GetWindowRect($rp, [ref]$r)
    $obj = [ordered]@{
      present = $true
      visible = [bool]$vis
      rect    = @{ left = $r.Left; top = $r.Top; right = $r.Right; bottom = $r.Bottom }
    }
    $obj | ConvertTo-Json -Compress
  }
  'tab-enum' {
    # Unlike sibling actions (capture, sendkey, ...), this action must
    # never exit non-zero on "window not found" — smoke-test polls with
    # retry and treats empty JSON as "not up yet". The contract is:
    # stdout is always exactly one line of valid JSON; any missing-state
    # reports as count=0 rather than throwing.
    $h = Get-LitePdfHwnd
    if ($h -eq [IntPtr]::Zero) {
      Write-Output '{"count":0,"active":-1,"labels":[]}'
      exit 0
    }
    Tab-Enum $h
  }
  default { Write-Error "Unknown action: $Action"; exit 1 }
}
