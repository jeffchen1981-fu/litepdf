# CJK System-Font Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drop the embedded ~4.84 MB CJK font (MuPDF `TOFU_CJK`) and render CJK from Windows system fonts via a DirectWrite `load_cjk_font` hook, shrinking `litepdf.exe` ~12.31 MB → ~7.5 MB (the 8 MB target).

**Architecture:** A new `src/core/SystemFonts.{hpp,cpp}` unit provides a pure `cjk_family_candidates` mapping, a DirectWrite-backed `resolve_cjk_system_font` hook (with a base14 last-resort so it never returns NULL — D6), and `install_system_cjk_font_loader`, installed once per `Document` base context. The prune flip adds `TOFU_CJK` (keeping `TOFU_CJK_LANG`) in `cmake/ImportMuPDF.cmake`. Size gates are re-baselined from the measured Release exe.

**Tech Stack:** C++17, MuPDF 1.27.2, DirectWrite (`dwrite.lib`), `Microsoft::WRL::ComPtr`, Catch2 v3, CMake (VS 2022 BuildTools, Release/MT), PowerShell 5.1-safe CI scripts.

**Spec:** [docs/superpowers/specs/2026-06-21-cjk-system-font-loader-design.md](../specs/2026-06-21-cjk-system-font-loader-design.md) (v3, two three-lens review rounds).

**Build/test reminder (project memory `reference_litepdf_build_test_commands`):** Build **Release**, not Debug (MuPDF static libs are MT_StaticRelease → LNK2038 on Debug). `cmake`/`ctest` are NOT on PATH. Use:
```
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
```
Run the test exe from the repo root (fixtures resolve relative to `CMAKE_SOURCE_DIR`).

---

## File Structure

