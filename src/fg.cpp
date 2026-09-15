#include "fg.h"
#include "log.h"
#include "ngx_nr.h"          // NgxMutex
#include "nvsdk_ngx.h"
#include <algorithm>
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
static const int kSlots = 3, kMaxGen = 3;

// Rest states: real CSRC, mv CDST, gen[] CSRC, depth NPSR, disable UAV. state: 0 free, 1 writing
// (producer), 2 ready, 3 presenting; guarded by Fg::mu.
// Queues: the main queue writes real/mv (FgRecord copies, fence = Gpu::fence value); the FG queue
// (Fg::ctx) reads them + writes gen[] (eval_fence = ctx fence value); the present queue reads
// real/gen[] after waiting on eval_fence. Reuse: the main queue waits on eval_fence and on the last
// present copy before the next FgRecord copies into the slot.
struct FgSlot
{
    ID3D12Resource *real = nullptr, *mv = nullptr, *gen[kMaxGen] = {};
    int    state = 0;
    UINT64 seq = 0, fence = 0, eval_fence = 0;
    bool   interpolate = false;
    double interval = 16.0;   // ms between this and the previous submit (our clock)
    double content_ms = 0;    // ms between this frame and the previous CAPTURED one, in the game's
                              // own clock: 0 = the capture gave no timestamp
    LONGLONG cap_qpc = 0, acq_qpc = 0;
};

