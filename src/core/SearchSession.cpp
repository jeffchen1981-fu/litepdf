#include "core/SearchSession.hpp"

#include "core/GenerationAbort.hpp"

#include <windows.h>  // WideCharToMultiByte for UTF-16 -> UTF-8

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

namespace litepdf::core {

// ---------------------------------------------------------------------------
// Impl: shared state between the SearchSession and the task lambdas.
//
// State is nested inside Impl so the .hpp stays free of <atomic>, <mutex>,
// and other implementation detail. Task lambdas capture a strong
// shared_ptr<State> (keeping State alive past session destruction) but
// the dispatcher's Task::state_weak holds the weak copy that governs
// runnability.
// ---------------------------------------------------------------------------
struct SearchSession::Impl {
    struct State {
        mutable std::mutex       m;
        std::wstring             query;
        Flags                    flags;
        // Atomic so the task lambda can check/reject stale runs without
        // taking the mutex in the fast path.
        std::atomic<std::uint64_t> epoch{0};
        // Live counter of task lambdas that have been submitted but whose
        // run() body has not yet returned. Incremented at submit time in
        // set_query() and decremented by an RAII scope guard at the top
        // of each task lambda so every early-return path (stale-epoch
        // skip, MuPDF throw, normal completion) decrements exactly once.
        // ~SearchSession spins on this counter so no worker is ever
        // mid-call on doc_ref after the session dtor returns — which
        // closes the critical UAF window where SearchSession destructs
        // before Document in DocumentView::Impl's member order.
        std::atomic<int>         pending_tasks{0};
        std::vector<Hit>         hits;
        std::size_t              cursor = 0;
        std::size_t              pages_remaining = 0;
        OnUpdate                 on_update;
    };

    const Document&                         doc;
    litepdf::app::ISearchDispatcher&        dispatcher;
    std::shared_ptr<State>                  state;
    // Per-generation abort tokens. Mutated only on the UI thread (set_query /
    // dtor); each generation's tasks capture their own token by value, so
    // workers never touch this object. See GenerationAbort.hpp.
    GenerationAbort                         gen;

