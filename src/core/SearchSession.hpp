#pragma once

// core::SearchSession — per-tab search state: query, flags, hit cache,
// cursor. The scan itself runs on an ISearchDispatcher's worker threads,
// on the session's own cloned context. See Phase 6 design §4 D1.
//
// Thread-safety:
//   - Public API is single-threaded (UI thread).
//   - Task lambdas submitted to dispatcher run on worker threads. They
//     touch SearchState under its own std::mutex.
//   - Every scan task runs under a session-owned mutex (scan_mtx), which
//     serialises the dispatcher's workers on the session's context. The
//     document it scans depends on the mode:
//       DocumentShare::Own (#130): the session's own fz_document, opened at
//         construction. A scan never touches the tab Document, so a UI-thread
//         call into the Document never waits on a search, and a search never
//         waits on a print or a text-selection build.
//       DocumentShare::Shared (#229, an FB2 tab): the tab Document's own
//         document, reached through detail::SharedDocumentAccess under the
//         Document's gate in the Scan class, one page per hold. A scan then
//         waits for the UI thread and the render worker (Foreground and
//         Render go first; a waiting Scan is admitted after at most
//         detail::kScanAfterRenders Render holds), and a UI-thread call waits
//         for at most the one page scan in progress.
//   - The UI-thread calls still use the tab Document: set_query's
//     page_count() (which takes no lock) and query_compiles() (a brief
//     Foreground hold of the Document's gate on the UI thread).

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentShare.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litepdf::core {

class SearchSession {
public:
    struct Flags {
        bool match_case = false;
        bool whole_word = false;
        bool regex      = false;
    };

    struct Hit {
        std::size_t       page;
        Document::PageHit geom;
    };

    using OnUpdate = std::function<void()>;

    // Caller must keep `doc` and `dispatcher` alive for the SearchSession's
    // lifetime. SearchSession holds non-owning refs. Clones a context from
    // `doc` for the scans.
    //
    // DocumentShare::Own (default): opens `doc.source_path()` again on that
    // context and, when that document needs a password, authenticates it
    // with `password` -- pass the password `doc` was authenticated with. The
    // scans run on that document only (see Thread-safety above). Its copy of
    // the password is wiped on every exit. Throws std::runtime_error if the
    // clone or the open fails, or if the document needs a password and
    // `password` is missing, empty or wrong, after releasing whatever it
    // acquired.
    //
    // DocumentShare::Shared (#229): opens nothing and does not read
    // `password`; the scans borrow `doc`'s own document under its gate.
    // Throws std::runtime_error only if the clone fails.
    SearchSession(const Document& doc, litepdf::app::ISearchDispatcher& dispatcher,
                  std::optional<std::string_view> password = std::nullopt,
                  DocumentShare share = DocumentShare::Own);
    ~SearchSession();

    SearchSession(const SearchSession&)            = delete;
    SearchSession& operator=(const SearchSession&) = delete;

    // Bumps epoch (cancels in-flight stale tasks via the run lambda's
    // epoch check), clears hits, discards the previous generation's tasks
    // still queued on the dispatcher, and submits one task per page: from
    // `start_page` (taken modulo the page count, so any value is valid)
    // forward, wrapping past the last page to page 0. Empty query clears
    // everything and marks scan_complete=true immediately.
    void set_query(std::wstring q, Flags f, std::size_t start_page = 0);
    void clear();

    // True iff `q` compiles as a search needle under `f` (delegates to
    // Document::query_compiles after UTF-16->UTF-8). Pure check; no scan.
    bool query_compiles(const std::wstring& q, Flags f) const;

    // Observer fires on the thread that completes a page task. For
    // ThreadPoolDispatcher, that's a worker thread — the observer is
    // responsible for marshaling to UI thread if it touches HWND.
    // For InlineDispatcher (tests), fires on the caller thread.
    void set_on_update(OnUpdate cb);

    // Returns a copy of the currently-installed observer (or an empty
    // std::function if none). Used by CrossTabSearch to chain its own
    // observer on top of whatever MainWindow installed, so that
    // per-tab find-bar counters keep updating during cross-tab scans.
    OnUpdate on_update() const;

    // Reposition cursor to first hit at-or-after the given page; wraps
    // to the first hit if nothing is at-or-after. Anchors the cursor when
    // there are hits.
    void set_reference_page(std::size_t page);

    // The cursor is unanchored after set_query() and stays so until the
    // first next()/prev() that finds a hit (or set_reference_page()):
    //   - current() is empty while unanchored (no match is selected).
    //   - The first next() returns the first hit on or after `from_page`
    //     (default: set_query's start page; wraps to the first hit); the
    //     first prev() returns the hit just before it (wraps to the last).
    //   - Once anchored, next()/prev() step one hit, wrapping, and ignore
    //     `from_page`.
    //   - With no hits yet both return nothing and stay unanchored.
    // The cursor keeps pointing at the same hit while pages land.
    std::optional<Hit> current() const;
    std::optional<Hit> next(std::optional<std::size_t> from_page = std::nullopt);
    std::optional<Hit> prev(std::optional<std::size_t> from_page = std::nullopt);

    std::size_t hit_count() const;
    bool        scan_complete() const;

    std::vector<Hit> hits_for_page(std::size_t page) const;

    // (#229) fz_open_document calls this session made: 1 with
    // DocumentShare::Own, 0 with Shared. Test observability only.
    [[nodiscard]] std::size_t documents_opened() const noexcept;

private:
    // Test seam: `hook` runs on the set_query() thread right after the
    // previous generation's hits are cleared and the state mutex is
    // released, before any task of the new generation is submitted. It lets
    // a test land a superseded worker in exactly that window, which no
    // dispatcher double can reach. Reached only via SearchSessionTestAccess.
    void set_after_reset_hook(std::function<void()> hook);
    friend struct SearchSessionTestAccess;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
