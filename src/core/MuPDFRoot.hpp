#pragma once

// INTERNAL to litepdf_core: includes <mupdf/fitz.h>. Include it only from .cpp
// files in src/core -- never from a public header.
//
// The root of one Document's MuPDF context family: the lock table MuPDF calls
// through for every context in the family, plus the context every other one is
// cloned from. MuPDF requires a table at fz_new_context time before it will
// clone at all, because clones share the store, the font cache and the glyph
// cache and must serialise access to them.
//
// It is owned through std::shared_ptr, not by the Document alone. fz_clone_context
// copies the lock callbacks -- including `fz.user`, which points HERE -- into
// every clone, and a clone can outlive the Document (a render completion still in
// the message queue when its tab closes; a text-selection handle mid-drag). Every
// such long-lived clone is a core::EscrowContext, which holds a strong reference.
// See #61.

#include <mupdf/fitz.h>

#include <array>
#include <memory>
#include <mutex>

namespace litepdf::core::detail {

struct MuPDFRoot : std::enable_shared_from_this<MuPDFRoot> {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context                    fz{};
    // The context every other context in this Document's family is cloned from.
    // Owned HERE, not by Document::Impl: MuPDF tears the shared colorspace
    // context down in whichever context dies last, and LCMS frees the ICC
    // profiles through the context that created them (fz_new_icc_context stores
    // it as cmsCreateContext user data). If the root died first, that teardown
    // would free through a dangling pointer. See #61 and the follow-up fix.
    fz_context* ctx = nullptr;

    MuPDFRoot();
    ~MuPDFRoot();
    MuPDFRoot(const MuPDFRoot&)            = delete;
    MuPDFRoot& operator=(const MuPDFRoot&) = delete;

    // A new table with its callbacks installed and `fz.user` pointing at it,
    // plus a root context created on it. Throws std::bad_alloc if either
    // allocation fails.
    static std::shared_ptr<MuPDFRoot> create();

    // The root `ctx` belongs to, or empty if `ctx` is null or does not lock
    // through a litepdf table (a context created by the CLI, a test, or MuPDF
    // itself). PRECONDITION: that root must still be alive -- this reads
    // `ctx->locks.user` before taking any reference. See
    // EscrowContext::clone_from.
    static std::shared_ptr<MuPDFRoot> of(fz_context* ctx) noexcept;
};

}  // namespace litepdf::core::detail
