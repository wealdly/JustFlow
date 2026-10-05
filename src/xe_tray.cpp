// justflow_xe's tray: tray_host's thread and message-only window, three state icons, and the menu built
// from the last pushed state each time it opens.
#include "xe_tray.h"
#include "tray_host.h"
#include "log.h"

struct XeTray : TrayHost
{
    std::mutex mu;   // guards state
    XeTrayState state;
    TrayQueue<XeTrayEvent> events;
    HICON icons[3] = {};   // XeTrayIcon: generating, watching, off (resources 1..3)

    void Show(DWORD msg);   // NIM_ADD / NIM_MODIFY with the current icon and tooltip
    void AddIcon() override { Show(NIM_ADD); }
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
};

void XeTray::Show(DWORD msg)
{
    NOTIFYICONDATAW n = Nid();
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; n.uCallbackMessage = WM_TRAY_ICON;
    std::wstring tip; int icon;
    { std::lock_guard<std::mutex> lk(mu); tip = L"JustFlow XE - " + state.status; icon = state.icon; }
    n.hIcon = icons[icon >= 0 && icon < 3 ? icon : 1];
    if (!n.hIcon) n.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(msg, &n);
}

static void ShowMenu(XeTray* t)
{
    XeTrayState s;
    { std::lock_guard<std::mutex> lk(t->mu); s = t->state; }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s.status.c_str());
    if (!s.advice.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s.advice.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.auto_on ? MF_CHECKED : 0), XeTrayToggleAuto, L"Automatic frame generation");
    AppendMenuW(m, MF_STRING | (s.lock120 ? MF_CHECKED : 0), XeTrayToggleLock120, L"Lock output to the display rate");
    AppendMenuW(m, MF_STRING | (s.extrap ? MF_CHECKED : 0), XeTrayToggleExtrap, L"Low latency (extrapolate, no hold)");
    AppendMenuW(m, MF_STRING | (s.vsync ? MF_CHECKED : 0), XeTrayToggleSync, L"Vsync (no tearing; off = lowest latency)");
    HMENU bat = CreatePopupMenu();
    AppendMenuW(bat, MF_STRING | (s.battery == 0 ? MF_CHECKED : 0), XeTrayBatteryFull, L"Full");
    AppendMenuW(bat, MF_STRING | (s.battery == 1 ? MF_CHECKED : 0), XeTrayBatteryEconomy, L"Economy (lighter flow, 30 fps and up)");
    AppendMenuW(bat, MF_STRING | (s.battery == 2 ? MF_CHECKED : 0), XeTrayBatteryOff, L"Off (\"Always\" apps still run, in economy)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)bat, L"On battery");
    if (!s.app.empty())
    {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (s.app_rule == 1 ? MF_CHECKED : 0), XeTrayAlways, (L"Always for " + s.app).c_str());
        AppendMenuW(m, MF_STRING | (s.app_rule == 2 ? MF_CHECKED : 0), XeTrayNever, (L"Never for " + s.app).c_str());
        AppendMenuW(m, MF_STRING | (s.app_rule == 0 ? MF_CHECKED : 0), XeTrayForget, (L"Automatic for " + s.app).c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, XeTrayOpenLog, L"Open log");
    AppendMenuW(m, MF_STRING, XeTrayQuit, L"Quit");
    const int cmd = t->TrackMenu(m, false);   // 0 = dismissed (XeTrayNone); menu ids are the XeTrayEvent values
    if (cmd != XeTrayNone) t->events.Push((XeTrayEvent)cmd);
}

bool XeTray::OnMessage(UINT msg, WPARAM, LPARAM lp)
{
    if (msg == WM_TRAY_ICON && (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)) { ShowMenu(this); return true; }
    if (msg == WM_TRAY_STATE) { Show(NIM_MODIFY); return true; }   // tooltip and icon
    return false;
}

XeTray* XeTrayCreate()
{
    XeTray* t = new XeTray;
    t->state.status = L"starting";
    for (int i = 0; i < 3; ++i) if (!(t->icons[i] = TrayLoadIcon(i + 1))) Log("[tray] icon resource %d did not load (err %lu) - stock icon instead", i + 1, GetLastError());
    if (!t->Start(L"JustFlowXeTray", true)) { XeTrayDestroy(t); return nullptr; }
    return t;
}

void XeTrayDestroy(XeTray* t)
{
    if (!t) return;
    t->Stop();
    for (HICON i : t->icons) if (i) DestroyIcon(i);
    delete t;
}

void XeTraySet(XeTray* t, const XeTrayState& s)
{
    bool changed;   // only the tooltip and icon are live; the menu is built from the state when opened
    {
        std::lock_guard<std::mutex> lk(t->mu);
        changed = s.status != t->state.status || s.icon != t->state.icon;
        t->state = s;
    }
    if (changed && t->hwnd) PostMessageW(t->hwnd, WM_TRAY_STATE, 0, 0);
}

bool XeTrayPoll(XeTray* t, XeTrayEvent& ev) { return t->events.Pop(ev); }
