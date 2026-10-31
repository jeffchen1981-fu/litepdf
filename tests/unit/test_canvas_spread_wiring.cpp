// #196: the spread wiring in PdfCanvas, driven for real. A hidden window holds a
// real canvas over a real DocumentView, and the two render completions are taken
// from the message queue one at a time, so a test decides which half lands first
// instead of racing the workers. The test process is not DPI aware: one device
// pixel is one DIP, on every machine.
//
// tests/fixtures/spread-pairs.pdf at FitWidth in a 1000 wide canvas: the slot is
// 496 DIPs wide and every page 420 pt wide, so the zoom is 496 / 420.
//
//   page | spread | height (pt) | height (DIP)
//     0  | cover  |  595        |  703
//     1  | left   |  400        |  473
//     2  | right  | 1600        | 1890
//     3  | left   | 1600        | 1890
//     4  | right  |  400        |  473

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include <d2d1_1.h>
#include <mupdf/fitz.h>
#include <windows.h>
#include <wrl/client.h>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "ui/PdfCanvas.hpp"
#include "ui/PdfCanvasImpl.hpp"
#include "ui/PdfCanvasLayout.hpp"

using Catch::Approx;
using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::core::SearchSession;
using litepdf::ui::CarriedPosition;
using litepdf::ui::LayoutKind;
using litepdf::ui::PageAnchor;
using litepdf::ui::PdfCanvas;
using litepdf::ui::WM_USER_RENDER_DONE;
using litepdf::ui::WM_USER_RENDER_DONE_RIGHT;
using litepdf::ui::WM_USER_STRIP_DONE;

namespace litepdf::ui {

struct PdfCanvasTestAccess {
    static ID2D1Bitmap* left_bitmap(const PdfCanvas& c) {
        return c.impl_->current_bitmap.Get();
    }
    static ID2D1Bitmap* right_bitmap(const PdfCanvas& c) {
        return c.impl_->right_bitmap.Get();
    }
    static std::vector<SearchSession::Hit> overlay_hits(const PdfCanvas& c) {
        return c.overlay_hits();
    }
    static bool spread_landed(const PdfCanvas& c) { return c.spread_landed(); }
    static void strip_scroll_to(PdfCanvas& c, const CarriedPosition& at) {
        c.impl_->strip_view.apply_carry(at);
    }

    // Paint, then read one pixel back from the render target as 0x00RRGGBB. Reads
    // the target and not the window, which is never shown.
    static std::uint32_t painted_pixel(PdfCanvas& c, int x, int y) {
        using Microsoft::WRL::ComPtr;
        SendMessageW(c.hwnd(), WM_PAINT, 0, 0);
        ID2D1HwndRenderTarget* const rt = c.impl_->rt.Get();
        REQUIRE(rt != nullptr);
        ComPtr<ID2D1DeviceContext> dc;
        REQUIRE(SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc))));
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat   = rt->GetPixelFormat();
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ;
        ComPtr<ID2D1Bitmap1> copy;
        REQUIRE(SUCCEEDED(dc->CreateBitmap(rt->GetPixelSize(), nullptr, 0, props, &copy)));
        REQUIRE(SUCCEEDED(copy->CopyFromRenderTarget(nullptr, rt, nullptr)));
        D2D1_MAPPED_RECT mapped{};
        REQUIRE(SUCCEEDED(copy->Map(D2D1_MAP_OPTIONS_READ, &mapped)));
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            mapped.bits + static_cast<std::size_t>(y) * mapped.pitch);
        const std::uint32_t px = row[x];
        copy->Unmap();
        return px & 0x00FFFFFFu;
    }
};

}  // namespace litepdf::ui

