#!/usr/bin/env python3
"""
Generate tests/fixtures/encrypted-text.pdf, the fixture that renders an
encrypted page through RenderEngine (tests/unit/test_render_engine_password.cpp).

One page of large black text, 128-bit RC4 (/V 2 /R 3), user password "test",
owner password "owner". The user password is not blank, so MuPDF's
blank-password attempt at open fails and leaves a wrong key behind (#126). The
text matters: tests/fixtures/encrypted.pdf has an empty page, so a render with
the wrong key looks the same as a correct one.

Why not AES: reportlab's 256-bit mode writes 48-byte /OE and /UE strings (it
pads them as AES-CBC data), and MuPDF rejects anything but the 32 bytes the
spec requires ("encryption dictionary missing owner encryption key"). The
defect is not cipher-specific: every revision leaves a wrong key behind after
the blank-password attempt, and #126 reproduced it on AES-128 and AES-256 files
too.

Byte-reproducible: rl_config.invariant pins the timestamp and the document ID
the RC4 key is derived from, and RC4 has no IV. pageCompression=0 leaves no
zlib stream whose bytes would depend on the interpreter's zlib build (see
scripts/generate-large-fixture.py). Generated with reportlab==4.4.10.

Usage:
  python scripts/generate-encrypted-text-fixture.py           # write the fixture
  python scripts/generate-encrypted-text-fixture.py --check   # exit 1 if it would change
"""

import argparse
import io
import os
import sys

from reportlab import rl_config

rl_config.invariant = 1

from reportlab.lib import pdfencrypt  # noqa: E402
from reportlab.lib.colors import black, white  # noqa: E402
from reportlab.pdfgen import canvas  # noqa: E402

PAGE_W, PAGE_H = 612, 792  # US Letter, points
USER_PASSWORD = "test"
OWNER_PASSWORD = "owner"

OUT_PATH = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "fixtures", "encrypted-text.pdf"))


def build():
    buf = io.BytesIO()
    enc = pdfencrypt.StandardEncryption(
        USER_PASSWORD, ownerPassword=OWNER_PASSWORD, strength=128)
    c = canvas.Canvas(buf, pagesize=(PAGE_W, PAGE_H), pageCompression=0,
                      encrypt=enc)
    # Explicit white page fill; see generate-search-fixture.py for why.
    c.saveState()
    c.setFillColor(white)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    c.restoreState()
    c.setFillColor(black)
    c.setFont("Helvetica-Bold", 48)
    for i in range(8):
        c.drawString(72, 680 - 80 * i, "ENCRYPTED %d" % (i + 1))
    c.showPage()
    c.save()
    return buf.getvalue()


def main():
    parser = argparse.ArgumentParser(
        description="Generate tests/fixtures/encrypted-text.pdf")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the committed fixture would change")
    args = parser.parse_args()

    data = build()
    if b"/FlateDecode" in data:
        print("encrypted-text.pdf unexpectedly contains a compressed stream",
              file=sys.stderr)
        return 1
    if b"/Filter /Standard" not in data or b"/R 3" not in data:
        print("encrypted-text.pdf is not 128-bit RC4 encrypted", file=sys.stderr)
        return 1

    if args.check:
        with open(OUT_PATH, "rb") as f:
            if f.read() != data:
                print("tests/fixtures/encrypted-text.pdf is stale; rerun without --check",
                      file=sys.stderr)
                return 1
        return 0

    with open(OUT_PATH, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (OUT_PATH, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
