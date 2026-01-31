#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:WinMainCRTStartup")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h> 
#include <dwmapi.h>  
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <filesystem> 

namespace fs = std::filesystem;
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

// --- Colors ---
COLORREF CLR_BG = RGB(35, 35, 38);
COLORREF CLR_WIDGET = RGB(55, 55, 58);
COLORREF CLR_TEXT = RGB(255, 255, 255);
COLORREF CLR_SELECT = RGB(75, 75, 80);
HBRUSH hbrBkgnd = CreateSolidBrush(CLR_BG);
HBRUSH hbrWidget = CreateSolidBrush(CLR_WIDGET);
HBRUSH hbrSelect = CreateSolidBrush(CLR_SELECT);
HFONT hFontMain;

struct WemEntry { uint32_t id, offset, length; <comment-tag id="1">uint32_t originalLength, didxEntryOffset;</comment-tag id="1"> bool modified = false; };
struct SbpFile { std::wstring path, name; std::vector<unsigned char> data; std::vector<WemEntry> entries; uint32_t audioBaseOffset; bool dirty = false; };

HWND g_hWnd, g_hTreeView, g_hBtnReplace, g_hBtnExtract, g_hSearchEdit, g_hBtnBatch, g_hInfoLabel, g_hBtnClear, g_hBtnFile, g_hStatus, g_hSearchLabel;
std::vector<SbpFile*> g_LoadedFiles;
SbpFile* g_SelectedSBP = nullptr;
WemEntry* g_SelectedWem = nullptr;
HMENU g_hFilePopup;

// --- UI Logic ---
void SetStatus(std::wstring msg) { SetWindowTextW(g_hStatus, (L"  " + msg).c_str()); }

void UpdateButtonStates() {
    bool hasFiles = !g_LoadedFiles.empty();
    EnableWindow(g_hBtnBatch, hasFiles); EnableWindow(g_hBtnClear, hasFiles);
    EnableWindow(g_hBtnReplace, g_SelectedWem != nullptr);
    EnableWindow(g_hBtnExtract, g_SelectedSBP != nullptr);
    InvalidateRect(g_hWnd, NULL, TRUE);
}

std::wstring PickFolder() {
    std::wstring outPath = L""; IFileOpenDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD d; pfd->GetOptions(&d); pfd->SetOptions(d | FOS_PICKFOLDERS);
        if (SUCCEEDED(pfd->Show(NULL))) { IShellItem* psi = nullptr; if (SUCCEEDED(pfd->GetResult(&psi))) { PWSTR p = nullptr; psi->GetDisplayName(SIGDN_FILESYSPATH, &p); outPath = p; CoTaskMemFree(p); psi->Release(); } }
        pfd->Release();
    }
    return outPath;
}

void InjectWem(SbpFile* sbp, WemEntry* wem, std::wstring path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return;
    size_t s = f.tellg(); std::vector<unsigned char> n(s); f.seekg(0); f.read((char*)n.data(), s);
    size_t p = sbp->audioBaseOffset + wem->offset;
    
    <comment-tag id="2">if (n.size() > wem->originalLength) {
        std::copy(n.begin(), n.begin() + wem->originalLength, sbp->data.begin() + p);
        wem->length = wem->originalLength;
    }
    else {
        std::copy(n.begin(), n.end(), sbp->data.begin() + p);
        std::fill(sbp->data.begin() + p + n.size(), sbp->data.begin() + p + wem->originalLength, 0);
        wem->length = (uint32_t)n.size();
    }
    
    // Update length of DIDX inside SBP buffer
    *(uint32_t*)&sbp->data[wem->didxEntryOffset + 8] = wem->length;</comment-tag id="2">
    
    wem->modified = true; sbp->dirty = true;
}

