# XPS page sizes from the FixedPage root (#227) — design

Status: approved for planning 2026-11-27 after a spike and two Fable design reviews (chat); spec
gate passed 2026-11-27 (routine Full: Opus slot-1, then gpt-5.6-terra, gpt-6.1-terra and
gpt-5.6-luna on one commit; fixes in a6793ff and the commit after it).
Issue: #227. Upstream spec: `docs/superpowers/specs/2026-10-22-continuous-scroll-design.md`
(§4.4 page-sizing gate, §7.3 formats). Precedent: `2026-10-25-cbz-header-sizing-176-design.md`
(CBZ header sizing and the zip read-ahead, `CbzPageSizer` / `ZipReadAhead`).

## 1. Problem

XPS stays out of the continuous-scroll strip: #169 measured `DocumentView::page_sizes()` over
the §4.4 gate of ≤ 100 ms on the UI thread (cold and warm, 1,000-page document).

`Document::page_size_held` (`src/core/Document.cpp`) sizes a non-PDF page with
`fz_load_page` + `fz_bound_page`. For XPS, `xps_load_page` → `xps_load_fixed_page`
(`third_party/mupdf/source/xps/xps-doc.c:400-450`) reads the page's whole FixedPage part
(`xps_read_part`, which inflates it with `read_zip_entry`), parses all of its XML, and keeps only
`atoi(Width)` and `atoi(Height)` of the root element; `xps_bound_page` (xps-doc.c:452-461) returns
`{0, 0, width * 72 / 96, height * 72 / 96}`.

The spike of 2026-11-26 (tools and raw numbers in `~/.claude/compact-dumps/litepdf/227-tools/`,
not in the repo) split that cost on WPF `XpsDocumentWriter` files of Letter pages of random words
(1,003 pages, 4.3 MB, 2,011 zip entries, each `.fpage` followed by its `_rels/N.fpage.rels`), NVMe,
MuPDF 1.27.2 Release:

| path, 1,003 pages | warm | cold |
|---|---|---|
| today (`litepdf-cli --bench-page-sizes`, build 8b9eafb) | 249 ms | 281-297 ms |
| one `fz_has_archive_entry` per page, nothing else | 62 ms | - |
| root-tag prefix through `fz_open_archive_entry` | 81 ms | 101-103 ms |
| the same after one sequential read of the file | - | 77-82 ms |

Two costs, then. `lookup_zip_entry` (`source/fitz/unzip.c:495-504`), a linear `fz_strcasecmp`
scan from entry 0: page k of a WPF file sits near entry 2k, so sizing N pages costs
O(N × entries); 866 → 1,003 pages took one lookup per page from 45 to 62 ms (ratio 1.38, the
square of the page ratio is 1.34). The current path pays it three times per page:
`xps_read_part` calls `fz_has_archive_entry` (xps-zip.c:81), then `fz_read_archive_entry`, whose
`fz_try_read_archive_entry` calls `has_entry` again (archive.c:98) before `read_zip_entry` looks the
name up a third time (unzip.c:551) — about 186 of the 249 ms. The rest is reading and parsing every
whole page (§3.3 removes it). A prefix read
through MuPDF's lookup would pass this machine warm with about 20 % margin, the margin growing
thinner with the page count and on an E-core (about 2× slower on this machine); §3.4 avoids the
scan.

## 2. Goal and non-goals

Goal: size an XPS page from the start of its FixedPage part, with a result bitwise equal to
`fz_load_page` + `fz_bound_page` for every page MuPDF loads, except the damaged-file cases of §3.6;
meet the §4.4 gate cold and warm on a 1,000-page XPS (§6); put XPS in the strip.

