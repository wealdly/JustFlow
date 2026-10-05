# JustFlow

An external neural-rendering and frame-generation layer for any game window on Windows.
JustFlow captures the game's output, runs NVIDIA's DLSS neural-rendering model on it, and
presents the result in a click-through overlay. It never touches the game process: no hooks,
no injection, no input. The same mechanisms OBS and overlay apps use, nothing more.

- **Neural rendering** (DLSS 5, feature 18) on a downscaled working copy, with the model's edit
  carried back to the native frame as a motion-warped residual. Fine detail stays native; lighting,
  tone and materials come from the model.
- **Frame generation** (DLSS frame generation, desktop path) on the composed frame, paced to the display's
  vblank. DLSS-G measures the motion itself, so it costs no optical flow of ours.
  Generated frames get the UI copied back out of the real frame, so text and action bars do not
  smear with the world.
  `[fg] engine=warp` swaps DLSS-G for extrapolation: the newest frame pushed ahead along its own
  optical flow. Nothing is held back, so it adds no latency (DLSS-G interpolates and shows every real
  frame half an interval late), and it costs about half the GPU time (0.8 vs 1.7 ms per real frame
  at 4K). It is a little less exact: scored against the true frame, 1.21 vs 1.09 grey levels at
  normal motion, 4.2 vs 3.2 on fast motion, where repeating the frame scores 5.9 / 14.4.
  `[fg] engine=latewarp` is Reflex 2's idea from outside the game: at every refresh the newest frame is
  re-projected by NVIDIA Frame Warp (`nvngx_latewarp.dll` next to the exe, not included) to where the
  mouse has turned the camera since the game drew it, so turning answers at the display's rate. It
  reads raw mouse input - its own hidden window, mouse only, read-only, only while this engine runs - and
  learns pixels-per-count and the game's input delay from our optical flow, separately with and without
  a button held (WoW turns only with a button held, so cursor movement never warps). It warps nothing
  until that fit is reliable. The warp is a camera rotation: exact for first-person mouse-look; for
  third-person cameras, what our flow shows pinned to the camera (the character) is held still.
  `[fg] engine=video` is for video and other 2D content: every refresh shows the in-between moment the
  video's own clock calls for (24 fps on 240 Hz = 10 even steps; 23.976 drifts smoothly), interpolated
  along our flow with fallbacks to the nearer frame where the flow fails and exact copies of unchanged
  pixels (subtitles). It shows the video one frame late - fine for video, not for games. The `desktop`
  profile uses it. A repeated capture (a browser repainting the same video frame) is not a frame, and a
  scene cut resets the model and frame generation instead of smearing the old shot into the new one. Against the true middle frame: 2.3 / 7.2 grey levels (normal / fast motion), vs
  5.8 / 14.1 for holding the frame.
- **Full desktop**: the `desktop` profile captures the whole primary monitor (browsers, video players,
  desktop apps) instead of one window; HDR desktops are converted at Windows' SDR white level. With the
  neural layer on it runs the model at full resolution, once per distinct video frame - the GPU is mostly
  idle during video, so the ~15 ms a 4K model costs fits between 24 or 30 fps frames. DRM-protected
  video is blacked out by Windows in every capture, so it shows black.
- **Capture** by Windows.Graphics.Capture, uncapped on Windows 11 24H2+ (`MinUpdateInterval`), or DXGI
  Desktop Duplication where that is unavailable (`[capture] mode=auto|wgc|dda`).
- **Per-game profiles**, global hotkeys, a tray menu, a live before/after wipe.

Out of the box it runs **frame generation on its own: neural layer off, filter layer off, frame
generation on** (`[nr] enabled=0`, `[filters] enabled=0`, `[fg] enabled=1`) - the cheapest layer
(about 1.7 ms a real frame at 4K). The profiles keep their neural and filter tuning, so F9 / F6 bring
those layers back exactly as set; the model costs about 3.8 ms at 1080p and 5.7 ms at 1440p on an
idle card, more with a game already loading the GPU. F9 / F6 / F8 switch the three layers and every
toggle is remembered; with all of them off JustFlow goes dormant -
overlay hidden, capture released - rather than sit on top of the game as a slower copy of it.

Frame generation only pays when it is really multiplying the frame rate, so a governor pauses it
below 1.5x gain and above 90 fps in (`[fg] max_input_fps`), where doubling is latency for smoothness
nobody sees. Many games ship their own - with real depth and motion vectors, which an external tool
cannot match. **If the game has its own frame generation, use that and turn ours off (F8)**:
stacking the two interpolates between interpolated frames.

