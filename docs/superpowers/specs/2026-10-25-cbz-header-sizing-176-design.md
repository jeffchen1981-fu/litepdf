# CBZ page sizes from image headers (#176) — design

Status: approved in chat 2026-10-25 (after a Fable design review); spec gate passed at a34de67.
§3.8 (read-ahead) added 2026-10-26 after the first implementation missed the §6 cold gate
(§6.1), chosen by the user from a Fable review; its spec gate is pending.
Issue: #176. Upstream spec: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md`
(§4.4 page-sizing gate, §7.3 formats, §9.3/§9.4 smoke and fixtures).

## 1. Problem

PR #177 (3e56568) took CBZ out of the continuous-scroll strip: `DocumentView::page_sizes()`
(`src/core/DocumentView.cpp:292`) took 641 ms with nothing cached and 329 ms cached on 300
stored 1200×1700 JPEGs (415 MB), against the §4.4 gate of ≤ 100 ms on the UI thread.

`Document::page_size` (`src/core/Document.cpp:408`) sizes a non-PDF page with `fz_load_page` +
`fz_bound_page`. For a CBZ, `cbz_load_page` (`third_party/mupdf/source/cbz/mucbz.c:234`) reads the
whole archive entry (`fz_read_archive_entry`) and then calls `fz_new_image_from_buffer`, which only
needs the image header: it dispatches on the format to `fz_load_*_info` and takes width, height,
resolution and (JPEG only) EXIF orientation. `cbz_bound_page` (mucbz.c:161) turns those into points.

## 2. Goal and non-goals

Goal: size a CBZ page from a prefix of its archive entry, with a result bitwise equal to the
`fz_load_page` + `fz_bound_page` result for every entry MuPDF can read in full; meet the §4.4 gate
on a real-size CBZ read cold; put CBZ back in the strip.

Non-goals: moving page sizing off the UI thread; patching MuPDF; TIFF/JPX/JXR/PNM/PSD/JBIG2 header
sizing (they keep the full read, see §3.4); HDD performance (§6).

## 3. Design

### 3.1 Unit: `detail::CbzPageSizer`

New files `src/core/detail/CbzPageSizer.hpp` and `src/core/CbzPageSizer.cpp`, added to
`litepdf_core` in `CMakeLists.txt`. MuPDF types appear only in this detail header, which only
`Document.cpp` and unit tests include (`Document.hpp` stays MuPDF-free).

```cpp
namespace litepdf::core::detail {
class CbzPageSizer {
public:
    // A sizer for `doc`, or nullptr when the header path does not apply: the archive at
    // `utf8_path` does not open, MuPDF did not open `doc` with its CBZ handler, or the
    // rebuilt page list does not have `page_count` entries. Never throws.
    static std::unique_ptr<CbzPageSizer> create(fz_context* ctx, fz_document* doc,
                                                const char* utf8_path, std::size_t page_count);
    ~CbzPageSizer();                       // fz_drop_archive on the same ctx
    // Page `index`'s size from its image header, or nullopt when the caller must fall back
    // to fz_load_page. Never throws.
    std::optional<PageSize> header_size(std::size_t index);
};
}
```

