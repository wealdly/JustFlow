// NVIDIA Optical Flow (SDK 5.0, D3D12 interface) session. Lifted from NeuralScreen nvofa.inl (MIT)
// minus env gates / dumps / fault injection; DLL search and grid selection are ours.
#include "nvofa.h"
#include "log.h"
#include "nvOpticalFlowD3D12.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

static const int kIn = 5, kOut = 3;   // input slots (0/1 per frame, 2..4 held); reg[] = inputs[0..kIn-1], then flow[i], cost[i] per pair
struct Ofa
{
    Gpu* g = nullptr;
    HMODULE lib = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST api = {};
    NvOFHandle session = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
    ID3D12Resource* inputs[kIn] = {};
    ID3D12Resource* flow[kOut] = {};
    ID3D12Resource* cost[kOut] = {};
    NvOFGPUBufferHandle reg[kIn + 2 * kOut] = {};
    UINT w = 0, h = 0, grid = 0, fw = 0, fh = 0;
    int current = 0;
    std::mutex mu;   // nvOFExecute + value from two threads (per-frame path, model track)
};

// ---- DLL search ---------------------------------------------------------------------------------
static UINT64 FileVersion(const wchar_t* path)
{
    const DWORD n = GetFileVersionInfoSizeW(path, nullptr);
    if (!n) return 0;
    std::vector<char> buf(n);
    if (!GetFileVersionInfoW(path, 0, n, buf.data())) return 0;
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return 0;
    return ((UINT64)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
}

static HMODULE LoadNvofa(const wchar_t* dll_override)
{
    HMODULE m = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m)
    {
        // DriverStore: every nv_dispi.inf_amd64_* package, newest nvofapi64.dll file version wins.
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring repo = std::wstring(sys) + L"\\DriverStore\\FileRepository\\";
        std::wstring best; UINT64 best_ver = 0;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((repo + L"nv_dispi.inf_amd64_*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::wstring p = repo + fd.cFileName + L"\\nvofapi64.dll";
                const UINT64 v = FileVersion(p.c_str());
                if (v > best_ver) { best_ver = v; best = p; }
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        if (!best.empty()) m = LoadLibraryExW(best.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    }
    if (!m && dll_override && *dll_override)
        m = LoadLibraryExW(dll_override, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!m) { Log("[ofa] nvofapi64.dll not found (System32, DriverStore, override)"); return nullptr; }
    wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(m, path, MAX_PATH);
    const UINT64 v = FileVersion(path);
    Log("[ofa] loaded %ls (%u.%u.%u.%u)", path, (unsigned)(v >> 48), (unsigned)(v >> 32) & 0xFFFF, (unsigned)(v >> 16) & 0xFFFF, (unsigned)v & 0xFFFF);
    return m;
}

// ---- session ------------------------------------------------------------------------------------
static bool Fail(Ofa* o, const char* what, NV_OF_STATUS st)
{
    char detail[512] = {}; uint32_t n = sizeof detail;
    if (o->session && o->api.nvOFGetLastError) o->api.nvOFGetLastError(o->session, detail, &n);
    Log("[ofa] %s failed (%u) %s", what, (unsigned)st, detail);
    return false;
}

static std::vector<uint32_t> Caps(Ofa* o, NV_OF_CAPS c)
{
    uint32_t n = 0;
    if (o->api.nvOFGetCaps(o->session, c, nullptr, &n) != NV_OF_SUCCESS || n == 0 || n > 128) return {};
    std::vector<uint32_t> v(n);
    if (o->api.nvOFGetCaps(o->session, c, v.data(), &n) != NV_OF_SUCCESS) return {};
    return v;
}

static bool FormatSupported(Ofa* o, NV_OF_BUFFER_USAGE usage, DXGI_FORMAT want, const char* name)
{
    uint32_t n = 0;
    if (o->api.nvOFGetSurfaceFormatCountD3D12(o->session, usage, NV_OF_MODE_OPTICALFLOW, &n) != NV_OF_SUCCESS || n == 0 || n > 128) return false;
    std::vector<DXGI_FORMAT> f(n);
    if (o->api.nvOFGetSurfaceFormatD3D12(o->session, usage, NV_OF_MODE_OPTICALFLOW, f.data()) != NV_OF_SUCCESS) return false;
    std::string list; for (DXGI_FORMAT x : f) list += std::to_string((int)x) + " ";
    const bool ok = std::find(f.begin(), f.end(), want) != f.end();
    Log("[ofa] %s formats: %s-> want %d %s", name, list.c_str(), (int)want, ok ? "ok" : "UNSUPPORTED");
    return ok;
}

Ofa* OfaCreate(Gpu& g, UINT w, UINT h, int grid, const wchar_t* dll_override)
{
    Ofa* o = new Ofa(); o->g = &g; o->w = w; o->h = h;
    o->lib = LoadNvofa(dll_override);
    if (!o->lib) { OfaDestroy(o); return nullptr; }
    auto create = (decltype(&NvOFAPICreateInstanceD3D12))GetProcAddress(o->lib, "NvOFAPICreateInstanceD3D12");
    if (!create) { Log("[ofa] NvOFAPICreateInstanceD3D12 missing"); OfaDestroy(o); return nullptr; }
    NV_OF_STATUS st = create(NV_OF_API_VERSION, &o->api);
    if (st != NV_OF_SUCCESS) { Fail(o, "NvOFAPICreateInstanceD3D12", st); OfaDestroy(o); return nullptr; }
    if (!o->api.nvCreateOpticalFlowD3D12 || !o->api.nvOFInit || !o->api.nvOFDestroy || !o->api.nvOFGetCaps ||
        !o->api.nvOFGetSurfaceFormatCountD3D12 || !o->api.nvOFGetSurfaceFormatD3D12 || !o->api.nvOFRegisterResourceD3D12 ||
        !o->api.nvOFUnregisterResourceD3D12 || !o->api.nvOFExecuteD3D12)
    { Log("[ofa] incomplete API table"); OfaDestroy(o); return nullptr; }
    st = o->api.nvCreateOpticalFlowD3D12(g.dev, &o->session);
    if (st != NV_OF_SUCCESS) { Fail(o, "nvCreateOpticalFlowD3D12", st); OfaDestroy(o); return nullptr; }

    // grid: smallest supported, or the requested one if supported
    const std::vector<uint32_t> grids = Caps(o, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
    if (grids.empty()) { Log("[ofa] no supported grid sizes"); OfaDestroy(o); return nullptr; }
    std::string gl; for (uint32_t x : grids) gl += std::to_string(x) + " ";
    o->grid = *std::min_element(grids.begin(), grids.end());
    if (grid > 0 && std::find(grids.begin(), grids.end(), (uint32_t)grid) != grids.end()) o->grid = (UINT)grid;
    else if (grid > 0) Log("[ofa] requested grid %d unsupported", grid);
    Log("[ofa] supported grids: %s-> using %u", gl.c_str(), o->grid);
    const std::vector<uint32_t> minw = Caps(o, NV_OF_CAPS_WIDTH_MIN), minh = Caps(o, NV_OF_CAPS_HEIGHT_MIN);
    const std::vector<uint32_t> maxw = Caps(o, NV_OF_CAPS_WIDTH_MAX), maxh = Caps(o, NV_OF_CAPS_HEIGHT_MAX);
    if (!minw.empty() && !minh.empty() && !maxw.empty() && !maxh.empty())
    {
        Log("[ofa] input range %ux%u .. %ux%u", minw[0], minh[0], maxw[0], maxh[0]);
        if (w < minw[0] || h < minh[0] || w > maxw[0] || h > maxh[0]) { Log("[ofa] %ux%u out of range", w, h); OfaDestroy(o); return nullptr; }
    }
    if (!FormatSupported(o, NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_R8_UNORM, "input") ||
        !FormatSupported(o, NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT, "flow") ||
        !FormatSupported(o, NV_OF_BUFFER_USAGE_COST, DXGI_FORMAT_R8_UINT, "cost"))
    { OfaDestroy(o); return nullptr; }

    NV_OF_INIT_PARAMS ip = {};
    ip.width = w; ip.height = h;
    ip.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)o->grid;
    ip.mode = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel = NV_OF_PERF_LEVEL_FAST;
    ip.enableOutputCost = NV_OF_TRUE;
    ip.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    st = o->api.nvOFInit(o->session, &ip);
    if (st != NV_OF_SUCCESS) { Fail(o, "nvOFInit", st); OfaDestroy(o); return nullptr; }

    if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&o->fence))) { Log("[ofa] fence failed"); OfaDestroy(o); return nullptr; }
    o->fw = (w + o->grid - 1) / o->grid; o->fh = (h + o->grid - 1) / o->grid;
    const wchar_t* in_names[kIn] = { L"ofa_in0", L"ofa_in1", L"ofa_in2", L"ofa_in3", L"ofa_in4" };
    const wchar_t* flow_names[kOut] = { L"ofa_flow", L"ofa_flow2", L"ofa_flow3" }, *cost_names[kOut] = { L"ofa_cost", L"ofa_cost2", L"ofa_cost3" };
    for (int i = 0; i < kIn; ++i)
        o->inputs[i] = GpuMakeTex(g, w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, in_names[i]);
    for (int i = 0; i < kOut; ++i)
    {
        o->flow[i] = GpuMakeTex(g, o->fw, o->fh, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, flow_names[i]);
        o->cost[i] = GpuMakeTex(g, o->fw, o->fh, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, cost_names[i]);
    }
    ID3D12Resource* res[kIn + 2 * kOut];
    for (int i = 0; i < kIn; ++i) res[i] = o->inputs[i];
    for (int i = 0; i < kOut; ++i) { res[kIn + 2 * i] = o->flow[i]; res[kIn + 2 * i + 1] = o->cost[i]; }
    for (int i = 0; i < kIn + 2 * kOut; ++i)
    {
        if (!res[i]) { OfaDestroy(o); return nullptr; }
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp = {};
        rp.resource = res[i]; rp.hOFGpuBuffer = &o->reg[i];
        rp.inputFencePoint = { g.fence, g.fence_value };
        rp.outputFencePoint = { o->fence, ++o->value };
        st = o->api.nvOFRegisterResourceD3D12(o->session, &rp);
        if (st != NV_OF_SUCCESS) { --o->value; Fail(o, "nvOFRegisterResourceD3D12", st); OfaDestroy(o); return nullptr; }
        if (!GpuWait(g, o->fence, o->value, 30000)) { Log("[ofa] register fence timeout"); OfaDestroy(o); return nullptr; }
    }
    Log("[ofa] active %ux%u grid=%u flow=%ux%u R16G16_SINT cost=R8_UINT perf=FAST", w, h, o->grid, o->fw, o->fh);
    return o;
}

