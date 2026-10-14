#include "core/SearchSession.hpp"

#include "core/EscrowContext.hpp"
#include "core/GenerationAbort.hpp"
#include "core/detail/OpenFailure.hpp"
#include "core/detail/PageHitsAccess.hpp"

#include <mupdf/fitz.h>

#include <windows.h>  // WideCharToMultiByte, SecureZeroMemory

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
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
        // A task the dispatcher discards never runs; set_query() and
        // ~SearchSession subtract the count discard() returns instead.
        // ~SearchSession spins on this counter so no worker is still
        // using the session's own document, context or scan mutex (all
        // in Impl) when the dtor drops them.
        std::atomic<int>         pending_tasks{0};
        std::vector<Hit>         hits;
        std::size_t              cursor = 0;
        // This generation's (reduced) start page; the first next()/prev()
        // places the cursor relative to it (#140).
        std::size_t              origin_page = 0;
        // False until the cursor is placed on a hit (by next(), prev() or
        // set_reference_page()); while false, current() is empty and a
        // page landing in `hits` has no cursor to keep in place.
        bool                     anchored = false;
        std::size_t              pages_remaining = 0;
        OnUpdate                 on_update;
    };

    const Document&                         doc;
    litepdf::app::ISearchDispatcher&        dispatcher;
    std::shared_ptr<State>                  state;
    // Per-generation cookies. Mutated only on the UI thread (set_query /
    // dtor); each generation's tasks capture their own cookie by value, so
    // workers never touch this object. See GenerationAbort.hpp.
    GenerationAbort                         gen;
    // Test seam; see SearchSession::set_after_reset_hook. UI thread only.
    std::function<void()>                   after_reset_hook;

    // The session's own context and document (#130): every scan runs on this
    // pair, never on the tab Document's, so a scan never takes doc_mutex.
    // scan_mtx serialises the dispatcher's workers on it (MuPDF allows one
    // document on several threads only if the uses never overlap). Tasks
    // capture raw pointers to these; ~SearchSession drains every task before
    // it drops them. They stay out of State on purpose: a task's State
    // reference can be the last one and die on a worker after the Document
    // has gone (the #61 shape, spec §3.6). ~SearchSession drops sdoc
    // explicitly; `escrow` then drops its context when Impl dies.
    EscrowContext                           escrow;
    fz_document*                            sdoc = nullptr;
    std::mutex                              scan_mtx;

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
                             litepdf::app::ISearchDispatcher& disp,
                             std::optional<std::string_view> password)
    : impl_(std::make_unique<Impl>(doc, disp)) {
    // Everything is acquired into locals and moved into Impl only once the
    // whole open has succeeded, so a throw below leaves Impl empty: the
    // local `escrow` drops the context, after the document (if any) was
    // dropped on it. An encrypted Document that is not authenticated counts
    // as not open, so it yields an empty escrow too.
    EscrowContext escrow = doc.clone_escrow_context();
    if (!escrow.valid()) {
        throw std::runtime_error("SearchSession: could not clone the Document's "
                                 "context (is the Document open?)");
    }
    fz_context* ctx = escrow.get();

    // fz_open_document takes UTF-8; path::u8string() is UTF-8 on MSVC, where
    // path::string() would be the ANSI code page. Same conversion as
    // RenderEngine's worker documents and Document::open.
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const auto path_u8 = doc.source_path().u8string();
    const std::string path_str(reinterpret_cast<const char*>(path_u8.data()),
                               path_u8.size());
#else
    const std::string path_str = doc.source_path().u8string();
#endif
    if (path_str.empty()) {
        throw std::runtime_error("SearchSession: Document has no source path "
                                 "(is the Document open?)");
    }

    // fz_authenticate_password needs a NUL-terminated copy; it is wiped on
    // every exit from this constructor (Phase 8 D3), like RenderEngine's.
    struct WipeOnExit {
        std::string& s;
        ~WipeOnExit() { SecureZeroMemory(s.data(), s.size()); }
    };
    std::string pw(password.value_or(std::string_view{}));
    const WipeOnExit wipe{pw};

    // MuPDF's message from whichever step below fails (#137). Taken with
    // fz_convert_error, which also marks the error handled, so dropping the
    // context does not log it a second time as an "UNHANDLED EXCEPTION!".
    std::string reason;

    fz_document* sdoc = nullptr;
    fz_var(sdoc);
    fz_try(ctx) {
        sdoc = fz_open_document(ctx, path_str.c_str());
    }
    fz_catch(ctx) {
        sdoc = nullptr;
        reason = fz_convert_error(ctx, nullptr);
    }
    if (!sdoc) {
        throw std::runtime_error(detail::open_failure(
            "SearchSession: fz_open_document failed", reason));
    }

    // Ask once, right after the open and BEFORE any authentication, and never
    // again after a successful one: pdf_needs_password re-runs a blank
    // pdf_authenticate_password (pdf-crypt.c:826-833), and that failed blank
    // attempt resets the document's access and key (:803), de-authenticating
    // it. An encrypted document scanned without its key decrypts every
    // content stream to garbage and reports 0 hits, so a missing, empty or
    // wrong password must fail here rather than scan.
    int needs = 0;
    bool query_failed = false;
    fz_var(needs);
    fz_var(query_failed);
    fz_try(ctx) {
        needs = fz_needs_password(ctx, sdoc);
    }
    fz_catch(ctx) {
        query_failed = true;
        reason = fz_convert_error(ctx, nullptr);
    }
    if (query_failed) {
        // Rollback: drop the document on the context it was opened on,
        // before that context (the local escrow) goes.
        fz_drop_document(ctx, sdoc);
        throw std::runtime_error(detail::open_failure(
            "SearchSession: could not tell whether the document needs a password",
            reason));
    }

    if (needs) {
        // Attempted whenever a password is needed, even an empty one, so the
        // document itself is what refuses (spec §3.2).
        int ok = 0;
        fz_var(ok);
        fz_try(ctx) {
            ok = fz_authenticate_password(ctx, sdoc, pw.c_str());
        }
        fz_catch(ctx) {
            ok = 0;
            reason = fz_convert_error(ctx, nullptr);
        }
        if (!ok) {
            // Rollback, as above.
            fz_drop_document(ctx, sdoc);
            throw std::runtime_error(detail::open_failure(pw.empty()
                ? "SearchSession: document needs a password"
                : "SearchSession: password rejected", reason));
        }
    }

    // Commit; neither step can throw.
    impl_->escrow = std::move(escrow);
    impl_->sdoc   = sdoc;
}

