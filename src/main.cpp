// JustFlow: capture a window -> DLSS 5 neural rendering -> click-through overlay.
//   justflow [--ini <file>] [--dump N]        live mode (profile from profiles\*.ini, see PickProfile)
//   justflow --bench <png|dir> [--frames N] [--work WxH] [--no-present]
#include "pipeline.h"
#include "capture.h"
#include "log.h"
#include "tray.h"
#include <shellapi.h>
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
    const std::wstring ini = dir + L"\\justflow.spike.ini";
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"spike", L"work", L"", buf, 64, ini.c_str());
    if (swscanf_s(buf, L"%ux%u", &c.work_w, &c.work_h) == 2 && c.work_w && c.work_h)
    {
        c.create_style = (int)GetPrivateProfileIntW(L"spike", L"create_style", c.create_style, ini.c_str());
        c.param_block = (int)GetPrivateProfileIntW(L"spike", L"param_block", c.param_block, ini.c_str());
        Log("[nr] work %ux%u style %d block %d from justflow.spike.ini", c.work_w, c.work_h, c.create_style, c.param_block);
    }
    else { c.work_w = 2560; c.work_h = 1440; Log("[nr] work auto -> 2560x1440 (no justflow.spike.ini)"); }
}

// Overlay hotkey ids: 1 toggle, 2 wipe, 3 reload, 4 quit, 5 fg (OverlayHotkey(p->ov, id) in main).
static void HotkeyDefs(const Config& c, HotkeyDef out[5])
{
    const HotkeyDef k[5] = { { 1, c.hk_toggle.mods, c.hk_toggle.vk }, { 2, c.hk_wipe.mods, c.hk_wipe.vk }, { 3, c.hk_reload.mods, c.hk_reload.vk },
                             { 4, c.hk_quit.mods, c.hk_quit.vk }, { 5, c.hk_fg.mods, c.hk_fg.vk } };
    memcpy(out, k, sizeof k);
}

// ---- pipeline ---------------------------------------------------------------------------------------
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const D3D12_RESOURCE_STATES COMMON = D3D12_RESOURCE_STATE_COMMON;
static const D3D12_RESOURCE_STATES CSRC = D3D12_RESOURCE_STATE_COPY_SOURCE, CDST = D3D12_RESOURCE_STATE_COPY_DEST;
static const D3D12_RESOURCE_FLAGS FUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

static bool AllocNative(Pipeline* p)
{
    Gpu& g = *p->g;
    p->color4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, NPSR, L"color4k");
    p->gray = GpuMakeTex(g, p->gw, p->gh, DXGI_FORMAT_R8_UNORM, FUAV, UAV, L"gray");
    p->out4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, D3D12_RESOURCE_STATE_COPY_SOURCE, L"out4k");
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

// ---- decoupled model track ([nr] mode=async) ---------------------------------------------------------
static bool AllocModel(Pipeline* p)
{
    Gpu& g = *p->g;
    if (!p->model_src) p->model_src = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, CDST, L"model_src");
    if (!p->nr_in_m)
    {
        p->nr_in_m = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, NPSR, L"nr_in_m");
        p->nr_out_m = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, UAV, L"nr_out_m");
        p->mv_m = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R16G16_FLOAT, FUAV, NPSR, L"mv_m");
        for (auto& r : p->residual) r = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R16G16B16A16_FLOAT, FUAV, NPSR, L"residual");
    }
    return p->model_src && p->nr_in_m && p->nr_out_m && p->mv_m && p->residual[0] && p->residual[1];
}
static void ReleaseModelWork(Pipeline* p) { REL(p->nr_in_m); REL(p->nr_out_m); REL(p->mv_m); REL(p->residual[0]); REL(p->residual[1]); }

static void SetModelParams(Pipeline* p, const Config& c)
{
    std::lock_guard<std::mutex> lk(p->model_mu);
    p->model_params = { c.zero_below, c.cost_reject, c.exposure_scale, c.model_max_fps, c.warmup };
}

