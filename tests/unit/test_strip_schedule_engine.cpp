// #55 PR 1 Task 11: the in-process scheduling gate (spec 9.4). StripRenderState
// drives a real DocumentView whose single worker is held, through 1000
// viewports of a top-to-bottom drag over the 1000-page fixture. Cancellation
// must keep the live queued work bounded, the sweep must clear pages cancelled
// from outside, and once the worker is released only the final view's pages
// may be rendered.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/PageCache.hpp"
#include "core/detail/PrefetchPlan.hpp"
#include "ui/detail/StripCompletion.hpp"
#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripSchedule.hpp"
#include "ui/detail/StripScroll.hpp"

// Plain extern "C" declarations keep this TU MuPDF-header-free, as in
// test_document_view.cpp.
extern "C" {
struct fz_context;
struct fz_pixmap;
void fz_drop_pixmap(fz_context* ctx, fz_pixmap* pix);
}

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::core::PageSize;
using litepdf::ui::BitmapResult;
using litepdf::ui::PageSlot;
using litepdf::ui::PageStateKind;
using litepdf::ui::ScheduleInput;
using litepdf::ui::StripCompletionAction;
using litepdf::ui::StripCompletionMeta;
using litepdf::ui::StripLayout;
using litepdf::ui::StripPageSize;
using litepdf::ui::StripRange;
using litepdf::ui::StripRenderState;
using litepdf::ui::StripTravel;

namespace {

constexpr const char* kStrip1000 = "tests/fixtures/strip-1000.pdf";
constexpr double kVpW = 800.0;
constexpr double kVpH = 600.0;
constexpr int kSteps = 1000;
constexpr int kOutsideCancelStep = 500;
constexpr std::uint64_t kEpoch = 1;

// The HeldWorker / ReleaseOnExit pattern of test_document_view.cpp, copied here
// (existing tests are not edited). The hold goes through request_page_render,
// so no prefetched neighbour joins the queue.
struct HeldWorker {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released;
};

// Releases the held worker on scope exit, a failed REQUIRE included, so the
// view's destructor can drain its engine. Declare it AFTER the view.
struct ReleaseOnExit {
    std::shared_ptr<HeldWorker> held;
    ~ReleaseOnExit() { release(); }
    void release() {
        try { held->release.set_value(); } catch (const std::future_error&) {}
    }
};

std::shared_ptr<HeldWorker> hold_worker(DocumentView& view, int page) {
    auto held = std::make_shared<HeldWorker>();
    held->released = held->release.get_future().share();
    auto entered = held->entered.get_future();
    (void)view.request_page_render(
        page, 0,
        [held](fz_pixmap* p, fz_context* ctx) {
            if (p) fz_drop_pixmap(ctx, p);
            held->entered.set_value();
            (void)held->released.wait_for(std::chrono::seconds(30));
        },
        std::make_shared<std::atomic<bool>>(false));
    REQUIRE(entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return held;
}

// One worker-side completion, as the strip's WM_USER_STRIP_DONE payload would
// carry it.
struct Completion {
    int page = 0;
    std::uint64_t seq = 0;
    bool has_pixmap = false;
    bool cancelled = false;
};

// Filled on the worker, drained on the test thread. Shared with every
// callback, so it outlives the engine's own drain in ~DocumentView.
struct CompletionSink {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Completion> q;

    void push(Completion c) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            q.push_back(c);
        }
        cv.notify_one();
    }

    // Waits up to `timeout` for at least one completion and takes them all.
    std::deque<Completion> take(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, timeout, [this] { return !q.empty(); });
        std::deque<Completion> out;
        out.swap(q);
        return out;
    }
};

// Submits real renders. The seq comes from this submitter's own counter, never
// from the engine's token id.
class ViewSubmitter : public StripRenderState::Submitter {
public:
    ViewSubmitter(DocumentView& view, std::shared_ptr<CompletionSink> sink)
        : view_(view), sink_(std::move(sink)) {}

