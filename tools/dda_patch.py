"""One-shot: add the Desktop Duplication path to src/capture.cpp + capture.h. Anchors must match once."""
import io, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def rd(rel): return io.open(os.path.join(ROOT, rel), encoding="utf-8").read()
def wr(rel, s): io.open(os.path.join(ROOT, rel), "w", encoding="utf-8", newline="").write(s)
def rep(s, old, new):
    if s.count(old) != 1: sys.exit("anchor x%d: %s" % (s.count(old), old[:70]))
    return s.replace(old, new)

s = rd("src/capture.cpp")
if "OpenDda" in s: sys.exit("already patched")
s = rep(s, '#include "log.h"', '#include "log.h"\n#include <dwmapi.h>\n#include <algorithm>\n#pragma comment(lib, "dwmapi.lib")')
s = rep(s, "    UINT pend_w = 0, pend_h = 0; ULONGLONG pend_since = 0;   // size-change deadband\n};", """    UINT pend_w = 0, pend_h = 0; ULONGLONG pend_since = 0;   // size-change deadband
    // Desktop Duplication path (delivers at the monitor refresh; WGC window capture tops out at 60 Hz)
    IDXGIOutputDuplication* dup = nullptr;
    bool  dup_frame_held = false;        // released right before the next acquire (the copy has long executed)
    RECT  out_rect = {};                 // monitor rect in desktop coords
    RECT  win_rect = {};                 // captured region (window bounds clamped to the monitor)
};""")
s = rep(s, "    for (int i = 0; i < 2; ++i) { REL(c->shared12[i]); REL(c->shared11[i]); }\n    REL(c->fence11);",
        "    if (c->dup) { if (c->dup_frame_held) c->dup->ReleaseFrame(); c->dup->Release(); c->dup = nullptr; }\n    for (int i = 0; i < 2; ++i) { REL(c->shared12[i]); REL(c->shared11[i]); }\n    REL(c->fence11);")

