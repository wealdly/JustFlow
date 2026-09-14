#include "ngx_nr.h"
#include "log.h"
#include "nvsdk_ngx.h"
#include <map>
#include <string>
#include <vector>

using PFN_NR_InitExt  = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using PFN_NR_Create   = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_NR_Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using PFN_NR_Release  = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// ---- P3: our own parameter block (no NGX core). A plain name->value map. --------------------
struct OwnParams : NVSDK_NGX_Parameter
{
    std::map<std::string, unsigned long long> u64; std::map<std::string, float> f32; std::map<std::string, double> f64;
    std::map<std::string, unsigned int> u32; std::map<std::string, int> i32; std::map<std::string, void*> ptr;
    void Set(const char* n, unsigned long long v) override { u64[n] = v; }
    void Set(const char* n, float v) override { f32[n] = v; }
    void Set(const char* n, double v) override { f64[n] = v; }
    void Set(const char* n, unsigned int v) override { u32[n] = v; }
    void Set(const char* n, int v) override { i32[n] = v; }
    void Set(const char* n, ID3D11Resource* v) override { ptr[n] = v; }
    void Set(const char* n, ID3D12Resource* v) override { ptr[n] = v; }
    void Set(const char* n, void* v) override { ptr[n] = v; }
    template <class M, class T> NVSDK_NGX_Result get(const M& m, const char* n, T* out) const
    { auto it = m.find(n); if (it == m.end()) return NVSDK_NGX_Result_FAIL_FeatureNotFound; *out = (T)it->second; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override { return get(u64, n, o); }
    NVSDK_NGX_Result Get(const char* n, float* o) const override { return get(f32, n, o); }
    NVSDK_NGX_Result Get(const char* n, double* o) const override { return get(f64, n, o); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override { return get(u32, n, o); }
    NVSDK_NGX_Result Get(const char* n, int* o) const override { return get(i32, n, o); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override { return get(ptr, n, (void**)o); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override { return get(ptr, n, (void**)o); }
    NVSDK_NGX_Result Get(const char* n, void** o) const override { return get(ptr, n, o); }
    void Reset() override { u64.clear(); f32.clear(); f64.clear(); u32.clear(); i32.clear(); ptr.clear(); }
};

struct Retired { NVSDK_NGX_Handle* h; int evals_left; };

struct Nr
{
    Gpu* g = nullptr;
    std::wstring dir;
    HMODULE fwd = nullptr;
    PFN_NR_InitExt init_ext = nullptr; PFN_NR_Create create = nullptr; PFN_NR_Evaluate evaluate = nullptr; PFN_NR_Release release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    OwnParams own;
    NrParamBlock block = NrBlockAllocate;
    bool core_inited = false;
    int float_slot = -1;          // -2 = typed Set(float) works, >=0 = vtable slot, -1 = unknown
    NVSDK_NGX_Handle* feature = nullptr;
    bool submitted = false;
    NrConfig live;
    std::vector<Retired> retired;
    std::string last_error;
    NVSDK_NGX_FeatureCommonInfo common = {};
};

static void NVSDK_CONV Discard(const char*, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {}

// ---- parameter writes: typed first, vtable slot when the block's typed float setter is a no-op --
// The driver's capability block does not lay out its setters the way the header declares them
// (fork finding: float setter answers on slot 6). Getters mirror setters +8. Calling convention
// is __thiscall with the block as `this`.
typedef void(__thiscall* PFN_SetF)(void*, const char*, float);
typedef void(__thiscall* PFN_SetU)(void*, const char*, unsigned int);
typedef void(__thiscall* PFN_SetULL)(void*, const char*, unsigned long long);

static void SetF(Nr* n, const char* name, float v)
{
    if (n->float_slot >= 0) { void** vt = *(void***)n->params; ((PFN_SetF)vt[n->float_slot])(n->params, name, v); }
    else n->params->Set(name, v);
}
static void SetU(Nr* n, const char* name, unsigned int v) { n->params->Set(name, v); }
static void SetRes(Nr* n, const char* name, ID3D12Resource* r)
{
    // resources go through the 64-bit setter (slot 0) as a ULL; the typed ID3D12Resource* setter
    // left them unset on the capability block (fork finding).
    if (n->block == NrBlockCapability) { void** vt = *(void***)n->params; ((PFN_SetULL)vt[0])(n->params, name, (unsigned long long)r); }
    else n->params->Set(name, r);
}

static void ProbeFloatSlot(Nr* n)
{
    const float expected = 0.3125f; float got = -1.0f;
    n->params->Set("NRF.FloatProbe", expected);
    if (n->params->Get("NRF.FloatProbe", &got) == NVSDK_NGX_Result_Success && got == expected) { n->float_slot = -2; Log("[ngx] float setter: typed Set works"); return; }
    static const int cand[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    void** vt = *(void***)n->params;
    for (int slot : cand)
    {
        const float probe = 0.375f; got = -1.0f;
        __try { ((PFN_SetF)vt[slot])(n->params, "NRF.FloatProbe2", probe); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (n->params->Get("NRF.FloatProbe2", &got) == NVSDK_NGX_Result_Success && got == probe) { n->float_slot = slot; Log("[ngx] float setter: vtable slot %d", slot); return; }
    }
    Log("[ngx] float setter: NOT FOUND (floats may not stick)");
}

Nr* NrInit(Gpu& g, const wchar_t* dir, NrParamBlock block)
{
    Nr* n = new Nr; n->g = &g; n->dir = dir; n->block = block;
    std::wstring fwd = n->dir + L"\\nvngx.dll_nrfilter.dll", rt = n->dir + L"\\nvngx_dlssnr.dll";
    n->fwd = LoadLibraryW(fwd.c_str());
    if (!n->fwd) { Log("[ngx] forwarder %ls did not load (err %lu)", fwd.c_str(), GetLastError()); delete n; return nullptr; }
    auto load = (int(*)(const wchar_t*))GetProcAddress(n->fwd, "NrfFwdLoad");
    auto where = (void(*)(wchar_t*, unsigned))GetProcAddress(n->fwd, "NrfFwdPath");
    n->init_ext = (PFN_NR_InitExt)GetProcAddress(n->fwd, "NrfFwdInitExt");
    n->create = (PFN_NR_Create)GetProcAddress(n->fwd, "NrfFwdCreate");
    n->evaluate = (PFN_NR_Evaluate)GetProcAddress(n->fwd, "NrfFwdEvaluate");
    n->release = (PFN_NR_Release)GetProcAddress(n->fwd, "NrfFwdRelease");
    if (!load || !n->init_ext || !n->create || !n->evaluate || !n->release) { Log("[ngx] forwarder exports missing"); delete n; return nullptr; }
    const int lr = load(rt.c_str());
    if (lr != 0) { Log("[ngx] forwarder could not load %ls (%d)", rt.c_str(), lr); delete n; return nullptr; }
    wchar_t actual[MAX_PATH] = {}; if (where) where(actual, MAX_PATH);
    Log("[ngx] calls go through %ls", actual);

    n->common.LoggingInfo.LoggingCallback = Discard;
    n->common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    n->common.LoggingInfo.DisableOtherLoggingSinks = true;
    if (block == NrBlockOwn) { n->params = &n->own; Log("[ngx] parameter block: own (no NGX core)"); }
    else
    {
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, dir, g.dev, &n->common, NVSDK_NGX_Version_API);
        Log("[ngx] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
        if (NVSDK_NGX_FAILED(r)) { delete n; return nullptr; }
        n->core_inited = true;
        r = block == NrBlockAllocate ? NVSDK_NGX_D3D12_AllocateParameters(&n->params) : NVSDK_NGX_D3D12_GetCapabilityParameters(&n->params);
        Log("[ngx] parameter block: %s -> 0x%08X", block == NrBlockAllocate ? "AllocateParameters" : "GetCapabilityParameters", r);
        if (NVSDK_NGX_FAILED(r) || !n->params) { delete n; return nullptr; }
    }
    ProbeFloatSlot(n);
    const NVSDK_NGX_Result r = n->init_ext(0x1000000ULL, dir, g.dev, NVSDK_NGX_Version_API, n->params);
    Log("[ngx] Init_Ext -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r)) { n->last_error = NgxResultName(r); delete n; return nullptr; }
    return n;
}

void NrShutdown(Nr* n)
{
    if (!n) return;
    for (auto& r : n->retired) n->release(r.h);
    if (n->feature) n->release(n->feature);
    if (n->core_inited && n->block != NrBlockOwn) { NVSDK_NGX_D3D12_DestroyParameters(n->params); NVSDK_NGX_D3D12_Shutdown1(n->g->dev); }
    if (n->fwd) FreeLibrary(n->fwd);
    delete n;
}

int NrFloatSlot(const Nr* n) { return n->float_slot; }
bool NrReady(const Nr* n) { return n->feature && n->submitted; }
void NrMarkSubmitted(Nr* n) { n->submitted = true; }
const NrConfig& NrLiveConfig(const Nr* n) { return n->live; }
const char* NrLastError(const Nr* n) { return n->last_error.c_str(); }

static void SetTuning(Nr* n, const NrTuning& t)
{
    SetU(n, "DLSSNR.Hint.Render.Preset", (unsigned)t.preset);
    SetF(n, "DLSSNR.Intensity", t.intensity);
    SetU(n, "DLSSNR.Style", (unsigned)t.style);
    SetF(n, "DLSSNR.LocalStructureStrength", t.local_structure);
    SetF(n, "DLSSNR.LocalToneStrength", t.local_tone);
    SetF(n, "DLSSNR.SkinStructureStrength", t.skin_structure);
    n->params->Set("DLSSNR.ControlMask", (void*)nullptr);
    SetU(n, "DLSSNR.UseAutoMask", t.auto_mask ? 1u : 0u);
    SetU(n, "DLSSNR.UICorrection", t.ui_correction ? 1u : 0u);
}

bool NrCreate(Nr* n, ID3D12GraphicsCommandList* cl, const NrConfig& cfg)
{
    if (n->feature) { n->retired.push_back({ n->feature, 32 }); n->feature = nullptr; n->submitted = false; }
    n->params->Reset();
    SetU(n, "CreationNodeMask", 1u); SetU(n, "VisibilityNodeMask", 1u);
    SetU(n, "DLSSNR.Enabled", 1u);
    SetU(n, "DLSSNR.Width", cfg.work_w); SetU(n, "DLSSNR.Height", cfg.work_h);
    if (cfg.create_style == NrCreateA)
    {
        SetU(n, "DLSSNR.InputWidth", cfg.work_w); SetU(n, "DLSSNR.InputHeight", cfg.work_h);
        SetU(n, "DLSSNR.OutputWidth", cfg.work_w); SetU(n, "DLSSNR.OutputHeight", cfg.work_h);
        SetU(n, "DLSSNR.Output.Width", cfg.work_w); SetU(n, "DLSSNR.Output.Height", cfg.work_h);
        SetU(n, "DLSSNR.Upscaling", 0u);
        SetF(n, "DLSSNR.Scale", 1.0f); SetF(n, "DLSSNR.ScalingRatio", 1.0f);
        SetU(n, "DLSS.Feature.Create.Flags", 0u);
    }
    SetTuning(n, cfg.tuning);
    NVSDK_NGX_Handle* h = nullptr; DWORD code = 0;
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = n->create(cl, NVSDK_NGX_Feature_Reserved18, n->params, &h); } __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); }
    if (code) { n->last_error = "CreateFeature raised an exception"; Log("[ngx] CreateFeature raised 0x%08X", code); return false; }
    if (NVSDK_NGX_FAILED(r) || !h) { n->last_error = NgxResultName(r); Log("[ngx] CreateFeature(18) %ux%u style %c -> 0x%08X (%s)", cfg.work_w, cfg.work_h, cfg.create_style == NrCreateA ? 'A' : 'B', r, NgxResultName(r)); return false; }
    n->feature = h; n->live = cfg; n->submitted = false;
    Log("[ngx] feature 18 created %ux%u style %c preset %d (submit before evaluating)", cfg.work_w, cfg.work_h, cfg.create_style == NrCreateA ? 'A' : 'B', cfg.tuning.preset);
    return true;
}

unsigned NrEvaluate(Nr* n, ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* output, bool reset, float exposure_scale)
{
    if (!n->feature) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const UINT w = n->live.work_w, h = n->live.work_h;
    n->params->Reset();
    SetRes(n, "DLSSNR.Color", color); SetRes(n, "DLSSNR.Output", output); SetRes(n, "DLSSNR.MVec", mv);
    SetU(n, "DLSSNR.ColorSubrectBaseX", 0u); SetU(n, "DLSSNR.ColorSubrectBaseY", 0u);
    SetU(n, "DLSSNR.ColorSubrectWidth", w); SetU(n, "DLSSNR.ColorSubrectHeight", h);
    SetU(n, "DLSSNR.MVecSubrectBaseX", 0u); SetU(n, "DLSSNR.MVecSubrectBaseY", 0u);
    SetU(n, "DLSSNR.MVecSubrectWidth", w); SetU(n, "DLSSNR.MVecSubrectHeight", h);
    SetU(n, "DLSSNR.OutputSubrectBaseX", 0u); SetU(n, "DLSSNR.OutputSubrectBaseY", 0u);
    SetU(n, "DLSSNR.OutputSubrectWidth", w); SetU(n, "DLSSNR.OutputSubrectHeight", h);
    SetF(n, "DLSSNR.MVecScaleX", 1.0f); SetF(n, "DLSSNR.MVecScaleY", 1.0f);
    SetU(n, "DLSSNR.Enabled", 1u); SetU(n, "DLSSNR.Reset", reset ? 1u : 0u);
    SetU(n, "DLSSNR.Width", w); SetU(n, "DLSSNR.Height", h);
    SetTuning(n, n->live.tuning);
    SetF(n, "DLSS.Pre.Exposure", 1.0f); SetF(n, "DLSS.Exposure.Scale", exposure_scale);
    DWORD code = 0; NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = n->evaluate(cl, n->feature, n->params, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); }
    if (code) { n->last_error = "EvaluateFeature raised an exception"; Log("[ngx] evaluate raised 0x%08X", code); n->g->failed = true; return 0xBAD00000; }
    if (NVSDK_NGX_FAILED(r)) { n->last_error = NgxResultName(r); }
    return (unsigned)r;
}

void NrRetireTick(Nr* n)
{
    for (size_t i = 0; i < n->retired.size();)
    {
        if (--n->retired[i].evals_left <= 0) { n->release(n->retired[i].h); n->retired.erase(n->retired.begin() + i); Log("[ngx] retired feature released"); }
        else ++i;
    }
}
