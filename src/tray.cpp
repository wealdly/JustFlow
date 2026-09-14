// System tray: own thread owns a hidden top-level window (needs to be top-level to receive the
// TaskbarCreated broadcast), the notify icon and the popup menu. Everything crossing threads goes
// through Tray::mu; the menu is rebuilt from the snapshot each time it pops, so there is no
// check-mark state to keep in sync.
#include "tray.h"
#include <shellapi.h>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
const UINT WM_TRAY_ICON  = WM_APP + 1;   // Shell_NotifyIcon callback
const UINT WM_TRAY_STATE = WM_APP + 2;   // TraySetState -> refresh tooltip on the tray thread
const UINT ICON_ID = 1;

enum { IDM_STATUS = 1, IDM_NR, IDM_FG, IDM_FG_POPUP, IDM_MULT2, IDM_MULT3, IDM_MULT4, IDM_WIPE, IDM_RELOAD,
       IDM_HOTKEYS, IDM_CONFIG, IDM_LOG, IDM_QUIT, IDM_PROFILE0 = 100 };
const int HK_EDIT0 = 100;   // dialog edit ids HK_EDIT0..HK_EDIT0+4
const wchar_t* const HK_LABELS[5] = { L"Toggle NR", L"Wipe", L"Reload", L"Frame gen", L"Quit" };
}

struct Tray
{
    std::wstring app, icon_path;
    std::thread  thread;
    HANDLE ready = nullptr;
    HWND   hwnd = nullptr, dlg = nullptr;
    HICON  icon = nullptr;
    UINT   taskbar_created = 0;

    std::mutex mu;   // guards everything below
    bool nr_on = true, fg_on = false;
    int  mult = 2, wipe = 0, profile = -1;
    std::wstring status;
    std::vector<std::wstring> profiles;
    std::wstring hk[5];
    std::deque<std::pair<TrayEvent, int>> events;
};

static void Push(Tray* t, TrayEvent ev, int arg = 0)
{
    std::lock_guard<std::mutex> lk(t->mu);
    t->events.emplace_back(ev, arg);
}

// ---- icon --------------------------------------------------------------------------------------

// ponytail: a filled disc with a lighter ring, 32x32 BGRA + empty AND mask; Windows scales it to 16.
static HICON MakeIcon()
{
    const int N = 32;
    std::vector<DWORD> xr(N * N, 0);
    std::vector<BYTE> an(N * N / 8, 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            const float dx = x - 15.5f, dy = y - 15.5f, r = dx * dx + dy * dy;
            if (r > 15.5f * 15.5f) continue;
            xr[y * N + x] = r > 11.5f * 11.5f ? 0xFF9ACBFF : 0xFF2A70D8;   // ARGB: ring, disc
        }
    return CreateIcon(GetModuleHandleW(nullptr), N, N, 1, 32, an.data(), (const BYTE*)xr.data());
}

// ---- hotkey dialog -----------------------------------------------------------------------------

static bool ValidHotkey(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towupper(c);
    size_t pos;
    while ((pos = s.find(L'+')) != std::wstring::npos)
    {
        const std::wstring m = s.substr(0, pos); s = s.substr(pos + 1);
        if (m != L"CTRL" && m != L"ALT" && m != L"SHIFT") return false;
    }
    if (s.size() >= 2 && s[0] == L'F') { const int n = _wtoi(s.c_str() + 1); return n >= 1 && n <= 24 && s == L"F" + std::to_wstring(n); }
    return s.size() == 1 && iswalnum(s[0]);
}

static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

