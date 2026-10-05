// justflow_xe's tray app: automatic frame generation for whichever window is in the foreground.
//
// The foreground window is captured (Windows.Graphics.Capture delivers a frame only when the window
// changes, so a static window costs nothing) and probed: every frame is reduced to a 1/8 luma
// thumbnail on the GPU and compared with the previous one on the CPU a few frames later. Frames that
// actually differ are "distinct" - a player or engine presenting at 120 Hz with 45 fps of content
// counts 45. Frame generation engages on a steady distinct rate from min_fps up to a bound that
// follows the monitor's CURRENT refresh (98% of it with the output locked to the display, 83%
// without: at 60 Hz on battery, 60 fps content has nothing to gain) for kEngageMs, and releases
// after kReleaseMs outside it (static, slower, faster).
// Power: plugged in = full; on battery the tray's policy (default economy: flow at 1/3 resolution,
// measured 0.85 ms instead of 1.64 for -0.9 dB at 45 fps x2, and at least 30 fps content); Battery
// Saver = off, except "always" apps, which run economy. Battery drain is logged engaged vs not.
// One window at a time; losing the foreground hides the overlay at once (present.cpp) and releases
// the window after kAwayMs. Per-app rules (always / never) live in justflow_xe.ini, keyed by the exe
// name, which comes from the process list - never from opening the process (README: no OpenProcess).
// No global hotkeys in this mode: a global F8 would be stolen from every game. The tray is the UI.
#include "xe_app.h"
#include "xe_fg.h"
#include "xe_tray.h"
#include "capture.h"
#include "present.h"
#include "log.h"
#include "cs_gray.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <timeapi.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "powrprof.lib")

static const double kWindowMs = 1500, kEngageMs = 1500, kReleaseMs = 3000, kAwayMs = 10000;

// ---- settings (justflow_xe.ini next to the exe) ------------------------------------------------------------
struct Settings
{
    std::wstring path;
    bool auto_on = true, lock120 = true;
    bool extrap = false;                // low latency: extrapolate ahead of the newest frame instead of interpolating behind it
    bool vsync = true;                  // sync mode: vsync (no tearing, late adaptive render) or immediate (tearing, lowest latency)
    int  min_fps = 25;
    int  battery = 1;                   // on battery: 0 full, 1 economy, 2 off
    // Upper bound of content worth generating for, from the display's current refresh.
    int  Upper(double hz) const { return (int)floor(hz * (lock120 ? 0.98 : 0.83)); }
    bool Is(const wchar_t* sec, const wchar_t* key, const wchar_t* def, const wchar_t* want) const
    {
        wchar_t v[16] = {}; GetPrivateProfileStringW(sec, key, def, v, 16, path.c_str());
        return !_wcsicmp(v, want);
    }
    int  Rule(const std::wstring& exe) const   // 0 automatic, 1 always, 2 never
    {
        return Is(L"apps", exe.c_str(), L"", L"always") ? 1 : Is(L"apps", exe.c_str(), L"", L"never") ? 2 : 0;
    }
    void SetRule(const std::wstring& exe, int r)
    {
        WritePrivateProfileStringW(L"apps", exe.c_str(), r == 1 ? L"always" : r == 2 ? L"never" : nullptr, path.c_str());
    }
    void Load()
    {
        auto I = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"auto", k, d, path.c_str()); };
        auto_on = I(L"enabled", 1) != 0; lock120 = I(L"lock120", 1) != 0;
        min_fps = std::clamp(I(L"min_fps", 25), 10, 100);
        extrap = Is(L"auto", L"engine", L"interpolate", L"extrapolate");
        vsync = !Is(L"auto", L"sync", L"vsync", L"off");
        battery = Is(L"power", L"battery", L"economy", L"full") ? 0 : Is(L"power", L"battery", L"economy", L"off") ? 2 : 1;
    }
    void Save() const
    {
        WritePrivateProfileStringW(L"auto", L"enabled", auto_on ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"auto", L"lock120", lock120 ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"auto", L"engine", extrap ? L"extrapolate" : L"interpolate", path.c_str());
        WritePrivateProfileStringW(L"auto", L"sync", vsync ? L"vsync" : L"off", path.c_str());
        wchar_t v[16];
        swprintf_s(v, L"%d", min_fps); WritePrivateProfileStringW(L"auto", L"min_fps", v, path.c_str());
        WritePrivateProfileStringW(L"auto", L"max_fps", nullptr, path.c_str());   // replaced by the refresh-relative bound
        WritePrivateProfileStringW(L"power", L"battery", battery == 0 ? L"full" : battery == 2 ? L"off" : L"economy", path.c_str());
    }
};

