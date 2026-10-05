// No CPU wait on the GPU anywhere in here (see capture.h).
#include "capture.h"
#include <vector>
#include "log.h"
#include <dwmapi.h>
#include <algorithm>
#pragma comment(lib, "dwmapi.lib")
#include <unknwn.h>
#include <inspectable.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowsapp.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

struct Capture
{
    HWND target = nullptr;
    UINT w = 0, h = 0;
    ID3D11Device*         dev = nullptr;
    ID3D11DeviceContext*  ctx = nullptr;
    ID3D11DeviceContext4* ctx4 = nullptr;
    // ponytail: a ring of shared textures instead of a CPU wait. 4 deep: the D3D11 copy must never wait
    // on a D3D12 reader (that throttled capture to 30 fps). Frame n lives in slot n % kRing.
    static const int kRing = 4;
    ID3D11Texture2D* shared11[kRing] = {};
    ID3D12Resource*  shared12[kRing] = {};
    ID3D12Fence*     fence12 = nullptr;
    ID3D11Fence*     fence11 = nullptr;
    UINT64           fence_value = 0;
    HANDLE           frame_event = nullptr;
    wd3d::IDirect3DDevice            device{ nullptr };
    wgc::GraphicsCaptureItem         item{ nullptr };
    wgc::Direct3D11CaptureFramePool  pool{ nullptr };
    wgc::GraphicsCaptureSession      session{ nullptr };
    wgc::GraphicsCaptureItem::Closed_revoker              closed_rev;
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived_rev;
    volatile LONG closed = 0;
    bool is_float = false, format_logged = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;   // the shared textures: BGRA8, or FP16 for DDA of an HDR desktop
    float sdr_white = 0;                            // HDR desktop: its SDR white level, scRGB units
    UINT pend_w = 0, pend_h = 0; ULONGLONG pend_since = 0;   // size-change deadband
    // Desktop Duplication path
    IDXGIOutputDuplication* dup = nullptr;
    bool  dup_frame_held = false;        // released right before the next acquire (the copy has long executed)
    RECT  out_rect = {};                 // monitor rect in desktop coords
    RECT  win_rect = {};                 // captured region (window bounds clamped to the monitor)
    ULONGLONG region_at = 0;             // last DwmGetWindowAttribute: throttled, it calls into dwm.exe
    UINT64 accum_sum = 0, accum_n = 0;   // DDA AccumulatedFrames telemetry
    std::vector<BYTE> meta;        // dirty/move rect scratch
    LONG  foreign = 0;             // desktop updates that did not touch our region (telemetry, reset on read)
};

static void CaptureFree(Capture* c)
{
    c->arrived_rev.revoke();
    c->closed_rev.revoke();
    try
    {
        if (c->session) c->session.Close();
        if (c->pool) c->pool.Close();
    }
    catch (winrt::hresult_error const&) {}
    c->session = nullptr; c->pool = nullptr; c->item = nullptr; c->device = nullptr;
    if (c->dup) { if (c->dup_frame_held) c->dup->ReleaseFrame(); c->dup->Release(); c->dup = nullptr; }
    for (int i = 0; i < Capture::kRing; ++i) { REL(c->shared12[i]); REL(c->shared11[i]); }
    REL(c->fence11); REL(c->fence12); REL(c->ctx4); REL(c->ctx); REL(c->dev);
    if (c->frame_event) { CloseHandle(c->frame_event); c->frame_event = nullptr; }
    delete c;
}

