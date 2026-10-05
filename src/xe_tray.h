// justflow_xe's tray icon: a hidden message window on its own thread, Shell_NotifyIcon, a popup menu
// built from the last pushed state. The main loop pushes state in (XeTraySet) and drains the user's
// choices out (XeTrayPoll); both are thread-safe. Re-adds the icon when Explorer restarts.
#pragma once
#include <windows.h>
#include <string>

// The values double as the popup menu's command ids (0 = menu dismissed). Battery Full/Economy/Off stay
// consecutive: xe_app maps them to Settings::battery 0..2.
enum XeTrayEvent { XeTrayNone = 0, XeTrayToggleAuto, XeTrayToggleLock120, XeTrayToggleExtrap, XeTrayToggleSync, XeTrayBatteryFull, XeTrayBatteryEconomy, XeTrayBatteryOff,
                   XeTrayAlways, XeTrayNever, XeTrayForget, XeTrayOpenLog, XeTrayQuit };

enum XeTrayIcon { XeIconGenerating = 0, XeIconWatching = 1, XeIconOff = 2 };   // src/justflow_xe.rc 1..3

struct XeTrayState
{
    int  icon = XeIconWatching;   // XeTrayIcon
    bool auto_on = true, lock120 = true, extrap = false, vsync = true;
    int  battery = 1;      // on battery: 0 full, 1 economy, 2 off
    std::wstring status;   // first menu line and the tooltip
    std::wstring advice;   // second menu line when set: the cap advisor's suggestion (xe_app.cpp)
    std::wstring app;      // exe of the window the per-app items apply to ("" = none)
    int  app_rule = 0;     // 0 automatic, 1 always, 2 never
};

struct XeTray;
XeTray* XeTrayCreate();
void    XeTrayDestroy(XeTray* t);
void    XeTraySet(XeTray* t, const XeTrayState& s);
bool    XeTrayPoll(XeTray* t, XeTrayEvent& ev);
