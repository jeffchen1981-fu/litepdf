#include "core/RenderEngine.hpp"
#include "core/Document.hpp"

extern "C" {
    void fz_drop_context(fz_context*);
}

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace litepdf::core {

struct RenderEngine::Impl {
    // Wraps a RenderRequest with stable-ordering metadata plus a
    // shared cancel flag copied from the returned RenderToken.
    // priority: lower value == more urgent (runs first).
    // seq: monotonically-increasing arrival tag; earlier seq wins on tie.
    // canceled: shared with the RenderToken; workers check it pre-callback.
    struct QueuedRequest {
        int priority;
        std::size_t seq;
        RenderRequest req;
        std::shared_ptr<std::atomic<bool>> canceled;
    };

    // Heap is a max-heap by default. We want the SMALLEST (priority, seq)
    // on top, so Compare returns true when `a` should sink — standard
    // invert-for-min-heap idiom, now applied against a vector-backed heap
    // (std::push_heap / std::pop_heap / std::make_heap) so we can iterate
    // the queue for cancel_all_below_priority().
    struct Compare {
        bool operator()(const QueuedRequest& a, const QueuedRequest& b) const {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.seq > b.seq;
        }
    };

    std::size_t num_workers = 0;
    std::vector<fz_context*> worker_ctxs;
    std::vector<std::thread> workers;
    mutable std::mutex mtx;  // mutable: pending_count() is const but locks
    std::condition_variable cv;
    bool stopping = false;
    // Heap-ordered vector (not std::priority_queue) — iteration required
    // by cancel_all_below_priority().
    std::vector<QueuedRequest> q;
    std::size_t next_seq = 1;  // guarded by mtx (submit holds it anyway)
    std::atomic<std::size_t> next_id{1};

    // Graceful shutdown: predicate wakes on stopping || !q.empty(); we only
    // return when stopping AND queue is empty, so in-flight queued requests
    // get drained by remaining workers on dtor. Real rendering (Task 7) may
    // want a hard-cancel path, but for Phase 2 drain-then-stop is correct.
    static void worker_loop(Impl* impl, std::size_t idx) {
        for (;;) {
            RenderRequest req;
            std::shared_ptr<std::atomic<bool>> canceled;
            {
                std::unique_lock<std::mutex> lk(impl->mtx);
                impl->cv.wait(lk, [impl]{ return impl->stopping || !impl->q.empty(); });
                if (impl->stopping && impl->q.empty()) return;
                // pop_heap moves the top element to the back; pop_back
                // destroys that slot. We move the payload out before
                // pop_back destroys the (now moved-from) element.
                std::pop_heap(impl->q.begin(), impl->q.end(), Compare{});
                auto entry = std::move(impl->q.back());
                impl->q.pop_back();
                req = std::move(entry.req);
                canceled = std::move(entry.canceled);
            }
            // Checkpoint A: pre-callback cancel check. Cancelation is
            // cooperative — entries stay in the heap after being flagged;
            // workers simply skip the callback when they reach them.
            if (canceled && canceled->load()) continue;
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

// Starvation note: a steady stream of high-priority submissions can
// indefinitely delay low-priority work. Acceptable for Phase 2 — UI
// submissions are bounded per paint cycle. Revisit if needed later.
RenderEngine::RenderToken RenderEngine::submit(RenderRequest req) {
    RenderToken tok;
    tok.id = impl_->next_id.fetch_add(1);
    tok.canceled = std::make_shared<std::atomic<bool>>(false);
    int prio = req.priority;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        std::size_t seq = impl_->next_seq++;
        impl_->q.push_back({prio, seq, std::move(req), tok.canceled});
        std::push_heap(impl_->q.begin(), impl_->q.end(), Impl::Compare{});
    }
    impl_->cv.notify_one();
    return tok;
}

void RenderEngine::cancel(const RenderToken& token) {
    // No lock required: the flag is a std::atomic, and the shared_ptr
    // keeps it alive for as long as either side holds a reference.
    if (token.canceled) token.canceled->store(true);
}

void RenderEngine::cancel_all_below_priority(int p) {
    // "Below priority p" means less urgent, i.e. priority value strictly
    // greater than p (lower value = more urgent in this engine).
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (auto& entry : impl_->q) {
        if (entry.priority > p && entry.canceled) {
            entry.canceled->store(true);
        }
    }
    // We do NOT remove entries from the heap — flagging is enough, and
    // leaving the heap untouched avoids any re-heapify cost.
}

std::size_t RenderEngine::num_workers() const noexcept { return impl_->num_workers; }

std::size_t RenderEngine::pending_count() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->q.size();
}

} // namespace litepdf::core
