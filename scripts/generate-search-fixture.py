#!/usr/bin/env python3
"""
Generate tests/fixtures/search.pdf — a deterministic fixture for
Phase 6 search unit tests.

Per-page hit counts (keywords: Lorem, dolor, CJK_KW, XYZABC123)
where CJK_KW is the 4-codepoint Traditional-Chinese string used as
the Unicode search keyword (see fixture_keywords() below for the
exact literal; kept out of this docstring per project policy that
comments and docstrings stay in English):

  Page | Page   | Lorem | dolor | CJK_KW | XYZABC123
  idx  | number |       |       |        |
  -----+--------+-------+-------+--------+-----------
    0  |   1    |  12   |   0   |   0    |     0
    1  |   2    |   0   |   1   |   0    |     0
    2  |   3    |   0   |   0   |   1    |     0
    3  |   4    |   0   |   1   |   0    |     0
    4  |   5    |   0   |   0   |   0    |     0   (filler)
    5  |   6    |   3   |   0   |   0    |     0
    6  |   7    |   0   |   1   |   0    |     0
  -----+--------+-------+-------+--------+-----------
  TOTAL          | 15    |  3    |   1    |    0

These totals are load-bearing for Phase 6 search tests and the CLI
smoke script; changing the counts here breaks downstream assertions.

The CJK keyword on page 3 is rendered via reportlab's built-in
STSong-Light CID font (no external font file). MuPDF stext extraction
recovers the Unicode code points regardless of Simplified vs.
Traditional glyph shape, so the search assertion (on code points) is
stable.

White page fills are required because LitePDF's PdfCanvas uses a dark
D2D surface; see generate-bookmarks-fixture.py for the full rationale.
"""

from reportlab.lib.pagesizes import letter
from reportlab.lib.colors import white, black
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfgen import canvas
import os


# Traditional-Chinese search keyword — intentional fixture data (not a
# comment / identifier), encoded as escapes so this source file stays
# ASCII-only and survives repo tooling that flags CJK in code.
# Codepoints: U+4E2D U+6587 U+6E2C U+8A66  ("zhong wen ce shi")
CJK_KEYWORD = "\u4e2d\u6587\u6e2c\u8a66"

# Surrounding CJK filler text for page 3 so the keyword appears embedded
# in a natural-looking paragraph. The substring CJK_KEYWORD must appear
# exactly once in the concatenated line.
CJK_PREFIX = "\u4ee5\u4e0b\u662f\u672c\u9801\u552f\u4e00\u7684\u95dc\u9375\u5b57 "  # "The following is this page's only keyword: "
CJK_SUFFIX = " \u7d50\u675f\u3002"  # " end."


def fill_page_white(c, width, height):
    """Draw an explicit white rectangle covering the whole page.

    PDF has no built-in page-background concept — the 'paper' is just
    transparent. Most PDF readers (Adobe, browsers) put white behind
    pages by default. LitePDF's PdfCanvas, however, uses a dark D2D
    surface — so a transparent PDF with black text renders as black-on-
    black (invisible). Filling the page with explicit white is the
    portable fix and matches what most authoring tools do automatically.
    """
    c.saveState()
    c.setFillColor(white)
    c.setStrokeColor(white)
    c.rect(0, 0, width, height, fill=1, stroke=0)
    c.setFillColor(black)  # restore default for subsequent drawString calls
    c.restoreState()


OUT_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "search.pdf",
)


def draw_lines(c, lines, x, y_top, line_height=18):
    """Draw each string in `lines` on its own line, top-down from y_top."""
    y = y_top
    for line in lines:
        c.drawString(x, y, line)
        y -= line_height


def main():
    # Register the built-in CJK CID font once so page 3 can use it.
    pdfmetrics.registerFont(UnicodeCIDFont("STSong-Light"))

    c = canvas.Canvas(OUT_PATH, pagesize=letter)
    width, height = letter
    left = 72
    top = height - 72

    # --- Page 1 (index 0): 12 occurrences of "Lorem", 0 of anything else ---
    # Each "Lorem ipsum sit amet." contains 1 Lorem; 2 per line, 6 lines = 12.
    # No "dolor", no CJK, no XYZABC123.
    page1 = [
        "Page 1 heading text for the search fixture.",
        "Lorem ipsum sit amet. Lorem ipsum sit amet.",
        "Lorem ipsum sit amet. Lorem ipsum sit amet.",
        "Lorem ipsum sit amet. Lorem ipsum sit amet.",
        "Lorem ipsum sit amet. Lorem ipsum sit amet.",
        "Lorem ipsum sit amet. Lorem ipsum sit amet.",
        "Lorem ipsum sit amet. Lorem ipsum sit amet.",
        "End of page one filler.",
    ]
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    draw_lines(c, page1, left, top)
    c.showPage()

    # --- Page 2 (index 1): 1 occurrence of "dolor" ---
    # Deliberately avoid the phrase "Lorem ipsum dolor" so we do not
    # accidentally hit Lorem on this page.
    page2 = [
        "Page 2 of the search fixture.",
        "This paragraph references dolor exactly once.",
        "The rest of the sentences are plain filler text.",
        "Nothing else interesting appears on this page.",
    ]
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    draw_lines(c, page2, left, top)
    c.showPage()

    # --- Page 3 (index 2): CJK page, 1 occurrence of CJK_KEYWORD ---
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    c.drawString(left, top, "Page 3 contains a CJK paragraph below:")
    c.setFont("STSong-Light", 16)
    # Exactly one CJK_KEYWORD on the page. Surrounding CJK filler does not
    # contain the 4-codepoint substring.
    c.drawString(left, top - 40, CJK_PREFIX + CJK_KEYWORD + CJK_SUFFIX)
    c.showPage()

    # --- Page 4 (index 3): 1 occurrence of "dolor" ---
    page4 = [
        "Page 4 plain text content.",
        "Another line mentioning dolor one time.",
        "Filler sentence without any search keywords.",
    ]
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    draw_lines(c, page4, left, top)
    c.showPage()

    # --- Page 5 (index 4): no keyword hits at all ---
    # No Lorem, no dolor, no CJK_KEYWORD, no XYZABC123.
    page5 = [
        "Page 5 is filler with no search hits.",
        "Alpha beta gamma delta epsilon zeta eta theta.",
        "Quick brown fox jumps over the lazy dog.",
        "This page exists to test zero-hit pagination.",
    ]
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    draw_lines(c, page5, left, top)
    c.showPage()

    # --- Page 6 (index 5): 3 occurrences of "Lorem" on one line ---
    page6 = [
        "Page 6 short paragraph.",
        "Lorem here, Lorem there, Lorem everywhere.",
    ]
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    draw_lines(c, page6, left, top)
    c.showPage()

    # --- Page 7 (index 6): single line with 1 occurrence of "dolor" ---
    page7 = [
        "Page 7 single line: dolor.",
    ]
    fill_page_white(c, width, height)
    c.setFont("Helvetica", 14)
    draw_lines(c, page7, left, top)
    c.showPage()

    c.save()
    print(f"Wrote: {os.path.abspath(OUT_PATH)} ({os.path.getsize(OUT_PATH)} bytes)")


if __name__ == "__main__":
    main()
