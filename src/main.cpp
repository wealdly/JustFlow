// JustFlow: capture a window -> DLSS 5 neural rendering -> click-through overlay.
//   justflow [--ini <file>] [--dump N]        live mode (app settings from justflow.ini, game profile from profiles\*.ini, see PickProfile)
//   justflow --bench <png|dir> [--frames N] [--work WxH] [--no-present]
#include "pipeline.h"
#include "capture.h"
#include "log.h"
#include "tray.h"
#include "settings.h"
#include "mouse.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <timeapi.h>
#include <cstdlib>
#include <cstring>

int  RunBench(int argc, char** argv);                                             // bench.cpp
bool SaveTexPng(Gpu& g, ID3D12Resource* tex, UINT w, UINT h, D3D12_RESOURCE_STATES state, const wchar_t* path);   // bench.cpp

const char* const kPipeStageName[PS_COUNT] = { "swizzle", "gray+downscale", "eval", "compose", "expand", "filter", "list1(all)", "list2(all)", "ofa", "ofa2" };

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

// [nr] work=auto. The model runs on a downscaled copy and its edit is composed back up, so the
// RATIO to the native size matters more than the pixel count. Measured against the model's own edit
// at native 4K, on two unrelated test frames:
//     work        cost     fidelity (r)    strength
//   2560x1440   5.65 ms    0.80 / 0.49    144% / 70%     <- 1.5x: a non-integer resample both ways
//   1920x1080   3.79 ms    0.88 / 0.67    106% / 103%    <- 2x: exact
//   1280x720    2.58 ms    0.73 / 0.55
//    960x540    2.30 ms    0.44 / 0.53    (cost has a ~1.8 ms floor; below 720p it buys almost nothing)
// 1080p beat 1440p on BOTH axes on those two frames. A third (tools/scene) reversed the fidelity order
// (1440p 62%, 1080p 53% of the native edit), and replacing our downscale AND upscale with offline Lanczos /
// bicubic moved no size by more than 2 points: the resampling is innocent, the model is scale-sensitive,
// non-monotonically and by content. Cost still decides for 1080p; only native is reliably faithful. So auto is the integer divisor of the native size
// nearest 1080 lines (ties go to the larger): 4K -> 1920x1080, 1440p -> 2560x1440 (1:1, exact),
// 1080p -> 1920x1080. It used to come from justflow.spike.ini, where the spike had picked 2560x1440
// on evaluate time alone - and the capture size was not even known when it was read.
//
// The loss at a small work size is the model behaving differently at the wrong scale, not missing input:
// a 640x360 edit could carry 77-90% of the native-4K edit, a 1080p one 87-99%, and we get 53-89%. What does
// recover it is spending the same budget in TIME instead: on a moving tools/scene sequence, share of the
// every-frame 4K edit reproduced -
//   1920x1080 every frame   57%   (3.8 ms/frame)       3840x2160 model_every=4   81%   (12.2 ms / 4 = 3.0)
//   1920x1080 model_every=3 49%                        3840x2160 model_every=6   76%
// The edit is low-frequency, so it survives being motion-warped for several frames. NOT the default: the
// bench cannot see what a 12 ms evaluate does to a game sharing the GPU. It is work=native + model_every,
// which the Balanced and Quality presets select (settings.cpp).
static void WorkAuto(Config& c, UINT w, UINT h)
{
    if (!c.work_auto || !w || !h) return;
    if (c.work_native) { c.work_w = w; c.work_h = h; return; }
    UINT best = 1; long best_d = 1L << 30;
    // Any divisor, rounded: CsDownscale is an exact area filter over fractional footprints. Exact
    // divisors only gave a 3840x2159 window (Chrome unfocused) no candidate but 1 - the model at 4K.
    for (UINT d = 1; d <= 4; ++d)
    {
        if ((h + d / 2) / d < 360) continue;
        const long dist = labs((long)((h + d / 2) / d) - 1080);
        if (dist < best_d) { best_d = dist; best = d; }   // strict <: on a tie the smaller divisor (larger size) stays
    }
    c.work_w = (w + best / 2) / best; c.work_h = (h + best / 2) / best;
}

void ResolveWork(Config& c, const std::wstring& dir)
{
    // The spike file still supplies the machine-level parameter block; the work SIZE is no longer its
    // business (see WorkAuto). An explicit work=WxH in the profile is left exactly as written.
    const std::wstring ini = dir + L"\\justflow.spike.ini";
    if (c.work_auto) c.param_block = (int)GetPrivateProfileIntW(L"spike", L"param_block", c.param_block, ini.c_str());
}

// Overlay hotkey ids: 1 toggle, 2 wipe, 3 reload, 4 quit, 5 fg, 6 hud, 7 filters (OverlayHotkey(p->ov, id) in main).
static const int kHotkeys = 7;
static void HotkeyDefs(const Config& c, HotkeyDef out[kHotkeys])
{
    const HotkeyDef k[kHotkeys] = { { 1, c.hk_toggle.mods, c.hk_toggle.vk }, { 2, c.hk_wipe.mods, c.hk_wipe.vk }, { 3, c.hk_reload.mods, c.hk_reload.vk },
                                    { 4, c.hk_quit.mods, c.hk_quit.vk }, { 5, c.hk_fg.mods, c.hk_fg.vk }, { 6, c.hk_hud.mods, c.hk_hud.vk },
                                    { 7, c.hk_filters.mods, c.hk_filters.vk } };
    memcpy(out, k, sizeof k);
}

void PipelineToast(Pipeline* p, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->toast, sizeof p->toast, fmt, ap);
    va_end(ap);
    p->toast_t0 = NowMs(); p->toast_until_ms = p->toast_t0 + 2000.0;
    Log("[ui] toast: %s", p->toast);
}

// ---- pipeline ---------------------------------------------------------------------------------------
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const D3D12_RESOURCE_STATES COMMON = D3D12_RESOURCE_STATE_COMMON;
static const D3D12_RESOURCE_STATES CSRC = D3D12_RESOURCE_STATE_COPY_SOURCE, CDST = D3D12_RESOURCE_STATE_COPY_DEST;
static const D3D12_RESOURCE_FLAGS FUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

// ---- addon UI mask strip (addon\JustFlow\JustFlow.lua): 130 cells of 4x4 px at the top-left ----------
static const UINT kStripCells = 2 * 64 + 2, kStripW = kStripCells * 4, kStripH = 4;   // 520 x 4 px
static const UINT kStripPitch = (kStripW * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

// RGBA8 strip rows (pitch bytes apart) -> rects. Each 4x4 cell is read at its pixel (2, 2) only: one
// pixel inside the cell, clear of a one-pixel bleed from either neighbour. Returns the rect count,
// -1 when the header magic or the checksum do not match (out is untouched then).
static int MaskDecode(const uint8_t* px, UINT pitch, UiRect* out)
{
    auto cell = [&](UINT i) { return px + 2 * pitch + (i * 4 + 2) * 4; };
    const uint8_t* hd = cell(0);
    if (hd[0] != 0x4A || hd[1] != 0x46 || hd[2] > 64) return -1;
    const int n = hd[2];
    UiRect r[64]; unsigned sum = 0;
    for (int i = 0; i < n; ++i)
    {
        const uint8_t *a = cell(2 * i + 1), *b = cell(2 * i + 2);
        sum += a[0] + a[1] + a[2] + b[0] + b[1] + b[2];
        r[i].x0 = (a[0] << 4) | (a[1] >> 4); r[i].y0 = ((a[1] & 15) << 8) | a[2];
        r[i].x1 = (b[0] << 4) | (b[1] >> 4); r[i].y1 = ((b[1] & 15) << 8) | b[2];
    }
    const uint8_t* ck = cell(2 * n + 1);
    if (ck[0] != (sum & 255) || ck[1] != ck[0] || ck[2] != ck[0]) return -1;
    memcpy(out, r, (size_t)n * sizeof(UiRect));
    return n;
}

// Decode the newest retired strip copy (non-blocking: fence already completed); a mask not decoded
// for 1 s expires to the manual rects. Logs once when the mask appears and once when it vanishes.
static void MaskUpdate(Pipeline* p)
{
    Gpu& g = *p->g;
    const UINT64 done = g.fence->GetCompletedValue();
    int best = -1;
    for (int i = 0; i < Gpu::kFrames; ++i)
        if (p->strip_fence[i] && p->strip_fence[i] <= done && (best < 0 || p->strip_fence[i] > p->strip_fence[best])) best = i;
    if (best >= 0)
    {
        const UINT64 bv = p->strip_fence[best];
        for (auto& f : p->strip_fence) if (f <= bv) f = 0;   // consumed (older copies too)
        uint8_t* px = nullptr; const D3D12_RANGE rr = { 0, (SIZE_T)kStripPitch * kStripH };
        if (SUCCEEDED(p->strip_rb[best]->Map(0, &rr, (void**)&px)))
        {
            const int n = MaskDecode(px, kStripPitch, p->mask_rects);
            const D3D12_RANGE none = { 0, 0 }; p->strip_rb[best]->Unmap(0, &none);
            if (n >= 0) { p->mask_n = n; p->mask_seen = NowMs(); }
        }
    }
    const bool fresh = p->cfg.mask && p->mask_seen > 0 && NowMs() - p->mask_seen < 1000.0;
    if (fresh == p->mask_active) return;
    p->mask_active = fresh;
    if (!fresh) Log("[ui] addon mask lost (no valid strip for 1 s) - manual rects only");
    else if (p->mask_n) Log("[ui] addon mask: %d rects, checksum ok (rect 1: %d,%d,%d,%d)", p->mask_n, p->mask_rects[0].x0, p->mask_rects[0].y0, p->mask_rects[0].x1, p->mask_rects[0].y1);
    else Log("[ui] addon mask: 0 rects, checksum ok");
    if (fresh) PipelineToast(p, "UI mask: %d rects", p->mask_n); else PipelineToast(p, "UI mask lost");
}

static bool AllocNative(Pipeline* p)
{
    Gpu& g = *p->g;
    p->color4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, NPSR, L"color4k");
    p->gray = GpuMakeTex(g, p->gw, p->gh, DXGI_FORMAT_R8_UNORM, FUAV, UAV, L"gray");
    p->out4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, D3D12_RESOURCE_STATE_COPY_SOURCE, L"out4k");
    p->sharp4k = GpuMakeTex(g, p->w, p->h, DXGI_FORMAT_R8G8B8A8_UNORM, FUAV, D3D12_RESOURCE_STATE_COPY_SOURCE, L"sharp4k");
    p->shown = p->out4k;
    const UINT tw = (p->w + 63) / 64, th = (p->h + 63) / 64;
    p->same_pitch = (tw * 2 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    p->same = GpuMakeTex(g, tw, th, DXGI_FORMAT_R8G8_UNORM, FUAV, CSRC, L"same"); p->same_prev.clear();
    p->same_rb = GpuMakeBuffer(g, (UINT64)p->same_pitch * th, D3D12_HEAP_TYPE_READBACK, CDST, D3D12_RESOURCE_FLAG_NONE, L"same_rb");
    if (p->w % p->gw || p->h % p->gh) Log("[main] warning: gray block %ux%u -> %ux%u is not integer", p->w, p->h, p->gw, p->gh);
    return p->color4k && p->gray && p->out4k && p->sharp4k && p->same && p->same_rb;
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
        p->mv_res = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R16G16_FLOAT, FUAV, NPSR, L"mv_res");
        for (auto& r : p->residual) r = GpuMakeTex(g, p->ww, p->wh, DXGI_FORMAT_R16G16B16A16_FLOAT, FUAV, NPSR, L"residual");
    }
    return p->model_src && p->nr_in_m && p->nr_out_m && p->mv_m && p->mv_res && p->residual[0] && p->residual[1];
}
static void ReleaseModelWork(Pipeline* p) { REL(p->nr_in_m); REL(p->nr_out_m); REL(p->mv_m); REL(p->mv_res); REL(p->residual[0]); REL(p->residual[1]); }

