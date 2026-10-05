#include "fg.h"
#include "config.h"   // FgEngine
#include "compose.h"   // CsVideoInterp
#include "latewarp.h"
#include "mouse.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "log.h"
#include "ngx_nr.h"          // NgxMutex
#include "nvsdk_ngx.h"
#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using PFN_FgInitExt  = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using PFN_FgCreate   = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_FgEvaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);
using PFN_FgRelease  = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// The DLSS-G helper header hard-codes the core's EvaluateFeature_C; route it through a pointer so
// the direct-DLL fallback can use the same parameter-setting code (NeuralScreen trick).
static PFN_FgEvaluate g_fg_evaluate = nullptr;
static NVSDK_NGX_Result NVSDK_CONV FgEvalBridge(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback_C cb)
{ return g_fg_evaluate(cl, h, p, cb); }
#define NVSDK_NGX_D3D12_EvaluateFeature_C FgEvalBridge
#include "nvsdk_ngx_helpers_dlssg.h"
#undef NVSDK_NGX_D3D12_EvaluateFeature_C

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const D3D12_RESOURCE_STATES CSRC = D3D12_RESOURCE_STATE_COPY_SOURCE;
static const D3D12_RESOURCE_STATES CDST = D3D12_RESOURCE_STATE_COPY_DEST;
static const int kSlots = 4, kMaxGen = 3;   // 4: engine=video holds three real frames while the pipeline writes a fourth

// Rest states: real CSRC, gen[] CSRC, mv/depth NPSR, disable UAV. state: 0 free, 1 writing
// (producer), 2 ready, 3 presenting; guarded by Fg::mu.
// Queues: the main queue writes real (list 2 composes into it; fence = Gpu::fence value) - and gen[]
// too for engine=warp; the FG queue (Fg::ctx) reads real and writes gen[] for DLSS-G (eval_fence = ctx
// fence value); the present queue reads real/gen[] after the fence that wrote them. Reuse: FgAcquire
// makes the main queue wait on eval_fence and on the last present copy before list 2 writes the slot.
struct FgSlot
{
    ID3D12Resource *real = nullptr, *gen[kMaxGen] = {};
    bool     gen_ok = false;   // engine=warp: the pipeline wrote gen[] for THIS frame (not while paused); engine=video: DLSS-G did
    bool     resolved = true;  // engine=video: the DLSS-G evaluate of this frame has been collected (EvaluateResolve)
    ID3D12Resource* mask = nullptr;   // engine=latewarp: Frame Warp's no-warp mask for this frame (R8, NPSR)
    bool     mask_ok = false;  // the pipeline wrote it for THIS frame
    ID3D12Resource* flow = nullptr;   // engine=video: this frame's backward flow to the previous one (R16G16_FLOAT, NPSR)
    int    state = 0;
    UINT64 seq = 0, fence = 0, eval_fence = 0;
    int    eval_slot = -1;    // ctx ring slot the evaluate stamped, read back when the fence lands
    UiRect rects[64] = {};    // the addon UI mask for this frame, in output pixels
    int    nrects = 0;
    bool   interpolate = false;
    double interval = 16.0;   // ms between this and the previous submit (our clock)
    double content_ms = 0;    // ms between this frame and the previous CAPTURED one, in the game's
                              // own clock: 0 = the capture gave no timestamp
    LONGLONG cap_qpc = 0, acq_qpc = 0;
};

struct Fg
{
    Gpu*     g = nullptr;
    FgSlot*  dbg = nullptr;   // last slot evaluated, for FgDebugGen
    Overlay* ov = nullptr;
    UINT     w = 0, h = 0, mw = 0, mh = 0;
    int      count = 1;                      // generated frames per real frame (0 = passthrough)
    int      gens = 1;                       // frames each DLSS-G evaluate makes: count; engine=video kMaxGen (0 = no DLSS-G)
    bool     warp = false;                   // engine=warp: the pipeline extrapolates into s->gen, no DLSS-G
    bool     lw = false;                     // engine=latewarp: every refresh re-projects the newest frame to the mouse (Reproject)
    Latewarp* lwf = nullptr; ID3D12Resource* lw_final[2] = {}; float lw_vfov = 1.0472f; ID3D12Resource* lw_last = nullptr;   // lw_last: bench
    bool     video = false;                  // engine=video: VideoPresent; lw_final/lw_last hold its interpolated frames
    Shaders* sh = nullptr;                   // engine=video: CsVideoInterp on the FG queue
    bool     vblank = true;
    std::atomic<double> phase{ 0.0 };        // FgSetTiming (main) -> Presenter
    std::atomic<double> min_gain{ 1.5 };     // governor threshold: presented / submitted (0 = governor off)
    std::atomic<double> max_in{ 90.0 };      // governor floor: no generation while the game already delivers more than this (0 = off)
    std::atomic<bool>   paused{ false };     // governor has generation switched off (HUD)
    std::atomic<UINT>   paused_frames{ 0 };  // real frames presented while paused
    std::wstring dir;
    HMODULE  dll = nullptr;
    PFN_FgCreate create = nullptr; PFN_FgRelease release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    ID3D12Resource *mv = nullptr, *depth = nullptr, *disable = nullptr, *disable_rb = nullptr;
    FgSlot   slots[kSlots];
    FgSlot*  pending = nullptr;              // reserved by FgRecord, handed over by FgSubmit
    UINT64   seq = 0; double last_submit = 0; bool history = false;
    LONGLONG last_cap = 0;                   // cap_qpc of the previous submit (FgSubmit only)
    // presenter thread
    std::thread thread;
    std::mutex mu; std::condition_variable cv;
    std::atomic<bool> stop{ false }, failed{ false }, hold{ false };   // hold: bench, the latewarp presenter idles so its debug textures stay put
    std::atomic<UINT> presented{ 0 }, drops{ 0 };
    // why a real frame produced no generated frame: no pair (sequence gap / reset),
    // DLSS-G raised its disable flag, or a newer slot pre-empted the schedule
    std::atomic<UINT> no_pair{ 0 }, disabled{ 0 }, preempts{ 0 }, gen_shown{ 0 };
    std::atomic<UINT64> vbw_us{ 0 }, vbw_n{ 0 };   // time the presenter sits in OverlayWaitVBlank
    std::atomic<UINT>   vbw_fail{ 0 };            // vblank wait failures
    std::atomic<UINT64> rec_us{ 0 }, rec_n{ 0 };  // main thread blocked in FgRecord with no free slot
    std::vector<double> spacing, age, pipe;   // guarded by mu; handed over by FgStats
    double eval_ring[256] = {}; unsigned eval_n = 0;   // guarded by mu; generation GPU ms per real frame (FgEvalMs)
    double last_present = 0;
    GpuCtx ctx;   // the FG queue: DLSS-G evaluates never sit behind NR / compose on Gpu::queue
};

static bool Fail(Fg* f, const char* why)
{
    if (!f->failed.exchange(true)) Log("[fg] disabled: %s", why);
    return false;
}

// The NEWEST ready slot, marked presenting; every older ready slot is dropped on the spot (counted).
// Caller holds f->mu. nullptr when nothing is ready.
static FgSlot* TakeNewestLocked(Fg* f)
{
    FgSlot* s = nullptr;
    for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq > s->seq)) s = &x;
    for (auto& x : f->slots) if (x.state == 2 && &x != s) { x.state = 0; ++f->drops; }
    if (s) s->state = 3;
    return s;
}

// CPU wait for the slot's list-2 writes (its render fence) - before presenting it or anything made from it.
static bool WaitRendered(Fg* f, const FgSlot* s) { return GpuCtxWait(f->ctx, f->g->fence, s->fence, 10000) || Fail(f, "render fence wait (device removed?)"); }

