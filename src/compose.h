// Compute passes (all cs_5_0, compiled by fxc at build time into gen/*.h, bound through
// GpuDispatch): swizzle, gray, downscale, expand (flow -> motion vectors), compose (matched
// residual + UI rects + wipe). Every function records into `cl`; the caller owns barriers:
//   sources must be NON_PIXEL_SHADER_RESOURCE, destinations UNORDERED_ACCESS.
#pragma once
#include "d3d.h"

struct Shaders;
Shaders* ShadersCreate(Gpu& g);
void     ShadersDestroy(Shaders* s);

// BGRA8 capture (w x h) -> RGBA8 color (w x h).
void CsSwizzle(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src_bgra, ID3D12Resource* dst_rgba, UINT w, UINT h);
// RGBA8 (w x h) -> R8 luminance (gw x gh), exact area average (block = w/gw x h/gh, integer).
void CsGray(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src_rgba, UINT w, UINT h, ID3D12Resource* dst_r8, UINT gw, UINT gh);
// RGBA8 (w x h) -> RGBA8 (dw x dh), area box filter (any ratio).
void CsDownscale(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT w, UINT h, ID3D12Resource* dst, UINT dw, UINT dh);
// OFA flow R16G16_SINT (fw x fh cells, S10.5, units = OFA-input pixels) -> R16G16_FLOAT motion
// vectors (mw x mh) in work-resolution pixels: v = flow/32 * (mw/ofa_w, mh/ofa_h), bilinear over
// the grid, zeroed below `zero_below` px or when `reset`. cost R8_UINT optional (nullptr = none):
// cells with cost > cost_reject (0 = off) are zeroed.
void CsExpand(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* flow, ID3D12Resource* cost, UINT fw, UINT fh, UINT ofa_w, UINT ofa_h,
              ID3D12Resource* dst_mv, UINT mw, UINT mh, float zero_below, UINT cost_reject, bool reset);

struct UiRect { int x0, y0, x1, y1; };
struct ComposeParams
{
    float residual_strength = 1.0f;   // res = native + (nr_out^ - nr_in^) * strength
    int   wipe_mode = 0;              // 0 off, 1 split at wipe_x, 2 show native only (bypass)
    float wipe_x = 0.5f;              // 0..1 of width
    int   feather = 12;               // px
    int   nrects = 0;
    UiRect rects[16];
};
// native RGBA8 (w x h) + nr_in / nr_out RGBA8 (ww x wh) -> out RGBA8 (w x h).
void CsCompose(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* native, ID3D12Resource* nr_in, ID3D12Resource* nr_out,
               UINT ww, UINT wh, ID3D12Resource* out, UINT w, UINT h, const ComposeParams& p);
