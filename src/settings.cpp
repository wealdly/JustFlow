// The settings table is the only place a user-facing key is described; the dialog is generated from
// it (one row per setting, one tab per group) and writes each key back through ConfigIsAppKey, so
// adding a setting means adding a row here and reading it in config.cpp.
//
// Diagnostics stay out of the table on purpose - create_style, param_block, warmup,
// rebuild_debounce_frames, selftest, fg.pacing, ui.rectN and the addon mask keys are ini-only.
#include "settings.h"
#include "config.h"
#include <commctrl.h>
#include <string>
#include <vector>

namespace
{
enum Type { Bool, Int, Float, Enum, Hotkey };

struct Setting
{
    int            tab;
    const wchar_t* sec;
    const wchar_t* key;
    const wchar_t* label;
    Type           type;
    const wchar_t* opts;    // Enum: "a|b|c"
    const wchar_t* def;     // shown when the key is absent from the file
};

const wchar_t* const kTabs[] = { L"Quality", L"Look", L"Frame gen", L"Performance", L"Display", L"Hotkeys" };
const int kTabCount = (int)(sizeof kTabs / sizeof *kTabs);

// Model resolutions: the measured cost is ~1.5 ms/MPix + 1 ms on a 5080, so 4K is a 60 fps tier.
const wchar_t* const kWork = L"auto|1920x1080|2560x1440|3200x1800|3840x2160";

const Setting kSettings[] = {
    { 0, L"nr", L"enabled",           L"Neural rendering",   Bool,  nullptr, L"0" },
    { 0, L"nr", L"work",              L"Model resolution",   Enum,  kWork,   L"auto" },
    { 0, L"nr", L"artcnn",            L"ArtCNN pre-pass",    Bool,  nullptr, L"0" },
    { 0, L"nr", L"sharpen",           L"Sharpen",            Float, nullptr, L"0.0" },
    { 0, L"nr", L"residual_strength", L"Effect strength",    Float, nullptr, L"1.0" },

    { 1, L"nr", L"chroma",            L"Keep model colour",  Float, nullptr, L"0.25" },
    { 1, L"nr", L"saturation",        L"Vibrance",           Float, nullptr, L"1.10" },
    { 1, L"nr", L"style",             L"Style (0-2)",        Int,   nullptr, L"0" },
    { 1, L"nr", L"intensity",         L"Intensity",          Float, nullptr, L"1.0" },
    { 1, L"nr", L"local_tone",        L"Local tone/colour",  Float, nullptr, L"0.2" },
    { 1, L"nr", L"local_structure",   L"Local structure",    Float, nullptr, L"1.0" },
    { 1, L"nr", L"skin_structure",    L"Skin structure",     Float, nullptr, L"-1" },
    { 1, L"nr", L"auto_mask",         L"Auto skin mask",     Bool,  nullptr, L"1" },
    { 1, L"nr", L"ui_correction",     L"UI correction",      Bool,  nullptr, L"1" },
    { 1, L"nr", L"exposure_scale",    L"Exposure scale",     Float, nullptr, L"1.0" },

    { 2, L"fg", L"enabled",           L"Frame generation",   Bool,  nullptr, L"1" },
    { 2, L"fg", L"multiplier",        L"Multiplier",         Enum,  L"2|3|4", L"2" },
    { 2, L"fg", L"mv_dilated",        L"Dilated motion",     Bool,  nullptr, L"1" },
    { 2, L"fg", L"phase_ms",          L"Present phase (ms)", Float, nullptr, L"0.0" },

    { 3, L"nr", L"max_fps",           L"FPS cap (0 = off)",  Int,   nullptr, L"0" },
    { 3, L"nr", L"mode",              L"Model mode",         Enum,  L"sync|async", L"sync" },
    { 3, L"nr", L"model_max_fps",     L"Async model cap",    Int,   nullptr, L"0" },
    { 3, L"nr", L"warp",              L"Async warp",         Float, nullptr, L"1.0" },
    { 3, L"gpu", L"adapter",          L"GPU (-1 = auto)",    Int,   nullptr, L"-1" },
    { 3, L"capture", L"mode",         L"Capture",            Enum,  L"dda|wgc", L"dda" },
    { 3, L"ofa", L"input",            L"Flow resolution",    Enum,  L"640x360|960x540|1280x720", L"960x540" },

    { 4, L"ui", L"hud",               L"Status HUD",         Bool,  nullptr, L"0" },
    { 4, L"ui", L"hud_corner",        L"HUD corner",         Enum,  L"tl|tr|bl|br", L"tl" },
    { 4, L"ui", L"hud_scale",         L"HUD size",           Int,   nullptr, L"3" },
    { 4, L"ui", L"toast",             L"Toasts",             Bool,  nullptr, L"1" },
    { 4, L"ui", L"toast_scale",       L"Toast size",         Int,   nullptr, L"4" },
    { 4, L"overlay", L"mode",         L"Overlay",            Enum,  L"composed|direct", L"composed" },
    { 4, L"capture", L"cursor",       L"Capture cursor",     Bool,  nullptr, L"0" },
    { 4, L"capture", L"border",       L"Capture border",     Bool,  nullptr, L"0" },

    { 5, L"hotkeys", L"toggle",       L"Neural rendering",   Hotkey, nullptr, L"F9" },
    { 5, L"hotkeys", L"fg",           L"Frame generation",   Hotkey, nullptr, L"F8" },
    { 5, L"hotkeys", L"hud",          L"Status HUD",         Hotkey, nullptr, L"F7" },
    { 5, L"hotkeys", L"wipe",         L"Wipe compare",       Hotkey, nullptr, L"F10" },
    { 5, L"hotkeys", L"reload",       L"Reload config",      Hotkey, nullptr, L"F11" },
    { 5, L"hotkeys", L"quit",         L"Quit",               Hotkey, nullptr, L"Ctrl+F12" },
};
const int kCount = (int)(sizeof kSettings / sizeof *kSettings);

const int ID_TAB = 50, ID_CTL0 = 1000, ID_LBL0 = 2000;

// "[Ctrl+][Alt+][Shift+]Key", Key = F1..F24 or one letter/digit. ParseHotkey in config.cpp is
// lenient (it falls back to a default vk); this is the strict check the user's typing needs.
bool ValidHotkey(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towupper(c);
    size_t pos;
    while ((pos = s.find(L'+')) != std::wstring::npos)
    {
        const std::wstring m = s.substr(0, pos); s = s.substr(pos + 1);
        if (m != L"CTRL" && m != L"ALT" && m != L"SHIFT") return false;
    }
    if (s.size() >= 2 && s[0] == L'F') { const int n = _wtoi(s.c_str() + 1); return n >= 1 && n <= 24 && s == L"F" + std::to_wstring(n); }
    return s.size() == 1 && iswalnum(s[0]);
}

// ---- ini -------------------------------------------------------------------------------------

const wchar_t* FileFor(const Setting& s, const wchar_t* app, const wchar_t* profile)
{
    return ConfigIsAppKey(s.sec, s.key) ? app : profile;
}

std::wstring Read(const wchar_t* file, const wchar_t* sec, const wchar_t* key, const wchar_t* def)
{
    if (!file || !*file) return def;
    wchar_t buf[256];
    GetPrivateProfileStringW(sec, key, def, buf, 256, file);
    std::wstring v = buf;
    while (!v.empty() && iswspace(v.back())) v.pop_back();
    return v;
}

// ---- presets ---------------------------------------------------------------------------------

struct PresetKey { const wchar_t *sec, *key, *val[kPresetCount]; };
// Only the cost dials. The look ([nr] style/intensity/...) is the user's, and no preset sets a
// frame-rate cap - a cap the user did not ask for reads as "the filter is slow".
const PresetKey kPreset[] = {
    // High performance runs NO DLSS model at all: ArtCNN alone carries the enhancement at ~2.3 ms
    // against the model's 12 ms in a real scene, which is the difference between fitting a 90 fps
    // budget alongside frame generation and not. The other three are model tiers.
    { L"nr",  L"enabled", { L"0",         L"1",         L"1",         L"1" } },
    { L"nr",  L"work",    { L"1920x1080", L"1920x1080", L"2560x1440", L"3200x1800" } },
    { L"nr",  L"artcnn",  { L"1",         L"0",         L"0",         L"1" } },
    { L"nr",  L"sharpen", { L"0.4",       L"0.3",       L"0.2",       L"0.0" } },
    { L"ofa", L"input",   { L"640x360",   L"960x540",   L"960x540",   L"1280x720" } },
};
const wchar_t* const kPresetNames[kPresetCount] = { L"High performance", L"Performance", L"Balanced", L"Quality" };

// ---- dialog template -------------------------------------------------------------------------

struct DlgData
{
    const wchar_t *app, *profile, *name;
    HWND           avoid = nullptr;
    std::wstring   initial[kCount];
    bool           wrote = false;
};

// Centre the dialog on a monitor that does not hold `avoid`. One monitor, or no game window: leave
// it wherever DS_CENTER put it.
struct MonPick { HMONITOR skip; RECT work; bool found; };

void PlaceClearOf(HWND h, HWND avoid)
{
    if (!avoid || !IsWindow(avoid)) return;
    MonPick pick = { MonitorFromWindow(avoid, MONITOR_DEFAULTTONEAREST), {}, false };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL
    {
        MonPick* c = (MonPick*)lp;
        if (c->found || m == c->skip) return TRUE;
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(m, &mi)) { c->work = mi.rcWork; c->found = true; }
        return TRUE;
    }, (LPARAM)&pick);
    if (!pick.found) return;
    RECT r = {};
    if (!GetWindowRect(h, &r)) return;
    const int w = r.right - r.left, ht = r.bottom - r.top;
    SetWindowPos(h, nullptr, pick.work.left + ((pick.work.right - pick.work.left) - w) / 2,
                 pick.work.top + ((pick.work.bottom - pick.work.top) - ht) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

std::vector<WORD> BuildTemplate()
{
    std::vector<WORD> w;
    auto dw  = [&](DWORD v) { w.push_back(LOWORD(v)); w.push_back(HIWORD(v)); };
    auto str = [&](const wchar_t* s) { do w.push_back(*s); while (*s++); };
    // class as an atom (0xFFFF + atom) or, when `cls` is given, as a name string
    auto item = [&](DWORD style, int x, int y, int cx, int cy, WORD id, WORD atom, const wchar_t* cls, const wchar_t* text)
    {
        if (w.size() & 1) w.push_back(0);
        dw(style | WS_CHILD | WS_VISIBLE); dw(0);
        w.push_back((WORD)x); w.push_back((WORD)y); w.push_back((WORD)cx); w.push_back((WORD)cy);
        w.push_back(id);
        if (cls) str(cls); else { w.push_back(0xFFFF); w.push_back(atom); }
        str(text);
        w.push_back(0);
    };

    // Size to the LONGEST tab instead of a number I typed once: adding rows to a tab used to push
    // them off the bottom of the panel (the Look tab did exactly that when it gained two).
    int rows_per_tab[kTabCount] = {};
    for (const auto& s : kSettings) ++rows_per_tab[s.tab];
    int max_rows = 1;
    for (int r : rows_per_tab) if (r > max_rows) max_rows = r;
    const int tab_h = 16 * max_rows + 29;       // rows start at y = 30, 16 apart, + padding
    const int btn_y = 5 + tab_h + 6, dlg_h = btn_y + 22;

    dw(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU); dw(0);
    w.push_back((WORD)(1 + kCount * 2 + 2));                  // tab + label/control pairs + 2 buttons
    w.push_back(0); w.push_back(0); w.push_back(300); w.push_back((WORD)dlg_h);
    w.push_back(0); w.push_back(0); str(L"JustFlow settings");
    w.push_back(8); str(L"MS Shell Dlg");

    // The tab control comes first: CreateWindow puts each later sibling above it, so the rows draw
    // on top of the tab body without any z-order fixing.
    item(WS_TABSTOP, 5, 5, 290, tab_h, ID_TAB, 0, WC_TABCONTROLW, L"");

    int row[kTabCount] = {};
    for (int i = 0; i < kCount; ++i)
    {
        const Setting& s = kSettings[i];
        const int y = 30 + row[s.tab]++ * 16;
        item(SS_LEFT, 14, y + 2, 100, 9, (WORD)(ID_LBL0 + i), 0x0082, nullptr, s.label);
        if (s.type == Bool)
            item(BS_AUTOCHECKBOX | WS_TABSTOP, 120, y, 120, 10, (WORD)(ID_CTL0 + i), 0x0080, nullptr, L"");
        else if (s.type == Enum)
            item(CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 120, y, 110, 90, (WORD)(ID_CTL0 + i), 0x0085, nullptr, L"");
        else
            item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 120, y, s.type == Hotkey ? 110 : 70, 12, (WORD)(ID_CTL0 + i), 0x0081, nullptr, L"");
    }
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, 185, btn_y, 52, 15, IDOK, 0x0080, nullptr, L"OK");
    item(BS_PUSHBUTTON | WS_TABSTOP, 242, btn_y, 52, 15, IDCANCEL, 0x0080, nullptr, L"Cancel");
    return w;
}