- **Create** `src/core/SystemFonts.hpp` — public surface: `cjk_family_candidates`, `resolve_cjk_system_font`, `install_system_cjk_font_loader`. Forward-declares the fz ABI (no mupdf headers) so test/consumer TUs without the mupdf include path can use it (mirrors `Document.hpp`).
- **Create** `src/core/SystemFonts.cpp` — DirectWrite resolution + process-wide cache + base14 last-resort + install. Includes the real `<mupdf/fitz.h>` (litepdf_core compiles with mupdf's include dir).
- **Create** `tests/unit/test_system_fonts.cpp` — unit tests for the pure mapping and the D6 last-resort.
- **Create** `tests/unit/test_cjk_extract_liveness.cpp` — open+extract liveness on the three CJK fixtures post-prune.
- **Modify** `CMakeLists.txt` — add `SystemFonts.cpp` to `litepdf_core`; add `dwrite` to its link libs.
- **Modify** `tests/CMakeLists.txt` — register `test_system_fonts.cpp`.
- **Modify** `src/core/Document.cpp:86` — install the hook.
- **Modify** `cmake/ImportMuPDF.cmake` — add `TOFU_CJK`, bump `_PRUNE_VER`, extend the assertion.
- **Modify** `benchmarks/thresholds.json`, `scripts/check-benchmark-regression.ps1`, `scripts/smoke-test.ps1` — size gates.
- **Create** `tests/fixtures/cjk-form.pdf` + generator — CJK form-appearance fixture (D6 integration liveness).
- **Modify** `tests/fixtures/cjk-reference/*.png` — regenerate (new system-font render).
- **Modify** `docs/plans/2026-04-15-litepdf-roadmap.md` — mark C-real / 8 MB DONE + known limitations.

---

## Task 1: Pure CJK family mapping (`cjk_family_candidates`)

**Files:**
- Create: `src/core/SystemFonts.hpp`
- Create: `src/core/SystemFonts.cpp`
- Create: `tests/unit/test_system_fonts.cpp`
- Modify: `CMakeLists.txt:49` (add source), `tests/CMakeLists.txt:71` (register test)

- [ ] **Step 1: Create the header**

`src/core/SystemFonts.hpp`:
```cpp
// src/core/SystemFonts.hpp
#pragma once

#include <string>
#include <vector>

// Forward-declare the MuPDF C ABI handles. Like Document.hpp, this header is
// included by translation units (and unit tests) that do NOT get MuPDF's include
// path, so it must not pull in any mupdf/*.h.
extern "C" {
struct fz_context;
struct fz_font;
}

namespace litepdf::core {

// Pure mapping: Adobe CJK ordering (FZ_ADOBE_CNS=0, GB=1, JAPAN=2, KOREA=3) +
// serif flag -> an ordered list of Windows font-family names to try. Empty for
// an unknown ordering. No OS calls — unit-testable.
std::vector<std::wstring> cjk_family_candidates(int ordering, int serif);

// MuPDF system-CJK-font hook (matches fz_load_system_cjk_font_fn). Resolves a
// Windows system font via DirectWrite; returns a new fz_font (caller owns one
// ref). NEVER returns NULL unless even the base14 last-resort fails (pathological).
// noexcept: no C++ exception may cross into MuPDF's C frame.
fz_font* resolve_cjk_system_font(fz_context* ctx, const char* name,
                                 int ordering, int serif) noexcept;

// Install resolve_cjk_system_font as ctx's CJK hook. Call once per Document base
// context, right after fz_register_document_handlers. fz_clone_context shares the
// refcounted font context, so all per-render/worker clones inherit it.
void install_system_cjk_font_loader(fz_context* ctx) noexcept;

}  // namespace litepdf::core
```

- [ ] **Step 2: Create the test file with the failing mapping test**

`tests/unit/test_system_fonts.cpp`:
```cpp
#include "core/SystemFonts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using litepdf::core::cjk_family_candidates;
using WVec = std::vector<std::wstring>;

TEST_CASE("cjk_family_candidates: maps ordering+serif to Windows families",
          "[core][fonts][cjk]") {
    // FZ_ADOBE_CNS=0 (Traditional Chinese)
    REQUIRE(cjk_family_candidates(0, 1) == WVec{L"PMingLiU", L"MingLiU"});
    REQUIRE(cjk_family_candidates(0, 0) ==
            WVec{L"Microsoft JhengHei", L"Microsoft JhengHei UI"});
    // FZ_ADOBE_GB=1 (Simplified Chinese)
    REQUIRE(cjk_family_candidates(1, 1) == WVec{L"SimSun", L"NSimSun"});
    REQUIRE(cjk_family_candidates(1, 0) ==
            WVec{L"Microsoft YaHei", L"Microsoft YaHei UI"});
    // FZ_ADOBE_JAPAN=2
    REQUIRE(cjk_family_candidates(2, 1) == WVec{L"MS Mincho", L"MS PMincho"});
    REQUIRE(cjk_family_candidates(2, 0) == WVec{L"Yu Gothic", L"MS Gothic", L"Meiryo"});
    // FZ_ADOBE_KOREA=3
    REQUIRE(cjk_family_candidates(3, 1) == WVec{L"Batang", L"BatangChe"});
    REQUIRE(cjk_family_candidates(3, 0) == WVec{L"Malgun Gothic", L"Gulim"});
}

TEST_CASE("cjk_family_candidates: unknown ordering is empty",
          "[core][fonts][cjk]") {
    REQUIRE(cjk_family_candidates(99, 1).empty());
    REQUIRE(cjk_family_candidates(-1, 0).empty());
}
```

- [ ] **Step 3: Create `SystemFonts.cpp` with ONLY the mapping (rest stubbed)**

`src/core/SystemFonts.cpp` (mapping first; the DirectWrite/install bodies are filled in Task 2 — for now stub them so the TU compiles and links):
```cpp
// src/core/SystemFonts.cpp
#include "core/SystemFonts.hpp"

namespace litepdf::core {

std::vector<std::wstring> cjk_family_candidates(int ordering, int serif) {
    // FZ_ADOBE_CNS=0, FZ_ADOBE_GB=1, FZ_ADOBE_JAPAN=2, FZ_ADOBE_KOREA=3.
    switch (ordering) {
        case 0:  // Traditional Chinese
            return serif ? std::vector<std::wstring>{L"PMingLiU", L"MingLiU"}
                         : std::vector<std::wstring>{L"Microsoft JhengHei",
                                                     L"Microsoft JhengHei UI"};
        case 1:  // Simplified Chinese
            return serif ? std::vector<std::wstring>{L"SimSun", L"NSimSun"}
                         : std::vector<std::wstring>{L"Microsoft YaHei",
                                                     L"Microsoft YaHei UI"};
        case 2:  // Japanese
            return serif ? std::vector<std::wstring>{L"MS Mincho", L"MS PMincho"}
                         : std::vector<std::wstring>{L"Yu Gothic", L"MS Gothic",
                                                     L"Meiryo"};
        case 3:  // Korean
            return serif ? std::vector<std::wstring>{L"Batang", L"BatangChe"}
                         : std::vector<std::wstring>{L"Malgun Gothic", L"Gulim"};
        default:
            return {};
    }
}

// resolve_cjk_system_font + install_system_cjk_font_loader are implemented in
// Task 2. Temporary stubs so the unit-test target links for the Task-1 mapping
// test. (fz_font is forward-declared; returning nullptr is valid here.)
fz_font* resolve_cjk_system_font(fz_context*, const char*, int, int) noexcept {
    return nullptr;
}
void install_system_cjk_font_loader(fz_context*) noexcept {}

}  // namespace litepdf::core
```

- [ ] **Step 4: Wire the source + test into CMake**

In `CMakeLists.txt`, add to the `litepdf_core` source list (after line 50, near `SearchQuery.cpp`):
```cmake
    src/core/SystemFonts.cpp       # cjk-system-font-loader Task 1
```
In `tests/CMakeLists.txt`, add to the `target_sources(litepdf_unit_tests PRIVATE ...)` list (after line 72):
```cmake
    unit/test_system_fonts.cpp         # cjk-system-font-loader Task 1
```

- [ ] **Step 5: Build and run the test — verify it passes**

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[core][fonts][cjk]"
Pop-Location
```
Expected: 2 test cases pass (the mapping + the unknown-ordering case). (CMake reconfigure on first build prints "MuPDF prune-effective assertion passed" — normal.)

- [ ] **Step 6: Commit**

```bash
git add src/core/SystemFonts.hpp src/core/SystemFonts.cpp tests/unit/test_system_fonts.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(fonts): cjk_family_candidates pure ordering->Windows-family mapping"
```

---

## Task 2: DirectWrite resolver, base14 last-resort, and install

**Files:**
- Modify: `src/core/SystemFonts.cpp` (replace the Task-1 stubs)
- Modify: `CMakeLists.txt:87-89` (add `dwrite` to `litepdf_core` link libs)
- Modify: `tests/unit/test_system_fonts.cpp` (add the D6 last-resort test)

- [ ] **Step 1: Add the D6 last-resort failing test**

Append to `tests/unit/test_system_fonts.cpp`:
```cpp
#include "core/Document.hpp"

// Forward-declare the fz ABI bits the test drops. litepdf_core links mupdf
// PRIVATE, so the test target has no mupdf include path — local extern-C decls
// are enough (same pattern as test_document_clone_context.cpp).
extern "C" {
struct fz_context;
struct fz_font;
void fz_drop_context(fz_context* ctx);
void fz_drop_font(fz_context* ctx, fz_font* font);
}

using litepdf::core::Document;
using litepdf::core::resolve_cjk_system_font;

TEST_CASE("resolve_cjk_system_font: never returns NULL — base14 last resort (D6)",
          "[core][fonts][cjk][d6]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx != nullptr);

    // Ordering 99 matches no candidate family, so DirectWrite resolution is
    // skipped entirely and the loader must fall through to the base14 Helvetica
    // last-resort — deterministic on any machine, even one with CJK fonts.
    fz_font* font = resolve_cjk_system_font(ctx, "SourceHanSerif", 99, 1);
    REQUIRE(font != nullptr);

    fz_drop_font(ctx, font);
    fz_drop_context(ctx);
}
```

- [ ] **Step 2: Run it — verify it FAILS**

```powershell
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[d6]"
Pop-Location
```
Expected: FAIL — the Task-1 stub `resolve_cjk_system_font` returns nullptr, so `REQUIRE(font != nullptr)` fails.

- [ ] **Step 3: Replace the stubs with the real implementation**

Replace the two stub functions at the bottom of `src/core/SystemFonts.cpp`, and add the includes/anonymous-namespace helpers. The full file becomes:
```cpp
// src/core/SystemFonts.cpp
#include "core/SystemFonts.hpp"

extern "C" {
#include <mupdf/fitz.h>
}

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace litepdf::core {

std::vector<std::wstring> cjk_family_candidates(int ordering, int serif) {
    switch (ordering) {  // FZ_ADOBE_CNS=0, GB=1, JAPAN=2, KOREA=3
        case 0:
            return serif ? std::vector<std::wstring>{L"PMingLiU", L"MingLiU"}
                         : std::vector<std::wstring>{L"Microsoft JhengHei",
                                                     L"Microsoft JhengHei UI"};
        case 1:
            return serif ? std::vector<std::wstring>{L"SimSun", L"NSimSun"}
                         : std::vector<std::wstring>{L"Microsoft YaHei",
                                                     L"Microsoft YaHei UI"};
        case 2:
            return serif ? std::vector<std::wstring>{L"MS Mincho", L"MS PMincho"}
                         : std::vector<std::wstring>{L"Yu Gothic", L"MS Gothic",
                                                     L"Meiryo"};
        case 3:
            return serif ? std::vector<std::wstring>{L"Batang", L"BatangChe"}
                         : std::vector<std::wstring>{L"Malgun Gothic", L"Gulim"};
        default:
            return {};
    }
}

namespace {
using Microsoft::WRL::ComPtr;

// Process-lifetime SHARED DirectWrite factory (free-threaded; created once).
// DirectWrite needs no CoInitialize (unlike the PrintDlgEx path).
IDWriteFactory* dwrite_factory() {
    static IDWriteFactory* factory = [] {
        IDWriteFactory* f = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&f));
        return f;  // process-lifetime leak is acceptable
    }();
    return factory;
}

struct ResolvedFace { std::string path; int index = 0; bool ok = false; };

std::mutex g_cache_mtx;
std::unordered_map<int, ResolvedFace> g_cache;  // key = ordering*2 + serif

// Resolve ONE family name -> (utf8 path, ttc face index) via DirectWrite.
bool resolve_family_path(const std::wstring& family, std::string& out_path,
                         int& out_index) {
    IDWriteFactory* factory = dwrite_factory();
    if (!factory) return false;
    ComPtr<IDWriteFontCollection> coll;
    if (FAILED(factory->GetSystemFontCollection(&coll, FALSE))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(coll->FindFamilyName(family.c_str(), &idx, &exists)) || !exists)
        return false;
    ComPtr<IDWriteFontFamily> fam;
    if (FAILED(coll->GetFontFamily(idx, &fam))) return false;
    ComPtr<IDWriteFont> font;
    if (FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
        return false;
    ComPtr<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(&face))) return false;
    UINT32 nfiles = 1;
    ComPtr<IDWriteFontFile> file;  // MUST stay alive until GetFilePathFromKey
    if (FAILED(face->GetFiles(&nfiles, file.GetAddressOf())) || nfiles != 1)
        return false;
    const void* key = nullptr;
    UINT32 keySize = 0;
    if (FAILED(file->GetReferenceKey(&key, &keySize))) return false;
    ComPtr<IDWriteFontFileLoader> loader;
    if (FAILED(file->GetLoader(&loader))) return false;
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (FAILED(loader.As(&local))) return false;  // non-local -> skip candidate
    UINT32 len = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &len))) return false;
    std::wstring wpath(static_cast<size_t>(len) + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, wpath.data(), len + 1)))
        return false;
    wpath.resize(len);
    out_index = static_cast<int>(face->GetIndex());
    int n = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    if (n <= 1) return false;
    out_path.assign(static_cast<size_t>(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, out_path.data(), n,
                        nullptr, nullptr);
    return true;
}

