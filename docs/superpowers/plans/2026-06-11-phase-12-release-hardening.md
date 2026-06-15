# Phase 12: Release Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add crash minidumps, crash-triggered session restore, and a manual smoke pass so LitePDF can ship v1.0.

**Architecture:** Three headless-testable core classes (`AppPaths`, `SessionState` + JSON codec, `SessionStore`) plus `AppRunGuard` live in `litepdf_core`. A thin exe-only Win32 shim (`CrashHandler`, `SetUnhandledExceptionFilter` → `MiniDumpWriteDump`) wires into `wWinMain`. `MainWindow` gains `capture_session()`, a sequential restore orchestrator, a debounced auto-save timer, and a single `on_clean_exit()` path shared by `WM_DESTROY` and `WM_ENDSESSION`. Restore is **prompt-after-abnormal-exit only** (a normal close never restores). Session persists to `%LOCALAPPDATA%\LitePDF\session.json`; minidumps to `%LOCALAPPDATA%\LitePDF\crashes\`. No third-party JSON dependency — a small fail-safe hand-rolled codec keeps the 19 MB binary-size gate untouched.

**Accepted tradeoffs:** (1) session auto-save is debounced ~1.5 s, so up to ~1.5 s of the most recent state can be lost if the process is killed mid-debounce — deliberate (avoids disk thrash); discrete low-frequency events (tab open/close/switch, page settle, zoom change, window move/resize) all schedule a save, and a clean exit / end-session flushes synchronously. (2) Restore is intentionally **sequential** (one tab opened at a time) for ordering + completion-attribution correctness, so total restore time is additive and an encrypted tab's password prompt pauses the rest of the chain; restore is capped at 24 tabs. A cheap future improvement: order encrypted tabs last so non-encrypted tabs restore before the first password dialog.

**Tech Stack:** C++ (Win32, Direct2D), MuPDF, CMake, Catch2 v3.5.4, dbghelp.dll (`MiniDumpWriteDump`), `SHGetKnownFolderPath`. Inno Setup for the installer prompt change.

---

## Review Hardening (4-lens review incorporated 2026-06-11)

This plan (v2) folds in every verified-real finding from a four-model plan review (Opus + Sonnet + Fable workflow with per-finding adversarial verification, plus an independent Codex/GPT lens). The blocker and the four high-severity items were all in the restore / abnormal-exit machinery — the crash-path code that cannot be meaningfully unit-tested later — so they are fixed in the plan text before execution:

- **BLOCKER** — Task 9 restore rewritten as a **sequential orchestrator** (open one tab at a time, chained off the existing `WM_USER_OPEN_OK` / `WM_USER_OPEN_FAILED` / password-success handlers), applying page+zoom at each insertion point, resolving the active tab by **path** not index, hooking the encrypted-PDF password path, and deferring the CLI `initial_path` open until restore completes.
- **HIGH** — Task 5 `set_zoom_scale` rewritten against the real pimpl (`impl_->zm` / `impl_->scale`; no `kMinZoom`/`kMaxZoom` exist).
- **HIGH** — `AppRunGuard` constructed **after** the single-instance gate (only the message-loop-owning instance owns the marker).
- **HIGH** — `WM_ENDSESSION` handled (shared `on_clean_exit()`) so an OS shutdown/reboot with tabs open does not leave a stale marker → no false restore prompt.
- **HIGH** — zoom-change and window move/resize added to the save-trigger list (spec-named state that otherwise never persists).
- **MEDIUM/LOW** — empty-`app_data_dir` guard (no CWD fallback), file-exists pre-filter on restore, off-screen window-placement guard, atomic write via `MoveFileExW`, `%.9g` float round-trip, bounded crash-dump buffer + re-entrancy guard, `\u` proper UTF-8 decode + surrogate reject, strict UTF-8 conversion, post-parse validation, session-file size cap, version gate, crash-dir retention, `KillTimer` on exit, byte-level installer BOM check, smoke-checklist expansions.

One finding (AppPaths in `src/app/` "namespace collision / shell32 link") was **rejected** on verification: directory location does not determine CMake target, `src/app/` already splits across `litepdf_core` and the exe target, and the `#pragma comment(lib)` mechanism is already used by other core sources.

