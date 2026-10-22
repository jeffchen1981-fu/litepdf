#!/usr/bin/env python3
"""
Generate the continuous-scroll strip fixtures (#55, spec section 9.4):

  tests/fixtures/strip-1000.pdf        1000 Letter pages, one heading line each
                                       ("Strip page N of 1000."); every page with
                                       0-based index i % 10 == 9 (9, 19, ...) is
                                       landscape (792 x 612) so page widths differ.
  tests/fixtures/strip-chapters.epub   200 chapters, each one <h1> and 10 identical
                                       Lorem ipsum paragraphs (each chapter spills
                                       onto a second page in litepdf-cli).
  tests/fixtures/strip-images.cbz      300 PNGs, 200 x 300 grayscale, each a flat
                                       fill with its index drawn in Pillow's
                                       default font, named 001.png ... 300.png.

They feed the page-sizing gate (spec section 4.4): litepdf-cli
--bench-page-sizes times DocumentView::page_sizes() on each of them. The
existing sample.epub / sample.cbz are far too small to produce a failing reading
for a 100 ms gate.

Determinism (the --check contract, as in generate-large-fixture.py):
  * PDF: reportlab invariant mode (rl_config.invariant = 1) pins the timestamp
    and /ID, and the canvas is built with pageCompression=0 so no stream is
    zlib-compressed. Compressed streams would depend on the interpreter's zlib
    build (zlib-ng on CPython 3.14+ for Windows, stock zlib before), so a
    compressed fixture would drift between a 3.14 dev box and an older CI
    Python. Generated with reportlab==4.4.10.
  * EPUB and CBZ: written with zipfile, every entry ZIP_STORED (deflate would
    reintroduce the zlib dependency), a fixed date_time, and a fixed
    create_system / external_attr (zipfile's defaults differ by platform).
  * The CBZ's PNG bytes are compressed by Pillow's own bundled zlib, so they are
    reproducible only with the pinned Pillow version (12.2.0); the benchmark
    workflow installs that version before running --check.

--check regenerates all three to a temp dir, asserts byte identity with the
committed files, asserts the PDF has zero FlateDecode streams and that every
zip entry is stored, and exits 1 on any drift.

--pdf-pages N --pdf-out PATH writes only an N-page PDF of the same shape to
PATH (the 10,000-page measurement of spec section 4.4); it is never committed.

White page fills are required because LitePDF's PdfCanvas uses a dark D2D
surface; see generate-search-fixture.py for the full rationale.

Usage:
  python scripts/generate-strip-fixtures.py            # write the three fixtures
  python scripts/generate-strip-fixtures.py --check    # verify byte identity
  python scripts/generate-strip-fixtures.py --pdf-pages 10000 --pdf-out %TEMP%\\strip-10000.pdf
"""

import argparse
import io
import os
import sys
import tempfile
import zipfile

# Invariant mode MUST be set before the pdfgen machinery stamps the document.
from reportlab import rl_config
rl_config.invariant = 1

from reportlab.lib.pagesizes import letter
from reportlab.lib.colors import white, black
from reportlab.pdfgen import canvas

from PIL import Image, ImageDraw, ImageFont


FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))

PDF_NAME = "strip-1000.pdf"
EPUB_NAME = "strip-chapters.epub"
CBZ_NAME = "strip-images.cbz"

PDF_PAGES = 1000
EPUB_CHAPTERS = 200
EPUB_PARAGRAPHS = 10
CBZ_IMAGES = 300
CBZ_SIZE = (200, 300)

ZIP_DATE_TIME = (2026, 1, 1, 0, 0, 0)

LOREM = ("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
         "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
         "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
         "commodo consequat.")


# --- PDF --------------------------------------------------------------------

def fill_page_white(c, width, height):
    """Draw an explicit white rectangle over the whole page (dark-surface fix)."""
    c.saveState()
    c.setFillColor(white)
    c.setStrokeColor(white)
    c.rect(0, 0, width, height, fill=1, stroke=0)
    c.restoreState()


def build_pdf(out_path, page_count):
    portrait = letter                     # 612 x 792
    landscape = (letter[1], letter[0])    # 792 x 612
    # pageCompression=0: see the module docstring.
    c = canvas.Canvas(out_path, pagesize=portrait, pageCompression=0)
    c.setTitle("LitePDF continuous-scroll strip fixture")
    c.setAuthor("litepdf")
    c.setSubject("Deterministic %d-page fixture for the page-sizing gate" % page_count)
    c.setCreator("generate-strip-fixtures.py")

    for i in range(page_count):
        width, height = landscape if i % 10 == 9 else portrait
        c.setPageSize((width, height))
        fill_page_white(c, width, height)
        c.setFillColor(black)
        c.setFont("Helvetica", 12)
        c.drawString(72, height - 72, "Strip page %d of %d." % (i + 1, page_count))
        c.showPage()
    c.save()


# --- zip helpers (EPUB, CBZ) ------------------------------------------------

def stored_entry(name):
    """A ZipInfo whose header bytes do not depend on the platform or the clock."""
    info = zipfile.ZipInfo(name, date_time=ZIP_DATE_TIME)
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 0      # zipfile's default is 0 on Windows, 3 elsewhere
    info.external_attr = 0
    return info


def write_stored_zip(out_path, entries):
    """Writes (name, bytes) pairs, in order, as stored entries."""
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_STORED) as z:
        for name, data in entries:
            z.writestr(stored_entry(name), data)


