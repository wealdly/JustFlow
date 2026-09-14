#include "fg.h"
#include "log.h"
#include "ngx_nr.h"          // NgxMutex
#include "nvsdk_ngx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using PFN_FgInitExt  = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using PFN_FgCreate   = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_FgEvaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);
using PFN_FgRelease  = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// The DLSS-G helper header hard-codes the core's EvaluateFeature_C; route it through a pointer so
// the direct-DLL fallback can use the same parameter-setting code (NeuralScreen trick).
static PFN_FgEvaluate g_fg_evaluate = nullptr;
static NVSDK_NGX_Result NVSDK_CONV FgEvalBridge(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback_C cb)
{ return g_fg_evaluate(cl, h, p, cb); }
#define NVSDK_NGX_D3D12_EvaluateFeature_C FgEvalBridge
#include "nvsdk_ngx_helpers_dlssg.h"
#undef NVSDK_NGX_D3D12_EvaluateFeature_C

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const D3D12_RESOURCE_STATES CSRC = D3D12_RESOURCE_STATE_COPY_SOURCE;
static const D3D12_RESOURCE_STATES CDST = D3D12_RESOURCE_STATE_COPY_DEST;
static const int kSlots = 3, kMaxGen = 3;

// Rest states: real CDST, mv CDST, gen[] CSRC, depth NPSR, disable UAV. state: 0 free, 1 writing
// (producer), 2 ready, 3 presenting; guarded by Fg::mu.
struct FgSlot
{
    ID3D12Resource *real = nullptr, *mv = nullptr, *gen[kMaxGen] = {};
    int    state = 0;
    UINT64 seq = 0, fence = 0;
    bool   interpolate = false;
    double interval = 16.0;   // ms between this and the previous submit
};

struct Fg
{
    Gpu*     g = nullptr;
    Overlay* ov = nullptr;
    UINT     w = 0, h = 0, mw = 0, mh = 0;
    int      count = 1;                      // generated frames per real frame
    std::wstring dir;
    const wchar_t* path_list[1] = {};
    NVSDK_NGX_FeatureCommonInfo common = {};
    HMODULE  dll = nullptr;
    PFN_FgCreate create = nullptr; PFN_FgRelease release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    ID3D12Resource *depth = nullptr, *disable = nullptr, *disable_rb = nullptr;
    FgSlot   slots[kSlots];
    FgSlot*  pending = nullptr;              // reserved by FgRecord, handed over by FgSubmit
    UINT64   seq = 0; double last_submit = 0; bool history = false;
    // presenter thread
    std::thread thread;
    std::mutex mu; std::condition_variable cv;
    std::atomic<bool> stop{ false }, failed{ false };
    std::atomic<UINT> presented{ 0 }, drops{ 0 };
    ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr; HANDLE event = nullptr; UINT64 fence_value = 0;
};

static bool Fail(Fg* f, const char* why)
{
    if (!f->failed.exchange(true)) Log("[fg] disabled: %s", why);
    return false;
}

static bool WaitFence(Fg* f, ID3D12Fence* fence, UINT64 v, DWORD ms)
{
    UINT64 done = fence->GetCompletedValue();
    if (done == UINT64_MAX) return false;
    if (done >= v) return true;
    ResetEvent(f->event);
    if (FAILED(fence->SetEventOnCompletion(v, f->event))) return false;
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        if (WaitForSingleObject(f->event, now >= deadline ? 0 : (DWORD)(deadline - now)) != WAIT_OBJECT_0) return false;
        done = fence->GetCompletedValue();
        if (done == UINT64_MAX) return false;
        if (done >= v) return true;
    }
}

