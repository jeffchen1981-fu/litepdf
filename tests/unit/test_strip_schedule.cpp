#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ui/detail/StripLayout.hpp"
#include "ui/detail/StripSchedule.hpp"

using namespace litepdf::ui;

namespace {

constexpr std::size_t kMiB = std::size_t{1} << 20;
constexpr std::size_t kGiB = std::size_t{1} << 30;

using Flag = std::shared_ptr<std::atomic<bool>>;

struct Submitted {
    int page;
    int priority;
    bool with_text;
    Flag flag;
    std::uint64_t seq;
};

class FakeSubmitter : public StripRenderState::Submitter {
public:
    std::uint64_t submit(int page, int priority, bool with_text, Flag flag) override {
        const std::uint64_t seq = next_seq_++;
        submitted.push_back({page, priority, with_text, std::move(flag), seq});
        return seq;
    }
    void cancel_thumbnails() override { ++thumbnail_cancels; }
    void released(int page) override { released_pages.push_back(page); }

    std::vector<int> pages_at(int priority) const {
        std::vector<int> out;
        for (const Submitted& s : submitted)
            if (s.priority == priority) out.push_back(s.page);
        return out;
    }
    int released_count(int page) const {
        return static_cast<int>(
            std::count(released_pages.begin(), released_pages.end(), page));
    }

    std::vector<Submitted> submitted;
    int thumbnail_cancels = 0;
    std::vector<int> released_pages;

private:
    std::uint64_t next_seq_ = 1;
};

// V in ascending order as visible_by_area, no prefetch, a wide retain range,
// scale 1, no invert, L1 5 entries / 1 GiB, 1 MiB per page, previous V empty.
ScheduleInput input(StripRange v, StripRange p = {}, StripRange retain = {0, 1000}) {
    ScheduleInput in;
    in.visible = v;
    for (int i = v.first; i <= v.last; ++i) in.visible_by_area.push_back(i);
    in.prefetch = p;
    in.retain = retain;
    in.scale = 1.0f;
    in.invert = false;
    in.l1_capacity = 5;
    in.l1_budget = kGiB;
    in.bytes_of = [](int) { return kMiB; };
    in.previous_first_visible = 0;
    in.previous_last_visible = -1;
    return in;
}

bool has(const std::vector<int>& v, int page) {
    return std::find(v.begin(), v.end(), page) != v.end();
}

PageSlot ready(float scale, bool invert = false) {
    PageSlot s;
    s.kind = PageStateKind::Ready;
    s.scale = scale;
    s.invert = invert;
    return s;
}

PageSlot failed(int priority) {
    PageSlot s;
    s.kind = PageStateKind::Failed;
    s.priority = priority;
    return s;
}

PageSlot pending(std::uint64_t seq, int priority, Flag flag) {
    PageSlot s;
    s.kind = PageStateKind::Pending;
    s.seq = seq;
    s.priority = priority;
    s.scale = 1.0f;
    s.cancel_flag = std::move(flag);
    return s;
}

Flag fresh_flag() { return std::make_shared<std::atomic<bool>>(false); }

// Task 1's three-page layout: pages at [8,208), [216,316), [324,624).
StripLayout three_pages() {
    return StripLayout({{100, 200}, {50, 100}, {100, 300}}, 1.0);
}

}  // namespace

TEST_CASE("StripSchedule: visible pages get P0 largest area first and the first carries text",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    ScheduleInput in = input({2, 4});
    in.visible_by_area = {3, 2, 4};

    const SchedulePlan plan = s.run_pass(in, out);

    CHECK(plan.submit_p0 == std::vector<int>{3, 2, 4});
    CHECK(plan.text_on_p0 == 3);
    REQUIRE(out.submitted.size() == 3);
    CHECK(out.pages_at(0) == std::vector<int>{3, 2, 4});
    CHECK(out.submitted[0].with_text);
    CHECK_FALSE(out.submitted[1].with_text);
    CHECK_FALSE(out.submitted[2].with_text);
    for (int p : {2, 3, 4}) {
        REQUIRE(s.slot(p) != nullptr);
        CHECK(s.slot(p)->kind == PageStateKind::Pending);
        CHECK(s.slot(p)->priority == 0);
    }
}

