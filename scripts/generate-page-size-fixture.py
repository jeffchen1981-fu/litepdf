#!/usr/bin/env python3
"""
Generate tests/fixtures/page-size.pdf, the fixture for Document::page_size
(#165) in tests/unit/test_document_page_size.cpp.

  Page 1  No MediaBox of its own: it inherits [0 0 612 792] from the page-tree
          node. CropBox [36 72 576 720], Rotate 90, UserUnit 2, so its size
          is 648 x 540 crop points, doubled by UserUnit and turned on its
          side: 1296 wide, 1080 high. Every input of the bounds has a case.
  Page 2  Ten link annotations and an image XObject. Loading the page loads
          all eleven objects (annotations, and the transparency scan of the
          resources); sizing it must load none of them. They sit between two
          64 KiB padding streams, away from the page objects at the start and
          the xref at the end, so reading them needs a file read of its own.

Written as literal uncompressed PDF objects with standard library only, so the
bytes are identical across runs: no timestamps, no /ID, xref offsets computed
from the bytes actually written.

Usage:
  python -I scripts/generate-page-size-fixture.py
"""

import os

FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))

ANNOT_COUNT = 10
PADDING = 64 * 1024


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


def page_size_pdf():
    # Objects 1-6 come first; 7 is padding, 8..17 the annotations, 18 the
    # image, 19 padding again. Nothing refers to the padding streams.
    first_annot = 8
    image = first_annot + ANNOT_COUNT
    annots = b" ".join(b"%d 0 R" % n
                       for n in range(first_annot, first_annot + ANNOT_COUNT))
    padding = stream_obj(b"", b"%" * PADDING)
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2"
        b" /MediaBox [0 0 612 792] >>",
        b"<< /Type /Page /Parent 2 0 R /CropBox [36 72 576 720] /Rotate 90"
        b" /UserUnit 2 /Contents 5 0 R /Resources << >> >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842]"
        b" /Contents 6 0 R /Resources << /XObject << /Im0 %d 0 R >> >>"
        b" /Annots [" % image + annots + b"] >>",
        stream_obj(b"", b"0 0 1 rg 100 100 200 200 re f\n"),
        stream_obj(b"", b"q 100 0 0 100 72 600 cm /Im0 Do Q\n"),
        padding,
    ]
    for k in range(ANNOT_COUNT):
        y = 700 - 40 * k
        objects.append(b"<< /Type /Annot /Subtype /Link /Rect [72 %d 300 %d]"
                       b" /Border [0 0 0] /A << /S /URI /URI (https://example.com/%d) >> >>"
                       % (y, y + 20, k))
    objects.append(stream_obj(b"/Type /XObject /Subtype /Image /Width 1"
                              b" /Height 1 /ColorSpace /DeviceGray"
                              b" /BitsPerComponent 8", b"\x80"))
    objects.append(padding)
    return build_pdf(objects)


def main():
    # Binary mode: Windows must not translate the \n line ends.
    with open(os.path.join(FIXTURE_DIR, "page-size.pdf"), "wb") as f:
        f.write(page_size_pdf())


if __name__ == "__main__":
    main()
