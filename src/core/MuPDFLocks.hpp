#pragma once

// INTERNAL to litepdf_core: includes <mupdf/fitz.h>. Include it only from .cpp
// files in src/core -- never from a public header.
//
// The lock table MuPDF calls through for every context in one Document's family:
// the Document's own context and every fz_clone_context of it. MuPDF requires a
// table at fz_new_context time before it will clone at all, because clones share
// the store, the font cache and the glyph cache and must serialise access to them.
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

struct MuPDFLocks : std::enable_shared_from_this<MuPDFLocks> {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context                    fz{};

    MuPDFLocks();
    ~MuPDFLocks();
    MuPDFLocks(const MuPDFLocks&)            = delete;
    MuPDFLocks& operator=(const MuPDFLocks&) = delete;

    // A new table with its callbacks installed and `fz.user` pointing at it,
    // ready to pass to fz_new_context.
    static std::shared_ptr<MuPDFLocks> create();

    // The table `ctx` locks through, or empty if `ctx` is null or does not lock
    // through a litepdf table (a context created by the CLI, a test, or MuPDF
    // itself). PRECONDITION: that table must still be alive -- this reads
    // `ctx->locks.user` before taking any reference. See
    // EscrowContext::clone_from.
    static std::shared_ptr<MuPDFLocks> of(fz_context* ctx) noexcept;
};

}  // namespace litepdf::core::detail
