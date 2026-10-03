#include "xe_flow.h"
#include "log.h"
#include "cs_gray.h"
#include "cs_xe_down.h"
#include "cs_xe_flow.h"
#include "cs_xe_flow_r2.h"
#include "cs_xe_median.h"
#include "cs_xe_static.h"
#include "cs_xe_refine.h"
#include <algorithm>

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const int kMaxLevels = 8;
static const UINT kBlock = 8;

struct XeFlow
{
    Gpu* g = nullptr;
    UINT w = 0, h = 0, factor = 2;
    int  levels = 0;
    UINT lw[kMaxLevels] = {}, lh[kMaxLevels] = {};   // level sizes
    UINT gw[kMaxLevels] = {}, gh[kMaxLevels] = {};   // block grid sizes
    float lambda = 0.0f;
    int  refine = 1;                                 // same-level propagation passes per level
    int  fine = 1;                                   // the finest `fine` levels search +-2 px (xe_flow_r2), the rest +-4
    int  stamp_base = -1;                            // profiling: GpuStamp pair stamp_base + 2k around level k (-1 = off)
    ComputePso gray, down, flow, flow_r2, median, prop, stat;
    ID3D12Resource* pyr[2][kMaxLevels] = {};         // R8 luma, NPSR
    ID3D12Resource* grid[2][kMaxLevels] = {};        // R16G16F flow per direction, NPSR
    ID3D12Resource* tmp[kMaxLevels] = {};            // raw (pre-median) flow, NPSR
};

#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

XeFlow* XeFlowCreate(Gpu& g, UINT w, UINT h, UINT factor, int levels)
{
    XeFlow* f = new XeFlow;
    f->g = &g; f->w = w; f->h = h; f->factor = std::clamp(factor, 1u, 4u);
    f->lw[0] = (w + f->factor - 1) / f->factor; f->lh[0] = (h + f->factor - 1) / f->factor;
    int n = 1;
    // Down to ~32 px wide: the coarsest level is then a handful of blocks, and the search there
    // (+-4 px = +-4 * 2^(n-1) L0 px) is what lets a fast pan be found at all.
    while (n < kMaxLevels && (levels <= 0 ? (f->lw[n - 1] + 1) / 2 >= 32 : n < levels))
    {
        f->lw[n] = (f->lw[n - 1] + 1) / 2; f->lh[n] = (f->lh[n - 1] + 1) / 2; ++n;
    }
    f->levels = n;
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    bool ok = GpuMakeCompute(g, g_cs_gray, sizeof g_cs_gray, 1, 1, 6, f->gray, L"xe_luma")
           && GpuMakeCompute(g, g_cs_xe_down, sizeof g_cs_xe_down, 1, 1, 4, f->down, L"xe_down")
           && GpuMakeCompute(g, g_cs_xe_flow, sizeof g_cs_xe_flow, 3, 1, 7, f->flow, L"xe_flow")
           && GpuMakeCompute(g, g_cs_xe_flow_r2, sizeof g_cs_xe_flow_r2, 3, 1, 7, f->flow_r2, L"xe_flow_r2")
           && GpuMakeCompute(g, g_cs_xe_median, sizeof g_cs_xe_median, 1, 1, 2, f->median, L"xe_median")
           && GpuMakeCompute(g, g_cs_xe_refine, sizeof g_cs_xe_refine, 3, 1, 5, f->prop, L"xe_refine")
           && GpuMakeCompute(g, g_cs_xe_static, sizeof g_cs_xe_static, 3, 1, 4, f->stat, L"xe_static");
    for (int k = 0; ok && k < n; ++k)
    {
        f->gw[k] = (f->lw[k] + kBlock - 1) / kBlock; f->gh[k] = (f->lh[k] + kBlock - 1) / kBlock;
        for (int s = 0; s < 2; ++s)
        {
            ok = ok && (f->pyr[s][k] = GpuMakeTex(g, f->lw[k], f->lh[k], DXGI_FORMAT_R8_UNORM, uav, NPSR, L"xe_pyr"));
            ok = ok && (f->grid[s][k] = GpuMakeTex(g, f->gw[k], f->gh[k], DXGI_FORMAT_R16G16_FLOAT, uav, NPSR, L"xe_grid"));
        }
        ok = ok && (f->tmp[k] = GpuMakeTex(g, f->gw[k], f->gh[k], DXGI_FORMAT_R16G16_FLOAT, uav, NPSR, L"xe_grid_raw"));
    }
    if (!ok) { Log("[xeflow] create failed"); XeFlowDestroy(f); return nullptr; }
    Log("[xeflow] %ux%u factor %u: L0 %ux%u, %d levels down to %ux%u, grid %ux%u, reach ~%u native px",
        w, h, f->factor, f->lw[0], f->lh[0], n, f->lw[n - 1], f->lh[n - 1], f->gw[0], f->gh[0], 4u * ((1u << n) - 1) * f->factor);
    return f;
}

