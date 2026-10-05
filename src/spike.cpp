// Spike 0: does the NR runtime accept a given work size, which parameter block works, and what
// does one evaluate cost? One child process per cell so a hang cannot poison the next cell; the
// parent is the watchdog. NB: a cell passing here only means it RAN - use --bench for the
// coverage check that says the model actually wrote the whole frame.
//
//   justflow_spike                      run the whole grid, print a table, write spike_results.txt
//   justflow_spike --cell W H 1|2|3 [--png file]       one cell (what the parent spawns)
#include "d3d.h"
#include "log.h"
#include "ngx_nr.h"
#include "nvsdk_ngx.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Synthetic frame: smooth gradients + hard edges + a text-like grid, shifted by `shift` pixels so
// the temporal path is exercised. RGBA8.
static void Synth(std::vector<uint8_t>& px, UINT w, UINT h, int shift)
{
    px.resize((size_t)w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const int sx = (int)x + shift;
            uint8_t r = (uint8_t)((sx * 255) / (int)w), g = (uint8_t)((y * 255) / h), b = (uint8_t)(((sx ^ (int)y) & 64) ? 200 : 60);
            if (((sx / 24) % 5 == 0) && ((y / 24) % 7 == 0)) { r = g = b = 240; }   // bright blocks = "UI"
            if ((sx % 97) < 2) { r = 20; g = 20; b = 20; }                          // hard vertical edges
            uint8_t* p = &px[((size_t)y * w + x) * 4]; p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
}

static int RunCell(UINT w, UINT h, NrParamBlock block, const char* out_path)
{
    FILE* out = nullptr; fopen_s(&out, out_path, "w");
    auto result = [&](const char* status, unsigned create, int slot, double med, double p95, int evals) {
        if (out) { fprintf(out, "%ux%u %d %s create=0x%08X slot=%d evals=%d med=%.2f p95=%.2f\n", w, h, (int)block, status, create, slot, evals, med, p95); fclose(out); out = nullptr; }
    };
    Gpu g;
    if (!GpuInit(g, -1)) { result("NOGPU", 0, -1, -1, -1, 0); return 2; }
    Nr* nr = NrInit(g, ExeDir().c_str(), block);
    if (!nr) { result("NOINIT", 0, -1, -1, -1, 0); return 2; }

    ID3D12Resource* color[2] = { GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"color0"),
                                 GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"color1") };
    ID3D12Resource* mv = GpuMakeTex(g, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"mv");
    ID3D12Resource* outp = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"output");
    if (!color[0] || !color[1] || !mv || !outp) { result("NOMEM", 0, -1, -1, -1, 0); return 2; }
    std::vector<uint8_t> px;
    Synth(px, w, h, 0); GpuUploadTex(g, color[0], px.data(), w, h, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Synth(px, w, h, 3); GpuUploadTex(g, color[1], px.data(), w, h, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::vector<uint8_t> zero((size_t)w * h * 4, 0); GpuUploadTex(g, mv, zero.data(), w, h, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    NrConfig cfg; cfg.work_w = w; cfg.work_h = h;
    if (!GpuBegin(g)) { result("NOBEGIN", 0, -1, -1, -1, 0); return 2; }
    const bool created = NrCreate(nr, g.list, cfg);
    const UINT64 cv = GpuEnd(g);
    if (!created) { result("CREATEFAIL", 0, NrFloatSlot(nr), -1, -1, 0); return 1; }
    if (!GpuWait(g, g.fence, cv, 20000)) { result("HANG_CREATE", 1, NrFloatSlot(nr), -1, -1, 0); return 3; }
    NrMarkSubmitted(nr);

    std::vector<double> ms;
    const int warm = 8, total = 128;
    for (int i = 0; i < total; ++i)
    {
        if (!GpuBegin(g)) { result("NOBEGIN", 1, NrFloatSlot(nr), -1, -1, i); return 2; }
        GpuStamp(g, g.list, 0);
        const unsigned r = NrEvaluate(nr, g.list, color[i & 1], mv, outp, i == 0, 1.0f);
        GpuStamp(g, g.list, 1);
        const UINT64 v = GpuEnd(g);
        if (r != NVSDK_NGX_Result_Success) { result("EVALFAIL", r, NrFloatSlot(nr), -1, -1, i); Log("[spike] evaluate -> 0x%08X (%s)", r, NgxResultName(r)); return 1; }
        if (!GpuWait(g, g.fence, v, 20000)) { result("HANG_EVAL", 1, NrFloatSlot(nr), -1, -1, i); return 3; }
        double t[1] = { -1 };
        if (i >= warm && GpuStampsMs(g, t, 1) && t[0] >= 0) ms.push_back(t[0]);
    }
    std::sort(ms.begin(), ms.end());
    const double med = ms.empty() ? -1 : ms[ms.size() / 2], p95 = ms.empty() ? -1 : ms[(size_t)(ms.size() * 0.95)];
    result("OK", 1, NrFloatSlot(nr), med, p95, total);
    Log("[spike] %ux%u block %d: eval median %.2f ms p95 %.2f ms over %zu frames", w, h, (int)block, med, p95, ms.size());
    NrShutdown(nr); GpuShutdown(g);
    return 0;
}

int main(int argc, char** argv)
{
    const std::wstring dir = ExeDir();
    if (argc >= 5 && strcmp(argv[1], "--cell") == 0)
    {
        LogInit((dir + L"\\spike_cell.log").c_str());
        const UINT w = (UINT)atoi(argv[2]), h = (UINT)atoi(argv[3]);
        const NrParamBlock b = (NrParamBlock)atoi(argv[4]);
        char out[MAX_PATH]; snprintf(out, sizeof out, "%ls\\spike_%ux%u_%d.txt", dir.c_str(), w, h, (int)b);
        return RunCell(w, h, b, out);
    }
    LogInit((dir + L"\\spike.log").c_str());
    struct Size { UINT w, h; } sizes[] = { {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160} };
    const int blocks[] = { 1, 2, 3 };
    FILE* table = nullptr; _wfopen_s(&table, (dir + L"\\spike_results.txt").c_str(), L"w");
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (const Size& sz : sizes) for (int bl : blocks)
    {
        wchar_t cmd[512]; swprintf_s(cmd, L"\"%ls\" --cell %u %u %d", exe, sz.w, sz.h, bl);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
        const double t0 = NowMs();
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { Log("[spike] spawn failed"); continue; }
        const DWORD wr = WaitForSingleObject(pi.hProcess, 90000);
        DWORD code = 0;
        if (wr == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 9); code = 9; }
        else GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        char path[MAX_PATH]; snprintf(path, sizeof path, "%ls\\spike_%ux%u_%d.txt", dir.c_str(), sz.w, sz.h, bl);
        char line[512] = {};
        FILE* f = nullptr; if (fopen_s(&f, path, "r") == 0 && f) { fgets(line, sizeof line, f); fclose(f); }
        if (!line[0]) snprintf(line, sizeof line, "%ux%u %d %s (exit %lu)\n", sz.w, sz.h, bl, code == 9 ? "WATCHDOG_KILL" : "NO_RESULT", code);
        Log("[spike] %s  (%.1f s, exit %lu)", line, (NowMs() - t0) / 1000.0, code);
        if (table) { fputs(line, table); fflush(table); }
    }
    if (table) fclose(table);
    Log("[spike] done -> spike_results.txt");
    return 0;
}
