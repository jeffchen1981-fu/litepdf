#!/usr/bin/env python3
"""
Generate tests/fixtures/xps-sizing.xps and tests/fixtures/xps-escaped-name.xps,
the fixtures for XPS page sizes read from the root start tag of each FixedPage
part (#227) in tests/unit/test_xps_page_sizer.cpp. Spec:
docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md, section 5.1.

xps-sizing.xps has 16 pages; W x H are the root's Width/Height values:

  p01  plain, deflated; the local header carries a 28-byte 0xA220 extra field
       the central record lacks                                    816 x 1056
  p02  XML declaration, then a comment before the root; deflated; local
       CRC/sizes zeroed                                            612 x 792
  p03  Width="1000.9" Height="  700" (atoi: 1000 x 700)            1000 x 700
  p04  Width="&#56;16" (a character reference, 816)                816 x 500
  p05  stored (method 0); local CRC/sizes zeroed                   500 x 400
  p06  the FixedPage sits inside mc:AlternateContent/mc:Choice
       Requires="xps" (the choice MuPDF takes, so it loads)        640 x 480
  p07  UTF-16LE with a BOM and an encoding="utf-16" declaration,
       deflated                                                    700 x 900
  p08  xml:lang="<CJK>" (UTF-8) before Width                       720 x 960
  p09  no Height (MuPDF's load throws)                             800 x -
  p10  a 20 KB attribute value (no '>') before Width               610 x 810
  p11  referenced as /Documents/1/Pages/P11.FPAGE, stored under the
       zip name Documents/1/Pages/p11.fpage                        620 x 820
  p12  written as Documents/1/Pages/p12.fpage/[0].piece (root tag) and
       .../[1].last.piece                                          630 x 830
  p13  p13.fpage (300 x 300) written before P13.fpage (400 x 400),
       referenced as p13.fpage                                     300 x 300
  p14  about 64 KB of random-word Glyphs (seed 227) after a short
       root                                                        640 x 840
  p15  Name="a>b" before Width (a cut that ends inside the value
       throws)                                                     650 x 850
  p16  UTF-16BE with a BOM, no declaration; before Width a CJK
       xml:lang and a Name of twenty U+4E3E (bytes 4E 3E, a 0x3E
       byte that is no '>') and one U+1F600 (a surrogate pair)     740 x 980

xps-escaped-name.xps has three pages: page1.fpage 300 x 200, the raw entry
name #U0070age2.fpage (ASCII, no UTF-8 flag; MuPDF renames it page2.fpage)
400 x 300, and page3.fpage 500 x 400.

The zip structure is fixed (entry order, date_time, create_system,
external_attr), but the deflated bytes come from this Python's zlib, so the
files are byte-identical only with the same zlib. Generated with zlib
%(zlib)s. There is no --check mode: the tests compare MuPDF's own readings,
not hard-coded bytes. The generator asserts every property the list above
claims.

Usage:
  python -I scripts/generate-xps-sizing-fixture.py
"""

import os
import random
import struct
import zipfile
import zlib
from io import BytesIO

__doc__ = __doc__ % {"zlib": zlib.ZLIB_RUNTIME_VERSION}

FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))
SIZING_NAME = "xps-sizing.xps"
ESCAPED_NAME = "xps-escaped-name.xps"

ZIP_DATE_TIME = (2026, 11, 27, 0, 0, 0)
NS = "http://schemas.microsoft.com/xps/2005/06"
REL_TYPE = "http://schemas.microsoft.com/xps/2005/06/fixedrepresentation"
MC_NS = "http://schemas.openxmlformats.org/markup-compatibility/2006"
PAGES_DIR = "Documents/1/Pages/"
CJK_LANG = chr(0x4e2d) + chr(0x6587)   # two CJK characters
FALSE_GT = chr(0x4e3e)                  # U+4E3E: 4E 3E in UTF-16BE
SMILE = chr(0x1f600)                    # a surrogate pair in UTF-16
EXTRA_FIELD = bytes.fromhex("20A21800") + bytes(24)   # type 0xA220, size 24

LOCAL_SIG = b"PK\x03\x04"
CENTRAL_SIG = b"PK\x01\x02"
END_SIG = b"PK\x05\x06"


