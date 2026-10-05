#include "xe_tray.h"
#include "log.h"
#include <shellapi.h>
#include <deque>
#include <mutex>
#include <thread>

#pragma comment(lib, "shell32.lib")

enum { WM_TRAY = WM_APP + 1 };   // menu item ids are the XeTrayEvent values

struct XeTray
{
    std::thread th;
    HWND hwnd = nullptr;
    UINT taskbar_created = 0;
    std::mutex mu;
    XeTrayState state;
    std::deque<XeTrayEvent> events;
    HANDLE ready = nullptr;
    HICON icons[3] = {};   // XeTrayIcon: generating, watching, off (resources 1..3)
};

static HICON LoadTrayIcon(int id)
{
    const UINT dpi = GetDpiForSystem();
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON,
                             GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR);
}

static void AddIcon(XeTray* t, DWORD msg)
{
    NOTIFYICONDATAW n = { sizeof n };
    n.hWnd = t->hwnd; n.uID = 1; n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; n.uCallbackMessage = WM_TRAY;
    std::wstring tip; int icon;
    { std::lock_guard<std::mutex> lk(t->mu); tip = L"JustFlow XE - " + t->state.status; icon = t->state.icon; }
    n.hIcon = t->icons[icon >= 0 && icon < 3 ? icon : 1];
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
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(t->hwnd);   // required, or the menu does not close when clicking elsewhere
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, t->hwnd, nullptr);   // 0 = dismissed (XeTrayNone)
    PostMessageW(t->hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd != XeTrayNone) { std::lock_guard<std::mutex> lk(t->mu); t->events.push_back((XeTrayEvent)cmd); }
}

static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    XeTray* t = (XeTray*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (t && msg == t->taskbar_created) { AddIcon(t, NIM_ADD); return 0; }   // Explorer restarted
    if (t && msg == WM_TRAY && (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)) { ShowMenu(t); return 0; }
    if (t && msg == WM_APP + 2) { AddIcon(t, NIM_MODIFY); return 0; }        // state changed: tooltip
    if (msg == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

static void Thread(XeTray* t)
{
    WNDCLASSW wc = {}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"JustFlowXeTray";
    RegisterClassW(&wc);
    t->hwnd = CreateWindowExW(0, wc.lpszClassName, L"JustFlow XE", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (t->hwnd)
    {
        SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, (LONG_PTR)t);
        t->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        AddIcon(t, NIM_ADD);
    }
    SetEvent(t->ready);
    MSG m;
    while (t->hwnd && GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (t->hwnd) { NOTIFYICONDATAW n = { sizeof n }; n.hWnd = t->hwnd; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }
}

XeTray* XeTrayCreate()
{
    XeTray* t = new XeTray;
    t->state.status = L"starting";
    for (int i = 0; i < 3; ++i) if (!(t->icons[i] = LoadTrayIcon(i + 1))) Log("[tray] icon resource %d did not load (err %lu) - stock icon instead", i + 1, GetLastError());
    t->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t->th = std::thread(Thread, t);
    WaitForSingleObject(t->ready, 5000);
    if (!t->hwnd) { XeTrayDestroy(t); return nullptr; }
    return t;
}

void XeTrayDestroy(XeTray* t)
{
    if (!t) return;
    if (t->hwnd) PostMessageW(t->hwnd, WM_CLOSE, 0, 0);
    if (t->th.joinable()) t->th.join();
    if (t->ready) CloseHandle(t->ready);
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
    if (changed && t->hwnd) PostMessageW(t->hwnd, WM_APP + 2, 0, 0);
}

bool XeTrayPoll(XeTray* t, XeTrayEvent& ev)
{
    std::lock_guard<std::mutex> lk(t->mu);
    if (t->events.empty()) return false;
    ev = t->events.front(); t->events.pop_front();
    return true;
}
