// justflow_xe: the frame-generation-only build for any D3D12 GPU, written for Intel Arc iGPUs.
// No NGX, no NVOFA: motion comes from xe_flow (compute-shader block matching).
//
//   justflow_xe.exe                                      tray app: automatic, foreground window (xe_app)
//   justflow_xe.exe --live --window <title substring>   frame generation over that window (xe_fg)
//
// This first cut is the optical-flow bench:
//   justflow_xe.exe --bench <dir|png> [--size WxH] [--factor N] [--lambda X] [--refine N] [--frames N] [--pairs N]
//                   [--dump] [--adapter I] [--vendor HEX]
//   <dir>  a tools/scene export (frame_NNNN.png [+ mv_NNNN.f32 + manifest.json]) or any folder of
//          consecutive PNGs: each consecutive pair is measured; with mv files the backward flow is
//          scored against the scene's exact motion (end-point error, native pixels)
//   <png>  one image: the second frame is synthesised by a known shift + zoom (--shift DX,DY
//          --zoom Z), so both directions are scored
//   --size resamples the input first (e.g. 2880x1800 = this laptop's panel), ground truth follows
// Every pair also reports the PSNR of prev warped by the backward flow against cur, next to the
// PSNR of prev itself (no motion compensation) - the gap is what the flow buys an interpolator.
//
//   justflow_xe.exe --interp N --seq <dir> [...]   interpolation against real skipped frames (below)
#include "d3d.h"
#include "log.h"
#include "png.h"
#include "xe_flow.h"
#include "xe_interp.h"
#include "xe_fg.h"
#include "xe_app.h"
#include <shellapi.h>
#include "capture.h"
#include "present.h"
#include <dwmapi.h>
#include <timeapi.h>
#include <atomic>
#include "cs_xe_warp.h"
#include "cs_gray.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p); return s.substr(0, s.find_last_of(L"\\/"));
}
static std::wstring Widen(const char* s) { return std::wstring(s, s + strlen(s)); }

static float Half(uint16_t h)
{
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = !e ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}

// ---- images ------------------------------------------------------------------------------------------
struct Image { UINT w = 0, h = 0; std::vector<uint8_t> px; };   // RGBA8

static void Bilinear(const Image& a, float x, float y, uint8_t* out)
{
    x = std::clamp(x - 0.5f, 0.0f, (float)a.w - 1); y = std::clamp(y - 0.5f, 0.0f, (float)a.h - 1);
    const UINT x0 = (UINT)x, y0 = (UINT)y, x1 = std::min(x0 + 1, a.w - 1), y1 = std::min(y0 + 1, a.h - 1);
    const float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; ++c)
    {
        auto p = [&](UINT xx, UINT yy) { return (float)a.px[((size_t)yy * a.w + xx) * 4 + c]; };
        const float v = (p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) + (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy;
        out[c] = (uint8_t)std::clamp(v + 0.5f, 0.0f, 255.0f);
    }
}

// Plain bilinear resample, no prefilter: it aliases a little on a downscale, which a flow test
// tolerates (and real captures are not prefiltered either).
static Image Resize(const Image& a, UINT w, UINT h)
{
    if (a.w == w && a.h == h) return a;
    Image b; b.w = w; b.h = h; b.px.resize((size_t)w * h * 4);
    const float sx = (float)a.w / w, sy = (float)a.h / h;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) Bilinear(a, (x + 0.5f) * sx, (y + 0.5f) * sy, &b.px[((size_t)y * w + x) * 4]);
    return b;
}

// ---- synthetic HUD (--ui) -----------------------------------------------------------------------------
// The scene's HUD is HTML over the WebGL canvas, so its exports have none. This draws a static one with
// GDI - semi-transparent panels, an action bar, a minimap, small anti-aliased text like a game chat -
// and Load composites it onto every frame: static UI over a moving world, the case frame generation
// smears. Scaled to the frame height (1800 = the sizes below).
struct UiLayer { UINT w = 0, h = 0; std::vector<uint8_t> rgba; };   // straight alpha
static UiLayer g_ui;

static void MakeUi(UINT w, UINT h)
{
    g_ui = UiLayer(); g_ui.w = w; g_ui.h = h; g_ui.rgba.assign((size_t)w * h * 4, 0);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = (LONG)w; bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bm) { if (dc) DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, bm);
    const float s = h / 1800.0f;
    auto S = [&](float v) { return (int)(v * s + 0.5f); };
    const COLORREF panel = RGB(26, 24, 20);   // the panel colour marks panel pixels (alpha 180 below)
    auto box = [&](int x0, int y0, int x1, int y1, COLORREF c) { RECT r = { x0, y0, x1, y1 }; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); };
    HFONT font = CreateFontW(-S(24), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    HGDIOBJ oldf = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(236, 236, 236));
    auto text = [&](int x, int y, const wchar_t* t) { TextOutW(dc, x, y, t, (int)wcslen(t)); };
    // unit frame
    box(S(36), S(80), S(600), S(232), panel);
    text(S(56), S(96), L"Testwalker <JustFlow>");
    box(S(56), S(140), S(576), S(168), RGB(60, 150, 70)); box(S(56), S(180), S(420), S(208), RGB(60, 90, 180));
    // chat
    box(S(36), (int)h - S(420), S(820), (int)h - S(80), panel);
    const wchar_t* lines[] = { L"[General] the quick brown fox jumps over the lazy dog", L"[Party] small text is what frame generation smears first",
                               L"[Say] 0123456789 iIlL1 oO0 |/\\-_=+", L"[Trade] WTS [Glowing Thing of Smearing] 99g", L"[Guild] brb, flow fields" };
    for (int i = 0; i < 5; ++i) text(S(56), (int)h - S(400) + i * S(40), lines[i]);
    // action bar: 12 framed squares
    const int bx = (int)w / 2 - S(12 * 72) / 2, by = (int)h - S(150);
    box(bx - S(10), by - S(10), bx + S(12 * 72), by + S(82), panel);
    for (int i = 0; i < 12; ++i) box(bx + i * S(72), by, bx + i * S(72) + S(62), by + S(62), i % 3 == 0 ? RGB(150, 60, 50) : i % 3 == 1 ? RGB(50, 80, 150) : RGB(130, 95, 50));
    // minimap
    HBRUSH mb = CreateSolidBrush(RGB(34, 52, 30)); HGDIOBJ ob = SelectObject(dc, mb);
    HPEN pen = CreatePen(PS_SOLID, S(5), RGB(110, 95, 50)); HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, (int)w - S(360), S(60), (int)w - S(60), S(360));
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(mb); DeleteObject(pen);
    // fps counter, on a small panel bottom right
    box((int)w - S(300), (int)h - S(70), (int)w - S(30), (int)h - S(30), panel);
    text((int)w - S(286), (int)h - S(68), L"2880x1800  45 fps");
    GdiFlush();
    const uint8_t* src = (const uint8_t*)bits;   // BGRA, 0 = no UI
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        const uint8_t b = src[i * 4], gch = src[i * 4 + 1], r = src[i * 4 + 2];
        if (!b && !gch && !r) continue;
        const bool is_panel = r == GetRValue(panel) && gch == GetGValue(panel) && b == GetBValue(panel);
        uint8_t* o = &g_ui.rgba[i * 4];
        o[0] = r; o[1] = gch; o[2] = b; o[3] = is_panel ? 180 : 255;
    }
    SelectObject(dc, oldf); DeleteObject(font); SelectObject(dc, old); DeleteObject(bm); DeleteDC(dc);
}

