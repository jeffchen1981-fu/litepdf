#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <utility>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/EscrowContext.hpp"
#include "ui/detail/ViewportMath.hpp"

// The unit-test target has no MuPDF include path (litepdf_core links MuPDF
// privately). fz_drop_context is plain extern "C", so a local declaration is
// enough -- the same approach as test_escrow_context.cpp.
extern "C" {
struct fz_context;
void fz_drop_context(fz_context* ctx);
}

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::ui::bitmap_px_to_dip;
using litepdf::ui::pdf_point_to_dip;

namespace {

Document open_simple() {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    return doc;
}

// Shared inline dispatcher for all DocumentView unit tests. Each test
// gets its own local one in practice (below) to keep tasks isolated;
// this free function reduces the boilerplate. Keeping it non-static so
// each TU that includes this file is fine — it's only used here.
}  // namespace

TEST_CASE("DocumentView constructs from opened Document", "[core][view][ctor]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    REQUIRE(view.page_count() > 0);
    REQUIRE(view.current_page() == 0);
    // Default zoom mode is FitWidth -- page width fills the canvas, and the
    // overflow below the fold is reached with the wheel or the arrow keys.
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE(view.ui_ctx() != nullptr);
    // source_path should round-trip through the move.
    REQUIRE(view.source_path().filename() == "simple.pdf");
    // Phase 6: search() accessor returns a valid session reference.
    (void)view.search();
}

TEST_CASE("DocumentView throws when given an unopened Document",
          "[core][view][ctor]") {
    InlineDispatcher disp;
    Document doc;  // never opened
    REQUIRE_THROWS_AS(DocumentView(std::move(doc), disp), std::runtime_error);
}

TEST_CASE("DocumentView::set_current_page clamps and signals change",
          "[core][view][page]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    REQUIRE(view.current_page() == 0);

    // Same-index, negative, and out-of-range inputs all clamp to [0, last].
    REQUIRE_FALSE(view.set_current_page(0));
    REQUIRE_FALSE(view.set_current_page(-5));
    REQUIRE(view.current_page() == 0);

    const int last = view.page_count() - 1;
    if (last > 0) {
        REQUIRE(view.set_current_page(last));
        REQUIRE(view.current_page() == last);
        REQUIRE_FALSE(view.set_current_page(9999));
        REQUIRE(view.current_page() == last);
    } else {
        // simple.pdf is 1-page; clamp to 0 for any request.
        REQUIRE_FALSE(view.set_current_page(9999));
        REQUIRE(view.current_page() == 0);
    }
}

