#include "xe_fg.h"
#include "xe_flow.h"
#include "xe_interp.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES CSRC = D3D12_RESOURCE_STATE_COPY_SOURCE;
static const D3D12_RESOURCE_STATES CDST = D3D12_RESOURCE_STATE_COPY_DEST;
static const D3D12_RESOURCE_STATES COMMON = D3D12_RESOURCE_STATE_COMMON;
static const D3D12_RESOURCE_STATES PRESENT = D3D12_RESOURCE_STATE_PRESENT;
static const int kFrames = 4, kPairs = 3;

// Rest states: frame NPSR, pair grids NPSR, backbuffers PRESENT. Guarded by XeFg::mu: every field below except
// the textures' contents, which the fences order.
struct XeFrame { ID3D12Resource* tex = nullptr; double t = 0; UINT64 read = 0; };   // read: ctx fence of the last interp that sampled it
struct XePair
{
    ID3D12Resource *fwd = nullptr, *bwd = nullptr, *stat = nullptr;   // stat: XeFlowStaticMap at L0
    int a = -1, b = -1;           // frame slots; a == b: a single real frame (no flow)
    bool valid = false, flow = false;
    double ta = 0, tb = 0, submit_ms = 0;
    UINT64 seq = 0, ready = 0;    // ready: Gpu::fence value that completes its frame + grids
    UINT64 read = 0;              // ctx fence of the last interp that read its grids
};

struct XeFg
{
    Gpu* g = nullptr; Overlay* ov = nullptr;
    UINT w = 0, h = 0;
    XeFlow* flow = nullptr; XeInterp interp; GpuCtx ctx;
    XeFrame fr[kFrames]; XePair pr[kPairs];
    // main thread only
    int last = -1; UINT64 pair_seq = 0; int slot = 0; int flow_stamp_slot = -1;
    bool have_pyr = false;   // the previous frame's pyramid is in slot ^ 1 (none are built while generation is off)
    std::vector<double> flow_ms;
    // shared
    std::mutex mu; std::condition_variable cv;
    std::thread th;
    std::atomic<bool> stop{ false }, failed{ false }, enabled{ true };
    std::atomic<double> margin{ 0.0 }, max_in{ 100.0 };
    std::atomic<bool>   extrap{ false };   // XeFgSetExtrapolate: show the newest frame pushed ahead instead of interpolating behind it
    std::atomic<bool>   vsync{ false };    // XeFgSetVsync: flip on vblank (no tearing) instead of immediately
    std::atomic<double> lead{ 1.5 };       // vsync: the least lead before the vblank the frame flips on (it adapts up on misses)
    // presenter-owned, published under mu for stats
    UINT in = 0, presented = 0, generated = 0, extrapolated = 0, held = 0, early = 0, missed = 0;
    UINT stall_hold = 0, stall_late = 0, stall_other = 0, jumps = 0;   // the smoothness check
    UINT repeats = 0, wait_timeouts = 0;   // refreshes with nothing new presented; vsync waits that timed out
    double lead_ms = 0;   // vsync: the current adaptive lead
    std::vector<double> spacing, interp_ms, after_vblank;   // after_vblank: refresh wake -> Present call, ms
    double hold_ms = 0, interval_ms = 0, extra_ms = 0; bool passthrough = false;
    // snapshot (XeFgRequestSnapshot / XeFgTakeSnapshot): the next GENERATED frame, copied on the ctx queue
    std::atomic<bool> snap_req{ false };
    ID3D12Resource* snap_rb = nullptr; D3D12_PLACED_SUBRESOURCE_FOOTPRINT snap_fp = {}; UINT64 snap_bytes = 0;
    bool snap_pending = false; UINT64 snap_fence = 0; float snap_t = 0;   // under mu
};

#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

static bool Fail(XeFg* f, const char* why)
{
    if (!f->failed.exchange(true)) Log("[xefg] stopped: %s", why);
    return false;
}