void ParseSBP(SbpFile* sbp) {
    auto& d = sbp->data;
    auto bkhd = std::search(d.begin(), d.end(), "BKHD", "BKHD" + 4); if (bkhd == d.end()) return;
    auto data = std::search(d.begin() + std::distance(d.begin(), bkhd), d.end(), "DATA", "DATA" + 4);
    auto didx = std::search(d.begin() + std::distance(d.begin(), bkhd), d.end(), "DIDX", "DIDX" + 4);
    if (didx == d.end() || data == d.end()) return;
    sbp->audioBaseOffset = (uint32_t)std::distance(d.begin(), data) + 8;
    uint32_t len = *(uint32_t*)&d[std::distance(d.begin(), didx) + 4];
    int cnt = len / 12; size_t curr = std::distance(d.begin(), didx) + 8;
    for (int i = 0; i < cnt; i++) {
        WemEntry e; 
        e.id = *(uint32_t*)&d[curr]; 
        e.offset = *(uint32_t*)&d[curr + 4]; 
        e.length = *(uint32_t*)&d[curr + 8];
        <comment-tag id="3">e.originalLength = e.length;
        e.didxEntryOffset = (uint32_t)curr;</comment-tag id="3">
        sbp->entries.push_back(e); curr += 12;
    }
}

