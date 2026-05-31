# LitePDF Icon Assets

This directory holds the source SVGs, rasterized PNGs, and bundled ICO files
for `IDI_APPICON` (the "Lightning Document" app icon) and `IDI_PDFDOC` (the
red "PDF" wordmark used for `.pdf` file association in Phase 10).

Design source of truth:
[`docs/superpowers/specs/2026-05-05-phase-9-icons-design.md`](../../docs/superpowers/specs/2026-05-05-phase-9-icons-design.md).

## Files

| File | Role |
|---|---|
| `litepdf-app.svg` | 256-px master for the app icon |
| `litepdf-doc.svg` | 256-px master for the document icon |
| `app-<N>.png`, `doc-<N>.png` | Rasterizations for `N` in {16, 20, 24, 32, 48, 64, 256}. Committed for PR diff visibility. |
| `litepdf-app.ico`, `litepdf-doc.ico` | Multi-resolution bundles consumed by `resources/litepdf.rc` |
| `regenerate.ps1` | Top-level driver — call this after editing an SVG |
| `regenerate.py` | Python helper invoked by the driver; pure logic |
| `requirements.txt` | `resvg-py` and `Pillow` version pins |

## Regenerating after an SVG edit

Pre-requisites: Python 3.10+ and `pip` on PATH. From the project root:

    powershell -File assets/icon/regenerate.ps1   # Windows PowerShell 5.1+
    pwsh assets/icon/regenerate.ps1               # PowerShell 7+

The script installs `resvg-py` and `Pillow` into the active Python environment
(use a venv if you prefer), rasterizes both SVGs at all seven sizes, and
bundles the per-variant `.ico` files. The PNG intermediates are also written
so PR reviewers can scan visual diffs on GitHub.

**Why `resvg-py` and not `cairosvg`?** `resvg` is a Rust SVG renderer shipped as
a statically linked wheel — no native libraries to install. `cairosvg` (and the
`svglib` + `reportlab` fallback, which uses the `rlPyCairo` backend) need a
system `libcairo` DLL that a stock Windows machine does not have, so they install
fine but crash at import time. `resvg-py` renders each size natively, so the
bundled 256-px ICO frame is pixel-identical to the standalone 256-px PNG with no
upscaling blur.

After regeneration, commit every changed `.png` and `.ico` together with the
`.svg` source change so reviewers see the visual delta in one commit.

## Verifying without rebuilding

    python assets/icon/regenerate.py --verify

This loads each committed `.ico` and asserts it contains seven frames at the
expected sizes. CI does not run this yet (Phase 12 hardening adds it); for
now, PR reviewers run it locally if they suspect drift between `.svg` and
`.ico`.