static void SetModelParams(Pipeline* p, const Config& c)
{
    std::lock_guard<std::mutex> lk(p->model_mu);
    p->model_params = { c.zero_below, c.exposure_scale, c.model_max_fps, c.warmup };
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
        if (ok) { NrMarkSubmitted(nr); p->eval_fails = 0; p->model_dead = false; Log("[nr] create submitted %ux%u (model thread)", nc.work_w, nc.work_h); }
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
        ID3D12Resource* flow = OfaFlow2(ofa);
        GpuBarrier(c.list, flow, COMMON, NPSR);
        GpuBarrier(c.list, p->mv_m, NPSR, UAV);
        CsExpand(g, p->sh, c.list, flow, OfaFlowWidth(ofa), OfaFlowHeight(ofa), p->gw, p->gh, p->mv_m, p->ww, p->wh, mp.zero_below, reset);
        GpuBarrier(c.list, p->mv_m, UAV, NPSR);
        GpuBarrier(c.list, flow, NPSR, COMMON);
        bool evaluated = false;
        if (nr && NrReady(nr))
        {
            GpuCtxStamp(c, 0);
            const unsigned r = NrEvaluate(nr, c.list, p->nr_in_m, p->mv_m, p->nr_out_m, reset, mp.exposure);
            GpuCtxStamp(c, 1);
            if (r == 1) { evaluated = true; p->eval_fails = 0; }
            else
            {
                if ((p->eval_fails++ % 120) == 0) Log("[nr] model evaluate -> 0x%08X (%s) %s", r, NgxResultName(r), NrLastError(nr));
                if (p->eval_fails >= 30) return fail("30 consecutive evaluate failures");   // -> the sync path, which switches the model off
            }
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
        if (evals > (UINT)std::max(0, mp.warmup)) { p->pub_idx = j; p->pub_fence = fb; p->pub_frame = fr.index; p->pub_held = fr.held; j ^= 1; }
    }
}

static bool StartModel(Pipeline* p)
{
    if (!p->model_ctx.queue && !GpuCtxInit(*p->g, p->model_ctx, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, L"model")) return false;
    if (!AllocModel(p)) return false;
    p->model_stop = false; p->model_frame_ready = false; p->model_failed = false; p->model_wants_frame = true; p->model_reset_pending = true;
    p->pub_idx = -1; p->cmp_idx = -1; p->cmp_fence = 0; p->cmp_held = -1;
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
    p->g = &g; p->cfg = cfg; p->w = w; p->h = h; p->target = target;
    WorkAuto(p->cfg, w, h);
    if (p->cfg.work_auto) Log("[nr] work auto -> %ux%u (integer divisor of the %ux%u capture nearest 1080 lines)", p->cfg.work_w, p->cfg.work_h, w, h);
    // CsGray needs an integer block: round the block, derive the gray size from it.
    const UINT bx = std::max(1u, (UINT)std::lround((double)w / cfg.ofa_w)), by = std::max(1u, (UINT)std::lround((double)h / cfg.ofa_h));
    p->gw = w / bx; p->gh = h / by;
    if (p->gw != cfg.ofa_w || p->gh != cfg.ofa_h) Log("[main] ofa input %ux%u adjusted to %ux%u (block %ux%u)", cfg.ofa_w, cfg.ofa_h, p->gw, p->gh, bx, by);
    p->hud = cfg.hud;
    if (with_overlay)
    {
        HotkeyDef keys[kHotkeys]; HotkeyDefs(cfg, keys);
        p->ov = OverlayCreate(g, target, w, h, keys, kHotkeys, cfg.exclude_from_capture, cfg.overlay_mode);
        if (!p->ov) { PipelineDestroy(p); return nullptr; }
    }
    p->sh = ShadersCreate(g);
    if (!p->sh) { PipelineDestroy(p); return nullptr; }
    // The model loads when the neural layer is ON, not at startup regardless: with the layer off (the
    // default) it would hold its feature - hundreds of MB of VRAM - for nothing. F9 brings it up.
    if (cfg.nr_enabled)
    {
        p->nr = NrInit(g, ExeDir().c_str(), (NrParamBlock)cfg.param_block);
        if (!p->nr) { PipelineDestroy(p); return nullptr; }
        p->create_pending = true;
    }
    p->ofa = OfaCreate(g, p->gw, p->gh, cfg.ofa_grid, cfg.ofa_dll.empty() ? nullptr : cfg.ofa_dll.c_str());
    if (!p->ofa) { PipelineDestroy(p); return nullptr; }
    if (!AllocNative(p) || !AllocWork(p)) { PipelineDestroy(p); return nullptr; }
    for (auto& r : p->strip_rb)   // 9 KB each, always there so [ui] mask can be switched on by a reload
    {
        r = GpuMakeBuffer(g, (UINT64)kStripPitch * kStripH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"ui_strip_readback");
        if (!r) { PipelineDestroy(p); return nullptr; }
    }
    return p;
}

void PipelineDestroy(Pipeline* p)
{
    if (!p) return;
    StopModel(p);
    GpuWaitIdle(*p->g);
    if (p->fg) FgDestroy(p->fg);
    REL(p->color4k); REL(p->gray); REL(p->out4k); REL(p->sharp4k); REL(p->nr_in); REL(p->nr_out); REL(p->mv);
    REL(p->model_src); ReleaseModelWork(p);
    for (auto& r : p->strip_rb) REL(r);
    REL(p->mvgrid); for (auto& r : p->mvgrid_rb) REL(r); REL(p->same); REL(p->same_rb);
    MouseStop();
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
    REL(p->color4k); REL(p->gray); REL(p->out4k); REL(p->sharp4k); REL(p->model_src); REL(p->same); REL(p->same_rb);
    p->w = w; p->h = h;
    p->force_reset = true;
    {   // a new native size can change what auto means; the ordinary rebuild reallocates and recreates in step
        const UINT ow = p->cfg.work_w, oh = p->cfg.work_h; WorkAuto(p->cfg, w, h);
        if (p->cfg.work_w != ow || p->cfg.work_h != oh) p->rebuild_countdown = std::max(1, p->rebuild_countdown);
    }
    if (!AllocNative(p)) return false;
    return !p->ov || OverlayResize(p->ov, w, h);
}

void PipelineReload(Pipeline* p, const Config& c_in)
{
    Config c = c_in; WorkAuto(c, p->w, p->h);   // resolve BEFORE comparing, or every reload of an auto profile looks like a size change
    const bool rebuild = ConfigNeedsRebuild(p->cfg, c);
    // recreated next frame (a multiplier change is caught there: the presenter is rebuilt only when it differs)
    if (c.fg_pacing_vblank != p->cfg.fg_pacing_vblank || c.fg_engine != p->cfg.fg_engine || c.fg_lw_vfov != p->cfg.fg_lw_vfov) DropFg(p);
    if (c.nr_enabled && !p->cfg.nr_enabled) p->force_reset = true;
    if (c.nr_enabled && !p->nr)
    {
        p->nr = NrInit(*p->g, ExeDir().c_str(), (NrParamBlock)c.param_block);
        if (p->nr) p->create_pending = true; else Log("[nr] init failed on reload");
    }
    if (c.nr_async != p->cfg.nr_async) { StopModel(p); p->model_failed = false; p->force_reset = true; Log("[nr] mode=%s", c.nr_async ? "async" : "sync"); }
    p->cfg = c;
    SetModelParams(p, c);   // live keys for the model thread (zero_below, exposure, model_max_fps, warmup)
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
        double ms[PS_STAMPED];
        if (!GpuStampsMsSlot(g, s, ms, PS_STAMPED)) continue;
        p->last_stamp_fence_slot[s] = v;
        for (int i = 0; i < PS_STAMPED; ++i) p->st[i].add(ms[i]);
    }
}

