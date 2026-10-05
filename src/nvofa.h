// NVIDIA Optical Flow (NVOFA) D3D12 session on a downscaled 8-bit gray pair. Produces a
// R16G16_SINT flow grid (S10.5 fixed point, one vector per grid cell).
// The caller signals `in_fence` after writing the gray input, OfaExecuteRef waits on it in the OFA
// queue and signals OfaFence at the value it returns when the flow is ready; the D3D12 queue then Waits.
#pragma once
#include "d3d.h"

struct Ofa;

// gray input size (e.g. 960x540). Loads nvofapi64.dll (System32, then DriverStore by driver
// version, then `dll_override`), queries supported grid sizes, picks the smallest (or `grid` if
// > 0 and supported). Logs the DLL path/version, grid and formats. nullptr on failure.
Ofa* OfaCreate(Gpu& g, UINT w, UINT h, int grid /* 0 = smallest supported */, const wchar_t* dll_override, int perf = 0 /* 0 fast, 1 medium, 2 slow */);
void OfaDestroy(Ofa* o);

// Gray inputs: 5 R8_UNORM textures. Slots 0/1 are the per-frame ping-pong (the caller keeps which is
// current); slots 2..4 are free for held references (the model track keeps its model frames' grays
// there). Resting state COMMON (copy into it from a COPY_SOURCE gray texture, or dispatch into it as
// UAV and transition back to COMMON before OfaExecuteRef).
ID3D12Resource* OfaInput(Ofa* o, int which);
// Flow input_idx -> ref_idx (current -> previous: the NR convention) into output pair `out_pair`
// (0 = per-frame flow for the sync model and the warp engines, 1 = model track: OfaFlow2, 2 = current
// frame -> model frame for the residual warp: OfaFlow3). `reset` = no reference frame: the flow is
// zeroed. Returns the OfaFence value that signals when the flow is ready (0 on failure). Thread-safe.
UINT64 OfaExecuteRef(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, int input_idx, int ref_idx, int out_pair, bool reset);
ID3D12Fence*    OfaFence(Ofa* o);
// Flow output R16G16_SINT (grid cells), state COMMON at rest. Grid cell size in input pixels.
ID3D12Resource* OfaFlow(Ofa* o);
UINT            OfaFlowWidth(Ofa* o);
UINT            OfaFlowHeight(Ofa* o);
// Output pair 1 (model track) and pair 2 (residual warp, main thread).
ID3D12Resource* OfaFlow2(Ofa* o);
ID3D12Resource* OfaFlow3(Ofa* o);
