// Frame Warp's no-warp mask (engine=latewarp): 1 where content moved under half the frame's median
// motion (g, the camera's) - pinned to the camera. A third-person character stays put on screen while the
// camera turns around it: warping it by the camera's rotation is what smudged its edges. Faster than the
// median (the far background of an orbit) is still the camera's and warps.
// One mask texel per 4x4 work pixels, looking one texel further out on every side so the edges are held
// too. mv: backward flow, work px; g: its median, same units (read back a few frames late - fine for a
// camera, whose motion is smooth).
Texture2D<float2>  mv  : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer C : register(b0) { uint ww, wh, mw, mh; float gx, gy; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= mw || id.y >= mh) return;
    const float2 g = float2(gx, gy);
    const float gg = dot(g, g);
    const int2 c0 = int2(id.xy) * 4 - 4, c1 = int2(id.xy) * 4 + 8;
    float hold = 0;
    [loop] for (int y = c0.y; y < c1.y; y += 2)
        [loop] for (int x = c0.x; x < c1.x; x += 2)
        {
            const int2 q = clamp(int2(x, y), int2(0, 0), int2(ww - 1, wh - 1));
            if (gg > 1 && dot(mv[q], g) < 0.5 * gg) hold = 1;   // moved under half the camera's way: pinned to it (the character)
        }
    dst[id.xy] = hold;
}