void ShowTab(HWND h, int tab)
{
    for (int i = 0; i < kCount; ++i)
    {
        const int cmd = kSettings[i].tab == tab ? SW_SHOW : SW_HIDE;
        ShowWindow(GetDlgItem(h, ID_CTL0 + i), cmd);
        ShowWindow(GetDlgItem(h, ID_LBL0 + i), cmd);
    }
}

std::wstring CtlValue(HWND h, int i)
{
    const Setting& s = kSettings[i];
    if (s.type == Bool) return IsDlgButtonChecked(h, ID_CTL0 + i) == BST_CHECKED ? L"1" : L"0";
    wchar_t buf[256] = {};
    GetDlgItemTextW(h, ID_CTL0 + i, buf, 256);
    std::wstring v = buf;
    while (!v.empty() && iswspace(v.back())) v.pop_back();
    size_t j = 0; while (j < v.size() && iswspace(v[j])) ++j;
    return v.substr(j);
}

INT_PTR CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    DlgData* d = (DlgData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        d = (DlgData*)lp; SetWindowLongPtrW(h, GWLP_USERDATA, lp);
        HWND tab = GetDlgItem(h, ID_TAB);
        for (int t = 0; t < kTabCount; ++t)
        {
            TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = const_cast<wchar_t*>(kTabs[t]);
            SendMessageW(tab, TCM_INSERTITEMW, t, (LPARAM)&ti);
        }
        for (int i = 0; i < kCount; ++i)
        {
            const Setting& s = kSettings[i];
            const std::wstring v = Read(FileFor(s, d->app, d->profile), s.sec, s.key, s.def);
            d->initial[i] = v;
            const HWND c = GetDlgItem(h, ID_CTL0 + i);
            if (s.type == Bool) CheckDlgButton(h, ID_CTL0 + i, v != L"0" && !v.empty() ? BST_CHECKED : BST_UNCHECKED);
            else if (s.type == Enum)
            {
                int sel = -1;
                for (const wchar_t* p = s.opts; p; )
                {
                    const wchar_t* bar = wcschr(p, L'|');
                    const std::wstring opt(p, bar ? bar - p : wcslen(p));
                    const int idx = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opt.c_str());
                    if (_wcsicmp(opt.c_str(), v.c_str()) == 0) sel = idx;
                    p = bar ? bar + 1 : nullptr;
                }
                // A hand-edited value that is not in the list (a work= size of its own) stays put.
                if (sel < 0) { sel = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)v.c_str()); }
                SendMessageW(c, CB_SETCURSEL, sel, 0);
            }
            else SetDlgItemTextW(h, ID_CTL0 + i, v.c_str());
        }
        ShowTab(h, 0);
        PlaceClearOf(h, d->avoid);
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == ID_TAB && ((NMHDR*)lp)->code == TCN_SELCHANGE)
            ShowTab(h, (int)SendMessageW(GetDlgItem(h, ID_TAB), TCM_GETCURSEL, 0, 0));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        if (LOWORD(wp) != IDOK) return FALSE;
        for (int i = 0; i < kCount; ++i)                         // validate before writing anything
        {
            if (kSettings[i].type != Hotkey) continue;
            const std::wstring v = CtlValue(h, i);
            if (ValidHotkey(v)) continue;
            SendMessageW(GetDlgItem(h, ID_TAB), TCM_SETCURSEL, kSettings[i].tab, 0);
            ShowTab(h, kSettings[i].tab);
            MessageBoxW(h, (L"\"" + v + L"\" is not a hotkey.\n\nUse [Ctrl+][Alt+][Shift+]Key, where Key is "
                            L"F1..F24 or a single letter or digit.").c_str(), d->name, MB_ICONWARNING);
            SetFocus(GetDlgItem(h, ID_CTL0 + i));
            return TRUE;
        }
        for (int i = 0; i < kCount; ++i)
        {
            const Setting& s = kSettings[i];
            const std::wstring v = CtlValue(h, i);
            if (v == d->initial[i]) continue;                    // only what the user changed
            const wchar_t* file = FileFor(s, d->app, d->profile);
            if (!file || !*file) continue;
            if (!WritePrivateProfileStringW(s.sec, s.key, v.c_str(), file))
            {
                const std::wstring m = L"Could not write " + std::wstring(s.sec) + L"." + s.key + L" to\n" + file;
                MessageBoxW(h, m.c_str(), d->name, MB_ICONWARNING);
                break;
            }
            d->wrote = true;
        }
        EndDialog(h, 1);
        return TRUE;
    }
    return FALSE;
}
}   // namespace