void OfaDestroy(Ofa* o)
{
    if (!o) return;
    Gpu& g = *o->g;
    // Both queues must be done with the buffers before unregister/release (reference: RTX 5080 fault).
    if (g.queue && g.fence) GpuWaitIdle(g, 30000);
    if (o->fence) GpuWait(g, o->fence, o->value, 30000);
    if (o->session)
        for (NvOFGPUBufferHandle& b : o->reg) if (b)
        {
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 up = {}; up.hOFGpuBuffer = b;
            o->api.nvOFUnregisterResourceD3D12(&up); b = nullptr;
        }
    for (ID3D12Resource*& r : o->inputs) if (r) { r->Release(); r = nullptr; }
    for (ID3D12Resource*& r : o->flow) if (r) { r->Release(); r = nullptr; }
    for (ID3D12Resource*& r : o->cost) if (r) { r->Release(); r = nullptr; }
    if (o->session) { o->api.nvOFDestroy(o->session); o->session = nullptr; }
    if (o->fence) { o->fence->Release(); o->fence = nullptr; }
    if (o->lib) FreeLibrary(o->lib);
    delete o;
}

ID3D12Resource* OfaInput(Ofa* o, int which) { return o->inputs[std::clamp(which, 0, kIn - 1)]; }
int             OfaCurrent(Ofa* o)          { return o->current; }
ID3D12Fence*    OfaFence(Ofa* o)            { return o->fence; }
UINT64          OfaFenceValue(Ofa* o)       { return o->value; }
ID3D12Resource* OfaFlow(Ofa* o)             { return o->flow[0]; }
UINT            OfaFlowWidth(Ofa* o)        { return o->fw; }
UINT            OfaFlowHeight(Ofa* o)       { return o->fh; }
UINT            OfaGrid(Ofa* o)             { return o->grid; }
ID3D12Resource* OfaCost(Ofa* o)             { return o->cost[0]; }
ID3D12Resource* OfaFlow2(Ofa* o)            { return o->flow[1]; }
ID3D12Resource* OfaCost2(Ofa* o)            { return o->cost[1]; }
ID3D12Resource* OfaFlow3(Ofa* o)            { return o->flow[2]; }
ID3D12Resource* OfaCost3(Ofa* o)            { return o->cost[2]; }

