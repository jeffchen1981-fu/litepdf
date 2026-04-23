// Tests for core::SearchSession — per-tab search state (Phase 6 Task 5).
//
// Uses the deterministic search.pdf fixture (Phase 6 Task 2):
//   p0: 12 Lorem
//   p1: 1 dolor
//   p2: CJK keyword
//   p3: 1 dolor
//   p4: (no hits)
//   p5: 3 Lorem
//   p6: 1 dolor
// Totals: Lorem=15, dolor=3.
//
// InlineDispatcher runs tasks synchronously on submit(), so after
// set_query() returns every page has already been scanned and
// scan_complete() is true. Exercising the worker-thread path is the
// job of the ThreadPoolDispatcher integration tests in later phases.

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/SearchSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

using namespace litepdf::core;
using namespace litepdf::app;

namespace {

Document open_search_fixture() {
    Document doc;
    auto err = doc.open(std::filesystem::path("tests/fixtures/search.pdf"));
    REQUIRE_FALSE(err.has_value());
    return doc;
}

// Bundles Document + InlineDispatcher + SearchSession so each TEST_CASE
// starts from a clean, fully-synchronous state.
struct Fx {
    Document doc;
    InlineDispatcher dispatcher;
    std::unique_ptr<SearchSession> session;

    Fx() : doc(open_search_fixture()) {
        session = std::make_unique<SearchSession>(doc, dispatcher);
    }
};

}  // namespace

TEST_CASE("SearchSession: empty query yields no hits, scan complete",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"", SearchSession::Flags{});
    REQUIRE(f.session->hit_count() == 0);
    REQUIRE(f.session->scan_complete());
}

TEST_CASE("SearchSession: Lorem query finds all 15 hits across pages 0 and 5",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"Lorem", SearchSession::Flags{});
    // With InlineDispatcher: submit() runs task synchronously. After
    // set_query returns, all 7 pages have been searched.
    REQUIRE(f.session->scan_complete());
    REQUIRE(f.session->hit_count() == 15);  // 12 on p0 + 3 on p5
}

TEST_CASE("SearchSession: dolor query walks page-then-position",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"dolor", SearchSession::Flags{});
    REQUIRE(f.session->hit_count() == 3);
    // dolor is on p1, p3, p6.
    auto h0 = f.session->current();
    REQUIRE(h0.has_value());
    REQUIRE(h0->page == 1);
    auto h1 = f.session->next();
    REQUIRE(h1.has_value());
    REQUIRE(h1->page == 3);
    auto h2 = f.session->next();
    REQUIRE(h2.has_value());
    REQUIRE(h2->page == 6);
}

TEST_CASE("SearchSession: no-hit query yields zero hits, scan complete",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"XYZABC123", SearchSession::Flags{});
    REQUIRE(f.session->hit_count() == 0);
    REQUIRE(f.session->scan_complete());
}

TEST_CASE("SearchSession: query change cancels previous",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"Lor", SearchSession::Flags{});
    const auto first = f.session->hit_count();
    f.session->set_query(L"Lorem", SearchSession::Flags{});
    const auto second = f.session->hit_count();
    REQUIRE(second == 15);          // only Lorem hits
    REQUIRE(first >= second);       // Lor is a superset of Lorem matches
}

TEST_CASE("SearchSession: cursor wraps at document boundaries",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"dolor", SearchSession::Flags{});
    // current=p1, next()->p3, next()->p6 (last)
    f.session->next();
    f.session->next();
    auto wrapped = f.session->next();
    REQUIRE(wrapped.has_value());
    REQUIRE(wrapped->page == 1);    // wrapped forward to first
    auto before = f.session->prev();
    REQUIRE(before.has_value());
    REQUIRE(before->page == 6);     // wrapped backward to last
}

TEST_CASE("SearchSession: set_reference_page repositions cursor",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"dolor", SearchSession::Flags{});
    f.session->set_reference_page(3);  // first hit at-or-after p3
    auto h = f.session->current();
    REQUIRE(h.has_value());
    REQUIRE(h->page == 3);
}

TEST_CASE("SearchSession: destruct while holding state is safe",
          "[search][session]") {
    Document doc = open_search_fixture();
    InlineDispatcher d;
    {
        SearchSession s(doc, d);
        s.set_query(L"Lorem", SearchSession::Flags{});
        // Session goes out of scope. InlineDispatcher has already completed
        // all tasks synchronously, but the destructor order (SearchState
        // shared_ptr released, Document still alive) exercises the
        // weak-ptr guard in the ThreadPoolDispatcher path in principle.
    }
    SUCCEED("destruct during/after scan is safe");
}
