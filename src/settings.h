// Settings dialog and quality presets. Both read and write the ini files directly (the tray owns the
// paths); the caller reloads afterwards, so nothing here touches the running pipeline.
#pragma once
#include <windows.h>

// Modal. Writes only the keys the user changed, each into the file of its layer (ConfigIsAppKey).
// Returns true if anything was written - the caller should reload.
bool SettingsDialog(HWND parent, const wchar_t* app_ini, const wchar_t* profile_ini, const wchar_t* app_name);

// Quality presets: the cost dials (model resolution, ArtCNN, sharpen, flow resolution) written into
// the game profile. 0 high performance, 1 performance, 2 balanced, 3 quality.
enum { kPresetCount = 4 };
const wchar_t* PresetName(int i);
void PresetApply(const wchar_t* profile_ini, int i);
// The preset whose keys all match the profile, else -1 (a hand-edited profile matches nothing).
int  PresetCurrent(const wchar_t* profile_ini);
