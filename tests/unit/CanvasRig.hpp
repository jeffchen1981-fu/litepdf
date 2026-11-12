#pragma once
// The canvas unit-test rig, shared by the test files that drive a real PdfCanvas
// (test_canvas_spread_wiring.cpp and its siblings). A hidden window holds a real
// canvas over a real DocumentView, and the two render completions are taken from
// the message queue one at a time, so a test decides which half lands first
// instead of racing the workers. The test process is not DPI aware: one device
// pixel is one DIP, on every machine.
//
// The helpers live in a named namespace, as inline functions and inline constexpr
// constants: an unnamed one would give them internal linkage, and a file that does
// not call one would raise C4505 under /W4.

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

namespace litepdf::ui {

struct PdfCanvasTestAccess {
    static ID2D1Bitmap* left_bitmap(const PdfCanvas& c) {
        return c.impl_->current_bitmap.Get();
    }
    static ID2D1Bitmap* right_bitmap(const PdfCanvas& c) {
        return c.impl_->right_bitmap.Get();
    }
    static std::vector<core::SearchSession::Hit> overlay_hits(const PdfCanvas& c) {
        return c.overlay_hits();
    }
    static bool spread_landed(const PdfCanvas& c) { return c.spread_landed(); }
    static void strip_scroll_to(PdfCanvas& c, const CarriedPosition& at) {
        c.impl_->strip_view.apply_carry(at);
    }
    static CarriedPosition strip_position(const PdfCanvas& c) {
        return c.impl_->strip_view.carry();
    }
    static void navigate(PdfCanvas& c, int page) {
        c.navigate_to_page(page, PageAnchor::top());
    }
    static void pan_by(PdfCanvas& c, float dx, float dy) { c.pan_by(dx, dy); }
    static void wheel(PdfCanvas& c, int delta) { c.on_wheel_scroll(delta); }
    static void hwheel(PdfCanvas& c, int delta) {
        c.on_hwheel_scroll(delta, HWheelSource::Tilt);
    }
    static void key(PdfCanvas& c, WPARAM vk) { c.on_key_down(vk); }
    // Where a press at client pixel (x, y) falls on the single page shown.
    static litepdf::core::SelPoint page_point_at(PdfCanvas& c, int x, int y) {
        Placement pl;
        REQUIRE(c.single_page_placement(pl));
        return c.page_point_at(x, y, pl);
    }
    // (#53) Where on_paint draws the single page, in the view frame: the
    // bitmap's turned extent. REQUIREs a bitmap and a render target.
    static Placement placement(PdfCanvas& c) {
        Placement pl;
        REQUIRE(c.single_page_placement(pl));
        return pl;
    }
    static float pan_y_for_hit(const PdfCanvas& c, const core::SearchSession::Hit& h) {
        return c.pan_y_for_hit(h);
    }
    // What a wheel flip leaves behind, for a batch the test submitted itself.
    static void mark_wheel_flip(PdfCanvas& c) { c.impl_->wheel_flip_seq = c.impl_->next_seq; }

    // (#53) The pending-anchor slot: its kind (None when empty; an applied anchor
    // keeps its kind until the next stamp()) and its applied mark.
    static PageAnchor::Kind anchor_kind(const PdfCanvas& c) { return c.impl_->anchor.kind(); }
    static bool anchor_applied(const PdfCanvas& c) { return c.impl_->anchor.applied(); }
    static bool held_place(const PdfCanvas& c) { return c.impl_->held_place.has_value(); }
    static bool gesture_live(const PdfCanvas& c) {
        return c.impl_->gesture.gesture() != Gesture::None;
    }

    // A plain left press at client pixel (x, y). Without the capture,
    // cancel_stale_gesture ends the drag on the first move, so a press that began
    // a gesture must have taken it.
    static void left_down(PdfCanvas& c, int x, int y) {
        c.on_left_button_down(false, x, y);
        if (gesture_live(c)) REQUIRE(GetCapture() == c.hwnd());
    }
    static void mouse_move(PdfCanvas& c, int x, int y) { c.on_mouse_move(x, y); }
    static void left_up(PdfCanvas& c, int x, int y) { c.on_left_button_up(x, y); }

    // The painted extent and its origin, in canvas DIPs. REQUIREs there is one.
    static PdfCanvas::ContentBox content_extent(const PdfCanvas& c) {
        PdfCanvas::ContentBox box{};
        REQUIRE(c.content_extent(box));
        return box;
    }