static bool CreateBridge(Gpu& g, Capture* c)
{
    winrt::com_ptr<ID3D11Device5> d5;
    if (FAILED(c->dev->QueryInterface(__uuidof(ID3D11Device5), d5.put_void()))) { Log("[cap] ID3D11Device5 unavailable"); return false; }
    HANDLE nt = nullptr;
    for (int i = 0; i < Capture::kRing; ++i)
    {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = c->w; sd.Height = c->h; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = c->fmt; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(c->dev->CreateTexture2D(&sd, nullptr, &c->shared11[i]))) { Log("[cap] shared texture %d failed", i); return false; }
        IDXGIResource1* r1 = nullptr;
        c->shared11[i]->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
        HRESULT hr = r1 ? r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt) : E_NOINTERFACE;
        REL(r1);
        if (FAILED(hr)) { Log("[cap] CreateSharedHandle failed 0x%08X", hr); return false; }
        hr = g.dev->OpenSharedHandle(nt, __uuidof(ID3D12Resource), (void**)&c->shared12[i]);
        Log("[cap] shared handle %p -> D3D12 resource %d (%s %ux%u) 0x%08X", nt, i, c->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? "FP16" : "BGRA8", c->w, c->h, hr);
        CloseHandle(nt);
        if (FAILED(hr)) return false;
        c->shared12[i]->SetName(i ? L"capture_shared1" : L"capture_shared0");
    }
    if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void**)&c->fence12))) { Log("[cap] shared fence failed"); return false; }
    if (FAILED(g.dev->CreateSharedHandle(c->fence12, nullptr, GENERIC_ALL, nullptr, &nt))) { Log("[cap] fence handle failed"); return false; }
    d5->OpenSharedFence(nt, __uuidof(ID3D11Fence), (void**)&c->fence11);
    CloseHandle(nt);
    if (!c->fence11) { Log("[cap] OpenSharedFence failed"); return false; }
    return true;
}

// Window bounds clamped to the monitor that holds the window. Borderless fullscreen = the monitor.
static bool WindowRegion(HWND target, const RECT& out, RECT& region)
{
    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(target, &r);
    region.left = std::max(r.left, out.left); region.top = std::max(r.top, out.top);
    region.right = std::min(r.right, out.right); region.bottom = std::min(r.bottom, out.bottom);
    return region.right > region.left && region.bottom > region.top;
}

// The SDR white level Windows composes SDR content at on an HDR monitor, in scRGB units (1.0 = 80 nits).
static float SdrWhite(HMONITOR mon)
{
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof mi;
    UINT32 np = 0, nm = 0;
    if (!GetMonitorInfoW(mon, &mi) || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 1.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np); std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return 1.0f;
    for (UINT32 i = 0; i < np; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof src, paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id };
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice)) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
        wl.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof wl, paths[i].targetInfo.adapterId, paths[i].targetInfo.id };
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel) return wl.SDRWhiteLevel / 1000.0f;
    }
    return 1.0f;
}

// True while a real resize settles (CaptureSizeChanged reports it). Not a few pixels: Chrome fullscreen
// flips 2160 <-> 2159 lines on every focus change, and each rebuild of capture and FG costs ~0.5 s.
static bool Settling(Capture* c, UINT w, UINT h)
{
    const auto off = [](UINT a, UINT b) { return (a > b ? a - b : b - a) > 8; };
    if (!off(w, c->w) && !off(h, c->h)) { c->pend_w = c->pend_h = 0; return false; }
    if (w != c->pend_w || h != c->pend_h) { c->pend_w = w; c->pend_h = h; c->pend_since = GetTickCount64(); }
    return true;
}

// The slot the next frame is copied into before Publish.
static ID3D11Texture2D* Next(Capture* c) { return c->shared11[(c->fence_value + 1) % Capture::kRing]; }

// Signals the frame copied into Next(c) to the D3D12 side; it becomes CaptureTexture.
static bool Publish(Capture* c, UINT64& fence_value)
{
    c->ctx4->Signal(c->fence11, ++c->fence_value);
    c->ctx->Flush();
    fence_value = c->fence_value;
    return true;
}

static bool SessionHas(const wchar_t* prop)
{
    return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", prop);
}

