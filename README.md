# LitePDF

A lightweight PDF / ePub / CBZ / XPS reader for Windows 11, optimized for mechanical hard drives. Single self-contained executable, no runtime dependencies.

- **Status:** under development (Phase 1 — document core)
- **License:** [AGPL-3.0](LICENSE)
- **Design:** [`docs/plans/2026-04-15-litepdf-design.md`](docs/plans/2026-04-15-litepdf-design.md)
- **Roadmap:** [`docs/plans/2026-04-15-litepdf-roadmap.md`](docs/plans/2026-04-15-litepdf-roadmap.md)

## Build

### Prerequisites

- Windows 11
- Visual Studio 2022 Build Tools with the **Desktop development with C++** workload (includes MSVC v143, Windows 11 SDK, and CMake ≥ 3.25).
- Git 2.30+ (for submodule support).

### Commands

```sh
git clone --recursive https://github.com/jeffchen1981-fu/litepdf
cd litepdf
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The produced `build/Release/litepdf.exe` is a single self-contained binary.

## Versioning

`VERSION` contains a semver string optionally suffixed with `-dev`, `-rc1`, etc. The CMake build reads this file and strips the pre-release suffix before passing the numeric triple to `project(VERSION ...)` (required by CMake).