    std::uint64_t submit(int page, int priority, bool /*with_text*/,
                         std::shared_ptr<std::atomic<bool>> flag) override {
        const std::uint64_t seq = ++next_seq_;
        auto sink = sink_;
        auto cb_flag = flag;
        (void)view_.request_page_render(
            page, priority,
            [sink, page, seq, cb_flag](fz_pixmap* p, fz_context* ctx) {
                Completion c;
                c.page = page;
                c.seq = seq;
                c.has_pixmap = p != nullptr;
                c.cancelled = cb_flag->load();
                if (p) fz_drop_pixmap(ctx, p);
                sink->push(c);
            },
            std::move(flag));
        return seq;
    }
    void cancel_thumbnails() override { view_.cancel_thumbnail_renders(); }
    void released(int /*page*/) override {}

private:
    DocumentView& view_;
    std::shared_ptr<CompletionSink> sink_;
    std::uint64_t next_seq_ = 0;
};

std::size_t range_size(StripRange r) {
    return r.empty() ? 0 : static_cast<std::size_t>(r.last - r.first + 1);
}

// The ScheduleInput of one viewport of a downward drag.
struct ViewInput {
    ScheduleInput in;
    StripRange v;
    StripRange p;
};

ViewInput make_input(const StripLayout& layout, const std::vector<PageSize>& sizes,
                     double scroll_y, float scale, bool invert, StripRange previous_v) {
    ViewInput out;
    const StripRange v = layout.visible(scroll_y, scroll_y + kVpH);
    const StripRange p =
        litepdf::ui::prefetch_range(layout, scroll_y, kVpH, StripTravel::Down);

    // derive_current_page's page first, then the rest by visible height
    // (ties to the lower index).
    std::vector<int> by_area;
    const int current = litepdf::ui::derive_current_page(layout, scroll_y, kVpH);
    if (v.contains(current)) by_area.push_back(current);
    std::vector<std::pair<double, int>> rest;
    for (int page = v.first; page <= v.last; ++page) {
        if (page == current) continue;
        const double t = layout.top(page);
        const double vis = std::min(t + layout.height(page), scroll_y + kVpH) -
                           std::max(t, scroll_y);
        rest.emplace_back(vis, page);
    }
    std::stable_sort(rest.begin(), rest.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& r : rest) by_area.push_back(r.second);

    ScheduleInput& in = out.in;
    in.visible = v;
    in.visible_by_area = std::move(by_area);
    in.prefetch = p;
    in.retain = layout.visible(scroll_y - kVpH, scroll_y + 2 * kVpH);
    in.scale = scale;
    in.invert = invert;
    in.l1_capacity = 5;
    in.l1_budget = litepdf::core::kDefaultL1BudgetBytes;
    in.bytes_of = [&sizes, scale](int page) {
        const PageSize& s = sizes[static_cast<std::size_t>(page)];
        return litepdf::core::estimated_pixmap_bytes(s.width_pt, s.height_pt, scale);
    };
    in.previous_first_visible = previous_v.first;
    in.previous_last_visible = previous_v.last;
    out.v = v;
    out.p = p;
    return out;
}

std::size_t live_queued(StripRenderState& state) {
    std::size_t n = 0;
    for (const auto& [page, s] : state.slots())
        if (s.kind == PageStateKind::Pending && !(s.cancel_flag && s.cancel_flag->load()))
            ++n;
    return n;
}

std::vector<int> pages_pending_with_flag_set(StripRenderState& state) {
    std::vector<int> out;
    for (const auto& [page, s] : state.slots())
        if (s.kind == PageStateKind::Pending && s.cancel_flag && s.cancel_flag->load())
            out.push_back(page);
    return out;
}

