// #61: a cloned fz_context must stay safe to drop after the Document it came
// from has been destroyed.
//
// fz_clone_context copies the whole context, including the lock callbacks whose
// `user` pointer names the Document's lock table. MuPDF keeps its own master
// context alive as a husk until the last clone dies, but it knows nothing about
// OUR table -- so a bare clone dropped after its Document calls fz_lock through
// freed memory. core::EscrowContext holds the table alive and releases it only
// after it has dropped its own context.
//
// The table count is what makes this test discriminating: release-mode heap
// reuse would let a use-after-free "pass" silently, but a table freed with its
// Document shows up as a count that dropped too early.
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>

// The unit-test target has no MuPDF include path (litepdf_core links MuPDF
// privately). fz_drop_context is plain extern "C", so a local declaration is
// enough -- the same approach as test_document_clone_context.cpp.
extern "C" {
struct fz_context;
void fz_drop_context(fz_context* ctx);
}

using litepdf::core::Document;
using litepdf::core::EscrowContext;
using litepdf::core::detail::live_lock_tables;

TEST_CASE("EscrowContext is empty for a null source", "[core][escrow]") {
    EscrowContext escrow = EscrowContext::clone_from(nullptr);
    REQUIRE_FALSE(escrow.valid());
    REQUIRE(escrow.get() == nullptr);
}

TEST_CASE("EscrowContext keeps the lock table alive after its Document is destroyed",
          "[core][escrow]") {
    const std::size_t before = live_lock_tables();
    EscrowContext escrow;
    {
        Document doc;
        REQUIRE(live_lock_tables() == before + 1);
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

        // The render path clones its escrow from a WORKER context, which is
        // itself a clone of the Document's. Mirror that.
        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);
        escrow = EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(escrow.valid());
    }

    // The Document is gone. Its lock table must not be.
    REQUIRE(live_lock_tables() == before + 1);

    // Dropping the escrow drops its context through the table, then the table.
    escrow = EscrowContext{};
    REQUIRE(live_lock_tables() == before);
}

TEST_CASE("EscrowContext move transfers ownership exactly once", "[core][escrow]") {
    const std::size_t before = live_lock_tables();
    {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);

        EscrowContext a = EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(a.valid());
        const fz_context* raw = a.get();

        EscrowContext b = std::move(a);
        REQUIRE_FALSE(a.valid());
        REQUIRE(b.get() == raw);

        EscrowContext c;
        c = std::move(b);
        REQUIRE_FALSE(b.valid());
        REQUIRE(c.get() == raw);
    }
    REQUIRE(live_lock_tables() == before);
}
