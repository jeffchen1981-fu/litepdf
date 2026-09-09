#!/usr/bin/env python3
"""
Generate tests/fixtures/spread-unequal.pdf -- a two-page document whose pages
differ in size, for the PR-A1 two-page-spread fit tests.

  Page | Page   | Size (pt)
  idx  | number |
  -----+--------+------------------
    0  |   1    | 420 x 595  (A5)
    1  |   2    | 595 x 842  (A4)

The size DIFFERENCE is the whole point: one render scale is shared by both
slots of a spread, so a fit derived from the left page alone overflows the
slot holding the right one. A fixture with two equal pages cannot tell a
correct implementation from one that ignores the pair.

pageCompression=0 keeps every content stream uncompressed so the output bytes
are zlib-independent (zlib-ng vs stock zlib), matching
generate-cjk-fixture.py:45 and generate-large-fixture.py:119.

White page fills are required because LitePDF's PdfCanvas uses a dark D2D
surface; see generate-bookmarks-fixture.py for the full rationale.
"""

import os

from reportlab.lib.colors import black, white
from reportlab.pdfgen import canvas

# Script-relative, like every other generator here -- generate-search-fixture.py:77,
# generate-large-fixture.py:56 and generate-bookmarks-fixture.py:44 all resolve the
# output this way so the script works from any cwd.
OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures",
                   "spread-unequal.pdf")
PAGES = [(420.0, 595.0), (595.0, 842.0)]


def main():
    c = canvas.Canvas(OUT, pageCompression=0)
    for i, (w, h) in enumerate(PAGES):
        c.setPageSize((w, h))
        c.setFillColor(white)
        c.rect(0, 0, w, h, stroke=0, fill=1)
        c.setFillColor(black)
        c.setFont("Helvetica", 24)
        c.drawString(40, h - 60, f"spread page {i + 1}  {int(w)}x{int(h)}")
        c.showPage()
    c.save()


if __name__ == "__main__":
    main()
