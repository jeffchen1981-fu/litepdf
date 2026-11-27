#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// PageSize only; Document.hpp is MuPDF-free.
#include "core/Document.hpp"
#include "core/detail/ZipReadAhead.hpp"

#include <mupdf/fitz.h>

// #227: an XPS page's size from the root start tag of its FixedPage part.
// MuPDF's xps_load_page reads and parses the whole part before it can bound the
// page, so sizing every page of a real-size XPS for the continuous-scroll strip
// parsed the whole document on the UI thread (spec docs/superpowers/specs/
// 2026-11-26-xps-header-sizing-227-design.md). This reads a prefix of each part
// and hands it to MuPDF's own XML parser, so the size is the one
// fz_load_page + fz_bound_page gives, bit for bit, for every part MuPDF can
// read in full (spec section 3.6 lists the exceptions).
//
// The page list, the archive and the part names are MuPDF's own (the
// xps_document it opened); this never owns an archive.
//
// Only Document.cpp and unit tests include this header.

namespace litepdf::core::detail {

class XpsPageSizer {
public:
    // Which path sized a page (test seam; production ignores it).
    enum class Route { None, Direct, Stream };

    // The read-ahead call prepare_all makes (test seam).
    using PrefetchFn = bool (*)(const std::wstring&, const std::vector<FileRange>&) noexcept;

    // A sizer for `doc`, or nullptr when it does not apply: `doc` was not
    // opened by MuPDF's XPS handler, its archive is not a zip, or MuPDF's page
    // list does not have `page_count` entries. Reads no file. Never throws.
    static std::unique_ptr<XpsPageSizer> create(fz_context* ctx, fz_document* doc,
                                                const char* utf8_path,
                                                std::size_t page_count) noexcept;

    XpsPageSizer(const XpsPageSizer&) = delete;
    XpsPageSizer& operator=(const XpsPageSizer&) = delete;

    // Page `index`'s size from the root of its FixedPage part, or nullopt when
    // the caller must load the page instead (spec section 3.6), or for an index
    // past the list. `*route`, when given, is the route that produced the size,
    // None for nullopt. Never throws; a MuPDF failure is marked handled.
    std::optional<PageSize> header_size(std::size_t index, Route* route = nullptr) noexcept;

    // Every page is about to be sized: builds the direct index (spec section
    // 3.4) and reads every page part's first bytes ahead (section 3.9), once.
    // True when this call was the one that did it, whatever the outcome.
    bool prepare_all() noexcept;

    // Replaces the read-ahead call prepare_all makes; nullptr restores
    // prefetch_file_ranges.
    static void set_prefetch_for_test(PrefetchFn fn) noexcept;

private:
    XpsPageSizer(fz_context* ctx, fz_document* doc, fz_archive* zip,
                 std::vector<std::string> names, std::string utf8_path) noexcept;

    fz_context* ctx_;    // borrowed: the Document's own context
    fz_document* doc_;   // borrowed
    fz_archive* zip_;    // borrowed: the archive MuPDF opened (xps_document::zip)
    std::vector<std::string> names_;   // page part names, in MuPDF's page order
    std::string utf8_path_;
};

}  // namespace litepdf::core::detail