    // The continuous strip's layout, scroll and page rectangles.
    static const StripLayout& strip_layout(const PdfCanvas& c) {
        return c.impl_->strip_view.layout();
    }
    static double strip_scroll_y(const PdfCanvas& c) { return c.impl_->strip_view.scroll_y(); }
    static StripPageRect strip_page_rect(const PdfCanvas& c, int page) {
        return c.impl_->strip_view.page_rect(page);
    }
    // A strip page's render state: None when the page has no slot.
    static PageStateKind strip_slot_kind(const PdfCanvas& c, int page) {
        const PageSlot* s = c.impl_->strip_set.state.slot(page);
        return s ? s->kind : PageStateKind::None;
    }
    // Ready at the view's current render scale and polarity.
    static bool strip_slot_fresh(const PdfCanvas& c, int page) {
        const PageSlot* s = c.impl_->strip_set.state.slot(page);
        const litepdf::core::DocumentView* v = c.impl_->view;
        return s && v && s->kind == PageStateKind::Ready &&
               !is_stale(*s, v->render_scale(), v->invert_colors());
    }
    // (#53) The rect a strip page is drawn into, in the view frame (spec R3).
    static Placement strip_drawn_rect(const PdfCanvas& c, int page) {
        return c.strip_drawn_rect(page);
    }
    // Where a press at client pixel (x, y) falls on strip page `page`, mapped
    // through that page's drawn rect.
    static litepdf::core::SelPoint strip_page_point_at(PdfCanvas& c, int page, int x, int y) {
        return c.page_point_at(x, y, c.strip_drawn_rect(page));
    }
    // The live selection's two raw points. REQUIREs a live selection.
    static litepdf::core::SelPoint live_anchor(const PdfCanvas& c) {
        REQUIRE(c.impl_->live_selection.has_value());
        return c.impl_->live_selection->anchor;
    }
    static litepdf::core::SelPoint live_extent(const PdfCanvas& c) {
        REQUIRE(c.impl_->live_selection.has_value());
        return c.impl_->live_selection->extent;
    }

