// NVIDIA Frame Warp - Reflex 2's "latewarp", NGX feature 15, nvngx_latewarp.dll next to the exe. It
// re-projects a finished frame to a different camera and fills what the new view uncovers.
// JustFlow has no camera or depth from the game, so it feeds what a capture can honestly say: a flat
// depth, zero motion vectors, an identity source camera and a target camera rotated by (yaw, pitch,
// roll). The UI layer goes in empty; callers copy their UI rects back over the result.
// Measured (bench --lwtest, tools/scene): an unchanged camera returns the frame byte-exact; RGBA8 and
// BGRA8 in, RGBA8 out, no channel games. The flat depth must be FAR: Frame Warp leaves pixels close to
// the camera unwarped (a first-person weapon). 2 deg of yaw moves the centre 66 px at 60 deg vfov (edges
// more), as a real turn does. It is a ROTATION: right for a
// camera turning in place (mouse-look), wrong for an orbit camera, whose pivot does not move - warping a
// third-person frame by its true rotation scored 8.35 against 5.87 for not warping at all. Orbit
// cameras need the no-warp mask on what stays put.
// Interface facts (parameter names, the register-then-warp evaluate pair) are from the runtime's own
// strings and NVIDIA's Streamline_Sample NGX path; none of its code is used.
#pragma once
#include "d3d.h"

struct Latewarp;

// w x h: colour and output size. dw x dh: depth / motion size (any; flat and zero anyway).
Latewarp* LatewarpCreate(Gpu& g, const wchar_t* dir, UINT w, UINT h, UINT dw, UINT dh);
void      LatewarpDestroy(Latewarp* l);
// color: RGBA8 in NPSR. out: RGBA8, UAV-capable, in UAV. The UI layer is always empty: callers copy
// their UI rects back afterwards. rendered = the first warp of a new frame (registers it first). Radians; vfov is
// the vertical field of view the angles are measured against.
bool LatewarpEvaluate(Latewarp* l, ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* out,
                      bool rendered, float yaw, float pitch, float roll, float vfov);
