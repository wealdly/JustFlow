#include "d3d.h"
#include "log.h"
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

double QpcToMs(LONGLONG qpc)
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return (double)qpc * 1000.0 / (double)f.QuadPart;
}

double NowMs()
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return QpcToMs(c.QuadPart);
}

const char* NgxResultName(unsigned r)
{
    switch (r)
    {
    case 0x1:        return "Success";
    case 0xBAD00000: return "FAIL";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00003: return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004: return "FAIL_FeatureNotFound";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00006: return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007: return "FAIL_NotInitialized";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD0000F: return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    case 0xBAD00012: return "FAIL_NotImplemented";
    default:         return "unknown";
    }
}

// ---------------------------------------------------------------------------------------------
bool GpuInit(Gpu& g, int want)
{
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&g.factory);
    if (FAILED(hr)) { Log("[gpu] CreateDXGIFactory2 failed 0x%08X", hr); return false; }
    IDXGIAdapter1* pick = nullptr;
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (g.factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d);
        const bool usable = d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        Log("[gpu] adapter %u: %ls vram=%lluMB luid=%08X:%08X%s", i, d.Description,
            (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned)d.AdapterLuid.HighPart,
            (unsigned)d.AdapterLuid.LowPart, usable ? "" : " (skipped)");
        if (pick == nullptr && usable && (want < 0 || (int)i == want)) { pick = a; g.adapter_index = (int)i; g.luid = d.AdapterLuid; continue; }
        a->Release();
    }
    if (!pick)
    {
        if (want >= 0) Log("[gpu] [gpu] adapter=%d is not a usable NVIDIA adapter - see the indices listed above", want);
        else Log("[gpu] no usable NVIDIA adapter");
        return false;
    }
    pick->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g.adapter);
    hr = D3D12CreateDevice(pick, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&g.dev);
    pick->Release();
    if (FAILED(hr)) { Log("[gpu] D3D12CreateDevice failed 0x%08X", hr); return false; }

    ID3D12DeviceRemovedExtendedDataSettings1* dred = nullptr;
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1), (void**)&dred)))
    {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    if (FAILED(g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g.queue))) { Log("[gpu] queue failed"); return false; }
    for (int i = 0; i < Gpu::kFrames; ++i)
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g.alloc[i]);
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g.list);
    if (!g.list) { Log("[gpu] list failed"); return false; }
    g.list->Close();
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g.fence);
    g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Gpu::kFrames * Gpu::kDescPerSlot;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g.desc_heap))) { Log("[gpu] desc heap failed"); return false; }
    g.desc_size = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = Gpu::kFrames * Gpu::kStamps;
    g.dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void**)&g.ts_heap);
    g.ts_readback = GpuMakeBuffer(g, (UINT64)Gpu::kFrames * Gpu::kStamps * 8, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"ts_readback");
    g.queue->GetTimestampFrequency(&g.ts_freq);
    Log("[gpu] device ready on adapter %d (luid %08X:%08X), ts freq %llu", g.adapter_index,
        (unsigned)g.luid.HighPart, (unsigned)g.luid.LowPart, (unsigned long long)g.ts_freq);
    return true;
}

void GpuShutdown(Gpu& g)
{
    if (g.queue && g.fence) GpuWaitIdle(g, 5000);
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
    REL(g.ts_readback); REL(g.ts_heap); REL(g.desc_heap); REL(g.list);
    for (int i = 0; i < Gpu::kFrames; ++i) REL(g.alloc[i]);
    REL(g.fence); REL(g.queue); REL(g.dev); REL(g.adapter); REL(g.factory);
#undef REL
    if (g.fence_event) { CloseHandle(g.fence_event); g.fence_event = nullptr; }
}

void GpuLogDeviceRemoved(Gpu& g, const char* where)
{
    if (!g.dev) return;
    const HRESULT reason = g.dev->GetDeviceRemovedReason();
    Log("[gpu] device removed at %s: reason 0x%08X", where, (unsigned)reason);
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(g.dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1), (void**)&dred))) return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)) && crumbs.pHeadAutoBreadcrumbNode)
    {
        const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode;
        Log("[gpu] DRED: %u breadcrumbs, last value %u", n->BreadcrumbCount, n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA) Log("[gpu] DRED: page fault at VA 0x%llX", (unsigned long long)pf.PageFaultVA);
    dred->Release();
}

