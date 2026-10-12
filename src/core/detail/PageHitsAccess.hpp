#pragma once

// core-internal (#130): the lock-free page-hits helper and the interlocked
// cookie accessors a search scan uses. Document.hpp stays MuPDF-free, so this
// header -- which names fz_context, fz_document and fz_cookie -- is included only
// by translation units that already build against <mupdf/fitz.h>.

#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <intrin.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace litepdf::core::detail {

// The interlocked intrinsics take a long; fz_cookie::abort is an int. Both are
// 32 bits on every Windows ABI, so the casts below address exactly the field.
static_assert(sizeof(long) == sizeof(int), "fz_cookie::abort is accessed as a long");

// Asks MuPDF to stop the job `c` is passed to. Interlocked, so it is safe from
// any thread while the job runs. MuPDF's contract: once set, `abort` is never
// reset (device.h, the fz_cookie comment). MuPDF's own read of the field is a
// plain load -- an accepted, vendor-defined data race (spec §3.4).
inline void request_abort(fz_cookie& c) noexcept {
    _InterlockedExchange(reinterpret_cast<volatile long*>(&c.abort), 1);
}

// An interlocked read of `c.abort`. Takes a non-const reference because the
// intrinsic needs a writable address; the compare-exchange of 0 with 0 never
// changes the value.
inline bool abort_requested(fz_cookie& c) noexcept {
    return _InterlockedCompareExchange(reinterpret_cast<volatile long*>(&c.abort), 0, 0) != 0;
}

// All hits for `needle_utf8` on `page` of `doc`, the body of Document::page_hits.
// Takes NO lock: the caller serialises its use of `ctx` and `doc` (doc_mutex for
// the tab Document's pair, the session mutex for a search session's own).
//
// The text build runs with `cookie` (null for none), so an abort set from another
// thread stops it at MuPDF's next poll; the match loop then checks the same
// abort and returns whatever hits it already collected. Empty on a null `ctx` or
// `doc`, an empty needle, an invalid regex, or a MuPDF failure, which is logged.
std::vector<Document::PageHit> page_hits_on(
    fz_context* ctx, fz_document* doc, std::size_t page,
    std::string_view needle_utf8, Document::SearchFlags flags, fz_cookie* cookie);

}  // namespace litepdf::core::detail
