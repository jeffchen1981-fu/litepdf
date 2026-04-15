#include "core/RenderEngine.hpp"
#include "core/Document.hpp"

extern "C" {
    void fz_drop_context(fz_context*);
}

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace litepdf::core {

struct RenderEngine::Impl {
    std::size_t num_workers = 0;
    std::vector<fz_context*> worker_ctxs;
    std::vector<std::thread> workers;
    mutable std::mutex mtx;  // mutable: pending_count() is const but locks
    std::condition_variable cv;
    bool stopping = false;
    std::queue<RenderRequest> q;
    std::atomic<std::size_t> next_id{1};

    // Graceful shutdown: predicate wakes on stopping || !q.empty(); we only
    // return when stopping AND queue is empty, so in-flight queued requests
    // get drained by remaining workers on dtor. Real rendering (Task 7) may
    // want a hard-cancel path, but for Phase 2 drain-then-stop is correct.
    static void worker_loop(Impl* impl, std::size_t idx) {
        for (;;) {
            RenderRequest req;
            {
                std::unique_lock<std::mutex> lk(impl->mtx);
                impl->cv.wait(lk, [impl]{ return impl->stopping || !impl->q.empty(); });
                if (impl->stopping && impl->q.empty()) return;
                req = std::move(impl->q.front());
                impl->q.pop();
            }
            // Run user callback OUTSIDE the lock to avoid deadlocks if they
            // call back into the engine (e.g. submit from within on_complete).
            if (req.on_complete) {
                req.on_complete(nullptr, impl->worker_ctxs[idx]);
            }
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

RenderEngine::RenderToken RenderEngine::submit(RenderRequest req) {
    RenderToken tok;
    tok.id = impl_->next_id.fetch_add(1);
    tok.canceled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->q.push(std::move(req));
    }
    impl_->cv.notify_one();
    return tok;
}

void RenderEngine::cancel(const RenderToken&) {}
void RenderEngine::cancel_all_below_priority(int) {}
std::size_t RenderEngine::num_workers() const noexcept { return impl_->num_workers; }

std::size_t RenderEngine::pending_count() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->q.size();
}

} // namespace litepdf::core