bool any_pending_in(const StripRenderState& state, StripRange r) {
    for (int page = r.first; page <= r.last; ++page) {
        const PageSlot* s = state.slot(page);
        if (s && s->kind == PageStateKind::Pending) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("StripSchedule: cancellation bounds live work over a 1000-page drag",
          "[ui][strip][schedule][engine]") {
    InlineDispatcher disp;
    auto sink = std::make_shared<CompletionSink>();

    Document doc;
    REQUIRE_FALSE(doc.open(kStrip1000).has_value());
    REQUIRE(doc.page_count() == 1000);
    auto view = std::make_unique<DocumentView>(std::move(doc), disp, std::nullopt,
                                               /*workers*/ 1, /*l1*/ 5, /*l2*/ 10,
                                               litepdf::core::kDefaultL1BudgetBytes);
    view->set_zoom_pct(1.0f);
    view->set_viewport(static_cast<float>(kVpW), static_cast<float>(kVpH), 96.0f);
    REQUIRE(view->zoom_pct() == 1.0f);

    const std::vector<PageSize> sizes = view->page_sizes();
    REQUIRE(sizes.size() == 1000);
    std::vector<StripPageSize> strip_sizes;
    strip_sizes.reserve(sizes.size());
    for (const PageSize& s : sizes) strip_sizes.push_back({s.width_pt, s.height_pt});
    const StripLayout layout(std::move(strip_sizes), 1.0);
    const double total = layout.total_height();
    REQUIRE(total > kVpH);

    const float scale = view->render_scale();
    const bool invert = view->invert_colors();

    ReleaseOnExit guard{hold_worker(*view, 0)};

    StripRenderState state;
    ViewSubmitter submitter(*view, sink);

    StripRange previous_v{};
    double scroll_y = 0.0;
    std::size_t max_live = 0;
    std::size_t max_bound = 0;
    std::size_t flagged_from_outside = 0;
    ViewInput last;
    const double step = (total - kVpH) / static_cast<double>(kSteps - 1);

    for (int i = 0; i < kSteps; ++i) {
        scroll_y = litepdf::ui::clamp_strip_scroll(i * step, total, kVpH);
        last = make_input(layout, sizes, scroll_y, scale, invert, previous_v);
        (void)state.run_pass(last.in, submitter);

        const std::size_t live = live_queued(state);
        const std::size_t bound = range_size(last.v) + range_size(last.p) + 1;
        INFO("step " << i << " scroll_y " << scroll_y << " live " << live
                     << " bound " << bound);
        REQUIRE(live <= bound);
        max_live = std::max(max_live, live);
        max_bound = std::max(max_bound, bound);

        if (i == kOutsideCancelStep) {
            // spec 9.4 step 4: a cancel from outside, then one pass on the same
            // viewport. The flagged pages must be swept, not left Pending.
            view->cancel_background_renders();
            const std::vector<int> flagged = pages_pending_with_flag_set(state);
            REQUIRE_FALSE(flagged.empty());  // the checks below are not vacuous
            flagged_from_outside = flagged.size();
            const auto plan = state.run_pass(last.in, submitter);
            REQUIRE(pages_pending_with_flag_set(state).empty());
            // Every flagged page sits in P here, so the same pass requests it
            // again and the slot check above holds even without the sweep;
            // the pass must still report each one as swept (spec 5.2 step 0).
            for (int page : flagged) {
                INFO("flagged page " << page);
                REQUIRE(std::find(plan.sweep.begin(), plan.sweep.end(), page) !=
                        plan.sweep.end());
            }
        }
        previous_v = last.v;
    }

    // Release the worker and drain on the test thread, as the UI thread would.
    guard.release();
    const StripRange final_v = last.v;
    const StripRange final_p = last.p;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (any_pending_in(state, final_v) && std::chrono::steady_clock::now() < deadline) {
        for (const Completion& c : sink->take(std::chrono::milliseconds(50))) {
            const PageSlot* slot = state.slot(c.page);
            StripCompletionMeta meta;
            meta.epoch = kEpoch;
            meta.page = c.page;
            meta.seq = c.seq;
            meta.cancelled = c.cancelled;
            switch (litepdf::ui::classify_strip_completion(slot, meta, kEpoch,
                                                           c.has_pixmap)) {
            case StripCompletionAction::Drop: break;
            case StripCompletionAction::CreateBitmap:
                state.slots()[c.page] = litepdf::ui::after_bitmap(*slot, BitmapResult::Ok);
                break;
            case StripCompletionAction::CancelAndReschedule:
                state.slots()[c.page] = litepdf::ui::after_null(*slot, true);
                (void)state.run_pass(last.in, submitter);
                break;
            case StripCompletionAction::MarkFailed:
                state.slots()[c.page] = litepdf::ui::after_null(*slot, false);
                break;
            }
        }
    }

    const std::size_t started = view->renders_started();
    const std::size_t started_bound = range_size(final_v) + range_size(final_p) + 1;
    WARN("strip scheduling gate: max live queued " << max_live << " (largest bound "
         << max_bound << "); flagged at step " << kOutsideCancelStep << ": "
         << flagged_from_outside << "; renders_started " << started << " (bound "
         << started_bound << "); final V " << range_size(final_v) << " [" << final_v.first
         << ", " << final_v.last << "], final P " << range_size(final_p));
    CHECK(started <= started_bound);
    for (int page = final_v.first; page <= final_v.last; ++page) {
        const PageSlot* s = state.slot(page);
        INFO("final V page " << page);
        REQUIRE(s != nullptr);
        CHECK(s->kind == PageStateKind::Ready);
    }
}