// ---- presenter ------------------------------------------------------------------------------------------
// One wait per display refresh: OverlayWaitVBlank (which itself falls back from WaitForVBlank to
// DwmFlush when the former returns without waiting - this laptop's panel), else a high-resolution
// waitable timer on the refresh grid.
struct DisplayClock
{
    bool timer_mode = false;
    double vb = 1000.0 / 60; double next = 0;
    HANDLE timer = nullptr;
    ~DisplayClock() { if (timer) CloseHandle(timer); }
    void Wait(Overlay* ov)
    {
        if (!timer_mode)
        {
            if (OverlayWaitVBlank(ov)) return;
            timer_mode = true; Log("[xefg] no usable vblank wait - pacing on a high-resolution timer");
        }
        const double t0 = NowMs();
        if (next < t0) next = t0 + vb;   // (re)anchor after a stall
        SleepUntil(next);
        next += vb;
    }
    // Sleep until `t` on the NowMs clock (high-resolution waitable timer; returns at once if past).
    void SleepUntil(double t)
    {
        const double now = NowMs();
        if (t <= now) return;
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((t - now) * 10000.0);   // relative, 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
        else Sleep((DWORD)std::max(1.0, t - now));
    }
};

static void Presenter(XeFg* f)
{
    Gpu& g = *f->g;
    DisplayClock clock; clock.vb = OverlayVBlankMs(f->ov);
    // hold: how far behind now the content time runs. A pair that lands at time s causes no pause only if
    // the content has not yet passed its start ta, i.e. hold >= s - ta. That need is measured per pair and
    // hold = the max over the last kNeed pairs - half a refresh + margin: a max, not a mean, because a
    // 45 fps game on 120 Hz lands 16.7 / 25 ms apart and a margin sized for the mean paused the motion.
    static const int kNeed = 64;
    double need[kNeed] = {}; int need_n = 0; double hold = 0; bool have_hold = false;
    // hold_target is what the pairs ask for; hold moves toward it per presented frame - up by at most
    // 0.5 ms a refresh, so a rise slows the content a little for a while instead of freezing a whole refresh.
    double hold_target = 0;
    double interval = 0; bool have_interval = false;   // EMA of tb - ta: the game's frame time
    // Learned margin on top (under load pairs land late more often): every SHORT pause that ends because a
    // late pair arrived adds its length (a paused game or a loading screen must not ratchet the latency
    // up); it decays ~0.1 ms a second, so a machine that calms down gets the latency back.
    double extra = 0, held_since = -1, extra_t = 0;
    bool vsync_applied = false;
    double lead_now = 0, last_signal = 0;   // vsync: the adaptive lead, and when the swapchain last signalled
    // Smoothness check: the content clock should advance one refresh per presented frame. A step under
    // half a refresh is a stall (motion stops), over 1.5 a jump; each is blamed on its cause.
    double c_shown = -1, hold_shown = 0; bool held_now = false, early_now = false;
    // Extrapolation runs the same clock with a different hold: a pair is NEEDED when its own frame is
    // due (s - tb, a few ms) instead of when its predecessor runs out (s - ta, a frame interval more),
    // and when the content time passes the newest frame it is pushed ahead along its own flow instead
    // of held - up to kMaxAhead intervals, beyond which the game really is late (held).
    static const double kMaxAhead = 1.0;
    bool extrap_prev = false; bool last_x = false;
    double c_prev = -1e300, last_present = 0, last_t = -1;
    UINT64 last_seq = 0, seen_seq = 0;
    int stamp_slot = -1;
    bool pass_prev = false;
    while (!f->stop && !f->failed)
    {
        // Vsync paces on the swapchain itself (max frame latency 1: it signals when the last frame reached
        // the screen, i.e. at a vblank), then renders LATE: the flip waits for the next vblank anyway, so
        // sleep until `lead` before it - the frame is `lead` old when it lands instead of a whole refresh.
        // Immediate mode paces on the vblank and flips at once.
        // The lead adapts: the swapchain signals a whole refresh late when the last frame missed its vblank,
        // and each miss renders 0.25 ms earlier from then on (up to 4 ms); on time, it creeps back toward
        // the minimum (--lead), ~0.1 ms a second.
        double woke;
        if (f->vsync && OverlayWaitFrameLatency(f->ov, 100))
        {
            woke = NowMs();
            const double min_lead = f->lead.load(std::memory_order_relaxed);
            if (lead_now < min_lead) lead_now = min_lead;
            if (last_signal > 0 && woke - last_signal > 1.5 * clock.vb) { lead_now = std::min(lead_now + 0.25, 4.0); ++f->missed; }
            else lead_now = std::max(min_lead, lead_now - 0.0008);
            last_signal = woke;
            clock.SleepUntil(woke + clock.vb - lead_now);
        }
        else { if (f->vsync) ++f->wait_timeouts; clock.Wait(f->ov); last_signal = 0; }
        woke = NowMs();
        f->lead_ms = f->vsync ? lead_now : 0.0;
        if (f->stop) break;
        const double now = NowMs();
        // interp GPU ms of the previous present (its ctx slot has retired by now or it is skipped)
        if (stamp_slot >= 0) { double ms; if (GpuCtxStampsMsSlot(f->ctx, stamp_slot, &ms, 1) && ms >= 0) { std::lock_guard<std::mutex> lk(f->mu); if (f->interp_ms.size() < 4096) f->interp_ms.push_back(ms); } stamp_slot = -1; }

        std::unique_lock<std::mutex> lk(f->mu);
        XePair* newest = nullptr;
        for (auto& p : f->pr) if (p.valid && (!newest || p.seq > newest->seq)) newest = &p;
        if (!newest) continue;
        // Once per new pair: learn from the pause it ended, the interval EMA, and the hold it needs.
        if (newest->seq != seen_seq)
        {
            seen_seq = newest->seq;
            if (held_since >= 0)
            {
                const double late = now - held_since;
                // Not while extrapolating: lateness up to an interval is absorbed by pushing further ahead,
                // and learning from a real stall there would buy back the latency the mode removes.
                // Only a pair late DESPITE the full hold teaches anything: while the hold ramps up, late
                // pairs are expected (learning from them pinned the margin at its 15 ms cap at every start).
                if (!f->extrap && have_interval && late < 2.0 * interval && hold >= hold_target - 0.5)
                { extra = std::min(extra + late + 0.5, 15.0); hold_target += late + 0.5; }
                held_since = -1;
            }
            if (newest->flow)
            {
                const double iv = newest->tb - newest->ta, n = newest->submit_ms - (f->extrap ? newest->tb : newest->ta);
                if (iv > 1 && iv < 100) { interval = have_interval ? interval + 0.1 * (iv - interval) : iv; have_interval = true; }
                // Extrapolating, the need (s - tb) is a few ms and can be NEGATIVE: Windows.Graphics.Capture
                // may stamp a frame slightly after we receive it. So the sign is no test - only range.
                if (n > -20 && n < 250)
                {
                    need[need_n++ % kNeed] = n;
                    double mx = 0; for (int k = 0; k < std::min(need_n, kNeed); ++k) mx = std::max(mx, need[k]);
                    // - half a refresh: the presenter only looks at vblanks, so a pair "late" by less than
                    // that between two of them never shows as a pause (measured at 45 on 120 Hz: ~5 ms ok, 8 too far).
                    const double target = mx - 0.5 * clock.vb + f->margin.load(std::memory_order_relaxed) + extra;
                    hold_target = std::max(0.0, target);   // content time never ahead of now
                    if (!have_hold) hold = hold_target;
                    have_hold = true;
                }
            }
        }
        if (f->extrap != extrap_prev)
        {
            // The hold means something else in each mode: start it over (one frame interval, roughly).
            extrap_prev = f->extrap; need_n = 0; hold = 0; have_hold = false; extra = 0; held_since = -1; c_prev = -1e300;
            Log("[xefg] %s", extrap_prev ? "extrapolating: the newest frame pushed ahead along its flow, no hold" : "interpolating between the last two frames");
        }
        if (extra > 0) extra = std::max(0.0, extra - 0.0001 * (now - extra_t));   // 0.1 ms per second
        extra_t = now;
        const double max_in = f->max_in.load(std::memory_order_relaxed);
        const bool fast = have_interval && max_in > 0 && interval < 1000.0 / max_in;
        const bool pass = !f->enabled || fast || !have_interval || !have_hold;
        if (pass != pass_prev)
        {
            if (!pass) Log("[xefg] generating: game at %.0f fps -> %.0f Hz", 1000.0 / interval, 1000.0 / clock.vb);
            else if (!f->enabled) Log("[xefg] generation off (F8) - showing the game's own frames");
            else if (fast) Log("[xefg] the game already runs at %.0f fps (above --max-in %.0f): nothing to generate, its own frames are shown. "
                               "Cap the game's frame rate (e.g. 45) in its settings or Intel Graphics Software to use frame generation.", 1000.0 / interval, max_in);
            else Log("[xefg] warming up");
            pass_prev = pass; c_prev = -1e300; c_shown = -1;
        }
        XePair* use = nullptr; float t = 1.0f; bool x = false;
        if (pass)
        {
            // Each real frame once, on the vblank after it lands.
            if (newest->seq == last_seq) continue;
            use = newest; t = 1.0f;
        }
        else
        {
            // up fast but never as a freeze; down slowly (~0.25%/refresh) so the content never visibly speeds up
            hold = hold_target > hold ? std::min(hold_target, hold + 0.5) : hold + 0.0025 * (hold_target - hold);
            double c = std::max(now - hold, c_prev);
            held_now = early_now = false;
            f->hold_ms = hold; f->interval_ms = interval; f->extra_ms = extra;
            XePair* oldest = nullptr;
            for (auto& p : f->pr)
            {
                if (!p.valid || !p.flow) continue;
                if (!oldest || p.seq < oldest->seq) oldest = &p;
                if (p.ta <= c && c <= p.tb && (!use || p.seq > use->seq)) use = &p;
            }
            if (!use && f->extrap && newest->flow && c > newest->tb)
            {
                // Past the newest frame: push it ahead. Beyond kMaxAhead the game is late - stop there.
                const double iv = std::max(1e-3, newest->tb - newest->ta), ahead = (c - newest->tb) / iv;
                use = newest; x = true; t = (float)std::min(ahead, kMaxAhead);
                if (ahead > kMaxAhead) { c = newest->tb + kMaxAhead * iv; ++f->held; held_now = true; if (held_since < 0) held_since = now; }
            }
            else if (!use)
            {
                if (c > newest->tb || !oldest) { use = newest; t = 1.0f; c = newest->tb; ++f->held; held_now = true; if (held_since < 0) held_since = now; }   // the game is late: hold, never extrapolate
                else { use = oldest; t = 0.0f; c = oldest->ta; ++f->early; early_now = true; }                                     // behind every pair: show its start
            }
            else t = (float)std::clamp((c - use->ta) / std::max(1e-3, use->tb - use->ta), 0.0, 1.0);
            c_prev = c;
            if (c_shown >= 0)
            {
                const double step = c - c_shown;
                if (step < 0.5 * clock.vb)
                {
                    if (held_now) ++f->stall_late;
                    else if (hold > hold_shown + 0.25) ++f->stall_hold;
                    else ++f->stall_other;
                }
                else if (step > 1.5 * clock.vb) ++f->jumps;
            }
            c_shown = c; hold_shown = hold;
            if (use->seq == last_seq && x == last_x && fabsf(t - (float)last_t) < 1e-4f) { ++f->repeats; continue; }   // nothing new to show
        }

        // Record under the lock: `use`'s frames and grids cannot be recycled until read is set.
        if (!GpuCtxBegin(g, f->ctx)) { Fail(f, "presenter queue begin (device removed?)"); break; }
        ID3D12GraphicsCommandList* cl = f->ctx.list;
        f->ctx.queue->Wait(g.fence, use->ready);   // GPU side: its frames and flow are done on the main queue
        // Straight into the swapchain's backbuffer: this queue IS the swapchain's, so DXGI orders the
        // write after the buffer's previous scan-out and the Present below after the write.
        ID3D12Resource* dst = OverlayBackbuffer(f->ov);
        const XeFrame& A = f->fr[use->a]; const XeFrame& B = f->fr[use->b];
        GpuCtxStamp(f->ctx, 0);
        XeInterpRecordRT(g, cl, f->interp, A.tex, B.tex, use->fwd, use->bwd, use->stat, XeFlowGridW(f->flow), XeFlowGridH(f->flow), XeFlowFactor(f->flow), XeFlowCell(f->flow),
                         dst, PRESENT, f->w, f->h, use->flow || x ? t : 1.0f, x);
        GpuCtxStamp(f->ctx, 1);
        const bool snap = f->snap_req && !f->snap_pending && use->flow && t > 0.0f && (x || t < 1.0f);
        if (snap)
        {
            D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
            from.pResource = dst; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = f->snap_rb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = f->snap_fp;
            GpuBarrier(cl, dst, PRESENT, CSRC);
            cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            GpuBarrier(cl, dst, CSRC, PRESENT);
        }
        const int slot = f->ctx.slot;
        const UINT64 v = GpuCtxEnd(f->ctx);
        if (!v) { Fail(f, "presenter queue submit"); break; }
        f->fr[use->a].read = f->fr[use->b].read = use->read = v;
        if (snap) { f->snap_req = false; f->snap_pending = true; f->snap_fence = v; f->snap_t = t; }
        const bool gen = use->flow && !x && t > 0.0f && t < 1.0f, ext = x && t > 0.0f;
        last_seq = use->seq; last_t = t; last_x = x;
        lk.unlock();

        if (f->vsync != vsync_applied) { vsync_applied = f->vsync; OverlaySetVsync(f->ov, vsync_applied); Log("[xefg] sync mode: %s", vsync_applied ? "vsync (no tearing)" : "immediate (tearing allowed)"); }
        const double present_at = NowMs();
        if (!OverlayPresentRecorded(f->ov)) { Fail(f, "present failed"); break; }
        stamp_slot = slot;
        const double pt = NowMs();
        std::lock_guard<std::mutex> lk2(f->mu);
        ++f->presented; if (gen) ++f->generated; if (ext) ++f->extrapolated;
        f->passthrough = pass;
        if (last_present > 0 && f->spacing.size() < 4096) f->spacing.push_back(pt - last_present);
        if (f->after_vblank.size() < 4096) f->after_vblank.push_back(present_at - woke);
        last_present = pt;
    }
}

