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

// Gray inputs: two R8_UNORM textures (ping-pong). Write `OfaInput(o, OfaCurrent(o))` this frame;
// resting state COMMON (copy into it from a COPY_SOURCE gray texture, or dispatch into it as UAV
// and transition back to COMMON before OfaExecute).
ID3D12Resource* OfaInput(Ofa* o, int which);
int             OfaCurrent(Ofa* o);
// Executes flow current -> previous (matches the NR convention). `reset` = no reference frame
// (first frame / scene cut): the flow is zeroed instead. Swaps current/previous after.
bool OfaExecute(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, bool reset);
ID3D12Fence*    OfaFence(Ofa* o);
UINT64          OfaFenceValue(Ofa* o);
// Flow output R16G16_SINT (grid cells), state COMMON at rest. Grid cell size in input pixels.
ID3D12Resource* OfaFlow(Ofa* o);
UINT            OfaFlowWidth(Ofa* o);
UINT            OfaFlowHeight(Ofa* o);
UINT            OfaGrid(Ofa* o);
// Cost output R8_UINT (same grid), COMMON at rest. Optional confidence mask input to the expand pass.
ID3D12Resource* OfaCost(Ofa* o);
