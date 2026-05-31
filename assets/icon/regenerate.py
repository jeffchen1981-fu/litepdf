"""Regenerate LitePDF icon assets from SVG masters.

Usage:
    python regenerate.py             # regenerate both variants
    python regenerate.py --verify    # verify committed .ico files match expected sizes

Layout (relative to repo root):
    assets/icon/litepdf-app.svg   ->  app-<N>.png  ->  litepdf-app.ico
    assets/icon/litepdf-doc.svg   ->  doc-<N>.png  ->  litepdf-doc.ico

Sizes: 16, 20, 24, 32, 48, 64, 256.

Rasterizer: resvg, via the `resvg-py` wheel. resvg is a Rust SVG renderer that
ships as a statically linked binary with no native-library dependencies, so
`pip install resvg-py` works out of the box on Windows/macOS/Linux. cairosvg
(the obvious first choice) needs a system libcairo DLL that is absent on a stock
Windows machine; see ../../docs/superpowers/specs/2026-05-05-phase-9-icons-design.md
section "Rasterizer choice" for the full rationale.
"""
from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

ICON_DIR = Path(__file__).resolve().parent
SIZES = (16, 20, 24, 32, 48, 64, 256)
VARIANTS = (
    ("app", "litepdf-app.svg", "litepdf-app.ico"),
    ("doc", "litepdf-doc.svg", "litepdf-doc.ico"),
)


def _rasterize(svg_path: Path, size: int) -> bytes:
    """Render one SVG to PNG bytes at size x size using resvg."""
    import resvg_py

    raw = resvg_py.svg_to_bytes(svg_path=str(svg_path), width=size, height=size)
    # resvg-py returns a list[int] of PNG bytes; normalize to a bytes object.
    return raw if isinstance(raw, (bytes, bytearray)) else bytes(raw)


def regenerate() -> None:
    from PIL import Image

    for prefix, svg_name, ico_name in VARIANTS:
        svg_path = ICON_DIR / svg_name
        if not svg_path.exists():
            raise FileNotFoundError(f"missing SVG master: {svg_path}")
        frames = {}
        for size in SIZES:
            png_bytes = _rasterize(svg_path, size)
            png_path = ICON_DIR / f"{prefix}-{size}.png"
            png_path.write_bytes(png_bytes)
            frames[size] = Image.open(io.BytesIO(png_bytes))
            print(f"  rasterized {png_path.name} ({size}x{size})")
        # Bundle every native-rendered frame into one multi-resolution ICO.
        # Pillow's ICO writer drops any requested size LARGER than the base
        # image's own dimensions, so the base must be the largest frame (256);
        # otherwise only the smallest entry survives. resvg rendered each size
        # natively, so Pillow matches every `sizes` entry to its own frame and
        # never up/downscales.
        ico_path = ICON_DIR / ico_name
        ordered = sorted(SIZES, reverse=True)  # 256 first -> base image
        frames[ordered[0]].save(
            ico_path,
            format="ICO",
            sizes=[(s, s) for s in SIZES],
            append_images=[frames[s] for s in ordered[1:]],
        )
        print(f"  bundled {ico_name} ({len(SIZES)} frames)")


def _ico_frame_sizes(ico_path: Path) -> list[tuple[int, int]]:
    """Return the sorted frame sizes stored in an ICO file.

    Pillow's IcoImageFile does not expose frames through the n_frames/seek
    sequence API (that is for GIF/TIFF); it loads only the largest frame by
    default. The full set of stored sizes is read from the underlying IcoFile
    via `.ico.sizes()`.
    """
    from PIL import Image

    with Image.open(ico_path) as ico:
        return sorted(ico.ico.sizes())


def verify() -> int:
    expected = sorted({(s, s) for s in SIZES})
    failures: list[str] = []
    for _prefix, _svg_name, ico_name in VARIANTS:
        ico_path = ICON_DIR / ico_name
        if not ico_path.exists():
            failures.append(f"{ico_name}: missing")
            continue
        actual = _ico_frame_sizes(ico_path)
        if actual != expected:
            failures.append(f"{ico_name}: sizes={actual} (expected {expected})")
        else:
            print(f"  {ico_name}: OK ({len(SIZES)} frames at expected sizes)")
    if failures:
        for line in failures:
            print(f"FAIL: {line}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Regenerate LitePDF icon assets.")
    parser.add_argument("--verify", action="store_true",
                        help="verify committed .ico files match expected sizes")
    args = parser.parse_args(argv)
    if args.verify:
        return verify()
    regenerate()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
