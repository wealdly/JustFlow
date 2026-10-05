// Frame generation (DLSS-G, nvngx_dlssg.dll, NGX feature 11) on the composed output frame: identity
// camera, flat 0.5 depth, a constant zero motion field (DLSS-G measures motion itself - see fg.cpp Evaluate).
// A presenter thread owns OverlayPresent while an Fg exists: it takes the newest ready slot, evaluates
// DLSS-G on its own D3D12 queue (a GpuCtx, so evaluates never queue behind NR / compose on Gpu::queue;
// the FG queue waits GPU-side on the slot's render fence, the present queue on the evaluate fence),
// then presents generated frames + the real frame through the overlay's present queue, paced on the
// monitor's vblank (or a CPU timer). Any failure sets FgFailed; the caller destroys and recreates the Fg.
//
// The presenter is ALWAYS on (a DXGI Present of a composed layered 4K window costs ~10 ms of CPU, which
// capped capture at ~70 fps on the main thread). multiplier 1 = passthrough: no NGX, no evaluates; the
// presenter CPU-waits each slot's render fence and presents the real frame on the next vblank (or at
// once on the timer). Only the NEWEST ready slot is taken, older ready ones are dropped (FgStatsOut::drops):
// a display slower than the capture never throttles the main thread.
//
// Pacing (vblank mode): output slot L = real interval / multiplier. Frame k of a slot is due at
// anchor + k*L, anchor = max(now, previous real frame's target + L) - a continuous cadence when
// the presenter is early, catch-up when late. Each frame goes out right after the vblank nearest
// its target. A generated frame later than L/2 (+ half a vblank of quantisation) is skipped, never
// shown stale; the real frame is never skipped for lateness (it is the freshest content). A newer
// ready slot waits for the current one to finish (DLSS-G); engine=warp cuts its generated frames short.
//
// Contract:
//   FgAcquire (main thread, BEFORE list 2 writes the frame): takes a slot and returns the texture
//             the pipeline composes into. Orders the reuse GPU-side (the slot's last evaluate and its
//             last present) before returning. No free slot: the OLDEST READY one is taken on the
//             spot (the presenter would drop it anyway). nullptr = no slot, no present.
//   FgRecord  (main thread): the acquired slot's UI rects.
//   FgSubmit  (main thread, after GpuEnd): hands the recorded slot to the presenter with the fence
//             value that completes the frame. reset = no interpolation against the previous frame.
//             cap_qpc/acq_qpc (QPC ticks, 0 = unknown) feed the age/pipe latency stats.
// UI rects: each generated frame gets the real frame's pixels copied back inside them before it is
// presented (CopyTextureRegion per rect on the FG queue, no shader).
#pragma once
#include "d3d.h"
#include "present.h"
#include "compose.h"   // UiRect
#include <vector>

struct Fg;

// vblank_pacing: false = CPU timer schedule.
Fg*  FgCreate(Gpu& g, Shaders* sh, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier /* 1 = passthrough, 2..4 */,
              bool vblank_pacing, int engine /* 0 DLSS-G, 1 warp (CsWarp extrapolation), 2 latewarp (Frame Warp to the mouse), 3 video */,
              float lw_vfov = 1.0472f /* radians, engine 2 */);
void FgDestroy(Fg* f);   // stops the presenter (drains the GPU), releases the feature and textures
int  FgMultiplier(const Fg* f);   // as created (1 = passthrough)
// Diagnostics (bench): the generated frames of the slot evaluated most recently, in COPY_SOURCE.
// i in [0, multiplier-1); nullptr when nothing has been generated yet.
ID3D12Resource* FgDebugGen(Fg* f, int i);
ID3D12Resource* FgDebugReal(Fg* f);      // the real frame those generated frames came from

ID3D12Resource* FgAcquire(Fg* f);
// engine=warp only: generated frame i of the slot FgAcquire took, for the pipeline to write (CSRC at
// rest, UAV-capable) in the same list as the real frame. nullptr past the last one, or with DLSS-G.
ID3D12Resource* FgWarpTarget(Fg* f, int i);
// engine=latewarp only: the no-warp mask of the slot FgAcquire took (R8, NPSR at rest), for the pipeline to
// write in the same list (CsNoWarpMask). nullptr for the other engines. FgDebugMask: bench.
ID3D12Resource* FgMaskTarget(Fg* f);
// engine=video only: the acquired slot's flow texture (R16G16_FLOAT mv size, NPSR at rest) - the pipeline copies
// this frame's backward flow into it in the same list. nullptr for the other engines.
ID3D12Resource* FgFlowTarget(Fg* f);
ID3D12Resource* FgDebugMask(Fg* f);
void FgDebugHold(Fg* f);   // bench: freeze the latewarp presenter before reading the debug textures
// rects/nrects: the addon UI mask for THIS frame, in output pixels, restored onto every generated
// frame out of s->real before it is presented.
bool FgRecord(Fg* f, const UiRect* rects = nullptr, int nrects = 0);
void FgSubmit(Fg* f, UINT64 render_fence_value, bool reset, LONGLONG cap_qpc, LONGLONG acq_qpc);
// Live pacing knob (no rebuild): phase_ms shifts every scheduled present target (negative = earlier).
// min_gain: the governor pauses generation when frames presented / frames submitted stays below
// this for 2 s (0 = never pause). It re-probes on a 3 s -> 30 s back-off and slews in and out.
void FgSetTiming(Fg* f, double phase_ms, double min_gain, double max_in_fps);
bool FgPaused(Fg* f);   // generation currently switched off by the governor
bool FgFailed(const Fg* f);

struct FgStatsOut
{
    UINT presented = 0, drops = 0;     // presented frames (real + generated), skipped/overwritten frames
    UINT gen_shown = 0;                // generated frames that actually reached the screen
    // why a real frame generated nothing: these three plus gen_shown account for every real frame
    UINT no_pair = 0;                  // sequence gap or reset: nothing to interpolate against
    UINT disabled = 0;                 // DLSS-G raised pOutputDisableInterpolation for the pair
    UINT paused = 0;                   // real frames presented while the governor had generation off
    UINT preempts = 0;                 // a newer slot arrived before the generated frame was due
    // Sums, not means: FgStats is drained every frame and the caller aggregates over its own window.
    double vblank_wait_sum_ms = 0; UINT vblank_waits = 0;   // presenter blocked in OverlayWaitVBlank
    double record_wait_sum_ms = 0; UINT record_waits = 0;   // MAIN thread in FgAcquire, no free slot
    std::vector<double> spacing_ms;    // present-to-present spacing of everything shown
    std::vector<double> age_ms;        // real frames: our present - capture timestamp
    std::vector<double> pipe_ms;       // real frames: our present - capture acquire
};
// Everything since the last call (the vectors are handed over, not copied).
void FgStats(Fg* f, FgStatsOut& out);
// GPU ms of DLSS-G generation per real frame (the slot's multiplier-1 evaluates, FG queue timestamps),
// median of the last 256; *p95 optional. -1 while nothing has been measured (passthrough, warm-up).
double FgEvalMs(Fg* f, double* p95);
