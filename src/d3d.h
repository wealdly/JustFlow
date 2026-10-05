// JustFlow D3D12 primitives shared by every module: device + queue, a 3-deep allocator ring with
// one fence, texture creation, barriers, a shader-visible descriptor ring, compute PSO helpers and
// GPU timestamps. Everything is plain C-style over raw COM pointers; no smart pointers on purpose.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>
#include <vector>

#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
// Barrier/creation shorthands: every module's state transitions read as one line.
static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static const D3D12_RESOURCE_STATES CSRC = D3D12_RESOURCE_STATE_COPY_SOURCE, CDST = D3D12_RESOURCE_STATE_COPY_DEST;
static const D3D12_RESOURCE_STATES COMMON = D3D12_RESOURCE_STATE_COMMON, PRESENT = D3D12_RESOURCE_STATE_PRESENT;
static const D3D12_RESOURCE_FLAGS FUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

// One queue's recording ring: a 3-deep allocator ring behind one fence + its own event (so another thread
// can wait without sharing one), a shader-visible descriptor heap and a timestamp heap, both split into
// kFrames slots. Gpu is the main queue's; a GpuCtx is a secondary queue's.
struct GpuRing
{
    static const int kFrames = 3;
    static const int kStamps = 32;             // timestamp slots per frame (pairs: even=begin, odd=end)
    static const UINT kDescPerSlot = 1024;     // shader-visible CBV/SRV/UAV descriptors per ring slot

    ID3D12CommandQueue*        queue = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandAllocator*    alloc[kFrames] = {};
    UINT64                     alloc_fence[kFrames] = {};
    int                        slot = 0;         // current ring slot (valid between Begin/End)
    ID3D12Fence*               fence = nullptr;
    HANDLE                     event = nullptr;
    UINT64                     fence_value = 0;
    bool                       failed = false;   // a submission failed or the device was removed
    ID3D12DescriptorHeap*      desc_heap = nullptr;     // kFrames * kDescPerSlot
    UINT                       desc_used = 0;           // within the current slot
    ID3D12QueryHeap*           ts_heap = nullptr;       // kFrames * kStamps
    ID3D12Resource*            ts_readback = nullptr;   // kFrames * kStamps * 8 bytes
    UINT64                     ts_freq = 0;
    bool                       ts_written[kFrames][kStamps] = {};
};

struct GpuCtx : GpuRing {};

struct Gpu : GpuRing
{
    static const int kCtx = 4;                 // secondary contexts (GpuCtxInit registers here)

    IDXGIFactory4*             factory = nullptr;
    IDXGIAdapter3*             adapter = nullptr;
    LUID                       luid = {};
    int                        adapter_index = -1;
    ID3D12Device*              dev = nullptr;
    ID3D12Device1*             dev1 = nullptr;          // SetResidencyPriority (null if the runtime lacks it)
    UINT                       desc_size = 0;
    GpuCtx*                    ctx[kCtx] = {};          // GpuDispatch / GpuSrvTable route by command list
};

// ---- lifecycle -------------------------------------------------------------------------------
// vendor: PCI vendor id the adapter must have (0x10DE NVIDIA, 0x8086 Intel, 0 = any hardware adapter).
bool   GpuInit(Gpu& g, int adapter_index /* -1 = first matching */, UINT vendor = 0x10DE);
void   GpuShutdown(Gpu& g);
// Waits for the ring slot, resets allocator + list, resets descriptor ring for the slot.
bool   GpuBegin(Gpu& g);
// Resolves timestamps, closes, executes, signals. Returns the fence value (0 on failure).
UINT64 GpuEnd(Gpu& g);
bool   GpuWait(Gpu& g, ID3D12Fence* f, UINT64 v, DWORD ms);
inline bool GpuWaitIdle(Gpu& g, DWORD ms = 5000) { return GpuWait(g, g.fence, g.fence_value, ms); }
// This process's local video memory, MB. False = the adapter would not answer.
bool GpuVram(Gpu& g, double& used_mb, double& budget_mb);
// Tell the OS how much video memory this process needs right now (IDXGIAdapter3::SetVideoMemoryReservation,
// local segment group = shared system memory on an iGPU): its current usage, clamped to what is available
// for reservation. Call after building and after releasing a pipeline. Returns the MB reserved, -1 on failure.
double GpuReserveCurrentUsage(Gpu& g);
void   GpuLogDeviceRemoved(Gpu& g, const char* where);

// ---- secondary context ------------------------------------------------------------------------
// Direct-type queue (NGX evaluate wants a graphics list) at `priority`. Registers in g.ctx[].
bool   GpuCtxInit(Gpu& g, GpuCtx& c, D3D12_COMMAND_QUEUE_PRIORITY priority, const wchar_t* name);
void   GpuCtxShutdown(Gpu& g, GpuCtx& c);
bool   GpuCtxBegin(Gpu& g, GpuCtx& c);
UINT64 GpuCtxEnd(GpuCtx& c);            // returns the ctx fence value (0 on failure)
// Wait on any fence using the ctx's own event (safe from the ctx's thread).
bool   GpuCtxWait(GpuCtx& c, ID3D12Fence* f, UINT64 v, DWORD ms);
inline bool GpuCtxWaitIdle(GpuCtx& c, DWORD ms = 5000) { return GpuCtxWait(c, c.fence, c.fence_value, ms); }