TEST_CASE("StripSchedule: prefetch only while L1 has room", "[ui][strip]") {
    {
        StripRenderState s;
        FakeSubmitter out;
        // |V| = 4 of 5 entries: room for exactly one prefetch.
        const SchedulePlan plan = s.run_pass(input({0, 3}, {4, 5}), out);
        CHECK(plan.submit_p1 == std::vector<int>{4});
        CHECK(out.pages_at(1) == std::vector<int>{4});
        CHECK(s.slot(5) == nullptr);
    }
    {
        StripRenderState s;
        FakeSubmitter out;
        ScheduleInput in = input({0, 3}, {4, 5});
        in.bytes_of = [](int) { return 300 * kMiB; };  // V alone is 1200 MiB > 1 GiB
        const SchedulePlan plan = s.run_pass(in, out);
        CHECK(plan.submit_p1.empty());
        CHECK(out.pages_at(1).empty());
        CHECK(out.pages_at(0).size() == 4);
    }
}

TEST_CASE("StripSchedule: pages leaving V and P are cancelled and their flags set",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.run_pass(input({0, 1}, {}, {0, 10}), out);
    REQUIRE(out.submitted.size() == 2);
    const Flag f0 = out.submitted[0].flag;
    const Flag f1 = out.submitted[1].flag;
    CHECK_FALSE(f0->load());
    CHECK_FALSE(f1->load());

    ScheduleInput in = input({5, 6}, {}, {0, 10});
    in.previous_first_visible = 0;
    in.previous_last_visible = 1;
    const SchedulePlan plan = s.run_pass(in, out);

    CHECK(f0->load());
    CHECK(f1->load());
    CHECK(has(plan.cancel, 0));
    CHECK(has(plan.cancel, 1));
    REQUIRE(s.slot(0) != nullptr);
    REQUIRE(s.slot(1) != nullptr);
    CHECK(s.slot(0)->kind == PageStateKind::None);
    CHECK(s.slot(1)->kind == PageStateKind::None);
    CHECK(out.released_pages.empty());
}

TEST_CASE("StripSchedule: a pending page whose flag was set outside is swept and requested again",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.run_pass(input({0, 0}), out);
    REQUIRE(out.submitted.size() == 1);
    const std::uint64_t first_seq = out.submitted[0].seq;
    out.submitted[0].flag->store(true);  // an outside cancel, no completion

    ScheduleInput in = input({0, 0});
    in.previous_first_visible = 0;
    in.previous_last_visible = 0;
    const SchedulePlan plan = s.run_pass(in, out);

    CHECK(plan.sweep == std::vector<int>{0});
    CHECK(plan.submit_p0 == std::vector<int>{0});
    REQUIRE(out.submitted.size() == 2);
    CHECK(out.submitted[1].page == 0);
    CHECK(out.submitted[1].priority == 0);
    REQUIRE(s.slot(0) != nullptr);
    CHECK(s.slot(0)->kind == PageStateKind::Pending);
    CHECK(s.slot(0)->seq == out.submitted[1].seq);
    CHECK(s.slot(0)->seq != first_seq);
    CHECK_FALSE(s.slot(0)->cancel_flag->load());
}

TEST_CASE("StripSchedule: a P1 page entering V is promoted", "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.run_pass(input({0, 0}, {1, 1}), out);
    REQUIRE(out.pages_at(1) == std::vector<int>{1});
    const Flag p1_flag = out.submitted[1].flag;
    REQUIRE(s.slot(1) != nullptr);
    CHECK(s.slot(1)->priority == 1);

    ScheduleInput in = input({1, 1});
    in.previous_first_visible = 0;
    in.previous_last_visible = 0;
    const SchedulePlan plan = s.run_pass(in, out);

    CHECK(p1_flag->load());
    CHECK(has(plan.cancel, 1));
    CHECK(plan.submit_p0 == std::vector<int>{1});
    CHECK(out.submitted.back().page == 1);
    CHECK(out.submitted.back().priority == 0);
    CHECK(s.slot(1)->kind == PageStateKind::Pending);
    CHECK(s.slot(1)->priority == 0);
    CHECK(s.slot(1)->seq == out.submitted.back().seq);
}