void XeFlowDestroy(XeFlow* f)
{
    if (!f) return;
    for (int k = 0; k < kMaxLevels; ++k) { REL(f->pyr[0][k]); REL(f->pyr[1][k]); REL(f->grid[0][k]); REL(f->grid[1][k]); REL(f->tmp[k]); }
    for (ComputePso* p : { &f->gray, &f->down, &f->flow, &f->flow_r2, &f->median, &f->prop, &f->stat }) { REL(p->pso); REL(p->root); }
    delete f;
}

void XeFlowPyramid(XeFlow* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* frame, int slot)
{
    Gpu& g = *f->g;
    ID3D12Resource** pyr = f->pyr[slot & 1];
    {
        const UINT c[6] = { f->w, f->h, f->lw[0], f->lh[0], f->factor, f->factor };
        const GpuView srv = { frame, DXGI_FORMAT_UNKNOWN }, dst = { pyr[0], DXGI_FORMAT_UNKNOWN };
        GpuBarrier(cl, pyr[0], NPSR, UAV);
        GpuDispatch(g, cl, f->gray, &srv, &dst, c, GpuGroups(f->lw[0], 8), GpuGroups(f->lh[0], 8));
        GpuBarrier(cl, pyr[0], UAV, NPSR);
    }
    for (int k = 1; k < f->levels; ++k)
    {
        const UINT c[4] = { f->lw[k - 1], f->lh[k - 1], f->lw[k], f->lh[k] };
        const GpuView srv = { pyr[k - 1], DXGI_FORMAT_UNKNOWN }, dst = { pyr[k], DXGI_FORMAT_UNKNOWN };
        GpuBarrier(cl, pyr[k], NPSR, UAV);
        GpuDispatch(g, cl, f->down, &srv, &dst, c, GpuGroups(f->lw[k], 8), GpuGroups(f->lh[k], 8));
        GpuBarrier(cl, pyr[k], UAV, NPSR);
    }
}

