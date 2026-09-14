// nrfilter D3D12 primitives shared by every module: device + queue, a 3-deep allocator ring with
// one fence, texture creation, barriers, a shader-visible descriptor ring, compute PSO helpers and
// GPU timestamps. Everything is plain C-style over raw COM pointers; no smart pointers on purpose.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

struct Gpu
{
    static const int kFrames = 3;
    static const int kStamps = 32;             // timestamp slots per frame (pairs: even=begin, odd=end)
    static const UINT kDescPerSlot = 1024;     // shader-visible CBV/SRV/UAV descriptors per ring slot

    IDXGIFactory4*             factory = nullptr;
    IDXGIAdapter3*             adapter = nullptr;
    LUID                       luid = {};
    int                        adapter_index = -1;
    ID3D12Device*              dev = nullptr;
    ID3D12CommandQueue*        queue = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandAllocator*    alloc[kFrames] = {};
    UINT64                     alloc_fence[kFrames] = {};
    int                        slot = 0;         // current ring slot (valid between GpuBegin/GpuEnd)
    ID3D12Fence*               fence = nullptr;
    HANDLE                     fence_event = nullptr;
    UINT64                     fence_value = 0;
    bool                       failed = false;   // a submission failed or the device was removed

    // descriptors
    ID3D12DescriptorHeap*      desc_heap = nullptr;     // shader visible, kFrames * kDescPerSlot
    UINT                       desc_size = 0;
    UINT                       desc_used = 0;           // within the current slot

    // timestamps
    ID3D12QueryHeap*           ts_heap = nullptr;       // kFrames * kStamps
    ID3D12Resource*            ts_readback = nullptr;   // kFrames * kStamps * 8 bytes
    UINT64                     ts_freq = 0;
    bool                       ts_written[kFrames][kStamps] = {};
};

// ---- lifecycle -------------------------------------------------------------------------------
bool   GpuInit(Gpu& g, int adapter_index /* -1 = first NVIDIA */);
void   GpuShutdown(Gpu& g);
// Waits for the ring slot, resets allocator + list, resets descriptor ring for the slot.
bool   GpuBegin(Gpu& g);
// Resolves timestamps, closes, executes, signals. Returns the fence value (0 on failure).
UINT64 GpuEnd(Gpu& g);
bool   GpuWait(Gpu& g, ID3D12Fence* f, UINT64 v, DWORD ms);
inline bool GpuWaitIdle(Gpu& g, DWORD ms = 5000) { return GpuWait(g, g.fence, g.fence_value, ms); }
void   GpuLogDeviceRemoved(Gpu& g, const char* where);

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
void GpuDispatch(Gpu& g, ID3D12GraphicsCommandList* cl, const ComputePso& p, const GpuView* srvs, const GpuView* uavs,
                 const void* consts, UINT groups_x, UINT groups_y, UINT groups_z = 1);
inline UINT GpuGroups(UINT n, UINT size) { return (n + size - 1) / size; }

// ---- timestamps ---------------------------------------------------------------------------------
// Write timestamp i (0..kStamps-1) into the current slot. Read back with GpuStamps for a retired frame.
void GpuStamp(Gpu& g, ID3D12GraphicsCommandList* cl, int i);
// Fill ms[i] with (stamp[i+1] - stamp[i]) in milliseconds for the most recently RETIRED slot;
// pairs where either stamp was not written give -1. Returns false if no frame has retired yet.
bool GpuStampsMs(Gpu& g, double* ms, int pairs);
// Same for one specific ring slot; false if that slot's fence has not completed.
bool GpuStampsMsSlot(Gpu& g, int slot, double* ms, int pairs);

// ---- misc ------------------------------------------------------------------------------------
double NowMs();   // QPC milliseconds
const char* NgxResultName(unsigned r);
