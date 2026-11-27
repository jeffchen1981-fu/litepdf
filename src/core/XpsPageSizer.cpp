#include "core/detail/XpsPageSizer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

// MuPDF's internal XPS types: xps_document (its archive and page list) and
// xps_load_page (the handler identity). This is the only file that includes it.
extern "C" {
#include "xps-imp.h"
}

#include <windows.h>   // MultiByteToWideChar

// Spec: docs/superpowers/specs/2026-11-26-xps-header-sizing-227-design.md.
//
// MuPDF error handling (#146): every fz_catch here is an expected-failure
// probe (a cut that ends inside markup, an entry that is not there), so it
// takes the error with fz_convert_error and discards it, and no error is
// reported per page. No C++ object with a destructor lives inside a fz_try,
// and no fz_try body is left by return, break or continue (#179): each MuPDF
// call that can throw sits in a helper of its own that reports by return value.

namespace litepdf::core::detail {

namespace {

// Spec section 3.3: the prefix grows by this much per read, up to the cap; at
// most this many cuts are handed to the parser.
constexpr std::size_t kXpsReadStep = 512;
constexpr std::size_t kXpsRootCap = 16 * 1024;
constexpr int kXpsCutTries = 16;

XpsPageSizer::PrefetchFn g_prefetch = prefetch_file_ranges;

// fz_read of up to `n` bytes into `dst`; false when it throws.
bool read_some(fz_context* ctx, fz_stream* stm, unsigned char* dst, std::size_t n,
               std::size_t* got) noexcept {
    bool ok = true;
    *got = 0;
    fz_try(ctx) {
        *got = fz_read(ctx, stm, dst, n);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        ok = false;
    }
    return ok;
}

enum class Parse {
    Error,        // the parser threw: the cut fell inside markup
    NoRoot,       // no element yet (an XML declaration or a comment only)
    NotSizable,   // the root is not a FixedPage, or has no Width or Height
    Sized,
};

// MuPDF's parse of the `n` bytes at `data`, and the root's Width and Height as
// xps_load_fixed_page (xps-doc.c) reads them.
Parse parse_cut(fz_context* ctx, const unsigned char* data, std::size_t n, int* w,
                int* h) noexcept {
    fz_buffer* buf = nullptr;
    fz_xml_doc* xml = nullptr;
    Parse result = Parse::Error;
    fz_var(buf);
    fz_var(xml);
    fz_try(ctx) {
        buf = fz_new_buffer_from_copied_data(ctx, data, n);
        xml = fz_parse_xml(ctx, buf, 0);
        fz_xml* root = fz_xml_root(xml);
        if (!root) {
            result = Parse::NoRoot;
        } else if (!fz_xml_is_tag(root, "FixedPage")) {
            // An AlternateContent root MuPDF resolves through its children,
            // which the prefix may not hold: the page is loaded instead.
            result = Parse::NotSizable;
        } else {
            const char* width = fz_xml_att(root, "Width");
            const char* height = fz_xml_att(root, "Height");
            if (!width || !height) {
                result = Parse::NotSizable;
            } else {
                *w = std::atoi(width);
                *h = std::atoi(height);
                result = Parse::Sized;
            }
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        result = Parse::Error;
    }
    return result;
}

bool starts_utf16(const std::vector<unsigned char>& b) {
    return b.size() >= 2 &&
           ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF));
}

// True when the first `n` bytes of `b`, after an optional UTF-8 BOM, are ASCII.
bool ascii_cut(const std::vector<unsigned char>& b, std::size_t n) {
    std::size_t i = 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;
    for (; i < n; ++i) {
        if (b[i] >= 0x80) return false;
    }
    return true;
}

// Spec section 3.3: the root start tag's Width and Height, read from a prefix
// of `stm`, or nullopt.
std::optional<std::pair<int, int>> read_root_size(fz_context* ctx, fz_stream* stm) noexcept {
    try {
        std::vector<unsigned char> buffer;
        buffer.reserve(kXpsRootCap);
        int tried = 0;
        std::size_t next_scan = 0;
        for (;;) {
            if (buffer.size() >= kXpsRootCap) return std::nullopt;
            const std::size_t old = buffer.size();
            const std::size_t step = (std::min)(kXpsReadStep, kXpsRootCap - old);
            buffer.resize(old + step);
            std::size_t got = 0;
            if (!read_some(ctx, stm, buffer.data() + old, step, &got)) return std::nullopt;
            buffer.resize(old + got);
            if (got == 0) return std::nullopt;
            // A cut on a '>' byte could split a UTF-16 code unit.
            if (starts_utf16(buffer)) return std::nullopt;

            for (std::size_t i = next_scan; i < buffer.size(); ++i) {
                if (buffer[i] != '>') continue;
                if (++tried > kXpsCutTries) return std::nullopt;
                const std::size_t n = i + 1;
                // fz_parse_xml may pick an encoding from a meta element past
                // the prefix; an ASCII cut parses the same under any of them.
                if (!ascii_cut(buffer, n)) return std::nullopt;
                int w = 0;
                int h = 0;
                switch (parse_cut(ctx, buffer.data(), n, &w, &h)) {
                case Parse::Error:
                case Parse::NoRoot:
                    continue;   // the next '>'
                case Parse::NotSizable:
                    return std::nullopt;
                case Parse::Sized:
                    return std::make_pair(w, h);
                }
            }
            next_scan = buffer.size();
        }
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

// The part `name` of `zip`, without its leading '/' as xps_read_part, or
// nullptr when it cannot be opened (a piece-split part has no such entry).
fz_stream* open_part(fz_context* ctx, fz_archive* zip, const char* name) noexcept {
    fz_stream* stm = nullptr;
    fz_try(ctx) {
        stm = fz_open_archive_entry(ctx, zip, name + (name[0] == '/'));
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        stm = nullptr;
    }
    return stm;
}

// Spec section 3.7: xps_bound_page's expression minus the rect's zero origin.
PageSize page_size_of(int w, int h) {
    return PageSize{w * 72.0f / 96.0f - 0.0f, h * 72.0f / 96.0f - 0.0f};
}

// Spec section 3.9: per page, the 30-byte local header, the page name and the
// first compressed bytes, which a 16 KiB root of text never exceeds.
constexpr std::size_t kXpsReadAheadSpan = 12 * 1024;

constexpr std::size_t kLocalHeaderSize = 30;
// A central field MuPDF takes from a Zip64 extra field instead
// (read_zip_dir_imp, unzip.c).
constexpr std::uint32_t kZip64Marker = 0xFFFFFFFFu;

unsigned le16(const unsigned char* p) {
    return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
}

// Spec section 3.4: the lookup key. Two names fold equal exactly when
// fz_strcasecmp (string.c) returns 0: it compares the same runes, lowered
// the same way, up to the first NUL.
std::u32string fold(const char* s) {
    std::u32string out;
    for (;;) {
        int rune = 0;
        s += fz_chartorune(&rune, s);
        if (rune == 0) return out;
        out.push_back(static_cast<char32_t>(fz_tolower(rune)));
    }
}

// A page name's key, turned in MuPDF's order: xps_read_part drops one leading
// '/' (xps-zip.c), fz_open_archive_entry cleans a copy sized as
// fz_cleanname_strdup sizes it (archive.c, string.c: an empty name becomes
// "."), lookup_zip_entry drops one leading '/' again (unzip.c).
std::u32string page_key(const std::string& name) {
    const char* p = name.c_str();
    if (*p == '/') ++p;
    const std::size_t len = std::strlen(p);
    std::vector<char> clean((std::max)(std::size_t{2}, len + 1));
    std::memcpy(clean.data(), p, len + 1);
    const char* q = fz_cleanname(clean.data());
    if (*q == '/') ++q;
    return fold(q);
}

// fz_count_archive_entries, or -1 when it throws.
int count_entries(fz_context* ctx, fz_archive* zip) noexcept {
    int n = -1;
    fz_try(ctx) {
        n = fz_count_archive_entries(ctx, zip);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        n = -1;
    }
    return n;
}

// fz_list_archive_entry, or nullptr when it throws.
const char* entry_name(fz_context* ctx, fz_archive* zip, int i) noexcept {
    const char* name = nullptr;
    fz_try(ctx) {
        name = fz_list_archive_entry(ctx, zip, i);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        name = nullptr;
    }
    return name;
}

// Spec section 3.4, the binding: true when MuPDF's directory has as many
// entries as `entries` and each stored name is byte-equal to the raw name of
// the same record, length included. `*names` gets MuPDF's names (they live as
// long as the archive).
bool binds(fz_context* ctx, fz_archive* zip, const std::vector<ZipEntryRef>& entries,
           std::vector<const char*>* names) {
    const int n = count_entries(ctx, zip);
    if (n < 0 || static_cast<std::size_t>(n) != entries.size()) return false;
    names->reserve(entries.size());
    for (int i = 0; i < n; ++i) {
        const char* const name = entry_name(ctx, zip, i);
        if (!name) return false;
        const std::string& raw = entries[static_cast<std::size_t>(i)].name;
        if (std::strlen(name) != raw.size() || std::memcmp(name, raw.data(), raw.size()) != 0)
            return false;
        names->push_back(name);
    }
    return true;
}

// The UTF-16 form of `utf8`, or nullopt when it is not valid UTF-8 or empty.
std::optional<std::wstring> wide_of(const std::string& utf8) {
    const int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (wide <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), out.data(), wide) != wide)
        return std::nullopt;
    return out;
}

// Seeks `file` to `offset` and reads up to a local header's 30 bytes into
// `header`: the count read, 0 when it throws.
std::size_t read_local_header(fz_context* ctx, fz_stream* file, std::uint64_t offset,
                              unsigned char* header) noexcept {
    std::size_t got = 0;
    fz_try(ctx) {
        fz_seek(ctx, file, static_cast<std::int64_t>(offset), SEEK_SET);
        got = fz_read(ctx, file, header, kLocalHeaderSize);
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        got = 0;
    }
    return got;
}

// Spec section 3.4: entry `e`'s data at `data` in `file`, as open_zip_entry
// (unzip.c) opens it but bounded by the entry: method 0, a null filter over
// the uncompressed size; method 8, a null filter over the compressed size
// under the same raw inflate. Returns ONE owned stream for the caller to drop
// (for method 8 the flate stream, which keeps its null filter alive with
// fz_keep_stream, filter-flate.c), or nullptr when opening throws.
fz_stream* open_direct(fz_context* ctx, fz_stream* file, unsigned method, const ZipEntryRef& e,
                       std::int64_t data) noexcept {
    fz_stream* null = nullptr;
    fz_stream* flate = nullptr;
    fz_var(null);
    fz_var(flate);
    fz_try(ctx) {
        null = fz_open_null_filter(ctx, file, method == 8 ? e.compressed_size : e.uncompressed_size,
                                   data);
        if (method == 8) flate = fz_open_flated(ctx, null, -15);
    }
    fz_always(ctx) {
        if (method == 8) {
            // The flate stream holds its own reference; this one is done.
            fz_drop_stream(ctx, null);
            null = nullptr;
        }
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
        fz_drop_stream(ctx, flate);
        flate = nullptr;
        fz_drop_stream(ctx, null);
        null = nullptr;
    }
    return method == 8 ? flate : null;
}

enum class DirectResult {
    Sized,      // `*wh` holds the root's Width and Height
    Declined,   // nullopt: the page is loaded instead
    Stream,     // the header could not be read: the stream route decides
};

// Spec section 3.4, one page through the archive's own file stream, as
// read_zip_entry_header (unzip.c) reads the local header: its method, name
// length and extra length, never its sizes.
DirectResult direct_size(fz_context* ctx, fz_archive* zip, const ZipEntryRef& e,
                         std::pair<int, int>* wh) noexcept {
    if (e.compressed_size == kZip64Marker || e.uncompressed_size == kZip64Marker ||
        e.local_header_offset == kZip64Marker)
        return DirectResult::Stream;

    unsigned char header[kLocalHeaderSize];
    if (read_local_header(ctx, zip->file, e.local_header_offset, header) != kLocalHeaderSize)
        return DirectResult::Stream;
    if (std::memcmp(header, "PK\x03\x04", 4) != 0) return DirectResult::Stream;
    if (le16(header + 6) & 1u) return DirectResult::Declined;   // encrypted: MuPDF throws
    const unsigned method = le16(header + 8);
    if (method != 0 && method != 8) return DirectResult::Declined;   // MuPDF throws
    const std::int64_t data = static_cast<std::int64_t>(e.local_header_offset) +
                              static_cast<std::int64_t>(kLocalHeaderSize) + le16(header + 26) +
                              le16(header + 28);

    fz_stream* const stm = open_direct(ctx, zip->file, method, e, data);
    if (!stm) return DirectResult::Stream;
    // Read outside any fz_try (read_root_size holds a std::vector), then
    // dropped once; fz_drop_stream does not throw.
    const std::optional<std::pair<int, int>> got = read_root_size(ctx, stm);
    fz_drop_stream(ctx, stm);
    // A flate error ends the stream early (fz_available): the stream route
    // would inflate the same bytes, so a missing root is final here.
    if (!got) return DirectResult::Declined;
    *wh = *got;
    return DirectResult::Sized;
}

}  // namespace