// ---- NGX calls with SEH (no unwindable objects in these frames) --------------------------------
static NVSDK_NGX_Result SafeCreate(Fg* f, ID3D12GraphicsCommandList* cl, DWORD* code)
{
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = f->create(cl, NVSDK_NGX_Feature_FrameGeneration, f->params, &f->feature); } __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); }
    return r;
}
static NVSDK_NGX_Result SafeEvaluate(Fg* f, ID3D12GraphicsCommandList* cl, NVSDK_NGX_D3D12_DLSSG_Eval_Params* ep, NVSDK_NGX_DLSSG_Opt_Eval_Params* opt, DWORD* code)
{
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)0x7FFFFFFF;
    __try { r = NGX_D3D12_EVALUATE_DLSSG(cl, f->feature, f->params, ep, opt); } __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); }
    return r;
}

// ---- presenter thread ---------------------------------------------------------------------------
static bool Begin(Fg* f) { return SUCCEEDED(f->alloc->Reset()) && SUCCEEDED(f->list->Reset(f->alloc, nullptr)) ? true : Fail(f, "list reset"); }

static bool Exec(Fg* f, DWORD ms)
{
    if (FAILED(f->list->Close())) return Fail(f, "list close");
    ID3D12CommandList* ls[] = { f->list };
    f->g->queue->ExecuteCommandLists(1, ls);
    const UINT64 v = ++f->fence_value;
    if (FAILED(f->g->queue->Signal(f->fence, v))) return Fail(f, "queue signal");
    return WaitFence(f, f->fence, v, ms) ? true : Fail(f, "fence wait (device removed?)");
}

// Copy src into the overlay backbuffer and present. real_to_rest: the slot's real texture goes
// back to its rest state on the same list (last present of the slot).
static bool Present(Fg* f, ID3D12Resource* src, ID3D12Resource* real_to_rest)
{
    if (!Begin(f)) return false;
    ID3D12Resource* bb = OverlayBackbuffer(f->ov);
    GpuBarrier(f->list, bb, D3D12_RESOURCE_STATE_PRESENT, CDST);
    f->list->CopyResource(bb, src);
    GpuBarrier(f->list, bb, CDST, D3D12_RESOURCE_STATE_PRESENT);
    if (real_to_rest) GpuBarrier(f->list, real_to_rest, CSRC, CDST);
    if (!Exec(f, 2000)) return false;
    if (!OverlayPresent(f->ov)) return Fail(f, "Present failed");
    ++f->presented;
    return true;
}

static bool Evaluate(Fg* f, FgSlot* s, bool interpolate, bool& allow)
{
    if (!Begin(f)) return false;
    ID3D12GraphicsCommandList* cl = f->list;
    GpuBarrier(cl, s->real, CDST, NPSR);
    GpuBarrier(cl, s->mv, CDST, NPSR);
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    for (int i = 0; i < 4; ++i)
        opt.cameraViewToClip[i][i] = opt.clipToCameraView[i][i] = opt.clipToLensClip[i][i] = opt.clipToPrevClip[i][i] = opt.prevClipToClip[i][i] = 1.0f;
    // mv is in work-res pixels (same convention as NeuralScreen's field): 1/size normalises it.
    opt.mvecScale[0] = 1.0f / (float)f->mw; opt.mvecScale[1] = 1.0f / (float)f->mh;
    opt.cameraUp[1] = opt.cameraRight[0] = opt.cameraFwd[2] = 1.0f;
    opt.cameraNear = 0.1f; opt.cameraFar = 1000.0f; opt.cameraFOV = 1.0f;
    opt.cameraAspectRatio = (float)f->w / (float)f->h;
    opt.cameraMotionIncluded = opt.orthoProjection = opt.motionVectorsDilated = true;
    opt.colorBuffersHDR = false;
    opt.reset = !interpolate;
    opt.mvecsSubrectSize = opt.depthSubrectSize = { f->mw, f->mh };
    opt.backbufferSubrectSize = opt.outputInterpSubrectSize = { f->w, f->h };
    opt.multiFrameCount = (unsigned)f->count;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
    ep.pBackbuffer = s->real; ep.pMVecs = s->mv; ep.pDepth = f->depth; ep.pOutputDisableInterpolation = f->disable;
    f->params->Reset();
    f->params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)s->seq);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success; DWORD code = 0;
    for (int i = 0; i < f->count; ++i)
    {
        opt.multiFrameIndex = (unsigned)i + 1;
        ep.pOutputInterpFrame = s->gen[i];
        GpuBarrier(cl, s->gen[i], CSRC, UAV);
        { std::lock_guard<std::mutex> lk(NgxMutex()); r = SafeEvaluate(f, cl, &ep, &opt, &code); }
        GpuBarrier(cl, f->disable, UAV, CSRC);
        cl->CopyBufferRegion(f->disable_rb, (UINT64)i * 4, f->disable, 0, 4);
        GpuBarrier(cl, f->disable, CSRC, UAV);
        GpuBarrier(cl, s->gen[i], UAV, CSRC);
        if (code || NVSDK_NGX_FAILED(r)) break;
    }
    GpuBarrier(cl, s->real, NPSR, CSRC);
    GpuBarrier(cl, s->mv, NPSR, CDST);
    if (!Exec(f, 10000)) return false;
    if (code) { Log("[fg] evaluate raised 0x%08X", code); return Fail(f, "evaluate raised an exception"); }
    if (NVSDK_NGX_FAILED(r)) { Log("[fg] evaluate -> 0x%08X (%s)", r, NgxResultName(r)); return Fail(f, "evaluate failed"); }
    allow = true;
    uint8_t* d = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)f->count * 4 };
    if (SUCCEEDED(f->disable_rb->Map(0, &rr, (void**)&d)))
    {
        for (int i = 0; i < f->count; ++i) allow = allow && d[i * 4] == 0;
        D3D12_RANGE none = { 0, 0 }; f->disable_rb->Unmap(0, &none);
    }
    else allow = false;
    return true;
}

