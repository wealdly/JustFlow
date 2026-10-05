// The tail every compose shares: UI rects blend back to native inside (feathered outside), then the wipe.
// No vibrance here: the filter layer (sharpen.hlsl) grades after the sharpen, model or no model.
// The includer declares rects, feather, nrects, wipe_mode, wipe_x and w (compose.hlsl's layout).
float3 UiRectsAndWipe(float3 res, float3 nat, uint2 q)
{
    const float2 p = float2(q);
    const float f = max(feather, 1);
    [loop] for (uint i = 0; i < min(nrects, 64u); ++i)
    {
        const float x0 = rects[uint2(i * 4 + 0, 0)], y0 = rects[uint2(i * 4 + 1, 0)];
        const float x1 = rects[uint2(i * 4 + 2, 0)], y1 = rects[uint2(i * 4 + 3, 0)];
        const float d = max(max(max(x0 - p.x, p.x - x1), max(y0 - p.y, p.y - y1)), 0);
        res = lerp(res, nat, 1 - smoothstep(0, f, d));
    }
    if (wipe_mode == 1)
    {
        const float split = wipe_x * w;
        if (abs(p.x - split) < 1.5) res = 1;
        else if (p.x < split)      res = nat;
    }
    return res;
}
