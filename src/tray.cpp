// System tray: own thread owns a hidden top-level window (needs to be top-level to receive the
// TaskbarCreated broadcast), the notify icon and the popup menu. Everything crossing threads goes
// through Tray::mu; the menu is rebuilt from the snapshot each time it pops, so there is no
// check-mark state to keep in sync.
#include "tray.h"
#include "config.h"   // FgEngine
#include <algorithm>
#include "settings.h"
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
const UINT WM_TRAY_SETTINGS = WM_APP + 3;   // double-click: open Settings once the menu loop has unwound
const UINT ICON_ID = 1;

enum { IDM_STATUS = 1, IDM_NR, IDM_FILTERS, IDM_FG, IDM_FG_POPUP, IDM_MULT2, IDM_MULT3, IDM_MULT4, IDM_ENG0, IDM_ENG1, IDM_ENG2, IDM_WIPE, IDM_RELOAD,
       IDM_SETTINGS, IDM_NEWPROFILE, IDM_PROFILE_RESET, IDM_PROFILE_REMOVE, IDM_CONFIG, IDM_APPCONFIG, IDM_LOG, IDM_QUIT,
       IDM_PRESET0 = 60, IDM_PROFILE0 = 100 };
}

struct Tray
{
    std::wstring app;
    std::thread  thread;
    HANDLE ready = nullptr;
    HWND   hwnd = nullptr;
    bool   in_menu = false, in_dialog = false;   // tray thread only: both run modal loops that pump messages
    HICON  icon = nullptr;
    UINT   taskbar_created = 0;

    std::wstring app_ini, profile_ini;   // guarded by mu; the dialog copies them before it blocks