TEST_CASE("StripSchedule: a page that failed at P1 gets one P0 when it enters V, one that failed at P0 does not",
          "[ui][strip]") {
    {
        StripRenderState s;
        FakeSubmitter out;
        s.slots()[2] = failed(1);
        const SchedulePlan plan = s.run_pass(input({2, 2}), out);
        CHECK(has(plan.fail_reset, 2));
        CHECK(plan.submit_p0 == std::vector<int>{2});
        CHECK(s.slot(2)->kind == PageStateKind::Pending);
        CHECK(s.slot(2)->priority == 0);
    }
    {
        StripRenderState s;
        FakeSubmitter out;
        s.slots()[3] = failed(0);
        const SchedulePlan in_v = s.run_pass(input({3, 3}), out);
        CHECK(in_v.submit_p0.empty());
        CHECK(in_v.fail_reset.empty());
        CHECK(out.submitted.empty());
        CHECK(s.slot(3)->kind == PageStateKind::Failed);

        ScheduleInput in = input({5, 5}, {}, {0, 10});
        in.previous_first_visible = 3;
        in.previous_last_visible = 3;
        const SchedulePlan left = s.run_pass(in, out);
        CHECK(has(left.fail_reset, 3));
        REQUIRE(s.slot(3) != nullptr);
        CHECK(s.slot(3)->kind == PageStateKind::None);
    }
}

TEST_CASE("StripSchedule: two quick scale changes keep the stale bitmap", "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[0] = ready(1.0f);

    s.change_render_params();
    CHECK(s.slot(0)->kind == PageStateKind::Ready);
    CHECK(is_stale(*s.slot(0), 2.0f, false));

    ScheduleInput in = input({0, 0});
    in.scale = 2.0f;
    s.run_pass(in, out);
    REQUIRE(s.slot(0) != nullptr);
    CHECK(s.slot(0)->kind == PageStateKind::Pending);
    CHECK(s.slot(0)->has_stale_bitmap);
    CHECK(s.slot(0)->stale_scale == 1.0f);
    CHECK(s.slot(0)->scale == 2.0f);
    const Flag first = s.slot(0)->cancel_flag;

    s.change_render_params();
    CHECK(first->load());
    CHECK(s.slot(0)->kind == PageStateKind::Ready);
    CHECK(s.slot(0)->scale == 1.0f);
    CHECK(is_stale(*s.slot(0), 3.0f, false));

    in.scale = 3.0f;
    in.previous_first_visible = 0;
    in.previous_last_visible = 0;
    s.run_pass(in, out);
    CHECK(s.slot(0)->kind == PageStateKind::Pending);
    CHECK(s.slot(0)->has_stale_bitmap);
    CHECK(s.slot(0)->stale_scale == 1.0f);
    CHECK(s.slot(0)->scale == 3.0f);
}

TEST_CASE("StripSchedule: thumbnails are cancelled only when V changes", "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    const SchedulePlan first = s.run_pass(input({0, 1}), out);
    CHECK(first.visible_changed);

    ScheduleInput same = input({0, 1});
    same.previous_first_visible = 0;
    same.previous_last_visible = 1;
    const SchedulePlan second = s.run_pass(same, out);
    CHECK_FALSE(second.visible_changed);
    CHECK(out.thumbnail_cancels == 1);

    ScheduleInput moved = input({1, 2});
    moved.previous_first_visible = 0;
    moved.previous_last_visible = 1;
    const SchedulePlan third = s.run_pass(moved, out);
    CHECK(third.visible_changed);
    CHECK(out.thumbnail_cancels == 2);
}

TEST_CASE("StripSchedule: pages outside retain are released", "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[2] = ready(1.0f);

    const SchedulePlan plan = s.run_pass(input({10, 10}, {}, {9, 11}), out);

    CHECK(plan.release == std::vector<int>{2});
    CHECK(out.released_count(2) == 1);
    CHECK(s.slot(2) == nullptr);
}

TEST_CASE("StripSchedule: drop_bitmaps keeps pending pages pending", "[ui][strip]") {
    StripRenderState s;
    s.slots()[0] = ready(1.0f);
    PageSlot p = pending(4, 0, fresh_flag());
    p.has_stale_bitmap = true;
    p.stale_scale = 1.0f;
    s.slots()[1] = p;

    s.drop_bitmaps();

    CHECK(s.slot(0)->kind == PageStateKind::None);
    CHECK(s.slot(1)->kind == PageStateKind::Pending);
    CHECK(s.slot(1)->seq == 4);
    CHECK_FALSE(s.slot(1)->has_stale_bitmap);
    CHECK_FALSE(s.slot(1)->cancel_flag->load());
}

