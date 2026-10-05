#include "d3d.h"
#include "log.h"
#include <algorithm>
#include <cstring>

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

// ---- rings (shared by Gpu and GpuCtx) ---------------------------------------------------------
static bool WaitFence(ID3D12Fence* f, UINT64 v, HANDLE ev, DWORD ms, bool& failed)
{
    if (v == 0) return true;
    UINT64 done = f->GetCompletedValue();
    if (done == UINT64_MAX) { failed = true; return false; }
    if (done >= v) return true;
    // One auto-reset event serves every wait and a stale registration can wake us early: re-check the
    // value on every wake and keep waiting until the deadline.
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

// name = nullptr: the main ring (Gpu); otherwise a GpuCtx, whose log lines say "ctx <name>:".
static bool RingInit(Gpu& g, GpuRing& r, D3D12_COMMAND_QUEUE_PRIORITY priority, const wchar_t* name)
{
    char pre[64] = ""; if (name) snprintf(pre, sizeof pre, "ctx %ls: ", name);
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Priority = priority;
    if (FAILED(g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&r.queue))) { Log("[gpu] %squeue failed", pre); return false; }
    if (name) r.queue->SetName(name);
    for (int i = 0; i < Gpu::kFrames; ++i)
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&r.alloc[i]);
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&r.list);
    if (!r.list) { Log("[gpu] %slist failed", pre); return false; }
    r.list->Close();
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&r.fence);
    r.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Gpu::kFrames * Gpu::kDescPerSlot;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&r.desc_heap))) { Log("[gpu] %sdesc heap failed", pre); return false; }
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = Gpu::kFrames * Gpu::kStamps;
    g.dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void**)&r.ts_heap);
    r.ts_readback = GpuMakeBuffer(g, (UINT64)Gpu::kFrames * Gpu::kStamps * 8, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, name ? L"ctx_ts_readback" : L"ts_readback");
    r.queue->GetTimestampFrequency(&r.ts_freq);
    return true;
}

static void RingShutdown(GpuRing& r)
{
    if (r.queue && r.fence) WaitFence(r.fence, r.fence_value, r.event, 5000, r.failed);
    REL(r.ts_readback); REL(r.ts_heap); REL(r.desc_heap); REL(r.list);
    for (int i = 0; i < Gpu::kFrames; ++i) REL(r.alloc[i]);
    REL(r.fence); REL(r.queue);
    if (r.event) { CloseHandle(r.event); r.event = nullptr; }
}

static bool RingBegin(Gpu& g, GpuRing& r)
{
    const bool ctx = &r != &g;
    if (r.failed || !r.list) return false;
    const UINT64 retire = r.alloc_fence[r.slot];
    if (retire && !WaitFence(r.fence, retire, r.event, 2000, r.failed)) { Log("[gpu] %sslot %d did not retire", ctx ? "ctx " : "", r.slot); return false; }
    if (FAILED(r.alloc[r.slot]->Reset())) { GpuLogDeviceRemoved(g, ctx ? "ctx allocator reset" : "allocator reset"); r.failed = true; return false; }
    if (FAILED(r.list->Reset(r.alloc[r.slot], nullptr))) { GpuLogDeviceRemoved(g, ctx ? "ctx list reset" : "list reset"); r.failed = true; return false; }
    r.desc_used = 0;
    memset(r.ts_written[r.slot], 0, sizeof r.ts_written[r.slot]);
    ID3D12DescriptorHeap* heaps[] = { r.desc_heap };
    r.list->SetDescriptorHeaps(1, heaps);
    return true;
}

static UINT64 RingEnd(GpuRing& r, const char* pre)
{
    if (r.ts_heap)
        r.list->ResolveQueryData(r.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, r.slot * Gpu::kStamps, Gpu::kStamps,
                                 r.ts_readback, (UINT64)r.slot * Gpu::kStamps * 8);
    const HRESULT closed = r.list->Close();
    if (FAILED(closed)) { Log("[gpu] %sClose failed 0x%08X", pre, closed); r.failed = true; return 0; }
    ID3D12CommandList* lists[] = { r.list };
    r.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++r.fence_value;
    if (FAILED(r.queue->Signal(r.fence, v))) { Log("[gpu] %sSignal failed", pre); r.failed = true; return 0; }
    r.alloc_fence[r.slot] = v;
    r.slot = (r.slot + 1) % Gpu::kFrames;
    return v;
}

