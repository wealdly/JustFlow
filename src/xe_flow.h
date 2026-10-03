// Compute-shader optical flow for the vendor-neutral frame generator (justflow_xe): no NVOFA.
// Each frame gets an R8 luma pyramid (L0 = native / factor, then halves down to ~32 px wide). The
// flow between two frames is block matching, coarse to fine: 8x8 blocks, +-4 px search per level
// around the best of the parent level's predictors, a 3x3 vector median after every level, then a
// same-level propagation pass (each block tries its neighbours' vectors, +-1 px) and a sub-pixel fit
// on L0. Reach is ~4 * 2^levels L0 pixels (~500 native px at 2880 wide, factor 2).
//
// Grids: one vector per 8x8 L0 block (R16G16_FLOAT, L0 pixels). Direction d of XeFlowEstimate is
// "blocks of `from`, positions in `to`": from = cur, to = prev is the backward flow (the NR/NVOFA
// convention, previous minus current), from = prev, to = cur the forward one.
//
// Everything is recorded into the caller's list; resting states are NON_PIXEL_SHADER_RESOURCE.
#pragma once
#include "d3d.h"

struct XeFlow;

// w x h: native frame size. factor: native pixels per L0 pixel (1..4). levels 0 = auto.
XeFlow* XeFlowCreate(Gpu& g, UINT w, UINT h, UINT factor, int levels = 0);
void    XeFlowDestroy(XeFlow* f);
// Luma pyramid of `frame` (w x h RGBA/BGRA, NPSR) into slot 0 or 1.
void    XeFlowPyramid(XeFlow* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* frame, int slot);
// Flow for the blocks of slot `from` into slot `to`, into output `dir` (0 or 1).
void    XeFlowEstimate(XeFlow* f, ID3D12GraphicsCommandList* cl, int from, int to, int dir);
void    XeFlowSetRefine(XeFlow* f, int passes);    // propagation passes per level, 0..4 (default 1)
void    XeFlowSetFine(XeFlow* f, int levels);      // the finest N levels search +-2 px instead of +-4 (default 1)
void    XeFlowSetStamps(XeFlow* f, int base);      // profiling (Gpu list only): stamp pair base + 2k around level k; -1 = off
void    XeFlowSetLambda(XeFlow* f, float lambda);   // search penalty per pixel of offset from the predictor (default 0: measured best on the scene sequences)

ID3D12Resource* XeFlowGrid(XeFlow* f, int dir);     // L0 grid of output `dir`, NPSR
UINT XeFlowGridW(const XeFlow* f);
UINT XeFlowGridH(const XeFlow* f);
UINT XeFlowFactor(const XeFlow* f);
int  XeFlowLevels(const XeFlow* f);
UINT XeFlowL0W(const XeFlow* f);
UINT XeFlowL0H(const XeFlow* f);
