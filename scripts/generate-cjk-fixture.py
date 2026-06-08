#!/usr/bin/env python3
"""Generate tests/fixtures/cjk-{zh-hant,ja,ko}.pdf — CJK render fixtures for the
Phase 11.5 TOFU_CJK_LANG downgrade verification (spec §4.3).

No CJK font is embedded: the text is drawn with a non-embedded CID font so MuPDF
must resolve glyphs through its inbuilt fallback (Droid Sans Fallback Full under
TOFU_CJK_LANG). A tofu render here means the downgrade lost coverage.

Determinism mirrors generate-large-fixture.py: reportlab invariant mode +
pageCompression=0 so the bytes are zlib-independent (zlib-ng vs stock zlib).
Generated with reportlab==4.4.10. --check asserts byte-identity + zero FlateDecode.
"""
import argparse
import os
import sys
import tempfile

from reportlab import rl_config
rl_config.invariant = 1

from reportlab.lib.pagesizes import letter
from reportlab.lib.colors import white, black
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont

FIXTURES = {
    # filename: (CID font name, sample text covering common glyphs of the script)
    "cjk-zh-hant.pdf": ("STSong-Light", "繁體中文測試 常用字 台北 標準萬國碼"),
    "cjk-ja.pdf":      ("HeiseiMin-W3", "日本語のテスト ひらがな カタカナ 漢字"),
    "cjk-ko.pdf":      ("HYSMyeongJo-Medium", "한국어 테스트 한글 서울 표준"),
}

FIXTURE_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures")


def build(out_path, cid_font, text):
    # STSong-Light/HeiseiMin/HYSMyeongJo are reportlab built-in CID fonts whose
    # CMaps ship with reportlab; registering them does NOT embed a font program,
    # so the rendered PDF carries Unicode CIDs with no font bytes -> MuPDF must
    # use its inbuilt fallback. This is the whole point of the fixture.
    pdfmetrics.registerFont(UnicodeCIDFont(cid_font))
    width, height = letter
    c = canvas.Canvas(out_path, pagesize=letter, pageCompression=0)
    c.setTitle("LitePDF Phase 11.5 CJK fixture")
    c.setAuthor("litepdf")
    c.setCreator("generate-cjk-fixture.py")
    # White fill (PdfCanvas dark-surface fix), then the CJK sample at 24pt.
    c.saveState()
    c.setFillColor(white); c.setStrokeColor(white)
    c.rect(0, 0, width, height, fill=1, stroke=0)
    c.restoreState()
    c.setFillColor(black)
    c.setFont(cid_font, 24)
    c.drawString(72, height - 96, text)
    c.showPage()
    c.save()


def out_path(name):
    return os.path.join(FIXTURE_DIR, name)


def main():
    p = argparse.ArgumentParser(description="Generate or verify CJK fixtures")
    p.add_argument("--check", action="store_true",
                   help="Regenerate to temp + assert byte-identity + zero FlateDecode")
    args = p.parse_args()

    if args.check:
        ok = True
        for name, (font, text) in FIXTURES.items():
            committed_path = out_path(name)
            if not os.path.exists(committed_path):
                print("[FAIL] missing committed fixture: %s" % committed_path); ok = False; continue
            with tempfile.TemporaryDirectory() as td:
                tmp = os.path.join(td, name)
                build(tmp, font, text)
                with open(committed_path, "rb") as f: committed = f.read()
                with open(tmp, "rb") as f: regenerated = f.read()
            flate = regenerated.count(b"/FlateDecode")
            if flate != 0:
                print("[FAIL] %s has %d FlateDecode stream(s); use pageCompression=0" % (name, flate)); ok = False; continue
            if committed != regenerated:
                print("[FAIL] %s drift: committed=%d B regenerated=%d B" % (name, len(committed), len(regenerated))); ok = False; continue
            print("[OK] %s byte-identical (%d B)" % (name, len(committed)))
        sys.exit(0 if ok else 1)

    for name, (font, text) in FIXTURES.items():
        build(out_path(name), font, text)
        print("Wrote: %s (%d bytes)" % (out_path(name), os.path.getsize(out_path(name))))


if __name__ == "__main__":
    main()
