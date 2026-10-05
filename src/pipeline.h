// The per-frame pipeline shared by the live loop (main.cpp) and --bench (bench.cpp):
// capture BGRA8 (COMMON) -> swizzle/gray/downscale -> OFA -> expand -> NR evaluate -> compose ->
// overlay backbuffer. The struct is plain data on purpose: main and bench poke at it directly.
#pragma once
#include "d3d.h"
#include "config.h"
#include "compose.h"
#include "fg.h"
#include "ngx_nr.h"
#include "nvofa.h"
#include "present.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct StageStats
{
    std::vector<double> v;
    // Bounded: every one of these is drained by the [stats] tick, so with stats off nothing
    // would ever clear them. 4096 samples is ~45 s at 90 fps, far more than any percentile
    // needs, and the cap costs a compare. (fg.cpp bounds its own ring the same way.)
    void   add(double x) { if (x >= 0 && v.size() < 4096) v.push_back(x); }
    double pct(double p) const;   // -1 when empty
    double med() const { return pct(0.5); }
    double p95() const { return pct(0.95); }
};

// PS_LIST1/PS_LIST2 are whole-submission spans, so they count the barriers and the copies that no
// per-stage stamp covers. They cannot be one span: GpuEnd advances the ring slot and GpuBegin
// clears it, so a pair cannot cross a submission. Real frame cost is LIST1 + ofa + LIST2 - the old
// frame_gpu_ms was a sum of stages with OFA excluded, which reported a fraction of the frame.
// The GPU-timed stages, in stamp-pair order: stage i is bracketed by stamps 2i and 2i+1 (PipelineReadStamps
// reads the first PS_STAMPED pairs in one loop). PS_OFA / PS_OFA2 come last: bench only, a CPU round trip.
enum PipeStage { PS_SWIZZLE, PS_GRAYDS, PS_EVAL, PS_COMPOSE, PS_EXPAND, PS_FILTER, PS_LIST1, PS_LIST2, PS_STAMPED, PS_OFA = PS_STAMPED, PS_OFA2, PS_COUNT };
extern const char* const kPipeStageName[PS_COUNT];

struct Pipeline
{
    Gpu*     g = nullptr;
    Config   cfg;
    UINT     w = 0, h = 0;        // native (capture) size
    UINT     ww = 0, wh = 0;      // work size (NR model)
    UINT     gw = 0, gh = 0;      // gray / OFA input size
    Shaders* sh = nullptr;
    Nr*      nr = nullptr;
    Ofa*     ofa = nullptr;
    Overlay* ov = nullptr;
    HWND     target = nullptr;    // the game window: the settings dialog opens on some other monitor
    Fg*      fg = nullptr;        // the presenter thread: always exists with an overlay (passthrough, or generation while cfg.fg_enabled works)
    ID3D12Resource *color4k = nullptr, *gray = nullptr, *out4k = nullptr;   // rest: NPSR, UAV, COPY_SOURCE
    ID3D12Resource *sharp4k = nullptr;                                     // out4k through the filter layer, rest COPY_SOURCE
    ID3D12Resource *shown = nullptr;                                       // the texture handed onward last frame (out4k or sharp4k)
    ID3D12Resource *nr_in = nullptr, *nr_out = nullptr, *mv = nullptr;     // rest: NPSR, UAV, NPSR
    // NR feature lifecycle
    bool create_pending = false;
    int  rebuild_countdown = 0;
    UINT evals_since_create = 0;
    bool force_reset = true;
    // user state
    int    wipe = 0;              // 0 off, 1 split 0.5, 2 sweep (2 s period)
    double wipe_t0 = 0;
    bool   measure_ofa = false;   // bench only: CPU-wait around OfaExecute to time it in isolation
    bool   last_evaluated = false;
    unsigned eval_fails = 0; bool model_dead = false;   // consecutive failed evaluates; 30 of them switch the model off until the next create
    bool   had_mv = false;        // motion vectors were computed last frame (a gap forces a flow reset)
    // capture timestamps of the frame being fed (QPC ticks, 0 = unknown): set by main before PipelineFrame
    LONGLONG cap_qpc = 0, acq_qpc = 0;
    // stats (GPU timestamps by stage), cleared by the reader
    StageStats st[PS_COUNT];
    StageStats cpu_wait[4];   // 0 GpuBegin(list1) 1 OfaExecute 2 GpuBegin(list2) 3 FgSubmit (age/pipe latency: FgStats)
    UINT64     last_stamp_fence_slot[3] = {};
    int        ofa_cur = 0;       // per-frame OFA input ping-pong (slots 0/1); 2..4 are the model track's held grays
    UINT       frame_index = 0;   // frames fed (main thread)

