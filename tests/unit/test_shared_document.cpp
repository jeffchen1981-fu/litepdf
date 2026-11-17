// #229: an FB2 tab's render worker and search session borrow the Document's own
// document through the gate instead of each opening a copy (spec §3.2-3.5,
// tests §6.1, §6.3, §6.4). On tests/fixtures/strip-chapters.fb2.
//
// Lifetimes in every case: the Document first, then whatever a borrower's hook
// or callback reaches, then the engine or session, then the JoinAll and the
// guards. The guards open every latch first, the threads are joined, the
// borrowers finish, and only then does the Document die.

#include <catch2/catch_test_macros.hpp>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentShare.hpp"
#include "core/EscrowContext.hpp"
#include "core/RenderEngine.hpp"
#include "core/SearchSession.hpp"
#include "core/detail/PriorityGate.hpp"
#include "core/detail/SharedDocumentAccess.hpp"
#include "gate_test_kit.hpp"
#include "search_test_kit.hpp"

#include <mupdf/fitz.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

using litepdf::app::InlineDispatcher;
using litepdf::app::ThreadPoolDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentShare;
using litepdf::core::EscrowContext;
using litepdf::core::RenderEngine;
using litepdf::core::SearchSession;
using litepdf::core::detail::GateClass;
using litepdf::core::detail::SharedDocumentAccess;
using litepdf::test_kit::Events;
using litepdf::test_kit::finishes_within;
using litepdf::test_kit::Gate;
using litepdf::test_kit::GateGuard;
using litepdf::test_kit::JoinAll;
using litepdf::test_kit::poll_until;

namespace {

constexpr const char* kFb2 = "tests/fixtures/strip-chapters.fb2";

Document open_fb2() {
    Document doc;
    REQUIRE_FALSE(doc.open(std::filesystem::path(kFb2)).has_value());
    REQUIRE(doc.is_fb2());
    return doc;
}

// One render's outcome, filled on the worker and read on the test thread.
struct RenderSlot {
    std::mutex                 m;
    std::condition_variable    cv;
    bool                       done     = false;
    bool                       non_null = false;
    int                        w        = 0;
    int                        h        = 0;
    std::ptrdiff_t             stride   = 0;
    std::vector<unsigned char> bytes;

    bool is_done() {
        std::lock_guard<std::mutex> g(m);
        return done;
    }
    bool wait(std::chrono::milliseconds limit = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, limit, [this] { return done; });
    }
};

// A render request whose callback copies the pixmap into `slot` and drops it.
RenderEngine::RenderRequest render_into(int page, float scale, std::shared_ptr<RenderSlot> slot) {
    RenderEngine::RenderRequest r;
    r.page_num = page;
    r.priority = 0;
    r.scale    = scale;
    r.on_complete = [slot](fz_pixmap* pix, fz_context* ctx) {
        std::lock_guard<std::mutex> g(slot->m);
        if (pix) {
            slot->w      = fz_pixmap_width(ctx, pix);
            slot->h      = fz_pixmap_height(ctx, pix);
            slot->stride = fz_pixmap_stride(ctx, pix);
            const unsigned char* s = fz_pixmap_samples(ctx, pix);
            slot->bytes.assign(s, s + slot->stride * slot->h);
            fz_drop_pixmap(ctx, pix);
        }
        slot->non_null = pix != nullptr;
        slot->done     = true;
        slot->cv.notify_all();
    };
    return r;
}

// Live pages on the document's open-page list, read under a Foreground hold. A
// dropped page stays listed, marked dead (page->doc == NULL), until the next
// load reaps it (fz_reap_dead_pages), so "the list is empty" is not the check.
int live_pages(const Document& doc) {
    int live = -1;
    SharedDocumentAccess::with_document(doc, GateClass::Foreground, [&](fz_document* d) {
        live = 0;
        for (fz_page* p = d->open; p; p = p->next) {
            if (p->doc) ++live;
        }
    });
    return live;
}

// Parks every borrower of class `cls` in the after-release hook until the test
// lets that park go: park k waits until release_one() has been called k+1
// times, so each park blocks on its own turn (a one-shot latch would let every
// later park through). release_all() opens every park, present and future.
class Parker {
public:
    explicit Parker(GateClass cls) : cls_(cls) {}

    static void hook(void* self, GateClass c) { static_cast<Parker*>(self)->park(c); }

    bool wait_parked(int n, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, limit, [&] { return parks_ >= n; });
    }
    void release_one() {
        {
            std::lock_guard<std::mutex> g(m_);
            ++released_;
        }
        cv_.notify_all();
    }
    void release_all() {
        {
            std::lock_guard<std::mutex> g(m_);
            open_all_ = true;
        }
        cv_.notify_all();
    }

private:
    void park(GateClass c) {
        if (c != cls_) return;
        std::unique_lock<std::mutex> lk(m_);
        const int k = parks_++;
        cv_.notify_all();
        cv_.wait(lk, [&] { return open_all_ || released_ > k; });
    }

    GateClass               cls_;
    std::mutex              m_;
    std::condition_variable cv_;
    int                     parks_    = 0;
    int                     released_ = 0;
    bool                    open_all_ = false;
};