// One iteration per handed-over frame: list A (downscale) -> model flow (OFA pair 1, held gray vs the
// previous model frame's) -> list B (expand, evaluate, residual). The next frame is requested as soon
// as the flow has consumed the held grays (the evaluate still runs), so the hand-off overlaps it.
static void ModelThread(Pipeline* p, bool create, NrConfig nc)
{
    Gpu& g = *p->g; GpuCtx& c = p->model_ctx; Nr* nr = p->nr; Ofa* ofa = p->ofa;
    auto fail = [&](const char* why) { Log("[model] stopped: %s", why); p->model_failed = true; };
    if (create && nr)
    {
        if (!GpuCtxBegin(g, c)) return fail("ctx begin (create)");
        const bool ok = NrCreate(nr, c.list, nc);
        const UINT64 v = GpuCtxEnd(c);
        if (!v) return fail("ctx end (create)");
        if (ok) { NrMarkSubmitted(nr); Log("[nr] create submitted %ux%u (model thread)", nc.work_w, nc.work_h); }
        else Log("[nr] create failed (%s) - native passthrough until reload", NrLastError(nr));
    }
    int prev_held = -1, j = 0; UINT evals = 0; double last_t = 0;
    for (;;)
    {
        Pipeline::ModelFrame fr; Pipeline::ModelParams mp;
        {
            std::unique_lock<std::mutex> lk(p->model_mu);
            p->model_cv.wait(lk, [&] { return p->model_stop || p->model_frame_ready; });
            if (p->model_stop) break;
            fr = p->model_frame; mp = p->model_params; p->model_frame_ready = false;
        }
        const bool reset = fr.reset || prev_held < 0;
        // list A: the handed-over native frame -> work res (after main's copies landed)
        c.queue->Wait(g.fence, fr.fence);
        if (!GpuCtxBegin(g, c)) return fail("ctx begin A");
        GpuBarrier(c.list, p->model_src, CDST, NPSR);
        GpuBarrier(c.list, p->nr_in_m, NPSR, UAV);
        CsDownscale(g, p->sh, c.list, p->model_src, p->w, p->h, p->nr_in_m, p->ww, p->wh);
        GpuBarrier(c.list, p->nr_in_m, UAV, NPSR);
        GpuBarrier(c.list, p->model_src, NPSR, CDST);
        const UINT64 fa = GpuCtxEnd(c);
        if (!fa) return fail("ctx end A");
        // The OFA engine runs executes in submission order: one submitted with a pending input fence
        // holds the per-frame flow behind it. List A is tiny and the ctx queue idle: wait for it here.
        if (!GpuCtxWait(c, c.fence, fa, 5000)) return fail("ctx fence wait A");
        const UINT64 ov = OfaExecuteRef(ofa, c.fence, fa, fr.held, reset ? fr.held : prev_held, 1, reset);
        if (!ov) return fail("ofa execute");
        c.queue->Wait(OfaFence(ofa), ov);
        // list B. residual[j] may still be read by an in-flight main list: wait for that fence first.
        c.queue->Wait(g.fence, p->residual_read_fence[j].load());
        if (!GpuCtxBegin(g, c)) return fail("ctx begin B");
        ID3D12Resource* flow = OfaFlow2(ofa); ID3D12Resource* cost = mp.cost_reject ? OfaCost2(ofa) : nullptr;
        GpuBarrier(c.list, flow, COMMON, NPSR); if (cost) GpuBarrier(c.list, cost, COMMON, NPSR);
        GpuBarrier(c.list, p->mv_m, NPSR, UAV);
        CsExpand(g, p->sh, c.list, flow, cost, OfaFlowWidth(ofa), OfaFlowHeight(ofa), p->gw, p->gh, p->mv_m, p->ww, p->wh, mp.zero_below, mp.cost_reject, reset);
        GpuBarrier(c.list, p->mv_m, UAV, NPSR);
        GpuBarrier(c.list, flow, NPSR, COMMON); if (cost) GpuBarrier(c.list, cost, NPSR, COMMON);
        bool evaluated = false;
        if (nr && NrReady(nr))
        {
            GpuCtxStamp(c, 0);
            const unsigned r = NrEvaluate(nr, c.list, p->nr_in_m, p->mv_m, p->nr_out_m, reset, mp.exposure);
            GpuCtxStamp(c, 1);
            if (r == 1) evaluated = true;
            else { static unsigned n = 0; if ((n++ % 120) == 0) Log("[nr] model evaluate -> 0x%08X (%s) %s", r, NgxResultName(r), NrLastError(nr)); }
        }
        if (evaluated)
        {
            GpuBarrier(c.list, p->nr_out_m, UAV, NPSR);
            GpuBarrier(c.list, p->residual[j], NPSR, UAV);
            CsResidual(g, p->sh, c.list, p->nr_in_m, p->nr_out_m, p->ww, p->wh, p->residual[j]);
            GpuBarrier(c.list, p->residual[j], UAV, NPSR);
            GpuBarrier(c.list, p->nr_out_m, NPSR, UAV);
        }
        const int slot_b = c.slot;
        const UINT64 fb = GpuCtxEnd(c);
        if (!fb) return fail("ctx end B");
        // flow done = both held grays and model_src are free: request the next frame (throttled first
        // so the frame taken is fresh) while the evaluate runs.
        if (!GpuCtxWait(c, OfaFence(ofa), ov, 5000)) return fail("ofa fence wait");
        prev_held = fr.held;
        if (mp.max_fps > 0) { const double due = last_t + 1000.0 / mp.max_fps, now = NowMs(); if (now < due) Sleep((DWORD)(due - now)); }
        last_t = NowMs();
        p->model_wants_frame = true;
        if (!GpuCtxWait(c, c.fence, fb, 5000)) return fail("ctx fence wait");
        if (!evaluated) continue;
        NrRetireTick(nr);
        ++evals; ++p->model_evals;
        double ms = -1; GpuCtxStampsMsSlot(c, slot_b, &ms, 1);
        std::lock_guard<std::mutex> lk(p->pub_mu);
        p->model_ms.add(ms);
        // publish only past the warm-up (the evaluates still run to build the temporal history)
        if (evals > (UINT)std::max(0, mp.warmup)) { p->pub_idx = j; p->pub_fence = fb; p->pub_frame = fr.index; j ^= 1; }
    }
}

