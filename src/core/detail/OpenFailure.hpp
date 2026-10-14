#pragma once

// core-internal (#137): the text a failed MuPDF open step throws, shared by
// SearchSession's and RenderEngine's constructors. MainWindow catches every
// open failure with `...` and shows a generic message, so the reason MuPDF
// gave is also written to stderr, like Document's page_hits log.

#include <cstddef>
#include <cstdio>
#include <string>

namespace litepdf::core::detail {

// Size of the buffer a constructor copies MuPDF's message into inside its
// fz_catch: the size of MuPDF's own (fz_error_context::message), so nothing
// is cut short. A fixed buffer, because the copy must not allocate: a
// bad_alloc there would skip the rollback that follows it.
inline constexpr std::size_t kOpenFailureReasonSize = 256;

// `step` is the fixed text the step has always thrown; `reason` is the text
// fz_convert_error returned in the fz_catch that saw the failure, or empty
// when the step failed without a MuPDF error (a password MuPDF declined,
// say). With no reason the fixed text is returned as is and nothing
// is logged. Neither argument may carry the password or anything derived
// from it: no open, needs-password or authenticate error message does.
// Called after any rollback: it allocates, so it may throw bad_alloc.
inline std::string open_failure(const std::string& step, const char* reason) {
    if (reason[0] == '\0') return step;
    std::fprintf(stderr, "litepdf: %s: %s\n", step.c_str(), reason);
    return step + ": " + reason;
}

}  // namespace litepdf::core::detail