// In-memory DLGTEMPLATE (WORD stream, items DWORD-aligned) so no .rc is needed.
static std::vector<WORD> BuildHotkeyTemplate()
{
    std::vector<WORD> w;
    auto dw = [&](DWORD v) { w.push_back(LOWORD(v)); w.push_back(HIWORD(v)); };
    auto str = [&](const wchar_t* s) { do w.push_back(*s); while (*s++); };
    auto item = [&](DWORD style, int x, int y, int cx, int cy, WORD id, WORD cls, const wchar_t* text)
    {
        if (w.size() & 1) w.push_back(0);
        dw(style | WS_CHILD | WS_VISIBLE); dw(0);
        w.push_back((WORD)x); w.push_back((WORD)y); w.push_back((WORD)cx); w.push_back((WORD)cy);
        w.push_back(id); w.push_back(0xFFFF); w.push_back(cls); str(text); w.push_back(0);
    };
    dw(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU); dw(0);
    w.push_back(5 * 2 + 2);                                       // cdit
    w.push_back(0); w.push_back(0); w.push_back(170); w.push_back(112);
    w.push_back(0); w.push_back(0); str(L"Hotkeys");             // menu, class, title
    w.push_back(8); str(L"MS Shell Dlg");
    for (int i = 0; i < 5; ++i)
    {
        item(SS_LEFT, 7, 9 + i * 16, 50, 8, (WORD)(200 + i), 0x0082, HK_LABELS[i]);
        item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 60, 7 + i * 16, 100, 12, (WORD)(HK_EDIT0 + i), 0x0081, L"");
    }
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, 57, 91, 50, 14, IDOK, 0x0080, L"OK");
    item(BS_PUSHBUTTON | WS_TABSTOP, 110, 91, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    return w;
}

static INT_PTR CALLBACK HotkeyDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Tray* t = (Tray*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        t = (Tray*)lp; SetWindowLongPtrW(h, GWLP_USERDATA, lp); t->dlg = h;
        std::lock_guard<std::mutex> lk(t->mu);
        for (int i = 0; i < 5; ++i) SetDlgItemTextW(h, HK_EDIT0 + i, t->hk[i].c_str());
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        if (LOWORD(wp) != IDOK) return FALSE;
        {
            std::wstring v[5];
            for (int i = 0; i < 5; ++i)
            {
                wchar_t buf[64]; GetDlgItemTextW(h, HK_EDIT0 + i, buf, 64);
                v[i] = Trim(buf);
                if (!ValidHotkey(v[i]))
                {
                    const std::wstring m = L"Invalid hotkey for \"" + std::wstring(HK_LABELS[i]) + L"\": \"" + v[i] +
                        L"\"\n\nUse [Ctrl+][Alt+][Shift+]Key, where Key is F1..F24 or a single letter/digit.";
                    MessageBoxW(h, m.c_str(), t->app.c_str(), MB_ICONWARNING);
                    SetFocus(GetDlgItem(h, HK_EDIT0 + i));
                    return TRUE;
                }
            }
            std::lock_guard<std::mutex> lk(t->mu);
            for (int i = 0; i < 5; ++i) t->hk[i] = v[i];
            t->events.emplace_back(TrayHotkeys, 0);
        }
        EndDialog(h, 1);
        return TRUE;
    }
    return FALSE;
}

static void ShowHotkeyDialog(Tray* t)
{
    if (t->dlg) { SetForegroundWindow(t->dlg); return; }
    static const std::vector<WORD> tmpl = BuildHotkeyTemplate();
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (const DLGTEMPLATE*)tmpl.data(), t->hwnd, HotkeyDlgProc, (LPARAM)t);
    t->dlg = nullptr;
}

// ---- menu --------------------------------------------------------------------------------------