// ---- resources -------------------------------------------------------------------------------
ID3D12Resource* GpuMakeTex(Gpu& g, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES initial, const wchar_t* name);
ID3D12Resource* GpuMakeBuffer(Gpu& g, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial,
                              D3D12_RESOURCE_FLAGS flags, const wchar_t* name);
void GpuBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
void GpuUavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r);
// Upload CPU pixels into a texture (row pitch = w * bpp). Blocking; for spikes/bench only.
bool GpuUploadTex(Gpu& g, ID3D12Resource* tex, const void* pixels, UINT w, UINT h, UINT bpp,
                  D3D12_RESOURCE_STATES tex_state);
inline UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }
// Records a copy of tex (COPY_SOURCE) into a READBACK buffer, rows `pitch` apart (256-aligned); the caller
// maps it once the list has retired. box = nullptr: the whole texture.
void GpuCopyToReadback(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, ID3D12Resource* rb, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, const D3D12_BOX* box = nullptr);
// Read a texture back to CPU (blocking). out must hold w*h*bpp bytes. For dumps/asserts only.
bool GpuReadbackTex(Gpu& g, ID3D12Resource* tex, void* out, UINT w, UINT h, UINT bpp, D3D12_RESOURCE_STATES tex_state);

// ---- compute -------------------------------------------------------------------------------------
// Root signature layout for every compute shader in this project:
//   root param 0 : 32-bit root constants (num_consts DWORDs)            -> cbuffer b0
//   root param 1 : descriptor table SRV t0..t(num_srv-1)
//   root param 2 : descriptor table UAV u0..u(num_uav-1)
//   static sampler s0 : linear, clamp
struct ComputePso
{
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    UINT num_srv = 0, num_uav = 0, num_consts = 0;
};
struct GpuView { ID3D12Resource* res; DXGI_FORMAT fmt; };   // fmt = DXGI_FORMAT_UNKNOWN -> resource's own
bool GpuMakeCompute(Gpu& g, const void* cso, size_t cso_len, UINT num_srv, UINT num_uav, UINT num_consts,
                    ComputePso& out, const wchar_t* name);
// Allocates descriptors from the current slot, creates the views, binds everything, dispatches.
// `cl` == a registered GpuCtx's list -> that ctx's heap/slot; otherwise Gpu's.
void GpuDispatch(Gpu& g, ID3D12GraphicsCommandList* cl, const ComputePso& p, const GpuView* srvs, const GpuView* uavs,
                 const void* consts, UINT groups_x, UINT groups_y, UINT groups_z = 1);
inline UINT GpuGroups(UINT n, UINT size) { return (n + size - 1) / size; }
// For draws: allocates n SRVs from the same ring GpuDispatch uses (by `cl`), sets the heap on `cl` and
// returns the table's GPU handle (ptr 0 = ring exhausted).
D3D12_GPU_DESCRIPTOR_HANDLE GpuSrvTable(Gpu& g, ID3D12GraphicsCommandList* cl, const GpuView* srvs, UINT n);

// ---- timestamps ---------------------------------------------------------------------------------
// Write timestamp i (0..kStamps-1) into the current slot. Read back with GpuStamps for a retired frame.
void GpuStamp(Gpu& g, ID3D12GraphicsCommandList* cl, int i);
// Fill ms[i] with (stamp[i+1] - stamp[i]) in milliseconds for the most recently RETIRED slot;
// pairs where either stamp was not written give -1. Returns false if no frame has retired yet.
bool GpuStampsMs(Gpu& g, double* ms, int pairs);
// Same for one specific ring slot; false if that slot's fence has not completed.
bool GpuStampsMsSlot(Gpu& g, int slot, double* ms, int pairs);
// Same three for a secondary context (stamps go into c.list's current slot).
void GpuCtxStamp(GpuCtx& c, int i);
bool GpuCtxStampsMsSlot(GpuCtx& c, int slot, double* ms, int pairs);

// ---- misc ------------------------------------------------------------------------------------
double NowMs();   // QPC milliseconds
double QpcToMs(LONGLONG qpc_ticks);   // same clock as NowMs
double Pct(std::vector<double> v, double p);   // sorted v[(size_t)(p * n)], clamped to the last; -1 when empty
const char* NgxResultName(unsigned r);

// ---- process (the exes' shared startup) ---------------------------------------------------------
std::wstring ExeDir();   // the exe's folder, no trailing backslash
// Windowed subsystem: when launched from a console, send stdout/stderr there.
void AttachParentConsole();
// __argv is NULL under a wide entry point (the UCRT only builds __wargv): a narrow copy of the command line,
// argv null-terminated and pointing into args. Returns argc.
int NarrowArgs(std::vector<std::string>& args, std::vector<char*>& argv);
// One live instance: the held named mutex, or (another instance runs) nullptr after telling the user.
HANDLE SingleInstance(const wchar_t* mutex_name, const wchar_t* message, const wchar_t* title);
