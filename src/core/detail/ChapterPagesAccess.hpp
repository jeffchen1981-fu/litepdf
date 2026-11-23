#pragma once

// core-internal (#232): how a separate fz_document of the same file (a render
// worker's or a search session's own copy) gets the Document's chapter table,
// so it loads pages by chapter location too. The table is the Document's own
// immutable one, shared by shared_ptr; a copy takes it only after its own
// chapter count equals the table's (a file replaced under the open Document
// would otherwise map pages onto the wrong chapters).

#include "core/Document.hpp"
#include "core/detail/ChapterPages.hpp"

#include <memory>

struct fz_context;
struct fz_document;

namespace litepdf::core::detail {

struct ChapterPagesAccess {
    // The Document's table, read under a Foreground hold; null when there is none or the
    // Document is not open. Throws std::system_error from the hold on re-entry (#70).
    static std::shared_ptr<const ChapterPages> table(const Document& d);
};

// `table` when `copy`'s chapter count equals table->chapter_count(); null when `table` is
// null (no MuPDF call), the count throws (reported, #146) or differs. Call it right after the
// copy is opened and authenticated, on the copy's context.
std::shared_ptr<const ChapterPages> table_for_copy(fz_context* ctx, fz_document* copy,
                                                   std::shared_ptr<const ChapterPages> table) noexcept;

}  // namespace litepdf::core::detail