// Cached resolution. Double-checked: the mutex is NOT held across the slow
// DirectWrite enumeration (release on miss, re-lock to insert).
ResolvedFace resolve_cached(int ordering, int serif) {
    const int key = ordering * 2 + (serif ? 1 : 0);
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it->second;
    }
    ResolvedFace r;
    for (const std::wstring& fam : cjk_family_candidates(ordering, serif))
        if (resolve_family_path(fam, r.path, r.index)) { r.ok = true; break; }
    if (!r.ok)  // fall through to the other style before giving up
        for (const std::wstring& fam : cjk_family_candidates(ordering, serif ? 0 : 1))
            if (resolve_family_path(fam, r.path, r.index)) { r.ok = true; break; }
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        g_cache[key] = r;  // a redundant concurrent compute is harmless
    }
    return r;
}
}  // namespace

fz_font* resolve_cjk_system_font(fz_context* ctx, const char* name, int ordering,
                                 int serif) noexcept {
    try {
        ResolvedFace r = resolve_cached(ordering, serif);
        if (r.ok) {
            fz_font* font = nullptr;
            fz_try(ctx) {
                font = fz_new_font_from_file(ctx, name, r.path.c_str(), r.index, 0);
            }
            fz_catch(ctx) { font = nullptr; }  // fall through to last resort
            if (font) return font;
        }
        // D6 last resort: a guaranteed base14 face so the hook never returns
        // NULL. A NULL return makes fz_new_cjk_font throw (font.c:967), which in
        // appearance synthesis (pdf-appearance.c:1670) blanks the page. Helvetica
        // has no CJK glyphs, so the page renders notdef tofu instead. Catch ALL
        // fz errors here (incl. FZ_ERROR_SYSTEM/TRYLATER, which
        // fz_load_system_cjk_font would otherwise rethrow) and return NULL.
        fz_font* fallback = nullptr;
        fz_try(ctx) {
            int len = 0;
            const unsigned char* data = fz_lookup_base14_font(ctx, "Helvetica", &len);
            if (data) fallback = fz_new_font_from_memory(ctx, name, data, len, 0, 0);
        }
        fz_catch(ctx) { fallback = nullptr; }
        return fallback;
    } catch (...) {
        return nullptr;  // no C++ exception may cross into MuPDF's C frame
    }
}

