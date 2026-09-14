// nrfilter: capture a window -> DLSS 5 neural rendering -> click-through overlay.
//   nrfilter [--dump N]                      live mode (target from nrfilter.ini)
//   nrfilter --bench <png|dir> [--frames N] [--work WxH] [--no-present]
#include "pipeline.h"
#include "capture.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int  RunBench(int argc, char** argv);                                             // bench.cpp
bool SavePngRgba(const wchar_t* path, const uint8_t* rgba, UINT w, UINT h);       // bench.cpp

const char* const kPipeStageName[PS_COUNT] = { "swizzle", "gray+downscale", "ofa", "expand", "eval", "compose" };

double StageStats::pct(double p) const
{
    if (v.empty()) return -1;
    std::vector<double> s = v; std::sort(s.begin(), s.end());
    size_t i = (size_t)(p * (double)s.size()); if (i >= s.size()) i = s.size() - 1;
    return s[i];
}

std::wstring ExeDir()
{
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (wchar_t* s = wcsrchr(p, L'\\')) *s = 0;
    return p;
}

void ResolveWork(Config& c, const std::wstring& dir)
{
    if (c.work_w && c.work_h) return;
    const std::wstring ini = dir + L"\\nrfilter.spike.ini";
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"spike", L"work", L"", buf, 64, ini.c_str());
    if (swscanf_s(buf, L"%ux%u", &c.work_w, &c.work_h) == 2 && c.work_w && c.work_h)
    {
        c.create_style = (int)GetPrivateProfileIntW(L"spike", L"create_style", c.create_style, ini.c_str());
        c.param_block = (int)GetPrivateProfileIntW(L"spike", L"param_block", c.param_block, ini.c_str());
        Log("[nr] work %ux%u style %d block %d from nrfilter.spike.ini", c.work_w, c.work_h, c.create_style, c.param_block);
    }
    else { c.work_w = 2560; c.work_h = 1440; Log("[nr] work auto -> 2560x1440 (no nrfilter.spike.ini)"); }
}

// ---- pipeline ---------------------------------------------------------------------------------------
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const D3D12_RESOURCE_FLAGS FUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

static bool AllocNative(Pipeline* p)
{
    Gpu& g = *p->g;
    p->color4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, NPSR, L"color4k");
    p->gray = GpuMakeTex(g, p->gw, p->gh, DXGI_FORMAT_R8_UNORM, FUAV, UAV, L"gray");
    p->out4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, UAV, L"out4k");
    if (p->w % p->gw || p->h % p->gh) Log("[main] warning: gray block %ux%u -> %ux%u is not integer", p->w, p->h, p->gw, p->gh);
    return p->color4k && p->gray && p->out4k;
}

static bool AllocWork(Pipeline* p)
{
    Gpu& g = *p->g;
    p->ww = p->cfg.work_w; p->wh = p->cfg.work_h;
    p->nr_in = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, NPSR, L"nr_in");
    p->nr_out = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, UAV, L"nr_out");
    p->mv = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R16G16_FLOAT, FUAV, NPSR, L"mv");
    return p->nr_in && p->nr_out && p->mv;
}

Pipeline* PipelineCreate(Gpu& g, const Config& cfg, UINT w, UINT h, bool with_overlay, HWND target)
{
    Pipeline* p = new Pipeline();
    p->g = &g; p->cfg = cfg; p->w = w; p->h = h;
    // CsGray needs an integer block: round the block, derive the gray size from it.
    const UINT bx = std::max(1u, (UINT)std::lround((double)w / cfg.ofa_w)), by = std::max(1u, (UINT)std::lround((double)h / cfg.ofa_h));
    p->gw = w / bx; p->gh = h / by;
    if (p->gw != cfg.ofa_w || p->gh != cfg.ofa_h) Log("[main] ofa input %ux%u adjusted to %ux%u (block %ux%u)", cfg.ofa_w, cfg.ofa_h, p->gw, p->gh, bx, by);
    if (with_overlay)
    {
        const HotkeyDef keys[4] = { { 1, cfg.hk_toggle.mods, cfg.hk_toggle.vk }, { 2, cfg.hk_wipe.mods, cfg.hk_wipe.vk },
                                    { 3, cfg.hk_reload.mods, cfg.hk_reload.vk }, { 4, cfg.hk_quit.mods, cfg.hk_quit.vk } };
        p->ov = OverlayCreate(g, target, w, h, keys, 4, cfg.exclude_from_capture);
        if (!p->ov) { PipelineDestroy(p); return nullptr; }
    }
    p->sh = ShadersCreate(g);
    if (!p->sh) { PipelineDestroy(p); return nullptr; }
    if (cfg.nr_enabled)
    {
        p->nr = NrInit(g, ExeDir().c_str(), (NrParamBlock)cfg.param_block);
        if (!p->nr) { PipelineDestroy(p); return nullptr; }
        p->create_pending = true;
    }
    p->ofa = OfaCreate(g, p->gw, p->gh, cfg.ofa_grid, cfg.ofa_dll.empty() ? nullptr : cfg.ofa_dll.c_str());
    if (!p->ofa) { PipelineDestroy(p); return nullptr; }
    if (!AllocNative(p) || !AllocWork(p)) { PipelineDestroy(p); return nullptr; }
    return p;
}

