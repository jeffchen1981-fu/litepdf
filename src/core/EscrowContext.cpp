#include "core/EscrowContext.hpp"

#include "core/MuPDFRoot.hpp"

#include <utility>

namespace litepdf::core {

EscrowContext::~EscrowContext() {
    reset();
}

EscrowContext::EscrowContext(EscrowContext&& other) noexcept
    : root_(std::move(other.root_)),
      ctx_(std::exchange(other.ctx_, nullptr)) {}

EscrowContext& EscrowContext::operator=(EscrowContext&& other) noexcept {
    if (this != &other) {
        reset();
        root_ = std::move(other.root_);
        ctx_  = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

void EscrowContext::reset() noexcept {
    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
    root_.reset();
}

EscrowContext EscrowContext::clone_from(fz_context* source) noexcept {
    EscrowContext out;
    std::shared_ptr<detail::MuPDFRoot> root = detail::MuPDFRoot::of(source);
    if (!root) return out;
    fz_context* clone = fz_clone_context(source);
    if (!clone) return out;
    out.root_ = std::move(root);
    out.ctx_  = clone;
    return out;
}

}  // namespace litepdf::core
