// NVIDIA Optical Flow (SDK 5.0, D3D12 interface) session. Lifted from NeuralScreen nvofa.inl (MIT)
// minus env gates / dumps / fault injection; DLL search and grid selection are ours.
#include "nvofa.h"
#include "log.h"
#include "nvOpticalFlowD3D12.h"
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

struct Ofa
{
    Gpu* g = nullptr;
    HMODULE lib = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST api = {};
    NvOFHandle session = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
    ID3D12Resource* inputs[2] = {};
    ID3D12Resource* flow = nullptr;
    ID3D12Resource* cost = nullptr;
    NvOFGPUBufferHandle reg[4] = {};   // inputs[0], inputs[1], flow, cost
    UINT w = 0, h = 0, grid = 0, fw = 0, fh = 0;
    int current = 0;
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
    o->inputs[0] = GpuMakeTex(g, w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, L"ofa_in0");
    o->inputs[1] = GpuMakeTex(g, w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, L"ofa_in1");
    o->flow = GpuMakeTex(g, o->fw, o->fh, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, L"ofa_flow");
    o->cost = GpuMakeTex(g, o->fw, o->fh, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, L"ofa_cost");
    ID3D12Resource* res[4] = { o->inputs[0], o->inputs[1], o->flow, o->cost };
    for (int i = 0; i < 4; ++i)
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
    if (o->flow) { o->flow->Release(); o->flow = nullptr; }
    if (o->cost) { o->cost->Release(); o->cost = nullptr; }
    if (o->session) { o->api.nvOFDestroy(o->session); o->session = nullptr; }
    if (o->fence) { o->fence->Release(); o->fence = nullptr; }
    if (o->lib) FreeLibrary(o->lib);
    delete o;
}

ID3D12Resource* OfaInput(Ofa* o, int which) { return o->inputs[which & 1]; }
int             OfaCurrent(Ofa* o)          { return o->current; }
ID3D12Fence*    OfaFence(Ofa* o)            { return o->fence; }
UINT64          OfaFenceValue(Ofa* o)       { return o->value; }
ID3D12Resource* OfaFlow(Ofa* o)             { return o->flow; }
UINT            OfaFlowWidth(Ofa* o)        { return o->fw; }
UINT            OfaFlowHeight(Ofa* o)       { return o->fh; }
UINT            OfaGrid(Ofa* o)             { return o->grid; }
ID3D12Resource* OfaCost(Ofa* o)             { return o->cost; }

bool OfaExecute(Ofa* o, ID3D12Fence* in_fence, UINT64 in_value, bool reset)
{
    // reset: input == reference (both current) -> ~zero flow, and the expand pass zeroes it anyway.
    NV_OF_FENCE_POINT ready = { in_fence, in_value }, done = { o->fence, ++o->value };
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in = {};
    in.inputFrame = o->reg[o->current];
    in.referenceFrame = o->reg[reset ? o->current : 1 - o->current];   // current -> previous (NR convention)
    in.disableTemporalHints = NV_OF_TRUE;
    in.numFencePoints = 1; in.fencePoint = &ready;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out = {};
    out.outputBuffer = o->reg[2]; out.outputCostBuffer = o->reg[3]; out.fencePoint = &done;
    const NV_OF_STATUS st = o->api.nvOFExecuteD3D12(o->session, &in, &out);
    if (st != NV_OF_SUCCESS) { --o->value; return Fail(o, "nvOFExecuteD3D12", st); }
    o->current = 1 - o->current;
    return true;
}