void install_system_cjk_font_loader(fz_context* ctx) noexcept {
    // Install only the CJK hook; load_font / load_fallback_font stay NULL (D2).
    fz_install_load_system_font_funcs(ctx, nullptr, &resolve_cjk_system_font,
                                      nullptr);
}

}  // namespace litepdf::core
```

- [ ] **Step 4: Add `dwrite` to the link libraries**

In `CMakeLists.txt`, change the `litepdf_core` link block (lines 87-89) to:
```cmake
target_link_libraries(litepdf_core PRIVATE
    litepdf::mupdf
    dwrite          # cjk-system-font-loader: DirectWrite system-font loader
)
```

- [ ] **Step 5: Build and run the D6 test — verify it PASSES**

```powershell
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[core][fonts][cjk]"
Pop-Location
```
Expected: all 3 cases pass (mapping, unknown-ordering, D6 last-resort returns non-NULL).

- [ ] **Step 6: Commit**

```bash
git add src/core/SystemFonts.cpp CMakeLists.txt tests/unit/test_system_fonts.cpp
git commit -m "feat(fonts): DirectWrite CJK resolver with base14 last-resort + install hook"
```

---

## Task 3: Install the hook in `Document`

**Files:**
- Modify: `src/core/Document.cpp` (include + install at line 86)

- [ ] **Step 1: Add the include**

Near the top of `src/core/Document.cpp` (with the other `core/` includes), add:
```cpp
#include "core/SystemFonts.hpp"
```

- [ ] **Step 2: Install the hook in `Document::Impl()`**

In `Document::Impl()` (currently `Document.cpp:80-87`), add the install call after `fz_register_document_handlers(ctx)`:
```cpp
        ctx = fz_new_context(nullptr, &locks->fz, FZ_STORE_DEFAULT);
        if (!ctx) throw std::bad_alloc();
        fz_register_document_handlers(ctx);
        install_system_cjk_font_loader(ctx);  // cjk-system-font-loader: system CJK
