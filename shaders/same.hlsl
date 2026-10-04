// Duplicate-frame test: 1 per 64x64 tile where this capture differs from the previous one at all. Desktop
// apps (a browser playing video) present at the display rate while the content changes at 24-30 fps, and
// every repaint is a byte-identical copy - counted as a frame, it made "fps in" swing 30..240 and the FG
// governor toggle. a: this capture, converted as the swizzle will (capture_in.hlsli); b: color4k, still
// the previous frame (the swizzle runs after).
#include "capture_in.hlsli"
Texture2D<float4>  a   : register(t0);
Texture2D<float4>  b   : register(t1);
RWTexture2D<float> dst : register(u0);
cbuffer C : register(b0) { uint w, h; float white; };

groupshared uint diff;

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) diff = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint2 o = gid.xy * 64 + tid.xy * 8;
    uint d = 0;
    [loop] for (uint y = 0; y < 8; ++y)
        [loop] for (uint x = 0; x < 8; ++x)
        {
            const uint2 q = o + uint2(x, y);
            if (q.x < w && q.y < h && any(round(saturate(CaptureIn(a, q, white)) * 255) != round(b[q] * 255))) d = 1;   // as color4k will store it
        }
    if (d) InterlockedOr(diff, 1);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) dst[gid.xy] = diff;
}