**v3 (delta re-review of the v2 orchestrator, 4-lens + Codex, per-finding verified):** the sequential restore orchestrator introduced its own bugs, now fixed in the Task 9 / Task 8 text:
- **Completion correlation (HIGH)** — `restore_on_tab_ready(opened)` now USES `opened`, matching it against `restore_pending_path_`; an interleaved user/IPC open (drag-drop, Ctrl+O, MRU, a 2nd instance's `WM_COPYDATA`) mid-restore no longer mis-applies restore state or shifts the queue.
- **Funnel every terminal (HIGH)** — the password post-auth construction `catch` (`:1279`), exhaustion, cancel, and the `PostMessageW(WM_USER_OPEN_OK)`-failure path all now advance the chain (the last by posting `WM_USER_OPEN_FAILED` instead of silently dropping), so `restoring_` can't get stuck true.
- **No mid-restore clobber (HIGH)** — `on_clean_exit()` skips the save AND `mark_clean_exit` while `restoring_`, so closing/shutting down mid-restore re-offers the FULL session next launch instead of overwriting it with a partial capture. The `WM_TIMER` save also guards on `restoring_`, and `restoring_` is set BEFORE `SetWindowPlacement`.
- **MEDIUM/LOW** — deferred `initial_path` replicates the MRU push/canonicalize (no Phase-5 regression) and `run()` drops its eager open (no double-open); fictional `canvas_viewport_*()` replaced with the real `GetClientRect`/`GetDpiForWindow` pattern; the dead `\u` test replaced with real raw-literal + lone-surrogate assertions; the empty-path guard moved into Task 6 code + `std::optional<AppRunGuard>`; `validate()` tightened (active_tab, showCmd whitelist) + `capture_session` stores the filtered active index; `restore_finish` has a deterministic active-tab fallback; `save_session` flushes before the atomic rename; restore is capped at 24 tabs with the count shown in the prompt.

Verification down-graded several first-pass "high"s (the page-0 "flash" is a non-bug — the first render is cancelled before paint; the `PostMessage`-failure stall is near-zero probability) but the cheap fixes were folded in anyway.

---

## Scope

In scope (the three roadmap Phase 12 deliverables):
1. Crash minidump via `SetUnhandledExceptionFilter` → `crashes\litepdf-<pid>-<ticks>.dmp`.
2. Session state (open tab paths + per-tab page + per-tab zoom mode/scale + active tab + window placement) serialized to `session.json`, restored **only after an abnormal exit**, behind a yes/no prompt.
3. A manual smoke checklist, created and executed before the v1.0 tag.

Explicitly OUT of scope (decided 2026-06-11):
- `%LOCALAPPDATA%\LitePDF\log.txt` non-fatal error log (design §6.2) — separate follow-up.
- Restoring intra-page pan/scroll position — design §6.3 lists "page numbers, zoom", not pan.
- Always-restore (Chrome-style) — rejected; design §6.3 says "after abnormal exit".

## Load-Bearing Constraints (read before coding)

- **Binary size CI gate is absolute**: smoke ceiling 25 MB, benchmark `size_ceiling_bytes` = 19,000,000 (self-test asserts 8/8, coupled to this value). Current exe = 12.25 MB. Do NOT add a heavy dependency. `dbghelp.lib` is a system import lib. The JSON codec is hand-rolled for the same reason.
- **`%LOCALAPPDATA%\LitePDF` does not exist today.** `src/` currently has ZERO `SHGetKnownFolderPath` / `LOCALAPPDATA` calls. Creating it ACTIVATES the previously-dormant installer keep-config uninstall prompt (`installer/litepdf.iss` `CurUninstallStepChanged`). Task 10 updates that prompt — REQUIRED.
- **`litepdf_core` vs exe target split**: testable classes (AppPaths, SessionState, SessionStore, AppRunGuard) go in `litepdf_core` (CMakeLists.txt). Directory does not determine target — `src/app/SearchDispatcher.cpp` / `CrossTabSearch.cpp` are already in core while `src/app/SingleInstance.cpp` is exe-only. `CrashHandler` stays exe-only.
- **PowerShell 5.1 floor** for any `.ps1` touched (no `?.`/`??`/ternary). Not expected in this phase.
- **`/utf-8` is set** (`cmake/CompilerFlags.cmake:16`), so the zh-TW `MessageBoxW` literals in Task 9/10 compile correctly.

## File Structure

| File | New/Modify | Responsibility | Target |
|---|---|---|---|
| `src/app/AppPaths.hpp` / `.cpp` | New | Resolve `%LOCALAPPDATA%\LitePDF` + sub-paths; pure composition split from the OS call; crash-dir retention prune | `litepdf_core` |
| `src/core/SessionState.hpp` / `.cpp` | New | Session struct + `to_json` / `from_json` fail-safe codec (strict, validated, UTF-8-correct) | `litepdf_core` |
| `src/core/SessionStore.hpp` / `.cpp` | New | File load/save (UTF-8, atomic via `MoveFileExW`, size-capped read) | `litepdf_core` |
| `src/app/AppRunGuard.hpp` / `.cpp` | New | Running-marker write/clear + abnormal-exit detection | `litepdf_core` |
| `src/app/CrashHandler.hpp` / `.cpp` | New | `SetUnhandledExceptionFilter` → `MiniDumpWriteDump`, bounded + re-entrant-guarded | exe |
| `src/core/DocumentView.hpp` / `.cpp` | Modify | Add `set_zoom_scale(float)` (pimpl `impl_->zm` / `impl_->scale`) | `litepdf_core` |
| `src/ui/MainWindow.hpp` / `.cpp` | Modify | `capture_session()`, restore orchestrator, debounced save, `on_clean_exit()` (WM_DESTROY + WM_ENDSESSION), restore-state members | exe |
| `src/main.cpp` | Modify | Install crash handler + run-guard AFTER the single-instance gate; drive restore; defer `initial_path` | exe |
| `tests/unit/test_app_paths.cpp` | New | AppPaths pure-composition tests | tests |
| `tests/unit/test_session_state.cpp` | New | JSON round-trip + corruption + CJK/UNC/long-path + validation tests | tests |
| `tests/unit/test_session_store.cpp` | New | File round-trip + size-cap + atomic-survive tests | tests |
| `tests/unit/test_run_guard.cpp` | New | Abnormal-exit detection logic | tests |
| `tests/CMakeLists.txt` | Modify | Register the four new test files | tests |
| `CMakeLists.txt` | Modify | Add new core sources to `litepdf_core`; link `dbghelp` to exe | build |
| `installer/litepdf.iss` | Modify | Update keep-config prompt wording (session.json + crashes\) | installer |
| `docs/SMOKE-CHECKLIST.md` | New | The pre-release manual smoke list | docs |
| `CHANGELOG.md`, `VERSION` | Modify | v1.0.0 release (Task 12, via /ship convention) | release |

---

## Task 1: AppPaths — resolve `%LOCALAPPDATA%\LitePDF`

**Files:**
- Create: `src/app/AppPaths.hpp`, `src/app/AppPaths.cpp`
- Test: `tests/unit/test_app_paths.cpp`
- Modify: `CMakeLists.txt` (add `src/app/AppPaths.cpp` to `litepdf_core`), `tests/CMakeLists.txt`

Pure path composition (testable) is split from the OS call. `app_data_dir()` returns an **empty path on failure** — every caller MUST treat empty as "persistence disabled" (Tasks 8/9), never compose against it (an empty base yields CWD-relative paths).

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_app_paths.cpp
#include <catch2/catch_test_macros.hpp>
#include "app/AppPaths.hpp"

using litepdf::app::compose_app_data_dir;
using litepdf::app::session_file_under;
using litepdf::app::crashes_dir_under;
using litepdf::app::running_marker_under;

TEST_CASE("compose_app_data_dir appends LitePDF", "[app][paths]") {
    auto root = std::filesystem::path(L"C:\\Users\\X\\AppData\\Local");
    REQUIRE(compose_app_data_dir(root) ==
            std::filesystem::path(L"C:\\Users\\X\\AppData\\Local\\LitePDF"));
}

TEST_CASE("sub-paths hang off the app data dir", "[app][paths]") {
    auto base = std::filesystem::path(L"D:\\appdata\\LitePDF");
    REQUIRE(session_file_under(base).filename() == L"session.json");
    REQUIRE(crashes_dir_under(base).filename() == L"crashes");
    REQUIRE(running_marker_under(base).filename() == L"running.lock");
    REQUIRE(session_file_under(base).parent_path() == base);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target litepdf_unit_tests` then `litepdf_unit_tests "[app][paths]"`
Expected: FAIL to compile — `app/AppPaths.hpp` not found.

- [ ] **Step 3: Write the header**

```cpp
// src/app/AppPaths.hpp
#pragma once
#include <filesystem>

namespace litepdf::app {

// Pure: <local_appdata_root>/LitePDF. Unit-testable, no OS call.
std::filesystem::path compose_app_data_dir(const std::filesystem::path& local_appdata_root);

// Pure sub-path helpers off an already-composed app data dir.
std::filesystem::path session_file_under(const std::filesystem::path& app_data_dir);
std::filesystem::path crashes_dir_under(const std::filesystem::path& app_data_dir);
std::filesystem::path running_marker_under(const std::filesystem::path& app_data_dir);

// OS-backed: resolve FOLDERID_LocalAppData, append "LitePDF", create it (and
// crashes\). Returns an EMPTY path on failure. Callers MUST treat empty as
// "persistence disabled" and never compose against it. NOT unit-tested.
std::filesystem::path app_data_dir();

// Best-effort: keep only the newest `keep` *.dmp files in crashes_dir.
// Silently no-ops on any error. Call once at startup.
void prune_crash_dumps(const std::filesystem::path& crashes_dir, std::size_t keep);

}  // namespace litepdf::app
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/app/AppPaths.cpp
#include "app/AppPaths.hpp"

#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath
#include <algorithm>
#include <system_error>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace litepdf::app {

std::filesystem::path compose_app_data_dir(const std::filesystem::path& root) {
    return root / L"LitePDF";
}
std::filesystem::path session_file_under(const std::filesystem::path& dir) {
    return dir / L"session.json";
}
std::filesystem::path crashes_dir_under(const std::filesystem::path& dir) {
    return dir / L"crashes";
}
std::filesystem::path running_marker_under(const std::filesystem::path& dir) {
    return dir / L"running.lock";
}

std::filesystem::path app_data_dir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::filesystem::path base(raw);
    CoTaskMemFree(raw);

    std::filesystem::path dir = compose_app_data_dir(base);
    std::error_code ec;
    std::filesystem::create_directories(crashes_dir_under(dir), ec);  // also creates dir itself
    if (ec) return {};
    return dir;
}

void prune_crash_dumps(const std::filesystem::path& crashes_dir, std::size_t keep) {
    std::error_code ec;
    if (!std::filesystem::is_directory(crashes_dir, ec)) return;
    std::vector<std::filesystem::path> dumps;
    for (auto& e : std::filesystem::directory_iterator(crashes_dir, ec)) {
        if (ec) return;
        if (e.is_regular_file(ec) && e.path().extension() == L".dmp")
            dumps.push_back(e.path());
    }
    if (dumps.size() <= keep) return;
    // Filenames embed GetTickCount; lexical sort is good enough to drop oldest.
    std::sort(dumps.begin(), dumps.end());
    for (std::size_t i = 0; i + keep < dumps.size(); ++i)
        std::filesystem::remove(dumps[i], ec);
}

}  // namespace litepdf::app
```

- [ ] **Step 5: Register sources** — `CMakeLists.txt`: add `src/app/AppPaths.cpp` to the `litepdf_core` source list (the lib `litepdf_unit_tests` links, `tests/CMakeLists.txt:74`). `tests/CMakeLists.txt`: add `unit/test_app_paths.cpp` to the `target_sources(litepdf_unit_tests PRIVATE ...)` block. The `#pragma comment(lib, "shell32.lib")` injects the import lib into the test binary on MSVC (same mechanism `ThumbnailPane.cpp`/`PasswordDialog.cpp` already use in core).

- [ ] **Step 6: Run test to verify it passes** — `litepdf_unit_tests "[app][paths]"` → PASS, 2 cases. Also confirm the test binary links cleanly.

- [ ] **Step 7: Commit**

```bash
git add src/app/AppPaths.hpp src/app/AppPaths.cpp tests/unit/test_app_paths.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(app): add AppPaths for %LOCALAPPDATA%\\LitePDF resolution + crash-dir prune"
```

---

## Task 2: SessionState struct + `to_json`

**Files:**
- Create: `src/core/SessionState.hpp`, `src/core/SessionState.cpp`
- Test: `tests/unit/test_session_state.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
// src/core/SessionState.hpp
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litepdf::core {

enum class SessionZoom { FitWidth, FitPage, Custom };

struct SessionTab {
    std::filesystem::path path;
    int page = 0;
    SessionZoom zoom_mode = SessionZoom::FitWidth;
    float zoom_scale = 1.0f;
};

struct SessionWindow {
    int flags = 0;   // WINDOWPLACEMENT.flags
    int show = 1;    // WINDOWPLACEMENT.showCmd (SW_SHOWNORMAL)
    int x = 0, y = 0, w = 0, h = 0;  // rcNormalPosition
};

struct SessionState {
    int version = 1;
    SessionWindow window;
    int active_tab = 0;
    std::vector<SessionTab> tabs;
};

inline constexpr int kSessionVersion = 1;

// Compact UTF-8 JSON. Paths are stored UTF-8 and JSON-escaped.
std::string to_json(const SessionState& s);

// Fail-safe: returns nullopt on ANY malformed input, unsupported version,
// invalid UTF-8, a \u escape outside the BMP-safe set, or a failed invariant
// check. A corrupt session.json must degrade to "no restore", never crash or
// partially apply.
std::optional<SessionState> from_json(std::string_view json);

}  // namespace litepdf::core
```

- [ ] **Step 2: Write the failing serialize test**

```cpp
// tests/unit/test_session_state.cpp
#include <catch2/catch_test_macros.hpp>
#include "core/SessionState.hpp"

using namespace litepdf::core;

TEST_CASE("to_json emits a backslash-escaped Windows path", "[core][session][json]") {
    SessionState s;
    s.window = {0, 1, 100, 80, 1024, 768};
    s.active_tab = 0;
    s.tabs.push_back({std::filesystem::path(L"C:\\docs\\a b.pdf"), 3,
                      SessionZoom::Custom, 1.25f});

    std::string j = to_json(s);
    REQUIRE(j.find("C:\\\\docs\\\\a b.pdf") != std::string::npos);
    REQUIRE(j.find("\"page\":3") != std::string::npos);
    REQUIRE(j.find("\"zoom_mode\":\"custom\"") != std::string::npos);
}
```

- [ ] **Step 3: Run test to verify it fails** — `litepdf_unit_tests "[core][session][json]"` → FAIL (`to_json` undefined).

- [ ] **Step 4: Implement `to_json`**

Note: `zoom_scale` is serialized with `%.9g` (9 significant digits guarantee an exact `float` round-trip; `std::to_string` only emits 6 decimals and corrupts non-power-of-two scales like `1.3f`).

