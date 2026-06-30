/*  main_gui.cpp - Windows GUI front-end for LightningDetector

    - Native Win32 window with drag-and-drop support (IDropTarget).
    - Multi-video queue processed in a background thread.
    - Real-time progress bar and log window.
    - JSON saved next to the executable (or chosen by user).
    - Supports MP4, AVI, MKV, MOV, WMV, M4V.
    - Modern themed UI (Common Controls v6 manifest, custom colors/fonts).
    - Settings dialog with working OK/Cancel and per-setting help icons.
*/

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <ole2.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <opencv2/core/ocl.hpp>
#include "LightningDetector.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "msimg32.lib")   // GradientFill (header bar)

// Enable Common Controls v6 (themed buttons / progress bar / etc.) without
// needing a separate .manifest file.
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' " \
    "processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

namespace fs = std::filesystem;

//  Control IDs 
enum {
    ID_BTN_ADD       = 1001,
    ID_BTN_CLEAR     = 1002,
    ID_BTN_ANALYSE   = 1003,
    ID_BTN_SAVEAS    = 1004,
    ID_BTN_SETTINGS  = 1005,
    ID_LISTBOX       = 1006,
    ID_LOG           = 1007,
    ID_PROGRESS      = 1008,
    ID_LABEL_STATUS  = 1009,
    ID_LABEL_DROP    = 1010,
    // Timer
    ID_TIMER_POLL    = 2001,
    // Settings dialog edit boxes
    ID_EDIT_BASE     = 3001,   // 3001..3007
    // Settings dialog help ("?") buttons
    ID_HELP_BASE     = 3101,   // 3101..3107
};

//  Message posted from worker thread 
#define WM_WORKER_DONE  (WM_USER + 10)
#define WM_WORKER_PROG  (WM_USER + 11)

//  Theme 
static const COLORREF COLOR_BG      = RGB(244, 246, 250);
static const COLORREF COLOR_HEADER1 = RGB(28, 78, 186);
static const COLORREF COLOR_HEADER2 = RGB(56, 124, 235);
static const COLORREF COLOR_TEXT    = RGB(35, 40, 48);
static const COLORREF COLOR_SUBTEXT = RGB(120, 128, 140);
static const COLORREF COLOR_ACCENT  = RGB(37, 99, 235);
static const int      HEADER_H      = 64;

static HFONT  g_fontUI     = nullptr;   // Segoe UI 9pt, regular controls
static HFONT  g_fontBold   = nullptr;   // Segoe UI 9pt bold, headings/labels
static HFONT  g_fontTitle  = nullptr;   // Segoe UI 14pt bold, header title
static HFONT  g_fontSmall  = nullptr;   // Segoe UI 8pt, subtitle / hints
static HBRUSH g_bgBrush    = nullptr;

//  Globals 
static HWND g_hwnd          = nullptr;
static HWND g_listbox       = nullptr;
static HWND g_log           = nullptr;
static HWND g_progress      = nullptr;
static HWND g_lblStatus     = nullptr;
static HWND g_btnAnalyse    = nullptr;
static HWND g_btnClear      = nullptr;
static HWND g_btnAdd        = nullptr;
static HWND g_btnSaveAs     = nullptr;

static std::vector<std::wstring> g_videoPaths;
static std::vector<VideoResult>  g_results;
static std::atomic<bool>         g_running{false};
static std::mutex                g_logMutex;
static std::string               g_lastJsonPath;

static LightningDetector         g_detector;

// Shared progress info (written by worker, read by main thread via WM_WORKER_PROG)
struct ProgInfo { int vi, vt, fd, ft; std::string msg; };
static std::mutex  g_progMutex;
static ProgInfo    g_progInfo;

//  Utility 

static std::string wstr_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
    return s;
}

static std::wstring utf8_to_wstr(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(sz - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), sz);
    return w;
}

static void SetUIFont(HWND h, HFONT f) {
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

static void AppendLog(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    auto wm = utf8_to_wstr("[" + []{
        auto t = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(t);
        char buf[16]; std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&tt));
        return std::string(buf);
    }() + "] " + msg + "\r\n");

    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)wm.c_str());
    SendMessageW(g_log, EM_SCROLL, SB_BOTTOM, 0);
}

