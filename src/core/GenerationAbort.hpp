#pragma once
// src/core/GenerationAbort.hpp
// Per-generation abort token for SearchSession (#130): each search generation
// (one set_query) owns a distinct fz_cookie. Its tasks pass the cookie to the
// text build, so `abort` set from another thread stops the build at MuPDF's
// next poll, and the match loop then returns whatever hits it already has.
//
// begin_generation() sets the PREVIOUS generation's cookie `abort` -- so a scan
// still building a page for the superseded query stops instead of finishing a
// now-stale page -- and returns a fresh, value-initialised cookie for the new
// generation. A cookie is never reset: once the application sets `abort`,
// MuPDF's contract forbids changing it again (device.h, the fz_cookie comment;
// spec §3.4). That is also why one shared cookie cannot serve every
// generation: the next one would need `abort` back at 0.
//
// The returned shared_ptr keeps the cookie alive for as long as a task holds
// it, so a superseded cookie stays valid across a generation swap. Our writes
// and reads of `abort` are interlocked (detail::request_abort /
// abort_requested); MuPDF's own read is a plain load, an accepted,
// vendor-defined data race (spec §3.4). The generation's tasks share the
// cookie, and the session mutex serialises them, so MuPDF's writes to
// `progress`, `errors` and `incomplete` never race each other.
//
// Thread-safety: mutation (begin_generation / abort_active) is single-threaded
// (SearchSession's UI-thread set_query and destructor). Workers only use their
// own captured cookie, never this object, so no internal locking is needed.
//
// Names fz_cookie, so only MuPDF-aware translation units include this header;
// SearchSession.hpp does not.

#include "core/detail/PageHitsAccess.hpp"

#include <memory>

namespace litepdf::core {

class GenerationAbort {
public:
    // Abort the previous generation (if any) and start a fresh one. Returns
    // the new generation's cookie (all fields 0); callers pass its raw pointer
    // to the text build and keep the shared_ptr alive for the scan.
    std::shared_ptr<fz_cookie> begin_generation() {
        if (current_) detail::request_abort(*current_);
        current_ = std::make_shared<fz_cookie>();  // value-initialised
        return current_;
    }

    // Abort the active generation's cookie (teardown path). No-op if no
    // generation has started.
    void abort_active() {
        if (current_) detail::request_abort(*current_);
    }

private:
    std::shared_ptr<fz_cookie> current_;
};

}  // namespace litepdf::core