    std::mutex mu;   // guards everything below
    bool nr_on = true, fg_on = false, filters_on = true;
    int  mult = 2, engine = 0, wipe = 0, profile = -1;
    HWND game = nullptr;
    std::wstring status;
    std::vector<std::wstring> profiles;
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

// ---- settings dialog ---------------------------------------------------------------------------

// profiles\<name>.ini -> the directory that holds them
static std::wstring ProfilesDir(const std::wstring& profile_ini)
{
    const size_t sl = profile_ini.find_last_of(L"\\/");
    return sl == std::wstring::npos ? L"profiles" : profile_ini.substr(0, sl);
}

static void ShowNewProfileDialog(Tray* t)
{
    struct Guard { bool& f; Guard(bool& x) : f(x) { f = true; } ~Guard() { f = false; } } guard(t->in_dialog);
    std::wstring app, profile;
    { std::lock_guard<std::mutex> lk(t->mu); app = t->app_ini; profile = t->profile_ini; }
    if (NewProfileDialog(t->hwnd, ProfilesDir(profile).c_str(), app.c_str(), t->app.c_str()))
        Push(t, TrayRescanProfiles);
}

// The active profile's name and the shipped default it could be reset to (profiles\\defaults\\ is
// refreshed by every build and is never live config, so it is always the pristine copy).
// NoLock: for callers that already hold t->mu (BuildMenu). std::mutex is not recursive - taking it
// twice on one thread throws, and an exception escaping a window procedure is terminate(): that
// was a crash on every tray-menu open with a profile active.
static std::wstring ActiveProfileNoLock(const std::wstring& ini, std::wstring* path = nullptr, std::wstring* def = nullptr)
{
    if (ini.empty()) return L"";
    const size_t sl = ini.find_last_of(L"\\/"), dot = ini.rfind(L".ini");
    const std::wstring name = ini.substr(sl == std::wstring::npos ? 0 : sl + 1, dot == std::wstring::npos ? std::wstring::npos : dot - (sl == std::wstring::npos ? 0 : sl + 1));
    if (path) *path = ini;
    if (def) *def = ProfilesDir(ini) + L"\\defaults\\" + name + L".ini";
    return name;
}

static std::wstring ActiveProfile(Tray* t, std::wstring* path = nullptr, std::wstring* def = nullptr)
{
    std::wstring ini; { std::lock_guard<std::mutex> lk(t->mu); ini = t->profile_ini; }
    return ActiveProfileNoLock(ini, path, def);
}

static void ResetProfile(Tray* t)
{
    std::wstring path, def; const std::wstring name = ActiveProfile(t, &path, &def);
    if (name.empty() || GetFileAttributesW(def.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::wstring q = L"Reset \"" + name + L"\" to its shipped defaults?\n\nEvery setting in this profile is replaced. The app settings (hotkeys, HUD) are not touched.";
    if (MessageBoxW(t->hwnd, q.c_str(), t->app.c_str(), MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST) != IDOK) return;
    if (CopyFileW(def.c_str(), path.c_str(), FALSE)) Push(t, TrayReload);
    else MessageBoxW(t->hwnd, L"Could not write the profile.", t->app.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST);
}

static void RemoveProfile(Tray* t)
{
    std::wstring path, app; const std::wstring name = ActiveProfile(t, &path);
    if (name.empty()) return;
    { std::lock_guard<std::mutex> lk(t->mu); app = t->app_ini; }
    const std::wstring q = L"Remove the profile \"" + name + L"\"?\n\nIt goes to the Recycle Bin, and JustFlow detaches until you pick another profile.";
    if (MessageBoxW(t->hwnd, q.c_str(), t->app.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST) != IDOK) return;
    // Recycle Bin, not DeleteFile: a profile can hold a lot of tuning, and "remove" should be undoable.
    std::wstring from = path; from.push_back(L'\0');   // SHFileOperation wants a double-NUL-terminated list
    SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0) { MessageBoxW(t->hwnd, L"Could not remove the profile.", t->app.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST); return; }
    WritePrivateProfileStringW(L"app", L"profile", L"none", app.c_str());
    Push(t, TrayRescanProfiles);
}

static void ShowSettingsDialog(Tray* t)
{
    struct Guard { bool& f; Guard(bool& x) : f(x) { f = true; } ~Guard() { f = false; } } guard(t->in_dialog);
    std::wstring app, profile; HWND game;
    { std::lock_guard<std::mutex> lk(t->mu); app = t->app_ini; profile = t->profile_ini; game = t->game; }
    // Apply reloads with the window still open; OK reloads only for what changed after the last Apply.
    if (SettingsDialog(t->hwnd, app.c_str(), profile.c_str(), t->app.c_str(), game,
                       [](void* c) { Push((Tray*)c, TrayReload); }, t)) Push(t, TrayReload);
}

// ---- menu --------------------------------------------------------------------------------------

static HMENU BuildMenu(Tray* t)
{
    std::lock_guard<std::mutex> lk(t->mu);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_STATUS, t->status.empty() ? t->app.c_str() : t->status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // the three layer switches, named as they are in Settings and on the HUD
    AppendMenuW(m, MF_STRING | (t->nr_on ? MF_CHECKED : 0), IDM_NR, L"Neural layer\tF9");
    AppendMenuW(m, MF_STRING | (t->filters_on ? MF_CHECKED : 0), IDM_FILTERS, L"Filter layer\tF6");

    HMENU fg = CreatePopupMenu();
    AppendMenuW(fg, MF_STRING | (t->fg_on ? MF_CHECKED : 0), IDM_FG, L"Enabled");
    AppendMenuW(fg, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fg, MF_STRING, IDM_ENG0, L"DLSS-G (interpolate)");
    AppendMenuW(fg, MF_STRING, IDM_ENG1, L"Warp (extrapolate, no added latency)");
    AppendMenuW(fg, MF_STRING, IDM_ENG2, L"Latewarp (Frame Warp to the mouse)");
    CheckMenuRadioItem(fg, IDM_ENG0, IDM_ENG2, IDM_ENG0 + std::clamp(t->engine, 0, FG_ENGINE_COUNT - 1), MF_BYCOMMAND);
    AppendMenuW(fg, MF_SEPARATOR, 0, nullptr);
    const UINT per_frame = t->engine == FG_LATEWARP ? MF_GRAYED : 0;   // latewarp runs at the display's refresh, not a multiple
    AppendMenuW(fg, MF_STRING | per_frame, IDM_MULT2, L"2X");
    AppendMenuW(fg, MF_STRING | per_frame, IDM_MULT3, L"3X");
    AppendMenuW(fg, MF_STRING | per_frame, IDM_MULT4, L"4X");
    const int mult = t->mult < 2 ? 2 : t->mult > 4 ? 4 : t->mult;
    CheckMenuRadioItem(fg, IDM_MULT2, IDM_MULT4, IDM_MULT2 + mult - 2, MF_BYCOMMAND);
    MENUITEMINFOW mi = { sizeof mi };
    mi.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_STATE | MIIM_ID;
    mi.wID = IDM_FG_POPUP; mi.hSubMenu = fg;
    mi.fState = t->fg_on ? MFS_CHECKED : MFS_UNCHECKED;
    mi.dwTypeData = const_cast<wchar_t*>(L"Frame generation");
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);

    AppendMenuW(m, MF_STRING | (t->wipe ? MF_CHECKED : 0), IDM_WIPE, L"Wipe compare");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU q = CreatePopupMenu();
    for (int i = 0; i < kPresetCount; ++i) AppendMenuW(q, MF_STRING, IDM_PRESET0 + i, PresetName(i));
    const int cur = PresetCurrent(t->profile_ini.c_str());
    if (cur >= 0) CheckMenuRadioItem(q, IDM_PRESET0, IDM_PRESET0 + kPresetCount - 1, IDM_PRESET0 + cur, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)q, L"Quality");

