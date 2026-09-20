#include "core/MuPDFRoot.hpp"

#include "core/EscrowContext.hpp"  // declares detail::live_mupdf_roots

#include <atomic>
#include <cstddef>
#include <new>

namespace litepdf::core::detail {

namespace {

std::atomic<std::size_t> g_live_roots{0};
std::atomic<std::size_t> g_roots_dropped{0};

void litepdf_lock(void* user, int lock) {
    static_cast<MuPDFRoot*>(user)->mutexes[static_cast<std::size_t>(lock)].lock();
}

void litepdf_unlock(void* user, int lock) {
    static_cast<MuPDFRoot*>(user)->mutexes[static_cast<std::size_t>(lock)].unlock();
}

}  // namespace

MuPDFRoot::MuPDFRoot() {
    g_live_roots.fetch_add(1, std::memory_order_relaxed);
}

MuPDFRoot::~MuPDFRoot() {
    // In the destructor BODY, not by a member with its own destructor: the
    // mutexes must still be alive here, because fz_drop_context takes
    // FZ_LOCK_ALLOC through them.
    if (ctx) {
        fz_drop_context(ctx);
        ctx = nullptr;
        g_roots_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_live_roots.fetch_sub(1, std::memory_order_relaxed);
}

std::shared_ptr<MuPDFRoot> MuPDFRoot::create() {
    auto root = std::make_shared<MuPDFRoot>();
    root->fz.user   = root.get();
    root->fz.lock   = &litepdf_lock;
    root->fz.unlock = &litepdf_unlock;
    root->ctx       = fz_new_context(nullptr, &root->fz, FZ_STORE_DEFAULT);
    if (!root->ctx) throw std::bad_alloc();
    return root;
}

std::shared_ptr<MuPDFRoot> MuPDFRoot::of(fz_context* ctx) noexcept {
    // Recognise our root by its callback before trusting `user`: a context made
    // anywhere else may carry a different lock implementation, or none.
    if (!ctx || ctx->locks.lock != &litepdf_lock || !ctx->locks.user) return {};
    return static_cast<MuPDFRoot*>(ctx->locks.user)->weak_from_this().lock();
}

std::size_t live_mupdf_roots() noexcept {
    return g_live_roots.load(std::memory_order_relaxed);
}

std::size_t root_contexts_dropped() noexcept {
    return g_roots_dropped.load(std::memory_order_relaxed);
}

int family_context_count(fz_context* ctx) noexcept {
    // fz_context::master points at the context the family was cloned from, and
    // only that master's context_count is maintained: fz_drop_context
    // decrements `ctx->master->context_count` on every drop. A master that has
    // already been dropped while clones remain is left with a null `master`
    // (the struct survives only to carry the count), so report 0 rather than
    // reading through it.
    if (!ctx || !ctx->master) return 0;
    return ctx->master->context_count;
}

}  // namespace litepdf::core::detail