static void CompositeUi(Image& im)
{
    if (g_ui.w != im.w || g_ui.h != im.h) return;
    for (size_t i = 0; i < (size_t)im.w * im.h; ++i)
    {
        const uint8_t* u = &g_ui.rgba[i * 4];
        if (!u[3]) continue;
        uint8_t* p = &im.px[i * 4];
        for (int c = 0; c < 3; ++c) p[c] = (uint8_t)((u[c] * u[3] + p[c] * (255 - u[3]) + 127) / 255);
    }
}

static bool Load(const std::wstring& path, Image& im, UINT want_w, UINT want_h)
{
    Image raw;
    if (!LoadPngRgba(path.c_str(), raw.px, raw.w, raw.h)) return false;
    im = (want_w && want_h) ? Resize(raw, want_w, want_h) : std::move(raw);
    CompositeUi(im);
    return true;
}

// RGB PSNR inside `border`; with `mask`, only where mask[pixel] != 0.
static double Psnr(const Image& a, const std::vector<uint8_t>& b, UINT border, const std::vector<uint8_t>* mask = nullptr)
{
    double se = 0; size_t n = 0;
    for (UINT y = border; y + border < a.h; ++y)
        for (UINT x = border; x + border < a.w; ++x)
            if (!mask || (*mask)[(size_t)y * a.w + x])
            for (int c = 0; c < 3; ++c)
            {
                const size_t i = ((size_t)y * a.w + x) * 4 + c;
                const double d = (double)a.px[i] - (double)b[i]; se += d * d; ++n;
            }
    return n && se > 0 ? 10.0 * log10(255.0 * 255.0 * n / se) : 99.0;
}

// ---- ground truth -------------------------------------------------------------------------------------
// Motion of a native pixel of the evaluated frame, in native pixels, in the grid's direction.
// false = no ground truth there (sky / uncovered).
struct Truth
{
    // scene export: mv at mvw x mvh, "previous minus current", mv-grid pixels; covered flag
    std::vector<float> mv; UINT mvw = 0, mvh = 0;
    // synthetic: prev -> cur is p' = c + z (p - c) + s
    bool synth = false; float zoom = 1, sx = 0, sy = 0, cx = 0, cy = 0;
    bool Get(int dir, UINT w, UINT h, float x, float y, float& gx, float& gy) const
    {
        if (synth)
        {
            if (dir == 0) { gx = cx + zoom * (x - cx) + sx - x; gy = cy + zoom * (y - cy) + sy - y; }        // prev -> cur
            else { gx = cx + (x - cx - sx) / zoom - x; gy = cy + (y - cy - sy) / zoom - y; }                  // cur -> prev
            return true;
        }
        if (dir != 1 || mv.empty()) return false;
        const UINT mx = std::min((UINT)(x * mvw / w), mvw - 1), my = std::min((UINT)(y * mvh / h), mvh - 1);
        const float* m = &mv[((size_t)my * mvw + mx) * 3];
        if (m[2] < 0.5f) return false;
        gx = m[0] * (float)w / mvw; gy = m[1] * (float)h / mvh;
        return true;
    }
};

struct Epe
{
    std::vector<float> e, zero;   // estimate error, and the error of assuming no motion
    void Add(float ex, float ey, float gx, float gy) { e.push_back(std::hypot(ex - gx, ey - gy)); zero.push_back(std::hypot(gx, gy)); }
    void Report(const char* name) const
    {
        if (e.empty()) { Log("[bench] %s: no ground truth", name); return; }
        auto pct = [](std::vector<float> v, double p) { std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(v.size() * p))]; };
        double mean = 0; size_t lt1 = 0, lt4 = 0;
        for (float x : e) { mean += x; lt1 += x < 1.0f; lt4 += x < 4.0f; }
        Log("[bench] %s EPE (native px, %zu blocks): median %.2f  mean %.2f  p90 %.2f  <1px %.1f%%  <4px %.1f%%   | motion itself: median %.1f p90 %.1f",
            name, e.size(), pct(e, 0.5), mean / e.size(), pct(e, 0.9), 100.0 * lt1 / e.size(), 100.0 * lt4 / e.size(), pct(zero, 0.5), pct(zero, 0.9));
    }
};

// Mean absolute luma gradient over the native footprint of a block, 0..255: below ~2 there is
// nothing to match (sky, a flat black sphere) and the motion there is not observable at all.
static float Texture(const Image& im, UINT x0, UINT y0, UINT n)
{
    auto luma = [&](UINT x, UINT y) { const uint8_t* p = &im.px[((size_t)y * im.w + x) * 4]; return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; };
    float sum = 0; UINT cnt = 0;
    for (UINT y = y0; y + 1 < std::min(y0 + n, im.h); ++y)
        for (UINT x = x0; x + 1 < std::min(x0 + n, im.w); ++x) { const float c = luma(x, y); sum += fabsf(luma(x + 1, y) - c) + fabsf(luma(x, y + 1) - c); ++cnt; }
    return cnt ? sum / cnt : 0;
}

// Score one grid: every block whose centre is at least `margin` native px inside the frame, into
// `all` and, when the block of `from` (the frame the grid's blocks belong to) has texture, `tex`.
static void Score(const std::vector<uint16_t>& grid, UINT gw, UINT gh, UINT f, UINT cell, UINT w, UINT h, const Truth& t, int dir,
                  const Image& from, Epe& all, Epe& tex)
{
    const float margin = 32;
    for (UINT by = 0; by < gh; ++by)
        for (UINT bx = 0; bx < gw; ++bx)
        {
            const float x = (bx * cell + cell * 0.5f) * f, y = (by * cell + cell * 0.5f) * f;
            if (x < margin || y < margin || x > w - margin || y > h - margin) continue;
            float gx, gy;
            if (!t.Get(dir, w, h, x, y, gx, gy)) continue;
            const size_t i = ((size_t)by * gw + bx) * 2;
            const float ex = Half(grid[i]) * f, ey = Half(grid[i + 1]) * f;
            all.Add(ex, ey, gx, gy);
            if (Texture(from, bx * cell * f, by * cell * f, cell * f) >= 2.0f) tex.Add(ex, ey, gx, gy);
        }
}

