#include "core/detail/XpsPageSizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

// MuPDF's internal XPS types: xps_document (its archive and page list) and
// xps_load_page (the handler identity). This is the only file that includes it.
extern "C" {
#include "xps-imp.h"
}

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
            const std::size_t step = std::min(kXpsReadStep, kXpsRootCap - old);
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
    return false;
}

std::optional<PageSize> XpsPageSizer::header_size(std::size_t index, Route* route) noexcept {
    if (route) *route = Route::None;
    if (index >= names_.size()) return std::nullopt;

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
