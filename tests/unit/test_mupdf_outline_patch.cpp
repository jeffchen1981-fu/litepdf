// #229: litepdf carries one local MuPDF change,
// third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch. After an
// HTML-engine document (FB2 here) is laid out, MuPDF 1.27.2 resolved every
// outline entry with its own walk of the whole box tree, so a book with N
// section titles cost O(N^2). The patch indexes the tree once and falls back
// to the original walk for any value the index cannot give exactly.
//
// These tests pin three things (spec:
// docs/superpowers/specs/2026-11-12-fb2-outline-one-pass-229-design.md, section 6):
// - every outline entry still gets exactly the position the original walk
//   gives, which fz_resolve_link_dest still computes (the patch leaves
//   htdoc_resolve_link alone), so it is the reference;
// - an allocation failure while building the index falls back instead of
//   failing the page count, and leaves no error code behind (#146);
// - the patched library is the one linked, and the page count grows with
//   the book rather than with the square of its titles.
//
// Every book is generated here, from a fixed word list and a fixed-seed LCG,
// so it is the same on every run.
#include "core/MuPdfPatchNotice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

namespace {

constexpr const char* kNotice =
    "Modified by litepdf on 2026-11-12: one-pass outline resolve (#229)";

// ---- Book text ----------------------------------------------------------

constexpr const char* kWords[20] = {
    "amber", "brook", "cedar", "drift", "ember", "fable", "grove", "harbor",
    "island", "juniper", "kettle", "lantern", "meadow", "nectar", "orchard",
    "pebble", "quarry", "river", "saddle", "timber"};

struct Lcg {
    std::uint32_t state;
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

std::string words(Lcg& rng, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        if (i) out += ' ';
        out += kWords[rng.next() % 20];
    }
    return out;
}

std::string id_attr(const std::string& id) {
    return id.empty() ? std::string() : " id=\"" + id + "\"";
}

// A paragraph of 60 words, the density of the books measured in #229.
std::string para(Lcg& rng, const std::string& id = {}) {
    return "<p" + id_attr(id) + ">" + words(rng, 60) + "</p>\n";
}

std::string title(const std::string& text, const std::string& id = {}) {
    return "<title" + id_attr(id) + "><p>" + text + "</p></title>\n";
}

std::string fb2(const std::string& body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
           "xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n"
           "<description><title-info><genre>prose</genre>"
           "<book-title>Outline fixture</book-title><lang>en</lang>"
           "</title-info></description>\n<body>\n" +
           body + "</body>\n</FictionBook>\n";
}

// N sections, each a title and 10 paragraphs: the shape of #229's books.
std::string plain_book(int sections) {
    Lcg rng{169};
    std::string body;
    for (int s = 1; s <= sections; ++s) {
        body += "<section>" + title("Chapter " + std::to_string(s));
        for (int p = 0; p < 10; ++p) body += para(rng);
        body += "</section>\n";
    }
    return fb2(body);
}

// ---- MuPDF plumbing -----------------------------------------------------

// Counts allocation calls; once armed, fails call fail_from and every later
// one. Failing only one call is not enough: on a NULL return MuPDF scavenges
// its store and asks again (memory.c, do_scavenging_malloc).
struct AllocState {
    int calls = 0;
    int armed = 0;
    int fail_from = 0;
};

void* counting_malloc(void* user, size_t size) {
    auto* state = static_cast<AllocState*>(user);
    ++state->calls;
    if (state->armed && state->calls >= state->fail_from) return nullptr;
    return std::malloc(size);
}

void* counting_realloc(void* user, void* old, size_t size) {
    auto* state = static_cast<AllocState*>(user);
    ++state->calls;
    if (state->armed && state->calls >= state->fail_from) return nullptr;
    return std::realloc(old, size);
}

void counting_free(void*, void* ptr) { std::free(ptr); }

struct Context {
    fz_context* ctx = nullptr;
    explicit Context(fz_alloc_context* alloc = nullptr) {
        ctx = fz_new_context(alloc, nullptr, FZ_STORE_DEFAULT);
        REQUIRE(ctx != nullptr);
        fz_register_document_handlers(ctx);
    }
    ~Context() { fz_drop_context(ctx); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};

void silence(fz_context* ctx) {
    fz_set_error_callback(ctx, [](void*, const char*) noexcept {}, nullptr);
    fz_set_warning_callback(ctx, [](void*, const char*) noexcept {}, nullptr);
}

struct Doc {
    fz_context* ctx;
    fz_document* doc = nullptr;
    // Opens `text` as an FB2. The caller keeps ownership of the stream
    // (document.h), so it is dropped here; the document has read it all.
    Doc(fz_context* c, const std::string& text) : ctx(c) {
        const auto* data = reinterpret_cast<const unsigned char*>(text.data());
        const size_t size = text.size();
        fz_stream* stm = nullptr;
        fz_document* opened = nullptr;
        fz_var(stm);
        fz_var(opened);
        fz_try(ctx) {
            stm = fz_open_memory(ctx, data, size);
            opened = fz_open_document_with_stream(ctx, "application/x-fictionbook", stm);
        }
        fz_always(ctx) {
            fz_drop_stream(ctx, stm);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            opened = nullptr;
        }
        doc = opened;
        REQUIRE(doc != nullptr);
    }
    ~Doc() { fz_drop_document(ctx, doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

struct CountResult {
    int pages;
    int threw;
    int stale;  // the error code left on the context, read inside the try
};

// fz_count_pages lays the book out (and, in MuPDF's html engine, updates the
// outline). The error code is read inside the try body, right after the
// call: the try's own fz_catch runs fz_do_catch even when nothing was
// thrown, which resets the code (context.h, error.c), so a read after the
// try could not see a catch inside fz_count_pages that forgot to clear it.
CountResult count_pages(fz_context* ctx, fz_document* doc) {
    int pages = 0;
    int threw = 0;
    int stale = 0;
    fz_var(pages);
    fz_var(threw);
    fz_var(stale);
    fz_try(ctx) {
        pages = fz_count_pages(ctx, doc);
        stale = fz_caught(ctx);
    }
    fz_catch(ctx) {
        threw = 1;
        fz_report_error(ctx);
    }
    return {pages, threw, stale};
}

fz_outline* load_outline(fz_context* ctx, fz_document* doc) {
    fz_outline* outline = nullptr;
    fz_var(outline);
    fz_try(ctx) {
        outline = fz_load_outline(ctx, doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        outline = nullptr;
    }
    return outline;
}

fz_link_dest resolve(fz_context* ctx, fz_document* doc, const char* uri) {
    fz_link_dest dest = fz_make_link_dest_none();
    fz_try(ctx) {
        dest = fz_resolve_link_dest(ctx, doc, uri);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        dest = fz_make_link_dest_none();
    }
    return dest;
}

int resolve_page(fz_context* ctx, fz_document* doc, const std::string& id) {
    return resolve(ctx, doc, ("#" + id).c_str()).loc.page;
}

// Both sides compute the same expression, so equal floats are bitwise
// equal; a "none" destination has NaN coordinates (link.c).
void require_same(float got, float want) {
    if (std::isnan(got) || std::isnan(want)) {
        REQUIRE(std::isnan(got));
        REQUIRE(std::isnan(want));
    } else {
        REQUIRE(got == want);
    }
}

void check_nodes(fz_context* ctx, fz_document* doc, fz_outline* node, int& count) {
    for (; node; node = node->next) {
        ++count;
        REQUIRE(node->uri != nullptr);
        const fz_link_dest want = resolve(ctx, doc, node->uri);
        INFO("outline uri " << node->uri);
        REQUIRE(node->page.chapter == want.loc.chapter);
        REQUIRE(node->page.page == want.loc.page);
        require_same(node->x, want.x);
        require_same(node->y, want.y);
        check_nodes(ctx, doc, node->down, count);
    }
}

// Every outline node's position equals what the original root walk gives
// for its uri. Returns the number of nodes.
int require_outline_matches_root_walk(fz_context* ctx, fz_document* doc) {
    fz_outline* outline = load_outline(ctx, doc);
    int count = 0;
    check_nodes(ctx, doc, outline, count);
    fz_drop_outline(ctx, outline);
    return count;
}

int max_outline_page(fz_outline* node) {
    int best = -1;
    for (; node; node = node->next) {
        if (node->page.page > best) best = node->page.page;
        const int down = max_outline_page(node->down);
        if (down > best) best = down;
    }
    return best;
}

std::string pool_id(int n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "id-%03d", n);
    return buf;
}

// The 20 duplicated ids sort in the middle of the pool ids: "id-005d" falls
// between "id-005" and "id-006".
std::string dup_id(int k) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "id-%03dd", k * 10 + 5);
    return buf;
}

}  // namespace

TEST_CASE("MuPDF outline patch: notice symbol is linked", "[mupdf][outline]") {
    REQUIRE(std::string(litepdf::core::mupdf_patch_notice()) == kNotice);
}

TEST_CASE("MuPDF outline patch: outline matches the root walk", "[mupdf][outline]") {
    // Spec 6.1. In FB2 markup the headings are title and subtitle, and only
    // inside a section (html-parse.c, get_heading_from_tag; h1-h6 would be
    // headings too, but FB2's default CSS makes them no block, so they get
    // no heading level); only headings become outline nodes.
    Lcg rng{229};
    std::string body;
    int headings = 0;
    int pool = 0;
    const std::string long_id = "long-" + std::string(60, 'L');
    for (int s = 0; s < 24; ++s) {
        body += "<section>";
        if (s == 0) {
            body += title("Long id", long_id);
        } else if (s == 1) {
            // An empty heading: no flow content, so its position is the
            // box's own layout y (find_first_content returns NULL).
            body += "<title id=\"empty-title\"/>\n";
        } else if (s >= 4) {
            // Repeats the id of a paragraph two sections back: the first
            // match in the tree is that paragraph, so this entry opens there.
            body += title("Repeats an id", dup_id(s - 4));
            body += para(rng, "after-" + dup_id(s - 4));
        } else {
            body += title("Chapter " + std::to_string(s));  // generated 'N id
        }
        ++headings;
        for (int p = 0; p < 8; ++p) body += para(rng, pool_id(pool++));
        if (s >= 2 && s <= 21) body += para(rng, dup_id(s - 2));
        if (s % 3 == 0) {
            body += "<section>" + title("Part " + std::to_string(s) + ".1") +
                    "<subtitle><p>A subtitle</p></subtitle>\n" + para(rng) + para(rng) +
                    "<section>" + title("Part " + std::to_string(s) + ".1.1") + para(rng) +
                    "</section></section>\n";
            headings += 3;
        }
        body += "</section>\n";
    }
    const std::string text = fb2(body);

    Context c;
    Doc d(c.ctx, text);
    const CountResult r = count_pages(c.ctx, d.doc);
    REQUIRE(r.threw == 0);
    REQUIRE(r.pages > 1);

    // The fixture is what it claims to be.
    fz_outline* outline = load_outline(c.ctx, d.doc);
    REQUIRE(outline != nullptr);
    REQUIRE(max_outline_page(outline) > 0);
    fz_drop_outline(c.ctx, outline);
    for (int k = 0; k < 20; ++k) {
        INFO("duplicated id " << dup_id(k));
        REQUIRE(resolve_page(c.ctx, d.doc, dup_id(k)) <
                resolve_page(c.ctx, d.doc, "after-" + dup_id(k)));
    }

    REQUIRE(require_outline_matches_root_walk(c.ctx, d.doc) == headings);

    // A second copy on the same context, as each GUI render worker opens.
    Doc d2(c.ctx, text);
    REQUIRE(count_pages(c.ctx, d2.doc).threw == 0);
    REQUIRE(require_outline_matches_root_walk(c.ctx, d2.doc) == headings);
}

TEST_CASE("MuPDF outline patch: book without titles opens", "[mupdf][outline]") {
    auto book = [](int ids) {
        Lcg rng{7};
        std::string body;
        for (int s = 0; s < 10; ++s) {
            body += "<section>";
            for (int p = 0; p < 10; ++p) {
                const int n = s * 10 + p;
                body += para(rng, n < ids ? pool_id(n) : std::string());
            }
            body += "</section>\n";
        }
        return fb2(body);
    };
    AllocState state;
    fz_alloc_context alloc{&state, counting_malloc, counting_realloc, counting_free};
    Context c(&alloc);

    {
        Doc d(c.ctx, book(0));
        REQUIRE(load_outline(c.ctx, d.doc) == nullptr);
        const CountResult r = count_pages(c.ctx, d.doc);  // also the warm-up
        REQUIRE(r.threw == 0);
        REQUIRE(r.pages > 0);
    }

    // With an empty outline nothing is indexed: 100 ids cost nothing at
    // layout, and an index would add two allocations per id.
    auto calls_for = [&](int ids) {
        Doc d(c.ctx, book(ids));
        state.calls = 0;
        REQUIRE(count_pages(c.ctx, d.doc).threw == 0);
        return state.calls;
    };
    const int without_ids = calls_for(0);
    const int with_ids = calls_for(100);
    INFO("allocation calls: " << without_ids << " without ids, " << with_ids << " with");
    REQUIRE(with_ids == without_ids);
}

TEST_CASE("MuPDF outline patch: negative first match falls back to the root walk",
          "[mupdf][outline]") {
    // Spec 6.2. MuPDF folds a first child's negative top margin into its
    // parent unless the child has its own top padding or border, and folds
    // an empty box's top margin into its bottom unless it has padding or a
    // border (html-layout.c), so the book's only subtitle gets a top padding;
    // a selector on every title would move them all. The subtitle is empty:
    // a heading with text resolves to its first line, and MuPDF places a
    // line that would start above the page at the page's top, so only a
    // heading without text keeps the box's own negative y. Everything after
    // the subtitle moves up by the same 200 pt.
    Lcg rng{31};
    std::string body = "<section><subtitle id=\"neg\"/>\n";
    for (int p = 0; p < 30; ++p) body += para(rng);
    // The repeat sits in the next section: find_box_target returns an own-id
    // match, negative or not, for the whole sibling list it is in, and only
    // a parent skips a negative result and goes on.
    body += "</section>\n<section>" + para(rng, "neg") + para(rng, "after-neg");
    body += "</section>\n";
    for (int s = 0; s < 3; ++s) {
        body += "<section>" + title("Chapter " + std::to_string(s));
        for (int p = 0; p < 5; ++p) body += para(rng);
        body += "</section>\n";
    }

    Context c;
    fz_set_user_css(c.ctx, "subtitle{padding-top:1px;margin-top:-200px}");
    Doc d(c.ctx, fb2(body));
    REQUIRE(count_pages(c.ctx, d.doc).threw == 0);

    // The root walk skipped the subtitle's negative position and went on to
    // the paragraph 30 paragraphs later. If the CSS failed to put the
    // subtitle above the page, "#neg" would resolve to page 0.
    const int neg_page = resolve_page(c.ctx, d.doc, "neg");
    INFO("#neg resolves to page " << neg_page);    REQUIRE(neg_page >= 3);
    REQUIRE(neg_page == resolve_page(c.ctx, d.doc, "after-neg"));

    REQUIRE(require_outline_matches_root_walk(c.ctx, d.doc) == 4);
}

TEST_CASE("MuPDF outline patch: allocation failure in the index falls back",
          "[mupdf][outline]") {
    // Spec 6.5. The index is the last thing fz_count_pages allocates, two
    // allocations (node, key copy) per distinct id. This book has 4
    // generated ids (titles without one) and 3 paragraph ids.
    Lcg rng{5};
    std::string body;
    for (int s = 0; s < 4; ++s) {
        body += "<section>" + title("Chapter " + std::to_string(s));
        body += para(rng, s < 3 ? pool_id(s) : std::string());
        body += para(rng);
        body += "</section>\n";
    }
    const std::string text = fb2(body);
    const int distinct_ids = 7;

    AllocState state;
    fz_alloc_context alloc{&state, counting_malloc, counting_realloc, counting_free};
    Context c(&alloc);
    silence(c.ctx);

    {
        Doc warm(c.ctx, text);
        REQUIRE(count_pages(c.ctx, warm.doc).threw == 0);
    }
    auto measure = [&] {
        Doc d(c.ctx, text);
        state.calls = 0;
        REQUIRE(count_pages(c.ctx, d.doc).threw == 0);
        return state.calls;
    };
    const int n = measure();
    REQUIRE(measure() == n);
    const int first_index_call = n - 2 * distinct_ids + 1;
    INFO("allocation calls in fz_count_pages: " << n);
    REQUIRE(first_index_call > 1);

    for (int k = first_index_call; k <= n; ++k) {
        Doc d(c.ctx, text);
        state.calls = 0;
        state.fail_from = k;
        state.armed = 1;
        const CountResult r = count_pages(c.ctx, d.doc);
        state.armed = 0;
        INFO("failing allocation " << k << " of " << n);
        REQUIRE(r.threw == 0);
        REQUIRE(r.stale == 0);
        REQUIRE(require_outline_matches_root_walk(c.ctx, d.doc) == 4);
    }

    // Earlier calls are the layout's own: the page count may fail, as it
    // does without the patch, but nothing crashes.
    for (int k = 1; k < first_index_call; ++k) {
        Doc d(c.ctx, text);
        state.calls = 0;
        state.fail_from = k;
        state.armed = 1;
        (void)count_pages(c.ctx, d.doc);
        state.armed = 0;
    }
}

TEST_CASE("MuPDF outline patch: page count grows with the book, not its titles",
          "[mupdf][outline]") {
    // Spec 6.3: a speed guard, not the stale-library oracle (the notice
    // symbol is). Thread cycles on one pinned core, so the P-core / E-core
    // split on this machine cannot move the ratio.
    const DWORD_PTR previous =
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << GetCurrentProcessorNumber());
    REQUIRE(previous != 0);

    Context c;
    auto min_cycles = [&](int sections) {
        const std::string text = plain_book(sections);
        ULONG64 best = ~ULONG64{0};
        for (int run = 0; run < 3; ++run) {
            Doc d(c.ctx, text);
            ULONG64 before = 0;
            ULONG64 after = 0;
            QueryThreadCycleTime(GetCurrentThread(), &before);
            const CountResult r = count_pages(c.ctx, d.doc);
            QueryThreadCycleTime(GetCurrentThread(), &after);
            REQUIRE(r.threw == 0);
            if (after - before < best) best = after - before;
        }
        return best;
    };
    const ULONG64 c200 = min_cycles(200);
    const ULONG64 c800 = min_cycles(800);
    SetThreadAffinityMask(GetCurrentThread(), previous);

    const double ratio = static_cast<double>(c800) / static_cast<double>(c200);
    INFO("cycles: 200 sections " << c200 << ", 800 sections " << c800 << ", ratio " << ratio);
    REQUIRE(ratio < 12.0);
}