    HMENU pr = CreatePopupMenu();
    if (t->profiles.empty()) AppendMenuW(pr, MF_STRING | MF_GRAYED, 0, L"(none)");
    for (size_t i = 0; i < t->profiles.size(); ++i) AppendMenuW(pr, MF_STRING, IDM_PROFILE0 + i, t->profiles[i].c_str());
    if (t->profile >= 0 && t->profile < (int)t->profiles.size())
        CheckMenuRadioItem(pr, IDM_PROFILE0, IDM_PROFILE0 + (UINT)t->profiles.size() - 1, IDM_PROFILE0 + t->profile, MF_BYCOMMAND);
    {
        std::wstring def; const std::wstring name = ActiveProfileNoLock(t->profile_ini, nullptr, &def);   // t->mu is held here
        if (!name.empty())
        {
            const bool has_def = GetFileAttributesW(def.c_str()) != INVALID_FILE_ATTRIBUTES;
            AppendMenuW(pr, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(pr, MF_STRING | (has_def ? 0 : MF_GRAYED), IDM_PROFILE_RESET, (L"Reset \"" + name + L"\" to defaults").c_str());
            AppendMenuW(pr, MF_STRING, IDM_PROFILE_REMOVE, (L"Remove \"" + name + L"\"...").c_str());
        }
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pr, L"Profiles");
    AppendMenuW(m, MF_STRING, IDM_NEWPROFILE, L"New profile from window...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CONFIG, L"Open profile ini");
    AppendMenuW(m, MF_STRING, IDM_APPCONFIG, L"Open app ini");
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
    // Keep the menu clear of the taskbar itself: TrackPopupMenuEx flips it to the other side of
    // rcExclude rather than letting it open underneath.
    TPMPARAMS tp = { sizeof tp };
    APPBARDATA ab = { sizeof ab };
    const bool have_bar = SHAppBarMessage(ABM_GETTASKBARPOS, &ab) != 0;
    if (have_bar) tp.rcExclude = ab.rc;
    t->in_menu = true;    // TrackPopupMenuEx pumps messages: a second click must not nest another menu inside this one
    const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, t->hwnd, have_bar ? &tp : nullptr);
    t->in_menu = false;
    PostMessageW(t->hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);   // destroys submenus too
    switch (cmd)
    {
    case IDM_NR:      Push(t, TrayToggleNr); break;
    case IDM_FILTERS: Push(t, TrayToggleFilters); break;
    case IDM_FG:      Push(t, TrayToggleFg); break;
    case IDM_MULT2: case IDM_MULT3: case IDM_MULT4: Push(t, TrayFgMultiplier, cmd - IDM_MULT2 + 2); break;
    case IDM_ENG0: case IDM_ENG1: case IDM_ENG2: Push(t, TrayFgEngine, cmd - IDM_ENG0); break;
    case IDM_WIPE:    Push(t, TrayWipe); break;
    case IDM_RELOAD:  Push(t, TrayReload); break;
    case IDM_CONFIG:  Push(t, TrayOpenConfig); break;
    case IDM_APPCONFIG: Push(t, TrayOpenAppConfig); break;
    case IDM_LOG:     Push(t, TrayOpenLog); break;
    case IDM_QUIT:    Push(t, TrayQuit); break;
    case IDM_SETTINGS: ShowSettingsDialog(t); break;
    case IDM_NEWPROFILE: ShowNewProfileDialog(t); break;
    case IDM_PROFILE_RESET:  ResetProfile(t); break;
    case IDM_PROFILE_REMOVE: RemoveProfile(t); break;
    default:
        if (cmd >= IDM_PROFILE0) Push(t, TraySelectProfile, cmd - IDM_PROFILE0);
        else if (cmd >= IDM_PRESET0 && cmd < IDM_PRESET0 + kPresetCount)
        {
            std::wstring profile;
            { std::lock_guard<std::mutex> lk(t->mu); profile = t->profile_ini; }
            PresetApply(profile.c_str(), cmd - IDM_PRESET0);
            Push(t, TrayReload);
        }
        break;
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
        switch (LOWORD(lp))
        {
        // The first click of a double-click has already opened the menu and is sitting in its modal
        // loop, so this arrives nested inside it: close the menu and open Settings from a posted
        // message, after that loop has unwound, rather than stacking a dialog on top of it.
        case WM_LBUTTONDBLCLK: EndMenu(); PostMessageW(h, WM_TRAY_SETTINGS, 0, 0); break;
        case WM_CONTEXTMENU: case NIN_SELECT: case NIN_KEYSELECT: if (!t->in_menu && !t->in_dialog) ShowMenu(t); break;
        }
        return 0;
    }
    if (msg == WM_TRAY_SETTINGS) { if (!t->in_dialog) ShowSettingsDialog(t); return 0; }
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
    t->icon = MakeIcon();
    t->hwnd = CreateWindowExW(0, wc.lpszClassName, t->app.c_str(), WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, t);
    // A popup menu is drawn above its OWNER. The taskbar is topmost, so a menu owned by an ordinary
    // hidden window comes up behind it and the bottom entries (Quit) cannot be clicked. Topmost owner,
    // topmost menu.
    if (t->hwnd) SetWindowPos(t->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (t->hwnd) AddIcon(t);
    SetEvent(t->ready);
    if (!t->hwnd) return;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// ---- public API --------------------------------------------------------------------------------

Tray* TrayCreate(const wchar_t* app_name)
{
    Tray* t = new Tray;
    t->app = app_name;
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

void TraySetPaths(Tray* t, const wchar_t* app_ini, const wchar_t* profile_ini)
{
    std::lock_guard<std::mutex> lk(t->mu);
    t->app_ini = app_ini ? app_ini : L"";
    t->profile_ini = profile_ini ? profile_ini : L"";
}

void TraySetState(Tray* t, const TrayState& s)
{
    {
        std::lock_guard<std::mutex> lk(t->mu);
        t->nr_on = s.nr_on; t->fg_on = s.fg_on; t->filters_on = s.filters_on; t->mult = s.fg_multiplier; t->engine = s.fg_engine;
        t->wipe = s.wipe_mode; t->profile = s.profile_index; t->game = s.game;
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

