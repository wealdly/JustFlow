#include "compose.h"
#include "log.h"
#include <cstring>
#include "cs_swizzle.h"
#include "cs_gray.h"
#include "cs_downscale.h"
#include "cs_expand.h"
#include "cs_compose.h"

struct Shaders
{
    ComputePso swizzle, gray, downscale, expand, compose;
    // UI rects for compose: 64x1 R32_SINT texture, refilled from a per-slot upload buffer inside
    // CsCompose (recorded into the caller's list; nothing blocks).
    ID3D12Resource* rect_tex = nullptr;
    ID3D12Resource* rect_up[Gpu::kFrames] = {};
};

static const UINT kRectBytes = 256;   // 64 ints, also the D3D12 row pitch alignment

Shaders* ShadersCreate(Gpu& g)
{
    Shaders* s = new Shaders();
    bool ok = true;
    ok &= GpuMakeCompute(g, g_cs_swizzle,   sizeof g_cs_swizzle,   1, 1, 2,  s->swizzle,   L"cs_swizzle");
    ok &= GpuMakeCompute(g, g_cs_gray,      sizeof g_cs_gray,      1, 1, 6,  s->gray,      L"cs_gray");
    ok &= GpuMakeCompute(g, g_cs_downscale, sizeof g_cs_downscale, 1, 1, 4,  s->downscale, L"cs_downscale");
    ok &= GpuMakeCompute(g, g_cs_expand,    sizeof g_cs_expand,    2, 1, 11, s->expand,    L"cs_expand");
    ok &= GpuMakeCompute(g, g_cs_compose,   sizeof g_cs_compose,   4, 1, 9,  s->compose,   L"cs_compose");
    s->rect_tex = GpuMakeTex(g, 64, 1, DXGI_FORMAT_R32_SINT, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"ui_rects");
    ok &= s->rect_tex != nullptr;
    for (int i = 0; i < Gpu::kFrames; ++i)
    {
        s->rect_up[i] = GpuMakeBuffer(g, kRectBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      D3D12_RESOURCE_FLAG_NONE, L"ui_rects_upload");
        ok &= s->rect_up[i] != nullptr;
    }
    if (!ok) { Log("[cs] shader setup failed"); ShadersDestroy(s); return nullptr; }
    return s;
}

void ShadersDestroy(Shaders* s)
{
    if (!s) return;
    ComputePso* p[] = { &s->swizzle, &s->gray, &s->downscale, &s->expand, &s->compose };
    for (ComputePso* x : p) { if (x->pso) x->pso->Release(); if (x->root) x->root->Release(); }
    if (s->rect_tex) s->rect_tex->Release();
    for (ID3D12Resource* r : s->rect_up) if (r) r->Release();
    delete s;
}

// ---------------------------------------------------------------------------------------------
void CsSwizzle(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h)
{
    const UINT c[2] = { w, h };
    const GpuView srv = { src, DXGI_FORMAT_B8G8R8A8_UNORM }, uav = { dst, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->swizzle, &srv, &uav, c, GpuGroups(w, 8), GpuGroups(h, 8));
}

void CsGray(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT w, UINT h, ID3D12Resource* dst, UINT gw, UINT gh)
{
    const UINT c[6] = { w, h, gw, gh, w / gw, h / gh };
    const GpuView srv = { src, DXGI_FORMAT_R8G8B8A8_UNORM }, uav = { dst, DXGI_FORMAT_R8_UNORM };
    GpuDispatch(g, cl, s->gray, &srv, &uav, c, GpuGroups(gw, 8), GpuGroups(gh, 8));
}

void CsDownscale(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT w, UINT h, ID3D12Resource* dst, UINT dw, UINT dh)
{
    const UINT c[4] = { w, h, dw, dh };
    const GpuView srv = { src, DXGI_FORMAT_R8G8B8A8_UNORM }, uav = { dst, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->downscale, &srv, &uav, c, GpuGroups(dw, 8), GpuGroups(dh, 8));
}

void CsExpand(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* flow, ID3D12Resource* cost, UINT fw, UINT fh, UINT ofa_w, UINT ofa_h,
              ID3D12Resource* dst_mv, UINT mw, UINT mh, float zero_below, UINT cost_reject, bool reset)
{
    struct { UINT fw, fh, ofa_w, ofa_h, mw, mh, grid; float zero_below; UINT cost_reject, use_cost, reset; } c =
        { fw, fh, ofa_w, ofa_h, mw, mh, (ofa_w + fw - 1) / fw, zero_below, cost_reject, cost && cost_reject ? 1u : 0u, reset ? 1u : 0u };
    const GpuView srv[2] = { { flow, DXGI_FORMAT_R16G16_SINT },
                             cost ? GpuView{ cost, DXGI_FORMAT_R8_UINT } : GpuView{ flow, DXGI_FORMAT_R16G16_SINT } };
    const GpuView uav = { dst_mv, DXGI_FORMAT_R16G16_FLOAT };
    GpuDispatch(g, cl, s->expand, srv, &uav, &c, GpuGroups(mw, 8), GpuGroups(mh, 8));
}

void CsCompose(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* native, ID3D12Resource* nr_in, ID3D12Resource* nr_out,
               UINT ww, UINT wh, ID3D12Resource* out, UINT w, UINT h, const ComposeParams& p)
{
    // ponytail: rects re-uploaded every frame (256 B copy, ~free); no change tracking.
    const int n = p.nrects < 0 ? 0 : p.nrects > 16 ? 16 : p.nrects;
    ID3D12Resource* up = s->rect_up[g.slot];
    void* mem = nullptr;
    if (SUCCEEDED(up->Map(0, nullptr, &mem)))
    {
        memset(mem, 0, kRectBytes);
        memcpy(mem, p.rects, (size_t)n * sizeof(UiRect));
        up->Unmap(0, nullptr);
    }
    GpuBarrier(cl, s->rect_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_SINT;
    src.PlacedFootprint.Footprint.Width = 64; src.PlacedFootprint.Footprint.Height = 1; src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = kRectBytes;
    dst.pResource = s->rect_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    GpuBarrier(cl, s->rect_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    struct { float strength; UINT wipe_mode; float wipe_x; int feather; UINT nrects, w, h, ww, wh; } c =
        { p.residual_strength, (UINT)p.wipe_mode, p.wipe_x, p.feather, (UINT)n, w, h, ww, wh };
    const GpuView srv[4] = { { native, DXGI_FORMAT_R8G8B8A8_UNORM }, { nr_in, DXGI_FORMAT_R8G8B8A8_UNORM },
                             { nr_out, DXGI_FORMAT_R8G8B8A8_UNORM }, { s->rect_tex, DXGI_FORMAT_R32_SINT } };
    const GpuView uav = { out, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->compose, srv, &uav, &c, GpuGroups(w, 8), GpuGroups(h, 8));
}
