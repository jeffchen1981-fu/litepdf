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
// Most cases use InlineDispatcher, which runs tasks synchronously on
// submit(), so after set_query() returns every page has already been
// scanned and scan_complete() is true. One case drives a real
// ThreadPoolDispatcher; the cases at the end use dispatcher doubles
// (DeferredDispatcher, ThrowingDispatcher) to pin down a task interleaving
// or a submit failure deterministically.

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"
#include "core/SearchSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace litepdf::core;
using namespace litepdf::app;

namespace litepdf::core {

// Friend of SearchSession: reaches the private after-reset test seam.
struct SearchSessionTestAccess {
    static void set_after_reset_hook(SearchSession& s, std::function<void()> hook) {
        s.set_after_reset_hook(std::move(hook));
    }
};

}  // namespace litepdf::core

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

// --- Flag threading (search-upgrade): exercises the SearchSession::Flags ->
// Document::SearchFlags hand-off so a future field-order edit can't silently
// misroute a flag. With InlineDispatcher the scan is synchronous, so hit_count
// is exact right after set_query.
TEST_CASE("SearchSession: match_case threads through to page_hits",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"lorem", SearchSession::Flags{false, false, false});
    REQUIRE(f.session->hit_count() == 15);   // case-insensitive: all "Lorem"
    f.session->set_query(L"lorem", SearchSession::Flags{true, false, false});
    REQUIRE(f.session->hit_count() == 0);    // exact: lowercase needle, "Lorem" text
}

TEST_CASE("SearchSession: regex flag threads through to page_hits",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"lo.em", SearchSession::Flags{false, false, true});
    REQUIRE(f.session->hit_count() == 15);   // '.' matches 'r' across both pages
    f.session->set_query(L"lo.em", SearchSession::Flags{false, false, false});
    REQUIRE(f.session->hit_count() == 0);    // literal: no "lo.em" in the text
}

TEST_CASE("SearchSession: whole_word flag threads through to page_hits",
          "[search][session]") {
    Fx f;
    f.session->set_query(L"Lorem", SearchSession::Flags{false, true, false});
    REQUIRE(f.session->hit_count() == 15);   // "Lorem" stands as a whole word
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

// Real worker pool (the InlineDispatcher path above is synchronous, so it
// never has an in-flight worker for a superseded query to abort). A burst of
// distinct queries with no waits leaves older generations very likely still
// scanning when the next set_query lands, exercising the per-generation abort
// token's lifetime across rapid generation swaps. The invariant under test is
// correctness/safety: no matter how many generations were cancelled, the FINAL
// query produces exactly its own hits and the run drains cleanly (a token-
// lifetime UAF would surface here as a crash). It does NOT assert that a
// superseded scan was cancelled *mid-page* — the epoch check alone already
// guarantees the final count, so that responsiveness property is owned by the
// deterministic test_generation_abort.cpp ("starting the next generation
// aborts the previous"), not by this end-to-end guard.
TEST_CASE("SearchSession: rapid query changes settle on correct results (ThreadPool)",
          "[search][session][threads]") {
    Document doc = open_search_fixture();
    ThreadPoolDispatcher pool(2);
    SearchSession s(doc, pool);

    for (int i = 0; i < 50; ++i) {
        s.set_query(L"Lor", SearchSession::Flags{});
        s.set_query(L"dolor", SearchSession::Flags{});
    }
    s.set_query(L"Lorem", SearchSession::Flags{});  // final generation

    // Wait (bounded) for the final generation to finish scanning all pages.
    for (int i = 0; i < 500 && !s.scan_complete(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(s.scan_complete());
    REQUIRE(s.hit_count() == 15);  // exactly Lorem's hits, no stale leakage
}

// #130 T1: a scan must not need the tab Document's doc_mutex. with_rendered_page
// holds doc_mutex for the whole callback, so a scan that still took it would
// stall on every page until the callback returned.
//
// The session, the pool and the Document are built OUTSIDE the callback, and
// the session outlives it: destroying the session inside it would hang instead
// of failing, because ~SearchSession spins on tasks blocked on the held lock.
//
// The callback holds no REQUIRE and calls no locking Document method. It runs
// outside fz_try, so a throw out of it (a failed REQUIRE) would leak the
// pixmap; and a same-thread re-entry of doc_mutex throws on MSVC. set_query's
// page_count() takes no lock, and scan_complete()/hit_count() lock only the
// session's state. The outcome goes into locals, asserted after the return.
// Before the change the poll times out after 5 s, the callback returns, the
// lock is released and the scan drains, so the test fails without hanging.
TEST_CASE("SearchSession: a scan completes while doc_mutex is held",
          "[search][session][threads]") {
    Document doc = open_search_fixture();
    ThreadPoolDispatcher pool(2);
    SearchSession s(doc, pool);

    bool        complete = false;
    std::size_t count    = 0;
    const bool rendered = doc.with_rendered_page(
        0, 1.0f, 1.0f, false, [&](int, int, const std::uint8_t*) {
            s.set_query(L"Lorem", SearchSession::Flags{});
            // A deadline, not an iteration count: Windows rounds a 5 ms sleep
            // up to the ~15.6 ms timer tick.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!s.scan_complete() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            complete = s.scan_complete();
            count    = s.hit_count();
        });

    REQUIRE(rendered);
    REQUIRE(complete);
    REQUIRE(count == 15);
}

// #130 T2: the session scans its own document, so it must authenticate that
// document itself. encrypted-text.pdf draws "ENCRYPTED n" on 8 lines
// (scripts/generate-encrypted-text-fixture.py:63-65); an unauthenticated
// document decrypts every content stream to garbage and reports 0, so the
// exact count rules that mode out.
TEST_CASE("SearchSession: scans a user-password PDF with the password",
          "[search][session]") {
    Document doc;
    const auto err = doc.open(std::filesystem::path("tests/fixtures/encrypted-text.pdf"));
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));

    InlineDispatcher inline_dispatcher;
    SearchSession s(doc, inline_dispatcher, std::string_view{"test"});
    s.set_query(L"ENCRYPTED", SearchSession::Flags{});
    REQUIRE(s.scan_complete());
    REQUIRE(s.hit_count() == 8);
}

// #130 T3: a session whose own document needs a password it was not given
// (none, or a wrong one) must fail loud at construction rather than scan
// garbage and report 0 hits.
TEST_CASE("SearchSession: refuses a user-password PDF without its password",
          "[search][session]") {
    Document doc;
    const auto err = doc.open(std::filesystem::path("tests/fixtures/encrypted-text.pdf"));
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));

    InlineDispatcher d;
    REQUIRE_THROWS_AS(SearchSession(doc, d), std::runtime_error);
    REQUIRE_THROWS_AS(SearchSession(doc, d, std::string_view{"wrong"}),
                      std::runtime_error);
}

