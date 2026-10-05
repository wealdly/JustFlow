#include "mouse.h"
#include "log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct Sample { LONGLONG t; int dx, dy; bool held; };
struct Obs { LONGLONG t0, t1; float mx, my; };
struct Fit { bool ok = false; double gx = 0, gy = 0, r2 = 0; int n = 0; };

const size_t kRing = 16384, kObs = 240;
std::mutex mu;                              // guards everything below
std::vector<Sample> ring(kRing); size_t head = 0, used = 0;
std::vector<Obs> obs; int since_fit = 0;
Fit fit[2];                                 // [0] no button held, [1] a button held
double delay_ms = 0;
std::thread th; std::atomic<bool> running{ false }; std::atomic<DWORD> tid{ 0 };
bool held_l = false, held_r = false, held_m = false;   // input thread only

LONGLONG Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
double QpcFreq() { static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }(); return f; }
// caller holds mu
void Push(const Sample& s) { ring[head] = s; head = (head + 1) % kRing; used = std::min(used + 1, kRing); }
void AddObs(const Obs& o) { obs.push_back(o); if (obs.size() > kObs) obs.erase(obs.begin()); }
void Clear() { head = used = 0; obs.clear(); fit[0] = fit[1] = Fit(); delay_ms = 0; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT)
    {
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE& r = ri.data.mouse; const USHORT f = r.usButtonFlags;
            if (f & RI_MOUSE_LEFT_BUTTON_DOWN) held_l = true;   if (f & RI_MOUSE_LEFT_BUTTON_UP) held_l = false;
            if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) held_r = true;  if (f & RI_MOUSE_RIGHT_BUTTON_UP) held_r = false;
            if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) held_m = true; if (f & RI_MOUSE_MIDDLE_BUTTON_UP) held_m = false;
            if (!(r.usFlags & MOUSE_MOVE_ABSOLUTE) && (r.lLastX || r.lLastY))   // absolute = tablets / remote desktop: no counts
            {
                std::lock_guard<std::mutex> lk(mu);
                Push({ Qpc(), (int)r.lLastX, (int)r.lLastY, held_l || held_r || held_m });
            }
        }
    }
    return DefWindowProcW(h, m, w, l);   // WM_INPUT needs it for cleanup
}

void Thread()
{
    tid = GetCurrentThreadId();
    WNDCLASSW wc = {}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"JustFlowMouse";
    RegisterClassW(&wc);
    // a plain hidden window, never shown: it exists only to receive WM_INPUT in the background
    HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE d = { 0x01, 0x02, RIDEV_INPUTSINK, wnd };   // generic desktop / mouse
    const bool ok = wnd && RegisterRawInputDevices(&d, 1, sizeof d);
    Log("[mouse] raw input %s", ok ? "registered (read-only, mouse only)" : "registration FAILED - latewarp will not warp");
    MSG msg;
    while (ok && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    if (ok) { d.dwFlags = RIDEV_REMOVE; d.hwndTarget = nullptr; RegisterRawInputDevices(&d, 1, sizeof d); }
    if (wnd) DestroyWindow(wnd);
    Log("[mouse] raw input released");
}

// counts per class in (a, b]; caller holds mu. The ring is in time order: binary search, then walk.
void Sum(LONGLONG a, LONGLONG b, double c[2][2])
{
    c[0][0] = c[0][1] = c[1][0] = c[1][1] = 0;
    auto at = [](size_t i) -> const Sample& { return ring[(head + kRing - used + i) % kRing]; };   // 0 = oldest
    size_t lo = 0, hi = used;
    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (at(mid).t <= a) lo = mid + 1; else hi = mid; }
    for (size_t i = lo; i < used && at(i).t <= b; ++i) { const Sample& s = at(i); c[s.held][0] += s.dx; c[s.held][1] += s.dy; }
}

