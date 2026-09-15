// Click-through overlay window on its own thread + flip-model swapchain on the Gpu queue.
#include "present.h"
#include "log.h"
#include <atomic>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static const int kMaxHot = 16;
static const UINT WM_SET_HOTKEYS = WM_APP + 1;   // wp = const HotkeyDef*, lp = count; returns the number of RegisterHotKey failures

// 3 buffers, 2 frames in flight: the compositor's waitable paces presents against DWM instead of
// WaitForVBlank, which drifts and lands frames mid-scanline. Frames in flight must exceed 1 or the
// present RATE is capped at one per retire, which frame generation cannot live with (it presents
// multiplier x the real rate). One buffer stays free for the copy.
static const int kBuffers = 3;

struct Overlay
{
    Gpu*   g = nullptr;
    HWND   target = nullptr;
    HWND   hwnd = nullptr;
    HANDLE thread = nullptr, ready = nullptr;
    DWORD  tid = 0;
    volatile LONG state = 0;            // 0 starting, 1 up, -1 failed
    IDXGISwapChain3* swap = nullptr;
    ID3D12Resource*  bb[kBuffers] = {};
    UINT   w = 0, h = 0, flags = 0, present_flags = 0;
    bool   revealed = false, shown = false, exclude = false, direct = false;
    bool   layered = true;              // WS_EX_LAYERED|WS_EX_TRANSPARENT added after the swapchain (composed, or direct after a FAILed self-test)
    RECT   mon = {};                    // direct: the monitor rect the window covers
    HWND   probe_hit = nullptr;         // cross-thread WindowFromPoint result of the self-test
    HotkeyDef keys[kMaxHot] = {};
    int    nkeys = 0;
    volatile LONG hot[kMaxHot] = {};
    int    follow_calls = 0;
    RECT   last = {};
    LONGLONG present_qpc = 0, scanout_qpc = 0;
    // present queue: swapchain + backbuffer copies, decoupled from the pipeline queue
    ID3D12CommandQueue* pq = nullptr;
    ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr; HANDLE event = nullptr; UINT64 fence_value = 0;
    // Where a present's wall time goes (presenter thread writes, stats reader drains). us, summed.
    std::atomic<UINT64> pres_n{ 0 }, pres_prev_us{ 0 }, pres_call_us{ 0 }, pres_total_us{ 0 };
    IDXGIOutput* output = nullptr; bool output_looked_up = false;
    HANDLE waitable = nullptr;          // DWM releases a back buffer a vblank before the last one scans out
    double vblank_ms = 1000.0 / 60.0;
};

static bool WaitPq(Overlay* o, UINT64 v, DWORD ms)
{
    if (!v || o->fence->GetCompletedValue() >= v) return true;
    ResetEvent(o->event);
    if (FAILED(o->fence->SetEventOnCompletion(v, o->event))) return false;
    return WaitForSingleObject(o->event, ms) == WAIT_OBJECT_0 && o->fence->GetCompletedValue() >= v;
}

static void Drain(Overlay* o)
{
    if (o->g) GpuWaitIdle(*o->g);
    if (o->pq && o->fence && SUCCEEDED(o->pq->Signal(o->fence, ++o->fence_value))) WaitPq(o, o->fence_value, 5000);
}

// window thread only (RegisterHotKey binds to the calling thread)
static int RegisterKeys(Overlay* o)
{
    int failed = 0;
    for (int i = 0; i < o->nkeys; ++i)
    {
        const HotkeyDef& k = o->keys[i];
        if (!RegisterHotKey(o->hwnd, k.id, k.mods | MOD_NOREPEAT, k.vk)) { ++failed; Log("[present] RegisterHotKey id %d mods 0x%X vk 0x%X failed, err=%lu", k.id, k.mods, k.vk, GetLastError()); }
    }
    return failed;
}
static void UnregisterKeys(Overlay* o) { for (int i = 0; i < o->nkeys; ++i) UnregisterHotKey(o->hwnd, o->keys[i].id); }

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    Overlay* o = (Overlay*)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m)
    {
    case WM_NCCREATE:      SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); break;
    case WM_NCHITTEST:     return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:    return 1;
    case WM_HOTKEY:        if (o && wp < (WPARAM)kMaxHot) InterlockedExchange(&o->hot[wp], 1); return 0;
    case WM_SET_HOTKEYS:
        if (!o || o->hwnd != w) return 0;
        UnregisterKeys(o);
        o->nkeys = (int)lp < kMaxHot ? (int)lp : kMaxHot;
        for (int i = 0; i < o->nkeys; ++i) o->keys[i] = ((const HotkeyDef*)wp)[i];
        return RegisterKeys(o);
    case WM_CLOSE:         DestroyWindow(w); return 0;
    case WM_DESTROY:
        if (o && o->hwnd == w) UnregisterKeys(o);
        if (!o || o->hwnd == w) PostQuitMessage(0);   // not for the direct-mode window torn down by the fallback
        return 0;
    default: break;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// direct: monitor-sized (target's monitor, else the primary) with no redirection surface.
