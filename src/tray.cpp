// justflow's system tray: the menu, dialogs and tooltip on tray_host's thread and topmost top-level window.
// State crossing threads goes through Tray::mu; the menu is rebuilt from the snapshot each time it pops,
// so there is no check-mark state to keep in sync.
#include "tray.h"
#include "tray_host.h"
#include "config.h"   // FgEngine
#include <algorithm>
#include "settings.h"
#include <string>
#include <vector>

namespace
{
const UINT WM_TRAY_SETTINGS = WM_APP + 3;   // double-click: open Settings once the menu loop has unwound

enum { IDM_STATUS = 1, IDM_NR, IDM_FILTERS, IDM_FG, IDM_FG_POPUP, IDM_MULT2, IDM_MULT3, IDM_MULT4, IDM_ENG0, IDM_ENG1, IDM_ENG2, IDM_ENG3, IDM_WIPE, IDM_RELOAD,
       IDM_SETTINGS, IDM_NEWPROFILE, IDM_PROFILE_RESET, IDM_PROFILE_REMOVE, IDM_CONFIG, IDM_APPCONFIG, IDM_LOG, IDM_QUIT,
       IDM_PRESET0 = 60, IDM_PROFILE0 = 100 };
}

struct Tray : TrayHost
{
    std::wstring app;
    bool   in_menu = false, in_dialog = false;   // tray thread only: both run modal loops that pump messages
    HICON  icon = nullptr;
    TrayQueue<std::pair<TrayEvent, int>> events;

    std::wstring app_ini, profile_ini;   // guarded by mu; the dialog copies them before it blocks

    std::mutex mu;   // guards everything below
    bool nr_on = true, fg_on = false, filters_on = true;
    int  mult = 2, engine = 0, wipe = 0, profile = -1;
    HWND game = nullptr;
    std::wstring status;
    std::vector<std::wstring> profiles;

    void AddIcon() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
};

static void Push(Tray* t, TrayEvent ev, int arg = 0) { t->events.Push({ ev, arg }); }

// ---- settings dialog ---------------------------------------------------------------------------

// Copies of the paths: the dialogs and message boxes below block, and the menu holds t->mu while it
// reads them (std::mutex is not recursive).
struct Paths { std::wstring app, profile; HWND game; };
static Paths GetPaths(Tray* t) { std::lock_guard<std::mutex> lk(t->mu); return { t->app_ini, t->profile_ini, t->game }; }

struct InDialog { bool& f; InDialog(bool& x) : f(x) { f = true; } ~InDialog() { f = false; } };

// profiles\<name>.ini -> the directory that holds them
static std::wstring ProfilesDir(const std::wstring& profile_ini)
{
    const size_t sl = profile_ini.find_last_of(L"\\/");
    return sl == std::wstring::npos ? L"profiles" : profile_ini.substr(0, sl);
}

// profiles\<name>.ini -> <name>; "" for no profile
static std::wstring ProfileName(const std::wstring& ini)
{
    const size_t sl = ini.find_last_of(L"\\/"), from = sl == std::wstring::npos ? 0 : sl + 1, dot = ini.rfind(L".ini");
    return ini.substr(from, dot == std::wstring::npos ? std::wstring::npos : dot - from);
}

// The shipped default a profile can be reset to: profiles\defaults\ is refreshed by every build and is
// never live config, so it is always the pristine copy.
static std::wstring DefaultsPath(const std::wstring& ini) { return ProfilesDir(ini) + L"\\defaults\\" + ProfileName(ini) + L".ini"; }

static void ShowNewProfileDialog(Tray* t)
{
    InDialog guard(t->in_dialog);
    const Paths p = GetPaths(t);
    if (NewProfileDialog(t->hwnd, ProfilesDir(p.profile).c_str(), p.app.c_str(), t->app.c_str()))
        Push(t, TrayRescanProfiles);
}

