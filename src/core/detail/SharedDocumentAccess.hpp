#pragma once

// core-internal (#229): lends a Document's own fz_document to a borrower's own
// fz_context for one hold of the Document's gate. The render worker and the
// search session of an FB2 tab reach the tab's only copy of the book this way
// (DocumentShare::Shared). Document.hpp stays MuPDF-free, so this header --
// which names fz_document -- is included only by core translation units and
// tests that already build against <mupdf/fitz.h>.
//
// MuPDF allows one document on several threads "as long as there are
// safeguards in place to prevent the usages being simultaneous"
// (docs/reference/c/overview.md, rule 2). The gate is that safeguard. Rules
// for `fn`, each one load-bearing:
//
//   - Every fz_page lives inside one call. fz_load_page, everything that reads
//     the page (a display list, an stext page, a search on it) and
//     fz_drop_page run inside the same `fn`. fz_drop_page writes the page's
//     links into the document's open-page list and drops its document
//     reference with no lock of its own, and the next load walks that list, so
//     a drop outside the gate races another thread's load. No fz_page* leaves
//     `fn`.
//   - What may leave `fn`: a display list, an stext page (inside a
//     Document::TextPage) and copied hits. Those keep fonts and images, not the
//     document or the page; their refcounts go through FZ_LOCK_ALLOC, and they
//     are dropped later on any context of the family.
//   - No C++ exception and no longjmp crosses `fn`'s boundary in practice: `fn`
//     wraps its MuPDF calls in its own fz_try on the borrower's context (#179:
//     no C++ object with a destructor inside it). A C++ exception that does
//     escape still releases the gate (GateHold).
//   - Only MuPDF work and the copying of its results run inside `fn`: no
//     callback into UI or caller code, no wait on another thread, no other lock
//     (detail/PriorityGate.hpp's invariant).
//   - The document is dropped last, on the Document's own context:
//     ~DocumentView resets the session (which drains its tasks), then the
//     engine (which joins its workers), then the Document dies.
//
// Once a DocumentView exists its Document is reached only as const Document&,
// so open(), close() and authenticate() -- non-const and ungated -- never run
// beside a borrower.

#include "core/Document.hpp"
#include "core/detail/PriorityGate.hpp"

#include <mupdf/fitz.h>

#include <memory>
#include <type_traits>

namespace litepdf::core::detail {

struct SharedDocumentAccess {
    // Runs fn(fz_document*) with the Document's document, holding its gate in
    // class `cls` for the whole call. `fn` uses the CALLER's own fz_context.
    // False (fn not run) when the document is not open. Throws what
    // PriorityGate::acquire throws; a C++ exception out of fn releases the
    // gate. No allocation.
    template <class F>
    static bool with_document(const Document& d, GateClass cls, F&& fn) {
        using Fn = std::remove_reference_t<F>;
        return borrow(
            d, cls,
            [](void* p, fz_document* doc) { (*static_cast<Fn*>(p))(doc); },
            const_cast<void*>(static_cast<const void*>(std::addressof(fn))));
    }

    // Test seams.
    static PriorityGate& gate(const Document& d) noexcept;
    // `hook(arg, cls)` runs on the borrower's thread inside with_document, after
    // the gate is released and before it returns, with the borrower's class.
    // Set before any borrower runs (it is written under a Foreground hold, and
    // read by each borrower under its own). A plain function pointer and
    // argument, so a borrower copies them while it holds the gate and reads
    // nothing of `d` after the release: the Document may be destroyed as soon as
    // the last hold ends.
    static void set_after_release_hook(const Document& d,
                                       void (*hook)(void* arg, GateClass cls), void* arg);

private:
    static bool borrow(const Document& d, GateClass cls,
                       void (*call)(void*, fz_document*), void* fn);
};

}  // namespace litepdf::core::detail