// ---------------------------------------------------------------------------------------------
bool GpuInit(Gpu& g, int want, UINT vendor)
{
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&g.factory);
    if (FAILED(hr)) { Log("[gpu] CreateDXGIFactory2 failed 0x%08X", hr); return false; }
    IDXGIAdapter1* pick = nullptr;
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (g.factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d);
        const bool usable = (!vendor || d.VendorId == vendor) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        Log("[gpu] adapter %u: %ls vram=%lluMB luid=%08X:%08X%s", i, d.Description,
            (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned)d.AdapterLuid.HighPart,
            (unsigned)d.AdapterLuid.LowPart, usable ? "" : " (skipped)");
        if (pick == nullptr && usable && (want < 0 || (int)i == want)) { pick = a; g.adapter_index = (int)i; g.luid = d.AdapterLuid; continue; }
        a->Release();
    }
    if (!pick)
    {
        if (want >= 0) Log("[gpu] [gpu] adapter=%d is not a usable adapter (vendor %04X) - see the indices listed above", want, vendor);
        else Log("[gpu] no usable adapter (vendor %04X)", vendor);
        return false;
    }
    pick->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g.adapter);
    hr = D3D12CreateDevice(pick, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&g.dev);
    pick->Release();
    if (FAILED(hr)) { Log("[gpu] D3D12CreateDevice failed 0x%08X", hr); return false; }
    g.dev->QueryInterface(__uuidof(ID3D12Device1), (void**)&g.dev1);   // optional: residency priority

    ID3D12DeviceRemovedExtendedDataSettings1* dred = nullptr;
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1), (void**)&dred)))
    {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
    }

    if (!RingInit(g, g, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, nullptr)) return false;
    g.desc_size = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Log("[gpu] device ready on adapter %d (luid %08X:%08X), ts freq %llu", g.adapter_index,
        (unsigned)g.luid.HighPart, (unsigned)g.luid.LowPart, (unsigned long long)g.ts_freq);
    return true;
}

void GpuShutdown(Gpu& g)
{
    RingShutdown(g);
    if (g.adapter) g.adapter->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 0);
    REL(g.dev1); REL(g.dev); REL(g.adapter); REL(g.factory);
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

bool GpuWait(Gpu& g, ID3D12Fence* f, UINT64 v, DWORD ms) { return WaitFence(f, v, g.event, ms, g.failed); }
bool GpuCtxWait(GpuCtx& c, ID3D12Fence* f, UINT64 v, DWORD ms) { return WaitFence(f, v, c.event, ms, c.failed); }
bool GpuBegin(Gpu& g) { return RingBegin(g, g); }
UINT64 GpuEnd(Gpu& g) { return RingEnd(g, ""); }

// Everything we create is marked HIGH residency priority: when the game fills video memory (on an iGPU,
// the shared system memory), the OS evicts lower-priority resources first instead of paging the frame
// generator's textures out mid-frame. Reservation (below) tells it how much we need.
static void HighResidency(Gpu& g, ID3D12Resource* r)
{
    if (!g.dev1 || !r) return;
    ID3D12Pageable* pg = r; const D3D12_RESIDENCY_PRIORITY pr = D3D12_RESIDENCY_PRIORITY_HIGH;
    g.dev1->SetResidencyPriority(1, &pg, &pr);
}

double GpuReserveCurrentUsage(Gpu& g)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (!g.adapter || FAILED(g.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) return -1;
    const UINT64 want = std::min<UINT64>(vm.CurrentUsage, vm.AvailableForReservation);
    if (FAILED(g.adapter->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, want))) return -1;
    return (double)want / (1024.0 * 1024.0);
}

// CurrentUsage counts every D3D12 resource we hold: a leak shows up as a number that never comes back down.
bool GpuVram(Gpu& g, double& used_mb, double& budget_mb)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (!g.adapter || FAILED(g.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) return false;
    used_mb = (double)vm.CurrentUsage / (1024.0 * 1024.0);
    budget_mb = (double)vm.Budget / (1024.0 * 1024.0);
    return true;
}