bool SettingsDialog(HWND parent, const wchar_t* app_ini, const wchar_t* profile_ini, const wchar_t* app_name, HWND keep_clear_of)
{
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_TAB_CLASSES };
    InitCommonControlsEx(&ic);
    static const std::vector<WORD> tmpl = BuildTemplate();
    DlgData d; d.app = app_ini; d.profile = profile_ini; d.name = app_name; d.avoid = keep_clear_of;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (const DLGTEMPLATE*)tmpl.data(), parent, DlgProc, (LPARAM)&d);
    return d.wrote;
}

const wchar_t* PresetName(int i) { return i >= 0 && i < kPresetCount ? kPresetNames[i] : L""; }

void PresetApply(const wchar_t* profile_ini, int i)
{
    if (!profile_ini || !*profile_ini || i < 0 || i >= kPresetCount) return;
    for (const auto& k : kPreset) WritePrivateProfileStringW(k.sec, k.key, k.val[i], profile_ini);
}

int PresetCurrent(const wchar_t* profile_ini)
{
    if (!profile_ini || !*profile_ini) return -1;
    for (int i = 0; i < kPresetCount; ++i)
    {
        bool all = true;
        for (const auto& k : kPreset)
            if (_wcsicmp(Read(profile_ini, k.sec, k.key, L"").c_str(), k.val[i]) != 0) { all = false; break; }
        if (all) return i;
    }
    return -1;
}