static HWND MakeWindow(Overlay* o, HINSTANCE hi, bool direct)
{
    int x = 0, y = 0, w = (int)o->w, h = (int)o->h;
    if (direct) { x = o->mon.left; y = o->mon.top; w = o->mon.right - o->mon.left; h = o->mon.bottom - o->mon.top; }
    return CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | (direct ? WS_EX_NOREDIRECTIONBITMAP : 0),
                           L"JustFlowPresent", L"JustFlowPresent", WS_POPUP, x, y, w, h, nullptr, nullptr, hi, o);
}

// Click-through self-test for a window without WS_EX_LAYERED: shown for the duration of one
// WindowFromPoint at its centre (nothing is painted - no redirection surface, no swapchain yet).
// The probe runs on ANOTHER thread while this one pumps: from the owner thread WindowFromPoint
// honours our HTTRANSPARENT and always misses (measured), from a foreign thread it reports what
// real mouse input sees (a non-layered HTTRANSPARENT window is hit and the click is dropped).
static DWORD WINAPI ProbeThread(LPVOID p)
{
    Overlay* o = (Overlay*)p;
    o->probe_hit = WindowFromPoint(POINT{ (o->mon.left + o->mon.right) / 2, (o->mon.top + o->mon.bottom) / 2 });
    return 0;
}
static bool ClickThroughOk(Overlay* o)
{
    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    const bool visible = IsWindowVisible(o->hwnd) != FALSE;   // a hidden window is never hit: no false PASS
    o->probe_hit = o->hwnd;                                    // a probe that never answers counts as FAIL
    HANDLE t = CreateThread(nullptr, 0, ProbeThread, o, 0, nullptr);
    if (t)
    {
        while (MsgWaitForMultipleObjects(1, &t, FALSE, 3000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)   // dispatch the probe's WM_NCHITTEST
        { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }
        CloseHandle(t);
    }
    ShowWindow(o->hwnd, SW_HIDE);
    const bool ok = visible && o->probe_hit != o->hwnd;
    Log("[present] direct-mode click-through self-test %s (cross-thread WindowFromPoint(centre) -> %p, overlay %p%s)", ok ? "PASS" : "FAIL", (void*)o->probe_hit, (void*)o->hwnd, visible ? "" : ", not visible");
    return ok;
}

static DWORD WINAPI WindowThread(LPVOID p)
{
    Overlay* o = (Overlay*)p;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"JustFlowPresent";
    RegisterClassExW(&wc);   // duplicate registration just fails
    if (o->direct)
    {
        MONITORINFO mi = { sizeof mi };
        const HMONITOR mon = o->target ? MonitorFromWindow(o->target, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi); o->mon = mi.rcMonitor;
        const UINT mw = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left), mh = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (mw != o->w || mh != o->h) { Log("[present] direct mode needs the capture to cover the monitor (%ux%u vs monitor %ux%u) - composed", o->w, o->h, mw, mh); o->direct = false; }
    }
    o->hwnd = MakeWindow(o, wc.hInstance, o->direct);
    if (o->hwnd && o->direct)
    {
        // WDA_EXCLUDEFROMCAPTURE on a NOREDIRECTIONBITMAP window: verified here, not assumed. Refused
        // while exclusion is required (DDA would capture the overlay) -> composed window instead.
        if (o->exclude && !SetWindowDisplayAffinity(o->hwnd, WDA_EXCLUDEFROMCAPTURE))
        {
            Log("[present] WDA_EXCLUDEFROMCAPTURE refused on the direct-mode window, err=%lu - falling back to composed", GetLastError());
            HWND dead = o->hwnd; o->hwnd = nullptr; DestroyWindow(dead);
            o->direct = false;
            o->hwnd = MakeWindow(o, wc.hInstance, false);
        }
        else if (o->exclude) Log("[present] WDA_EXCLUDEFROMCAPTURE accepted on the direct-mode window");
    }
    if (o->hwnd && o->direct)
    {
        // Self-test FAIL: keep the monitor-sized window without a redirection bitmap and add the
        // layered style after the swapchain like composed does (direct_layered) - click-through is
        // then guaranteed; whether DWM still grants independent flip is for PresentMon to say.
        o->layered = !ClickThroughOk(o);
        if (o->layered) Log("[present] direct mode continues as direct_layered (monitor-sized, no redirection bitmap, layered click-through)");
    }
    if (!o->hwnd)
    {
        Log("[present] CreateWindowEx failed, err=%lu", GetLastError());
        InterlockedExchange(&o->state, -1); SetEvent(o->ready);
        return 0;
    }
    if (!o->direct && o->exclude && !SetWindowDisplayAffinity(o->hwnd, WDA_EXCLUDEFROMCAPTURE)) Log("[present] SetWindowDisplayAffinity failed, err=%lu", GetLastError());
    RegisterKeys(o);
    // hidden until the first Present
    SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    InterlockedExchange(&o->state, 1); SetEvent(o->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

static bool GetBuffers(Overlay* o)
{
    for (int i = 0; i < kBuffers; ++i)
    {
        if (FAILED(o->swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&o->bb[i]))) { Log("[present] GetBuffer %d failed", i); return false; }
        wchar_t name[16]; swprintf_s(name, L"backbuffer%d", i);
        o->bb[i]->SetName(name);
    }
    return true;
}

static void ReleaseBuffers(Overlay* o)
{
    for (int i = 0; i < kBuffers; ++i) if (o->bb[i]) { o->bb[i]->Release(); o->bb[i] = nullptr; }
}

bool OverlayIsDirect(const Overlay* o) { return o->direct; }

Overlay* OverlayCreate(Gpu& g, HWND target, UINT w, UINT h, const HotkeyDef* keys, int nkeys, bool exclude_from_capture, int mode)
{
    Overlay* o = new Overlay();
    o->g = &g; o->target = target; o->w = w; o->h = h; o->exclude = exclude_from_capture;
    o->direct = mode != 0;
    o->nkeys = nkeys < kMaxHot ? nkeys : kMaxHot;
    for (int i = 0; i < o->nkeys; ++i) o->keys[i] = keys[i];
    o->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    o->thread = CreateThread(nullptr, 0, WindowThread, o, 0, &o->tid);
    if (!o->thread || WaitForSingleObject(o->ready, 3000) != WAIT_OBJECT_0 || o->state != 1)
    {
        Log("[present] window did not come up");
        OverlayDestroy(o);
        return nullptr;
    }

    BOOL tearing = FALSE;
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(g.factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5)))
    {
        if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE;
        f5->Release();
    }
    o->flags = (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    o->present_flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    o->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&o->pq)) ||
        FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&o->alloc)) ||
        FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, o->alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&o->list)) ||
        FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&o->fence)) || !o->event)
    { Log("[present] present queue objects failed"); OverlayDestroy(o); return nullptr; }
    o->list->Close();
    o->pq->SetName(L"present_queue");

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; sd.Flags = o->flags;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = g.factory->CreateSwapChainForHwnd(o->pq, o->hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr) || !sc1) { Log("[present] CreateSwapChainForHwnd failed 0x%08X", hr); OverlayDestroy(o); return nullptr; }
    g.factory->MakeWindowAssociation(o->hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&o->swap);
    sc1->Release();
    if (FAILED(hr) || !o->swap) { Log("[present] IDXGISwapChain3 unavailable 0x%08X", hr); OverlayDestroy(o); return nullptr; }
    // Frames allowed in flight. 1 paces hard against the compositor but caps the present RATE at one
    // frame per retire, which is fatal for frame generation: it has to present multiplier x the real
    // rate. 2 keeps the back-pressure and lets a generated frame be in flight while the real one
    // retires. kBuffers is 3, so one buffer stays free for the copy.
    o->swap->SetMaximumFrameLatency(kBuffers - 1);
    o->waitable = o->swap->GetFrameLatencyWaitableObject();
    if (!o->waitable) Log("[present] no frame-latency waitable - pacing falls back to WaitForVBlank");
    if (!GetBuffers(o)) { OverlayDestroy(o); return nullptr; }

    if (o->layered)
    {
        // Layered + transparent AFTER the swapchain exists: flip model refuses a layered window at create.
        const LONG ex = GetWindowLongW(o->hwnd, GWL_EXSTYLE);
        SetWindowLongW(o->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
        if (!SetLayeredWindowAttributes(o->hwnd, 0, 255, LWA_ALPHA)) Log("[present] SetLayeredWindowAttributes failed, err=%lu", GetLastError());
        SetWindowPos(o->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);   // never activate: the game drops to its background fps cap when it loses focus
    }
    if (target) OverlayFollow(o, 0);
    const char* mode_name = !o->direct ? "composed" : o->layered ? "direct_layered" : "direct";
    Log("[present] overlay %ux%u ready (flip-discard%s, %s%s, own present queue)", w, h, tearing ? ", tearing" : "", mode_name, exclude_from_capture ? ", excluded from capture" : "");
    Log("[stats] present_mode=%s", mode_name);
    return o;
}