static void Presenter(Fg* f)
{
    using clock = std::chrono::steady_clock;
    UINT64 prev = 0;
    auto newer_ready = [&](UINT64 seq) { for (auto& x : f->slots) if (x.state == 2 && x.seq > seq) return true; return false; };
    while (!f->stop && !f->failed)
    {
        FgSlot* s = nullptr;
        {
            std::unique_lock<std::mutex> lk(f->mu);
            f->cv.wait(lk, [&] { return f->stop || newer_ready(0); });
            if (f->stop) break;
            for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq > s->seq)) s = &x;
            for (auto& x : f->slots) if (x.state == 2 && &x != s) { x.state = 0; ++f->drops; }
            s->state = 3;
        }
        bool ok = WaitFence(f, f->g->fence, s->fence, 2000) ? true : Fail(f, "render fence wait");
        bool allow = false;
        const bool interp = s->interpolate && s->seq == prev + 1;   // a dropped frame breaks the pair
        if (ok) ok = Evaluate(f, s, interp, allow);
        if (ok)
        {
            const auto start = clock::now();
            if (interp && allow)
                for (int i = 0; i < f->count && !f->stop; ++i)
                {
                    const auto deadline = start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(s->interval * (i + 1) / (f->count + 1)));
                    if (clock::now() >= deadline) { ++f->drops; continue; }   // late: never burst stale frames
                    if (!Present(f, s->gen[i], nullptr)) { ok = false; break; }
                    std::unique_lock<std::mutex> lk(f->mu);
                    if (f->cv.wait_until(lk, deadline, [&] { return f->stop || newer_ready(s->seq); })) break;
                }
            if (ok && !f->stop) ok = Present(f, s->real, s->real);
        }
        prev = s->seq;
        std::lock_guard<std::mutex> lk(f->mu);
        s->state = 0;
    }
}

// ---- lifecycle ---------------------------------------------------------------------------------------
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