    // The tallest bitmap this canvas's target takes. A GPU's limit is 16384; the
    // software target of a runner with no GPU takes 512 times that.
    static int max_bitmap_px(PdfCanvas& c) {
        c.create_render_target();
        ID2D1HwndRenderTarget* const rt = c.impl_->rt.Get();
        REQUIRE(rt != nullptr);
        return static_cast<int>(rt->GetMaximumBitmapSize());
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

namespace canvas_rig {

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::core::SearchSession;
using litepdf::ui::CarriedPosition;
using litepdf::ui::KickReason;
using litepdf::ui::LayoutKind;
using litepdf::ui::PageAnchor;
using litepdf::ui::PageStateKind;
using litepdf::ui::PdfCanvas;
using litepdf::ui::StripKickActions;
using litepdf::ui::WM_USER_RENDER_DONE;
using litepdf::ui::WM_USER_RENDER_DONE_RIGHT;
using litepdf::ui::WM_USER_STRIP_DONE;

using Access = litepdf::ui::PdfCanvasTestAccess;

inline constexpr int kWidth  = 1000;
inline constexpr int kHeight = 850;

inline constexpr const char* kPairs   = "tests/fixtures/spread-pairs.pdf";
inline constexpr const char* kDamaged = "tests/fixtures/damaged-pages.pdf";
inline constexpr const char* kLarge   = "tests/fixtures/large.pdf";
// Pages of unequal widths, unlike kPairs, whose pages are all 420 pt wide.
inline constexpr const char* kUnequal = "tests/fixtures/spread-unequal.pdf";

// (#53) scripts/generate-rotation-fixture.py: four A4 portrait pages. Page 0 has
// a black square at page space [36, 108] x [36, 108] and the line "Rotation
// marker text" (Helvetica 12, baseline 60 pt from the top, from x = 150); page 2
// the single word "ZENITH" near the top-right corner; pages 1 and 3 are blank.
inline constexpr const char* kMarker = "tests/fixtures/rotation-marker.pdf";
inline constexpr int kMarkWordPage = 2;
// The centre x of ZENITH's hit quad, in points, measured to 0.1 pt. The smoke
// direction row in scripts/smoke-test.ps1 must carry the same value.
inline constexpr float kMarkWordCentreXPt = 491.7f;

inline Document open_fixture(const char* path) {
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    return doc;
}

enum class Layout { Spread, Single };

// Which half of a batch fails, and how (#198).
enum class Fail { None, Left, Right, LeftBitmap, RightBitmap };

// A pixmap past the target's limit: the worker delivers it, CreateBitmap refuses.
inline fz_pixmap* too_tall_for_a_bitmap(fz_pixmap* rendered, fz_context* ctx, int height) {
    fz_drop_pixmap(ctx, rendered);
    return fz_new_pixmap(ctx, fz_device_bgr(ctx), 1, height, nullptr, 1);
}

class Rig {
public:
    explicit Rig(int width = kWidth, int height = kHeight, Layout layout = Layout::Spread,
                 const char* path = kPairs)
        : view_(open_fixture(path), dispatcher_) {
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
        if (second_) second_->cancel_stale_renders(0);
        MSG m;
        while (PeekMessageW(&m, canvas_->hwnd(), WM_USER_RENDER_DONE, WM_USER_STRIP_DONE,
                            PM_REMOVE)) {
            DispatchMessageW(&m);
        }
        DestroyWindow(parent_);
        second_.reset();
        canvas_.reset();
    }

    PdfCanvas&    canvas() { return *canvas_; }
    DocumentView& view() { return view_; }

    // (#53) A second view, as a second tab on the same document: on the rig's
    // dispatcher, and gone before the canvas. One per rig.
    DocumentView& second_view(const char* path) {
        REQUIRE_FALSE(second_.has_value());
        second_.emplace(open_fixture(path), dispatcher_);
        return *second_;
    }

    // MainWindow::kick_render's paged branches: in a spread, one batch for both
    // halves. A failing half asks the worker for a page past the end, so
    // fz_load_page throws there as it does for a damaged page, and the null result
    // is reported as the pair's own page.
    void kick(int page, Fail fail = Fail::None) {
        if (!view_.dual_page()) {
            kick_single(view_, fail == Fail::Left ? view_.page_count() + 100 : page, page);
            return;
        }
        const HWND target = canvas_->hwnd();
        const std::uint64_t epoch = canvas_->render_epoch();
        const std::uint64_t seq   = canvas_->next_render_seq();
        const int total = view_.page_count();
        const int left  = litepdf::ui::dual_page_compute_left(page, total);
        const int right = litepdf::ui::dual_page_compute_right(left, total);
        view_.cancel_stale_renders(0);
        canvas_->change_current_page(left);
        canvas_->apply_viewport();
        const int past_end = total + 100;
        const int too_tall = fail == Fail::LeftBitmap || fail == Fail::RightBitmap
                                 ? Access::max_bitmap_px(*canvas_) + 1 : 0;
        const int left_too_tall = fail == Fail::LeftBitmap ? too_tall : 0;
        view_.request_render(fail == Fail::Left ? past_end : left,
            [target, epoch, left, seq, left_too_tall](fz_pixmap* p, fz_context* ctx) {
                if (left_too_tall > 0) p = too_tall_for_a_bitmap(p, ctx, left_too_tall);
                PdfCanvas::post_render_done(target, p, ctx, epoch, left, seq);
            });
        if (right >= 0) {
            const int right_too_tall = fail == Fail::RightBitmap ? too_tall : 0;
            view_.request_render(fail == Fail::Right ? past_end : right,
                [target, epoch, right, seq, right_too_tall](fz_pixmap* p, fz_context* ctx) {
                    if (right_too_tall > 0) p = too_tall_for_a_bitmap(p, ctx, right_too_tall);
                    PdfCanvas::post_render_done_right(target, p, ctx, epoch, right, seq);
                });
        }
    }

    // (#53) kick's single-page branch, rendering from `v`: kick itself renders
    // from the rig's own view, so after set_view(&second) it would post the first
    // view's pixmap.
    void kick_view(DocumentView& v, int page) { kick_single(v, page, page); }

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

    // Hand the canvas one completion of a slot, whatever it brings.
    void land_left_completion() { land_completion(WM_USER_RENDER_DONE); }
    void land_right_completion() { land_completion(WM_USER_RENDER_DONE_RIGHT); }

    // (#53) MainWindow's two-page and continuous toggles into the strip, then the
    // kick that follows them.
    void enter_strip() {
        view_.set_dual_page(false);
        view_.set_continuous(true);
        canvas_->switch_layout(LayoutKind::Strip, true);
        strip_kick(KickReason::Layout);
    }

    // MainWindow::kick_render's strip branch.
    void strip_kick(KickReason r) {
        const StripKickActions a = strip_kick_actions(r);
        if (a.refit) canvas_->apply_viewport();
        canvas_->strip_kick(a, nullptr);
    }

    // Hand the canvas strip completions until `page`'s slot is Ready at the
    // view's render scale and polarity.
    void land_strip(int page) {
        land_strip_until([this, page] { return Access::strip_slot_fresh(*canvas_, page); });
    }
    // The same, until `page`'s slot is Failed.
    void land_strip_failed(int page) {
        land_strip_until([this, page] {
            return Access::strip_slot_kind(*canvas_, page) == PageStateKind::Failed;
        });
    }

    // MainWindow's rotate command: the canvas turns, then the kick, by layout.
    void rotate(bool cw) {
        canvas_->rotate(cw);
        if (canvas_->layout() == LayoutKind::Strip) {
            strip_kick(KickReason::Rotate);
        } else {
            kick(view_.current_page());
        }
    }

    float pan_x() { return canvas_->pan().x; }
    float pan_y() { return canvas_->pan().y; }

    // A tab switch back to this view: neither bitmap counts any more, and the
    // tab's stored pan is restored before its renders land.
    void reenter(float pan_y) {
        canvas_->set_view(&view_);
        canvas_->set_pan(0.0f, pan_y);
    }

private:
    // The single-page batch: `asked` is the page the worker renders, `page` the
    // one the completion reports.
    void kick_single(DocumentView& v, int asked, int page) {
        const HWND target = canvas_->hwnd();
        const std::uint64_t epoch = canvas_->render_epoch();
        const std::uint64_t seq   = canvas_->next_render_seq();
        canvas_->apply_viewport();
        v.request_render(asked, [target, epoch, page, seq](fz_pixmap* p, fz_context* ctx) {
            PdfCanvas::post_render_done(target, p, ctx, epoch, page, seq);
        });
    }

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

    template <class Done>
    void land_strip_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done()) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            MSG m;
            if (PeekMessageW(&m, canvas_->hwnd(), WM_USER_STRIP_DONE, WM_USER_STRIP_DONE,
                             PM_REMOVE)) {
                DispatchMessageW(&m);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void land_completion(UINT msg) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        MSG m;
        while (!PeekMessageW(&m, canvas_->hwnd(), msg, msg, PM_REMOVE)) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        DispatchMessageW(&m);
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

    InlineDispatcher            dispatcher_;
    DocumentView                view_;
    HWND                        parent_ = nullptr;
    std::optional<PdfCanvas>    canvas_;
    std::optional<DocumentView> second_;
};

// An 18 pt high quad, 100 pt in from the page's left edge.
inline SearchSession::Hit hit_at(int page, float top_pt) {
    SearchSession::Hit h{};
    h.page = static_cast<std::size_t>(page);
    h.geom.ul_x = h.geom.ll_x = 100.0f;
    h.geom.ur_x = h.geom.lr_x = 200.0f;
    h.geom.ul_y = h.geom.ur_y = top_pt;
    h.geom.ll_y = h.geom.lr_y = top_pt + 18.0f;
    return h;
}

// The pan that centres hit_at(page, top_pt) on a page whose top is at 0.
inline float centred_pan(DocumentView& view, float top_pt, int vp_h) {
    return static_cast<float>(vp_h) * 0.5f - (top_pt + 9.0f) * view.zoom_pct();
}

// A page's height in DIPs: MuPDF rounds a pixmap up.
inline float page_dip(DocumentView& view, float height_pt) {
    return std::ceil(height_pt * view.zoom_pct());
}

inline float bottom_pan(DocumentView& view, int vp_h) {
    return static_cast<float>(vp_h) - page_dip(view, 1600.0f);
}

inline bool overlay_has(PdfCanvas& canvas, int page) {
    for (const auto& h : Access::overlay_hits(canvas)) {
        if (static_cast<int>(h.page) == page) return true;
    }
    return false;
}

// MainWindow's two-page toggle, in each direction.
inline void enter_spread(Rig& rig) {
    rig.view().set_continuous(false);
    rig.view().set_dual_page(true);
    rig.canvas().switch_layout(LayoutKind::PagedSpread, true);
}

inline void leave_spread(Rig& rig) {
    rig.view().set_dual_page(false);
    rig.view().set_continuous(true);
    rig.canvas().switch_layout(LayoutKind::Strip, true);
}

// A reader in the strip, `offset` DIPs into `page`, turns two-page on.
inline void enter_spread_from(Rig& rig, int page, double offset) {
    leave_spread(rig);
    Access::strip_scroll_to(rig.canvas(), CarriedPosition{page, offset});
    enter_spread(rig);
}

// One hit per page, on the page it names.
inline PdfCanvas::HitsFn hits_on_every_page() {
    return [](std::size_t page) {
        return std::vector<SearchSession::Hit>{hit_at(static_cast<int>(page), 100.0f)};
    };
}

inline constexpr std::uint32_t kWhite  = 0x00FFFFFFu;
inline constexpr std::uint32_t kChrome = 0x00F0F0F0u;
inline constexpr std::uint32_t kBlack  = 0x00000000u;

// A painted pixel is dark when every channel is below 0x40: the fixture's ink,
// not the page, the chrome or the antialiased edge of either.
inline bool is_dark(std::uint32_t px) {
    return ((px >> 16) & 0xFFu) < 0x40u && ((px >> 8) & 0xFFu) < 0x40u && (px & 0xFFu) < 0x40u;
}

// The search hit on kMarker's word.
inline SearchSession::Hit marker_hit(DocumentView& view) {
    const auto hits = view.document().page_hits(kMarkWordPage, "ZENITH", {});
    REQUIRE_FALSE(hits.empty());
    return SearchSession::Hit{static_cast<std::size_t>(kMarkWordPage), hits[0]};
}

}  // namespace canvas_rig
