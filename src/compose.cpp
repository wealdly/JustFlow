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
#include "cs_text.h"
#include "cs_sharpen.h"
#include <algorithm>
#include <cstdlib>
#include <vector>

struct Shaders
{
    ComputePso swizzle, gray, downscale, expand, compose, residual, compose_residual, text, sharpen, artcnn;
    ID3D12Resource* feat[3] = {};         // CsArtCnn feature maps (2w x 2h RGBA16F, UAV at rest), sized feat_w x feat_h
    UINT feat_w = 0, feat_h = 0;
    // UI rects for compose: 256x1 R32_SINT texture (64 rects x 4), refilled from a per-slot upload
    // buffer inside CsCompose (recorded into the caller's list; nothing blocks).
    ID3D12Resource* rect_tex = nullptr;
    ID3D12Resource* rect_up[Gpu::kFrames] = {};
    ID3D12Resource* font_tex = nullptr;   // 95 x 8 R8_UINT, one row byte per texel (CsText)
};

static const UINT kMaxRects = 64, kRectInts = kMaxRects * 4, kRectBytes = kRectInts * 4;   // 1024 B, a multiple of the 256 B row pitch alignment

// 8x8 bitmap font, ASCII 32..126 (font8x8_basic, Daniel Hepper, public domain): 8 row bytes per
// glyph, bit i of a row = column i lit. Baked into font_tex as texel (glyph, row) = the row byte.
static const uint8_t kFont8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};
static_assert(sizeof kFont8x8 == 760, "95 glyphs x 8 rows");

// Read a file that sits next to the executable. Used for shader blobs kept out of the binary.
static bool LoadBlobNextToExe(const wchar_t* name, std::vector<uint8_t>& out)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* sl = wcsrchr(path, L'\\')) *(sl + 1) = 0;
    wcsncat_s(path, name, _TRUNCATE);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

