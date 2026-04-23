#include "app/CrossTabSearch.hpp"

#include "core/DocumentView.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace litepdf::app {

// ---------------------------------------------------------------------------
// Impl
//
// Per-dispatch state:
//   * `sentinel` — a shared_ptr<void> whose lifetime defines the current
//     dispatch's liveness. Every Hit holds a weak_ptr<void> to it. When
//     clear() or the next dispatch() runs, we drop this shared_ptr, so
//     all hits from the superseded dispatch observe an expired weak_ptr
//     on .lock(). This gives row-click handlers a cheap "is this result
//     from a still-live dispatch?" check.
//
//   * `tabs_snapshot` — the frozen list of TabRefs handed to dispatch().
//     Tabs opened after dispatch() are not joined (§D9).
//
//   * `observers` — per-tab record of (view, previous_observer) so we
//     can chain the caller's observer ahead of our aggregation callback.
//     We do not restore the previous observer on clear(); MainWindow is
//     expected to re-install its own on the next tab switch or Ctrl+F.
//     Restoring would require gating against concurrent worker callbacks
//     mid-scan, which is more complexity than Phase 6.2 needs.
// ---------------------------------------------------------------------------
struct CrossTabSearch::Impl {
    mutable std::mutex                  m;
    std::vector<Hit>                    hits;
    OnUpdate                            on_update;

    // Bumped by dispatch() and clear() to invalidate in-flight
    // aggregation callbacks from the previous generation. The sentinel
    // is the user-visible half of the same invariant.
    std::shared_ptr<void>               sentinel;

    // One entry per TabRef in the current snapshot. `prev_observer`
    // is whatever was installed on the SearchSession at dispatch() time;
    // our chained observer calls it first, then our aggregator, then
    // CrossTabSearch::on_update.
    struct TabRec {
        litepdf::core::DocumentView*         view;
        std::wstring                         label;
        litepdf::core::SearchSession::OnUpdate prev_observer;
    };
    std::vector<TabRec>                 tabs;
};

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------
CrossTabSearch::CrossTabSearch() : impl_(std::make_unique<Impl>()) {}
CrossTabSearch::~CrossTabSearch() = default;

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------
void CrossTabSearch::dispatch(std::wstring                        query,
                              litepdf::core::SearchSession::Flags flags,
                              std::vector<TabRef>                 tabs) {
    // Phase 1: drop old state under lock. Dropping the old sentinel
    // here expires every Hit from the previous dispatch, even ones
    // still in `impl_->hits` we're about to clear. This is the epoch
    // bump — in-flight aggregation lambdas from the prior generation
    // look up a weak_ptr to this sentinel and bail when it's expired.
    std::shared_ptr<void> new_sentinel = std::make_shared<int>(0);
    std::weak_ptr<void>   weak_sentinel = new_sentinel;

    std::vector<Impl::TabRec> new_recs;
    new_recs.reserve(tabs.size());
    for (const auto& t : tabs) {
        if (!t.view) continue;
        auto& sess = t.view->search();
        new_recs.push_back(Impl::TabRec{
            t.view, t.label, sess.on_update()  // capture whatever MainWindow set
        });
    }

    {
        std::lock_guard<std::mutex> g(impl_->m);
        impl_->hits.clear();
        impl_->sentinel = new_sentinel;
        impl_->tabs     = std::move(new_recs);
    }

    // Phase 2: install chained observer on each captured session and
    // kick off the search. We snapshot what we need by value into the
    // lambda so nothing in the lambda body touches `impl_->tabs` (which
    // could be mutated by a subsequent dispatch() / clear()).
    std::vector<Impl::TabRec> snapshot_copy;
    {
        std::lock_guard<std::mutex> g(impl_->m);
        snapshot_copy = impl_->tabs;  // shallow copy: pointers + labels + prev cb
    }

    for (const auto& rec : snapshot_copy) {
        auto* view  = rec.view;
        auto* sess  = &view->search();
        auto  label = rec.label;
        auto  prev  = rec.prev_observer;

        // Capture `this` + weak_sentinel so the aggregator can detect a
        // superseded dispatch and return without touching impl_.
        sess->set_on_update([this, weak_sentinel, view, label, prev, sess]() {
            // Chain: let MainWindow's observer fire first so find-bar
            // counters keep refreshing.
            if (prev) prev();

            // Epoch check: if our dispatch has been superseded, our
            // sentinel is gone and we must not touch impl_ (which may
            // belong to a newer generation's state).
            auto live = weak_sentinel.lock();
            if (!live) return;

            // Re-read this session's full hit list. Simpler than
            // incremental merge; for <1000 hits across tabs this is
            // fine. We collect per-page to amortize mutex acquisitions
            // on the session side — one hits_for_page per page is O(N)
            // in session hits anyway.
            const std::size_t pages = view->page_count() > 0
                ? static_cast<std::size_t>(view->page_count()) : 0;
            std::vector<Hit> fresh;
            for (std::size_t p = 0; p < pages; ++p) {
                auto page_hits = sess->hits_for_page(p);
                for (auto& h : page_hits) {
                    Hit out;
                    out.file_label     = label;
                    out.page           = h.page;
                    out.geom           = std::move(h.geom);
                    out.session_state  = weak_sentinel;
                    out.view_at_submit = view;
                    fresh.push_back(std::move(out));
                }
            }

            OnUpdate cb;
            {
                std::lock_guard<std::mutex> g(impl_->m);
                // Re-check epoch under lock; the sentinel might have
                // been dropped between our weak_sentinel.lock() above
                // and acquiring the mutex.
                if (impl_->sentinel.get() != live.get()) return;

                // Clear this view's hits, re-add the freshly-read set.
                // The session's hit list only grows within a generation,
                // so re-adding yields no duplicates.
                impl_->hits.erase(
                    std::remove_if(impl_->hits.begin(), impl_->hits.end(),
                        [view](const Hit& h) { return h.view_at_submit == view; }),
                    impl_->hits.end());
                impl_->hits.insert(impl_->hits.end(),
                                   std::make_move_iterator(fresh.begin()),
                                   std::make_move_iterator(fresh.end()));
                cb = impl_->on_update;
            }
            if (cb) cb();
        });

        sess->set_query(query, flags);
    }
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------
void CrossTabSearch::clear() {
    std::vector<Impl::TabRec> to_detach;
    {
        std::lock_guard<std::mutex> g(impl_->m);
        impl_->hits.clear();
        impl_->sentinel.reset();          // expire all outstanding weak_ptrs
        to_detach = std::move(impl_->tabs);
        impl_->tabs.clear();
    }

    // Drop our observers. MainWindow is expected to reinstall its own
    // on the next tab switch or Ctrl+F. We do NOT attempt to restore
    // prev_observer because concurrent worker callbacks could race with
    // the restore; the simpler contract is "clear() detaches".
    for (const auto& rec : to_detach) {
        if (rec.view) {
            rec.view->search().set_on_update(litepdf::core::SearchSession::OnUpdate{});
        }
    }
}

// ---------------------------------------------------------------------------
// observer / accessors
// ---------------------------------------------------------------------------
void CrossTabSearch::set_on_update(OnUpdate cb) {
    std::lock_guard<std::mutex> g(impl_->m);
    impl_->on_update = std::move(cb);
}

std::vector<CrossTabSearch::Hit> CrossTabSearch::hits() const {
    std::lock_guard<std::mutex> g(impl_->m);
    return impl_->hits;
}

std::size_t CrossTabSearch::hit_count() const {
    std::lock_guard<std::mutex> g(impl_->m);
    return impl_->hits.size();
}

}  // namespace litepdf::app
