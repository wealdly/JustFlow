#include "latewarp.h"
#include "log.h"
#include "ngx_nr.h"          // NgxMutex, NgxResultName
#include "nvsdk_ngx.h"
#include <cmath>
#include <string>
#include <vector>

struct Latewarp
{
    Gpu* g = nullptr;
    UINT w = 0, h = 0, dw = 0, dh = 0;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    ID3D12Resource *depth = nullptr, *mv = nullptr, *no_ui = nullptr;
    unsigned long long frame_id = 0;
    std::wstring dir; const wchar_t* path_list[1] = {};
    NVSDK_NGX_FeatureCommonInfo common = {};
};

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

static NVSDK_NGX_Result SafeEvaluate(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Handle* h, NVSDK_NGX_Parameter* p, DWORD* code)
{
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = NVSDK_NGX_D3D12_EvaluateFeature(cl, h, p, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); }
    return r;
}

void LatewarpDestroy(Latewarp* l)
{
    if (!l) return;
    if (l->feature) { std::lock_guard<std::mutex> lk(NgxMutex()); NVSDK_NGX_D3D12_ReleaseFeature(l->feature); }
    if (l->params) NVSDK_NGX_D3D12_DestroyParameters(l->params);
    for (ID3D12Resource* r : { l->depth, l->mv, l->no_ui }) if (r) r->Release();
    delete l;
}

Latewarp* LatewarpCreate(Gpu& g, const wchar_t* dir, UINT w, UINT h, UINT dw, UINT dh)
{
    Latewarp* l = new Latewarp;
    l->g = &g; l->w = w; l->h = h; l->dw = dw; l->dh = dh; l->dir = dir;
    auto fail = [&](const char* why) { Log("[latewarp] disabled: %s", why); LatewarpDestroy(l); return (Latewarp*)nullptr; };
    l->path_list[0] = l->dir.c_str();
    l->common.PathListInfo.Path = l->path_list; l->common.PathListInfo.Length = 1;
    l->common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, dir, g.dev, &l->common, NVSDK_NGX_Version_API);   // harmless if NR/FG already did it
    if (NVSDK_NGX_FAILED(r)) { Log("[latewarp] NGX init -> 0x%08X (%s)", r, NgxResultName(r)); return fail("NGX init"); }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&l->params)) || !l->params) return fail("no parameter block");

    // flat depth, zero motion, no UI: what a capture can say about a frame it did not render
    l->depth = GpuMakeTex(g, dw, dh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"lw_depth");
    l->mv = GpuMakeTex(g, dw, dh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"lw_mv");
    l->no_ui = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, NPSR, L"lw_no_ui");
    // Far, not mid: Frame Warp leaves pixels close to the camera unwarped (a first-person weapon), and
    // 0.5 NDC with near 0.1 is 0.2 units away - it held 50-70% of the frame still. 0.9995 is ~170 units.
    const std::vector<float> flat((size_t)dw * dh, 0.9995f);
    const std::vector<uint32_t> zero_mv((size_t)dw * dh, 0), zero_ui((size_t)w * h, 0);
    if (!l->depth || !l->mv || !l->no_ui || !GpuUploadTex(g, l->depth, flat.data(), dw, dh, 4, NPSR) ||
        !GpuUploadTex(g, l->mv, zero_mv.data(), dw, dh, 4, NPSR) || !GpuUploadTex(g, l->no_ui, zero_ui.data(), w, h, 4, NPSR))
        return fail("guide textures");

    if (!GpuBegin(g)) return fail("create list");
    l->params->Set("Latewarp.Output.Width", w); l->params->Set("Latewarp.Output.Height", h);
    { std::lock_guard<std::mutex> lk(NgxMutex()); r = NVSDK_NGX_D3D12_CreateFeature(g.list, NVSDK_NGX_Feature_Reserved15, l->params, &l->feature); }
    const UINT64 v = GpuEnd(g);
    if (NVSDK_NGX_FAILED(r) || !l->feature) { Log("[latewarp] CreateFeature -> 0x%08X (%s)", r, NgxResultName(r)); return fail("CreateFeature (is nvngx_latewarp.dll next to the exe?)"); }
    if (!v || !GpuWaitIdle(g)) return fail("create submit");
    Log("[latewarp] feature created %ux%u (guides %ux%u)", w, h, dw, dh);
    return l;
}