// ---- NGX calls with SEH (no unwindable objects in these frames) --------------------------------
static NVSDK_NGX_Result SafeCreate(Fg* f, ID3D12GraphicsCommandList* cl, DWORD* code)
{
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = f->create(cl, NVSDK_NGX_Feature_FrameGeneration, f->params, &f->feature); } __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); }
    return r;
}
static NVSDK_NGX_Result SafeEvaluate(Fg* f, ID3D12GraphicsCommandList* cl, NVSDK_NGX_D3D12_DLSSG_Eval_Params* ep, NVSDK_NGX_DLSSG_Opt_Eval_Params* opt, DWORD* code)
{
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = NGX_D3D12_EVALUATE_DLSSG(cl, f->feature, f->params, ep, opt); } __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); }
    return r;
}

// ---- presenter thread ---------------------------------------------------------------------------
// Copy src (a texture of slot s) into the overlay backbuffer (present queue, after the slot's
// evaluate on the FG queue; passthrough: after its render fence) and present. `real`: this is the
// slot's real frame (latency stats).
static bool PresentAfter(Fg* f, ID3D12Resource* src, const FgSlot* s, bool real, ID3D12Fence* after, UINT64 after_value)
{
    if (!OverlayPresent(f->ov, src, after, after_value)) return Fail(f, "Present failed");
    const FgSlot* real_of = real ? s : nullptr;
    const LONGLONG pq = OverlayPresentQpc(f->ov);
    const double t = QpcToMs(pq);
    ++f->presented;
    std::lock_guard<std::mutex> lk(f->mu);
    auto push = [](std::vector<double>& v, double x) { if (v.size() < 4096) v.push_back(x); };   // bounded if nobody reads
    if (f->last_present > 0) push(f->spacing, t - f->last_present);
    f->last_present = t;
    if (real_of && real_of->cap_qpc) { push(f->age, QpcToMs(pq - real_of->cap_qpc)); push(f->pipe, QpcToMs(pq - real_of->acq_qpc)); }
    return true;
}

static bool Present(Fg* f, ID3D12Resource* src, const FgSlot* s, bool real, bool evaluated = true)
{
    return (f->count && evaluated) ? PresentAfter(f, src, s, real, f->ctx.fence, s->eval_fence) : PresentAfter(f, src, s, real, f->g->fence, s->fence);
}

// The slot's UI rects copied from src into dst (both COPY_SOURCE at rest): the UI never moves with
// the world. Axis-aligned boxes from the addon, so a copy per rect - no shader, no descriptors.
static void CopyRects(Fg* f, ID3D12GraphicsCommandList* cl, const FgSlot* s, ID3D12Resource* src_tex, ID3D12Resource* dst_tex)
{
    if (s->nrects <= 0) return;
    GpuBarrier(cl, dst_tex, CSRC, CDST);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = src_tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = dst_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (int r = 0; r < s->nrects; ++r)
    {
        const UINT x0 = (UINT)std::clamp(s->rects[r].x0, 0, (int)f->w), y0 = (UINT)std::clamp(s->rects[r].y0, 0, (int)f->h);
        const UINT x1 = (UINT)std::clamp(s->rects[r].x1, 0, (int)f->w), y1 = (UINT)std::clamp(s->rects[r].y1, 0, (int)f->h);
        if (x1 <= x0 || y1 <= y0) continue;
        const D3D12_BOX box = { x0, y0, 0, x1, y1, 1 };
        cl->CopyTextureRegion(&dst, x0, y0, 0, &src, &box);
    }
    GpuBarrier(cl, dst_tex, CDST, CSRC);
}

static bool Evaluate(Fg* f, FgSlot* s, bool interpolate)
{
    Gpu& g = *f->g;
    if (!GpuCtxBegin(g, f->ctx)) return Fail(f, "FG queue begin (device removed?)");
    ID3D12GraphicsCommandList* cl = f->ctx.list;
    GpuBarrier(cl, s->real, CSRC, NPSR);
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    for (int i = 0; i < 4; ++i)
        opt.cameraViewToClip[i][i] = opt.clipToCameraView[i][i] = opt.clipToLensClip[i][i] = opt.clipToPrevClip[i][i] = opt.prevClipToClip[i][i] = 1.0f;
    // mv: a constant zero field (see below), so the scale is only a formality.
    opt.mvecScale[0] = 1.0f / (float)f->mw; opt.mvecScale[1] = 1.0f / (float)f->mh;
    opt.cameraUp[1] = opt.cameraRight[0] = opt.cameraFwd[2] = 1.0f;
    opt.cameraNear = 0.1f; opt.cameraFar = 1000.0f; opt.cameraFOV = 1.0f;
    opt.cameraAspectRatio = (float)f->w / (float)f->h;
    // Decisions (nvsdk_ngx_params_dlssg.h):
    //  cameraMotionIncluded = true: there is no separate camera term to add.
    //  orthoProjection = true + identity matrices + flat depth: no camera model exists for a capture.
    //  colorBuffersHDR = false: RGBA8 UNORM (sRGB-encoded SDR) backbuffer.
    //  motionVectorsInvalidValue: "which value represents an invalid (un-initialized) value" - the
    //    zero-init default (0) would flag our whole zero field as invalid, which scores worse.
    //  The motion vectors are a constant ZERO field, and that is not a shortcut: DLSS-G measures the
    //    motion itself and, with this camera model, ignores the values it is given. Scored against
    //    the true in-between frame (tools/scene rendered at 2x, fed every other frame): our optical
    //    flow, zeros and a field deliberately 50 px wrong all give 1.089 grey levels at 10 px/frame
    //    and 3.22 at 5x that motion (run-to-run noise 0.01). Only the validity flag counts - the
    //    invalid value is worse (1.32). The OFA pass it used to cost was 0.4-0.6 ms a frame.
    //  motionVectorsDilated = true: skips DLSS-G's dilation pass over a field with nothing in it.
    opt.cameraMotionIncluded = opt.orthoProjection = true;
    opt.motionVectorsDilated = true;
    opt.motionVectorsInvalidValue = -65504.0f;
    opt.colorBuffersHDR = false;
    opt.reset = !interpolate;
    opt.mvecsSubrectSize = opt.depthSubrectSize = { f->mw, f->mh };
    opt.backbufferSubrectSize = opt.outputInterpSubrectSize = { f->w, f->h };
    opt.multiFrameCount = (unsigned)f->gens;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
    ep.pBackbuffer = s->real; ep.pMVecs = f->mv; ep.pDepth = f->depth; ep.pOutputDisableInterpolation = f->disable;
    f->params->Reset();
    f->params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)s->seq);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success; DWORD code = 0;
    for (int i = 0; i < f->gens; ++i)
    {
        opt.multiFrameIndex = (unsigned)i + 1;
        ep.pOutputInterpFrame = s->gen[i];
        GpuBarrier(cl, s->gen[i], CSRC, UAV);
        GpuCtxStamp(f->ctx, 2 * i);
        { std::lock_guard<std::mutex> lk(NgxMutex()); r = SafeEvaluate(f, cl, &ep, &opt, &code); }
        GpuCtxStamp(f->ctx, 2 * i + 1);
        GpuBarrier(cl, f->disable, UAV, CSRC);
        cl->CopyBufferRegion(f->disable_rb, (UINT64)i * 4, f->disable, 0, 4);
        GpuBarrier(cl, f->disable, CSRC, UAV);
        GpuBarrier(cl, s->gen[i], UAV, CSRC);
        if (code || NVSDK_NGX_FAILED(r)) break;
    }
    GpuBarrier(cl, s->real, NPSR, CSRC);
    // The UI back onto every generated frame. DLSS-G interpolates the whole image, so text and
    // action bars smear along whatever motion the world had - the one documented cost of turning the
    // multiplier up. The rects come from the addon, so they are the game's own frame geometry rather
    // than a guess, and they are axis-aligned: a copy per rect, no shader and no descriptors.
    // Real and generated frames then agree inside the rects, which is what stops the UI shimmering.
    for (int i = 0; i < f->gens; ++i) CopyRects(f, cl, s, s->real, s->gen[i]);
    f->ctx.queue->Wait(g.fence, s->fence);   // GPU-side: the slot's real frame (list 2) is complete on the main queue
    const int slot = f->ctx.slot;
    const UINT64 v = GpuCtxEnd(f->ctx);
    if (!v) return Fail(f, "FG queue submit");
    s->eval_fence = v;
    s->eval_slot = slot;
    f->dbg = s;
    if (code) { Log("[fg] evaluate raised 0x%08X", code); return Fail(f, "evaluate raised an exception"); }
    if (NVSDK_NGX_FAILED(r)) { Log("[fg] evaluate -> 0x%08X (%s)", r, NgxResultName(r)); return Fail(f, "evaluate failed"); }
    return true;
}