static bool WaitFence(ID3D12Fence* f, UINT64 v, HANDLE ev, DWORD ms, bool& failed)
{
    if (v == 0) return true;
    UINT64 done = f->GetCompletedValue();
    if (done == UINT64_MAX) { failed = true; return false; }
    if (done >= v) return true;
    // One auto-reset event serves every wait; a stale registration can wake us early, so re-check
    // the value on every wake and keep waiting until the deadline (NeuralScreen issue #33 lesson).
    ResetEvent(ev);
    if (f->GetCompletedValue() >= v) return true;
    if (FAILED(f->SetEventOnCompletion(v, ev))) return false;
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= deadline ? 0 : (DWORD)(deadline - now);
        if (WaitForSingleObject(ev, left) != WAIT_OBJECT_0) return false;
        done = f->GetCompletedValue();
        if (done == UINT64_MAX) { failed = true; return false; }
        if (done >= v) return true;
    }
}

bool GpuWait(Gpu& g, ID3D12Fence* f, UINT64 v, DWORD ms) { return WaitFence(f, v, g.fence_event, ms, g.failed); }

// Driver-reported local (device) video memory for THIS process. CurrentUsage counts every D3D12
// resource we hold, so a leak shows up here as a number that climbs and never comes back down.
bool GpuVram(Gpu& g, double& used_mb, double& budget_mb)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (!g.adapter || FAILED(g.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) return false;
    used_mb = (double)vm.CurrentUsage / (1024.0 * 1024.0);
    budget_mb = (double)vm.Budget / (1024.0 * 1024.0);
    return true;
}
bool GpuCtxWait(GpuCtx& c, ID3D12Fence* f, UINT64 v, DWORD ms) { return WaitFence(f, v, c.event, ms, c.failed); }

bool GpuBegin(Gpu& g)
{
    if (g.failed || !g.list) return false;
    const UINT64 retire = g.alloc_fence[g.slot];
    if (retire && !GpuWait(g, g.fence, retire, 2000)) { Log("[gpu] slot %d did not retire", g.slot); return false; }
    if (FAILED(g.alloc[g.slot]->Reset())) { GpuLogDeviceRemoved(g, "allocator reset"); g.failed = true; return false; }
    if (FAILED(g.list->Reset(g.alloc[g.slot], nullptr))) { GpuLogDeviceRemoved(g, "list reset"); g.failed = true; return false; }
    g.desc_used = 0;
    memset(g.ts_written[g.slot], 0, sizeof g.ts_written[g.slot]);
    ID3D12DescriptorHeap* heaps[] = { g.desc_heap };
    g.list->SetDescriptorHeaps(1, heaps);
    return true;
}

UINT64 GpuEnd(Gpu& g)
{
    if (g.ts_heap)
        g.list->ResolveQueryData(g.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * Gpu::kStamps, Gpu::kStamps,
                                 g.ts_readback, (UINT64)g.slot * Gpu::kStamps * 8);
    const HRESULT closed = g.list->Close();
    if (FAILED(closed)) { Log("[gpu] Close failed 0x%08X", closed); g.failed = true; return 0; }
    ID3D12CommandList* lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g.fence_value;
    if (FAILED(g.queue->Signal(g.fence, v))) { Log("[gpu] Signal failed"); g.failed = true; return 0; }
    g.alloc_fence[g.slot] = v;
    g.slot = (g.slot + 1) % Gpu::kFrames;
    return v;
}

// ---------------------------------------------------------------------------------------------
bool GpuCtxInit(Gpu& g, GpuCtx& c, D3D12_COMMAND_QUEUE_PRIORITY priority, const wchar_t* name)
{
    int reg = -1;
    for (int i = 0; i < Gpu::kCtx; ++i) if (!g.ctx[i]) { reg = i; break; }
    if (reg < 0) { Log("[gpu] ctx %ls: no free registry slot", name); return false; }
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Priority = priority;
    if (FAILED(g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&c.queue))) { Log("[gpu] ctx %ls: queue failed", name); return false; }
    c.queue->SetName(name);
    for (int i = 0; i < Gpu::kFrames; ++i)
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&c.alloc[i]);
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&c.list);
    if (!c.list) { Log("[gpu] ctx %ls: list failed", name); return false; }
    c.list->Close();
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&c.fence);
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Gpu::kFrames * Gpu::kDescPerSlot;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&c.desc_heap))) { Log("[gpu] ctx %ls: desc heap failed", name); return false; }
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = Gpu::kFrames * Gpu::kStamps;
    g.dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void**)&c.ts_heap);
    c.ts_readback = GpuMakeBuffer(g, (UINT64)Gpu::kFrames * Gpu::kStamps * 8, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"ctx_ts_readback");
    c.queue->GetTimestampFrequency(&c.ts_freq);
    g.ctx[reg] = &c;
    Log("[gpu] ctx %ls ready (priority %d)", name, (int)priority);
    return true;
}

