#!/usr/bin/env python3
"""
Generate tests/fixtures/cbz-sizing.cbz, the fixture for CBZ page sizes read
from image headers (#176) in tests/unit/test_cbz_page_sizer.cpp. Each entry
covers one branch of detail::CbzPageSizer::header_size (spec
docs/superpowers/specs/2026-10-25-cbz-header-sizing-176-design.md, section 5.1):

  p01_jfif300.jpg        600 x 400 JPEG, JFIF density 300 dpi
  p02_exif6.jpg          400 x 600 JPEG, EXIF orientation 6 (width and height swap)
  p03_sos_past_64k.jpg   300 x 300 JPEG, ~100 KB of COM segments before SOF
                         (sized at the 512 KB step)
  p04_large.jpg          900 x 700 noise JPEG, >= 512 KB, SOS in the first 8 KB
                         (the read-count oracle)
  p05_sos_past_512k.jpg  300 x 300 JPEG, ~600 KB of COM segments before SOF
                         (no header size: falls back)
  p06_phys.png           500 x 300 PNG, pHYs 144 dpi before IDAT
  p07.gif                120 x 80 GIF
  p08.bmp                90 x 60 BMP
  p09_t01.png, p09_t001.png, p09_t1.png
                         100 x 200, 100 x 300, 100 x 100 PNGs. MuPDF's natural
                         sort ties them, so their page order is whatever the
                         CRT qsort makes of the zip order. They are the last
                         three entries written, in this order, where the UCRT
                         qsort puts them t1, t001, t01: the reverse of a stable
                         sort, so a rebuilt list that sorts any other way sizes
                         them wrong
  p10.tif                64 x 64 TIFF (no header path: falls back)
  p11_broken.jpg         a JPEG signature and 1 KB of junk (MuPDF cannot decode it)
  p12_deflated.jpg       300 x 450 JPEG, the only ZIP_DEFLATED entry
  p13_png_text.png       200 x 150 PNG, a ~100 KB tEXt chunk before IDAT
                         (sized at the 512 KB step)
  p14_bmp_profile.bmp    80 x 50 BMP v5 with an embedded sRGB profile placed after
                         the pixels, past 8 KB

The zip structure is fixed (entry order, date_time, create_system,
external_attr), but the image bytes come from Pillow's codecs and the deflated
entry from its zlib, so the file is byte-identical only with the same Pillow
build. Generated with Pillow 12.2.0 (libjpeg-turbo 8.0, zlib-ng 1.3.1). There is
no --check mode: the tests compare MuPDF's own readings, not hard-coded bytes.
The generator asserts every property the list above claims.

Usage:
  python scripts/generate-cbz-sizing-fixture.py
"""

import io
import os
import struct
import zipfile

from PIL import Image, ImageCms, PngImagePlugin


FIXTURE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures"))
CBZ_NAME = "cbz-sizing.cbz"

ZIP_DATE_TIME = (2026, 1, 1, 0, 0, 0)
KB = 1024


def solid(size, mode="RGB", value=(200, 120, 40)):
    return Image.new(mode, size, value)


def noise(size, seed):
    # Deterministic noise: the multiplier walk is reproducible on every Python.
    w, h = size
    data = bytearray(w * h * 3)
    x = seed
    for i in range(len(data)):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        data[i] = (x >> 16) & 0xFF
    return Image.frombytes("RGB", size, bytes(data))


def encode(img, fmt, **params):
    buf = io.BytesIO()
    img.save(buf, format=fmt, **params)
    return buf.getvalue()


def jpeg_sos_offset(data):
    """Offset of the SOS marker, walking the marker segments from SOI."""
    assert data[:2] == b"\xff\xd8", "not a JPEG"
    pos = 2
    while pos + 4 <= len(data):
        assert data[pos] == 0xFF, "marker expected at %d" % pos
        marker = data[pos + 1]
        if marker == 0xDA:
            return pos
        (length,) = struct.unpack(">H", data[pos + 2:pos + 4])
        pos += 2 + length
    raise AssertionError("no SOS marker")


def with_com_segments(jpeg, total):
    """Inserts COM segments of `total` payload bytes right after SOI."""
    segments = bytearray()
    left = total
    while left > 0:
        n = min(left, 60000)
        segments += b"\xff\xfe" + struct.pack(">H", n + 2) + b"c" * n
        left -= n
    return jpeg[:2] + bytes(segments) + jpeg[2:]