static bool OpenDda(Gpu& g, Capture* c)
{
    const HMONITOR mon = MonitorFromWindow(c->target, MONITOR_DEFAULTTONEAREST);
    IDXGIOutput* out = nullptr;
    for (UINT i = 0; g.adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_OUTPUT_DESC d = {}; out->GetDesc(&d);
        if (d.Monitor == mon) { c->out_rect = d.DesktopCoordinates; break; }
        out->Release(); out = nullptr;
    }
    if (!out) { Log("[cap] DDA: the game's monitor is not on adapter %d - plug the display into that card, or change [gpu] adapter in justflow.ini (the startup log lists them)", g.adapter_index); return false; }
    // An HDR desktop comes as FP16 scRGB, converted to 8-bit in our swizzle at the SDR white level
    // (capture_in.hlsli). Asking Windows for BGRA8 converts it at 80 nits = white: far too bright.
    HRESULT hr = E_NOINTERFACE;
    IDXGIOutput5* out5 = nullptr; IDXGIOutput1* out1 = nullptr;
    if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput5), (void**)&out5)))
    {
        const DXGI_FORMAT fmts[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
        hr = out5->DuplicateOutput1(c->dev, 0, 2, fmts, &c->dup);
        out5->Release();
    }
    if (FAILED(hr) && SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1))) { hr = out1->DuplicateOutput(c->dev, &c->dup); out1->Release(); }
    out->Release();
    if (FAILED(hr)) { Log("[cap] DDA: DuplicateOutput failed 0x%08X - falling back to window capture", (unsigned)hr); return false; }
    if (!WindowRegion(c->target, c->out_rect, c->win_rect)) { Log("[cap] DDA: window has no visible region"); c->dup->Release(); c->dup = nullptr; return false; }
    c->w = (UINT)(c->win_rect.right - c->win_rect.left); c->h = (UINT)(c->win_rect.bottom - c->win_rect.top);
    DXGI_OUTDUPL_DESC dd = {}; c->dup->GetDesc(&dd);
    if (dd.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        c->fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; c->is_float = true; c->sdr_white = SdrWhite(mon);
        Log("[cap] DDA: HDR desktop - FP16, converted at SDR white %.0f nits", c->sdr_white * 80.0f);
    }
    Log("[cap] DDA: monitor %ldx%ld @ %u/%u Hz, format %u, region %ux%u at %ld,%ld",
        c->out_rect.right - c->out_rect.left, c->out_rect.bottom - c->out_rect.top,
        dd.ModeDesc.RefreshRate.Numerator, dd.ModeDesc.RefreshRate.Denominator, (unsigned)dd.ModeDesc.Format,
        c->w, c->h, c->win_rect.left - c->out_rect.left, c->win_rect.top - c->out_rect.top);
    return true;
}

