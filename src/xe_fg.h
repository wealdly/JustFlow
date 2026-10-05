// xe_fg: the live frame generator of justflow_xe. Real frames go in on the capture thread; a
// presenter thread puts a frame on screen at EVERY vblank, interpolated at the content time that
// vblank should show - not a fixed 2x/3x. On a fixed-refresh display a 45 fps game at 2x is 90 fps
// on a 120 Hz grid: three frames per four vblanks, one held twice as long, a steady judder. Sampling
// the motion at each vblank's own time is even at any base rate (~2.67 frames per real frame at
// 45 -> 120), and a generated frame costs ~0.3 ms, so the extra ones are cheap next to the flow.
//
// Main thread, per captured frame (XeFgSubmit, all on Gpu::queue):
//   copy into a ring of kFrames textures, luma pyramid, flow both ways against the previous frame,
//   and the two L0 grids copied into a ring of kPairs pairs, each tagged with the game's own present
//   times of its two frames (ta, tb).
// Presenter thread, per vblank (own GpuCtx queue):
//   content time c = now - hold (c never moves backwards). hold is the most any recent pair needed:
//   a pair ready at time s shows without a pause only if hold >= s - ta, so hold = the max of that
//   over the last 64 pairs - half a refresh (the slack of sampling at vblanks) + margin (default 0),
//   fast up, slow down. The pair with ta <= c <= tb is interpolated at
//   t = (c - ta) / (tb - ta) and presented through the overlay. c past the newest pair = the game
//   stalled: the newest real frame is held (t = 1) - or, with XeFgSetExtrapolate, pushed ahead first.
// Reuse: the main queue waits (GPU side) on the presenter's last read of a frame or pair before it
// overwrites it - the same scheme as fg.cpp's slots.
// Off (XeFgSetEnabled false), or the game already above max_in_fps: real frames only, each shown
// once on the vblank after it lands (passthrough; while disabled no flow is computed either).
#pragma once
#include "d3d.h"
#include "present.h"
#include <vector>

struct XeFg;

// Creates the overlay over `target` too, with its swapchain on the presenter's queue: generated frames
// are written straight into the backbuffer (no staging texture, no 20 MB copy per present).
// keys: hotkeys registered on the overlay window (may be null).
XeFg* XeFgCreate(Gpu& g, HWND target, UINT w, UINT h, UINT flow_factor, const HotkeyDef* keys = nullptr, int nkeys = 0);
void  XeFgDestroy(XeFg* f);   // stops the presenter, drains every queue, destroys the overlay
Overlay* XeFgOverlay(XeFg* f);   // for OverlayFollow / OverlayHotkey / OverlayVBlankMs
// The copy of the frame the last XeFgSubmit took (BGRA8, NPSR at rest). Main thread; valid for work
// recorded on Gpu::queue right after that submit (the ring reuses it four frames later).
ID3D12Resource* XeFgLastFrame(XeFg* f);
// src: BGRA8 w x h in COMMON; Gpu::queue waits wait_fence/wait_value first. cap_ms: the game's own
// present time on the NowMs clock. reset: do not pair with the previous frame (scene cut, gap).
bool  XeFgSubmit(XeFg* f, ID3D12Resource* src, ID3D12Fence* wait_fence, UINT64 wait_value, double cap_ms, bool reset);
void  XeFgSetEnabled(XeFg* f, bool on);
void  XeFgSetTiming(XeFg* f, double margin_ms, double max_in_fps);
// Extrapolation: the content clock runs just behind the newest frame's arrival instead of one frame
// interval behind it, and past the newest frame shows it pushed ahead along its own flow (up to one
// interval). Removes the interpolation's hold (~a frame interval of latency); disocclusions stretch.
void  XeFgSetExtrapolate(XeFg* f, bool on);
// Sync mode. Vsync: the frame is rendered `lead` before a vblank and flips on it - no tearing; measured
// 1.3 ms present -> screen at 120 Hz (rendering right after the vblank instead made it 8.1 ms). Off:
// each frame flips as soon as it is rendered - ~1.0-1.5 ms, but the tear sits ~13% down the screen
// (measured 9-17%), where a game's top HUD and sky are.
void  XeFgSetVsync(XeFg* f, bool on);
void  XeFgSetVsyncLead(XeFg* f, double ms);   // vsync: the least lead before the vblank (default 1.5 ms; adapts up on misses)
bool  XeFgFailed(const XeFg* f);

struct XeFgStatsOut
{
    UINT in = 0;             // real frames submitted
    UINT presented = 0;      // frames put on screen (real + generated)
    UINT generated = 0;      // presented with 0 < t < 1 (interpolated)
    UINT extrapolated = 0;   // presented pushed ahead of the newest frame (extrapolation mode)
    UINT held = 0;           // vblanks that had to hold the newest frame (the game was late)
    UINT early = 0;          // vblanks whose content time was older than every pair (hold too long)
    UINT missed = 0;         // vsync: frames that missed their vblank (shown a refresh late)
    // Smoothness: presented frames whose content advanced < 0.5 refresh (stall) or > 1.5 (jump), by cause:
    UINT stall_hold = 0;     //   the hold rose (content clock waits for real time to catch up)
    UINT stall_late = 0;     //   the newest pair ran out (the game's frame was late)
    UINT stall_other = 0;    //   anything else
    UINT jumps = 0;
    UINT repeats = 0;        // refreshes the presenter skipped: nothing new to show (the last frame stays)
    UINT wait_timeouts = 0;  // vsync: swapchain waits that timed out
    double lead_ms = 0;      // vsync: the current adaptive lead before the vblank
    double flow_ms = -1;     // median GPU ms of pyramid + both flows per real frame
    double interp_ms = -1;   // median GPU ms per presented frame
    double hold_ms = 0, interval_ms = 0;   // the presenter's current hold and frame-interval EMA
    double extra_ms = 0;                   // the learned part of the hold (late pairs under load), decaying
    bool passthrough = false;              // generation off (disabled, or the game is fast enough)
    std::vector<double> spacing_ms;        // present-to-present
    std::vector<double> after_vblank_ms;   // refresh wake -> Present call (where in the scan an immediate flip lands)
};
void XeFgStats(XeFg* f, XeFgStatsOut& out);   // everything since the last call

// Debug: copy the next GENERATED frame the presenter shows (0 < t < 1) to a readback buffer.
// XeFgTakeSnapshot returns true once it has landed (RGBA8, w x h) with the t it was made at.
void XeFgRequestSnapshot(XeFg* f);
bool XeFgTakeSnapshot(XeFg* f, std::vector<uint8_t>& rgba, float& t);
