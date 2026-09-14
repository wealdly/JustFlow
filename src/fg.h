// Phase 2: DLSS frame generation (nvngx_dlssg.dll, NGX feature 11) on the composed 4K frame.
// Desktop path lifted from NeuralScreen's frame_generation.inl: identity camera, flat 0.5 depth,
// our OFA motion field in work-res pixels. A presenter thread owns OverlayPresent while an Fg
// exists: it picks the newest ready slot, evaluates DLSS-G (multiplier - 1) times on its own
// list/allocator/fence (executed on Gpu::queue), then presents the generated frames on a QPC
// schedule followed by the real frame. Any failure sets FgFailed; the caller destroys the Fg and
// presentation returns to the main thread.
//
// Contract:
//   FgRecord  (main thread, inside list 2): copies composed (COPY_SOURCE) and mv (NPSR, restored)
//             into a slot. Slot textures are owned here, so the pipeline may overwrite out4k/mv
//             on the next frame. Returns false when no slot can be taken (frame not presented).
//   FgSubmit  (main thread, after GpuEnd): hands the recorded slot to the presenter with the fence
//             value that completes the copy. reset = no interpolation against the previous frame.
// UI rects: the composed frame already has them restored; DLSS-G warps them in generated frames
// (accepted for v1).
#pragma once
#include "d3d.h"
#include "present.h"

struct Fg;

Fg*  FgCreate(Gpu& g, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier /* 2..4 */);
void FgDestroy(Fg* f);   // stops the presenter (drains the GPU), releases the feature and textures
bool FgRecord(Fg* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* composed_rgba8, ID3D12Resource* mv);
void FgSubmit(Fg* f, UINT64 render_fence_value, bool reset);
bool FgFailed(const Fg* f);
// Presented frames (real + generated) and dropped frames since the last call.
void FgStats(Fg* f, UINT& presented, UINT& drops);