TEST_CASE("DocumentView FitWidth derives a percentage from viewport pixels",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // 1190.44 px / 595.22 pt = exactly 2.0 at 96 dpi.
    view.set_viewport(1190.44f, 800.0f, 96.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::FitWidth);
    REQUIRE(view.zoom_pct()     == Catch::Approx(2.0f).epsilon(0.001));
    REQUIRE(view.render_scale() == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("DocumentView render scale carries the dpi factor",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // The same physical viewport at 192 dpi: each DIP is two device pixels, so
    // the percentage halves while the render scale -- and the pixmap width in
    // pixels -- is unchanged.
    view.set_viewport(1190.44f, 800.0f, 192.0f);
    REQUIRE(view.zoom_pct()     == Catch::Approx(1.0f).epsilon(0.001));
    REQUIRE(view.render_scale() == Catch::Approx(2.0f).epsilon(0.001));
}

TEST_CASE("DocumentView FitPage recomputes from the stored viewport",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    view.set_viewport(1190.44f, 842.0f, 96.0f);
    REQUIRE(view.zoom_pct() == Catch::Approx(2.0f).epsilon(0.001));
    // Same viewport, fit-page: height binds at exactly 1.0.
    view.set_zoom_mode_fit_page();
    REQUIRE(view.zoom_pct() == Catch::Approx(1.0f).epsilon(0.001));
}

// Spec S5 regression: pdf_point_to_dip(pt, zoom_pct()) -- the overlay path --
// must agree with bitmap_px_to_dip(pt * render_scale(), dpi) -- the pixmap
// path -- at every dpi, for a fixed zoom_pct_. This is the identity the whole
// overlay path rests on, and it is the reversed-unit error's regression test:
// exercised through a real DocumentView (not just the pure ViewportMath /
// ZoomMath functions) so it also pins DocumentView::render_scale() itself.
TEST_CASE("DocumentView quad mapping to dip is dpi invariant",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    // Custom, set explicitly -- not the constructor default -- so zoom_pct_
    // stays fixed across the viewport/dpi changes below instead of being
    // re-derived by a fit mode.
    view.set_zoom_pct(1.5f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);

    const float pt = 100.0f;  // an arbitrary PDF-point coordinate
    for (const float dpi : {96.0f, 144.0f, 192.0f}) {
        // Custom freezes zoom_pct_ but set_viewport still stores the new dpi,
        // which render_scale() depends on.
        view.set_viewport(800.0f, 600.0f, dpi);
        REQUIRE(view.zoom_pct() == Catch::Approx(1.5f));

        const float via_overlay = pdf_point_to_dip(pt, view.zoom_pct());
        const float via_pixmap  = bitmap_px_to_dip(pt * view.render_scale(), dpi);
        REQUIRE(via_overlay == Catch::Approx(via_pixmap));
    }
}

TEST_CASE("DocumentView zoom ladder walks the extended preset table",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_pct(1.0f);
    int up_steps = 0;
    while (view.zoom_in()) {
        ++up_steps;
        REQUIRE(up_steps < 32);
    }
    REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE_FALSE(view.zoom_in());

    int down_steps = 0;
    while (view.zoom_out()) {
        ++down_steps;
        REQUIRE(down_steps < 32);
    }
    REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    REQUIRE_FALSE(view.zoom_out());
}

TEST_CASE("DocumentView zoom in works above the old preset ceiling",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_mode_fit_width();
    // A wide canvas against a narrow page derives a percentage above 4.0
    // (3000 / 595.22 = 5.04) -- exactly the state in which the shipped
    // zoom_in() could never find a larger rung and silently did nothing.
    view.set_viewport(3000.0f, 900.0f, 96.0f);
    REQUIRE(view.zoom_pct() > 4.0f);
    REQUIRE(view.zoom_in());
    REQUIRE(view.zoom_pct() == Catch::Approx(6.0f));
}

TEST_CASE("DocumentView set_zoom_pct clamps to the extended span",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    view.set_zoom_pct(99.0f);  REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
    view.set_zoom_pct(0.01f);  REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    view.set_zoom_pct(0.25f);  REQUIRE(view.zoom_pct() == Catch::Approx(0.25f));
    view.set_zoom_pct(8.0f);   REQUIRE(view.zoom_pct() == Catch::Approx(8.0f));
}

// This pins the two lines that make zoom STICK, which is the whole point of
// PR-A1: set_zoom_pct's `zm = Custom` assignment, and set_viewport's early
// return while the mode is Custom. Without the early return, the very next
// set_viewport -- kick_render calls one through apply_viewport on every render,
// resize and DPI change -- would re-derive the fit percentage and silently
// throw the user's zoom away, which is exactly the defect this PR exists to
// fix. Both lines were previously uncovered: deleting either left the suite
// green.
TEST_CASE("DocumentView Custom zoom survives a later set_viewport",
          "[core][view][zoom]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);
    // Start from a fit mode with a known viewport, so the assertion below
    // cannot pass by the fit happening to agree with the custom value.
    view.set_zoom_mode_fit_width();
    view.set_viewport(1190.44f, 800.0f, 96.0f);
    REQUIRE(view.zoom_pct() == Catch::Approx(2.0f).epsilon(0.001));

    view.set_zoom_pct(3.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE(view.zoom_pct() == Catch::Approx(3.0f));

    // A different viewport: the fit would be 1.0 here (595.22 px / 595.22 pt),
    // so a re-derivation is unmissable.
    view.set_viewport(595.22f, 400.0f, 96.0f);
    REQUIRE(view.zoom_mode() == DocumentView::ZoomMode::Custom);
    REQUIRE(view.zoom_pct() == Catch::Approx(3.0f));

    // The viewport IS stored even while frozen -- leaving Custom must fit the
    // dimensions just handed in, not the stale ones from before the zoom.
    view.set_zoom_mode_fit_width();
    REQUIRE(view.zoom_pct() == Catch::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("DocumentView set_viewport fits the larger page of a spread pair",
          "[core][view][zoom]") {
    // spread-unequal.pdf: page 0 is 420x595, page 1 is 595x842. This is the
    // test that fails if set_viewport ignores pair_page -- the ZoomMath case
    // computes max() in the test body and so cannot detect that.
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/spread-unequal.pdf").has_value());
    InlineDispatcher disp;
    DocumentView view(std::move(doc), disp);
    view.set_zoom_mode_fit_page();

    const float slot_px = 600.0f, slot_h_px = 900.0f;

    // Without the pair: derived from page 0 (420x595) alone.
    // min(600/420, 900/595) = 1.42857. At that scale page 1 (595x842) is
    // 850 x 1203 -- overflowing its slot on BOTH axes. Compare width against
    // slot width and height against slot height; crossing them silently
    // asserts nothing.
    view.set_viewport(slot_px, slot_h_px, 96.0f);
    const float solo = view.zoom_pct();
    REQUIRE(solo == Catch::Approx(1.42857f).epsilon(0.001));
    REQUIRE(595.0f * solo > slot_px);        // page 1 too wide for its slot
    REQUIRE(842.0f * solo > slot_h_px);      // and too tall

    // With the pair: derived from max(420,595) x max(595,842).
    view.set_viewport(slot_px, slot_h_px, 96.0f, /*pair_page=*/1);
    const float paired = view.zoom_pct();
    REQUIRE(paired < solo);
    REQUIRE(420.0f * paired <= Catch::Approx(slot_px));     // page 0 width
    REQUIRE(595.0f * paired <= Catch::Approx(slot_h_px));   // page 0 height
    REQUIRE(595.0f * paired <= Catch::Approx(slot_px));     // page 1 width
    REQUIRE(842.0f * paired <= Catch::Approx(slot_h_px));   // page 1 height

    // An out-of-range pair index is ignored, not clamped into a wrong page.
    view.set_viewport(slot_px, slot_h_px, 96.0f, /*pair_page=*/99);
    REQUIRE(view.zoom_pct() == Catch::Approx(solo));
}

TEST_CASE("DocumentView selection persists across a page change until cleared",
          "[core][view][selection]") {
    InlineDispatcher disp;
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/search.pdf").has_value());
    DocumentView view(std::move(doc), disp);
    REQUIRE_FALSE(view.selection().has_value());

    litepdf::core::TextSelection sel;
    sel.page      = 0;
    sel.anchor    = { 10.0f, 20.0f };
    sel.extent    = { 200.0f, 40.0f };
    sel.text_utf8 = "Lorem";
    view.set_selection(sel);

    // Spec §2, model 1b: a page change does not clear it.
    REQUIRE(view.set_current_page(3));
    REQUIRE(view.selection().has_value());
    REQUIRE(view.selection()->page == 0);
    REQUIRE(view.selection()->text_utf8 == "Lorem");

    view.clear_selection();
    REQUIRE_FALSE(view.selection().has_value());
}

TEST_CASE("DocumentView set_selection replaces the previous selection",
          "[core][view][selection]") {
    InlineDispatcher disp;
    DocumentView view(open_simple(), disp);

    litepdf::core::TextSelection first;
    first.page      = 0;
    first.text_utf8 = "first";
    view.set_selection(first);

    litepdf::core::TextSelection second;
    second.page      = 0;
    second.text_utf8 = "second";
    view.set_selection(second);

    REQUIRE(view.selection()->text_utf8 == "second");
}

// #74: DocumentView clones ui_ctx and cache_ctx before it builds PageCache,
// RenderEngine and SearchSession. Only ~DocumentView used to drop those two
// clones, and ~DocumentView never runs for a half-built object -- so a throw
// from any of the three leaked both.
//
// num_workers == 0 is the deterministic seam: RenderEngine's ctor rejects it
// with std::invalid_argument, after the clones exist and after PageCache has
// been built on cache_ctx.
//
// The escrow is the oracle. It holds the family's root alive past the
// Document's death, so family_context_count keeps reporting after the throw:
// root + escrow == 2 when the clones were dropped, 4 when they leaked.
TEST_CASE("DocumentView drops its context clones when a member ctor throws",
          "[core][view][ctor]") {
    InlineDispatcher disp;
    litepdf::core::EscrowContext probe;
    {
        Document doc;
        REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

        fz_context* worker = doc.clone_context();
        REQUIRE(worker != nullptr);
        probe = litepdf::core::EscrowContext::clone_from(worker);
        fz_drop_context(worker);
        REQUIRE(probe.valid());
        REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == 2);

        REQUIRE_THROWS_AS(DocumentView(std::move(doc), disp, 0),
                          std::invalid_argument);
    }

    REQUIRE(litepdf::core::detail::family_context_count(probe.get()) == 2);
}