static void RefreshFileList() {
    SendMessageW(g_listbox, LB_RESETCONTENT, 0, 0);
    for (auto& p : g_videoPaths) {
        std::wstring name = fs::path(p).filename().wstring();
        SendMessageW(g_listbox, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
    std::wstring title = L"Lightning Detector  [" + std::to_wstring(g_videoPaths.size()) + L" file(s)]";
    SetWindowTextW(g_hwnd, title.c_str());
}

static bool AddVideoPath(const std::wstring& path) {
    static const std::vector<std::wstring> exts =
        {L".mp4", L".avi", L".mkv", L".mov", L".wmv", L".m4v", L".mpg", L".mpeg", L".ts", L".flv"};
    std::wstring ext = fs::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (std::find(exts.begin(), exts.end(), ext) == exts.end()) return false;
    for (auto& p : g_videoPaths) if (p == path) return false;
    g_videoPaths.push_back(path);
    return true;
}

//  Drop Target 

class DropTarget : public IDropTarget {
    ULONG m_refs = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --m_refs; }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDO, DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDO, DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg = {};
        if (SUCCEEDED(pDO->GetData(&fmt, &stg))) {
            HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
            if (hDrop) {
                int count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                int added = 0;
                for (int i = 0; i < count; i++) {
                    WCHAR buf[MAX_PATH];
                    DragQueryFileW(hDrop, i, buf, MAX_PATH);
                    if (AddVideoPath(buf)) added++;
                }
                GlobalUnlock(stg.hGlobal);
                if (added > 0) {
                    RefreshFileList();
                    AppendLog("Added " + std::to_string(added) + " video(s) via drag-and-drop.");
                }
            }
            ReleaseStgMedium(&stg);
        }
        return S_OK;
    }
};

static DropTarget* g_dropTarget = nullptr;

//  Worker thread 

static void WorkerThread(std::vector<std::string> paths) {
    auto prog_cb = [](int vi, int vt, int fd, int ft, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(g_progMutex);
            g_progInfo = {vi, vt, fd, ft, msg};
        }
        PostMessageW(g_hwnd, WM_WORKER_PROG, 0, 0);
    };

    g_results = g_detector.analyseVideos(paths, prog_cb);
    g_running = false;
    PostMessageW(g_hwnd, WM_WORKER_DONE, 0, 0);
}

//  Save JSON helper 

static std::string DefaultJsonPath() {
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path p(exe);
    p = p.parent_path() / "lightning_results.json";
    return wstr_to_utf8(p.wstring());
}

static bool DoSaveJSON(bool ask) {
    std::string outPath = DefaultJsonPath();
    if (ask) {
        WCHAR buf[MAX_PATH] = L"lightning_results.json";
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = g_hwnd;
        ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.Flags       = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = L"json";
        if (!GetSaveFileNameW(&ofn)) return false;
        outPath = wstr_to_utf8(buf);
    }
    bool ok = LightningDetector::saveJSON(g_results, outPath);
    if (ok) {
        g_lastJsonPath = outPath;
        AppendLog("Saved JSON -> " + outPath);
    } else {
        AppendLog("ERROR: Could not save JSON to " + outPath);
    }
    return ok;
}

//  Settings dialog 
//
// Built at runtime as a popup of the predefined "#32770" dialog class.
// IMPORTANT: button clicks (WM_COMMAND) are SENT directly to the dialog's
// window procedure - they never appear in the GetMessage() queue. The
// dialog must therefore have its own window procedure that reacts to
// IDOK / IDCANCEL synchronously. We do this by subclassing the popup
// (SetWindowLongPtr GWLP_WNDPROC) instead of trying to "catch" the click
// in an external message loop, which is what caused OK/Cancel to silently
// do nothing in the previous version.

struct SettingHelp {
    const wchar_t* label;
    const wchar_t* help;
};

