#pragma once
// src/printing/PrintProgressDlg.hpp -- Phase 8.5 Tasks 4-5
// In-memory DLGTEMPLATE modal with two modes:
//   - "config":   pre-flight scale picker (Fit / Actual / Custom %)
//   - "progress": "Printing page X of Y" + Cancel button
// Pattern mirrors src/ui/PasswordDialog.* from Phase 8.

#include "printing/PrintAbortFlag.hpp"
#include "printing/PrintGeometry.hpp"  // for ScaleMode

#include <cstddef>
#include <optional>
#include <windows.h>

namespace litepdf::printing {

struct ScaleChoice {
    ScaleMode mode;
    float     custom_pct;  // valid only when mode == CustomPct, in [10, 400]
};

class PrintProgressDlg {
public:
    // Show the pre-flight modal. Returns the user's choice, or nullopt
    // if they clicked Cancel. Default selection is FitToPage 100%.
    [[nodiscard]] static std::optional<ScaleChoice> show_config(HWND parent);

    // Show the progress modal as a *modeless* child of `parent` so the
    // caller's print loop can keep running. The returned object owns the
    // HWND; destruction (or close()) destroys the dialog. Pass the
    // shared abort flag -- Cancel button writes to it.
    PrintProgressDlg(HWND parent, PrintAbortFlag& abort_flag, std::size_t total_pages);
    ~PrintProgressDlg();
    PrintProgressDlg(const PrintProgressDlg&)            = delete;
    PrintProgressDlg& operator=(const PrintProgressDlg&) = delete;

    // Update the "Printing page X of Y" label. Safe to call from the
    // print loop between pages. Pumps queued messages so the dialog
    // stays responsive (Cancel click -> abort_flag).
    void set_progress(std::size_t current_1based);

    void close();

    [[nodiscard]] bool is_valid() const noexcept { return hwnd_ != nullptr; }

    // Internal accessor exposed only for the dialog proc (sets the abort
    // flag from the Cancel button click handler).
    PrintAbortFlag& abort_flag_for_dialog() noexcept { return abort_flag_; }

private:
    HWND            hwnd_         = nullptr;
    PrintAbortFlag& abort_flag_;
    std::size_t     total_pages_  = 0;
};

} // namespace litepdf::printing