    // ---- addon UI mask ([ui] mask=1): the JustFlow addon draws a 4 px strip of 4x4 cells at the
    // top-left of the game frame (addon\JustFlow\JustFlow.lua). List 1 copies that region of color4k
    // into a READBACK ring; a retired slot is decoded on the CPU after the next submission.
    ID3D12Resource* strip_rb[Gpu::kFrames] = {};
    UINT64     strip_fence[Gpu::kFrames] = {};   // main fence value of the copy into strip_rb[i], 0 = nothing pending
    int        mask_n = 0;                       // rects from the last valid strip
    UiRect     mask_rects[64] = {};
    double     mask_seen = 0;                    // NowMs() of the last valid decode (0 = never); stale after 1 s
    bool       mask_active = false;              // a fresh mask is being applied (logged on change)

    // ---- on-screen text (CsText onto the frame handed onward): toast (2 s, 0.4 s fade) + status HUD
    char       toast[65] = "";
    double     toast_t0 = 0, toast_until_ms = 0;
    bool       model_toast_pending = false;      // "Model ready" once a rebuilt model composes
    bool       hud = false;                      // [ui] hud, F7
    std::string ui_key;                          // PipelineUi: what the UI layer shows now (redrawn when it changes)
    char       hud_line[3][65] = {};             // filled by main every 250 ms (empty line = nothing drawn); 64 = what CsText draws

    // ---- decoupled model track ([nr] mode=async) --------------------------------------------------
    // Main hands one native frame at a time to the model thread (model_src copy + gray in a held OFA
    // slot, completed by main fence `fence`) whenever the thread asks (model_wants_frame). The thread
    // downscales, runs its own flow (pair 1, held slot vs the previous model frame's), evaluates NR on
    // its own queue and publishes residual[idx] (= nr_out - nr_in at work res) with its ctx fence
    // value and the held slot of its frame; main composes native + residual each frame, warped by the
    // flow current frame -> that held gray (pair 2 -> mv_res), so the warp spans the residual's age.
    // Held slots 2..4: a hand-off never takes the last handed-off slot (the model's next flow reference)
    // nor cmp_held (main's flow reference until it moves to a newer residual); the model thread only
    // reads them, and asks for the next frame once its flow has consumed them.
    struct ModelFrame { int held = 2; UINT64 fence = 0; UINT index = 0; bool reset = true; };
    struct ModelParams { float zero_below = 0.5f; float exposure = 1.0f; int max_fps = 0, warmup = 8; };
    GpuCtx     model_ctx;
    std::thread model_thread;
    std::mutex model_mu; std::condition_variable model_cv;   // guards model_stop/model_frame_ready/model_frame/model_params
    bool       model_stop = false, model_frame_ready = false;
    ModelFrame model_frame; ModelParams model_params;
    std::atomic<bool> model_wants_frame{ true }, model_failed{ false };
    std::atomic<UINT> model_evals{ 0 };
    std::atomic<UINT64> residual_read_fence[2] = { 0, 0 };   // main fence value of the last list that read residual[i]
    bool       model_reset_pending = true;             // main: reset flags accumulated since the last hand-off
    ID3D12Resource* model_src = nullptr;               // RGBA8 native, COPY_DEST at rest
    ID3D12Resource *nr_in_m = nullptr, *nr_out_m = nullptr, *mv_m = nullptr;   // work res: NPSR, UAV, NPSR
    // latewarp engine: the flow averaged on a 32x18 grid, read back a few frames later for the mouse model
    ID3D12Resource* mvgrid = nullptr; ID3D12Resource* mvgrid_rb[4] = {};
    // duplicate frames (CsSame): per-64px-tile change flags, read back in the same frame before FG
    ID3D12Resource* same = nullptr; ID3D12Resource* same_rb = nullptr; UINT same_w = 0, same_h = 0, same_pitch = 0;
    float    hdr_white = 0;   // the capture is FP16 (HDR desktop): its SDR white in scRGB units (main sets it per frame)
    bool     shown_native = false;   // the last composed frame was native with the model on: duplicates are not skipped until it is not
    ID3D12Resource* deb4k = nullptr;   // the filter layer's deband output (RGBA8, NPSR at rest)
    UINT     dups = 0;   // stats: captures identical to the previous one, not handed to FG
    UINT     cuts = 0;   // stats: scene cuts (the model and FG were reset)
    UINT     reset_frame = 0;   // frame_index of the last reset (cut, gap, resize): async residuals from before it are not composed
    std::vector<uint8_t> same_prev, same_cur;   // tile luma (CsSame G) of the previous and this frame, for the cut test
    struct { UINT64 fence = 0; LONGLONG t0 = 0, t1 = 0; } mvgrid_q[4];
    LONGLONG lw_prev_cap = 0;
    float    lw_g[2] = {};   // the flow's median (work px, backward) from the last grid read back: the camera's motion
    ID3D12Resource* mv_res = nullptr;                  // main: motion current frame -> residual's model frame (work res, NPSR)
    ID3D12Resource* residual[2] = {};                  // RGBA16F work res, NPSR at rest
    std::mutex pub_mu;                                 // guards pub_* and model_ms
    int        pub_idx = -1; UINT64 pub_fence = 0; UINT pub_frame = 0; int pub_held = -1;
    StageStats model_ms;                               // model evaluate GPU ms (drained by the stats reader)
    int        cmp_idx = -1; UINT64 cmp_fence = 0; int cmp_held = -1;   // residual the main queue last waited for + its held gray slot
    StageStats residual_age;                           // per composed frame: frame_index - residual's frame index
};

