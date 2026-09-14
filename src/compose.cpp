#include "compose.h"
#include "log.h"
#include <cstring>
#include "cs_swizzle.h"
#include "cs_gray.h"
#include "cs_downscale.h"
#include "cs_expand.h"
#include "cs_compose.h"
#include "cs_residual.h"
#include "cs_compose_residual.h"
#include <cstdlib>
#include <vector>

struct Shaders
{
    ComputePso swizzle, gray, downscale, expand, compose, residual, compose_residual;
    // UI rects for compose: 256x1 R32_SINT texture (64 rects x 4), refilled from a per-slot upload
    // buffer inside CsCompose (recorded into the caller's list; nothing blocks).
    ID3D12Resource* rect_tex = nullptr;
    ID3D12Resource* rect_up[Gpu::kFrames] = {};
};

static const UINT kMaxRects = 64, kRectInts = kMaxRects * 4, kRectBytes = kRectInts * 4;   // 1024 B, a multiple of the 256 B row pitch alignment

Shaders* ShadersCreate(Gpu& g)
{
    Shaders* s = new Shaders();
    bool ok = true;
    ok &= GpuMakeCompute(g, g_cs_swizzle,   sizeof g_cs_swizzle,   1, 1, 2,  s->swizzle,   L"cs_swizzle");
    ok &= GpuMakeCompute(g, g_cs_gray,      sizeof g_cs_gray,      1, 1, 6,  s->gray,      L"cs_gray");
    ok &= GpuMakeCompute(g, g_cs_downscale, sizeof g_cs_downscale, 1, 1, 4,  s->downscale, L"cs_downscale");
    ok &= GpuMakeCompute(g, g_cs_expand,    sizeof g_cs_expand,    2, 1, 11, s->expand,    L"cs_expand");
    ok &= GpuMakeCompute(g, g_cs_compose,   sizeof g_cs_compose,   4, 1, 11, s->compose,   L"cs_compose");
    ok &= GpuMakeCompute(g, g_cs_residual,  sizeof g_cs_residual,  2, 1, 2,  s->residual,  L"cs_residual");
    ok &= GpuMakeCompute(g, g_cs_compose_residual, sizeof g_cs_compose_residual, 4, 1, 12, s->compose_residual, L"cs_compose_residual");
    s->rect_tex = GpuMakeTex(g, kRectInts, 1, DXGI_FORMAT_R32_SINT, D3D12_RESOURCE_FLAG_NONE,
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
    ComputePso* p[] = { &s->swizzle, &s->gray, &s->downscale, &s->expand, &s->compose, &s->residual, &s->compose_residual };
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

// Refill rect_tex from this slot's upload buffer (recorded into cl). Returns the clamped rect count.
// Main-queue lists only (uses g.slot); both compose variants run there.
static UINT UploadRects(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, const ComposeParams& p)
{
    // ponytail: rects re-uploaded every frame (1 KB copy, ~free); no change tracking.
    const int n = p.nrects < 0 ? 0 : p.nrects > (int)kMaxRects ? (int)kMaxRects : p.nrects;
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
    src.PlacedFootprint.Footprint.Width = kRectInts; src.PlacedFootprint.Footprint.Height = 1; src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = kRectBytes;
    dst.pResource = s->rect_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    GpuBarrier(cl, s->rect_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return (UINT)n;
}

void CsCompose(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* native, ID3D12Resource* nr_in, ID3D12Resource* nr_out,
               UINT ww, UINT wh, ID3D12Resource* out, UINT w, UINT h, const ComposeParams& p)
{
    const UINT n = UploadRects(g, s, cl, p);
    struct { float strength; UINT wipe_mode; float wipe_x; int feather; UINT nrects, w, h, ww, wh, strip_w, strip_h; } c =
        { p.residual_strength, (UINT)p.wipe_mode, p.wipe_x, p.feather, n, w, h, ww, wh, (UINT)p.strip_w, (UINT)p.strip_h };
    const GpuView srv[4] = { { native, DXGI_FORMAT_R8G8B8A8_UNORM }, { nr_in, DXGI_FORMAT_R8G8B8A8_UNORM },
                             { nr_out, DXGI_FORMAT_R8G8B8A8_UNORM }, { s->rect_tex, DXGI_FORMAT_R32_SINT } };
    const GpuView uav = { out, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->compose, srv, &uav, &c, GpuGroups(w, 8), GpuGroups(h, 8));
}

// ---------------------------------------------------------------------------------------------
void CsResidual(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* nr_in, ID3D12Resource* nr_out, UINT ww, UINT wh,
                ID3D12Resource* dst_residual)
{
    const UINT c[2] = { ww, wh };
    const GpuView srv[2] = { { nr_in, DXGI_FORMAT_R8G8B8A8_UNORM }, { nr_out, DXGI_FORMAT_R8G8B8A8_UNORM } };
    const GpuView uav = { dst_residual, DXGI_FORMAT_R16G16B16A16_FLOAT };
    GpuDispatch(g, cl, s->residual, srv, &uav, c, GpuGroups(ww, 8), GpuGroups(wh, 8));
}

void CsComposeResidual(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* native, ID3D12Resource* residual, UINT ww, UINT wh,
                       ID3D12Resource* mv, ID3D12Resource* out, UINT w, UINT h, const ComposeParams& p)
{
    const UINT n = UploadRects(g, s, cl, p);
    struct { float strength; UINT wipe_mode; float wipe_x; int feather; UINT nrects, w, h, ww, wh; float warp; UINT strip_w, strip_h; } c =
        { p.residual_strength, (UINT)p.wipe_mode, p.wipe_x, p.feather, n, w, h, ww, wh, p.warp, (UINT)p.strip_w, (UINT)p.strip_h };
    const GpuView srv[4] = { { native, DXGI_FORMAT_R8G8B8A8_UNORM }, { residual, DXGI_FORMAT_R16G16B16A16_FLOAT },
                             { mv, DXGI_FORMAT_R16G16_FLOAT }, { s->rect_tex, DXGI_FORMAT_R32_SINT } };
    const GpuView uav = { out, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->compose_residual, srv, &uav, &c, GpuGroups(w, 8), GpuGroups(h, 8));
}

bool ComposeSelfTest(Gpu& g)
{
    const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_FLAGS RW = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const UINT w = 16, h = 16, ww = 8, wh = 8;
    Shaders* s = ShadersCreate(g);
    if (!s) return false;
    ID3D12Resource* native = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, RW, NPSR, L"st_native");
    ID3D12Resource* nr_in  = GpuMakeTex(g, ww, wh, DXGI_FORMAT_R8G8B8A8_UNORM, RW, NPSR, L"st_nr_in");
    ID3D12Resource* nr_out = GpuMakeTex(g, ww, wh, DXGI_FORMAT_R8G8B8A8_UNORM, RW, NPSR, L"st_nr_out");
    ID3D12Resource* mv     = GpuMakeTex(g, ww, wh, DXGI_FORMAT_R16G16_FLOAT, RW, NPSR, L"st_mv");
    ID3D12Resource* resid  = GpuMakeTex(g, ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, RW, UAV, L"st_residual");
    ID3D12Resource* out    = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, RW, UAV, L"st_out");
    bool ok = native && nr_in && nr_out && mv && resid && out;
    // native: gradient (read by index in the shader, so exact); nr_in/nr_out: constants -> residual (+30, -20, 0)
    std::vector<uint8_t> nat(w * h * 4), a(ww * wh * 4), b(ww * wh * 4), zero(ww * wh * 4, 0), got(w * h * 4);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
    { uint8_t* p = &nat[(y * w + x) * 4]; p[0] = (uint8_t)(x * 16); p[1] = (uint8_t)(y * 16); p[2] = 128; p[3] = 255; }
    for (UINT i = 0; i < ww * wh; ++i) { a[i * 4] = 50; a[i * 4 + 1] = 120; a[i * 4 + 2] = 200; a[i * 4 + 3] = 255;
                                         b[i * 4] = 80; b[i * 4 + 1] = 100; b[i * 4 + 2] = 200; b[i * 4 + 3] = 255; }
    ok = ok && GpuUploadTex(g, native, nat.data(), w, h, 4, NPSR) && GpuUploadTex(g, nr_in, a.data(), ww, wh, 4, NPSR) &&
         GpuUploadTex(g, nr_out, b.data(), ww, wh, 4, NPSR) && GpuUploadTex(g, mv, zero.data(), ww, wh, 4, NPSR);
    if (ok && GpuBegin(g))
    {
        ComposeParams cp;   // strength 1, no rects, no wipe, warp 1 (mv = 0)
        CsResidual(g, s, g.list, nr_in, nr_out, ww, wh, resid);
        GpuBarrier(g.list, resid, UAV, NPSR);
        CsComposeResidual(g, s, g.list, native, resid, ww, wh, mv, out, w, h, cp);
        const UINT64 v = GpuEnd(g);
        ok = v && GpuWait(g, g.fence, v, 5000) && GpuReadbackTex(g, out, got.data(), w, h, 4, UAV);
    }
    else ok = false;
    int bad = 0;
    if (ok)
        for (UINT i = 0; i < w * h; ++i)
        {
            const int d[3] = { 30, -20, 0 };
            for (int ch = 0; ch < 3; ++ch)
            {
                int e = (int)nat[i * 4 + ch] + d[ch]; e = e < 0 ? 0 : e > 255 ? 255 : e;
                if (abs(e - (int)got[i * 4 + ch]) > 1 && bad++ < 4)
                    Log("[cs] selftest px %u ch %d: expected %d got %d", i, ch, e, (int)got[i * 4 + ch]);
            }
        }
    ok = ok && bad == 0;
    Log("[cs] compose self-test %s", ok ? "PASS" : "FAIL");
    ID3D12Resource* rs[] = { native, nr_in, nr_out, mv, resid, out };
    for (ID3D12Resource* r : rs) if (r) r->Release();
    ShadersDestroy(s);
    return ok;
}