That case has a second consequence. The overlay is opaque, so JustFlow's pipeline rate is the rate
you see. A game presenting 135-270 fps with its own FG outruns a model that takes 6-10 ms a frame,
and a per-frame (sync) model would *lower* the visible frame rate. Set **Model every Nth frame**
to 2-4 there: the main path stays around 0.2 ms, the model runs beside it on its own queue, and its
edit is warped onto every frame. The shipped Dawnwalker profile does this (`model_every=3`).

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
justflow.exe --settings              the settings window on its own, no GPU and no game
justflow.exe --preset 2              write a quality preset into the profile (0..3) and exit
justflow.exe --bench frame.png --frames 120   pipeline timing on a PNG, no game needed
```

Nothing has to be edited by hand: **Settings...** in the tray menu opens a tabbed window over the
same keys, one tab per pipeline layer (Neural, Model, Filters, Frame gen, Display, System,
Hotkeys), on a monitor the game is not
on, so it never lands over what you are playing. It writes each key to the file it belongs
in, and applies it on OK - live keys immediately, create-latched ones (model size, look) as one
debounced rebuild, and the handful the capture and overlay only read when they are created
(capture mode, window match, cursor, border, overlay mode) by rebuilding the pipeline in place.

**Quality** in the same menu sets the four cost dials at once - High performance,
Performance, Balanced, Quality (model resolution and cadence, sharpen, flow resolution). No preset
touches the look dials, and none sets a frame-rate cap.

The model tiers trade **time, not resolution**. The model's edit is smooth, so it survives being
motion-warped for several frames, while a smaller model resolution makes the model itself behave
differently (45-89% of the native edit, depending on content). Measured on a moving 4K sequence:

| tier | setting | native edit reproduced | average model cost |
|---|---|---|---|
| Performance | `work=auto` (50% = 1080p on 4K), every frame | 57% | 3.8 ms |
| Balanced | `work=100%`, `model_every=6` | 76% | 2.0 ms |
| Quality | `work=100%`, `model_every=3` | 80% | 4.1 ms |

`[nr] work` is the **model resolution**, a share of the screen: `100%`, `67%`, `50%`, `33%` or `auto`
(nearest 1080p). It never upscales: the model's result is always applied to the full-resolution
frame. `[nr] passes=1..3` runs the model that many times per frame, each pass refining the last, at
that many times the cost (4K: 14 ms, 29 ms with 2 passes).

Performance remains the small per-frame evaluate because it cannot hitch a game sharing the GPU; a
native evaluate is one ~12 ms block at 4K. If Balanced or Quality shows a rhythmic stutter, use Performance.

### Three layers, three switches

The pipeline is three independent layers, each with one on/off that reaches into nothing else:

| Layer | Switch | Key | What it does |
|---|---|---|---|
| Neural | `[nr] enabled` | F9 | the DLSS model, at work resolution, composed back as a residual |
| Filters | `[filters] enabled` | F6 | deband (on the capture, before the model), then RCAS sharpen and vibrance, at native resolution, UI rects untouched |
| Frame gen | `[fg] enabled` | F8 | frames between the game's: DLSS-G, warp, latewarp or video (`[fg] engine`); the UI is copied back onto every one |

They apply in that order, with the UI mask between filters and frame generation. The order is not
arbitrary: deband runs first so the model never sees compression steps as detail, sharpening has to
see what the model produced, vibrance grades after the sharpen rather than feeding it exaggerated
contrast, and frame generation runs last because it works on the frame as the viewer sees it. The
HUD and toasts are drawn over every presented frame, generated or real, so they are never interpolated.

All eight combinations are valid — a layer that is off passes its input straight through. Turning
a layer off keeps its tuned values, so an A/B costs nothing. The HUD names the live layers.

Two ini files, both next to the exe:

- `justflow.ini` — **app settings**, this machine, every game: `[hotkeys]`, `[ui]` toast and HUD
  (`toast`, `toast_scale`, `hud`, `hud_corner`, `hud_scale`), `[overlay]`, `[log]` `stats_every` /
  `gpu_timestamps` / `selftest`, `[app] profile` (startup profile: `auto` or a profile name),
  `[ofa] dll_path`, `[nr] param_block`.
- `profiles\<game>.ini` — **game settings**: `[capture]` (mode, window match, cursor, border),
  `[nr]` (model resolution, passes, look, mode, warp, caps...), `[filters]` (deband, sharpen,
  saturation), `[ofa]` input/grid/zero_below,
  `[ui]` mask/mask_every/feather/rectN, `[fg]`, and `[log] file`.

Each key is read from its own file only; an app key left in a profile is ignored and the log names
it. Profiles hold quality dials only; the layer switches are app-wide. Profiles `wow`, `wow-4k`,
`valheim`, `dawnwalker` and `desktop` ship (`desktop` is never auto-picked). Without `--ini` (and with `[app] profile=auto`),
the first profile whose game window is open right now is used, else `wow`, else the first one.
The tray menu switches profiles live, toggles the effect and frame generation, sets the FG
multiplier and the quality preset, opens the settings and hotkey windows, and opens either ini
and the log for hand editing.

Keys (configurable in `justflow.ini` and the tray menu): F9 neural layer, F6 filter layer, F8
frame generation, F10 cycle the before/after wipe, F11 reload both files, F7 status HUD,
Ctrl+F12 quit.
Every state change shows a 2 s toast at the top of the frame (`[ui] toast=0` turns it off); the
HUD (`[ui] hud`, corner and scale there too) shows capture/output rate, frame age, model cost and
mode, FG and mask state. The look defaults keep the game's own art and spend the model on detail: `[nr] style=0`
(Standard, the least restyling of the three), `local_tone=0.20` (it remaps tone and colour) and
`local_structure=1.00` (it adds detail). Raise `local_tone`, or `style` to 1 (Natural) or 2
(Cinematic), only when a shift in look is actually wanted - the Look tab in the settings window
has all of them, and every row has a tooltip. `[filters] sharpen=0.3..0.5` in the profile adds an
RCAS sharpen after the effect (UI rects excluded), live on F11.

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
networking library.

Whether a given game's terms allow overlays is between you and its publisher.

## Credits

Capture bridge, overlay recipe, optical-flow session, desktop frame-generation path and the
caller-gate forwarder are derived from NeuralScreen (MIT). See NOTICE for all third-party terms.
