// Click-through overlay window on its own thread + flip-model swapchain on the Gpu queue.
#include "present.h"
#include "log.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static const int kMaxHot = 16;

struct Overlay
{
    Gpu*   g = nullptr;
    HWND   target = nullptr;
    HWND   hwnd = nullptr;
    HANDLE thread = nullptr, ready = nullptr;
    DWORD  tid = 0;
    volatile LONG state = 0;            // 0 starting, 1 up, -1 failed
    IDXGISwapChain3* swap = nullptr;
    ID3D12Resource*  bb[2] = {};
    UINT   w = 0, h = 0, flags = 0, present_flags = 0;
    bool   revealed = false, shown = false, exclude = false;
    HotkeyDef keys[kMaxHot] = {};
    int    nkeys = 0;
    volatile LONG hot[kMaxHot] = {};
    int    follow_calls = 0;
    RECT   last = {};
    LONGLONG present_qpc = 0, scanout_qpc = 0;
};

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
    case WM_CLOSE:         DestroyWindow(w); return 0;
    case WM_DESTROY:
        if (o) for (int i = 0; i < o->nkeys; ++i) UnregisterHotKey(w, o->keys[i].id);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI WindowThread(LPVOID p)
{
    Overlay* o = (Overlay*)p;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrfPresent";
    RegisterClassExW(&wc);   // duplicate registration just fails
    o->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, wc.lpszClassName, L"NrfPresent",
                              WS_POPUP, 0, 0, (int)o->w, (int)o->h, nullptr, nullptr, wc.hInstance, o);
    if (!o->hwnd)
    {
        Log("[present] CreateWindowEx failed, err=%lu", GetLastError());
        InterlockedExchange(&o->state, -1); SetEvent(o->ready);
        return 0;
    }
    if (o->exclude && !SetWindowDisplayAffinity(o->hwnd, WDA_EXCLUDEFROMCAPTURE)) Log("[present] SetWindowDisplayAffinity failed, err=%lu", GetLastError());
    for (int i = 0; i < o->nkeys; ++i)
    {
        const HotkeyDef& k = o->keys[i];
        if (!RegisterHotKey(o->hwnd, k.id, k.mods | MOD_NOREPEAT, k.vk)) Log("[present] RegisterHotKey id %d mods 0x%X vk 0x%X failed, err=%lu", k.id, k.mods, k.vk, GetLastError());
    }
    // hidden until the first Present
    SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    InterlockedExchange(&o->state, 1); SetEvent(o->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

static bool GetBuffers(Overlay* o)
{
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(o->swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&o->bb[i]))) { Log("[present] GetBuffer %d failed", i); return false; }
        o->bb[i]->SetName(i ? L"backbuffer1" : L"backbuffer0");
    }
    return true;
}

static void ReleaseBuffers(Overlay* o)
{
    for (int i = 0; i < 2; ++i) if (o->bb[i]) { o->bb[i]->Release(); o->bb[i] = nullptr; }
}

Overlay* OverlayCreate(Gpu& g, HWND target, UINT w, UINT h, const HotkeyDef* keys, int nkeys, bool exclude_from_capture)
{
    Overlay* o = new Overlay();
    o->g = &g; o->target = target; o->w = w; o->h = h; o->exclude = exclude_from_capture;
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
    o->flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    o->present_flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; sd.Flags = o->flags;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = g.factory->CreateSwapChainForHwnd(g.queue, o->hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr) || !sc1) { Log("[present] CreateSwapChainForHwnd failed 0x%08X", hr); OverlayDestroy(o); return nullptr; }
    g.factory->MakeWindowAssociation(o->hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&o->swap);
    sc1->Release();
    if (FAILED(hr) || !o->swap) { Log("[present] IDXGISwapChain3 unavailable 0x%08X", hr); OverlayDestroy(o); return nullptr; }
    if (!GetBuffers(o)) { OverlayDestroy(o); return nullptr; }

    // Layered + transparent AFTER the swapchain exists: flip model refuses a layered window at create.
    const LONG ex = GetWindowLongW(o->hwnd, GWL_EXSTYLE);
    SetWindowLongW(o->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    if (!SetLayeredWindowAttributes(o->hwnd, 0, 255, LWA_ALPHA)) Log("[present] SetLayeredWindowAttributes failed, err=%lu", GetLastError());
    SetWindowPos(o->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (target) OverlayFollow(o, 0);
    Log("[present] overlay %ux%u ready (flip-discard%s, layered click-through%s)", w, h, tearing ? ", tearing" : "",
        exclude_from_capture ? ", excluded from capture" : "");
    return o;
}

void OverlayDestroy(Overlay* o)
{
    if (!o) return;
    if (o->swap && o->g) GpuWaitIdle(*o->g);
    ReleaseBuffers(o);
    if (o->swap) { o->swap->Release(); o->swap = nullptr; }
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

ID3D12Resource* OverlayBackbuffer(Overlay* o) { return o->bb[o->swap->GetCurrentBackBufferIndex()]; }
HWND OverlayHwnd(Overlay* o) { return o->hwnd; }

bool OverlayPresent(Overlay* o)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const HRESULT hr = o->swap->Present(0, o->present_flags);
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
    return true;
}

void OverlayTimes(Overlay* o, LONGLONG& present_qpc, LONGLONG& scanout_qpc)
{
    present_qpc = o->present_qpc; scanout_qpc = o->scanout_qpc;
}

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
    GpuWaitIdle(*o->g);
    ReleaseBuffers(o);
    const HRESULT hr = o->swap->ResizeBuffers(2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, o->flags);
    if (FAILED(hr)) { Log("[present] ResizeBuffers %ux%u failed 0x%08X", w, h, (unsigned)hr); return false; }
    o->w = w; o->h = h; o->last = RECT{};
    if (!GetBuffers(o)) return false;
    Log("[present] overlay resized to %ux%u", w, h);
    return true;
}

bool OverlayHotkey(Overlay* o, int id)
{
    if (!o || id < 0 || id >= kMaxHot) return false;
    return InterlockedExchange(&o->hot[id], 0) != 0;
}
