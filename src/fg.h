// Phase 2: DLSS frame generation (nvngx_dlssg.dll, NGX feature 11) on the composed 4K frame.
// Desktop path lifted from NeuralScreen's frame_generation.inl: identity camera, flat 0.5 depth,
// our OFA motion field in work-res pixels. A presenter thread owns OverlayPresent while an Fg
// exists: it picks the newest ready slot, evaluates DLSS-G (multiplier - 1) times on its own
// list/allocator/fence (executed on Gpu::queue), then presents generated frames + the real frame
// through the overlay's present queue, paced on the monitor's vblank (or a CPU timer). Any failure
// sets FgFailed; the caller destroys the Fg and presentation returns to the main thread.
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
//             into a slot. Slot textures are owned here, so the pipeline may overwrite out4k/mv
//             on the next frame. Returns false when no slot can be taken (frame not presented).
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
Fg*  FgCreate(Gpu& g, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier /* 2..4 */,
              bool vblank_pacing, bool mv_dilated);
void FgDestroy(Fg* f);   // stops the presenter (drains the GPU), releases the feature and textures
bool FgRecord(Fg* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* composed_rgba8, ID3D12Resource* mv);
void FgSubmit(Fg* f, UINT64 render_fence_value, bool reset, LONGLONG cap_qpc, LONGLONG acq_qpc);
bool FgFailed(const Fg* f);

struct FgStatsOut
{
    UINT presented = 0, drops = 0;     // presented frames (real + generated), skipped/overwritten frames
    std::vector<double> spacing_ms;    // present-to-present spacing of everything shown
    std::vector<double> age_ms;        // real frames: our present - capture timestamp
    std::vector<double> pipe_ms;       // real frames: our present - capture acquire
};
// Everything since the last call (the vectors are handed over, not copied).
void FgStats(Fg* f, FgStatsOut& out);