def png_chunks(data):
    """(type, offset) of every chunk."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    out = []
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        out.append((data[pos + 4:pos + 8], pos))
        pos += 12 + length
    return out


def first_chunk(data, kind):
    return next(off for t, off in png_chunks(data) if t == kind)


def exif_orientation(value):
    exif = Image.Exif()
    exif[0x0112] = value
    return exif.tobytes()


def bmp_v5_with_profile(width, height, profile):
    """A 24-bit bottom-up BMP v5 whose embedded ('MBED') profile follows the pixels."""
    row = width * 3
    assert row % 4 == 0, "rows must need no padding"
    pixels = bytes([40, 120, 200]) * (width * height)        # BGR
    header_size = 124
    bitmap_offset = 14 + header_size
    profile_offset = header_size + len(pixels)                # from the info header
    file_size = bitmap_offset + len(pixels) + len(profile)
    file_header = b"BM" + struct.pack("<IHHI", file_size, 0, 0, bitmap_offset)
    info = struct.pack("<IiiHHIIiiII", header_size, width, height, 1, 24, 0,
                       len(pixels), 2835, 2835, 0, 0)
    info += struct.pack("<IIII", 0, 0, 0, 0)                  # colour masks (unused)
    info += struct.pack("<I", 0x4D424544)                     # bV5CSType 'MBED'
    info += b"\0" * 36 + b"\0" * 12                           # endpoints, gamma
    info += struct.pack("<IIII", 4, profile_offset, len(profile), 0)
    assert len(info) == header_size
    return file_header + info + pixels + profile


def build_entries():
    entries = []

    p01 = encode(solid((600, 400)), "JPEG", quality=90, dpi=(300, 300))
    entries.append(("p01_jfif300.jpg", p01))

    p02 = encode(solid((400, 600)), "JPEG", quality=90, exif=exif_orientation(6))
    entries.append(("p02_exif6.jpg", p02))

    p03 = with_com_segments(encode(solid((300, 300)), "JPEG", quality=90), 100 * KB)
    assert 64 * KB < jpeg_sos_offset(p03) < 512 * KB
    entries.append(("p03_sos_past_64k.jpg", p03))

    p04 = encode(noise((900, 700), 176), "JPEG", quality=95)
    assert len(p04) >= 512 * KB, "p04 is %d bytes" % len(p04)
    assert jpeg_sos_offset(p04) < 8 * KB
    entries.append(("p04_large.jpg", p04))

    p05 = with_com_segments(encode(solid((300, 300)), "JPEG", quality=90), 600 * KB)
    assert jpeg_sos_offset(p05) > 512 * KB
    entries.append(("p05_sos_past_512k.jpg", p05))

    p06 = encode(solid((500, 300)), "PNG", dpi=(144, 144))
    assert first_chunk(p06, b"pHYs") < first_chunk(p06, b"IDAT")
    entries.append(("p06_phys.png", p06))

    entries.append(("p07.gif", encode(solid((120, 80), "P", 3), "GIF")))
    entries.append(("p08.bmp", encode(solid((90, 60)), "BMP")))

    entries.append(("p10.tif", encode(solid((64, 64)), "TIFF")))

    junk = bytes(range(256)) * 4
    entries.append(("p11_broken.jpg", b"\xff\xd8\xff" + junk))

    entries.append(("p12_deflated.jpg", encode(solid((300, 450)), "JPEG", quality=90)))

    text = PngImagePlugin.PngInfo()
    text.add_text("Comment", "t" * (100 * KB))
    p13 = encode(solid((200, 150)), "PNG", pnginfo=text)
    assert 64 * KB < first_chunk(p13, b"IDAT") < 512 * KB
    entries.append(("p13_png_text.png", p13))

    srgb = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
    p14 = bmp_v5_with_profile(80, 50, srgb)
    assert 14 + 124 + 80 * 50 * 3 > 8 * KB
    entries.append(("p14_bmp_profile.bmp", p14))

    # The tie trio, written last, t01, t001, t1 (see the module docstring).
    for name, h in (("p09_t01.png", 200), ("p09_t001.png", 300), ("p09_t1.png", 100)):
        entries.append((name, encode(solid((100, h)), "PNG")))

    return entries


def zip_entry(name, compress_type):
    """A ZipInfo whose header bytes do not depend on the platform or the clock."""
    info = zipfile.ZipInfo(name, date_time=ZIP_DATE_TIME)
    info.compress_type = compress_type
    info.create_system = 0
    info.external_attr = 0
    return info


def main():
    path = os.path.join(FIXTURE_DIR, CBZ_NAME)
    with zipfile.ZipFile(path, "w") as z:
        for name, data in build_entries():
            kind = zipfile.ZIP_DEFLATED if name == "p12_deflated.jpg" else zipfile.ZIP_STORED
            z.writestr(zip_entry(name, kind), data)
    print("Wrote: %s (%d bytes)" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    main()
