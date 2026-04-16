#!/usr/bin/env python3
"""
Generate tests/fixtures/bookmarks.pdf — a test fixture with:
  - 3 pages with visible content
  - Outline tree:
      Chapter 1 (page 1)
        └─ Section 1.1 (page 2)
      Chapter 2 (page 3)

Replaces the previous bookmarks.pdf which had a corrupt content stream
(MuPDF reported "premature end of data in flate filter" and rendered
black pages). New fixture has clean compressed streams so litepdf can
actually render the page content during UX testing.

Test contract preserved (per tests/unit/test_document_outline.cpp):
  - entries.size() >= 3
  - at least one entry with depth > 0 (Section 1.1 is nested under Chapter 1)
  - all titles non-empty
"""

from reportlab.lib.pagesizes import letter
from reportlab.lib.colors import white, black
from reportlab.pdfgen import canvas
import os


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
    "..", "tests", "fixtures", "bookmarks.pdf",
)


def main():
    c = canvas.Canvas(OUT_PATH, pagesize=letter)
    width, height = letter

    # --- Page 1: Chapter 1 ---
    c.bookmarkPage("ch1")
    fill_page_white(c, width, height)
    c.setFont("Helvetica-Bold", 28)
    c.drawString(72, height - 100, "Chapter 1")
    c.setFont("Helvetica", 14)
    c.drawString(72, height - 140, "This is the first chapter of the test document.")
    c.drawString(72, height - 160, "It exists so litepdf has visible page content")
    c.drawString(72, height - 180, "to render during outline-pane UX verification.")
    c.showPage()

    # --- Page 2: Section 1.1 ---
    c.bookmarkPage("sec1_1")
    fill_page_white(c, width, height)
    c.setFont("Helvetica-Bold", 22)
    c.drawString(72, height - 100, "Section 1.1")
    c.setFont("Helvetica", 14)
    c.drawString(72, height - 140, "A nested section, child of Chapter 1.")
    c.drawString(72, height - 160, "Clicking 'Section 1.1' in the outline pane")
    c.drawString(72, height - 180, "should navigate to this page (page index 1).")
    c.showPage()

    # --- Page 3: Chapter 2 ---
    c.bookmarkPage("ch2")
    fill_page_white(c, width, height)
    c.setFont("Helvetica-Bold", 28)
    c.drawString(72, height - 100, "Chapter 2")
    c.setFont("Helvetica", 14)
    c.drawString(72, height - 140, "Second top-level chapter, sibling of Chapter 1.")
    c.drawString(72, height - 160, "Verifies sibling depth handling in OutlinePane")
    c.drawString(72, height - 180, "depth_stack algorithm.")
    c.showPage()

    # --- Outline tree ---
    # addOutlineEntry(title, key, level=0, closed=None)
    c.addOutlineEntry("Chapter 1", "ch1", level=0)
    c.addOutlineEntry("Section 1.1", "sec1_1", level=1)
    c.addOutlineEntry("Chapter 2", "ch2", level=0)
    c.showOutline()

    c.save()
    print(f"Wrote: {os.path.abspath(OUT_PATH)} ({os.path.getsize(OUT_PATH)} bytes)")


if __name__ == "__main__":
    main()