// Exe name of a process from the system's process list (Toolhelp snapshot): the process itself is
// never opened.
static std::wstring ExeOfPid(DWORD pid)
{
    std::wstring name;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return name;
    PROCESSENTRY32W e = { sizeof e };
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e)) if (e.th32ProcessID == pid) { name = e.szExeFile; break; }
    CloseHandle(s);
    return name;
}

// ---- power ---------------------------------------------------------------------------------------------------
enum PowerMode { PowerFull, PowerEconomy, PowerOff };
struct PowerState { bool ac = true, saver = false; int percent = -1; };
static PowerState ReadPower()
{
    PowerState p; SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) return p;
    p.ac = s.ACLineStatus != 0;               // 1 = AC, 255 = unknown (no battery): both count as plugged in
    p.saver = (s.SystemStatusFlag & 1) != 0;  // Battery Saver
    p.percent = s.BatteryLifePercent <= 100 ? s.BatteryLifePercent : -1;
    return p;
}
static void LogPower(const PowerState& p) { Log("[power] %s%s, battery %d%%", p.ac ? "plugged in" : "on battery", p.saver ? " (Battery Saver)" : "", p.percent); }
static PowerMode Mode(const PowerState& p, const Settings& st, int rule)
{
    if (p.ac) return PowerFull;
    if (p.saver || st.battery == 2) return rule == 1 ? PowerEconomy : PowerOff;
    return (PowerMode)st.battery;   // Settings::battery uses the PowerMode values
}
static const char* ModeName(PowerMode m) { return m == PowerFull ? "full" : m == PowerEconomy ? "economy" : "off"; }
// Battery power in watts, negative while discharging (0 = plugged in or unknown).
static double BatteryWatts()
{
    SYSTEM_BATTERY_STATE b = {};
    if (CallNtPowerInformation(SystemBatteryState, nullptr, 0, &b, sizeof b) != 0 || !b.BatteryPresent || b.AcOnLine) return 0;
    return (double)(LONG)b.Rate / 1000.0;
}
// Refresh rate of the monitor a window is on, from its current display mode (it changes on battery).
static double RefreshHz(HWND h)
{
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof mi;
    DEVMODEW dm = {}; dm.dmSize = sizeof dm;
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return 60;
    return dm.dmDisplayFrequency;
}