// Flow grid as colour (hue = direction, brightness = magnitude / scale), each block 8x8 pixels.
static void DumpFlow(const wchar_t* path, const std::vector<uint16_t>& grid, UINT gw, UINT gh, UINT f, UINT cell, float scale)
{
    const UINT w = gw * cell, h = gh * cell;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const size_t i = ((size_t)(y / cell) * gw + x / cell) * 2;
            const float vx = Half(grid[i]) * f, vy = Half(grid[i + 1]) * f;
            const float v = std::min(1.0f, std::hypot(vx, vy) / scale), a = atan2f(vy, vx) / 6.2831853f + 0.5f;
            uint8_t* o = &px[((size_t)y * w + x) * 4];
            for (int c = 0; c < 3; ++c)
            {
                const float k = fmodf(a * 6.0f + (c == 0 ? 0.0f : c == 1 ? 4.0f : 2.0f), 6.0f);
                o[c] = (uint8_t)(255.0f * v * (1.0f - std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f)));
            }
            o[3] = 255;
        }
    SavePngRgba(path, px.data(), w, h);
}

// ---- bench --------------------------------------------------------------------------------------------
static std::vector<std::wstring> ListFrames(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return out;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName); } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    std::sort(out.begin(), out.end());
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::wstring& n) { return n.rfind(L"preview", 0) == 0; }), out.end());
    return out;
}

static bool ReadFile(const std::wstring& path, std::vector<char>& data)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") || !fp) return false;
    fseek(fp, 0, SEEK_END); data.resize((size_t)ftell(fp)); fseek(fp, 0, SEEK_SET);
    const bool ok = fread(data.data(), 1, data.size(), fp) == data.size();
    fclose(fp); return ok;
}

