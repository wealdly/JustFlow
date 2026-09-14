// Click-through overlay: WS_POPUP topmost/noactivate/toolwindow window on its own thread with a
// flip-model swapchain (R8G8B8A8_UNORM, 2 buffers, tearing allowed) created on the Gpu queue.
// WS_EX_LAYERED|WS_EX_TRANSPARENT are added AFTER swapchain creation (flip model refuses layered
// at create). Hidden until the first Present. Global hotkeys are registered on the same thread.
#pragma once
#include "d3d.h"

struct Overlay;

struct HotkeyDef { int id; UINT mods; UINT vk; };   // mods = MOD_CONTROL|MOD_ALT|MOD_SHIFT (MOD_NOREPEAT added)

Overlay* OverlayCreate(Gpu& g, HWND target, UINT w, UINT h, const HotkeyDef* keys, int nkeys, bool exclude_from_capture);
void     OverlayDestroy(Overlay* o);

// Backbuffer to copy into this frame (state PRESENT at rest; caller transitions to COPY_DEST and back).
ID3D12Resource* OverlayBackbuffer(Overlay* o);
// Present(0, ALLOW_TEARING). Reveals the window on the first successful present. Returns false on
// DXGI failure (device removed etc.).
bool OverlayPresent(Overlay* o);
// Reposition over the target's DWMWA_EXTENDED_FRAME_BOUNDS, hide while the target is iconic,
// re-assert topmost every `reassert_every` calls. Call once per frame (or per second when idle).
void OverlayFollow(Overlay* o, int reassert_every);
// Recreate swapchain buffers at a new size (after CaptureSizeChanged). Waits for GPU idle.
bool OverlayResize(Overlay* o, UINT w, UINT h);
// Hotkey polling: returns true once per press (consumes the flag).
bool OverlayHotkey(Overlay* o, int id);
// Quit requested (WM_CLOSE / quit hotkey handled by main via OverlayHotkey).
HWND OverlayHwnd(Overlay* o);
// QPC of the last successful Present call and DXGI frame statistics scanout QPC (0 if unknown).
void OverlayTimes(Overlay* o, LONGLONG& present_qpc, LONGLONG& scanout_qpc);