void OverlayDestroy(Overlay* o)
{
    if (!o) return;
    Drain(o);
    ReleaseBuffers(o);
    if (o->waitable) { CloseHandle(o->waitable); o->waitable = nullptr; }
    if (o->swap) { o->swap->Release(); o->swap = nullptr; }
    if (o->output) { o->output->Release(); o->output = nullptr; }
    if (o->list) { o->list->Release(); o->list = nullptr; }
    if (o->alloc) { o->alloc->Release(); o->alloc = nullptr; }
    if (o->fence) { o->fence->Release(); o->fence = nullptr; }
    if (o->pq) { o->pq->Release(); o->pq = nullptr; }
    if (o->event) { CloseHandle(o->event); o->event = nullptr; }
    if (o->hwnd) PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
    if (o->thread)
    {
        if (WaitForSingleObject(o->thread, 2000) != WAIT_OBJECT_0)
        {
            Log("[present] window thread did not exit on WM_CLOSE");
            PostThreadMessageW(o->tid, WM_QUIT, 0, 0);
            WaitForSingleObject(o->thread, 1000);
        }
        CloseHandle(o->thread);
    }
    if (o->ready) CloseHandle(o->ready);
    delete o;
}

HWND OverlayHwnd(Overlay* o) { return o->hwnd; }