static const SettingHelp kSettingHelp[7] = {
    { L"Brightness threshold (0-255):",
      L"How much brighter a pixel must get (0-255) compared to the rolling "
      L"background before it counts as part of a flash.\n\n"
      L"Default: 30\n"
      L"Lower (15-20): catches faint or distant lightning, but may pick up "
      L"other bright events too.\n"
      L"Higher (40-60): requires a more intense flash, cutting false "
      L"positives but possibly missing weak strikes." },

    { L"Min flash pixel fraction (0-1):",
      L"The fraction of the whole frame that must light up at once for it "
      L"to count as lightning (0.05 = 5% of the frame).\n\n"
      L"Default: 0.05\n"
      L"Lower (0.02-0.03): catches small or localized flashes.\n"
      L"Higher (0.10-0.15): requires a much larger portion of the frame to "
      L"flash, which filters out car headlights, camera flashes, and scene "
      L"cuts." },

    { L"Min gap between events (frames):",
      L"How many analyzed 'quiet' frames must pass between two flashes "
      L"before they're counted as two separate events, instead of one.\n\n"
      L"Default: 15\n"
      L"Increase (30-60) for fast, repeated lightning (storm cells) so one "
      L"storm isn't split into many tiny events." },

    { L"Max processing threads (0=auto):",
      L"How many videos are analyzed at the same time, each on its own CPU "
      L"thread.\n\n"
      L"Default: 0 (automatic - uses all CPU cores)\n"
      L"Set a specific number (e.g. 2 or 4) to leave some CPU power free "
      L"for other tasks while analysis runs." },

    { L"Analyse at FPS (0=full, no skip):",
      L"High frame-rate video (120/240 fps) is analyzed at this reduced "
      L"rate to save time, since lightning is easy to detect without "
      L"checking every single frame.\n\n"
      L"Default: 60\n"
      L"Set to 0 to analyze every frame at the original frame rate "
      L"(slower, maximum accuracy). Reported timestamps always match the "
      L"true video time, regardless of this setting." },

    { L"Max analysis dimension px (0=full):",
      L"Frames are shrunk so their longest side is at most this many "
      L"pixels before analysis. Lightning is a large, frame-wide "
      L"brightness event, so this barely affects detection while greatly "
      L"speeding up processing.\n\n"
      L"Default: 720\n"
      L"Set to 0 to analyze at full original resolution (slower, "
      L"marginally more precise)." },

    { L"Use GPU acceleration (1=on, 0=off):",
      L"Offloads per-frame image processing (color conversion, frame "
      L"differencing, thresholding) to your GPU via OpenCL, instead of "
      L"doing all of it on the CPU. This is what cuts CPU usage during "
      L"analysis.\n\n"
      L"Default: 1 (on)\n"
      L"If your GPU/driver doesn't support OpenCL, this automatically "
      L"and silently falls back to CPU processing with identical "
      L"results - set to 0 only if you want to force CPU-only "
      L"processing (e.g. to free up the GPU for something else)." },
};

struct SettingsDlgState {
    bool done      = false;
    bool okPressed = false;
    HWND edits[7]  = {};
};

static WNDPROC g_origSettingsProc = nullptr;

static void ShowSettingHelp(HWND owner, int idx) {
    if (idx < 0 || idx >= 6) return;
    std::wstring title = L"Help: ";
    title += kSettingHelp[idx].label;
    MessageBoxW(owner, kSettingHelp[idx].help, title.c_str(), MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK SettingsSubclassProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsDlgState* st = (SettingsDlgState*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) {
            if (st) { st->okPressed = true; st->done = true; }
            return 0;
        }
        if (id == IDCANCEL) {
            if (st) { st->okPressed = false; st->done = true; }
            return 0;
        }
        if (id >= ID_HELP_BASE && id < ID_HELP_BASE + 7) {
            ShowSettingHelp(hDlg, id - ID_HELP_BASE);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) { st->okPressed = false; st->done = true; }
        return 0;
    case WM_NOTIFY: {
        // Tooltip "GetDispInfo" for hover help text on edit boxes / "?" buttons.
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp;
            int ctrlId = (int)GetWindowLongPtrW((HWND)hdr->idFrom, GWL_ID);
            int idx = -1;
            if (ctrlId >= ID_HELP_BASE && ctrlId < ID_HELP_BASE + 7) idx = ctrlId - ID_HELP_BASE;
            else if (ctrlId >= ID_EDIT_BASE && ctrlId < ID_EDIT_BASE + 7) idx = ctrlId - ID_EDIT_BASE;
            if (idx >= 0) di->lpszText = (LPWSTR)kSettingHelp[idx].help;
            return 0;
        }
        break;
    }
    }
    return CallWindowProcW(g_origSettingsProc, hDlg, msg, wp, lp);
}

