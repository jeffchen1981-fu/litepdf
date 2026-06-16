#pragma once

// ui::FindBar — floating Ctrl+F bar anchored to canvas top-right.
// Child HWND (WS_CHILD | WS_CLIPSIBLINGS); NOT WS_POPUP (popups detach
// from parent lifecycle — see Phase 6 design §4 D6).
//
// Hosts: Edit(query), Static(counter), buttons prev/next/case/close.
// Emits callbacks to MainWindow for query change, next, prev, close.

#include <functional>
#include <memory>
#include <string>
#include <windows.h>

namespace litepdf::ui {

class FindBar {
public:
    // text, match_case, whole_word, regex.
    using QueryChanged =
        std::function<void(std::wstring, bool, bool, bool)>;
    using NavAction    = std::function<void()>;

    FindBar(HINSTANCE hInstance, HWND parent);
    ~FindBar();

    FindBar(const FindBar&)            = delete;
    FindBar& operator=(const FindBar&) = delete;

    HWND hwnd() const;

    // Show or focus the bar; text argument is prefill for Edit. If caller
    // passes the last session's query, typing ctrl+f twice in a row lets
    // the user continue where they left off.
    void show_or_focus(const std::wstring& prefill);
    void hide();
    bool visible() const;

    // Re-anchor to canvas top-right. Caller computes canvas rect in
    // MainWindow::on_layout() and passes it here. 16 DIP right margin,
    // 8 DIP top margin.
    void reposition(const RECT& canvas_rect);

    // Counter Static text (e.g., "3 / 12" or "3 / 12+" for scanning, "" for idle).
    void set_counter(const std::wstring& txt);

    // Mark the query field as holding an invalid pattern (e.g. a malformed
    // regex). Paints the Edit text red; cleared automatically on the next
    // successful run or edit. Idempotent.
    void set_invalid_pattern(bool invalid);

    // Callback wiring from MainWindow. All fire on UI thread.
    void set_on_query_changed(QueryChanged cb);
    void set_on_next(NavAction cb);
    void set_on_prev(NavAction cb);
    void set_on_close(NavAction cb);

    // Impl is forward-declared public only so free-standing Win32 procs in
    // FindBar.cpp (the bar WndProc and per-child subclass procs) can name
    // the type without a friend-declaration gauntlet. Its definition still
    // lives in the .cpp — this is PIMPL with a slightly looser name lookup.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
