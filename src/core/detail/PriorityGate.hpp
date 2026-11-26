#pragma once

// core::detail::PriorityGate -- the lock on one Document's fz_document and
// fz_context (#229). It replaces Document::Impl's std::mutex for every
// Document. A Document that lends nothing has only Foreground users, so for it
// the gate behaves as that mutex did.
//
// Classes, in order:
//   Foreground  the Document's own methods (page_size, text_page, outline,
//               print...), in the app called on the UI thread.
//   Render      a render worker that borrows the Document's document
//               (detail::SharedDocumentAccess, an FB2 tab).
//   Scan        a search session's scan task on that borrowed document.
//
// Admission (spec §3.3), decided under the gate's own mutex, so a borrower
// that started waiting before the UI thread cannot slip in after it:
//   - Foreground first, always.
//   - Render before Scan, but a waiting Scan is admitted after at most
//     kScanAfterRenders consecutive Render holds.
//   - Inside a class, first come, first served: each acquire takes a ticket
//     and is admitted only when its ticket is the oldest unserved one.
// Bounds that follow: a Foreground caller waits for the hold in progress plus
// older Foreground callers; a Render caller for the hold in progress, any
// Foreground holds that arrive meanwhile and at most one Scan hold; a Scan
// caller for at most kScanAfterRenders Render holds plus Foreground holds.
//
// Invariant: only MuPDF work on the document or the Document's context, and
// copying its results (the Document's own caches of MuPDF results included),
// runs under the gate. No callback into UI or caller code, no message pump, no
// wait on another thread. So the UI thread never waits for a borrower while it
// holds the gate.
//
// Lock order: SearchSession's scan_mtx may be held while acquiring the gate.
// Nothing else may, and the holder takes no other lock (the MuPDF family's
// lock table aside, which is a leaf).
//
// Re-entry on the owning thread throws std::system_error
// (resource_deadlock_would_occur), as std::mutex::lock() does on MSVC, which
// Document::text_page turns into an empty handle (#70). The check comes BEFORE
// the ticket is taken: a throw after it would leave a ticket that is never
// served, and from then on nothing of a lower class would be admitted -- the
// render worker would stop and closing the tab would hang in ~SearchSession and
// ~RenderEngine.
//
// Hold it through GateHold only: methods under it throw, and a leaked hold
// would hang every other user of the Document.
//
// MuPDF-free.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace litepdf::core::detail {

enum class GateClass { Foreground, Render, Scan };

// A waiting Scan is admitted after this many consecutive Render holds.
inline constexpr unsigned kScanAfterRenders = 4;

// The gate's whole state, kept apart from the gate so the admission rule can be
// tested without threads.
struct GateState {
    bool held = false;
    // Per class, indexed by static_cast<std::size_t>(GateClass): the next
    // ticket to hand out and the oldest ticket not yet served. Their
    // difference is the number of waiters (plus none: a served ticket is no
    // longer waiting).
    std::uint64_t next_ticket[3] = {};
    std::uint64_t serving[3]     = {};
    // Render holds since the last Scan hold.
    unsigned renders_since_scan = 0;
};

// Whether the acquire holding `ticket` in class `cls` may take the gate now.
bool admissible(const GateState& s, GateClass cls, std::uint64_t ticket) noexcept;

class PriorityGate {
public:
    PriorityGate() = default;
    PriorityGate(const PriorityGate&)            = delete;
    PriorityGate& operator=(const PriorityGate&) = delete;
    PriorityGate(PriorityGate&&)                 = delete;
    PriorityGate& operator=(PriorityGate&&)      = delete;

    // Blocks until admitted. Throws std::system_error
    // (resource_deadlock_would_occur) when this thread already holds the gate.
    void acquire(GateClass cls);
    void release() noexcept;

    // Tickets taken minus served in `cls`: the callers waiting in that class.
    // For tests.
    std::size_t waiting(GateClass cls) const;

    // Holds admitted in `cls` since the gate was made, the one in progress
    // included. For tests.
    std::uint64_t served(GateClass cls) const;

private:
    mutable std::mutex      m_;
    std::condition_variable cv_;
    GateState               st_;
    std::thread::id         owner_;
};

// Holds the gate for its lifetime.
class GateHold {
public:
    GateHold(PriorityGate& gate, GateClass cls) : gate_(gate) { gate_.acquire(cls); }
    ~GateHold() { gate_.release(); }

    GateHold(const GateHold&)            = delete;
    GateHold& operator=(const GateHold&) = delete;
    GateHold(GateHold&&)                 = delete;
    GateHold& operator=(GateHold&&)      = delete;

private:
    PriorityGate& gate_;
};

}  // namespace litepdf::core::detail