```
(`Document.cpp` is in `namespace litepdf::core`, so the call is unqualified.)

- [ ] **Step 3: Build the full test target — verify still green**

```powershell
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe
Pop-Location
```
Expected: full suite passes (the install is a no-op behaviorally until the prune flip in Task 4, since the embedded CJK font is still present and MuPDF never calls the hook yet). Confirm exit 0, no new failures.

- [ ] **Step 4: Commit**

```bash
git add src/core/Document.cpp
git commit -m "feat(fonts): install system CJK font loader on every Document context"
```

---

## Task 4: Prune flip (`TOFU_CJK`) + measure exe

**Files:**
- Modify: `cmake/ImportMuPDF.cmake` (lines 99, 117, 169, 177)

- [ ] **Step 1: Add `TOFU_CJK` to the inject set and bump the version**

In `cmake/ImportMuPDF.cmake`:
- Line 99: `set(_PRUNE_VER "LITEPDF_PRUNE_V9")` → `set(_PRUNE_VER "LITEPDF_PRUNE_V10")`
- Line 117: after `"#define TOFU_CJK_LANG\n"` add a new line **keeping** `TOFU_CJK_LANG`:
```cmake
    "#define TOFU_CJK_LANG\n"
    "#define TOFU_CJK\n")
```
(Note: the closing `)` moves to the new last line. `TOFU_CJK` is the outermost guard at `font-table.h:281`, so it drops the embedded CJK block; keeping `TOFU_CJK_LANG` leaves the `font.c:2124` Han script-fallback consult compiled out — byte-identical there.)

- [ ] **Step 2: Extend the prune-effective assertion**

In `cmake/ImportMuPDF.cmake`:
- Line 169: `foreach(_t TOFU_SYMBOL TOFU_NOTO TOFU_CJK_LANG)` → `foreach(_t TOFU_SYMBOL TOFU_NOTO TOFU_CJK_LANG TOFU_CJK)`
- Line 177: update the status message count:
```cmake
message(STATUS "MuPDF prune-effective assertion passed (12 FZ_ENABLE_* + 4 TOFU_*; TOFU_EMOJI exempt).")
```

- [ ] **Step 3: Reconfigure + rebuild MuPDF + litepdf (Release)**

The `_PRUNE_VER` bump makes the configure step restore `config.h`, re-inject the V10 set, and delete the ExternalProject stamp so MuPDF rebuilds. Run a full configure + build:
```powershell
& $cmake -S C:\Users\User\projects\litepdf -B C:\Users\User\projects\litepdf\build
& $cmake --build C:\Users\User\projects\litepdf\build --config Release
```
Expected: configure prints `MuPDF config.h pruned (LITEPDF_PRUNE_V10).` and `MuPDF prune-effective assertion passed (... + 4 TOFU_* ...)`. MuPDF recompiles (the stamp was deleted); litepdf relinks. No link errors.

- [ ] **Step 4: Measure the new exe size and record it**

```powershell
(Get-Item C:\Users\User\projects\litepdf\build\Release\litepdf.exe).Length
```
Expected: roughly **7,000,000–8,000,000** bytes (down from 12,314,624). **Record the exact value** — it sets the gate numbers in Task 6. If it is materially above 8 MB, stop and investigate (no further font lever remains; §9 of the spec).

- [ ] **Step 5: Commit**

```bash
git add cmake/ImportMuPDF.cmake
git commit -m "build(mupdf): TOFU_CJK prune drops embedded CJK font (V10); keep TOFU_CJK_LANG"
```

---

## Task 5: CJK rendering verification

**Files:**
- Verify: `tests/unit/test_document_search.cpp` (existing CJK case, must stay green)
- Create: `tests/unit/test_cjk_render_liveness.cpp` + register in `tests/CMakeLists.txt`
- Create: `scripts/generate-cjk-form-fixture.py` + `tests/fixtures/cjk-form.pdf`
- Modify: `tests/fixtures/cjk-reference/cjk-zh-hant.png`, `cjk-ja.png`, `cjk-ko.png`

- [ ] **Step 1: Confirm the existing CJK search/extraction test is still green**

This is the automated extraction-regression gate (spec §6). Run it post-prune:
```powershell
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[document][search][unicode]"
Pop-Location
```
Expected: `page_hits: Unicode needle matches CJK text` PASSES (extraction is ToUnicode/CMap-based, font-independent). If it fails, the font switch regressed extraction — stop and investigate before proceeding.

- [ ] **Step 2: Add CJK render liveness tests**

`tests/unit/test_cjk_extract_liveness.cpp`:
```cpp
// Liveness: the three CJK fixtures still open and extract text after the embedded
// CJK font is dropped (TOFU_CJK) and CJK is system-resolved. Extraction is
// ToUnicode/CMap-based (font-independent), so this stays green on any runner
// regardless of which CJK fonts it has. Glyph-rendering fidelity is checked via
// the CLI render + manual visual vs the reference PNGs (Step 6), not in CI.
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using litepdf::core::Document;

namespace {
void require_opens_and_extracts(const char* fixture) {
    Document doc;
    REQUIRE_FALSE(doc.open(std::filesystem::path(fixture)).has_value());
    REQUIRE(doc.page_count() >= 1);
    std::string text = doc.page_text(0);  // must not throw post-prune
    (void)text;                            // content varies across fixtures
    SUCCEED();
}
}  // namespace

