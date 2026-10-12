#pragma once

// #135: keep at most one notification message queued. Every tab's search scan
// reports each page it finishes from a worker thread, about 200 times a second
// on a large document, and the UI handler reads the session state under the
// session mutex, which the worker holds while it sorts the hits. With a common
// word a handler can take longer than the gap between two reports, so the
// messages queued up and a key press waited behind hundreds of them. With this
// gate the handler runs at most once per queue drain, however fast the reports
// come. Pure, so headless-testable.

#include <atomic>

namespace litepdf::ui {

class PostCoalescer {
public:
    // Worker side, after the state change it reports. True when the caller must
    // post: no message is queued. False when one already is -- the handler reads
    // the state after this change, see release().
    bool claim() noexcept {
        return !pending_.exchange(true, std::memory_order_acq_rel);
    }

    // UI side, FIRST thing in the handler, BEFORE reading the state. A change
    // whose claim() returns false happened before that claim; the exchange here
    // reads the value that claim wrote, so the change is visible to the reads
    // that follow. A change claimed after this call posts a new message.
    void release() noexcept {
        pending_.exchange(false, std::memory_order_acq_rel);
    }

    // The post failed (window gone, queue full): nothing is queued, so the next
    // claim() must post rather than wait for a message that will never come.
    // Claim and post once more after this: a claim() that returned false while
    // the failed one held the gate posted nothing and relied on that message.
    void post_failed() noexcept { release(); }

private:
    std::atomic<bool> pending_{false};
};

}  // namespace litepdf::ui