static int RunBench(int argc, char** argv)
{
    std::wstring input; UINT size_w = 0, size_h = 0, factor = 2; int frames = 200, pairs = 1 << 30, adapter = -1, refine = 1, fine = 1, sub = 1; bool profile = false; UINT vendor = 0;
    float lambda = 0.0f, zoom = 1.02f, shx = 23.0f, shy = -11.0f; bool dump = false;
    for (int i = 1; i < argc; ++i)
    {
        const bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--bench") && more) input = Widen(argv[++i]);
        else if (!strcmp(argv[i], "--size") && more) sscanf_s(argv[++i], "%ux%u", &size_w, &size_h);
        else if (!strcmp(argv[i], "--factor") && more) factor = (UINT)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lambda") && more) lambda = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--refine") && more) refine = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fine") && more) fine = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sub") && more) sub = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile")) profile = true;
        else if (!strcmp(argv[i], "--frames") && more) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pairs") && more) pairs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && more) zoom = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--shift") && more) sscanf_s(argv[++i], "%f,%f", &shx, &shy);
        else if (!strcmp(argv[i], "--adapter") && more) adapter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vendor") && more) vendor = (UINT)strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--dump")) dump = true;
    }
    const std::wstring dir = ExeDir();
    LogInit((dir + L"\\xe_bench.log").c_str());

    // ---- inputs: a pair source (lazy for sequences: 4K frames are 33 MB each) ----
    Truth truth; std::wstring seq_dir; std::vector<std::wstring> names; Image first;
    const DWORD attr = GetFileAttributesW(input.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { Log("[bench] no input at %ls", input.c_str()); return 1; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        seq_dir = input; names = ListFrames(seq_dir);
        if (names.size() < 2) { Log("[bench] %ls: need at least 2 PNGs", input.c_str()); return 1; }
        std::vector<char> man;
        if (ReadFile(seq_dir + L"\\manifest.json", man))
        {
            man.push_back(0);
            const char* m = strstr(man.data(), "\"mv\":{");
            if (m) sscanf_s(m, "\"mv\":{\"w\":%u,\"h\":%u", &truth.mvw, &truth.mvh);
        }
        if (!Load(seq_dir + L"\\" + names[0], first, size_w, size_h)) return 1;
        Log("[bench] sequence %ls: %zu frames of %ux%u%s", input.c_str(), names.size(), first.w, first.h,
            truth.mvw ? " with exact motion vectors" : " (no motion vectors: PSNR only)");
    }
    else
    {
        if (!Load(input, first, size_w, size_h)) return 1;
        truth.synth = true; truth.zoom = zoom; truth.sx = shx; truth.sy = shy; truth.cx = first.w * 0.5f; truth.cy = first.h * 0.5f;
        Log("[bench] %ls %ux%u, synthetic second frame: shift (%.1f, %.1f) zoom %.3f", input.c_str(), first.w, first.h, shx, shy, zoom);
    }
    const UINT w = first.w, h = first.h;
    // Synthetic cur = prev pushed through the affine map: cur(x) = prev(inverse(x)).
    auto synth_cur = [&](const Image& a)
    {
        Image b; b.w = a.w; b.h = a.h; b.px.resize(a.px.size());
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                float gx, gy; truth.Get(1, w, h, x + 0.5f, y + 0.5f, gx, gy);
                Bilinear(a, x + 0.5f + gx, y + 0.5f + gy, &b.px[((size_t)y * w + x) * 4]);
            }
        return b;
    };

    // ---- GPU ----
    Gpu g;
    if (!GpuInit(g, adapter, vendor)) return 1;
    DXGI_ADAPTER_DESC1 ad = {}; { IDXGIAdapter1* a1 = nullptr; if (SUCCEEDED(g.adapter->QueryInterface(IID_PPV_ARGS(&a1)))) { a1->GetDesc1(&ad); a1->Release(); } }
    XeFlow* xf = XeFlowCreate(g, w, h, factor);
    if (!xf) return 1;
    XeFlowSetLambda(xf, lambda);
    XeFlowSetRefine(xf, refine); XeFlowSetFine(xf, fine); XeFlowSetSubsel(xf, sub != 0);
    const UINT gw = XeFlowGridW(xf), gh = XeFlowGridH(xf), f = XeFlowFactor(xf), cell = XeFlowCell(xf);
    ComputePso warp;
    ID3D12Resource* tex[2] = { GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, NPSR, L"bench_prev"),
                               GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, NPSR, L"bench_cur") };
    ID3D12Resource* recon = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"bench_recon");
    if (!tex[0] || !tex[1] || !recon || !GpuMakeCompute(g, g_cs_xe_warp, sizeof g_cs_xe_warp, 2, 1, 6, warp, L"xe_warp")) return 1;

    // ---- accuracy: every pair ----
    Epe epe[2], epe_tex[2]; double psnr_sum = 0, psnr0_sum = 0; int npairs = 0;
    // Reference: prev warped by the scene's exact motion vectors, all three PSNRs on the pixels that
    // have them. Those vectors follow geometry, not what is visible: moving shadows, highlights and
    // disocclusions are wrong in them, so a good optical flow can (and does) beat them here. This
    // is also why the end-point error against them overstates the flow's error.
    double gt_sum = 0, ours_m_sum = 0, zero_m_sum = 0; int ngt = 0;
    std::vector<uint8_t> gtw((size_t)w * h * 4), mask((size_t)w * h);
    Image prev = first, cur;
    std::vector<uint16_t> grid[2]; std::vector<uint8_t> rec((size_t)w * h * 4);
    for (size_t i = 1; ; ++i)
    {
        if (truth.synth) { if (i > 1) break; cur = synth_cur(prev); }
        else
        {
            if (i >= names.size() || npairs >= pairs) break;
            if (!Load(seq_dir + L"\\" + names[i], cur, size_w, size_h) || cur.w != w || cur.h != h) { Log("[bench] %ls skipped", names[i].c_str()); continue; }
            truth.mv.clear();
            if (truth.mvw)
            {
                std::vector<char> raw; wchar_t mvn[64]; _snwprintf_s(mvn, _TRUNCATE, L"mv_%04zu.f32", i);
                if (ReadFile(seq_dir + L"\\" + mvn, raw) && raw.size() == (size_t)truth.mvw * truth.mvh * 12)
                    truth.mv.assign((const float*)raw.data(), (const float*)raw.data() + raw.size() / 4);
            }
        }
        if (!GpuUploadTex(g, tex[0], prev.px.data(), w, h, 4, NPSR) || !GpuUploadTex(g, tex[1], cur.px.data(), w, h, 4, NPSR)) return 1;
        if (!GpuBegin(g)) return 1;
        XeFlowPyramid(xf, g.list, tex[0], 0);
        XeFlowPyramid(xf, g.list, tex[1], 1);
        XeFlowEstimate(xf, g.list, 0, 1, 0);   // forward: prev blocks -> cur
        XeFlowEstimate(xf, g.list, 1, 0, 1);   // backward: cur blocks -> prev
        struct { UINT w, h, gw, gh; float f, cell; } wc = { w, h, gw, gh, (float)f, (float)cell };
        const GpuView ws[2] = { { tex[0], DXGI_FORMAT_UNKNOWN }, { XeFlowGrid(xf, 1), DXGI_FORMAT_UNKNOWN } }, wu = { recon, DXGI_FORMAT_UNKNOWN };
        GpuBarrier(g.list, recon, NPSR, UAV);
        GpuDispatch(g, g.list, warp, ws, &wu, &wc, GpuGroups(w, 8), GpuGroups(h, 8));
        GpuBarrier(g.list, recon, UAV, NPSR);
        if (!GpuEnd(g) || !GpuWaitIdle(g)) return 1;
        for (int d = 0; d < 2; ++d)
        {
            grid[d].resize((size_t)gw * gh * 2);
            if (!GpuReadbackTex(g, XeFlowGrid(xf, d), grid[d].data(), gw, gh, 4, NPSR)) return 1;
            Score(grid[d], gw, gh, f, cell, w, h, truth, d, d ? cur : prev, epe[d], epe_tex[d]);
        }
        if (!GpuReadbackTex(g, recon, rec.data(), w, h, 4, NPSR)) return 1;
        const double p = Psnr(cur, rec, 16), p0 = Psnr(cur, prev.px, 16);
        psnr_sum += p; psnr0_sum += p0; ++npairs;
        if (!truth.mv.empty())   // synthetic pairs are generated by this same warp: its ceiling is trivially exact
        {
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x)
                {
                    float gx, gy; const size_t i2 = (size_t)y * w + x;
                    mask[i2] = truth.Get(1, w, h, x + 0.5f, y + 0.5f, gx, gy);
                    if (mask[i2]) Bilinear(prev, x + 0.5f + gx, y + 0.5f + gy, &gtw[i2 * 4]);
                }
            const double pg = Psnr(cur, gtw, 16, &mask), pm = Psnr(cur, rec, 16, &mask), pz = Psnr(cur, prev.px, 16, &mask);
            gt_sum += pg; ours_m_sum += pm; zero_m_sum += pz; ++ngt;
        }
        if (dump)
        {
            wchar_t path[MAX_PATH];
            _snwprintf_s(path, _TRUNCATE, L"%ls\\xe_flow_%03zu.png", dir.c_str(), i); DumpFlow(path, grid[1], gw, gh, f, cell, 64.0f);
            _snwprintf_s(path, _TRUNCATE, L"%ls\\xe_warp_%03zu.png", dir.c_str(), i); SavePngRgba(path, rec.data(), w, h);
        }
        if (!truth.synth) Log("[bench] pair %zu: PSNR warped %.2f dB, unwarped %.2f dB", i, p, p0);
        prev = std::move(cur);
    }
    epe[0].Report("forward  all     "); epe_tex[0].Report("forward  textured");
    epe[1].Report("backward all     "); epe_tex[1].Report("backward textured");
    if (npairs) Log("[bench] %d pairs: mean PSNR prev warped by the backward flow %.2f dB vs %.2f dB unwarped (+%.2f)",
                    npairs, psnr_sum / npairs, psnr0_sum / npairs, (psnr_sum - psnr0_sum) / npairs);
    if (ngt) Log("[bench] on pixels with ground truth: ours %.2f dB, warped by the scene's own motion vectors %.2f dB (ours %+.2f), unwarped %.2f dB",
                 ours_m_sum / ngt, gt_sum / ngt, (ours_m_sum - gt_sum) / ngt, zero_m_sum / ngt);

    // ---- cost: per new frame = one pyramid + both directions ----
    // Waits for idle every frame so each timestamp window is the GPU's own time, not queueing.
    std::vector<double> t[4];
    for (int i = 0; i < frames; ++i)
    {
        if (!GpuBegin(g)) return 1;
        const int slot = g.slot, s = i & 1;
        GpuStamp(g, g.list, 6);
        GpuStamp(g, g.list, 0); XeFlowPyramid(xf, g.list, tex[s], s); GpuStamp(g, g.list, 1);
        GpuStamp(g, g.list, 2); XeFlowEstimate(xf, g.list, s, s ^ 1, 1); GpuStamp(g, g.list, 3);
        GpuStamp(g, g.list, 4); XeFlowEstimate(xf, g.list, s ^ 1, s, 0); GpuStamp(g, g.list, 5);
        GpuStamp(g, g.list, 7);
        if (!GpuEnd(g) || !GpuWaitIdle(g)) return 1;
        double ms[4];
        if (i >= 10 && GpuStampsMsSlot(g, slot, ms, 4)) for (int k = 0; k < 4; ++k) t[k].push_back(ms[k]);
    }
    auto med = [](std::vector<double> v) { if (v.empty()) return -1.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    auto p95 = [](std::vector<double> v) { if (v.empty()) return -1.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, v.size() * 95 / 100)]; };
    Log("[bench] %ls, %ux%u, L0 %ux%u (factor %u), %d levels, grid %ux%u, lambda %.4f",
        ad.Description, w, h, XeFlowL0W(xf), XeFlowL0H(xf), f, XeFlowLevels(xf), gw, gh, lambda);
    Log("[bench] GPU ms per frame (median / p95 of %zu): pyramid %.3f / %.3f  backward %.3f / %.3f  forward %.3f / %.3f  total %.3f / %.3f",
        t[3].size(), med(t[0]), p95(t[0]), med(t[1]), p95(t[1]), med(t[2]), p95(t[2]), med(t[3]), p95(t[3]));

    // ---- --profile: where the GPU time of one real frame and one presented frame goes ----
    // Stamps: pair 0 pyramid, 1..levels the backward flow per level (finest first), then a full-frame
    // copy (what OverlayPresent pays per present), the interpolation pass, and the probe's thumbnail.
    if (profile)
    {
        ID3D12Resource* rtt = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, NPSR, L"prof_rt");
        ID3D12Resource* stat = GpuMakeTex(g, XeFlowL0W(xf), XeFlowL0H(xf), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"prof_static");
        XeInterp xi; ComputePso probe; ID3D12Resource* tiny = GpuMakeTex(g, (w + 7) / 8, (h + 7) / 8, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"prof_tiny");
        if (!XeInterpInit(g, xi) || !tiny || !GpuMakeCompute(g, g_cs_gray, sizeof g_cs_gray, 1, 1, 6, probe, L"prof_probe")) return 1;
        const int L = XeFlowLevels(xf), pc = 1 + L + 4;
        std::vector<std::vector<double>> pt(pc);
        XeFlowSetStamps(xf, 2);
        for (int i = 0; i < frames; ++i)
        {
            if (!GpuBegin(g)) return 1;
            const int slot = g.slot, s = i & 1;
            ID3D12GraphicsCommandList* cl = g.list;
            GpuStamp(g, cl, 0); XeFlowPyramid(xf, cl, tex[s], s); GpuStamp(g, cl, 1);
            XeFlowEstimate(xf, cl, s, s ^ 1, 1);
            XeFlowStaticMap(xf, cl, s, s ^ 1, stat);
            const int b = 2 + 2 * L;
            GpuStamp(g, cl, b);
            GpuBarrier(cl, tex[s], NPSR, D3D12_RESOURCE_STATE_COPY_SOURCE); GpuBarrier(cl, recon, NPSR, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(recon, tex[s]);
            GpuBarrier(cl, tex[s], D3D12_RESOURCE_STATE_COPY_SOURCE, NPSR); GpuBarrier(cl, recon, D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
            GpuStamp(g, cl, b + 1);
            GpuStamp(g, cl, b + 2);
            XeInterpRecord(g, cl, xi, tex[0], tex[1], XeFlowGrid(xf, 0), XeFlowGrid(xf, 1), stat, gw, gh, f, cell, recon, w, h, 0.5f);
            GpuStamp(g, cl, b + 3);
            GpuStamp(g, cl, b + 4);
            const UINT pcn[6] = { w, h, (w + 7) / 8, (h + 7) / 8, 8, 8 };
            const GpuView ps = { tex[s], DXGI_FORMAT_UNKNOWN }, pu = { tiny, DXGI_FORMAT_UNKNOWN };
            GpuBarrier(cl, tiny, NPSR, UAV); GpuDispatch(g, cl, probe, &ps, &pu, pcn, GpuGroups(pcn[2], 8), GpuGroups(pcn[3], 8)); GpuBarrier(cl, tiny, UAV, NPSR);
            GpuStamp(g, cl, b + 5);
            GpuStamp(g, cl, b + 6);
            XeInterpRecordRT(g, cl, xi, tex[0], tex[1], XeFlowGrid(xf, 0), XeFlowGrid(xf, 1), stat, gw, gh, f, cell, rtt, NPSR, w, h, 0.5f);
            GpuStamp(g, cl, b + 7);
            if (!GpuEnd(g) || !GpuWaitIdle(g)) return 1;
            double ms[16];
            if (i >= 10 && GpuStampsMsSlot(g, slot, ms, pc)) for (int k = 0; k < pc; ++k) pt[k].push_back(ms[k]);
        }
        XeFlowSetStamps(xf, -1);
        {   // the probe's thumbnail from an ordinary texture: must not be black
            std::vector<uint8_t> tp((size_t)((w + 7) / 8) * ((h + 7) / 8));
            if (GpuReadbackTex(g, tiny, tp.data(), (w + 7) / 8, (h + 7) / 8, 1, NPSR))
            { double m = 0; for (uint8_t v : tp) m += v; Log("[profile] probe thumbnail mean luma %.1f", m / tp.size()); }
        }
        std::string lv;
        for (int k = 0; k < L; ++k) { char tmp[48]; sprintf_s(tmp, "L%d %.3f  ", k, med(pt[1 + k])); lv += tmp; }
        double flow_sum = 0; for (int k = 0; k < L; ++k) flow_sum += med(pt[1 + k]);
        Log("[profile] per real frame: pyramid %.3f ms, backward flow %.3f ms by level: %s", med(pt[0]), flow_sum, lv.c_str());
        Log("[profile] per present: full-frame copy %.3f ms, interpolation %.3f ms (compute) / %.3f ms (draw) | probe thumbnail (full read) %.3f ms",
            med(pt[1 + L]), med(pt[2 + L]), med(pt[4 + L]), med(pt[3 + L]));
        {
            // the draw must match the compute version pixel for pixel (same shader body, same samples)
            std::vector<uint8_t> a((size_t)w * h * 4), b2(a.size());
            if (GpuReadbackTex(g, recon, a.data(), w, h, 4, NPSR) && GpuReadbackTex(g, rtt, b2.data(), w, h, 4, NPSR))
            {
                int maxd = 0; size_t diff = 0;
                for (size_t i = 0; i < a.size(); ++i) { const int d = abs((int)a[i] - (int)b2[i]); maxd = std::max(maxd, d); diff += d > 0; }
                Log("[profile] draw vs compute output: %zu of %zu values differ, max difference %d", diff, a.size(), maxd);
            }
        }
        REL(rtt); REL(stat); REL(tiny); REL(probe.pso); REL(probe.root); XeInterpRelease(xi);
    }

    REL(tex[0]); REL(tex[1]); REL(recon); REL(warp.pso); REL(warp.root);
    XeFlowDestroy(xf);
    GpuShutdown(g);
    return 0;
}