static bool CreateFeature(Fg* f, const char* how)
{
    Gpu& g = *f->g;
    NVSDK_NGX_Parameter* p = f->params;
    p->Reset();
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u); p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, f->w); p->Set(NVSDK_NGX_Parameter_Height, f->h);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)DXGI_FORMAT_R8G8B8A8_UNORM);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, f->mw); p->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, f->mh);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    if (!GpuBegin(g)) return false;
    DWORD code = 0; NVSDK_NGX_Result r;
    { std::lock_guard<std::mutex> lk(NgxMutex()); r = SafeCreate(f, g.list, &code); }
    const UINT64 v = GpuEnd(g);
    const bool ok = v && GpuWait(g, g.fence, v, 30000) && !code && !NVSDK_NGX_FAILED(r) && f->feature;
    if (!ok) { Log("[fg] CreateFeature(11) via %s -> 0x%08X (%s)%s", how, r, NgxResultName(r), code ? " raised" : ""); f->feature = nullptr; }
    return ok;
}

Fg* FgCreate(Gpu& g, Overlay* ov, const wchar_t* dir, UINT out_w, UINT out_h, UINT mv_w, UINT mv_h, int multiplier)
{
    Fg* f = new Fg;
    f->g = &g; f->ov = ov; f->dir = dir; f->w = out_w; f->h = out_h; f->mw = mv_w; f->mh = mv_h;
    f->count = std::clamp(multiplier, 2, 4) - 1;
    auto fail = [&](const char* why) { Fail(f, why); FgDestroy(f); return (Fg*)nullptr; };

    if (FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&f->alloc)) ||
        FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f->alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&f->list)) ||
        FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&f->fence)))
        return fail("presenter command objects");
    f->list->Close();
    f->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // NGX core: Init is harmless if ngx_nr already did it; the parameter block comes from the core.
    f->path_list[0] = f->dir.c_str();
    f->common.PathListInfo.Path = f->path_list; f->common.PathListInfo.Length = 1;
    f->common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, dir, g.dev, &f->common, NVSDK_NGX_Version_API);
    Log("[fg] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    r = NVSDK_NGX_D3D12_AllocateParameters(&f->params);
    if (NVSDK_NGX_FAILED(r) || !f->params) { Log("[fg] AllocateParameters -> 0x%08X (%s)", r, NgxResultName(r)); return fail("no NGX parameter block"); }

    f->create = NVSDK_NGX_D3D12_CreateFeature; f->release = NVSDK_NGX_D3D12_ReleaseFeature; g_fg_evaluate = NVSDK_NGX_D3D12_EvaluateFeature_C;
    if (!CreateFeature(f, "NGX core"))
    {
        // Fallback: drive nvngx_dlssg.dll directly (NeuralScreen path) with the same parameter block.
        const std::wstring path = f->dir + L"\\nvngx_dlssg.dll";
        f->dll = LoadLibraryW(path.c_str());
        if (!f->dll) { Log("[fg] %ls did not load (err %lu)", path.c_str(), GetLastError()); return fail("CreateFeature failed and no direct runtime"); }
        auto init = (PFN_FgInitExt)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_Init_Ext");
        f->create = (PFN_FgCreate)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_CreateFeature");
        f->release = (PFN_FgRelease)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_ReleaseFeature");
        g_fg_evaluate = (PFN_FgEvaluate)GetProcAddress(f->dll, "NVSDK_NGX_D3D12_EvaluateFeature");
        if (!init || !f->create || !f->release || !g_fg_evaluate) return fail("nvngx_dlssg.dll exports missing");
        r = init(0x1000000ULL, dir, g.dev, NVSDK_NGX_Version_API, f->params);
        Log("[fg] direct Init_Ext -> 0x%08X (%s)", r, NgxResultName(r));
        if (NVSDK_NGX_FAILED(r) || !CreateFeature(f, "nvngx_dlssg.dll")) return fail("CreateFeature failed (core and direct)");
    }

    // guides + outputs
    f->depth = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, NPSR, L"fg_depth");
    std::vector<float> flat((size_t)mv_w * mv_h, 0.5f);
    if (!f->depth || !GpuUploadTex(g, f->depth, flat.data(), mv_w, mv_h, 4, NPSR)) return fail("depth texture");
    f->disable = GpuMakeBuffer(g, 16, D3D12_HEAP_TYPE_DEFAULT, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"fg_disable");
    f->disable_rb = GpuMakeBuffer(g, 16, D3D12_HEAP_TYPE_READBACK, CDST, D3D12_RESOURCE_FLAG_NONE, L"fg_disable_rb");
    if (!f->disable || !f->disable_rb) return fail("disable buffers");
    for (auto& s : f->slots)
    {
        s.real = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, CDST, L"fg_real");
        s.mv = GpuMakeTex(g, mv_w, mv_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, CDST, L"fg_mv");
        if (!s.real || !s.mv) return fail("slot textures");
        for (int i = 0; i < f->count; ++i)
            if (!(s.gen[i] = GpuMakeTex(g, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, CSRC, L"fg_gen"))) return fail("slot textures");
    }
    f->thread = std::thread(Presenter, f);
    Log("[fg] feature created %ux%u mv %ux%u multiplier %d", out_w, out_h, mv_w, mv_h, f->count + 1);
    return f;
}

