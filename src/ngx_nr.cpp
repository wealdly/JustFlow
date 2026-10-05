#include "ngx_nr.h"
#include "log.h"
#include "nvsdk_ngx.h"
#include <algorithm>
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
    HMODULE fwd = nullptr;
    PFN_NR_InitExt init_ext = nullptr; PFN_NR_Create create = nullptr; PFN_NR_Evaluate evaluate = nullptr; PFN_NR_Release release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    OwnParams own;
    NrParamBlock block = NrBlockAllocate;
    int float_slot = -1;          // -2 = typed Set(float) works, >=0 = vtable slot, -1 = unknown
    static const int kMaxPasses = 3;
    NVSDK_NGX_Handle* pass[kMaxPasses] = {};      // one feature per model pass; pass[0] exists whenever any does
    ID3D12Resource*   mid[kMaxPasses - 1] = {};   // mid[k]: pass k's output, pass k+1's input (RGBA8 work size, UAV at rest)
    bool submitted = false;
    NrConfig live;
    std::vector<Retired> retired;
    std::string last_error;
    NVSDK_NGX_FeatureCommonInfo common = {};
};

static void NVSDK_CONV Discard(const char*, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {}

// ---- parameter writes: typed first, vtable slot when the block's typed float setter is a no-op --
// The driver's capability block does not lay out its setters the way the header declares them
// (its float setter answers on slot 6). __thiscall with the block as `this`.
typedef void(__thiscall* PFN_SetF)(void*, const char*, float);
typedef void(__thiscall* PFN_SetULL)(void*, const char*, unsigned long long);

static void SetF(Nr* n, const char* name, float v)
{
    if (n->float_slot >= 0) { void** vt = *(void***)n->params; ((PFN_SetF)vt[n->float_slot])(n->params, name, v); }
    else n->params->Set(name, v);
}
static void SetU(Nr* n, const char* name, unsigned int v) { n->params->Set(name, v); }
static void SetRes(Nr* n, const char* name, ID3D12Resource* r)
{
    // resources go through the 64-bit setter (slot 0) as a ULL: the typed ID3D12Resource* setter
    // leaves them unset on the capability block.
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

NVSDK_NGX_Parameter* NgxCoreParams(Gpu& g, const wchar_t* dir, const char* tag)
{
    // NGX is handed pointers into these: they live for the process, like the core itself.
    static std::wstring path; static const wchar_t* list[1]; static NVSDK_NGX_FeatureCommonInfo common = {};
    if (path.empty()) path = dir;
    list[0] = path.c_str(); common.PathListInfo.Path = list; common.PathListInfo.Length = 1;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, dir, g.dev, &common, NVSDK_NGX_Version_API);
    Log("%s NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", tag, r, NgxResultName(r));
    NVSDK_NGX_Parameter* p = nullptr;
    r = NVSDK_NGX_D3D12_AllocateParameters(&p);
    if (NVSDK_NGX_FAILED(r) || !p) { Log("%s AllocateParameters -> 0x%08X (%s)", tag, r, NgxResultName(r)); return nullptr; }
    return p;
}

Nr* NrInit(Gpu& g, const wchar_t* dir, NrParamBlock block)
{
    Nr* n = new Nr; n->g = &g; n->block = block;
    const std::wstring fwd = std::wstring(dir) + L"\\nvngx.dll_justflow.dll", rt = std::wstring(dir) + L"\\nvngx_dlssnr.dll";
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
    for (auto* h : n->pass) if (h) n->release(h);
    for (auto* t : n->mid) if (t) t->Release();
    if (n->block != NrBlockOwn) { NVSDK_NGX_D3D12_DestroyParameters(n->params); NVSDK_NGX_D3D12_Shutdown1(n->g->dev); }
    if (n->fwd) FreeLibrary(n->fwd);
    delete n;
}

int NrFloatSlot(const Nr* n) { return n->float_slot; }
bool NrReady(const Nr* n) { return n->pass[0] && n->submitted; }
void NrMarkSubmitted(Nr* n) { n->submitted = true; }
const char* NrLastError(const Nr* n) { return n->last_error.c_str(); }
bool NrMatches(const Nr* n, UINT w, UINT h) { return n && n->pass[0] && n->live.work_w == w && n->live.work_h == h; }

static void SetCommon(Nr* n, UINT w, UINT h, const NrTuning& t)
{
    SetU(n, "DLSSNR.Enabled", 1u);
    SetU(n, "DLSSNR.Width", w); SetU(n, "DLSSNR.Height", h);
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

// Pass k's tuning: pass 1 as configured; later passes the same minus local tone, which would re-grade
// an already re-graded image (OptiScaler: "local tone is applied only by the first layer").
static NrTuning PassTuning(const NrTuning& t, int k) { NrTuning p = t; if (k) p.local_tone = 0.0f; return p; }

static NVSDK_NGX_Handle* CreateOne(Nr* n, ID3D12GraphicsCommandList* cl, const NrConfig& cfg, const NrTuning& tuning)
{
    n->params->Reset();
    SetU(n, "CreationNodeMask", 1u); SetU(n, "VisibilityNodeMask", 1u);
    // No Input/Output dims, no Upscaling/Scale: feature 18 ignores them (byte-identical output with them set).
    SetCommon(n, cfg.work_w, cfg.work_h, tuning);
    NVSDK_NGX_Handle* h = nullptr; DWORD code = 0;
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    NgxMutex().lock();   // no lock_guard: __try forbids unwindable objects in this frame
    __try { r = n->create(cl, NVSDK_NGX_Feature_Reserved18, n->params, &h); } __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); }
    NgxMutex().unlock();
    if (code) { n->last_error = "CreateFeature raised an exception"; Log("[ngx] CreateFeature raised 0x%08X", code); return nullptr; }
    if (NVSDK_NGX_FAILED(r) || !h) { n->last_error = NgxResultName(r); Log("[ngx] CreateFeature(18) %ux%u -> 0x%08X (%s)", cfg.work_w, cfg.work_h, r, NgxResultName(r)); return nullptr; }
    return h;
}

