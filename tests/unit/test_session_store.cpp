#include <catch2/catch_test_macros.hpp>
#include "core/SessionStore.hpp"
#include <filesystem>
#include <fstream>

using namespace litepdf::core;

static std::filesystem::path temp_session() {
    auto p = std::filesystem::temp_directory_path() / L"litepdf_test_session.json";
    std::error_code ec; std::filesystem::remove(p, ec);
    return p;
}

TEST_CASE("save then load round-trips", "[core][session][store]") {
    auto file = temp_session();
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\x\\y.pdf"), 4, SessionZoom::FitPage, 1.0f});

    REQUIRE(save_session(file, s));
    auto r = load_session(file);
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.size() == 1);
    REQUIRE(r->tabs[0].page == 4);

    std::error_code ec; std::filesystem::remove(file, ec);
}

TEST_CASE("load of a missing file is nullopt", "[core][session][store]") {
    auto file = std::filesystem::temp_directory_path() / L"litepdf_does_not_exist.json";
    std::error_code ec; std::filesystem::remove(file, ec);
    REQUIRE_FALSE(load_session(file).has_value());
}

TEST_CASE("load rejects an oversized file", "[core][session][store]") {
    auto file = temp_session();
    { std::ofstream o(file, std::ios::binary); std::string big(kMaxSessionBytes + 1, 'x'); o.write(big.data(), big.size()); }
    REQUIRE_FALSE(load_session(file).has_value());
    std::error_code ec; std::filesystem::remove(file, ec);
}

TEST_CASE("save leaves the prior file intact when it cannot replace", "[core][session][store]") {
    // Make the destination a directory so MoveFileExW fails; the existing
    // good content must survive (here: the directory stays, no data clobbered).
    auto dir = std::filesystem::temp_directory_path() / L"litepdf_locked.json";
    std::error_code ec; std::filesystem::create_directory(dir, ec);
    SessionState s;
    REQUIRE_FALSE(save_session(dir, s));   // cannot overwrite a directory
    REQUIRE(std::filesystem::is_directory(dir, ec));
    std::filesystem::remove(dir, ec);
}