bool PipelineFrame(Pipeline* p, ID3D12Resource* cap, ID3D12Fence* wait_fence, UINT64 wait_value, bool reset)
{
    Gpu& g = *p->g; const Config& c = p->cfg; ID3D12GraphicsCommandList* cl = g.list;
    p->last_evaluated = false; ++p->frame_index;
    for (auto& q : p->mvgrid_q)   // latewarp: flow grids whose frames have finished -> the mouse model
        if (q.fence && g.fence->GetCompletedValue() >= q.fence)
        {
            const int i = (int)(&q - p->mvgrid_q);
            float* d = nullptr; D3D12_RANGE rr = { 0, kMvGridPitch * kMvGridH };
            if (SUCCEEDED(p->mvgrid_rb[i]->Map(0, &rr, (void**)&d)))
            {
                constexpr UINT n = kMvGridW * kMvGridH;   // rows are exactly one pitch of float2s: the buffer is dense
                float xs[n], ys[n];
                for (UINT j = 0; j < n; ++j) { xs[j] = d[2 * j]; ys[j] = d[2 * j + 1]; }
                D3D12_RANGE none = { 0, 0 }; p->mvgrid_rb[i]->Unmap(0, &none);
                std::nth_element(xs, xs + n / 2, xs + n); std::nth_element(ys, ys + n / 2, ys + n);
                const float k = -(float)p->w / (float)p->ww;   // backward flow, work px -> where the content went, output px
                p->lw_g[0] = xs[n / 2]; p->lw_g[1] = ys[n / 2];
                if (q.t0 && q.t1) MouseObserve(q.t0, q.t1, xs[n / 2] * k, ys[n / 2] * k);
            }
            q.fence = 0;
        }
    if (p->force_reset) { reset = true; p->force_reset = false; }
    if (p->rebuild_countdown > 0 && --p->rebuild_countdown == 0)
    {
        StopModel(p); p->model_failed = false;   // async: the restart below creates the new feature on the model thread
        if (p->ww != c.work_w || p->wh != c.work_h) { GpuWaitIdle(g); DropFg(p); REL(p->nr_in); REL(p->nr_out); REL(p->mv); ReleaseModelWork(p); if (!AllocWork(p)) return false; }
        p->create_pending = true; reset = true;
        PipelineToast(p, "Rebuilding model %ux%u...", p->ww, p->wh); p->model_toast_pending = true;
    }
    // model_on, not p->nr: the object outlives the setting. Once the model had been on, turning it
    // off left p->nr alive, so every gate below still fired - measured live as ~5000 failing
    // EvaluateFeature calls with model=0, each one taking the NgxMutex that DLSS-G also needs.
    // The layer switch is part of it: with the layer off (F9, Settings, tray) the model does not run at
    // all - it used to keep evaluating on the async thread, unseen, while the compose showed native.
    const bool model_on = c.nr_enabled && p->nr && !p->model_dead;
    const bool async = c.nr_async && model_on && !p->model_failed;
    // Motion vectors have two consumers: the model (its temporal reprojection) and the warp engine
    // (it extrapolates along them). DLSS-G is not one - it measures motion itself and ignores the
    // values it is handed (fg.cpp, Evaluate: our flow, zeros and a field 50 px wrong score
    // identically). Without either, the gray pass, the OFA execute, the queue wait on it and the
    // expand ran for nobody, and they were 82-93% of an FG-only frame.
    const bool warp_fg = c.fg_enabled && c.fg_engine == FG_WARP, lw_fg = c.fg_enabled && c.fg_engine == FG_LATEWARP;   // lw: the mouse model learns from the flow
    if (lw_fg) MouseStart(); else MouseStop();
    const bool need_mv = model_on || warp_fg || lw_fg;
    if (need_mv && !p->had_mv) reset = true;   // the previous gray is stale: no flow across the gap
    p->had_mv = need_mv;
    // The per-frame flow (pair 0, current -> previous -> p->mv) has two readers: the SYNC evaluate and
    // the warp engine. The async track warps its residual with a different flow (pair 2, current ->
    // the residual's own frame), so it needs no pair 0 for itself.
    const bool need_pair0 = (model_on && !async) || warp_fg || lw_fg;
    if (!async && p->model_thread.joinable()) StopModel(p);   // layer off or mode changed: the thread stops paying
    if (async && !p->model_thread.joinable() && !StartModel(p)) return false;
    // The presenter is always on while an overlay exists: passthrough (multiplier 1) with FG off,
    // generation with cfg.fg_enabled (ini, F8). A generation failure turns the flag off (F8 retries)
    // and the presenter is rebuilt as passthrough; a passthrough failure is a lost device: frame fails.
    if (p->ov)
    {
        const int want = c.fg_enabled ? c.fg_multiplier : 1;
        if (p->fg && FgFailed(p->fg))
        {
            if (FgMultiplier(p->fg) == 1) { Log("[fg] passthrough presenter failed"); return false; }
            p->cfg.fg_enabled = false; PipelineToast(p, "FG disabled: presenter failed");
        }
        if (p->fg && (FgFailed(p->fg) || FgMultiplier(p->fg) != want)) DropFg(p);
        if (!p->fg)
        {
            p->fg = FgCreate(g, p->ov, ExeDir().c_str(), p->w, p->h, p->ww, p->wh, want, c.fg_pacing_vblank, c.fg_engine, c.fg_lw_vfov * 3.14159265f / 180.0f);
            if (!p->fg && want > 1) { p->cfg.fg_enabled = false; PipelineToast(p, "FG disabled: create failed"); p->fg = FgCreate(g, p->ov, ExeDir().c_str(), p->w, p->h, p->ww, p->wh, 1, c.fg_pacing_vblank, 0); }
            if (!p->fg) { Log("[fg] passthrough presenter create failed"); return false; }
        }
        FgSetTiming(p->fg, c.fg_phase_ms, c.fg_min_gain, c.fg_max_in_fps);   // ponytail: one atomic store per frame, no reload plumbing
    }
    auto stamp = [&](int i) { if (c.gpu_timestamps) GpuStamp(g, cl, i); };
    // async: the residual composed this frame = the newest published one (the queue waits for it once
    // per publish, before list 2). Picked before list 1 so the hand-off below can avoid its held slot.
    UINT residual_frame = 0; bool wait_pub = false;
    if (async)
    {
        int idx, held; UINT64 pf;
        { std::lock_guard<std::mutex> lk(p->pub_mu); idx = p->pub_idx; pf = p->pub_fence; residual_frame = p->pub_frame; held = p->pub_held; }
        if (idx >= 0 && (idx != p->cmp_idx || pf != p->cmp_fence)) { wait_pub = true; p->cmp_idx = idx; p->cmp_fence = pf; p->cmp_held = held; }
    }

    // ---- list 1: swizzle, gray, downscale, gray -> OFA input --------------------------------------
    if (wait_fence) g.queue->Wait(wait_fence, wait_value);
    // Timestamps live in the ring slot GpuBegin is about to reset. Under GPU load nothing has retired
    // by the time the after-submit read runs, so every slot was being wiped unread: all stages came
    // back -1 and the HUD showed "NR 0.0 ms" exactly when the numbers mattered. GpuBegin waits for
    // this slot to retire anyway - wait here first, read it, and nothing is lost or added.
    auto harvest = [&] { if (!c.gpu_timestamps) return; const UINT64 v = g.alloc_fence[g.slot]; if (v) GpuWait(g, g.fence, v, 2000); PipelineReadStamps(p); };
    double tw = NowMs();
    harvest();
    if (!GpuBegin(g)) return false;
    p->cpu_wait[0].add(NowMs() - tw);
    stamp(2 * PS_LIST1);   // spans the whole submission, barriers and copies included
    GpuBarrier(cl, cap, D3D12_RESOURCE_STATE_COMMON, NPSR);
    const bool check_same = !reset;   // after a reset color4k is not the previous frame
    if (check_same)
    {
        GpuBarrier(cl, p->same, CSRC, UAV);
        CsSame(g, p->sh, cl, cap, p->color4k, p->w, p->h, p->hdr_white, p->same);
        GpuBarrier(cl, p->same, UAV, CSRC);
        const D3D12_RESOURCE_DESC sd = p->same->GetDesc();
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = p->same; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = p->same_rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8_UNORM, (UINT)sd.Width, sd.Height, 1, p->same_pitch };
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    GpuBarrier(cl, p->color4k, NPSR, UAV);
    stamp(2 * PS_SWIZZLE); CsSwizzle(g, p->sh, cl, cap, p->color4k, p->w, p->h, p->hdr_white); stamp(2 * PS_SWIZZLE + 1);
    GpuBarrier(cl, p->color4k, UAV, NPSR);
    // addon mask: the top-left strip of this frame -> this slot's readback buffer (decoded once retired)
    int strip_slot = -1;
    if (c.mask && p->w >= kStripW && p->h >= 2 * kStripH && p->frame_index % (UINT)std::max(1, c.mask_every) == 0)
    {
        strip_slot = g.slot;
        GpuBarrier(cl, p->color4k, NPSR, CSRC);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = p->color4k; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = p->strip_rb[strip_slot]; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kStripW, kStripH, 1, kStripPitch };
        const D3D12_BOX box = { 0, 0, 0, kStripW, kStripH, 1 };
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        GpuBarrier(cl, p->color4k, CSRC, NPSR);
    }
    GpuBarrier(cl, p->nr_in, NPSR, UAV);
    stamp(2 * PS_GRAYDS);
    if (need_mv) CsGray(g, p->sh, cl, p->color4k, p->w, p->h, p->gray, p->gw, p->gh);
    // Only the model reads nr_in. Without it (FG-only, the default), the compose takes the native
    // path and never samples it, so this area filter would be dead work - and none of it while the
    // layer is switched off (F9), when the compose shows native.
    if (!async && model_on) CsDownscale(g, p->sh, cl, p->color4k, p->w, p->h, p->nr_in, p->ww, p->wh);   // async: the model thread downscales its own copy
    stamp(2 * PS_GRAYDS + 1);
    GpuBarrier(cl, p->nr_in, UAV, NPSR);
    if (need_mv)
    {
        GpuBarrier(cl, p->gray, UAV, CSRC);
        ID3D12Resource* oin = OfaInput(p->ofa, p->ofa_cur);
        GpuBarrier(cl, oin, COMMON, CDST);
        cl->CopyResource(oin, p->gray);
        GpuBarrier(cl, oin, CDST, COMMON);
    }
    // async: hand this frame to the model thread when it asks (its gray into the other held OFA slot,
    // the native frame into model_src); the fence value that completes the copies goes with it.
    Pipeline::ModelFrame mf; bool handoff = false;
    p->model_reset_pending |= reset;
    // model_every: hand a frame over only on the Nth, so the model's cadence is locked to the
    // capture (90 in, N=2 -> 45 evaluates/s, residual always exactly one frame old) rather than
    // free-running against a sleep-based rate cap and drifting in and out of phase.
    if (async && p->model_wants_frame.load() && (c.model_every <= 1 || p->frame_index % (UINT)c.model_every == 0))
    {
        handoff = true; p->model_wants_frame = false;
        // held slot 2..4: not the last hand-off's (the model's next flow reference), not cmp_held (ours)
        mf.held = 2; while (mf.held == p->model_frame.held || mf.held == p->cmp_held) ++mf.held;
        mf.index = p->frame_index; mf.reset = p->model_reset_pending; p->model_reset_pending = false;
        ID3D12Resource* hin = OfaInput(p->ofa, mf.held);
        GpuBarrier(cl, hin, COMMON, CDST); cl->CopyResource(hin, p->gray); GpuBarrier(cl, hin, CDST, COMMON);
        GpuBarrier(cl, p->color4k, NPSR, CSRC); cl->CopyResource(p->model_src, p->color4k); GpuBarrier(cl, p->color4k, CSRC, NPSR);
    }
    if (need_mv) GpuBarrier(cl, p->gray, CSRC, UAV);   // pairs with the UAV->CSRC above (async implies need_mv)
    GpuBarrier(cl, cap, NPSR, COMMON);
    stamp(2 * PS_LIST1 + 1);
    const UINT64 f1 = GpuEnd(g);
    if (!f1) return false;
    if (strip_slot >= 0) p->strip_fence[strip_slot] = f1;
    MaskUpdate(p);
    PipelineReadStamps(p);

    // ---- optical flow (pair 0, per frame; the model track runs its own on pair 1) -----------------
    // async + a residual to compose: a second flow, this frame -> the residual's model frame (its gray
    // is still held in slot cmp_held), pair 2 -> mv_res: the warp then spans the residual's age.
    const bool warp_res = async && p->cmp_idx >= 0;
    double ofa_t0 = 0;
    if (p->measure_ofa) { GpuWait(g, g.fence, f1, 5000); ofa_t0 = NowMs(); }
    tw = NowMs();
    const UINT64 ov = need_pair0 ? OfaExecuteRef(p->ofa, g.fence, f1, p->ofa_cur, 1 - p->ofa_cur, 0, reset) : 0;   // OfaFenceValue is shared with the model thread: use the returned value
    const UINT64 ov2 = warp_res ? OfaExecuteRef(p->ofa, g.fence, f1, p->ofa_cur, p->cmp_held, 2, reset) : 0;
    p->cpu_wait[1].add(NowMs() - tw);
    if ((need_pair0 && !ov) || (warp_res && !ov2)) { Log("[ofa] execute failed"); return false; }
    if (need_mv) p->ofa_cur ^= 1;
    if (p->measure_ofa)
    {
        if (ov) { GpuWait(g, OfaFence(p->ofa), ov, 5000); p->st[PS_OFA].add(NowMs() - ofa_t0); }
        if (ov2) { ofa_t0 = NowMs(); GpuWait(g, OfaFence(p->ofa), ov2, 5000); p->st[PS_OFA2].add(NowMs() - ofa_t0); }
    }
    if (ov) g.queue->Wait(OfaFence(p->ofa), ov);
    if (ov2) g.queue->Wait(OfaFence(p->ofa), ov2);   // also orders the next list 1's hand-off copies after this read of the held slot
    if (wait_pub) g.queue->Wait(p->model_ctx.fence, p->cmp_fence);

    // ---- duplicate / scene-cut test (CsSame in list 1), before anything below uses `reset` -------------
    // A capture identical to the last one (a browser repainting the same video frame at the display
    // rate) is not a frame: handed to FG it inflated "fps in" past the governor's floor and paired
    // identical images. A scene cut (a film changing camera, a game teleporting) is a reset: the model
    // carried the old shot's history into the new one and smeared it for several frames, and DLSS-G
    // interpolated between two unrelated images. List 1 is tiny and the OFA submission ran after it,
    // so the wait is normally free.
    bool dup = false, cut = false;
    if (!check_same) p->same_prev.clear();   // the previous frame is not comparable across a reset
    else if (GpuWait(g, g.fence, f1, 100))
    {
        const D3D12_RESOURCE_DESC sd = p->same->GetDesc(); uint8_t* px = nullptr;
        const UINT n = (UINT)sd.Width * sd.Height;
        const D3D12_RANGE rr = { 0, (SIZE_T)p->same_pitch * sd.Height };
        if (SUCCEEDED(p->same_rb->Map(0, &rr, (void**)&px)))
        {
            const UINT tw = (UINT)sd.Width, th = sd.Height;
            std::vector<uint8_t> cur(n); UINT changed = 0;
            for (UINT y = 0; y < th; ++y)
                for (UINT x = 0; x < tw; ++x)
                {
                    const uint8_t* t = px + (size_t)y * p->same_pitch + x * 2;
                    changed += t[0] != 0; cur[y * tw + x] = t[1];
                }
            const D3D12_RANGE none = { 0, 0 }; p->same_rb->Unmap(0, &none);
            dup = changed == 0;
            // Each tile against the closest of the previous frame's 3x3 tiles around it, so camera motion
            // up to a tile a frame is not change. tools/scene: motion <= 0.016 (a 30 fps third-person turn
            // included), a jump to another angle of the same scene >= 0.042. ponytail: one global
            // threshold - a miss is a few smeared frames, a false hit one frame without history.
            if (!dup && p->same_prev.size() == n)
            {
                UINT dsum = 0;
                for (UINT y = 0; y < th; ++y)
                    for (UINT x = 0; x < tw; ++x)
                    {
                        int best = 255;
                        for (UINT yy = y ? y - 1 : 0; yy <= std::min(y + 1, th - 1); ++yy)
                            for (UINT xx = x ? x - 1 : 0; xx <= std::min(x + 1, tw - 1); ++xx)
                                best = std::min(best, abs((int)cur[y * tw + x] - (int)p->same_prev[yy * tw + xx]));
                        dsum += (UINT)best;
                    }
                cut = dsum > 0.03 * 255.0 * n;
            }
            if (!dup) p->same_prev.swap(cur);
        }
    }
    if (dup) ++p->dups;
    if (cut) { reset = true; ++p->cuts; p->cut_frame = p->frame_index; }
    if (handoff)
    {
        mf.fence = f1; mf.reset |= cut;
        { std::lock_guard<std::mutex> lk(p->model_mu); p->model_frame = mf; p->model_frame_ready = true; }
        p->model_cv.notify_one();
    }

    // ---- list 2: expand, (create | evaluate), compose, hand-off to the presenter -------------------
    tw = NowMs();
    harvest();
    if (!GpuBegin(g)) return false;
    p->cpu_wait[2].add(NowMs() - tw);
    stamp(2 * PS_LIST2);
    if (need_pair0)
    {
        ID3D12Resource* flow = OfaFlow(p->ofa);
        GpuBarrier(cl, flow, D3D12_RESOURCE_STATE_COMMON, NPSR);
        GpuBarrier(cl, p->mv, NPSR, UAV);
        stamp(2 * PS_EXPAND);
        CsExpand(g, p->sh, cl, flow, OfaFlowWidth(p->ofa), OfaFlowHeight(p->ofa), p->gw, p->gh, p->mv, p->ww, p->wh, c.zero_below, reset);
        stamp(2 * PS_EXPAND + 1);
        GpuBarrier(cl, p->mv, UAV, NPSR);
        GpuBarrier(cl, flow, NPSR, D3D12_RESOURCE_STATE_COMMON);
    }
    int grid_q = -1;   // latewarp: this frame's flow, averaged and queued for the mouse model
    if (lw_fg && need_pair0 && !reset)   // timestamps only matter to the mouse model (checked on read-back)
    {
        if (!p->mvgrid)
        {
            p->mvgrid = GpuMakeTex(g, kMvGridW, kMvGridH, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"mvgrid");
            for (auto& r : p->mvgrid_rb) r = GpuMakeBuffer(g, kMvGridPitch * kMvGridH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"mvgrid_rb");
        }
        for (int i = 0; i < 4 && grid_q < 0; ++i) if (!p->mvgrid_q[i].fence && p->mvgrid_rb[i]) grid_q = i;
        if (p->mvgrid && grid_q >= 0)
        {
            GpuBarrier(cl, p->mvgrid, CSRC, UAV);
            CsMvGrid(g, p->sh, cl, p->mv, p->ww, p->wh, p->mvgrid);
            GpuBarrier(cl, p->mvgrid, UAV, CSRC);
            D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
            src.pResource = p->mvgrid; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = p->mvgrid_rb[grid_q]; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32_FLOAT, kMvGridW, kMvGridH, 1, kMvGridPitch };
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            p->mvgrid_q[grid_q].t0 = p->lw_prev_cap; p->mvgrid_q[grid_q].t1 = p->cap_qpc;
        }
        else grid_q = -1;
    }
    p->lw_prev_cap = lw_fg ? p->cap_qpc : 0;
    if (warp_res)
    {
        ID3D12Resource* flow3 = OfaFlow3(p->ofa);
        GpuBarrier(cl, flow3, COMMON, NPSR);
        GpuBarrier(cl, p->mv_res, NPSR, UAV);
        CsExpand(g, p->sh, cl, flow3, OfaFlowWidth(p->ofa), OfaFlowHeight(p->ofa), p->gw, p->gh, p->mv_res, p->ww, p->wh, c.zero_below, reset);
        GpuBarrier(cl, p->mv_res, UAV, NPSR);
        GpuBarrier(cl, flow3, NPSR, COMMON);
    }

    // Self-heal: a ready feature at the wrong size (NrEvaluate refuses it) schedules the ordinary
    // rebuild, which reallocates and recreates in step. Without this a mismatch would sit on native
    // forever, because nothing else would notice.
    // Not while a create is already pending - that create IS the heal, and re-arming the countdown
    // here starved it forever (the create waits for countdown == 0). --rework in bench caught that.
    if (!async && model_on && NrReady(p->nr) && !NrMatches(p->nr, p->ww, p->wh) && p->rebuild_countdown == 0 && !p->create_pending)
    { Log("[nr] feature size does not match the %ux%u work textures - rebuilding", p->ww, p->wh); p->rebuild_countdown = 1; }
    // Not while a rebuild is counting down: that rebuild reallocates the work textures and then
    // sets create_pending itself. Creating here used the NEW config size against the OLD textures.
    // NOT gated on !NrReady: a rebuild leaves the old feature ready, and that gate meant the rebuild
    // set create_pending, toasted "Rebuilding model" and then never recreated anything - every live
    // change to a create-latched key (work size, style, local tone/structure) was silently ignored
    // in sync mode. NrCreate retires the old feature itself, and this branch returns before any
    // evaluate, so create and evaluate still never share a list.
    if (!async && model_on && p->create_pending && p->rebuild_countdown == 0)
    {
        p->create_pending = false;
        const bool ok = NrCreate(p->nr, cl, ConfigToNr(c));
        const UINT64 v = GpuEnd(g);
        if (!v) return false;
        if (ok) { NrMarkSubmitted(p->nr); p->evals_since_create = 0; p->eval_fails = 0; p->model_dead = false; p->force_reset = true; Log("[nr] create submitted %ux%u", p->ww, p->wh); }
        else Log("[nr] create failed (%s) - native passthrough until reload", NrLastError(p->nr));
        return true;   // no evaluate on the create list
    }

    bool evaluated = false;
    if (!async && model_on && NrReady(p->nr))
    {
        stamp(2 * PS_EVAL);
        const unsigned r = NrEvaluate(p->nr, cl, p->nr_in, p->mv, p->nr_out, reset, c.exposure_scale);
        stamp(2 * PS_EVAL + 1);
        if (r == 1) { evaluated = true; ++p->evals_since_create; p->eval_fails = 0; }
        else
        {
            if ((p->eval_fails++ % 120) == 0) Log("[nr] evaluate -> 0x%08X (%s) %s", r, NgxResultName(r), NrLastError(p->nr));
            // Paying for a model that never answers is worse than none: after 30 in a row, off until the
            // next create (reload, resize, rebuild), and say so.
            if (p->eval_fails == 30) { p->model_dead = true; PipelineToast(p, "DLSS model failing (0x%08X) - off until reload", r); }
        }
    }

    ComposeParams cp;
    cp.residual_strength = c.residual_strength; cp.feather = c.feather; cp.warp = c.warp; cp.reject = c.warp_reject;
    cp.chroma = c.chroma;
    if (p->mask_active)   // addon rects first, the strip itself hidden; manual ini rects appended
    {
        cp.nrects = p->mask_n; memcpy(cp.rects, p->mask_rects, (size_t)p->mask_n * sizeof(UiRect));
        cp.strip_w = (int)kStripW; cp.strip_h = (int)kStripH;
    }
    for (int i = 0; i < c.nrects && cp.nrects < 64; ++i) cp.rects[cp.nrects++] = c.rects[i];
    const bool native = async ? (p->cmp_idx < 0 || residual_frame < p->cut_frame) : (!evaluated || p->evals_since_create <= (UINT)std::max(0, c.warmup));
    if (native) cp.wipe_mode = 2;
    else if (p->wipe == 1) { cp.wipe_mode = 1; cp.wipe_x = 0.5f; }
    else if (p->wipe == 2) { cp.wipe_mode = 1; cp.wipe_x = (float)fmod((NowMs() - p->wipe_t0) / 2000.0, 1.0); }
    // Take the FG slot now, so the LAST 4K pass of this frame writes straight into it. The frame
    // used to be composed into out4k/sharp4k and then CopyResource'd into the slot: 33 MB read plus
    // 33 MB written per frame, ~6 GB/s of memory traffic at 90 fps, for a copy that produced nothing
    // the previous pass had not already computed. Nobody downstream needs to know: the slot texture
    // has the same format, size and resting state (COPY_SOURCE) as the targets it replaces.
    // A duplicate (above) is not handed to FG: the overlay keeps showing the last frame.
    ID3D12Resource* const fg_dst = (p->fg && !dup) ? FgAcquire(p->fg) : nullptr;
    const bool filt = FiltersLive(c);
    ID3D12Resource* const compose_dst = filt ? p->out4k : (fg_dst ? fg_dst : p->out4k);
    GpuBarrier(cl, compose_dst, CSRC, UAV);
    if (async)
    {
        // The residual is from model frame M; mv_res is this frame's motion current -> M (the flow
        // against M's held gray), scaled by [nr] warp. mv (current -> previous) stays with FG.
        stamp(2 * PS_COMPOSE);
        CsComposeResidual(g, p->sh, cl, p->color4k, p->residual[std::max(p->cmp_idx, 0)], p->ww, p->wh, p->mv_res, compose_dst, p->w, p->h, cp);
        stamp(2 * PS_COMPOSE + 1);
        if (!native) { evaluated = true; p->residual_age.add((double)(p->frame_index - residual_frame)); }
    }
    else
    {
        GpuBarrier(cl, p->nr_out, UAV, NPSR);
        stamp(2 * PS_COMPOSE);
        CsCompose(g, p->sh, cl, p->color4k, p->nr_in, p->nr_out, p->ww, p->wh, compose_dst, p->w, p->h, cp);
        stamp(2 * PS_COMPOSE + 1);
        GpuBarrier(cl, p->nr_out, NPSR, UAV);
    }
    if (!native && p->model_toast_pending) { p->model_toast_pending = false; PipelineToast(p, "Model ready"); }
    // ---- the order of application, in one place ------------------------------------------------
    // Three layers, three independent switches, and none of them reaches into another:
    //
    //   capture -> NEURAL ([nr] enabled, F9)      the DLSS model, at work resolution,
    //                                             composed back as a residual at native resolution.
    //           -> FILTERS ([filters] enabled, F6) sharpen then vibrance, at native resolution.
    //           -> UI rects + text                the addon mask and the HUD/toasts.
    //           -> FRAME GEN ([fg] enabled, F8)   interpolates the finished frame; the UI is copied
    //                                             back onto the generated ones.
    //
    // The order is not arbitrary. Sharpening has to see what the model produced or it sharpens the
    // wrong image, vibrance grades after the sharpen rather than feeding it exaggerated contrast,
    // text goes on after both so it stays crisp, and FG runs last because it interpolates the frame
    // as the viewer sees it. Every combination is valid: any layer off just passes its input through.
    // Vibrance used to live inside the neural compose, which meant it silently died with the neural
    // layer while sharpen carried on - that is the bug this layout exists to prevent.
    ID3D12Resource* shown = compose_dst;
    if (filt)
    {
        ID3D12Resource* const filt_dst = fg_dst ? fg_dst : p->sharp4k;
        GpuBarrier(cl, p->out4k, UAV, NPSR);
        GpuBarrier(cl, filt_dst, CSRC, UAV);
        stamp(2 * PS_FILTER); CsSharpen(g, p->sh, cl, p->out4k, filt_dst, p->w, p->h, c.sharpen, c.saturation, (UINT)cp.nrects); stamp(2 * PS_FILTER + 1);   // rect_tex holds this frame's rects (compose above)
        GpuBarrier(cl, p->out4k, NPSR, CSRC);
        shown = filt_dst;
    }
    // toast (2 s, the last 0.4 s fade) top-centre; status HUD in its corner (also in bypass)
    const double now = NowMs();
    const bool toast_on = c.toast && p->toast[0] && now < p->toast_until_ms, hud_on = p->hud && p->hud_line[0][0];
    if (toast_on || hud_on)
    {
        GpuUavBarrier(cl, shown);
        if (toast_on)
            CsText(g, p->sh, cl, shown, p->w, p->h, p->toast, -1, -1, c.toast_scale, (float)std::min(1.0, (p->toast_until_ms - now) / 400.0), 2 * c.toast_scale);
        if (hud_on)
        {
            if (toast_on) GpuUavBarrier(cl, shown);   // a wide toast can reach a wide HUD line
            const int sc = std::max(1, c.hud_scale), pad = 2 * sc, margin = 24, gap = sc, bh = TextBoxH(sc, pad);
            const bool right = c.hud_corner & 1, bottom = c.hud_corner & 2;
            const int nlines = (int)(sizeof p->hud_line / sizeof p->hud_line[0]);
            int y = bottom ? (int)p->h - margin - nlines * bh - (nlines - 1) * gap : margin;
            for (const char* line : p->hud_line)
            {
                const int bw = TextBoxW(strlen(line), sc, pad);
                if (*line) CsText(g, p->sh, cl, shown, p->w, p->h, line, right ? (int)p->w - margin - bw : margin, y, sc, 1.0f, pad);
                y += bh + gap;
            }
        }
    }
    GpuBarrier(cl, shown, UAV, CSRC);
    p->shown = shown;
    // the presenter thread presents (passthrough or generation); --no-present has no Fg
    // engine=latewarp: Frame Warp holds still what does not move with the camera (a third-person
    // character, anything moving on its own) - warping it by the camera's rotation smudged its edges.
    if (fg_dst && lw_fg && need_pair0 && !reset)
        if (ID3D12Resource* m = FgMaskTarget(p->fg))
        {
            const D3D12_RESOURCE_DESC md = m->GetDesc();
            GpuBarrier(cl, m, NPSR, UAV);
            CsNoWarpMask(g, p->sh, cl, p->mv, p->ww, p->wh, m, (UINT)md.Width, md.Height, p->lw_g[0], p->lw_g[1]);
            GpuBarrier(cl, m, UAV, NPSR);
        }
    // engine=warp: the generated frames are this one pushed ahead along its own flow, (i+1)/multiplier
    // of an interval each, written straight into the slot. Not while the governor has FG paused.
    if (fg_dst && warp_fg && !FgPaused(p->fg) && FgWarpTarget(p->fg, 0))
    {
        // The pair the flow came from, for the static-pixel test: OfaExecuteRef ran ofa_cur -> 1 - ofa_cur
        // and ofa_cur has flipped since, so this frame's gray is 1 - ofa_cur and the previous one ofa_cur.
        // OFA inputs rest in COMMON; the OFA queue is done with them (p->mv was expanded from its output).
        ID3D12Resource* gcur = OfaInput(p->ofa, 1 - p->ofa_cur);
        ID3D12Resource* gprev = OfaInput(p->ofa, p->ofa_cur);
        GpuBarrier(cl, shown, CSRC, NPSR);
        GpuBarrier(cl, gcur, D3D12_RESOURCE_STATE_COMMON, NPSR); GpuBarrier(cl, gprev, D3D12_RESOURCE_STATE_COMMON, NPSR);
        for (int i = 0; ID3D12Resource* gen = FgWarpTarget(p->fg, i); ++i)
        {
            GpuBarrier(cl, gen, CSRC, UAV);
            CsWarp(g, p->sh, cl, shown, p->mv, gen, p->w, p->h, p->ww, p->wh, (i + 1.0f) / (float)FgMultiplier(p->fg), (UINT)cp.nrects,
                   gcur, gprev, p->gw, p->gh);
            GpuBarrier(cl, gen, UAV, CSRC);
        }
        GpuBarrier(cl, gcur, NPSR, D3D12_RESOURCE_STATE_COMMON); GpuBarrier(cl, gprev, NPSR, D3D12_RESOURCE_STATE_COMMON);
        GpuBarrier(cl, shown, NPSR, CSRC);
    }
    const bool fg_recorded = fg_dst && FgRecord(p->fg, cp.rects, (int)cp.nrects);
    stamp(2 * PS_LIST2 + 1);
    const UINT64 f2 = GpuEnd(g);
    if (!f2) return false;
    if (grid_q >= 0) p->mvgrid_q[grid_q].fence = f2;
    if (async && p->cmp_idx >= 0) p->residual_read_fence[p->cmp_idx] = f2;   // the model thread waits for it before rewriting that residual
    PipelineReadStamps(p);
    tw = NowMs();
    if (fg_recorded) FgSubmit(p->fg, f2, reset, p->cap_qpc, p->acq_qpc);
    p->cpu_wait[3].add(NowMs() - tw);
    if (evaluated && !async) NrRetireTick(p->nr);   // async: the model thread ticks
    p->last_evaluated = evaluated;   // async: composed with a residual
    return true;
}

