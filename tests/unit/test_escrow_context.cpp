// #61: a cloned fz_context must stay safe to drop after the Document it came
// from has been destroyed.
//
// fz_clone_context copies the whole context, including the lock callbacks whose
// `user` pointer names the Document's lock table. MuPDF keeps its own master
// context alive as a husk until the last clone dies, but it knows nothing about
// OUR table -- so a bare clone dropped after its Document calls fz_lock through
// freed memory. core::EscrowContext holds the table alive and releases it only
// after it has dropped its own context. The same holder owns the root context
// the family was cloned from, for the same reason one step further in: MuPDF
// frees this family's ICC profiles through the context that created them.
//
// The holder count is what makes these tests discriminating: release-mode heap
// reuse would let a use-after-free "pass" silently, but a holder freed with its
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
using litepdf::core::detail::live_mupdf_roots;
using litepdf::core::detail::root_contexts_dropped;

TEST_CASE("EscrowContext is empty for a null source", "[core][escrow]") {
    EscrowContext escrow = EscrowContext::clone_from(nullptr);
    REQUIRE_FALSE(escrow.valid());
    REQUIRE(escrow.get() == nullptr);
}

TEST_CASE("EscrowContext keeps the lock table alive after its Document is destroyed",
          "[core][escrow]") {
    const std::size_t before = live_mupdf_roots();
    EscrowContext escrow;
    {
        Document doc;
        REQUIRE(live_mupdf_roots() == before + 1);
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
    REQUIRE(live_mupdf_roots() == before + 1);

    // Dropping the escrow drops its context through the table, then the table.
    escrow = EscrowContext{};
    REQUIRE(live_mupdf_roots() == before);
}

// What this case CANNOT catch: a second drop of the root that happens AFTER the
// family count is read below -- two drops inside ~MuPDFRoot, say. A second drop
// before that point never reaches the count either: the first one leaves the
// root a husk with a null `master` (fz_drop_context's delayed-free branch), so
// the second faults where fz_drop_context decrements `ctx->master->context_count`.
// Reproducing a double free deterministically needs a poisoning
// allocator -- running the suite under a debugger enables the NT debug heap,
// which is how the original crash was made 15/15 reproducible.
TEST_CASE("EscrowContext keeps the root context alive until the last escrow dies",
          "[core][escrow]") {
    const std::size_t before_roots = live_mupdf_roots();
    const std::size_t before_drops = root_contexts_dropped();
    EscrowContext escrow;
    {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);
        escrow = EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(escrow.valid());
    }

    // The Document is gone, but its root context must NOT have been dropped:
    // MuPDF tears the shared colorspace context down in whichever context dies
    // last, and LCMS frees the ICC profiles through the context that created
    // them -- the root. Dropping the root first leaves that pointer dangling.
    REQUIRE(live_mupdf_roots() == before_roots + 1);

    // The root context is still alive: MuPDF still counts it in this family
    // alongside the escrow's clone. A root dropped with its Document -- the
    // defect this test exists for -- leaves 1 here, and leaves the ICC
    // teardown pointing at freed memory.
    REQUIRE(litepdf::core::detail::family_context_count(escrow.get()) == 2);

    escrow = EscrowContext{};
    REQUIRE(live_mupdf_roots() == before_roots);
    REQUIRE(root_contexts_dropped() == before_drops + 1);
}

TEST_CASE("EscrowContext move transfers ownership exactly once", "[core][escrow]") {
    const std::size_t before = live_mupdf_roots();
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
    REQUIRE(live_mupdf_roots() == before);
}