Non-goals: patching MuPDF; sizing piece-split parts, AlternateContent roots, UTF-16 parts or
non-ASCII root tags by prefix (they load the page, §3.6); moving page sizing off the UI thread;
HDD performance (§6, as #176); `.oxps` (not in `Document`'s `kAllowedExts`).

## 3. Design

### 3.1 Unit: `detail::XpsPageSizer`

New files `src/core/detail/XpsPageSizer.hpp` and `src/core/XpsPageSizer.cpp` in `litepdf_core`.
As `CbzPageSizer`: MuPDF types appear only in this detail header, which only `Document.cpp` and
unit tests include. Only `XpsPageSizer.cpp` includes MuPDF's internal `xps-imp.h` (§3.2).

```cpp
namespace litepdf::core::detail {

class XpsPageSizer {
public:
    // Which path sized a page (test seam; production ignores it).
    enum class Route { None, Direct, Stream };

    // A sizer for `doc`, or nullptr when it does not apply: `doc` was not opened by MuPDF's XPS
    // handler, its archive is not a zip, or MuPDF's page list does not have `page_count`
    // entries. Reads no file. Never throws; a MuPDF failure is marked handled.
    static std::unique_ptr<XpsPageSizer> create(fz_context* ctx, fz_document* doc,
                                                const char* utf8_path,
                                                std::size_t page_count) noexcept;

    // Page `index`'s size from the root of its FixedPage part, or nullopt when the caller must
    // load the page (§3.6). Never throws; a MuPDF failure is marked handled.
    std::optional<PageSize> header_size(std::size_t index, Route* route = nullptr) noexcept;

    // Every page is about to be sized: builds the direct index (§3.4) and reads every page
    // part's first bytes ahead (§3.9), once. True when this call was the one that did it,
    // whatever the outcome. A directory or binding failure leaves every page on the stream
    // route (§3.5) and skips the read-ahead; a read-ahead failure keeps the direct index.
    bool prepare_all() noexcept;
};

}
```

It holds the Document's context and document (borrowed), the page names (copied from MuPDF's
list), the UTF-8 path, and after `prepare_all` the direct index. It never owns an archive: every
read goes through the archive MuPDF opened (`xps_document::zip`).

### 3.2 The page list: MuPDF's own

`xps_document` and `xps_fixpage` are not in `include/`, but are fully defined in
`third_party/mupdf/source/xps/xps-imp.h` (`struct xps_document_s` at line 193, `xps_fixpage_s` at
line 70), which has no `#include` of its own and no C++ keywords. `XpsPageSizer.cpp` includes it
inside `extern "C" { }` after `<mupdf/fitz.h>`; `litepdf_core` gets
`third_party/mupdf/source/xps` as a PRIVATE include directory. (The spike built a probe exactly so
with cl `/std:c++20 /MT` and linked it against the Release libs.)

`create`:

1. Guard 1, handler identity: `doc->load_page == xps_load_page` (declared in xps-imp.h:33,
   defined non-static in xps-doc.c:470; litepdf links MuPDF statically). Not a format string.
2. Guard 2: the archive `reinterpret_cast<xps_document*>(doc)->zip` is non-null and its
   `format` is `"zip"` (`Document::open` already requires `PK\x03\x04` for `.xps`, so a directory
   archive never reaches here; the check keeps §3.4 honest if that changes).
3. Walk `first_page` → `next`, copying each `name`. Guard 3: the count equals `page_count`
   (`xps_count_pages` returns `page_count` of the same list, so this fails only if a MuPDF change
   makes the list lazy or partial).

So the page order, the de-duplication and the URI resolution are MuPDF's by construction; nothing
of `xps_read_page_list` is copied.

### 3.3 Reading a root tag from a prefix

Shared by both routes (§3.4, §3.5). Input: an `fz_stream*` over the part's inflated bytes.

- Read in 512-byte steps into a buffer, at most `kXpsRootCap` = 16 KiB.
- After each step, for every `>` byte not tried yet (at most `kXpsCutTries` = 16 in all): copy the
  bytes up to and including it into a new buffer and call `fz_parse_xml(ctx, cut, 0)`. A parse
  error means the cut fell inside markup: try the next `>`. A parse with no root element (a cut
  after the XML declaration or a comment only) also tries the next. MuPDF's parser accepts a
  document that ends inside an open element (`parse_text` returns at the terminator,
  `fitz/xml.c:646-656`; open nodes get `next = NULL`, xml.c:1107-1108), so the first cut whose
  parse has a root is the first at which the root start tag is complete; quoting, comments, the
  XML declaration and processing instructions are MuPDF's to judge.
- With a root: it must be `fz_xml_is_tag(root, "FixedPage")` and carry `Width` and `Height`
  (`fz_xml_att`); then `w = atoi(Width)`, `h = atoi(Height)`, with the same `atoi` MuPDF uses.
  Anything else (an `AlternateContent` root, which MuPDF resolves through its children
  (`xps_lookup_alternate_content`); another tag; a missing attribute) → nullopt.
