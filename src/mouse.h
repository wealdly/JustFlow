// Raw mouse input for the latewarp engine, and the model that turns it into "how far the picture
// will have moved". Read-only and mouse-only: JustFlow registers its own hidden window for raw mouse
// input (RIDEV_INPUTSINK) - no hooks, nothing injected, no keyboard, nothing sent - and only while
// the latewarp engine runs.
//
// The model is learned, not configured. Each captured frame, the pipeline reports how far its content
// moved (our optical flow, median over the frame) between two capture timestamps; the model fits
// pixels-per-count and the input delay (the game's own input-to-present latency) that best explain it.
// Movement with a mouse button held and without are fitted separately: in WoW the camera turns only
// while a button is held, so cursor movement fits nothing and is never used to warp.
#pragma once
#include <windows.h>

void MouseStart();   // idempotent; starts the input thread
void MouseStop();    // idempotent

// Content moved (mx, my) output pixels (forward: where it went, not where it came from) between the
// capture timestamps t0 and t1 (QPC).
void MouseObserve(LONGLONG t0, LONGLONG t1, float mx, float my);
// The content shift the input since the frame at `t_frame` will have caused by `t_now`: false while
// the model has not learned a reliable fit (then: do not warp).
bool MousePredict(LONGLONG t_frame, LONGLONG t_now, float& sx, float& sy);
// Synthetic input with a known gain and delay through the real fit; leaves the model empty. ([log] selftest=1)
bool MouseSelfTest();