// A foreground window worth watching: a real, visible, uncloaked top-level window of another
// process, not the shell, big enough to be content.
static bool Watchable(HWND h)
{
    if (!h || !IsWindowVisible(h) || IsIconic(h)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    for (const wchar_t* shell : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"ConsoleWindowClass" })
        if (!wcscmp(cls, shell)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    RECT r; if (!GetWindowRect(h, &r)) return false;
    return r.right - r.left >= 640 && r.bottom - r.top >= 400;
}

// ---- probe: distinct-content rate of the captured window ----------------------------------------------------
struct Probe
{
    static const int kSlots = 4, kDiv = 8;
    Gpu* g = nullptr; ComputePso gray;
    UINT w = 0, h = 0, tw = 0, th = 0;
    ID3D12Resource* tiny[kSlots] = {}; ID3D12Resource* rb[kSlots] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 rb_bytes = 0;
    UINT64 fence[kSlots] = {}, seq[kSlots] = {}; double t[kSlots] = {}; bool pending[kSlots] = {};
    UINT64 next = 0, read = 0;
    std::vector<uint8_t> prev, cur; bool have_prev = false;
    std::deque<double> distinct;   // cap_ms of frames that differed from the one before
    size_t last_changed = 0;       // thumbnail cells that moved in the last frame read (diagnostics)
    double last_mean = 0;          // its mean luma, 0..255 (diagnostics)

    bool Init(Gpu& gpu, UINT w_, UINT h_)
    {
        g = &gpu; w = w_; h = h_; tw = (w + kDiv - 1) / kDiv; th = (h + kDiv - 1) / kDiv;
        if (!GpuMakeCompute(gpu, g_cs_gray, sizeof g_cs_gray, 1, 1, 6, gray, L"probe_luma")) return false;
        for (auto& t : tiny) if (!(t = GpuMakeTex(gpu, tw, th, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"probe_tiny"))) return false;
        const D3D12_RESOURCE_DESC d = tiny[0]->GetDesc(); gpu.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &rb_bytes);
        for (auto& r : rb) if (!(r = GpuMakeBuffer(gpu, rb_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"probe_rb"))) return false;
        prev.resize((size_t)tw * th); cur.resize(prev.size());
        return true;
    }
    void Release()
    {
        if (g) GpuWaitIdle(*g);
        for (int i = 0; i < kSlots; ++i) { REL(tiny[i]); REL(rb[i]); }
        REL(gray.pso); REL(gray.root);
    }
    // Reads every finished slot, in order; `block` waits for the oldest (slot reuse).
    void Drain(bool block_oldest)
    {
        for (;;)
        {
            int s = -1;
            for (int i = 0; i < kSlots; ++i) if (pending[i] && seq[i] == read) s = i;
            if (s < 0) return;
            if (g->fence->GetCompletedValue() < fence[s]) { if (!block_oldest) return; GpuWait(*g, g->fence, fence[s], 1000); block_oldest = false; }
            // The read range is the buffer's real size: GetCopyableFootprints leaves the LAST row unpadded,
            // so RowPitch * rows runs past the end and Map refuses it (silently black thumbnails, once).
            uint8_t* p = nullptr; const D3D12_RANGE rr = { 0, (SIZE_T)rb_bytes };
            const HRESULT mh = rb[s]->Map(0, &rr, (void**)&p);
            if (FAILED(mh)) { static bool once = false; if (!once) { once = true; Log("[auto] probe readback Map failed 0x%08X", (unsigned)mh); } }
            else
            {
                for (UINT y = 0; y < th; ++y) memcpy(&cur[(size_t)y * tw], p + (size_t)y * fp.Footprint.RowPitch, tw);
                const D3D12_RANGE none = { 0, 0 }; rb[s]->Unmap(0, &none);
                // Distinct: enough thumbnail pixels moved by more than noise. A blinking caret or a
                // clock digit touches a handful of 8x8 cells; motion touches hundreds.
                size_t changed = 0;
                if (have_prev) for (size_t k = 0; k < cur.size(); ++k) changed += (unsigned)abs((int)cur[k] - (int)prev[k]) > 3;
                last_changed = changed;
                { double m = 0; for (uint8_t v : cur) m += v; last_mean = cur.empty() ? 0 : m / cur.size(); }
                if (have_prev && changed >= std::max<size_t>(8, cur.size() / 1000)) distinct.push_back(t[s]);
                prev.swap(cur); have_prev = true;
            }
            pending[s] = false; ++read;
        }
    }
    // One frame, its own small submission. capture != null: the capture's shared texture (COMMON, ready
    // at wf/wv), sampled directly. Otherwise `frame`: a texture already on Gpu::queue (NPSR) - engaged,
    // xe_fg's copy of the same frame, so the probe adds no full-frame read of the capture.
    bool Submit(ID3D12Resource* capture, ID3D12Fence* wf, UINT64 wv, ID3D12Resource* frame, double cap_ms)
    {
        const int s = (int)(next % kSlots);
        if (pending[s]) Drain(true);
        if (pending[s]) return false;
        if (!GpuBegin(*g)) return false;
        ID3D12GraphicsCommandList* cl = g->list;
        ID3D12Resource* src = frame;
        if (capture)
        {
            g->queue->Wait(wf, wv);
            GpuBarrier(cl, capture, COMMON, NPSR);
            src = capture;
        }
        GpuBarrier(cl, tiny[s], NPSR, UAV);
        const UINT c[6] = { w, h, tw, th, kDiv, kDiv };
        const GpuView sv = { src, DXGI_FORMAT_UNKNOWN }, uv = { tiny[s], DXGI_FORMAT_UNKNOWN };
        GpuDispatch(*g, cl, gray, &sv, &uv, c, GpuGroups(tw, 8), GpuGroups(th, 8));
        GpuBarrier(cl, tiny[s], UAV, CSRC);
        GpuCopyToReadback(cl, tiny[s], rb[s], fp.Footprint.Format, fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch);   // fp.Offset is 0
        GpuBarrier(cl, tiny[s], CSRC, NPSR);
        if (capture) GpuBarrier(cl, capture, NPSR, COMMON);
        const UINT64 v = GpuEnd(*g);
        if (!v) return false;
        fence[s] = v; seq[s] = next++; t[s] = cap_ms; pending[s] = true;
        Drain(false);
        return true;
    }
    struct Rate { double fps = 0, cv = 1, span = 0, gap = 1e9; };
    Rate Measure(double now)
    {
        while (!distinct.empty() && distinct.front() < now - kWindowMs) distinct.pop_front();
        Rate r;
        if (!distinct.empty()) r.gap = now - distinct.back();
        if (distinct.size() < 3) return r;
        r.span = distinct.back() - distinct.front();
        r.fps = (distinct.size() - 1) * 1000.0 / std::max(1.0, r.span);
        double sum = 0, sq = 0; const size_t n = distinct.size() - 1;
        for (size_t i = 1; i < distinct.size(); ++i) { const double d = distinct[i] - distinct[i - 1]; sum += d; sq += d * d; }
        const double mean = sum / n; r.cv = mean > 0 ? sqrt(std::max(0.0, sq / n - mean * mean)) / mean : 1;
        return r;
    }
};

