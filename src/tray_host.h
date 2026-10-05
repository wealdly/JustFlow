// The notify-icon plumbing justflow's and justflow_xe's trays share: a hidden window on its own thread
// owning icon id 1, re-added on the TaskbarCreated broadcast (Explorer restarted), removed on close, and a
// thread-safe queue carrying the user's choices to the main loop. Each tray derives from TrayHost and keeps
// its own icon, tooltip, menu and messages.
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <deque>
#include <mutex>
#include <thread>

const UINT WM_TRAY_ICON  = WM_APP + 1;   // Shell_NotifyIcon callback
const UINT WM_TRAY_STATE = WM_APP + 2;   // the main loop pushed new state: refresh the icon on the tray thread

HICON TrayLoadIcon(int id);   // icon resource `id` at the small-icon size for the system DPI

struct TrayHost
{
    HWND hwnd = nullptr;
    UINT taskbar_created = 0;
    std::thread thread;

    virtual ~TrayHost() = default;
    virtual void AddIcon() = 0;                                  // tray thread: NIM_ADD, at start and after an Explorer restart
    virtual bool OnMessage(UINT msg, WPARAM wp, LPARAM lp) = 0;  // tray thread: the product's messages; true = handled

    // A message-only window never receives broadcasts, so TaskbarCreated reaches only the top-level one,
    // which is also made topmost: a popup menu is drawn above its OWNER, and under an ordinary hidden
    // window it comes up behind the (topmost) taskbar where its bottom entries cannot be clicked.
    bool Start(const wchar_t* window_class, bool message_only);   // false = no window (the thread has ended)
    void Stop();                                                  // removes the icon and joins; call before deleting
    NOTIFYICONDATAW Nid() const { NOTIFYICONDATAW n = { sizeof n }; n.hWnd = hwnd; n.uID = 1; return n; }
    // Pops `m` at the cursor and destroys it; 0 = dismissed. avoid_taskbar flips it to the far side of the
    // taskbar rather than letting it open underneath.
    int TrackMenu(HMENU m, bool avoid_taskbar);
};

template <class T> class TrayQueue
{
    std::mutex mu;
    std::deque<T> q;
public:
    void Push(const T& v) { std::lock_guard<std::mutex> lk(mu); q.push_back(v); }
    bool Pop(T& v) { std::lock_guard<std::mutex> lk(mu); if (q.empty()) return false; v = q.front(); q.pop_front(); return true; }
};