static double UsSince(LARGE_INTEGER a)
{
    LARGE_INTEGER b, f; QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
}

bool OverlayPresent(Overlay* o, ID3D12Resource* src, ID3D12Fence* after, UINT64 after_value)
{
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    // ponytail: one allocator, so wait for the previous copy before reuse (it retired ~a frame ago).
    if (!WaitPq(o, o->fence_value, 2000)) { Log("[present] previous copy did not retire"); return false; }
    o->pres_prev_us += (UINT64)UsSince(t0);
    if (FAILED(o->alloc->Reset()) || FAILED(o->list->Reset(o->alloc, nullptr))) { Log("[present] list reset failed"); return false; }
    ID3D12Resource* bb = o->bb[o->swap->GetCurrentBackBufferIndex()];
    GpuBarrier(o->list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    o->list->CopyResource(bb, src);
    GpuBarrier(o->list, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(o->list->Close())) { Log("[present] list close failed"); return false; }
    if (after) o->pq->Wait(after, after_value);
    ID3D12CommandList* ls[] = { o->list };
    o->pq->ExecuteCommandLists(1, ls);
    o->pq->Signal(o->fence, ++o->fence_value);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const HRESULT hr = o->swap->Present(0, o->present_flags);
    o->pres_call_us += (UINT64)UsSince(q);
    if (FAILED(hr)) { Log("[present] Present failed 0x%08X", (unsigned)hr); return false; }
    o->present_qpc = q.QuadPart;
    if (!o->revealed)
    {
        ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        o->revealed = o->shown = true;
        Log("[present] window revealed on the first Present");
    }
    DXGI_FRAME_STATISTICS fs = {};
    o->scanout_qpc = SUCCEEDED(o->swap->GetFrameStatistics(&fs)) ? fs.SyncQPCTime.QuadPart : 0;
    o->pres_total_us += (UINT64)UsSince(t0);
    ++o->pres_n;
    return true;
}

void OverlayPresentStats(Overlay* o, double& prev_ms, double& call_ms, double& total_ms)
{
    const UINT64 n = o->pres_n.exchange(0);
    const double d = n ? 1000.0 * (double)n : 0.0;   // us -> ms, divided by n
    prev_ms  = n ? (double)o->pres_prev_us.exchange(0) / d : -1.0;
    call_ms  = n ? (double)o->pres_call_us.exchange(0) / d : -1.0;
    total_ms = n ? (double)o->pres_total_us.exchange(0) / d : -1.0;
    if (!n) { o->pres_prev_us = 0; o->pres_call_us = 0; o->pres_total_us = 0; }
}

void OverlayTimes(Overlay* o, LONGLONG& present_qpc, LONGLONG& scanout_qpc)
{
    present_qpc = o->present_qpc; scanout_qpc = o->scanout_qpc;
}

void OverlayGuard(Overlay* o, ID3D12CommandQueue* q) { if (o->fence_value) q->Wait(o->fence, o->fence_value); }
void OverlayDrain(Overlay* o) { Drain(o); }

// ponytail: the output is resolved once (first call); a window dragged to another monitor keeps
// pacing on the old one until the overlay is recreated (resize / capture reopen).
static IDXGIOutput* Output(Overlay* o)
{
    if (o->output_looked_up) return o->output;
    o->output_looked_up = true;
    const HMONITOR mon = MonitorFromWindow(o->hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIOutput* out = nullptr;
    for (UINT i = 0; o->g->adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_OUTPUT_DESC d = {};
        if (SUCCEEDED(out->GetDesc(&d)) && d.Monitor == mon)
        {
            DEVMODEW dm = {}; dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) o->vblank_ms = 1000.0 / dm.dmDisplayFrequency;
            Log("[present] pacing on %ls (%lu Hz, vblank %.2f ms)", d.DeviceName, dm.dmDisplayFrequency, o->vblank_ms);
            o->output = out;
            return out;
        }
        out->Release(); out = nullptr;
    }
    Log("[present] no DXGI output on this adapter matches the overlay's monitor - timer pacing");
    return nullptr;
}

// Pace on the compositor's own signal when we have it; WaitForVBlank is the pre-8.1 fallback.
bool OverlayWaitVBlank(Overlay* o)
{
    if (o->waitable) return WaitForSingleObjectEx(o->waitable, 100, TRUE) == WAIT_OBJECT_0;
    IDXGIOutput* out = Output(o);
    return out && SUCCEEDED(out->WaitForVBlank());
}
double OverlayVBlankMs(Overlay* o) { Output(o); return o->vblank_ms; }

void OverlayFollow(Overlay* o, int reassert_every)
{
    if (!o->target || !IsWindow(o->target)) return;
    if (IsIconic(o->target))
    {
        if (o->shown) { ShowWindow(o->hwnd, SW_HIDE); o->shown = false; Log("[present] target minimised - overlay hidden"); }
        return;
    }
    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(o->target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(o->target, &r)) return;
    if (!o->shown && o->revealed) { ShowWindow(o->hwnd, SW_SHOWNOACTIVATE); o->shown = true; Log("[present] target restored - overlay shown"); }
    const bool moved = r.left != o->last.left || r.top != o->last.top || r.right != o->last.right || r.bottom != o->last.bottom;
    const bool reassert = reassert_every > 0 && (++o->follow_calls % reassert_every) == 0;
    if (o->direct) { if (reassert) SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); return; }   // stays monitor-sized
    if (!moved && !reassert) return;
    o->last = r;
    // Follow the size only when the buffer matches it; otherwise keep the buffer size clamped
    // inside the target rect so stale content is never stretched during a drag-resize.
    int left = r.left, top = r.top;
    UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
    if (w != o->w || h != o->h)
    {
        w = o->w; h = o->h;
        if (left + (int)w > r.right) left = r.right - (int)w;
        if (top + (int)h > r.bottom) top = r.bottom - (int)h;
        if (left < r.left) left = r.left;
        if (top < r.top) top = r.top;
    }
    SetWindowPos(o->hwnd, HWND_TOPMOST, left, top, (int)w, (int)h, SWP_NOACTIVATE);
}

bool OverlayResize(Overlay* o, UINT w, UINT h)
{
    Drain(o);
    ReleaseBuffers(o);
    const HRESULT hr = o->swap->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, o->flags);
    if (FAILED(hr)) { Log("[present] ResizeBuffers %ux%u failed 0x%08X", w, h, (unsigned)hr); return false; }
    o->w = w; o->h = h; o->last = RECT{};
    if (!GetBuffers(o)) return false;
    Log("[present] overlay resized to %ux%u", w, h);
    if (o->direct && ((int)w != o->mon.right - o->mon.left || (int)h != o->mon.bottom - o->mon.top))
        Log("[present] direct mode: buffer no longer matches the monitor - DWM scales it, independent flip unlikely");
    return true;
}

bool OverlayHotkey(Overlay* o, int id)
{
    if (!o || id < 0 || id >= kMaxHot) return false;
    return InterlockedExchange(&o->hot[id], 0) != 0;
}

bool OverlaySetHotkeys(Overlay* o, const HotkeyDef* keys, int nkeys)
{
    if (!o || !o->hwnd) return false;
    return SendMessageW(o->hwnd, WM_SET_HOTKEYS, (WPARAM)keys, (LPARAM)nkeys) == 0;   // the window thread is pumping: synchronous
}
