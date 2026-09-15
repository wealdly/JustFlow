#include "config.h"
#include <cstdlib>
#include <cstring>
#include <cwctype>

// ---- layers -----------------------------------------------------------------------------------------
// App-layer keys (justflow.ini). key == nullptr = the whole section. Anything not listed is a profile key
// ([ui] rectN included). Entries are grouped by section (ConfigStrayKeys scans one pass per section).
struct AppKey { const wchar_t *sec, *key; };
static const AppKey kAppKeys[] = {
    { L"app", nullptr },
    { L"hotkeys", nullptr },
    { L"overlay", nullptr },
    { L"ui", L"toast" }, { L"ui", L"toast_scale" }, { L"ui", L"hud" }, { L"ui", L"hud_corner" }, { L"ui", L"hud_scale" },
    { L"log", L"stats_every" }, { L"log", L"gpu_timestamps" }, { L"log", L"selftest" },
    { L"ofa", L"dll_path" },
    { L"nr", L"create_style" }, { L"nr", L"param_block" },
};

static bool IsAppKey(const wchar_t* sec, const wchar_t* key)
{
    for (const auto& k : kAppKeys) if (!_wcsicmp(k.sec, sec) && (!k.key || !_wcsicmp(k.key, key))) return true;
    return false;
}

bool ConfigIsAppKey(const wchar_t* sec, const wchar_t* key) { return IsAppKey(sec, key); }

std::wstring ConfigStrayKeys(const wchar_t* profile)
{
    std::wstring out;
    if (!profile || !*profile || GetFileAttributesW(profile) == INVALID_FILE_ATTRIBUTES) return out;
    const wchar_t* last = L"";
    for (const auto& k : kAppKeys)
    {
        if (!_wcsicmp(k.sec, last)) continue;
        last = k.sec;
        wchar_t buf[4096]; GetPrivateProfileSectionW(k.sec, buf, 4096, profile);   // "key=val\0key=val\0\0", comments dropped
        for (const wchar_t* e = buf; *e; e += wcslen(e) + 1)
        {
            std::wstring key(e, wcscspn(e, L"="));
            while (!key.empty() && iswspace(key.back())) key.pop_back();
            if (IsAppKey(k.sec, key.c_str())) out += (out.empty() ? L"" : L", ") + std::wstring(k.sec) + L"." + key;
        }
    }
    return out;
}

HotkeySpec ParseHotkey(std::wstring s, UINT def_vk)
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

std::wstring FormatHotkey(const HotkeySpec& h)
{
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl+";
    if (h.mods & MOD_ALT) s += L"Alt+";
    if (h.mods & MOD_SHIFT) s += L"Shift+";
    if (h.vk >= VK_F1 && h.vk <= VK_F24) s += L"F" + std::to_wstring(h.vk - VK_F1 + 1);
    else if (h.vk) s += (wchar_t)h.vk;   // letters/digits: vk == the character
    return s;
}