// The blocking half, split off so the caller can pace first: DLSS-G's ~1.5 ms then runs inside a
// wait the presenter was going to make anyway instead of serialising in front of the schedule.
// Only the CPU needs this - Present already orders the present queue on eval_fence.
static bool EvaluateResolve(Fg* f, FgSlot* s, bool& allow)
{
    if (!GpuCtxWait(f->ctx, f->ctx.fence, s->eval_fence, 10000)) return Fail(f, "evaluate fence wait (device removed?)");
    double ms[kMaxGen] = {};   // generation cost of this real frame = the sum of its evaluates
    if (s->eval_slot >= 0 && GpuCtxStampsMsSlot(f->ctx, s->eval_slot, ms, f->gens))
    {
        double sum = 0; bool valid = true;
        for (int i = 0; i < f->gens; ++i) { if (ms[i] < 0) valid = false; sum += ms[i]; }
        if (valid) { std::lock_guard<std::mutex> lk(f->mu); f->eval_ring[f->eval_n++ % 256] = sum; }
    }
    allow = true;
    uint8_t* d = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)f->gens * 4 };
    if (SUCCEEDED(f->disable_rb->Map(0, &rr, (void**)&d)))
    {
        for (int i = 0; i < f->gens; ++i) allow = allow && d[i * 4] == 0;
        D3D12_RANGE none = { 0, 0 }; f->disable_rb->Unmap(0, &none);
    }
    else allow = false;
    return true;
}

