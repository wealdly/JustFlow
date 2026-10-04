// Duplicate-frame and scene-cut test, one texel per 64x64 tile: R = 1 where this capture differs from the
// previous one at all, G = the tile's mean luma. Desktop apps (a browser playing video) present at the
// display rate while the content changes at 24-30 fps, and every repaint is a byte-identical copy - counted
// as a frame, it made "fps in" swing 30..240 and the FG governor toggle. The tile means feed the cut test
// (main: each tile against its best 3x3 neighbour in the previous frame, mean > 0.03 = a cut).
// a: this capture, converted as the swizzle will (capture_in.hlsli); b: color4k, still the previous frame
// (the swizzle runs after).
#include "capture_in.hlsli"
Texture2D<float4>   a   : register(t0);
Texture2D<float4>   b   : register(t1);
RWTexture2D<float2> dst : register(u0);
cbuffer C : register(b0) { uint w, h; float white; };

groupshared uint diff, lsum, cnt;

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { diff = 0; lsum = 0; cnt = 0; }
    GroupMemoryBarrierWithGroupSync();
    const uint2 o = gid.xy * 64 + tid.xy * 8;
    uint d = 0, l = 0, n = 0;
    [loop] for (uint y = 0; y < 8; ++y)
        [loop] for (uint x = 0; x < 8; ++x)
        {
            const uint2 q = o + uint2(x, y);
            if (q.x >= w || q.y >= h) continue;
            const float4 c = round(saturate(CaptureIn(a, q, white)) * 255);   // as color4k will store it
            if (any(c != round(b[q] * 255))) d = 1;
            l += (uint)dot(c.rgb, float3(0.2126, 0.7152, 0.0722)); ++n;
        }
    if (d) InterlockedOr(diff, 1);
    InterlockedAdd(lsum, l); InterlockedAdd(cnt, n);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) dst[gid.xy] = float2(diff, cnt ? lsum / (cnt * 255.0) : 0);
}