// Every hit of `needle` in `doc`, page by page, through the Document's own
// (gated) page_hits: the single-threaded answer a scan must equal.
std::size_t total_hits(const Document& doc, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = 0; p < doc.page_count(); ++p) {
        n += doc.page_hits(p, needle, Document::SearchFlags{}).size();
    }
    return n;
}

// Opens every park when the scope ends, so a failed REQUIRE never leaves a
// borrower parked while its engine or session is being destroyed.
struct ReleaseAll {
    Parker& p;
    ~ReleaseAll() { p.release_all(); }
};

}  // namespace

// S4, engine half.
TEST_CASE("RenderEngine: a shared engine opens no document", "[core][shared]") {
    Document doc = open_fb2();
    {
        RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);
        CHECK(eng.documents_opened() == 0);
        auto slot = std::make_shared<RenderSlot>();
        eng.submit(render_into(0, 0.5f, slot));
        REQUIRE(slot->wait());
        CHECK(slot->non_null);
    }
    {
        RenderEngine own(doc, 2);
        CHECK(own.documents_opened() == 2);
    }
}

// B1: the worker's display-list build waits for the gate.
TEST_CASE("RenderEngine: a shared worker waits for the gate before building",
          "[core][shared]") {
    Document doc = open_fb2();
    Gate   latch;
    Events progress;
    auto   slot = std::make_shared<RenderSlot>();
    RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);
    JoinAll   threads;
    GateGuard guard(latch);

    threads.add(std::thread([&] {
        SharedDocumentAccess::with_document(doc, GateClass::Foreground, [&](fz_document*) {
            progress.record("held");
            latch.wait();
        });
    }));
    REQUIRE(progress.wait_for("held"));

    eng.submit(render_into(0, 0.5f, slot));
    REQUIRE(poll_until(
        [&] { return SharedDocumentAccess::gate(doc).waiting(GateClass::Render) == 1; }));
    CHECK_FALSE(slot->is_done());

    latch.open();
    REQUIRE(slot->wait(std::chrono::seconds(5)));
    CHECK(slot->non_null);
}

// B3: every page a borrower loads is dropped inside its hold. The after-release
// hook parks the borrower right after the gate is released and before
// with_document returns: a page kept past the gate is still live there.
TEST_CASE("SharedDocument: no live page is left past the gate", "[core][shared]") {
    Document doc = open_fb2();

    SECTION("render build") {
        Parker parker(GateClass::Render);
        SharedDocumentAccess::set_after_release_hook(doc, &Parker::hook, &parker);
        auto slot = std::make_shared<RenderSlot>();
        RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);
        ReleaseAll release{parker};

        eng.submit(render_into(0, 0.5f, slot));
        REQUIRE(parker.wait_parked(1));
        CHECK(live_pages(doc) == 0);
        parker.release_one();
        REQUIRE(slot->wait());
        CHECK(slot->non_null);
    }

    SECTION("text build") {
        Parker parker(GateClass::Render);
        SharedDocumentAccess::set_after_release_hook(doc, &Parker::hook, &parker);
        auto slot   = std::make_shared<RenderSlot>();
        auto wanted = std::make_shared<std::atomic<int>>(0);
        auto texts  = std::make_shared<Events>();
        RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);
        ReleaseAll release{parker};

        auto req = render_into(0, 0.5f, slot);
        req.text_wanted = wanted;
        req.on_text = [texts](Document::TextPage t) {
            texts->record(t.valid() ? "valid" : "empty");
        };
        eng.submit(std::move(req));

        // Park 1: the display list. Park 2: the text.
        REQUIRE(parker.wait_parked(1));
        CHECK(live_pages(doc) == 0);
        parker.release_one();
        REQUIRE(parker.wait_parked(2));
        CHECK(live_pages(doc) == 0);
        parker.release_one();
        REQUIRE(texts->wait_for("valid", std::chrono::seconds(10)));
        CHECK(slot->non_null);
    }

    SECTION("scan") {
        Parker parker(GateClass::Scan);
        SharedDocumentAccess::set_after_release_hook(doc, &Parker::hook, &parker);
        ThreadPoolDispatcher pool(1);
        SearchSession s(doc, pool, std::nullopt, DocumentShare::Shared);
        ReleaseAll release{parker};

        s.set_query(L"Lorem", SearchSession::Flags{});
        REQUIRE(parker.wait_parked(1));
        CHECK(live_pages(doc) == 0);
        parker.release_all();
        CHECK(poll_until([&] { return s.scan_complete(); }, std::chrono::seconds(10)));
    }
}