// ---- lifecycle --------------------------------------------------------------------------------------------
XeFg* XeFgCreate(Gpu& g, HWND target, UINT w, UINT h, UINT flow_factor, const HotkeyDef* keys, int nkeys)
{
    XeFg* f = new XeFg;
    f->g = &g; f->w = w; f->h = h;
    auto fail = [&](const char* why) { Log("[xefg] create failed: %s", why); XeFgDestroy(f); return (XeFg*)nullptr; };
    if (!(f->flow = XeFlowCreate(g, w, h, flow_factor))) return fail("flow");
    if (!XeInterpInit(g, f->interp)) return fail("interp shader");
    if (!GpuCtxInit(g, f->ctx, D3D12_COMMAND_QUEUE_PRIORITY_HIGH, L"xefg")) return fail("presenter queue");
    if (!(f->ov = OverlayCreateOnQueue(g, target, w, h, keys, nkeys, f->ctx.queue))) return fail("overlay");
    for (auto& fr : f->fr)
        if (!(fr.tex = GpuMakeTex(g, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, NPSR, L"xefg_frame"))) return fail("frame textures");
    for (auto& p : f->pr)
    {
        p.fwd = GpuMakeTex(g, XeFlowGridW(f->flow), XeFlowGridH(f->flow), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"xefg_fwd");
        p.bwd = GpuMakeTex(g, XeFlowGridW(f->flow), XeFlowGridH(f->flow), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"xefg_bwd");
        p.stat = GpuMakeTex(g, XeFlowL0W(f->flow), XeFlowL0H(f->flow), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"xefg_static");
        if (!p.fwd || !p.bwd || !p.stat) return fail("pair grids");
    }
    {
        const D3D12_RESOURCE_DESC d = OverlayBackbuffer(f->ov)->GetDesc();
        g.dev->GetCopyableFootprints(&d, 0, 1, 0, &f->snap_fp, nullptr, nullptr, &f->snap_bytes);
        if (!(f->snap_rb = GpuMakeBuffer(g, f->snap_bytes, D3D12_HEAP_TYPE_READBACK, CDST, D3D12_RESOURCE_FLAG_NONE, L"xefg_snap"))) return fail("snapshot buffer");
    }
    f->th = std::thread(Presenter, f);
    if (!SetThreadPriority(f->th.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL)) Log("[xefg] presenter priority unchanged (err %lu)", GetLastError());
    Log("[xefg] %ux%u, flow grid %ux%u, %d frames / %d pairs, vblank %.2f ms", w, h, XeFlowGridW(f->flow), XeFlowGridH(f->flow), kFrames, kPairs, OverlayVBlankMs(f->ov));
    return f;
}