// ---- live mode ----------------------------------------------------------------------------------------
static HWND FindTarget(const Config& c)
{
    // Both empty matches the FIRST window on the desktop, which is how a profile-less start would
    // attach itself to something arbitrary. A profile that names nothing targets nothing.
    // Full desktop: the desktop window stands for the primary monitor - DDA duplicates its output, WGC
    // captures the monitor, the overlay covers it and never hides for a foreground change.
    if (c.desktop) return GetDesktopWindow();
    if (c.window_class.empty() && c.window_title.empty()) return nullptr;
    // The LARGEST plausible match, not the first. A title like "World of Warcraft" is also carried by
    // launcher pages, taskbar thumbnails, tooltips and browser tabs, and first-in-Z-order once
    // attached the overlay to a 318x157 popup on the wrong monitor. A game window is a real,
    // visible, uncloaked top-level window and it is bigger than any of those; a minimised one is
    // judged by the size it restores to, so alt-tabbing out does not lose the target.
    HWND best = nullptr; LONGLONG best_area = 0;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, c.window_class.empty() ? nullptr : c.window_class.c_str(), nullptr)) != nullptr)
    {
        if (!c.window_title.empty())
        {
            wchar_t t[256] = {}; GetWindowTextW(h, t, 256);
            if (!wcsstr(t, c.window_title.c_str())) continue;
        }
        if (!IsWindowVisible(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) continue;
        RECT r = {};
        WINDOWPLACEMENT wp = { sizeof wp };
        if (IsIconic(h) && GetWindowPlacement(h, &wp)) r = wp.rcNormalPosition; else if (!GetWindowRect(h, &r)) continue;
        const LONGLONG w = r.right - r.left, ht = r.bottom - r.top;
        if (w < 320 || ht < 240) continue;
        if (w * ht > best_area) { best_area = w * ht; best = h; }
    }
    return best;
}

