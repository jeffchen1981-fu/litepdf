#include "core/EscrowContext.hpp"

#include "core/MuPDFLocks.hpp"

#include <utility>

namespace litepdf::core {

EscrowContext::~EscrowContext() {
    reset();
}

EscrowContext::EscrowContext(EscrowContext&& other) noexcept
    : locks_(std::move(other.locks_)),
      ctx_(std::exchange(other.ctx_, nullptr)) {}

EscrowContext& EscrowContext::operator=(EscrowContext&& other) noexcept {
    if (this != &other) {
        reset();
        locks_ = std::move(other.locks_);
        ctx_   = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

void EscrowContext::reset() noexcept {
    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
    locks_.reset();
}

EscrowContext EscrowContext::clone_from(fz_context* source) noexcept {
    EscrowContext out;
    std::shared_ptr<detail::MuPDFLocks> table = detail::MuPDFLocks::of(source);
    if (!table) return out;
    fz_context* clone = fz_clone_context(source);
    if (!clone) return out;
    out.locks_ = std::move(table);
    out.ctx_   = clone;
    return out;
}

}  // namespace litepdf::core
