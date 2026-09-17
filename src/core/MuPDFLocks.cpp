#include "core/MuPDFLocks.hpp"

#include "core/EscrowContext.hpp"  // declares detail::live_lock_tables

#include <atomic>
#include <cstddef>

namespace litepdf::core::detail {

namespace {

std::atomic<std::size_t> g_live_tables{0};

void litepdf_lock(void* user, int lock) {
    static_cast<MuPDFLocks*>(user)->mutexes[static_cast<std::size_t>(lock)].lock();
}

void litepdf_unlock(void* user, int lock) {
    static_cast<MuPDFLocks*>(user)->mutexes[static_cast<std::size_t>(lock)].unlock();
}

}  // namespace

MuPDFLocks::MuPDFLocks() {
    g_live_tables.fetch_add(1, std::memory_order_relaxed);
}

MuPDFLocks::~MuPDFLocks() {
    g_live_tables.fetch_sub(1, std::memory_order_relaxed);
}

std::shared_ptr<MuPDFLocks> MuPDFLocks::create() {
    auto table = std::make_shared<MuPDFLocks>();
    table->fz.user   = table.get();
    table->fz.lock   = &litepdf_lock;
    table->fz.unlock = &litepdf_unlock;
    return table;
}

std::shared_ptr<MuPDFLocks> MuPDFLocks::of(fz_context* ctx) noexcept {
    // Recognise our table by its callback before trusting `user`: a context made
    // anywhere else may carry a different lock implementation, or none.
    if (!ctx || ctx->locks.lock != &litepdf_lock || !ctx->locks.user) return {};
    return static_cast<MuPDFLocks*>(ctx->locks.user)->weak_from_this().lock();
}

std::size_t live_lock_tables() noexcept {
    return g_live_tables.load(std::memory_order_relaxed);
}

}  // namespace litepdf::core::detail
