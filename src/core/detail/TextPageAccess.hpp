#pragma once

// core-internal (#98): the definition of Document::TextPage::Impl and the one
// text-page builder. Document.hpp stays MuPDF-free, so this header -- which names
// fz_stext_page -- is included only by translation units that already include
// <mupdf/fitz.h>: Document.cpp and RenderEngine.cpp.

#include "core/Document.hpp"
#include "core/EscrowContext.hpp"

#include <mupdf/fitz.h>

namespace litepdf::core {

struct Document::TextPage::Impl {
    // The destructor body drops `stext` THROUGH `escrow`. Members are destroyed
    // only after the body has run, so the escrow is alive for that drop; the
    // escrow itself then drops its context before releasing the lock table.
    // Net order: stext page -> escrow context -> lock table (spec §3.3).
    // With share() this runs when the last handle on the Impl is released.
    EscrowContext  escrow;
    fz_stext_page* stext = nullptr;
    int            page  = -1;
    // The page's first and last character edges. Invariant for the handle's
    // life -- the stext page never changes -- so it is computed once at
    // acquisition instead of per WM_MOUSEMOVE: snap() consults it on every
    // word or line drag move, and the walk is over every character on the page.
    bool           has_range  = false;
    SelPoint       range_first{};
    SelPoint       range_last{};
    // highlight() runs on every drag move; log its quad cap once per handle.
    bool           quad_cap_logged = false;

    Impl() = default;
    ~Impl() {
        if (stext) fz_drop_stext_page(escrow.get(), stext);
    }
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
};

namespace detail {

struct TextPageAccess {
    // The single text-page builder: Document::text_page (under doc_mutex) and
    // the render worker (on its own document, lock-free) both call it, so the
    // option block exists once (spec §3.1).
    //
    // `ctx` and `doc` belong to the caller, and the caller serialises their use
    // (doc_mutex for the Document's, the worker's own thread for its clone). The
    // handle gets its OWN escrow clone of `ctx`, so it outlives both. Empty on
    // every failure -- a bad index, a MuPDF error, a failed clone, out of memory
    // -- and never throws.
    [[nodiscard]] static Document::TextPage build(fz_context* ctx, fz_document* doc,
                                                  int index) noexcept;
};

} // namespace detail

} // namespace litepdf::core
