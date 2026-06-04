#!/usr/bin/env python3
"""
Generate tests/fixtures/large.pdf — the primary gated timing fixture for the
Phase 11 benchmark regression gate.

Design (Phase 11 spec §3.4):
  * ~500 pages so the document-open path (xref / page-tree walk) has a chance
    of measurable cost. Whether open_ms actually clears the CI noise floor is
    decided empirically by PR1's measure-only run, not assumed here.
  * Page 0 is deliberately DENSE — many text runs plus vector content — so the
    first-page rasterize (render_ms) is measurable above the timer floor. The
    benchmark renders page 0 only; a sparse page would rasterize in near-zero
    time and give render_ms no signal (Codex round-2 finding).
  * Deterministic across environments: reportlab invariant mode
    (rl_config.invariant = 1) pins the document timestamp and /ID, AND the canvas
    is built with pageCompression=0 so no PDF stream is zlib-compressed. The
    latter is load-bearing: PDF stream compression goes through the interpreter's
    zlib, and CPython 3.14+ on Windows bundles zlib-ng while <=3.13 bundles stock
    zlib — the two emit different deflate bytes, so a *compressed* fixture is only
    byte-reproducible on the same zlib build (e.g. it drifts between a 3.14 dev box
    and a 3.12 CI runner). With zero FlateDecode streams the bytes depend solely on
    reportlab's pure-Python output, so any machine with reportlab==4.4.10
    regenerates the identical file. Generated with reportlab==4.4.10; the benchmark
    workflow installs that same pinned version before running --check.
  * --check regenerates to a temp path, asserts byte-identity against the committed
    fixture, AND asserts the regenerated PDF has zero FlateDecode streams (so
    re-enabling compression is caught on the dev's own machine, before it breaks a
    different-zlib CI). Any drift (a reportlab bump, an accidental edit, a
    compression re-enable) fails CI.

This fixture is GATE-LOAD-BEARING: changing its content shifts the baseline
timings the regression gate protects. Regenerate only deliberately.

White page fills are required because LitePDF's PdfCanvas uses a dark D2D
surface; see generate-search-fixture.py for the full rationale.
"""

import argparse
import os
import sys
import tempfile

# Invariant mode MUST be set before the pdfgen machinery stamps the document so
# the fixed timestamp + deterministic /ID take effect. This is what makes
# regeneration byte-stable (the --check contract).
from reportlab import rl_config
rl_config.invariant = 1

from reportlab.lib.pagesizes import letter
from reportlab.lib.colors import white, black
from reportlab.pdfgen import canvas


PAGE_COUNT = 500

OUT_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "large.pdf",
)


def fill_page_white(c, width, height):
    """Draw an explicit white rectangle over the whole page (dark-surface fix)."""
    c.saveState()
    c.setFillColor(white)
    c.setStrokeColor(white)
    c.rect(0, 0, width, height, fill=1, stroke=0)
    c.setFillColor(black)  # restore default for subsequent drawString calls
    c.restoreState()


def draw_dense_page0(c, width, height):
    """Render-heavy first page: many text runs + vector content so render_ms
    has signal above the timer floor (spec §3.4)."""
    fill_page_white(c, width, height)

    # Two columns of ~11pt text gives a few hundred glyph runs on page 0.
    c.setFillColor(black)
    c.setFont("Helvetica", 11)
    left_x = 54
    right_x = width / 2 + 18
    top_y = height - 54
    line_h = 13
    sentence = ("The quick brown fox jumps over the lazy dog while "
                "rendering benchmark page zero.")
    rows = int((top_y - 54) / line_h)
    for col_x in (left_x, right_x):
        y = top_y
        for r in range(rows):
            c.drawString(col_x, y, "%03d %s" % (r, sentence))
            y -= line_h

    # Vector content: stroked rectangles + tick lines so the display list has
    # non-trivial fill/stroke ops, not just text.
    c.setStrokeColor(black)
    c.setLineWidth(0.5)
    for gx in range(0, int(width), 24):
        c.line(gx, 40, gx, 52)
    for i in range(40):
        x0 = 54 + i * 6
        c.rect(x0, 40, 5, 8, fill=0, stroke=1)


def draw_filler_page(c, width, height, page_number):
    """Light filler page: a single heading line. Keeps per-page open cost real
    (page-tree entries) without inflating render time on non-page-0 pages."""
    fill_page_white(c, width, height)
    c.setFillColor(black)
    c.setFont("Helvetica", 12)
    c.drawString(72, height - 72, "Filler page %d of %d." % (page_number, PAGE_COUNT))


def build(out_path):
    width, height = letter
    # pageCompression=0 keeps every content stream uncompressed so the bytes are
    # independent of the interpreter's zlib (zlib-ng vs stock zlib); see the module
    # docstring. Passed explicitly rather than via rl_config so the value can never
    # be shadowed by an import-time default binding.
    c = canvas.Canvas(out_path, pagesize=letter, pageCompression=0)
    # Pinned metadata (belt-and-suspenders on top of invariant mode).
    c.setTitle("LitePDF Phase 11 benchmark fixture")
    c.setAuthor("litepdf")
    c.setSubject("Deterministic large fixture for the cold-start regression gate")
    c.setCreator("generate-large-fixture.py")

    draw_dense_page0(c, width, height)
    c.showPage()
    for n in range(2, PAGE_COUNT + 1):
        draw_filler_page(c, width, height, n)
        c.showPage()
    c.save()


def main():
    parser = argparse.ArgumentParser(description="Generate or verify large.pdf")
    parser.add_argument("--check", action="store_true",
                        help="Regenerate to a temp path and assert byte-identity "
                             "with the committed fixture; exit 1 on drift.")
    args = parser.parse_args()

    if args.check:
        if not os.path.exists(OUT_PATH):
            print("[FAIL] committed fixture missing: %s" % OUT_PATH)
            sys.exit(1)
        with tempfile.TemporaryDirectory() as td:
            tmp = os.path.join(td, "large.pdf")
            build(tmp)
            with open(OUT_PATH, "rb") as f:
                committed = f.read()
            with open(tmp, "rb") as f:
                regenerated = f.read()
        # Enforce the zlib-independence invariant on the dev's own machine: a
        # FlateDecode stream would make the bytes depend on the local zlib build
        # and silently pass --check here while breaking a different-zlib CI.
        flate = regenerated.count(b"/FlateDecode")
        if flate != 0:
            print("[FAIL] large.pdf has %d FlateDecode stream(s); build the canvas "
                  "with pageCompression=0 so the bytes are zlib-independent "
                  "(see module docstring)." % flate)
            sys.exit(1)
        if committed != regenerated:
            print("[FAIL] large.pdf drift: committed=%d B, regenerated=%d B "
                  "(byte-identity broken)" % (len(committed), len(regenerated)))
            sys.exit(1)
        print("[OK] large.pdf byte-identical (%d B)" % len(committed))
        sys.exit(0)

    build(OUT_PATH)
    print("Wrote: %s (%d bytes)" % (os.path.abspath(OUT_PATH), os.path.getsize(OUT_PATH)))


if __name__ == "__main__":
    main()
