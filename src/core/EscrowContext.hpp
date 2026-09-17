#pragma once

// core::EscrowContext -- a cloned fz_context that stays safe to drop after the
// Document it was cloned from has been destroyed.
//
// A bare fz_clone_context is NOT that. The clone carries a raw pointer to the
// Document's MuPDF lock table, and every fz_keep_* / fz_drop_* -- including the
// clone's own fz_drop_context -- locks through it. MuPDF keeps its master context
// alive until the last clone dies, but it knows nothing about our table (#61).
// An EscrowContext holds a strong reference to that table for its whole life and
// releases it LAST, after the context has been dropped.
//
// Contract for anything freed through get() -- a pixmap, an stext page: free it
// BEFORE this object is destroyed or assigned over. The destructor drops only the
// context.
//
// Header stays MuPDF-free (PIMPL discipline): the lock table is held type-erased.

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
    // lock table `source` locks through must still be alive for the duration of
    // this call -- i.e. its Document, or some EscrowContext of the same family,
    // is alive. `source` merely being a live pointer is NOT enough: recovering
    // the table reads `source`'s lock-callback `user` pointer before any
    // reference is taken, so cloning from a bare clone whose Document has died
    // is exactly the #61 use-after-free, one call earlier. Every caller today
    // runs while the Document is alive (Document::text_page under doc_mutex; the
    // render callback on a RenderEngine worker, which the Document outlives).
    // Empty on a null source, a context that does not lock through a litepdf
    // lock table, or a failed clone (out of memory). Thread-safe.
    [[nodiscard]] static EscrowContext clone_from(fz_context* source) noexcept;

    [[nodiscard]] bool        valid() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] fz_context* get()   const noexcept { return ctx_; }

private:
    // Drop the context, THEN release the table. fz_drop_context takes
    // FZ_LOCK_ALLOC through the table, so the order is the whole point.
    void reset() noexcept;

    std::shared_ptr<void> locks_;
    fz_context*           ctx_ = nullptr;
};

namespace detail {

// Number of litepdf MuPDF lock tables alive in the process. Test observability
// for #61 only -- nothing in the product reads it.
[[nodiscard]] std::size_t live_lock_tables() noexcept;

}  // namespace detail

}  // namespace litepdf::core