static bool StartModel(Pipeline* p)
{
    if (!p->model_ctx.queue && !GpuCtxInit(*p->g, p->model_ctx, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, L"model")) return false;
    if (!AllocModel(p)) return false;
    p->model_stop = false; p->model_frame_ready = false; p->model_failed = false; p->model_wants_frame = true; p->model_reset_pending = true;
    p->pub_idx = -1; p->cmp_idx = -1; p->cmp_fence = 0;
    SetModelParams(p, p->cfg);
    const bool create = p->create_pending; p->create_pending = false;
    p->model_thread = std::thread(ModelThread, p, create, ConfigToNr(p->cfg));
    Log("[model] thread started%s", create ? " (creates the NR feature)" : "");
    return true;
}

static void StopModel(Pipeline* p)
{
    if (!p->model_thread.joinable()) return;
    { std::lock_guard<std::mutex> lk(p->model_mu); p->model_stop = true; }
    p->model_cv.notify_all();
    p->model_thread.join();
    GpuCtxWaitIdle(p->model_ctx);
    p->pub_idx = -1; p->cmp_idx = -1;
    Log("[model] thread stopped");
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
        HotkeyDef keys[5]; HotkeyDefs(cfg, keys);
        p->ov = OverlayCreate(g, target, w, h, keys, 5, cfg.exclude_from_capture, cfg.overlay_direct);
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
    StopModel(p);
    GpuWaitIdle(*p->g);
    if (p->fg) FgDestroy(p->fg);
    REL(p->color4k); REL(p->gray); REL(p->out4k); REL(p->nr_in); REL(p->nr_out); REL(p->mv);
    REL(p->model_src); ReleaseModelWork(p);
    if (p->model_ctx.queue) GpuCtxShutdown(*p->g, p->model_ctx);   // its queue may hold a Wait on the OFA fence: before OfaDestroy
    if (p->ofa) OfaDestroy(p->ofa);
    if (p->nr) NrShutdown(p->nr);
    if (p->sh) ShadersDestroy(p->sh);
    if (p->ov) OverlayDestroy(p->ov);
    delete p;
}

static void DropFg(Pipeline* p) { if (p->fg) { FgDestroy(p->fg); p->fg = nullptr; } }

bool PipelineResize(Pipeline* p, UINT w, UINT h)
{
    StopModel(p);   // restarted lazily by the next frame (model_src is native-sized)
    GpuWaitIdle(*p->g);
    DropFg(p);   // sized to the output; recreated on the next frame
    REL(p->color4k); REL(p->gray); REL(p->out4k); REL(p->model_src);
    p->w = w; p->h = h;
    p->force_reset = true;
    if (!AllocNative(p)) return false;
    return !p->ov || OverlayResize(p->ov, w, h);
}

void PipelineReload(Pipeline* p, const Config& c)
{
    const bool rebuild = ConfigNeedsRebuild(p->cfg, c);
    if (c.fg_multiplier != p->cfg.fg_multiplier || c.fg_pacing_vblank != p->cfg.fg_pacing_vblank || c.fg_mv_dilated != p->cfg.fg_mv_dilated) DropFg(p);   // recreated next frame
    if (c.overlay_direct != p->cfg.overlay_direct) Log("[overlay] mode=%s takes effect on the next capture open / restart (window recreate)", c.overlay_direct ? "direct" : "composed");
    if (c.nr_async != p->cfg.nr_async) { StopModel(p); p->model_failed = false; p->force_reset = true; Log("[nr] mode=%s", c.nr_async ? "async" : "sync"); }
    p->cfg = c;
    SetModelParams(p, c);   // live keys for the model thread (zero_below, cost_reject, exposure, model_max_fps, warmup)
    if (rebuild && p->nr)
    {
        p->rebuild_countdown = std::max(1, c.rebuild_debounce_frames);
        Log("[nr] create-latched key changed - rebuild in %d frames", p->rebuild_countdown);
    }
}

void PipelineReadStamps(Pipeline* p)
{
    Gpu& g = *p->g;
    for (int s = 0; s < Gpu::kFrames; ++s)
    {
        const UINT64 v = g.alloc_fence[s];
        if (!v || v == p->last_stamp_fence_slot[s]) continue;
        double ms[5];
        if (!GpuStampsMsSlot(g, s, ms, 5)) continue;
        p->last_stamp_fence_slot[s] = v;
        p->st[PS_SWIZZLE].add(ms[0]); p->st[PS_GRAYDS].add(ms[1]); p->st[PS_EVAL].add(ms[2]); p->st[PS_COMPOSE].add(ms[3]); p->st[PS_EXPAND].add(ms[4]);
    }
}

