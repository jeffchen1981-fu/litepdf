#pragma once
// src/printing/PrintJob.hpp -- Phase 8.5 Task 6+
// Stack-allocated, single-call orchestrator. Prints the active document
// to a printer chosen by the user via PrintDlg. Returns true on success,
// false on user-cancel or error (errors get a MessageBox of their own).

#include <cstddef>
#include <windows.h>

namespace litepdf::core { class Document; }

namespace litepdf::printing {

struct PrintJob {
    // active_page is reserved for future "print current page only" UX;
    // unused in T2.
    [[nodiscard]] static bool run(HWND parent,
                                  litepdf::core::Document& doc,
                                  std::size_t active_page);
};

} // namespace litepdf::printing
