#include "core/TabList.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace litepdf::core;

namespace {
// Test helper: build a Tab with just a label; DocumentView is left null
// because TabList is pure-logic and does not dereference view.
std::unique_ptr<Tab> make_tab(std::wstring label) {
    auto t = std::make_unique<Tab>();
    t->label = std::move(label);
    return t;
}
}  // namespace

TEST_CASE("TabList starts empty", "[tablist]") {
    TabList list;
    REQUIRE(list.size() == 0);
    REQUIRE(list.empty());
    REQUIRE(list.active_index() == -1);
    REQUIRE(list.active() == nullptr);
}

TEST_CASE("TabList add appends and returns index", "[tablist]") {
    TabList list;
    REQUIRE(list.add(make_tab(L"a")) == 0);
    REQUIRE(list.add(make_tab(L"b")) == 1);
    REQUIRE(list.add(make_tab(L"c")) == 2);
    REQUIRE(list.size() == 3);
    // add() does NOT auto-activate (caller decides).
    REQUIRE(list.active_index() == -1);
}

TEST_CASE("TabList set_active updates active pointer", "[tablist]") {
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    REQUIRE(list.set_active(1));
    REQUIRE(list.active_index() == 1);
    REQUIRE(list.active()->label == L"b");
    REQUIRE_FALSE(list.set_active(5));  // out of range
    REQUIRE(list.active_index() == 1);  // unchanged
}

TEST_CASE("TabList remove: right-neighbor activation", "[tablist]") {
    // A, B, C — active=1 (B). Remove B → active should be C (now at idx 1).
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    list.add(make_tab(L"c"));
    list.set_active(1);
    int new_active = list.remove(1);
    REQUIRE(new_active == 1);
    REQUIRE(list.size() == 2);
    REQUIRE(list.active()->label == L"c");
}

TEST_CASE("TabList remove last: left-neighbor activation", "[tablist]") {
    // A, B — active=1 (B, the last). Remove B → active should be A.
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    list.set_active(1);
    int new_active = list.remove(1);
    REQUIRE(new_active == 0);
    REQUIRE(list.active()->label == L"a");
}

TEST_CASE("TabList remove non-active shifts indices", "[tablist]") {
    // A, B, C — active=2 (C). Remove A → C is now at idx 1, active should
    // stay on C.
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    list.add(make_tab(L"c"));
    list.set_active(2);
    int new_active = list.remove(0);
    REQUIRE(new_active == 1);
    REQUIRE(list.active()->label == L"c");
}

TEST_CASE("TabList remove last remaining tab resets active", "[tablist]") {
    TabList list;
    list.add(make_tab(L"solo"));
    list.set_active(0);
    int new_active = list.remove(0);
    REQUIRE(new_active == -1);
    REQUIRE(list.empty());
    REQUIRE(list.active() == nullptr);
}

TEST_CASE("TabList at() bounds-checks", "[tablist]") {
    TabList list;
    list.add(make_tab(L"a"));
    REQUIRE(list.at(0) != nullptr);
    REQUIRE(list.at(1) == nullptr);  // one past end
    REQUIRE(list.at(std::size_t(-1)) == nullptr);  // wrap-around
}

TEST_CASE("TabList remove without prior set_active leaves active at -1", "[tablist]") {
    // Regression: when active_ == -1 and remove() is called on a non-empty
    // list, the signed/unsigned compare must NOT decrement active_ below
    // -1. Without the guard, active_ underflowed to -2 here.
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    REQUIRE(list.active_index() == -1);   // add() does not auto-activate
    int new_active = list.remove(0);
    REQUIRE(new_active == -1);
    REQUIRE(list.active_index() == -1);
    REQUIRE(list.size() == 1);
    REQUIRE(list.active() == nullptr);
}

// Regression coverage for the Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+1..9
// arithmetic. These helpers are the single source of truth that both
// the main window's WM_COMMAND dispatch and the canvas's defensive
// Ctrl+Tab forwarder rely on, so the mapping must be exercised even
// though the surrounding Win32 message routing is not.
TEST_CASE("next_tab_index wraps forward and no-ops on tiny lists", "[tab_nav]") {
    REQUIRE(next_tab_index(0, 2) == 1);
    REQUIRE(next_tab_index(1, 2) == 0);        // wrap
    REQUIRE(next_tab_index(2, 3) == 0);        // wrap
    REQUIRE(next_tab_index(0, 1) == -1);       // only one tab -> no-op
    REQUIRE(next_tab_index(-1, 3) == -1);      // no active -> no-op
    REQUIRE(next_tab_index(0, 0) == -1);       // empty list
}

TEST_CASE("prev_tab_index wraps backward and no-ops on tiny lists", "[tab_nav]") {
    REQUIRE(prev_tab_index(1, 2) == 0);
    REQUIRE(prev_tab_index(0, 2) == 1);        // wrap
    REQUIRE(prev_tab_index(0, 3) == 2);        // wrap
    REQUIRE(prev_tab_index(0, 1) == -1);
    REQUIRE(prev_tab_index(-1, 3) == -1);
    REQUIRE(prev_tab_index(0, 0) == -1);
}

TEST_CASE("goto_tab_index maps 1-indexed shortcut to 0-indexed slot", "[tab_nav]") {
    REQUIRE(goto_tab_index(1, 3) == 0);
    REQUIRE(goto_tab_index(2, 3) == 1);
    REQUIRE(goto_tab_index(3, 3) == 2);
    REQUIRE(goto_tab_index(4, 3) == -1);       // out of range
    REQUIRE(goto_tab_index(0, 3) == -1);       // below range
    REQUIRE(goto_tab_index(1, 0) == -1);       // empty list
}
