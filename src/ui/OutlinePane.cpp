#include "ui/OutlinePane.hpp"

#include <commctrl.h>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace litepdf::ui {

struct OutlinePane::Impl {
    HWND tree = nullptr;
    NavigateCb on_navigate;
    // Map from HTREEITEM -> page_index for click-to-navigate.
    // We store page indices in the item's lParam.
    WNDPROC original_parent_proc = nullptr;
    HWND parent = nullptr;
};

// We subclass the parent to intercept WM_NOTIFY from the TreeView.
// This is simpler than creating a dedicated parent HWND.
// However, since MainWindow already handles WM_NOTIFY, we'll use
// a different approach: store a static map and forward from MainWindow.
//
// Actually, the simplest Win32 pattern: MainWindow forwards
// WM_NOTIFY to OutlinePane via a public method. No subclassing needed.

OutlinePane::OutlinePane(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>()) {
    impl_->parent = parent;

    // TreeView control - WS_CHILD, initially hidden.
    impl_->tree = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_TREEVIEWW,
        L"",
        WS_CHILD | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT
            | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
        0, 0, 250, 100,
        parent,
        nullptr,
        hInstance,
        nullptr);
}

OutlinePane::~OutlinePane() {
    if (impl_->tree) {
        DestroyWindow(impl_->tree);
        impl_->tree = nullptr;
    }
}

HWND OutlinePane::hwnd() const { return impl_->tree; }

void OutlinePane::set_on_navigate(NavigateCb cb) {
    impl_->on_navigate = std::move(cb);
}

void OutlinePane::populate(
        const std::vector<litepdf::core::Document::OutlineEntry>& entries) {
    if (!impl_->tree) return;
    TreeView_DeleteAllItems(impl_->tree);

    // Track parent HTREEITEM at each depth so we can build the tree
    // hierarchy from the flat list.
    std::vector<HTREEITEM> depth_stack;  // depth_stack[d] = parent at depth d

    for (const auto& e : entries) {
        TVINSERTSTRUCTW tvi = {};
        tvi.hInsertAfter = TVI_LAST;

        // Determine parent: depth 0 = root, depth N = child of depth N-1.
        if (e.depth == 0 || depth_stack.empty()) {
            tvi.hParent = TVI_ROOT;
        } else {
            int parent_depth = e.depth - 1;
            if (parent_depth < static_cast<int>(depth_stack.size())) {
                tvi.hParent = depth_stack[static_cast<std::size_t>(parent_depth)];
            } else {
                tvi.hParent = depth_stack.back();
            }
        }

        // Convert UTF-8 title to UTF-16 for the TreeView.
        int wlen = MultiByteToWideChar(CP_UTF8, 0,
                       e.title.c_str(), static_cast<int>(e.title.size()),
                       nullptr, 0);
        std::wstring wtitle(static_cast<std::size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0,
            e.title.c_str(), static_cast<int>(e.title.size()),
            wtitle.data(), wlen);

        tvi.item.mask    = TVIF_TEXT | TVIF_PARAM;
        tvi.item.pszText = const_cast<wchar_t*>(wtitle.c_str());
        // Store page_index in lParam. Use -1 for kNoPage.
        tvi.item.lParam  = (e.page_index == litepdf::core::Document::kNoPage)
                               ? static_cast<LPARAM>(-1)
                               : static_cast<LPARAM>(e.page_index);

        HTREEITEM item = TreeView_InsertItem(impl_->tree, &tvi);

        // Maintain depth_stack: ensure it has an entry for this depth.
        auto d = static_cast<std::size_t>(e.depth);
        if (d < depth_stack.size()) {
            depth_stack[d] = item;
            // Trim anything deeper - they're from a previous sibling branch.
            depth_stack.resize(d + 1);
        } else {
            // Fill gaps if the outline skips depths (shouldn't happen in
            // well-formed PDFs, but be defensive).
            while (depth_stack.size() < d) {
                depth_stack.push_back(item);
            }
            depth_stack.push_back(item);
        }
    }

    // Expand the first level so the outline is immediately useful.
    HTREEITEM root = TreeView_GetRoot(impl_->tree);
    while (root) {
        TreeView_Expand(impl_->tree, root, TVE_EXPAND);
        root = TreeView_GetNextSibling(impl_->tree, root);
    }
}

void OutlinePane::clear() {
    if (impl_->tree)
        TreeView_DeleteAllItems(impl_->tree);
}

void OutlinePane::show() {
    if (impl_->tree)
        ShowWindow(impl_->tree, SW_SHOW);
}

void OutlinePane::hide() {
    if (impl_->tree)
        ShowWindow(impl_->tree, SW_HIDE);
}

bool OutlinePane::visible() const {
    return impl_->tree && IsWindowVisible(impl_->tree);
}

}  // namespace litepdf::ui
