// Stress test reproducing the "Ctrl+W during cross-tab scan" crash.
//
// Repro sequence (matches user report):
//   1. CrossTabSearch::dispatch(Lorem) on 2 SearchSessions backed by a
//      real ThreadPoolDispatcher.
//   2. Destroy one session immediately (simulates user closing the tab
//      mid-scan).
//   3. Keep the other session alive long enough to let workers churn.
//
// If the crash is real, this test will hit an access violation or ASan
// error on Windows. 100 iterations amplify timing windows.

#include "app/CrossTabSearch.hpp"
#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

using namespace litepdf::app;
using namespace litepdf::core;
using namespace std::chrono_literals;

namespace {
Document open_search_fixture() {
    Document d;
    REQUIRE_FALSE(d.open(std::filesystem::path("tests/fixtures/search.pdf"))
                      .has_value());
    return d;
}
}  // namespace

TEST_CASE("Ctrl+W during cross-tab scan — no crash", "[crosstab][stress][threads]") {
    for (int iter = 0; iter < 20; ++iter) {
        ThreadPoolDispatcher pool(2);
        auto v1 = std::make_unique<DocumentView>(open_search_fixture(), pool);
        auto v2 = std::make_unique<DocumentView>(open_search_fixture(), pool);

        CrossTabSearch xts;
        xts.dispatch(L"Lorem", SearchSession::Flags{},
                     {CrossTabSearch::TabRef{v1.get(), L"s1"},
                      CrossTabSearch::TabRef{v2.get(), L"s2"}});

        // Simulate Ctrl+W: clear() drops chained observers, then we destroy
        // the tab's DocumentView. ~SearchSession inside ~DocumentView must
        // drain outstanding page_hits tasks before Document tears down.
        xts.clear();
        v1.reset();

        // Let v2 continue scanning briefly. If worker threads have any
        // UAF path touching v1 after clear()+reset, ASan / SEH crashes
        // during this sleep.
        std::this_thread::sleep_for(50ms);

        v2.reset();
        // pool destructor drains workers.
    }
}

TEST_CASE("Ctrl+W during cross-tab scan — close BEFORE xts.clear()",
          "[crosstab][stress][threads]") {
    // Intentionally reverse the order that MainWindow uses. If the crash
    // only happens here (not in the test above), the bug is in the
    // MainWindow::on_tab_close_request guard ordering.
    for (int iter = 0; iter < 20; ++iter) {
        ThreadPoolDispatcher pool(2);
        auto v1 = std::make_unique<DocumentView>(open_search_fixture(), pool);
        auto v2 = std::make_unique<DocumentView>(open_search_fixture(), pool);

        CrossTabSearch xts;
        xts.dispatch(L"Lorem", SearchSession::Flags{},
                     {CrossTabSearch::TabRef{v1.get(), L"s1"},
                      CrossTabSearch::TabRef{v2.get(), L"s2"}});

        // NO clear() — simulate a code path that forgets it.
        v1.reset();
        std::this_thread::sleep_for(50ms);
        // xts still holds chained observers pointing to v1 (now destroyed)
        // but v1's SearchSession is also gone, so session->on_update is
        // gone too. Observer can't fire on a dead session.
        v2.reset();
    }
}