// ---- interpolation bench --------------------------------------------------------------------------------
// --interp N on a sequence: real frames k*N and (k+1)*N are the pair, frames k*N + j (j = 1..N-1) are
// the ground truth for t = j/N. On the 90 fps scene export, N = 3 is a 30 fps base at 3x and N = 2 a
// 45 fps base at 2x. Every generated frame is scored against two baselines: repeating the nearest
// real frame (what no frame generation shows) and a plain crossfade.
static int RunInterpBench(int argc, char** argv)
{
    std::wstring input; UINT size_w = 0, size_h = 0, factor = 2; int n = 3, pairs = 1 << 30, adapter = -1, refine = 1, fine = 1, sub = 1; UINT vendor = 0;
    bool extrap = false;   // --extrap: predict the frames AFTER the pair (cur pushed ahead), truth = the real ones
    bool ui = false;       // --ui: a static synthetic HUD on every frame, and PSNR inside it reported apart
    bool dump = false; XeInterp xi;
    for (int i = 1; i < argc; ++i)
    {
        const bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--interp") && more) n = std::clamp(atoi(argv[++i]), 2, 8);
        else if (!strcmp(argv[i], "--seq") && more) input = Widen(argv[++i]);
        else if (!strcmp(argv[i], "--size") && more) sscanf_s(argv[++i], "%ux%u", &size_w, &size_h);
        else if (!strcmp(argv[i], "--factor") && more) factor = (UINT)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--refine") && more) refine = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fine") && more) fine = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sub") && more) sub = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pairs") && more) pairs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tol") && more) xi.tol = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--tolrel") && more) xi.tol_rel = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--extrap")) extrap = true;
        else if (!strcmp(argv[i], "--ui")) ui = true;
        else if (!strcmp(argv[i], "--adapter") && more) adapter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vendor") && more) vendor = (UINT)strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--dump")) dump = true;
    }
    const std::wstring dir = ExeDir();
    LogInit((dir + L"\\xe_bench.log").c_str());
    const std::vector<std::wstring> names = ListFrames(input);
    if (names.size() < (size_t)n + 1) { Log("[interp] %ls: need at least %d PNGs", input.c_str(), n + 1); return 1; }
    std::vector<Image> fr(n + 1);
    if (!Load(input + L"\\" + names[0], fr[0], size_w, size_h)) return 1;
    const UINT w = fr[0].w, h = fr[0].h;
    std::vector<uint8_t> ui_mask;   // --ui: pixels the HUD covers
    if (ui)
    {
        MakeUi(w, h); CompositeUi(fr[0]);
        ui_mask.resize((size_t)w * h);
        for (size_t i = 0; i < ui_mask.size(); ++i) ui_mask[i] = g_ui.rgba[i * 4 + 3] != 0;
    }

    Gpu g;
    if (!GpuInit(g, adapter, vendor)) return 1;
    XeFlow* xf = XeFlowCreate(g, w, h, factor);
    if (!xf || !XeInterpInit(g, xi)) return 1;
    XeFlowSetRefine(xf, refine); XeFlowSetFine(xf, fine); XeFlowSetSubsel(xf, sub != 0);
    ID3D12Resource* tex[2] = { GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, NPSR, L"interp_a"),
                               GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, NPSR, L"interp_b") };
    ID3D12Resource* out = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"interp_out");
    ID3D12Resource* stat = GpuMakeTex(g, XeFlowL0W(xf), XeFlowL0H(xf), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"interp_static");
    if (!tex[0] || !tex[1] || !out) return 1;
    Log("[interp] %ls: %zu frames of %ux%u, base = every %dth frame, %d generated per pair, tol %.2f + %.2f x motion",
        input.c_str(), names.size(), w, h, n, n - 1, xi.tol, xi.tol_rel);

    std::vector<double> ours[8], rep[8], mix[8], cost;   // per j
    std::vector<double> ui_ours, ui_rep;                 // --ui: PSNR inside the HUD
    std::vector<uint8_t> px((size_t)w * h * 4), blend((size_t)w * h * 4);
    int done = 0;
    for (size_t k = 0; (k + 1) * n < names.size() && done < pairs; ++k, ++done)
    {
        if (k > 0) fr[0] = std::move(fr[n]);
        bool ok = true;
        for (int j = 1; j <= n && ok; ++j) ok = Load(input + L"\\" + names[k * n + j], fr[j], size_w, size_h) && fr[j].w == w && fr[j].h == h;
        if (!ok) { Log("[interp] pair %zu: frames did not load", k); break; }
        if (!GpuUploadTex(g, tex[0], fr[0].px.data(), w, h, 4, NPSR) || !GpuUploadTex(g, tex[1], fr[n].px.data(), w, h, 4, NPSR)) return 1;
        for (int j = 1; j < n; ++j)
        {
            const float t = (float)j / n;
            if (!GpuBegin(g)) return 1;
            const int slot = g.slot;
            if (j == 1)
            {
                XeFlowPyramid(xf, g.list, tex[0], 0); XeFlowPyramid(xf, g.list, tex[1], 1);
                XeFlowEstimate(xf, g.list, 0, 1, 0); XeFlowEstimate(xf, g.list, 1, 0, 1); XeFlowStaticMap(xf, g.list, 1, 0, stat);
            }
            GpuStamp(g, g.list, 0);
            XeInterpRecord(g, g.list, xi, tex[0], tex[1], XeFlowGrid(xf, 0), XeFlowGrid(xf, 1), stat, XeFlowGridW(xf), XeFlowGridH(xf), XeFlowFactor(xf), XeFlowCell(xf), out, w, h, t,
                           false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, extrap);
            GpuStamp(g, g.list, 1);
            if (!GpuEnd(g) || !GpuWaitIdle(g) || !GpuReadbackTex(g, out, px.data(), w, h, 4, NPSR)) return 1;
            double ms; if (GpuStampsMsSlot(g, slot, &ms, 1) && ms >= 0) cost.push_back(ms);
            for (size_t i = 0; i < blend.size(); ++i) blend[i] = (uint8_t)((1 - t) * fr[0].px[i] + t * fr[n].px[i] + 0.5f);
            // Extrapolation is shown in place of the frame t intervals AFTER cur: that is its truth, and
            // repeating cur (what is on screen without frame generation) its baseline.
            Image future;
            if (extrap && !(k * n + n + j < names.size() && Load(input + L"\\" + names[k * n + n + j], future, size_w, size_h) && future.w == w)) continue;
            const Image& truth = extrap ? future : fr[j];
            ours[j].push_back(Psnr(truth, px, 16));
            rep[j].push_back(Psnr(truth, extrap ? fr[n].px : t <= 0.5f ? fr[0].px : fr[n].px, 16));
            mix[j].push_back(Psnr(truth, blend, 16));
            if (ui) { ui_ours.push_back(Psnr(truth, px, 16, &ui_mask)); ui_rep.push_back(Psnr(truth, extrap ? fr[n].px : t <= 0.5f ? fr[0].px : fr[n].px, 16, &ui_mask)); }
            if (dump)
            {
                wchar_t path[MAX_PATH];
                _snwprintf_s(path, _TRUNCATE, L"%ls\\xe_interp_%03zu_%d.png", dir.c_str(), k, j); SavePngRgba(path, px.data(), w, h);
                if (!GpuBegin(g)) return 1;
                XeInterpRecord(g, g.list, xi, tex[0], tex[1], XeFlowGrid(xf, 0), XeFlowGrid(xf, 1), stat, XeFlowGridW(xf), XeFlowGridH(xf), XeFlowFactor(xf), XeFlowCell(xf), out, w, h, t, true);
                if (!GpuEnd(g) || !GpuWaitIdle(g) || !GpuReadbackTex(g, out, px.data(), w, h, 4, NPSR)) return 1;
                _snwprintf_s(path, _TRUNCATE, L"%ls\\xe_vis_%03zu_%d.png", dir.c_str(), k, j); SavePngRgba(path, px.data(), w, h);
            }
        }
    }
    auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
    double all[3] = {}; int nall = 0;
    for (int j = 1; j < n; ++j)
    {
        Log("[interp] t = %d/%d over %zu pairs: ours %.2f dB | repeat nearest %.2f dB | crossfade %.2f dB",
            j, n, ours[j].size(), mean(ours[j]), mean(rep[j]), mean(mix[j]));
        all[0] += mean(ours[j]); all[1] += mean(rep[j]); all[2] += mean(mix[j]); ++nall;
    }
    if (ui) Log("[interp] inside the HUD (%.1f%% of the frame): ours %.2f dB | repeat nearest %.2f dB   (a perfect static UI is identical in every frame)",
                100.0 * std::count(ui_mask.begin(), ui_mask.end(), 1) / ui_mask.size(), mean(ui_ours), mean(ui_rep));
    std::sort(cost.begin(), cost.end());
    Log("[interp] all generated frames: ours %.2f dB (%+.2f vs repeat, %+.2f vs crossfade); interpolation pass %.3f ms median per generated frame (%ls)",
        all[0] / nall, (all[0] - all[1]) / nall, (all[0] - all[2]) / nall, cost.empty() ? -1.0 : cost[cost.size() / 2], L"flow not included");
    REL(tex[0]); REL(tex[1]); REL(out); REL(stat);
    XeInterpRelease(xi); XeFlowDestroy(xf); GpuShutdown(g);
    return 0;
}

