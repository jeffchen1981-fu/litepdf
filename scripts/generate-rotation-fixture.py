#!/usr/bin/env python3
"""
Generate tests/fixtures/rotation-marker.pdf, the fixture for the on-screen view
rotation tests in tests/unit/test_canvas_rotation.cpp and the smoke direction row
in scripts/smoke-test.ps1 (#53).

Four A4 portrait pages, 595.276 x 841.89 pt, so a page drawn at zoom 1.0 has a
fractional extent on both axes. Positions below are in page space: points from
the page's top-left corner, as MuPDF reports them.

Page index -> what it pins:

  0  A solid black square, [36, 108] x [36, 108], near the top-left corner: a
     turn moves it to another corner of the screen, which a pixel read tells
     apart. And the line "Rotation marker text" (Helvetica 12, baseline 60 pt
     from the top, starting at x = 150), for presses and drag-select on a
     turned page.
  1  Blank.
  2  The single word "ZENITH" (Helvetica 12, baseline 80 pt from the top,
     starting at x = 470), near the top-right corner: at a quarter turn its
     vertical place on screen comes from its x, so a turn in the wrong direction
     puts it several hundred DIP away. Its hit's centre x is kMarkWordCentreXPt
     in tests/unit/CanvasRig.hpp, and the smoke direction row carries the same
     value.
  3  Blank, so that centring the hit on page 2 is never clamped by the end of
     the document.

Byte-reproducible: rl_config.invariant pins the timestamp and document ID, and
pageCompression=0 leaves no zlib stream whose bytes would depend on the
interpreter's zlib build (see scripts/generate-large-fixture.py). Generated with
reportlab==4.4.10.

Usage:
  python scripts/generate-rotation-fixture.py           # write the fixture
  python scripts/generate-rotation-fixture.py --check   # exit 1 if it would change
"""

import argparse
import io
import os
import sys

from reportlab import rl_config

rl_config.invariant = 1

from reportlab.lib.colors import black, white  # noqa: E402
from reportlab.lib.pagesizes import A4  # noqa: E402
from reportlab.pdfgen import canvas  # noqa: E402

PAGE_W, PAGE_H = A4  # 595.276 x 841.89 points

OUT_PATH = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "rotation-marker.pdf"))


def fill_page_white(c):
    """Explicit white page fill; see generate-search-fixture.py for why."""
    c.saveState()
    c.setFillColor(white)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    c.restoreState()
    c.setFillColor(black)


def page_square_and_line(c):
    # Page space [36, 108] x [36, 108]; reportlab's origin is the bottom-left.
    c.rect(36, PAGE_H - 108, 72, 72, fill=1, stroke=0)
    c.setFont("Helvetica", 12)
    c.drawString(150, PAGE_H - 60, "Rotation marker text")


def page_blank(c):
    pass


def page_top_right_word(c):
    c.setFont("Helvetica", 12)
    c.drawString(470, PAGE_H - 80, "ZENITH")


PAGES = [
    page_square_and_line,
    page_blank,
    page_top_right_word,
    page_blank,
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
        description="Generate tests/fixtures/rotation-marker.pdf")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the committed fixture would change")
    args = parser.parse_args()

    data = build()
    if b"/FlateDecode" in data:
        print("rotation-marker.pdf unexpectedly contains a compressed stream",
              file=sys.stderr)
        return 1

    if args.check:
        with open(OUT_PATH, "rb") as f:
            if f.read() != data:
                print("tests/fixtures/rotation-marker.pdf is stale; rerun without --check",
                      file=sys.stderr)
                return 1
        return 0

    with open(OUT_PATH, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (OUT_PATH, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