- Before any parse: a buffer starting `FF FE` or `FE FF` (UTF-16; a cut on a `>` byte could split a
  code unit) → nullopt. Every byte of the cut, after an optional UTF-8 BOM, must be below 0x80,
  else nullopt. Why: `fz_parse_xml` picks an encoding from the XML declaration before the first `>`
  or, failing that, from a `<meta http-equiv content-type charset=...>` anywhere in the buffer
  (`find_xml_encoding` / `find_meta_encoding`, xml.c:895-952). A whole page could carry such a
  meta element past the root and the prefix not. Every table MuPDF can load for an
  `encoding_aliases` name (xml.c:824) maps bytes below 0x80 to ASCII, so an ASCII cut with no BOM
  parses the same either way (checked on crafted files for big5, big5-hkscs, gbk, euc-kr, euc-jp,
  euc-tw, koi8-r, windows-1252 at the spec gate); `shift_jis` names a CMap that does not exist
  (`90msp-H`, text-decoder.c:203-204), so the whole page throws, which is §3.6 item 1's class. The
  UTF-8 BOM is the one non-ASCII prefix allowed (WPF writes one on every page), and it is not
  exact: `convert_to_utf8` skips the BOM only when it found no encoding (xml.c:989-1004), so with a
  multi-byte table the BOM's last byte and the root's `<` can decode as one character. §3.6 item 3
  accepts that.
- Cap reached, stream ended, or 16 cuts tried without a root → nullopt.

### 3.4 Direct route (after `prepare_all`)

What `fz_open_archive_entry` does for a zip, with a hash table in place of the linear scan and
the entry's compressed size bounding the read.

**Directory.** `read_zip_directory` (ZipReadAhead) on the file's UTF-16 path, its own `ReadFile`
handle. `ZipEntryRef` gains `compressed_size` and `uncompressed_size` (central record +20, +24,
32-bit). The parser itself is unchanged otherwise (still refuses a Zip64 end record).