static void Presenter(Fg* f)
{
    using clock = std::chrono::steady_clock;
    double vb = f->vblank ? OverlayVBlankMs(f->ov) : 0;   // 0 = timer pacing
    UINT64 prev = 0; double prev_real_target = 0;
    // ---- governor ---------------------------------------------------------------------------------
    // Frame generation is only worth its GPU time and its added latency while it is actually
    // multiplying the frame rate. Gain = frames PRESENTED / frames SUBMITTED, per ~1 s window -
    // against submitted, not against the real frames we happened to show: a window that showed 52
    // real + 34 generated out of 107 submitted reads 1.65x the naive way and is really 0.8x.
    // Two bad windows pause generation (no Evaluate at all - dropped generated frames were still
    // being paid for); a probe re-engages after a back-off that doubles 3 s -> 30 s while it keeps
    // failing. The switch itself must not hitch: with generation on, the real frame is held back
    // count*L behind its arrival, so stepping that hold to zero collapses one interval by half.
    // DRAIN and RAMP slew it by 10% a frame instead - about a tenth of a second either way.
    // A second, simpler rule: above `max_in` input fps generation is off regardless of gain. A game
    // at 180 fps on a 240 Hz panel measured 0.6-0.95x (2x needs 360 Hz) and paid FG's evaluate
    // for the probes; and even where 2x fits, doubling 120 is latency for smoothness nobody sees.
    enum { G_ENGAGED, G_DRAIN, G_PAUSED, G_RAMP }; int gstate = G_ENGAGED;
    double hold = 0, gwin_t0 = NowMs(), backoff = 3000.0, resume_at = 0; UINT64 gwin_seq0 = 0; UINT gwin_presented = 0; int gbad = 0;
    auto newer_ready = [&](UINT64 seq) { for (auto& x : f->slots) if (x.state == 2 && x.seq > seq) return true; return false; };
    // Pre-emption is gone, and it is only safe to remove it BECAUSE the pick above now takes the
    // newest slot and frees the rest. Under the old oldest-first FIFO it was load-bearing - the
    // only way to skip ahead in a backlog - and removing it there was a disaster (age 168-260 ms,
    // 10 fps out). With newest-and-drop no backlog can exist: the queue is emptied at every pick.
    // Cancelling on "a newer slot is ready" then just discards a frame DLSS-G has already been paid
    // to make, and it fired constantly for a structural reason - the producer submits every
    // ~11 ms while the generated frame waits L = interval/2 ~ 5.5 ms, so a new slot lands inside
    // that window about half the time. That is exactly the measured 50%: gen 90, preempt 90.
    // Blocks until `target` (NowMs clock): vblank mode returns right after the vblank nearest the
    // target, timer mode at the target. False = stop.
    auto wait_until = [&](double target) -> bool
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk(f->mu);
                if (vb <= 0)
                {
                    const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(target - NowMs()));
                    return !f->cv.wait_until(lk, deadline, [&] { return (bool)f->stop; });
                }
                if (f->stop) return false;
            }
            LARGE_INTEGER vt0, vt1; QueryPerformanceCounter(&vt0);
            const bool vok = OverlayWaitVBlank(f->ov);
            QueryPerformanceCounter(&vt1);
            f->vbw_us += (UINT64)(QpcToMs(vt1.QuadPart - vt0.QuadPart) * 1000.0); ++f->vbw_n;
            // OverlayWaitVBlank only fails structurally (no DXGI output under the overlay, or no wait
            // that actually waits - it already fell back to DwmFlush), so this one IS permanent and the
            // timer schedule is the right answer. It is logged now rather than silent - a run that
            // quietly lost vblank pacing looked like a pacing bug for hours.
            if (!vok) { Log("[fg] no usable vblank wait under the overlay - pacing on the CPU timer from here"); vb = 0; continue; }
            if (NowMs() + vb * 0.5 >= target) return true;
        }
    };
    while (!f->stop && !f->failed)
    {
        FgSlot* s = nullptr; UINT64 seq_now = 0;
        {
            std::unique_lock<std::mutex> lk(f->mu);
            f->cv.wait(lk, [&] { return f->stop || newer_ready(0); });
            if (f->stop) break;
            // NEWEST ready, and free every other ready slot on the spot. Oldest-first FIFO is what
            // broke frame generation, and the reason is arithmetic rather than taste: the presenter
            // is serial across slots and schedules the real frame at prev_real_target + (count+1)*L,
            // which IS prev_real_target + interval. Its retire period while generating therefore
            // equals the producer's submit period exactly, with no margin - it can never work off
            // a backlog. Lateness could only be shed through pre-emption, which exists precisely to
            // throw the generated frame away, so the queue had two equilibria: 2x with zero
            // stability, and 1x backlogged that nothing could escape. Every jitter source pushed it
            // into the second and none pushed it back. Keeping only the freshest frame means a
            // backlog cannot form, so the lag that arms pre-emption never builds.
            s = TakeNewestLocked(f);
            seq_now = f->seq;
        }
        bool ok = true;
        bool allow = false;
        // Pace from CONTENT time. cap_qpc is the game's own present timestamp, so the gap to the
        // last frame we evaluated is exactly the span DLSS-G interpolates across, and it is immune
        // to anything happening on our threads. s->interval is submit-to-submit and therefore
        // carries our own stalls back into the cadence that caused them. Fall back to it only when
        // the capture gave us no timestamp.
        const double span = (s->content_ms > 0.5 && s->content_ms < 100.0) ? s->content_ms : s->interval;
        // A slot we dropped does NOT invalidate the pair. DLSS-G's history holds the last frame we
        // EVALUATED, and we evaluate every slot we pick, so a gap only widens the motion delta -
        // which `span` has just measured. Requiring seq == prev + 1 spent a generated frame on
        // every dropped one: fg_drops and fg_nopair came back equal on every single line.
        const bool interp = s->interpolate && prev != 0 && span < 100.0;
        const double full_hold = f->warp ? 0.0 : span * f->count / (f->count + 1);   // how far the real frame trails its arrival while generating (extrapolation: not at all)
        const double gmin = f->min_gain.load(std::memory_order_relaxed), gmax_in = f->max_in.load(std::memory_order_relaxed);
        if (gmin <= 0 && gmax_in <= 0 && gstate != G_ENGAGED) { gstate = G_RAMP; }   // governor switched off live: come back smoothly
        if (gstate == G_PAUSED && NowMs() >= resume_at) { gstate = G_RAMP; hold = 0; prev = 0; Log("[fg] governor: probing - generation back on"); }
        if (f->warp && gstate == G_RAMP) { gstate = G_ENGAGED; f->paused = false; gwin_t0 = NowMs(); gwin_seq0 = seq_now; gwin_presented = 0; gbad = 0; }   // no history to warm, no hold to grow
        if (gstate == G_DRAIN || gstate == G_PAUSED)
        {
            // No Evaluate: the point of pausing is to stop paying. The real frame goes out `hold` after
            // it lands, and hold drains to zero a tenth at a time so no single interval takes the step.
            ok = WaitRendered(f, s);
            if (gstate == G_DRAIN) { hold = std::min(hold, full_hold) - full_hold * 0.1; if (hold <= 0) { hold = 0; gstate = G_PAUSED; } }
            if (ok && wait_until(NowMs() + hold)) ok = Present(f, s->real, s, true, false);
            ++f->paused_frames; prev = 0; prev_real_target = 0;
        }
        else if (gstate == G_RAMP)
        {
            // Evaluate every frame so DLSS-G's history is warm, show none of it yet, and grow the hold
            // back to where generation needs it. The cadence is then handed to the engaged path whole:
            // its next anchor is prev_real_target + L, which is exactly one span after this frame.
            ok = Evaluate(f, s, interp);
            hold = std::min(full_hold, hold + full_hold * 0.1);
            const double target = NowMs() + hold;
            if (ok && wait_until(target)) { ok = EvaluateResolve(f, s, allow) && Present(f, s->real, s, true); }
            prev_real_target = target;
            if (hold >= full_hold) { gstate = G_ENGAGED; f->paused = false; gwin_t0 = NowMs(); gwin_seq0 = seq_now; gwin_presented = 0; gbad = 0; }
        }
        else
        {
        if (ok && !f->warp) ok = Evaluate(f, s, interp);
        if (ok)
        {
            const double L = span / (f->count + 1);
            // phase shifts every present target; the cadence (prev_real_target) stays unshifted so it never accumulates.
            const double ph = f->phase.load(std::memory_order_relaxed);
            if (f->warp)
            {
                // Extrapolation: list 2 already pushed the real frame ahead along its own flow into
                // s->gen, so nothing is held back. The real frame goes out on the next vblank, each
                // generated one a further L after it, and a newer real frame cuts the rest short.
                // No evaluate, no FG queue: the render fence covers the real and generated frames alike.
                f->dbg = s;
                const double t0 = NowMs();
                ok = WaitRendered(f, s);
                if (ok && wait_until(t0 + ph)) { ok = Present(f, s->real, s, true, false); ++gwin_presented; }
                if (!interp) ++f->no_pair;
                for (int i = 0; ok && interp && s->gen_ok && i < f->count; ++i)
                {
                    if (!wait_until(t0 + (i + 1) * L + ph)) break;
                    bool newer = false;
                    { std::lock_guard<std::mutex> lk(f->mu); newer = newer_ready(s->seq); }
                    if (newer) { ++f->preempts; break; }
                    if (!Present(f, s->gen[i], s, false, false)) { ok = false; break; }
                    ++f->gen_shown; ++gwin_presented;
                }
                prev_real_target = t0;
            }
            else
            {
                const double anchor = std::max(NowMs(), prev_real_target + L);
                double real_target = anchor;
                bool preempted = false;
                // Whatever happens next, the first thing to do is wait for `anchor`: generated frame 0
                // targets it, and with no generation the real frame does. So pace FIRST and collect the
                // evaluate afterwards - on schedule DLSS-G has had the whole L window to finish and the
                // fence is already signalled, which turns a ~1.5 ms serial stall into nothing.
                if (!wait_until(anchor + ph)) { ++f->preempts; preempted = true; }
                if (!preempted && !EvaluateResolve(f, s, allow)) ok = false;
                const bool gen = ok && !preempted && interp && allow;
                if (!interp) ++f->no_pair; else if (!allow) ++f->disabled;
                if (gen)
                {
                    for (int i = 0; i < f->count; ++i)
                    {
                        const double target = anchor + i * L + ph;
                        if (i && !wait_until(target)) { preempted = true; ++f->preempts; break; }   // i == 0: waited above
                        if (NowMs() - target > L * 0.5 + vb * 0.5) { ++f->drops; continue; }   // stale: skip, never burst
                        if (!Present(f, s->gen[i], s, false)) { ok = false; break; }
                        ++f->gen_shown; ++gwin_presented;
                    }
                    real_target = preempted ? NowMs() : anchor + f->count * L;
                }
                // The real frame is never skipped for lateness: only `stop` interrupts.
                if (ok && wait_until(real_target + ph)) { ok = Present(f, s->real, s, true); ++gwin_presented; }
                prev_real_target = real_target;
            }
            const double now = NowMs();
            if ((gmin > 0 || gmax_in > 0) && now - gwin_t0 >= 1000.0)
            {
                const UINT64 submitted = seq_now - gwin_seq0;
                if (submitted >= 20)   // too few frames to judge (a paused game, a loading screen)
                {
                    const double gain = (double)gwin_presented / (double)submitted, in_fps = submitted * 1000.0 / (now - gwin_t0);
                    const bool over = gmax_in > 0 && in_fps > gmax_in, low = gmin > 0 && gain < gmin;
                    if (!over && !low) { gbad = 0; backoff = 3000.0; }
                    else if (++gbad >= 2)
                    {
                        if (over) Log("[fg] governor: %.0f fps in > %.0f floor for 2 s - generation paused, next probe in %.0f s", in_fps, gmax_in, backoff / 1000.0);
                        else Log("[fg] governor: gain %.2fx < %.2fx for 2 s - generation paused, next probe in %.0f s", gain, gmin, backoff / 1000.0);
                        gstate = G_DRAIN; hold = full_hold; gbad = 0; f->paused = true;
                        resume_at = now + backoff; backoff = std::min(backoff * 2.0, 30000.0);
                    }
                }
                gwin_t0 = now; gwin_seq0 = seq_now; gwin_presented = 0;
            }
        }
        }
        if (gstate != G_DRAIN && gstate != G_PAUSED) prev = s->seq;
        { std::lock_guard<std::mutex> lk(f->mu); s->state = 0; }
        f->cv.notify_all();   // FgRecord may be waiting for a free slot
    }
}

