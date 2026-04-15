#include "core/RenderEngine.hpp"

namespace litepdf::core {

struct RenderEngine::Impl {
    std::size_t num_workers;
};

RenderEngine::RenderEngine(Document& /*doc*/, std::size_t num_workers)
    : impl_(std::make_unique<Impl>(Impl{num_workers})) {}

RenderEngine::~RenderEngine() = default;

RenderEngine::RenderToken RenderEngine::submit(RenderRequest /*req*/) {
    return {};
}

void RenderEngine::cancel(const RenderToken&) {}
void RenderEngine::cancel_all_below_priority(int) {}

std::size_t RenderEngine::num_workers() const noexcept { return impl_->num_workers; }
std::size_t RenderEngine::pending_count() const noexcept { return 0; }

} // namespace litepdf::core
