// Phase 2: DLSS frame generation (nvngx_dlssg.dll, NGX feature 11) on the composed 4K frame.
// Desktop path lifted from NeuralScreen's frame_generation.inl: identity camera, flat 0.5 depth,
// our OFA motion field in work-res pixels. A presenter thread owns OverlayPresent while an Fg
// exists: it takes the oldest ready slot, evaluates DLSS-G (multiplier - 1) times on its own
// D3D12 queue (a GpuCtx: the evaluates never queue behind NR / compose on Gpu::queue; the FG
// queue waits GPU-side on the slot's render fence, the present queue on the evaluate fence),
// then presents generated frames + the real frame through the overlay's present queue, paced
// on the monitor's vblank (or a CPU timer). Any failure sets FgFailed; the caller destroys the
// Fg and recreates it (a generation failure drops to passthrough).
//
// The presenter is ALWAYS on (a DXGI Present of a composed layered 4K window costs ~10 ms of CPU;
// on the main thread that capped capture at ~70 fps). multiplier 1 = passthrough: no NGX, no
// evaluates, no gen/mv textures; the presenter CPU-waits each slot's render fence and presents the
// real frame on the next vblank (pacing=vblank) or immediately (timer). It always takes the NEWEST
// ready slot: older ready ones are dropped (counted in FgStatsOut::drops) - a display slower than
// the capture never throttles the main thread, and the frames it skips would not have reached the
// screen anyway. The slot ring, FgRecord's bounded wait and the latency stats are shared with
// generation mode.
//
// Pacing (vblank mode): output slot L = real interval / multiplier. Frame k of a slot is due at
// anchor + k*L, anchor = max(now, previous real frame's target + L) - a continuous cadence when
// the presenter is early, catch-up when late. Each frame goes out right after the vblank nearest
// its target. A generated frame later than L/2 (+ half a vblank of quantisation) is skipped, never
// shown stale; the real frame is never skipped for lateness (it is the freshest content), only
// pre-empted: a newer ready slot cancels the remaining generated frames, the real frame goes out
// on the next vblank and the newer slot takes over.
//
// Contract:
//   FgRecord  (main thread, inside list 2): copies composed (COPY_SOURCE) and mv (NPSR, restored)
//             into a slot (Gpu::queue waits GPU-side for the slot's last evaluate + present copy).
//             Slot textures are owned here, so the pipeline may overwrite out4k/mv on the next
//             frame. With every slot in flight it waits (bounded, ~2 real intervals) for the
//             presenter to retire one rather than drop a real frame; on timeout the oldest ready
//             slot is overwritten. Returns false when no slot can be taken (frame not presented).
//   FgSubmit  (main thread, after GpuEnd): hands the recorded slot to the presenter with the fence
//             value that completes the copy. reset = no interpolation against the previous frame.
//             cap_qpc/acq_qpc (QPC ticks, 0 = unknown) feed the age/pipe latency stats.
// UI rects: the composed frame already has them restored; DLSS-G warps them in generated frames
// (accepted for v1).
#pragma once
#include "d3d.h"
#include "present.h"
#include <vector>

struct Fg;

// vblank_pacing: false = CPU timer schedule (the original path, for comparison).
// mv_dilated: value of NVSDK_NGX_DLSSG_Opt_Eval_Params::motionVectorsDilated (see Evaluate).
Fg*  FgCreate(Gpu& g, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier /* 1 = passthrough, 2..4 */,
              bool vblank_pacing, bool mv_dilated);
void FgDestroy(Fg* f);   // stops the presenter (drains the GPU), releases the feature and textures
int  FgMultiplier(const Fg* f);   // as created (1 = passthrough)
// mv may be nullptr in passthrough (never read).
bool FgRecord(Fg* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* composed_rgba8, ID3D12Resource* mv);
void FgSubmit(Fg* f, UINT64 render_fence_value, bool reset, LONGLONG cap_qpc, LONGLONG acq_qpc);
// Live pacing knob (no rebuild): phase_ms shifts every scheduled present target (negative = earlier).
void FgSetTiming(Fg* f, double phase_ms);
bool FgFailed(const Fg* f);

struct FgStatsOut
{
    UINT presented = 0, drops = 0;     // presented frames (real + generated), skipped/overwritten frames
    UINT gen_shown = 0;                // generated frames that actually reached the screen
    // why a real frame generated nothing: these three plus gen_shown account for every real frame
    UINT no_pair = 0;                  // sequence gap or reset: nothing to interpolate against
    UINT disabled = 0;                 // DLSS-G raised pOutputDisableInterpolation for the pair
    UINT preempts = 0;                 // a newer slot arrived before the generated frame was due
    // Sums, not means: FgStats is drained every frame and the caller aggregates over its own
    // window. A mean here could not be added up, and silently read as -1.
    double vblank_wait_sum_ms = 0; UINT vblank_waits = 0;   // presenter blocked in OverlayWaitVBlank
    double record_wait_sum_ms = 0; UINT record_waits = 0;   // MAIN thread in FgRecord, no free slot
    std::vector<double> spacing_ms;    // present-to-present spacing of everything shown
    std::vector<double> age_ms;        // real frames: our present - capture timestamp
    std::vector<double> pipe_ms;       // real frames: our present - capture acquire
};
// Everything since the last call (the vectors are handed over, not copied).
void FgStats(Fg* f, FgStatsOut& out);
// GPU ms of DLSS-G generation per real frame (the slot's multiplier-1 evaluates, FG queue timestamps),
// median of the last 256; *p95 optional. -1 while nothing has been measured (passthrough, warm-up).
double FgEvalMs(Fg* f, double* p95);
