// Settings dialog and quality presets. Both read and write the ini files directly (the tray owns the
// paths); the caller reloads afterwards, so nothing here touches the running pipeline.
#pragma once
#include <windows.h>

// Modal. Writes only the keys the user changed, each into the file of its layer (ConfigIsAppKey).
// Returns true if anything was written - the caller should reload.
// keep_clear_of = the game window (may be null): the dialog opens on a monitor that does not hold
// it, so opening settings mid-game does not put a window over what is being played.
// on_apply(ctx): called from the Apply button after a write, with the dialog STILL OPEN, so a change
// can be tried and adjusted without reopening the window each time. The caller reloads in it.
// What Apply already delivered is not reported again: the return value covers only what OK wrote.
bool SettingsDialog(HWND parent, const wchar_t* app_ini, const wchar_t* profile_ini, const wchar_t* app_name,
                    HWND keep_clear_of = nullptr, void (*on_apply)(void*) = nullptr, void* ctx = nullptr);

// Lists the open top-level windows and writes a profile for the one picked, then points
// [app] profile at it. Window title and class only - deliberately no OpenProcess, so the binary
// keeps its clean import table. Returns true if a profile was written.
bool NewProfileDialog(HWND parent, const wchar_t* profiles_dir, const wchar_t* app_ini, const wchar_t* app_name);

// Quality presets: the cost dials (model resolution and cadence, sharpen, flow resolution) written into
// the game profile. 0 high performance, 1 performance, 2 balanced, 3 quality.
enum { kPresetCount = 4 };
const wchar_t* PresetName(int i);
void PresetApply(const wchar_t* profile_ini, int i);
// The preset whose keys all match the profile, else -1 (a hand-edited profile matches nothing).
int  PresetCurrent(const wchar_t* profile_ini);