// Returns true if the user pressed OK (and writes new values into *p).
static bool ShowSettingsDialog(HWND owner, LightningDetector::Params* p) {
    const int DLG_W = 460, DLG_H = 410;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"#32770", L"Detection Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        0, 0, DLG_W, DLG_H, owner, nullptr, nullptr, nullptr);
    if (!hDlg) return false;
    SetUIFont(hDlg, g_fontUI);

    auto lbl = [&](const wchar_t* t, int x, int y, int w = 260) {
        HWND h = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, x, y, w, 18, hDlg, nullptr, nullptr, nullptr);
        SetUIFont(h, g_fontUI);
        return h;
    };
    auto edt = [&](int id, int x, int y) -> HWND {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, x, y, 80, 22, hDlg, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SetUIFont(h, g_fontUI);
        return h;
    };
    auto helpBtn = [&](int id, int x, int y) -> HWND {
        HWND h = CreateWindowW(L"BUTTON", L"?",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, 24, 22, hDlg, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SetUIFont(h, g_fontBold);
        return h;
    };

    SettingsDlgState st;
    int rowY = 16;
    const int rowGap = 38;
    for (int i = 0; i < 7; i++) {
        lbl(kSettingHelp[i].label, 14, rowY + 3);
        st.edits[i] = edt(ID_EDIT_BASE + i, 286, rowY);
        helpBtn(ID_HELP_BASE + i, 376, rowY);
        rowY += rowGap;
    }

    lbl(L"Tip: click any '?' or hover over a field for a full explanation.",
        14, rowY + 4, DLG_W - 50);

    HWND btnOK = CreateWindowW(L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, DLG_W/2 - 110, DLG_H - 80, 100, 30, hDlg, (HMENU)IDOK, nullptr, nullptr);
    HWND btnCN = CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, DLG_W/2 + 10, DLG_H - 80, 100, 30, hDlg, (HMENU)IDCANCEL, nullptr, nullptr);
    SetUIFont(btnOK, g_fontUI);
    SetUIFont(btnCN, g_fontUI);

    // Tooltip control (hover help for edit boxes + "?" buttons)
    HWND hTip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hDlg, nullptr, nullptr, nullptr);
    SetWindowPos(hTip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (int i = 0; i < 7; i++) {
        TOOLINFOW ti = {};
        ti.cbSize   = sizeof(ti);
        ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd     = hDlg;
        ti.uId      = (UINT_PTR)st.edits[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

        TOOLINFOW ti2 = ti;
        ti2.uId = (UINT_PTR)GetDlgItem(hDlg, ID_HELP_BASE + i);
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti2);
    }

    // Populate current values
    auto setNum = [&](HWND h, double v, int decimals) {
        std::wostringstream ss;
        if (decimals > 0) ss << std::fixed << std::setprecision(decimals) << v;
        else ss << (long long)v;
        SetWindowTextW(h, ss.str().c_str());
    };
    setNum(st.edits[0], p->brightness_threshold, 0);
    setNum(st.edits[1], p->pixel_fraction_required, 3);
    setNum(st.edits[2], p->min_event_gap_frames, 0);
    setNum(st.edits[3], p->max_threads, 0);
    setNum(st.edits[4], p->target_analysis_fps, 0);
    setNum(st.edits[5], p->analysis_max_dimension, 0);
    setNum(st.edits[6], p->use_gpu ? 1 : 0, 0);

    SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)&st);
    g_origSettingsProc = (WNDPROC)SetWindowLongPtrW(hDlg, GWLP_WNDPROC, (LONG_PTR)SettingsSubclassProc);

    // Center over owner
    RECT rc; GetWindowRect(owner, &rc);
    SetWindowPos(hDlg, nullptr,
        rc.left + (rc.right - rc.left - DLG_W) / 2,
        rc.top  + (rc.bottom - rc.top - DLG_H) / 2,
        DLG_W, DLG_H, SWP_NOZORDER);
    ShowWindow(hDlg, SW_SHOW);
    EnableWindow(owner, FALSE);
    SetFocus(st.edits[0]);

    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hDlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (st.okPressed) {
        WCHAR buf[64];
        GetWindowTextW(st.edits[0], buf, 64); p->brightness_threshold    = (float)_wtof(buf);
        GetWindowTextW(st.edits[1], buf, 64); p->pixel_fraction_required = (float)_wtof(buf);
        GetWindowTextW(st.edits[2], buf, 64); p->min_event_gap_frames    = _wtoi(buf);
        GetWindowTextW(st.edits[3], buf, 64); p->max_threads             = _wtoi(buf);
        GetWindowTextW(st.edits[4], buf, 64); p->target_analysis_fps     = _wtof(buf);
        GetWindowTextW(st.edits[5], buf, 64); p->analysis_max_dimension  = _wtoi(buf);
        GetWindowTextW(st.edits[6], buf, 64); p->use_gpu                 = (_wtoi(buf) != 0);
    }

    EnableWindow(owner, TRUE);
    DestroyWindow(hDlg);
    SetForegroundWindow(owner);
    return st.okPressed;
}