// ---- the watched window ---------------------------------------------------------------------------------------
struct Session
{
    HWND hwnd = nullptr; DWORD pid = 0; std::wstring exe;
    Capture* cap = nullptr; Probe probe;
    Overlay* ov = nullptr; XeFg* fg = nullptr;   // non-null while engaged
    UINT w = 0, h = 0;
    double good_since = -1, bad_since = -1, away_since = -1, stats_t = 0, engaged_t = 0;
    int rule = 0;                                // per-app rule, cached (0 automatic, 1 always, 2 never)
    double hz = 60, hz_t = 0;                    // the window's monitor refresh, re-read every second
    PowerMode engaged_mode = PowerFull;          // the power mode frame generation was created for
    Gpu* gpu = nullptr;                          // for the memory reservation
    LONGLONG last_sysrel = 0; bool reset = true;
    double fps_avg = 0, fps_avg_t = 0;           // distinct fps, smoothed over ~2 s (the cap advisor)
    double fps_before = 0;                       // distinct fps when frame generation engaged
    std::wstring advice, advice_next; double advice_next_t = 0;
};

// ---- cap advisor --------------------------------------------------------------------------------------------
// Frame generation paces to the LONGEST gap between the game's frames (the hold), so a game running between
// unit fractions of the refresh - 45 fps at 120 Hz: gaps of 3, 3, 2 refreshes - pays for its longest gap
// anyway and shows uneven motion on top. Capped at refresh/n it has the same longest gap, every gap equal,
// and the difference as spare GPU time (for the game, or for us on a GPU-bound one). The advice: the
// largest refresh/n the game holds with 2% to spare.
struct CapAdvice { bool already = false; int n = 0; double cap = 0, saved = 0, extra_ms = 0; };
static CapAdvice AdviseCap(double fps, double cv, double hz)
{
    CapAdvice a;
    if (fps <= 0 || hz <= 0) return a;
    const int m = std::max(1, (int)lround(hz / fps));
    if (fabs(fps - hz / m) < 0.02 * hz / m && cv < 0.3) { a.already = true; a.n = m; a.cap = hz / m; return a; }
    a.n = std::max(1, (int)ceil(hz / (fps * 0.98)));
    a.cap = hz / a.n;
    a.saved = std::max(0.0, 1.0 - a.cap / fps);
    const int longest = (int)ceil(hz / fps - 1e-6);   // refreshes between frames at worst, uncapped
    a.extra_ms = std::max(0, a.n - longest) * 1000.0 / hz;
    return a;
}

static std::wstring CapAdviceText(const CapAdvice& a, double hz, double slowed)
{
    if (!a.n) return L"";
    wchar_t b[200];
    if (a.already) swprintf_s(b, L"game capped well: %.0f fps = %.0f Hz / %d, even pacing", a.cap, hz, a.n);
    else
    {
        wchar_t lat[40];
        if (a.extra_ms > 0.5) swprintf_s(lat, L"+%.0f ms latency", a.extra_ms); else wcscpy_s(lat, L"same latency");
        swprintf_s(b, L"tip: cap the game at %.0f fps (%.0f/%d) - even pacing, %ls, ~%.0f%% less GPU", a.cap, hz, a.n, lat, a.saved * 100);
    }
    std::wstring s = b;
    if (slowed >= 0.08) { swprintf_s(b, L"GPU-bound: the game lost %.0f%% to frame generation; ", slowed * 100); s = b + s; }
    return s;
}

static void Disengage(Session& s, const char* why)
{
    if (!s.fg) return;
    Log("[auto] release %ls: %s (after %.0f s)", s.exe.c_str(), why, (NowMs() - s.engaged_t) / 1000.0);
    XeFgDestroy(s.fg); s.fg = nullptr; s.ov = nullptr;   // the overlay belongs to the XeFg
    if (s.gpu) GpuReserveCurrentUsage(*s.gpu);   // give the reservation back
}

static void Close(Session& s)
{
    Disengage(s, "window closed or changed");
    s.probe.Release(); s.probe = Probe();
    if (s.cap) { CaptureClose(s.cap); s.cap = nullptr; }
    s.hwnd = nullptr;
}

static bool Open(Session& s, Gpu& g, HWND h, const Settings& st)
{
    s = Session(); s.hwnd = h;
    GetWindowThreadProcessId(h, &s.pid); s.exe = ExeOfPid(s.pid); s.rule = st.Rule(s.exe);
    s.hz = RefreshHz(h); s.hz_t = NowMs();
    s.cap = CaptureOpen(g, h, false, false, false);
    if (!s.cap) return false;
    if (CaptureIsFloat(s.cap)) { Log("[auto] %ls is HDR (FP16) - not supported", s.exe.c_str()); CaptureClose(s.cap); s.cap = nullptr; return false; }
    s.w = CaptureWidth(s.cap); s.h = CaptureHeight(s.cap);
    if (!s.probe.Init(g, s.w, s.h)) { Log("[auto] probe init failed"); s.probe.Release(); CaptureClose(s.cap); s.cap = nullptr; return false; }
    Log("[auto] watching %ls (%ux%u, %.0f Hz)", s.exe.c_str(), s.w, s.h, s.hz);
    return true;
}

