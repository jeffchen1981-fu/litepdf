// #61 / #66: MuPDFRoot::of trusts a context's lock `user` pointer only when the
// context locks through litepdf's own callbacks.
//
// This file includes <mupdf/fitz.h>, so it reaches MuPDFRoot directly instead of
// declaring fz_* functions locally as test_escrow_context.cpp does.
#include "core/EscrowContext.hpp"
#include "core/MuPDFRoot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>

using litepdf::core::EscrowContext;
using litepdf::core::detail::MuPDFRoot;

namespace {

// A lock implementation that is not litepdf's: it ignores `user` entirely.
std::array<std::mutex, FZ_LOCK_MAX> g_foreign_mutexes;

void foreign_lock(void*, int lock) {
    g_foreign_mutexes[static_cast<std::size_t>(lock)].lock();
}

void foreign_unlock(void*, int lock) {
    g_foreign_mutexes[static_cast<std::size_t>(lock)].unlock();
}

}  // namespace

TEST_CASE("MuPDFRoot of rejects a context that does not lock through a litepdf table",
          "[core][escrow]") {
    const std::shared_ptr<MuPDFRoot> root = MuPDFRoot::create();
    REQUIRE(MuPDFRoot::of(root->ctx) == root);

    // `user` points at a LIVE MuPDFRoot, but the callbacks are someone else's.
    // Only the callback check can tell: a guard that trusted any non-null
    // `user` would hand back `root` here -- and for a real foreign context it
    // would cast whatever that caller's `user` is to a MuPDFRoot.
    fz_locks_context foreign{};
    foreign.user   = root.get();
    foreign.lock   = &foreign_lock;
    foreign.unlock = &foreign_unlock;
    fz_context* ctx = fz_new_context(nullptr, &foreign, FZ_STORE_DEFAULT);
    REQUIRE(ctx != nullptr);

    // CHECK, not REQUIRE: ctx must be dropped whatever the outcome.
    CHECK_FALSE(MuPDFRoot::of(ctx));
    {
        const EscrowContext escrow = EscrowContext::clone_from(ctx);
        CHECK_FALSE(escrow.valid());
    }
    fz_drop_context(ctx);
}