// ---------------------------------------------------------------------------------------------
bool GpuCtxInit(Gpu& g, GpuCtx& c, D3D12_COMMAND_QUEUE_PRIORITY priority, const wchar_t* name)
{
    int reg = -1;
    for (int i = 0; i < Gpu::kCtx; ++i) if (!g.ctx[i]) { reg = i; break; }
    if (reg < 0) { Log("[gpu] ctx %ls: no free registry slot", name); return false; }
    if (!RingInit(g, c, priority, name)) return false;
    g.ctx[reg] = &c;
    Log("[gpu] ctx %ls ready (priority %d)", name, (int)priority);
    return true;
}

void GpuCtxShutdown(Gpu& g, GpuCtx& c)
{
    for (int i = 0; i < Gpu::kCtx; ++i) if (g.ctx[i] == &c) g.ctx[i] = nullptr;
    RingShutdown(c);   // waits for the queue first
}

bool GpuCtxBegin(Gpu& g, GpuCtx& c) { return RingBegin(g, c); }
UINT64 GpuCtxEnd(GpuCtx& c) { return RingEnd(c, "ctx "); }

// ---------------------------------------------------------------------------------------------
static ID3D12Resource* Committed(Gpu& g, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES initial, const wchar_t* name, HRESULT& hr)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
    ID3D12Resource* r = nullptr;
    hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr, __uuidof(ID3D12Resource), (void**)&r);
    if (FAILED(hr)) return nullptr;
    if (name) r->SetName(name);
    HighResidency(g, r);
    return r;
}

ID3D12Resource* GpuMakeTex(Gpu& g, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES initial, const wchar_t* name)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
    HRESULT hr; ID3D12Resource* r = Committed(g, D3D12_HEAP_TYPE_DEFAULT, d, initial, name, hr);
    if (!r) Log("[gpu] texture %ls %ux%u fmt %d failed 0x%08X", name, w, h, (int)fmt, hr);
    return r;
}

ID3D12Resource* GpuMakeBuffer(Gpu& g, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial,
                              D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    HRESULT hr; ID3D12Resource* r = Committed(g, heap, d, initial, name, hr);
    if (!r) Log("[gpu] buffer %ls %llu bytes failed 0x%08X", name, (unsigned long long)bytes, hr);
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

void GpuCopyToReadback(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, ID3D12Resource* rb, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, const D3D12_BOX* box)
{
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { fmt, w, h, 1, pitch };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, box);
}

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
    s.PlacedFootprint.Footprint = { tex->GetDesc().Format, w, h, 1, pitch };
    d.pResource = tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
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
    GpuCopyToReadback(g.list, tex, rb, tex->GetDesc().Format, w, h, pitch);
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
    if (num_srv == 0) params[1] = params[2];
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1 + (num_srv ? 1 : 0) + (num_uav ? 1 : 0); rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
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

// Writes ns SRVs then nu UAVs into the next descriptors of cl's ring (a registered GpuCtx's list -> that
// ctx's, else Gpu's) and binds its heap on cl. table = the first; false = the ring slot is exhausted.
static bool Views(Gpu& g, ID3D12GraphicsCommandList* cl, const GpuView* srvs, UINT ns, const GpuView* uavs, UINT nu, D3D12_GPU_DESCRIPTOR_HANDLE& table)
{
    GpuRing* r = &g;
    for (GpuCtx* c : g.ctx) if (c && c->list == cl) r = c;
    if (r->desc_used + ns + nu > Gpu::kDescPerSlot) { Log("[gpu] descriptor ring exhausted"); r->failed = true; return false; }
    const UINT base = r->slot * Gpu::kDescPerSlot + r->desc_used;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = r->desc_heap->GetCPUDescriptorHandleForHeapStart();
    table = r->desc_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)base * g.desc_size; table.ptr += (UINT64)base * g.desc_size;
    for (UINT i = 0; i < ns + nu; ++i, cpu.ptr += g.desc_size)
    {
        const GpuView& v = i < ns ? srvs[i] : uavs[i - ns];
        const DXGI_FORMAT fmt = v.fmt == DXGI_FORMAT_UNKNOWN ? v.res->GetDesc().Format : v.fmt;
        if (i < ns)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
            d.Format = fmt; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = 1;
            g.dev->CreateShaderResourceView(v.res, &d, cpu);
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
            d.Format = fmt; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g.dev->CreateUnorderedAccessView(v.res, nullptr, &d, cpu);
        }
    }
    r->desc_used += ns + nu;
    // Every call, not once in GpuBegin: an NGX EvaluateFeature recorded into the same list binds its own
    // heaps (command-list state), and the root tables set after it must point into ours. A redundant bind is free.
    ID3D12DescriptorHeap* heaps[] = { r->desc_heap };
    cl->SetDescriptorHeaps(1, heaps);
    return true;
}