// Review Focus 3: a page that fails to load inside the hold is caught there,
// and the gate is released for the next page.
TEST_CASE("RenderEngine: a shared worker's failed page releases the gate", "[core][shared]") {
    Document doc = open_fb2();
    RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);

    auto bad = std::make_shared<RenderSlot>();
    eng.submit(render_into(static_cast<int>(doc.page_count()) + 5, 0.5f, bad));
    REQUIRE(bad->wait());
    CHECK_FALSE(bad->non_null);

    auto good = std::make_shared<RenderSlot>();
    eng.submit(render_into(0, 0.5f, good));
    REQUIRE(good->wait());
    CHECK(good->non_null);

    auto& gate = SharedDocumentAccess::gate(doc);
    CHECK(gate.waiting(GateClass::Foreground) == 0);
    CHECK(gate.waiting(GateClass::Render) == 0);
    CHECK(gate.waiting(GateClass::Scan) == 0);
}

// Review Focus 5: a shared engine destroyed with requests still queued gives
// back every context it cloned and leaves no worker on the gate.
TEST_CASE("RenderEngine: a shared engine releases its contexts", "[core][shared]") {
    auto doc = std::make_shared<Document>(open_fb2());

    fz_context* tmp = doc->clone_context();
    REQUIRE(tmp != nullptr);
    EscrowContext probe = EscrowContext::clone_from(tmp);
    fz_drop_context(tmp);
    REQUIRE(probe.valid());
    const int before = litepdf::core::detail::family_context_count(probe.get());

    auto eng = std::make_shared<RenderEngine>(*doc, 1, nullptr, std::nullopt,
                                              DocumentShare::Shared);
    for (int i = 0; i < 20; ++i) {
        RenderEngine::RenderRequest r;
        r.page_num = i % static_cast<int>(doc->page_count());
        r.priority = 1;
        r.scale    = 0.5f;
        r.on_complete = [](fz_pixmap* pix, fz_context* ctx) {
            if (pix) fz_drop_pixmap(ctx, pix);
        };
        eng->submit(std::move(r));
    }
    // The lambda holds the only reference to the engine, and one to the
    // Document, so a destructor that never returns leaves both alive.
    REQUIRE(finishes_within(std::chrono::seconds(10),
                            [eng = std::move(eng), doc]() mutable { eng.reset(); }));
    CHECK(litepdf::core::detail::family_context_count(probe.get()) == before);
}

// S4, session half: a shared session scans the Document's own document and
// finds what the Document itself finds.
TEST_CASE("SearchSession: a shared session opens no document", "[core][shared]") {
    Document doc = open_fb2();
    const std::size_t expected = total_hits(doc, "Lorem");
    REQUIRE(expected > 0);

    InlineDispatcher d;
    {
        SearchSession s(doc, d, std::nullopt, DocumentShare::Shared);
        CHECK(s.documents_opened() == 0);
        s.set_query(L"Lorem", SearchSession::Flags{});
        CHECK(s.scan_complete());
        CHECK(s.hit_count() == expected);
    }
    {
        SearchSession own(doc, d);
        CHECK(own.documents_opened() == 1);
    }
}

// B2: a shared scan waits for the gate before it scans, and publishes nothing
// until it gets it.
TEST_CASE("SearchSession: a shared scan waits for the gate before scanning",
          "[core][shared]") {
    Document doc = open_fb2();
    const std::size_t expected = total_hits(doc, "Lorem");
    Gate   latch;
    Events progress;
    ThreadPoolDispatcher pool(1);
    SearchSession s(doc, pool, std::nullopt, DocumentShare::Shared);
    JoinAll   threads;
    GateGuard guard(latch);

    threads.add(std::thread([&] {
        SharedDocumentAccess::with_document(doc, GateClass::Foreground, [&](fz_document*) {
            progress.record("held");
            latch.wait();
        });
    }));
    REQUIRE(progress.wait_for("held"));

    s.set_query(L"Lorem", SearchSession::Flags{});
    REQUIRE(poll_until(
        [&] { return SharedDocumentAccess::gate(doc).waiting(GateClass::Scan) == 1; }));
    CHECK(s.hit_count() == 0);
    CHECK_FALSE(s.scan_complete());

    latch.open();
    REQUIRE(poll_until([&] { return s.scan_complete(); }, std::chrono::seconds(10)));
    CHECK(s.hit_count() == expected);
}

// Review Focus 4: the family's context count sees a leaked session context; a
// never-scanned shared session and a scanned one must both give theirs back.
TEST_CASE("SearchSession: a shared session releases its context, scanned or not",
          "[core][shared]") {
    Document doc = open_fb2();
    fz_context* tmp = doc.clone_context();
    REQUIRE(tmp != nullptr);
    EscrowContext probe = EscrowContext::clone_from(tmp);
    fz_drop_context(tmp);
    REQUIRE(probe.valid());
    const int before = litepdf::core::detail::family_context_count(probe.get());

    InlineDispatcher d;
    {
        SearchSession s(doc, d, std::nullopt, DocumentShare::Shared);
        REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == before + 1);
    }
    REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == before);
    {
        SearchSession s(doc, d, std::nullopt, DocumentShare::Shared);
        s.set_query(L"Lorem", SearchSession::Flags{});
        REQUIRE(s.hit_count() > 0);
    }
    REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == before);
}