// Passthrough (count == 0): the newest ready slot goes out as soon as its render fence completes,
// on the next vblank (vblank pacing) or right away (timer). Older ready slots are dropped, never
// shown stale, and the main thread is never throttled by a display slower than the capture.
static void Passthrough(Fg* f)
{
    bool vb = f->vblank;
    while (!f->stop && !f->failed)
    {
        FgSlot* s = nullptr;
        {
            std::unique_lock<std::mutex> lk(f->mu);
            f->cv.wait(lk, [&] { for (auto& x : f->slots) if (x.state == 2) return true; return f->stop.load(); });
            if (f->stop) break;
            s = TakeNewestLocked(f);
        }
        bool ok = WaitRendered(f, s);
        if (ok && vb && !OverlayWaitVBlank(f->ov)) vb = false;   // no output: immediate from here on
        if (ok) Present(f, s->real, s, true);
        { std::lock_guard<std::mutex> lk(f->mu); s->state = 0; }
        f->cv.notify_all();   // FgRecord may be waiting for a free slot
    }
}

// engine=latewarp: Frame Warp at every refresh. The newest real frame is re-projected to the camera
// the mouse has turned to since the game drew it (mouse.h: counts -> pixels, learned against our own
// flow), so the view answers the mouse at the display's rate whatever the game's frame rate - Reflex 2's
// idea from outside the game. With no reliable mouse model yet (or no mouse movement) a frame is shown
// once, as passthrough would. The rotation is right for a camera turning in place; see latewarp.h for
// orbit cameras.
static void Reproject(Fg* f)
{
    Gpu& g = *f->g; bool vb = f->vblank; FgSlot* cur = nullptr; bool fresh = false, registered = false; int k = 0;
    const float focal = (float)f->h * 0.5f / tanf(f->lw_vfov * 0.5f);   // output px per radian at the centre
    const double period = OverlayVBlankMs(f->ov);
    while (!f->stop && !f->failed)
    {
        if (vb) { if (!OverlayWaitVBlank(f->ov)) { Log("[fg] no usable vblank wait under the overlay - latewarp paced on the CPU timer"); vb = false; } }
        else Sleep((DWORD)std::max(1.0, period));
        if (f->hold) continue;
        {
            std::lock_guard<std::mutex> lk(f->mu);
            if (FgSlot* s = TakeNewestLocked(f)) { if (cur) cur->state = 0; cur = s; fresh = true; registered = false; }
        }
        f->cv.notify_all();   // FgAcquire may be waiting for a free slot
        if (!cur) continue;
        if (fresh && !WaitRendered(f, cur)) break;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float sx = 0, sy = 0;
        bool warp = MousePredict(cur->cap_qpc ? cur->cap_qpc : cur->acq_qpc, now.QuadPart, sx, sy) && fabsf(sx) + fabsf(sy) >= 0.25f;
        char t[64]; if (GetEnvironmentVariableA("JF_LW_SHIFT", t, sizeof t)) warp = sscanf_s(t, "%f,%f", &sx, &sy) == 2;   // TEST: a fixed shift, no mouse (bench)
        if (!warp)
        {
            if (fresh && !PresentAfter(f, cur->real, cur, true, g.fence, cur->fence)) break;
            fresh = false; continue;
        }
        if (!GpuCtxBegin(g, f->ctx)) { Fail(f, "FG queue begin (device removed?)"); break; }
        ID3D12GraphicsCommandList* cl = f->ctx.list;
        ID3D12Resource* dst = f->lw_final[k];
        OverlayGuard(f->ov, f->ctx.queue);   // the last present copy has finished reading its source
        GpuBarrier(cl, cur->real, CSRC, NPSR); GpuBarrier(cl, dst, CSRC, UAV);
        // +yaw moves content left, +pitch moves it up (measured): turn the camera against the shift
        const bool ok = LatewarpEvaluate(f->lwf, cl, cur->real, dst, !registered, atanf(-sx / focal), atanf(-sy / focal), 0.0f, f->lw_vfov, cur->mask_ok && !GetEnvironmentVariableA("JF_LW_NOMASK", t, sizeof t) ? cur->mask : nullptr);
        registered = true;
        GpuBarrier(cl, dst, UAV, CSRC); GpuBarrier(cl, cur->real, NPSR, CSRC);
        CopyRects(f, cl, cur, cur->real, dst);
        f->ctx.queue->Wait(g.fence, cur->fence);
        const UINT64 v = GpuCtxEnd(f->ctx);
        if (!ok || !v) { Fail(f, ok ? "latewarp submit" : "latewarp evaluate"); break; }
        if (!PresentAfter(f, dst, cur, fresh, f->ctx.fence, v)) break;
        if (!fresh) ++f->gen_shown;   // a refresh the game did not draw
        f->lw_last = dst; f->dbg = cur;
        k ^= 1; fresh = false;
    }
    { std::lock_guard<std::mutex> lk(f->mu); if (cur) cur->state = 0; }
    f->cv.notify_all();
}

