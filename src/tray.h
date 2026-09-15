// System tray icon + menu on its own thread (hidden message window, Shell_NotifyIcon).
// Main loop pushes state in (TraySetState) and drains user actions out (TrayPoll); both thread-safe.
#pragma once
#include <windows.h>

enum TrayEvent
{
    TrayNone = 0,
    TrayToggleNr,
    TrayToggleFg,
    TrayFgMultiplier,   // arg = 2..4
    TrayWipe,           // cycle wipe mode
    TrayReload,
    TraySelectProfile,  // arg = index into TraySetProfiles
    TrayOpenConfig,     // the active profile (profiles\<game>.ini), for hand editing
    TrayOpenAppConfig,  // justflow.ini (hotkeys, ui, overlay, log stats), for hand editing
    TrayOpenLog,
    TrayQuit,
};

struct TrayState
{
    bool nr_on = true, fg_on = false;
    int  fg_multiplier = 2;
    int  wipe_mode = 0;         // 0 = off
    int  profile_index = -1;    // -1 = none
    HWND game = nullptr;        // the captured window; the settings dialog opens on a different monitor
    const wchar_t* status = L"";   // e.g. L"WoW  90->180 fps  age 12 ms" (menu status line + tooltip)
};

struct Tray;

Tray* TrayCreate(const wchar_t* app_name, const wchar_t* icon_path_or_null);   // null = generated icon
void  TrayDestroy(Tray*);
void  TraySetProfiles(Tray*, const wchar_t* const* names, int count);          // Profiles submenu (radio)
// The two ini files the Settings dialog and the quality presets write to. Call again on a profile
// switch; a write is always followed by a TrayReload event.
void  TraySetPaths(Tray*, const wchar_t* app_ini, const wchar_t* profile_ini);
void  TraySetState(Tray*, const TrayState&);                                    // check marks + tooltip
bool  TrayPoll(Tray*, TrayEvent& ev, int& arg);                                 // one event per call
void  TrayNotify(Tray*, const wchar_t* title, const wchar_t* text);             // balloon (NIIF_INFO)