bool PipelineFrame(Pipeline* p, ID3D12Resource* cap, ID3D12Fence* wait_fence, UINT64 wait_value, bool reset)
{
    Gpu& g = *p->g; const Config& c = p->cfg; ID3D12GraphicsCommandList* cl = g.list;
    p->last_evaluated = false; ++p->frame_index;
    if (p->force_reset) { reset = true; p->force_reset = false; }
    if (p->rebuild_countdown > 0 && --p->rebuild_countdown == 0)
    {
        StopModel(p); p->model_failed = false;   // async: the restart below creates the new feature on the model thread
        if (p->ww != c.work_w || p->wh != c.work_h) { GpuWaitIdle(g); DropFg(p); REL(p->nr_in); REL(p->nr_out); REL(p->mv); ReleaseModelWork(p); if (!AllocWork(p)) return false; }
        p->create_pending = true; reset = true;
    }
    const bool async = c.nr_async && p->nr && !p->model_failed;
    if (async && !p->model_thread.joinable() && !StartModel(p)) return false;
    // FG lifecycle follows cfg.fg_enabled (ini, F8). A presenter failure turns the flag off; F8 retries.
    if (p->fg && (!c.fg_enabled || FgFailed(p->fg))) { if (FgFailed(p->fg)) p->cfg.fg_enabled = false; DropFg(p); }
    if (p->ov && c.fg_enabled && !p->fg)
    {
        p->fg = FgCreate(g, p->ov, ExeDir().c_str(), p->w, p->h, p->ww, p->wh, c.fg_multiplier, c.fg_pacing_vblank, c.fg_mv_dilated);
        if (!p->fg) p->cfg.fg_enabled = false;
    }
    if (p->fg) FgSetTiming(p->fg, c.fg_phase_ms, c.fg_anchor_delay_slots);   // ponytail: two atomic stores per frame, no reload plumbing
    auto stamp = [&](int i) { if (c.gpu_timestamps) GpuStamp(g, cl, i); };

    // ---- list 1: swizzle, gray, downscale, gray -> OFA input --------------------------------------
    if (wait_fence) g.queue->Wait(wait_fence, wait_value);
    double tw = NowMs();
    if (!GpuBegin(g)) return false;
    p->cpu_wait[0].add(NowMs() - tw);
    GpuBarrier(cl, cap, D3D12_RESOURCE_STATE_COMMON, NPSR);
    GpuBarrier(cl, p->color4k, NPSR, UAV);
    stamp(0); CsSwizzle(g, p->sh, cl, cap, p->color4k, p->w, p->h); stamp(1);
    GpuBarrier(cl, p->color4k, UAV, NPSR);
    GpuBarrier(cl, p->nr_in, NPSR, UAV);
    stamp(2);
    CsGray(g, p->sh, cl, p->color4k, p->w, p->h, p->gray, p->gw, p->gh);
    if (!async) CsDownscale(g, p->sh, cl, p->color4k, p->w, p->h, p->nr_in, p->ww, p->wh);   // async: the model thread downscales its own copy
    stamp(3);
    GpuBarrier(cl, p->nr_in, UAV, NPSR);
    GpuBarrier(cl, p->gray, UAV, CSRC);
    ID3D12Resource* oin = OfaInput(p->ofa, p->ofa_cur);
    GpuBarrier(cl, oin, COMMON, CDST);
    cl->CopyResource(oin, p->gray);
    GpuBarrier(cl, oin, CDST, COMMON);
    // async: hand this frame to the model thread when it asks (its gray into the other held OFA slot,
    // the native frame into model_src); the fence value that completes the copies goes with it.
    Pipeline::ModelFrame mf; bool handoff = false;
    p->model_reset_pending |= reset;
    if (async && p->model_wants_frame.load())
    {
        handoff = true; p->model_wants_frame = false;
        mf.held = p->model_frame.held == 2 ? 3 : 2; mf.index = p->frame_index; mf.reset = p->model_reset_pending; p->model_reset_pending = false;
        ID3D12Resource* hin = OfaInput(p->ofa, mf.held);
        GpuBarrier(cl, hin, COMMON, CDST); cl->CopyResource(hin, p->gray); GpuBarrier(cl, hin, CDST, COMMON);
        GpuBarrier(cl, p->color4k, NPSR, CSRC); cl->CopyResource(p->model_src, p->color4k); GpuBarrier(cl, p->color4k, CSRC, NPSR);
    }
    GpuBarrier(cl, p->gray, CSRC, UAV);
    GpuBarrier(cl, cap, NPSR, COMMON);
    const UINT64 f1 = GpuEnd(g);
    if (!f1) return false;
    if (handoff)
    {
        mf.fence = f1;
        { std::lock_guard<std::mutex> lk(p->model_mu); p->model_frame = mf; p->model_frame_ready = true; }
        p->model_cv.notify_one();
    }
    PipelineReadStamps(p);

    // ---- optical flow (pair 0, per frame; the model track runs its own on pair 1) -----------------
    double ofa_t0 = 0;
    if (p->measure_ofa) { GpuWait(g, g.fence, f1, 5000); ofa_t0 = NowMs(); }
    tw = NowMs();
    const UINT64 ov = OfaExecuteRef(p->ofa, g.fence, f1, p->ofa_cur, 1 - p->ofa_cur, 0, reset);   // OfaFenceValue is shared with the model thread: use the returned value
    p->cpu_wait[1].add(NowMs() - tw);
    if (!ov) { Log("[ofa] execute failed"); return false; }
    p->ofa_cur ^= 1;
    if (p->measure_ofa) { GpuWait(g, OfaFence(p->ofa), ov, 5000); p->st[PS_OFA].add(NowMs() - ofa_t0); }
    g.queue->Wait(OfaFence(p->ofa), ov);

    // async: pick up the newest published residual; the queue waits for it once per publish
    UINT residual_frame = 0;
    if (async)
    {
        int idx; UINT64 pf;
        { std::lock_guard<std::mutex> lk(p->pub_mu); idx = p->pub_idx; pf = p->pub_fence; residual_frame = p->pub_frame; }
        if (idx >= 0 && (idx != p->cmp_idx || pf != p->cmp_fence)) { g.queue->Wait(p->model_ctx.fence, pf); p->cmp_idx = idx; p->cmp_fence = pf; }
    }

    // ---- list 2: expand, (create | evaluate), compose, hand-off to the presenter -------------------
    if (p->ov && !p->fg) OverlayGuard(p->ov, g.queue);   // the present queue may still be copying out4k
    tw = NowMs();
    if (!GpuBegin(g)) return false;
    p->cpu_wait[2].add(NowMs() - tw);
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

    if (!async && p->nr && !NrReady(p->nr) && p->create_pending)
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
    if (!async && p->nr && NrReady(p->nr) && !p->bypass)
    {
        stamp(4);
        const unsigned r = NrEvaluate(p->nr, cl, p->nr_in, p->mv, p->nr_out, reset, c.exposure_scale);
        stamp(5);
        if (r == 1) { evaluated = true; ++p->evals_since_create; }
        else { static unsigned n = 0; if ((n++ % 120) == 0) Log("[nr] evaluate -> 0x%08X (%s) %s", r, NgxResultName(r), NrLastError(p->nr)); }
    }

    ComposeParams cp;
    cp.residual_strength = c.residual_strength; cp.feather = c.feather; cp.nrects = c.nrects; cp.warp = c.warp;
    memcpy(cp.rects, c.rects, sizeof cp.rects);
    const bool native = async ? (p->cmp_idx < 0 || p->bypass) : (!evaluated || p->evals_since_create <= (UINT)std::max(0, c.warmup));
    if (native) cp.wipe_mode = 2;
    else if (p->wipe == 1) { cp.wipe_mode = 1; cp.wipe_x = 0.5f; }
    else if (p->wipe == 2) { cp.wipe_mode = 1; cp.wipe_x = (float)fmod((NowMs() - p->wipe_t0) / 2000.0, 1.0); }
    GpuBarrier(cl, p->out4k, CSRC, UAV);
    if (async)
    {
        // The residual is from model frame M; mv is only this frame's motion (current -> previous).
        // v1 limitation: the ideal warp is the motion accumulated since M; we use this frame's mv
        // scaled by [nr] warp, which is exact for a 1-frame-old residual and approximate beyond.
        stamp(6);
        CsComposeResidual(g, p->sh, cl, p->color4k, p->residual[std::max(p->cmp_idx, 0)], p->ww, p->wh, p->mv, p->out4k, p->w, p->h, cp);
        stamp(7);
        if (!native) { evaluated = true; p->residual_age.add((double)(p->frame_index - residual_frame)); }
    }
    else
    {
        GpuBarrier(cl, p->nr_out, UAV, NPSR);
        stamp(6);
        CsCompose(g, p->sh, cl, p->color4k, p->nr_in, p->nr_out, p->ww, p->wh, p->out4k, p->w, p->h, cp);
        stamp(7);
        GpuBarrier(cl, p->nr_out, NPSR, UAV);
    }
    GpuBarrier(cl, p->out4k, UAV, CSRC);
    bool fg_recorded = false;
    if (p->fg) fg_recorded = FgRecord(p->fg, cl, p->out4k, p->mv);   // the presenter thread presents
    const UINT64 f2 = GpuEnd(g);
    if (!f2) return false;
    if (async && p->cmp_idx >= 0) p->residual_read_fence[p->cmp_idx] = f2;   // the model thread waits for it before rewriting that residual
    PipelineReadStamps(p);
    tw = NowMs();
    if (p->fg) { if (fg_recorded) FgSubmit(p->fg, f2, reset, p->cap_qpc, p->acq_qpc); }
    else if (p->ov)
    {
        // the present queue copies out4k once list 2 completes (fence f2) - no CPU wait here
        if (!OverlayPresent(p->ov, p->out4k, g.fence, f2)) { GpuLogDeviceRemoved(g, "present"); return false; }
        LONGLONG pq = 0, sq = 0; OverlayTimes(p->ov, pq, sq);
        if (p->cap_qpc) { p->age_ms.add(QpcToMs(pq - p->cap_qpc)); p->pipe_ms.add(QpcToMs(pq - p->acq_qpc)); }
    }
    p->cpu_wait[3].add(NowMs() - tw);
    if (evaluated && !async) NrRetireTick(p->nr);   // async: the model thread ticks
    p->last_evaluated = evaluated;   // async: composed with a residual
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
    if (GpuReadbackTex(*p->g, p->out4k, px.data(), p->w, p->h, 4, D3D12_RESOURCE_STATE_COPY_SOURCE))
    { swprintf_s(path, L"%ls\\dump_%03d_out.png", dir.c_str(), i); SavePngRgba(path, px.data(), p->w, p->h); }
    Log("[main] dumped frame %d", i);
}