static bool AcquireDda(Capture* c, DWORD wait_ms, UINT64& fence_value, LONGLONG& sys_rel_100ns)
{
    if (c->dup_frame_held) { c->dup->ReleaseFrame(); c->dup_frame_held = false; }
    DXGI_OUTDUPL_FRAME_INFO info = {}; IDXGIResource* res = nullptr;
    const HRESULT hr = c->dup->AcquireNextFrame(wait_ms, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) { Log("[cap] DDA: AcquireNextFrame 0x%08X - capture lost (mode change / access lost)", (unsigned)hr); InterlockedExchange(&c->closed, 1); return false; }
    c->dup_frame_held = true;
    c->accum_sum += info.AccumulatedFrames; ++c->accum_n;
    if (info.LastPresentTime.QuadPart == 0) { res->Release(); return false; }   // only the cursor / metadata moved
    // DDA reports updates of the whole OUTPUT (another app, a notification): a frame whose dirty and move
    // rects miss our region is not ours, or FG interpolates between identical images.
    if (info.TotalMetadataBufferSize)
    {
        if (c->meta.size() < info.TotalMetadataBufferSize) c->meta.resize(info.TotalMetadataBufferSize);
        const RECT rg = { c->win_rect.left - c->out_rect.left, c->win_rect.top - c->out_rect.top,
                          c->win_rect.right - c->out_rect.left, c->win_rect.bottom - c->out_rect.top };
        auto hits = [&](const RECT& r) { return r.left < rg.right && r.right > rg.left && r.top < rg.bottom && r.bottom > rg.top; };
        bool ours = false, known = false; UINT got = 0;
        if (SUCCEEDED(c->dup->GetFrameMoveRects((UINT)c->meta.size(), (DXGI_OUTDUPL_MOVE_RECT*)c->meta.data(), &got)))
        {
            known = true;
            const DXGI_OUTDUPL_MOVE_RECT* m = (const DXGI_OUTDUPL_MOVE_RECT*)c->meta.data();
            for (UINT i = 0; i < got / sizeof *m && !ours; ++i) ours = hits(m[i].DestinationRect);
        }
        if (!ours && SUCCEEDED(c->dup->GetFrameDirtyRects((UINT)c->meta.size(), (RECT*)c->meta.data(), &got)))
        {
            known = true;
            const RECT* d = (const RECT*)c->meta.data();
            for (UINT i = 0; i < got / sizeof *d && !ours; ++i) ours = hits(d[i]);
        }
        // fail open: if the metadata cannot be read, take the frame rather than starve the pipeline
        if (known && !ours) { InterlockedIncrement(&c->foreign); res->Release(); return false; }
    }
    // DwmGetWindowAttribute is a round trip to dwm.exe: polled at 10 Hz, well inside the 250 ms settle deadband.
    const ULONGLONG now_ms = GetTickCount64();
    if (now_ms - c->region_at >= 100)
    {
        c->region_at = now_ms;
        RECT region;
        if (WindowRegion(c->target, c->out_rect, region))
        {
            if (Settling(c, (UINT)(region.right - region.left), (UINT)(region.bottom - region.top))) { res->Release(); return false; }
            c->win_rect = region;
        }
    }
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC fd = {}; tex->GetDesc(&fd);
    if (fd.Format == c->fmt)   // an HDR switch mid-session changes it: the copy would fail, the reopen fixes it
    {
        // clamped to the shared texture: within the resize tolerance the region may be a few pixels larger
        const UINT x0 = (UINT)(c->win_rect.left - c->out_rect.left), y0 = (UINT)(c->win_rect.top - c->out_rect.top);
        D3D11_BOX box = { x0, y0, 0, x0 + std::min(c->w, (UINT)(c->win_rect.right - c->win_rect.left)), y0 + std::min(c->h, (UINT)(c->win_rect.bottom - c->win_rect.top)), 1 };
        c->ctx->CopySubresourceRegion(Next(c), 0, 0, 0, 0, tex, 0, &box);
    }
    tex->Release();
    sys_rel_100ns = info.LastPresentTime.QuadPart;   // QPC ticks
    return Publish(c, fence_value);
}