static void ResetProfile(Tray* t)
{
    const std::wstring path = GetPaths(t).profile, name = ProfileName(path), def = DefaultsPath(path);
    if (name.empty() || GetFileAttributesW(def.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::wstring q = L"Reset \"" + name + L"\" to its shipped defaults?\n\nEvery setting in this profile is replaced. The app settings (hotkeys, HUD) are not touched.";
    if (MessageBoxW(t->hwnd, q.c_str(), t->app.c_str(), MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST) != IDOK) return;
    if (CopyFileW(def.c_str(), path.c_str(), FALSE)) Push(t, TrayReload);
    else MessageBoxW(t->hwnd, L"Could not write the profile.", t->app.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST);
}

static void RemoveProfile(Tray* t)
{
    const Paths p = GetPaths(t); const std::wstring name = ProfileName(p.profile);
    if (name.empty()) return;
    const std::wstring q = L"Remove the profile \"" + name + L"\"?\n\nIt goes to the Recycle Bin, and JustFlow detaches until you pick another profile.";
    if (MessageBoxW(t->hwnd, q.c_str(), t->app.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST) != IDOK) return;
    // Recycle Bin, not DeleteFile: a profile can hold a lot of tuning, and "remove" should be undoable.
    std::wstring from = p.profile; from.push_back(L'\0');   // SHFileOperation wants a double-NUL-terminated list
    SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0) { MessageBoxW(t->hwnd, L"Could not remove the profile.", t->app.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST); return; }
    WritePrivateProfileStringW(L"app", L"profile", L"none", p.app.c_str());
    Push(t, TrayRescanProfiles);
}

static void ShowSettingsDialog(Tray* t)
{
    InDialog guard(t->in_dialog);
    const Paths p = GetPaths(t);
    // Apply reloads with the window still open; OK reloads only for what changed after the last Apply.
    if (SettingsDialog(t->hwnd, p.app.c_str(), p.profile.c_str(), t->app.c_str(), p.game,
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
    static const wchar_t* const kEngines[FG_ENGINE_COUNT] = { L"DLSS-G (interpolate)", L"Warp (extrapolate, no added latency)",
        L"Latewarp (Frame Warp to the mouse)", L"Video (smooth playback, one frame late)" };
    for (int i = 0; i < FG_ENGINE_COUNT; ++i) AppendMenuW(fg, MF_STRING, IDM_ENG0 + i, kEngines[i]);
    CheckMenuRadioItem(fg, IDM_ENG0, IDM_ENG3, IDM_ENG0 + std::clamp(t->engine, 0, FG_ENGINE_COUNT - 1), MF_BYCOMMAND);
    AppendMenuW(fg, MF_SEPARATOR, 0, nullptr);
    const UINT per_frame = t->engine == FG_LATEWARP || t->engine == FG_VIDEO ? MF_GRAYED : 0;   // both run at the display's refresh, not a multiple
    for (int x = 2; x <= 4; ++x) AppendMenuW(fg, MF_STRING | per_frame, IDM_MULT2 + x - 2, (std::to_wstring(x) + L"X").c_str());
    CheckMenuRadioItem(fg, IDM_MULT2, IDM_MULT4, IDM_MULT2 + std::clamp(t->mult, 2, 4) - 2, MF_BYCOMMAND);
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
    if (const std::wstring name = ProfileName(t->profile_ini); !name.empty())
    {
        const bool has_def = GetFileAttributesW(DefaultsPath(t->profile_ini).c_str()) != INVALID_FILE_ATTRIBUTES;
        AppendMenuW(pr, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pr, MF_STRING | (has_def ? 0 : MF_GRAYED), IDM_PROFILE_RESET, (L"Reset \"" + name + L"\" to defaults").c_str());
        AppendMenuW(pr, MF_STRING, IDM_PROFILE_REMOVE, (L"Remove \"" + name + L"\"...").c_str());
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
    t->in_menu = true;    // the menu loop pumps messages: a second click must not nest another menu inside this one
    const int cmd = t->TrackMenu(BuildMenu(t), true);
    t->in_menu = false;
    switch (cmd)
    {
    case IDM_NR:      Push(t, TrayToggleNr); break;
    case IDM_FILTERS: Push(t, TrayToggleFilters); break;
    case IDM_FG:      Push(t, TrayToggleFg); break;
    case IDM_MULT2: case IDM_MULT3: case IDM_MULT4: Push(t, TrayFgMultiplier, cmd - IDM_MULT2 + 2); break;
    case IDM_ENG0: case IDM_ENG1: case IDM_ENG2: case IDM_ENG3: Push(t, TrayFgEngine, cmd - IDM_ENG0); break;
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
            PresetApply(GetPaths(t).profile.c_str(), cmd - IDM_PRESET0);
            Push(t, TrayReload);
        }
        break;
    }
}

// ---- notify icon / window ----------------------------------------------------------------------

static void SetTip(Tray* t, NOTIFYICONDATAW& n)   // caller holds mu
{
    n.uFlags |= NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = t->app;
    if (!t->status.empty()) tip += L"\n" + t->status;
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
}

void Tray::AddIcon()
{
    NOTIFYICONDATAW n = Nid();
    n.uFlags = NIF_ICON | NIF_MESSAGE;
    n.uCallbackMessage = WM_TRAY_ICON;
    n.hIcon = icon;
    { std::lock_guard<std::mutex> lk(mu); SetTip(this, n); }
    Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}

bool Tray::OnMessage(UINT msg, WPARAM, LPARAM lp)
{
    if (msg == WM_TRAY_ICON)
    {
        switch (LOWORD(lp))
        {
        // The first click of a double-click has already opened the menu and is sitting in its modal
        // loop, so this arrives nested inside it: close the menu and open Settings from a posted
        // message, after that loop has unwound, rather than stacking a dialog on top of it.
        case WM_LBUTTONDBLCLK: EndMenu(); PostMessageW(hwnd, WM_TRAY_SETTINGS, 0, 0); break;
        case WM_CONTEXTMENU: case NIN_SELECT: case NIN_KEYSELECT: if (!in_menu && !in_dialog) ShowMenu(this); break;
        }
        return true;
    }
    if (msg == WM_TRAY_SETTINGS) { if (!in_dialog) ShowSettingsDialog(this); return true; }
    if (msg == WM_TRAY_STATE)   // tooltip only: the menu is rebuilt from the state each time it pops
    {
        NOTIFYICONDATAW n = Nid();
        { std::lock_guard<std::mutex> lk(mu); SetTip(this, n); }
        Shell_NotifyIconW(NIM_MODIFY, &n);
        return true;
    }
    return false;
}

// ---- public API --------------------------------------------------------------------------------

Tray* TrayCreate(const wchar_t* app_name)
{
    Tray* t = new Tray;
    t->app = app_name;
    t->icon = TrayLoadIcon(1);   // the exe's icon (src/justflow.rc, art/justflow.ico)
    if (!t->Start(L"JustFlowTray", false)) { if (t->icon) DestroyIcon(t->icon); delete t; return nullptr; }
    return t;
}

void TrayDestroy(Tray* t)
{
    if (!t) return;
    t->Stop();
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
    std::pair<TrayEvent, int> e;
    if (!t->events.Pop(e)) return false;
    ev = e.first; arg = e.second;
    return true;
}

void TrayNotify(Tray* t, const wchar_t* title, const wchar_t* text)
{
    // Shell_NotifyIcon is thread-agnostic; the shell serialises it against the tray thread's modifies.
    NOTIFYICONDATAW n = t->Nid();
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title ? title : L"", _TRUNCATE);
    wcsncpy_s(n.szInfo, text ? text : L"", _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

