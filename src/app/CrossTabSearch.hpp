#pragma once

// app::CrossTabSearch — cross-tab query orchestrator for Phase 6.2.
// Fans a query out to a snapshot of open tabs' SearchSessions, aggregates
// hits into a flat list for virtual ListView display.
//
// Snapshot semantics (design §D9): the tab list at dispatch time is
// frozen. Tabs opened later are NOT joined to the running scan.
// Tabs closed during scan have their weak_ptr expire; their already-
// accumulated hits remain in the result list but row-click resolves
// to "result is no longer available".
//
// Observer chaining: each SearchSession supports ONE on_update observer.
// MainWindow's find-bar installs one to refresh counters. dispatch()
// captures the existing observer via SearchSession::on_update(), chains
// it ahead of CrossTabSearch's own aggregation callback, and re-installs
// the chained pair. clear() restores the previously-captured observer
// on every tracked session, so the per-tab find-bar counter resumes
// updating the moment the cross-tab panel is dismissed (I1 fix).
// dispatch() itself calls clear() first to avoid stacking chained
// lambdas across repeated Ctrl+Shift+F invocations.

#include "core/Document.hpp"
#include "core/SearchSession.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace litepdf::core { class DocumentView; }

namespace litepdf::app {

class CrossTabSearch {
public:
    struct TabRef {
        litepdf::core::DocumentView* view;    // non-owning; must outlive submit()
        std::wstring                 label;   // for display (e.g. file basename)
    };

    struct Hit {
        std::wstring                     file_label;
        std::size_t                      page = 0;
        litepdf::core::Document::PageHit geom;
        // Liveness token: holds a weak reference to a per-dispatch sentinel
        // owned by CrossTabSearch. Expires when the owning dispatch is
        // cleared or superseded by the next dispatch. Row-click handlers
        // use .lock() to decide whether the result is still meaningful.
        std::weak_ptr<void>              session_state;
        // Non-owning back-pointer to the tab that produced this hit. May
        // dangle after the tab is closed — treat with the same caution
        // as the weak_ptr above; prefer checking .lock() first.
        litepdf::core::DocumentView*     view_at_submit = nullptr;
    };

    using OnUpdate = std::function<void()>;

    CrossTabSearch();
    ~CrossTabSearch();

    CrossTabSearch(const CrossTabSearch&)            = delete;
    CrossTabSearch& operator=(const CrossTabSearch&) = delete;

    // Fan-out query to all tabs in snapshot. Bumps an internal epoch;
    // any in-flight hits from prior dispatch are discarded. Observers
    // already installed on each tab's SearchSession are captured and
    // chained — they continue to fire during cross-tab scan.
    void dispatch(std::wstring                          query,
                  litepdf::core::SearchSession::Flags   flags,
                  std::vector<TabRef>                   tabs);

    // Clear all results, expire the dispatch epoch (so in-flight
    // aggregation callbacks become no-ops), and restore each captured
    // session's previous on_update observer. Safe to call on a fresh
    // instance or repeatedly.
    void clear();

    void set_on_update(OnUpdate cb);

    // Snapshot copy. Cheap for the Phase 6.2 target of <1000 hits.
    std::vector<Hit> hits() const;
    std::size_t      hit_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::app
