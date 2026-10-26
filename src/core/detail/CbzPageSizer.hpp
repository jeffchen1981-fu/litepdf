#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// PageSize only; Document.hpp is MuPDF-free.
#include "core/Document.hpp"

#include <mupdf/fitz.h>

// #176: a CBZ page's size from its image header. MuPDF's cbz_load_page reads
// the whole image out of the archive before it reads the header, so sizing
// every page of a real-size CBZ for the continuous-scroll strip read the whole
// archive on the UI thread (spec docs/superpowers/specs/
// 2026-10-25-cbz-header-sizing-176-design.md). This reads a prefix of each
// entry and hands it to MuPDF's own image-info code, so the size is the one
// fz_load_page + fz_bound_page gives, bit for bit, for every entry MuPDF can
// read in full (spec section 3.4 lists the two exceptions).
//
// Only Document.cpp and unit tests include this header.

namespace litepdf::core::detail {

class CbzPageSizer {
public:
    // A sizer for `doc`, or nullptr when the header path does not apply: the
    // archive at `utf8_path` does not open, MuPDF did not open `doc` with its
    // CBZ handler, or the page list rebuilt from the archive does not have
    // `page_count` entries. Never throws; a MuPDF failure is marked handled.
    static std::unique_ptr<CbzPageSizer> create(fz_context* ctx, fz_document* doc,
                                                const char* utf8_path,
                                                std::size_t page_count) noexcept;

    ~CbzPageSizer();
    CbzPageSizer(const CbzPageSizer&) = delete;
    CbzPageSizer& operator=(const CbzPageSizer&) = delete;

    // Page `index`'s size from its image header, or nullopt when the caller
    // must load the page instead: a format with no header path, a header
    // MuPDF cannot read from the prefix, or an index past the list. Never
    // throws; a MuPDF failure is marked handled.
    std::optional<PageSize> header_size(std::size_t index) noexcept;

    // Reads every page entry's first bytes into the file cache ahead of
    // header_size, once (spec section 3.8): for when every page is about to be
    // sized. A cache hint only; any failure skips it. True when this call was
    // the one that did it, whether or not the read-ahead itself succeeded.
    bool read_ahead() noexcept;

private:
    CbzPageSizer(fz_context* ctx, fz_archive* arch, std::vector<std::string> names,
                 std::string utf8_path) noexcept;

    fz_context* ctx_;           // borrowed: the Document's own context
    fz_archive* arch_;          // owned
    std::vector<std::string> names_;   // page entries, in MuPDF's page order
    std::string utf8_path_;
    bool read_ahead_done_ = false;
};

}  // namespace litepdf::core::detail
