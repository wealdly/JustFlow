# JustFlow

An external neural-rendering and frame-generation layer for any game window on Windows.
JustFlow captures the game's output, runs NVIDIA's DLSS neural-rendering model on it, and
presents the result in a click-through overlay. It never touches the game process: no hooks,
no injection, no input. The same mechanisms OBS and overlay apps use, nothing more.

- **Neural rendering** (DLSS 5, feature 18) on a downscaled working copy, with the model's edit
  carried back to the native frame as a motion-warped residual. Fine detail stays native; lighting,
  tone and materials come from the model.
- **Frame generation** (DLSS frame generation, desktop path) on the composed frame with hardware
  optical-flow motion vectors, paced to the display's vblank.
- **Capture** by DXGI Desktop Duplication at the monitor's refresh rate (window capture fallback).
- **Per-game profiles**, global hotkeys, a tray menu, a live before/after wipe.

Tested on an RTX 5080 at 4K 240 Hz with World of Warcraft, Valheim and The Blood of Dawnwalker.

## Requirements

- Windows 11, an NVIDIA RTX 50 series card (the neural-rendering model refuses older architectures),
  driver 616.xx or newer.
- Two NVIDIA runtimes next to `justflow.exe`, **not included** (see NOTICE):
  - `nvngx_dlssnr.dll` — the DLSS neural-rendering runtime (310.8.0.0).
  - `nvngx_dlssg.dll` — the DLSS frame-generation runtime, from NVIDIA's public DLSS SDK
    repository (`lib/Windows_x86_64/rel/`). Both are Authenticode-signed by NVIDIA; JustFlow logs
    the version it loaded.
- The game in **borderless / windowed-fullscreen** mode. Exclusive fullscreen cannot be overlaid.

## Build

MSVC 2022 Build Tools, Windows SDK 10.0.26100, CMake 3.25+, Ninja. `build.cmd` configures and
builds into `build\`. Shaders are compiled by fxc at build time.

## Use

```
justflow.exe                        auto-picks a profile from profiles\ (see below)
justflow.exe --ini profiles\valheim.ini
justflow.exe --bench frame.png --frames 120   pipeline timing on a PNG, no game needed
```

Profiles are `profiles\*.ini` next to the exe (`wow`, `valheim`, `dawnwalker` ship). Without
`--ini`, the first profile whose game window is open right now is used, else `wow`, else the
first one. The tray menu switches profiles live, toggles the effect and frame generation, sets the
FG multiplier, opens the profile and the log, and edits the hotkeys (written back to the profile).

Keys (configurable in the profile and the tray menu): F9 toggle the effect, F10 cycle the
before/after wipe, F8 frame generation, F11 reload the profile, Ctrl+F12 quit.

The `[stats]` lines in the log report capture rate, model cost, generation cadence, dropped
frames, and the age of the frame on screen relative to the game's own present.

## Budget, in one paragraph

Everything shares one GPU. Per real frame the game keeps its own cost; the model costs about
1.5 ms per megapixel of working size (4 ms at 1080p, 6.5 ms at 1440p on a 5080) and each generated
frame about 3 ms at 4K. In `mode=async` the model runs on its own queue at whatever rate is left
over and its residual is re-applied every frame, so its average cost per frame drops to its rate
over the base rate. Cap the game's frame rate so the sum fits; the log tells you when it does not.

## On anti-cheat

JustFlow reads the game window's position, captures the screen through the OS, draws its own
window on top, and listens for hotkeys. It does not open the game process, read or write its
memory, hook it, or send input. That is the same class of software as OBS window capture,
Discord's overlay, and Lossless Scaling, which are used with online games every day; the author
has run Lossless Scaling with World of Warcraft for years without issue. JustFlow does strictly
less than those: OBS's game-capture mode and the Discord overlay inject a hook DLL into the game,
JustFlow never does.

The import tables of both binaries are auditable with `dumpbin /imports`: no OpenProcess,
ReadProcessMemory, WriteProcessMemory, CreateRemoteThread, SetWindowsHookEx, SendInput or any
networking library. The optional WoW addon only reads frame positions and draws a texture; it calls
no protected or automation API and has an off switch (`/justflow off`).

Whether a given game's terms allow overlays is between you and its publisher.

## Credits

Capture bridge, overlay recipe, optical-flow session, desktop frame-generation path and the
caller-gate forwarder are derived from NeuralScreen (MIT). See NOTICE for all third-party terms.
