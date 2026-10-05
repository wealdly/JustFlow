#include "tray_host.h"

#pragma comment(lib, "shell32.lib")

// The .ico files carry 16-256 px, so Windows picks a real size instead of scaling.
HICON TrayLoadIcon(int id)
{
    const UINT dpi = GetDpiForSystem();
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON,
                             GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR);
}

static LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) { SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); return TRUE; }
    TrayHost* t = (TrayHost*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!t) return DefWindowProcW(h, msg, wp, lp);
    if (msg == t->taskbar_created) { t->AddIcon(); return 0; }
    if (msg == WM_CLOSE) { NOTIFYICONDATAW n = t->Nid(); Shell_NotifyIconW(NIM_DELETE, &n); DestroyWindow(h); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (t->OnMessage(msg, wp, lp)) return 0;
    return DefWindowProcW(h, msg, wp, lp);
}

bool TrayHost::Start(const wchar_t* window_class)
{
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread = std::thread([=] {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {}; wc.lpfnWndProc = HostProc; wc.hInstance = inst; wc.lpszClassName = window_class;
        RegisterClassW(&wc);   // second registration in-process just fails; CreateWindow still works
        taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        hwnd = CreateWindowExW(0, window_class, nullptr, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (hwnd) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (hwnd) AddIcon();
        SetEvent(ready);
        if (!hwnd) return;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    });
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    if (!hwnd) thread.join();
    return hwnd != nullptr;
}

void TrayHost::Stop()
{
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (thread.joinable()) thread.join();
}

int TrayHost::TrackMenu(HMENU m)
{
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd);   // so the menu closes when the user clicks elsewhere
    TPMPARAMS tp = { sizeof tp };
    APPBARDATA ab = { sizeof ab };
    const bool have_bar = SHAppBarMessage(ABM_GETTASKBARPOS, &ab) != 0;
    if (have_bar) tp.rcExclude = ab.rc;
    const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, hwnd, have_bar ? &tp : nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);   // destroys submenus too
    return cmd;
}
