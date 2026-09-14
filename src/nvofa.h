// NVIDIA Optical Flow (NVOFA) D3D12 session on a downscaled 8-bit gray pair. Produces a
// R16G16_SINT flow grid (S10.5 fixed point, one vector per grid cell) plus an R8_UINT cost.
// The caller signals `in_fence` after writing the current gray input, OfaExecute waits on it in
// the OFA queue and signals OfaFence(value) when the flow is ready; the D3D12 queue then Waits.
#pragma once
#include "d3d.h"

struct Ofa;

// gray input size (e.g. 960x540). Loads nvofapi64.dll (System32, then DriverStore by driver
// version, then `dll_override`), queries supported grid sizes, picks the smallest (or `grid` if
// > 0 and supported). Logs the DLL path/version, grid and formats. nullptr on failure.
Ofa* OfaCreate(Gpu& g, UINT w, UINT h, int grid /* 0 = smallest supported */, const wchar_t* dll_override);
void OfaDestroy(Ofa* o);

// Gray inputs: OfaInputCount R8_UNORM textures. Slots 0/1 are the per-frame ping-pong: write
// `OfaInput(o, OfaCurrent(o))` this frame; slots 2..4 are free for held references (the model track
// keeps its model frames' grays there). Resting state COMMON (copy into it from a COPY_SOURCE gray
// texture, or dispatch into it as UAV and transition back to COMMON before OfaExecute).
inline int      OfaInputCount(Ofa*) { return 5; }
ID3D12Resource* OfaInput(Ofa* o, int which);
int             OfaCurrent(Ofa* o);
// Executes flow current -> previous (matches the NR convention) into output pair 0. `reset` = no
// reference frame (first frame / scene cut): the flow is zeroed instead. Swaps current/previous.
bool OfaExecute(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, bool reset);
// Explicit inputs: flow input_idx -> ref_idx into output pair `out_pair` (0 = per-frame flow read
// by the FG path, 1 = model track: OfaFlow2/OfaCost2, 2 = current frame -> model frame for the
// residual warp: OfaFlow3/OfaCost3). Does not touch OfaCurrent. Returns the OfaFence value that
// signals when the flow is ready (0 on failure). Thread-safe with OfaExecute.
UINT64 OfaExecuteRef(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, int input_idx, int ref_idx, int out_pair, bool reset);
ID3D12Fence*    OfaFence(Ofa* o);
UINT64          OfaFenceValue(Ofa* o);
// Flow output R16G16_SINT (grid cells), state COMMON at rest. Grid cell size in input pixels.
ID3D12Resource* OfaFlow(Ofa* o);
UINT            OfaFlowWidth(Ofa* o);
UINT            OfaFlowHeight(Ofa* o);
UINT            OfaGrid(Ofa* o);
// Cost output R8_UINT (same grid), COMMON at rest. Optional confidence mask input to the expand pass.
ID3D12Resource* OfaCost(Ofa* o);
// Output pair 1 (model track) and pair 2 (residual warp, main thread).
ID3D12Resource* OfaFlow2(Ofa* o);
ID3D12Resource* OfaCost2(Ofa* o);
ID3D12Resource* OfaFlow3(Ofa* o);
ID3D12Resource* OfaCost3(Ofa* o);