static HMENU BuildMenu(Tray* t)
{
    std::lock_guard<std::mutex> lk(t->mu);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_STATUS, t->status.empty() ? t->app.c_str() : t->status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (t->nr_on ? MF_CHECKED : 0), IDM_NR, L"Neural rendering");

    HMENU fg = CreatePopupMenu();
    AppendMenuW(fg, MF_STRING | (t->fg_on ? MF_CHECKED : 0), IDM_FG, L"Enabled");
    AppendMenuW(fg, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fg, MF_STRING, IDM_MULT2, L"2X");
    AppendMenuW(fg, MF_STRING, IDM_MULT3, L"3X");
    AppendMenuW(fg, MF_STRING, IDM_MULT4, L"4X");
    const int mult = t->mult < 2 ? 2 : t->mult > 4 ? 4 : t->mult;
    CheckMenuRadioItem(fg, IDM_MULT2, IDM_MULT4, IDM_MULT2 + mult - 2, MF_BYCOMMAND);
    MENUITEMINFOW mi = { sizeof mi };
    mi.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_STATE | MIIM_ID;
    mi.wID = IDM_FG_POPUP; mi.hSubMenu = fg;
    mi.fState = t->fg_on ? MFS_CHECKED : MFS_UNCHECKED;
    mi.dwTypeData = const_cast<wchar_t*>(L"Frame generation");
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);

    AppendMenuW(m, MF_STRING | (t->wipe ? MF_CHECKED : 0), IDM_WIPE, L"Wipe compare");
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU pr = CreatePopupMenu();
    if (t->profiles.empty()) AppendMenuW(pr, MF_STRING | MF_GRAYED, 0, L"(none)");
    for (size_t i = 0; i < t->profiles.size(); ++i) AppendMenuW(pr, MF_STRING, IDM_PROFILE0 + i, t->profiles[i].c_str());
    if (t->profile >= 0 && t->profile < (int)t->profiles.size())
        CheckMenuRadioItem(pr, IDM_PROFILE0, IDM_PROFILE0 + (UINT)t->profiles.size() - 1, IDM_PROFILE0 + t->profile, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pr, L"Profiles");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING, IDM_HOTKEYS, L"Hotkeys...");
    AppendMenuW(m, MF_STRING, IDM_CONFIG, L"Open config file");
    AppendMenuW(m, MF_STRING, IDM_LOG, L"Open log");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit");
    return m;
}

static void ShowMenu(Tray* t)
{
    POINT p; GetCursorPos(&p);
    HMENU m = BuildMenu(t);
    SetForegroundWindow(t->hwnd);   // so the menu closes when the user clicks elsewhere
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, t->hwnd, nullptr);
    PostMessageW(t->hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);   // destroys submenus too
    switch (cmd)
    {
    case IDM_NR:      Push(t, TrayToggleNr); break;
    case IDM_FG:      Push(t, TrayToggleFg); break;
    case IDM_MULT2: case IDM_MULT3: case IDM_MULT4: Push(t, TrayFgMultiplier, cmd - IDM_MULT2 + 2); break;
    case IDM_WIPE:    Push(t, TrayWipe); break;
    case IDM_RELOAD:  Push(t, TrayReload); break;
    case IDM_CONFIG:  Push(t, TrayOpenConfig); break;
    case IDM_LOG:     Push(t, TrayOpenLog); break;
    case IDM_QUIT:    Push(t, TrayQuit); break;
    case IDM_HOTKEYS: ShowHotkeyDialog(t); break;
    default: if (cmd >= IDM_PROFILE0) Push(t, TraySelectProfile, cmd - IDM_PROFILE0); break;
    }
}

// ---- notify icon / window ----------------------------------------------------------------------

static NOTIFYICONDATAW Nid(Tray* t)
{
    NOTIFYICONDATAW n = { sizeof n };
    n.hWnd = t->hwnd; n.uID = ICON_ID;
    return n;
}

static void SetTip(Tray* t, NOTIFYICONDATAW& n)   // caller holds mu
{
    n.uFlags |= NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = t->app;
    if (!t->status.empty()) tip += L"\n" + t->status;
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
}

static void AddIcon(Tray* t)
{
    NOTIFYICONDATAW n = Nid(t);
    n.uFlags = NIF_ICON | NIF_MESSAGE;
    n.uCallbackMessage = WM_TRAY_ICON;
    n.hIcon = t->icon;
    { std::lock_guard<std::mutex> lk(t->mu); SetTip(t, n); }
    Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}