def page(w, h, attrs_before="", attrs_after="", body=""):
    wh = ""
    if w is not None:
        wh += ' Width="%s"' % w
    if h is not None:
        wh += ' Height="%s"' % h
    return ('<FixedPage xmlns="%s"%s%s%s>%s</FixedPage>'
            % (NS, attrs_before, wh, attrs_after, body))


def glyph_body(rng, target):
    words = ["alpha", "bravo", "delta", "echo", "lima", "mike", "oscar",
             "papa", "romeo", "tango", "victor", "xray", "zulu", "quartz"]
    out = []
    size = 0
    y = 20
    while size < target:
        text = " ".join(rng.choice(words) for _ in range(rng.randint(3, 8)))
        g = ('<Glyphs OriginX="%d" OriginY="%d" FontRenderingEmSize="12" '
             'UnicodeString="%s" Fill="#FF000000"/>'
             % (rng.randint(10, 700), y, text))
        out.append(g)
        size += len(g)
        y += 14
    return "".join(out)


def fdoc_xml(names):
    items = "".join('<PageContent Source="/%s"/>' % n for n in names)
    return ('<FixedDocument xmlns="%s">%s</FixedDocument>' % (NS, items))


def package_parts(page_sources):
    rels = ('<?xml version="1.0" encoding="utf-8"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="R1" Type="%s" Target="/FixedDocumentSequence.fdseq"/>'
            '</Relationships>' % REL_TYPE)
    fdseq = ('<FixedDocumentSequence xmlns="%s">'
             '<DocumentReference Source="/Documents/1/FixedDocument.fdoc"/>'
             '</FixedDocumentSequence>' % NS)
    return [
        ("_rels/.rels", rels.encode("utf-8"), zipfile.ZIP_DEFLATED),
        ("FixedDocumentSequence.fdseq", fdseq.encode("utf-8"), zipfile.ZIP_DEFLATED),
        ("Documents/1/FixedDocument.fdoc",
         fdoc_xml(page_sources).encode("utf-8"), zipfile.ZIP_DEFLATED),
    ]


def build_zip(parts):
    """parts: [(entry name, bytes, compress_type)] in zip order."""
    buf = BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data, ctype in parts:
            zi = zipfile.ZipInfo(name, date_time=ZIP_DATE_TIME)
            zi.create_system = 0
            zi.external_attr = 0
            zi.compress_type = ctype
            zf.writestr(zi, data)
    return bytearray(buf.getvalue())


def central_records(blob):
    """[(name, central offset, local header offset)] from the central directory."""
    end = blob.rfind(END_SIG)
    assert end >= 0 and len(blob) - end == 22, "end record not last"
    n, cd_size, cd_off = struct.unpack_from("<HII", blob, end + 10)
    recs = []
    pos = cd_off
    for _ in range(n):
        assert blob[pos:pos + 4] == CENTRAL_SIG
        nlen, xlen, clen = struct.unpack_from("<HHH", blob, pos + 28)
        lho = struct.unpack_from("<I", blob, pos + 42)[0]
        name = bytes(blob[pos + 46:pos + 46 + nlen]).decode("ascii")
        recs.append((name, pos, lho))
        pos += 46 + nlen + xlen + clen
    assert pos == cd_off + cd_size
    return recs, end, cd_off


def add_local_extra(blob, name):
    """Give `name`'s local header a 28-byte extra field its central record lacks."""
    recs, end, cd_off = central_records(blob)
    by_name = {r[0]: r for r in recs}
    lho = by_name[name][2]
    assert blob[lho:lho + 4] == LOCAL_SIG
    nlen, xlen = struct.unpack_from("<HH", blob, lho + 26)
    assert xlen == 0
    struct.pack_into("<H", blob, lho + 28, len(EXTRA_FIELD))
    blob[lho + 30 + nlen:lho + 30 + nlen] = EXTRA_FIELD
    shift = len(EXTRA_FIELD)
    # Everything after the insertion point moved by `shift`.
    for _, cpos, other in recs:
        if other > lho:
            struct.pack_into("<I", blob, cpos + 42 + shift, other + shift)
    end += shift
    struct.pack_into("<I", blob, end + 16, cd_off + shift)


