#pragma once

// core-internal (#232): the one page loader. Every per-page load of a
// document goes through load_page_at, so a multi-chapter document (an ePub)
// loads each page by its chapter location (fz_load_chapter_page) whenever a
// chapter table serves that document, instead of fz_load_page, which walks the
// chapters from the first (#230). Defined in Document.cpp; the fz types are
// forward-declared, so this header does not pull in MuPDF.

#include "core/detail/ChapterPages.hpp"

#include <cstddef>

struct fz_context;
struct fz_document;
struct fz_page;

namespace litepdf::core::detail {

// Where to load a flat page from: by chapter location when `by_location`, else
// by flat number. Plain ints, so it crosses into a fz_try with nothing to
// destroy (#179).
struct PageAt {
    bool by_location = false;
    int chapter = 0;
    int page = 0;   // within the chapter
};

// Where flat page `index` lies in `table`. By location only when `table` is
// non-null and holds the index; otherwise by flat number, so an index past the
// table (or a negative int index cast to std::size_t, which wraps past any
// table) loads by flat number and fails as fz_load_page does. `table` must
// serve the document the page is then loaded from: see Document::Impl::chapters.
// Called before the caller's fz_try.
PageAt page_at(const ChapterPages* table, std::size_t index) noexcept;

// Loads the page `at` names: fz_load_chapter_page(at.chapter, at.page) when
// at.by_location, else fz_load_page(index). Called INSIDE the caller's fz_try,
// and throws as MuPDF does; the caller drops the page.
fz_page* load_page_at(fz_context* ctx, fz_document* doc, PageAt at, int index);

}  // namespace litepdf::core::detail