UINT64 OfaExecuteRef(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, int input_idx, int ref_idx, int out_pair, bool reset)
{
    std::lock_guard<std::mutex> lk(o->mu);
    // reset: input == reference -> ~zero flow, and the expand pass zeroes it anyway.
    input_idx = std::clamp(input_idx, 0, kIn - 1); ref_idx = reset ? input_idx : std::clamp(ref_idx, 0, kIn - 1); out_pair = std::clamp(out_pair, 0, kOut - 1);
    NV_OF_FENCE_POINT ready = { in_fence, in_value }, done = { o->fence, ++o->value };
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in = {};
    in.inputFrame = o->reg[input_idx];
    in.referenceFrame = o->reg[ref_idx];
    in.disableTemporalHints = NV_OF_TRUE;
    in.numFencePoints = 1; in.fencePoint = &ready;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out = {};
    out.outputBuffer = o->reg[kIn + 2 * out_pair]; out.outputCostBuffer = o->reg[kIn + 2 * out_pair + 1]; out.fencePoint = &done;
    const NV_OF_STATUS st = o->api.nvOFExecuteD3D12(o->session, &in, &out);
    if (st != NV_OF_SUCCESS) { --o->value; Fail(o, "nvOFExecuteD3D12", st); return 0; }
    return o->value;
}

bool OfaExecute(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, bool reset)
{
    // current -> previous (NR convention) into pair 0
    if (!OfaExecuteRef(o, in_fence, in_value, o->current, 1 - o->current, 0, reset)) return false;
    o->current = 1 - o->current;
    return true;
}
