#include <catch2/catch_test_macros.hpp>
#include "core/SessionState.hpp"

using namespace litepdf::core;

TEST_CASE("to_json emits a backslash-escaped Windows path", "[core][session][json]") {
    SessionState s;
    s.window = {0, 1, 100, 80, 1024, 768};
    s.active_tab = 0;
    s.tabs.push_back({std::filesystem::path(L"C:\\docs\\a b.pdf"), 3,
                      SessionZoom::Custom, 1.25f});

    std::string j = to_json(s);
    REQUIRE(j.find("C:\\\\docs\\\\a b.pdf") != std::string::npos);
    REQUIRE(j.find("\"page\":3") != std::string::npos);
    REQUIRE(j.find("\"zoom_mode\":\"custom\"") != std::string::npos);
    // 1.25f is a power-of-two-divisible float, but assert it round-trips
    // exactly through %.9g (no trailing 1.2500000x noise).
    REQUIRE(j.find("\"zoom_scale\":1.25") != std::string::npos);
}

TEST_CASE("to_json emits empty tabs array", "[core][session][json]") {
    SessionState s;  // default-constructed: no tabs
    std::string j = to_json(s);
    REQUIRE(j.find("\"tabs\":[]") != std::string::npos);
}

TEST_CASE("to_json maps each fit zoom mode to its string", "[core][session][json]") {
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"a.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    s.tabs.push_back({std::filesystem::path(L"b.pdf"), 0,
                      SessionZoom::FitPage, 1.0f});

    std::string j = to_json(s);
    REQUIRE(j.find("\"zoom_mode\":\"fit_width\"") != std::string::npos);
    REQUIRE(j.find("\"zoom_mode\":\"fit_page\"") != std::string::npos);
}

TEST_CASE("from_json round-trips a two-tab session", "[core][session][json]") {
    SessionState s;
    s.version = 1;
    s.window = {0, 3, 10, 20, 1280, 800};
    s.active_tab = 1;
    s.tabs.push_back({std::filesystem::path(L"C:\\a\\one.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    s.tabs.push_back({std::filesystem::path(L"C:\\b\\two.pdf"), 7,
                      SessionZoom::Custom, 1.5f});

    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->active_tab == 1);
    REQUIRE(r->window.w == 1280);
    REQUIRE(r->tabs.size() == 2);
    REQUIRE(r->tabs[0].path == std::filesystem::path(L"C:\\a\\one.pdf"));
    REQUIRE(r->tabs[1].page == 7);
    REQUIRE(r->tabs[1].zoom_mode == SessionZoom::Custom);
    REQUIRE(r->tabs[1].zoom_scale == 1.5f);
}

TEST_CASE("from_json round-trips a non-exact float zoom", "[core][session][json]") {
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\z.pdf"), 0, SessionZoom::Custom, 1.3f});
    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->tabs[0].zoom_scale == 1.3f);   // exact thanks to %.9g
}

TEST_CASE("from_json round-trips a CJK and a UNC path", "[core][session][json]") {
    SessionState s;
    s.tabs.push_back({std::filesystem::path(L"C:\\文件\\檔案.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    s.tabs.push_back({std::filesystem::path(L"\\\\server\\share\\b.pdf"), 0,
                      SessionZoom::FitWidth, 1.0f});
    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->tabs[0].path == std::filesystem::path(L"C:\\文件\\檔案.pdf"));
    REQUIRE(r->tabs[1].path == std::filesystem::path(L"\\\\server\\share\\b.pdf"));
}

TEST_CASE("from_json decodes a uXXXX BMP escape to UTF-8", "[core][session][json]") {
    // A foreign writer may escape CJK codepoints as \uXXXX; we must decode them, not truncate.
    // Raw literal => the backslash reaches from_json literally.
    auto r = from_json(R"({"version":1,"window":{"flags":0,"show":1,"x":0,"y":0,"w":0,"h":0},"active":0,"tabs":[{"path":"C:\\中.pdf","page":0,"zoom_mode":"fit_width","zoom_scale":1}]})");
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.size() == 1);
    REQUIRE(r->tabs[0].path == std::filesystem::path(L"C:\\中.pdf"));   // U+4E2D 中
}

TEST_CASE("from_json rejects a lone surrogate escape", "[core][session][json]") {
    auto r = from_json(R"({"version":1,"window":{"flags":0,"show":1,"x":0,"y":0,"w":0,"h":0},"active":0,"tabs":[{"path":"\ud800.pdf","page":0,"zoom_mode":"fit_width","zoom_scale":1}]})");
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("from_json rejects malformed / unsupported input as nullopt", "[core][session][json]") {
    REQUIRE_FALSE(from_json("").has_value());
    REQUIRE_FALSE(from_json("{").has_value());
    REQUIRE_FALSE(from_json("not json at all").has_value());
    REQUIRE_FALSE(from_json("{\"version\":1,\"tabs\":[{").has_value());
    REQUIRE_FALSE(from_json("{}garbage").has_value());                 // trailing junk
    REQUIRE_FALSE(from_json("{\"version\":2,\"tabs\":[]}").has_value()); // unsupported version
    REQUIRE_FALSE(from_json("{\"version\":1,\"bogus\":1,\"tabs\":[]}").has_value()); // unknown key
}

TEST_CASE("from_json tolerates an empty-tabs session", "[core][session][json]") {
    SessionState s;  // zero tabs
    auto r = from_json(to_json(s));
    REQUIRE(r.has_value());
    REQUIRE(r->tabs.empty());
}