namespace {

using Access = litepdf::ui::PdfCanvasTestAccess;

constexpr int kWidth  = 1000;
constexpr int kHeight = 850;

Document open_pairs() {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/spread-pairs.pdf").has_value());
    return doc;
}

enum class Layout { Spread, Single };

// Which half of a batch fails, and how (#198).
enum class Fail { None, Left, Right, RightBitmap };

// A pixmap no render target can hold: the worker delivers it, CreateBitmap refuses.
fz_pixmap* too_tall_for_a_bitmap(fz_pixmap* rendered, fz_context* ctx) {
    fz_drop_pixmap(ctx, rendered);
    return fz_new_pixmap(ctx, fz_device_bgr(ctx), 1, 1 << 18, nullptr, 1);
}

class Rig {
public:
    explicit Rig(int width = kWidth, int height = kHeight, Layout layout = Layout::Spread)
        : view_(open_pairs(), dispatcher_) {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        parent_ = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, width, height,
                                  nullptr, nullptr, inst, nullptr);
        REQUIRE(parent_ != nullptr);
        canvas_.emplace(inst, parent_);
        canvas_->set_view(&view_);
        canvas_->set_dual_page(layout == Layout::Spread);
        view_.set_dual_page(layout == Layout::Spread);
    }

    ~Rig() {
        canvas_->set_view(nullptr);
        view_.cancel_stale_renders(0);
        MSG m;
        while (PeekMessageW(&m, canvas_->hwnd(), WM_USER_RENDER_DONE, WM_USER_STRIP_DONE,
                            PM_REMOVE)) {
            DispatchMessageW(&m);
        }
        DestroyWindow(parent_);
        canvas_.reset();
    }

    PdfCanvas&    canvas() { return *canvas_; }
    DocumentView& view() { return view_; }

    // MainWindow::kick_render's paged branches: in a spread, one batch for both
    // halves. A failing half asks the worker for a page past the end, so
    // fz_load_page throws there as it does for a damaged page, and the null result
    // is reported as the pair's own page.
    void kick(int page, Fail fail = Fail::None) {
        const HWND target = canvas_->hwnd();
        const std::uint64_t epoch = canvas_->render_epoch();
        const std::uint64_t seq   = canvas_->next_render_seq();
        if (!view_.dual_page()) {
            canvas_->apply_viewport();
            view_.request_render(page, [target, epoch, page, seq](fz_pixmap* p, fz_context* ctx) {
                PdfCanvas::post_render_done(target, p, ctx, epoch, page, seq);
            });
            return;
        }
        const int total = view_.page_count();
        const int left  = litepdf::ui::dual_page_compute_left(page, total);
        const int right = litepdf::ui::dual_page_compute_right(left, total);
        view_.cancel_stale_renders(0);
        canvas_->change_current_page(left);
        canvas_->apply_viewport();
        const int past_end = total + 100;
        view_.request_render(fail == Fail::Left ? past_end : left,
            [target, epoch, left, seq](fz_pixmap* p, fz_context* ctx) {
                PdfCanvas::post_render_done(target, p, ctx, epoch, left, seq);
            });
        if (right >= 0) {
            view_.request_render(fail == Fail::Right ? past_end : right,
                [target, epoch, right, seq, fail](fz_pixmap* p, fz_context* ctx) {
                    if (fail == Fail::RightBitmap) p = too_tall_for_a_bitmap(p, ctx);
                    PdfCanvas::post_render_done_right(target, p, ctx, epoch, right, seq);
                });
        }
    }

    // Hand the canvas one slot's completions until that slot holds a new bitmap.
    // The canvas drops a superseded batch's completions on the way.
    void land_left() {
        land(WM_USER_RENDER_DONE, [this] { return Access::left_bitmap(*canvas_); });
    }
    void land_right() {
        land(WM_USER_RENDER_DONE_RIGHT, [this] { return Access::right_bitmap(*canvas_); });
    }
    void land_both() {
        land_left();
        land_right();
    }

    // Hand the canvas one slot's completions up to and including a failed one.
    void land_left_failure() { land_failure(WM_USER_RENDER_DONE); }
    void land_right_failure() { land_failure(WM_USER_RENDER_DONE_RIGHT); }