bool NrCreate(Nr* n, ID3D12GraphicsCommandList* cl, const NrConfig& cfg)
{
    for (auto*& h : n->pass) if (h) { n->retired.push_back({ h, 32 }); h = nullptr; }
    n->submitted = false;
    const int passes = std::clamp(cfg.passes, 1, Nr::kMaxPasses);
    if (!(n->pass[0] = CreateOne(n, cl, cfg, PassTuning(cfg.tuning, 0)))) return false;
    for (int k = 1; k < passes; ++k)
        if (!(n->pass[k] = CreateOne(n, cl, cfg, PassTuning(cfg.tuning, k)))) { Log("[ngx] pass %d not created - running %d", k + 1, k); break; }
    for (int i = 0; i < Nr::kMaxPasses - 1; ++i)
    {
        const bool need = n->pass[i + 1] != nullptr;
        const D3D12_RESOURCE_DESC d = n->mid[i] ? n->mid[i]->GetDesc() : D3D12_RESOURCE_DESC{};
        if (n->mid[i] && (!need || d.Width != cfg.work_w || d.Height != cfg.work_h)) { n->mid[i]->Release(); n->mid[i] = nullptr; }
        if (need && !n->mid[i]) n->mid[i] = GpuMakeTex(*n->g, cfg.work_w, cfg.work_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"nr_pass");
        if (need && !n->mid[i]) { n->release(n->pass[i + 1]); n->pass[i + 1] = nullptr; }
    }
    n->live = cfg;
    int live = 0; for (auto* h : n->pass) live += h != nullptr;
    Log("[ngx] feature 18 created %ux%u preset %d, %d pass%s (submit before evaluating)", cfg.work_w, cfg.work_h, cfg.tuning.preset, live, live > 1 ? "es" : "");
    return true;
}

static unsigned EvaluateOne(Nr* n, ID3D12GraphicsCommandList* cl, NVSDK_NGX_Handle* f, const NrTuning& tuning, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* output, bool reset, float exposure_scale)
{
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
    SetU(n, "DLSSNR.Reset", reset ? 1u : 0u);
    SetCommon(n, w, h, tuning);
    SetF(n, "DLSS.Pre.Exposure", 1.0f); SetF(n, "DLSS.Exposure.Scale", exposure_scale);
    DWORD code = 0; NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    NgxMutex().lock();
    __try { r = n->evaluate(cl, f, n->params, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); }
    NgxMutex().unlock();
    if (code) { n->last_error = "EvaluateFeature raised an exception"; Log("[ngx] evaluate raised 0x%08X", code); n->g->failed = true; return 0xBAD00000; }
    if (NVSDK_NGX_FAILED(r)) { n->last_error = NgxResultName(r); }
    return (unsigned)r;
}

unsigned NrEvaluate(Nr* n, ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* output, bool reset, float exposure_scale)
{
    if (!n->pass[0]) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const UINT w = n->live.work_w, h = n->live.work_h;
    // Checked here, where every caller comes through: a feature larger than the textures fails every
    // frame; a SMALLER one succeeds but writes only a top-left subrect (the rest composes dark).
    // Refusing shows native until the rebuild brings them back in line.
    const D3D12_RESOURCE_DESC oc = output->GetDesc(), ic = color->GetDesc();
    if (oc.Width != w || oc.Height != h || ic.Width != w || ic.Height != h)
    {
        n->last_error = "work textures do not match the feature size";
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    // pass k reads pass k-1's output: color -> mid[0] -> mid[1] -> output, as many as are live
    int last = 0; while (last < Nr::kMaxPasses - 1 && n->pass[last + 1]) ++last;   // index of the last pass
    ID3D12Resource* in = color; unsigned r = 1;
    for (int k = 0; k <= last && r == 1; ++k)
    {
        ID3D12Resource* out = k == last ? output : n->mid[k];
        r = EvaluateOne(n, cl, n->pass[k], PassTuning(n->live.tuning, k), in, mv, out, reset, exposure_scale);
        if (k < last) GpuBarrier(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        in = out;
    }
    for (int i = 0; i < last; ++i) GpuBarrier(cl, n->mid[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return r;
}

void NrRetireTick(Nr* n)
{
    auto& v = n->retired;
    v.erase(std::remove_if(v.begin(), v.end(), [n](Retired& r) { if (--r.evals_left > 0) return false; n->release(r.h); Log("[ngx] retired feature released"); return true; }), v.end());
}