void RefreshTreeView(std::wstring filter = L"") {
    SendMessageW(g_hTreeView, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    std::wstring fL = filter; std::transform(fL.begin(), fL.end(), fL.begin(), ::towlower);
    for (auto& sbp : g_LoadedFiles) {
        std::wstring sLow = sbp->name; std::transform(sLow.begin(), sLow.end(), sLow.begin(), ::towlower);
        bool sMatch = fL.empty() || sLow.find(fL) != std::wstring::npos;
        std::vector<WemEntry*> matches;
        for (auto& w : sbp->entries) { if (fL.empty() || sMatch || std::to_wstring(w.id).find(fL) != std::wstring::npos) matches.push_back(&w); }
        if (!matches.empty()) {
            TVINSERTSTRUCTW tvis = { 0 }; tvis.item.mask = TVIF_TEXT | TVIF_PARAM; tvis.item.pszText = (LPWSTR)sbp->name.c_str(); tvis.item.lParam = (LPARAM)sbp;
            HTREEITEM hRoot = (HTREEITEM)SendMessageW(g_hTreeView, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
            for (auto m : matches) {
                std::wstring n = std::to_wstring(m->id) + (m->modified ? L" [MODDED]" : L".wem");
                TVINSERTSTRUCTW tc = { 0 }; tc.hParent = hRoot; tc.item.mask = TVIF_TEXT | TVIF_PARAM; tc.item.pszText = (LPWSTR)n.c_str(); tc.item.lParam = (LPARAM)m;
                SendMessageW(g_hTreeView, TVM_INSERTITEMW, 0, (LPARAM)&tc);
            }
            if (!fL.empty()) SendMessageW(g_hTreeView, TVM_EXPAND, TVE_EXPAND, (LPARAM)hRoot);
        }
    }
    UpdateButtonStates();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        BOOL value = TRUE; DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
        DragAcceptFiles(hWnd, TRUE);
        hFontMain = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        g_hBtnFile = CreateWindowW(L"BUTTON", L"File", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 10, 10, 65, 28, hWnd, (HMENU)1000, NULL, NULL);
        g_hSearchLabel = CreateWindowW(L"STATIC", L"Search:", WS_VISIBLE | WS_CHILD, 85, 13, 60, 20, hWnd, NULL, NULL, NULL);
        g_hSearchEdit = CreateWindowExW(0, L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 150, 10, 160, 28, hWnd, (HMENU)200, NULL, NULL);

        g_hTreeView = CreateWindowExW(0, WC_TREEVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_FULLROWSELECT, 10, 45, 300, 420, hWnd, (HMENU)100, NULL, NULL);
        SendMessageW(g_hTreeView, TVM_SETBKCOLOR, 0, (LPARAM)CLR_BG); SendMessageW(g_hTreeView, TVM_SETTEXTCOLOR, 0, (LPARAM)CLR_TEXT);
        SetWindowTheme(g_hTreeView, L"Explorer", NULL);

        g_hBtnReplace = CreateWindowW(L"BUTTON", L"Replace Single WEM", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 320, 45, 140, 35, hWnd, (HMENU)101, NULL, NULL);
        g_hBtnBatch = CreateWindowW(L"BUTTON", L"Batch Replace WEMs", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 320, 85, 140, 35, hWnd, (HMENU)104, NULL, NULL);
        g_hBtnExtract = CreateWindowW(L"BUTTON", L"Extract All", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 320, 125, 140, 35, hWnd, (HMENU)102, NULL, NULL);
        g_hBtnClear = CreateWindowW(L"BUTTON", L"Clear All", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 320, 165, 140, 35, hWnd, (HMENU)106, NULL, NULL);

        g_hInfoLabel = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD, 325, 215, 150, 150, hWnd, NULL, NULL, NULL);
        g_hStatus = CreateWindowW(L"STATIC", L"  Ready", WS_VISIBLE | WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        g_hFilePopup = CreatePopupMenu();
        AppendMenuW(g_hFilePopup, MF_OWNERDRAW, 1, L"Open SBP(s)");
        AppendMenuW(g_hFilePopup, MF_OWNERDRAW, 3, L"Save Selected SBP");
        AppendMenuW(g_hFilePopup, MF_OWNERDRAW, 4, L"Save ALL Modded");

        MENUINFO mi = { 0 };
        mi.cbSize = sizeof(mi);
        mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
        mi.hbrBack = hbrWidget;
        SetMenuInfo(g_hFilePopup, &mi);

        EnumChildWindows(hWnd, [](HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }, (LPARAM)hFontMain);
        UpdateButtonStates(); return 0;
    }
    case WM_MEASUREITEM: { LPMEASUREITEMSTRUCT lpm = (LPMEASUREITEMSTRUCT)lParam; if (lpm->CtlType == ODT_MENU) { lpm->itemWidth = 140; lpm->itemHeight = 25; } return TRUE; }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT p = (LPDRAWITEMSTRUCT)lParam;
        bool bSel = p->itemState & ODS_SELECTED; bool bDis = p->itemState & ODS_DISABLED;
        FillRect(p->hDC, &p->rcItem, bSel ? hbrSelect : (bDis ? hbrBkgnd : hbrWidget));
        SetTextColor(p->hDC, bDis ? RGB(100, 100, 105) : CLR_TEXT); SetBkMode(p->hDC, TRANSPARENT);
        wchar_t b[128];
        if (p->CtlType == ODT_MENU) {
            if (p->itemID == 1) wcscpy_s(b, L"Open SBP(s)"); else if (p->itemID == 3) wcscpy_s(b, L"Save Selected"); else if (p->itemID == 4) wcscpy_s(b, L"Save ALL Modded");
            p->rcItem.left += 10; DrawTextW(p->hDC, b, -1, &p->rcItem, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        else { GetWindowTextW(p->hwndItem, b, 128); DrawTextW(p->hDC, b, -1, &p->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE); FrameRect(p->hDC, &p->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH)); }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: { SetTextColor((HDC)wParam, CLR_TEXT); SetBkColor((HDC)wParam, CLR_BG); return (INT_PTR)hbrBkgnd; }
    case WM_CTLCOLOREDIT: { SetTextColor((HDC)wParam, CLR_TEXT); SetBkColor((HDC)wParam, CLR_WIDGET); return (INT_PTR)hbrWidget; }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 1000) { RECT r; GetWindowRect(g_hBtnFile, &r); TrackPopupMenu(g_hFilePopup, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, hWnd, NULL); }
        if (LOWORD(wParam) == 1) { // Open
            wchar_t sz[16384] = { 0 }; OPENFILENAMEW ofn = { sizeof(ofn), hWnd }; ofn.lpstrFile = sz; ofn.nMaxFile = 16384; ofn.lpstrFilter = L"SBP Files\0*.sbp;*.bnk\0"; ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER;
            if (GetOpenFileNameW(&ofn)) { wchar_t* p = sz; std::wstring dir = p; p += dir.length() + 1; if (*p == 0) { std::ifstream f(dir, std::ios::binary | std::ios::ate); if (f.is_open()) { size_t s = f.tellg(); SbpFile* sbp = new SbpFile(); sbp->path = dir; sbp->name = dir.substr(dir.find_last_of(L"\\/") + 1); sbp->data.resize(s); f.seekg(0); f.read((char*)sbp->data.data(), s); ParseSBP(sbp); g_LoadedFiles.push_back(sbp); } } else while (*p != 0) { std::wstring fn = p; std::wstring fp = dir + L"\\" + fn; std::ifstream f(fp, std::ios::binary | std::ios::ate); if (f.is_open()) { size_t s = f.tellg(); SbpFile* sbp = new SbpFile(); sbp->path = fp; sbp->name = fn; sbp->data.resize(s); f.seekg(0); f.read((char*)sbp->data.data(), s); ParseSBP(sbp); g_LoadedFiles.push_back(sbp); } p += fn.length() + 1; } RefreshTreeView(); SetStatus(L"Files Loaded."); }
        }
        if (LOWORD(wParam) == 106) { for (auto f : g_LoadedFiles) delete f; g_LoadedFiles.clear(); g_SelectedSBP = nullptr; g_SelectedWem = nullptr; SetWindowTextW(g_hInfoLabel, L""); RefreshTreeView(); SetStatus(L"Ready."); }
        if (LOWORD(wParam) == 104) {
            std::wstring folder = PickFolder(); if (folder == L"") break;
            int count = 0; SetStatus(L"Batching...");
            for (const auto& ent : fs::directory_iterator(folder)) { if (ent.path().extension() == ".wem") { try { uint32_t id = std::stoul(ent.path().stem().string()); for (auto sbp : g_LoadedFiles) { for (auto& w : sbp->entries) { if (w.id == id) { InjectWem(sbp, &w, ent.path().wstring()); count++; } } } } catch (...) {} } }
            RefreshTreeView(); SetStatus(L"Batch Processed: " + std::to_wstring(count));
        }
        if (LOWORD(wParam) == 101) { wchar_t sz[MAX_PATH]; OPENFILENAMEW ofn = { sizeof(ofn), hWnd }; ofn.lpstrFile = sz; ofn.nMaxFile = MAX_PATH; ofn.lpstrFilter = L"WEM\0*.wem\0"; if (GetOpenFileNameW(&ofn)) { InjectWem(g_SelectedSBP, g_SelectedWem, sz); RefreshTreeView(); SetStatus(L"WEM Injected."); } }
        if (LOWORD(wParam) == 102) {
            std::wstring r = PickFolder(); if (r == L"") break; std::wstring d = r + L"\\" + g_SelectedSBP->name + L"_extracted"; CreateDirectoryW(d.c_str(), NULL);
            for (auto& w : g_SelectedSBP->entries) { std::ofstream o(d + L"\\" + std::to_wstring(w.id) + L".wem", std::ios::binary); o.write((char*)&g_SelectedSBP->data[g_SelectedSBP->audioBaseOffset + w.offset], w.length); }
            SetStatus(L"Extracted Successfully.");
        }
        if (LOWORD(wParam) == 4) {
            std::wstring r = PickFolder(); if (r == L"") break;
            for (auto s : g_LoadedFiles) { if (s->dirty) { std::ofstream o(std::wstring(r) + L"\\" + s->name, std::ios::binary); o.write((char*)s->data.data(), s->data.size()); s->dirty = false; } }
            SetStatus(L"All files saved.");
        }
        if (LOWORD(wParam) == 3) {
            if (!g_SelectedSBP) break;
            std::wstring r = PickFolder();
            if (r != L"") {
                std::wstring outPath = r + L"\\" + g_SelectedSBP->name;
                std::ofstream o(outPath, std::ios::binary);
                o.write((char*)s->data.data(), s->data.size());
                o.close(); g_SelectedSBP->dirty = false; SetStatus(L"Saved selected SBP.");
            }
        }
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == 200) { wchar_t t[256]; GetWindowTextW(g_hSearchEdit, t, 256); RefreshTreeView(t); }
        break;
    }
    case WM_NOTIFY: {
        LPNMTREEVIEWW p = (LPNMTREEVIEWW)lParam;
        if (p->hdr.code == TVN_SELCHANGEDW) {
            HTREEITEM hP = (HTREEITEM)SendMessageW(g_hTreeView, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)p->itemNew.hItem);
            if (hP) {
                g_SelectedWem = (WemEntry*)p->itemNew.lParam; TVITEMW pi = { 0 }; pi.hItem = hP; pi.mask = TVIF_PARAM; SendMessageW(g_hTreeView, TVM_GETITEMW, 0, (LPARAM)&pi); g_SelectedSBP = (SbpFile*)pi.lParam;
                SetWindowTextW(g_hInfoLabel, (L"ID: " + std::to_wstring(g_SelectedWem->id) + L"\nSize: " + std::to_wstring(g_SelectedWem->length / 1024) + L" KB\nOffset: " + std::to_wstring(g_SelectedWem->offset)).c_str());
            }
            else { g_SelectedSBP = (SbpFile*)p->itemNew.lParam; g_SelectedWem = nullptr; if (g_SelectedSBP) SetWindowTextW(g_hInfoLabel, L"SBP Selected"); }
            UpdateButtonStates();
        }
        break;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        MoveWindow(g_hBtnFile, 10, 10, 65, 28, 1); MoveWindow(g_hSearchLabel, 85, 13, 60, 20, 1); MoveWindow(g_hSearchEdit, 150, 10, w - 170, 28, 1);
        MoveWindow(g_hTreeView, 10, 45, w - 180, h - 75, 1);
        int bx = w - 160; MoveWindow(g_hBtnReplace, bx, 45, 150, 35, 1); MoveWindow(g_hBtnBatch, bx, 85, 150, 35, 1);
        MoveWindow(g_hBtnExtract, bx, 125, 150, 35, 1); MoveWindow(g_hBtnClear, bx, 165, 150, 35, 1);
        MoveWindow(g_hInfoLabel, bx + 5, 215, 150, 150, 1);
        MoveWindow(g_hStatus, 0, h - 25, w, 25, 1); break;
    }
    case WM_ERASEBKGND: { RECT r; GetClientRect(hWnd, &r); FillRect((HDC)wParam, &r, hbrBkgnd); return 1; }
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int nC) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); 
    InitCommonControls();

    // --- LOAD ICON FROM FILE (logo.ico) ---
    HICON hIco = (HICON)LoadImageW(NULL, L"logo.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hI;
    wc.hbrBackground = hbrBkgnd;
    wc.lpszClassName = L"GGOWATool";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIco;   // Taskbar Icon
    wc.hIconSm = hIco; // Window Corner Icon

    RegisterClassExW(&wc);
    g_hWnd = CreateWindowW(L"GGOWATool", L"Giorma's GOW Audio Tool", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 620, 560, 0, 0, hI, 0);
    MSG m; 
    while (GetMessageW(&m, 0, 0, 0)) { 
        if (GetKeyState(VK_CONTROL) & 0x8000 && m.message == WM_KEYDOWN && m.wParam == 'F') 
            SetFocus(g_hSearchEdit); 
        TranslateMessage(&m); 
        DispatchMessageW(&m); 
    }
    CoUninitialize(); 
    return 0;
}