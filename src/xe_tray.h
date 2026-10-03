// justflow_xe's tray icon: a hidden message window on its own thread, Shell_NotifyIcon, a popup menu
// built from the last pushed state. The main loop pushes state in (XeTraySet) and drains the user's
// choices out (XeTrayPoll); both are thread-safe. Re-adds the icon when Explorer restarts.
#pragma once
#include <windows.h>
#include <string>

enum XeTrayEvent { XeTrayNone = 0, XeTrayToggleAuto, XeTrayToggleLock120, XeTrayBatteryFull, XeTrayBatteryEconomy, XeTrayBatteryOff,
                   XeTrayAlways, XeTrayNever, XeTrayForget, XeTrayOpenLog, XeTrayQuit };

struct XeTrayState
{
    bool auto_on = true, lock120 = true;
    int  battery = 1;      // on battery: 0 full, 1 economy, 2 off
    std::wstring status;   // first menu line and the tooltip
    std::wstring app;      // exe of the window the per-app items apply to ("" = none)
    int  app_rule = 0;     // 0 automatic, 1 always, 2 never
};

struct XeTray;
XeTray* XeTrayCreate();
void    XeTrayDestroy(XeTray* t);
void    XeTraySet(XeTray* t, const XeTrayState& s);
bool    XeTrayPoll(XeTray* t, XeTrayEvent& ev);