static void DumpFrame(Pipeline* p, const std::wstring& dir, int i)
{
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%ls\\dump_%03d_native.png", dir.c_str(), i); SaveTexPng(*p->g, p->color4k, p->w, p->h, NPSR, path);
    _snwprintf_s(path, _TRUNCATE, L"%ls\\dump_%03d_out.png", dir.c_str(), i); SaveTexPng(*p->g, p->shown, p->w, p->h, D3D12_RESOURCE_STATE_COPY_SOURCE, path);
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
    Config a; ConfigLoad((dir + L"\\justflow.ini").c_str(), nullptr, a);   // [app] profile=<name> pins the startup profile
    // none = start attached to nothing. The tray picks one, or "New profile from window..."
    // generates one. JustFlow should not grab a game the user did not ask it to.
    if (_wcsicmp(a.profile.c_str(), L"none") == 0) { index = -1; return L""; }
    if (!a.profile.empty() && _wcsicmp(a.profile.c_str(), L"auto") != 0)
    {
        index = find(a.profile);
        if (index < 0) Log("[main] justflow.ini [app] profile=%ls is not in profiles\\ - auto-picking", a.profile.c_str());
    }
    for (size_t i = 0; i < names.size() && index < 0; ++i)   // the first profile whose window is up right now
    { Config c; if ((ConfigLoad(nullptr, (pdir + L"\\" + names[i] + L".ini").c_str(), c) & 2) && FindTarget(c) && !c.desktop) index = (int)i; }
    if (index < 0) index = find(L"wow");
    if (index < 0) index = 0;
    return pdir + L"\\" + names[index] + L".ini";
}

void LogConfigFiles(const std::wstring& app, const std::wstring& profile, int have)
{
    Log("[config] app %ls (%s) + profile %ls (%s)", app.c_str(), have & 1 ? "loaded" : "missing - defaults", profile.c_str(), have & 2 ? "loaded" : "missing - defaults");
    const std::wstring stray = ConfigStrayKeys(profile.c_str());
    if (!stray.empty()) Log("[config] ignored in %ls (app-layer keys, they belong in justflow.ini): %ls", profile.c_str(), stray.c_str());
}

static int RealMain(int argc, char** argv);

// Windowed subsystem: no console window in live mode (the tray icon is the app). When launched
// from a console (bench, spike, --dump), attach to it so stdout still lands there.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr);
    }
    // __argv is NULL under a wide entry point (the UCRT only builds __wargv): make a narrow copy.
    int argc = 0; wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args; std::vector<char*> argv;
    for (int i = 0; i < argc; ++i) { std::string a; for (const wchar_t* w = wargv[i]; *w; ++w) a.push_back((char)*w); args.push_back(a); }   // ponytail: ASCII args only
    for (auto& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    if (wargv) LocalFree(wargv);
    return RealMain(argc, argv.data());
}