    // Hand the canvas one right completion, whatever it brings.
    void land_right_completion() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        MSG m;
        while (!PeekMessageW(&m, canvas_->hwnd(), WM_USER_RENDER_DONE_RIGHT,
                             WM_USER_RENDER_DONE_RIGHT, PM_REMOVE)) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        DispatchMessageW(&m);
    }

    float pan_y() { return canvas_->pan().y; }

    // A tab switch back to this view: neither bitmap counts any more, and the
    // tab's stored pan is restored before its renders land.
    void reenter(float pan_y) {
        canvas_->set_view(&view_);
        canvas_->set_pan(0.0f, pan_y);
    }

private:
    template <class Current>
    void land(UINT msg, Current current) {
        ID2D1Bitmap* const before = current();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (current() == before) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            MSG m;
            if (PeekMessageW(&m, canvas_->hwnd(), msg, msg, PM_REMOVE)) {
                DispatchMessageW(&m);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void land_failure(UINT msg) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            MSG m;
            if (PeekMessageW(&m, canvas_->hwnd(), msg, msg, PM_REMOVE)) {
                const bool failed = m.wParam == 0;
                DispatchMessageW(&m);
                if (failed) return;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    InlineDispatcher         dispatcher_;
    DocumentView             view_;
    HWND                     parent_ = nullptr;
    std::optional<PdfCanvas> canvas_;
};

// An 18 pt high quad, 100 pt in from the page's left edge.
SearchSession::Hit hit_at(int page, float top_pt) {
    SearchSession::Hit h{};
    h.page = static_cast<std::size_t>(page);
    h.geom.ul_x = h.geom.ll_x = 100.0f;
    h.geom.ur_x = h.geom.lr_x = 200.0f;
    h.geom.ul_y = h.geom.ur_y = top_pt;
    h.geom.ll_y = h.geom.lr_y = top_pt + 18.0f;
    return h;
}

// The pan that centres hit_at(page, top_pt) on a page whose top is at 0.
float centred_pan(DocumentView& view, float top_pt, int vp_h) {
    return static_cast<float>(vp_h) * 0.5f - (top_pt + 9.0f) * view.zoom_pct();
}

// A page's height in DIPs: MuPDF rounds a pixmap up.
float page_dip(DocumentView& view, float height_pt) {
    return std::ceil(height_pt * view.zoom_pct());
}

float bottom_pan(DocumentView& view, int vp_h) {
    return static_cast<float>(vp_h) - page_dip(view, 1600.0f);
}

bool overlay_has(PdfCanvas& canvas, int page) {
    for (const auto& h : Access::overlay_hits(canvas)) {
        if (static_cast<int>(h.page) == page) return true;
    }
    return false;
}

// One hit per page, on the page it names.
PdfCanvas::HitsFn hits_on_every_page() {
    return [](std::size_t page) {
        return std::vector<SearchSession::Hit>{hit_at(static_cast<int>(page), 100.0f)};
    };
}

constexpr std::uint32_t kWhite = 0x00FFFFFFu;

}  // namespace

// --- apply_anchor: None ---

TEST_CASE("PdfCanvas spread: a restored pan survives whichever half lands first",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();

    // -1000 is inside either pair's range, and outside what one short page and
    // whatever the slot beside it still holds would allow.
    rig.reenter(-1000.0f);

    SECTION("left first") {
        rig.kick(1);
        rig.land_left();
        CHECK_FALSE(Access::spread_landed(rig.canvas()));
        CHECK(rig.pan_y() == -1000.0f);
        rig.land_right();
    }
    SECTION("right first") {
        rig.kick(3);   // the left slot still holds the outgoing short page
        rig.land_right();
        CHECK_FALSE(Access::spread_landed(rig.canvas()));
        CHECK(rig.pan_y() == -1000.0f);
        rig.land_left();
    }
    CHECK(Access::spread_landed(rig.canvas()));
    CHECK(rig.pan_y() == -1000.0f);
}

TEST_CASE("PdfCanvas spread: the pan is clamped once both halves are in",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();

    // Past the pair's own range: the guard above must not outlive the landing.
    rig.reenter(-5000.0f);
    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: the outgoing pair's right bitmap does not count as landed",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    // The spread kick keeps the old right bitmap on screen until its own lands.
    rig.kick(1);
    rig.canvas().set_pan(0.0f, -1000.0f);
    rig.land_left();
    CHECK_FALSE(Access::spread_landed(rig.canvas()));
    CHECK(rig.pan_y() == -1000.0f);
}

// --- apply_anchor: Bottom and Hit ---

TEST_CASE("PdfCanvas spread: a Bottom anchor reaches the right page in the same batch",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a Bottom anchor outlives a batch that lands only one half",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(1);
    rig.land_left();

    // A resize or a zoom: the anchor would be retired by this batch had the left
    // half alone counted as placing it.
    rig.kick(1);
    rig.land_left();
    rig.land_right();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a Hit anchor outlives a batch that lands only one half",
          "[ui][canvas][spread]") {
    constexpr int height = 300;
    Rig rig(kWidth, height);
    const auto h = hit_at(1, 380.0f);   // near the bottom of the short left page
    rig.canvas().set_pending_anchor(PageAnchor::hit(h));
    rig.kick(1);
    rig.land_left();
    const float left_alone = static_cast<float>(height) - page_dip(rig.view(), 400.0f);
    CHECK(rig.pan_y() == Approx(left_alone).margin(1.0f));

    rig.kick(1);
    rig.land_left();
    rig.land_right();
    CHECK(rig.pan_y() == Approx(centred_pan(rig.view(), 380.0f, height)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a Hit on the right page stays unmeasured until its slot lands",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    rig.canvas().set_pending_anchor(PageAnchor::hit(hit_at(2, 860.0f)));
    rig.kick(1);
    rig.canvas().set_pan(0.0f, -1000.0f);

    // A left page alone would clamp this pan to 0: nothing here may touch it.
    rig.land_left();
    CHECK(rig.pan_y() == -1000.0f);

    rig.land_right();
    CHECK(rig.pan_y() == Approx(centred_pan(rig.view(), 860.0f, kHeight)).margin(1.0f));
}

// --- A half that fails (#198) ---

TEST_CASE("PdfCanvas spread: a failed right half ends a Bottom anchor's landing",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(3, Fail::Right);
    SECTION("left first") {
        rig.land_left();
        rig.land_right_failure();
    }
    SECTION("right first") {
        rig.land_right_failure();
        rig.land_left();
    }
    REQUIRE(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));

    // The reader scrolls, then a resize or a zoom renders the pair again.
    rig.canvas().set_pan(0.0f, -10.0f);
    rig.kick(3, Fail::Right);
    rig.land_left();
    rig.land_right_failure();
    CHECK(rig.pan_y() == -10.0f);
}

TEST_CASE("PdfCanvas spread: a failed right half ends a Hit anchor's landing on the left page",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::hit(hit_at(3, 860.0f)));
    rig.kick(3, Fail::Right);
    SECTION("left first") {
        rig.land_left();
        rig.land_right_failure();
    }
    SECTION("right first") {
        rig.land_right_failure();
        rig.land_left();
    }
    REQUIRE(rig.pan_y() == Approx(centred_pan(rig.view(), 860.0f, kHeight)).margin(1.0f));

    rig.canvas().set_pan(0.0f, -10.0f);
    rig.kick(3, Fail::Right);
    rig.land_left();
    rig.land_right_failure();
    CHECK(rig.pan_y() == -10.0f);
}

TEST_CASE("PdfCanvas spread: a Hit on the failed right page waits for a render that works",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::hit(hit_at(2, 860.0f)));
    rig.kick(1, Fail::Right);
    rig.land_left();
    rig.land_right_failure();
    CHECK(rig.pan_y() == 0.0f);

    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(centred_pan(rig.view(), 860.0f, kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a failed left half keeps the anchor for the retry",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1, Fail::Right);
    rig.land_left();
    rig.land_right_failure();

    // The short left page is still on screen. Placing the anchor against it would
    // retire it before the tall right page could arrive.
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(1, Fail::Left);
    rig.land_left_failure();

    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a failure from a superseded batch ends no landing",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(1, Fail::Right);
    rig.kick(1);
    rig.land_right_failure();
    rig.land_left();

    // The right page is the pair's tall one: the anchor reaches it only if the
    // older batch's failure did not count against this one.
    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a failure from a superseded batch leaves the right slot alone",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    rig.kick(1, Fail::Right);
    rig.kick(1);
    rig.land_right_failure();
    CHECK(Access::right_bitmap(rig.canvas()) != nullptr);
}

TEST_CASE("PdfCanvas spread: a failed right half counts only for its own batch",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1, Fail::Right);
    rig.land_left();
    rig.land_right_failure();

    // The right page renders this time. Its half is still to come when a resize
    // opens another batch, so the left half alone must not place the anchor.
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(1);
    rig.land_left();
    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a pan is clamped once a failed right half ends the landing",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1, Fail::Right);
    rig.land_left();
    rig.land_right_failure();

    // The left page fits, and nothing is coming to make the pair taller.
    rig.reenter(-1000.0f);
    rig.kick(1, Fail::Right);
    SECTION("left first") {
        rig.land_left();
        REQUIRE(rig.pan_y() == -1000.0f);
        rig.land_right_failure();
    }
    SECTION("right first") {
        rig.land_right_failure();
        REQUIRE(rig.pan_y() == -1000.0f);
        rig.land_left();
    }
    CHECK(rig.pan_y() == 0.0f);
}

TEST_CASE("PdfCanvas spread: a failed right half waits for this batch's left half",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3, Fail::Right);
    rig.land_left();
    rig.land_right_failure();

    // A zoom: the pan belongs to the new extent, and the left bitmap on screen,
    // though this pair's, still measures the old one.
    rig.view().set_zoom_pct(rig.view().zoom_pct() * 2.0f);
    rig.kick(3, Fail::Right);
    rig.canvas().set_pan(0.0f, -2000.0f);
    rig.land_right_failure();
    CHECK(rig.pan_y() == -2000.0f);
    rig.land_left();
    CHECK(rig.pan_y() == -2000.0f);
}

