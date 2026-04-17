#pragma once

// ui::TabManager — Win32 SysTabControl32 wrapper owning a core::TabList.
// Header stays PIMPL-clean; the only Win32 types exposed are HWND/HINSTANCE
// which appear throughout the UI layer already.

#include "core/TabList.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class TabManager {
public:
    using SwitchCb       = std::function<void(int new_index, int old_index)>;
    using CloseRequestCb = std::function<void(int index)>;

    TabManager(HINSTANCE hInstance, HWND parent);
    ~TabManager();

    TabManager(const TabManager&)            = delete;
    TabManager& operator=(const TabManager&) = delete;

    HWND hwnd() const;

    // Append a tab and activate it. Returns the new index.
    int add_tab(std::unique_ptr<litepdf::core::Tab> t);

    // Remove the tab at index. The active-index policy from TabList
    // decides the next active tab; SwitchCb fires if the active tab
    // changed as a result.
    void close_tab(int index);

    int                     count() const;
    int                     active_index() const;
    litepdf::core::Tab*     active_tab();
    litepdf::core::Tab*     tab_at(int index);

    // Programmatic activate (e.g. Ctrl+1..9). Fires SwitchCb.
    bool set_active(int index);

    // Update the label shown in the tab header.
    void set_tab_label(int index, const std::wstring& label);

    void set_on_switch(SwitchCb cb);
    void set_on_close_request(CloseRequestCb cb);

    // Parent's WM_NOTIFY for TCN_SELCHANGE routes here so TabManager
    // can emit SwitchCb. Returns true if the notification was handled.
    bool handle_notify(const NMHDR* hdr);

    // Reserve vertical space: writes tab-strip height into *h_out in
    // pixels for the given DPI. Used by MainWindow::on_layout().
    int  strip_height(UINT dpi) const;

    // Show / hide the tab strip (hidden when count()==0 per D1).
    void set_visible(bool v);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // The Win32 subclass proc (defined in the .cpp, anonymous namespace)
    // needs to reach impl_ to invoke on_close_request. Friend-declared here
    // so it can name TabManager::Impl without making Impl public.
    friend LRESULT CALLBACK tab_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                              UINT_PTR, DWORD_PTR);
};

}  // namespace litepdf::ui
