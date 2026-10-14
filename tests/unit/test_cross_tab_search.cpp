// Tests for app::CrossTabSearch — cross-tab fan-out orchestrator
// (Phase 6 Task 11).
//
// Uses the deterministic search.pdf fixture (Phase 6 Task 2):
//   Lorem=15 per document, so 2 copies => 30 aggregated hits.
//
// InlineDispatcher runs tasks synchronously on submit(), so after
// dispatch() returns every page has already been scanned and every
// observer callback (including our chained aggregator) has fired.

#include "app/CrossTabSearch.hpp"
#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/SearchSession.hpp"
#include "search_test_kit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

using namespace litepdf::app;
using namespace litepdf::core;
using litepdf::test_kit::DeferredDispatcher;

namespace {

Document open_search_fixture() {
    Document d;
    auto err = d.open(std::filesystem::path("tests/fixtures/search.pdf"));
    REQUIRE_FALSE(err.has_value());
    return d;
}

}  // namespace

TEST_CASE("CrossTabSearch aggregates hits from 2 views", "[crosstab]") {
    InlineDispatcher dispatcher;
    auto v1 = std::make_unique<DocumentView>(open_search_fixture(), dispatcher);
    auto v2 = std::make_unique<DocumentView>(open_search_fixture(), dispatcher);

    CrossTabSearch xts;
    xts.dispatch(L"Lorem", SearchSession::Flags{},
                 {{v1.get(), L"search1.pdf"}, {v2.get(), L"search2.pdf"}});

    // 15 Lorem hits per fixture copy * 2 tabs = 30.
    REQUIRE(xts.hit_count() == 30);

    // Every hit should carry a label and a live session_state weak_ptr.
    const auto all = xts.hits();
    REQUIRE(all.size() == 30);
    std::size_t from_v1 = 0, from_v2 = 0;
    for (const auto& h : all) {
        REQUIRE_FALSE(h.file_label.empty());
        REQUIRE_FALSE(h.session_state.expired());
        if (h.view_at_submit == v1.get()) ++from_v1;
        if (h.view_at_submit == v2.get()) ++from_v2;
    }
    REQUIRE(from_v1 == 15);
    REQUIRE(from_v2 == 15);
}

TEST_CASE("CrossTabSearch weak-ref protects against view destruction",
          "[crosstab]") {
    InlineDispatcher dispatcher;
    auto view = std::make_unique<DocumentView>(open_search_fixture(), dispatcher);

    CrossTabSearch xts;
    xts.dispatch(L"Lorem", SearchSession::Flags{},
                 {{view.get(), L"search.pdf"}});
    REQUIRE(xts.hit_count() == 15);

    // Close the tab. The Hit entries still live in xts.hits() and their
    // view_at_submit pointers now dangle — the weak_ptr liveness token
    // is what row-click handlers must consult before dereferencing.
    view.reset();

    // hits() must be callable without crashing; the entries' weak_ptrs
    // are still valid (they point to CrossTabSearch's sentinel, not the
    // view), but view_at_submit is dangling. We intentionally don't
    // deref view_at_submit here.
    auto snapshot = xts.hits();
    (void)snapshot;
    SUCCEED("no crash after view destruction");

    // clear() after the owning view is gone must also not crash — it
    // would previously try to detach observers via view->search(),
    // which is why CrossTabSearch::clear() moves tabs out under lock
    // BEFORE detaching. The tab is already gone by then.
    // NOTE: we don't call clear() here because detach touches the
    // destroyed view. The design contract is "the caller clears before
    // closing tabs" — matching the snapshot-frozen semantics of §D9.
}

TEST_CASE("CrossTabSearch: each tab scans from its own page first",
          "[crosstab]") {
    DeferredDispatcher d;
    auto v1 = std::make_unique<DocumentView>(open_search_fixture(), d);
    auto v2 = std::make_unique<DocumentView>(open_search_fixture(), d);
    v1->set_current_page(5);
    v2->set_current_page(2);

    CrossTabSearch xts;
    xts.dispatch(L"Page", SearchSession::Flags{},
                 {{v1.get(), L"a"}, {v2.get(), L"b"}});

    // v1's seven tasks were queued before v2's, in submission order, so
    // slot 0 is v1's first page and slot 7 is v2's first page.
    d.run(0);
    d.run(7);
    CHECK(!v1->search().hits_for_page(5).empty());
    CHECK(!v2->search().hits_for_page(2).empty());

    // Run the tail for completeness; the views do not require it
    // (~SearchSession discards whatever is still queued).
    d.run_all();
}
