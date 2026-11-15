#pragma once

// #229: when a SearchSession opens its own copy of the tab's document.

namespace litepdf::core {

enum class SearchOpen {
    // In the constructor: an open that fails throws from it. The default,
    // and what every session with a password does.
    AtConstruction,
    // At the first scan, on the task's thread, so a tab that is never
    // searched never parses the file a second time. Worth it only for a
    // format whose open is expensive (FB2 parses the whole book at open).
    // A failed open there does not throw: the scan ends with no hits and
    // SearchSession::open_failed() is true.
    AtFirstScan,
};

}  // namespace litepdf::core