// The D3D11 device, then Desktop Duplication (if preferred and it opens) or WGC, then the bridge. False: free c.
static bool Open(Gpu& g, Capture* c, bool show_cursor, bool show_border, bool prefer_dda)
{
    const HWND target = c->target;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(g.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   fls, 2, D3D11_SDK_VERSION, &c->dev, nullptr, &c->ctx);
    if (FAILED(hr)) { Log("[cap] D3D11CreateDevice failed 0x%08X", hr); return false; }
    if (FAILED(c->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&c->ctx4))) { Log("[cap] ID3D11DeviceContext4 unavailable"); return false; }
    Log("[cap] D3D11 device on adapter %d (luid %08X:%08X)", g.adapter_index, (unsigned)g.luid.HighPart, (unsigned)g.luid.LowPart);

    if (prefer_dda && OpenDda(g, c))
    {
        if (!CreateBridge(g, c)) return false;
        Log("[cap] capturing window %p via Desktop Duplication, region %ux%u", (void*)target, c->w, c->h);
        return true;
    }
    bool uncapped = false;
    try
    {
        if (!wgc::GraphicsCaptureSession::IsSupported()) { Log("[cap] Windows Graphics Capture not supported"); return false; }
        IDXGIDevice* dxgi = nullptr;
        c->dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
        winrt::com_ptr<::IInspectable> insp;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi, insp.put());
        dxgi->Release();
        if (FAILED(hr)) { Log("[cap] CreateDirect3D11DeviceFromDXGIDevice failed 0x%08X", hr); return false; }
        c->device = insp.as<wd3d::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        // the desktop window stands for its monitor (full-desktop mode): capture the monitor
        hr = target == GetDesktopWindow()
            ? interop->CreateForMonitor(MonitorFromWindow(target, MONITOR_DEFAULTTOPRIMARY), winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c->item))
            : interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c->item));
        if (FAILED(hr) || !c->item) { Log("[cap] CreateForWindow/Monitor failed 0x%08X", hr); return false; }
        const auto size = c->item.Size();
        if (size.Width <= 0 || size.Height <= 0) { Log("[cap] window has no size (minimised?)"); return false; }
        c->w = (UINT)size.Width; c->h = (UINT)size.Height;
        Log("[cap] item %ux%u, pool format B8G8R8A8UIntNormalized (%d)", c->w, c->h, (int)wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);

        c->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(c->device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        c->session = c->pool.CreateCaptureSession(c->item);
        c->arrived_rev = c->pool.FrameArrived(winrt::auto_revoke, [ev = c->frame_event](auto&&, auto&&) { SetEvent(ev); });
        c->closed_rev = c->item.Closed(winrt::auto_revoke, [pc = &c->closed](auto&&, auto&&) { InterlockedExchange(pc, 1); });
        try { c->session.IsCursorCaptureEnabled(show_cursor); }
        catch (winrt::hresult_error const&) { Log("[cap] cursor capture setting not honoured"); }
        if (SessionHas(L"IsBorderRequired"))
        {
            try { c->session.IsBorderRequired(show_border); }
            catch (winrt::hresult_error const&) { Log("[cap] border setting not honoured"); }
        }
        // Windows 11 24H2+: a 1 ms MinUpdateInterval delivers at the rate the window updates (else ~60 Hz).
        if (SessionHas(L"MinUpdateInterval"))
        {
            try { c->session.MinUpdateInterval(std::chrono::milliseconds(1)); uncapped = true; }
            catch (winrt::hresult_error const&) { Log("[cap] MinUpdateInterval not honoured - window capture stays near 60 Hz"); }
        }
        if (!CreateBridge(g, c)) return false;
        c->session.StartCapture();
    }
    catch (winrt::hresult_error const& e)
    {
        Log("[cap] open threw 0x%08X: %ls", (unsigned)e.code(), e.message().c_str());
        return false;
    }
    Log("[cap] capturing window %p at %ux%u via Windows.Graphics.Capture (%s)", (void*)target, c->w, c->h,
        uncapped ? "uncapped: MinUpdateInterval 1 ms" : "~60 Hz ceiling: this Windows build has no MinUpdateInterval");
    return true;
}

Capture* CaptureOpen(Gpu& g, HWND target, bool show_cursor, bool show_border, bool prefer_dda)
{
    static bool apartment = false;
    if (!apartment)
    {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch (winrt::hresult_error const& e) { Log("[cap] init_apartment 0x%08X (already initialised?)", (unsigned)e.code()); }
        apartment = true;
    }
    if (!IsWindow(target)) { Log("[cap] %p is not a window", (void*)target); return nullptr; }
    // A minimised window captures as its 219x30 caption, which the pipeline cannot use: wait (silently,
    // the caller retries every second) - FindTarget keeps it as the target so alt-tab does not lose it.
    if (IsIconic(target)) return nullptr;
    Capture* c = new Capture();
    c->target = target;
    c->frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!Open(g, c, show_cursor, show_border, prefer_dda)) { CaptureFree(c); return nullptr; }
    return c;
}

void CaptureClose(Capture* c)
{
    if (!c) return;
    CaptureFree(c);
    Log("[cap] closed");
}

