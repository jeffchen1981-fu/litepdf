#!/usr/bin/env python3
"""
Generate tests/fixtures/damaged-pages.pdf: eight pages, some of which cannot be
looked up, for the code that sizes and pairs pages.

  Page | Spread | Size (pt)
  idx  | slot   |
  -----+--------+------------
    0  | cover  | 420 x 595
    1  | left   | 420 x 400
    2  | right  | damaged
    3  | left   | damaged
    4  | right  | 300 x 800
    5  | left   | damaged
    6  | right  | damaged
    7  | tail   | 500 x 500

A damaged page is a /Pages node that contains itself and counts one page, so
every other page still resolves past it (its /Count is skipped) while looking up
this one throws "cycle in page tree". A /Pages node with an empty /Kids does NOT
do this: MuPDF renders it as a blank page. Page 0 cannot be damaged in this way,
because opening a document looks page 0 up and fails there (#119).

Every good page is a solid colour, so a screen capture shows which one landed.

Written by hand with no PDF library, like generate-page-size-fixture.py: the
bytes depend on nothing but this file.
"""

import os

# Objects 1 and 2 are the catalog and the page tree. Each entry below takes one
# object, or two for a good page and its content stream.
PAGES = [
    (420, 595, "0.80 0.20 0.20"),
    (420, 400, "0.20 0.60 0.20"),
    None,
    None,
    (300, 800, "0.20 0.20 0.80"),
    None,
    None,
    (500, 500, "0.80 0.60 0.20"),
]


def build():
    bodies = {}
    kids = []
    number = 3
    for page in PAGES:
        kids.append(number)
        if page is None:
            bodies[number] = (
                f"<< /Type /Pages /Parent 2 0 R /Count 1 "
                f"/Kids [{number} 0 R] >>"
            ).encode("ascii")
            number += 1
            continue
        width, height, colour = page
        stream = f"{colour} rg 0 0 {width} {height} re f\n".encode("ascii")
        bodies[number] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {width} {height}] "
            f"/Resources << >> /Contents {number + 1} 0 R >>"
        ).encode("ascii")
        bodies[number + 1] = (
            f"<< /Length {len(stream)} >>\nstream\n".encode("ascii")
            + stream
            + b"endstream"
        )
        number += 2

    bodies[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    refs = " ".join(f"{kid} 0 R" for kid in kids)
    bodies[2] = f"<< /Type /Pages /Count {len(PAGES)} /Kids [{refs}] >>".encode("ascii")

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for n in sorted(bodies):
        offsets[n] = len(out)
        out += f"{n} 0 obj\n".encode("ascii") + bodies[n] + b"\nendobj\n"

    xref_at = len(out)
    size = len(bodies) + 1
    out += f"xref\n0 {size}\n0000000000 65535 f \n".encode("ascii")
    for n in range(1, size):
        out += f"{offsets[n]:010d} 00000 n \n".encode("ascii")
    out += (
        f"trailer\n<< /Size {size} /Root 1 0 R >>\n"
        f"startxref\n{xref_at}\n%%EOF\n"
    ).encode("ascii")
    return bytes(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "tests", "fixtures", "damaged-pages.pdf")
    with open(path, "wb") as f:
        f.write(build())
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
