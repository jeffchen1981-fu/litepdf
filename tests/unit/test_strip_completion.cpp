#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "ui/detail/StripCompletion.hpp"
#include "ui/detail/StripSchedule.hpp"

using namespace litepdf::ui;

namespace {

PageSlot pending(std::uint64_t seq, float scale = 1.0f, int priority = 0) {
    PageSlot s;
    s.kind = PageStateKind::Pending;
    s.seq = seq;
    s.priority = priority;
    s.scale = scale;
    return s;
}

StripCompletionMeta meta(std::uint64_t epoch, int page, std::uint64_t seq,
                         bool cancelled = false) {
    StripCompletionMeta m;
    m.epoch = epoch;
    m.page = page;
    m.seq = seq;
    m.cancelled = cancelled;
    return m;
}

}  // namespace

TEST_CASE("StripCompletion: a completion from another epoch is dropped") {
    const PageSlot s = pending(7);
    CHECK(classify_strip_completion(&s, meta(3, 0, 7), 4, true) == StripCompletionAction::Drop);
    CHECK(classify_strip_completion(&s, meta(3, 0, 7, true), 4, false) ==
          StripCompletionAction::Drop);
    CHECK(classify_strip_completion(&s, meta(3, 0, 7), 3, true) ==
          StripCompletionAction::CreateBitmap);
}

TEST_CASE("StripCompletion: a completion for a page that is not pending is dropped") {
    PageSlot ready;
    ready.kind = PageStateKind::Ready;
    ready.seq = 7;
    CHECK(classify_strip_completion(&ready, meta(4, 0, 7), 4, true) ==
          StripCompletionAction::Drop);
    CHECK(classify_strip_completion(nullptr, meta(4, 0, 7), 4, true) ==
          StripCompletionAction::Drop);

    PageSlot none;
    PageSlot failed;
    failed.kind = PageStateKind::Failed;
    CHECK(classify_strip_completion(&none, meta(4, 0, 0), 4, false) ==
          StripCompletionAction::Drop);
    CHECK(classify_strip_completion(&failed, meta(4, 0, 0), 4, false) ==
          StripCompletionAction::Drop);
}

TEST_CASE("StripCompletion: a stale seq is dropped") {
    const PageSlot s = pending(7);
    CHECK(classify_strip_completion(&s, meta(4, 0, 6), 4, true) == StripCompletionAction::Drop);
    CHECK(classify_strip_completion(&s, meta(4, 0, 8), 4, false) ==
          StripCompletionAction::Drop);
}

TEST_CASE("StripCompletion: a pixmap is created and lands at the submitted scale") {
    const PageSlot s = pending(7, 2.0f);
    REQUIRE(classify_strip_completion(&s, meta(4, 0, 7), 4, true) ==
            StripCompletionAction::CreateBitmap);

    const PageSlot r = after_bitmap(s, BitmapResult::Ok);
    CHECK(r.kind == PageStateKind::Ready);
    CHECK(r.scale == 2.0f);
    CHECK(r.invert == false);
    // The view moved to 3.0 after the submit and the cancel missed it.
    CHECK(is_stale(r, 3.0f, false));
    CHECK_FALSE(is_stale(r, 2.0f, false));

    PageSlot inv = pending(7, 2.0f);
    inv.invert = true;
    const PageSlot ri = after_bitmap(inv, BitmapResult::Ok);
    CHECK(ri.invert == true);
    CHECK(is_stale(ri, 2.0f, false));
}

TEST_CASE("StripCompletion: bitmap failures") {
    const PageSlot recreate = after_bitmap(pending(7, 2.0f), BitmapResult::RecreateOrNoTarget);
    CHECK(recreate.kind == PageStateKind::None);

    const PageSlot failed = after_bitmap(pending(7, 2.0f, 1), BitmapResult::OtherFailure);
    CHECK(failed.kind == PageStateKind::Failed);
    CHECK(failed.priority == 1);

    const PageSlot failed0 = after_bitmap(pending(7, 2.0f, 0), BitmapResult::OtherFailure);
    CHECK(failed0.kind == PageStateKind::Failed);
    CHECK(failed0.priority == 0);
}

TEST_CASE("StripCompletion: cancelled null takes the cancel transition") {
    PageSlot with_stale = pending(7, 2.0f);
    with_stale.has_stale_bitmap = true;
    with_stale.stale_scale = 1.5f;
    with_stale.stale_invert = true;
    REQUIRE(classify_strip_completion(&with_stale, meta(4, 0, 7, true), 4, false) ==
            StripCompletionAction::CancelAndReschedule);
    const PageSlot a = after_null(with_stale, true);
    CHECK(a.kind == PageStateKind::Ready);
    CHECK(a.scale == 1.5f);
    CHECK(a.invert == true);
    CHECK(is_stale(a, 2.0f, true));

    const PageSlot without = pending(7, 2.0f);
    REQUIRE(classify_strip_completion(&without, meta(4, 0, 7, true), 4, false) ==
            StripCompletionAction::CancelAndReschedule);
    CHECK(after_null(without, true).kind == PageStateKind::None);
}

TEST_CASE("StripCompletion: uncancelled null fails") {
    const PageSlot s = pending(7, 2.0f, 1);
    CHECK(classify_strip_completion(&s, meta(4, 0, 7, false), 4, false) ==
          StripCompletionAction::MarkFailed);
    const PageSlot f = after_null(s, false);
    CHECK(f.kind == PageStateKind::Failed);
    CHECK(f.priority == 1);
}
