#pragma once

// core::EscrowContext -- a cloned fz_context that stays safe to drop after the
// Document it was cloned from has been destroyed.
//
// A bare fz_clone_context is NOT that. The clone carries a raw pointer to the
// Document's MuPDF lock table, and every fz_keep_* / fz_drop_* -- including the
// clone's own fz_drop_context -- locks through it. It also shares the colorspace
// context, which MuPDF tears down in whichever context of the family dies last:
// that teardown frees the ICC profiles through the context that created them,
// the root the Document cloned from. MuPDF keeps its master context alive until
// the last clone dies, but it knows nothing about either of ours (#61).
// An EscrowContext holds a strong reference to the lock table AND the root
// context for its whole life, and releases them LAST, after it has dropped its
// own context.
//
// Contract for anything freed through get() -- a pixmap, an stext page: free it
// BEFORE this object is destroyed or assigned over. The destructor drops only the
// context.
//
// Header stays MuPDF-free (PIMPL discipline): the root is held type-erased.

#include <cstddef>
#include <memory>

struct fz_context;

namespace litepdf::core {

class EscrowContext {
public:
    EscrowContext() noexcept = default;
    ~EscrowContext();

    EscrowContext(EscrowContext&& other) noexcept;
    EscrowContext& operator=(EscrowContext&& other) noexcept;
    EscrowContext(const EscrowContext&)            = delete;
    EscrowContext& operator=(const EscrowContext&) = delete;

    // Clone `source`, a Document's context or a clone of one. PRECONDITION: the
    // root `source` locks through must still be alive for the duration of this
    // call -- i.e. its Document, or some EscrowContext of the same family, is
    // alive. `source` merely being a live pointer is NOT enough: recovering the
    // root reads `source`'s lock-callback `user` pointer before any reference is
    // taken, so cloning from a bare clone whose Document has died is exactly the
    // #61 use-after-free, one call earlier. Every caller today runs while the
    // Document is alive: the render callback on a RenderEngine worker, which the
    // Document outlives.
    // Empty on a null source, a context that does not lock through a litepdf
    // lock table, or a failed clone (out of memory). Thread-safe.
    [[nodiscard]] static EscrowContext clone_from(fz_context* source) noexcept;

    [[nodiscard]] bool        valid() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] fz_context* get()   const noexcept { return ctx_; }

private:
    // Drop the context, THEN release the root. fz_drop_context takes
    // FZ_LOCK_ALLOC through the root's lock table, and the root context must
    // outlive every clone of it, so the order is the whole point.
    void reset() noexcept;

    std::shared_ptr<void> root_;
    fz_context*           ctx_ = nullptr;
};

namespace detail {

// Number of litepdf MuPDF roots (lock table + root context) alive in the
// process. Test observability for #61 only -- nothing in the product reads it.
[[nodiscard]] std::size_t live_mupdf_roots() noexcept;

// Monotonic count of root contexts dropped since the process started. Test
// observability for #61 only -- nothing in the product reads it.
[[nodiscard]] std::size_t root_contexts_dropped() noexcept;

// Number of fz_contexts alive in the family `ctx` belongs to -- the root plus
// every clone of it -- read from MuPDF's own master bookkeeping. Test
// observability for the root-context lifetime only; nothing in the product
// reads it. The read is unsynchronised, so it is meaningful only while no other
// thread is cloning or dropping a context of the same family.
[[nodiscard]] int family_context_count(fz_context* ctx) noexcept;

}  // namespace detail

}  // namespace litepdf::core