//  Window procedure 

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        //  Buttons (positioned below the header bar) 
        const int topY = HEADER_H + 14;
        g_btnAdd = CreateWindowW(L"BUTTON", L"Add Videos...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 10, topY, 120, 32, hwnd, (HMENU)ID_BTN_ADD, nullptr, nullptr);
        g_btnClear = CreateWindowW(L"BUTTON", L"Clear List",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 140, topY, 100, 32, hwnd, (HMENU)ID_BTN_CLEAR, nullptr, nullptr);
        HWND btnSettings = CreateWindowW(L"BUTTON", L"Settings...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 250, topY, 100, 32, hwnd, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
        g_btnAnalyse = CreateWindowW(L"BUTTON", L">  Analyse",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 360, topY, 120, 32, hwnd, (HMENU)ID_BTN_ANALYSE, nullptr, nullptr);
        g_btnSaveAs = CreateWindowW(L"BUTTON", L"Save JSON As...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 490, topY, 130, 32, hwnd, (HMENU)ID_BTN_SAVEAS, nullptr, nullptr);

        //  Drop zone label 
        HWND lblDrop = CreateWindowW(L"STATIC",
            L"Drag & drop video files here, or use 'Add Videos' above",
            WS_CHILD|WS_VISIBLE|SS_CENTER,
            10, topY + 40, 760, 20, hwnd, (HMENU)ID_LABEL_DROP, nullptr, nullptr);

        //  File list 
        g_listbox = CreateWindowW(L"LISTBOX", L"",
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,
            10, topY + 65, 760, 150, hwnd, (HMENU)ID_LISTBOX, nullptr, nullptr);

        //  Status label 
        g_lblStatus = CreateWindowW(L"STATIC", L"Idle - add videos to get started.",
            WS_CHILD|WS_VISIBLE,
            10, topY + 225, 760, 20, hwnd, (HMENU)ID_LABEL_STATUS, nullptr, nullptr);

        //  Progress bar 
        g_progress = CreateWindowW(PROGRESS_CLASSW, L"",
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            10, topY + 250, 760, 18, hwnd, (HMENU)ID_PROGRESS, nullptr, nullptr);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

        //  Log 
        g_log = CreateWindowW(L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            10, topY + 275, 760, 280, hwnd, (HMENU)ID_LOG, nullptr, nullptr);

        // Fonts
        g_fontUI    = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontBold  = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontTitle = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_bgBrush   = CreateSolidBrush(COLOR_BG);

        SetUIFont(g_btnAdd, g_fontUI);
        SetUIFont(g_btnClear, g_fontUI);
        SetUIFont(btnSettings, g_fontUI);
        SetUIFont(g_btnAnalyse, g_fontBold);
        SetUIFont(g_btnSaveAs, g_fontUI);
        SetUIFont(lblDrop, g_fontUI);
        SetUIFont(g_listbox, g_fontUI);
        SetUIFont(g_lblStatus, g_fontUI);
        SetUIFont(g_log, CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas"));

        AppendLog("Lightning Detector ready.");
        AppendLog("Drag & drop video files into this window, or click 'Add Videos'.");
        AppendLog("Supports: MP4, AVI, MKV, MOV, WMV, M4V, MPG, TS, FLV.");
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // we paint the whole client area ourselves (header + bg)

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client; GetClientRect(hwnd, &client);

        // Background
        HBRUSH bg = CreateSolidBrush(COLOR_BG);
        RECT bgRect = client; bgRect.top = HEADER_H;
        FillRect(dc, &bgRect, bg);
        DeleteObject(bg);

        // Header gradient (simple two-band fill to fake a gradient cheaply)
        RECT hdr = { 0, 0, client.right, HEADER_H };
        TRIVERTEX vtx[2];
        vtx[0].x = 0; vtx[0].y = 0;
        vtx[0].Red = GetRValue(COLOR_HEADER1) << 8; vtx[0].Green = GetGValue(COLOR_HEADER1) << 8;
        vtx[0].Blue = GetBValue(COLOR_HEADER1) << 8; vtx[0].Alpha = 0;
        vtx[1].x = client.right; vtx[1].y = HEADER_H;
        vtx[1].Red = GetRValue(COLOR_HEADER2) << 8; vtx[1].Green = GetGValue(COLOR_HEADER2) << 8;
        vtx[1].Blue = GetBValue(COLOR_HEADER2) << 8; vtx[1].Alpha = 0;
        GRADIENT_RECT gr = { 0, 1 };
        GradientFill(dc, vtx, 2, &gr, 1, GRADIENT_FILL_RECT_H);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255,255,255));
        HFONT old = (HFONT)SelectObject(dc, g_fontTitle);
        RECT titleRect = { 18, 8, client.right - 20, 36 };
        DrawTextW(dc, L"Lightning Detector", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_fontSmall);
        RECT subRect = { 20, 36, client.right - 20, 58 };
        DrawTextW(dc, L"Video lightning detection & analysis", -1, &subRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, COLOR_TEXT);
        return (LRESULT)g_bgBrush;
    }

    case WM_SIZE: {
        int W = LOWORD(lp), H = HIWORD(lp);
        if (W < 100 || H < 100) break;
        const int topY = HEADER_H + 14;
        MoveWindow(g_btnAdd,     10, topY,        120, 32, TRUE);
        MoveWindow(g_btnClear,   140, topY,       100, 32, TRUE);
        MoveWindow(g_listbox,    10, topY + 65,  W-20, 150, TRUE);
        MoveWindow(g_lblStatus,  10, topY + 225,  W-20,  20, TRUE);
        MoveWindow(g_progress,   10, topY + 250,  W-20,  18, TRUE);
        MoveWindow(g_log,        10, topY + 275,  W-20, H - (topY + 285), TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_BTN_ADD) {
            WCHAR buf[32768] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize  = sizeof(ofn);
            ofn.hwndOwner    = hwnd;
            ofn.lpstrFilter  = L"Video Files\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.m4v;*.mpg;*.mpeg;*.ts;*.flv\0All Files\0*.*\0";
            ofn.lpstrFile    = buf;
            ofn.nMaxFile     = sizeof(buf)/sizeof(WCHAR);
            ofn.Flags        = OFN_FILEMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
            if (GetOpenFileNameW(&ofn)) {
                std::wstring dir(buf);
                WCHAR* p = buf + ofn.nFileOffset;
                int added = 0;
                if (*(p - 1) == L'\0' && *p) {
                    while (*p) {
                        std::wstring full = dir + L"\\" + p;
                        if (AddVideoPath(full)) added++;
                        p += wcslen(p) + 1;
                    }
                } else {
                    if (AddVideoPath(dir)) added++;
                }
                if (added) {
                    RefreshFileList();
                    AppendLog("Added " + std::to_string(added) + " video(s).");
                }
            }
        }
        else if (id == ID_BTN_CLEAR) {
            if (!g_running) {
                g_videoPaths.clear();
                RefreshFileList();
                AppendLog("File list cleared.");
            }
        }
        else if (id == ID_BTN_SETTINGS) {
            auto p = g_detector.getParams();
            if (ShowSettingsDialog(hwnd, &p)) {
                g_detector.setParams(p);
                AppendLog("Settings updated.");
            } else {
                AppendLog("Settings unchanged (cancelled).");
            }
        }
        else if (id == ID_BTN_ANALYSE) {
            if (g_running) break;
            if (g_videoPaths.empty()) {
                MessageBoxW(hwnd, L"Please add at least one video file first.",
                    L"No Videos", MB_OK|MB_ICONINFORMATION);
                break;
            }
            g_running = true;
            g_results.clear();
            EnableWindow(g_btnAnalyse, FALSE);
            EnableWindow(g_btnAdd,     FALSE);
            EnableWindow(g_btnClear,   FALSE);

            SendMessageW(g_progress, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_lblStatus, L"Starting analysis...");
            AppendLog("");
            AppendLog("Starting analysis of " + std::to_string(g_videoPaths.size()) + " video(s)...");

            std::vector<std::string> paths;
            for (auto& w : g_videoPaths) paths.push_back(wstr_to_utf8(w));

            std::thread(WorkerThread, paths).detach();
        }
        else if (id == ID_BTN_SAVEAS) {
            if (g_results.empty()) {
                MessageBoxW(hwnd, L"No results to save yet. Run analysis first.",
                    L"Nothing to save", MB_OK|MB_ICONINFORMATION);
                break;
            }
            DoSaveJSON(true);
        }
        break;
    }

    case WM_WORKER_PROG: {
        ProgInfo pi;
        { std::lock_guard<std::mutex> lk(g_progMutex); pi = g_progInfo; }
        double overall = ((double)pi.vi + (pi.ft > 0 ? (double)pi.fd / pi.ft : 0.0)) / std::max(pi.vt, 1);
        SendMessageW(g_progress, PBM_SETPOS, (WPARAM)(int)(overall * 1000), 0);
        std::wstring status = L"Video " + std::to_wstring(pi.vi + 1) + L"/" + std::to_wstring(pi.vt) +
            L"  Frame " + std::to_wstring(pi.fd) + L"/" + std::to_wstring(pi.ft) +
            L"  - " + utf8_to_wstr(pi.msg);
        SetWindowTextW(g_lblStatus, status.c_str());
        break;
    }

    case WM_WORKER_DONE: {
        SendMessageW(g_progress, PBM_SETPOS, 1000, 0);
        SetWindowTextW(g_lblStatus, L"Analysis complete.");
        EnableWindow(g_btnAnalyse, TRUE);
        EnableWindow(g_btnAdd,     TRUE);
        EnableWindow(g_btnClear,   TRUE);

        AppendLog("");
        int total_events = 0;
        for (auto& r : g_results) {
            if (r.success) {
                total_events += (int)r.events.size();
                std::ostringstream os;
                os << r.video_filename << ": " << r.events.size()
                   << " event(s) detected in "
                   << std::fixed << std::setprecision(2) << r.processing_time_seconds << "s";
                AppendLog(os.str());
                for (auto& ev : r.events) {
                    std::ostringstream es;
                    es << "  [" << ev.description << "] "
                       << "t=" << std::fixed << std::setprecision(3) << ev.timestamp_seconds << "s"
                       << "  frame=" << ev.frame_number
                       << "  confidence=" << std::setprecision(1) << (ev.confidence*100.f) << "%";
                    AppendLog(es.str());
                }
            } else {
                AppendLog("ERROR - " + r.video_filename + ": " + r.error_message);
            }
        }
        AppendLog("Total lightning events detected: " + std::to_string(total_events));

        DoSaveJSON(false);
        AppendLog("");
        MessageBoxW(hwnd,
            (L"Analysis complete!\n\n"
             L"Lightning events found: " + std::to_wstring(total_events) + L"\n"
             L"Results saved to:\n" + utf8_to_wstr(g_lastJsonPath)).c_str(),
            L"Done", MB_OK|MB_ICONINFORMATION);
        break;
    }

    case WM_CLOSE:
        if (g_running) {
            if (MessageBoxW(hwnd, L"Analysis is running. Force quit?",
                L"Quit?", MB_YESNO|MB_ICONWARNING) != IDYES) break;
        }
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        if (g_dropTarget) {
            RevokeDragDrop(hwnd);
            g_dropTarget->Release();
        }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

//  WinMain 

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icx);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"LightningDetectorWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint the background ourselves (WM_ERASEBKGND/WM_PAINT)
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"LightningDetectorWnd",
        L"Lightning Detector",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 810, 700,
        nullptr, nullptr, hInst, nullptr);

    g_dropTarget = new DropTarget();
    RegisterDragDrop(g_hwnd, g_dropTarget);

    // Warm up the GPU/OpenCL context in the background so the window shows
    // instantly (no startup stall) while the GPU is still ready by the
    // time the user clicks Analyse.
    std::thread([]{ (void)cv::ocl::haveOpenCL(); }).detach();

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    return (int)msg.wParam;
}
