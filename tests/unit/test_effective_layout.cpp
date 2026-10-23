#include <catch2/catch_test_macros.hpp>

#include "ui/detail/EffectiveLayout.hpp"

using namespace litepdf::ui;

namespace {

struct Expect {
    KickReason reason;
    bool refit;
    bool refresh_readout;
    bool restore_tab_position;
    bool relayout;
    bool restyle;
    bool reschedule;
};

bool matches(const Expect& e) {
    const StripKickActions a = strip_kick_actions(e.reason);
    return a.refit == e.refit && a.refresh_readout == e.refresh_readout &&
           a.restore_tab_position == e.restore_tab_position &&
           a.relayout == e.relayout && a.restyle == e.restyle &&
           a.reschedule == e.reschedule;
}

}  // namespace

TEST_CASE("EffectiveLayout: spread wins over the preference",
          "[effective_layout]") {
    CHECK(effective_layout(true, true, true, true) == LayoutKind::PagedSpread);
    CHECK(effective_layout(true, true, false, false) ==
          LayoutKind::PagedSpread);
}

TEST_CASE("EffectiveLayout: the strip needs the preference and a format that passed",
          "[effective_layout]") {
    CHECK(effective_layout(true, false, true, true) == LayoutKind::Strip);
    CHECK(effective_layout(true, false, true, false) == LayoutKind::PagedSingle);
    CHECK(effective_layout(true, false, false, true) == LayoutKind::PagedSingle);
    CHECK(effective_layout(true, false, false, false) ==
          LayoutKind::PagedSingle);
    CHECK(effective_layout(false, false, true, true) == LayoutKind::PagedSingle);
    CHECK(effective_layout(false, true, true, true) == LayoutKind::PagedSingle);
    CHECK(effective_layout(false, false, false, false) ==
          LayoutKind::PagedSingle);
}

TEST_CASE("EffectiveLayout: kick reasons map to the spec table",
          "[effective_layout]") {
    //                    reason                refit  readout restore relayout restyle resched
    CHECK(matches({KickReason::Layout,    true,  true,  false, true,  false, true}));
    CHECK(matches({KickReason::Zoom,      false, true,  false, true,  false, true}));
    CHECK(matches({KickReason::Navigate,  false, false, false, false, false, true}));
    CHECK(matches({KickReason::TabSwitch, true,  true,  true,  true,  false, true}));
    CHECK(matches({KickReason::Restore,   true,  true,  false, true,  false, true}));
    CHECK(matches({KickReason::Restyle,   false, false, false, false, true,  true}));
    CHECK(matches({KickReason::Spread,    true,  true,  false, true,  false, true}));
}

TEST_CASE("EffectiveLayout: a same-index tab switch only reschedules",
          "[effective_layout]") {
    CHECK(tab_switch_reason(2, 2) == KickReason::Navigate);
    CHECK(tab_switch_reason(2, 1) == KickReason::TabSwitch);
    CHECK(tab_switch_reason(0, 3) == KickReason::TabSwitch);
}