TEST_CASE("StripSchedule: a text request goes out only for a page still in L1",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    const Flag own = fresh_flag();
    s.slots()[4] = pending(7, 0, own);

    auto slot4_unchanged = [&] {
        REQUIRE(s.slot(4) != nullptr);
        CHECK(s.slot(4)->kind == PageStateKind::Pending);
        CHECK(s.slot(4)->seq == 7);
        CHECK(s.slot(4)->priority == 0);
        CHECK(s.slot(4)->cancel_flag == own);
        CHECK_FALSE(own->load());
    };

    CHECK(s.request_text(4, false, false, true, out));
    REQUIRE(out.submitted.size() == 1);
    CHECK(out.submitted[0].page == 4);
    CHECK(out.submitted[0].priority == 1);
    CHECK(out.submitted[0].with_text);
    slot4_unchanged();

    CHECK(s.request_text(5, false, false, true, out));
    REQUIRE(out.submitted.size() == 2);
    CHECK(out.submitted[0].flag->load());  // page 4's text-only request
    CHECK(out.submitted[1].page == 5);
    CHECK(out.submitted[1].priority == 1);
    CHECK(out.submitted[1].with_text);
    CHECK(s.slot(5) == nullptr);
    slot4_unchanged();

    CHECK_FALSE(s.request_text(6, false, false, false, out));  // not in L1
    CHECK_FALSE(s.request_text(6, true, false, true, out));    // text in the slot
    CHECK_FALSE(s.request_text(6, false, true, true, out));    // text pending
    CHECK(out.submitted.size() == 2);
    CHECK_FALSE(out.submitted[1].flag->load());
    slot4_unchanged();
}

TEST_CASE("StripSchedule: prefetch range follows the travel direction", "[ui][strip]") {
    const StripLayout l = three_pages();

    const StripRange down_from_top = prefetch_range(l, 100, 100, StripTravel::Down);
    CHECK(down_from_top.first == 1);
    CHECK(down_from_top.last == 1);

    const StripRange up_from_bottom = prefetch_range(l, 400, 100, StripTravel::Up);
    CHECK(up_from_bottom.first == 1);
    CHECK(up_from_bottom.last == 1);

    CHECK(prefetch_range(l, 400, 100, StripTravel::Down).empty());
    CHECK(prefetch_range(l, 0, 100, StripTravel::Up).empty());
}

TEST_CASE("StripSchedule: a page that failed at P0 is requested again after a scale change",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[3] = failed(0);

    s.change_render_params();
    CHECK(s.slot(3)->kind == PageStateKind::None);

    ScheduleInput in = input({3, 3});
    in.scale = 2.0f;
    const SchedulePlan plan = s.run_pass(in, out);
    CHECK(plan.submit_p0 == std::vector<int>{3});
    CHECK(s.slot(3)->kind == PageStateKind::Pending);
    CHECK(s.slot(3)->scale == 2.0f);
}

TEST_CASE("StripSchedule: a pending page outside retain is cancelled once and released",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    const Flag f = fresh_flag();
    s.slots()[2] = pending(1, 0, f);

    const SchedulePlan plan = s.run_pass(input({10, 10}, {}, {9, 11}), out);

    CHECK(has(plan.release, 2));
    CHECK_FALSE(has(plan.cancel, 2));
    CHECK(f->load());
    CHECK(out.released_count(2) == 1);
    CHECK(s.slot(2) == nullptr);
}

TEST_CASE("StripSchedule: an identical second pass adds no prefetch", "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    // |V| = 4 of 5 entries: page 4 takes the last one.
    const SchedulePlan first = s.run_pass(input({0, 3}, {4, 5}), out);
    CHECK(first.submit_p1 == std::vector<int>{4});

    ScheduleInput again = input({0, 3}, {4, 5});
    again.previous_first_visible = 0;
    again.previous_last_visible = 3;
    const SchedulePlan second = s.run_pass(again, out);
    CHECK(second.submit_p1.empty());
    CHECK(second.submit_p0.empty());
    CHECK(out.pages_at(1) == std::vector<int>{4});
    CHECK(s.slot(5) == nullptr);
    // The prefetch in flight is left alone, not cancelled and sent again.
    CHECK_FALSE(has(second.cancel, 4));
    REQUIRE(s.slot(4) != nullptr);
    CHECK(s.slot(4)->kind == PageStateKind::Pending);
    CHECK_FALSE(s.slot(4)->cancel_flag->load());
}

