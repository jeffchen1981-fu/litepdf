#!/usr/bin/env python3
"""
Generate two fixtures whose pages differ in size:

tests/fixtures/spread-unequal.pdf -- a two-page document for the PR-A1
two-page-spread fit tests.

  Page | Page   | Size (pt)
  idx  | number |
  -----+--------+------------------
    0  |   1    | 420 x 595  (A5)
    1  |   2    | 595 x 842  (A4)

The size DIFFERENCE is the whole point: one render scale is shared by both
slots of a spread, so a fit derived from the left page alone overflows the
slot holding the right one. A fixture with two equal pages cannot tell a
correct implementation from one that ignores the pair. It only feeds
DocumentView's pair fit: in the app, page 0 is the cover and page 1 the odd
tail, so this file never shows two pages side by side.

tests/fixtures/spread-pairs.pdf -- five pages that really do pair on screen
(#171, #191). Every page is 420 pt wide; the heights differ so that at
FitWidth in a window about 1000 x 850 DIP, exactly one page of each spread
overflows the viewport.

  Page | Spread | Size (pt)  | "needle" text tops (pt from the page top)
  idx  | slot   |            |
  -----+--------+------------+------------------------------------------
    0  | cover  | 420 x 595  | none
    1  | left   | 420 x 400  | 200
    2  | right  | 420 x 1600 | 100, 860, 900, 940, 1500
    3  | left   | 420 x 1600 | 150, 1500
    4  | right  | 420 x 400  | 200, 340

Each page carries a ruler down its left edge: a 16 pt wide column of 100 pt
blocks, each block its own colour, so a screen capture can read where a page
sits (see ruler_colour).

pageCompression=0 keeps every content stream uncompressed so the output bytes
are zlib-independent (zlib-ng vs stock zlib), matching
generate-cjk-fixture.py:45 and generate-large-fixture.py:119.

White page fills are required because LitePDF's PdfCanvas uses a dark D2D
surface; see generate-bookmarks-fixture.py for the full rationale.
"""

import os

from reportlab.lib.colors import Color, black, white
from reportlab.pdfgen import canvas

# Script-relative, like every other generator here -- generate-search-fixture.py:77,
# generate-large-fixture.py:56 and generate-bookmarks-fixture.py:44 all resolve the
# output this way so the script works from any cwd.
OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures",
                   "spread-unequal.pdf")
PAGES = [(420.0, 595.0), (595.0, 842.0)]

OUT_PAIRS = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures",
                         "spread-pairs.pdf")
# (width, height, tops of the "needle" text, in points from the page top)
PAIR_PAGES = [
    (420.0, 595.0, []),
    (420.0, 400.0, [200]),
    (420.0, 1600.0, [100, 860, 900, 940, 1500]),
    (420.0, 1600.0, [150, 1500]),
    (420.0, 400.0, [200, 340]),
]
RULER_W = 16.0
RULER_BLOCK = 100.0


def ruler_colour(block):
    # Block N from the page top. Green walks up in steps wide enough to survive
    # a capture; red stays 0, which no page fill or hit overlay has.
    return Color(0.0, (40 + 12 * block) / 255.0, 200 / 255.0)


def draw_page(c, i, w, h):
    c.setPageSize((w, h))
    c.setFillColor(white)
    c.rect(0, 0, w, h, stroke=0, fill=1)
    c.setFillColor(black)
    c.setFont("Helvetica", 24)
    c.drawString(40, h - 60, f"spread page {i + 1}  {int(w)}x{int(h)}")


def write_unequal():
    c = canvas.Canvas(OUT, pageCompression=0)
    for i, (w, h) in enumerate(PAGES):
        draw_page(c, i, w, h)
        c.showPage()
    c.save()


def write_pairs():
    c = canvas.Canvas(OUT_PAIRS, pageCompression=0, invariant=1)
    for i, (w, h, needles) in enumerate(PAIR_PAGES):
        draw_page(c, i, w, h)
        for block in range(int(h // RULER_BLOCK) + 1):
            top = block * RULER_BLOCK
            bottom = min(top + RULER_BLOCK, h)
            if bottom <= top:
                continue
            c.setFillColor(ruler_colour(block))
            c.rect(0, h - bottom, RULER_W, bottom - top, stroke=0, fill=1)
        c.setFillColor(black)
        c.setFont("Helvetica", 18)
        for top in needles:
            c.drawString(60, h - top - 18, "needle")
        c.showPage()
    c.save()


def main():
    write_unequal()
    write_pairs()


if __name__ == "__main__":
    main()
