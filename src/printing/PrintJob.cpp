// src/printing/PrintJob.cpp -- Phase 8.5 Task 6
// PRINTPAGERANGE and the lpPageRanges / nPageRanges / nMaxPageRanges
// fields live on PRINTDLGEXW (PrintDlgExW), not PRINTDLGW. The plan
// referred to PRINTDLG.lpPageRanges; that was an error in the plan
// (those fields don't exist on PRINTDLG[W]). We use PrintDlgExW which
// is the modern Win32 print API and what real-world readers (SumatraPDF,
// Foxit) use.
#ifndef WINVER
#define WINVER       0x0601  // Windows 7 baseline
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
// commdlg.h gates PRINTPAGERANGE / PRINTDLGEXW under
// #ifdef STDMETHOD which lives in <objbase.h>. The project's global
// WIN32_LEAN_AND_MEAN suppresses OLE/COM auto-includes from windows.h,
// so we include objbase.h explicitly here before commdlg.h.
#include <objbase.h>
#include <commdlg.h>

#include "printing/PrintJob.hpp"
#include "printing/PrintAbortFlag.hpp"
#include "printing/PrintGeometry.hpp"
#include "printing/PrintProgressDlg.hpp"
#include "printing/PrintRange.hpp"
#include "core/Document.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdi32.lib")

namespace litepdf::printing {

namespace {

// Convert PRINTPAGERANGE[] (Win32) -> our PageRange[] (POD).
std::vector<PageRange> to_page_ranges(PRINTPAGERANGE* p, std::size_t n) {
    std::vector<PageRange> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back({ p[i].nFromPage, p[i].nToPage });
    }
    return out;
}

void show_error(HWND parent, const wchar_t* msg) {
    MessageBoxW(parent, msg, L"Print", MB_OK | MB_ICONERROR);
}

} // anonymous namespace

bool PrintJob::run(HWND parent,
                   litepdf::core::Document& doc,
                   std::size_t /*active_page*/)
{
    if (!doc.is_open() || doc.page_count() == 0) {
        show_error(parent, L"No document is open.");
        return false;
    }

    // [1] Pre-flight scale picker.
    auto scale = PrintProgressDlg::show_config(parent);
    if (!scale) return false;
    (void)scale;  // Task 7 wires this into the render call.

    // [2] OS PrintDlgEx (modern API; supports lpPageRanges).
    PRINTPAGERANGE ranges[10] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize    = sizeof(pd);
    pd.hwndOwner      = parent;
    pd.Flags          = PD_RETURNDC | PD_NOSELECTION
                      | PD_NOCURRENTPAGE
                      | PD_USEDEVMODECOPIESANDCOLLATE
                      | PD_COLLATE;
    pd.nMinPage       = 1;
    pd.nMaxPage       = static_cast<DWORD>(
                          std::min<std::size_t>(doc.page_count(), 0xFFFF));
    pd.nCopies        = 1;
    pd.lpPageRanges   = ranges;
    pd.nMaxPageRanges = 10;
    pd.nPageRanges    = 0;
    pd.nStartPage     = START_PAGE_GENERAL;

    HRESULT hr = PrintDlgExW(&pd);
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"PrintDlgEx failed (hr=0x%08X).",
                   static_cast<unsigned>(hr));
        show_error(parent, buf);
        return false;
    }
    if (pd.dwResultAction != PD_RESULT_PRINT) {
        // PD_RESULT_CANCEL or PD_RESULT_APPLY -- silently return.
        if (pd.hDC)       DeleteDC(pd.hDC);
        if (pd.hDevMode)  GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return false;
    }

    // RAII: PD_RETURNDC transferred HDC ownership to us.
    struct HdcOwner { HDC h; ~HdcOwner() { if (h) DeleteDC(h); } };
    HdcOwner hdc_owner{ pd.hDC };

    // RAII: PrintDlgEx allocates DEVMODE/DEVNAMES via GMEM_MOVEABLE.
    struct GMemOwner { HGLOBAL h; ~GMemOwner() { if (h) GlobalFree(h); } };
    GMemOwner devmode_owner { pd.hDevMode  };
    GMemOwner devnames_owner{ pd.hDevNames };

    // [3] Compute the page list.
    std::vector<std::size_t> pages;
    if (pd.Flags & PD_PAGENUMS) {
        std::vector<PageRange> pr = to_page_ranges(
            pd.lpPageRanges, static_cast<std::size_t>(pd.nPageRanges));
        pages = parse_page_ranges(pr.data(), pr.size(), doc.page_count());
    } else {
        pages.reserve(doc.page_count());
        for (std::size_t i = 0; i < doc.page_count(); ++i) pages.push_back(i);
    }
    if (pages.empty()) {
        show_error(parent, L"No pages selected.");
        return false;
    }

    // Copies: PrintDlgEx returns the resolved nCopies on the struct itself
    // when PD_USEDEVMODECOPIESANDCOLLATE is set.
    DWORD copies = std::max<DWORD>(1, pd.nCopies);

    // [4] StartDoc.
    DOCINFOW di = { sizeof(di) };
    auto leaf = doc.source_path().filename().wstring();
    di.lpszDocName = leaf.empty() ? L"LitePDF" : leaf.c_str();
    if (StartDocW(pd.hDC, &di) <= 0) {
        show_error(parent, L"Failed to start print job.");
        return false;
    }

    // [5] Skeleton page loop -- no rendering yet (Task 7 fills it in).
    PrintAbortFlag abort_flag;
    PrintProgressDlg progress(parent, abort_flag, pages.size() * copies);
    std::size_t emitted = 0;
    bool error_aborted = false;
    for (DWORD c = 0; c < copies && !abort_flag.is_aborted() && !error_aborted; ++c) {
        for (std::size_t p : pages) {
            if (abort_flag.is_aborted()) break;
            ++emitted;
            progress.set_progress(emitted);
            if (StartPage(pd.hDC) <= 0) {
                AbortDoc(pd.hDC);
                show_error(parent, L"Printer reported an error mid-job.");
                error_aborted = true;
                break;
            }
            // Render goes here in Task 7.
            (void)p;
            EndPage(pd.hDC);
        }
    }

    if (error_aborted) return false;

    // [6] EndDoc / AbortDoc.
    if (abort_flag.is_aborted()) {
        AbortDoc(pd.hDC);
        return false;
    }
    EndDoc(pd.hDC);
    return true;
}

} // namespace litepdf::printing