TEST_CASE("StripSchedule: a resident prefetch page counts against L1 wherever it sits in P",
          "[ui][strip]") {
    // |V| = 4 of 5 entries, and page 6 (the far end of P) already holds the last.
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[6] = ready(1.0f);
    const SchedulePlan plan = s.run_pass(input({0, 3}, {4, 6}), out);
    CHECK(plan.submit_p1.empty());
    CHECK(out.pages_at(1).empty());
}

TEST_CASE("StripSchedule: travelling up, the nearest prefetch page goes first",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    ScheduleInput in = input({5, 5}, {3, 4});
    in.l1_capacity = 2;  // room for one beside V
    const SchedulePlan plan = s.run_pass(in, out);
    CHECK(plan.submit_p1 == std::vector<int>{4});
}

TEST_CASE("StripSchedule: prefetch stops at the first page that does not fit",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    ScheduleInput in = input({0, 3}, {4, 5});
    in.l1_capacity = 10;
    in.bytes_of = [](int page) { return page == 4 ? 2 * kGiB : kMiB; };
    const SchedulePlan plan = s.run_pass(in, out);
    CHECK(plan.submit_p1.empty());
}

TEST_CASE("StripSchedule: a failed prefetch page holds no L1 room", "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[4] = failed(1);
    // |V| = 4 of 5 entries: page 4 has nothing resident, so page 5 gets the last.
    const SchedulePlan plan = s.run_pass(input({0, 3}, {4, 5}), out);
    CHECK(plan.submit_p1 == std::vector<int>{5});
    CHECK(s.slot(4)->kind == PageStateKind::Failed);
}

TEST_CASE("StripSchedule: a page that failed at P0 and left V into P is prefetched in the same pass",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[4] = failed(0);
    ScheduleInput in = input({5, 5}, {4, 4}, {0, 10});
    in.previous_first_visible = 4;
    in.previous_last_visible = 4;
    const SchedulePlan plan = s.run_pass(in, out);
    CHECK(has(plan.fail_reset, 4));
    CHECK(plan.submit_p1 == std::vector<int>{4});
    REQUIRE(s.slot(4) != nullptr);
    CHECK(s.slot(4)->kind == PageStateKind::Pending);
    CHECK(s.slot(4)->priority == 1);
}

TEST_CASE("StripSchedule: no text rides on P0 when the current page needs no render",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[3] = ready(1.0f);
    ScheduleInput in = input({2, 4});
    in.visible_by_area = {3, 2, 4};
    const SchedulePlan plan = s.run_pass(in, out);
    CHECK(plan.submit_p0 == std::vector<int>{2, 4});
    CHECK(plan.text_on_p0 == -1);
    for (const Submitted& x : out.submitted) CHECK_FALSE(x.with_text);
}

TEST_CASE("StripSchedule: a failed prefetch is not retried while it stays in P",
          "[ui][strip]") {
    StripRenderState s;
    FakeSubmitter out;
    s.slots()[1] = failed(1);
    auto submits_of_1 = [&] {
        return std::count_if(out.submitted.begin(), out.submitted.end(),
                             [](const Submitted& x) { return x.page == 1; });
    };

    const SchedulePlan first = s.run_pass(input({0, 0}, {1, 1}), out);
    CHECK_FALSE(has(first.fail_reset, 1));
    CHECK(s.slot(1)->kind == PageStateKind::Failed);

    ScheduleInput again = input({0, 0}, {1, 1});
    again.previous_first_visible = 0;
    again.previous_last_visible = 0;
    const SchedulePlan second = s.run_pass(again, out);
    CHECK_FALSE(has(second.fail_reset, 1));
    CHECK(s.slot(1)->kind == PageStateKind::Failed);
    CHECK(submits_of_1() == 0);

    ScheduleInput entered = input({1, 1});
    entered.previous_first_visible = 0;
    entered.previous_last_visible = 0;
    const SchedulePlan third = s.run_pass(entered, out);
    CHECK(has(third.fail_reset, 1));
    CHECK(third.submit_p0 == std::vector<int>{1});
    CHECK(submits_of_1() == 1);
    CHECK(out.submitted.back().page == 1);
    CHECK(out.submitted.back().priority == 0);
}