struct Fg
{
    Gpu*     g = nullptr;
    Overlay* ov = nullptr;
    UINT     w = 0, h = 0, mw = 0, mh = 0;
    int      count = 1;                      // generated frames per real frame (0 = passthrough)
    bool     vblank = true, mv_dilated = true;
    std::atomic<double> phase{ 0.0 };        // FgSetTiming (main) -> Presenter
    std::wstring dir;
    const wchar_t* path_list[1] = {};
    NVSDK_NGX_FeatureCommonInfo common = {};
    HMODULE  dll = nullptr;
    PFN_FgCreate create = nullptr; PFN_FgRelease release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    ID3D12Resource *depth = nullptr, *disable = nullptr, *disable_rb = nullptr;
    FgSlot   slots[kSlots];
    FgSlot*  pending = nullptr;              // reserved by FgRecord, handed over by FgSubmit
    UINT64   seq = 0; double last_submit = 0; bool history = false;
    LONGLONG last_cap = 0;                   // cap_qpc of the previous submit (FgSubmit only)
    // presenter thread
    std::thread thread;
    std::mutex mu; std::condition_variable cv;
    std::atomic<bool> stop{ false }, failed{ false };
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
static bool Present(Fg* f, ID3D12Resource* src, const FgSlot* s, bool real)
{
    const bool ok = f->count ? OverlayPresent(f->ov, src, f->ctx.fence, s->eval_fence) : OverlayPresent(f->ov, src, f->g->fence, s->fence);
    if (!ok) return Fail(f, "Present failed");
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

static bool Evaluate(Fg* f, FgSlot* s, bool interpolate, bool& allow)
{
    Gpu& g = *f->g;
    if (!GpuCtxBegin(g, f->ctx)) return Fail(f, "FG queue begin (device removed?)");
    ID3D12GraphicsCommandList* cl = f->ctx.list;
    GpuBarrier(cl, s->real, CSRC, NPSR);
    GpuBarrier(cl, s->mv, CDST, NPSR);
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    for (int i = 0; i < 4; ++i)
        opt.cameraViewToClip[i][i] = opt.clipToCameraView[i][i] = opt.clipToLensClip[i][i] = opt.clipToPrevClip[i][i] = opt.prevClipToClip[i][i] = 1.0f;
    // mv is in work-res pixels (same convention as NeuralScreen's field): 1/size normalises it.
    opt.mvecScale[0] = 1.0f / (float)f->mw; opt.mvecScale[1] = 1.0f / (float)f->mh;
    opt.cameraUp[1] = opt.cameraRight[0] = opt.cameraFwd[2] = 1.0f;
    opt.cameraNear = 0.1f; opt.cameraFar = 1000.0f; opt.cameraFOV = 1.0f;
    opt.cameraAspectRatio = (float)f->w / (float)f->h;
    // Decisions (nvsdk_ngx_params_dlssg.h):
    //  cameraMotionIncluded = true: OFA measures total screen motion (camera + objects), there is no
    //    separate camera term to add.
    //  orthoProjection = true + identity matrices + flat depth: no camera model exists for a capture.
    //  colorBuffersHDR = false: RGBA8 UNORM (sRGB-encoded SDR) backbuffer.
    //  motionVectorsInvalidValue: "which value represents an invalid (un-initialized) value" - the
    //    zero-init default (0) would flag every static pixel and every zero_below-zeroed vector as
    //    invalid. -65504 (min half) never comes out of the expand shader (R16G16_FLOAT, |v| < 4K).
    //  motionVectorsDilated: "already dilated or not". Ours are a dense per-pixel field (bilinear
    //    expand of a 1-px OFA grid), not depth-dilated - the honest value is false, but DLSS-G's
    //    dilation pass keys on depth, which is flat here, so true (the NeuralScreen setting) just
    //    skips a no-op pass. Default 1; [fg] mv_dilated=0 A/Bs it live (F11 rebuilds the Fg).
    opt.cameraMotionIncluded = opt.orthoProjection = true;
    opt.motionVectorsDilated = f->mv_dilated;
    opt.motionVectorsInvalidValue = -65504.0f;
    opt.colorBuffersHDR = false;
    opt.reset = !interpolate;
    opt.mvecsSubrectSize = opt.depthSubrectSize = { f->mw, f->mh };
    opt.backbufferSubrectSize = opt.outputInterpSubrectSize = { f->w, f->h };
    opt.multiFrameCount = (unsigned)f->count;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
    ep.pBackbuffer = s->real; ep.pMVecs = s->mv; ep.pDepth = f->depth; ep.pOutputDisableInterpolation = f->disable;
    f->params->Reset();
    f->params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)s->seq);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success; DWORD code = 0;
    for (int i = 0; i < f->count; ++i)
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
    GpuBarrier(cl, s->mv, NPSR, CDST);
    f->ctx.queue->Wait(g.fence, s->fence);   // GPU-side: the slot's real + mv copies (list 2) are complete on the main queue
    const int slot = f->ctx.slot;
    const UINT64 v = GpuCtxEnd(f->ctx);
    if (!v) return Fail(f, "FG queue submit");
    s->eval_fence = v;
    // CPU wait on the FG queue only (the disable flag decides whether the generated frames go out);
    // the present queue orders itself on eval_fence, never on this thread.
    if (!GpuCtxWait(f->ctx, f->ctx.fence, v, 10000)) return Fail(f, "evaluate fence wait (device removed?)");
    if (code) { Log("[fg] evaluate raised 0x%08X", code); return Fail(f, "evaluate raised an exception"); }
    if (NVSDK_NGX_FAILED(r)) { Log("[fg] evaluate -> 0x%08X (%s)", r, NgxResultName(r)); return Fail(f, "evaluate failed"); }
    // generation cost of this real frame = the sum of its evaluates (the fence just completed, so the slot is readable)
    double ms[kMaxGen] = {};
    if (GpuCtxStampsMsSlot(f->ctx, slot, ms, f->count))
    {
        double sum = 0; bool valid = true;
        for (int i = 0; i < f->count; ++i) { if (ms[i] < 0) valid = false; sum += ms[i]; }
        if (valid) { std::lock_guard<std::mutex> lk(f->mu); f->eval_ring[f->eval_n++ % 256] = sum; }
    }
    allow = true;
    uint8_t* d = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)f->count * 4 };
    if (SUCCEEDED(f->disable_rb->Map(0, &rr, (void**)&d)))
    {
        for (int i = 0; i < f->count; ++i) allow = allow && d[i * 4] == 0;
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
            LARGE_INTEGER vt0, vt1, vf; QueryPerformanceCounter(&vt0);
            const bool vok = OverlayWaitVBlank(f->ov);
            QueryPerformanceCounter(&vt1); QueryPerformanceFrequency(&vf);
            f->vbw_us += (UINT64)((vt1.QuadPart - vt0.QuadPart) * 1000000 / vf.QuadPart); ++f->vbw_n;
            // WaitForVBlank only fails structurally (no DXGI output under the overlay), so this one
            // IS permanent and the timer schedule is the right answer. It is logged now rather than
            // silent - a run that quietly lost vblank pacing looked like a pacing bug for hours.
            if (!vok) { Log("[fg] no DXGI output under the overlay - pacing on the CPU timer from here"); vb = 0; continue; }
            if (NowMs() + vb * 0.5 >= target) return true;
        }
    };
    while (!f->stop && !f->failed)
    {
        FgSlot* s = nullptr;
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
            for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq > s->seq)) s = &x;
            for (auto& x : f->slots) if (x.state == 2 && &x != s) { x.state = 0; ++f->drops; }
            s->state = 3;
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
        if (ok) ok = Evaluate(f, s, interp, allow);
        if (ok)
        {
            const double L = span / (f->count + 1);
            const bool gen = interp && allow;
            if (!interp) ++f->no_pair; else if (!allow) ++f->disabled;
            // phase shifts every present target; the cadence (prev_real_target) stays unshifted so it never accumulates.
            const double ph = f->phase.load(std::memory_order_relaxed);
            const double anchor = std::max(NowMs(), prev_real_target + L);
            double real_target = anchor;
            bool preempted = false;
            if (gen)
            {
                for (int i = 0; i < f->count; ++i)
                {
                    const double target = anchor + i * L + ph;
                    if (!wait_until(target)) { preempted = true; ++f->preempts; break; }
                    if (NowMs() - target > L * 0.5 + vb * 0.5) { ++f->drops; continue; }   // stale: skip, never burst
                    if (!Present(f, s->gen[i], s, false)) { ok = false; break; }
                    ++f->gen_shown;
                }
                real_target = preempted ? NowMs() : anchor + f->count * L;
            }
            // The real frame is never skipped for lateness: only `stop` interrupts (seq MAX = no pre-emption).
            if (ok && wait_until(real_target + ph)) ok = Present(f, s->real, s, true);
            prev_real_target = real_target;
        }
        prev = s->seq;
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
            for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq > s->seq)) s = &x;
            for (auto& x : f->slots) if (x.state == 2 && &x != s) { x.state = 0; ++f->drops; }
            s->state = 3;
        }
        bool ok = GpuCtxWait(f->ctx, f->g->fence, s->fence, 10000) || Fail(f, "render fence wait (device removed?)");
        if (ok && vb && !OverlayWaitVBlank(f->ov)) vb = false;   // no output: immediate from here on
        if (ok) Present(f, s->real, s, true);
        { std::lock_guard<std::mutex> lk(f->mu); s->state = 0; }
        f->cv.notify_all();   // FgRecord may be waiting for a free slot
    }
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

