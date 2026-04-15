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