void XeFlowEstimate(XeFlow* f, ID3D12GraphicsCommandList* cl, int from, int to, int dir)
{
    Gpu& g = *f->g;
    ID3D12Resource** out = f->grid[dir & 1];
    for (int k = f->levels - 1; k >= 0; --k)
    {
        const bool parent = k + 1 < f->levels;
        if (f->stamp_base >= 0) GpuStamp(g, cl, f->stamp_base + 2 * k);
        struct { UINT lw, lh, pw, ph, use_parent, subpixel; float lambda; } c =
            { f->lw[k], f->lh[k], parent ? f->gw[k + 1] : 1, parent ? f->gh[k + 1] : 1, parent ? 1u : 0u, k == 0 ? 1u : 0u, f->lambda };
        // Without a parent the slot is bound but never read: out[k] is NPSR and not the target.
        const GpuView srv[3] = { { f->pyr[from & 1][k], DXGI_FORMAT_UNKNOWN }, { f->pyr[to & 1][k], DXGI_FORMAT_UNKNOWN },
                                 { parent ? out[k + 1] : out[k], DXGI_FORMAT_UNKNOWN } };
        const GpuView raw = { f->tmp[k], DXGI_FORMAT_UNKNOWN };
        GpuBarrier(cl, f->tmp[k], NPSR, UAV);
        GpuDispatch(g, cl, k < f->fine && parent ? f->flow_r2 : f->flow, srv, &raw, &c, f->gw[k], f->gh[k]);
        GpuBarrier(cl, f->tmp[k], UAV, NPSR);

        const UINT mc[2] = { f->gw[k], f->gh[k] };
        const GpuView msrv = { f->tmp[k], DXGI_FORMAT_UNKNOWN }, mdst = { out[k], DXGI_FORMAT_UNKNOWN };
        auto median = [&]
        {
            GpuBarrier(cl, out[k], NPSR, UAV);
            GpuDispatch(g, cl, f->median, &msrv, &mdst, mc, GpuGroups(f->gw[k], 8), GpuGroups(f->gh[k], 8));
            GpuBarrier(cl, out[k], UAV, NPSR);
        };
        median();
        // Propagation: out[k] -> tmp[k] (neighbours' vectors tried), then the median back into out[k].
        for (int r = 0; r < f->refine; ++r)
        {
            const UINT rc[5] = { f->lw[k], f->lh[k], f->gw[k], f->gh[k], k == 0 ? 1u : 0u };
            const GpuView rs[3] = { srv[0], srv[1], { out[k], DXGI_FORMAT_UNKNOWN } };
            GpuBarrier(cl, f->tmp[k], NPSR, UAV);
            GpuDispatch(g, cl, f->prop, rs, &raw, rc, f->gw[k], f->gh[k]);
            GpuBarrier(cl, f->tmp[k], UAV, NPSR);
            median();
        }
        if (f->stamp_base >= 0) GpuStamp(g, cl, f->stamp_base + 2 * k + 1);
    }
}

void XeFlowSetLambda(XeFlow* f, float lambda) { f->lambda = std::max(0.0f, lambda); }
void XeFlowSetRefine(XeFlow* f, int passes) { f->refine = std::clamp(passes, 0, 4); }

void XeFlowStaticMap(XeFlow* f, ID3D12GraphicsCommandList* cl, int cur, int prev, ID3D12Resource* dst)
{
    const UINT c[4] = { f->lw[0], f->lh[0], f->gw[0], f->gh[0] };
    const GpuView srv[3] = { { f->pyr[cur & 1][0], DXGI_FORMAT_UNKNOWN }, { f->pyr[prev & 1][0], DXGI_FORMAT_UNKNOWN }, { f->grid[1][0], DXGI_FORMAT_UNKNOWN } };
    const GpuView uav = { dst, DXGI_FORMAT_UNKNOWN };
    GpuBarrier(cl, dst, NPSR, UAV);
    GpuDispatch(*f->g, cl, f->stat, srv, &uav, c, GpuGroups(f->lw[0], 8), GpuGroups(f->lh[0], 8));
    GpuBarrier(cl, dst, UAV, NPSR);
}
void XeFlowSetStamps(XeFlow* f, int base) { f->stamp_base = base; }
void XeFlowSetFine(XeFlow* f, int levels) { f->fine = std::clamp(levels, 0, kMaxLevels); }
ID3D12Resource* XeFlowGrid(XeFlow* f, int dir) { return f->grid[dir & 1][0]; }
UINT XeFlowGridW(const XeFlow* f) { return f->gw[0]; }
UINT XeFlowGridH(const XeFlow* f) { return f->gh[0]; }
UINT XeFlowFactor(const XeFlow* f) { return f->factor; }
int  XeFlowLevels(const XeFlow* f) { return f->levels; }
UINT XeFlowL0W(const XeFlow* f) { return f->lw[0]; }
UINT XeFlowL0H(const XeFlow* f) { return f->lh[0]; }
