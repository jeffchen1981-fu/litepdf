#!/usr/bin/env python3
"""
Generate tests/fixtures/cbz-broken-first.cbz, whose page 0 cannot be sized, for
the US Letter fallback in DocumentView::set_viewport. A PDF cannot give that:
opening one looks page 0 up and fails there (#119). A CBZ opens without
decoding an image, and MuPDF bounds an image it cannot decode by fz_empty_rect
without throwing, which the code that sizes pages must treat as no size at all.

  Page | Entry           | Size (pt)
  idx  |                 |
  -----+-----------------+------------
    0  | p1_broken.jpg   | not usable
    1  | p2.png          | 300 x 450
    2  | p3_broken.jpg   | not usable
    3  | p4_broken.jpg   | not usable

Page 1 shares neither dimension with Letter (612 x 792), so a fit that kept its
zoom could not pass for Letter's. Pages 2 and 3 are a pair with nothing to fit.

The broken entries are the same bytes as p11_broken.jpg in cbz-sizing.cbz. That
file cannot serve: its page 0 must stay good for the tests that index it. The
zip structure is fixed as in generate-cbz-sizing-fixture.py, and the PNG comes
from Pillow's codec, so the file is byte-identical only with the same Pillow
build. Generated with Pillow 12.2.0 (zlib-ng 2.3.3).

Usage:
  python scripts/generate-cbz-broken-first-fixture.py
"""

import io
import os
import zipfile

from PIL import Image


FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))
CBZ_NAME = "cbz-broken-first.cbz"

ZIP_DATE_TIME = (2026, 1, 1, 0, 0, 0)


def broken_jpeg():
    # A JPEG signature and 1 KB of junk: MuPDF cannot decode it.
    return b"\xff\xd8\xff" + bytes(range(256)) * 4


def good_png():
    buf = io.BytesIO()
    Image.new("RGB", (300, 450), (200, 120, 40)).save(buf, format="PNG", dpi=(72, 72))
    return buf.getvalue()


def build_entries():
    return [
        ("p1_broken.jpg", broken_jpeg()),
        ("p2.png", good_png()),
        ("p3_broken.jpg", broken_jpeg()),
        ("p4_broken.jpg", broken_jpeg()),
    ]


def zip_entry(name):
    """A ZipInfo whose header bytes do not depend on the platform or the clock."""
    info = zipfile.ZipInfo(name, date_time=ZIP_DATE_TIME)
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 0
    info.external_attr = 0
    return info


def main():
    path = os.path.join(FIXTURE_DIR, CBZ_NAME)
    with zipfile.ZipFile(path, "w") as z:
        for name, data in build_entries():
            z.writestr(zip_entry(name), data)
    print("Wrote: %s (%d bytes)" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    main()