static int RealMain(int argc, char** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 1 ms timer resolution. Windows' default is 15.6 ms, and a Sleep(1) that waited a whole 15.6 ms
    // in the capture loop was the single worst bug of this project - it stalled a frame every time
    // the cursor moved. The same granularity applies to condition_variable timed waits, which is how
    // the presenter paces when vblank pacing is unavailable: a 4.17 ms deadline would become 15.6 ms
    // and the fallback would be worse than no pacing at all. Per-process since Win10 2004, so this
    // does not change the timer for the rest of the system.
    // Fail-safe: if it fails we are exactly where we were, and the vblank path does not need it.
    // Reported after LogInit, not here - a silent failure is what made the Sleep(1) bug take a day.
    const bool timer_1ms = timeBeginPeriod(1) == TIMERR_NOERROR;
    int dump = 0, preset = -1; bool settings_only = false;
    std::wstring ini_arg;   // --ini <file>: an explicit profile (next to the exe, or a path); else PickProfile
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--bench")) return RunBench(argc, argv);
        if (!strcmp(argv[i], "--settings")) settings_only = true;   // edit the ini files and exit; no GPU, no game
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) preset = atoi(argv[++i]);   // 0..3, written to the profile
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = atoi(argv[++i]);
        if (!strcmp(argv[i], "--ini") && i + 1 < argc) { const char* a = argv[++i]; ini_arg.assign(a, a + strlen(a)); }
    }
    const std::wstring dir = ExeDir(), app_path = dir + L"\\justflow.ini";   // app layer; the profile is the game layer
    std::vector<std::wstring> profiles; int profile = -1;
    std::wstring ini_path = PickProfile(dir, ini_arg, profiles, profile);
    if (preset >= 0)
    {
        PresetApply(ini_path.c_str(), preset);
        const int now = PresetCurrent(ini_path.c_str());   // re-read: the write is only real if it reads back
        wprintf(L"%ls -> %ls\n", ini_path.c_str(), now == preset ? PresetName(preset) : L"NOT APPLIED");
        return now == preset ? 0 : 1;
    }
    Config cfg;
    const int have = ConfigLoad(app_path.c_str(), ini_path.c_str(), cfg);
    // Standalone settings: dodge the game's monitor too, in case it is already running.
    if (settings_only) return SettingsDialog(nullptr, app_path.c_str(), ini_path.c_str(), L"JustFlow", FindTarget(cfg)) ? 0 : 1;
    // One LIVE instance only. Two of them both open Desktop Duplication, both put a topmost overlay
    // on the same window and both present to the same display - they fight, and every number in the
    // log becomes meaningless. Held for the life of the process; --settings / --preset / --bench are
    // short-lived tools and are deliberately not covered (they return above this).
    HANDLE only_one = CreateMutexW(nullptr, TRUE, L"Local\\JustFlow.SingleInstance");
    if (!only_one || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"JustFlow is already running.\n\nUse its tray icon: right-click the icon to quit, "
                             L"or press the quit hotkey (Ctrl+F12 by default).", L"JustFlow", MB_ICONINFORMATION | MB_OK);
        if (only_one) CloseHandle(only_one);
        return 1;
    }
    std::wstring log_path = JoinPath(dir, cfg.log_file);
    LogInit(log_path.c_str());
    if (!timer_1ms) Log("[main] 1 ms timer resolution refused - condition_variable waits fall back to ~15.6 ms, so timer pacing will be coarse");
    LogConfigFiles(app_path, ini_path, have);
    Log("[main] profile %ls (%zu in profiles\\)", ini_path.c_str(), profiles.size());
    Gpu g;
    // [gpu] adapter: a second card can host the whole filter. Desktop Duplication can only capture
    // an output its OWN adapter owns, and the present has to reach the display, so the monitor must
    // be plugged into whichever card this names - the capture device follows the same LUID.
    if (!GpuInit(g, cfg.gpu_adapter)) return 1;
    if (cfg.selftest) ComposeSelfTest(g);
    ResolveWork(cfg, dir);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);

    SetConsoleTitleW(L"JustFlow");
    Tray* tray = TrayCreate(L"JustFlow");
    if (!tray) Log("[tray] not available (no icon; hotkeys still work)");
    std::vector<const wchar_t*> pnames; for (auto& n : profiles) pnames.push_back(n.c_str());
    if (tray) TraySetProfiles(tray, pnames.data(), (int)pnames.size());
    if (tray) TraySetPaths(tray, app_path.c_str(), ini_path.c_str());

    Pipeline* p = nullptr;   // the live pipeline, null while waiting for the window
    bool quit = false; int pending_profile = -1, dumped = 0, rc = 0;
    bool go_idle = false;   // the active profile was removed: leave the pipeline and sit in the tray
    wchar_t status[128] = L"";
    auto profile_name = [&]() { return profile >= 0 ? profiles[profile] : Stem(ini_path); };
    auto tray_state = [&]
    {
        if (!tray) return;
        TrayState s; s.profile_index = profile; s.status = status;
        if (p) { s.nr_on = p->cfg.nr_enabled; s.filters_on = p->cfg.filters_enabled; s.fg_on = p->cfg.fg_enabled; s.fg_multiplier = p->cfg.fg_multiplier; s.fg_engine = p->cfg.fg_engine; s.wipe_mode = p->wipe; s.game = p->target; }
        TraySetState(tray, s);
    };
    auto apply_hotkeys = [&]   // cfg.hk_* -> overlay registration (a pipeline created later registers from cfg itself)
    {
        if (!p || !p->ov) return true;
        HotkeyDef k[kHotkeys]; HotkeyDefs(cfg, k);
        return OverlaySetHotkeys(p->ov, k, kHotkeys);
    };
    // F9 / F10 / F8 / F11 and their tray menu items
    // A toggle is a setting, not session state: write it back through its own layer so a restart
    // keeps it. Wipe is deliberately not persisted - it is a comparison view, not a preference.
    auto persist = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* value)
    {
        const std::wstring& file = ConfigIsAppKey(sec, key) ? app_path : ini_path;
        if (!WritePrivateProfileStringW(sec, key, value, file.c_str()))
            Log("[main] could not write [%ls] %ls to %ls", sec, key, file.c_str());
    };
    // flip a switch in the live pipeline and in the loaded config, and save it; returns the new state
    auto flip = [&](bool& live, bool& loaded, const wchar_t* sec, const wchar_t* key)
    {
        loaded = live = !live; persist(sec, key, live ? L"1" : L"0");
        Log("[main] [%ls] %ls=%d", sec, key, (int)live);
        return live;
    };
    auto toggle_nr = [&]
    {
        if (!p) return;
        if (flip(p->cfg.nr_enabled, cfg.nr_enabled, L"nr", L"enabled"))
        {
            p->force_reset = true;
            // First time on this session: the model loads now (see PipelineCreate).
            if (!p->nr)
            {
                p->nr = NrInit(g, ExeDir().c_str(), (NrParamBlock)p->cfg.param_block);
                if (p->nr) p->create_pending = true; else Log("[nr] init failed while enabling");
            }
        }
        if (!p->cfg.nr_enabled) PipelineToast(p, "Neural layer OFF");
        else if (p->nr) PipelineToast(p, "Neural layer ON");
        else PipelineToast(p, "Neural layer ON but the DLSS model did not load - see the log");
        tray_state();
    };
    auto cycle_wipe = [&] { if (!p) return; p->wipe = (p->wipe + 1) % 3; p->wipe_t0 = NowMs(); Log("[main] wipe %d", p->wipe); PipelineToast(p, "Wipe: %s", p->wipe == 1 ? "split" : p->wipe == 2 ? "sweep" : "off"); tray_state(); };
    auto toggle_fg = [&]
    {
        if (!p) return;
        if (!flip(p->cfg.fg_enabled, cfg.fg_enabled, L"fg", L"enabled")) PipelineToast(p, "Frame generation OFF");
        else if (p->cfg.fg_engine == FG_LATEWARP) PipelineToast(p, "Frame generation ON: latewarp");
        else PipelineToast(p, "Frame generation ON %dX (%ls)", p->cfg.fg_multiplier, FgEngineName(p->cfg.fg_engine));
        tray_state();
    };
    auto toggle_filters = [&]
    {
        if (!p) return;
        PipelineToast(p, "Filter layer %s", flip(p->cfg.filters_enabled, cfg.filters_enabled, L"filters", L"enabled") ? "ON" : "OFF");
        tray_state();
    };
    auto toggle_hud = [&] { if (!p) return; PipelineToast(p, "Status HUD %s", flip(p->hud, cfg.hud, L"ui", L"hud") ? "ON" : "OFF"); };
    // the caps in force, for the toasts: "uncapped" | "cap 60" | "cap 60  model 30/s" | "model 30/s"
    auto caps = [](const Config& c)
    {
        std::string s;
        if (c.max_fps > 0) s = "cap " + std::to_string(c.max_fps);
        if (c.model_max_fps > 0) s += (s.empty() ? "" : "  ") + std::string("model ") + std::to_string(c.model_max_fps) + "/s";
        return s.empty() ? std::string("uncapped") : s;
    };
    auto reload = [&]
    {
        Config nc; const int have = ConfigLoad(app_path.c_str(), ini_path.c_str(), nc); const bool ok = (have & 2) != 0;
        ResolveWork(nc, dir);
        if (!p)
        {
            // No pipeline yet (waiting for the game window): the config still has to land, or a
            // hotkey edited from the settings window would not take until the game came up.
            cfg = nc;
            LogConfigFiles(app_path, ini_path, have);
            Log("[main] config reloaded (no pipeline yet)");
            return;
        }
        // The device is created once at startup, so this one cannot be applied by any reload.
        if (nc.gpu_adapter != cfg.gpu_adapter)
        {
            Log("[gpu] adapter %d -> %d takes effect when JustFlow restarts", cfg.gpu_adapter, nc.gpu_adapter);
            PipelineToast(p, "GPU change applies on restart");
        }
        if (ok && profile >= 0 && ConfigNeedsRestart(cfg, nc))
        {
            // Capture and overlay read these only at create: rebuild the pipeline the way a profile
            // switch does (break the frame loop, destroy, come back round the top with the new ini).
            Log("[main] capture/overlay key changed - rebuilding the pipeline");
            PipelineToast(p, "Applying...");
            pending_profile = profile;
            return;
        }
        const bool cap_changed = nc.max_fps != cfg.max_fps || nc.model_max_fps != cfg.model_max_fps;
        PipelineReload(p, nc); cfg = nc;
        apply_hotkeys();
        LogConfigFiles(app_path, ini_path, have);
        Log("[main] config reloaded%s", ok ? "" : " (profile missing - defaults)");
        if (!ok) PipelineToast(p, "Reload failed");
        else if (cap_changed) { if (cfg.max_fps > 0 || cfg.model_max_fps > 0) PipelineToast(p, "Cap: %s", caps(cfg).c_str()); else PipelineToast(p, "Cap: none"); }
        else PipelineToast(p, "Profile reloaded: %ls", profile_name().c_str());
        tray_state();
    };
    auto handle_tray = [&]
    {
        TrayEvent ev; int arg;
        while (tray && TrayPoll(tray, ev, arg)) switch (ev)
        {
        case TrayToggleNr: toggle_nr(); break;
        case TrayToggleFilters: toggle_filters(); break;
        case TrayToggleFg: toggle_fg(); break;
        case TrayWipe:     cycle_wipe(); break;
        case TrayReload:   reload(); break;
        case TrayFgMultiplier:
            if (p) { Config nc = p->cfg; nc.fg_multiplier = arg; PipelineReload(p, nc); }   // like a reload: FG is recreated next frame
            cfg.fg_multiplier = arg; Log("[main] fg multiplier %d", arg); tray_state();
            persist(L"fg", L"multiplier", std::to_wstring(arg).c_str());   // saved, like every other toggle
            break;
        case TrayFgEngine:
        {
            const int e = std::clamp(arg, 0, FG_ENGINE_COUNT - 1);
            if (p) { Config nc = p->cfg; nc.fg_engine = e; PipelineReload(p, nc); }
            cfg.fg_engine = e; Log("[main] fg engine %ls", FgEngineName(e)); tray_state();
            persist(L"fg", L"engine", FgEngineName(e));
            if (p) PipelineToast(p, "Frame generation engine: %ls%s", FgEngineName(e), p->cfg.fg_enabled ? "" : " (FG is off - F8)");
            break;
        }
        case TraySelectProfile: if (arg >= 0 && arg < (int)profiles.size() && arg != profile) pending_profile = arg; break;
        case TrayRescanProfiles:
        {
            // PickProfile honours [app] profile, which the picker just pointed at the new file.
            std::vector<std::wstring> names; int idx = -1;
            PickProfile(dir, L"", names, idx);
            profiles = names;
            std::vector<const wchar_t*> pn; for (auto& n : profiles) pn.push_back(n.c_str());
            TraySetProfiles(tray, pn.data(), (int)pn.size());
            if (idx >= 0 && idx != profile) pending_profile = idx;
            else if (idx < 0 && profile >= 0) go_idle = true;   // [app] profile=none now (the active one was removed)
            Log("[main] profiles rescanned (%zu), selected %d", profiles.size(), idx);
            break;
        }
        case TrayOpenConfig:    ShellExecuteW(nullptr, L"open", ini_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case TrayOpenAppConfig: ShellExecuteW(nullptr, L"open", app_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case TrayOpenLog:       ShellExecuteW(nullptr, L"open", log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case TrayQuit:       Log("[main] quit (tray)"); quit = true; break;
        default: break;
        }
    };

    bool switched = false;   // the next pipeline comes from a tray profile switch (toast "Profile: x" instead of the startup line)
    while (!quit)
    {
        if (go_idle) { go_idle = false; profile = -1; ini_path.clear(); if (tray) TraySetPaths(tray, app_path.c_str(), L""); Log("[main] no active profile - idle"); }
        if (pending_profile >= 0)   // tray: switch profile (the pipeline is already torn down)
        {
            profile = pending_profile; pending_profile = -1; switched = true;
            ini_path = dir + L"\\profiles\\" + profiles[profile] + L".ini";
            if (tray) TraySetPaths(tray, app_path.c_str(), ini_path.c_str());
            Config nc; const int have = ConfigLoad(app_path.c_str(), ini_path.c_str(), nc);
            ResolveWork(nc, dir); cfg = nc;
            const std::wstring lp = JoinPath(dir, cfg.log_file);
            if (lp != log_path) { log_path = lp; LogInit(log_path.c_str()); }
            LogConfigFiles(app_path, ini_path, have);
            Log("[main] profile %ls", ini_path.c_str());
            if (tray) TrayNotify(tray, L"JustFlow", (L"Profile: " + profiles[profile]).c_str());
        }
        // No profile: sit in the tray. Nothing is captured and no window is touched until the user
        // picks a profile or generates one - "auto" is opt-in, not the default.
        if (profile < 0 || ini_path.empty())
        {
            _snwprintf_s(status, _TRUNCATE, L"no profile - pick one from the tray");
            tray_state();
            while (!quit && pending_profile < 0) { Sleep(100); handle_tray(); }
            continue;
        }
        _snwprintf_s(status, _TRUNCATE, L"%ls  waiting for window", profile_name().c_str()); tray_state();
        HWND target = nullptr;
        for (int i = 0; !quit && !go_idle && pending_profile < 0 && !(target = FindTarget(cfg)); ++i)
        {
            if (i % 30 == 0) Log("[main] waiting for window class=%ls title=%ls", cfg.window_class.c_str(), cfg.window_title.c_str());
            for (int t = 0; t < 10 && !quit && !go_idle && pending_profile < 0; ++t) { Sleep(100); handle_tray(); }
        }
        if (!target) continue;
        // FindTarget accepts a minimised window (judged by its restored size, so alt-tab keeps the
        // target), but its capture is the 219x30 caption: the pipeline refused it and the app exited.
        if (IsIconic(target))
        {
            _snwprintf_s(status, _TRUNCATE, L"%ls  window minimised", profile_name().c_str()); tray_state();
            for (int t = 0; t < 5 && !quit && !go_idle && pending_profile < 0; ++t) { Sleep(100); handle_tray(); }
            continue;
        }
        Log("[main] target window %p", (void*)target);
        Capture* cap = CaptureOpen(g, target, cfg.cursor, cfg.border, cfg.dda);
        // Desktop Duplication and monitor capture see the whole monitor, our overlay included:
        // exclusion is mandatory there, or it captures and re-presents itself.
        if (cap && (CaptureIsDda(cap) || cfg.desktop)) cfg.exclude_from_capture = true;
        if (!cap) { Sleep(1000); continue; }
        p = PipelineCreate(g, cfg, CaptureWidth(cap), CaptureHeight(cap), true, target);
        if (p) Log("[gpu] reserved %.0f MB of video memory (residency priority high)", GpuReserveCurrentUsage(g));
        if (!p) { CaptureClose(cap); rc = 1; break; }
        _snwprintf_s(status, _TRUNCATE, L"%ls", profile_name().c_str()); tray_state();
        if (switched) { switched = false; PipelineToast(p, "Profile: %ls", profile_name().c_str()); }
        else
        {
            char model[24]; if (p->nr) sprintf_s(model, "%up model", p->wh); else strcpy_s(model, "no model");
            PipelineToast(p, "JustFlow  %ls  %s  %s %.0f Hz  %s", profile_name().c_str(), model, CaptureIsDda(cap) ? "DDA" : "WGC", 1000.0 / OverlayVBlankMs(p->ov), caps(cfg).c_str());
        }

        bool reset = true, dormant = false;
        LONGLONG last_sysrel = 0;
        UINT frames = 0, skips = 0, rate_drops = 0; double last_processed_ms = 0;
        double win_t0 = NowMs();
        std::vector<double> cpu_ms;
        // wall time inside CaptureAcquire (DDA wait + D3D11 copy/Flush), accumulated over the attempts
        // (timeouts, rate drops) that precede each processed frame; one sample per processed frame
        StageStats acq_ms, hud_acq; double acq_acc = 0;
        // FG / model counters are handed over on read: the HUD tick (250 ms) and the [stats] line share one drain
        FgStatsOut fs_agg; UINT model_evals_acc = 0;
        double hud_t = NowMs(); UINT hud_frames = 0, hud_presented = 0, hud_model = 0;
        UINT hud_gen = 0, hud_nopair = 0, hud_disabled = 0, hud_preempt = 0;   // FG gates, live on the HUD
        auto drain = [&]
        {
            if (p->fg)
            {
                FgStatsOut fs; FgStats(p->fg, fs);
                fs_agg.presented += fs.presented; fs_agg.drops += fs.drops; hud_presented += fs.presented;
                fs_agg.gen_shown += fs.gen_shown; fs_agg.no_pair += fs.no_pair;
                fs_agg.disabled += fs.disabled; fs_agg.preempts += fs.preempts; fs_agg.paused += fs.paused;
                hud_gen += fs.gen_shown; hud_nopair += fs.no_pair;
                hud_disabled += fs.disabled; hud_preempt += fs.preempts;
                fs_agg.vblank_wait_sum_ms += fs.vblank_wait_sum_ms; fs_agg.vblank_waits += fs.vblank_waits;
                fs_agg.record_wait_sum_ms += fs.record_wait_sum_ms; fs_agg.record_waits += fs.record_waits;
                fs_agg.spacing_ms.insert(fs_agg.spacing_ms.end(), fs.spacing_ms.begin(), fs.spacing_ms.end());
                fs_agg.age_ms.insert(fs_agg.age_ms.end(), fs.age_ms.begin(), fs.age_ms.end());
                fs_agg.pipe_ms.insert(fs_agg.pipe_ms.end(), fs.pipe_ms.begin(), fs.pipe_ms.end());
            }
            const UINT me = p->model_evals.exchange(0); model_evals_acc += me; hud_model += me;
        };
        for (;;)
        {
            handle_tray();
            if (quit || go_idle || pending_profile >= 0) break;
            if (OverlayHotkey(p->ov, 4)) { Log("[main] quit hotkey"); quit = true; break; }
            if (OverlayHotkey(p->ov, 1)) toggle_nr();
            if (OverlayHotkey(p->ov, 2)) cycle_wipe();
            if (OverlayHotkey(p->ov, 5)) toggle_fg();
            if (OverlayHotkey(p->ov, 3)) reload();
            if (OverlayHotkey(p->ov, 6)) toggle_hud();
            if (OverlayHotkey(p->ov, 7)) toggle_filters();
            if (cap && CaptureLost(cap)) { Log("[main] capture lost - back to waiting for the window"); break; }
            UINT nw = 0, nh = 0;
            if (cap && CaptureSizeChanged(cap, nw, nh))
            {
                Log("[main] window settled at %ux%u - recreating capture", nw, nh);
                GpuWaitIdle(g);
                CaptureClose(cap);
                cap = CaptureOpen(g, target, cfg.cursor, cfg.border, cfg.dda);
                if (!cap || !PipelineResize(p, CaptureWidth(cap), CaptureHeight(cap))) { Log("[main] recreate failed"); break; }
                PipelineToast(p, "Capture %ux%u", CaptureWidth(cap), CaptureHeight(cap));
                reset = true; last_sysrel = 0;
                continue;
            }
            // ---- dormant -------------------------------------------------------------------------
            // With no layer doing anything the overlay is a strictly worse copy of the game: an opaque
            // topmost window re-presenting the capture at the CAPTURE rate, on top of a game that may
            // be running faster underneath (Dawnwalker's own frame generation), and a full-screen
            // composed window also costs the game independent flip and VRR. So get out of the way
            // completely: hide the overlay and RELEASE the desktop duplication, then idle on the
            // hotkeys and the tray. Any toggle that enables a layer wakes it within 50 ms.
            {
                const Config& lc = p->cfg;
                const bool neural = lc.nr_enabled;
                const bool filters = FiltersLive(lc);
                const bool active = neural || filters || lc.fg_enabled || p->wipe != 0 || dump > dumped;
                if (!active)
                {
                    if (!dormant)
                    {
                        dormant = true; GpuWaitIdle(g); OverlayHide(p->ov);
                        if (cap) { CaptureClose(cap); cap = nullptr; }
                        Log("[main] dormant: no layer is enabled - overlay hidden, capture released");
                        _snwprintf_s(status, _TRUNCATE, L"%ls  dormant (all layers off)", profile_name().c_str()); tray_state();
                    }
                    if (!IsWindow(target)) { Log("[main] target window gone"); break; }
                    Sleep(50);
                    continue;
                }
                if (dormant)
                {
                    dormant = false;
                    cap = CaptureOpen(g, target, cfg.cursor, cfg.border, cfg.dda);
                    if (!cap) { Log("[main] wake: capture did not reopen"); break; }
                    if (CaptureWidth(cap) != p->w || CaptureHeight(cap) != p->h) { if (!PipelineResize(p, CaptureWidth(cap), CaptureHeight(cap))) break; }
                    reset = true; last_sysrel = 0;
                    Log("[main] awake: a layer was enabled - capture reopened");
                    _snwprintf_s(status, _TRUNCATE, L"%ls", profile_name().c_str()); tray_state();
                }
            }
            UINT64 fv = 0; LONGLONG sysrel = 0;
            const double acq_t0 = NowMs();
            const bool acquired = CaptureAcquire(cap, 50, fv, sysrel);
            acq_acc += NowMs() - acq_t0;
            if (!acquired)
            {
                ++skips;
                // Every empty wait: a game that lost the foreground often stops rendering, and the
                // overlay must hide promptly (OverlayFollow throttles its own geometry query to 50 ms).
                OverlayFollow(p->ov, cfg.reassert_topmost_every);
                // No sleep here. CaptureAcquire already blocks on its own timeout (AcquireNextFrame /
                // the frame event), and every early return runs after that wait, so this cannot spin.
                // The Sleep(1) that was here stalled the capture loop on every MOUSE MOVE: Desktop
                // Duplication reports a cursor-only update (LastPresentTime == 0), we skip it, and without
                // timeBeginPeriod a Sleep(1) waits for the next 15.6 ms scheduler tick - so the real game
                // frame behind it sat waiting. Moving the mouse is exactly when FG matters, and it turned
                // into irregular capture, a jittery content interval, drops and warping. The stall was
                // after acq_ms and before cpu_ms, so no stat ever showed it.
                continue;
            }
            p->hdr_white = CaptureSdrWhite(cap);   // an HDR desktop duplicated as FP16: converted in the swizzle
            if (CaptureIsFloat(cap) && p->hdr_white <= 0) { Log("[main] FP16 (HDR) window capture is not supported - exiting"); quit = true; rc = 2; break; }
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
            acq_ms.add(acq_acc); hud_acq.add(acq_acc); acq_acc = 0;
            if (!PipelineFrame(p, CaptureTexture(cap), CaptureFence(cap), fv, reset)) { Log("[main] frame failed - exiting"); GpuLogDeviceRemoved(g, "frame"); quit = true; rc = 3; break; }
            reset = false;
            OverlayFollow(p->ov, cfg.reassert_topmost_every);
            if (cfg.stats_every > 0) cpu_ms.push_back(NowMs() - t0);   // only the [stats] tick reads it, and only that tick clears it
            if (p->last_evaluated && dumped < dump) DumpFrame(p, dir, dumped++);
            ++hud_frames;
            if (p->hud && NowMs() - hud_t >= 250.0)   // status HUD: two lines from the live counters
            {
                drain();
                const double dt = NowMs() - hud_t, cap_fps = hud_frames * 1000.0 / dt, out_fps = hud_presented * 1000.0 / dt;
                const double age = StageStats{ fs_agg.age_ms }.med();
                char capstr[12]; if (cfg.max_fps > 0) sprintf_s(capstr, "%d", cfg.max_fps); else strcpy_s(capstr, "none");
                snprintf(p->hud_line[0], sizeof p->hud_line[0], "in %.0f  out %.0f  age %.0f ms  acq %.1f ms  cap %s", cap_fps, out_fps, std::max(0.0, age), std::max(0.0, hud_acq.med()), capstr);
                hud_acq.v.clear();
                // snprintf, not sprintf_s: an overflow here must clip the HUD, not fast-fail the
                // process (sprintf_s calls the invalid-parameter handler, which is a hard kill).
                char nr[48];
                // Which LAYERS are live, not which settings are set: the three switches are
                // independent, so the HUD has to be able to say "neural off, filters on, FG on".
                if (!p->cfg.nr_enabled) strcpy_s(nr, "neural off");
                else if (!p->nr) strcpy_s(nr, "neural: no model");
                else if (cfg.nr_async)
                {
                    double ms; { std::lock_guard<std::mutex> lk(p->pub_mu); ms = p->model_ms.med(); }
                    snprintf(nr, sizeof nr, "NR %.1f ms %up async %.0f fps", std::max(0.0, ms), p->wh, hud_model * 1000.0 / dt);
                    if (cfg.model_max_fps > 0) snprintf(nr + strlen(nr), sizeof nr - strlen(nr), " cap %d/s", cfg.model_max_fps);
                }
                else if (p->st[PS_EVAL].med() < 0) snprintf(nr, sizeof nr, "NR -- ms %up", p->wh);   // no sample is not 0.0 ms
                else snprintf(nr, sizeof nr, "NR %.1f ms %up", p->st[PS_EVAL].med(), p->wh);
                char mask[8]; if (p->mask_active) sprintf_s(mask, "%d", p->mask_n); else strcpy_s(mask, "-");
                const char* filt_s = !cfg.filters_enabled ? "off" : FiltersLive(cfg) ? "on" : "-";
                if (p->fg && FgMultiplier(p->fg) > 1 && FgPaused(p->fg)) snprintf(p->hud_line[1], sizeof p->hud_line[1], "%s  filt %s  FG auto-paused  mask %s", nr, filt_s, mask);
                else if (p->fg && FgMultiplier(p->fg) > 1) snprintf(p->hud_line[1], sizeof p->hud_line[1], "%s  filt %s  FG %dX %.1fms  mask %s", nr, filt_s, FgMultiplier(p->fg), std::max(0.0, FgEvalMs(p->fg, nullptr)), mask);
                else snprintf(p->hud_line[1], sizeof p->hud_line[1], "%s  filt %s  FG off  mask %s", nr, filt_s, mask);
                // Why FG is or is not paying off: shown generated frames, then the gate that ate the rest.
                if (p->fg && FgMultiplier(p->fg) > 1 && hud_frames)
                    snprintf(p->hud_line[2], sizeof p->hud_line[2], "gen %.0f%%  nopair %u  off %u  late %u", hud_gen * 100.0 / hud_frames, hud_nopair, hud_disabled, hud_preempt);
                else p->hud_line[2][0] = 0;
                hud_t = NowMs(); hud_frames = 0; hud_presented = 0; hud_model = 0;
                hud_gen = 0; hud_nopair = 0; hud_disabled = 0; hud_preempt = 0;
            }
            // stats_every <= 0 means OFF. It used to clamp to 1, so the one value a user would pick
            // to silence the log instead ran the most expensive block in the loop - GpuVram, three
            // vector sorts and a thirty-argument Log - on every single frame.
            if (cfg.stats_every > 0 && ++frames % (UINT)cfg.stats_every == 0)
            {
                StageStats cpu{ cpu_ms }, spacing, age, pipe;
                double gpu = 0;
                // The real cost: both whole submissions plus the flow they wait on, not a sum of
                // the stages someone remembered to wrap in stamps.
                gpu = std::max(0.0, p->st[PS_LIST1].med()) + std::max(0.0, p->st[PS_LIST2].med()) + std::max(0.0, p->st[PS_OFA].med());
                drain();
                FgStatsOut fs; std::swap(fs, fs_agg);
                spacing.v.swap(fs.spacing_ms); age.v.swap(fs.age_ms); pipe.v.swap(fs.pipe_ms);
                double fg_eval_p95 = -1; const double fg_eval = p->fg ? FgEvalMs(p->fg, &fg_eval_p95) : -1;
                StageStats mm; { std::lock_guard<std::mutex> lk(p->pub_mu); mm.v.swap(p->model_ms.v); }
                const UINT model_evals = model_evals_acc; model_evals_acc = 0;
                const double span = NowMs() - win_t0, cap_fps = frames * 1000.0 / span, fg_fps = fs.presented * 1000.0 / span;
                double vram_used = -1, vram_budget = -1; GpuVram(g, vram_used, vram_budget);
                double pres_prev = -1, pres_call = -1, pres_total = -1;
                if (p->ov) OverlayPresentStats(p->ov, pres_prev, pres_call, pres_total);
                char mask[16]; if (p->mask_active) sprintf_s(mask, "%d", p->mask_n); else strcpy_s(mask, "none");
                Log("[stats] cap_fps=%.1f eval_ms=%.2f/%.2f(med/p95) frame_gpu_ms=%.2f cpu_ms=%.2f acq_ms=%.2f dda_accum=%.2f dda_foreign=%u static_skips=%u rate_drops=%u dups=%u cuts=%u age_ms=%.1f pipe_ms=%.1f fg_out_fps=%.1f fg_spacing_ms=%.2f/%.2f(med/p95) fg_drops=%u fg_gen=%u fg_nopair=%u fg_disabled=%u fg_preempt=%u fg_paused=%u fg_eval_ms=%.2f/%.2f(med/p95) pres_ms=%.2f(prev %.2f, call %.2f) vbwait_ms=%.2f recwait_ms=%.2f/%u model_fps=%.1f model_ms=%.2f residual_age_frames=%.1f mask=%s vram_mb=%.0f/%.0f",
                    cap_fps, p->st[PS_EVAL].med(), p->st[PS_EVAL].p95(), gpu, cpu.med(), acq_ms.med(), CaptureAccumMean(cap), CaptureForeign(cap), skips, rate_drops, p->dups, p->cuts, age.med(), pipe.med(),
                    fg_fps, spacing.med(), spacing.p95(), fs.drops, fs.gen_shown, fs.no_pair, fs.disabled, fs.preempts, fs.paused, fg_eval, fg_eval_p95, pres_total, pres_prev, pres_call, fs.vblank_waits ? fs.vblank_wait_sum_ms / fs.vblank_waits : -1.0,
                    fs.record_waits ? fs.record_wait_sum_ms / fs.record_waits : -1.0, fs.record_waits,
                    model_evals * 1000.0 / span, mm.med(), p->residual_age.med(), mask, vram_used, vram_budget);
                _snwprintf_s(status, _TRUNCATE, L"%ls  cap %.0f  out %.0f fps  age %.0f ms", profile_name().c_str(), cap_fps, fg_fps, std::max(0.0, age.med()));
                tray_state();
                p->residual_age.v.clear();
                Log("[stats] gpu: list1=%.2f list2=%.2f (ofa between them is bench-only) | swz=%.2f gray+ds=%.2f expand=%.2f eval=%.2f compose=%.2f filter=%.2f | cpu waits: begin1=%.1f ofa=%.1f begin2=%.1f present=%.1f",
                    p->st[PS_LIST1].med(), p->st[PS_LIST2].med(),
                    p->st[PS_SWIZZLE].med(), p->st[PS_GRAYDS].med(), p->st[PS_EXPAND].med(), p->st[PS_EVAL].med(), p->st[PS_COMPOSE].med(), p->st[PS_FILTER].med(),
                    p->cpu_wait[0].med(), p->cpu_wait[1].med(), p->cpu_wait[2].med(), p->cpu_wait[3].med());
                for (auto& s : p->cpu_wait) s.v.clear();
                frames = 0; skips = 0; rate_drops = 0; p->dups = 0; p->cuts = 0; win_t0 = NowMs(); cpu_ms.clear(); acq_ms.v.clear();
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