// The rollback of a throwing constructor, and the destructor, must each give
// back the session's cloned context. The probe keeps the family countable
// (same pattern as test_document_selection's Type3 case).
TEST_CASE("SearchSession: constructor failure and destruction release the session context",
          "[search][session]") {
    Document doc;
    const auto err = doc.open(std::filesystem::path("tests/fixtures/encrypted-text.pdf"));
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));

    fz_context* tmp = doc.clone_context();
    REQUIRE(tmp != nullptr);
    EscrowContext probe = EscrowContext::clone_from(tmp);
    fz_drop_context(tmp);
    REQUIRE(probe.valid());
    const int before = detail::family_context_count(probe.get());

    InlineDispatcher d;
    REQUIRE_THROWS_AS(SearchSession(doc, d), std::runtime_error);
    REQUIRE(detail::family_context_count(probe.get()) == before);

    {
        SearchSession s(doc, d, std::string_view{"test"});
        REQUIRE(detail::family_context_count(probe.get()) == before + 1);
    }
    REQUIRE(detail::family_context_count(probe.get()) == before);
}

namespace {

// Copies a fixture to a unique temp path and removes it (best effort) when
// the scope ends, so a failed assertion does not leave the copy behind.
struct TempCopy {
    std::filesystem::path path;
    explicit TempCopy(const char* fixture, const char* name)
        : path(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::copy_file(fixture, path);
    }
    ~TempCopy() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

}  // namespace

// The context count cannot see a leaked fz_document (it holds no context
// reference). On Windows an open fz_document holds its file open, so a
// delete after the tab Document has closed fails if the rollback leaked it.
TEST_CASE("SearchSession: constructor failure releases the session document",
          "[search][session]") {
    const TempCopy copy("tests/fixtures/encrypted-text.pdf",
                        "litepdf-130-t3-rollback.pdf");
    Document doc;
    const auto err = doc.open(copy.path);
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));

    InlineDispatcher d;
    REQUIRE_THROWS(SearchSession(doc, d));
    doc.close();
    REQUIRE(std::filesystem::remove(copy.path));
}

