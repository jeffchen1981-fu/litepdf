# Phase 0: Bootstrap — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Stand up the CMake project, integrate MuPDF as a submodule, produce a `litepdf.exe` that opens an empty Win32 window in ≤ 500 ms on HDD, and establish a green CI pipeline on Windows. No PDF functionality yet.

**Architecture:** A single-target CMake build with MuPDF linked statically via `add_subdirectory`. `src/main.cpp` contains a minimal `WinMain` that registers a window class, creates a top-level HWND, runs a message loop. Common Controls v6 and per-monitor DPI awareness are declared in a side-by-side manifest. Resources (`.rc`) reserve IDs for icons (actual icons arrive in Phase 9; Phase 0 uses a placeholder). CI on GitHub Actions builds on a `windows-latest` runner and enforces exe size and smoke-test gates.

**Tech Stack:** CMake 3.25+, MSVC 2022 (or clang-cl), Windows SDK, MuPDF (static, submodule, tag-pinned), Inno Setup (not used yet but directory reserved), Catch2 (vendored in Phase 1). Static CRT (`/MT`).

**Prerequisites:**

- Windows 11 with Visual Studio 2022 Build Tools or IDE (Desktop C++ workload)
- `git` 2.30+
- `cmake` 3.25+ on PATH
- Internet access (to fetch MuPDF submodule)

**Done when:**

1. `cmake --build build --config Release` succeeds from a clean clone.
2. `build/Release/litepdf.exe` exists and is ≤ 2 MB (no MuPDF rendering yet; this budget tightens after Phase 2).
3. Running the exe opens an empty 1024×768 window titled "LitePDF" within 500 ms on an HDD.
4. GitHub Actions workflow `ci.yml` is green on the initial push.
5. All Phase 0 tasks are committed in small, atomic commits on `main`.

---

## Task 1: Create `.gitignore` and `LICENSE`

**Files:**

- Create: `.gitignore`
- Create: `LICENSE`

**Step 1: Write `.gitignore`**

```gitignore
# Build output
/build/
/out/

# IDE
.vs/
.vscode/
*.user
*.suo
CMakeSettings.json

# CMake
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
Makefile

# Compiler output
*.obj
*.exe
*.pdb
*.ilk
*.exp
*.lib

# Misc
Thumbs.db
.DS_Store
```

**Step 2: Write `LICENSE`**

Fetch the canonical AGPL-3.0 text and save to `LICENSE`. In PowerShell:

```powershell
Invoke-WebRequest -Uri https://www.gnu.org/licenses/agpl-3.0.txt -OutFile LICENSE
```

Verify the file starts with `GNU AFFERO GENERAL PUBLIC LICENSE` and ends with the full license text.

**Step 3: Commit**

```bash
git add .gitignore LICENSE
git commit -m "chore: add .gitignore and AGPL-3.0 license"
```

---

## Task 2: Create `VERSION` and `README.md` skeleton

**Files:**

- Create: `VERSION`
- Create: `README.md`

**Step 1: Write `VERSION`**

```
0.1.0-dev
```

**Step 2: Write `README.md` skeleton**