// engine=video: frame interpolation for 2D video, timed by the video's own clock. The in-between frames are
// DLSS-G's (3 per real frame, at 1/4 steps, the nearest shown) when it is there and allows the pair,
// else our own flow interpolation (CsVideoInterp) at the exact instant. Every refresh shows the
// content instant due now minus one source period (plus the pipeline's lag): the two newest real frames
// A and B bracket it, so the refresh gets A and B blended along B's flow at t = (instant - A) / (B - A).
// 24 fps on 240 Hz becomes ten evenly spaced steps per frame and 23.976 drifts smoothly instead of
// stuttering - DLSS-G's fixed 2-4x cannot land on either. The timeline is the capture timestamps
// (the browser's presents, quantised to the desktop's refresh), smoothed: each frame lands one period
// after the last, pulled a tenth of the way to where it was observed, and re-synced on a jump.
// Duplicates never get here (main drops them), so the period is the video's. Costs one source frame of
// latency - fine for video, wrong for a game (it would also lag the audio by that much).
static void VideoPresent(Fg* f)
{
    Gpu& g = *f->g; bool vb = f->vblank; const double vbms = OverlayVBlankMs(f->ov);
    // The real frames in hand, oldest first, each with its place on the smoothed content timeline (ms):
    // [A, B] is the pair being shown, a third is the next one, waiting until the shown instant passes B.
    // Moving on the moment it arrived jumped t from ~0.6 back to 0 - a skip in every frame.
    struct Held { FgSlot* s; double t; }; Held held[3]; int nheld = 0;
    double ob = 0, period = 0, lag = 0;   // the newest frame's observed time; source period; arrival lag behind content time
    const FgSlot* shown = nullptr; float shown_t = -1; int k = 0; ID3D12Resource* shown_tex = nullptr;   // shown/shown_t: the flow path's last frame
    const bool flow_only = GetEnvironmentVariableA("JF_VID_FLOW", nullptr, 0) != 0;   // TEST: in-between frames from our flow only (A/B)
    char ev[16]; const float forced = GetEnvironmentVariableA("JF_VID_T", ev, sizeof ev) ? (float)atof(ev) : -1.0f;   // TEST: a fixed t (bench)
    auto release = [&] { { std::lock_guard<std::mutex> lk(f->mu); held[0].s->state = 0; } held[0] = held[1]; held[1] = held[2]; --nheld; f->cv.notify_all(); };
    while (!f->stop && !f->failed)
    {
        // On DwmFlush the wait only wakes when something on the desktop is drawn - a frame we decided not
        // to present draws nothing, so the loop slept until the video's next frame and showed real frames
        // only (live: fg_gen 0, output = input). The engine needs every refresh: then the CPU timer.
        if (vb && !OverlayVBlankIsDwm(f->ov)) { if (!OverlayWaitVBlank(f->ov)) { Log("[fg] no usable vblank wait under the overlay - video paced on the CPU timer"); vb = false; } }
        else Sleep((DWORD)std::max(1.0, vbms));
        if (f->hold) continue;
        FgSlot* n = nullptr;
        { std::lock_guard<std::mutex> lk(f->mu); n = TakeNewestLocked(f); }
        f->cv.notify_all();   // FgAcquire may be waiting for a free slot
        const double now = NowMs();
        if (n)
        {
            if (!WaitRendered(f, n)) break;
            const double o = n->cap_qpc ? QpcToMs(n->cap_qpc) : now, gap = o - ob;
            double tn = o;
            if (nheld && n->interpolate && gap > 2.0 && gap < 250.0)
            {
                period = period > 0 ? period + 0.1 * (gap - period) : gap;
                const double due = held[nheld - 1].t + period;
                tn = fabs(o - due) < 0.5 * period ? due + 0.1 * (o - due) : o;   // locked; re-synced on a jump
            }
            ob = o;
            if (nheld == 3) release();   // a burst: the oldest goes
            held[nheld++] = { n, tn };
            // DLSS-G makes the in-between frames now, against the frame it evaluated last (the one before);
            // they are collected when this pair comes on screen - a period later, long finished.
            n->gen_ok = false; n->resolved = true;
            if (f->gens && !flow_only) { if (!Evaluate(f, n, n->interpolate && nheld >= 2)) break; n->resolved = false; }
            lag = std::max(now - tn, lag - 0.05);   // the worst recent arrival lag, decaying slowly
        }
        if (!nheld) continue;
        // the shown instant: one period plus the arrival lag plus a refresh behind now, so the frame after
        // B is normally in hand by the time it is needed
        const double c = now - (period + lag + vbms);
        while (nheld == 3 && c >= held[1].t) release();
        FgSlot* a = nheld >= 2 ? held[0].s : nullptr;
        FgSlot* b = nheld >= 2 ? held[1].s : held[0].s;
        // where the instant falls between A and B; across a cut (B not interpolable) A, then B
        float t = 1.0f;
        if (a && period > 0)
        {
            const double ta = held[0].t, tb = held[1].t;
            t = b->interpolate ? (float)std::clamp((c - ta) / std::max(tb - ta, 1.0), 0.0, 1.0) : (c < tb ? 0.0f : 1.0f);
        }
        if (forced >= 0 && a) t = forced;
        if (a && !b->resolved) { bool allow = false; if (!EvaluateResolve(f, b, allow)) break; b->resolved = true; b->gen_ok = allow && b->interpolate; }
        if (a && b->gen_ok)
        {
            // DLSS-G's frames sit at i/(gens+1): show the nearest - at 24 fps on 240 Hz that is an even
            // 96 fps timeline, each step held for 2 or 3 refreshes.
            const int steps = f->gens + 1, i = (int)std::lround(t * steps);
            ID3D12Resource* tex = i <= 0 ? a->real : i >= steps ? b->real : b->gen[i - 1];
            if (tex == shown_tex) continue;
            const bool gen = i > 0 && i < steps;
            if (!(gen ? PresentAfter(f, tex, b, false, f->ctx.fence, b->eval_fence) : PresentAfter(f, tex, i <= 0 ? a : b, i >= steps, g.fence, (i <= 0 ? a : b)->fence))) break;
            if (gen) ++f->gen_shown;
            shown_tex = tex; shown = nullptr; f->lw_last = tex; f->dbg = b;
            continue;
        }
        const FgSlot* src = t <= 0.02f ? a : t >= 0.98f ? b : nullptr;   // an end: that real frame as it is
        if (src)
        {
            if (src->real == shown_tex) continue;   // already on screen
            if (!PresentAfter(f, src->real, src, src == b, g.fence, src->fence)) break;
            shown = src; shown_t = -1; shown_tex = src->real; f->dbg = b;
            continue;
        }
        if (shown == b && shown_tex == f->lw_final[k ^ 1] && fabsf(t - shown_t) < 0.005f) continue;
        if (!GpuCtxBegin(g, f->ctx)) { Fail(f, "FG queue begin (device removed?)"); break; }
        ID3D12GraphicsCommandList* cl = f->ctx.list;
        ID3D12Resource* dst = f->lw_final[k];
        OverlayGuard(f->ov, f->ctx.queue);   // the last present copy has finished reading its source
        GpuBarrier(cl, a->real, CSRC, NPSR); GpuBarrier(cl, b->real, CSRC, NPSR); GpuBarrier(cl, dst, CSRC, UAV);
        CsVideoInterp(g, f->sh, cl, a->real, b->real, b->flow, f->mw, f->mh, dst, f->w, f->h, t);
        GpuBarrier(cl, dst, UAV, CSRC); GpuBarrier(cl, a->real, NPSR, CSRC); GpuBarrier(cl, b->real, NPSR, CSRC);
        CopyRects(f, cl, b, b->real, dst);
        const UINT64 v = GpuCtxEnd(f->ctx);   // A and B were CPU-waited on arrival (WaitRendered)
        if (!v) { Fail(f, "video submit"); break; }
        if (!PresentAfter(f, dst, b, false, f->ctx.fence, v)) break;
        ++f->gen_shown; f->lw_last = dst; f->dbg = b; shown = b; shown_t = t; shown_tex = dst; k ^= 1;
    }
    { std::lock_guard<std::mutex> lk(f->mu); for (int i = 0; i < nheld; ++i) held[i].s->state = 0; }
    f->cv.notify_all();
}

// ---- lifecycle ---------------------------------------------------------------------------------------
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

static bool CreateFeature(Fg* f, const char* how)
{
    Gpu& g = *f->g;
    NVSDK_NGX_Parameter* p = f->params;
    p->Reset();
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u); p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, f->w); p->Set(NVSDK_NGX_Parameter_Height, f->h);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)DXGI_FORMAT_R8G8B8A8_UNORM);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, f->mw); p->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, f->mh);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    if (!GpuBegin(g)) return false;
    DWORD code = 0; NVSDK_NGX_Result r;
    { std::lock_guard<std::mutex> lk(NgxMutex()); r = SafeCreate(f, g.list, &code); }
    const UINT64 v = GpuEnd(g);
    const bool ok = v && GpuWait(g, g.fence, v, 30000) && !code && !NVSDK_NGX_FAILED(r) && f->feature;
    if (!ok) { Log("[fg] CreateFeature(11) via %s -> 0x%08X (%s)%s", how, r, NgxResultName(r), code ? " raised" : ""); f->feature = nullptr; }
    return ok;
}

// The presenter is latency-critical and tiny: it wakes, presents, and sleeps again, using about 6%
// of one core. What hurts it is not starvation but being scheduled LATE - and WoW pegs a couple of
// cores, so landing on one of those costs it a slice of a 4.17 ms vblank budget. Priority, not
// affinity: we must never inspect the game's threads to find its hot cores (that is exactly the
// process poking we refuse to do), and a hardcoded core is a guess that makes things worse when it
// collides. ABOVE_NORMAL just wins the race for the ~50 us it needs, then yields.
// Fail-safe: a failure here is ignored - the thread runs at normal priority, as it always did.
static void RaisePresenterPriority(std::thread& t)
{
    if (!SetThreadPriority(t.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL))
        Log("[fg] presenter priority unchanged (err %lu) - normal priority is fine, just jitterier", GetLastError());
}

