// Compute passes (all cs_5_0, compiled by fxc at build time into gen/*.h, bound through
// GpuDispatch): swizzle, gray, downscale, expand (flow -> motion vectors), compose (matched
// residual + UI rects + wipe), text (toast / HUD), sharpen (CAS-style). Every function records into `cl`; the caller owns barriers:
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
    UiRect rects[64];
    float warp = 1.0f;                // CsComposeResidual only: residual sampled at uv + mv_uv * warp
    // addon mask strip (top-left strip_w x strip_h px): output rows 0..strip_h-1 there replicate the
    // composed rows just beneath, so the strip's pixels never reach the screen. 0 = off.
    int   strip_w = 0, strip_h = 0;
};
// native RGBA8 (w x h) + nr_in / nr_out RGBA8 (ww x wh) -> out RGBA8 (w x h).
void CsCompose(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* native, ID3D12Resource* nr_in, ID3D12Resource* nr_out,
               UINT ww, UINT wh, ID3D12Resource* out, UINT w, UINT h, const ComposeParams& p);

// ---- decoupled model track ------------------------------------------------------------------
// nr_in / nr_out RGBA8 (ww x wh) -> residual R16G16B16A16_FLOAT (ww x wh) = nr_out - nr_in (signed).
void CsResidual(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* nr_in, ID3D12Resource* nr_out, UINT ww, UINT wh,
                ID3D12Resource* dst_residual);
// native RGBA8 (w x h) + residual RGBA16F (ww x wh) warped by mv R16G16_FLOAT (motion from this
// frame to the residual's model frame, in work-res pixels; any size, sampled by uv) -> out RGBA8 (w x h).
// Rects / feather / wipe as CsCompose; p.warp scales the warp (0 = no warp).
void CsComposeResidual(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* native, ID3D12Resource* residual, UINT ww, UINT wh,
                       ID3D12Resource* mv, ID3D12Resource* out, UINT w, UINT h, const ComposeParams& p);
// ---- on-screen text -------------------------------------------------------------------------
// Draws `text` (ASCII 32..126, up to 64 chars, 8x8 font at `scale` px per font pixel) white with a
// 1 px dark outline on a rounded dark box (padding box_pad) into an RGBA8 UAV (w x h). x/y = box
// top-left; x < 0 centres horizontally, y < 0 puts it 48 px from the top. Dispatch covers the box only.
void CsText(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT w, UINT h, const char* text,
            int x, int y, int scale, float alpha, int box_pad);
static const size_t kMaxText = 64;
inline int TextBoxW(size_t len, int scale, int pad) { return (int)(len < kMaxText ? len : kMaxText) * 8 * scale + 2 * pad; }
inline int TextBoxH(int scale, int pad) { return 8 * scale + 2 * pad; }

// ---- sharpen ----------------------------------------------------------------------------------
// Contrast-adaptive sharpen (CAS-style) RGBA8 src (NPSR) -> dst (UAV), both w x h, strength 0..1.
// Pixels inside the first `nrects` rects of the rect texture (as uploaded by the compose recorded
// earlier in the same list) pass through.
void CsSharpen(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h,
               float strength, UINT nrects);

// ---- ArtCNN ----------------------------------------------------------------------------------
// ArtCNN C4F16_DS (shaders/artcnn.hlsl, generated by tools/artcnn_port.py) on the luma of RGBA8 src (NPSR)
// -> dst (UAV) = src + (luma' - luma), both w x h. 7 conv passes over 3 scratch RGBA16F feature textures
// (2w x 2h) kept in `s`, (re)allocated lazily for the last (w, h): callers change size only while idle.
// Works on a GpuCtx list too (sync and async never run it concurrently).
void CsArtCnn(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src_rgba8, ID3D12Resource* dst_rgba8, UINT w, UINT h);

// Blocking round trip on g.list (not inside GpuBegin/GpuEnd): residual + compose_residual with mv=0
// against a CPU reference within 1/255. Logs PASS/FAIL.
bool ComposeSelfTest(Gpu& g);
// Blocking: CsArtCnn on a flat grey image (must pass through unchanged, |delta| <= 2/255) and on a step
// edge (must change something). Logs PASS/FAIL with the measured deltas.
bool ArtCnnSelfTest(Gpu& g);
