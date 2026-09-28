# Build identity: show which build is running (#60) — design

Date: 2026-09-28
Base: `main` @ `f7b1ed68fb18f095a99e55a35b4244ad06ecbf12` (v1.3.0 + 18)
Issue: [#60](https://github.com/jeffchen1981-fu/litepdf/issues/60) — decision recorded in
[the decision comment](https://github.com/jeffchen1981-fu/litepdf/issues/60#issuecomment-5862562227)

A bug report was investigated at length against `main` when the reporter was running
a four-releases-old dev build. Nothing in the app said so. This design makes every
non-release build say so where the user already looks, gives every build a traceable
identity, and adds gates that fail when the identity is wrong.

`VERSION` is not bumped (`project_litepdf_ship_version_convention`).

---

## 1. Decisions (fixed — do not reopen without new evidence)

| Decision | Reason |
|---|---|
| **LitePDF stays offline.** No update check of any kind, manual or automatic. The "should it ever check?" question is parked. | Design §10 and installer spec §9 put update checking out of scope. The only evidence for it is a dev build on a dev machine. |
| No "Check for Updates" menu item. The Releases URL goes in About as plain text. | A menu item that only opens a web page duplicates the installer's `AppUpdatesURL` and the README, and checks nothing. |
| The title bar shows a version **only on non-release builds**: `LitePDF 1.3.0-dev — x.pdf`. Release builds keep `LitePDF — x.pdf`. | The failure to prevent is "didn't know this wasn't a release". Release users gain nothing from a version in every title. |
| The `-dev` marker is **automatic**, derived from git. `VERSION` does not go back to the `x.y.z-dev` convention. | Nobody has to remember to edit it, so it cannot be forgotten. |
| **Git decides, and anything unproven is dev.** A build is a release only when git positively proves it. | A false "dev" costs a title suffix. A false "release" is the #60 defect. |
| No `export-subst` stamp file. A user building the release source tarball sees `-dev`. | That build is not the official binary, so `-dev` is accurate. A stamp would add `%D` parsing and depend on `git-archive-all` behaviour nobody has verified. |

### Why a version number alone would not have prevented the incident

`VERSION` moves only at release boundaries, so every post-release build carries the
released number. On `main` today, `git describe --tags --long` prints
`v1.3.0-18-gf7b1ed6`, while `VERSION` is `1.3.0` and the local exe's `ProductVersion`
is `1.3.0.0` — identical to the release. The incident build already showed `v0.0.13`
in About. What was missing was provenance, not a number.

## 2. Architecture

```
configure:  find_package(Git) -> GIT_EXECUTABLE
build:      litepdf_build_id (add_custom_target, runs every build)
              cmake -P cmake/CollectBuildFacts.cmake
                -> build/generated/litepdf_build_facts.h  (copy_if_different)
            litepdf_core
              src/core/Version.cpp  #includes the facts header
                classify(facts) -> BuildIdentity
consumers:  MainWindow title + About · litepdf-cli --version · CrashHandler dump name
```

### 2.1 Fact collection — `cmake/CollectBuildFacts.cmake` (build time)

The script **collects facts and never classifies**. Inputs: `GIT_EXECUTABLE`, `SRC`
(= `CMAKE_SOURCE_DIR`), `OUT`. It runs git with `-C <SRC>` and records:

| Fact | Source | Absent / error |
|---|---|---|
| `toplevel_match` | `file(REAL_PATH)` of `git rev-parse --show-toplevel` equals `file(REAL_PATH)` of `SRC`, compared case-insensitively | `0` |
| `exact_tag` | `git describe --tags --exact-match` | empty |
| `describe` | `git describe --tags --long --dirty` | empty |
| `short_sha` | `git rev-parse --short HEAD` | empty |
| `dirty` | `1` if `git describe --always --dirty` ends in `-dirty` — the same mechanism as `describe`, so both facts agree, and it works with no tags | `0` |

- Any git failure is recorded as "no answer" (the empty or `0` in the table above). The
  script **never fails the build on a git error**. A bug in the script itself still errors
  and fails the build, which is intended.
- Every string fact is **sanitized as a whole value**: it must match
  `^[A-Za-z0-9._-]+$` or it becomes empty. All 12 current tags pass.
- The header is written to a temp file, then `copy_if_different` onto `OUT`. When the
  commit has not changed, the header's mtime does not change and nothing recompiles.
  When it has, only `Version.cpp` recompiles.
- `toplevel_match` guards against a git repo that is not ours. release.yml's tarball
  build-back step extracts into `_backcheck\` inside the checkout, and git run from there
  would find the parent repo.
- Measured: the MuPDF submodule has prune modifications, and `git describe --dirty` still
  reports clean because of `ignore = dirty`. The release build stays clean.

CMake wiring: `find_package(Git)` at configure. `add_custom_target(litepdf_build_id
COMMAND ${CMAKE_COMMAND} -DGIT_EXECUTABLE=... -DSRC=... -DOUT=... -P ...)` has no
OUTPUT, so it runs on every build. `add_dependencies(litepdf_core litepdf_build_id)`
orders it first. `build/generated` goes on `litepdf_core`'s PRIVATE include path. When
Git is not found, the command passes an empty `GIT_EXECUTABLE` and the script records
"no answer".

A target-wide compile definition is rejected: it would rebuild all 15 exe TUs on every
commit. Configure-time `execute_process` is rejected too, because CMake only re-runs
configure when `VERSION` changes, so the SHA would go stale.

### 2.2 Classification — `src/core/Version.{hpp,cpp}` (in `litepdf_core`)

```cpp
struct BuildFacts { bool toplevel_match; std::string_view exact_tag, describe, short_sha; bool dirty; };
struct BuildIdentity { std::string display_version; std::string build_id; bool is_release; };

BuildIdentity classify(const BuildFacts&, std::string_view version);  // pure
const BuildIdentity& build_identity();  // classify(<generated facts>, <VERSION triple>), computed once
```

`version` is the stripped `VERSION` triple (`LITEPDF_VERSION`, passed as a generated
constant in the same header).

- **Release** iff `toplevel_match && !exact_tag.empty() && !dirty`. Then
  `display_version = version` and `build_id = exact_tag` (for example `v1.3.0`).
- **Otherwise dev**: `display_version = version + "-dev"`. `build_id` is the first
  non-empty value of:
  - `describe` (for example `v1.3.0-18-gf7b1ed6` or `v1.3.0-0-g88513f2-dirty`), when
    `toplevel_match`
  - `"g" + short_sha + (dirty ? "-dirty" : "")`, when `toplevel_match`
  - `""`
- When `toplevel_match` is false, git facts about a foreign repo are ignored: the
  result is dev with an empty `build_id`.

`build_id` for a release is the exact tag, never the long form: at a tag
`describe --tags --long` prints `v1.3.0-0-g88513f2`.

### 2.3 Consumers

| Where | Release | Dev |
|---|---|---|
| Title, no document (`MainWindow.cpp` `kWindowTitle` path) | `LitePDF` | `LitePDF 1.3.0-dev` |
| Title, with document (`update_window_title`) | `LitePDF — x.pdf` | `LitePDF 1.3.0-dev — x.pdf` |
| About first line | `LitePDF 1.3.0` | `LitePDF 1.3.0-dev` |
| About `Build:` line | `Build: v1.3.0` | `Build: v1.3.0-18-gf7b1ed6`, omitted when `build_id` is empty |
| About Releases line | `Releases: https://github.com/jeffchen1981-fu/litepdf/releases` | same |
| `litepdf-cli --version` | `version=1.3.0` / `build=v1.3.0` / `release=1` | `version=1.3.0-dev` / `build=...` / `release=0` |
| Crash dump name | `litepdf-1.3.0-<pid>-<tick>.dmp` | `litepdf-1.3.0-dev-<pid>-<tick>.dmp` |

- The hard-coded `L"LitePDF v1.3.0\n\n"` About literal is removed. `Engine: MuPDF
  1.27.2` stays a literal. It is sourced from the submodule, and this design does not
  change it.
- `kWindowTitle` stays `L"LitePDF"`: it is also the caption of every `MessageBoxW`
  (About included). The dev title is built separately in the title-setting paths. The
  single-instance lookup finds the window by class (`SingleInstance.cpp:26`), so it is
  unaffected.
- `litepdf-cli`: recognize `--version` **before** the `argc < 2` usage exit and the
  `argv[1]` path read (`src/cli/main.cpp:229-238`). Print the three `key=value` lines to
  stdout and exit 0. No other consumer parses cli stdout except `benchmark.ps1`, which
  reads only `--json` output.
- Crash dump: `install_crash_handler` appends `litepdf-<display_version>-` to `g_prefix`
  once, at install time. The exception filter's format becomes `%s%lu-%lu.dmp`, so the
  filter does no more work than today. `prune_crash_dumps` selects by the `.dmp`
  extension and by mtime (`AppPaths.cpp:45-69`), so it is unaffected. Update the
  comments that spell the old name: `CrashHandler.hpp:8` and `AppPaths.cpp:55`.
- `scripts/ux-probe.ps1:57`: the exact-title fallback `FindWindow($null, 'LitePDF')`
  stops matching on dev builds. Replace it with a `LitePDF` prefix match.
- README "Versioning": document the automatic `-dev` marker and what About shows.

## 3. Gates

### 3.1 `scripts/check-version-sync.ps1`

Kept: the `.rc` template-parametric check and the generated-`.rc` check.

Removed: the About-literal check. The literal no longer exists.

Added — assertions on the built artifact:

- Run `build/Release/litepdf-cli.exe --version` and parse its `key=value` lines, trimming
  each line.
- `version`'s numeric triple == `VERSION` (stripped) == exe `ProductVersion` minus `.0`.
- One of two modes (mutually exclusive parameter sets):
  - `-ExpectRelease [-Tag <name>]`: `release=1`, `version` has no `-dev`, and `build` ==
    `-Tag`. `-Tag` defaults to `v<VERSION>`. release.yml passes
    `-ExpectRelease -Tag $env:GITHUB_REF_NAME`.
  - `-ExpectDev`: `release=0`, `version` ends in `-dev`, and `build` is non-empty. ci.yml
    passes `-ExpectDev`. CI is a shallow, tagless checkout, so `build` is `g<sha>`.
  - Neither mode given, for a local run: the artifact checks run, the classification
    check is skipped, and the script prints a notice.
- Missing cli exe: **fails** when `$env:CI -eq 'true'`. Locally it is skipped with a
  notice, the same as today's generated-`.rc` handling.
- The script stays 5.1-compatible and ASCII-only outside comments
  (`reference_litepdf_powershell_51_only`).

Self-test: add `-SelfTest`, modelled on `check-benchmark-regression.ps1`. It injects fake
cli output through a `-CliOutput <string[]>` parameter and checks that each of
`-ExpectRelease` and `-ExpectDev` passes once and fails once. Register it as ctest
`version_sync_selftest`, beside `benchmark_selftest` (`CMakeLists.txt:167-176`).

A release misclassified as dev fails `-ExpectRelease` at release.yml's "Version sync
gate" step. That step runs before "Create draft release", so nothing is published.

### 3.2 `scripts/smoke-test.ps1`

The script gets `-ExpectDev` / `-ExpectRelease`. ci.yml passes `-ExpectDev`; release.yml
passes `-ExpectRelease`. The bookmarks.pdf title check (`smoke-test.ps1:147-154`)
becomes:

- `-ExpectDev`: title matches `^LitePDF <VERSION>-dev <U+2014> bookmarks`
- `-ExpectRelease`: title matches `^LitePDF <U+2014> bookmarks`. This is the only check
  that proves the **release exe** has the title prefix off.
- neither: the existing `bookmarks` substring check.

The dash is written `[char]0x2014` inside the regex, never as a literal. `smoke-test.ps1`
has no BOM, and Windows PowerShell 5.1 on a CP950 machine decodes a literal UTF-8 em dash
as `??`: CI would pass and local runs would fail. `VERSION` goes through
`[regex]::Escape`.

## 4. Testing

| Test | What it proves |
|---|---|
| Catch2 `Version: ...` cases for `classify` | Every branch: exact tag clean → release; exact tag + dirty → dev with `-dirty` describe; commits past tag → dev; no tags → `g<sha>`; no tags + dirty → `g<sha>-dirty`; git no answer → dev with empty id; `toplevel_match` false with valid facts → dev with empty id; empty `exact_tag` (sanitized away) → dev. ASCII names, `Version:` prefix (`reference_litepdf_ctest_ascii_test_names`). |
| ctest `version_script_selftest` (PowerShell, 5.1-safe) | Runs `CollectBuildFacts.cmake` against a temp git repo in six states — exact tag; tag + dirty; commits past tag; no tags; `SRC` = a subdirectory (toplevel mismatch); `GIT_EXECUTABLE` pointing at a missing exe — and asserts the emitted facts. Without it, the tag, dirty and mismatch paths would first run inside the release workflow. Needs no network. |
| ctest `version_sync_selftest` | The gate can fail (§3.1). |
| CI gate `-ExpectDev` + smoke `-ExpectDev` | The real CI artifact classifies as dev, has a non-empty id, and shows the title prefix. |
| Release gate `-ExpectRelease` + smoke `-ExpectRelease` | The real release artifact classifies as release, its id equals the tag, and it has no title prefix. |
| Manual (listed in the plan) | Build twice: the second build recompiles nothing. Make a new commit and build: only `Version.cpp` recompiles. Screenshot About on a dev build. |

Build and test in **Release**: `ctest --test-dir build -C Release`
(`reference_litepdf_build_test_commands`).

## 5. Out of scope

- Any network access, update check or telemetry (parked per §1).
- A provenance stamp for git-less source archives (§1).
- Putting `build_id` into the `.rc` `ProductVersion` string (Explorer file properties).
  This design leaves that possible later.
- Changing the `Engine: MuPDF` literal's sourcing.
