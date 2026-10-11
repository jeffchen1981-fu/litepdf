#!/usr/bin/env python3
"""
Generate tests/fixtures/type3.pdf and tests/fixtures/annot.pdf, the fixtures for
the text-page prefetch tests (#98) in tests/unit/test_document_selection.cpp and
the prefetch equivalence oracle.

  type3.pdf  One page whose text "ABBA" is drawn with a hand-written Type3 font:
             glyphs A and B are filled rectangles, /Encoding /Differences maps
             codes 65 and 66 to them. The glyph names are uppercase on purpose:
             MuPDF maps Type3 glyph names to Unicode through the Adobe Glyph List
             (pdf-type3.c), so extraction yields exactly "ABBA". No other
             fixture has a Type3 font, so this one covers the Type3 teardown path
             (a text page that outlives its Document, pdf-xref.c / font.c).
  annot.pdf  One page with the body text "Body text" (Helvetica, at 72,700) and
             a /FreeText annotation whose /AP /N form XObject draws "Annotation
             text". Document::text_page extracts from the page without its
             annotations, but a display list made by fz_new_display_list_from_page
             includes them, so a text page built from that list contains the
             annotation text -- which is what tells the two builders apart.

Written as literal uncompressed PDF objects with standard library only, so the
bytes are identical across runs: no timestamps, no /ID, xref offsets computed
from the bytes actually written.

Usage:
  python -I scripts/generate-prefetch-fixtures.py   # write both fixtures
"""

import os

FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))


def stream_obj(dict_body, data):
    """An object body for a stream with `dict_body` entries and raw `data`."""
    return (b"<< " + dict_body + b" /Length " + str(len(data)).encode("ascii")
            + b" >>\nstream\n" + data + b"\nendstream")


def build_pdf(objects):
    """Serialise `objects` (object bodies, numbered 1..n; object 1 is the
    catalog) with a classic xref table whose offsets match the bytes written."""
    out = bytearray(b"%PDF-1.7\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += str(number).encode("ascii") + b" 0 obj\n" + body + b"\nendobj\n"
    xref_pos = len(out)
    out += b"xref\n0 " + str(len(objects) + 1).encode("ascii") + b"\n"
    out += b"0000000000 65535 f \n"           # every entry is exactly 20 bytes
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += (b"trailer\n<< /Size " + str(len(objects) + 1).encode("ascii")
            + b" /Root 1 0 R >>\nstartxref\n" + str(xref_pos).encode("ascii")
            + b"\n%%EOF\n")
    return bytes(out)


def type3_pdf():
    page_content = b"BT /T3 24 Tf 72 700 Td (ABBA) Tj ET\n"
    glyph_a = b"600 0 0 0 600 700 d1\n0 0 600 700 re f\n"
    glyph_b = b"800 0 0 0 800 700 d1\n0 0 800 700 re f\n"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
        b" /Contents 4 0 R /Resources << /Font << /T3 5 0 R >> >> >>",
        stream_obj(b"", page_content),
        b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000]"
        b" /FontMatrix [0.001 0 0 0.001 0 0]"
        b" /CharProcs << /A 6 0 R /B 7 0 R >>"
        b" /Encoding << /Type /Encoding /Differences [65 /A /B] >>"
        b" /FirstChar 65 /LastChar 66 /Widths [600 800] >>",
        stream_obj(b"", glyph_a),
        stream_obj(b"", glyph_b),
    ]
    return build_pdf(objects)


def annot_pdf():
    page_content = b"BT /F1 12 Tf 72 700 Td (Body text) Tj ET\n"
    appearance = b"BT /F1 12 Tf 4 12 Td (Annotation text) Tj ET\n"
    helvetica = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
        b" /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >>"
        b" /Annots [6 0 R] >>",
        stream_obj(b"", page_content),
        helvetica,
        b"<< /Type /Annot /Subtype /FreeText /Rect [72 600 300 640] /F 4"
        b" /Contents (Annotation text) /DA (/F1 12 Tf 0 g)"
        b" /AP << /N 7 0 R >> >>",
        stream_obj(b"/Type /XObject /Subtype /Form /BBox [0 0 228 40]"
                   b" /Resources << /Font << /F1 5 0 R >> >>", appearance),
    ]
    return build_pdf(objects)


def write_fixture(name, data):
    # Binary mode: Windows must not translate the \n line ends.
    with open(os.path.join(FIXTURE_DIR, name), "wb") as f:
        f.write(data)


def main():
    write_fixture("type3.pdf", type3_pdf())
    write_fixture("annot.pdf", annot_pdf())


if __name__ == "__main__":
    main()