// Row-major, row-vector (D3D / UE convention): v_view = v_world * View, v_clip = v_view * Proj.
static void Perspective(float m[16], float vfov, float aspect)
{
    const float f = 1.0f / tanf(vfov * 0.5f), n = 0.1f, fa = 1000.0f, q = fa / (fa - n);
    const float p[16] = { f / aspect, 0, 0, 0,   0, f, 0, 0,   0, 0, q, 1,   0, 0, -n * q, 0 };
    memcpy(m, p, sizeof p);
}
// The target camera turned by yaw (about up), pitch (about right), roll (about forward) relative to
// the camera that rendered the frame, which is the identity.
static void TurnedView(float m[16], float yaw, float pitch, float roll)
{
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    // camera orientation C = Ry(yaw) * Rx(pitch) * Rz(roll) (column vectors, left-handed, +Z forward);
    // world-to-view is C^-1 = C^T for column vectors, which in row-vector form is C itself.
    const float c[9] = {
        cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
        cp * sr,                cp * cr,                 -sp,
        -sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp };
    const float v[16] = { c[0], c[1], c[2], 0,   c[3], c[4], c[5], 0,   c[6], c[7], c[8], 0,   0, 0, 0, 1 };
    memcpy(m, v, sizeof v);
}

bool LatewarpEvaluate(Latewarp* l, ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* ui, ID3D12Resource* out,
                      bool rendered, float yaw, float pitch, float roll, float vfov)
{
    static float proj[16], src[16], dst[16];   // the runtime reads these while recording: keep them alive
    Perspective(proj, vfov, (float)l->w / (float)l->h);
    TurnedView(src, 0, 0, 0); TurnedView(dst, yaw, pitch, roll);
    NVSDK_NGX_Parameter* p = l->params;
    p->Set("Latewarp.Backbuffer", color); p->Set("Latewarp.HudlessColor", color);
    p->Set("Latewarp.UIColorAlpha", ui ? ui : l->no_ui);
    p->Set("Depth", l->depth); p->Set("MotionVectors", l->mv); p->Set("Output", out);
    p->Set("Latewarp.NoWarpMask", (ID3D12Resource*)nullptr);
    auto rect = [&](const char* k, UINT rw, UINT rh)
    {
        const std::string b = k;
        p->Set((b + ".Subrect.Base.X").c_str(), 0u); p->Set((b + ".Subrect.Base.Y").c_str(), 0u);
        p->Set((b + ".Subrect.Width").c_str(), rw); p->Set((b + ".Subrect.Height").c_str(), rh);
    };
    rect("Latewarp.Backbuffer", l->w, l->h); rect("Latewarp.HudlessColor", l->w, l->h); rect("Latewarp.UIColorAlpha", l->w, l->h);
    rect("Latewarp.Output", l->w, l->h); rect("Latewarp.Depth", l->dw, l->dh); rect("Latewarp.MV", l->dw, l->dh);
    p->Set("Latewarp.WorldToViewMatrix", (void*)dst); p->Set("Latewarp.ViewToClipMatrix", (void*)proj);
    p->Set("Latewarp.PrevRenderedWorldToViewMatrix", (void*)src); p->Set("Latewarp.PrevRenderedViewToClipMatrix", (void*)proj);
    p->Set("Latewarp.DepthInverted", 0u); p->Set("Latewarp.EvalFlags", 0u); p->Set("Latewarp.UsePremultiplyUIAlpha", 0u);
    DWORD code = 0; NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    std::lock_guard<std::mutex> lk(NgxMutex());
    // A rendered-frame evaluate registers the frame and ignores the target camera, so a new frame
    // takes two: register, then warp.
    if (rendered)
    {
        p->Set("Latewarp.IsRenderedFrame", 1u); p->Set("Latewarp.FrameID", ++l->frame_id);
        r = SafeEvaluate(cl, l->feature, p, &code);
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = out; cl->ResourceBarrier(1, &b);
    }
    if (!code && !NVSDK_NGX_FAILED(r))
    {
        p->Set("Latewarp.IsRenderedFrame", 0u); p->Set("Latewarp.FrameID", ++l->frame_id);
        r = SafeEvaluate(cl, l->feature, p, &code);
    }
    if (code || NVSDK_NGX_FAILED(r)) { Log("[latewarp] evaluate -> 0x%08X (%s) code 0x%08X", r, NgxResultName(r), code); return false; }
    return true;
}
