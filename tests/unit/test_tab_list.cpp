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