void FgDestroy(Fg* f)
{
    if (!f) return;
    f->stop = true; f->cv.notify_all();
    if (f->thread.joinable()) f->thread.join();
    // Drain: producer copies and presenter lists still referencing the slots share Gpu::queue.
    if (f->fence && f->event && SUCCEEDED(f->g->queue->Signal(f->fence, ++f->fence_value))) WaitFence(f, f->fence, f->fence_value, 5000);
    if (f->feature) { std::lock_guard<std::mutex> lk(NgxMutex()); f->release(f->feature); f->feature = nullptr; }
    // ponytail: no Shutdown1 - NR's parameter block lives in the same core; the refcount leaks until exit.
    if (f->params) NVSDK_NGX_D3D12_DestroyParameters(f->params);
    for (auto& s : f->slots) { REL(s.real); REL(s.mv); for (auto& t : s.gen) REL(t); }
    REL(f->depth); REL(f->disable); REL(f->disable_rb);
    REL(f->list); REL(f->alloc); REL(f->fence);
    if (f->event) CloseHandle(f->event);
    if (f->dll) FreeLibrary(f->dll);
    delete f;
}

bool FgFailed(const Fg* f) { return f->failed; }
void FgStats(Fg* f, UINT& presented, UINT& drops) { presented = f->presented.exchange(0); drops = f->drops.exchange(0); }

bool FgRecord(Fg* f, ID3D12GraphicsCommandList* cl, ID3D12Resource* composed, ID3D12Resource* mv)
{
    if (f->failed) return false;
    FgSlot* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        for (auto& x : f->slots) if (x.state == 0) { s = &x; break; }
        if (!s)   // producer ahead of the presenter: overwrite the oldest ready slot (it would be discarded anyway)
        {
            for (auto& x : f->slots) if (x.state == 2 && (!s || x.seq < s->seq)) s = &x;
            if (!s) return false;
            ++f->drops;
        }
        s->state = 1;
    }
    f->pending = s;
    cl->CopyResource(s->real, composed);
    GpuBarrier(cl, mv, NPSR, CSRC);
    cl->CopyResource(s->mv, mv);
    GpuBarrier(cl, mv, CSRC, NPSR);
    return true;
}

void FgSubmit(Fg* f, UINT64 render_fence_value, bool reset)
{
    FgSlot* s = f->pending;
    if (!s) return;
    f->pending = nullptr;
    const double now = NowMs();
    const double interval = f->history ? now - f->last_submit : 16.0;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        if (!render_fence_value) { s->state = 0; return; }
        s->seq = ++f->seq; s->fence = render_fence_value;
        s->interpolate = f->history && !reset && interval < 100.0;
        s->interval = std::clamp(interval, 4.0, 50.0);
        s->state = 2;
    }
    f->history = true; f->last_submit = now;
    f->cv.notify_one();
}