std::unique_ptr<XpsPageSizer> XpsPageSizer::create(fz_context* ctx, fz_document* doc,
                                                   const char* utf8_path,
                                                   std::size_t page_count) noexcept {
    if (!ctx || !doc || !utf8_path) return nullptr;
    // Guard 1, handler identity: MuPDF links statically, so the XPS handler's
    // pages are loaded by this very function.
    if (doc->load_page != xps_load_page) return nullptr;
    auto* const xps = reinterpret_cast<xps_document*>(doc);
    // Guard 2: the archive is a zip (section 3.4 reads its directory).
    fz_archive* const zip = xps->zip;
    if (!zip || !zip->format || std::strcmp(zip->format, "zip") != 0) return nullptr;

    try {
        // Guard 3: MuPDF's own page list, whole.
        std::vector<std::string> names;
        for (const xps_fixpage* page = xps->first_page; page; page = page->next) {
            if (!page->name) return nullptr;
            names.emplace_back(page->name);
        }
        if (names.size() != page_count) return nullptr;
        return std::unique_ptr<XpsPageSizer>(
            new XpsPageSizer(ctx, doc, zip, std::move(names), utf8_path));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

XpsPageSizer::XpsPageSizer(fz_context* ctx, fz_document* doc, fz_archive* zip,
                           std::vector<std::string> names, std::string utf8_path) noexcept
    : ctx_(ctx), doc_(doc), zip_(zip), names_(std::move(names)),
      utf8_path_(std::move(utf8_path)) {}

void XpsPageSizer::set_prefetch_for_test(PrefetchFn fn) noexcept {
    g_prefetch = fn ? fn : prefetch_file_ranges;
}

bool XpsPageSizer::prepare_all() noexcept {
    if (prepared_) return false;
    prepared_ = true;
    try {
        const std::optional<std::wstring> path = wide_of(utf8_path_);
        if (!path) return true;
        // Section 3.4: the directory read from the file, used only if it is
        // MuPDF's own record for record.
        std::optional<std::vector<ZipEntryRef>> entries = read_zip_directory(*path);
        if (!entries) return true;
        std::vector<const char*> mupdf_names;
        if (!binds(ctx_, zip_, *entries, &mupdf_names)) return true;

        // A duplicate key resolves to its first entry, as MuPDF's first-match
        // scan does (emplace keeps an existing key).
        std::unordered_map<std::u32string, std::size_t> by_key;
        by_key.reserve(mupdf_names.size());
        for (std::size_t i = 0; i < mupdf_names.size(); ++i) {
            by_key.emplace(fold(mupdf_names[i]), i);
        }

        // A page with no entry (MuPDF reads it as pieces) keeps the stream route.
        std::vector<std::optional<std::size_t>> entry_of_page(names_.size());
        std::vector<std::uint64_t> offsets;
        offsets.reserve(names_.size());
        for (std::size_t p = 0; p < names_.size(); ++p) {
            const auto it = by_key.find(page_key(names_[p]));
            if (it == by_key.end()) continue;
            entry_of_page[p] = it->second;
            const std::uint64_t at = (*entries)[it->second].local_header_offset;
            if (at != kZip64Marker) offsets.push_back(at);
        }
        entries_ = std::move(*entries);
        entry_of_page_ = std::move(entry_of_page);

        // Section 3.9: a cache hint only; its failure changes no size and
        // keeps the direct index.
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(*path, ec);
        if (!ec && !offsets.empty()) {
            (void)g_prefetch(*path, plan_read_ahead(offsets, size, kXpsReadAheadSpan));
        }
    } catch (const std::exception&) {
        // Out of memory: the index is either committed whole or not at all.
    }
    return true;
}

std::optional<PageSize> XpsPageSizer::header_size(std::size_t index, Route* route) noexcept {
    if (route) *route = Route::None;
    if (index >= names_.size()) return std::nullopt;

    // Section 3.4, the direct route, for a page prepare_all resolved.
    if (index < entry_of_page_.size() && entry_of_page_[index]) {
        std::pair<int, int> wh{0, 0};
        switch (direct_size(ctx_, zip_, entries_[*entry_of_page_[index]], &wh)) {
        case DirectResult::Sized:
            if (route) *route = Route::Direct;
            return page_size_of(wh.first, wh.second);
        case DirectResult::Declined:
            return std::nullopt;
        case DirectResult::Stream:
            break;
        }
    }

    // Section 3.5, the stream route: MuPDF's lookup, header read and inflate.
    fz_stream* const stm = open_part(ctx_, zip_, names_[index].c_str());
    if (!stm) return std::nullopt;
    const std::optional<std::pair<int, int>> wh = read_root_size(ctx_, stm);
    fz_drop_stream(ctx_, stm);
    if (!wh) return std::nullopt;

    if (route) *route = Route::Stream;
    return page_size_of(wh->first, wh->second);
}

}  // namespace litepdf::core::detail
