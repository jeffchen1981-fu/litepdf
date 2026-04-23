#pragma once

// core::SearchSession — per-tab search state: query, flags, hit cache,
// cursor. Pure logic, no Win32, no threads. Background search work is
// delegated to an ISearchDispatcher. See Phase 6 design §4 D1.
//
// Thread-safety:
//   - Public API is single-threaded (UI thread).
//   - Task lambdas submitted to dispatcher run on worker threads. They
//     touch SearchState under its own std::mutex.
//   - Document::page_hits is not thread-safe on the same Document;
//     since one SearchSession owns one Document, and SearchDispatcher
//     is FIFO per priority, concurrent page_hits on the same doc does
//     not occur in Phase 6.1 (single tab, one SearchSession). Phase 6.2
//     cross-tab uses distinct Documents per tab, so there is still no
//     race. Revisit this if we ever want two workers to strip-mine
//     pages of the same Document concurrently — then workers would
//     need cloned fz_contexts (see Document::clone_context()).

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace litepdf::core {

class SearchSession {
public:
    struct Flags {
        bool match_case = false;
    };

    struct Hit {
        std::size_t       page;
        Document::PageHit geom;
    };

    using OnUpdate = std::function<void()>;

    // Caller must keep `doc` and `dispatcher` alive for the SearchSession's
    // lifetime. SearchSession holds non-owning refs.
    SearchSession(const Document& doc, litepdf::app::ISearchDispatcher& dispatcher);
    ~SearchSession();

    SearchSession(const SearchSession&)            = delete;
    SearchSession& operator=(const SearchSession&) = delete;

    // Bumps epoch (cancels in-flight stale tasks via the run lambda's
    // epoch check), clears hits, and submits one task per page. Empty
    // query clears everything and marks scan_complete=true immediately.
    void set_query(std::wstring q, Flags f);
    void clear();

    // Observer fires on the thread that completes a page task. For
    // ThreadPoolDispatcher, that's a worker thread — the observer is
    // responsible for marshaling to UI thread if it touches HWND.
    // For InlineDispatcher (tests), fires on the caller thread.
    void set_on_update(OnUpdate cb);

    // Reposition cursor to first hit at-or-after the given page; wraps
    // to the first hit if nothing is at-or-after.
    void set_reference_page(std::size_t page);

    std::optional<Hit> current() const;
    std::optional<Hit> next();
    std::optional<Hit> prev();

    std::size_t hit_count() const;
    bool        scan_complete() const;

    std::vector<Hit> hits_for_page(std::size_t page) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
