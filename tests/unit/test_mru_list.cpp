#include "core/MruList.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

using namespace litepdf::core;

// Use a disposable registry key so tests don't pollute real MRU.
static constexpr wchar_t kTestKey[] = L"SOFTWARE\\LitePDF\\TestMRU";

// Helper: delete the test key to start clean.
static void delete_test_key() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kTestKey);
}

TEST_CASE("MruList starts empty", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    REQUIRE(mru.entries().empty());
}

TEST_CASE("MruList push adds entry and persists via save/load", "[mru]") {
    delete_test_key();
    {
        MruList mru(kTestKey);
        mru.load();
        mru.push(L"C:\\docs\\a.pdf");
        mru.push(L"C:\\docs\\b.pdf");
        mru.save();
    }
    {
        MruList mru2(kTestKey);
        mru2.load();
        auto e = mru2.entries();
        REQUIRE(e.size() == 2);
        // Most recent first
        REQUIRE(e[0] == L"C:\\docs\\b.pdf");
        REQUIRE(e[1] == L"C:\\docs\\a.pdf");
    }
    delete_test_key();
}

TEST_CASE("MruList push moves duplicate to front", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    mru.push(L"C:\\a.pdf");
    mru.push(L"C:\\b.pdf");
    mru.push(L"C:\\a.pdf");  // should move to front, not duplicate
    REQUIRE(mru.entries().size() == 2);
    REQUIRE(mru.entries()[0] == L"C:\\a.pdf");
    REQUIRE(mru.entries()[1] == L"C:\\b.pdf");
    delete_test_key();
}

TEST_CASE("MruList caps at 10 entries", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    for (int i = 0; i < 15; ++i) {
        mru.push(L"C:\\file" + std::to_wstring(i) + L".pdf");
    }
    REQUIRE(mru.entries().size() == 10);
    // Most recent (file14) is first
    REQUIRE(mru.entries()[0] == L"C:\\file14.pdf");
    delete_test_key();
}

TEST_CASE("MruList remove erases by index", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    mru.push(L"C:\\a.pdf");
    mru.push(L"C:\\b.pdf");
    mru.push(L"C:\\c.pdf");
    mru.remove(1);  // remove b.pdf (index 1 in [c, b, a])
    auto e = mru.entries();
    REQUIRE(e.size() == 2);
    REQUIRE(e[0] == L"C:\\c.pdf");
    REQUIRE(e[1] == L"C:\\a.pdf");
    delete_test_key();
}
