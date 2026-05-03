// Phase 8 Task 1: pure-logic unit tests for the 3-attempt retry loop.
// No Win32 modal is involved — prompt/auth callbacks are injected.

#include "ui/password_retry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using litepdf::ui::try_authenticate_with_retry;

TEST_CASE("password_retry: succeeds on first attempt", "[password_retry]") {
    int prompt_calls = 0;
    auto r = try_authenticate_with_retry(
        [&](const std::wstring&) { ++prompt_calls; return std::optional<std::string>{"correct"}; },
        [](const std::string& s) { return s == "correct"; },
        [](int)                  { return std::wstring{}; });
    REQUIRE(r.accepted);
    REQUIRE(r.attempts == 1);
    REQUIRE(prompt_calls == 1);
}

TEST_CASE("password_retry: fails first, succeeds on second", "[password_retry]") {
    std::vector<std::wstring> statuses_seen;
    int call = 0;
    auto r = try_authenticate_with_retry(
        [&](const std::wstring& s) {
            statuses_seen.push_back(s);
            return std::optional<std::string>{++call == 1 ? "wrong" : "correct"};
        },
        [](const std::string& s) { return s == "correct"; },
        [](int n)                { return L"fail-status-" + std::to_wstring(n); });
    REQUIRE(r.accepted);
    REQUIRE(r.attempts == 2);
    REQUIRE(statuses_seen.size() == 2);
    REQUIRE(statuses_seen[0].empty());                  // first prompt: no status yet
    REQUIRE(statuses_seen[1] == L"fail-status-1");      // after 1 failure
}

TEST_CASE("password_retry: 3 wrong attempts exhausts", "[password_retry]") {
    int prompts = 0;
    auto r = try_authenticate_with_retry(
        [&](const std::wstring&) { ++prompts; return std::optional<std::string>{"x"}; },
        [](const std::string&)   { return false; },
        [](int)                  { return L"err"; });
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.attempts == 3);
    REQUIRE(prompts == 3);
}

TEST_CASE("password_retry: cancel returns immediately, no auth call",
          "[password_retry]") {
    int auth_calls = 0;
    auto r = try_authenticate_with_retry(
        [](const std::wstring&)        { return std::optional<std::string>{}; },
        [&](const std::string&)        { ++auth_calls; return false; },
        [](int)                        { return L"err"; });
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.attempts == 0);
    REQUIRE(auth_calls == 0);
}
