// WIC PNG load/save and --bench: PNG frames through the same pipeline as live capture.
#include "pipeline.h"
#include "log.h"
#include <wincodec.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

static IWICImagingFactory* Wic()
{
    static IWICImagingFactory* f = nullptr;
    if (!f)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // same apartment kind as winrt::init_apartment
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) { Log("[png] WIC factory failed"); f = nullptr; }
    }
    return f;
}

bool LoadPngRgba(const wchar_t* path, std::vector<uint8_t>& rgba, UINT& w, UINT& h)
{
    IWICImagingFactory* f = Wic(); if (!f) return false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr; IWICBitmapSource* src = nullptr;
    bool ok = false;
    if (FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) { Log("[png] cannot open %ls", path); goto done; }
    if (FAILED(dec->GetFrame(0, &frame))) goto done;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &src))) goto done;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) goto done;
    rgba.resize((size_t)w * h * 4);
    ok = SUCCEEDED(src->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
done:
    REL(src); REL(frame); REL(dec);
    return ok;
}

bool SavePngRgba(const wchar_t* path, const uint8_t* rgba, UINT w, UINT h)
{
    IWICImagingFactory* f = Wic(); if (!f) return false;
    IWICStream* stream = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* frame = nullptr;
    std::vector<uint8_t> bgra;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    bool ok = false;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) { Log("[png] cannot write %ls", path); goto done; }
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) goto done;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt))) goto done;
    if (fmt == GUID_WICPixelFormat32bppBGRA)   // the encoder picked BGRA: swap in a copy
    {
        bgra.assign(rgba, rgba + (size_t)w * h * 4);
        for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        rgba = bgra.data();
    }
    else if (fmt != GUID_WICPixelFormat32bppRGBA) { Log("[png] encoder refused RGBA"); goto done; }
    if (FAILED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)rgba))) goto done;
    ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
done:
    REL(frame); REL(enc); REL(stream);
    return ok;
}

// ---- bench ----------------------------------------------------------------------------------------
static std::vector<std::wstring> ListPngs(const std::wstring& path)
{
    std::vector<std::wstring> out;
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return out;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { out.push_back(path); return out; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(path + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

int RunBench(int argc, char** argv)
{
    std::wstring input; int frames = 120; UINT work_w = 0, work_h = 0; bool present = true;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--bench") && i + 1 < argc) { const char* s = argv[++i]; input.assign(s, s + strlen(s)); }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) sscanf_s(argv[++i], "%ux%u", &work_w, &work_h);
        else if (!strcmp(argv[i], "--no-present")) present = false;
    }
    const std::wstring dir = ExeDir();
    Config cfg; ConfigLoad((dir + L"\\nrfilter.ini").c_str(), cfg);
    LogInit((dir + L"\\bench.log").c_str());
    if (work_w && work_h) { cfg.work_w = work_w; cfg.work_h = work_h; }

    const std::vector<std::wstring> files = ListPngs(input);
    if (files.empty()) { Log("[bench] no PNG at %ls", input.c_str()); return 1; }
    std::vector<std::vector<uint8_t>> images; UINT w = 0, h = 0;
    for (const auto& f : files)
    {
        std::vector<uint8_t> px; UINT iw = 0, ih = 0;
        if (!LoadPngRgba(f.c_str(), px, iw, ih)) continue;
        if (!w) { w = iw; h = ih; }
        if (iw != w || ih != h) { Log("[bench] %ls is %ux%u, expected %ux%u - skipped", f.c_str(), iw, ih, w, h); continue; }
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // RGBA -> BGRA like a capture
        images.push_back(std::move(px));
    }
    if (images.empty()) { Log("[bench] nothing loaded"); return 1; }
    Log("[bench] %zu frames of %ux%u from %ls", images.size(), w, h, input.c_str());

    Gpu g;
    if (!GpuInit(g, -1)) return 1;
    ResolveWork(cfg, dir);
    Pipeline* p = PipelineCreate(g, cfg, w, h, present, nullptr);
    if (!p) { GpuShutdown(g); return 1; }
    p->measure_ofa = true;

    std::vector<ID3D12Resource*> tex;
    for (size_t i = 0; i < images.size(); ++i)
    {
        ID3D12Resource* t = GpuMakeTex(g, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"bench_capture");
        if (!t || !GpuUploadTex(g, t, images[i].data(), w, h, 4, D3D12_RESOURCE_STATE_COMMON)) { Log("[bench] upload %zu failed", i); PipelineDestroy(p); GpuShutdown(g); return 1; }
        tex.push_back(t);
    }
    images.clear();

    int evaluated = 0, rc = 0;
    const double t0 = NowMs();
    for (int i = 0; evaluated < frames && i < frames + 64; ++i)
    {
        if (!PipelineFrame(p, tex[i % tex.size()], nullptr, 0, i == 0)) { Log("[bench] frame %d failed", i); GpuLogDeviceRemoved(g, "bench"); rc = 2; break; }
        // ponytail: idle after each frame so both lists of the frame retire and get sampled (the
        // stamp reader only sees the most recently retired slot). Per-stage GPU times are unaffected;
        // the fps line below is therefore not a throughput number.
        GpuWaitIdle(g); PipelineReadStamps(p);
        if (!p->last_evaluated && !p->nr) p->last_evaluated = true;   // NR disabled: count composed frames
        if (p->last_evaluated && ++evaluated == std::max(0, cfg.warmup)) for (auto& s : p->st) s.v.clear();   // drop warm-up samples
    }
    const double wall = NowMs() - t0;
    GpuWaitIdle(g);
    PipelineReadStamps(p);

    printf("\n%-16s %10s %10s %8s\n", "stage", "median ms", "p95 ms", "samples");
    FILE* csv = nullptr; _wfopen_s(&csv, (dir + L"\\bench.csv").c_str(), L"w");
    if (csv) fprintf(csv, "stage,median_ms,p95_ms,samples\n");
    for (int s = 0; s < PS_COUNT; ++s)
    {
        const StageStats& st = p->st[s];
        printf("%-16s %10.3f %10.3f %8zu\n", kPipeStageName[s], st.med(), st.p95(), st.v.size());
        if (csv) fprintf(csv, "%s,%.3f,%.3f,%zu\n", kPipeStageName[s], st.med(), st.p95(), st.v.size());
    }
    printf("%d frames in %.1f ms (%.1f fps)\n", evaluated, wall, evaluated * 1000.0 / wall);
    fflush(stdout);   // the NGX runtime's teardown can end the process before the CRT flushes
    if (csv) { fprintf(csv, "frames,%d,wall_ms,%.1f\n", evaluated, wall); fclose(csv); Log("[bench] wrote bench.csv"); }

    for (auto t : tex) t->Release();
    PipelineDestroy(p);
    GpuShutdown(g);
    return rc;
}