static std::wstring JoinPath(const std::wstring& dir, const std::wstring& f)
{
    return (f.size() > 1 && (f[1] == L':' || f[0] == L'\\')) ? f : dir + L"\\" + f;
}

// ---- profiles ---------------------------------------------------------------------------------------
static std::wstring Stem(const std::wstring& path)
{
    const size_t s = path.find_last_of(L"\\/"), b = s == std::wstring::npos ? 0 : s + 1, e = path.rfind(L'.');
    return path.substr(b, e == std::wstring::npos || e < b ? std::wstring::npos : e - b);
}

std::wstring PickProfile(const std::wstring& dir, const std::wstring& ini, std::vector<std::wstring>& names, int& index)
{
    names.clear(); index = -1;
    const std::wstring pdir = dir + L"\\profiles";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((pdir + L"\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do names.push_back(Stem(fd.cFileName)); while (FindNextFileW(h, &fd)); FindClose(h); }
    std::sort(names.begin(), names.end());
    auto find = [&](const std::wstring& n) { for (size_t i = 0; i < names.size(); ++i) if (!_wcsicmp(names[i].c_str(), n.c_str())) return (int)i; return -1; };
    if (!ini.empty()) { index = find(Stem(ini)); return ini.find(L'\\') != std::wstring::npos ? ini : dir + L"\\" + ini; }
    if (names.empty()) return L"";
    for (size_t i = 0; i < names.size() && index < 0; ++i)   // the first profile whose window is up right now
    { Config c; if (ConfigLoad((pdir + L"\\" + names[i] + L".ini").c_str(), c) && FindTarget(c)) index = (int)i; }
    if (index < 0) index = find(L"wow");
    if (index < 0) index = 0;
    return pdir + L"\\" + names[index] + L".ini";
}

int main(int argc, char** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int dump = 0;
    std::wstring ini_arg;   // --ini <file>: an explicit profile (next to the exe, or a path); else PickProfile
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--bench")) return RunBench(argc, argv);
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = atoi(argv[++i]);
        if (!strcmp(argv[i], "--ini") && i + 1 < argc) { const char* a = argv[++i]; ini_arg.assign(a, a + strlen(a)); }
    }
    const std::wstring dir = ExeDir();
    std::vector<std::wstring> profiles; int profile = -1;
    std::wstring ini_path = PickProfile(dir, ini_arg, profiles, profile);
    Config cfg;
    const bool have_ini = ConfigLoad(ini_path.c_str(), cfg);
    std::wstring log_path = JoinPath(dir, cfg.log_file);
    LogInit(log_path.c_str());
    if (!have_ini) Log("[main] %ls not found - defaults in use", ini_path.c_str());
    else Log("[main] profile %ls (%zu in profiles\\)", ini_path.c_str(), profiles.size());
    Gpu g;
    if (!GpuInit(g, -1)) return 1;
    if (cfg.selftest) ComposeSelfTest(g);
    ResolveWork(cfg, dir);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);

    SetConsoleTitleW(L"JustFlow");
    Tray* tray = TrayCreate(L"JustFlow", nullptr);
    if (!tray) Log("[tray] not available (no icon; hotkeys still work)");
    std::vector<const wchar_t*> pnames; for (auto& n : profiles) pnames.push_back(n.c_str());
    if (tray) TraySetProfiles(tray, pnames.data(), (int)pnames.size());

    Pipeline* p = nullptr;   // the live pipeline, null while waiting for the window
    bool quit = false; int pending_profile = -1, dumped = 0, rc = 0;
    wchar_t status[128] = L"";
    auto profile_name = [&]() { return profile >= 0 ? profiles[profile] : Stem(ini_path); };
    auto tray_state = [&]
    {
        if (!tray) return;
        TrayState s; s.profile_index = profile; s.status = status;
        if (p) { s.nr_on = !p->bypass; s.fg_on = p->cfg.fg_enabled; s.fg_multiplier = p->cfg.fg_multiplier; s.wipe_mode = p->wipe; }
        TraySetState(tray, s);
    };
    auto tray_hotkeys = [&]   // cfg.hk_* -> the tray's Hotkeys dialog (ini order: toggle, wipe, reload, fg, quit)
    {
        if (!tray) return;
        const std::wstring s[5] = { FormatHotkey(cfg.hk_toggle), FormatHotkey(cfg.hk_wipe), FormatHotkey(cfg.hk_reload), FormatHotkey(cfg.hk_fg), FormatHotkey(cfg.hk_quit) };
        const wchar_t* v[5] = { s[0].c_str(), s[1].c_str(), s[2].c_str(), s[3].c_str(), s[4].c_str() };
        TraySetHotkeys(tray, v);
    };
    auto apply_hotkeys = [&]   // cfg.hk_* -> overlay registration (a pipeline created later registers from cfg itself) + tray
    {
        bool ok = true;
        if (p && p->ov) { HotkeyDef k[5]; HotkeyDefs(cfg, k); ok = OverlaySetHotkeys(p->ov, k, 5); }
        tray_hotkeys();
        return ok;
    };
    // F9 / F10 / F8 / F11 and their tray menu items
    auto toggle_nr = [&] { if (!p) return; p->bypass = !p->bypass; if (!p->bypass) p->force_reset = true; Log("[main] bypass %s", p->bypass ? "on" : "off"); tray_state(); };
    auto cycle_wipe = [&] { if (!p) return; p->wipe = (p->wipe + 1) % 3; p->wipe_t0 = NowMs(); Log("[main] wipe %d", p->wipe); tray_state(); };
    auto toggle_fg = [&] { if (!p) return; p->cfg.fg_enabled = !p->cfg.fg_enabled; Log("[main] fg %s", p->cfg.fg_enabled ? "on" : "off"); tray_state(); };
    auto reload = [&]
    {
        if (!p) return;
        Config nc; ConfigLoad(ini_path.c_str(), nc);
        ResolveWork(nc, dir);
        PipelineReload(p, nc); cfg = nc;
        apply_hotkeys();
        Log("[main] config reloaded");
        tray_state();
    };
    auto handle_tray = [&]
    {
        TrayEvent ev; int arg;
        while (tray && TrayPoll(tray, ev, arg)) switch (ev)
        {
        case TrayToggleNr: toggle_nr(); break;
        case TrayToggleFg: toggle_fg(); break;
        case TrayWipe:     cycle_wipe(); break;
        case TrayReload:   reload(); break;
        case TrayFgMultiplier:
            if (p) { Config nc = p->cfg; nc.fg_multiplier = arg; PipelineReload(p, nc); }   // like a reload: FG is recreated next frame
            cfg.fg_multiplier = arg; Log("[main] fg multiplier %d", arg); tray_state();
            break;
        case TraySelectProfile: if (arg >= 0 && arg < (int)profiles.size() && arg != profile) pending_profile = arg; break;
        case TrayOpenConfig: ShellExecuteW(nullptr, L"open", ini_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case TrayOpenLog:    ShellExecuteW(nullptr, L"open", log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case TrayQuit:       Log("[main] quit (tray)"); quit = true; break;
        case TrayHotkeys:
        {
            wchar_t hk[5][32]; TrayGetHotkeys(tray, hk);
            static const wchar_t* const keys[5] = { L"toggle", L"wipe", L"reload", L"fg", L"quit" };
            for (int i = 0; i < 5; ++i) WritePrivateProfileStringW(L"hotkeys", keys[i], hk[i], ini_path.c_str());
            cfg.hk_toggle = ParseHotkey(hk[0], 0); cfg.hk_wipe = ParseHotkey(hk[1], 0); cfg.hk_reload = ParseHotkey(hk[2], 0);
            cfg.hk_fg = ParseHotkey(hk[3], 0); cfg.hk_quit = ParseHotkey(hk[4], 0);
            const bool ok = apply_hotkeys();
            Log("[main] hotkeys %ls %ls %ls %ls %ls -> %ls%s", hk[0], hk[1], hk[2], hk[3], hk[4], ini_path.c_str(), ok ? "" : " (some did not register)");
            TrayNotify(tray, L"JustFlow", ok ? L"Hotkeys updated" : L"Some hotkeys could not be registered (taken by another app?)");
            break;
        }
        default: break;
        }
    };

    tray_hotkeys();
    while (!quit)
    {
        if (pending_profile >= 0)   // tray: switch profile (the pipeline is already torn down)
        {
            profile = pending_profile; pending_profile = -1;
            ini_path = dir + L"\\profiles\\" + profiles[profile] + L".ini";
            Config nc;
            if (!ConfigLoad(ini_path.c_str(), nc)) Log("[main] %ls not found - defaults in use", ini_path.c_str());
            ResolveWork(nc, dir); cfg = nc;
            const std::wstring lp = JoinPath(dir, cfg.log_file);
            if (lp != log_path) { log_path = lp; LogInit(log_path.c_str()); }
            Log("[main] profile %ls", ini_path.c_str());
            tray_hotkeys();
            if (tray) TrayNotify(tray, L"JustFlow", (L"Profile: " + profiles[profile]).c_str());
        }
        swprintf_s(status, L"%ls  waiting for window", profile_name().c_str()); tray_state();
        HWND target = nullptr;
        for (int i = 0; !quit && pending_profile < 0 && !(target = FindTarget(cfg)); ++i)
        {
            if (i % 30 == 0) Log("[main] waiting for window class=%ls title=%ls", cfg.window_class.c_str(), cfg.window_title.c_str());
            for (int t = 0; t < 10 && !quit && pending_profile < 0; ++t) { Sleep(100); handle_tray(); }
        }
        if (!target) continue;
        Log("[main] target window %p", (void*)target);
        Capture* cap = CaptureOpen(g, target, cfg.cursor, cfg.border, cfg.dda);
        // Desktop Duplication sees the whole monitor, our overlay included: exclusion is mandatory there.
        if (cap && CaptureIsDda(cap)) cfg.exclude_from_capture = true;
        if (!cap) { Sleep(1000); continue; }
        p = PipelineCreate(g, cfg, CaptureWidth(cap), CaptureHeight(cap), true, target);
        if (!p) { CaptureClose(cap); rc = 1; break; }
        swprintf_s(status, L"%ls", profile_name().c_str()); tray_state();

        bool reset = true;
        LONGLONG last_sysrel = 0;
        UINT frames = 0, skips = 0, rate_drops = 0; int follow_tick = 0; double last_processed_ms = 0;
        double win_t0 = NowMs();
        std::vector<double> cpu_ms;
        for (;;)
        {
            handle_tray();
            if (quit || pending_profile >= 0) break;
            if (OverlayHotkey(p->ov, 4)) { Log("[main] quit hotkey"); quit = true; break; }
            if (OverlayHotkey(p->ov, 1)) toggle_nr();
            if (OverlayHotkey(p->ov, 2)) cycle_wipe();
            if (OverlayHotkey(p->ov, 5)) toggle_fg();
            if (OverlayHotkey(p->ov, 3)) reload();
            if (CaptureLost(cap)) { Log("[main] capture lost - back to waiting for the window"); break; }
            UINT nw = 0, nh = 0;
            if (CaptureSizeChanged(cap, nw, nh))
            {
                Log("[main] window settled at %ux%u - recreating capture", nw, nh);
                GpuWaitIdle(g);
                CaptureClose(cap);
                cap = CaptureOpen(g, target, cfg.cursor, cfg.border, cfg.dda);
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
            LARGE_INTEGER acq; QueryPerformanceCounter(&acq);
            const double t0 = NowMs();
            // max_fps: a GPU-bound game (Dawnwalker with 3X FG presents ~135 fps) would otherwise get a
            // full pipeline pass per presented frame and lose the GPU time. Drop frames above the cap.
            // Tolerant limiter: a frame that arrives a little early on capture jitter is still taken, and the
            // schedule advances by whole periods so 90 fps in stays 90 fps through (a strict "< period"
            // test rejected every early frame and turned a 90 fps capture into ~60).
            if (cfg.max_fps > 0)
            {
                const double period = 1000.0 / cfg.max_fps;
                if (t0 - last_processed_ms < period * 0.6) { ++rate_drops; continue; }
                last_processed_ms = (t0 - last_processed_ms < period * 1.5) ? last_processed_ms + period : t0;
            }
            // > 250 ms gap (DDA: raw QPC ticks; WGC: 100 ns) -> scene reset
            const bool dda = CaptureIsDda(cap);
            if (last_sysrel && sysrel - last_sysrel > (dda ? qpf.QuadPart / 4 : 2500000)) reset = true;
            last_sysrel = sysrel;
            // Capture timestamp in QPC ticks: DDA LastPresentTime already is; WGC SystemRelativeTime is
            // 100 ns units (integer split keeps it exact: t * qpf overflows int64).
            p->cap_qpc = dda ? sysrel : sysrel / 10000000 * qpf.QuadPart + (sysrel % 10000000) * qpf.QuadPart / 10000000;
            p->acq_qpc = acq.QuadPart;
            if (!PipelineFrame(p, CaptureTexture(cap), CaptureFence(cap), fv, reset)) { Log("[main] frame failed - exiting"); GpuLogDeviceRemoved(g, "frame"); quit = true; rc = 3; break; }
            reset = false;
            OverlayFollow(p->ov, cfg.reassert_topmost_every);
            cpu_ms.push_back(NowMs() - t0);
            if (p->last_evaluated && dumped < dump) DumpFrame(p, dir, dumped++);
            if (++frames % (UINT)std::max(1, cfg.stats_every) == 0)
            {
                StageStats cpu{ cpu_ms }, spacing, age = p->age_ms, pipe = p->pipe_ms;
                double gpu = 0;
                for (int s = 0; s < PS_COUNT; ++s) if (s != PS_OFA && p->st[s].med() > 0) gpu += p->st[s].med();
                FgStatsOut fs;
                if (p->fg) { FgStats(p->fg, fs); spacing.v.swap(fs.spacing_ms); age.v.swap(fs.age_ms); pipe.v.swap(fs.pipe_ms); }
                StageStats mm; { std::lock_guard<std::mutex> lk(p->pub_mu); mm.v.swap(p->model_ms.v); }
                const UINT model_evals = p->model_evals.exchange(0);
                const double span = NowMs() - win_t0, cap_fps = frames * 1000.0 / span, fg_fps = fs.presented * 1000.0 / span;
                Log("[stats] cap_fps=%.1f eval_ms=%.2f/%.2f(med/p95) frame_gpu_ms=%.2f cpu_ms=%.2f static_skips=%u rate_drops=%u age_ms=%.1f pipe_ms=%.1f fg_out_fps=%.1f fg_spacing_ms=%.2f/%.2f(med/p95) fg_drops=%u model_fps=%.1f model_ms=%.2f residual_age_frames=%.1f",
                    cap_fps, p->st[PS_EVAL].med(), p->st[PS_EVAL].p95(), gpu, cpu.med(), skips, rate_drops, age.med(), pipe.med(),
                    fg_fps, spacing.med(), spacing.p95(), fs.drops,
                    model_evals * 1000.0 / span, mm.med(), p->residual_age.med());
                swprintf_s(status, L"%ls  cap %.0f  out %.0f fps  age %.0f ms", profile_name().c_str(), cap_fps, p->fg ? fg_fps : cap_fps, std::max(0.0, age.med()));
                tray_state();
                p->residual_age.v.clear();
                Log("[stats] gpu: swz=%.2f gray+ds=%.2f expand=%.2f eval=%.2f compose=%.2f | cpu waits: begin1=%.1f ofa=%.1f begin2=%.1f present=%.1f",
                    p->st[PS_SWIZZLE].med(), p->st[PS_GRAYDS].med(), p->st[PS_EXPAND].med(), p->st[PS_EVAL].med(), p->st[PS_COMPOSE].med(),
                    p->cpu_wait[0].med(), p->cpu_wait[1].med(), p->cpu_wait[2].med(), p->cpu_wait[3].med());
                for (auto& s : p->cpu_wait) s.v.clear();
                p->age_ms.v.clear(); p->pipe_ms.v.clear();
                frames = 0; skips = 0; rate_drops = 0; win_t0 = NowMs(); cpu_ms.clear();
                for (auto& s : p->st) s.v.clear();
            }
        }
        PipelineDestroy(p); p = nullptr;
        CaptureClose(cap);
    }
    if (tray) TrayDestroy(tray);
    GpuShutdown(g);
    return rc;
}
