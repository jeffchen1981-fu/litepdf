#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

// Forward-declare the bits of the MuPDF C ABI we need here. The unit-tests
// target does not get MuPDF's include path (litepdf_core links mupdf PRIVATE),
// but fz_drop_context is a plain extern-C function so a local declaration is
// enough to exercise clone_context's return value at the raw context level.
extern "C" {
struct fz_context;
void fz_drop_context(fz_context* ctx);
}

using namespace litepdf::core;

TEST_CASE("Document::clone_context returns distinct fz_context per call",
          "[core][document][clone]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    fz_context* c1 = doc.clone_context();
    fz_context* c2 = doc.clone_context();
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);
    REQUIRE(c1 != c2);
    fz_drop_context(c1);
    fz_drop_context(c2);
}

TEST_CASE("Document::clone_context on un-opened doc returns nullptr",
          "[core][document][clone]") {
    Document doc;
    REQUIRE(doc.clone_context() == nullptr);
}