```cpp
// src/core/SessionState.cpp
#include "core/SessionState.hpp"

#include <windows.h>   // WideCharToMultiByte / MultiByteToWideChar
#include <cstdio>      // snprintf
#include <string>

namespace litepdf::core {
namespace {

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    // Source is a live std::filesystem::path (valid UTF-16); WC_ERR_INVALID_CHARS
    // is belt-and-suspenders. On failure return empty (caller path is then "").
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string float_to_str(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)f);
    return buf;
}

void append_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);  // UTF-8 bytes pass through
                }
        }
    }
    out.push_back('"');
}

const char* zoom_to_str(SessionZoom z) {
    switch (z) {
        case SessionZoom::FitWidth: return "fit_width";
        case SessionZoom::FitPage:  return "fit_page";
        case SessionZoom::Custom:   return "custom";
    }
    return "fit_width";
}

}  // namespace

std::string to_json(const SessionState& s) {
    std::string o;
    o.reserve(256);
    o += "{\"version\":" + std::to_string(s.version);
    o += ",\"window\":{";
    o += "\"flags\":" + std::to_string(s.window.flags);
    o += ",\"show\":"  + std::to_string(s.window.show);
    o += ",\"x\":"     + std::to_string(s.window.x);
    o += ",\"y\":"     + std::to_string(s.window.y);
    o += ",\"w\":"     + std::to_string(s.window.w);
    o += ",\"h\":"     + std::to_string(s.window.h);
    o += "}";
    o += ",\"active\":" + std::to_string(s.active_tab);
    o += ",\"tabs\":[";
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const auto& t = s.tabs[i];
        if (i) o.push_back(',');
        o += "{\"path\":";
        append_escaped(o, wide_to_utf8(t.path.wstring()));
        o += ",\"page\":" + std::to_string(t.page);
        o += ",\"zoom_mode\":\"";
        o += zoom_to_str(t.zoom_mode);
        o += "\",\"zoom_scale\":" + float_to_str(t.zoom_scale);
        o += "}";
    }
    o += "]}";
    return o;
}

}  // namespace litepdf::core
```

- [ ] **Step 5: Register sources** (`CMakeLists.txt` → `litepdf_core`; `tests/CMakeLists.txt` → `unit/test_session_state.cpp`).

- [ ] **Step 6: Run test to verify it passes** — PASS.

- [ ] **Step 7: Commit**

```bash
git add src/core/SessionState.hpp src/core/SessionState.cpp tests/unit/test_session_state.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(core): add SessionState struct + to_json serializer"
```

---

## Task 3: `from_json` — fail-safe, validated, UTF-8-correct parser

**Files:**
- Modify: `src/core/SessionState.cpp`, `tests/unit/test_session_state.cpp`

A small recursive-descent parser over the exact subset `to_json` emits. Any unexpected token, unknown key, unsupported version, invalid UTF-8, or out-of-range `\u` throws an internal `ParseError`, caught at the top → `nullopt`. Design choice (deliberate, reviewed): the parser is **strict** — unknown keys are rejected rather than skipped, and an explicit `version != kSessionVersion` gate rejects forward/foreign files. session.json is a single-app, single-machine cache written and read by the same binary; strict-reject IS the intended fail-safe (worst case: one missed restore, never a corrupt one).

- [ ] **Step 1: Write the failing round-trip + corruption + CJK tests**

```cpp
// append to tests/unit/test_session_state.cpp
TEST_CASE("from_json round-trips a two-tab session", "[core][session][json]") {
    SessionState s;
    s.version = 1;
    s.window = {0, 3, 10, 20, 1280, 800};
    s.active_tab = 1;
    s.tabs.push_back({std::filesystem::path(L"C:\\a\\one.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    s.tabs.push_back({std::filesystem::path(L"C:\\b\\two.pdf"), 7,
                      SessionZoom::Custom, 1.5f});

    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->active_tab == 1);
    REQUIRE(r->window.w == 1280);
    REQUIRE(r->tabs.size() == 2);
    REQUIRE(r->tabs[0].path == std::filesystem::path(L"C:\\a\\one.pdf"));
    REQUIRE(r->tabs[1].page == 7);
    REQUIRE(r->tabs[1].zoom_mode == SessionZoom::Custom);
    REQUIRE(r->tabs[1].zoom_scale == 1.5f);
}

TEST_CASE("from_json round-trips a non-exact float zoom", "[core][session][json]") {
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\z.pdf"), 0, SessionZoom::Custom, 1.3f});
    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->tabs[0].zoom_scale == 1.3f);   // exact thanks to %.9g
}

TEST_CASE("from_json round-trips a CJK and a UNC path", "[core][session][json]") {
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\文件\\檔案.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    s.tabs.push_back({std::filesystem::path(L"\\\\server\\share\\b.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->tabs[0].path == std::filesystem::path(L"C:\\文件\\檔案.pdf"));
    REQUIRE(r->tabs[1].path == std::filesystem::path(L"\\\\server\\share\\b.pdf"));
}

TEST_CASE("from_json decodes a uXXXX BMP escape to UTF-8", "[core][session][json]") {
    // A foreign writer may escape CJK as 中; we must decode it, not truncate.
    // Raw literal => the backslash reaches from_json literally.
    auto r = from_json(R"({"version":1,"window":{"flags":0,"show":1,"x":0,"y":0,"w":0,"h":0},"active":0,"tabs":[{"path":"C:\\中.pdf","page":0,"zoom_mode":"fit_width","zoom_scale":1}]})");
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.size() == 1);
    REQUIRE(r->tabs[0].path == std::filesystem::path(L"C:\\中.pdf"));   // C:\中.pdf
}

TEST_CASE("from_json rejects a lone surrogate escape", "[core][session][json]") {
    auto r = from_json(R"({"version":1,"window":{"flags":0,"show":1,"x":0,"y":0,"w":0,"h":0},"active":0,"tabs":[{"path":"\ud800.pdf","page":0,"zoom_mode":"fit_width","zoom_scale":1}]})");
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("from_json rejects malformed / unsupported input as nullopt", "[core][session][json]") {
    REQUIRE_FALSE(from_json("").has_value());
    REQUIRE_FALSE(from_json("{").has_value());
    REQUIRE_FALSE(from_json("not json at all").has_value());
    REQUIRE_FALSE(from_json("{\"version\":1,\"tabs\":[{").has_value());
    REQUIRE_FALSE(from_json("{}garbage").has_value());                 // trailing junk
    REQUIRE_FALSE(from_json("{\"version\":2,\"tabs\":[]}").has_value()); // unsupported version
    REQUIRE_FALSE(from_json("{\"version\":1,\"bogus\":1,\"tabs\":[]}").has_value()); // unknown key
}

TEST_CASE("from_json tolerates an empty-tabs session", "[core][session][json]") {
    SessionState s;  // zero tabs
    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.empty());
}
```

> **Executor note on the `\u` test:** write the escaped JSON with a C++ raw string literal `R"({...中...})"` so the backslash reaches `from_json` literally, and assert `r->tabs[0].path == std::filesystem::path(L"C:\\中.pdf")`. The placeholder above is illustrative; replace it with a real raw-literal assertion. Also add a surrogate-range case: a lone `\ud800` must yield `nullopt`.

- [ ] **Step 2: Run to verify it fails** — `litepdf_unit_tests "[core][session][json]"` → FAIL.

- [ ] **Step 3: Implement the parser**

Add `#include <cmath>` and `#include <cstdlib>` to the top of `SessionState.cpp`. Add to the anonymous namespace:

```cpp
// Strict UTF-8 -> UTF-16. Returns false on invalid UTF-8 (caller fails the parse).
bool utf8_to_wide_strict(const std::string& s, std::wstring& out) {
    if (s.empty()) { out.clear(); return true; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out.assign(n, L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(),
                               out.data(), n) > 0;
}

struct ParseError {};

class Json {
public:
    explicit Json(std::string_view in) : s_(in) {}

    void parse_root_object_into(SessionState& out) {
        ws();
        parse_object(out);
        ws();
        if (i_ != s_.size()) throw ParseError{};   // reject trailing junk
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    [[noreturn]] void fail() { throw ParseError{}; }
    void ws() { while (i_ < s_.size() && (s_[i_]==' '||s_[i_]=='\t'||s_[i_]=='\n'||s_[i_]=='\r')) ++i_; }
    char peek() { if (i_ >= s_.size()) fail(); return s_[i_]; }
    char get()  { if (i_ >= s_.size()) fail(); return s_[i_++]; }
    void expect(char c) { if (get() != c) fail(); }

    int hex4() {
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = get(); v <<= 4;
            if (h>='0'&&h<='9') v |= h-'0';
            else if (h>='a'&&h<='f') v |= h-'a'+10;
            else if (h>='A'&&h<='F') v |= h-'A'+10;
            else fail();
        }
        return v;
    }

    // Append codepoint cp (BMP only here) as UTF-8 bytes.
    static void append_utf8(std::string& r, unsigned cp) {
        if (cp < 0x80) { r.push_back((char)cp); }
        else if (cp < 0x800) {
            r.push_back((char)(0xC0 | (cp >> 6)));
            r.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            r.push_back((char)(0xE0 | (cp >> 12)));
            r.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            r.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string r;
        for (;;) {
            char c = get();
            if (c == '"') return r;
            if (c == '\\') {
                char e = get();
                switch (e) {
                    case '"': r.push_back('"'); break;
                    case '\\': r.push_back('\\'); break;
                    case '/': r.push_back('/'); break;
                    case 'b': r.push_back('\b'); break;
                    case 'f': r.push_back('\f'); break;
                    case 'n': r.push_back('\n'); break;
                    case 'r': r.push_back('\r'); break;
                    case 't': r.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = (unsigned)hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {        // high surrogate
                            if (get() != '\\' || get() != 'u') fail();
                            unsigned lo = (unsigned)hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) fail();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            // 4-byte UTF-8
                            r.push_back((char)(0xF0 | (cp >> 18)));
                            r.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                            r.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            r.push_back((char)(0x80 | (cp & 0x3F)));
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {  // lone low surrogate
                            fail();
                        } else {
                            append_utf8(r, cp);
                        }
                        break;
                    }
                    default: fail();
                }
            } else {
                r.push_back(c);  // raw UTF-8 bytes pass through
            }
        }
    }

    long parse_int() {
        ws();
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_]=='-'||s_[i_]=='+')) ++i_;
        while (i_ < s_.size() && s_[i_]>='0' && s_[i_]<='9') ++i_;
        if (i_ == start) fail();
        return std::strtol(std::string(s_.substr(start, i_-start)).c_str(), nullptr, 10);
    }

    double parse_number() {
        ws();
        size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E') ++i_;
            else break;
        }
        if (i_ == start) fail();
        return std::strtod(std::string(s_.substr(start, i_-start)).c_str(), nullptr);
    }

    SessionZoom parse_zoom() {
        std::string z = parse_string();
        if (z == "fit_width") return SessionZoom::FitWidth;
        if (z == "fit_page")  return SessionZoom::FitPage;
        if (z == "custom")    return SessionZoom::Custom;
        fail();
    }

    void parse_window(SessionWindow& w) {
        expect('{');
        for (;;) {
            ws();
            std::string key = parse_string();
            ws(); expect(':');
            long v = parse_int();
            if (key=="flags") w.flags=(int)v;
            else if (key=="show") w.show=(int)v;
            else if (key=="x") w.x=(int)v;
            else if (key=="y") w.y=(int)v;
            else if (key=="w") w.w=(int)v;
            else if (key=="h") w.h=(int)v;
            else fail();
            ws();
            char c = get();
            if (c=='}') break;
            if (c!=',') fail();
        }
    }

    void parse_tab(SessionTab& t) {
        expect('{');
        for (;;) {
            ws();
            std::string key = parse_string();
            ws(); expect(':');
            if (key=="path") {
                std::wstring w;
                if (!utf8_to_wide_strict(parse_string(), w)) fail();
                t.path = std::filesystem::path(w);
            }
            else if (key=="page") t.page = (int)parse_int();
            else if (key=="zoom_mode") t.zoom_mode = parse_zoom();
            else if (key=="zoom_scale") t.zoom_scale = (float)parse_number();
            else fail();
            ws();
            char c = get();
            if (c=='}') break;
            if (c!=',') fail();
        }
    }

    void parse_tabs(std::vector<SessionTab>& tabs) {
        expect('[');
        ws();
        if (peek()==']') { get(); return; }
        for (;;) {
            ws();
            SessionTab t;
            parse_tab(t);
            tabs.push_back(std::move(t));
            ws();
            char c = get();
            if (c==']') break;
            if (c!=',') fail();
        }
    }

    void parse_object(SessionState& out) {
        expect('{');
        for (;;) {
            ws();
            std::string key = parse_string();
            ws(); expect(':');
            if (key=="version") out.version = (int)parse_int();
            else if (key=="window") parse_window(out.window);
            else if (key=="active") out.active_tab = (int)parse_int();
            else if (key=="tabs") parse_tabs(out.tabs);
            else fail();
            ws();
            char c = get();
            if (c=='}') break;
            if (c!=',') fail();
        }
    }
};

// Post-parse invariant validation. Returns false to reject the whole file.
bool validate(const SessionState& s) {
    if (s.version != kSessionVersion) return false;
    // active_tab must index a real tab. Reject a negative active_tab whenever
    // there are tabs to restore (a stray -1 alongside non-empty tabs is the
    // shutdown-race case); a zero-tab session with active_tab 0 is benign.
    if (s.tabs.empty()) {
        if (s.active_tab != 0 && s.active_tab != -1) return false;
    } else if (s.active_tab < 0 || s.active_tab >= (int)s.tabs.size()) {
        return false;
    }
    for (const auto& t : s.tabs) {
        if (t.page < 0) return false;
        if (!std::isfinite(t.zoom_scale) || t.zoom_scale <= 0.0f) return false;
    }
    // Window dims: 0 means "unset" (open at default); negative is corrupt.
    if (s.window.w < 0 || s.window.h < 0) return false;
    return true;
}
```