def zero_local_sizes(blob, name):
    """Zero the local CRC-32 and both sizes of `name`."""
    recs, _, _ = central_records(blob)
    lho = {r[0]: r[2] for r in recs}[name]
    assert blob[lho:lho + 4] == LOCAL_SIG
    struct.pack_into("<III", blob, lho + 14, 0, 0, 0)


def local_fields(blob, central):
    lho = central.header_offset
    assert blob[lho:lho + 4] == LOCAL_SIG
    crc, csize, usize, nlen, xlen = struct.unpack_from("<IIIHH", blob, lho + 14)
    return crc, csize, usize, xlen


def attr_value(xml, attr):
    """The raw value of the first `attr="..."` in the root tag's text."""
    i = xml.index(attr + '="') + len(attr) + 2
    return xml[i:xml.index('"', i)]


def make_sizing():
    rng = random.Random(227)
    D = zipfile.ZIP_DEFLATED
    pages = []  # (reference, [(zip entry, bytes, ctype)]) in page order

    def one(ref_name, entry_name, text, ctype=D, encoding="utf-8"):
        pages.append((PAGES_DIR + ref_name,
                      [(PAGES_DIR + entry_name, text.encode(encoding), ctype)]))

    one("p01.fpage", "p01.fpage", page(816, 1056))
    one("p02.fpage", "p02.fpage",
        '<?xml version="1.0" encoding="utf-8"?><!-- c -->' + page(612, 792))
    one("p03.fpage", "p03.fpage", page("1000.9", "  700"))
    one("p04.fpage", "p04.fpage", page("&#56;16", 500))
    one("p05.fpage", "p05.fpage", page(500, 400), ctype=zipfile.ZIP_STORED)
    one("p06.fpage", "p06.fpage",
        '<mc:AlternateContent xmlns:mc="%s"><mc:Choice Requires="xps">%s'
        '</mc:Choice></mc:AlternateContent>' % (MC_NS, page(640, 480)))
    pages.append((PAGES_DIR + "p07.fpage", [(
        PAGES_DIR + "p07.fpage",
        b"\xff\xfe" + ('<?xml version="1.0" encoding="utf-16"?>' + page(700, 900))
        .encode("utf-16-le"), D)]))
    one("p08.fpage", "p08.fpage", page(720, 960, attrs_before=' xml:lang="%s"' % CJK_LANG))
    one("p09.fpage", "p09.fpage", page(800, None))
    one("p10.fpage", "p10.fpage",
        page(610, 810, attrs_before=' Name="%s"' % ("n" * (20 * 1024))))
    one("P11.FPAGE", "p11.fpage", page(620, 820))
    root12 = page(630, 830, body="@@")
    head12, tail12 = root12.split("@@")
    pages.append((PAGES_DIR + "p12.fpage", [
        (PAGES_DIR + "p12.fpage/[0].piece", head12.encode("utf-8"), D),
        (PAGES_DIR + "p12.fpage/[1].last.piece", tail12.encode("utf-8"), D)]))
    pages.append((PAGES_DIR + "p13.fpage", [
        (PAGES_DIR + "p13.fpage", page(300, 300).encode("utf-8"), D),
        (PAGES_DIR + "P13.fpage", page(400, 400).encode("utf-8"), D)]))
    one("p14.fpage", "p14.fpage", page(640, 840, body=glyph_body(rng, 64 * 1024)))
    one("p15.fpage", "p15.fpage", page(650, 850, attrs_before=' Name="a>b"'))
    pages.append((PAGES_DIR + "p16.fpage", [(
        PAGES_DIR + "p16.fpage",
        b"\xfe\xff" + page(740, 980, attrs_before=' xml:lang="%s" Name="%s%s"'
                            % (CJK_LANG, FALSE_GT * 20, SMILE)).encode("utf-16-be"), D)]))

    parts = package_parts([ref for ref, _ in pages])
    for _, entries in pages:
        parts.extend(entries)
    blob = build_zip(parts)
    add_local_extra(blob, PAGES_DIR + "p01.fpage")
    zero_local_sizes(blob, PAGES_DIR + "p02.fpage")
    zero_local_sizes(blob, PAGES_DIR + "p05.fpage")
    return bytes(blob), [e for _, es in pages for e in es]