// DLSS-G: the NGX feature (core, else nvngx_dlssg.dll directly) and its guides. nullptr on success, else why not.
static const char* SetupDlssg(Fg* f, const wchar_t* dir)
{
    Gpu& g = *f->g; const UINT mv_w = f->mw, mv_h = f->mh;
    if (!(f->params = NgxCoreParams(g, dir, "[fg]"))) return "no NGX parameter block";
    NVSDK_NGX_Result r;

    f->create = NVSDK_NGX_D3D12_CreateFeature; f->release = NVSDK_NGX_D3D12_ReleaseFeature; g_fg_evaluate = NVSDK_NGX_D3D12_EvaluateFeature_C;
    if (!CreateFeature(f, "NGX core"))
    {
        // Fallback: drive nvngx_dlssg.dll directly (NeuralScreen path) with the same parameter block.
        const std::wstring path = f->dir + L"\\nvngx_dlssg.dll";
        f->dll = LoadLibraryW(path.c_str());
        if (!f->dll) { Log("[fg] %ls did not load (err %lu)", path.c_str(), GetLastError()); return "CreateFeature failed and no direct runtime"; }
        auto init = (PFN_FgInitExt)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_Init_Ext");
        f->create = (PFN_FgCreate)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_CreateFeature");
        f->release = (PFN_FgRelease)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_ReleaseFeature");
        g_fg_evaluate = (PFN_FgEvaluate)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_EvaluateFeature");
        if (!init || !f->create || !f->release || !g_fg_evaluate) return "nvngx_dlssg.dll exports missing";
        r = init(0x1000000ULL, dir, g.dev, NVSDK_NGX_Version_API, f->params);
        Log("[fg] direct Init_Ext -> 0x%08X (%s)", r, NgxResultName(r));
        if (NVSDK_NGX_FAILED(r) || !CreateFeature(f, "nvngx_dlssg.dll")) return "CreateFeature failed (core and direct)";
    }

    // guides + outputs
    f->depth = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"fg_depth");
    std::vector<float> flat((size_t)mv_w * mv_h, 0.5f);
    if (!f->depth || !GpuUploadTex(g, f->depth, flat.data(), mv_w, mv_h, 4, NPSR)) return "depth texture";
    f->mv = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"fg_mv");   // zeros, for good: see Evaluate
    const std::vector<uint32_t> zero((size_t)mv_w * mv_h, 0);
    if (!f->mv || !GpuUploadTex(g, f->mv, zero.data(), mv_w, mv_h, 4, NPSR)) return "mv texture";
    f->disable = GpuMakeBuffer(g, 16, D3D12_HEAP_TYPE_DEFAULT, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"fg_disable");
    f->disable_rb = GpuMakeBuffer(g, 16, D3D12_HEAP_TYPE_READBACK, CDST, D3D12_RESOURCE_FLAG_NONE, L"fg_disable_rb");
    if (!f->disable || !f->disable_rb) return "disable buffers";
    return nullptr;
}

Fg* FgCreate(Gpu& g, Shaders* sh, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier, bool vblank_pacing, int engine, float lw_vfov)
{
    Fg* f = new Fg;
    f->g = &g; f->sh = sh; f->ov = ov; f->dir = dir; f->w = out_w; f->h = out_h; f->mw = mv_w; f->mh = mv_h;
    f->count = std::clamp(multiplier, 1, 4) - 1;
    f->vblank = vblank_pacing; f->warp = engine == FG_WARP; f->lw = engine == FG_LATEWARP; f->video = engine == FG_VIDEO; f->lw_vfov = lw_vfov;
    auto fail = [&](const char* why) { Fail(f, why); FgDestroy(f); return (Fg*)nullptr; };

    // ponytail: passthrough keeps the ctx too (its event is what the presenter waits on) - one code path.
    if (!GpuCtxInit(g, f->ctx, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, L"fg")) return fail("FG queue");
    if (!f->count && !f->lw)
    {
        for (auto& s : f->slots)
            if (!(s.real = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"fg_real"))) return fail("slot textures");
        f->thread = std::thread(Passthrough, f);
        RaisePresenterPriority(f->thread);
        Log("[fg] passthrough presenter %ux%u pacing=%s", out_w, out_h, vblank_pacing ? "vblank" : "timer");
        return f;
    }

    // DLSS-G: required by engine=dlssg; engine=video uses it for its in-between frames when it can, and
    // interpolates along our own flow when it cannot.
    if (!f->warp && !f->lw)
        if (const char* why = SetupDlssg(f, dir))
        {
            if (!f->video) return fail(why);
            Log("[fg] video: no DLSS-G (%s) - in-between frames from our flow", why); f->feature = nullptr;
        }
    f->gens = f->video ? (f->feature ? kMaxGen : 0) : f->count;
    for (auto& s : f->slots)
    {
        s.real = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"fg_real");
        if (!s.real) return fail("slot textures");
        if (f->video && !(s.flow = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"fg_flow"))) return fail("video flow textures");
        for (int i = 0; i < (f->lw ? 0 : f->gens); ++i)
            if (!(s.gen[i] = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"fg_gen"))) return fail("slot textures");
    }
    if (f->video)
    {
        for (auto& t : f->lw_final) t = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"video_out");
        if (!f->lw_final[0] || !f->lw_final[1]) return fail("video textures");
    }
    if (f->lw)
    {
        if (!(f->lwf = LatewarpCreate(g, dir, out_w, out_h, mv_w, mv_h))) return fail("Frame Warp unavailable (nvngx_latewarp.dll next to the exe?)");
        for (auto& t : f->lw_final) t = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"lw_final");
        for (auto& s : f->slots)
            if (!(s.mask = GpuMakeTex(g, (mv_w + 3) / 4, (mv_h + 3) / 4, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"lw_mask"))) return fail("latewarp mask");
        if (!f->lw_final[0] || !f->lw_final[1]) return fail("latewarp textures");
    }
    f->thread = std::thread(f->lw ? Reproject : f->video ? VideoPresent : Presenter, f);
    RaisePresenterPriority(f->thread);
    Log("[fg] %s %ux%u multiplier %d pacing=%s", f->lw ? "latewarp engine (Frame Warp to the mouse)" : f->video ? "video engine (interpolation on the content clock)" : f->warp ? "warp engine (extrapolation)" : "feature created", out_w, out_h, f->count + 1, vblank_pacing ? "vblank" : "timer");
    return f;
}

void FgDestroy(Fg* f)
{
    if (!f) return;
    f->stop = true; f->cv.notify_all();
    if (f->thread.joinable()) f->thread.join();
    // Drain: evaluates on the FG queue, producer copies on Gpu::queue and the last backbuffer copy
    // on the present queue all reference the slots.
    if (f->ctx.queue) GpuCtxWaitIdle(f->ctx, 5000);
    if (f->ov) OverlayDrain(f->ov);
    if (f->feature) { std::lock_guard<std::mutex> lk(NgxMutex()); f->release(f->feature); f->feature = nullptr; }
    // ponytail: no Shutdown1 - NR's parameter block lives in the same core; the refcount leaks until exit.
    if (f->params) NVSDK_NGX_D3D12_DestroyParameters(f->params);
    for (auto& s : f->slots) { REL(s.real); REL(s.mask); REL(s.flow); for (auto& t : s.gen) REL(t); }
    REL(f->mv); REL(f->depth); REL(f->disable); REL(f->disable_rb);
    LatewarpDestroy(f->lwf); for (auto& t : f->lw_final) REL(t);
    GpuCtxShutdown(*f->g, f->ctx);
    if (f->dll) FreeLibrary(f->dll);
    delete f;
}