void GpuCtxShutdown(Gpu& g, GpuCtx& c)
{
    if (c.queue && c.fence) GpuCtxWaitIdle(c, 5000);
    for (int i = 0; i < Gpu::kCtx; ++i) if (g.ctx[i] == &c) g.ctx[i] = nullptr;
#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
    REL(c.ts_readback); REL(c.ts_heap); REL(c.desc_heap); REL(c.list);
    for (int i = 0; i < Gpu::kFrames; ++i) REL(c.alloc[i]);
    REL(c.fence); REL(c.queue);
#undef REL
    if (c.event) { CloseHandle(c.event); c.event = nullptr; }
}

bool GpuCtxBegin(Gpu& g, GpuCtx& c)
{
    if (c.failed || !c.list) return false;
    const UINT64 retire = c.alloc_fence[c.slot];
    if (retire && !GpuCtxWait(c, c.fence, retire, 2000)) { Log("[gpu] ctx slot %d did not retire", c.slot); return false; }
    if (FAILED(c.alloc[c.slot]->Reset())) { GpuLogDeviceRemoved(g, "ctx allocator reset"); c.failed = true; return false; }
    if (FAILED(c.list->Reset(c.alloc[c.slot], nullptr))) { GpuLogDeviceRemoved(g, "ctx list reset"); c.failed = true; return false; }
    c.desc_used = 0;
    memset(c.ts_written[c.slot], 0, sizeof c.ts_written[c.slot]);
    ID3D12DescriptorHeap* heaps[] = { c.desc_heap };
    c.list->SetDescriptorHeaps(1, heaps);
    return true;
}

UINT64 GpuCtxEnd(GpuCtx& c)
{
    if (c.ts_heap)
        c.list->ResolveQueryData(c.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, c.slot * Gpu::kStamps, Gpu::kStamps,
                                 c.ts_readback, (UINT64)c.slot * Gpu::kStamps * 8);
    const HRESULT closed = c.list->Close();
    if (FAILED(closed)) { Log("[gpu] ctx Close failed 0x%08X", closed); c.failed = true; return 0; }
    ID3D12CommandList* lists[] = { c.list };
    c.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++c.fence_value;
    if (FAILED(c.queue->Signal(c.fence, v))) { Log("[gpu] ctx Signal failed"); c.failed = true; return 0; }
    c.alloc_fence[c.slot] = v;
    c.slot = (c.slot + 1) % Gpu::kFrames;
    return v;
}

// ---------------------------------------------------------------------------------------------
ID3D12Resource* GpuMakeTex(Gpu& g, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES initial, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr, __uuidof(ID3D12Resource), (void**)&r);
    if (FAILED(hr)) { Log("[gpu] texture %ls %ux%u fmt %d failed 0x%08X", name, w, h, (int)fmt, hr); return nullptr; }
    if (name) r->SetName(name);
    return r;
}

ID3D12Resource* GpuMakeBuffer(Gpu& g, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial,
                              D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr, __uuidof(ID3D12Resource), (void**)&r);
    if (FAILED(hr)) { Log("[gpu] buffer %ls %llu bytes failed 0x%08X", name, (unsigned long long)bytes, hr); return nullptr; }
    if (name) r->SetName(name);
    return r;
}

void GpuBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

void GpuUavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

static UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

