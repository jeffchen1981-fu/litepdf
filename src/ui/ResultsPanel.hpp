#pragma once
// ui::ResultsPanel — bottom-docked cross-tab search results pane.
// Layout:
//   [Edit query + "Search" hint + close ✕]    ← top row, ~32 DIP tall
//   [ListView LVS_REPORT | LVS_OWNERDATA ]    ← rest
// Columns: File | Page | Snippet.
// On row click: fire OnRowClick(hit_index).
// On query Edit Enter: fire OnQuerySubmit(query).

#include "app/CrossTabSearch.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <windows.h>

namespace litepdf::ui {

class ResultsPanel {
public:
    using OnQuerySubmit = std::function<void(std::wstring query)>;
    using OnRowClick    = std::function<void(std::size_t hit_index)>;
    using OnClose       = std::function<void()>;

    ResultsPanel(HINSTANCE, HWND parent,
                 const litepdf::app::CrossTabSearch& xts);
    ~ResultsPanel();

    ResultsPanel(const ResultsPanel&)            = delete;
    ResultsPanel& operator=(const ResultsPanel&) = delete;

    HWND hwnd() const;

    void show_and_focus_edit();   // show + SetFocus on query Edit
    void hide();
    bool visible() const;

    // Called during MainWindow::on_layout with the full panel rect.
    void set_bounds(const RECT& bounds);

    // Call when CrossTabSearch hits vector grows; triggers
    // ListView_SetItemCountEx(... LVSICF_NOSCROLL).
    void refresh_count();

    void set_on_query_submit(OnQuerySubmit);
    void set_on_row_click(OnRowClick);
    void set_on_close(OnClose);

    // Impl is forward-declared public only so free-standing Win32 procs in
    // ResultsPanel.cpp (the panel WndProc and per-child subclass procs) can
    // name the type without a friend-declaration gauntlet. Its definition
    // still lives in the .cpp — this is PIMPL with looser name lookup.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