bool FgFailed(const Fg* f) { return f->failed; }
int  FgMultiplier(const Fg* f) { return f->count + 1; }
double FgEvalMs(Fg* f, double* p95)
{
    std::vector<double> v;
    { std::lock_guard<std::mutex> lk(f->mu); v.assign(f->eval_ring, f->eval_ring + std::min(f->eval_n, 256u)); }
    if (p95) *p95 = -1;
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    if (p95) *p95 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.95))];
    return v[v.size() / 2];
}
void FgSetTiming(Fg* f, double phase_ms, double min_gain, double max_in_fps)
{
    f->phase.store(std::clamp(phase_ms, -50.0, 50.0), std::memory_order_relaxed);
    f->min_gain.store(std::clamp(min_gain, 0.0, 8.0), std::memory_order_relaxed);
    f->max_in.store(std::clamp(max_in_fps, 0.0, 1000.0), std::memory_order_relaxed);
}
void FgStats(Fg* f, FgStatsOut& out)
{
    out.presented = f->presented.exchange(0); out.drops = f->drops.exchange(0);
    out.no_pair = f->no_pair.exchange(0); out.disabled = f->disabled.exchange(0);
    out.preempts = f->preempts.exchange(0); out.gen_shown = f->gen_shown.exchange(0);
    out.paused = f->paused_frames.exchange(0);
    out.vblank_waits = (UINT)f->vbw_n.exchange(0); out.vblank_wait_sum_ms = (double)f->vbw_us.exchange(0) / 1000.0;
    out.record_waits = (UINT)f->rec_n.exchange(0); out.record_wait_sum_ms = (double)f->rec_us.exchange(0) / 1000.0;
    std::lock_guard<std::mutex> lk(f->mu);
    out.spacing_ms.swap(f->spacing); out.age_ms.swap(f->age); out.pipe_ms.swap(f->pipe);
    f->spacing.clear(); f->age.clear(); f->pipe.clear();
}

ID3D12Resource* FgDebugGen(Fg* f, int i)
{
    if (f && (f->lw || f->video)) return i == 0 ? f->lw_last : nullptr;   // latewarp / video: the last generated refresh
    return (f && f->dbg && i >= 0 && i < f->count) ? f->dbg->gen[i] : nullptr;
}

bool FgPaused(Fg* f) { return f && f->paused.load(std::memory_order_relaxed); }

ID3D12Resource* FgDebugReal(Fg* f) { return (f && f->dbg) ? f->dbg->real : nullptr; }

ID3D12Resource* FgMaskTarget(Fg* f)
{
    if (!f->lw || !f->pending) return nullptr;
    f->pending->mask_ok = true;   // the caller writes it in this list
    return f->pending->mask;
}

void FgDebugHold(Fg* f) { if (f && (f->lw || f->video)) { f->hold = true; Sleep(100); } }   // one in-flight refresh finishes
ID3D12Resource* FgDebugMask(Fg* f) { return (f && f->dbg) ? f->dbg->mask : nullptr; }

ID3D12Resource* FgFlowTarget(Fg* f) { return (f->video && f->pending) ? f->pending->flow : nullptr; }

ID3D12Resource* FgWarpTarget(Fg* f, int i)
{
    if (!f->warp || !f->pending || i < 0 || i >= f->count) return nullptr;
    f->pending->gen_ok = true;   // the caller writes it in this list
    return f->pending->gen[i];
}

ID3D12Resource* FgAcquire(Fg* f)
{
    if (f->failed) return nullptr;
    FgSlot* s = nullptr; UINT64 eval = 0;
    {
        std::unique_lock<std::mutex> lk(f->mu);
        auto free_slot = [&] { for (auto& x : f->slots) if (x.state == 0) return &x; return (FgSlot*)nullptr; };
        if (!(s = free_slot()))
        {
            // No free slot, so take the OLDEST READY one immediately. It is not worth waiting for:
            // the presenter picks the NEWEST ready slot and frees every other one on the spot, so an
            // older ready slot is already destined for the bin. The old code waited up to 50 ms to
            // avoid "dropping a real frame" - but the same frame died either way, just later and
            // with the capture thread stalled behind it. That wait was the largest remaining stall
            // in the pipeline (recwait_ms reached 45 ms), and it bought nothing.
            for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq < s->seq)) s = &x;
            if (s) ++f->drops;
        }
        if (!s)
        {
            // Nothing free and nothing ready: every slot is mid-record or mid-present. One presenter
            // and one recorder cannot hold every slot (engine=video holds three of four), so this is unreachable today - it stays as a
            // bounded wait rather than a silent dropped frame, and rec_us still reports it if the
            // slot count or the thread model ever changes.
            const double t_block = NowMs();
            const double bound = std::clamp(2.0 * (f->history ? NowMs() - f->last_submit : 16.0), 8.0, 50.0);
            f->cv.wait_for(lk, std::chrono::duration<double, std::milli>(bound), [&] { return f->stop || f->failed || (s = free_slot()) != nullptr; });
            f->rec_us += (UINT64)((NowMs() - t_block) * 1000.0); ++f->rec_n;
            if (!s) return nullptr;
        }
        s->state = 1; eval = s->eval_fence; s->gen_ok = false; s->mask_ok = false;
    }
    f->pending = s;
    // GPU-side ordering for the reuse: the FG queue's last evaluate of this slot (reads real/mv,
    // writes gen) and the present queue's last copy (reads real/gen) complete before list 2 writes
    // into s->real. The queue Wait lands before this list is executed by GpuEnd.
    if (eval) f->g->queue->Wait(f->ctx.fence, eval);
    OverlayGuard(f->ov, f->g->queue);
    return s->real;
}

bool FgRecord(Fg* f, const UiRect* rects, int nrects)
{
    FgSlot* s = f->pending;
    if (f->failed || !s) return false;
    s->nrects = std::clamp(nrects, 0, 64);
    if (s->nrects) memcpy(s->rects, rects, (size_t)s->nrects * sizeof(UiRect));
    return true;
}

void FgSubmit(Fg* f, UINT64 render_fence_value, bool reset, LONGLONG cap_qpc, LONGLONG acq_qpc)
{
    FgSlot* s = f->pending;
    if (!s) return;
    f->pending = nullptr;
    const double now = NowMs();
    const double interval = f->history ? now - f->last_submit : 16.0;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        if (!render_fence_value) { s->state = 0; return; }
        s->seq = ++f->seq; s->fence = render_fence_value; s->cap_qpc = cap_qpc; s->acq_qpc = acq_qpc;
        // The game's own frame interval. Between consecutive SUBMITS, not between the frames the
        // presenter happens to evaluate: measuring the evaluated gap made dropping self-amplifying
        // (drop one, the gap doubles, L doubles, the presenter halves, it drops more) and it settled
        // at a stable half rate - out == cap with drops == gen.
        s->content_ms = (cap_qpc && f->last_cap) ? QpcToMs(cap_qpc - f->last_cap) : 0.0;
        if (cap_qpc) f->last_cap = cap_qpc;
        s->interpolate = f->history && !reset && interval < 100.0;
        s->interval = std::clamp(interval, 4.0, 50.0);
        s->state = 2;
    }
    f->history = true; f->last_submit = now;
    f->cv.notify_one();
}