TEST_CASE("PdfCanvas spread: a failed right half leaves the anchor to this batch's left half",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();

    // Both slots hold this pair's older render, so the failure alone must not
    // place the anchor: a zoom now would retire it at the old height.
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(1, Fail::Right);
    rig.land_right_failure();

    rig.view().set_zoom_pct(rig.view().zoom_pct() * 2.0f);
    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a right page with no bitmap ends a Bottom anchor's landing",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_pending_anchor(PageAnchor::bottom());
    rig.kick(3, Fail::RightBitmap);
    rig.land_left();
    rig.land_right_completion();
    REQUIRE(Access::right_bitmap(rig.canvas()) == nullptr);
    REQUIRE(rig.pan_y() == Approx(bottom_pan(rig.view(), kHeight)).margin(1.0f));

    rig.canvas().set_pan(0.0f, -10.0f);
    rig.kick(3, Fail::RightBitmap);
    rig.land_left();
    rig.land_right_completion();
    CHECK(rig.pan_y() == -10.0f);
}

TEST_CASE("PdfCanvas spread: a right page with no bitmap drops the outgoing pair's bitmap",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    rig.kick(1, Fail::RightBitmap);
    rig.land_right_completion();
    CHECK(Access::right_bitmap(rig.canvas()) == nullptr);
    rig.land_left();
}