Then the public function (in `namespace litepdf::core`, NOT the anonymous namespace):

```cpp
std::optional<SessionState> from_json(std::string_view json) {
    try {
        SessionState s;
        Json(json).parse_root_object_into(s);
        if (!validate(s)) return std::nullopt;
        return s;
    } catch (const ParseError&) {
        return std::nullopt;
    }
}
```

- [ ] **Step 4: Run to verify it passes** — all round-trip + corruption + CJK/UNC + version + `\u` + surrogate cases PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/SessionState.cpp tests/unit/test_session_state.cpp
git commit -m "feat(core): add fail-safe validated from_json parser (UTF-8-correct, version-gated)"
```

---

## Task 4: SessionStore — atomic file load/save with size cap

**Files:**
- Create: `src/core/SessionStore.hpp`, `src/core/SessionStore.cpp`
- Test: `tests/unit/test_session_store.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Save writes to `*.tmp` then atomically replaces via `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` — on failure the existing `session.json` is left intact (a stale-but-valid session beats none) and the tmp is cleaned up. Load rejects files above a small size cap before parsing (a corrupt/hostile huge file must not waste startup memory).

- [ ] **Step 1: Write the header**

```cpp
// src/core/SessionStore.hpp
#pragma once
#include <filesystem>
#include <optional>
#include "core/SessionState.hpp"

namespace litepdf::core {

inline constexpr unsigned kMaxSessionBytes = 1u << 20;  // 1 MB

// Returns false on any I/O failure (and leaves any existing file untouched).
bool save_session(const std::filesystem::path& file, const SessionState& s);

// Returns nullopt if the file is missing, oversized, unreadable, or malformed.
std::optional<SessionState> load_session(const std::filesystem::path& file);

}  // namespace litepdf::core
```

- [ ] **Step 2: Write the failing test**

```cpp
// tests/unit/test_session_store.cpp
#include <catch2/catch_test_macros.hpp>
#include "core/SessionStore.hpp"
#include <filesystem>
#include <fstream>

using namespace litepdf::core;

static std::filesystem::path temp_session() {
    auto p = std::filesystem::temp_directory_path() / L"litepdf_test_session.json";
    std::error_code ec; std::filesystem::remove(p, ec);
    return p;
}

TEST_CASE("save then load round-trips", "[core][session][store]") {
    auto file = temp_session();
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\x\\y.pdf"), 4, SessionZoom::FitPage, 1.0f});

    REQUIRE(save_session(file, s));
    auto r = load_session(file);
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.size() == 1);
    REQUIRE(r->tabs[0].page == 4);

    std::error_code ec; std::filesystem::remove(file, ec);
}

TEST_CASE("load of a missing file is nullopt", "[core][session][store]") {
    auto file = std::filesystem::temp_directory_path() / L"litepdf_does_not_exist.json";
    std::error_code ec; std::filesystem::remove(file, ec);
    REQUIRE_FALSE(load_session(file).has_value());
}

TEST_CASE("load rejects an oversized file", "[core][session][store]") {
    auto file = temp_session();
    { std::ofstream o(file, std::ios::binary); std::string big(kMaxSessionBytes + 1, 'x'); o.write(big.data(), big.size()); }
    REQUIRE_FALSE(load_session(file).has_value());
    std::error_code ec; std::filesystem::remove(file, ec);
}

TEST_CASE("save leaves the prior file intact when it cannot replace", "[core][session][store]") {
    // Make the destination a directory so MoveFileExW fails; the existing
    // good content must survive (here: the directory stays, no data clobbered).
    auto dir = std::filesystem::temp_directory_path() / L"litepdf_locked.json";
    std::error_code ec; std::filesystem::create_directory(dir, ec);
    SessionState s;
    REQUIRE_FALSE(save_session(dir, s));   // cannot overwrite a directory
    REQUIRE(std::filesystem::is_directory(dir, ec));
    std::filesystem::remove(dir, ec);
}
```

- [ ] **Step 3: Run to verify it fails** — FAIL (header not found).

- [ ] **Step 4: Implement**

```cpp
// src/core/SessionStore.cpp
#include "core/SessionStore.hpp"

#include <windows.h>   // MoveFileExW
#include <fstream>
#include <sstream>
#include <system_error>

namespace litepdf::core {

bool save_session(const std::filesystem::path& file, const SessionState& s) {
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        std::string json = to_json(s);
        out.write(json.data(), (std::streamsize)json.size());
        out.flush();   // surface a disk-full/quota failure NOW, not silently at dtor close
        if (!out) { out.close(); std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
    }
    // Atomic replace. On failure, the original file is untouched; drop the tmp.
    if (!MoveFileExW(tmp.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec; std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<SessionState> load_session(const std::filesystem::path& file) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(file, ec);
    if (ec) return std::nullopt;                 // missing / unstat-able
    if (sz > kMaxSessionBytes) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return from_json(ss.str());
}

}  // namespace litepdf::core
```

- [ ] **Step 5: Register sources** (`litepdf_core`; `unit/test_session_store.cpp`).

- [ ] **Step 6: Run to verify it passes** — PASS.

- [ ] **Step 7: Commit**

```bash
git add src/core/SessionStore.hpp src/core/SessionStore.cpp tests/unit/test_session_store.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(core): add SessionStore atomic (MoveFileEx) load/save with size cap"
```

---

## Task 5: `DocumentView::set_zoom_scale(float)` — exact Custom restore

**Files:**
- Modify: `src/core/DocumentView.hpp`, `src/core/DocumentView.cpp`, `tests/unit/test_document_view.cpp`

`DocumentView` is pimpl-based: zoom state is `impl_->zm` (`ZoomMode`) and `impl_->scale` (`float`) — see `DocumentView.cpp:48-49`, accessors `:196-202`. There are **NO** `kMinZoom`/`kMaxZoom` constants; the only zoom bounds are the discrete `Impl::kPresets = {0.5f .. 4.0f}` table (`:57-59`) used by `zoom_in`/`zoom_out`. `set_zoom_mode(Custom, …)` deliberately does NOT touch `scale` (`:242-244`), so a restored exact scale survives a later `kick_render`.

- [ ] **Step 1: Write the failing test**

```cpp
// append to tests/unit/test_document_view.cpp
TEST_CASE("set_zoom_scale sets Custom mode and clamps to preset span", "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_scale(1.75f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE(view.zoom_scale() == Catch::Approx(1.75f));

    view.set_zoom_scale(99.0f);                 // above preset max
    REQUIRE(view.zoom_scale() == Catch::Approx(4.0f));
    view.set_zoom_scale(0.01f);                 // below preset min
    REQUIRE(view.zoom_scale() == Catch::Approx(0.5f));
}
```

(Confirm the `ZoomMode` qualifier matches how `test_document_view.cpp` already refers to it; include `<catch2/catch_approx.hpp>`.)

- [ ] **Step 2: Run to verify it fails** — FAIL (no `set_zoom_scale`).

- [ ] **Step 3: Declare in header** (next to `set_zoom_mode`):

