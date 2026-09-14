#include "config.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <cwctype>

static std::wstring S(const wchar_t* path, const wchar_t* sec, const wchar_t* key, const std::wstring& def)
{
    wchar_t buf[512]; GetPrivateProfileStringW(sec, key, def.c_str(), buf, 512, path); return buf;
}
static int   I(const wchar_t* path, const wchar_t* sec, const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, path); }
static bool  B(const wchar_t* path, const wchar_t* sec, const wchar_t* key, bool def) { return I(path, sec, key, def ? 1 : 0) != 0; }
static float F(const wchar_t* path, const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64]; GetPrivateProfileStringW(sec, key, L"", buf, 64, path);
    return buf[0] ? (float)wcstod(buf, nullptr) : def;
}

static HotkeySpec ParseHotkey(std::wstring s, UINT def_vk)
{
    HotkeySpec h; h.vk = def_vk;
    for (auto& c : s) c = (wchar_t)towupper(c);
    size_t pos;
    while ((pos = s.find(L'+')) != std::wstring::npos)
    {
        const std::wstring m = s.substr(0, pos); s = s.substr(pos + 1);
        if (m == L"CTRL") h.mods |= MOD_CONTROL; else if (m == L"ALT") h.mods |= MOD_ALT; else if (m == L"SHIFT") h.mods |= MOD_SHIFT;
    }
    if (s.size() >= 2 && s[0] == L'F') { const int n = _wtoi(s.c_str() + 1); if (n >= 1 && n <= 24) h.vk = VK_F1 + n - 1; }
    else if (s.size() == 1) h.vk = (UINT)s[0];
    return h;
}

bool ConfigLoad(const wchar_t* path, Config& c)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    c.window_class = S(path, L"capture", L"window_class", c.window_class);
    c.window_title = S(path, L"capture", L"window_title", c.window_title);
    c.cursor = B(path, L"capture", L"cursor", c.cursor);
    c.dda = S(path, L"capture", L"mode", L"dda") != L"wgc";
    c.border = B(path, L"capture", L"border", c.border);

    c.nr_enabled = B(path, L"nr", L"enabled", c.nr_enabled);
    const std::wstring work = S(path, L"nr", L"work", L"auto");
    if (swscanf_s(work.c_str(), L"%ux%u", &c.work_w, &c.work_h) != 2) { c.work_w = c.work_h = 0; }
    c.create_style = I(path, L"nr", L"create_style", c.create_style);
    c.param_block = I(path, L"nr", L"param_block", c.param_block);
    c.tuning.preset = I(path, L"nr", L"preset", c.tuning.preset);
    c.tuning.style = I(path, L"nr", L"style", c.tuning.style);
    c.tuning.intensity = F(path, L"nr", L"intensity", c.tuning.intensity);
    c.tuning.local_tone = F(path, L"nr", L"local_tone", c.tuning.local_tone);
    c.tuning.local_structure = F(path, L"nr", L"local_structure", c.tuning.local_structure);
    c.tuning.skin_structure = F(path, L"nr", L"skin_structure", c.tuning.skin_structure);
    c.tuning.auto_mask = B(path, L"nr", L"auto_mask", c.tuning.auto_mask);
    c.tuning.ui_correction = B(path, L"nr", L"ui_correction", c.tuning.ui_correction);
    c.exposure_scale = F(path, L"nr", L"exposure_scale", c.exposure_scale);
    c.residual_strength = F(path, L"nr", L"residual_strength", c.residual_strength);
    c.warmup = I(path, L"nr", L"warmup", c.warmup);
    c.rebuild_debounce_frames = I(path, L"nr", L"rebuild_debounce_frames", c.rebuild_debounce_frames);
    c.max_fps = I(path, L"nr", L"max_fps", c.max_fps);

    const std::wstring in = S(path, L"ofa", L"input", L"960x540");
    if (swscanf_s(in.c_str(), L"%ux%u", &c.ofa_w, &c.ofa_h) != 2) { c.ofa_w = 960; c.ofa_h = 540; }
    const std::wstring grid = S(path, L"ofa", L"grid", L"auto");
    c.ofa_grid = grid == L"auto" ? 0 : _wtoi(grid.c_str());
    c.cost_reject = (UINT)I(path, L"ofa", L"cost_reject", (int)c.cost_reject);
    c.zero_below = F(path, L"ofa", L"zero_below", c.zero_below);
    c.ofa_dll = S(path, L"ofa", L"dll_path", L"");

    c.feather = I(path, L"ui", L"feather", c.feather);
    c.nrects = 0;
    for (int i = 1; i <= 16; ++i)
    {
        wchar_t key[16]; swprintf_s(key, L"rect%d", i);
        const std::wstring r = S(path, L"ui", key, L"");
        UiRect rc;
        if (swscanf_s(r.c_str(), L"%d,%d,%d,%d", &rc.x0, &rc.y0, &rc.x1, &rc.y1) == 4) c.rects[c.nrects++] = rc;
    }

    c.fg_enabled = B(path, L"fg", L"enabled", c.fg_enabled);
    c.fg_multiplier = I(path, L"fg", L"multiplier", c.fg_multiplier);

    c.exclude_from_capture = B(path, L"overlay", L"exclude_from_capture", c.exclude_from_capture);
    c.reassert_topmost_every = I(path, L"overlay", L"reassert_topmost_every", c.reassert_topmost_every);

    c.hk_toggle = ParseHotkey(S(path, L"hotkeys", L"toggle", L"F9"), VK_F9);
    c.hk_wipe = ParseHotkey(S(path, L"hotkeys", L"wipe", L"F10"), VK_F10);
    c.hk_reload = ParseHotkey(S(path, L"hotkeys", L"reload", L"F11"), VK_F11);
    c.hk_quit = ParseHotkey(S(path, L"hotkeys", L"quit", L"Ctrl+F12"), VK_F12);
    c.hk_fg = ParseHotkey(S(path, L"hotkeys", L"fg", L"F8"), VK_F8);

    c.log_file = S(path, L"log", L"file", c.log_file);
    c.stats_every = I(path, L"log", L"stats_every", c.stats_every);
    c.gpu_timestamps = B(path, L"log", L"gpu_timestamps", c.gpu_timestamps);
    return true;
}

bool ConfigNeedsRebuild(const Config& a, const Config& b)
{
    const NrTuning &x = a.tuning, &y = b.tuning;
    return a.work_w != b.work_w || a.work_h != b.work_h || a.create_style != b.create_style || a.param_block != b.param_block ||
           x.preset != y.preset || x.style != y.style || x.intensity != y.intensity || x.local_tone != y.local_tone ||
           x.local_structure != y.local_structure || x.skin_structure != y.skin_structure || x.auto_mask != y.auto_mask || x.ui_correction != y.ui_correction;
}

NrConfig ConfigToNr(const Config& c)
{
    NrConfig n; n.work_w = c.work_w; n.work_h = c.work_h;
    n.create_style = c.create_style == 0 ? NrCreateA : NrCreateB;
    n.block = (NrParamBlock)c.param_block;
    n.tuning = c.tuning;
    return n;
}