// ---- live -------------------------------------------------------------------------------------------------
// The largest visible, uncloaked top-level window whose title contains `title` (and whose class is
// `cls`, when given) - the same rule as justflow.exe: launcher pages, thumbnails and tooltips carry
// game titles too, and the game window is bigger than all of them.
static HWND FindTarget(const std::wstring& title, const std::wstring& cls)
{
    HWND best = nullptr, h = nullptr; LONGLONG best_area = 0;
    while ((h = FindWindowExW(nullptr, h, cls.empty() ? nullptr : cls.c_str(), nullptr)) != nullptr)
    {
        if (!title.empty()) { wchar_t t[256] = {}; GetWindowTextW(h, t, 256); if (!wcsstr(t, title.c_str())) continue; }
        if (!IsWindowVisible(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) continue;
        if (GetWindowThreadProcessId(h, nullptr) && GetConsoleWindow() == h) continue;   // not our own console
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) continue;
        RECT r = {}; WINDOWPLACEMENT wp = { sizeof wp };
        if (IsIconic(h) && GetWindowPlacement(h, &wp)) r = wp.rcNormalPosition; else if (!GetWindowRect(h, &r)) continue;
        const LONGLONG w = r.right - r.left, ht = r.bottom - r.top;
        if (w < 320 || ht < 240) continue;
        if (w * ht > best_area) { best_area = w * ht; best = h; }
    }
    return best;
}

