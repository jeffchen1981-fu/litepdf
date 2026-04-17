#include "ui/TabManager.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>

#pragma comment(lib, "comctl32.lib")

namespace litepdf::ui {

namespace {
constexpr UINT_PTR kTabSubclassId = 0xAB01;
}  // namespace

// Forward declaration at namespace scope (not anonymous) so the friend
// declaration in TabManager matches by exact linkage / lookup.
LRESULT CALLBACK tab_subclass_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                   UINT_PTR id, DWORD_PTR ref_data);

struct TabManager::Impl {
    HWND                        hwnd = nullptr;
    litepdf::core::TabList      list;
    TabManager::SwitchCb        on_switch;
    TabManager::CloseRequestCb  on_close_request;

    // Called by the subclass proc on WM_MBUTTONUP when the hit-tested tab
    // index is valid. Kept as a member (rather than inline in the proc) so
    // the callback invocation lives in a named C++ function, which static
    // analyzers prefer over loose access from a free-function subclass proc.
    void fire_close_request(int index) {
        if (on_close_request) on_close_request(index);
    }
};

TabManager::TabManager(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    impl_->hwnd = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
        0, 0, 0, 0,
        parent, nullptr, hInstance, nullptr);
    // Subclass so we can catch WM_MBUTTONUP (middle-click close).
    SetWindowSubclass(impl_->hwnd, tab_subclass_proc,
                      kTabSubclassId, reinterpret_cast<DWORD_PTR>(this));
}

TabManager::~TabManager() {
    if (impl_ && impl_->hwnd) {
        RemoveWindowSubclass(impl_->hwnd, tab_subclass_proc, kTabSubclassId);
        DestroyWindow(impl_->hwnd);
    }
}

HWND TabManager::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }
int  TabManager::count()        const { return impl_ ? static_cast<int>(impl_->list.size()) : 0; }
int  TabManager::active_index() const { return impl_ ? impl_->list.active_index() : -1; }

litepdf::core::Tab* TabManager::active_tab() {
    return impl_ ? impl_->list.active() : nullptr;
}
litepdf::core::Tab* TabManager::tab_at(int index) {
    if (!impl_ || index < 0) return nullptr;
    return impl_->list.at(static_cast<std::size_t>(index));
}

int TabManager::add_tab(std::unique_ptr<litepdf::core::Tab> t) {
    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT;
    tci.pszText = const_cast<LPWSTR>(t->label.c_str());

    const int old_active = impl_->list.active_index();
    const int new_index  = static_cast<int>(impl_->list.add(std::move(t)));
    SendMessageW(impl_->hwnd, TCM_INSERTITEMW,
                 static_cast<WPARAM>(new_index),
                 reinterpret_cast<LPARAM>(&tci));

    impl_->list.set_active(static_cast<std::size_t>(new_index));
    SendMessageW(impl_->hwnd, TCM_SETCURSEL,
                 static_cast<WPARAM>(new_index), 0);
    if (impl_->on_switch) impl_->on_switch(new_index, old_active);
    return new_index;
}

void TabManager::close_tab(int index) {
    if (index < 0 || index >= count()) return;
    const int old_active = impl_->list.active_index();
    SendMessageW(impl_->hwnd, TCM_DELETEITEM,
                 static_cast<WPARAM>(index), 0);
    const int new_active = impl_->list.remove(static_cast<std::size_t>(index));
    if (new_active >= 0) {
        SendMessageW(impl_->hwnd, TCM_SETCURSEL,
                     static_cast<WPARAM>(new_active), 0);
    }
    if (new_active != old_active && impl_->on_switch) {
        impl_->on_switch(new_active, old_active);
    }
}

bool TabManager::set_active(int index) {
    if (index < 0) return false;
    const int old_active = impl_->list.active_index();
    if (!impl_->list.set_active(static_cast<std::size_t>(index))) return false;
    SendMessageW(impl_->hwnd, TCM_SETCURSEL,
                 static_cast<WPARAM>(index), 0);
    if (impl_->on_switch) impl_->on_switch(index, old_active);
    return true;
}

void TabManager::set_tab_label(int index, const std::wstring& label) {
    if (index < 0 || index >= count()) return;
    if (auto* t = impl_->list.at(static_cast<std::size_t>(index))) {
        t->label = label;
    }
    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT;
    tci.pszText = const_cast<LPWSTR>(label.c_str());
    SendMessageW(impl_->hwnd, TCM_SETITEMW,
                 static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&tci));
}

void TabManager::set_on_switch(SwitchCb cb) {
    impl_->on_switch = std::move(cb);
}
void TabManager::set_on_close_request(CloseRequestCb cb) {
    impl_->on_close_request = std::move(cb);
}

bool TabManager::handle_notify(const NMHDR* hdr) {
    if (!hdr || hdr->hwndFrom != impl_->hwnd) return false;
    if (hdr->code == TCN_SELCHANGE) {
        const int new_index = static_cast<int>(
            SendMessageW(impl_->hwnd, TCM_GETCURSEL, 0, 0));
        const int old_active = impl_->list.active_index();
        if (new_index >= 0 && new_index != old_active) {
            impl_->list.set_active(static_cast<std::size_t>(new_index));
            if (impl_->on_switch) impl_->on_switch(new_index, old_active);
        }
        return true;
    }
    return false;
}

int TabManager::strip_height(UINT dpi) const {
    const int baseline = 24;
    return MulDiv(baseline, static_cast<int>(dpi), 96);
}

void TabManager::set_visible(bool v) {
    if (impl_ && impl_->hwnd) {
        ShowWindow(impl_->hwnd, v ? SW_SHOW : SW_HIDE);
    }
}

LRESULT CALLBACK tab_subclass_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                   UINT_PTR /*id*/, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<TabManager*>(ref_data);
    if (msg == WM_MBUTTONUP && self && self->impl_) {
        TCHITTESTINFO hti = {};
        hti.pt.x = GET_X_LPARAM(l);
        hti.pt.y = GET_Y_LPARAM(l);
        const int hit = static_cast<int>(
            SendMessageW(hwnd, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&hti)));
        if (hit >= 0) {
            self->impl_->fire_close_request(hit);
        }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

}  // namespace litepdf::ui