void PipelineDestroy(Pipeline* p)
{
    if (!p) return;
    GpuWaitIdle(*p->g);
    REL(p->color4k); REL(p->gray); REL(p->out4k); REL(p->nr_in); REL(p->nr_out); REL(p->mv);
    if (p->ofa) OfaDestroy(p->ofa);
    if (p->nr) NrShutdown(p->nr);
    if (p->sh) ShadersDestroy(p->sh);
    if (p->ov) OverlayDestroy(p->ov);
    delete p;
}

bool PipelineResize(Pipeline* p, UINT w, UINT h)
{
    GpuWaitIdle(*p->g);
    REL(p->color4k); REL(p->gray); REL(p->out4k);
    p->w = w; p->h = h;
    p->force_reset = true;
    if (!AllocNative(p)) return false;
    return !p->ov || OverlayResize(p->ov, w, h);
}

void PipelineReload(Pipeline* p, const Config& c)
{
    const bool rebuild = ConfigNeedsRebuild(p->cfg, c);
    p->cfg = c;
    if (rebuild && p->nr)
    {
        p->rebuild_countdown = std::max(1, c.rebuild_debounce_frames);
        Log("[nr] create-latched key changed - rebuild in %d frames", p->rebuild_countdown);
    }
}

void PipelineReadStamps(Pipeline* p)
{
    Gpu& g = *p->g;
    // same slot selection as GpuStampsMs, so the fence value identifies the slot for dedupe
    int s = (g.slot + Gpu::kFrames - 1) % Gpu::kFrames; UINT64 v = 0;
    for (int t = 0; t < Gpu::kFrames; ++t, s = (s + Gpu::kFrames - 1) % Gpu::kFrames)
    {
        v = g.alloc_fence[s];
        if (v && g.fence->GetCompletedValue() >= v) break;
        v = 0;
    }
    if (!v || v == p->last_stamp_fence) return;
    double ms[5];
    if (!GpuStampsMs(g, ms, 5)) return;
    p->last_stamp_fence = v;
    p->st[PS_SWIZZLE].add(ms[0]); p->st[PS_GRAYDS].add(ms[1]); p->st[PS_EVAL].add(ms[2]); p->st[PS_COMPOSE].add(ms[3]); p->st[PS_EXPAND].add(ms[4]);
}