    Impl(const Document& d, litepdf::app::ISearchDispatcher& disp)
        : doc(d), dispatcher(disp), state(std::make_shared<State>()) {}
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

std::string utf16_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                          static_cast<int>(ws.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------
SearchSession::SearchSession(const Document& doc,
                             litepdf::app::ISearchDispatcher& disp)
    : impl_(std::make_unique<Impl>(doc, disp)) {}

SearchSession::~SearchSession() {
    // Drain any in-flight task before letting Document (which outlives
    // this session only briefly — it's the LAST member of
    // DocumentView::Impl) go out of scope under the caller's destructor.
    //
    // Step 1: bump epoch so queued-but-not-yet-running tasks observe the
    // new generation and return without calling doc_ref.page_hits().
    // They still pay one scope-guard decrement each, which is what lets
    // the drain loop below terminate.
    auto& st = *impl_->state;
    st.epoch.fetch_add(1, std::memory_order_acq_rel);
    // Also abort the active generation's token so any task already inside
    // page_hits() bails mid-page (its incremental search loop polls the
    // token each iteration) instead of scanning the whole page first.
    impl_->gen.abort_active();

    // Step 2: spin-wait until every submitted task has executed its
    // scope guard. Tasks that haven't run yet will be popped off the
    // dispatcher queue and — seeing the bumped epoch — return
    // immediately, decrementing the counter inside their guard's dtor.
    //
    // Worst case: all N page tasks are sequenced behind a higher-priority
    // batch on a 2-worker pool; they still burn down in O(N / workers)
    // epoch-checks. In practice this is microseconds.
    while (st.pending_tasks.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

// ---------------------------------------------------------------------------
// set_query / clear
// ---------------------------------------------------------------------------
void SearchSession::set_query(std::wstring q, Flags f) {
    auto& st = *impl_->state;

    {
        std::lock_guard<std::mutex> g(st.m);
        st.query           = std::move(q);
        st.flags           = f;
        st.hits.clear();
        st.cursor          = 0;
        st.pages_remaining = 0;
    }

    // Bump the epoch AFTER clearing so in-flight tasks observe the new
    // value and skip appending to the now-empty hit list.
    const std::uint64_t ep =
        st.epoch.fetch_add(1, std::memory_order_acq_rel) + 1;

    // Start a new abort generation: this flips the PREVIOUS generation's
    // token so a worker still inside page_hits() for the old query bails
    // mid-page, and mints a fresh token (value 0) that this generation's
    // tasks capture. (A single shared flag couldn't do this — resetting it
    // to 0 here would strand the old in-flight worker.) Empty-query clears
    // also abort prior in-flight work, which is the desired behavior.
    std::shared_ptr<std::atomic<int>> abort_token = impl_->gen.begin_generation();

    // Empty query: treat as "cleared" — no tasks, scan_complete=true
    // immediately (pages_remaining already zeroed above).
    OnUpdate cb_snapshot;
    {
        std::lock_guard<std::mutex> g(st.m);
        cb_snapshot = st.on_update;
    }
    if (impl_->state->query.empty()) {
        if (cb_snapshot) cb_snapshot();
        return;
    }

    const std::size_t pages = impl_->doc.page_count();
    {
        std::lock_guard<std::mutex> g(st.m);
        st.pages_remaining = pages;
    }

    // If the document has no pages (unusual but not impossible), the
    // loop below is a no-op — just notify and return.
    if (pages == 0) {
        if (cb_snapshot) cb_snapshot();
        return;
    }

    // Capture a strong handle to State in each task's run lambda so the
    // state outlives the SearchSession in edge cases where a worker
    // thread is mid-execution when the session is destroyed. The
    // dispatcher itself holds a weak_ptr for the runnability gate.
    std::shared_ptr<Impl::State> state_strong = impl_->state;
    std::weak_ptr<Impl::State>   state_weak   = impl_->state;
    const Document&              doc_ref      = impl_->doc;
    const std::string            needle_utf8  = utf16_to_utf8(state_strong->query);
    const Flags                  captured_flags = f;

    // Arm the drain counter BEFORE submitting. fetch_add (not store)
    // because a prior generation's tasks may still be in-flight — they
    // decrement via their own scope guards, so we must not clobber their
    // contribution by overwriting. The counter transiently reflects
    // "sum of in-flight across all generations", which is exactly what
    // ~SearchSession wants to drain down to zero.
    st.pending_tasks.fetch_add(static_cast<int>(pages),
                               std::memory_order_acq_rel);

    // Priority: all tasks at P2 in v1. Phase 6.x optimization will use
    // set_reference_page() to bump nearby pages to P0/P1.
    for (std::size_t i = 0; i < pages; ++i) {
        litepdf::app::ISearchDispatcher::Task t{};
        t.state_weak = std::weak_ptr<void>(
            std::static_pointer_cast<void>(state_strong));
        t.priority = 2;
        t.run = [state_strong, &doc_ref, i, needle_utf8, captured_flags, ep,
                 abort_token]() {
            // RAII drain guard: always decrement pending_tasks on return,
            // whether we hit the stale-epoch fast exit below, MuPDF
            // throws out of page_hits, or we complete normally. This is
            // what makes ~SearchSession's spin-wait terminate.
            struct Guard {
                std::shared_ptr<Impl::State> s;
                ~Guard() {
                    s->pending_tasks.fetch_sub(1, std::memory_order_acq_rel);
                }
            } guard{state_strong};

            // Epoch check #1: caller may have bumped state->epoch after
            // we were submitted but before we started running.
            if (state_strong->epoch.load(std::memory_order_acquire) != ep) return;

            Document::SearchFlags df{ captured_flags.match_case,
                                      captured_flags.whole_word,
                                      captured_flags.regex };
            auto raw_hits = doc_ref.page_hits(i, needle_utf8, df,
                                              abort_token.get());

            // Epoch check #2: we were busy in MuPDF; the user may have
            // typed more characters in the meantime. Drop stale results.
            OnUpdate cb;
            {
                std::lock_guard<std::mutex> g(state_strong->m);
                if (state_strong->epoch.load(std::memory_order_acquire) != ep) {
                    return;
                }
                state_strong->hits.reserve(state_strong->hits.size()
                                           + raw_hits.size());
                for (auto& ph : raw_hits) {
                    state_strong->hits.push_back(Hit{i, std::move(ph)});
                }
                // Keep hits sorted by (page, top-to-bottom). std::sort
                // on the growing vector is O(n log n) per insert but n
                // is typically in the tens for a human-typed query;
                // the simpler invariant beats a fancier merge here.
                std::sort(state_strong->hits.begin(), state_strong->hits.end(),
                    [](const Hit& a, const Hit& b) {
                        if (a.page != b.page) return a.page < b.page;
                        return a.geom.ul_y < b.geom.ul_y;
                    });
                if (state_strong->pages_remaining > 0) {
                    --state_strong->pages_remaining;
                }
                cb = state_strong->on_update;
            }
            if (cb) cb();
        };
        impl_->dispatcher.submit(std::move(t));
    }
}

void SearchSession::clear() {
    set_query(L"", {});
}

bool SearchSession::query_compiles(const std::wstring& q, Flags f) const {
    const std::string utf8 = utf16_to_utf8(q);
    Document::SearchFlags df{ f.match_case, f.whole_word, f.regex };
    return impl_->doc.query_compiles(utf8, df);
}

// ---------------------------------------------------------------------------
// Observer / cursor
// ---------------------------------------------------------------------------
void SearchSession::set_on_update(OnUpdate cb) {
    std::lock_guard<std::mutex> g(impl_->state->m);
    impl_->state->on_update = std::move(cb);
}

SearchSession::OnUpdate SearchSession::on_update() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    return impl_->state->on_update;
}

void SearchSession::set_reference_page(std::size_t page) {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) {
        st.cursor = 0;
        return;
    }
    // Hits are sorted by (page, ul_y). First hit at-or-after `page`:
    auto it = std::lower_bound(st.hits.begin(), st.hits.end(), page,
        [](const Hit& h, std::size_t p) { return h.page < p; });
    if (it == st.hits.end()) it = st.hits.begin();  // wrap
    st.cursor = static_cast<std::size_t>(std::distance(st.hits.begin(), it));
}

std::optional<SearchSession::Hit> SearchSession::current() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    if (impl_->state->hits.empty()) return std::nullopt;
    return impl_->state->hits[impl_->state->cursor];
}

std::optional<SearchSession::Hit> SearchSession::next() {
    std::lock_guard<std::mutex> g(impl_->state->m);
    if (impl_->state->hits.empty()) return std::nullopt;
    auto& c = impl_->state->cursor;
    const auto n = impl_->state->hits.size();
    c = (c + 1) % n;
    return impl_->state->hits[c];
}

std::optional<SearchSession::Hit> SearchSession::prev() {
    std::lock_guard<std::mutex> g(impl_->state->m);
    if (impl_->state->hits.empty()) return std::nullopt;
    auto& c = impl_->state->cursor;
    const auto n = impl_->state->hits.size();
    c = (c + n - 1) % n;
    return impl_->state->hits[c];
}

std::size_t SearchSession::hit_count() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    return impl_->state->hits.size();
}

bool SearchSession::scan_complete() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    return impl_->state->pages_remaining == 0;
}

std::vector<SearchSession::Hit>
SearchSession::hits_for_page(std::size_t page) const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    std::vector<Hit> out;
    for (const auto& h : impl_->state->hits) {
        if (h.page == page) out.push_back(h);
    }
    return out;
}

}  // namespace litepdf::core