**Binding to MuPDF's directory.** The direct index is built only if
`fz_count_archive_entries(ctx, zip)` equals the parsed record count and, for every index i,
`fz_list_archive_entry(ctx, zip, i)` (MuPDF's stored name, `list_zip_entry`, unzip.c:622-628) is
byte-equal to record i's raw name, length included (a raw name holding a NUL fails). MuPDF fills
its entry array in central-directory order. MuPDF runs `unescape` on a name that has no UTF-8
flag (unzip.c:96, :361-362), which changes it only where it holds `#Uxxxx` / `#Lxxxxxx` (turned
into UTF-8; any other byte is copied), and replaces a name with an Info-ZIP Unicode Path field
(0x7075, unzip.c:390-404). Only a name actually rewritten differs, so only an archive holding one
loses the direct route; ordinary ASCII names without the UTF-8 flag (every WPF entry) bind, and
the rule needs no "ASCII only" test. Any mismatch, or
`read_zip_directory` returning nullopt (Zip64, malformed, I/O error), leaves the sizer on the
stream route for every page.

**Lookup key.** `fold(s)`: decode `s` rune by rune with `fz_chartorune`, map each with
`fz_tolower`, stop at the terminating NUL; the result as a `std::u32string`. Two names fold equal
exactly when `fz_strcasecmp` returns 0 (string.c:137-154 compares the same runes). The table maps
`fold(MuPDF name i)` → the first i only, so a duplicate name resolves as MuPDF's first-match scan
does. A page name is turned into its key in MuPDF's order: drop one leading `/` (xps-zip.c:76-77),
`fz_cleanname` on a copy (`fz_open_archive_entry`, archive.c:59) held in a `std::vector<char>` of
`max(2, len + 1)` bytes, as `fz_cleanname_strdup` sizes it (string.c:526: an empty name becomes
`"."`), drop one leading `/` again
(`lookup_zip_entry`, unzip.c:498-499), fold. Each page's entry index is resolved once in
`prepare_all`; a page with no entry (MuPDF then reads it as pieces) takes the stream route.

**Per page** (`header_size`, entry `e`), through the archive's own file stream
(`zip->file`), which MuPDF itself repositions before every read (`read_zip_entry_header` seeks to
the entry): an entry whose central compressed size, uncompressed size or local-header offset is
`0xFFFFFFFF` (MuPDF takes a Zip64 extra field for it, unzip.c:370-388) takes the stream route.
Otherwise, as `read_zip_entry_header` (unzip.c:433-462): seek to the local header; the signature
must be `50 4B 03 04`; general flag bit 0 (encrypted) → nullopt, as MuPDF throws; take the method,
name length and extra length from the **local** header (MuPDF ignores its sizes); data starts after
them. Method 0: `fz_open_null_filter(file, uncompressed_size, data)`, as `open_zip_entry`
(unzip.c:518-519). Method 8: a null filter `fz_open_null_filter(file, compressed_size, data)`
under `fz_open_flated(null, -15)` — the same inflate as MuPDF's stream, bounded by the entry so a
read never runs into the next local header, and never cut short of the entry, so the flate
filter's "premature end" warning (filter-flate.c:75-78) cannot fire on an intact entry. Both
streams are locals of their own (`fz_var`), each dropped in `fz_always`: each filter keeps its
chain (`fz_keep_stream`, filter-basic.c:77, filter-flate.c:142), so a nested call that drops only
the outer stream leaks the null filter and with it a reference on MuPDF's file, which then stays
open after the document is dropped. Any other method → nullopt (MuPDF throws).
Then §3.3. A failure of the header read itself (short read, wrong signature) → stream route for
that page. A throw while reading the data (a flate error) → nullopt: the stream route would
inflate the same bytes.

### 3.5 Stream route (before `prepare_all`, or where §3.4 declines)

`fz_open_archive_entry(ctx, zip, name)` with the name after its leading `/` (as `xps_read_part`),
then §3.3. Exact by construction (MuPDF's lookup, header read and inflate), linear lookup and all.
It serves a paged XPS (one page per `set_viewport`), printing, `litepdf-cli <file>` and any
document §3.4 refused. An entry that does not exist (a piece-split part) throws there → nullopt.

### 3.6 Fallback and accepted deviations

nullopt means `page_size_held` takes its existing `fz_load_page` path, so every fallback is
exact. Always falling back: piece-split pages, an `AlternateContent` root, UTF-16 parts, a root
tag with a non-ASCII byte or longer than 16 KiB, a root other than `FixedPage`, a missing `Width`
or `Height` (the full path then throws, as today), encrypted entries, methods other than 0 and 8.

Where the prefix gives a size and the full path does not:

1. **Damaged past the root tag.** A deflated part corrupt or cut short after the bytes §3.3
   reads, wrong central sizes of a deflated entry (`read_zip_entry` throws at unzip.c:583-586; a
   stored entry's short read only warns, unzip.c:547-549, and both routes read the same
   `uncompressed_size` bytes from the data start, so a stored entry is not in this item), or XML
   malformed past
   the root: the full path throws, `page_size` throws today and the strip gives that page its
   stand-in; the prefix gives the root's size. Rendering that page still fails. Same class as
   #176 §3.4 item 2.
2. **A file changed on disk after open.** `read_zip_directory` reads the file by path when
   `prepare_all` runs, MuPDF holds the handle it opened. A file replaced in between with one whose
   directory names match byte for byte but whose offsets differ would size from the other file
   (each read is bounds-checked; a wrong signature takes the stream route). `CbzPageSizer` reopens
   by path too; accepted for the same reason.

3. **A UTF-8 BOM and a multi-byte `<meta ... charset=...>` element past the root.** The full parse
   decodes the BOM with the table (§3.3) and can swallow the root's `<`: a single-root page then
   throws in MuPDF (tag mismatch), a page with a second root element sizes from that one (both
   measured at the spec gate on crafted files with `charset=gbk`); the prefix gives the first
   root's size. Needs a non-conforming FixedPage (no `meta` element in the XPS schema). Refusing
   BOM prefixes instead would send every WPF page to the full load.

Where a size differs, the consequences are those of #176 §3.4: the strip slot and the bitmap
differ, paged fit and printing scale by the header size.

### 3.7 Size arithmetic

`PageSize{w * 72.0f / 96.0f - 0.0f, h * 72.0f / 96.0f - 0.0f}` with `w`, `h` the `int`s of §3.3:
the expression of `xps_bound_page` (int × float / float) minus the rect's zero origin, as
`page_size_held` computes `x1 - x0`. MuPDF and litepdf both build with `/fp:precise`.

### 3.8 Integration in `Document`

- `Document::Impl` gains `std::string xps_path`, `bool xps_probed`,
  `std::unique_ptr<detail::XpsPageSizer> xps`, next to the CBZ members and with the same rules:
  declared after `root`; `~Impl` resets `xps` before `fz_drop_document`; `reset_cbz()` becomes
  `reset_sizers()` and resets both; `close()` and the start of `open()` call it.
- `open()`: as for `.cbz`, a path whose extension is `.xps` (any case) stores
  `xps_path = absolute_utf8(path)`; empty when that fails (#182), and then no sizer.
- `page_size_held`, before the `fz_try` and after the CBZ block: if `xps_path` is set and
  `!xps_probed`, create once; if `xps` and `header_size(index)` gives a size, return it.
- `read_ahead_page_sizes()`: its early return becomes `!doc || (cbz_path.empty() &&
  xps_path.empty())` (today it returns on an empty `cbz_path`, Document.cpp:746, which would skip
  every XPS); under its gate hold, for an XPS, create if not probed, then `xps->prepare_all()`. Its comment changes from "a CBZ reads its pages' headers ahead" to "every
  page is about to be sized" (`DocumentView::page_sizes()` calls it before `size_pages`, so the
  direct route and the read-ahead are inside the §6 timing).
- Thread safety: the sizer uses only the Document's context, document and archive, always under
  the gate in the Foreground class. No MuPDF stream of that archive is open across a gate release
  (every MuPDF read of a part opens and closes its stream inside one call), so repositioning
  `zip->file` cannot disturb another reader. XPS is never shared (only FB2 is, #229).

### 3.9 Read-ahead

`prepare_all` reuses the directory it read for §3.4: the local-header offsets of the entries the
page names resolved to, `plan_read_ahead` with a fixed span, `prefetch_file_ranges` (one
`PrefetchVirtualMemory` over a read-only mapping that is never read, #176 §3.8). New overload
`plan_read_ahead(offsets, file_size, std::size_t span)`; the existing two-argument one keeps its
clamp and becomes a call to it. `kXpsReadAheadSpan` = 12 KiB: the 30-byte local header, a page
name, and the first 12 KiB of compressed data minus those, which a 16 KiB root cap of text at
WPF's ratio (about 4.5:1) never exceeds. Ranges of a dense file overlap; that only reads the same
pages once. Without the direct index (binding failed) the read-ahead is skipped: the stream route
reads at MuPDF's pace. A cache hint only: its failure changes no size and keeps the direct index.

### 3.10 MuPDF error state (#146)

Every new `fz_catch` is an expected-failure probe (`fz_parse_xml` on a cut inside markup, an
entry that is not there): `fz_convert_error(ctx, nullptr)`, result discarded, so nothing is
printed per page. Buffers, streams and XML trees are dropped in `fz_always`; every MuPDF-owned
local assigned inside a `fz_try` and read in its `fz_always` or `fz_catch` is declared before it and
marked `fz_var` (#178). No `return`, `break` or `continue` leaves a `fz_try` body, and no C++
object with a destructor lives inside one (#179).

### 3.11 MuPDF bump guard

§3.3-§3.5 and §3.7 rely on rules in ten MuPDF files: the FixedPage root rules and the bound
arithmetic (`source/xps/xps-doc.c`), the part-name handling (`source/xps/xps-zip.c`), the struct
layout and `xps_load_page` (`source/xps/xps-imp.h`), the zip lookup, name rewriting, local
header and stream rules (`source/fitz/unzip.c`), the cleaning of a name before every entry call
(`source/fitz/archive.c`), the case folding `fz_strcasecmp` does (`source/fitz/string.c`), the
truncated-document parse, attribute order and encoding choice (`source/fitz/xml.c`), the
encoding tables (`source/fitz/text-decoder.c`), and the null filter's bound and the flate
filter's inflate, warnings and chain ownership (`source/fitz/filter-basic.c`,
`source/fitz/filter-flate.c`). `cmake/ImportMuPDF.cmake` gets the #229 pattern
for each: SHA-256 of the file with CRLF normalised to LF, compared at configure against a
recorded value; a change fails configure with a message naming #227 and the rules to re-check.
The ten files join `CMAKE_CONFIGURE_DEPENDS`. A renamed field fails the build anyway; the hash
catches a changed rule.

## 4. XPS in the strip

- `strip_format_ok` (`src/ui/detail/EffectiveLayout.hpp:73`) accepts `.xps`;
  `tests/unit/test_strip_helpers.cpp` gains the case.
- Smoke: row 0c after 0b in `scripts/smoke-test.ps1` and `docs/SMOKE-CHECKLIST.md` — the §5.1
  fixture opens in the strip with View → Continuous Scroll enabled and checked.
- `CHANGELOG.md` (Unreleased): XPS documents open in continuous scroll. `README.md`'s format list
  where it says which formats scroll continuously.
- Upstream spec §4.4: the XPS block records the §6 readings and that XPS passes, with the drive
  (NVMe) and that a hard drive was not measured (README's hard-drive target, as #176 recorded);
  the "the format passed §4.4" sentence lists XPS.

## 5. Tests

### 5.1 Fixtures

New `scripts/generate-xps-sizing-fixture.py` (Python `zipfile`, fixed `date_time`,
`create_system`, `external_attr`; deflated entries depend on zlib, so the docstring names the
version and the file is not byte-reproducible across zlib versions, as `cbz-sizing.cbz`). After
`zipfile` writes the archive, the generator rewrites p01's local header to carry a 28-byte extra
field (type 0xA220, as System.IO.Packaging writes on every WPF entry; its central record keeps
none), shifting the later local-header offsets in the central directory and the directory offset
in the end record: `zipfile` writes the same extra in both headers, and WPF files differ there,
so without it no test sees a data start taken from the central lengths. It also zeroes the
CRC-32, compressed and uncompressed size fields of p02's (deflated) and p05's (stored) local
headers, as a writer using data descriptors leaves them (MuPDF ignores them), so no test passes
with sizes read from the local header. It writes `_rels/.rels`, the FixedDocumentSequence, one FixedDocument, and these pages, every
size different so a wrong entry shows (the spike's files had one size on every page and could not
tell):

| page | FixedPage part | expected route |
|---|---|---|
| p01 | `Width="816" Height="1056"`, deflated, local extra field ≠ central | Direct |
| p02 | `<?xml ...?>` and a comment before the root, 612 × 792; local sizes zero | Direct (third cut) |
| p03 | `Width="1000.9" Height="  700"` (`atoi` stops / skips spaces) | Direct |
| p04 | `Width="&#56;16"` (character reference) | Direct |
| p05 | stored (method 0), 500 × 400; local sizes zero | Direct |
| p06 | `mc:AlternateContent` root around a FixedPage | None |
| p07 | UTF-16LE with BOM | None |
| p08 | `xml:lang` with a non-ASCII value before `Width` | None |
| p09 | no `Height` | None (and the full path throws) |
| p10 | a 20 KB attribute before `Width` | None |
| p11 | referenced as `/Documents/1/Pages/P11.FPAGE`, stored as `.../p11.fpage` | Direct |
| p12 | split as `p12.fpage/[0].piece` + `[1].last.piece` | None |
| p13 | two entries `p13.fpage` (300 × 300) then `P13.fpage` (400 × 400) | Direct, 300 × 300 |
| p14 | a 64 KB page (random-word Glyphs) behind a short root | Direct |
| p15 | `Name="a>b"` before `Width` (a `>` inside a quoted value, so the first cut throws) | Direct |

`tests/fixtures/xps-sizing.xps` (15 pages) and `tests/fixtures/xps-escaped-name.xps`: three pages
of which one entry's raw name, written without the UTF-8 flag, contains `#U0070` (MuPDF renames
it to `p`), so the binding fails and every page takes the stream route. Both are small; they test
equality, not the gate (the gate files stay local, §6, as #169 ruled).

### 5.2 Unit tests (`tests/unit/test_xps_page_sizer.cpp`, `[core][xps]`)

With a test-owned context and document that collects the context's warnings (the file-local
`OwnDocument` of `tests/unit/test_cbz_page_sizer.cpp:53-120`, with its `throw_on_context`):

1. `create`: non-null for both fixtures; null for `simple.pdf` (another handler) and for
   `page_count` off by one.
2. Before `prepare_all`, every page of `xps-sizing.xps`: `header_size` returns nullopt with route
   None for the table's None rows, Stream for every other row; each size is bitwise equal
   (`std::memcmp` of the floats) to `fz_load_page` + `fz_bound_page`'s `x1 - x0`, `y1 - y0`.
3. After `prepare_all` (returns true once, false the second time): the same, with the table's
   routes.
4. `xps-escaped-name.xps` after `prepare_all`: every page Stream, sizes bitwise equal.
5. Through `Document`: `page_size(i)` equals the test-owned bounds for every page; p09 throws;
   `size_pages` after `read_ahead_page_sizes()` fills the same values.
6. A damaged copy, written by the test to a temp file: p14's compressed bytes past its first
   4 KiB overwritten with `0xFF` in place (the file length and every offset unchanged): the full
   path throws, `header_size` gives p14's size (§3.6 item 1 recorded as a test, not left
   implicit).
7. Mutation checks before commit (`mutation-test-hygiene`): the fold keeping the last duplicate
   instead of the first (p13 must fail), skipping the binding (the escaped fixture must fail), a
   cut only at the first `>` (p02 must fail), a parse error ending the search instead of trying the
   next `>` (p15 must fail), a parse error left uncleared (the error-state test must fail),
   dropping the ASCII rule (p08 must fail), and the data
   start taken from the central record's name and extra lengths (p01 must leave the Direct route),
   and the sizes taken from the local header (p02 and p05 must leave it). A prefetch forced to
   fail (test seam) must leave every Direct row Direct.
8. Stream ownership: after `prepare_all` and `header_size` over every page of a temp copy of the
   fixture, closing the `Document` lets `DeleteFileW` remove the copy (a leaked null filter keeps
   MuPDF's file open, §3.4).

### 5.3 Read-ahead tests (`tests/unit/test_zip_read_ahead.cpp`)

`parse_zip_directory` returns both sizes from crafted records; `plan_read_ahead` with a fixed
span clips to the file and drops ranges at the end; the two-argument overload's existing cases are
unchanged.

## 6. Gate (local, not in CI)

Files (scratch, never committed):

- **dense**: 1,003 Letter pages of random words (4.3 MB), the spike's file
  (`227-tools/make-wpf-xps.ps1 -Words 540000 -Seed 228`);
- **scattered**: the dense file rewritten by a local Python script (`227-tools/scatter-xps.py`)
  that copies every entry in order and inserts after each `.fpage` entry an unreferenced, stored
  entry `Resources/pad/<n>.bin` of 64 KiB seeded random bytes, so page parts lie 64 KiB apart and
  the cold read cannot ride the cache manager's clustering (the dense file's cold reading is only
  20 ms above warm for that reason). MuPDF reads only referenced parts, so the pages and sizes are
  the dense file's. (A WPF `Image` per page does not do it: the plan gate measured WPF writing one
  shared image part for all pages, and the stretched images doubled the page count.) The rewrite
  goes through `zipfile`, so its local headers lose WPF's 0xA220 extra field; the dense file keeps
  them.

Protocol (#176 §6): a Release build of the branch head; per file three cold singles
(`evict.ps1`, then `litepdf-cli <file> --bench-page-sizes --iterations 1`), then
`--iterations 9` (warm median); `--bench-page-sizes` times `DocumentView::page_sizes()`, so the
directory read, the binding, the index and the read-ahead are inside. Also per file one warm
median with the process pinned to an E-core (`start "" /b /wait /affinity <mask>`, `/b` so the
output stays in the redirect) and one cold
single after an hour without touching the file. Pass: on both files, every cold single, the
hour-idle single and the warm median ≤ 100 ms unpinned, and the E-core warm median ≤ 100 ms (no
E-core cold reading: the I/O wait does not depend on the core). Recorded, not gated: the scattered file with the
read-ahead disabled, and `xps-escaped-name`-style binding failure on the dense file (stream route
throughout). Raw output saved next to the scripts. A miss stops the PR before `strip_format_ok`
changes.

## 7. Review

Tier Full (src/core, CMake). Spec gate on this file; PR-merge gate before merge.