```markdown
# LitePDF

A lightweight PDF / ePub / CBZ / XPS reader for Windows 11, optimized for mechanical hard drives. Single self-contained executable, no runtime dependencies.

- **Status:** under development (Phase 0 — bootstrap)
- **License:** AGPL-3.0
- **Design:** [`docs/plans/2026-04-15-litepdf-design.md`](docs/plans/2026-04-15-litepdf-design.md)
- **Roadmap:** [`docs/plans/2026-04-15-litepdf-roadmap.md`](docs/plans/2026-04-15-litepdf-roadmap.md)

## Build

```
git clone --recursive https://github.com/<user>/litepdf
cd litepdf
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The produced `build/Release/litepdf.exe` is a single self-contained binary.
```

**Step 3: Commit**

```bash
git add VERSION README.md
git commit -m "docs: add VERSION and README skeleton"
```

---

## Task 3: Add MuPDF as a git submodule (tag-pinned)

**Files:**

- Modify: `.gitmodules` (auto-generated)
- Create: `third_party/mupdf/` (submodule working tree)

**Step 1: Decide on pinned tag**

Use the latest stable release tag (e.g. `1.24.11`). Verify it exists:

```bash
git ls-remote --tags https://github.com/ArtifexSoftware/mupdf.git | grep -E "refs/tags/1\.24\." | tail -5
```

**Step 2: Add submodule**

```bash
git submodule add https://github.com/ArtifexSoftware/mupdf.git third_party/mupdf
cd third_party/mupdf
git checkout 1.24.11   # replace with the version chosen in Step 1
git submodule update --init --recursive
cd ../..
```

**Step 3: Verify**

```bash
ls third_party/mupdf/include/mupdf/fitz.h     # should exist
cat .gitmodules                               # should reference mupdf at the chosen path
```

**Step 4: Commit**

```bash
git add .gitmodules third_party/mupdf
git commit -m "build: add MuPDF 1.24.11 as submodule"
```

---

## Task 4: Write `cmake/CompilerFlags.cmake`

**Files:**

- Create: `cmake/CompilerFlags.cmake`

**Step 1: Write the file**

```cmake
# Compiler flags shared across all LitePDF targets.
# Sets static CRT, warning level, Unicode, and C++17.

function(litepdf_apply_flags target)
    target_compile_features(${target} PRIVATE cxx_std_17)

    if(MSVC)
        # Static CRT: no VC++ Redistributable dependency at runtime.
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

        target_compile_options(${target} PRIVATE
            /W4             # high warning level
            /permissive-    # strict conformance
            /Zc:__cplusplus # report correct __cplusplus value
            /utf-8          # source + execution charset UTF-8
        )

        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE             # Win32 W-APIs by default
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )
    endif()
endfunction()
```

**Step 2: Commit**

```bash
git add cmake/CompilerFlags.cmake
git commit -m "build: add shared compiler flags helper"
```

---

## Task 5: Write `resources/manifest.xml`

**Files:**

- Create: `resources/manifest.xml`

**Step 1: Write the manifest**

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="0.1.0.0" processorArchitecture="*" name="LitePDF" type="win32"/>
  <description>LitePDF — lightweight PDF reader</description>

  <!-- Common Controls v6 (modern look, TreeView/Tab/ListView features) -->
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
                        version="6.0.0.0" processorArchitecture="*"
                        publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>

  <!-- Per-monitor DPI awareness v2 (correct rendering on mixed-DPI setups) -->
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>

  <!-- Declare supported Windows versions (10/11) -->
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/> <!-- Windows 10/11 -->
    </application>
  </compatibility>
</assembly>
```

**Step 2: Write `resources/litepdf.rc`**

```
#include <winres.h>

// Version info
VS_VERSION_INFO VERSIONINFO
FILEVERSION     0,1,0,0
PRODUCTVERSION  0,1,0,0
FILEFLAGSMASK   VS_FFI_FILEFLAGSMASK
FILEFLAGS       0
FILEOS          VOS_NT_WINDOWS32
FILETYPE        VFT_APP
FILESUBTYPE     VFT2_UNKNOWN
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName",      "LitePDF"
      VALUE "FileDescription",  "LitePDF — lightweight PDF reader"
      VALUE "FileVersion",      "0.1.0.0"
      VALUE "InternalName",     "litepdf"
      VALUE "LegalCopyright",   "AGPL-3.0"
      VALUE "OriginalFilename", "litepdf.exe"
      VALUE "ProductName",      "LitePDF"
      VALUE "ProductVersion",   "0.1.0.0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0409, 1200
  END
END

// Manifest (side-by-side assembly)
CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST "manifest.xml"

// Icon slots (placeholder; real icons arrive in Phase 9)
// IDI_APPICON    101 ICON "icon/litepdf-app.ico"
// IDI_PDFDOC     102 ICON "icon/litepdf-doc.ico"
```

