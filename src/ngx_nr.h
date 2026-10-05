// DLSS 5 neural rendering (nvngx_dlssnr.dll, NGX feature 18) on D3D12 through the caller-gate
// forwarder (nvngx.dll_justflow.dll). Owns the parameter block, the float-slot probe, the feature
// handle, and a retire ring for replaced features (each ~420 MB; freed ~32 evaluates later).
#pragma once
#include "d3d.h"
#include <mutex>
#include <string>

// Serialises NGX entry points (NR on the main thread, DLSS-G on the FG presenter thread): the
// runtime's thread-safety across features is undocumented and each call only records commands.
inline std::mutex& NgxMutex() { static std::mutex m; return m; }
// NGX core init for a feature beside NR (DLSS-G, Frame Warp), and a parameter block of its own. Init is
// harmless when NR already ran it. nullptr on failure; both results are logged under `tag`.
struct NVSDK_NGX_Parameter;
NVSDK_NGX_Parameter* NgxCoreParams(Gpu& g, const wchar_t* dir, const char* tag);

enum NrParamBlock  { NrBlockAllocate = 1, NrBlockCapability = 2, NrBlockOwn = 3 };

struct NrTuning
{
    int   preset = 0;              // DLSSNR.Hint.Render.Preset
    int   style = 0;               // 0 Standard (least restyling), 1 Natural, 2 Cinematic
    float intensity = 1.0f;
    float local_structure = 1.0f;
    float local_tone = 0.2f;       // remaps tone and colour: low keeps the game's own look
    float skin_structure = -1.0f;  // -1 = follow local structure
    bool  auto_mask = true;
    bool  ui_correction = true;
};

struct NrConfig
{
    UINT          work_w = 2560, work_h = 1440;   // model size; Color/Output/MVec are this size
    NrParamBlock  block = NrBlockCapability;
    NrTuning      tuning;
    // Model passes per frame, 1..3 (OptiScaler's "Model passes"): pass k+1 refines pass k's output, each
    // with a feature - and a temporal history - of its own; local tone on the first pass only. The
    // caller still composes the final output against the original input once, so colour and strength
    // do not compound. Cost is linear: the model is nearly all of it.
    int           passes = 1;
};

struct Nr;

// Loads forwarder + runtime from `dir` (nvngx.dll_justflow.dll and nvngx_dlssnr.dll next to the
// exe), inits NGX core (per `block`), calls Init_Ext through the forwarder, probes the float slot.
Nr*  NrInit(Gpu& g, const wchar_t* dir, NrParamBlock block);
void NrShutdown(Nr* n);
int  NrFloatSlot(const Nr* n);           // -1 if not found

// Records CreateFeature(18) on `cl` with the given config. The caller MUST submit that list and
// must not evaluate on it; the first NrEvaluate is legal on a later list. A previous feature is
// parked and released after ~32 evaluates. Returns false on failure (result logged).
bool NrCreate(Nr* n, ID3D12GraphicsCommandList* cl, const NrConfig& cfg);
bool NrReady(const Nr* n);                // a feature exists and its create list was submitted
void NrMarkSubmitted(Nr* n);              // call after the create list's GpuEnd
bool NrMatches(const Nr* n, UINT w, UINT h);   // the live feature was created at exactly w x h

// Records EvaluateFeature on `cl` - once per pass, chained through intermediates of its own. color/mv must
// be in NON_PIXEL_SHADER_RESOURCE, output in UNORDERED_ACCESS. color/output: RGBA8 work-size. mv: R16G16_FLOAT work-size, pixel units
// (MVecScale is passed as 1.0). Wrapped in __try. Returns the NGX result (1 = success).
unsigned NrEvaluate(Nr* n, ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv,
                    ID3D12Resource* output, bool reset, float exposure_scale);
// Tick the retire ring (call once per evaluate).
void NrRetireTick(Nr* n);
// Human-readable failure text for the last create/evaluate.
const char* NrLastError(const Nr* n);