Fg* FgCreate(Gpu& g, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier, bool vblank_pacing, bool mv_dilated)
{
    Fg* f = new Fg;
    f->g = &g; f->ov = ov; f->dir = dir; f->w = out_w; f->h = out_h; f->mw = mv_w; f->mh = mv_h;
    f->count = std::clamp(multiplier, 1, 4) - 1;
    f->vblank = vblank_pacing; f->mv_dilated = mv_dilated;
    auto fail = [&](const char* why) { Fail(f, why); FgDestroy(f); return (Fg*)nullptr; };

    // ponytail: passthrough keeps the ctx too (its event is what the presenter waits on) - one code path.
    if (!GpuCtxInit(g, f->ctx, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, L"fg")) return fail("FG queue");
    if (!f->count)
    {
        for (auto& s : f->slots)
            if (!(s.real = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, CSRC, L"fg_real"))) return fail("slot textures");
        f->thread = std::thread(Passthrough, f);
        Log("[fg] passthrough presenter %ux%u pacing=%s", out_w, out_h, vblank_pacing ? "vblank" : "timer");
        return f;
    }

    // NGX core: Init is harmless if ngx_nr already did it; the parameter block comes from the core.
    f->path_list[0] = f->dir.c_str();
    f->common.PathListInfo.Path = f->path_list; f->common.PathListInfo.Length = 1;
    f->common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, dir, g.dev, &f->common, NVSDK_NGX_Version_API);
    Log("[fg] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    r = NVSDK_NGX_D3D12_AllocateParameters(&f->params);
    if (NVSDK_NGX_FAILED(r) || !f->params) { Log("[fg] AllocateParameters -> 0x%08X (%s)", r, NgxResultName(r)); return fail("no NGX parameter block"); }

    f->create = NVSDK_NGX_D3D12_CreateFeature; f->release = NVSDK_NGX_D3D12_ReleaseFeature; g_fg_evaluate = NVSDK_NGX_D3D12_EvaluateFeature_C;
    if (!CreateFeature(f, "NGX core"))
    {
        // Fallback: drive nvngx_dlssg.dll directly (NeuralScreen path) with the same parameter block.
        const std::wstring path = f->dir + L"\\nvngx_dlssg.dll";
        f->dll = LoadLibraryW(path.c_str());
        if (!f->dll) { Log("[fg] %ls did not load (err %lu)", path.c_str(), GetLastError()); return fail("CreateFeature failed and no direct runtime"); }
        auto init = (PFN_FgInitExt)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_Init_Ext");
        f->create = (PFN_FgCreate)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_CreateFeature");
        f->release = (PFN_FgRelease)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_ReleaseFeature");
        g_fg_evaluate = (PFN_FgEvaluate)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_EvaluateFeature");
        if (!init || !f->create || !f->release || !g_fg_evaluate) return fail("nvngx_dlssg.dll exports missing");
        r = init(0x1000000ULL, dir, g.dev, NVSDK_NGX_Version_API, f->params);
        Log("[fg] direct Init_Ext -> 0x%08X (%s)", r, NgxResultName(r));
        if (NVSDK_NGX_FAILED(r) || !CreateFeature(f, "nvngx_dlssg.dll")) return fail("CreateFeature failed (core and direct)");
    }

    // guides + outputs
    f->depth = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"fg_depth");
    std::vector<float> flat((size_t)mv_w * mv_h, 0.5f);
    if (!f->depth || !GpuUploadTex(g, f->depth, flat.data(), mv_w, mv_h, 4, NPSR)) return fail("depth texture");
    f->disable = GpuMakeBuffer(g, 16, D3D12_HEAP_TYPE_DEFAULT, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"fg_disable");
    f->disable_rb = GpuMakeBuffer(g, 16, D3D12_HEAP_TYPE_READBACK, CDST, D3D12_RESOURCE_FLAG_NONE, L"fg_disable_rb");
    if (!f->disable || !f->disable_rb) return fail("disable buffers");
    for (auto& s : f->slots)
    {
        s.real = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, CSRC, L"fg_real");
        s.mv = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, CDST, L"fg_mv");
        if (!s.real || !s.mv) return fail("slot textures");
        for (int i = 0; i < f->count; ++i)
            if (!(s.gen[i] = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"fg_gen"))) return fail("slot textures");
    }
    f->thread = std::thread(Presenter, f);
    Log("[fg] feature created %ux%u mv %ux%u multiplier %d pacing=%s mv_dilated=%d", out_w, out_h, mv_w, mv_h, f->count + 1, vblank_pacing ? "vblank" : "timer", mv_dilated ? 1 : 0);
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
    for (auto& s : f->slots) { REL(s.real); REL(s.mv); for (auto& t : s.gen) REL(t); }
    REL(f->depth); REL(f->disable); REL(f->disable_rb);
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
void FgSetTiming(Fg* f, double phase_ms)
{
    f->phase.store(std::clamp(phase_ms, -50.0, 50.0), std::memory_order_relaxed);
}
void FgStats(Fg* f, FgStatsOut& out)
{
    out.presented = f->presented.exchange(0); out.drops = f->drops.exchange(0);
    out.no_pair = f->no_pair.exchange(0); out.disabled = f->disabled.exchange(0);
    out.preempts = f->preempts.exchange(0); out.gen_shown = f->gen_shown.exchange(0);
    out.vblank_waits = (UINT)f->vbw_n.exchange(0); out.vblank_wait_sum_ms = (double)f->vbw_us.exchange(0) / 1000.0;
    out.record_waits = (UINT)f->rec_n.exchange(0); out.record_wait_sum_ms = (double)f->rec_us.exchange(0) / 1000.0;
    std::lock_guard<std::mutex> lk(f->mu);
    out.spacing_ms.swap(f->spacing); out.age_ms.swap(f->age); out.pipe_ms.swap(f->pipe);
    f->spacing.clear(); f->age.clear(); f->pipe.clear();
}