**Step 3: Commit**

```bash
git add resources/manifest.xml resources/litepdf.rc
git commit -m "build: add application manifest (Common Controls v6, PerMonitor DPI, UTF-8)"
```

---

## Task 6: Write minimal `src/main.cpp`

**Files:**

- Create: `src/main.cpp`

**Step 1: Write the minimal WinMain**

```cpp
// LitePDF — bootstrap entry point.
// Phase 0: opens an empty top-level window. No PDF logic yet.

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"LitePDFMainWindow";
constexpr wchar_t kWindowTitle[]     = L"LitePDF";

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool RegisterMainClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    return RegisterClassExW(&wc) != 0;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    if (!RegisterMainClass(hInstance)) {
        MessageBoxW(nullptr, L"Failed to register window class.", kWindowTitle, MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create main window.", kWindowTitle, MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
```

**Step 2: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add minimal WinMain that opens an empty window"
```

---

## Task 7: Write root `CMakeLists.txt`

**Files:**

- Create: `CMakeLists.txt`

**Step 1: Write the build script**

```cmake
cmake_minimum_required(VERSION 3.25)
project(litepdf
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "Lightweight PDF reader for Windows")

# Global settings
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerFlags)

# --- litepdf executable ---------------------------------------------------
add_executable(litepdf WIN32
    src/main.cpp
    resources/litepdf.rc
)

litepdf_apply_flags(litepdf)

target_link_libraries(litepdf PRIVATE
    comctl32
)

set_target_properties(litepdf PROPERTIES
    OUTPUT_NAME           "litepdf"
    RUNTIME_OUTPUT_NAME   "litepdf"
)

# Resource compiler: ensure it can find manifest.xml relative to the .rc file
target_include_directories(litepdf PRIVATE
    "${CMAKE_SOURCE_DIR}/resources"
)

# MuPDF will be wired in during Phase 1 (Document core). Deliberately omitted
# here to keep Phase 0 build time small and isolate bootstrap failures.
```

**Step 2: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add root CMakeLists with litepdf executable target"
```

---

## Task 8: Verify a clean build

**Step 1: Configure**

Run:

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
```

Expected: configuration succeeds with no errors. A `build/` folder appears.

**Step 2: Build**

```bash
cmake --build build --config Release --parallel
```

Expected: `build/Release/litepdf.exe` is produced. No warnings at `/W4`.

**Step 3: Run**

Double-click `build/Release/litepdf.exe` (or run `./build/Release/litepdf.exe`).

Expected: a 1024×768 window titled "LitePDF" appears. Closing it terminates the process. Process exit code is 0.

**Step 4: Verify size budget**

```bash
# size in bytes
stat -c %s build/Release/litepdf.exe  # bash
# or
(Get-Item build/Release/litepdf.exe).Length  # PowerShell
```

Expected: under **2 097 152 bytes (2 MB)** — Phase 0 does not yet include MuPDF in the exe.

**Step 5: No commit** (this task is pure verification). If any step fails, fix the underlying task and re-verify before proceeding.

---

## Task 9: Add a smoke test script

**Files:**

- Create: `scripts/smoke-test.ps1`

**Step 1: Write the script**

```powershell
#!/usr/bin/env pwsh
# Phase 0 smoke test: build, run briefly, verify window opens and exe is under budget.
# Exits non-zero on any failure; CI uses this as the final gate.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$exe = Join-Path $repoRoot "build/Release/litepdf.exe"

if (-not (Test-Path $exe)) {
    throw "exe not found at $exe — build first"
}