```cpp
// Directly set a Custom zoom at an exact scale (used by session restore).
// Clamps to the preset span [0.5, 4.0]. Does not require viewport dims.
void set_zoom_scale(float scale) noexcept;
```

- [ ] **Step 4: Implement** against the real pimpl (clamp to the preset table's ends):

```cpp
void DocumentView::set_zoom_scale(float scale) noexcept {
    const float lo = Impl::kPresets.front();   // 0.5f
    const float hi = Impl::kPresets.back();    // 4.0f
    if (scale < lo) scale = lo;
    if (scale > hi) scale = hi;
    impl_->zm    = ZoomMode::Custom;
    impl_->scale = scale;
}
```

(`impl_` is always non-null post-construction, so `noexcept` is safe. If `kPresets` is a `std::array`, `.front()/.back()` are `constexpr`; if it's a C array, use `kPresets[0]` / `kPresets[N-1]`.)

- [ ] **Step 5: Run to verify it passes** — PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/DocumentView.hpp src/core/DocumentView.cpp tests/unit/test_document_view.cpp
git commit -m "feat(core): add DocumentView::set_zoom_scale (pimpl, preset-clamped)"
```

---

## Task 6: AppRunGuard — abnormal-exit detection

**Files:**
- Create: `src/app/AppRunGuard.hpp`, `src/app/AppRunGuard.cpp`
- Test: `tests/unit/test_run_guard.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

A running-marker file. **Lifecycle contract (load-bearing):** the marker must be created ONLY by the instance that actually runs the message loop — i.e. `AppRunGuard` is constructed AFTER the single-instance gate (Task 9 Step 1). A forwarded second instance must never touch it.

- [ ] **Step 1: Write the header**

```cpp
// src/app/AppRunGuard.hpp
#pragma once
#include <filesystem>

namespace litepdf::app {

class AppRunGuard {
public:
    // Inspects `marker`: if present => previous exit was abnormal. Then
    // creates/refreshes the marker so THIS run is tracked. Construct ONLY in
    // the primary (message-loop-owning) instance, after the single-instance gate.
    explicit AppRunGuard(std::filesystem::path marker);

    bool previous_exit_was_abnormal() const noexcept { return prev_abnormal_; }

    // Mark THIS run as a clean exit (removes the marker). Idempotent — safe to
    // call from both WM_DESTROY and WM_ENDSESSION.
    void mark_clean_exit() noexcept;

private:
    std::filesystem::path marker_;
    bool prev_abnormal_ = false;
};

}  // namespace litepdf::app
```

- [ ] **Step 2: Write the failing test**

```cpp
// tests/unit/test_run_guard.cpp
#include <catch2/catch_test_macros.hpp>
#include "app/AppRunGuard.hpp"
#include <filesystem>
#include <fstream>

using litepdf::app::AppRunGuard;

static std::filesystem::path marker_path() {
    auto p = std::filesystem::temp_directory_path() / L"litepdf_test_run.lock";
    std::error_code ec; std::filesystem::remove(p, ec);
    return p;
}

TEST_CASE("fresh start is not abnormal; clean exit removes marker", "[app][runguard]") {
    auto m = marker_path();
    AppRunGuard g(m);
    REQUIRE_FALSE(g.previous_exit_was_abnormal());
    REQUIRE(std::filesystem::exists(m));
    g.mark_clean_exit();
    REQUIRE_FALSE(std::filesystem::exists(m));
    g.mark_clean_exit();   // idempotent
}

TEST_CASE("leftover marker => previous exit abnormal", "[app][runguard]") {
    auto m = marker_path();
    { std::ofstream(m) << "x"; }   // simulate a crash leaving the marker
    AppRunGuard g(m);
    REQUIRE(g.previous_exit_was_abnormal());
    std::error_code ec; std::filesystem::remove(m, ec);
}

TEST_CASE("empty marker path => no file, never abnormal", "[app][runguard]") {
    AppRunGuard g(std::filesystem::path{});   // persistence-disabled construction
    REQUIRE_FALSE(g.previous_exit_was_abnormal());
    g.mark_clean_exit();   // must not throw or create a "" file
}
```

- [ ] **Step 3: Run to verify it fails** — FAIL.

- [ ] **Step 4: Implement**

```cpp
// src/app/AppRunGuard.cpp
#include "app/AppRunGuard.hpp"
#include <fstream>
#include <system_error>

namespace litepdf::app {

AppRunGuard::AppRunGuard(std::filesystem::path marker) : marker_(std::move(marker)) {
    if (marker_.empty()) return;   // persistence disabled: no marker, never abnormal
    std::error_code ec;
    prev_abnormal_ = std::filesystem::exists(marker_, ec) && !ec;
    std::ofstream(marker_, std::ios::binary | std::ios::trunc) << "running";
}

void AppRunGuard::mark_clean_exit() noexcept {
    if (marker_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(marker_, ec);
}

}  // namespace litepdf::app
```

- [ ] **Step 5: Register sources** (`litepdf_core`, test file).

- [ ] **Step 6: Run to verify it passes** — PASS.

- [ ] **Step 7: Commit**

```bash
git add src/app/AppRunGuard.hpp src/app/AppRunGuard.cpp tests/unit/test_run_guard.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(app): add AppRunGuard abnormal-exit detection"
```

---

## Task 7: CrashHandler — minidump on unhandled exception

**Files:**
- Create: `src/app/CrashHandler.hpp`, `src/app/CrashHandler.cpp`
- Modify: `CMakeLists.txt` (exe target, link `dbghelp`)

NOT unit-testable. Hardened for the corrupted-process reality: the dump path is **prebuilt at install time** into a fixed buffer (no formatting of the directory in the filter), a `StringCchPrintfW` bounded append avoids `MAX_PATH` overflow, and an `InterlockedExchange` re-entrancy guard stops infinite recursion if `MiniDumpWriteDump` itself faults. Stack-overflow dumps remain best-effort (the filter runs on the exhausted stack).

- [ ] **Step 1: Write the header**

```cpp
// src/app/CrashHandler.hpp
#pragma once
#include <filesystem>

namespace litepdf::app {

// Installs SetUnhandledExceptionFilter. On an unhandled exception, writes a
// minidump to <crashes_dir>\litepdf-<pid>-<ticks>.dmp, then returns
// EXCEPTION_CONTINUE_SEARCH so WER/debuggers still run. Call once, early in
// the primary instance. No-op if crashes_dir is empty.
void install_crash_handler(const std::filesystem::path& crashes_dir);

}  // namespace litepdf::app
```

- [ ] **Step 2: Implement**

```cpp
// src/app/CrashHandler.cpp
#include "app/CrashHandler.hpp"

#include <windows.h>
#include <dbghelp.h>     // MiniDumpWriteDump
#include <strsafe.h>     // StringCchPrintfW (bounded)

#pragma comment(lib, "dbghelp.lib")

namespace litepdf::app {
namespace {

// Prebuilt at install time: "<crashes_dir>\" prefix in a fixed buffer, so the
// filter only appends a short bounded suffix and never formats the directory.
wchar_t g_prefix[1024] = {0};
LONG    g_in_filter = 0;

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_in_filter, 1)) return EXCEPTION_CONTINUE_SEARCH;  // re-entrancy
    if (g_prefix[0]) {
        wchar_t name[1024];
        if (SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), L"%slitepdf-%lu-%lu.dmp",
                                       g_prefix, GetCurrentProcessId(), GetTickCount()))) {
            HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;
                const auto type = (MINIDUMP_TYPE)(MiniDumpNormal |
                                                  MiniDumpWithThreadInfo |
                                                  MiniDumpWithUnloadedModules);
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  f, type, &mei, nullptr, nullptr);
                CloseHandle(f);
            }
        }
    }
    // NB: best-effort. On heap/stack corruption (esp. EXCEPTION_STACK_OVERFLOW)
    // the dump may fail to write — do NOT add stack-hungry work here.
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_handler(const std::filesystem::path& crashes_dir) {
    if (crashes_dir.empty()) return;
    std::wstring p = crashes_dir.wstring();
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    StringCchCopyW(g_prefix, ARRAYSIZE(g_prefix), p.c_str());  // truncates safely if huge
    SetUnhandledExceptionFilter(on_unhandled);
}

}  // namespace litepdf::app
```

- [ ] **Step 3: Register + link** — add `src/app/CrashHandler.cpp` to the exe target in `CMakeLists.txt`; add `dbghelp` to its `target_link_libraries`.

- [ ] **Step 4: Manual verification (no unit test)**

Add a temporary `#if defined(_DEBUG)` trigger (e.g. `--crashtest` arg) doing `*(volatile int*)0 = 0;`. Build Debug, run, trigger; confirm `%LOCALAPPDATA%\LitePDF\crashes\litepdf-*.dmp` is written and opens in VS/WinDbg with a resolvable stack. REMOVE the trigger before committing. Also confirm `prune_crash_dumps` (wired at startup, Task 9 Step 1) keeps only the newest N.

- [ ] **Step 5: Commit**

```bash
git add src/app/CrashHandler.hpp src/app/CrashHandler.cpp CMakeLists.txt
git commit -m "feat(app): write minidump on unhandled exception (bounded, re-entrant-guarded)"
```

---

## Task 8: MainWindow — capture, debounced auto-save, shared clean-exit

**Files:**
- Modify: `src/ui/MainWindow.hpp`, `src/ui/MainWindow.cpp`

> **Executor note:** confirm the signatures: `tabs_->count()` / `active_index()` / `tab_at(i)` → `core::Tab*` with `->path` and `->view.get()`; per-view `current_page()`, `zoom_mode()`, `zoom_scale()` (`DocumentView.hpp:78-82`). Window geometry via `GetWindowPlacement(hwnd_, &wp)`.

All restore-related members are declared HERE (Task 8) so this task builds and commits standalone — Task 9 only assigns `run_guard_` from `wWinMain`.

- [ ] **Step 1: Declare members + helpers**

In `MainWindow.hpp` private section:

```cpp
core::SessionState capture_session() const;
void schedule_session_save();   // debounced; no-op if persistence disabled or restoring
void on_clean_exit();           // flush save + clear marker; shared by WM_DESTROY & WM_ENDSESSION

std::filesystem::path             app_data_dir_;     // empty => persistence disabled
litepdf::app::AppRunGuard*        run_guard_ = nullptr;  // non-owning; lives in wWinMain past app.run()
bool                              restoring_ = false;    // suppresses auto-save during restore
// ... restore-orchestrator members are added in Task 9 Step 2 ...
static constexpr UINT_PTR kSessionSaveTimer = 0xC0DE;  // confirm no collision (grep SetTimer ids)
```

Add `#include "app/AppRunGuard.hpp"`, `#include "app/AppPaths.hpp"`, `#include "core/SessionStore.hpp"`. Initialize `app_data_dir_ = litepdf::app::app_data_dir();` in the MainWindow constructor (or have `wWinMain` pass it in — match the existing construction pattern).

- [ ] **Step 2: Implement `capture_session()`**

```cpp
core::SessionState MainWindow::capture_session() const {
    core::SessionState s;
    s.version = core::kSessionVersion;

    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        s.window.flags = (int)wp.flags;
        s.window.show  = (int)wp.showCmd;
        s.window.x = wp.rcNormalPosition.left;
        s.window.y = wp.rcNormalPosition.top;
        s.window.w = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
        s.window.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
    // active_tab is stored as the index WITHIN the filtered (non-empty-path) tab
    // list we actually emit, NOT the raw TabManager index — so validate()'s
    // range check is meaningful and restore lands on the right document.
    const int raw_active = tabs_->active_index();
    s.active_tab = -1;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* tab = tabs_->tab_at(i);
        if (!tab || tab->path.empty()) continue;
        if (i == raw_active) s.active_tab = (int)s.tabs.size();
        core::SessionTab st;
        st.path = tab->path;
        if (auto* v = tab->view.get()) {
            st.page = v->current_page();
            switch (v->zoom_mode()) {
                case core::DocumentView::ZoomMode::FitWidth: st.zoom_mode = core::SessionZoom::FitWidth; break;
                case core::DocumentView::ZoomMode::FitPage:  st.zoom_mode = core::SessionZoom::FitPage;  break;
                case core::DocumentView::ZoomMode::Custom:   st.zoom_mode = core::SessionZoom::Custom;   break;
            }
            st.zoom_scale = v->zoom_scale();
        }
        s.tabs.push_back(std::move(st));
    }
    if (s.active_tab < 0 && !s.tabs.empty()) s.active_tab = 0;  // keep validate() satisfied
    return s;
}
```

- [ ] **Step 3: Implement the debounced save + trigger wiring**

```cpp
void MainWindow::schedule_session_save() {
    if (app_data_dir_.empty() || restoring_) return;   // disabled / mid-restore
    SetTimer(hwnd_, kSessionSaveTimer, 1500, nullptr);  // coalesces bursts
}
```

In the `WM_TIMER` handler, when `wParam == kSessionSaveTimer`: `KillTimer(hwnd_, kSessionSaveTimer);` then — as defense in depth, because a timer can be armed (e.g. by the `SetWindowPlacement`-induced `WM_SIZE` during restore) *before* `restoring_` flips — `if (restoring_ || app_data_dir_.empty()) return 0;` and only then `core::save_session(app::session_file_under(app_data_dir_), capture_session());`. Without this guard a debounced save firing mid-restore overwrites `session.json` with a partial capture.

Call `schedule_session_save()` at the end of these handlers (grep each): **tab opened** (after insert in `WM_USER_OPEN_OK`), **tab closed**, **tab switched** (`on_tab_switch`), **page-nav settle**, **zoom change** (Ctrl +/- and fit-mode toggles — the same handlers that call `set_zoom_mode`/`zoom_in`/`zoom_out`), and **window move/resize** (`WM_EXITSIZEMOVE`, and `WM_SIZE` with `wParam==SIZE_MAXIMIZED`/`SIZE_RESTORED`). Zoom and window geometry are spec-named saved state; without their own triggers a zoom-only or move-only change would never persist. All are debounced, so cost is unchanged.

- [ ] **Step 4: Implement `on_clean_exit()` and share it**

```cpp
void MainWindow::on_clean_exit() {
    KillTimer(hwnd_, kSessionSaveTimer);            // drain any pending debounce
    // If a restore is still in flight, do NOT touch session.json or the marker:
    // capture_session() now would persist a HALF-restored set, and clearing the
    // marker would suppress the next launch's prompt — together they permanently
    // lose the unrestored remainder of the crash session. Leaving both intact
    // means the next launch re-offers the FULL session.
    if (restoring_) return;
    if (!app_data_dir_.empty())
        core::save_session(app::session_file_under(app_data_dir_), capture_session());
    if (run_guard_) run_guard_->mark_clean_exit();  // idempotent
}
```

Wire it into BOTH:
- `WM_DESTROY` (`MainWindow.cpp:~1294`): call `on_clean_exit();` BEFORE `PostQuitMessage(0);`.
- `WM_ENDSESSION` (new handler): `if (wParam /* TRUE: session really ending */) on_clean_exit(); return 0;`. Optionally return `TRUE` from `WM_QUERYENDSESSION`. This is the fix for OS shutdown/reboot/logoff, which terminates the process WITHOUT `WM_DESTROY` — without it, every reboot with tabs open would leave the marker and falsely prompt restore.

`on_clean_exit()` is idempotent (save fully overwrites; `mark_clean_exit` removes a possibly-already-removed file), so being called from both paths is safe.

- [ ] **Step 5: Build + smoke** — `cmake --build build`. Open 2 PDFs, change page + zoom in each, wait 2 s, confirm `session.json` exists with both paths/pages/zooms. Change ONLY zoom in one, wait 2 s, confirm the new zoom is persisted (regression guard for the missing-trigger bug).

- [ ] **Step 6: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): capture + debounced auto-save; shared clean-exit for WM_DESTROY/WM_ENDSESSION"
```

---

## Task 9: Sequential restore orchestrator + wiring

**Files:**
- Modify: `src/main.cpp`, `src/ui/MainWindow.hpp`, `src/ui/MainWindow.cpp`

> **Executor note — verified async model:** `open_tab_async` (`MainWindow.cpp:140`) runs `doc.open()` on a **detached thread** and posts back `WM_USER_OPEN_OK` (tab inserted via `add_tab`, `:1163`, which fires `on_tab_switch` synchronously), `WM_USER_OPEN_FAILED` (`:1170`, modal error box), or `WM_USER_PASSWORD_PROMPT` (`:175`) for encrypted docs (tab built separately at `:1278`). Worker completion order is non-deterministic. A naive "open all then `set_active(saved_index)`" reorders tabs, lands on the wrong tab, never knows when "all" are open, skips encrypted tabs' page/zoom, and races the CLI `initial_path`. The orchestrator below opens tabs **one at a time**, applies page/zoom at each insertion point (including the password path), tracks completion across success/failure/cancel, resolves the active tab by **path**, and defers `initial_path`.

- [ ] **Step 1: Install handler + guard AFTER the single-instance gate**

In `wWinMain`, move the persistence wiring to AFTER the `if (already) return forward_to_running_instance(...) ? 0 : 1;` gate (`main.cpp:~44-52`) — only the message-loop-owning instance may own the marker (a forwarded second instance must never create/clear it):

```cpp
// ... single-instance gate returns early for a forwarded second instance ...

const std::filesystem::path data_dir = litepdf::app::app_data_dir();  // empty on failure
std::optional<litepdf::app::AppRunGuard> run_guard;   // constructed ONLY when persistence is enabled
bool offer_restore = false;
if (!data_dir.empty()) {
    litepdf::app::install_crash_handler(litepdf::app::crashes_dir_under(data_dir));
    litepdf::app::prune_crash_dumps(litepdf::app::crashes_dir_under(data_dir), 5);
    run_guard.emplace(litepdf::app::running_marker_under(data_dir));
    offer_restore = run_guard->previous_exit_was_abnormal();
}
```

> Empty-`data_dir` cascade is closed by construction: the `std::optional` stays empty when `data_dir` is empty, so NO crash handler, NO marker, and NO dumps are created — nothing lands in the process CWD (often the user's document folder for a shell-association launch). Task 6's `AppRunGuard` ctor and `mark_clean_exit` ALSO early-return on an empty path as belt-and-suspenders (so a future caller can't reintroduce the cascade).

Wire into MainWindow: `window.app_data_dir_ = data_dir; window.run_guard_ = run_guard ? &*run_guard : nullptr;` and pass `offer_restore` + the CLI `initial_path` (do NOT open `initial_path` eagerly when restoring — defer it; see Step 5). `run_guard` (and thus the pointer) lives in `wWinMain` until after `app.run()` returns, which outlives the window.

- [ ] **Step 2: Declare the orchestrator state** (`MainWindow.hpp`, with the Task 8 members)

```cpp
void maybe_offer_restore(bool offer);
void restore_open_next();              // issue the next sequential open
void restore_on_tab_ready(const std::filesystem::path& opened);  // OPEN_OK / password-success
void restore_on_tab_failed();          // ANY non-success terminal (OPEN_FAILED / password cancel/exhaust/ctor-throw)
void restore_finish();
void open_deferred_initial_path();     // honor the CLI file once, after restore (or on decline)

std::vector<core::SessionTab> restore_queue_;
std::size_t                   restore_index_ = 0;
std::filesystem::path         restore_pending_path_;  // path restore_open_next last issued (completion correlation)
std::filesystem::path         restore_active_path_;   // resolve active tab by PATH, not index
std::optional<std::filesystem::path> deferred_initial_path_;

static constexpr std::size_t kMaxRestoreTabs = 24;    // cap a runaway session restore
```

- [ ] **Step 3: `maybe_offer_restore` — prompt, filter, place, start**

```cpp
void MainWindow::maybe_offer_restore(bool offer) {
    if (!offer || app_data_dir_.empty()) { open_deferred_initial_path(); return; }
    auto saved = core::load_session(app::session_file_under(app_data_dir_));
    if (!saved || saved->tabs.empty()) { open_deferred_initial_path(); return; }

    // Pre-filter to existing files (skip deleted/moved/disconnected silently),
    // capped so a runaway session can't spawn an unbounded restore.
    restore_queue_.clear();
    restore_active_path_.clear();
    const int saved_active = saved->active_tab;
    for (int i = 0; i < (int)saved->tabs.size() &&
                    restore_queue_.size() < kMaxRestoreTabs; ++i) {
        std::error_code ec;
        if (std::filesystem::exists(saved->tabs[i].path, ec) && !ec) {
            if (i == saved_active) restore_active_path_ = saved->tabs[i].path;
            restore_queue_.push_back(saved->tabs[i]);
        }
    }
    if (restore_queue_.empty()) { open_deferred_initial_path(); return; }
    if (restore_active_path_.empty()) restore_active_path_ = restore_queue_.front().path;

    wchar_t msg[128];
    StringCchPrintfW(msg, ARRAYSIZE(msg),
                     L"上次未正常結束。要復原先前開啟的 %zu 個分頁嗎?", restore_queue_.size());
    if (MessageBoxW(hwnd_, msg, L"LitePDF",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES) {
        restore_queue_.clear();
        open_deferred_initial_path();
        return;
    }

    restoring_ = true;       // set BEFORE SetWindowPlacement so its WM_SIZE can't arm a save

    // Window geometry, guarded against (a) an off-screen multi-monitor change and
    // (b) a minimized/garbage show state, which would run the whole restore
    // against a 0x0 client rect (FitWidth/FitPage scale -> 0, degenerate renders).
    if (saved->window.w > 0 && saved->window.h > 0) {
        UINT show = (UINT)saved->window.show;
        if (show != SW_SHOWMAXIMIZED) show = SW_SHOWNORMAL;   // whitelist; minimized/unknown -> normal
        WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
        wp.flags   = 0;
        wp.showCmd = show;
        wp.rcNormalPosition = { saved->window.x, saved->window.y,
                                saved->window.x + saved->window.w,
                                saved->window.y + saved->window.h };
        if (MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL) != nullptr)
            SetWindowPlacement(hwnd_, &wp);   // skip rect if entirely off all monitors
    }

    restore_index_ = 0;
    restore_open_next();
}
```

(`StringCchPrintfW` needs `#include <strsafe.h>` in `MainWindow.cpp` — add it if not already present.)

- [ ] **Step 4: Sequential open + per-tab apply + completion**

```cpp
void MainWindow::restore_open_next() {
    if (restore_index_ >= restore_queue_.size()) { restore_finish(); return; }
    restore_pending_path_ = restore_queue_[restore_index_].path;  // correlation tag
    open_tab_async(restore_pending_path_);   // one at a time => saved order preserved
}

// Called from WM_USER_OPEN_OK AND the password-success path, AFTER add_tab has
// inserted (and activated) the new tab. `opened` is the path that completed.
void MainWindow::restore_on_tab_ready(const std::filesystem::path& opened) {
    // Correlation guard: consume a queue slot ONLY if this completion is the
    // open the orchestrator issued. A concurrent user/IPC open (drag-drop,
    // Ctrl+O, MRU, WM_COPYDATA from a 2nd instance) completing mid-restore
    // falls through to normal handling and must NOT shift the restore queue.
    if (!restoring_ || restore_index_ >= restore_queue_.size() ||
        opened != restore_pending_path_) return;

    const auto& st = restore_queue_[restore_index_];
    if (auto* v = active_view()) {           // the just-added tab is the active one
        // Load-bearing: add_tab fired on_tab_switch synchronously, seeding the
        // view's viewport dims and kicking ONE render that the kick_render below
        // cancels before it can paint (cancel-on-new-request) — no page-0 flash.
        v->set_current_page(st.page);
        switch (st.zoom_mode) {
            case core::SessionZoom::Custom:
                v->set_zoom_scale(st.zoom_scale);  // no viewport needed (Task 5)
                break;
            case core::SessionZoom::FitWidth:
            case core::SessionZoom::FitPage: {
                RECT rc; GetClientRect(canvas_->hwnd(), &rc);   // same pattern as on_tab_switch/kick_render
                const UINT dpi = GetDpiForWindow(hwnd_);
                v->set_zoom_mode(st.zoom_mode == core::SessionZoom::FitWidth
                                     ? core::DocumentView::ZoomMode::FitWidth
                                     : core::DocumentView::ZoomMode::FitPage,
                                 static_cast<float>(rc.right - rc.left),
                                 static_cast<float>(rc.bottom - rc.top),
                                 static_cast<float>(dpi));
                break;
            }
        }
        kick_render(st.page);
    }
    restore_pending_path_.clear();
    ++restore_index_;
    restore_open_next();
}

// Called on EVERY non-success terminal of the in-flight restore open: OPEN_FAILED
// (missing/corrupt/worker ctor-throw), password cancel, password exhaustion, and
// the post-auth construction catch. Advances the chain exactly once.
void MainWindow::restore_on_tab_failed() {
    if (!restoring_) return;
    restore_pending_path_.clear();
    ++restore_index_;        // skip the failed tab, keep the chain moving
    restore_open_next();
}

void MainWindow::restore_finish() {
    // Resolve the active tab by PATH (indices were filtered/reordered). If the
    // saved-active doc failed to open / was filtered out, fall back to tab 0.
    int active = -1;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* t = tabs_->tab_at(i);
        if (t && t->path == restore_active_path_) { active = i; break; }
    }
    if (active < 0 && tabs_->count() > 0) active = 0;
    if (active >= 0) tabs_->set_active(active);

    restoring_ = false;             // SINGLE terminal exit: re-enables live auto-save
    restore_queue_.clear();
    restore_pending_path_.clear();
    open_deferred_initial_path();   // now honor the CLI file, if any
    schedule_session_save();        // one save reflecting the restored state
}
```

Wire the completion handlers so EVERY terminal outcome funnels into the orchestrator **exactly once**. A missed funnel leaves `restoring_` stuck true, which permanently disables auto-save AND lets `on_clean_exit` overwrite the good session with a half-restored capture (Task 8 Step 4 now skips the save while `restoring_`, but the chain must still terminate). Grep the existing handlers:

- **`WM_USER_OPEN_OK`** (`:1145`): capture the path BEFORE the `std::move` into `add_tab` — `auto opened = raw->path;` (the `unique_ptr` is null after the move) — then, after `add_tab`, `if (restoring_) restore_on_tab_ready(opened);`. The correlation guard makes a non-restore open a no-op.
- **`WM_USER_OPEN_FAILED`** (`:1170`): the lParam carries the failed path (0 for the worker ctor-throw case). If `restoring_` AND (that path == `restore_pending_path_` OR lParam==0): call `restore_on_tab_failed()` and SUPPRESS the modal error box. Otherwise show the box as normal (a user-initiated failure mid-restore still gets feedback).
- **`open_tab_async`** (`:211-214`): when `PostMessageW(WM_USER_OPEN_OK)` returns FALSE the worker currently does `delete raw; return;`. Change it to post `WM_USER_OPEN_FAILED` (lParam 0) instead, so a dropped success still produces an observable completion and the chain advances rather than stalling forever.
- **Password path** (`WM_USER_PASSWORD_PROMPT`; build `:1278`, catch `:1279`, exhaustion box `:1249`): call `restore_on_tab_ready(path)` on successful auth + build; call `restore_on_tab_failed()` on EVERY other exit — cancel, 3-attempt exhaustion, AND the post-auth construction `catch(...)`. Suppress the exhaustion `MessageBox` while `restoring_`. Passwords are not persisted, so an encrypted tab still prompts during restore (accepted — see the sequential-restore tradeoff note in the header).

> **Executor note:** the `RECT`/`GetClientRect(canvas_->hwnd())`/`GetDpiForWindow(hwnd_)` pattern above is exactly what `on_tab_switch` (`:597-603`) and `kick_render` (`:230-231`) already use to size `set_zoom_mode` — reuse those call sites as the reference. For Custom zoom no viewport is needed.

- [ ] **Step 5: Defer the CLI `initial_path`; call restore after the window shows**

`open_deferred_initial_path()` MUST replicate the EXISTING eager-open block (`run()` `:1647-1658`), not just call `open_tab_async`: that block does `canonicalize_for_mru` + `mru_.push` + `mru_.save` before opening (a Phase 5 fix; dropping it regresses MRU for the most common open path — Explorer double-click / CLI):

```cpp
void MainWindow::open_deferred_initial_path() {
    if (!deferred_initial_path_ || deferred_initial_path_->empty()) { deferred_initial_path_.reset(); return; }
    std::filesystem::path normalized = canonicalize_for_mru(*deferred_initial_path_);
    mru_.push(normalized.wstring());
    mru_.save();
    deferred_initial_path_.reset();
    open_tab_async(std::move(normalized));
}
```

Then **REMOVE the eager `initial_path` open block from `run()`** (`:1647-1658`) so the file is not opened twice. Set `deferred_initial_path_ = initial_path;` before showing the window, and after `ShowWindow` / first `WM_SIZE` (so `MessageBoxW` has a parent and placement APIs are valid) call `maybe_offer_restore(offer_restore)` — which runs the restore chain (opening the deferred file at `restore_finish`) or, on decline / no-session, opens it immediately. The deferral is now the single authority for the initial file (no double-open), and the MRU push the eager path did is preserved.

- [ ] **Step 6: Build + manual verification (core acceptance)**

1. Open 3 PDFs (mixed sizes, incl. `encrypted.pdf`), set distinct pages + zooms, make tab 2 active. Wait 2 s.
2. Force-kill (Task Manager). Relaunch → prompt "上次未正常結束…".
3. Yes → tabs reopen in the SAME order, same pages/zooms, window placement, and tab 2 active; the encrypted tab re-prompts for its password and then restores its page/zoom.
4. Delete one file, repeat → missing file is skipped silently (no dialog storm), others restore.
5. Normal File→Exit once, relaunch → NO prompt.
6. Restart Windows with tabs open, relaunch → NO prompt (WM_ENDSESSION cleared the marker).
7. While the restore chain is still opening tabs, launch a 2nd instance pointing at a NEW PDF (forwarded via WM_COPYDATA) → it opens as its own tab with its OWN page/zoom (not a restore-queue entry), and the restore chain still finishes correctly (correlation guard).
8. Force-kill, relaunch, click Yes, then close the window while tabs are still loading → relaunch shows the prompt again and re-offers the FULL session (on_clean_exit skipped the mid-restore clobber).

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat: sequential, path-resolved session restore after abnormal exit (prompted)"
```

### Executor deviations (2026-06-14, vs plan text — controller-patches rule)

Two deviations from the Step 1 / Step 3 pseudocode, both verified against the live code:

1. **`app_data_dir` wiring — single resolution via a getter, not `window.app_data_dir_ = data_dir`.**
   Task 8 (committed) already resolves `app_data_dir_` in the MainWindow ctor, and the member is
   private. Rather than re-resolve in `wWinMain` (double `SHGetKnownFolderPath` + `create_directories`)
   or expose the member, a public `const std::filesystem::path& app_data_dir() const` getter was added.
   `wWinMain` constructs the window first (its ctor resolves the dir once), then reads it via the getter
   to install the crash handler + `prune_crash_dumps` + construct `std::optional<AppRunGuard>` — so the
   crash dir, run marker, and session file all share ONE resolution. The window is still constructed
   AFTER the single-instance gate, so a forwarded second instance never reaches this code and never
   touches the marker (the load-bearing invariant holds). Trade-off: the crash handler is installed
   just after the (trivial: dispatcher + MRU load + path resolve) ctor instead of just before it —
   negligible lost coverage.

2. **Restore prompt is English, not the zh-TW literal.** Every existing runtime `MessageBoxW` in the
   app is English ("Failed to open the document.", "Incorrect password.", …); a lone Chinese restore
   dialog would be inconsistent, and the `check-chinese-in-code` hook blocks CJK in source. Used
   `L"LitePDF closed unexpectedly last time.\nRestore the %zu previously open tab(s)?"`. The installer
   (Task 10) stays zh-TW — it is a separate artifact with its own audience. (If the app UI is ever
   meant to be zh-TW, that is a separate sweep translating ALL existing English strings, out of scope
   for Task 9.)

3. **v4 review fix — password failure terminals must be path-correlated (HIGH, found by the Opus
   review lens).** Step 4's funnel text said to call `restore_on_tab_failed()` on cancel / exhaustion /
   post-auth `catch` gated only on `restoring_`. That is wrong: `PasswordDialog::prompt` runs a modal
   *nested* message loop, so a concurrent user/IPC open of a DIFFERENT encrypted file (drag-drop /
   Ctrl+O / MRU / WM_COPYDATA) can be dispatched reentrantly while a restore tab sits on its password
   modal. Cancelling/exhausting that concurrent open would call `restore_on_tab_failed()` against the
   restore's still-pending slot, advancing `restore_index_` and clearing `restore_pending_path_` — so
   the real restore tab loses its page/zoom (and tab order can scramble). Fix: a single
   `is_restore_tab = restoring_ && opened == restore_pending_path_` gates ALL three password terminals
   (and the exhaustion-box suppression), symmetric with the already-correlated `WM_USER_OPEN_OK` /
   `WM_USER_OPEN_FAILED` paths. The Sonnet lens independently cleared the other six invariants.

Verified: `litepdf` exe builds clean in Release (only pre-existing MuPDF C4702/C4703 + the pre-existing
PrintJob::run C4834); exe ≈ 11.74 MB, ~6.69 MB under the 19,000,000 gate; unit suite still green
(200 cases, 199 pass + 1 expected-fail). Manual restore acceptance (Step 6) is interactive GUI
testing, folded into the Task 11 smoke pass.

---

## Task 10: Installer — update keep-config prompt wording

**Files:**
- Modify: `installer/litepdf.iss`

`%LOCALAPPDATA%\LitePDF` now exists, so the uninstaller's keep/delete prompt (`CurUninstallStepChanged`, `:144-161`) fires for the first time. `DelTree` already covers the whole dir; only the zh-TW message text needs to name the new artifacts.

> **Encoding gotcha:** `litepdf.iss` MUST keep its UTF-8 BOM or zh-TW mojibakes on a non-Chinese build runner. Do NOT round-trip through PowerShell 5.1 `Get-Content`. Edit in place. See `reference_litepdf_installer_encoding_gotchas` memory.

- [ ] **Step 1: Update the prompt string** (`:154`):

```pascal
if MsgBox('要一併刪除 LitePDF 的設定、工作階段與當機記錄嗎?' + #13#10 + ConfigDir + #13#10#13#10 +
          '選「否」會保留這些資料 (預設)。', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
  DelTree(ConfigDir, True, True, True);
```

- [ ] **Step 2: Verify BOM intact (byte-level, not git stat)**

```powershell
$b = [System.IO.File]::ReadAllBytes('installer/litepdf.iss')
('{0:X2}{1:X2}{2:X2}' -f $b[0],$b[1],$b[2])   # expect EF BB BF
```

- [ ] **Step 3: Commit**

```bash
git add installer/litepdf.iss
git commit -m "chore(installer): keep-config prompt now names session + crash data"
```

---

## Task 11: Smoke checklist — create + execute

**Files:**
- Create: `docs/SMOKE-CHECKLIST.md`

- [ ] **Step 1: Write the checklist**

```markdown
# LitePDF Pre-Release Smoke Checklist

Run on a Release build before every `vX.Y.Z` tag. Record pass/fail + notes.

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
```

- [ ] **Step 2: Execute the full pass** on a Release build. Record results in the PR description. Any FAIL blocks the v1.0 tag — fix and re-run.

- [ ] **Step 3: Commit**

```bash
git add docs/SMOKE-CHECKLIST.md
git commit -m "docs: add pre-release smoke checklist"
```

---

## Task 12: Release v1.0

**Files:** `VERSION`, `CHANGELOG.md` (+ git tag)

> Follow the project ship convention (`project_litepdf_ship_version_convention` memory): VERSION bumps only at phase boundaries; this IS the v1.0 boundary. Prefer `/ship` over manual edits.

- [ ] **Step 1:** Ensure all unit tests pass (`ctest --test-dir build`) AND **all items in `docs/SMOKE-CHECKLIST.md` passed** (13 Phase-12 + 7 core items — this supersedes the roadmap's 6-item baseline; do NOT treat the checklist as optional).
- [ ] **Step 2:** Move accumulated `## [Unreleased]` CHANGELOG entries (MuPDF 1.27.2, size prune, this phase) under `## [1.0.0] — <date> — v1.0 Release`; add Phase 12 lines (minidump, crash-restore, smoke checklist, installer prompt).
- [ ] **Step 3:** Set `VERSION` to `1.0.0` (strip `-dev`).
- [ ] **Step 4:** Commit, tag `v1.0.0`, push, open the release PR (or let `/ship` do 2-4). The tag-triggered `release.yml` builds installer + portable zip + source tarball.

---

## Self-Review

**1. Spec coverage (design §6.3 + roadmap Phase 12):**
- Minidump via SetUnhandledExceptionFilter → `%LOCALAPPDATA%\LitePDF\crashes\` → Tasks 1, 7. ✅ (bounded buffer + re-entrancy guard + retention.)
- Session state (tabs, page, zoom) → Tasks 2-5, 8; restored only after abnormal exit → Tasks 6, 9. ✅ (window placement + active tab added; pan excluded per scope.)
- No telemetry / local-only → no network code. ✅
- Manual smoke list → Task 11. ✅
- v1.0 tag + release → Task 12. ✅
- Installer interaction → Task 10. ✅

**2. Review findings folded in:** blocker (async restore → sequential orchestrator, Task 9); high (Task 5 pimpl; run-guard after gate; WM_ENDSESSION; zoom/window triggers); medium/low (empty-dir guard; file-exists pre-filter; off-screen guard; MoveFileEx atomic write; %.9g float; \u UTF-8 decode + surrogate reject; strict UTF-8 conversion; post-parse validate; size cap; version gate; crash-dir prune; KillTimer; byte-level BOM check; smoke expansions). Rejected finding (AppPaths/shell32) not actioned, per verification.

**3. Type consistency:** `SessionState`/`SessionTab`/`SessionWindow`/`SessionZoom`/`kSessionVersion` (Task 2) used identically in Tasks 3, 4, 8, 9. `to_json`/`from_json`/`validate` (2/3) consumed by `save_session`/`load_session` (4). `set_zoom_scale` (5) used in Task 9. `AppRunGuard` API (6) used in Tasks 8-9. `app_data_dir`/`*_under`/`prune_crash_dumps` (1) used in Tasks 7-9. Restore-orchestrator members (Task 9 Step 2) consumed only within Task 9. Consistent.

**Residual known limitations (accepted for v1.0, documented not fixed):** stack-overflow minidumps are best-effort; ≤1.5 s of state can be lost to the save debounce; restore is sequential (additive time; a password prompt pauses the chain; capped at 24 tabs); a TOCTOU exists between the restore file-exists pre-filter and `open_tab_async` (a file deleted in that window falls through to the normal open-failed path, suppressed during restore); a `WM_USER_OPEN_FAILED` with `lParam==0` (worker ctor-throw) arriving from a *concurrent user open* during restore is attributed to the in-flight restore slot (near-zero probability; worst case one mis-stepped queue slot, still terminates).

---

## Next gate (per repo review policy)

This plan passed TWO multi-model review rounds (Opus + Sonnet + Fable + Codex, per-finding adversarially verified): round 1 hardened the originals (v2), round 2 hardened the new orchestrator (v3). All four lenses agreed the architecture is sound and the remaining items are localized; round 2's fixes are mechanical (correlation guard, terminal funnels, clobber-skip) and are now in the text. The plan is **converged** — further plan-text review has diminishing returns; residual risk is best caught reviewing REAL code. Execute via **subagent-driven-development** (fresh subagent per task, two-stage review between tasks), which reviews each task's actual implementation against this plan. Per the controller-patches-source-of-truth rule, if a task surfaces a deviation from this plan (e.g. an assumed handler line/signature is different in the live code), patch this plan before dispatching the next task.