SearchSession::~SearchSession() {
    // Drain every in-flight task before dropping the session's own document
    // and context, which those tasks use through raw pointers.
    //
    // Step 1: bump epoch so a task that has not started scanning yet --
    // one already popped, or one still queued on a dispatcher that cannot
    // discard (Step 2) -- returns without scanning, including one already
    // waiting on scan_mtx, which re-checks the epoch once it holds it. It
    // still pays one scope-guard decrement, which is what lets the drain
    // loop below terminate.
    auto& st = *impl_->state;
    st.epoch.fetch_add(1, std::memory_order_acq_rel);
    // Also abort the active generation's cookie so a task already building a
    // page's text stops at MuPDF's next cookie poll (before every content-
    // stream token in a PDF) and its match loop returns at once, instead of
    // finishing the page first. Formats whose build ignores the cookie
    // (ePub, FB2, SVG, CBZ) still finish the one page in progress (§3.4).
    impl_->gen.abort_active();

    // Step 2: discard this session's tasks still queued, and take their
    // count back -- they never run, so they never decrement it. Otherwise
    // they would have to wait for turns behind other tabs' scans before
    // each could return at its epoch check.
    const std::size_t discarded = impl_->dispatcher.discard(impl_->state);
    st.pending_tasks.fetch_sub(static_cast<int>(discarded), std::memory_order_acq_rel);

    // Step 3: spin-wait until every task not discarded has executed its
    // scope guard. On ThreadPoolDispatcher queued tasks were discarded, so
    // only a task already popped is left, and the pool runs at most one
    // task of this session at a time: this waits for at most one, which
    // returns at its epoch check or stops at the cookie poll (or, on a
    // format whose build ignores the cookie, finishes its page). A task
    // already past epoch check #2 also calls the on_update observer before
    // its guard decrements; every production observer posts, so the spin
    // cannot deadlock on it.
    while (st.pending_tasks.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Step 4: no task can still be using the pair. Drop the document on its
    // own context; `escrow` drops the context when Impl dies, after this.
    if (impl_->sdoc) {
        fz_drop_document(impl_->escrow.get(), impl_->sdoc);
        impl_->sdoc = nullptr;
    }
}

// ---------------------------------------------------------------------------
// set_query / clear
// ---------------------------------------------------------------------------
void SearchSession::set_query(std::wstring q, Flags f, std::size_t start_page) {
    auto& st = *impl_->state;

    // page_count() takes no lock. Read once, here: the scan starts at
    // `start` (reduced, so any start page is valid) and wraps forward.
    const std::size_t pages = impl_->doc.page_count();
    const std::size_t start = pages ? start_page % pages : 0;

    // Bump the epoch in the SAME critical section that clears the hits. A
    // superseded task re-checks the epoch under st.m before appending, so
    // it must never be able to take the mutex between the clear and the
    // bump -- it would see its own epoch and append stale hits to the
    // freshly cleared list.
    std::uint64_t ep = 0;
    {
        // Test seam (see set_after_reset_hook). Declared before the lock so
        // it fires on scope exit right AFTER the mutex is released: nothing
        // can sit between the unlock and the hook, so the seam's regression
        // test also catches a bump moved out of this block.
        struct AfterReset {
            const std::function<void()>& hook;
            ~AfterReset() { if (hook) hook(); }
        } after_reset{impl_->after_reset_hook};

        std::lock_guard<std::mutex> g(st.m);
        st.query           = std::move(q);
        st.flags           = f;
        st.hits.clear();
        st.cursor          = 0;
        st.origin_page     = start;
        st.anchored        = false;
        st.pages_remaining = 0;
        ep = st.epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    // Discard the previous generations' tasks still queued, so this
    // generation's tasks do not wait behind them on this session's queue
    // (every generation shares `state`, one owner to the dispatcher). None
    // of them ran, so none will decrement pending_tasks: take their count
    // back here. Until then the count is briefly too high, never too low.
    // Above the empty-query return, so clear() discards too.
    const std::size_t discarded = impl_->dispatcher.discard(impl_->state);
    st.pending_tasks.fetch_sub(static_cast<int>(discarded), std::memory_order_acq_rel);

    // Start a new abort generation: this sets the PREVIOUS generation's
    // cookie `abort`, so a worker still building a page for the old query
    // stops at MuPDF's next poll, and mints a fresh cookie that this
    // generation's tasks share. (One shared cookie couldn't do this: MuPDF
    // forbids resetting `abort`, and this generation needs it at 0.)
    // Empty-query clears also abort prior in-flight work, which is the
    // desired behavior.
    std::shared_ptr<fz_cookie> cookie = impl_->gen.begin_generation();

    // Empty query: treat as "cleared" — no tasks, scan_complete=true
    // immediately (pages_remaining already zeroed above). This early return
    // MUST stay below begin_generation() above: clearing the query still has
    // to abort the prior generation's in-flight worker, so do not hoist this
    // check above the begin_generation() call.
    OnUpdate cb_snapshot;
    {
        std::lock_guard<std::mutex> g(st.m);
        cb_snapshot = st.on_update;
    }
    if (impl_->state->query.empty()) {
        if (cb_snapshot) cb_snapshot();
        return;
    }

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
    // The session's own pair and its mutex (#130). Raw pointers into Impl:
    // ~SearchSession drains every task before it drops them.
    fz_context*                  sctx         = impl_->escrow.get();
    fz_document*                 sdoc         = impl_->sdoc;
    std::mutex*                  smtx         = &impl_->scan_mtx;
    const std::string            needle_utf8  = utf16_to_utf8(state_strong->query);
    const Flags                  captured_flags = f;

    // All tasks at P2. The order pages complete in comes from the order they
    // are submitted -- the start page, then forward with wrap -- and the
    // dispatcher's per-owner FIFO, which runs one task of this session at a
    // time (spec #140 §3.1-3.3).
    for (std::size_t k = 0; k < pages; ++k) {
        const std::size_t i = (start + k) % pages;
        litepdf::app::ISearchDispatcher::Task t{};
        t.state_weak = std::weak_ptr<void>(
            std::static_pointer_cast<void>(state_strong));
        t.priority = 2;
        t.run = [state_strong, sctx, sdoc, smtx, i, needle_utf8, captured_flags,
                 ep, cookie]() {
            // RAII drain guard: always decrement pending_tasks on return,
            // whether we hit a stale-epoch exit below, a throw escapes the
            // scan, or we complete normally. This is what makes
            // ~SearchSession's spin-wait terminate. Declared first, so it
            // runs last: the scan lock below is already released when the
            // decrement lets ~SearchSession destroy the mutex.
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
            std::vector<Document::PageHit> raw_hits;
            {
                // One task at a time on the session's pair. Scoped to the
                // scan alone and opened after check #1, so it lives inside
                // the Guard's lifetime and unlocks before pending_tasks drops.
                std::lock_guard<std::mutex> scan_lock(*smtx);
                // Epoch check #1b: this task may have waited here while a
                // newer query superseded it. Without this re-check it would
                // run a whole stale build -- and on a format whose build
                // ignores the cookie (spec §3.4) nothing would cut it short.
                if (state_strong->epoch.load(std::memory_order_acquire) != ep) {
                    return;
                }
                raw_hits = detail::page_hits_on(sctx, sdoc, i, needle_utf8, df,
                                                cookie.get());
            }

            // Order this page's hits top to bottom before taking the state
            // mutex (#139). Stable, so hits sharing a line keep the order
            // the page reports them in: left to right.
            std::vector<Hit> page_hits;
            page_hits.reserve(raw_hits.size());
            for (auto& ph : raw_hits) page_hits.push_back(Hit{i, std::move(ph)});
            std::stable_sort(page_hits.begin(), page_hits.end(),
                [](const Hit& a, const Hit& b) { return a.geom.ul_y < b.geom.ul_y; });

            // Epoch check #2: we were busy in MuPDF; the user may have
            // typed more characters in the meantime. Drop stale results.
            OnUpdate cb;
            {
                std::lock_guard<std::mutex> g(state_strong->m);
                if (state_strong->epoch.load(std::memory_order_acquire) != ep) {
                    return;
                }
                // `hits` stays sorted by page, so this page's hits go in
                // as one block before the first hit of a later page. No
                // re-sort of the whole list under the mutex, which UI-thread
                // readers wait on (#139).
                auto& hits = state_strong->hits;
                const auto at = std::upper_bound(hits.begin(), hits.end(), i,
                    [](std::size_t p, const Hit& h) { return p < h.page; });
                // Keep the cursor on the hit it points at: hits inserted
                // at or before it push it right (#140).
                const auto index = static_cast<std::size_t>(at - hits.begin());
                const auto added = page_hits.size();
                hits.insert(at, std::make_move_iterator(page_hits.begin()),
                            std::make_move_iterator(page_hits.end()));
                if (state_strong->anchored && index <= state_strong->cursor) {
                    state_strong->cursor += added;
                }
                if (state_strong->pages_remaining > 0) {
                    --state_strong->pages_remaining;
                }
                cb = state_strong->on_update;
            }
            if (cb) cb();
        };
        // Arm the drain counter per task, BEFORE its submit (the task may
        // run and decrement before submit returns). fetch_add (not store)
        // because a prior generation's tasks may still be in-flight — they
        // decrement via their own scope guards, so we must not clobber
        // their contribution by overwriting. The counter transiently
        // reflects "sum of in-flight across all generations", which is
        // exactly what ~SearchSession wants to drain down to zero. If
        // submit throws (e.g. bad_alloc on the queue push), the task never
        // reached a worker and will never decrement, so take its count
        // back — otherwise ~SearchSession would spin forever. The tasks
        // not yet built are simply never counted. This generation is left
        // partial (pages_remaining never reaches 0, so scan_complete()
        // stays false) until the next set_query resets it.
        st.pending_tasks.fetch_add(1, std::memory_order_acq_rel);
        try {
            impl_->dispatcher.submit(std::move(t));
        } catch (...) {
            st.pending_tasks.fetch_sub(1, std::memory_order_acq_rel);
            throw;
        }
    }
}

void SearchSession::set_after_reset_hook(std::function<void()> hook) {
    impl_->after_reset_hook = std::move(hook);
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

namespace {

// Index of the first hit on or after `page` in `hits` (sorted by page);
// hits.size() if there is none.
std::size_t first_hit_at_or_after(const std::vector<SearchSession::Hit>& hits,
                                  std::size_t page) {
    const auto it = std::lower_bound(hits.begin(), hits.end(), page,
        [](const SearchSession::Hit& h, std::size_t p) { return h.page < p; });
    return static_cast<std::size_t>(it - hits.begin());
}

}  // namespace

void SearchSession::set_reference_page(std::size_t page) {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) {
        st.cursor = 0;
        return;
    }
    // First hit at-or-after `page`, wrapping to the first hit.
    const std::size_t i = first_hit_at_or_after(st.hits, page);
    st.cursor = i == st.hits.size() ? 0 : i;
    st.anchored = true;
}

std::optional<SearchSession::Hit> SearchSession::current() const {
    std::lock_guard<std::mutex> g(impl_->state->m);
    if (!impl_->state->anchored) return std::nullopt;
    return impl_->state->hits[impl_->state->cursor];
}

std::optional<SearchSession::Hit>
SearchSession::next(std::optional<std::size_t> from_page) {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) return std::nullopt;
    if (!st.anchored) {
        // First use: the first hit on or after the page on screen.
        const std::size_t i =
            first_hit_at_or_after(st.hits, from_page.value_or(st.origin_page));
        st.cursor = i == st.hits.size() ? 0 : i;
        st.anchored = true;
    } else {
        st.cursor = (st.cursor + 1) % st.hits.size();
    }
    return st.hits[st.cursor];
}

std::optional<SearchSession::Hit>
SearchSession::prev(std::optional<std::size_t> from_page) {
    auto& st = *impl_->state;
    std::lock_guard<std::mutex> g(st.m);
    if (st.hits.empty()) return std::nullopt;
    const auto n = st.hits.size();
    if (!st.anchored) {
        // First use: the hit just before the first one on or after the page
        // on screen, wrapping to the last.
        const std::size_t i =
            first_hit_at_or_after(st.hits, from_page.value_or(st.origin_page));
        st.cursor = (i + n - 1) % n;
        st.anchored = true;
    } else {
        st.cursor = (st.cursor + n - 1) % n;
    }
    return st.hits[st.cursor];
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
    // Hits are sorted by page: the page's hits are one contiguous range.
    const auto& hits = impl_->state->hits;
    const auto first = std::lower_bound(hits.begin(), hits.end(), page,
        [](const Hit& h, std::size_t p) { return h.page < p; });
    const auto last = std::upper_bound(first, hits.end(), page,
        [](std::size_t p, const Hit& h) { return p < h.page; });
    return std::vector<Hit>(first, last);
}

}  // namespace litepdf::core
