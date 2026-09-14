// Profile ini schema (profiles\*.ini). Loaded with GetPrivateProfile*; reload on hotkey.
#pragma once
#include <windows.h>
#include <string>
#include "compose.h"
#include "ngx_nr.h"

struct HotkeySpec { UINT mods = 0; UINT vk = 0; };

struct Config
{
    // [capture]
    std::wstring window_class = L"GxWindowClass";
    std::wstring window_title = L"World of Warcraft";
    bool cursor = false, border = false;
    bool dda = true;                       // capture.mode: dda (monitor refresh rate) | wgc (60 Hz ceiling)
    // [nr]
    bool  nr_enabled = true;
    UINT  work_w = 0, work_h = 0;          // 0 = auto (from spike results / default 2560x1440)
    int   create_style = 1;                // 0 = A (NeuralScreen set), 1 = B (plain)
    int   param_block = 1;                 // 1 Allocate, 2 Capability, 3 Own
    NrTuning tuning;
    float exposure_scale = 1.0f;
    float residual_strength = 1.0f;
    int   warmup = 8;
    int   rebuild_debounce_frames = 30;
    int   max_fps = 0;                     // 0 = process every captured frame; else cap the pipeline rate
    bool  nr_async = false;                // mode=sync (evaluate per frame) | async (decoupled model thread, residual compose)
    float warp = 1.0f;                     // async: residual sampled at uv + mv * warp (this frame's mv only, see PipelineFrame)
    int   model_max_fps = 0;               // async: throttle the model thread (0 = as fast as the leftover GPU allows)
    // [ofa]
    UINT  ofa_w = 960, ofa_h = 540;
    int   ofa_grid = 0;
    UINT  cost_reject = 0;
    float zero_below = 0.5f;
    std::wstring ofa_dll;
    // [ui]
    int   feather = 12;
    int   nrects = 0;
    UiRect rects[16] = {};
    // [fg]
    bool  fg_enabled = false;
    int   fg_multiplier = 2;               // presented frames per rendered frame, 2..4
    bool  fg_pacing_vblank = true;         // pacing=vblank | timer
    bool  fg_mv_dilated = true;            // DLSS-G motionVectorsDilated (see fg.cpp Evaluate)
    float fg_phase_ms = 0.0f;              // shifts every scheduled present target (negative = earlier); live (F11)
    int   fg_anchor_delay_slots = 0;       // 1 = first generated frame gets a full slot after the model pass; live (F11)
    // [overlay]
    bool  overlay_direct = false;          // mode=composed (layered, DWM-composed) | direct (monitor-sized, independent-flip capable)
    bool  exclude_from_capture = false;
    int   reassert_topmost_every = 300;
    // [hotkeys]
    HotkeySpec hk_toggle, hk_wipe, hk_reload, hk_quit, hk_fg;
    // [log]
    std::wstring log_file = L"justflow.log";
    int   stats_every = 180;
    bool  gpu_timestamps = true;
    bool  selftest = false;                // run ComposeSelfTest once at startup
};

// Reads `path`; missing keys keep the defaults above. Returns false if the file is absent.
bool ConfigLoad(const wchar_t* path, Config& c);
// True if a key that is latched at CreateFeature differs (work size, style, block, tuning).
bool ConfigNeedsRebuild(const Config& a, const Config& b);
// Fills an NrConfig from the config (work size must already be resolved).
NrConfig ConfigToNr(const Config& c);
// "[Ctrl+][Alt+][Shift+]Key" (Key = F1..F24 or a letter/digit) <-> HotkeySpec. Unparsable key -> def_vk.
HotkeySpec   ParseHotkey(std::wstring s, UINT def_vk);
std::wstring FormatHotkey(const HotkeySpec& h);