void XeFgDestroy(XeFg* f)
{
    if (!f) return;
    f->stop = true; f->cv.notify_all();
    if (f->th.joinable()) f->th.join();
    if (f->g) GpuWaitIdle(*f->g);
    if (f->ctx.queue) GpuCtxWaitIdle(f->ctx, 5000);   // the swapchain's queue: its presents and writes are done
    if (f->ov) { OverlayDestroy(f->ov); f->ov = nullptr; }
    for (auto& fr : f->fr) REL(fr.tex);
    for (auto& p : f->pr) { REL(p.fwd); REL(p.bwd); REL(p.stat); }
    REL(f->snap_rb);
    if (f->ctx.queue) GpuCtxShutdown(*f->g, f->ctx);
    XeInterpRelease(f->interp);
    XeFlowDestroy(f->flow);
    delete f;
}

bool XeFgSubmit(XeFg* f, ID3D12Resource* src, ID3D12Fence* wait_fence, UINT64 wait_value, double cap_ms, bool reset)
{
    if (f->failed) return false;
    Gpu& g = *f->g;
    const int i = (f->last + 1) % kFrames;
    const bool on = f->enabled;   // once: a hotkey may flip it mid-call
    const bool pair = on && f->have_pyr && f->last >= 0 && !reset && cap_ms - f->fr[f->last].t > 0.5 && cap_ms - f->fr[f->last].t < 100.0;
    const int j = (int)(f->pair_seq % kPairs);
    UINT64 wait_frame = 0, wait_pair = 0;
    {
        // Recycling: frame i and pair j leave the presenter's choice now, and the main queue waits
        // for any interp already recorded against them before it overwrites them.
        std::lock_guard<std::mutex> lk(f->mu);
        for (auto& p : f->pr) if (p.valid && (p.a == i || p.b == i)) p.valid = false;
        f->pr[j].valid = false;
        wait_frame = f->fr[i].read; wait_pair = f->pr[j].read;
    }
    if (!GpuBegin(g)) return Fail(f, "main queue begin (device removed?)");
    if (wait_fence) g.queue->Wait(wait_fence, wait_value);
    if (const UINT64 v = std::max(wait_frame, wait_pair)) g.queue->Wait(f->ctx.fence, v);
    ID3D12GraphicsCommandList* cl = g.list;
    const int stamp_slot = g.slot;
    GpuBarrier(cl, src, COMMON, CSRC);
    GpuBarrier(cl, f->fr[i].tex, NPSR, CDST);
    cl->CopyResource(f->fr[i].tex, src);
    GpuBarrier(cl, f->fr[i].tex, CDST, NPSR);
    GpuBarrier(cl, src, CSRC, COMMON);
    GpuStamp(g, cl, 0);
    if (pair)
    {
        XeFlowPyramid(f->flow, cl, f->fr[i].tex, f->slot);
        XeFlowEstimate(f->flow, cl, f->slot ^ 1, f->slot, 0);   // forward: prev -> cur
        XeFlowEstimate(f->flow, cl, f->slot, f->slot ^ 1, 1);   // backward: cur -> prev
        XeFlowStaticMap(f->flow, cl, f->slot, f->slot ^ 1, f->pr[j].stat);   // where standing still beats the flow (UI)
        ID3D12Resource* grids[2] = { XeFlowGrid(f->flow, 0), XeFlowGrid(f->flow, 1) };
        ID3D12Resource* dst[2] = { f->pr[j].fwd, f->pr[j].bwd };
        for (int d = 0; d < 2; ++d)
        {
            GpuBarrier(cl, grids[d], NPSR, CSRC); GpuBarrier(cl, dst[d], NPSR, CDST);
            cl->CopyResource(dst[d], grids[d]);
            GpuBarrier(cl, grids[d], CSRC, NPSR); GpuBarrier(cl, dst[d], CDST, NPSR);
        }
    }
    else if (on) XeFlowPyramid(f->flow, cl, f->fr[i].tex, f->slot);   // the next frame's pair needs this one's pyramid
    GpuStamp(g, cl, 1);
    const UINT64 v = GpuEnd(g);
    if (!v) return Fail(f, "main queue submit");
    // flow GPU ms of an earlier submission whose slot has retired
    if (f->flow_stamp_slot >= 0) { double ms; if (GpuStampsMsSlot(g, f->flow_stamp_slot, &ms, 1) && ms > 0.05 && f->flow_ms.size() < 4096) f->flow_ms.push_back(ms); }
    f->flow_stamp_slot = pair ? stamp_slot : -1;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        f->fr[i].t = cap_ms;
        XePair& p = f->pr[j];
        p.a = pair ? f->last : i; p.b = i; p.flow = pair;
        p.ta = pair ? f->fr[f->last].t : cap_ms; p.tb = cap_ms;
        p.submit_ms = NowMs(); p.ready = v; p.seq = ++f->pair_seq; p.valid = true;
        ++f->in;
    }
    f->last = i;
    f->have_pyr = pair || on;
    if (f->have_pyr) f->slot ^= 1;
    f->cv.notify_all();
    return true;
}