It holds the borrowed `fz_context*` (the Document's own), its own `fz_archive*`, and the page
names as `std::string` copies, in MuPDF's page order.

### 3.2 The page list

`cbz_document` is private to mucbz.c (no declaration under `include/`), so `create` rebuilds the
list the way `cbz_create_page_list` (mucbz.c:118) does, on a second archive opened with
`fz_open_archive(ctx, utf8_path)`:

- the same path `Document::open` passed to `fz_open_document`; both reach
  `fz_open_archive_with_stream` over `fz_open_file` of that path, so the entry order matches;
- entries whose `strrchr(name, '.')` matches `cbz_ext_list` (mucbz.c:30) case-insensitively
  (`fz_strcasecmp`), NULL names skipped;
- sorted by a copy of `cbz_strnatcmp` (mucbz.c:82) with the CRT `qsort`, on the same input order
  (both sides link the same static UCRT, so ties land identically).

Guards (both must hold, else `create` returns nullptr):

1. `fz_lookup_metadata(ctx, doc, FZ_META_FORMAT)` equals `fz_archive_format(ctx, arch)`.
   `cbz_lookup_metadata` returns the archive format (mucbz.c:273); EPUB returns "EPUB"
   (epub-doc.c:1097) and XPS returns "XPS" (xps-zip.c:250), so a `.cbz` that MuPDF opened with
   another handler fails it.
2. The rebuilt list's size equals `page_count`.

No C++ code that can throw runs between a `fz_try` and its `fz_catch`: the MuPDF calls fill
plain locals, and the `std::vector`/`std::string` work happens outside the MuPDF try blocks, where
a `std::bad_alloc` makes `create` return nullptr (and `header_size` return nullopt).

### 3.3 Per-page header read

`header_size(i)` opens `fz_open_archive_entry(ctx, arch, names[i])` and reads into a buffer in
steps of 8 KB, 64 KB, 512 KB (stop early at end of entry). After each step:

- fewer than 8 bytes and end of entry → nullopt;
- `fz_recognize_image_format` on the first 8 bytes:
  - **JPEG, GIF, BMP**: `fz_new_image_from_buffer` on the prefix. Success → size (§3.5).
    Failure → next step, or nullopt at end of entry or after 512 KB.
    - JPEG exactness: MuPDF's `fill_input_buffer` (load-jpeg.c:116) supplies only `FF D9` past the
      end and `skip_input_data` (load-jpeg.c:124) clamps without carrying a deficit, so any read past
      the prefix meets EOI; `jpeg_read_header(&cinfo, 1)` (load-jpeg.c:532) then fails with
      JERR_NO_IMAGE. Success therefore means the real SOS marker was read in sequence, and every
      value MuPDF uses (SOF dimensions, JFIF density, saved APP1/APP13/APP2) came from bytes before
      it. `fz_load_jpeg_info` reads nothing after `jpeg_read_header`.
    - GIF: `gif_read_image(only_metadata)` returns after the header and logical screen descriptor
      (load-gif.c:495). BMP: `bmp_read_image` returns after the headers and colour profile
      (load-bmp.c:1304). An embedded profile past the prefix is not an error there:
      `bmp_read_color_profile` warns and returns NULL (load-bmp.c:849-858) and the image takes
      device RGB, so the prefix succeeds with the right size (the colour space does not enter it).
  - **PNG**: MuPDF's metadata loop walks every chunk to IEND (load-png.c:497), so the prefix is not
    handed over as is. Walk the chunk headers from offset 8; at the first `IDAT`, take the bytes
    before it, append an IEND chunk (`00 00 00 00 'IEND' AE 42 60 82`) and call
    `fz_new_image_from_buffer`: success → size, failure → nullopt. A chunk header past the prefix →
    next step; IEND before any IDAT, or no IDAT within 512 KB → nullopt.
  - **anything else** → nullopt.

### 3.4 Fallback and accepted deviations

nullopt means `Document::page_size` takes its existing `fz_load_page` path, so every fallback is
exact by construction. Formats that always fall back: TIFF (its IFD may sit at the end of the
file), JPX, JXR, PNM, PSD, JBIG2. A CBZ made of them stays as slow as today.

The header path differs from the full read only for files MuPDF cannot read in full, plus one
non-conforming PNG layout. Both are accepted and recorded in the upstream spec §4.4:

1. **PNG chunks after the first IDAT.** The PNG spec requires pHYs (and iCCP) before the first
   IDAT; MuPDF's metadata loop also reads them later (load-png.c:508-513), the header path does not.
   A later pHYs changes the resolution, so the size differs by its ratio; a later pHYs of the wrong
   length makes the full read throw (load-png.c:425), which gives the empty rect of item 2; a later
   iCCP changes only the colour space.
2. **A damaged entry or image past the header**: a deflated entry whose data is corrupt or cut
   short (`read_zip_entry` throws at unzip.c:586, the flate stream only warns, filter-flate.c:75), a
   PNG whose later chunks are cut short, or a BMP whose embedded profile MuPDF rejects (the full
   read throws in `fz_new_icc_colorspace`; the prefix never reaches the profile). The full read
   leaves the page's image NULL, `fz_bound_page` returns `fz_empty_rect`, and `fill_page_sizes`
   gives the page page 0's size in the strip; the header path gives the image's real size. MuPDF
   renders that page blank either way. (Only zip archives reach this path: `Document::open`
   requires `PK\x03\x04` for `.cbz`, Document.cpp:136-141.)

Where the size differs, every `page_size` caller sees the header size while rendering uses
`fz_bound_page` of the loaded page (`with_rendered_page`, Document.cpp:298): the strip slot
differs from the bitmap, which the strip draws at its natural size
(`src/ui/PdfCanvasStrip.cpp:855`); paged fit-to-width/page (`DocumentView.cpp:330`) is off by the
same ratio; and printing (`PrintJob.cpp:282`) scales the page by it, so the image under- or
over-fills the paper. Accepted because both cases need a non-conforming or damaged file, and for
item 2 the page has no image to show.

### 3.5 Size arithmetic

A copy of `cbz_bound_page` (mucbz.c:161-186) with the same types, so the result is bitwise equal:
`fz_image_resolution(image, &xres, &yres)`; `orientation = fz_image_orientation(ctx, image)`;
with `constexpr float kCbzDpi = 72.0f`, if `orientation == 0 || (orientation & 1) == 1` then
`w = image->w * kCbzDpi / xres`, `h = image->h * kCbzDpi / yres`, else the two swap
(`h = image->w * kCbzDpi / xres`, `w = image->h * kCbzDpi / yres`). `page_size` returns
`{w - 0, h - 0}` as today's `x1 - x0`. MuPDF and litepdf both build with the default `/fp:precise`.

### 3.6 Integration in `Document`

- `Document::Impl` gains `std::unique_ptr<detail::CbzPageSizer> cbz`, `bool cbz_probed` and
  `std::string cbz_path`, declared after `root`. `~Impl` resets `cbz` before `fz_drop_document`,
  and member order keeps the root context alive until after it (#61). `close()` and the start of
  `open()` reset all three.
- `open()`, after `fz_open_document` succeeds on a path whose extension is `.cbz`
  (case-insensitive), stores `cbz_path` = the UTF-8 bytes of `std::filesystem::absolute(path)`, so a
  later working-directory change cannot point the lazy probe at another file. The probe itself stays
  lazy: a paged CBZ that never sizes a page pays nothing.
- In `page_size`, under `doc_mutex` and **before** the existing `fz_try` (no `return` from inside a
  `fz_try`): if the document is not a PDF, `!cbz_probed` and `cbz_path` is set, call `create` once
  and set `cbz_probed`. Then, if `cbz` is set and `header_size(index)` returns a size, return it;
  otherwise run the existing `fz_try` unchanged. An index past the list makes `header_size` return
  nullopt, and the existing branch throws as today.
- Workers and search open their own documents and do not change. Printing and paged fit call
  `page_size` and get the same values as today except for the §3.4 inputs.
- Thread safety: the sizer uses only the Document's own context, always under `doc_mutex`.
  `open()`, `close()` and `~Impl` reset it without the mutex, as they already drop `doc` without
  it: a Document is opened and closed by one thread that is not inside another of its methods
  (Document.cpp:60-63, "In practice only the UI thread takes it"). This change does not alter that
  contract.

### 3.7 MuPDF error state (#146)

Every new `fz_catch` clears the error code. The archive open in `create` and the decode attempts
in `header_size` are expected-failure probes: `fz_convert_error(ctx, nullptr)`, result discarded,
so a GIF-only or TIFF-only CBZ prints no line per page. The fallback keeps its
`fz_report_error` catch. Buffers, streams and images are dropped in `fz_always`, and every
MuPDF-owned local that is assigned inside a `fz_try` and read in its `fz_always` or `fz_catch` is
declared before the `fz_try` and marked `fz_var` (`context.h:88-94`; as `page` at
Document.cpp:414-416).

### 3.8 Read-ahead of every page's first bytes

**Why.** §3.3 alone misses the cold gate (§6.1). Per page it costs three 4 KB `fread`s at queue
depth 1: `read_zip_entry_header` reads 4 KB at the local header and its relative `fz_seek` over
the name and extra field discards that buffer (unzip.c:439-459, stream-open.c:156-157); the null
filter seeks again and reads the first 4 KB of data, then the next 4 KB for the 8 KB step
(filter-basic.c:46-47). That spans 4 pages of the file, faulted one at a time: about 160 µs for the
first and 65 µs for each further page on this NVMe, so 300 pages cost about 100 ms before any
decoding, and a JPEG whose SOS lies past 8 KB costs a 64 KB step as well. A smaller first step only
moves the floor (one 512-byte read per entry is still 48 ms cold). What changes the regime is
reading at a queue depth above 1.

**What.** Once, in `CbzPageSizer::create`, after the guards pass and before `create` returns, hand
the memory manager every page entry's first bytes as one `PrefetchVirtualMemory` call over a
read-only mapping of the file. The pages land in the system file cache; the §3.3 reads that follow
hit it. The read-ahead changes which bytes are cached, never which bytes MuPDF reads, so the sizes
and every §3.4 statement stay as they are. Any failure skips the read-ahead and §3.3 runs as it does
without it.

**New unit.** `src/core/detail/ZipReadAhead.hpp` and `src/core/ZipReadAhead.cpp` (in
`litepdf_core`), Win32 and the C++ standard library only, no MuPDF:

```cpp
namespace litepdf::core::detail {
struct ZipDirectory { std::uint64_t offset; std::uint64_t size; std::size_t count; };
struct ZipEntryRef { std::string name; std::uint64_t local_header_offset; };

// Pure, over bytes the caller read (unit-testable on crafted buffers). nullopt for anything
// the parser does not handle (§3.8 "Parser").
// `tail` is the file's last bytes; `tail_offset` is where they start, `tail_offset + n` is
// the file size.
std::optional<ZipDirectory> find_zip_directory(const unsigned char* tail, std::size_t n,
                                               std::uint64_t tail_offset);
// `dir` is the directory's `dir.size` bytes, read from `dir.offset`.
std::optional<std::vector<ZipEntryRef>> parse_zip_directory(const unsigned char* bytes,
                                                            std::size_t n, const ZipDirectory& dir);

// File level. Never throw.
// The directory of the zip at `path`, read with ReadFile on its own handle.
std::optional<std::vector<ZipEntryRef>> read_zip_directory(const std::wstring& path) noexcept;
// One PrefetchVirtualMemory call over [offset, offset + span) for each offset, clipped to the
// file, through a read-only mapping that is unmapped and closed before returning. True when the
// call succeeded.
bool prefetch_file_ranges(const std::wstring& path, const std::vector<std::uint64_t>& offsets,
                          std::size_t span) noexcept;
}
```

`create` calls them on the Document's path (UTF-8 → UTF-16 with `MultiByteToWideChar(CP_UTF8)`,
`MB_ERR_INVALID_CHARS`), keeps the entries whose names pass the same `is_page_name` test as the
page list (§3.2), and passes their offsets. The order of the offsets does not matter.

**Parser.** Reads the directory with `ReadFile` on its own handle (never through the mapping: a
read error on a mapped page is an `EXCEPTION_IN_PAGE_ERROR`, not an error code). Steps:

1. Read the last min(size, 22 + 65,535) bytes; find the end-of-central-directory signature
   `50 4B 05 06` scanning backwards, at a position whose comment length reaches exactly the end of
   the file. None → nullopt.
2. Take the entry count (total), the directory size and the directory offset. Any of them at its
   Zip64 escape value (`0xFFFF`, `0xFFFFFFFF`), a directory that does not fit before the EOCD record,
   or more than 1,000,000 entries → nullopt (MuPDF still reads such files; only the read-ahead is
   skipped).
3. Read the directory and walk `count` records: signature `50 4B 01 02`, name length (+28), extra
   length (+30), comment length (+32), local header offset (+42), name (+46). A record that runs past
   the directory, a wrong signature, or an offset at or past the directory start → nullopt.

**Read-ahead.** `CreateFileW(GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
OPEN_EXISTING)`, `CreateFileMappingW(PAGE_READONLY)`, `MapViewOfFile(FILE_MAP_READ)` of the whole
file, one `PrefetchVirtualMemory(GetCurrentProcess(), n, ranges, 0)`, then `UnmapViewOfFile` and both
`CloseHandle`s before returning, so `close()` still releases every handle (test 9). The mapped view
is never dereferenced. Measured on the gate files (§6): the call took 17-41 ms with 64 KB ranges and
returned with the pages resident; sizing then took 4-19 ms.

**Span.** Each range starts at the local header and covers `span` = clamp(32 MiB / pages, 12 KiB,
68 KiB) bytes: 68 KiB holds the 30-byte local header, the name and extra field, and the first 64 KB
of data, so a JPEG whose SOS lies past 8 KB is also served from the cache. The total stays at
32 MiB until the 12 KiB floor (2,730 pages and up). The pages go to the standby list, not to the
process working set.

**Cost when it does not help.** A warm archive pays the parse (one read of the EOCD tail and one of
the directory, both cached after MuPDF's own open) and a prefetch that finds the pages resident:
about 1 ms on the gate files. A CBZ that falls back for every page (§3.4 formats) pays the
read-ahead for nothing; accepted.

**Errors.** No MuPDF call is involved, so #146 does not apply. `read_zip_directory` and
`prefetch_file_ranges` are `noexcept`: a `std::bad_alloc` inside them gives nullopt / false, and
every handle is closed on every path. `create` calls them outside its `fz_try`. Nothing is logged.
On a system without `PrefetchVirtualMemory` (before Windows 8; litepdf requires Windows 10 1903)
the call fails and is skipped like any other failure; the import is resolved at load time, which
Windows 10 always satisfies.

## 4. Re-enabling CBZ in the strip

- `strip_format_ok` (`src/ui/detail/EffectiveLayout.hpp:57`): add `.cbz`; the comment says the
  header path makes it pass and points at this spec. Admission stays by extension: a file of
  another format renamed `.cbz` (MuPDF opens it with that format's handler) enters the strip and is
  sized by the full read, as a file renamed `.epub` or `.pdf` already is today. Not addressed here.
- `tests/unit/test_strip_helpers.cpp:82-83`: `CHECK_FALSE` → `CHECK`.
- `scripts/smoke-test.ps1`: the `sample.cbz` launch (line 1786) drops `-Paged` and asserts the
  strip like the EPUB launch. The `-Paged` switch of `Start-LitePDFSmokeLaunch` (line 1726) has no
  other caller and is removed. Row 20 keeps `page-size.pdf`.
- `docs/SMOKE-CHECKLIST.md:30`: the `.cbz` line becomes "opens in continuous scroll" (the XPS line
  stays).
- Docs: README:41 (XPS and FB2 stay one page at a time); a CHANGELOG Unreleased entry (the
  released 1.6.0 section stays as the record); upstream spec §4.4 (re-measured reading, the §3.4
  deviations, the fallback formats, the drive and the §3.8 read-ahead), §7.3 line 541, §9.3 line 734;
  `scripts/generate-strip-fixtures.py` docstring ("CBZ stays paged; #176").
- Comments: `tests/unit/test_strip_fixtures.cpp:5-6` ("CBZ stays paged, #176");
  `tests/unit/test_document_page_size.cpp:88-90` ("the ePub and CBZ, which still load the page").
  `scripts/smoke-test.ps1:1488` "(until CBZ left the strip)" stays as history.
- Build: `src/core/CbzPageSizer.cpp` and `src/core/ZipReadAhead.cpp` in `litepdf_core`
  (`CMakeLists.txt`), `tests/unit/test_cbz_page_sizer.cpp` and `tests/unit/test_zip_read_ahead.cpp`
  in `tests/CMakeLists.txt`.
- `src/core/CbzPageSizer.cpp`'s header comment says nothing is printed per page; MuPDF's own
  warnings (a BMP profile past the prefix, libjpeg's corrupt-data notes) are, so it says that
  only the discarded errors are silent.

## 5. Tests

### 5.1 Fixture `tests/fixtures/cbz-sizing.cbz`

From a new `scripts/generate-cbz-sizing-fixture.py` (Pillow; fixed zip `date_time`,
`create_system`, `external_attr`). JPEG/PNG bytes depend on Pillow's codecs and the deflated entry
on zlib, so the file is not byte-reproducible across Pillow/zlib versions; the docstring says so
and names the version used. Entries, in natural-sort order (the generator asserts each property it
claims, e.g. SOS offsets and sizes):

| entry | content | header path |
|---|---|---|
| `p01_jfif300.jpg` | 600×400, JFIF 300 dpi | size |
| `p02_exif6.jpg` | 400×600, EXIF orientation 6 | size (w/h swapped) |
| `p03_sos_past_64k.jpg` | 300×300, ~100 KB of COM segments before SOF | size (512 KB step) |
| `p04_large.jpg` | 900×700 noise, quality 95, ≥ 512 KB, SOS < 8 KB | size (read-count oracle) |
| `p05_sos_past_512k.jpg` | 300×300, ~600 KB of COM segments before SOF | nullopt |
| `p06_phys.png` | 500×300, pHYs 144 dpi | size |
| `p07.gif` | 120×80 | size |
| `p08.bmp` | 90×60 | size |
| `p09_t1.png`, `p09_t01.png`, `p09_t001.png` | 100×100, 100×200, 100×300; tie under natcmp; the last three entries written, in the order t01, t001, t1 | size |
| `p10.tif` | 64×64 | nullopt |
| `p11_broken.jpg` | `FF D8 FF` + 1 KB of junk | nullopt |
| `p12_deflated.jpg` | 300×450, `ZIP_DEFLATED` | size |
| `p13_png_text.png` | 200×150, a ~100 KB tEXt chunk before IDAT | size (PNG 512 KB step) |
| `p14_bmp_profile.bmp` | 80×50 BMP v5, embedded sRGB profile placed past 8 KB | size |

### 5.2 Unit tests (`tests/unit/test_cbz_page_sizer.cpp`, `[core][cbz]`)

With a test-owned context and document (the `OwnDocument` pattern of
`tests/unit/test_document_page_size.cpp:26`):

1. `create` is non-null for `cbz-sizing.cbz` and `sample.cbz`; null for `simple.pdf` (no archive
   handler); and null for `sample.epub` copied to a temp `renamed.cbz` called with
   `page_count = 0`. `sample.epub` has no image entries, so its rebuilt list is empty and 0 passes
   guard 2: only the metadata guard can reject it. And null for `cbz-sizing.cbz` called with its
   page count + 1: only guard 2 can reject that.
2. For every page of `cbz-sizing.cbz`, `sample.cbz` and `strip-images.cbz`: `header_size` returns a
   value exactly for the pages the table marks "size", and that value is bitwise equal
   (`std::memcmp` of the floats) to `fz_load_page` + `fz_bound_page` on the test's document.
3. `Document::page_size` equals the full-read result for every page of the same three files and of
   the renamed EPUB (covers both branches and the fallback).
4. Read-count oracle (`GetProcessIoCounters`, as `test_document_page_size.cpp:67`): after sizing
   page 0, `Document::page_size` of `p04_large.jpg` costs ≤ 20 read operations; the control
   `fz_load_page` of the same page on the test's document costs ≥ 60.
5. A UTF-8 (CJK) path: `cbz-sizing.cbz` copied into a temp directory with a CJK name; `create` is
   non-null and page 0 matches.
6. #146, on the test's own context with a warning callback installed, no fallback in between:
   `header_size` on `p11_broken.jpg` and on `p05_sos_past_512k.jpg` (both enter the decode
   `fz_catch`), and `create` on `simple.pdf` (the archive-open catch); after each, an immediate
   `fz_throw` caught by the test reports no stale "UNHANDLED EXCEPTION!" (the warning-callback
   oracle of `RootWarnings` in `tests/unit/test_document_error_state.cpp:44`).
7. Reset: one `Document` opens `cbz-sizing.cbz`, sizes page 0, `close()`s, opens `sample.cbz`; every
   `page_size` then equals `sample.cbz`'s full read (a stale sizer would size A's entries).
8. Out of range: `header_size(page_count)` returns nullopt; `Document::page_size(page_count)`
   still throws `std::runtime_error`.
9. `close()` releases the archive: after `open`, `page_size(0)` and `close()` on a temp copy, the
   copy can be removed (MuPDF opens files without `FILE_SHARE_DELETE`).

Mutation checks, run once while implementing and recorded in the PR: replacing `qsort` with
`std::stable_sort` turns test 2 red. (Written among the other entries, the trio came out of the
UCRT `qsort` in its stable order and the first run of this mutant stayed green; `ucrtbase.dll`'s
`qsort`, called from a script over the whole list, puts the trio t1, t001, t01 when it is written
last, the reverse of a stable sort, so the generator writes it last.) Dropping guard 1 turns test 1
red. Removing the `fz_convert_error` from `create`'s catch turns test 6 red. Removing it from the
decode catch in `try_image` leaves test 6 green, and no test can tell: that catch always runs
inside `header_size`'s own `fz_try`, and MuPDF's `fz_do_catch` overwrites the error code when that
outer try completes; it stays for the #146 rule. Dropping the reset in `close()` leaves test 7
green (`open()` resets too) and turns test 9 red.

### 5.3 Read-ahead tests (`tests/unit/test_zip_read_ahead.cpp`, `[core][zip]`)

10. `read_zip_directory` on `cbz-sizing.cbz`, `sample.cbz`, `strip-images.cbz` and `sample.epub`:
    the entry count equals `fz_count_archive_entries` and the names, in order, equal
    `fz_list_archive_entry(i)` on a test-owned archive; at every `local_header_offset` the file
    holds `50 4B 03 04`.
11. Crafted buffers: `find_zip_directory` returns nullopt for an empty tail, a tail with no
    signature, a signature whose comment length does not reach the end, and each Zip64 escape value
    (count `0xFFFF`, size or offset `0xFFFFFFFF`). A zip comment that contains the bytes
    `50 4B 05 06` does not fool it: the backward scan skips a candidate whose comment length does
    not reach the end of the file. `parse_zip_directory` returns nullopt for a wrong record
    signature, a name running past the directory, and a local offset at the directory start.
12. `prefetch_file_ranges` returns true on `cbz-sizing.cbz` with its page offsets, false for a
    missing file, and after it returns the file can be removed (handles closed).
13. The existing tests 2-9 run with the read-ahead in `create` (it is unconditional), so they show
    that sizing with it still equals the full read and that `close()` still releases the file.
    Whether the read-ahead makes sizing faster is not observable from a unit test (the file cache
    has no residency query a test can use without timing); §6 settles it.

Mutation checks for §3.8 (recorded in the PR): making `create` skip the read-ahead leaves every
unit test green (it is a cache hint) and is caught by §6 only; corrupting the parsed offsets by
+1 turns test 10 red.

`test_strip_helpers.cpp` changes as §4 says. Test names are ASCII and prefixed `CbzPageSizer:` or
`ZipReadAhead:`.

## 6. Gate (local, not in CI)

Scratch files, never committed: 300 stored 1200×1700 JPEGs (510 MB as generated); the same as PNG
(193 MB); the same JPEGs saved with an sRGB ICC profile; the same JPEGs with a 16 KB COM segment
after SOI (`hdr16k`, standing in for the EXIF thumbnails and Photoshop/XMP blocks real scans carry).

Protocol per file: evict the file from the OS file cache by opening it with
`FILE_FLAG_NO_BUFFERING` and closing it. Checked on 2026-10-25 with the build at 7b33740: a 510 MB
JPEG CBZ read 1,009-1,029 ms in three cold singles against a 516 ms warm median, and a 193 MB PNG
CBZ 567-602 ms against 150 ms. Cold singles 2 and 3 each ran right after the previous single had
read the whole file into the cache and stayed slow, so the eviction holds. (Those readings
validate the method only; they are not gate readings.) The helper is a local script, not
committed. The gate readings come from a Release build of the branch head that contains the
implementation, and the gate record names that commit. Then
`litepdf-cli <file> --bench-page-sizes --iterations 1`, three times (cold singles); then
`--iterations 9` once (warm median). Pass: every cold single ≤ 100 ms for the JPEG and PNG files.
The ICC and `hdr16k` files are recorded, not gated. Recorded in the upstream spec §4.4 with the
drive (NVMe); the read pattern (§3.8 "Why": three 4 KB reads over four pages per page, served from
the cache after the read-ahead); that the read-ahead was measured on NVMe only (on a hard drive
its value depends on the drive's command queue, not measured); and that `lookup_zip_entry` is a
linear name search, so CBZ sizing is O(N²) in name compares.

A cold miss stops the PR (CBZ stays paged; the header path may still land for paged sizing).

### 6.1 Record: the first implementation (247646e) missed

Build at 247646e (§3.3 without §3.8), same protocol, 2026-10-25/26:

| file | before #176 (7b33740) cold / warm | 247646e cold / warm |
|---|---|---|
| JPEG 510 MB | 1,009-1,029 / 516 ms | 122-149 / 5.2 ms |
| PNG 193 MB | 567-602 / 150 ms | 122-130 / 4.6 ms |
| JPEG + ICC | - | 124-132 / 6.7 ms |
| JPEG `hdr16k` | - | 360-362 / 18.7 ms |

A 2 KB first step (experiment, not kept) gave 80-98 ms on the first three and 149 ms on `hdr16k`
(the latter read is from a binary later found to predate a source revert; the 360 ms row is the
clean one). The cold-read floor for one unbuffered read per entry, from a script: 512 B 48-50 ms,
4 KB 67-76, 8 KB 86-96, 12 KB 102-104, 16 KB 151-153. With the pages prefetched by a script
(`PrefetchVirtualMemory`, 64 KB per entry, the file evicted first): call 17-41 ms, then sizing in a
separate process 4.2-5.5 ms (JPEG), 4.2-4.5 (PNG), 18.5-19.3 (`hdr16k`). Hence §3.8.

## 7. Review

Tier Full (src/core + CMake). Spec gate now; PR-merge gate before merge.
