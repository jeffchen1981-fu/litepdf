#include "core/RenderEngine.hpp"
#include "core/Document.hpp"

extern "C" {
    void fz_drop_context(fz_context*);
}

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace litepdf::core {

struct RenderEngine::Impl {
    std::size_t num_workers = 0;
    std::vector<fz_context*> worker_ctxs;
    std::vector<std::thread> workers;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopping = false;

    static void worker_loop(Impl* impl, std::size_t /*idx*/) {
        for (;;) {
            std::unique_lock<std::mutex> lk(impl->mtx);
            impl->cv.wait(lk, [impl]{ return impl->stopping; });
            // Predicate currently only fires on stopping; Task 4 adds queue-push wakeups.
            if (impl->stopping) return;
        }
    }
};

RenderEngine::RenderEngine(Document& doc, std::size_t n)
    : impl_(std::make_unique<Impl>()) {
    if (n == 0)
        throw std::invalid_argument("RenderEngine: num_workers must be >= 1");

    impl_->num_workers = n;
    impl_->worker_ctxs.reserve(n);

    // Clone all contexts first; clean up on failure.
    try {
        for (std::size_t i = 0; i < n; ++i) {
            fz_context* ctx = doc.clone_context();
            if (!ctx) {
                throw std::runtime_error("RenderEngine: failed to clone fz_context "
                                         "(is the Document open?)");
            }
            impl_->worker_ctxs.push_back(ctx);
        }
    } catch (...) {
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
        impl_->worker_ctxs.clear();
        throw;
    }

    // Spawn workers only after all contexts are ready.
    impl_->workers.reserve(n);
    try {
        for (std::size_t i = 0; i < n; ++i) {
            impl_->workers.emplace_back(&Impl::worker_loop, impl_.get(), i);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->stopping = true;
        }
        impl_->cv.notify_all();
        for (auto& t : impl_->workers) {
            if (t.joinable()) t.join();
        }
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
        impl_->worker_ctxs.clear();
        throw;
    }
}

RenderEngine::~RenderEngine() {
    if (impl_) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->stopping = true;
        }
        impl_->cv.notify_all();
        for (auto& t : impl_->workers) {
            if (t.joinable()) t.join();
        }
        // Contexts dropped AFTER all threads join.
        for (auto* c : impl_->worker_ctxs) fz_drop_context(c);
    }
}

RenderEngine::RenderToken RenderEngine::submit(RenderRequest /*req*/) { return {}; }
void RenderEngine::cancel(const RenderToken&) {}
void RenderEngine::cancel_all_below_priority(int) {}
std::size_t RenderEngine::num_workers() const noexcept { return impl_->num_workers; }
std::size_t RenderEngine::pending_count() const noexcept { return 0; }

} // namespace litepdf::core
