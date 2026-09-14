// Click-through overlay: WS_POPUP topmost/noactivate/toolwindow window on its own thread with a
// flip-model swapchain (R8G8B8A8_UNORM, 2 buffers, tearing allowed). The swapchain lives on its
// OWN direct queue: backbuffer copies and Present never queue behind NR / FG work on Gpu::queue.
// Two modes:
//   composed: WS_EX_LAYERED|WS_EX_TRANSPARENT added AFTER swapchain creation (flip model refuses
//             layered at create), window follows the target. A layered window is always DWM-composed.
//   direct:   no layered style, WS_EX_NOREDIRECTIONBITMAP, window = the whole monitor (needs w x h to
//             equal the monitor size) so DWM can grant independent flip / MPO to the swapchain (verify
//             with PresentMon: "Hardware: Independent Flip"; no public API reports it). Click-through
//             without WS_EX_LAYERED is self-tested at startup (cross-thread WindowFromPoint at the
//             centre must miss us). FAIL (the case on Win11 26200: a non-layered HTTRANSPARENT window
//             swallows foreign-thread clicks) keeps the monitor-sized window and adds the layered style
//             after the swapchain like composed does -> present_mode=direct_layered. Only
//             WDA_EXCLUDEFROMCAPTURE refusing while exclusion is required falls back to composed.
// Hidden until the first Present. Global hotkeys are registered on the same thread.
#pragma once
#include "d3d.h"

struct Overlay;

struct HotkeyDef { int id; UINT mods; UINT vk; };   // mods = MOD_CONTROL|MOD_ALT|MOD_SHIFT (MOD_NOREPEAT added)

Overlay* OverlayCreate(Gpu& g, HWND target, UINT w, UINT h, const HotkeyDef* keys, int nkeys, bool exclude_from_capture, bool direct);
void     OverlayDestroy(Overlay* o);
bool     OverlayIsDirect(const Overlay* o);   // the mode actually in effect (after fallback)

// Copies `src` (COPY_SOURCE, overlay size, RGBA8) into the current backbuffer on the present queue
// and Presents (0, ALLOW_TEARING). `after`/`after_value`: fence the present queue waits on first
// (the pipeline fence that completes `src`; nullptr = already complete). One caller at a time (main
// thread, or the FG presenter while an Fg exists). Reveals the window on the first successful
// present. Returns false on DXGI failure (device removed etc.).
bool OverlayPresent(Overlay* o, ID3D12Resource* src, ID3D12Fence* after, UINT64 after_value);
// `q` waits for the last OverlayPresent copy to finish reading its source (call before overwriting it).
void OverlayGuard(Overlay* o, ID3D12CommandQueue* q);
// CPU wait for both queues to go idle (before releasing anything a present copy may still read).
void OverlayDrain(Overlay* o);
// Blocks until the next vblank of the monitor under the overlay. False = no DXGI output matches
// (display on another adapter): caller falls back to timer pacing.
bool   OverlayWaitVBlank(Overlay* o);
double OverlayVBlankMs(Overlay* o);   // refresh period of that monitor (1000/60 if unknown)
// Reposition over the target's DWMWA_EXTENDED_FRAME_BOUNDS, hide while the target is iconic,
// re-assert topmost every `reassert_every` calls. Call once per frame (or per second when idle).
void OverlayFollow(Overlay* o, int reassert_every);
// Recreate swapchain buffers at a new size (after CaptureSizeChanged). Waits for GPU idle.
bool OverlayResize(Overlay* o, UINT w, UINT h);
// Hotkey polling: returns true once per press (consumes the flag).
bool OverlayHotkey(Overlay* o, int id);
// Replace the registered hotkeys (unregister + register on the window thread, synchronous).
// False = at least one RegisterHotKey failed (the others stay registered).
bool OverlaySetHotkeys(Overlay* o, const HotkeyDef* keys, int nkeys);
// Quit requested (WM_CLOSE / quit hotkey handled by main via OverlayHotkey).
HWND OverlayHwnd(Overlay* o);
// QPC of the last successful Present call and DXGI frame statistics scanout QPC (0 if unknown).
void OverlayTimes(Overlay* o, LONGLONG& present_qpc, LONGLONG& scanout_qpc);