// Least squares through the origin per class and axis, for every candidate delay; keep the delay
// that explains the most. Frames count for a class only when that class dominates their input.
void Refit()
{
    Fit best[2]; double best_d = 0, best_score = -1;
    for (double d = 0; d <= 120; d += 4)
    {
        const LONGLONG dq = (LONGLONG)(d * QpcFreq() / 1000.0);
        double smc[2][2] = {}, scc[2][2] = {}, smm[2] = {}; int n[2] = {};
        for (const Obs& o : obs)
        {
            double c[2][2]; Sum(o.t0 - dq, o.t1 - dq, c);
            const double a0 = std::fabs(c[0][0]) + std::fabs(c[0][1]), a1 = std::fabs(c[1][0]) + std::fabs(c[1][1]);
            const int k = a1 > a0 ? 1 : 0;
            const double mine = k ? a1 : a0, other = k ? a0 : a1;
            if (mine < 2 || other > 0.2 * mine) continue;   // no input, or mixed
            for (int ax = 0; ax < 2; ++ax) { const double m = ax ? o.my : o.mx; smc[k][ax] += m * c[k][ax]; scc[k][ax] += c[k][ax] * c[k][ax]; smm[k] += m * m; }
            ++n[k];
        }
        Fit f[2]; double score = 0;
        for (int k = 0; k < 2; ++k)
        {
            if (n[k] < 20 || smm[k] <= 0) continue;
            f[k].gx = scc[k][0] > 0 ? smc[k][0] / scc[k][0] : 0; f[k].gy = scc[k][1] > 0 ? smc[k][1] / scc[k][1] : 0;
            // residual = smm - g^2 scc per axis (least squares through the origin)
            const double res = smm[k] - f[k].gx * f[k].gx * scc[k][0] - f[k].gy * f[k].gy * scc[k][1];
            f[k].r2 = 1.0 - res / smm[k]; f[k].n = n[k]; f[k].ok = f[k].r2 >= 0.6;
            score += std::max(0.0, f[k].r2);
        }
        if (score > best_score) { best_score = score; best_d = d; best[0] = f[0]; best[1] = f[1]; }
    }
    const bool changed = best[0].ok != fit[0].ok || best[1].ok != fit[1].ok;
    fit[0] = best[0]; fit[1] = best[1]; delay_ms = best_d;
    if (changed)
        Log("[mouse] model: delay %.0f ms | free %s g=(%.3f,%.3f) r2=%.2f n=%d | button %s g=(%.3f,%.3f) r2=%.2f n=%d",
            delay_ms, fit[0].ok ? "ON" : "off", fit[0].gx, fit[0].gy, fit[0].r2, fit[0].n,
            fit[1].ok ? "ON" : "off", fit[1].gx, fit[1].gy, fit[1].r2, fit[1].n);
}
}   // namespace

bool MouseSelfTest()
{
    std::lock_guard<std::mutex> lk(mu);
    const double qf = QpcFreq(); const LONGLONG ms = (LONGLONG)(qf / 1000.0);
    Clear();
    // 1 kHz mouse for 3 s: a button held for the first half (the camera turns, 0.5 px per count, seen
    // 20 ms later), none for the second (cursor only: the picture does not move).
    for (int i = 0; i < 3000; ++i)
        Push({ (i + 1) * ms, (int)(20 * sin(i * 0.004)), (int)(8 * cos(i * 0.003)), i < 1500 });
    for (LONGLONG t = 30 * ms; t + 11 * ms < 3000 * ms; t += 11 * ms)
    {
        double c[2][2]; Sum(t - 20 * ms, t + 11 * ms - 20 * ms, c);
        AddObs({ t, t + 11 * ms, (float)(0.5 * c[1][0]), (float)(0.5 * c[1][1]) });
        if (obs.size() == kObs && t > 2000 * ms) break;   // a window that holds both halves
    }
    Refit();
    const bool pass = fit[1].ok && std::fabs(fit[1].gx - 0.5) < 0.03 && std::fabs(fit[1].gy - 0.5) < 0.05 && std::fabs(delay_ms - 20) <= 4 && !fit[0].ok;
    Log("[mouse] self-test %s: button g=(%.3f,%.3f) r2 %.2f, free %s, delay %.0f ms (want 0.5, 0.5, off, 20)", pass ? "PASS" : "FAIL", fit[1].gx, fit[1].gy, fit[1].r2, fit[0].ok ? "ON" : "off", delay_ms);
    Clear();
    return pass;
}

void MouseStart()
{
    if (running.exchange(true)) return;
    th = std::thread(Thread);
}

void MouseStop()
{
    if (!running.exchange(false)) return;
    while (!tid) Sleep(1);
    PostThreadMessageW(tid, WM_QUIT, 0, 0);
    th.join(); tid = 0;
}

void MouseObserve(LONGLONG t0, LONGLONG t1, float mx, float my)
{
    if (t1 <= t0) return;
    std::lock_guard<std::mutex> lk(mu);
    AddObs({ t0, t1, mx, my });
    if (++since_fit >= 30) { since_fit = 0; Refit(); }
}

bool MousePredict(LONGLONG t_frame, LONGLONG t_now, float& sx, float& sy)
{
    std::lock_guard<std::mutex> lk(mu);
    sx = sy = 0;
    if (!fit[0].ok && !fit[1].ok) return false;
    // The frame shows input up to t_frame - delay; the screen should show it up to now.
    double c[2][2]; Sum(t_frame - (LONGLONG)(delay_ms * QpcFreq() / 1000.0), t_now, c);
    for (int k = 0; k < 2; ++k) if (fit[k].ok) { sx += (float)(fit[k].gx * c[k][0]); sy += (float)(fit[k].gy * c[k][1]); }
    return true;
}
