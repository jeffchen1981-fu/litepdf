#!/usr/bin/env python3
"""
Generate tests/fixtures/selection.pdf, the fixture for the text-selection engine
tests in tests/unit/test_document_selection.cpp (#52).

Page index -> what it pins:

  0  Two columns of unequal length (8 lines left, 4 right). Select All must copy
     both columns, down to the page's final character.
  1  One baseline, two runs ~270 pt apart. MuPDF starts a new stext line for
     horizontal motion wider than 0.8 em (SPACE_MAX_DIST in stext-device.c), so
     the highlight is two quads rather than one bar across the whitespace.
  2  No text at all.
  3  "alpha beta gamma" over "delta epsilon": word, line and backward-drag
     snapping.
  4  30 rows x 10 runs, 55 pt apart: 300 separate highlight quads -- past the
     256-quad initial buffer, so the grow-and-retry path runs.
  5  CropBox [36 36 576 756], "ORIGIN" drawn at user space (108, 684). MuPDF moves
     the CropBox origin to (0, 0), so the word starts at page space (72, 72).
     MUST STAY THE LAST PAGE: reportlab applies a CropBox to the page that sets
     it and to every page after.

Byte-reproducible: rl_config.invariant pins the timestamp and document ID, and
pageCompression=0 leaves no zlib stream whose bytes would depend on the
interpreter's zlib build (see scripts/generate-large-fixture.py). Generated with
reportlab==4.4.10.

Usage:
  python scripts/generate-selection-fixture.py           # write the fixture
  python scripts/generate-selection-fixture.py --check   # exit 1 if it would change
"""

import argparse
import io
import os
import sys

from reportlab import rl_config

rl_config.invariant = 1

from reportlab.lib.colors import black, white  # noqa: E402
from reportlab.pdfgen import canvas  # noqa: E402

PAGE_W, PAGE_H = 612, 792  # US Letter, points

OUT_PATH = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "selection.pdf"))


def fill_page_white(c):
    """Explicit white page fill; see generate-search-fixture.py for why."""
    c.saveState()
    c.setFillColor(white)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    c.restoreState()
    c.setFillColor(black)


def page_two_columns(c):
    c.setFont("Helvetica", 12)
    for i in range(8):
        c.drawString(72, 720 - 20 * i, "Left column line %d" % (i + 1))
    for i in range(4):
        c.drawString(324, 720 - 20 * i, "Right column line %d" % (i + 1))


def page_wide_gap(c):
    c.setFont("Helvetica", 12)
    c.drawString(72, 720, "LEFTRUN")
    c.drawString(400, 720, "RIGHTRUN")


def page_blank(c):
    pass


def page_words(c):
    c.setFont("Helvetica", 12)
    c.drawString(72, 720, "alpha beta gamma")
    c.drawString(72, 700, "delta epsilon")


def page_many_runs(c):
    c.setFont("Helvetica", 8)
    for row in range(30):
        for col in range(10):
            c.drawString(40 + 55 * col, 740 - 20 * row, "xx")


def page_crop_offset(c):
    c.setCropBox((36, 36, 576, 756))
    c.setFont("Helvetica", 12)
    c.drawString(108, 684, "ORIGIN")


PAGES = [
    page_two_columns,
    page_wide_gap,
    page_blank,
    page_words,
    page_many_runs,
    page_crop_offset,  # must stay last -- see the module docstring
]


def build():
    buf = io.BytesIO()
    c = canvas.Canvas(buf, pagesize=(PAGE_W, PAGE_H), pageCompression=0)
    for draw in PAGES:
        fill_page_white(c)
        draw(c)
        c.showPage()
    c.save()
    return buf.getvalue()


def main():
    parser = argparse.ArgumentParser(
        description="Generate tests/fixtures/selection.pdf")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the committed fixture would change")
    args = parser.parse_args()

    data = build()
    if b"/FlateDecode" in data:
        print("selection.pdf unexpectedly contains a compressed stream",
              file=sys.stderr)
        return 1

    if args.check:
        with open(OUT_PATH, "rb") as f:
            if f.read() != data:
                print("tests/fixtures/selection.pdf is stale; rerun without --check",
                      file=sys.stderr)
                return 1
        return 0

    with open(OUT_PATH, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (OUT_PATH, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