void XeFgSetEnabled(XeFg* f, bool on) { f->enabled = on; }
void XeFgSetTiming(XeFg* f, double margin_ms, double max_in_fps)
{
    f->margin.store(std::clamp(margin_ms, -20.0, 50.0)); f->max_in.store(std::clamp(max_in_fps, 0.0, 1000.0));
}
bool XeFgFailed(const XeFg* f) { return f->failed; }
void XeFgSetExtrapolate(XeFg* f, bool on) { f->extrap = on; }
void XeFgSetVsync(XeFg* f, bool on) { f->vsync = on; }
void XeFgSetVsyncLead(XeFg* f, double ms) { f->lead = std::clamp(ms, 0.5, 4.0); }
Overlay* XeFgOverlay(XeFg* f) { return f->ov; }
ID3D12Resource* XeFgLastFrame(XeFg* f) { return f->last >= 0 ? f->fr[f->last].tex : nullptr; }

void XeFgStats(XeFg* f, XeFgStatsOut& out)
{
    auto med = [](std::vector<double>& v) { if (v.empty()) return -1.0; std::sort(v.begin(), v.end()); const double m = v[v.size() / 2]; v.clear(); return m; };
    out.flow_ms = med(f->flow_ms);   // main thread (the caller) owns it
    std::lock_guard<std::mutex> lk(f->mu);
    out.in = f->in; out.presented = f->presented; out.generated = f->generated; out.extrapolated = f->extrapolated; out.held = f->held; out.early = f->early;
    out.missed = f->missed; out.lead_ms = f->lead_ms;
    out.stall_hold = f->stall_hold; out.stall_late = f->stall_late; out.stall_other = f->stall_other; out.jumps = f->jumps;
    out.repeats = f->repeats; out.wait_timeouts = f->wait_timeouts; f->repeats = f->wait_timeouts = 0;
    f->in = f->presented = f->generated = f->extrapolated = f->held = f->early = f->missed = 0;
    f->stall_hold = f->stall_late = f->stall_other = f->jumps = 0;
    out.interp_ms = med(f->interp_ms);
    out.hold_ms = f->hold_ms; out.interval_ms = f->interval_ms; out.extra_ms = f->extra_ms; out.passthrough = f->passthrough;
    out.spacing_ms.swap(f->spacing); f->spacing.clear();
    out.after_vblank_ms.swap(f->after_vblank); f->after_vblank.clear();
}

void XeFgRequestSnapshot(XeFg* f) { f->snap_req = true; }
bool XeFgTakeSnapshot(XeFg* f, std::vector<uint8_t>& rgba, float& t)
{
    UINT64 v;
    { std::lock_guard<std::mutex> lk(f->mu); if (!f->snap_pending) return false; v = f->snap_fence; t = f->snap_t; }
    if (f->ctx.fence->GetCompletedValue() < v) return false;
    uint8_t* p = nullptr; const D3D12_RANGE rr = { 0, (SIZE_T)f->snap_bytes };   // the last row is unpadded: not RowPitch * h
    if (FAILED(f->snap_rb->Map(0, &rr, (void**)&p))) return false;
    rgba.resize((size_t)f->w * f->h * 4);
    for (UINT y = 0; y < f->h; ++y) memcpy(&rgba[(size_t)y * f->w * 4], p + (size_t)y * f->snap_fp.Footprint.RowPitch, (size_t)f->w * 4);
    const D3D12_RANGE none = { 0, 0 }; f->snap_rb->Unmap(0, &none);
    std::lock_guard<std::mutex> lk(f->mu); f->snap_pending = false;
    return true;
}
