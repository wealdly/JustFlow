// Two ini layers, loaded with GetPrivateProfile*; reload on hotkey.
//   justflow.ini (next to the exe) = APP: [app], [hotkeys], [overlay], [ui] toast*/hud*, [log] stats_every/
//     gpu_timestamps/selftest, [ofa] dll_path, [nr] create_style/param_block (machine-level, from the spike).
//   profiles\<game>.ini = GAME: everything else. The table in config.cpp (kAppKeys) is the authority.
#pragma once
#include <windows.h>
#include <string>
#include "compose.h"
#include "ngx_nr.h"

struct HotkeySpec { UINT mods = 0; UINT vk = 0; };

struct Config
{
    // [app] (justflow.ini)
    std::wstring profile = L"auto";        // startup profile: auto (first whose window is up, else wow, else first) | <name>
    // [gpu] (app layer: which card, not which game)
    int   gpu_adapter = -1;                // -1 = the first usable NVIDIA adapter; else the index the log prints
    // [capture]
    std::wstring window_class = L"";      // profiles name the game; the binary carries no game-specific strings
    std::wstring window_title = L"";
    bool cursor = false, border = false;
    bool dda = true;                       // capture.mode: dda (monitor refresh rate) | wgc (60 Hz ceiling)
    // [nr]
    // Two separate things, because one key could not say "effect on, model off, ArtCNN on":
    //   enabled = the effect as a whole, which is what F9 toggles and what persists.
    //   model   = use the DLSS neural-rendering model (expensive: ~12 ms in a real scene).
    //   artcnn  = use ArtCNN (~2.3 ms), on its own or ahead of the model.
    bool  nr_enabled = true;
    bool  nr_model = false;
    UINT  work_w = 0, work_h = 0;          // 0 = auto (from spike results / default 2560x1440)
    int   create_style = 1;                // 0 = A (NeuralScreen set), 1 = B (plain)   [app layer]
    int   param_block = 1;                 // 1 Allocate, 2 Capability, 3 Own           [app layer]
    NrTuning tuning;
    float exposure_scale = 1.0f;
    float residual_strength = 1.0f;
    float chroma = 1.0f;                   // 0..1: how much of the model's colour change to keep (0 = its luma only)
    float saturation = 1.0f;               // vibrance of the composed frame, applied last (1 = untouched)
    int   warmup = 8;
    int   rebuild_debounce_frames = 30;
    int   max_fps = 0;                     // 0 = process every captured frame; else cap the pipeline rate
    bool  nr_async = false;                // mode=sync (evaluate per frame) | async (decoupled model thread, residual compose)
    float warp = 1.0f;                     // async: residual sampled at uv + mv * warp (this frame's mv only, see PipelineFrame)
    int   model_max_fps = 0;               // async: throttle the model thread (0 = as fast as the leftover GPU allows)
    float sharpen = 0.0f;                  // CAS-style sharpen after the compose, 0 = off (0.3-0.5 typical); live (F11)
    bool  artcnn = false;                  // ArtCNN C4F16_DS luma pass on the model input (CsArtCnn nr_in -> nr_in2); live (F11)
    // [ofa]
    UINT  ofa_w = 960, ofa_h = 540;
    int   ofa_grid = 0;
    float zero_below = 0.5f;
    std::wstring ofa_dll;                  // [app layer]
    // [ui] (toast*/hud* are app layer, the rest profile)
    int   feather = 12;
    int   nrects = 0;                      // manual rects (rect1..rect16); the addon mask adds up to 64 more at compose time
    UiRect rects[16] = {};
    bool  mask = false;                    // decode the JustFlow addon's UI-mask strip from the capture
    int   mask_every = 1;                  // read the strip back every N frames
    bool  toast = true;                    // on-screen toast for state changes (top-centre, 2 s)
    int   toast_scale = 4;                 // font pixel scale (4 = 32 px glyphs at 4K)
    bool  hud = false;                     // status HUD at start (F7 toggles)
    int   hud_corner = 0;                  // 0 tl, 1 tr, 2 bl, 3 br
    int   hud_scale = 3;
    // [fg]
    bool  fg_enabled = true;
    int   fg_multiplier = 2;               // presented frames per rendered frame, 2..4
    bool  fg_pacing_vblank = true;         // pacing=vblank | timer
    bool  fg_mv_dilated = true;            // DLSS-G motionVectorsDilated (see fg.cpp Evaluate)
    float fg_phase_ms = 0.0f;              // shifts every scheduled present target (negative = earlier); live (F11)
    // [overlay] (app layer)
    // mode: 0 composed (layered, DWM-composed) | 1 direct (monitor-sized, no redirection bitmap;
    // falls back to layered when the click-through self-test fails). Both stay click-through.
    int   overlay_mode = 0;
    bool  exclude_from_capture = false;
    int   reassert_topmost_every = 300;
    // [hotkeys] (app layer)
    HotkeySpec hk_toggle, hk_wipe, hk_reload, hk_quit, hk_fg, hk_hud;
    // [log] (file is per profile, the rest app layer)
    std::wstring log_file = L"justflow.log";
    int   stats_every = 180;
    bool  gpu_timestamps = true;
    bool  selftest = false;                // run ComposeSelfTest once at startup
};

// Reads every key from the file of its layer only (`app` = justflow.ini, `profile` = profiles\<game>.ini), so an
// app key inside a profile is ignored. Missing keys/files keep the defaults above; either path may be null.
// Returns a mask of the files found: 1 = app, 2 = profile.
int ConfigLoad(const wchar_t* app, const wchar_t* profile, Config& c);
// "sec.key, sec.key" of app-layer keys present in `profile` (ignored by ConfigLoad); empty = none.
std::wstring ConfigStrayKeys(const wchar_t* profile);
// Which file a key belongs in: true = justflow.ini, false = profiles\<game>.ini. The settings UI
// writes through this so the layer split has one authority (kAppKeys in config.cpp).
bool ConfigIsAppKey(const wchar_t* sec, const wchar_t* key);
// True if a key that is latched at CreateFeature differs (work size, style, block, tuning).
bool ConfigNeedsRebuild(const Config& a, const Config& b);
// True if a key that is only read when the capture and the overlay are created differs, so a
// reload has to tear the pipeline down and build it again. Deliberately NOT exclude_from_capture:
// opening a DDA capture forces it on in the live config, and comparing that against the ini would
// restart on every reload.
bool ConfigNeedsRestart(const Config& a, const Config& b);
// Fills an NrConfig from the config (work size must already be resolved).
NrConfig ConfigToNr(const Config& c);
// "[Ctrl+][Alt+][Shift+]Key" (Key = F1..F24 or a letter/digit) <-> HotkeySpec. Unparsable key -> def_vk.
HotkeySpec   ParseHotkey(std::wstring s, UINT def_vk);
std::wstring FormatHotkey(const HotkeySpec& h);