def check_sizing(blob):
    zf = zipfile.ZipFile(BytesIO(blob))
    assert zf.testzip() is None, "an entry fails its CRC"
    info = {i.filename: i for i in zf.infolist()}
    names = [i.filename for i in zf.infolist()]
    assert names[:3] == ["_rels/.rels", "FixedDocumentSequence.fdseq",
                         "Documents/1/FixedDocument.fdoc"]
    assert b'Type="%s"' % REL_TYPE.encode() in zf.read("_rels/.rels")
    fdoc = zf.read("Documents/1/FixedDocument.fdoc").decode()
    assert fdoc.count("<PageContent ") == 16

    def part(n):
        return zf.read(PAGES_DIR + n)

    # Compression: only p05 is stored; every other page part is deflated.
    for n, i in info.items():
        if n.startswith(PAGES_DIR):
            want = zipfile.ZIP_STORED if n.endswith("p05.fpage") else zipfile.ZIP_DEFLATED
            assert i.compress_type == want, n
        assert not (i.flag_bits & 0x800), n

    # p01: local extra 28 bytes, central extra 0.
    p01 = info[PAGES_DIR + "p01.fpage"]
    assert len(p01.extra) == 0
    assert local_fields(blob, p01)[3] == 28
    lho = p01.header_offset
    nlen = struct.unpack_from("<H", blob, lho + 26)[0]
    assert blob[lho + 30 + nlen:lho + 30 + nlen + 28] == EXTRA_FIELD
    assert b"Width=\"816\"" in part("p01.fpage")
    # p02 and p05: local CRC and sizes zero, central ones not.
    for n in ("p02.fpage", "p05.fpage"):
        i = info[PAGES_DIR + n]
        assert local_fields(blob, i)[:3] == (0, 0, 0), n
        assert i.CRC != 0 and i.compress_size != 0 and i.file_size != 0, n
    p02 = part("p02.fpage").decode()
    assert p02.startswith('<?xml version="1.0" encoding="utf-8"?><!-- c --><FixedPage')
    assert 'Width="612"' in p02 and 'Height="792"' in p02
    p03 = part("p03.fpage").decode()
    assert 'Width="1000.9" Height="  700"' in p03
    p04 = part("p04.fpage").decode()
    assert 'Width="&#56;16" Height="500"' in p04
    p05 = part("p05.fpage").decode()
    assert 'Width="500" Height="400"' in p05
    p06 = part("p06.fpage").decode()
    assert p06.startswith('<mc:AlternateContent xmlns:mc="%s"><mc:Choice Requires="xps"><FixedPage' % MC_NS)
    assert 'Width="640" Height="480"' in p06
    p07 = part("p07.fpage")
    assert p07[:2] == b"\xff\xfe"
    p07t = p07[2:].decode("utf-16-le")
    assert 'Width="700" Height="900"' in p07t
    p08 = part("p08.fpage").decode("utf-8")
    assert p08.index('xml:lang="%s"' % CJK_LANG) < p08.index("Width=")
    assert 'Width="720" Height="960"' in p08
    p09 = part("p09.fpage").decode()
    assert 'Width="800"' in p09 and "Height" not in p09
    p10 = part("p10.fpage").decode()
    long_value = attr_value(p10, "Name")
    assert len(long_value) == 20 * 1024 and ">" not in long_value
    assert p10.index("Name=") < p10.index("Width=")
    assert 'Width="610" Height="810"' in p10
    # p11: referenced in upper case, stored in lower case.
    assert '"/Documents/1/Pages/P11.FPAGE"' in fdoc
    assert PAGES_DIR + "p11.fpage" in info and PAGES_DIR + "P11.FPAGE" not in info
    assert b'Width="620" Height="820"' in part("p11.fpage")
    # p12: two pieces, the root tag in piece 0 only.
    assert PAGES_DIR + "p12.fpage" not in info
    piece0 = zf.read(PAGES_DIR + "p12.fpage/[0].piece").decode()
    piece1 = zf.read(PAGES_DIR + "p12.fpage/[1].last.piece").decode()
    assert 'Width="630" Height="830"' in piece0 and piece1 == "</FixedPage>"
    assert names.index(PAGES_DIR + "p12.fpage/[0].piece") + 1 == \
        names.index(PAGES_DIR + "p12.fpage/[1].last.piece")
    # p13: lower-case entry (300) before the upper-case one (400).
    assert names.index(PAGES_DIR + "p13.fpage") < names.index(PAGES_DIR + "P13.fpage")
    assert b'Width="300" Height="300"' in part("p13.fpage")
    assert b'Width="400" Height="400"' in part("P13.fpage")
    assert '"/Documents/1/Pages/p13.fpage"' in fdoc
    # p14: a short root, then about 64 KB of Glyphs.
    p14 = part("p14.fpage").decode()
    assert p14.index("<Glyphs") < 200 and 'Width="640" Height="840"' in p14[:200]
    assert 64 * 1024 <= len(p14) < 66 * 1024, len(p14)
    # p15: a '>' inside an attribute value before Width.
    p15 = part("p15.fpage").decode()
    assert p15.index('Name="a>b"') < p15.index("Width=")
    assert 'Width="650" Height="850"' in p15
    # p16: UTF-16BE; the root holds 0x3E bytes that are no '>' unit, before Width.
    p16 = part("p16.fpage")
    assert p16[:2] == b"\xfe\xff"
    p16t = p16[2:].decode("utf-16-be")
    assert p16t.startswith("<FixedPage ") and "<?xml" not in p16t
    assert p16t.index('xml:lang="%s"' % CJK_LANG) < p16t.index("Width=")
    assert p16t.index(FALSE_GT * 20 + SMILE) < p16t.index("Width=")
    assert 'Width="740" Height="980"' in p16t
    root16 = p16[:p16.index(">".encode("utf-16-be")) + 2]
    assert root16.count(b">") == 21, root16.count(b">")   # 20 false bytes + the root's
    assert b"\xd8\x3d\xde\x00" in p16
    # p01 sanity and page order in the FixedDocument.
    refs = [s for s in fdoc.split('Source="')[1:]]
    order = [r.split('"')[0].split("/")[-1].lower() for r in refs]
    assert order == ["p%02d.fpage" % n for n in range(1, 17)], order


