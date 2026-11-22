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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
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
using litepdf::test_kit::BoundedRun;
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

namespace {

// The whole text of page `p`, through the Document's own (gated) text_page.
std::string page_text_copy(const Document& doc, std::size_t p) {
    const Document::TextPage t = doc.text_page(p);
    if (!t.valid()) return "<invalid>";
    litepdf::core::SelPoint first{}, last{};
    if (!t.full_range(first, last)) return "";
    return t.copy(first, last);
}

bool same_quad(const Document::PageHit& a, const Document::PageHit& b) {
    return a.ul_x == b.ul_x && a.ul_y == b.ul_y && a.ur_x == b.ur_x && a.ur_y == b.ur_y &&
           a.ll_x == b.ll_x && a.ll_y == b.ll_y && a.lr_x == b.lr_x && a.lr_y == b.lr_y;
}

}  // namespace

// C1 (spec §6.4): a search, a render of every page and UI-thread text builds,
// all at once on one shared document, give exactly what each gives alone. The
// baselines come from the same document through the same code paths, so a
// second parse cannot differ: any difference is interference.
TEST_CASE("SharedDocument: results under contention equal the single-threaded results",
          "[core][shared][contention]") {
    Document doc = open_fb2();
    const std::size_t pages = doc.page_count();
    REQUIRE(pages > 1);
    RenderEngine eng(doc, 1, nullptr, std::nullopt, DocumentShare::Shared);

    // 1. Baselines, nothing else running.
    std::vector<std::shared_ptr<RenderSlot>> base_px(pages);
    for (std::size_t p = 0; p < pages; ++p) {
        base_px[p] = std::make_shared<RenderSlot>();
        eng.submit(render_into(static_cast<int>(p), 0.5f, base_px[p]));
        REQUIRE(base_px[p]->wait());
        REQUIRE(base_px[p]->non_null);
    }
    std::vector<std::vector<Document::PageHit>> base_hits(pages);
    std::vector<std::string> base_text(pages);
    for (std::size_t p = 0; p < pages; ++p) {
        base_hits[p] = doc.page_hits(p, "Lorem", Document::SearchFlags{});
        // The session orders a page's hits top to bottom, stably (#139).
        std::stable_sort(base_hits[p].begin(), base_hits[p].end(),
                         [](const Document::PageHit& a, const Document::PageHit& b) {
                             return a.ul_y < b.ul_y;
                         });
        base_text[p] = page_text_copy(doc, p);
    }

    // 2. Contention, all at once.
    ThreadPoolDispatcher pool(2);
    SearchSession s(doc, pool, std::nullopt, DocumentShare::Shared);
    s.set_query(L"Lorem", SearchSession::Flags{});
    std::vector<std::shared_ptr<RenderSlot>> busy_px(pages);
    for (std::size_t p = 0; p < pages; ++p) {
        busy_px[p] = std::make_shared<RenderSlot>();
        eng.submit(render_into(static_cast<int>(p), 0.5f, busy_px[p]));
    }
    const auto all_rendered = [&] {
        return std::all_of(busy_px.begin(), busy_px.end(),
                           [](const std::shared_ptr<RenderSlot>& x) { return x->is_done(); });
    };
    std::mt19937 rng(229);
    std::uniform_int_distribution<std::size_t> pick(0, pages - 1);
    std::vector<std::pair<std::size_t, std::string>> busy_text;
    // Text builds that started while the scan or a render was still running:
    // without at least one, the text half of this case compares nothing.
    std::size_t contended_text = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!(s.scan_complete() && all_rendered()) &&
           std::chrono::steady_clock::now() < deadline) {
        const std::size_t p = pick(rng);
        if (!(s.scan_complete() && all_rendered())) ++contended_text;
        busy_text.emplace_back(p, page_text_copy(doc, p));
    }
    REQUIRE(s.scan_complete());
    REQUIRE(all_rendered());
    CAPTURE(busy_text.size());
    REQUIRE(contended_text > 0);

    // 3. Check.
    for (std::size_t p = 0; p < pages; ++p) {
        CAPTURE(p);
        const auto hits = s.hits_for_page(p);
        REQUIRE(hits.size() == base_hits[p].size());
        for (std::size_t i = 0; i < hits.size(); ++i) {
            CHECK(same_quad(hits[i].geom, base_hits[p][i]));
        }
        REQUIRE(busy_px[p]->non_null);
        CHECK(busy_px[p]->w == base_px[p]->w);
        CHECK(busy_px[p]->h == base_px[p]->h);
        CHECK(busy_px[p]->stride == base_px[p]->stride);
        CHECK(busy_px[p]->bytes == base_px[p]->bytes);
    }
    for (const auto& [p, text] : busy_text) {
        CAPTURE(p);
        CHECK(text == base_text[p]);
    }
}

// Review Focus 2: a print that overlaps a render prints what a quiet print
// prints. A helper holds the gate as a Render borrower while a shared engine
// has a render queued, so the print queues behind a Render hold and ahead of a
// Render waiter; it then calls back with the gate released.
TEST_CASE("SharedDocument: a print during renders equals a quiet print",
          "[core][shared][contention]") {
    auto doc = std::make_shared<Document>(open_fb2());

    struct Printed {
        bool ok = false;
        int  w  = 0;
        int  h  = 0;
        std::vector<std::uint8_t> bytes;
    };
    const auto print_into = [](const Document& d, Printed& out) {
        out.ok = d.with_rendered_page(
            0, 2.0f, 2.0f, false, [&out](int w, int h, const std::uint8_t* bgra) {
                out.w = w;
                out.h = h;
                out.bytes.assign(bgra, bgra + static_cast<std::size_t>(w) * h * 4);
            });
    };
    Printed quiet;
    print_into(*doc, quiet);
    REQUIRE(quiet.ok);

    Gate   latch;
    Events progress;
    auto   slot = std::make_shared<RenderSlot>();
    RenderEngine eng(*doc, 1, nullptr, std::nullopt, DocumentShare::Shared);
    JoinAll   threads;
    GateGuard guard(latch);

    threads.add(std::thread([&] {
        SharedDocumentAccess::with_document(*doc, GateClass::Render, [&](fz_document*) {
            progress.record("held");
            latch.wait();
        });
    }));
    REQUIRE(progress.wait_for("held"));
    eng.submit(render_into(1, 0.5f, slot));
    auto& gate = SharedDocumentAccess::gate(*doc);
    REQUIRE(poll_until([&] { return gate.waiting(GateClass::Render) == 1; }));

    auto busy = std::make_shared<Printed>();
    auto run  = BoundedRun::start([doc, busy, print_into] { print_into(*doc, *busy); });
    REQUIRE(poll_until([&] { return gate.waiting(GateClass::Foreground) == 1; }));
    latch.open();
    REQUIRE(run->wait_for(std::chrono::seconds(10)));

    CHECK(busy->ok);
    CHECK(busy->w == quiet.w);
    CHECK(busy->h == quiet.h);
    CHECK(busy->bytes == quiet.bytes);
    REQUIRE(slot->wait());
    CHECK(slot->non_null);
}