void GpuDispatch(Gpu& g, ID3D12GraphicsCommandList* cl, const ComputePso& p, const GpuView* srvs, const GpuView* uavs,
                 const void* consts, UINT gx, UINT gy, UINT gz)
{
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table;
    if (!Views(g, cl, srvs, p.num_srv, uavs, p.num_uav, srv_table)) return;
    cl->SetComputeRootSignature(p.root);
    cl->SetPipelineState(p.pso);
    if (p.num_consts) cl->SetComputeRoot32BitConstants(0, p.num_consts, consts, 0);
    UINT idx = 1;
    if (p.num_srv) cl->SetComputeRootDescriptorTable(idx++, srv_table);
    if (p.num_uav) cl->SetComputeRootDescriptorTable(idx++, { srv_table.ptr + (UINT64)p.num_srv * g.desc_size });
    cl->Dispatch(gx, gy, gz);
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuSrvTable(Gpu& g, ID3D12GraphicsCommandList* cl, const GpuView* srvs, UINT n)
{
    D3D12_GPU_DESCRIPTOR_HANDLE table = {};
    Views(g, cl, srvs, n, nullptr, 0, table);   // left 0 when exhausted
    return table;
}

// ---------------------------------------------------------------------------------------------
static bool Retired(const GpuRing& r, int s) { const UINT64 v = r.alloc_fence[s]; return v && r.fence->GetCompletedValue() >= v; }

static bool ReadStamps(GpuRing& r, int s, double* ms, int pairs)
{
    UINT64* data = nullptr;
    D3D12_RANGE rr = { (SIZE_T)s * Gpu::kStamps * 8, (SIZE_T)(s + 1) * Gpu::kStamps * 8 };
    if (FAILED(r.ts_readback->Map(0, &rr, (void**)&data))) return false;
    const UINT64* st = data + s * Gpu::kStamps;
    const bool* wr = r.ts_written[s];
    for (int i = 0; i < pairs; ++i)
    {
        const int a = 2 * i, b = 2 * i + 1;
        ms[i] = (b < Gpu::kStamps && wr[a] && wr[b] && st[b] >= st[a]) ? (double)(st[b] - st[a]) * 1000.0 / (double)r.ts_freq : -1.0;
    }
    D3D12_RANGE none = { 0, 0 }; r.ts_readback->Unmap(0, &none);
    return true;
}

static void Stamp(GpuRing& r, ID3D12GraphicsCommandList* cl, int i)
{
    if (!r.ts_heap || i < 0 || i >= Gpu::kStamps) return;
    cl->EndQuery(r.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, r.slot * Gpu::kStamps + i);
    r.ts_written[r.slot][i] = true;
}

void GpuStamp(Gpu& g, ID3D12GraphicsCommandList* cl, int i) { Stamp(g, cl, i); }
void GpuCtxStamp(GpuCtx& c, int i) { Stamp(c, c.list, i); }
bool GpuStampsMsSlot(Gpu& g, int s, double* ms, int pairs) { return Retired(g, s) && ReadStamps(g, s, ms, pairs); }
bool GpuCtxStampsMsSlot(GpuCtx& c, int s, double* ms, int pairs) { return Retired(c, s) && ReadStamps(c, s, ms, pairs); }

bool GpuStampsMs(Gpu& g, double* ms, int pairs)
{
    for (int k = 1; k <= Gpu::kFrames; ++k)   // newest first: the slot before the current one
        if (const int s = (g.slot + Gpu::kFrames - k) % Gpu::kFrames; Retired(g, s)) return ReadStamps(g, s, ms, pairs);
    return false;
}
