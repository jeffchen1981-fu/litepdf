// #55 PR 2 Task 5: DocumentView reports the L1 limits it was constructed with,
// so the strip scheduler reads them from the view instead of hard-coding them
// (spec 5.2: "l1_capacity - |V| entries free and l1_budget - bytes(V) bytes free").

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <utility>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/PageCache.hpp"

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;

namespace {
constexpr const char* kFixture = "tests/fixtures/selection.pdf";
}  // namespace

TEST_CASE("DocumentView: l1 limits report the constructor values", "[core][view][strip]") {
    InlineDispatcher disp;

    SECTION("explicit capacity and budget") {
        Document doc;
        REQUIRE_FALSE(doc.open(kFixture).has_value());
        const std::size_t budget = std::size_t{64} << 20;
        DocumentView view(std::move(doc), disp, std::nullopt,
                          /*workers*/ 1, /*l1*/ 3, /*l2*/ 10, budget);
        CHECK(view.l1_capacity() == 3);
        CHECK(view.l1_budget_bytes() == budget);
    }

    SECTION("defaults") {
        Document doc;
        REQUIRE_FALSE(doc.open(kFixture).has_value());
        DocumentView view(std::move(doc), disp);
        CHECK(view.l1_capacity() == 5);
        CHECK(view.l1_budget_bytes() == litepdf::core::kDefaultL1BudgetBytes);
    }
}
