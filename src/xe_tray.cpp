#include "xe_tray.h"
#include <shellapi.h>
#include <deque>
#include <mutex>
#include <thread>

#pragma comment(lib, "shell32.lib")

enum { WM_TRAY = WM_APP + 1, ID_AUTO = 1, ID_LOCK, ID_BAT_FULL, ID_BAT_ECO, ID_BAT_OFF, ID_ALWAYS, ID_NEVER, ID_FORGET, ID_LOG, ID_QUIT };

struct XeTray
{
    std::thread th;
    HWND hwnd = nullptr;
    UINT taskbar_created = 0;
    std::mutex mu;
    XeTrayState state;
    std::deque<XeTrayEvent> events;
    HANDLE ready = nullptr;
};

static void AddIcon(XeTray* t, DWORD msg)
{
    NOTIFYICONDATAW n = { sizeof n };
    n.hWnd = t->hwnd; n.uID = 1; n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; n.uCallbackMessage = WM_TRAY;
    n.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    std::wstring tip;
    { std::lock_guard<std::mutex> lk(t->mu); tip = L"JustFlow XE - " + t->state.status; }
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(msg, &n);
}

static void ShowMenu(XeTray* t)
{
    XeTrayState s;
    { std::lock_guard<std::mutex> lk(t->mu); s = t->state; }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s.status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.auto_on ? MF_CHECKED : 0), ID_AUTO, L"Automatic frame generation");
    AppendMenuW(m, MF_STRING | (s.lock120 ? MF_CHECKED : 0), ID_LOCK, L"Lock output to the display rate");
    HMENU bat = CreatePopupMenu();
    AppendMenuW(bat, MF_STRING | (s.battery == 0 ? MF_CHECKED : 0), ID_BAT_FULL, L"Full");
    AppendMenuW(bat, MF_STRING | (s.battery == 1 ? MF_CHECKED : 0), ID_BAT_ECO, L"Economy (lighter flow, 30 fps and up)");
    AppendMenuW(bat, MF_STRING | (s.battery == 2 ? MF_CHECKED : 0), ID_BAT_OFF, L"Off (\"Always\" apps still run, in economy)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)bat, L"On battery");
    if (!s.app.empty())
    {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (s.app_rule == 1 ? MF_CHECKED : 0), ID_ALWAYS, (L"Always for " + s.app).c_str());
        AppendMenuW(m, MF_STRING | (s.app_rule == 2 ? MF_CHECKED : 0), ID_NEVER, (L"Never for " + s.app).c_str());
        AppendMenuW(m, MF_STRING | (s.app_rule == 0 ? MF_CHECKED : 0), ID_FORGET, (L"Automatic for " + s.app).c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(t->hwnd);   // required, or the menu does not close when clicking elsewhere
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, t->hwnd, nullptr);
    PostMessageW(t->hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    XeTrayEvent ev = XeTrayNone;
    switch (cmd)
    {
    case ID_AUTO: ev = XeTrayToggleAuto; break;
    case ID_LOCK: ev = XeTrayToggleLock120; break;
    case ID_BAT_FULL: ev = XeTrayBatteryFull; break;
    case ID_BAT_ECO: ev = XeTrayBatteryEconomy; break;
    case ID_BAT_OFF: ev = XeTrayBatteryOff; break;
    case ID_ALWAYS: ev = XeTrayAlways; break;
    case ID_NEVER: ev = XeTrayNever; break;
    case ID_FORGET: ev = XeTrayForget; break;
    case ID_LOG: ev = XeTrayOpenLog; break;
    case ID_QUIT: ev = XeTrayQuit; break;
    }
    if (ev != XeTrayNone) { std::lock_guard<std::mutex> lk(t->mu); t->events.push_back(ev); }
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
    delete t;
}

void XeTraySet(XeTray* t, const XeTrayState& s)
{
    bool changed;
    {
        std::lock_guard<std::mutex> lk(t->mu);
        changed = s.status != t->state.status || s.auto_on != t->state.auto_on || s.lock120 != t->state.lock120 || s.app != t->state.app || s.app_rule != t->state.app_rule || s.battery != t->state.battery;
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