bool FgRecord(Fg* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* composed, ID3D12Resource* mv)
{
    if (f->failed) return false;
    FgSlot* s = nullptr; UINT64 eval = 0;
    {
        std::unique_lock<std::mutex> lk(f->mu);
        auto free_slot = [&] { for (auto& x : f->slots) if (x.state == 0) return &x; return (FgSlot*)nullptr; };
        if (!(s = free_slot()))
        {
            const double t_block = NowMs();
            // Every slot is in flight (one presenting, two ready): wait for the presenter to retire
            // the oldest (a newer ready slot pre-empts its generated frames, so ~1 vblank) instead
            // of dropping a real frame. ponytail: bound = 2 real intervals; on timeout the oldest
            // ready slot is overwritten (the presenter is stuck behind the display).
            const double bound = std::clamp(2.0 * (f->history ? NowMs() - f->last_submit : 16.0), 8.0, 50.0);
            f->cv.wait_for(lk, std::chrono::duration<double, std::milli>(bound), [&] { return f->stop || f->failed || (s = free_slot()) != nullptr; });
            // This block was invisible: it lands in main's undifferentiated cpu_ms, which is how a
            // 7 ms per-frame stall hid for a whole session.
            f->rec_us += (UINT64)((NowMs() - t_block) * 1000.0); ++f->rec_n;
            if (!s)
            {
                for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq < s->seq)) s = &x;
                if (!s) return false;
                ++f->drops;
            }
        }
        s->state = 1; eval = s->eval_fence;
    }
    f->pending = s;
    // GPU-side ordering for the reuse: the FG queue's last evaluate of this slot (reads real/mv,
    // writes gen) and the present queue's last copy (reads real/gen) complete before list 2 writes.
    if (eval) f->g->queue->Wait(f->ctx.fence, eval);
    OverlayGuard(f->ov, f->g->queue);
    GpuBarrier(cl, s->real, CSRC, CDST);
    cl->CopyResource(s->real, composed);
    GpuBarrier(cl, s->real, CDST, CSRC);
    if (!f->count) return true;   // passthrough: no mv
    GpuBarrier(cl, mv, NPSR, CSRC);
    cl->CopyResource(s->mv, mv);
    GpuBarrier(cl, mv, CSRC, NPSR);
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