bool CaptureAcquire(Capture* c, DWORD wait_ms, UINT64& fence_value, LONGLONG& sys_rel_100ns)
{
    if (!c || CaptureLost(c)) return false;
    if (c->dup) return AcquireDda(c, wait_ms, fence_value, sys_rel_100ns);
    WaitForSingleObject(c->frame_event, wait_ms);   // the pool decides; a stale event just costs a TryGetNextFrame
    try
    {
        wgc::Direct3D11CaptureFrame frame = c->pool.TryGetNextFrame();
        if (!frame) return false;
        for (int drained = 0; drained < 8; ++drained)
        {
            auto next = c->pool.TryGetNextFrame();
            if (!next) break;
            frame.Close();
            frame = next;
        }
        sys_rel_100ns = frame.SystemRelativeTime().count();
        const auto cs = frame.ContentSize();
        // The pool surface stays c->w x c->h inside the tolerance, so the copy below is still in bounds.
        if (Settling(c, (UINT)cs.Width, (UINT)cs.Height)) { frame.Close(); return false; }   // hold the last picture while the size settles
        auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) { frame.Close(); return false; }
        D3D11_TEXTURE2D_DESC fd = {}; tex->GetDesc(&fd);
        if (!c->format_logged)
        {
            c->format_logged = true;
            c->is_float = fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            Log("[cap] frame format %u (%s), surface %ux%u", (unsigned)fd.Format,
                fd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : c->is_float ? "FP16" : "other", fd.Width, fd.Height);
        }
        // A format other than the shared texture's cannot be copied (D3D11 refuses); the fence is
        // still signalled so the caller sees the frame and refuses on CaptureIsFloat.
        if (fd.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            D3D11_BOX box = { 0, 0, 0, c->w, c->h, 1 };
            c->ctx->CopySubresourceRegion(Next(c), 0, 0, 0, 0, tex, 0, &box);
        }
        tex->Release();
        frame.Close();
        return Publish(c, fence_value);
    }
    catch (winrt::hresult_error const& e)
    {
        Log("[cap] acquire threw 0x%08X: %ls - capture lost", (unsigned)e.code(), e.message().c_str());
        InterlockedExchange(&c->closed, 1);
        return false;
    }
}

ID3D12Resource* CaptureTexture(Capture* c) { return c->shared12[c->fence_value % Capture::kRing]; }
ID3D12Fence*    CaptureFence(Capture* c)   { return c->fence12; }
UINT            CaptureWidth(Capture* c)   { return c->w; }
UINT            CaptureHeight(Capture* c)  { return c->h; }
bool            CaptureIsFloat(Capture* c) { return c->is_float; }
bool            CaptureSeesOverlay(Capture* c) { return c->dup || c->target == GetDesktopWindow(); }
float           CaptureSdrWhite(Capture* c) { return c->sdr_white; }
bool            CaptureIsDda(Capture* c)   { return c && c->dup != nullptr; }
UINT            CaptureForeign(Capture* c) { return c ? (UINT)InterlockedExchange(&c->foreign, 0) : 0; }
bool CaptureWgcUncapped()
{
    try { return SessionHas(L"MinUpdateInterval"); }
    catch (...) { return false; }
}

// 1.0 = every composed frame reached us; 3.0 = the compositor produced three per acquire, i.e. we are slow.
double CaptureAccumMean(Capture* c) { if (!c || !c->dup || !c->accum_n) return 0; double m = (double)c->accum_sum / (double)c->accum_n; c->accum_sum = 0; c->accum_n = 0; return m; }

bool CaptureSizeChanged(Capture* c, UINT& new_w, UINT& new_h)
{
    if (!c->pend_w || !c->pend_h || GetTickCount64() - c->pend_since < 250) return false;
    new_w = c->pend_w; new_h = c->pend_h;
    return true;
}

bool CaptureLost(Capture* c)
{
    return !c || c->closed != 0 || !IsWindow(c->target);
}