# --- EPUB -------------------------------------------------------------------

CONTAINER_XML = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""


def chapter_name(n):
    return "ch%03d.xhtml" % n


def content_opf():
    manifest = "\n".join(
        '    <item id="ch%03d" href="%s" media-type="application/xhtml+xml"/>'
        % (n, chapter_name(n)) for n in range(1, EPUB_CHAPTERS + 1))
    spine = "\n".join(
        '    <itemref idref="ch%03d"/>' % n for n in range(1, EPUB_CHAPTERS + 1))
    return """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>LitePDF strip fixture</dc:title>
    <dc:language>en</dc:language>
    <dc:identifier id="bookid">litepdf-strip-chapters</dc:identifier>
  </metadata>
  <manifest>
%s
  </manifest>
  <spine>
%s
  </spine>
</package>
""" % (manifest, spine)


def chapter_xhtml(n):
    paragraphs = "\n".join("<p>%s</p>" % LOREM for _ in range(EPUB_PARAGRAPHS))
    return """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Chapter %d</title></head>
<body>
<h1>Chapter %d</h1>
%s
</body>
</html>
""" % (n, n, paragraphs)


def build_epub(out_path):
    entries = [
        ("mimetype", b"application/epub+zip"),
        ("META-INF/container.xml", CONTAINER_XML.encode("utf-8")),
        ("OEBPS/content.opf", content_opf().encode("utf-8")),
    ]
    for n in range(1, EPUB_CHAPTERS + 1):
        entries.append(("OEBPS/" + chapter_name(n), chapter_xhtml(n).encode("utf-8")))
    write_stored_zip(out_path, entries)


# --- CBZ --------------------------------------------------------------------

def cbz_png(n, font):
    img = Image.new("L", CBZ_SIZE, 230)
    ImageDraw.Draw(img).text((20, 20), "%d" % n, fill=0, font=font)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def build_cbz(out_path):
    font = ImageFont.load_default()
    entries = [("%03d.png" % n, cbz_png(n, font)) for n in range(1, CBZ_IMAGES + 1)]
    write_stored_zip(out_path, entries)


# --- driver -----------------------------------------------------------------

BUILDERS = [
    (PDF_NAME, lambda p: build_pdf(p, PDF_PAGES)),
    (EPUB_NAME, build_epub),
    (CBZ_NAME, build_cbz),
]


def check():
    failed = False
    with tempfile.TemporaryDirectory() as td:
        for name, build in BUILDERS:
            committed_path = os.path.join(FIXTURE_DIR, name)
            if not os.path.exists(committed_path):
                print("[FAIL] committed fixture missing: %s" % committed_path)
                failed = True
                continue
            tmp = os.path.join(td, name)
            build(tmp)
            with open(committed_path, "rb") as f:
                committed = f.read()
            with open(tmp, "rb") as f:
                regenerated = f.read()

            # The zlib-independence invariants, enforced on the dev's own
            # machine so a compressed stream or entry cannot pass here and
            # then drift on a CI with a different zlib build.
            if name.endswith(".pdf"):
                flate = regenerated.count(b"/FlateDecode")
                if flate != 0:
                    print("[FAIL] %s has %d FlateDecode stream(s); build the canvas "
                          "with pageCompression=0 (see module docstring)."
                          % (name, flate))
                    failed = True
                    continue
            else:
                with zipfile.ZipFile(tmp) as z:
                    deflated = [i.filename for i in z.infolist()
                                if i.compress_type != zipfile.ZIP_STORED]
                if deflated:
                    print("[FAIL] %s has %d compressed entr(y/ies), first %s; every "
                          "entry must be ZIP_STORED (see module docstring)."
                          % (name, len(deflated), deflated[0]))
                    failed = True
                    continue

            if committed != regenerated:
                print("[FAIL] %s drift: committed=%d B, regenerated=%d B "
                      "(byte-identity broken)" % (name, len(committed), len(regenerated)))
                failed = True
                continue
            print("[OK] %s byte-identical (%d B)" % (name, len(committed)))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description="Generate or verify the strip fixtures")
    parser.add_argument("--check", action="store_true",
                        help="Regenerate to a temp dir and assert byte identity "
                             "with the committed fixtures; exit 1 on drift.")
    parser.add_argument("--pdf-pages", type=int,
                        help="Write only an N-page strip PDF (needs --pdf-out).")
    parser.add_argument("--pdf-out",
                        help="Where --pdf-pages writes; never a committed path.")
    args = parser.parse_args()

    if (args.pdf_pages is None) != (args.pdf_out is None):
        parser.error("--pdf-pages and --pdf-out go together")
    if args.pdf_pages is not None:
        if args.check:
            parser.error("--check does not take --pdf-pages / --pdf-out")
        if args.pdf_pages < 1:
            parser.error("--pdf-pages must be >= 1")
        build_pdf(args.pdf_out, args.pdf_pages)
        print("Wrote: %s (%d pages, %d bytes)"
              % (os.path.abspath(args.pdf_out), args.pdf_pages,
                 os.path.getsize(args.pdf_out)))
        return 0

    if args.check:
        return check()

    for name, build in BUILDERS:
        path = os.path.join(FIXTURE_DIR, name)
        build(path)
        print("Wrote: %s (%d bytes)" % (path, os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
