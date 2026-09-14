// Contrast-adaptive sharpening, written from the published FidelityFX CAS algorithm (not its source):
// per pixel the 3x3 min/max says how much contrast headroom is left; the negative-lobe weight of a
// cross kernel scales with it, so flat detail sharpens fully while edges that already span the range
// get little (no ringing, no clipping). strength 0..1 -> lobe -1/8 .. -1/5. Pixels inside a UI rect
// (the compose's rect texture, nrects entries) pass through.
Texture2D<float4>   src   : register(t0);
Texture2D<int>      rects : register(t1);
RWTexture2D<float4> dst   : register(u0);
cbuffer C : register(b0) { uint w, h; float strength; uint nrects; };

float3 Px(int2 p) { return src[clamp(p, int2(0, 0), int2(w - 1, h - 1))].rgb; }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const int2 p = int2(id.xy);
    const float3 e = src[p].rgb;
    [loop] for (uint r = 0; r < min(nrects, 64u); ++r)
        if (p.x >= rects[uint2(r * 4 + 0, 0)] && p.y >= rects[uint2(r * 4 + 1, 0)] &&
            p.x <= rects[uint2(r * 4 + 2, 0)] && p.y <= rects[uint2(r * 4 + 3, 0)])
        { dst[id.xy] = float4(e, 1); return; }
    const float3 a = Px(p + int2(-1, -1)), b = Px(p + int2(0, -1)), c = Px(p + int2(1, -1));
    const float3 d = Px(p + int2(-1,  0)),                            f = Px(p + int2(1,  0));
    const float3 g = Px(p + int2(-1,  1)), hh = Px(p + int2(0,  1)), i = Px(p + int2(1,  1));
    // cross min/max widened by the diagonals: sums of two, so the headroom test is against 2
    float3 mn = min(min(min(d, e), min(f, b)), hh), mx = max(max(max(d, e), max(f, b)), hh);
    mn += min(min(a, c), min(g, i)); mx += max(max(a, c), max(g, i));
    const float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
    const float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, saturate(strength)));
    dst[id.xy] = float4(saturate(((b + d + f + hh) * wgt + e) / (1.0 + 4.0 * wgt)), 1);
}