# Size budget (Phase 0): 2 MB
$maxBytes = 2MB
$size = (Get-Item $exe).Length
if ($size -gt $maxBytes) {
    throw "litepdf.exe is $size bytes, exceeds Phase 0 budget of $maxBytes"
}
Write-Host "[OK] exe size: $size bytes"

# Launch briefly, verify process starts and window appears, then kill.
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Milliseconds 800
if ($proc.HasExited) {
    throw "litepdf.exe exited immediately (exit code $($proc.ExitCode))"
}
if (-not $proc.MainWindowHandle -or $proc.MainWindowHandle -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc.Id -Force
    throw "litepdf.exe did not create a main window within 800 ms"
}
Write-Host "[OK] main window handle: $($proc.MainWindowHandle)"

Stop-Process -Id $proc.Id -Force
Write-Host "[OK] Phase 0 smoke test passed."
```

**Step 2: Run locally**

```powershell
pwsh scripts/smoke-test.ps1
```

Expected output:

```
[OK] exe size: <N> bytes
[OK] main window handle: <handle>
[OK] Phase 0 smoke test passed.
```

Expected exit code: 0.

**Step 3: Commit**

```bash
git add scripts/smoke-test.ps1
git commit -m "test: add Phase 0 smoke test (size budget + window creation)"
```

---

## Task 10: GitHub Actions CI

**Files:**

- Create: `.github/workflows/ci.yml`

**Step 1: Write the workflow**

```yaml
name: CI

on:
  push:
    branches: [ main ]
  pull_request:

jobs:
  build-windows:
    runs-on: windows-latest

    steps:
      - name: Checkout with submodules
        uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Configure
        run: cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build --config Release --parallel

      - name: Smoke test
        shell: pwsh
        run: ./scripts/smoke-test.ps1

      - name: Upload exe artifact
        uses: actions/upload-artifact@v4
        with:
          name: litepdf-phase0
          path: build/Release/litepdf.exe
          if-no-files-found: error
```

**Step 2: Commit and push**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: build and smoke-test on Windows runner"
git push -u origin main   # or open a PR if you prefer
```

**Step 3: Verify CI green**

Watch the Actions tab on GitHub. Expected: the `build-windows` job completes successfully, uploads `litepdf-phase0` artifact.

If the job fails:

- If the MuPDF submodule checkout is slow/flaky, add `fetch-depth: 1` to the checkout step.
- If compiler errors appear that didn't appear locally, the runner's MSVC version may differ — tighten warning suppressions rather than lowering `/W4`.
- **Do not** mark Phase 0 complete until the CI is green at least once.

---

## Task 11: Tag the phase

**Step 1: Tag and push**

```bash
git tag -a v0.0.1-phase0 -m "Phase 0 complete: bootstrap"
git push origin v0.0.1-phase0
```

This anchors the phase boundary for future `git log` archaeology and gives Phase 1's re-planning step a concrete commit to diff against.

---

## Exit Checklist

Before declaring Phase 0 done:

- [ ] `cmake --build build --config Release` is green from a clean clone
- [ ] `build/Release/litepdf.exe` exists, is < 2 MB
- [ ] Running the exe opens an empty "LitePDF" window and closes cleanly
- [ ] `scripts/smoke-test.ps1` exits 0 locally
- [ ] GitHub Actions `build-windows` job is green
- [ ] All commits are small, atomic, and on `main`
- [ ] `v0.0.1-phase0` tag is pushed

## What Phase 1 Will Build On

Phase 1 (Document core) will:

- Wire MuPDF into the CMake build as a second target linked into `litepdf`.
- Add `src/core/Document.hpp` + `.cpp` and unit tests under `tests/unit/`.
- Introduce Catch2 (either via submodule or single-header drop-in).
- Produce a `litepdf-cli` console test harness that prints PDF metadata — a throwaway artifact that makes Phase 1 visibly demoable without any UI.

Re-plan Phase 1 with `superpowers:writing-plans` after Phase 0 is tagged and pushed.