TEST_CASE("PdfCanvas spread: a failed right half drops the outgoing pair's bitmap",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    rig.kick(1, Fail::Right);
    rig.land_right_failure();
    CHECK(Access::right_bitmap(rig.canvas()) == nullptr);
    rig.land_left();
}

TEST_CASE("PdfCanvas spread: a failed right half keeps the pair's own right bitmap",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();

    rig.kick(1, Fail::Right);
    rig.land_right_failure();
    CHECK(Access::right_bitmap(rig.canvas()) != nullptr);
    rig.land_left();
}

TEST_CASE("PdfCanvas spread: a failed left half leaves the right slot alone",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    rig.kick(1, Fail::Left);
    rig.land_left_failure();
    CHECK(Access::right_bitmap(rig.canvas()) != nullptr);
}

// --- scroll_into_view ---

TEST_CASE("PdfCanvas spread: a visible hit on the right page moves neither pan nor page",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();
    int page_changes = 0;
    rig.canvas().set_on_page_changed([&](int) { ++page_changes; });

    rig.canvas().scroll_into_view(hit_at(2, 500.0f));
    // MainWindow kicks after every find.
    rig.kick(1);
    rig.land_both();
    CHECK(rig.pan_y() == 0.0f);
    CHECK(page_changes == 0);
}

