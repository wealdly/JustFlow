// --bench: PNG frames through the same pipeline as live capture.
#include "latewarp.h"
#include "mouse.h"
#include "pipeline.h"
#include <DirectXPackedVector.h>
#include "png.h"
#include <cmath>
#include "log.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

// A texture (8-bit RGBA or BGRA, w x h, resting in `state`) read back and written as a PNG.
bool SaveTexPng(Gpu& g, ID3D12Resource* tex, UINT w, UINT h, D3D12_RESOURCE_STATES state, const wchar_t* path)
{
    std::vector<uint8_t> px((size_t)w * h * 4);
    return tex && GpuReadbackTex(g, tex, px.data(), w, h, 4, state) && SavePngRgba(path, px.data(), w, h);
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
    std::wstring input, ini; int frames = 120; UINT work_w = 0, work_h = 0; bool present = true; double pace = 0; bool lw = false; float lw_a[4] = { 0, 0, 0, 60 };
    UINT rework_w = 0, rework_h = 0;   // --rework WxH: change the work size LIVE a third of the way in
    int nr_toggle_at = -1;             // --nr-toggle-at N: flip [nr] enabled through PipelineReload at frame N (what Settings and the tray do)
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--bench") && i + 1 < argc) { const char* s = argv[++i]; input.assign(s, s + strlen(s)); }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) sscanf_s(argv[++i], "%ux%u", &work_w, &work_h);
        else if (!strcmp(argv[i], "--no-present")) present = false;
        else if (!strcmp(argv[i], "--pace") && i + 1 < argc) pace = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lwtest") && i + 1 < argc) { lw = true; sscanf_s(argv[++i], "%f,%f,%f,%f", &lw_a[0], &lw_a[1], &lw_a[2], &lw_a[3]); }
        else if (!strcmp(argv[i], "--rework") && i + 1 < argc) sscanf_s(argv[++i], "%ux%u", &rework_w, &rework_h);
        else if (!strcmp(argv[i], "--nr-toggle-at") && i + 1 < argc) nr_toggle_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ini") && i + 1 < argc) { const char* s = argv[++i]; ini.assign(s, s + strlen(s)); }   // profile next to the exe, or a path
    }
    const std::wstring dir = ExeDir();
    std::vector<std::wstring> names; int idx;
    const std::wstring ini_path = PickProfile(dir, ini, names, idx), app_path = dir + L"\\justflow.ini";   // no --ini: the same auto-pick as live mode
    Config cfg; const int have = ConfigLoad(app_path.c_str(), ini_path.c_str(), cfg);
    LogInit((dir + L"\\bench.log").c_str());
    LogConfigFiles(app_path, ini_path, have);
    if (work_w && work_h) { cfg.work_w = work_w; cfg.work_h = work_h; cfg.work_auto = false; }   // an explicit size is not auto

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
    if (lw)   // --lwtest yaw,pitch,roll,vfov (degrees): Frame Warp the LAST input frame to a turned camera -> lw_out.png
    {
        const float k = 3.14159265f / 180.0f; int rc = 1;
        ID3D12Resource* in = GpuMakeTex(g, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"lw_in");
        ID3D12Resource* out = GpuMakeTex(g, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"lw_out");
        Latewarp* l = LatewarpCreate(g, dir.c_str(), w, h, w / 2, h / 2);
        if (in && out && l && GpuUploadTex(g, in, images.back().data(), w, h, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && GpuBegin(g))
        {
            ID3D12Resource* mask = nullptr; char mt[4];   // TEST JF_LW_MASKTEST: the left half held still
            if (GetEnvironmentVariableA("JF_LW_MASKTEST", mt, 4))
            {
                std::vector<uint8_t> m((size_t)(w / 4) * (h / 4));
                for (UINT y = 0; y < h / 4; ++y) for (UINT x = 0; x < w / 4; ++x) m[(size_t)y * (w / 4) + x] = x < w / 8 ? 255 : 0;
                mask = GpuMakeTex(g, w / 4, h / 4, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"lw_mask");
                GpuEnd(g); GpuUploadTex(g, mask, m.data(), w / 4, h / 4, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); GpuBegin(g);
            }
            const bool ok = LatewarpEvaluate(l, g.list, in, out, true, lw_a[0] * k, lw_a[1] * k, lw_a[2] * k, lw_a[3] * k, mask);
            if (GpuEnd(g) && GpuWaitIdle(g) && ok)
            {
                if (SaveTexPng(g, out, w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"lw_out.png")) rc = 0;
            }
        }
        Log("[bench] latewarp test yaw %.3f pitch %.3f roll %.3f vfov %.1f -> %s", lw_a[0], lw_a[1], lw_a[2], lw_a[3], rc ? "FAILED" : "lw_out.png");
        LatewarpDestroy(l); if (in) in->Release(); if (out) out->Release(); GpuShutdown(g); return rc;
    }
    if (cfg.selftest) { ComposeSelfTest(g); MouseSelfTest(); }
    ResolveWork(cfg, dir);
    Pipeline* p = PipelineCreate(g, cfg, w, h, present, nullptr);
    if (!p) { GpuShutdown(g); return 1; }
    p->measure_ofa = true;

    // TEST JF_BENCH_HDR=<white>: the frames arrive as an HDR desktop does under DDA - FP16 scRGB, SDR content
    // sRGB-decoded and scaled to that white (400 nits = 5.0) - through the same swizzle conversion as live.
    char hv[16]; const float hdr = GetEnvironmentVariableA("JF_BENCH_HDR", hv, sizeof hv) ? (float)atof(hv) : 0.0f;
    if (hdr > 0) { p->hdr_white = hdr; Log("[bench] input as FP16 scRGB, SDR white %.2f", hdr); }
    std::vector<ID3D12Resource*> tex;
    for (size_t i = 0; i < images.size(); ++i)
    {
        std::vector<uint16_t> half;
        if (hdr > 0)
        {
            half.resize((size_t)w * h * 4);
            for (size_t k = 0; k < half.size(); ++k)
            {
                const size_t px = k / 4, ch = k % 4;   // images are BGRA bytes; FP16 is RGBA
                const float v = images[i][px * 4 + (ch == 3 ? 3 : 2 - ch)] / 255.0f;
                const float lin = ch == 3 ? 1.0f : (v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f)) * hdr;
                half[k] = DirectX::PackedVector::XMConvertFloatToHalf(lin);
            }
        }
        ID3D12Resource* t = GpuMakeTex(g, w, h, hdr > 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"bench_capture");
        if (!t || !GpuUploadTex(g, t, hdr > 0 ? (const void*)half.data() : (const void*)images[i].data(), w, h, hdr > 0 ? 8 : 4, D3D12_RESOURCE_STATE_COMMON)) { Log("[bench] upload %zu failed", i); PipelineDestroy(p); GpuShutdown(g); return 1; }
        tex.push_back(t);
    }
    images.clear();

    int evaluated = 0, rc = 0;
    const double t0 = NowMs();
    const int slack = cfg.nr_async ? 256 : 64;   // async: feature create + warm-up happen on the model thread while frames keep flowing
    // toast self-test ([log] selftest=1, single input frame): frame 5 composes with a forced toast, frame 6
    // without; the box region must differ in >= 100 pixels.
    const bool toast_test = cfg.selftest && tex.size() == 1;
    static const char* const kToastText = "toast self-test";
    std::vector<uint8_t> with_toast;
    int last_in = 0;   // input index of the final frame: warm-up frames are not counted, so it is not (frames-1) % n
    // async: the feature is created on the model thread (~0.5 s) while frames keep flowing, and a bench
    // frame takes a millisecond - the whole run used to end before the first residual existed. Hold on
    // input 0 until the model is live (10 s cap), then play the sequence from its start.
    int seq = 0; double pace_due = 0;
    for (int i = 0; evaluated < frames && (cfg.nr_async && evaluated == 0 ? NowMs() - t0 < 10000 : seq < frames + slack); ++i)
    {
        last_in = seq % (int)tex.size();
        if (toast_test && i == 5) { p->cfg.toast = true; PipelineToast(p, "%s", kToastText); }
        // The thing every other bench run lacks: a reload while running. Fresh processes create the
        // feature once at the right size, so they could never see a rebuild that failed to rebuild.
        if (rework_w && i == frames / 3)
        {
            Config nc = p->cfg; nc.work_w = rework_w; nc.work_h = rework_h; nc.work_auto = false; nc.rebuild_debounce_frames = 4;
            Log("[bench] live work-size change %ux%u -> %ux%u at frame %d", p->ww, p->wh, rework_w, rework_h, i);
            PipelineReload(p, nc);
        }
        if (i == nr_toggle_at)
        {
            Config nc = p->cfg; nc.nr_enabled = !nc.nr_enabled;
            Log("[bench] neural layer %s by reload at frame %d", nc.nr_enabled ? "ON" : "OFF", i);
            PipelineReload(p, nc);
        }
        if (toast_test && i == 6) p->toast_until_ms = 0;
        // Paced like a game: a frame that is late is late, and the schedule starts again from it. Pacing
        // against the start time made every stall (DLSS-G's ~1 s feature create on frame 0) come out as a
        // back-to-back burst of the frames "owed", which FG drops - read for a day as a DLSS-G slowdown in
        // FG-only mode (a model evaluate per frame happened to spread the burst).
        if (pace > 0) { if (NowMs() > pace_due + 1000.0 / pace) pace_due = NowMs(); while (NowMs() < pace_due) Sleep(1); pace_due += 1000.0 / pace; }
        if (!PipelineFrame(p, tex[last_in], nullptr, 0, i == 0)) { Log("[bench] frame %d failed", i); GpuLogDeviceRemoved(g, "bench"); rc = 2; break; }
        // ponytail: idle after each frame so both lists of the frame retire and get sampled (the
        // stamp reader only sees the most recently retired slot). Per-stage GPU times are unaffected;
        // the fps line below is therefore not a throughput number.
        GpuWaitIdle(g); PipelineReadStamps(p);
        if (toast_test && (i == 5 || i == 6))
        {
            std::vector<uint8_t> px((size_t)w * h * 4);
            const bool got = GpuReadbackTex(g, p->shown, px.data(), w, h, 4, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (i == 5) { if (got) with_toast.swap(px); }
            else
            {
                const int sc = std::max(1, p->cfg.toast_scale), pad = 2 * sc, bw = TextBoxW(strlen(kToastText), sc, pad), bh = TextBoxH(sc, pad);
                const int x0 = std::max(0, ((int)w - bw) / 2), y0 = 48;
                int changed = 0;
                if (got && !with_toast.empty())
                    for (int y = y0; y < std::min((int)h, y0 + bh); ++y) for (int x = x0; x < std::min((int)w, x0 + bw); ++x)
                    {
                        const uint8_t *a = &with_toast[((size_t)y * w + x) * 4], *b = &px[((size_t)y * w + x) * 4];
                        if (abs(a[0] - b[0]) > 8 || abs(a[1] - b[1]) > 8 || abs(a[2] - b[2]) > 8) ++changed;
                    }
                Log("[cs] toast self-test %s (%d px changed in the %dx%d box at %d,%d)", changed >= 100 ? "PASS" : "FAIL", changed, bw, bh, x0, y0);
            }
        }
        if (!cfg.nr_async || evaluated > 0 || p->last_evaluated) ++seq;
        if (!p->last_evaluated && !p->nr) p->last_evaluated = true;   // NR disabled: count composed frames
        if (p->last_evaluated && ++evaluated == std::max(0, cfg.warmup)) for (auto& s : p->st) s.v.clear();   // drop warm-up samples
    }
    const double wall = NowMs() - t0;
    Log("[bench] last input frame %d of %zu, %u duplicates not handed to FG, %u scene cuts", last_in, tex.size(), p->dups, p->cuts);   // final_*.png and mv_bench.f32 describe this one
    GpuWaitIdle(g);
    PipelineReadStamps(p);
    if (p->fg)
    {
        FgStatsOut fs; FgStats(p->fg, fs); StageStats sp; sp.v = fs.spacing_ms;
        const double sp_max = sp.v.empty() ? -1.0 : *std::max_element(sp.v.begin(), sp.v.end());   // a single hitch hides from p95
        double ev95 = -1; const double ev = FgEvalMs(p->fg, &ev95);
        Log("[fg] bench: %u frames presented (%.1f fps), %u dropped, spacing %.2f/%.2f/%.2f ms (med/p95/MAX, %zu samples), %u paused, eval %.2f/%.2f ms (med/p95), gen %u, no pair %u, disabled %u, preempted %u, vblank wait %.2f ms x%u, record wait %.2f ms x%u", fs.presented, fs.presented * 1000.0 / wall, fs.drops, sp.med(), sp.p95(), sp_max, sp.v.size(), fs.paused, ev, ev95, fs.gen_shown, fs.no_pair, fs.disabled, fs.preempts, fs.vblank_waits ? fs.vblank_wait_sum_ms / fs.vblank_waits : -1.0, fs.vblank_waits, fs.record_waits ? fs.record_wait_sum_ms / fs.record_waits : -1.0, fs.record_waits);
    }

    if (p->fg && FgMultiplier(p->fg) > 1)
    {
        FgDebugHold(p->fg);
        SaveTexPng(g, FgDebugGen(p->fg, 0), w, h, D3D12_RESOURCE_STATE_COPY_SOURCE, L"fg_gen.png");
        SaveTexPng(g, FgDebugReal(p->fg), w, h, D3D12_RESOURCE_STATE_COPY_SOURCE, L"fg_real.png");
        if (ID3D12Resource* m = FgDebugMask(p->fg))
        {
            const D3D12_RESOURCE_DESC md = m->GetDesc(); std::vector<uint8_t> px((size_t)md.Width * md.Height), rgba(px.size() * 4, 255);
            if (GpuReadbackTex(g, m, px.data(), (UINT)md.Width, md.Height, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
            { for (size_t i = 0; i < px.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = px[i]; SavePngRgba(L"fg_mask.png", rgba.data(), (UINT)md.Width, md.Height); }
        }
    }

    // The UI restore on generated frames: inside a mask rect a generated frame must be pixel-exact
    // against the composed frame it came from, and outside it must not be (DLSS-G interpolated it).
    // Both halves matter - "identical everywhere" would mean the generated frame is just a copy.
    if (p->fg && FgMultiplier(p->fg) > 1 && p->cfg.nrects > 0)
    {
        ID3D12Resource* gen = FgDebugGen(p->fg, 0), *real = FgDebugReal(p->fg);
        std::vector<uint8_t> a((size_t)w * h * 4), b((size_t)w * h * 4);
        if (gen && real && GpuReadbackTex(g, gen, a.data(), w, h, 4, D3D12_RESOURCE_STATE_COPY_SOURCE)
                       && GpuReadbackTex(g, real, b.data(), w, h, 4, D3D12_RESOURCE_STATE_COPY_SOURCE))
        {
            const UiRect& r = p->cfg.rects[0];
            size_t in_n = 0, in_diff = 0, out_n = 0, out_diff = 0;
            for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
            {
                const size_t o = ((size_t)y * w + x) * 4;
                const bool same = a[o] == b[o] && a[o + 1] == b[o + 1] && a[o + 2] == b[o + 2];
                const bool inside = (int)x >= r.x0 && (int)x < r.x1 && (int)y >= r.y0 && (int)y < r.y1;
                if (inside) { ++in_n; if (!same) ++in_diff; } else { ++out_n; if (!same) ++out_diff; }
            }
            const double outpct = out_n ? 100.0 * (double)out_diff / (double)out_n : 0.0;
            const bool pass = in_diff == 0 && out_diff > 0;
            Log("[bench] FG ui restore: %zu/%zu px differ inside rect1, %.1f%% differ outside - %s",
                in_diff, in_n, outpct, pass ? "PASS" : "FAIL");
            printf("FG ui restore: %zu/%zu px differ inside rect1, %.1f%% differ outside - %s\n",
                   in_diff, in_n, outpct, pass ? "PASS" : "FAIL");
        }
    }

    // The motion vectors, checked against KNOWN motion. These feed the NR model and the warp engines, so
    // a wrong sign, scale or reference frame shows up as warping and temporal instability rather
    // than as an error - which is why it can hide for a long time behind "FG is unstable".
    // Feed --bench a panning sequence and the expected magnitude is arithmetic: a shift of N native
    // pixels is N * (ww / w) at work resolution.
    if (p->mv)
    {
        const UINT ww = p->ww, wh = p->wh;
        std::vector<uint8_t> raw((size_t)ww * wh * 4);
        if (GpuReadbackTex(g, p->mv, raw.data(), ww, wh, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        {
            auto half = [](uint16_t h) -> float {
                const int e = (h >> 10) & 0x1F, m = h & 0x3FF; const float sgn = (h & 0x8000) ? -1.0f : 1.0f;
                if (e == 0) return sgn * ldexpf((float)m, -24);
                if (e == 31) return sgn * (m ? NAN : INFINITY);
                return sgn * ldexpf((float)(m | 0x400), e - 25);
            };
            std::vector<float> xs, ys; size_t nz = 0, bad = 0;
            for (size_t i = 0; i < (size_t)ww * wh; ++i)
            {
                const uint16_t* q = (const uint16_t*)&raw[i * 4];
                const float vx = half(q[0]), vy = half(q[1]);
                if (!std::isfinite(vx) || !std::isfinite(vy)) { ++bad; continue; }
                if (vx != 0.0f || vy != 0.0f) { ++nz; xs.push_back(vx); ys.push_back(vy); }
            }
            {   // the raw field, for scoring against ground truth (tools/scene exports exact motion)
                std::vector<float> xy((size_t)ww * wh * 2);
                for (size_t i = 0; i < (size_t)ww * wh; ++i) { const uint16_t* q = (const uint16_t*)&raw[i * 4]; xy[i * 2] = half(q[0]); xy[i * 2 + 1] = half(q[1]); }
                FILE* mf = nullptr; if (_wfopen_s(&mf, L"mv_bench.f32", L"wb") == 0 && mf) { fwrite(xy.data(), sizeof(float), xy.size(), mf); fclose(mf); }
                Log("[bench] mv_bench.f32 = input frame %d vs the one before it%s", last_in, last_in == 0 && tex.size() > 1 ? " (the WRAP pair: a scene cut, not motion)" : "");
            }
            auto med = [](std::vector<float>& v) { if (v.empty()) return 0.0f; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
            // Left and right halves separately: a rigid pan cannot tell a coarse flow grid from a
            // fine one (every vector is the same, so downsampling loses nothing). Split motion can.
            std::vector<float> lx, rx;
            for (UINT y = 0; y < wh; ++y) for (UINT x = 0; x < ww; ++x)
            {
                const uint16_t* q = (const uint16_t*)&raw[((size_t)y * ww + x) * 4];
                const float vx = half(q[0]);
                if (!std::isfinite(vx)) continue;
                (x < ww / 2 ? lx : rx).push_back(vx);
            }
            const float lmed = med(lx), rmed = med(rx);
            // Transition width: columns whose median vector matches NEITHER side. That is the smear
            // a coarse flow grid leaves at a motion boundary, and it is the only thing a rigid pan
            // cannot show. Expect it to scale with the grid: one grid cell = `grid` OFA-input px.
            int ambiguous = 0;
            for (UINT x = 0; x < ww; ++x)
            {
                std::vector<float> col;
                for (UINT y = 0; y < wh; ++y)
                {
                    const uint16_t* q = (const uint16_t*)&raw[((size_t)y * ww + x) * 4];
                    const float vx = half(q[0]); if (std::isfinite(vx)) col.push_back(vx);
                }
                const float m = med(col);
                if (fabsf(m - lmed) > 1.0f && fabsf(m - rmed) > 1.0f) ++ambiguous;
            }
            Log("[bench] mv transition: %d of %u columns match neither side", ambiguous, ww);
            printf("mv transition: %d of %u columns match neither side\n", ambiguous, ww);
            const double frac = 100.0 * (double)nz / ((double)ww * wh);
            const float mx = med(xs), my = med(ys);
            Log("[bench] mv %ux%u: %.1f%% non-zero, median (%.2f, %.2f) work px, %zu non-finite", ww, wh, frac, mx, my, bad);
            printf("mv %ux%u: %.1f%% non-zero, median (%.2f, %.2f) work px, %zu non-finite\n", ww, wh, frac, mx, my, bad);
            Log("[bench] mv halves: left %.2f  right %.2f work px", lmed, rmed);
            printf("mv halves: left %.2f  right %.2f work px\n", lmed, rmed);
        }
    }

    // The finished frame and the native one it came from, for judging what a setting does to the
    // PICTURE rather than to a work-resolution intermediate (e.g. how much of the model's edit
    // survives being composed up from a small work size).
    if (p->shown && p->color4k)
    {
        wchar_t f[64]; _snwprintf_s(f, _TRUNCATE, L"final_%ux%u.png", p->ww, p->wh);
        SaveTexPng(g, p->shown, w, h, D3D12_RESOURCE_STATE_COPY_SOURCE, f);
        SaveTexPng(g, p->color4k, w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"final_native.png");
    }

    // What the spike never checked: that the model wrote the whole work texture. A subrect here is
    // the "renders in the top-left, dark everywhere else" symptom, and it is silent at runtime.
    if (p->nr)
    {
        ID3D12Resource* out = cfg.nr_async ? p->nr_out_m : p->nr_out;
        const UINT ww = p->ww, wh = p->wh;
        std::vector<uint8_t> px((size_t)ww * wh * 4);
        if (out && GpuReadbackTex(g, out, px.data(), ww, wh, 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            UINT x0 = ww, y0 = wh, x1 = 0, y1 = 0; size_t nz = 0;
            for (UINT y = 0; y < wh; ++y) for (UINT x = 0; x < ww; ++x)
            {
                const uint8_t* q = &px[((size_t)y * ww + x) * 4];
                if (q[0] | q[1] | q[2])
                { ++nz; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
            }
            const double frac = 100.0 * (double)nz / ((double)ww * wh);
            const char* verdict = frac > 99.0 ? "PASS" : "FAIL (the model wrote a subrect)";
            {   // the model's own before/after, for inspecting what it did with scale
                std::vector<uint8_t> in((size_t)ww * wh * 4);
                ID3D12Resource* src = cfg.nr_async ? p->nr_in_m : p->nr_in;
                if (src && GpuReadbackTex(g, src, in.data(), ww, wh, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                {
                    wchar_t f1[64], f2[64];
                    _snwprintf_s(f1, _TRUNCATE, L"work_in_%ux%u.png", ww, wh);
                    _snwprintf_s(f2, _TRUNCATE, L"work_out_%ux%u.png", ww, wh);
                    SavePngRgba(f1, in.data(), ww, wh); SavePngRgba(f2, px.data(), ww, wh);
                    Log("[bench] wrote %ls and %ls", f1, f2);
                }
            }
            Log("[bench] nr_out coverage %ux%u: %.1f%% written, bbox %u,%u..%u,%u - %s", ww, wh, frac, x0, y0, x1, y1, verdict);
            printf("nr_out coverage %ux%u: %.1f%% written, bbox %u,%u..%u,%u - %s\n", ww, wh, frac, x0, y0, x1, y1, verdict);
        }
    }

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
    if (cfg.nr_async)
    {
        StageStats mm; { std::lock_guard<std::mutex> lk(p->pub_mu); mm.v.swap(p->model_ms.v); }
        const UINT me = p->model_evals.exchange(0);
        printf("model (async): %u evals in %.1f ms (%.1f fps), eval %.3f/%.3f ms (med/p95), residual_age_frames %.1f/%.1f (med/p95, %zu samples)\n",
               me, wall, me * 1000.0 / wall, mm.med(), mm.p95(), p->residual_age.med(), p->residual_age.p95(), p->residual_age.v.size());
        if (csv) fprintf(csv, "model_evals,%u,model_ms,%.3f,model_p95_ms,%.3f,residual_age_frames,%.1f\n", me, mm.med(), mm.p95(), p->residual_age.med());
    }
    fflush(stdout);   // the NGX runtime's teardown can end the process before the CRT flushes
    if (csv) { fprintf(csv, "frames,%d,wall_ms,%.1f\n", evaluated, wall); fclose(csv); Log("[bench] wrote bench.csv"); }

    for (auto t : tex) t->Release();
    PipelineDestroy(p);
    GpuShutdown(g);
    return rc;
}
