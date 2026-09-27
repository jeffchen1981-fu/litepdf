#include <catch2/catch_test_macros.hpp>

#include "cli/bench_selection.hpp"
#include "core/Document.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <windows.h>

// Contract for litepdf-cli's `--bench-selection` (#73). The timings themselves
// are machine-dependent, so their size is never asserted; what is pinned is
// that every figure is actually measured (> 0) on a page with text, and that a
// page the harness cannot get a handle for fails loudly instead of printing a
// row of zeros. The two helpers that keep the machine out of the numbers --
// a warm file cache and an efficiency-core flag -- are pinned below.

namespace {
constexpr const char* kFixture = "tests/fixtures/selection.pdf";
}

TEST_CASE("cli bench-selection measures every figure on a page with text",
          "[cli][bench][selection]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open(kFixture).has_value());

    litepdf::cli::SelectionPageTiming t;
    REQUIRE(litepdf::cli::bench_selection_page(kFixture, doc, 0, 3, t));
    CHECK(t.page == 0);
    CHECK(t.chars > 0);
    CHECK(t.quads > 0);
    CHECK(t.scan_first_ms > 0.0);
    CHECK(t.acquire_first_ms > 0.0);
    CHECK(t.acquire_seq_ms > 0.0);
    CHECK(t.acquire_ms > 0.0);
    CHECK(t.search_ms > 0.0);
    for (int m = 0; m < 3; ++m) {
        CHECK(t.move_full_ms[m] > 0.0);
        CHECK(t.move_short_ms[m] > 0.0);
        CHECK(t.release_full_ms[m] > 0.0);
    }
    CHECK(t.select_all_ms > 0.0);
}

TEST_CASE("cli bench-selection fails on a page it cannot open",
          "[cli][bench][selection]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open(kFixture).has_value());

    litepdf::cli::SelectionPageTiming t;
    CHECK_FALSE(litepdf::cli::bench_selection_page(
        kFixture, doc, static_cast<int>(doc.page_count()), 1, t));
}

TEST_CASE("cli bench-selection reads the whole file into the cache",
          "[cli][bench][selection]") {
    // Every column measures CPU cost: the first reader of a page's bytes on a
    // cold file cache otherwise waits on the disk, and only that one does.
    const auto size = std::filesystem::file_size(kFixture);
    CHECK(litepdf::cli::warm_file_cache(kFixture) == size);
    CHECK(litepdf::cli::warm_file_cache("tests/fixtures/no-such-file.pdf") == 0);
}

namespace {

// Independent of the harness's own detection: CPU sets, not processor
// relationships. Only the processors this thread may run on, in its current
// processor group -- a restricted affinity (`start /affinity`, a job object)
// makes pinning to any other one fail, and a machine with more than 64
// logical processors may start the process in a group other than 0.
struct ClassedCpu { WORD group; unsigned number; unsigned char efficiency_class; };

std::vector<ClassedCpu> allowed_cpus() {
    GROUP_AFFINITY current{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &current)) return {};
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<unsigned char> buf(len);
    auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data());
    std::vector<ClassedCpu> out;
    if (!GetSystemCpuSetInformation(first, len, &len, GetCurrentProcess(), 0)) return out;
    for (ULONG off = 0; off < len;) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
        const unsigned number = info->CpuSet.LogicalProcessorIndex;
        if (info->Type == CpuSetInformation && info->CpuSet.Group == current.Group
            && number < 64 && (current.Mask & (KAFFINITY{ 1 } << number)) != 0) {
            out.push_back({ current.Group, number, info->CpuSet.EfficiencyClass });
        }
        off += info->Size;
    }
    return out;
}

// on_efficiency_core() with the thread pinned to one processor.
bool pinned_answer(const ClassedCpu& cpu) {
    GROUP_AFFINITY pin{};
    pin.Group = cpu.group;
    pin.Mask  = KAFFINITY{ 1 } << cpu.number;
    GROUP_AFFINITY old{};
    REQUIRE(SetThreadGroupAffinity(GetCurrentThread(), &pin, &old));   // else: some other core
    const bool answer = litepdf::cli::on_efficiency_core();
    SetThreadGroupAffinity(GetCurrentThread(), &old, nullptr);
    return answer;
}

}  // namespace

TEST_CASE("cli bench-selection flags an efficiency core and nothing else",
          "[cli][bench][selection]") {
    // The opt-out is a scheduling preference with no observable effect on a
    // pinned thread; only that Windows accepts it is pinned here.
    CHECK(litepdf::cli::prefer_performance_cores());

    const auto cpus = allowed_cpus();
    REQUIRE_FALSE(cpus.empty());
    // Classes are ranked machine-wide, so the reference is every processor's
    // highest, not only the allowed ones'.
    unsigned char top = 0, bottom = 255;
    {
        ULONG len = 0;
        GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<unsigned char> buf(len);
        auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data());
        REQUIRE(GetSystemCpuSetInformation(first, len, &len, GetCurrentProcess(), 0));
        for (ULONG off = 0; off < len;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
            if (info->Type == CpuSetInformation) {
                top    = (std::max)(top, info->CpuSet.EfficiencyClass);
                bottom = (std::min)(bottom, info->CpuSet.EfficiencyClass);
            }
            off += info->Size;
        }
    }
    // Not SKIP: an all-skipped Catch2 run exits 4, which ctest -- one test per
    // process, and no SKIP_RETURN_CODE -- reports as a failure on a
    // single-class CI runner.
    // Every allowed processor, so each class present is checked both ways.
    for (const auto& c : cpus) {
        INFO("processor " << c.group << ":" << c.number << ", class " << int{ c.efficiency_class });
        CHECK(pinned_answer(c) == (c.efficiency_class < top));
    }
    if (top == bottom) WARN("one core class: no efficiency core to pin to");
}
