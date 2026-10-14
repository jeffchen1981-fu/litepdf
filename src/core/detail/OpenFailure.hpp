#pragma once

// core-internal (#137): the text a failed MuPDF open step throws, shared by
// SearchSession's and RenderEngine's constructors. MainWindow catches every
// open failure with `...` and shows a generic message, so the reason MuPDF
// gave is also written to stderr, like Document's page_hits log.

#include <cstdio>
#include <string>

namespace litepdf::core::detail {

// `step` is the fixed text the step has always thrown; `reason` is the text
// fz_convert_error returned in the fz_catch that saw the failure, or empty
// when the step failed without a MuPDF error (a password MuPDF declined,
// say). With no reason the fixed text is returned as is and nothing
// is logged. Neither argument may carry the password or anything derived
// from it: no open, needs-password or authenticate error message does.
inline std::string open_failure(const std::string& step, const std::string& reason) {
    if (reason.empty()) return step;
    std::fprintf(stderr, "litepdf: %s: %s\n", step.c_str(), reason.c_str());
    return step + ": " + reason;
}

}  // namespace litepdf::core::detail