static bool Engage(Session& s, Gpu& g, const Settings& st, PowerMode mode)
{
    s.fg = XeFgCreate(g, s.hwnd, s.w, s.h, mode == PowerEconomy ? 3 : 2);   // economy: flow at 1/3 resolution
    if (!s.fg) return false;
    s.gpu = &g;
    Log("[auto] reserved %.0f MB of video memory (residency priority high)", GpuReserveCurrentUsage(g));
    s.ov = XeFgOverlay(s.fg);
    XeFgSetTiming(s.fg, 0.0, st.Upper(s.hz));
    XeFgSetExtrapolate(s.fg, st.extrap);
    XeFgSetVsync(s.fg, st.vsync);
    s.engaged_mode = mode;
    s.engaged_t = s.stats_t = NowMs(); s.reset = true; s.bad_since = -1;
    return true;
}

int RunTrayApp(const std::wstring& dir)
{
    HANDLE only_one = SingleInstance(L"Local\\JustFlowXe.SingleInstance", L"JustFlow XE is already running - right-click its tray icon to quit.", L"JustFlow XE");
    if (!only_one) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool timer_1ms = timeBeginPeriod(1) == TIMERR_NOERROR;
    const std::wstring log_path = dir + L"\\xe_auto.log";
    LogInit(log_path.c_str());
    Settings st; st.path = dir + L"\\justflow_xe.ini"; st.Load(); st.Save();
    PowerState pw = ReadPower(); double pw_t = NowMs();
    Log("[auto] JustFlow XE tray app: automatic %s, content from %d fps up to %.0f%% of the refresh, on battery %s. Settings: %ls",
        st.auto_on ? "on" : "off", st.min_fps, st.lock120 ? 98.0 : 83.0, ModeName((PowerMode)st.battery), st.path.c_str());
    LogPower(pw);
    // Measured cost: battery watts averaged while engaged vs not, reported every minute on battery.
    double watts_sum[2] = {}, watts_n[2] = {}, watts_report_t = NowMs();
    XeTray* tray = XeTrayCreate();
    if (!tray) { Log("[auto] tray icon failed"); return 1; }

    Gpu g; bool gpu_ok = GpuInit(g, -1, 0);
    int gpu_failures = 0; double gpu_retry_at = 0;
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    Session s; HWND rejected = nullptr;   // a window capture refused (or a "never" app): not retried until focus moves on
    std::wstring never_exe;               // the focused app is on the never list: the tray still offers to undo it
    std::wstring status = L"starting"; double status_t = 0;
    bool quit = false;
    while (!quit)
    {
        // ---- tray --------------------------------------------------------------------------------------
        XeTrayEvent ev;
        while (XeTrayPoll(tray, ev))
        {
            switch (ev)
            {
            case XeTrayToggleAuto: st.auto_on = !st.auto_on; st.Save(); Log("[auto] automatic %s", st.auto_on ? "on" : "off"); if (!st.auto_on) Disengage(s, "automatic turned off"); break;
            case XeTrayToggleLock120: st.lock120 = !st.lock120; st.Save(); if (s.fg) XeFgSetTiming(s.fg, 0.0, st.Upper(s.hz)); Log("[auto] lock to display %s (content up to %d fps at %.0f Hz)", st.lock120 ? "on" : "off", st.Upper(s.hz), s.hz); break;
            case XeTrayToggleSync:
                st.vsync = !st.vsync; st.Save(); if (s.fg) XeFgSetVsync(s.fg, st.vsync);
                Log("[auto] sync mode: %s", st.vsync ? "vsync (no tearing)" : "off (immediate, tearing allowed)");
                break;
            case XeTrayToggleExtrap:
                st.extrap = !st.extrap; st.Save(); if (s.fg) XeFgSetExtrapolate(s.fg, st.extrap);
                Log("[auto] %s", st.extrap ? "low latency: extrapolating ahead of the newest frame" : "interpolating between the last two frames");
                break;
            case XeTrayBatteryFull: case XeTrayBatteryEconomy: case XeTrayBatteryOff:
                st.battery = ev - XeTrayBatteryFull; st.Save();   // Full, Economy, Off = 0, 1, 2
                Log("[power] on battery: %s", ModeName((PowerMode)st.battery));
                break;
            case XeTrayAlways: case XeTrayNever: case XeTrayForget:
                if (const std::wstring app = !s.exe.empty() ? s.exe : never_exe; !app.empty())
                {
                    const int r = ev == XeTrayAlways ? 1 : ev == XeTrayNever ? 2 : 0;
                    st.SetRule(app, r); Log("[auto] %ls: %s", app.c_str(), r == 1 ? "always" : r == 2 ? "never" : "automatic");
                    if (app == s.exe) s.rule = r;
                    if (r == 2) Close(s);
                    rejected = nullptr; never_exe.clear();   // re-evaluate the focused window under the new rule
                }
                break;
            case XeTrayOpenLog: ShellExecuteW(nullptr, L"open", log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case XeTrayQuit: quit = true; break;
            default: break;
            }
        }
        if (quit) break;

        // ---- power ---------------------------------------------------------------------------------------
        if (NowMs() - pw_t >= 1000.0)
        {
            const double dt = (NowMs() - pw_t) / 1000.0; pw_t = NowMs();
            const PowerState p = ReadPower();
            if (p.ac != pw.ac || p.saver != pw.saver)
                LogPower(p);
            pw = p;
            const double wt = BatteryWatts();
            if (wt < 0) { const int k = s.fg ? 1 : 0; watts_sum[k] += -wt * dt; watts_n[k] += dt; }
            if (!pw.ac && NowMs() - watts_report_t >= 60000.0)
            {
                watts_report_t = NowMs();
                Log("[power] battery drain, averaged since start: %.1f W engaged (%.0f s), %.1f W not engaged (%.0f s), battery %d%%",
                    watts_n[1] > 0 ? watts_sum[1] / watts_n[1] : 0.0, watts_n[1], watts_n[0] > 0 ? watts_sum[0] / watts_n[0] : 0.0, watts_n[0], pw.percent);
            }
        }

        // ---- GPU health ---------------------------------------------------------------------------------
        if (!gpu_ok)
        {
            if (NowMs() >= gpu_retry_at)
            {
                gpu_ok = GpuInit(g, -1, 0);
                if (!gpu_ok) { gpu_retry_at = NowMs() + 5000; status = L"GPU unavailable - retrying"; }
            }
            if (!gpu_ok) { Sleep(100); goto tray_update; }
        }

        {
            // ---- which window --------------------------------------------------------------------------
            const HWND fgw = GetForegroundWindow();
            DWORD fg_pid = 0; if (fgw) GetWindowThreadProcessId(fgw, &fg_pid);
            const bool ours = fg_pid == GetCurrentProcessId();   // the tray menu takes the foreground briefly
            const double now = NowMs();
            if (s.hwnd && (!IsWindow(s.hwnd) || (s.cap && CaptureLost(s.cap)))) { Log("[auto] %ls: window gone", s.exe.c_str()); Close(s); }
            if (s.hwnd && !ours && fg_pid != s.pid)
            {
                // Focus left the window. To another real window: follow it now. Anywhere else (desktop,
                // taskbar, a console) while engaged: the overlay hid itself (present.cpp) and the session
                // is kept a while, so Alt-Tab back is instant.
                if (!s.fg || Watchable(fgw)) Close(s);
                else if (s.away_since < 0) s.away_since = now;
                else if (now - s.away_since > kAwayMs) Close(s);
            }
            else if (s.hwnd) s.away_since = -1;
            if (fgw != rejected && !ours) { rejected = nullptr; never_exe.clear(); }
            if (!s.hwnd && !ours && fgw && fgw != rejected && Watchable(fgw))
            {
                DWORD pid = 0; GetWindowThreadProcessId(fgw, &pid);
                const std::wstring exe = ExeOfPid(pid);
                if (st.Rule(exe) == 2) { rejected = fgw; never_exe = exe; }
                else if (!Open(s, g, fgw, st)) { Log("[auto] %ls: cannot capture this window", exe.c_str()); rejected = fgw; }
            }
            if (!s.hwnd)
            {
                status = !never_exe.empty() ? L"never for " + never_exe : st.auto_on ? L"idle - no window to watch" : L"automatic off";
                Sleep(50); goto tray_update;
            }

            // ---- one frame -----------------------------------------------------------------------------
            UINT nw, nh;
            if (CaptureSizeChanged(s.cap, nw, nh)) { Log("[auto] %ls resized to %ux%u", s.exe.c_str(), nw, nh); Close(s); goto tray_update; }
            UINT64 fv = 0; LONGLONG sysrel = 0;
            const bool got = CaptureAcquire(s.cap, 20, fv, sysrel);
            if (s.ov) OverlayFollow(s.ov, 300);
            if (got)
            {
                const bool dda = CaptureIsDda(s.cap);
                if (s.last_sysrel && sysrel - s.last_sysrel > (dda ? qpf.QuadPart / 4 : 2500000)) s.reset = true;
                s.last_sysrel = sysrel;
                const LONGLONG cap_qpc = dda ? sysrel : sysrel / 10000000 * qpf.QuadPart + (sysrel % 10000000) * qpf.QuadPart / 10000000;
                const double cap_ms = QpcToMs(cap_qpc);
                bool ok = true;
                if (s.fg && s.away_since < 0)
                {
                    ok = XeFgSubmit(s.fg, CaptureTexture(s.cap), CaptureFence(s.cap), fv, cap_ms, s.reset); s.reset = false;
                    if (ok) ok = s.probe.Submit(nullptr, nullptr, 0, XeFgLastFrame(s.fg), cap_ms);   // xe_fg's copy: no second one
                }
                else ok = s.probe.Submit(CaptureTexture(s.cap), CaptureFence(s.cap), fv, nullptr, cap_ms);
                if (s.fg && s.away_since >= 0) s.reset = true;   // frames skipped while away: no pair across the gap
                if (!ok || (s.fg && XeFgFailed(s.fg)) || g.failed)
                {
                    GpuLogDeviceRemoved(g, "frame");
                    Close(s); GpuShutdown(g); g = Gpu(); gpu_ok = false;
                    gpu_retry_at = NowMs() + 1000 * (1 << std::min(++gpu_failures, 5));
                    Log("[auto] GPU failure %d - everything released, retrying in %.0f s", gpu_failures, (gpu_retry_at - NowMs()) / 1000.0);
                    goto tray_update;
                }
            }
            else s.probe.Drain(false);

            // ---- decide ------------------------------------------------------------------------------
            const Probe::Rate r = s.probe.Measure(now);
            if (now - s.hz_t >= 1000.0)
            {
                s.hz_t = now;
                const double hz = RefreshHz(s.hwnd);
                if (hz != s.hz)
                {
                    Log("[auto] %ls: display now %.0f Hz (was %.0f)", s.exe.c_str(), hz, s.hz);
                    s.hz = hz;
                    Disengage(s, "display refresh changed");   // the overlay's pacing was built for the old one
                }
            }
            const int rule = s.rule;
            const PowerMode mode = Mode(pw, st, rule);
            if (s.fg && (mode == PowerOff || mode != s.engaged_mode))
                Disengage(s, mode == PowerOff ? "power: off on battery" : "power mode changed");
            const int lo = mode == PowerEconomy ? std::max(st.min_fps, 30) : st.min_fps, hi = st.Upper(s.hz);
            const bool steady = rule == 1 || (r.cv < 0.6 && r.span >= kWindowMs * 0.8);
            const bool in_range = r.fps >= lo && r.fps <= hi && r.gap < 100;
            // ---- cap advisor: on a smoothed rate, a new suggestion only after it held for 3 s
            {
                const double dt = s.fps_avg_t > 0 ? now - s.fps_avg_t : 0; s.fps_avg_t = now;
                const bool live = r.fps > 0 && r.gap < 250 && r.span >= kWindowMs * 0.8;
                s.fps_avg = !live ? 0 : s.fps_avg <= 0 ? r.fps : s.fps_avg + (r.fps - s.fps_avg) * (1.0 - exp(-dt / 2000.0));
                std::wstring want;
                if (live && s.fps_avg >= lo && s.fps_avg <= hi)
                {
                    const double slowed = s.fg && s.fps_before > 0 ? 1.0 - s.fps_avg / s.fps_before : 0;
                    want = CapAdviceText(AdviseCap(s.fps_avg, r.cv, s.hz), s.hz, slowed);
                }
                if (want.empty() || want == s.advice) { s.advice_next.clear(); if (want.empty()) s.advice.clear(); }
                else if (want != s.advice_next) { s.advice_next = want; s.advice_next_t = now; }
                else if (now - s.advice_next_t >= 3000.0)
                {
                    s.advice = want; s.advice_next.clear();
                    Log("[cap] %ls: %ls (content %.1f fps, cadence cv %.2f, %.0f Hz)", s.exe.c_str(), s.advice.c_str(), s.fps_avg, r.cv, s.hz);
                }
            }
            wchar_t line[160];
            if (!s.fg)
            {
                s.good_since = (st.auto_on && rule != 2 && mode != PowerOff && in_range && steady) ? (s.good_since < 0 ? now : s.good_since) : -1;
                if (s.good_since >= 0 && now - s.good_since >= (rule == 1 ? 300.0 : kEngageMs))
                {
                    Log("[auto] engage %ls: %.1f distinct fps (cadence cv %.2f) at %.0f Hz, power %s%s", s.exe.c_str(), r.fps, r.cv, s.hz, ModeName(mode), rule == 1 ? " [always]" : "");
                    if (!Engage(s, g, st, mode)) { Log("[auto] engage failed"); s.good_since = -1; }
                    else s.fps_before = s.fps_avg > 0 ? s.fps_avg : r.fps;
                }
                if (mode == PowerOff) swprintf_s(line, L"watching %ls - off on battery", s.exe.c_str());
                else if (r.gap > 1000) swprintf_s(line, L"watching %ls - static", s.exe.c_str());
                else if (r.fps > hi) swprintf_s(line, L"watching %ls - %.0f fps (fast enough)", s.exe.c_str(), r.fps);
                else if (r.fps < lo) swprintf_s(line, L"watching %ls - %.0f fps (too slow)", s.exe.c_str(), r.fps);
                else swprintf_s(line, L"watching %ls - %.0f fps%ls", s.exe.c_str(), r.fps, steady ? L"" : L" (uneven)");
                // why it is (not) engaging, every 5 s: the numbers the rule above looks at
                if (now - s.stats_t >= 5000.0)
                {
                    s.stats_t = now;
                    Log("[auto] %ls: %.1f distinct fps (range %d-%d), cadence cv %.2f, span %.0f ms, last change %.0f ms ago%s | probe %llu in, %llu read, %zu cells moved last, mean luma %.1f",
                        s.exe.c_str(), r.fps, lo, hi, r.cv, r.span, std::min(r.gap, 99999.0), mode == PowerOff ? ", power off" : "",
                        (unsigned long long)s.probe.next, (unsigned long long)s.probe.read, s.probe.last_changed, s.probe.last_mean);
                }
            }
            else
            {
                // Release on sustained trouble only: our own GPU load can pull a GPU-bound game's rate
                // down a little, and a stall or a loading screen should not flap the overlay.
                const bool bad = !(r.fps >= lo - 5 && r.fps <= hi + 5) || r.gap > 250;
                s.bad_since = bad ? (s.bad_since < 0 ? now : s.bad_since) : -1;
                if (rule != 1 && s.bad_since >= 0 && now - s.bad_since >= kReleaseMs)
                    Disengage(s, r.gap > 250 ? "content static" : r.fps > hi ? "content faster than the range" : "content slower than the range");
                else swprintf_s(line, L"generating %ls - %.0f -> %.0f fps%ls%ls", s.exe.c_str(), r.fps, s.hz, st.extrap ? L" (low latency)" : L"", s.engaged_mode == PowerEconomy ? L" (economy)" : L"");
                if (!s.fg) swprintf_s(line, L"watching %ls", s.exe.c_str());   // released just above
            }
            status = line;

            if (s.fg && now - s.stats_t >= 5000.0)
            {
                XeFgStatsOut o; XeFgStats(s.fg, o);
                const double dt = (now - s.stats_t) / 1000.0; s.stats_t = now;
                auto q = [&](double p) { return Pct(o.spacing_ms, p); };
                Log("[stats] %ls: content %.1f fps  in %.1f  out %.1f fps (gen %u, ext %u, held %u)%s | flow %.2f ms  interp %.2f ms | hold %.1f ms | spacing p5 %.2f  med %.2f  p95 %.2f ms",
                    s.exe.c_str(), r.fps, o.in / dt, o.presented / dt, o.generated, o.extrapolated, o.held, o.passthrough ? " PASSTHROUGH" : "",
                    o.flow_ms, o.interp_ms, o.hold_ms, q(0.05), q(0.5), q(0.95));
            }
        }

    tray_update:
        if (NowMs() - status_t > 250)
        {
            status_t = NowMs();
            XeTrayState ts; ts.auto_on = st.auto_on; ts.lock120 = st.lock120; ts.extrap = st.extrap; ts.vsync = st.vsync; ts.status = status; ts.battery = st.battery;
            ts.advice = s.hwnd ? s.advice : L"";
            ts.icon = s.fg ? XeIconGenerating : (!st.auto_on || (!pw.ac && (pw.saver || st.battery == 2))) ? XeIconOff : XeIconWatching;
            if (!pw.ac) ts.status += pw.saver ? L" [battery saver]" : L" [battery]";
            ts.app = !s.exe.empty() ? s.exe : never_exe; ts.app_rule = !s.exe.empty() ? s.rule : ts.app.empty() ? 0 : 2;
            XeTraySet(tray, ts);
        }
    }
    Close(s);
    if (gpu_ok) GpuShutdown(g);
    XeTrayDestroy(tray);
    if (timer_1ms) timeEndPeriod(1);
    Log("[auto] quit");
    CloseHandle(only_one);
    return 0;
}