TEST_CASE("CJK fixtures open + extract without crashing (post-TOFU_CJK)",
          "[core][fonts][cjk][liveness]") {
    require_opens_and_extracts("tests/fixtures/cjk-zh-hant.pdf");
    require_opens_and_extracts("tests/fixtures/cjk-ja.pdf");
    require_opens_and_extracts("tests/fixtures/cjk-ko.pdf");
}
```
Register it in `tests/CMakeLists.txt`:
```cmake
    unit/test_cjk_extract_liveness.cpp  # cjk-system-font-loader Task 5
```

- [ ] **Step 3: Build + run the liveness test**

```powershell
& $cmake --build C:\Users\User\projects\litepdf\build --target litepdf_unit_tests --config Release
Push-Location C:\Users\User\projects\litepdf
& .\build\tests\Release\litepdf_unit_tests.exe "[cjk][liveness]"
Pop-Location
```
Expected: PASS (opens + extracts, no crash).

- [ ] **Step 4: Create the CJK form-appearance fixture (D6 integration check)**

`scripts/generate-cjk-form-fixture.py` — emits a single-page AcroForm PDF with a text field whose `/V` is a CJK string, `NeedAppearances true`, **no `/AP`**, and a non-embedded CIDFont in the AcroForm `/DR`, so MuPDF must synthesize the appearance via `fz_new_cjk_font`:
```python
#!/usr/bin/env python3
"""Generate tests/fixtures/cjk-form.pdf — a CJK AcroForm field with no /AP, so
MuPDF synthesizes the appearance (exercising the pdf-appearance.c fz_new_cjk_font
path that D6 protects). Deterministic raw-PDF emit (no compression)."""
import struct, zlib  # noqa: F401
from pathlib import Path

# Minimal hand-built PDF. CJK value U+4E2D U+6587 (UTF-16BE) in the field /V.
objs = []
def add(s): objs.append(s); return len(objs)
# 1 catalog, 2 pages, 3 page, 4 acroform, 5 field/widget, 6 CIDFont type0.
cat   = "<< /Type /Catalog /Pages 2 0 R /AcroForm 4 0 R >>"
pages = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"
page  = ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
         "/Annots [5 0 R] /Resources << >> >>")
acro  = ("<< /Fields [5 0 R] /NeedAppearances true "
         "/DR << /Font << /F0 6 0 R >> >> /DA (/F0 12 Tf 0 g) >>")
# /V is UTF-16BE for the two CJK code points; no /AP -> MuPDF synthesizes it.
field = ("<< /Type /Annot /Subtype /Widget /FT /Tx /T (cjk) "
         "/Rect [10 30 190 70] /F 4 /P 3 0 R "
         "/DA (/F0 12 Tf 0 g) /V <FEFF4E2D6587> >>")
cid   = ("<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light "
         "/Encoding /UniGB-UCS2-H "
         "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType0 "
         "/BaseFont /STSong-Light "
         "/CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> "
         "/FontDescriptor << /Type /FontDescriptor /FontName /STSong-Light "
         "/Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 880 "
         "/Descent -120 /StemV 80 >> >>] >>")
for s in (cat, pages, page, acro, field, cid): add(s)

out = bytearray(b"%PDF-1.7\n")
offsets = []
for i, body in enumerate(objs, start=1):
    offsets.append(len(out))
    out += f"{i} 0 obj\n{body}\nendobj\n".encode("latin-1")
xref_pos = len(out)
out += f"xref\n0 {len(objs)+1}\n".encode()
out += b"0000000000 65535 f \n"
for off in offsets:
    out += f"{off:010d} 00000 n \n".encode()
out += (f"trailer\n<< /Size {len(objs)+1} /Root 1 0 R >>\n"
        f"startxref\n{xref_pos}\n%%EOF\n").encode()