// Validates the oracle above: the session's own document blocks the delete
// for as long as the session lives, even with the tab Document closed.
TEST_CASE("SearchSession: holds its own document open until destroyed",
          "[search][session]") {
    const TempCopy copy("tests/fixtures/search.pdf",
                        "litepdf-130-t3-holds-open.pdf");
    Document doc;
    REQUIRE_FALSE(doc.open(copy.path).has_value());

    InlineDispatcher d;
    auto s = std::make_unique<SearchSession>(doc, d);
    doc.close();
    std::error_code ec;
    REQUIRE_FALSE(std::filesystem::remove(copy.path, ec));
    s.reset();
    REQUIRE(std::filesystem::remove(copy.path));
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

namespace {

// Queues every submitted task without running it, so a test decides when
// (and inside which window) each one runs. run() honours the weak-state
// gate the real dispatchers apply.
class DeferredDispatcher final : public ISearchDispatcher {
public:
    void submit(Task t) override { tasks_.push_back(std::move(t)); }

    void run(std::size_t i) {
        Task t = std::move(tasks_.at(i));
        if (!t.state_weak.expired() && t.run) t.run();
    }

    // Runs every task not yet run, including ones submitted meanwhile.
    void run_all() {
        for (std::size_t i = 0; i < tasks_.size(); ++i) run(i);
    }

private:
    std::vector<Task> tasks_;
};

// Runs tasks inline like InlineDispatcher, but submit() throws bad_alloc on
// call number `fail_on` (1-based) -- as ThreadPoolDispatcher::submit's queue
// push can -- without running that task.
class ThrowingDispatcher final : public ISearchDispatcher {
public:
    explicit ThrowingDispatcher(int fail_on) : fail_on_(fail_on) {}

    void submit(Task t) override {
        if (++calls_ == fail_on_) throw std::bad_alloc();
        if (!t.state_weak.expired() && t.run) t.run();
    }

private:
    int fail_on_;
    int calls_ = 0;
};

}  // namespace

// A worker of the superseded generation that takes the state mutex right
// after set_query() cleared the hit list must see the new epoch and drop its
// results. Before the fix the epoch was bumped only after the mutex was
// released, so such a worker appended its stale page-0 Lorem hits (12) to
// the freshly cleared list and they leaked into the next query's results.
TEST_CASE("SearchSession: superseded worker cannot append hits after a retype",
          "[search][session]") {
    Document doc = open_search_fixture();
    DeferredDispatcher d;
    SearchSession s(doc, d);

    s.set_query(L"Lorem", SearchSession::Flags{});  // tasks 0..6, none run
    bool fired = false;
    SearchSessionTestAccess::set_after_reset_hook(s, [&] {
        if (fired) return;
        fired = true;
        d.run(0);  // Lorem page 0, landing between reset and submit
    });
    s.set_query(L"dolor", SearchSession::Flags{});
    // CHECK, not REQUIRE: the queued tasks must still drain below, or
    // ~SearchSession would spin on them.
    CHECK(fired);
    CHECK(s.hit_count() == 0);

    d.run_all();  // stale Lorem tasks drop; dolor scans all 7 pages
    REQUIRE(s.scan_complete());
    REQUIRE(s.hit_count() == 3);  // dolor only: p1, p3, p6
}

TEST_CASE("SearchSession: superseded worker cannot append hits after a clear",
          "[search][session]") {
    Document doc = open_search_fixture();
    DeferredDispatcher d;
    SearchSession s(doc, d);

    s.set_query(L"Lorem", SearchSession::Flags{});
    bool fired = false;
    SearchSessionTestAccess::set_after_reset_hook(s, [&] {
        if (fired) return;
        fired = true;
        d.run(0);
    });
    s.clear();
    CHECK(fired);
    CHECK(s.hit_count() == 0);

    d.run_all();  // drain the remaining stale tasks before ~SearchSession
    REQUIRE(s.hit_count() == 0);
}

// If submit() throws part-way through a generation, the tasks never handed
// to the dispatcher must not be counted as pending: ~SearchSession spins
// until that counter reaches zero, so before the fix it never returned.
// The destructor runs on a helper thread so a regression fails the test
// instead of hanging the suite; on timeout the session is deliberately
// leaked (the spinning thread still references it).
TEST_CASE("SearchSession: destructor returns after a dispatcher submit throws",
          "[search][session][threads]") {
    auto doc = std::make_unique<Document>(open_search_fixture());
    auto d   = std::make_unique<ThrowingDispatcher>(3);  // tasks 1-2 run, 3 throws
    auto s   = std::make_unique<SearchSession>(*doc, *d);

    REQUIRE_THROWS_AS(s->set_query(L"Lorem", SearchSession::Flags{}),
                      std::bad_alloc);

    auto              destroyed = std::make_shared<std::promise<void>>();
    std::future<void> done      = destroyed->get_future();
    SearchSession*    raw       = s.release();
    std::thread t([raw, destroyed] {
        delete raw;
        destroyed->set_value();
    });
    if (done.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        t.join();
        SUCCEED("~SearchSession returned");
    } else {
        // Keep everything the spinning destructor can reach alive forever.
        t.detach();
        (void)doc.release();
        (void)d.release();
        FAIL("~SearchSession still spinning 5 s after a submit throw");
    }
}