def make_escaped():
    D = zipfile.ZIP_DEFLATED
    raw = [("page1.fpage", 300, 200), ("#U0070age2.fpage", 400, 300),
           ("page3.fpage", 500, 400)]
    parts = package_parts([PAGES_DIR + "page%d.fpage" % n for n in (1, 2, 3)])
    for name, w, h in raw:
        parts.append((PAGES_DIR + name, page(w, h).encode("utf-8"), D))
    return bytes(build_zip(parts))


def check_escaped(blob):
    zf = zipfile.ZipFile(BytesIO(blob))
    assert zf.testzip() is None
    info = {i.filename: i for i in zf.infolist()}
    raw = PAGES_DIR + "#U0070age2.fpage"
    assert raw in info and not (info[raw].flag_bits & 0x800)
    assert PAGES_DIR + "page2.fpage" not in info
    fdoc = zf.read("Documents/1/FixedDocument.fdoc").decode()
    assert fdoc.count("<PageContent ") == 3
    for n, wh in ((1, "300\" Height=\"200"), (2, "400\" Height=\"300"),
                  (3, "500\" Height=\"400")):
        assert '"/Documents/1/Pages/page%d.fpage"' % n in fdoc
    assert 'Width="400" Height="300"' in zf.read(raw).decode()


def write(name, blob):
    path = os.path.join(FIXTURE_DIR, name)
    with open(path, "wb") as f:
        f.write(blob)
    return path


def main():
    sizing, _ = make_sizing()
    check_sizing(sizing)
    escaped = make_escaped()
    check_escaped(escaped)
    p1 = write(SIZING_NAME, sizing)
    p2 = write(ESCAPED_NAME, escaped)
    print("%s: %d bytes, 16 pages (zlib %s)" % (p1, len(sizing), zlib.ZLIB_RUNTIME_VERSION))
    print("%s: %d bytes, 3 pages (zlib %s)" % (p2, len(escaped), zlib.ZLIB_RUNTIME_VERSION))


if __name__ == "__main__":
    main()