static LRESULT CALLBACK TrayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Tray* t = (Tray*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) { SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); return TRUE; }
    if (!t) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_TRAY_ICON)
    {
        switch (LOWORD(lp)) { case WM_CONTEXTMENU: case NIN_SELECT: case NIN_KEYSELECT: ShowMenu(t); break; }
        return 0;
    }
    if (msg == WM_TRAY_STATE)
    {
        NOTIFYICONDATAW n = Nid(t);
        { std::lock_guard<std::mutex> lk(t->mu); SetTip(t, n); }
        Shell_NotifyIconW(NIM_MODIFY, &n);
        return 0;
    }
    if (msg == t->taskbar_created) { AddIcon(t); return 0; }   // Explorer restarted
    switch (msg)
    {
    case WM_CLOSE:   { NOTIFYICONDATAW n = Nid(t); Shell_NotifyIconW(NIM_DELETE, &n); DestroyWindow(h); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void TrayThread(Tray* t)
{
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc; wc.hInstance = inst; wc.lpszClassName = L"JustFlowTray";
    RegisterClassW(&wc);   // second registration in-process just fails; CreateWindow still works
    t->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    t->icon = t->icon_path.empty() ? nullptr :
        (HICON)LoadImageW(nullptr, t->icon_path.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!t->icon) t->icon = MakeIcon();
    t->hwnd = CreateWindowExW(0, wc.lpszClassName, t->app.c_str(), WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, t);
    if (t->hwnd) AddIcon(t);
    SetEvent(t->ready);
    if (!t->hwnd) return;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// ---- public API --------------------------------------------------------------------------------

Tray* TrayCreate(const wchar_t* app_name, const wchar_t* icon_path_or_null)
{
    Tray* t = new Tray;
    t->app = app_name;
    if (icon_path_or_null) t->icon_path = icon_path_or_null;
    t->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t->thread = std::thread(TrayThread, t);
    WaitForSingleObject(t->ready, INFINITE);
    CloseHandle(t->ready); t->ready = nullptr;
    if (!t->hwnd) { t->thread.join(); if (t->icon) DestroyIcon(t->icon); delete t; return nullptr; }
    return t;
}

void TrayDestroy(Tray* t)
{
    if (!t) return;
    PostMessageW(t->hwnd, WM_CLOSE, 0, 0);
    t->thread.join();
    if (t->icon) DestroyIcon(t->icon);
    delete t;
}

void TraySetProfiles(Tray* t, const wchar_t* const* names, int count)
{
    std::lock_guard<std::mutex> lk(t->mu);
    t->profiles.assign(names, names + count);
}

void TraySetState(Tray* t, const TrayState& s)
{
    {
        std::lock_guard<std::mutex> lk(t->mu);
        t->nr_on = s.nr_on; t->fg_on = s.fg_on; t->mult = s.fg_multiplier;
        t->wipe = s.wipe_mode; t->profile = s.profile_index;
        t->status = s.status ? s.status : L"";
    }
    PostMessageW(t->hwnd, WM_TRAY_STATE, 0, 0);
}

bool TrayPoll(Tray* t, TrayEvent& ev, int& arg)
{
    std::lock_guard<std::mutex> lk(t->mu);
    if (t->events.empty()) return false;
    ev = t->events.front().first; arg = t->events.front().second;
    t->events.pop_front();
    return true;
}

void TrayNotify(Tray* t, const wchar_t* title, const wchar_t* text)
{
    // Shell_NotifyIcon is thread-agnostic; the shell serialises it against the tray thread's modifies.
    NOTIFYICONDATAW n = Nid(t);
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title ? title : L"", _TRUNCATE);
    wcsncpy_s(n.szInfo, text ? text : L"", _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TraySetHotkeys(Tray* t, const wchar_t* const* five)
{
    std::lock_guard<std::mutex> lk(t->mu);
    for (int i = 0; i < 5; ++i) t->hk[i] = five[i] ? five[i] : L"";
}

void TrayGetHotkeys(Tray* t, wchar_t out[5][32])
{
    std::lock_guard<std::mutex> lk(t->mu);
    for (int i = 0; i < 5; ++i) wcsncpy_s(out[i], 32, t->hk[i].c_str(), _TRUNCATE);
}