bool GpuUploadTex(Gpu& g, ID3D12Resource* tex, const void* pixels, UINT w, UINT h, UINT bpp, D3D12_RESOURCE_STATES tex_state)
{
    const UINT pitch = AlignUp(w * bpp, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    ID3D12Resource* up = GpuMakeBuffer(g, (UINT64)pitch * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"upload");
    if (!up) return false;
    uint8_t* dst = nullptr; up->Map(0, nullptr, (void**)&dst);
    for (UINT y = 0; y < h; ++y) memcpy(dst + (size_t)y * pitch, (const uint8_t*)pixels + (size_t)y * w * bpp, (size_t)w * bpp);
    up->Unmap(0, nullptr);
    if (!GpuBegin(g)) { up->Release(); return false; }
    GpuBarrier(g.list, tex, tex_state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION s = {}, d = {};
    s.pResource = up; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint.Footprint.Format = tex->GetDesc().Format;
    s.PlacedFootprint.Footprint.Width = w; s.PlacedFootprint.Footprint.Height = h; s.PlacedFootprint.Footprint.Depth = 1;
    s.PlacedFootprint.Footprint.RowPitch = pitch;
    d.pResource = tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
    g.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    GpuBarrier(g.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, tex_state);
    const UINT64 v = GpuEnd(g);
    const bool ok = GpuWait(g, g.fence, v, 5000);
    up->Release();
    return ok;
}

bool GpuReadbackTex(Gpu& g, ID3D12Resource* tex, void* out, UINT w, UINT h, UINT bpp, D3D12_RESOURCE_STATES tex_state)
{
    const UINT pitch = AlignUp(w * bpp, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    ID3D12Resource* rb = GpuMakeBuffer(g, (UINT64)pitch * h, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"readback");
    if (!rb) return false;
    if (!GpuBegin(g)) { rb->Release(); return false; }
    GpuBarrier(g.list, tex, tex_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s = {}, d = {};
    s.pResource = tex; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Footprint.Format = tex->GetDesc().Format;
    d.PlacedFootprint.Footprint.Width = w; d.PlacedFootprint.Footprint.Height = h; d.PlacedFootprint.Footprint.Depth = 1;
    d.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    GpuBarrier(g.list, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, tex_state);
    const UINT64 v = GpuEnd(g);
    bool ok = GpuWait(g, g.fence, v, 5000);
    if (ok)
    {
        uint8_t* src = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)pitch * h };
        rb->Map(0, &rr, (void**)&src);
        for (UINT y = 0; y < h; ++y) memcpy((uint8_t*)out + (size_t)y * w * bpp, src + (size_t)y * pitch, (size_t)w * bpp);
        D3D12_RANGE none = { 0, 0 }; rb->Unmap(0, &none);
    }
    rb->Release();
    return ok;
}

// ---------------------------------------------------------------------------------------------
bool GpuMakeCompute(Gpu& g, const void* cso, size_t cso_len, UINT num_srv, UINT num_uav, UINT num_consts, ComputePso& out, const wchar_t* name)
{
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = num_srv; ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = num_uav; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = num_consts ? num_consts : 1;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1; params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    UINT used = 1;
    if (num_srv == 0) { params[1] = params[2]; used = num_uav ? 2 : 1; }
    else used = num_uav ? 3 : 2;
    rs.NumParameters = used;
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    { Log("[gpu] root signature %ls: %s", name, err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    HRESULT hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&out.root);
    blob->Release();
    if (FAILED(hr)) { Log("[gpu] CreateRootSignature %ls failed 0x%08X", name, hr); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = out.root; pd.CS.pShaderBytecode = cso; pd.CS.BytecodeLength = cso_len;
    hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&out.pso);
    if (FAILED(hr)) { Log("[gpu] compute PSO %ls failed 0x%08X", name, hr); return false; }
    out.pso->SetName(name);
    out.num_srv = num_srv; out.num_uav = num_uav; out.num_consts = num_consts;
    return true;
}

static void Dispatch(Gpu& g, ID3D12DescriptorHeap* heap, int slot, UINT& used, bool& failed, ID3D12GraphicsCommandList* cl,
                     const ComputePso& p, const GpuView* srvs, const GpuView* uavs, const void* consts, UINT gx, UINT gy, UINT gz)
{
    const UINT need = p.num_srv + p.num_uav;
    if (used + need > Gpu::kDescPerSlot) { Log("[gpu] descriptor ring exhausted"); failed = true; return; }
    const UINT base = slot * Gpu::kDescPerSlot + used;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)base * g.desc_size; gpu.ptr += (UINT64)base * g.desc_size;
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = gpu;
    for (UINT i = 0; i < p.num_srv; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = srvs[i].fmt == DXGI_FORMAT_UNKNOWN ? srvs[i].res->GetDesc().Format : srvs[i].fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(srvs[i].res, &d, cpu);
        cpu.ptr += g.desc_size; gpu.ptr += g.desc_size;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE uav_table = gpu;
    for (UINT i = 0; i < p.num_uav; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = uavs[i].fmt == DXGI_FORMAT_UNKNOWN ? uavs[i].res->GetDesc().Format : uavs[i].fmt;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->CreateUnorderedAccessView(uavs[i].res, nullptr, &d, cpu);
        cpu.ptr += g.desc_size; gpu.ptr += g.desc_size;
    }
    used += need;
    cl->SetComputeRootSignature(p.root);
    cl->SetPipelineState(p.pso);
    if (p.num_consts) cl->SetComputeRoot32BitConstants(0, p.num_consts, consts, 0);
    UINT idx = 1;
    if (p.num_srv) cl->SetComputeRootDescriptorTable(idx++, srv_table);
    if (p.num_uav) cl->SetComputeRootDescriptorTable(idx++, uav_table);
    cl->Dispatch(gx, gy, gz);
}

void GpuDispatch(Gpu& g, ID3D12GraphicsCommandList* cl, const ComputePso& p, const GpuView* srvs, const GpuView* uavs,
                 const void* consts, UINT gx, UINT gy, UINT gz)
{
    for (GpuCtx* c : g.ctx)
        if (c && c->list == cl) { Dispatch(g, c->desc_heap, c->slot, c->desc_used, c->failed, cl, p, srvs, uavs, consts, gx, gy, gz); return; }
    Dispatch(g, g.desc_heap, g.slot, g.desc_used, g.failed, cl, p, srvs, uavs, consts, gx, gy, gz);
}

// ---------------------------------------------------------------------------------------------
// Shared by Gpu and GpuCtx: ts_written is [kFrames][kStamps] flattened.
static bool ReadStamps(ID3D12Resource* rb, const bool* written, UINT64 freq, int s, double* ms, int pairs)
{
    UINT64* data = nullptr;
    D3D12_RANGE rr = { (SIZE_T)s * Gpu::kStamps * 8, (SIZE_T)(s + 1) * Gpu::kStamps * 8 };
    if (FAILED(rb->Map(0, &rr, (void**)&data))) return false;
    const UINT64* st = data + s * Gpu::kStamps;
    const bool* wr = written + s * Gpu::kStamps;
    for (int i = 0; i < pairs; ++i)
    {
        const int a = 2 * i, b = 2 * i + 1;
        ms[i] = (b < Gpu::kStamps && wr[a] && wr[b] && st[b] >= st[a]) ? (double)(st[b] - st[a]) * 1000.0 / (double)freq : -1.0;
    }
    D3D12_RANGE none = { 0, 0 }; rb->Unmap(0, &none);
    return true;
}

// most recently retired slot = the one before the current, if its fence completed; -1 if none
static int RetiredSlot(int cur, const UINT64* alloc_fence, ID3D12Fence* fence)
{
    int s = (cur + Gpu::kFrames - 1) % Gpu::kFrames;
    for (int tries = 0; tries < Gpu::kFrames; ++tries, s = (s + Gpu::kFrames - 1) % Gpu::kFrames)
    {
        const UINT64 v = alloc_fence[s];
        if (v && fence->GetCompletedValue() >= v) return s;
    }
    return -1;
}

void GpuStamp(Gpu& g, ID3D12GraphicsCommandList* cl, int i)
{
    if (!g.ts_heap || i < 0 || i >= Gpu::kStamps) return;
    cl->EndQuery(g.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * Gpu::kStamps + i);
    g.ts_written[g.slot][i] = true;
}

bool GpuStampsMs(Gpu& g, double* ms, int pairs)
{
    const int s = RetiredSlot(g.slot, g.alloc_fence, g.fence);
    return s >= 0 && ReadStamps(g.ts_readback, &g.ts_written[0][0], g.ts_freq, s, ms, pairs);
}

bool GpuStampsMsSlot(Gpu& g, int s, double* ms, int pairs)
{
    const UINT64 v = g.alloc_fence[s];
    if (!v || g.fence->GetCompletedValue() < v) return false;
    return ReadStamps(g.ts_readback, &g.ts_written[0][0], g.ts_freq, s, ms, pairs);
}

void GpuCtxStamp(GpuCtx& c, int i)
{
    if (!c.ts_heap || i < 0 || i >= Gpu::kStamps) return;
    c.list->EndQuery(c.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, c.slot * Gpu::kStamps + i);
    c.ts_written[c.slot][i] = true;
}

bool GpuCtxStampsMs(GpuCtx& c, double* ms, int pairs)
{
    const int s = RetiredSlot(c.slot, c.alloc_fence, c.fence);
    return s >= 0 && ReadStamps(c.ts_readback, &c.ts_written[0][0], c.ts_freq, s, ms, pairs);
}

bool GpuCtxStampsMsSlot(GpuCtx& c, int s, double* ms, int pairs)
{
    const UINT64 v = c.alloc_fence[s];
    if (!v || c.fence->GetCompletedValue() < v) return false;
    return ReadStamps(c.ts_readback, &c.ts_written[0][0], c.ts_freq, s, ms, pairs);
}