bool PipelineFrame(Pipeline* p, ID3D12Resource* cap, ID3D12Fence* wait_fence, UINT64 wait_value, bool reset)
{
    Gpu& g = *p->g; const Config& c = p->cfg; ID3D12GraphicsCommandList* cl = g.list;
    p->last_evaluated = false;
    if (p->force_reset) { reset = true; p->force_reset = false; }
    if (p->rebuild_countdown > 0 && --p->rebuild_countdown == 0)
    {
        if (p->ww != c.work_w || p->wh != c.work_h) { GpuWaitIdle(g); REL(p->nr_in); REL(p->nr_out); REL(p->mv); if (!AllocWork(p)) return false; }
        p->create_pending = true; reset = true;
    }
    auto stamp = [&](int i) { if (c.gpu_timestamps) GpuStamp(g, cl, i); };

    // ---- list 1: swizzle, gray, downscale, gray -> OFA input --------------------------------------
    if (wait_fence) g.queue->Wait(wait_fence, wait_value);
    if (!GpuBegin(g)) return false;
    GpuBarrier(cl, cap, D3D12_RESOURCE_STATE_COMMON, NPSR);
    GpuBarrier(cl, p->color4k, NPSR, UAV);
    stamp(0); CsSwizzle(g, p->sh, cl, cap, p->color4k, p->w, p->h); stamp(1);
    GpuBarrier(cl, p->color4k, UAV, NPSR);
    GpuBarrier(cl, p->nr_in, NPSR, UAV);
    stamp(2);
    CsGray(g, p->sh, cl, p->color4k, p->w, p->h, p->gray, p->gw, p->gh);
    CsDownscale(g, p->sh, cl, p->color4k, p->w, p->h, p->nr_in, p->ww, p->wh);
    stamp(3);
    GpuBarrier(cl, p->nr_in, UAV, NPSR);
    GpuBarrier(cl, p->gray, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource* oin = OfaInput(p->ofa, OfaCurrent(p->ofa));
    GpuBarrier(cl, oin, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(oin, p->gray);
    GpuBarrier(cl, oin, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    GpuBarrier(cl, p->gray, D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
    GpuBarrier(cl, cap, NPSR, D3D12_RESOURCE_STATE_COMMON);
    const UINT64 f1 = GpuEnd(g);
    if (!f1) return false;
    PipelineReadStamps(p);

    // ---- optical flow ----------------------------------------------------------------------------
    double ofa_t0 = 0;
    if (p->measure_ofa) { GpuWait(g, g.fence, f1, 5000); ofa_t0 = NowMs(); }
    if (!OfaExecute(p->ofa, g.fence, f1, reset)) { Log("[ofa] execute failed"); return false; }
    if (p->measure_ofa) { GpuWait(g, OfaFence(p->ofa), OfaFenceValue(p->ofa), 5000); p->st[PS_OFA].add(NowMs() - ofa_t0); }
    g.queue->Wait(OfaFence(p->ofa), OfaFenceValue(p->ofa));

    // ---- list 2: expand, (create | evaluate), compose, copy to backbuffer -------------------------
    if (!GpuBegin(g)) return false;
    ID3D12Resource* flow = OfaFlow(p->ofa);
    ID3D12Resource* cost = c.cost_reject ? OfaCost(p->ofa) : nullptr;
    GpuBarrier(cl, flow, D3D12_RESOURCE_STATE_COMMON, NPSR);
    if (cost) GpuBarrier(cl, cost, D3D12_RESOURCE_STATE_COMMON, NPSR);
    GpuBarrier(cl, p->mv, NPSR, UAV);
    stamp(8);
    CsExpand(g, p->sh, cl, flow, cost, OfaFlowWidth(p->ofa), OfaFlowHeight(p->ofa), p->gw, p->gh, p->mv, p->ww, p->wh, c.zero_below, c.cost_reject, reset);
    stamp(9);
    GpuBarrier(cl, p->mv, UAV, NPSR);
    GpuBarrier(cl, flow, NPSR, D3D12_RESOURCE_STATE_COMMON);
    if (cost) GpuBarrier(cl, cost, NPSR, D3D12_RESOURCE_STATE_COMMON);

    if (p->nr && !NrReady(p->nr) && p->create_pending)
    {
        p->create_pending = false;
        const bool ok = NrCreate(p->nr, cl, ConfigToNr(c));
        const UINT64 v = GpuEnd(g);
        if (!v) return false;
        if (ok) { NrMarkSubmitted(p->nr); p->evals_since_create = 0; p->force_reset = true; Log("[nr] create submitted %ux%u", p->ww, p->wh); }
        else Log("[nr] create failed (%s) - native passthrough until reload", NrLastError(p->nr));
        return true;   // no evaluate on the create list
    }

    bool evaluated = false;
    if (p->nr && NrReady(p->nr) && !p->bypass)
    {
        stamp(4);
        const unsigned r = NrEvaluate(p->nr, cl, p->nr_in, p->mv, p->nr_out, reset, c.exposure_scale);
        stamp(5);
        if (r == 1) { evaluated = true; ++p->evals_since_create; }
        else { static unsigned n = 0; if ((n++ % 120) == 0) Log("[nr] evaluate -> 0x%08X (%s) %s", r, NgxResultName(r), NrLastError(p->nr)); }
    }

    ComposeParams cp;
    cp.residual_strength = c.residual_strength; cp.feather = c.feather; cp.nrects = c.nrects;
    memcpy(cp.rects, c.rects, sizeof cp.rects);
    if (!evaluated || p->evals_since_create <= (UINT)std::max(0, c.warmup)) cp.wipe_mode = 2;
    else if (p->wipe == 1) { cp.wipe_mode = 1; cp.wipe_x = 0.5f; }
    else if (p->wipe == 2) { cp.wipe_mode = 1; cp.wipe_x = (float)fmod((NowMs() - p->wipe_t0) / 2000.0, 1.0); }
    GpuBarrier(cl, p->nr_out, UAV, NPSR);
    stamp(6);
    CsCompose(g, p->sh, cl, p->color4k, p->nr_in, p->nr_out, p->ww, p->wh, p->out4k, p->w, p->h, cp);
    stamp(7);
    GpuBarrier(cl, p->nr_out, NPSR, UAV);
    if (p->ov)
    {
        ID3D12Resource* bb = OverlayBackbuffer(p->ov);
        GpuBarrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        GpuBarrier(cl, p->out4k, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyResource(bb, p->out4k);
        GpuBarrier(cl, p->out4k, D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
        GpuBarrier(cl, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (!GpuEnd(g)) return false;
    PipelineReadStamps(p);
    if (p->ov && !OverlayPresent(p->ov)) { GpuLogDeviceRemoved(g, "present"); return false; }
    if (evaluated) NrRetireTick(p->nr);
    p->last_evaluated = evaluated;
    return true;
}

// ---- live mode ----------------------------------------------------------------------------------------
static HWND FindTarget(const Config& c)
{
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, c.window_class.empty() ? nullptr : c.window_class.c_str(), nullptr)) != nullptr)
    {
        if (c.window_title.empty()) return h;
        wchar_t t[256] = {}; GetWindowTextW(h, t, 256);
        if (wcsstr(t, c.window_title.c_str())) return h;
    }
    return nullptr;
}

static void DumpFrame(Pipeline* p, const std::wstring& dir, int i)
{
    std::vector<uint8_t> px((size_t)p->w * p->h * 4);
    wchar_t path[MAX_PATH];
    if (GpuReadbackTex(*p->g, p->color4k, px.data(), p->w, p->h, 4, NPSR))
    { swprintf_s(path, L"%ls\\dump_%03d_native.png", dir.c_str(), i); SavePngRgba(path, px.data(), p->w, p->h); }
    if (GpuReadbackTex(*p->g, p->out4k, px.data(), p->w, p->h, 4, UAV))
    { swprintf_s(path, L"%ls\\dump_%03d_out.png", dir.c_str(), i); SavePngRgba(path, px.data(), p->w, p->h); }
    Log("[main] dumped frame %d", i);
}

static std::wstring JoinPath(const std::wstring& dir, const std::wstring& f)
{
    return (f.size() > 1 && (f[1] == L':' || f[0] == L'\\')) ? f : dir + L"\\" + f;
}

int main(int argc, char** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int dump = 0;
    std::wstring ini_name = L"nrfilter.ini";   // --ini <file>: a per-game profile next to the exe
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--bench")) return RunBench(argc, argv);
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = atoi(argv[++i]);
        if (!strcmp(argv[i], "--ini") && i + 1 < argc) { const char* a = argv[++i]; ini_name.assign(a, a + strlen(a)); }
    }
    const std::wstring dir = ExeDir();
    Config cfg;
    const std::wstring ini_path = ini_name.find(L'\\') != std::wstring::npos ? ini_name : dir + L"\\" + ini_name;
    const bool have_ini = ConfigLoad(ini_path.c_str(), cfg);
    LogInit(JoinPath(dir, cfg.log_file).c_str());
    if (!have_ini) Log("[main] %ls not found - defaults in use", ini_path.c_str());
    Gpu g;
    if (!GpuInit(g, -1)) return 1;
    ResolveWork(cfg, dir);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);

    bool quit = false; int dumped = 0, rc = 0;
    while (!quit)
    {
        HWND target = nullptr;
        for (int i = 0; !(target = FindTarget(cfg)); ++i)
        {
            if (i % 30 == 0) Log("[main] waiting for window class=%ls title=%ls", cfg.window_class.c_str(), cfg.window_title.c_str());
            Sleep(1000);
        }
        Log("[main] target window %p", (void*)target);
        Capture* cap = CaptureOpen(g, target, cfg.cursor, cfg.border);
        if (!cap) { Sleep(1000); continue; }
        Pipeline* p = PipelineCreate(g, cfg, CaptureWidth(cap), CaptureHeight(cap), true, target);
        if (!p) { CaptureClose(cap); rc = 1; break; }

        bool reset = true;
        LONGLONG last_sysrel = 0;
        UINT frames = 0, skips = 0; int follow_tick = 0;
        double win_t0 = NowMs();
        std::vector<double> cpu_ms, lat_ms;
        for (;;)
        {
            if (OverlayHotkey(p->ov, 4)) { Log("[main] quit hotkey"); quit = true; break; }
            if (OverlayHotkey(p->ov, 1)) { p->bypass = !p->bypass; if (!p->bypass) p->force_reset = true; Log("[main] bypass %s", p->bypass ? "on" : "off"); }
            if (OverlayHotkey(p->ov, 2)) { p->wipe = (p->wipe + 1) % 3; p->wipe_t0 = NowMs(); Log("[main] wipe %d", p->wipe); }
            if (OverlayHotkey(p->ov, 3))
            {
                Config nc; ConfigLoad(ini_path.c_str(), nc);
                ResolveWork(nc, dir);
                PipelineReload(p, nc); cfg = nc;
                Log("[main] config reloaded");
            }
            if (CaptureLost(cap)) { Log("[main] capture lost - back to waiting for the window"); break; }
            UINT nw = 0, nh = 0;
            if (CaptureSizeChanged(cap, nw, nh))
            {
                Log("[main] window settled at %ux%u - recreating capture", nw, nh);
                GpuWaitIdle(g);
                CaptureClose(cap);
                cap = CaptureOpen(g, target, cfg.cursor, cfg.border);
                if (!cap || !PipelineResize(p, CaptureWidth(cap), CaptureHeight(cap))) { Log("[main] recreate failed"); break; }
                reset = true; last_sysrel = 0;
                continue;
            }
            UINT64 fv = 0; LONGLONG sysrel = 0;
            if (!CaptureAcquire(cap, 50, fv, sysrel))
            {
                ++skips;
                if ((++follow_tick % 10) == 0) OverlayFollow(p->ov, cfg.reassert_topmost_every);
                Sleep(1);
                continue;
            }
            if (CaptureIsFloat(cap)) { Log("[main] FP16 (HDR) capture is not supported in phase 1 - exiting"); quit = true; rc = 2; break; }
            const double t0 = NowMs();
            if (last_sysrel && sysrel - last_sysrel > 2500000) reset = true;   // > 250 ms gap
            last_sysrel = sysrel;
            if (!PipelineFrame(p, CaptureTexture(cap), CaptureFence(cap), fv, reset)) { Log("[main] frame failed - exiting"); GpuLogDeviceRemoved(g, "frame"); quit = true; rc = 3; break; }
            reset = false;
            OverlayFollow(p->ov, cfg.reassert_topmost_every);
            cpu_ms.push_back(NowMs() - t0);
            if (p->last_evaluated)
            {
                LONGLONG pq = 0, sq = 0; OverlayTimes(p->ov, pq, sq);
                if (pq) lat_ms.push_back((double)pq * 1000.0 / (double)qpf.QuadPart - (double)sysrel / 10000.0);
                if (dumped < dump) DumpFrame(p, dir, dumped++);
            }
            if (++frames % (UINT)std::max(1, cfg.stats_every) == 0)
            {
                StageStats cpu{ cpu_ms }, lat{ lat_ms };
                double gpu = 0;
                for (int s = 0; s < PS_COUNT; ++s) if (s != PS_OFA && p->st[s].med() > 0) gpu += p->st[s].med();
                Log("[stats] cap_fps=%.1f eval_ms=%.2f/%.2f(med/p95) frame_gpu_ms=%.2f cpu_ms=%.2f static_skips=%u latency_ms(cap->present)=%.1f",
                    frames * 1000.0 / (NowMs() - win_t0), p->st[PS_EVAL].med(), p->st[PS_EVAL].p95(), gpu, cpu.med(), skips, lat.med());
                frames = 0; skips = 0; win_t0 = NowMs(); cpu_ms.clear(); lat_ms.clear();
                for (auto& s : p->st) s.v.clear();
            }
        }
        PipelineDestroy(p);
        CaptureClose(cap);
    }
    GpuShutdown(g);
    return rc;
}