// Every window FindTarget could pick with the right --window, largest first.
static void LogWindows()
{
    struct Win { std::wstring title; LONGLONG area; };
    std::vector<Win> v;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
    {
        wchar_t t[256] = {}; if (!GetWindowTextW(h, t, 256) || !IsWindowVisible(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) continue;
        DWORD cloaked = 0; if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) continue;
        RECT r = {}; if (!GetWindowRect(h, &r) || r.right - r.left < 320 || r.bottom - r.top < 240) continue;
        v.push_back({ t, (LONGLONG)(r.right - r.left) * (r.bottom - r.top) });
    }
    std::sort(v.begin(), v.end(), [](const Win& a, const Win& b) { return a.area > b.area; });
    Log("[live] no match yet. Windows that --window can name (any part of the title):");
    for (const Win& x : v) Log("[live]   \"%ls\"", x.title.c_str());
}

static std::atomic<bool> g_quit{ false };
static BOOL WINAPI OnConsoleCtrl(DWORD) { g_quit = true; return TRUE; }

static int RunLive(int argc, char** argv)
{
    std::wstring title, cls; UINT factor = 2; bool fg_on = true, cursor = false; int adapter = -1; UINT vendor = 0;
    double margin = 0.0, max_in = 100.0;   // margin: ms added to the adaptive hold (negative trims it)
    double snap_s = -1;   // --snap S: save the first generated frame after S seconds (xe_snap.png)
    bool extrap = false;  // --extrap: push the newest frame ahead instead of interpolating behind it
    for (int i = 1; i < argc; ++i)
    {
        const bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--window") && more) title = Widen(argv[++i]);
        else if (!strcmp(argv[i], "--class") && more) cls = Widen(argv[++i]);
        else if (!strcmp(argv[i], "--factor") && more) factor = (UINT)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--margin") && more) margin = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-in") && more) max_in = atof(argv[++i]);
        else if (!strcmp(argv[i], "--snap") && more) snap_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--extrap")) extrap = true;
        else if (!strcmp(argv[i], "--adapter") && more) adapter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vendor") && more) vendor = (UINT)strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--off")) fg_on = false;
        else if (!strcmp(argv[i], "--cursor")) cursor = true;
    }
    if (title.empty() && cls.empty()) { fprintf(stderr, "--live needs --window <title substring> and/or --class <window class>\n"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool timer_1ms = timeBeginPeriod(1) == TIMERR_NOERROR;   // timed waits at 1 ms, not 15.6 (see justflow.exe's RealMain)
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    const std::wstring dir = ExeDir();
    LogInit((dir + L"\\xe_live.log").c_str());
    Log("[live] window title~\"%ls\" class=\"%ls\", flow factor %u, generation %s, margin %.1f ms, max in %.0f fps%s",
        title.c_str(), cls.c_str(), factor, fg_on ? "on" : "off", margin, max_in, timer_1ms ? "" : " (timeBeginPeriod(1) failed)");
    Log("[live] keys: F8 generation on/off, Ctrl+F12 quit (Ctrl+C in this console too)");
    Gpu g;
    if (!GpuInit(g, adapter, vendor)) return 1;
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    const HotkeyDef keys[] = { { 1, 0, VK_F8 }, { 2, MOD_CONTROL, VK_F12 } };
    int rc = 0;
    while (!g_quit)
    {
        HWND target = nullptr;
        for (int i = 0; !g_quit && !(target = FindTarget(title, cls)); ++i)
        {
            if (i % 20 == 0) Log("[live] waiting for a window whose title contains \"%ls\"...", title.c_str());
            if (i == 4) LogWindows();   // after 2 s: show what could be matched, a wrong title is the usual cause
            Sleep(500);
        }
        if (!target) break;
        Capture* cap = CaptureOpen(g, target, cursor, false, false);
        if (!cap) { Sleep(1000); continue; }
        if (CaptureIsFloat(cap)) { Log("[live] FP16 (HDR) capture is not supported - exiting"); CaptureClose(cap); rc = 2; break; }
        const UINT w = CaptureWidth(cap), h = CaptureHeight(cap);
        XeFg* fg = XeFgCreate(g, target, w, h, factor, keys, 2);
        if (!fg) { CaptureClose(cap); rc = 1; break; }
        Overlay* ov = XeFgOverlay(fg);
        XeFgSetEnabled(fg, fg_on); XeFgSetTiming(fg, margin, max_in); XeFgSetExtrapolate(fg, extrap);
        Log("[live] attached to %p, %ux%u, display %.1f Hz", (void*)target, w, h, 1000.0 / OverlayVBlankMs(ov));

        bool reset = true; LONGLONG last_sysrel = 0; double stats_t = NowMs(); const double attach_t = stats_t; bool snapped = snap_s < 0;
        while (!g_quit)
        {
            if (OverlayHotkey(ov, 2)) { Log("[live] quit hotkey"); g_quit = true; break; }
            if (OverlayHotkey(ov, 1)) { fg_on = !fg_on; XeFgSetEnabled(fg, fg_on); reset = true; Log("[live] generation %s", fg_on ? "on" : "off"); }
            if (CaptureLost(cap)) { Log("[live] capture lost - waiting for the window again"); break; }
            UINT nw, nh;
            if (CaptureSizeChanged(cap, nw, nh)) { Log("[live] window is now %ux%u - reattaching", nw, nh); break; }
            if (XeFgFailed(fg)) { rc = 3; g_quit = true; break; }
            UINT64 fv = 0; LONGLONG sysrel = 0;
            if (!CaptureAcquire(cap, 50, fv, sysrel)) { OverlayFollow(ov, 300); continue; }   // a game that lost focus often stops rendering: still hide promptly
            const bool dda = CaptureIsDda(cap);
            if (last_sysrel && sysrel - last_sysrel > (dda ? qpf.QuadPart / 4 : 2500000)) reset = true;   // > 250 ms gap
            last_sysrel = sysrel;
            const LONGLONG cap_qpc = dda ? sysrel : sysrel / 10000000 * qpf.QuadPart + (sysrel % 10000000) * qpf.QuadPart / 10000000;
            if (!XeFgSubmit(fg, CaptureTexture(cap), CaptureFence(cap), fv, QpcToMs(cap_qpc), reset)) { GpuLogDeviceRemoved(g, "submit"); rc = 3; g_quit = true; break; }
            reset = false;
            OverlayFollow(ov, 300);
            const double now = NowMs();
            if (!snapped)
            {
                std::vector<uint8_t> px; float st = 0;
                if (XeFgTakeSnapshot(fg, px, st))
                {
                    const std::wstring path = dir + L"\\xe_snap.png";
                    snapped = true; SavePngRgba(path.c_str(), px.data(), w, h);
                    Log("[live] snapshot: generated frame at t = %.3f -> %ls", st, path.c_str());
                }
                else if (now - attach_t >= snap_s * 1000.0) XeFgRequestSnapshot(fg);
            }
            if (now - stats_t >= 2000.0)
            {
                XeFgStatsOut s; XeFgStats(fg, s);
                const double dt = (now - stats_t) / 1000.0; stats_t = now;
                std::vector<double>& sp = s.spacing_ms; std::sort(sp.begin(), sp.end());
                auto q = [&](double p) { return sp.empty() ? -1.0 : sp[std::min(sp.size() - 1, (size_t)(sp.size() * p))]; };
                const double vbm = OverlayVBlankMs(ov); size_t late = 0;
                for (double x : sp) late += x > vbm * 1.5;
                Log("[stats] in %.1f fps  out %.1f fps (gen %u, ext %u, held %u, early %u)%s | flow %.2f ms  interp %.2f ms | hold %.1f ms (learned %.1f)  interval %.1f ms | spacing p5 %.2f  med %.2f  p95 %.2f ms, %.1f%% > 1.5 vblank",
                    s.in / dt, s.presented / dt, s.generated, s.extrapolated, s.held, s.early, s.passthrough ? " PASSTHROUGH" : "",
                    s.flow_ms, s.interp_ms, s.hold_ms, s.extra_ms, s.interval_ms, q(0.05), q(0.5), q(0.95), sp.empty() ? 0.0 : 100.0 * late / sp.size());
            }
        }
        XeFgDestroy(fg); CaptureClose(cap);   // XeFgDestroy takes the overlay down
    }
    GpuShutdown(g);
    if (timer_1ms) timeEndPeriod(1);
    return rc;
}

static int CliMain(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--live")) return RunLive(argc, argv);
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--interp")) return RunInterpBench(argc, argv);
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--bench")) return RunBench(argc, argv);
    fprintf(stderr, "justflow_xe: frame generation for any D3D12 GPU (work in progress)\n"
                    "  --bench <dir|png> [--size WxH] [--factor N] [--lambda X] [--refine N] [--frames N] [--pairs N] [--dump]\n"
                    "          [--shift DX,DY] [--zoom Z] [--adapter I] [--vendor HEX]\n"
                    "  --interp N --seq <dir> [--size WxH] [--pairs K] [--tol PX] [--tolrel X] [--refine N] [--extrap] [--ui] [--dump]\n"
                    "  --live --window <title substring> [--class C] [--off] [--factor N] [--margin MS] [--max-in FPS] [--cursor] [--snap S] [--extrap]\n");
    return 2;
}

#ifdef XE_CONSOLE
// justflow_xe_cli.exe: the console build, for --bench / --interp / --live (a windowed exe is not
// waited for by shells and cannot write to a redirected stdout). No arguments = the tray app here too.
int main(int argc, char** argv)
{
    if (argc <= 1) return RunTrayApp(ExeDir());
    return CliMain(argc, argv);
}
#else
// justflow_xe.exe, windowed subsystem: the tray app, no console window. Given arguments it still runs
// the CLI modes, attached to the parent console (justflow_xe_cli.exe is the one to script).
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0; wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc <= 1) { if (wargv) LocalFree(wargv); return RunTrayApp(ExeDir()); }
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr);
    }
    std::vector<std::string> args; std::vector<char*> argv;   // ASCII arguments, like justflow.exe
    for (int i = 0; i < argc; ++i) { std::string a; for (const wchar_t* w = wargv[i]; *w; ++w) a.push_back((char)*w); args.push_back(a); }
    for (auto& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    LocalFree(wargv);
    return CliMain(argc, argv.data());
}
#endif
