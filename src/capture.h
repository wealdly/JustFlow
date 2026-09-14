// Window capture: Windows.Graphics.Capture on a private D3D11 device (same adapter as the D3D12
// device), frames copied into a shared NT-handle BGRA8 texture and handed to D3D12 through a shared
// fence. No CPU wait on the frame: the caller does queue->Wait(CaptureFence, value).
#pragma once
#include "d3d.h"

struct Capture;

// Opens WGC on `target`. Fails if the window is not capturable. Logs sizes and formats.
// prefer_dda: DXGI Desktop Duplication on the window's monitor first (monitor refresh rate; the overlay
// must be excluded from capture), falling back to Windows.Graphics.Capture (60 Hz ceiling).
Capture* CaptureOpen(Gpu& g, HWND target, bool show_cursor, bool show_border, bool prefer_dda = false);   // default off until main wires the overlay exclusion
bool     CaptureIsDda(Capture* c);
void     CaptureClose(Capture* c);

// Drains the pool to the newest frame (max 8). If one arrived: copies it into the shared texture,
// signals the shared fence, and returns true with `fence_value` to wait on the D3D12 queue and
// `sys_rel_100ns` = frame.SystemRelativeTime (QPC-based, 100 ns units). Blocks up to wait_ms for
// the first frame. Returns false when nothing new arrived (static skip) or capture is lost.
bool CaptureAcquire(Capture* c, DWORD wait_ms, UINT64& fence_value, LONGLONG& sys_rel_100ns);

// The D3D12 view of the shared texture (DXGI_FORMAT_B8G8R8A8_UNORM). Resting state is COMMON;
// transition to NON_PIXEL_SHADER_RESOURCE to read and back to COMMON after.
ID3D12Resource* CaptureTexture(Capture* c);
ID3D12Fence*    CaptureFence(Capture* c);
UINT            CaptureWidth(Capture* c);
double          CaptureAccumMean(Capture* c);   // DDA: mean AccumulatedFrames per acquire since the last call (0 = n/a)
UINT            CaptureHeight(Capture* c);
// True once the captured item size differed from the open size for >= 250 ms (deadband). The
// caller then closes and reopens. new_w/new_h carry the settled size.
bool CaptureSizeChanged(Capture* c, UINT& new_w, UINT& new_h);
// True if the target window is gone or the capture session was closed by the system.
bool CaptureLost(Capture* c);
// True if frames arrive as FP16 (HDR). Phase 1 refuses these with a log line.
bool CaptureIsFloat(Capture* c);