DDA = r'''// Window bounds clamped to the monitor that holds the window. Borderless fullscreen = the monitor.
static bool WindowRegion(HWND target, const RECT& out, RECT& region)
{
    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(target, &r);
    region.left = std::max(r.left, out.left); region.top = std::max(r.top, out.top);
    region.right = std::min(r.right, out.right); region.bottom = std::min(r.bottom, out.bottom);
    return region.right > region.left && region.bottom > region.top;
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
    if (!out) { Log("[cap] DDA: the window's monitor is not on the NGX adapter"); return false; }
    IDXGIOutput1* out1 = nullptr;
    out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    out->Release();
    if (!out1) { Log("[cap] DDA: IDXGIOutput1 unavailable"); return false; }
    const HRESULT hr = out1->DuplicateOutput(c->dev, &c->dup);
    out1->Release();
    if (FAILED(hr)) { Log("[cap] DDA: DuplicateOutput failed 0x%08X - falling back to window capture", (unsigned)hr); return false; }
    if (!WindowRegion(c->target, c->out_rect, c->win_rect)) { Log("[cap] DDA: window has no visible region"); c->dup->Release(); c->dup = nullptr; return false; }
    c->w = (UINT)(c->win_rect.right - c->win_rect.left); c->h = (UINT)(c->win_rect.bottom - c->win_rect.top);
    DXGI_OUTDUPL_DESC dd = {}; c->dup->GetDesc(&dd);
    Log("[cap] DDA: monitor %ldx%ld @ %u/%u Hz, format %u, region %ux%u at %ld,%ld",
        c->out_rect.right - c->out_rect.left, c->out_rect.bottom - c->out_rect.top,
        dd.ModeDesc.RefreshRate.Numerator, dd.ModeDesc.RefreshRate.Denominator, (unsigned)dd.ModeDesc.Format,
        c->w, c->h, c->win_rect.left - c->out_rect.left, c->win_rect.top - c->out_rect.top);
    c->is_float = dd.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    c->format_logged = true;
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
    if (info.LastPresentTime.QuadPart == 0) { res->Release(); return false; }   // only the cursor / metadata moved
    RECT region;
    if (WindowRegion(c->target, c->out_rect, region))
    {
        const UINT nw = (UINT)(region.right - region.left), nh = (UINT)(region.bottom - region.top);
        if (nw != c->w || nh != c->h)
        {
            if (nw != c->pend_w || nh != c->pend_h) { c->pend_w = nw; c->pend_h = nh; c->pend_since = GetTickCount64(); }
            res->Release(); return false;
        }
        c->pend_w = c->pend_h = 0;
        c->win_rect = region;
    }
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (!tex) return false;
    const int idx = (int)((c->fence_value + 1) & 1);
    if (!c->is_float)
    {
        D3D11_BOX box = { (UINT)(c->win_rect.left - c->out_rect.left), (UINT)(c->win_rect.top - c->out_rect.top), 0,
                          (UINT)(c->win_rect.right - c->out_rect.left), (UINT)(c->win_rect.bottom - c->out_rect.top), 1 };
        c->ctx->CopySubresourceRegion(c->shared11[idx], 0, 0, 0, 0, tex, 0, &box);
    }
    tex->Release();
    ++c->fence_value;
    c->ctx4->Signal(c->fence11, c->fence_value);
    c->ctx->Flush();
    c->cur = idx;
    fence_value = c->fence_value;
    sys_rel_100ns = info.LastPresentTime.QuadPart;   // QPC ticks
    return true;
}

Capture* CaptureOpen(Gpu& g, HWND target, bool show_cursor, bool show_border, bool prefer_dda)
{'''
s = rep(s, "Capture* CaptureOpen(Gpu& g, HWND target, bool show_cursor, bool show_border)\n{", DDA)
s = rep(s, """    try
    {
        if (!wgc::GraphicsCaptureSession::IsSupported()) { Log("[cap] Windows Graphics Capture not supported"); CaptureFree(c); return nullptr; }""",
"""    if (prefer_dda && OpenDda(g, c))
    {
        if (!CreateBridge(g, c)) { CaptureFree(c); return nullptr; }
        Log("[cap] capturing window %p via Desktop Duplication, region %ux%u", (void*)target, c->w, c->h);
        return c;
    }
    try
    {
        if (!wgc::GraphicsCaptureSession::IsSupported()) { Log("[cap] Windows Graphics Capture not supported"); CaptureFree(c); return nullptr; }""")
s = rep(s, "bool CaptureAcquire(Capture* c, DWORD wait_ms, UINT64& fence_value, LONGLONG& sys_rel_100ns)\n{\n    if (!c || CaptureLost(c)) return false;",
        "bool CaptureAcquire(Capture* c, DWORD wait_ms, UINT64& fence_value, LONGLONG& sys_rel_100ns)\n{\n    if (!c || CaptureLost(c)) return false;\n    if (c->dup) return AcquireDda(c, wait_ms, fence_value, sys_rel_100ns);")
s = rep(s, '    Log("[cap] capturing window %p at %ux%u", (void*)target, c->w, c->h);',
        '    Log("[cap] capturing window %p at %ux%u via Windows.Graphics.Capture (60 Hz ceiling)", (void*)target, c->w, c->h);')
s = rep(s, "bool            CaptureIsFloat(Capture* c) { return c->is_float; }",
        "bool            CaptureIsFloat(Capture* c) { return c->is_float; }\nbool            CaptureIsDda(Capture* c)   { return c && c->dup != nullptr; }")
wr("src/capture.cpp", s)

h = rd("src/capture.h")
h = rep(h, "Capture* CaptureOpen(Gpu& g, HWND target, bool show_cursor, bool show_border);",
        "// prefer_dda: DXGI Desktop Duplication on the window's monitor first (monitor refresh rate; the overlay\n// must be excluded from capture), falling back to Windows.Graphics.Capture (60 Hz ceiling).\nCapture* CaptureOpen(Gpu& g, HWND target, bool show_cursor, bool show_border, bool prefer_dda = true);\nbool     CaptureIsDda(Capture* c);")
wr("src/capture.h", h)
print("DDA path written")