TEST_CASE("PdfCanvas spread: a hit on the right page is measured from that page's top",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();

    // On screen from the right page's top; off it from the short left page's,
    // which starts 189 DIPs lower.
    rig.canvas().scroll_into_view(hit_at(2, 640.0f));
    CHECK(rig.pan_y() == 0.0f);
}

TEST_CASE("PdfCanvas spread: a hit on the right page waits for its slot to judge visibility",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();
    rig.kick(1);
    rig.land_left();

    // Would read as on screen against the left page, and be dropped.
    rig.canvas().scroll_into_view(hit_at(2, 500.0f));
    CHECK(rig.pan_y() == 0.0f);

    rig.land_right();
    CHECK(rig.pan_y() == Approx(centred_pan(rig.view(), 500.0f, kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: an off-screen hit on the right page centres at once",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();

    rig.canvas().scroll_into_view(hit_at(2, 860.0f));
    CHECK(rig.pan_y() == Approx(centred_pan(rig.view(), 860.0f, kHeight)).margin(1.0f));
}

TEST_CASE("PdfCanvas spread: a visible hit drops an older hit that has not landed",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    // Find on another pair, its slot (the right one) still to land.
    rig.canvas().scroll_into_view(hit_at(2, 1500.0f));
    rig.kick(1);
    rig.land_left();
    REQUIRE(rig.pan_y() == 0.0f);

    // Find Next, on the page that has landed and already in view.
    rig.canvas().scroll_into_view(hit_at(1, 200.0f));
    rig.kick(1);
    rig.land_left();
    rig.land_right();
    CHECK(rig.pan_y() == 0.0f);
}

// --- The overlay ---

TEST_CASE("PdfCanvas spread: the overlay covers only the slots showing their own page",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_hits_source(hits_on_every_page());
    rig.kick(1);
    rig.land_both();
    REQUIRE(overlay_has(rig.canvas(), 1));
    REQUIRE(overlay_has(rig.canvas(), 2));

    rig.reenter(0.0f);
    rig.kick(1);

    SECTION("left first") {
        rig.land_left();
        CHECK(overlay_has(rig.canvas(), 1));
        CHECK_FALSE(overlay_has(rig.canvas(), 2));
    }
    SECTION("right first") {
        rig.land_right();
        CHECK_FALSE(overlay_has(rig.canvas(), 1));
        CHECK(overlay_has(rig.canvas(), 2));
    }
    rig.canvas().set_hits_source(nullptr);
}

TEST_CASE("PdfCanvas spread: the overlay leaves out a right slot showing the outgoing pair",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.canvas().set_hits_source(hits_on_every_page());
    rig.kick(3);
    rig.land_both();

    rig.kick(1);
    rig.land_left();
    CHECK(overlay_has(rig.canvas(), 1));
    CHECK_FALSE(overlay_has(rig.canvas(), 2));
    rig.canvas().set_hits_source(nullptr);
}

TEST_CASE("PdfCanvas spread: each slot paints only its own page's hits",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(3);
    rig.land_both();

    // Inside a quad 100 pt down its page, one slot at a time: page 3 overflows
    // and starts at the top, page 4 is centred in its slot. A hit painted in the
    // wrong slot lands at the same place on that slot's page.
    const float pct = rig.view().zoom_pct();
    const int   qx  = static_cast<int>(150.0f * pct);
    const int   qy  = static_cast<int>(109.0f * pct);
    const int   right_x   = 504 + qx;
    const int   right_top = (kHeight - static_cast<int>(page_dip(rig.view(), 400.0f))) / 2;
    auto& canvas = rig.canvas();

    SECTION("a hit on the left page") {
        rig.canvas().set_hits_source([](std::size_t page) {
            return page == 3 ? std::vector<SearchSession::Hit>{hit_at(3, 100.0f)}
                             : std::vector<SearchSession::Hit>{};
        });
        CHECK(Access::painted_pixel(canvas, qx, qy) != kWhite);
        CHECK(Access::painted_pixel(canvas, right_x, right_top + qy) == kWhite);
    }
    SECTION("a hit on the right page") {
        rig.canvas().set_hits_source([](std::size_t page) {
            return page == 4 ? std::vector<SearchSession::Hit>{hit_at(4, 100.0f)}
                             : std::vector<SearchSession::Hit>{};
        });
        CHECK(Access::painted_pixel(canvas, right_x, right_top + qy) != kWhite);
        CHECK(Access::painted_pixel(canvas, qx, qy) == kWhite);
    }
    rig.canvas().set_hits_source(nullptr);
}

// --- apply_viewport and the carry ---

TEST_CASE("PdfCanvas spread: moving to a page with no partner drops the right bitmap",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.kick(1);
    rig.land_both();
    REQUIRE(Access::right_bitmap(rig.canvas()) != nullptr);

    rig.kick(0);
    CHECK(Access::right_bitmap(rig.canvas()) == nullptr);
}

TEST_CASE("PdfCanvas spread: a carry from the strip uses the carried page's own top",
          "[ui][canvas][spread]") {
    Rig rig;
    rig.view().set_dual_page(false);
    rig.view().set_continuous(true);
    rig.canvas().switch_layout(LayoutKind::Strip, true);
    const float z_strip = rig.view().zoom_pct();

    // 300 DIPs into page 2, the pair's taller right page: its top is the union's
    // top, while the short left page centres 189 DIPs below it.
    Access::strip_scroll_to(rig.canvas(), CarriedPosition{2, 300.0});

    rig.view().set_continuous(false);
    rig.view().set_dual_page(true);
    rig.canvas().switch_layout(LayoutKind::PagedSpread, true);
    const float z_spread = rig.view().zoom_pct();

    CHECK(rig.view().current_page() == 1);
    CHECK(rig.pan_y() == Approx(-300.0f * z_spread / z_strip).margin(1.0f));
}

// --- A single page: nothing waits for a right half ---

TEST_CASE("PdfCanvas single page: a re-render clamps the pan at once",
          "[ui][canvas][spread]") {
    Rig rig(kWidth, kHeight, Layout::Single);
    rig.canvas().change_current_page(1);
    rig.kick(1);
    rig.land_left();

    // Past the page's range. A spread would hold this pan for its right half.
    rig.canvas().set_pan(0.0f, -1000.0f);
    rig.kick(1);
    rig.land_left();
    CHECK(rig.pan_y() == Approx(kHeight - page_dip(rig.view(), 400.0f)).margin(1.0f));
}

TEST_CASE("PdfCanvas single page: a Hit is placed by its own completion",
          "[ui][canvas][spread]") {
    Rig rig(kWidth, kHeight, Layout::Single);
    rig.canvas().change_current_page(2);
    rig.canvas().set_pending_anchor(PageAnchor::hit(hit_at(2, 1500.0f)));
    rig.kick(2);
    rig.land_left();
    REQUIRE(rig.pan_y() < 0.0f);

    // Placed, so the next batch retires it and the reader's pan stands.
    rig.canvas().set_pan(0.0f, -10.0f);
    rig.kick(2);
    rig.land_left();
    CHECK(rig.pan_y() == -10.0f);
}