std::wstring ExeDir();
// param_block from justflow.spike.ini; the work size is WorkAuto's (main.cpp), resolved once the capture size is known.
void ResolveWork(Config& c, const std::wstring& dir);
bool WorkSelfTest();   // the work-size rule against known answers (selftest)
// Profiles: profiles\*.ini next to the exe, `names` = the sorted stems. `ini` (--ini) wins when non-empty
// (index = its stem's slot in names, -1 if none); else justflow.ini [app] profile=<name> when it is not "auto";
// otherwise the first profile whose [capture] window exists right now, else "wow", else the first.
// Returns the ini path ("" when there is no profile at all).
std::wstring PickProfile(const std::wstring& dir, const std::wstring& ini, std::vector<std::wstring>& names, int& index);
// "[config] app ... + profile ..." with `have` from ConfigLoad, plus the app-layer keys the profile carries (ignored).
void LogConfigFiles(const std::wstring& app, const std::wstring& profile, int have);

Pipeline* PipelineCreate(Gpu& g, const Config& cfg, UINT w, UINT h, bool with_overlay, HWND target);
// One frame. capture_bgra is BGRA8 w x h in COMMON; the queue first waits on wait_fence/wait_value
// (nullptr = no wait). Returns false on a GPU failure. p->last_evaluated tells whether NR ran (a
// create-only frame does not present).
bool PipelineFrame(Pipeline* p, ID3D12Resource* capture_bgra, ID3D12Fence* wait_fence, UINT64 wait_value, bool reset);
// The HUD and the toast into the overlay's UI layer, drawn over every presented frame (uiblend.hlsl) - only
// when what they show changed. Call on every pass of the capture loop, frames or not.
void PipelineUi(Pipeline* p);
// New native size: reallocates the 4K textures and the overlay buffers (waits for idle).
bool PipelineResize(Pipeline* p, UINT w, UINT h);
// Hot reload: compose params apply next frame; a create-latched change schedules a debounced rebuild.
void PipelineReload(Pipeline* p, const Config& c);
// Toast `fmt` (printf) top-centre for 2 s (drawn by the next PipelineFrame while [ui] toast=1). Main thread only.
void PipelineToast(Pipeline* p, const char* fmt, ...);
// Pull retired GPU timestamps into p->st (called inside PipelineFrame; call once more after idle).
void PipelineReadStamps(Pipeline* p);
void PipelineDestroy(Pipeline* p);
