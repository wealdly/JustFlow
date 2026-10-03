// Frame interpolation from xe_flow's two grids (shaders/xe_interp.hlsl): bidirectional flow to the
// intermediate time, forward-backward consistency as per-side visibility, time-weighted blend.
#pragma once
#include "d3d.h"

struct XeInterp
{
    ComputePso pso;
    // the same interpolation as a full-screen draw into a render target (XeInterpRecordRT)
    ID3D12RootSignature* gfx_root = nullptr; ID3D12PipelineState* gfx_pso = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    float tol = 1.5f;       // consistency tolerance, native px ...
    float tol_rel = 0.1f;   // ... plus this fraction of the motion
};

bool XeInterpInit(Gpu& g, XeInterp& x);
void XeInterpRelease(XeInterp& x);
// dst (w x h RGBA8, UAV-capable) = the frame at time t between prev (t = 0) and cur (t = 1).
// fwd: prev -> cur grid, bwd: cur -> prev grid (XeFlowGrid 0/1 of that pair, or copies of them);
// stat: the pair's XeFlowStaticMap (where standing still beats the flow: static UI);
// gw x gh vectors, one per cell x cell L0 pixels (XeFlowCell), `factor` native pixels per L0 pixel. All inputs and dst rest in NPSR.
// debug: the visibility masks instead of colour (red = prev side rejected, green = cur side).
// XeInterpRecordRT draws the same frame into `rt` (R8G8B8A8_UNORM, RENDER_TARGET-capable, resting in
// `rt_rest` - PRESENT for a swapchain backbuffer); XeInterpRecord dispatches into a UAV texture.
// extrap: instead, cur pushed t frame intervals AHEAD along bwd (t >= 0; prev and fwd unused).
void XeInterpRecord(Gpu& g, ID3D12GraphicsCommandList* cl, const XeInterp& x,
                    ID3D12Resource* prev, ID3D12Resource* cur, ID3D12Resource* fwd, ID3D12Resource* bwd, ID3D12Resource* stat,
                    UINT gw, UINT gh, UINT factor, UINT cell, ID3D12Resource* dst, UINT w, UINT h, float t, bool debug = false,
                    D3D12_RESOURCE_STATES dst_rest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, bool extrap = false);
void XeInterpRecordRT(Gpu& g, ID3D12GraphicsCommandList* cl, const XeInterp& x,
                      ID3D12Resource* prev, ID3D12Resource* cur, ID3D12Resource* fwd, ID3D12Resource* bwd, ID3D12Resource* stat,
                      UINT gw, UINT gh, UINT factor, UINT cell, ID3D12Resource* rt, D3D12_RESOURCE_STATES rt_rest, UINT w, UINT h, float t, bool extrap = false);