Path("tests/fixtures/cjk-form.pdf").write_bytes(out)
print("wrote tests/fixtures/cjk-form.pdf", len(out), "bytes")
```
Run it from the repo root:
```powershell
Push-Location C:\Users\User\projects\litepdf
python scripts\generate-cjk-form-fixture.py
Pop-Location
```

- [ ] **Step 5: Manually verify D6 closes the blank-page path (dev machine)**

This is the integration check for D6 (the deterministic unit guard is Task 2's test; this confirms the real appearance path). On the dev machine:
1. Render the form fixture with the loader installed (current build) and confirm the page is **not blank**. `--render N` emits a PPM to stdout, so redirect it, then check for non-white pixels with Pillow:
```powershell
Push-Location C:\Users\User\projects\litepdf
& .\build\Release\litepdf-cli.exe tests\fixtures\cjk-form.pdf --render 0 > out-form.ppm
python -c "from PIL import Image; im=Image.open('out-form.ppm').convert('RGB'); print('non-white pixels:', any(p!=(255,255,255) for p in im.getdata()))"
Pop-Location
```
Expected: `non-white pixels: True` (the widget border / synthesized field text paints).
2. (Optional, to prove the guard) Temporarily comment out the `install_system_cjk_font_loader(ctx)` line, rebuild, re-render: the page should now be **blank/white** (`non-white pixels: False` — appearance synthesis throws → display list dropped). Restore the line and rebuild. Record the before/after in the PR description. (Do not commit the commented-out version.)

- [ ] **Step 6: Regenerate the reference PNGs + manual visual fidelity check**

The committed `tests/fixtures/cjk-reference/*.png` were rendered against Droid Sans Fallback and are now stale. `--render N` emits a PPM to stdout; render page 0 of each fixture to PPM and convert to PNG with Pillow, then eyeball that real glyphs (not tofu) appear, and that Traditional Chinese is **PMingLiU** (明體), not MingLiU_HKSCS (confirms the TTC face index):
```powershell
Push-Location C:\Users\User\projects\litepdf
foreach ($f in @("cjk-zh-hant","cjk-ja","cjk-ko")) {
    & .\build\Release\litepdf-cli.exe "tests\fixtures\$f.pdf" --render 0 > "$f.ppm"
    python -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$f.ppm" "tests\fixtures\cjk-reference\$f.png"
    Remove-Item "$f.ppm"
}
Pop-Location
```
Open each PNG and confirm real CJK glyphs. Attach before/after to the PR.

- [ ] **Step 7: GUI smoke (per `reference_litepdf_scripted_gui_smoke`)**

Launch the GUI on the zh-Hant fixture, screenshot, confirm real glyphs render on screen:
```powershell
Push-Location C:\Users\User\projects\litepdf
$env:LITEPDF_NO_RESTORE = "1"
& .\build\Release\litepdf.exe tests\fixtures\cjk-zh-hant.pdf
Pop-Location
```
Drive + screenshot per the scripted-GUI-smoke memory; confirm the page shows real Traditional Chinese glyphs.

- [ ] **Step 8: Commit**

```bash
git add tests/unit/test_cjk_extract_liveness.cpp tests/CMakeLists.txt scripts/generate-cjk-form-fixture.py tests/fixtures/cjk-form.pdf tests/fixtures/cjk-reference/cjk-zh-hant.png tests/fixtures/cjk-reference/cjk-ja.png tests/fixtures/cjk-reference/cjk-ko.png
git commit -m "test(fonts): CJK extract liveness + form-appearance fixture + refreshed reference PNGs"
```

---

## Task 6: Re-baseline the size gates

**Files:**
- Modify: `benchmarks/thresholds.json`
- Modify: `scripts/check-benchmark-regression.ps1` (the `-SelfTest` synthetics)
- Modify: `scripts/smoke-test.ps1`

> Let `M` = the measured Release exe size from Task 4 Step 4. Set `CEIL = M + 1_500_000` (≈ 9,000,000 if M ≈ 7.5 MB), rounded to a clean value. Use `CEIL` below.

- [ ] **Step 1: Lower the absolute ceiling**

In `benchmarks/thresholds.json`, change `size_ceiling_bytes` from `19000000` to `CEIL`:
```json
{"time_regression_pct": 10, "time_min_delta_ms": 5, "size_regression_bytes": 262144, "size_ceiling_bytes": 9000000}
```
(use the computed `CEIL`).

- [ ] **Step 2: Rebase ALL `-SelfTest` synthetic exe_bytes (not just assertion 8)**

In `scripts/check-benchmark-regression.ps1`, the self-test (lines ~123-172) hardcodes `exe_bytes ≈ 18,000,000` in the timing/delta assertions (1-5) and `18900000/19050000` in assertion 8. Because `Invoke-BenchmarkCompare` checks `$prBytes -gt size_ceiling_bytes` on every call, the ~18 MB values would now exceed `CEIL` and flip the "allowed" assertions 2/3/5 to failed. Replace every synthetic `exe_bytes` so the timing/delta assertions sit **well under** `CEIL` and only assertion 8 straddles it. Concretely (assuming `CEIL = 9000000`), rewrite lines 124-172 using a `~8,000,000` base:
```powershell
    # 1: +15% delta, above the absolute min-delta -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 8000000) (New-SyntheticResult 115 8000000) $thr
    Check ($r.Failed -eq $true) "1: +15% (> min-delta) blocks"

    # 2: +5% delta -> allowed (under the pct threshold).
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 8000000) (New-SyntheticResult 105 8000000) $thr
    Check ($r.Failed -eq $false) "2: +5% allowed"

    # 3: +50% but tiny absolute (4 ms < 5 ms floor) -> allowed.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 8 8000000) (New-SyntheticResult 12 8000000) $thr
    Check ($r.Failed -eq $false) "3: +50% but < min-delta allowed"

    # 4: exe growth beyond the 256 KB tolerance -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 8000000) (New-SyntheticResult 100 8300000) $thr
    Check ($r.Failed -eq $true) "4: exe +300000 B blocks"

    # 5: exe growth within tolerance -> allowed.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 8000000) (New-SyntheticResult 100 8100000) $thr
    Check ($r.Failed -eq $false) "5: exe +100000 B allowed"
```
Then assertions 6 and 7 (the validation-error cases) keep their `18000000` literals **or** drop them to `8000000` for consistency — they throw before the ceiling check so either works; prefer `8000000` for tidiness. Finally assertion 8 straddles `CEIL`:
```powershell
    # 8: PR within the per-PR delta but ABOVE the absolute ceiling -> blocked.
    $r = Invoke-BenchmarkCompare (New-SyntheticResult 100 8900000) (New-SyntheticResult 100 9050000) $thr
    Check ($r.Failed -eq $true) "8: exe above size_ceiling_bytes blocks"
```
(For a different `CEIL`, use `CEIL-100000` / `CEIL+50000` for assertion 8 and keep the timing assertions ≥ ~1 MB under `CEIL`.) Keep all edits 5.1-parse-safe (no `?.`/`??`/ternary).

- [ ] **Step 3: Lower the smoke-test `$maxBytes` value AND its comment**

In `scripts/smoke-test.ps1` (lines 25-27), update both the comment and the value:
```powershell
# cjk-system-font-loader: dropped the embedded CJK font (TOFU_CJK); CJK now
# renders from Windows system fonts. Release build ~7.5 MB. 12 MB leaves headroom.
$maxBytes = 12MB
```

- [ ] **Step 4: Run the self-test — verify 8/8**

```powershell
Push-Location C:\Users\User\projects\litepdf
& powershell -NoProfile -File scripts\check-benchmark-regression.ps1 -SelfTest
Pop-Location
```
Expected: prints all 8 `[PASS]` lines and exits 0. (Equivalently `ctest -R benchmark_selftest`.)

- [ ] **Step 5: Commit**

```bash
git add benchmarks/thresholds.json scripts/check-benchmark-regression.ps1 scripts/smoke-test.ps1
git commit -m "ci(size): re-baseline gates to post-TOFU_CJK exe (~7.5 MB)"
```

---

## Task 7: Docs — mark C-real / 8 MB done

**Files:**
- Modify: `docs/plans/2026-04-15-litepdf-roadmap.md`

- [ ] **Step 1: Update the roadmap "Out of Scope" / 8 MB notes**

In `docs/plans/2026-04-15-litepdf-roadmap.md`, the design's §3 8 MB target and the Phase 11.5 "C-real" deferral are now realized. Add a short note under the relevant section recording: exe `M` bytes (from Task 4), CJK now renders from Windows system fonts via DirectWrite, and the accepted known limitation:
```markdown
- **8 MB exe target — DONE (cjk-system-font-loader, 2026-06-21).** The embedded
  Droid Sans Fallback Full CJK font (~4.84 MB) was dropped (`TOFU_CJK`); CJK now
  renders from Windows system fonts via a DirectWrite `load_cjk_font` hook
  (`src/core/SystemFonts.cpp`). `litepdf.exe` ≈ <M> bytes. **Known limitation:**
  on a Windows install lacking the relevant CJK family (Windows N, debloated, no
  language pack), CJK runs render as `.notdef` tofu — never blank or crashing (a
  base14 last-resort guarantees the page renders). The Han **script-fallback**
  path (a non-CJK base font hitting a CJK codepoint) is out of scope and renders
  notdef, like the non-CJK scripts already tofu since v1.1.0.
```
(Substitute the measured `<M>`.)

- [ ] **Step 2: Commit**

```bash
git add docs/plans/2026-04-15-litepdf-roadmap.md
git commit -m "docs(roadmap): 8 MB exe target reached via CJK system-font loader"
```

---

## Final verification (before opening the PR)

- [ ] Full suite green: `& .\build\tests\Release\litepdf_unit_tests.exe` (run from repo root) → exit 0, no new failures.
- [ ] `ctest -R benchmark_selftest` → 8/8.
- [ ] `scripts\smoke-test.ps1` passes (exe under 12 MB; cold-start under the existing 1500 ms ceiling).
- [ ] `large.pdf` benchmark shows no render regression vs base.
- [ ] Reference PNGs regenerated + before/after attached to the PR; GUI smoke screenshot attached.
- [ ] Version: per project convention, bump `VERSION` at ship time (feature → likely `v1.2.0`); do NOT bump per-PR.

---

## Notes for the implementer

- **fz_try/fz_catch in C++:** the project already mixes these (Document.cpp, RenderEngine.cpp). Keep the `fz_try` blocks in `SystemFonts.cpp` free of C++ objects with non-trivial destructors (they only call `fz_*`), so the setjmp/longjmp does not skip destructors — matching project convention.
- **The hook fires only after Task 4.** Before the `TOFU_CJK` flip, the embedded font services CJK and MuPDF never calls `resolve_cjk_system_font`; the Task-2 unit test calls it directly, so it is testable before the flip.
- **CI runner fonts:** the windows-2022 runner has CJK fonts, so the D6 last-resort branch never fires there — the deterministic guard is the Task-2 unit test (bogus ordering). CI assertions on CJK stay liveness-only (no pixel/hash), per spec §6/§9.
- **Perf fallback (spec §9):** if the benchmark shows a CJK render regression from the per-context `fz_new_font_from_file` whole-file read, switch the cache to hold font bytes process-wide and use `fz_new_buffer_from_shared_data` + `fz_new_font_from_buffer`.
