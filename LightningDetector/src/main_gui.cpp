/*  main_gui.cpp - Lightning Detector GUI

    Fixes in this version:
    - Per-row progress bars drawn via FillRect (no GDI pen/brush conflicts)
    - Save buttons drawn manually inside owner-draw rows; clicks handled via
      listbox subclass WM_LBUTTONDOWN (no invisible child-window positioning hack)
    - Hover highlight on Save buttons via WM_MOUSEMOVE / WM_MOUSELEAVE
    - Per-video completion via CompletionCallback -> Save available immediately
      when a video finishes, even if other videos are still running
    - FPS shown per row once video completes (from CompletionCallback)
    - Output format (JSON/CSV/Both) as radio buttons in Settings dialog
    - All settings + output folder persisted to HKCU registry
    - .webm support
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
#include <cmath>
#include <opencv2/core/ocl.hpp>
#include <opencv2/videoio.hpp>
#include "LightningDetector.h"

#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"msimg32.lib")
#pragma comment(lib,"advapi32.lib")

#pragma comment(linker,\
    "\"/manifestdependency:type='win32' "\
    "name='Microsoft.Windows.Common-Controls' "\
    "version='6.0.0.0' "\
    "processorArchitecture='*' "\
    "publicKeyToken='6595b64144ccf1df' "\
    "language='*'\"")

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  Layout constants
// ---------------------------------------------------------------------------
static const int HEADER_H = 64;
static const int ROW_H    = 56;   // height of each file-list row
static const int BAR_H    = 16;   // progress bar height inside row
static const int BAR_W    = 190;  // progress bar width
static const int FRAMES_W = 120;  // "fd / ft" text width
static const int SAVE_W   = 72;   // Save button width
static const int SAVE_H   = 28;   // Save button height
static const int ROW_PAD  = 8;    // row left/right padding

// ---------------------------------------------------------------------------
//  Control IDs
// ---------------------------------------------------------------------------
enum {
    ID_BTN_ADD        = 1001,
    ID_BTN_CLEAR      = 1002,
    ID_BTN_ANALYSE    = 1003,
    ID_BTN_SET_FOLDER = 1004,
    ID_BTN_SETTINGS   = 1005,
    ID_LISTBOX        = 1006,
    ID_LOG            = 1007,
    ID_PROGRESS       = 1008,
    ID_LABEL_STATUS   = 1009,
    ID_BTN_DAY        = 1010,
    ID_BTN_NIGHT      = 1011,
    ID_LABEL_MODE     = 1012,
    ID_LABEL_FOLDER   = 1013,
    ID_BTN_STILL      = 1014,
    ID_BTN_MOTION     = 1015,
    ID_TIMER_REDRAW   = 2001,
    // Settings dialog
    ID_EDIT_BASE      = 3001,   // 3001-3004 numeric edits, 3005/3006 custom fps/dim edits
    ID_HELP_BASE      = 3101,   // 3101-3107
    ID_FMT_JSON       = 3201,
    ID_FMT_CSV        = 3202,
    ID_FMT_BOTH       = 3203,
    ID_GPU_ON         = 3204,
    ID_GPU_OFF        = 3205,
    ID_FPS_PRESET_BASE= 3210,   // 3210-3213 (Full Quality,120,60,30)
    ID_DIM_PRESET_BASE= 3220,   // 3220-3224 (Full Quality,1080,720,480,360)
};

#define WM_WORKER_PROG  (WM_USER+11)
#define WM_WORKER_DONE  (WM_USER+10)

// ---------------------------------------------------------------------------
//  Theme
// ---------------------------------------------------------------------------
static const COLORREF C_BG    = RGB(244,246,250);
static const COLORREF C_HDR1  = RGB( 28, 78,186);
static const COLORREF C_HDR2  = RGB( 56,124,235);
static const COLORREF C_TEXT  = RGB( 35, 40, 48);
static const COLORREF C_SUB   = RGB(120,128,140);
static const COLORREF C_BARBG = RGB(210,218,235);
static const COLORREF C_BARFG = RGB( 37, 99,235);
static const COLORREF C_BARDO = RGB( 22,163, 74);
static const COLORREF C_SAVEB = RGB( 37, 99,235);
static const COLORREF C_SAVEH = RGB( 59,130,246);  // hover

static HFONT  g_fUI    = nullptr;
static HFONT  g_fBold  = nullptr;
static HFONT  g_fTitle = nullptr;
static HFONT  g_fSmall = nullptr;
static HBRUSH g_bgBr   = nullptr;

// ---------------------------------------------------------------------------
//  Registry helpers
// ---------------------------------------------------------------------------
static const wchar_t* REG_KEY = L"Software\\LightningDetector";

static void RWStr(const wchar_t* name, const std::wstring& v) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,REG_KEY,0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)!=ERROR_SUCCESS) return;
    RegSetValueExW(hk,name,0,REG_SZ,(BYTE*)v.c_str(),(DWORD)((v.size()+1)*sizeof(wchar_t)));
    RegCloseKey(hk);
}
static std::wstring RRStr(const wchar_t* name, const wchar_t* def=L"") {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,REG_KEY,0,KEY_READ,&hk)!=ERROR_SUCCESS) return def;
    WCHAR buf[1024]; DWORD sz=sizeof(buf),type=0;
    LSTATUS r=RegQueryValueExW(hk,name,nullptr,&type,(BYTE*)buf,&sz);
    RegCloseKey(hk);
    return (r==ERROR_SUCCESS)?buf:def;
}
static double RRDbl(const wchar_t* name, double def) {
    auto s=RRStr(name); if(s.empty()) return def;
    try{return std::stod(s);}catch(...){return def;}
}
static void RWDbl(const wchar_t* name, double v) {
    std::wostringstream ss; ss<<v; RWStr(name,ss.str());
}

// ---------------------------------------------------------------------------
//  Per-video state
// ---------------------------------------------------------------------------
struct VideoEntry {
    std::wstring path, name;
    double fileSizeMB = 0.0;
    double fps        = 0.0;   // filled from CompletionCallback
    int    fd = 0, ft = 0;
    bool   done = false, success = false;
};

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
static HWND g_hwnd=nullptr, g_listbox=nullptr, g_log=nullptr;
static HWND g_progress=nullptr, g_lblStatus=nullptr;
static HWND g_btnAnalyse=nullptr, g_btnClear=nullptr, g_btnAdd=nullptr;
static HWND g_btnSetFolder=nullptr, g_lblFolder=nullptr;
static HWND g_btnDay=nullptr, g_btnNight=nullptr;
static HWND g_btnStill=nullptr, g_btnMotion=nullptr;

static std::vector<VideoEntry>  g_entries;
static std::vector<VideoResult> g_results;   // populated by completion callback
static std::atomic<bool>        g_running{false};
static std::mutex               g_logMtx;
static std::wstring             g_outputFolder;
static int                      g_outputFormat = 0;  // 0=JSON 1=CSV 2=Both
static int                      g_detMode      = 0;  // 0=custom 1=day 2=night
static int                      g_motionMode   = 0;  // 0=none 1=still 2=motion
static LightningDetector::Params g_baseParams;       // params before still/motion adjustment
static LightningDetector::Params g_customParams;     // last user/settings params, restored when Day/Night is toggled off
static LightningDetector        g_detector;
static int                      g_hotSaveRow   = -1; // row with hovered Save btn

// Shared progress/completion info
struct ProgInfo { int fd=0,ft=0; bool done=false,success=false; double fps=0.0; };
static std::mutex            g_progMtx;
static std::vector<ProgInfo> g_progInfos;

// Listbox original wndproc (for subclassing)
static WNDPROC g_origListProc = nullptr;

// ---------------------------------------------------------------------------
//  String utils
// ---------------------------------------------------------------------------
static std::string w2u(const std::wstring& w) {
    if(w.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string s(n-1,'\0');
    WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,s.data(),n,nullptr,nullptr);
    return s;
}
static std::wstring u2w(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
    std::wstring w(n-1,L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);
    return w;
}
static void SF(HWND h, HFONT f){ SendMessageW(h,WM_SETFONT,(WPARAM)f,TRUE); }

static void AppendLog(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[16]; std::strftime(buf,sizeof(buf),"%H:%M:%S",std::localtime(&t));
    std::wstring wm=u2w(std::string("[")+buf+"] "+msg+"\r\n");
    int len=GetWindowTextLengthW(g_log);
    SendMessageW(g_log,EM_SETSEL,len,len);
    SendMessageW(g_log,EM_REPLACESEL,FALSE,(LPARAM)wm.c_str());
    SendMessageW(g_log,EM_SCROLL,SB_BOTTOM,0);
}

// ---------------------------------------------------------------------------
//  Output folder / saving
// ---------------------------------------------------------------------------
static std::wstring DefaultOutputFolder() {
    WCHAR exe[MAX_PATH]; GetModuleFileNameW(nullptr,exe,MAX_PATH);
    return (fs::path(exe).parent_path()/L"LightningResults").wstring();
}
static void EnsureOutputFolder() {
    if(g_outputFolder.empty()) g_outputFolder=DefaultOutputFolder();
    fs::create_directories(g_outputFolder);
}
static void UpdateFolderLabel() {
    EnsureOutputFolder();
    SetWindowTextW(g_lblFolder,(L"Output: "+g_outputFolder).c_str());
}
static std::vector<std::string> SaveOne(const VideoResult& r) {
    EnsureOutputFolder();
    std::vector<std::string> saved;
    auto stem=fs::path(u2w(r.video_filename)).stem().wstring();
    auto unique=[&](std::wstring ext){
        fs::path p=fs::path(g_outputFolder)/(stem+ext);
        int n=2; while(fs::exists(p)) p=fs::path(g_outputFolder)/(stem+L"_"+std::to_wstring(n++)+ext);
        return p.wstring();
    };
    std::vector<VideoResult> single={r};
    if(g_outputFormat==0||g_outputFormat==2){auto p=unique(L".json");if(LightningDetector::saveJSON(single,w2u(p)))saved.push_back(w2u(p));}
    if(g_outputFormat==1||g_outputFormat==2){auto p=unique(L".csv"); if(LightningDetector::saveCSV(single,w2u(p))) saved.push_back(w2u(p));}
    return saved;
}

// ---------------------------------------------------------------------------
//  Presets
// ---------------------------------------------------------------------------
static LightningDetector::Params DayParams() {
    LightningDetector::Params p;
    p.brightness_threshold=45;p.pixel_fraction_required=0.08f;p.min_event_gap_frames=20;
    p.max_threads=0;p.target_analysis_fps=60;p.analysis_max_dimension=720;p.use_gpu=true;
    return p;
}
static LightningDetector::Params NightParams() {
    LightningDetector::Params p;
    p.brightness_threshold=18;p.pixel_fraction_required=0.03f;p.min_event_gap_frames=12;
    p.max_threads=0;p.target_analysis_fps=60;p.analysis_max_dimension=720;p.use_gpu=true;
    return p;
}

// Still (tripod/fixed-camera) videos have no motion blur or shake, so we can
// afford to be more sensitive without picking up false positives.
static LightningDetector::Params ApplyStillAdjust(LightningDetector::Params p) {
    p.pixel_fraction_required = std::max(0.005f, p.pixel_fraction_required*0.7f);
    p.min_event_gap_frames    = std::max(5, p.min_event_gap_frames-5);
    return p;
}
// Motion (handheld/panning) videos need higher thresholds so camera shake
// and panning brightness changes aren't mistaken for lightning.
static LightningDetector::Params ApplyMotionAdjust(LightningDetector::Params p) {
    p.pixel_fraction_required = p.pixel_fraction_required*1.5f;
    p.brightness_threshold    = p.brightness_threshold*1.2f;
    p.min_event_gap_frames    = p.min_event_gap_frames+5;
    return p;
}
// Recomputes detector params from the current base (day/night/custom) plus
// whichever still/motion adjustment (if any) is currently toggled on.
static void ApplyCombinedParams() {
    LightningDetector::Params p=g_baseParams;
    if(g_motionMode==1)      p=ApplyStillAdjust(p);
    else if(g_motionMode==2) p=ApplyMotionAdjust(p);
    g_detector.setParams(p);
}
static void RefreshModeButtons() {
    if(g_btnDay)   SetWindowTextW(g_btnDay,   g_detMode==1   ?L"[Day]"  :L"Day");
    if(g_btnNight) SetWindowTextW(g_btnNight, g_detMode==2   ?L"[Night]":L"Night");
    if(g_btnStill) SetWindowTextW(g_btnStill, g_motionMode==1?L"[Still]":L"Still");
    if(g_btnMotion)SetWindowTextW(g_btnMotion,g_motionMode==2?L"[Motion]":L"Motion");
}

// ---------------------------------------------------------------------------
//  Settings persistence
// ---------------------------------------------------------------------------
static void LoadSettings() {
    LightningDetector::Params p;
    p.brightness_threshold   =(float)RRDbl(L"BrightThresh",30);
    p.pixel_fraction_required=(float)RRDbl(L"PixFrac",0.05);
    p.min_event_gap_frames   =(int)  RRDbl(L"MinGap",15);
    p.max_threads            =(int)  RRDbl(L"MaxThreads",0);
    p.target_analysis_fps    =       RRDbl(L"TargetFPS",60);
    p.analysis_max_dimension =(int)  RRDbl(L"MaxDim",720);
    p.use_gpu                =(bool)(RRDbl(L"UseGPU",1)!=0);
    g_outputFormat           =(int)  RRDbl(L"OutputFormat",0);
    g_baseParams             =p;
    g_customParams           =p;
    g_detector.setParams(p);
    auto fld=RRStr(L"OutputFolder");
    if(!fld.empty()) g_outputFolder=fld;
}
static void SaveSettings(const LightningDetector::Params& p, int fmt) {
    RWDbl(L"BrightThresh", p.brightness_threshold);
    RWDbl(L"PixFrac",      p.pixel_fraction_required);
    RWDbl(L"MinGap",       p.min_event_gap_frames);
    RWDbl(L"MaxThreads",   p.max_threads);
    RWDbl(L"TargetFPS",    p.target_analysis_fps);
    RWDbl(L"MaxDim",       p.analysis_max_dimension);
    RWDbl(L"UseGPU",       p.use_gpu?1:0);
    RWDbl(L"OutputFormat", fmt);
}

// ---------------------------------------------------------------------------
//  Save button rect for a given listbox row rect
//  (in listbox client coordinates)
// ---------------------------------------------------------------------------
static RECT SaveBtnRect(const RECT& rc) {
    return RECT{
        rc.right - SAVE_W - ROW_PAD,
        rc.top   + (ROW_H - SAVE_H) / 2,
        rc.right - ROW_PAD,
        rc.top   + (ROW_H - SAVE_H) / 2 + SAVE_H
    };
}

// ---------------------------------------------------------------------------
//  Listbox subclass - handles Save button clicks and hover
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ListboxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {

    case WM_LBUTTONDOWN: {
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        LRESULT lr=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,MAKELPARAM(pt.x,pt.y));
        int idx=(int)LOWORD(lr);
        bool outside=(bool)HIWORD(lr);
        if(!outside && idx>=0 && idx<(int)g_entries.size()) {
            if(g_entries[idx].done && g_entries[idx].success) {
                RECT rc={}; SendMessageW(hwnd,LB_GETITEMRECT,idx,(LPARAM)&rc);
                RECT sr=SaveBtnRect(rc);
                if(PtInRect(&sr,pt)) {
                    // Save this video
                    if(idx<(int)g_results.size() && g_results[idx].success) {
                        auto saved=SaveOne(g_results[idx]);
                        for(auto&p:saved) AppendLog("Saved: "+p);
                        if(!saved.empty())
                            ShellExecuteW(g_hwnd,L"open",g_outputFolder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                        else AppendLog("ERROR: Could not save for "+g_results[idx].video_filename);
                    }
                    return 0; // consume - don't select the item
                }
            }
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        int newHot=-1;
        LRESULT lr=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,MAKELPARAM(pt.x,pt.y));
        int idx=(int)LOWORD(lr); bool outside=(bool)HIWORD(lr);
        if(!outside && idx>=0 && idx<(int)g_entries.size() &&
            g_entries[idx].done && g_entries[idx].success) {
            RECT rc={}; SendMessageW(hwnd,LB_GETITEMRECT,idx,(LPARAM)&rc);
            RECT sr=SaveBtnRect(rc);
            if(PtInRect(&sr,pt)) newHot=idx;
        }
        if(newHot!=g_hotSaveRow) {
            g_hotSaveRow=newHot;
            InvalidateRect(hwnd,nullptr,FALSE);
        }
        TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hwnd,0};
        TrackMouseEvent(&tme);
        break;
    }

    case WM_MOUSELEAVE:
        if(g_hotSaveRow!=-1){ g_hotSaveRow=-1; InvalidateRect(hwnd,nullptr,FALSE); }
        break;
    }
    return CallWindowProcW(g_origListProc,hwnd,msg,wp,lp);
}

// ---------------------------------------------------------------------------
//  Owner-draw listbox row
// ---------------------------------------------------------------------------
static void DrawListRow(DRAWITEMSTRUCT* di) {
    if(di->itemID==(UINT)-1 || di->itemID>=(UINT)g_entries.size()) return;
    const VideoEntry& e=g_entries[di->itemID];
    HDC dc=di->hDC;
    RECT rc=di->rcItem;

    // ---- Row background ----
    bool sel=(di->itemState&ODS_SELECTED)!=0;
    HBRUSH bgbr=CreateSolidBrush(sel?RGB(230,238,252):RGB(255,255,255));
    FillRect(dc,&rc,bgbr); DeleteObject(bgbr);

    // Bottom separator
    HPEN sep=CreatePen(PS_SOLID,1,RGB(220,226,238));
    HPEN osep=(HPEN)SelectObject(dc,sep);
    MoveToEx(dc,rc.left,rc.bottom-1,nullptr);
    LineTo(dc,rc.right,rc.bottom-1);
    SelectObject(dc,osep); DeleteObject(sep);

    SetBkMode(dc,TRANSPARENT);

    // ---- Columns layout (right to left) ----
    // [PAD] [Save btn SAVE_W] [PAD] [frames FRAMES_W] [PAD] [bar BAR_W] [PAD] [text...]
    int right       = rc.right;
    int saveRight   = right - ROW_PAD;
    int saveLeft    = saveRight - SAVE_W;
    int framesRight = saveLeft - ROW_PAD;
    int framesLeft  = framesRight - FRAMES_W;
    int barRight    = framesLeft - ROW_PAD;
    int barLeft     = barRight - BAR_W;
    int textRight   = barLeft - ROW_PAD;

    // ---- Left text section ----
    int ty = rc.top;
    // Filename (bold)
    HFONT of=(HFONT)SelectObject(dc,g_fBold);
    SetTextColor(dc,C_TEXT);
    RECT nr={rc.left+ROW_PAD, ty+5, textRight, ty+22};
    DrawTextW(dc,e.name.c_str(),-1,&nr,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

    // Size + FPS
    SelectObject(dc,g_fSmall);
    SetTextColor(dc,C_SUB);
    std::wostringstream info;
    info<<std::fixed<<std::setprecision(2)<<e.fileSizeMB<<L" MB";
    if(e.fps>0.0) info<<L"   "<<std::setprecision(2)<<e.fps<<L" fps";
    RECT ir={rc.left+ROW_PAD, ty+23, textRight, ty+39};
    DrawTextW(dc,info.str().c_str(),-1,&ir,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

    // Status text
    std::wstring statusStr;
    COLORREF statusColor;
    if(e.done && e.success)  { statusStr=L"Completed";    statusColor=C_BARDO; }
    else if(e.done)          { statusStr=L"Error";        statusColor=RGB(200,50,50); }
    else if(e.ft>0||e.fd>0) { statusStr=L"Analysing..."; statusColor=C_BARFG; }
    else                     { statusStr=L"Queued";       statusColor=C_SUB; }
    SetTextColor(dc,statusColor);
    RECT sr2={rc.left+ROW_PAD, ty+40, textRight, ty+ROW_H-2};
    DrawTextW(dc,statusStr.c_str(),-1,&sr2,DT_LEFT|DT_SINGLELINE|DT_VCENTER);

    SelectObject(dc,of);

    // ---- Progress bar ----
    int barTop = rc.top + (ROW_H - BAR_H) / 2;
    int barBot = barTop + BAR_H;

    // Background (always draw so bar outline is visible even when empty)
    HBRUSH bbbr=CreateSolidBrush(C_BARBG);
    RECT bgrc={barLeft, barTop, barRight, barBot};
    FillRect(dc,&bgrc,bbbr); DeleteObject(bbbr);

    // Fill
    float frac=0.f;
    if(e.done && e.success)   frac=1.f;
    else if(e.ft>0)           frac=std::min(1.f,(float)e.fd/(float)e.ft);
    else if(e.fd>0)           frac=0.3f;  // analysing, unknown total
    if(frac>0.f) {
        COLORREF fc=(e.done&&e.success)?C_BARDO:C_BARFG;
        int fillX=barLeft+(int)((barRight-barLeft)*frac);
        HBRUSH fillbr=CreateSolidBrush(fc);
        RECT fillrc={barLeft,barTop,fillX,barBot};
        FillRect(dc,&fillrc,fillbr); DeleteObject(fillbr);
    }

    // Border (drawn AFTER fill so it's always on top)
    HBRUSH bordbr=CreateSolidBrush(RGB(170,182,210));
    FrameRect(dc,&bgrc,bordbr); DeleteObject(bordbr);

    // ---- Frames text ----
    SelectObject(dc,g_fSmall);
    SetTextColor(dc,C_TEXT);
    std::wstring frTxt;
    if(e.ft>0)       frTxt=std::to_wstring(e.fd)+L" / "+std::to_wstring(e.ft);
    else if(e.fd>0)  frTxt=std::to_wstring(e.fd)+L" frames";
    RECT frRc={framesLeft, rc.top, framesRight, rc.bottom};
    DrawTextW(dc,frTxt.c_str(),-1,&frRc,DT_LEFT|DT_SINGLELINE|DT_VCENTER);

    // ---- Save button (drawn manually, shown only when done+success) ----
    if(e.done && e.success) {
        bool hot=(int)di->itemID==g_hotSaveRow;
        COLORREF btnBg=hot?C_SAVEH:C_SAVEB;
        RECT btnRc={saveLeft, rc.top+(ROW_H-SAVE_H)/2, saveRight, rc.top+(ROW_H-SAVE_H)/2+SAVE_H};

        // Button fill
        HBRUSH btnbr=CreateSolidBrush(btnBg);
        FillRect(dc,&btnRc,btnbr); DeleteObject(btnbr);

        // Slight border
        HBRUSH btnbord=CreateSolidBrush(RGB(29,78,216));
        FrameRect(dc,&btnRc,btnbord); DeleteObject(btnbord);

        // Button text
        SelectObject(dc,g_fSmall);
        SetTextColor(dc,RGB(255,255,255));
        std::wstring btnTxt=g_outputFormat==1?L"Save CSV":g_outputFormat==2?L"Save All":L"Save JSON";
        DrawTextW(dc,btnTxt.c_str(),-1,&btnRc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
}

// ---------------------------------------------------------------------------
//  File list
// ---------------------------------------------------------------------------
static double ProbeVideoFPS(const std::wstring& path) {
    try {
        cv::VideoCapture cap(w2u(path));
        if(!cap.isOpened()) return 0.0;
        double fps=cap.get(cv::CAP_PROP_FPS);
        cap.release();
        return (fps>0.0)?fps:0.0;
    } catch(...) { return 0.0; }
}
static bool AddVideoPath(const std::wstring& path) {
    static const std::vector<std::wstring> exts={
        L".mp4",L".avi",L".mkv",L".mov",L".wmv",L".m4v",
        L".mpg",L".mpeg",L".ts",L".flv",L".webm"};
    std::wstring ext=fs::path(path).extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
    if(std::find(exts.begin(),exts.end(),ext)==exts.end()) return false;
    for(auto& e:g_entries) if(e.path==path) return false;
    VideoEntry ve;
    ve.path=path; ve.name=fs::path(path).filename().wstring();
    try{ve.fileSizeMB=(double)fs::file_size(path)/1048576.0;}catch(...){}
    ve.fps=ProbeVideoFPS(path);   // read immediately so fps shows before analysis
    g_entries.push_back(ve);
    return true;
}
static void RebuildListbox() {
    SendMessageW(g_listbox,LB_RESETCONTENT,0,0);
    for(auto& e:g_entries)
        SendMessageW(g_listbox,LB_ADDSTRING,0,(LPARAM)e.name.c_str());
    SetWindowTextW(g_hwnd,(L"Lightning Detector  ["+std::to_wstring(g_entries.size())+L" file(s)]").c_str());
    InvalidateRect(g_listbox,nullptr,FALSE);
}

// ---------------------------------------------------------------------------
//  Drop target
// ---------------------------------------------------------------------------
class DropTarget:public IDropTarget {
    ULONG m_r=1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r,void**p)override{
        if(r==IID_IUnknown||r==IID_IDropTarget){*p=this;AddRef();return S_OK;}
        *p=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++m_r;}
    ULONG STDMETHODCALLTYPE Release()override{return --m_r;}
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*,DWORD,POINTL,DWORD*e)override{*e=DROPEFFECT_COPY;return S_OK;}
    HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD*e)override{*e=DROPEFFECT_COPY;return S_OK;}
    HRESULT STDMETHODCALLTYPE DragLeave()override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*pDO,DWORD,POINTL,DWORD*e)override{
        *e=DROPEFFECT_COPY;
        FORMATETC fmt={CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
        STGMEDIUM stg={};
        if(SUCCEEDED(pDO->GetData(&fmt,&stg))){
            HDROP hd=(HDROP)GlobalLock(stg.hGlobal);
            if(hd){int cnt=DragQueryFileW(hd,0xFFFFFFFF,nullptr,0),added=0;
                for(int i=0;i<cnt;i++){WCHAR b[MAX_PATH];DragQueryFileW(hd,i,b,MAX_PATH);if(AddVideoPath(b))added++;}
                GlobalUnlock(stg.hGlobal);
                if(added){RebuildListbox();AppendLog("Added "+std::to_string(added)+" video(s).");}}
            ReleaseStgMedium(&stg);}
        return S_OK;}
};
static DropTarget* g_drop=nullptr;

// ---------------------------------------------------------------------------
//  Worker thread
// ---------------------------------------------------------------------------
static void WorkerThread(std::vector<std::string> paths, int total) {
    {
        std::lock_guard<std::mutex> lk(g_progMtx);
        g_progInfos.assign(total,{});
        g_results.resize(total);
    }

    auto prog_cb=[](int vi,int,int fd,int ft,const std::string&){
        {std::lock_guard<std::mutex> lk(g_progMtx);
            if(vi<(int)g_progInfos.size()){g_progInfos[vi].fd=fd;g_progInfos[vi].ft=ft;}}
        PostMessageW(g_hwnd,WM_WORKER_PROG,0,0);
    };

    auto done_cb=[](int idx,const VideoResult& r){
        // Called immediately when one video finishes
        {std::lock_guard<std::mutex> lk(g_progMtx);
            if(idx<(int)g_progInfos.size()){
                g_progInfos[idx].done=true;
                g_progInfos[idx].success=r.success;
                g_progInfos[idx].fps=r.fps;
                g_progInfos[idx].fd=r.total_frames;
                g_progInfos[idx].ft=r.total_frames;}
            if(idx<(int)g_results.size()) g_results[idx]=r;}
        PostMessageW(g_hwnd,WM_WORKER_PROG,0,0);  // triggers UI update immediately
    };

    g_detector.analyseVideos(paths, prog_cb, done_cb);
    g_running=false;
    PostMessageW(g_hwnd,WM_WORKER_DONE,0,0);
}

// ---------------------------------------------------------------------------
//  Settings dialog
// ---------------------------------------------------------------------------
struct SHelp{const wchar_t*label,*help;};
static const SHelp kHelp[7]={
    {L"Brightness threshold (0-255):",
     L"How much brighter a pixel must get vs background to count as a flash.\n\n"
     L"Default: 30\nLower (15-20): catches faint lightning.\nHigher (40-60): requires intense flash."},
    {L"Min flash pixel fraction (0-1):",
     L"Fraction of the frame that must light up at once.\n\n"
     L"Default: 0.05 (5%)\nLower: catches small flashes.\nHigher (0.10): filters glare/headlights."},
    {L"Min gap between events (frames):",
     L"Quiet frames required between two separate events.\n\nDefault: 15\n"
     L"Increase (30-60) so rapid flashes aren't split into many tiny events."},
    {L"Max processing threads (0=auto):",
     L"Videos analysed simultaneously.\n\nDefault: 0 (all CPU cores)\n"
     L"Set a number to leave CPU free for other tasks."},
    {L"Analyse at FPS:",
     L"High-fps video is analysed at this rate to save time.\n\nDefault: 60fps\n"
     L"Timestamps always match true video time. Choose Full Quality to analyse every frame,\n"
     L"or type a custom value."},
    {L"Max analysis dimension:",
     L"Frames shrunk to this size before analysis.\n\nDefault: 720p\n"
     L"Smaller = faster. Choose Full Quality for full resolution, or type a custom value in px."},
    {L"Use GPU acceleration:",
     L"Offloads image math to GPU via OpenCL.\n\nDefault: On\n"
     L"Falls back to CPU automatically if OpenCL is unavailable."},
};

// Presets for the FPS and max-dimension buttons. Index 0 in each is the
// "Full Quality" option (0 = disabled/no downscale or frame-skip).
static const wchar_t* kFpsLabels[4] ={L"Full Quality",L"120fps",L"60fps",L"30fps"};
static const double   kFpsVals[4]   ={0,120,60,30};
static const wchar_t* kDimLabels[5] ={L"Full Quality",L"1080p",L"720p",L"480p",L"360p"};
static const int      kDimVals[5]   ={0,1080,720,480,360};

struct SDlgState{
    bool done=false,ok=false;
    HWND edits[4]={};                 // brightness, pixel-frac, min-gap, max-threads
    HWND fpsBtns[4]={}; HWND fpsCustomEdit=nullptr; int fpsSel=-1;   // -1 = custom value in use
    HWND dimBtns[5]={}; HWND dimCustomEdit=nullptr; int dimSel=-1;   // -1 = custom value in use
    HWND gpuOnBtn=nullptr, gpuOffBtn=nullptr; bool gpuOn=true;
};
static WNDPROC g_origSettProc=nullptr;

static void RefreshFpsBtns(SDlgState*st){
    for(int i=0;i<4;i++){
        std::wstring t=(st->fpsSel==i)?(std::wstring(L"[")+kFpsLabels[i]+L"]"):kFpsLabels[i];
        SetWindowTextW(st->fpsBtns[i],t.c_str());}
}
static void RefreshDimBtns(SDlgState*st){
    for(int i=0;i<5;i++){
        std::wstring t=(st->dimSel==i)?(std::wstring(L"[")+kDimLabels[i]+L"]"):kDimLabels[i];
        SetWindowTextW(st->dimBtns[i],t.c_str());}
}
static void RefreshGpuBtns(SDlgState*st){
    SetWindowTextW(st->gpuOnBtn, st->gpuOn ?L"[On]" :L"On");
    SetWindowTextW(st->gpuOffBtn,!st->gpuOn?L"[Off]":L"Off");
}

static LRESULT CALLBACK SettingsProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    SDlgState*st=(SDlgState*)GetWindowLongPtrW(h,GWLP_USERDATA);
    switch(msg){
    case WM_COMMAND:{int id=LOWORD(wp);
        if(st&&HIWORD(wp)==EN_CHANGE){
            if(id==ID_EDIT_BASE+4&&GetWindowTextLengthW(st->fpsCustomEdit)>0){st->fpsSel=-1;RefreshFpsBtns(st);return 0;}
            if(id==ID_EDIT_BASE+5&&GetWindowTextLengthW(st->dimCustomEdit)>0){st->dimSel=-1;RefreshDimBtns(st);return 0;}
            break;}
        if(id==IDOK){if(st){st->ok=true;st->done=true;}return 0;}
        if(id==IDCANCEL){if(st)st->done=true;return 0;}
        if(id>=ID_FPS_PRESET_BASE&&id<ID_FPS_PRESET_BASE+4&&st){
            st->fpsSel=id-ID_FPS_PRESET_BASE;SetWindowTextW(st->fpsCustomEdit,L"");RefreshFpsBtns(st);return 0;}
        if(id>=ID_DIM_PRESET_BASE&&id<ID_DIM_PRESET_BASE+5&&st){
            st->dimSel=id-ID_DIM_PRESET_BASE;SetWindowTextW(st->dimCustomEdit,L"");RefreshDimBtns(st);return 0;}
        if(id==ID_GPU_ON&&st){st->gpuOn=true;RefreshGpuBtns(st);return 0;}
        if(id==ID_GPU_OFF&&st){st->gpuOn=false;RefreshGpuBtns(st);return 0;}
        if(id>=ID_HELP_BASE&&id<ID_HELP_BASE+7){
            std::wstring t=L"Help: ";t+=kHelp[id-ID_HELP_BASE].label;
            MessageBoxW(h,kHelp[id-ID_HELP_BASE].help,t.c_str(),MB_OK|MB_ICONINFORMATION);
            return 0;}
        break;}
    case WM_CLOSE:if(st)st->done=true;return 0;
    case WM_NOTIFY:{NMHDR*hdr=(NMHDR*)lp;
        if(hdr->code==TTN_GETDISPINFOW){NMTTDISPINFOW*di=(NMTTDISPINFOW*)lp;
            int cid=(int)GetWindowLongPtrW((HWND)hdr->idFrom,GWL_ID),idx=-1;
            if(cid>=ID_HELP_BASE&&cid<ID_HELP_BASE+7)idx=cid-ID_HELP_BASE;
            else if(cid>=ID_EDIT_BASE&&cid<ID_EDIT_BASE+6)idx=cid-ID_EDIT_BASE;
            else if(cid==ID_GPU_ON||cid==ID_GPU_OFF)idx=6;
            if(idx>=0)di->lpszText=(LPWSTR)kHelp[idx].help;return 0;}break;}
    }
    return CallWindowProcW(g_origSettProc,h,msg,wp,lp);
}

static bool ShowSettings(HWND owner, LightningDetector::Params* p, int* fmt) {
    const int W=560, H=480;
    HWND hd=CreateWindowExW(WS_EX_DLGMODALFRAME,L"#32770",L"Detection Settings",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,0,0,W,H,owner,nullptr,nullptr,nullptr);
    if(!hd) return false;
    SF(hd,g_fUI);

    SDlgState st;
    int y=12;
    // Rows 0-3: numeric edit boxes. "?" help button sits before the label,
    // with the label/edit shifted right to make room for it while keeping
    // everything on the same line as before.
    for(int i=0;i<4;i++){
        HWND hb=CreateWindowW(L"BUTTON",L"?",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            12,y,24,22,hd,(HMENU)(INT_PTR)(ID_HELP_BASE+i),nullptr,nullptr);
        SF(hb,g_fBold);
        HWND lb=CreateWindowW(L"STATIC",kHelp[i].label,WS_CHILD|WS_VISIBLE,44,y+3,266,18,hd,nullptr,nullptr,nullptr);
        SF(lb,g_fUI);
        st.edits[i]=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,318,y,82,22,hd,(HMENU)(INT_PTR)(ID_EDIT_BASE+i),nullptr,nullptr);
        SF(st.edits[i],g_fUI);
        y+=36;
    }

    const int BTN_H=26, BTN_GAP=4;
    const int FULL_W=96, PRESET_W=64;   // "Full Quality" needs more room than e.g. "60fps"

    // FPS row: "?" help before the label (same style as rows above), then preset buttons + custom box
    y+=4;
    {HWND hb=CreateWindowW(L"BUTTON",L"?",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,12,y,24,22,hd,(HMENU)(INT_PTR)(ID_HELP_BASE+4),nullptr,nullptr);
     SF(hb,g_fBold);
     HWND lb=CreateWindowW(L"STATIC",kHelp[4].label,WS_CHILD|WS_VISIBLE,44,y+3,266,18,hd,nullptr,nullptr,nullptr);
     SF(lb,g_fUI);}
    y+=24;
    {int x=44;
     for(int i=0;i<4;i++){
        int w=(i==0)?FULL_W:PRESET_W;
        st.fpsBtns[i]=CreateWindowW(L"BUTTON",kFpsLabels[i],WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            x,y,w,BTN_H,hd,(HMENU)(INT_PTR)(ID_FPS_PRESET_BASE+i),nullptr,nullptr);
        SF(st.fpsBtns[i],g_fUI);
        x+=w+BTN_GAP;
     }
     HWND cl=CreateWindowW(L"STATIC",L"Custom:",WS_CHILD|WS_VISIBLE,x+4,y+5,50,18,hd,nullptr,nullptr,nullptr);
     SF(cl,g_fUI);
     st.fpsCustomEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,x+56,y+1,64,24,hd,(HMENU)(INT_PTR)(ID_EDIT_BASE+4),nullptr,nullptr);
     SF(st.fpsCustomEdit,g_fUI);}
    y+=BTN_H+8;

    // Dimension row: "?" help before the label, then preset buttons + custom box
    {HWND hb=CreateWindowW(L"BUTTON",L"?",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,12,y,24,22,hd,(HMENU)(INT_PTR)(ID_HELP_BASE+5),nullptr,nullptr);
     SF(hb,g_fBold);
     HWND lb=CreateWindowW(L"STATIC",kHelp[5].label,WS_CHILD|WS_VISIBLE,44,y+3,266,18,hd,nullptr,nullptr,nullptr);
     SF(lb,g_fUI);}
    y+=24;
    {int x=44;
     for(int i=0;i<5;i++){
        int w=(i==0)?FULL_W:PRESET_W;
        st.dimBtns[i]=CreateWindowW(L"BUTTON",kDimLabels[i],WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            x,y,w,BTN_H,hd,(HMENU)(INT_PTR)(ID_DIM_PRESET_BASE+i),nullptr,nullptr);
        SF(st.dimBtns[i],g_fUI);
        x+=w+BTN_GAP;
     }
     HWND cl=CreateWindowW(L"STATIC",L"Custom:",WS_CHILD|WS_VISIBLE,x+4,y+5,50,18,hd,nullptr,nullptr,nullptr);
     SF(cl,g_fUI);
     st.dimCustomEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,x+56,y+1,64,24,hd,(HMENU)(INT_PTR)(ID_EDIT_BASE+5),nullptr,nullptr);
     SF(st.dimCustomEdit,g_fUI);}
    y+=BTN_H+8;

    // GPU row: "?" help before the label, then separate On / Off buttons
    {HWND hb=CreateWindowW(L"BUTTON",L"?",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,12,y,24,22,hd,(HMENU)(INT_PTR)(ID_HELP_BASE+6),nullptr,nullptr);
     SF(hb,g_fBold);
     HWND lb=CreateWindowW(L"STATIC",kHelp[6].label,WS_CHILD|WS_VISIBLE,44,y+3,266,18,hd,nullptr,nullptr,nullptr);
     SF(lb,g_fUI);
     st.gpuOnBtn=CreateWindowW(L"BUTTON",L"On",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        318,y,52,22,hd,(HMENU)(INT_PTR)ID_GPU_ON,nullptr,nullptr);
     SF(st.gpuOnBtn,g_fUI);
     st.gpuOffBtn=CreateWindowW(L"BUTTON",L"Off",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        374,y,52,22,hd,(HMENU)(INT_PTR)ID_GPU_OFF,nullptr,nullptr);
     SF(st.gpuOffBtn,g_fUI);}
    y+=36;

    // Output format section (radio buttons)
    y+=4;
    HWND fmtLbl=CreateWindowW(L"STATIC",L"Output format:",WS_CHILD|WS_VISIBLE,12,y+2,120,18,hd,nullptr,nullptr,nullptr);
    SF(fmtLbl,g_fBold);
    HWND r1=CreateWindowW(L"BUTTON",L"JSON",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON|WS_TABSTOP|WS_GROUP,
        138,y,70,20,hd,(HMENU)ID_FMT_JSON,nullptr,nullptr);
    HWND r2=CreateWindowW(L"BUTTON",L"CSV",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON|WS_TABSTOP,
        216,y,70,20,hd,(HMENU)ID_FMT_CSV,nullptr,nullptr);
    HWND r3=CreateWindowW(L"BUTTON",L"Both",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON|WS_TABSTOP,
        294,y,70,20,hd,(HMENU)ID_FMT_BOTH,nullptr,nullptr);
    SF(r1,g_fUI);SF(r2,g_fUI);SF(r3,g_fUI);
    // Set initial selection
    HWND initR=(*fmt==1)?r2:(*fmt==2)?r3:r1;
    SendMessageW(initR,BM_SETCHECK,BST_CHECKED,0);
    y+=28;

    HWND hint=CreateWindowW(L"STATIC",L"Settings are saved automatically when you click OK.",
        WS_CHILD|WS_VISIBLE,12,y+4,W-40,18,hd,nullptr,nullptr,nullptr);
    SF(hint,g_fSmall);
    HWND bOK=CreateWindowW(L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,W/2-110,H-60,100,30,hd,(HMENU)IDOK,nullptr,nullptr);
    HWND bCN=CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,W/2+10,H-60,100,30,hd,(HMENU)IDCANCEL,nullptr,nullptr);
    SF(bOK,g_fUI);SF(bCN,g_fUI);

    // Tooltips
    HWND tip=CreateWindowExW(0,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hd,nullptr,nullptr,nullptr);
    SetWindowPos(tip,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    auto addTip=[&](HWND ctrl){
        TOOLINFOW ti={};ti.cbSize=sizeof(ti);ti.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
        ti.hwnd=hd;ti.uId=(UINT_PTR)ctrl;ti.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(tip,TTM_ADDTOOLW,0,(LPARAM)&ti);};
    for(int i=0;i<4;i++){addTip(st.edits[i]);addTip(GetDlgItem(hd,ID_HELP_BASE+i));}
    addTip(st.fpsCustomEdit);addTip(GetDlgItem(hd,ID_HELP_BASE+4));
    addTip(st.dimCustomEdit);addTip(GetDlgItem(hd,ID_HELP_BASE+5));
    addTip(st.gpuOnBtn);addTip(st.gpuOffBtn);addTip(GetDlgItem(hd,ID_HELP_BASE+6));

    // Populate numeric edit boxes
    auto setN=[&](HWND h2,double v,int dp){
        std::wostringstream ss;if(dp>0)ss<<std::fixed<<std::setprecision(dp)<<v;else ss<<(long long)v;
        SetWindowTextW(h2,ss.str().c_str());};
    setN(st.edits[0],p->brightness_threshold,0);
    setN(st.edits[1],p->pixel_fraction_required,3);
    setN(st.edits[2],p->min_event_gap_frames,0);
    setN(st.edits[3],p->max_threads,0);

    // Determine initial FPS / dimension selection (preset match or custom)
    for(int i=0;i<4;i++) if(std::abs(p->target_analysis_fps-kFpsVals[i])<0.001) st.fpsSel=i;
    if(st.fpsSel<0) setN(st.fpsCustomEdit,p->target_analysis_fps,0);
    RefreshFpsBtns(&st);
    for(int i=0;i<5;i++) if(p->analysis_max_dimension==kDimVals[i]) st.dimSel=i;
    if(st.dimSel<0) setN(st.dimCustomEdit,p->analysis_max_dimension,0);
    RefreshDimBtns(&st);
    st.gpuOn=p->use_gpu;
    RefreshGpuBtns(&st);

    SetWindowLongPtrW(hd,GWLP_USERDATA,(LONG_PTR)&st);
    g_origSettProc=(WNDPROC)SetWindowLongPtrW(hd,GWLP_WNDPROC,(LONG_PTR)SettingsProc);
    RECT rc;GetWindowRect(owner,&rc);
    SetWindowPos(hd,nullptr,rc.left+(rc.right-rc.left-W)/2,rc.top+(rc.bottom-rc.top-H)/2,W,H,SWP_NOZORDER);
    ShowWindow(hd,SW_SHOW);EnableWindow(owner,FALSE);SetFocus(st.edits[0]);

    MSG m;
    while(!st.done&&GetMessageW(&m,nullptr,0,0))
        {if(!IsDialogMessageW(hd,&m)){TranslateMessage(&m);DispatchMessageW(&m);}}

    if(st.ok){
        WCHAR b[64];
        GetWindowTextW(st.edits[0],b,64);p->brightness_threshold   =(float)_wtof(b);
        GetWindowTextW(st.edits[1],b,64);p->pixel_fraction_required =(float)_wtof(b);
        GetWindowTextW(st.edits[2],b,64);p->min_event_gap_frames    =_wtoi(b);
        GetWindowTextW(st.edits[3],b,64);p->max_threads             =_wtoi(b);
        if(st.fpsSel>=0) p->target_analysis_fps=kFpsVals[st.fpsSel];
        else {GetWindowTextW(st.fpsCustomEdit,b,64);p->target_analysis_fps=_wtof(b);}
        if(st.dimSel>=0) p->analysis_max_dimension=kDimVals[st.dimSel];
        else {GetWindowTextW(st.dimCustomEdit,b,64);p->analysis_max_dimension=_wtoi(b);}
        p->use_gpu=st.gpuOn;
        if(IsDlgButtonChecked(hd,ID_FMT_CSV)==BST_CHECKED)  *fmt=1;
        else if(IsDlgButtonChecked(hd,ID_FMT_BOTH)==BST_CHECKED)*fmt=2;
        else                                                      *fmt=0;
    }
    EnableWindow(owner,TRUE);DestroyWindow(hd);SetForegroundWindow(owner);
    return st.ok;
}

// ---------------------------------------------------------------------------
//  WndProc
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){

    case WM_CREATE:{
        const int Y1=HEADER_H+14;   // toolbar row 1
        const int Y2=Y1+40;         // toolbar row 2 (mode + folder)
        const int LY=Y2+34;         // list area

        // Row 1: main buttons
        g_btnAdd=CreateWindowW(L"BUTTON",L"Add Videos...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,10,Y1,120,32,hwnd,(HMENU)ID_BTN_ADD,nullptr,nullptr);
        g_btnClear=CreateWindowW(L"BUTTON",L"Clear List",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,138,Y1,100,32,hwnd,(HMENU)ID_BTN_CLEAR,nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"Settings...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,246,Y1,100,32,hwnd,(HMENU)ID_BTN_SETTINGS,nullptr,nullptr);
        g_btnAnalyse=CreateWindowW(L"BUTTON",L">  Analyse",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,354,Y1,120,32,hwnd,(HMENU)ID_BTN_ANALYSE,nullptr,nullptr);
        g_btnSetFolder=CreateWindowW(L"BUTTON",L"Set Output Folder...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,482,Y1,160,32,hwnd,(HMENU)ID_BTN_SET_FOLDER,nullptr,nullptr);

        // Row 2: mode + folder label
        CreateWindowW(L"STATIC",L"Mode:",WS_CHILD|WS_VISIBLE,
            10,Y2+5,44,18,hwnd,(HMENU)ID_LABEL_MODE,nullptr,nullptr);
        g_btnDay=CreateWindowW(L"BUTTON",L"Day",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,58,Y2,70,26,hwnd,(HMENU)ID_BTN_DAY,nullptr,nullptr);
        g_btnNight=CreateWindowW(L"BUTTON",L"Night",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,136,Y2,70,26,hwnd,(HMENU)ID_BTN_NIGHT,nullptr,nullptr);
        CreateWindowW(L"STATIC",L"Camera:",WS_CHILD|WS_VISIBLE,
            216,Y2+5,58,18,hwnd,(HMENU)0,nullptr,nullptr);
        g_btnStill=CreateWindowW(L"BUTTON",L"Still",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,278,Y2,70,26,hwnd,(HMENU)ID_BTN_STILL,nullptr,nullptr);
        g_btnMotion=CreateWindowW(L"BUTTON",L"Motion",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,356,Y2,70,26,hwnd,(HMENU)ID_BTN_MOTION,nullptr,nullptr);
        g_lblFolder=CreateWindowW(L"STATIC",L"Output: ",
            WS_CHILD|WS_VISIBLE|SS_ENDELLIPSIS,436,Y2+5,650,18,hwnd,(HMENU)ID_LABEL_FOLDER,nullptr,nullptr);

        // Drop hint + owner-draw listbox
        HWND lDrop=CreateWindowW(L"STATIC",L"Drag & drop video files here, or click Add Videos...",
            WS_CHILD|WS_VISIBLE|SS_CENTER,10,LY,770,18,hwnd,(HMENU)0,nullptr,nullptr);

        g_listbox=CreateWindowW(L"LISTBOX",L"",
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|LBS_OWNERDRAWFIXED|LBS_NOTIFY|LBS_HASSTRINGS,
            10,LY+22,770,230,hwnd,(HMENU)ID_LISTBOX,nullptr,nullptr);
        SendMessageW(g_listbox,LB_SETITEMHEIGHT,0,ROW_H);

        // Subclass listbox for click/hover handling
        g_origListProc=(WNDPROC)SetWindowLongPtrW(g_listbox,GWLP_WNDPROC,(LONG_PTR)ListboxProc);

        // Status + master progress bar
        g_lblStatus=CreateWindowW(L"STATIC",L"Idle - add videos to begin.",
            WS_CHILD|WS_VISIBLE,10,LY+262,770,18,hwnd,(HMENU)ID_LABEL_STATUS,nullptr,nullptr);
        g_progress=CreateWindowW(PROGRESS_CLASSW,L"",WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            10,LY+284,770,16,hwnd,(HMENU)ID_PROGRESS,nullptr,nullptr);
        SendMessageW(g_progress,PBM_SETRANGE,0,MAKELPARAM(0,1000));

        // Log
        g_log=CreateWindowW(L"EDIT",L"",
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            10,LY+308,770,230,hwnd,(HMENU)ID_LOG,nullptr,nullptr);

        // Fonts
        g_fUI   =CreateFontW(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        g_fBold =CreateFontW(-15,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        g_fTitle=CreateFontW(-24,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        g_fSmall=CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        g_bgBr  =CreateSolidBrush(C_BG);

        // Apply fonts to all children
        EnumChildWindows(hwnd,[](HWND c,LPARAM f)->BOOL{SF(c,(HFONT)f);return TRUE;},(LPARAM)g_fUI);
        SF(g_btnAnalyse,g_fBold);
        SF(g_lblFolder,g_fSmall);
        SF(lDrop,g_fSmall);
        SF(g_log,CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,ANSI_CHARSET,0,0,DEFAULT_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas"));

        LoadSettings();
        RefreshModeButtons();
        UpdateFolderLabel();
        std::wstring fmtStr=g_outputFormat==0?L"JSON":g_outputFormat==1?L"CSV":L"JSON+CSV";
        AppendLog("Lightning Detector ready.");
        AppendLog("Output format: "+w2u(fmtStr)+"  |  Folder: "+w2u(g_outputFolder));
        return 0;
    }

    case WM_ERASEBKGND:return 1;

    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);
        RECT cl;GetClientRect(hwnd,&cl);
        HBRUSH bg=CreateSolidBrush(C_BG);RECT bgR=cl;bgR.top=HEADER_H;
        FillRect(dc,&bgR,bg);DeleteObject(bg);
        TRIVERTEX v[2];
        v[0]={0,0,(COLOR16)(GetRValue(C_HDR1)<<8),(COLOR16)(GetGValue(C_HDR1)<<8),(COLOR16)(GetBValue(C_HDR1)<<8),0};
        v[1]={(LONG)cl.right,HEADER_H,(COLOR16)(GetRValue(C_HDR2)<<8),(COLOR16)(GetGValue(C_HDR2)<<8),(COLOR16)(GetBValue(C_HDR2)<<8),0};
        GRADIENT_RECT gr={0,1};GradientFill(dc,v,2,&gr,1,GRADIENT_FILL_RECT_H);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        HFONT of=(HFONT)SelectObject(dc,g_fTitle);
        RECT tr={18,8,cl.right-20,38};DrawTextW(dc,L"Lightning Detector",-1,&tr,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        SelectObject(dc,g_fSmall);RECT sr={20,38,cl.right-20,60};
        DrawTextW(dc,L"Video lightning detection & analysis",-1,&sr,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        SelectObject(dc,of);EndPaint(hwnd,&ps);return 0;}

    case WM_CTLCOLORSTATIC:{HDC dc=(HDC)wp;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,C_TEXT);return(LRESULT)g_bgBr;}
    case WM_MEASUREITEM:{MEASUREITEMSTRUCT*mi=(MEASUREITEMSTRUCT*)lp;mi->itemHeight=ROW_H;return TRUE;}
    case WM_DRAWITEM:{if(wp==ID_LISTBOX)DrawListRow((DRAWITEMSTRUCT*)lp);return TRUE;}

    case WM_TIMER:
        if(wp==ID_TIMER_REDRAW&&g_running) InvalidateRect(g_listbox,nullptr,FALSE);
        break;

    case WM_SIZE:{int W=LOWORD(lp),H=HIWORD(lp);if(W<300||H<300)break;
        const int Y1=HEADER_H+14,Y2=Y1+40,LY=Y2+34;
        MoveWindow(g_listbox,  10,LY+22, W-20,230,TRUE);
        MoveWindow(g_lblStatus,10,LY+262,W-20, 18,TRUE);
        MoveWindow(g_progress, 10,LY+284,W-20, 16,TRUE);
        MoveWindow(g_log,      10,LY+308,W-20,H-(LY+318),TRUE);
        MoveWindow(g_lblFolder,436,Y2+5, W-446, 18,TRUE);
        InvalidateRect(hwnd,nullptr,FALSE);break;}

    case WM_COMMAND:{
        int id=LOWORD(wp);
        switch(id){
        case ID_BTN_ADD:{
            WCHAR buf[32768]={};OPENFILENAMEW ofn={};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=hwnd;
            ofn.lpstrFilter=L"Video Files\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.m4v;*.mpg;*.mpeg;*.ts;*.flv;*.webm\0All Files\0*.*\0";
            ofn.lpstrFile=buf;ofn.nMaxFile=sizeof(buf)/sizeof(WCHAR);
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
            if(GetOpenFileNameW(&ofn)){
                std::wstring dir(buf);WCHAR*p=buf+ofn.nFileOffset;int a=0;
                if(*(p-1)==L'\0'&&*p){while(*p){if(AddVideoPath(dir+L"\\"+p))a++;p+=wcslen(p)+1;}}
                else if(AddVideoPath(dir))a++;
                if(a){RebuildListbox();AppendLog("Added "+std::to_string(a)+" video(s).");}}
            break;}
        case ID_BTN_CLEAR:
            if(!g_running){
                g_entries.clear();g_results.clear();
                RebuildListbox();AppendLog("List cleared.");}
            break;
        case ID_BTN_SET_FOLDER:{
            BROWSEINFOW bi={};bi.hwndOwner=hwnd;
            bi.lpszTitle=L"Select folder for saved results:";
            bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl=SHBrowseForFolderW(&bi);
            if(pidl){WCHAR path[MAX_PATH];
                if(SHGetPathFromIDListW(pidl,path)){
                    g_outputFolder=path;RWStr(L"OutputFolder",g_outputFolder);
                    UpdateFolderLabel();AppendLog("Output folder: "+w2u(g_outputFolder));}
                CoTaskMemFree(pidl);}
            break;}
        case ID_BTN_DAY:
            if(g_detMode==1){ // toggle off: restore last custom/settings params
                g_detMode=0;g_baseParams=g_customParams;
            } else {
                g_detMode=1;g_baseParams=DayParams();
            }
            ApplyCombinedParams();
            RefreshModeButtons();
            AppendLog(g_detMode==1?"Mode: DAY":"Mode: (unset)");break;
        case ID_BTN_NIGHT:
            if(g_detMode==2){ // toggle off: restore last custom/settings params
                g_detMode=0;g_baseParams=g_customParams;
            } else {
                g_detMode=2;g_baseParams=NightParams();
            }
            ApplyCombinedParams();
            RefreshModeButtons();
            AppendLog(g_detMode==2?"Mode: NIGHT":"Mode: (unset)");break;
        case ID_BTN_STILL:
            g_motionMode=(g_motionMode==1)?0:1;ApplyCombinedParams();
            RefreshModeButtons();
            AppendLog(g_motionMode==1?"Camera: STILL":"Camera: (unset)");break;
        case ID_BTN_MOTION:
            g_motionMode=(g_motionMode==2)?0:2;ApplyCombinedParams();
            RefreshModeButtons();
            AppendLog(g_motionMode==2?"Camera: MOTION":"Camera: (unset)");break;
        case ID_BTN_SETTINGS:{
            auto p=g_baseParams;int fmt=g_outputFormat;
            if(ShowSettings(hwnd,&p,&fmt)){
                g_baseParams=p;g_customParams=p;g_outputFormat=fmt;
                g_detMode=0;ApplyCombinedParams();
                RefreshModeButtons();
                SaveSettings(p,fmt);
                std::wstring fs=fmt==0?L"JSON":fmt==1?L"CSV":L"JSON+CSV";
                AppendLog("Settings saved. Format: "+w2u(fs));
                InvalidateRect(g_listbox,nullptr,FALSE); // refresh Save button labels
            } else AppendLog("Settings unchanged.");
            break;}
        case ID_BTN_ANALYSE:
            if(g_running)break;
            if(g_entries.empty()){MessageBoxW(hwnd,L"Add videos first.",L"No Videos",MB_OK|MB_ICONINFORMATION);break;}
            g_running=true;
            EnableWindow(g_btnAnalyse,FALSE);EnableWindow(g_btnAdd,FALSE);EnableWindow(g_btnClear,FALSE);
            for(auto&e:g_entries){e.fd=0;e.ft=0;e.done=false;e.success=false;}
            SendMessageW(g_progress,PBM_SETPOS,0,0);
            SetWindowTextW(g_lblStatus,L"Starting analysis...");
            AppendLog("Analysing "+std::to_string(g_entries.size())+" video(s)...");
            {std::vector<std::string>paths;for(auto&e:g_entries)paths.push_back(w2u(e.path));
             int n=(int)paths.size();
             std::thread(WorkerThread,paths,n).detach();}
            SetTimer(hwnd,ID_TIMER_REDRAW,100,nullptr);
            break;
        }
        break;}

    case WM_WORKER_PROG:{
        std::vector<ProgInfo> pis;
        {std::lock_guard<std::mutex> lk(g_progMtx);pis=g_progInfos;}
        int tf=0,df=0;
        for(int i=0;i<(int)pis.size()&&i<(int)g_entries.size();i++){
            g_entries[i].fd=pis[i].fd;
            g_entries[i].ft=pis[i].ft;
            if(pis[i].done){
                g_entries[i].done=true;
                g_entries[i].success=pis[i].success;
                g_entries[i].fps=pis[i].fps;
            }
            tf+=std::max(1,pis[i].ft);df+=pis[i].fd;
        }
        int pos=(tf>0)?(int)((double)df/tf*1000):0;
        SendMessageW(g_progress,PBM_SETPOS,pos,0);
        int vd=(int)std::count_if(pis.begin(),pis.end(),[](const ProgInfo&p){return p.done;});
        SetWindowTextW(g_lblStatus,(std::to_wstring(vd)+L" / "+std::to_wstring((int)pis.size())+
            L" videos done   "+std::to_wstring(df)+L" / "+std::to_wstring(tf)+L" frames").c_str());
        InvalidateRect(g_listbox,nullptr,FALSE);
        break;}

    case WM_WORKER_DONE:{
        KillTimer(hwnd,ID_TIMER_REDRAW);
        SendMessageW(g_progress,PBM_SETPOS,1000,0);
        // Final sync
        {std::lock_guard<std::mutex> lk(g_progMtx);
            for(int i=0;i<(int)g_progInfos.size()&&i<(int)g_entries.size();i++){
                g_entries[i].done=true;g_entries[i].success=g_progInfos[i].success;
                g_entries[i].fps=g_progInfos[i].fps;
                g_entries[i].fd=g_progInfos[i].ft;g_entries[i].ft=g_progInfos[i].ft;}}
        InvalidateRect(g_listbox,nullptr,FALSE);
        EnableWindow(g_btnAnalyse,TRUE);EnableWindow(g_btnAdd,TRUE);EnableWindow(g_btnClear,TRUE);
        SetWindowTextW(g_lblStatus,L"Analysis complete. Click Save on any row to export results.");
        AppendLog("");
        int te=0;
        {std::lock_guard<std::mutex> lk(g_progMtx);
            for(auto&r:g_results){
                if(r.success){te+=(int)r.events.size();}
                else AppendLog("ERROR - "+r.video_filename+": "+r.error_message);}}
        AppendLog("Analysis finished. "+std::to_string(te)+" event(s) total across "+
                  std::to_string(g_results.size())+" video(s).");
        std::wstring fmtStr=g_outputFormat==0?L"JSON":g_outputFormat==1?L"CSV":L"JSON+CSV";
        MessageBoxW(hwnd,(L"Analysis complete!\n\nEvents found: "+std::to_wstring(te)+
            L"\n\nClick Save on each row to export "+fmtStr+L" files.\n\nOutput folder:\n"+
            g_outputFolder).c_str(),L"Done",MB_OK|MB_ICONINFORMATION);
        break;}

    case WM_CLOSE:
        if(g_running){if(MessageBoxW(hwnd,L"Analysis is running. Force quit?",
            L"Quit?",MB_YESNO|MB_ICONWARNING)!=IDYES)break;}
        DestroyWindow(hwnd);break;
    case WM_DESTROY:
        if(g_drop){RevokeDragDrop(hwnd);g_drop->Release();}
        PostQuitMessage(0);break;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

// ---------------------------------------------------------------------------
//  WinMain
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int nShow){
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icx={sizeof(icx),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES|ICC_BAR_CLASSES};
    InitCommonControlsEx(&icx);
    WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WndProc;wc.hInstance=hInst;
    wc.lpszClassName=L"LightningDetectorWnd";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=nullptr;wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
    RegisterClassExW(&wc);
    g_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"LightningDetectorWnd",L"Lightning Detector",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,900,800,nullptr,nullptr,hInst,nullptr);
    g_drop=new DropTarget(); RegisterDragDrop(g_hwnd,g_drop);
    std::thread([](){
        if(cv::ocl::haveOpenCL()){
            cv::ocl::Context ctx;ctx.create(4);
            auto name=cv::ocl::Device::getDefault().name();
            AppendLog(name.empty()?"OpenCL: no GPU device.":"OpenCL device: "+name);
        } else AppendLog("OpenCL not available - CPU only.");
    }).detach();
    ShowWindow(g_hwnd,nShow);UpdateWindow(g_hwnd);
    MSG m;while(GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessageW(&m);}
    OleUninitialize();return(int)m.wParam;
}