Shaders* ShadersCreate(Gpu& g)
{
    Shaders* s = new Shaders();
    bool ok = true;
    ok &= GpuMakeCompute(g, g_cs_swizzle,   sizeof g_cs_swizzle,   1, 1, 2,  s->swizzle,   L"cs_swizzle");
    ok &= GpuMakeCompute(g, g_cs_gray,      sizeof g_cs_gray,      1, 1, 6,  s->gray,      L"cs_gray");
    ok &= GpuMakeCompute(g, g_cs_downscale, sizeof g_cs_downscale, 1, 1, 4,  s->downscale, L"cs_downscale");
    ok &= GpuMakeCompute(g, g_cs_expand,    sizeof g_cs_expand,    1, 1,  9, s->expand,    L"cs_expand");
    ok &= GpuMakeCompute(g, g_cs_compose,   sizeof g_cs_compose,   4, 1, 11, s->compose,   L"cs_compose");
    ok &= GpuMakeCompute(g, g_cs_residual,  sizeof g_cs_residual,  2, 1, 2,  s->residual,  L"cs_residual");
    ok &= GpuMakeCompute(g, g_cs_compose_residual, sizeof g_cs_compose_residual, 4, 1, 12, s->compose_residual, L"cs_compose_residual");
    ok &= GpuMakeCompute(g, g_cs_text,      sizeof g_cs_text,      1, 1, 24, s->text,      L"cs_text");
    ok &= GpuMakeCompute(g, g_cs_sharpen,   sizeof g_cs_sharpen,   2, 1, 4,  s->sharpen,   L"cs_sharpen");
    // artcnn.cso sits next to the exe (see CMakeLists): a quarter-megabyte of generated bytecode in
    // the binary tripped Smart App Control. Missing file = the pass stays unavailable, not a failure.
    {
        std::vector<uint8_t> cso;
        if (LoadBlobNextToExe(L"artcnn.cso", cso))
            ok &= GpuMakeCompute(g, cso.data(), cso.size(), 3, 1, 3, s->artcnn, L"cs_artcnn");
        else
            Log("[cs] artcnn.cso not found next to the exe - the ArtCNN pass is unavailable");
    }
    s->rect_tex = GpuMakeTex(g, kRectInts, 1, DXGI_FORMAT_R32_SINT, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"ui_rects");
    ok &= s->rect_tex != nullptr;
    // font: texel (glyph, row) = row byte -> upload the table transposed (row-major 95 x 8)
    s->font_tex = GpuMakeTex(g, 95, 8, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"font8x8");
    uint8_t font[8][95];
    for (int gl = 0; gl < 95; ++gl) for (int r = 0; r < 8; ++r) font[r][gl] = kFont8x8[gl][r];
    ok &= s->font_tex && GpuUploadTex(g, s->font_tex, font, 95, 8, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
    ComputePso* p[] = { &s->swizzle, &s->gray, &s->downscale, &s->expand, &s->compose, &s->residual, &s->compose_residual, &s->text, &s->sharpen, &s->artcnn };
    for (ComputePso* x : p) { if (x->pso) x->pso->Release(); if (x->root) x->root->Release(); }
    for (ID3D12Resource* r : s->feat) if (r) r->Release();
    if (s->rect_tex) s->rect_tex->Release();
    if (s->font_tex) s->font_tex->Release();
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

void CsExpand(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* flow, UINT fw, UINT fh, UINT ofa_w, UINT ofa_h,
              ID3D12Resource* dst_mv, UINT mw, UINT mh, float zero_below, bool reset)
{
    struct { UINT fw, fh, ofa_w, ofa_h, mw, mh, grid; float zero_below; UINT reset; } c =
        { fw, fh, ofa_w, ofa_h, mw, mh, (ofa_w + fw - 1) / fw, zero_below, reset ? 1u : 0u };
    const GpuView srv = { flow, DXGI_FORMAT_R16G16_SINT };
    const GpuView uav = { dst_mv, DXGI_FORMAT_R16G16_FLOAT };
    GpuDispatch(g, cl, s->expand, &srv, &uav, &c, GpuGroups(mw, 8), GpuGroups(mh, 8));
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

// ---------------------------------------------------------------------------------------------
void CsText(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT w, UINT h, const char* text,
            int x, int y, int scale, float alpha, int box_pad)
{
    struct { int x0, y0; UINT scale; float alpha; int pad; UINT len, w, h; UINT chars[16]; } c = {};
    const size_t n = std::min(strlen(text), kMaxText);
    if (!n) return;
    memcpy(c.chars, text, n);   // 4 chars per DWORD, little-endian = char i at byte i
    scale = std::max(1, scale); box_pad = std::max(0, box_pad);
    const int bw = TextBoxW(n, scale, box_pad), bh = TextBoxH(scale, box_pad);
    c.x0 = x < 0 ? ((int)w - bw) / 2 : x; c.y0 = y < 0 ? 48 : y;
    c.scale = (UINT)scale; c.alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha; c.pad = box_pad; c.len = (UINT)n; c.w = w; c.h = h;
    const GpuView srv = { s->font_tex, DXGI_FORMAT_R8_UINT }, uav = { dst, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->text, &srv, &uav, &c, GpuGroups((UINT)bw, 8), GpuGroups((UINT)bh, 8));
}

void CsSharpen(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h,
               float strength, UINT nrects)
{
    struct { UINT w, h; float strength; UINT nrects; } c = { w, h, strength, std::min(nrects, kMaxRects) };
    const GpuView srv[2] = { { src, DXGI_FORMAT_R8G8B8A8_UNORM }, { s->rect_tex, DXGI_FORMAT_R32_SINT } };
    const GpuView uav = { dst, DXGI_FORMAT_R8G8B8A8_UNORM };
    GpuDispatch(g, cl, s->sharpen, srv, &uav, &c, GpuGroups(w, 8), GpuGroups(h, 8));
}

// ---------------------------------------------------------------------------------------------
void CsArtCnn(Gpu& g, Shaders* s, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h)
{
    const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s->feat_w != w || s->feat_h != h)   // ponytail: one size at a time; the pipeline idles before a work-size change
    {
        for (ID3D12Resource*& t : s->feat)
        {
            if (t) t->Release();
            t = GpuMakeTex(g, 2 * w, 2 * h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, UAV, L"artcnn_feat");
            if (!t) { Log("[cs] artcnn feature texture %ux%u failed", 2 * w, 2 * h); s->feat_w = s->feat_h = 0; return; }
        }
        s->feat_w = w; s->feat_h = h;
    }
    ID3D12Resource *A = s->feat[0], *B = s->feat[1], *C = s->feat[2];
    // views take each resource's own format (unused SRV slots are bound to src, which is NPSR throughout)
    auto pass = [&](UINT layer, ID3D12Resource* in, ID3D12Resource* skip, ID3D12Resource* out)
    {
        const UINT c[3] = { w, h, layer };
        const GpuView srv[3] = { { in, DXGI_FORMAT_UNKNOWN }, { skip, DXGI_FORMAT_UNKNOWN }, { src, DXGI_FORMAT_UNKNOWN } };
        const GpuView uav = { out, DXGI_FORMAT_UNKNOWN };
        GpuDispatch(g, cl, s->artcnn, srv, &uav, c, GpuGroups(w, 8), GpuGroups(h, 8));
    };
    pass(0, src, src, A); GpuBarrier(cl, A, UAV, NPSR);
    pass(1, A, src, B);   GpuBarrier(cl, B, UAV, NPSR);
    pass(2, B, src, C);   GpuBarrier(cl, C, UAV, NPSR); GpuBarrier(cl, B, NPSR, UAV);
    pass(3, C, src, B);   GpuBarrier(cl, B, UAV, NPSR); GpuBarrier(cl, C, NPSR, UAV);
    pass(4, B, src, C);   GpuBarrier(cl, C, UAV, NPSR); GpuBarrier(cl, B, NPSR, UAV);
    pass(5, C, src, B);   GpuBarrier(cl, B, UAV, NPSR); GpuBarrier(cl, C, NPSR, UAV);
    pass(6, B, A, dst);   GpuBarrier(cl, B, NPSR, UAV); GpuBarrier(cl, A, NPSR, UAV);
}

bool ArtCnnSelfTest(Gpu& g)
{
    const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const UINT w = 64, h = 64;
    Shaders* s = ShadersCreate(g);
    if (!s) return false;
    ID3D12Resource* src = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, NPSR, L"st_artcnn_src");
    ID3D12Resource* dst = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, UAV, L"st_artcnn_dst");
    bool ok = src && dst;
    // run(image) -> max |dst - src| over RGB. flat: 128 everywhere; edge: 64 | 192 split down the middle
    std::vector<uint8_t> px((size_t)w * h * 4), got((size_t)w * h * 4);
    auto run = [&](bool edge, int& max_delta) -> bool
    {
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
        { uint8_t* p = &px[((size_t)y * w + x) * 4]; p[0] = p[1] = p[2] = (uint8_t)(edge ? (x < w / 2 ? 64 : 192) : 128); p[3] = 255; }
        if (!GpuUploadTex(g, src, px.data(), w, h, 4, NPSR) || !GpuBegin(g)) return false;
        CsArtCnn(g, s, g.list, src, dst, w, h);
        const UINT64 v = GpuEnd(g);
        if (!v || !GpuWait(g, g.fence, v, 5000) || !GpuReadbackTex(g, dst, got.data(), w, h, 4, UAV)) return false;
        max_delta = 0;
        for (size_t i = 0; i < px.size(); ++i) if (i % 4 != 3) max_delta = std::max(max_delta, abs((int)px[i] - (int)got[i]));
        return true;
    };
    int flat = -1, edge = -1;
    ok = ok && run(false, flat) && run(true, edge);
    ok = ok && flat <= 2 && edge > 0;
    Log("[cs] artcnn self-test %s (flat grey max delta %d/255, step edge max delta %d/255)", ok ? "PASS" : "FAIL", flat, edge);
    if (src) src->Release(); if (dst) dst->Release();
    ShadersDestroy(s);
    return ok;
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