int ConfigLoad(const wchar_t* app, const wchar_t* profile, Config& c)
{
    auto exists = [](const wchar_t* p) { return p && *p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; };
    const wchar_t* pa = exists(app) ? app : nullptr;
    const wchar_t* pp = exists(profile) ? profile : nullptr;
    // each key is read from the file of its layer; a missing file reads as "key absent" (default kept)
    auto P = [&](const wchar_t* sec, const wchar_t* key) { return IsAppKey(sec, key) ? pa : pp; };
    auto S = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& def) -> std::wstring
    {
        const wchar_t* path = P(sec, key); if (!path) return def;
        wchar_t buf[512]; GetPrivateProfileStringW(sec, key, def.c_str(), buf, 512, path); return buf;
    };
    auto I = [&](const wchar_t* sec, const wchar_t* key, int def) { const wchar_t* path = P(sec, key); return path ? (int)GetPrivateProfileIntW(sec, key, def, path) : def; };
    auto B = [&](const wchar_t* sec, const wchar_t* key, bool def) { return I(sec, key, def ? 1 : 0) != 0; };
    auto F = [&](const wchar_t* sec, const wchar_t* key, float def) { const std::wstring s = S(sec, key, L""); return s.empty() ? def : (float)wcstod(s.c_str(), nullptr); };

    c.profile = S(L"app", L"profile", c.profile);

    c.window_class = S(L"capture", L"window_class", c.window_class);
    c.window_title = S(L"capture", L"window_title", c.window_title);
    c.cursor = B(L"capture", L"cursor", c.cursor);
    c.dda = S(L"capture", L"mode", L"dda") != L"wgc";
    c.border = B(L"capture", L"border", c.border);

    c.nr_enabled = B(L"nr", L"enabled", c.nr_enabled);
    const std::wstring work = S(L"nr", L"work", L"auto");
    if (swscanf_s(work.c_str(), L"%ux%u", &c.work_w, &c.work_h) != 2) { c.work_w = c.work_h = 0; }
    c.create_style = I(L"nr", L"create_style", c.create_style);
    c.param_block = I(L"nr", L"param_block", c.param_block);
    c.tuning.preset = I(L"nr", L"preset", c.tuning.preset);
    c.tuning.style = I(L"nr", L"style", c.tuning.style);
    c.tuning.intensity = F(L"nr", L"intensity", c.tuning.intensity);
    c.tuning.local_tone = F(L"nr", L"local_tone", c.tuning.local_tone);
    c.tuning.local_structure = F(L"nr", L"local_structure", c.tuning.local_structure);
    c.tuning.skin_structure = F(L"nr", L"skin_structure", c.tuning.skin_structure);
    c.tuning.auto_mask = B(L"nr", L"auto_mask", c.tuning.auto_mask);
    c.tuning.ui_correction = B(L"nr", L"ui_correction", c.tuning.ui_correction);
    c.exposure_scale = F(L"nr", L"exposure_scale", c.exposure_scale);
    c.residual_strength = F(L"nr", L"residual_strength", c.residual_strength);
    c.chroma = F(L"nr", L"chroma", c.chroma);
    c.saturation = F(L"nr", L"saturation", c.saturation);
    c.warmup = I(L"nr", L"warmup", c.warmup);
    c.rebuild_debounce_frames = I(L"nr", L"rebuild_debounce_frames", c.rebuild_debounce_frames);
    c.max_fps = I(L"nr", L"max_fps", c.max_fps);
    c.nr_async = S(L"nr", L"mode", c.nr_async ? L"async" : L"sync") == L"async";
    c.warp = F(L"nr", L"warp", c.warp);
    c.model_max_fps = I(L"nr", L"model_max_fps", c.model_max_fps);
    c.sharpen = F(L"nr", L"sharpen", c.sharpen);
    c.artcnn = B(L"nr", L"artcnn", c.artcnn);

    const std::wstring in = S(L"ofa", L"input", L"960x540");
    if (swscanf_s(in.c_str(), L"%ux%u", &c.ofa_w, &c.ofa_h) != 2) { c.ofa_w = 960; c.ofa_h = 540; }
    const std::wstring grid = S(L"ofa", L"grid", L"auto");
    c.ofa_grid = grid == L"auto" ? 0 : _wtoi(grid.c_str());
    c.zero_below = F(L"ofa", L"zero_below", c.zero_below);
    c.ofa_dll = S(L"ofa", L"dll_path", L"");

    c.feather = I(L"ui", L"feather", c.feather);
    c.nrects = 0;
    for (int i = 1; i <= 16; ++i)
    {
        wchar_t key[16]; swprintf_s(key, L"rect%d", i);
        const std::wstring r = S(L"ui", key, L"");
        UiRect rc;
        if (swscanf_s(r.c_str(), L"%d,%d,%d,%d", &rc.x0, &rc.y0, &rc.x1, &rc.y1) == 4) c.rects[c.nrects++] = rc;
    }
    c.mask = B(L"ui", L"mask", c.mask);
    c.mask_every = I(L"ui", L"mask_every", c.mask_every);
    c.toast = B(L"ui", L"toast", c.toast);
    c.toast_scale = I(L"ui", L"toast_scale", c.toast_scale);
    c.hud = B(L"ui", L"hud", c.hud);
    const std::wstring hc = S(L"ui", L"hud_corner", L"tl");
    c.hud_corner = hc == L"tl" ? 0 : hc == L"bl" ? 2 : hc == L"br" ? 3 : 1;
    c.hud_scale = I(L"ui", L"hud_scale", c.hud_scale);

    c.fg_enabled = B(L"fg", L"enabled", c.fg_enabled);
    c.fg_multiplier = I(L"fg", L"multiplier", c.fg_multiplier);
    c.fg_pacing_vblank = S(L"fg", L"pacing", c.fg_pacing_vblank ? L"vblank" : L"timer") != L"timer";
    c.fg_mv_dilated = B(L"fg", L"mv_dilated", c.fg_mv_dilated);
    c.fg_phase_ms = F(L"fg", L"phase_ms", c.fg_phase_ms);

    c.overlay_mode = S(L"overlay", L"mode", c.overlay_mode == 1 ? L"direct" : L"composed") == L"direct" ? 1 : 0;
    c.exclude_from_capture = B(L"overlay", L"exclude_from_capture", c.exclude_from_capture);
    c.reassert_topmost_every = I(L"overlay", L"reassert_topmost_every", c.reassert_topmost_every);

    c.hk_toggle = ParseHotkey(S(L"hotkeys", L"toggle", L"F9"), VK_F9);
    c.hk_wipe = ParseHotkey(S(L"hotkeys", L"wipe", L"F10"), VK_F10);
    c.hk_reload = ParseHotkey(S(L"hotkeys", L"reload", L"F11"), VK_F11);
    c.hk_quit = ParseHotkey(S(L"hotkeys", L"quit", L"Ctrl+F12"), VK_F12);
    c.hk_fg = ParseHotkey(S(L"hotkeys", L"fg", L"F8"), VK_F8);
    c.hk_hud = ParseHotkey(S(L"hotkeys", L"hud", L"F7"), VK_F7);

    c.log_file = S(L"log", L"file", c.log_file);
    c.stats_every = I(L"log", L"stats_every", c.stats_every);
    c.gpu_timestamps = B(L"log", L"gpu_timestamps", c.gpu_timestamps);
    c.selftest = B(L"log", L"selftest", c.selftest);
    return (pa ? 1 : 0) | (pp ? 2 : 0);
}

bool ConfigNeedsRebuild(const Config& a, const Config& b)
{
    const NrTuning &x = a.tuning, &y = b.tuning;
    return a.work_w != b.work_w || a.work_h != b.work_h || a.create_style != b.create_style || a.param_block != b.param_block ||
           x.preset != y.preset || x.style != y.style || x.intensity != y.intensity || x.local_tone != y.local_tone ||
           x.local_structure != y.local_structure || x.skin_structure != y.skin_structure || x.auto_mask != y.auto_mask || x.ui_correction != y.ui_correction;
}

bool ConfigNeedsRestart(const Config& a, const Config& b)
{
    return a.dda != b.dda || a.overlay_mode != b.overlay_mode || a.cursor != b.cursor ||
           a.border != b.border || a.window_class != b.window_class || a.window_title != b.window_title;
}

NrConfig ConfigToNr(const Config& c)
{
    NrConfig n; n.work_w = c.work_w; n.work_h = c.work_h;
    n.create_style = c.create_style == 0 ? NrCreateA : NrCreateB;
    n.block = (NrParamBlock)c.param_block;
    n.tuning = c.tuning;
    return n;
}
